#ifndef NDNSF_DI_NATIVE_PROTECTED_PROVIDER_HPP
#define NDNSF_DI_NATIVE_PROTECTED_PROVIDER_HPP
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include <functional>
namespace ndnsf::di {
struct NativeProviderHandlerConfig;
struct NativeProtectedGrantConfig;
// Resolve the operator registry and the selected Provider's recipient key.
NativeProtectedGrantConfig loadNativeProtectedGrantConfig(
  const std::string& provider, const std::string& modelFamily,
  const std::string& protectionEpoch);
// Exact Data transport; authorization remains in ProtectedRuntime's verifier.
std::string fetchNativeProtectedGrant(
  const std::string& name, int timeoutMs,
  const std::function<bool()>& cancelled = {});
// Generated service plans retain the /Model/<family> operator spelling;
// the artifact-policy registry stores the family component itself.
inline std::string nativeProtectedModelFamily(const std::string& model)
{
  const std::string prefix = "/Model/";
  if (model.compare(0, prefix.size(), prefix) == 0 &&
      model.find('/', prefix.size()) == std::string::npos) {
    return model.substr(prefix.size());
  }
  return model;
}
std::string nativeProtectedFencingToken(
  const NativeSelectionProjectionV3& projection, const std::string& providerBootId,
  const std::map<std::string, std::string>& assignmentFields);
void installNativeProtectedGrantFactory(NativeProviderHandlerConfig& config);
}
#endif
