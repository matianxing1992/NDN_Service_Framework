#ifndef NDNSF_DI_NATIVE_ONNX_RANK_TRANSFERS_HPP
#define NDNSF_DI_NATIVE_ONNX_RANK_TRANSFERS_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace ndnsf::di {

struct NativeOnnxGraphInspection;
struct NativeOnnxNodeLayout;

/** Original operator endpoints, rank ownership and physical wire tensor type. */
struct NativeOnnxRankTransfer
{
  std::uint64_t producerOrdinal;
  std::uint64_t consumerOrdinal;
  std::size_t producerRank;
  std::size_t consumerRank;
  NativeTensorContract tensor;
  std::optional<NativeOnnxTransferRange> range;
};

/** Resolve internal cross-rank operands against the pinned source graph. */
std::vector<NativeOnnxRankTransfer>
deriveNativeOnnxRankTransfers(const NativeOnnxGraphInspection& inspection,
                              const std::vector<NativeOnnxNodeLayout>& layouts);

/** The step template and graph-defined terminal port. Artifact entries named
 * by the template must still be materialized and verified before execution. */
struct NativeOnnxTensorStepPlan
{
  NativeExecutionPlan plan;
  std::string terminalStep;
  std::map<std::string, NativeOnnxEntryPort> portsBySlot;
  std::array<NativeOnnxSourceEntryProgramV1, 2> programs;
};

NativeOnnxTensorStepPlan
buildNativeOnnxTensorStepPlan(const NativeOnnxGraphInspection& inspection,
  const std::vector<NativeOnnxNodeLayout>& layouts, const std::array<std::string, 2>& roles,
  const std::string& modelName);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_ONNX_RANK_TRANSFERS_HPP
