#ifndef NDNSF_DI_NATIVE_GRANT_CLIENT_HPP
#define NDNSF_DI_NATIVE_GRANT_CLIENT_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace ndn { class Buffer; class Name; }

namespace ndnsf::di {

struct NativeGrantRequest
{
  std::string requesterIdentity;
  std::string providerIdentity;
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string planCoreDigest;
  std::string modelManifestDigest;
  std::string protectionEpoch;
  std::string artifactDigest;
  std::uint64_t expiresAtMs = 0;
};

struct NativeKeyGrant
{
  std::string grantName;
  std::string grantDigest;
  std::string recipient;
  std::string wireJson;
  std::uint64_t expiresAtMs = 0;
};

/** Native policy issuer port. Cryptographic envelope/signature creation is
 * supplied by the configured authority; this class owns validation only. */
class NativeArtifactPolicyAuthority
{
public:
  using IssuePort = std::function<NativeKeyGrant(const NativeGrantRequest&)>;

  explicit NativeArtifactPolicyAuthority(IssuePort issuePort);
  NativeKeyGrant issue(const NativeGrantRequest& request,
                       std::chrono::system_clock::time_point now) const;

private:
  IssuePort m_issuePort;
};

/** Requester-side grant acquisition and exact-name publication. */
class NativeGrantClient
{
public:
  using PublishPort = std::function<std::string(const std::string&,
                                                const std::string&)>;

  NativeGrantClient(std::string requesterIdentity,
                    std::shared_ptr<const NativeArtifactPolicyAuthority> authority,
                    PublishPort publish);

  NativeGrantBinding acquire(
    const NativeProviderGrantView& view,
    std::chrono::system_clock::time_point deadline) const;

private:
  std::string m_requesterIdentity;
  std::shared_ptr<const NativeArtifactPolicyAuthority> m_authority;
  PublishPort m_publish;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_GRANT_CLIENT_HPP
