#include "native-material-fragment-cache.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstddef>
#include <fcntl.h>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ndnsf::di {
namespace {

bool
isSha256Digest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (unsigned char ch) {
      return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

std::string
sha256Hex(ndn::span<const std::uint8_t> bytes)
{
  ndn::util::Sha256 hash;
  hash.update(bytes);
  auto value = hash.toString();
  std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return "sha256:" + value;
}

bool
writeAll(int fd, const std::uint8_t* data, std::size_t size)
{
  while (size > 0) {
    const auto written = ::write(fd, data, size);
    if (written <= 0) {
      if (written < 0 && errno == EINTR)
        continue;
      return false;
    }
    data += written;
    size -= static_cast<std::size_t>(written);
  }
  return true;
}

} // namespace

NativeMaterialFragmentCache::NativeMaterialFragmentCache(std::filesystem::path root)
  : m_root(std::move(root))
{
}

std::filesystem::path
NativeMaterialFragmentCache::objectPath(const std::string& digest) const
{
  const auto hex = digest.substr(7);
  return m_root / "sha256" / hex.substr(0, 2) / hex;
}

std::optional<std::vector<std::uint8_t>>
NativeMaterialFragmentCache::read(const std::string& digest,
                                  std::uint64_t expectedBytes,
                                  std::uint64_t maxBytes) const
{
  if (m_root.empty() || !isSha256Digest(digest) || expectedBytes == 0 ||
      maxBytes == 0 || expectedBytes > maxBytes)
    return std::nullopt;
  const auto path = objectPath(digest);
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || status.type() != std::filesystem::file_type::regular)
    return std::nullopt;
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return std::nullopt;
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
      static_cast<std::uint64_t>(info.st_size) != expectedBytes) {
    ::close(fd);
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(expectedBytes));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
    if (count <= 0) {
      if (count < 0 && errno == EINTR)
        continue;
      ::close(fd);
      return std::nullopt;
    }
    offset += static_cast<std::size_t>(count);
  }
  ::close(fd);
  if (sha256Hex(ndn::span<const std::uint8_t>(bytes.data(), bytes.size())) != digest)
    return std::nullopt;
  return bytes;
}

bool
NativeMaterialFragmentCache::write(const std::string& digest,
                                   ndn::span<const std::uint8_t> bytes) const
{
  if (m_root.empty() || !isSha256Digest(digest) || bytes.empty() ||
      sha256Hex(bytes) != digest)
    return false;
  const auto path = objectPath(digest);
  std::error_code error;
  const auto existing = std::filesystem::symlink_status(path, error);
  if (!error) {
    if (existing.type() == std::filesystem::file_type::regular) {
      if (read(digest, bytes.size(), bytes.size()))
        return true;
      std::filesystem::remove(path, error);
    }
    else if (existing.type() != std::filesystem::file_type::not_found) {
      std::filesystem::remove_all(path, error);
    }
  }
  const auto parent = path.parent_path();
  std::filesystem::create_directories(parent, error);
  if (error)
    return false;
  for (const auto& directory : {m_root, m_root / "sha256", parent}) {
    std::error_code permissionError;
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace,
                                 permissionError);
  }
  const auto staging = m_root / "staging";
  std::filesystem::create_directories(staging, error);
  if (error)
    return false;
  {
    std::error_code permissionError;
    std::filesystem::permissions(staging, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace,
                                 permissionError);
  }
  static std::atomic<std::uint64_t> sequence{0};
  const auto temporary = staging / ("fragment-" + std::to_string(::getpid()) + "-" +
    std::to_string(sequence.fetch_add(1)));
  const int fd = ::open(temporary.c_str(),
    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (fd < 0)
    return false;
  bool written = writeAll(fd, bytes.data(), bytes.size());
  if (written && ::fsync(fd) != 0)
    written = false;
  if (::close(fd) != 0)
    written = false;
  if (!written) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    // A concurrent writer may have committed the same digest first.
    const auto current = read(digest, bytes.size(), bytes.size());
    std::filesystem::remove(temporary, error);
    return current.has_value();
  }
  return true;
}

} // namespace ndnsf::di
