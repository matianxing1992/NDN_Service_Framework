#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <openssl/evp.h>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ndnsf::di {
namespace {

constexpr std::size_t MAX_TENSOR_COUNT = 256;
constexpr std::size_t MAX_TENSOR_NAME_BYTES = 1024;
constexpr std::size_t MAX_TENSOR_RANK = 16;
constexpr std::uint64_t MAX_TENSOR_DIMENSION = 1ULL << 31;
constexpr std::uint64_t MAX_TENSOR_PAYLOAD_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr std::size_t MAX_MANIFEST_STRING_BYTES = 1U << 20;
constexpr std::size_t MAX_MANIFEST_SEGMENTS = 1U << 20;
constexpr std::size_t MAX_MANIFEST_WIRE_BYTES = 64U << 20;

const std::string&
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

void
appendBytes(std::vector<std::uint8_t>& output, const std::uint8_t* data, std::size_t size)
{
  output.insert(output.end(), data, data + size);
}

std::vector<std::uint8_t>
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

void
appendSizedBytes(std::vector<std::uint8_t>& output,
                 const std::vector<std::uint8_t>& value)
{
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("TensorObjectManifestV1 byte field exceeds bound");
  }
  appendScalar<std::uint32_t>(output, static_cast<std::uint32_t>(value.size()));
  appendBytes(output, value.data(), value.size());
}

void
appendString(std::vector<std::uint8_t>& output, const std::string& value)
{
  if (value.size() > MAX_MANIFEST_STRING_BYTES) {
    throw std::invalid_argument("TensorObjectManifestV1 string exceeds bound");
  }
  appendScalar<std::uint32_t>(output, static_cast<std::uint32_t>(value.size()));
  appendBytes(output,
              reinterpret_cast<const std::uint8_t*>(value.data()),
              value.size());
}

std::string
readString(const std::vector<std::uint8_t>& input, std::size_t& offset)
{
  const auto size = readScalar<std::uint32_t>(input, offset);
  if (size > MAX_MANIFEST_STRING_BYTES) {
    throw std::invalid_argument("TensorObjectManifestV1 string exceeds bound");
  }
  const auto bytes = readBytes(input, offset, size);
  return std::string(bytes.begin(), bytes.end());
}

std::vector<std::uint8_t>
readSizedBytes(const std::vector<std::uint8_t>& input,
               std::size_t& offset,
               std::size_t maxBytes)
{
  const auto size = readScalar<std::uint32_t>(input, offset);
  if (size > maxBytes) {
    throw std::invalid_argument("TensorObjectManifestV1 byte field exceeds bound");
  }
  return readBytes(input, offset, size);
}

void
appendStrings(std::vector<std::uint8_t>& output,
              const std::vector<std::string>& values)
{
  if (values.size() > MAX_MANIFEST_SEGMENTS) {
    throw std::invalid_argument("TensorObjectManifestV1 list exceeds bound");
  }
  appendScalar<std::uint32_t>(output, static_cast<std::uint32_t>(values.size()));
  for (const auto& value : values) {
    appendString(output, value);
  }
}

std::vector<std::string>
readStrings(const std::vector<std::uint8_t>& input, std::size_t& offset)
{
  const auto count = readScalar<std::uint32_t>(input, offset);
  if (count > MAX_MANIFEST_SEGMENTS) {
    throw std::invalid_argument("TensorObjectManifestV1 list exceeds bound");
  }
  std::vector<std::string> values;
  values.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    values.push_back(readString(input, offset));
  }
  return values;
}

bool
isSha256Digest(const std::string& value)
{
  if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) {
    return false;
  }
  return std::all_of(value.begin() + 7, value.end(), [] (unsigned char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
  });
}

} // namespace

