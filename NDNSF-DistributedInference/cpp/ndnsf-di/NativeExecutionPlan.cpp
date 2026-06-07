#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"

#include <stdexcept>
#include <utility>

namespace ndnsf::di {

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
