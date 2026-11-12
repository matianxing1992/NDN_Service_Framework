#ifndef NDNSF_DI_ONNX_TENSOR_TYPE_HPP
#define NDNSF_DI_ONNX_TENSOR_TYPE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <cstdint>
#include <optional>

namespace ndnsf::di {

/** Preserve unsupported internal ONNX types without inventing a wire encoding. */
std::optional<TensorElementType>
onnxTensorWireType(std::int32_t scalarType);

/** Return the original ONNX scalar ID for an admitted NDITB element type. */
std::int32_t
onnxTensorScalarType(TensorElementType wireType);

} // namespace ndnsf::di

#endif // NDNSF_DI_ONNX_TENSOR_TYPE_HPP
