#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DecodeStateIdentity.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"

#include <ndn-cxx/name.hpp>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ndnsf::di {
namespace {
ndn_service_framework::NamingDigestField
stringField(std::uint16_t tag, std::uint8_t kind, const std::string& value)
{
  return {tag, kind, {value.begin(), value.end()}};
}

ndn_service_framework::NamingDigestField
uint64Field(std::uint16_t tag, std::uint64_t value)
{
  ndn_service_framework::NamingDigestField field{tag, 3, {}};
  field.value.reserve(8);
  for (std::size_t index = 8; index-- > 0;)
    field.value.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
  return field;
}

ndn_service_framework::NamingDigestField
digestField(std::uint16_t tag, const std::string& lowerHex)
{
  const auto component = ndn_service_framework::makeNamingDigestComponent(lowerHex);
  return {tag, 4, {component.value_begin(), component.value_end()}};
}
} // namespace

std::string
computeTensorExecutionObjectDigest(const NativeTensorEndpointV3& endpoint)
{
  detail::validateTensorEndpointForNaming(endpoint);
  auto fields = std::vector<ndn_service_framework::NamingDigestField>{
    stringField(1, 2, endpoint.tensorDigest), stringField(2, 2, endpoint.layoutDigest),
    stringField(3, 2, endpoint.targetLayoutDigest), stringField(4, 2, endpoint.manifestDigest),
    stringField(5, 2, endpoint.securityProfile),
    stringField(6, 5, nativeCanonicalJson(NativeJson(endpoint.bundleTensorNames)))};
  if (!endpoint.namingStepId.empty()) {
    // The N5 coordinates are source-side execution facts, so they belong to the object commitment. The phase is not
    // among them: it is derived from the round and the stride when the name is built.
    fields.push_back(stringField(7, 2, endpoint.namingStepId));
    fields.push_back(uint64Field(9, endpoint.namingLayerBegin));
    fields.push_back(uint64Field(10, endpoint.namingLayerEnd));
    fields.push_back(stringField(11, 2, endpoint.namingPortId));
  }
  if (endpoint.segmentManifest) {
    fields.push_back(stringField(12, 2, "segment-manifest-v1"));
    fields.push_back(uint64Field(13, endpoint.segmentManifest->sourceSegments));
    fields.push_back(uint64Field(14, endpoint.segmentManifest->segmentPayloadBytes));
  }
  return ndn_service_framework::computeNamingDigest("ndnsf-naming/tensor-object", fields);
}

std::string
computeTensorRepresentationCommitment(const NativeTensorSemanticFacts& facts)
{
  const auto text = [](std::uint16_t tag, const std::string& value) {
    return ndn_service_framework::NamingDigestField{tag, 2,
      std::vector<std::uint8_t>(value.begin(), value.end())};
  };
  std::vector<ndn_service_framework::NamingDigestField> fields{
    text(1, "ndnsf-di-tensor-v1"),
    text(2, facts.requestId),
    uint64Field(3, facts.attempt),
    uint64Field(4, facts.round),
    uint64Field(5, facts.index),
    text(6, facts.phase),
    text(7, facts.role),
    text(8, facts.step),
    uint64Field(9, facts.layerBegin),
    uint64Field(10, facts.layerEnd),
    text(11, facts.port),
    text(12, facts.operation),
    uint64Field(13, facts.rank),
    uint64Field(14, facts.microbatch),
    text(15, facts.tensorId),
    text(16, facts.tensorDigest),
    text(17, facts.layoutDigest),
    text(18, facts.targetLayoutDigest),
    text(19, facts.securityProfile),
    uint64Field(20, facts.segmentCount),
    text(21, facts.bundleTensorNamesDigest),
    uint64Field(22, facts.inlineFirstSegment)};
  if (!facts.namingCapabilityDigest.empty())
    fields.push_back(text(23, facts.namingCapabilityDigest));
  if (!facts.publicRequestId.empty())
    fields.push_back(text(24, facts.publicRequestId));
  if (facts.segmentManifest) {
    fields.push_back(text(25, "segment-manifest-v1"));
    fields.push_back(uint64Field(26, facts.segmentManifest->sourceSegments));
    fields.push_back(uint64Field(27, facts.segmentManifest->segmentPayloadBytes));
  }
  return ndn_service_framework::computeNamingDigest("ndnsf-naming/tensor-representation", fields);
}

std::uint64_t
tensorPublicationRound(const std::string& operation, std::uint64_t operationEpoch,
                       const std::string& namingCapabilityDigest)
{
  // Feedback is created before the epoch that consumes it. The capability
  // still authorizes the original packed operation at that consumption epoch.
  return !namingCapabilityDigest.empty() && operation == "TOKEN_FEEDBACK" && operationEpoch > 0
    ? operationEpoch - 1 : operationEpoch;
}

std::string
tensorObjectNamePrefix(const NativeTensorEndpointV3& endpoint)
{
  // The object commitment is no longer part of the name, so the endpoint is validated here directly.
  detail::validateTensorEndpointForNaming(endpoint);
  const bool input = endpoint.sourceKind == "APPLICATION_INPUT";
  const auto stride = endpoint.namingOperationStride;
  const auto round = input || stride == 0 ? 0 : endpoint.round / stride;
  const auto publicationRound = tensorPublicationRound(endpoint.operation, round,
                                                       endpoint.namingCapabilityDigest);
  const auto index = input ? 0 : stride == 0 ? endpoint.round : endpoint.round % stride;
  // The representation commits to what the object is, not to how it is transported or to the plan that scheduled it.
  NativeTensorSemanticFacts facts;
  facts.requestId = endpoint.requestId;
  facts.attempt = endpoint.attempt;
  facts.round = round;
  facts.index = index;
  facts.phase = input || stride == 0 ? "execute" :
    (publicationRound == 0 ? "prefill" : "continuation");
  facts.role = endpoint.producerRole;
  facts.step = endpoint.namingStepId;
  facts.layerBegin = endpoint.namingLayerBegin;
  facts.layerEnd = endpoint.namingLayerEnd;
  facts.port = endpoint.namingPortId;
  facts.operation = endpoint.operation;
  facts.rank = endpoint.producerRank;
  facts.microbatch = endpoint.microbatch;
  facts.tensorId = endpoint.tensorId;
  facts.tensorDigest = endpoint.tensorDigest;
  facts.layoutDigest = endpoint.layoutDigest;
  facts.targetLayoutDigest = endpoint.targetLayoutDigest;
  facts.securityProfile = endpoint.securityProfile;
  facts.segmentCount = endpoint.segmentCount;
  const auto bundle = nativeCanonicalJson(NativeJson(endpoint.bundleTensorNames));
  facts.bundleTensorNamesDigest = ndn_service_framework::computeNamingDigest(
    "ndnsf-naming/tensor-bundle",
    {{1, 5, std::vector<std::uint8_t>(bundle.begin(), bundle.end())}});
  facts.inlineFirstSegment = endpoint.inlineFirstSegment ? 1 : 0;
  facts.segmentManifest = endpoint.segmentManifest;
  facts.namingCapabilityDigest = endpoint.namingCapabilityDigest;
  facts.publicRequestId = endpoint.publicRequestId;
  const auto key = computeTensorRepresentationCommitment(facts);
  auto name = ndn_service_framework::makeProducerDataPrefix(
    ndn::Name(endpoint.producerNamespace), "NDNSF-DI", "tensor");
  if (!endpoint.namingStepId.empty()) {
    // N5 fixed schema. The packed operation index remains bound in the step/metadata and in the object
    // commitment above; it is not a separate name component. The phase names the epoch that produced the object:
    // a generation plan's first epoch is prefill and every later one is continuation, a non-generation plan is
    // execute. Deriving it here means no cached or rebound projection can disagree about it.
    const auto label = [](const std::string& value) {
      return ndn::name::Component(value);
    };
    name.append("request").append(endpoint.publicRequestId.empty() ? label(endpoint.requestId) :
      ndn_service_framework::makePublicNamingLabelComponent(endpoint.publicRequestId));
    name.append("attempt").append(ndn_service_framework::makeNamingDecimalComponent(endpoint.attempt));
    name.append("round").append(ndn_service_framework::makeNamingDecimalComponent(publicationRound));
    name.append("phase").append(facts.phase);
    name.append("role").append(label(endpoint.producerRole));
    name.append("step").append(label(endpoint.namingStepId));
    name.append("layers");
    if (endpoint.namingLayerEnd == 0) {
      name.append("none").append("none");
    }
    else {
      name.append(ndn_service_framework::makeNamingDecimalComponent(endpoint.namingLayerBegin));
      name.append(ndn_service_framework::makeNamingDecimalComponent(endpoint.namingLayerEnd));
    }
    name.append("op").append(ndn::name::Component(ndn::tlv::GenericNameComponent,
      endpoint.operation.begin(), endpoint.operation.end()));
    name.append("rank").append(ndn_service_framework::makeNamingDecimalComponent(endpoint.producerRank));
    name.append("batch").append(ndn_service_framework::makeNamingDecimalComponent(endpoint.microbatch));
    name.append("port").append(label(endpoint.namingPortId));
    name.append("rep").append(ndn_service_framework::makeNamingDigestComponent(key));
    ndn_service_framework::checkNamingNameSize(name);
    return name.toUri(ndn::name::UriFormat::CANONICAL);
  }
  name.append("REQUEST").append(ndn_service_framework::makeNamingLabelComponent(
    ndn::Name(endpoint.requestId).toUri(ndn::name::UriFormat::CANONICAL)));
  name.append("ATTEMPT").append(ndn_service_framework::makeNamingDecimalComponent(endpoint.attempt));
  name.append("ROUND").append(ndn_service_framework::makeNamingDecimalComponent(round));
  name.append("POSITION").append(ndn_service_framework::makeNamingLabelComponent(
    input ? "model-input" : endpoint.producerRole));
  name.append("OP").append(ndn::name::Component(ndn::tlv::GenericNameComponent,
    endpoint.operation.begin(), endpoint.operation.end()));
  name.append("INDEX").append(ndn_service_framework::makeNamingDecimalComponent(index));
  name.append("RANK").append(ndn_service_framework::makeNamingDecimalComponent(endpoint.producerRank));
  name.append("BATCH").append(ndn_service_framework::makeNamingDecimalComponent(endpoint.microbatch));
  name.append(ndn_service_framework::makeNamingLabelComponent(endpoint.tensorId));
  name.append(ndn_service_framework::makeNamingDigestComponent(key));
  ndn_service_framework::checkNamingNameSize(name);
  return name.toUri(ndn::name::UriFormat::CANONICAL);
}

