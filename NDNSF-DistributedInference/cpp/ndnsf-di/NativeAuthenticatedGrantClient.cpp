#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace ndnsf::di {
namespace {
std::uint64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}
// ndn-cxx's DEFAULT_FRESHNESS_PERIOD is zero, while the Core APP-data
// primitive requires a positive cache lifetime. Grants are issued with a
// bounded lifetime by the authority, so keep the publication cache window
// at the one-minute native grant default.
constexpr auto kGrantFreshness = ndn::time::milliseconds(60000);
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
          ndn::Buffer(wire.begin(), wire.end()), kGrantFreshness).toUri();
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

NativeAuthenticatedGrantClient::Issue coreIssue(
  std::shared_ptr<ndn_service_framework::ServiceUser> user,
  std::string authorityIdentity,
  std::string authorityService)
{
  if (!user || authorityIdentity.empty() || authorityService.empty()) {
    throw std::invalid_argument("grant authority Core transport configuration is incomplete");
  }
  return [user = std::move(user), authorityIdentity = std::move(authorityIdentity),
          authorityService = std::move(authorityService)](
           const NativeSignedGrantRequest& request,
           const std::string& publishedManifestJson,
           std::uint64_t expiresAtMs,
           const NativeGrantControl& control) {
    control.check();
    if (user->isOnIoThread()) {
      throw std::logic_error("grant authority acquisition cannot wait on Core I/O thread");
    }
    struct Pending {
      std::mutex mutex;
      std::condition_variable changed;
      std::atomic<bool> abandoned{false};
      bool done = false;
      std::exception_ptr error;
      ndn_service_framework::ResponseMessage response;
    };
    auto pending = std::make_shared<Pending>();
    struct Abandon {
      std::shared_ptr<Pending> state;
      ~Abandon() { state->abandoned.store(true); }
    } abandon{pending};

    NativeGrantAuthorityRequest authorityRequest{request, expiresAtMs, publishedManifestJson};
    const auto wire = nativeGrantAuthorityRequestJson(authorityRequest);
    ndn_service_framework::RequestMessage message;
    ndn::Buffer payload(reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size());
    message.setPayload(payload, payload.size());
    const auto now = std::chrono::system_clock::now();
    if (now >= control.deadline) {
      control.check();
      throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: authority request deadline expired");
    }
    const auto boundedDeadline = std::min(control.deadline,
      now + std::chrono::milliseconds(60000));
    const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      boundedDeadline - now).count();
    const auto timeoutMs = static_cast<int>(std::max<std::int64_t>(1, remainingMs));
    const auto completeWithError = [](const std::shared_ptr<Pending>& state,
                                      std::exception_ptr error) {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->abandoned.load() || state->done) return;
      state->error = std::move(error);
      state->done = true;
      state->changed.notify_all();
    };
    const auto timeoutHandler = [pending, completeWithError](const ndn::Name&) {
      completeWithError(pending, std::make_exception_ptr(std::runtime_error(
        "DI_PROTECTED_GRANT_REJECTED: authority request timed out")));
    };
    const auto responseHandler = [pending](const ndn_service_framework::ResponseMessage& response) {
      std::lock_guard<std::mutex> lock(pending->mutex);
      if (pending->abandoned.load() || pending->done) return;
      pending->response = response;
      pending->done = true;
      pending->changed.notify_all();
    };

    // RequestServiceTargeted mutates ServiceUser's pending-call maps and must
    // run on the Face's existing IO context.  The native planner remains the
    // blocking worker: it waits on Pending while the Core owns admission,
    // publication, timeout and callback state.  Abandoning the wait before
    // dispatch makes the queued closure a no-op; abandoning after dispatch
    // leaves Core's own bounded timeout to erase its pending call, while late
    // callbacks are ignored by the shared state.
    user->postToIo([user, pending, authorityIdentity = std::move(authorityIdentity),
                    authorityService = std::move(authorityService), message = std::move(message),
                    timeoutMs, control, timeoutHandler, responseHandler,
                    completeWithError] () mutable {
      if (pending->abandoned.load()) return;
      try {
        control.check();
        const auto requestId = user->RequestServiceTargeted(
          ndn::Name(authorityIdentity), ndn::Name(authorityService), std::move(message),
          timeoutMs, timeoutHandler, responseHandler);
        if (requestId.empty()) {
          completeWithError(pending, std::make_exception_ptr(std::runtime_error(
            "DI_PROTECTED_GRANT_REJECTED: authority request was not admitted")));
          return;
        }
        std::lock_guard<std::mutex> lock(pending->mutex);
        if (pending->abandoned.load() || pending->done) return;
      }
      catch (...) {
        completeWithError(pending, std::current_exception());
      }
    });

    std::unique_lock<std::mutex> lock(pending->mutex);
    while (!pending->done) {
      control.check();
      pending->changed.wait_until(lock, std::min(control.deadline,
        std::chrono::system_clock::now() + std::chrono::milliseconds(10)));
    }
    control.check();
    if (pending->error) std::rethrow_exception(pending->error);
    if (!pending->response.getStatus()) {
      throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: authority rejected request: " +
                               pending->response.getErrorInfo());
    }
    const auto responsePayload = pending->response.getPayload();
    if (responsePayload.empty()) {
      throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: authority returned an empty grant");
    }
    return nativeKeyGrantFromJson(std::string(
      reinterpret_cast<const char*>(responsePayload.data()), responsePayload.size()));
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
    m_protectionEpoch(issuer ? issuer->protectionEpoch() : std::string{}),
    m_issue([issuer = std::move(issuer), issueClock = clock ? clock : Clock(nowMs)](
                                          const NativeSignedGrantRequest& request,
                                          const std::string& manifest,
                                          std::uint64_t expiresAtMs,
                                          const NativeGrantControl& control) {
      control.check();
      if (!issuer) throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: local issuer is missing");
      auto result = issuer->issue(request, issueClock(), expiresAtMs, manifest);
      control.check();
      return result;
    }),
    m_publish(std::move(publish)), m_clock(clock ? std::move(clock) : Clock(nowMs))
{
  if (m_requester.empty() || m_authority.empty() || m_requester == m_authority ||
      !m_requesterKey || EVP_PKEY_id(m_requesterKey.get()) != EVP_PKEY_ED25519 ||
      m_authorityPublicKey.size() != 32 || m_protectionEpoch.empty() || !m_issue || !m_publish)
    throw std::invalid_argument("authenticated grant client configuration is incomplete");
  detail::signNativeGrantBytes(*m_requesterKey, "requester-key-preflight");
}

