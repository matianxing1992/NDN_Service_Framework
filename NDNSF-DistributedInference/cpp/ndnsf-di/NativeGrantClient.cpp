#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantClient.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantVerifier.hpp"

#include <algorithm>
#include <stdexcept>

namespace ndnsf::di {
namespace {

bool digest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void validateRequest(const NativeGrantRequest& request)
{
  if (request.requesterIdentity.empty() || request.providerIdentity.empty() ||
      request.requestId.empty() || request.attempt == 0 ||
      !digest(request.planCoreDigest) || !digest(request.modelManifestDigest) ||
      !digest(request.artifactDigest) || request.protectionEpoch.empty() ||
      request.expiresAtMs == 0) {
    throw std::invalid_argument("native grant request identity is incomplete");
  }
}

} // namespace

NativeArtifactPolicyAuthority::NativeArtifactPolicyAuthority(IssuePort issuePort)
  : m_issuePort(std::move(issuePort))
{
  if (!m_issuePort) throw std::invalid_argument("native grant issue port is empty");
}

NativeKeyGrant NativeArtifactPolicyAuthority::issue(
  const NativeGrantRequest& request, std::chrono::system_clock::time_point now) const
{
  validateRequest(request);
  const auto nowMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
  if (request.expiresAtMs <= nowMs) {
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: grant request is expired");
  }
  // A requester cannot authorize a grant to itself: the request must be
  // signed and issued for a distinct Provider identity (Python frozen
  // "grant requester cannot be the selected Provider").
  if (request.providerIdentity == request.requesterIdentity) {
    throw std::runtime_error(
      "DI_PROTECTED_GRANT_REJECTED: grant requester cannot be the selected Provider");
  }
  auto result = m_issuePort(request);
  if (result.grantName.empty() || !digest(result.grantDigest) ||
      result.recipient.empty() || result.wireJson.empty() ||
      result.expiresAtMs != request.expiresAtMs) {
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: issuer returned incomplete grant");
  }
  return result;
}

NativeGrantClient::NativeGrantClient(
  std::string requesterIdentity,
  std::shared_ptr<const NativeArtifactPolicyAuthority> authority,
  PublishPort publish)
  : m_requesterIdentity(std::move(requesterIdentity))
  , m_authority(std::move(authority))
  , m_publish(std::move(publish))
{
  if (m_requesterIdentity.empty() || !m_authority || !m_publish) {
    throw std::invalid_argument("native grant client ports are incomplete");
  }
}

NativeGrantBinding NativeGrantClient::acquire(
  const NativeProviderGrantView& view,
  std::chrono::system_clock::time_point deadline) const
{
  if (std::chrono::system_clock::now() >= deadline) {
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: grant acquisition deadline expired");
  }
  if (view.requesterIdentity != m_requesterIdentity || view.requestId.empty() ||
      view.attempt == 0 || view.protectionEpoch.empty() || view.expiresAtMs == 0 ||
      !digest(view.planCoreDigest) || !digest(view.modelDigest) ||
      !digest(view.graphDigest) || !digest(view.artifactDigest)) {
    throw std::invalid_argument("native provider grant view is incomplete");
  }
  NativeGrantRequest request;
  request.requesterIdentity = view.requesterIdentity;
  request.providerIdentity = view.provider;
  request.requestId = view.requestId;
  request.attempt = view.attempt;
  request.planCoreDigest = view.planCoreDigest;
  request.modelManifestDigest = view.modelManifestDigest.empty() ?
    view.modelDigest : view.modelManifestDigest;
  request.protectionEpoch = view.protectionEpoch;
  request.artifactDigest = view.artifactDigest;
  request.expiresAtMs = view.expiresAtMs;
  auto grant = m_authority->issue(request, std::chrono::system_clock::now());
  if (std::chrono::system_clock::now() >= deadline) {
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: grant acquisition deadline expired");
  }
  const auto expectedName = canonicalNativeGrantName(
    m_requesterIdentity, view.provider, view.requestId, view.attempt,
    view.planCoreDigest, request.modelManifestDigest, view.protectionEpoch,
    grant.grantDigest);
  const auto publishedName = m_publish(expectedName, grant.wireJson);
  if (publishedName != expectedName) {
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: publication name mismatch");
  }
  return {view.provider, view.role, publishedName, grant.grantDigest,
          grant.recipient, grant.wireJson, grant.expiresAtMs};
}

} // namespace ndnsf::di
