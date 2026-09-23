#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndn_service_framework {

enum class EncryptedLargeDataRetention
{
  Transient,
  Durable,
};

struct EncryptedLargeDataCommitOptions
{
  EncryptedLargeDataRetention retention = EncryptedLargeDataRetention::Transient;

  // Non-secret identity supplied by the Core/crypto owner. The Repo stores
  // these values as opaque manifest metadata; it never unwraps or creates a
  // key from them.
  std::string publicationIdentity;
  std::string protectionEpoch;
  std::string keyReferenceId;
  std::string keyReferenceVersion;
  std::string ciphertextManifestDigest;
  std::string servingLocator;
};

/** Immutable encrypted envelope. The last owner releases its storage lease.
 * No key, plaintext, NDN name rewriting or signing belongs to this interface. */
class EncryptedLargeDataRangeSource
{
public:
  virtual ~EncryptedLargeDataRangeSource() = default;
  virtual std::uint64_t size() const noexcept = 0;
  /** True only when the committed object is not owned by this request lease. */
  virtual bool isDurable() const noexcept { return false; }
  /** Explicit owner-driven invalidation; implementations must be idempotent. */
  virtual void release() const noexcept {}
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

  /**
   * Explicit retention is deliberately a separate overload.  Adapters that
   * only implement the legacy transient path must fail closed for Durable;
   * they must not silently turn a durable request into request-scoped data.
   */
  virtual std::shared_ptr<const EncryptedLargeDataRangeSource> commitFile(
    const std::string& encryptedName, const std::filesystem::path& file,
    std::uint64_t size, const EncryptedLargeDataCommitOptions& options,
    const std::function<void()>& requireActive = {})
  {
    if (options.retention != EncryptedLargeDataRetention::Transient)
      throw std::runtime_error("DURABLE_RETENTION_UNSUPPORTED");
    return commitFile(encryptedName, file, size, requireActive);
  }
};

} // namespace ndn_service_framework
