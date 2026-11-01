#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace ndnsf::di {

struct NativeAssemblyTensorContractV3
{
  std::string name;
  std::string dtype;
  // Wire identity distinguishes numeric dimensions from symbolic strings.
  std::vector<std::variant<std::int64_t, std::string>> shape;
};

struct NativeSelectionRoleV3
{
  std::string role;
  std::string selectedRole;
  std::uint64_t rank = 0;
  std::uint64_t layerBegin = 0;
  std::uint64_t layerEnd = 0;
  std::string backend;
  std::vector<std::string> deviceSet;
  std::uint64_t requiredDeviceMemoryMb = 0;
  std::string artifactDigest;
  std::string recipeDigest;
  std::string roleKind;
  std::string adapterId;
  std::string adapterVersion;
  std::string modelManifestDigest;
  std::string artifactProfileDigest;
  std::string graphDigest;
  std::string canonicalInitializerDigest;
  std::string adapterDescriptorDigest;
  std::string assemblerDescriptorDigest;
  std::string backendAbi;
  std::vector<std::uint64_t> nodeIndices;
  std::vector<NativeAssemblyTensorContractV3> expectedInputs;
  std::vector<NativeAssemblyTensorContractV3> expectedOutputs;
  // Internal worker binding: the parent rebuilt this role from the
  // authenticated post-Selection material manifest, so the worker receives
  // a compact role graph rather than the complete canonical graph.
  bool materializedRole = false;
  std::string precision;
  std::string quantization;
  std::string layout;
  std::string padding;
  std::string protectionEpoch = "plaintext-v1";
  // Adapter-declared terminal postprocessing. NATIVE_POSTPROCESS has no
  // model-layer artifact; the Provider consumes dependency tensors directly.
  std::string mergeKind;
  std::string postprocessIdentity;
  std::string postprocessOutputName;
  double postprocessConfidenceThreshold = 0.0;
  std::string postprocessSort;
  std::uint64_t maxSourceBytes = 0;
  std::uint64_t maxAssembledBytes = 0;
  std::uint64_t maxNodes = 0;
};

} // namespace ndnsf::di
