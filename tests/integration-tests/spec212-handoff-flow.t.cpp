#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace ndnsf::di::tests::Spec204ColdProviderFlow {
void runSpec212ProductionAdmissionRejectsConcurrentSwitchAtEventCapacity();
void runSpec212ProductionAdmissionRejectsConcurrentSwitchAtRoleCapacity();
void runSpec212ControlProgressWhileRoleCallbackIsParked();
void runSpec212ControlProgressWhileTwoRolesAreParked();
void runSpec212ControlProgressAcrossTwoSourcesWhileParked();
void runSpec212TwoEventCapacityAcrossTwoSources();
void runSpec212TwoRoleCapacityAcrossTwoSources();
void runSpec212CloseDrainsWhileOwnerPacketSetBoundaryIsActive();
}

namespace ndnsf::di {
namespace {

constexpr char PROVIDER_NAME[] = "/spec212/provider/handoff-flow";
constexpr char OWNER_NAME[] = "/spec212/user/handoff-flow";
constexpr char SERVICE_NAME[] = "/spec212/service/handoff-flow";
constexpr char ROLE_NAME[] = "/Backbone";
using namespace std::chrono_literals;

class UnusedRunnerFactory final : public NativeModelRunnerFactory
{
public:
  std::shared_ptr<NativeModelRunner>
  create(const NativeModelRunnerSpec&) const override
  {
    throw std::logic_error("handoff admission test must not create a runner");
  }
};

class CountingPacketStore final :
  public ndn_service_framework::EncryptedLargeDataRangeStore
{
public:
  std::shared_ptr<const ndn_service_framework::EncryptedLargeDataRangeSource>
  commitFile(const std::string&, const std::filesystem::path&, std::uint64_t,
             const std::function<void()>&) override
  {
    ++commitCalls;
    return {};
  }

  std::atomic<unsigned> commitCalls{0};
};

NativeProviderHandlerConfig
makeConfig()
{
  NativeProviderHandlerConfig config;
  config.plan.executionPolicy = "DATA_DRIVEN_V2";
  config.executionPolicy = "DATA_DRIVEN_V2";
  config.localProviderName = PROVIDER_NAME;
  config.providerBootId = "spec212-handoff-flow-boot";
  config.runnerFactory = std::make_shared<UnusedRunnerFactory>();
  return config;
}

NativeServiceDefinition
makeService()
{
  NativeServiceDefinition service;
  service.serviceName = SERVICE_NAME;
  service.allowedRoles = {ROLE_NAME};
  service.ackHandler = [](const ndn_service_framework::RequestMessage&) {
    ndn_service_framework::ServiceProvider::AckDecision decision;
    decision.status = true;
    decision.message = "accepted";
    return decision;
  };
  return service;
}

NativeConversationHandoffOwnerContextV1
makeOwnerContext(const ndn::security::Certificate& ownerCertificate,
                 const std::shared_ptr<CountingPacketStore>& packetStore)
{
  NativeConversationHandoffOwnerContextV1 context;
  context.ownerEncryptionCertificate = ownerCertificate;
  context.durableStorageOwner.ownerIdentity = OWNER_NAME;
  context.durableStorageOwner.resolveGrant = [](
      const ndn::Name&, const std::string&,
      const std::function<void()>&)
      -> ndn_service_framework::DurableStorageGrantSelector {
    throw std::runtime_error("unexpected Repo grant resolution");
  };
  context.originalPacketStore = packetStore;
  context.saveBatchLimits = {1, 1024};
  context.portableLimits = {{1024, 2048, {}}, 1024, 1024};
  context.protectedCheckpointLimits = {1024, 2048, {}};
  context.policy.enabled = true;
  context.policy.maxConcurrentEvents = 2;
  context.policy.maxRolesPerEvent = 1;
  context.policy.maxConcurrentRoleSlots = 2;
  context.policy.maxPacketCountPerRole = 1;
  context.policy.maxPacketSetWireBytesPerRole = 4096;
  context.policy.maxAggregateBytes = 4096;
  context.policy.maxScratchBytesPerEvent = 4096;
  context.policy.scratchQuotaBytes = 8192;
  context.policy.maxSaveDurationMs = 300000;
  context.policy.scratchRoot = std::filesystem::temp_directory_path() /
    "ndnsf-spec212-handoff-flow";
  context.repoTargetByRole.emplace(ROLE_NAME,
    NativeDurableStorageGrantRepoTargetV1{
      "/spec212/repo", "/spec212/repo-provider", "/spec212/repo/service"});
  return context;
}

NativeConversationHandoffControlV1
makeControl()
{
  NativeConversationHandoffControlV1 control;
  control.operationId = "spec212-duplicate-operation-0001";
  control.conversationId = "spec212-duplicate-conversation-0001";
  control.checkpointDigest = "sha256:" + std::string(64, '6');
  control.sourceRoleMapDigest = "sha256:" + std::string(64, '7');
  control.destinationRoleMapDigest = "sha256:" + std::string(64, '8');
  control.deadlineMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 120000;
  return control;
}

ndn_service_framework::RequestMessage
makeRequest(const NativeConversationHandoffControlV1& control)
{
  const auto wire = nativeConversationCanonicalJson(
    nativeConversationHandoffControlV1ToJson(control));
  ndn::Buffer payload(wire.begin(), wire.end());
  ndn_service_framework::RequestMessage request;
  request.setPayload(payload, payload.size());
  return request;
}

} // namespace
} // namespace ndnsf::di

