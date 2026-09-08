#ifndef NDNSF_DI_NATIVE_ARTIFACT_POLICY_AUTHORITY_HPP
#define NDNSF_DI_NATIVE_ARTIFACT_POLICY_AUTHORITY_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantClient.hpp"
#include <openssl/ossl_typ.h>
#include <map>
#include <set>
#include <vector>

namespace ndnsf::di {

/** GrantRequestV1 wire value. Private keys never enter this value. */
struct NativeSignedGrantRequest
{
  std::string providerIdentity, requestId, planCoreDigest, grantViewDigest;
  std::string modelManifestDigest, protectionEpoch, requesterIdentity;
  std::uint64_t attempt = 0, issuedAtMs = 0;
  std::vector<std::string> allowedResidencyTiers{"DISK_CIPHERTEXT_ASSEMBLED"};
  std::string purpose = "DISK_CIPHERTEXT_ASSEMBLED";
  std::string requesterSignature;

  std::string signingBytes() const;
  NativeSignedGrantRequest sign(EVP_PKEY& requesterPrivateKey) const;
};

struct NativeGrantPublicationSource
{
  std::string modelName, modelContentDigest, canonicalSourceDigest;
  std::string initializerObjectDigest, artifactProfileDigest;
};

struct NativeGrantIssuerConfig
{
  std::string authorityIdentity, requesterIdentity, protectionEpoch, keyId;
  std::shared_ptr<EVP_PKEY> authorityPrivateKey, requesterPublicKey;
  std::set<std::string> allowedModelManifests;
  // Optional immutable authorization for republishing an already owned source.
  // Indexed by the original allowed manifest; never populated by a request.
  std::map<std::string, NativeGrantPublicationSource> publicationSources;
  std::set<std::string> allowedResidencyTiers{"DISK_CIPHERTEXT_ASSEMBLED"};
  std::map<std::string, std::shared_ptr<EVP_PKEY>> recipientPublicKeys;
  /** Return the already-owned model key, not a newly generated grant key. */
  std::function<std::vector<std::uint8_t>(const std::string&, const std::string&)> contentKey;
};

/** Concrete in-process policy/crypto owner, backing the requester authority seam.
 * Keys are native handles with shared lifetime and must not be mutated by their
 * caller after construction. Policy containers are copied into the owner.
 * issue has no publication side effect. Returned wire contains only ciphertext.
 */
class NativeArtifactGrantIssuer
{
public:
  explicit NativeArtifactGrantIssuer(NativeGrantIssuerConfig config);
  NativeKeyGrant issue(const NativeSignedGrantRequest& request,
                       std::uint64_t nowMs, std::uint64_t expiresAtMs) const;
  NativeKeyGrant issue(const NativeSignedGrantRequest& request,
                       std::uint64_t nowMs, std::uint64_t expiresAtMs,
                       const std::string& publishedManifestJson) const;

  const std::string& requesterIdentity() const noexcept { return m_config.requesterIdentity; }
  const std::string& protectionEpoch() const noexcept { return m_config.protectionEpoch; }

private:
  NativeGrantIssuerConfig m_config;
};

namespace detail {
void verifyNativeIssuedGrant(const NativeKeyGrant& grant, const NativeSignedGrantRequest& request,
  const std::string& authority, const std::string& authorityPublicKeyRaw,
  std::uint64_t nowMs, std::uint64_t expiresAtMs);
// Shared implementation in NativeGrantVerifier.cpp owns canonical wire and crypto.
std::string signNativeGrantBytes(EVP_PKEY& key, const std::string& bytes);
bool verifyNativeGrantBytes(EVP_PKEY& key, const std::string& bytes,
                           const std::string& signature);
NativeKeyGrant issueNativeGrantWire(const NativeSignedGrantRequest& request,
  const std::string& authority, const std::string& keyId, EVP_PKEY& authorityKey,
  EVP_PKEY& recipientKey, const std::vector<std::uint8_t>& contentKey,
  std::uint64_t nowMs, std::uint64_t expiresAtMs);
}
} // namespace ndnsf::di
#endif
