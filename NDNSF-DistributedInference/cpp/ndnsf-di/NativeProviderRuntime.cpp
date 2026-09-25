#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DiTimelineTrace.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {

namespace {

std::uint64_t
conversationRetentionDeadline(std::uint64_t nowMs, std::uint64_t retentionMs)
{
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  return nowMs > maximum - retentionMs ? maximum : nowMs + retentionMs;
}

/**
 * Erase Provider-owned state bytes before releasing the container holding
 * them. TensorBundle payloads can contain KV/recurrent/convolution state;
 * clearing the vector alone leaves the previous bytes in allocator storage.
 * The volatile walk prevents the compiler from removing the wipe.
 */
void
wipeTensorBundle(TensorBundle& bundle) noexcept
{
  volatile std::uint8_t* bytes = bundle.payload.data();
  for (std::size_t index = 0; index < bundle.payload.size(); ++index) {
    bytes[index] = 0;
  }
  bundle.payload.clear();
  bundle.expectedBytes = 0;
  bundle.expectedSegments = 0;
}

std::string
jsonString(const std::string& value)
{
  std::ostringstream out;
  out << '"';
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<unsigned int>(ch) << std::dec;
        }
        else {
          out << static_cast<char>(ch);
        }
    }
  }
  out << '"';
  return out.str();
}

std::string
jsonStringArray(const std::vector<std::string>& values)
{
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << jsonString(values[i]);
  }
  out << ']';
  return out.str();
}

std::string
metadataValue(const NativeModelRunnerSpec& spec,
              std::initializer_list<const char*> keys)
{
  for (const auto* key : keys) {
    const auto found = spec.metadata.find(key);
    if (found != spec.metadata.end() && !found->second.empty()) {
      return found->second;
    }
  }
  return {};
}

std::uint64_t
metadataUint64(const NativeModelRunnerSpec& spec,
               std::initializer_list<const char*> keys,
               std::uint64_t fallback = 0)
{
  const auto value = metadataValue(spec, keys);
  if (value.empty()) {
    return fallback;
  }
  std::size_t consumed = 0;
  const auto parsed = std::stoull(value, &consumed);
  if (consumed != value.size()) {
    throw std::invalid_argument("invalid Provider decode-state integer metadata");
  }
  return parsed;
}

std::string
stateSchemaIdentity(const NativeModelRunnerSpec& spec, const RoleSpec& role)
{
  const auto declared = metadataValue(
    spec, {"state.schemaDigest", "stateSchemaDigest", "state_schema_digest"});
  if (!declared.empty()) {
    return declared;
  }
  std::ostringstream value;
  for (const auto& name : role.stateInputNames) {
    value << "in:" << name << ';';
  }
  for (const auto& name : role.stateOutputNames) {
    value << "out:" << name << ';';
  }
  return value.str();
}

KvStateBinding
decodeStateBindingFor(const NativeModelRunnerSpec& spec,
                      const std::string& sessionId,
                      const RoleSpec& role)
{
  KvStateBinding binding;
  binding.sessionId = sessionId;
  binding.stage = role.role;
  binding.contextEpoch = role.inferenceEpoch;
  if (role.candidateDecodeStateIdentity) {
    const auto& exact = *role.candidateDecodeStateIdentity;
    exact.validate();
    binding.modelDigest = exact.modelDigest;
    binding.planDigest = role.generationLineage
      ? role.generationLineage->planDigest
      : metadataValue(spec, {"evidence.planDigest", "planDigest", "plan_digest"});
    binding.providerName = exact.providerIdentity;
    binding.providerBootId = exact.providerBootId;
    binding.securityEpoch = exact.cacheEpoch;
    binding.requestId = exact.requestId;
    binding.attemptEpoch = exact.attemptEpoch;
    binding.generationId = exact.generationId;
    binding.stateSchemaDigest = exact.stateSchemaDigest;
    binding.exactIdentity = exact;
    binding.validate();
    return binding;
  }
  binding.modelDigest = metadataValue(
    spec, {"evidence.modelDigest", "modelDigest", "model_digest"});
  binding.planDigest = metadataValue(
    spec, {"evidence.planDigest", "planDigest", "plan_digest"});
  binding.providerName = metadataValue(
    spec, {"evidence.providerName", "providerName", "provider_name"});
  binding.providerBootId = metadataValue(
    spec, {"evidence.providerBootId", "providerBootId", "provider_boot_id"});
  binding.securityEpoch = metadataUint64(
    spec, {"state.securityEpoch", "securityEpoch", "security_epoch"});
  binding.requestId = role.requestId.empty() ? sessionId : role.requestId;
  binding.attemptEpoch = role.attemptEpoch;
  binding.generationId = metadataValue(
    spec, {"state.generationId", "generationId", "generation_id"});
  if (binding.generationId.empty()) {
    binding.generationId = sessionId;
  }
  binding.stateSchemaDigest = stateSchemaIdentity(spec, role);
  binding.validate();
  return binding;
}

TensorBundle
stateBundleFromResult(const ProviderRoleResult& result, const RoleSpec& role)
{
  if (role.stateInputNames.size() != role.stateOutputNames.size() ||
      role.stateInputNames.empty()) {
    throw std::invalid_argument(
      "Provider decode-state input/output metadata length mismatch");
  }
  std::vector<NamedTensor> nextState;
  nextState.reserve(role.stateInputNames.size());
  for (std::size_t index = 0; index < role.stateOutputNames.size(); ++index) {
    bool found = false;
    for (const auto& output : result.outputsByScope) {
      if (!isEncodedTensorBundle(output.second.payload)) {
        continue;
      }
      try {
        auto tensor = findTensor(
          decodeTensorBundle(output.second.payload), role.stateOutputNames[index]);
        tensor.name = role.stateInputNames[index];
        nextState.push_back(std::move(tensor));
        found = true;
        break;
      }
      catch (const std::out_of_range&) {
      }
    }
    if (!found) {
      throw std::runtime_error(
        "Provider role is missing decode-state output: " +
        role.stateOutputNames[index]);
    }
  }
  return makeEncodedTensorBundle("__ndnsf_provider_decode_state",
                                 std::move(nextState));
}

TensorBundle
opaqueStateBundleFromHandle(const NativeOpaqueStateHandleV1& handle,
                            const RoleSpec& role,
                            const std::string& sessionId)
{
  handle.validate();
  if (role.stateInputNames.empty() ||
      handle.sessionId != sessionId ||
      handle.role != role.role) {
    throw std::invalid_argument("opaque Provider state handle does not bind to role");
  }
  std::vector<NamedTensor> handles;
  handles.reserve(role.stateInputNames.size());
  for (const auto& inputName : role.stateInputNames) {
    handles.push_back(NamedTensor{
      inputName,
      TensorElementType::UInt8,
      {static_cast<std::int64_t>(handle.token.size())},
      std::vector<std::uint8_t>(handle.token.begin(), handle.token.end())});
  }
  return makeEncodedTensorBundle("__ndnsf_provider_decode_state",
                                 std::move(handles));
}

} // namespace

void
ProviderConversationStateReceiptV1::validate() const
{
  if (conversationId.size() < 16 || conversationId.find('/') != std::string::npos ||
      successorContextEpoch != parentContextEpoch + 1 || originRequestId.empty() ||
      originGenerationId.empty() || serviceName.empty() || serviceName.front() != '/' ||
      requesterIdentity.empty() || roleName.empty() || providerIdentity.empty() ||
      providerBootId.empty() || expiresAtMs == 0 || stateComponentDigests.empty()) {
    throw std::invalid_argument("Provider conversation receipt is incomplete");
  }
  using decode_state_identity_detail::requireDigest;
  requireDigest(securityDomainDigest, "receipt security domain");
  requireDigest(modelDigest, "receipt model");
  requireDigest(graphSemanticDigest, "receipt graph semantic");
  requireDigest(adapterDigest, "receipt adapter");
  requireDigest(roleSplitDigest, "receipt role split");
  requireDigest(layoutDigest, "receipt layout");
  requireDigest(planRoleMapDigest, "receipt plan-role map");
  requireDigest(prefixDigest, "receipt prefix");
  requireDigest(positionDigest, "receipt position");
  requireDigest(stateSchemaDigest, "receipt state schema");
  for (const auto& digest : stateComponentDigests) {
    requireDigest(digest, "receipt state component");
  }
}

