#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ndn_service_framework {

/** Immutable encrypted envelope. The last owner releases its storage lease.
 * No key, plaintext, NDN name rewriting or signing belongs to this interface. */
class EncryptedLargeDataRangeSource
{
public:
  virtual ~EncryptedLargeDataRangeSource() = default;
  virtual std::uint64_t size() const noexcept = 0;
  virtual std::vector<std::uint8_t> read(std::uint64_t offset,
                                       std::uint64_t length) const = 0;
};

/** Synchronous, bounded publication of an already encrypted envelope.
 * The caller retains the private input file through this call. Success means
 * committed, verified and readable; failure must not remove pre-existing data.
 * Names are allocated by Core, not the backend. Implementations may reject a
 * duplicate name; prepared-handle reuse does not call commitFile again. */
class EncryptedLargeDataRangeStore
{
public:
  virtual ~EncryptedLargeDataRangeStore() = default;
  virtual std::shared_ptr<const EncryptedLargeDataRangeSource> commitFile(
    const std::string& encryptedName, const std::filesystem::path& file,
    std::uint64_t size, const std::function<void()>& requireActive = {}) = 0;
};

} // namespace ndn_service_framework
