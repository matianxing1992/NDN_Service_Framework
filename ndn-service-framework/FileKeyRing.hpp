#ifndef NDNSF_FILE_KEY_RING_HPP
#define NDNSF_FILE_KEY_RING_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <stdexcept>
#include <utility>

namespace ndn_service_framework {

/** One named file-protection key. */
struct FileKeyRingEntry
{
  std::string keyId;
  std::vector<std::uint8_t> key;
};

/** Path of the deployment key ring, from `NDNSF_FILE_KEYRING`.
 *
 * The environment carries only where the keys live; the keys themselves stay in a private
 * file so a process listing or a shell history never holds one.
 */
std::optional<std::filesystem::path>
fileKeyRingPath();

/** Look one key up by ID. Returns nothing when the ring does not define it.
 *
 * The file must be owned by the caller with mode 0600 and must not be a symlink; every line is
 * `keyId hex`, and a duplicate or malformed line rejects the whole ring rather than silently
 * picking one.
 */
std::optional<FileKeyRingEntry>
loadFileKeyRingEntry(const std::string& keyId);

/** Every key in the ring, in file order.
 *
 * The first line is the write key for new material; the rest keep older material readable, which
 * is what lets a rotation read the previous generation and write the next one.
 */
std::vector<FileKeyRingEntry>
loadFileKeyRingEntries();

/** Parse the ring text. Every line is validated even when the caller wants one ID, and a
 * duplicate ID rejects the whole ring rather than letting "first one wins" pick a key. */
inline std::vector<FileKeyRingEntry>
parseKeyRingText(const std::string& text)
{
  auto value = [] (char ch) -> unsigned {
    if (ch >= '0' && ch <= '9') return static_cast<unsigned>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<unsigned>(ch - 'a' + 10);
    if (ch >= 'A' && ch <= 'F') return static_cast<unsigned>(ch - 'A' + 10);
    throw std::runtime_error("NDNSF_FILE_KEYRING_INVALID: key is not hexadecimal");
  };
  std::vector<FileKeyRingEntry> entries;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    auto end = text.find('\n', begin);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(begin, end - begin);
    begin = end + 1;
    if (line.empty() || line[0] == '#')
      continue;
    const auto space = line.find(' ');
    if (space == std::string::npos || space == 0)
      throw std::runtime_error("NDNSF_FILE_KEYRING_INVALID: each line is '<keyId> <hex>'");
    FileKeyRingEntry entry;
    entry.keyId = line.substr(0, space);
    const auto hex = line.substr(space + 1);
    if (hex.size() != 64)
      throw std::runtime_error("NDNSF_FILE_KEYRING_INVALID: key must be 64 hex characters");
    entry.key.reserve(32);
    for (std::size_t index = 0; index < hex.size(); index += 2)
      entry.key.push_back(static_cast<std::uint8_t>((value(hex[index]) << 4) |
                                                    value(hex[index + 1])));
    for (const auto& existing : entries)
      if (existing.keyId == entry.keyId)
        throw std::runtime_error("NDNSF_FILE_KEYRING_INVALID: duplicate key ID in the key ring");
    entries.push_back(std::move(entry));
  }
  return entries;
}

/** Read a bounded private key ring, validating the open descriptor. */
std::vector<FileKeyRingEntry>
readKeyRingFile(const std::filesystem::path& path);

} // namespace ndn_service_framework

#endif // NDNSF_FILE_KEY_RING_HPP
