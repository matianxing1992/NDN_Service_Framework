#ifndef NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace ndnsf::di {

enum class TensorElementType : std::uint32_t
{
  Float32 = 1,
  Float16 = 2,
  Int64 = 3,
  Bool = 4,
};

struct NamedTensor
{
  std::string name;
  TensorElementType elementType = TensorElementType::Float32;
  std::vector<std::int64_t> shape;
  std::vector<std::uint8_t> payload;
};

std::size_t
tensorElementByteSize(TensorElementType elementType);

void
validateNamedTensor(const NamedTensor& tensor);

bool
isEncodedTensorBundle(const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t>
float32Payload(std::initializer_list<float> values);

NamedTensor
makeFloat32Tensor(std::string name,
                  std::vector<std::int64_t> shape,
                  const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t>
encodeTensorBundle(const std::vector<NamedTensor>& tensors);

std::vector<NamedTensor>
decodeTensorBundle(const std::vector<std::uint8_t>& payload);

const NamedTensor&
findTensor(const std::vector<NamedTensor>& tensors, const std::string& name);

std::vector<NamedTensor>
selectTensors(const std::vector<NamedTensor>& tensors,
              const std::vector<std::string>& names);

TensorBundle
makeEncodedTensorBundle(std::string name, const std::vector<NamedTensor>& tensors);

TensorBundle
selectTensorBundle(std::string name,
                   const TensorBundle& bundle,
                   const std::vector<std::string>& tensorNames);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP
