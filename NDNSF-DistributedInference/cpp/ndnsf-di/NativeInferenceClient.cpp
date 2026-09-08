#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace ndnsf::di {

namespace {

std::atomic<std::uint64_t> NEXT_REQUEST_ID{1};

// Bounded observer delivery capacity (CD-001 M09 / CD-007 notified event
// queue): only bounded non-secret observation events are queued.  A terminal
// is a single event per operation, so saturation is unreachable until
// token/progress events arrive with the stream tasks (T010-C); when it
// happens it records DELIVERY_OVERFLOW and drops the event instead of
// fabricating a business result.
constexpr std::size_t kObservedEventCapacity = 64;

struct ObserverEntry
{
  std::function<void(const NativeInferenceEvent&)> function;
  std::size_t nextEvent = 0;
};

std::function<std::chrono::steady_clock::time_point()>
defaultClock()
{
  return [] { return std::chrono::steady_clock::now(); };
}

} // namespace

// DI request phase (CD-007 State Authority): the serial executor advances
// NEW → PREPARING_INPUT → REQUESTING → PLANNING → COMMITTED, and every
// outcome funnels into the single TERMINAL state exactly once.  Core network
// state stays with Core; this phase is request-side bookkeeping only and late
// results can never resurrect a terminal.
enum class DiRequestPhase { New, PreparingInput, Requesting, Planning, Committed, Terminal };

// Client-owned serial work executor (C01): DI state transitions are submitted
// here and run one at a time on a detached worker thread — never on the
// caller thread and never on the Core I/O thread.  Tasks capture only
// operation/dependency shared_ptrs, never the client, so an in-flight task
// can safely outlive the client.  close() never joins the worker: a
// callback-driven close must not wait on a thread that may itself be waiting
// on a Core callback. The worker owns only its queue state and exits after
// the executor facade is stopped or released and queued deliveries drain.
class SerialRequestExecutor
{
  struct State {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::function<void()>> queue;
    std::multimap<std::chrono::steady_clock::time_point,
                  std::pair<std::uint64_t, std::function<void()>>> timers;
    std::uint64_t nextTimer = 0;
    bool stopped = false;
  };
public:
  static std::shared_ptr<SerialRequestExecutor>
  create(std::function<void(std::function<void()>)> submitHook = {})
  {
    auto executor = std::shared_ptr<SerialRequestExecutor>(
      new SerialRequestExecutor(std::move(submitHook)));
    if (!executor->m_submitHook) {
      // The thread owns queue state, not the executor facade. Releasing the
      // last client/operation facade stops the queue, including self-release
      // from an observer; there is no self-owning idle thread cycle.
      std::thread([state = executor->m_state] {
        for (;;) {
          std::function<void()> task;
          {
            std::unique_lock<std::mutex> lock(state->mutex);
            for (;;) {
              if (!state->queue.empty()) {
                task = std::move(state->queue.front());
                state->queue.pop_front();
                break;
              }
              if (state->stopped) return;
              if (state->timers.empty()) {
                state->condition.wait(lock);
              } else if (state->timers.begin()->first <= std::chrono::steady_clock::now()) {
                task = std::move(state->timers.begin()->second.second);
                state->timers.erase(state->timers.begin());
                break;
              } else {
                // Copy the deadline: cancelling a timer may erase its node
                // while wait_until releases the mutex.
                const auto deadline = state->timers.begin()->first;
                state->condition.wait_until(lock, deadline);
              }
            }
          }
          task();
        }
      }).detach();
    }
    return executor;
  }

  ~SerialRequestExecutor() { stop(); }

  void submit(std::function<void()> task)
  {
    if (m_submitHook) {
      m_submitHook(std::move(task));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(m_state->mutex);
      if (m_state->stopped) return;
      m_state->queue.push_back(std::move(task));
    }
    m_state->condition.notify_one();
  }

  void stop() noexcept
  {
    {
      std::lock_guard<std::mutex> lock(m_state->mutex);
      m_state->stopped = true;
    }
    m_state->condition.notify_all();
  }

