#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"

#include <limits>
#include <memory>
#include <stdexcept>

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
  std::map<std::string, TensorBundle> run(const RoleExecutionContext&) override
  { throw std::logic_error("retention fixture must not run inference"); }
  bool releaseConversationState(const NativeConversationStateHandleV1&) override
  { ++releases; return true; }
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
} // namespace ndnsf::di::tests
