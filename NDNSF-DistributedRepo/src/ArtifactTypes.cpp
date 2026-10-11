#include "ndnsf-distributed-repo/ArtifactTypes.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndnsf-distributed-repo/detail/StrictJson.hpp"

#include <ndn-cxx/name.hpp>

#include <openssl/sha.h>

#include <array>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace ndnsf_distributed_repo {

namespace {

using detail::JsonValue;

constexpr size_t MAX_PUBLISH_OPERATION_STATUS_JSON_BYTES = 1024 * 1024;
constexpr size_t MAX_PUBLISH_OPERATION_STATUS_REQUEST_BYTES = 64 * 1024;

std::string
getString(const JsonValue::Object& object, const char* key)
{
  return detail::member(object, key, JsonValue::Kind::String).string;
}

uint64_t
getUnsigned(const JsonValue::Object& object, const char* key)
{
  return detail::member(object, key, JsonValue::Kind::Unsigned).number;
}

uint32_t
getU32(const JsonValue::Object& object, const char* key)
{
  const auto value = getUnsigned(object, key);
  if (value > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument(std::string("artifact-json-invalid: integer overflow in ") + key);
  }
  return static_cast<uint32_t>(value);
}

bool
getBoolean(const JsonValue::Object& object, const char* key)
{
  return detail::member(object, key, JsonValue::Kind::Boolean).boolean;
}

std::vector<std::string>
getStringArray(const JsonValue::Object& object, const char* key)
{
  const auto& array = detail::member(object, key, JsonValue::Kind::Array).array;
  std::vector<std::string> result;
  result.reserve(array.size());
  for (const auto& item : array) {
    if (item.kind != JsonValue::Kind::String) {
      throw std::invalid_argument(std::string("artifact-json-invalid: non-string member in ") + key);
    }
    result.push_back(item.string);
  }
  return result;
}

std::string
stringArrayJson(const std::vector<std::string>& values)
{
  std::string result = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) result.push_back(',');
    result += detail::quoteJson(values[i]);
  }
  result.push_back(']');
  return result;
}

std::string
referenceJson(const ArtifactReference& reference)
{
  return "{\"logicalName\":" + detail::quoteJson(reference.logicalName) +
    ",\"digestAlgorithm\":" + detail::quoteJson(reference.digestAlgorithm) +
    ",\"contentDigest\":" + detail::quoteJson(reference.contentDigest) +
    ",\"sizeBytes\":" + std::to_string(reference.sizeBytes) +
    ",\"formatVersion\":" + detail::quoteJson(reference.formatVersion) +
    ",\"rootManifestName\":" + detail::quoteJson(reference.rootManifestName) +
    ",\"publisherIdentity\":" + detail::quoteJson(reference.publisherIdentity) +
    ",\"policyEpoch\":" + detail::quoteJson(reference.policyEpoch) + "}";
}

ArtifactReference
parseReference(const JsonValue& value)
{
  const auto& object = detail::requireObject(value);
  detail::requireExactKeys(object, {"logicalName", "digestAlgorithm", "contentDigest",
    "sizeBytes", "formatVersion", "rootManifestName", "publisherIdentity", "policyEpoch"});
  ArtifactReference result;
  result.logicalName = getString(object, "logicalName");
  result.digestAlgorithm = getString(object, "digestAlgorithm");
  result.contentDigest = getString(object, "contentDigest");
  result.sizeBytes = getUnsigned(object, "sizeBytes");
  result.formatVersion = getString(object, "formatVersion");
  result.rootManifestName = getString(object, "rootManifestName");
  result.publisherIdentity = getString(object, "publisherIdentity");
  result.policyEpoch = getString(object, "policyEpoch");
  return result;
}

std::string
targetJson(const RepoArtifactTarget& target)
{
  return "{\"repoNode\":" + detail::quoteJson(target.repoNode) +
    ",\"providerIdentity\":" + detail::quoteJson(target.providerIdentity) +
    ",\"servicePrefix\":" + detail::quoteJson(target.servicePrefix) + "}";
}

RepoArtifactTarget
parseTarget(const JsonValue& value)
{
  const auto& object = detail::requireObject(value);
  detail::requireExactKeys(object, {"repoNode", "providerIdentity", "servicePrefix"});
  return {getString(object, "repoNode"), getString(object, "providerIdentity"),
          getString(object, "servicePrefix")};
}

std::vector<RepoArtifactTarget>
parseTargets(const JsonValue& value)
{
  if (value.kind != JsonValue::Kind::Array || value.array.size() > 256) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "operation targets must be a bounded array");
  }
  std::vector<RepoArtifactTarget> result;
  result.reserve(value.array.size());
  for (const auto& item : value.array) result.push_back(parseTarget(item));
  return result;
}

std::string
targetsJson(const std::vector<RepoArtifactTarget>& targets)
{
  std::string result = "[";
  for (size_t i = 0; i < targets.size(); ++i) {
    if (i != 0) result.push_back(',');
    result += targetJson(targets[i]);
  }
  result.push_back(']');
  return result;
}

RepoArtifactCommitReceipt
parseCommitReceipt(const JsonValue& value)
{
  const auto& object = detail::requireObject(value);
  detail::requireExactKeys(object, {"schemaVersion", "repoNode", "providerIdentity",
    "operationId", "receiptId", "artifact", "generation", "persistedBytes",
    "completedAtMs", "locationVersion", "routeState", "forwardingHint"});
  if (getUnsigned(object, "schemaVersion") != 1) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "unsupported Repo commit receipt version");
  }
  RepoArtifactCommitReceipt result;
  result.repoNode = getString(object, "repoNode");
  result.providerIdentity = getString(object, "providerIdentity");
  result.operationId = getString(object, "operationId");
  result.receiptId = getString(object, "receiptId");
  result.reference = parseReference(object.at("artifact"));
  result.generation = getUnsigned(object, "generation");
  result.persistedBytes = getUnsigned(object, "persistedBytes");
  result.completedAtMs = getUnsigned(object, "completedAtMs");
  result.locationVersion = getUnsigned(object, "locationVersion");
  result.routeState = getString(object, "routeState");
  result.forwardingHint = getString(object, "forwardingHint");
  return result;
}

PublishOperationTargetStatus
parseTargetStatus(const JsonValue& value)
{
  const auto& object = detail::requireObject(value);
  detail::requireExactKeys(object, {"target", "state", "attemptCount", "updatedAtMs",
    "receipt", "failureCode"});
  PublishOperationTargetStatus result;
  result.target = parseTarget(object.at("target"));
  result.state = getString(object, "state");
  result.attemptCount = getU32(object, "attemptCount");
  result.updatedAtMs = getUnsigned(object, "updatedAtMs");
  const auto& receipt = object.at("receipt");
  if (receipt.kind != JsonValue::Kind::Null) {
    result.receipt = parseCommitReceipt(receipt);
  }
  result.failureCode = getString(object, "failureCode");
  return result;
}

std::string
targetStatusJson(const PublishOperationTargetStatus& status)
{
  return "{\"target\":" + targetJson(status.target) +
    ",\"state\":" + detail::quoteJson(status.state) +
    ",\"attemptCount\":" + std::to_string(status.attemptCount) +
    ",\"updatedAtMs\":" + std::to_string(status.updatedAtMs) +
    ",\"receipt\":" + (status.receipt ? status.receipt->toJson() : "null") +
    ",\"failureCode\":" + detail::quoteJson(status.failureCode) + "}";
}

std::string
targetStatusesJson(const std::vector<PublishOperationTargetStatus>& targets)
{
  std::string result = "[";
  for (size_t i = 0; i < targets.size(); ++i) {
    if (i != 0) result.push_back(',');
    result += targetStatusJson(targets[i]);
  }
  result.push_back(']');
  return result;
}

std::string
publishOperationStatusJson(const PublishOperationStatus& status)
{
  return "{\"schemaVersion\":1,\"operationId\":" + detail::quoteJson(status.operationId) +
    ",\"artifact\":" + referenceJson(status.reference) +
    ",\"generation\":" + std::to_string(status.generation) +
    ",\"seed\":" + targetJson(status.seed) +
    ",\"consistency\":" + detail::quoteJson(status.consistency) +
    ",\"targets\":" + targetStatusesJson(status.targets) +
    ",\"state\":" + detail::quoteJson(status.state) +
    ",\"updatedAtMs\":" + std::to_string(status.updatedAtMs) +
    ",\"receiptId\":" + detail::quoteJson(status.receiptId) + "}";
}

