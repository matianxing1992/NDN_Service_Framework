#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-publication.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-save.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/protected-kv-packets.hpp"

#include "ndn-service-framework/ServiceUser.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {

ndnsf_distributed_repo::RepoDataReference
nativeKvPacketReference(const NativeProtectedKvPacketSet& packets, const ndn::Name& producer)
{
  if (packets.packetNames.size() != packets.packetWires.size())
    throw std::invalid_argument("HANDOFF_PACKET_INDEX_SIZE_MISMATCH");
  ndnsf_distributed_repo::RepoDataReference object;
  object.objectName = packets.rootName.toUri(ndn::name::UriFormat::ALTERNATE);
  object.dataPrefix = producer.toUri(ndn::name::UriFormat::ALTERNATE);
  object.expectedSize = packets.wireBytes;
  object.expectedSha256 = packets.wireDigest.substr(7);
  object.packetSetMetadata.emplace();
  auto& metadata = *object.packetSetMetadata;
  metadata.objectName = object.objectName;
  metadata.objectType = "original-packet-set";
  metadata.size = object.expectedSize;
  metadata.sha256 = object.expectedSha256;
  // These are original NDKVE001 packets. Nonempty metadata.dataPrefix marks a
  // Core ProtectedBlobManifest envelope and would select the wrong Repo decoder.
  metadata.packetOffsets = {0};
  for (std::size_t i = 0; i < packets.packetNames.size(); ++i) {
    metadata.packetNames.push_back(packets.packetNames[i].toUri(ndn::name::UriFormat::ALTERNATE));
    metadata.packetOffsets.push_back(metadata.packetOffsets.back() + packets.packetWires[i].size());
  }
  object.validateExactPacketSet();
  return object;
}

