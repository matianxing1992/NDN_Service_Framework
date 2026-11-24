#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include <boost/property_tree/ptree_fwd.hpp>
#include <string_view>

namespace ndnsf::di {
NativeJson nativeAssemblyJson(const NativeSelectionRoleV3& role);
// Compare the exact encoded fields without rebuilding a full role JSON object.
bool nativeAssemblyJsonMatches(const NativeSelectionRoleV3& role, const NativeJson& value);
NativeJson nativeDependenciesJson(const std::vector<NativeDependencySpec>& dependencies);
NativeJson nativeGenerationJson(const NativeGenerationExecutionContractV1& generation);
NativeJson nativeEndpointJson(const NativeTensorEndpointV3& endpoint);
NativeJson nativeDataflowJson(const NativeRoleDataflowContractV3& dataflow);
/** Validate the complete current flow, including its compact recipe commitment. */
void
validateNativeRoleDataflowContractV3(const NativeRoleDataflowContractV3& dataflow);
NativeJson nativeDeviceBindingJson(const NativeDeviceBindingV3& binding);
void validateNativeAssembly(const NativeSelectionRoleV3& role);
/** Verify that the selected recipe authorizes exactly the canonical CALL cuts. */
void validateNativeOnnxEntrySelection(const NativeExecutionPlan& plan,
                                      const NativeSelectionRoleV3& role);
/** Check Qwen rank membership and authenticate the original recipe before assembly. */
void
validateNativeQwenTensorAssembly(const NativeSelectionRoleV3& role,
                                 const std::vector<NativeStageLayout>& stages);
/** Check request-local automatic metadata before decoding or cache use.
 * Source-envelope reconstruction and object admission remain with the adapter. */
void validateNativeAutomaticSelectionBinding(const NativeSelectionProjectionV3& projection);
namespace detail {
/** Stable identity of the selected role's sealed entry and step bindings. */
std::string nativeRoleStepContractDigest(const NativeExecutionPlan& plan, std::string_view role);
// Reuse the complete endpoint admission contract at the naming leaf.
void validateTensorEndpointForNaming(const NativeTensorEndpointV3& endpoint);
/** Encode a complete step or port ID as a bounded public name component. */
std::string nativeTensorNamingCoordinate(const std::string& id);
// Internal wire path. root must be the unchanged read_json result of this exact
// wire, owned by the caller in the same callback-free scope. Strict wire/type
// parsing and the complete public semantic decoder still run here.
NativeSelectionProjectionV3 decodeNativeSelectionProjectionV3WithLegacyTree(
  const std::string& wire, const boost::property_tree::ptree& root,
  const std::string& selectedRole);
// Internal generated-value path: canonical serialization must succeed first.
// Uses the same semantic decoder as the fully checked public wire parser.
void validateNativeSelectionProjectionV3Value(const NativeJson& value,
                                             const NativeSelectionProjectionV3& generated);
}
}
