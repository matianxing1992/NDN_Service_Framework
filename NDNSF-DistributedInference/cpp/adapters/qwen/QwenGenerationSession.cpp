#include "NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

#include <openssl/sha.h>

namespace ndnsf::di {
namespace {

using boost::property_tree::ptree;

const std::regex SPEC107_CANDIDATE_RE(
  R"(^spec107-c1(-[0-9a-f]{12}){6}$)");
const std::regex SPEC110_CANDIDATE_RE(
  R"(^spec110-c1(-[0-9a-f]{12}){6}$)");
const std::regex DIGEST_RE(R"(^sha256:[0-9a-f]{64}$)");

void
require(bool condition, const std::string& reason)
{
  if (!condition) {
    throw std::invalid_argument(reason);
  }
}

void
rejectForbiddenFields(const ptree& node)
{
  static const std::set<std::string> forbidden{
    "prompt", "payload", "tensor", "kv", "kvValue", "token", "tokenValue",
    "secret", "privateKey", "userToken", "providerToken",
  };
  for (const auto& item : node) {
    if (forbidden.count(item.first) != 0) {
      throw std::invalid_argument(
        "qwen generation session contains forbidden field: " + item.first);
    }
    rejectForbiddenFields(item.second);
  }
}

std::string
writeJson(const ptree& root)
{
  std::ostringstream output;
  boost::property_tree::write_json(output, root, false);
  auto value = output.str();
  if (!value.empty() && value.back() == '\n') {
    value.pop_back();
  }
  return value;
}

ptree
readJson(const std::string& json)
{
  std::istringstream input(json);
  ptree root;
  boost::property_tree::read_json(input, root);
  return root;
}

std::size_t
kindIndex(QwenResourceKind kind)
{
  const auto value = static_cast<std::size_t>(kind);
  if (value >= static_cast<std::size_t>(QwenResourceKind::Count)) {
    throw std::invalid_argument("unknown qwen generation resource kind");
  }
  return value;
}

} // namespace

void
DecodeStateComponentV1::validate() const
{
  if (name.empty() || dtype.empty() || shape.empty() ||
      std::any_of(shape.begin(), shape.end(), [] (std::int64_t value) {
        return value < 0;
      }) || !std::regex_match(digest, DIGEST_RE)) {
    throw std::invalid_argument("invalid decode state component");
  }
}

void
DecodeStateBundleV1::validate() const
{
  identity.validate();
  if (fullAttentionKv.empty() || recurrentConvolution.empty()) {
    throw std::invalid_argument(
      "decode state requires attention KV and recurrent/convolution state");
  }
  std::set<std::string> names;
  std::vector<std::string> digests;
  for (const auto* family : {&fullAttentionKv, &recurrentConvolution}) {
    for (const auto& component : *family) {
      component.validate();
      if (!names.insert(component.name).second) {
        throw std::invalid_argument("duplicate decode state component");
      }
      digests.push_back(component.digest);
    }
  }
  if (digests != identity.stateComponentDigests ||
      tokenEpoch != identity.prefixTokenCount) {
    throw std::invalid_argument("decode state component/epoch binding mismatch");
  }
}

std::string
DecodeStateBundleV1::digest() const
{
  validate();
  std::ostringstream input;
  input << identity.modelDigest << '|' << identity.graphSemanticDigest << '|'
        << identity.artifactDigest << '|' << identity.roleName << '|'
        << identity.layerBegin << ':' << identity.layerEnd << '|'
        << identity.prefixDigest << '|' << identity.prefixTokenCount << '|'
        << identity.positionDigest << '|' << identity.stateSchemaDigest << '|'
        << tokenEpoch;
  for (const auto& component : fullAttentionKv) {
    input << '|' << component.name << ':' << component.digest;
  }
  for (const auto& component : recurrentConvolution) {
    input << '|' << component.name << ':' << component.digest;
  }
  std::array<unsigned char, SHA256_DIGEST_LENGTH> hash{};
  const auto text = input.str();
  SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), hash.data());
  std::ostringstream output;
  output << "sha256:" << std::hex << std::setfill('0');
  for (const auto byte : hash) output << std::setw(2) << static_cast<unsigned int>(byte);
  return output.str();
}

