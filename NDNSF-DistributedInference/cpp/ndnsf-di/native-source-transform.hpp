#ifndef NDNSF_DI_NATIVE_SOURCE_TRANSFORM_HPP
#define NDNSF_DI_NATIVE_SOURCE_TRANSFORM_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Versioned lowering instruction carried by a transformed ONNX role recipe.
 *
 * The source graph and initializer identities stay bound to the original
 * admitted model; this descriptor only states how one role derives its own
 * executable graph from that source.
 */
struct NativeOnnxSourceTransformV1
{
  std::string schema = "source_transform_v1";
  std::string kind;
  std::string node;
  std::string axis;
  std::string input;
  std::string weight;
  std::string bias;
  std::string output;
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  std::uint64_t extent = 0;
  std::vector<std::int64_t> pads;
  std::vector<std::int64_t> strides;
  std::vector<std::int64_t> dilations;

  void validate() const;
  std::string canonicalJson() const;
  static NativeOnnxSourceTransformV1 parse(const std::string& wire);
};

/** Split an admitted Conv input-channel axis into contiguous rank ranges. */
std::vector<std::pair<std::uint64_t, std::uint64_t>>
nativeConvInputChannelRanges(std::uint64_t extent, std::uint64_t degree);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_SOURCE_TRANSFORM_HPP
