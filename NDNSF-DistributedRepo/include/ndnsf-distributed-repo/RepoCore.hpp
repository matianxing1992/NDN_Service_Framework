#ifndef NDNSF_DISTRIBUTED_REPO_REPO_CORE_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_CORE_HPP

#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <ndn-cxx/interest.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>

namespace ndnsf_distributed_repo {

struct RepoUserStorageWrite;

/** Immutable metadata and an owned, transaction-fenced physical range reader. */
struct RepoRangeReadSnapshot
{
  std::shared_ptr<const RepoObjectManifest> manifest;
  RepoRangeReadFunction readRange;
};

class RepoCore
{
public:
  RepoCore(StorageCapability capability, std::shared_ptr<RepoStoreBackend> store);

  RepoObjectManifest put(const std::string& objectName,
                         const std::vector<uint8_t>& payload,
                         const std::string& objectType = "object",
                         uint32_t replicationFactor = 1,
                         const std::string& policyEpoch = "",
                         std::vector<std::string> replicaNodes = {});

  std::vector<uint8_t> get(const std::string& objectName) const;

  /** Typed presence check used by native source owners; avoids parsing error text. */
  bool has(const std::string& objectName) const;

  RepoObjectManifest getManifest(const std::string& objectName) const;

  RepoObjectMetadataProbe probeObjectMetadata(
    const std::string& objectName) const;

  std::vector<RepoObjectManifest> list() const;

  bool remove(const std::string& objectName);
  /** Trusted caller holds physical writer/Owner fences; no permission is granted here. */
  bool canRemoveUserStorageObject(const RepoUserStorageObject& expected) const;
  bool removeUserStorageObject(const RepoUserStorageObject& expected);
  bool confirmUserStorageRemoval(const RepoUserStorageObject& expected);

  RepoObjectManifest putManifest(const RepoObjectManifest& manifest);

  // Bounded large-object authority path.  The manifest is published only by
  // commitRanges after all ranges have been verified by the backend.
  void putRange(const RepoObjectManifest& manifest,
                RepoByteRange range,
                const std::vector<uint8_t>& bytes);
  RepoObjectManifest commitRanges(const RepoObjectManifest& manifest);
  void abortRanges(const std::string& objectName);
  std::vector<uint8_t> getRange(const std::string& objectName,
                                RepoByteRange range) const;

  /** Transaction-bound reads and cleanup. Check and action share m_mutex.
   * expected.operationId must identify the unique publication transaction;
   * callers must never reuse it for a replacement object. */
  std::vector<uint8_t> getRangeIfCurrent(const RepoObjectManifest& expected,
                                       RepoByteRange range) const;
  static RepoRangeReadSnapshot openRangeSnapshot(std::shared_ptr<RepoCore> owner,
                                                 RepoObjectManifest expected);
  bool removeIfCurrent(const RepoObjectManifest& expected);
  bool abortRangesIfOwned(const RepoObjectManifest& expected);
  /** Exact Core and backend staging proof; caller retains Owner/epoch checks. */
  bool abortUserStorageRangesIfOwned(const RepoObjectManifest& expected);
  bool abortFailedRangesIfOwned(const RepoObjectManifest& expected);
  bool abortUserStorageWriteRangesIfOwned(const RepoUserStorageWrite& expected);
  /** Drop idle range staging and its capacity reservation. */
  uint64_t reapExpiredRangeReservations(std::chrono::steady_clock::duration idleTimeout);
  void putRangeIfAbsent(const RepoObjectManifest& manifest, RepoByteRange range,
                        const std::vector<uint8_t>& bytes);
  RepoObjectManifest commitRangesIfOwned(const RepoObjectManifest& manifest);
  /** Reconfirm exact publication and directory durability without granting access. */
  RepoObjectManifest confirmDurablePublicationIfCurrent(const RepoObjectManifest& expected);

  RepoObjectManifest putDataPacket(const std::string& dataName,
                                   const std::vector<uint8_t>& wire,
                                   const std::string& repoNode = {},
                                   const std::string& providerIdentity = {});

  /** Explicit local migration of a previously stored Data wire. */
  RepoWriteReceipt importLegacyDataPacketReceipt(const std::string& dataName,
                                                  const std::string& repoNode,
                                                  const std::string& providerIdentity);

  std::vector<uint8_t> getDataPacket(const std::string& dataName) const;
  std::vector<uint8_t> getDataPacket(const std::string& dataName,
    const std::function<bool(const RepoObjectManifest&)>& acceptObject) const;
  std::vector<uint8_t> getDataPacket(const ndn::Interest& interest,
    const std::function<bool(const RepoObjectManifest&)>& acceptObject = {}) const;

  /** Return only a receipt embedded at the original Data commit boundary. */
  RepoWriteReceipt getDataPacketReceipt(const std::string& dataName,
                                        const std::string& wireSha256) const;

  bool hasDataPacket(const std::string& dataName) const;

  bool supportsOriginalPacketTransactions() const noexcept
  {
    return m_store->supportsRange() && m_store->supportsManifestLookup();
  }

  bool supportsOwnedOriginalPacketTransactions() const noexcept
  {
    return supportsOriginalPacketTransactions() && m_store->supportsUserStorageRecovery();
  }

  RepoObjectManifest commitOriginalPacketFile(
    const RepoObjectManifest& manifest, const std::filesystem::path& file,
    const std::function<void()>& requireActive = {});

  using OriginalPacketRangeReader =
    std::function<std::vector<uint8_t>(uint64_t offset, std::size_t length)>;

