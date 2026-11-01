#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"

#include <limits>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <functional>
#include <future>

namespace ndnsf::di::tests {
namespace {

std::string digest(char c) { return "sha256:" + std::string(64, c); }

ConversationStateBinding binding(std::uint64_t expires)
{
  DecodeStateIdentityV1 identity;
  identity.modelDigest = digest('1'); identity.graphSemanticDigest = digest('2');
  identity.artifactDigest = digest('3'); identity.adapterDigest = digest('4');
  identity.tokenizerDigest = digest('5'); identity.runnerDigest = digest('6');
  identity.roleName = "/Stage/0"; identity.roleSplitDigest = digest('7');
  identity.layerBegin = 0; identity.layerEnd = 4;
  identity.prefixDigest = digest('8'); identity.prefixTokenCount = 3;
  identity.positionDigest = digest('9'); identity.precision = "fp32";
  identity.layoutDigest = digest('a'); identity.stateSchemaDigest = digest('b');
  identity.stateComponentDigests = {digest('c'), digest('d')};
  identity.runtimeAbiDigest = digest('e'); identity.securityDomainDigest = digest('f');
  identity.providerIdentity = "/provider/A"; identity.providerBootId = "boot-a";
  identity.cacheEpoch = 7; identity.stateInferenceEpoch = 0;
  identity.requestId = "request-a"; identity.attemptEpoch = 1;
  identity.generationId = "generation-a";
  identity.validate();
  ConversationStateBinding result;
  result.conversationId = "conversation-a-0001"; result.contextEpoch = 1;
  result.serviceName = "/LLM/Pipeline/Generate";
  result.planRoleMapDigest = digest('0'); result.receiptDigest = digest('9');
  result.expiresAtMs = expires; result.identity = std::move(identity);
  result.validate();
  return result;
}

// The store owns adapter-handle retention, not model inference. Count actual
// release callbacks so expiration cannot merely hide an unreleased entry.
class RetentionRunner final : public NativeModelRunner {
public:
  std::size_t releases = 0;
  std::atomic<std::size_t> exports{0};
  bool releaseSucceeds = true;
  std::function<void()> releaseProbe;
  std::function<bool()> pauseProbe;
  std::function<bool()> cancelProbe;
  std::function<std::future<bool>()> prefetch;
  std::map<std::string, TensorBundle> run(const RoleExecutionContext&) override
  { throw std::logic_error("retention fixture must not run inference"); }
  bool supportsPortableConversationState() const override { return true; }
  NativePortableKvSnapshotV1 exportConversationState(
    const NativeConversationStateHandleV1&, const DecodeStateIdentityV1&,
    const NativePortableKvLimitsV1&) override
  {
    ++exports;
    throw std::logic_error("ordinary retention cleanup must not export KV");
  }
  bool releaseConversationState(const NativeConversationStateHandleV1&) override
  {
    ++releases;
    if (releaseProbe) releaseProbe();
    return releaseSucceeds;
  }
  bool pauseConversationStateToHost(const NativeConversationStateHandleV1&) override
  { return pauseProbe ? pauseProbe() : true; }
  std::future<bool> prefetchConversationStateToGpu(const NativeConversationStateHandleV1&) override
  { return prefetch ? prefetch() : std::async(std::launch::deferred, [] { return true; }); }
  bool cancelConversationStatePrefetch(const NativeConversationStateHandleV1&) override
  { return cancelProbe ? cancelProbe() : true; }
};

struct ClearRetentionCallbacksOnExit
{
  std::shared_ptr<RetentionRunner> runner;
  ~ClearRetentionCallbacksOnExit()
  {
    runner->releaseProbe = {};
    runner->pauseProbe = {};
    runner->cancelProbe = {};
    runner->prefetch = {};
  }
};

bool stage(ConversationStateStore& store, const ConversationStateBinding& b,
           const std::shared_ptr<RetentionRunner>& runner, bool opaque, std::uint64_t now)
{
  if (!opaque)
    return store.stagePromotion(b.identity.requestId, b.identity.roleName, b,
      TensorBundle{"state", std::vector<std::uint8_t>{1, 2, 3, 4}, 1}, now);
  NativeConversationStateHandleV1 state;
  state.opaque.providerIdentity = b.identity.providerIdentity;
  state.opaque.providerBootId = b.identity.providerBootId;
  state.opaque.sessionId = b.identity.requestId;
  state.opaque.role = b.identity.roleName;
  state.opaque.token = "ndnsf-device-state-v1:retention";
  state.conversationKey = "retention-fixture"; state.logicalBytes = 4;
  state.validate();
  return store.stageAdapterPromotion(b.identity.requestId, b.identity.roleName,
    b, std::move(state), runner, now);
}

bool available(ConversationStateStore& store, const ConversationStateBinding& b,
               bool opaque, std::uint64_t now)
{
  return opaque ? store.adapterHandle(b, now).has_value() : store.lookup(b, now).has_value();
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec189ConversationRetention)

BOOST_AUTO_TEST_CASE(PolicyBoundsAndDefaultRemainFinite)
{
  for (const auto ttl : {std::uint64_t{0}, std::uint64_t{3600001},
                         std::numeric_limits<std::uint64_t>::max()}) {
    BOOST_CHECK_THROW(ConversationStateStore(16, 2, 16, 2, ttl), std::invalid_argument);
  }
  BOOST_CHECK_NO_THROW(ConversationStateStore(16, 2, 16, 2, 1));
  BOOST_CHECK_NO_THROW(ConversationStateStore(16, 2, 16, 2, 3600000));
  ConversationStateStore store(16, 2);
  const auto b = binding(900000);
  auto runner = std::make_shared<RetentionRunner>();
  BOOST_REQUIRE(stage(store, b, runner, false, 1000));
  BOOST_REQUIRE(store.commitStagedPromotion(b, digest('8')));
  BOOST_CHECK(available(store, b, false, 300999));
  BOOST_CHECK_EQUAL(store.cleanupExpired(301000), 1U);
  BOOST_CHECK(!available(store, b, false, 301000));
}

BOOST_AUTO_TEST_CASE(HostAndAdapterKvSurviveFiveMinutesButExpireAtPolicyBoundary)
{
  for (bool opaque : {false, true}) {
    BOOST_TEST_CONTEXT("opaque=" << opaque) {
      // A later conversation deadline cannot extend the finite host policy.
      ConversationStateStore store(16, 2, 16, 2, 600000);
      store.setProviderBinding("/provider/A", "boot-a", 7);
      const auto b = binding(900000);
      auto runner = std::make_shared<RetentionRunner>();
      BOOST_REQUIRE(stage(store, b, runner, opaque, 1000));
      BOOST_REQUIRE(store.commitStagedPromotion(b, digest('8')));
      auto resumed = b;
      resumed.identity.requestId = "request-b";
      resumed.identity.generationId = "generation-b";
      BOOST_CHECK(available(store, resumed, opaque, 301001));
      BOOST_CHECK(available(store, resumed, opaque, 600999));
      BOOST_CHECK_EQUAL(store.cleanupExpired(600999), 0U);
      BOOST_CHECK_EQUAL(store.cleanupExpired(601000), 1U);
      BOOST_CHECK(!available(store, resumed, opaque, 601000));
      BOOST_CHECK_EQUAL(store.snapshot().entries, 0U);
      BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 0U);
      BOOST_CHECK_EQUAL(runner->releases, opaque ? 1U : 0U);
      BOOST_CHECK_EQUAL(runner->exports.load(), 0U);
    }
  }
}

BOOST_AUTO_TEST_CASE(EarlierAuthenticatedDeadlineAndStagedExpiryStillWin)
{
  for (bool opaque : {false, true}) {
    for (bool commit : {false, true}) {
      BOOST_TEST_CONTEXT("opaque=" << opaque << " committed=" << commit) {
        ConversationStateStore store(16, 2, 16, 2, 3600000);
        const auto b = binding(400000);
        auto runner = std::make_shared<RetentionRunner>();
        BOOST_REQUIRE(stage(store, b, runner, opaque, 1000));
        if (commit) BOOST_REQUIRE(store.commitStagedPromotion(b, digest('8')));
        else BOOST_CHECK_EQUAL(store.snapshot().committingEntries, 1U);
        BOOST_CHECK_EQUAL(store.cleanupExpired(399999), 0U);
        BOOST_CHECK_EQUAL(store.cleanupExpired(400000), 1U);
        BOOST_CHECK(!available(store, b, opaque, 400000));
        BOOST_CHECK_EQUAL(store.snapshot().entries, 0U);
        BOOST_CHECK_EQUAL(store.snapshot().committingEntries, 0U);
        BOOST_CHECK_EQUAL(runner->releases, opaque ? 1U : 0U);
        BOOST_CHECK_EQUAL(runner->exports.load(), 0U);
        BOOST_CHECK(!stage(store, b, runner, opaque, 400000));
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(DeadlineAdditionSaturatesForBothPromotionPaths)
{
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  for (bool opaque : {false, true}) {
    BOOST_TEST_CONTEXT("opaque=" << opaque) {
      ConversationStateStore store(16, 2, 16, 2, 3600000);
      const auto b = binding(maximum);
      auto runner = std::make_shared<RetentionRunner>();
      BOOST_REQUIRE(stage(store, b, runner, opaque, maximum - 100));
      BOOST_REQUIRE(store.commitStagedPromotion(b, digest('8')));
      BOOST_CHECK(available(store, b, opaque, maximum - 1));
      BOOST_CHECK_EQUAL(store.cleanupExpired(maximum), 1U);
      BOOST_CHECK(!available(store, b, opaque, maximum));
      BOOST_CHECK_EQUAL(runner->releases, opaque ? 1U : 0U);
    }
  }
}

BOOST_AUTO_TEST_CASE(LongRetentionDoesNotRelaxByteOrEntryCaps)
{
  auto runner = std::make_shared<RetentionRunner>();
  const auto b = binding(3601000);
  for (bool opaque : {false, true}) {
    ConversationStateStore tooSmall(3, 1, 3, 1, 3600000);
    BOOST_CHECK(!stage(tooSmall, b, runner, opaque, 1000));
    BOOST_CHECK_EQUAL(tooSmall.snapshot().entries, 0U);
    ConversationStateStore pinned(4, 1, 4, 1, 3600000);
    BOOST_REQUIRE(stage(pinned, b, runner, opaque, 1000));
    auto other = b; other.contextEpoch = 2;
    BOOST_CHECK(!stage(pinned, other, runner, opaque, 1000));
    BOOST_CHECK_EQUAL(pinned.snapshot().entries, 1U);
    BOOST_CHECK_EQUAL(pinned.snapshot().gpuBytes, 4U);
    BOOST_CHECK_EQUAL(pinned.cleanupExpired(3601000), 1U);
  }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(Spec204ImportedStateAdmission)

NativeConversationStateHandleV1 importedHandle(const ConversationStateBinding& b)
{
  NativeConversationStateHandleV1 state;
  state.opaque.providerIdentity = b.identity.providerIdentity;
  state.opaque.providerBootId = b.identity.providerBootId;
  state.opaque.sessionId = b.identity.requestId;
  state.opaque.role = b.identity.roleName;
  state.opaque.stateInferenceEpoch = b.identity.stateInferenceEpoch;
  state.opaque.token = "ndnsf-device-state-v1:imported";
  state.conversationKey = "imported-fixture";
  state.logicalBytes = 4;
  return state;
}

BOOST_AUTO_TEST_CASE(RejectsCapacityWithoutEvictingCommittedState)
{
  const auto b = binding(9000);
  auto runner = std::make_shared<RetentionRunner>();
  for (bool byteLimit : {false, true}) {
    ConversationStateStore store(byteLimit ? 4 : 16, byteLimit ? 4 : 1);
    store.setProviderBinding(b.identity.providerIdentity, b.identity.providerBootId,
                             b.identity.cacheEpoch);
    BOOST_REQUIRE(stage(store, b, runner, true, 1000));
    BOOST_REQUIRE(store.commitStagedPromotion(b, digest('a')));
    auto committed = b; committed.checkpointDigest = digest('a');
    auto next = b; next.contextEpoch = 2;
    const auto before = store.snapshot();
    BOOST_CHECK(!store.stageImportedAdapterState(next.identity.requestId,
      next.identity.roleName, next, importedHandle(next), runner, 1001,
      "/requester/A", nullptr));
    BOOST_CHECK_EQUAL(store.snapshot().entries, before.entries);
    BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, before.gpuBytes);
    BOOST_CHECK_EQUAL(store.snapshot().evictions, before.evictions);
    BOOST_CHECK_EQUAL(runner->releases, byteLimit ? 1U : 0U);
    BOOST_REQUIRE(store.adapterHandle(committed, 1001).has_value());
    BOOST_CHECK(store.release(committed));
  }
}

BOOST_AUTO_TEST_CASE(ChecksExactDestinationHandleAndConfiguredIncarnation)
{
  auto b = binding(9000);
  auto runner = std::make_shared<RetentionRunner>();
  ConversationStateStore store(16, 4);
  BOOST_CHECK(!store.stageImportedAdapterState(b.identity.requestId,
    b.identity.roleName, b, importedHandle(b), runner, 1000,
    "/requester/A", nullptr));
  store.setProviderBinding(b.identity.providerIdentity, b.identity.providerBootId,
                           b.identity.cacheEpoch);
  for (unsigned mismatch = 0; mismatch != 5; ++mismatch) {
    auto state = importedHandle(b);
    auto candidate = b;
    switch (mismatch) {
      case 0: state.opaque.sessionId = "different-request"; break;
      case 1: ++state.opaque.stateInferenceEpoch; break;
      case 2: state.opaque.providerBootId = "different-boot"; break;
      case 3: ++candidate.identity.cacheEpoch; break;
      case 4: candidate.checkpointDigest = digest('a'); break;
    }
    BOOST_CHECK(!store.stageImportedAdapterState(candidate.identity.requestId,
      candidate.identity.roleName, candidate, state, runner, 1000,
      "/requester/A", nullptr));
    BOOST_CHECK_EQUAL(store.snapshot().entries, 0U);
    BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 0U);
    BOOST_CHECK_EQUAL(runner->releases, 0U);
  }
}

BOOST_AUTO_TEST_CASE(RequiresCheckpointCommitAndRejectsDuplicateAdmission)
{
  const auto b = binding(9000);
  auto runner = std::make_shared<RetentionRunner>();
  ConversationStateStore store(16, 4);
  store.setProviderBinding(b.identity.providerIdentity, b.identity.providerBootId,
                           b.identity.cacheEpoch);
  BOOST_REQUIRE(store.stageImportedAdapterState(b.identity.requestId,
    b.identity.roleName, b, importedHandle(b), runner, 1000,
    "/requester/A", nullptr));
  BOOST_CHECK(!store.adapterHandle(b, 1000).has_value());
  BOOST_CHECK(!store.commitStagedPromotion(b));
  BOOST_CHECK(!store.unpin(b));
  BOOST_CHECK(!store.adapterHandle(b, 1000).has_value());
  BOOST_CHECK(!store.stageImportedAdapterState(b.identity.requestId,
    b.identity.roleName, b, importedHandle(b), runner, 1000,
    "/requester/A", nullptr));
  BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 4U);
  BOOST_REQUIRE(store.commitStagedPromotion(b, digest('a')));
  auto committed = b; committed.checkpointDigest = digest('a');
  BOOST_REQUIRE(store.adapterHandle(committed, 1001).has_value());
  BOOST_CHECK(store.release(committed));
  BOOST_CHECK_EQUAL(runner->releases, 1U);
  BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 0U);
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE(Spec204StateLeaseLifecycle)

BOOST_AUTO_TEST_CASE(ConversationUseCopiesKeepAdapterAliveThroughDeferredClear)
{
  auto store = std::make_shared<ConversationStateStore>(16, 4);
  auto runner = std::make_shared<RetentionRunner>();
  const auto b = binding(9000);
  BOOST_REQUIRE(stage(*store, b, runner, true, 1000));
  BOOST_REQUIRE(store->commitStagedPromotion(b, digest('a')));
  auto resumed = b;
  resumed.identity.requestId = "request-b";
  resumed.identity.generationId = "generation-b";
  auto lease = store->acquireConversationUse(resumed, 1001);
  BOOST_REQUIRE(lease);
  BOOST_REQUIRE(lease->state);
  BOOST_CHECK(lease->runner == runner);
  BOOST_CHECK(!lease->bundle);
  BOOST_CHECK_EQUAL(lease->binding.identity.requestId, b.identity.requestId);
  auto copy = lease;
  BOOST_CHECK(!store->unpin(resumed));
  BOOST_CHECK(!store->adapterHandle(resumed, 1001));
  BOOST_CHECK(!store->pauseToHost(resumed, 1001));
  BOOST_CHECK(!store->acquireConversationUse(resumed, 1001));
  BOOST_CHECK(!store->release(resumed));
  BOOST_CHECK_EQUAL(store->cleanupExpired(9000), 0U);
  store->clear();
  BOOST_CHECK_EQUAL(store->drainSnapshot().activeUses, 1U);
  std::weak_ptr<ConversationStateStore> weak = store;
  store.reset(); lease.reset();
  BOOST_CHECK(!weak.expired());
  BOOST_CHECK_EQUAL(runner->releases, 0U);
  copy.reset();
  BOOST_CHECK(weak.expired());
  BOOST_CHECK_EQUAL(runner->releases, 1U);
}

BOOST_AUTO_TEST_CASE(ConversationUseOwnsLegacyBundleAndReleasesBeforeNextTurn)
{
  auto store = std::make_shared<ConversationStateStore>(16, 4);
  const auto b = binding(9000);
  BOOST_REQUIRE(stage(*store, b, std::make_shared<RetentionRunner>(), false, 1000));
  BOOST_CHECK(!store->acquireConversationUse(b, 1001));
  BOOST_REQUIRE(store->commitStagedPromotion(b, digest('a')));
  auto lease = store->acquireConversationUse(b, 1001);
  BOOST_REQUIRE(lease);
  BOOST_REQUIRE(lease->bundle);
  BOOST_CHECK(!lease->state);
  BOOST_CHECK(!lease->runner);
  BOOST_CHECK(lease->bundle->payload == std::vector<std::uint8_t>({1, 2, 3, 4}));
  BOOST_CHECK(!store->lookup(b, 1001));
  BOOST_CHECK(!store->unpin(b));
  lease.reset();
  BOOST_CHECK_EQUAL(store->drainSnapshot().activeUses, 0U);
  BOOST_CHECK(store->acquireConversationUse(b, 1002).has_value());
  BOOST_CHECK(!store->acquireConversationUse(b, 9000));
  BOOST_CHECK_EQUAL(store->cleanupExpired(9000), 1U);
}

BOOST_AUTO_TEST_CASE(ConversationUseRequiresSharedStoreOwner)
{
  ConversationStateStore store(16, 4);
  const auto b = binding(9000);
  BOOST_REQUIRE(stage(store, b, std::make_shared<RetentionRunner>(), false, 1000));
  BOOST_REQUIRE(store.commitStagedPromotion(b, digest('a')));
  BOOST_CHECK_THROW(store.acquireConversationUse(b, 1001), std::runtime_error);
  BOOST_CHECK_EQUAL(store.drainSnapshot().activeUses, 0U);
}

BOOST_AUTO_TEST_CASE(CopiedSnapshotOwnsStoreAndPreventsMutationUntilFinalRelease)
{
  auto store = std::make_shared<ConversationStateStore>(16, 4);
  auto runner = std::make_shared<RetentionRunner>();
  auto b = binding(9000);
  BOOST_REQUIRE(stage(*store, b, runner, true, 1000));
  BOOST_REQUIRE(store->commitStagedPromotion(b, digest('a')));
  b.checkpointDigest = digest('a');
  auto differentRequest = b;
  differentRequest.identity.requestId = "different-request";
  BOOST_CHECK(!store->acquireAdapterSnapshot(differentRequest, 1001).has_value());
  auto lease = store->acquireAdapterSnapshot(b, 1001);
  BOOST_REQUIRE(lease.has_value());
  auto copy = lease;
  BOOST_CHECK(!store->pin(b, 1001));
  BOOST_CHECK(!store->unpin(b));
  BOOST_CHECK(!store->adapterHandle(b, 1001).has_value());
  BOOST_CHECK(!store->acquireAdapterSnapshot(b, 1001).has_value());
  BOOST_CHECK_EQUAL(store->cleanupExpired(9000), 0U);
  store->clear();
  BOOST_CHECK_EQUAL(store->drainSnapshot().activeSnapshots, 1U);
  BOOST_CHECK_EQUAL(store->drainSnapshot().retainedGpuBytes, 4U);
  BOOST_CHECK_EQUAL(runner->releases, 0U);
  std::weak_ptr<ConversationStateStore> weak = store;
  store.reset(); lease.reset();
  BOOST_CHECK(!weak.expired());
  BOOST_CHECK_EQUAL(runner->releases, 0U);
  copy.reset();
  BOOST_CHECK(weak.expired());
  BOOST_CHECK_EQUAL(runner->releases, 1U);
}

BOOST_AUTO_TEST_CASE(FinalSnapshotOwnerRequestsCleanupWithoutPriorClear)
{
  auto store = std::make_shared<ConversationStateStore>(16, 4);
  auto runner = std::make_shared<RetentionRunner>();
  auto b = binding(9000);
  BOOST_REQUIRE(stage(*store, b, runner, true, 1000));
  BOOST_REQUIRE(store->commitStagedPromotion(b, digest('a')));
  b.checkpointDigest = digest('a');
  auto lease = store->acquireAdapterSnapshot(b, 1001);
  BOOST_REQUIRE(lease.has_value());
  std::weak_ptr<ConversationStateStore> weak = store;
  store.reset();
  BOOST_CHECK(!weak.expired());
  BOOST_CHECK_EQUAL(runner->releases, 0U);
  lease.reset();
  BOOST_CHECK(weak.expired());
  BOOST_CHECK_EQUAL(runner->releases, 1U);
}

BOOST_AUTO_TEST_CASE(RebindWaitsForSnapshotAndThenAdmitsOnlyNewIncarnation)
{
  auto store = std::make_shared<ConversationStateStore>(16, 4);
  auto runner = std::make_shared<RetentionRunner>();
  auto b = binding(9000);
  store->setProviderBinding(b.identity.providerIdentity, b.identity.providerBootId, b.identity.cacheEpoch);
  BOOST_REQUIRE(stage(*store, b, runner, true, 1000));
  BOOST_REQUIRE(store->commitStagedPromotion(b, digest('a')));
  auto committed = b; committed.checkpointDigest = digest('a');
  auto lease = store->acquireAdapterSnapshot(committed, 1001);
  BOOST_REQUIRE(lease.has_value());
  auto next = b;
  next.identity.providerIdentity = "/provider/B";
  next.identity.providerBootId = "boot-b";
  next.identity.cacheEpoch = 8;
  BOOST_CHECK(store->setProviderBinding("/provider/B", "boot-b", 8));
  BOOST_CHECK(!store->stageImportedAdapterState(next.identity.requestId, next.identity.roleName,
    next, Spec204ImportedStateAdmission::importedHandle(next), runner, 1001,
    "/requester/A", nullptr));
  BOOST_CHECK(!store->drainSnapshot().complete);
  lease.reset();
  BOOST_CHECK(store->drainSnapshot().complete);
  BOOST_CHECK_EQUAL(runner->releases, 1U);
  BOOST_CHECK(!store->stageImportedAdapterState(b.identity.requestId, b.identity.roleName,
    b, Spec204ImportedStateAdmission::importedHandle(b), runner, 1001,
    "/requester/A", nullptr));
  BOOST_REQUIRE(store->stageImportedAdapterState(next.identity.requestId, next.identity.roleName,
    next, Spec204ImportedStateAdmission::importedHandle(next), runner, 1001,
    "/requester/A", nullptr));
  store->clear();
  BOOST_CHECK(store->drainSnapshot().complete);
  BOOST_CHECK_EQUAL(runner->releases, 2U);
}

BOOST_AUTO_TEST_CASE(FailedReleaseRetainsChargeAndRetriesOutsideMutex)
{
  auto store = std::make_shared<ConversationStateStore>(16, 4);
  auto runner = std::make_shared<RetentionRunner>();
  const auto b = binding(9000);
  BOOST_REQUIRE(stage(*store, b, runner, true, 1000));
  BOOST_REQUIRE(store->commitStagedPromotion(b, digest('a')));
  std::weak_ptr<ConversationStateStore> weak = store;
  runner->releaseProbe = [weak] {
    if (auto owner = weak.lock()) BOOST_CHECK(owner->drainSnapshot().closing);
  };
  runner->releaseSucceeds = false;
  store->clear();
  auto status = store->drainSnapshot();
  BOOST_CHECK(!status.complete);
  BOOST_CHECK_EQUAL(status.reason, "CLEANUP_FAILED");
  BOOST_CHECK_EQUAL(status.cleanupFailures, 1U);
  BOOST_CHECK_EQUAL(status.retainedGpuBytes, 4U);
  BOOST_CHECK_EQUAL(store->snapshot().entries, 1U);
  BOOST_CHECK(!store->release(b));
  BOOST_CHECK_EQUAL(store->snapshot().gpuBytes, 4U);
  BOOST_CHECK(!store->retryDeferredCleanup());
  BOOST_CHECK_EQUAL(store->snapshot().gpuBytes, 4U);
  runner->releaseSucceeds = true;
  BOOST_CHECK(store->retryDeferredCleanup());
  BOOST_CHECK_EQUAL(store->snapshot().entries, 0U);
  BOOST_CHECK_EQUAL(store->snapshot().gpuBytes, 0U);
  BOOST_CHECK_EQUAL(runner->releases, 3U);
}

BOOST_AUTO_TEST_CASE(LegacyPinDefersClearAndExpiryUntilPairedUnpin)
{
  auto store = std::make_shared<ConversationStateStore>(16, 4);
  auto runner = std::make_shared<RetentionRunner>();
  const auto b = binding(9000);
  BOOST_REQUIRE(stage(*store, b, runner, false, 1000));
  BOOST_REQUIRE(store->commitStagedPromotion(b, digest('a')));
  BOOST_REQUIRE(store->pin(b, 1001));
  store->clear();
  BOOST_CHECK_EQUAL(store->cleanupExpired(9000), 0U);
  BOOST_CHECK_EQUAL(store->drainSnapshot().activeUses, 1U);
  BOOST_CHECK_EQUAL(store->snapshot().gpuBytes, 4U);
  BOOST_CHECK(store->unpin(b));
  BOOST_CHECK(store->drainSnapshot().complete);
  BOOST_CHECK_EQUAL(store->snapshot().gpuBytes, 0U);
}

BOOST_AUTO_TEST_CASE(AdapterLaunchFailureReleasesReservationAndTransferOwnership)
{
  auto store = std::make_shared<ConversationStateStore>(4, 1, 4, 1);
  auto runner = std::make_shared<RetentionRunner>();
  runner->prefetch = [] () -> std::future<bool> {
    throw std::runtime_error("fixture-adapter-launch-failed");
  };
  auto b = binding(9000);
  BOOST_REQUIRE(stage(*store, b, runner, true, 1000));
  BOOST_REQUIRE(store->commitStagedPromotion(b, digest('a')));
  BOOST_REQUIRE(store->pauseToHost(b, 1001));
  BOOST_CHECK(!store->prefetchToGpu(b, 1001).get());
  const auto status = store->drainSnapshot();
  BOOST_CHECK(status.complete);
  BOOST_CHECK_EQUAL(status.activePrefetches, 0U);
  BOOST_CHECK_EQUAL(status.retainedGpuBytes, 0U);
  BOOST_CHECK_EQUAL(status.retainedHostBytes, 4U);
  BOOST_CHECK_EQUAL(store->snapshot().prefetchBytes, 0U);
  b.checkpointDigest = digest('a');
  auto lease = store->acquireAdapterSnapshot(b, 1002);
  BOOST_REQUIRE(lease.has_value());
  store->clear();
  lease.reset();
  BOOST_CHECK(store->drainSnapshot().complete);
  BOOST_CHECK_EQUAL(runner->releases, 1U);
}

BOOST_AUTO_TEST_CASE(DelayedPrefetchRetainsStoreAndRunnerThroughCancellationCompletion)
{
  auto store = std::make_shared<ConversationStateStore>(4, 1, 4, 1);
  auto runner = std::make_shared<RetentionRunner>();
  auto transfer = std::make_shared<std::promise<bool>>();
  auto launched = std::make_shared<std::promise<void>>();
  auto launchObserved = launched->get_future();
  runner->prefetch = [transfer, launched] {
    auto future = transfer->get_future();
    launched->set_value();
    return future;
  };
  const auto b = binding(9000);
  BOOST_REQUIRE(stage(*store, b, runner, true, 1000));
  BOOST_REQUIRE(store->commitStagedPromotion(b, digest('a')));
  BOOST_REQUIRE(store->pauseToHost(b, 1001));
  auto future = store->prefetchToGpu(b, 1001);
  struct CompleteTransferOnExit
  {
    std::shared_ptr<std::promise<bool>> promise;
    ~CompleteTransferOnExit()
    {
      try { promise->set_value(false); } catch (...) { }
    }
  } unwindCompletion{transfer};
  const bool launchReady = launchObserved.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  BOOST_REQUIRE(launchReady);
  BOOST_CHECK_EQUAL(store->drainSnapshot().activePrefetches, 1U);
  BOOST_CHECK_EQUAL(store->drainSnapshot().retainedGpuBytes, 4U);
  BOOST_CHECK_EQUAL(store->drainSnapshot().retainedHostBytes, 4U);
  BOOST_REQUIRE(store->cancelPrefetch(b));
  BOOST_CHECK_EQUAL(store->drainSnapshot().activePrefetches, 1U);
  BOOST_CHECK(!store->release(b));
  store->clear();
  BOOST_CHECK(!store->drainSnapshot().complete);
  BOOST_CHECK_EQUAL(runner->releases, 0U);
  std::weak_ptr<ConversationStateStore> weakStore = store;
  std::weak_ptr<RetentionRunner> weakRunner = runner;
  store.reset(); runner.reset();
  BOOST_CHECK(!weakStore.expired());
  BOOST_CHECK(!weakRunner.expired());
  transfer->set_value(true);
  BOOST_CHECK(!future.get());
  BOOST_CHECK(weakStore.expired());
  BOOST_CHECK(weakRunner.expired());
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE(Spec204StateRemoval)

BOOST_AUTO_TEST_CASE(ImportedAdapterRollbackReleasesOutsideMutexAndCountsOnce)
{
  ConversationStateStore store(4, 1);
  const auto b = binding(9000);
  auto runner = std::make_shared<RetentionRunner>();
  store.setProviderBinding(b.identity.providerIdentity, b.identity.providerBootId, b.identity.cacheEpoch);
  BOOST_REQUIRE(store.stageImportedAdapterState(b.identity.requestId, b.identity.roleName,
    b, Spec204ImportedStateAdmission::importedHandle(b), runner, 1000,
    "/requester/A", nullptr));
  ClearRetentionCallbacksOnExit resetCallbacks{runner};
  runner->releaseProbe = [&] {
    BOOST_CHECK_EQUAL(store.drainSnapshot().activeTransitions, 1U);
    BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 4U);
    BOOST_CHECK(!store.commitStagedPromotion(b, digest('a')));
    BOOST_CHECK(!store.rollbackStagedPromotion(b));
  };
  BOOST_CHECK(store.rollbackStagedPromotion(b));
  BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 0U);
  BOOST_CHECK_EQUAL(store.snapshot().promotionRollbacks, 1U);
  BOOST_CHECK_EQUAL(runner->releases, 1U);
  BOOST_CHECK(!store.rollbackStagedPromotion(b));
}

BOOST_AUTO_TEST_CASE(FailedRollbackRetainsChargeUntilExplicitRetry)
{
  ConversationStateStore store(4, 1);
  const auto b = binding(9000);
  auto runner = std::make_shared<RetentionRunner>();
  runner->releaseSucceeds = false;
  BOOST_REQUIRE(stage(store, b, runner, true, 1000));
  BOOST_CHECK(!store.rollbackStagedPromotion(b));
  BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 4U);
  BOOST_CHECK_EQUAL(store.snapshot().promotionRollbacks, 0U);
  BOOST_CHECK_EQUAL(store.drainSnapshot().cleanupFailures, 1U);
  store.clear();
  BOOST_CHECK_EQUAL(runner->releases, 1U);
  BOOST_CHECK(!store.retryDeferredCleanup());
  BOOST_CHECK_EQUAL(runner->releases, 2U);
  runner->releaseSucceeds = true;
  BOOST_CHECK(store.retryDeferredCleanup());
  BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 0U);
  BOOST_CHECK_EQUAL(store.snapshot().promotionRollbacks, 1U);
  BOOST_CHECK_EQUAL(runner->releases, 3U);
}

