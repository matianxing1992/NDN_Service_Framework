#include "wan-checkpoint.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/onnx/initializer-slice.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ndnsf::di::wan {
namespace {

constexpr std::uint64_t PREFIX_BYTES = 8;
constexpr std::uint64_t MAX_HEADER_BYTES = 4U << 20;

std::uint64_t
unsignedInteger(const NativeJson& value)
{
  if (!value.is_number_unsigned())
    throw std::invalid_argument("Wan checkpoint expects an unsigned integer");
  return value.get<std::uint64_t>();
}

bool
isShardLabel(const std::string& name)
{
  constexpr std::string_view ALLOWED =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";
  return !name.empty() && name != "." && name != ".." &&
         name.find_first_not_of(ALLOWED) == std::string::npos;
}

NativeJson
readHeader(const NativeCanonicalByteRangeSource& source, std::uint64_t& payloadStart)
{
  if (source.size() < PREFIX_BYTES)
    throw std::invalid_argument("Wan SafeTensors length prefix is missing");
  const auto prefix = source.read(0, PREFIX_BYTES);
  if (prefix.size() != PREFIX_BYTES)
    throw std::invalid_argument("Wan SafeTensors length prefix is truncated");
  std::uint64_t length = 0;
  for (std::size_t i = 0; i < prefix.size(); ++i)
    length |= static_cast<std::uint64_t>(prefix[i]) << (8 * i);
  if (length == 0 || length > MAX_HEADER_BYTES || length > source.size() - PREFIX_BYTES)
    throw std::invalid_argument("Wan SafeTensors header exceeds its bounds");
  const auto bytes = source.read(PREFIX_BYTES, length);
  if (bytes.size() != length || bytes.front() != '{')
    throw std::invalid_argument("Wan SafeTensors header is invalid");
  payloadStart = PREFIX_BYTES + length;
  return nativeParseJson(std::string(bytes.begin(), bytes.end()), 8);
}

void
setExternalRange(onnx::TensorProto& tensor, const std::string& shard,
                 std::uint64_t start, std::uint64_t length)
{
  tensor.clear_external_data();
  tensor.set_data_location(onnx::TensorProto::EXTERNAL);
  for (const auto& [key, value] : {std::pair<std::string, std::string>{"location", shard},
                                   {"offset", std::to_string(start)},
                                   {"length", std::to_string(length)}}) {
    auto* entry = tensor.add_external_data();
    entry->set_key(key);
    entry->set_value(value);
  }
}

onnx::TensorProto
externalTensor(const std::string& name, const std::string& shard, const NativeJson& row,
               std::uint64_t start, std::uint64_t length)
{
  if (!row.is_object() || row.size() != 3 || !row.contains("shape") ||
      row.value("dtype", NativeJson()) != "F32" || !row.at("shape").is_array() ||
      row.at("shape").size() > 5 || name.empty() || name.find('\0') != std::string::npos)
    throw std::invalid_argument("Wan checkpoint has an unsupported tensor descriptor: " + name);

  onnx::TensorProto tensor;
  tensor.set_name(name);
  tensor.set_data_type(onnx::TensorProto::FLOAT);
  std::uint64_t expected = 4;
  for (const auto& extent : row.at("shape")) {
    const auto dim = unsignedInteger(extent);
    if (dim == 0 || dim > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        dim > std::numeric_limits<std::uint64_t>::max() / expected)
      throw std::invalid_argument("Wan checkpoint tensor extent overflows: " + name);
    expected *= dim;
    tensor.add_dims(static_cast<std::int64_t>(dim));
  }
  if (expected != length)
    throw std::invalid_argument("Wan checkpoint tensor shape and byte length differ: " + name);

  setExternalRange(tensor, shard, start, length);
  return tensor;
}

class TensorRange final : public NativeCanonicalByteRangeSource
{
public:
  TensorRange(NativeOnnxExternalReader reader, std::string shard,
              std::uint64_t offset, std::uint64_t length)
    : m_reader(std::move(reader)), m_shard(std::move(shard)), m_offset(offset), m_length(length)
  {
  }

  std::uint64_t size() const noexcept override { return m_length; }