namespace ndn_service_framework::test {
namespace {
using namespace ndnsf::di;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_SUITE(Spec212HandoffFlow)

BOOST_AUTO_TEST_CASE(DuplicateJoinsOwnerResolutionAndConflictingBindingIsRejected)
{
  ndn::security::KeyChain keyChain("pib-memory:spec212-handoff-flow",
                                   "tpm-memory:spec212-handoff-flow");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName(ndnsf::di::PROVIDER_NAME);
  const ndn::Name ownerName(ndnsf::di::OWNER_NAME);
  const ndn::Name serviceName(ndnsf::di::SERVICE_NAME);
  auto providerCertificate = makeRsaIdentity(keyChain, providerName);
  auto authorityCertificate = makeRsaIdentity(
    keyChain, ndn::Name("/spec212/authority/handoff-flow"));
  auto ownerCertificate = makeRsaIdentity(keyChain, ownerName);
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec212/group/handoff-flow"), providerCertificate,
    authorityCertificate, "examples/trust-any.conf");
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  auto packetStore = std::make_shared<CountingPacketStore>();

  std::mutex resolverMutex;
  std::condition_variable resolverChanged;
  bool resolverEntered = false;
  unsigned resolverEnteredCount = 0;
  bool releaseResolver = false;
  std::atomic<unsigned> resolverCalls{0};
  auto service = ndnsf::di::makeService();
  service.handoffOwnerContextResolver =
    [&](const ndn::Name& authenticatedOwner, const ndn::Name& requestedService,
        const std::string&) {
      BOOST_CHECK_EQUAL(authenticatedOwner, ownerName);
      BOOST_CHECK_EQUAL(requestedService, serviceName);
      ++resolverCalls;
      std::unique_lock<std::mutex> lock(resolverMutex);
      resolverEntered = true;
      ++resolverEnteredCount;
      resolverChanged.notify_all();
      resolverChanged.wait(lock, [&] { return releaseResolver; });
      return ndnsf::di::makeOwnerContext(ownerCertificate, packetStore);
    };
  auto registration = host->serve(service, ndnsf::di::makeConfig());

  auto control = ndnsf::di::makeControl();
  const auto firstRequest = ndnsf::di::makeRequest(control);
  auto first = std::async(std::launch::async, [&] {
    return provider->invokeAsyncTargetedHandlerForTest(
      serviceName, ownerName, ndn::Name("/spec212/handoff/request/1"),
      firstRequest, [](ResponseMessage) {});
  });
  struct ReleaseOwnerResolver
  {
    std::mutex& mutex;
    std::condition_variable& changed;
    bool& released;
    void release()
    {
      {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
      }
      changed.notify_all();
    }
    ~ReleaseOwnerResolver() { release(); }
  } releaseOwnerResolver{resolverMutex, resolverChanged, releaseResolver};

  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(resolverMutex);
    entered = resolverChanged.wait_for(lock, 2s, [&] { return resolverEntered; });
  }
  if (!entered) {
    releaseOwnerResolver.release();
    BOOST_CHECK_MESSAGE(false, "production handoff did not enter Owner resolution");
    (void)first.get();
    registration.close();
    BOOST_CHECK(registration.drain(2s));
    return;
  }

