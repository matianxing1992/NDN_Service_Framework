#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <stdexcept>
#include <utility>

namespace ndnsf::di {

std::vector<std::string>
stringArrayFromJson(const boost::property_tree::ptree& node, const std::string& key)
{
  std::vector<std::string> values;
  const auto child = node.get_child_optional(key);
  if (!child) {
    return values;
  }
  for (const auto& item : child.get()) {
    values.push_back(item.second.get_value<std::string>());
  }
  return values;
}

namespace {

SegmentNamingSpec
segmentNamingFromJson(const boost::property_tree::ptree& dependency,
                      std::size_t expectedSegments)
{
  SegmentNamingSpec spec;
  spec.staticSegmentCount = expectedSegments;
  spec.dynamicFallback = expectedSegments == 0;

  const auto segmentNaming = dependency.get_child_optional("segmentNaming");
  if (!segmentNaming) {
    return spec;
  }

  spec.mode = segmentNaming->get<std::string>("mode", spec.mode);
  spec.staticSegmentCount =
    segmentNaming->get<std::size_t>("staticSegmentCount", spec.staticSegmentCount);
  spec.dynamicFallback =
    segmentNaming->get<bool>("dynamicFallback", spec.dynamicFallback);
  if (spec.staticSegmentCount == 0) {
    spec.dynamicFallback = true;
  }
  return spec;
}

} // namespace

std::map<std::string, NativeExecutionPlan>
nativeExecutionPlansByServiceFromJson(std::istream& input)
{
  boost::property_tree::ptree root;
  boost::property_tree::read_json(input, root);
  const auto version = root.get<int>("version", 0);
  if (version != 1 && version != 2) {
    throw std::invalid_argument("unsupported native execution plan version");
  }

  std::map<std::string, NativeExecutionPlan> plans;
  const auto services = root.get_child_optional("services");
  if (!services) {
    throw std::invalid_argument("native execution plan missing services");
  }

  for (const auto& serviceNode : services.get()) {
    const auto& service = serviceNode.second;
    const auto serviceName = service.get<std::string>("service", "");
    if (serviceName.empty()) {
      throw std::invalid_argument("native execution plan service missing name");
    }

    NativeExecutionPlan plan;
    plan.version = version;
    plan.serviceName = serviceName;
    plan.modelName = service.get<std::string>("model", "");
    plan.modelFamily = service.get<std::string>("modelFamily", "generic-onnx");
    plan.modelFormat = service.get<std::string>("modelFormat", "unknown");
    plan.plannerKind = service.get<std::string>("plannerKind", "onnx-dag");
    plan.roles = stringArrayFromJson(service, "roles");
    const auto dependencies = service.get_child_optional("dependencies");
    if (dependencies) {
      for (const auto& depNode : dependencies.get()) {
        const auto& dep = depNode.second;
        NativeDependencySpec spec;
        spec.producers = stringArrayFromJson(dep, "producers");
        spec.consumers = stringArrayFromJson(dep, "consumers");
        spec.keyScope = dep.get<std::string>("keyScope", "");
        spec.topicPrefix = dep.get<std::string>("topicPrefix", "");
        spec.objectNameTemplate = dep.get<std::string>("objectNameTemplate", "");
        spec.expectedSegments = dep.get<std::size_t>("expectedSegments", 0);
        spec.expectedBytes = dep.get<std::size_t>("expectedBytes", 0);
        spec.tensors = stringArrayFromJson(dep, "tensors");
        spec.segmentNaming = segmentNamingFromJson(dep, spec.expectedSegments);
        if (spec.keyScope.empty()) {
          throw std::invalid_argument(
            "native execution plan dependency missing keyScope");
        }
        plan.dependencies.push_back(std::move(spec));
      }
    }
    plans.emplace(serviceName, std::move(plan));
  }
  return plans;
}

NativeExecutionPlan
nativeExecutionPlanForServiceFromJson(std::istream& input, const std::string& serviceName)
{
  auto plans = nativeExecutionPlansByServiceFromJson(input);
  const auto found = plans.find(serviceName);
  if (found == plans.end()) {
    throw std::out_of_range("native execution plan has no service: " + serviceName);
  }
  return found->second;
}

} // namespace ndnsf::di
