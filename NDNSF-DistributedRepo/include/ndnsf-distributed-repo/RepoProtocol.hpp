#ifndef NDNSF_DISTRIBUTED_REPO_REPO_PROTOCOL_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_PROTOCOL_HPP

#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <ndn-cxx/name.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf_distributed_repo {

/** Storage selectors only; the writer identity comes from authenticated Core context. */
struct RepoUserStorageInsertRequest
{
  /** 1: identity/trust validation (unchanged default).
   * 2: DigestSha256 packets bound to this authenticated writer's exact storage transaction.
   * Digest mode does not establish consumer trust; consumers need a pretrusted commitment.
   */
  uint32_t schemaVersion = 1;
  std::string operationId;
  uint64_t generation = 0;
  RepoDataReference reference;
  RepoUserStorageBinding storageBinding;
};

ndn::Name
makeRepoServiceName(const ndn::Name& prefix, const std::string& operation);

std::vector<uint8_t>
toBytes(const std::string& text);

std::string
toString(const std::vector<uint8_t>& bytes);

std::vector<uint8_t>
encodeStoreRequest(const RepoObjectManifest& manifest,
                   const std::vector<uint8_t>& payload);

/** Encode one bounded range write without concatenating the object payload. */
std::vector<uint8_t>
encodeRangeWriteRequest(const RepoObjectManifest& manifest,
                        RepoByteRange range,
                        const std::vector<uint8_t>& bytes);

/** Encode a bounded range read request. */
std::vector<uint8_t>
encodeRangeReadRequest(const std::string& objectName, RepoByteRange range);

std::vector<uint8_t>
encodeRangeAbortRequest(const std::string& objectName);

std::vector<uint8_t>
encodeManifestRequest(const RepoObjectManifest& manifest);

std::vector<uint8_t>
encodeDataReferenceRequest(const RepoDataReference& reference);

std::vector<uint8_t>
encodeStatusRequest(const std::string& operationId);

std::vector<uint8_t>
encodeUserStorageInsertRequest(const RepoUserStorageInsertRequest& request);

RepoUserStorageInsertRequest
decodeUserStorageInsertRequest(const std::vector<uint8_t>& request);

std::vector<uint8_t>
encodeCatalogDeltaRequest(uint64_t sinceEpoch);

std::vector<uint8_t>
encodeCatalogLookupRequest(const std::string& objectName);

void
decodeStoreRequest(const std::vector<uint8_t>& request,
                   RepoObjectManifest& manifest,
                   std::vector<uint8_t>& payload);

void
decodeRangeWriteRequest(const std::vector<uint8_t>& request,
                        RepoObjectManifest& manifest,
                        RepoByteRange& range,
                        std::vector<uint8_t>& bytes);

void
decodeRangeReadRequest(const std::vector<uint8_t>& request,
                       std::string& objectName,
                       RepoByteRange& range);

std::string
decodeRangeAbortRequest(const std::vector<uint8_t>& request);

RepoDataReference
parseDataReferenceJson(const std::string& referenceJson);

RepoOperationStatus
parseOperationStatusJson(const std::string& statusJson);

RepoObjectManifest
parseManifestJson(const std::string& manifestJson);

RepoCatalogEntry
parseCatalogEntryJson(const std::string& entryJson);

RepoCatalogStatus
parseCatalogStatusJson(const std::string& statusJson);

RepoCacheStatus
parseCacheStatusJson(const std::string& statusJson);

RepoCatalogDelta
parseCatalogDeltaJson(const std::string& deltaJson);

std::vector<RepoObjectManifest>
parseInventoryJson(const std::string& inventoryJson);

std::string
encodeInventory(const std::vector<RepoObjectManifest>& manifests);

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_PROTOCOL_HPP
