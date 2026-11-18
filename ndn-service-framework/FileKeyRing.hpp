#ifndef NDNSF_FILE_KEY_RING_HPP
#define NDNSF_FILE_KEY_RING_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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

} // namespace ndn_service_framework

#endif // NDNSF_FILE_KEY_RING_HPP
