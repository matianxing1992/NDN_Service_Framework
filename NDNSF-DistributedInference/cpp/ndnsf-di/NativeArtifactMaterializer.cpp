#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactMaterializer.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ndnsf::di {

namespace {

std::string
readString(const boost::property_tree::ptree& node, const std::string& key)
{
  return node.get<std::string>(key, "");
}

const boost::property_tree::ptree&
childOrEmpty(const boost::property_tree::ptree& node, const std::string& key)
{
  static const boost::property_tree::ptree EMPTY;
  const auto child = node.get_child_optional(key);
  return child ? child.get() : EMPTY;
}

std::string
manifestSha256(const boost::property_tree::ptree& entry)
{
  const auto manifest = entry.get_child_optional("repoManifest");
  if (manifest) {
    return manifest->get<std::string>("sha256", "");
  }
  const auto legacyManifest = entry.get_child_optional("repo_manifest");
  if (legacyManifest) {
    return legacyManifest->get<std::string>("sha256", "");
  }
  return entry.get<std::string>("sha256", "");
}

std::uintmax_t
manifestSize(const boost::property_tree::ptree& entry)
{
  const auto manifest = entry.get_child_optional("repoManifest");
  if (manifest) {
    return manifest->get<std::uintmax_t>("size", 0);
  }
  const auto legacyManifest = entry.get_child_optional("repo_manifest");
  if (legacyManifest) {
    return legacyManifest->get<std::uintmax_t>("size", 0);
  }
  return entry.get<std::uintmax_t>("size", 0);
}

std::string
manifestObjectName(const boost::property_tree::ptree& entry)
{
  const auto manifest = entry.get_child_optional("repoManifest");
  if (manifest) {
    return manifest->get<std::string>("objectName", "");
  }
  const auto legacyManifest = entry.get_child_optional("repo_manifest");
  if (legacyManifest) {
    return legacyManifest->get<std::string>("objectName", "");
  }
  return entry.get<std::string>("objectName", "");
}

std::string
manifestJson(const boost::property_tree::ptree& entry)
{
  const auto manifest = entry.get_child_optional("repoManifest");
  if (manifest) {
    std::ostringstream output;
    boost::property_tree::write_json(output, manifest.get(), false);
    return output.str();
  }
  const auto legacyManifest = entry.get_child_optional("repo_manifest");
  if (legacyManifest) {
    std::ostringstream output;
    boost::property_tree::write_json(output, legacyManifest.get(), false);
    return output.str();
  }
  return "";
}

std::string
entryLocalPayloadPath(const boost::property_tree::ptree& entry)
{
  auto path = readString(entry, "localPayloadPath");
  if (!path.empty()) {
    return path;
  }
  path = readString(entry, "payloadPath");
  if (!path.empty()) {
    return path;
  }
  const auto& metadata = childOrEmpty(entry, "metadata");
  return readString(metadata, "localPayloadPath");
}

std::string
entryFilename(const boost::property_tree::ptree& entry,
              const std::string& slot,
              const std::string& objectName)
{
  auto filename = readString(entry, "filename");
  if (!filename.empty()) {
    return filename;
  }
  const auto& metadata = childOrEmpty(entry, "metadata");
  filename = readString(metadata, "filename");
  if (!filename.empty()) {
    return filename;
  }
  if (!objectName.empty()) {
    return std::filesystem::path(objectName).filename().string();
  }
  return slot + ".bin";
}

std::string
toLowerAscii(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string
safePathToken(std::string value)
{
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), [] (unsigned char ch) {
                return !std::isspace(ch);
              }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [] (unsigned char ch) {
                return !std::isspace(ch);
              }).base(),
              value.end());
  while (!value.empty() && value.front() == '/') {
    value.erase(value.begin());
  }
  for (auto& ch : value) {
    if (ch == '/' || ch == '\\' || ch == ':' || std::isspace(static_cast<unsigned char>(ch))) {
      ch = '-';
    }
  }
  return value.empty() ? "artifact" : value;
}

std::vector<std::uint8_t>
readFile(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input.good()) {
    throw std::runtime_error("cannot open artifact payload: " + path.string());
  }
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                  std::istreambuf_iterator<char>());
}

std::string
sha256Hex(const std::vector<std::uint8_t>& payload)
{
  ndn::util::Sha256 digest;
  digest.update(ndn::span<const uint8_t>(payload.data(), payload.size()));
  return digest.toString();
}

