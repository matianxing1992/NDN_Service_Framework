#include "wan-source-layout.hpp"

#include <limits>
#include <stdexcept>

namespace ndnsf::di::wan {
namespace {

std::uint64_t
multiply(std::uint64_t left, std::uint64_t right)
{
  if (right == 0) {
    throw std::invalid_argument("Wan source extent must be positive");
  }
  if (left > std::numeric_limits<std::uint64_t>::max() / right) {
    throw std::overflow_error("Wan source extent exceeds uint64");
  }
  return left * right;
}

} // namespace

VideoGeometry
planVideoGeometry(std::uint64_t frames, std::uint64_t height, std::uint64_t width,
                  std::uint64_t hiddenWidth)
{
  return planVideoGeometry(frames, height, width, hiddenWidth, 8);
}

VideoGeometry
planVideoGeometry(std::uint64_t frames, std::uint64_t height, std::uint64_t width,
                  std::uint64_t hiddenWidth, std::uint64_t vaeSpatialStride)
{
  if (vaeSpatialStride != 8 && vaeSpatialStride != 16)
    throw std::invalid_argument("unsupported Wan VAE spatial stride");
  const auto patchStride = 2 * vaeSpatialStride;
  // Both source VAEs start with one frame, then cover strides of four.
  if (frames == 0 || (frames - 1) % 4 != 0 ||
      height == 0 || height % patchStride != 0 || width == 0 || width % patchStride != 0) {
    throw std::invalid_argument("video shape does not cover complete Wan VAE and DiT patches");
  }
  const auto latentFrames = 1 + (frames - 1) / 4;
  const auto tokenCount = multiply(multiply(latentFrames, height / patchStride), width / patchStride);
  const auto hiddenBytes = multiply(multiply(tokenCount, hiddenWidth), 4);
  const auto rgbBytes = multiply(multiply(multiply(frames, height), width), 3);
  return {latentFrames, height / vaeSpatialStride, width / vaeSpatialStride,
          tokenCount, hiddenBytes, rgbBytes};
}

} // namespace ndnsf::di::wan
