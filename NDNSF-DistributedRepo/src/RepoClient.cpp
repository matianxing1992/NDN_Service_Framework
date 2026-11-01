#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"

#include "ndn-service-framework/LocalServiceRegistry.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/util/segmenter.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace ndnsf_distributed_repo {

namespace {
std::string
toLowerHex(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

void
validateUserStorageClientTarget(const RepoArtifactTarget& target, int timeoutMs)
{
  if (timeoutMs <= 0) throw std::invalid_argument("repo-user-storage-invalid: timeout");
  for (const auto* value : {&target.repoNode, &target.providerIdentity, &target.servicePrefix}) {
    if (value->empty() || value->size() > 4096 || value->front() != '/')
      throw std::invalid_argument("repo-user-storage-invalid: target");
    const ndn::Name name(*value);
    if (name.empty() || name.toUri(ndn::name::UriFormat::ALTERNATE) != *value)
      throw std::invalid_argument("repo-user-storage-invalid: target");
  }
}

template<typename Validator>
ndn_service_framework::ServiceUser::ResponseHandler
checkedUserStorageResponse(Validator validate,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return [validate = std::move(validate), onResponse = std::move(onResponse)](
    const ndn_service_framework::ResponseMessage& response) {
    if (!onResponse) return;
    if (!response.getStatus()) { onResponse(response); return; }
    std::optional<ndn_service_framework::ResponseMessage> invalid;
    try {
      const auto payload = response.getPayload();
      validate(std::string(payload.begin(), payload.end()));
    }
    catch (const std::exception& e) {
      invalid.emplace();
      invalid->setStatus(false);
      invalid->setErrorInfo(e.what());
      ndn::Buffer payload(reinterpret_cast<const uint8_t*>(e.what()), std::strlen(e.what()));
      invalid->setPayload(payload, payload.size());
    }
    // A user callback exception must not be treated as validation failure or
    // cause the callback to run a second time.
    onResponse(invalid ? *invalid : response);
  };
}
} // namespace

ndn::Name
RepoClient::requestUserStorageGrant(ndn_service_framework::ServiceUser& user,
  const RepoArtifactTarget& target, const RepoUserStorageGrantRequest& request,
  int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  validateUserStorageClientTarget(target, timeoutMs);
  const auto payload = toBytes(request.toJson());
  const auto owner = user.getName().toUri(ndn::name::UriFormat::ALTERNATE);
  auto checked = checkedUserStorageResponse([target, owner, request](const std::string& wire) {
    RepoUserStorageGrantReply::fromJson(wire).validateExpected(target, owner, request);
  }, std::move(onResponse));
  return user.RequestService(std::vector<ndn::Name>{ndn::Name(target.providerIdentity)},
    makeRepoServiceName(ndn::Name(target.servicePrefix), "USER_STORAGE_GRANT"),
    makeRequest(payload), timeoutMs, std::move(onTimeout), std::move(checked),
    ndn_service_framework::tlv::FirstResponding);
}

ndn::Name
RepoClient::requestUserStorageList(ndn_service_framework::ServiceUser& user,
  const RepoArtifactTarget& target, const RepoUserStorageListRequest& request,
  int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  validateUserStorageClientTarget(target, timeoutMs);
  const auto payload = toBytes(request.toJson());
  const auto owner = user.getName().toUri(ndn::name::UriFormat::ALTERNATE);
  auto checked = checkedUserStorageResponse([target, owner, request](const std::string& wire) {
    RepoUserStorageListReply::fromJson(wire).validateExpected(target, owner, request);
  }, std::move(onResponse));
  return user.RequestService(std::vector<ndn::Name>{ndn::Name(target.providerIdentity)},
    makeRepoServiceName(ndn::Name(target.servicePrefix), "USER_STORAGE_LIST"),
    makeRequest(payload), timeoutMs, std::move(onTimeout), std::move(checked),
    ndn_service_framework::tlv::FirstResponding);
}

ndn::Name
RepoClient::requestUserStorageDelete(ndn_service_framework::ServiceUser& user,
  const RepoArtifactTarget& target, const RepoUserStorageDeleteRequest& request,
  int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  validateUserStorageClientTarget(target, timeoutMs);
  const auto payload = toBytes(request.toJson());
  const auto owner = user.getName().toUri(ndn::name::UriFormat::ALTERNATE);
  auto checked = checkedUserStorageResponse([target, owner, request](const std::string& wire) {
    RepoUserStorageDeleteReply::fromJson(wire).validateExpected(target, owner, request);
  }, std::move(onResponse));
  return user.RequestService(std::vector<ndn::Name>{ndn::Name(target.providerIdentity)},
    makeRepoServiceName(ndn::Name(target.servicePrefix), "USER_STORAGE_DELETE"),
    makeRequest(payload), timeoutMs, std::move(onTimeout), std::move(checked),
    ndn_service_framework::tlv::FirstResponding);
}

ndn::Name
RepoClient::requestUserStorageUsage(ndn_service_framework::ServiceUser& user,
  const RepoArtifactTarget& target, const RepoUserStorageUsageRequest& request,
  int timeoutMs, ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  validateUserStorageClientTarget(target, timeoutMs);
  const auto payload = toBytes(request.toJson());
  const auto owner = user.getName().toUri(ndn::name::UriFormat::ALTERNATE);
  auto checked = checkedUserStorageResponse([target, owner](const std::string& wire) {
    RepoUserStorageUsageReply::fromJson(wire).validateExpected(target, owner);
  }, std::move(onResponse));
  return user.RequestService(std::vector<ndn::Name>{ndn::Name(target.providerIdentity)},
    makeRepoServiceName(ndn::Name(target.servicePrefix), "USER_STORAGE_USAGE"),
    makeRequest(payload), timeoutMs, std::move(onTimeout), std::move(checked),
    ndn_service_framework::tlv::FirstResponding);
}

std::vector<uint8_t>
RepoClient::fetchOriginalDataPacket(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& dataName,
  const std::string& expectedWireSha256,
  const std::vector<OriginalDataReplica>& replicas,
  const std::function<bool(const ndn::Data&)>& verifyProducerSignature,
  int timeoutMs)
{
  if (dataName.empty() || expectedWireSha256.size() != 64 || replicas.empty() ||
      !verifyProducerSignature || timeoutMs <= 0) {
    throw std::invalid_argument("repo-original-data-request-invalid");
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  std::set<std::string> visitedProviders;
  std::string lastError = "no committed, current Repo location";
  for (size_t index = 0; index < replicas.size(); ++index) {
    const auto& replica = replicas[index];
    if (replica.repoNode.empty() || replica.providerIdentity.empty() ||
        replica.servicePrefix.empty() ||
        !visitedProviders.insert(replica.providerIdentity.toUri()).second) {
      continue;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now()).count();
    if (remaining < 3000) break;
    const auto candidateBudget = remaining / static_cast<int64_t>(replicas.size() - index);
    if (candidateBudget < 3000) break;
    const auto candidateDeadline = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(candidateBudget);
    const RepoDataPacketQuery query{1, dataName.toUri(ndn::name::UriFormat::CANONICAL), expectedWireSha256};
    const auto request = toBytes(query.toJson());
    bool originalInterestExpressed = false;
    try {
      const int probeBudget = static_cast<int>(candidateBudget / 3);
      const int ackBudget = std::max(1, std::min(250, probeBudget / 4));
      const auto receiptWire = requestSelectedService(
        user, replica.providerIdentity,
        makeRepoServiceName(replica.servicePrefix, "DATA_RECEIPT"),
        request, ackBudget, probeBudget);
      const auto receipt = RepoWriteReceipt::dataPacketFromJson(toString(receiptWire));
      if (receipt.objectName != query.dataName ||
          receipt.digest != query.expectedWireSha256 ||
          receipt.repoNode != replica.repoNode ||
          receipt.providerIdentity != replica.providerIdentity.toUri()) {
        throw std::runtime_error("repo-original-data-receipt-mismatch");
      }
      const auto locationWire = requestSelectedService(
        user, replica.providerIdentity,
        makeRepoServiceName(replica.servicePrefix, "DATA_LOCATION"),
        request, ackBudget, probeBudget);
      const auto location = RepoDataPacketLocation::fromJson(toString(locationWire));
      const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
      if (location.receiptId != receipt.operationId ||
          location.repoNode != receipt.repoNode ||
          location.providerIdentity != receipt.providerIdentity ||
          location.dataName != query.dataName ||
          location.wireSha256 != query.expectedWireSha256 ||
          location.state != "PRESENT" ||
          location.forwardingHint != receipt.providerIdentity ||
          location.observedAtMs > static_cast<uint64_t>(nowMs + 5000) ||
          location.expiresAtMs <= static_cast<uint64_t>(nowMs)) {
        throw std::runtime_error("repo-original-data-location-not-current");
      }

      ndn::Face face;
      ndn::Interest interest(dataName);
      interest.setCanBePrefix(false);
      interest.setMustBeFresh(true);
      interest.setForwardingHint({replica.providerIdentity});
      const auto fetchRemaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        candidateDeadline - std::chrono::steady_clock::now()).count();
      if (fetchRemaining <= 0) throw std::runtime_error("repo-original-data-deadline");
      interest.setInterestLifetime(ndn::time::milliseconds(fetchRemaining));
      bool done = false;
      std::vector<uint8_t> wire;
      std::string fetchError;
      auto pending = face.expressInterest(
        interest,
        [&] (const ndn::Interest&, const ndn::Data& data) {
          if (data.getName() != dataName) {
            fetchError = "repo-original-data-name-mismatch";
          }
          else {
            const auto& block = data.wireEncode();
            wire.assign(block.begin(), block.end());
          }
          done = true;
        },
        [&] (const ndn::Interest&, const ndn::lp::Nack&) {
          fetchError = "repo-original-data-nack";
          done = true;
        },
        [&] (const ndn::Interest&) {
          fetchError = "repo-original-data-timeout";
          done = true;
        });
      originalInterestExpressed = true;
      while (!done && std::chrono::steady_clock::now() < candidateDeadline) {
        face.processEvents(ndn::time::milliseconds(25));
        face.getIoContext().restart();
      }
      if (!done || !fetchError.empty()) {
        throw std::runtime_error(fetchError.empty() ?
          "repo-original-data-deadline" : fetchError);
      }
      if (wire.size() != receipt.persistedBytes ||
          sha256Hex(wire) != expectedWireSha256) {
        throw std::runtime_error("repo-original-data-wire-mismatch");
      }
      ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
      block.parse();
      const ndn::Data data(block);
      if (data.getName() != dataName || !verifyProducerSignature(data)) {
        throw std::runtime_error("repo-original-data-producer-signature-invalid");
      }
      return wire;
    }
    catch (const std::exception& error) {
      lastError = error.what();
      if (originalInterestExpressed) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
        if (remaining > 1500) {
          try {
            const int recheckBudget = static_cast<int>(std::min<int64_t>(3000, remaining / 3));
            (void)RepoDataPacketLocation::fromJson(toString(requestSelectedService(
              user, replica.providerIdentity,
              makeRepoServiceName(replica.servicePrefix, "DATA_LOCATION"),
              request, std::min(250, recheckBudget / 4), recheckBudget)));
          }
          catch (const std::exception&) {
            // A failed recheck cannot make the old location usable; continue
            // with a distinct candidate under the original deadline.
          }
        }
      }
    }
  }
  throw std::runtime_error("repo-original-data-unavailable: " + lastError);
}