BOOST_AUTO_TEST_CASE(FailedAndThrowingReleaseKeepBytesAndRejectOrdinaryRetry)
{
  for (bool throws : {false, true}) {
    ConversationStateStore store(4, 1);
    const auto b = binding(9000);
    auto runner = std::make_shared<RetentionRunner>();
    BOOST_REQUIRE(stage(store, b, runner, true, 1000));
    BOOST_REQUIRE(store.commitStagedPromotion(b, digest('a')));
    runner->releaseSucceeds = false;
    ClearRetentionCallbacksOnExit resetCallbacks{runner};
  runner->releaseProbe = [&] {
      BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 4U);
      if (throws) throw std::runtime_error("fixture-release-failed");
    };
    BOOST_CHECK(!store.release(b));
    BOOST_CHECK_EQUAL(store.snapshot().entries, 1U);
    BOOST_CHECK_EQUAL(store.snapshot().cleanups, 0U);
    BOOST_CHECK(!store.release(b));
    store.clear();
    BOOST_CHECK_EQUAL(runner->releases, 1U);
    BOOST_CHECK_EQUAL(store.cleanupExpired(9000), 0U);
    runner->releaseProbe = {};
    runner->releaseSucceeds = true;
    BOOST_CHECK(store.retryDeferredCleanup());
    BOOST_CHECK_EQUAL(store.snapshot().cleanups, 1U);
    BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 0U);
  }
}