std::string
tensorObjectManifestName(const NativeTensorEndpointV3& endpoint)
{
  auto name = ndn::Name(tensorObjectNamePrefix(endpoint)).append("MANIFEST");
  ndn_service_framework::checkNamingNameSize(name);
  return name.toUri(ndn::name::UriFormat::CANONICAL);
}

ndn::Name
makeTensorSegmentDataName(const ndn::Name& objectPrefix, std::uint64_t segmentNo)
{
  auto name = ndn::Name(objectPrefix).append("DATA").appendSegment(segmentNo);
  ndn_service_framework::checkNamingNameSize(name);
  return name;
}

std::string
tensorObjectSegmentName(const NativeTensorEndpointV3& endpoint,
                        std::size_t segmentNo)
{
  if (segmentNo >= endpoint.segmentCount)
    throw std::out_of_range("tensor segment is outside the declared endpoint");
  return makeTensorSegmentDataName(ndn::Name(tensorObjectNamePrefix(endpoint)), segmentNo)
    .toUri(ndn::name::UriFormat::CANONICAL);
}

DiReservationAuthority::DiReservationAuthority(
  std::string providerBootId,
  DiReservationPolicy policy)
  : m_providerBootId(std::move(providerBootId))
  , m_policy(std::move(policy))
{
  if (m_providerBootId.empty() || m_policy.globalLimit == 0 ||
      m_policy.requesterLimit == 0 ||
      m_policy.serviceLimit == 0 || m_policy.tentativeLeaseMs == 0) {
    throw std::invalid_argument("DI reservation limits and lifetime must be positive");
  }
}

DiReservationAuthority::~DiReservationAuthority()
{
  releaseAll(static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()));
}

std::string
DiReservationAuthority::keyFor(const DiReservationRequest& request) const
{
  return request.providerName + '\n' + request.requesterName + '\n' +
         request.serviceName + '\n' + request.requestId;
}

bool
DiReservationAuthority::withinQuota(const DiReservationRequest& request,
                                    std::uint64_t nowMs)
{
  std::size_t global = 0;
  std::size_t requester = 0;
  std::size_t service = 0;
  for (const auto& item : m_records) {
    const auto& lease = item.second.lease;
    if (lease.state == DiReservationState::Tentative && nowMs >= lease.expiresAtMs) {
      continue;
    }
    if (lease.state != DiReservationState::Tentative &&
        lease.state != DiReservationState::Committed) continue;
    ++global;
    requester += lease.requesterName == request.requesterName;
    service += lease.serviceName == request.serviceName;
  }
  return global < m_policy.globalLimit &&
         requester < m_policy.requesterLimit &&
         service < m_policy.serviceLimit;
}

DiReservationResult
DiReservationAuthority::reserve(const DiReservationRequest& request,
                                std::uint64_t nowMs)
{
  DiReservationResult rejected;
  if (!request.authorized) {
    rejected.reasonCode = "AUTHORIZATION_FAILED";
    return rejected;
  }
  std::lock_guard<std::mutex> lock(m_reservationMutex);
  const auto key = keyFor(request);
  auto prior = m_records.find(key);
  std::uint64_t expiresAtMs = nowMs + m_policy.tentativeLeaseMs;
  if (prior != m_records.end()) {
    auto& lease = prior->second.lease;
    if (lease.state == DiReservationState::Tentative && nowMs < lease.expiresAtMs) {
      return {true, "OK", lease, true}; // duplicate ACK never extends lease
    }
    rejected.reasonCode = "RESERVATION_NOT_LIVE";
    return rejected;
  }
  else if (!withinQuota(request, nowMs)) {
    rejected.reasonCode = "LEASE_CAPACITY_REJECTED";
    return rejected;
  }
  DiReservationLease lease;
  lease.reservationId = m_providerBootId + "-reservation-" +
                        std::to_string(m_nextReservationId++);
  lease.providerBootId = m_providerBootId;
  lease.providerName = request.providerName;
  lease.requesterName = request.requesterName;
  lease.requestId = request.requestId;
  lease.serviceName = request.serviceName;
  lease.planDigest = request.planDigest;
  lease.resourceBindingProof = request.resourceBindingProof;
  lease.conflictKeys = request.conflictKeys;
  lease.expiresAtMs = expiresAtMs;
  m_records.emplace(key, Record{lease, {}});
  m_keyByReservation.emplace(lease.reservationId, key);
  return {true, "OK", lease, false};
}

DiReservationResult
DiReservationAuthority::commit(const std::string& reservationId,
                               std::uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_reservationMutex);
  auto key = m_keyByReservation.find(reservationId);
  if (key == m_keyByReservation.end()) return {false, "RESERVATION_NOT_FOUND", {}, false};
  auto& lease = m_records.at(key->second).lease;
  if (lease.state == DiReservationState::Committed) return {true, "OK", lease, true};
  if (lease.state != DiReservationState::Tentative || nowMs >= lease.expiresAtMs) {
    if (lease.state == DiReservationState::Tentative) lease.state = DiReservationState::Expired;
    return {false, "RESERVATION_EXPIRED", lease, false};
  }
  lease.state = DiReservationState::Committed;
  return {true, "OK", lease, false};
}

bool
DiReservationAuthority::release(const std::string& reservationId,
                                const std::string& cause,
                                std::uint64_t nowMs)
{
  (void)nowMs;
  std::lock_guard<std::mutex> lock(m_reservationMutex);
  auto key = m_keyByReservation.find(reservationId);
  if (key == m_keyByReservation.end()) return false;
  auto& record = m_records.at(key->second);
  if (record.lease.state == DiReservationState::Released ||
      record.lease.state == DiReservationState::Expired) return true;
  record.lease.state = DiReservationState::Released;
  record.releaseCause = cause;
  return true;
}

std::size_t
DiReservationAuthority::cleanupExpired(std::uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_reservationMutex);
  std::size_t count = 0;
  for (auto& item : m_records) {
    auto& lease = item.second.lease;
    if (lease.state == DiReservationState::Tentative && nowMs >= lease.expiresAtMs) {
      lease.state = DiReservationState::Expired;
      item.second.releaseCause = "TENTATIVE_TIMEOUT";
      ++count;
    }
  }
  return count;
}

void
DiReservationAuthority::releaseAll(std::uint64_t nowMs, const std::string& cause)
{
  std::lock_guard<std::mutex> lock(m_reservationMutex);
  for (auto& item : m_records) {
    if (item.second.lease.state == DiReservationState::Tentative ||
        item.second.lease.state == DiReservationState::Committed) {
      item.second.lease.state = DiReservationState::Released;
      item.second.releaseCause = cause;
    }
  }
}

void
ExecutionAttemptKey::validate() const
{
  if (requestId.empty() || attemptEpoch == 0) {
    throw std::invalid_argument(
      "execution attempt requires requestId and positive attemptEpoch");
  }
}

std::string
ExecutionAttemptKey::scopedSessionId() const
{
  validate();
  // Keep the attempt discriminator as ordinary generic name components.
  // `attempt=<epoch>` is interpreted by ndn-cxx as typed-component URI
  // syntax, and `attempt` is not a registered component type.
  return trimSlashes(requestId) + "/attempt/" + std::to_string(attemptEpoch);
}

std::map<std::string, std::string>
ExecutionAttemptKey::assignmentFields() const
{
  validate();
  return {
    {"executionRequestId", requestId},
    {"executionAttemptEpoch", std::to_string(attemptEpoch)},
  };
}

bool
ExecutionAttemptKey::operator==(const ExecutionAttemptKey& other) const noexcept
{
  return requestId == other.requestId && attemptEpoch == other.attemptEpoch;
}

const char*
toString(ExecutionAttemptAdmission admission) noexcept
{
  switch (admission) {
  case ExecutionAttemptAdmission::Accepted: return "ACCEPTED";
  case ExecutionAttemptAdmission::Stale: return "STALE";
  case ExecutionAttemptAdmission::Cancelled: return "CANCELLED";
  case ExecutionAttemptAdmission::DuplicateTerminal: return "DUPLICATE_TERMINAL";
  }
  return "STALE";
}

std::ostream&
operator<<(std::ostream& os, ExecutionAttemptAdmission admission)
{
  return os << toString(admission);
}

ExecutionAttemptAdmission
ExecutionAttemptAuthority::admit(const ExecutionAttemptKey& key)
{
  return admit(key, {});
}

ExecutionAttemptAdmission
ExecutionAttemptAuthority::admit(const ExecutionAttemptKey& key, std::string controlScope)
{
  key.validate();
  std::lock_guard<std::mutex> lock(m_mutex);
  auto& state = m_states[key.requestId];
  if (key.attemptEpoch < state.currentEpoch) {
    return ExecutionAttemptAdmission::Stale;
  }
  if (key.attemptEpoch > state.currentEpoch) {
    state = State{key.attemptEpoch, false, false, std::move(controlScope), {}};
    return ExecutionAttemptAdmission::Accepted;
  }
  if (state.terminal) {
    return ExecutionAttemptAdmission::DuplicateTerminal;
  }
  if (state.cancelled) {
    return ExecutionAttemptAdmission::Cancelled;
  }
  if (!controlScope.empty()) {
    if (state.controlScope.empty()) {
      state.controlScope = std::move(controlScope);
    }
    else if (state.controlScope != controlScope) {
      return ExecutionAttemptAdmission::Stale;
    }
  }
  return ExecutionAttemptAdmission::Accepted;
}

bool
ExecutionAttemptAuthority::cancel(const ExecutionAttemptKey& key)
{
  key.validate();
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_states.find(key.requestId);
  if (found == m_states.end() || found->second.currentEpoch != key.attemptEpoch ||
      found->second.terminal || found->second.cancelled) {
    return false;
  }
  found->second.cancelled = true;
  return true;
}

bool
ExecutionAttemptAuthority::cancel(const ExecutionAttemptKey& key, const std::string& controlScope,
                                  const std::string& controlDigest)
{
  key.validate();
  std::lock_guard lock(m_mutex);
  const auto found = m_states.find(key.requestId);
  if (found == m_states.end() || found->second.currentEpoch != key.attemptEpoch ||
      found->second.terminal || controlScope.empty() != controlDigest.empty() ||
      found->second.controlScope != controlScope) {
    return false;
  }
  auto& state = found->second;
  if (state.cancelled) {
    return !controlDigest.empty() && state.cancellationDigest == controlDigest;
  }
  state.cancellationDigest = controlDigest;
  state.cancelled = true;
  return true;
}

bool
ExecutionAttemptAuthority::supersedeLegacy(const ExecutionAttemptKey& key,
                                           std::uint64_t replacementEpoch)
{
  key.validate();
  std::lock_guard lock(m_mutex);
  const auto found = m_states.find(key.requestId);
  if (found == m_states.end() || found->second.currentEpoch != key.attemptEpoch ||
      !found->second.controlScope.empty() || found->second.terminal ||
      replacementEpoch <= key.attemptEpoch) {
    return false;
  }
  found->second = State{replacementEpoch, false, false, {}, {}};
  return true;
}

bool
ExecutionAttemptAuthority::complete(const ExecutionAttemptKey& key)
{
  key.validate();
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_states.find(key.requestId);
  if (found == m_states.end() || found->second.currentEpoch != key.attemptEpoch ||
      found->second.terminal || found->second.cancelled) {
    return false;
  }
  found->second.terminal = true;
  return true;
}