NativeAuthenticatedGrantClient::NativeAuthenticatedGrantClient(
  std::string requester, std::shared_ptr<EVP_PKEY> requesterKey,
  std::string authority, std::string authorityPublicKeyRaw,
  std::string protectionEpoch, Issue issue, Publish publish, Clock clock)
  : m_requester(std::move(requester)), m_authority(std::move(authority)),
    m_authorityPublicKey(std::move(authorityPublicKeyRaw)),
    m_protectionEpoch(std::move(protectionEpoch)), m_requesterKey(std::move(requesterKey)),
    m_issue(std::move(issue)), m_publish(std::move(publish)),
    m_clock(clock ? std::move(clock) : Clock(nowMs))
{
  if (m_requester.empty() || m_authority.empty() || m_requester == m_authority ||
      !m_requesterKey || EVP_PKEY_id(m_requesterKey.get()) != EVP_PKEY_ED25519 ||
      m_authorityPublicKey.size() != 32 || m_protectionEpoch.empty() || !m_issue || !m_publish)
    throw std::invalid_argument("authenticated grant authority client configuration is incomplete");
  detail::signNativeGrantBytes(*m_requesterKey, "requester-key-preflight");
}

NativeAuthenticatedGrantClient::Issue
NativeAuthenticatedGrantClient::issueThroughCore(
  std::shared_ptr<ndn_service_framework::ServiceUser> user,
  std::string authorityIdentity,
  std::string authorityService)
{
  return coreIssue(std::move(user), std::move(authorityIdentity), std::move(authorityService));
}

NativeGrantBinding NativeAuthenticatedGrantClient::acquire(const NativePlacementPlanCore& core,
  const NativeAdmittedOfferV3& offer, const NativeSecurityPolicySnapshot& security,
  const NativeGrantControl& control, const std::string& role) const
{
  control.check();
  const auto view = NativePlanSealer::grantView(core, offer, security, role);
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
  const auto grant = m_issue(request, core.artifacts.canonicalManifestJson,
                             view.expiresAtMs, control);
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
