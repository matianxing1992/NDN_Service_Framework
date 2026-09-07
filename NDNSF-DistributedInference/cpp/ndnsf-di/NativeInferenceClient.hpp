#ifndef NDNSF_DI_NATIVE_INFERENCE_CLIENT_HPP
#define NDNSF_DI_NATIVE_INFERENCE_CLIENT_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

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
class NativeConversationCoordinator;
class NativeRequestPreparation;
class NativeOfferAdmission;

struct NativeModelRef : NativeModelDescriptor
{
  std::string sourceRevision;
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

// CD-001 non-public test port (test-only publication boundary): injects the
// steady clock that derives operation deadlines and optionally replaces the
// worker-thread dispatch with an explicit hook a unit test pumps, so
// lifecycle ordering is deterministic.  Production callers use the
// constructor without a test port; the default clock is steady_clock::now and
// dispatch runs on the client-owned serial executor.
struct NativeClientTestPort
{
  std::function<std::chrono::steady_clock::time_point()> now;
  // When set, every request dispatch is handed to this function instead of
  // the worker thread (no thread is started).  A test queues the functions
  // and runs each one explicitly for deterministic ordering.
  std::function<void(std::function<void()>)> submitHook;
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
  NativeInferenceClient(
    const NativeClientTestPort& testPort,
    std::shared_ptr<ndn_service_framework::ServiceUser> user,
    std::shared_ptr<const NativeAdapterRegistry> adapters,
    std::shared_ptr<NativeGrantClient> grants = nullptr,
    std::shared_ptr<NativeConversationCoordinator> conversations = nullptr,
    std::shared_ptr<NativeRequestPreparation> preparation = nullptr,
    std::shared_ptr<const NativeOfferAdmission> admission = nullptr);
  ~NativeInferenceClient() noexcept;

  NativeInferenceHandle request(
    const NativeModelRef& model,
    const NativeApplicationInput& input,
    std::shared_ptr<const NativeModelSplitStrategy> splitStrategy,
    std::shared_ptr<const NativePlacementStrategy> placementStrategy,
    const NativeRequestOptions& options);
  void close() noexcept;

private:
  std::shared_ptr<ndn_service_framework::ServiceUser> m_user;
  std::shared_ptr<const NativeAdapterRegistry> m_adapters;
  std::shared_ptr<NativeGrantClient> m_grants;
  std::shared_ptr<NativeConversationCoordinator> m_conversations;
  std::shared_ptr<NativeRequestPreparation> m_preparation;
  std::shared_ptr<const NativeOfferAdmission> m_admission;
  std::function<std::chrono::steady_clock::time_point()> m_now;
  std::shared_ptr<SerialRequestExecutor> m_executor;
  std::shared_ptr<SerialRequestExecutor> m_notifications;
  mutable std::mutex m_mutex;
  std::vector<std::weak_ptr<NativeInferenceHandle::Operation>> m_operations;
  bool m_closed = false;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_INFERENCE_CLIENT_HPP
