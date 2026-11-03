#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DiTimelineTrace.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"

#include <ndn-cxx/name.hpp>
#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ndnsf::di {

namespace {

bool
rejectConversationImport(const char* stage, const char* reason,
                         const ConversationStateBinding& binding)
{
  logRuntimeEvidence(std::string("NDNSF_DI_KV_IMPORT_REJECTED stage=") + stage +
    " reason=" + reason + " requestId=" + binding.identity.requestId +
    " role=" + binding.identity.roleName);
  return false;
}

std::uint64_t
conversationRetentionDeadline(std::uint64_t nowMs, std::uint64_t retentionMs)
{
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  return nowMs > maximum - retentionMs ? maximum : nowMs + retentionMs;
}

bool
isCanonicalScopeName(const std::string& value)
{
  if (value.empty() || value.size() > 4096) return false;
  try {
    const ndn::Name name(value);
    return !name.empty() && name.toUri() == value;
  }
  catch (const std::exception&) { return false; }
}

bool
isConversationScopeId(const std::string& value)
{
  return !value.empty() && value.size() <= 4096 && value.find('/') == std::string::npos;
}

bool
saveMetadataMatches(const ConversationSaveMetadataV1& metadata,
                    const ConversationStateBinding& binding)
{
  try {
    binding.validate();
    metadata.receipt.validate();
    const auto& receipt = metadata.receipt;
    const auto& identity = binding.identity;
    const ndn::Name requester(metadata.authenticatedRequester);
    return !requester.empty() && requester.toUri() == metadata.authenticatedRequester &&
      receipt.requesterIdentity == metadata.authenticatedRequester &&
      receipt.conversationId == binding.conversationId &&
      receipt.parentContextEpoch + 1 == receipt.successorContextEpoch &&
      receipt.successorContextEpoch == binding.contextEpoch &&
      receipt.serviceName == binding.serviceName &&
      receipt.originRequestId == identity.requestId &&
      receipt.originGenerationId == identity.generationId &&
      receipt.roleName == identity.roleName &&
      receipt.planRoleMapDigest == binding.planRoleMapDigest &&
      receipt.providerIdentity == identity.providerIdentity &&
      receipt.providerBootId == identity.providerBootId &&
      receipt.cacheEpoch == identity.cacheEpoch &&
      receipt.securityDomainDigest == identity.securityDomainDigest &&
      receipt.modelDigest == identity.modelDigest &&
      receipt.graphSemanticDigest == identity.graphSemanticDigest &&
      receipt.adapterDigest == identity.adapterDigest &&
      receipt.roleSplitDigest == identity.roleSplitDigest &&
      receipt.layoutDigest == identity.layoutDigest &&
      receipt.prefixDigest == identity.prefixDigest &&
      receipt.prefixTokenCount == identity.prefixTokenCount &&
      receipt.positionDigest == identity.positionDigest &&
      receipt.stateSchemaDigest == identity.stateSchemaDigest &&
      receipt.stateComponentDigests == identity.stateComponentDigests &&
      receipt.expiresAtMs == binding.expiresAtMs &&
      receipt.computedDigest() == binding.receiptDigest;
  }
  catch (const std::exception&) {
    return false;
  }
}

std::size_t
serializedSaveMetadataBytes(const ConversationSaveMetadataV1& metadata)
{
  // This is the compact canonical wrapper used by the save-batch accounting:
  // {"authenticatedRequester":<JSON string>,"receipt":<receipt JSON>}.
  // Requester strings are canonical NDN URIs, but count JSON escaping anyway
  // so the bound remains exact if that representation is extended later.
  std::size_t requesterJsonBytes = 2;
  for (const auto ch : metadata.authenticatedRequester) {
    const auto byte = static_cast<unsigned char>(ch);
    const std::size_t addition = byte == '"' || byte == '\\' ? 2 :
      (byte < 0x20 ? 6 : 1);
    if (requesterJsonBytes > std::numeric_limits<std::size_t>::max() - addition)
      throw std::overflow_error("conversation-save-metadata-size-overflow");
    requesterJsonBytes += addition;
  }
  const auto receiptJsonBytes = metadata.receipt.toJson().size();
  constexpr std::size_t prefixBytes = sizeof("{\"authenticatedRequester\":") - 1;
  constexpr std::size_t middleBytes = sizeof(",\"receipt\":") - 1;
  constexpr std::size_t suffixBytes = 1; // closing object brace
  const auto maximum = std::numeric_limits<std::size_t>::max();
  if (requesterJsonBytes > maximum - prefixBytes ||
      middleBytes > maximum - prefixBytes - requesterJsonBytes ||
      receiptJsonBytes > maximum - prefixBytes - requesterJsonBytes - middleBytes ||
      suffixBytes > maximum - prefixBytes - requesterJsonBytes - middleBytes - receiptJsonBytes)
    throw std::overflow_error("conversation-save-metadata-size-overflow");
  return prefixBytes + requesterJsonBytes + middleBytes + receiptJsonBytes + suffixBytes;
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
  NativeWorkScope stateWork(role.workStatistics, "role-state-result-pack",
                            NativeWorkKind::LOCAL);
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
          decodeTensorBundle(output.second.payload, role.workStatistics),
          role.stateOutputNames[index]);
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
  if (!isConversationScopeId(conversationId) ||
      parentContextEpoch == std::numeric_limits<std::uint64_t>::max() ||
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
  if (importedOriginIdentity) {
    importedOriginIdentity->validate();
    if (importedOriginIdentity->requestId == identity.requestId ||
        importedOriginIdentity->generationId == identity.generationId ||
        !nativePortableKvIdentityCompatible(*importedOriginIdentity, identity)) {
      throw std::invalid_argument("conversation imported origin is inconsistent");
    }
  }
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
         importedOriginIdentity == other.importedOriginIdentity &&
         sameConversationIdentity(identity, other.identity);
}

bool
nativeConversationStateAllowsContinuation(
  const ConversationStateBinding& binding, const DecodeStateIdentityV1& current)
{
  binding.validate();
  if (binding.identity.requestId != current.requestId &&
      binding.identity.generationId != current.generationId) return true;
  if (!binding.importedOriginIdentity || binding.identity.requestId != current.requestId ||
      binding.identity.generationId != current.generationId) return false;
  auto parent = current;
  parent.prefixDigest = binding.identity.prefixDigest;
  parent.prefixTokenCount = binding.identity.prefixTokenCount;
  parent.positionDigest = binding.identity.positionDigest;
  parent.stateInferenceEpoch = binding.identity.stateInferenceEpoch;
  parent.predecessorInferenceEpoch = binding.identity.predecessorInferenceEpoch;
  parent.validate();
  return parent == binding.identity;
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

struct ConversationStateStore::ExecutionAdmissionToken
{
  std::weak_ptr<ConversationStateStore> owner;
  std::uint64_t id = 0;
  AdmissionScope scope;
  bool armed = false;
  ~ExecutionAdmissionToken()
  {
    if (armed) {
      if (auto store = owner.lock()) store->releaseExecutionAdmission(id);
    }
  }
};

struct ConversationStateStore::SaveFenceToken
{
  std::weak_ptr<ConversationStateStore> owner;
  std::shared_ptr<SaveFenceToken> parent;
  std::uint64_t id = 0;
  SaveScope scope;
  bool armed = false;
  ~SaveFenceToken()
  {
    if (armed) {
      if (auto store = owner.lock()) store->releaseSaveFence(id);
    }
  }
};

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

ConversationStateStore::~ConversationStateStore()
{
  // Coordinators observe/retry failures before dropping their final owner.
  // Final destruction is a bounded best-effort cleanup, never a drain ACK.
  try { clear(); } catch (...) { }
}

bool
ConversationStateStore::saveScopeBlocksAdmission(
  const SaveScope& fence, const AdmissionScope& admission) noexcept
{
  if (fence.generation != admission.generation) return false;
  if (fence.kind == SaveScopeKind::Provider) return true;
  if (fence.service != admission.service) return false;
  if (fence.kind == SaveScopeKind::Service) return true;
  return fence.requester == admission.requester &&
         fence.conversation == admission.conversation;
}

bool
ConversationStateStore::saveScopesOverlap(
  const SaveScope& left, const SaveScope& right) noexcept
{
  if (left.generation != right.generation) return false;
  if (left.kind == SaveScopeKind::Provider || right.kind == SaveScopeKind::Provider)
    return true;
  if (left.service != right.service) return false;
  if (left.kind == SaveScopeKind::Service || right.kind == SaveScopeKind::Service)
    return true;
  return left.requester == right.requester && left.conversation == right.conversation;
}

bool
ConversationStateStore::isEntryFencedLocked(
  const ConversationStateBinding& binding, const std::string& requester) const
{
  for (const auto& item : m_saveFences) {
    const auto& fence = item.second;
    if (fence.generation != m_admissionGeneration) continue;
    if (fence.kind == SaveScopeKind::Provider) return true;
    if (fence.service != binding.serviceName) continue;
    if (fence.kind == SaveScopeKind::Service) return true;
    if (fence.conversation == binding.conversationId &&
        (requester.empty() || fence.requester == requester)) return true;
  }
  return false;
}

bool
ConversationStateStore::executionAdmissionMatchesLocked(
  const ExecutionAdmission* admission,
  const ConversationStateBinding& binding,
  const std::string& authenticatedRequester,
  const std::optional<ConversationSaveMetadataV1>& saveMetadata) const
{
  if (!admission || !admission->m_token) return false;
  const auto& token = *admission->m_token;
  const auto owner = token.owner.lock();
  const auto found = m_executionAdmissions.find(token.id);
  return owner.get() == this && token.armed && found != m_executionAdmissions.end() &&
         token.scope.generation == m_admissionGeneration &&
         found->second.generation == m_admissionGeneration &&
         found->second.service == token.scope.service &&
         found->second.conversation == token.scope.conversation &&
         found->second.requester == token.scope.requester &&
         token.scope.service == binding.serviceName &&
         token.scope.conversation == binding.conversationId &&
         !authenticatedRequester.empty() &&
         token.scope.requester == authenticatedRequester &&
         (!saveMetadata || saveMetadata->authenticatedRequester == authenticatedRequester);
}

void
ConversationStateStore::releaseExecutionAdmission(std::uint64_t id) noexcept
{
  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_executionAdmissions.erase(id);
  }
  catch (...) { }
}

bool
ConversationStateStore::requesterMatchesLocked(
  const Entry& entry, const std::string& requester) const
{
  if (requester.empty()) return true; // Trusted local callers retain their existing API.
  return isCanonicalScopeName(requester) && entry.authenticatedRequester == requester;
}

void
ConversationStateStore::releaseSaveFence(std::uint64_t id) noexcept
{
  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_saveFences.erase(id);
  }
  catch (...) { }
}

std::size_t
ConversationStateStore::controlCapacity(bool fences) const noexcept
{
  const auto maximum = std::numeric_limits<std::size_t>::max();
  const auto entries = m_gpuMaxEntries > maximum - m_hostMaxEntries
    ? maximum : m_gpuMaxEntries + m_hostMaxEntries;
  // At most one conversation and one service fence per retained scope,
  // plus a Provider parent. Fencing stays available when executions are full.
  if (!fences) return entries;
  return entries > (maximum - 1) / 2 ? maximum : 2 * entries + 1;
}

std::optional<ConversationStateStore::ExecutionAdmission>
ConversationStateStore::tryAcquireConversationExecution(
  const std::string& authenticatedRequester,
  const std::string& serviceName,
  const std::string& conversationId)
{
  if (!isCanonicalScopeName(authenticatedRequester) || !isCanonicalScopeName(serviceName) ||
      !isConversationScopeId(conversationId))
    throw std::invalid_argument("conversation-execution-scope-invalid");
  auto owner = weak_from_this().lock();
  if (!owner) throw std::runtime_error("conversation-state-owner-required");
  auto token = std::make_shared<ExecutionAdmissionToken>();
  token->owner = owner;
  token->scope = {authenticatedRequester, serviceName, conversationId};
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_closing || m_executionAdmissions.size() >= controlCapacity(false)) return std::nullopt;
  token->scope.generation = m_admissionGeneration;
  for (const auto& item : m_saveFences) {
    if (saveScopeBlocksAdmission(item.second, token->scope)) return std::nullopt;
  }
  if (m_nextExecutionAdmissionId == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("conversation-execution-admission-id-exhausted");
  token->id = m_nextExecutionAdmissionId;
  const auto inserted = m_executionAdmissions.emplace(token->id, token->scope);
  if (!inserted.second) throw std::logic_error("conversation-execution-admission-id-collision");
  ++m_nextExecutionAdmissionId;
  token->armed = true;
  return ExecutionAdmission(std::move(token));
}

std::optional<ConversationStateStore::SaveFence>
ConversationStateStore::beginConversationSaveFence(
  const std::string& authenticatedRequester,
  const std::string& serviceName,
  const std::string& conversationId)
{
  if (!isCanonicalScopeName(authenticatedRequester) || !isCanonicalScopeName(serviceName) ||
      !isConversationScopeId(conversationId))
    throw std::invalid_argument("conversation-save-fence-scope-invalid");
  auto owner = weak_from_this().lock();
  if (!owner) throw std::runtime_error("conversation-state-owner-required");
  auto token = std::make_shared<SaveFenceToken>();
  token->owner = owner;
  token->scope = {SaveScopeKind::Conversation, authenticatedRequester,
                  serviceName, conversationId};
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_closing || m_saveFences.size() >= controlCapacity(true)) return std::nullopt;
  token->scope.generation = m_admissionGeneration;
  for (const auto& item : m_saveFences) {
    if (saveScopesOverlap(item.second, token->scope)) return std::nullopt;
  }
  if (m_nextSaveFenceId == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("conversation-save-fence-id-exhausted");
  token->id = m_nextSaveFenceId;
  const auto inserted = m_saveFences.emplace(token->id, token->scope);
  if (!inserted.second) throw std::logic_error("conversation-save-fence-id-collision");
  ++m_nextSaveFenceId;
  token->armed = true;
  return SaveFence(std::move(token));
}

std::optional<ConversationStateStore::SaveFence>
ConversationStateStore::beginServiceWithdrawalSaveFence(
  const std::string& serviceName, const std::string& withdrawalOperationId,
  std::uint64_t registrationGeneration)
{
  if (withdrawalOperationId.empty() || withdrawalOperationId.size() > 256 ||
      registrationGeneration == 0)
    throw std::invalid_argument("service-withdrawal-fence-lease-invalid");
  if (!isCanonicalScopeName(serviceName)) {
    throw std::invalid_argument("service-withdrawal-fence-scope-invalid");
  }
  auto owner = weak_from_this().lock();
  if (!owner) throw std::runtime_error("conversation-state-owner-required");
  auto token = std::make_shared<SaveFenceToken>();
  token->owner = owner;
  token->scope = {SaveScopeKind::Service, {}, serviceName, {}};
  token->scope.withdrawalOperationId = withdrawalOperationId;
  token->scope.registrationGeneration = registrationGeneration;
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_closing || m_saveFences.size() >= controlCapacity(true)) return std::nullopt;
  token->scope.generation = m_admissionGeneration;
  for (const auto& item : m_saveFences) {
    if (saveScopesOverlap(item.second, token->scope)) return std::nullopt;
  }
  if (m_nextSaveFenceId == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("conversation-save-fence-id-exhausted");
  token->id = m_nextSaveFenceId;
  const auto inserted = m_saveFences.emplace(token->id, token->scope);
  if (!inserted.second) throw std::logic_error("conversation-save-fence-id-collision");
  ++m_nextSaveFenceId;
  token->armed = true;
  return SaveFence(std::move(token));
}