  // Only the independent deadline executor receives timed work. A blocked
  // preparation job or observer cannot postpone this wakeup.
  std::function<void()> scheduleAt(std::chrono::steady_clock::time_point deadline,
                                   std::function<void()> task)
  {
    std::uint64_t id;
    {
      std::lock_guard<std::mutex> lock(m_state->mutex);
      if (m_state->stopped) throw std::runtime_error("native deadline executor stopped");
      id = ++m_state->nextTimer;
      m_state->timers.emplace(deadline, std::make_pair(id, std::move(task)));
    }
    m_state->condition.notify_one();
    return [state = m_state, id] {
      std::function<void()> retired;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (auto it = state->timers.begin(); it != state->timers.end(); ++it) {
          if (it->second.first == id) {
            retired = std::move(it->second.second);
            state->timers.erase(it);
            break;
          }
        }
      }
      state->condition.notify_one();
      // Captures are destroyed outside the queue lock.
    };
  }

private:
  explicit SerialRequestExecutor(std::function<void(std::function<void()>)> submitHook)
    : m_submitHook(std::move(submitHook)), m_state(std::make_shared<State>()) {}

  std::function<void(std::function<void()>)> m_submitHook;
  std::shared_ptr<State> m_state;
};

struct NativeInferenceHandle::Operation
{
  // The caller's Core owner survives asynchronous work and handle waits.
  // It does not transfer ownership of the application's Face to DI.
  std::shared_ptr<ndn_service_framework::ServiceUser> user;
  // Submission values are owned snapshots. Worker/callback work must never
  // read the caller's model/input/options or borrow the client's lifetime.
  NativeModelRef model;
  NativeApplicationInput input;
  NativeRequestOptions options;
  std::shared_ptr<const NativeModelSplitStrategy> splitStrategy;
  std::shared_ptr<const NativePlacementStrategy> placementStrategy;
  std::shared_ptr<NativeRequestPreparation> preparation;
  std::optional<NativePreparedInput> preparedInput;
  std::shared_ptr<const NativeRequestContract> requestContract;
  std::uint64_t wireDeadlineMs = 0;
  std::optional<NativeEncodedRequest> encodedRequest;
  std::shared_ptr<const NativeRequestRuntime> runtime;
  std::shared_ptr<const NativeOfferAdmission> admission;
  std::shared_ptr<const NativeAdapterRegistry> adapters;
  std::shared_ptr<SerialRequestExecutor> worker;
  std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
  NativeRequestOptions coreOptions;
  std::optional<NativeInspectedModel> inspected;
  std::optional<NativePlannedRequest> planned;
  bool coreActive = false;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::string requestId;
  NativeRequestStatus status = NativeRequestStatus::Pending;
  DiRequestPhase phase = DiRequestPhase::New;
  std::uint64_t attempt = 1;
  std::chrono::steady_clock::time_point deadline{};
  std::function<void()> cancelDeadline;
  NativeInferenceResult result;
  std::shared_ptr<NativeDiError> error;
  std::uint64_t staleCallbacks = 0;    // late/duplicate terminal attempts: counted, never resurrecting
  std::uint64_t deliveryOverflows = 0; // DELIVERY_OVERFLOW records on the bounded observation queue
  std::vector<NativeInferenceEvent> events; // bounded observer-only events, non-authoritative
  std::vector<ObserverEntry> observers;
  std::shared_ptr<SerialRequestExecutor> notifications;
};

