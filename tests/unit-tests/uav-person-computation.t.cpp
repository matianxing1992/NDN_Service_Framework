#include "tests/boost-test.hpp"
#include "tests/fixtures/uav-person-detector-profile-golden.hpp"
#include "tests/fixtures/uav-person-profile-golden.hpp"

#include "NDNSF-UAV-APP/shared/uav-person-computation.hpp"
#include "ndn-service-framework/strict-json.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::test {
namespace {

using namespace examples::uav;
namespace json = ndn_service_framework::json;

std::string
testDigest(char digit)
{
  return "sha256:" + std::string(64, digit);
}

std::map<std::string, std::string>
backendInventory()
{
  std::map<std::string, std::string> result;
  for (const auto key : {"tracking_worker.py", "person_reid.py", "person_profile.py",
                        "observation_schema.py", "motion_schema.py", "motion_estimator.py",
                        "byte_track_state.py", "person_runtime.py", "requirements-person.lock",
                        "bytetrack.yaml", "runtime/fingerprint", "runtime/torchreid-source"}) {
    result.emplace(key, testDigest('a'));
  }
  return result;
}

std::map<std::string, std::string>
nativeInventory()
{
  return {{"executable:/opt/ndnsf/bin/UavTrackingNode", testDigest('b')},
          {"library:/opt/ndnsf/lib/libndnsf.so", testDigest('c')}};
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavPersonComputation)

BOOST_AUTO_TEST_CASE(ExplicitDetectorIdentityBindsManifestAndRequestAcrossVersions)
{
  const auto legacy = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_DETECTOR_PROFILE_GOLDEN));
  const auto oldManifest = makeUavPersonComputationManifest(legacy, backendInventory(),
                                                          nativeInventory());
  const auto manifest = makeUavPersonComputationManifest(profile, backendInventory(),
                                                       nativeInventory());
  // Independent SHA-256 over the V2 literal profile and the unchanged inventory oracle.
  BOOST_CHECK_EQUAL(manifest.computationDigest,
    "sha256:3e0d72c907bfc08d968bcb69ec61c4dd3191544504f6769ee2488374eab89342");
  BOOST_CHECK(manifest.computationDigest != oldManifest.computationDigest);
  const auto parsed = parseUavPersonComputationManifest(manifest.canonicalJson, profile);
  BOOST_CHECK_EQUAL(parsed.canonicalJson, manifest.canonicalJson);
  const auto fields = makeUavPersonRequestBindingFields(profile, manifest);
  BOOST_CHECK_EQUAL(fields.at("tracking_profile_json"),
                    std::string(PERSON_DETECTOR_PROFILE_GOLDEN));
  BOOST_CHECK_EQUAL(fields.at("tracking_profile_digest"),
                    std::string(PERSON_DETECTOR_PROFILE_DIGEST));
  BOOST_CHECK_NO_THROW(requireUavPersonRequestBindingFields(fields, profile, manifest));
  BOOST_CHECK_THROW(parseUavPersonComputationManifest(manifest.canonicalJson, legacy),
                    std::invalid_argument);
  BOOST_CHECK_THROW(parseUavPersonComputationManifest(oldManifest.canonicalJson, profile),
                    std::invalid_argument);
  BOOST_CHECK_THROW(requireUavPersonRequestBindingFields(fields, legacy, oldManifest),
                    std::invalid_argument);
  BOOST_CHECK_THROW(requireUavPersonRequestBindingFields(
    makeUavPersonRequestBindingFields(legacy, oldManifest), profile, manifest),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(LiteralIdentityOracleAndCanonicalRoundtrip)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto manifest = makeUavPersonComputationManifest(profile, backendInventory(),
                                                       nativeInventory());
  // Independent offline SHA-256 vectors over the literal fixture maps and DM-1 byte formula.
  BOOST_CHECK_EQUAL(manifest.backendClosureDigest,
    "sha256:4c1a2a2402ab87b5cd7e6c5fe3dd14b630b13493936ef5178271f88c1efbefb4");
  BOOST_CHECK_EQUAL(manifest.nativeClosureDigest,
    "sha256:0add95d260c412416614eba3c73932a657dfdb92924bfb501051406519f95823");
  BOOST_CHECK_EQUAL(manifest.computationDigest,
    "sha256:5bc1f9aebf40df0ac23dacefb2fb98a1e47e18f5ace43862a662906a5a15657a");
  const auto parsed = parseUavPersonComputationManifest(" \n" + manifest.canonicalJson, profile);
  BOOST_CHECK_EQUAL(parsed.canonicalJson, manifest.canonicalJson);
  auto reordered = manifest.canonicalJson;
  const std::string schema = "\"schema\":\"uav-person-computation-manifest-v1\"";
  const auto offset = reordered.find("," + schema);
  BOOST_REQUIRE(offset != std::string::npos);
  reordered.erase(offset, schema.size() + 1);
  reordered.insert(1, schema + ",");
  const auto escapedOffset = reordered.find("tracking_worker.py");
  BOOST_REQUIRE(escapedOffset != std::string::npos);
  reordered.replace(escapedOffset, 1, "\\u0074");
  BOOST_CHECK_EQUAL(parseUavPersonComputationManifest(reordered, profile).canonicalJson,
                    manifest.canonicalJson);
}