void
validatePublishStatusSizeEnvelope(const PublishOperationIntent& intent,
                                  const ArtifactLimits& limits)
{
  const std::string maximumReceiptId(256, '\x01');
  PublishOperationStatus maximum;
  maximum.operationId = std::string(64, 'a');
  maximum.reference = intent.reference;
  maximum.generation = intent.generation;
  maximum.seed = intent.seed;
  maximum.consistency = intent.consistency;
  maximum.state = "COMMITTED";
  maximum.updatedAtMs = std::numeric_limits<uint64_t>::max();
  maximum.receiptId = maximumReceiptId;
  maximum.targets.reserve(intent.targets.size());
  for (const auto& target : intent.targets) {
    RepoArtifactCommitReceipt receipt;
    receipt.repoNode = target.repoNode;
    receipt.providerIdentity = target.providerIdentity;
    receipt.operationId = maximum.operationId;
    receipt.receiptId = maximumReceiptId;
    receipt.reference = intent.reference;
    receipt.generation = intent.generation;
    receipt.persistedBytes = intent.reference.sizeBytes;
    receipt.completedAtMs = std::numeric_limits<uint64_t>::max();
    receipt.locationVersion = std::numeric_limits<uint64_t>::max();
    receipt.routeState = "ROUTABLE";
    receipt.forwardingHint = target.servicePrefix;

    PublishOperationTargetStatus targetStatus;
    targetStatus.target = target;
    targetStatus.state = "COMMITTED";
    targetStatus.attemptCount = 65535;
    targetStatus.updatedAtMs = std::numeric_limits<uint64_t>::max();
    targetStatus.receipt = std::move(receipt);
    maximum.targets.push_back(std::move(targetStatus));
  }
  maximum.validate(limits);
  if (publishOperationStatusJson(maximum).size() >
      MAX_PUBLISH_OPERATION_STATUS_JSON_BYTES) {
    throw ArtifactValidationError(
      artifact_error::LimitExceeded,
      "selected publish set can produce a status larger than the 1 MiB wire limit");
  }
}

bool
isUtf8(const std::string& value)
{
  const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
  size_t i = 0;
  while (i < value.size()) {
    const unsigned char first = bytes[i++];
    if (first <= 0x7f) continue;
    uint32_t codePoint = 0;
    size_t continuation = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      codePoint = first & 0x1f;
      continuation = 1;
    }
    else if (first >= 0xe0 && first <= 0xef) {
      codePoint = first & 0x0f;
      continuation = 2;
    }
    else if (first >= 0xf0 && first <= 0xf4) {
      codePoint = first & 0x07;
      continuation = 3;
    }
    else {
      return false;
    }
    if (continuation > value.size() - i) return false;
    const unsigned char second = bytes[i];
    if ((first == 0xe0 && second < 0xa0) ||
        (first == 0xed && second >= 0xa0) ||
        (first == 0xf0 && second < 0x90) ||
        (first == 0xf4 && second >= 0x90)) {
      return false;
    }
    for (size_t j = 0; j < continuation; ++j) {
      const unsigned char next = bytes[i++];
      if ((next & 0xc0) != 0x80) return false;
      codePoint = (codePoint << 6) | (next & 0x3f);
    }
    if (codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
      return false;
    }
  }
  return true;
}

void
validateCanonicalName(const std::string& value, const ArtifactLimits& limits,
                      const std::string& field)
{
  validateName(value, limits, field);
  if (ndn::Name(value).toUri() != value) {
    throw ArtifactValidationError(artifact_error::InvalidName,
                                  field + " must use canonical NDN URI spelling");
  }
}

void
appendU32(std::string& output, uint32_t value)
{
  output.push_back(static_cast<char>(value >> 24));
  output.push_back(static_cast<char>(value >> 16));
  output.push_back(static_cast<char>(value >> 8));
  output.push_back(static_cast<char>(value));
}

void
appendU64(std::string& output, uint64_t value)
{
  for (int shift = 56; shift >= 0; shift -= 8) {
    output.push_back(static_cast<char>(value >> shift));
  }
}

void
appendString(std::string& output, const std::string& value)
{
  if (value.size() > std::numeric_limits<uint32_t>::max()) {
    throw ArtifactValidationError(artifact_error::LimitExceeded,
                                  "canonical digest field is too long");
  }
  appendU32(output, static_cast<uint32_t>(value.size()));
  output.append(value);
}

std::string
sha256Hex(const std::string& value)
{
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest.data());
  static constexpr char HEX[] = "0123456789abcdef";
  std::string result;
  result.resize(digest.size() * 2);
  for (size_t i = 0; i < digest.size(); ++i) {
    result[i * 2] = HEX[digest[i] >> 4];
    result[i * 2 + 1] = HEX[digest[i] & 0x0f];
  }
  return result;
}

std::string
decodeSha256Hex(const std::string& value)
{
  validateDigest("sha256", value, "sha256 digest");
  auto hexValue = [] (unsigned char ch) -> unsigned char {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return ch - 'A' + 10;
  };
  std::string result(32, '\0');
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] = static_cast<char>((hexValue(value[i * 2]) << 4) |
                                  hexValue(value[i * 2 + 1]));
  }
  return result;
}

bool
targetByteLess(const RepoArtifactTarget& lhs, const RepoArtifactTarget& rhs)
{
  const std::array<std::string_view, 3> left = {
    lhs.repoNode, lhs.providerIdentity, lhs.servicePrefix};
  const std::array<std::string_view, 3> right = {
    rhs.repoNode, rhs.providerIdentity, rhs.servicePrefix};
  for (size_t field = 0; field < left.size(); ++field) {
    const auto& a = left[field];
    const auto& b = right[field];
    const size_t common = std::min(a.size(), b.size());
    for (size_t i = 0; i < common; ++i) {
      const auto ac = static_cast<unsigned char>(a[i]);
      const auto bc = static_cast<unsigned char>(b[i]);
      if (ac != bc) return ac < bc;
    }
    if (a.size() != b.size()) return a.size() < b.size();
  }
  return false;
}

void
appendTarget(std::string& output, const RepoArtifactTarget& target)
{
  appendString(output, target.repoNode);
  appendString(output, target.providerIdentity);
  appendString(output, target.servicePrefix);
}

} // namespace

bool
isHex(const std::string& value)
{
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isxdigit(ch) != 0;
  });
}

bool
isKnownFormat(const std::string& value)
{
  return value == "artifact-manifest-v2" || value == "exact-packet-v1";
}

bool
isKnownDigestAlgorithm(const std::string& value)
{
  return value == "sha256";
}

bool
isPublicRootSignatureAlgorithm(const std::string& value)
{
  return value == "rsa-sha256" || value == "ecdsa-sha256" || value == "ed25519";
}

void
validateName(const std::string& value, const ArtifactLimits& limits,
             const std::string& field)
{
  if (value.empty() || value.front() != '/' || value.find('\0') != std::string::npos ||
      value.find("//") != std::string::npos) {
    throw ArtifactValidationError(
      artifact_error::InvalidName, field + " must be one canonical absolute NDN name");
  }
  if (value.size() > limits.maxNameBytes) {
    throw ArtifactValidationError(
      artifact_error::LimitExceeded, field + " exceeds the configured name limit");
  }
}

void
validateIdentifier(const std::string& value, size_t maxBytes,
                   const std::string& field, const char* errorCode)
{
  if (value.empty() || value.size() > maxBytes ||
      value.find('\0') != std::string::npos) {
    throw ArtifactValidationError(
      errorCode, field + " must be non-empty and within its encoded limit");
  }
}

void
validateDigest(const std::string& algorithm, const std::string& digest,
               const std::string& field)
{
  if (!isKnownDigestAlgorithm(algorithm)) {
    throw ArtifactValidationError(
      artifact_error::UnsupportedAlgorithm,
      field + " uses unsupported digest algorithm " + algorithm);
  }
  if (digest.size() != 64 || !isHex(digest)) {
    throw ArtifactValidationError(
      artifact_error::InvalidDigest, field + " must be a 32-byte SHA-256 hex digest");
  }
}

void
validateUniqueStrings(const std::vector<std::string>& values, size_t maximum,
                      const std::string& field, const char* errorCode)
{
  if (values.empty() || values.size() > maximum) {
    throw ArtifactValidationError(
      errorCode, field + " must be non-empty and within the configured count limit");
  }
  std::set<std::string> unique;
  for (const auto& value : values) {
    if (value.empty() || value.size() > 128 || !unique.insert(value).second) {
      throw ArtifactValidationError(
        errorCode, field + " contains an empty, oversized, or duplicate value");
    }
  }
}

