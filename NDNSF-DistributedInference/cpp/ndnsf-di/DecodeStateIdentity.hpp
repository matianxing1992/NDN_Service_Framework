#ifndef NDNSF_DISTRIBUTED_INFERENCE_DECODE_STATE_IDENTITY_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_DECODE_STATE_IDENTITY_HPP

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::di {

namespace decode_state_identity_detail {

inline bool
isSha256Digest(const std::string& value)
{
  static constexpr char PREFIX[] = "sha256:";
  return value.size() == sizeof(PREFIX) - 1 + 64 &&
         value.compare(0, sizeof(PREFIX) - 1, PREFIX) == 0 &&
         std::all_of(value.begin() + sizeof(PREFIX) - 1, value.end(),
                     [] (unsigned char ch) {
                       return (ch >= '0' && ch <= '9') ||
                              (ch >= 'a' && ch <= 'f');
                     });
}

inline void
requireDigest(const std::string& value, const char* label)
{
  if (!isSha256Digest(value)) {
    throw std::invalid_argument(
      std::string("invalid decode state ") + label + " digest");
  }
}

} // namespace decode_state_identity_detail

/** Exact request-scoped identity for one Provider-local decode state.
 *
 * This contract is shared by model adapters and the Provider runtime. A cache
 * key may index an entry compactly, but reuse is authorized only by equality
 * of this complete value.
 */
struct DecodeStateIdentityV1
{
  std::string modelDigest;
  std::string graphSemanticDigest;
  std::string artifactDigest;
  std::string adapterDigest;
  std::string tokenizerDigest;
  std::string runnerDigest;
  std::string roleName;
  std::string roleSplitDigest;
  std::uint32_t layerBegin = 0;
  std::uint32_t layerEnd = 0;
  std::string prefixDigest;
  std::uint32_t prefixTokenCount = 0;
  std::string positionDigest;
  std::string precision;
  std::string layoutDigest;
  std::string stateSchemaDigest;
  std::vector<std::string> stateComponentDigests;
  std::string runtimeAbiDigest;
  std::string securityDomainDigest;
  std::string providerIdentity;
  std::string providerBootId;
  // Token progress belongs to the state lineage, not to cacheEpoch. Prefill
  // produces state epoch 0 without a predecessor; decode epoch e consumes only
  // the state committed by e-1.
  std::uint64_t stateInferenceEpoch = 0;
  std::optional<std::uint64_t> predecessorInferenceEpoch;
  // Provider-local cache incarnation. It is stable for one lineage and changes
  // only when the runtime resets or invalidates the cache.
  std::uint64_t cacheEpoch = 0;
  std::string requestId;
  std::uint64_t attemptEpoch = 1;
  std::string generationId;

  void
  validate() const
  {
    using decode_state_identity_detail::requireDigest;
    requireDigest(modelDigest, "model");
    requireDigest(graphSemanticDigest, "graph semantic");
    requireDigest(artifactDigest, "artifact");
    requireDigest(adapterDigest, "adapter");
    requireDigest(tokenizerDigest, "tokenizer");
    requireDigest(runnerDigest, "runner");
    requireDigest(roleSplitDigest, "role split");
    requireDigest(prefixDigest, "prefix");
    requireDigest(positionDigest, "position");
    requireDigest(layoutDigest, "layout");
    requireDigest(stateSchemaDigest, "state schema");
    requireDigest(runtimeAbiDigest, "runtime ABI");
    requireDigest(securityDomainDigest, "security domain");
    if (roleName.empty() || precision.empty() || providerIdentity.empty() ||
        providerBootId.empty() || requestId.empty() || generationId.empty()) {
      throw std::invalid_argument("decode state identity is incomplete");
    }
    if (attemptEpoch < 1 || attemptEpoch > 2 || layerEnd <= layerBegin ||
        stateComponentDigests.empty()) {
      throw std::invalid_argument("decode state range/components are invalid");
    }
    if ((stateInferenceEpoch == 0 && predecessorInferenceEpoch) ||
        (stateInferenceEpoch > 0 &&
         (!predecessorInferenceEpoch ||
          *predecessorInferenceEpoch + 1 != stateInferenceEpoch))) {
      throw std::invalid_argument(
        "decode state predecessor inference epoch is invalid");
    }
    for (const auto& digest : stateComponentDigests) {
      requireDigest(digest, "state component");
    }
  }

  bool
  operator==(const DecodeStateIdentityV1& other) const
  {
    return modelDigest == other.modelDigest &&
           graphSemanticDigest == other.graphSemanticDigest &&
           artifactDigest == other.artifactDigest &&
           adapterDigest == other.adapterDigest &&
           tokenizerDigest == other.tokenizerDigest &&
           runnerDigest == other.runnerDigest && roleName == other.roleName &&
           roleSplitDigest == other.roleSplitDigest &&
           layerBegin == other.layerBegin && layerEnd == other.layerEnd &&
           prefixDigest == other.prefixDigest &&
           prefixTokenCount == other.prefixTokenCount &&
           positionDigest == other.positionDigest &&
           precision == other.precision && layoutDigest == other.layoutDigest &&
           stateSchemaDigest == other.stateSchemaDigest &&
           stateComponentDigests == other.stateComponentDigests &&
           runtimeAbiDigest == other.runtimeAbiDigest &&
           securityDomainDigest == other.securityDomainDigest &&
           providerIdentity == other.providerIdentity &&
           providerBootId == other.providerBootId &&
           stateInferenceEpoch == other.stateInferenceEpoch &&
           predecessorInferenceEpoch == other.predecessorInferenceEpoch &&
           cacheEpoch == other.cacheEpoch && requestId == other.requestId &&
           attemptEpoch == other.attemptEpoch &&
           generationId == other.generationId;
  }

  bool
  operator!=(const DecodeStateIdentityV1& other) const
  {
    return !(*this == other);
  }
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_DECODE_STATE_IDENTITY_HPP
