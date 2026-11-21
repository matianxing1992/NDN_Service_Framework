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
materializeNativeOnnxFfnEntrySource(const NativeCanonicalSource& source,
  const NativeOnnxSourceEntryRecipeV1& entry, const std::vector<std::uint64_t>& sourceOrdinals,
  const NativeExecutionStepSpec& call, const NativeAssemblyControl& control,
  const NativeOnnxExternalAppender& append);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_ONNX_SOURCE_TRANSFORM_HPP