namespace {

NativeInferenceEvent
makeTerminalEvent(const NativeInferenceHandle::Operation& operation)
{
  NativeInferenceEvent event;
  event.requestId = operation.requestId;
  event.terminal = true;
  return event;
}

NativeDiError
makeError(const NativeInferenceHandle::Operation& operation)
{
  return NativeDiError(operation.error ? operation.error->code() : "NATIVE_REQUEST_FAILED",
                       operation.error ? operation.error->domain() : "runtime",
                       operation.error ? operation.error->boundary() : "request",
                       operation.error ? operation.error->what() : "native request failed",
                       operation.requestId, operation.attempt);
}

// Single-terminal gate (M02/CD-007): an operation leaves Pending at most
// once.  Every later terminal attempt — a duplicate cancel, a late dispatch,
// a Core callback landing after the terminal — is counted as stale and
// consumed; it can never resurrect or replace the recorded outcome.  Callers
// must not hold the operation lock.
bool
markTerminal(const std::shared_ptr<NativeInferenceHandle::Operation>& operation,
             NativeRequestStatus terminal,
             std::shared_ptr<NativeDiError> error = nullptr,
             const NativeInferenceResult* result = nullptr)
{
  std::function<void()> cancelDeadline;
  bool cancelCore = false;
  std::vector<std::string> releaseScopes;
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (operation->status != NativeRequestStatus::Pending) {
      ++operation->staleCallbacks;
      return false;
    }
    if (result) {
      operation->result = *result;
    }
    if (error) {
      operation->error = std::move(error);
    }
    operation->status = terminal;
    operation->cancelled->store(true);
    cancelCore = operation->coreActive;
    if (operation->planned) {
      for (const auto& scope : operation->planned->corePlan.keyScopes) releaseScopes.push_back(scope.name);
    }
    operation->phase = DiRequestPhase::Terminal;
    cancelDeadline = std::move(operation->cancelDeadline);
  }
  if (cancelDeadline) cancelDeadline();
  operation->condition.notify_all();
  if (cancelCore) {
    const auto user = operation->user;
    const auto id = ndn::Name(operation->requestId);
    user->postToIo([user, id, scopes = std::move(releaseScopes)] {
      user->CancelCollaboration(id);
      // Core may already have consumed the pending call before its terminal
      // callback. Release retained per-scope data independently of that map.
      for (const auto& scope : scopes) user->clearVerifiedCollaborationData(id, scope);
    });
  }
  return true;
}

// Bounded observer delivery (M09/CD-007): appends one non-secret event to the
// operation queue, then drains pending events to every registered observer
// from its own cursor.  Cursors advance before the callbacks run, so a
// throwing observer never receives a replay and never changes the request
// outcome; observer exceptions are isolated.
void
publishEvent(const std::shared_ptr<NativeInferenceHandle::Operation>& operation,
             NativeInferenceEvent event)
{
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (operation->events.size() >= kObservedEventCapacity) {
      ++operation->deliveryOverflows;
      return;
    }
    operation->events.push_back(std::move(event));
    for (auto& observer : operation->observers) {
      for (; observer.nextEvent < operation->events.size(); ++observer.nextEvent) {
        // Queue under the operation lock: late replay and new delivery share
        // one enqueue order. User code runs only on the notification worker.
        operation->notifications->submit(
          [function = observer.function, event = operation->events[observer.nextEvent]] {
            try { function(event); } catch (...) {}
          });
      }
    }
  }
}

void
failOperation(const std::shared_ptr<NativeInferenceHandle::Operation>& operation,
              NativeDiError error)
{
  if (!markTerminal(operation, NativeRequestStatus::Failed,
                    std::make_shared<NativeDiError>(std::move(error)))) {
    return;
  }
  publishEvent(operation, makeTerminalEvent(*operation));
}

void
cancelOperation(const std::shared_ptr<NativeInferenceHandle::Operation>& operation)
{
  if (!markTerminal(operation, NativeRequestStatus::Cancelled)) {
    return;
  }
  publishEvent(operation, makeTerminalEvent(*operation));
}

void enqueueOperation(const std::shared_ptr<NativeInferenceHandle::Operation>& operation,
                      std::function<void()> work, const std::string& boundary)
{
  try {
    operation->worker->submit([operation, work = std::move(work), boundary] {
      if (operation->cancelled->load()) return;
      try { work(); }
      catch (const NativeDiError& error) { failOperation(operation, error); }
      catch (...) {
        failOperation(operation, NativeDiError("NATIVE_REQUEST_STAGE_FAILED", "runtime", boundary,
          "native request stage failed", operation->requestId, operation->attempt));
      }
    });
  }
  catch (const std::exception&) {
    if (!operation->cancelled->load()) failOperation(operation, NativeDiError(
      "NATIVE_REQUEST_DISPATCH_FAILED", "local", boundary, "request worker is unavailable",
      operation->requestId, operation->attempt));
  }
}