void
ArtifactCapabilityRequirements::validate(const ArtifactLimits& hardLimits) const
{
  artifact.validate(hardLimits);
  if (artifact.formatVersion == "artifact-manifest-v2") {
    if (!isPublicRootSignatureAlgorithm(rootSignatureAlgorithm)) {
      throw ArtifactValidationError(
        artifact_error::UnsupportedAlgorithm,
        "artifact-manifest-v2 requires one supported public root signature algorithm");
    }
    if (chunkBytes == 0 || chunkBytes > hardLimits.maxChunkBytes ||
        rootEncodedBytes == 0 || rootEncodedBytes > hardLimits.maxRootEncodedBytes ||
        pageEncodedBytes == 0 || pageEncodedBytes > hardLimits.maxPageEncodedBytes ||
        pageEntries == 0 || pageEntries > hardLimits.maxPageEntries ||
        manifestDepth == 0 || manifestDepth > hardLimits.maxManifestDepth) {
      throw ArtifactValidationError(
        artifact_error::LimitExceeded,
        "artifact capability requirements exceed hard parser or transfer limits");
    }
    return;
  }

  if (!rootSignatureAlgorithm.empty() || chunkBytes != 0 ||
      rootEncodedBytes != 0 || pageEncodedBytes != 0 || pageEntries != 0 ||
      manifestDepth != 0 || requireResume || requireReplicaReceipts) {
    throw ArtifactValidationError(
      artifact_error::InvalidCapability,
      "exact-packet-v1 cannot be negotiated with artifact-manifest-v2 requirements");
  }
}

std::vector<std::string>
ArtifactCapability::incompatibilities(
  const ArtifactCapabilityRequirements& requirements,
  const ArtifactLimits& hardLimits) const
{
  validate(hardLimits);
  requirements.validate(hardLimits);

  std::vector<std::string> reasons;
  const auto& artifact = requirements.artifact;
  if (std::find(formatVersions.begin(), formatVersions.end(),
                artifact.formatVersion) == formatVersions.end()) {
    reasons.emplace_back("format-version");
  }
  if (std::find(digestAlgorithms.begin(), digestAlgorithms.end(),
                artifact.digestAlgorithm) == digestAlgorithms.end()) {
    reasons.emplace_back("digest-algorithm");
  }
  if (artifact.sizeBytes > maxArtifactBytes) {
    reasons.emplace_back("artifact-size-limit");
  }
  if (artifact.policyEpoch != policyEpoch) {
    reasons.emplace_back("policy-epoch");
  }

  if (artifact.formatVersion == "artifact-manifest-v2") {
    if (std::find(signatureAlgorithms.begin(), signatureAlgorithms.end(),
                  requirements.rootSignatureAlgorithm) ==
        signatureAlgorithms.end()) {
      reasons.emplace_back("root-signature-algorithm");
    }
    if (requirements.chunkBytes > maxChunkBytes) {
      reasons.emplace_back("chunk-size-limit");
    }
    if (requirements.rootEncodedBytes > maxRootEncodedBytes) {
      reasons.emplace_back("root-size-limit");
    }
    if (requirements.pageEncodedBytes > maxPageEncodedBytes) {
      reasons.emplace_back("page-size-limit");
    }
    if (requirements.pageEntries > maxPageEntries) {
      reasons.emplace_back("page-entry-limit");
    }
    if (requirements.manifestDepth > maxManifestDepth) {
      reasons.emplace_back("manifest-depth-limit");
    }
    if (requirements.requireResume && !supportsResume) {
      reasons.emplace_back("resume");
    }
    if (requirements.requireReplicaReceipts && !supportsReplicaReceipts) {
      reasons.emplace_back("replica-receipts");
    }
  }
  return reasons;
}

void
ArtifactCapability::requireSupport(
  const ArtifactCapabilityRequirements& requirements,
  const ArtifactLimits& hardLimits) const
{
  const auto reasons = incompatibilities(requirements, hardLimits);
  if (!reasons.empty()) {
    throw ArtifactValidationError(
      artifact_error::UnsupportedCapability,
      "repository " + repoNode + " does not satisfy " + reasons.front());
  }
}

std::string
ArtifactReference::toJson() const
{
  validate();
  return referenceJson(*this);
}

ArtifactReference
ArtifactReference::fromJson(const std::string& wire,
                            const ArtifactLimits& limits)
{
  const auto result = parseReference(detail::parseJson(wire, 16 * 1024));
  result.validate(limits);
  return result;
}

std::string
artifactStorageObjectName(const ArtifactReference& reference)
{
  const auto wire = reference.toJson();
  unsigned char digest[SHA256_DIGEST_LENGTH]{};
  SHA256(reinterpret_cast<const unsigned char*>(wire.data()), wire.size(), digest);
  static constexpr char HEX[] = "0123456789abcdef";
  std::string result = "/_ndnsf/artifact-v2/";
  result.reserve(result.size() + SHA256_DIGEST_LENGTH * 2);
  for (const auto value : digest) {
    result.push_back(HEX[value >> 4]);
    result.push_back(HEX[value & 0x0f]);
  }
  return result;
}

std::string
ArtifactCapabilityAck::toJson() const
{
  if (schemaVersion != 1) {
    throw ArtifactValidationError(
      artifact_error::InvalidCapability, "unsupported capability ACK schema version");
  }
  capability.validate();
  const auto& value = capability;
  std::string result = "{\"schemaVersion\":1,\"capability\":{";
  result += "\"repoNode\":" + detail::quoteJson(value.repoNode);
  result += ",\"formatVersions\":" + stringArrayJson(value.formatVersions);
  result += ",\"digestAlgorithms\":" + stringArrayJson(value.digestAlgorithms);
  result += ",\"signatureAlgorithms\":" + stringArrayJson(value.signatureAlgorithms);
  result += ",\"maxArtifactBytes\":" + std::to_string(value.maxArtifactBytes);
  result += ",\"maxChunkBytes\":" + std::to_string(value.maxChunkBytes);
  result += ",\"maxRootEncodedBytes\":" + std::to_string(value.maxRootEncodedBytes);
  result += ",\"maxPageEncodedBytes\":" + std::to_string(value.maxPageEncodedBytes);
  result += ",\"maxPageEntries\":" + std::to_string(value.maxPageEntries);
  result += ",\"maxManifestDepth\":" + std::to_string(value.maxManifestDepth);
  result += std::string(",\"supportsResume\":") + (value.supportsResume ? "true" : "false");
  result += std::string(",\"supportsReplicaReceipts\":") +
            (value.supportsReplicaReceipts ? "true" : "false");
  result += ",\"policyEpoch\":" + detail::quoteJson(value.policyEpoch) + "}}";
  if (result.size() > 16 * 1024) {
    throw ArtifactValidationError(
      artifact_error::LimitExceeded, "capability ACK exceeds its 16 KiB wire limit");
  }
  return result;
}

ArtifactCapabilityAck
ArtifactCapabilityAck::fromJson(const std::string& wire,
                                const ArtifactLimits& hardLimits)
{
  const auto root = detail::parseJson(wire, 16 * 1024);
  const auto& envelope = detail::requireObject(root);
  detail::requireExactKeys(envelope, {"schemaVersion", "capability"});
  if (getUnsigned(envelope, "schemaVersion") != 1) {
    throw ArtifactValidationError(
      artifact_error::InvalidCapability, "unsupported capability ACK schema version");
  }
  const auto& object = detail::requireObject(envelope.at("capability"));
  detail::requireExactKeys(object, {"repoNode", "formatVersions", "digestAlgorithms",
    "signatureAlgorithms", "maxArtifactBytes", "maxChunkBytes", "maxRootEncodedBytes",
    "maxPageEncodedBytes", "maxPageEntries", "maxManifestDepth", "supportsResume",
    "supportsReplicaReceipts", "policyEpoch"});
  ArtifactCapabilityAck result;
  result.capability.repoNode = getString(object, "repoNode");
  result.capability.formatVersions = getStringArray(object, "formatVersions");
  result.capability.digestAlgorithms = getStringArray(object, "digestAlgorithms");
  result.capability.signatureAlgorithms = getStringArray(object, "signatureAlgorithms");
  result.capability.maxArtifactBytes = getUnsigned(object, "maxArtifactBytes");
  result.capability.maxChunkBytes = getUnsigned(object, "maxChunkBytes");
  result.capability.maxRootEncodedBytes = getUnsigned(object, "maxRootEncodedBytes");
  result.capability.maxPageEncodedBytes = getUnsigned(object, "maxPageEncodedBytes");
  result.capability.maxPageEntries = getU32(object, "maxPageEntries");
  result.capability.maxManifestDepth = getU32(object, "maxManifestDepth");
  result.capability.supportsResume = getBoolean(object, "supportsResume");
  result.capability.supportsReplicaReceipts = getBoolean(object, "supportsReplicaReceipts");
  result.capability.policyEpoch = getString(object, "policyEpoch");
  result.capability.validate(hardLimits);
  return result;
}

