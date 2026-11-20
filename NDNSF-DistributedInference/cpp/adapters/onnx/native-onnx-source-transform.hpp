#ifndef NDNSF_DI_NATIVE_ONNX_SOURCE_TRANSFORM_HPP
#define NDNSF_DI_NATIVE_ONNX_SOURCE_TRANSFORM_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"

#include <onnx/onnx_pb.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Resolve one external initializer range when the role graph carries external data. */
using NativeOnnxExternalReader =
  std::function<std::vector<std::uint8_t>(const std::string& location,
                                          std::uint64_t offset, std::uint64_t length)>;

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
                               const NativeOnnxExternalReader& external = {});

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_ONNX_SOURCE_TRANSFORM_HPP
