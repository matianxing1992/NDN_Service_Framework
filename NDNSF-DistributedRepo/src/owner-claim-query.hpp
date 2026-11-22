#ifndef NDNSF_DISTRIBUTED_REPO_OWNER_CLAIM_QUERY_HPP
#define NDNSF_DISTRIBUTED_REPO_OWNER_CLAIM_QUERY_HPP

#include <optional>
#include <string>

namespace ndnsf_distributed_repo::detail {

struct ExactOwnerClaimQuery
{
  std::string objectName;
  bool withManifest = false;
};

std::string
encodeExactOwnerClaimQuery(const std::string& objectName, bool withManifest = false);

/** Decode the exact USER_STORAGE_LIST selector. Version 1 uses the pagination codec. */
std::optional<ExactOwnerClaimQuery>
decodeExactOwnerClaimQuery(const std::string& wire);

} // namespace ndnsf_distributed_repo::detail

#endif // NDNSF_DISTRIBUTED_REPO_OWNER_CLAIM_QUERY_HPP