std::optional<ConversationStateStore::SaveFence>
ConversationStateStore::beginProviderWithdrawalSaveFence(
  const std::string& withdrawalOperationId,
  std::uint64_t registrationGeneration)
{
  if (withdrawalOperationId.empty() || withdrawalOperationId.size() > 256 ||
      registrationGeneration == 0)
    throw std::invalid_argument("provider-withdrawal-fence-lease-invalid");
  auto owner = weak_from_this().lock();
  if (!owner) throw std::runtime_error("conversation-state-owner-required");
  auto token = std::make_shared<SaveFenceToken>();
  token->owner = owner;
  token->scope = {SaveScopeKind::Provider, {}, {}, {}};
  token->scope.withdrawalOperationId = withdrawalOperationId;
  token->scope.registrationGeneration = registrationGeneration;
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_closing || m_saveFences.size() >= controlCapacity(true)) return std::nullopt;
  token->scope.generation = m_admissionGeneration;
  for (const auto& item : m_saveFences) {
    if (saveScopesOverlap(item.second, token->scope)) return std::nullopt;
  }
  if (m_nextSaveFenceId == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("conversation-save-fence-id-exhausted");
  token->id = m_nextSaveFenceId;
  const auto inserted = m_saveFences.emplace(token->id, token->scope);
  if (!inserted.second) throw std::logic_error("conversation-save-fence-id-collision");
  ++m_nextSaveFenceId;
  token->armed = true;
  return SaveFence(std::move(token));
}

std::optional<ConversationStateStore::SaveFence>
ConversationStateStore::borrowServiceWithdrawalSaveFence(
  const SaveFence& parent, const std::string& serviceName,
  const std::string& withdrawalOperationId,
  std::uint64_t registrationGeneration)
{
  if (withdrawalOperationId.empty() || withdrawalOperationId.size() > 256 ||
      registrationGeneration == 0)
    throw std::invalid_argument("service-withdrawal-borrow-lease-invalid");
  if (!isCanonicalScopeName(serviceName)) {
    throw std::invalid_argument("service-withdrawal-borrow-scope-invalid");
  }
  if (!parent.m_token || parent.m_token->owner.lock().get() != this)
    return std::nullopt;
  auto owner = weak_from_this().lock();
  if (!owner) throw std::runtime_error("conversation-state-owner-required");
  auto token = std::make_shared<SaveFenceToken>();
  token->owner = owner;
  token->scope = {SaveScopeKind::Service, {}, serviceName, {}};
  token->scope.withdrawalOperationId = withdrawalOperationId;
  token->scope.registrationGeneration = registrationGeneration;
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_closing || m_saveFences.size() >= controlCapacity(true) ||
      !saveFenceMatchesLocked(parent) ||
      parent.m_token->scope.kind != SaveScopeKind::Provider)
    return std::nullopt;
  token->scope.generation = m_admissionGeneration;
  for (const auto& item : m_saveFences) {
    if (item.first != parent.m_token->id &&
        saveScopesOverlap(item.second, token->scope))
      return std::nullopt;
  }
  if (m_nextSaveFenceId == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("conversation-save-fence-id-exhausted");
  token->id = m_nextSaveFenceId;
  const auto inserted = m_saveFences.emplace(token->id, token->scope);
  if (!inserted.second) throw std::logic_error("conversation-save-fence-id-collision");
  ++m_nextSaveFenceId;
  token->parent = parent.m_token;
  token->armed = true;
  return SaveFence(std::move(token));
}

bool
ConversationStateStore::matchesServiceWithdrawalSaveFence(
  const SaveFence& fence, const std::string& serviceName,
  const std::string& withdrawalOperationId,
  std::uint64_t registrationGeneration) const noexcept
{
  try {
    if (!fence.m_token || fence.m_token->owner.lock().get() != this)
      return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto& scope = fence.m_token->scope;
    return saveFenceMatchesLocked(fence) &&
      scope.kind == SaveScopeKind::Service &&
      scope.service == serviceName &&
      scope.withdrawalOperationId == withdrawalOperationId &&
      scope.registrationGeneration == registrationGeneration;
  }
  catch (...) { return false; }
}

std::optional<ConversationStateStore::SaveFence>
ConversationStateStore::borrowConversationWithdrawalSaveFence(
  const SaveFence& parent, const std::string& authenticatedRequester,
  const std::string& serviceName, const std::string& conversationId,
  const std::string& withdrawalOperationId,
  std::uint64_t registrationGeneration)
{
  if (!isCanonicalScopeName(authenticatedRequester) || !isCanonicalScopeName(serviceName) ||
      !isConversationScopeId(conversationId) ||
      withdrawalOperationId.empty() || withdrawalOperationId.size() > 256 ||
      registrationGeneration == 0)
    throw std::invalid_argument("conversation-withdrawal-borrow-scope-invalid");
  if (!parent.m_token || parent.m_token->owner.lock().get() != this)
    return std::nullopt;
  auto owner = weak_from_this().lock();
  if (!owner) throw std::runtime_error("conversation-state-owner-required");
  auto token = std::make_shared<SaveFenceToken>();
  token->owner = owner;
  token->scope = {SaveScopeKind::Conversation, authenticatedRequester,
                  serviceName, conversationId};
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_closing || m_saveFences.size() >= controlCapacity(true) ||
      !saveFenceMatchesLocked(parent)) return std::nullopt;
  const auto& parentScope = parent.m_token->scope;
  if (parentScope.kind == SaveScopeKind::Conversation ||
      (parentScope.kind == SaveScopeKind::Service && parentScope.service != serviceName) ||
      parentScope.withdrawalOperationId != withdrawalOperationId ||
      parentScope.registrationGeneration != registrationGeneration)
    return std::nullopt;
  token->scope.generation = m_admissionGeneration;
  token->scope.withdrawalOperationId = withdrawalOperationId;
  token->scope.registrationGeneration = registrationGeneration;
  for (const auto& item : m_saveFences) {
    bool ancestor = false;
    for (auto current = parent.m_token; current; current = current->parent) {
      if (item.first == current->id) {
        ancestor = true;
        break;
      }
    }
    if (!ancestor && saveScopesOverlap(item.second, token->scope))
      return std::nullopt;
  }
  if (m_nextSaveFenceId == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("conversation-save-fence-id-exhausted");
  token->id = m_nextSaveFenceId;
  const auto inserted = m_saveFences.emplace(token->id, token->scope);
  if (!inserted.second) throw std::logic_error("conversation-save-fence-id-collision");
  ++m_nextSaveFenceId;
  token->parent = parent.m_token;
  token->armed = true;
  return SaveFence(std::move(token));
}

bool
ConversationStateStore::saveFenceMatchesLocked(const SaveFence& fence) const noexcept
{
  if (!fence.m_token || !fence.m_token->armed) return false;
  const auto& token = *fence.m_token;
  const auto found = m_saveFences.find(token.id);
  return found != m_saveFences.end() &&
    found->second.generation == m_admissionGeneration &&
    token.scope.generation == m_admissionGeneration &&
    found->second.kind == token.scope.kind &&
    found->second.requester == token.scope.requester &&
    found->second.service == token.scope.service &&
    found->second.conversation == token.scope.conversation &&
    found->second.withdrawalOperationId == token.scope.withdrawalOperationId &&
    found->second.registrationGeneration == token.scope.registrationGeneration;
}

bool
ConversationStateStore::isSaveFenceQuiescent(const SaveFence& fence) const noexcept
{
  try {
    if (!fence.m_token || !fence.m_token->armed) return false;
    const auto owner = fence.m_token->owner.lock();
    if (owner.get() != this) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!saveFenceMatchesLocked(fence)) return false;
    const auto found = m_saveFences.find(fence.m_token->id);
    return std::none_of(m_executionAdmissions.begin(), m_executionAdmissions.end(),
      [&] (const auto& item) { return saveScopeBlocksAdmission(found->second, item.second); });
  }
  catch (...) { return false; }
}

bool
ConversationStateStore::setProviderBinding(std::string providerIdentity,
                                            std::string providerBootId,
                                            std::uint64_t cacheEpoch)
{
  PendingProviderBinding next{std::move(providerIdentity), std::move(providerBootId), cacheEpoch};
  bool changed;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_providerIdentity != next.identity || m_providerBootId != next.bootId ||
        m_cacheEpoch != next.epoch) ++m_admissionGeneration;
    changed = !m_providerBootId.empty() &&
      (m_providerIdentity != next.identity || m_providerBootId != next.bootId ||
       m_cacheEpoch != next.epoch);
    if (changed || (m_closing && !m_preserveCommittedOnImportFailure)) {
      m_preserveCommittedOnImportFailure = false;
      m_pendingProviderBinding = std::move(next);
      m_closing = true;
    }
    else {
      m_providerIdentity = std::move(next.identity);
      m_providerBootId = std::move(next.bootId);
      m_cacheEpoch = next.epoch;
    }
  }
  drainDeferredState();
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
         left.importedOriginIdentity == right.importedOriginIdentity &&
         (left.checkpointDigest.empty() || right.checkpointDigest.empty() ||
          left.checkpointDigest == right.checkpointDigest) &&
         left.expiresAtMs == right.expiresAtMs &&
         sameConversationIdentity(left.identity, right.identity);
}

bool
ConversationStateStore::retainsStagedBinding(const ConversationStateBinding& binding) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(keyFor(binding));
  return !m_closing && found != m_entries.end() &&
    found->second.lifecycle == ConversationStateLifecycle::COMMITTING &&
    found->second.binding == binding && found->second.binding.identity == binding.identity;
}

bool
ConversationStateStore::removeEntry(const std::string& key,
                                    bool countCleanup,
                                    std::unique_lock<std::mutex>& lock,
                                    bool rollback)
{
  const auto found = m_entries.find(key);
  if (found == m_entries.end()) return false;
  auto& entry = found->second;
  const bool administrative = entry.lifecycle == ConversationStateLifecycle::COMMITTING &&
                              entry.pinCount == 1;
  if (entry.operationActive || entry.cleanupFailed || entry.snapshotOwned ||
      entry.useOwned || entry.activePrefetches != 0 ||
      (entry.pinCount != 0 && !administrative)) return false;
  // Copy potentially allocating identity/handle fields before admission changes.
  auto state = entry.adapterState;
  auto runner = entry.adapterRunner;
  entry.operationActive = true;
  entry.evictionRequested = !countCleanup;
  entry.rollbackRequested = rollback;
  ++m_activeTransitions;
  lock.unlock();
  bool released = true;
  try {
    if (state && runner) released = runner->releaseConversationState(*state);
  }
  catch (...) { released = false; }
  lock.lock();
  // The operation fence prevents erasure/replacement while the mutex is open.
  if (released) {
    auto bytes = std::move(entry.state);
    lock.unlock();
    wipeTensorBundle(bytes);
    lock.lock();
    if (entry.residency == ConversationStateResidency::GPU_RESIDENT)
      m_gpuBytes -= std::min(m_gpuBytes, entry.allocatedBytes);
    else if (entry.residency == ConversationStateResidency::HOST_RESIDENT)
      m_hostBytes -= std::min(m_hostBytes, entry.allocatedBytes);
    if (countCleanup) ++m_cleanups;
    else ++m_evictions;
    if (entry.rollbackRequested) ++m_promotionRollbacks;
    m_entries.erase(found);
  }
  else {
    entry.operationActive = false;
    entry.cleanupFailed = true;
    m_closing = true;
  }
  --m_activeTransitions;
  lock.unlock();
  runner.reset();
  // Normal drain skips failed entries. Only explicit retry repeats a failure.
  drainDeferredState();
  lock.lock();
  return released;
}

bool
ConversationStateStore::evictGpuUntilFits(std::size_t incomingBytes,
                                          const std::string& replacingKey,
                                          std::unique_lock<std::mutex>& lock)
{
  const auto admission = m_admissionGeneration;
  if (incomingBytes > m_gpuMaxBytes) return false;
  for (;;) {
    if (m_closing || admission != m_admissionGeneration) return false;
    const auto entries = static_cast<std::size_t>(std::count_if(
      m_entries.begin(), m_entries.end(), [] (const auto& item) {
        return item.second.residency == ConversationStateResidency::GPU_RESIDENT ||
               item.second.prefetchGpuReserved;
      }));
    if (m_gpuBytes <= m_gpuMaxBytes && incomingBytes <= m_gpuMaxBytes - m_gpuBytes &&
        entries < m_gpuMaxEntries) return true;
    auto victim = m_entries.end();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
      const auto& entry = it->second;
      if (it->first == replacingKey || entry.operationActive || entry.cleanupFailed ||
          entry.residency != ConversationStateResidency::GPU_RESIDENT ||
          entry.lifecycle != ConversationStateLifecycle::IDLE ||
          entry.pinCount != 0 || entry.activePrefetches != 0) continue;
      if (victim == m_entries.end() || entry.lastAccess < victim->second.lastAccess)
        victim = it;
    }
    if (victim == m_entries.end()) return false;
    const auto key = victim->first;
    if (!removeEntry(key, false, lock)) return false;
  }
}

