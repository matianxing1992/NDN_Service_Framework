#include "NDNSF-DistributedInference/cpp/ndnsf-di/Conversation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCheckpointExport.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelPackage.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

std::string randomHexId()
{
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    throw DiError("CONVERSATION_ID_ALLOCATION_FAILED", "local", "conversation",
                  "native conversation id allocation failed");
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 0x0f]);
  }
  return result;
}

std::string randomGenerationId()
{
  return randomHexId();
}

NativeJson parseCheckpointWire(const std::string& wire)
{
  if (wire.empty() || wire.size() > (16U << 20) || wire.find('\0') != std::string::npos)
    throw DiError("INVALID_CHECKPOINT", "local", "conversation",
                  "conversation checkpoint bytes are empty or oversized");
  try {
    const auto value = nativeParseJson(wire);
    if (!value.is_object() || value.value("schema", std::string{}) !=
        "ndnsf-di-conversation-checkpoint-v1" ||
        value.value("version", 0U) != 1U ||
        !value.value("conversationId", std::string{}).size() ||
        !value.value("serviceName", std::string{}).size() ||
        !value.value("requesterIdentity", std::string{}).size() ||
        !value.value("modelContractDigest", std::string{}).size() ||
        !value.value("signature", std::string{}).size())
      throw std::invalid_argument("conversation checkpoint envelope is incomplete");
    return value;
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw DiError("INVALID_CHECKPOINT", "local", "conversation", error.what());
  }
}

std::uint64_t nowMs()
{
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string requestString(const PreparedModelPackage& package, const char* field)
{
  try {
    const auto root = nativeParseJson(package.registration->configurationJson);
    return root.at("request").at(field).get<std::string>();
  }
  catch (const std::exception& error) {
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "conversation",
                  std::string("verified conversation request field is unavailable: ") + error.what());
  }
}

std::uint64_t configuredRetentionDeadlineFromPackage(const PreparedModelPackage& package)
{
  const auto root = nativeParseJson(package.registration->configurationJson);
  const auto conversation = root.find("conversation");
  if (conversation == root.end() || !conversation->is_object())
    return 0;
  const auto turn = conversation->find("turn");
  if (turn == conversation->end() || !turn->is_object())
    return 0;
  if (!turn->contains("retention_deadline_ms"))
    return 0;
  const auto deadline = turn->at("retention_deadline_ms").get<std::uint64_t>();
  if (deadline == 0)
    throw std::invalid_argument("conversation retention_deadline_ms must be positive");
  return deadline;
}

bool isCanonicalDigest(const std::string& digest)
{
  return digest.size() == 71 && digest.compare(0, 7, "sha256:") == 0 &&
    digest.find_first_not_of("0123456789abcdef", 7) == std::string::npos;
}

bool isCanonicalName(const std::string& uri)
{
  try {
    const ndn::Name name(uri);
    return !name.empty() && name.toUri(ndn::name::UriFormat::ALTERNATE) == uri;
  }
  catch (const std::exception&) {
    return false;
  }
}

std::string handoffBindingDigest(
  const std::string& checkpointDigest,
  const std::map<std::string, std::string>& sourceProvidersByRole,
  const std::map<std::string, std::string>& destinationProvidersByRole,
  const std::map<std::string, NativeRemoteProtectedKvRoleTrustPolicyV1>& trustByRole,
  std::chrono::steady_clock::time_point deadline)
{
  NativeJson binding = NativeJson::object();
  binding["checkpointDigest"] = checkpointDigest;
  binding["deadlineTicks"] = deadline.time_since_epoch().count();
  binding["sourceProvidersByRole"] = NativeJson::object();
  binding["destinationProvidersByRole"] = NativeJson::object();
  binding["trustByRole"] = NativeJson::object();
  for (const auto& [role, provider] : sourceProvidersByRole)
    binding["sourceProvidersByRole"][role] = provider;
  for (const auto& [role, provider] : destinationProvidersByRole)
    binding["destinationProvidersByRole"][role] = provider;
  for (const auto& [role, trust] : trustByRole) {
    NativeJson policy = NativeJson::object();
    policy["target"] = NativeJson{{"repoNode", trust.target.repoNode},
      {"providerIdentity", trust.target.providerIdentity},
      {"servicePrefix", trust.target.servicePrefix}};
    policy["producerVerifierPolicyDigest"] = trust.producerVerifierPolicyDigest;
    policy["limits"] = NativeJson{
      {"listingPageSize", trust.limits.listingPageSize},
      {"maxListingPagesPerScan", trust.limits.maxListingPagesPerScan},
      {"maxListedObjectsPerScan", trust.limits.maxListedObjectsPerScan},
      {"maxListingWireBytes", trust.limits.maxListingWireBytes},
      {"maxPacketCount", trust.limits.maxPacketCount},
      {"maxPacketWireBytes", trust.limits.maxPacketWireBytes},
      {"maxManifestWireBytes", trust.limits.maxManifestWireBytes},
      {"ackCollectionTimeMs", trust.limits.ackCollectionTimeMs}};
    binding["trustByRole"][role] = std::move(policy);
  }
  return nativePlanningDigest(nativeCanonicalJson(binding));
}

} // namespace

ConversationCheckpoint ConversationCheckpoint::fromBytes(
  const std::vector<std::uint8_t>& bytes)
{
  const std::string wire(bytes.begin(), bytes.end());
  (void)parseCheckpointWire(wire);
  return ConversationCheckpoint(wire);
}

std::vector<std::uint8_t> ConversationCheckpoint::bytes() const
{
  return std::vector<std::uint8_t>(m_wire.begin(), m_wire.end());
}

struct ConversationHandoffHandle::State
{
  explicit State(std::string id)
    : operationId(std::move(id))
  {
  }

  mutable std::mutex mutex;
  std::condition_variable handoffChanged;
  std::condition_variable changed;
  std::string operationId;
  ConversationHandoffStatus status = ConversationHandoffStatus::Pending;
  std::exception_ptr failure;
  std::function<bool()> cancel;
  std::function<bool()> isCoreIoThread;
  Subscription appendCompletion;
};

