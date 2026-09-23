#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGenerationLimits.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DecodeStateIdentity.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <openssl/rand.h>
#include <unistd.h>

namespace ndnsf::di {

namespace {

std::atomic<std::uint64_t> NEXT_REQUEST_ID{1};

std::string
processRequestOwnerScope()
{
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    // RAND_bytes is expected to be available after OpenSSL initialization;
    // retain a per-client uniqueness fallback if the provider is not
    // initialized yet.  The PID and monotonic clock make this distinct from
    // the deterministic unit-test scope without claiming cryptographic use.
    const auto tick = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
    const auto pid = static_cast<std::uint64_t>(::getpid());
    for (std::size_t i = 0; i < sizeof(tick); ++i)
      bytes[i] = static_cast<unsigned char>(tick >> (i * 8));
    for (std::size_t i = 0; i < sizeof(pid); ++i)
      bytes[sizeof(tick) + i] = static_cast<unsigned char>(pid >> (i * 8));
  }
  static constexpr char hex[] = "0123456789abcdef";
  std::string value;
  value.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    value += hex[byte >> 4];
    value += hex[byte & 0x0f];
  }
  return value;
}

constexpr char kConversationStateScope[] = "ndnsf-di-conversation-state-v1";
constexpr char kConversationReceiptTopic[] = "/ndnsf-di/conversation/receipt";
constexpr char kConversationCommitTopic[] = "/ndnsf-di/conversation/commit";
constexpr char kConversationRollbackTopic[] = "/ndnsf-di/conversation/rollback";
constexpr char kConversationControlTopic[] = "/ndnsf-di/conversation/control";

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

struct NativeIoCleanupState
{
  std::atomic<std::size_t> pending{0};
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::function<void()> notifyOwner;
};

void notifyIoCleanupSettled(const std::shared_ptr<NativeIoCleanupState>& state) noexcept
{
  if (!state)
    return;
  state->pending.fetch_sub(1, std::memory_order_acq_rel);
  state->condition.notify_all();
  std::function<void()> notifier;
  try {
    std::lock_guard<std::mutex> lock(state->mutex);
    notifier = state->notifyOwner;
  }
  catch (...) {
    return;
  }
  if (notifier) {
    try { notifier(); }
    catch (...) {}
  }
}

struct NativeInferenceHandle::Operation
{
  // The caller's Core owner survives asynchronous work and handle waits.
  // It does not transfer ownership of the application's Face to DI.
  std::shared_ptr<ndn_service_framework::ServiceUser> user;
  std::shared_ptr<void> ownerLease;
  // PreparedModel attaches its cache lease before exposing the handle.  Keep
  // that lease with the operation until terminal cleanup so refresh/eviction
  // cannot replace identities still used by asynchronous work.
  std::shared_ptr<void> packageLease;
  // Submission values are owned snapshots. Worker/callback work must never
  // read the caller's model/input/options or borrow the client's lifetime.
  NativeModelRef model;
  NativeApplicationInput input;
  NativeRequestOptions options;
  // Exactly one strategy-port bundle is captured by an operation.  The
  // concrete owner pointers below only keep the selected vtable alive for the
  // legacy/cooperative adapters; planning consumes the single type-erased
  // bundle and never maintains a second strategy state machine.
  NativeStrategyPorts strategies;
  std::shared_ptr<const NativeModelSplitStrategy> legacySplitter;
  std::shared_ptr<const NativePlacementStrategy> legacyPlacement;
  std::shared_ptr<const CooperativeModelSplitStrategy> cooperativeSplitter;
  std::shared_ptr<const CooperativePlacementStrategy> cooperativePlacement;
  bool cooperativeStrategies = false;
  std::shared_ptr<NativeRequestPreparation> preparation;
  std::optional<NativePreparedInput> preparedInput;
  std::shared_ptr<const NativeRequestContract> requestContract;
  std::uint64_t wireDeadlineMs = 0;
  std::optional<NativeEncodedRequest> encodedRequest;
  std::shared_ptr<ndn_service_framework::OperationRuntime> operationRuntime;
  std::shared_ptr<ndn_service_framework::OperationState<NativeInferenceResult,
                                                        NativeInferenceEvent>> coreState;
  ndn_service_framework::OperationSubscription deadlineSubscription;
  std::vector<ndn_service_framework::OperationSubscription> observerSubscriptions;
  std::shared_ptr<const NativeRequestRuntime> runtime;
  std::shared_ptr<const NativeOfferAdmission> admission;
  std::shared_ptr<const NativeAdapterRegistry> adapters;
  std::weak_ptr<NativeOperationRegistry> registry;
  std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<NativeIoCleanupState> ioCleanupState;
  NativeRequestOptions coreOptions;
  std::optional<NativeInspectedModel> inspected;
  std::optional<NativePlannedRequest> planned;
  std::shared_ptr<NativeConversationCoordinator> conversations;
  std::optional<NativeConversationTurn> conversationTurn;
  // Stored only after NativeConversationCoordinator::commitTurn returns.  It
  // is an opaque authenticated wire owned by the native coordinator; no
  // transcript or Provider state is copied into the handle.
  std::optional<std::string> conversationCheckpointWire;
  bool conversationCommitted = false;
  bool conversationTransactionActive = false;
  bool conversationCleanupDeferred = false;
  bool conversationCancelDeferred = false;
  bool coreActive = false;
  mutable std::mutex mutex;
  std::string requestId;
  std::string applicationRequestId;
  std::string coreRequestId; // Per-attempt transport identity; public requestId remains stable.
  std::optional<NativeGenerationRecovery> recovery;
  DiRequestPhase phase = DiRequestPhase::New;
  std::uint64_t attempt = 1;
  std::chrono::steady_clock::time_point deadline{};
  std::function<void()> cancelDeadline;
  std::function<void()> cancelBootstrapRetry;
  std::uint64_t staleCallbacks = 0;    // late/duplicate terminal attempts: counted, never resurrecting
  std::uint64_t deliveryOverflows = 0; // reliable stream publication failures (internal)
  std::uint64_t nextEventSequence = 0;

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
    const bool corePending = coreState && coreState->isPending();
    std::lock_guard<std::mutex> lock(mutex);
    if (!corePending || cancelled->load() ||
        phase == DiRequestPhase::Terminal || sourceAttempt != attempt) {
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
    logRuntimePhase(
      "di-cli", "tokenReceived", requestId, std::to_string(attempt),
      {{"executionRole", "requester"},
       {"conversationId", conversationTurn ? conversationTurn->parent.conversationId : "none"},
       {"contextEpoch", conversationTurn ?
          std::to_string(conversationTurn->parent.parentContextEpoch) : "none"},
       {"inferenceEpoch", std::to_string(attempt)},
       {"tokenIndex", std::to_string(acceptedTokenIds.size())}});
    return true;
  }

