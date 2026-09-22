#ifndef NDNSF_DI_NATIVE_INFERENCE_CLIENT_HPP
#define NDNSF_DI_NATIVE_INFERENCE_CLIENT_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "ndn-service-framework/InvocationStream.hpp"
#include "ndn-service-framework/OperationState.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndn_service_framework { class ServiceUser; }

namespace ndnsf::di {

struct NativeOperationRegistry;
struct NativeIoCleanupState;

class Runtime;
class NativeGrantClient;
class NativeRequestPreparation;
class NativeOfferAdmission;
struct NativeRequestRuntime;
struct NativeStrategyPorts;

struct NativeModelRef : NativeModelDescriptor
{
};

enum class NativeInputTransportMode { Inline, RepositoryReference };

struct NativeApplicationInput
{
  std::string taskName;
  std::string inputSchemaDigest;
  std::string optionsSchemaDigest;
  std::vector<std::uint8_t> payload;
  std::vector<std::uint8_t> options;
  NativeInputTransportMode transportMode = NativeInputTransportMode::Inline;
  std::string repositoryReference;
};

struct NativeRequestOptions
{
  std::uint64_t timeoutMs = 30'000;
  std::uint64_t ackTimeoutMs = 5'000;
  std::string taskName;
  std::string outputMode = "FULL";
  // Optional Provider identities for a maintained native caller. An empty
  // list keeps Core's discovery selection; a non-empty list pins the request
  // to these candidates without moving protocol logic into the caller.
  std::vector<ndn::Name> providerNames;
  // Optional caller correlation only. The native owner still allocates the
  // authoritative Core requestId; this value is carried locally so a
  // maintained caller can prove its wire request maps to that owner.
  std::string applicationRequestId;
  std::optional<NativeGenerationExecutionContractV1> generation;
  std::optional<ndn_service_framework::StreamRequestOptions> stream;
  // A conversation turn is owned by the native coordinator. The caller only
  // supplies the authenticated parent continuation; request/attempt/epoch
  // identities are allocated and fenced by the requester. The current
  // requestContractDigest may be empty because it covers the owner-allocated
  // request ID; the requester fills it from the encoded envelope.
  std::optional<NativeConversationContinuation> conversation;
  // Runs after process-local acceptance on the request worker. An exception
  // fails the request without rolling back accepted tokens or replaying them.
  // This is distinct from the non-authoritative handle.observe() callback.
  std::function<void(const std::vector<std::uint8_t>&)> onGenerationEvent;
};

/** Pinned service/task metadata supplied by the application's catalog owner.
 * Composition/task identities cannot be inferred from a model content hash. */
struct NativeRequestContract
{
  std::string serviceName;
  std::string taskName;
  std::string adapterName;
  std::string adapterDescriptorDigest;
  std::string adapterCompositionDigest;
  std::string taskDescriptorDigest;
  std::string generationMode = "TOKEN_DIAGNOSTIC";
  // Operator-pinned tokenizer identity.  The native requester owns this
  // value and compares every derived generation contract against it; model
  // semantics and automatic-planner metadata are never substitutes.
  std::string tokenizerDigest;
};

enum class NativeRequestStatus { Pending, Succeeded, Failed, Cancelled };

struct NativeInferenceResult
{
  std::vector<std::uint8_t> payload;
  std::string modelDigest;
  std::string planDigest;
};

struct NativeInferenceEvent
{
  std::string requestId;
  std::vector<std::uint8_t> payload;
  bool terminal = false;
  std::uint64_t sequence = 0;
};

struct NativeInferenceDiagnostics
{
  std::uint64_t observationDropped = 0;
};

using NativeEventReader = ndn_service_framework::OperationReader<NativeInferenceEvent>;
using NativeOperationSubscription = ndn_service_framework::OperationSubscription;

class NativeDiError : public std::runtime_error
{
public:
  NativeDiError(std::string code, std::string domain, std::string boundary,
                std::string message, std::string requestId = {},
                std::uint64_t attempt = 0);

