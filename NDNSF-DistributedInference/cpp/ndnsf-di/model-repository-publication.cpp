#include "model-repository-publication.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-repository.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"
#include "ndn-service-framework/TimelineTrace.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"

#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/random.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

namespace ndnsf::di {
namespace {

namespace repo = ndnsf_distributed_repo;
constexpr std::size_t MAX_GROUP_PACKETS = 1024;
// Leave room for offsets and operation fields in Repo's 256 KiB JSON dispatch bound.
constexpr std::size_t MAX_GROUP_NAMES = 128U << 10;
constexpr std::uint64_t MAX_GROUP_WIRE = 8U << 20;
constexpr std::size_t MAX_MODEL_OBJECTS = 4096;
constexpr std::uint64_t MAX_PACKET_GROUPS = 65536;
// Exact authenticated ACKs close early; allow bounded SVS recovery before silence.
constexpr int MAX_ACK_COLLECTION_MS = 3000;

void
require(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}

int
remainingRepoBudget(std::chrono::steady_clock::time_point deadline,
                    const std::function<void()>& current)
{
  current();
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
    deadline - std::chrono::steady_clock::now()).count();
  require(remaining > 1, "MODEL_REPO_STORE_DEADLINE");
  return static_cast<int>(std::min<std::int64_t>(300000, remaining));
}

struct PacketGroup
{
  std::vector<std::vector<std::uint8_t>> wires;
  repo::RepoDataReference reference;
  std::size_t nameBytes = 0;
};

std::string
packetDigest(const NativeStoredModelPacketIo::PacketWires& wires)
{
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  require(hash && EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) == 1,
          "MODEL_REPO_STORE_DIGEST_FAILED");
  for (const auto& wire : wires)
    require(EVP_DigestUpdate(hash.get(), wire.data(), wire.size()) == 1,
            "MODEL_REPO_STORE_DIGEST_FAILED");
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned length = 0;
  require(EVP_DigestFinal_ex(hash.get(), bytes, &length) == 1 && length == 32,
          "MODEL_REPO_STORE_DIGEST_FAILED");
  static constexpr char HEX[] = "0123456789abcdef";
  std::string digest;
  digest.reserve(64);
  for (unsigned i = 0; i < length; ++i) {
    digest.push_back(HEX[bytes[i] >> 4]);
    digest.push_back(HEX[bytes[i] & 15]);
  }
  return digest;
}

void
commitPacketGroup(ndn_service_framework::ServiceUser& writer,
  const repo::RepoArtifactTarget& target, PacketGroup& group,
  const std::function<bool(const ndn::Data&)>& verifyPacket,
  const std::function<int()>& budget, const std::function<void()>& current,
  std::uint32_t schemaVersion, const std::string& digest)
{
  auto& reference = group.reference;
  reference.expectedSha256 = reference.packetSetMetadata->sha256 = digest;
  reference.packetSetMetadata->size = reference.expectedSize;
  reference.validateExactPacketSet();
  const auto timeout = budget();
  require(timeout > 1, "MODEL_REPO_STORE_DEADLINE");
  repo::RepoUserStorageGrantRequest request;
  request.grantId = "model-" + digest + "-" + std::to_string(ndn::random::generateWord64());
  request.writerIdentity = writer.getName().toUri(ndn::name::UriFormat::ALTERNATE);
  request.objectName = reference.objectName;
  request.expiresAtMs = static_cast<std::uint64_t>(std::chrono::duration_cast<
    std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) +
    static_cast<std::uint64_t>(timeout);
  const auto grantStarted = std::chrono::steady_clock::now();
  const auto encoded = request.toJson();
  const auto replyWire = repo::RepoClient::requestSelectedService(writer,
    ndn::Name(target.providerIdentity), repo::makeRepoServiceName(
      ndn::Name(target.servicePrefix), "USER_STORAGE_GRANT"),
    std::vector<std::uint8_t>(encoded.begin(), encoded.end()),
    std::min(MAX_ACK_COLLECTION_MS, std::max(1, timeout / 2)), timeout, {}, current);
  const auto reply = repo::RepoUserStorageGrantReply::fromJson(
    std::string(replyWire.begin(), replyWire.end()));
  reply.validateExpected(target, request.writerIdentity, request);
  try {
    if (ndn_service_framework::controlTimingEnvEnabled()) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - grantStarted).count();
      ndn_service_framework::logControlTiming("repo-client", "original-publication",
        ndn::Name(reference.objectName), {
          {"operation_id", request.grantId}, {"stage", "grant"},
          {"elapsed_ns", std::to_string(elapsed)}});
    }
  }
  catch (...) {
    // A diagnostic observation cannot grant or reject storage authority.
  }
  repo::RepoUserStorageInsertRequest insert;
  insert.schemaVersion = schemaVersion;
  insert.operationId = request.grantId;
  insert.generation = 1;
  insert.reference = reference;
  insert.storageBinding = {1, reply.grant.ownerIdentity, reply.grant.grantId,
                           reply.grant.ownerEpoch};
  std::size_t next = 0;
  const auto committed = repo::RepoClient::commitUserStorageOriginalPackets(writer,
    target, insert, [&]() -> std::optional<std::vector<std::uint8_t>> {
      current();
      if (next == group.wires.size()) return std::nullopt;
      return std::move(group.wires[next++]);
    }, verifyPacket, std::min(MAX_ACK_COLLECTION_MS, std::max(1, budget() / 2)),
    budget(), current);
  require(committed.sha256 == digest && committed.size == reference.expectedSize,
          "MODEL_REPO_STORE_COMMIT_MISMATCH");
}

} // namespace

