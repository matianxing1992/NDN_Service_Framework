#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"

#include "ndn-service-framework/LocalServiceRegistry.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ndnsf_distributed_repo {

namespace {

std::vector<uint8_t>
payloadBytes(const ndn_service_framework::ResponseMessage& response)
{
  const auto payload = response.getPayload();
  return std::vector<uint8_t>(payload.begin(), payload.end());
}

std::vector<uint8_t>
localRequest(ndn_service_framework::LocalServiceRegistry& registry,
             const ndn::Name& repoServicePrefix,
             const std::string& operation,
             const std::vector<uint8_t>& payload)
{
  auto request = RepoClient::makeRequest(payload);
  auto response = registry.localInvokeRaw(
    makeRepoServiceName(repoServicePrefix, operation),
    request);
  request.Clear();
  if (!response.getStatus()) {
    throw std::runtime_error("local repo " + operation + " failed: " +
                             response.getErrorInfo());
  }
  return payloadBytes(response);
}

std::string
segmentObjectName(const std::string& objectName, size_t segmentIndex)
{
  return objectName + "/seg/" + std::to_string(segmentIndex);
}

RepoObjectManifest
makeParentManifest(const std::string& objectName,
                   const std::vector<uint8_t>& payload,
                   const StoreOptions& options,
                   size_t segmentCount)
{
  RepoObjectManifest manifest;
  manifest.objectName = objectName;
  manifest.objectType = options.objectType;
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.segmentCount = static_cast<uint32_t>(segmentCount);
  manifest.replicationFactor = options.replicationFactor;
  manifest.replicaNodes = options.replicaNodes;
  manifest.policyEpoch = options.policyEpoch;
  return manifest;
}

void
verifySegmentedPayload(const RepoObjectManifest& manifest,
                       const std::vector<uint8_t>& payload)
{
  if (payload.size() != manifest.size) {
    throw std::runtime_error("segmented repo object size mismatch: " +
                             manifest.objectName);
  }
  if (!manifest.sha256.empty() && sha256Hex(payload) != manifest.sha256) {
    throw std::runtime_error("segmented repo object sha256 mismatch: " +
                             manifest.objectName);
  }
}

} // namespace

RepoObjectManifest
RepoClient::put(RepoNode& node,
                const std::string& objectName,
                const std::vector<uint8_t>& payload,
                StoreOptions options)
{
  return node.put(objectName,
                  payload,
                  options.objectType,
                  options.replicationFactor,
                  options.policyEpoch,
                  std::move(options.replicaNodes));
}

std::vector<uint8_t>
RepoClient::get(const RepoNode& node, const std::string& objectName)
{
  return node.get(objectName);
}

RepoObjectManifest
RepoClient::getManifest(const RepoNode& node, const std::string& objectName)
{
  return node.getManifest(objectName);
}

std::vector<RepoObjectManifest>
RepoClient::list(const RepoNode& node)
{
  return node.list();
}

bool
RepoClient::remove(RepoNode& node, const std::string& objectName)
{
  return node.remove(objectName);
}

RepoObjectManifest
RepoClient::putSegmented(RepoNode& node,
                         const std::string& objectName,
                         const std::vector<uint8_t>& payload,
                         StoreOptions options,
                         size_t maxSegmentPayload)
{
  maxSegmentPayload = std::max<size_t>(1, maxSegmentPayload);
  if (payload.size() <= maxSegmentPayload) {
    return put(node, objectName, payload, std::move(options));
  }

  const size_t segmentCount =
    (payload.size() + maxSegmentPayload - 1) / maxSegmentPayload;
  StoreOptions segmentOptions = options;
  segmentOptions.objectType = options.objectType + ".segment";
  for (size_t i = 0; i < segmentCount; ++i) {
    const auto start = i * maxSegmentPayload;
    const auto end = std::min(payload.size(), start + maxSegmentPayload);
    const std::vector<uint8_t> segmentPayload(payload.begin() + start,
                                              payload.begin() + end);
    put(node, segmentObjectName(objectName, i), segmentPayload, segmentOptions);
  }

  auto manifest = makeParentManifest(objectName, payload, options, segmentCount);
  return node.core().putManifest(manifest);
}

std::vector<uint8_t>
RepoClient::getSegmented(const RepoNode& node, const RepoObjectManifest& manifest)
{
  if (manifest.segmentCount <= 1) {
    auto payload = get(node, manifest.objectName);
    verifySegmentedPayload(manifest, payload);
    return payload;
  }

  std::vector<uint8_t> payload;
  payload.reserve(static_cast<size_t>(manifest.size));
  for (uint32_t i = 0; i < manifest.segmentCount; ++i) {
    auto segment = get(node, segmentObjectName(manifest.objectName, i));
    payload.insert(payload.end(), segment.begin(), segment.end());
  }
  verifySegmentedPayload(manifest, payload);
  return payload;
}

std::vector<uint8_t>
RepoClient::getObject(const RepoNode& node, const RepoObjectManifest& manifest)
{
  return getSegmented(node, manifest);
}

RepoObjectManifest
RepoClient::localPut(ndn_service_framework::LocalServiceRegistry& registry,
                     const ndn::Name& repoServicePrefix,
                     const std::string& objectName,
                     const std::vector<uint8_t>& payload,
                     StoreOptions options)
{
  auto manifest = makeManifest(objectName,
                               options.objectType,
                               payload,
                               options.replicationFactor,
                               std::move(options.replicaNodes),
                               std::move(options.policyEpoch));
  return parseManifestJson(toString(localRequest(
    registry, repoServicePrefix, "STORE", encodeStoreRequest(manifest, payload))));
}