void beginCoreRequest(const std::shared_ptr<NativeInferenceHandle::Operation>& operation)
{
  operation->user->postToIo([operation] {
    try {
      {
        std::lock_guard<std::mutex> lock(operation->mutex);
        if (operation->status != NativeRequestStatus::Pending) return;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          operation->deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= static_cast<std::int64_t>(operation->options.ackTimeoutMs))
          throw NativeDiError("NATIVE_REQUEST_TIMEOUT", "local", "begin",
            "request budget cannot accommodate the ACK window", operation->requestId, operation->attempt);
        operation->coreOptions = operation->options;
        operation->coreOptions.timeoutMs = static_cast<std::uint64_t>(remaining);
        operation->phase = DiRequestPhase::Requesting;
        operation->coreActive = true;
      }
      const auto ackClosed = [operation](const ndn_service_framework::CollaborationAckClosure& closure) {
        enqueueOperation(operation, [operation, closure] {
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            if (operation->status != NativeRequestStatus::Pending || operation->phase != DiRequestPhase::Requesting) return;
            operation->phase = DiRequestPhase::Planning;
          }
          NativeRequestControl control{operation->requestId, operation->attempt, operation->deadline,
            [flag = operation->cancelled] { return flag->load(); }};
          auto planned = planNativeRequest(*operation->runtime, operation->coreOptions,
            *operation->inspected, *operation->encodedRequest, *operation->splitStrategy,
            *operation->placementStrategy, *operation->preparation, *operation->admission,
            closure, control, operation->wireDeadlineMs, operation->cancelled);
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            if (operation->status != NativeRequestStatus::Pending) return;
            operation->planned = std::move(planned);
          }
          operation->user->postToIo([operation, digest = closure.digest] {
            try {
              if (operation->cancelled->load()) return;
              if (std::chrono::steady_clock::now() >= operation->deadline)
                throw NativeDiError("NATIVE_REQUEST_TIMEOUT", "local", "commit",
                  "request expired before commit", operation->requestId, operation->attempt);
              if (!operation->user->CommitCollaborationPlan(ndn::Name(operation->requestId),
                    digest, operation->planned->corePlan))
                throw std::runtime_error("Core rejected the sealed plan");
              std::lock_guard<std::mutex> lock(operation->mutex);
              if (operation->status == NativeRequestStatus::Pending) operation->phase = DiRequestPhase::Committed;
            }
            catch (const NativeDiError& error) { failOperation(operation, error); }
            catch (...) { failOperation(operation, NativeDiError(
              "NATIVE_REQUEST_COMMIT_FAILED", "runtime", "commit", "Core plan commit failed",
              operation->requestId, operation->attempt)); }
          });
        }, "ACK_CLOSED");
      };
      const auto response = [operation](const ndn_service_framework::ResponseMessage& message) {
        enqueueOperation(operation, [operation, message] {
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            if (operation->status != NativeRequestStatus::Pending) return;
            if (operation->phase != DiRequestPhase::Committed || !operation->planned)
              throw std::runtime_error("response preceded plan commit");
          }
          if (!message.getStatus()) throw NativeDiError("NATIVE_PROVIDER_FAILED", "provider", "response",
            "selected Provider returned a failure", operation->requestId, operation->attempt);
          if (message.getDataName().empty() || message.getSignerCertificate().empty() ||
              message.getWireDigest().empty() || !ndn::Name(operation->planned->terminalProvider).isPrefixOf(
                ndn::Name(message.getDataName())))
            throw NativeDiError("NATIVE_RESPONSE_BINDING_REJECTED", "provider", "response",
              "response lacks the terminal Provider transport binding", operation->requestId, operation->attempt);
          auto adapter = operation->adapters->find(operation->model.adapterId);
          NativeInferenceResult result;
          const auto payload = message.getPayload();
          result.payload = adapter->decodeResult(std::vector<std::uint8_t>(payload.begin(), payload.end()));
          result.modelDigest = operation->encodedRequest->modelIntentDigest;
          result.planDigest = operation->planned->sealed.planDigest;
          if (std::chrono::steady_clock::now() >= operation->deadline)
            throw NativeDiError("NATIVE_REQUEST_TIMEOUT", "local", "response",
              "request expired during result decoding", operation->requestId, operation->attempt);
          if (markTerminal(operation, NativeRequestStatus::Succeeded, nullptr, &result))
            publishEvent(operation, makeTerminalEvent(*operation));
        }, "response");
      };
      const auto timeout = [operation](const ndn::Name&) {
        failOperation(operation, NativeDiError("NATIVE_REQUEST_TIMEOUT", "local", "Core",
          "Core request deadline expired", operation->requestId, operation->attempt));
      };
      ndn_service_framework::RequestCapabilities capabilities;
      capabilities.setField("NDNSF_DATA_V1", "required");
      operation->user->BeginCollaboration(ndn::Name(operation->runtime->contract.serviceName),
        ndn::Buffer(operation->encodedRequest->wire.begin(), operation->encodedRequest->wire.end()),
        static_cast<int>(operation->coreOptions.ackTimeoutMs),
        static_cast<int>(operation->coreOptions.timeoutMs), ackClosed, response, timeout,
        ndn::Name(operation->requestId), {}, capabilities);
    }
    catch (const NativeDiError& error) { failOperation(operation, error); }
    catch (...) { failOperation(operation, NativeDiError(
      "NATIVE_REQUEST_BEGIN_FAILED", "runtime", "begin", "Core request start failed",
      operation->requestId, operation->attempt)); }
  });
}

