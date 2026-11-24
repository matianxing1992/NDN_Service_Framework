#ifndef NDNSF_DI_WAN_PREPARATION_HPP
#define NDNSF_DI_WAN_PREPARATION_HPP

#include "wan-summed-projection.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalRolePreparer.hpp"

#include <filesystem>

namespace ndnsf::di::wan {

struct CompiledComponent
{
  NativeCanonicalSource source;
  std::string configurationJson;
};

/** Compile a lowered component into the maintained ONNX source catalog.
 * The catalog supplies the model descriptor, source name and manifest, recipe,
 * publication, and explicit role profiles. Graph facts and plans are derived.
 * An optional initializer source must describe the lowered model's sidecar.
 */
CompiledComponent
compileWanComponent(onnx::ModelProto model, OnnxParallelPlan layout,
                    std::uint64_t maxExactBundleBytes, NativeCanonicalSource initializerSource,
                    NativeJson catalog, const NativeAssemblyControl& control);

/** Compile a lowered component already held in the canonical source representation. */
CompiledComponent
compileWanComponent(NativeCanonicalSource source, OnnxParallelPlan layout,
                    std::uint64_t maxExactBundleBytes, NativeJson catalog,
                    const NativeAssemblyControl& control);

/** Write the source graph, optional bounded sidecar and catalog.json as one new directory.
 * The directory's parent must exist. The returned catalog uses relative file references.
 */
std::filesystem::path
writeWanComponent(const CompiledComponent& component, const std::filesystem::path& directory,
                  const NativeAssemblyControl& control);

/** Verify explicit checkpoint files, lower one component and commit its ONNX catalog. */
std::filesystem::path
prepareWanCheckpointComponent(NativeJson request, const std::filesystem::path& base,
                              const std::filesystem::path& directory,
                              const NativeAssemblyControl& control);

/** Bind a compiled Wan component to the inspected graph used by preparation. */
NativeSplitCandidate
makeWanSplitCandidate(const NativeInspectedModel& model, const NativeOnnxGraphInspection& source,
                      const OnnxParallelPlan& layout, const std::string& serviceName,
                      std::map<std::string, NativeRoleResourceRequirement> resources,
                      const NativeRequestControl& control);

/** Use the inspected canonical node IDs as the shared publication units. */
NativeCanonicalRolePreparer::NodeMap
makeWanMaterialNodeMap(const NativeOnnxGraphInspection& source);

class NativeWanSplitStrategy final : public NativeModelSplitStrategy,
                                     public CooperativeModelSplitStrategy
{
public:
  explicit NativeWanSplitStrategy(NativeSplitCandidate candidate);

  NativeStrategyIdentity identity() const override;
  std::vector<NativeSplitCandidate>
  enumerate(const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
            const NativeCandidateBudget& budget) const override;
  std::vector<NativeSplitCandidate>
  enumerate(const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
            const NativeCandidateBudget& budget, const ExtensionControl& control) const override;

private:
  NativeSplitCandidate m_candidate;
};

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_PREPARATION_HPP
