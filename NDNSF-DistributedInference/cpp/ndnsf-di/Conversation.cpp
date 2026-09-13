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
#include <limits>
#include <mutex>
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

struct Conversation::State
{
  explicit State(PreparedModel preparedModel,
                 std::shared_ptr<NativeInferenceClient> nativeClient,
                 std::shared_ptr<NativeConversationCoordinator> owner,
                 std::string id,
                 std::string service,
                 std::string security,
                 std::string tokenizer)
    : model(std::move(preparedModel)), client(std::move(nativeClient)), coordinator(std::move(owner)),
      conversationId(std::move(id)), serviceName(std::move(service)),
      securityDomainDigest(std::move(security)), tokenizerDigest(std::move(tokenizer))
  {
  }

  mutable std::mutex mutex;
  PreparedModel model;
  std::shared_ptr<NativeInferenceClient> client;
  std::shared_ptr<NativeConversationCoordinator> coordinator;
  std::string conversationId;
  std::string serviceName;
  std::string securityDomainDigest;
  std::string tokenizerDigest;
  bool closed = false;
  bool active = false;
  std::uint64_t activeGeneration = 0;
  std::uint64_t nextGeneration = 0;
  RequestHandle activeHandle;
  Subscription completionSubscription;
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
    continuation.retentionDeadlineMs = nowMs() + 300'000;
    return continuation;
  }

  continuation.mode = "APPEND_DELTA";
  continuation.parentContextEpoch = current->checkpoint.successorContextEpoch;
  continuation.parentCheckpointDigest = current->checkpoint.checkpointDigest;
  continuation.parentCheckpointWire = current->checkpoint.wire;
  continuation.planRoleMapDigest = current->planRoleMapDigest;
  continuation.expectedRoles = current->checkpoint.expectedRoles;
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
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->closed)
      throw DiError("RUNTIME_CLOSED", "local", "conversation", "conversation is closed");
    if (m_state->active)
      throw DiError("CONVERSATION_TURN_IN_PROGRESS", "local", "conversation",
                    "conversation accepts only one active turn");
    m_state->active = true;
    generation = ++m_state->nextGeneration;
    m_state->activeGeneration = generation;
    m_state->activeHandle = RequestHandle{};
  }

  try {
    auto continuation = makeContinuation(input);
    auto requestOptions = options;
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

    auto weakState = std::weak_ptr<State>(m_state);
    auto completion = handle.onCompletion(
      [weakState, generation](std::exception_ptr, std::optional<Result>) {
        if (const auto state = weakState.lock()) {
          std::lock_guard<std::mutex> lock(state->mutex);
          if (state->active && state->activeGeneration == generation) {
            state->active = false;
            state->activeHandle = RequestHandle{};
          }
        }
      });
    {
      std::lock_guard<std::mutex> lock(m_state->mutex);
      if (m_state->active && m_state->activeGeneration == generation &&
          !m_state->closed)
        m_state->completionSubscription = std::move(completion);
    }
    return handle;
  }
  catch (...) {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->active = false;
    m_state->activeHandle = RequestHandle{};
    throw;
  }
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
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->closed)
      return;
    m_state->closed = true;
    active = m_state->activeHandle;
  }
  active.cancel();
}

Conversation PreparedModel::openConversation(const ConversationOptions& options) const
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
    std::move(id), service, security, tokenizer));
}

} // namespace ndnsf::di