// Serial dispatch driver, run on the executor worker or the unit-test pump.
// The absolute per-request deadline is checked before any stage work: work
// that can no longer finish inside the request budget is refused and the
// operation fails once with NATIVE_REQUEST_TIMEOUT.  Orchestration stages
// (CD-013 preparation, Core BeginCollaboration, planning, commit) link into
// this driver in T010-B; this installable-boundary build records the frozen
// structured failure instead of claiming a synthetic success.
void
dispatchOperation(const std::shared_ptr<NativeInferenceHandle::Operation>& operation,
                  const std::function<std::chrono::steady_clock::time_point()>& now)
{
  bool expired = false;
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (operation->status != NativeRequestStatus::Pending ||
        operation->phase != DiRequestPhase::New) {
      // cancel/close won the race: consume the late dispatch, do not resurrect.
      ++operation->staleCallbacks;
      return;
    }
    operation->phase = DiRequestPhase::PreparingInput;
    expired = now() >= operation->deadline;
  }
  if (expired) {
    failOperation(operation, NativeDiError(
      "NATIVE_REQUEST_TIMEOUT", "local", "request",
      "native request budget expired before dispatch", operation->requestId,
      operation->attempt));
    return;
  }
  if (operation->preparation) {
    NativePreparedInput prepared;
    try {
      const auto& input = operation->input;
      const auto& options = operation->options;
      if (input.transportMode != NativeInputTransportMode::Inline)
        throw std::invalid_argument("repository input resolution is not linked");
      if (!options.taskName.empty() && !input.taskName.empty() &&
          options.taskName != input.taskName)
        throw std::invalid_argument("request task names disagree");
      prepared = operation->preparation->prepareInput(
        operation->model, options.taskName.empty() ? input.taskName : options.taskName,
        input.inputSchemaDigest, input.optionsSchemaDigest, input.payload,
        input.repositoryReference, operation->deadline);
    }
    catch (const NativeDiError&) { throw; }
    catch (const std::exception&) {
      if (now() >= operation->deadline) {
        failOperation(operation, NativeDiError(
          "NATIVE_REQUEST_TIMEOUT", "local", "request",
          "native request budget expired during preparation", operation->requestId,
          operation->attempt));
        return;
      }
      failOperation(operation, NativeDiError(
        "NATIVE_INPUT_PREPARATION_FAILED", "planning", "prepareInput",
        "native request input preparation failed", operation->requestId, operation->attempt));
      return;
    }
    // Encoding can invoke application adapters while close/cancel/deadline
    // runs concurrently. Only the pending operation may retain its output.
    {
      std::lock_guard<std::mutex> lock(operation->mutex);
      if (operation->status != NativeRequestStatus::Pending) {
        ++operation->staleCallbacks;
        return;
      }
      expired = now() >= operation->deadline;
      if (!expired) operation->preparedInput = std::move(prepared);
    }
    if (expired) {
      failOperation(operation, NativeDiError(
        "NATIVE_REQUEST_TIMEOUT", "local", "request",
        "native request budget expired during preparation", operation->requestId,
        operation->attempt));
      return;
    }
  }
  if (operation->requestContract && operation->preparedInput) {
    auto input = operation->input;
    input.payload = operation->preparedInput->payload;
    input.taskName = operation->preparedInput->taskName;
    NativeEncodedRequest encoded;
    try {
      encoded = encodeNativeRequestEnvelope(operation->model, input,
        *operation->requestContract, operation->requestId, operation->attempt,
        operation->wireDeadlineMs);
    }
    catch (const std::exception&) {
      failOperation(operation, NativeDiError(
        "NATIVE_REQUEST_CONTRACT_INVALID", "planning", "requestWire",
        "native request contract encoding failed", operation->requestId, operation->attempt));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(operation->mutex);
      if (operation->status != NativeRequestStatus::Pending) {
        ++operation->staleCallbacks;
        return;
      }
      expired = now() >= operation->deadline;
      if (!expired) operation->encodedRequest = std::move(encoded);
    }
    if (expired) {
      failOperation(operation, NativeDiError(
        "NATIVE_REQUEST_TIMEOUT", "local", "request",
        "native request budget expired during encoding", operation->requestId, operation->attempt));
      return;
    }
  }
  if (operation->runtime && operation->preparedInput && operation->encodedRequest) {
    auto inspected = operation->preparation->inspectModel(*operation->preparedInput);
    {
      std::lock_guard<std::mutex> lock(operation->mutex);
      if (operation->status != NativeRequestStatus::Pending) return;
      operation->inspected = std::move(inspected);
    }
    beginCoreRequest(operation);
    return;
  }
  failOperation(operation, NativeDiError(
    "NATIVE_REQUEST_PIPELINE_NOT_READY", "planning", "request",
    "native request orchestration is not linked in this build",
    operation->requestId, operation->attempt));
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
  if (waitTimeout.count() < 0) {
    throw NativeDiError("INVALID_WAIT_TIMEOUT", "local", "wait",
                        "native result wait duration must be nonnegative");
  }
  // Work on a local copy of the operation: the wait must keep the operation
  // alive even if the last user reference to this handle is released from
  // another thread while the wait is parked.
  const auto operation = m_operation;
  if (waitTimeout.count() > 0 && operation->user->isOnIoThread()) {
    throw NativeDiError("CORE_IO_WAIT_FORBIDDEN", "local", "wait",
                        "native result cannot block the Core I/O thread",
                        operation->requestId, operation->attempt);
  }
  std::unique_lock<std::mutex> lock(operation->mutex);
  if (!operation->condition.wait_for(lock, waitTimeout, [&operation] {
        return operation->status != NativeRequestStatus::Pending;
      })) {
    // The wait timed out; the request itself is untouched and may still
    // complete, be cancelled, or be waited on again (M07).
    throw NativeDiError("LOCAL_WAIT_TIMEOUT", "local", "wait",
                        "native result wait timed out", operation->requestId,
                        operation->attempt);
  }
  if (operation->status == NativeRequestStatus::Succeeded) {
    return operation->result;
  }
  if (operation->status == NativeRequestStatus::Cancelled) {
    throw NativeDiError("CANCELLED", "local", "request",
                        "native request was cancelled", operation->requestId,
                        operation->attempt);
  }
  throw makeError(*operation);
}