namespace {

void
require(bool condition, const char* reason)
{
  if (!condition) throw std::runtime_error(reason);
}

NativeProtectedKvPublicationV1
publish(const std::shared_ptr<ndn_service_framework::ServiceProvider>& producer,
  const ndn::Name& authenticatedOwner, const ndn::Name& serviceName,
  const ndn::Name& requestId, const std::filesystem::path& protectedPath,
  const NativeProtectedKvCheckpointReferenceV1& reference,
  const NativeDurableStorageOwner& owner,
  const NativeProtectedKvCheckpointLimitsV1& limits,
  const std::function<void()>& current)
{
  require(producer && current && owner.resolveGrant && owner.onGrantRequired &&
    owner.onDurablePacketSetBoundary, "HANDOFF_PUBLICATION_INPUT_MISSING");
  current();
  const auto producerName = producer->getName();
  const auto producerUri = producerName.toUri(ndn::name::UriFormat::ALTERNATE);
  nativeValidateKvSaveConfig(owner.publication, producerName);
  require(authenticatedOwner.toUri(ndn::name::UriFormat::ALTERNATE) == owner.ownerIdentity &&
    owner.receipt.requesterIdentity == owner.ownerIdentity &&
    owner.receipt.providerIdentity == producerUri &&
    owner.receipt.serviceName == serviceName.toUri(ndn::name::UriFormat::ALTERNATE) &&
    producerName.isPrefixOf(requestId) && requestId.size() > producerName.size() &&
    reference.ownerIdentity == authenticatedOwner && reference.producerPrefix == producerName &&
    reference.checkpointIdentityDigest == owner.receipt.computedDigest() &&
    owner.publication.signingCertificate.getName() == producer->getSigningCertificateName(),
    "HANDOFF_PUBLICATION_CONTEXT_MISMATCH");
  require(owner.policy.enabled && owner.policy.maxPacketCountPerRole > 0 &&
    owner.policy.maxPacketSetWireBytesPerRole > 0,
    "HANDOFF_PUBLICATION_POLICY_REQUIRED");
  NativeProtectedKvPacketLimits packetLimits;
  packetLimits.checkpoint = limits;
  packetLimits.maxPackets = owner.policy.maxPacketCountPerRole;
  packetLimits.maxPacketWireBytes = owner.policy.maxPacketSetWireBytesPerRole;
  const auto packets = nativePrepareProtectedKvPackets(protectedPath, reference,
    owner.publication.signingCertificate, packetLimits, [&](ndn::Data& packet) {
      require(producer->signProducerDataUntil(packet, owner.deadline, current),
        "HANDOFF_PRODUCER_SIGNING_FAILED");
    }, current);

  NativeDurableStorageGrantRoleChallengeV1 challenge;
  challenge.roleName = owner.receipt.roleName;
  challenge.repoTarget = owner.repoTarget;
  challenge.producerPrefix = producerUri;
  challenge.writerIdentity = producerUri;
  challenge.manifestName = packets.rootName.toUri(ndn::name::UriFormat::ALTERNATE);
  challenge.publicationIdentity = packets.wireDigest;
  // The immutable encrypted-file digest identifies this protection instance.
  challenge.protectionEpoch = reference.wireDigest.substr(7);
  challenge.dataPrefix = reference.objectName().toUri(ndn::name::UriFormat::ALTERNATE);
  challenge.packetWireBytes = packets.wireBytes;
  challenge.auxiliaryPacketNames = {
    owner.publication.signingCertificate.getName().toUri(ndn::name::UriFormat::ALTERNATE)};
  owner.onGrantRequired(challenge);
  const auto grant = owner.resolveGrant(packets.rootName, producerUri, current);
  require(grant.ownerIdentity == owner.ownerIdentity && grant.writerIdentity == producerUri &&
    grant.repoNode == owner.repoTarget.repoNode && grant.objectName == challenge.manifestName &&
    !grant.grantId.empty() && grant.ownerEpoch > 0, "HANDOFF_PUBLICATION_GRANT_MISMATCH");

  // Preserve the original pre-INSERT boundary. Every local role parks here;
  // only the Owner's all-Source COMMIT releases the Source operation.
  owner.onDurablePacketSetBoundary();
  current();
  require(std::chrono::steady_clock::now() < owner.deadline,
    "HANDOFF_DEADLINE_EXPIRED");
  ndnsf_distributed_repo::RepoUserStorageInsertRequest insert;
  insert.operationId = nativePlanningDigest(
    requestId.toUri(ndn::name::UriFormat::ALTERNATE)).substr(7);
  insert.generation = 1;
  insert.storageBinding = {1, grant.ownerIdentity, grant.grantId, grant.ownerEpoch};
  insert.reference = nativeKvPacketReference(packets, producerName);
  const auto& object = insert.reference;
  const auto verify = [&](const ndn::Data& packet) {
    const auto& wire = packet.wireEncode();
    return owner.publication.storageWriter->validateProducerPacketWireUntil(
      ndn::Buffer(wire.begin(), wire.end()), producerName, owner.deadline);
  };
  require(verify(owner.publication.signingCertificate), "HANDOFF_SOURCE_CERTIFICATE_REJECTED");
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
    owner.deadline - std::chrono::steady_clock::now()).count();
  require(remaining > 0, "HANDOFF_DEADLINE_EXPIRED");
  const auto budget = static_cast<int>(std::min<std::int64_t>(remaining,
    std::numeric_limits<int>::max()));
  const ndnsf_distributed_repo::RepoArtifactTarget target{
    owner.repoTarget.repoNode, owner.repoTarget.providerIdentity, owner.repoTarget.servicePrefix};
  std::size_t next = 0;
  const auto committed = [&] {
    try {
      return ndnsf_distributed_repo::RepoClient::commitUserStorageOriginalPackets(
        *owner.publication.storageWriter, target, insert,
        [&]() -> std::optional<std::vector<std::uint8_t>> {
          if (next == packets.packetWires.size()) return std::nullopt;
          return packets.packetWires[next++];
        }, verify, std::min(1000, std::max(1, budget / 2)), budget, current);
    }
    catch (const std::exception& error) {
      // Retain only an internal error code, never packet contents or exception detail.
      std::string code(error.what());
      code = code.substr(0, code.find(':'));
      if (code.size() > 80 || code.rfind("repo-", 0) != 0 ||
          !std::all_of(code.begin(), code.end(), [] (char c) {
            return (c >= 'a' && c <= 'z') || c == '-';
          }))
        code = "UNCLASSIFIED_EXCEPTION";
      logRuntimeEvidence("NDNSF_DI_HANDOFF_REPO_PUBLICATION_FAILED reasonCode=" + code);
      throw;
    }
  }();
  require(ndn::Name(committed.objectName) == ndn::Name(object.objectName) &&
    committed.size == object.expectedSize &&
    committed.sha256 == object.expectedSha256, "HANDOFF_DURABLE_TUPLE_MISMATCH");
  current();
  NativeSavedKvRoleV1 saved;
  saved.protection = reference;
  saved.receipt = owner.receipt;
  saved.rootName = packets.rootName;
  saved.repoNode = target.repoNode;
  saved.repoProvider = target.providerIdentity;
  saved.repoServicePrefix = target.servicePrefix;
  saved.originalWireBytes = committed.size;
  saved.originalWireSha256 = committed.sha256;
  saved.ownerEpoch = grant.ownerEpoch;
  return NativeProtectedKvPublicationV1(std::move(saved), current);
}

} // namespace

void
bindNativeProviderDurableKvPublisher(NativeProviderCollaborationRuntime& runtime,
  std::shared_ptr<ndn_service_framework::ServiceProvider> producer)
{
  if (!producer) throw std::invalid_argument("HANDOFF_PRODUCER_REQUIRED");
  if (runtime.publishProtectedKvCheckpoint)
    throw std::logic_error("HANDOFF_PUBLICATION_ALREADY_BOUND");
  const auto weakProducer = std::weak_ptr<ndn_service_framework::ServiceProvider>(producer);
  runtime.publishProtectedKvCheckpoint = [weakProducer](
    const ndn::Name& authenticatedOwner, const ndn::Name& serviceName,
    const ndn::Name& requestId, const std::filesystem::path& protectedPath,
    const NativeProtectedKvCheckpointReferenceV1& reference,
    const NativeDurableStorageOwner& owner,
    const NativeProtectedKvCheckpointLimitsV1& limits,
    const std::function<void()>& current) {
    const auto activeProducer = weakProducer.lock();
    if (!activeProducer) throw std::runtime_error("HANDOFF_PRODUCER_EXPIRED");
    return publish(activeProducer, authenticatedOwner, serviceName, requestId,
      protectedPath, reference, owner, limits, current);
  };
}

} // namespace ndnsf::di