std::vector<uint8_t>
RepoClient::fetchOriginalDataPacketFromUserStorage(
  ndn_service_framework::ServiceUser& user,
  const RepoArtifactTarget& target,
  const ndn::Name& dataName,
  uint64_t expectedWireBytes,
  const std::function<bool(const ndn::Data&)>& verifyProducerSignature,
  int timeoutMs)
{
  validateUserStorageClientTarget(target, timeoutMs);
  if (dataName.empty() || expectedWireBytes == 0 ||
      expectedWireBytes > ndn::MAX_NDN_PACKET_SIZE ||
      !verifyProducerSignature) {
    throw std::invalid_argument("repo-user-storage-packet-request-invalid");
  }
  if (user.isOnIoThread()) {
    throw std::logic_error(
      "repo-user-storage-packet-request-on-io-thread: blocking packet fetch cannot run on the Face I/O thread");
  }

  struct FetchState
  {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    std::string error;
    std::vector<uint8_t> wire;
    std::optional<ndn::PendingInterestHandle> pending;
  };

  const auto state = std::make_shared<FetchState>();
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  ndn::Interest interest(dataName);
  interest.setCanBePrefix(false);
  interest.setMustBeFresh(true);
  interest.setForwardingHint({ndn::Name(target.providerIdentity)});
  interest.setInterestLifetime(ndn::time::milliseconds(timeoutMs));

  const auto finish = [state] (std::string error, std::vector<uint8_t> wire) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->done) return;
      state->error = std::move(error);
      state->wire = std::move(wire);
      state->pending.reset();
      state->done = true;
    }
    state->condition.notify_all();
  };

  user.postToIoWithFace(
    [state, interest = std::move(interest), dataName, expectedWireBytes, finish]
    (ndn::Face& face) mutable {
      std::unique_lock<std::mutex> lock(state->mutex);
      if (state->done) return;
      try {
        state->pending.emplace(face.expressInterest(
          interest,
          [dataName, expectedWireBytes, finish] (const ndn::Interest&, const ndn::Data& data) {
            if (data.getName() != dataName) {
              finish("repo-user-storage-packet-name-mismatch", {});
              return;
            }
            const auto& block = data.wireEncode();
            if (block.size() != expectedWireBytes) {
              finish("repo-user-storage-packet-size-mismatch", {});
              return;
            }
            finish({}, std::vector<uint8_t>(block.begin(), block.end()));
          },
          [finish] (const ndn::Interest&, const ndn::lp::Nack&) {
            finish("repo-user-storage-packet-nack", {});
          },
          [finish] (const ndn::Interest&) {
            finish("repo-user-storage-packet-timeout", {});
          }));
      }
      catch (const std::exception& e) {
        lock.unlock();
        finish(std::string("repo-user-storage-packet-express-failed: ") + e.what(), {});
      }
      catch (...) {
        lock.unlock();
        finish("repo-user-storage-packet-express-failed", {});
      }
    });

  std::vector<uint8_t> wire;
  std::string fetchError;
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_until(lock, deadline, [&] { return state->done; })) {
      state->done = true;
      state->error = "repo-user-storage-packet-deadline";
      if (state->pending) {
        state->pending->cancel();
        state->pending.reset();
      }
    }
    fetchError = state->error;
    wire = std::move(state->wire);
  }
  if (!fetchError.empty()) {
    throw std::runtime_error(fetchError);
  }
  if (wire.size() != expectedWireBytes) {
    throw std::runtime_error("repo-user-storage-packet-size-mismatch");
  }

  ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
  block.parse();
  const ndn::Data data(block);
  if (data.getName() != dataName || !verifyProducerSignature(data)) {
    throw std::runtime_error("repo-user-storage-packet-producer-signature-invalid");
  }
  return wire;
}

