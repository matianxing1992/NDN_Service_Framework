#ifndef NDNSF_DI_NATIVE_INFERENCE_CLIENT_HPP
#define NDNSF_DI_NATIVE_INFERENCE_CLIENT_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "ndn-service-framework/InvocationStream.hpp"

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

class NativeGrantClient;
class NativeRequestPreparation;
class NativeOfferAdmission;
struct NativeRequestRuntime;

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
};

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
  NativeRequestStatus status() const;
  NativeInferenceResult result(std::chrono::milliseconds waitTimeout) const;
  void cancel();
  void observe(std::function<void(const NativeInferenceEvent&)> observer);

public:
  struct Operation;
private:
  explicit NativeInferenceHandle(std::shared_ptr<Operation> operation);
  std::shared_ptr<Operation> m_operation;
  friend class NativeInferenceClient;
};

class SerialRequestExecutor; // internal serial executor, defined in the .cpp

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
  void close() noexcept;

private:
  // Only the unit-test friend can replace clocks/dispatch. Production
  // construction always uses a monotonic clock and an independent timer.
  friend class NativeClientTestAccess;
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

  std::shared_ptr<ndn_service_framework::ServiceUser> m_user;
  std::shared_ptr<const NativeAdapterRegistry> m_adapters;
  std::shared_ptr<NativeGrantClient> m_grants;
  std::shared_ptr<NativeConversationCoordinator> m_conversations;
  std::shared_ptr<NativeRequestPreparation> m_preparation;
  std::shared_ptr<const NativeOfferAdmission> m_admission;
  std::shared_ptr<const NativeRequestContract> m_requestContract;
  std::shared_ptr<const NativeRequestRuntime> m_runtime;
  std::function<std::chrono::steady_clock::time_point()> m_now;
  std::shared_ptr<SerialRequestExecutor> m_executor;
  std::shared_ptr<SerialRequestExecutor> m_notifications;
  std::shared_ptr<SerialRequestExecutor> m_deadlines;
  std::function<std::function<void()>(std::chrono::steady_clock::time_point,
                                     std::function<void()>)> m_schedule;
  mutable std::mutex m_mutex;
  std::vector<std::weak_ptr<NativeInferenceHandle::Operation>> m_operations;
  bool m_closed = false;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_INFERENCE_CLIENT_HPP
