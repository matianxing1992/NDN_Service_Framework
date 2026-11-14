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
};

} // namespace ndnsf_distributed_repo

#endif