bool
ConversationStateStore::evictHostUntilFits(std::size_t incomingBytes,
                                           const std::string& replacingKey,
                                           std::unique_lock<std::mutex>& lock)
{
  const auto admission = m_admissionGeneration;
  if (incomingBytes > m_hostMaxBytes) return false;
  for (;;) {
    if (m_closing || admission != m_admissionGeneration) return false;
    const auto entries = static_cast<std::size_t>(std::count_if(
      m_entries.begin(), m_entries.end(), [] (const auto& item) {
        return item.second.residency == ConversationStateResidency::HOST_RESIDENT ||
               item.second.hostReserved;
      }));
    if (m_hostBytes <= m_hostMaxBytes && incomingBytes <= m_hostMaxBytes - m_hostBytes &&
        entries < m_hostMaxEntries) return true;
    auto victim = m_entries.end();
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
      const auto& entry = it->second;
      if (it->first == replacingKey || entry.operationActive || entry.cleanupFailed ||
          entry.residency != ConversationStateResidency::HOST_RESIDENT ||
          entry.lifecycle != ConversationStateLifecycle::IDLE ||
          entry.pinCount != 0 || entry.activePrefetches != 0) continue;
      if (victim == m_entries.end() || entry.lastAccess < victim->second.lastAccess)
        victim = it;
    }
    if (victim == m_entries.end()) return false;
    const auto key = victim->first;
    if (!removeEntry(key, false, lock)) return false;
  }
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
                                        std::uint64_t nowMs,
                                        std::optional<ConversationSaveMetadataV1> saveMetadata,
                                        const ExecutionAdmission* admission)
{
  binding.validate();
  cleanupExpired(nowMs);
  if (originRequestId.empty() || role.empty() || role != binding.identity.roleName ||
      originRequestId != binding.identity.requestId || state.payload.empty() ||
      nowMs >= binding.expiresAtMs ||
      (saveMetadata && !saveMetadataMatches(*saveMetadata, binding))) {
    return false;
  }
  const auto key = keyFor(binding);
  std::unique_lock<std::mutex> lock(m_mutex);
  const auto requester = saveMetadata ? saveMetadata->authenticatedRequester : std::string{};
  // One successor epoch has exactly one winner. Never replace an already
  // committed entry or another child's in-flight staged candidate.
  if (m_closing || (!m_providerIdentity.empty() &&
      (binding.identity.providerIdentity != m_providerIdentity ||
       binding.identity.providerBootId != m_providerBootId ||
       binding.identity.cacheEpoch != m_cacheEpoch)) || m_entries.find(key) != m_entries.end() ||
      (admission ? !executionAdmissionMatchesLocked(admission, binding, requester,
                                                    saveMetadata)
                 : isEntryFencedLocked(binding, requester))) {
    return false;
  }
  const auto bytes = state.payload.size();
  if (!evictGpuUntilFits(bytes, key, lock)) {
    return false;
  }
  if (admission ? !executionAdmissionMatchesLocked(admission, binding, requester,
                                                   saveMetadata)
                : isEntryFencedLocked(binding, requester)) return false;
  Entry entry;
  entry.binding = std::move(binding);
  entry.state = std::move(state);
  entry.residency = ConversationStateResidency::GPU_RESIDENT;
  entry.lifecycle = ConversationStateLifecycle::COMMITTING;
  entry.logicalBytes = bytes;
  entry.allocatedBytes = bytes;
  entry.pinCount = 1;
  entry.authenticatedRequester = requester;
  entry.saveMetadata = std::move(saveMetadata);
  entry.executionAdmissionId = admission ? admission->m_token->id : 0;
  entry.lastAccess = ++m_accessSequence;
  entry.lastUsedAtMs = nowMs;
  entry.expiresAtMs = std::min(entry.binding.expiresAtMs,
                             conversationRetentionDeadline(nowMs, m_retentionMs));
  const auto inserted = m_entries.emplace(key, std::move(entry));
  if (!inserted.second) return false;
  m_gpuBytes += inserted.first->second.allocatedBytes;
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
  std::uint64_t nowMs,
  std::optional<ConversationSaveMetadataV1> saveMetadata,
  const ExecutionAdmission* admission)
{
  binding.validate();
  state.validate();
  if (!runner || originRequestId.empty() || role.empty() ||
      role != binding.identity.roleName || role != state.opaque.role ||
      originRequestId != binding.identity.requestId ||
      nowMs >= binding.expiresAtMs ||
      state.opaque.providerIdentity != binding.identity.providerIdentity ||
      state.opaque.providerBootId != binding.identity.providerBootId ||
      (saveMetadata && !saveMetadataMatches(*saveMetadata, binding))) {
    return false;
  }
  const auto key = keyFor(binding);
  std::unique_lock<std::mutex> lock(m_mutex);
  const auto requester = saveMetadata ? saveMetadata->authenticatedRequester : std::string{};
  if (m_closing || (!m_providerIdentity.empty() &&
      (binding.identity.providerIdentity != m_providerIdentity ||
       binding.identity.providerBootId != m_providerBootId ||
       binding.identity.cacheEpoch != m_cacheEpoch)) || m_entries.find(key) != m_entries.end() ||
      (admission ? !executionAdmissionMatchesLocked(admission, binding, requester,
                                                    saveMetadata)
                 : isEntryFencedLocked(binding, requester)) ||
      !evictGpuUntilFits(state.logicalBytes, key, lock)) {
    return false;
  }
  if (admission ? !executionAdmissionMatchesLocked(admission, binding, requester,
                                                   saveMetadata)
                : isEntryFencedLocked(binding, requester)) return false;
  Entry entry;
  entry.binding = std::move(binding);
  entry.adapterState = std::move(state);
  entry.adapterRunner = std::move(runner);
  entry.residency = ConversationStateResidency::GPU_RESIDENT;
  entry.lifecycle = ConversationStateLifecycle::COMMITTING;
  entry.logicalBytes = entry.adapterState->logicalBytes;
  entry.allocatedBytes = entry.logicalBytes;
  entry.pinCount = 1;
  entry.authenticatedRequester = requester;
  entry.saveMetadata = std::move(saveMetadata);
  entry.executionAdmissionId = admission ? admission->m_token->id : 0;
  entry.lastAccess = ++m_accessSequence;
  entry.lastUsedAtMs = nowMs;
  entry.expiresAtMs = std::min(entry.binding.expiresAtMs,
                             conversationRetentionDeadline(nowMs, m_retentionMs));
  const auto inserted = m_entries.emplace(key, std::move(entry));
  if (!inserted.second) return false;
  m_gpuBytes += inserted.first->second.allocatedBytes;
  ++m_stagedPromotions;
  return true;
}

struct ConversationStateStore::ImportReservationToken
{
  std::shared_ptr<ConversationStateStore> owner;
  ConversationStateBinding binding;
  std::string key;
  std::uint64_t generation = 0;
  bool armed = false;
  ~ImportReservationToken() { if (armed) owner->cancelImportedReservation(*this); }
};

std::shared_ptr<ConversationStateStore::ImportReservationToken>
ConversationStateStore::reserveImportedState(
  ConversationStateBinding binding,
  std::shared_ptr<NativeModelRunner> runner,
  std::size_t bytes,
  std::uint64_t nowMs,
  const std::string& authenticatedRequester,
  const ExecutionAdmission* admission)
{
  binding.validate();
  if (!runner || !binding.checkpointDigest.empty() || bytes == 0 || nowMs >= binding.expiresAtMs)
    return {};
  if (!authenticatedRequester.empty() && !isCanonicalScopeName(authenticatedRequester)) return {};
  auto token = std::make_shared<ImportReservationToken>();
  token->owner = weak_from_this().lock();
  if (!token->owner) throw std::runtime_error("conversation-state-owner-required");
  token->binding = binding;
  token->key = keyFor(binding);
  Entry entry;
  entry.binding = std::move(binding);
  entry.authenticatedRequester = authenticatedRequester;
  entry.adapterRunner = std::move(runner);
  entry.logicalBytes = bytes; entry.allocatedBytes = bytes;
  entry.lifecycle = ConversationStateLifecycle::COMMITTING;
  entry.pinCount = 1; entry.importedCandidate = true;
  entry.importReserved = true; entry.operationActive = true;
  entry.importAdmission = token;
  entry.lastUsedAtMs = nowMs;
  entry.expiresAtMs = std::min(entry.binding.expiresAtMs, conversationRetentionDeadline(nowMs, m_retentionMs));
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_closing || m_providerIdentity.empty() ||
      entry.binding.identity.providerIdentity != m_providerIdentity ||
      entry.binding.identity.providerBootId != m_providerBootId ||
      entry.binding.identity.cacheEpoch != m_cacheEpoch ||
      (admission ? !executionAdmissionMatchesLocked(
                     admission, entry.binding, authenticatedRequester, std::nullopt)
                 : isEntryFencedLocked(entry.binding, authenticatedRequester)) ||
      m_entries.count(token->key) || m_gpuBytes > m_gpuMaxBytes ||
      bytes > m_gpuMaxBytes - m_gpuBytes || m_activeTransitions == std::numeric_limits<std::uint64_t>::max())
  {
    rejectConversationImport("reserve", m_closing ? "store-closing" :
      m_providerIdentity.empty() ? "provider-unbound" :
      entry.binding.identity.providerIdentity != m_providerIdentity ? "provider-mismatch" :
      entry.binding.identity.providerBootId != m_providerBootId ? "boot-mismatch" :
      entry.binding.identity.cacheEpoch != m_cacheEpoch ? "cache-epoch-mismatch" :
      m_entries.count(token->key) ? "destination-exists" :
      m_gpuBytes > m_gpuMaxBytes || bytes > m_gpuMaxBytes - m_gpuBytes ? "byte-capacity" :
      m_activeTransitions == std::numeric_limits<std::uint64_t>::max() ? "transition-capacity" :
      "execution-admission", entry.binding);
    return {};
  }
  const auto entries = static_cast<std::size_t>(std::count_if(m_entries.begin(), m_entries.end(),
    [](const auto& item) { return item.second.residency == ConversationStateResidency::GPU_RESIDENT ||
      item.second.lifecycle == ConversationStateLifecycle::PREFETCHING; }));
  if (entries >= m_gpuMaxEntries) {
    rejectConversationImport("reserve", "entry-capacity", entry.binding);
    return {};
  }
  entry.executionAdmissionId = admission ? admission->m_token->id : 0;
  token->generation = m_admissionGeneration;
  const auto inserted = m_entries.emplace(token->key, std::move(entry));
  if (!inserted.second) return {};
  inserted.first->second.lastAccess = ++m_accessSequence;
  m_gpuBytes += bytes;
  ++m_activeTransitions;
  ++m_stagedPromotions;
  token->armed = true;
  return token;
}

bool
ConversationStateStore::completeImportedState(
  const std::shared_ptr<ImportReservationToken>& token,
  NativeConversationStateHandleV1 state,
  const NativePortableKvLimitsV1& limits)
{
  static_assert(std::is_nothrow_move_constructible_v<NativeConversationStateHandleV1>);
  std::unique_lock<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(token->key);
  if (token->owner.get() != this || !token->armed || found == m_entries.end() ||
      found->second.importAdmission.lock() != token || !found->second.importReserved ||
      !(found->second.binding.identity == token->binding.identity))
    throw std::logic_error("portable-kv-reservation-owner-lost");
  auto& entry = found->second;
  // Own the returned allocation before validation/cancellation can throw.
  entry.adapterState.emplace(std::move(state));
  entry.adapterCleanupUnsafe = true;
  const auto& actual = *entry.adapterState;
  entry.importOccupancyUnverified = actual.logicalBytes != entry.logicalBytes;
  actual.validate();
  if (actual.conversationKey != token->key ||
      actual.opaque.providerIdentity != entry.binding.identity.providerIdentity ||
      actual.opaque.providerBootId != entry.binding.identity.providerBootId ||
      actual.opaque.role != entry.binding.identity.roleName ||
      actual.opaque.sessionId != entry.binding.identity.requestId ||
      actual.opaque.stateInferenceEpoch != entry.binding.identity.stateInferenceEpoch)
    return rejectConversationImport("complete", "adapter-identity", entry.binding);
  entry.adapterCleanupUnsafe = false;
  if (entry.importOccupancyUnverified || m_closing || token->generation != m_admissionGeneration)
    return rejectConversationImport("complete", entry.importOccupancyUnverified ?
      "adapter-occupancy" : m_closing ? "store-closing" : "admission-generation", entry.binding);
  lock.unlock();
  const bool cancelled = limits.cancelled && limits.cancelled();
  lock.lock();
  if (cancelled || m_closing || token->generation != m_admissionGeneration)
    return rejectConversationImport("complete", cancelled ? "cancelled" :
      m_closing ? "store-closing" : "admission-generation", entry.binding);
  entry.importReserved = false;
  entry.operationActive = false;
  --m_activeTransitions;
  token->armed = false;
  return true;
}

void
ConversationStateStore::cancelImportedReservation(ImportReservationToken& token) noexcept
{
  try {
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto found = m_entries.find(token.key);
    if (!token.armed || found == m_entries.end() || !found->second.importReserved) return;
    auto& entry = found->second;
    token.armed = false;
    entry.operationActive = false;
    entry.importReserved = false;
    --m_activeTransitions;
    entry.rollbackRequested = true;
    // An import cleanup failure closes admission without sacrificing an
    // earlier committed winner. Explicit clear/rebind still requests a drain.
    if (!m_closing) m_preserveCommittedOnImportFailure = true;
    if (entry.adapterCleanupUnsafe) {
      entry.cleanupFailed = true;
      m_closing = true;
    }
    else {
      try {
        if (!removeEntry(token.key, true, lock, true)) {
          const auto retained = m_entries.find(token.key);
          if (retained != m_entries.end()) retained->second.cleanupFailed = true;
          m_closing = true;
        }
      }
      catch (...) {
        // removeEntry's allocating handle copy can fail before its own fence.
        // The reserved resource and charge stay owned, closed and retryable.
        entry.cleanupFailed = true;
        m_closing = true;
      }
    }
    if (!m_closing) m_preserveCommittedOnImportFailure = false;
    lock.unlock();
    drainDeferredState();
  }
  catch (...) { /* A destructor cannot propagate a mutex/platform failure. */ }
}

ImportedStateReleaseResult
ConversationStateStore::releaseImportedState(
  const std::shared_ptr<ImportReservationToken>& token)
{
  if (!token) return ImportedStateReleaseResult::AlreadyGone;
  if (token->owner.get() != this)
    throw std::invalid_argument("import ownership belongs to another Store");
  std::unique_lock<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(token->key);
  if (found == m_entries.end() || found->second.importAdmission.lock() != token)
    return ImportedStateReleaseResult::AlreadyGone;
  auto& entry = found->second;
  if (!compatible(entry.binding, token->binding) ||
      !(entry.binding.identity == token->binding.identity))
    throw std::logic_error("import ownership binding changed");
  if (entry.cleanupFailed) return ImportedStateReleaseResult::CleanupUnconfirmed;
  const bool administrative = entry.importedCandidate &&
    entry.lifecycle == ConversationStateLifecycle::COMMITTING && entry.pinCount == 1;
  if (token->armed || entry.importReserved || entry.operationActive || entry.snapshotOwned ||
      entry.useOwned || entry.activePrefetches || (entry.pinCount && !administrative) ||
      (entry.lifecycle != ConversationStateLifecycle::IDLE && !administrative))
    return ImportedStateReleaseResult::Busy;
  if (!m_closing) m_preserveCommittedOnImportFailure = true;
  bool released = false;
  try { released = removeEntry(token->key, true, lock, administrative); }
  catch (...) {
    const auto retained = m_entries.find(token->key);
    if (retained != m_entries.end() && retained->second.importAdmission.lock() == token) {
      retained->second.cleanupFailed = true;
      m_closing = true;
    }
  }
  if (!m_closing) m_preserveCommittedOnImportFailure = false;
  return released ? ImportedStateReleaseResult::Released
                  : ImportedStateReleaseResult::CleanupUnconfirmed;
}

