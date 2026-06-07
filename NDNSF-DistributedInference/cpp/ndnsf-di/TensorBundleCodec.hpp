#ifndef NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ndnsf::di {

enum class TensorElementType : std::uint32_t
{
  Float32 = 1,
};

struct NamedTensor
{
  std::string name;
  TensorElementType elementType = TensorElementType::Float32;
  std::vector<std::int64_t> shape;
  std::vector<std::uint8_t> payload;
};

namespace detail {

inline const std::string&
tensorBundleMagic()
{
  static const std::string magic = "NDITB001";
  return magic;
}

template<typename T>
void
appendScalar(std::vector<std::uint8_t>& output, T value)
{
  static_assert(std::is_integral<T>::value, "appendScalar expects integral type");
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    output.push_back(static_cast<std::uint8_t>(
      (static_cast<typename std::make_unsigned<T>::type>(value) >> (i * 8)) & 0xff));
  }
}

template<typename T>
T
readScalar(const std::vector<std::uint8_t>& input, std::size_t& offset)
{
  static_assert(std::is_integral<T>::value, "readScalar expects integral type");
  if (offset + sizeof(T) > input.size()) {
    throw std::invalid_argument("truncated tensor bundle scalar");
  }
  typename std::make_unsigned<T>::type value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<typename std::make_unsigned<T>::type>(input[offset + i]) << (i * 8);
  }
  offset += sizeof(T);
  return static_cast<T>(value);
}

inline void
appendBytes(std::vector<std::uint8_t>& output, const std::uint8_t* data, std::size_t size)
{
  output.insert(output.end(), data, data + size);
}

inline std::vector<std::uint8_t>
readBytes(const std::vector<std::uint8_t>& input, std::size_t& offset, std::size_t size)
{
  if (offset + size > input.size()) {
    throw std::invalid_argument("truncated tensor bundle bytes");
  }
  std::vector<std::uint8_t> bytes(input.begin() + static_cast<std::ptrdiff_t>(offset),
                                  input.begin() + static_cast<std::ptrdiff_t>(offset + size));
  offset += size;
  return bytes;
}

} // namespace detail

inline bool
isEncodedTensorBundle(const std::vector<std::uint8_t>& payload)
{
  const auto& magic = detail::tensorBundleMagic();
  return payload.size() >= magic.size() &&
         std::memcmp(payload.data(), magic.data(), magic.size()) == 0;
}

inline std::vector<std::uint8_t>
float32Payload(std::initializer_list<float> values)
{
  std::vector<float> floats(values);
  std::vector<std::uint8_t> payload(floats.size() * sizeof(float));
  if (!payload.empty()) {
    std::memcpy(payload.data(), floats.data(), payload.size());
  }
  return payload;
}

inline NamedTensor
makeFloat32Tensor(std::string name,
                  std::vector<std::int64_t> shape,
                  const std::vector<std::uint8_t>& payload)
{
  NamedTensor tensor;
  tensor.name = std::move(name);
  tensor.elementType = TensorElementType::Float32;
  tensor.shape = std::move(shape);
  tensor.payload = payload;
  return tensor;
}

inline std::vector<std::uint8_t>
encodeTensorBundle(const std::vector<NamedTensor>& tensors)
{
  std::vector<std::uint8_t> output;
  const auto& magic = detail::tensorBundleMagic();
  detail::appendBytes(output,
                      reinterpret_cast<const std::uint8_t*>(magic.data()),
                      magic.size());
  detail::appendScalar<std::uint32_t>(output, static_cast<std::uint32_t>(tensors.size()));
  for (const auto& tensor : tensors) {
    if (tensor.name.empty()) {
      throw std::invalid_argument("tensor bundle tensor name must not be empty");
    }
    detail::appendScalar<std::uint32_t>(output, static_cast<std::uint32_t>(tensor.name.size()));
    detail::appendBytes(output,
                        reinterpret_cast<const std::uint8_t*>(tensor.name.data()),
                        tensor.name.size());
    detail::appendScalar<std::uint32_t>(output, static_cast<std::uint32_t>(tensor.elementType));
    detail::appendScalar<std::uint32_t>(output, static_cast<std::uint32_t>(tensor.shape.size()));
    for (const auto dim : tensor.shape) {
      detail::appendScalar<std::int64_t>(output, dim);
    }
    detail::appendScalar<std::uint64_t>(output, static_cast<std::uint64_t>(tensor.payload.size()));
    detail::appendBytes(output, tensor.payload.data(), tensor.payload.size());
  }
  return output;
}