DecodeStateTransactionV1::DecodeStateTransactionV1(DecodeStateBundleV1 committed)
  : m_committed(std::move(committed))
{
  m_committed.validate();
}

const DecodeStateBundleV1&
DecodeStateTransactionV1::committed() const noexcept
{
  return m_committed;
}

void
DecodeStateTransactionV1::apply(
  const DecodeStateBundleV1& candidate,
  std::function<bool(const DecodeStateBundleV1&)> admit)
{
  candidate.validate();
  const auto& current = m_committed.identity;
  const auto& next = candidate.identity;
  if (next.modelDigest != current.modelDigest ||
      next.graphSemanticDigest != current.graphSemanticDigest ||
      next.artifactDigest != current.artifactDigest ||
      next.adapterDigest != current.adapterDigest ||
      next.tokenizerDigest != current.tokenizerDigest ||
      next.runnerDigest != current.runnerDigest || next.roleName != current.roleName ||
      next.roleSplitDigest != current.roleSplitDigest ||
      next.layerBegin != current.layerBegin || next.layerEnd != current.layerEnd ||
      next.positionDigest != current.positionDigest || next.precision != current.precision ||
      next.layoutDigest != current.layoutDigest ||
      next.stateSchemaDigest != current.stateSchemaDigest ||
      next.stateComponentDigests != current.stateComponentDigests ||
      next.runtimeAbiDigest != current.runtimeAbiDigest ||
      next.securityDomainDigest != current.securityDomainDigest ||
      next.providerIdentity != current.providerIdentity ||
      next.providerBootId != current.providerBootId ||
      next.requestId != current.requestId || next.attemptEpoch != current.attemptEpoch ||
      next.generationId != current.generationId) {
    throw std::invalid_argument("decode state immutable binding mismatch");
  }
  if (next.prefixTokenCount != current.prefixTokenCount + 1 ||
      candidate.tokenEpoch != m_committed.tokenEpoch + 1) {
    throw std::invalid_argument("decode state prefix is not contiguous");
  }
  if (next.cacheEpoch < current.cacheEpoch) {
    throw std::invalid_argument("decode state cache epoch regressed");
  }
  if (admit && !admit(candidate)) {
    throw std::invalid_argument("decode state downstream admission rejected");
  }
  m_committed = candidate;
}

void
QwenGenerationSessionSpec::validate() const
{
  require(schema == "ndnsf-di-qwen-generation-session-v1",
          "unsupported qwen generation session schema");
  require(std::regex_match(candidateId, SPEC107_CANDIDATE_RE) ||
            std::regex_match(candidateId, SPEC110_CANDIDATE_RE),
          "invalid qwen generation candidate id");
  require(std::regex_match(planDigest, DIGEST_RE), "invalid qwen generation plan digest");
  require(std::regex_match(modelDigest, DIGEST_RE), "invalid qwen generation model digest");
  require(std::regex_match(artifactDigest, DIGEST_RE),
          "invalid qwen generation artifact digest");
  require(!logicalSessionId.empty() && !requestId.empty(),
          "qwen generation session identity missing");
  require(!serviceName.empty() && serviceName.front() == '/',
          "invalid qwen generation service name");
  require(attemptEpoch >= 1 && attemptEpoch <= 2,
          "qwen generation attempt bound exceeded");
  require(inputTokenCount >= 1 && inputTokenCount <= 512,
          "qwen generation input token bound exceeded");
  require(maxGeneratedTokens >= 1 && maxGeneratedTokens <= 64,
          "qwen generation output token bound exceeded");
  require(tokenEpoch < maxGeneratedTokens, "qwen generation token epoch out of range");
  require(deadlineEpochMs > 0, "qwen generation deadline missing");
  require(!contextReference.empty() && !feedbackTopic.empty(),
          "qwen generation object reference missing");
  require(roles.size() == 3, "qwen generation requires exactly three roles");
  const std::array<std::string, 3> expectedRoles{
    "/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1", "/LLM/Pipeline/Stage/2",
  };
  for (std::size_t index = 0; index < roles.size(); ++index) {
    const auto& binding = roles[index];
    require(binding.role == expectedRoles[index], "qwen generation role order mismatch");
    require(!binding.provider.empty() && !binding.providerBootId.empty(),
            "qwen generation provider binding missing");
  }
}

