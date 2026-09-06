#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"

#include <atomic>
#include <unistd.h>

namespace ndnsf::di {

void
bindNativeRunnerPreparationContext(NativeModelRunnerSpec& spec,
                                   const NativeSelectionProjectionV3& projection,
                                   const NativeRunnerPreparationContext& context)
{
  spec.metadata["evidence.providerName"] = context.providerName;
  spec.metadata["evidence.providerBootId"] = context.providerBootId;
  spec.metadata["evidence.epoch"] = "1";
  spec.metadata["evidence.createdAtMs"] = std::to_string(context.providerStartedAtMs);
  spec.metadata["evidence.planDigest"] = projection.planDigest;
  spec.metadata["evidence.modelDigest"] = projection.assembly.modelManifestDigest.empty()
    ? projection.planDigest : projection.assembly.modelManifestDigest;
  spec.metadata["evidence.artifactDigest"] = projection.assembly.artifactDigest;

  // Observing adapters consume this unique request profile context. Other
  // adapters may ignore it; no model name or generation mode is required.
  static std::atomic<std::uint64_t> profileSequence{0};
  spec.metadata["providerProfilePrefix"] = context.cacheDirectory +
    "/ort-profile-" + std::to_string(getpid()) + "-" +
    std::to_string(profileSequence.fetch_add(1));
  spec.metadata["profileAfterRequest"] = "true";
}

} // namespace ndnsf::di
