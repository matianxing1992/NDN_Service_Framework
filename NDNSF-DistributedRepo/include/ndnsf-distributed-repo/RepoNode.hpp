#ifndef NDNSF_DISTRIBUTED_REPO_REPO_NODE_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_NODE_HPP

#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"
#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"

#include <ndn-cxx/face.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <future>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace ndn_service_framework {
class LocalServiceRegistry;
class ResponseMessage;
class ServiceProvider;
class ServiceUser;
} // namespace ndn_service_framework

namespace ndnsf_distributed_repo {

namespace detail {

/** Bounded original-wire source borrowed synchronously by Repo's commit path. */
class DataReferencePacketSource
{
public:
  virtual ~DataReferencePacketSource() = default;
  virtual std::size_t packetCount() const noexcept = 0;
  virtual uint64_t totalWireBytes() const = 0;
  virtual std::vector<uint8_t> readPacket(std::size_t index) const = 0;
  virtual std::vector<uint8_t> readRange(uint64_t offset, std::size_t length) const = 0;
};

} // namespace detail

class RepoNode : public std::enable_shared_from_this<RepoNode>
{
public:
  using DataReferenceFetcher =
    std::function<std::vector<std::vector<uint8_t>>(const RepoDataReference&)>;
  using DataReferenceSpoolFetcher =
    std::function<std::shared_ptr<detail::DataReferencePacketSource>(const RepoDataReference&)>;

  RepoNode(ndn::Name servicePrefix,
           StorageCapability capability,
           std::shared_ptr<RepoStoreBackend> store,
           std::string repoNode = {},
           std::string providerIdentity = {},
           std::string artifactMetadataPath = {},
           ArtifactLimits artifactLimits = {},
           uint64_t availabilityFreshnessMs = 5000);

  ~RepoNode();

  const ndn::Name& servicePrefix() const;

  // Low-level mutable access bypasses RepoNode's storage transaction mutex.
  // Callers must serialize Core mutations against Persistent Repo operations.
  RepoCore& core();

  const RepoCore& core() const;

  void registerLocalServices(ndn_service_framework::LocalServiceRegistry& registry);

  /** Register the Persistent network operations on a Core Provider.
   *
   * The node must be owned by std::shared_ptr before registration. Provider
   * callbacks retain that owner until the Provider releases its handlers.
   */
  void registerServices(ndn_service_framework::ServiceProvider& provider);
  void registerServices(ndn_service_framework::ServiceProvider& provider,
                        std::shared_ptr<ndn_service_framework::ServiceUser> publisherUser,
                        int ackCollectionTimeMs = 250,
                        int requestTimeoutMs = 5000);

  /** Serve committed original Data packets under their producer names.
   *  The per-Repo Provider identity is the forwarding hint route. The Face
   *  must outlive this RepoNode, and callers must opt in explicitly.
   */
  void registerOriginalDataServing(ndn::Face& face);
  bool isOriginalDataServingReady() const noexcept;

  RepoObjectManifest put(const std::string& objectName,
                         const std::vector<uint8_t>& payload,
                         const std::string& objectType = "object",
                         uint32_t replicationFactor = 1,
                         const std::string& policyEpoch = "",
                         std::vector<std::string> replicaNodes = {});

  RepoObjectManifest putManifest(const RepoObjectManifest& manifest);

  std::vector<uint8_t> get(const std::string& objectName) const;

  void putRange(const RepoObjectManifest& manifest,
                RepoByteRange range,
                const std::vector<uint8_t>& bytes);

  RepoObjectManifest commitRanges(const RepoObjectManifest& manifest);

  void abortRanges(const std::string& objectName);

  std::vector<uint8_t> getRange(const std::string& objectName,
                                RepoByteRange range) const;

  RepoObjectManifest getManifest(const std::string& objectName) const;

  std::vector<RepoObjectManifest> list() const;

  bool remove(const std::string& objectName);

  RepoObjectManifest putDataPacket(const std::string& dataName,
                                   const std::vector<uint8_t>& wire);

  RepoWriteReceipt importLegacyDataPacketReceipt(const std::string& dataName);

  std::vector<uint8_t> getDataPacket(const std::string& dataName) const;
  std::vector<uint8_t> getDataPacket(const ndn::Interest& interest) const;
  /** Validated Data from the same lookup, so the serving worker can send it
   *  without parsing the wire a second time. */
  ndn::Data getDataPacketObject(const ndn::Interest& interest) const;

  bool hasDataPacket(const std::string& dataName) const;

  /** Configure while quiescent; fetch callbacks must support concurrent requests. */
  void setDataReferenceFetcher(DataReferenceFetcher fetcher);
  void setDataReferenceSpoolFetcher(DataReferenceSpoolFetcher fetcher);
  void stopOriginalPacketOperations() noexcept;
  bool isStoppingOriginalPacketOperations() const noexcept;

  RepoOperationStatus insertWirePackets(
    const RepoDataReference& reference,
    const std::vector<std::vector<uint8_t>>& wirePackets);