BOOST_AUTO_TEST_CASE(FailedEvictionCannotAdmitAnotherStateOrFreeItsCharge)
{
  ConversationStateStore store(4, 1);
  const auto b = binding(9000);
  auto next = b; next.conversationId = "conversation-b-0002";
  auto runner = std::make_shared<RetentionRunner>();
  BOOST_REQUIRE(stage(store, b, runner, true, 1000));
  BOOST_REQUIRE(store.commitStagedPromotion(b, digest('a')));
  runner->releaseSucceeds = false;
  BOOST_CHECK(!stage(store, next, runner, true, 1001));
  BOOST_CHECK_EQUAL(store.snapshot().entries, 1U);
  BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 4U);
  BOOST_CHECK_EQUAL(store.snapshot().evictions, 0U);
  BOOST_CHECK_EQUAL(runner->releases, 1U);
  runner->releaseSucceeds = true;
  BOOST_REQUIRE(store.retryDeferredCleanup());
  BOOST_CHECK_EQUAL(store.snapshot().evictions, 1U);
  BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 0U);
  BOOST_CHECK_EQUAL(runner->exports.load(), 0U);
}

BOOST_AUTO_TEST_CASE(ReentrantClearInvalidatesSameIncarnationAdmission)
{
  ConversationStateStore store(4, 1);
  const auto b = binding(9000);
  auto next = b; next.conversationId = "conversation-b-0002";
  auto runner = std::make_shared<RetentionRunner>();
  BOOST_REQUIRE(stage(store, b, runner, true, 1000));
  BOOST_REQUIRE(store.commitStagedPromotion(b, digest('a')));
  ClearRetentionCallbacksOnExit resetCallbacks{runner};
  runner->releaseProbe = [&] {
    BOOST_CHECK(!store.pin(b, 1001));
    store.clear();
    BOOST_CHECK(!store.drainSnapshot().complete);
  };
  BOOST_CHECK(!stage(store, next, runner, true, 1001));
  BOOST_CHECK(store.drainSnapshot().complete);
  BOOST_CHECK_EQUAL(store.snapshot().entries, 0U);
  BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 0U);
  BOOST_CHECK_EQUAL(store.snapshot().evictions, 1U);
}

