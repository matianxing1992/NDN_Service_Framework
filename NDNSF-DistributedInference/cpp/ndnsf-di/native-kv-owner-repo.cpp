#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-owner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-restore.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/portable-kv-checkpoint.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/private-buffer-wiper.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/protected-kv-packets.hpp"

#include "ndn-service-framework/ServiceUser.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"

#include <ndn-cxx/util/scope.hpp>

#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ndnsf::di {
namespace {

namespace repo = ndnsf_distributed_repo;
using User = ndn_service_framework::ServiceUser;

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

struct OwnerContext
{
  std::shared_ptr<User> user;
  std::shared_ptr<void> coreOwner, activity;
  std::shared_ptr<NativeConversationCoordinator> conversations;
  std::function<void()> callerCurrent;
  std::chrono::steady_clock::time_point deadline;
  std::string conversation, checkpoint;

  void current() const
  {
    callerCurrent();
    const auto head = conversations->find(conversation);
    require(std::chrono::steady_clock::now() < deadline && head &&
      head->checkpoint.checkpointDigest == checkpoint && head->retentionDeadlineMs > nowMs(),
      "handoff original parent or deadline ended");
  }

  int remaining() const
  {
    current();
    return static_cast<int>(std::max<std::int64_t>(1,
      std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count()));
  }
};

struct PrivateCipher
{
  std::filesystem::path directory, file, plain;
  explicit PrivateCipher(const std::filesystem::path& parent, const std::string& operation)
    : directory(parent / ("kv-owner-" + operation)), file(directory / "state.encrypted"),
      plain(directory / "state.private")
  {
    require(std::filesystem::create_directory(directory), "handoff workspace already exists");
    require(::chmod(directory.c_str(), 0700) == 0, "handoff private workspace mode failed");
  }
  ~PrivateCipher()
  {
    std::error_code ignored;
    std::filesystem::remove(plain, ignored);
    std::filesystem::remove(file, ignored);
    std::filesystem::remove(directory, ignored);
  }
};

} // namespace

void
NativeKvHandoffOwner::validateReadBudget(std::uint64_t roleCount,
  const repo::RepoUserStoragePacketSetFetchLimits& limits)
{
  constexpr std::uint64_t MAX_TOTAL_LISTING_PAGES = 2048;
  constexpr std::uint64_t MAX_TOTAL_LISTED_OBJECTS = 204800;
  constexpr std::uint64_t MAX_TOTAL_LISTING_WIRE_BYTES = 192ULL << 20;
  constexpr std::uint64_t MAX_TOTAL_PACKET_COUNT = 16384;
  constexpr std::uint64_t MAX_TOTAL_PACKET_WIRE_BYTES = 256ULL << 20;
  constexpr std::uint64_t MAX_TOTAL_MANIFEST_WIRE_BYTES = 16ULL << 20;
  // Fetch uses initial/final claim scans; promotion rechecks every role once.
  // The fetch scans share one wire allowance; the final recheck has another.
  const auto fits = [roleCount](std::uint64_t perRole, std::uint64_t repetitions,
                                std::uint64_t maximum) {
    return perRole > 0 && perRole <= maximum / repetitions &&
      roleCount <= maximum / (perRole * repetitions);
  };
  require(roleCount > 0 &&
    fits(limits.maxListingPagesPerScan, 3, MAX_TOTAL_LISTING_PAGES) &&
    fits(limits.maxListedObjectsPerScan, 3, MAX_TOTAL_LISTED_OBJECTS) &&
    fits(limits.maxListingWireBytes, 2, MAX_TOTAL_LISTING_WIRE_BYTES) &&
    fits(limits.maxPacketCount, 1, MAX_TOTAL_PACKET_COUNT) &&
    fits(limits.maxPacketWireBytes, 1, MAX_TOTAL_PACKET_WIRE_BYTES) &&
    fits(limits.maxManifestWireBytes, 1, MAX_TOTAL_MANIFEST_WIRE_BYTES),
    "handoff aggregate Repo read budget exceeded");
}

