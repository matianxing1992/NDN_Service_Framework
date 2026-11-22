#ifndef NDNSF_DI_WAN_SUMMED_PROJECTION_HPP
#define NDNSF_DI_WAN_SUMMED_PROJECTION_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"

#include <onnx/onnx_pb.h>

#include <array>

namespace ndnsf::di::wan {

/** Select inline slicing or the checkpoint sidecar while lowering a graph fragment. */
struct WanInitializerAccess
{
  NativeOnnxExternalReader reader;
  NativeOnnxInitializerSlicer slicer;

  onnx::TensorProto
  slice(const onnx::TensorProto& tensor, std::size_t axis,
        std::uint64_t begin, std::uint64_t end) const
  {
    if (slicer) return slicer(tensor, axis, begin, end);
    return sliceNativeOnnxFloatInitializer(tensor, axis, begin, end, reader);
  }
};

struct OnnxEntryCut
{
  std::vector<std::uint64_t> nodeIndices;
  std::vector<NativeAssemblyTensorContractV3> inputs;
  std::vector<NativeAssemblyTensorContractV3> outputs;
};

/** A canonical Add implemented as a local SUM on its consuming rank or both ranks. */
struct OnnxReductionCut
{
  OnnxEntryCut cut;
  bool bothRanks = false;
  std::size_t firstRank = 0;
};

struct OnnxParallelPlan
{
  NativeExecutionPlan plan;
  std::map<std::string, OnnxEntryCut> entries;
};

/** A shared canonical graph with independent CALL cuts around a native SUM. */
struct SummedProjection
{
  onnx::ModelProto model;
  std::array<OnnxEntryCut, 2> partials;
  OnnxEntryCut addBias;
};

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_SUMMED_PROJECTION_HPP