BOOST_AUTO_TEST_CASE(PauseReservesHostCapacityBeforeReentrantAdapterCallback)
{
  ConversationStateStore store(8, 2, 4, 1);
  const auto b = binding(9000);
  auto other = b; other.conversationId = "conversation-b-0002";
  auto runner = std::make_shared<RetentionRunner>();
  auto otherRunner = std::make_shared<RetentionRunner>();
  BOOST_REQUIRE(stage(store, b, runner, true, 1000));
  BOOST_REQUIRE(store.commitStagedPromotion(b, digest('a')));
  BOOST_REQUIRE(stage(store, other, otherRunner, true, 1000));
  BOOST_REQUIRE(store.commitStagedPromotion(other, digest('a')));
  ClearRetentionCallbacksOnExit resetCallbacks{runner};
  runner->pauseProbe = [&] {
    BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 8U);
    BOOST_CHECK_EQUAL(store.snapshot().hostBytes, 4U);
    BOOST_CHECK_EQUAL(store.drainSnapshot().activeTransitions, 1U);
    BOOST_CHECK(!store.pauseToHost(other, 1001));
    BOOST_CHECK(!store.release(b));
    return true;
  };
  BOOST_CHECK(store.pauseToHost(b, 1001));
  BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 4U);
  BOOST_CHECK_EQUAL(store.snapshot().hostBytes, 4U);
  BOOST_CHECK_EQUAL(store.drainSnapshot().activeTransitions, 0U);
}

