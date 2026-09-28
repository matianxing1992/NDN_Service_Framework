#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include "ndn-service-framework/LocalServiceRegistry.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <chrono>
#include <exception>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ndnsf_distributed_repo {

namespace {

std::vector<uint8_t>
payloadOf(const ndn_service_framework::RequestMessage& request)
{
  const auto payload = request.getPayload();
  return std::vector<uint8_t>(payload.begin(), payload.end());
}

std::string
digestHex(const std::vector<uint8_t>& bytes)
{
  unsigned char digest[SHA256_DIGEST_LENGTH]{};
  SHA256(bytes.data(), bytes.size(), digest);
  static constexpr char HEX[] = "0123456789abcdef";
  std::string result;
  result.reserve(SHA256_DIGEST_LENGTH * 2);
  for (const auto value : digest) {
    result.push_back(HEX[value >> 4]);
    result.push_back(HEX[value & 0x0f]);
  }
  return result;
}

uint64_t
nowMilliseconds()
{
  const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  return value <= 0 ? 1 : static_cast<uint64_t>(value);
}

RepoObjectManifest
artifactManifest(const ArtifactReference& reference,
                 const std::string& operationId, uint64_t generation)
{
  RepoObjectManifest manifest;
  manifest.objectName = artifactStorageObjectName(reference);
  manifest.objectType = "artifact-v2-bytes";
  manifest.sha256 = reference.contentDigest;
  std::transform(manifest.sha256.begin(), manifest.sha256.end(),
                 manifest.sha256.begin(), [] (unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  manifest.size = reference.sizeBytes;
  manifest.segmentCount = 1;
  manifest.replicationFactor = 1;
  manifest.policyEpoch = reference.policyEpoch;
  manifest.generation = generation;
  manifest.writeConsistency = "ONE";
  manifest.requiredWriteAcks = 1;
  manifest.operationId = operationId;
  manifest.lifecycleState = "COMMITTED";
  return manifest;
}

std::string
resolveRepoNode(const std::string& coreRepoNode, std::string configuredRepoNode)
{
  if (configuredRepoNode.empty()) {
    return coreRepoNode;
  }
  if (coreRepoNode.empty() || configuredRepoNode != coreRepoNode) {
    throw std::invalid_argument(
      "repo-artifact-identity-mismatch: artifact and RepoCore logical Repo IDs must match");
  }
  return configuredRepoNode;
}

void
verifyArtifactBytes(const RepoCore& core, const RepoObjectManifest& manifest,
                    const ArtifactReference& reference)
{
  std::string expectedDigest = reference.contentDigest;
  std::transform(expectedDigest.begin(), expectedDigest.end(), expectedDigest.begin(),
                 [] (unsigned char value) { return static_cast<char>(std::tolower(value)); });
  if (!core.has(manifest.objectName) || manifest.size != reference.sizeBytes ||
      manifest.sha256 != expectedDigest) {
    throw std::runtime_error(
      "repo-artifact-byte-verification-failed: stored manifest does not match full reference");
  }

  EVP_MD_CTX* rawContext = EVP_MD_CTX_new();
  if (rawContext == nullptr || EVP_DigestInit_ex(rawContext, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(rawContext);
    throw std::runtime_error("repo-artifact-digest-failed: cannot initialize SHA-256");
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(rawContext, EVP_MD_CTX_free);
  constexpr uint64_t VERIFY_RANGE_BYTES = 8 * 1024 * 1024;
  uint64_t offset = 0;
  while (offset < reference.sizeBytes) {
    const auto length = std::min<uint64_t>(VERIFY_RANGE_BYTES, reference.sizeBytes - offset);
    const auto bytes = core.getRange(manifest.objectName, RepoByteRange{offset, length});
    if (bytes.size() != length || EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1) {
      throw std::runtime_error(
        "repo-artifact-byte-verification-failed: stored range length or digest update is invalid");
    }
    offset += length;
  }
  unsigned char digest[EVP_MAX_MD_SIZE]{};
  unsigned int digestSize = 0;
  if (EVP_DigestFinal_ex(context.get(), digest, &digestSize) != 1 || digestSize != 32) {
    throw std::runtime_error("repo-artifact-digest-failed: SHA-256 finalization failed");
  }
  static constexpr char HEX[] = "0123456789abcdef";
  std::string actualDigest;
  actualDigest.reserve(digestSize * 2);
  for (unsigned int i = 0; i < digestSize; ++i) {
    actualDigest.push_back(HEX[digest[i] >> 4]);
    actualDigest.push_back(HEX[digest[i] & 0x0f]);
  }
  if (actualDigest != expectedDigest) {
    throw std::runtime_error(
      "repo-artifact-byte-verification-failed: stored bytes do not match full reference digest");
  }
}

} // namespace

RepoNode::RepoNode(ndn::Name servicePrefix,
                   StorageCapability capability,
                   std::shared_ptr<RepoStoreBackend> store,
                   std::string repoNode,
                   std::string providerIdentity,
                   std::string artifactMetadataPath,
                   ArtifactLimits artifactLimits)
  : m_servicePrefix(std::move(servicePrefix))
  , m_repoNode(resolveRepoNode(capability.repoNode, std::move(repoNode)))
  , m_providerIdentity(std::move(providerIdentity))
  , m_artifactLimits(std::move(artifactLimits))
  , m_core(std::move(capability), std::move(store))
{
  if (m_servicePrefix.empty()) {
    throw std::invalid_argument("repo-service-prefix-invalid: service prefix is required");
  }
  if (!artifactMetadataPath.empty()) {
    validateName(m_repoNode, m_artifactLimits, "repoNode");
    validateName(m_providerIdentity, m_artifactLimits, "providerIdentity");
    m_artifactMetadataStore = std::make_unique<SqliteArtifactMetadataStore>(
      std::move(artifactMetadataPath), true, 14, true);
    if (m_artifactMetadataStore->schemaGeneration() != 14 ||
        !m_artifactMetadataStore->artifactWritesEnabled()) {
      throw std::runtime_error("repo-artifact-catalog-unavailable: Persistent catalog must be writable at generation 14");
    }
  }
}

RepoNode::~RepoNode()
{
  stopRangeReservationReaper();
}

void
RepoNode::startRangeReservationReaper()
{
  if (!m_artifactMetadataStore) {
    return;
  }
  std::lock_guard<std::mutex> guard(m_rangeReaperMutex);
  if (m_rangeReaperThread.joinable()) {
    return;
  }
  m_stopRangeReaper = false;
  m_rangeReaperThread = std::thread([this] {
    constexpr auto ReapInterval = std::chrono::seconds(30);
    constexpr auto IdleTimeout = std::chrono::minutes(10);
    std::unique_lock<std::mutex> lock(m_rangeReaperMutex);
    while (!m_rangeReaperWake.wait_for(lock, ReapInterval,
                                       [this] { return m_stopRangeReaper; })) {
      lock.unlock();
      try {
        (void)m_core.reapExpiredRangeReservations(IdleTimeout);
      }
      catch (...) {
        // A failed cleanup pass leaves the reservation owned and retryable.
      }
      lock.lock();
    }
  });
}

void
RepoNode::stopRangeReservationReaper() noexcept
{
  {
    std::lock_guard<std::mutex> guard(m_rangeReaperMutex);
    m_stopRangeReaper = true;
  }
  m_rangeReaperWake.notify_all();
  if (m_rangeReaperThread.joinable()) {
    m_rangeReaperThread.join();
  }
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
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.put(objectName, payload, objectType, replicationFactor,
                    policyEpoch, std::move(replicaNodes));
}

RepoObjectManifest
RepoNode::putManifest(const RepoObjectManifest& manifest)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.putManifest(manifest);
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
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  m_core.putRange(manifest, range, bytes);
}

RepoObjectManifest
RepoNode::commitRanges(const RepoObjectManifest& manifest)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.commitRanges(manifest);
}

void
RepoNode::abortRanges(const std::string& objectName)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
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
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.remove(objectName);
}

RepoObjectManifest
RepoNode::putDataPacket(const std::string& dataName,
                        const std::vector<uint8_t>& wire)
{
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
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
  const auto self = weak_from_this().lock();
  if (!self) {
    throw std::logic_error(
      "RepoNode network service registration requires shared ownership");
  }
  auto ack = [] (const ndn_service_framework::RequestMessage&) {
    ndn_service_framework::ServiceProvider::AckDecision decision;
    decision.status = true;
    decision.message = "repo-ready";
    return decision;
  };

  const auto registerRepoService =
    [&provider, &ack, self](
      const ndn::Name& serviceName,
      ndn_service_framework::ServiceProvider::RequestHandler handler,
      ndn_service_framework::ServiceProvider::AckStrategyHandler serviceAck = {}) {
      if (!serviceAck) {
        serviceAck = ack;
      }
      ndn_service_framework::ServiceProvider::RequestHandler retainedHandler =
        [self, handler = std::move(handler)](
          const ndn::Name& requesterIdentity,
          const ndn::Name& providerName,
          const ndn::Name& resolvedServiceName,
          const ndn::Name& requestId,
          const ndn_service_framework::RequestMessage& request) {
          (void)self;
          return handler(requesterIdentity, providerName, resolvedServiceName,
                         requestId, request);
        };
      provider.addService(serviceName, std::move(serviceAck), std::move(retainedHandler));
    };

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "STORE"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStore(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "STORE_RANGE"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStoreRange(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "COMMIT_RANGES"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleCommitRanges(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "ABORT_RANGES"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleAbortRanges(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "INSERT"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleInsert(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "STORE_MANIFEST"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStoreManifest(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "FETCH"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleFetch(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "FETCH_RANGE"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleFetchRange(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "MANIFEST"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleManifest(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "INVENTORY"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleInventory());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "CAPABILITY"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCapability());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    },
    [this] (const ndn_service_framework::RequestMessage&) {
      ndn_service_framework::ServiceProvider::AckDecision decision;
      decision.status = true;
      decision.message = "repo-artifact-capability-v1";
      // A fresh capability inquiry is the maintenance boundary before a new
      // placement operation. The periodic owner sweep bounds abandoned client
      // staging; backend startup also removes orphaned staging after a Repo
      // process restart.
      (void)m_core.reapExpiredRangeReservations(std::chrono::minutes(10));
      const auto wire = ArtifactCapabilityAck{1, artifactCapability()}.toJson();
      decision.payload = ndn::Buffer(
        reinterpret_cast<const uint8_t*>(wire.data()), wire.size());
      return decision;
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "CACHE_STATUS"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCacheStatus());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "STATUS"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleStatus(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "CATALOG_STATUS"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCatalogStatus());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "CATALOG_SNAPSHOT"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage&) {
      try {
        return makeResponse(handleCatalogSnapshot());
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "CATALOG_DELTA"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleCatalogDelta(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "CATALOG_LOOKUP"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleCatalogLookup(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  registerRepoService(
    makeRepoServiceName(m_servicePrefix, "DELETE"),
    [this] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
            const ndn_service_framework::RequestMessage& request) {
      try {
        return makeResponse(handleDelete(payloadOf(request)));
      }
      catch (const std::exception& e) {
        return makeError(e.what());
      }
    });

  startRangeReservationReaper();
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
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
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

void
RepoNode::validatePersistentWriteIdentity(const ArtifactReference& reference,
                                          const std::string& operationId,
                                          uint64_t generation) const
{
  if (!m_artifactMetadataStore || m_repoNode.empty() || m_providerIdentity.empty()) {
    throw std::runtime_error(
      "repo-artifact-identity-unconfigured: Persistent receipt owner is unavailable");
  }
  if (const auto existing = m_artifactMetadataStore->lookupPersistentLocation(
        reference, m_repoNode, m_providerIdentity, 0)) {
    if (existing->operationId != operationId || existing->generation != generation) {
      throw std::runtime_error(
        "repo-artifact-receipt-conflict: immutable reference already has another commit");
    }
  }

  const auto expected = artifactManifest(reference, operationId, generation);
  if (m_core.has(expected.objectName)) {
    const auto stored = m_core.getManifest(expected.objectName);
    if (stored.objectType != expected.objectType || stored.sha256 != expected.sha256 ||
        stored.size != expected.size || stored.policyEpoch != expected.policyEpoch ||
        (!stored.operationId.empty() && stored.operationId != operationId) ||
        (stored.generation != 0 && stored.generation != generation)) {
      throw std::runtime_error(
        "repo-artifact-byte-commit-conflict: stored manifest is bound to another identity");
    }
  }
}

std::vector<uint8_t>
RepoNode::handleStore(const std::vector<uint8_t>& request)
{
  static constexpr uint8_t MAGIC[] = {'A', 'R', 'V', '2', 'P', 'U', 'T', '1'};
  if (request.size() >= sizeof(MAGIC) &&
      std::equal(std::begin(MAGIC), std::end(MAGIC), request.begin())) {
    const auto write = ArtifactStoreRequest::fromWire(request, m_artifactLimits);
    std::string expectedDigest = write.reference.contentDigest;
    std::transform(expectedDigest.begin(), expectedDigest.end(), expectedDigest.begin(),
                   [] (unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (digestHex(write.bytes) != expectedDigest) {
      throw std::runtime_error(
        "repo-artifact-byte-verification-failed: STORE payload does not match full reference digest");
    }
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    validatePersistentWriteIdentity(write.reference, write.operationId,
                                    write.generation);
    const auto manifest = artifactManifest(write.reference, write.operationId,
                                           write.generation);
    (void)m_core.handleStore(encodeStoreRequest(manifest, write.bytes));
    ArtifactRangeCommitRequest commit;
    commit.operationId = write.operationId;
    commit.generation = write.generation;
    commit.reference = write.reference;
    return handlePersistentCommitRanges(commit, true);
  }
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.handleStore(request);
}

std::vector<uint8_t>
RepoNode::handleStoreRange(const std::vector<uint8_t>& request)
{
  static constexpr uint8_t MAGIC[] = {'A', 'R', 'V', '2', 'R', 'N', 'G', '1'};
  if (request.size() >= sizeof(MAGIC) &&
      std::equal(std::begin(MAGIC), std::end(MAGIC), request.begin())) {
    const auto write = ArtifactRangeWriteRequest::fromWire(request, m_artifactLimits);
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    validatePersistentWriteIdentity(write.reference, write.operationId,
                                    write.generation);
    auto manifest = artifactManifest(write.reference, write.operationId, write.generation);
    m_core.putRange(manifest,
                    RepoByteRange{write.offsetBytes, write.lengthBytes},
                    write.bytes);
    return toBytes("staged");
  }
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.handleStoreRange(request);
}

std::vector<uint8_t>
RepoNode::handleCommitRanges(const std::vector<uint8_t>& request)
{
  static constexpr uint8_t MAGIC[] = {'A', 'R', 'V', '2', 'C', 'O', 'M', '1'};
  if (request.size() >= sizeof(MAGIC) &&
      std::equal(std::begin(MAGIC), std::end(MAGIC), request.begin())) {
    const auto commit = ArtifactRangeCommitRequest::fromWire(request, m_artifactLimits);
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    return handlePersistentCommitRanges(commit, false);
  }
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.handleCommitRanges(request);
}

std::vector<uint8_t>
RepoNode::handlePersistentCommitRanges(const ArtifactRangeCommitRequest& commit,
                                       bool payloadAlreadyCommitted)
{
  validatePersistentWriteIdentity(commit.reference, commit.operationId,
                                   commit.generation);
  auto manifest = artifactManifest(commit.reference, commit.operationId, commit.generation);
  if (!m_core.has(manifest.objectName)) {
    if (commit.reference.sizeBytes == 0) {
      manifest = m_core.put(manifest.objectName, {}, manifest.objectType, 1,
                            manifest.policyEpoch);
      manifest.generation = commit.generation;
      manifest.operationId = commit.operationId;
    }
    else {
      m_core.commitRanges(manifest);
    }
  }
  else if (commit.reference.sizeBytes != 0 && !payloadAlreadyCommitted) {
    // A range retry against an already committed payload still creates a
    // Core reservation, even though the backend can satisfy the bytes from
    // its immutable generation. Run the idempotent commit path to clear it.
    m_core.commitRanges(manifest);
  }
  const auto stored = m_core.getManifest(manifest.objectName);
  if (stored.sha256 != manifest.sha256 || stored.size != commit.reference.sizeBytes ||
      (!stored.operationId.empty() && stored.operationId != commit.operationId) ||
      (stored.generation != 0 && stored.generation != commit.generation)) {
    throw std::runtime_error(
      "repo-artifact-byte-commit-conflict: durable object manifest differs from request");
  }

  verifyArtifactBytes(m_core, stored, commit.reference);
  if (stored.operationId.empty() || stored.generation == 0) {
    m_core.putManifest(manifest);
  }
  const auto boundManifest = m_core.getManifest(manifest.objectName);
  if (boundManifest.operationId != commit.operationId ||
      boundManifest.generation != commit.generation ||
      boundManifest.size != commit.reference.sizeBytes ||
      boundManifest.sha256 != manifest.sha256) {
    throw std::runtime_error(
      "repo-artifact-manifest-binding-failed: durable manifest lacks commit operation or generation");
  }

  if (const auto existing = m_artifactMetadataStore->lookupPersistentLocation(
        commit.reference, m_repoNode, m_providerIdentity, 0)) {
    if (existing->operationId != commit.operationId ||
        existing->generation != commit.generation) {
      throw std::runtime_error(
        "repo-artifact-receipt-conflict: immutable reference already has another commit");
    }
    return toBytes(existing->toJson());
  }

  ArtifactLocationRecord location;
  location.repoNode = m_repoNode;
  location.providerIdentity = m_providerIdentity;
  location.operationId = commit.operationId;
  const auto referenceWire = commit.reference.toJson();
  const auto receiptMaterial = m_repoNode + "\n" + m_providerIdentity + "\n" +
                               commit.operationId + "\n" + referenceWire;
  const std::vector<uint8_t> receiptBytes(receiptMaterial.begin(), receiptMaterial.end());
  location.receiptId = "repo-artifact-" + digestHex(receiptBytes);
  location.reference = commit.reference;
  location.generation = commit.generation;
  location.locationVersion = 1;
  location.committedAtMs = nowMilliseconds();
  location.expiresAtMs = location.committedAtMs + 60 * 60 * 1000;
  location.routeState = "ROUTABLE";
  location.forwardingHint = m_servicePrefix.toUri();
  m_artifactMetadataStore->recordPersistentCommit(location);
  return toBytes(location.toJson());
}

std::vector<uint8_t>
RepoNode::handleAbortRanges(const std::vector<uint8_t>& request)
{
  static constexpr uint8_t MAGIC[] = {'A', 'R', 'V', '2', 'A', 'B', 'T', '1'};
  if (request.size() >= sizeof(MAGIC) &&
      std::equal(std::begin(MAGIC), std::end(MAGIC), request.begin())) {
    if (!m_artifactMetadataStore || m_repoNode.empty() || m_providerIdentity.empty()) {
      throw std::runtime_error(
        "repo-artifact-identity-unconfigured: Persistent range owner is unavailable");
    }
    const auto abort = ArtifactRangeAbortRequest::fromWire(request, m_artifactLimits);
    std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
    validatePersistentWriteIdentity(abort.reference, abort.operationId,
                                    abort.generation);
    const auto manifest = artifactManifest(abort.reference, abort.operationId,
                                           abort.generation);
    (void)m_core.abortRangesIfOwned(manifest);
    return toBytes("aborted");
  }
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
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
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
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

ArtifactCapability
RepoNode::artifactCapability() const
{
  ArtifactCapability capability;
  capability.repoNode = m_repoNode;
  capability.formatVersions = {"artifact-manifest-v2", "exact-packet-v1"};
  capability.digestAlgorithms = {"sha256"};
  capability.signatureAlgorithms = {"rsa-sha256", "ecdsa-sha256", "ed25519"};
  capability.maxArtifactBytes = m_artifactLimits.maxArtifactBytes;
  capability.maxChunkBytes = m_artifactLimits.maxChunkBytes;
  capability.maxRootEncodedBytes = m_artifactLimits.maxRootEncodedBytes;
  capability.maxPageEncodedBytes = m_artifactLimits.maxPageEncodedBytes;
  capability.maxPageEntries = m_artifactLimits.maxPageEntries;
  capability.maxManifestDepth = m_artifactLimits.maxManifestDepth;
  capability.supportsResume = true;
  capability.supportsReplicaReceipts = static_cast<bool>(m_artifactMetadataStore);
  capability.policyEpoch = "repo-policy-v1";
  return capability;
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
  std::lock_guard<std::mutex> writeGuard(m_storeWriteMutex);
  return m_core.handleDelete(request);
}

std::vector<uint8_t>
RepoNode::handleStatus(const std::vector<uint8_t>& request) const
{
  const auto operationId = toString(request);
  if (!request.empty() && request.front() == '{') {
    if (!m_artifactMetadataStore || m_repoNode.empty() || m_providerIdentity.empty()) {
      throw std::runtime_error(
        "repo-artifact-location-unavailable: Persistent receipt owner is not configured");
    }
    const auto query = ArtifactLocationRequest::fromJson(operationId, m_artifactLimits);
    const auto now = nowMilliseconds();
    auto location = m_artifactMetadataStore->lookupPersistentLocation(
      query.reference, m_repoNode, m_providerIdentity, now);
    if (!location) {
      throw std::out_of_range("repo-artifact-location-not-found: no current Repo-local location");
    }
    const auto manifest = m_core.getManifest(artifactStorageObjectName(query.reference));
    verifyArtifactBytes(m_core, manifest, query.reference);
    if (manifest.operationId != location->operationId ||
        manifest.generation != location->generation) {
      throw std::runtime_error(
        "repo-artifact-location-invalid: stored manifest does not match receipt operation and generation");
    }
    return toBytes(location->toJson());
  }
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
