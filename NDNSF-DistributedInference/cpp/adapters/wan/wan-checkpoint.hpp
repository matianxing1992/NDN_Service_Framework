#ifndef NDNSF_DI_WAN_CHECKPOINT_HPP
#define NDNSF_DI_WAN_CHECKPOINT_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"

#include <map>
#include <memory>
#include <string>

namespace ndnsf::di::wan {

using WanCheckpointShards =
  std::map<std::string, std::shared_ptr<const NativeCanonicalByteRangeSource>>;

/** Check the FP32 SafeTensors index and headers without reading tensor payloads.
 * Shard names are explicit local labels and must match the archive index.
 * An unsharded checkpoint may omit the index.
 */
class WanCheckpoint
{
public:
  explicit WanCheckpoint(WanCheckpointShards shards, const std::string& indexJson = {});

  const std::map<std::string, onnx::TensorProto>& parameters() const noexcept
  {
    return m_parameters;
  }

  /** Read only within one verified tensor extent. The reader owns its shard sources. */
  NativeOnnxExternalReader reader() const;

  /** Return a range owner whose offset zero is the start of one named tensor. */
  std::shared_ptr<const NativeCanonicalByteRangeSource>
  tensorSource(const std::string& name) const;

private:
  struct Shard;
  std::map<std::string, onnx::TensorProto> m_parameters;
  std::shared_ptr<const std::map<std::string, Shard>> m_shards;
};

/** Pack bounded checkpoint slices into ordinary ONNX external-data coordinates. */
class WanCheckpointSidecar final : public NativeCanonicalByteRangeSource
{
public:
  explicit WanCheckpointSidecar(std::shared_ptr<const NativeCanonicalByteRangeSource> prefix);

  onnx::TensorProto
  addSlice(const WanCheckpoint& checkpoint, const std::string& name,
           std::size_t axis, std::uint64_t begin, std::uint64_t end);

  std::uint64_t size() const noexcept override { return m_size; }
  std::vector<std::uint8_t> read(std::uint64_t offset, std::uint64_t length) const override;

private:
  std::uint64_t append(std::shared_ptr<const NativeCanonicalByteRangeSource> source);

  std::map<std::uint64_t, std::shared_ptr<const NativeCanonicalByteRangeSource>> m_sources;
  std::uint64_t m_size = 0;
};

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_CHECKPOINT_HPP