BOOST_AUTO_TEST_CASE(PauseSettlesActualHostResidencyBeforeDeferredClear)
{
  ConversationStateStore store(4, 1, 4, 1);
  const auto b = binding(9000);
  auto runner = std::make_shared<RetentionRunner>();
  BOOST_REQUIRE(stage(store, b, runner, true, 1000));
  BOOST_REQUIRE(store.commitStagedPromotion(b, digest('a')));
  bool hostChargeObserved = false;
  ClearRetentionCallbacksOnExit resetCallbacks{runner};
  runner->pauseProbe = [&] { store.clear(); return true; };
  runner->releaseProbe = [&] {
    const auto state = store.drainSnapshot();
    hostChargeObserved = state.retainedGpuBytes == 0 && state.retainedHostBytes == 4;
  };
  BOOST_CHECK(!store.pauseToHost(b, 1001));
  BOOST_CHECK(hostChargeObserved);
  BOOST_CHECK_EQUAL(runner->releases, 1U);
  BOOST_CHECK(store.drainSnapshot().complete);
  BOOST_CHECK_EQUAL(store.snapshot().hostBytes, 0U);
}

BOOST_AUTO_TEST_CASE(ThrowingPauseReleasesOnlyDestinationReservation)
{
  ConversationStateStore store(4, 1, 4, 1);
  const auto b = binding(9000);
  auto runner = std::make_shared<RetentionRunner>();
  BOOST_REQUIRE(stage(store, b, runner, true, 1000));
  BOOST_REQUIRE(store.commitStagedPromotion(b, digest('a')));
  ClearRetentionCallbacksOnExit resetCallbacks{runner};
  runner->pauseProbe = [] () -> bool { throw std::runtime_error("fixture-pause-failed"); };
  BOOST_CHECK(!store.pauseToHost(b, 1001));
  BOOST_CHECK_EQUAL(store.snapshot().gpuBytes, 4U);
  BOOST_CHECK_EQUAL(store.snapshot().hostBytes, 0U);
  BOOST_CHECK_EQUAL(store.drainSnapshot().activeTransitions, 0U);
  BOOST_CHECK_EQUAL(runner->releases, 0U);
  BOOST_CHECK(store.release(b));
}

