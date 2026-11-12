#ifndef NDNSF_TESTS_SPEC233_AUTOMATIC_SOURCE_PROJECTION_HPP
#define NDNSF_TESTS_SPEC233_AUTOMATIC_SOURCE_PROJECTION_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-model.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace ndnsf::di::fixture {

/** Complete structural Selection for source-admission tests, not a grant or
 * distributed-plan oracle. Each behavioral test supplies its own source/seed.
 */
inline NativeSelectionProjectionV3
automaticSourceProjection(const AutomaticOnnxModel& model, const NativeAutomaticShapeSeed& seed)
{
  NativeSelectionProjectionV3 projection;
  projection.automaticShapeSeed = seed;
  projection.provider = "/source-provider";
  projection.requestId = "/source-request";
  projection.canonicalArtifactName = "/source-publisher/root";
  projection.attempt = 1;
  projection.planDigest = nativePlanningDigest("source-plan");
  projection.requestContractDigest = nativePlanningDigest("source-request-contract");
  projection.plan.serviceName = "/service";
  projection.plan.modelName = model.descriptor.modelName;
  auto& role = projection.assembly;
  role.role = role.selectedRole = "/role";
  role.backend = "onnxruntime-cpu";
  role.artifactDigest = model.opened.modelDigest;
  role.roleKind = "COMPONENT_SET";
  role.adapterId = model.descriptor.adapterId;
  role.adapterVersion = model.descriptor.adapterVersion;
  role.modelManifestDigest = nativePlanningDigest("source-root");
  role.artifactProfileDigest = nativePlanningDigest("source-profile");
  role.graphDigest = model.inspection.canonicalIdentity.graphDigest;
  role.canonicalInitializerDigest = model.inspection.canonicalIdentity.initializerDigest;
  role.adapterDescriptorDigest = nativePlanningDigest("source-adapter");
  role.assemblerDescriptorDigest =
    "sha256:5291ee00f425c59605f72e26c9b27a73aca43b976421218515fc1a38085c7a89";
  role.backendAbi = "onnxruntime";
  for (std::uint64_t index = 0; index < model.inspection.graph.nodes.size(); ++index)
    role.nodeIndices.push_back(index);
  const auto port = [&] (const std::string& name) {
    const auto fact = std::find_if(model.contract.tensors.begin(), model.contract.tensors.end(),
      [&] (const auto& tensor) { return tensor.name == name; });
    if (fact == model.contract.tensors.end() || !fact->wireElementType ||
        name.size() <= 6 || name.compare(0, 6, "graph/") != 0)
      throw std::invalid_argument("source fixture port is unavailable");
    NativeAssemblyTensorContractV3 result;
    result.name = name.substr(6);
    result.dtype = tensorElementTypeName(*fact->wireElementType);
    if (fact->dimensions) {
      for (const auto& dimension : *fact->dimensions) {
        if (dimension.kind == DimensionConstraint::Kind::FIXED)
          result.shape.emplace_back(dimension.fixed);
        else
          result.shape.emplace_back(dimension.symbol.empty() ? "dynamic" : dimension.symbol);
      }
    }
    return result;
  };
  for (const auto& name : model.contract.modelInputs) role.expectedInputs.push_back(port(name));
  for (const auto& name : model.contract.modelOutputs) role.expectedOutputs.push_back(port(name));
  role.precision = model.descriptor.precision;
  role.quantization = model.descriptor.quantizationSubtype;
  role.layout = "native";
  role.padding = "none";
  role.maxSourceBytes = role.maxAssembledBytes = 8U << 20;
  role.maxNodes = 1024;
  role.automaticContractDigest = seed.contractDigest;
  role.recipeDigest = nativePlanningDigest(canonicalNativeOnnxRecipeJson(role));
  projection.selectedRole = role;
  projection.executionRole.roleId = role.role;
  projection.executionRole.adapterId = role.adapterId;
  projection.executionRole.adapterVersion = role.adapterVersion;
  projection.dataflow.role = role.role;
  projection.dataflow.requestId = projection.requestId;
  projection.dataflow.attempt = projection.attempt;
  projection.dataflow.planDigest = projection.planDigest;
  projection.dataflow.terminalResponseOwner = true;
  return projection;
}

} // namespace ndnsf::di::fixture

#endif // NDNSF_TESTS_SPEC233_AUTOMATIC_SOURCE_PROJECTION_HPP