inline std::vector<NamedTensor>
decodeTensorBundle(const std::vector<std::uint8_t>& payload)
{
  const auto& magic = detail::tensorBundleMagic();
  if (!isEncodedTensorBundle(payload)) {
    throw std::invalid_argument("payload is not an NDNSF-DI tensor bundle");
  }
  std::size_t offset = magic.size();
  const auto count = detail::readScalar<std::uint32_t>(payload, offset);
  std::vector<NamedTensor> tensors;
  tensors.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto nameSize = detail::readScalar<std::uint32_t>(payload, offset);
    auto nameBytes = detail::readBytes(payload, offset, nameSize);
    NamedTensor tensor;
    tensor.name.assign(nameBytes.begin(), nameBytes.end());
    tensor.elementType = static_cast<TensorElementType>(
      detail::readScalar<std::uint32_t>(payload, offset));
    const auto rank = detail::readScalar<std::uint32_t>(payload, offset);
    tensor.shape.reserve(rank);
    for (std::uint32_t dim = 0; dim < rank; ++dim) {
      tensor.shape.push_back(detail::readScalar<std::int64_t>(payload, offset));
    }
    const auto payloadSize = detail::readScalar<std::uint64_t>(payload, offset);
    tensor.payload = detail::readBytes(payload, offset, static_cast<std::size_t>(payloadSize));
    tensors.push_back(std::move(tensor));
  }
  if (offset != payload.size()) {
    throw std::invalid_argument("tensor bundle has trailing bytes");
  }
  return tensors;
}

inline const NamedTensor&
findTensor(const std::vector<NamedTensor>& tensors, const std::string& name)
{
  for (const auto& tensor : tensors) {
    if (tensor.name == name) {
      return tensor;
    }
  }
  throw std::out_of_range("tensor bundle has no tensor: " + name);
}

inline std::vector<NamedTensor>
selectTensors(const std::vector<NamedTensor>& tensors,
              const std::vector<std::string>& names)
{
  if (names.empty()) {
    return tensors;
  }
  std::vector<NamedTensor> selected;
  selected.reserve(names.size());
  std::set<std::string> seen;
  for (const auto& name : names) {
    if (!seen.insert(name).second) {
      continue;
    }
    selected.push_back(findTensor(tensors, name));
  }
  return selected;
}

inline TensorBundle
makeEncodedTensorBundle(std::string name, const std::vector<NamedTensor>& tensors)
{
  TensorBundle bundle;
  bundle.name = std::move(name);
  bundle.payload = encodeTensorBundle(tensors);
  bundle.expectedBytes = bundle.payload.size();
  return bundle;
}

inline TensorBundle
selectTensorBundle(std::string name,
                   const TensorBundle& bundle,
                   const std::vector<std::string>& tensorNames)
{
  if (tensorNames.empty()) {
    TensorBundle copy = bundle;
    copy.name = std::move(name);
    return copy;
  }
  if (!isEncodedTensorBundle(bundle.payload)) {
    throw std::invalid_argument(
      "cannot select named tensors from a non-encoded tensor bundle");
  }
  auto selected = selectTensors(decodeTensorBundle(bundle.payload), tensorNames);
  return makeEncodedTensorBundle(std::move(name), selected);
}

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP
