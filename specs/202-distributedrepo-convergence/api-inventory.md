# Current API and Source Ownership Inventory

**Baseline**: Repo `2fcfb415` + preserved dirty overlay；STATIC_ONLY inventory，不证明旧artifact等于当前source。

## Exact Header Declarations and Overloads

下表按当前三个主要公开header逐声明登记，保留同名overload；mode构造/值类型validate与wire codec的normal/negative由native suites覆盖。具体59case定义见validation-matrix；每个request及local项必须结合其同名operation/profile核对，不能将local调用作为network证明。

| Public declaration | Definition TU | Build owner/output | Acceptance |
| --- | --- | --- | --- |
| `ArtifactRepositoryApi::makeInApp(InAppRepoConfig config)` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A01–04 |
| `ArtifactRepositoryApi::makePersistent(ndn_service_framework::ServiceUser& serviceUser, PersistentRepoClientConfig config = {})` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A04–05 |
| `ArtifactRepositoryApi::publishFile(const std::string& sourcePath, const ArtifactReference& reference)` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A01–12/A14 |
| `ArtifactRepositoryApi::publishFile(const std::string& sourcePath, const ArtifactReference& reference, const SelectedRepoSet& selected, int timeoutMs)` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A01–12/A14 |
| `ArtifactRepositoryApi::beginPublishFile(const std::string& sourcePath, const ArtifactReference& reference, const SelectedRepoSet& selected, const std::string& idempotencyKey, int timeoutMs)` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A10–12 |
| `ArtifactRepositoryApi::beginArchive(const ArtifactRepositoryApi& inAppSource, const ArtifactHandle& handle, const SelectedRepoSet& selected, const std::string& idempotencyKey, int timeoutMs)` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A13 |
| `ArtifactRepositoryApi::getPublishStatus(const PublishOperationHandle& handle, int timeoutMs = 0)` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A10–12 |
| `ArtifactRepositoryApi::discoverRepoCandidates(const ArtifactCapabilityRequirements& requirements, int timeoutMs)` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A05/A09 |
| `ArtifactRepositoryApi::archive(const ArtifactRepositoryApi& inAppSource, const ArtifactHandle& handle, const SelectedRepoSet& selected, int timeoutMs)` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A13 |
| `ArtifactRepositoryApi::fetchFile(const ArtifactReference& reference, const std::string& destinationPath)` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A02–03/A15–22/R01–05 |
| `ArtifactRepositoryApi::fetchFile(const ArtifactHandle& handle, const std::string& destinationPath, int timeoutMs)` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A02–03/A15–22/R01–05 |
| `ArtifactRepositoryApi::lookupLocalCommit(const ArtifactReference& reference)` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A01–03 |
| `ArtifactRepositoryApi::getReceipt(const std::string& receiptId, std::optional<ArtifactHandle> handle = std::nullopt, int timeoutMs = 0)` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A16 |
| `ArtifactRepositoryApi::checkAvailability(const ArtifactHandle& handle, const std::string& probeKind = "metadata", int timeoutMs = 0, uint64_t maxAgeMs = 0)` | `ArtifactRepositoryApi.cpp` | Repo archive；对应native consumer；Python extension独立编译 | A17–20/A22 |
| `RepoClient::fetchOriginalDataPacket(ndn_service_framework::ServiceUser& user, const ndn::Name& dataName, const std::string& expectedWireSha256, const std::vector<OriginalDataReplica>& replicas, const std::function<bool(const ndn::Data&)>& verifyProducerSignature, int timeoutMs)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O17 |
| `RepoClient::put(RepoNode& node, const std::string& objectName, const std::vector<uint8_t>& payload, StoreOptions options = {})` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01 |
| `RepoClient::get(const RepoNode& node, const std::string& objectName)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01 |
| `RepoClient::putRange(RepoNode& node, const RepoObjectManifest& manifest, RepoByteRange range, const std::vector<uint8_t>& bytes)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O02 |
| `RepoClient::commitRanges(RepoNode& node, const RepoObjectManifest& manifest)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O02 |
| `RepoClient::abortRanges(RepoNode& node, const std::string& objectName)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O03 |
| `RepoClient::getRange(const RepoNode& node, const std::string& objectName, RepoByteRange range)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O02 |
| `RepoClient::getManifest(const RepoNode& node, const std::string& objectName)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01 |
| `RepoClient::list(const RepoNode& node)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01 |
| `RepoClient::remove(RepoNode& node, const std::string& objectName)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O04–05 |
| `RepoClient::putDataPacket(RepoNode& node, const std::vector<uint8_t>& wire)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O08–09 |
| `RepoClient::getDataPacket(const RepoNode& node, const std::string& dataName)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O08–09 |
| `RepoClient::getDataPackets(const RepoNode& node, const RepoObjectManifest& manifest)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O08–09 |
| `RepoClient::insert(RepoNode& node, const RepoDataReference& reference)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O10–12 |
| `RepoClient::insertPayload(RepoNode& node, const std::string& objectName, const std::vector<uint8_t>& payload, ndn::KeyChain& keyChain, const ndn::security::SigningInfo& signingInfo, StoreOptions options = {}, size_t maxSegmentPayload = 6000)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O10–12 |
| `RepoClient::status(const RepoNode& node, const std::string& operationId)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O10–12 |
| `RepoClient::catalogStatus(const RepoNode& node)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O13–15/L03/N09 |
| `RepoClient::cacheStatus(const RepoNode& node)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O13/L03/N09 |
| `RepoClient::catalogSnapshot(const RepoNode& node)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O13–15/L03/N09 |
| `RepoClient::catalogDelta(const RepoNode& node, uint64_t sinceEpoch)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O13–15/L03/N09 |
| `RepoClient::catalogLookup(const RepoNode& node, const std::string& objectName)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O13–15/L03/N09 |
| `RepoClient::putSegmented(RepoNode& node, const std::string& objectName, const std::vector<uint8_t>& payload, StoreOptions options = {}, size_t maxSegmentPayload = 6000)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O06–07 |
| `RepoClient::getSegmented(const RepoNode& node, const RepoObjectManifest& manifest)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O06–07 |
| `RepoClient::getObject(const RepoNode& node, const RepoObjectManifest& manifest)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01/O06–07 |
| `RepoClient::localPut(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const std::string& objectName, const std::vector<uint8_t>& payload, StoreOptions options = {})` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L01 |
| `RepoClient::localGet(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const std::string& objectName)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L01 |
| `RepoClient::localPutRange(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const RepoObjectManifest& manifest, RepoByteRange range, const std::vector<uint8_t>& bytes)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L02 |
| `RepoClient::localCommitRanges(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const RepoObjectManifest& manifest)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L02 |
| `RepoClient::localAbortRanges(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const std::string& objectName)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L02 |
| `RepoClient::localGetRange(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const std::string& objectName, RepoByteRange range)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L02 |
| `RepoClient::localGetManifest(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const std::string& objectName)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L01 |
| `RepoClient::localList(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L01 |
| `RepoClient::localRemove(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const std::string& objectName)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L01 |
| `RepoClient::localInsert(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const RepoDataReference& reference)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L03 |
| `RepoClient::localStatus(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const std::string& operationId)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L03 |
| `RepoClient::localCatalogStatus(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L03 |
| `RepoClient::localCacheStatus(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L03 |
| `RepoClient::localCatalogSnapshot(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L03 |
| `RepoClient::localCatalogDelta(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, uint64_t sinceEpoch)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L03 |
| `RepoClient::localCatalogLookup(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const std::string& objectName)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L03 |
| `RepoClient::localPutSegmented(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const std::string& objectName, const std::vector<uint8_t>& payload, StoreOptions options = {}, size_t maxSegmentPayload = 6000)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L04 |
| `RepoClient::localGetSegmented(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const RepoObjectManifest& manifest)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L04 |
| `RepoClient::localGetObject(ndn_service_framework::LocalServiceRegistry& registry, const ndn::Name& repoServicePrefix, const RepoObjectManifest& manifest)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L04 |
| `RepoClient::makeManifest(std::string objectName, std::string objectType, const std::vector<uint8_t>& payload, uint32_t replicationFactor, std::vector<std::string> replicaNodes, std::string policyEpoch)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01/O06–07/N01（native value construction） |
| `RepoClient::makeRequest(const std::vector<uint8_t>& payload)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N01（native request construction） |
| `RepoClient::requestCapability(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N01/N08 |
| `RepoClient::requestCacheStatus(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N01/N08 |
| `RepoClient::requestStore(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, const RepoObjectManifest& manifest, const std::vector<uint8_t>& payload, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N02/N08/N10 |
| `RepoClient::requestStoreRange(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, const RepoObjectManifest& manifest, RepoByteRange range, const std::vector<uint8_t>& bytes, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N02/N08/N10 |
| `RepoClient::requestArtifactStoreRange(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, const ArtifactRangeWriteRequest& request, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N03/N08 |
| `RepoClient::requestArtifactStore(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, const ArtifactStoreRequest& request, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N03/N08 |
| `RepoClient::requestSelectedService(ndn_service_framework::ServiceUser& user, const ndn::Name& providerIdentity, const ndn::Name& serviceName, const std::vector<uint8_t>& payload, int ackCollectionTimeMs, int timeoutMs, std::shared_ptr<std::atomic<bool>> requestMayHaveBeenEmitted = {})` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N01–10/N08（对应operation/profile） |
| `RepoClient::requestCommitRanges(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, const RepoObjectManifest& manifest, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N02/N08/N10 |
| `RepoClient::requestArtifactCommitRanges(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, const ArtifactRangeCommitRequest& request, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N03/N08 |
| `RepoClient::requestAbortRanges(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, const std::string& objectName, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N02/N08/N10 |
| `RepoClient::requestInsert(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, const RepoDataReference& reference, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N06/N08/N10 |
| `RepoClient::requestFetch(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, const std::string& objectName, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N04/N08 |
| `RepoClient::requestFetchRange(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, const std::string& objectName, RepoByteRange range, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N04/N08 |
| `RepoClient::requestManifest(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, const std::string& objectName, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N04/N08 |
| `RepoClient::requestInventory(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N04/N08 |
| `RepoClient::requestDelete(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, const std::string& objectName, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N05/N08 |
| `RepoClient::requestStatus(ndn_service_framework::ServiceUser& user, const ndn::Name& repoServicePrefix, const std::string& operationId, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N05/N08 |
| `RepoClient::requestLocation(ndn_service_framework::ServiceUser& user, const ndn::Name& provider, const ndn::Name& repoServicePrefix, const ArtifactLocationRequest& request, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N07/N08 |
| `RepoClient::requestReceipt(ndn_service_framework::ServiceUser& user, const ndn::Name& provider, const ndn::Name& repoServicePrefix, const RepoReceiptLookupRequest& request, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N07/N08 |
| `RepoClient::requestAvailability(ndn_service_framework::ServiceUser& user, const ndn::Name& provider, const ndn::Name& repoServicePrefix, const RepoAvailabilityProbeRequest& request, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N07/N08 |
| `RepoClient::requestFetchRange(ndn_service_framework::ServiceUser& user, const ndn::Name& provider, const ndn::Name& repoServicePrefix, const std::string& objectName, RepoByteRange range, int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout, ndn_service_framework::ServiceUser::ResponseHandler onResponse)` | `RepoClient.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N04/N08 |
| `RepoNode::RepoNode(ndn::Name servicePrefix, StorageCapability capability, std::shared_ptr<RepoStoreBackend> store, std::string repoNode = {}, std::string providerIdentity = {}, std::string artifactMetadataPath = {}, ArtifactLimits artifactLimits = {}, uint64_t availabilityFreshnessMs = 5000)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L01–04/nativeownership |
| `RepoNode::~RepoNode()` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L01–04/nativeownership |
| `RepoNode::servicePrefix()` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L01–04/nativeownership |
| `RepoNode::core()` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L01–04/nativeownership |
| `RepoNode::core()` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L01–04/nativeownership |
| `RepoNode::registerLocalServices(ndn_service_framework::LocalServiceRegistry& registry)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | L01–04/nativeownership |
| `RepoNode::registerServices(ndn_service_framework::ServiceProvider& provider)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N01–10（注册集合）；A04 |
| `RepoNode::registerServices(ndn_service_framework::ServiceProvider& provider, std::shared_ptr<ndn_service_framework::ServiceUser> publisherUser, int ackCollectionTimeMs = 250, int requestTimeoutMs = 5000)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | N01–10（注册集合）；A04 |
| `RepoNode::registerOriginalDataServing(ndn::Face& face)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O17 |
| `RepoNode::put(const std::string& objectName, const std::vector<uint8_t>& payload, const std::string& objectType = "object", uint32_t replicationFactor = 1, const std::string& policyEpoch = "", std::vector<std::string> replicaNodes = {})` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01/N02（component与network分别证明） |
| `RepoNode::putManifest(const RepoObjectManifest& manifest)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01/N02–04/N10 |
| `RepoNode::get(const std::string& objectName)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01/N02–04/N10 |
| `RepoNode::putRange(const RepoObjectManifest& manifest, RepoByteRange range, const std::vector<uint8_t>& bytes)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O02–03/N02–04/N10 |
| `RepoNode::commitRanges(const RepoObjectManifest& manifest)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O02–03/N02–04/N10 |
| `RepoNode::abortRanges(const std::string& objectName)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O02–03/N02–04/N10 |
| `RepoNode::getRange(const std::string& objectName, RepoByteRange range)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O02–03/N02–04/N10 |
| `RepoNode::getManifest(const std::string& objectName)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01/N02–04/N10 |
| `RepoNode::list()` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01/N02–04/N10 |
| `RepoNode::remove(const std::string& objectName)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O04–05/N05/N10 |
| `RepoNode::putDataPacket(const std::string& dataName, const std::vector<uint8_t>& wire)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O08–09/O17 |
| `RepoNode::importLegacyDataPacketReceipt(const std::string& dataName)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O17 |
| `RepoNode::getDataPacket(const std::string& dataName)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O08–09/O17 |
| `RepoNode::hasDataPacket(const std::string& dataName)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O08–09/O17 |
| `RepoNode::setDataReferenceFetcher(DataReferenceFetcher fetcher)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O10–12/N06/N10 |
| `RepoNode::insertWirePackets(const RepoDataReference& reference, const std::vector<std::vector<uint8_t>>& wirePackets)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O08–09/O17 |
| `RepoNode::handleStore(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01/N02/N03/N10（matching profile） |
| `RepoNode::handleStoreRange(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O02–03/N02–04/N10 |
| `RepoNode::handleCommitRanges(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O02–03/N02–04/N10 |
| `RepoNode::handleAbortRanges(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O02–03/N02–04/N10 |
| `RepoNode::handleInsert(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O10–12/N06/N10 |
| `RepoNode::handleStoreManifest(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01/N02–04/N10 |
| `RepoNode::handleFetch(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01/N02–04/N10 |
| `RepoNode::handleFetchRange(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O02–03/N02–04/N10 |
| `RepoNode::handleManifest(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01/N02–04/N10 |
| `RepoNode::handleDataReceipt(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O17 |
| `RepoNode::handleDataLocation(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O17 |
| `RepoNode::handleInventory()` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O01/N02–04/N10 |
| `RepoNode::handleCapability()` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O13/N01 |
| `RepoNode::cacheStatus()` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O13/N01 |
| `RepoNode::handleCacheStatus()` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O13/N01 |
| `RepoNode::handleCatalogStatus()` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O13–15/N09 |
| `RepoNode::handleCatalogSnapshot()` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O13–15/N09 |
| `RepoNode::handleCatalogDelta(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O13–15/N09 |
| `RepoNode::handleCatalogLookup(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O13–15/N09 |
| `RepoNode::handleDelete(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O04–05/N05/N10 |
| `RepoNode::handleStatus(const std::vector<uint8_t>& request)` | `RepoNode.cpp` | Repo archive；对应native consumer；Python extension独立编译 | O12/N05/N07/A10–12/A16–22 |

## Build and Consumer Definition Map

- Repo `wscript`生产静态target `ndnsf-distributed-repo`：ArtifactRepositoryApi/ArtifactTypes/ArtifactManifest/ArtifactTransfer、RepoClient/Core/Node/Protocol/Types/StoreBackend、FilesystemArtifactStore/FilesystemRepoStoreBackend、LegacyRepoImporter/PersistentRepoServerConfig。输出 `build/NDNSF-DistributedRepo/libndnsf-distributed-repo.a`；actual位置以Waf task输出为准。
- Server `build/NDNSF-DistributedRepo/DistributedRepoServer`；explicit importer `build/NDNSF-DistributedRepo/ndnsf-repo-import`由LegacyRepoImport.cpp+上述archive定义；不得以Python server替代。
- Native consumers：`build/spec192-repo-client`、`spec193-object-repo-server`、`spec192-repo-inapp-api`、`spec192-repo-persistent-api`、`spec192-repo-persistent-service`、`spec192-repo-legacy-import-unit/integration`，source/registration在tests/wscript；final artifact SHA及loaded transitive libraries由T009重建封存。
- Python `py_repoclient._py_repoclient`：setup.py独立编译其Repo TU清单+_py_repoclient.cpp/artifact_repository_bindings.cpp，不链接Waf Repo archive；Core shared library另外加载。API/Node改变后必须重建extension。当前native API/Core/backend不引用LegacyRepoImporter，module无该未定义symbol；production import独立CLI，不为清单形式一致无理由添加TU。
- Core nativePython `pythonWrapper/src/ndnsf/_ndnsf.cpp`与service.py投影：ServiceUser/Provider/Core identity与grant由Core shared runtime；P01–02证明native lifetime/delegation。
- DI contracts/facades/repo_reference、repo_artifact_resolver.py：显式generic/native identity与冷暖授权；P03–04及nativeDI conformance。Python仅编排/绑定，不拥有Repo持久状态。
- LegacyRepoImporter::import+CLI对应P05/R07和native import suites；retired facade rejection对应P06。

## Dirty Overlay Ownership and Release Boundary

当前未提交Repo headers/native/SQLite/filesystem、thinbinding、Core signer/admission、DI adapter、example/experiment caller、unit/integration/Python/MiniNDN harness均沿既有owner保留；见baseline owner分类。build、dependencies、keys、models、pcap/results不属于待提交源码。历史Spec192 tasks/failure-log保持原证据，新的状态只更新202。

CodeGraph/源码复核已确认G001–003及实际调用链，source closure新增重点为Python独立TU；全affected suite与artifact重建属于T009而非本baseline文档PASS。公开local/object/request分层、OriginalData fetch入口、两个requestFetchRange overload与Node注册/handler均有inventory；进一步真实边界证明由T005–T008/T009/T010关闭。静态coverage风险不伪称已观察到产品故障。

DB002前置：调用者在fetch期间独占destination namespace，不支持跨API/process同时修改同目的路径；保留leaf symlink本身的namespace（不follow它），拒绝目录/其他特殊leaf，parent leaf symlink受既有O_NOFOLLOW directory barrier限制。owner原operation lock保留；不为未声明并发增加跨process锁。
DB003：native worker join/drain先于publisher User/Face/scheduler析构，fault fixture须bounded释放handler；count是worker attempt，emission用独立handler/pcap。
DB004：锁前/锁后verified replay都重新执行parent barrier，post-rename目标不删除，source inventory/identity/source unchanged继续验证。
Public模式：当前native路径显式BeginCollaborationWithProviders/CommitCollaborationPlan；旧Targeted compat按版本化reject/明确隔离，不虚构新native mode switch。
## 当前 API 用法与用例（2026-10-10）

核对对象：`Repo` 分支 `9fafa6ef` 加当前未提交源码。上方保留原始 baseline；本节按当前 header 和 Python wrapper 补充调用指南，不代表新的运行资格。三个主要 C++ 公开类逐声明共 126 项：ArtifactRepositoryApi 14、RepoClient 66、RepoNode 46（包含 Node 构造、析构及两个 core overload）。更正原清单把 `~RepoNode()` 写成无参构造的错误；当前没有公开无参 RepoNode 构造。

范围为 Repo 主要业务/客户端/节点入口，不把所有 backend、协议 codec、SQLite 内部操作或每个值类型的 validate/toJson/fromJson 算成额外业务 API。完整参数类型见上方逐声明清单及三个 `.hpp`；下面每个声明均有用途、调用示意和验收用例。用例 ID 指向 `validation-matrix.md` 的要求，不等于当前已通过。

### 先选调用层

| 调用层 | 适用场景 | 执行要求 |
| --- | --- | --- |
| ArtifactRepositoryApi In-App | 应用自己保存文件、重启后读取 | durableRoot 为应用本地持久目录；不自动发网络 STORE |
| ArtifactRepositoryApi Persistent | 显式选 Repo、发布文件、跨进程取回、收据与位置查询 | ServiceUser/Face 已初始化并授权；Face loop 在另一个线程运行 |
| RepoClient 直接 node 方法 | 对象、range、原始 signed Data 的组件测试 | 调用本进程 RepoNode；不构成网络证据 |
| RepoClient local 方法 | 同进程 LocalServiceRegistry 调用 | 先 registerLocalServices；不发 NDNSF 网络消息 |
| RepoClient request 方法 | MiniNDN 中低层协议请求 | ServiceUser 的 ACK→selection→response 路径；普通方法异步回调，requestSelectedService 阻塞返回 bytes |
| RepoNode 注册/handler | Repo 服务端与协议适配 | 注册时 Node 已由 shared_ptr 管理；直接调 handler 不证明网络认证 |

Persistent 调用不得阻塞自己的 Face 线程；ServiceUser 和 Face 必须活得比 API 长。关闭时先停并 join 事件循环，再在 ServiceUser 销毁前销毁 API。服务端 worker 也要先停止/回收。Persistent artifact 服务会明确拒绝传统对象 STORE/range/INSERT/STORE_MANIFEST/DELETE（N10）；下表保留的传统对象接口不意味着可以用它们写入 Persistent artifact 服务。

### 示例变量约定

以下表格是 C++ 调用表达式示例，省略已有环境的创建和错误处理，不是彼此独立可运行的完整程序。`api` 为对应模式实例，`persistent`/`inApp` 为两种实例；`node` 为现有 RepoNode 引用；`reg` 为已注册的 LocalServiceRegistry；`user`/`provider`/`face` 为已初始化的框架对象；`publisherUser` 为 shared_ptr<ServiceUser>。

`prefix` 为 ndn::Name("/NDNSF/DistributedRepo/Object/v1")；`providerId` 为本地 MiniNDN 内认证的 Provider identity。`name` 为 "/demo/object"；`bytes` 为 vector<uint8_t>；`m` 为匹配 bytes 的 RepoObjectManifest；`r` 为合法 RepoByteRange；`part` 为该区间精确字节；`opts` 为 StoreOptions。`ref` 为源文件的真实 SHA-256/长度等完整 ArtifactReference；`h` 为成功发布返回的 ArtifactHandle；`selected` 为显式选择且含有效 seedRepoId 的 SelectedRepoSet。`op` 为发布操作 handle，`opId` 为操作 ID，`receiptId` 为真实收据 ID。

`wire`/`wires` 为已签名 Data 的完整编码；`dataName` 为 wire 内完整 Data name；`dataRef` 为匹配的 RepoDataReference；`verifyProducer` 为实际执行 producer trust 验证的函数。`aw`/`as`/`ac` 分别为合法 ArtifactRangeWriteRequest/ArtifactStoreRequest/ArtifactRangeCommitRequest；`locationReq`/`receiptReq`/`availabilityReq` 为对应合法协议值；`encoded` 是与该 handler 匹配的 RepoProtocol 编码，不能用任意 bytes 代替。`onTimeout`/`onResponse` 使用 ServiceUser 的真实回调类型；`timeout` 为正毫秒，`ackMs` 小于 request timeout。所有身份名称只指本机 MiniNDN fixtures。

### ArtifactRepositoryApi：14 项

| API | 用途与调用示例 | 用例 |
| --- | --- | --- |
| makeInApp | 创建本地 owner：`auto inApp = ArtifactRepositoryApi::makeInApp(config);` | A01–04 |
| makePersistent | 借用网络 user：`auto persistent = ArtifactRepositoryApi::makePersistent(user, config);` | A04–05 |
| publishFile(path, reference) | In-App 提交：`auto result = inApp.publishFile("input.bin", ref);` | A01–03/A14 |
| publishFile(path, reference, selected, timeout) | Persistent 前台发布：`auto result = persistent.publishFile("input.bin", ref, selected, timeout);` | A06–09/A14 |
| beginPublishFile | seed 提交屏障后返回操作 handle：`auto op = persistent.beginPublishFile("input.bin", ref, selected, "upload-001", timeout);` | A10–12 |
| beginArchive | 异步归档本地已提交 artifact：`auto op = persistent.beginArchive(inApp, h, selected, "archive-001", timeout);` | A13 |
| getPublishStatus | 查询操作进度/终态：`auto status = persistent.getPublishStatus(op, timeout);` | A10–12 |
| discoverRepoCandidates | 发现满足能力要求的候选，不自动选中或写入：`auto candidates = persistent.discoverRepoCandidates(requirements, timeout);` | A05/A09 |
| archive | 前台显式归档：`auto result = persistent.archive(inApp, h, selected, timeout);` | A13 |
| fetchFile(reference, destination) | 从 In-App 本地 store 取回：`inApp.fetchFile(ref, "output.bin");` | A02–03/R03–05 |
| fetchFile(handle, destination, timeout) | 按已提交 handle 取回并报告各尝试：`auto result = persistent.fetchFile(h, "output.bin", timeout);` | A15–22/R01–05 |
| lookupLocalCommit | 查本地提交记录：`auto local = inApp.lookupLocalCommit(ref);`；检查 optional | A01–03 |
| getReceipt | 查询历史提交凭据：`auto receipt = persistent.getReceipt(receiptId, h, timeout);` | A16 |
| checkAvailability | 查当前可用性：`auto report = persistent.checkAvailability(h, "verify_content", timeout, 0);`；metadata 仅元数据，verify_content 校验内容 | A17–20/A22 |

ArtifactRepositoryApi 另有析构、move constructor、move assignment；禁止 copy constructor/copy assignment。这些是 C++ 对象生命周期规则，未计入上述 14 个业务声明。构造一个 In-App owner 后以 `auto moved = std::move(inApp);` 转移所有权；不要继续调用 moved-from 实例。

### RepoClient：直接对象/原始 Data 接口 25 项

| API | 用途与调用示例 | 用例 |
| --- | --- | --- |
| fetchOriginalDataPacket | 先查 receipt/current location，再发带副本 hint 的原名 Interest：`RepoClient::fetchOriginalDataPacket(user, dataName, wireSha256, replicas, verifyProducer, timeout);`；返回原始 wire | O17 |
| put | 整对象写入：`auto m = RepoClient::put(node, name, bytes, opts);` | O01 |
| get | 整对象读取：`auto bytes = RepoClient::get(node, name);` | O01 |
| putRange | 暂存一段：`RepoClient::putRange(node, m, r, part);` | O02 |
| commitRanges | 完整区间提交：`RepoClient::commitRanges(node, m);` | O02–03 |
| abortRanges | 取消暂存：`RepoClient::abortRanges(node, name);` | O03 |
| getRange | 读字节窗口：`RepoClient::getRange(node, name, r);` | O02 |
| getManifest | 查询对象清单：`RepoClient::getManifest(node, name);` | O01 |
| list | 枚举对象：`RepoClient::list(node);` | O01/O16 |
| remove | 删除对象：`bool removed = RepoClient::remove(node, name);` | O04–05 |
| putDataPacket | 从 signed wire 取完整 Data name 后存原包：`RepoClient::putDataPacket(node, wire);` | O08–09 |
| getDataPacket | 取完整原包：`RepoClient::getDataPacket(node, dataName);` | O08–09 |
| getDataPackets | 按 manifest packetNames 顺序取完整包集：`RepoClient::getDataPackets(node, m);`；缺包整体失败 | O08–09 |
| insert | 使用引用 fetcher 导入已发布 Data：`auto status = RepoClient::insert(node, dataRef);` | O10–12 |
| insertPayload | 先签名分段，再 INSERT：`RepoClient::insertPayload(node, name, bytes, keyChain, signingInfo, opts, 6000);` | O10–12 |
| status | 查 INSERT 等对象操作：`RepoClient::status(node, opId);` | O10–12 |
| catalogStatus | 查目录 epoch/摘要：`RepoClient::catalogStatus(node);` | O13–15 |
| cacheStatus | 查 backend cache：`RepoClient::cacheStatus(node);`；不等于 NFD CS 命中证据 | O13 |
| catalogSnapshot | 取全量目录：`RepoClient::catalogSnapshot(node);` | O14 |
| catalogDelta | 取 epoch 之后增删：`RepoClient::catalogDelta(node, previousEpoch);` | O14–15 |
| catalogLookup | 查单对象目录项：`RepoClient::catalogLookup(node, name);` | O14–15 |
| putSegmented | 传统 opaque-byte 对象级分段：`auto m = RepoClient::putSegmented(node, name, bytes, opts, 6000);` | O06–07 |
| getSegmented | 重组并在调用侧校验大小/hash：`RepoClient::getSegmented(node, m);` | O06–07 |
| getObject | 根据 manifest 自动读整对象或分段：`RepoClient::getObject(node, m);` | O01/O06–07 |
| makeManifest | 从 payload 建对象元数据：`auto m = RepoClient::makeManifest(name, "object", bytes, 1, {}, "epoch-1");` | O01/O06–07/N01 |

### RepoClient：LocalServiceRegistry 接口 19 项

所有调用均经本进程 registry。先执行 `node.registerLocalServices(reg)`，再调用；表中保留 local 前缀以避免把它误当网络接口。

| API | 用途与调用示例 | 用例 |
| --- | --- | --- |
| localPut | 本地整对象写：`RepoClient::localPut(reg, prefix, name, bytes, opts);` | L01 |
| localGet | 本地整对象读：`RepoClient::localGet(reg, prefix, name);` | L01 |
| localPutRange | 本地区间暂存：`RepoClient::localPutRange(reg, prefix, m, r, part);` | L02 |
| localCommitRanges | 本地区间提交：`RepoClient::localCommitRanges(reg, prefix, m);` | L02 |
| localAbortRanges | 本地取消暂存：`RepoClient::localAbortRanges(reg, prefix, name);` | L02 |
| localGetRange | 本地窗口读：`RepoClient::localGetRange(reg, prefix, name, r);` | L02 |
| localGetManifest | 本地清单读：`RepoClient::localGetManifest(reg, prefix, name);` | L01 |
| localList | 本地枚举：`RepoClient::localList(reg, prefix);` | L01 |
| localRemove | 本地删除：`RepoClient::localRemove(reg, prefix, name);` | L01 |
| localInsert | 本地按引用导入 Data：`RepoClient::localInsert(reg, prefix, dataRef);` | L03 |
| localStatus | 本地操作状态：`RepoClient::localStatus(reg, prefix, opId);` | L03 |
| localCatalogStatus | 本地目录状态：`RepoClient::localCatalogStatus(reg, prefix);` | L03 |
| localCacheStatus | 本地 backend cache 状态：`RepoClient::localCacheStatus(reg, prefix);` | L03 |
| localCatalogSnapshot | 本地全量目录：`RepoClient::localCatalogSnapshot(reg, prefix);` | L03 |
| localCatalogDelta | 本地目录增量：`RepoClient::localCatalogDelta(reg, prefix, previousEpoch);` | L03 |
| localCatalogLookup | 本地查目录项：`RepoClient::localCatalogLookup(reg, prefix, name);` | L03 |
| localPutSegmented | 本地 opaque-byte 分段写：`RepoClient::localPutSegmented(reg, prefix, name, bytes, opts, 6000);` | L04 |
| localGetSegmented | 本地分段重组校验：`RepoClient::localGetSegmented(reg, prefix, m);` | L04 |
| localGetObject | 本地整对象/分段统一读：`RepoClient::localGetObject(reg, prefix, m);` | L04 |

### RepoClient：请求/消息接口 22 项

普通 request 方法返回请求 ID，成功数据通过 onResponse 交付；返回 ID 不等于请求成功。必须解码并检查协议状态、签名和内容。requestSelectedService 是阻塞 helper；不能从它所依赖的 Face 线程调用。这里的网络实验限定本机 MiniNDN。

| API | 用途与调用示例 | 用例 |
| --- | --- | --- |
| makeRequest | 封装已有协议 payload，不发送：`auto request = RepoClient::makeRequest(encoded);` | N01 |
| requestCapability | 查询能力：`RepoClient::requestCapability(user, prefix, timeout, onTimeout, onResponse);` | N01/N08 |
| requestCacheStatus | 查询 backend cache：`RepoClient::requestCacheStatus(user, prefix, timeout, onTimeout, onResponse);` | N01/N08 |
| requestStore | 传统对象整写请求：`RepoClient::requestStore(user, prefix, m, bytes, timeout, onTimeout, onResponse);` | N02/N08/N10 |
| requestStoreRange | 传统对象窗口写：`RepoClient::requestStoreRange(user, prefix, m, r, part, timeout, onTimeout, onResponse);` | N02/N08/N10 |
| requestArtifactStoreRange | artifact-v2 窗口写：`RepoClient::requestArtifactStoreRange(user, prefix, aw, timeout, onTimeout, onResponse);` | N03/N08 |
| requestArtifactStore | artifact-v2 单次写：`RepoClient::requestArtifactStore(user, prefix, as, timeout, onTimeout, onResponse);` | N03/N08 |
| requestSelectedService | 限定一个认证 Provider 的完整请求：`auto reply = RepoClient::requestSelectedService(user, providerId, serviceName, encoded, ackMs, timeout);` | N01–10；目录查询 N09 |
| requestCommitRanges | 传统对象区间提交：`RepoClient::requestCommitRanges(user, prefix, m, timeout, onTimeout, onResponse);` | N02/N08/N10 |
| requestArtifactCommitRanges | artifact receipt transaction 提交：`RepoClient::requestArtifactCommitRanges(user, prefix, ac, timeout, onTimeout, onResponse);` | N03/N08 |
| requestAbortRanges | 取消区间暂存：`RepoClient::requestAbortRanges(user, prefix, name, timeout, onTimeout, onResponse);` | N02/N08/N10 |
| requestInsert | 导入引用 Data：`RepoClient::requestInsert(user, prefix, dataRef, timeout, onTimeout, onResponse);` | N06/N08/N10 |
| requestFetch | 读取对象：`RepoClient::requestFetch(user, prefix, name, timeout, onTimeout, onResponse);` | N04/N08 |
| requestFetchRange（无 provider 参数） | 一般选择流程的窗口读：`RepoClient::requestFetchRange(user, prefix, name, r, timeout, onTimeout, onResponse);` | N04/N08 |
| requestManifest | 读取清单：`RepoClient::requestManifest(user, prefix, name, timeout, onTimeout, onResponse);` | N04/N08 |
| requestInventory | 读取库存：`RepoClient::requestInventory(user, prefix, timeout, onTimeout, onResponse);` | N04/N08 |
| requestDelete | 传统对象删除请求：`RepoClient::requestDelete(user, prefix, name, timeout, onTimeout, onResponse);` | N05/N08/N10 |
| requestStatus | 查询协议操作状态：`RepoClient::requestStatus(user, prefix, opId, timeout, onTimeout, onResponse);` | N05/N08 |
| requestLocation | 向指定身份查当前位置：`RepoClient::requestLocation(user, providerId, prefix, locationReq, timeout, onTimeout, onResponse);` | N07/N08 |
| requestReceipt | 向指定身份查历史收据：`RepoClient::requestReceipt(user, providerId, prefix, receiptReq, timeout, onTimeout, onResponse);` | N07/N08 |
| requestAvailability | 向指定身份查当前可用性：`RepoClient::requestAvailability(user, providerId, prefix, availabilityReq, timeout, onTimeout, onResponse);` | N07/N08 |
| requestFetchRange（含 provider 参数） | 指定副本窗口读：`RepoClient::requestFetchRange(user, providerId, prefix, name, r, timeout, onTimeout, onResponse);` | N04/N08 |

### RepoNode：46 项

| API | 用途与调用示例 | 用例 |
| --- | --- | --- |
| RepoNode 构造 | 服务端节点：`auto owner = std::make_shared<RepoNode>(prefix, capability, store, repoId, providerUri, metadataPath);` | L01–04/owner |
| ~RepoNode | 停 worker/reaper、释放节点：最后一个 shared_ptr 被释放时自动调用；须先满足 runtime 生命周期要求 | L01–04/owner |
| servicePrefix | 取注册根：`node.servicePrefix();` | L01–04 |
| core() const | 只读 Core：`std::as_const(node).core();` | native owner |
| core() | 可变 Core：`node.core();`；绕过 Node transaction mutex，调用者须串行化 mutation | native owner |
| registerLocalServices | 注册同进程服务：`node.registerLocalServices(reg);` | L01–04 |
| registerServices(provider) | 注册网络 handlers：`owner->registerServices(provider);` | N01–10/A04 |
| registerServices(provider, publisherUser, ackMs, timeout) | 注册且配置 publish worker：`owner->registerServices(provider, publisherUser, ackMs, timeout);` | N01–10/A10–12 |
| registerOriginalDataServing | 显式原名 Data 服务：`owner->registerOriginalDataServing(face);`；Face 比 Node 长寿 | O17 |
| put | 节点整对象写：`node.put(name, bytes, "object", 1, "epoch-1", {});` | O01/N02 |
| putManifest | 保存 manifest-only 对象：`node.putManifest(m);` | O01/O06/N02–04 |
| get | 节点整对象读：`node.get(name);` | O01/N04 |
| putRange | 节点区间暂存：`node.putRange(m, r, part);` | O02–03/N02 |
| commitRanges | 节点区间提交：`node.commitRanges(m);` | O02–03/N02 |
| abortRanges | 节点取消暂存：`node.abortRanges(name);` | O03/N02 |
| getRange | 节点窗口读：`node.getRange(name, r);` | O02/N04 |
| getManifest | 节点清单读：`node.getManifest(name);` | O01/N04 |
| list | 节点枚举：`node.list();` | O01/O16/N04 |
| remove | 节点删除：`node.remove(name);` | O04–05/N05 |
| putDataPacket | 按明确 Data name 保存完整 wire：`node.putDataPacket(dataName, wire);`；name 必须匹配 wire | O08–09/O17 |
| importLegacyDataPacketReceipt | 为 legacy Data 显式建立持久 receipt：`node.importLegacyDataPacketReceipt(dataName);` | O17 |
| getDataPacket | 原包读取：`node.getDataPacket(dataName);` | O08–09/O17 |
| hasDataPacket | 原包存在性：`node.hasDataPacket(dataName);` | O08–09/O17 |
| setDataReferenceFetcher | 注入 Data 引用 fetcher：`node.setDataReferenceFetcher(fetcher);`；fetcher 返回完整 wire 集合 | O10–12/N06 |
| insertWirePackets | 导入匹配引用的包集：`node.insertWirePackets(dataRef, wires);` | O08–12/O17 |
| handleStore | 解析 STORE：`node.handleStore(encoded);`；协议组件调用 | O01/N02–03/N10 |
| handleStoreRange | 解析 STORE_RANGE：`node.handleStoreRange(encoded);` | O02–03/N02–03/N10 |
| handleCommitRanges | 解析 COMMIT_RANGES：`node.handleCommitRanges(encoded);` | O02–03/N02–03/N10 |
| handleAbortRanges | 解析 ABORT_RANGES：`node.handleAbortRanges(encoded);` | O03/N02/N10 |
| handleInsert | 解析 INSERT：`node.handleInsert(encoded);` | O10–12/N06/N10 |
| handleStoreManifest | 解析 STORE_MANIFEST：`node.handleStoreManifest(encoded);` | O01/N02–04/N10 |
| handleFetch | 解析 FETCH：`node.handleFetch(encoded);` | O01/N04 |
| handleFetchRange | 解析 FETCH_RANGE：`node.handleFetchRange(encoded);` | O02/N04 |
| handleManifest | 解析 MANIFEST：`node.handleManifest(encoded);` | O01/N04 |
| handleDataReceipt | 原包持久 receipt 查询：`node.handleDataReceipt(encoded);` | O17 |
| handleDataLocation | 原包当前 location 查询：`node.handleDataLocation(encoded);` | O17 |
| handleInventory | 编码库存响应：`node.handleInventory();` | O01/O16/N04 |
| handleCapability | 编码 capability 响应：`node.handleCapability();` | O13/N01 |
| cacheStatus | 返回 backend cache 值类型：`node.cacheStatus();` | O13/N01 |
| handleCacheStatus | 编码 cache 响应：`node.handleCacheStatus();` | O13/N01 |
| handleCatalogStatus | 编码目录状态：`node.handleCatalogStatus();` | O14–15/N09 |
| handleCatalogSnapshot | 编码全量目录：`node.handleCatalogSnapshot();` | O14/N09 |
| handleCatalogDelta | 编码指定 epoch 后增量：`node.handleCatalogDelta(encoded);` | O14–15/N09 |
| handleCatalogLookup | 编码对象目录项：`node.handleCatalogLookup(encoded);` | O14–15/N09 |
| handleDelete | 解析 DELETE：`node.handleDelete(encoded);` | O04–05/N05/N10 |
| handleStatus | 编码操作状态：`node.handleStatus(encoded);` | O12/N05/N07/A10–12 |

### Python 投影与可用示例

Python 业务入口是 native ArtifactRepositoryApi 的 snake_case 映射，不是拥有另一套存储状态的实现。

| C++ | Python | 用法 |
| --- | --- | --- |
| makeInApp | make_in_app | `ArtifactRepositoryApi.make_in_app(durable_root=store_dir, owner_id=publisher)` |
| makePersistent | make_persistent | `ArtifactRepositoryApi.make_persistent(user, config)` |
| publishFile（两 overload） | publish_file | In-App `(path, ref)`；Persistent `(path, ref, selected, timeout_ms)` |
| beginPublishFile | begin_publish_file | `(path, ref, selected, idempotency_key, timeout_ms)` |
| beginArchive | begin_archive | `(in_app, handle, selected, idempotency_key, timeout_ms)` |
| getPublishStatus | get_publish_status | `(operation_handle, timeout_ms)` |
| discoverRepoCandidates | discover_repo_candidates | `(requirements, timeout_ms)` |
| archive | archive | `(in_app, handle, selected, timeout_ms)` |
| fetchFile（两 overload） | fetch_file | In-App `(reference, destination)`；Persistent `(handle, destination, timeout_ms)` |
| lookupLocalCommit | lookup_local_commit | In-App `(reference)` |
| getReceipt | get_receipt | `(receipt_id, handle, timeout_ms)` |
| checkAvailability | check_availability | `(handle, "metadata", timeout_ms, max_age_ms)` |

`py_repoclient.RepoClient(user, ...)` 是 Persistent 兼容 facade，另外公开 `artifact_api` 属性。publish_file 必须提供 selected；fetch_file 必须提供 ArtifactHandle。configure_artifact_backend、make_manifest 以及旧 get/store/insert/delete/list/inventory、旧异步方法等入口明确抛版本化兼容错误，不算可工作的旧 Python 对象 API（P06）。不要根据 C++ RepoClient 的 66 项推断 Python facade 也全部支持。

下面 In-App 示例沿用仓库 `examples/python/NDNSF-DistributedRepo/artifact_api/publish_file.py` 的实际构造方式。需要已构建且可导入的 py_repoclient native extension；示例未在本次清单整理中执行。

```python
import hashlib
from pathlib import Path
from py_repoclient import ArtifactReference, ArtifactRepositoryApi

source = Path("input.bin")  # 已存在的应用文件
publisher = "/demo/publisher"
digest = hashlib.sha256(source.read_bytes()).hexdigest()
ref = ArtifactReference(
    "/demo/input", digest, source.stat().st_size,
    f"{publisher}/NDNSF/ARTIFACT/{digest}/root",
    publisher, "epoch-1",
)
api = ArtifactRepositoryApi.make_in_app(
    durable_root=Path("./demo-repo"), owner_id=publisher,
)
result = api.publish_file(source, ref)
api.fetch_file(result.reference, Path("output.bin"))
assert hashlib.sha256(Path("output.bin").read_bytes()).hexdigest() == digest
```

Persistent C++ 操作顺序如下；前置 user、Face loop、证书/授权、selected target 均由本机 MiniNDN fixture 提供。发现候选后应用按需求显式构造 selected；不能把 discovery 等同于提交。

```cpp
namespace repo = ndnsf_distributed_repo;
repo::PersistentRepoClientConfig cfg;
cfg.servicePrefix = prefix;
cfg.ackCollectionTimeMs = ackMs;
cfg.requestTimeoutMs = timeout;
cfg.operationWaitTimeoutMs = timeout;
auto api = repo::ArtifactRepositoryApi::makePersistent(user, cfg);
auto candidates = api.discoverRepoCandidates(requirements, timeout);
// selected 由应用从认证候选中明确选择，含有效 seedRepoId。
auto published = api.publishFile("input.bin", ref, selected, timeout);
if (!published.handle || !published.receipt) {
  throw std::runtime_error("publication has no committed handle/receipt");
}
auto receipt = api.getReceipt(published.receipt->receiptId,
                              *published.handle, timeout);
auto report = api.checkAvailability(*published.handle, "verify_content",
                                    timeout, 0);
auto fetched = api.fetchFile(*published.handle, "output.bin", timeout);
// 验收仍独立核对 receipt 状态、required/achieved、bytesVerified 和目的 digest。
```

### 典型用例与正确验收

| 场景 | API 顺序 | 需要证明 |
| --- | --- | --- |
| 应用本地持久化 | makeInApp→publishFile→lookupLocalCommit→新进程 makeInApp→fetchFile | 重启后同摘要/完整 bytes，零网络 STORE；A01–03 |
| 一/三副本发布 | makePersistent→discoverRepoCandidates→明确 selected→publishFile | 仅选定身份写入；ALL/ONE/QUORUM required/achieved 和每节点 receipt；A05–09 |
| 异步发布与恢复 | beginPublishFile→getPublishStatus；重启后同幂等键恢复 | seed barrier、同 operation ID、唯一持久 receipt、终态不回滚；A10–12 |
| 显式归档 | In-App publishFile→Persistent archive/beginArchive | 归档前无远端写入，归档后新 Persistent handle，历史 In-App receipt 不合并；A13 |
| 新消费者取回/故障切换 | fetchFile(handle)→检查 attempts/digest | 原文件已删除仍可跨进程读取；离线、损坏、别名去重/cooldown；A15/A19–22 |
| 查询历史与当前状态 | getReceipt→checkAvailability | 旧 receipt 不变；当前 metadata/content 可用性可变化；A16–20/A22 |
| 本地区间事务 | localPutRange 多次→localCommitRanges 或 localAbortRanges | 有空洞/越界拒绝，无半提交，abort 释放暂存；L02/O02–03 |
| 原始 signed Data | putDataPacket/insertPayload→fetchOriginalDataPacket | 原名 Interest、非空 hint、原始 wire 不变、producer 签名验证；O08–12/O17 |
| 目录同步 | catalogSnapshot→更新/删除→catalogDelta→catalogLookup | epoch 增长、tombstone、删除对象不再可见；O14–15/N09 |
| Python/C++ 互通与 DI | native publish→Python get_receipt/fetch_file，反方向及 DI resolver | 同 reference/receipt/digest；未授权及 warm-cache 不交付内容；P01–04 |

当前 T009/T010 尚未关闭，恢复发布测试仍有 late ACK 红测；完整 sanitizer 与统一 candidate 的两轮 MiniNDN 资格未完成。上述清单、调用示例和 case 映射不构成“所有 API 无 bug”的证明。下一步保持现有调试 checkpoint，完成 ACK 延迟定位，再按 Spec202 对冻结候选执行全量验收。