  auto independentControl = control;
  independentControl.operationId = "spec212-independent-operation-0002";
  independentControl.conversationId = "spec212-independent-conversation-0002";
  auto independent = std::async(std::launch::async, [&] {
    return provider->invokeAsyncTargetedHandlerForTest(
      serviceName, ownerName, ndn::Name("/spec212/handoff/request/independent"),
      ndnsf::di::makeRequest(independentControl), [](ResponseMessage) {});
  });
  bool independentResolverEntered = false;
  {
    std::unique_lock<std::mutex> lock(resolverMutex);
    independentResolverEntered = resolverChanged.wait_for(lock, 2s, [&] {
      return resolverEnteredCount == 2;
    });
  }
  if (!independentResolverEntered) {
    releaseOwnerResolver.release();
    (void)first.get();
    (void)independent.get();
    registration.close();
    BOOST_CHECK(registration.drain(2s));
    BOOST_CHECK_MESSAGE(false,
      "a distinct conversation could not resolve Owner context while the first was parked");
    return;
  }

  bool duplicateCompleted = false;
  bool duplicateStatus = false;
  std::string duplicatePayload;
  BOOST_REQUIRE(provider->invokeAsyncTargetedHandlerForTest(
    serviceName, ownerName, ndn::Name("/spec212/handoff/request/2"),
    ndnsf::di::makeRequest(control), [&](ResponseMessage response) {
      duplicateStatus = response.getStatus();
      const auto payload = response.getPayload();
      duplicatePayload.assign(payload.begin(), payload.end());
      duplicateCompleted = true;
    }));
  BOOST_CHECK(duplicateCompleted);
  BOOST_CHECK(duplicateStatus);
  const auto duplicateReply = nativeConversationHandoffReplyV1FromJson(
    duplicatePayload);
  BOOST_CHECK_EQUAL(duplicateReply.operationId, control.operationId);
  BOOST_CHECK_EQUAL(duplicateReply.conversationId, control.conversationId);

  auto conflictingControl = control;
  conflictingControl.destinationRoleMapDigest = "sha256:" + std::string(64, '9');
  bool conflictCompleted = false;
  bool conflictStatus = true;
  BOOST_REQUIRE(provider->invokeAsyncTargetedHandlerForTest(
    serviceName, ownerName, ndn::Name("/spec212/handoff/request/3"),
    ndnsf::di::makeRequest(conflictingControl), [&](ResponseMessage response) {
      conflictStatus = response.getStatus();
      conflictCompleted = true;
    }));
  BOOST_CHECK(conflictCompleted);
  BOOST_CHECK(!conflictStatus);
  BOOST_CHECK_EQUAL(resolverCalls.load(), 2U);
  BOOST_CHECK_EQUAL(packetStore->commitCalls.load(), 0U);

  releaseOwnerResolver.release();
  BOOST_CHECK(first.get());
  BOOST_CHECK(independent.get());
  registration.close();
  BOOST_CHECK(registration.drain(5s));
  BOOST_CHECK_EQUAL(resolverCalls.load(), 2U);
  BOOST_CHECK_EQUAL(packetStore->commitCalls.load(), 0U);
}