void
ArtifactLocationRecord::validate(uint64_t nowMs,
                                 const ArtifactLimits& limits) const
{
  if (schemaVersion != 1 || locationVersion == 0 || generation == 0 ||
      committedAtMs == 0 || expiresAtMs <= committedAtMs ||
      (nowMs != 0 && nowMs >= expiresAtMs)) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "location version or validity interval is invalid");
  }
  validateName(repoNode, limits, "repoNode");
  validateName(providerIdentity, limits, "providerIdentity");
  validateIdentifier(operationId, 256, "operationId", artifact_error::InvalidReceipt);
  validateIdentifier(receiptId, 256, "receiptId", artifact_error::InvalidReceipt);
  reference.validate(limits);
  if (routeState == "ROUTABLE") {
    validateName(forwardingHint, limits, "forwardingHint");
  }
  else if (routeState != "UNROUTABLE" || !forwardingHint.empty()) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "location route state is invalid");
  }
}

std::string
ArtifactLocationRecord::toJson() const
{
  validate();
  return "{\"schemaVersion\":1,\"repoNode\":" + detail::quoteJson(repoNode) +
    ",\"providerIdentity\":" + detail::quoteJson(providerIdentity) +
    ",\"operationId\":" + detail::quoteJson(operationId) +
    ",\"receiptId\":" + detail::quoteJson(receiptId) +
    ",\"artifact\":" + referenceJson(reference) +
    ",\"generation\":" + std::to_string(generation) +
    ",\"locationVersion\":" + std::to_string(locationVersion) +
    ",\"committedAtMs\":" + std::to_string(committedAtMs) +
    ",\"expiresAtMs\":" + std::to_string(expiresAtMs) +
    ",\"routeState\":" + detail::quoteJson(routeState) +
    ",\"forwardingHint\":" + detail::quoteJson(forwardingHint) + "}";
}

ArtifactLocationRecord
ArtifactLocationRecord::fromJson(const std::string& wire,
                                 const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, 64 * 1024);
  const auto& object = detail::requireObject(root);
  detail::requireExactKeys(object, {"schemaVersion", "repoNode", "providerIdentity",
    "operationId", "receiptId", "artifact", "generation", "locationVersion",
    "committedAtMs", "expiresAtMs", "routeState", "forwardingHint"});
  if (getUnsigned(object, "schemaVersion") != 1) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unsupported location schema version");
  }
  ArtifactLocationRecord result;
  result.repoNode = getString(object, "repoNode");
  result.providerIdentity = getString(object, "providerIdentity");
  result.operationId = getString(object, "operationId");
  result.receiptId = getString(object, "receiptId");
  result.reference = parseReference(object.at("artifact"));
  result.generation = getUnsigned(object, "generation");
  result.locationVersion = getUnsigned(object, "locationVersion");
  result.committedAtMs = getUnsigned(object, "committedAtMs");
  result.expiresAtMs = getUnsigned(object, "expiresAtMs");
  result.routeState = getString(object, "routeState");
  result.forwardingHint = getString(object, "forwardingHint");
  result.validate(0, limits);
  return result;
}

std::string
ArtifactLocationRequest::toJson() const
{
  if (schemaVersion != 1) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unsupported location request version");
  }
  reference.validate();
  return "{\"schemaVersion\":1,\"artifact\":" + referenceJson(reference) + "}";
}

ArtifactLocationRequest
ArtifactLocationRequest::fromJson(const std::string& wire,
                                  const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, 32 * 1024);
  const auto& object = detail::requireObject(root);
  detail::requireExactKeys(object, {"schemaVersion", "artifact"});
  if (getUnsigned(object, "schemaVersion") != 1) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unsupported location request version");
  }
  ArtifactLocationRequest result;
  result.reference = parseReference(object.at("artifact"));
  result.reference.validate(limits);
  return result;
}

std::string
RepoReceiptLookupRequest::toJson() const
{
  reference.validate();
  validateIdentifier(receiptId, 256, "receiptId", artifact_error::InvalidReceipt);
  validateName(repoNode, ArtifactLimits{}, "repoNode");
  validateName(providerIdentity, ArtifactLimits{}, "providerIdentity");
  return "{\"schemaVersion\":1,\"operation\":\"RECEIPT\",\"receiptId\":" +
    detail::quoteJson(receiptId) + ",\"repoNode\":" + detail::quoteJson(repoNode) +
    ",\"providerIdentity\":" + detail::quoteJson(providerIdentity) +
    ",\"artifact\":" + referenceJson(reference) + "}";
}

RepoReceiptLookupRequest
RepoReceiptLookupRequest::fromJson(const std::string& wire,
                                   const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, 64 * 1024);
  const auto& object = detail::requireObject(root);
  detail::requireExactKeys(object, {"schemaVersion", "operation", "receiptId",
    "repoNode", "providerIdentity", "artifact"});
  if (getUnsigned(object, "schemaVersion") != 1 ||
      getString(object, "operation") != "RECEIPT") {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unsupported receipt lookup request");
  }
  RepoReceiptLookupRequest result;
  result.receiptId = getString(object, "receiptId");
  result.repoNode = getString(object, "repoNode");
  result.providerIdentity = getString(object, "providerIdentity");
  result.reference = parseReference(object.at("artifact"));
  result.reference.validate(limits);
  validateIdentifier(result.receiptId, 256, "receiptId", artifact_error::InvalidReceipt);
  validateName(result.repoNode, limits, "repoNode");
  validateName(result.providerIdentity, limits, "providerIdentity");
  return result;
}

void
ArtifactExpectedLocation::validate(const ArtifactLimits& limits) const
{
  validateName(repoNode, limits, "expectedLocation.repoNode");
  validateName(providerIdentity, limits, "expectedLocation.providerIdentity");
  validateIdentifier(operationId, 256, "expectedLocation.operationId",
                      artifact_error::InvalidReceipt);
  validateIdentifier(receiptId, 256, "expectedLocation.receiptId",
                      artifact_error::InvalidReceipt);
  if (generation == 0 || locationVersion == 0) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "expected location generation/version must be positive");
  }
}

std::string
ArtifactExpectedLocation::toJson() const
{
  validate();
  return "{\"repoNode\":" + detail::quoteJson(repoNode) +
    ",\"providerIdentity\":" + detail::quoteJson(providerIdentity) +
    ",\"operationId\":" + detail::quoteJson(operationId) +
    ",\"receiptId\":" + detail::quoteJson(receiptId) +
    ",\"generation\":" + std::to_string(generation) +
    ",\"locationVersion\":" + std::to_string(locationVersion) + "}";
}

ArtifactExpectedLocation
ArtifactExpectedLocation::fromJson(const std::string& wire,
                                   const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, 16 * 1024);
  const auto& object = detail::requireObject(root);
  detail::requireExactKeys(object, {"repoNode", "providerIdentity", "operationId",
    "receiptId", "generation", "locationVersion"});
  ArtifactExpectedLocation result;
  result.repoNode = getString(object, "repoNode");
  result.providerIdentity = getString(object, "providerIdentity");
  result.operationId = getString(object, "operationId");
  result.receiptId = getString(object, "receiptId");
  result.generation = getUnsigned(object, "generation");
  result.locationVersion = getUnsigned(object, "locationVersion");
  result.validate(limits);
  return result;
}

std::string
RepoAvailabilityProbeRequest::toJson() const
{
  reference.validate();
  expectedLocation.validate();
  if (schemaVersion != 1 ||
      (probeKind != "metadata" && probeKind != "verify_content")) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "availability request version or probe kind is invalid");
  }
  return "{\"schemaVersion\":1,\"operation\":\"AVAILABILITY\",\"artifact\":" +
    referenceJson(reference) + ",\"expectedLocation\":" + expectedLocation.toJson() +
    ",\"probeKind\":" + detail::quoteJson(probeKind) + "}";
}

