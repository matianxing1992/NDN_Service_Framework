#ifndef NDNSF_DISTRIBUTED_REPO_PACKET_STORE_LOOKUP_HPP
#define NDNSF_DISTRIBUTED_REPO_PACKET_STORE_LOOKUP_HPP

#include "ndnsf-distributed-repo/RepoTypes.hpp"

namespace ndnsf_distributed_repo::detail {

/** Pin current immutable metadata; generic backends retain their owned-value contract. */
std::shared_ptr<const RepoObjectManifest>
readManifestSnapshot(const RepoStoreBackend& store, const std::string& objectName);

/** Observe current physical state without copying an owned backend's immutable packet index. */
RepoObjectMetadataSnapshot
probeObjectMetadataSnapshot(const RepoStoreBackend& store, const std::string& objectName);

/** Pure metadata filter; no storage access or admission callback. */
bool
matchesPacketLookup(const RepoObjectManifest& manifest, const std::string& canonical,
                    const std::string& alternate, bool prefixMatch);

/** Return owned candidate metadata, releasing backend locks before Core admission.
 * Owned filesystem/tiered stores avoid copying unrelated packet indexes. Other
 * backends retain their list contract. The base virtual interface is unchanged.
 */
std::vector<RepoObjectManifest>
findPacketManifests(const RepoStoreBackend& store, const std::string& canonical,
                    const std::string& alternate, bool prefixMatch);

} // namespace ndnsf_distributed_repo::detail

#endif // NDNSF_DISTRIBUTED_REPO_PACKET_STORE_LOOKUP_HPP
