#ifndef NDNSF_DI_NATIVE_INFERENCE_PROVIDER_HPP
#define NDNSF_DI_NATIVE_INFERENCE_PROVIDER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionLeaseService.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

// Internal host singleton state. Its definition lives in
// NativeInferenceProvider.cpp; it is opaque to callers and reached only
// through NativeInferenceProvider members.
class HostState;
class NativeLocalWithdrawalState;
class NativeWithdrawalCommitGate;
class NativeWithdrawalWireBudget;

enum class NativeWithdrawalFailurePolicyV1 { Abort, ForceUnsaved };
enum class NativeWithdrawalStatusV1 {
  Quiescing, AwaitingOwners, Saving, Confirmed, Closed, Aborted,
  Failed, ForcedUnsaved
};

struct NativeWithdrawalOptionsV1
{
  std::chrono::steady_clock::time_point deadline;
  std::chrono::steady_clock::time_point drainDeadline;
  NativeWithdrawalFailurePolicyV1 failurePolicy = NativeWithdrawalFailurePolicyV1::Abort;
  std::size_t maxConversations = 0;
  std::uint64_t maxTotalBytes = 0;
  std::string persistencePolicyId;
};

struct NativeWithdrawalConversationResultV1
{
  std::string ownerIdentity, serviceName, conversationId, checkpointDigest;
  std::string operationId, classification, reason;
};

struct NativeWithdrawalResultV1
{
  std::string withdrawalId;
  NativeWithdrawalScopeV1 scope = NativeWithdrawalScopeV1::Service;
  bool closed = false;
  bool forced = false;
  std::string reason;
  std::vector<NativeWithdrawalConversationResultV1> conversationResults;
};

/** Local operator handle. result() may block and must be called away from the
 * Provider Face and withdrawal worker threads. A wait timeout leaves the
 * operation running and never implies ForceUnsaved. */
class NativeWithdrawalHandle
{
public:
  NativeWithdrawalHandle() noexcept = default;
  bool valid() const noexcept;
  const std::string& operationId() const noexcept;
  NativeWithdrawalStatusV1 status() const;
  NativeWithdrawalResultV1 result(std::chrono::milliseconds waitTimeout) const;
  bool cancel() noexcept;
private:
  explicit NativeWithdrawalHandle(std::shared_ptr<NativeLocalWithdrawalState> state);
  std::shared_ptr<NativeLocalWithdrawalState> m_state;
  friend class NativeServiceRegistration;
  friend class NativeInferenceProvider;
};

// Native service definition consumed by NativeInferenceProvider::serve.
// serviceName/allowedRoles come from a verified native plan; ackHandler
// preserves the caller's existing offer/provisioning/readiness ACK decision
// (empty means the host accepts by default).
struct NativeServiceDefinition
{
  using AsyncTargetedRequestHandlerFactory =
    std::function<ndn_service_framework::ServiceProvider::AsyncTargetedRequestHandler(
      const NativeProviderCollaborationRuntime&,
      NativeConversationHandoffOwnerContextResolverV1)>;

  std::string serviceName;
  std::vector<std::string> allowedRoles;
  // Optional per-service ACK strategy. Empty means the host accepts the
  // collaboration ACK by default; the Core admission/permission gates stay
  // authoritative either way. With a handoffOwnerContextResolver, a canonical
  // targeted-bootstrap handoff control receives an internal positive ACK so
  // the model-offer callback does not interpret the control payload as an
  // inference request; Core still performs targeted selection and authorization.
  ndn_service_framework::ServiceProvider::AckStrategyHandler ackHandler;
  // Async variant for request-scoped telemetry or other bounded ACK work.
  // Core supplies authenticated outer identity, registration generation,
  // cancellation, and a non-extendable local deadline.
  ndn_service_framework::ServiceProvider::AsyncAckStrategyHandler asyncAckHandler;
  // Optional authenticated targeted control requests (for example an
  // explicit conversation handoff request) share this collaboration
  // registration's permission and lifetime generation. The factory receives
  // this service's actual runtime so event callbacks can retain its Store and
  // save-batch operations. Do not install a second targeted registration for
  // the same inference service.
  AsyncTargetedRequestHandlerFactory asyncTargetedRequestHandlerFactory;
  // The factory may use this resolver for authenticated handoff events. It is
  // invoked per event with Core's requester/service identities and the
  // immutable operation ID; it must verify the Owner certificate against the
  // Provider's configured trust profile and return that call's storage owner,
  // original packet store, bounded publication policy, and per-role Repo
  // route context. The Owner still checks its pinned Repo trust policy before
  // issuing any grant. The factory
  // receives the function object by value so an async handler can own it;
  // state captured by reference inside the callable must still outlive its use.
  NativeConversationHandoffOwnerContextResolverV1 handoffOwnerContextResolver;
  // Local orderly withdrawal is disabled unless this operator-supplied ID is
  // configured together with the authenticated Owner context resolver.
  // withdraw() must present this exact ID; it is never taken from wire input.
  std::string withdrawalPersistencePolicyId;
  // Optional observation seam invoked once after the native runtime is
  // assembled and before its collaboration registration is installed. The
  // caller may capture runtime capacity/evidence snapshots for readiness or
  // telemetry; it must not mutate the runtime or replace its handler.
  std::function<void(const NativeProviderCollaborationRuntime&)> runtimeObserver;
};

