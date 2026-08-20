#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"

#include <ndn-cxx/name.hpp>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

std::string
opaqueNameComponent(const std::string& value)
{
  static constexpr char HEX[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (const auto byte : value) {
    const auto unsignedByte = static_cast<unsigned char>(byte);
    result.push_back(HEX[unsignedByte >> 4]);
    result.push_back(HEX[unsignedByte & 0x0f]);
  }
  return result;
}

void
appendLabelledOpaque(ndn::Name& name,
                     const char* label,
                     const std::string& value)
{
  if (value.empty()) {
    throw std::invalid_argument(
      std::string("empty tensor endpoint component: ") + label);
  }
  name.append(label).append(opaqueNameComponent(value));
}

} // namespace

std::string
tensorObjectNamePrefix(const NativeTensorEndpointV3& endpoint)
{
  if (endpoint.producerNamespace.empty() ||
      endpoint.producerNamespace.front() != '/' || endpoint.requester.empty() ||
      endpoint.requestId.empty() || endpoint.attempt == 0 ||
      endpoint.planDigest.empty() || endpoint.groupId.empty() ||
      endpoint.groupEpoch.empty() || endpoint.operation.empty() ||
      endpoint.consumerRole.empty() || endpoint.tensorId.empty() ||
      endpoint.tensorDigest.empty() || endpoint.segmentCount == 0) {
    throw std::invalid_argument("incomplete V3 tensor endpoint name binding");
  }
  const auto sourceRole = endpoint.producerRole.empty()
    ? std::string("INPUT") : endpoint.producerRole;
  ndn::Name name(endpoint.producerNamespace);
  name.append("NDNSF-DI").append("TENSOR").append("v1");
  appendLabelledOpaque(name, "REQUESTER", endpoint.requester);
  appendLabelledOpaque(name, "REQ", endpoint.requestId);
  name.append("ATTEMPT").appendNumber(endpoint.attempt);
  appendLabelledOpaque(name, "PLAN", endpoint.planDigest);
  appendLabelledOpaque(name, "GROUP", endpoint.groupId);
  appendLabelledOpaque(name, "EPOCH", endpoint.groupEpoch);
  appendLabelledOpaque(name, "OP", endpoint.operation);
  name.append("ROUND").appendNumber(endpoint.round);
  appendLabelledOpaque(name, "SOURCE-ROLE", sourceRole);
  name.append("RANK").appendNumber(endpoint.producerRank);
  appendLabelledOpaque(name, "TENSOR", endpoint.tensorId);
  name.append(opaqueNameComponent(endpoint.tensorDigest));
  name.append("MICROBATCH").appendNumber(endpoint.microbatch);
  return name.toUri();
}

std::string
tensorObjectManifestName(const NativeTensorEndpointV3& endpoint)
{
  return ndn::Name(tensorObjectNamePrefix(endpoint)).append("MANIFEST").toUri();
}

std::string
tensorObjectSegmentName(const NativeTensorEndpointV3& endpoint,
                        std::size_t segmentNo)
{
  if (segmentNo >= endpoint.segmentCount) {
    throw std::out_of_range("tensor segment is outside the declared endpoint");
  }
  return ndn::Name(tensorObjectNamePrefix(endpoint))
    .append("SEG")
    .appendSegment(segmentNo)
    .toUri();
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
  key.validate();
  std::lock_guard<std::mutex> lock(m_mutex);
  auto& state = m_states[key.requestId];
  if (key.attemptEpoch < state.currentEpoch) {
    return ExecutionAttemptAdmission::Stale;
  }
  if (key.attemptEpoch > state.currentEpoch) {
    state = State{key.attemptEpoch, false, false};
    return ExecutionAttemptAdmission::Accepted;
  }
  if (state.terminal) {
    return ExecutionAttemptAdmission::DuplicateTerminal;
  }
  if (state.cancelled) {
    return ExecutionAttemptAdmission::Cancelled;
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

RoleSpec
roleSpecFor(const NativeExecutionPlan& plan,
            const std::string& role,
            const std::string& sessionId,
            const NativeProviderAssignment& assignment,
            const std::string& localProvider)
{
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
                                      producer,
                                      dep.topicPrefix,
                                      producerProvider),
          expectedSegments,
          dep.expectedBytes,
          dep.tensors,
        };
        edge.useNdnsfDataV1 = dep.useNdnsfDataV1;
        edge.collectiveOperationIndex = dep.collectiveOperationIndex;
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
                                      producerProvider),
          expectedSegments,
          dep.expectedBytes,
          dep.tensors,
        };
        edge.useNdnsfDataV1 = dep.useNdnsfDataV1;
        edge.collectiveOperationIndex = dep.collectiveOperationIndex;
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
        spec.outputs.push_back(std::move(edge));
      }
    }
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
