#ifndef NDNSF_DI_ONNX_INITIALIZER_SLICE_HPP
#define NDNSF_DI_ONNX_INITIALIZER_SLICE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace ndnsf::di {

/** Row-major byte ranges for a half-open slice along one initializer axis.
 * Each block can be streamed independently, including from ONNX external data.
 */
class InitializerSlice
{
public:
  InitializerSlice(const std::vector<std::uint64_t>& dimensions, std::size_t axis,
                   std::uint64_t begin, std::uint64_t end, std::uint64_t elementBytes);

  std::uint64_t sourceBytes() const noexcept { return m_sourceBytes; }
  std::uint64_t sliceBytes() const noexcept { return m_sliceBytes; }
  std::uint64_t blockCount() const noexcept { return m_blockCount; }
  std::uint64_t blockBytes() const noexcept { return m_blockBytes; }

  /** Offset relative to the beginning of the source initializer. */
  std::uint64_t sourceOffset(std::uint64_t block) const;

  /** Visit the source ranges in rank-output order, with at most windowBytes per read. */
  template<typename Visitor>
  void forEachRange(std::uint64_t windowBytes, Visitor&& visit) const
  {
    forEachRange(0, m_sliceBytes, windowBytes, visit);
  }

  /** Map a bounded byte window of the compact slice back to the source. */
  template<typename Visitor>
  void forEachRange(std::uint64_t offset, std::uint64_t length,
                    std::uint64_t windowBytes, Visitor&& visit) const
  {
    if (windowBytes == 0) {
      throw std::invalid_argument("initializer slice read window is empty");
    }
    if (offset > m_sliceBytes || length > m_sliceBytes - offset) {
      throw std::out_of_range("initializer slice read exceeds the compact range");
    }
    while (length > 0) {
      const auto block = offset / m_blockBytes;
      const auto within = offset % m_blockBytes;
      const auto count = std::min({length, windowBytes, m_blockBytes - within});
      visit(sourceOffset(block) + within, count);
      offset += count;
      length -= count;
    }
  }

private:
  static std::uint64_t checkedProduct(std::uint64_t left, std::uint64_t right);

  std::uint64_t m_sourceBytes;
  std::uint64_t m_sliceBytes;
  std::uint64_t m_blockCount;
  std::uint64_t m_blockBytes;
  std::uint64_t m_sourceStride;
  std::uint64_t m_firstOffset;
};

inline std::uint64_t
InitializerSlice::checkedProduct(std::uint64_t left, std::uint64_t right)
{
  if (right == 0) {
    throw std::invalid_argument("Initializer extent must be positive");
  }
  if (left > std::numeric_limits<std::uint64_t>::max() / right) {
    throw std::overflow_error("Initializer extent exceeds uint64");
  }
  return left * right;
}

inline
InitializerSlice::InitializerSlice(const std::vector<std::uint64_t>& dimensions,
                                   std::size_t axis, std::uint64_t begin,
                                   std::uint64_t end, std::uint64_t elementBytes)
{
  if (axis >= dimensions.size() || begin >= end || end > dimensions[axis] ||
      elementBytes == 0) {
    throw std::invalid_argument("invalid initializer slice");
  }
  auto innerBytes = elementBytes;
  for (auto index = axis + 1; index < dimensions.size(); ++index) {
    innerBytes = checkedProduct(innerBytes, dimensions[index]);
  }
  m_blockCount = 1;
  for (std::size_t index = 0; index < axis; ++index) {
    m_blockCount = checkedProduct(m_blockCount, dimensions[index]);
  }
  m_sourceStride = checkedProduct(innerBytes, dimensions[axis]);
  m_sourceBytes = checkedProduct(m_blockCount, m_sourceStride);
  m_blockBytes = checkedProduct(innerBytes, end - begin);
  m_sliceBytes = checkedProduct(m_blockCount, m_blockBytes);
  m_firstOffset = innerBytes * begin;
}

inline std::uint64_t
InitializerSlice::sourceOffset(std::uint64_t block) const
{
  if (block >= m_blockCount) {
    throw std::out_of_range("initializer slice block is out of range");
  }
  // The constructor bounds all offsets by the checked full source size.
  return block * m_sourceStride + m_firstOffset;
}

} // namespace ndnsf::di

#endif // NDNSF_DI_ONNX_INITIALIZER_SLICE_HPP