VerifiedRepoUserStoragePacketSetV1
RepoClient::fetchOriginalDataPacketSetFromUserStorage(
  ndn_service_framework::ServiceUser& owner,
  const RepoArtifactTarget& target,
  const std::string& objectName,
  const std::function<bool(const ndn::Data&)>& verifyProducerSignature,
  int timeoutMs,
  RepoUserStoragePacketSetFetchLimits limits)
{
  validateUserStorageClientTarget(target, timeoutMs);
  if (owner.getName().empty() || objectName.empty() || objectName.size() > 4096 ||
      !verifyProducerSignature || limits.listingPageSize == 0 ||
      limits.listingPageSize > 100 || limits.maxListingPagesPerScan == 0 ||
      limits.maxListingPagesPerScan > 4096 || limits.maxListedObjectsPerScan == 0 ||
      limits.maxListedObjectsPerScan > 1000000 || limits.maxListingWireBytes == 0 ||
      limits.maxListingWireBytes > (64ULL << 20) || limits.maxPacketCount == 0 ||
      limits.maxPacketCount > 65536 || limits.maxPacketWireBytes == 0 ||
      limits.maxPacketWireBytes > REPO_USER_STORAGE_LIMIT_BYTES ||
      limits.maxManifestWireBytes == 0 || limits.maxManifestWireBytes > (16ULL << 20) ||
      limits.ackCollectionTimeMs <= 0 || timeoutMs <= limits.ackCollectionTimeMs) {
    throw std::invalid_argument("repo-user-storage-packet-set-request-invalid");
  }
  if (owner.isOnIoThread()) {
    throw std::logic_error(
      "repo-user-storage-packet-set-on-io-thread: blocking packet-set fetch cannot run on the Face I/O thread");
  }
  const ndn::Name expectedObjectName(objectName);
  if (expectedObjectName.empty() ||
      expectedObjectName.toUri(ndn::name::UriFormat::ALTERNATE) != objectName) {
    throw std::invalid_argument("repo-user-storage-packet-set-object-name-invalid");
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  const auto remainingTimeoutMs = [&] {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= limits.ackCollectionTimeMs)
      throw std::runtime_error("repo-user-storage-packet-set-deadline");
    return static_cast<int>(std::min<int64_t>(remaining, std::numeric_limits<int>::max()));
  };
  const auto ownerIdentity = owner.getName().toUri(ndn::name::UriFormat::ALTERNATE);
  const auto provider = ndn::Name(target.providerIdentity);
  const auto servicePrefix = ndn::Name(target.servicePrefix);
  const auto listService = makeRepoServiceName(servicePrefix, "USER_STORAGE_LIST");
  const auto manifestService = makeRepoServiceName(servicePrefix, "MANIFEST");
  uint64_t listingWireBytes = 0;

  const auto listClaim = [&]() {
    std::string cursor;
    uint64_t scannedObjects = 0;
    for (uint32_t pageIndex = 0; pageIndex < limits.maxListingPagesPerScan; ++pageIndex) {
      RepoUserStorageListRequest request;
      request.afterObjectName = cursor;
      request.limit = limits.listingPageSize;
      const auto response = requestSelectedService(owner, provider, listService,
        toBytes(request.toJson()), limits.ackCollectionTimeMs,
        remainingTimeoutMs());
      if (response.size() > limits.maxListingWireBytes - listingWireBytes)
        throw std::runtime_error("repo-user-storage-list-byte-budget-exceeded");
      listingWireBytes += response.size();
      const auto responseText = toString(response);
      auto reply = RepoUserStorageListReply::fromJson(responseText);
      reply.validateExpected(target, ownerIdentity, request);
      if (reply.toJson() != responseText || reply.objects.size() > request.limit ||
          reply.objects.size() > limits.maxListedObjectsPerScan - scannedObjects) {
        throw std::runtime_error("repo-user-storage-list-page-invalid");
      }
      scannedObjects += reply.objects.size();

      std::string previousName = cursor;
      std::optional<RepoUserStorageClaim> match;
      bool passedTarget = false;
      for (const auto& claim : reply.objects) {
        validateRepoUserStorageObject(claim.object);
        if (claim.repoNode != target.repoNode || claim.ownerIdentity != ownerIdentity ||
            claim.object.objectName <= previousName) {
          throw std::runtime_error("repo-user-storage-list-order-or-scope-invalid");
        }
        previousName = claim.object.objectName;
        if (claim.object.objectName == objectName) match = claim;
        else if (claim.object.objectName > objectName) passedTarget = true;
      }
      if (match) {
        if (match->state != RepoUserStorageClaimState::Committed ||
            match->grantId.empty() || match->ownerEpoch == 0 ||
            match->object.manifestBytes == 0 || match->object.payloadWireBytes == 0) {
          throw std::runtime_error("repo-user-storage-claim-not-currently-committed");
        }
        return *match;
      }
      if (passedTarget) break;
      if (reply.nextAfterObjectName.empty()) break;
      if (reply.objects.empty() || reply.nextAfterObjectName != previousName ||
          reply.nextAfterObjectName <= cursor) {
        throw std::runtime_error("repo-user-storage-list-cursor-invalid");
      }
      cursor = reply.nextAfterObjectName;
    }
    throw std::runtime_error("repo-user-storage-current-claim-not-found-within-bounds");
  };

  const auto initialClaim = listClaim();
  const auto manifestResponse = requestSelectedService(owner, provider, manifestService,
    toBytes(objectName), limits.ackCollectionTimeMs, remainingTimeoutMs());
  if (manifestResponse.empty() || manifestResponse.size() > limits.maxManifestWireBytes ||
      manifestResponse.size() != initialClaim.object.manifestBytes) {
    throw std::runtime_error("repo-user-storage-manifest-size-mismatch");
  }
  const auto manifestText = toString(manifestResponse);
  const auto manifest = parseManifestJson(manifestText);
  const auto throwManifestClaimMismatch = [] (const char* field) {
    throw std::runtime_error(
      std::string("repo-user-storage-manifest-claim-mismatch:") + field);
  };
  if (manifest.toJson() != manifestText)
    throwManifestClaimMismatch("noncanonical-json");
  const ndn::Name manifestObjectName(manifest.objectName);
  if (manifestObjectName != expectedObjectName)
    throwManifestClaimMismatch("object-name");
  if (manifest.objectName != manifestObjectName.toUri(ndn::name::UriFormat::CANONICAL) &&
      manifest.objectName != manifestObjectName.toUri(ndn::name::UriFormat::ALTERNATE))
    throwManifestClaimMismatch("object-name-uri-format");
  if (manifest.objectType != "original-packet-set")
    throwManifestClaimMismatch("object-type");
  if (manifest.lifecycleState != "COMMITTED")
    throwManifestClaimMismatch("lifecycle-state");
  if (manifest.generation != initialClaim.object.generation)
    throwManifestClaimMismatch("generation");
  if (manifest.sha256 != initialClaim.object.wireSha256)
    throwManifestClaimMismatch("wire-digest");
  if (manifest.size != initialClaim.object.payloadWireBytes)
    throwManifestClaimMismatch("payload-size");
  if (manifest.size > limits.maxPacketWireBytes)
    throwManifestClaimMismatch("packet-wire-limit");
  if (manifest.packetNames.empty())
    throwManifestClaimMismatch("empty-packet-names");
  if (manifest.packetNames.size() > limits.maxPacketCount)
    throwManifestClaimMismatch("packet-count-limit");
  if (manifest.packetOffsets.size() != manifest.packetNames.size() + 1)
    throwManifestClaimMismatch("packet-offset-count");
  if (manifest.packetOffsets.front() != 0)
    throwManifestClaimMismatch("first-packet-offset");
  if (manifest.packetOffsets.back() != manifest.size)
    throwManifestClaimMismatch("final-packet-offset");
  if (ndn::Name(manifest.packetNames.front()) != expectedObjectName)
    throwManifestClaimMismatch("root-packet-name");

  std::set<ndn::Name> uniquePacketNames;
  for (const auto& packetNameText : manifest.packetNames) {
    const ndn::Name packetName(packetNameText);
    if (packetName.empty() ||
        (packetName.toUri(ndn::name::UriFormat::CANONICAL) != packetNameText &&
         packetName.toUri(ndn::name::UriFormat::ALTERNATE) != packetNameText) ||
        !uniquePacketNames.insert(packetName).second) {
      throw std::runtime_error("repo-user-storage-manifest-packet-name-invalid");
    }
  }
  for (std::size_t i = 0; i + 1 < manifest.packetOffsets.size(); ++i) {
    if (manifest.packetOffsets[i + 1] <= manifest.packetOffsets[i] ||
        manifest.packetOffsets[i + 1] - manifest.packetOffsets[i] > ndn::MAX_NDN_PACKET_SIZE) {
      throw std::runtime_error("repo-user-storage-manifest-packet-offset-invalid");
    }
  }

  std::vector<std::vector<uint8_t>> packetWires;
  packetWires.reserve(manifest.packetNames.size());
  ndn::util::Sha256 aggregateDigest;
  for (std::size_t i = 0; i < manifest.packetNames.size(); ++i) {
    const auto packetName = ndn::Name(manifest.packetNames[i]);
    const auto expectedWireBytes = manifest.packetOffsets[i + 1] - manifest.packetOffsets[i];
    auto wire = fetchOriginalDataPacketFromUserStorage(owner, target, packetName,
      expectedWireBytes, verifyProducerSignature, remainingTimeoutMs());
    aggregateDigest.update(ndn::span<const uint8_t>(wire.data(), wire.size()));
    packetWires.push_back(std::move(wire));
  }
  if (toLowerHex(aggregateDigest.toString()) != initialClaim.object.wireSha256)
    throw std::runtime_error("repo-user-storage-ordered-wire-digest-mismatch");

  const auto currentClaim = listClaim();
  const auto sameClaim = [](const RepoUserStorageClaim& a, const RepoUserStorageClaim& b) {
    return a.repoNode == b.repoNode && a.ownerIdentity == b.ownerIdentity &&
      a.grantId == b.grantId && a.ownerEpoch == b.ownerEpoch &&
      a.object.objectName == b.object.objectName &&
      a.object.wireSha256 == b.object.wireSha256 &&
      a.object.generation == b.object.generation &&
      a.object.payloadWireBytes == b.object.payloadWireBytes &&
      a.object.manifestBytes == b.object.manifestBytes && a.state == b.state &&
      a.cleanupId == b.cleanupId && a.committedAtMs == b.committedAtMs &&
      a.deletedAtMs == b.deletedAtMs;
  };
  if (!sameClaim(initialClaim, currentClaim))
    throw std::runtime_error("repo-user-storage-owner-claim-changed-during-fetch");

  return VerifiedRepoUserStoragePacketSetV1(target, initialClaim, manifest,
                                             std::move(packetWires));
}

