#include "ndnsf-distributed-repo/ArtifactTypes.hpp"

namespace ndnsf_distributed_repo {

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

} // namespace ndnsf_distributed_repo