std::string
ProviderConversationStateReceiptV1::canonicalUnsignedJson() const
{
  validate();
  // Alphabetical keys exactly match Python json.dumps(sort_keys=True,
  // separators=(",", ":"), ensure_ascii=False).
  std::ostringstream out;
  out << '{'
      << "\"adapterDigest\":" << jsonString(adapterDigest) << ','
      << "\"cacheEpoch\":" << cacheEpoch << ','
      << "\"conversationId\":" << jsonString(conversationId) << ','
      << "\"expiresAtMs\":" << expiresAtMs << ','
      << "\"graphSemanticDigest\":" << jsonString(graphSemanticDigest) << ','
      << "\"layoutDigest\":" << jsonString(layoutDigest) << ','
      << "\"modelDigest\":" << jsonString(modelDigest) << ','
      << "\"originGenerationId\":" << jsonString(originGenerationId) << ','
      << "\"originRequestId\":" << jsonString(originRequestId) << ','
      << "\"parentContextEpoch\":" << parentContextEpoch << ','
      << "\"planRoleMapDigest\":" << jsonString(planRoleMapDigest) << ','
      << "\"positionDigest\":" << jsonString(positionDigest) << ','
      << "\"prefixDigest\":" << jsonString(prefixDigest) << ','
      << "\"prefixTokenCount\":" << prefixTokenCount << ','
      << "\"providerBootId\":" << jsonString(providerBootId) << ','
      << "\"providerIdentity\":" << jsonString(providerIdentity) << ','
      << "\"requesterIdentity\":" << jsonString(requesterIdentity) << ','
      << "\"roleName\":" << jsonString(roleName) << ','
      << "\"roleSplitDigest\":" << jsonString(roleSplitDigest) << ','
      << "\"schema\":\"ndnsf-di-provider-conversation-receipt-v1\","
      << "\"securityDomainDigest\":" << jsonString(securityDomainDigest) << ','
      << "\"serviceName\":" << jsonString(serviceName) << ','
      << "\"stateComponentDigests\":" << jsonStringArray(stateComponentDigests) << ','
      << "\"stateSchemaDigest\":" << jsonString(stateSchemaDigest) << ','
      << "\"successorContextEpoch\":" << successorContextEpoch
      << '}';
  return out.str();
}

std::string
ProviderConversationStateReceiptV1::computedDigest() const
{
  const auto value = canonicalUnsignedJson();
  return sha256TensorBytes(
    std::vector<std::uint8_t>(value.begin(), value.end()));
}

std::string
ProviderConversationStateReceiptV1::toJson() const
{
  const auto receiptDigest = computedDigest();
  std::ostringstream out;
  out << '{'
      << "\"adapterDigest\":" << jsonString(adapterDigest) << ','
      << "\"cacheEpoch\":" << cacheEpoch << ','
      << "\"conversationId\":" << jsonString(conversationId) << ','
      << "\"expiresAtMs\":" << expiresAtMs << ','
      << "\"graphSemanticDigest\":" << jsonString(graphSemanticDigest) << ','
      << "\"layoutDigest\":" << jsonString(layoutDigest) << ','
      << "\"modelDigest\":" << jsonString(modelDigest) << ','
      << "\"originGenerationId\":" << jsonString(originGenerationId) << ','
      << "\"originRequestId\":" << jsonString(originRequestId) << ','
      << "\"parentContextEpoch\":" << parentContextEpoch << ','
      << "\"planRoleMapDigest\":" << jsonString(planRoleMapDigest) << ','
      << "\"positionDigest\":" << jsonString(positionDigest) << ','
      << "\"prefixDigest\":" << jsonString(prefixDigest) << ','
      << "\"prefixTokenCount\":" << prefixTokenCount << ','
      << "\"providerBootId\":" << jsonString(providerBootId) << ','
      << "\"providerIdentity\":" << jsonString(providerIdentity) << ','
      << "\"receiptDigest\":" << jsonString(receiptDigest) << ','
      << "\"requesterIdentity\":" << jsonString(requesterIdentity) << ','
      << "\"roleName\":" << jsonString(roleName) << ','
      << "\"roleSplitDigest\":" << jsonString(roleSplitDigest) << ','
      << "\"schema\":\"ndnsf-di-provider-conversation-receipt-v1\","
      << "\"securityDomainDigest\":" << jsonString(securityDomainDigest) << ','
      << "\"serviceName\":" << jsonString(serviceName) << ','
      << "\"signature\":\"\","
      << "\"stateComponentDigests\":" << jsonStringArray(stateComponentDigests) << ','
      << "\"stateSchemaDigest\":" << jsonString(stateSchemaDigest) << ','
      << "\"successorContextEpoch\":" << successorContextEpoch
      << '}';
  return out.str();
}

namespace {

bool
sameConversationIdentity(const DecodeStateIdentityV1& left,
                         const DecodeStateIdentityV1& right)
{
  return left.modelDigest == right.modelDigest &&
         left.graphSemanticDigest == right.graphSemanticDigest &&
         left.artifactDigest == right.artifactDigest &&
         left.adapterDigest == right.adapterDigest &&
         left.tokenizerDigest == right.tokenizerDigest &&
         left.runnerDigest == right.runnerDigest &&
         left.roleName == right.roleName &&
         left.roleSplitDigest == right.roleSplitDigest &&
         left.layerBegin == right.layerBegin &&
         left.layerEnd == right.layerEnd &&
         left.prefixDigest == right.prefixDigest &&
         left.prefixTokenCount == right.prefixTokenCount &&
         left.positionDigest == right.positionDigest &&
         left.precision == right.precision &&
         left.layoutDigest == right.layoutDigest &&
         left.stateSchemaDigest == right.stateSchemaDigest &&
         left.stateComponentDigests == right.stateComponentDigests &&
         left.runtimeAbiDigest == right.runtimeAbiDigest &&
         left.securityDomainDigest == right.securityDomainDigest &&
         left.providerIdentity == right.providerIdentity &&
         left.providerBootId == right.providerBootId &&
         left.stateInferenceEpoch == right.stateInferenceEpoch &&
         left.predecessorInferenceEpoch == right.predecessorInferenceEpoch &&
         left.cacheEpoch == right.cacheEpoch;
}

std::future<bool>
readyBool(bool value)
{
  return std::async(std::launch::deferred, [value] { return value; });
}

} // namespace

void
ConversationStateBinding::validate() const
{
  if (conversationId.empty() || conversationId.find('/') != std::string::npos ||
      contextEpoch == 0 || serviceName.empty() || serviceName.front() != '/' ||
      expiresAtMs == 0 || planRoleMapDigest.empty() || receiptDigest.empty()) {
    throw std::invalid_argument("conversation state binding is incomplete");
  }
  identity.validate();
  decode_state_identity_detail::requireDigest(
    planRoleMapDigest, "conversation plan-role map");
  decode_state_identity_detail::requireDigest(
    receiptDigest, "conversation role receipt");
}

bool
ConversationStateBinding::operator==(const ConversationStateBinding& other) const
{
  return conversationId == other.conversationId &&
         contextEpoch == other.contextEpoch &&
         serviceName == other.serviceName &&
         planRoleMapDigest == other.planRoleMapDigest &&
         receiptDigest == other.receiptDigest &&
         checkpointDigest == other.checkpointDigest &&
         expiresAtMs == other.expiresAtMs &&
         sameConversationIdentity(identity, other.identity);
}

void
ConversationStateReferenceV1::validate() const
{
  if (conversationId.empty() || conversationId.find('/') != std::string::npos ||
      contextEpoch == 0 || serviceName.empty() || serviceName.front() != '/' ||
      roleName.empty() || expiresAtMs == 0) {
    throw std::invalid_argument("conversation state reference is incomplete");
  }
  decode_state_identity_detail::requireDigest(
    planRoleMapDigest, "conversation plan-role map");
  decode_state_identity_detail::requireDigest(
    checkpointDigest, "conversation checkpoint");
  decode_state_identity_detail::requireDigest(
    roleReceiptDigest, "conversation role receipt");
}

void
ConversationTurnBindingV1::validate() const
{
  if (conversationId.empty() || conversationId.find('/') != std::string::npos ||
      successorContextEpoch != parentContextEpoch + 1 ||
      serviceName.empty() || serviceName.front() != '/' ||
      retentionDeadlineMs == 0) {
    throw std::invalid_argument("conversation turn binding is incomplete");
  }
  decode_state_identity_detail::requireDigest(
    planRoleMapDigest, "conversation turn plan-role map");
  decode_state_identity_detail::requireDigest(
    requestContractDigest, "conversation turn request contract");
  if (parentContextEpoch == 0) {
    if (!parentCheckpointDigest.empty()) {
      throw std::invalid_argument(
        "initial conversation turn carries a parent checkpoint");
    }
  }
  else {
    decode_state_identity_detail::requireDigest(
      parentCheckpointDigest, "conversation turn parent checkpoint");
  }
}

