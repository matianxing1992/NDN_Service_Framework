#include "onnx-tensor-type.hpp"

#include <onnx/onnx_pb.h>

#include <array>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

const std::array<std::pair<std::int32_t, TensorElementType>, 8> TYPES = {{
  {onnx::TensorProto::FLOAT, TensorElementType::Float32},
  {onnx::TensorProto::FLOAT16, TensorElementType::Float16},
  {onnx::TensorProto::INT64, TensorElementType::Int64},
  {onnx::TensorProto::INT32, TensorElementType::Int32},
  {onnx::TensorProto::BOOL, TensorElementType::Bool},
  {onnx::TensorProto::UINT8, TensorElementType::UInt8},
  {onnx::TensorProto::INT8, TensorElementType::INT8},
  {onnx::TensorProto::BFLOAT16, TensorElementType::BFLOAT16},
}};

} // namespace

std::optional<TensorElementType>
onnxTensorWireType(std::int32_t scalarType)
{
  for (const auto& [scalar, wire] : TYPES) {
    if (scalar == scalarType) return wire;
  }
  return std::nullopt;
}

std::int32_t
onnxTensorScalarType(TensorElementType wireType)
{
  for (const auto& [scalar, wire] : TYPES) {
    if (wire == wireType) return scalar;
  }
  throw std::invalid_argument("unsupported NDNSF tensor element type");
}

} // namespace ndnsf::di