RepoUserStorageClaim
RepoClient::revalidateCurrentUserStorageClaim(
  ndn_service_framework::ServiceUser& owner,
  const RepoArtifactTarget& target,
  const RepoUserStorageClaim& expectedClaim,
  int timeoutMs,
  RepoUserStoragePacketSetFetchLimits limits)
{
  validateUserStorageClientTarget(target, timeoutMs);
  if (owner.getName().empty())
    throw std::invalid_argument("repo-user-storage-claim-revalidation-owner-invalid");
  if (owner.isOnIoThread())
    throw std::logic_error(
      "repo-user-storage-claim-revalidation-on-io-thread: blocking claim recheck cannot run on the Face I/O thread");
  if (limits.listingPageSize == 0 || limits.listingPageSize > 100 ||
      limits.maxListingPagesPerScan == 0 || limits.maxListingPagesPerScan > 4096 ||
      limits.maxListedObjectsPerScan == 0 || limits.maxListedObjectsPerScan > 1000000 ||
      limits.maxListingWireBytes == 0 || limits.maxListingWireBytes > (64ULL << 20) ||
      limits.ackCollectionTimeMs <= 0 || timeoutMs <= limits.ackCollectionTimeMs ||
      expectedClaim.state != RepoUserStorageClaimState::Committed ||
      expectedClaim.repoNode != target.repoNode ||
      expectedClaim.ownerIdentity != owner.getName().toUri(ndn::name::UriFormat::ALTERNATE) ||
      expectedClaim.grantId.empty() || expectedClaim.ownerEpoch == 0 ||
      expectedClaim.object.objectName.empty() || expectedClaim.object.manifestBytes == 0 ||
      expectedClaim.object.payloadWireBytes == 0) {
    throw std::invalid_argument("repo-user-storage-claim-revalidation-request-invalid");
  }
  validateRepoUserStorageObject(expectedClaim.object);

  const auto expectedObjectName = ndn::Name(expectedClaim.object.objectName);
  if (expectedObjectName.empty() ||
      expectedObjectName.toUri(ndn::name::UriFormat::ALTERNATE) != expectedClaim.object.objectName) {
    throw std::invalid_argument("repo-user-storage-claim-revalidation-object-invalid");
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  const auto remainingTimeoutMs = [&] {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= limits.ackCollectionTimeMs)
      throw std::runtime_error("repo-user-storage-claim-revalidation-deadline");
    return static_cast<int>(std::min<int64_t>(remaining, std::numeric_limits<int>::max()));
  };
  const auto sameClaim = [](const RepoUserStorageClaim& a, const RepoUserStorageClaim& b) {
    return a.repoNode == b.repoNode && a.ownerIdentity == b.ownerIdentity &&
      a.grantId == b.grantId && a.ownerEpoch == b.ownerEpoch &&
      a.object.objectName == b.object.objectName &&
      a.object.wireSha256 == b.object.wireSha256 &&
      a.object.generation == b.object.generation &&
      a.object.payloadWireBytes == b.object.payloadWireBytes &&
      a.object.manifestBytes == b.object.manifestBytes && a.state == b.state &&
      a.cleanupId == b.cleanupId && a.committedAtMs == b.committedAtMs &&
      a.deletedAtMs == b.deletedAtMs;
  };

  const auto provider = ndn::Name(target.providerIdentity);
  const auto service = makeRepoServiceName(ndn::Name(target.servicePrefix), "USER_STORAGE_LIST");
  const auto ownerIdentity = owner.getName().toUri(ndn::name::UriFormat::ALTERNATE);
  std::string cursor;
  uint64_t scannedObjects = 0;
  uint64_t listingWireBytes = 0;
  for (uint32_t pageIndex = 0; pageIndex < limits.maxListingPagesPerScan; ++pageIndex) {
    RepoUserStorageListRequest request;
    request.afterObjectName = cursor;
    request.limit = limits.listingPageSize;
    const auto response = requestSelectedService(owner, provider, service,
      toBytes(request.toJson()), limits.ackCollectionTimeMs, remainingTimeoutMs());
    if (response.size() > limits.maxListingWireBytes - listingWireBytes)
      throw std::runtime_error("repo-user-storage-claim-revalidation-byte-budget-exceeded");
    listingWireBytes += response.size();
    const auto responseText = toString(response);
    auto reply = RepoUserStorageListReply::fromJson(responseText);
    reply.validateExpected(target, ownerIdentity, request);
    if (reply.toJson() != responseText || reply.objects.size() > request.limit ||
        reply.objects.size() > limits.maxListedObjectsPerScan - scannedObjects) {
      throw std::runtime_error("repo-user-storage-claim-revalidation-page-invalid");
    }
    scannedObjects += reply.objects.size();

    std::string previousName = cursor;
    std::optional<RepoUserStorageClaim> match;
    bool passedTarget = false;
    for (const auto& claim : reply.objects) {
      validateRepoUserStorageObject(claim.object);
      if (claim.repoNode != target.repoNode || claim.ownerIdentity != ownerIdentity ||
          claim.object.objectName <= previousName) {
        throw std::runtime_error("repo-user-storage-claim-revalidation-order-or-scope-invalid");
      }
      previousName = claim.object.objectName;
      if (claim.object.objectName == expectedClaim.object.objectName) {
        match = claim;
      }
      else if (claim.object.objectName > expectedClaim.object.objectName) {
        passedTarget = true;
      }
    }
    if (match) {
      if (match->state != RepoUserStorageClaimState::Committed || !sameClaim(expectedClaim, *match))
        throw std::runtime_error("repo-user-storage-owner-claim-changed-after-semantic-validation");
      return *match;
    }
    if (passedTarget) break;
    if (reply.nextAfterObjectName.empty()) break;
    if (reply.objects.empty() || reply.nextAfterObjectName != previousName ||
        reply.nextAfterObjectName <= cursor) {
      throw std::runtime_error("repo-user-storage-claim-revalidation-cursor-invalid");
    }
    cursor = reply.nextAfterObjectName;
  }
  throw std::runtime_error("repo-user-storage-owner-claim-changed-after-semantic-validation");
}

namespace {

struct SelectedRequestResult
{
  std::mutex mutex;
  std::condition_variable condition;
  bool done = false;
  std::string error;
  bool noAckObservations = false;
  std::vector<uint8_t> payload;
};

struct SelectedRequestStart
{
  enum class CommitState
  {
    Available,
    Claimed,
    Abandoned
  };

