#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Explicitly choose declared semantic types or the inspected source boundary. */
enum class NativeStateContractBinding { DECLARED, SOURCE };

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

/** Actual source graph ports crossing a selected semantic region. */
struct NativeSourceRegionBoundary
{
  std::map<std::string, NativeAssemblyTensorContractV3> inputs;
  std::map<std::string, NativeAssemblyTensorContractV3> outputs;
  std::string identityDigest;
};

/** Resolve the exact original graph frontier for a set of canonical node ordinals. */
NativeSourceRegionBoundary
inspectNativeSourceBoundary(const NativeGraphSnapshot& graph, const NativeJson& metadata,
                            const std::set<std::uint64_t>& owned,
                            const std::vector<std::string>& additionalOutputs,
                            const std::function<void()>& requireActive);

/** Source-checked recipe producer. Semantic node mappings come from the
 * authenticated adapter/catalog; ordinal equality is never assumed. The
 * owner retains inspected metadata only, not another copy of model bytes. */
class NativeCanonicalRolePreparer
{
public:
  using NodeMap = std::map<std::string, std::vector<std::uint64_t>>;
  NativeCanonicalRolePreparer(NativeInspectedModel model, const NativeCanonicalSource& source,
    NativeRoleRecipeProfile profile, const NativeAssemblyControl& control, NodeMap mapping = {});
  NativeCanonicalRolePreparer(NativeInspectedModel model, NativeOnnxGraphInspection sourceGraph,
    NativeRoleRecipeProfile profile, const NativeAssemblyControl& control, NodeMap mapping = {});
  NativeRequestPreparation::RolePort rolePort() const;
  /** Inspect a region before constructing its execution plan. Additional outputs must
   * be produced by the region; no placeholder tensors or candidate are accepted.
   */
  NativeSourceRegionBoundary sourceRegion(const std::vector<std::string>& nodeIds,
    const std::vector<std::string>& additionalOutputs, const NativeRequestControl& control) const;
  /** Before candidate selection only: returns a new, fully digested candidate.
   * DECLARED preserves semantic cover/type checks. SOURCE uses the explicit
   * mapping and checked boundary types without trusting abstract placeholders.
   */
  NativeSplitCandidate bindStateContracts(const NativeInspectedModel& model,
    const NativeSplitCandidate& candidate, const NativeStateTensorMapping& mapping,
    const NativeRequestControl& control,
    NativeStateContractBinding binding = NativeStateContractBinding::DECLARED) const;
  std::vector<NativeSelectionRoleV3> prepare(const NativeInspectedModel& model,
    const NativeSplitCandidate& candidate, const NativeRequestControl& control) const;

private:
  void validateFrozen(const NativeAssemblyControl& control);
  NativeInspectedModel m_model;
  NativeRoleRecipeProfile m_profile;
  NativeOnnxGraphInspection m_sourceGraph;
  NodeMap m_mapping;
};

} // namespace ndnsf::di
