#ifndef NDNSF_TESTS_WAN_VIDEO_FIXTURE_HPP
#define NDNSF_TESTS_WAN_VIDEO_FIXTURE_HPP

#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-preparation.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-umt5.hpp"

#include <array>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ndnsf::di::wan::fixture {

inline constexpr std::array<float, 6> UMT5_WORDS{1, -1, 2, 3, 100, -50};
inline constexpr float UMT5_EPSILON = 0.25F;

class EmbeddingTable final : public NativeCanonicalByteRangeSource
{
public:
  std::vector<std::uint8_t> bytes;
  std::uint64_t size() const noexcept override { return bytes.size(); }
  std::vector<std::uint8_t> read(std::uint64_t offset, std::uint64_t length) const override
  {
    if (offset > bytes.size() || length > bytes.size() - offset)
      throw std::out_of_range("fixture embedding read outside source");
    return {bytes.begin() + offset, bytes.begin() + offset + length};
  }
};

struct Umt5Source
{
  std::shared_ptr<EmbeddingTable> table;
  Umt5EmbeddingLayout embedding;
  std::vector<Umt5EncoderLayer> layers;
  Umt5RmsLayout finalNorm;
};

onnx::TensorProto
floatTensor(std::initializer_list<int> dims, std::initializer_list<float> data);

Umt5Source
makeUmt5Source();

onnx::ModelProto
makeLinearSource(int tokens, int inWidth, int outWidth,
                 const std::vector<std::pair<int, int>>& nonzero, bool normalized);

std::map<std::string, onnx::TensorProto>
makeVaeWeights();

std::map<std::string, onnx::TensorProto>
makeDitCheckpointWeights();

NativeJson
makeCatalogTemplate(const NativeModelDescriptor& descriptor,
                    const NativeRoleRecipeProfile& profile,
                    const std::string& sourceName, const std::string& manifestDigest,
                    const std::string& artifactRoot, const std::string& serviceName,
                    const std::map<std::string, NativeRoleResourceRequirement>& resources);

/** Write an exact Add graph and independent raw values for the checkpoint CLI. */
NativeJson
writeWanCheckpointOracleFixture(const std::filesystem::path& root);

/** Write the same three tiny canonical graphs for TP, PP and both Hybrid layouts.
 * Return the fixture identity, component catalogs and the per-mode role groups.
 */
NativeJson
writeWanVideoFixture(const std::filesystem::path& root, const ndn::Name& requester,
                     const ndn::Name& service, const std::string& protectionEpoch,
                     bool tileDit = false);

} // namespace ndnsf::di::wan::fixture

#endif // NDNSF_TESTS_WAN_VIDEO_FIXTURE_HPP
