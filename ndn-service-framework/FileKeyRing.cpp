#include "FileKeyRing.hpp"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace ndn_service_framework {
namespace {

std::vector<std::uint8_t>
parseKey(const std::string& hex)
{
  if (hex.size() != 64)
    throw std::runtime_error("NDNSF_FILE_KEYRING_INVALID: key must be 64 hex characters");
  auto value = [] (char c) -> unsigned {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
    throw std::runtime_error("NDNSF_FILE_KEYRING_INVALID: key is not hexadecimal");
  };
  std::vector<std::uint8_t> key;
  key.reserve(32);
  for (std::size_t index = 0; index < hex.size(); index += 2)
    key.push_back(static_cast<std::uint8_t>((value(hex[index]) << 4) | value(hex[index + 1])));
  return key;
}

} // namespace

std::optional<std::filesystem::path>
fileKeyRingPath()
{
  const char* configured = std::getenv("NDNSF_FILE_KEYRING");
  if (configured == nullptr || *configured == '\0')
    return std::nullopt;
  return std::filesystem::path(configured);
}

std::optional<FileKeyRingEntry>
loadFileKeyRingEntry(const std::string& keyId)
{
  const auto path = fileKeyRingPath();
  if (!path || keyId.empty())
    return std::nullopt;
  struct stat info{};
  if (::lstat(path->c_str(), &info) != 0)
    throw std::runtime_error("NDNSF_FILE_KEYRING_UNREADABLE: cannot stat the configured key ring");
  if (!S_ISREG(info.st_mode) || S_ISLNK(info.st_mode) || info.st_uid != ::geteuid() ||
      (info.st_mode & 0077) != 0)
    throw std::runtime_error("NDNSF_FILE_KEYRING_INVALID: key ring must be a private owned file");
  std::ifstream input(*path);
  if (!input)
    throw std::runtime_error("NDNSF_FILE_KEYRING_UNREADABLE: cannot open the configured key ring");
  std::optional<FileKeyRingEntry> found;
  bool duplicated = false;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    const auto space = line.find(' ');
    if (space == std::string::npos || space == 0)
      throw std::runtime_error("NDNSF_FILE_KEYRING_INVALID: each line is '<keyId> <hex>'");
    const auto id = line.substr(0, space);
    if (id.find_first_of("\r\n\t") != std::string::npos)
      throw std::runtime_error("NDNSF_FILE_KEYRING_INVALID: key ID is invalid");
    // Every line is validated, not just the requested one: a malformed key ring is a
    // deployment error, and silently skipping the bad line would hide it.
    const auto key = parseKey(line.substr(space + 1));
    if (id != keyId)
      continue;
    if (found)
      duplicated = true;
    else
      found = FileKeyRingEntry{id, key};
  }
  if (duplicated)
    throw std::runtime_error("NDNSF_FILE_KEYRING_INVALID: duplicate key ID in the key ring");
  return found;
}

} // namespace ndn_service_framework
