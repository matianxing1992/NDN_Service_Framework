#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_JSON_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_JSON_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ConversationStateBinding.hpp"

#include <boost/property_tree/ptree_fwd.hpp>

#include <istream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeAssemblyTensorContractV3
{
  std::string name;
  std::string dtype;
  std::vector<std::string> shape;
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

struct NativeGenerationExecutionContractV1
{
  bool enabled = false;
  std::string mode;
  std::size_t maxGeneratedTokens = 0;
  std::string tokenInputName;
  std::vector<std::string> stateInputNames;
  std::vector<std::string> stateOutputNames;
  std::vector<std::int64_t> eosTokenIds;
  std::string samplingDigest;
  std::string tokenizerDigest;
  std::string samplingMode = "Greedy";
  double samplingTemperature = 0.0;
  std::uint64_t samplingTopK = 1;
  double samplingTopP = 1.0;
  double samplingRepetitionPenalty = 1.0;
  std::uint64_t samplingSeed = 1'750'001;
  std::vector<std::string> stopStrings;
  std::string generationId;
  std::vector<std::int64_t> committedPrefixTokenIds;
  std::uint64_t streamingOperationStride = 0;
};

struct NativeSelectionProjectionV3
{
  std::string provider;
  std::string requestId;
  // The canonical root assigned by NDNSF Core. This is populated from the
  // authenticated CollaborationAssignment after Selection; it is deliberately
  // not requester-controlled JSON so a Provider assembly factory can fetch
  // only the root bound to its assignment.
  std::string canonicalArtifactName;
  std::uint64_t attempt = 0;
  std::string planCoreDigest;
  std::string planDigest;
  std::string ackClosedDigest;
  std::string offerDigest;
  std::string securityPolicySnapshotDigest;
  std::string requestContractDigest;
  std::uint64_t deadlineMs = 0;
  std::string groupCapabilityV1;
  bool hasGrantBinding = false;
  std::string grantName;
  std::string grantDigest;
  NativeSelectionRoleV3 selectedRole;
  NativeExecutionRoleV3 executionRole;
  NativeSelectionRoleV3 assembly;
  NativeRoleDataflowContractV3 dataflow;
  NativeDeviceBindingV3 deviceBinding;
  NativeGenerationExecutionContractV1 generationContract;
  std::optional<ConversationStateReferenceV1> conversationStateReference;
  std::optional<ConversationTurnBindingV1> conversationTurnBinding;
  NativeExecutionPlan plan;
};

std::vector<std::string>
stringArrayFromJson(const boost::property_tree::ptree& node, const std::string& key);

std::map<std::string, NativeExecutionPlan>
nativeExecutionPlansByServiceFromJson(std::istream& input);

NativeExecutionPlan
nativeExecutionPlanForServiceFromJson(std::istream& input, const std::string& serviceName);

NativeSelectionProjectionV3
nativeSelectionProjectionV3FromJson(std::istream& input,
                                    const std::string& selectedRole);

/** Validate the global one-role/one-Provider cover and complete named tensor
 * graph after all per-Provider projections have been decoded.
 */
void
validateNativeSelectionProjectionSetV3(
  const std::vector<NativeSelectionProjectionV3>& projections);

/**
 * Project only the local role's sealed mayPublish/mustFetch tensor endpoints
 * into executable dependency edges. The legacy plan.dependencies graph is not
 * authoritative for a V3 Selection.
 */
RoleSpec
roleSpecFromSelectionProjectionV3(
  const NativeSelectionProjectionV3& projection,
  const std::string& localProvider = "");

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_JSON_HPP
