#ifndef NDNSF_DI_NATIVE_SOURCE_PARTITION_STRATEGY_HPP
#define NDNSF_DI_NATIVE_SOURCE_PARTITION_STRATEGY_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

enum class NativeSourcePartitionMode
{
  FULL,
  COMPONENT,
  HYBRID,
};

/** Verified facts for one contiguous pair of FFN projections. */
struct NativeSourceFfnWindow
{
  NativeOnnxSourceTransformV1 transform;
  std::vector<std::string> nodes;
  std::vector<std::int64_t> inputShape;
  std::vector<std::int64_t> outputShape;
  std::uint64_t weightBytes = 0;
};

/** Declarative mode and source boundary for a generic ONNX source plan. */
struct NativeSourcePartitionProfile
{
  NativeSourcePartitionMode mode = NativeSourcePartitionMode::FULL;
  std::string boundaryNode;
  std::vector<std::string> roles;
  std::string inputIngressRole;
  std::string resultEgressRole;
  std::string postprocessingJson = "{}";
  std::uint64_t inputChannels = 0;
  std::string convInputAxis = "input-channel";
  std::vector<std::int64_t> activationShape;
  std::vector<std::int64_t> partialShape;
  std::string activationDtype = "float32";
  std::string activationLayoutDigest;
  std::uint64_t convWeightBytes = 0;
  std::uint64_t convBiasBytes = 0;
  std::string convInputTensor;
  std::string convWeightInitializer;
  std::string convBiasInitializer;
  std::string convOutputTensor;
  std::vector<std::int64_t> convPads;
  std::vector<std::int64_t> convStrides;
  std::vector<std::int64_t> convDilations;
  std::optional<std::uint64_t> suffixWeightBytes;
  std::map<std::string, std::uint64_t> denseWeightBytesByRole;
  std::vector<NativeSourceFfnWindow> ffnWindows;
  std::optional<std::uint64_t> ffnPrefixWeightBytes;
};

/**
 * Build full, component and hybrid candidates from verified source facts.
 *
 * Full and component use ordinary source regions. Hybrid lowers a selected
 * degree-two Conv or paired projection region between single-role stages.
 */
class NativeSourcePartitionStrategy final : public NativeModelSplitStrategy,
                                             public CooperativeModelSplitStrategy
{
public:
  explicit NativeSourcePartitionStrategy(NativeSourcePartitionProfile profile);

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

  NativeSourcePartitionProfile m_profile;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_SOURCE_PARTITION_STRATEGY_HPP
