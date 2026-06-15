#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_ARTIFACT_MATERIALIZER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_ARTIFACT_MATERIALIZER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <istream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeArtifactMaterializerOptions
{
  std::string cacheDir = "/tmp/ndnsf-di-native-artifacts";
  std::function<std::vector<std::uint8_t>(const std::string& objectName,
                                          const std::string& repoManifestJson)> repoFetchFromManifest;
  std::function<std::vector<std::uint8_t>(const std::string& objectName)> repoFetch;
};

std::map<std::string, NativeModelRunnerSpec>
materializeNativeModelArtifactsFromReferencesJson(
  const std::map<std::string, NativeModelRunnerSpec>& specsByRole,
  std::istream& artifactReferences,
  const NativeArtifactMaterializerOptions& options = {});

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_ARTIFACT_MATERIALIZER_HPP
