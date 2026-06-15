#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::di {

struct SegmentNamingSpec
{
  std::string mode = "ndn-segment-component";
  std::size_t staticSegmentCount = 0;
  bool dynamicFallback = true;
};

struct NativeDependencySpec
{
  NativeDependencySpec() = default;

  NativeDependencySpec(std::vector<std::string> producers,
                       std::vector<std::string> consumers,
                       std::string keyScope,
                       std::string topicPrefix,
                       std::string objectNameTemplate,
                       std::size_t expectedSegments = 0,
                       std::size_t expectedBytes = 0,
                       std::vector<std::string> tensors = {});

  std::vector<std::string> producers;
  std::vector<std::string> consumers;
  std::string keyScope;
  std::string topicPrefix;
  std::string objectNameTemplate;
  std::size_t expectedSegments = 0;
  std::size_t expectedBytes = 0;
  std::vector<std::string> tensors;
  SegmentNamingSpec segmentNaming;
};

struct NativeExecutionPlan
{
  int version = 1;
  std::string serviceName;
  std::string modelName;
  std::string modelFamily = "generic-onnx";
  std::string modelFormat = "unknown";
  std::string plannerKind = "onnx-dag";
  std::vector<std::string> roles;
  std::vector<NativeDependencySpec> dependencies;
};

struct NativeProviderAssignment
{
  std::map<std::string, std::string> providerByRole;
};

struct NativePlanSession
{
  std::string sessionId;
  NativeExecutionPlan plan;
  NativeProviderAssignment assignment;
  std::map<std::string, RoleSpec> rolesByName;
};

std::string
trimSlashes(std::string value);

std::string
replaceAll(std::string value, const std::string& from, const std::string& to);

std::string
plannedDataNameFromTemplate(const std::string& objectNameTemplate,
                            const std::string& sessionId,
                            const std::string& keyScope,
                            const std::string& producerRole,
                            const std::string& consumerRole,
                            const std::string& topicPrefix,
                            const std::string& producerProvider,
                            std::size_t sequence = 0);

std::string
plannedSegmentName(const std::string& plannedDataName, std::size_t segmentNo);

std::vector<std::string>
plannedSegmentNamesForEdge(const DependencyEdge& edge);

bool
hasStaticSegmentPlan(const NativeDependencySpec& dependency);

std::string
providerForRole(const NativeProviderAssignment& assignment,
                const std::string& role,
                const std::string& fallbackProvider = "");

RoleSpec
roleSpecFor(const NativeExecutionPlan& plan,
            const std::string& role,
            const std::string& sessionId,
            const NativeProviderAssignment& assignment,
            const std::string& localProvider = "");

NativePlanSession
deployNativePlanSession(NativeExecutionPlan plan,
                        std::string sessionId,
                        NativeProviderAssignment assignment);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_HPP
