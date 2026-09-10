#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DecodeStateIdentity.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <openssl/rand.h>

namespace ndnsf::di {

namespace {

std::atomic<std::uint64_t> NEXT_REQUEST_ID{1};

// Bounded observer delivery capacity (CD-001 M09 / CD-007 notified event
// queue): only bounded non-secret observation events are queued.  A terminal
// is a single event per operation, so saturation is unreachable until
// token/progress events are accepted by the stream path; when the observer
// queue saturates it records DELIVERY_OVERFLOW and drops the event instead of
// fabricating a business result.
constexpr std::size_t kObservedEventCapacity = 64;
constexpr char kConversationStateScope[] = "ndnsf-di-conversation-state-v1";
constexpr char kConversationReceiptTopic[] = "/ndnsf-di/conversation/receipt";
constexpr char kConversationCommitTopic[] = "/ndnsf-di/conversation/commit";
constexpr char kConversationRollbackTopic[] = "/ndnsf-di/conversation/rollback";
constexpr char kConversationControlTopic[] = "/ndnsf-di/conversation/control";

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

ProviderConversationStateReceiptV1
parseConversationReceipt(const NativeJson& value)
{
  static const std::set<std::string> fields = {
    "adapterDigest", "cacheEpoch", "conversationId", "expiresAtMs",
    "graphSemanticDigest", "layoutDigest", "modelDigest",
    "originGenerationId", "originRequestId", "parentContextEpoch",
    "planRoleMapDigest", "positionDigest", "prefixDigest", "prefixTokenCount",
    "providerBootId", "providerIdentity", "receiptDigest", "requesterIdentity",
    "roleName", "roleSplitDigest", "schema", "securityDomainDigest",
    "serviceName", "signature", "stateComponentDigests", "stateSchemaDigest",
    "successorContextEpoch",
  };
  if (!value.is_object() || value.value("schema", std::string{}) !=
        "ndnsf-di-provider-conversation-receipt-v1")
    throw std::invalid_argument("conversation receipt schema mismatch");
  std::set<std::string> seen;
  for (const auto& item : value.items()) seen.insert(item.key());
  if (seen != fields) throw std::invalid_argument("conversation receipt field set mismatch");
  ProviderConversationStateReceiptV1 result;
  result.conversationId = value.at("conversationId").get<std::string>();
  result.parentContextEpoch = value.at("parentContextEpoch").get<std::uint64_t>();
  result.successorContextEpoch = value.at("successorContextEpoch").get<std::uint64_t>();
  result.originRequestId = value.at("originRequestId").get<std::string>();
  result.originGenerationId = value.at("originGenerationId").get<std::string>();
  result.serviceName = value.at("serviceName").get<std::string>();
  result.requesterIdentity = value.at("requesterIdentity").get<std::string>();
  result.securityDomainDigest = value.at("securityDomainDigest").get<std::string>();
  result.modelDigest = value.at("modelDigest").get<std::string>();
  result.graphSemanticDigest = value.at("graphSemanticDigest").get<std::string>();
  result.adapterDigest = value.at("adapterDigest").get<std::string>();
  result.roleName = value.at("roleName").get<std::string>();
  result.roleSplitDigest = value.at("roleSplitDigest").get<std::string>();
  result.layoutDigest = value.at("layoutDigest").get<std::string>();
  result.planRoleMapDigest = value.at("planRoleMapDigest").get<std::string>();
  result.providerIdentity = value.at("providerIdentity").get<std::string>();
  result.providerBootId = value.at("providerBootId").get<std::string>();
  result.cacheEpoch = value.at("cacheEpoch").get<std::uint64_t>();
  result.prefixDigest = value.at("prefixDigest").get<std::string>();
  result.prefixTokenCount = value.at("prefixTokenCount").get<std::uint32_t>();
  result.positionDigest = value.at("positionDigest").get<std::string>();
  result.stateSchemaDigest = value.at("stateSchemaDigest").get<std::string>();
  result.stateComponentDigests = value.at("stateComponentDigests").get<std::vector<std::string>>();
  result.expiresAtMs = value.at("expiresAtMs").get<std::uint64_t>();
  result.validate();
  if (value.at("receiptDigest") != result.computedDigest() ||
      !value.at("signature").is_string() || !value.at("signature").get<std::string>().empty())
    throw std::invalid_argument("conversation receipt digest or signature field mismatch");
  return result;
}

NativeJson
conversationControlJson(const char* action,
                        const NativeConversationTurn& turn,
                        const ProviderConversationStateReceiptV1& receipt,
                        const std::string& checkpointDigest,
                        std::uint64_t expiresAtMs)
{
  return NativeJson{
    {"schema", "ndnsf-di-conversation-promotion-control-v1"},
    {"action", action},
    {"conversationId", turn.parent.conversationId},
    {"parentContextEpoch", receipt.parentContextEpoch},
    {"successorContextEpoch", receipt.successorContextEpoch},
    {"serviceName", turn.parent.serviceName},
    {"planRoleMapDigest", turn.parent.planRoleMapDigest},
    {"roleName", receipt.roleName},
    {"receiptDigest", receipt.computedDigest()},
    {"checkpointDigest", checkpointDigest},
    {"expiresAtMs", expiresAtMs},
  };
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
  std::shared_ptr<NativeConversationCoordinator> conversations;
  std::optional<NativeConversationTurn> conversationTurn;
  bool conversationCommitted = false;
  bool conversationTransactionActive = false;
  bool conversationCleanupDeferred = false;
  bool conversationCancelDeferred = false;
  bool coreActive = false;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::string requestId;
  std::string applicationRequestId;
  std::string coreRequestId; // Per-attempt transport identity; public requestId remains stable.
  std::optional<NativeGenerationRecovery> recovery;
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

  // Requester acceptance is process-local state, not a Provider commit or a
  // durable conversation checkpoint. Only the serial request worker accepts
  // events; the mutex also fences concurrent cancel/deadline transitions.
  std::vector<std::int64_t> acceptedTokenIds;
  std::string acceptedText;
  std::string acceptedTerminalHint = "NONE";
  std::string generationId;
  std::string samplingDigest;
  std::size_t maxGenerationTokens = 0;
  bool replacementStarted = false;
  std::size_t queuedStreamEvents = 0;

  bool acceptGenerationEvent(std::uint64_t sourceAttempt,
                             const std::vector<std::uint8_t>& payload)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (status != NativeRequestStatus::Pending || sourceAttempt != attempt) {
      ++staleCallbacks;
      return false;
    }
    const auto reject = [&](const char* message) {
      throw NativeDiError("StreamEventLineageMismatch", "provider", "stream-accept",
                          message, requestId, attempt);
    };
    // Bound allocation before JSON parsing; the transport applies its own
    // smaller per-event wire limit as well.
    if (payload.size() > (1U << 20)) reject("generation event exceeds wire bound");
    const auto event = nativeParseJson(std::string(payload.begin(), payload.end()));
    if (!event.is_object() || event.value("schema", std::string{}) != "GenerationTokenEventV1" ||
        !event.contains("tokenId") || !event.at("tokenId").is_number_integer() ||
        !event.contains("tokenEpoch") || !event.at("tokenEpoch").is_number_unsigned() ||
        !event.contains("textDelta") || !event.at("textDelta").is_string() ||
        !event.contains("finishHint") || !event.at("finishHint").is_string() ||
        !event.contains("acceptedPrefixDigest") || !event.at("acceptedPrefixDigest").is_string())
      reject("invalid generation event fields");
    const auto& tokenValue = event.at("tokenId");
    if ((tokenValue.is_number_unsigned() && tokenValue.get<std::uint64_t>() >
         static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) ||
        tokenValue.get<std::int64_t>() < 0)
      reject("invalid generation token ID");
    const auto token = tokenValue.get<std::int64_t>();
    const auto hint = event.at("finishHint").get<std::string>();
    if (acceptedTerminalHint != "NONE" || !maxGenerationTokens ||
        acceptedTokenIds.size() >= maxGenerationTokens ||
        event.at("tokenEpoch").get<std::uint64_t>() != acceptedTokenIds.size() + 1 ||
        (hint != "NONE" && hint != "EOS" && hint != "STOP_SEQUENCE" && hint != "MAX_TOKENS"))
      reject("generation epoch or terminal hint mismatch");
    // Legacy GenerationTokenEventV1 omits request/generation IDs; Core's
    // authenticated stream binding supplies them. If present, require equality.
    for (const auto& identity : {std::pair<const char*, std::string>{"requestId", coreRequestId},
                                 {"generationId", generationId}, {"samplingDigest", samplingDigest}}) {
      if (event.contains(identity.first) &&
          (!event.at(identity.first).is_string() || event.at(identity.first) != identity.second))
        reject("generation event identity mismatch");
    }
    auto tokens = acceptedTokenIds;
    tokens.push_back(token);
    std::string transcript;
    for (const auto id : tokens) {
      if (!transcript.empty()) transcript += ',';
      transcript += std::to_string(id);
    }
    if (event.at("acceptedPrefixDigest") != nativePlanningDigest(transcript))
      reject("generation token prefix digest mismatch");
    auto text = acceptedText + event.at("textDelta").get<std::string>();
    if (text.size() > (16U << 20)) reject("generation text exceeds output bound");
    // Prepare every allocation before committing any accepted state.
    // Advance the coordinator before publishing the process-local prefix.
    // If the turn was concurrently aborted/replaced, no half-accepted token
    // may remain in the requester operation.
    if (conversationTurn && conversations) {
      conversations->acceptTokenPrefix(*conversationTurn, tokens);
    }
    auto terminalHint = hint;
    acceptedTokenIds.swap(tokens);
    acceptedText.swap(text);
    acceptedTerminalHint.swap(terminalHint);
    return true;
  }