BOOST_AUTO_TEST_CASE(RejectChangedDeclarationsAndProfile)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto manifest = makeUavPersonComputationManifest(profile, backendInventory(),
                                                       nativeInventory());
  const auto baseline = json::parseJson(manifest.canonicalJson, MAX_PERSON_COMPUTATION_BYTES);
  for (const auto& [key, value] : baseline.object) {
    auto missing = baseline;
    missing.object.erase(key);
    BOOST_CHECK_THROW(parseUavPersonComputationManifest(json::renderJson(missing), profile),
                      std::invalid_argument);
    auto wrongType = baseline;
    wrongType.object.at(key).kind = json::JsonValue::Kind::Null;
    BOOST_CHECK_THROW(parseUavPersonComputationManifest(json::renderJson(wrongType), profile),
                      std::invalid_argument);
    auto mutated = baseline;
    auto& target = mutated.object.at(key);
    if (target.kind == json::JsonValue::Kind::String) {
      target.string += "x";
    }
    else {
      target.object.begin()->second.string = testDigest('d');
    }
    BOOST_CHECK_THROW(parseUavPersonComputationManifest(json::renderJson(mutated), profile),
                      std::invalid_argument);
  }
  BOOST_CHECK_THROW(parseUavPersonComputationManifest(
    "{\"schema\":\"uav-person-computation-manifest-v1\"," + manifest.canonicalJson.substr(1),
    profile), std::invalid_argument);
  auto extra = baseline;
  extra.object.emplace("extra", json::JsonValue{});
  BOOST_CHECK_THROW(parseUavPersonComputationManifest(json::renderJson(extra), profile),
                    std::invalid_argument);
  BOOST_CHECK_THROW(parseUavPersonComputationManifest(
    std::string(MAX_PERSON_COMPUTATION_BYTES + 1, ' '), profile), std::invalid_argument);
  const auto profileJson = json::parseJson(profile.canonicalJson, MAX_PERSON_PROFILE_BYTES);
  for (const auto key : {"detectorDigest", "reidDigest", "requestedDevice"}) {
    auto modified = profileJson;
    modified.object.at(key).string = std::string(key) == "requestedDevice" ? "cpu" : testDigest('d');
    const auto other = parseUavPersonTrackingProfile(json::renderJson(modified));
    BOOST_CHECK_THROW(parseUavPersonComputationManifest(manifest.canonicalJson, other),
                      std::invalid_argument);
    BOOST_CHECK(makeUavPersonComputationManifest(other, backendInventory(), nativeInventory())
                  .computationDigest != manifest.computationDigest);
  }
}