bool
ConversationStateStore::stageImportedAdapterState(
  std::string originRequestId,
  std::string role,
  ConversationStateBinding binding,
  NativeConversationStateHandleV1 state,
  std::shared_ptr<NativeModelRunner> runner,
  std::uint64_t nowMs,
  const std::string& authenticatedRequester,
  const ExecutionAdmission* admission)
{
  binding.validate();
  state.validate();
  if (!authenticatedRequester.empty() && !isCanonicalScopeName(authenticatedRequester))
    return false;
  if (!runner || originRequestId.empty() || role.empty() ||
      !binding.checkpointDigest.empty() || state.logicalBytes == 0 ||
      originRequestId != binding.identity.requestId ||
      role != binding.identity.roleName || role != state.opaque.role ||
      state.opaque.providerIdentity != binding.identity.providerIdentity ||
      state.opaque.providerBootId != binding.identity.providerBootId ||
      state.opaque.sessionId != binding.identity.requestId ||
      state.opaque.stateInferenceEpoch != binding.identity.stateInferenceEpoch ||
      nowMs >= binding.expiresAtMs) {
    return false;
  }
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  // Import requires the exact destination incarnation, even in a standalone
  // store. Capacity rejection must preserve every existing entry.
  if (m_closing || m_providerIdentity.empty() ||
      binding.identity.providerIdentity != m_providerIdentity ||
      binding.identity.providerBootId != m_providerBootId ||
      binding.identity.cacheEpoch != m_cacheEpoch ||
      (admission ? !executionAdmissionMatchesLocked(
                     admission, binding, authenticatedRequester, std::nullopt)
                 : isEntryFencedLocked(binding, authenticatedRequester)) ||
      m_entries.find(key) != m_entries.end() ||
      m_gpuBytes > m_gpuMaxBytes ||
      state.logicalBytes > m_gpuMaxBytes - m_gpuBytes) {
    return false;
  }
  const auto gpuEntries = static_cast<std::size_t>(std::count_if(
    m_entries.begin(), m_entries.end(), [](const auto& item) {
      return item.second.residency == ConversationStateResidency::GPU_RESIDENT ||
             item.second.lifecycle == ConversationStateLifecycle::PREFETCHING;
    }));
  if (gpuEntries >= m_gpuMaxEntries) {
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
  entry.importedCandidate = true;
  entry.authenticatedRequester = authenticatedRequester;
  entry.executionAdmissionId = admission ? admission->m_token->id : 0;
  entry.lastUsedAtMs = nowMs;
  entry.expiresAtMs = std::min(entry.binding.expiresAtMs,
                             conversationRetentionDeadline(nowMs, m_retentionMs));
  const auto inserted = m_entries.emplace(key, std::move(entry));
  if (!inserted.second) {
    return false;
  }
  // All potentially throwing construction/insertion precedes accounting.
  inserted.first->second.lastAccess = ++m_accessSequence;
  m_gpuBytes += inserted.first->second.allocatedBytes;
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
  if (found == m_entries.end() || found->second.operationActive || found->second.cleanupFailed ||
      !compatible(found->second.binding, binding) ||
      found->second.lifecycle != ConversationStateLifecycle::COMMITTING ||
      found->second.importedCandidate || found->second.pinCount != 1) {
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
  return commitStagedPromotionImpl(binding, checkpointDigest, {});
}

bool
ConversationStateStore::commitStagedPromotionImpl(
  const ConversationStateBinding& binding, const std::string& checkpointDigest,
  const std::shared_ptr<ImportReservationToken>& requiredAdmission)
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
  if (found == m_entries.end() || found->second.operationActive || found->second.cleanupFailed ||
      (requiredAdmission && (m_closing || requiredAdmission->armed ||
       requiredAdmission->owner.get() != this || found->second.importReserved ||
       found->second.importAdmission.lock() != requiredAdmission ||
       !(found->second.binding.identity == requiredAdmission->binding.identity))) ||
      found->second.binding.conversationId != binding.conversationId ||
      found->second.binding.contextEpoch != binding.contextEpoch ||
      found->second.binding.serviceName != binding.serviceName ||
      found->second.binding.planRoleMapDigest != binding.planRoleMapDigest ||
      found->second.binding.receiptDigest != binding.receiptDigest ||
      found->second.binding.importedOriginIdentity != binding.importedOriginIdentity ||
      found->second.binding.expiresAtMs != binding.expiresAtMs ||
      !sameConversationIdentity(found->second.binding.identity, binding.identity) ||
      found->second.lifecycle != ConversationStateLifecycle::COMMITTING ||
      found->second.pinCount != 1 ||
      !found->second.binding.checkpointDigest.empty()) {
    if (requiredAdmission) {
      rejectConversationImport("commit", found == m_entries.end() ? "entry-missing" :
        found->second.operationActive ? "operation-active" :
        found->second.cleanupFailed ? "cleanup-failed" :
        m_closing ? "store-closing" :
        requiredAdmission->armed || found->second.importReserved ? "import-incomplete" :
        requiredAdmission->owner.get() != this ||
          found->second.importAdmission.lock() != requiredAdmission ? "ownership-mismatch" :
        found->second.lifecycle != ConversationStateLifecycle::COMMITTING ? "lifecycle" :
        found->second.pinCount != 1 ? "pin-count" :
        !found->second.binding.checkpointDigest.empty() ? "checkpoint-already-bound" :
        "binding-mismatch", binding);
    }
    return false;
  }
  found->second.binding.checkpointDigest = checkpointDigest;
  found->second.lifecycle = ConversationStateLifecycle::IDLE;
  found->second.pinCount = 0;
  ++m_promotions;
  return true;
}

bool
ConversationStateStore::finalizeSaveEligibility(const ConversationStateBinding& binding)
{
  try {
    binding.validate();
    decode_state_identity_detail::requireDigest(
      binding.checkpointDigest, "conversation save checkpoint");
  }
  catch (const std::exception&) {
    return false;
  }
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end()) return false;
  auto& entry = found->second;
  if (entry.saveEligible) return true;
  if (!entry.saveMetadata || entry.authenticatedRequester !=
      entry.saveMetadata->authenticatedRequester || entry.importedCandidate || entry.operationActive ||
      entry.cleanupFailed || entry.lifecycle != ConversationStateLifecycle::IDLE ||
      entry.pinCount != 0 || entry.binding.checkpointDigest != binding.checkpointDigest ||
      !compatible(entry.binding, binding) ||
      !sameConversationIdentity(entry.binding.identity, binding.identity) ||
      (m_providerIdentity.empty() ||
       entry.binding.identity.providerIdentity != m_providerIdentity ||
       entry.binding.identity.providerBootId != m_providerBootId ||
       entry.binding.identity.cacheEpoch != m_cacheEpoch) ||
      entry.executionAdmissionId == 0 ||
      m_executionAdmissions.find(entry.executionAdmissionId) == m_executionAdmissions.end() ||
      m_executionAdmissions.at(entry.executionAdmissionId).generation != m_admissionGeneration ||
      m_executionAdmissions.at(entry.executionAdmissionId).service != entry.binding.serviceName ||
      m_executionAdmissions.at(entry.executionAdmissionId).conversation != entry.binding.conversationId ||
      m_executionAdmissions.at(entry.executionAdmissionId).requester !=
        entry.saveMetadata->authenticatedRequester ||
      !saveMetadataMatches(*entry.saveMetadata, entry.binding))
    return false;
  entry.saveEligible = true;
  return true;
}

bool
ConversationStateStore::rollbackStagedPromotion(
  const ConversationStateBinding& binding)
{
  binding.validate();
  const auto key = keyFor(binding);
  std::unique_lock<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() || found->second.operationActive ||
      !compatible(found->second.binding, binding) ||
      found->second.lifecycle != ConversationStateLifecycle::COMMITTING ||
      found->second.pinCount != 1) {
    return false;
  }
  return removeEntry(key, true, lock, true);
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
  std::unique_lock<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (m_closing || found == m_entries.end() || found->second.operationActive || found->second.snapshotOwned || found->second.useOwned || found->second.activePrefetches != 0 || found->second.adapterState ||
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
    removeEntry(key, true, lock);
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
  if (m_closing || found == m_entries.end() || found->second.operationActive || found->second.snapshotOwned || found->second.useOwned || found->second.activePrefetches != 0 || !found->second.adapterState ||
      !compatible(found->second.binding, binding) ||
      found->second.lifecycle == ConversationStateLifecycle::COMMITTING ||
      nowMs >= found->second.expiresAtMs) {
    return std::nullopt;
  }
  return found->second.adapterState;
}

std::shared_ptr<NativeModelRunner>
ConversationStateStore::adapterRunner(
  const ConversationStateBinding& binding,
  std::uint64_t nowMs) const
{
  binding.validate();
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (m_closing || found == m_entries.end() || found->second.operationActive || found->second.snapshotOwned || found->second.useOwned || found->second.activePrefetches != 0 || !found->second.adapterState ||
      !found->second.adapterRunner ||
      !compatible(found->second.binding, binding) ||
      found->second.lifecycle == ConversationStateLifecycle::COMMITTING ||
      nowMs >= found->second.expiresAtMs) {
    return {};
  }
  return found->second.adapterRunner;
}

std::optional<ConversationStateBinding>
ConversationStateStore::resolve(const ConversationStateReferenceV1& reference,
                                std::uint64_t nowMs, const std::string& authenticatedRequester)
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
  if (m_closing || found == m_entries.end() || found->second.operationActive || found->second.snapshotOwned || found->second.useOwned || found->second.activePrefetches != 0 || nowMs >= found->second.expiresAtMs ||
      nowMs >= reference.expiresAtMs ||
      found->second.lifecycle == ConversationStateLifecycle::COMMITTING ||
      found->second.binding.serviceName != reference.serviceName ||
      found->second.binding.planRoleMapDigest != reference.planRoleMapDigest ||
      found->second.binding.checkpointDigest != reference.checkpointDigest ||
      found->second.binding.receiptDigest != reference.roleReceiptDigest ||
      !requesterMatchesLocked(found->second, authenticatedRequester) ||
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
  std::uint64_t nowMs, const std::string& authenticatedRequester)
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
  if (m_closing || found == m_entries.end() || found->second.operationActive || found->second.snapshotOwned || found->second.useOwned || found->second.activePrefetches != 0 ||
      found->second.lifecycle != ConversationStateLifecycle::COMMITTING ||
      found->second.pinCount != 1 || nowMs >= found->second.expiresAtMs ||
      nowMs >= reference.expiresAtMs ||
      found->second.binding.serviceName != reference.serviceName ||
      found->second.binding.planRoleMapDigest != reference.planRoleMapDigest ||
      found->second.binding.receiptDigest != reference.roleReceiptDigest ||
      !requesterMatchesLocked(found->second, authenticatedRequester) ||
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
  if (m_closing || found == m_entries.end() || found->second.operationActive || found->second.snapshotOwned || found->second.useOwned || found->second.activePrefetches != 0 || !compatible(found->second.binding, binding) ||
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
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_entries.find(keyFor(binding));
    if (found == m_entries.end() || found->second.operationActive || !compatible(found->second.binding, binding) ||
        found->second.snapshotOwned || found->second.useOwned ||
        found->second.lifecycle == ConversationStateLifecycle::COMMITTING ||
        found->second.pinCount == 0) return false;
    --found->second.pinCount;
    if (found->second.pinCount == 0) found->second.lifecycle = ConversationStateLifecycle::IDLE;
  }
  drainDeferredState();
  return true;
}

bool
ConversationStateStore::pauseToHost(const ConversationStateBinding& binding,
                                     std::uint64_t nowMs)
{
  binding.validate();
  const auto key = keyFor(binding);
  std::unique_lock<std::mutex> lock(m_mutex);
  const auto eligible = [&](const auto& found) {
    return !m_closing && found != m_entries.end() && !found->second.operationActive &&
      !found->second.cleanupFailed && !found->second.snapshotOwned && !found->second.useOwned &&
      found->second.activePrefetches == 0 && compatible(found->second.binding, binding) &&
      found->second.residency == ConversationStateResidency::GPU_RESIDENT &&
      found->second.lifecycle == ConversationStateLifecycle::IDLE &&
      found->second.pinCount == 0 && nowMs < found->second.expiresAtMs;
  };
  auto found = m_entries.find(key);
  if (!eligible(found)) return false;
  const auto admission = m_admissionGeneration;
  const auto reservedBytes = found->second.allocatedBytes;
  if (!evictHostUntilFits(reservedBytes, key, lock)) return false;
  found = m_entries.find(key);
  if (admission != m_admissionGeneration || !eligible(found) ||
      found->second.allocatedBytes > reservedBytes) return false;
  auto state = found->second.adapterState;
  auto runner = found->second.adapterRunner;
  const auto bytes = found->second.allocatedBytes;
  found->second.operationActive = true;
  found->second.hostReserved = true;
  m_hostBytes += bytes;
  ++m_activeTransitions;
  lock.unlock();
  bool transferred = true;
  try {
    if (state && runner) transferred = runner->pauseConversationStateToHost(*state);
  }
  catch (...) { transferred = false; }
  lock.lock();
  found = m_entries.find(key); // The transition fence retains the exact entry.
  if (transferred) {
    m_gpuBytes -= std::min(m_gpuBytes, bytes);
    found->second.residency = ConversationStateResidency::HOST_RESIDENT;
    found->second.lastAccess = ++m_accessSequence;
    found->second.lastUsedAtMs = nowMs;
  }
  else {
    m_hostBytes -= std::min(m_hostBytes, bytes);
  }
  found->second.hostReserved = false;
  found->second.operationActive = false;
  --m_activeTransitions;
  const bool admitted = transferred && !m_closing && admission == m_admissionGeneration;
  lock.unlock();
  runner.reset();
  drainDeferredState();
  return admitted;
}

struct ConversationStateStore::PrefetchToken
{
  std::shared_ptr<ConversationStateStore> store;
  std::shared_ptr<NativeModelRunner> runner;
  std::string key;
  bool armed = false;
  ~PrefetchToken()
  {
    if (!armed) return;
    {
      std::lock_guard<std::mutex> lock(store->m_mutex);
      const auto found = store->m_entries.find(key);
      if (found != store->m_entries.end()) {
        if (found->second.activePrefetches != 0) --found->second.activePrefetches;
        if (found->second.prefetchGpuReserved) {
          store->m_gpuBytes -= std::min(store->m_gpuBytes, found->second.allocatedBytes);
          found->second.prefetchGpuReserved = false;
        }
        if (found->second.lifecycle == ConversationStateLifecycle::PREFETCHING)
          found->second.lifecycle = ConversationStateLifecycle::IDLE;
      }
    }
    store->drainDeferredState();
  }
};