  /** Commit immutable original packets from a caller-owned, synchronous source.
   * The reader must return exactly the requested range and remain valid until return.
   * This provides storage integrity, not Producer authentication or Owner permission.
   */
  RepoObjectManifest commitOriginalPacketReader(
    const RepoObjectManifest& manifest, const OriginalPacketRangeReader& readRange,
    const std::function<void()>& requireActive = {});

  std::vector<uint8_t> handleStore(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleStoreRange(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleCommitRanges(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleStoreManifest(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleFetch(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleFetchRange(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleManifest(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleInventory() const;

  std::vector<uint8_t> handleCapability() const;

  RepoCacheStatus cacheStatus() const;

  std::vector<uint8_t> handleCacheStatus() const;

  RepoCatalogStatus catalogStatus() const;

  RepoCatalogDelta catalogSnapshot() const;

  RepoCatalogDelta catalogDelta(uint64_t sinceEpoch) const;

  RepoCatalogEntry catalogLookup(const std::string& objectName) const;

  std::vector<uint8_t> handleCatalogStatus() const;

  std::vector<uint8_t> handleCatalogSnapshot() const;

  std::vector<uint8_t> handleCatalogDelta(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleCatalogLookup(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleDelete(const std::vector<uint8_t>& request);

  /**
   * Serialize a multi-object publication transaction that uses the bounded
   * range API.  The lock is owned by this RepoCore instance, so independent
   * RepoSourceProvider adapters cannot abort one another's staging reservation.
   */
  std::unique_lock<std::mutex> acquirePublicationLock() const
  {
    return std::unique_lock<std::mutex>(m_publicationMutex);
  }

private:
  friend struct RepoCorePacketIndexTestAccess;
  RepoObjectManifest describeDataPacket(const std::string& name) const;
  void validateReadPacketIndex(const RepoObjectManifest& manifest) const;
  RepoObjectManifest commitOriginalPacketReaderImpl(
    const RepoObjectManifest& manifest, const OriginalPacketRangeReader& readRange,
    const std::function<void()>& requireActive, const std::filesystem::path& sourceFile);
  std::optional<std::pair<RepoObjectManifest, size_t>> findPacketRecord(
    const ndn::Name& name,
    const std::function<bool(const RepoObjectManifest&)>& acceptObject = {}) const;
  std::string resolveObjectKey(const std::string& objectName) const;
  std::vector<uint8_t> deleteLocked(const std::string& objectName);
  bool canRemoveUserStorageObjectLocked(const RepoUserStorageObject& expected) const;
  void putRangeLocked(const RepoObjectManifest&, RepoByteRange, const std::vector<uint8_t>&);
  // Caller holds m_mutex and has checked immutable-packet admission.
  void putAdmittedRangeLocked(const RepoObjectManifest&, RepoByteRange, const std::vector<uint8_t>&);
  RepoObjectManifest commitRangesLocked(const RepoObjectManifest&);

  void refreshCapabilityUsage();

  void updateCapabilityUsage(uint64_t oldSize, uint64_t newSize);

  RepoCatalogEntry makeCatalogEntry(const RepoObjectManifest& manifest,
                                    std::string state,
                                    uint64_t epoch) const;

  void rememberCatalogChange(const RepoObjectManifest& manifest,
                             const std::string& state);

  bool recoverAmbiguousCommit(const std::string& objectName,
                              RepoObjectManifest& durable);

  void refreshCapabilityUsageAfterCommit(uint64_t oldSize,
                                         uint64_t newSize) noexcept;

  void clearRangeReservation(const std::string& objectName);
  bool abortUserStorageRangesIfOwnedLocked(const RepoObjectManifest& expected);
  bool abortFailedOwnedRangeIfUncommittedLocked(const RepoObjectManifest& expected);

private:
  StorageCapability m_capability;
  uint64_t m_capacityBytes = 0;
  mutable std::mutex m_mutex;
  mutable std::mutex m_publicationMutex;
  std::shared_ptr<RepoStoreBackend> m_store;
  uint64_t m_catalogEpoch = 0;
  bool m_catalogReconciliationRequired = false;
  std::vector<RepoCatalogEntry> m_catalogChanges;

  struct RangeReservation
  {
    std::string canonicalIdentity;
    std::string digest;
    uint64_t size = 0;
    uint64_t generation = 0;
    uint64_t additionalBytes = 0;
    std::chrono::steady_clock::time_point lastActivity;
  };
  std::unordered_map<std::string, RangeReservation> m_rangeReservations;
  uint64_t m_reservedRangeBytes = 0;

  // Maps a packet-name base (the packet name without its trailing segment)
  // to the manifests that can contain it. A fresh Repo builds this from the
  // durable manifests so exact packet reads never scan the whole store.
  void buildPacketBaseIndex() const;
  std::optional<std::pair<RepoObjectManifest, size_t>> findIndexedOriginalPacket(
    const std::string& packetName) const;
  mutable std::mutex m_packetBaseIndexMutex;
  mutable std::unordered_map<std::string, std::vector<std::string>> m_packetBaseIndex;
  mutable bool m_packetBaseIndexBuilt = false;

  struct PacketIndexProof
  {
    uint64_t size = 0;
    std::vector<std::string> names;
    std::vector<uint64_t> offsets;
    size_t chargedBytes = 0;
  };
  // Structural proofs only. Every read still fetches current metadata and
  // checks its current visibility/claims; this cache never admits an object.
  mutable std::mutex m_packetIndexProofMutex;
  mutable std::unordered_map<std::string, PacketIndexProof> m_packetIndexProofs;
  mutable size_t m_packetIndexProofBytes = 0;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_CORE_HPP