std::string
sha256TensorBytes(const std::vector<std::uint8_t>& bytes)
{
  std::vector<std::uint8_t> digest(EVP_MAX_MD_SIZE);
  unsigned int digestSize = 0;
  auto* context = EVP_MD_CTX_new();
  if (context == nullptr ||
      EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, bytes.data(), bytes.size()) != 1 ||
      EVP_DigestFinal_ex(context, digest.data(), &digestSize) != 1 ||
      digestSize != 32) {
    EVP_MD_CTX_free(context);
    throw std::runtime_error("TensorObjectManifestV1 SHA-256 failed");
  }
  EVP_MD_CTX_free(context);
  static constexpr char HEX[] = "0123456789abcdef";
  std::string value = "sha256:";
  value.reserve(71);
  for (std::size_t i = 0; i < digestSize; ++i) {
    value.push_back(HEX[digest[i] >> 4]);
    value.push_back(HEX[digest[i] & 0x0f]);
  }
  return value;
}

void
TensorObjectManifestV1::validate() const
{
  for (const auto* value : {
         &capabilityDigest, &planDigest, &sourceLayoutDigest,
         &targetLayoutDigest, &tensorDigest, &contentDigest,
         &endpointDigest, &manifestContractDigest, &objectManifestDigest}) {
    if (!isSha256Digest(*value)) {
      throw std::invalid_argument(
        "TensorObjectManifestV1 contains a non-canonical digest");
    }
  }
  if (epochKeyId.empty() || requester.empty() || requestId.empty() ||
      attemptId.empty() || groupId.empty() || epoch.empty() ||
      operationKind.empty() || producerRole.empty() || consumerRoles.empty() ||
      tensorId.empty() || totalBytes == 0 || segmentSize == 0 ||
      segmentCount == 0 || segmentCount != orderedSegmentDigests.size() ||
      segmentCount > MAX_MANIFEST_SEGMENTS || createdAtMs == 0 ||
      noProgressMs == 0 || hardDeadlineMs < noProgressMs ||
      producerSignature.empty()) {
    throw std::invalid_argument("invalid TensorObjectManifestV1 bounds");
  }
  std::set<std::string> uniqueConsumers;
  for (const auto& consumer : consumerRoles) {
    if (consumer.empty() || !uniqueConsumers.insert(consumer).second) {
      throw std::invalid_argument("invalid TensorObjectManifestV1 consumers");
    }
  }
  for (const auto& digest : orderedSegmentDigests) {
    if (!isSha256Digest(digest)) {
      throw std::invalid_argument(
        "TensorObjectManifestV1 contains an invalid segment digest");
    }
  }
  if (objectManifestDigest != digest()) {
    throw std::invalid_argument("TensorObjectManifestV1 digest mismatch");
  }
}

std::vector<std::uint8_t>
TensorObjectManifestV1::signingBytes() const
{
  std::vector<std::uint8_t> output;
  const std::string marker = "TensorObjectManifestV1";
  appendString(output, marker);
  appendString(output, capabilityDigest);
  appendString(output, epochKeyId);
  appendString(output, requester);
  appendString(output, requestId);
  appendString(output, attemptId);
  appendString(output, planDigest);
  appendString(output, groupId);
  appendString(output, epoch);
  appendScalar<std::uint64_t>(output, operationIndex);
  appendScalar<std::uint64_t>(output, round);
  appendString(output, operationKind);
  appendString(output, producerRole);
  appendScalar<std::uint64_t>(output, producerRank);
  appendStrings(output, consumerRoles);
  appendScalar<std::uint64_t>(output, microbatch);
  appendString(output, sourceLayoutDigest);
  appendString(output, targetLayoutDigest);
  appendString(output, tensorId);
  appendString(output, tensorDigest);
  appendString(output, contentDigest);
  appendScalar<std::uint64_t>(output, totalBytes);
  appendScalar<std::uint64_t>(output, segmentSize);
  appendScalar<std::uint64_t>(output, segmentCount);
  appendStrings(output, orderedSegmentDigests);
  appendScalar<std::uint64_t>(output, createdAtMs);
  appendScalar<std::uint64_t>(output, noProgressMs);
  appendScalar<std::uint64_t>(output, hardDeadlineMs);
  appendString(output, endpointDigest);
  appendString(output, manifestContractDigest);
  return output;
}

