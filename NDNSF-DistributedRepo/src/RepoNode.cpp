#include "ndnsf-distributed-repo/RepoNode.hpp"

#include "ndn-service-framework/LocalServiceRegistry.hpp"

#include <exception>
#include <stdexcept>
#include <sstream>
#include <utility>

namespace ndnsf_distributed_repo {

namespace {

std::vector<uint8_t>
payloadOf(const ndn_service_framework::RequestMessage& request)
{
  const auto payload = request.getPayload();
  return std::vector<uint8_t>(payload.begin(), payload.end());
}

} // namespace

RepoNode::RepoNode(ndn::Name servicePrefix, StorageCapability capability)
  : m_servicePrefix(std::move(servicePrefix))
  , m_core(std::move(capability))
{
}

RepoNode::RepoNode(ndn::Name servicePrefix,
                   StorageCapability capability,
                   std::shared_ptr<RepoStoreBackend> store)
  : m_servicePrefix(std::move(servicePrefix))
  , m_core(std::move(capability), std::move(store))
{
}

const ndn::Name&
RepoNode::servicePrefix() const
{
  return m_servicePrefix;
}

RepoCore&
RepoNode::core()
{
  return m_core;
}

const RepoCore&
RepoNode::core() const
{
  return m_core;
}

RepoObjectManifest
RepoNode::put(const std::string& objectName,
              const std::vector<uint8_t>& payload,
              const std::string& objectType,
              uint32_t replicationFactor,
              const std::string& policyEpoch,
              std::vector<std::string> replicaNodes)
{
  return m_core.put(objectName, payload, objectType, replicationFactor,
                    policyEpoch, std::move(replicaNodes));
}

std::vector<uint8_t>
RepoNode::get(const std::string& objectName) const
{
  return m_core.get(objectName);
}

RepoObjectManifest
RepoNode::getManifest(const std::string& objectName) const
{
  return m_core.getManifest(objectName);
}

std::vector<RepoObjectManifest>
RepoNode::list() const
{
  return m_core.list();
}

bool
RepoNode::remove(const std::string& objectName)
{
  return m_core.remove(objectName);
}

void
RepoNode::registerServices(ndn_service_framework::ServiceProvider& provider)
{
  auto ack = [] (const ndn_service_framework::RequestMessage&) {
    ndn_service_framework::ServiceProvider::AckDecision decision;
    decision.status = true;
    decision.message = "repo-ready";
    return decision;
  };

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "STORE"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStore(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "INSERT"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleInsert(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "STORE_MANIFEST"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStoreManifest(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "FETCH"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleFetch(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "MANIFEST"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleManifest(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "INVENTORY"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleInventory());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "CAPABILITY"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCapability());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "STATUS"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStatus(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "DELETE"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleDelete(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });
}

void
RepoNode::registerLocalServices(ndn_service_framework::LocalServiceRegistry& registry)
{
  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "STORE"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStore(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "INSERT"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleInsert(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "STORE_MANIFEST"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStoreManifest(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "FETCH"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleFetch(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "MANIFEST"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleManifest(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "INVENTORY"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleInventory());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "CAPABILITY"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCapability());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "STATUS"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStatus(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registry.registerLocalService(
    makeRepoServiceName(m_servicePrefix, "DELETE"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleDelete(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });
}

void
RepoNode::registerDeploymentServices(
  ndn_service_framework::ServiceProvider* provider,
  ndn_service_framework::LocalServiceRegistry* registry,
  RepoDeploymentMode mode)
{
  if (enablesRemote(mode)) {
    if (provider == nullptr) {
      throw std::invalid_argument("remote repo deployment mode requires a ServiceProvider");
    }
    registerServices(*provider);
  }

  if (enablesEmbedded(mode)) {
    if (registry == nullptr) {
      throw std::invalid_argument(
        "embedded repo deployment mode requires a LocalServiceRegistry");
    }
    registerLocalServices(*registry);
  }
}

void
RepoNode::setDataReferenceFetcher(DataReferenceFetcher fetcher)
{
  m_dataReferenceFetcher = std::move(fetcher);
}

RepoOperationStatus
RepoNode::insertWirePackets(const RepoDataReference& reference,
                            const std::vector<std::vector<uint8_t>>& wirePackets)
{
  if (reference.objectName.empty()) {
    throw std::invalid_argument("repo data reference objectName must not be empty");
  }

  RepoOperationStatus status;
  status.operationId = allocateOperationId();
  status.operation = "INSERT";
  status.state = "STORING";
  status.objectName = reference.objectName;
  status.totalSegments = wirePackets.size();
  status.message = "storing opaque Data wire packets";
  rememberStatus(status);

  try {
    std::vector<uint8_t> concatenated;
    uint64_t totalSize = 0;
    for (size_t i = 0; i < wirePackets.size(); ++i) {
      totalSize += wirePackets[i].size();
      concatenated.insert(concatenated.end(), wirePackets[i].begin(), wirePackets[i].end());
      m_core.put(reference.objectName + "/ndn-data/" + std::to_string(i),
                 wirePackets[i],
                 reference.objectType + ".wire",
                 1,
                 "",
                 {});
      status.completedSegments = i + 1;
      rememberStatus(status);
    }

    if (reference.expectedSize != 0 && totalSize != reference.expectedSize) {
      status.state = "FAILED";
      status.message = "fetched wire packet size mismatch";
      rememberStatus(status);
      return status;
    }
    if (!reference.expectedSha256.empty() &&
        sha256Hex(concatenated) != reference.expectedSha256) {
      status.state = "FAILED";
      status.message = "fetched wire packet sha256 mismatch";
      rememberStatus(status);
      return status;
    }

    RepoObjectManifest manifest;
    manifest.objectName = reference.objectName;
    manifest.objectType = reference.objectType;
    manifest.sha256 = sha256Hex(concatenated);
    manifest.size = totalSize;
    manifest.segmentCount = static_cast<uint32_t>(wirePackets.size());
    m_core.putManifest(manifest);

    status.state = "DONE";
    status.message = "stored app-owned segmented Data wire packets";
    status.completedSegments = wirePackets.size();
    status.totalSegments = wirePackets.size();
    rememberStatus(status);
    return status;
  }
  catch (const std::exception& e) {
    status.state = "FAILED";
    status.message = e.what();
    rememberStatus(status);
    return status;
  }
}

std::vector<uint8_t>
RepoNode::handleStore(const std::vector<uint8_t>& request)
{
  return m_core.handleStore(request);
}

std::vector<uint8_t>
RepoNode::handleInsert(const std::vector<uint8_t>& request)
{
  const auto reference = parseDataReferenceJson(toString(request));
  if (reference.objectName.empty()) {
    throw std::invalid_argument("repo data reference objectName must not be empty");
  }
  if (reference.dataPrefix.empty()) {
    throw std::invalid_argument("repo data reference dataPrefix must not be empty");
  }

  if (m_dataReferenceFetcher) {
    try {
      auto wirePackets = m_dataReferenceFetcher(reference);
      const auto storedStatus = insertWirePackets(reference, wirePackets);
      return toBytes(storedStatus.toJson());
    }
    catch (const std::exception& e) {
      RepoOperationStatus status;
      status.operationId = allocateOperationId();
      status.operation = "INSERT";
      status.state = "FAILED";
      status.objectName = reference.objectName;
      status.message = e.what();
      rememberStatus(status);
      return toBytes(status.toJson());
    }
  }

  RepoOperationStatus status;
  status.operationId = allocateOperationId();
  status.operation = "INSERT";
  status.state = "FAILED";
  status.objectName = reference.objectName;
  status.totalSegments = reference.hasFinalSegment
    ? (reference.finalSegment >= reference.firstSegment
       ? reference.finalSegment - reference.firstSegment + 1
       : 0)
    : 0;
  status.message = "no SegmentFetcher adapter configured";
  rememberStatus(status);
  return toBytes(status.toJson());
}

std::vector<uint8_t>
RepoNode::handleStoreManifest(const std::vector<uint8_t>& request)
{
  return m_core.handleStoreManifest(request);
}

std::vector<uint8_t>
RepoNode::handleFetch(const std::vector<uint8_t>& request) const
{
  return m_core.handleFetch(request);
}

std::vector<uint8_t>
RepoNode::handleManifest(const std::vector<uint8_t>& request) const
{
  return m_core.handleManifest(request);
}

std::vector<uint8_t>
RepoNode::handleInventory() const
{
  return m_core.handleInventory();
}

std::vector<uint8_t>
RepoNode::handleCapability() const
{
  return m_core.handleCapability();
}

std::vector<uint8_t>
RepoNode::handleDelete(const std::vector<uint8_t>& request)
{
  return m_core.handleDelete(request);
}

std::vector<uint8_t>
RepoNode::handleStatus(const std::vector<uint8_t>& request) const
{
  const auto operationId = toString(request);
  std::lock_guard<std::mutex> guard(m_statusMutex);
  const auto it = m_statusById.find(operationId);
  if (it == m_statusById.end()) {
    RepoOperationStatus status;
    status.operationId = operationId;
    status.operation = "UNKNOWN";
    status.state = "UNKNOWN";
    status.message = "repo operation status not found";
    return toBytes(status.toJson());
  }
  return toBytes(it->second.toJson());
}

ndn_service_framework::ResponseMessage
RepoNode::makeResponse(const std::vector<uint8_t>& payload) const
{
  ndn::Buffer responsePayload(payload.data(), payload.size());
  ndn_service_framework::ResponseMessage response;
  response.setStatus(true);
  response.setErrorInfo("No error");
  response.setPayload(responsePayload, responsePayload.size());
  return response;
}

ndn_service_framework::ResponseMessage
RepoNode::makeError(const std::string& error) const
{
  ndn_service_framework::ResponseMessage response;
  response.setStatus(false);
  response.setErrorInfo(error);
  auto payload = toBytes(error);
  ndn::Buffer responsePayload(payload.data(), payload.size());
  response.setPayload(responsePayload, responsePayload.size());
  return response;
}

std::string
RepoNode::allocateOperationId()
{
  std::lock_guard<std::mutex> guard(m_statusMutex);
  std::ostringstream os;
  os << "repo-op-" << ++m_nextOperationId;
  return os.str();
}

void
RepoNode::rememberStatus(const RepoOperationStatus& status)
{
  std::lock_guard<std::mutex> guard(m_statusMutex);
  m_statusById[status.operationId] = status;
}

} // namespace ndnsf_distributed_repo
