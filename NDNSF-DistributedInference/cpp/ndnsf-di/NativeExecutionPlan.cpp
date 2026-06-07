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
          dep.expectedSegments,
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
          dep.expectedSegments,
          dep.expectedBytes,
          dep.tensors,
        });
      }
    }
  }
  return spec;
}

} // namespace ndnsf::di