ConversationStateStore::ConversationStateStore(
  std::size_t gpuMaxBytes,
  std::size_t gpuMaxEntries,
  std::size_t hostMaxBytes,
  std::size_t hostMaxEntries,
  std::uint64_t retentionMs)
  : m_gpuMaxBytes(gpuMaxBytes)
  , m_gpuMaxEntries(gpuMaxEntries)
  , m_hostMaxBytes(hostMaxBytes)
  , m_hostMaxEntries(hostMaxEntries)
  , m_retentionMs(retentionMs)
{
  if (retentionMs == 0 || retentionMs > 3'600'000) {
    throw std::invalid_argument("conversation state retention is out of range");
  }
}

bool
ConversationStateStore::setProviderBinding(std::string providerIdentity,
                                            std::string providerBootId,
                                            std::uint64_t cacheEpoch)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const bool changed = (!m_providerBootId.empty() &&
                        (m_providerIdentity != providerIdentity ||
                         m_providerBootId != providerBootId ||
                         m_cacheEpoch != cacheEpoch));
  if (changed) {
    m_cleanups += m_entries.size();
    for (auto& item : m_entries) {
      if (item.second.adapterState && item.second.adapterRunner) {
        (void)item.second.adapterRunner->releaseConversationState(
          *item.second.adapterState);
      }
      wipeTensorBundle(item.second.state);
    }
    m_entries.clear();
    m_gpuBytes = 0;
    m_hostBytes = 0;
  }
  m_providerIdentity = std::move(providerIdentity);
  m_providerBootId = std::move(providerBootId);
  m_cacheEpoch = cacheEpoch;
  return changed;
}

std::string
ConversationStateStore::keyFor(const ConversationStateBinding& binding)
{
  return binding.conversationId + '\x1f' +
         std::to_string(binding.contextEpoch) + '\x1f' +
         binding.identity.roleName;
}

bool
ConversationStateStore::compatible(const ConversationStateBinding& left,
                                    const ConversationStateBinding& right)
{
  // Request-internal callers may carry the compact binding without the
  // aggregate checkpoint digest. Selection resolution never uses this helper:
  // resolve() compares the checkpoint strictly before returning a binding.
  // Treat an omitted checkpoint as a compatibility wildcard only for these
  // local state operations, while rejecting two present but different digests.
  return left.conversationId == right.conversationId &&
         left.contextEpoch == right.contextEpoch &&
         left.serviceName == right.serviceName &&
         left.planRoleMapDigest == right.planRoleMapDigest &&
         left.receiptDigest == right.receiptDigest &&
         (left.checkpointDigest.empty() || right.checkpointDigest.empty() ||
          left.checkpointDigest == right.checkpointDigest) &&
         left.expiresAtMs == right.expiresAtMs &&
         sameConversationIdentity(left.identity, right.identity);
}

void
ConversationStateStore::removeEntry(const std::string& key,
                                    bool countCleanup)
{
  const auto found = m_entries.find(key);
  if (found == m_entries.end()) {
    return;
  }
  auto& entry = found->second;
  if (entry.residency == ConversationStateResidency::GPU_RESIDENT) {
    m_gpuBytes -= std::min(m_gpuBytes, entry.allocatedBytes);
  }
  else if (entry.residency == ConversationStateResidency::HOST_RESIDENT) {
    m_hostBytes -= std::min(m_hostBytes, entry.allocatedBytes);
  }
  if (entry.adapterState && entry.adapterRunner) {
    (void)entry.adapterRunner->releaseConversationState(*entry.adapterState);
  }
  wipeTensorBundle(entry.state);
  m_entries.erase(found);
  if (countCleanup) {
    ++m_cleanups;
  }
}

bool
ConversationStateStore::evictGpuUntilFits(std::size_t incomingBytes,
                                          const std::string& replacingKey)
{
  while (m_gpuBytes + incomingBytes > m_gpuMaxBytes ||
         static_cast<std::size_t>(std::count_if(
           m_entries.begin(), m_entries.end(), [] (const auto& item) {
             return item.second.residency == ConversationStateResidency::GPU_RESIDENT;
           })) + 1 > m_gpuMaxEntries) {
    auto victim = m_entries.end();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
      const auto& entry = it->second;
      if (it->first == replacingKey ||
          entry.residency != ConversationStateResidency::GPU_RESIDENT ||
          entry.lifecycle != ConversationStateLifecycle::IDLE ||
          entry.pinCount != 0) {
        continue;
      }
      if (victim == m_entries.end() ||
          entry.lastAccess < victim->second.lastAccess) {
        victim = it;
      }
    }
    if (victim == m_entries.end()) {
      return false;
    }
    removeEntry(victim->first, false);
    ++m_evictions;
  }
  return true;
}

bool
ConversationStateStore::evictHostUntilFits(std::size_t incomingBytes,
                                           const std::string& replacingKey)
{
  while (m_hostBytes + incomingBytes > m_hostMaxBytes ||
         static_cast<std::size_t>(std::count_if(
           m_entries.begin(), m_entries.end(), [] (const auto& item) {
             return item.second.residency == ConversationStateResidency::HOST_RESIDENT;
           })) + 1 > m_hostMaxEntries) {
    auto victim = m_entries.end();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
      const auto& entry = it->second;
      if (it->first == replacingKey ||
          entry.residency != ConversationStateResidency::HOST_RESIDENT ||
          entry.lifecycle != ConversationStateLifecycle::IDLE ||
          entry.pinCount != 0) {
        continue;
      }
      if (victim == m_entries.end() ||
          entry.lastAccess < victim->second.lastAccess) {
        victim = it;
      }
    }
    if (victim == m_entries.end()) {
      return false;
    }
    removeEntry(victim->first, false);
    ++m_evictions;
  }
  return true;
}

bool
ConversationStateStore::promote(std::string originRequestId,
                                 std::string role,
                                 ConversationStateBinding binding,
                                 TensorBundle state,
                                 std::uint64_t nowMs)
{
  const auto stagedBinding = binding;
  if (!stagePromotion(std::move(originRequestId), std::move(role),
                      std::move(binding), std::move(state), nowMs)) {
    return false;
  }
  if (commitStagedPromotion(stagedBinding)) {
    return true;
  }
  rollbackStagedPromotion(stagedBinding);
  return false;
}

bool
ConversationStateStore::stagePromotion(std::string originRequestId,
                                        std::string role,
                                        ConversationStateBinding binding,
                                        TensorBundle state,
                                        std::uint64_t nowMs)
{
  binding.validate();
  cleanupExpired(nowMs);
  if (originRequestId.empty() || role.empty() || role != binding.identity.roleName ||
      originRequestId != binding.identity.requestId || state.payload.empty() ||
      nowMs >= binding.expiresAtMs ||
      (!m_providerIdentity.empty() &&
       (binding.identity.providerIdentity != m_providerIdentity ||
        binding.identity.providerBootId != m_providerBootId ||
        binding.identity.cacheEpoch != m_cacheEpoch))) {
    return false;
  }
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  // One successor epoch has exactly one winner. Never replace an already
  // committed entry or another child's in-flight staged candidate.
  if (m_entries.find(key) != m_entries.end()) {
    return false;
  }
  const auto bytes = state.payload.size();
  if (!evictGpuUntilFits(bytes, key)) {
    return false;
  }
  Entry entry;
  entry.binding = std::move(binding);
  entry.state = std::move(state);
  entry.residency = ConversationStateResidency::GPU_RESIDENT;
  entry.lifecycle = ConversationStateLifecycle::COMMITTING;
  entry.logicalBytes = bytes;
  entry.allocatedBytes = bytes;
  entry.pinCount = 1;
  entry.lastAccess = ++m_accessSequence;
  entry.lastUsedAtMs = nowMs;
  entry.expiresAtMs = std::min(entry.binding.expiresAtMs,
                             conversationRetentionDeadline(nowMs, m_retentionMs));
  m_gpuBytes += bytes;
  m_entries.emplace(key, std::move(entry));
  ++m_stagedPromotions;
  return true;
}