  bool validateGenerationFinal(std::uint64_t sourceAttempt,
                               const std::vector<std::uint8_t>& payload)
  {
    const bool corePending = coreState && coreState->isPending();
    std::lock_guard<std::mutex> lock(mutex);
    if (!corePending || cancelled->load() ||
        phase == DiRequestPhase::Terminal || sourceAttempt != attempt) {
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

void
logClientLifecyclePhase(const std::shared_ptr<NativeInferenceHandle::Operation>& operation,
                        const char* phase)
{
  if (!operation || !phase || operation->requestId.empty()) return;
  std::string conversationId = "none";
  std::string contextEpoch = "none";
  std::uint64_t attempt = 1;
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    attempt = operation->attempt;
    if (operation->conversationTurn) {
      conversationId = operation->conversationTurn->parent.conversationId;
      contextEpoch = std::to_string(operation->conversationTurn->successorContextEpoch);
    }
  }
  logRuntimePhase(
    "di-cli", phase, operation->requestId, std::to_string(attempt),
    {{"executionRole", "requester"},
     {"conversationId", conversationId},
     {"contextEpoch", contextEpoch},
     {"inferenceEpoch", std::to_string(attempt)}});
}

// The client must retain every pending operation until it reaches a terminal
// state, even when the caller drops its public handle.  A weak-only client
// index lets close() miss such an operation while an executor task still owns
// it, so the request can begin after the client has been closed.  The registry
// owns pending operations strongly and is referenced weakly by each operation;
// terminal completion removes the entry without retaining completed results.
struct NativeOperationRegistry
{
  std::mutex mutex;
  std::unordered_map<NativeInferenceHandle::Operation*,
                     std::shared_ptr<NativeInferenceHandle::Operation>> pending;
};

namespace {

void
unregisterOperation(const std::shared_ptr<NativeInferenceHandle::Operation>& operation) noexcept
{
  if (auto registry = operation->registry.lock()) {
    std::lock_guard<std::mutex> lock(registry->mutex);
    registry->pending.erase(operation.get());
  }
}

NativeInferenceEvent
makeTerminalEvent(const NativeInferenceHandle::Operation& operation)
{
  NativeInferenceEvent event;
  event.requestId = operation.requestId;
  event.terminal = true;
  return event;
}

void
publishEvent(const std::shared_ptr<NativeInferenceHandle::Operation>& operation,
             NativeInferenceEvent event, bool requirePending = false);

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
             std::uint64_t expectedAttempt = 0,
             bool inlineCoreCleanup = false)
{
  std::function<void()> cancelDeadline;
  std::function<void()> cancelBootstrapRetry;
  bool cancelCore = false;
  std::vector<std::string> releaseScopes;
  std::shared_ptr<NativeConversationCoordinator> conversations;
  std::optional<NativeConversationTurn> conversationTurn;
  bool conversationCommitted = false;
  bool deferConversationCleanup = false;
  std::function<void()> conversationTerminal;
  bool won = false;
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (operation->cancelled->load() ||
        (expectedAttempt && operation->attempt != expectedAttempt)) {
      ++operation->staleCallbacks;
      return false;
    }
    // Once the coordinator's durable commit gate has published the
    // checkpoint, that decision wins over a concurrent cancel/deadline or
    // late failure callback.  The stream-final worker still owns the
    // succeeding terminal transition; refusing the competing terminal here
    // prevents a committed parent from being reported as an ordinary failed
    // or cancelled handle.
    if (operation->conversationCommitted && terminal != NativeRequestStatus::Succeeded) {
      ++operation->staleCallbacks;
      return false;
    }
    if (!operation->coreState ||
        (terminal == NativeRequestStatus::Succeeded && !result)) {
      ++operation->staleCallbacks;
      return false;
    }
    if (!error && terminal != NativeRequestStatus::Succeeded)
      error = std::make_shared<NativeDiError>(
        terminal == NativeRequestStatus::Cancelled ? "CANCELLED" : "NATIVE_REQUEST_FAILED",
        "runtime", "request",
        terminal == NativeRequestStatus::Cancelled ? "native request was cancelled" :
          "native request failed",
        operation->requestId, operation->attempt);
    // Claim the DI terminal transition before releasing this mutex, so a
    // concurrent cancel/deadline cannot enter Core and a synchronous test
    // submit hook cannot re-enter markTerminal while the lock is held.
    operation->cancelled->store(true);
    cancelCore = operation->coreActive;
    conversations = operation->conversations;
    conversationTurn = operation->conversationTurn;
    conversationCommitted = operation->conversationCommitted;
    if (operation->options.conversation && operation->options.conversation->onTerminal)
      conversationTerminal = operation->options.conversation->onTerminal;
    deferConversationCleanup = operation->conversationTransactionActive;
    if (deferConversationCleanup) {
      if (inlineCoreCleanup) {
        // The Runtime Face is already in its failure boundary.  Do not leave
        // a deferred conversation cleanup queued behind the callback that
        // just failed; the inline path below owns it and the guard will see
        // both deferred flags cleared when it later unwinds.
        operation->conversationCleanupDeferred = false;
        operation->conversationCancelDeferred = false;
        deferConversationCleanup = false;
      }
      else {
        operation->conversationCleanupDeferred = true;
        if (cancelCore) operation->conversationCancelDeferred = true;
      }
    }
    if (operation->planned) {
      for (const auto& scope : operation->planned->corePlan.keyScopes) releaseScopes.push_back(scope.name);
    }
    operation->phase = DiRequestPhase::Terminal;
    cancelDeadline = std::move(operation->cancelDeadline);
    cancelBootstrapRetry = std::move(operation->cancelBootstrapRetry);
  }
  // Conversation rollback is part of the internal terminal transition. It
  // must finish before either the event reader or result waiter can observe a
  // terminal request; otherwise a caller can submit the next turn while the
  // coordinator still owns the previous transaction.
  if (conversations && conversationTurn && !conversationCommitted) {
    try {
      const NativeDiError cancelledError(
        terminal == NativeRequestStatus::Cancelled ? "CANCELLED" : "NATIVE_REQUEST_FAILED",
        "conversation", "terminal", "native conversation turn terminated",
        operation->requestId, conversationTurn->attempt);
      conversations->abortTurn(*conversationTurn, cancelledError);
    }
    catch (...) {
      // Preserve the terminal request outcome. The lifecycle hook below is
      // still best-effort; close/drain remains the owner of any failed retry.
    }
  }
  if (conversationTerminal) {
    try { conversationTerminal(); }
    catch (...) {}
    if (terminal == NativeRequestStatus::Succeeded && conversationTurn) {
      logRuntimePhase(
        "di-cli", "turnReady", operation->requestId,
        std::to_string(conversationTurn->attempt),
        {{"executionRole", "requester"},
         {"conversationId", conversationTurn->parent.conversationId},
         {"inferenceEpoch", std::to_string(conversationTurn->attempt)},
         {"contextEpoch", std::to_string(conversationTurn->successorContextEpoch)}});
    }
  }
  // Publish the terminal frame before completing the Core state. Core wakes a
  // pending reader during complete/fail; publishing first prevents an EOF
  // callback from racing ahead of the terminal observation. Event history or
  // observer bookkeeping is best-effort at this boundary: an allocation or
  // copy exception must never strand the operation before Core owns a result.
  try {
    publishEvent(operation, makeTerminalEvent(*operation));
  }
  catch (...) {
    {
      std::lock_guard<std::mutex> lock(operation->mutex);
      ++operation->deliveryOverflows;
    }
    // The reliable stream is an observation channel.  A terminal frame
    // allocation/insertion failure must make that channel explicitly gapped,
    // while Core still owns the authoritative business result/status below.
    operation->coreState->failReader(
      ndn_service_framework::OperationErrorCode::EventGap,
      "terminal event delivery failed");
  }
  // Core callbacks are dispatched after the DI lock is released.  Production
  // runtimes are asynchronous, but test ports may invoke a submit hook inline;
  // the lock-free boundary is required for both modes.
  try {
    if (terminal == NativeRequestStatus::Succeeded)
      won = operation->coreState->complete(*result);
    else
      won = operation->coreState->fail(std::make_exception_ptr(*error));
  }
  catch (...) {
    // A throwing result copy still has one terminal Core outcome.  Preserve
    // the exception as the failure delivered to waiters.
    try { won = operation->coreState->fail(std::current_exception()); }
    catch (...) { won = false; }
  }
  if (!won) {
    std::lock_guard<std::mutex> lock(operation->mutex);
    ++operation->staleCallbacks;
    return false;
  }
  unregisterOperation(operation);
  if (cancelDeadline) cancelDeadline();
  if (cancelBootstrapRetry) cancelBootstrapRetry();
  if (cancelCore && !deferConversationCleanup) {
    const auto user = operation->user;
    const auto id = ndn::Name(operation->coreRequestId);
    const auto cleanupState = operation->ioCleanupState;
    const auto operationRuntime = operation->operationRuntime;
    if (cleanupState)
      cleanupState->pending.fetch_add(1, std::memory_order_acq_rel);
    auto cleanup = [user, id, scopes = std::move(releaseScopes), cleanupState,
                    operationRuntime] {
      try {
        user->CancelCollaboration(id);
        // A conversation commit may still need the request-scope key for
        // ROLLBACK/FINALIZE. The transaction owner clears it after its callback
        // leaves the coordinator; ordinary terminal paths clear immediately.
        for (const auto& scope : scopes)
          user->clearVerifiedCollaborationData(id, scope);
      }
      catch (...) {
        // Terminal delivery already won.  Keep the cleanup ticket until this
        // task has finished so Runtime cannot mistake a throwing cleanup for
        // an empty Face queue.
      }
      notifyIoCleanupSettled(cleanupState);
      if (operationRuntime)
        operationRuntime->notifyWaiters();
    };
    if (inlineCoreCleanup && user->isOnIoThread()) {
      cleanup();
    }
    else {
      try {
        user->postToIo(std::move(cleanup));
      }
      catch (...) {
        notifyIoCleanupSettled(cleanupState);
        throw;
      }
    }
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
             NativeInferenceEvent event, bool requirePending)
{
  if (!operation->coreState)
    return;
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    if (event.sequence == 0)
      event.sequence = ++operation->nextEventSequence;
  }
  const auto bytes = event.payload.size() + event.requestId.size() + 1;
  bool admitted = !requirePending;
  const bool published = requirePending
    ? operation->coreState->publishIf(
        std::move(event), bytes, [operation, &admitted] {
          std::lock_guard<std::mutex> lock(operation->mutex);
          if (operation->cancelled->load() || operation->phase == DiRequestPhase::Terminal)
            return false;
          admitted = true;
          return true;
        })
    : operation->coreState->publishTerminal(std::move(event), bytes);
  if (requirePending && !admitted) {
    std::lock_guard<std::mutex> lock(operation->mutex);
    ++operation->staleCallbacks;
    return;
  }
  if (!published) {
    std::lock_guard<std::mutex> lock(operation->mutex);
    ++operation->deliveryOverflows;
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
}

void
cancelOperation(const std::shared_ptr<NativeInferenceHandle::Operation>& operation)
{
  if (!markTerminal(operation, NativeRequestStatus::Cancelled)) {
    return;
  }
}

void enqueueOperation(const std::shared_ptr<NativeInferenceHandle::Operation>& operation,
                      std::function<void()> work, const std::string& boundary)
{
  try {
    auto ticket = operation->operationRuntime->acquire();
    operation->operationRuntime->post(ticket, [operation, work = std::move(work), boundary] {
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
  std::shared_ptr<NativeIoCleanupState> cleanupState;
  const auto operationRuntime = operation->operationRuntime;
  try {
    const auto user = operation->user;
    const auto id = ndn::Name(operation->coreRequestId);
    cleanupState = operation->ioCleanupState;
    if (cleanupState)
      cleanupState->pending.fetch_add(1, std::memory_order_acq_rel);
    auto cleanup = [user, id, scopes = std::move(scopes), cancelCore, cleanupState,
                    operationRuntime] {
      try {
        if (cancelCore) user->CancelCollaboration(id);
        for (const auto& scope : scopes)
          user->clearVerifiedCollaborationData(id, scope);
      }
      catch (...) {
        // The operation is already terminal; preserve lifecycle accounting
        // even if a late conversation cleanup cannot reach the Core map.
      }
      notifyIoCleanupSettled(cleanupState);
      if (operationRuntime)
        operationRuntime->notifyWaiters();
    };
    user->postToIo(std::move(cleanup));
  }
  catch (...) {
    notifyIoCleanupSettled(cleanupState);
    if (operationRuntime)
      operationRuntime->notifyWaiters();
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
        !operation->planned || operation->cancelled->load())
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
  completed.durableCommitGate = [operation, turn](const std::function<void()>& publish) {
    {
      std::lock_guard<std::mutex> lock(operation->mutex);
      if (operation->cancelled->load() ||
          std::chrono::steady_clock::now() >= operation->deadline)
        throw std::runtime_error("conversation durable commit fenced by terminal request");
      publish();
      operation->conversationCommitted = true;
    }
    logRuntimePhase(
      "di-cli", "checkpointCommitted", operation->requestId,
      std::to_string(turn.attempt),
      {{"executionRole", "requester"},
       {"conversationId", turn.parent.conversationId},
       {"inferenceEpoch", std::to_string(turn.attempt)},
       {"contextEpoch", std::to_string(turn.successorContextEpoch)}});
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
  const auto record = operation->conversations->commitTurn(turn, checkpoint);
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    operation->conversationCheckpointWire = record.checkpoint.wire;
  }
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
    if (operation->cancelled->load() || sourceAttempt != 1 ||
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
    if (!operation->cancelled->load() && operation->attempt == sourceAttempt &&
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
        if (operation->cancelled->load() || operation->phase == DiRequestPhase::Terminal ||
            operation->attempt != sourceAttempt)
          return;
      }
      // ServiceUser starts NAC-ABE and Controller permission discovery
      // asynchronously. Do not invoke BeginCollaboration until the
      // protected requester can pass its admission checks: doing so would
      // turn a normal bootstrap race into a misleading begin failure. Retry
      // through the operation owner rather than sleeping or blocking the Face
      // I/O thread. The original request deadline remains the only budget.
      if (operation->runtime && !operation->user->isRequestBootstrapReady(
            ndn::Name(operation->runtime->contract.serviceName),
            operation->options.providerNames)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= operation->deadline)
          throw NativeDiError("NATIVE_REQUEST_BOOTSTRAP_TIMEOUT", "authorization",
            "bootstrap", "Core authorization bootstrap did not become ready before the request deadline",
            operation->requestId, sourceAttempt);
        auto ticket = operation->operationRuntime->acquire();
        auto cancelRetry = operation->operationRuntime->scheduleAt(
          ticket, std::min(operation->deadline, now + std::chrono::milliseconds(20)),
          [operation, sourceAttempt] {
            {
              std::lock_guard<std::mutex> lock(operation->mutex);
              // The timer has fired and is no longer cancellable. Clear the
              // stale cancellation closure before posting the next probe.
              operation->cancelBootstrapRetry = {};
              if (operation->cancelled->load() || operation->phase == DiRequestPhase::Terminal ||
                  operation->attempt != sourceAttempt)
                return;
            }
            beginCoreRequest(operation);
          });
        bool retainRetry = false;
        {
          std::lock_guard<std::mutex> lock(operation->mutex);
          if (!operation->cancelled->load() && operation->phase != DiRequestPhase::Terminal &&
              operation->attempt == sourceAttempt) {
            operation->cancelBootstrapRetry = cancelRetry;
            retainRetry = true;
          }
        }
        if (!retainRetry)
          cancelRetry();
        return;
      }
      {
        std::lock_guard<std::mutex> lock(operation->mutex);
        if (operation->cancelled->load() || operation->attempt != sourceAttempt) return;
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
          std::shared_ptr<const NativeRequestRuntime> runtime;
          NativeRequestOptions coreOptions;
          std::shared_ptr<const NativeInspectedModel> inspected;
          NativeStrategyPorts strategies;
          std::shared_ptr<const NativeModelSplitStrategy> legacySplitter;
          std::shared_ptr<const NativePlacementStrategy> legacyPlacement;
          std::shared_ptr<const CooperativeModelSplitStrategy> cooperativeSplitter;
          std::shared_ptr<const CooperativePlacementStrategy> cooperativePlacement;
          bool cooperativeStrategies = false;
          std::shared_ptr<NativeRequestPreparation> preparation;
          std::shared_ptr<const NativeOfferAdmission> admission;
          std::shared_ptr<NativeConversationCoordinator> conversations;
          std::optional<NativeConversationTurn> conversationTurn;
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            if (operation->cancelled->load() || operation->attempt != sourceAttempt ||
                operation->phase != DiRequestPhase::Requesting) return;
            operation->phase = DiRequestPhase::Planning;
            runtime = operation->runtime;
            coreOptions = operation->coreOptions;
            if (operation->inspected) inspected = std::make_shared<NativeInspectedModel>(*operation->inspected);
            strategies = operation->strategies;
            legacySplitter = operation->legacySplitter;
            legacyPlacement = operation->legacyPlacement;
            cooperativeSplitter = operation->cooperativeSplitter;
            cooperativePlacement = operation->cooperativePlacement;
            cooperativeStrategies = operation->cooperativeStrategies;
            preparation = operation->preparation;
            admission = operation->admission;
            conversations = operation->conversations;
            conversationTurn = operation->conversationTurn;
          }
          {
            std::ostringstream record;
            record << "NDNSF_DI_NATIVE_ACK_CLOSED"
                   << " requestId=" << operation->requestId
                   << " attemptEpoch=" << sourceAttempt
                   << " epochMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count()
                   << " ackDigest=" << closure.digest;
            logRuntimeEvidence(record.str());
          }
          NativeRequestControl control{coreRequestId, sourceAttempt, operation->deadline,
            [flag = operation->cancelled] { return flag->load(); }};
          if (!runtime || !inspected || !strategies.enumerate || !strategies.proposeRoles ||
              (cooperativeStrategies ? (!cooperativeSplitter || !cooperativePlacement) :
                                       (!legacySplitter || !legacyPlacement)) ||
              !preparation || !admission)
            throw NativeDiError("NATIVE_REQUEST_PLANNING_INPUT_MISSING", "planning", "ACK_CLOSED",
              "native planning inputs were not published before ACK closure", operation->requestId,
              sourceAttempt);
          const auto phaseConversationId = conversationTurn ?
            conversationTurn->parent.conversationId : std::string("none");
          const auto phaseContextEpoch = conversationTurn ?
            std::to_string(conversationTurn->parent.parentContextEpoch) : std::string("none");
          const std::vector<std::pair<std::string, std::string>> planPhaseFields = {
            {"executionRole", "requester"},
            {"conversationId", phaseConversationId},
            {"contextEpoch", phaseContextEpoch},
            {"inferenceEpoch", std::to_string(sourceAttempt)}};
          logRuntimePhase("di-cli", "planBegin", operation->requestId,
                          std::to_string(sourceAttempt), planPhaseFields);
          const auto encoded = [&] {
            std::lock_guard<std::mutex> lock(operation->mutex);
            if (operation->cancelled->load() || operation->attempt != sourceAttempt ||
                operation->phase != DiRequestPhase::Planning || !operation->encodedRequest)
              throw NativeDiError("NATIVE_REQUEST_STALE", "planning", "ACK_CLOSED",
                "native planning callback became stale before request snapshot", operation->requestId,
                sourceAttempt);
            return *operation->encodedRequest;
          }();
          auto planned = cooperativeStrategies
            ? planNativeRequestCooperative(*runtime, coreOptions, *inspected, encoded,
                *cooperativeSplitter, *cooperativePlacement, *preparation, *admission,
                closure, control, operation->wireDeadlineMs, operation->cancelled,
                conversationTurn ? &*conversationTurn : nullptr)
            : planNativeRequest(*runtime, coreOptions, *inspected, encoded,
                *legacySplitter, *legacyPlacement, *preparation, *admission,
                closure, control, operation->wireDeadlineMs, operation->cancelled,
                conversationTurn ? &*conversationTurn : nullptr);
          logRuntimePhase("di-cli", "planEnd", operation->requestId,
                          std::to_string(sourceAttempt), planPhaseFields);
          std::optional<NativeConversationTurn> plannedConversationTurn = conversationTurn;
          if (plannedConversationTurn && conversations) {
            if (plannedConversationTurn->attempt == 2) {
              plannedConversationTurn = conversations->bindAttemptPlanRoleMap(
                *plannedConversationTurn, planned.sealed.core.assignment.providerByRole);
            }
            else if (plannedConversationTurn->parent.planRoleMapDigest.empty()) {
              plannedConversationTurn = conversations->bindInitialPlanRoleMap(
                *plannedConversationTurn, planned.sealed.core.assignment.providerByRole);
            }
          }
          const auto corePlan = planned.corePlan;
          const auto sealedPlanDigest = planned.sealed.planDigest;
          bool published = false;
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            const bool sameTurn = (!conversationTurn && !operation->conversationTurn) ||
              (conversationTurn && operation->conversationTurn &&
               conversationTurn->ticket == operation->conversationTurn->ticket &&
               conversationTurn->attempt == operation->conversationTurn->attempt &&
               conversationTurn->requestId == operation->conversationTurn->requestId);
            if (!operation->cancelled->load() && operation->attempt == sourceAttempt &&
                operation->phase == DiRequestPhase::Planning && sameTurn) {
              operation->planned = std::move(planned);
              if (plannedConversationTurn)
                operation->conversationTurn = std::move(plannedConversationTurn);
              published = true;
            }
          }
          if (!published) {
            if (plannedConversationTurn && conversations) {
              const NativeDiError stale(
                "NATIVE_CONVERSATION_PLAN_CANCELLED", "conversation", "planning",
                "conversation turn became terminal before plan publication", operation->requestId,
                sourceAttempt);
              conversations->abortTurn(*plannedConversationTurn, stale);
            }
            return;
          }
          operation->user->postToIo([operation, sourceAttempt, coreRequestId,
                                    digest = closure.digest, plan = corePlan,
                                    sealedPlanDigest] {
            try {
              {
                std::lock_guard<std::mutex> lock(operation->mutex);
                if (operation->cancelled->load() || operation->attempt != sourceAttempt) return;
              }
              if (std::chrono::steady_clock::now() >= operation->deadline)
                throw NativeDiError("NATIVE_REQUEST_TIMEOUT", "local", "commit",
                  "request expired before commit", operation->requestId, operation->attempt);
              if (!operation->user->CommitCollaborationPlan(ndn::Name(coreRequestId), digest, plan))
                throw std::runtime_error("Core rejected the sealed plan");
              {
                std::ostringstream record;
                record << "NDNSF_DI_NATIVE_SELECTION_COMMITTED"
                       << " requestId=" << operation->requestId
                       << " attemptEpoch=" << sourceAttempt
                       << " epochMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count()
                       << " planDigest=" << sealedPlanDigest;
                logRuntimeEvidence(record.str());
              }
              std::lock_guard<std::mutex> lock(operation->mutex);
            if (!operation->cancelled->load() && operation->attempt == sourceAttempt)
                operation->phase = DiRequestPhase::Committed;
            }
            catch (const NativeDiError& error) { failOperation(operation, error, sourceAttempt); }
            catch (const std::exception& error) {
              // Keep the first Core boundary visible instead of replacing
              // all validation/publication errors with one opaque message.
              auto detail = std::string(error.what()).substr(0, 1024);
              std::replace(detail.begin(), detail.end(), '\n', ' ');
              std::replace(detail.begin(), detail.end(), '\r', ' ');
              failOperation(operation, NativeDiError(
                "NATIVE_REQUEST_COMMIT_FAILED", "runtime", "commit",
                "Core plan commit failed: " + detail,
                operation->requestId, sourceAttempt), sourceAttempt);
            }
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
            if (operation->cancelled->load()) return;
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
          markTerminal(operation, NativeRequestStatus::Succeeded, nullptr, &result);
        }, "response");
      };
      const auto timeout = [operation, sourceAttempt](const ndn::Name&) {
        enqueueOperation(operation, [operation, sourceAttempt] {
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            if (operation->cancelled->load() || operation->attempt != sourceAttempt) return;
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
          if (operation->cancelled->load() || operation->attempt != sourceAttempt) {
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
          std::string conversationId = "none";
          std::string contextEpoch = "none";
          std::size_t tokenIndex = 0;
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            if (operation->conversationTurn) {
              conversationId = operation->conversationTurn->parent.conversationId;
              contextEpoch = std::to_string(
                operation->conversationTurn->parent.parentContextEpoch);
            }
            tokenIndex = operation->acceptedTokenIds.size();
          }
          logRuntimePhase(
            "di-cli", "tokenDelivered", operation->requestId,
            std::to_string(sourceAttempt),
            {{"executionRole", "requester"},
             {"conversationId", conversationId},
             {"contextEpoch", contextEpoch},
             {"inferenceEpoch", std::to_string(sourceAttempt)},
             {"tokenIndex", std::to_string(tokenIndex)}});
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
            if (operation->cancelled->load() || operation->attempt != sourceAttempt) return;
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
          markTerminal(operation, NativeRequestStatus::Succeeded, nullptr, &result);
        }, "stream-final");
      };
      const auto streamError = [operation, sourceAttempt](const ndn_service_framework::StreamedInvocationError& error) {
        enqueueOperation(operation, [operation, sourceAttempt, error] {
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            if (operation->cancelled->load() || operation->attempt != sourceAttempt) {
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
      operation->user->BeginCollaborationWithProviders(ndn::Name(operation->runtime->contract.serviceName),
        ndn::Buffer(operation->encodedRequest->wire.begin(), operation->encodedRequest->wire.end()),
        static_cast<int>(operation->coreOptions.ackTimeoutMs),
        static_cast<int>(operation->coreOptions.timeoutMs), ackClosed, response, timeout,
        ndn::Name(coreRequestId), {}, capabilities, operation->options.stream,
        streamEvent, streamComplete, streamError, operation->options.providerNames);
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
    if (operation->cancelled->load() ||
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
      if (operation->cancelled->load()) {
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
    catch (const std::exception& error) {
      std::string detail = "native request contract encoding failed";
      if (error.what() != nullptr && *error.what() != '\0')
        detail += ": " + std::string(error.what());
      failOperation(operation, NativeDiError(
        "NATIVE_REQUEST_CONTRACT_INVALID", "planning", "requestWire",
        detail, operation->requestId, operation->attempt));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(operation->mutex);
      if (operation->cancelled->load()) {
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
      if (!operation->cancelled->load()) {
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
    catch (const std::exception& exc) {
      std::string detail = exc.what();
      if (detail.empty()) detail = "unknown native conversation error";
      failOperation(operation, NativeDiError(
        "NATIVE_CONVERSATION_BEGIN_FAILED", "conversation", "begin",
        "native conversation turn could not be opened: " + detail, operation->requestId,
        operation->attempt));
      return;
    }
  }
  if (operation->runtime && operation->preparedInput && operation->encodedRequest) {
    auto inspected = operation->preparation->inspectModel(*operation->preparedInput);
    {
      std::lock_guard<std::mutex> lock(operation->mutex);
      if (operation->cancelled->load()) return;
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

std::chrono::steady_clock::time_point NativeInferenceHandle::deadline() const
{
  if (!m_operation)
    throw NativeDiError("INVALID_HANDLE", "local", "handle",
                        "native inference handle is empty");
  std::lock_guard<std::mutex> lock(m_operation->mutex);
  return m_operation->deadline;
}

std::optional<std::string> NativeInferenceHandle::conversationCheckpoint() const
{
  if (!m_operation) return std::nullopt;
  if (!m_operation->coreState || !m_operation->coreState->succeeded())
    return std::nullopt;
  std::lock_guard<std::mutex> lock(m_operation->mutex);
  return m_operation->conversationCheckpointWire;
}

NativeRequestStatus NativeInferenceHandle::status() const
{
  if (!m_operation) throw NativeDiError("INVALID_HANDLE", "local", "handle",
                                         "native inference handle is empty");
  if (!m_operation->coreState || m_operation->coreState->isPending())
    return NativeRequestStatus::Pending;
  if (m_operation->coreState->succeeded())
    return NativeRequestStatus::Succeeded;
  if (const auto failure = m_operation->coreState->failure()) {
    try {
      std::rethrow_exception(failure);
    }
    catch (const NativeDiError& error) {
      if (error.code() == "CANCELLED")
        return NativeRequestStatus::Cancelled;
    }
    catch (...) {}
  }
  return NativeRequestStatus::Failed;
}

NativeInferenceDiagnostics NativeInferenceHandle::diagnostics() const
{
  if (!m_operation)
    throw NativeDiError("INVALID_HANDLE", "local", "diagnostics",
                        "native inference handle is empty");
  std::shared_ptr<ndn_service_framework::OperationState<NativeInferenceResult,
                                                        NativeInferenceEvent>> coreState;
  {
    std::lock_guard<std::mutex> lock(m_operation->mutex);
    coreState = m_operation->coreState;
  }
  return NativeInferenceDiagnostics{coreState ? coreState->observationDropped() : 0};
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
  std::string requestId;
  std::uint64_t attempt = 0;
  {
    std::lock_guard<std::mutex> lock(operation->mutex);
    requestId = operation->requestId;
    attempt = operation->attempt;
  }
  if (waitTimeout.count() > 0 && operation->user->isOnIoThread()) {
    throw NativeDiError("CORE_IO_WAIT_FORBIDDEN", "local", "wait",
                        "native result cannot block the Core I/O thread",
                        requestId, attempt);
  }
  try {
    return operation->coreState->result(waitTimeout);
  }
  catch (const ndn_service_framework::OperationError& error) {
    if (error.code() == ndn_service_framework::OperationErrorCode::Timeout)
      throw NativeDiError("LOCAL_WAIT_TIMEOUT", "local", "wait",
                          "native result wait timed out", requestId, attempt);
    if (error.code() == ndn_service_framework::OperationErrorCode::WouldDeadlock)
      throw NativeDiError("CORE_WORKER_WAIT_FORBIDDEN", "local", "wait",
                          "native result cannot block its Core worker", requestId, attempt);
    if (error.code() == ndn_service_framework::OperationErrorCode::Cancelled)
      throw NativeDiError("CANCELLED", "local", "request",
                          "native request was cancelled", requestId, attempt);
    throw NativeDiError("NATIVE_REQUEST_FAILED", "runtime", "result",
                        error.what(), requestId, attempt);
  }
}

void NativeInferenceHandle::cancel()
{
  if (!m_operation) return;
  cancelOperation(m_operation);
}

void NativeInferenceHandle::retain(std::shared_ptr<void> owner)
{
  if (!m_operation)
    return;
  std::lock_guard<std::mutex> lock(m_operation->mutex);
  if (m_operation->phase != DiRequestPhase::Terminal)
    m_operation->packageLease = std::move(owner);
}

void NativeInferenceClient::retainOwner(std::shared_ptr<void> owner)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_ownerLease = std::move(owner);
}

void NativeInferenceHandle::observe(
  std::function<void(const NativeInferenceEvent&)> observer)
{
  auto subscription = observeSubscription(std::move(observer));
  std::lock_guard<std::mutex> lock(m_operation->mutex);
  m_operation->observerSubscriptions.push_back(std::move(subscription));
}

NativeOperationSubscription NativeInferenceHandle::observeSubscription(
  std::function<void(const NativeInferenceEvent&)> observer)
{
  if (!m_operation || !observer) {
    throw NativeDiError("INVALID_OBSERVER", "local", "observer",
                        "native observer is empty");
  }
  try {
    return m_operation->coreState->observe(std::move(observer));
  }
  catch (const ndn_service_framework::OperationError& error) {
    const auto code = error.code() == ndn_service_framework::OperationErrorCode::Capacity
      ? "SUBSCRIPTION_LIMIT" :
      error.code() == ndn_service_framework::OperationErrorCode::Closed
        ? "RUNTIME_CLOSED" : "NATIVE_OBSERVER_FAILED";
    throw NativeDiError(code, "local", "observer", error.what(),
                        m_operation->requestId, m_operation->attempt);
  }
}

NativeOperationSubscription NativeInferenceHandle::onCompletion(
  std::function<void(std::exception_ptr, std::optional<NativeInferenceResult>)> callback)
{
  if (!m_operation || !callback)
    throw NativeDiError("INVALID_COMPLETION", "local", "completion",
                        "native completion callback is empty");
  try {
    return m_operation->coreState->onCompletion([operation = m_operation,
                                                  callback = std::move(callback)] {
      std::optional<NativeInferenceResult> result;
      std::exception_ptr error;
      try {
        result = operation->coreState->result(std::chrono::milliseconds(0));
      }
      catch (...) {
        error = std::current_exception();
      }
      try { callback(std::move(error), std::move(result)); }
      catch (...) {}
    });
  }
  catch (const ndn_service_framework::OperationError& error) {
    const auto code = error.code() == ndn_service_framework::OperationErrorCode::Capacity
      ? "SUBSCRIPTION_LIMIT" :
      error.code() == ndn_service_framework::OperationErrorCode::Closed
        ? "RUNTIME_CLOSED" : "NATIVE_COMPLETION_FAILED";
    throw NativeDiError(code, "local", "completion", error.what(),
                        m_operation->requestId, m_operation->attempt);
  }
}

NativeOperationSubscription NativeInferenceHandle::resultAsync(
  std::chrono::milliseconds timeout,
  std::function<void(std::optional<NativeInferenceResult>, std::exception_ptr)> callback)
{
  if (!m_operation || !callback)
    throw NativeDiError("INVALID_COMPLETION", "local", "completion",
                        "native result callback is empty");
  if (timeout.count() < 0)
    throw NativeDiError("INVALID_WAIT_TIMEOUT", "local", "wait",
                        "native result timeout is negative", m_operation->requestId,
                        m_operation->attempt);
  try {
    return m_operation->coreState->resultAsync(timeout, std::move(callback));
  }
  catch (const ndn_service_framework::OperationError& error) {
    const auto code = error.code() == ndn_service_framework::OperationErrorCode::Capacity
      ? "SUBSCRIPTION_LIMIT" :
      error.code() == ndn_service_framework::OperationErrorCode::Closed
        ? "RUNTIME_CLOSED" : "NATIVE_COMPLETION_FAILED";
    throw NativeDiError(code, "local", "completion", error.what(),
                        m_operation->requestId, m_operation->attempt);
  }
}

NativeEventReader NativeInferenceHandle::events() const
{
  if (!m_operation || !m_operation->coreState)
    throw NativeDiError("INVALID_HANDLE", "local", "events", "native inference handle is empty");
  {
    std::lock_guard<std::mutex> lock(m_operation->mutex);
    if (!m_operation->options.stream)
      throw NativeDiError("UNSUPPORTED_CAPABILITY", "local", "events",
                          "reliable event reading requires an enabled stream",
                          m_operation->requestId, m_operation->attempt);
  }
  try {
    return m_operation->coreState->openReader();
  }
  catch (const ndn_service_framework::OperationError& error) {
    const auto code = error.code() == ndn_service_framework::OperationErrorCode::Capacity
      ? "READ_IN_PROGRESS" :
      error.code() == ndn_service_framework::OperationErrorCode::Closed
        ? "READER_CLOSED" :
      error.code() == ndn_service_framework::OperationErrorCode::EventGap
        ? "STREAM_GAP" : "NATIVE_EVENT_READER_FAILED";
    throw NativeDiError(code, "local", "events", error.what(),
                        m_operation->requestId, m_operation->attempt);
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
  , m_operationRuntime(ndn_service_framework::OperationRuntime::create(testPort.submitHook))
  , m_requestOwnerScope(testPort.submitHook ? std::string{} : processRequestOwnerScope())
  , m_now(testPort.now ? testPort.now : defaultClock())
  , m_operationRegistry(std::make_shared<NativeOperationRegistry>())
  , m_schedule(testPort.scheduleHook)
{
  m_ioCleanupState = std::make_shared<NativeIoCleanupState>();
  if (!m_user || !m_adapters) {
    throw NativeDiError("INVALID_CLIENT_CONFIGURATION", "local", "constructor",
                        "native client requires a ServiceUser and adapter registry");
  }
}

NativeInferenceClient::~NativeInferenceClient() noexcept
{
  close();
}

NativeInferenceHandle NativeInferenceClient::requestImpl(
  const NativeModelRef& model,
  const NativeApplicationInput& input,
  NativeStrategyPorts strategies,
  std::shared_ptr<const NativeModelSplitStrategy> legacySplitter,
  std::shared_ptr<const NativePlacementStrategy> legacyPlacement,
  std::shared_ptr<const CooperativeModelSplitStrategy> cooperativeSplitter,
  std::shared_ptr<const CooperativePlacementStrategy> cooperativePlacement,
  const NativeRequestOptions& options)
{
  std::shared_ptr<NativeInferenceHandle::Operation> operation;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed) {
      throw NativeDiError("CLIENT_CLOSED", "local", "request",
                          "native inference client is closed");
    }
    if (!strategies.enumerate || !strategies.proposeRoles || options.timeoutMs == 0 ||
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
    operation->ownerLease = m_ownerLease;
    operation->model = model;
    operation->input = input;
    operation->options = options;
    operation->strategies = std::move(strategies);
    operation->legacySplitter = std::move(legacySplitter);
    operation->legacyPlacement = std::move(legacyPlacement);
    operation->cooperativeSplitter = std::move(cooperativeSplitter);
    operation->cooperativePlacement = std::move(cooperativePlacement);
    operation->cooperativeStrategies = static_cast<bool>(operation->cooperativeSplitter);
    operation->preparation = m_preparation;
    operation->requestContract = m_requestContract;
    operation->runtime = m_runtime;
    operation->admission = m_admission;
    operation->adapters = m_adapters;
    operation->ioCleanupState = m_ioCleanupState;
    operation->conversations = m_conversations;
    operation->operationRuntime = m_operationRuntime;
    operation->coreState = std::make_shared<ndn_service_framework::OperationState<
      NativeInferenceResult, NativeInferenceEvent>>(
        m_operationRuntime, ndn_service_framework::OperationState<
          NativeInferenceResult, NativeInferenceEvent>::CancelFunction{},
        [] (const NativeInferenceEvent& event) {
          return event.payload.size() + event.requestId.size() + 1;
        });
    operation->registry = m_operationRegistry;
    operation->applicationRequestId = options.applicationRequestId;
    // The requestId comes from a unique native owner allocated at submission
    // (runtime-boundaries: Core allocation or unique native owner); the
    // operation then binds ACK/plan/grant/result to this stable URI.
    // Keep the owner scope and monotonic counter in one final Name component.
    // Core/Provider V2 names historically treat the request identity as a
    // suffix; introducing another path component breaks legacy filters and
    // compact-selection parsing even though the identity itself is unique.
    operation->requestId = "/NDNSF/DI/REQUEST/" +
      (m_requestOwnerScope.empty() ? std::string{} : m_requestOwnerScope + "-") +
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
        const auto requiredStreamEvents = nativeStreamEventBudgetForGeneration(
          generation.maxGeneratedTokens);
        // StreamRequestOptions::maxEvents includes the terminal event and is
        // independently bounded by the Core stream protocol. Derive the
        // minimum from the authenticated generation contract so a caller
        // cannot request 1025 tokens with the generic 512-event default.
        if (requiredStreamEvents > 4096) {
          throw NativeDiError(
            "INVALID_GENERATION_OPTIONS", "local", "request",
            "generation budget exceeds the native stream event bound");
        }
        operation->options.stream->maxEvents = std::max<std::uint32_t>(
          operation->options.stream->maxEvents,
          static_cast<std::uint32_t>(requiredStreamEvents));
      }
    }
    // Keep a weak diagnostic index for the existing tests and a separate
    // strong registry entry for every pending operation.  The registry is
    // removed by markTerminal, so completed fire-and-forget requests do not
    // remain owned by the client while close() still reaches dropped handles.
    m_operations.erase(
      std::remove_if(m_operations.begin(), m_operations.end(),
                     [] (const auto& weak) { return weak.expired(); }),
      m_operations.end());
    m_operations.push_back(operation);
    {
      std::lock_guard<std::mutex> registryLock(m_operationRegistry->mutex);
      m_operationRegistry->pending.emplace(operation.get(), operation);
    }
  }
  // Submission returns a Pending handle; the dispatch runs on the
  // client-owned serial executor, never on the caller or the Core I/O thread.
  // The dispatch task captures only the operation and the clock and may
  // safely outlive this client.
  auto now = m_now;
  try {
    if (m_schedule) {
      std::lock_guard<std::mutex> lock(operation->mutex);
      if (!operation->cancelled->load()) {
        operation->cancelDeadline = m_schedule(operation->deadline,
          [weak = std::weak_ptr<NativeInferenceHandle::Operation>(operation)] {
            if (auto pending = weak.lock()) {
              failOperation(pending, NativeDiError(
                "NATIVE_REQUEST_TIMEOUT", "local", "request",
                "native request budget expired", pending->requestId, pending->attempt));
            }
          });
      }
    }
    else {
      auto deadlineSubscription = operation->coreState->resultAsync(
        std::chrono::milliseconds(options.timeoutMs),
        [weak = std::weak_ptr<NativeInferenceHandle::Operation>(operation)]
        (std::optional<NativeInferenceResult>, std::exception_ptr error) {
          if (!error) return;
          if (auto pending = weak.lock()) {
            failOperation(pending, NativeDiError(
              "NATIVE_REQUEST_TIMEOUT", "local", "request",
              "native request budget expired", pending->requestId, pending->attempt));
          }
        });
      operation->deadlineSubscription = std::move(deadlineSubscription);
    }
    auto ticket = m_operationRuntime->acquire();
    m_operationRuntime->post(ticket, [operation, now] {
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

NativeInferenceHandle NativeInferenceClient::request(
  const NativeModelRef& model,
  const NativeApplicationInput& input,
  std::shared_ptr<const NativeModelSplitStrategy> splitStrategy,
  std::shared_ptr<const NativePlacementStrategy> placementStrategy,
  const NativeRequestOptions& options)
{
  if (!splitStrategy || !placementStrategy)
    throw NativeDiError("INVALID_REQUEST", "local", "request",
                        "native request strategies are required");
  NativeStrategyPorts ports;
  ports.splitterIdentity = splitStrategy->identity();
  ports.placementIdentity = placementStrategy->identity();
  ports.enumerate = [splitStrategy](const NativeModelDescriptor& descriptor,
                                    const NativeGraphSnapshot& graph,
                                    const NativeCandidateBudget& budget,
                                    const ExtensionControl& extension) {
    extension.requireActive();
    auto result = splitStrategy->enumerate(descriptor, graph, budget);
    extension.requireActive();
    return result;
  };
  ports.proposeRoles = [placementStrategy](const NativeOfferBindingContext& context,
                                            const std::string& ackClosedDigest,
                                            const std::vector<NativeSelectionRoleV3>& roles,
                                            const std::vector<NativeAdmittedOfferV3>& offers,
                                            std::uint64_t nowMs,
                                            const ExtensionControl& extension) {
    extension.requireActive();
    auto result = placementStrategy->proposeRoles(context, ackClosedDigest, roles, offers, nowMs);
    extension.requireActive();
    return result;
  };
  return requestImpl(model, input, std::move(ports), std::move(splitStrategy),
                     std::move(placementStrategy), nullptr, nullptr, options);
}

NativeInferenceHandle NativeInferenceClient::requestCooperative(
  const NativeModelRef& model,
  const NativeApplicationInput& input,
  std::shared_ptr<const CooperativeModelSplitStrategy> splitStrategy,
  std::shared_ptr<const CooperativePlacementStrategy> placementStrategy,
  const NativeRequestOptions& options)
{
  if (!splitStrategy || !placementStrategy)
    throw NativeDiError("INVALID_REQUEST", "local", "request",
                        "cooperative request strategies are required");
  NativeStrategyPorts ports;
  ports.splitterIdentity = splitStrategy->identity();
  ports.placementIdentity = placementStrategy->identity();
  ports.enumerate = [splitStrategy](const NativeModelDescriptor& descriptor,
                                    const NativeGraphSnapshot& graph,
                                    const NativeCandidateBudget& budget,
                                    const ExtensionControl& extension) {
    return splitStrategy->enumerate(descriptor, graph, budget, extension);
  };
  ports.proposeRoles = [placementStrategy](const NativeOfferBindingContext& context,
                                            const std::string& ackClosedDigest,
                                            const std::vector<NativeSelectionRoleV3>& roles,
                                            const std::vector<NativeAdmittedOfferV3>& offers,
                                            std::uint64_t nowMs,
                                            const ExtensionControl& extension) {
    return placementStrategy->proposeRoles(context, ackClosedDigest, roles, offers, nowMs,
                                            extension);
  };
  return requestImpl(model, input, std::move(ports), nullptr, nullptr,
                     std::move(splitStrategy), std::move(placementStrategy), options);
}

void NativeInferenceClient::close() noexcept
{
  std::vector<std::shared_ptr<NativeInferenceHandle::Operation>> operations;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed) return;
    m_closed = true;
    {
      std::lock_guard<std::mutex> registryLock(m_operationRegistry->mutex);
      for (auto& entry : m_operationRegistry->pending)
        operations.push_back(std::move(entry.second));
      m_operationRegistry->pending.clear();
    }
    for (const auto& weak : m_operations) {
      if (const auto operation = weak.lock(); operation &&
          std::none_of(operations.begin(), operations.end(),
                       [&operation](const auto& existing) {
                         return existing.get() == operation.get();
                       })) {
        operations.push_back(operation);
      }
    }
    m_closedOperations = operations;
    m_operations.clear();
  }
  for (const auto& operation : operations)
    logClientLifecyclePhase(operation, "closeBegin");
  for (const auto& operation : operations) {
    cancelOperation(operation);
  }
  // The Core runtime owns the request worker, timers, and notification
  // deliveries.  Closing it rejects new work and lets existing callbacks
  // converge through its join/drain barrier.
  m_operationRuntime->close();
}

void NativeInferenceClient::failIo(const std::string& reason) noexcept
{
  std::vector<std::shared_ptr<NativeInferenceHandle::Operation>> operations;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed)
      return;
    m_closed = true;
    {
      std::lock_guard<std::mutex> registryLock(m_operationRegistry->mutex);
      for (auto& entry : m_operationRegistry->pending)
        operations.push_back(std::move(entry.second));
      m_operationRegistry->pending.clear();
    }
    for (const auto& weak : m_operations) {
      if (const auto operation = weak.lock(); operation &&
          std::none_of(operations.begin(), operations.end(),
                       [&operation](const auto& existing) {
                         return existing.get() == operation.get();
                       })) {
        operations.push_back(operation);
      }
    }
    m_closedOperations = operations;
    m_operations.clear();
  }
  for (const auto& operation : operations)
    logClientLifecyclePhase(operation, "closeBegin");
  for (const auto& operation : operations) {
    try {
      auto error = std::make_shared<NativeDiError>(
        "NATIVE_REQUEST_FAILED", "runtime", "transport",
        reason.empty() ? "Core Face I/O failed" : "Core Face I/O failed: " + reason,
        operation->requestId, operation->attempt);
      markTerminal(operation, NativeRequestStatus::Failed, std::move(error), nullptr, 0, true);
    }
    catch (...) {
      // Preserve the original Face failure even if one request's terminal
      // delivery or synchronous cleanup is itself unavailable.
    }
  }
  m_operationRuntime->close();
}

bool NativeInferenceClient::drain(std::chrono::milliseconds timeout)
{
  if (timeout.count() < 0)
    throw NativeDiError("INVALID_ARGUMENT", "local", "lifecycle",
                        "native client drain timeout must not be negative");
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  try {
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = now >= deadline ? std::chrono::milliseconds(0) :
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (!m_operationRuntime || !m_operationRuntime->drain(remaining))
      return false;
    if (m_ioCleanupState) {
      std::unique_lock<std::mutex> lock(m_ioCleanupState->mutex);
      if (!m_ioCleanupState->condition.wait_until(lock, deadline, [this] {
        return !m_ioCleanupState ||
               m_ioCleanupState->pending.load(std::memory_order_acquire) == 0;
      })) {
        return false;
      }
    }
    std::vector<std::shared_ptr<NativeInferenceHandle::Operation>> drained;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      drained.swap(m_closedOperations);
    }
    for (const auto& operation : drained)
      logClientLifecyclePhase(operation, "drainEnd");
    return true;
  }
  catch (const ndn_service_framework::OperationError& error) {
    if (error.code() == ndn_service_framework::OperationErrorCode::WouldDeadlock)
      throw NativeDiError("WOULD_DEADLOCK", "local", "lifecycle", error.what());
    throw NativeDiError("CLIENT_CLOSED", "local", "lifecycle", error.what());
  }
}

void NativeInferenceClient::setDrainNotifier(std::function<void()> notifier)
{
  if (m_ioCleanupState) {
    std::lock_guard<std::mutex> lock(m_ioCleanupState->mutex);
    m_ioCleanupState->notifyOwner = notifier;
  }
  m_operationRuntime->setNotifyCallback(std::move(notifier));
}

bool NativeInferenceClient::isQuiescent() const noexcept
{
  return (!m_operationRuntime || m_operationRuntime->isQuiescent()) &&
         (!m_ioCleanupState ||
          m_ioCleanupState->pending.load(std::memory_order_acquire) == 0);
}

bool NativeInferenceClient::isWorkerThread() const noexcept
{
  return m_operationRuntime && m_operationRuntime->isWorkerThread();
}

std::shared_ptr<NativeConversationCoordinator>
NativeInferenceClient::conversationCoordinator() const noexcept
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_conversations;
}

} // namespace ndnsf::di