std::string
TensorObjectManifestV1::digest() const
{
  return sha256TensorBytes(signingBytes());
}

std::vector<std::uint8_t>
encodeTensorObjectManifest(const TensorObjectManifestV1& manifest)
{
  manifest.validate();
  auto output = manifest.signingBytes();
  appendSizedBytes(output, manifest.producerSignature);
  appendString(output, manifest.objectManifestDigest);
  if (output.size() > MAX_MANIFEST_WIRE_BYTES) {
    throw std::invalid_argument("TensorObjectManifestV1 wire exceeds bound");
  }
  return output;
}

TensorObjectManifestV1
decodeTensorObjectManifest(const std::vector<std::uint8_t>& wire)
{
  if (wire.empty() || wire.size() > MAX_MANIFEST_WIRE_BYTES) {
    throw std::invalid_argument("TensorObjectManifestV1 wire exceeds bound");
  }
  std::size_t offset = 0;
  if (readString(wire, offset) != "TensorObjectManifestV1") {
    throw std::invalid_argument("invalid TensorObjectManifestV1 marker");
  }
  TensorObjectManifestV1 value;
  value.capabilityDigest = readString(wire, offset);
  value.epochKeyId = readString(wire, offset);
  value.requester = readString(wire, offset);
  value.requestId = readString(wire, offset);
  value.attemptId = readString(wire, offset);
  value.planDigest = readString(wire, offset);
  value.groupId = readString(wire, offset);
  value.epoch = readString(wire, offset);
  value.operationIndex = readScalar<std::uint64_t>(wire, offset);
  value.round = readScalar<std::uint64_t>(wire, offset);
  value.operationKind = readString(wire, offset);
  value.producerRole = readString(wire, offset);
  value.producerRank = readScalar<std::uint64_t>(wire, offset);
  value.consumerRoles = readStrings(wire, offset);
  value.microbatch = readScalar<std::uint64_t>(wire, offset);
  value.sourceLayoutDigest = readString(wire, offset);
  value.targetLayoutDigest = readString(wire, offset);
  value.tensorId = readString(wire, offset);
  value.tensorDigest = readString(wire, offset);
  value.contentDigest = readString(wire, offset);
  value.totalBytes = readScalar<std::uint64_t>(wire, offset);
  value.segmentSize = readScalar<std::uint64_t>(wire, offset);
  value.segmentCount = readScalar<std::uint64_t>(wire, offset);
  value.orderedSegmentDigests = readStrings(wire, offset);
  value.createdAtMs = readScalar<std::uint64_t>(wire, offset);
  value.noProgressMs = readScalar<std::uint64_t>(wire, offset);
  value.hardDeadlineMs = readScalar<std::uint64_t>(wire, offset);
  value.endpointDigest = readString(wire, offset);
  value.manifestContractDigest = readString(wire, offset);
  value.producerSignature = readSizedBytes(wire, offset, 1U << 20);
  value.objectManifestDigest = readString(wire, offset);
  if (offset != wire.size()) {
    throw std::invalid_argument("TensorObjectManifestV1 has trailing bytes");
  }
  value.validate();
  return value;
}

std::size_t
tensorElementByteSize(TensorElementType elementType)
{
  switch (elementType) {
    case TensorElementType::Float32:
      return 4;
    case TensorElementType::Float16:
      return 2;
    case TensorElementType::Int64:
      return 8;
    case TensorElementType::Bool:
      return 1;
  }
  throw std::invalid_argument("unsupported tensor element type");
}