std::future<bool>
ConversationStateStore::prefetchToGpu(const ConversationStateBinding& binding,
                                     std::uint64_t nowMs)
{
  binding.validate();
  const auto key = keyFor(binding);
  std::uint64_t generation = 0;
  std::optional<NativeConversationStateHandleV1> adapterState;
  auto token = std::make_shared<PrefetchToken>();
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    auto found = m_entries.find(key);
    if (m_closing || found == m_entries.end() || found->second.operationActive || !compatible(found->second.binding, binding) ||
        found->second.snapshotOwned || found->second.useOwned || found->second.cleanupFailed ||
        found->second.lifecycle != ConversationStateLifecycle::IDLE ||
        found->second.pinCount != 0 || found->second.activePrefetches != 0 ||
        nowMs >= found->second.expiresAtMs) return readyBool(false);
    if (found->second.residency == ConversationStateResidency::GPU_RESIDENT) return readyBool(true);
    token->store = weak_from_this().lock();
    if (!token->store) throw std::runtime_error("conversation-state-owner-required");
    const auto admission = m_admissionGeneration;
    const auto reservedBytes = found->second.allocatedBytes;
    if (!evictGpuUntilFits(reservedBytes, key, lock)) return readyBool(false);
    found = m_entries.find(key);
    if (m_closing || admission != m_admissionGeneration || found == m_entries.end() ||
        found->second.operationActive || found->second.cleanupFailed ||
        !compatible(found->second.binding, binding) ||
        found->second.lifecycle != ConversationStateLifecycle::IDLE ||
        found->second.residency != ConversationStateResidency::HOST_RESIDENT ||
        found->second.allocatedBytes > reservedBytes ||
        found->second.pinCount != 0 || found->second.activePrefetches != 0 ||
        nowMs >= found->second.expiresAtMs) return readyBool(false);
    token->runner = found->second.adapterRunner;
    token->key = key;
    adapterState = found->second.adapterState;
    found->second.lifecycle = ConversationStateLifecycle::PREFETCHING;
    m_gpuBytes += found->second.allocatedBytes;
    found->second.prefetchGpuReserved = true;
    ++found->second.activePrefetches;
    generation = ++found->second.prefetchGeneration;
    token->armed = true;
  }
  const auto queuedAt = std::chrono::steady_clock::now();
  return std::async(std::launch::async,
    [token, binding, key, generation, nowMs, queuedAt, adapterState] () mutable {
      auto completion = std::move(token);
      {
        std::lock_guard<std::mutex> lock(completion->store->m_mutex);
        const auto found = completion->store->m_entries.find(key);
        if (completion->store->m_closing || found == completion->store->m_entries.end() ||
            found->second.prefetchGeneration != generation) return false;
      }
      bool transferred = true;
      try {
        if (completion->runner && adapterState) {
          auto adapterTransfer = completion->runner->prefetchConversationStateToGpu(*adapterState);
          transferred = adapterTransfer.get();
        }
      }
      catch (...) { transferred = false; }
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - queuedAt).count();
      const auto delta = static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed));
      const auto completed = nowMs > std::numeric_limits<std::uint64_t>::max() - delta
        ? std::numeric_limits<std::uint64_t>::max() : nowMs + delta;
      auto& store = *completion->store;
      std::lock_guard<std::mutex> lock(store.m_mutex);
      const auto found = store.m_entries.find(key);
      if (found == store.m_entries.end() || !compatible(found->second.binding, binding)) return false;
      auto& entry = found->second;
      if (!transferred) return false;
      const auto bytes = entry.allocatedBytes;
      // A completed adapter transfer occupies GPU memory even if cancellation
      // or expiry made this result undispatchable. Keep that residency charged.
      store.m_hostBytes -= std::min(store.m_hostBytes, bytes);
      entry.prefetchGpuReserved = false;
      entry.residency = ConversationStateResidency::GPU_RESIDENT;
      entry.lifecycle = ConversationStateLifecycle::IDLE;
      entry.lastAccess = ++store.m_accessSequence;
      entry.lastUsedAtMs = completed;
      ++store.m_prefetchedEntries;
      store.m_prefetchBytes += bytes;
      store.m_prefetchLatencyMs += delta;
      return !store.m_closing && entry.prefetchGeneration == generation && completed < entry.expiresAtMs;
    });
}

bool
ConversationStateStore::cancelPrefetch(const ConversationStateBinding& binding)
{
  binding.validate();
  const auto key = keyFor(binding);
  std::unique_lock<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (found == m_entries.end() || found->second.operationActive ||
      !compatible(found->second.binding, binding) ||
      found->second.lifecycle != ConversationStateLifecycle::PREFETCHING) return false;
  auto runner = found->second.adapterRunner;
  auto state = found->second.adapterState;
  ++found->second.prefetchGeneration;
  found->second.operationActive = true;
  ++m_activeTransitions;
  lock.unlock();
  bool cancelled = true;
  try {
    if (runner && state) cancelled = runner->cancelConversationStatePrefetch(*state);
  }
  catch (...) { cancelled = false; }
  lock.lock();
  found->second.operationActive = false;
  --m_activeTransitions;
  lock.unlock();
  runner.reset();
  drainDeferredState();
  return cancelled;
}

bool
ConversationStateStore::release(const ConversationStateBinding& binding)
{
  binding.validate();
  const auto key = keyFor(binding);
  std::unique_lock<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (m_closing || found == m_entries.end() || found->second.operationActive || found->second.cleanupFailed ||
      found->second.activePrefetches != 0 || !compatible(found->second.binding, binding) ||
      found->second.lifecycle != ConversationStateLifecycle::IDLE ||
      found->second.pinCount != 0) {
    return false;
  }
  return removeEntry(key, true, lock);
}

std::size_t
ConversationStateStore::cleanupExpired(std::uint64_t nowMs)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  const auto admission = m_admissionGeneration;
  std::vector<std::string> expired;
  for (const auto& item : m_entries) {
    const auto& entry = item.second;
    if (nowMs >= entry.expiresAtMs && !entry.operationActive && !entry.cleanupFailed &&
        entry.activePrefetches == 0 &&
        (entry.pinCount == 0 || entry.lifecycle == ConversationStateLifecycle::COMMITTING))
      expired.push_back(item.first);
  }
  std::size_t removed = 0;
  for (const auto& key : expired) {
    if (m_closing || admission != m_admissionGeneration) break;
    const auto found = m_entries.find(key);
    if (found == m_entries.end() || nowMs < found->second.expiresAtMs) continue;
    if (removeEntry(key, true, lock)) ++removed;
  }
  return removed;
}

struct ConversationStateStore::SnapshotToken
{
  std::shared_ptr<ConversationStateStore> store;
  std::shared_ptr<NativeModelRunner> runner;
  ConversationStateBinding binding;
  std::uint64_t id = 0;
  bool armed = false;
  ~SnapshotToken() { if (armed) store->releaseAdapterSnapshot(binding, id); }
};

std::optional<ConversationAdapterSnapshotLeaseV1>
ConversationStateStore::acquireAdapterSnapshot(
  const ConversationStateBinding& binding, std::uint64_t nowMs)
{
  binding.validate();
  auto owner = weak_from_this().lock();
  if (!owner) throw std::runtime_error("conversation-state-owner-required");
  const auto key = keyFor(binding);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(key);
  if (m_closing || found == m_entries.end() || found->second.operationActive ||
      !(found->second.binding == binding) ||
      !(found->second.binding.identity == binding.identity) || binding.checkpointDigest.empty() ||
      found->second.lifecycle != ConversationStateLifecycle::IDLE ||
      found->second.pinCount != 0 || found->second.activePrefetches != 0 ||
      nowMs >= found->second.expiresAtMs || !found->second.adapterState ||
      !found->second.adapterRunner || found->second.cleanupFailed) {
    return std::nullopt;
  }
  if (m_nextSnapshotTokenId == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("conversation-snapshot-token-id-exhausted");
  ConversationAdapterSnapshotLeaseV1 lease;
  lease.binding = found->second.binding;
  lease.state = *found->second.adapterState;
  lease.runner = found->second.adapterRunner;
  auto token = std::make_shared<SnapshotToken>();
  token->store = std::move(owner);
  token->runner = lease.runner;
  token->binding = lease.binding;
  token->id = m_nextSnapshotTokenId;
  lease.lifetime = token;
  found->second.snapshotOwned = true;
  found->second.snapshotTokenId = token->id;
  found->second.pinCount = 1;
  found->second.lifecycle = ConversationStateLifecycle::PINNED;
  ++m_nextSnapshotTokenId;
  token->armed = true;
  return lease;
}

void
ConversationStateStore::releaseAdapterSnapshot(const ConversationStateBinding& binding,
                                                std::uint64_t tokenId) noexcept
{
  try {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto found = m_entries.find(keyFor(binding));
      if (found != m_entries.end() && found->second.snapshotOwned &&
          found->second.snapshotTokenId == tokenId && found->second.binding == binding) {
        found->second.snapshotOwned = false;
        found->second.snapshotTokenId = 0;
        found->second.pinCount = 0;
        found->second.lifecycle = ConversationStateLifecycle::IDLE;
      }
    }
    drainDeferredState();
  }
  catch (...) { /* Lease destruction never propagates cleanup errors. */ }
}

ConversationStateStore::SaveBatchResult
ConversationStateStore::tryAcquireCommittedSaveBatch(
  const SaveFence& fence, ConversationSaveBatchLimitsV1 limits,
  std::uint64_t nowMs)
{
  SaveBatchResult result;
  if (!fence.m_token || !fence.m_token->armed) return result;
  const auto fenceOwner = fence.m_token->owner.lock();
  if (fenceOwner.get() != this) return result;
  auto storeOwner = weak_from_this().lock();
  if (!storeOwner) throw std::runtime_error("conversation-state-owner-required");

  struct Candidate
  {
    Entry* entry = nullptr;
    std::size_t metadataBytes = 0;
  };

  std::unique_lock<std::mutex> lock(m_mutex);
  if (!saveFenceMatchesLocked(fence)) return result;
  const auto& scope = m_saveFences.find(fence.m_token->id)->second;
  if (m_closing) {
    result.status = ConversationSaveBatchStatusV1::Busy;
    return result;
  }
  if (std::any_of(m_executionAdmissions.begin(), m_executionAdmissions.end(),
      [&] (const auto& item) { return saveScopeBlocksAdmission(scope, item.second); })) {
    result.status = ConversationSaveBatchStatusV1::Busy;
    return result;
  }

  const auto entryMatchesScope = [&] (const Entry& entry) {
    if (scope.kind == SaveScopeKind::Provider) return true;
    if (entry.binding.serviceName != scope.service) return false;
    if (scope.kind == SaveScopeKind::Service) return true;
    if (entry.binding.conversationId != scope.conversation) return false;
    // Without validated authenticated metadata, the Store cannot prove that
    // a same-service/conversation entry belongs to another owner and omit it.
    if (!entry.saveMetadata || !saveMetadataMatches(*entry.saveMetadata, entry.binding))
      return true;
    return entry.saveMetadata->authenticatedRequester == scope.requester;
  };

  using ConversationKey = std::tuple<std::string, std::string, std::string>;
  // Older committed epochs may remain cached, but one save must use the
  // newest checkpoint for every role of each authenticated conversation.
  std::map<ConversationKey, std::uint64_t> latestEpoch;
  for (const auto& item : m_entries) {
    const auto& entry = item.second;
    if (!entryMatchesScope(entry)) continue;
    if (entry.operationActive || entry.lifecycle != ConversationStateLifecycle::IDLE ||
        entry.pinCount != 0 || entry.snapshotOwned || entry.useOwned ||
        entry.activePrefetches != 0) {
      result.status = ConversationSaveBatchStatusV1::Busy;
      return result;
    }
    if (!entry.saveMetadata ||
        !saveMetadataMatches(*entry.saveMetadata, entry.binding) ||
        m_providerIdentity.empty() ||
        entry.binding.identity.providerIdentity != m_providerIdentity ||
        entry.binding.identity.providerBootId != m_providerBootId ||
        entry.binding.identity.cacheEpoch != m_cacheEpoch) {
      result.status = ConversationSaveBatchStatusV1::IneligibleOrUnavailable;
      return result;
    }
    const ConversationKey key{entry.saveMetadata->authenticatedRequester,
      entry.binding.serviceName, entry.binding.conversationId};
    auto& epoch = latestEpoch[key];
    epoch = std::max(epoch, entry.binding.contextEpoch);
  }

  std::vector<Candidate> candidates;
  candidates.reserve(std::min(m_entries.size(), limits.maxEntries));
  bool foundAffectedEntry = false;
  std::size_t affectedCount = 0;
  std::size_t metadataBytes = 0;
  bool metadataSizeOverflow = false;
  std::uint64_t totalBytes = 0;
  bool totalSizeOverflow = false;
  for (auto& item : m_entries) {
    auto& entry = item.second;
    if (!entryMatchesScope(entry)) continue;
    const ConversationKey key{entry.saveMetadata->authenticatedRequester,
      entry.binding.serviceName, entry.binding.conversationId};
    if (entry.binding.contextEpoch != latestEpoch.at(key)) continue;
    foundAffectedEntry = true;
    ++affectedCount;
    if (entry.operationActive || entry.lifecycle != ConversationStateLifecycle::IDLE ||
        entry.pinCount != 0 || entry.snapshotOwned || entry.useOwned ||
        entry.activePrefetches != 0) {
      result.status = ConversationSaveBatchStatusV1::Busy;
      return result;
    }
    if (!entry.saveEligible || !entry.saveMetadata || entry.importedCandidate ||
        entry.cleanupFailed || entry.adapterCleanupUnsafe ||
        entry.importOccupancyUnverified || !entry.adapterState || !entry.adapterRunner ||
        nowMs >= entry.expiresAtMs || nowMs >= entry.binding.expiresAtMs ||
        m_providerIdentity.empty() ||
        entry.binding.identity.providerIdentity != m_providerIdentity ||
        entry.binding.identity.providerBootId != m_providerBootId ||
        entry.binding.identity.cacheEpoch != m_cacheEpoch ||
        !saveMetadataMatches(*entry.saveMetadata, entry.binding)) {
      result.status = ConversationSaveBatchStatusV1::IneligibleOrUnavailable;
      return result;
    }

    const auto rowBytes = serializedSaveMetadataBytes(*entry.saveMetadata);
    const auto maximum = std::numeric_limits<std::size_t>::max();
    if (rowBytes > maximum - metadataBytes) {
      metadataSizeOverflow = true;
      metadataBytes = maximum;
    }
    else if (!metadataSizeOverflow) {
      metadataBytes += rowBytes;
    }
    const auto maximumTotal = std::numeric_limits<std::uint64_t>::max();
    if (!totalSizeOverflow) {
      if (rowBytes > maximumTotal - totalBytes ||
          entry.logicalBytes > maximumTotal - totalBytes - rowBytes) {
        totalSizeOverflow = true;
        totalBytes = maximumTotal;
      }
      else {
        totalBytes += rowBytes + entry.logicalBytes;
      }
    }
    if (candidates.size() < limits.maxEntries)
      candidates.push_back({&entry, rowBytes});
  }
  result.metadataBytes = metadataBytes;
  result.totalBytes = totalBytes;
  if (!foundAffectedEntry) {
    result.status = ConversationSaveBatchStatusV1::Empty;
    return result;
  }
  if (metadataSizeOverflow || totalSizeOverflow ||
      totalBytes > limits.maxTotalBytes || candidates.size() != affectedCount ||
      metadataBytes > limits.maxMetadataBytes) {
    if (candidates.size() == affectedCount) {
      result.affectedRows.reserve(candidates.size());
      for (const auto& candidate : candidates)
        result.affectedRows.push_back({candidate.entry->binding,
          *candidate.entry->saveMetadata, candidate.metadataBytes});
    }
    result.status = ConversationSaveBatchStatusV1::BoundsExceeded;
    return result;
  }
  if (candidates.empty()) {
    result.status = ConversationSaveBatchStatusV1::BoundsExceeded;
    return result;
  }
  if (candidates.size() > std::numeric_limits<std::uint64_t>::max() -
      m_nextSnapshotTokenId) {
    throw std::overflow_error("conversation-snapshot-token-id-exhausted");
  }

  SaveBatch batch;
  batch.m_owner = storeOwner;
  batch.m_fence = fence;
  batch.m_generation = m_admissionGeneration;
  batch.m_rows.reserve(candidates.size());
  batch.m_snapshots.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    const auto& entry = *candidate.entry;
    batch.m_rows.push_back({entry.binding, *entry.saveMetadata, candidate.metadataBytes});

    ConversationAdapterSnapshotLeaseV1 lease;
    lease.binding = entry.binding;
    lease.state = *entry.adapterState;
    lease.runner = entry.adapterRunner;
    auto token = std::make_shared<SnapshotToken>();
    token->store = storeOwner;
    token->runner = lease.runner;
    token->binding = lease.binding;
    lease.lifetime = std::move(token);
    batch.m_snapshots.push_back(std::move(lease));
  }

  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const auto tokenId = m_nextSnapshotTokenId + index;
    auto token = std::static_pointer_cast<SnapshotToken>(
      batch.m_snapshots[index].lifetime);
    token->id = tokenId;
    auto& entry = *candidates[index].entry;
    entry.snapshotOwned = true;
    entry.snapshotTokenId = tokenId;
    entry.pinCount = 1;
    entry.lifecycle = ConversationStateLifecycle::PINNED;
  }
  for (auto& lease : batch.m_snapshots) {
    std::static_pointer_cast<SnapshotToken>(lease.lifetime)->armed = true;
  }
  m_nextSnapshotTokenId += static_cast<std::uint64_t>(candidates.size());
  result.status = ConversationSaveBatchStatusV1::Ready;
  result.batch.emplace(std::move(batch));
  return result;
}