  std::vector<std::uint8_t>
  read(std::uint64_t offset, std::uint64_t length) const override
  {
    if (offset > m_length || length > m_length - offset)
      throw std::out_of_range("Wan checkpoint tensor read exceeds its range");
    return length == 0 ? std::vector<std::uint8_t>{} : m_reader(m_shard, m_offset + offset, length);
  }

private:
  NativeOnnxExternalReader m_reader;
  std::string m_shard;
  std::uint64_t m_offset;
  std::uint64_t m_length;
};

class TensorSlice final : public NativeCanonicalByteRangeSource
{
public:
  TensorSlice(std::shared_ptr<const NativeCanonicalByteRangeSource> source, InitializerSlice slice)
    : m_source(std::move(source)), m_slice(std::move(slice))
  {
    if (m_source->size() != m_slice.sourceBytes())
      throw std::invalid_argument("Wan checkpoint slice differs from its source tensor");
  }

  std::uint64_t size() const noexcept override { return m_slice.sliceBytes(); }

  std::vector<std::uint8_t>
  read(std::uint64_t offset, std::uint64_t length) const override
  {
    if (length > NativeCanonicalMaterialBundleMaxBytes)
      throw std::out_of_range("Wan checkpoint slice exceeds its read window");
    std::vector<std::uint8_t> output;
    output.reserve(length);
    m_slice.forEachRange(offset, length, NativeCanonicalMaterialBundleMaxBytes,
                         [&](auto from, auto bytes) {
      const auto chunk = m_source->read(from, bytes);
      output.insert(output.end(), chunk.begin(), chunk.end());
    });
    return output;
  }

private:
  std::shared_ptr<const NativeCanonicalByteRangeSource> m_source;
  InitializerSlice m_slice;
};

} // namespace

struct WanCheckpoint::Shard
{
  std::shared_ptr<const NativeCanonicalByteRangeSource> owner;
  std::map<std::uint64_t, std::uint64_t> tensorsByOffset;
};

WanCheckpoint::WanCheckpoint(WanCheckpointShards shards, const std::string& indexJson)
{
  if (shards.empty() || (indexJson.empty() && shards.size() != 1) ||
      indexJson.size() > MAX_HEADER_BYTES)
    throw std::invalid_argument("Wan checkpoint requires one shard or an explicit index");

  const auto index = indexJson.empty() ? NativeJson() : nativeParseJson(indexJson, 8);
  if (!indexJson.empty() && (!index.is_object() || !index.contains("weight_map") ||
      !index.at("weight_map").is_object() || index.at("weight_map").empty() ||
      !index.contains("metadata") || !index.at("metadata").contains("total_size")))
    throw std::invalid_argument("Wan checkpoint weight index is missing");
  const auto weights = index.is_null() ? NativeJson() : index.at("weight_map");

  auto validated = std::make_shared<std::map<std::string, Shard>>();
  std::uint64_t totalBytes = 0;
  for (const auto& [label, source] : shards) {
    if (!source || !isShardLabel(label))
      throw std::invalid_argument("Wan checkpoint has an invalid shard label or owner");
    std::uint64_t payloadStart = 0;
    auto header = readHeader(*source, payloadStart);
    header.erase("__metadata__");
    if (header.empty())
      throw std::invalid_argument("Wan checkpoint shard contains no tensors");
    auto& shard = validated->try_emplace(label, Shard{source, {}}).first->second;
    const auto payloadSize = source->size() - payloadStart;
    for (const auto& [name, row] : header.items()) {
      if (!weights.is_null() && (!weights.contains(name) || weights.at(name) != label))
        throw std::invalid_argument("Wan checkpoint index disagrees with tensor shard: " + name);
      if (!row.contains("data_offsets") || !row.at("data_offsets").is_array() ||
          row.at("data_offsets").size() != 2)
        throw std::invalid_argument("Wan checkpoint tensor range is missing: " + name);
      const auto& range = row.at("data_offsets");
      const auto begin = unsignedInteger(range.at(0));
      const auto end = unsignedInteger(range.at(1));
      if (begin >= end || end > payloadSize ||
          !shard.tensorsByOffset.emplace(payloadStart + begin, payloadStart + end).second)
        throw std::invalid_argument("Wan checkpoint tensor outside its shard: " + name);
      auto tensor = externalTensor(name, label, row, payloadStart + begin, end - begin);
      if (!m_parameters.emplace(name, std::move(tensor)).second)
        throw std::invalid_argument("duplicate Wan checkpoint tensor: " + name);
    }
    auto next = payloadStart;
    for (const auto& [begin, end] : shard.tensorsByOffset) {
      if (begin != next)
        throw std::invalid_argument("Wan checkpoint tensors overlap or leave a gap");
      next = end;
    }
    if (next != source->size() ||
        payloadSize > std::numeric_limits<std::uint64_t>::max() - totalBytes)
      throw std::invalid_argument("Wan checkpoint shard size disagrees with its tensor ranges");
    totalBytes += payloadSize;
  }
  if (!weights.is_null() && (weights.size() != m_parameters.size() ||
      unsignedInteger(index.at("metadata").at("total_size")) != totalBytes))
    throw std::invalid_argument("Wan checkpoint index does not cover its declared payload");
  m_shards = std::move(validated);
}