void
validateNamedTensor(const NamedTensor& tensor)
{
  if (tensor.name.empty() || tensor.name.size() > MAX_TENSOR_NAME_BYTES) {
    throw std::invalid_argument("tensor bundle tensor name has invalid size");
  }
  if (tensor.shape.size() > MAX_TENSOR_RANK) {
    throw std::invalid_argument("tensor bundle tensor rank exceeds limit");
  }
  std::uint64_t elements = 1;
  for (const auto dim : tensor.shape) {
    if (dim < 0 || static_cast<std::uint64_t>(dim) > MAX_TENSOR_DIMENSION) {
      throw std::invalid_argument("tensor bundle tensor dimension must be bounded and non-negative");
    }
    const auto value = static_cast<std::uint64_t>(dim);
    if (elements > std::numeric_limits<std::uint64_t>::max() / value) {
      throw std::invalid_argument("tensor bundle tensor shape overflows element count");
    }
    elements *= value;
  }
  const auto elementBytes = static_cast<std::uint64_t>(
    tensorElementByteSize(tensor.elementType));
  if (elements > MAX_TENSOR_PAYLOAD_BYTES / elementBytes) {
    throw std::invalid_argument("tensor bundle tensor payload exceeds limit");
  }
  const auto expectedBytes = elements * elementBytes;
  if (tensor.payload.size() != expectedBytes) {
    throw std::invalid_argument("tensor bundle tensor payload size does not match dtype and shape");
  }
}

bool
isEncodedTensorBundle(const std::vector<std::uint8_t>& payload)
{
  const auto& magic = tensorBundleMagic();
  return payload.size() >= magic.size() &&
         std::memcmp(payload.data(), magic.data(), magic.size()) == 0;
}

std::vector<std::uint8_t>
float32Payload(std::initializer_list<float> values)
{
  std::vector<float> floats(values);
  std::vector<std::uint8_t> payload(floats.size() * sizeof(float));
  if (!payload.empty()) {
    std::memcpy(payload.data(), floats.data(), payload.size());
  }
  return payload;
}

NamedTensor
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

std::vector<std::uint8_t>
encodeTensorBundle(const std::vector<NamedTensor>& tensors)
{
  if (tensors.size() > MAX_TENSOR_COUNT) {
    throw std::invalid_argument("tensor bundle tensor count exceeds limit");
  }
  std::vector<std::uint8_t> output;
  const auto& magic = tensorBundleMagic();
  appendBytes(output,
              reinterpret_cast<const std::uint8_t*>(magic.data()),
              magic.size());
  appendScalar<std::uint32_t>(output, static_cast<std::uint32_t>(tensors.size()));
  for (const auto& tensor : tensors) {
    validateNamedTensor(tensor);
    appendScalar<std::uint32_t>(output, static_cast<std::uint32_t>(tensor.name.size()));
    appendBytes(output,
                reinterpret_cast<const std::uint8_t*>(tensor.name.data()),
                tensor.name.size());
    appendScalar<std::uint32_t>(output, static_cast<std::uint32_t>(tensor.elementType));
    appendScalar<std::uint32_t>(output, static_cast<std::uint32_t>(tensor.shape.size()));
    for (const auto dim : tensor.shape) {
      appendScalar<std::int64_t>(output, dim);
    }
    appendScalar<std::uint64_t>(output, static_cast<std::uint64_t>(tensor.payload.size()));
    appendBytes(output, tensor.payload.data(), tensor.payload.size());
  }
  return output;
}

