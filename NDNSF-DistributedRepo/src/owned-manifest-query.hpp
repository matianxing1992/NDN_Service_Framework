#ifndef NDNSF_DISTRIBUTED_REPO_OWNED_MANIFEST_QUERY_HPP
#define NDNSF_DISTRIBUTED_REPO_OWNED_MANIFEST_QUERY_HPP

#include "ndnsf-distributed-repo/RepoProtocol.hpp"

namespace ndnsf_distributed_repo::detail {

/** Owner-bound MANIFEST selector without retransmitting the packet index.
 * An empty manifestSha256 requests the full manifest. Otherwise the Server
 * confirms that exact canonical manifest and returns its digest after admission.
 */
struct OwnedManifestQuery
{
  OwnedManifestQuery() = default;

  explicit OwnedManifestQuery(const RepoUserStorageInsertRequest& insert)
    : operationId(insert.operationId)
    , generation(insert.generation)
    , objectName(insert.reference.objectName)
    , dataPrefix(insert.reference.dataPrefix)
    , expectedSize(insert.reference.expectedSize)
    , expectedSha256(insert.reference.expectedSha256)
    , storageBinding(insert.storageBinding)
  {
  }

  std::string operationId;
  uint64_t generation = 0;
  std::string objectName;
  std::string dataPrefix;
  uint64_t expectedSize = 0;
  std::string expectedSha256;
  RepoUserStorageBinding storageBinding;
  std::string manifestSha256;
};

std::vector<uint8_t>
encodeOwnedManifestQuery(const OwnedManifestQuery& request);

OwnedManifestQuery
decodeOwnedManifestQuery(const std::vector<uint8_t>& request);

} // namespace ndnsf_distributed_repo::detail

#endif // NDNSF_DISTRIBUTED_REPO_OWNED_MANIFEST_QUERY_HPP