ConversationHandoffHandle ConversationHandoffHandle::create(std::string operationId)
{
  if (operationId.empty() || operationId.size() > 256 ||
      operationId.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._~-") != std::string::npos)
    throw DiError("INVALID_HANDOFF_OPERATION_ID", "local", "handoff",
                  "conversation handoff operation ID is invalid");
  return ConversationHandoffHandle(std::make_shared<State>(std::move(operationId)));
}

std::string ConversationHandoffHandle::operationId() const
{
  if (!m_state)
    throw DiError("INVALID_HANDLE", "local", "handoff", "conversation handoff handle is empty");
  return m_state->operationId;
}

ConversationHandoffStatus ConversationHandoffHandle::status() const
{
  if (!m_state)
    throw DiError("INVALID_HANDLE", "local", "handoff", "conversation handoff handle is empty");
  std::lock_guard<std::mutex> lock(m_state->mutex);
  return m_state->status;
}

void ConversationHandoffHandle::result(std::chrono::milliseconds waitTimeout) const
{
  if (!m_state)
    throw DiError("INVALID_HANDLE", "local", "handoff", "conversation handoff handle is empty");
  if (waitTimeout.count() < 0)
    throw DiError("INVALID_WAIT_TIMEOUT", "local", "wait",
                  "conversation handoff wait duration must be nonnegative",
                  m_state->operationId);
  const auto state = m_state;
  std::unique_lock<std::mutex> lock(state->mutex);
  if (waitTimeout.count() > 0 && state->isCoreIoThread && state->isCoreIoThread())
    throw DiError("CORE_IO_WAIT_FORBIDDEN", "local", "wait",
                  "conversation handoff result cannot block the Core I/O thread",
                  state->operationId);
  const auto terminal = [&] {
    return state->status == ConversationHandoffStatus::Succeeded ||
      state->status == ConversationHandoffStatus::Cancelled ||
      state->status == ConversationHandoffStatus::Failed;
  };
  if (!terminal())
    state->changed.wait_for(lock, waitTimeout, terminal);
  if (!terminal())
    throw DiError("LOCAL_WAIT_TIMEOUT", "local", "wait",
                  "conversation handoff result wait timed out", state->operationId);
  const auto status = state->status;
  const auto failure = state->failure;
  lock.unlock();
  if (status == ConversationHandoffStatus::Cancelled)
    throw DiError("CANCELLED", "conversation", "handoff",
                  "conversation handoff was cancelled", state->operationId);
  if (status == ConversationHandoffStatus::Failed) {
    if (failure) std::rethrow_exception(failure);
    throw DiError("CONVERSATION_HANDOFF_FAILED", "conversation", "handoff",
                  "conversation handoff failed", state->operationId);
  }
}

bool ConversationHandoffHandle::cancel() const
{
  if (!m_state)
    throw DiError("INVALID_HANDLE", "local", "handoff", "conversation handoff handle is empty");
  std::function<bool()> cancel;
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->status != ConversationHandoffStatus::Pending)
      return false;
    cancel = m_state->cancel;
  }
  return cancel && cancel();
}

void ConversationHandoffHandle::setCancel(std::function<bool()> cancel)
{
  if (!m_state) throw DiError("INVALID_HANDLE", "local", "handoff", "empty handoff handle");
  std::lock_guard<std::mutex> lock(m_state->mutex);
  if (m_state->status == ConversationHandoffStatus::Pending)
    m_state->cancel = std::move(cancel);
}

void ConversationHandoffHandle::setCoreIoThreadProbe(std::function<bool()> probe)
{
  if (!m_state) throw DiError("INVALID_HANDLE", "local", "handoff", "empty handoff handle");
  std::lock_guard<std::mutex> lock(m_state->mutex);
  m_state->isCoreIoThread = std::move(probe);
}

void ConversationHandoffHandle::setPromoted()
{
  if (!m_state) return;
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->status != ConversationHandoffStatus::Pending) return;
    m_state->status = ConversationHandoffStatus::PromotedAwaitingAppend;
    m_state->cancel = {};
  }
  m_state->changed.notify_all();
}

void ConversationHandoffHandle::setSucceeded()
{
  if (!m_state) return;
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->status != ConversationHandoffStatus::PromotedAwaitingAppend) return;
    m_state->status = ConversationHandoffStatus::Succeeded;
  }
  m_state->changed.notify_all();
}

void ConversationHandoffHandle::setSavedWithoutPlacement()
{
  if (!m_state) return;
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->status != ConversationHandoffStatus::Pending) return;
    m_state->status = ConversationHandoffStatus::Succeeded;
    m_state->cancel = {};
  }
  m_state->changed.notify_all();
}

void ConversationHandoffHandle::setCancelled()
{
  if (!m_state) return;
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->status != ConversationHandoffStatus::Pending) return;
    m_state->status = ConversationHandoffStatus::Cancelled;
    m_state->cancel = {};
  }
  m_state->changed.notify_all();
}

void ConversationHandoffHandle::setFailed(std::exception_ptr failure)
{
  if (!m_state) return;
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->status == ConversationHandoffStatus::Succeeded ||
        m_state->status == ConversationHandoffStatus::Cancelled ||
        m_state->status == ConversationHandoffStatus::Failed)
      return;
    m_state->status = ConversationHandoffStatus::Failed;
    m_state->failure = std::move(failure);
    m_state->cancel = {};
  }
  m_state->changed.notify_all();
}

void ConversationHandoffHandle::retainAppendCompletion(Subscription completion)
{
  if (!m_state) return;
  std::lock_guard<std::mutex> lock(m_state->mutex);
  if (m_state->status == ConversationHandoffStatus::PromotedAwaitingAppend)
    m_state->appendCompletion = std::move(completion);
}

struct NativeDepartureWatchState
{
  std::mutex mutex;
  std::condition_variable changed;
  std::shared_ptr<ndn_service_framework::OperationRuntime> worker;
  std::weak_ptr<NativeInferenceClient> client;
  NativeDepartureWatchResultV1 outcome;
  std::optional<ConversationHandoffHandle> save;
  bool cancellationRequested = false;
  bool saveStarting = false;
  bool published = false;

  void finish(NativeDepartureWatchStatusV1 status, std::string reason = {})
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (published) return;
      outcome.status = status;
      outcome.reason = std::move(reason);
      published = true;
    }
    changed.notify_all();
  }

  bool cancel() noexcept
  {
    std::optional<ConversationHandoffHandle> activeSave;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (published || cancellationRequested) return false;
      activeSave = save;
      if (!activeSave) {
        if (saveStarting) return false;
        cancellationRequested = true;
        changed.notify_all();
        return true;
      }
    }
    try {
      if (!activeSave->cancel()) return false;
    }
    catch (...) { return false; }
    {
      std::lock_guard<std::mutex> lock(mutex);
      cancellationRequested = true;
    }
    changed.notify_all();
    return true;
  }
};

bool NativeDepartureWatchHandle::valid() const noexcept
{
  return static_cast<bool>(m_state);
}

NativeDepartureWatchStatusV1 NativeDepartureWatchHandle::status() const
{
  if (!m_state)
    throw std::logic_error("DEPARTURE_WATCH_HANDLE_INVALID");
  std::lock_guard<std::mutex> lock(m_state->mutex);
  return m_state->outcome.status;
}

NativeDepartureWatchResultV1
NativeDepartureWatchHandle::result(std::chrono::milliseconds waitTimeout) const
{
  if (!m_state)
    throw std::logic_error("DEPARTURE_WATCH_HANDLE_INVALID");
  if (waitTimeout.count() < 0)
    throw std::invalid_argument("DEPARTURE_WATCH_WAIT_TIMEOUT_INVALID");
  if (m_state->worker && m_state->worker->isWorkerThread())
    throw std::logic_error("DEPARTURE_WATCH_WORKER_CANNOT_WAIT_ON_ITSELF");
  if (const auto client = m_state->client.lock(); client && client->isCoreIoThread())
    throw std::logic_error("DEPARTURE_WATCH_FACE_CANNOT_WAIT");
  std::unique_lock<std::mutex> lock(m_state->mutex);
  if (!m_state->changed.wait_for(lock, waitTimeout,
        [this] { return m_state->published; }))
    throw ndn_service_framework::OperationError(
      ndn_service_framework::OperationErrorCode::Timeout,
      "DEPARTURE_WATCH_WAIT_TIMEOUT");
  return m_state->outcome;
}

bool NativeDepartureWatchHandle::cancel() noexcept
{
  return m_state && m_state->cancel();
}

void NativeDepartureWatchHandle::close() noexcept
{
  (void)cancel();
}

struct Conversation::State
{
  explicit State(PreparedModel preparedModel,
                 std::shared_ptr<NativeInferenceClient> nativeClient,
                 std::shared_ptr<NativeConversationCoordinator> owner,
                 std::string id,
                 std::uint64_t configuredRetentionDeadline,
                 std::string service,
                 std::string security,
                 std::string tokenizer)
    : model(std::move(preparedModel)), client(std::move(nativeClient)), coordinator(std::move(owner)),
      conversationId(std::move(id)), configuredRetentionDeadlineMs(configuredRetentionDeadline),
      serviceName(std::move(service)),
      securityDomainDigest(std::move(security)), tokenizerDigest(std::move(tokenizer))
  {
  }

  mutable std::mutex mutex;
  std::condition_variable handoffChanged;
  PreparedModel model;
  std::shared_ptr<NativeInferenceClient> client;
  std::shared_ptr<NativeConversationCoordinator> coordinator;
  std::string conversationId;
  std::uint64_t configuredRetentionDeadlineMs = 0;
  std::string serviceName;
  std::string securityDomainDigest;
  std::string tokenizerDigest;
  bool closed = false;
  bool active = false;
  bool handoffStarting = false;
  bool handoffAppendStarted = false;
  std::uint64_t activeGeneration = 0;
  std::uint64_t nextGeneration = 0;
  RequestHandle activeHandle;
  Subscription completionSubscription;
  std::optional<ConversationHandoffHandle> handoffHandle;
  std::optional<NativeDepartureWatchHandle> departureWatch;
  std::string handoffBindingDigest;
};

Conversation::~Conversation() noexcept
{
  close();
}

Conversation& Conversation::operator=(Conversation&& other) noexcept
{
  if (this == &other)
    return *this;
  close();
  m_state = std::move(other.m_state);
  return *this;
}

void Conversation::requireIdle() const
{
  if (!m_state)
    throw DiError("INVALID_HANDLE", "local", "conversation", "conversation handle is empty");
  std::lock_guard<std::mutex> lock(m_state->mutex);
  if (m_state->closed)
    throw DiError("RUNTIME_CLOSED", "local", "conversation", "conversation is closed");
  if (m_state->active)
    throw DiError("CONVERSATION_TURN_IN_PROGRESS", "local", "conversation",
                  "conversation accepts only one active turn");
}

NativeConversationContinuation Conversation::makeContinuation(const Input& input) const
{
  if (!m_state || !m_state->coordinator)
    throw DiError("UNSUPPORTED_CAPABILITY", "local", "conversation",
                  "native conversation coordinator is unavailable");

  NativeConversationContinuation continuation;
  continuation.conversationId = m_state->conversationId;
  continuation.serviceName = m_state->serviceName;
  continuation.generationId = randomGenerationId();
  const auto inputTokens = m_state->model.conversationInputTokens(input);
  const auto current = m_state->coordinator->find(m_state->conversationId);
  if (!current) {
    continuation.mode = "FULL_CONTEXT";
    continuation.parentContextEpoch = 0;
    continuation.canonicalTokenIds = inputTokens;
    const auto now = nowMs();
    continuation.retentionDeadlineMs = m_state->configuredRetentionDeadlineMs != 0
      ? m_state->configuredRetentionDeadlineMs : now + 300'000;
    if (continuation.retentionDeadlineMs <= now)
      throw DiError("CONVERSATION_RETENTION_EXPIRED", "conversation", "request",
                    "configured conversation retention deadline has expired");
    return continuation;
  }

  continuation.mode = "APPEND_DELTA";
  continuation.parentContextEpoch = current->checkpoint.successorContextEpoch;
  continuation.parentCheckpointDigest = current->checkpoint.checkpointDigest;
  continuation.parentCheckpointWire = current->checkpoint.wire;
  continuation.planRoleMapDigest = current->planRoleMapDigest;
  continuation.expectedRoles = current->checkpoint.expectedRoles;
  continuation.expectedProvidersByRole = current->nextTurnProvidersByRole;
  continuation.retentionDeadlineMs = current->retentionDeadlineMs;
  std::vector<std::int64_t> parentTokens;
  try {
    parentTokens = current->checkpoint.transcript.at(
      "canonicalTokenIds").get<std::vector<std::int64_t>>();
  }
  catch (const std::exception& error) {
    throw DiError("CONVERSATION_STATE_UNAVAILABLE", "conversation", "checkpoint",
                  std::string("durable conversation transcript is unavailable: ") + error.what());
  }
  continuation.canonicalTokenIds = std::move(parentTokens);
  continuation.canonicalTokenIds.insert(continuation.canonicalTokenIds.end(),
                                         inputTokens.begin(), inputTokens.end());
  return continuation;
}