std::vector<NamedTensor>
decodeTensorBundle(const std::vector<std::uint8_t>& payload)
{
  const auto& magic = tensorBundleMagic();
  if (!isEncodedTensorBundle(payload)) {
    throw std::invalid_argument("payload is not an NDNSF-DI tensor bundle");
  }
  std::size_t offset = magic.size();
  const auto count = readScalar<std::uint32_t>(payload, offset);
  if (count > MAX_TENSOR_COUNT) {
    throw std::invalid_argument("tensor bundle tensor count exceeds limit");
  }
  std::vector<NamedTensor> tensors;
  tensors.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto nameSize = readScalar<std::uint32_t>(payload, offset);
    if (nameSize == 0 || nameSize > MAX_TENSOR_NAME_BYTES) {
      throw std::invalid_argument("tensor bundle tensor name has invalid size");
    }
    auto nameBytes = readBytes(payload, offset, nameSize);
    NamedTensor tensor;
    tensor.name.assign(nameBytes.begin(), nameBytes.end());
    tensor.elementType = static_cast<TensorElementType>(
      readScalar<std::uint32_t>(payload, offset));
    const auto rank = readScalar<std::uint32_t>(payload, offset);
    if (rank > MAX_TENSOR_RANK) {
      throw std::invalid_argument("tensor bundle tensor rank exceeds limit");
    }
    tensor.shape.reserve(rank);
    for (std::uint32_t dim = 0; dim < rank; ++dim) {
      tensor.shape.push_back(readScalar<std::int64_t>(payload, offset));
    }
    const auto payloadSize = readScalar<std::uint64_t>(payload, offset);
    if (payloadSize > MAX_TENSOR_PAYLOAD_BYTES ||
        payloadSize > std::numeric_limits<std::size_t>::max()) {
      throw std::invalid_argument("tensor bundle tensor payload exceeds limit");
    }
    tensor.payload = readBytes(payload, offset, static_cast<std::size_t>(payloadSize));
    validateNamedTensor(tensor);
    tensors.push_back(std::move(tensor));
  }
  if (offset != payload.size()) {
    throw std::invalid_argument("tensor bundle has trailing bytes");
  }
  return tensors;
}

const NamedTensor&
findTensor(const std::vector<NamedTensor>& tensors, const std::string& name)
{
  for (const auto& tensor : tensors) {
    if (tensor.name == name) {
      return tensor;
    }
  }
  throw std::out_of_range("tensor bundle has no tensor: " + name);
}

std::vector<NamedTensor>
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

TensorBundle
makeEncodedTensorBundle(std::string name, const std::vector<NamedTensor>& tensors)
{
  TensorBundle bundle;
  bundle.name = std::move(name);
  bundle.payload = encodeTensorBundle(tensors);
  bundle.expectedBytes = bundle.payload.size();
  return bundle;
}

TensorBundle
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

