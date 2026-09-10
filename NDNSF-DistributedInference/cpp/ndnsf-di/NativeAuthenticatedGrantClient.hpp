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
  using Issue = std::function<NativeKeyGrant(const NativeSignedGrantRequest&,
                                             const std::string&,
                                             std::uint64_t,
                                             const NativeGrantControl&)>;
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
  /** Request a grant from an independently deployed native authority.  The
   * issue callback is transport only; it never receives authority private
   * keys or content keys. */
  NativeAuthenticatedGrantClient(std::string requester, std::shared_ptr<EVP_PKEY> requesterKey,
    std::string authority, std::string authorityPublicKeyRaw,
    std::string protectionEpoch, Issue issue, Publish publish, Clock clock = {});

  /** Core service transport for the independent authority service. */
  static Issue issueThroughCore(std::shared_ptr<ndn_service_framework::ServiceUser> user,
                                std::string authorityIdentity,
                                std::string authorityService);

  NativeGrantBinding acquire(const NativePlacementPlanCore& core,
    const NativeAdmittedOfferV3& offer, const NativeSecurityPolicySnapshot& security,
    const NativeGrantControl& control, const std::string& role = {}) const;

  /** Identity bound to the requester key and grant issuer at construction. */
  const std::string& requesterIdentity() const noexcept { return m_requester; }
  /** Protection epoch accepted by the native grant issuer. */
  const std::string& protectionEpoch() const noexcept { return m_protectionEpoch; }

private:
  std::string m_requester, m_authority, m_authorityPublicKey;
  std::string m_protectionEpoch;
  std::shared_ptr<EVP_PKEY> m_requesterKey;
  Issue m_issue;
  Publish m_publish;
  Clock m_clock;
};
} // namespace ndnsf::di
