#ifndef NDNSF_DI_NATIVE_YOLO_PLANNER_HPP
#define NDNSF_DI_NATIVE_YOLO_PLANNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

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
  std::string candidateDigest;
};

/** Native counterpart of the registered YOLO candidate splitter. */
class NativeYoloComponentSplit final : public NativeModelSplitStrategy
{
public:
  explicit NativeYoloComponentSplit(
    std::vector<NativeYoloComponentSpec> candidates);

  NativeStrategyIdentity identity() const override;
  std::vector<NativeSplitCandidate> enumerate(
    const NativeModelDescriptor& model,
    const NativeGraphSnapshot& graph,
    const NativeCandidateBudget& budget) const override;

private:
  std::vector<NativeYoloComponentSpec> m_candidates;
};

} // namespace ndnsf::di::yolo

#endif // NDNSF_DI_NATIVE_YOLO_PLANNER_HPP
