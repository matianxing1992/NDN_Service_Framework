#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_JSON_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_JSON_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"

#include <boost/property_tree/ptree_fwd.hpp>

#include <istream>
#include <map>
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
  std::uint64_t maxSourceBytes = 0;
  std::uint64_t maxAssembledBytes = 0;
  std::uint64_t maxNodes = 0;
};

struct NativeSelectionProjectionV3
{
  std::string provider;
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string planCoreDigest;
  std::string planDigest;
  std::string ackClosedDigest;
  std::string offerDigest;
  std::string securityPolicySnapshotDigest;
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
