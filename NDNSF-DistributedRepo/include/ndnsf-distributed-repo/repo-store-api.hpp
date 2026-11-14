#ifndef NDNSF_DISTRIBUTED_REPO_REPO_STORE_API_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_STORE_API_HPP

#include "ndnsf-distributed-repo/RepoClient.hpp"

namespace ndnsf_distributed_repo {

/** Store-side helpers that used to hang off the client class. They take a RepoNode,
 *  so they are server API, not client operations; this header is not installed. */
class RepoStoreApi
{
public:
  static RepoCatalogEntry catalogLookup(const RepoNode& node,
                                        const std::string& objectName);
  static RepoCatalogStatus catalogStatus(const RepoNode& node);
  static RepoCatalogDelta catalogSnapshot(const RepoNode& node);
  static RepoCatalogDelta catalogDelta(const RepoNode& node, uint64_t sinceEpoch);
  static std::vector<uint8_t> getSegmented(const RepoNode& node,
                                           const RepoObjectManifest& manifest);
  static RepoObjectManifest commitRanges(RepoNode& node,
                                         const RepoObjectManifest& manifest);
  static void abortRanges(RepoNode& node, const std::string& objectName);
  static RepoOperationStatus insert(
    RepoNode& node,
    const RepoDataReference& reference);
  static RepoObjectManifest putSegmented(
    RepoNode& node,
    const std::string& objectName,
    const std::vector<uint8_t>& payload,
    StoreOptions options = {},
    size_t maxSegmentPayload = 6000);
  static std::vector<uint8_t> getDataPacket(const RepoNode& node,
                                            const std::string& dataName);
  static RepoOperationStatus status(const RepoNode& node,
                                    const std::string& operationId);
  static RepoCacheStatus cacheStatus(const RepoNode& node);
  static RepoObjectManifest put(RepoNode& node,
                                const std::string& objectName,
                                const std::vector<uint8_t>& payload,
                                StoreOptions options = {});
  static std::vector<RepoObjectManifest> list(const RepoNode& node);
  static std::vector<uint8_t> get(const RepoNode& node,
                                  const std::string& objectName);
  static RepoObjectManifest getManifest(const RepoNode& node,
                                        const std::string& objectName);
  static bool remove(RepoNode& node,
                     const std::string& objectName);
  static std::vector<std::vector<uint8_t>> getDataPackets(
    const RepoNode& node,
    const RepoObjectManifest& manifest);
  static void putRange(RepoNode& node,
                       const RepoObjectManifest& manifest,
                       RepoByteRange range,
                       const std::vector<uint8_t>& bytes);
  static std::vector<uint8_t> getRange(const RepoNode& node,
                                       const std::string& objectName,
                                       RepoByteRange range);
  static RepoOperationStatus insertPayload(
    RepoNode& node,
    const std::string& objectName,
    const std::vector<uint8_t>& payload,
    ndn::KeyChain& keyChain,
    const ndn::security::SigningInfo& signingInfo,
    StoreOptions options = {},
    size_t maxSegmentPayload = 6000);
  static RepoObjectManifest putDataPacket(RepoNode& node,
                                          const std::vector<uint8_t>& wire);
  static std::vector<uint8_t> getObject(
    const RepoNode& node,
    const RepoObjectManifest& manifest);
};

} // namespace ndnsf_distributed_repo

#endif