  const std::string& code() const noexcept { return m_code; }
  const std::string& domain() const noexcept { return m_domain; }
  const std::string& boundary() const noexcept { return m_boundary; }
  const std::string& requestId() const noexcept { return m_requestId; }
  std::uint64_t attempt() const noexcept { return m_attempt; }

private:
  std::string m_code;
  std::string m_domain;
  std::string m_boundary;
  std::string m_requestId;
  std::uint64_t m_attempt = 0;
};

class NativeInferenceHandle
{
public:
  NativeInferenceHandle() = default;
  std::string requestId() const;
  std::string applicationRequestId() const;
  /** Return the absolute native submission deadline for owner-side waiters. */
  std::chrono::steady_clock::time_point deadline() const;
  // Returns the authenticated checkpoint committed by the native conversation
  // owner, if this request completed a conversation turn.  An ordinary
  // request, an in-flight turn, or a failed/cancelled turn has no checkpoint.
  std::optional<std::string> conversationCheckpoint() const;
  NativeRequestStatus status() const;
  NativeInferenceResult result(std::chrono::milliseconds waitTimeout) const;
  NativeInferenceDiagnostics diagnostics() const;
  void cancel();
  /** Retain an owner (used by PreparedModel for its package lease) through
   * terminal cleanup even when the public handle is released. */
  void retain(std::shared_ptr<void> owner);
  void observe(std::function<void(const NativeInferenceEvent&)> observer);
  NativeOperationSubscription observeSubscription(
    std::function<void(const NativeInferenceEvent&)> observer);
  NativeOperationSubscription onCompletion(
    std::function<void(std::exception_ptr, std::optional<NativeInferenceResult>)> callback);
  NativeOperationSubscription resultAsync(
    std::chrono::milliseconds timeout,
    std::function<void(std::optional<NativeInferenceResult>, std::exception_ptr)> callback);
  NativeEventReader events() const;

public:
  struct Operation;
private:
  explicit NativeInferenceHandle(std::shared_ptr<Operation> operation);
  std::shared_ptr<Operation> m_operation;
  friend class NativeInferenceClient;
};

class NativeInferenceClient
{
public:
  NativeInferenceClient(
    std::shared_ptr<ndn_service_framework::ServiceUser> user,
    std::shared_ptr<const NativeAdapterRegistry> adapters,
    std::shared_ptr<NativeGrantClient> grants = nullptr,
    std::shared_ptr<NativeConversationCoordinator> conversations = nullptr,
    std::shared_ptr<NativeRequestPreparation> preparation = nullptr,
    std::shared_ptr<const NativeOfferAdmission> admission = nullptr);
  ~NativeInferenceClient() noexcept;

  NativeInferenceClient(
    std::shared_ptr<ndn_service_framework::ServiceUser> user,
    std::shared_ptr<const NativeAdapterRegistry> adapters,
    const NativeRequestContract& contract,
    std::shared_ptr<NativeRequestPreparation> preparation,
    std::shared_ptr<const NativeOfferAdmission> admission);

  NativeInferenceClient(
    std::shared_ptr<ndn_service_framework::ServiceUser> user,
    std::shared_ptr<const NativeAdapterRegistry> adapters,
    const NativeRequestRuntime& runtime,
    std::shared_ptr<NativeRequestPreparation> preparation,
    std::shared_ptr<const NativeOfferAdmission> admission);

  // Conversation requests use the same protected runtime as ordinary
  // requests plus the native coordinator that owns parent/checkpoint state.
  // Keep the five-argument runtime constructor above for non-conversation
  // callers and compatibility tests.
  NativeInferenceClient(
    std::shared_ptr<ndn_service_framework::ServiceUser> user,
    std::shared_ptr<const NativeAdapterRegistry> adapters,
    const NativeRequestRuntime& runtime,
    std::shared_ptr<NativeConversationCoordinator> conversations,
    std::shared_ptr<NativeRequestPreparation> preparation,
    std::shared_ptr<const NativeOfferAdmission> admission);

  NativeInferenceHandle request(
    const NativeModelRef& model,
    const NativeApplicationInput& input,
    std::shared_ptr<const NativeModelSplitStrategy> splitStrategy,
    std::shared_ptr<const NativePlacementStrategy> placementStrategy,
    const NativeRequestOptions& options);

  /** Submit through the cooperative strategy ports used by PreparedModel.
   * The operation owner and Core request path are identical to request();
   * only the strategy extension contract differs. */
  NativeInferenceHandle requestCooperative(
    const NativeModelRef& model,
    const NativeApplicationInput& input,
    std::shared_ptr<const CooperativeModelSplitStrategy> splitStrategy,
    std::shared_ptr<const CooperativePlacementStrategy> placementStrategy,
    const NativeRequestOptions& options);
  void close() noexcept;