std::optional<ConversationAdapterSnapshotLeaseV1>
ConversationStateStore::snapshotForSaveBatch(
  const SaveBatch& batch, std::size_t index, std::uint64_t nowMs) const
{
  if (index >= batch.m_rows.size() || index >= batch.m_snapshots.size())
    return std::nullopt;
  const auto batchOwner = batch.m_owner.lock();
  const auto fenceOwner = batch.m_fence.m_token ?
    batch.m_fence.m_token->owner.lock() : std::shared_ptr<ConversationStateStore>{};
  if (batchOwner.get() != this || fenceOwner.get() != this) return std::nullopt;

  std::lock_guard<std::mutex> lock(m_mutex);
  if (batch.m_generation != m_admissionGeneration ||
      !saveFenceMatchesLocked(batch.m_fence)) return std::nullopt;
  const auto& row = batch.m_rows[index];
  const auto& lease = batch.m_snapshots[index];
  const auto token = std::static_pointer_cast<SnapshotToken>(lease.lifetime);
  if (!token || !token->armed || token->store.get() != this || token->id == 0 ||
      !(token->binding == row.binding) || !(lease.binding == row.binding) ||
      token->runner != lease.runner) return std::nullopt;
  const auto found = m_entries.find(keyFor(row.binding));
  if (found == m_entries.end()) return std::nullopt;
  const auto& entry = found->second;
  if (!entry.snapshotOwned || entry.snapshotTokenId != token->id ||
      entry.pinCount != 1 || entry.lifecycle != ConversationStateLifecycle::PINNED ||
      entry.operationActive || entry.cleanupFailed || !entry.saveEligible ||
      !entry.saveMetadata || !entry.adapterState || !entry.adapterRunner ||
      entry.adapterRunner != lease.runner || !(entry.binding == row.binding) ||
      !(entry.binding.identity == row.binding.identity) ||
      !saveMetadataMatches(*entry.saveMetadata, entry.binding) ||
      entry.saveMetadata->authenticatedRequester != row.metadata.authenticatedRequester ||
      entry.saveMetadata->receipt.computedDigest() != row.metadata.receipt.computedDigest() ||
      nowMs >= entry.expiresAtMs || nowMs >= entry.binding.expiresAtMs || m_closing ||
      entry.binding.identity.providerIdentity != m_providerIdentity ||
      entry.binding.identity.providerBootId != m_providerBootId ||
      entry.binding.identity.cacheEpoch != m_cacheEpoch) return std::nullopt;
  return lease;
}

struct ConversationStateStore::UseToken
{
  std::shared_ptr<ConversationStateStore> store;
  std::shared_ptr<NativeModelRunner> runner;
  ConversationStateBinding binding;
  bool armed = false;
  bool executionClaimed = false;
  ~UseToken()
  {
    if (armed) store->releaseConversationUse(binding);
  }
};

std::optional<ConversationStateUseLeaseV1>
ConversationStateStore::acquireConversationUse(
  const ConversationStateBinding& binding, std::uint64_t nowMs)
{
  binding.validate();
  auto owner = weak_from_this().lock();
  if (!owner) {
    throw std::runtime_error("conversation-state-owner-required");
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(keyFor(binding));
  if (m_closing || found == m_entries.end() || found->second.operationActive ||
      !compatible(found->second.binding, binding) ||
      found->second.lifecycle != ConversationStateLifecycle::IDLE ||
      found->second.residency != ConversationStateResidency::GPU_RESIDENT ||
      found->second.pinCount != 0 || found->second.activePrefetches != 0 ||
      found->second.cleanupFailed || nowMs >= found->second.expiresAtMs) {
    ++m_misses;
    return std::nullopt;
  }
  ConversationStateUseLeaseV1 lease;
  lease.binding = found->second.binding;
  if (found->second.adapterState) {
    if (!found->second.adapterRunner) {
      return std::nullopt;
    }
    lease.state = found->second.adapterState;
    lease.runner = found->second.adapterRunner;
  }
  else {
    lease.bundle = found->second.state;
  }
  auto token = std::make_shared<UseToken>();
  token->store = std::move(owner);
  token->runner = lease.runner;
  token->binding = lease.binding;
  lease.lifetime = token;
  found->second.useOwned = true;
  found->second.pinCount = 1;
  found->second.lifecycle = ConversationStateLifecycle::PINNED;
  found->second.lastAccess = ++m_accessSequence;
  found->second.lastUsedAtMs = nowMs;
  token->armed = true;
  ++m_hits;
  return lease;
}

std::optional<ConversationStateUseLeaseV1>
ConversationStateStore::claimConversationUseForExecution(
  ConversationStateUseLeaseV1 lease, const ConversationStateBinding& expected,
  std::uint64_t nowMs)
{
  static_assert(std::is_nothrow_move_constructible_v<ConversationStateUseLeaseV1>);
  expected.validate();
  if (!lease.lifetime) return std::nullopt;
  const auto token = std::static_pointer_cast<UseToken>(lease.lifetime);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(keyFor(expected));
  if (token->store.get() != this || !token->armed || token->executionClaimed ||
      !(token->binding == expected) || !(token->binding.identity == expected.identity) ||
      !(lease.binding == expected) || !(lease.binding.identity == expected.identity) ||
      m_closing || found == m_entries.end() || !found->second.useOwned ||
      found->second.lifecycle != ConversationStateLifecycle::PINNED ||
      found->second.residency != ConversationStateResidency::GPU_RESIDENT ||
      found->second.pinCount != 1 || found->second.operationActive ||
      found->second.snapshotOwned || found->second.activePrefetches ||
      found->second.cleanupFailed || nowMs >= found->second.expiresAtMs ||
      !(found->second.binding == expected) || !(found->second.binding.identity == expected.identity))
    return std::nullopt;
  ConversationStateUseLeaseV1 actual;
  actual.binding = found->second.binding;
  if (found->second.adapterState) {
    if (!found->second.adapterRunner) return std::nullopt;
    actual.state = found->second.adapterState;
    actual.runner = found->second.adapterRunner;
  }
  else actual.bundle = found->second.state;
  actual.lifetime = lease.lifetime;
  token->executionClaimed = true;
  return actual;
}

void
ConversationStateStore::releaseConversationUse(const ConversationStateBinding& binding) noexcept
{
  try {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto found = m_entries.find(keyFor(binding));
      if (found != m_entries.end() && found->second.useOwned &&
          found->second.binding == binding && found->second.binding.identity == binding.identity) {
        found->second.useOwned = false;
        found->second.pinCount = 0;
        found->second.lifecycle = ConversationStateLifecycle::IDLE;
      }
    }
    drainDeferredState();
  }
  catch (...) { }
}

void
ConversationStateStore::clear()
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_admissionGeneration;
    m_executionAdmissions.clear();
    m_saveFences.clear();
    m_preserveCommittedOnImportFailure = false;
    m_closing = true;
  }
  drainDeferredState();
}

void
ConversationStateStore::drainDeferredState(bool retryFailures) noexcept
{
  try {
    std::map<std::string, Entry> detached;
    bool preservingBatch = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (!m_closing || m_drainRunning || m_activeTransitions != 0) return;
      if (m_preserveCommittedOnImportFailure && !retryFailures) return;
      for (const auto& item : m_entries) {
        const auto& entry = item.second;
        if (entry.activePrefetches != 0 ||
            (entry.pinCount != 0 && entry.lifecycle != ConversationStateLifecycle::COMMITTING))
          return;
      }
      m_drainRunning = true;
      preservingBatch = m_preserveCommittedOnImportFailure;
      if (m_preserveCommittedOnImportFailure) {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
          if (it->second.cleanupFailed) detached.insert(m_entries.extract(it++));
          else ++it;
        }
      }
      else detached.swap(m_entries);
    }
    // Detached entries remain fully charged. Admission is closed throughout
    // adapter callbacks, including reentrant clear/rebind requests.
    for (auto it = detached.begin(); it != detached.end();) {
      if (it->second.adapterCleanupUnsafe || (it->second.cleanupFailed && !retryFailures)) { ++it; continue; }
      bool released = true;
      if (it->second.adapterState && it->second.adapterRunner) {
        try { released = it->second.adapterRunner->releaseConversationState(*it->second.adapterState); }
        catch (...) { released = false; }
      }
      if (!released) {
        it->second.cleanupFailed = true;
        ++it;
        continue;
      }
      wipeTensorBundle(it->second.state);
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (it->second.residency == ConversationStateResidency::GPU_RESIDENT)
          m_gpuBytes -= std::min(m_gpuBytes, it->second.allocatedBytes);
        else if (it->second.residency == ConversationStateResidency::HOST_RESIDENT)
          m_hostBytes -= std::min(m_hostBytes, it->second.allocatedBytes);
        if (it->second.evictionRequested) ++m_evictions;
        else ++m_cleanups;
        if (it->second.rollbackRequested) ++m_promotionRollbacks;
      }
      it = detached.erase(it);
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_entries.merge(detached); // Reuse failed nodes without new allocations.
    m_drainRunning = false;
    if (m_preserveCommittedOnImportFailure) {
      if (std::none_of(m_entries.begin(), m_entries.end(),
          [](const auto& item) { return item.second.cleanupFailed; })) {
        m_preserveCommittedOnImportFailure = false;
        m_closing = false;
      }
      return;
    }
    if (preservingBatch && !m_entries.empty()) {
      // A clear/rebind arriving during selective retry requests a full drain.
      lock.unlock();
      drainDeferredState(false); // Already-attempted failures need another explicit retry.
      return;
    }
    if (!m_entries.empty()) return;
    if (m_pendingProviderBinding) {
      m_providerIdentity = std::move(m_pendingProviderBinding->identity);
      m_providerBootId = std::move(m_pendingProviderBinding->bootId);
      m_cacheEpoch = m_pendingProviderBinding->epoch;
      m_pendingProviderBinding.reset();
    }
    m_closing = false;
  }
  catch (...) { /* Adapter failures are retained above; no destructor throws. */ }
}

ConversationStateDrainSnapshotV1
ConversationStateStore::drainSnapshot() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  ConversationStateDrainSnapshotV1 result;
  result.closing = m_closing;
  result.complete = !m_closing && !m_drainRunning;
  result.retainedGpuBytes = m_gpuBytes;
  result.retainedHostBytes = m_hostBytes;
  for (const auto& item : m_entries) {
    const auto& entry = item.second;
    result.activeSnapshots += entry.snapshotOwned ? 1 : 0;
    if (!entry.snapshotOwned && entry.lifecycle != ConversationStateLifecycle::COMMITTING)
      result.activeUses += entry.pinCount;
    result.activePrefetches += entry.activePrefetches;
    result.activeTransitions += entry.operationActive ? 1 : 0;
    result.cleanupFailures += entry.cleanupFailed ? 1 : 0;
    result.unverifiedOccupancyEntries += entry.importOccupancyUnverified ? 1 : 0;
    result.unsafeCleanupEntries += entry.adapterCleanupUnsafe ? 1 : 0;
  }
  if (!result.complete) result.reason = result.cleanupFailures ? "CLEANUP_FAILED" : "DRAIN_PENDING";
  return result;
}