BOOST_AUTO_TEST_CASE(RejectInventoryShapePathsAndBounds)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto backend = backendInventory();
  const auto native = nativeInventory();
  for (const auto& [key, hash] : backend) {
    auto changed = backend;
    changed.erase(key);
    BOOST_CHECK_THROW(makeUavPersonComputationManifest(profile, changed, native),
                      std::invalid_argument);
    changed.emplace("unapproved.py", hash);
    BOOST_CHECK_THROW(makeUavPersonComputationManifest(profile, changed, native),
                      std::invalid_argument);
    changed = backend;
    changed.at(key) = testDigest('A');
    BOOST_CHECK_THROW(makeUavPersonComputationManifest(profile, changed, native),
                      std::invalid_argument);
  }
  for (const auto& key : std::vector<std::string>{
         "library:relative.so", "library:/opt/../lib/x.so", "library:/opt//lib/x.so",
         "library:/opt/./lib/x.so", "library:/opt/lib/", "library:/", "other:/opt/x.so",
         "library:/opt/" + std::string(4096, 'x'), std::string("library:/opt/a\0b", 16),
         "executable:/opt/second"}) {
    auto changed = native;
    changed.emplace(key, testDigest('d'));
    BOOST_CHECK_THROW(makeUavPersonComputationManifest(profile, backend, changed),
                      std::invalid_argument);
  }
  auto noExecutable = native;
  noExecutable.erase(noExecutable.begin());
  BOOST_CHECK_THROW(makeUavPersonComputationManifest(profile, backend, noExecutable),
                    std::invalid_argument);
  BOOST_CHECK_THROW(makeUavPersonComputationManifest(profile, backend, {}), std::invalid_argument);
  auto tooMany = native;
  for (size_t i = 0; i < 255; ++i) {
    tooMany.emplace("library:/opt/lib/" + std::to_string(i), testDigest('d'));
  }
  BOOST_CHECK_THROW(makeUavPersonComputationManifest(profile, backend, tooMany),
                    std::invalid_argument);
  auto tooLarge = native;
  for (size_t i = 0; i < 64; ++i) {
    tooLarge.emplace("library:/opt/" + std::string(1024, 'x') + std::to_string(i), testDigest('d'));
  }
  BOOST_CHECK_THROW(makeUavPersonComputationManifest(profile, backend, tooLarge),
                    std::invalid_argument);
  auto wrongProfile = profile;
  wrongProfile.profileDigest = testDigest('d');
  BOOST_CHECK_THROW(makeUavPersonComputationManifest(wrongProfile, backend, native),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RequireEverySignedRequestBindingAndPreserveLegacyFields)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto manifest = makeUavPersonComputationManifest(profile, backendInventory(),
                                                       nativeInventory());
  const auto expected = makeUavPersonRequestBindingFields(profile, manifest);
  BOOST_REQUIRE_EQUAL(expected.size(), 6);
  BOOST_CHECK(!hasUavPersonRequestBindingFields({{"run_id", "run1"}}));
  auto accepted = expected;
  accepted.emplace("run_id", "run1");
  accepted.emplace("source_generation_UAV1", "gen1");
  BOOST_CHECK_NO_THROW(requireUavPersonRequestBindingFields(accepted, profile, manifest));
  BOOST_CHECK_EQUAL(expected.at("tracking_mode"), "indoor-person-reid");
  BOOST_CHECK_EQUAL(expected.at("tracking_profile_json"), std::string(PERSON_PROFILE_GOLDEN));
  BOOST_CHECK_EQUAL(expected.at("tracking_profile_digest"), std::string(PERSON_PROFILE_DIGEST));
  BOOST_CHECK_EQUAL(expected.at("tracking_backend_closure_digest"), manifest.backendClosureDigest);
  BOOST_CHECK_EQUAL(expected.at("tracking_native_closure_digest"), manifest.nativeClosureDigest);
  BOOST_CHECK_EQUAL(expected.at("tracking_computation_digest"), manifest.computationDigest);
  for (const auto& [key, value] : expected) {
    BOOST_CHECK(hasUavPersonRequestBindingFields({{key, value}}));
    auto missing = accepted;
    missing.erase(key);
    BOOST_CHECK_THROW(requireUavPersonRequestBindingFields(missing, profile, manifest),
                      std::invalid_argument);
    auto changed = accepted;
    changed.at(key) += "x";
    BOOST_CHECK_THROW(requireUavPersonRequestBindingFields(changed, profile, manifest),
                      std::invalid_argument);
  }
  auto changedManifest = manifest;
  changedManifest.computationDigest = testDigest('d');
  BOOST_CHECK_THROW(makeUavPersonRequestBindingFields(profile, changedManifest),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
