#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp"

#include <memory>

namespace ndnsf::di::test {
// Reuse the maintained, cancellable loopback transport with real native runners.
std::shared_ptr<DependencyIo> makeSamplingDependencyIo();

// Defined beside the existing epoch fixtures. The real coordinator invokes
// its private sampler; this helper supplies tensors and transport only.
NativeEpochCoordinatorResult runSamplingEpochs(
  std::vector<std::vector<float>> logits,
  const std::function<void(NativeEpochCoordinatorConfig&)>& configure,
  std::size_t rowsPerEpoch = 1);
}
