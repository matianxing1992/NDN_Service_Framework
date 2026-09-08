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
