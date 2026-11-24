#ifndef NDNSF_DI_WAN_SOURCE_LAYOUT_HPP
#define NDNSF_DI_WAN_SOURCE_LAYOUT_HPP

#include <cstdint>

namespace ndnsf::di::wan {

struct VideoGeometry
{
  std::uint64_t latentFrames;
  std::uint64_t latentHeight;
  std::uint64_t latentWidth;
  std::uint64_t videoTokens;
  std::uint64_t hiddenBytes;
  std::uint64_t rgbBytes;
};

/** Plan batch-one video geometry for the selected VAE spatial stride. */
VideoGeometry
planVideoGeometry(std::uint64_t frames, std::uint64_t height, std::uint64_t width,
                  std::uint64_t hiddenWidth);

VideoGeometry
planVideoGeometry(std::uint64_t frames, std::uint64_t height, std::uint64_t width,
                  std::uint64_t hiddenWidth, std::uint64_t vaeSpatialStride);

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_SOURCE_LAYOUT_HPP