  bool validateGenerationFinal(std::uint64_t sourceAttempt,
                               const std::vector<std::uint8_t>& payload)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (status != NativeRequestStatus::Pending || sourceAttempt != attempt) {
      ++staleCallbacks;
      return false;
    }
    if (payload.size() > (64U << 20))
      throw NativeDiError("StreamFinalMismatch", "provider", "stream-final",
                          "generation final exceeds wire bound", requestId, attempt);
    const auto value = nativeParseJson(std::string(payload.begin(), payload.end()));
    bool tokensMatch = value.is_object() && value.contains("tokenIds") &&
      value.at("tokenIds").is_array() && value.at("tokenIds").size() == acceptedTokenIds.size();
    if (tokensMatch) {
      for (std::size_t i = 0; i < acceptedTokenIds.size(); ++i) {
        const auto& token = value.at("tokenIds").at(i);
        if (!token.is_number_integer() || token != acceptedTokenIds[i]) {
          tokensMatch = false;
          break;
        }
      }
    }
    if (!tokensMatch || acceptedTerminalHint == "NONE" ||
        value.value("schema", std::string{}) != "NDNSF-DI-FINAL-V1" ||
        !value.contains("text") || !value.at("text").is_string() || value.at("text") != acceptedText ||
        !value.contains("finishHint") || value.at("finishHint") != acceptedTerminalHint)
      throw NativeDiError("StreamFinalMismatch", "provider", "stream-final",
                          "final payload disagrees with accepted generation", requestId, attempt);
    for (const auto& identity : {std::pair<const char*, std::string>{"requestId", coreRequestId},
                                 {"generationId", generationId}}) {
      if (value.contains(identity.first) &&
          (!value.at(identity.first).is_string() || value.at(identity.first) != identity.second))
        throw NativeDiError("StreamFinalMismatch", "provider", "stream-final",
                            "generation final identity mismatch", requestId, attempt);
    }
    return true;
  }
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
             const NativeInferenceResult* result = nullptr,
             std::uint64_t expectedAttempt = 0)
{
  std::function<void()> cancelDeadline;
  bool cancelCore = false;
  std::vector<std::string> releaseScopes;
  std::shared_ptr<NativeConversationCoordinator> conversations;
  std::optional<NativeConversationTurn> conversationTurn;
  bool conversationCommitted = false;
  bool deferConversationCleanup = false;
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (operation->status != NativeRequestStatus::Pending ||
        (expectedAttempt && operation->attempt != expectedAttempt)) {
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
    conversations = operation->conversations;
    conversationTurn = operation->conversationTurn;
    conversationCommitted = operation->conversationCommitted;
    deferConversationCleanup = operation->conversationTransactionActive;
    if (deferConversationCleanup) {
      operation->conversationCleanupDeferred = true;
      if (cancelCore) operation->conversationCancelDeferred = true;
    }
    if (operation->planned) {
      for (const auto& scope : operation->planned->corePlan.keyScopes) releaseScopes.push_back(scope.name);
    }
    operation->phase = DiRequestPhase::Terminal;
    cancelDeadline = std::move(operation->cancelDeadline);
  }
  if (cancelDeadline) cancelDeadline();
  operation->condition.notify_all();
  if (conversations && conversationTurn && !conversationCommitted) {
    try {
      const NativeDiError cancelledError(
        terminal == NativeRequestStatus::Cancelled ? "CANCELLED" : "NATIVE_REQUEST_FAILED",
        "conversation", "terminal", "native conversation turn terminated",
        operation->requestId, conversationTurn->attempt);
      conversations->abortTurn(*conversationTurn, cancelledError);
    }
    catch (...) {
      // A terminal request must not be resurrected by an abort-side failure.
    }
  }
  if (cancelCore && !deferConversationCleanup) {
    const auto user = operation->user;
    const auto id = ndn::Name(operation->coreRequestId);
    user->postToIo([user, id, scopes = std::move(releaseScopes), deferConversationCleanup] {
      user->CancelCollaboration(id);
      // A conversation commit may still need the request-scope key for
      // ROLLBACK/FINALIZE. The transaction owner clears it after its callback
      // leaves the coordinator; ordinary terminal paths clear immediately.
      if (!deferConversationCleanup) {
        for (const auto& scope : scopes) user->clearVerifiedCollaborationData(id, scope);
      }
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
             NativeInferenceEvent event, bool requirePending = false)
{
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (requirePending && operation->status != NativeRequestStatus::Pending) {
      ++operation->staleCallbacks;
      return;
    }
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
              NativeDiError error, std::uint64_t expectedAttempt = 0)
{
  if (!markTerminal(operation, NativeRequestStatus::Failed,
                    std::make_shared<NativeDiError>(std::move(error)), nullptr, expectedAttempt)) {
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
      catch (const std::exception& error) {
        failOperation(operation, NativeDiError("NATIVE_REQUEST_STAGE_FAILED", "runtime", boundary,
          std::string("native request stage failed: ") + error.what(),
          operation->requestId, operation->attempt));
      }
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

int conversationWaitBudgetMs(
  const std::shared_ptr<NativeInferenceHandle::Operation>& operation)
{
  std::chrono::steady_clock::time_point deadline;
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    deadline = operation->deadline;
  }
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
    deadline - std::chrono::steady_clock::now()).count();
  if (remaining <= 0) {
    throw NativeDiError("NATIVE_REQUEST_TIMEOUT", "conversation", "receipt",
      "conversation transaction exceeded the request deadline", operation->requestId,
      operation->attempt);
  }
  return static_cast<int>(std::min<std::int64_t>(remaining,
    static_cast<std::int64_t>(std::numeric_limits<int>::max())));
}

void finishConversationTransaction(
  const std::shared_ptr<NativeInferenceHandle::Operation>& operation) noexcept
{
  std::vector<std::string> scopes;
  bool clearScopes = false;
  bool cancelCore = false;
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (!operation->conversationTransactionActive) return;
    operation->conversationTransactionActive = false;
    clearScopes = operation->conversationCleanupDeferred;
    operation->conversationCleanupDeferred = false;
    cancelCore = operation->conversationCancelDeferred;
    operation->conversationCancelDeferred = false;
    if (clearScopes && operation->planned) {
      for (const auto& scope : operation->planned->corePlan.keyScopes)
        scopes.push_back(scope.name);
    }
  }
  if (!clearScopes && !cancelCore) return;
  try {
    const auto user = operation->user;
    const auto id = ndn::Name(operation->coreRequestId);
    user->postToIo([user, id, scopes = std::move(scopes), cancelCore] {
      if (cancelCore) user->CancelCollaboration(id);
      for (const auto& scope : scopes) user->clearVerifiedCollaborationData(id, scope);
    });
  }
  catch (...) {
    // The terminal outcome is already fenced. A failed asynchronous cleanup
    // remains observable through the request-scope retention bound.
  }
}

struct ConversationTransactionGuard
{
  std::shared_ptr<NativeInferenceHandle::Operation> operation;

  ~ConversationTransactionGuard()
  {
    finishConversationTransaction(operation);
  }
};

std::vector<ProviderConversationStateReceiptV1>
collectConversationReceipts(
  const std::shared_ptr<NativeInferenceHandle::Operation>& operation,
  const NativeConversationTurn& turn,
  const NativePlannedRequest& planned)
{
  const std::set<std::string> expectedRoles(turn.parent.expectedRoles.begin(),
                                             turn.parent.expectedRoles.end());
  if (expectedRoles.empty())
    throw NativeDiError("NATIVE_CONVERSATION_RECEIPTS_INCOMPLETE", "conversation", "receipt",
      "conversation turn has no expected Provider roles", operation->requestId, turn.attempt);
  const auto records = operation->user->waitForVerifiedCollaborationData(
    ndn::Name(operation->coreRequestId), kConversationStateScope,
    ndn::Name(kConversationReceiptTopic), expectedRoles.size(),
    conversationWaitBudgetMs(operation), true);
  if (records.size() != expectedRoles.size())
    throw NativeDiError("NATIVE_CONVERSATION_RECEIPTS_INCOMPLETE", "conversation", "receipt",
      "authenticated conversation receipt set is incomplete", operation->requestId, turn.attempt);

  std::vector<ProviderConversationStateReceiptV1> receipts;
  std::set<std::string> seenRoles;
  for (const auto& record : records) {
    if (record.requestId != ndn::Name(operation->coreRequestId) ||
        record.keyScope != kConversationStateScope ||
        !ndn::Name(kConversationReceiptTopic).isPrefixOf(record.topic) ||
        record.producerRole.empty()) {
      throw NativeDiError("NATIVE_CONVERSATION_RECEIPT_BINDING", "conversation", "receipt",
        "conversation receipt transport binding mismatch", operation->requestId, turn.attempt);
    }
    ProviderConversationStateReceiptV1 receipt;
    try {
      receipt = parseConversationReceipt(nativeParseJson(
        std::string(record.payload.begin(), record.payload.end())));
    }
    catch (const std::exception&) {
      throw NativeDiError("NATIVE_CONVERSATION_RECEIPT_INVALID", "conversation", "receipt",
        "authenticated conversation receipt is malformed", operation->requestId, turn.attempt);
    }
    const auto assigned = planned.sealed.core.assignment.providerByRole.find(receipt.roleName);
    if (assigned == planned.sealed.core.assignment.providerByRole.end() ||
        !seenRoles.insert(receipt.roleName).second ||
        expectedRoles.count(receipt.roleName) == 0 ||
        record.producer != ndn::Name(receipt.providerIdentity) ||
        record.producerRole != receipt.roleName ||
        receipt.conversationId != turn.parent.conversationId ||
        receipt.parentContextEpoch != turn.parent.parentContextEpoch ||
        receipt.successorContextEpoch != turn.successorContextEpoch ||
        receipt.serviceName != turn.parent.serviceName ||
        receipt.planRoleMapDigest != turn.parent.planRoleMapDigest ||
        receipt.originRequestId != turn.executionRequestId ||
        receipt.originGenerationId != turn.parent.generationId ||
        receipt.requesterIdentity != operation->runtime->requesterIdentity ||
        receipt.expiresAtMs <= static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) ||
        assigned->second != receipt.providerIdentity) {
      throw NativeDiError("NATIVE_CONVERSATION_RECEIPT_BINDING", "conversation", "receipt",
        "conversation receipt identity does not match the sealed placement", operation->requestId,
        turn.attempt);
    }
    receipts.push_back(std::move(receipt));
  }
  if (seenRoles != expectedRoles)
    throw NativeDiError("NATIVE_CONVERSATION_RECEIPTS_INCOMPLETE", "conversation", "receipt",
      "authenticated conversation receipt roles are incomplete", operation->requestId, turn.attempt);
  std::sort(receipts.begin(), receipts.end(),
    [](const auto& left, const auto& right) { return left.roleName < right.roleName; });
  return receipts;
}

void publishConversationControls(
  const std::shared_ptr<NativeInferenceHandle::Operation>& operation,
  const NativeConversationTurn& turn,
  const std::vector<ProviderConversationStateReceiptV1>& receipts,
  const char* action,
  const std::string& checkpointDigest,
  std::uint64_t expiresAtMs)
{
  for (const auto& receipt : receipts) {
    const auto payload = nativeCanonicalJson(conversationControlJson(
      action, turn, receipt, checkpointDigest, expiresAtMs));
    if (!operation->user->publishCollaborationData(
          ndn::Name(receipt.providerIdentity), ndn::Name(operation->coreRequestId),
          kConversationStateScope, ndn::Name(kConversationControlTopic),
          ndn::Buffer(payload.begin(), payload.end()))) {
      throw NativeDiError("NATIVE_CONVERSATION_CONTROL_FAILED", "conversation", "control",
        "conversation control publication failed", operation->requestId, turn.attempt);
    }
  }
}

void waitConversationCommitAcks(
  const std::shared_ptr<NativeInferenceHandle::Operation>& operation,
  const NativeConversationTurn& turn,
  const std::vector<ProviderConversationStateReceiptV1>& receipts,
  const NativePlannedRequest& planned,
  const std::string& checkpointDigest)
{
  const auto records = operation->user->waitForVerifiedCollaborationData(
    ndn::Name(operation->coreRequestId), kConversationStateScope,
    ndn::Name(kConversationCommitTopic), receipts.size(),
    conversationWaitBudgetMs(operation), true);
  if (records.size() != receipts.size())
    throw NativeDiError("NATIVE_CONVERSATION_COMMIT_ACK_INCOMPLETE", "conversation", "commit",
      "conversation Provider commit acknowledgement set is incomplete", operation->requestId,
      turn.attempt);
  std::set<std::string> seen;
  for (const auto& record : records) {
    NativeJson value;
    try { value = nativeParseJson(std::string(record.payload.begin(), record.payload.end())); }
    catch (...) {
      throw NativeDiError("NATIVE_CONVERSATION_COMMIT_ACK_INVALID", "conversation", "commit",
        "conversation Provider commit acknowledgement is malformed", operation->requestId, turn.attempt);
    }
    static const std::set<std::string> fields = {
      "schema", "requestId", "attemptEpoch", "generationId", "planDigest",
      "conversationId", "parentContextEpoch", "successorContextEpoch", "serviceName",
      "planRoleMapDigest", "roleName", "receiptDigest", "checkpointDigest",
      "providerIdentity", "providerBootId", "cacheEpoch", "committed",
    };
    std::set<std::string> seenFields;
    if (!value.is_object()) {
      throw NativeDiError("NATIVE_CONVERSATION_COMMIT_ACK_INVALID", "conversation", "commit",
        "conversation Provider commit acknowledgement is not an object", operation->requestId, turn.attempt);
    }
    for (const auto& item : value.items()) seenFields.insert(item.key());
    const auto role = value.value("roleName", std::string{});
    const auto provider = value.value("providerIdentity", std::string{});
    const auto receipt = std::find_if(receipts.begin(), receipts.end(),
      [&](const auto& item) { return item.roleName == role; });
    const auto assigned = planned.sealed.core.assignment.providerByRole.find(role);
    if (seenFields != fields || receipt == receipts.end() || assigned == planned.sealed.core.assignment.providerByRole.end() ||
        !seen.insert(role).second || record.requestId != ndn::Name(operation->coreRequestId) ||
        record.keyScope != kConversationStateScope || record.producer != ndn::Name(provider) ||
        record.producerRole != role || provider != receipt->providerIdentity ||
        value.value("schema", std::string{}) != "ndnsf-di-provider-conversation-commit-ack-v1" ||
        value.value("requestId", std::string{}) != operation->coreRequestId ||
        value.value("attemptEpoch", std::uint64_t{0}) != turn.attempt ||
        value.value("generationId", std::string{}) != turn.parent.generationId ||
        value.value("planDigest", std::string{}) != planned.sealed.planDigest ||
        value.value("conversationId", std::string{}) != turn.parent.conversationId ||
        value.value("parentContextEpoch", std::uint64_t{0}) != turn.parent.parentContextEpoch ||
        value.value("successorContextEpoch", std::uint64_t{0}) != turn.successorContextEpoch ||
        value.value("serviceName", std::string{}) != turn.parent.serviceName ||
        value.value("planRoleMapDigest", std::string{}) != turn.parent.planRoleMapDigest ||
        value.value("receiptDigest", std::string{}) != receipt->computedDigest() ||
        value.value("checkpointDigest", std::string{}) != checkpointDigest ||
        value.value("providerBootId", std::string{}) != receipt->providerBootId ||
        value.value("cacheEpoch", std::uint64_t{0}) != receipt->cacheEpoch ||
        value.value("committed", false) != true) {
      throw NativeDiError("NATIVE_CONVERSATION_COMMIT_ACK_BINDING", "conversation", "commit",
        "conversation Provider commit acknowledgement identity mismatch", operation->requestId, turn.attempt);
    }
  }
  if (seen.size() != receipts.size())
    throw NativeDiError("NATIVE_CONVERSATION_COMMIT_ACK_INCOMPLETE", "conversation", "commit",
      "conversation Provider commit acknowledgement roles are incomplete", operation->requestId, turn.attempt);
}

void commitConversationTurn(
  const std::shared_ptr<NativeInferenceHandle::Operation>& operation)
{
  NativeConversationTurn turn;
  NativePlannedRequest planned;
  std::vector<std::int64_t> accepted;
  std::string generationId;
  std::string tokenizerDigest;
  std::string chatTemplateDigest;
  std::string applicationMessages;
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (!operation->conversationTurn || !operation->conversations ||
        !operation->planned || operation->status != NativeRequestStatus::Pending)
      throw NativeDiError("NATIVE_CONVERSATION_TURN_UNAVAILABLE", "conversation", "commit",
        "conversation turn is unavailable for completion", operation->requestId, operation->attempt);
    turn = *operation->conversationTurn;
    planned = *operation->planned;
    accepted = operation->acceptedTokenIds;
    generationId = operation->generationId;
    if (!operation->runtime ||
        operation->runtime->contract.tokenizerDigest.empty() ||
        !operation->options.generation ||
        operation->options.generation->tokenizerDigest !=
          operation->runtime->contract.tokenizerDigest) {
      throw NativeDiError(
        "TOKENIZER_DIGEST_MISMATCH", "conversation", "commit",
        "conversation tokenizer digest is not bound to the operator runtime contract",
        operation->requestId, operation->attempt);
    }
    // The operator-pinned runtime contract is the sole tokenizer authority.
    // Model semantics identify graph/chat behavior and must never substitute
    // for tokenizer identity in a persisted conversation transcript.
    tokenizerDigest = operation->runtime->contract.tokenizerDigest;
    applicationMessages.assign(operation->input.payload.begin(), operation->input.payload.end());
    operation->conversationTransactionActive = true;
  }
  ConversationTransactionGuard transactionGuard{operation};
  chatTemplateDigest = operation->model.semanticsDigest;
  try {
    const auto options = nativeParseJson(std::string(operation->input.options.begin(), operation->input.options.end()));
    if (options.is_object()) {
      chatTemplateDigest = options.value("chatTemplateDigest",
        options.value("chat_template_digest", chatTemplateDigest));
    }
  }
  catch (...) {
    // The request envelope already authenticated options bytes; missing JSON
    // metadata simply uses the model's pinned semantic digest.
  }
  const auto receipts = collectConversationReceipts(operation, turn, planned);
  const auto committedCheckpointDigest = std::make_shared<std::string>();
  NativeCompletedAttempt completed;
  completed.requestId = turn.executionRequestId;
  completed.attempt = turn.attempt;
  completed.tokenIds = turn.parent.canonicalTokenIds;
  completed.tokenIds.insert(completed.tokenIds.end(), accepted.begin(), accepted.end());
  completed.complete = true;
  completed.generationId = generationId;
  completed.modelContractDigest = operation->model.intentDigest();
  completed.tokenizerDigest = tokenizerDigest;
  completed.chatTemplateDigest = chatTemplateDigest;
  completed.applicationMessages = std::move(applicationMessages);
  for (const auto& receipt : receipts) {
    completed.authenticatedReceipts.push_back(nativeParseJson(receipt.toJson()));
  }
  completed.commitProviderState = [operation, turn, receipts, planned, committedCheckpointDigest](const std::string& checkpointDigest) {
    *committedCheckpointDigest = checkpointDigest;
    const auto expiresAt = std::min_element(receipts.begin(), receipts.end(),
      [](const auto& left, const auto& right) { return left.expiresAtMs < right.expiresAtMs; })->expiresAtMs;
    publishConversationControls(operation, turn, receipts, "COMMIT", checkpointDigest, expiresAt);
    waitConversationCommitAcks(operation, turn, receipts, planned, checkpointDigest);
  };
  completed.rollbackProviderState = [operation, turn, receipts, committedCheckpointDigest] {
    try {
      if (committedCheckpointDigest->empty()) return;
      const auto expiresAt = std::min_element(receipts.begin(), receipts.end(),
        [](const auto& left, const auto& right) { return left.expiresAtMs < right.expiresAtMs; })->expiresAtMs;
      publishConversationControls(operation, turn, receipts, "ROLLBACK", *committedCheckpointDigest, expiresAt);
    }
    catch (...) {}
  };
  completed.durableCommitGate = [operation](const std::function<void()>& publish) {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (operation->status != NativeRequestStatus::Pending ||
        std::chrono::steady_clock::now() >= operation->deadline)
      throw std::runtime_error("conversation durable commit fenced by terminal request");
    publish();
    operation->conversationCommitted = true;
  };
  completed.finalizeProviderState = [operation, turn, receipts, committedCheckpointDigest] {
    try {
      if (committedCheckpointDigest->empty()) return;
      const auto expiresAt = std::min_element(receipts.begin(), receipts.end(),
        [](const auto& left, const auto& right) { return left.expiresAtMs < right.expiresAtMs; })->expiresAtMs;
      publishConversationControls(operation, turn, receipts, "FINALIZE", *committedCheckpointDigest, expiresAt);
    }
    catch (...) {}
  };
  const auto checkpoint = operation->conversations->prepareCheckpoint(turn, completed);
  operation->conversations->commitTurn(turn, checkpoint);
}

