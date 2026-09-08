#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

namespace ndnsf::di {

struct NativeRoleRecipeProfile
{
  std::string artifactProfileDigest, assemblerDescriptorDigest, backendAbi;
  std::string precision, quantization, layout, padding, protectionEpoch;
  std::uint64_t maxSourceBytes = 0, maxAssembledBytes = 0, maxNodes = 0;
};

/** Authenticated export configuration maps semantic state families to actual
 * source tensors. Shapes and dtypes are always read from the checked source. */
struct NativeStateTensorMapping
{
  using Roles = std::map<std::string, std::map<std::string, std::vector<std::string>>>;
  Roles inputs, outputs;
};

/** Source-checked recipe producer. Semantic node mappings come from the
 * authenticated adapter/catalog; ordinal equality is never assumed. The
 * owner retains inspected metadata only, not another copy of model bytes. */
class NativeCanonicalRolePreparer
{
public:
  using NodeMap = std::map<std::string, std::vector<std::uint64_t>>;
  NativeCanonicalRolePreparer(NativeInspectedModel model, const NativeCanonicalSource& source,
    NativeRoleRecipeProfile profile, const NativeAssemblyControl& control, NodeMap mapping = {});
  NativeRequestPreparation::RolePort rolePort() const;
  // Before candidate selection only: returns a new, fully digested candidate.
  NativeSplitCandidate bindStateContracts(const NativeInspectedModel& model,
    const NativeSplitCandidate& candidate, const NativeStateTensorMapping& mapping,
    const NativeRequestControl& control) const;
  std::vector<NativeSelectionRoleV3> prepare(const NativeInspectedModel& model,
    const NativeSplitCandidate& candidate, const NativeRequestControl& control) const;

private:
  NativeInspectedModel m_model;
  NativeRoleRecipeProfile m_profile;
  NativeOnnxGraphInspection m_sourceGraph;
  NodeMap m_mapping;
};

} // namespace ndnsf::di