std::string
qwenGenerationSessionSpecToJson(const QwenGenerationSessionSpec& spec)
{
  spec.validate();
  ptree root;
  root.put("schema", spec.schema);
  root.put("candidateId", spec.candidateId);
  root.put("planDigest", spec.planDigest);
  root.put("modelDigest", spec.modelDigest);
  root.put("artifactDigest", spec.artifactDigest);
  root.put("logicalSessionId", spec.logicalSessionId);
  root.put("requestId", spec.requestId);
  root.put("serviceName", spec.serviceName);
  root.put("attemptEpoch", spec.attemptEpoch);
  root.put("tokenEpoch", spec.tokenEpoch);
  root.put("inputTokenCount", spec.inputTokenCount);
  root.put("maxGeneratedTokens", spec.maxGeneratedTokens);
  root.put("deadlineEpochMs", spec.deadlineEpochMs);
  root.put("contextReference", spec.contextReference);
  root.put("feedbackTopic", spec.feedbackTopic);
  ptree rolesNode;
  for (const auto& binding : spec.roles) {
    ptree role;
    role.put("role", binding.role);
    role.put("provider", binding.provider);
    role.put("providerBootId", binding.providerBootId);
    rolesNode.push_back({"", role});
  }
  root.add_child("roles", rolesNode);
  return writeJson(root);
}

QwenGenerationSessionSpec
qwenGenerationSessionSpecFromJson(const std::string& json)
{
  const auto root = readJson(json);
  rejectForbiddenFields(root);
  QwenGenerationSessionSpec spec;
  spec.schema = root.get<std::string>("schema", "");
  spec.candidateId = root.get<std::string>("candidateId", "");
  spec.planDigest = root.get<std::string>("planDigest", "");
  spec.modelDigest = root.get<std::string>("modelDigest", "");
  spec.artifactDigest = root.get<std::string>("artifactDigest", "");
  spec.logicalSessionId = root.get<std::string>("logicalSessionId", "");
  spec.requestId = root.get<std::string>("requestId", "");
  spec.serviceName = root.get<std::string>("serviceName", "");
  spec.attemptEpoch = root.get<std::uint64_t>("attemptEpoch", 2);
  spec.tokenEpoch = root.get<std::uint32_t>("tokenEpoch", 33);
  spec.inputTokenCount = root.get<std::uint32_t>("inputTokenCount", 0);
  spec.maxGeneratedTokens = root.get<std::uint32_t>("maxGeneratedTokens", 0);
  spec.deadlineEpochMs = root.get<std::uint64_t>("deadlineEpochMs", 0);
  spec.contextReference = root.get<std::string>("contextReference", "");
  spec.feedbackTopic = root.get<std::string>("feedbackTopic", "");
  if (const auto roles = root.get_child_optional("roles")) {
    for (const auto& item : roles.get()) {
      spec.roles.push_back({
        item.second.get<std::string>("role", ""),
        item.second.get<std::string>("provider", ""),
        item.second.get<std::string>("providerBootId", ""),
      });
    }
  }
  spec.validate();
  return spec;
}

