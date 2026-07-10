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

bool
extractJsonBool(const std::string& json, const std::string& key, bool fallback)
{
  const std::string marker = "\"" + key + "\":";
  const auto start = json.find(marker);
  if (start == std::string::npos) {
    return fallback;
  }
  const auto valueStart = start + marker.size();
  if (json.compare(valueStart, 4, "true") == 0) {
    return true;
  }
  if (json.compare(valueStart, 5, "false") == 0) {
    return false;
  }
  return fallback;
}

std::string
extractJsonObject(const std::string& json, const std::string& key)
{
  const std::string marker = "\"" + key + "\":";
  const auto markerStart = json.find(marker);
  if (markerStart == std::string::npos) {
    return "";
  }
  const auto valueStart = json.find('{', markerStart + marker.size());
  if (valueStart == std::string::npos) {
    return "";
  }

  size_t depth = 0;
  bool inString = false;
  bool escaping = false;
  for (size_t i = valueStart; i < json.size(); ++i) {
    const char ch = json[i];
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
      ++depth;
      continue;
    }
    if (ch == '}') {
      if (depth == 0) {
        return "";
      }
      --depth;
      if (depth == 0) {
        return json.substr(valueStart, i - valueStart + 1);
      }
    }
  }
  return "";
}

std::vector<std::string>
extractJsonObjectArray(const std::string& json, const std::string& key)
{
  std::vector<std::string> objects;
  const std::string marker = "\"" + key + "\":[";
  const auto start = json.find(marker);
  if (start == std::string::npos) {
    return objects;
  }
  const auto arrayStart = start + marker.size();
  size_t depth = 0;
  size_t objectStart = std::string::npos;
  bool inString = false;
  bool escaping = false;

  for (size_t i = arrayStart; i < json.size(); ++i) {
    const char ch = json[i];
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
        throw std::invalid_argument("repo catalog JSON has unmatched object close");
      }
      --depth;
      if (depth == 0 && objectStart != std::string::npos) {
        objects.push_back(json.substr(objectStart, i - objectStart + 1));
        objectStart = std::string::npos;
      }
      continue;
    }
    if (ch == ']' && depth == 0) {
      break;
    }
  }
  return objects;
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

std::vector<uint8_t>
encodeDataReferenceRequest(const RepoDataReference& reference)
{
  return toBytes(reference.toJson());
}

std::vector<uint8_t>
encodeStatusRequest(const std::string& operationId)
{
  return toBytes(operationId);
}

std::vector<uint8_t>
encodeCatalogDeltaRequest(uint64_t sinceEpoch)
{
  return toBytes(std::to_string(sinceEpoch));
}