  std::atomic<bool> abandoned{false};
  std::atomic<CommitState> commitState{CommitState::Available};
  std::atomic<bool> cancellationIssued{false};
};

void
completeSelectedRequest(const std::shared_ptr<SelectedRequestResult>& result,
                        const std::string& error,
                        bool noAckObservations = false)
{
  {
    std::lock_guard<std::mutex> guard(result->mutex);
    if (result->done) return;
    result->error = error;
    result->noAckObservations = noAckObservations;
    result->done = true;
  }
  result->condition.notify_all();
}

class ExactRepoParticipantSelection final :
  public ndn_service_framework::ParticipantSelectionPolicy
{
public:
  ExactRepoParticipantSelection(ndn::Name provider, ndn::Name service)
    : m_provider(std::move(provider))
    , m_service(std::move(service))
  {
  }

  std::vector<ndn_service_framework::SelectedParticipant>
  select(const std::vector<ndn_service_framework::AckCandidate>& candidates,
         const std::vector<ndn_service_framework::CollaborationRoleSpec>& roles) const override
  {
    if (roles.size() != 1) return {};
    const auto found = std::find_if(candidates.begin(), candidates.end(), [&] (const auto& candidate) {
      return candidate.providerName == m_provider && candidate.serviceName == m_service &&
             candidate.ack.getStatus() &&
             candidate.authenticationEvidence.trustSchemaValidated &&
             candidate.authenticationEvidence.signerIdentity == m_provider.toUri();
    });
    if (found == candidates.end()) return {};
    ndn_service_framework::SelectedParticipant selected;
    selected.role = roles.front().role;
    selected.service = roles.front().service;
    selected.provider = found->providerName;
    selected.assignedArtifact = roles.front().requiredArtifact;
    selected.requiresProvisioning = roles.front().allowDynamicProvisioning;
    selected.provisioningTimeoutMs = roles.front().provisioningTimeoutMs;
    selected.assignmentPayload = roles.front().assignmentPayload;
    selected.ack = *found;
    return {std::move(selected)};
  }

private:
  ndn::Name m_provider;
  ndn::Name m_service;
};

std::vector<uint8_t>
localRequest(ndn_service_framework::LocalServiceRegistry& registry,
             const ndn::Name& repoServicePrefix,
             const std::string& operation,
             const std::vector<uint8_t>& payload)
{
  auto request = RepoClient::makeRequest(payload);
  ndn_service_framework::ResponseMessage response;
  registry.localInvokeRawInto(
    makeRepoServiceName(repoServicePrefix, operation),
    request,
    response);
  if (!response.getStatus()) {
    throw std::runtime_error("local repo " + operation + " failed: " +
                             response.getErrorInfo());
  }
  const auto responsePayload = response.getPayload();
  return std::vector<uint8_t>(responsePayload.begin(), responsePayload.end());
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

void
RepoClient::putRange(RepoNode& node,
                     const RepoObjectManifest& manifest,
                     RepoByteRange range,
                     const std::vector<uint8_t>& bytes)
{
  node.putRange(manifest, range, bytes);
}

RepoObjectManifest
RepoClient::commitRanges(RepoNode& node, const RepoObjectManifest& manifest)
{
  return node.commitRanges(manifest);
}

void
RepoClient::abortRanges(RepoNode& node, const std::string& objectName)
{
  node.abortRanges(objectName);
}

std::vector<uint8_t>
RepoClient::getRange(const RepoNode& node,
                    const std::string& objectName,
                    RepoByteRange range)
{
  return node.getRange(objectName, range);
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
RepoClient::putDataPacket(RepoNode& node, const std::vector<uint8_t>& wire)
{
  ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
  block.parse();
  const ndn::Data data(block);
  return node.putDataPacket(data.getName().toUri(ndn::name::UriFormat::CANONICAL), wire);
}

std::vector<uint8_t>
RepoClient::getDataPacket(const RepoNode& node, const std::string& dataName)
{
  return node.getDataPacket(dataName);
}

std::vector<std::vector<uint8_t>>
RepoClient::getDataPackets(const RepoNode& node,
                           const RepoObjectManifest& manifest)
{
  if (manifest.packetNames.empty()) {
    throw std::invalid_argument(
      "repo-packet-index-invalid: manifest has no packet names: " +
      manifest.objectName);
  }
  if (manifest.packetNames.size() != manifest.segmentCount) {
    throw std::invalid_argument(
      "repo-packet-index-invalid: packet count does not match segmentCount: " +
      manifest.objectName);
  }
  std::set<ndn::Name> uniqueNames;
  for (const auto& name : manifest.packetNames)
    uniqueNames.emplace(name);
  if (uniqueNames.size() != manifest.packetNames.size()) {
    throw std::invalid_argument(
      "repo-packet-index-invalid: manifest contains duplicate packet names: " +
      manifest.objectName);
  }

  std::vector<std::vector<uint8_t>> wires;
  wires.reserve(manifest.packetNames.size());
  for (const auto& expectedName : manifest.packetNames) {
    auto wire = getDataPacket(node, expectedName);
    ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
    block.parse();
    const ndn::Data data(block);
    const auto actualName = data.getName().toUri(ndn::name::UriFormat::CANONICAL);
    if (data.getName() != ndn::Name(expectedName)) {
      throw std::runtime_error(
        "repo-data-name-mismatch: manifest=" + expectedName +
        " wire=" + actualName);
    }
    wires.push_back(std::move(wire));
  }
  return wires;
}

RepoOperationStatus
RepoClient::insert(RepoNode& node,
                   const RepoDataReference& reference)
{
  return parseOperationStatusJson(toString(
    node.handleInsert(encodeDataReferenceRequest(reference))));
}

RepoOperationStatus
RepoClient::insertPayload(RepoNode& node,
                          const std::string& objectName,
                          const std::vector<uint8_t>& payload,
                          ndn::KeyChain& keyChain,
                          const ndn::security::SigningInfo& signingInfo,
                          StoreOptions options,
                          size_t maxSegmentPayload)
{
  using SigningInfo = ndn::security::SigningInfo;
  ndn::Name producer;
  switch (signingInfo.getSignerType()) {
    case SigningInfo::SIGNER_TYPE_NULL: producer = keyChain.getPib().getDefaultIdentity().getName(); break;
    case SigningInfo::SIGNER_TYPE_ID: producer = signingInfo.getSignerName(); break;
    case SigningInfo::SIGNER_TYPE_KEY:
      producer = ndn::security::extractIdentityFromKeyName(signingInfo.getSignerName()); break;
    case SigningInfo::SIGNER_TYPE_CERT:
      producer = ndn::security::extractIdentityFromCertName(signingInfo.getSignerName()); break;
    default: throw std::invalid_argument("repo-publisher-identity-unresolved");
  }
  const ndn::Name prefix(objectName);
  if (!ndn_service_framework::parseProducerDataName(prefix, producer))
    throw std::invalid_argument("repo-publisher-prefix-mismatch");
  const auto identity = keyChain.getPib().getIdentity(producer);
  auto key = identity.getDefaultKey();
  if (signingInfo.getSignerType() == SigningInfo::SIGNER_TYPE_KEY)
    key = identity.getKey(signingInfo.getSignerName());
  else if (signingInfo.getSignerType() == SigningInfo::SIGNER_TYPE_CERT)
    key = identity.getKey(ndn::security::extractKeyNameFromCertName(signingInfo.getSignerName()));
  const size_t segmentBytes = std::max<size_t>(1, maxSegmentPayload);
  const size_t segmentCount = payload.empty() ? 1 : 1 + (payload.size() - 1) / segmentBytes;

  std::vector<std::vector<uint8_t>> wirePackets;
  wirePackets.reserve(segmentCount);
  std::vector<uint8_t> concatenated;
  for (size_t i = 0; i < segmentCount; ++i) {
    const auto offset = i * segmentBytes;
    const auto length = std::min(segmentBytes, payload.size() - offset);
    ndn::Data data(ndn::Name(prefix).appendSegment(i));
    data.setContent(length ? ndn::span<const uint8_t>(payload.data() + offset, length)
                           : ndn::span<const uint8_t>());
    data.setFreshnessPeriod(ndn::time::hours(1));
    data.setFinalBlock(ndn::name::Component::fromSegment(segmentCount - 1));
    if (node.hasDataPacket(data.getName().toUri())) {
      const auto existing = node.getDataPacket(data.getName().toUri());
      const ndn::Data saved(ndn::Block(ndn::span<const uint8_t>(existing.data(), existing.size())));
      if (saved.getContent() != data.getContent() || saved.getMetaInfo().wireEncode() != data.getMetaInfo().wireEncode() ||
          !ndn::security::verifySignature(saved, key.getPublicKey()))
        throw std::runtime_error("repo-data-wire-conflict");
      wirePackets.push_back(existing);
      concatenated.insert(concatenated.end(), existing.begin(), existing.end());
      continue;
    }
    keyChain.sign(data, signingInfo);
    const auto& wire = data.wireEncode();
    std::vector<uint8_t> wireBytes(wire.begin(), wire.end());
    concatenated.insert(concatenated.end(), wireBytes.begin(), wireBytes.end());
    wirePackets.push_back(std::move(wireBytes));
  }

  RepoDataReference reference;
  reference.objectName = ndn::Name(prefix).appendSegment(0).toUri(ndn::name::UriFormat::CANONICAL);
  reference.dataPrefix = prefix.toUri(ndn::name::UriFormat::CANONICAL);
  reference.firstSegment = 0;
  reference.hasFinalSegment = !wirePackets.empty();
  reference.finalSegment = wirePackets.empty() ? 0 : wirePackets.size() - 1;
  reference.expectedSize = concatenated.size();
  reference.expectedSha256 = sha256Hex(concatenated);
  reference.storeWirePackets = true;
  reference.objectType = options.objectType.empty() ? "ndn-segmented-data" : options.objectType;

  return node.insertWirePackets(reference, wirePackets);
}

RepoOperationStatus
RepoClient::status(const RepoNode& node, const std::string& operationId)
{
  return parseOperationStatusJson(toString(node.handleStatus(encodeStatusRequest(operationId))));
}

RepoCatalogStatus
RepoClient::catalogStatus(const RepoNode& node)
{
  return parseCatalogStatusJson(toString(node.handleCatalogStatus()));
}

RepoCacheStatus
RepoClient::cacheStatus(const RepoNode& node)
{
  return parseCacheStatusJson(toString(node.handleCacheStatus()));
}

RepoCatalogDelta
RepoClient::catalogSnapshot(const RepoNode& node)
{
  return parseCatalogDeltaJson(toString(node.handleCatalogSnapshot()));
}

RepoCatalogDelta
RepoClient::catalogDelta(const RepoNode& node, uint64_t sinceEpoch)
{
  return parseCatalogDeltaJson(toString(
    node.handleCatalogDelta(encodeCatalogDeltaRequest(sinceEpoch))));
}

RepoCatalogEntry
RepoClient::catalogLookup(const RepoNode& node,
                          const std::string& objectName)
{
  return parseCatalogEntryJson(toString(
    node.handleCatalogLookup(encodeCatalogLookupRequest(objectName))));
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
  return node.putManifest(manifest);
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

void
RepoClient::localPutRange(ndn_service_framework::LocalServiceRegistry& registry,
                          const ndn::Name& repoServicePrefix,
                          const RepoObjectManifest& manifest,
                          RepoByteRange range,
                          const std::vector<uint8_t>& bytes)
{
  const auto response = localRequest(
    registry, repoServicePrefix, "STORE_RANGE",
    encodeRangeWriteRequest(manifest, range, bytes));
  if (toString(response) != "accepted") {
    throw std::runtime_error("local repo STORE_RANGE returned unexpected response");
  }
}

RepoObjectManifest
RepoClient::localCommitRanges(ndn_service_framework::LocalServiceRegistry& registry,
                              const ndn::Name& repoServicePrefix,
                              const RepoObjectManifest& manifest)
{
  return parseManifestJson(toString(localRequest(
    registry, repoServicePrefix, "COMMIT_RANGES", encodeManifestRequest(manifest))));
}

void
RepoClient::localAbortRanges(ndn_service_framework::LocalServiceRegistry& registry,
                             const ndn::Name& repoServicePrefix,
                             const std::string& objectName)
{
  const auto response = localRequest(
    registry, repoServicePrefix, "ABORT_RANGES",
    encodeRangeAbortRequest(objectName));
  if (toString(response) != "aborted") {
    throw std::runtime_error("local repo ABORT_RANGES returned unexpected response");
  }
}

std::vector<uint8_t>
RepoClient::localGetRange(ndn_service_framework::LocalServiceRegistry& registry,
                          const ndn::Name& repoServicePrefix,
                          const std::string& objectName,
                          RepoByteRange range)
{
  return localRequest(registry, repoServicePrefix, "FETCH_RANGE",
                      encodeRangeReadRequest(objectName, range));
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

RepoOperationStatus
RepoClient::localInsert(ndn_service_framework::LocalServiceRegistry& registry,
                                       const ndn::Name& repoServicePrefix,
                                       const RepoDataReference& reference)
{
  return parseOperationStatusJson(toString(localRequest(
    registry, repoServicePrefix, "INSERT",
    encodeDataReferenceRequest(reference))));
}

RepoOperationStatus
RepoClient::localStatus(ndn_service_framework::LocalServiceRegistry& registry,
                        const ndn::Name& repoServicePrefix,
                        const std::string& operationId)
{
  return parseOperationStatusJson(toString(localRequest(
    registry, repoServicePrefix, "STATUS", encodeStatusRequest(operationId))));
}

RepoCatalogStatus
RepoClient::localCatalogStatus(ndn_service_framework::LocalServiceRegistry& registry,
                               const ndn::Name& repoServicePrefix)
{
  return parseCatalogStatusJson(toString(localRequest(
    registry, repoServicePrefix, "CATALOG_STATUS", {})));
}

RepoCacheStatus
RepoClient::localCacheStatus(ndn_service_framework::LocalServiceRegistry& registry,
                             const ndn::Name& repoServicePrefix)
{
  return parseCacheStatusJson(toString(localRequest(
    registry, repoServicePrefix, "CACHE_STATUS", {})));
}

RepoCatalogDelta
RepoClient::localCatalogSnapshot(ndn_service_framework::LocalServiceRegistry& registry,
                                 const ndn::Name& repoServicePrefix)
{
  return parseCatalogDeltaJson(toString(localRequest(
    registry, repoServicePrefix, "CATALOG_SNAPSHOT", {})));
}

RepoCatalogDelta
RepoClient::localCatalogDelta(ndn_service_framework::LocalServiceRegistry& registry,
                              const ndn::Name& repoServicePrefix,
                              uint64_t sinceEpoch)
{
  return parseCatalogDeltaJson(toString(localRequest(
    registry, repoServicePrefix, "CATALOG_DELTA",
    encodeCatalogDeltaRequest(sinceEpoch))));
}

RepoCatalogEntry
RepoClient::localCatalogLookup(ndn_service_framework::LocalServiceRegistry& registry,
                               const ndn::Name& repoServicePrefix,
                               const std::string& objectName)
{
  return parseCatalogEntryJson(toString(localRequest(
    registry, repoServicePrefix, "CATALOG_LOOKUP",
    encodeCatalogLookupRequest(objectName))));
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
  ndn::Buffer buffer;
  if (!payload.empty()) {
    buffer = ndn::Buffer(payload.data(), payload.size());
  }
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
RepoClient::requestCacheStatus(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "CACHE_STATUS"),
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
RepoClient::requestStoreRange(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const RepoObjectManifest& manifest,
  RepoByteRange range,
  const std::vector<uint8_t>& bytes,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "STORE_RANGE"),
    makeRequest(encodeRangeWriteRequest(manifest, range, bytes)),
    timeoutMs,
    std::move(onTimeout),
    std::move(onResponse));
}

ndn::Name
RepoClient::requestArtifactStoreRange(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const ArtifactRangeWriteRequest& request,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "STORE_RANGE"),
    makeRequest(request.toWire()), timeoutMs,
    std::move(onTimeout), std::move(onResponse));
}

ndn::Name
RepoClient::requestArtifactStore(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const ArtifactStoreRequest& request,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "STORE"),
    makeRequest(request.toWire()), timeoutMs,
    std::move(onTimeout), std::move(onResponse));
}

