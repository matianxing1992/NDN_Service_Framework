#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <algorithm>
#include <stdexcept>

namespace ndnsf::di {
namespace {
std::runtime_error reject(const char* message)
{
  return std::runtime_error(std::string("DI_PROTECTED_GRANT_REJECTED: ") + message);
}
bool digest(const std::string& v)
{
  return v.size() == 71 && v.substr(0, 7) == "sha256:" &&
    std::all_of(v.begin() + 7, v.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
}

std::string NativeSignedGrantRequest::signingBytes() const
{
  if (providerIdentity.empty() || requestId.empty() || requesterIdentity.empty() ||
      attempt == 0 || !digest(planCoreDigest) || !digest(grantViewDigest) ||
      !digest(modelManifestDigest) || protectionEpoch.empty() ||
      purpose != "DISK_CIPHERTEXT_ASSEMBLED" || allowedResidencyTiers.empty() ||
      std::any_of(allowedResidencyTiers.begin(), allowedResidencyTiers.end(),
                  [](const auto& tier) { return tier.empty(); }))
    throw reject("signed request is incomplete");
  const auto bytes = NativeJson{{"providerIdentity", providerIdentity}, {"requestId", requestId},
    {"attempt", attempt}, {"planCoreDigest", planCoreDigest}, {"grantViewDigest", grantViewDigest},
    {"modelManifestDigest", modelManifestDigest}, {"protectionEpoch", protectionEpoch},
    {"requesterIdentity", requesterIdentity}, {"issuedAtMs", issuedAtMs},
    {"allowedResidencyTiers", allowedResidencyTiers}, {"purpose", purpose}}.dump();
  if (bytes.size() > 65536) throw reject("signed request exceeds wire limit");
  return bytes;
}

NativeSignedGrantRequest NativeSignedGrantRequest::sign(EVP_PKEY& key) const
{
  auto result = *this;
  result.requesterSignature = detail::signNativeGrantBytes(key, signingBytes());
  return result;
}

NativeArtifactGrantIssuer::NativeArtifactGrantIssuer(NativeGrantIssuerConfig config)
  : m_config(std::move(config))
{
  if (m_config.authorityIdentity.empty() || m_config.requesterIdentity.empty() ||
      m_config.authorityIdentity == m_config.requesterIdentity || m_config.keyId.empty() ||
      m_config.protectionEpoch.empty() || !m_config.authorityPrivateKey ||
      !m_config.requesterPublicKey || !m_config.contentKey ||
      EVP_PKEY_id(m_config.authorityPrivateKey.get()) != EVP_PKEY_ED25519 ||
      EVP_PKEY_id(m_config.requesterPublicKey.get()) != EVP_PKEY_ED25519 ||
      EVP_PKEY_cmp(m_config.authorityPrivateKey.get(), m_config.requesterPublicKey.get()) == 1 ||
      m_config.allowedModelManifests.empty() || m_config.allowedResidencyTiers.empty() ||
      m_config.allowedResidencyTiers.count("") ||
      std::any_of(m_config.allowedModelManifests.begin(), m_config.allowedModelManifests.end(),
                  [](const auto& value) { return !digest(value); }))
    throw reject("operator issuer configuration is incomplete or not independent");
  // Fail configuration before any request can consume a content key.
  for (const auto& entry : m_config.recipientPublicKeys)
    if (entry.first.empty() || !entry.second)
      throw reject("recipient registry entry is incomplete");
  detail::signNativeGrantBytes(*m_config.authorityPrivateKey, "issuer-key-preflight");
}

NativeKeyGrant NativeArtifactGrantIssuer::issue(const NativeSignedGrantRequest& request,
  std::uint64_t nowMs, std::uint64_t expiresAtMs) const
{
  const auto bytes = request.signingBytes();
  if (request.requesterIdentity != m_config.requesterIdentity ||
      request.providerIdentity == request.requesterIdentity ||
      request.protectionEpoch != m_config.protectionEpoch ||
      !m_config.allowedModelManifests.count(request.modelManifestDigest) ||
      expiresAtMs <= nowMs || request.issuedAtMs > nowMs ||
      std::any_of(request.allowedResidencyTiers.begin(), request.allowedResidencyTiers.end(),
        [&](const auto& tier) { return !m_config.allowedResidencyTiers.count(tier); }) ||
      !detail::verifyNativeGrantBytes(*m_config.requesterPublicKey, bytes, request.requesterSignature))
    throw reject("request signature or operator policy rejected");
  const auto recipient = m_config.recipientPublicKeys.find(request.providerIdentity);
  if (recipient == m_config.recipientPublicKeys.end()) throw reject("recipient key is not configured");
  auto secret = m_config.contentKey(request.modelManifestDigest, request.protectionEpoch);
  struct Cleanse { std::vector<std::uint8_t>& v; ~Cleanse() { OPENSSL_cleanse(v.data(), v.size()); } } guard{secret};
  if (secret.empty() || secret.size() > 256) throw reject("content key is empty or oversized");
  return detail::issueNativeGrantWire(request, m_config.authorityIdentity, m_config.keyId,
    *m_config.authorityPrivateKey, *recipient->second, secret, nowMs, expiresAtMs);
}
} // namespace ndnsf::di