bool
ConversationStateStore::stageAdapterPromotion(
  std::string originRequestId,
  std::string role,
  ConversationStateBinding binding,
  NativeConversationStateHandleV1 state,
  std::shared_ptr<NativeModelRunner> runner,
  std::uint64_t nowMs)
{
  binding.validate();
  state.validate();
  if (!runner || originRequestId.empty() || role.empty() ||
      role != binding.identity.roleName || role != state.opaque.role ||
      originRequestId != binding.identity.requestId ||
      nowMs >= binding.expiresAtMs ||
      state.opaque.providerIdentity != binding.identity.providerIdentity ||
      state.opaque.providerBootId != binding.identity.providerBootId ||
      (!m_providerIdentity.empty() &&
       (binding.identity.providerIdentity != m_providerIdentity ||
        binding.identity.providerBootId != m_providerBootId ||
        binding.identity.cacheEpoch != m_cacheEpoch))) {
    return false;
  }
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_entries.find(key) != m_entries.end() ||
      !evictGpuUntilFits(state.logicalBytes, key)) {
    return false;
  }
  Entry entry;
  entry.binding = std::move(binding);
  entry.adapterState = std::move(state);
  entry.adapterRunner = std::move(runner);
  entry.residency = ConversationStateResidency::GPU_RESIDENT;
  entry.lifecycle = ConversationStateLifecycle::COMMITTING;
  entry.logicalBytes = entry.adapterState->logicalBytes;
  entry.allocatedBytes = entry.logicalBytes;
  entry.pinCount = 1;
  entry.lastAccess = ++m_accessSequence;
  entry.lastUsedAtMs = nowMs;
  entry.expiresAtMs = std::min(entry.binding.expiresAtMs,
                             conversationRetentionDeadline(nowMs, m_retentionMs));
  m_gpuBytes += entry.allocatedBytes;
  m_entries.emplace(key, std::move(entry));
  ++m_stagedPromotions;
  return true;
}

bool
ConversationStateStore::commitStagedPromotion(
  const ConversationStateBinding& binding)
{
  binding.validate();
  // The legacy no-checkpoint overload is intentionally limited to the
  // pre-commit helper. A caller carrying an aggregate checkpoint must use
  // the authenticated two-argument overload so that the digest is bound at
  // the commit control boundary.
  if (!binding.checkpointDigest.empty()) {
    return false;
  }
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() ||
      !compatible(found->second.binding, binding) ||
      found->second.lifecycle != ConversationStateLifecycle::COMMITTING ||
      found->second.pinCount != 1) {
    return false;
  }
  found->second.lifecycle = ConversationStateLifecycle::IDLE;
  found->second.pinCount = 0;
  ++m_promotions;
  return true;
}

bool
ConversationStateStore::commitStagedPromotion(
  const ConversationStateBinding& binding,
  const std::string& checkpointDigest)
{
  decode_state_identity_detail::requireDigest(
    checkpointDigest, "conversation checkpoint");
  binding.validate();
  if (!binding.checkpointDigest.empty()) {
    decode_state_identity_detail::requireDigest(
      binding.checkpointDigest, "conversation binding checkpoint");
    if (binding.checkpointDigest != checkpointDigest) {
      return false;
    }
  }
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() ||
      found->second.binding.conversationId != binding.conversationId ||
      found->second.binding.contextEpoch != binding.contextEpoch ||
      found->second.binding.serviceName != binding.serviceName ||
      found->second.binding.planRoleMapDigest != binding.planRoleMapDigest ||
      found->second.binding.receiptDigest != binding.receiptDigest ||
      found->second.binding.expiresAtMs != binding.expiresAtMs ||
      !sameConversationIdentity(found->second.binding.identity, binding.identity) ||
      found->second.lifecycle != ConversationStateLifecycle::COMMITTING ||
      found->second.pinCount != 1 ||
      !found->second.binding.checkpointDigest.empty()) {
    return false;
  }
  found->second.binding.checkpointDigest = checkpointDigest;
  found->second.lifecycle = ConversationStateLifecycle::IDLE;
  found->second.pinCount = 0;
  ++m_promotions;
  return true;
}

bool
ConversationStateStore::rollbackStagedPromotion(
  const ConversationStateBinding& binding)
{
  binding.validate();
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() || found->second.adapterState ||
      !compatible(found->second.binding, binding) ||
      found->second.lifecycle != ConversationStateLifecycle::COMMITTING ||
      found->second.pinCount != 1) {
    return false;
  }
  removeEntry(key, true);
  ++m_promotionRollbacks;
  return true;
}

void
ConversationStateStore::noteRequestLocalRelease()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  ++m_requestLocalReleases;
}

std::optional<TensorBundle>
ConversationStateStore::lookup(const ConversationStateBinding& binding,
                               std::uint64_t nowMs)
{
  binding.validate();
  cleanupExpired(nowMs);
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() || found->second.adapterState ||
      !compatible(found->second.binding, binding) ||
      found->second.lifecycle == ConversationStateLifecycle::COMMITTING ||
      (m_providerIdentity.empty() ? false :
       (binding.identity.providerIdentity != m_providerIdentity ||
        binding.identity.providerBootId != m_providerBootId ||
        binding.identity.cacheEpoch != m_cacheEpoch))) {
    ++m_misses;
    return std::nullopt;
  }
  if (nowMs >= found->second.expiresAtMs) {
    removeEntry(key, true);
    ++m_misses;
    return std::nullopt;
  }
  found->second.lastAccess = ++m_accessSequence;
  found->second.lastUsedAtMs = nowMs;
  ++m_hits;
  return found->second.state;
}

std::optional<NativeConversationStateHandleV1>
ConversationStateStore::adapterHandle(
  const ConversationStateBinding& binding,
  std::uint64_t nowMs) const
{
  binding.validate();
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() || !found->second.adapterState ||
      !compatible(found->second.binding, binding) ||
      found->second.lifecycle == ConversationStateLifecycle::COMMITTING ||
      nowMs >= found->second.expiresAtMs) {
    return std::nullopt;
  }
  return found->second.adapterState;
}

std::optional<ConversationStateBinding>
ConversationStateStore::resolve(const ConversationStateReferenceV1& reference,
                                std::uint64_t nowMs)
{
  reference.validate();
  cleanupExpired(nowMs);
  ConversationStateBinding keyBinding;
  keyBinding.conversationId = reference.conversationId;
  keyBinding.contextEpoch = reference.contextEpoch;
  keyBinding.identity.roleName = reference.roleName;
  const auto key = keyFor(keyBinding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() || nowMs >= found->second.expiresAtMs ||
      nowMs >= reference.expiresAtMs ||
      found->second.lifecycle == ConversationStateLifecycle::COMMITTING ||
      found->second.binding.serviceName != reference.serviceName ||
      found->second.binding.planRoleMapDigest != reference.planRoleMapDigest ||
      found->second.binding.checkpointDigest != reference.checkpointDigest ||
      found->second.binding.receiptDigest != reference.roleReceiptDigest ||
      (!m_providerIdentity.empty() &&
       (found->second.binding.identity.providerIdentity != m_providerIdentity ||
        found->second.binding.identity.providerBootId != m_providerBootId ||
        found->second.binding.identity.cacheEpoch != m_cacheEpoch))) {
    ++m_misses;
    return std::nullopt;
  }
  found->second.lastAccess = ++m_accessSequence;
  found->second.lastUsedAtMs = nowMs;
  ++m_hits;
  return found->second.binding;
}

std::optional<ConversationStateBinding>
ConversationStateStore::resolveStaged(
  const ConversationStateReferenceV1& reference,
  std::uint64_t nowMs)
{
  reference.validate();
  cleanupExpired(nowMs);
  ConversationStateBinding keyBinding;
  keyBinding.conversationId = reference.conversationId;
  keyBinding.contextEpoch = reference.contextEpoch;
  keyBinding.identity.roleName = reference.roleName;
  const auto key = keyFor(keyBinding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() ||
      found->second.lifecycle != ConversationStateLifecycle::COMMITTING ||
      found->second.pinCount != 1 || nowMs >= found->second.expiresAtMs ||
      nowMs >= reference.expiresAtMs ||
      found->second.binding.serviceName != reference.serviceName ||
      found->second.binding.planRoleMapDigest != reference.planRoleMapDigest ||
      found->second.binding.receiptDigest != reference.roleReceiptDigest ||
      (!m_providerIdentity.empty() &&
       (found->second.binding.identity.providerIdentity != m_providerIdentity ||
        found->second.binding.identity.providerBootId != m_providerBootId ||
        found->second.binding.identity.cacheEpoch != m_cacheEpoch))) {
    return std::nullopt;
  }
  return found->second.binding;
}

bool
ConversationStateStore::pin(const ConversationStateBinding& binding,
                             std::uint64_t nowMs)
{
  binding.validate();
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() || !compatible(found->second.binding, binding) ||
      found->second.residency != ConversationStateResidency::GPU_RESIDENT ||
      found->second.lifecycle == ConversationStateLifecycle::COMMITTING ||
      nowMs >= found->second.expiresAtMs) {
    return false;
  }
  ++found->second.pinCount;
  found->second.lifecycle = ConversationStateLifecycle::PINNED;
  found->second.lastAccess = ++m_accessSequence;
  found->second.lastUsedAtMs = nowMs;
  return true;
}