bool
ConversationStateStore::retryDeferredCleanup()
{
  drainDeferredState(true);
  return drainSnapshot().complete;
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
    if (entry.saveEligible) ++result.saveEligibleEntries;
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
  , m_conversationStateStore(std::make_shared<ConversationStateStore>(conversationGpuMaxBytes,
                             conversationGpuMaxEntries,
                             conversationHostMaxBytes,
                             conversationHostMaxEntries,
                             conversationRetentionMs))
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
  if (!providerIdentity.empty() && !providerBootId.empty()) {
    bindConversationStateProvider(providerIdentity, providerBootId, cacheEpoch);
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  m_runnerSpecs[spec.role] = spec;
  m_runners[std::move(spec.role)] = std::move(runner);
}

void
NativeProviderRuntime::bindConversationStateProvider(
  std::string providerIdentity, std::string providerBootId, std::uint64_t cacheEpoch)
{
  if (providerIdentity.empty() || providerBootId.empty() || cacheEpoch == 0)
    throw std::invalid_argument("conversation Provider binding is incomplete");
  if (m_conversationStateStore->setProviderBinding(
        std::move(providerIdentity), std::move(providerBootId), cacheEpoch)) {
    // The store invalidates committed entries on a Provider boot/cache
    // change. Prune the NativeProviderRuntime side index as well; leaving
    // staged records behind would retain obsolete ownership and make a later
    // COMMIT look like a live candidate.
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_stagedConversationPromotions.begin(); it != m_stagedConversationPromotions.end();) {
      // A new-incarnation import may have staged after Store reopened but
      // before registration acquired this mutex. Preserve its live record.
      if (m_conversationStateStore->retainsStagedBinding(it->second.binding)) ++it;
      else it = m_stagedConversationPromotions.erase(it);
    }
  }
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

ProviderRoleResult
NativeProviderRuntime::executeRole(
  std::string sessionId,
  RoleSpec role,
  std::shared_ptr<DependencyIo> io,
  ProviderRoleWorker::NativeRunnerPreparation prepareRunner,
  std::map<std::string, TensorBundle> initialInputsByScope,
  RoleExecutionContext::StreamEventSink eventSink,
  std::function<void()> executionGuard)
{
  return executeRoleAsyncImpl(std::move(sessionId), std::move(role),
                              std::move(io), nullptr, std::move(prepareRunner),
                              std::move(initialInputsByScope), std::move(eventSink),
                              std::move(executionGuard), std::launch::deferred).get();
}

std::shared_ptr<NativeModelRunner>
NativeProviderRuntime::registeredRunnerForRole(const std::string& role) const
{
  return findRunner(role);
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
                              std::move(eventSink), std::move(executionGuard),
                              std::launch::async);
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
                              std::move(eventSink), std::move(executionGuard),
                              std::launch::async);
}

std::future<std::shared_ptr<NativeModelRunner>>
NativeProviderRuntime::prepareRunnerAsync(
  ProviderRoleWorker::NativeRunnerPreparation prepareRunner,
  std::function<void()> executionGuard)
{
  return m_worker.prepareRunnerAsync(std::move(prepareRunner),
                                     std::move(executionGuard));
}

std::optional<ConversationStateUseLeaseV1>
NativeProviderRuntime::acquireConversationUse(
  const ConversationStateBinding& binding, std::uint64_t nowMs)
{
  return m_conversationStateStore->acquireConversationUse(binding, nowMs);
}

std::future<ProviderRoleResult>
NativeProviderRuntime::executeRoleWithConversationUseAsync(
  std::string sessionId, RoleSpec role, std::shared_ptr<DependencyIo> io,
  ConversationStateUseLeaseV1 use, std::map<std::string, TensorBundle> initialInputsByScope,
  RoleExecutionContext::StreamEventSink eventSink, std::function<void()> executionGuard)
{
  return executeRoleAsyncImpl(std::move(sessionId), std::move(role), std::move(io),
    nullptr, {}, std::move(initialInputsByScope), std::move(eventSink),
    std::move(executionGuard), std::launch::async, std::move(use));
}

ProviderRoleResult
NativeProviderRuntime::executeRoleWithConversationUse(
  std::string sessionId, RoleSpec role, std::shared_ptr<DependencyIo> io,
  ConversationStateUseLeaseV1 use, std::map<std::string, TensorBundle> initialInputsByScope,
  RoleExecutionContext::StreamEventSink eventSink, std::function<void()> executionGuard)
{
  return executeRoleAsyncImpl(std::move(sessionId), std::move(role), std::move(io),
    nullptr, {}, std::move(initialInputsByScope), std::move(eventSink),
    std::move(executionGuard), std::launch::deferred, std::move(use)).get();
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
  std::function<void()> executionGuard,
  std::launch stateCompletionPolicy,
  std::optional<ConversationStateUseLeaseV1> readyUse)
{
  if (readyUse && (!role.conversationStateBinding || role.stateInputNames.empty() || prepareRunner))
    throw std::invalid_argument("READY use requires one stateful continuation without preparation");
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
  const bool stateful = !role.stateInputNames.empty() ||
                        !role.stateOutputNames.empty();
  auto runner = std::move(preparedRunner);
  if (!runner && stateful && role.inferenceEpoch > 0 &&
      role.candidateDecodeStateIdentity) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto sessionRunner = m_sessionRunners.find(sessionRunnerKey(
      sessionId, role.role,
      role.candidateDecodeStateIdentity->attemptEpoch));
    if (sessionRunner != m_sessionRunners.end()) {
      runner = sessionRunner->second;
    }
  }
  if (runner) {
    // A resumed conversation may have restored the original adapter runner.
    // Keep that exact runner for every later decode epoch in this attempt.
    prepareRunner = {};
  }
  if (!runner && !prepareRunner && !(stateful && role.conversationStateBinding)) {
    runner = findRunner(role.role);
  }
  const auto runnerSpec = findRunnerSpec(role.role);
  std::optional<KvStateBinding> stateBinding;
  std::optional<KvStateBinding> predecessorBinding;
  std::optional<ConversationStateBinding> conversationBinding;
  std::optional<ConversationStateUseLeaseV1> conversationUse;
  struct Completion
  {
    std::future<ProviderRoleResult> future;
    std::optional<ConversationStateUseLeaseV1> use;
  };
  auto completion = std::make_shared<Completion>();
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
          !nativeConversationStateAllowsContinuation(requested, *role.candidateDecodeStateIdentity)) {
        throw std::invalid_argument(
          "conversation state restore requires a fresh request identity");
      }
      // A host-resident entry must be made dispatchable before the runner is
      // called.  The prefetch is single-flight in ConversationStateStore, so
      // concurrent turns cannot duplicate the transfer.  Pin the restored
      // entry until this role's worker has finished; otherwise an inactive-LRU
      // eviction could invalidate the state between lookup and execution.
      if (readyUse) {
        conversationUse = m_conversationStateStore->claimConversationUseForExecution(
          std::move(*readyUse), requested, role.conversationStateLookupNowMs);
        readyUse.reset();
      }
      else {
        if (!m_conversationStateStore->prefetchToGpu(
              requested, role.conversationStateLookupNowMs).get()) {
          throw std::runtime_error("PROVIDER_CONVERSATION_STATE_MISSING");
        }
        conversationUse = m_conversationStateStore->acquireConversationUse(
          requested, role.conversationStateLookupNowMs);
      }
      if (!conversationUse) {
        throw std::runtime_error("PROVIDER_CONVERSATION_STATE_MISSING");
      }
      conversationBinding = conversationUse->binding;
      const auto adapterState = conversationUse->state;
      if (adapterState) {
        // A committed adapter entry owns the exact runner that created its
        // state.  Rebind that runner before restore; invoking the
        // post-Selection preparation callback here would assemble a second
        // runner, while leaving `runner` empty would dereference it before
        // the worker can invoke the callback.
        runner = conversationUse->runner;
        if (!runner || !runner->restoreConversationState(*adapterState, sessionId)) {
          throw std::runtime_error("PROVIDER_CONVERSATION_STATE_MISSING");
        }
        if (role.generationLineage) {
          std::ostringstream reuseRecord;
          reuseRecord << "NDNSF_DI_PROVIDER_PREPARATION phase=RUNNER_REUSE_HIT"
                      << " status=observed requestId="
                      << role.candidateDecodeStateIdentity->requestId
                      << " attemptEpoch="
                      << role.candidateDecodeStateIdentity->attemptEpoch
                      << " provider="
                      << role.candidateDecodeStateIdentity->providerIdentity
                      << " role=" << role.role
                      << " planDigest=" << role.generationLineage->planDigest
                      << " detail=live-runner";
          logRuntimeEvidence(reuseRecord.str());
        }
        prepareRunner = {};
      }
      else {
        if (!conversationUse->bundle) {
          throw std::runtime_error("PROVIDER_CONVERSATION_STATE_MISSING");
        }
        if (!runner && !prepareRunner) runner = findRunner(role.role);
        initialInputsByScope["__ndnsf_provider_decode_state"] =
          std::move(*conversationUse->bundle);
        conversationUse->bundle.reset();
      }
      // Emit only after the exact retained parent was pinned and restored.
      // This is distinct from an assembled-model/runner cache hit.
      std::ostringstream record;
      record << "NDNSF_DI_CONVERSATION_KV_RESTORED"
             << " requestId=" << role.candidateDecodeStateIdentity->requestId
             << " role=" << role.role
             << " statePath=" << (adapterState ? "adapter" : "tensor-bundle")
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
  try {
    completion->use = std::move(conversationUse);
    // WorkItem retains this owner until inference and output publication settle.
    // The guard must never access use: only the completion releases that lease.
    const auto workerGuard = [completion, executionGuard] {
      if (executionGuard) executionGuard();
    };
    if (prepareRunner) {
      completion->future = m_worker.executePreparedAsync(
        sessionId, role, std::move(io), std::move(prepareRunner),
        std::move(initialInputsByScope), std::move(eventSink),
        workerGuard);
    }
    else {
      completion->future = m_worker.executeAsync(
        sessionId, role, std::move(io), std::move(runner),
        std::move(initialInputsByScope), std::move(eventSink),
        workerGuard);
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
    return std::move(completion->future);
  }
  try {
    return std::async(
    stateCompletionPolicy,
    [this, role = std::move(role), stateSessionId = sessionId,
     binding = std::move(*stateBinding),
     predecessor = predecessorBinding,
     executionGuard = std::move(executionGuard),
     completion] () mutable {
      NativeWorkScope completionWork(role.workStatistics, "role-runtime-state-completion",
                                     NativeWorkKind::UNCLASSIFIED);
      bool candidateStaged = false;
      const auto releaseConversationPin = [&completion] {
        completion->use.reset();
      };
      try {
        NativeWorkScope workerWait(role.workStatistics, "role-runtime-worker-wait",
                                   NativeWorkKind::WAIT);
        auto result = completion->future.get();
        workerWait.finish();
        if (executionGuard) executionGuard();
        if (result.runner) {
          std::lock_guard<std::mutex> lock(m_mutex);
          const auto attemptEpoch = role.candidateDecodeStateIdentity
            ? role.candidateDecodeStateIdentity->attemptEpoch
            : role.attemptEpoch;
          m_sessionRunners[sessionRunnerKey(
            stateSessionId, role.role, attemptEpoch)] =
            result.runner;
        }
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
  catch (...) {
    const auto failure = std::current_exception();
    // Submission can fail after the worker has started. Keep its state pinned
    // until the actual worker settles before restoring the predecessor.
    if (completion->future.valid()) completion->future.wait();
    completion->use.reset();
    if (predecessorBinding &&
        m_decodeStateStore.rollbackTransition(*predecessorBinding)) {
      ++m_decodeStateRollbacks;
    }
    std::rethrow_exception(failure);
  }
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
  return m_conversationStateStore->snapshot();
}

std::optional<ConversationStateStore::ExecutionAdmission>
NativeProviderRuntime::tryAcquireConversationExecution(
  const std::string& authenticatedRequester,
  const std::string& serviceName,
  const std::string& conversationId)
{
  return m_conversationStateStore->tryAcquireConversationExecution(
    authenticatedRequester, serviceName, conversationId);
}

std::optional<ConversationStateStore::SaveFence>
NativeProviderRuntime::beginConversationSaveFence(
  const std::string& authenticatedRequester,
  const std::string& serviceName,
  const std::string& conversationId)
{
  return m_conversationStateStore->beginConversationSaveFence(
    authenticatedRequester, serviceName, conversationId);
}

std::optional<ConversationStateStore::SaveFence>
NativeProviderRuntime::beginServiceWithdrawalSaveFence(
  const std::string& serviceName, const std::string& withdrawalOperationId,
  std::uint64_t registrationGeneration)
{
  return m_conversationStateStore->beginServiceWithdrawalSaveFence(
    serviceName, withdrawalOperationId, registrationGeneration);
}

std::optional<ConversationStateStore::SaveFence>
NativeProviderRuntime::beginProviderWithdrawalSaveFence(
  const std::string& withdrawalOperationId,
  std::uint64_t registrationGeneration)
{
  return m_conversationStateStore->beginProviderWithdrawalSaveFence(
    withdrawalOperationId, registrationGeneration);
}

std::optional<ConversationStateStore::SaveFence>
NativeProviderRuntime::borrowServiceWithdrawalSaveFence(
  const ConversationStateStore::SaveFence& parent,
  const std::string& serviceName, const std::string& withdrawalOperationId,
  std::uint64_t registrationGeneration)
{
  return m_conversationStateStore->borrowServiceWithdrawalSaveFence(
    parent, serviceName, withdrawalOperationId, registrationGeneration);
}

bool
NativeProviderRuntime::matchesServiceWithdrawalSaveFence(
  const ConversationStateStore::SaveFence& fence,
  const std::string& serviceName,
  const std::string& withdrawalOperationId,
  std::uint64_t registrationGeneration) const noexcept
{
  return m_conversationStateStore->matchesServiceWithdrawalSaveFence(
    fence, serviceName, withdrawalOperationId, registrationGeneration);
}

std::optional<ConversationStateStore::SaveFence>
NativeProviderRuntime::borrowConversationWithdrawalSaveFence(
  const ConversationStateStore::SaveFence& parent,
  const std::string& authenticatedRequester,
  const std::string& serviceName, const std::string& conversationId,
  const std::string& withdrawalOperationId,
  std::uint64_t registrationGeneration)
{
  return m_conversationStateStore->borrowConversationWithdrawalSaveFence(
    parent, authenticatedRequester, serviceName, conversationId,
    withdrawalOperationId, registrationGeneration);
}

bool
NativeProviderRuntime::isSaveFenceQuiescent(
  const ConversationStateStore::SaveFence& fence) const noexcept
{
  return m_conversationStateStore->isSaveFenceQuiescent(fence);
}

ConversationStateStore::SaveBatchResult
NativeProviderRuntime::tryAcquireCommittedSaveBatch(
  const ConversationStateStore::SaveFence& fence,
  ConversationSaveBatchLimitsV1 limits, std::uint64_t nowMs)
{
  return m_conversationStateStore->tryAcquireCommittedSaveBatch(
    fence, limits, nowMs);
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
  std::uint64_t nowMs,
  std::optional<ConversationSaveMetadataV1> saveMetadata,
  const ConversationStateStore::ExecutionAdmission* admission)
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
  StagedConversationPromotion staged;
  staged.sessionId = sessionId;
  staged.role = role;
  staged.binding = promotionBinding;
  if (admission) staged.executionAdmission = *admission;
  const auto key = conversationPromotionKey(promotionBinding);
  std::shared_ptr<NativeModelRunner> runner;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto sessionRunner = m_sessionRunners.find(
      sessionRunnerKey(sessionId, role.role,
                       role.candidateDecodeStateIdentity->attemptEpoch));
    if (sessionRunner != m_sessionRunners.end()) {
      runner = sessionRunner->second;
    }
    else {
      const auto processRunner = m_runners.find(role.role);
      if (processRunner != m_runners.end()) {
        runner = processRunner->second;
      }
    }
  }
  const bool supportsAdapterConversationState =
    runner && runner->supportsConversationStateTransfer();
  const char* promotionStatePath = "tensor-bundle";
  if (supportsAdapterConversationState) {
    const auto conversationKey = conversationPromotionKey(promotionBinding);
    const auto adapterState = runner->promoteSessionStateToConversation(
      sessionId, conversationKey);
    if (!adapterState ||
        !m_conversationStateStore->stageAdapterPromotion(
          sourceBinding.requestId, role.role, std::move(binding),
          *adapterState, runner, nowMs, std::move(saveMetadata), admission)) {
      if (adapterState) {
        (void)runner->releaseConversationState(*adapterState);
      }
      return false;
    }
    promotionStatePath = "adapter";
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
    if (!m_conversationStateStore->stagePromotion(
          sourceBinding.requestId, role.role, std::move(binding), *state, nowMs,
          std::move(saveMetadata), admission)) {
      return false;
    }
  }
  bool inserted = false;
  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    inserted = m_stagedConversationPromotions.emplace(key, std::move(staged)).second;
  }
  catch (...) {
    const auto failure = std::current_exception();
    try { m_conversationStateStore->rollbackStagedPromotion(promotionBinding); }
    catch (...) { /* The staged entry remains owned/charged for later cleanup. */ }
    std::rethrow_exception(failure);
  }
  if (!inserted) {
    m_conversationStateStore->rollbackStagedPromotion(promotionBinding);
    return false;
  }
  std::ostringstream record;
  record << "NDNSF_DI_CONVERSATION_STATE event=promotion-staged"
         << " requestId=" << promotionBinding.identity.requestId
         << " role=" << role.role
         << " statePath=" << promotionStatePath
         << " runnerAvailable=" << (runner ? "true" : "false")
         << " adapterTransfer=" << (supportsAdapterConversationState ? "true" : "false");
  logRuntimeEvidence(record.str());
  return true;
}

bool
NativeProviderRuntime::stageImportedConversationState(
  ConversationStateBinding binding,
  NativeConversationStateHandleV1 state,
  std::shared_ptr<NativeModelRunner> runner,
  std::uint64_t nowMs)
{
  binding.validate();
  state.validate();
  StagedConversationPromotion staged;
  staged.sessionId = binding.identity.requestId;
  staged.role.role = binding.identity.roleName;
  staged.role.candidateDecodeStateIdentity = binding.identity;
  staged.binding = binding;
  staged.imported = true;
  const auto key = conversationPromotionKey(binding);
  const auto request = binding.identity.requestId;
  const auto role = binding.identity.roleName;
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto inserted = m_stagedConversationPromotions.emplace(key, std::move(staged));
  if (!inserted.second) return false;
  try {
    // Admission invokes no adapter callbacks. Reserve the side index first so
    // no throwing Runtime mutation follows Store ownership transfer.
    if (m_conversationStateStore->stageImportedAdapterState(
          request, role, std::move(binding), std::move(state), std::move(runner), nowMs, {}))
      return true;
  }
  catch (...) {
    m_stagedConversationPromotions.erase(inserted.first);
    throw;
  }
  m_stagedConversationPromotions.erase(inserted.first);
  return false;
}

bool
NativeProviderRuntime::importConversationState(
  const NativePortableKvSnapshotV1& snapshot, ConversationStateBinding binding,
  std::shared_ptr<NativeModelRunner> runner, const NativePortableKvLimitsV1& limits,
  std::uint64_t nowMs)
{
  return importConversationStateOwned(snapshot, std::move(binding), std::move(runner),
                                      limits, nowMs).has_value();
}

std::optional<ConversationStateImportOwnershipV1>
NativeProviderRuntime::importConversationStateOwned(
  const NativePortableKvSnapshotV1& snapshot,
  ConversationStateBinding binding,
  std::shared_ptr<NativeModelRunner> runner,
  const NativePortableKvLimitsV1& limits,
  std::uint64_t nowMs,
  const std::string& authenticatedRequester,
  const ConversationStateStore::ExecutionAdmission* admission)
{
  validateNativePortableKvSnapshot(snapshot, limits);
  binding.validate();
  if (!runner || !runner->supportsPortableConversationState() ||
      !binding.checkpointDigest.empty() || nowMs >= binding.expiresAtMs ||
      snapshot.logicalBytes == 0 || snapshot.logicalBytes > std::numeric_limits<std::size_t>::max() ||
      !nativePortableKvIdentityCompatible(snapshot.originIdentity, binding.identity) ||
      (binding.importedOriginIdentity && *binding.importedOriginIdentity != snapshot.originIdentity))
  {
    rejectConversationImport("validate", !runner ? "runner-missing" :
      !runner->supportsPortableConversationState() ? "runner-unsupported" :
      !binding.checkpointDigest.empty() ? "checkpoint-already-bound" :
      nowMs >= binding.expiresAtMs ? "expired" :
      snapshot.logicalBytes == 0 || snapshot.logicalBytes > std::numeric_limits<std::size_t>::max() ?
      "logical-bytes" : "identity-incompatible", binding);
    return std::nullopt;
  }
  StagedConversationPromotion staged;
  staged.sessionId = binding.identity.requestId;
  staged.role.role = binding.identity.roleName;
  staged.role.candidateDecodeStateIdentity = binding.identity;
  staged.binding = binding;
  staged.imported = true;
  if (admission) staged.executionAdmission = *admission;
  const auto key = conversationPromotionKey(binding);
  std::shared_ptr<ConversationStateStore::ImportReservationToken> reservation;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto inserted = m_stagedConversationPromotions.emplace(key, std::move(staged));
    if (!inserted.second) {
      rejectConversationImport("stage", "destination-exists", binding);
      return std::nullopt;
    }
    try {
      reservation = m_conversationStateStore->reserveImportedState(
        binding, runner, static_cast<std::size_t>(snapshot.logicalBytes), nowMs,
        authenticatedRequester, admission);
    }
    catch (...) { m_stagedConversationPromotions.erase(inserted.first); throw; }
    if (!reservation) { m_stagedConversationPromotions.erase(inserted.first); return std::nullopt; }
    inserted.first->second.importAdmission = reservation;
  }
  const auto forget = [&] {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_stagedConversationPromotions.find(key);
    if (found != m_stagedConversationPromotions.end() && found->second.imported &&
        found->second.binding == binding && found->second.binding.identity == binding.identity &&
        found->second.importAdmission.lock() == reservation)
      m_stagedConversationPromotions.erase(found);
  };
  try {
    auto state = runner->importConversationState(snapshot, binding.identity, reservation->key, limits);
    const bool accepted = m_conversationStateStore->completeImportedState(reservation, std::move(state), limits);
    if (!accepted) {
      forget();
      // Never execute token/adapter cleanup while holding the Runtime mutex.
      reservation.reset();
      return std::nullopt;
    }
    return ConversationStateImportOwnershipV1(std::move(reservation));
  }
  catch (...) {
    forget();
    reservation.reset();
    throw;
  }
}