void NativeInferenceHandle::cancel()
{
  if (!m_operation) return;
  cancelOperation(m_operation);
}

void NativeInferenceHandle::observe(
  std::function<void(const NativeInferenceEvent&)> observer)
{
  if (!m_operation || !observer) {
    throw NativeDiError("INVALID_OBSERVER", "local", "observer",
                        "native observer is empty");
  }
  // The new observer replays every event recorded so far (a terminal arrives
  // once, and a late observer still sees it), while its drain cursor starts
  // at events.size(): already-delivered history is never re-drained.  The
  // function and events are copied into the independent notification queue
  // under the lock; replay and new events therefore have one serial order.
  {
    std::lock_guard<std::mutex> lock(m_operation->mutex);
    for (const auto& event : m_operation->events) {
      m_operation->notifications->submit([function = observer, event] {
        try { function(event); } catch (...) {}
      });
    }
    m_operation->observers.push_back(
      ObserverEntry{std::move(observer), m_operation->events.size()});
  }
}

NativeInferenceClient::NativeInferenceClient(
  std::shared_ptr<ndn_service_framework::ServiceUser> user,
  std::shared_ptr<const NativeAdapterRegistry> adapters,
  std::shared_ptr<NativeGrantClient> grants,
  std::shared_ptr<NativeConversationCoordinator> conversations,
  std::shared_ptr<NativeRequestPreparation> preparation,
  std::shared_ptr<const NativeOfferAdmission> admission)
  : NativeInferenceClient(TestPort{},
                          std::move(user), std::move(adapters),
                          std::move(grants), std::move(conversations),
                          std::move(preparation), std::move(admission))
{
}

