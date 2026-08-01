#ifndef NDNSF_DISTRIBUTED_REPO_ARTIFACT_MANIFEST_HPP
#define NDNSF_DISTRIBUTED_REPO_ARTIFACT_MANIFEST_HPP

#include "ndnsf-distributed-repo/ArtifactTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf_distributed_repo {

namespace artifact_manifest_error {
inline constexpr const char* MalformedEncoding = "artifact-manifest-malformed-encoding";
inline constexpr const char* InvalidSignature = "artifact-manifest-invalid-signature";
inline constexpr const char* TrustPolicyRejected = "artifact-manifest-trust-policy-rejected";
inline constexpr const char* RevokedPublisher = "artifact-manifest-revoked-publisher";
inline constexpr const char* ExpiredPolicy = "artifact-manifest-expired-policy";
inline constexpr const char* UnsupportedCriticalField =
  "artifact-manifest-unsupported-critical-field";
inline constexpr const char* Substitution = "artifact-manifest-substitution";
inline constexpr const char* Downgrade = "artifact-manifest-downgrade";
inline constexpr const char* DigestMismatch = "artifact-manifest-digest-mismatch";
inline constexpr const char* InvalidGraph = "artifact-manifest-invalid-graph";
inline constexpr const char* Cycle = "artifact-manifest-cycle";
inline constexpr const char* CryptoBudgetExceeded =
  "artifact-manifest-crypto-budget-exceeded";
inline constexpr const char* MixedResume = "artifact-manifest-mixed-resume";
} // namespace artifact_manifest_error

struct SignedArtifactRoot
{
  ArtifactRootManifest root;
  std::vector<uint8_t> signatureValue;
};

/**
 * Resolved NDNSF trust-policy decision for one verification.
 *
 * The verifier never discovers trust anchors on its own.  The caller resolves
 * the publisher/key under the configured NDNSF trust schema and supplies the
 * accepted algorithms, policy epoch, evaluation time, and public key.
 */
struct ArtifactManifestTrustPolicy
{
  std::string trustedPublisherIdentity;
  std::string trustedKeyLocator;
  std::string publicKeyPem;
  std::string policyEpoch;
  uint64_t evaluationTimeMs = 0;
  std::vector<std::string> allowedDigestAlgorithms;
  std::vector<std::string> allowedSignatureAlgorithms;
  std::vector<std::string> supportedCriticalExtensions;
  std::vector<std::string> revokedKeyLocators;

  void validate(const ArtifactLimits& limits = {}) const;
};

struct ArtifactManifestVerificationResult
{
  ArtifactReference artifact;
  uint64_t verifiedPageCount = 0;
  uint64_t verifiedChunkCount = 0;
  uint64_t asymmetricVerificationCount = 0;
  uint64_t digestVerificationCount = 0;
  std::vector<std::string> derivedPageNames;
};

std::string
artifactSha256Hex(const std::vector<uint8_t>& bytes);

std::vector<uint8_t>
canonicalRootManifestBytes(const ArtifactRootManifest& root,
                           const ArtifactLimits& limits = {});

std::vector<uint8_t>
encodeSignedArtifactRoot(const SignedArtifactRoot& signedRoot,
                         const ArtifactLimits& limits = {});

SignedArtifactRoot
decodeSignedArtifactRoot(const std::vector<uint8_t>& wire,
                         const ArtifactLimits& limits = {});

std::vector<uint8_t>
canonicalManifestPageBytes(const ArtifactManifestPage& page,
                           const ArtifactLimits& limits = {});

std::vector<uint8_t>
encodeArtifactManifestPage(const ArtifactManifestPage& page,
                           const ArtifactLimits& limits = {});

ArtifactManifestPage
decodeArtifactManifestPage(const std::vector<uint8_t>& wire,
                           const ArtifactLimits& limits = {});

std::string
deriveManifestPageName(const ArtifactRootManifest& root,
                       const std::string& pageDigest);

std::string
deriveArtifactDataName(const ArtifactRootManifest& root,
                       uint64_t chunkIndex, uint64_t segment);

void
verifySignedArtifactRoot(const SignedArtifactRoot& signedRoot,
                         const ArtifactReference& expectedArtifact,
                         const ArtifactCapability& capability,
                         const ArtifactManifestTrustPolicy& policy,
                         const ArtifactLimits& limits = {});

ArtifactManifestVerificationResult
verifyArtifactManifestGraph(
  const SignedArtifactRoot& signedRoot,
  const ArtifactReference& expectedArtifact,
  const std::vector<ArtifactManifestPage>& pages,
  const std::vector<ArtifactChunk>& chunks,
  const ArtifactCapability& capability,
  const ArtifactManifestTrustPolicy& policy,
  const ArtifactLimits& limits = {});

void
verifyArtifactChunkPayload(const ArtifactChunk& chunk,
                           const std::vector<uint8_t>& payload);

void
verifyArtifactPayload(const ArtifactReference& artifact,
                      const std::vector<uint8_t>& payload);

void
validateArtifactResumeIdentity(const ArtifactReference& expectedArtifact,
                               const ArtifactRootManifest& expectedRoot,
                               const ArtifactReference& resumedArtifact,
                               const ArtifactRootManifest& resumedRoot);

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_ARTIFACT_MANIFEST_HPP