bool
ConversationStateStore::unpin(const ConversationStateBinding& binding)
{
  binding.validate();
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() || !compatible(found->second.binding, binding) ||
      found->second.pinCount == 0) {
    return false;
  }
  --found->second.pinCount;
  if (found->second.pinCount == 0) {
    found->second.lifecycle = ConversationStateLifecycle::IDLE;
  }
  return true;
}

bool
ConversationStateStore::pauseToHost(const ConversationStateBinding& binding,
                                     std::uint64_t nowMs)
{
  binding.validate();
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() || !compatible(found->second.binding, binding) ||
      found->second.residency != ConversationStateResidency::GPU_RESIDENT ||
      found->second.lifecycle != ConversationStateLifecycle::IDLE ||
      found->second.pinCount != 0 || nowMs >= found->second.expiresAtMs) {
    return false;
  }
  const auto bytes = found->second.allocatedBytes;
  if (!evictHostUntilFits(bytes, key)) {
    return false;
  }
  if (found->second.adapterState && found->second.adapterRunner &&
      !found->second.adapterRunner->pauseConversationStateToHost(
        *found->second.adapterState)) {
    return false;
  }
  m_gpuBytes -= std::min(m_gpuBytes, bytes);
  m_hostBytes += bytes;
  found->second.residency = ConversationStateResidency::HOST_RESIDENT;
  found->second.lastAccess = ++m_accessSequence;
  found->second.lastUsedAtMs = nowMs;
  return true;
}

std::future<bool>
ConversationStateStore::prefetchToGpu(const ConversationStateBinding& binding,
                                     std::uint64_t nowMs)
{
  binding.validate();
  const auto key = keyFor(binding);
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_entries.find(key);
    if (found == m_entries.end() || !compatible(found->second.binding, binding) ||
        found->second.lifecycle == ConversationStateLifecycle::COMMITTING ||
        nowMs >= found->second.expiresAtMs) {
      ++m_misses;
      return readyBool(false);
    }
    if (found->second.residency == ConversationStateResidency::GPU_RESIDENT) {
      return readyBool(true);
    }
    if (found->second.lifecycle != ConversationStateLifecycle::IDLE) {
      return readyBool(false);
    }
    found->second.lifecycle = ConversationStateLifecycle::PREFETCHING;
    generation = ++found->second.prefetchGeneration;
  }
  const auto queuedAt = std::chrono::steady_clock::now();
  std::future<bool> adapterTransfer;
  std::shared_ptr<NativeModelRunner> adapterRunner;
  std::optional<NativeConversationStateHandleV1> adapterState;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_entries.find(key);
    if (found != m_entries.end()) {
      adapterRunner = found->second.adapterRunner;
      adapterState = found->second.adapterState;
    }
  }
  if (adapterRunner && adapterState) {
    adapterTransfer = adapterRunner->prefetchConversationStateToGpu(*adapterState);
  }
  return std::async(
    std::launch::async,
    [this, binding, key, generation, nowMs, queuedAt,
     adapterTransfer = std::move(adapterTransfer)] () mutable {
      const auto started = std::chrono::steady_clock::now();
      bool transferred = true;
      if (adapterTransfer.valid()) {
        try {
          transferred = adapterTransfer.get();
        }
        catch (...) {
          transferred = false;
        }
      }
      const auto elapsedMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
          started - queuedAt).count());
      const auto completedNowMs = nowMs + elapsedMs;
      // Admission used the caller's timestamp.  Rebase that same clock at
      // completion so a delayed worker cannot resurrect an entry after its
      // retention deadline.  (Callers may use a logical test clock, so using
      // system_clock here would incorrectly expire every such entry.)
      cleanupExpired(completedNowMs);
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto found = m_entries.find(key);
      if (found == m_entries.end() || !compatible(found->second.binding, binding) ||
          found->second.prefetchGeneration != generation ||
          found->second.lifecycle != ConversationStateLifecycle::PREFETCHING ||
          completedNowMs >= found->second.expiresAtMs) {
        return false;
      }
      if (!transferred) {
        found->second.lifecycle = ConversationStateLifecycle::IDLE;
        return false;
      }
      const auto bytes = found->second.allocatedBytes;
      if (!evictGpuUntilFits(bytes, key)) {
        found->second.lifecycle = ConversationStateLifecycle::IDLE;
        return false;
      }
      m_hostBytes -= std::min(m_hostBytes, bytes);
      m_gpuBytes += bytes;
      found->second.residency = ConversationStateResidency::GPU_RESIDENT;
      found->second.lifecycle = ConversationStateLifecycle::IDLE;
      found->second.lastAccess = ++m_accessSequence;
      found->second.lastUsedAtMs = completedNowMs;
      ++m_prefetchedEntries;
      m_prefetchBytes += bytes;
      m_prefetchLatencyMs += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started).count());
      return true;
    });
}

bool
ConversationStateStore::cancelPrefetch(const ConversationStateBinding& binding)
{
  binding.validate();
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() || !compatible(found->second.binding, binding) ||
      found->second.lifecycle != ConversationStateLifecycle::PREFETCHING) {
    return false;
  }
  // Invalidate the generation observed by the worker.  The worker will leave
  // the entry in HOST_RESIDENT and return false when it eventually wakes; it
  // can then be retried or released by the owning request.
  if (found->second.adapterState && found->second.adapterRunner) {
    (void)found->second.adapterRunner->cancelConversationStatePrefetch(
      *found->second.adapterState);
  }
  ++found->second.prefetchGeneration;
  found->second.lifecycle = ConversationStateLifecycle::IDLE;
  return true;
}

bool
ConversationStateStore::release(const ConversationStateBinding& binding)
{
  binding.validate();
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() || !compatible(found->second.binding, binding) ||
      found->second.lifecycle != ConversationStateLifecycle::IDLE ||
      found->second.pinCount != 0) {
    return false;
  }
  removeEntry(key, true);
  return true;
}

std::size_t
ConversationStateStore::cleanupExpired(std::uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::size_t removed = 0;
  for (auto it = m_entries.begin(); it != m_entries.end();) {
    if (nowMs < it->second.expiresAtMs) {
      ++it;
      continue;
    }
    const auto key = it->first;
    ++it;
    removeEntry(key, true);
    ++removed;
  }
  return removed;
}

void
ConversationStateStore::clear()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_cleanups += m_entries.size();
  for (auto& item : m_entries) {
    if (item.second.adapterState && item.second.adapterRunner) {
      (void)item.second.adapterRunner->releaseConversationState(
        *item.second.adapterState);
    }
    wipeTensorBundle(item.second.state);
  }
  m_entries.clear();
  m_gpuBytes = 0;
  m_hostBytes = 0;
}

ConversationStateSnapshot
ConversationStateStore::snapshot() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  ConversationStateSnapshot result;
  result.entries = m_entries.size();
  result.gpuBytes = m_gpuBytes;
  result.hostBytes = m_hostBytes;
  result.requestLocalReleases = m_requestLocalReleases;
  result.stagedPromotions = m_stagedPromotions;
  result.promotions = m_promotions;
  result.promotionRollbacks = m_promotionRollbacks;
  result.hits = m_hits;
  result.misses = m_misses;
  result.evictions = m_evictions;
  result.prefetchedEntries = m_prefetchedEntries;
  result.prefetchBytes = m_prefetchBytes;
  result.prefetchLatencyMs = m_prefetchLatencyMs;
  result.cleanups = m_cleanups;
  for (const auto& item : m_entries) {
    const auto& entry = item.second;
    if (entry.residency == ConversationStateResidency::GPU_RESIDENT) {
      ++result.gpuEntries;
    }
    else if (entry.residency == ConversationStateResidency::HOST_RESIDENT) {
      ++result.hostEntries;
    }
    if (entry.pinCount != 0) {
      ++result.pinnedEntries;
    }
    if (entry.lifecycle == ConversationStateLifecycle::COMMITTING) {
      ++result.committingEntries;
    }
    if (entry.lifecycle == ConversationStateLifecycle::PREFETCHING) {
      ++result.prefetchingEntries;
    }
  }
  return result;
}

NativeProviderRuntime::NativeProviderRuntime(
  std::size_t workerCount,
  std::size_t readyQueueCapacity,
  std::size_t decodeStateMaxBytes,
  std::size_t decodeStateMaxEntries,
  std::size_t conversationGpuMaxBytes,
  std::size_t conversationGpuMaxEntries,
  std::size_t conversationHostMaxBytes,
  std::size_t conversationHostMaxEntries,
  std::uint64_t conversationRetentionMs)
  : m_decodeStateStore(decodeStateMaxBytes, decodeStateMaxEntries)
  , m_conversationStateStore(conversationGpuMaxBytes,
                             conversationGpuMaxEntries,
                             conversationHostMaxBytes,
                             conversationHostMaxEntries,
                             conversationRetentionMs)
  , m_worker(workerCount, 4, 1024, std::chrono::seconds(120),
             readyQueueCapacity)
{
}