BOOST_AUTO_TEST_CASE(ClosedRegistrationIgnoresLateOwnerResolutionAfterReservingNewGeneration)
{
  ndn::security::KeyChain keyChain("pib-memory:spec212-late-handoff",
                                   "tpm-memory:spec212-late-handoff");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName(ndnsf::di::PROVIDER_NAME);
  const ndn::Name ownerName(ndnsf::di::OWNER_NAME);
  const ndn::Name serviceName(ndnsf::di::SERVICE_NAME);
  auto providerCertificate = makeRsaIdentity(keyChain, providerName);
  auto authorityCertificate = makeRsaIdentity(
    keyChain, ndn::Name("/spec212/authority/late-handoff"));
  auto ownerCertificate = makeRsaIdentity(keyChain, ownerName);
  auto provider = std::make_shared<LocalServiceProvider>(
    face, ndn::Name("/spec212/group/late-handoff"), providerCertificate,
    authorityCertificate, "examples/trust-any.conf");
  auto host = std::make_shared<NativeInferenceProvider>(
    provider, std::make_shared<NativeAdapterRegistry>());
  auto packetStore = std::make_shared<CountingPacketStore>();

  std::mutex resolverMutex;
  std::condition_variable resolverChanged;
  bool oldEntered = false;
  bool releaseOld = false;
  std::atomic<unsigned> oldResolverCalls{0};
  std::atomic<unsigned> newResolverCalls{0};
  auto oldService = ndnsf::di::makeService();
  oldService.handoffOwnerContextResolver =
    [&](const ndn::Name&, const ndn::Name&, const std::string&) {
      ++oldResolverCalls;
      std::unique_lock<std::mutex> lock(resolverMutex);
      oldEntered = true;
      resolverChanged.notify_all();
      resolverChanged.wait(lock, [&] { return releaseOld; });
      return ndnsf::di::makeOwnerContext(ownerCertificate, packetStore);
    };
  auto oldRegistration = host->serve(oldService, ndnsf::di::makeConfig());
  const auto oldGeneration = oldRegistration.generation();
  auto oldControl = ndnsf::di::makeControl();
  std::atomic<unsigned> oldReplyCalls{0};
  std::atomic<unsigned> oldPositiveReplies{0};
  auto oldInvocation = std::async(std::launch::async, [&] {
    return provider->invokeAsyncTargetedHandlerForTest(
      serviceName, ownerName, ndn::Name("/spec212/handoff/late/old"),
      ndnsf::di::makeRequest(oldControl), [&](ResponseMessage reply) {
        ++oldReplyCalls;
        if (reply.getStatus()) ++oldPositiveReplies;
      });
  });
  struct ReleaseOldResolver
  {
    std::mutex& mutex;
    std::condition_variable& changed;
    bool& released;
    ~ReleaseOldResolver()
    {
      {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
      }
      changed.notify_all();
    }
  } releaseOnExit{resolverMutex, resolverChanged, releaseOld};

  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(resolverMutex);
    entered = resolverChanged.wait_for(lock, 2s, [&] { return oldEntered; });
  }
  if (!entered) {
    {
      std::lock_guard<std::mutex> lock(resolverMutex);
      releaseOld = true;
    }
    resolverChanged.notify_all();
    (void)oldInvocation.get();
    oldRegistration.close();
    BOOST_CHECK(oldRegistration.drain(5s));
    BOOST_CHECK_MESSAGE(false, "old handoff did not enter Owner resolution");
    return;
  }

  oldRegistration.close();
  auto newService = ndnsf::di::makeService();
  newService.handoffOwnerContextResolver =
    [&](const ndn::Name&, const ndn::Name&, const std::string&) {
      ++newResolverCalls;
      return ndnsf::di::makeOwnerContext(ownerCertificate, packetStore);
    };
  auto newRegistration = host->serve(newService, ndnsf::di::makeConfig());
  BOOST_CHECK_NE(newRegistration.generation(), oldGeneration);
  {
    std::lock_guard<std::mutex> lock(resolverMutex);
    releaseOld = true;
  }
  resolverChanged.notify_all();
  (void)oldInvocation.get();
  BOOST_CHECK(oldRegistration.drain(5s));

  auto newControl = ndnsf::di::makeControl();
  newControl.operationId = "spec212-late-generation-operation-0002";
  newControl.conversationId = "spec212-late-generation-conversation-0002";
  BOOST_CHECK(provider->invokeAsyncTargetedHandlerForTest(
    serviceName, ownerName, ndn::Name("/spec212/handoff/late/new"),
    ndnsf::di::makeRequest(newControl), [](ResponseMessage) {}));
  newRegistration.close();
  BOOST_CHECK(newRegistration.drain(5s));
  BOOST_CHECK_EQUAL(oldResolverCalls.load(), 1U);
  BOOST_CHECK_EQUAL(newResolverCalls.load(), 1U);
  BOOST_CHECK_LE(oldReplyCalls.load(), 1U);
  BOOST_CHECK_EQUAL(oldPositiveReplies.load(), 0U);
  BOOST_CHECK_EQUAL(packetStore->commitCalls.load(), 0U);
}