const char*
toString(QwenGenerationState state) noexcept
{
  switch (state) {
    case QwenGenerationState::Created: return "CREATED";
    case QwenGenerationState::Selecting: return "SELECTING";
    case QwenGenerationState::Preparing: return "PREPARING";
    case QwenGenerationState::Prefilling: return "PREFILLING";
    case QwenGenerationState::Decoding: return "DECODING";
    case QwenGenerationState::Draining: return "DRAINING";
    case QwenGenerationState::Rebuilding: return "REBUILDING";
    case QwenGenerationState::Completed: return "COMPLETED";
    case QwenGenerationState::Terminal: return "TERMINAL";
    case QwenGenerationState::Cancelled: return "CANCELLED";
  }
  return "UNKNOWN";
}

const char*
toString(QwenGenerationFinishReason reason) noexcept
{
  switch (reason) {
    case QwenGenerationFinishReason::Eos: return "EOS";
    case QwenGenerationFinishReason::StopSequence: return "STOP_SEQUENCE";
    case QwenGenerationFinishReason::MaxTokens: return "MAX_TOKENS";
    case QwenGenerationFinishReason::ApplicationComplete:
      return "APPLICATION_COMPLETE";
  }
  return "UNKNOWN";
}

const char*
toString(QwenGenerationTerminal reason) noexcept
{
  switch (reason) {
    case QwenGenerationTerminal::None: return "NONE";
    case QwenGenerationTerminal::ProviderLost: return "PROVIDER_LOST";
    case QwenGenerationTerminal::DependencyMissing: return "DEPENDENCY_MISSING";
    case QwenGenerationTerminal::DependencyHashMismatch: return "DEPENDENCY_HASH_MISMATCH";
    case QwenGenerationTerminal::CacheMissFullContextRequired:
      return "CACHE_MISS_FULL_CONTEXT_REQUIRED";
    case QwenGenerationTerminal::NoCompatibleReplacement:
      return "NO_COMPATIBLE_REPLACEMENT";
    case QwenGenerationTerminal::RequestDeadline: return "REQUEST_DEADLINE";
    case QwenGenerationTerminal::AttemptCancelled: return "ATTEMPT_CANCELLED";
  }
  return "UNKNOWN";
}

QwenGenerationSessionStateMachine::QwenGenerationSessionStateMachine(
  QwenGenerationSessionSpec spec)
  : m_spec(std::move(spec))
  , m_attemptEpoch(m_spec.attemptEpoch)
  , m_generatedTokenCount(m_spec.tokenEpoch)
{
  m_spec.validate();
}

QwenGenerationState
QwenGenerationSessionStateMachine::state() const noexcept
{
  return m_state;
}

QwenGenerationTerminal
QwenGenerationSessionStateMachine::terminalReason() const noexcept
{
  return m_terminalReason;
}

QwenGenerationFinishReason
QwenGenerationSessionStateMachine::finishReason() const noexcept
{
  return m_finishReason;
}

std::uint64_t
QwenGenerationSessionStateMachine::attemptEpoch() const noexcept
{
  return m_attemptEpoch;
}

std::uint32_t
QwenGenerationSessionStateMachine::generatedTokenCount() const noexcept
{
  return m_generatedTokenCount;
}

bool
QwenGenerationSessionStateMachine::isTerminal() const noexcept
{
  return m_state == QwenGenerationState::Completed ||
         m_state == QwenGenerationState::Terminal ||
         m_state == QwenGenerationState::Cancelled;
}

void
QwenGenerationSessionStateMachine::requireState(
  QwenGenerationState expected, const char* operation) const
{
  if (m_state != expected) {
    throw std::logic_error(
      std::string("invalid qwen generation transition for ") + operation +
      ": state=" + toString(m_state));
  }
}

void
QwenGenerationSessionStateMachine::beginSelection()
{
  requireState(QwenGenerationState::Created, "beginSelection");
  m_state = QwenGenerationState::Selecting;
}