void
NativeProviderRuntime::registerRunner(std::string role,
                                      std::shared_ptr<NativeModelRunner> runner)
{
  if (role.empty()) {
    throw std::invalid_argument("NativeProviderRuntime role must not be empty");
  }
  if (!runner) {
    throw std::invalid_argument("NativeProviderRuntime runner must not be null");
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  m_runners[std::move(role)] = std::move(runner);
}

void
NativeProviderRuntime::registerRunner(NativeModelRunnerSpec spec,
                                      std::shared_ptr<NativeModelRunner> runner)
{
  if (spec.role.empty()) {
    throw std::invalid_argument("NativeProviderRuntime role must not be empty");
  }
  if (!runner) {
    throw std::invalid_argument("NativeProviderRuntime runner must not be null");
  }
  const auto providerBootId = metadataValue(
    spec, {"evidence.providerBootId", "providerBootId", "provider_boot_id"});
  if (!providerBootId.empty()) {
    m_decodeStateStore.setProviderBootId(providerBootId);
  }
  const auto providerIdentity = metadataValue(
    spec, {"evidence.providerName", "providerName", "provider_name"});
  const auto cacheEpoch = metadataUint64(
    spec, {"state.cacheEpoch", "cacheEpoch", "state.securityEpoch",
           "securityEpoch"}, 1);
  bool conversationBindingChanged = false;
  if (!providerIdentity.empty() && !providerBootId.empty()) {
    conversationBindingChanged = m_conversationStateStore.setProviderBinding(
      providerIdentity, providerBootId, cacheEpoch);
  }
  if (conversationBindingChanged) {
    // The store invalidates committed entries on a Provider boot/cache
    // change.  Clear the NativeProviderRuntime side index as well; leaving
    // staged records behind would retain obsolete ownership and make a later
    // COMMIT look like a live candidate.
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stagedConversationPromotions.clear();
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  m_runnerSpecs[spec.role] = spec;
  m_runners[std::move(spec.role)] = std::move(runner);
}

void
NativeProviderRuntime::registerRunner(std::string role, RoleRunner runner)
{
  registerRunner(std::move(role), makeNativeModelRunner(std::move(runner)));
}

bool
NativeProviderRuntime::hasRunner(const std::string& role) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_runners.find(role) != m_runners.end();
}

std::future<ProviderRoleResult>
NativeProviderRuntime::executeRoleAsync(std::string sessionId,
                                        RoleSpec role,
                                        std::shared_ptr<DependencyIo> io,
                                        std::map<std::string, TensorBundle> initialInputsByScope,
                                        RoleExecutionContext::StreamEventSink eventSink,
                                        std::function<void()> executionGuard)
{
  return executeRoleAsyncImpl(std::move(sessionId), std::move(role),
                              std::move(io), nullptr, {},
                              std::move(initialInputsByScope),
                              std::move(eventSink), std::move(executionGuard));
}

std::future<ProviderRoleResult>
NativeProviderRuntime::executePreparedRoleAsync(
  std::string sessionId,
  RoleSpec role,
  std::shared_ptr<DependencyIo> io,
  ProviderRoleWorker::NativeRunnerPreparation prepareRunner,
  std::map<std::string, TensorBundle> initialInputsByScope,
  RoleExecutionContext::StreamEventSink eventSink,
  std::function<void()> executionGuard)
{
  if (!prepareRunner) {
    throw std::invalid_argument(
      "NativeProviderRuntime requires a runner preparation callback");
  }
  return executeRoleAsyncImpl(std::move(sessionId), std::move(role),
                              std::move(io), nullptr,
                              std::move(prepareRunner),
                              std::move(initialInputsByScope),
                              std::move(eventSink), std::move(executionGuard));
}

std::future<std::shared_ptr<NativeModelRunner>>
NativeProviderRuntime::prepareRunnerAsync(
  ProviderRoleWorker::NativeRunnerPreparation prepareRunner,
  std::function<void()> executionGuard)
{
  return m_worker.prepareRunnerAsync(std::move(prepareRunner),
                                     std::move(executionGuard));
}

std::future<ProviderRoleResult>
NativeProviderRuntime::executeRoleAsyncImpl(
  std::string sessionId,
  RoleSpec role,
  std::shared_ptr<DependencyIo> io,
  std::shared_ptr<NativeModelRunner> preparedRunner,
  ProviderRoleWorker::NativeRunnerPreparation prepareRunner,
  std::map<std::string, TensorBundle> initialInputsByScope,
  RoleExecutionContext::StreamEventSink eventSink,
  std::function<void()> executionGuard)
{
  if (executionGuard) executionGuard();
  const auto timelineRequestId = role.requestId.empty()
    ? "/ndnsf-di/session/" + sessionId
    : role.requestId;
  logDiTimelineTrace(
    "di-provider", "role_validation_start", timelineRequestId,
    {{"sessionId", sessionId},
     {"role", role.role},
     {"inferenceEpoch", std::to_string(role.inferenceEpoch)},
     {"providerBootId", role.candidateDecodeStateIdentity ?
                          role.candidateDecodeStateIdentity->providerBootId : "none"},
     {"attemptEpoch", std::to_string(role.attemptEpoch)}});
  auto runner = std::move(preparedRunner);
  if (!runner && !prepareRunner) {
    runner = findRunner(role.role);
  }
  const auto runnerSpec = findRunnerSpec(role.role);
  const bool stateful = !role.stateInputNames.empty() ||
                        !role.stateOutputNames.empty();
  std::optional<KvStateBinding> stateBinding;
  std::optional<KvStateBinding> predecessorBinding;
  std::optional<ConversationStateBinding> conversationBinding;
  if (stateful) {
    if (role.stateInputNames.size() != role.stateOutputNames.size() ||
        role.stateInputNames.empty()) {
      throw std::invalid_argument(
        "Provider decode-state input/output metadata length mismatch");
    }
    if (!runnerSpec.has_value() && !role.candidateDecodeStateIdentity) {
      throw std::invalid_argument(
        "Provider stateful role requires registered runner metadata");
    }
    const NativeModelRunnerSpec emptyRunnerSpec;
    stateBinding = decodeStateBindingFor(
      runnerSpec ? *runnerSpec : emptyRunnerSpec, sessionId, role);
    if (!runnerSpec && role.candidateDecodeStateIdentity) {
      m_decodeStateStore.setProviderBootId(
        role.candidateDecodeStateIdentity->providerBootId);
    }
    if (role.conversationStateBinding) {
      if (role.inferenceEpoch != 0 ||
          role.conversationStateLookupNowMs == 0 ||
          initialInputsByScope.count("__ndnsf_provider_decode_state") != 0) {
        throw std::invalid_argument(
          "conversation state restore must start a fresh request epoch");
      }
      auto requested = *role.conversationStateBinding;
      requested.validate();
      if (requested.identity.roleName != role.role ||
          !role.candidateDecodeStateIdentity ||
          role.candidateDecodeStateIdentity->requestId ==
            requested.identity.requestId ||
          role.candidateDecodeStateIdentity->generationId ==
            requested.identity.generationId) {
        throw std::invalid_argument(
          "conversation state restore requires a fresh request identity");
      }
      // A host-resident entry must be made dispatchable before the runner is
      // called.  The prefetch is single-flight in ConversationStateStore, so
      // concurrent turns cannot duplicate the transfer.  Pin the restored
      // entry until this role's worker has finished; otherwise an inactive-LRU
      // eviction could invalidate the state between lookup and execution.
      if (!m_conversationStateStore.prefetchToGpu(
            requested, role.conversationStateLookupNowMs).get()) {
        throw std::runtime_error("PROVIDER_CONVERSATION_STATE_MISSING");
      }
      if (!m_conversationStateStore.pin(
            requested, role.conversationStateLookupNowMs)) {
        throw std::runtime_error("PROVIDER_CONVERSATION_STATE_MISSING");
      }
      conversationBinding = std::move(requested);
      if (const auto adapterState = m_conversationStateStore.adapterHandle(
            *conversationBinding, role.conversationStateLookupNowMs)) {
        if (!runner->restoreConversationState(*adapterState, sessionId)) {
          m_conversationStateStore.unpin(*conversationBinding);
          conversationBinding.reset();
          throw std::runtime_error("PROVIDER_CONVERSATION_STATE_MISSING");
        }
      }
      else {
        auto restored = m_conversationStateStore.lookup(
          *conversationBinding, role.conversationStateLookupNowMs);
        if (!restored) {
          m_conversationStateStore.unpin(*conversationBinding);
          conversationBinding.reset();
          throw std::runtime_error("PROVIDER_CONVERSATION_STATE_MISSING");
        }
        initialInputsByScope["__ndnsf_provider_decode_state"] =
          std::move(*restored);
      }
      // Emit only after the exact retained parent was pinned and restored.
      // This is distinct from an assembled-model/runner cache hit.
      std::ostringstream record;
      record << "NDNSF_DI_CONVERSATION_KV_RESTORED"
             << " requestId=" << role.candidateDecodeStateIdentity->requestId
             << " role=" << role.role
             << " parentContextEpoch=" << conversationBinding->contextEpoch
             << " prefixTokenCount=" << conversationBinding->identity.prefixTokenCount;
      logRuntimeEvidence(record.str());
    }
    if (role.inferenceEpoch > 0) {
      if (role.predecessorDecodeStateIdentity.has_value() !=
          role.candidateDecodeStateIdentity.has_value()) {
        throw std::invalid_argument(
          "Provider decode-state transition has incomplete exact identity");
      }
      auto predecessor = *stateBinding;
      predecessor.contextEpoch = role.inferenceEpoch - 1;
      predecessor.exactIdentity = role.predecessorDecodeStateIdentity;
      predecessor.validate();
      auto committed = m_decodeStateStore.beginTransition(predecessor);
      if (!committed.has_value()) {
        ++m_decodeStateMisses;
        throw std::runtime_error("PROVIDER_DECODE_STATE_MISSING");
      }
      ++m_decodeStateHits;
      predecessorBinding = predecessor;
      initialInputsByScope["__ndnsf_provider_decode_state"] =
        std::move(*committed);
    }
    else if (role.attemptEpoch > 1) {
      ++m_decodeStateRecomputes;
    }
  }
  logDiTimelineTrace(
    "di-provider", "role_validation_done", timelineRequestId,
    {{"sessionId", sessionId},
     {"role", role.role},
     {"inferenceEpoch", std::to_string(role.inferenceEpoch)},
     {"providerBootId", role.candidateDecodeStateIdentity ?
                          role.candidateDecodeStateIdentity->providerBootId : "none"},
     {"attemptEpoch", std::to_string(role.attemptEpoch)}});
  std::future<ProviderRoleResult> workerFuture;
  try {
    if (prepareRunner) {
      workerFuture = m_worker.executePreparedAsync(
        sessionId, role, std::move(io), std::move(prepareRunner),
        std::move(initialInputsByScope), std::move(eventSink),
        executionGuard);
    }
    else {
      workerFuture = m_worker.executeAsync(
        sessionId, role, std::move(io), std::move(runner),
        std::move(initialInputsByScope), std::move(eventSink),
        executionGuard);
    }
  }
  catch (...) {
    if (predecessorBinding &&
        m_decodeStateStore.rollbackTransition(*predecessorBinding)) {
      ++m_decodeStateRollbacks;
    }
    throw;
  }
  if (!stateful) {
    return workerFuture;
  }
  return std::async(
    std::launch::async,
    [this, role = std::move(role), stateSessionId = sessionId,
     binding = std::move(*stateBinding),
     predecessor = std::move(predecessorBinding),
     conversationBinding = std::move(conversationBinding),
     executionGuard = std::move(executionGuard),
     future = std::move(workerFuture)] () mutable {
      bool candidateStaged = false;
      const auto releaseConversationPin = [this, &conversationBinding] {
        if (conversationBinding) {
          m_conversationStateStore.unpin(*conversationBinding);
          conversationBinding.reset();
        }
      };
      try {
        auto result = future.get();
        if (executionGuard) executionGuard();
        TensorBundle state;
        if (role.streamingStateExecution &&
            result.runnerSupportsOpaqueStateHandles) {
          if (!result.stateHandle) {
            throw std::runtime_error(
              "PROVIDER_OPAQUE_STATE_HANDLE_MISSING");
          }
          result.stateHandle->validate();
          if (!binding.exactIdentity ||
              result.stateHandle->providerIdentity !=
                binding.exactIdentity->providerIdentity ||
              result.stateHandle->providerBootId !=
                binding.exactIdentity->providerBootId ||
              result.stateHandle->sessionId != stateSessionId ||
              result.stateHandle->role != role.role ||
              result.stateHandle->stateInferenceEpoch !=
                binding.exactIdentity->stateInferenceEpoch) {
            throw std::runtime_error(
              "PROVIDER_OPAQUE_STATE_HANDLE_IDENTITY_MISMATCH");
          }
          state = opaqueStateBundleFromHandle(*result.stateHandle, role,
                                              stateSessionId);
        }
        else if (result.providerDecodeState) {
          state = *result.providerDecodeState;
        }
        else {
          state = stateBundleFromResult(result, role);
        }
        if (executionGuard) executionGuard();
        if (!m_decodeStateStore.stageCandidate(
              predecessor, binding, std::move(state))) {
          ++m_decodeStateCommitFailures;
          throw std::runtime_error("PROVIDER_DECODE_STATE_CANDIDATE_FAILED");
        }
        candidateStaged = true;
        if (!role.deferStateCommit) {
          if (executionGuard) executionGuard();
          if (!m_decodeStateStore.commitCandidate(binding)) {
            ++m_decodeStateCommitFailures;
            throw std::runtime_error("PROVIDER_DECODE_STATE_COMMIT_FAILED");
          }
          candidateStaged = false;
          ++m_decodeStateCommits;
        }
        releaseConversationPin();
        return result;
      }
      catch (...) {
        releaseConversationPin();
        const auto rollbackBinding = candidateStaged
          ? binding
          : predecessor;
        if (rollbackBinding &&
            m_decodeStateStore.rollbackTransition(*rollbackBinding)) {
          ++m_decodeStateRollbacks;
        }
        throw;
      }
    });
}

ProviderRoleWorkerSnapshot
NativeProviderRuntime::snapshot() const
{
  return m_worker.snapshot();
}

ProviderDecodeStateSnapshot
NativeProviderRuntime::decodeStateSnapshot() const
{
  ProviderDecodeStateSnapshot state;
  state.hits = m_decodeStateHits.load();
  state.misses = m_decodeStateMisses.load();
  state.commits = m_decodeStateCommits.load();
  state.commitFailures = m_decodeStateCommitFailures.load();
  state.rollbacks = m_decodeStateRollbacks.load();
  state.recomputes = m_decodeStateRecomputes.load();
  state.evictions = m_decodeStateStore.evictionCount();
  state.cleanups = m_decodeStateStore.cleanupCount();
  state.entries = m_decodeStateStore.size();
  state.bytes = m_decodeStateStore.usedBytes();
  state.pinnedEntries = m_decodeStateStore.pinnedCount();
  state.candidates = m_decodeStateStore.candidateCount();
  return state;
}

ConversationStateSnapshot
NativeProviderRuntime::conversationStateSnapshot() const
{
  return m_conversationStateStore.snapshot();
}

bool
NativeProviderRuntime::promoteDecodeStateToConversation(
  const std::string& sessionId,
  const RoleSpec& role,
  ConversationStateBinding binding,
  std::uint64_t nowMs)
{
  const auto promotionBinding = binding;
  if (!stageDecodeStatePromotion(sessionId, role, std::move(binding), nowMs)) {
    return false;
  }
  if (commitStagedDecodeStatePromotion(promotionBinding)) {
    return true;
  }
  rollbackStagedDecodeStatePromotion(promotionBinding);
  return false;
}

bool
NativeProviderRuntime::stageDecodeStatePromotion(
  const std::string& sessionId,
  const RoleSpec& role,
  ConversationStateBinding binding,
  std::uint64_t nowMs)
{
  binding.validate();
  const auto runnerSpec = findRunnerSpec(role.role);
  if (!role.candidateDecodeStateIdentity ||
      role.candidateDecodeStateIdentity->roleName != role.role ||
      *role.candidateDecodeStateIdentity != binding.identity) {
    return false;
  }
  // Post-Selection preparation creates the exact runner for this request but
  // intentionally does not publish a process-wide runner spec.  The complete
  // candidate identity is sufficient to derive the request-local source key.
  const NativeModelRunnerSpec emptyRunnerSpec;
  const auto sourceBinding = decodeStateBindingFor(
    runnerSpec ? *runnerSpec : emptyRunnerSpec, sessionId, role);
  const auto promotionBinding = binding;
  std::shared_ptr<NativeModelRunner> runner;
  if (hasRunner(role.role)) {
    runner = findRunner(role.role);
  }
  if (runner && runner->supportsConversationStateTransfer()) {
    const auto conversationKey = conversationPromotionKey(promotionBinding);
    const auto adapterState = runner->promoteSessionStateToConversation(
      sessionId, conversationKey);
    if (!adapterState ||
        !m_conversationStateStore.stageAdapterPromotion(
          sourceBinding.requestId, role.role, std::move(binding),
          *adapterState, runner, nowMs)) {
      if (adapterState) {
        (void)runner->releaseConversationState(*adapterState);
      }
      return false;
    }
  }
  else {
    auto state = m_decodeStateStore.lookup(sourceBinding);
    if (!state) {
      // A conversation turn deliberately defers decode-state commit until
      // the requester commits the complete role set.  Promote that exact
      // request-local candidate without making it visible to a later turn.
      state = m_decodeStateStore.lookupCandidate(sourceBinding);
    }
    if (!state.has_value()) {
      return false;
    }
    if (!m_conversationStateStore.stagePromotion(
          sourceBinding.requestId, role.role, std::move(binding), *state, nowMs)) {
      return false;
    }
  }
  StagedConversationPromotion staged;
  staged.sessionId = sessionId;
  staged.role = role;
  staged.binding = promotionBinding;
  const auto key = conversationPromotionKey(promotionBinding);
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_stagedConversationPromotions.emplace(key, std::move(staged)).second) {
      m_conversationStateStore.rollbackStagedPromotion(promotionBinding);
      return false;
    }
  }
  return true;
}