void
nativeValidateModelRepositoryTarget(const repo::RepoArtifactTarget& repository,
                                    const ndn::Name& producer)
{
  for (const auto* text : {&repository.repoNode, &repository.providerIdentity,
                           &repository.servicePrefix}) {
    if (text->empty() || text->size() > 4096 || text->front() != '/')
      throw std::invalid_argument("MODEL_REPO_STORE_TARGET_INVALID");
    const ndn::Name name(*text);
    if (name.empty() || (name.toUri(ndn::name::UriFormat::ALTERNATE) != *text &&
                        name.toUri(ndn::name::UriFormat::CANONICAL) != *text))
      throw std::invalid_argument("MODEL_REPO_STORE_TARGET_INVALID");
  }
  if (producer.empty() || ndn::Name(repository.providerIdentity) == producer)
    throw std::invalid_argument("MODEL_REPO_STORE_REQUIRES_INDEPENDENT_REPO");
}

NativeStoredModelPacketIo
nativeStoredModelRepositoryIo(std::shared_ptr<ndn_service_framework::ServiceUser> user,
  const repo::RepoArtifactTarget& repository, std::chrono::steady_clock::time_point deadline,
  std::function<void()> requireCurrent, bool allowPublication,
  std::function<std::shared_ptr<void>()> acquireOperationLifetime)
{
  require(user != nullptr, "MODEL_REPO_USER_MISSING");
  nativeValidateModelRepositoryTarget(repository, user->getName());
  const repo::RepoArtifactTarget target{
    ndn::Name(repository.repoNode).toUri(ndn::name::UriFormat::ALTERNATE),
    ndn::Name(repository.providerIdentity).toUri(ndn::name::UriFormat::ALTERNATE),
    ndn::Name(repository.servicePrefix).toUri(ndn::name::UriFormat::ALTERNATE)};
  const auto current = [deadline, requireCurrent = std::move(requireCurrent)] {
    if (requireCurrent) requireCurrent();
    require(std::chrono::steady_clock::now() < deadline, "MODEL_REPO_STORE_DEADLINE");
  };
  const auto budget = [deadline, current] {
    return remainingRepoBudget(deadline, current);
  };
  const auto verify = [](const ndn::Data& packet) {
    return packet.getSignatureType() == ndn::tlv::DigestSha256 &&
      ndn::security::verifySignature(packet, std::optional<ndn::security::Certificate>{});
  };
  current();
  NativeStoredModelPacketIo io;
  io.requireCurrent = current;
  if (allowPublication) {
    io.requireCurrent = [user, current] {
      current();
      // Reject the owner before it can wait for asynchronous commits that need
      // this same Face to run. Worker-side checks alone would miss that deadlock.
      require(!user->isOnIoThread(), "MODEL_REPO_STORE_ON_FACE_THREAD");
    };
  }
  io.fetch = [user, target, budget, current, verify, acquireOperationLifetime] (
    const std::vector<ndn::Name>& names) {
    auto operationLifetime = acquireOperationLifetime ? acquireOperationLifetime() : nullptr;
    return repo::RepoClient::fetchOriginalDataPackets(user, target, names,
      NATIVE_STORED_MODEL_UPLOAD_WIRE,
      verify, budget(), current, std::move(operationLifetime));
  };
  if (allowPublication) {
    io.requireCommitted = [user, target, budget, current](const ndn::Name& name,
      const std::string& expectedDigest, std::uint64_t expectedBytes) {
      current();
      require(user->getName().isPrefixOf(name) && user->getName() != name &&
        name.toUri(ndn::name::UriFormat::CANONICAL).size() <= 4096 &&
        expectedBytes > 0 && expectedBytes <= NATIVE_STORED_MODEL_UPLOAD_WIRE &&
        expectedDigest.size() == 71 && expectedDigest.compare(0, 7, "sha256:") == 0 &&
        std::all_of(expectedDigest.begin() + 7, expectedDigest.end(), [](unsigned char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }), "MODEL_REPO_RECOVERY_REFERENCE_INVALID");
      const auto objectName = name.toUri(ndn::name::UriFormat::ALTERNATE);
      const auto timeout = budget();
      const auto wire = repo::RepoClient::requestSelectedService(*user,
        ndn::Name(target.providerIdentity), repo::makeRepoServiceName(
          ndn::Name(target.servicePrefix), "MANIFEST"), repo::toBytes(objectName),
        std::min(MAX_ACK_COLLECTION_MS, std::max(1, timeout / 2)), timeout, {}, current);
      require(!wire.empty() && wire.size() <= (1U << 20), "MODEL_REPO_RECOVERY_MANIFEST_BUDGET");
      const auto text = repo::toString(wire);
      const auto manifest = repo::parseManifestJson(text);
      require(manifest.toJson() == text && ndn::Name(manifest.objectName) == name &&
        manifest.objectType == "original-packet-set" && manifest.lifecycleState == "COMMITTED" &&
        !manifest.operationId.empty() && manifest.generation > 0 &&
        manifest.size == expectedBytes && "sha256:" + manifest.sha256 == expectedDigest &&
        !manifest.packetNames.empty() && ndn::Name(manifest.packetNames.front()) == name,
        "MODEL_REPO_RECOVERY_COMMIT_MISMATCH");
      current();
    };
    const auto commit = [user, target, deadline, current, verify, acquireOperationLifetime](
      NativeStoredModelPacketIo::PacketWires wires, const std::string& expectedDigest,
      const std::function<void()>& additionalCurrent) {
      const auto callCurrent = [current, additionalCurrent] {
        current();
        if (additionalCurrent) additionalCurrent();
      };
      const auto callBudget = [deadline, callCurrent] {
        return remainingRepoBudget(deadline, callCurrent);
      };
      callCurrent();
      const auto lifetime = acquireOperationLifetime ? acquireOperationLifetime() : nullptr;
      require(!user->isOnIoThread(), "MODEL_REPO_STORE_ON_FACE_THREAD");
      require(!wires.empty() && wires.size() <= NATIVE_STORED_MODEL_UPLOAD_PACKETS,
              "MODEL_REPO_STORE_GROUP_BUDGET");
      require(expectedDigest.size() == 71 && expectedDigest.compare(0, 7, "sha256:") == 0 &&
        std::all_of(expectedDigest.begin() + 7, expectedDigest.end(), [](unsigned char ch) {
          return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        }), "MODEL_REPO_STORE_DIGEST_INVALID");
      const auto producer = user->getName();
      PacketGroup group;
      group.reference.dataPrefix = producer.toUri(ndn::name::UriFormat::ALTERNATE);
      group.reference.packetSetMetadata.emplace();
      auto& metadata = *group.reference.packetSetMetadata;
      metadata.objectType = "original-packet-set";
      metadata.packetOffsets = {0};
      std::set<ndn::Name> names;
      for (const auto& wire : wires) {
        callCurrent();
        require(!wire.empty() && wire.size() <= ndn::MAX_NDN_PACKET_SIZE &&
          wire.size() <= NATIVE_STORED_MODEL_UPLOAD_WIRE - group.reference.expectedSize,
          "MODEL_REPO_STORE_BYTE_BUDGET");
        const ndn::Data packet(ndn::Block(ndn::span<const std::uint8_t>(wire.data(), wire.size())));
        const auto name = packet.getName().toUri(ndn::name::UriFormat::ALTERNATE);
        require(packet.wireEncode().size() == wire.size() && producer.isPrefixOf(packet.getName()) &&
          packet.getName() != producer && names.insert(packet.getName()).second &&
          name.size() <= 4096 &&
          name.size() <= NATIVE_STORED_MODEL_UPLOAD_NAMES - group.nameBytes,
          "MODEL_REPO_STORE_PACKET_INVALID");
        if (metadata.packetNames.empty())
          group.reference.objectName = metadata.objectName = name;
        group.nameBytes += name.size();
        group.reference.expectedSize += wire.size();
        metadata.packetNames.push_back(name);
        metadata.packetOffsets.push_back(group.reference.expectedSize);
      }
      group.wires = std::move(wires);
      // RepoClient validates every signature and the supplied aggregate digest
      // before installing the Producer filter or emitting INSERT. An authorized
      // GRANT may precede rejection; unchecked packets never become available.
      commitPacketGroup(*user, target, group, verify, callBudget, callCurrent, 2,
                        expectedDigest.substr(7));
      callCurrent();
    };
    io.commitConcurrent = commit;
    io.commit = [commit](NativeStoredModelPacketIo::PacketWires wires,
                        const std::string& expectedDigest) {
      commit(std::move(wires), expectedDigest, {});
    };
  }
  return io;
}