BOOST_AUTO_TEST_CASE(ExpiryRefindsEntriesAfterReentrantClear)
{
  ConversationStateStore store(8, 2);
  const auto b = binding(9000);
  auto other = b; other.conversationId = "conversation-b-0002";
  auto runner = std::make_shared<RetentionRunner>();
  BOOST_REQUIRE(stage(store, b, runner, true, 1000));
  BOOST_REQUIRE(store.commitStagedPromotion(b, digest('a')));
  BOOST_REQUIRE(stage(store, other, runner, true, 1000));
  BOOST_REQUIRE(store.commitStagedPromotion(other, digest('a')));
  ClearRetentionCallbacksOnExit resetCallbacks{runner};
  runner->releaseProbe = [&] { store.clear(); };
  BOOST_CHECK_EQUAL(store.cleanupExpired(9000), 1U);
  BOOST_CHECK_EQUAL(runner->releases, 2U);
  BOOST_CHECK_EQUAL(store.snapshot().cleanups, 2U);
  BOOST_CHECK_EQUAL(store.snapshot().entries, 0U);
}

BOOST_AUTO_TEST_CASE(PrefetchRefindsTargetAfterEvictionCallbackClearsStore)
{
  auto store = std::make_shared<ConversationStateStore>(4, 1, 4, 1);
  const auto b = binding(9000);
  auto other = b; other.conversationId = "conversation-b-0002";
  auto hostRunner = std::make_shared<RetentionRunner>();
  auto gpuRunner = std::make_shared<RetentionRunner>();
  BOOST_REQUIRE(stage(*store, b, hostRunner, true, 1000));
  BOOST_REQUIRE(store->commitStagedPromotion(b, digest('a')));
  BOOST_REQUIRE(store->pauseToHost(b, 1001));
  BOOST_REQUIRE(stage(*store, other, gpuRunner, true, 1000));
  BOOST_REQUIRE(store->commitStagedPromotion(other, digest('a')));
  std::size_t launches = 0;
  hostRunner->prefetch = [&] { ++launches; return std::async(std::launch::deferred, [] { return true; }); };
  ClearRetentionCallbacksOnExit resetCallbacks{gpuRunner};
  gpuRunner->releaseProbe = [&] { store->clear(); };
  BOOST_CHECK(!store->prefetchToGpu(b, 1001).get());
  BOOST_CHECK_EQUAL(launches, 0U);
  BOOST_CHECK_EQUAL(store->snapshot().entries, 0U);
  BOOST_CHECK_EQUAL(store->snapshot().gpuBytes, 0U);
  BOOST_CHECK_EQUAL(store->snapshot().hostBytes, 0U);
}