RequestHandle Conversation::request(const Input& input, const RequestOptions& options) const
{
  if (!m_state)
    throw DiError("INVALID_HANDLE", "local", "conversation", "conversation handle is empty");
  if (!m_state->model.capabilities().streaming)
    throw DiError("UNSUPPORTED_CAPABILITY", "local", "conversation",
                  "conversation requires an authenticated streaming model");
  if (options.stream && !options.stream->enabled)
    throw DiError("INVALID_ARGUMENT", "local", "conversation",
                  "conversation requests require enabled streaming");

  std::uint64_t generation = 0;
  std::optional<ConversationHandoffHandle> handoffForAppend;
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->closed)
      throw DiError("RUNTIME_CLOSED", "local", "conversation", "conversation is closed");
    if (m_state->handoffStarting)
      throw DiError("CONVERSATION_HANDOFF_IN_PROGRESS", "conversation", "request",
                    "conversation Provider switch has not reached promotion");
    if (m_state->active)
      throw DiError("CONVERSATION_TURN_IN_PROGRESS", "local", "conversation",
                    "conversation accepts only one active turn");
    if (m_state->handoffHandle) {
      const auto handoffStatus = m_state->handoffHandle->status();
      if (handoffStatus == ConversationHandoffStatus::Pending)
        throw DiError("CONVERSATION_HANDOFF_IN_PROGRESS", "conversation", "request",
                      "conversation Provider switch has not reached promotion");
      if (handoffStatus == ConversationHandoffStatus::PromotedAwaitingAppend) {
        if (m_state->handoffAppendStarted)
          throw DiError("CONVERSATION_HANDOFF_APPEND_IN_PROGRESS", "conversation", "request",
                        "the first destination append is already in progress");
        m_state->handoffAppendStarted = true;
        handoffForAppend = *m_state->handoffHandle;
      }
    }
    m_state->active = true;
    generation = ++m_state->nextGeneration;
    m_state->activeGeneration = generation;
    m_state->activeHandle = RequestHandle{};
  }

  try {
    auto continuation = makeContinuation(input);
    auto weakState = std::weak_ptr<State>(m_state);
    continuation.onTerminal = [weakState, generation] {
      if (const auto state = weakState.lock()) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->active && state->activeGeneration == generation) {
          state->active = false;
          state->activeHandle = RequestHandle{};
        }
      }
    };
    auto requestOptions = options;
    if (!continuation.expectedProvidersByRole.empty()) {
      std::set<std::string> expectedProviders;
      for (const auto& [role, provider] : continuation.expectedProvidersByRole) {
        (void)role;
        if (provider.empty())
          throw DiError("CONVERSATION_STATE_UNAVAILABLE", "conversation", "placement",
                        "persisted next-turn Provider map is invalid");
        expectedProviders.insert(provider);
      }
      if (!options.providerNames.empty()) {
        const std::set<std::string> requestedProviders(options.providerNames.begin(),
                                                       options.providerNames.end());
        if (requestedProviders.size() != options.providerNames.size() ||
            requestedProviders != expectedProviders)
          throw DiError("INVALID_ARGUMENT", "conversation", "placement",
                        "providerNames conflicts with the Owner-selected next-turn map");
      }
      requestOptions.providerNames.assign(expectedProviders.begin(), expectedProviders.end());
    }
    if (!requestOptions.stream)
      requestOptions.stream = StreamOptions{true};
    auto handle = m_state->model.requestInternal(input, requestOptions, continuation);

    bool cancelAfterClose = false;
    {
      std::lock_guard<std::mutex> lock(m_state->mutex);
      cancelAfterClose = m_state->closed;
      if (!cancelAfterClose)
        m_state->activeHandle = handle;
    }
    if (cancelAfterClose)
      handle.cancel();

    const auto weakHandoff = handoffForAppend
      ? std::weak_ptr<ConversationHandoffHandle::State>(handoffForAppend->m_state)
      : std::weak_ptr<ConversationHandoffHandle::State>{};
    auto completion = handle.onCompletion(
      [weakState, weakHandoff, generation](
        std::exception_ptr error, std::optional<Result> result) mutable {
        if (const auto handoffState = weakHandoff.lock()) {
          ConversationHandoffHandle handoff(handoffState);
          if (error) handoff.setFailed(error);
          else if (result) handoff.setSucceeded();
          else handoff.setFailed(std::make_exception_ptr(DiError(
            "CONVERSATION_APPEND_RESULT_MISSING", "conversation", "handoff",
            "the first destination APPEND completed without a result")));
        }
        if (const auto state = weakState.lock()) {
          std::lock_guard<std::mutex> lock(state->mutex);
          if (state->active && state->activeGeneration == generation) {
            state->active = false;
            state->activeHandle = RequestHandle{};
          }
        }
      });
    if (handoffForAppend)
      handoffForAppend->retainAppendCompletion(std::move(completion));
    else {
      std::lock_guard<std::mutex> lock(m_state->mutex);
      if (m_state->active && m_state->activeGeneration == generation &&
          !m_state->closed)
        m_state->completionSubscription = std::move(completion);
    }
    return handle;
  }
  catch (...) {
    const auto failure = std::current_exception();
    if (handoffForAppend)
      handoffForAppend->setFailed(failure);
    {
      std::lock_guard<std::mutex> lock(m_state->mutex);
      m_state->active = false;
      if (handoffForAppend)
        m_state->handoffAppendStarted = false;
      m_state->activeHandle = RequestHandle{};
    }
    throw;
  }
}

