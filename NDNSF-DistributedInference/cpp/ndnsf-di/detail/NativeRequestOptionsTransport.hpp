#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"

namespace ndnsf::di::detail {

// Deployment opt-in; the public encoder always retains INLINE wire parity.
NativeEncodedRequest encodeNativeRequestEnvelopeForRuntime(
  const NativeModelDescriptor& model, const NativeApplicationInput& input,
  const NativeRequestContract& contract, const std::string& requestId,
  std::uint64_t attempt, std::uint64_t deadlineMs,
  const std::optional<NativeGenerationRecovery>& recovery,
  const std::vector<std::string>& candidateProviders,
  const std::optional<NativeCandidateArtifactInventoryV4>& candidateArtifactInventory,
  const std::optional<NativePlacementProbeAllowlistV4>& probeAllowlist,
  std::string requesterIdentity, const std::string& optionsTransport);

// Returns true only for the explicit Selection generation mode. Throws for
// an invalid native mode or a deployment that has not enabled that mode.
// Legacy payloads and INLINE requests retain their existing receiver path.
bool requiresSelectionGenerationOptions(const std::string& requestWire,
                                        bool allowSelectionGenerationOptions);

} // namespace ndnsf::di::detail