void beginCoreRequest(const std::shared_ptr<NativeInferenceHandle::Operation>& operation);

bool beginReplacement(const std::shared_ptr<NativeInferenceHandle::Operation>& operation,
                      std::uint64_t sourceAttempt,
                      const ndn_service_framework::StreamedInvocationError& error)
{
  using Code = ndn_service_framework::StreamedInvocationErrorCode;
  if (error.code != Code::EventTimeout && error.code != Code::EventOutsideRetention &&
      error.code != Code::ProviderFailure) return false;
  NativeGenerationRecovery recovery;
  NativeApplicationInput input;
  std::vector<std::string> oldScopes;
  std::string oldRequest;
  std::optional<NativeConversationTurn> oldConversationTurn;
  std::optional<NativeConversationTurn> replacementConversationTurn;
  std::shared_ptr<NativeConversationCoordinator> conversations;
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (operation->status != NativeRequestStatus::Pending || sourceAttempt != 1 ||
        operation->attempt != sourceAttempt || operation->replacementStarted ||
        operation->acceptedTerminalHint != "NONE" || !operation->options.generation ||
        !operation->options.stream || !operation->options.stream->allowReplacement ||
        operation->options.stream->maxReplacements != 1 || !operation->planned ||
        !operation->preparedInput || !operation->encodedRequest || error.providerName.empty() ||
        error.requestId != ndn::Name(operation->coreRequestId) ||
        !operation->planned->sealed.core.offerDigestByProvider.count(error.providerName.toUri()) ||
        std::chrono::steady_clock::now() + std::chrono::milliseconds(operation->options.ackTimeoutMs) >= operation->deadline)
      return false;
    oldRequest = operation->coreRequestId;
    oldConversationTurn = operation->conversationTurn;
    conversations = operation->conversations;
    recovery = {operation->generationId, oldRequest,
      operation->requestId + "/recovery/" + std::to_string(NEXT_REQUEST_ID.fetch_add(1)),
      operation->encodedRequest->inputManifestDigest, operation->planned->sealed.planDigest,
      error.providerName.toUri(), operation->acceptedTokenIds};
    input = operation->input;
    input.payload = operation->preparedInput->payload;
    input.taskName = operation->preparedInput->taskName;
    for (const auto& scope : operation->planned->corePlan.keyScopes) oldScopes.push_back(scope.name);
  }
  auto encoded = encodeNativeRequestEnvelope(operation->model, input, *operation->requestContract,
    recovery.recoveryRequestId, 2, operation->wireDeadlineMs, recovery);
  if (oldConversationTurn && conversations) {
    try {
      replacementConversationTurn = conversations->replaceAttempt(
        *oldConversationTurn, recovery.recoveryRequestId, encoded.requestContractDigest);
    }
    catch (const std::exception&) {
      throw NativeDiError("NATIVE_CONVERSATION_REPLACEMENT_FAILED", "conversation", "replacement",
        "conversation turn replacement was rejected", operation->requestId, sourceAttempt);
    }
  }
  bool installedReplacement = false;
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (operation->status == NativeRequestStatus::Pending && operation->attempt == sourceAttempt &&
        !operation->replacementStarted) {
      operation->options.generation->committedPrefixTokenIds = recovery.committedTokenIds;
      operation->coreRequestId = recovery.recoveryRequestId;
      operation->recovery = std::move(recovery);
      operation->encodedRequest = std::move(encoded);
      operation->replacementStarted = true;
      operation->attempt = 2;
      if (replacementConversationTurn) {
        operation->conversationTurn = std::move(replacementConversationTurn);
      }
      operation->phase = DiRequestPhase::New;
      operation->planned.reset();
      operation->coreActive = false;
      operation->options.stream->attemptEpoch = operation->options.stream->streamEpoch = 2;
      operation->options.stream->allowReplacement = false;
      operation->options.stream->maxReplacements = 0;
      installedReplacement = true;
    }
  }
  if (!installedReplacement) {
    if (replacementConversationTurn && conversations) {
      const NativeDiError stale(
        "NATIVE_CONVERSATION_REPLACEMENT_CANCELLED", "conversation", "replacement",
        "conversation replacement became terminal before installation", operation->requestId,
        sourceAttempt);
      conversations->abortTurn(*replacementConversationTurn, stale);
    }
    return false;
  }
  const auto user = operation->user;
  user->postToIo([user, oldRequest, oldScopes = std::move(oldScopes)] {
    const ndn::Name id(oldRequest);
    user->CancelCollaboration(id);
    for (const auto& scope : oldScopes) user->clearVerifiedCollaborationData(id, scope);
  });
  beginCoreRequest(operation);
  return true;
}

