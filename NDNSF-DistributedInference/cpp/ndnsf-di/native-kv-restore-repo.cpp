#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-restore-provider.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/private-buffer-wiper.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-restore.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-save.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/portable-kv-checkpoint.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/protected-kv-packets.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <ndn-cxx/util/scope.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <unistd.h>

namespace ndnsf::di {
namespace {

namespace repo = ndnsf_distributed_repo;

void
require(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}

std::uint64_t
nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

DecodeStateIdentityV1
destinationIdentity(const DecodeStateIdentityV1& origin, DecodeStateIdentityV1 trusted)
{
  trusted.prefixDigest = origin.prefixDigest;
  trusted.prefixTokenCount = origin.prefixTokenCount;
  trusted.positionDigest = origin.positionDigest;
  trusted.stateInferenceEpoch = origin.stateInferenceEpoch;
  trusted.predecessorInferenceEpoch = origin.predecessorInferenceEpoch;
  require(nativePortableKvIdentityCompatible(origin, trusted),
    "KV restore differs from current model/assembly/state contract");
  return trusted;
}

struct PrivateFiles
{
  std::filesystem::path directory, cipher, plain;
  explicit PrivateFiles(const std::filesystem::path& root)
  {
    auto pattern = (root / "kv-restore-XXXXXX").string();
    const auto created = ::mkdtemp(pattern.data());
    require(created != nullptr, "KV restore private workspace unavailable");
    directory = created;
    cipher = directory / "state.encrypted";
    plain = directory / "state.private";
  }
  ~PrivateFiles()
  {
    std::error_code ignored;
    std::filesystem::remove(plain, ignored);
    std::filesystem::remove(cipher, ignored);
    // An unconfirmed atomic rollback backup is deliberately retained.
    std::filesystem::remove(directory, ignored);
  }
};

} // namespace

std::unique_ptr<NativeKvRestoreAdmission>
nativeRestoreKvForInvocation(NativeProviderRuntime& runtime,
  ndn_service_framework::ServiceProvider::CollaborationContext& context,
  const NativeProviderHandoffConfig& policy, const NativeSelectionProjectionV3& projection,
  const DecodeStateIdentityV1& trustedTemplate,
  std::function<std::shared_ptr<NativeModelRunner>()> prepareRunner,
  const ConversationStateStore::ExecutionAdmission* admission,
  std::function<void()> requireCurrent)
{
  require(projection.protectedKvRestore && admission && prepareRunner && requireCurrent,
    "KV restore current invocation admission missing");
  nativeValidateKvSaveConfig(policy, context.localProvider());
  auto restore = nativeReadKvRestore(*projection.protectedKvRestore, projection,
    context.requesterName(), context.assignment().service, nowMs());
  auto selected = context.assignment().roleProviders;
  selected[context.role()] = context.localProvider();
  NativeJson selectedMap = NativeJson::array();
  for (const auto& item : selected)
    selectedMap.push_back(NativeJson::array({item.first, item.second.toUri()}));
  require(projection.conversationTurnBinding->planRoleMapDigest ==
    nativePlanningDigest(nativeCanonicalJson(selectedMap)),
    "KV restore target map differs from actual Core selection");
  require(restore.saved.repoNode == policy.repoNode &&
    ndn::Name(restore.saved.repoProvider) == ndn::Name(policy.repoProvider) &&
    ndn::Name(restore.saved.repoServicePrefix) == ndn::Name(policy.repoServicePrefix),
    "KV restore Repo differs from configured service policy");
  const auto deadline = context.assignment().workDeadline;
  const auto current = [&context, requireCurrent, deadline, expires = restore.recipient.expiresAtMs] {
    requireCurrent();
    require(context.hasCurrentWorkAuthority() && std::chrono::steady_clock::now() < deadline &&
      nowMs() < expires, "KV restore current authority or original deadline ended");
  };
  const auto remaining = [&] {
    current();
    const auto steady = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now()).count();
    const auto now = nowMs();
    require(restore.recipient.expiresAtMs > now, "KV restore recipient expired");
    const auto wall = restore.recipient.expiresAtMs - now;
    const auto milliseconds = std::min<std::uint64_t>(std::max<std::int64_t>(1, steady), wall);
    require(milliseconds > 1000, "KV restore has insufficient original transport budget");
    return static_cast<int>(std::min<std::uint64_t>(milliseconds, std::numeric_limits<int>::max()));
  };
  current();
  auto lease = std::make_unique<NativeKvRestoreAdmission>(runtime);
  const auto& parent = *projection.conversationStateReference;
  auto resolved = runtime.resolveConversationState(parent, nowMs(), context.requesterName().toUri());
  {
    // Recovery always admits the referenced original object and recipient
    // capsule. A matching private state can avoid importing, not validation.
    PrivateFiles files(policy.workspace);
    repo::RepoUserStoragePacketSetFetchLimits limits;
    limits.ackCollectionTimeMs = 1000;
    limits.maxPacketCount = 65536;
    limits.maxPacketWireBytes = 256ULL * 1024 * 1024;
    const auto root = restore.saved.rootName.toUri(ndn::name::UriFormat::ALTERNATE);
    const auto manifestBytes = repo::RepoClient::requestSelectedService(*policy.storageWriter,
      ndn::Name(policy.repoProvider), repo::makeRepoServiceName(ndn::Name(policy.repoServicePrefix),
        "MANIFEST"), std::vector<std::uint8_t>(root.begin(), root.end()),
      limits.ackCollectionTimeMs, remaining());
    current();
    require(!manifestBytes.empty() && manifestBytes.size() <= limits.maxManifestWireBytes,
      "KV restore manifest exceeds transport budget");
    const auto manifest = repo::parseManifestJson(std::string(manifestBytes.begin(), manifestBytes.end()));
    require(manifest.lifecycleState == "COMMITTED" && manifest.generation > 0 &&
      manifest.size == restore.saved.originalWireBytes &&
      manifest.sha256 == restore.saved.originalWireSha256,
      "KV restore Repo manifest differs from Owner commitment");
    repo::RepoDataReference reference;
    reference.objectName = root;
    reference.dataPrefix = restore.saved.protection.producerPrefix.toUri();
    reference.expectedSize = restore.saved.originalWireBytes;
    reference.expectedSha256 = restore.saved.originalWireSha256;
    reference.forwardingHint = ndn::Name(policy.repoProvider).toUri();
    reference.packetSetMetadata = manifest;
    const auto verifier = [writer = policy.storageWriter, current, deadline,
                           producer = restore.saved.protection.producerPrefix](const ndn::Data& data) {
      current();
      const auto& wire = data.wireEncode();
      return writer->validateProducerPacketWireUntil(ndn::Buffer(wire.begin(), wire.end()),
        producer, deadline);
    };
    const auto packets = repo::RepoClient::fetchOriginalDataPacketSet(*policy.storageWriter,
      reference, verifier, remaining(), limits, current);
    std::vector<ndn::Buffer> wires;
    for (const auto& wire : packets) wires.emplace_back(wire.begin(), wire.end());
    NativeProtectedKvPacketLimits packetLimits;
    packetLimits.checkpoint = {256ULL * 1024 * 1024, 256ULL * 1024 * 1024,
      [current] { current(); return false; }};
    packetLimits.maxPackets = limits.maxPacketCount;
    packetLimits.maxPacketWireBytes = limits.maxPacketWireBytes;
    nativeMaterializeProtectedKvPackets(wires, restore.saved.protection, restore.saved.rootName,
      files.cipher, packetLimits,
      [writer = policy.storageWriter, current, deadline](const ndn::Buffer& wire,
                                                      const ndn::Name& producer) {
        current();
        return writer->validateProducerPacketWireUntil(wire, producer, deadline);
      }, current);
    const NativeProtectedKvRecipientConsumerV1 consumer = [&context, deadline](const auto& capsule,
      const auto& certificate, const auto& keyDigest, const auto& aad, const auto& consume) {
      return context.consumeRecipientAssignmentCurrent(capsule, certificate, keyDigest, aad,
        deadline, consume);
    };
    nativeRecoverProtectedPortableKvCheckpointForRecipient(files.cipher, files.plain,
      restore.saved.protection, restore.capsule, restore.recipient, packetLimits.checkpoint,
      consumer, current);
    NativePortableKvCheckpointLimitsV1 portable;
    portable.portable = {policy.maxStateBytes, 256ULL * 1024 * 1024,
      [current] { current(); return false; }};
    portable.maxMetadataBytes = 1024 * 1024;
    portable.maxCheckpointBytes = 256ULL * 1024 * 1024;
    auto snapshot = nativeReadPrivatePortableKvCheckpoint(files.plain, portable);
    const auto wipe = ndn::make_scope_exit([&snapshot] {
      for (auto& component : snapshot.components) {
        detail::PrivateBufferWiper clear(component.tensorWire);
      }
    });
    nativeValidateSavedKvOrigin(snapshot.originIdentity, restore.saved.receipt);
    ConversationStateBinding destination;
    destination.conversationId = parent.conversationId;
    destination.contextEpoch = parent.contextEpoch;
    destination.serviceName = parent.serviceName;
    destination.planRoleMapDigest = parent.planRoleMapDigest;
    destination.receiptDigest = parent.roleReceiptDigest;
    destination.expiresAtMs = parent.expiresAtMs;
    destination.identity = destinationIdentity(snapshot.originIdentity, trustedTemplate);
    destination.importedOriginIdentity = snapshot.originIdentity;
    if (!resolved) {
      current();
      auto runner = prepareRunner();
      current();
      lease->ownership = runtime.importConversationStateOwned(snapshot, destination, runner,
        portable.portable, nowMs(), context.requesterName().toUri(), admission);
      require(lease->ownership.has_value(), "KV restore owned import rejected");
      current();
      require(runtime.commitImportedConversationState(*lease->ownership, parent.checkpointDigest),
        "KV restore owned import commit failed");
      resolved = runtime.resolveConversationState(parent, nowMs(), context.requesterName().toUri());
    }
    require(std::filesystem::remove(files.plain), "KV restore plaintext cleanup failed");
  }
  require(resolved.has_value(), "KV restore exact local parent missing");
  const auto& origin = resolved->importedOriginIdentity
    ? *resolved->importedOriginIdentity : resolved->identity;
  nativeValidateSavedKvOrigin(origin, restore.saved.receipt);
  (void)destinationIdentity(resolved->identity, trustedTemplate);
  require(resolved->identity.providerIdentity == trustedTemplate.providerIdentity &&
    resolved->identity.providerBootId == trustedTemplate.providerBootId &&
    resolved->identity.cacheEpoch == trustedTemplate.cacheEpoch,
    "KV restore local incarnation differs from current Provider");
  current();
  require(runtime.prefetchConversationStateToGpu(*resolved, nowMs()).get(),
    "KV restore state residency unavailable");
  current();
  lease->use = runtime.acquireConversationUse(*resolved, nowMs());
  require(lease->use && lease->use->binding == *resolved && lease->use->runner && lease->use->state,
    "KV restore current adapter state use unavailable");
  return lease;
}

} // namespace ndnsf::di
