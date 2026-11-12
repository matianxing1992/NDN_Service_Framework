#ifndef NDNSF_DI_AUTOMATIC_ONNX_GENERATION_HPP
#define NDNSF_DI_AUTOMATIC_ONNX_GENERATION_HPP

#include "automatic-onnx-model.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::di {

/** A finite source rule cannot prove generation; owner/resource errors differ. */
class AutomaticGenerationUnproven : public std::invalid_argument
{
public:
  explicit AutomaticGenerationUnproven(const char* reason)
    : std::invalid_argument(reason)
  {
  }
};

/** Recover unique full-cache pairs using formal operator ports and use-def paths.
 * Currently proves com.microsoft.GroupQueryAttention v1 with unpacked Q/K/V
 * and floating caches of the same dtype. Mixed T/T_CACHE and packed QKV are
 * outside this proof, independently of their possible runtime kernel support.
 * Identity, same-type Cast and reversible Transpose may surround cache ports.
 * Empty means no recognized attention; recognized but unprovable/ambiguous cache
 * boundaries throw. No runtime state, callbacks, weights or file handles retained.
 * Does not infer token, position, causal mask, total-length or request policy.
 */
std::vector<AutomaticAttentionState>
deriveAutomaticOnnxAttentionStates(const AutomaticOnnxContract& contract,
                                   const NativeAssemblyControl& control);

/** Derive source semantics only. Never trusts contract.generation as evidence. */
std::optional<AutomaticGenerationContract>
deriveAutomaticOnnxGeneration(const AutomaticOnnxContract& contract,
                               const NativeAssemblyControl& control);

/** Canonical fields; request policy and readiness are not serialized here. */
NativeJson
automaticOnnxGenerationJson(const std::optional<AutomaticGenerationContract>& generation);

} // namespace ndnsf::di

#endif // NDNSF_DI_AUTOMATIC_ONNX_GENERATION_HPP