ndn::Name
RepoClient::requestCommitRanges(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const RepoObjectManifest& manifest,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "COMMIT_RANGES"),
    makeRequest(encodeManifestRequest(manifest)),
    timeoutMs,
    std::move(onTimeout),
    std::move(onResponse));
}

ndn::Name
RepoClient::requestArtifactCommitRanges(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const ArtifactRangeCommitRequest& request,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "COMMIT_RANGES"),
    makeRequest(request.toWire()), timeoutMs,
    std::move(onTimeout), std::move(onResponse));
}

std::vector<uint8_t>
RepoClient::requestSelectedService(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& providerIdentity,
  const ndn::Name& serviceName,
  const std::vector<uint8_t>& payload,
  int ackCollectionTimeMs,
  int timeoutMs,
  std::shared_ptr<std::atomic<bool>> requestMayHaveBeenEmitted)
{
  if (providerIdentity.empty() || serviceName.empty() || ackCollectionTimeMs <= 0 ||
      timeoutMs <= ackCollectionTimeMs) {
    throw std::invalid_argument("repo-selected-request-invalid: Provider, service, or deadline is invalid");
  }
  if (user.isOnIoThread()) {
    throw std::logic_error(
      "repo-selected-request-on-io-thread: blocking selected requests cannot run on the Face I/O thread");
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  const std::string timeoutError =
    "repo-request-timeout: Provider request timed out";
  auto result = std::make_shared<SelectedRequestResult>();
  auto startState = std::make_shared<SelectedRequestStart>();
  ndn::Buffer requestPayload;
  if (!payload.empty()) requestPayload = ndn::Buffer(payload.data(), payload.size());
  const std::vector<ndn::Name> providers{providerIdentity};
  const auto cancelRequest = [&user, startState] (const ndn::Name& requestId) {
    if (!user.isOnIoThread() ||
        startState->cancellationIssued.exchange(true,
                                                 std::memory_order_acq_rel)) {
      return;
    }
    try {
      user.CancelCollaboration(requestId);
    }
    catch (...) {
      // Cancellation is best-effort after the caller's absolute deadline.
    }
  };
  auto startRequest = [&user, result, startState, providerIdentity, serviceName,
                       requestPayload, ackCollectionTimeMs, deadline,
                       timeoutError, requestMayHaveBeenEmitted,
                       providers, cancelRequest] {
    try {
      if (startState->abandoned.load(std::memory_order_acquire) ||
          std::chrono::steady_clock::now() >= deadline) {
        startState->abandoned.store(true, std::memory_order_release);
        completeSelectedRequest(result, timeoutError);
        return;
      }
      const auto remainingTimeoutMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      if (remainingTimeoutMs <= ackCollectionTimeMs) {
        startState->abandoned.store(true, std::memory_order_release);
        completeSelectedRequest(result, timeoutError);
        return;
      }
      const auto invocationTimeoutMs = static_cast<int>(remainingTimeoutMs);
      const auto requestId = user.BeginCollaborationWithProviders(
        serviceName, requestPayload, ackCollectionTimeMs, invocationTimeoutMs,
        [&user, result, startState, providerIdentity, serviceName,
         ackCollectionTimeMs, invocationTimeoutMs, deadline, timeoutError,
         requestMayHaveBeenEmitted, cancelRequest]
        (const ndn_service_framework::CollaborationAckClosure& closure) {
          try {
            if (startState->abandoned.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              cancelRequest(closure.requestId);
              completeSelectedRequest(result, timeoutError);
              return;
            }
            size_t matching = 0;
            for (const auto& candidate : closure.candidates) {
              if (candidate.providerName == providerIdentity &&
                  candidate.serviceName == serviceName && candidate.ack.getStatus() &&
                  candidate.authenticationEvidence.trustSchemaValidated &&
                  candidate.authenticationEvidence.signerIdentity ==
                    providerIdentity.toUri()) {
                ++matching;
              }
            }
            if (matching != 1) {
              const auto error =
                std::string("repo-ack-authentication-failed: selected Provider ACK was absent, duplicated, or untrusted") +
                " (provider=" + providerIdentity.toUri(ndn::name::UriFormat::ALTERNATE) +
                ", service=" + serviceName.toUri(ndn::name::UriFormat::ALTERNATE) +
                ", matching=" + std::to_string(matching) +
                ", observed=" + std::to_string(closure.candidates.size()) + ")";
              cancelRequest(closure.requestId);
              if (std::chrono::steady_clock::now() >= deadline) {
                startState->abandoned.store(true, std::memory_order_release);
                completeSelectedRequest(result, timeoutError);
                return;
              }
              completeSelectedRequest(result, error,
                closure.candidates.empty() && !closure.ackObserved);
              return;
            }
            if (startState->abandoned.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              cancelRequest(closure.requestId);
              completeSelectedRequest(result, timeoutError);
              return;
            }
            ndn_service_framework::CollaborationRoleSpec role;
            role.role = "artifact-repo-target";
            role.service = serviceName;
            role.minProviders = 1;
            role.maxProviders = 1;
            role.terminalResponseOwner = true;
            role.assignmentPayload = ndn::Buffer(1);
            role.assignmentPayload[0] = 1;
            ndn_service_framework::CollaborationPlan plan;
            plan.ackCollectionTimeMs = ackCollectionTimeMs;
            plan.timeoutMs = invocationTimeoutMs;
            plan.roles.push_back(std::move(role));
            plan.participantSelector = std::make_shared<ExactRepoParticipantSelection>(
              providerIdentity, serviceName);
            if (startState->abandoned.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              cancelRequest(closure.requestId);
              completeSelectedRequest(result, timeoutError);
              return;
            }
            // CommitCollaborationPlan can publish a Selection before reporting
            // an error. This atomic claim races the caller's deadline so a
            // timed-out request cannot newly cross the emission boundary.
            auto expectedCommitState = SelectedRequestStart::CommitState::Available;
            if (!startState->commitState.compare_exchange_strong(
                  expectedCommitState, SelectedRequestStart::CommitState::Claimed,
                  std::memory_order_acq_rel)) {
              startState->abandoned.store(true, std::memory_order_release);
              cancelRequest(closure.requestId);
              completeSelectedRequest(result, timeoutError);
              return;
            }
            if (startState->abandoned.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              startState->commitState.store(
                SelectedRequestStart::CommitState::Abandoned,
                std::memory_order_release);
              cancelRequest(closure.requestId);
              completeSelectedRequest(result, timeoutError);
              return;
            }
      if (requestMayHaveBeenEmitted) {
        requestMayHaveBeenEmitted->store(true, std::memory_order_release);
      }
      if (!user.CommitCollaborationPlan(closure.requestId, closure.digest,
                                        std::move(plan))) {
        throw std::runtime_error(
          "repo-selection-commit-failed: Core did not commit selected Provider");
      }
      if (startState->abandoned.load(std::memory_order_acquire) ||
          std::chrono::steady_clock::now() >= deadline) {
        startState->abandoned.store(true, std::memory_order_release);
        startState->commitState.store(
          SelectedRequestStart::CommitState::Abandoned,
          std::memory_order_release);
        cancelRequest(closure.requestId);
        completeSelectedRequest(result, timeoutError);
      }
          }
          catch (const std::exception& error) {
            cancelRequest(closure.requestId);
            if (startState->abandoned.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              completeSelectedRequest(result, timeoutError);
            }
            else {
              completeSelectedRequest(result, error.what());
            }
          }
          catch (...) {
            cancelRequest(closure.requestId);
            if (startState->abandoned.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              completeSelectedRequest(result, timeoutError);
            }
            else {
              completeSelectedRequest(
                result, "repo-selection-commit-failed: selected Provider request failed");
            }
          }
        },
        [result, startState, deadline, timeoutError]
        (const ndn_service_framework::ResponseMessage& response) {
          const auto responsePayload = response.getPayload();
          {
            std::lock_guard<std::mutex> guard(result->mutex);
            if (result->done) return;
            if (std::chrono::steady_clock::now() >= deadline) {
              startState->abandoned.store(true, std::memory_order_release);
              result->error = timeoutError;
            }
            else if (!response.getStatus()) {
              result->error = response.getErrorInfo();
            }
            else {
              result->payload.assign(responsePayload.begin(), responsePayload.end());
            }
            result->done = true;
          }
          result->condition.notify_all();
        },
        [result, startState, timeoutError] (const ndn::Name&) {
          startState->abandoned.store(true, std::memory_order_release);
          completeSelectedRequest(result, timeoutError);
        },
        ndn::Name(),
        [providerIdentity, serviceName] (
          const std::vector<ndn_service_framework::AckCandidate>& candidates) {
          // The fixed Provider set is covered by one authenticated ACK. Core
          // still freezes ACK_CLOSED; the callback above rechecks eligibility.
          return std::count_if(candidates.begin(), candidates.end(),
            [&providerIdentity, &serviceName] (const auto& candidate) {
              return candidate.providerName == providerIdentity &&
                     candidate.serviceName == serviceName && candidate.ack.getStatus() &&
                     candidate.authenticationEvidence.trustSchemaValidated &&
                     candidate.authenticationEvidence.signerIdentity ==
                       providerIdentity.toUri();
            }) == 1;
        }, ndn_service_framework::RequestCapabilities(),
        std::nullopt, {}, {}, {}, providers);
      // Begin can return an ID after local admission rejection; the lifecycle
      // check below handles that case. Otherwise the Request is admitted or
      // queued, and ACK-close runs after this Face-thread start returns, so an
      // empty ACK set leaves the provider outcome uncertain before Selection.
      if (!requestId.empty()) {
        const auto requestStatus = user.getRequestStatus(requestId);
        const bool admissionRejected = requestStatus &&
          requestStatus->state ==
            ndn_service_framework::ServiceUser::RequestLifecycleState::ADMISSION_REJECTED;
        if (admissionRejected) {
          completeSelectedRequest(
            result,
            "repo-request-admission-rejected: local admission queue rejected Provider request");
          return;
        }
        if (requestMayHaveBeenEmitted) {
          requestMayHaveBeenEmitted->store(true, std::memory_order_release);
        }
      }
      if (requestId.empty()) {
        if (startState->abandoned.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= deadline) {
          startState->abandoned.store(true, std::memory_order_release);
          completeSelectedRequest(result, timeoutError);
        }
        else {
          completeSelectedRequest(
            result, "repo-request-start-failed: Core rejected selected Provider request");
        }
        return;
      }
      if (startState->abandoned.load(std::memory_order_acquire) ||
          std::chrono::steady_clock::now() >= deadline) {
        startState->abandoned.store(true, std::memory_order_release);
        cancelRequest(requestId);
        completeSelectedRequest(result, timeoutError);
      }
    }
    catch (const std::exception& error) {
      if (startState->abandoned.load(std::memory_order_acquire) ||
          std::chrono::steady_clock::now() >= deadline) {
        startState->abandoned.store(true, std::memory_order_release);
        completeSelectedRequest(result, timeoutError);
      }
      else {
        completeSelectedRequest(result, error.what());
      }
    }
    catch (...) {
      if (startState->abandoned.load(std::memory_order_acquire) ||
          std::chrono::steady_clock::now() >= deadline) {
        startState->abandoned.store(true, std::memory_order_release);
        completeSelectedRequest(result, timeoutError);
      }
      else {
        completeSelectedRequest(
          result, "repo-request-start-failed: selected Provider request could not start");
      }
    }
  };
  try {
    user.postToIo(std::move(startRequest));
  }
  catch (const std::exception& error) {
    startState->abandoned.store(true, std::memory_order_release);
    if (std::chrono::steady_clock::now() >= deadline) {
      completeSelectedRequest(result, timeoutError);
    }
    else {
      completeSelectedRequest(result, error.what());
    }
  }
  catch (...) {
    startState->abandoned.store(true, std::memory_order_release);
    if (std::chrono::steady_clock::now() >= deadline) {
      completeSelectedRequest(result, timeoutError);
    }
    else {
      completeSelectedRequest(
        result, "repo-request-start-failed: selected Provider request could not start");
    }
  }
  std::unique_lock<std::mutex> lock(result->mutex);
  const bool completed = result->condition.wait_until(
    lock, deadline, [&] { return result->done; });
  if (!completed) {
    startState->abandoned.store(true, std::memory_order_release);
    auto expectedCommitState = SelectedRequestStart::CommitState::Available;
    if (!startState->commitState.compare_exchange_strong(
          expectedCommitState, SelectedRequestStart::CommitState::Abandoned,
          std::memory_order_acq_rel) &&
        expectedCommitState == SelectedRequestStart::CommitState::Claimed &&
        requestMayHaveBeenEmitted) {
      // The ACK callback claimed the Selection boundary just before timeout;
      // keep fallback logic from treating that in-flight publish as absent.
      requestMayHaveBeenEmitted->store(true, std::memory_order_release);
    }
    result->error = timeoutError;
    result->done = true;
  }
  auto error = std::move(result->error);
  const bool noAckObservations = result->noAckObservations;
  auto responsePayload = std::move(result->payload);
  lock.unlock();
  if (!completed) {
    result->condition.notify_all();
  }
  if (!error.empty()) {
    if (noAckObservations) throw RepoNoAckObservations(error);
    throw std::runtime_error(error);
  }
  return responsePayload;
}

ndn::Name
RepoClient::requestAbortRanges(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const std::string& objectName,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "ABORT_RANGES"),
    makeRequest(encodeRangeAbortRequest(objectName)),
    timeoutMs,
    std::move(onTimeout),
    std::move(onResponse));
}

