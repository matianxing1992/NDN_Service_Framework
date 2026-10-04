#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

namespace ndnsf::di {

struct NativePreparedPlanningCache;

struct NativeCandidateArtifactInventoryV4
{
  std::vector<NativeJson> identities;
  std::string digest;
};

/** Build the exact placement-independent artifact inventory from the same
 * prepared candidate prefix considered by the planner. `nullopt` means the
 * inventory is unavailable or exceeds the V4 bound and the request must use
 * the V3 profile. */
std::optional<NativeCandidateArtifactInventoryV4>
buildNativeCandidateArtifactInventoryV4(
  const NativePreparedPlanningCache& planningCache, std::size_t maxCandidates);

struct NativePlacementProbePairV4
{
  std::string sourceProvider;
  std::string destinationProvider;

  bool operator<(const NativePlacementProbePairV4& other) const
  {
    return sourceProvider < other.sourceProvider ||
      (sourceProvider == other.sourceProvider &&
       destinationProvider < other.destinationProvider);
  }
};

struct NativePlacementProbeAllowlistV4
{
  // For RTT, sourceProvider is the observer and destinationProvider is the
  // target. Goodput uses the same pair shape for payload source and receiver.
  std::vector<NativePlacementProbePairV4> rttPairs;
  std::vector<NativePlacementProbePairV4> goodputEdges;
};

/** Derive the deterministic bounded Provider pair scope from the prepared
 * prefix. If the graph needs more than the one-shot cap, the canonical roster
 * order selects the first pairs and every omitted pair remains unknown. */
std::optional<NativePlacementProbeAllowlistV4>
buildNativePlacementProbeAllowlistV4(
  const NativePreparedPlanningCache& planningCache,
  std::size_t maxCandidates,
  const std::vector<std::string>& candidateProviders);

struct NativeGenerationRecovery
{
  std::string generationId;
  std::string originalRequestId;
  std::string recoveryRequestId;
  std::string originalInputManifestDigest;
  std::string priorPlanDigest;
  std::string failedProvider;
  std::vector<std::int64_t> committedTokenIds;
};

struct NativeEncodedRequest
{
  std::vector<std::uint8_t> wire;
  std::string modelIntentDigest;
  std::string logicalInputDigest;
  std::string inputManifestDigest;
  std::string invocationId;
  std::string requestContractDigest;
  std::string requesterIdentity;
  std::vector<NativeJson> candidateArtifacts;
  std::vector<std::string> candidateProviders;
  std::string candidateSetDigest;
  std::string candidateArtifactSetDigest;
  std::vector<NativeJson> allowedRttPairs;
  std::vector<NativeJson> allowedGoodputEdges;
  std::string probeScopeDigest;
  std::optional<NativeGenerationRecovery> recovery;
};

/** Return whether a request contract uses a generation mode understood by the
 * native requester/provider wire contract.  All public construction paths use
 * this predicate so direct C++ callers cannot bypass the JSON configuration
 * gate. */
bool
isSupportedNativeGenerationMode(const std::string& mode) noexcept;

/** Derive the generation values from the same application options bytes bound
 * into the request envelope. Placement adds stride and authenticated recovery
 * prefixes later; it cannot substitute different sampling or tokenizer data. */
NativeGenerationExecutionContractV1 nativeGenerationFromOptions(
  const std::vector<std::uint8_t>& options, const std::string& generationId);

/** Exact DIRequestEnvelopeV2 compatibility wire, shared by native requester
 * and offline SDK parity tests. Payload is already encoded by its adapter. */
NativeEncodedRequest encodeNativeRequestEnvelope(
  const NativeModelDescriptor& model, const NativeApplicationInput& input,
  const NativeRequestContract& contract, const std::string& requestId,
  std::uint64_t attempt, std::uint64_t deadlineMs,
  const std::optional<NativeGenerationRecovery>& recovery = std::nullopt,
  const std::vector<std::string>& candidateProviders = {},
  const std::optional<NativeCandidateArtifactInventoryV4>& candidateArtifactInventory = std::nullopt,
  const std::optional<NativePlacementProbeAllowlistV4>& probeAllowlist = std::nullopt,
  std::string requesterIdentity = {});

} // namespace ndnsf::di
