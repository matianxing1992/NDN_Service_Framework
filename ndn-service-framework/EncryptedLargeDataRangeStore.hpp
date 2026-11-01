#pragma once

#include <ndn-cxx/name.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ndn_service_framework {

enum class EncryptedLargeDataRetention
{
  Transient,
  Durable,
};

/** Exact destination selectors, not a portable credential or wire authority. */
struct DurableStorageGrantSelector
{
  std::string repoNode;
  std::string ownerIdentity;
  std::string grantId;
  std::uint64_t ownerEpoch = 0;
};

/** Exact-object authorization supplied by a strongly retained owner lane. */
using DurableStorageGrantResolver = std::function<DurableStorageGrantSelector(
  const ndn::Name&, const std::string&, const std::function<void()>&)>;

/** Trusted Core wiring derives the writer from the producing ServiceUser. */
struct DurableStorageActorContext
{
  std::string authenticatedWriterIdentity;
  DurableStorageGrantSelector grant;
};

struct EncryptedLargeDataCommitOptions
{
  static constexpr std::size_t MaxAuxiliaryPacketCount = 8192;
  static constexpr std::uint64_t MaxAuxiliaryPacketWireBytes = 64ULL * 1024 * 1024;
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
  // Plaintext identity is metadata only; the backend stores no plaintext.
  // These fields let a durable lookup prove that a caller's current input
  // is the same immutable publication before it reuses ciphertext.
  std::string contentDigest;
  std::uint64_t plaintextSize = 0;
  // Distinguish immutable signed packet storage from the historical envelope
  // byte-range format. The backend must never infer this from a Name label.
  std::string packetStorageFormat = "encrypted-envelope-bytes";
  std::string manifestName;
  std::string dataPrefix;
  std::uint64_t packetWireBytes = 0;
  std::optional<DurableStorageActorContext> storageActor;
  // Exact ordered original control packets appended after the envelope segments.
  std::vector<ndn::Name> auxiliaryPacketNames;
};

/**
 * Non-secret exact-object metadata exposed after packet preparation and before
 * Repo commit. This is a challenge description, not storage authority.
 */
struct DurableStorageGrantChallengeV1
{
  ndn::Name manifestName;
  std::string writerIdentity;
  std::string publicationIdentity;
  std::string protectionEpoch;
  ndn::Name dataPrefix;
  std::uint64_t packetWireBytes = 0;
  std::vector<ndn::Name> auxiliaryPacketNames;
};

using DurableStorageGrantChallengeObserverV1 =
  std::function<void(const DurableStorageGrantChallengeV1&)>;

struct EncryptedLargeDataLookupResult
{
  std::string encryptedName;
  std::string publicationIdentity;
  std::string protectionEpoch;
  std::string keyReferenceId;
  std::string keyReferenceVersion;
  std::string ciphertextManifestDigest;
  std::string servingLocator;
  std::string contentDigest;
  std::uint64_t plaintextSize = 0;
  std::shared_ptr<const class EncryptedLargeDataRangeSource> source;
  // Appended after the historical aggregate fields for source compatibility.
  std::string packetStorageFormat = "encrypted-envelope-bytes";
  std::string manifestName;
  std::string dataPrefix;
  std::uint64_t packetWireBytes = 0;
  // Provisional names from the committed packet index for an exact-object
  // grant challenge. These names are not read authority; after retain, the
  // owner-bound source must provide the authoritative list for comparison.
  std::vector<ndn::Name> auxiliaryPacketNames;
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
  /** Check an actual current storage binding, without granting authority or
   * extending a producer/key serving lease. Legacy sources fail closed. */
  virtual void requireCurrentDurableStorageOwner(const ndn::Name&,
                                                const std::string&) const
  {
    throw std::runtime_error("DURABLE_OWNER_BINDING_REQUIRED");
  }
  /** Serialize current owner admission with cleanup through enqueue's return.
   * The synchronous callback may only enqueue Data, never wait for I/O or
   * re-enter this source/store. Already queued packets may remain in flight.
   * Legacy sources fail closed rather than promise a fence they do not own. */
  virtual void withCurrentDurableStorageOwner(const ndn::Name&,
                                            const std::string&,
                                            const std::function<void()>&) const
  {
    throw std::runtime_error("DURABLE_OWNER_BINDING_REQUIRED");
  }
  /** Release a request-owned lease, idempotently. Durable bytes require the
   * storage owner's separate authenticated cleanup operation. */
  virtual void release() const noexcept {}
  virtual std::vector<std::uint8_t> read(std::uint64_t offset,
                                       std::uint64_t length) const = 0;
  /** Return the complete original signed packet, never reconstructed content.
   * Range-only sources cannot satisfy this contract. */
  virtual std::vector<std::uint8_t> readOriginalDataPacket(const ndn::Name&) const
  {
    throw std::runtime_error("ORIGINAL_DATA_PACKET_UNSUPPORTED");
  }
  /** Original auxiliary names from the current owned packet index, never credentials.
   * Implementations may throw when the claim/physical object is unavailable. */
  virtual std::vector<ndn::Name> auxiliaryDataPacketNames() const { return {}; }
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
  /** Fixed adapter policy; request options cannot disable owner admission. */
  virtual bool requiresDurableStorageActor() const noexcept { return false; }
  /** Rebind a provisional durable lookup to the current authorized owner.
   * Trusted-local stores preserve their existing source; enforcing adapters
   * verify destination authority and actual storage before returning a lease. */
  virtual std::shared_ptr<const EncryptedLargeDataRangeSource> retainDurableSource(
    const EncryptedLargeDataLookupResult& candidate,
    const EncryptedLargeDataCommitOptions&,
    const std::function<void()>& requireActive = {})
  {
    if (requireActive) requireActive();
    return candidate.source;
  }
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
    if (options.packetStorageFormat != "encrypted-envelope-bytes")
      throw std::runtime_error("ORIGINAL_PACKET_STORAGE_REQUIRES_PACKET_COMMIT");
    if (options.retention != EncryptedLargeDataRetention::Transient)
      throw std::runtime_error("DURABLE_RETENTION_UNSUPPORTED");
    return commitFile(encryptedName, file, size, requireActive);
  }

  /**
   * Find an immutable Durable ciphertext by its Core-owned publication
   * identity.  A null result is a normal miss.  Backends that do not own a
   * persistent namespace must keep the default miss rather than pretending
   * that a request-scoped source survived a restart.
   */
  virtual std::optional<EncryptedLargeDataLookupResult> lookupDurable(
    const std::string&, const std::function<void()>& = {}) const
  {
    return std::nullopt;
  }

  /** Whether this adapter has an owner that can survive the request lease. */
  virtual bool supportsDurableRetention() const noexcept { return false; }

  virtual bool supportsOriginalDataPackets() const noexcept { return false; }

  /** Consume one bounded already-signed packet at a time. Success commits the
   * complete manifest-referenced packet set; failure preserves older objects.
   * The callback owns encoding/signing and must keep its input owner alive. */
  virtual std::shared_ptr<const EncryptedLargeDataRangeSource> commitOriginalDataPackets(
    const ndn::Name&, const EncryptedLargeDataCommitOptions&,
    const std::function<std::optional<std::pair<ndn::Name,
                           std::vector<std::uint8_t>>>()>&,
    const std::function<void()>& = {})
  {
    throw std::runtime_error("ORIGINAL_DATA_PACKET_UNSUPPORTED");
  }
  /** Whether atomic original packet commits accept a bounded auxiliary suffix. */
  virtual bool supportsAuxiliaryDataPackets() const noexcept { return false; }
};

} // namespace ndn_service_framework
