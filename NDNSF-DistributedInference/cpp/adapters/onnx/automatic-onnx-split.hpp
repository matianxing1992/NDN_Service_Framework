#ifndef NDNSF_DI_AUTOMATIC_ONNX_SPLIT_HPP
#define NDNSF_DI_AUTOMATIC_ONNX_SPLIT_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-model.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Structural splitter for an admitted automatic ONNX source.
 *
 * The strategy owns immutable source facts and never copies weights, reads a
 * file or selects a Provider. A cut is one legal source-graph edge whose two
 * sides each retain a complete source-proven state pair; otherwise no candidate
 * is returned instead of inventing a stateless generation role.
 */
class AutomaticOnnxSplitStrategy final : public NativeModelSplitStrategy,
                                         public CooperativeModelSplitStrategy
{
public:
  explicit AutomaticOnnxSplitStrategy(std::shared_ptr<const AutomaticOnnxModel> model);

  NativeStrategyIdentity identity() const override;

  /** Return the planning graph that was inspected with the source facts. */
  NativeGraphSnapshot inspectGraph(const NativeModelDescriptor& model,
                                   const std::string& revision,
                                   std::uint64_t maxNodes) const;

  std::vector<NativeSplitCandidate> enumerate(
    const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
    const NativeCandidateBudget& budget) const override;

  std::vector<NativeSplitCandidate> enumerate(
    const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
    const NativeCandidateBudget& budget, const ExtensionControl& control) const override;

private:
  std::vector<NativeSplitCandidate> enumerateImpl(
    const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
    const NativeCandidateBudget& budget, const ExtensionControl* control) const;

  std::shared_ptr<const AutomaticOnnxModel> m_model;
};

/** One stage's source-derived ports. Activation tensors keep canonical graph
 * names; state names are the bare export names. */
struct AutomaticOnnxStageBinding
{
  std::string role;
  std::vector<std::string> nodeIds;
  std::vector<std::string> activationInputs;
  std::vector<std::string> activationOutputs;
  std::vector<std::string> stateInputs;
  std::vector<std::string> stateOutputs;
  bool terminal = false;
};

/** Derive and check the stage projection of one candidate against the
 * immutable source proof. Rejects an unassigned node, a state pair with no
 * single owner or with two owners, a cut that is not a real producer/consumer
 * boundary, a role without a state pair, or logits owned by a non-terminal
 * role. */
/** Planning-side runner contract of one stage. The authenticated wire form is
 * not frozen; this value is stored in the runner metadata for admission. */
struct AutomaticOnnxStageRunnerContract
{
  std::string role;
  bool terminal = false;
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;

  std::string canonicalJson() const;
  static AutomaticOnnxStageRunnerContract fromJson(const std::string& encoded);
};

/** Check that a stage runner exposes exactly the ports of its projection: no
 * full-source port set, no logits on a non-terminal stage, and no missing
 * frontier or state port. */
std::optional<std::string>
checkAutomaticOnnxStageRunner(const NativeModelRunnerSpec& spec,
                              const AutomaticOnnxStageBinding& binding,
                              const NativeSplitCandidate& candidate,
                              const AutomaticOnnxContract& contract);

std::vector<AutomaticOnnxStageBinding>
deriveAutomaticOnnxStageBindings(const AutomaticOnnxModel& model,
                                 const NativeSplitCandidate& candidate);

} // namespace ndnsf::di

#endif // NDNSF_DI_AUTOMATIC_ONNX_SPLIT_HPP