RepoAvailabilityProbeRequest
RepoAvailabilityProbeRequest::fromJson(const std::string& wire,
                                       const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, 64 * 1024);
  const auto& object = detail::requireObject(root);
  detail::requireExactKeys(object, {"schemaVersion", "operation", "artifact",
    "expectedLocation", "probeKind"});
  if (getUnsigned(object, "schemaVersion") != 1 ||
      getString(object, "operation") != "AVAILABILITY") {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unsupported availability request");
  }
  RepoAvailabilityProbeRequest result;
  result.reference = parseReference(object.at("artifact"));
  const auto& expected = detail::requireObject(object.at("expectedLocation"));
  detail::requireExactKeys(expected, {"repoNode", "providerIdentity", "operationId",
    "receiptId", "generation", "locationVersion"});
  result.expectedLocation.repoNode = getString(expected, "repoNode");
  result.expectedLocation.providerIdentity = getString(expected, "providerIdentity");
  result.expectedLocation.operationId = getString(expected, "operationId");
  result.expectedLocation.receiptId = getString(expected, "receiptId");
  result.expectedLocation.generation = getUnsigned(expected, "generation");
  result.expectedLocation.locationVersion = getUnsigned(expected, "locationVersion");
  result.expectedLocation.validate(limits);
  result.probeKind = getString(object, "probeKind");
  result.reference.validate(limits);
  if (result.probeKind != "metadata" && result.probeKind != "verify_content") {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unsupported availability probe kind");
  }
  return result;
}

void
AvailabilityLocationReport::validate(const ArtifactLimits& limits) const
{
  if (schemaVersion != 1 ||
      (locationKind != "IN_APP" && locationKind != "PERSISTENT") ||
      (reachability != "REACHABLE" && reachability != "UNREACHABLE" &&
       reachability != "UNKNOWN") ||
      (objectPresence != "PRESENT" && objectPresence != "ABSENT" &&
       objectPresence != "PARTIAL" && objectPresence != "UNKNOWN") ||
      (integrity != "NOT_CHECKED" && integrity != "VERIFIED" &&
       integrity != "FAILED") ||
      (probeKind != "metadata" && probeKind != "verify_content") ||
      observedAtMs == 0 || validUntilMs < observedAtMs || reason.size() > 256 ||
      !isUtf8(reason)) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "availability report fields are invalid");
  }
  if (locationKind == "PERSISTENT") {
    validateName(repoNode, limits, "repoNode");
    validateName(providerIdentity, limits, "providerIdentity");
    const bool hasCurrentTuple = generation != 0 && locationVersion != 0 &&
      !operationId.empty() && !receiptId.empty();
    const bool hasNoCurrentTuple = locationVersion == 0 && operationId.empty() &&
      receiptId.empty();
    if ((!hasCurrentTuple && !hasNoCurrentTuple) ||
        (hasNoCurrentTuple && (objectPresence != "UNKNOWN" ||
                               integrity != "NOT_CHECKED")) ||
        (generation == 0 && (reachability != "UNKNOWN" ||
                             !hasNoCurrentTuple ||
                             objectPresence != "UNKNOWN" ||
                             integrity != "NOT_CHECKED"))) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "Persistent current availability tuple is incomplete");
    }
    if (hasCurrentTuple) {
      validateIdentifier(operationId, 256, "operationId", artifact_error::InvalidReceipt);
      validateIdentifier(receiptId, 256, "receiptId", artifact_error::InvalidReceipt);
    }
  }
  else if (!repoNode.empty() || !providerIdentity.empty() ||
           !operationId.empty() || !receiptId.empty() || locationVersion != 0) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt,
      "In-App report cannot claim Repo, Provider, or current LOCATION identity");
  }
  if (reachability != "REACHABLE" &&
      (objectPresence != "UNKNOWN" || integrity != "NOT_CHECKED")) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unreachable or unknown location cannot claim object state");
  }
  if ((objectPresence == "UNKNOWN" || objectPresence == "ABSENT" ||
       objectPresence == "PARTIAL") && integrity != "NOT_CHECKED") {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unverified object state must use NOT_CHECKED integrity");
  }
  if (integrity != "NOT_CHECKED" &&
      (reachability != "REACHABLE" || objectPresence != "PRESENT" ||
       probeKind != "verify_content")) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "content integrity requires a reachable complete object probe");
  }
}

std::string
AvailabilityLocationReport::toJson() const
{
  validate();
  return "{\"schemaVersion\":1,\"locationKind\":" + detail::quoteJson(locationKind) +
    ",\"repoNode\":" + detail::quoteJson(repoNode) +
    ",\"providerIdentity\":" + detail::quoteJson(providerIdentity) +
    ",\"generation\":" + std::to_string(generation) +
    (locationVersion != 0 ? ",\"operationId\":" + detail::quoteJson(operationId) +
      ",\"receiptId\":" + detail::quoteJson(receiptId) +
      ",\"locationVersion\":" + std::to_string(locationVersion) : std::string{}) +
    ",\"reachability\":" + detail::quoteJson(reachability) +
    ",\"objectPresence\":" + detail::quoteJson(objectPresence) +
    ",\"integrity\":" + detail::quoteJson(integrity) +
    ",\"observedAtMs\":" + std::to_string(observedAtMs) +
    ",\"validUntilMs\":" + std::to_string(validUntilMs) +
    ",\"probeKind\":" + detail::quoteJson(probeKind) +
    ",\"reason\":" + detail::quoteJson(reason) + "}";
}

AvailabilityLocationReport
AvailabilityLocationReport::fromJson(const std::string& wire,
                                     const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, 16 * 1024);
  const auto& object = detail::requireObject(root);
  const bool hasOperation = object.count("operationId") != 0;
  const bool hasReceipt = object.count("receiptId") != 0;
  const bool hasLocationVersion = object.count("locationVersion") != 0;
  if (hasOperation != hasReceipt || hasOperation != hasLocationVersion) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "availability current tuple fields must appear together");
  }
  if (hasOperation) {
    detail::requireExactKeys(object, {"schemaVersion", "locationKind", "repoNode",
      "providerIdentity", "generation", "operationId", "receiptId", "locationVersion",
      "reachability", "objectPresence", "integrity", "observedAtMs", "validUntilMs",
      "probeKind", "reason"});
  }
  else {
    detail::requireExactKeys(object, {"schemaVersion", "locationKind", "repoNode",
      "providerIdentity", "generation", "reachability", "objectPresence", "integrity",
      "observedAtMs", "validUntilMs", "probeKind", "reason"});
  }
  if (getUnsigned(object, "schemaVersion") != 1) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unsupported availability report version");
  }
  AvailabilityLocationReport result;
  result.locationKind = getString(object, "locationKind");
  result.repoNode = getString(object, "repoNode");
  result.providerIdentity = getString(object, "providerIdentity");
  result.generation = getUnsigned(object, "generation");
  if (hasOperation) {
    result.operationId = getString(object, "operationId");
    result.receiptId = getString(object, "receiptId");
    result.locationVersion = getUnsigned(object, "locationVersion");
    if (result.operationId.empty() || result.receiptId.empty() ||
        result.locationVersion == 0) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "availability current tuple is empty");
    }
  }
  result.reachability = getString(object, "reachability");
  result.objectPresence = getString(object, "objectPresence");
  result.integrity = getString(object, "integrity");
  result.observedAtMs = getUnsigned(object, "observedAtMs");
  result.validUntilMs = getUnsigned(object, "validUntilMs");
  result.probeKind = getString(object, "probeKind");
  result.reason = getString(object, "reason");
  result.validate(limits);
  return result;
}

void
RepoAvailabilityProbeResponse::validate(const ArtifactLimits& limits) const
{
  if (schemaVersion != 1) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unsupported availability response version");
  }
  reference.validate(limits);
  expectedLocation.validate(limits);
  report.validate(limits);
  if (report.locationKind != "PERSISTENT" ||
      report.repoNode != expectedLocation.repoNode ||
      report.providerIdentity != expectedLocation.providerIdentity ||
      report.generation != expectedLocation.generation ||
      report.reachability != "REACHABLE" ||
      (!report.operationId.empty() &&
       (report.operationId != expectedLocation.operationId ||
        report.receiptId != expectedLocation.receiptId ||
        report.locationVersion != expectedLocation.locationVersion))) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "availability response report differs from echoed request binding");
  }
}

std::string
RepoAvailabilityProbeResponse::toJson() const
{
  validate();
  return "{\"schemaVersion\":1,\"artifact\":" + referenceJson(reference) +
    ",\"expectedLocation\":" + expectedLocation.toJson() +
    ",\"report\":" + report.toJson() + "}";
}