namespace {

std::size_t
resolvedAxis(const RedistributionSpec& redistribution, const NamedTensor& tensor)
{
  if (tensor.shape.empty()) {
    throw std::invalid_argument("redistribution requires a ranked tensor");
  }
  const auto rank = static_cast<std::int64_t>(tensor.shape.size());
  const auto axis = redistribution.axis < 0
    ? rank + redistribution.axis
    : redistribution.axis;
  if (axis < 0 || axis >= rank) {
    throw std::invalid_argument("redistribution axis is outside tensor rank");
  }
  return static_cast<std::size_t>(axis);
}

std::size_t
dimensionProduct(const std::vector<std::int64_t>& shape,
                 std::size_t begin,
                 std::size_t end)
{
  std::size_t value = 1;
  for (std::size_t index = begin; index < end; ++index) {
    if (shape[index] <= 0 ||
        static_cast<std::uint64_t>(shape[index]) >
          std::numeric_limits<std::size_t>::max() / value) {
      throw std::invalid_argument("redistribution tensor shape is invalid");
    }
    value *= static_cast<std::size_t>(shape[index]);
  }
  return value;
}

NamedTensor
scatterTensor(const NamedTensor& source,
              const RedistributionSpec& redistribution,
              std::uint64_t consumerRank)
{
  const auto found = std::find(redistribution.consumerRanks.begin(),
                               redistribution.consumerRanks.end(),
                               consumerRank);
  if (found == redistribution.consumerRanks.end()) {
    throw std::invalid_argument(
      "redistribution consumer rank is outside certified cover");
  }
  const auto shardIndex = static_cast<std::size_t>(
    std::distance(redistribution.consumerRanks.begin(), found));
  const auto shardCount = redistribution.consumerRanks.size();
  const auto axis = resolvedAxis(redistribution, source);
  const auto axisSize = static_cast<std::size_t>(source.shape[axis]);
  if (axisSize % shardCount != 0) {
    throw std::invalid_argument(
      "SCATTER tensor axis is not divisible by consumer rank count");
  }

  const auto outer = dimensionProduct(source.shape, 0, axis);
  const auto inner = dimensionProduct(source.shape, axis + 1, source.shape.size());
  const auto shardAxisSize = axisSize / shardCount;
  const auto elementBytes = tensorElementByteSize(source.elementType);
  const auto shardBlockBytes = shardAxisSize * inner * elementBytes;
  const auto sourceBlockBytes = axisSize * inner * elementBytes;

  NamedTensor shard = source;
  shard.shape[axis] = static_cast<std::int64_t>(shardAxisSize);
  shard.payload.clear();
  shard.payload.reserve(outer * shardBlockBytes);
  for (std::size_t outerIndex = 0; outerIndex < outer; ++outerIndex) {
    const auto offset = outerIndex * sourceBlockBytes +
                        shardIndex * shardBlockBytes;
    shard.payload.insert(shard.payload.end(),
                         source.payload.begin() + static_cast<std::ptrdiff_t>(offset),
                         source.payload.begin() +
                           static_cast<std::ptrdiff_t>(offset + shardBlockBytes));
  }
  validateNamedTensor(shard);
  return shard;
}

NamedTensor
gatherTensor(const std::map<std::uint64_t, NamedTensor>& shards,
             const RedistributionSpec& redistribution)
{
  if (shards.size() != redistribution.producerRanks.size()) {
    throw std::invalid_argument(
      "GATHER input does not cover every certified producer rank");
  }
  std::vector<const NamedTensor*> ordered;
  ordered.reserve(redistribution.producerRanks.size());
  for (const auto rank : redistribution.producerRanks) {
    const auto shard = shards.find(rank);
    if (shard == shards.end()) {
      throw std::invalid_argument(
        "GATHER input is missing a certified producer rank");
    }
    ordered.push_back(&shard->second);
  }

  NamedTensor gathered = *ordered.front();
  const auto axis = resolvedAxis(redistribution, gathered);
  std::size_t gatheredAxisSize = 0;
  for (const auto* shard : ordered) {
    validateNamedTensor(*shard);
    if (shard->name != gathered.name ||
        shard->elementType != gathered.elementType ||
        shard->shape.size() != gathered.shape.size()) {
      throw std::invalid_argument("GATHER tensor metadata mismatch");
    }
    for (std::size_t dimension = 0; dimension < gathered.shape.size(); ++dimension) {
      if (dimension != axis && shard->shape[dimension] != gathered.shape[dimension]) {
        throw std::invalid_argument("GATHER tensor non-axis shape mismatch");
      }
    }
    gatheredAxisSize += static_cast<std::size_t>(shard->shape[axis]);
  }

  const auto outer = dimensionProduct(gathered.shape, 0, axis);
  const auto inner = dimensionProduct(
    gathered.shape, axis + 1, gathered.shape.size());
  const auto elementBytes = tensorElementByteSize(gathered.elementType);
  gathered.shape[axis] = static_cast<std::int64_t>(gatheredAxisSize);
  gathered.payload.clear();
  gathered.payload.reserve(
    outer * gatheredAxisSize * inner * elementBytes);
  for (std::size_t outerIndex = 0; outerIndex < outer; ++outerIndex) {
    for (const auto* shard : ordered) {
      const auto shardAxisSize = static_cast<std::size_t>(shard->shape[axis]);
      const auto shardBlockBytes = shardAxisSize * inner * elementBytes;
      const auto offset = outerIndex * shardBlockBytes;
      gathered.payload.insert(
        gathered.payload.end(),
        shard->payload.begin() + static_cast<std::ptrdiff_t>(offset),
        shard->payload.begin() +
          static_cast<std::ptrdiff_t>(offset + shardBlockBytes));
    }
  }
  validateNamedTensor(gathered);
  return gathered;
}

} // namespace

