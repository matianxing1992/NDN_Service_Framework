#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"

namespace ndnsf::di::fixture {

inline void assemblies(NativePlanSealingInputs& inputs)
{
  inputs.assemblyByRole.clear();
  for (const auto& item : inputs.artifacts.artifactDigestByRole) {
    NativeSelectionRoleV3 role;
    role.role = role.selectedRole = item.first;
    role.layerEnd = 1;
    role.backend = "onnxruntime-cpu";
    role.artifactDigest = item.second;
    role.recipeDigest = inputs.artifacts.recipeDigest;
    role.roleKind = "PIPELINE_RANGE";
    role.adapterId = "qwen";
    role.adapterVersion = "1";
    role.modelManifestDigest = inputs.artifacts.manifestDigest;
    role.artifactProfileDigest = nativePlanningDigest("fixture-profile");
    role.graphDigest = inputs.artifacts.graphDigest;
    role.canonicalInitializerDigest = nativePlanningDigest("fixture-initializers");
    role.adapterDescriptorDigest = nativePlanningDigest("fixture-adapter");
    role.assemblerDescriptorDigest = nativePlanningDigest("fixture-assembler");
    role.backendAbi = "onnxruntime-cpu-v1";
    role.nodeIndices = {0};
    role.expectedInputs = {{"x", "float32", {std::int64_t(1), std::string("batch")}}};
    role.expectedOutputs = {{"y", "float32", {std::int64_t(1), std::string("batch")}}};
    role.precision = "fp32";
    role.quantization = "none";
    role.layout = "native";
    role.padding = "none";
    role.protectionEpoch = inputs.protectionEpoch;
    role.maxSourceBytes = 4096;
    role.maxAssembledBytes = 8192;
    role.maxNodes = 16;
    inputs.assemblyByRole.emplace(item.first, role);
  }
}

inline NativeRoleProjectionInputs projection(const NativeSealedPlan& sealed, const std::string& provider)
{
  auto role = sealed.core.assemblyByRole.begin()->first;
  for (const auto& assignment : sealed.core.assignment.providerByRole) {
    if (assignment.second == provider) role = assignment.first;
  }
  const auto& assembly = sealed.core.assemblyByRole.at(role);
  NativeRoleProjectionInputs inputs;
  inputs.executionRole = {role, assembly.role, assembly.rank, assembly.layerBegin,
    assembly.layerEnd, assembly.backend, assembly.adapterId, assembly.adapterVersion};
  inputs.dataflow.requestId = sealed.core.requestId;
  inputs.dataflow.attempt = sealed.core.attempt;
  inputs.dataflow.planDigest = sealed.planDigest;
  inputs.dataflow.role = role;
  inputs.dataflow.terminalResponseOwner = true;
  inputs.dataflow.dataflowDigest = nativePlanningDigest("fixture-dataflow");
  inputs.deviceBinding = {"CPU", provider, role, sealed.core.offerDigestByProvider.begin()->second,
    nativePlanningDigest("fixture-topology"), nativePlanningDigest("fixture-resources"), 1, "", "EXCLUSIVE"};
  const auto offer = sealed.core.offerDigestByProvider.find(provider);
  if (offer != sealed.core.offerDigestByProvider.end()) inputs.deviceBinding.offerDigest = offer->second;
  return inputs;
}

} // namespace ndnsf::di::fixture
