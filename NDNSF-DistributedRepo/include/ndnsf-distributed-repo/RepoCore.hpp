#ifndef NDNSF_DISTRIBUTED_REPO_REPO_CORE_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_CORE_HPP

#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ndnsf_distributed_repo {

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

  std::vector<RepoObjectManifest> list() const;

  bool remove(const std::string& objectName);

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
  bool removeIfCurrent(const RepoObjectManifest& expected);
  bool abortRangesIfOwned(const RepoObjectManifest& expected);
  void putRangeIfAbsent(const RepoObjectManifest& manifest, RepoByteRange range,
                        const std::vector<uint8_t>& bytes);
  RepoObjectManifest commitRangesIfOwned(const RepoObjectManifest& manifest);

  RepoObjectManifest putDataPacket(const std::string& dataName,
                                   const std::vector<uint8_t>& wire);

  std::vector<uint8_t> getDataPacket(const std::string& dataName) const;

  bool hasDataPacket(const std::string& dataName) const;

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
  std::vector<uint8_t> deleteLocked(const std::string& objectName);
  void putRangeLocked(const RepoObjectManifest&, RepoByteRange, const std::vector<uint8_t>&);
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
  };
  std::unordered_map<std::string, RangeReservation> m_rangeReservations;
  uint64_t m_reservedRangeBytes = 0;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_CORE_HPP