void
QwenGenerationSessionStateMachine::activate()
{
  if (m_state != QwenGenerationState::Selecting &&
      m_state != QwenGenerationState::Rebuilding) {
    throw std::logic_error("invalid qwen generation transition for activate");
  }
  m_state = QwenGenerationState::Prefilling;
}

void
QwenGenerationSessionStateMachine::completePrefill()
{
  requireState(QwenGenerationState::Prefilling, "completePrefill");
  m_state = QwenGenerationState::Decoding;
}

std::uint32_t
QwenGenerationSessionStateMachine::completeTokenEpoch()
{
  return completeTokenEpoch(m_attemptEpoch);
}

std::uint32_t
QwenGenerationSessionStateMachine::completeTokenEpoch(std::uint64_t attemptEpoch)
{
  requireState(QwenGenerationState::Decoding, "completeTokenEpoch");
  if (attemptEpoch != m_attemptEpoch) {
    throw std::logic_error("stale qwen generation attempt epoch");
  }
  if (m_generatedTokenCount >= m_spec.maxGeneratedTokens) {
    throw std::logic_error("qwen generation token bound exceeded");
  }
  return ++m_generatedTokenCount;
}

void
QwenGenerationSessionStateMachine::observeEosToken()
{
  requireState(QwenGenerationState::Decoding, "observeEosToken");
  m_eosObserved = true;
}

void
QwenGenerationSessionStateMachine::observeStopSequence()
{
  requireState(QwenGenerationState::Decoding, "observeStopSequence");
  m_stopSequenceObserved = true;
}

void
QwenGenerationSessionStateMachine::beginDrain(QwenGenerationFinishReason reason)
{
  requireState(QwenGenerationState::Decoding, "beginDrain");
  switch (reason) {
    case QwenGenerationFinishReason::Eos:
      if (!m_eosObserved) {
        throw std::logic_error("EOS drain lacks EOS evidence");
      }
      break;
    case QwenGenerationFinishReason::StopSequence:
      if (!m_stopSequenceObserved) {
        throw std::logic_error("stop drain lacks stop-sequence evidence");
      }
      break;
    case QwenGenerationFinishReason::MaxTokens:
      if (m_generatedTokenCount != m_spec.maxGeneratedTokens) {
        throw std::logic_error("max-token drain before exact token count");
      }
      break;
    case QwenGenerationFinishReason::ApplicationComplete:
      break;
  }
  m_finishReason = reason;
  m_state = QwenGenerationState::Draining;
}

void
QwenGenerationSessionStateMachine::beginReplacement()
{
  requireState(QwenGenerationState::Decoding, "beginReplacement");
  if (m_attemptEpoch >= 2) {
    throw std::logic_error("qwen generation replacement bound exceeded");
  }
  ++m_attemptEpoch;
  m_state = QwenGenerationState::Rebuilding;
}

void
QwenGenerationSessionStateMachine::complete(QwenGenerationFinishReason reason)
{
  requireState(QwenGenerationState::Draining, "complete");
  if (reason != m_finishReason) {
    throw std::logic_error("qwen generation completion reason mismatch");
  }
  m_state = QwenGenerationState::Completed;
}

void
QwenGenerationSessionStateMachine::terminate(QwenGenerationTerminal reason)
{
  if (isTerminal() || reason == QwenGenerationTerminal::None) {
    throw std::logic_error("invalid qwen generation terminal transition");
  }
  m_terminalReason = reason;
  m_state = QwenGenerationState::Terminal;
}

void
QwenGenerationSessionStateMachine::cancel()
{
  if (isTerminal()) {
    throw std::logic_error("qwen generation already terminal");
  }
  m_terminalReason = QwenGenerationTerminal::AttemptCancelled;
  m_state = QwenGenerationState::Cancelled;
}

bool
QwenGenerationSessionStateMachine::expireIfDeadlineReached(std::uint64_t nowEpochMs)
{
  if (isTerminal() || nowEpochMs < m_spec.deadlineEpochMs) {
    return false;
  }
  terminate(QwenGenerationTerminal::RequestDeadline);
  return true;
}