  /** Trusted adapter: caller supplies the authenticated writer; network uses Core validation. */
  RepoOperationStatus insertWirePackets(
    const RepoUserStorageInsertRequest& request,
    const std::vector<std::vector<uint8_t>>& wirePackets,
    const std::string& authenticatedWriter);

  std::vector<uint8_t> handleStore(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleStoreRange(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleCommitRanges(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleAbortRanges(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleInsert(const std::vector<uint8_t>& request);

  /** Trusted adapters supply the Owner authenticated by Core, never the payload. */
  std::vector<uint8_t> handleUserStorageGrant(const std::vector<uint8_t>& request,
                                            const std::string& authenticatedOwner);
  std::vector<uint8_t> handleUserStorageUsage(const std::vector<uint8_t>& request,
                                            const std::string& authenticatedOwner) const;
  std::vector<uint8_t> handleUserStorageList(const std::vector<uint8_t>& request,
                                           const std::string& authenticatedOwner) const;
  std::vector<uint8_t> handleUserStorageDelete(const std::vector<uint8_t>& request,
                                             const std::string& authenticatedOwner);

  std::vector<uint8_t> handleStoreManifest(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleFetch(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleFetchRange(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleManifest(const std::vector<uint8_t>& request) const;
  std::vector<uint8_t> handleManifest(const std::vector<uint8_t>& request,
                                    const std::string& authenticatedWriter) const;

  std::vector<uint8_t> handleDataReceipt(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleDataLocation(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleInventory() const;

  std::vector<uint8_t> handleCapability() const;

  RepoCacheStatus cacheStatus() const;

  std::vector<uint8_t> handleCacheStatus() const;

  std::vector<uint8_t> handleCatalogStatus() const;

  std::vector<uint8_t> handleCatalogSnapshot() const;

  std::vector<uint8_t> handleCatalogDelta(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleCatalogLookup(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleDelete(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleStatus(const std::vector<uint8_t>& request) const;

private:
  friend struct RepoNodeTestAccess;
  void executeStorageWrite(uint64_t bytes, std::function<void()> write,
                           const std::function<void()>& requireActive = {});
  void runStorageWriteWorker() noexcept;
  void requestStorageWriteWorkerStop() noexcept;
  void stopStorageWriteWorker() noexcept;
  ArtifactCapability artifactCapability() const;
  bool enqueueOriginalDataInterest(const ndn::Interest& interest);
  void runOriginalDataWorker(ndn::Face& face);
  void requestOriginalDataWorkerStop() noexcept;
  void stopOriginalDataWorker() noexcept;
  RepoUserStorageObject measurePersistentUserStorageObject(
    const RepoObjectManifest& manifest, bool verifyPayload = true) const;
  void reconcilePersistentUserStorage();
  RepoUserStorageWrite reservePersistentOriginalPacketStorage(
    const RepoObjectManifest& manifest, const std::string& operationId,
    const std::string& authenticatedWriter, const RepoUserStorageBinding& binding);
  RepoOperationStatus insertWirePacketsImpl(
    const RepoDataReference& reference, std::size_t packetCount,
    const std::function<std::vector<uint8_t>(std::size_t)>& readPacket,
    const RepoCore::OriginalPacketRangeReader& readRange,
    const std::string& operationId, uint64_t generation,
    const std::string& authenticatedWriter,
    const std::optional<RepoUserStorageBinding>& storageBinding,
    const std::function<void()>& requireActive = {});
  std::vector<uint8_t> handleInsert(const std::vector<uint8_t>& request,
                                   const std::string& requesterIdentity, bool networkRequest);

  bool isPersistentArtifactVisible(const RepoObjectManifest& manifest) const;
  void requirePersistentArtifactVisible(const RepoObjectManifest& manifest) const;

  void validatePersistentWriteIdentity(const ArtifactReference& reference,
                                       const std::string& operationId,
                                       uint64_t generation,
                                       const std::string& requesterIdentity,
                                       const std::optional<RepoArtifactTarget>& operationOwner,
                                       bool allowRemoteOwnerQuery = true,
                                       bool forceRemoteOwnerQuery = false) const;

  std::vector<uint8_t> handleStore(const std::vector<uint8_t>& request,
                                   const std::string& requesterIdentity,
                                   bool networkRequest);
  std::vector<uint8_t> handleStoreRange(const std::vector<uint8_t>& request,
                                        const std::string& requesterIdentity,
                                        bool networkRequest);
  std::vector<uint8_t> handleCommitRanges(const std::vector<uint8_t>& request,
                                          const std::string& requesterIdentity,
                                          bool networkRequest);
  std::vector<uint8_t> handleAbortRanges(const std::vector<uint8_t>& request,
                                         const std::string& requesterIdentity,
                                         bool networkRequest);
  std::vector<uint8_t> handleStatus(const std::vector<uint8_t>& request,
                                    const std::string& requesterIdentity) const;
  std::vector<uint8_t> handlePublishIntent(const std::vector<uint8_t>& request,
                                           const std::string& requesterIdentity);

  // Caller holds m_storeWriteMutex across byte validation and receipt publish.
  std::vector<uint8_t> handlePersistentCommitRanges(
    const ArtifactRangeCommitRequest& commit, bool payloadAlreadyCommitted,
    const std::string& requesterIdentity = {});

  void startPublishOperationWorker(
                                   std::shared_ptr<ndn_service_framework::ServiceUser> publisherUser,
                                   int ackCollectionTimeMs,
                                   int requestTimeoutMs);
  void stopPublishOperationWorker() noexcept;
  void enqueuePublishOperation(const std::string& operationId);
  void runPublishOperationWorker();
  bool runPublishOperationPass(const std::string& operationId);

  ndn_service_framework::ResponseMessage makeResponse(const std::vector<uint8_t>& payload) const;

  ndn_service_framework::ResponseMessage makeError(const std::string& error) const;

  std::string allocateOperationId();

  void rememberStatus(const RepoOperationStatus& status);

  void startRangeReservationReaper();
  void stopRangeReservationReaper() noexcept;

private:
  ndn::Name m_servicePrefix;
  std::string m_repoNode;
  std::string m_providerIdentity;
  ArtifactLimits m_artifactLimits;
  uint64_t m_availabilityFreshnessMs;
  std::unique_ptr<SqliteArtifactMetadataStore> m_artifactMetadataStore;
  RepoCore m_core;
  // Keeps legacy Repo mutations from interleaving a persistent artifact byte
  // commit and its immutable receipt publication.
  mutable std::mutex m_storeWriteMutex;
  struct StorageWriteJob
  {
    uint64_t bytes;
    std::function<void()> write;
    std::shared_ptr<std::promise<void>> completion;
  };
  static constexpr uint64_t STORAGE_WRITE_BYTE_LIMIT = 64U << 20;
  std::mutex m_storageWriteQueueMutex;
  std::condition_variable m_storageWriteCondition;
  std::deque<StorageWriteJob> m_storageWrites;
  uint64_t m_storageWriteBytes = 0;
  bool m_storageWriteStopping = false;
  std::thread m_storageWriteWorker;
  DataReferenceFetcher m_dataReferenceFetcher;
  DataReferenceSpoolFetcher m_dataReferenceSpoolFetcher;
  std::atomic<bool> m_ownedInsertStopping{false};
  bool m_originalDataServingRegistered = false;
  std::atomic<bool> m_originalDataRouteReady{false};
  ndn::ScopedInterestFilterHandle m_originalDataFilter;
  ndn::ScopedRegisteredPrefixHandle m_originalDataRoute;
  struct OriginalDataJob
  {
    ndn::Interest interest;
    std::chrono::steady_clock::time_point deadline;
    std::size_t byteCharge;
  };
  static constexpr std::size_t ORIGINAL_DATA_PENDING_BYTE_LIMIT = 64U << 20;
  std::mutex m_originalDataMutex;
  std::condition_variable m_originalDataWake;
  std::deque<OriginalDataJob> m_originalDataQueue;
  std::shared_ptr<std::atomic<std::size_t>> m_originalDataPending =
    std::make_shared<std::atomic<std::size_t>>(0);
  std::shared_ptr<std::atomic<std::size_t>> m_originalDataPendingBytes =
    std::make_shared<std::atomic<std::size_t>>(0);
  std::size_t m_originalDataAdmissionDrops = 0;
  std::size_t m_originalDataPeakPending = 0;
  std::size_t m_originalDataPeakPendingBytes = 0;
  bool m_stopOriginalDataWorker = false;
  std::mutex m_originalDataJoinMutex;
  std::thread m_originalDataWorker;
  mutable std::mutex m_statusMutex;
  std::map<std::string, RepoOperationStatus> m_statusById;
  uint64_t m_nextOperationId = 0;
  std::mutex m_rangeReaperMutex;
  std::condition_variable m_rangeReaperWake;
  bool m_stopRangeReaper = false;
  std::thread m_rangeReaperThread;
  std::shared_ptr<ndn_service_framework::ServiceUser> m_publishUser;
  struct VerifiedRemotePublishBinding
  {
    ArtifactReference reference;
    uint64_t generation = 0;
    RepoArtifactTarget owner;
    RepoArtifactTarget target;
    std::chrono::steady_clock::time_point verifiedAt;
  };
  mutable std::mutex m_remotePublishBindingMutex;
  mutable std::map<std::string, VerifiedRemotePublishBinding> m_remotePublishBindings;
  mutable std::mutex m_publishWorkerMutex;
  std::condition_variable m_publishWorkerWake;
  std::deque<std::string> m_publishQueue;
  std::set<std::string> m_publishQueued;
  bool m_stopPublishWorker = false;
  int m_publishAckCollectionTimeMs = 250;
  int m_publishRequestTimeoutMs = 5000;
  std::thread m_publishWorkerThread;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_NODE_HPP
