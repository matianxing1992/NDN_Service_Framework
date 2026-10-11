#include "OriginalPacketFile.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace ndn_service_framework::detail {
namespace {
std::FILE* openPrivateFile(const std::filesystem::path& directory)
{
  const auto pattern = (directory / "ndnsf-packets-XXXXXX").string();
  std::vector<char> path(pattern.begin(), pattern.end());
  path.push_back('\0');
  const int fd = ::mkstemp(path.data());
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "packet file create");
  if (::unlink(path.data()) != 0) {
    const int error = errno;
    ::close(fd);
    throw std::system_error(error, std::generic_category(), "packet file unlink");
  }
  auto* file = ::fdopen(fd, "w+b");
  if (!file) {
    const int error = errno;
    ::close(fd);
    throw std::system_error(error, std::generic_category(), "packet file open");
  }
  return file;
}
void writeExact(std::FILE* file, const void* bytes, std::size_t size)
{
  if (std::fwrite(bytes, 1, size, file) != size)
    throw std::runtime_error("ORIGINAL_PACKET_WRITE_FAILED");
}
void readExact(std::FILE* file, void* bytes, std::size_t size)
{
  if (std::fread(bytes, 1, size, file) != size)
    throw std::runtime_error("ORIGINAL_PACKET_READ_FAILED");
}
void seekExact(std::FILE* file, std::uint64_t offset)
{
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
      ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0)
    throw std::runtime_error("ORIGINAL_PACKET_SEEK_FAILED");
}
void checkPacket(const std::vector<std::uint8_t>& wire, const ndn::Name& expected,
                 std::uint64_t finalSegment)
{
  if (wire.empty() || wire.size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::runtime_error("ORIGINAL_PACKET_SIZE_INVALID");
  ndn::Block block(ndn::span<const std::uint8_t>(wire.data(), wire.size()));
  if (block.size() != wire.size())
    throw std::runtime_error("ORIGINAL_PACKET_TRAILING_BYTES");
  const ndn::Data data(block);
  if (data.getName() != expected)
    throw std::runtime_error("ORIGINAL_PACKET_NAME_MISMATCH");
  if (!data.getFinalBlock() || !data.getFinalBlock()->isSegment() ||
      data.getFinalBlock()->toSegment() != finalSegment)
    throw std::runtime_error("ORIGINAL_PACKET_FINAL_BLOCK_MISMATCH");
}
} // namespace

OriginalPacketFile::~OriginalPacketFile()
{
  if (m_packets) std::fclose(m_packets);
  if (m_index) std::fclose(m_index);
}

std::shared_ptr<const OriginalPacketFile> OriginalPacketFile::create(
  const std::filesystem::path& directory, const ndn::Name& prefix,
  std::uint64_t count, std::uint64_t budget, const NextPacket& next,
  const std::function<void()>& current)
{
  if (prefix.empty() || count == 0 || !next)
    throw std::invalid_argument("ORIGINAL_PACKET_SET_INVALID");
  auto owner = std::shared_ptr<OriginalPacketFile>(new OriginalPacketFile);
  owner->m_dataPrefix = prefix;
  owner->m_segmentCount = count;
  if (current) current();
  owner->m_packets = openPrivateFile(directory);
  owner->m_index = openPrivateFile(directory);
  std::uint64_t offset = 0;
  for (std::uint64_t segment = 0; segment < count; ++segment) {
    if (current) current();
    const auto wire = next(segment);
    if (current) current();
    checkPacket(wire, ndn::Name(prefix).appendSegment(segment), count - 1);
    const std::uint64_t length = wire.size();
    const auto cost = length + 2 * sizeof(std::uint64_t);
    if (owner->m_storageBytes > budget || cost > budget - owner->m_storageBytes)
      throw std::runtime_error("ORIGINAL_PACKET_STORAGE_BUDGET_EXCEEDED");
    writeExact(owner->m_index, &offset, sizeof(offset));
    writeExact(owner->m_packets, &length, sizeof(length));
    writeExact(owner->m_packets, wire.data(), wire.size());
    owner->m_storageBytes += cost;
    offset += sizeof(length) + length;
  }
  if (std::fflush(owner->m_packets) != 0 || std::fflush(owner->m_index) != 0)
    throw std::runtime_error("ORIGINAL_PACKET_FLUSH_FAILED");
  if (current) current();
  return owner;
}

std::vector<std::uint8_t> OriginalPacketFile::read(std::uint64_t segment) const
{
  if (segment >= m_segmentCount ||
      segment > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) /
                  sizeof(std::uint64_t))
    throw std::out_of_range("ORIGINAL_PACKET_SEGMENT_OUT_OF_RANGE");
  std::lock_guard<std::mutex> lock(m_mutex);
  seekExact(m_index, segment * sizeof(std::uint64_t));
  std::uint64_t offset = 0, length = 0;
  readExact(m_index, &offset, sizeof(offset));
  seekExact(m_packets, offset);
  readExact(m_packets, &length, sizeof(length));
  if (length == 0 || length > ndn::MAX_NDN_PACKET_SIZE)
    throw std::runtime_error("ORIGINAL_PACKET_SIZE_INVALID");
  std::vector<std::uint8_t> wire(static_cast<std::size_t>(length));
  readExact(m_packets, wire.data(), wire.size());
  checkPacket(wire, ndn::Name(m_dataPrefix).appendSegment(segment), m_segmentCount - 1);
  return wire;
}
} // namespace ndn_service_framework::detail