void
writeFileIfNeeded(const std::filesystem::path& path,
                  const std::vector<std::uint8_t>& payload)
{
  std::filesystem::create_directories(path.parent_path());
  if (std::filesystem::exists(path)) {
    const auto existing = readFile(path);
    if (existing == payload) {
      return;
    }
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.good()) {
    throw std::runtime_error("cannot write materialized artifact: " + path.string());
  }
  output.write(reinterpret_cast<const char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
}

std::filesystem::path
materializeEntry(const boost::property_tree::ptree& entry,
                 const std::string& role,
                 const std::string& slot,
                 const NativeArtifactMaterializerOptions& options)
{
  const auto localPath = entryLocalPayloadPath(entry);
  const auto objectName = manifestObjectName(entry);
  std::vector<std::uint8_t> payload;
  if (!localPath.empty()) {
    payload = readFile(localPath);
  }
  else if (options.repoFetchFromManifest && !objectName.empty()) {
    payload = options.repoFetchFromManifest(objectName, manifestJson(entry));
  }
  else if (options.repoFetch && !objectName.empty()) {
    payload = options.repoFetch(objectName);
  }
  else {
    throw std::runtime_error(
      "native artifact materializer cannot fetch repo-only artifact yet: " +
      (objectName.empty() ? slot : objectName));
  }
  const auto expectedSize = manifestSize(entry);
  if (expectedSize > 0 && payload.size() != expectedSize) {
    std::ostringstream error;
    error << "artifact size mismatch for " << (objectName.empty() ? slot : objectName)
          << ": expected " << expectedSize << ", got " << payload.size();
    throw std::runtime_error(error.str());
  }
  const auto expectedHash = manifestSha256(entry);
  if (!expectedHash.empty()) {
    const auto actualHash = sha256Hex(payload);
    if (toLowerAscii(actualHash) != toLowerAscii(expectedHash)) {
      throw std::runtime_error(
        "artifact sha256 mismatch for " + (objectName.empty() ? slot : objectName) +
        ": expected " + expectedHash + ", got " + actualHash);
    }
  }
  const auto digest = toLowerAscii(expectedHash.empty() ? sha256Hex(payload) : expectedHash);
  const auto filename = entryFilename(entry, slot, objectName);
  auto target = std::filesystem::path(options.cacheDir.empty() ?
                                     "/tmp/ndnsf-di-native-artifacts" :
                                     options.cacheDir);
  target /= safePathToken(role);
  target /= safePathToken(slot);
  target /= digest.substr(0, std::min<std::size_t>(16, digest.size()));
  target /= filename;
  writeFileIfNeeded(target, payload);
  if (entry.get<bool>("executable", false)) {
    std::filesystem::permissions(
      target,
      std::filesystem::perms::owner_exec |
        std::filesystem::perms::group_exec |
        std::filesystem::perms::others_exec,
      std::filesystem::perm_options::add);
  }
  return target;
}

const boost::property_tree::ptree*
findRoleEntry(const boost::property_tree::ptree& root, const std::string& role)
{
  const auto roles = root.get_child_optional("roles");
  if (!roles) {
    return nullptr;
  }
  const auto direct = roles->get_child_optional(role);
  if (direct) {
    return &direct.get();
  }
  for (const auto& item : roles.get()) {
    if (item.first == role) {
      return &item.second;
    }
  }
  return nullptr;
}

} // namespace

std::map<std::string, NativeModelRunnerSpec>
materializeNativeModelArtifactsFromReferencesJson(
  const std::map<std::string, NativeModelRunnerSpec>& specsByRole,
  std::istream& artifactReferences,
  const NativeArtifactMaterializerOptions& options)
{
  boost::property_tree::ptree root;
  boost::property_tree::read_json(artifactReferences, root);
  std::map<std::string, NativeModelRunnerSpec> materialized = specsByRole;
  for (auto& item : materialized) {
    const auto& role = item.first;
    auto& spec = item.second;
    const auto* roleEntry = findRoleEntry(root, role);
    if (roleEntry == nullptr) {
      continue;
    }
    auto modelEntry = roleEntry->get_child_optional("model");
    if (!modelEntry) {
      continue;
    }
    const auto path = materializeEntry(modelEntry.get(), role, "model", options);
    spec.path = path.string();
    spec.metadata["materializedFrom"] = "artifact-references";
    spec.metadata["materializedPath"] = spec.path;
  }
  return materialized;
}

} // namespace ndnsf::di