ndn::Name
RepoClient::requestInsert(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const RepoDataReference& reference,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "INSERT"),
    makeRequest(encodeDataReferenceRequest(reference)),
    timeoutMs,
    std::move(onTimeout),
    std::move(onResponse));
}

ndn::Name
RepoClient::requestInsert(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const RepoUserStorageInsertRequest& request,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "INSERT"),
    makeRequest(encodeUserStorageInsertRequest(request)),
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
RepoClient::requestReceipt(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& provider,
  const ndn::Name& repoServicePrefix,
  const RepoReceiptLookupRequest& request,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  const auto payload = toBytes(request.toJson());
  const char* traceSetting =
    std::getenv("NDNSF_SPEC192_SELECTED_REQUEST_TRACE");
  const bool traceEnabled = traceSetting != nullptr &&
                            traceSetting[0] == '1' && traceSetting[1] == '\0';
  if (!traceEnabled) {
    return user.RequestService(
      std::vector<ndn::Name>{provider},
      makeRepoServiceName(repoServicePrefix, "STATUS"), makeRequest(payload),
      timeoutMs, std::move(onTimeout), std::move(onResponse),
      ndn_service_framework::tlv::FirstResponding);
  }
  const auto traceStage = [] (const char* stage) {
    std::fprintf(stderr, "Spec192 receipt request: stage=%s service=STATUS\n",
                 stage);
    std::fflush(stderr);
  };
  traceStage("request-service-begin");
  auto tracedOnTimeout = [traceStage, onTimeout = std::move(onTimeout)]
                         (const ndn::Name& timedOutRequest) mutable {
    traceStage("timeout-callback");
    if (onTimeout) onTimeout(timedOutRequest);
  };
  auto tracedOnResponse = [traceStage, onResponse = std::move(onResponse)]
                          (const ndn_service_framework::ResponseMessage& response)
                          mutable {
    traceStage("response-callback");
    if (onResponse) onResponse(response);
  };
  auto requestId = user.RequestService(
    std::vector<ndn::Name>{provider},
    makeRepoServiceName(repoServicePrefix, "STATUS"), makeRequest(payload),
    timeoutMs, std::move(tracedOnTimeout), std::move(tracedOnResponse),
    ndn_service_framework::tlv::FirstResponding);
  traceStage("request-service-returned");
  return requestId;
}