bool
QwenGenerationSessionStateMachine::claimTerminalResponse()
{
  if (!isTerminal()) {
    throw std::logic_error("qwen generation terminal response before terminal state");
  }
  if (m_terminalResponseClaimed) {
    return false;
  }
  m_terminalResponseClaimed = true;
  return true;
}

const char*
toString(QwenResourceKind kind) noexcept
{
  switch (kind) {
    case QwenResourceKind::Generation: return "generation";
    case QwenResourceKind::Request: return "request";
    case QwenResourceKind::Wait: return "wait";
    case QwenResourceKind::Callback: return "callback";
    case QwenResourceKind::TokenPair: return "token-pair";
    case QwenResourceKind::Assignment: return "assignment";
    case QwenResourceKind::Tensor: return "tensor";
    case QwenResourceKind::Metrics: return "metrics";
    case QwenResourceKind::Count: break;
  }
  return "unknown";
}

void
QwenGenerationResourceLimits::validate() const
{
  for (const auto kind : {
         QwenResourceKind::Generation, QwenResourceKind::Request,
         QwenResourceKind::Wait, QwenResourceKind::Callback,
         QwenResourceKind::TokenPair, QwenResourceKind::Assignment,
         QwenResourceKind::Tensor, QwenResourceKind::Metrics,
       }) {
    const auto value = capacity(kind);
    if (value == 0 || value > 1'000'000) {
      throw std::invalid_argument(
        std::string("invalid qwen resource capacity: ") + toString(kind));
    }
  }
}

std::size_t
QwenGenerationResourceLimits::capacity(QwenResourceKind kind) const
{
  switch (kind) {
    case QwenResourceKind::Generation: return generationCapacity;
    case QwenResourceKind::Request: return requestCapacity;
    case QwenResourceKind::Wait: return waitCapacity;
    case QwenResourceKind::Callback: return callbackCapacity;
    case QwenResourceKind::TokenPair: return tokenPairCapacity;
    case QwenResourceKind::Assignment: return assignmentCapacity;
    case QwenResourceKind::Tensor: return tensorCapacity;
    case QwenResourceKind::Metrics: return metricsCapacity;
    case QwenResourceKind::Count: break;
  }
  throw std::invalid_argument("unknown qwen generation resource kind");
}

QwenGenerationResourceLedger::QwenGenerationResourceLedger(
  QwenGenerationResourceLimits limits)
  : m_limits(std::move(limits))
{
  m_limits.validate();
}

std::size_t
QwenGenerationResourceLedger::index(QwenResourceKind kind)
{
  return kindIndex(kind);
}

QwenResourceAcquireResult
QwenGenerationResourceLedger::tryAcquire(QwenResourceKind kind)
{
  const auto slot = index(kind);
  const auto capacity = m_limits.capacity(kind);
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_occupancy[slot] >= capacity) {
    ++m_rejected[slot];
    return {false, "QUEUE_FULL", m_occupancy[slot], capacity};
  }
  ++m_occupancy[slot];
  return {true, "ACCEPTED", m_occupancy[slot], capacity};
}

void
QwenGenerationResourceLedger::release(QwenResourceKind kind)
{
  const auto slot = index(kind);
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_occupancy[slot] == 0) {
    throw std::logic_error(
      std::string("qwen resource release underflow: ") + toString(kind));
  }
  --m_occupancy[slot];
}

QwenResourceSnapshot
QwenGenerationResourceLedger::snapshot(QwenResourceKind kind) const
{
  const auto slot = index(kind);
  std::lock_guard<std::mutex> lock(m_mutex);
  return {
    kind,
    toString(kind),
    m_occupancy[slot],
    m_limits.capacity(kind),
    m_rejected[slot],
  };
}

} // namespace ndnsf::di
