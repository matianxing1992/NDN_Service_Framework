#ifndef NDNSF_DISTRIBUTED_INFERENCE_GENERATION_EPOCH_LINEAGE_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_GENERATION_EPOCH_LINEAGE_HPP

#include <cstdint>
#include <string>

namespace ndnsf::di {

/**
 * Authenticated request-scoped token lineage carried inside encrypted DATA_V1
 * dependency payloads.  It contains bounded identity metadata only; Provider-
 * local KV tensors never leave the Provider through this structure.
 */
struct GenerationEpochLineageV1
{
  // The transition phase is authenticated along with the epoch.  Keep the
  // wire representation textual so older diagnostic tooling can inspect it,
  // but validate it as a closed enum at the protocol boundary.
  static constexpr const char* PREFILL = "PREFILL";
  static constexpr const char* DECODE = "DECODE";
  static constexpr const char* CHECKPOINT_FINALIZE = "CHECKPOINT_FINALIZE";

  std::string requestId;
  std::uint64_t attemptEpoch = 0;
  std::string planDigest;
  std::string generationId;
  std::uint64_t streamEpoch = 0;
  std::uint64_t inferenceEpoch = 0;
  std::string transitionKind = DECODE;
  std::string logicalPrefixDigest;
  std::uint32_t logicalPrefixTokenCount = 0;
  std::string positionDigest;
  std::string producerRole;
  std::string consumerRole;
  std::uint64_t operationIndex = 0;

  /** Validate authenticated request/generation state before edge-local routing
   *  fields are bound by the executing Provider. */
  void validateCore() const;

  /** Validate the complete wire/publication form, including edge routing. */
  void validate() const;

  /** Compare the shared generation state while ignoring edge-local routing. */
  bool sameGenerationState(const GenerationEpochLineageV1& other) const noexcept;

  bool operator==(const GenerationEpochLineageV1& other) const noexcept;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_GENERATION_EPOCH_LINEAGE_HPP