ConversationHandoffHandle Conversation::switchProviders(
  const std::map<std::string, std::string>& destinationProvidersByRole,
  const std::map<std::string, NativeRemoteProtectedKvRoleTrustPolicyV1>&
    trustedSourcePolicyByRole,
  std::chrono::steady_clock::time_point deadline) const
{
  if (!m_state || !m_state->client || !m_state->coordinator)
    throw DiError("INVALID_HANDLE", "local", "handoff", "conversation handle is empty");
  if (deadline <= std::chrono::steady_clock::now())
    throw DiError("INVALID_DEADLINE", "conversation", "handoff",
                  "conversation handoff deadline must be in the future");

  const auto current = m_state->coordinator->find(m_state->conversationId);
  if (!current)
    throw DiError("CHECKPOINT_NOT_READY", "conversation", "handoff",
                  "conversation has no durable committed checkpoint");
  const auto& checkpoint = current->checkpoint;
  const auto& sourceProvidersByRole = checkpoint.providersByRole;
  if (checkpoint.conversationId != m_state->conversationId ||
      checkpoint.checkpointDigest.empty() || checkpoint.expectedRoles.empty() ||
      checkpoint.expectedRoles.size() > 64 || sourceProvidersByRole.empty())
    throw DiError("CONVERSATION_STATE_UNAVAILABLE", "conversation", "handoff",
                  "current checkpoint has no complete source role map");

  std::set<std::string> expectedRoles(checkpoint.expectedRoles.begin(),
                                      checkpoint.expectedRoles.end());
  if (expectedRoles.size() != checkpoint.expectedRoles.size() ||
      expectedRoles.size() != sourceProvidersByRole.size() ||
      expectedRoles.size() != destinationProvidersByRole.size() ||
      expectedRoles.size() != trustedSourcePolicyByRole.size())
    throw DiError("INVALID_HANDOFF_ROLE_MAP", "conversation", "handoff",
                  "source, destination, and trust maps must exactly cover checkpoint roles");

  std::set<std::string> sourceProviderSet;
  std::set<std::string> destinationProviderSet;
  for (const auto& role : expectedRoles) {
    const auto source = sourceProvidersByRole.find(role);
    const auto destination = destinationProvidersByRole.find(role);
    const auto trust = trustedSourcePolicyByRole.find(role);
    if (source == sourceProvidersByRole.end() || destination == destinationProvidersByRole.end() ||
        trust == trustedSourcePolicyByRole.end() ||
        !isCanonicalName(source->second) || !isCanonicalName(destination->second))
      throw DiError("INVALID_HANDOFF_ROLE_MAP", "conversation", "handoff",
                    "handoff role map contains a missing role or invalid Provider name");
    sourceProviderSet.insert(source->second);
    destinationProviderSet.insert(destination->second);

    const auto& policy = trust->second;
    if (!isCanonicalName(policy.target.repoNode) ||
        !isCanonicalName(policy.target.providerIdentity) ||
        !isCanonicalName(policy.target.servicePrefix) ||
        !isCanonicalDigest(policy.producerVerifierPolicyDigest) ||
        !policy.verifyProducerSignature || !policy.limits.listingPageSize ||
        !policy.limits.maxListingPagesPerScan || !policy.limits.maxListedObjectsPerScan ||
        !policy.limits.maxListingWireBytes || !policy.limits.maxPacketCount ||
        !policy.limits.maxPacketWireBytes || !policy.limits.maxManifestWireBytes)
      throw DiError("INVALID_HANDOFF_TRUST_POLICY", "conversation", "handoff",
                    "trusted source policy is incomplete or outside its bounded contract");
  }
  if (destinationProviderSet.empty() || destinationProviderSet.size() > 16 ||
      destinationProvidersByRole == sourceProvidersByRole)
    throw DiError("INVALID_HANDOFF_DESTINATION", "conversation", "handoff",
                  "destination role map must differ from source and fit the Provider roster");

  const auto binding = handoffBindingDigest(checkpoint.checkpointDigest,
    sourceProvidersByRole, destinationProvidersByRole, trustedSourcePolicyByRole, deadline);
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
    deadline - std::chrono::steady_clock::now());
  if (remaining.count() <= 0)
    throw DiError("HANDOFF_DEADLINE_EXPIRED", "conversation", "handoff",
                  "conversation handoff deadline expired during validation");

  std::unique_lock<std::mutex> lock(m_state->mutex);
  while (m_state->handoffStarting) {
    if (m_state->handoffBindingDigest != binding)
      throw DiError("CONVERSATION_HANDOFF_CONFLICT", "conversation", "handoff",
                    "a different Provider switch is already starting");
    if (m_state->client->isWorkerThread() || m_state->client->isCoreIoThread())
      throw DiError("WOULD_DEADLOCK", "local", "handoff",
                    "duplicate Provider switch cannot wait on a Core-owned worker thread");
    if (!m_state->handoffChanged.wait_until(lock, deadline, [this] {
          return !m_state->handoffStarting;
        }))
      throw DiError("HANDOFF_DEADLINE_EXPIRED", "conversation", "handoff",
                    "deadline expired while joining Provider switch startup");
  }
  if (m_state->closed)
    throw DiError("RUNTIME_CLOSED", "local", "handoff", "conversation is closed");
  if (m_state->active)
    throw DiError("CONVERSATION_TURN_IN_PROGRESS", "conversation", "handoff",
                  "conversation cannot switch Providers during an active turn");
  if (m_state->handoffHandle) {
    const auto status = m_state->handoffHandle->status();
    if (status == ConversationHandoffStatus::Pending ||
        status == ConversationHandoffStatus::PromotedAwaitingAppend) {
      if (m_state->handoffBindingDigest != binding)
        throw DiError("CONVERSATION_HANDOFF_CONFLICT", "conversation", "handoff",
                      "a different Provider switch is already unsettled");
      return *m_state->handoffHandle;
    }
  }
  const auto previousBinding = m_state->handoffBindingDigest;
  m_state->handoffBindingDigest = binding;
  m_state->handoffStarting = true;
  lock.unlock();

  try {
    auto handle = m_state->client->switchConversationProviders(
      m_state->conversationId, checkpoint.checkpointDigest, sourceProvidersByRole,
      destinationProvidersByRole, trustedSourcePolicyByRole, deadline);
    bool closed = false;
    {
      std::lock_guard<std::mutex> guard(m_state->mutex);
      closed = m_state->closed;
      if (!closed) {
        m_state->handoffHandle = handle;
        m_state->handoffAppendStarted = false;
      }
      m_state->handoffStarting = false;
      if (closed)
        m_state->handoffBindingDigest = previousBinding;
    }
    m_state->handoffChanged.notify_all();
    if (closed) {
      (void)handle.cancel();
      throw DiError("RUNTIME_CLOSED", "local", "handoff",
                    "conversation closed while the Provider switch was starting");
    }
    return handle;
  }
  catch (...) {
    {
      std::lock_guard<std::mutex> guard(m_state->mutex);
      m_state->handoffStarting = false;
      if (!m_state->handoffHandle ||
          (m_state->handoffHandle->status() != ConversationHandoffStatus::Pending &&
           m_state->handoffHandle->status() != ConversationHandoffStatus::PromotedAwaitingAppend))
        m_state->handoffBindingDigest = previousBinding;
    }
    m_state->handoffChanged.notify_all();
    throw;
  }
}