NativeOnnxExternalReader
WanCheckpoint::reader() const
{
  return [shards = m_shards] (const std::string& label, std::uint64_t offset,
                             std::uint64_t length) {
    const auto shard = shards->find(label);
    if (shard == shards->end())
      throw std::invalid_argument("Wan checkpoint reader received an unknown shard");
    const auto& offsets = shard->second.tensorsByOffset;
    auto range = offsets.upper_bound(offset);
    if (range == offsets.begin())
      throw std::out_of_range("Wan checkpoint reader received a header offset");
    --range;
    if (length == 0 || offset >= range->second || length > range->second - offset)
      throw std::out_of_range("Wan checkpoint read crosses its tensor range");
    auto result = shard->second.owner->read(offset, length);
    if (result.size() != length)
      throw std::invalid_argument("Wan checkpoint tensor read returned a short range");
    return result;
  };
}

std::shared_ptr<const NativeCanonicalByteRangeSource>
WanCheckpoint::tensorSource(const std::string& name) const
{
  const auto& tensor = m_parameters.at(name);
  // These three fields were built by setExternalRange during checkpoint admission.
  return std::make_shared<TensorRange>(reader(), tensor.external_data(0).value(),
    std::stoull(tensor.external_data(1).value()), std::stoull(tensor.external_data(2).value()));
}

WanCheckpointSidecar::WanCheckpointSidecar(
  std::shared_ptr<const NativeCanonicalByteRangeSource> prefix)
{
  if (prefix) append(std::move(prefix));
}

std::uint64_t
WanCheckpointSidecar::append(std::shared_ptr<const NativeCanonicalByteRangeSource> source)
{
  const auto offset = m_size;
  const auto length = source->size();
  if (length == 0 || length > std::numeric_limits<std::uint64_t>::max() - offset)
    throw std::invalid_argument("Wan checkpoint sidecar range overflows");
  m_sources.emplace(offset, std::move(source));
  m_size += length;
  return offset;
}

onnx::TensorProto
WanCheckpointSidecar::addSlice(const WanCheckpoint& checkpoint, const std::string& name,
                              std::size_t axis, std::uint64_t begin, std::uint64_t end)
{
  const auto& tensor = checkpoint.parameters().at(name);
  const std::vector<std::uint64_t> dimensions(tensor.dims().begin(), tensor.dims().end());
  const InitializerSlice range(dimensions, axis, begin, end, sizeof(float));
  onnx::TensorProto result = tensor;
  result.set_dims(static_cast<int>(axis), static_cast<std::int64_t>(end - begin));
  const auto offset = append(
    std::make_shared<TensorSlice>(checkpoint.tensorSource(name), range));
  setExternalRange(result, "model.onnx.data", offset, range.sliceBytes());
  return result;
}

std::vector<std::uint8_t>
WanCheckpointSidecar::read(std::uint64_t offset, std::uint64_t length) const
{
  if (offset > m_size || length > m_size - offset || length > NativeCanonicalMaterialBundleMaxBytes)
    throw std::out_of_range("Wan checkpoint sidecar read exceeds its range or window");
  std::vector<std::uint8_t> output;
  output.reserve(length);
  while (length != 0) {
    const auto source = std::prev(m_sources.upper_bound(offset));
    const auto local = offset - source->first;
    const auto count = std::min(length, source->second->size() - local);
    auto chunk = source->second->read(local, count);
    if (chunk.size() != count)
      throw std::runtime_error("Wan checkpoint sidecar source returned a short range");
    output.insert(output.end(), chunk.begin(), chunk.end());
    offset += count;
    length -= count;
  }
  return output;
}

} // namespace ndnsf::di::wan
