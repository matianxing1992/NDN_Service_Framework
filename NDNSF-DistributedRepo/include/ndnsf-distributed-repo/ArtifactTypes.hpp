#ifndef NDNSF_DISTRIBUTED_REPO_ARTIFACT_TYPES_HPP
#define NDNSF_DISTRIBUTED_REPO_ARTIFACT_TYPES_HPP

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf_distributed_repo {

namespace artifact_error {
inline constexpr const char* InvalidName = "artifact-invalid-name";
inline constexpr const char* InvalidDigest = "artifact-invalid-digest";
inline constexpr const char* UnsupportedFormat = "artifact-unsupported-format";
inline constexpr const char* UnsupportedAlgorithm = "artifact-unsupported-algorithm";
inline constexpr const char* LimitExceeded = "artifact-limit-exceeded";
inline constexpr const char* InvalidRange = "artifact-invalid-range";
inline constexpr const char* InvalidManifest = "artifact-invalid-manifest";
inline constexpr const char* InvalidCapability = "artifact-invalid-capability";
inline constexpr const char* UnsupportedCapability = "artifact-unsupported-capability";
inline constexpr const char* InvalidLease = "artifact-invalid-lease";
inline constexpr const char* InvalidReceipt = "artifact-invalid-receipt";
} // namespace artifact_error

class ArtifactValidationError : public std::invalid_argument
{
public:
  ArtifactValidationError(std::string code, std::string message)
    : std::invalid_argument(code + ": " + message)
    , m_code(std::move(code))
  {
  }

  const std::string&
  code() const noexcept
  {
    return m_code;
  }

private:
  std::string m_code;
};

struct ArtifactLimits
{
  uint64_t maxArtifactBytes = 1ULL << 50; // 1 PiB hard policy default.
  uint64_t maxChunkBytes = 64ULL * 1024 * 1024;
  uint64_t maxRootEncodedBytes = 64ULL * 1024;
  uint64_t maxPageEncodedBytes = 4ULL * 1024 * 1024;
  uint32_t maxPageEntries = 65536;
  uint32_t maxManifestDepth = 16;
  uint32_t maxCriticalExtensions = 32;
  uint32_t maxNameBytes = 4096;
  uint32_t maxPacketPayloadBytes = 8800;
  uint32_t maxSignatureBytes = 16384;
  uint32_t maxManifestPages = 1U << 20;
  uint32_t maxManifestChunks = 1U << 24;
  uint64_t maxCryptographicOperations = 1ULL << 26;
};

bool
isHex(const std::string& value);

bool
isKnownFormat(const std::string& value);

bool
isKnownDigestAlgorithm(const std::string& value);

bool
isPublicRootSignatureAlgorithm(const std::string& value);

void
validateName(const std::string& value, const ArtifactLimits& limits,
             const std::string& field);

void
validateIdentifier(const std::string& value, size_t maxBytes, const std::string& field,
                   const char* errorCode);

void
validateDigest(const std::string& algorithm, const std::string& digest,
               const std::string& field);

void
validateUniqueStrings(const std::vector<std::string>& values, size_t maximum,
                      const std::string& field, const char* errorCode);

struct ArtifactReference
{
  std::string logicalName;
  std::string digestAlgorithm = "sha256";
  std::string contentDigest;
  uint64_t sizeBytes = 0;
  std::string formatVersion = "artifact-manifest-v2";
  std::string rootManifestName;
  std::string publisherIdentity;
  std::string policyEpoch;

  void
  validate(const ArtifactLimits& limits = {}) const
  {
    validateName(logicalName, limits, "logicalName");
    validateDigest(digestAlgorithm, contentDigest, "contentDigest");
    if (sizeBytes > limits.maxArtifactBytes) {
      throw ArtifactValidationError(
        artifact_error::LimitExceeded, "artifact size exceeds the configured limit");
    }
    if (!isKnownFormat(formatVersion)) {
      throw ArtifactValidationError(
        artifact_error::UnsupportedFormat, "unsupported artifact format " + formatVersion);
    }
    validateName(rootManifestName, limits, "rootManifestName");
    validateName(publisherIdentity, limits, "publisherIdentity");
    validateIdentifier(policyEpoch, 256, "policyEpoch", artifact_error::InvalidManifest);
  }

  bool
  sameBytes(const ArtifactReference& other) const
  {
    return digestAlgorithm == other.digestAlgorithm &&
           contentDigest == other.contentDigest &&
           sizeBytes == other.sizeBytes;
  }
};

/**
 * Exact requirements for one artifact-manifest-v2 replica.
 *
 * These values are derived from the selected manifest geometry; they are not
 * advisory provider telemetry.  An exact-packet-v1 request must leave every
 * v2-only field empty/zero so legacy packet trust is never reinterpreted as a
 * signed-root manifest.
 */
struct ArtifactCapabilityRequirements
{
  ArtifactReference artifact;
  std::string rootSignatureAlgorithm;
  uint64_t chunkBytes = 0;
  uint64_t rootEncodedBytes = 0;
  uint64_t pageEncodedBytes = 0;
  uint32_t pageEntries = 0;
  uint32_t manifestDepth = 0;
  bool requireResume = false;
  bool requireReplicaReceipts = false;

  void
  validate(const ArtifactLimits& hardLimits = {}) const;
};

struct ArtifactCapability
{
  std::string repoNode;
  std::vector<std::string> formatVersions;
  std::vector<std::string> digestAlgorithms;
  std::vector<std::string> signatureAlgorithms;
  uint64_t maxArtifactBytes = 0;
  uint64_t maxChunkBytes = 0;
  uint64_t maxRootEncodedBytes = 0;
  uint64_t maxPageEncodedBytes = 0;
  uint32_t maxPageEntries = 0;
  uint32_t maxManifestDepth = 0;
  bool supportsResume = false;
  bool supportsReplicaReceipts = false;
  std::string policyEpoch;

  void
  validate(const ArtifactLimits& hardLimits = {}) const
  {
    validateName(repoNode, hardLimits, "repoNode");
    validateUniqueStrings(
      formatVersions, 16, "formatVersions", artifact_error::InvalidCapability);
    validateUniqueStrings(
      digestAlgorithms, 16, "digestAlgorithms", artifact_error::InvalidCapability);
    validateUniqueStrings(
      signatureAlgorithms, 16, "signatureAlgorithms", artifact_error::InvalidCapability);
    for (const auto& value : formatVersions) {
      if (!isKnownFormat(value)) {
        throw ArtifactValidationError(
          artifact_error::UnsupportedFormat, "capability advertises unknown format " + value);
      }
    }
    for (const auto& value : digestAlgorithms) {
      if (!isKnownDigestAlgorithm(value)) {
        throw ArtifactValidationError(
          artifact_error::UnsupportedAlgorithm,
          "capability advertises unknown digest algorithm " + value);
      }
    }
    for (const auto& value : signatureAlgorithms) {
      if (!isPublicRootSignatureAlgorithm(value)) {
        throw ArtifactValidationError(
          artifact_error::UnsupportedAlgorithm,
          "capability advertises unsupported public root signature " + value);
      }
    }
    if (maxArtifactBytes == 0 || maxArtifactBytes > hardLimits.maxArtifactBytes ||
        maxChunkBytes == 0 || maxChunkBytes > hardLimits.maxChunkBytes ||
        maxRootEncodedBytes == 0 ||
        maxRootEncodedBytes > hardLimits.maxRootEncodedBytes ||
        maxPageEncodedBytes == 0 ||
        maxPageEncodedBytes > hardLimits.maxPageEncodedBytes ||
        maxPageEntries == 0 || maxPageEntries > hardLimits.maxPageEntries ||
        maxManifestDepth == 0 || maxManifestDepth > hardLimits.maxManifestDepth) {
      throw ArtifactValidationError(
        artifact_error::LimitExceeded,
        "capability limits must be positive and no larger than hard safety limits");
    }
    validateIdentifier(
      policyEpoch, 256, "policyEpoch", artifact_error::InvalidCapability);
  }

  bool
  supports(const ArtifactReference& reference, const std::string& rootSignature) const
  {
    return std::find(formatVersions.begin(), formatVersions.end(),
                     reference.formatVersion) != formatVersions.end() &&
           std::find(digestAlgorithms.begin(), digestAlgorithms.end(),
                     reference.digestAlgorithm) != digestAlgorithms.end() &&
           std::find(signatureAlgorithms.begin(), signatureAlgorithms.end(),
                     rootSignature) != signatureAlgorithms.end() &&
           reference.sizeBytes <= maxArtifactBytes &&
           reference.policyEpoch == policyEpoch;
  }

  std::vector<std::string>
  incompatibilities(const ArtifactCapabilityRequirements& requirements,
                    const ArtifactLimits& hardLimits = {}) const;

  void
  requireSupport(const ArtifactCapabilityRequirements& requirements,
                 const ArtifactLimits& hardLimits = {}) const;
};

struct ArtifactManifestChild
{
  std::string kind;
  uint64_t index = 0;
  uint64_t offsetBytes = 0;
  uint64_t lengthBytes = 0;
  std::string digestAlgorithm = "sha256";
  std::string digest;

  void
  validate(const ArtifactLimits& limits = {}) const
  {
    if (kind != "page" && kind != "chunk") {
      throw ArtifactValidationError(
        artifact_error::InvalidManifest, "manifest child kind must be page or chunk");
    }
    if (lengthBytes == 0 || offsetBytes > limits.maxArtifactBytes ||
        lengthBytes > limits.maxArtifactBytes - offsetBytes) {
      throw ArtifactValidationError(
        artifact_error::InvalidRange, "manifest child range is empty or overflows");
    }
    validateDigest(digestAlgorithm, digest, "manifest child digest");
  }
};

struct ArtifactRootManifest
{
  ArtifactReference artifact;
  uint32_t packetPayloadBytes = 0;
  uint64_t chunkBytes = 0;
  std::string namingTemplate;
  std::string manifestRootDigestAlgorithm = "sha256";
  std::string manifestRootDigest;
  std::string signatureAlgorithm;
  std::string publisherKeyLocator;
  uint64_t createdAtMs = 0;
  uint64_t expiresAtMs = 0;
  std::vector<std::string> criticalExtensions;

  void
  validate(uint64_t encodedBytes, const ArtifactLimits& limits = {}) const
  {
    artifact.validate(limits);
    if (artifact.formatVersion != "artifact-manifest-v2") {
      throw ArtifactValidationError(
        artifact_error::UnsupportedFormat,
        "root manifest requires artifact-manifest-v2");
    }
    if (encodedBytes == 0 || encodedBytes > limits.maxRootEncodedBytes) {
      throw ArtifactValidationError(
        artifact_error::LimitExceeded, "root manifest exceeds its encoded-size limit");
    }
    if (packetPayloadBytes == 0 ||
        packetPayloadBytes > limits.maxPacketPayloadBytes ||
        chunkBytes < packetPayloadBytes || chunkBytes > limits.maxChunkBytes) {
      throw ArtifactValidationError(
        artifact_error::LimitExceeded, "packet or chunk geometry is outside policy limits");
    }
    if (namingTemplate.empty() || namingTemplate.size() > limits.maxNameBytes ||
        namingTemplate.find("{chunk}") == std::string::npos ||
        namingTemplate.find("{segment}") == std::string::npos) {
      throw ArtifactValidationError(
        artifact_error::InvalidManifest,
        "namingTemplate must be bounded and contain {chunk} and {segment}");
    }
    validateDigest(
      manifestRootDigestAlgorithm, manifestRootDigest, "manifestRootDigest");
    if (!isPublicRootSignatureAlgorithm(signatureAlgorithm)) {
      throw ArtifactValidationError(
        artifact_error::UnsupportedAlgorithm,
        "root manifest requires an approved public signature algorithm");
    }
    validateName(publisherKeyLocator, limits, "publisherKeyLocator");
    if (createdAtMs == 0 || (expiresAtMs != 0 && expiresAtMs <= createdAtMs)) {
      throw ArtifactValidationError(
        artifact_error::InvalidManifest, "root manifest validity interval is invalid");
    }
    if (criticalExtensions.size() > limits.maxCriticalExtensions) {
      throw ArtifactValidationError(
        artifact_error::LimitExceeded, "too many critical root extensions");
    }
    std::set<std::string> unique;
    for (const auto& extension : criticalExtensions) {
      if (extension.empty() || extension.size() > 128 ||
          !unique.insert(extension).second) {
        throw ArtifactValidationError(
          artifact_error::InvalidManifest,
          "critical extensions must be bounded, non-empty, and unique");
      }
    }
  }
};

struct ArtifactManifestPage
{
  std::string pageVersion = "artifact-manifest-page-v2";
  uint32_t depth = 0;
  uint64_t offsetBytes = 0;
  uint64_t lengthBytes = 0;
  std::string pageDigestAlgorithm = "sha256";
  std::string pageDigest;
  std::vector<ArtifactManifestChild> children;

  void
  validate(uint64_t encodedBytes, const ArtifactLimits& limits = {}) const
  {
    if (pageVersion != "artifact-manifest-page-v2") {
      throw ArtifactValidationError(
        artifact_error::UnsupportedFormat, "unsupported manifest page version");
    }
    if (encodedBytes == 0 || encodedBytes > limits.maxPageEncodedBytes ||
        depth > limits.maxManifestDepth ||
        children.empty() || children.size() > limits.maxPageEntries) {
      throw ArtifactValidationError(
        artifact_error::LimitExceeded,
        "manifest page size, depth, or entry count is outside policy limits");
    }
    if (lengthBytes == 0 || offsetBytes > limits.maxArtifactBytes ||
        lengthBytes > limits.maxArtifactBytes - offsetBytes) {
      throw ArtifactValidationError(
        artifact_error::InvalidRange, "manifest page range is empty or overflows");
    }
    validateDigest(pageDigestAlgorithm, pageDigest, "pageDigest");
    uint64_t expectedOffset = offsetBytes;
    uint64_t previousIndex = 0;
    bool first = true;
    for (const auto& child : children) {
      child.validate(limits);
      if (child.offsetBytes != expectedOffset ||
          (!first && child.index <= previousIndex)) {
        throw ArtifactValidationError(
          artifact_error::InvalidRange,
          "manifest children must be ordered, strictly indexed, and gap-free");
      }
      expectedOffset += child.lengthBytes;
      previousIndex = child.index;
      first = false;
    }
    if (expectedOffset != offsetBytes + lengthBytes) {
      throw ArtifactValidationError(
        artifact_error::InvalidRange,
        "manifest children must exactly cover the page range");
    }
  }
};

struct ArtifactChunk
{
  uint64_t index = 0;
  uint64_t offsetBytes = 0;
  uint64_t lengthBytes = 0;
  std::string digestAlgorithm = "sha256";
  std::string digest;
  uint64_t firstSegment = 0;
  uint64_t finalSegment = 0;

  void
  validate(const ArtifactReference& artifact,
           const ArtifactLimits& limits = {}) const
  {
    artifact.validate(limits);
    if (lengthBytes == 0 || lengthBytes > limits.maxChunkBytes ||
        offsetBytes > artifact.sizeBytes ||
        lengthBytes > artifact.sizeBytes - offsetBytes ||
        firstSegment > finalSegment) {
      throw ArtifactValidationError(
        artifact_error::InvalidRange, "artifact chunk coordinates are invalid");
    }
    validateDigest(digestAlgorithm, digest, "chunk digest");
  }
};

struct ArtifactUploadLease
{
  std::string leaseId;
  std::string operationId;
  std::string repoNode;
  ArtifactReference artifact;
  uint64_t reservedBytes = 0;
  uint64_t issuedAtMs = 0;
  uint64_t expiresAtMs = 0;
  std::string replayId;

  void
  validate(uint64_t nowMs, const ArtifactLimits& limits = {}) const
  {
    validateIdentifier(leaseId, 256, "leaseId", artifact_error::InvalidLease);
    validateIdentifier(operationId, 256, "operationId", artifact_error::InvalidLease);
    validateIdentifier(replayId, 256, "replayId", artifact_error::InvalidLease);
    validateName(repoNode, limits, "repoNode");
    artifact.validate(limits);
    if (reservedBytes < artifact.sizeBytes ||
        issuedAtMs == 0 || expiresAtMs <= issuedAtMs || nowMs >= expiresAtMs) {
      throw ArtifactValidationError(
        artifact_error::InvalidLease,
        "lease capacity or validity interval does not authorize this artifact");
    }
  }
};

struct ArtifactReplicaReceipt
{
  std::string receiptId;
  std::string operationId;
  std::string repoNode;
  ArtifactReference artifact;
  uint64_t committedAtMs = 0;
  uint64_t storageGeneration = 0;
  std::string policyEpoch;
  std::string state = "COMMITTED";

  void
  validate(const ArtifactLimits& limits = {}) const
  {
    validateIdentifier(receiptId, 256, "receiptId", artifact_error::InvalidReceipt);
    validateIdentifier(operationId, 256, "operationId", artifact_error::InvalidReceipt);
    validateName(repoNode, limits, "repoNode");
    artifact.validate(limits);
    if (committedAtMs == 0 || state != "COMMITTED" ||
        policyEpoch != artifact.policyEpoch) {
      throw ArtifactValidationError(
        artifact_error::InvalidReceipt,
        "receipt must prove a committed artifact under its exact policy epoch");
    }
  }
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_ARTIFACT_TYPES_HPP