  /** Close must be followed by this bounded join when the enclosing Runtime
   * promises that no requester callback can outlive its Core owner. */
  bool drain(std::chrono::milliseconds timeout);

  /** Notify an enclosing lifecycle owner whenever this client's work settles. */
  void setDrainNotifier(std::function<void()> notifier);

  /** Return whether this client's private operation runtime is quiescent. */
  bool isQuiescent() const noexcept;
  /** Return whether the caller is already on the Core/operation worker. */
  bool isWorkerThread() const noexcept;
  /** Return the Runtime-owned conversation coordinator, if configured. */
  std::shared_ptr<NativeConversationCoordinator> conversationCoordinator() const noexcept;
  /** Retain the enclosing transport owner until this client and its
   * operations are fully destroyed. */
  void retainOwner(std::shared_ptr<void> owner);

private:
  // Runtime uses this only from its owned Face exception boundary.  It
  // delivers a stable native failure to active requests and performs their
  // Core cancellation while postToIo can still execute on that Face.
  void failIo(const std::string& reason) noexcept;

  // Only the unit-test friend can replace clocks/dispatch. Production
  // construction always uses a monotonic clock and an independent timer.
  friend class NativeClientTestAccess;
  friend class Runtime;
  struct TestPort {
    std::function<std::chrono::steady_clock::time_point()> now;
    std::function<void(std::function<void()>)> submitHook;
    std::function<std::function<void()>(std::chrono::steady_clock::time_point,
                                       std::function<void()>)> scheduleHook;
  };
  NativeInferenceClient(
    const TestPort& testPort,
    std::shared_ptr<ndn_service_framework::ServiceUser> user,
    std::shared_ptr<const NativeAdapterRegistry> adapters,
    std::shared_ptr<NativeGrantClient> grants = nullptr,
    std::shared_ptr<NativeConversationCoordinator> conversations = nullptr,
    std::shared_ptr<NativeRequestPreparation> preparation = nullptr,
    std::shared_ptr<const NativeOfferAdmission> admission = nullptr);

  NativeInferenceHandle requestImpl(
    const NativeModelRef& model,
    const NativeApplicationInput& input,
    NativeStrategyPorts strategies,
    std::shared_ptr<const NativeModelSplitStrategy> legacySplitter,
    std::shared_ptr<const NativePlacementStrategy> legacyPlacement,
    std::shared_ptr<const CooperativeModelSplitStrategy> cooperativeSplitter,
    std::shared_ptr<const CooperativePlacementStrategy> cooperativePlacement,
    const NativeRequestOptions& options);

  std::shared_ptr<ndn_service_framework::ServiceUser> m_user;
  std::shared_ptr<void> m_ownerLease;
  std::shared_ptr<const NativeAdapterRegistry> m_adapters;
  std::shared_ptr<NativeGrantClient> m_grants;
  std::shared_ptr<NativeConversationCoordinator> m_conversations;
  std::shared_ptr<NativeRequestPreparation> m_preparation;
  std::shared_ptr<const NativeOfferAdmission> m_admission;
  std::shared_ptr<const NativeRequestContract> m_requestContract;
  std::shared_ptr<const NativeRequestRuntime> m_runtime;
  std::shared_ptr<ndn_service_framework::OperationRuntime> m_operationRuntime;
  // Production request names include a fresh owner scope per native client,
  // so separately launched or forked requester owners cannot reuse a cached
  // process scope. The private test port keeps deterministic legacy IDs for
  // unit assertions.
  std::string m_requestOwnerScope;
  std::function<std::chrono::steady_clock::time_point()> m_now;
  std::shared_ptr<NativeOperationRegistry> m_operationRegistry;
  // Core cleanup callbacks posted to the Face remain part of this client's
  // lifecycle until their task has actually run; Runtime stop fences use the
  // count to avoid truncating a close-produced cancellation.
  std::shared_ptr<NativeIoCleanupState> m_ioCleanupState;
  std::function<std::function<void()>(std::chrono::steady_clock::time_point,
                                     std::function<void()>)> m_schedule;
  mutable std::mutex m_mutex;
  std::vector<std::weak_ptr<NativeInferenceHandle::Operation>> m_operations;
  // Keep closed operations alive until the explicit drain boundary can emit
  // one terminal lifecycle observation for each request.
  std::vector<std::shared_ptr<NativeInferenceHandle::Operation>> m_closedOperations;
  bool m_closed = false;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_INFERENCE_CLIENT_HPP