NativeModelRepositoryReceipt
nativeStorePreparedModelPublication(ndn_service_framework::ServiceUser& writer,
  const ndn::security::Certificate& certificate,
  const ndn_service_framework::EncryptedLargeDataRangeStore& source,
  const NativePreparedCanonicalPublication& publication,
  const repo::RepoArtifactTarget& repository,
  std::chrono::steady_clock::time_point deadline, const std::function<void()>& requireCurrent)
{
  const auto current = [&] {
    if (requireCurrent) requireCurrent();
    require(std::chrono::steady_clock::now() < deadline, "MODEL_REPO_STORE_DEADLINE");
  };
  current();
  publication.validate();
  const auto producer = writer.getName();
  nativeValidateModelRepositoryTarget(repository, producer);
  const repo::RepoArtifactTarget target{
    ndn::Name(repository.repoNode).toUri(ndn::name::UriFormat::ALTERNATE),
    ndn::Name(repository.providerIdentity).toUri(ndn::name::UriFormat::ALTERNATE),
    ndn::Name(repository.servicePrefix).toUri(ndn::name::UriFormat::ALTERNATE)};
  require(!writer.isOnIoThread(), "MODEL_REPO_STORE_ON_FACE_THREAD");
  require(certificate.getIdentity() == producer && certificate.getValidityPeriod().isValid() &&
    source.supportsDurableRetention() && source.supportsOriginalDataPackets() &&
    publication.ownerRecoverableObjects && !publication.ownerRecoverableObjects->empty() &&
    publication.ownerRecoverableObjects->size() <= MAX_MODEL_OBJECTS,
    "MODEL_REPO_STORE_OWNER_OR_SOURCE_INVALID");
  require(producer.isPrefixOf(ndn::Name(publication.rootDataName)) &&
    producer.isPrefixOf(ndn::Name(publication.preparedMetadataDataName)),
    "MODEL_REPO_STORE_PUBLICATION_PRODUCER_MISMATCH");
  require(publication.missingDataNames.empty(), "MODEL_REPO_STORE_INCOMPLETE_PUBLICATION");
  const ndn_service_framework::LargeDataPublishResult* root = nullptr;
  const ndn_service_framework::LargeDataPublishResult* metadata = nullptr;
  std::set<ndn::Name> names;
  for (const auto& object : *publication.ownerRecoverableObjects) {
    require(object.success && producer.isPrefixOf(object.manifestName) &&
      producer.isPrefixOf(object.dataPrefix) && names.insert(object.manifestName).second,
      "MODEL_REPO_STORE_PUBLICATION_PRODUCER_MISMATCH");
    if (object.manifestName == ndn::Name(publication.rootDataName)) root = &object;
    if (object.manifestName == ndn::Name(publication.preparedMetadataDataName)) metadata = &object;
  }
  require(root && metadata && root != metadata &&
    root->contentDigest == publication.manifestDigest &&
    root->plaintextSize == publication.canonicalManifestJson.size() &&
    metadata->contentDigest == publication.preparedMetadataDigest &&
    metadata->plaintextSize == publication.preparedMetadataBytes,
    "MODEL_REPO_STORE_PUBLICATION_REFERENCE_MISMATCH");
  const auto requireReference = [&](const std::string& name) {
    if (!name.empty()) require(names.count(ndn::Name(name)) != 0,
                              "MODEL_REPO_STORE_INCOMPLETE_PUBLICATION");
  };
  for (const auto& name : {publication.sourceDataName, publication.initializerDataName,
                         publication.materialManifestDataName, publication.materialReceiptDataName})
    requireReference(name);
  for (const auto& name : publication.layerDataNames) requireReference(name);
  for (const auto& name : publication.materialDataNames) requireReference(name);
  NativeModelRepositoryReceipt receipt;
  receipt.producer = producer;
  receipt.certificateName = certificate.getName();
  receipt.rootName = ndn::Name(publication.rootDataName);
  receipt.metadataName = ndn::Name(publication.preparedMetadataDataName);
  receipt.rootDigest = publication.manifestDigest;
  receipt.metadataDigest = publication.preparedMetadataDigest;
  NativeModelCommitPipeline pipeline(current);
  std::uint64_t submittedGroups = 0;
  std::uint64_t submittedBytes = 0;
  PacketGroup group;
  const auto flush = [&] {
    if (group.wires.empty()) return;
    current();
    require(submittedGroups < MAX_PACKET_GROUPS, "MODEL_REPO_STORE_GROUP_BUDGET");
    const auto bytes = group.reference.expectedSize;
    auto pending = std::make_shared<PacketGroup>(std::move(group));
    group = {};
    pipeline.submit(bytes, [&, pending, target, producer, current, deadline]
      (const NativeModelCommitPipeline::CurrentCheck& additionalCurrent) {
        const auto callCurrent = [current, additionalCurrent] {
          current();
          additionalCurrent();
        };
        const auto budget = [deadline, callCurrent] {
          return remainingRepoBudget(deadline, callCurrent);
        };
        commitPacketGroup(writer, target, *pending, [&](const ndn::Data& packet) {
          const auto& wire = packet.wireEncode();
          return writer.validateProducerPacketWireUntil(
            {wire.begin(), wire.end()}, producer,
            std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(5)));
        }, budget, callCurrent, 1, packetDigest(pending->wires));
      }, [&receipt, bytes] {
        receipt.wireBytes += bytes;
        ++receipt.packetGroups;
      });
    submittedBytes += bytes;
    ++submittedGroups;
  };
  const auto append = [&](const ndn::Name& name, std::vector<std::uint8_t> wire) {
    current();
    const auto text = name.toUri(ndn::name::UriFormat::ALTERNATE);
    require(producer.isPrefixOf(name) && !wire.empty() &&
      wire.size() <= ndn::MAX_NDN_PACKET_SIZE && text.size() <= 4096,
      "MODEL_REPO_STORE_PACKET_INVALID");
    const ndn::Data packet(ndn::Block(ndn::span<const std::uint8_t>(wire.data(), wire.size())));
    require(packet.getName() == name && packet.wireEncode().size() == wire.size(),
            "MODEL_REPO_STORE_PACKET_NAME_MISMATCH");
    if (group.wires.size() == MAX_GROUP_PACKETS ||
        wire.size() > MAX_GROUP_WIRE - group.reference.expectedSize ||
        text.size() > MAX_GROUP_NAMES - group.nameBytes)
      flush();
    require(submittedBytes <= repo::REPO_USER_STORAGE_LIMIT_BYTES &&
      group.reference.expectedSize <= repo::REPO_USER_STORAGE_LIMIT_BYTES - submittedBytes &&
      wire.size() <= repo::REPO_USER_STORAGE_LIMIT_BYTES - submittedBytes -
        group.reference.expectedSize, "MODEL_REPO_STORE_BYTE_BUDGET");
    if (group.wires.empty()) {
      group.reference.objectName = text;
      group.reference.dataPrefix = producer.toUri(ndn::name::UriFormat::ALTERNATE);
      group.reference.packetSetMetadata.emplace();
      auto& metadata = *group.reference.packetSetMetadata;
      metadata.objectName = text;
      metadata.objectType = "original-packet-set";
      metadata.packetOffsets = {0};
    }
    group.nameBytes += text.size();
    group.reference.expectedSize += wire.size();
    auto& metadata = *group.reference.packetSetMetadata;
    metadata.packetNames.push_back(text);
    metadata.packetOffsets.push_back(group.reference.expectedSize);
    group.wires.push_back(std::move(wire));
  };
  const auto& certificateWire = certificate.wireEncode();
  append(certificate.getName(), {certificateWire.begin(), certificateWire.end()});
  flush();
  const auto storeObject = [&](const auto& object) {
    current();
    const auto found = source.lookupDurableObject(object.publicationIdentity,
                                                 object.manifestName, current);
    require(found && found->source && found->source->isDurable() &&
      found->publicationIdentity == object.publicationIdentity &&
      found->contentDigest == object.contentDigest && found->plaintextSize == object.plaintextSize &&
      found->keyReferenceId == object.keyReferenceId &&
      found->keyReferenceVersion == object.keyReferenceVersion &&
      found->protectionEpoch == object.protectionEpoch &&
      found->ciphertextManifestDigest == object.ciphertextManifestDigest &&
      ndn::Name(found->manifestName) == object.manifestName &&
      ndn::Name(found->dataPrefix) == object.dataPrefix,
      "MODEL_REPO_STORE_SOURCE_REFERENCE_MISMATCH");
    auto wire = found->source->readOriginalDataPacket(object.manifestName);
    const ndn::Data packet(ndn::Block(ndn::span<const std::uint8_t>(wire.data(), wire.size())));
    const auto& content = packet.getContent();
    require(nativePlanningDigest(content.value(), content.value_size()) == object.manifestDigest,
            "MODEL_REPO_STORE_MANIFEST_DIGEST_MISMATCH");
    const auto manifest = ndn_service_framework::ProtectedBlobManifest::decode(
      {content.value(), content.value() + content.value_size()}, producer, object.manifestName);
    require(manifest.dataPrefix == object.dataPrefix &&
      manifest.plaintextBytes == object.plaintextSize, "MODEL_REPO_STORE_MANIFEST_MISMATCH");
    append(object.manifestName, std::move(wire));
    for (std::uint64_t segment = 0; segment < manifest.segmentCount; ++segment) {
      auto name = manifest.dataPrefix;
      name.appendSegment(segment);
      append(name, found->source->readOriginalDataPacket(name));
    }
    flush();
    ++receipt.protectedObjects;
  };
  for (const auto& object : *publication.ownerRecoverableObjects) {
    if (&object != root) storeObject(object);
  }
  pipeline.drain();
  storeObject(*root);
  pipeline.finish();
  current();
  return receipt;
}

} // namespace ndnsf::di
