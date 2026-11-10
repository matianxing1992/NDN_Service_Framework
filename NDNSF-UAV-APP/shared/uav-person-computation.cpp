#include "uav-person-computation.hpp"

#include "UavDigest.hpp"
#include "ndn-service-framework/strict-json.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace ndnsf::examples::uav {
namespace {

namespace json = ndn_service_framework::json;
using Kind = json::JsonValue::Kind;

constexpr std::array<std::string_view, 12> BACKEND_KEYS = {
  "tracking_worker.py", "person_reid.py", "person_profile.py", "observation_schema.py",
  "motion_schema.py", "motion_estimator.py", "byte_track_state.py", "person_runtime.py",
  "requirements-person.lock", "bytetrack.yaml", "runtime/fingerprint", "runtime/torchreid-source"
};

constexpr std::array<std::string_view, 6> REQUEST_KEYS = {
  "tracking_mode", "tracking_profile_json", "tracking_profile_digest",
  "tracking_backend_closure_digest", "tracking_native_closure_digest",
  "tracking_computation_digest"
};

std::string
digest(const std::string& bytes)
{
  ndn::util::Sha256 hash;
  hash << bytes;
  return canonicalSha256Digest(hash.toString());
}

void
requireDigest(const std::string& value)
{
  if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0 ||
      canonicalSha256Digest(value.substr(7)) != value) {
    throw std::invalid_argument("person computation requires canonical SHA-256 digests");
  }
}

json::JsonValue
stringValue(const std::string& text)
{
  json::JsonValue value;
  value.kind = Kind::String;
  value.string = text;
  return value;
}

json::JsonValue
inventoryValue(const std::map<std::string, std::string>& inventory)
{
  json::JsonValue value;
  value.kind = Kind::Object;
  for (const auto& [name, hash] : inventory) {
    value.object.emplace(name, stringValue(hash));
  }
  return value;
}

void
validateInventories(const std::map<std::string, std::string>& backend,
                    const std::map<std::string, std::string>& native)
{
  if (backend.size() != BACKEND_KEYS.size() || native.empty() || native.size() > 256) {
    throw std::invalid_argument("person computation inventory count is invalid");
  }
  for (const auto& [key, value] : backend) {
    if (std::find(BACKEND_KEYS.begin(), BACKEND_KEYS.end(), key) == BACKEND_KEYS.end()) {
      throw std::invalid_argument("unknown person backend artifact");
    }
    requireDigest(value);
  }
  size_t executableCount = 0;
  size_t inventoryBytes = 0;
  for (const auto& [key, value] : native) {
    // Bound allocation before constructing the JSON representation, including escape expansion.
    if (key.size() > 4096 || !json::isValidUtf8(key) ||
        std::any_of(key.begin(), key.end(), [] (unsigned char ch) { return ch < 0x20; })) {
      throw std::invalid_argument("invalid person native artifact key");
    }
    size_t prefixSize = 0;
    if (key.compare(0, 11, "executable:") == 0) {
      prefixSize = 11;
      ++executableCount;
    }
    else if (key.compare(0, 8, "library:") == 0) {
      prefixSize = 8;
    }
    else {
      throw std::invalid_argument("unknown person native artifact kind");
    }
    const auto pathText = key.substr(prefixSize);
    const std::filesystem::path path(pathText);
    if (!path.is_absolute() || path.filename().empty() ||
        path.lexically_normal().generic_string() != pathText) {
      throw std::invalid_argument("person native artifact path must be normalized and absolute");
    }
    requireDigest(value);
    inventoryBytes += key.size() * 2 + value.size() + 6;
    if (inventoryBytes > MAX_PERSON_COMPUTATION_BYTES) {
      throw std::invalid_argument("person native inventory exceeds manifest bound");
    }
  }
  if (executableCount != 1) {
    throw std::invalid_argument("person native inventory requires exactly one executable");
  }
}

std::map<std::string, std::string>
readInventory(const json::JsonValue& value)
{
  std::map<std::string, std::string> inventory;
  for (const auto& [key, entry] : json::requireObject(value)) {
    if (entry.kind != Kind::String) {
      throw std::invalid_argument("person inventory digest must be a string");
    }
    inventory.emplace(key, entry.string);
  }
  return inventory;
}

} // namespace

