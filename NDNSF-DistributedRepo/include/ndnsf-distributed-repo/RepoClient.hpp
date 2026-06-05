#ifndef NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_HPP

#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-info.hpp>

namespace ndn_service_framework {
class LocalServiceRegistry;
}

namespace ndnsf_distributed_repo {

class RepoNode;

struct StoreOptions
{
  std::string objectType = "object";
  uint32_t replicationFactor = 1;
  std::vector<std::string> replicaNodes;
  std::string policyEpoch;
};

class RepoClient
{
public:
  static constexpr const char* DEFAULT_SERVICE_NAME = "/NDNSF/DistributedRepo";

  static RepoObjectManifest put(RepoNode& node,
                                const std::string& objectName,
                                const std::vector<uint8_t>& payload,
                                StoreOptions options = {});

  static std::vector<uint8_t> get(const RepoNode& node,
                                  const std::string& objectName);

  static RepoObjectManifest getManifest(const RepoNode& node,
                                        const std::string& objectName);

  static std::vector<RepoObjectManifest> list(const RepoNode& node);

  static bool remove(RepoNode& node,
                     const std::string& objectName);

  static RepoOperationStatus insert(
    RepoNode& node,
    const RepoDataReference& reference);

  /**
   * Convenience INSERT adapter for callers that have payload bytes instead of
   * pre-published Data. The adapter segments and signs the payload under
   * objectName using ndn-cxx Segmenter, then stores the resulting Data wire
   * packets through the same opaque segment path used by insert().
   */
  static RepoOperationStatus insertPayload(
    RepoNode& node,
    const std::string& objectName,
    const std::vector<uint8_t>& payload,
    ndn::KeyChain& keyChain,
    const ndn::security::SigningInfo& signingInfo,
    StoreOptions options = {},
    size_t maxSegmentPayload = 6000);

  static RepoOperationStatus status(const RepoNode& node,
                                    const std::string& operationId);

  static RepoCatalogStatus catalogStatus(const RepoNode& node);

  static RepoCatalogDelta catalogSnapshot(const RepoNode& node);

  static RepoCatalogDelta catalogDelta(const RepoNode& node, uint64_t sinceEpoch);

  static RepoCatalogEntry catalogLookup(const RepoNode& node,
                                        const std::string& objectName);

  /**
   * Store a large object as object-level chunks named
   * <objectName>/seg/<index>, then store a manifest-only parent object.
   *
   * Repo storage is opaque: the repo does not perform APP trust, signature, or
   * hash verification while storing. Use getSegmented() on the APP side to
   * reassemble and verify size/hash against the returned manifest.
   */
  static RepoObjectManifest putSegmented(
    RepoNode& node,
    const std::string& objectName,
    const std::vector<uint8_t>& payload,
    StoreOptions options = {},
    size_t maxSegmentPayload = 6000);

  /**
   * Fetch chunks described by manifest, reassemble them, and verify APP-side
   * size/hash metadata. Throws std::runtime_error on mismatch.
   */
  static std::vector<uint8_t> getSegmented(
    const RepoNode& node,
    const RepoObjectManifest& manifest);

  /**
   * Fetch one logical repo object described by manifest. This is the preferred
   * object-level API for callers that do not care whether the object was stored
   * as one payload or as object-level chunks.
   */
  static std::vector<uint8_t> getObject(
    const RepoNode& node,
    const RepoObjectManifest& manifest);

  static RepoObjectManifest localPut(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName,
    const std::vector<uint8_t>& payload,
    StoreOptions options = {});

  static std::vector<uint8_t> localGet(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName);

  static RepoObjectManifest localGetManifest(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName);

  static std::vector<RepoObjectManifest> localList(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix);

  static bool localRemove(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName);

  static RepoOperationStatus localInsert(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const RepoDataReference& reference);

  static RepoOperationStatus localStatus(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& operationId);

  static RepoCatalogStatus localCatalogStatus(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix);

  static RepoCatalogDelta localCatalogSnapshot(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix);

  static RepoCatalogDelta localCatalogDelta(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    uint64_t sinceEpoch);

  static RepoCatalogEntry localCatalogLookup(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName);

  /**
   * Same as putSegmented(), but invokes a repo registered in the same trusted
   * LocalServiceRegistry instead of using NDNSF network messages.
   */
  static RepoObjectManifest localPutSegmented(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName,
    const std::vector<uint8_t>& payload,
    StoreOptions options = {},
    size_t maxSegmentPayload = 6000);

  /**
   * Same as getSegmented(), but fetches chunks from the local registry.
   */
  static std::vector<uint8_t> localGetSegmented(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const RepoObjectManifest& manifest);

  /**
   * Local trusted equivalent of getObject().
   */
  static std::vector<uint8_t> localGetObject(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const RepoObjectManifest& manifest);

  static RepoObjectManifest makeManifest(std::string objectName,
                                         std::string objectType,
                                         const std::vector<uint8_t>& payload,
                                         uint32_t replicationFactor,
                                         std::vector<std::string> replicaNodes,
                                         std::string policyEpoch);

  static ndn_service_framework::RequestMessage makeRequest(
    const std::vector<uint8_t>& payload);

  static ndn::Name requestCapability(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);

  static ndn::Name requestStore(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const RepoObjectManifest& manifest,
    const std::vector<uint8_t>& payload,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);

  static ndn::Name requestInsert(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const RepoDataReference& reference,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);

  static ndn::Name requestFetch(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);

  static ndn::Name requestManifest(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);

  static ndn::Name requestInventory(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);

  static ndn::Name requestDelete(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);

  static ndn::Name requestStatus(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const std::string& operationId,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_HPP
