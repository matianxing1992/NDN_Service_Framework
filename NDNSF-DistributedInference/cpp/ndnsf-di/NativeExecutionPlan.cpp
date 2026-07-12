#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"

#include <stdexcept>
#include <utility>

namespace ndnsf::di {

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
  return trimSlashes(requestId) + "/attempt=" + std::to_string(attemptEpoch);
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
        spec.inputs.push_back(DependencyEdge{
          dep.keyScope,
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
        });
      }
    }
    for (const auto& producer : dep.producers) {
      if (producer != role) {
        continue;
      }
      for (const auto& consumer : dep.consumers) {
        const auto producerProvider = providerForRole(assignment, producer, localProvider);
        const auto expectedSegments = effectiveExpectedSegments(dep);
        spec.outputs.push_back(DependencyEdge{
          dep.keyScope,
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
        });
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
