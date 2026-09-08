#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"

namespace ndnsf::di {
NativeJson nativeAssemblyJson(const NativeSelectionRoleV3& role);
NativeJson nativeDependenciesJson(const std::vector<NativeDependencySpec>& dependencies);
NativeJson nativeGenerationJson(const NativeGenerationExecutionContractV1& generation);
void validateNativeAssembly(const NativeSelectionRoleV3& role);
}
