#ifndef NDNSF_DI_NATIVE_ONNX_SOURCE_TRANSFORM_HPP
#define NDNSF_DI_NATIVE_ONNX_SOURCE_TRANSFORM_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"

#include <onnx/onnx_pb.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di {

struct NativeCanonicalSource;
struct NativeCanonicalMaterializedFiles;
struct NativeAssemblyControl;
struct NativeExecutionStepSpec;

/** Resolve one external initializer range when the role graph carries external data. */
using NativeOnnxExternalReader =
  std::function<std::vector<std::uint8_t>(const std::string& location,
                                          std::uint64_t offset, std::uint64_t length)>;

/** Append to a new, empty model.onnx.data owned by the caller. */
using NativeOnnxExternalAppender = std::function<void(const std::vector<std::uint8_t>&)>;

/** Derive two nonempty output shards, keeping grouped Conv inputs with their groups. */
std::array<NativeOnnxConvShard, 2>
inspectNativeOnnxConvOutputShards(const onnx::ModelProto& model, const onnx::NodeProto& node);

/** Slice verified Conv weights and bias after the caller binds the local input range.
 * The caller owns graph boundary port types and rewriting other sidecar references. */
void
sliceNativeOnnxConvOutput(onnx::ModelProto& model, onnx::NodeProto& node, std::size_t rank,
  const NativeOnnxConvShard& admitted, const NativeOnnxExternalReader& external,
  const NativeOnnxExternalAppender& append, std::uint64_t& sidecarOffset);

/** Derive the two head ranges for one static [batch, heads, rows, columns] operation.
 *
 * The caller must prove that both MatMul inputs share the same logical head order
 * and that Softmax consumes the complete key axis of the corresponding scores.
 */
std::vector<std::pair<std::uint64_t, std::uint64_t>>
inspectNativeOnnxAttentionHeadShards(const onnx::ModelProto& model, const onnx::NodeProto& node);

/** Map a shard through a static ONNX Reshape if local execution keeps its elements. */
std::optional<NativeOnnxTwoRankLayout>
inspectNativeOnnxReshapeLayout(const onnx::ModelProto& model, const onnx::NodeProto& node,
                              NativeOnnxTwoRankLayout inputLayout);

/** Analyze static ONNX ports and propagate the two-rank layout along original SSA edges. */
std::vector<NativeOnnxNodeLayout>
deriveNativeOnnxTwoRankLayouts(const onnx::ModelProto& model,
                               const std::function<void()>& requireActive = {});

/** Geometry of a dense or symmetric block-32 linear projection. */
struct NativeOnnxLinearProjection
{
  static constexpr std::int64_t BLOCK = 32;
  static constexpr std::int64_t BLOB_BYTES = BLOCK / 2;
  std::int64_t k = 0;
  std::int64_t n = 0;
  bool packed = false;
};

/** Check the operator and its initializer ABI before deriving any rank range. */
NativeOnnxLinearProjection
inspectNativeOnnxLinearProjection(const onnx::ModelProto& model, const onnx::NodeProto& node);

enum class NativeOnnxProjectionAxis
{
  INPUT,
  OUTPUT
};

/** Narrow the original initializer and operator without changing projection precision. */
void
sliceNativeOnnxLinearProjection(onnx::ModelProto& model, onnx::NodeProto& node,
  NativeOnnxProjectionAxis axis, std::uint64_t begin, std::uint64_t end,
  const NativeOnnxExternalReader& external, const NativeOnnxExternalAppender& append,
  std::uint64_t& sidecarOffset);

struct NativeOnnxRmsNormEntries
{
  onnx::ModelProto statistics;
  onnx::ModelProto normalization;
};

/** Split one source FP32 RMSNorm into local squares and global-stat normalization. */
NativeOnnxRmsNormEntries
lowerNativeOnnxRmsNorm(const onnx::ModelProto& source, std::uint64_t begin,
                      std::uint64_t end, const std::string& globalSquares,
                      const NativeOnnxExternalReader& external = {});

/** The tail starts at normalized Q/K and projected V, and ends before the output bias. */
onnx::ModelProto
lowerNativeOnnxAttentionHeads(onnx::ModelProto source, const std::string& query,
  const std::string& key, const std::string& value, std::uint64_t headBegin,
  std::uint64_t headEnd, const NativeOnnxExternalReader& external = {},
  const NativeOnnxExternalAppender& append = {}, std::uint64_t sealedHeadWidth = 0);

/** Slice the original Q/K/V output channels and their separate bias operands. */
onnx::ModelProto
lowerNativeOnnxAttentionProjections(onnx::ModelProto source, std::uint64_t begin,
  std::uint64_t end, const NativeOnnxExternalReader& external = {},
  const NativeOnnxExternalAppender& append = {});

using NativeOnnxInitializerSlicer = std::function<onnx::TensorProto(
  const onnx::TensorProto&, std::size_t, std::uint64_t, std::uint64_t)>;

/** Materialize one rank's FP32 initializer, reading external data only for that slice. */
onnx::TensorProto
sliceNativeOnnxFloatInitializer(const onnx::TensorProto& tensor, std::size_t axis,
                                std::uint64_t begin, std::uint64_t end,
                                const NativeOnnxExternalReader& external = {});

/**
 * Apply a source_transform_v1 lowering to one materialized role graph.
 *
 * The input graph is the ordinary selected subgraph; the returned graph is the
 * derived executable form. The caller remains responsible for binding the
 * derived identity into the sealed recipe and the assembly receipt.
 */
onnx::ModelProto
applyNativeOnnxSourceTransform(const onnx::ModelProto& source,
                               const NativeOnnxSourceTransformV1& transform,
                               const NativeOnnxExternalReader& external = {},
                               const NativeOnnxExternalAppender& append = {},
                               const NativeOnnxInitializerSlicer& slicer = {});

/** Lower a complete FFN entry after materialization in verified ordinal order. */
onnx::ModelProto
lowerNativeMaterializedFfnEntry(onnx::ModelProto source,
                                const NativeOnnxSourceTransformV1& transform,
                                const std::vector<std::uint64_t>& sourceOrdinals,
                                const NativeOnnxExternalReader& external = {},
                                const NativeOnnxExternalAppender& append = {});

/** Rebuild only one entry's authenticated graph and initializer closure from a shared spool. */
NativeCanonicalMaterializedFiles
materializeNativeOnnxEntrySource(const NativeCanonicalSource& source,
  const NativeOnnxSourceEntryRecipeV1& entry, const std::vector<std::uint64_t>& sourceOrdinals,
  const NativeExecutionStepSpec& call, const NativeAssemblyControl& control,
  const NativeOnnxExternalAppender& append,
  const NativeOnnxSourceEntryProgramV1* tensorProgram = nullptr);

/** Check physical slots against authenticated ONNX ValueInfo and recover the original
 * inputs and requested outputs for an entry before tensor-layout lowering. */
void
bindNativeOnnxTensorEntrySource(onnx::ModelProto& source,
  const NativeOnnxSourceEntryProgramV1& program, const NativeExecutionStepSpec& call);

/** Lower one recovered source window to its exact physical CALL ABI. */
onnx::ModelProto
lowerNativeOnnxTensorEntry(onnx::ModelProto source,
  const NativeOnnxSourceEntryProgramV1& program, const NativeExecutionStepSpec& call,
  std::size_t rank, const NativeAssemblyControl& control, const NativeOnnxExternalReader& external,
  const NativeOnnxExternalAppender& append);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_ONNX_SOURCE_TRANSFORM_HPP