NativeDepartureWatchHandle Conversation::watchProviderDeparture(
  const NativeDepartureWatchOptionsV1& options) const
{
  if (!m_state || !m_state->client || !m_state->coordinator)
    throw DiError("INVALID_HANDLE", "local", "watch", "conversation handle is empty");
  const auto now = std::chrono::steady_clock::now();
  if (options.deadline <= now || options.deadline - now > std::chrono::minutes(10) ||
      options.pollInterval.count() <= 0 || options.pollInterval > std::chrono::minutes(1) ||
      options.queryTimeout.count() <= 0 || options.queryTimeout > std::chrono::minutes(1) ||
      options.saveTimeout.count() <= 0 || options.saveTimeout > std::chrono::minutes(10) ||
      options.maxOutstandingQueries == 0 || options.maxOutstandingQueries > 16 ||
      options.maxConcurrentSaves == 0 || options.maxConcurrentSaves > 16)
    throw DiError("INVALID_DEPARTURE_WATCH_OPTIONS", "local", "watch",
                  "departure watch requires finite bounded polling and save budgets");
  const auto initial = m_state->coordinator->find(m_state->conversationId);
  if (!initial || initial->checkpoint.providersByRole.empty() ||
      initial->checkpoint.providersByRole.size() != options.trustedSourcePolicyByRole.size())
    throw DiError("DEPARTURE_WATCH_CHECKPOINT_NOT_READY", "conversation", "watch",
                  "departure watch requires a complete committed source and trust role set");
  for (const auto& [role, provider] : initial->checkpoint.providersByRole) {
    const auto policy = options.trustedSourcePolicyByRole.find(role);
    if (provider.empty() || policy == options.trustedSourcePolicyByRole.end() ||
        !policy->second.verifyProducerSignature)
      throw DiError("DEPARTURE_WATCH_TRUST_POLICY_INCOMPLETE", "authorization", "watch",
                    "departure watch lacks a Producer verifier for a committed source role");
  }

  std::lock_guard<std::mutex> conversationLock(m_state->mutex);
  if (m_state->closed)
    throw DiError("RUNTIME_CLOSED", "local", "watch", "conversation is closed");
  if (m_state->departureWatch) {
    const auto prior = m_state->departureWatch->status();
    if (prior == NativeDepartureWatchStatusV1::Watching ||
        prior == NativeDepartureWatchStatusV1::Saving)
      throw DiError("DEPARTURE_WATCH_ACTIVE", "conversation", "watch",
                    "conversation already has an active departure watch");
  }

  auto watch = std::make_shared<NativeDepartureWatchState>();
  watch->worker = ndn_service_framework::OperationRuntime::create();
  watch->client = m_state->client;
  watch->outcome.conversationId = m_state->conversationId;
  const std::weak_ptr<NativeDepartureWatchState> weakWatch = watch;
  m_state->client->registerDepartureWatchLifecycle(
    std::weak_ptr<void>(watch),
    [weakWatch] { if (const auto current = weakWatch.lock()) (void)current->cancel(); },
    [weakWatch](std::chrono::milliseconds timeout) {
      const auto current = weakWatch.lock();
      return !current || current->worker->drain(timeout);
    },
    [weakWatch] {
      const auto current = weakWatch.lock();
      return !current || current->worker->isQuiescent();
    },
    [weakWatch](std::function<void()> notifier) {
      if (const auto current = weakWatch.lock())
        current->worker->setNotifyCallback(std::move(notifier));
    });
  auto ticket = std::make_shared<ndn_service_framework::OperationRuntime::WorkTicket>(
    watch->worker->acquire());
  const std::weak_ptr<State> weakConversation = m_state;
  auto client = m_state->client;
  auto coordinator = m_state->coordinator;
  const auto conversationId = m_state->conversationId;
  watch->worker->post(*ticket, [watch, ticket, weakConversation, client,
                                coordinator, conversationId, options] {
    const auto cancelled = [watch] {
      std::lock_guard<std::mutex> lock(watch->mutex);
      return watch->cancellationRequested;
    };
    const auto pause = [watch, &options] {
      std::unique_lock<std::mutex> lock(watch->mutex);
      watch->changed.wait_until(lock,
        std::min(options.deadline, std::chrono::steady_clock::now() + options.pollInterval),
        [watch] { return watch->cancellationRequested; });
    };
    const auto conversationBusy = [&weakConversation] {
      const auto conversation = weakConversation.lock();
      if (!conversation) return -1;
      std::lock_guard<std::mutex> lock(conversation->mutex);
      if (conversation->closed) return -1;
      if (conversation->active || conversation->handoffStarting) return 1;
      if (conversation->handoffHandle) {
        const auto status = conversation->handoffHandle->status();
        if (status == ConversationHandoffStatus::Pending ||
            status == ConversationHandoffStatus::PromotedAwaitingAppend)
          return 1;
      }
      return 0;
    };
    try {
      for (;;) {
        if (cancelled() || conversationBusy() < 0) {
          watch->finish(NativeDepartureWatchStatusV1::Cancelled, "CANCELLED");
          return;
        }
        if (std::chrono::steady_clock::now() >= options.deadline) {
          watch->finish(NativeDepartureWatchStatusV1::Expired, "NO_POSITIVE_INTENT");
          return;
        }
        if (conversationBusy() > 0) { pause(); continue; }
        const auto current = coordinator->find(conversationId);
        if (!current || current->checkpoint.providersByRole.empty()) {
          watch->finish(NativeDepartureWatchStatusV1::Failed,
                        "CHECKPOINT_NOT_READY");
          return;
        }
        const auto sourceProviders = current->checkpoint.providersByRole;
        if (sourceProviders.size() != options.trustedSourcePolicyByRole.size()) {
          watch->finish(NativeDepartureWatchStatusV1::Failed,
                        "TRUST_POLICY_STALE");
          return;
        }
        std::set<std::string> providers;
        for (const auto& [role, provider] : sourceProviders) {
          const auto policy = options.trustedSourcePolicyByRole.find(role);
          if (policy == options.trustedSourcePolicyByRole.end() ||
              !policy->second.verifyProducerSignature) {
            watch->finish(NativeDepartureWatchStatusV1::Failed,
                          "TRUST_POLICY_STALE");
            return;
          }
          providers.insert(provider);
        }
        bool restart = false;
        for (const auto& provider : providers) {
          if (cancelled() || conversationBusy() != 0) { restart = true; break; }
          const auto queryDeadline = std::min(options.deadline,
            std::chrono::steady_clock::now() + options.queryTimeout);
          if (queryDeadline <= std::chrono::steady_clock::now()) break;
          NativeWithdrawalQueryReplyV1 reply;
          try {
            reply = client->queryProviderWithdrawalIntent(
              conversationId, current->checkpoint.checkpointDigest, provider,
              [&options, &sourceProviders, &provider](const ndn::Data& data) {
                for (const auto& [role, source] : sourceProviders) {
                  if (source != provider) continue;
                  const auto& policy = options.trustedSourcePolicyByRole.at(role);
                  if (!policy.verifyProducerSignature(data)) return false;
                }
                return true;
              }, queryDeadline);
          }
          catch (const NativeDiError& error) {
            if (error.code() == "RUNTIME_CLOSED") {
              watch->finish(NativeDepartureWatchStatusV1::Cancelled,
                            error.code());
              return;
            }
            if (error.code() == "HANDOFF_PROVIDER_TIMEOUT" ||
                error.code() == "HANDOFF_DEADLINE_EXPIRED" ||
                error.code() == "HANDOFF_PROVIDER_REQUEST_FAILED" ||
                error.code() == "HANDOFF_DISPATCH_FAILED" ||
                error.code() == "WITHDRAWAL_QUERY_CHECKPOINT_CHANGED") {
              restart = true;
              continue;
            }
            throw;
          }
          if (reply.status != NativeWithdrawalQueryStatusV1::Intent)
            continue;
          if (cancelled() || conversationBusy() != 0) { restart = true; break; }
          const auto latest = coordinator->find(conversationId);
          if (!latest ||
              latest->checkpoint.checkpointDigest != current->checkpoint.checkpointDigest ||
              latest->checkpoint.providersByRole != sourceProviders) {
            restart = true;
            break;
          }
          const auto systemNowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
          if (!reply.intent || reply.intent->deadlineMs <=
                static_cast<std::uint64_t>(systemNowMs)) {
            restart = true;
            break;
          }
          const auto now = std::chrono::steady_clock::now();
          const auto intentRemaining = std::min<std::uint64_t>(
            reply.intent->deadlineMs - static_cast<std::uint64_t>(systemNowMs),
            static_cast<std::uint64_t>(std::chrono::duration_cast<
              std::chrono::milliseconds>(options.deadline - now).count()));
          const auto saveDeadline = std::min({options.deadline,
            now + options.saveTimeout,
            now + std::chrono::milliseconds(intentRemaining)});
          if (saveDeadline <= now) { restart = true; break; }
          const auto conversation = weakConversation.lock();
          if (!conversation) {
            watch->finish(NativeDepartureWatchStatusV1::Cancelled, "CANCELLED");
            return;
          }
          std::unique_lock<std::mutex> conversationLock(conversation->mutex);
          bool busy = conversation->closed || conversation->active ||
                      conversation->handoffStarting;
          if (conversation->handoffHandle) {
            const auto status = conversation->handoffHandle->status();
            busy = busy || status == ConversationHandoffStatus::Pending ||
                   status == ConversationHandoffStatus::PromotedAwaitingAppend;
          }
          if (busy) { restart = true; break; }
          {
            std::lock_guard<std::mutex> lock(watch->mutex);
            if (watch->cancellationRequested) { restart = true; break; }
            watch->saveStarting = true;
            watch->outcome.status = NativeDepartureWatchStatusV1::Saving;
            watch->outcome.checkpointDigest = current->checkpoint.checkpointDigest;
            watch->outcome.providerIdentity = provider;
            watch->outcome.withdrawalId = reply.intent->withdrawalId;
          }
          auto save = client->saveConversationForWithdrawal(
            conversationId, current->checkpoint.checkpointDigest,
            sourceProviders, options.trustedSourcePolicyByRole, provider,
            reply, saveDeadline);
          {
            std::lock_guard<std::mutex> lock(watch->mutex);
            watch->outcome.saveOperationId = save.operationId();
            watch->save = save;
            watch->saveStarting = false;
          }
          conversationLock.unlock();
          watch->changed.notify_all();
          while (save.status() == ConversationHandoffStatus::Pending) {
            if (cancelled()) (void)save.cancel();
            std::unique_lock<std::mutex> lock(watch->mutex);
            watch->changed.wait_for(lock, std::chrono::milliseconds(20));
          }
          if (save.status() == ConversationHandoffStatus::Succeeded)
            watch->finish(NativeDepartureWatchStatusV1::Saved);
          else if (save.status() == ConversationHandoffStatus::Cancelled)
            watch->finish(NativeDepartureWatchStatusV1::Cancelled, "CANCELLED");
          else {
            std::string reason = "WITHDRAWAL_SAVE_FAILED";
            try { save.result(std::chrono::milliseconds(0)); }
            catch (const std::exception& error) { reason = error.what(); }
            watch->finish(NativeDepartureWatchStatusV1::Failed, std::move(reason));
          }
          return;
        }
        if (restart || !providers.empty()) pause();
      }
    }
    catch (const std::exception& error) {
      watch->finish(NativeDepartureWatchStatusV1::Failed, error.what());
    }
    catch (...) {
      watch->finish(NativeDepartureWatchStatusV1::Failed,
                    "DEPARTURE_WATCH_UNKNOWN_FAILURE");
    }
  });
  NativeDepartureWatchHandle handle(watch);
  m_state->departureWatch = handle;
  return handle;
}