NativeInferenceClient::NativeInferenceClient(
  std::shared_ptr<ndn_service_framework::ServiceUser> user,
  std::shared_ptr<const NativeAdapterRegistry> adapters,
  const NativeRequestContract& contract,
  std::shared_ptr<NativeRequestPreparation> preparation,
  std::shared_ptr<const NativeOfferAdmission> admission)
  : NativeInferenceClient(std::move(user), std::move(adapters), nullptr, nullptr,
                          std::move(preparation), std::move(admission))
{
  if (!m_preparation || !m_admission || contract.serviceName.empty() ||
      contract.serviceName.front() != '/' || contract.taskName.empty())
    throw NativeDiError("INVALID_CLIENT_CONFIGURATION", "local", "constructor",
                        "native requester requires service, task, preparation and admission");
  m_requestContract = std::make_shared<const NativeRequestContract>(contract);
}

NativeInferenceClient::NativeInferenceClient(
  std::shared_ptr<ndn_service_framework::ServiceUser> user,
  std::shared_ptr<const NativeAdapterRegistry> adapters,
  const NativeRequestRuntime& runtime,
  std::shared_ptr<NativeRequestPreparation> preparation,
  std::shared_ptr<const NativeOfferAdmission> admission)
  : NativeInferenceClient(std::move(user), std::move(adapters), runtime.contract,
                          std::move(preparation), std::move(admission))
{
  runtime.budget.validate();
  if (!runtime.grants || !runtime.security.requireProtectedArtifacts || runtime.requesterIdentity.empty() ||
      runtime.protectionEpoch.empty() || runtime.protectionEpoch == "plaintext-v1" ||
      !runtime.maxSegments || !runtime.noProgressMs)
    throw NativeDiError("INVALID_CLIENT_CONFIGURATION", "local", "constructor",
      "native requester requires protected runtime policy and grant owner");
  m_runtime = std::make_shared<const NativeRequestRuntime>(runtime);
}