bool
NativeProviderRuntime::commitImportedConversationState(
  const ConversationStateImportOwnershipV1& ownership, const std::string& checkpointDigest)
{
  const auto& token = ownership.m_token;
  if (!token || token->owner != m_conversationStateStore) return false;
  if (!m_conversationStateStore->commitStagedPromotionImpl(
        token->binding, checkpointDigest, token)) return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_stagedConversationPromotions.find(token->key);
  if (found != m_stagedConversationPromotions.end() &&
      found->second.importAdmission.lock() == token)
    m_stagedConversationPromotions.erase(found);
  return true;
}

ImportedStateReleaseResult
NativeProviderRuntime::releaseImportedConversationState(
  const ConversationStateImportOwnershipV1& ownership)
{
  const auto& token = ownership.m_token;
  const auto result = m_conversationStateStore->releaseImportedState(token);
  if (token && (result == ImportedStateReleaseResult::Released ||
                result == ImportedStateReleaseResult::AlreadyGone)) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_stagedConversationPromotions.find(token->key);
    if (found != m_stagedConversationPromotions.end() &&
        found->second.importAdmission.lock() == token)
      m_stagedConversationPromotions.erase(found);
  }
  return result;
}

ConversationStateDrainSnapshotV1
NativeProviderRuntime::conversationStateDrainSnapshot() const
{
  return m_conversationStateStore->drainSnapshot();
}

bool
NativeProviderRuntime::retryConversationStateCleanup()
{
  return m_conversationStateStore->retryDeferredCleanup();
}

NativePortableKvSnapshotV1
NativeProviderRuntime::exportConversationState(
  const ConversationStateBinding& binding,
  const NativePortableKvLimitsV1& limits,
  std::uint64_t nowMs)
{
  auto lease = m_conversationStateStore->acquireAdapterSnapshot(binding, nowMs);
  if (!lease) throw std::runtime_error("portable-kv-committed-state-unavailable");
  if (!lease->runner->supportsPortableConversationState())
    throw std::runtime_error("portable-kv-unsupported");
  return lease->runner->exportConversationState(lease->state, lease->binding.identity, limits);
}

NativePortableKvSnapshotV1
NativeProviderRuntime::exportConversationState(
  const ConversationStateStore::SaveBatch& batch,
  std::size_t index,
  const NativePortableKvLimitsV1& limits,
  std::uint64_t nowMs)
{
  auto lease = m_conversationStateStore->snapshotForSaveBatch(batch, index, nowMs);
  if (!lease) throw std::runtime_error("portable-kv-save-batch-unavailable");
  if (!lease->runner->supportsPortableConversationState())
    throw std::runtime_error("portable-kv-unsupported");
  return lease->runner->exportConversationState(lease->state, lease->binding.identity, limits);
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
  if (staged.imported || !staged.role.candidateDecodeStateIdentity ||
      *staged.role.candidateDecodeStateIdentity != binding.identity ||
      !m_conversationStateStore->commitStagedPromotion(binding)) {
    return false;
  }
  if (!m_decodeStateStore.erase(staged.sessionId, staged.role.role)) {
    // Keep the conversation store fail-closed if ownership cannot be removed
    // from the request-local store.  Normal runtime operation is serialized
    // per role, so this is a defensive rollback path.
    m_conversationStateStore->release(binding);
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
    m_sessionRunners.erase(sessionRunnerKey(
      staged.sessionId, staged.role.role,
      staged.role.candidateDecodeStateIdentity->attemptEpoch));
  }
  m_conversationStateStore->noteRequestLocalRelease();
  (void)m_conversationStateStore->finalizeSaveEligibility(binding);
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
  if (!m_conversationStateStore->commitStagedPromotion(binding,
                                                       checkpointDigest)) {
    return false;
  }
  if (staged.imported) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stagedConversationPromotions.erase(key);
    return true;
  }
  auto committedBinding = binding;
  committedBinding.checkpointDigest = checkpointDigest;
  if (!m_decodeStateStore.erase(staged.sessionId, staged.role.role)) {
    // Keep the conversation store fail-closed if ownership cannot be removed
    // from the request-local store. Normal runtime operation is serialized per
    // role, so this remains a defensive rollback path.
    m_conversationStateStore->release(committedBinding);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stagedConversationPromotions.erase(key);
    }
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stagedConversationPromotions.erase(key);
    m_sessionRunners.erase(sessionRunnerKey(
      staged.sessionId, staged.role.role,
      staged.role.candidateDecodeStateIdentity->attemptEpoch));
  }
  m_conversationStateStore->noteRequestLocalRelease();
  (void)m_conversationStateStore->finalizeSaveEligibility(committedBinding);
  return true;
}

bool
NativeProviderRuntime::rollbackStagedDecodeStatePromotion(
  const ConversationStateBinding& binding)
{
  if (!m_conversationStateStore->rollbackStagedPromotion(binding)) {
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
  return m_conversationStateStore->lookup(binding, nowMs);
}

std::optional<ConversationStateBinding>
NativeProviderRuntime::resolveConversationState(
  const ConversationStateReferenceV1& reference,
  std::uint64_t nowMs, const std::string& authenticatedRequester)
{
  return m_conversationStateStore->resolve(reference, nowMs, authenticatedRequester);
}

std::optional<ConversationStateBinding>
NativeProviderRuntime::resolveStagedConversationState(
  const ConversationStateReferenceV1& reference,
  std::uint64_t nowMs, const std::string& authenticatedRequester)
{
  return m_conversationStateStore->resolveStaged(reference, nowMs, authenticatedRequester);
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

std::string
NativeProviderRuntime::sessionRunnerKey(const std::string& sessionId,
                                        const std::string& role,
                                        std::uint64_t attemptEpoch)
{
  return sessionId + '\x1f' + role + '\x1f' +
         std::to_string(attemptEpoch);
}

bool
NativeProviderRuntime::pauseConversationStateToHost(
  const ConversationStateBinding& binding,
  std::uint64_t nowMs)
{
  return m_conversationStateStore->pauseToHost(binding, nowMs);
}

std::future<bool>
NativeProviderRuntime::prefetchConversationStateToGpu(
  const ConversationStateBinding& binding,
  std::uint64_t nowMs)
{
  return m_conversationStateStore->prefetchToGpu(binding, nowMs);
}

bool
NativeProviderRuntime::cancelConversationStatePrefetch(
  const ConversationStateBinding& binding)
{
  return m_conversationStateStore->cancelPrefetch(binding);
}

bool
NativeProviderRuntime::pinConversationState(
  const ConversationStateBinding& binding,
  std::uint64_t nowMs)
{
  return m_conversationStateStore->pin(binding, nowMs);
}

bool
NativeProviderRuntime::unpinConversationState(
  const ConversationStateBinding& binding)
{
  return m_conversationStateStore->unpin(binding);
}

bool
NativeProviderRuntime::releaseConversationState(
  const ConversationStateBinding& binding)
{
  return m_conversationStateStore->release(binding);
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
  std::vector<std::shared_ptr<NativeModelRunner>> runners;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto prefix = sessionId + '\x1f' + role + '\x1f';
    for (auto found = m_sessionRunners.begin();
         found != m_sessionRunners.end();) {
      if (found->first.compare(0, prefix.size(), prefix) == 0) {
        const auto duplicate = std::find_if(
          runners.begin(), runners.end(), [&] (const auto& runner) {
            return runner.get() == found->second.get();
          });
        if (duplicate == runners.end()) {
          runners.push_back(found->second);
        }
        found = m_sessionRunners.erase(found);
      }
      else {
        ++found;
      }
    }
    if (runners.empty()) {
      const auto found = m_runners.find(role);
      if (found != m_runners.end()) {
        runners.push_back(found->second);
      }
    }
  }
  const auto erased = m_decodeStateStore.erase(sessionId, role);
  for (const auto& runner : runners) {
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