bool
NativeProviderRuntime::commitStagedDecodeStatePromotion(
  const ConversationStateBinding& binding)
{
  StagedConversationPromotion staged;
  const auto key = conversationPromotionKey(binding);
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_stagedConversationPromotions.find(key);
    if (found == m_stagedConversationPromotions.end() ||
        !(found->second.binding == binding)) {
      return false;
    }
    staged = found->second;
  }
  if (!staged.role.candidateDecodeStateIdentity ||
      *staged.role.candidateDecodeStateIdentity != binding.identity ||
      !m_conversationStateStore.commitStagedPromotion(binding)) {
    return false;
  }
  if (!m_decodeStateStore.erase(staged.sessionId, staged.role.role)) {
    // Keep the conversation store fail-closed if ownership cannot be removed
    // from the request-local store.  Normal runtime operation is serialized
    // per role, so this is a defensive rollback path.
    m_conversationStateStore.release(binding);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      // Do not leave an orphaned promotion record that can block a later
      // successor with the same conversation/epoch/role key.
      m_stagedConversationPromotions.erase(key);
    }
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stagedConversationPromotions.erase(key);
  }
  m_conversationStateStore.noteRequestLocalRelease();
  return true;
}

bool
NativeProviderRuntime::commitStagedDecodeStatePromotion(
  const ConversationStateBinding& binding,
  const std::string& checkpointDigest)
{
  StagedConversationPromotion staged;
  const auto key = conversationPromotionKey(binding);
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_stagedConversationPromotions.find(key);
    if (found == m_stagedConversationPromotions.end() ||
        !(found->second.binding == binding)) {
      return false;
    }
    staged = found->second;
  }
  if (!staged.role.candidateDecodeStateIdentity ||
      *staged.role.candidateDecodeStateIdentity != binding.identity) {
    return false;
  }
  if (!m_conversationStateStore.commitStagedPromotion(binding,
                                                       checkpointDigest)) {
    return false;
  }
  auto committedBinding = binding;
  committedBinding.checkpointDigest = checkpointDigest;
  if (!m_decodeStateStore.erase(staged.sessionId, staged.role.role)) {
    // Keep the conversation store fail-closed if ownership cannot be removed
    // from the request-local store. Normal runtime operation is serialized per
    // role, so this remains a defensive rollback path.
    m_conversationStateStore.release(committedBinding);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stagedConversationPromotions.erase(key);
    }
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stagedConversationPromotions.erase(key);
  }
  m_conversationStateStore.noteRequestLocalRelease();
  return true;
}

