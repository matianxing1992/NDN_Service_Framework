#include "ndnsf-distributed-repo/RepoNode.hpp"

#include "ndn-service-framework/LocalServiceRegistry.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>

#include <algorithm>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <sstream>
#include <utility>
#include <openssl/evp.h>
#include <cstdio>
#include <unistd.h>
#include <iomanip>
#include <limits>

namespace ndnsf_distributed_repo {

namespace {

std::vector<uint8_t>
payloadOf(const ndn_service_framework::RequestMessage& request)
{
  const auto payload = request.getPayload();
  return std::vector<uint8_t>(payload.begin(), payload.end());
}

} // namespace

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

void
RepoNode::putRange(const RepoObjectManifest& manifest,
                   RepoByteRange range,
                   const std::vector<uint8_t>& bytes)
{
  m_core.putRange(manifest, range, bytes);
}

RepoObjectManifest
RepoNode::commitRanges(const RepoObjectManifest& manifest)
{
  return m_core.commitRanges(manifest);
}

void
RepoNode::abortRanges(const std::string& objectName)
{
  m_core.abortRanges(objectName);
}

std::vector<uint8_t>
RepoNode::getRange(const std::string& objectName, RepoByteRange range) const
{
  return m_core.getRange(objectName, range);
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
    makeRepoServiceName(m_servicePrefix, "STORE_RANGE"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStoreRange(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "COMMIT_RANGES"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleCommitRanges(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  provider.addService(
    makeRepoServiceName(m_servicePrefix, "ABORT_RANGES"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleAbortRanges(payloadOf(request)));
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
    makeRepoServiceName(m_servicePrefix, "FETCH_RANGE"),
    ack,
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleFetchRange(payloadOf(request)));
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
          const ndn_service_framework::RequestMessage& request,
          ndn_service_framework::ResponseMessage& response) {
          try {
            response = makeResponse(handler(payloadOf(request)));
          }
          catch (const std::exception& e) {
            response = makeError(e.what());
          }
        });
    };

  registerLocalRepoService("STORE", [this] (const std::vector<uint8_t>& request) {
    return handleStore(request);
  });
  registerLocalRepoService("STORE_RANGE", [this] (const std::vector<uint8_t>& request) {
    return handleStoreRange(request);
  });
  registerLocalRepoService("COMMIT_RANGES", [this] (const std::vector<uint8_t>& request) {
    return handleCommitRanges(request);
  });
  registerLocalRepoService("ABORT_RANGES", [this] (const std::vector<uint8_t>& request) {
    return handleAbortRanges(request);
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
  registerLocalRepoService("FETCH_RANGE", [this] (const std::vector<uint8_t>& request) {
    return handleFetchRange(request);
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
    if (!m_core.supportsOriginalPacketTransactions() || wirePackets.empty())
      throw std::runtime_error("repo-original-packet-transaction-unsupported-or-empty");
    auto spool = std::unique_ptr<std::FILE, decltype(&std::fclose)>(std::tmpfile(), std::fclose);
    auto digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!spool || !digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
      throw std::runtime_error("repo-packet-spool-initialization-failed");
    uint64_t totalSize = 0;
    uint64_t contentBytes = 0;
    std::vector<uint64_t> packetOffsets{0};
    std::vector<std::string> packetNames;
    packetNames.reserve(wirePackets.size());
    const ndn::Name expectedPrefix(reference.dataPrefix);
    for (size_t i = 0; i < wirePackets.size(); ++i) {
      if (wirePackets[i].empty() || wirePackets[i].size() > ndn::MAX_NDN_PACKET_SIZE ||
          totalSize > std::numeric_limits<uint64_t>::max() - wirePackets[i].size())
        throw std::invalid_argument("repo-packet-set-invalid: packet size");
      totalSize += wirePackets[i].size();
      ndn::Block block(ndn::span<const uint8_t>(wirePackets[i].data(),
                                                wirePackets[i].size()));
      block.parse();
      const ndn::Data data(block);
      if (data.wireEncode().size() != wirePackets[i].size())
        throw std::invalid_argument("repo-packet-set-invalid: trailing bytes");
      contentBytes += data.getContent().value_size();
      if (!expectedPrefix.isPrefixOf(data.getName())) {
        throw std::runtime_error("repo-packet-set-invalid: Data name is outside "
                                 "declared dataPrefix: " +
                                 data.getName().toUri());
      }
      const auto dataName = data.getName().toUri(ndn::name::UriFormat::CANONICAL);
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
      packetOffsets.push_back(totalSize);
      if (std::fwrite(wirePackets[i].data(), 1, wirePackets[i].size(), spool.get()) != wirePackets[i].size() ||
          EVP_DigestUpdate(digest.get(), wirePackets[i].data(), wirePackets[i].size()) != 1)
        throw std::runtime_error("repo-packet-spool-write-failed");
    }

    if (ndn::Name(reference.objectName) != ndn::Name(packetNames.front()))
      throw std::invalid_argument("repo-original-set-root-name-mismatch");
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned hashSize = 0;
    if (EVP_DigestFinal_ex(digest.get(), hash, &hashSize) != 1 || std::fflush(spool.get()) != 0)
      throw std::runtime_error("repo-packet-spool-finalization-failed");
    std::ostringstream hashText;
    for (unsigned i = 0; i < hashSize; ++i)
      hashText << std::hex << std::setw(2) << std::setfill('0') << unsigned(hash[i]);

    if (reference.expectedSize != 0 && totalSize != reference.expectedSize) {
      status.state = "FAILED";
      status.message = "fetched wire packet size mismatch";
      rememberStatus(status);
      return status;
    }
    if (!reference.expectedSha256.empty() &&
        hashText.str() != reference.expectedSha256) {
      status.state = "FAILED";
      status.message = "fetched wire packet sha256 mismatch";
      rememberStatus(status);
      return status;
    }

    RepoObjectManifest manifest;
    manifest.objectName = ndn::Name(reference.objectName).toUri(ndn::name::UriFormat::CANONICAL);
    manifest.objectType = "original-packet-set";
    manifest.sha256 = hashText.str();
    manifest.size = totalSize;
    if (packetNames.size() > std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument("repo-packet-count-overflow");
    manifest.segmentCount = static_cast<uint32_t>(packetNames.size());
    manifest.packetNames = packetNames;
    manifest.packetOffsets = std::move(packetOffsets);
    manifest.envelopeBytes = contentBytes;
    manifest.dataPrefix = expectedPrefix.toUri(ndn::name::UriFormat::CANONICAL);
    manifest.operationId = status.operationId;
    m_core.commitOriginalPacketFile(manifest,
      "/proc/self/fd/" + std::to_string(::fileno(spool.get())));

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
RepoNode::handleStoreRange(const std::vector<uint8_t>& request)
{
  return m_core.handleStoreRange(request);
}

std::vector<uint8_t>
RepoNode::handleCommitRanges(const std::vector<uint8_t>& request)
{
  return m_core.handleCommitRanges(request);
}

std::vector<uint8_t>
RepoNode::handleAbortRanges(const std::vector<uint8_t>& request)
{
  m_core.abortRanges(decodeRangeAbortRequest(request));
  return toBytes("aborted");
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
RepoNode::handleFetchRange(const std::vector<uint8_t>& request) const
{
  return m_core.handleFetchRange(request);
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
