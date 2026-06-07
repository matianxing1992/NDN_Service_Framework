#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_SERVICE_MANIFEST_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_SERVICE_MANIFEST_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <istream>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::di {

std::vector<NativeModelRunnerSpec>
nativeModelRunnerSpecsForServiceManifestFromJson(std::istream& input,
                                                 const std::string& serviceName);

std::map<std::string, NativeModelRunnerSpec>
nativeModelRunnerSpecsByRoleForServiceManifestFromJson(std::istream& input,
                                                       const std::string& serviceName);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_SERVICE_MANIFEST_HPP