NativeInferenceClient::NativeInferenceClient(
  const TestPort& testPort,
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
  , m_now(testPort.now ? testPort.now : defaultClock())
  , m_executor(SerialRequestExecutor::create(testPort.submitHook))
  , m_notifications(SerialRequestExecutor::create())
  , m_deadlines(SerialRequestExecutor::create())
  , m_schedule(testPort.scheduleHook ? testPort.scheduleHook :
      [executor = m_deadlines](std::chrono::steady_clock::time_point deadline,
                               std::function<void()> task) {
        return executor->scheduleAt(deadline, std::move(task));
      })
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
        options.ackTimeoutMs == 0 || options.ackTimeoutMs >= options.timeoutMs ||
        options.timeoutMs > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        (input.payload.empty() && input.repositoryReference.empty())) {
      throw NativeDiError("INVALID_REQUEST", "local", "request",
                          "native request arguments are invalid");
    }
    model.validate();
    if (!m_adapters->find(model.adapterId)) {
      throw NativeDiError("ADAPTER_NOT_REGISTERED", "planning", "request",
                          "native model adapter is not registered");
    }
    operation = std::make_shared<NativeInferenceHandle::Operation>();
    operation->user = m_user;
    operation->model = model;
    operation->input = input;
    operation->options = options;
    operation->splitStrategy = std::move(splitStrategy);
    operation->placementStrategy = std::move(placementStrategy);
    operation->preparation = m_preparation;
    operation->requestContract = m_requestContract;
    operation->runtime = m_runtime;
    operation->admission = m_admission;
    operation->adapters = m_adapters;
    operation->worker = m_executor;
    operation->notifications = m_notifications;
    // The requestId comes from a unique native owner allocated at submission
    // (runtime-boundaries: Core allocation or unique native owner); the
    // operation then binds ACK/plan/grant/result to this stable URI.
    operation->requestId = "/NDNSF/DI/REQUEST/" +
      std::to_string(NEXT_REQUEST_ID.fetch_add(1));
    operation->attempt = 1;
    // Absolute budget: computed once from the submission clock; waits and
    // cancels never re-arm it (deadline / operation row, CD-007).
    operation->deadline = m_now() + std::chrono::milliseconds(options.timeoutMs);
    // Freeze the wire wall-clock expiry at the same submission boundary.
    // Dispatch/ACK latency must not grant a new relative request budget.
    const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    if (epochMs <= 0) throw NativeDiError("INVALID_REQUEST_CLOCK", "local", "request",
                                        "request wall clock precedes the epoch");
    operation->wireDeadlineMs = static_cast<std::uint64_t>(epochMs) + options.timeoutMs;
    m_operations.push_back(operation);
  }
  // Submission returns a Pending handle; the dispatch runs on the
  // client-owned serial executor, never on the caller or the Core I/O thread.
  // The dispatch task captures only the operation and the clock and may
  // safely outlive this client.
  auto now = m_now;
  try {
    {
      std::lock_guard<std::mutex> lock(operation->mutex);
      if (operation->status != NativeRequestStatus::Pending) {
        return NativeInferenceHandle(std::move(operation));
      }
      operation->cancelDeadline = m_schedule(operation->deadline,
        [weak = std::weak_ptr<NativeInferenceHandle::Operation>(operation)] {
          if (auto pending = weak.lock()) {
            failOperation(pending, NativeDiError(
              "NATIVE_REQUEST_TIMEOUT", "local", "request",
              "native request budget expired", pending->requestId, pending->attempt));
          }
        });
    }
    m_executor->submit([operation, now] {
      try {
        dispatchOperation(operation, now);
      } catch (const NativeDiError& error) {
        failOperation(operation, error);
      } catch (...) {
        failOperation(operation, NativeDiError(
          "NATIVE_REQUEST_DISPATCH_FAILED", "local", "request",
          "native request dispatch failed", operation->requestId, operation->attempt));
      }
    });
  } catch (...) {
    failOperation(operation, NativeDiError(
      "NATIVE_REQUEST_DISPATCH_FAILED", "local", "request",
      "native request dispatch failed", operation->requestId, operation->attempt));
  }
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
    cancelOperation(operation);
  }
  // Stop serialization without joining: queued dispatches drain as no-ops on
  // already-terminal operations, and a callback-driven close never waits on
  // the executor.  The shared Core/user is not closed or joined here.
  m_executor->stop();
  m_deadlines->stop();
}

} // namespace ndnsf::di