RepoAvailabilityProbeResponse
RepoAvailabilityProbeResponse::fromJson(const std::string& wire,
                                        const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, 64 * 1024);
  const auto& object = detail::requireObject(root);
  detail::requireExactKeys(object, {"schemaVersion", "artifact", "expectedLocation", "report"});
  if (getUnsigned(object, "schemaVersion") != 1) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unsupported availability response version");
  }
  RepoAvailabilityProbeResponse result;
  result.reference = parseReference(object.at("artifact"));
  const auto& expected = detail::requireObject(object.at("expectedLocation"));
  detail::requireExactKeys(expected, {"repoNode", "providerIdentity", "operationId",
    "receiptId", "generation", "locationVersion"});
  result.expectedLocation.repoNode = getString(expected, "repoNode");
  result.expectedLocation.providerIdentity = getString(expected, "providerIdentity");
  result.expectedLocation.operationId = getString(expected, "operationId");
  result.expectedLocation.receiptId = getString(expected, "receiptId");
  result.expectedLocation.generation = getUnsigned(expected, "generation");
  result.expectedLocation.locationVersion = getUnsigned(expected, "locationVersion");

  const auto& report = detail::requireObject(object.at("report"));
  const bool hasOperation = report.count("operationId") != 0;
  const bool hasReceipt = report.count("receiptId") != 0;
  const bool hasVersion = report.count("locationVersion") != 0;
  if (hasOperation != hasReceipt || hasOperation != hasVersion) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "availability response current tuple fields must appear together");
  }
  if (hasOperation) {
    detail::requireExactKeys(report, {"schemaVersion", "locationKind", "repoNode",
      "providerIdentity", "generation", "operationId", "receiptId", "locationVersion",
      "reachability", "objectPresence", "integrity", "observedAtMs", "validUntilMs",
      "probeKind", "reason"});
  }
  else {
    detail::requireExactKeys(report, {"schemaVersion", "locationKind", "repoNode",
      "providerIdentity", "generation", "reachability", "objectPresence", "integrity",
      "observedAtMs", "validUntilMs", "probeKind", "reason"});
  }
  if (getUnsigned(report, "schemaVersion") != 1) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unsupported availability location version");
  }
  auto& location = result.report;
  location.locationKind = getString(report, "locationKind");
  location.repoNode = getString(report, "repoNode");
  location.providerIdentity = getString(report, "providerIdentity");
  location.generation = getUnsigned(report, "generation");
  if (hasOperation) {
    location.operationId = getString(report, "operationId");
    location.receiptId = getString(report, "receiptId");
    location.locationVersion = getUnsigned(report, "locationVersion");
    if (location.operationId.empty() || location.receiptId.empty() ||
        location.locationVersion == 0) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "availability response current tuple is empty");
    }
  }
  location.reachability = getString(report, "reachability");
  location.objectPresence = getString(report, "objectPresence");
  location.integrity = getString(report, "integrity");
  location.observedAtMs = getUnsigned(report, "observedAtMs");
  location.validUntilMs = getUnsigned(report, "validUntilMs");
  location.probeKind = getString(report, "probeKind");
  location.reason = getString(report, "reason");
  result.validate(limits);
  return result;
}

void
AvailabilityReport::validate(const ArtifactLimits& limits) const
{
  if (schemaVersion != 1 || locations.empty() || locations.size() > 256 ||
      (probeKind != "metadata" && probeKind != "verify_content")) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "availability report version or location count is invalid");
  }
  reference.validate(limits);
  for (const auto& location : locations) {
    location.validate(limits);
    if (location.probeKind != probeKind) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "availability location probe kind differs from its report");
    }
  }
}

std::string
AvailabilityReport::toJson() const
{
  validate();
  std::string result = "{\"schemaVersion\":1,\"artifact\":" +
    referenceJson(reference) + ",\"probeKind\":" + detail::quoteJson(probeKind) +
    ",\"locations\":[";
  for (size_t i = 0; i < locations.size(); ++i) {
    if (i != 0) result.push_back(',');
    result += locations[i].toJson();
  }
  result += "]}";
  return result;
}

AvailabilityReport
AvailabilityReport::fromJson(const std::string& wire,
                             const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, 2 * 1024 * 1024);
  const auto& object = detail::requireObject(root);
  detail::requireExactKeys(object, {"schemaVersion", "artifact", "probeKind", "locations"});
  if (getUnsigned(object, "schemaVersion") != 1) {
    throw ArtifactValidationError(
      artifact_error::InvalidReceipt, "unsupported availability report version");
  }
  const auto& rows = detail::member(object, "locations", JsonValue::Kind::Array).array;
  AvailabilityReport result;
  result.reference = parseReference(object.at("artifact"));
  result.probeKind = getString(object, "probeKind");
  result.locations.reserve(rows.size());
  for (const auto& row : rows) {
    const auto& item = detail::requireObject(row);
    const bool hasOperation = item.count("operationId") != 0;
    const bool hasReceipt = item.count("receiptId") != 0;
    const bool hasLocationVersion = item.count("locationVersion") != 0;
    if (hasOperation != hasReceipt || hasOperation != hasLocationVersion) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "availability current tuple fields must appear together");
    }
    if (hasOperation) {
      detail::requireExactKeys(item, {"schemaVersion", "locationKind", "repoNode",
        "providerIdentity", "generation", "operationId", "receiptId", "locationVersion",
        "reachability", "objectPresence", "integrity", "observedAtMs", "validUntilMs",
        "probeKind", "reason"});
    }
    else {
      detail::requireExactKeys(item, {"schemaVersion", "locationKind", "repoNode",
        "providerIdentity", "generation", "reachability", "objectPresence", "integrity",
        "observedAtMs", "validUntilMs", "probeKind", "reason"});
    }
    if (getUnsigned(item, "schemaVersion") != 1) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt, "unsupported availability location version");
    }
    AvailabilityLocationReport location;
    location.locationKind = getString(item, "locationKind");
    location.repoNode = getString(item, "repoNode");
    location.providerIdentity = getString(item, "providerIdentity");
    location.generation = getUnsigned(item, "generation");
    if (hasOperation) {
      location.operationId = getString(item, "operationId");
      location.receiptId = getString(item, "receiptId");
      location.locationVersion = getUnsigned(item, "locationVersion");
      if (location.operationId.empty() || location.receiptId.empty() ||
          location.locationVersion == 0) {
        throw ArtifactValidationError(
          artifact_error::InvalidReceipt, "availability current tuple is empty");
      }
    }
    location.reachability = getString(item, "reachability");
    location.objectPresence = getString(item, "objectPresence");
    location.integrity = getString(item, "integrity");
    location.observedAtMs = getUnsigned(item, "observedAtMs");
    location.validUntilMs = getUnsigned(item, "validUntilMs");
    location.probeKind = getString(item, "probeKind");
    location.reason = getString(item, "reason");
    location.validate(limits);
    result.locations.push_back(std::move(location));
  }
  result.validate(limits);
  return result;
}

void
PublishOperationIntent::validate(const ArtifactLimits& limits) const
{
  if (schemaVersion != 1 || generation == 0 || targets.empty() || targets.size() > 256 ||
      (consistency != "ONE" && consistency != "QUORUM" && consistency != "ALL")) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "publish intent version, generation, consistency, or targets are invalid");
  }
  validateDigest("sha256", idempotencyKeyDigest, "idempotency key digest");
  validateDigest("sha256", requestDigest, "publish request digest");
  reference.validate(limits);
  validateCanonicalName(seed.repoNode, limits, "seed.repoNode");
  validateCanonicalName(seed.providerIdentity, limits, "seed.providerIdentity");
  validateCanonicalName(seed.servicePrefix, limits, "seed.servicePrefix");
  std::set<std::string> repoNodes;
  std::set<std::string> providers;
  bool containsSeed = false;
  for (const auto& target : targets) {
    validateCanonicalName(target.repoNode, limits, "target.repoNode");
    validateCanonicalName(target.providerIdentity, limits, "target.providerIdentity");
    validateCanonicalName(target.servicePrefix, limits, "target.servicePrefix");
    if (!repoNodes.insert(target.repoNode).second ||
        !providers.insert(target.providerIdentity).second) {
      throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                    "publish intent targets must have distinct Repo and Provider identities");
    }
    containsSeed = containsSeed || target == seed;
  }
  if (!containsSeed) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "publish intent seed must be one of its selected targets");
  }
  if (requestDigest != makePublishRequestDigest(reference, generation, seed,
                                                consistency, targets)) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "publish request digest does not bind the intent fields");
  }
  validatePublishStatusSizeEnvelope(*this, limits);
}

std::string
PublishOperationIntent::toJson() const
{
  validate();
  return "{\"schemaVersion\":1,\"operation\":\"PUBLISH_INTENT\",\"idempotencyKeyDigest\":" +
    detail::quoteJson(idempotencyKeyDigest) + ",\"requestDigest\":" +
    detail::quoteJson(requestDigest) + ",\"artifact\":" + referenceJson(reference) +
    ",\"generation\":" + std::to_string(generation) + ",\"seed\":" +
    targetJson(seed) + ",\"consistency\":" + detail::quoteJson(consistency) +
    ",\"targets\":" + targetsJson(targets) + "}";
}

PublishOperationIntent
PublishOperationIntent::fromJson(const std::string& wire, const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, 256 * 1024);
  const auto& object = detail::requireObject(root);
  detail::requireExactKeys(object, {"schemaVersion", "operation", "idempotencyKeyDigest",
    "requestDigest", "artifact", "generation", "seed", "consistency", "targets"});
  if (getUnsigned(object, "schemaVersion") != 1 ||
      getString(object, "operation") != "PUBLISH_INTENT") {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "unsupported publish intent envelope");
  }
  PublishOperationIntent result;
  result.idempotencyKeyDigest = getString(object, "idempotencyKeyDigest");
  result.requestDigest = getString(object, "requestDigest");
  result.reference = parseReference(object.at("artifact"));
  result.generation = getUnsigned(object, "generation");
  result.seed = parseTarget(object.at("seed"));
  result.consistency = getString(object, "consistency");
  result.targets = parseTargets(object.at("targets"));
  result.validate(limits);
  return result;
}

std::string
PublishOperationStatusRequest::toJson() const
{
  if (schemaVersion != 1) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "unsupported publish status request version");
  }
  validateDigest("sha256", operationId, "publish operation ID");
  std::string result = "{\"schemaVersion\":1,\"operation\":\"PUBLISH_STATUS\",\"operationId\":" +
    detail::quoteJson(operationId);
  if (target) {
    const ArtifactLimits limits;
    validateCanonicalName(target->repoNode, limits, "target.repoNode");
    validateCanonicalName(target->providerIdentity, limits, "target.providerIdentity");
    validateCanonicalName(target->servicePrefix, limits, "target.servicePrefix");
    result += ",\"target\":" + targetJson(*target);
  }
  result.push_back('}');
  if (result.size() > MAX_PUBLISH_OPERATION_STATUS_REQUEST_BYTES) {
    throw ArtifactValidationError(artifact_error::LimitExceeded,
                                  "publish status request exceeds its wire limit");
  }
  return result;
}

PublishOperationStatusRequest
PublishOperationStatusRequest::fromJson(const std::string& wire)
{
  const auto root = detail::parseJson(wire, MAX_PUBLISH_OPERATION_STATUS_REQUEST_BYTES);
  const auto& object = detail::requireObject(root);
  if (object.size() == 3) {
    detail::requireExactKeys(object, {"schemaVersion", "operation", "operationId"});
  }
  else {
    detail::requireExactKeys(object, {"schemaVersion", "operation", "operationId", "target"});
  }
  if (getUnsigned(object, "schemaVersion") != 1 ||
      getString(object, "operation") != "PUBLISH_STATUS") {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "unsupported publish status request envelope");
  }
  PublishOperationStatusRequest result;
  result.operationId = getString(object, "operationId");
  validateDigest("sha256", result.operationId, "publish operation ID");
  const auto target = object.find("target");
  if (target != object.end()) {
    result.target = parseTarget(target->second);
    const ArtifactLimits limits;
    validateCanonicalName(result.target->repoNode, limits, "target.repoNode");
    validateCanonicalName(result.target->providerIdentity, limits, "target.providerIdentity");
    validateCanonicalName(result.target->servicePrefix, limits, "target.servicePrefix");
  }
  return result;
}

void
PublishOperationHandle::validate(const ArtifactLimits& limits) const
{
  if (schemaVersion != 1) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "unsupported publish operation handle version");
  }
  validateDigest("sha256", operationId, "publish operation ID");
  validateCanonicalName(seed.repoNode, limits, "seed.repoNode");
  validateCanonicalName(seed.providerIdentity, limits, "seed.providerIdentity");
  validateCanonicalName(seed.servicePrefix, limits, "seed.servicePrefix");
}

std::string
PublishOperationHandle::toJson() const
{
  validate();
  return "{\"schemaVersion\":1,\"operationId\":" + detail::quoteJson(operationId) +
    ",\"seed\":" + targetJson(seed) + "}";
}

PublishOperationHandle
PublishOperationHandle::fromJson(const std::string& wire, const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, 16 * 1024);
  const auto& object = detail::requireObject(root);
  detail::requireExactKeys(object, {"schemaVersion", "operationId", "seed"});
  if (getUnsigned(object, "schemaVersion") != 1) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "unsupported publish operation handle version");
  }
  PublishOperationHandle result;
  result.operationId = getString(object, "operationId");
  result.seed = parseTarget(object.at("seed"));
  result.validate(limits);
  return result;
}

void
RepoArtifactCommitReceipt::validate(const ArtifactLimits& limits) const
{
  if (schemaVersion != 1 || generation == 0 || completedAtMs == 0 ||
      persistedBytes != reference.sizeBytes ||
      (routeState != "ROUTABLE" && routeState != "UNROUTABLE") ||
      (routeState == "ROUTABLE" && (locationVersion == 0 || forwardingHint.empty())) ||
      (routeState == "UNROUTABLE" && (locationVersion != 0 || !forwardingHint.empty()))) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "Repo commit receipt fields are inconsistent");
  }
  validateCanonicalName(repoNode, limits, "repoNode");
  validateCanonicalName(providerIdentity, limits, "providerIdentity");
  validateDigest("sha256", operationId, "operationId");
  validateIdentifier(receiptId, 256, "receiptId", artifact_error::InvalidReceipt);
  reference.validate(limits);
  if (routeState == "ROUTABLE") {
    validateCanonicalName(forwardingHint, limits, "forwardingHint");
  }
}

std::string
RepoArtifactCommitReceipt::toJson() const
{
  validate();
  return "{\"schemaVersion\":1,\"repoNode\":" + detail::quoteJson(repoNode) +
    ",\"providerIdentity\":" + detail::quoteJson(providerIdentity) +
    ",\"operationId\":" + detail::quoteJson(operationId) +
    ",\"receiptId\":" + detail::quoteJson(receiptId) +
    ",\"artifact\":" + referenceJson(reference) +
    ",\"generation\":" + std::to_string(generation) +
    ",\"persistedBytes\":" + std::to_string(persistedBytes) +
    ",\"completedAtMs\":" + std::to_string(completedAtMs) +
    ",\"locationVersion\":" + std::to_string(locationVersion) +
    ",\"routeState\":" + detail::quoteJson(routeState) +
    ",\"forwardingHint\":" + detail::quoteJson(forwardingHint) + "}";
}

RepoArtifactCommitReceipt
RepoArtifactCommitReceipt::fromJson(const std::string& wire,
                                    const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, 64 * 1024);
  auto result = parseCommitReceipt(root);
  result.validate(limits);
  return result;
}

