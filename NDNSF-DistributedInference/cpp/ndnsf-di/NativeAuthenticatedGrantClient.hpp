#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include <atomic>

namespace ndn_service_framework { class ServiceUser; }
namespace ndnsf::di {
struct NativeGrantControl
{
  std::chrono::system_clock::time_point deadline;
  std::shared_ptr<const std::atomic<bool>> cancelled;
  void check() const;
};

/** Authenticated grant acquisition. Production construction binds Core transport;
 * the alternate constructor substitutes transport only, never policy or crypto.
 */
class NativeAuthenticatedGrantClient
{
public:
  using Publish = std::function<std::string(const std::string&, const std::string&,
                                           const NativeGrantControl&)>;
  using Clock = std::function<std::uint64_t()>;
  /** Shared Core transport factory; its returned operation must run on a worker. */
  static Publish publishThroughCore(std::shared_ptr<ndn_service_framework::ServiceUser> user);
  NativeAuthenticatedGrantClient(std::string requester, std::shared_ptr<EVP_PKEY> requesterKey,
    std::string authority, std::string authorityPublicKeyRaw,
    std::shared_ptr<const NativeArtifactGrantIssuer> issuer,
    std::shared_ptr<ndn_service_framework::ServiceUser> user);
  NativeAuthenticatedGrantClient(std::string requester, std::shared_ptr<EVP_PKEY> requesterKey,
    std::string authority, std::string authorityPublicKeyRaw,
    std::shared_ptr<const NativeArtifactGrantIssuer> issuer, Publish publish, Clock clock = {});

  NativeGrantBinding acquire(const NativePlacementPlanCore& core,
    const NativeAdmittedOfferV3& offer, const NativeSecurityPolicySnapshot& security,
    const NativeGrantControl& control) const;
private:
  std::string m_requester, m_authority, m_authorityPublicKey;
  std::shared_ptr<EVP_PKEY> m_requesterKey;
  std::shared_ptr<const NativeArtifactGrantIssuer> m_issuer;
  Publish m_publish;
  Clock m_clock;
};
} // namespace ndnsf::di
