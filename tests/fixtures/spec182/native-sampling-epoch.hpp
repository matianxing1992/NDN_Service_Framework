#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp"

namespace ndnsf::di::test {
// Defined beside the existing epoch fixtures. The real coordinator invokes
// its private sampler; this helper supplies tensors and transport only.
NativeEpochCoordinatorResult runSamplingEpochs(
  std::vector<std::vector<float>> logits,
  const std::function<void(NativeEpochCoordinatorConfig&)>& configure);
}