bool
NativeProviderRuntime::rollbackStagedDecodeStatePromotion(
  const ConversationStateBinding& binding)
{
  if (!m_conversationStateStore.rollbackStagedPromotion(binding)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  m_stagedConversationPromotions.erase(conversationPromotionKey(binding));
  return true;
}

std::optional<TensorBundle>
NativeProviderRuntime::lookupConversationState(
  const ConversationStateBinding& binding,
  std::uint64_t nowMs)
{
  return m_conversationStateStore.lookup(binding, nowMs);
}

std::optional<ConversationStateBinding>
NativeProviderRuntime::resolveConversationState(
  const ConversationStateReferenceV1& reference,
  std::uint64_t nowMs)
{
  return m_conversationStateStore.resolve(reference, nowMs);
}

std::optional<ConversationStateBinding>
NativeProviderRuntime::resolveStagedConversationState(
  const ConversationStateReferenceV1& reference,
  std::uint64_t nowMs)
{
  return m_conversationStateStore.resolveStaged(reference, nowMs);
}

std::string
NativeProviderRuntime::conversationPromotionKey(
  const ConversationStateBinding& binding)
{
  // Keep the internal key single-line because the adapter handle is carried
  // through validation/logging as metadata; use the same non-printing
  // delimiter as ConversationStateStore::keyFor rather than embedding newlines.
  return binding.conversationId + '\x1f' +
         std::to_string(binding.contextEpoch) + '\x1f' +
         binding.identity.roleName;
}

bool
NativeProviderRuntime::pauseConversationStateToHost(
  const ConversationStateBinding& binding,
  std::uint64_t nowMs)
{
  return m_conversationStateStore.pauseToHost(binding, nowMs);
}

std::future<bool>
NativeProviderRuntime::prefetchConversationStateToGpu(
  const ConversationStateBinding& binding,
  std::uint64_t nowMs)
{
  return m_conversationStateStore.prefetchToGpu(binding, nowMs);
}

bool
NativeProviderRuntime::cancelConversationStatePrefetch(
  const ConversationStateBinding& binding)
{
  return m_conversationStateStore.cancelPrefetch(binding);
}

bool
NativeProviderRuntime::pinConversationState(
  const ConversationStateBinding& binding,
  std::uint64_t nowMs)
{
  return m_conversationStateStore.pin(binding, nowMs);
}

bool
NativeProviderRuntime::unpinConversationState(
  const ConversationStateBinding& binding)
{
  return m_conversationStateStore.unpin(binding);
}

bool
NativeProviderRuntime::releaseConversationState(
  const ConversationStateBinding& binding)
{
  return m_conversationStateStore.release(binding);
}

bool
NativeProviderRuntime::commitDecodeStateTransition(
  const std::string& sessionId,
  const RoleSpec& role)
{
  const auto runnerSpec = findRunnerSpec(role.role);
  if (!runnerSpec && !role.candidateDecodeStateIdentity) {
    return false;
  }
  const NativeModelRunnerSpec emptyRunnerSpec;
  const auto binding = decodeStateBindingFor(
    runnerSpec ? *runnerSpec : emptyRunnerSpec, sessionId, role);
  if (!m_decodeStateStore.commitCandidate(binding)) {
    ++m_decodeStateCommitFailures;
    return false;
  }
  ++m_decodeStateCommits;
  return true;
}

bool
NativeProviderRuntime::rollbackDecodeStateTransition(
  const std::string& sessionId,
  const RoleSpec& role)
{
  const auto runnerSpec = findRunnerSpec(role.role);
  if (!runnerSpec && !role.candidateDecodeStateIdentity) {
    return false;
  }
  const NativeModelRunnerSpec emptyRunnerSpec;
  const auto binding = decodeStateBindingFor(
    runnerSpec ? *runnerSpec : emptyRunnerSpec, sessionId, role);
  if (!m_decodeStateStore.rollbackTransition(binding)) {
    return false;
  }
  ++m_decodeStateRollbacks;
  return true;
}

bool
NativeProviderRuntime::releaseDecodeState(const std::string& sessionId,
                                          const std::string& role)
{
  std::shared_ptr<NativeModelRunner> runner;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_runners.find(role);
    if (found != m_runners.end()) {
      runner = found->second;
    }
  }
  const auto erased = m_decodeStateStore.erase(sessionId, role);
  if (runner) {
    runner->releaseSessionState(sessionId);
  }
  return erased;
}

std::shared_ptr<NativeModelRunner>
NativeProviderRuntime::findRunner(const std::string& role) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_runners.find(role);
  if (found == m_runners.end()) {
    throw std::out_of_range("NativeProviderRuntime has no runner for role: " + role);
  }
  return found->second;
}

std::optional<NativeModelRunnerSpec>
NativeProviderRuntime::findRunnerSpec(const std::string& role) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_runnerSpecs.find(role);
  if (found == m_runnerSpecs.end()) {
    return std::nullopt;
  }
  return found->second;
}

} // namespace ndnsf::di
