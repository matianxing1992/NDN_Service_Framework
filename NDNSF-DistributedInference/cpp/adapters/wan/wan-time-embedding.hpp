#ifndef NDNSF_DI_WAN_TIME_EMBEDDING_HPP
#define NDNSF_DI_WAN_TIME_EMBEDDING_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "wan-summed-projection.hpp"

#include <array>

namespace ndnsf::di::wan {

struct TimeLinearParameters
{
  const onnx::TensorProto& weight;
  const onnx::TensorProto& bias;
};

struct TimeEmbeddingLayout
{
  SummedProjection embedding;
  std::array<OnnxEntryCut, 2> modulation;
};

/** Compile Wan timestep conditioning from the three canonical MatMul parameters.
 * Weights are arranged as [input, output] for the two time embedding linears
 * and the time projection, respectively. The input timestep is int64 [1]
 * in the Wan2.1 training range [0, 1000).
 * The outputs are the head embedding [1, hidden] and two rank-local block
 * modulation tensors [1, 6, rankHidden]. Each modulation entry consumes the
 * embedding from the single bias owner after the native SUM.
 */
TimeEmbeddingLayout
lowerTimeEmbedding(const std::array<TimeLinearParameters, 3>& parameters,
                   const WanInitializerAccess& material = {});

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_TIME_EMBEDDING_HPP