BOOST_AUTO_TEST_CASE(CancelCallbackRetainsStateAfterTransferFutureSettles)
{
  auto store = std::make_shared<ConversationStateStore>(4, 1, 4, 1);
  auto runner = std::make_shared<RetentionRunner>();
  auto transfer = std::make_shared<std::promise<bool>>();
  auto launched = std::make_shared<std::promise<void>>();
  auto launchObserved = launched->get_future();
  runner->prefetch = [transfer, launched] {
    auto result = transfer->get_future();
    launched->set_value();
    return result;
  };
  const auto b = binding(9000);
  BOOST_REQUIRE(stage(*store, b, runner, true, 1000));
  BOOST_REQUIRE(store->commitStagedPromotion(b, digest('a')));
  BOOST_REQUIRE(store->pauseToHost(b, 1001));
  auto future = store->prefetchToGpu(b, 1001);
  struct CompleteTransferOnExit {
    std::shared_ptr<std::promise<bool>> promise;
    ~CompleteTransferOnExit() { try { promise->set_value(false); } catch (...) { } }
  } unwindCompletion{transfer};
  BOOST_REQUIRE(launchObserved.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  ClearRetentionCallbacksOnExit resetCallbacks{runner};
  runner->cancelProbe = [&] {
    store->clear();
    transfer->set_value(true);
    BOOST_CHECK(!future.get());
    BOOST_CHECK_EQUAL(store->drainSnapshot().activePrefetches, 0U);
    BOOST_CHECK_EQUAL(store->drainSnapshot().activeTransitions, 1U);
    BOOST_CHECK_EQUAL(store->snapshot().gpuBytes, 4U);
    BOOST_CHECK_EQUAL(store->snapshot().hostBytes, 0U);
    BOOST_CHECK_EQUAL(runner->releases, 0U);
    return true;
  };
  BOOST_REQUIRE(store->cancelPrefetch(b));
  BOOST_CHECK_EQUAL(runner->releases, 1U);
  BOOST_CHECK(store->drainSnapshot().complete);
  BOOST_CHECK_EQUAL(store->snapshot().gpuBytes, 0U);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndnsf::di::tests