void beginCoreRequest(const std::shared_ptr<NativeInferenceHandle::Operation>& operation)
{
  const auto sourceAttempt = operation->attempt;
  const auto coreRequestId = operation->coreRequestId;
  operation->user->postToIo([operation, sourceAttempt, coreRequestId] {
    try {
      {
        std::lock_guard<std::mutex> lock(operation->mutex);
        if (operation->status != NativeRequestStatus::Pending || operation->attempt != sourceAttempt) return;
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
      const auto ackClosed = [operation, sourceAttempt, coreRequestId](const ndn_service_framework::CollaborationAckClosure& closure) {
        enqueueOperation(operation, [operation, sourceAttempt, coreRequestId, closure] {
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            if (operation->status != NativeRequestStatus::Pending || operation->attempt != sourceAttempt ||
                operation->phase != DiRequestPhase::Requesting) return;
            operation->phase = DiRequestPhase::Planning;
          }
          NativeRequestControl control{coreRequestId, sourceAttempt, operation->deadline,
            [flag = operation->cancelled] { return flag->load(); }};
          auto planned = planNativeRequest(*operation->runtime, operation->coreOptions,
            *operation->inspected, *operation->encodedRequest, *operation->splitStrategy,
            *operation->placementStrategy, *operation->preparation, *operation->admission,
            closure, control, operation->wireDeadlineMs, operation->cancelled,
            operation->conversationTurn ? &*operation->conversationTurn : nullptr);
          if (operation->conversationTurn && operation->conversations &&
              operation->conversationTurn->attempt == 2) {
            operation->conversationTurn = operation->conversations->bindAttemptPlanRoleMap(
              *operation->conversationTurn, planned.sealed.core.assignment.providerByRole);
          }
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            if (operation->status != NativeRequestStatus::Pending) return;
            operation->planned = std::move(planned);
          }
          operation->user->postToIo([operation, sourceAttempt, coreRequestId, digest = closure.digest,
                                    plan = operation->planned->corePlan] {
            try {
              {
                std::lock_guard<std::mutex> lock(operation->mutex);
                if (operation->status != NativeRequestStatus::Pending || operation->attempt != sourceAttempt) return;
              }
              if (std::chrono::steady_clock::now() >= operation->deadline)
                throw NativeDiError("NATIVE_REQUEST_TIMEOUT", "local", "commit",
                  "request expired before commit", operation->requestId, operation->attempt);
              if (!operation->user->CommitCollaborationPlan(ndn::Name(coreRequestId), digest, plan))
                throw std::runtime_error("Core rejected the sealed plan");
              std::lock_guard<std::mutex> lock(operation->mutex);
              if (operation->status == NativeRequestStatus::Pending && operation->attempt == sourceAttempt)
                operation->phase = DiRequestPhase::Committed;
            }
            catch (const NativeDiError& error) { failOperation(operation, error, sourceAttempt); }
            catch (...) { failOperation(operation, NativeDiError(
              "NATIVE_REQUEST_COMMIT_FAILED", "runtime", "commit", "Core plan commit failed",
              operation->requestId, sourceAttempt), sourceAttempt); }
          });
        }, "ACK_CLOSED");
      };
      const auto response = [operation](const ndn_service_framework::ResponseMessage& message) {
        // Core's stream completion callback owns final acceptance; an ordinary
        // Response must never bypass the accepted token/text transcript gate.
        if (operation->options.stream) return;
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
      const auto timeout = [operation, sourceAttempt](const ndn::Name&) {
        enqueueOperation(operation, [operation, sourceAttempt] {
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            if (operation->status != NativeRequestStatus::Pending || operation->attempt != sourceAttempt) return;
          }
          throw NativeDiError("NATIVE_REQUEST_TIMEOUT", "local", "Core",
            "Core request deadline expired", operation->requestId, sourceAttempt);
        }, "Core-timeout");
      };
      ndn_service_framework::RequestCapabilities capabilities;
      capabilities.setField("NDNSF_DATA_V1", "required");
      const auto streamEvent = [operation, sourceAttempt](const ndn::Buffer& bytes) {
        bool overflow = false;
        {
          std::lock_guard<std::mutex> lock(operation->mutex);
          if (operation->status != NativeRequestStatus::Pending || operation->attempt != sourceAttempt) {
            ++operation->staleCallbacks;
            return;
          }
          const auto capacity = operation->options.stream->callbackQueueCapacity;
          overflow = operation->queuedStreamEvents >= capacity || bytes.size() > (1U << 20);
          if (!overflow) ++operation->queuedStreamEvents;
        }
        if (overflow) {
          failOperation(operation, NativeDiError("StreamDeliveryOverflow", "local", "stream-accept",
            "stream callback queue capacity exceeded", operation->requestId, sourceAttempt));
          return;
        }
        enqueueOperation(operation, [operation, sourceAttempt, payload = std::vector<std::uint8_t>(bytes.begin(), bytes.end())] {
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            --operation->queuedStreamEvents;
          }
          if (operation->options.generation && !operation->acceptGenerationEvent(sourceAttempt, payload)) return;
          if (operation->cancelled->load()) return;
          NativeInferenceEvent observed;
          observed.requestId = operation->requestId;
          observed.payload = payload;
          observed.terminal = false;
          publishEvent(operation, std::move(observed), true);
          try {
            if (operation->options.onGenerationEvent) operation->options.onGenerationEvent(payload);
          }
          catch (...) {
            throw NativeDiError("StreamCallbackFailed", "local", "stream-callback",
              "application stream callback failed", operation->requestId, sourceAttempt);
          }
        }, "stream-event");
      };
      const auto streamComplete = [operation, sourceAttempt](const ndn::Buffer& bytes) {
        enqueueOperation(operation, [operation, sourceAttempt, payload = std::vector<std::uint8_t>(bytes.begin(), bytes.end())] {
          if (operation->options.generation && !operation->validateGenerationFinal(sourceAttempt, payload)) return;
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            if (operation->status != NativeRequestStatus::Pending || operation->attempt != sourceAttempt) return;
            if (!operation->planned) throw std::runtime_error("stream final preceded sealed plan");
          }
          NativeInferenceResult result;
          result.payload = operation->adapters->find(operation->model.adapterId)->decodeResult(payload);
          result.modelDigest = operation->encodedRequest->modelIntentDigest;
          result.planDigest = operation->planned->sealed.planDigest;
          if (std::chrono::steady_clock::now() >= operation->deadline)
            throw NativeDiError("NATIVE_REQUEST_TIMEOUT", "local", "stream-final",
              "request expired during stream result decoding", operation->requestId, sourceAttempt);
          bool hasConversationTurn = false;
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            hasConversationTurn = operation->conversationTurn.has_value();
          }
          if (hasConversationTurn) {
            commitConversationTurn(operation);
          }
          if (markTerminal(operation, NativeRequestStatus::Succeeded, nullptr, &result))
            publishEvent(operation, makeTerminalEvent(*operation));
        }, "stream-final");
      };
      const auto streamError = [operation, sourceAttempt](const ndn_service_framework::StreamedInvocationError& error) {
        enqueueOperation(operation, [operation, sourceAttempt, error] {
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            if (operation->status != NativeRequestStatus::Pending || operation->attempt != sourceAttempt) {
              ++operation->staleCallbacks;
              return;
            }
          }
          if (beginReplacement(operation, sourceAttempt, error)) return;
          throw NativeDiError("NATIVE_STREAM_FAILED", "provider", "stream",
            std::string("Core stream failed: ") + error.message,
            operation->requestId, sourceAttempt);
        }, "stream-error");
      };
      operation->user->BeginCollaboration(ndn::Name(operation->runtime->contract.serviceName),
        ndn::Buffer(operation->encodedRequest->wire.begin(), operation->encodedRequest->wire.end()),
        static_cast<int>(operation->coreOptions.ackTimeoutMs),
        static_cast<int>(operation->coreOptions.timeoutMs), ackClosed, response, timeout,
        ndn::Name(coreRequestId), {}, capabilities, operation->options.stream,
        streamEvent, streamComplete, streamError);
    }
    catch (const NativeDiError& error) { failOperation(operation, error, sourceAttempt); }
    catch (...) { failOperation(operation, NativeDiError(
      "NATIVE_REQUEST_BEGIN_FAILED", "runtime", "begin", "Core request start failed",
      operation->requestId, sourceAttempt), sourceAttempt); }
  });
}