ConversationCheckpoint Conversation::checkpoint() const
{
  if (!m_state || !m_state->coordinator)
    throw DiError("INVALID_HANDLE", "local", "conversation", "conversation handle is empty");
  const auto current = m_state->coordinator->find(m_state->conversationId);
  if (!current)
    throw DiError("CHECKPOINT_NOT_READY", "conversation", "checkpoint",
                  "conversation has no durable committed checkpoint");
  return ConversationCheckpoint(current->checkpoint.wire);
}

void Conversation::exportCheckpoint(const std::filesystem::path& destination) const
{
  if (!m_state || !m_state->coordinator)
    throw DiError("INVALID_HANDLE", "local", "conversation", "conversation handle is empty");
  if (m_state->client && m_state->client->isWorkerThread())
    throw DiError("WOULD_DEADLOCK", "local", "export",
                  "blocking checkpoint export from Core worker would deadlock");
  const auto current = m_state->coordinator->find(m_state->conversationId);
  if (!current)
    throw DiError("CHECKPOINT_NOT_READY", "conversation", "export",
                  "conversation has no durable committed checkpoint");
  try {
    const auto state = nativeParseJson(current->checkpoint.wire);
    nativeExportPrivateCheckpoint(destination, state);
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw DiError("CHECKPOINT_EXPORT_FAILED", "conversation", "export", error.what());
  }
}

