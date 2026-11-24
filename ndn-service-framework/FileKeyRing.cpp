#include "FileKeyRing.hpp"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace ndn_service_framework {

std::vector<FileKeyRingEntry>
readKeyRingFile(const std::filesystem::path& path)
{
  constexpr std::size_t MAX_BYTES = 64 * 1024;
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    throw std::runtime_error("NDNSF_FILE_KEYRING_UNREADABLE: cannot open the configured key ring");

  std::unique_ptr<std::FILE, decltype(&std::fclose)> input(::fdopen(fd, "rb"), &std::fclose);
  if (!input) {
    ::close(fd);
    throw std::runtime_error("NDNSF_FILE_KEYRING_UNREADABLE: cannot read the configured key ring");
  }

  struct stat info{};
  if (::fstat(fd, &info) != 0)
    throw std::runtime_error("NDNSF_FILE_KEYRING_UNREADABLE: cannot inspect the configured key ring");
  if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
      info.st_nlink != 1 || (info.st_mode & 0077) != 0)
    throw std::runtime_error("NDNSF_FILE_KEYRING_INVALID: key ring must be a private owned file");
  if (info.st_size > static_cast<off_t>(MAX_BYTES))
    throw std::runtime_error("NDNSF_FILE_KEYRING_INVALID: key ring exceeds size limit");

  std::string text(MAX_BYTES + 1, '\0');
  const auto size = std::fread(text.data(), 1, text.size(), input.get());
  if (std::ferror(input.get()))
    throw std::runtime_error("NDNSF_FILE_KEYRING_UNREADABLE: cannot read the configured key ring");
  if (size > MAX_BYTES)
    throw std::runtime_error("NDNSF_FILE_KEYRING_INVALID: key ring exceeds size limit");
  text.resize(size);
  return parseKeyRingText(text);
}

std::optional<std::filesystem::path>
fileKeyRingPath()
{
  const char* configured = std::getenv("NDNSF_FILE_KEYRING");
  if (configured == nullptr || *configured == '\0')
    return std::nullopt;
  return std::filesystem::path(configured);
}

std::vector<FileKeyRingEntry>
loadFileKeyRingEntries()
{
  const auto path = fileKeyRingPath();
  if (!path)
    return {};
  return readKeyRingFile(*path);
}

std::optional<FileKeyRingEntry>
loadFileKeyRingEntry(const std::string& keyId)
{
  if (keyId.empty())
    return std::nullopt;
  for (auto& entry : loadFileKeyRingEntries())
    if (entry.keyId == keyId)
      return entry;
  return std::nullopt;
}

} // namespace ndn_service_framework
