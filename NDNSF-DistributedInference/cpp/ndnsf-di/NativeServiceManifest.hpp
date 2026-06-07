#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_SERVICE_MANIFEST_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_SERVICE_MANIFEST_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <istream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::di {

namespace detail {

inline bool
ptreeIsArray(const boost::property_tree::ptree& node)
{
  if (node.empty()) {
    return false;
  }
  return std::all_of(node.begin(), node.end(), [] (const auto& item) {
    return item.first.empty();
  });
}

inline std::string
joinStrings(const std::vector<std::string>& values)
{
  std::ostringstream output;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      output << ",";
    }
    output << values[i];
  }
  return output.str();
}

inline std::string
manifestScalarValue(const boost::property_tree::ptree& node)
{
  return node.get_value<std::string>("");
}

inline std::string
manifestFlattenedValue(const boost::property_tree::ptree& node)
{
  if (node.empty()) {
    return manifestScalarValue(node);
  }
  if (ptreeIsArray(node)) {
    std::vector<std::string> values;
    for (const auto& item : node) {
      values.push_back(manifestFlattenedValue(item.second));
    }
    return joinStrings(values);
  }
  std::ostringstream output;
  boost::property_tree::write_json(output, node, false);
  return output.str();
}

inline std::map<std::string, std::string>
manifestMetadata(const boost::property_tree::ptree& artifact)
{
  std::map<std::string, std::string> metadata;
  const auto rawMetadata = artifact.get_child_optional("metadata");
  if (rawMetadata) {
    for (const auto& item : rawMetadata.get()) {
      metadata[item.first] = manifestFlattenedValue(item.second);
    }
  }

  for (const auto& key : {"role", "kind", "backend", "artifact", "filename"}) {
    const auto value = artifact.get_optional<std::string>(key);
    if (value && !value->empty()) {
      metadata[key] = *value;
    }
  }
  return metadata;
}

} // namespace detail

inline std::vector<NativeModelRunnerSpec>
nativeModelRunnerSpecsForServiceManifestFromJson(std::istream& input,
                                                 const std::string& serviceName)
{
  boost::property_tree::ptree root;
  boost::property_tree::read_json(input, root);

  const auto services = root.get_child_optional("services");
  if (!services) {
    throw std::invalid_argument("service manifest missing services");
  }

  for (const auto& serviceNode : services.get()) {
    const auto& service = serviceNode.second;
    const auto name = service.get<std::string>("name", "");
    if (name != serviceName) {
      continue;
    }

    std::vector<NativeModelRunnerSpec> specs;
    const auto artifacts = service.get_child_optional("artifacts");
    if (!artifacts) {
      return specs;
    }

    for (const auto& artifactNode : artifacts.get()) {
      const auto& artifact = artifactNode.second;
      NativeModelRunnerSpec spec;
      spec.role = artifact.get<std::string>("role", "");
      spec.kind = artifact.get<std::string>("kind", "");
      spec.backend = artifact.get<std::string>("backend", "");
      spec.path = artifact.get<std::string>("path", "");
      spec.metadata = detail::manifestMetadata(artifact);
      if (spec.role.empty()) {
        throw std::invalid_argument("service manifest artifact missing role");
      }
      if (spec.backend.empty()) {
        throw std::invalid_argument("service manifest artifact missing backend for role: " +
                                    spec.role);
      }
      specs.push_back(std::move(spec));
    }
    return specs;
  }

  throw std::out_of_range("service manifest has no service: " + serviceName);
}

inline std::map<std::string, NativeModelRunnerSpec>
nativeModelRunnerSpecsByRoleForServiceManifestFromJson(std::istream& input,
                                                       const std::string& serviceName)
{
  std::map<std::string, NativeModelRunnerSpec> specsByRole;
  for (auto& spec : nativeModelRunnerSpecsForServiceManifestFromJson(input, serviceName)) {
    const auto role = spec.role;
    if (!specsByRole.emplace(role, std::move(spec)).second) {
      throw std::invalid_argument("service manifest has duplicate artifact role: " + role);
    }
  }
  return specsByRole;
}

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_SERVICE_MANIFEST_HPP
