#ifndef NDNSF_DISTRIBUTED_REPO_REPO_NODE_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_NODE_HPP

#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"
#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"

#include <functional>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace ndn_service_framework {
class LocalServiceRegistry;
class ResponseMessage;
class ServiceProvider;
} // namespace ndn_service_framework

namespace ndnsf_distributed_repo {

class RepoNode : public std::enable_shared_from_this<RepoNode>
{
public:
  using DataReferenceFetcher =
    std::function<std::vector<std::vector<uint8_t>>(const RepoDataReference&)>;

  RepoNode(ndn::Name servicePrefix,
           StorageCapability capability,
           std::shared_ptr<RepoStoreBackend> store,
           std::string repoNode = {},
           std::string providerIdentity = {},
           std::string artifactMetadataPath = {},
           ArtifactLimits artifactLimits = {});

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

  std::vector<uint8_t> getDataPacket(const std::string& dataName) const;

  bool hasDataPacket(const std::string& dataName) const;

  void setDataReferenceFetcher(DataReferenceFetcher fetcher);

  RepoOperationStatus insertWirePackets(
    const RepoDataReference& reference,
    const std::vector<std::vector<uint8_t>>& wirePackets);

  std::vector<uint8_t> handleStore(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleStoreRange(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleCommitRanges(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleAbortRanges(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleInsert(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleStoreManifest(const std::vector<uint8_t>& request);

  std::vector<uint8_t> handleFetch(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleFetchRange(const std::vector<uint8_t>& request) const;

  std::vector<uint8_t> handleManifest(const std::vector<uint8_t>& request) const;

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
  ArtifactCapability artifactCapability() const;

  void validatePersistentWriteIdentity(const ArtifactReference& reference,
                                       const std::string& operationId,
                                       uint64_t generation) const;

  // Caller holds m_storeWriteMutex across byte validation and receipt publish.
  std::vector<uint8_t> handlePersistentCommitRanges(
    const ArtifactRangeCommitRequest& commit, bool payloadAlreadyCommitted);

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
  std::unique_ptr<SqliteArtifactMetadataStore> m_artifactMetadataStore;
  RepoCore m_core;
  // Keeps legacy Repo mutations from interleaving a persistent artifact byte
  // commit and its immutable receipt publication.
  std::mutex m_storeWriteMutex;
  DataReferenceFetcher m_dataReferenceFetcher;
  mutable std::mutex m_statusMutex;
  std::map<std::string, RepoOperationStatus> m_statusById;
  uint64_t m_nextOperationId = 0;
  std::mutex m_rangeReaperMutex;
  std::condition_variable m_rangeReaperWake;
  bool m_stopRangeReaper = false;
  std::thread m_rangeReaperThread;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_NODE_HPP