NativeDurableStorageGrantRoleSubmissionV1
nativeGrantKvRoleStorage(User& user, const std::string& operationId,
  const NativeDurableStorageGrantRoleChallengeV1& challenge, std::uint64_t expiresAtMs,
  std::chrono::steady_clock::time_point deadline)
{
  const repo::RepoArtifactTarget target{challenge.repoTarget.repoNode,
    challenge.repoTarget.providerIdentity, challenge.repoTarget.servicePrefix};
  repo::RepoUserStorageGrantRequest request;
  request.grantId = operationId + "-" + nativePlanningDigest(challenge.roleName).substr(7);
  request.writerIdentity = challenge.writerIdentity;
  request.objectName = challenge.manifestName;
  request.expiresAtMs = expiresAtMs;
  const auto encoded = request.toJson();
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
    deadline - std::chrono::steady_clock::now()).count();
  require(remaining > 1 && remaining <= 10 * 60 * 1000, "handoff grant deadline expired");
  const auto budget = static_cast<int>(remaining);
  const auto wire = repo::RepoClient::requestSelectedService(user,
    ndn::Name(target.providerIdentity), repo::makeRepoServiceName(
      ndn::Name(target.servicePrefix), "USER_STORAGE_GRANT"),
    std::vector<std::uint8_t>(encoded.begin(), encoded.end()),
    std::min(1000, std::max(1, budget / 2)), budget);
  const auto granted = repo::RepoUserStorageGrantReply::fromJson(
    std::string(wire.begin(), wire.end()));
  granted.validateExpected(target, user.getName().toUri(ndn::name::UriFormat::ALTERNATE), request);
  return {challenge.roleName, target.repoNode, granted.grant.ownerIdentity,
    granted.grant.writerIdentity, granted.grant.objectName, granted.grant.grantId,
    granted.grant.ownerEpoch};
}

NativeConversationRecord
NativeKvHandoffOwner::verifyAndPromote(std::shared_ptr<User> user,
  std::shared_ptr<void> coreOwner, std::shared_ptr<void> activity,
  std::shared_ptr<NativeConversationCoordinator> conversations,
  std::shared_ptr<const NativeKvOwnerKeyOperations> keys, const std::string& operation,
  const std::string& conversation, const std::string& expectedCheckpoint,
  const NativeJson& references, const std::map<std::string, std::string>& destinations,
  const std::map<std::string, NativeRemoteProtectedKvRoleTrustPolicyV1>& trustedSourcePolicies,
  std::chrono::steady_clock::time_point deadline,
  std::function<void()> requireCurrent)
{
  require(user && coreOwner && activity && conversations && keys && keys->unwrap && requireCurrent &&
    !user->isOnIoThread() && keys->certificate.getIdentity() == user->getName() &&
    !operation.empty() && operation.size() <= 256 &&
    operation.find_first_not_of(
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._~-") == std::string::npos,
    "handoff actual Owner transport unavailable");
  auto context = std::make_shared<OwnerContext>();
  context->user = std::move(user);
  context->coreOwner = std::move(coreOwner);
  context->activity = std::move(activity);
  context->conversations = std::move(conversations);
  context->callerCurrent = std::move(requireCurrent);
  context->deadline = deadline;
  context->conversation = conversation;
  context->checkpoint = expectedCheckpoint;
  context->current();
  const auto initial = *context->conversations->find(conversation);
  require(!context->conversations->journalRoot().empty(), "handoff requires a durable journal");
  const auto checkpoint = nativeParseJson(initial.checkpoint.wire);
  nativeValidateKvNextPlacement(destinations, checkpoint);
  repo::RepoUserStoragePacketSetFetchLimits limits;
  limits.ackCollectionTimeMs = 1000;
  std::map<std::string, NativeSavedKvRoleV1> saved;
  require(references.is_object(), "handoff requires the complete saved role set");
  for (const auto& item : references.items())
    saved.emplace(item.key(), nativeReadSavedKvRole(item.value()));
  nativeValidateProtectedKvReferences(references, checkpoint, initial.checkpoint.providersByRole);
  validateReadBudget(saved.size(), limits);
  PrivateCipher file(context->conversations->journalRoot(), operation);
  std::vector<std::pair<repo::RepoArtifactTarget, repo::RepoUserStorageClaim>> claims;
  for (const auto& item : saved) {
    context->current();
    const auto& role = item.second;
    const auto& target = trustedSourcePolicies.at(item.first).target;
    require(role.repoNode == target.repoNode && role.repoProvider == target.providerIdentity &&
      role.repoServicePrefix == target.servicePrefix, "handoff pinned Repo target changed");
    const repo::RepoArtifactTarget replica{role.repoNode, role.repoProvider, role.repoServicePrefix};
    const auto verifier = [context, producer = role.protection.producerPrefix](const ndn::Data& data) {
      context->current();
      const auto& wire = data.wireEncode();
      return context->user->validateProducerPacketWireUntil(
        ndn::Buffer(wire.begin(), wire.end()), producer, context->deadline);
    };
    auto original = repo::RepoClient::fetchOriginalDataPacketSetFromUserStorage(*context->user,
      replica, role.rootName.toUri(), verifier, context->remaining(), limits);
    require(original.manifest().size == role.originalWireBytes &&
      original.manifest().sha256 == role.originalWireSha256 &&
      original.claim().ownerEpoch == role.ownerEpoch, "handoff actual Repo tuple changed");
    std::vector<ndn::Buffer> wires;
    for (const auto& wire : original.packetWires()) wires.emplace_back(wire.begin(), wire.end());
    NativeProtectedKvPacketLimits packets;
    packets.maxPackets = limits.maxPacketCount;
    packets.maxPacketWireBytes = limits.maxPacketWireBytes;
    packets.checkpoint = {256ULL * 1024 * 1024, 256ULL * 1024 * 1024, {}};
    nativeMaterializeProtectedKvPackets(wires, role.protection, role.rootName, file.file, packets,
      [context](const ndn::Buffer& wire, const ndn::Name& producer) {
        context->current();
        return context->user->validateProducerPacketWireUntil(wire, producer, context->deadline);
      }, [context] { context->current(); });
    const auto current = [context] { context->current(); };
    const auto protectionLimits = NativeProtectedKvCheckpointLimitsV1{
      256ULL * 1024 * 1024, 256ULL * 1024 * 1024,
      [current] { current(); return false; }};
    nativeRecoverProtectedPortableKvCheckpoint(file.file, file.plain, role.protection,
      [context, keys, reference = role.protection, current](const ndn::Buffer& wrapped) {
        return keys->unwrap(reference, wrapped, context->deadline, current);
      }, protectionLimits);
    {
      NativePortableKvCheckpointLimitsV1 portable;
      portable.portable = {128ULL * 1024 * 1024, 256ULL * 1024 * 1024,
        [current] { current(); return false; }};
      portable.maxMetadataBytes = 1024 * 1024;
      portable.maxCheckpointBytes = 256ULL * 1024 * 1024;
      auto snapshot = nativeReadPrivatePortableKvCheckpoint(file.plain, portable);
      const auto wipe = ndn::make_scope_exit([&snapshot] {
        for (auto& component : snapshot.components) {
          detail::PrivateBufferWiper clear(component.tensorWire);
        }
      });
      // The authenticated receipt commits the shared state identity. Backend
      // state-IO compatibility is additionally checked by the destination runner.
      nativeValidateSavedKvOrigin(snapshot.originIdentity, role.receipt);
    }
    require(std::filesystem::remove(file.plain), "handoff plaintext cleanup failed");
    claims.emplace_back(replica, original.claim());
  }
  for (const auto& item : claims) {
    context->current();
    (void)repo::RepoClient::revalidateCurrentUserStorageClaim(*context->user, item.first,
      item.second, context->remaining(), limits);
  }

  auto promoted = initial;
  promoted.protectedKvReferences = references;
  promoted.nextTurnProvidersByRole = destinations;
  context->conversations->promoteProtectedKvSet(conversation,
    expectedCheckpoint, references, destinations, [context] {
      context->callerCurrent();
      require(std::chrono::steady_clock::now() < context->deadline,
        "handoff deadline ended before durable promotion");
    });
  static_assert(std::is_nothrow_move_constructible_v<NativeConversationRecord>);
  return promoted;
}

} // namespace ndnsf::di
