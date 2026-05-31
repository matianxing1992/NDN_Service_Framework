#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <algorithm>
#include <stdexcept>
#include <sstream>

namespace ndnsf_distributed_repo {

namespace {

std::string
extractJsonString(const std::string& json, const std::string& key)
{
  const std::string marker = "\"" + key + "\":\"";
  const auto start = json.find(marker);
  if (start == std::string::npos) {
    return "";
  }
  const auto valueStart = start + marker.size();
  std::string value;
  bool escaping = false;
  for (size_t i = valueStart; i < json.size(); ++i) {
    const char ch = json[i];
    if (escaping) {
      switch (ch) {
      case 'n':
        value.push_back('\n');
        break;
      case 'r':
        value.push_back('\r');
        break;
      case 't':
        value.push_back('\t');
        break;
      default:
        value.push_back(ch);
        break;
      }
      escaping = false;
      continue;
    }
    if (ch == '\\') {
      escaping = true;
      continue;
    }
    if (ch == '"') {
      break;
    }
    value.push_back(ch);
  }
  return value;
}

uint64_t
extractJsonUInt(const std::string& json, const std::string& key, uint64_t fallback)
{
  const std::string marker = "\"" + key + "\":";
  const auto start = json.find(marker);
  if (start == std::string::npos) {
    return fallback;
  }
  const auto valueStart = start + marker.size();
  size_t valueEnd = valueStart;
  while (valueEnd < json.size() && json[valueEnd] >= '0' && json[valueEnd] <= '9') {
    ++valueEnd;
  }
  if (valueEnd == valueStart) {
    return fallback;
  }
  return std::stoull(json.substr(valueStart, valueEnd - valueStart));
}

std::vector<std::string>
extractJsonStringArray(const std::string& json, const std::string& key)
{
  std::vector<std::string> values;
  const std::string marker = "\"" + key + "\":[";
  const auto start = json.find(marker);
  if (start == std::string::npos) {
    return values;
  }
  const auto arrayStart = start + marker.size();
  const auto arrayEnd = json.find(']', arrayStart);
  if (arrayEnd == std::string::npos) {
    return values;
  }
  std::string array = json.substr(arrayStart, arrayEnd - arrayStart);
  size_t pos = 0;
  while (pos < array.size()) {
    const auto quote = array.find('"', pos);
    if (quote == std::string::npos) {
      break;
    }
    const auto endQuote = array.find('"', quote + 1);
    if (endQuote == std::string::npos) {
      break;
    }
    values.push_back(array.substr(quote + 1, endQuote - quote - 1));
    pos = endQuote + 1;
  }
  return values;
}

} // namespace

ndn::Name
makeRepoServiceName(const ndn::Name& prefix, const std::string& operation)
{
  ndn::Name service(prefix);
  service.append(operation);
  return service;
}

std::vector<uint8_t>
toBytes(const std::string& text)
{
  return std::vector<uint8_t>(text.begin(), text.end());
}

std::string
toString(const std::vector<uint8_t>& bytes)
{
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::vector<uint8_t>
encodeStoreRequest(const RepoObjectManifest& manifest,
                   const std::vector<uint8_t>& payload)
{
  const auto manifestJson = manifest.toJson();
  const auto header = std::to_string(manifestJson.size()) + "\n";
  std::vector<uint8_t> encoded;
  encoded.reserve(header.size() + manifestJson.size() + payload.size());
  encoded.insert(encoded.end(), header.begin(), header.end());
  encoded.insert(encoded.end(), manifestJson.begin(), manifestJson.end());
  encoded.insert(encoded.end(), payload.begin(), payload.end());
  return encoded;
}

std::vector<uint8_t>
encodeManifestRequest(const RepoObjectManifest& manifest)
{
  return toBytes(manifest.toJson());
}

void
decodeStoreRequest(const std::vector<uint8_t>& request,
                   RepoObjectManifest& manifest,
                   std::vector<uint8_t>& payload)
{
  const auto newline = std::find(request.begin(), request.end(), '\n');
  if (newline == request.end()) {
    throw std::invalid_argument("repo store request missing manifest length");
  }
  const std::string lengthText(request.begin(), newline);
  const auto manifestSize = static_cast<size_t>(std::stoull(lengthText));
  const auto manifestStart = static_cast<size_t>(std::distance(request.begin(), newline)) + 1;
  if (request.size() < manifestStart + manifestSize) {
    throw std::invalid_argument("repo store request truncated manifest");
  }
  const std::string manifestJson(
    reinterpret_cast<const char*>(request.data() + manifestStart),
    manifestSize);
  manifest = parseManifestJson(manifestJson);
  payload.assign(request.begin() + manifestStart + manifestSize, request.end());
}

RepoObjectManifest
parseManifestJson(const std::string& manifestJson)
{
  RepoObjectManifest manifest;
  manifest.objectName = extractJsonString(manifestJson, "objectName");
  manifest.objectType = extractJsonString(manifestJson, "objectType");
  manifest.sha256 = extractJsonString(manifestJson, "sha256");
  manifest.size = extractJsonUInt(manifestJson, "size", 0);
  manifest.segmentCount = static_cast<uint32_t>(
    extractJsonUInt(manifestJson, "segmentCount", 1));
  manifest.replicationFactor = static_cast<uint32_t>(
    extractJsonUInt(manifestJson, "replicationFactor", 1));
  manifest.policyEpoch = extractJsonString(manifestJson, "policyEpoch");
  manifest.replicaNodes = extractJsonStringArray(manifestJson, "replicaNodes");
  return manifest;
}

std::vector<RepoObjectManifest>
parseInventoryJson(const std::string& inventoryJson)
{
  std::vector<RepoObjectManifest> manifests;
  size_t depth = 0;
  size_t objectStart = std::string::npos;
  bool inString = false;
  bool escaping = false;

  for (size_t i = 0; i < inventoryJson.size(); ++i) {
    const char ch = inventoryJson[i];
    if (inString) {
      if (escaping) {
        escaping = false;
      }
      else if (ch == '\\') {
        escaping = true;
      }
      else if (ch == '"') {
        inString = false;
      }
      continue;
    }

    if (ch == '"') {
      inString = true;
      continue;
    }
    if (ch == '{') {
      if (depth == 0) {
        objectStart = i;
      }
      ++depth;
      continue;
    }
    if (ch == '}') {
      if (depth == 0) {
        throw std::invalid_argument("repo inventory JSON has unmatched object close");
      }
      --depth;
      if (depth == 0 && objectStart != std::string::npos) {
        manifests.push_back(
          parseManifestJson(inventoryJson.substr(objectStart, i - objectStart + 1)));
        objectStart = std::string::npos;
      }
    }
  }

  if (depth != 0 || inString) {
    throw std::invalid_argument("repo inventory JSON is truncated");
  }
  return manifests;
}

std::string
encodeInventory(const std::vector<RepoObjectManifest>& manifests)
{
  std::ostringstream os;
  os << "[";
  for (size_t i = 0; i < manifests.size(); ++i) {
    if (i != 0) {
      os << ",";
    }
    os << manifests[i].toJson();
  }
  os << "]";
  return os.str();
}

} // namespace ndnsf_distributed_repo