bool
ExecutionAttemptAuthority::isAuthoritative(const ExecutionAttemptKey& key) const
{
  if (key.requestId.empty() || key.attemptEpoch == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_states.find(key.requestId);
  return found != m_states.end() &&
         found->second.currentEpoch == key.attemptEpoch &&
         !found->second.cancelled && !found->second.terminal;
}

NativeDependencySpec::NativeDependencySpec(std::vector<std::string> producers,
                                           std::vector<std::string> consumers,
                                           std::string keyScope,
                                           std::string topicPrefix,
                                           std::string objectNameTemplate,
                                           std::size_t expectedSegments,
                                           std::size_t expectedBytes,
                                           std::vector<std::string> tensors)
  : producers(std::move(producers))
  , consumers(std::move(consumers))
  , keyScope(std::move(keyScope))
  , topicPrefix(std::move(topicPrefix))
  , objectNameTemplate(std::move(objectNameTemplate))
  , expectedSegments(expectedSegments)
  , expectedBytes(expectedBytes)
  , tensors(std::move(tensors))
{
}

namespace {

bool
isStepOperation(const std::string& operation)
{
  return operation == "CALL_SUBGRAPH" || operation == "SUM";
}

bool
isSlotSource(const std::string& source)
{
  return source == "EPOCH_INPUT" || source == "TOKEN_FEEDBACK" ||
         source == "LOCAL_STEP" || source == "REMOTE_STEP" ||
         source == "LOCAL_STATE";
}

bool
isDependencySlotSource(const std::string& source)
{
  return source == "LOCAL_STEP" || source == "REMOTE_STEP";
}

} // namespace

void
NativeStepSlotSpec::validate() const
{
  if (slotId.empty() || dtype.empty() || layoutDigest.empty() || !isSlotSource(source)) {
    throw std::invalid_argument("execution step slot identity is incomplete");
  }
  for (const auto& dim : shape) {
    if (const auto* text = std::get_if<std::string>(&dim)) {
      if (text->empty()) throw std::invalid_argument("execution step slot has an empty symbol");
    }
    else if (std::get<std::int64_t>(dim) <= 0) {
      throw std::invalid_argument("execution step slot has a non-positive dimension");
    }
  }
  // A step-bound slot must name the producing output slot as well as the
  // producing step: the consumer's own input name is step-local and cannot be
  // used to find the producer's output.
  if (!producerStepId.empty() != !producerSlotId.empty()) {
    throw std::invalid_argument(
      "execution step slot names a producer step without its output slot");
  }
}

void
NativeExecutionStepSpec::validate() const
{
  if (stepId.empty() || unitId.empty() || !isStepOperation(operation)) {
    throw std::invalid_argument("execution step identity or operation is invalid");
  }
  if ((sourceLayers.first != 0 || sourceLayers.second != 0) &&
      sourceLayers.first >= sourceLayers.second)
    throw std::invalid_argument("execution step has an invalid source layer interval");
  // SUM is a local reduction over admitted contributions: it has no subgraph,
  // so it must not fabricate an artifact entry or an empty ONNX graph.
  if ((operation == "CALL_SUBGRAPH") != !artifactEntry.empty()) {
    throw std::invalid_argument("execution step artifact entry disagrees with its operation");
  }
  if (!onnxNodeIndices.empty() &&
      (operation != "CALL_SUBGRAPH" || !localStateSlots.empty() ||
       inputSlots.empty() || outputSlots.empty() ||
       std::adjacent_find(onnxNodeIndices.begin(), onnxNodeIndices.end(),
                          std::greater_equal<std::uint64_t>{}) != onnxNodeIndices.end())) {
    throw std::invalid_argument("execution step has an invalid canonical ONNX node selection");
  }
  std::set<std::string> inputIds;
  for (const auto& slot : inputSlots) {
    slot.validate();
    if (!inputIds.insert(slot.slotId).second) {
      throw std::invalid_argument("execution step has duplicate input slots");
    }
    // An input fed by another step must name it, so a dependency slot can never
    // be satisfied by an unbound local placeholder; the inverse keeps a slot
    // that claims a producer from being read as an epoch input.
    if (isDependencySlotSource(slot.source) != !slot.producerStepId.empty()) {
      throw std::invalid_argument("execution step input slot producer binding disagrees with its source");
    }
    if (slot.producerStepId == stepId) {
      throw std::invalid_argument("execution step consumes its own output");
    }
  }
  std::set<std::string> outputIds;
  for (const auto& slot : outputSlots) {
    slot.validate();
    if (inputIds.count(slot.slotId) != 0 || !outputIds.insert(slot.slotId).second) {
      throw std::invalid_argument("execution step output slot is missing or ambiguous");
    }
    if (!slot.producerStepId.empty()) {
      throw std::invalid_argument("execution step output slot names a foreign producer");
    }
  }
  std::set<std::string> stateIds;
  for (const auto& name : localStateSlots) {
    if (name.empty() || !stateIds.insert(name).second) {
      throw std::invalid_argument("execution step local state slot is empty or duplicated");
    }
  }
}

namespace {

/** Element width of a fixed-width slot dtype. Anything else is unknown rather
 * than assumed to be four bytes.
 */
std::optional<std::uint64_t>
slotDtypeBytes(const std::string& dtype)
{
  if (dtype == "float32" || dtype == "int32") return 4;
  if (dtype == "float16" || dtype == "bfloat16") return 2;
  if (dtype == "int64") return 8;
  if (dtype == "bool" || dtype == "uint8" || dtype == "int8") return 1;
  return std::nullopt;
}

} // namespace

std::uint64_t
NativeExecutionPlan::degreeForRole(const std::string& role) const
{
  for (const auto& stage : stageLayout) {
    for (const auto& unit : stage.unitRoleIds) {
      if (unit == role)
        return stage.degree();
    }
  }
  return tensorDegree;
}

NativeStepCommunicationCost
nativeStepCommunicationCost(const NativeExecutionPlan& plan)
{
  NativeStepCommunicationCost cost;
  // A remote input is carried once per consuming step, which is exactly one
  // network transfer per contributing unit per reduction. Local slots are
  // in-process and cost nothing on the wire.
  for (const auto& step : plan.steps) {
    for (const auto& slot : step.inputSlots) {
      if (slot.source != "REMOTE_STEP") continue;
      ++cost.remoteEdges;
      const auto elementBytes = slotDtypeBytes(slot.dtype);
      if (!elementBytes) {
        ++cost.unknownTransferTensors;
        continue;
      }
      std::uint64_t bytes = *elementBytes;
      bool known = true;
      for (const auto& dimension : slot.shape) {
        const auto* value = std::get_if<std::int64_t>(&dimension);
        // A symbolic dimension means the real payload depends on a runtime
        // value, so the tensor is reported as unknown instead of guessed.
        if (value == nullptr || *value <= 0 ||
            bytes > std::numeric_limits<std::uint64_t>::max() /
                      static_cast<std::uint64_t>(*value)) {
          known = false;
          break;
        }
        bytes *= static_cast<std::uint64_t>(*value);
      }
      if (!known) {
        ++cost.unknownTransferTensors;
        continue;
      }
      if (bytes > std::numeric_limits<std::uint64_t>::max() - cost.knownTransferBytes) {
        throw std::overflow_error("step communication cost exceeds uint64");
      }
      cost.knownTransferBytes += bytes;
    }
  }
  return cost;
}

void
NativeStageLayout::validate() const
{
  if (stageId.empty())
    throw std::invalid_argument("stage layout entry has an empty stage id");
  if (unitRoleIds.empty())
    throw std::invalid_argument("stage layout entry has no units");
  for (const auto& unit : unitRoleIds)
    if (unit.empty())
      throw std::invalid_argument("stage layout entry declares an empty unit id");
}

std::string
NativeStageLayout::canonicalJson() const
{
  validate();
  NativeJson value{{"stage_id", stageId}, {"source_region_digest", sourceRegionDigest},
                   {"unit_role_ids", unitRoleIds}};
  return value.dump();
}

bool
isSharedTensorPartial(const NativeDependencySpec& dependency) noexcept
{
  return dependency.operationKind == "TENSOR_PARTIAL" && dependency.consumers.size() > 1;
}

std::pair<std::string_view, std::string_view>
findSharedPartialInput(const NativeExecutionPlan& plan, const NativeDependencySpec& dependency,
                       std::string_view consumerRole)
{
  if (!isSharedTensorPartial(dependency) ||
      std::find(dependency.consumers.begin(), dependency.consumers.end(), consumerRole) ==
        dependency.consumers.end()) {
    throw std::invalid_argument("shared TP partial names an unauthorized consumer");
  }
  const auto producer = std::find_if(plan.steps.begin(), plan.steps.end(),
    [&] (const auto& step) { return step.stepId == dependency.producerStepId; });
  if (dependency.producers.size() != 1 || producer == plan.steps.end() ||
      producer->unitId != dependency.producers.front() || producer->unitId == consumerRole ||
      producer->operation != "CALL_SUBGRAPH") {
    throw std::invalid_argument("shared TP partial has an invalid producing step");
  }
  const auto output = std::find_if(producer->outputSlots.begin(), producer->outputSlots.end(),
    [&] (const auto& slot) { return slot.slotId == dependency.producerSlotId; });
  if (output == producer->outputSlots.end()) {
    throw std::invalid_argument("shared TP partial has an undeclared output slot");
  }
  std::pair<std::string_view, std::string_view> result;
  for (const auto& step : plan.steps) {
    if (step.unitId != consumerRole || step.operation != "SUM") continue;
    for (const auto& input : step.inputSlots) {
      if (input.source != "REMOTE_STEP" || input.producerStepId != producer->stepId ||
          input.producerSlotId != output->slotId) continue;
      if (!result.first.empty() || input.dtype != output->dtype ||
          input.shape != output->shape || input.layoutDigest != output->layoutDigest) {
        throw std::invalid_argument("shared TP partial SUM input is ambiguous or mismatched");
      }
      result = {step.stepId, input.slotId};
    }
  }
  if (result.first.empty()) {
    throw std::invalid_argument("shared TP partial has no sealed SUM input");
  }
  return result;
}

std::optional<NativeExecutionPlan>
makeReceivingStageReduction(const NativeExecutionPlan& plan, std::string_view resultRole)
{
  if (plan.stepPlanVersion != NATIVE_STEP_PLAN_VERSION_V2 || plan.stageLayout.size() != 2)
    return std::nullopt;
  const auto& ranks = plan.stageLayout.front().unitRoleIds;
  const auto& receiver = plan.stageLayout.back().unitRoleIds;
  if (ranks.size() != 2 || receiver.size() != 1 || receiver.front() != resultRole)
    return std::nullopt;
  plan.validateSteps();

  const auto findStep = [&](const std::string& id) {
    return std::find_if(plan.steps.begin(), plan.steps.end(),
      [&](const auto& step) { return step.stepId == id; });
  };
  const NativeDependencySpec* boundary = nullptr;
  for (const auto& edge : plan.dependencies) {
    if (edge.consumers != receiver || edge.producers.size() != 1 ||
        std::find(ranks.begin(), ranks.end(), edge.producers.front()) == ranks.end() ||
        edge.operationKind != "ACTIVATION" || !edge.useNdnsfDataV1 ||
        edge.tensors.size() != 1 || !edge.redistributions.empty()) continue;
    const auto producer = findStep(edge.producerStepId);
    if (producer == plan.steps.end() || producer->operation != "SUM") continue;
    boundary = &edge;
    break;
  }
  if (!boundary || boundary->dependencyId.empty()) return std::nullopt;
  const auto original = findStep(boundary->producerStepId);
  const auto downstream = findStep(boundary->consumerStepId);
  if (downstream == plan.steps.end() || downstream->operation != "CALL_SUBGRAPH" ||
      original->inputSlots.size() != ranks.size() || original->outputSlots.size() != 1)
    return std::nullopt;
  if (std::count_if(plan.steps.begin(), plan.steps.end(), [&](const auto& step) {
        return step.unitId == receiver.front() && step.operation == "CALL_SUBGRAPH";
      }) != 1) return std::nullopt;
  const auto port = std::find_if(downstream->inputSlots.begin(), downstream->inputSlots.end(),
    [&](const auto& slot) { return slot.slotId == boundary->consumerSlotId; });
  if (port == downstream->inputSlots.end() || port->source != "REMOTE_STEP" ||
      port->dtype != "float32")
    return std::nullopt;

  using Port = std::pair<std::string, std::string>;
  const auto operands = [](const NativeExecutionStepSpec& step) {
    std::set<Port> result;
    for (const auto& slot : step.inputSlots)
      result.emplace(slot.producerStepId, slot.producerSlotId);
    return result;
  };
  const auto required = operands(*original);
  const auto sameTensor = [](const auto& a, const auto& b) {
    return a.dtype == b.dtype && a.shape == b.shape && a.layoutDigest == b.layoutDigest;
  };
  if (required.size() != ranks.size() || !sameTensor(original->outputSlots.front(), *port))
    return std::nullopt;
  std::set<std::string> upstreamSums;
  for (const auto& step : plan.steps) {
    if (step.operation == "SUM" &&
        std::find(ranks.begin(), ranks.end(), step.unitId) != ranks.end() &&
        step.inputSlots.size() == ranks.size() && step.outputSlots.size() == 1 &&
        sameTensor(step.outputSlots.front(), *port) && operands(step) == required) {
      upstreamSums.insert(step.stepId);
    }
  }
  auto removed = upstreamSums;
  for (const auto& step : plan.steps) {
    for (const auto& slot : step.inputSlots) {
      if (upstreamSums.count(slot.producerStepId) == 0) continue;
      if (std::find(ranks.begin(), ranks.end(), step.unitId) != ranks.end()) {
        removed.erase(slot.producerStepId);
        continue;
      }
      if (step.stepId != downstream->stepId || slot.slotId != port->slotId)
        return std::nullopt;
    }
  }

  NativeExecutionStepSpec sum;
  sum.stepId = downstream->stepId + "/sum/" + port->slotId;
  if (findStep(sum.stepId) != plan.steps.end()) return std::nullopt;
  sum.unitId = receiver.front();
  sum.operation = "SUM";
  sum.sourceLayers = original->sourceLayers;
  sum.outputSlots = original->outputSlots;
  sum.outputSlots.front().slotId = port->slotId;
  std::vector<NativeDependencySpec> contributions;
  std::set<std::size_t> replacedRaw;
  for (std::size_t rank = 0; rank < ranks.size(); ++rank) {
    const auto& input = original->inputSlots[rank];
    const auto producer = findStep(input.producerStepId);
    if (producer == plan.steps.end() || producer->unitId != ranks[rank] ||
        producer->operation != "CALL_SUBGRAPH" || !sameTensor(input, *port))
      return std::nullopt;
    const auto fromOperand = [&](const auto& edge) {
      return edge.producerStepId == input.producerStepId &&
             edge.producerSlotId == input.producerSlotId;
    };
    const auto raw = std::find_if(plan.dependencies.begin(), plan.dependencies.end(), fromOperand);
    if (raw != plan.dependencies.end() &&
        (raw->operationKind != "TENSOR_PARTIAL" || !raw->useNdnsfDataV1 ||
         raw->tensors != boundary->tensors || raw->consumers.size() != 1 ||
         !raw->redistributions.empty()))
      return std::nullopt;
    if (raw != plan.dependencies.end() &&
        (upstreamSums.count(raw->consumerStepId) == 0 ||
         std::any_of(std::next(raw), plan.dependencies.end(), fromOperand)))
      return std::nullopt;
    auto& slot = sum.inputSlots.emplace_back(input);
    slot.source = "REMOTE_STEP";
    slot.slotId = "rank-" + std::to_string(rank);
    // A rank with only a local operand in the old DAG had no outgoing raw edge.
    // The old cut supplies its DATA_V1 contract for the same typed graph value.
    auto& edge = contributions.emplace_back(raw == plan.dependencies.end() ? *boundary : *raw);
    edge.operationKind = "TENSOR_PARTIAL";
    edge.producers = {producer->unitId};
    edge.producerStepId = input.producerStepId;
    edge.producerSlotId = input.producerSlotId;
    edge.keyScope = boundary->keyScope + "-partial-" + std::to_string(rank);
    edge.dependencyId = edge.keyScope;
    if (std::any_of(plan.dependencies.begin(), plan.dependencies.end(), [&](const auto& existing) {
          return existing.dependencyId == edge.dependencyId || existing.keyScope == edge.keyScope;
        })) return std::nullopt;
    if (raw != plan.dependencies.end() && removed.count(raw->consumerStepId) == 0) {
      replacedRaw.insert(static_cast<std::size_t>(std::distance(plan.dependencies.begin(), raw)));
      edge.consumers.push_back(receiver.front());
      std::sort(edge.consumers.begin(), edge.consumers.end());
      edge.consumerStepId.clear();
      edge.consumerSlotId.clear();
    }
    else {
      edge.consumers = receiver;
      edge.consumerStepId = sum.stepId;
      edge.consumerSlotId = slot.slotId;
    }
  }

  NativeExecutionPlan result = plan;
  const auto downstreamIndex = std::distance(plan.steps.begin(), downstream);
  const auto portIndex = std::distance(downstream->inputSlots.begin(), port);
  auto& rebound = result.steps[downstreamIndex].inputSlots[portIndex];
  rebound.source = "LOCAL_STEP";
  rebound.producerStepId = sum.stepId;
  rebound.producerSlotId = sum.outputSlots.front().slotId;
  result.steps.erase(std::remove_if(result.steps.begin(), result.steps.end(),
    [&](const auto& step) { return removed.count(step.stepId) != 0; }), result.steps.end());
  const auto next = std::find_if(result.steps.begin(), result.steps.end(),
    [&](const auto& step) { return step.stepId == downstream->stepId; });
  result.steps.insert(next, std::move(sum));
  result.dependencies.clear();
  for (std::size_t index = 0; index < plan.dependencies.size(); ++index) {
    const auto& edge = plan.dependencies[index];
    if (edge.dependencyId != boundary->dependencyId &&
        removed.count(edge.consumerStepId) == 0 && replacedRaw.count(index) == 0) {
      result.dependencies.push_back(edge);
    }
  }
  result.dependencies.insert(result.dependencies.end(),
                             contributions.begin(), contributions.end());
  result.validateSteps();
  return result;
}

void
NativeExecutionPlan::validateSteps() const
{
  const std::set<std::string> roleSet(roles.begin(), roles.end());
  if (stepPlanVersion.empty()) {
    if (!steps.empty() ||
        std::any_of(dependencies.begin(), dependencies.end(), isSharedTensorPartial)) {
      throw std::invalid_argument("execution plan carries step bindings without a step plan version");
    }
    if (tensorDegree != 1) {
      throw std::invalid_argument("execution plan tensor degree requires a step plan");
    }
    if (!stageLayout.empty()) {
      throw std::invalid_argument("execution plan stage layout requires a step plan version");
    }
    return;
  }
  const bool stepPlanV2 = stepPlanVersion == NATIVE_STEP_PLAN_VERSION_V2;
  if (!stepPlanV2 && stepPlanVersion != NATIVE_STEP_PLAN_VERSION_V1) {
    throw std::invalid_argument("execution plan declares an unsupported step plan version: " +
                                stepPlanVersion);
  }
  if (steps.empty()) {
    throw std::invalid_argument("execution plan declares an unsupported or empty step plan");
  }
  // v1 was sealed for tensor-parallel plans only, so a single unit could never be
  // described. v2 admits degree-one stages, which is what lets one plan mix a sharded
  // stage with a plain one.
  if (!stepPlanV2 && tensorDegree < 2) {
    throw std::invalid_argument("execution plan step template requires tensor degree >= 2");
  }
  if (stepPlanV2 && tensorDegree < 1) {
    throw std::invalid_argument("execution plan step template requires tensor degree >= 1");
  }
  // v2 exists to describe per-stage geometry. Without a layout every unit would inherit the
  // plan-wide degree, which is exactly the defect the version was introduced to remove.
  if (stepPlanV2 && stageLayout.empty()) {
    throw std::invalid_argument("a v2 step plan must declare its stage layout");
  }
  if (!stageLayout.empty()) {
    if (!stepPlanV2) {
      throw std::invalid_argument("v1 step plan cannot carry a stage layout");
    }
    std::set<std::string> seenStages;
    std::set<std::string> seenUnits;
    std::uint64_t widest = 0;
    for (const auto& stage : stageLayout) {
      stage.validate();
      if (!seenStages.insert(stage.stageId).second)
        throw std::invalid_argument("execution plan declares a duplicate stage id: " + stage.stageId);
      for (const auto& unit : stage.unitRoleIds) {
        if (roleSet.count(unit) == 0)
          throw std::invalid_argument("stage layout names an undeclared unit: " + unit);
        if (!seenUnits.insert(unit).second)
          throw std::invalid_argument("stage layout repeats a unit across stages: " + unit);
      }
      widest = std::max<std::uint64_t>(widest, stage.degree());
    }
    if (seenUnits.size() != roleSet.size())
      throw std::invalid_argument("stage layout does not cover every declared unit");
    if (widest != tensorDegree) {
      throw std::invalid_argument(
        "execution plan tensor degree must equal the widest stage degree");
    }
  }
  if (roleSet.size() != roles.size()) {
    std::string detail;
    for (const auto& role : roles) {
      if (!detail.empty()) detail += ',';
      detail += role;
    }
    throw std::invalid_argument("execution plan declares duplicate units: " + detail);
  }

  const bool withOnnxCuts = std::any_of(steps.begin(), steps.end(), [](const auto& step) {
    return !step.onnxNodeIndices.empty();
  });
  std::set<std::uint64_t> onnxNodes;
  std::set<std::string> onnxEntries;
  std::map<std::string, const NativeExecutionStepSpec*> byId;
  for (const auto& step : steps) {
    step.validate();
    if (withOnnxCuts && step.operation == "CALL_SUBGRAPH") {
      if (!stepPlanV2 || step.onnxNodeIndices.empty() || !onnxEntries.insert(step.artifactEntry).second)
        throw std::invalid_argument("canonical ONNX step plan requires a distinct cut per CALL");
      for (const auto node : step.onnxNodeIndices) {
        if (!onnxNodes.insert(node).second)
          throw std::invalid_argument("canonical ONNX step plans cannot overlap node cuts");
      }
    }
    if (roleSet.count(step.unitId) == 0) {
      std::string detail = "execution step belongs to an undeclared unit: " + step.unitId +
        " declared={";
      std::size_t index = 0;
      for (const auto& role : roles) {
        if (index++) detail += ',';
        detail += role;
      }
      throw std::invalid_argument(detail + "}");
    }
    if (!byId.emplace(step.stepId, &step).second) {
      throw std::invalid_argument("execution plan declares duplicate step ids");
    }
  }

  // Every step-to-step edge must resolve to one declared producer step, and the
  // template as a whole must be acyclic. Cycle checking happens on the step DAG
  // rather than on the coarse role graph, because two tensor-parallel units
  // legitimately reference each other's previous-step partials.
  //
  // Slot names are step-local: a consumer names its own input slot, and the
  // dependency contract separately binds that input to a named output slot of
  // the producer. Requiring the two names to match would forbid the legitimate
  // case where the same tensor feeds several differently-named inputs.
  std::map<std::string, std::set<std::string>> successors;
  std::map<std::string, std::size_t> incoming;
  for (const auto& step : steps) {
    incoming[step.stepId];
    for (const auto& slot : step.inputSlots) {
      if (slot.producerStepId.empty()) continue;
      const auto producer = byId.find(slot.producerStepId);
      if (producer == byId.end()) {
        throw std::invalid_argument("execution step slot names an unknown producer step");
      }
      // LOCAL_STEP means produced in this same unit, in-process; REMOTE_STEP
      // means it travels over Named Data from a peer unit. Getting the two
      // confused is a real defect: a LOCAL_STEP bound to a peer's step would tell
      // the runtime to read a value in-process that only exists after a network
      // fetch, and a REMOTE_STEP bound inside the unit would create a dependency
      // the peer never publishes. The dependency check enforces the same rule
      // from the edge side, so a plan that satisfies one side but not the other
      // is inconsistent rather than merely unusual.
      const bool sameUnit = producer->second->unitId == step.unitId;
      if (sameUnit != (slot.source == "LOCAL_STEP")) {
        throw std::invalid_argument(
          "execution step local or remote slot disagrees with its producer unit");
      }
      // A local slot has no dependency edge to carry its type contract, so it is
      // checked against the producer's declared output here. Without this a
      // float32 value could feed an int32 input inside one unit and the mismatch
      // would only surface as a wrong read at execution time. The binding is the
      // explicit producer slot, never a name guess.
      if (slot.source == "LOCAL_STEP") {
        const auto& outputs = producer->second->outputSlots;
        const auto matched = std::find_if(outputs.begin(), outputs.end(),
          [&slot] (const auto& candidate) {
            return candidate.slotId == slot.producerSlotId;
          });
        if (matched == outputs.end()) {
          throw std::invalid_argument(
            "execution step local slot names an undeclared producer output slot");
        }
        if (matched->dtype != slot.dtype ||
            matched->shape != slot.shape ||
            matched->layoutDigest != slot.layoutDigest) {
          throw std::invalid_argument(
            "execution step local slot type, shape or layout disagrees with its producer");
        }
      }
      if (successors[slot.producerStepId].insert(step.stepId).second) {
        ++incoming[step.stepId];
      }
    }
  }
  std::vector<std::string> ready;
  for (const auto& [id, count] : incoming) {
    if (count == 0) ready.push_back(id);
  }
  std::size_t visited = 0;
  while (!ready.empty()) {
    const auto id = ready.back();
    ready.pop_back();
    ++visited;
    for (const auto& next : successors[id]) {
      if (--incoming.at(next) == 0) ready.push_back(next);
    }
  }
  if (visited != steps.size()) {
    throw std::invalid_argument("execution step template is cyclic");
  }

  // Dependency identity is the existing keyScope and must be unique; the
  // group label is never a lookup key. Step-bound dependencies must agree with
  // the template on both ends.
  using StepSlot = std::pair<std::string_view, std::string_view>;
  std::map<StepSlot, unsigned> remoteBindings;
  std::map<StepSlot, bool> partialOutputs;
  std::set<std::string> dependencyIds;
  for (const auto& dependency : dependencies) {
    const auto& id = dependency.dependencyId.empty() ? dependency.keyScope
                                                     : dependency.dependencyId;
    if (id.empty() || !dependencyIds.insert(id).second) {
      throw std::invalid_argument("execution plan dependency identity is missing or duplicated");
    }
    const bool shared = isSharedTensorPartial(dependency);
    if (dependency.operationKind == "TENSOR_PARTIAL" && !dependency.producerStepId.empty()) {
      const auto [it, inserted] = partialOutputs.emplace(
        StepSlot{dependency.producerStepId, dependency.producerSlotId}, shared);
      if (!inserted && (it->second || shared))
        throw std::invalid_argument("shared TP partial duplicates a producing slot");
    }
    if (shared) {
      const auto& audience = dependency.consumers;
      const bool stageHandoff = stageLayout.size() == 2 &&
        stageLayout.front().degree() == 2 && stageLayout.back().degree() == 1 &&
        dependency.producers.size() == 1 && audience.size() == 2 &&
        std::find(stageLayout.front().unitRoleIds.begin(), stageLayout.front().unitRoleIds.end(),
                  dependency.producers.front()) != stageLayout.front().unitRoleIds.end() &&
        std::find(audience.begin(), audience.end(), stageLayout.back().unitRoleIds.front()) !=
          audience.end();
      if ((!stageHandoff && (tensorDegree < 3 || stageLayout.size() > 1)) ||
          !dependency.useNdnsfDataV1 || dependency.dependencyId.empty() ||
          dependency.producers.size() != 1 || dependency.producerStepId.empty() ||
          dependency.producerSlotId != "partial" || dependency.tensors.size() != 1 ||
          dependency.tensors.front().empty() || !dependency.redistributions.empty() ||
          !dependency.consumerStepId.empty() || !dependency.consumerSlotId.empty() ||
          !std::is_sorted(audience.begin(), audience.end()) ||
          std::adjacent_find(audience.begin(), audience.end()) != audience.end()) {
        throw std::invalid_argument("invalid shared TP partial dependency");
      }
      for (const auto& role : audience) {
        ++remoteBindings[findSharedPartialInput(*this, dependency, role)];
      }
      continue;
    }
    const bool bound = !dependency.producerStepId.empty() ||
                       !dependency.producerSlotId.empty() ||
                       !dependency.consumerStepId.empty() ||
                       !dependency.consumerSlotId.empty();
    if (!bound) continue;
    if (dependency.producerStepId.empty() || dependency.producerSlotId.empty() ||
        dependency.consumerStepId.empty() || dependency.consumerSlotId.empty()) {
      throw std::invalid_argument("execution plan dependency step binding is partial");
    }
    const auto consumer = byId.find(dependency.consumerStepId);
    if (consumer == byId.end()) {
      throw std::invalid_argument("execution plan dependency names an unknown consumer step");
    }
    const bool consumed = std::any_of(consumer->second->inputSlots.begin(),
      consumer->second->inputSlots.end(), [&dependency] (const auto& slot) {
        return slot.slotId == dependency.consumerSlotId &&
               slot.producerStepId == dependency.producerStepId;
      });
    if (!consumed) {
      throw std::invalid_argument("execution plan dependency is not an admitted step input");
    }
    const auto producer = byId.find(dependency.producerStepId);
    if (producer == byId.end()) {
      throw std::invalid_argument("execution plan dependency names an unknown producer step");
    }
    if (producer->second->outputSlots.end() ==
        std::find_if(producer->second->outputSlots.begin(),
                     producer->second->outputSlots.end(),
                     [&dependency] (const auto& slot) {
                       return slot.slotId == dependency.producerSlotId;
                     })) {
      throw std::invalid_argument("execution plan dependency producer slot is undeclared");
    }
    if (dependency.operationKind.empty()) {
      throw std::invalid_argument("execution plan step dependency lacks an operation kind");
    }
    // The slot contract must agree on the wire: a producer slot carries the
    // tensor type the consumer expects, and the consumer reads it as a step
    // input, never as an epoch or feedback value. Without this, a plan could
    // bind a dependency to a slot whose declared source contradicts it.
    const auto& producerSlot = *std::find_if(producer->second->outputSlots.begin(),
      producer->second->outputSlots.end(), [&dependency] (const auto& slot) {
        return slot.slotId == dependency.producerSlotId;
      });
    const auto& consumerSlot = *std::find_if(consumer->second->inputSlots.begin(),
      consumer->second->inputSlots.end(), [&dependency] (const auto& slot) {
        return slot.slotId == dependency.consumerSlotId;
      });
    if (producerSlot.dtype != consumerSlot.dtype ||
        producerSlot.layoutDigest != consumerSlot.layoutDigest) {
      throw std::invalid_argument(
        "execution plan step dependency slot type or layout disagrees");
    }
    if (consumerSlot.source != "LOCAL_STEP" && consumerSlot.source != "REMOTE_STEP") {
      throw std::invalid_argument(
        "execution plan step dependency consumer slot is not a step input");
    }
    // A remote producer and a local one must be distinguishable, otherwise the
    // runtime cannot tell which contributions travel over Named Data.
    const bool sameUnit = producer->second->unitId == consumer->second->unitId;
    if (consumerSlot.source == "REMOTE_STEP") {
      ++remoteBindings[{dependency.consumerStepId, dependency.consumerSlotId}];
    }
    if (sameUnit != (consumerSlot.source == "LOCAL_STEP")) {
      throw std::invalid_argument(
        "execution plan step dependency source disagrees with its unit placement");
    }
  }
  // Reverse cover: every remote step input must be carried by exactly one
  // dependency, or it would silently wait forever. A local step input is
  // produced by the same unit in-process and needs no admitted network edge, so
  // requiring one there would forbid ordinary in-unit chaining.
  for (const auto& step : steps) {
    for (const auto& slot : step.inputSlots) {
      if (slot.producerStepId.empty() || slot.source != "REMOTE_STEP") continue;
      if (remoteBindings[{step.stepId, slot.slotId}] != 1) {
        throw std::invalid_argument(
          "execution plan step input has no unique admitting dependency");
      }
    }
  }
}

std::string
trimSlashes(std::string value)
{
  while (!value.empty() && value.front() == '/') {
    value.erase(value.begin());
  }
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

std::string
replaceAll(std::string value, const std::string& from, const std::string& to)
{
  if (from.empty()) {
    return value;
  }
  std::size_t pos = 0;
  while ((pos = value.find(from, pos)) != std::string::npos) {
    value.replace(pos, from.size(), to);
    pos += to.size();
  }
  return value;
}

std::string
plannedDataNameFromTemplate(const std::string& objectNameTemplate,
                            const std::string& sessionId,
                            const std::string& keyScope,
                            const std::string& producerRole,
                            const std::string& consumerRole,
                            const std::string& topicPrefix,
                            const std::string& producerProvider,
                            std::size_t sequence)
{
  if (objectNameTemplate.empty()) {
    return "";
  }
  std::string value = objectNameTemplate;
  value = replaceAll(value, "{producerProvider}", producerProvider);
  value = replaceAll(value, "{sessionId}", trimSlashes(sessionId));
  value = replaceAll(value, "{keyScope}", keyScope);
  value = replaceAll(value, "{producerRole}", trimSlashes(producerRole));
  value = replaceAll(value, "{role}", trimSlashes(consumerRole));
  value = replaceAll(value, "{topicPrefix}", trimSlashes(topicPrefix));
  value = replaceAll(value, "{sequence}", std::to_string(sequence));
  return value;
}

std::string
plannedSegmentName(const std::string& plannedDataName, std::size_t segmentNo)
{
  if (plannedDataName.empty()) {
    return "";
  }
  return plannedDataName + "/seg=" + std::to_string(segmentNo);
}

std::vector<std::string>
plannedSegmentNamesForEdge(const DependencyEdge& edge)
{
  std::vector<std::string> names;
  if (edge.plannedDataName.empty() || edge.expectedSegments == 0) {
    return names;
  }
  names.reserve(edge.expectedSegments);
  for (std::size_t segmentNo = 0; segmentNo < edge.expectedSegments; ++segmentNo) {
    names.push_back(plannedSegmentName(edge.plannedDataName, segmentNo));
  }
  return names;
}

bool
hasStaticSegmentPlan(const NativeDependencySpec& dependency)
{
  return dependency.segmentNaming.mode == "ndn-segment-component" &&
         dependency.segmentNaming.staticSegmentCount > 0 &&
         !dependency.segmentNaming.dynamicFallback;
}

std::string
providerForRole(const NativeProviderAssignment& assignment,
                const std::string& role,
                const std::string& fallbackProvider)
{
  const auto found = assignment.providerByRole.find(role);
  if (found != assignment.providerByRole.end()) {
    return found->second;
  }
  return fallbackProvider;
}

namespace {

std::size_t
effectiveExpectedSegments(const NativeDependencySpec& dependency)
{
  if (hasStaticSegmentPlan(dependency)) {
    return dependency.segmentNaming.staticSegmentCount;
  }
  return dependency.expectedSegments;
}

} // namespace

std::size_t
bindNativeEdgeEndpoints(RoleSpec& spec,
                        const std::vector<NativeEdgeEndpointBinding>& bindings)
{
  // A binding names its producer and consumer; the edge it belongs to is the one that
  // names both. Ambiguity is refused rather than resolved by order, because two edges
  // between the same roles would otherwise receive one binding's transport identity.
  // Every field the binding states must match the edge. A role pair alone is not
  // unique -- each layer produces an attention and an MLP partial for the same peer --
  // so a binding that names only roles would be ambiguous, and one that names the
  // dependency and the step/slot pair identifies exactly one edge.
  const auto matches = [&bindings] (const DependencyEdge& edge) {
    std::vector<const NativeEdgeEndpointBinding*> found;
    for (const auto& binding : bindings) {
      if (binding.producerRole != edge.producerRole) continue;
      if (binding.consumerRole != edge.consumerRole) continue;
      if (!binding.dependencyId.empty() && binding.dependencyId != edge.scope) continue;
      if (!binding.operationKind.empty() && binding.operationKind != edge.operationKind) continue;
      if (!binding.producerStepId.empty() &&
          binding.producerStepId != edge.producerStepId) continue;
      if (!binding.producerSlotId.empty() &&
          binding.producerSlotId != edge.producerSlotId) continue;
      if (!binding.consumerStepId.empty() &&
          binding.consumerStepId != edge.consumerStepId) continue;
      if (!binding.consumerSlotId.empty() &&
          binding.consumerSlotId != edge.consumerSlotId) continue;
      found.push_back(&binding);
    }
    return found;
  };
  const auto apply = [] (DependencyEdge& edge, const NativeEdgeEndpointBinding& binding) {
    edge.endpointDigest = binding.endpointDigest;
    edge.manifestDataName = binding.manifestDataName;
    if (!binding.plannedDataName.empty()) edge.plannedDataName = binding.plannedDataName;
    edge.tensorDigest = binding.tensorDigest;
    edge.layoutDigest = binding.layoutDigest;
    edge.collectiveTargetLayoutDigest = binding.targetLayoutDigest;
    edge.securityProfile = binding.securityProfile;
    edge.maxSegments = binding.maxSegments;
    edge.noProgressDeadlineMs = binding.noProgressDeadlineMs;
    edge.hardDeadlineMs = binding.hardDeadlineMs;
    edge.declaredByV3 = true;
  };
  std::set<const NativeEdgeEndpointBinding*> consumed;
  std::size_t bound = 0;
  const auto visit = [&] (DependencyEdge& edge) {
    if (!edge.useNdnsfDataV1) return;
    const auto found = matches(edge);
    if (found.empty()) {
      throw std::invalid_argument(
        "DATA_V1 edge has no certified endpoint: " + edge.scope);
    }
    if (found.size() != 1) {
      throw std::invalid_argument(
        "DATA_V1 edge matches more than one certified endpoint: " + edge.scope);
    }
    apply(edge, *found.front());
    consumed.insert(found.front());
    ++bound;
  };
  for (auto& edge : spec.inputs) visit(edge);
  for (auto& edge : spec.outputs) visit(edge);
  for (const auto& binding : bindings) {
    if (consumed.count(&binding) == 0) {
      throw std::invalid_argument(
        "certified endpoint matched no DATA_V1 edge: " + binding.endpointDigest);
    }
  }
  return bound;
}

RoleSpec
roleSpecFor(const NativeExecutionPlan& plan,
            const std::string& role,
            const std::string& sessionId,
            const NativeProviderAssignment& assignment,
            const std::string& localProvider)
{
  return roleSpecFor(plan, role, sessionId, assignment, localProvider, 0);
}

RoleSpec
roleSpecFor(const NativeExecutionPlan& plan,
            const std::string& role,
            const std::string& sessionId,
            const NativeProviderAssignment& assignment,
            const std::string& localProvider,
            std::size_t sequence)
{
  if (std::any_of(plan.dependencies.begin(), plan.dependencies.end(), isSharedTensorPartial)) {
    throw std::invalid_argument("shared TP partials require an authenticated role projection");
  }
  RoleSpec spec;
  spec.role = role;
  bool knownRole = false;
  for (const auto& item : plan.roles) {
    if (item == role) {
      knownRole = true;
      break;
    }
  }
  if (!knownRole) {
    throw std::out_of_range("NativeExecutionPlan has no role: " + role);
  }

  const auto operationIndexFor = [&plan, sequence] (
                                  const NativeDependencySpec& dependency) {
    if (!dependency.useNdnsfDataV1 || sequence == 0) {
      return dependency.collectiveOperationIndex;
    }
    if (plan.streamingOperationStride == 0) {
      throw std::invalid_argument(
        "streamed NDNSF_DATA_V1 plan is missing operation stride");
    }
    return dependency.collectiveOperationIndex +
           sequence * plan.streamingOperationStride;
  };

  for (const auto& dep : plan.dependencies) {
    for (const auto& consumer : dep.consumers) {
      if (consumer != role) {
        continue;
      }
      for (const auto& producer : dep.producers) {
        const auto producerProvider = providerForRole(assignment, producer, localProvider);
        const auto expectedSegments = effectiveExpectedSegments(dep);
        const auto runtimeScope = !dep.redistributions.empty() &&
                                  dep.producers.size() > 1
          ? dep.keyScope + "/from/" + trimSlashes(producer)
          : dep.keyScope;
        DependencyEdge edge{
          runtimeScope,
          producer,
          consumer,
          plannedDataNameFromTemplate(dep.objectNameTemplate,
                                      sessionId,
                                      dep.keyScope,
                                      producer,
                                      consumer,
                                      dep.topicPrefix,
                                      producerProvider,
                                      sequence),
          expectedSegments,
          dep.expectedBytes,
          dep.tensors,
        };
        edge.useNdnsfDataV1 = dep.useNdnsfDataV1;
        edge.collectiveOperationIndex = operationIndexFor(dep);
        edge.collectiveProducerRank = dep.collectiveProducerRank;
        edge.collectiveSourceLayoutDigest = dep.collectiveSourceLayoutDigest;
        edge.collectiveTargetLayoutDigest = dep.collectiveTargetLayoutDigest;
        edge.collectiveTensorDigest = dep.collectiveTensorDigest;
        edge.redistributions = dep.redistributions;
        if (!dep.redistributions.empty()) {
          const auto& redistribution = dep.redistributions.front();
          const auto producerIndex = static_cast<std::size_t>(
            std::distance(dep.producers.begin(),
                          std::find(dep.producers.begin(), dep.producers.end(), producer)));
          const auto consumerIndex = static_cast<std::size_t>(
            std::distance(dep.consumers.begin(),
                          std::find(dep.consumers.begin(), dep.consumers.end(), consumer)));
          if (producerIndex < redistribution.producerRanks.size()) {
            edge.redistributionProducerRank =
              redistribution.producerRanks[producerIndex];
          }
          if (consumerIndex < redistribution.consumerRanks.size()) {
            edge.redistributionConsumerRank =
              redistribution.consumerRanks[consumerIndex];
          }
        }
        edge.transportScope = dep.keyScope;
        edge.producerProvider = producerProvider;
        edge.topicPrefix = dep.topicPrefix;
        edge.operationKind = dep.operationKind;
        edge.round = sequence;
        // The sealed step binding travels with the runtime edge so the worker can
        // match a step's remote input to this exact dependency. It is already
        // validated to name a real producer and consumer step/slot pair.
        edge.producerStepId = dep.producerStepId;
        edge.producerSlotId = dep.producerSlotId;
        edge.consumerStepId = dep.consumerStepId;
        edge.consumerSlotId = dep.consumerSlotId;
        spec.inputs.push_back(std::move(edge));
      }
    }
    for (const auto& producer : dep.producers) {
      if (producer != role) {
        continue;
      }
      // A redistribution publishes one producer-rank tensor once. All
      // consumers fetch that immutable object and apply the certified layout
      // transition locally; publishing once per consumer would create replay-
      // conflicting DATA_V1 objects for the same operation/rank.
      const auto outputConsumers = dep.redistributions.empty()
        ? dep.consumers
        : std::vector<std::string>{dep.consumers.front()};
      for (const auto& consumer : outputConsumers) {
        const auto producerProvider = providerForRole(assignment, producer, localProvider);
        const auto expectedSegments = effectiveExpectedSegments(dep);
        const auto runtimeScope = !dep.redistributions.empty() &&
                                  dep.producers.size() > 1
          ? dep.keyScope + "/from/" + trimSlashes(producer)
          : dep.keyScope;
        DependencyEdge edge{
          runtimeScope,
          producer,
          consumer,
          plannedDataNameFromTemplate(dep.objectNameTemplate,
                                      sessionId,
                                      dep.keyScope,
                                      producer,
                                      consumer,
                                      dep.topicPrefix,
                                      producerProvider,
                                      sequence),
          expectedSegments,
          dep.expectedBytes,
          dep.tensors,
        };
        edge.useNdnsfDataV1 = dep.useNdnsfDataV1;
        edge.collectiveOperationIndex = operationIndexFor(dep);
        edge.collectiveProducerRank = dep.collectiveProducerRank;
        edge.collectiveSourceLayoutDigest = dep.collectiveSourceLayoutDigest;
        edge.collectiveTargetLayoutDigest = dep.collectiveTargetLayoutDigest;
        edge.collectiveTensorDigest = dep.collectiveTensorDigest;
        edge.redistributions = dep.redistributions;
        if (!dep.redistributions.empty()) {
          const auto& redistribution = dep.redistributions.front();
          const auto producerIndex = static_cast<std::size_t>(
            std::distance(dep.producers.begin(),
                          std::find(dep.producers.begin(), dep.producers.end(), producer)));
          const auto consumerIndex = static_cast<std::size_t>(
            std::distance(dep.consumers.begin(),
                          std::find(dep.consumers.begin(), dep.consumers.end(), consumer)));
          if (producerIndex < redistribution.producerRanks.size()) {
            edge.redistributionProducerRank =
              redistribution.producerRanks[producerIndex];
          }
          if (consumerIndex < redistribution.consumerRanks.size()) {
            edge.redistributionConsumerRank =
              redistribution.consumerRanks[consumerIndex];
          }
        }
        edge.transportScope = dep.keyScope;
        edge.producerProvider = producerProvider;
        edge.topicPrefix = dep.topicPrefix;
        edge.operationKind = dep.operationKind;
        edge.round = sequence;
        edge.producerStepId = dep.producerStepId;
        edge.producerSlotId = dep.producerSlotId;
        edge.consumerStepId = dep.consumerStepId;
        edge.consumerSlotId = dep.consumerSlotId;
        spec.outputs.push_back(std::move(edge));
      }
    }
  }
  // Carry this role's slice of the sealed template. Only the steps belonging to
  // this unit travel with it, because a unit never runs a peer's step: peers
  // contribute values through admitted dependencies, not by executing here.
  if (!plan.steps.empty()) {
    auto owned = std::make_shared<std::vector<NativeExecutionStepSpec>>();
    for (const auto& step : plan.steps)
      if (step.unitId == role) owned->push_back(step);
    if (owned->empty())
      throw std::invalid_argument(
        "sealed step plan has no steps for role " + role);
    // The shared owner outlives every asynchronous drain of this role, which is
    // what lets the worker reference the template without copying it per epoch.
    spec.steps = std::move(owned);
    spec.stepPlanVersion = plan.stepPlanVersion;
    // A unit's degree is its own stage's member count. Reading the plan-wide maximum
    // gives a degree-one stage the geometry of a sharded neighbour.
    spec.tensorDegree = plan.degreeForRole(role);
  }
  return spec;
}

RoleSpec
roleSpecFor(const NativeExecutionPlan& plan,
            const std::string& role,
            const ExecutionAttemptKey& attempt,
            const NativeProviderAssignment& assignment,
            const std::string& localProvider)
{
  attempt.validate();
  auto spec = roleSpecFor(plan, role, attempt.scopedSessionId(),
                          assignment, localProvider);
  spec.requestId = attempt.requestId;
  spec.attemptEpoch = attempt.attemptEpoch;
  for (auto& edge : spec.inputs) {
    edge.requestId = attempt.requestId;
    edge.attemptEpoch = attempt.attemptEpoch;
  }
  for (auto& edge : spec.outputs) {
    edge.requestId = attempt.requestId;
    edge.attemptEpoch = attempt.attemptEpoch;
  }
  return spec;
}

NativePlanSession
deployNativePlanSession(NativeExecutionPlan plan,
                        std::string sessionId,
                        NativeProviderAssignment assignment)
{
  if (sessionId.empty()) {
    throw std::invalid_argument("NativePlanSession requires a non-empty sessionId");
  }

  NativePlanSession session;
  session.sessionId = std::move(sessionId);
  session.assignment = std::move(assignment);
  session.plan = std::move(plan);
  for (const auto& role : session.plan.roles) {
    session.rolesByName.emplace(
      role,
      roleSpecFor(session.plan, role, session.sessionId, session.assignment));
  }
  return session;
}

} // namespace ndnsf::di