BOOST_AUTO_TEST_CASE(OwnerCommitAndCancelRaceHasOneTerminalOutcome)
{
  NativeConversationHandoffBoundaryGateV1 gate(
    {"/spec212/provider/source-a", "/spec212/provider/source-b"},
    std::chrono::steady_clock::now() + 5s);
  BOOST_REQUIRE(gate.reportBoundaryReady("/spec212/provider/source-a"));
  BOOST_REQUIRE(gate.reportBoundaryReady("/spec212/provider/source-b"));

  std::promise<void> start;
  const auto ready = start.get_future().share();
  auto commit = std::async(std::launch::async, [&] {
    ready.wait();
    return gate.releaseCommit();
  });
  auto cancel = std::async(std::launch::async, [&] {
    ready.wait();
    return gate.cancel();
  });
  start.set_value();

  const bool commitWon = commit.get();
  const bool cancelWon = cancel.get();
  BOOST_CHECK_NE(commitWon, cancelWon);
  if (commitWon) {
    BOOST_CHECK(gate.state() == NativeConversationHandoffBoundaryGateV1::State::CommitReleased);
    BOOST_CHECK(gate.releaseCommit());
    BOOST_CHECK(!gate.cancel());
  }
  else {
    BOOST_CHECK(gate.state() == NativeConversationHandoffBoundaryGateV1::State::Cancelled);
    BOOST_CHECK(!gate.releaseCommit());
    BOOST_CHECK(gate.cancel());
  }
}

BOOST_AUTO_TEST_CASE(ProductionAdmissionRejectsConcurrentSwitchAtEventCapacity)
{
  ndnsf::di::tests::Spec204ColdProviderFlow::
    runSpec212ProductionAdmissionRejectsConcurrentSwitchAtEventCapacity();
}

BOOST_AUTO_TEST_CASE(ProductionAdmissionRejectsConcurrentSwitchAtRoleCapacity)
{
  ndnsf::di::tests::Spec204ColdProviderFlow::
    runSpec212ProductionAdmissionRejectsConcurrentSwitchAtRoleCapacity();
}

BOOST_AUTO_TEST_CASE(ControlProgressWhileRoleCallbackIsParked)
{
  ndnsf::di::tests::Spec204ColdProviderFlow::
    runSpec212ControlProgressWhileRoleCallbackIsParked();
}

BOOST_AUTO_TEST_CASE(ControlProgressWhileTwoRolesAreParked)
{
  ndnsf::di::tests::Spec204ColdProviderFlow::
    runSpec212ControlProgressWhileTwoRolesAreParked();
}

BOOST_AUTO_TEST_CASE(ControlProgressAcrossTwoSourcesWhileParked)
{
  ndnsf::di::tests::Spec204ColdProviderFlow::
    runSpec212ControlProgressAcrossTwoSourcesWhileParked();
}

BOOST_AUTO_TEST_CASE(TwoEventCapacityAcrossTwoSources)
{
  ndnsf::di::tests::Spec204ColdProviderFlow::
    runSpec212TwoEventCapacityAcrossTwoSources();
}

BOOST_AUTO_TEST_CASE(TwoRoleCapacityAcrossTwoSources)
{
  ndnsf::di::tests::Spec204ColdProviderFlow::
    runSpec212TwoRoleCapacityAcrossTwoSources();
}

BOOST_AUTO_TEST_CASE(CloseDrainsWhileOwnerPacketSetBoundaryIsActive)
{
  ndnsf::di::tests::Spec204ColdProviderFlow::
    runSpec212CloseDrainsWhileOwnerPacketSetBoundaryIsActive();
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace
} // namespace ndn_service_framework::test
