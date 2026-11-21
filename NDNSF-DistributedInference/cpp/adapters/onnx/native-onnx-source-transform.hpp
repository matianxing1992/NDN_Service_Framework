#ifndef NDNSF_DI_NATIVE_ONNX_SOURCE_TRANSFORM_HPP
#define NDNSF_DI_NATIVE_ONNX_SOURCE_TRANSFORM_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"

#include <onnx/onnx_pb.h>

#include <cstdint>
#include <functional>
#include <string>
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
                               const NativeOnnxExternalAppender& append = {});

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
  const NativeOnnxExternalAppender& append);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_ONNX_SOURCE_TRANSFORM_HPP
