#ifndef NDNSF_DI_AUTOMATIC_ONNX_BACKEND_HPP
#define NDNSF_DI_AUTOMATIC_ONNX_BACKEND_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-model.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Capabilities an offer must declare for one automatic source. The strings are
 * a planning-side contract only; the authenticated offer/readiness wire
 * extension remains open, and a missing capability is never a fallback. */
struct AutomaticOnnxBackendRequirement
{
  std::string contractSchema;
  std::vector<std::string> capabilities;
};

/** Derive the capability set from the immutable source contract. */
AutomaticOnnxBackendRequirement
automaticOnnxBackendRequirement(const AutomaticOnnxModel& model,
                                const std::string& contractSchema);

/** Return a definite rejection reason when a capability is absent. */
std::optional<std::string>
checkAutomaticOnnxBackend(const AutomaticOnnxBackendRequirement& requirement,
                          const NativeProviderPlanningView& offer);

} // namespace ndnsf::di

#endif // NDNSF_DI_AUTOMATIC_ONNX_BACKEND_HPP