void Conversation::close() noexcept
{
  if (!m_state)
    return;
  RequestHandle active;
  std::optional<ConversationHandoffHandle> handoff;
  std::optional<NativeDepartureWatchHandle> departureWatch;
  bool handoffAppendStarted = false;
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->closed)
      return;
    m_state->closed = true;
    active = m_state->activeHandle;
    handoff = m_state->handoffHandle;
    departureWatch = m_state->departureWatch;
    handoffAppendStarted = m_state->handoffAppendStarted;
  }
  active.cancel();
  if (departureWatch) departureWatch->close();
  if (handoff) {
    bool cancelled = false;
    try { cancelled = handoff->cancel(); }
    catch (...) {}
    if (!cancelled && !handoffAppendStarted) {
      try {
        handoff->setFailed(std::make_exception_ptr(DiError(
          "RUNTIME_CLOSED", "conversation", "handoff",
          "conversation closed before the first destination APPEND completed")));
      }
      catch (...) { handoff->setFailed(std::current_exception()); }
    }
  }
}

Conversation PreparedModel::openConversation(const ConversationOptions& options) const
{
  return openConversationInternal(options);
}

Conversation PreparedModel::openConversationInternal(const ConversationOptions& options) const
{
  if (!m_package || !m_clientFactory)
    throw DiError("RUNTIME_CLOSED", "local", "conversation",
                  "prepared model is not bound to a native Runtime client");
  if (!m_package->capabilities.conversations)
    throw DiError("UNSUPPORTED_CAPABILITY", "local", "conversation",
                  "verified model does not enable conversations");
  if (!m_package->capabilities.streaming)
    throw DiError("UNSUPPORTED_CAPABILITY", "local", "conversation",
                  "conversation requires an authenticated streaming model");

  std::shared_ptr<NativeInferenceClient> client;
  try {
    client = m_clientFactory(m_package);
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw DiError("RUNTIME_CLOSED", "local", "conversation", error.what());
  }
  const auto coordinator = client ? client->conversationCoordinator() : nullptr;
  if (!coordinator)
    throw DiError("UNSUPPORTED_CAPABILITY", "local", "conversation",
                  "Runtime has no configured native conversation coordinator");

  const auto service = requestString(*m_package, "service");
  const auto security = requestString(*m_package, "security_policy_digest");
  const auto tokenizer = requestString(*m_package, "tokenizer_digest");
  if (tokenizer.empty())
    throw DiError("UNSUPPORTED_CAPABILITY", "local", "conversation",
                  "conversation requires a pinned tokenizer digest");

  std::uint64_t configuredRetentionDeadline = 0;
  try {
    configuredRetentionDeadline = configuredRetentionDeadlineFromPackage(*m_package);
  }
  catch (const std::exception& error) {
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "conversation",
                  std::string("verified conversation retention is invalid: ") + error.what());
  }
  if (options.retentionDeadlineMs && *options.retentionDeadlineMs == 0)
    throw DiError("INVALID_ARGUMENT", "local", "conversation",
                  "retention deadline must be positive");
  const auto retentionDeadline = options.retentionDeadlineMs.value_or(
    configuredRetentionDeadline);
  if (retentionDeadline != 0 && retentionDeadline <= nowMs())
    throw DiError("CONVERSATION_RETENTION_EXPIRED", "conversation", "open",
                  "configured conversation retention deadline has expired");

  std::string id = options.conversationId;
  if (options.checkpoint) {
    const auto header = parseCheckpointWire(options.checkpoint->m_wire);
    const auto checkpointId = header.at("conversationId").get<std::string>();
    if (id.empty()) id = checkpointId;
    if (id != checkpointId)
      throw DiError("CONVERSATION_STATE_CONFLICT", "conversation", "checkpoint",
                    "conversation id does not match checkpoint");
    const auto current = coordinator->find(id);
    if (!current || current->checkpoint.wire != options.checkpoint->m_wire)
      throw DiError("CONVERSATION_STATE_UNAVAILABLE", "conversation", "checkpoint",
                    "checkpoint is not the current authenticated journal state");
    if (current->checkpoint.modelContractDigest != m_package->catalog.model.descriptor.intentDigest())
      throw DiError("CONVERSATION_STATE_CONFLICT", "conversation", "checkpoint",
                    "checkpoint model identity does not match PreparedModel");
    if (current->serviceName != service ||
        current->checkpoint.transcript.value("tokenizerDigest", std::string{}) != tokenizer)
      throw DiError("CONVERSATION_STATE_CONFLICT", "conversation", "checkpoint",
                    "checkpoint task or tokenizer identity does not match PreparedModel");
  }
  else if (id.empty()) {
    id = randomHexId();
  }
  if (id.size() < 16 || id.find_first_of("/\\") != std::string::npos)
    throw DiError("INVALID_ARGUMENT", "local", "conversation", "conversation id is invalid");

  return Conversation(std::make_shared<Conversation::State>(
    PreparedModel(m_package, m_receipt, m_lease, m_clientFactory), client, coordinator,
    std::move(id), retentionDeadline, service, security, tokenizer));
}

} // namespace ndnsf::di