std::vector<uint8_t>
RepoClient::localGet(ndn_service_framework::LocalServiceRegistry& registry,
                     const ndn::Name& repoServicePrefix,
                     const std::string& objectName)
{
  return localRequest(registry, repoServicePrefix, "FETCH", toBytes(objectName));
}

RepoObjectManifest
RepoClient::localGetManifest(ndn_service_framework::LocalServiceRegistry& registry,
                             const ndn::Name& repoServicePrefix,
                             const std::string& objectName)
{
  return parseManifestJson(toString(localRequest(
    registry, repoServicePrefix, "MANIFEST", toBytes(objectName))));
}

std::vector<RepoObjectManifest>
RepoClient::localList(ndn_service_framework::LocalServiceRegistry& registry,
                      const ndn::Name& repoServicePrefix)
{
  return parseInventoryJson(toString(localRequest(
    registry, repoServicePrefix, "INVENTORY", {})));
}

bool
RepoClient::localRemove(ndn_service_framework::LocalServiceRegistry& registry,
                        const ndn::Name& repoServicePrefix,
                        const std::string& objectName)
{
  return toString(localRequest(registry, repoServicePrefix, "DELETE",
                               toBytes(objectName))) == "deleted";
}

RepoObjectManifest
RepoClient::localPutSegmented(ndn_service_framework::LocalServiceRegistry& registry,
                              const ndn::Name& repoServicePrefix,
                              const std::string& objectName,
                              const std::vector<uint8_t>& payload,
                              StoreOptions options,
                              size_t maxSegmentPayload)
{
  maxSegmentPayload = std::max<size_t>(1, maxSegmentPayload);
  if (payload.size() <= maxSegmentPayload) {
    return localPut(registry, repoServicePrefix, objectName, payload,
                    std::move(options));
  }

  const size_t segmentCount =
    (payload.size() + maxSegmentPayload - 1) / maxSegmentPayload;
  StoreOptions segmentOptions = options;
  segmentOptions.objectType = options.objectType + ".segment";
  for (size_t i = 0; i < segmentCount; ++i) {
    const auto start = i * maxSegmentPayload;
    const auto end = std::min(payload.size(), start + maxSegmentPayload);
    const std::vector<uint8_t> segmentPayload(payload.begin() + start,
                                              payload.begin() + end);
    localPut(registry, repoServicePrefix, segmentObjectName(objectName, i),
             segmentPayload, segmentOptions);
  }

  auto manifest = makeParentManifest(objectName, payload, options, segmentCount);
  return parseManifestJson(toString(localRequest(
    registry, repoServicePrefix, "STORE_MANIFEST", encodeManifestRequest(manifest))));
}

std::vector<uint8_t>
RepoClient::localGetSegmented(ndn_service_framework::LocalServiceRegistry& registry,
                              const ndn::Name& repoServicePrefix,
                              const RepoObjectManifest& manifest)
{
  if (manifest.segmentCount <= 1) {
    auto payload = localGet(registry, repoServicePrefix, manifest.objectName);
    verifySegmentedPayload(manifest, payload);
    return payload;
  }

  std::vector<uint8_t> payload;
  payload.reserve(static_cast<size_t>(manifest.size));
  for (uint32_t i = 0; i < manifest.segmentCount; ++i) {
    auto segment = localGet(registry, repoServicePrefix,
                            segmentObjectName(manifest.objectName, i));
    payload.insert(payload.end(), segment.begin(), segment.end());
  }
  verifySegmentedPayload(manifest, payload);
  return payload;
}

std::vector<uint8_t>
RepoClient::localGetObject(ndn_service_framework::LocalServiceRegistry& registry,
                           const ndn::Name& repoServicePrefix,
                           const RepoObjectManifest& manifest)
{
  return localGetSegmented(registry, repoServicePrefix, manifest);
}

RepoObjectManifest
RepoClient::makeManifest(std::string objectName,
                         std::string objectType,
                         const std::vector<uint8_t>& payload,
                         uint32_t replicationFactor,
                         std::vector<std::string> replicaNodes,
                         std::string policyEpoch)
{
  RepoObjectManifest manifest;
  manifest.objectName = std::move(objectName);
  manifest.objectType = std::move(objectType);
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.segmentCount = 1;
  manifest.replicationFactor = replicationFactor;
  manifest.replicaNodes = std::move(replicaNodes);
  manifest.policyEpoch = std::move(policyEpoch);
  return manifest;
}

ndn_service_framework::RequestMessage
RepoClient::makeRequest(const std::vector<uint8_t>& payload)
{
  ndn::Buffer buffer(payload.data(), payload.size());
  ndn_service_framework::RequestMessage request;
  request.setPayload(buffer, buffer.size());
  return request;
}

ndn::Name
RepoClient::requestCapability(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "CAPABILITY"),
                             makeRequest({}),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestStore(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const RepoObjectManifest& manifest,
  const std::vector<uint8_t>& payload,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "STORE"),
                             makeRequest(encodeStoreRequest(manifest, payload)),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestFetch(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const std::string& objectName,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "FETCH"),
                             makeRequest(toBytes(objectName)),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestManifest(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const std::string& objectName,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "MANIFEST"),
                             makeRequest(toBytes(objectName)),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestInventory(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "INVENTORY"),
                             makeRequest({}),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestDelete(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const std::string& objectName,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "DELETE"),
                             makeRequest(toBytes(objectName)),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

} // namespace ndnsf_distributed_repo