std::map<std::string, TensorBundle>
applyCertifiedTensorRedistributions(const RoleExecutionContext& context)
{
  auto transformed = context.inputsByScope;
  std::set<std::string> completedTransportScopes;
  for (const auto& item : context.inputEdgesByScope) {
    const auto& scope = item.first;
    const auto& edge = item.second;
    if (edge.redistributions.empty()) {
      continue;
    }
    const auto transportScope = edge.transportScope.empty()
      ? scope
      : edge.transportScope;
    if (!completedTransportScopes.insert(transportScope).second) {
      continue;
    }
    const auto input = context.inputsByScope.find(scope);
    if (input == context.inputsByScope.end() ||
        !isEncodedTensorBundle(input->second.payload)) {
      throw std::invalid_argument(
        "certified redistribution requires an encoded tensor bundle");
    }
    auto tensors = decodeTensorBundle(input->second.payload);
    for (const auto& redistribution : edge.redistributions) {
      if (redistribution.operation == "GATHER" ||
          redistribution.operation == "RESHARD") {
        std::map<std::uint64_t, NamedTensor> shards;
        for (const auto& candidate : context.inputEdgesByScope) {
          const auto& candidateEdge = candidate.second;
          const auto candidateTransportScope = candidateEdge.transportScope.empty()
            ? candidate.first
            : candidateEdge.transportScope;
          if (candidateTransportScope != transportScope) {
            continue;
          }
          if (!candidateEdge.redistributionProducerRank) {
            throw std::invalid_argument(
              "GATHER projection is missing its producer rank");
          }
          const auto candidateInput = context.inputsByScope.find(candidate.first);
          if (candidateInput == context.inputsByScope.end() ||
              !isEncodedTensorBundle(candidateInput->second.payload)) {
            throw std::invalid_argument(
              "GATHER requires an encoded input from every producer rank");
          }
          const auto candidateTensors =
            decodeTensorBundle(candidateInput->second.payload);
          if (!shards.emplace(
                *candidateEdge.redistributionProducerRank,
                findTensor(candidateTensors, redistribution.tensor)).second) {
            throw std::invalid_argument("GATHER has a duplicate producer rank");
          }
          transformed.erase(candidate.first);
        }
        auto tensor = std::find_if(
          tensors.begin(), tensors.end(), [&] (const NamedTensor& candidate) {
            return candidate.name == redistribution.tensor;
          });
        if (tensor == tensors.end()) {
          throw std::invalid_argument(
            "redistribution tensor is absent from encoded input bundle");
        }
        auto gathered = gatherTensor(shards, redistribution);
        if (redistribution.operation == "RESHARD") {
          if (!edge.redistributionConsumerRank) {
            throw std::invalid_argument(
              "RESHARD projection is missing its consumer rank");
          }
          gathered = scatterTensor(
            gathered, redistribution, *edge.redistributionConsumerRank);
        }
        *tensor = std::move(gathered);
        continue;
      }
      if (redistribution.operation != "SCATTER") {
        throw std::invalid_argument(
          "certified redistribution operation is not implemented");
      }
      if (!edge.redistributionConsumerRank) {
        throw std::invalid_argument(
          "SCATTER projection is missing its consumer rank");
      }
      auto tensor = std::find_if(
        tensors.begin(), tensors.end(), [&] (const NamedTensor& candidate) {
          return candidate.name == redistribution.tensor;
        });
      if (tensor == tensors.end()) {
        throw std::invalid_argument(
          "redistribution tensor is absent from encoded input bundle");
      }
      *tensor = scatterTensor(
        *tensor, redistribution, *edge.redistributionConsumerRank);
    }
    const auto outputScope = transportScope;
    if (outputScope != scope) {
      transformed.erase(scope);
    }
    transformed[outputScope] = makeEncodedTensorBundle(outputScope, tensors);
  }
  return transformed;
}

} // namespace ndnsf::di