void
PublishOperationStatus::validate(const ArtifactLimits& limits) const
{
  if (schemaVersion != 1 || generation == 0 || updatedAtMs == 0 || targets.empty() ||
      targets.size() > 256 ||
      (consistency != "ONE" && consistency != "QUORUM" && consistency != "ALL") ||
      (state != "ACCEPTED" && state != "IN_PROGRESS" && state != "COMMITTED" &&
       state != "PARTIAL" && state != "FAILED")) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "publish operation status fields are invalid");
  }
  validateDigest("sha256", operationId, "publish operation ID");
  reference.validate(limits);
  validateCanonicalName(seed.repoNode, limits, "seed.repoNode");
  validateCanonicalName(seed.providerIdentity, limits, "seed.providerIdentity");
  validateCanonicalName(seed.servicePrefix, limits, "seed.servicePrefix");
  std::set<std::string> repos;
  std::set<std::string> providers;
  bool containsSeed = false;
  uint32_t committed = 0;
  bool hasNonterminal = false;
  const PublishOperationTargetStatus* seedStatus = nullptr;
  for (const auto& targetStatus : targets) {
    const auto& target = targetStatus.target;
    validateCanonicalName(target.repoNode, limits, "target.repoNode");
    validateCanonicalName(target.providerIdentity, limits, "target.providerIdentity");
    validateCanonicalName(target.servicePrefix, limits, "target.servicePrefix");
    if (!repos.insert(target.repoNode).second ||
        !providers.insert(target.providerIdentity).second || targetStatus.updatedAtMs == 0 ||
        targetStatus.attemptCount > 65535) {
      throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                    "publish operation target status is duplicated or invalid");
    }
    containsSeed = containsSeed || target == seed;
    if (target == seed) seedStatus = &targetStatus;
    const auto& targetState = targetStatus.state;
    if (targetState == "COMMITTED") {
      if (!targetStatus.receipt || !targetStatus.failureCode.empty()) {
        throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                      "committed target must have one receipt and no failure");
      }
      const auto& receipt = *targetStatus.receipt;
      receipt.validate(limits);
      if (receipt.repoNode != target.repoNode ||
          receipt.providerIdentity != target.providerIdentity ||
          receipt.operationId != operationId || receipt.reference != reference ||
          receipt.generation != generation || receipt.persistedBytes != reference.sizeBytes) {
        throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                      "target receipt does not bind operation, target, or artifact");
      }
      ++committed;
    }
    else if (targetState == "QUEUED" || targetState == "IN_FLIGHT" ||
             targetState == "STAGED") {
      if (targetStatus.receipt || !targetStatus.failureCode.empty()) {
        throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                      "nonterminal target cannot carry a receipt or failure");
      }
      hasNonterminal = true;
    }
    else if (targetState == "FAILED" || targetState == "UNKNOWN") {
      if (targetStatus.receipt || targetStatus.failureCode.empty()) {
        throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                      "failed or unknown target requires a failure code only");
      }
    }
    else if (targetState == "NOT_ATTEMPTED") {
      if (targetStatus.receipt || !targetStatus.failureCode.empty()) {
        throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                      "unattempted target cannot carry a receipt or failure");
      }
    }
    else {
      throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                    "unknown publish target state");
    }
  }
  if (!containsSeed || seedStatus == nullptr || seedStatus->state != "COMMITTED" ||
      (state == "ACCEPTED" && committed == 0) ||
      (state == "IN_PROGRESS" && !hasNonterminal) ||
      (state == "COMMITTED" &&
       committed < requiredWriteAcks(static_cast<uint32_t>(targets.size()),
                                     parseRepoWriteConsistency(consistency))) ||
      (state == "PARTIAL" &&
       (committed == 0 || committed >= requiredWriteAcks(
          static_cast<uint32_t>(targets.size()), parseRepoWriteConsistency(consistency)))) ||
      (state == "FAILED" && hasNonterminal) ||
      ((state == "COMMITTED" || state == "PARTIAL") && hasNonterminal) ||
      ((state == "ACCEPTED" || state == "IN_PROGRESS") && !receiptId.empty()) ||
      ((state == "COMMITTED" || state == "PARTIAL" || state == "FAILED") && receiptId.empty())) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "publish operation state does not match target outcomes");
  }
  if (!receiptId.empty()) {
    validateIdentifier(receiptId, 256, "receiptId", artifact_error::InvalidReceipt);
  }
}

std::string
PublishOperationStatus::toJson() const
{
  validate();
  auto wire = publishOperationStatusJson(*this);
  if (wire.size() > MAX_PUBLISH_OPERATION_STATUS_JSON_BYTES) {
    throw ArtifactValidationError(artifact_error::LimitExceeded,
                                  "publish operation status exceeds the 1 MiB wire limit");
  }
  return wire;
}

PublishOperationStatus
PublishOperationStatus::fromJson(const std::string& wire, const ArtifactLimits& limits)
{
  const auto root = detail::parseJson(wire, MAX_PUBLISH_OPERATION_STATUS_JSON_BYTES);
  const auto& object = detail::requireObject(root);
  detail::requireExactKeys(object, {"schemaVersion", "operationId", "artifact", "generation",
    "seed", "consistency", "targets", "state", "updatedAtMs", "receiptId"});
  if (getUnsigned(object, "schemaVersion") != 1) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "unsupported publish operation status version");
  }
  PublishOperationStatus result;
  result.operationId = getString(object, "operationId");
  result.reference = parseReference(object.at("artifact"));
  result.generation = getUnsigned(object, "generation");
  result.seed = parseTarget(object.at("seed"));
  result.consistency = getString(object, "consistency");
  const auto& targetValues = object.at("targets");
  if (targetValues.kind != JsonValue::Kind::Array || targetValues.array.size() > 256) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "publish status targets must be a bounded array");
  }
  result.targets.reserve(targetValues.array.size());
  for (const auto& value : targetValues.array) {
    result.targets.push_back(parseTargetStatus(value));
  }
  result.state = getString(object, "state");
  result.updatedAtMs = getUnsigned(object, "updatedAtMs");
  result.receiptId = getString(object, "receiptId");
  result.validate(limits);
  return result;
}

std::string
digestPublishIdempotencyKey(const std::string& key)
{
  if (key.empty() || key.size() > 256 || !isUtf8(key)) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "idempotency key must contain 1 to 256 valid UTF-8 bytes");
  }
  std::string material = "ndnsf-repo-idempotency-key-v1";
  appendU32(material, static_cast<uint32_t>(key.size()));
  material.append(key);
  return sha256Hex(material);
}

std::string
makePublishOperationId(const std::string& submittingPrincipal,
                       const RepoArtifactTarget& seed,
                       const std::string& idempotencyKeyDigest)
{
  validateCanonicalName(submittingPrincipal, ArtifactLimits{}, "submittingPrincipal");
  validateCanonicalName(seed.repoNode, ArtifactLimits{}, "seed.repoNode");
  validateCanonicalName(seed.providerIdentity, ArtifactLimits{}, "seed.providerIdentity");
  validateCanonicalName(seed.servicePrefix, ArtifactLimits{}, "seed.servicePrefix");
  const auto rawKeyDigest = decodeSha256Hex(idempotencyKeyDigest);
  std::string material = "ndnsf-repo-publish-operation-v1";
  appendString(material, submittingPrincipal);
  appendTarget(material, seed);
  material.append(rawKeyDigest);
  return sha256Hex(material);
}

std::string
makePublishRequestDigest(const ArtifactReference& reference, uint64_t generation,
                         const RepoArtifactTarget& seed,
                         const std::string& consistency,
                         const std::vector<RepoArtifactTarget>& targets)
{
  reference.validate();
  if (generation == 0 || targets.empty() || targets.size() > 256 ||
      (consistency != "ONE" && consistency != "QUORUM" && consistency != "ALL")) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "publish request digest inputs are invalid");
  }
  validateCanonicalName(reference.logicalName, ArtifactLimits{}, "logicalName");
  validateCanonicalName(reference.rootManifestName, ArtifactLimits{}, "rootManifestName");
  validateCanonicalName(reference.publisherIdentity, ArtifactLimits{}, "publisherIdentity");
  validateCanonicalName(seed.repoNode, ArtifactLimits{}, "seed.repoNode");
  validateCanonicalName(seed.providerIdentity, ArtifactLimits{}, "seed.providerIdentity");
  validateCanonicalName(seed.servicePrefix, ArtifactLimits{}, "seed.servicePrefix");
  std::vector<RepoArtifactTarget> ordered = targets;
  std::sort(ordered.begin(), ordered.end(), targetByteLess);
  std::set<std::string> repos;
  std::set<std::string> providers;
  bool containsSeed = false;
  for (const auto& target : ordered) {
    validateCanonicalName(target.repoNode, ArtifactLimits{}, "target.repoNode");
    validateCanonicalName(target.providerIdentity, ArtifactLimits{}, "target.providerIdentity");
    validateCanonicalName(target.servicePrefix, ArtifactLimits{}, "target.servicePrefix");
    if (!repos.insert(target.repoNode).second ||
        !providers.insert(target.providerIdentity).second) {
      throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                    "publish request targets must have distinct Repo and Provider identities");
    }
    containsSeed = containsSeed || target == seed;
  }
  if (!containsSeed) {
    throw ArtifactValidationError(artifact_error::InvalidReceipt,
                                  "publish request seed must be one of its selected targets");
  }
  std::string material = "ndnsf-repo-publish-request-v1";
  appendString(material, reference.logicalName);
  appendString(material, reference.digestAlgorithm);
  appendString(material, reference.contentDigest);
  appendU64(material, reference.sizeBytes);
  appendString(material, reference.formatVersion);
  appendString(material, reference.rootManifestName);
  appendString(material, reference.publisherIdentity);
  appendString(material, reference.policyEpoch);
  appendU64(material, generation);
  appendTarget(material, seed);
  appendString(material, consistency);
  appendU32(material, static_cast<uint32_t>(ordered.size()));
  for (const auto& target : ordered) appendTarget(material, target);
  return sha256Hex(material);
}

} // namespace ndnsf_distributed_repo
