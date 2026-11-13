#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-backend.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace ndnsf::di {

AutomaticOnnxBackendRequirement
automaticOnnxBackendRequirement(const AutomaticOnnxModel& model,
                                const std::string& contractSchema)
{
  model.descriptor.validate();
  if (!model.contract.generation)
    throw std::invalid_argument("automatic backend requirement needs a generation proof");
  if (contractSchema.empty())
    throw std::invalid_argument("automatic backend requirement needs a contract schema");
  AutomaticOnnxBackendRequirement result;
  result.contractSchema = contractSchema;
  const std::string backend = model.descriptor.adapter.backends.empty() ?
    std::string("onnxruntime") : model.descriptor.adapter.backends.front();
  std::set<std::string> capabilities{"automatic-onnx", result.contractSchema, backend};
  for (const auto& state : model.contract.generation->states)
    capabilities.insert(std::string("automatic-dtype:") + tensorElementTypeName(state.elementType));
  if (!model.descriptor.quantizationSubtype.empty())
    capabilities.insert(std::string("automatic-quant:") + model.descriptor.quantizationSubtype);
  result.capabilities.assign(capabilities.begin(), capabilities.end());
  return result;
}

std::optional<std::string>
checkAutomaticOnnxBackend(const AutomaticOnnxBackendRequirement& requirement,
                          const NativeProviderPlanningView& offer)
{
  offer.validate();
  for (const auto& capability : requirement.capabilities) {
    if (std::find(offer.capabilities.begin(), offer.capabilities.end(), capability) ==
        offer.capabilities.end())
      return "AUTOMATIC_BACKEND_CAPABILITY_MISSING:" + capability;
  }
  return std::nullopt;
}

} // namespace ndnsf::di