// Serial dispatch driver, run on the executor worker or the unit-test pump.
// The absolute per-request deadline is checked before any stage work: work
// that can no longer finish inside the request budget is refused and the
// operation fails once with NATIVE_REQUEST_TIMEOUT.  Orchestration stages
// (CD-013 preparation, Core BeginCollaboration, planning, commit) link into
// this driver when a complete NativeRequestRuntime is configured.  The
// constructor used by compatibility/component tests has no runtime and keeps
// the structured not-ready failure instead of claiming a synthetic success.
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
      if ((input.transportMode == NativeInputTransportMode::Inline &&
           !input.repositoryReference.empty()) ||
          (input.transportMode == NativeInputTransportMode::RepositoryReference &&
           input.repositoryReference.empty()))
        throw std::invalid_argument("native input transport and reference disagree");
      if (input.transportMode != NativeInputTransportMode::Inline &&
          input.transportMode != NativeInputTransportMode::RepositoryReference)
        throw std::invalid_argument("native input transport is invalid");
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
        *operation->requestContract, operation->coreRequestId, operation->attempt,
        operation->wireDeadlineMs, operation->recovery);
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
  if (operation->options.conversation) {
    if (!operation->runtime || !operation->conversations || !operation->options.generation ||
        !operation->options.stream) {
      failOperation(operation, NativeDiError(
        "INVALID_CONVERSATION_OPTIONS", "conversation", "request",
        "conversation requests require a native runtime, coordinator and authenticated streaming",
        operation->requestId, operation->attempt));
      return;
    }
    try {
      auto continuation = *operation->options.conversation;
      // The request-contract digest covers the complete envelope, including
      // the request ID allocated by this native owner. Callers cannot know
      // that ID before submission; an empty value is therefore filled from
      // the encoded envelope at this ownership boundary. A supplied value is
      // retained and remains subject to the normal coordinator/planner
      // equality checks.
      if (continuation.requestContractDigest.empty()) {
        if (!operation->encodedRequest || operation->encodedRequest->requestContractDigest.empty()) {
          failOperation(operation, NativeDiError(
            "NATIVE_CONVERSATION_CONTRACT_UNAVAILABLE", "conversation", "begin",
            "encoded request contract digest is unavailable", operation->requestId,
            operation->attempt));
          return;
        }
        continuation.requestContractDigest = operation->encodedRequest->requestContractDigest;
      }
      auto turn = operation->conversations->beginTurn(
        continuation, operation->coreRequestId, operation->attempt);
      bool installed = false;
      {
        std::lock_guard<std::mutex> lock(operation->mutex);
        if (operation->status == NativeRequestStatus::Pending) {
          operation->conversationTurn = turn;
          installed = true;
        }
      }
      if (!installed) {
        const NativeDiError stale(
          "NATIVE_CONVERSATION_BEGIN_CANCELLED", "conversation", "begin",
          "conversation turn became terminal before installation", operation->requestId,
          operation->attempt);
        operation->conversations->abortTurn(turn, stale);
        return;
      }
    }
    catch (const NativeDiError&) { throw; }
    catch (const std::exception&) {
      failOperation(operation, NativeDiError(
        "NATIVE_CONVERSATION_BEGIN_FAILED", "conversation", "begin",
        "native conversation turn could not be opened", operation->requestId,
        operation->attempt));
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

std::string NativeInferenceHandle::applicationRequestId() const
{
  if (!m_operation) return {};
  std::lock_guard<std::mutex> lock(m_operation->mutex);
  return m_operation->applicationRequestId;
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
      contract.serviceName.front() != '/' || contract.taskName.empty() ||
      !isSupportedNativeGenerationMode(contract.generationMode))
    throw NativeDiError("INVALID_CLIENT_CONFIGURATION", "local", "constructor",
                        "native requester contract has unsupported generation mode or missing service/task/owners");
  m_requestContract = std::make_shared<const NativeRequestContract>(contract);
}

NativeInferenceClient::NativeInferenceClient(
  std::shared_ptr<ndn_service_framework::ServiceUser> user,
  std::shared_ptr<const NativeAdapterRegistry> adapters,
  const NativeRequestRuntime& runtime,
  std::shared_ptr<NativeRequestPreparation> preparation,
  std::shared_ptr<const NativeOfferAdmission> admission)
  : NativeInferenceClient(std::move(user), std::move(adapters), runtime, nullptr,
                          std::move(preparation), std::move(admission))
{
}

NativeInferenceClient::NativeInferenceClient(
  std::shared_ptr<ndn_service_framework::ServiceUser> user,
  std::shared_ptr<const NativeAdapterRegistry> adapters,
  const NativeRequestRuntime& runtime,
  std::shared_ptr<NativeConversationCoordinator> conversations,
  std::shared_ptr<NativeRequestPreparation> preparation,
  std::shared_ptr<const NativeOfferAdmission> admission)
  : NativeInferenceClient(std::move(user), std::move(adapters), runtime.contract,
                          std::move(preparation), std::move(admission))
{
  runtime.budget.validate();
  if (!runtime.grants || !runtime.security.requireProtectedArtifacts || runtime.requesterIdentity.empty() ||
      runtime.protectionEpoch.empty() || runtime.protectionEpoch == "plaintext-v1" ||
      !runtime.maxSegments || !runtime.noProgressMs ||
      runtime.requesterIdentity != runtime.grants->requesterIdentity() ||
      runtime.protectionEpoch != runtime.grants->protectionEpoch())
    throw NativeDiError("INVALID_CLIENT_CONFIGURATION", "local", "constructor",
      "native requester requires protected runtime policy and grant owner");
  m_runtime = std::make_shared<const NativeRequestRuntime>(runtime);
  m_conversations = std::move(conversations);
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
        options.applicationRequestId.size() > 256 ||
        options.applicationRequestId.find('\0') != std::string::npos ||
        (input.payload.empty() && input.repositoryReference.empty()) ||
        (input.transportMode == NativeInputTransportMode::Inline &&
         (!input.repositoryReference.empty() || input.payload.empty())) ||
        (input.transportMode == NativeInputTransportMode::RepositoryReference &&
         (!input.payload.empty() || input.repositoryReference.empty()))) {
      throw NativeDiError("INVALID_REQUEST", "local", "request",
                          "native request arguments are invalid");
    }
    const NativeRequestContract* requestContract =
      m_runtime ? &m_runtime->contract : m_requestContract.get();
    if (requestContract) {
      const auto& contract = *requestContract;
      if (!contract.tokenizerDigest.empty() &&
          !decode_state_identity_detail::isSha256Digest(contract.tokenizerDigest))
        throw NativeDiError("INVALID_CLIENT_CONFIGURATION", "local", "request",
                            "native requester tokenizer digest is not canonical");
      if (contract.generationMode == "TOKEN_STREAMING" &&
          (contract.tokenizerDigest.empty() || !options.stream))
        throw NativeDiError("INVALID_STREAM_OPTIONS", "local", "request",
                            "TOKEN_STREAMING runtime requires pinned tokenizer digest and stream options");
      if (contract.generationMode != "TOKEN_STREAMING" && options.generation)
        throw NativeDiError("INVALID_GENERATION_OPTIONS", "local", "request",
                            "generation options require a TOKEN_STREAMING runtime contract");
    }
    if (options.generation && !options.stream)
      throw NativeDiError("INVALID_GENERATION_OPTIONS", "local", "request",
                          "generation options require stream options");
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
    operation->conversations = m_conversations;
    operation->worker = m_executor;
    operation->notifications = m_notifications;
    operation->applicationRequestId = options.applicationRequestId;
    // The requestId comes from a unique native owner allocated at submission
    // (runtime-boundaries: Core allocation or unique native owner); the
    // operation then binds ACK/plan/grant/result to this stable URI.
    operation->requestId = "/NDNSF/DI/REQUEST/" +
      std::to_string(NEXT_REQUEST_ID.fetch_add(1));
    operation->coreRequestId = operation->requestId;
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
    if (operation->options.stream) {
      auto& stream = *operation->options.stream;
      const bool generationRequested = operation->options.generation.has_value() ||
        (operation->requestContract && operation->requestContract->generationMode == "TOKEN_STREAMING");
      if (stream.mode != ndn_service_framework::InvocationMode::Normal || stream.attemptEpoch != 1)
        throw NativeDiError("INVALID_STREAM_OPTIONS", "local", "request",
                            "stream collaboration must start in Normal attempt 1");
      std::string requestedGenerationId = operation->options.generation ? operation->options.generation->generationId : "";
      if (generationRequested && requestedGenerationId.empty()) {
        const auto application = nativeParseJson(std::string(input.options.begin(), input.options.end()));
        requestedGenerationId = application.value("generationId", application.value("generation_id", std::string{}));
      }
      if (!requestedGenerationId.empty() &&
          (requestedGenerationId.size() != stream.generationId.size() * 2 ||
           requestedGenerationId.find_first_not_of("0123456789abcdef") != std::string::npos))
        throw NativeDiError("INVALID_GENERATION_OPTIONS", "local", "request",
                            "generation identity must be lowercase 16-byte hex");
      if (stream.generationId == ndn_service_framework::StreamGenerationId{} && !requestedGenerationId.empty()) {
        for (std::size_t i = 0; i < stream.generationId.size(); ++i)
          stream.generationId[i] = static_cast<std::uint8_t>(
            std::stoul(requestedGenerationId.substr(i * 2, 2), nullptr, 16));
      }
      if (stream.generationId == ndn_service_framework::StreamGenerationId{} &&
          RAND_bytes(stream.generationId.data(), stream.generationId.size()) != 1)
        throw NativeDiError("STREAM_ID_ALLOCATION_FAILED", "local", "request",
                            "cannot allocate generation identity");
      static constexpr char hex[] = "0123456789abcdef";
      for (const auto byte : stream.generationId) {
        operation->generationId += hex[byte >> 4];
        operation->generationId += hex[byte & 15];
      }
      if (!requestedGenerationId.empty() && requestedGenerationId != operation->generationId)
        throw NativeDiError("INVALID_GENERATION_OPTIONS", "local", "request",
                            "generation identity disagrees with stream identity");
      stream.streamEpoch = 1;
      stream.deadlineEpochMs = operation->wireDeadlineMs;
      if (generationRequested) {
        auto derived = nativeGenerationFromOptions(input.options, operation->generationId);
        if (operation->runtime &&
            !operation->runtime->contract.tokenizerDigest.empty() &&
            derived.tokenizerDigest != operation->runtime->contract.tokenizerDigest)
          throw NativeDiError("TOKENIZER_DIGEST_MISMATCH", "local", "request",
                              "generation tokenizer digest disagrees with operator-pinned runtime digest");
        if (operation->options.generation) {
          auto requested = *operation->options.generation;
          requested.generationId = operation->generationId;
          requested.streamingOperationStride = 0; // Placement, not the caller, assigns operation indices.
          if (!requested.enabled || nativeGenerationJson(requested) != nativeGenerationJson(derived))
            throw NativeDiError("INVALID_GENERATION_OPTIONS", "local", "request",
                                "explicit generation differs from bound application options");
        }
        operation->options.generation = std::move(derived);
        auto& generation = *operation->options.generation;
        if (!generation.enabled || !generation.maxGeneratedTokens ||
            generation.maxGeneratedTokens > (1U << 20) ||
            (!generation.generationId.empty() && generation.generationId != operation->generationId))
          throw NativeDiError("INVALID_GENERATION_OPTIONS", "local", "request",
                              "generation contract disagrees with stream identity or budget");
        generation.generationId = operation->generationId;
        operation->samplingDigest = generation.samplingDigest;
        operation->maxGenerationTokens = generation.maxGeneratedTokens;
      }
    }
    // Keep only live handles in the close-tracking index.  The index is weak
    // by design so a client does not own completed operations, but without
    // compaction every fire-and-forget request would leave an expired entry
    // until client shutdown.
    m_operations.erase(
      std::remove_if(m_operations.begin(), m_operations.end(),
                     [] (const auto& weak) { return weak.expired(); }),
      m_operations.end());
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