/** Build the bounded async handler for the independent network probe service.
 * The caller owns a Core scoped targeted registration and keeps provider alive
 * until that registration and its callbacks have drained. Probe permission is
 * independent of the inference service carried in the request scope. */
ndn_service_framework::ServiceProvider::AsyncTargetedRequestHandler
makeNativePlacementMeasurementTargetedHandler(
  ndn_service_framework::ServiceProvider& provider);

// Move-only per-service registration handle. close() is idempotent: it first
// raises the target's draining fence (the fixed lease entry rejects new
// Prepare/Commit/Renew while Abort/Release keep routing), then closes the
// Core scoped registration (new ACKs/Selections/queued executions become
// invalid immediately; the entry is retired by Core on the serialized Face
// path). For handoff work, close() requests cancellation but does not wait.
// Call drain() from a non-Face, non-handoff-worker thread while the Provider's
// Face is still running and before destroying the Provider/Face. drain() waits
// for accepted handoff work and its callbacks to settle; false means the
// timeout elapsed and the caller may retry. The destructor only closes.
class NativeServiceRegistration
{
public:
  NativeServiceRegistration() noexcept = default;
  NativeServiceRegistration(NativeServiceRegistration&& other) noexcept = default;
  NativeServiceRegistration& operator=(NativeServiceRegistration&& other) noexcept =
    default;
  NativeServiceRegistration(const NativeServiceRegistration&) = delete;
  NativeServiceRegistration& operator=(const NativeServiceRegistration&) = delete;
  ~NativeServiceRegistration() noexcept;

  NativeWithdrawalHandle withdraw(const NativeWithdrawalOptionsV1& options);
  void close() noexcept;
  bool drain(std::chrono::milliseconds timeout);
  bool closed() const noexcept;
  bool valid() const noexcept;
  std::uint64_t generation() const noexcept;
  const std::string& serviceName() const noexcept;

private:
  struct State;
  explicit NativeServiceRegistration(std::shared_ptr<State> state,
                                     bool closeOnDestroy = true);
  NativeWithdrawalHandle withdrawPrepared(
    const NativeWithdrawalOptionsV1& options,
    std::optional<ConversationStateStore::SaveFence> preparedFence,
    std::string preparedId,
    std::shared_ptr<NativeWithdrawalCommitGate> parentCommitGate = {},
    std::shared_ptr<NativeWithdrawalWireBudget> parentWireBudget = {});
  std::shared_ptr<State> m_state;
  bool m_closeOnDestroy = true;
  friend class NativeInferenceProvider;
};

// Least-authority shared native Provider host (CD-014). The first serve()
// creates the host-wide shared lease state and registers the single fixed
// lease entry; every later serve() assembles that target's runtime through
// NativeProviderHandlerConfig, routes it to an ExecutionLeaseService over the
// shared state, and installs the scoped collaboration registration. serve()
// The config's localProviderName must be the exact identity returned by the
// underlying ServiceProvider, and providerBootId must be non-empty; serve()
// rejects an unbound identity before creating host-wide lease state. This
// prevents a small in-process fixture from masking cross-host offer,
// evidence, data-prefix, or lease binding errors.
// serve() may only be called on the Face event thread or before the event loop
// starts, the same constraint as the Core scoped registration APIs; close()
// and stop() may be triggered from any thread. The host never closes the
// shared ServiceProvider/Face and never joins an I/O thread.
class NativeInferenceProvider
{
public:
  NativeInferenceProvider(
    std::shared_ptr<ndn_service_framework::ServiceProvider> provider,
    std::shared_ptr<const NativeAdapterRegistry> adapters);
  ~NativeInferenceProvider() noexcept;
  NativeInferenceProvider(const NativeInferenceProvider&) = delete;
  NativeInferenceProvider& operator=(const NativeInferenceProvider&) = delete;
  NativeInferenceProvider(NativeInferenceProvider&&) = delete;
  NativeInferenceProvider& operator=(NativeInferenceProvider&&) = delete;

  NativeServiceRegistration serve(const NativeServiceDefinition& service,
                                  const NativeProviderHandlerConfig& config);
  NativeWithdrawalHandle withdraw(const NativeWithdrawalOptionsV1& options);
  void stop() noexcept;
  bool drain(std::chrono::milliseconds timeout);

private:
  std::shared_ptr<ndn_service_framework::ServiceProvider> m_provider;
  std::shared_ptr<const NativeAdapterRegistry> m_adapters;
  std::mutex m_mutex;
  bool m_stopped = false;
  std::shared_ptr<HostState> m_host;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_INFERENCE_PROVIDER_HPP
