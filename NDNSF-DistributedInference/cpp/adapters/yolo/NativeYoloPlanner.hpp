#ifndef NDNSF_DI_NATIVE_YOLO_PLANNER_HPP
#define NDNSF_DI_NATIVE_YOLO_PLANNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

#include <map>
#include <string>
#include <vector>

namespace ndnsf::di::yolo {

struct NativeYoloComponentSpec
{
  std::string candidateId;
  int priority = 0;
  std::vector<std::string> roles;
  std::map<std::string, std::vector<std::string>> nodeNamesByRole;
  std::string inputIngressRole;
  std::string resultEgressRole;
  std::string mergeKind;
  /** Required registered-catalogue identity, not the complete SplitCandidate digest. */
  std::string candidateDigest;
};

/** Catalog identity plus the maintained semanticPartition document. */
struct NativeYoloCatalogComponent
{
  NativeYoloComponentSpec component;
  std::string semanticPartitionJson;
};

/** Native counterpart of the registered YOLO candidate splitter. */
class NativeYoloComponentSplit final : public NativeModelSplitStrategy,
                                       public CooperativeModelSplitStrategy
{
public:
  explicit NativeYoloComponentSplit(
    std::vector<NativeYoloComponentSpec> candidates, std::string postprocessingJson = "{}");

  /** Inspect actual source bytes and bind registered semantic partitions to
   * their graph. Source authentication remains with the native catalog owner. */
  static NativeYoloComponentSplit fromOnnxCatalog(
    const NativeModelDescriptor& model, const NativeCanonicalSource& source,
    const NativeAssemblyControl& control, std::vector<NativeYoloCatalogComponent> candidates,
    std::string postprocessingJson = "{}");

  NativeStrategyIdentity identity() const override;
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

  std::vector<NativeYoloComponentSpec> m_candidates;
  /** Adapter-owned terminal configuration, applied only to explicit Merge roles. */
  std::string m_postprocessingJson;
  std::string m_catalogModelDigest;
  std::optional<NativeGraphSnapshot> m_catalogGraph;
};

} // namespace ndnsf::di::yolo

#endif // NDNSF_DI_NATIVE_YOLO_PLANNER_HPP
