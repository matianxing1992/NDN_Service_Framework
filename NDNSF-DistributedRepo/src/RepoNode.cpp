#include "ndnsf-distributed-repo/RepoNode.hpp"

#include "ndn-service-framework/LocalServiceRegistry.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>

#include <algorithm>
#include <exception>
#include <iterator>
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

RepoCacheStatus
RepoNode::cacheStatus() const
{
  return m_core.cacheStatus();
}

bool
RepoNode::remove(const std::string& objectName)
{
  return m_core.remove(objectName);
}

RepoObjectManifest
RepoNode::putDataPacket(const std::string& dataName,
                        const std::vector<uint8_t>& wire)
{
  return m_core.putDataPacket(dataName, wire);
}

std::vector<uint8_t>
RepoNode::getDataPacket(const std::string& dataName) const
{
  return m_core.getDataPacket(dataName);
}

bool
RepoNode::hasDataPacket(const std::string& dataName) const
{
  return m_core.hasDataPacket(dataName);
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
    makeRepoServiceName(m_servicePrefix, "CACHE_STATUS"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCacheStatus());
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
    makeRepoServiceName(m_servicePrefix, "CATALOG_STATUS"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCatalogStatus());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "CATALOG_SNAPSHOT"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCatalogSnapshot());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "CATALOG_DELTA"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleCatalogDelta(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "CATALOG_LOOKUP"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleCatalogLookup(payloadOf(request)));
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
  auto registerLocalRepoService =
    [&] (const std::string& operation,
         std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> handler) {
      registry.registerLocalService(
        makeRepoServiceName(m_servicePrefix, operation),
        [this, handler = std::move(handler)] (
          const ndn::Name&, const ndn::Name&,
          const ndn_service_framework::RequestMessage& request) {
          try {
            return makeResponse(handler(payloadOf(request)));
          }
          catch (const std::exception& e) {
            return makeError(e.what());
          }
        });
    };

  registerLocalRepoService("STORE", [this] (const std::vector<uint8_t>& request) {
    return handleStore(request);
  });
  registerLocalRepoService("INSERT", [this] (const std::vector<uint8_t>& request) {
    return handleInsert(request);
  });
  registerLocalRepoService("STORE_MANIFEST", [this] (const std::vector<uint8_t>& request) {
    return handleStoreManifest(request);
  });
  registerLocalRepoService("FETCH", [this] (const std::vector<uint8_t>& request) {
    return handleFetch(request);
  });
  registerLocalRepoService("MANIFEST", [this] (const std::vector<uint8_t>& request) {
    return handleManifest(request);
  });
  registerLocalRepoService("INVENTORY", [this] (const std::vector<uint8_t>&) {
    return handleInventory();
  });
  registerLocalRepoService("CAPABILITY", [this] (const std::vector<uint8_t>&) {
    return handleCapability();
  });
  registerLocalRepoService("CACHE_STATUS", [this] (const std::vector<uint8_t>&) {
    return handleCacheStatus();
  });
  registerLocalRepoService("STATUS", [this] (const std::vector<uint8_t>& request) {
    return handleStatus(request);
  });
  registerLocalRepoService("CATALOG_STATUS", [this] (const std::vector<uint8_t>&) {
    return handleCatalogStatus();
  });
  registerLocalRepoService("CATALOG_SNAPSHOT", [this] (const std::vector<uint8_t>&) {
    return handleCatalogSnapshot();
  });
  registerLocalRepoService("CATALOG_DELTA", [this] (const std::vector<uint8_t>& request) {
    return handleCatalogDelta(request);
  });
  registerLocalRepoService("CATALOG_LOOKUP", [this] (const std::vector<uint8_t>& request) {
    return handleCatalogLookup(request);
  });
  registerLocalRepoService("DELETE", [this] (const std::vector<uint8_t>& request) {
    return handleDelete(request);
  });
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
    std::vector<std::string> packetNames;
    packetNames.reserve(wirePackets.size());
    const ndn::Name expectedPrefix(reference.dataPrefix);
    for (size_t i = 0; i < wirePackets.size(); ++i) {
      totalSize += wirePackets[i].size();
      concatenated.insert(concatenated.end(), wirePackets[i].begin(), wirePackets[i].end());
      ndn::Block block(ndn::span<const uint8_t>(wirePackets[i].data(),
                                                wirePackets[i].size()));
      block.parse();
      const ndn::Data data(block);
      if (!expectedPrefix.isPrefixOf(data.getName())) {
        throw std::runtime_error("repo-packet-set-invalid: Data name is outside "
                                 "declared dataPrefix: " +
                                 data.getName().toUri());
      }
      const auto dataName = data.getName().toUri();
      if (std::find(packetNames.begin(), packetNames.end(), dataName) != packetNames.end()) {
        throw std::runtime_error(
          "repo-packet-set-invalid: duplicate NDN Data name: " + dataName);
      }
      if (m_core.hasDataPacket(dataName) &&
          m_core.getDataPacket(dataName) != wirePackets[i]) {
        throw std::runtime_error(
          "repo-data-wire-conflict: immutable NDN Data name conflict: " + dataName);
      }
      packetNames.push_back(dataName);
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

    for (size_t i = 0; i < wirePackets.size(); ++i) {
      m_core.putDataPacket(packetNames[i], wirePackets[i]);
      status.completedSegments = i + 1;
      rememberStatus(status);
    }

    RepoObjectManifest manifest;
    manifest.objectName = reference.objectName;
    manifest.objectType = reference.objectType;
    manifest.sha256 = sha256Hex(concatenated);
    manifest.size = totalSize;
    manifest.segmentCount = static_cast<uint32_t>(wirePackets.size());
    manifest.packetNames = packetNames;
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
RepoNode::handleCacheStatus() const
{
  return m_core.handleCacheStatus();
}

std::vector<uint8_t>
RepoNode::handleCatalogStatus() const
{
  return m_core.handleCatalogStatus();
}

std::vector<uint8_t>
RepoNode::handleCatalogSnapshot() const
{
  return m_core.handleCatalogSnapshot();
}

std::vector<uint8_t>
RepoNode::handleCatalogDelta(const std::vector<uint8_t>& request) const
{
  return m_core.handleCatalogDelta(request);
}

std::vector<uint8_t>
RepoNode::handleCatalogLookup(const std::vector<uint8_t>& request) const
{
  return m_core.handleCatalogLookup(request);
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
  constexpr size_t MaxRetainedStatuses = 1024;
  while (m_statusById.size() > MaxRetainedStatuses) {
    auto oldest = m_statusById.begin();
    for (auto it = std::next(m_statusById.begin()); it != m_statusById.end(); ++it) {
      if (it->second.updatedAtMs < oldest->second.updatedAtMs) {
        oldest = it;
      }
    }
    m_statusById.erase(oldest);
  }
}

} // namespace ndnsf_distributed_repo