ndn::Name
RepoClient::requestAvailability(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& provider,
  const ndn::Name& repoServicePrefix,
  const RepoAvailabilityProbeRequest& request,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  const auto payload = toBytes(request.toJson());
  if (request.schemaVersion == 2) {
    if (timeoutMs <= 0 || provider.empty() || repoServicePrefix.empty() ||
        ndn::Name(request.expectedLocation.providerIdentity) != provider)
      throw std::invalid_argument("repo-owned-availability-target-invalid");
    const auto owner = user.getName().toUri(ndn::name::UriFormat::ALTERNATE);
    if (user.getName().empty())
      throw std::invalid_argument("repo-owned-availability-owner-invalid");
    onResponse = checkedUserStorageResponse([request, owner](const std::string& wire) {
      RepoAvailabilityProbeResponse::fromJson(wire).validateExpected(request, owner);
    }, std::move(onResponse));
  }
  return user.RequestService(
    std::vector<ndn::Name>{provider},
    makeRepoServiceName(repoServicePrefix, "STATUS"), makeRequest(payload),
    timeoutMs, std::move(onTimeout), std::move(onResponse),
    ndn_service_framework::tlv::FirstResponding);
}

ndn::Name
RepoClient::requestFetchRange(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const std::string& objectName,
  RepoByteRange range,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    makeRepoServiceName(repoServicePrefix, "FETCH_RANGE"),
    makeRequest(encodeRangeReadRequest(objectName, range)),
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

ndn::Name
RepoClient::requestStatus(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& repoServicePrefix,
  const std::string& operationId,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(makeRepoServiceName(repoServicePrefix, "STATUS"),
                             makeRequest(encodeStatusRequest(operationId)),
                             timeoutMs,
                             std::move(onTimeout),
                             std::move(onResponse));
}

ndn::Name
RepoClient::requestLocation(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& provider,
  const ndn::Name& repoServicePrefix,
  const ArtifactLocationRequest& request,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  const auto wire = request.toJson();
  const auto payload = toBytes(wire);
  return user.RequestService(
    std::vector<ndn::Name>{provider},
    makeRepoServiceName(repoServicePrefix, "STATUS"), makeRequest(payload),
    timeoutMs, std::move(onTimeout), std::move(onResponse),
    ndn_service_framework::tlv::FirstResponding);
}

ndn::Name
RepoClient::requestFetchRange(
  ndn_service_framework::ServiceUser& user,
  const ndn::Name& provider,
  const ndn::Name& repoServicePrefix,
  const std::string& objectName,
  RepoByteRange range,
  int timeoutMs,
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
  ndn_service_framework::ServiceUser::ResponseHandler onResponse)
{
  return user.RequestService(
    std::vector<ndn::Name>{provider},
    makeRepoServiceName(repoServicePrefix, "FETCH_RANGE"),
    makeRequest(encodeRangeReadRequest(objectName, range)), timeoutMs,
    std::move(onTimeout), std::move(onResponse),
    ndn_service_framework::tlv::FirstResponding);
}

} // namespace ndnsf_distributed_repo
