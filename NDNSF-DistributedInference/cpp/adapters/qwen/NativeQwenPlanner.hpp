#ifndef NDNSF_DI_NATIVE_QWEN_PLANNER_HPP
#define NDNSF_DI_NATIVE_QWEN_PLANNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di::qwen {

/** Native counterpart of the maintained three-stage Qwen splitter.
 *
 * The adapter consumes an already inspected, digest-bound graph.  It does not
 * read model files, contact a Provider, or choose a placement.
 */
class NativeQwenLayerSplit final : public NativeModelSplitStrategy,
                                   public CooperativeModelSplitStrategy
{
public:
  using LayerRange = std::pair<std::uint64_t, std::uint64_t>;

  NativeQwenLayerSplit(
    std::vector<LayerRange> layerRanges,
    std::map<std::string, std::string> artifactDigestsByRole,
    std::map<std::string, std::uint64_t> weightBytesByRole,
    std::vector<std::string> roles = {
      "/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1",
      "/LLM/Pipeline/Stage/2"},
    std::vector<std::uint64_t> tensorDegrees = {1, 1, 1},
    std::string inputIngressRole = {},
    std::string resultEgressRole = {},
    std::string modelFamily = "qwen");

  NativeStrategyIdentity identity() const override;
  /** Build the maintained semantic graph from pinned model metadata. This is
   * not a mapping from decoder layers to canonical ONNX node indices. */
  NativeGraphSnapshot inspectGraph(const NativeModelDescriptor& model,
    const std::string& revision, std::uint64_t maxNodes) const;
  std::vector<NativeSplitCandidate> enumerateFromMetadata(const NativeModelDescriptor& model,
    const std::string& revision, std::uint64_t maxNodes, const NativeCandidateBudget& budget) const;
  std::vector<NativeSplitCandidate> enumerate(
    const NativeModelDescriptor& model,
    const NativeGraphSnapshot& graph,
    const NativeCandidateBudget& budget) const override;

  std::vector<NativeSplitCandidate> enumerate(
    const NativeModelDescriptor& model,
    const NativeGraphSnapshot& graph,
    const NativeCandidateBudget& budget,
    const ExtensionControl& control) const override;

private:
  std::vector<NativeSplitCandidate> enumerateImpl(
    const NativeModelDescriptor& model,
    const NativeGraphSnapshot& graph,
    const NativeCandidateBudget& budget,
    const ExtensionControl* control) const;

  std::vector<LayerRange> m_layerRanges;
  std::map<std::string, std::string> m_artifactDigestsByRole;
  std::map<std::string, std::uint64_t> m_weightBytesByRole;
  std::vector<std::string> m_roles;
  std::vector<std::uint64_t> m_tensorDegrees;
  std::string m_inputIngressRole;
  std::string m_resultEgressRole;
  std::string m_modelFamily;
};

} // namespace ndnsf::di::qwen

#endif // NDNSF_DI_NATIVE_QWEN_PLANNER_HPP