UavPersonComputationManifest
makeUavPersonComputationManifest(const UavPersonTrackingProfile& profile,
                               const std::map<std::string, std::string>& backendInventory,
                               const std::map<std::string, std::string>& nativeInventory)
{
  const auto validatedProfile = parseUavPersonTrackingProfile(profile.canonicalJson);
  if (validatedProfile.profileDigest != profile.profileDigest) {
    throw std::invalid_argument("person profile bytes and digest disagree");
  }
  validateInventories(backendInventory, nativeInventory);
  UavPersonComputationManifest result;
  result.profileDigest = validatedProfile.profileDigest;
  result.backendInventory = backendInventory;
  result.nativeInventory = nativeInventory;
  const auto backend = inventoryValue(backendInventory);
  const auto native = inventoryValue(nativeInventory);
  result.backendClosureDigest = digest(json::renderJson(backend));
  result.nativeClosureDigest = digest(json::renderJson(native));
  result.computationDigest = digest("uav-person-computation-v1\n" + result.profileDigest + "\n" +
    result.backendClosureDigest + "\n" + result.nativeClosureDigest + "\n");
  json::JsonValue root;
  root.kind = Kind::Object;
  root.object = {
    {"schema", stringValue("uav-person-computation-manifest-v1")},
    {"profileDigest", stringValue(result.profileDigest)},
    {"backendInventory", backend}, {"nativeInventory", native},
    {"backendClosureDigest", stringValue(result.backendClosureDigest)},
    {"nativeClosureDigest", stringValue(result.nativeClosureDigest)},
    {"computationDigest", stringValue(result.computationDigest)}
  };
  result.canonicalJson = json::renderJson(root);
  if (result.canonicalJson.size() > MAX_PERSON_COMPUTATION_BYTES) {
    throw std::invalid_argument("person computation manifest exceeds byte bound");
  }
  return result;
}

UavPersonComputationManifest
parseUavPersonComputationManifest(const std::string& wire,
                                const UavPersonTrackingProfile& profile)
{
  const auto root = json::parseJson(wire, MAX_PERSON_COMPUTATION_BYTES);
  const auto& object = json::requireObject(root);
  json::requireExactKeys(object, {"schema", "profileDigest", "backendInventory", "nativeInventory",
    "backendClosureDigest", "nativeClosureDigest", "computationDigest"});
  if (json::member(object, "schema", Kind::String).string !=
      "uav-person-computation-manifest-v1") {
    throw std::invalid_argument("unsupported person computation schema");
  }
  const auto result = makeUavPersonComputationManifest(profile,
    readInventory(json::member(object, "backendInventory", Kind::Object)),
    readInventory(json::member(object, "nativeInventory", Kind::Object)));
  for (const auto key : {"profileDigest", "backendClosureDigest", "nativeClosureDigest",
                         "computationDigest"}) {
    requireDigest(json::member(object, key, Kind::String).string);
  }
  // Canonical equality checks every declared digest and inventory after independent recomputation.
  if (json::renderJson(root) != result.canonicalJson) {
    throw std::invalid_argument("person computation manifest digest mismatch");
  }
  return result;
}

std::map<std::string, std::string>
makeUavPersonRequestBindingFields(const UavPersonTrackingProfile& profile,
                                const UavPersonComputationManifest& manifest)
{
  const auto validated = parseUavPersonComputationManifest(manifest.canonicalJson, profile);
  if (validated.profileDigest != manifest.profileDigest ||
      validated.backendInventory != manifest.backendInventory ||
      validated.nativeInventory != manifest.nativeInventory ||
      validated.backendClosureDigest != manifest.backendClosureDigest ||
      validated.nativeClosureDigest != manifest.nativeClosureDigest ||
      validated.computationDigest != manifest.computationDigest) {
    throw std::invalid_argument("person computation object disagrees with its canonical bytes");
  }
  return {{"tracking_mode", "indoor-person-reid"},
          {"tracking_profile_json", parseUavPersonTrackingProfile(profile.canonicalJson).canonicalJson},
          {"tracking_profile_digest", validated.profileDigest},
          {"tracking_backend_closure_digest", validated.backendClosureDigest},
          {"tracking_native_closure_digest", validated.nativeClosureDigest},
          {"tracking_computation_digest", validated.computationDigest}};
}

bool
hasUavPersonRequestBindingFields(const std::map<std::string, std::string>& fields)
{
  return std::any_of(REQUEST_KEYS.begin(), REQUEST_KEYS.end(), [&] (const auto key) {
    return fields.find(std::string(key)) != fields.end();
  });
}

void
requireUavPersonRequestBindingFields(const std::map<std::string, std::string>& fields,
                                   const UavPersonTrackingProfile& profile,
                                   const UavPersonComputationManifest& manifest)
{
  for (const auto& [key, expected] : makeUavPersonRequestBindingFields(profile, manifest)) {
    const auto found = fields.find(key);
    if (found == fields.end() || found->second != expected) {
      throw std::invalid_argument("person request binding mismatch: " + key);
    }
  }
}

} // namespace ndnsf::examples::uav