std::vector<uint8_t>
encodeCatalogLookupRequest(const std::string& objectName)
{
  return toBytes(objectName);
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

RepoDataReference
parseDataReferenceJson(const std::string& referenceJson)
{
  RepoDataReference reference;
  reference.objectName = extractJsonString(referenceJson, "objectName");
  reference.dataPrefix = extractJsonString(referenceJson, "dataPrefix");
  reference.firstSegment = extractJsonUInt(referenceJson, "firstSegment", 0);
  reference.finalSegment = extractJsonUInt(referenceJson, "finalSegment", 0);
  reference.hasFinalSegment = extractJsonBool(referenceJson, "hasFinalSegment", false);
  reference.forwardingHint = extractJsonString(referenceJson, "forwardingHint");
  reference.expectedSha256 = extractJsonString(referenceJson, "expectedSha256");
  reference.expectedSize = extractJsonUInt(referenceJson, "expectedSize", 0);
  reference.storeWirePackets = extractJsonBool(referenceJson, "storeWirePackets", true);
  reference.objectType = extractJsonString(referenceJson, "objectType");
  if (reference.objectType.empty()) {
    reference.objectType = "ndn-segmented-data";
  }
  return reference;
}

RepoOperationStatus
parseOperationStatusJson(const std::string& statusJson)
{
  RepoOperationStatus status;
  status.operationId = extractJsonString(statusJson, "operationId");
  status.operation = extractJsonString(statusJson, "operation");
  status.state = extractJsonString(statusJson, "state");
  status.objectName = extractJsonString(statusJson, "objectName");
  status.message = extractJsonString(statusJson, "message");
  status.completedSegments = extractJsonUInt(statusJson, "completedSegments", 0);
  status.totalSegments = extractJsonUInt(statusJson, "totalSegments", 0);
  status.createdAtMs = extractJsonUInt(statusJson, "createdAtMs", 0);
  status.updatedAtMs = extractJsonUInt(statusJson, "updatedAtMs", 0);
  status.expiresAtMs = extractJsonUInt(statusJson, "expiresAtMs", 0);
  return status;
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
  manifest.generation = extractJsonUInt(manifestJson, "generation", 0);
  const auto parentText = extractJsonString(manifestJson, "parentGeneration");
  (void)parentText;
  const std::string parentMarker = "\"parentGeneration\":";
  const auto parentStart = manifestJson.find(parentMarker);
  if (parentStart != std::string::npos) {
    const auto valueStart = parentStart + parentMarker.size();
    size_t valueEnd = valueStart;
    if (valueEnd < manifestJson.size() && manifestJson[valueEnd] == '-') {
      ++valueEnd;
    }
    while (valueEnd < manifestJson.size() &&
           manifestJson[valueEnd] >= '0' && manifestJson[valueEnd] <= '9') {
      ++valueEnd;
    }
    manifest.parentGeneration = std::stoll(
      manifestJson.substr(valueStart, valueEnd - valueStart));
  }
  manifest.writeConsistency = extractJsonString(manifestJson, "writeConsistency");
  if (manifest.writeConsistency.empty()) {
    manifest.writeConsistency = "ALL";
  }
  manifest.requiredWriteAcks = static_cast<uint32_t>(extractJsonUInt(
    manifestJson, "requiredWriteAcks",
    ndnsf_distributed_repo::requiredWriteAcks(
      manifest.replicationFactor,
      parseRepoWriteConsistency(manifest.writeConsistency))));
  manifest.operationId = extractJsonString(manifestJson, "operationId");
  manifest.lifecycleState = extractJsonString(manifestJson, "lifecycleState");
  if (manifest.lifecycleState.empty()) {
    manifest.lifecycleState = "COMMITTED";
  }
  manifest.policyEpoch = extractJsonString(manifestJson, "policyEpoch");
  manifest.replicaNodes = extractJsonStringArray(manifestJson, "replicaNodes");
  manifest.confirmedReplicaNodes = extractJsonStringArray(
    manifestJson, "confirmedReplicaNodes");
  if (manifest.confirmedReplicaNodes.empty()) {
    manifest.confirmedReplicaNodes = manifest.replicaNodes;
  }
  manifest.packetNames = extractJsonStringArray(manifestJson, "packetNames");
  return manifest;
}

RepoCatalogEntry
parseCatalogEntryJson(const std::string& entryJson)
{
  RepoCatalogEntry entry;
  const auto manifestJson = extractJsonObject(entryJson, "manifest");
  if (!manifestJson.empty()) {
    entry.manifest = parseManifestJson(manifestJson);
  }
  else {
    entry.manifest.objectName = extractJsonString(entryJson, "objectName");
    entry.manifest.objectType = extractJsonString(entryJson, "objectType");
    entry.manifest.sha256 = extractJsonString(entryJson, "manifestSha256");
    entry.manifest.size = extractJsonUInt(entryJson, "size", 0);
    entry.manifest.segmentCount = static_cast<uint32_t>(
      extractJsonUInt(entryJson, "segmentCount", 1));
    entry.manifest.replicaNodes = extractJsonStringArray(entryJson, "replicaNodes");
  }
  entry.sourceRepo = extractJsonString(entryJson, "sourceRepo");
  entry.repoMode = extractJsonString(entryJson, "repoMode");
  if (entry.repoMode.empty()) {
    entry.repoMode = "persistent";
  }
  entry.state = extractJsonString(entryJson, "state");
  if (entry.state.empty()) {
    entry.state = "AVAILABLE";
  }
  entry.catalogEpoch = extractJsonUInt(entryJson, "catalogEpoch", 0);
  return entry;
}

RepoCatalogStatus
parseCatalogStatusJson(const std::string& statusJson)
{
  RepoCatalogStatus status;
  status.repoNode = extractJsonString(statusJson, "repoNode");
  status.repoMode = extractJsonString(statusJson, "repoMode");
  if (status.repoMode.empty()) {
    status.repoMode = "persistent";
  }
  status.catalogEpoch = extractJsonUInt(statusJson, "catalogEpoch", 0);
  status.objectCount = extractJsonUInt(statusJson, "objectCount", 0);
  status.acceptsBackupReplica = extractJsonBool(statusJson, "acceptsBackupReplica", true);
  return status;
}

RepoCacheStatus
parseCacheStatusJson(const std::string& statusJson)
{
  RepoCacheStatus status;
  status.storageBackend = extractJsonString(statusJson, "storageBackend");
  status.authoritativeBackend = extractJsonString(statusJson, "authoritativeBackend");
  status.cachePolicy = extractJsonString(statusJson, "cachePolicy");
  status.budgetBytes = extractJsonUInt(statusJson, "budgetBytes", 0);
  status.usedBytes = extractJsonUInt(statusJson, "usedBytes", 0);
  status.entryCount = extractJsonUInt(statusJson, "entryCount", 0);
  status.hits = extractJsonUInt(statusJson, "hits", 0);
  status.misses = extractJsonUInt(statusJson, "misses", 0);
  status.admissions = extractJsonUInt(statusJson, "admissions", 0);
  status.evictions = extractJsonUInt(statusJson, "evictions", 0);
  status.invalidations = extractJsonUInt(statusJson, "invalidations", 0);
  status.oversizedBypasses = extractJsonUInt(statusJson, "oversizedBypasses", 0);
  status.backingReads = extractJsonUInt(statusJson, "backingReads", 0);
  status.backingWrites = extractJsonUInt(statusJson, "backingWrites", 0);
  return status;
}

RepoCatalogDelta
parseCatalogDeltaJson(const std::string& deltaJson)
{
  RepoCatalogDelta delta;
  delta.repoNode = extractJsonString(deltaJson, "repoNode");
  delta.repoMode = extractJsonString(deltaJson, "repoMode");
  if (delta.repoMode.empty()) {
    delta.repoMode = "persistent";
  }
  delta.sinceEpoch = extractJsonUInt(deltaJson, "sinceEpoch", 0);
  delta.catalogEpoch = extractJsonUInt(deltaJson, "catalogEpoch", 0);
  for (const auto& entryJson : extractJsonObjectArray(deltaJson, "entries")) {
    delta.entries.push_back(parseCatalogEntryJson(entryJson));
  }
  return delta;
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
