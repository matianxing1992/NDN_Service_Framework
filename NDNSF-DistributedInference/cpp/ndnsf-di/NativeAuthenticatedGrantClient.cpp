#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include <openssl/evp.h>
#include <condition_variable>
#include <mutex>

namespace ndnsf::di {
namespace {
std::uint64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}
NativeAuthenticatedGrantClient::Publish corePublisher(
  std::shared_ptr<ndn_service_framework::ServiceUser> user)
{
  if (!user) throw std::invalid_argument("grant Core owner is missing");
  return [user = std::move(user)](const std::string& name, const std::string& wire,
                                 const NativeGrantControl& control) {
    control.check();
    if (user->isOnIoThread()) throw std::logic_error("grant acquisition cannot wait on Core I/O thread");
    struct Pending {
      std::mutex mutex;
      std::condition_variable changed;
      std::atomic<bool> abandoned{false};
      bool done = false;
      std::string name;
      std::exception_ptr error;
    };
    auto pending = std::make_shared<Pending>();
    struct Abandon {
      std::shared_ptr<Pending> state;
      ~Abandon() { state->abandoned.store(true); }
    } abandon{pending};
    user->postToIo([user, pending, name, wire, control] {
      if (pending->abandoned.load()) return;
      std::string published;
      std::exception_ptr error;
      try {
        control.check();
        published = user->publishSignedAppData(ndn::Name(name),
          ndn::Buffer(wire.begin(), wire.end())).toUri();
      } catch (...) { error = std::current_exception(); }
      std::lock_guard<std::mutex> lock(pending->mutex);
      if (pending->abandoned.load()) return;
      pending->name = std::move(published); pending->error = error; pending->done = true;
      pending->changed.notify_all();
    });
    std::unique_lock<std::mutex> lock(pending->mutex);
    while (!pending->done) {
      control.check();
      pending->changed.wait_until(lock, std::min(control.deadline,
        std::chrono::system_clock::now() + std::chrono::milliseconds(10)));
    }
    control.check();
    if (pending->error) std::rethrow_exception(pending->error);
    return pending->name;
  };
}
}

NativeAuthenticatedGrantClient::Publish NativeAuthenticatedGrantClient::publishThroughCore(
  std::shared_ptr<ndn_service_framework::ServiceUser> user)
{
  return corePublisher(std::move(user));
}

void NativeGrantControl::check() const
{
  if ((cancelled && cancelled->load()) || std::chrono::system_clock::now() >= deadline)
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: grant cancelled or deadline expired");
}

NativeAuthenticatedGrantClient::NativeAuthenticatedGrantClient(std::string requester,
  std::shared_ptr<EVP_PKEY> requesterKey, std::string authority, std::string authorityPublicKeyRaw,
  std::shared_ptr<const NativeArtifactGrantIssuer> issuer,
  std::shared_ptr<ndn_service_framework::ServiceUser> user)
  : NativeAuthenticatedGrantClient(std::move(requester), std::move(requesterKey),
      std::move(authority), std::move(authorityPublicKeyRaw), std::move(issuer), corePublisher(std::move(user)))
{}

NativeAuthenticatedGrantClient::NativeAuthenticatedGrantClient(std::string requester,
  std::shared_ptr<EVP_PKEY> requesterKey, std::string authority, std::string authorityPublicKeyRaw,
  std::shared_ptr<const NativeArtifactGrantIssuer> issuer, Publish publish, Clock clock)
  : m_requester(std::move(requester)), m_authority(std::move(authority)),
    m_authorityPublicKey(std::move(authorityPublicKeyRaw)), m_requesterKey(std::move(requesterKey)),
    m_issuer(std::move(issuer)), m_publish(std::move(publish)), m_clock(clock ? std::move(clock) : Clock(nowMs))
{
  if (m_requester.empty() || m_authority.empty() || m_requester == m_authority ||
      !m_requesterKey || EVP_PKEY_id(m_requesterKey.get()) != EVP_PKEY_ED25519 ||
      m_authorityPublicKey.size() != 32 || !m_issuer || !m_publish)
    throw std::invalid_argument("authenticated grant client configuration is incomplete");
  detail::signNativeGrantBytes(*m_requesterKey, "requester-key-preflight");
}

NativeGrantBinding NativeAuthenticatedGrantClient::acquire(const NativePlacementPlanCore& core,
  const NativeAdmittedOfferV3& offer, const NativeSecurityPolicySnapshot& security,
  const NativeGrantControl& control) const
{
  control.check();
  const auto view = NativePlanSealer::grantView(core, offer, security);
  if (view.requesterIdentity != m_requester || view.protectionEpoch == "plaintext-v1" ||
      !security.requireProtectedArtifacts || view.modelManifestDigest.empty())
    throw std::invalid_argument("authenticated grant view does not match requester or protection policy");
  NativeSignedGrantRequest request;
  request.requesterIdentity = m_requester; request.providerIdentity = view.provider;
  request.requestId = view.requestId; request.attempt = view.attempt;
  request.planCoreDigest = view.planCoreDigest; request.modelManifestDigest = view.modelManifestDigest;
  request.protectionEpoch = view.protectionEpoch; request.issuedAtMs = m_clock();
  const auto roleDigest = nativePlanningDigest(nativeCanonicalJson(nativeAssemblyJson(core.assemblyByRole.at(view.role))));
  request.grantViewDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"provider", view.provider}, {"request_id", view.requestId}, {"attempt", view.attempt},
    {"plan_core_digest", view.planCoreDigest}, {"offer_digest", core.offerDigestByProvider.at(view.provider)},
    {"role_digests", NativeJson::array({roleDigest})}, {"security_policy_snapshot_digest", view.policyDigest},
    {"model_manifest_digest", view.modelManifestDigest}, {"protection_epoch", view.protectionEpoch}}));
  request = request.sign(*m_requesterKey);
  control.check();
  const auto grant = m_issuer->issue(request, m_clock(), view.expiresAtMs);
  control.check();
  detail::verifyNativeIssuedGrant(grant, request, m_authority, m_authorityPublicKey,
    m_clock(), view.expiresAtMs);
  control.check();
  const auto name = m_publish(grant.grantName, grant.wireJson, control);
  control.check();
  if (name != grant.grantName)
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: publication name mismatch");
  return {view.provider, view.role, name, grant.grantDigest, grant.recipient, grant.wireJson, grant.expiresAtMs};
}
} // namespace ndnsf::di
