#ifndef NDNSF_DISTRIBUTED_REPO_REPO_PROTOCOL_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_PROTOCOL_HPP

#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <ndn-cxx/name.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf_distributed_repo {

ndn::Name
makeRepoServiceName(const ndn::Name& prefix, const std::string& operation);

std::vector<uint8_t>
toBytes(const std::string& text);

std::string
toString(const std::vector<uint8_t>& bytes);

std::vector<uint8_t>
encodeStoreRequest(const RepoObjectManifest& manifest,
                   const std::vector<uint8_t>& payload);

void
decodeStoreRequest(const std::vector<uint8_t>& request,
                   RepoObjectManifest& manifest,
                   std::vector<uint8_t>& payload);

RepoObjectManifest
parseManifestJson(const std::string& manifestJson);

std::string
encodeInventory(const std::vector<RepoObjectManifest>& manifests);

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_PROTOCOL_HPP
