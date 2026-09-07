#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"

#include <atomic>
#include <chrono>
#include <utility>

namespace ndnsf::di {

namespace {
std::atomic<std::uint64_t> NEXT_REQUEST_ID{1};
} // namespace

struct NativeInferenceHandle::Operation
{
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::string requestId;
  NativeRequestStatus status = NativeRequestStatus::Pending;
  NativeInferenceResult result;
  std::shared_ptr<NativeDiError> error;
  std::vector<std::function<void(const NativeInferenceEvent&)>> observers;
};

namespace {
NativeDiError makeError(const NativeInferenceHandle::Operation& operation)
{
  return NativeDiError(operation.error ? operation.error->code() : "NATIVE_REQUEST_FAILED",
                       operation.error ? operation.error->domain() : "runtime",
                       operation.error ? operation.error->boundary() : "request",
                       operation.error ? operation.error->what() : "native request failed",
                       operation.requestId, 1);
}
} // namespace

NativeDiError::NativeDiError(std::string code, std::string domain,
                             std::string boundary, std::string message,
                             std::string requestId, std::uint64_t attempt)
  : std::runtime_error(std::move(message))
  , m_code(std::move(code))
  , m_domain(std::move(domain))
  , m_boundary(std::move(boundary))
  , m_requestId(std::move(requestId))
  , m_attempt(attempt)
{
  if (m_code.empty() || m_domain.empty() || m_boundary.empty()) {
    throw std::invalid_argument("native DI error identity is incomplete");
  }
}

NativeInferenceHandle::NativeInferenceHandle(std::shared_ptr<Operation> operation)
  : m_operation(std::move(operation))
{
}

std::string NativeInferenceHandle::requestId() const
{
  if (!m_operation) return {};
  std::lock_guard<std::mutex> lock(m_operation->mutex);
  return m_operation->requestId;
}

NativeRequestStatus NativeInferenceHandle::status() const
{
  if (!m_operation) throw NativeDiError("INVALID_HANDLE", "local", "handle",
                                         "native inference handle is empty");
  std::lock_guard<std::mutex> lock(m_operation->mutex);
  return m_operation->status;
}

NativeInferenceResult
NativeInferenceHandle::result(std::chrono::milliseconds waitTimeout) const
{
  if (!m_operation) throw NativeDiError("INVALID_HANDLE", "local", "handle",
                                         "native inference handle is empty");
  std::unique_lock<std::mutex> lock(m_operation->mutex);
  if (!m_operation->condition.wait_for(lock, waitTimeout, [this] {
        return m_operation->status != NativeRequestStatus::Pending;
      })) {
    throw NativeDiError("LOCAL_WAIT_TIMEOUT", "local", "wait",
                        "native result wait timed out", m_operation->requestId, 1);
  }
  if (m_operation->status == NativeRequestStatus::Succeeded) {
    return m_operation->result;
  }
  if (m_operation->status == NativeRequestStatus::Cancelled) {
    throw NativeDiError("CANCELLED", "local", "request",
                        "native request was cancelled", m_operation->requestId, 1);
  }
  throw makeError(*m_operation);
}

void NativeInferenceHandle::cancel()
{
  if (!m_operation) return;
  std::vector<std::function<void(const NativeInferenceEvent&)>> observers;
  NativeInferenceEvent event;
  {
    std::lock_guard<std::mutex> lock(m_operation->mutex);
    if (m_operation->status != NativeRequestStatus::Pending) return;
    m_operation->status = NativeRequestStatus::Cancelled;
    event.requestId = m_operation->requestId;
    event.terminal = true;
    observers = m_operation->observers;
  }
  m_operation->condition.notify_all();
  for (const auto& observer : observers) {
    try { observer(event); } catch (...) { }
  }
}

void NativeInferenceHandle::observe(
  std::function<void(const NativeInferenceEvent&)> observer)
{
  if (!m_operation || !observer) {
    throw NativeDiError("INVALID_OBSERVER", "local", "observer",
                        "native observer is empty");
  }
  std::optional<NativeInferenceEvent> terminal;
  {
    std::lock_guard<std::mutex> lock(m_operation->mutex);
    m_operation->observers.push_back(observer);
    if (m_operation->status != NativeRequestStatus::Pending) {
      terminal = NativeInferenceEvent{m_operation->requestId, {}, true};
    }
  }
  if (terminal) {
    try { observer(*terminal); } catch (...) { }
  }
}

NativeInferenceClient::NativeInferenceClient(
  std::shared_ptr<ndn_service_framework::ServiceUser> user,
  std::shared_ptr<const NativeAdapterRegistry> adapters,
  std::shared_ptr<NativeGrantClient> grants,
  std::shared_ptr<NativeConversationCoordinator> conversations,
  std::shared_ptr<NativeRequestPreparation> preparation,
  std::shared_ptr<const NativeOfferAdmission> admission)
  : m_user(std::move(user))
  , m_adapters(std::move(adapters))
  , m_grants(std::move(grants))
  , m_conversations(std::move(conversations))
  , m_preparation(std::move(preparation))
  , m_admission(std::move(admission))
{
  if (!m_user || !m_adapters) {
    throw NativeDiError("INVALID_CLIENT_CONFIGURATION", "local", "constructor",
                        "native client requires a ServiceUser and adapter registry");
  }
}

NativeInferenceClient::~NativeInferenceClient() noexcept
{
  close();
}

NativeInferenceHandle NativeInferenceClient::request(
  const NativeModelRef& model,
  const NativeApplicationInput& input,
  std::shared_ptr<const NativeModelSplitStrategy> splitStrategy,
  std::shared_ptr<const NativePlacementStrategy> placementStrategy,
  const NativeRequestOptions& options)
{
  std::shared_ptr<NativeInferenceHandle::Operation> operation;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed) {
      throw NativeDiError("CLIENT_CLOSED", "local", "request",
                          "native inference client is closed");
    }
    if (!splitStrategy || !placementStrategy || options.timeoutMs == 0 ||
        options.ackTimeoutMs == 0 || options.ackTimeoutMs > options.timeoutMs ||
        input.payload.empty() && input.repositoryReference.empty()) {
      throw NativeDiError("INVALID_REQUEST", "local", "request",
                          "native request arguments are invalid");
    }
    model.validate();
    if (!m_adapters->find(model.adapterId)) {
      throw NativeDiError("ADAPTER_NOT_REGISTERED", "planning", "request",
                          "native model adapter is not registered");
    }
    operation = std::make_shared<NativeInferenceHandle::Operation>();
    operation->requestId = "/NDNSF/DI/REQUEST/" +
      std::to_string(NEXT_REQUEST_ID.fetch_add(1));
    m_operations.push_back(operation);
  }
  // This installable-boundary implementation intentionally does not claim a
  // network result before the preparation/admission/Core orchestration units
  // are linked.  The handle therefore records a structured failure instead
  // of returning a synthetic success; later lifecycle tasks replace this
  // single transition with the real asynchronous pipeline.
  operation->error = std::make_shared<NativeDiError>(
    "NATIVE_REQUEST_PIPELINE_NOT_READY", "planning", "request",
    "native request orchestration is not linked in this build",
    operation->requestId, 1);
  operation->status = NativeRequestStatus::Failed;
  operation->condition.notify_all();
  return NativeInferenceHandle(std::move(operation));
}

void NativeInferenceClient::close() noexcept
{
  std::vector<std::shared_ptr<NativeInferenceHandle::Operation>> operations;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed) return;
    m_closed = true;
    for (auto& weak : m_operations) {
      if (auto operation = weak.lock()) operations.push_back(std::move(operation));
    }
    m_operations.clear();
  }
  for (const auto& operation : operations) {
    NativeInferenceHandle(operation).cancel();
  }
}

} // namespace ndnsf::di
