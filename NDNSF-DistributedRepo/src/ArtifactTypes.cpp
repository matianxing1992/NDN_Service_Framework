#include "ndnsf-distributed-repo/ArtifactTypes.hpp"

#include "ndnsf-distributed-repo/detail/StrictJson.hpp"

#include <openssl/sha.h>

#include <limits>
#include <sstream>

namespace ndnsf_distributed_repo {

namespace {

using detail::JsonValue;

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

} // namespace ndnsf_distributed_repo
