#include "tests/boost-test.hpp"
#include "ndnsf-integration-fixture.hpp"
#include "ndn-service-framework/detail/exact-packet-index.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/signal/scoped-connection.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace ndn_service_framework::test {
using namespace std::chrono_literals;

/** Test access to the guarded Context path, never the legacy Provider helper. */
struct ExactPublicationOracle
{
  static auto
  publishManifest(ServiceProvider::CollaborationContext& context, const SegmentManifestScope& scope,
                  uint32_t count = 2, ndn::span<const uint8_t> app = {}, uint64_t diskLimit = 0)
  {
    std::vector<ndn::ConstBufferPtr> contents;
    contents.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      contents.push_back(std::make_shared<const ndn::Buffer>(ndn::Buffer{
        static_cast<uint8_t>(2 * i + 1), static_cast<uint8_t>(2 * i + 2)}));
    }
    return context.publishManifestAuthenticatedExactData("manifest-authority", scope,
      {4096, 65536, count}, contents, app, 60000, nullptr, diskLimit);
  }

  static ServiceProvider::ExactDataFetchOperation
  readManifest(ServiceProvider::CollaborationContext& context, const SegmentManifestScope& scope,
               uint32_t count, std::function<bool()> onValidatedData,
               std::chrono::milliseconds timeout = 2s,
               std::function<void(std::exception_ptr)> onFirstRead = {})
  {
    return context.startVerifiedManifestAuthenticatedExactData("manifest-authority", scope,
      {4096, 65536, count}, std::chrono::steady_clock::now() + timeout, {},
      std::move(onFirstRead), std::move(onValidatedData), 16);
  }

  static bool
  replayManifest(ServiceProvider::CollaborationContext& context,
                 const ServiceProvider::CollaborationContext::ExactManifestPublication& original)
  {
    return context.replayManifestAuthenticatedExactData("manifest-authority", original, nullptr);
  }

  static bool
  publish(ServiceProvider::CollaborationContext& context, const ndn::Name& name,
          const ndn::Buffer& payload, CollaborationTransferMetrics* metrics,
          std::vector<std::shared_ptr<const ndn::Data>>& originals, int freshnessMs = 60000,
          std::uint64_t diskLimit = 0)
  {
    return context.publishOwnedExactData(
      "exact-authority", {{name, payload}}, freshnessMs, metrics, originals, 65536, diskLimit);
  }

  static bool
  publishBatch(ServiceProvider::CollaborationContext& context,
               const std::vector<std::pair<ndn::Name, ndn::Buffer>>& objects,
               std::vector<std::shared_ptr<const ndn::Data>>& originals)
  {
    return context.publishOwnedExactData(
      "exact-authority", objects, 60000, nullptr, originals, 65536, 65536);
  }

  static bool
  replay(ServiceProvider::CollaborationContext& context,
         const std::vector<std::shared_ptr<const ndn::Data>>& originals,
         CollaborationTransferMetrics* metrics)
  {
    return context.replayOwnedExactData("exact-authority", originals, metrics);
  }

  static std::shared_ptr<void>
  holdReadLease(ServiceProvider::CollaborationContext& context)
  {
    std::lock_guard lock(*context.m_workAuthorityCommitMutex);
    return context.m_retainedExactPacketSet.lock();
  }
};

BOOST_AUTO_TEST_CASE(ManifestOriginalsAnswerOrdinaryAndPendingDigestInterests)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  auto& provider = environment.provider(0);
  auto& face = environment.providerFace(0);
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "manifest-authority";
  assignment.service = profile.serviceName;
  assignment.workDeadline = std::chrono::steady_clock::now() + 90s;
  assignment.workCurrent = [] { return true; };
  const ndn::Name requestId("/fixture/manifest-publication");
  ServiceProvider::CollaborationContext context(
    provider, profile.userIdentity, requestId, RequestMessage(), assignment);
  const SegmentManifestScope scope{provider.getName(),
    ndn::Name(provider.getName()).append("NDNSF").append("TP").appendVersion(1)};

  const auto handle = ExactPublicationOracle::publishManifest(context, scope);
  BOOST_REQUIRE(handle);
  std::vector<ndn::Data> received;
  ndn::signal::ScopedConnection capture = face.onSendData.connect([&] (const ndn::Data& data) {
    if (data.getName() == scope.sourceName(0)) {
      received.push_back(data);
    }
  });
  const auto request = [&] (const ndn::Name& name) {
    ndn::Interest interest(name);
    interest.setCanBePrefix(false).setInterestLifetime(ndn::time::milliseconds(10000));
    face.receive(interest);
  };
  request(scope.sourceName(0));
  environment.pumpUntil([&] { return !received.empty(); });
  BOOST_REQUIRE_EQUAL(received.size(), 1U);
  const auto original = received.front();
  BOOST_REQUIRE_EQUAL(provider.evictImsPrefixForTest(scope.objectPrefix), 3U);

  request(original.getFullName());
  const auto pendingUntil = std::chrono::steady_clock::now() + 30ms;
  environment.pumpUntil([&] { return std::chrono::steady_clock::now() >= pendingUntil; });
  BOOST_CHECK_EQUAL(received.size(), 1U);
  ServiceProvider::CollaborationContext wrongRequest(
    provider, profile.userIdentity, ndn::Name(requestId).append("other"),
    RequestMessage(), assignment);
  BOOST_CHECK(!ExactPublicationOracle::replayManifest(wrongRequest, *handle));
  BOOST_REQUIRE(ExactPublicationOracle::replayManifest(context, *handle));
  environment.pumpUntil([&] { return received.size() == 2; });
  BOOST_REQUIRE_EQUAL(received.size(), 2U);
  BOOST_CHECK(received.back().wireEncode() == original.wireEncode());

  BOOST_REQUIRE(context.tryReleaseExactPacketRetention());
  BOOST_CHECK(!ExactPublicationOracle::publishManifest(context, scope));
  BOOST_CHECK(!ExactPublicationOracle::replayManifest(context, *handle));
}

BOOST_AUTO_TEST_CASE(ManifestReadAdmitsRootBeforeSchedulingProvenMembers)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  // Route the peers explicitly so a held packet cannot arrive through the built-in bridge.
  environment.disconnectProviderPeerTransportForTest();
  auto& provider = environment.provider(0);
  auto& receiver = environment.provider(1);
  auto& tx = environment.providerFace(0);
  auto& rx = environment.providerFace(1);
  std::atomic<bool> current{true};
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "manifest-authority";
  assignment.service = profile.serviceName;
  assignment.workDeadline = std::chrono::steady_clock::now() + 90s;
  assignment.workCurrent = [&] { return current.load(); };
  const ndn::Name requestId("/fixture/manifest-read");
  ServiceProvider::CollaborationContext producer(
    provider, profile.userIdentity, requestId, RequestMessage(), assignment);
  ServiceProvider::CollaborationContext consumer(
    receiver, profile.userIdentity, requestId, RequestMessage(), assignment);
  const SegmentManifestScope scope{provider.getName(),
    ndn::Name(provider.getName()).append("NDNSF").append("TP").appendVersion(2)};
  const ndn::Buffer metadata{0, 1, 2, 3};
  constexpr uint32_t sourceCount = 129;
  BOOST_CHECK_EXCEPTION(ExactPublicationOracle::publishManifest(producer, scope, sourceCount,
                        metadata, 1), std::runtime_error, [] (const std::runtime_error& error) {
    return std::string_view(error.what()) == "ORIGINAL_PACKET_STORAGE_BUDGET_EXCEEDED";
  });
  const auto original = ExactPublicationOracle::publishManifest(producer, scope, sourceCount,
                                                                 metadata, 65536);
  BOOST_REQUIRE(original);
  BOOST_CHECK_EQUAL(provider.evictImsPrefixForTest(scope.objectPrefix), 0U);
  BOOST_CHECK(ExactPublicationOracle::replayManifest(producer, *original));

  bool holdFirstPage = true;
  bool holdRoot = false;
  std::optional<ndn::Data> heldPage;
  std::vector<ndn::Name> issued;
  ndn::signal::ScopedConnection interests = rx.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      if (scope.objectPrefix.isPrefixOf(interest.getName())) {
        issued.push_back(interest.getName());
      }
      tx.receive(interest);
    });
  ndn::signal::ScopedConnection data = tx.onSendData.connect(
    [&] (const ndn::Data& packet) {
      if (packet.getName() == scope.rootName() && holdRoot) {
        return;
      }
      if (packet.getName() == scope.pageName(0) && holdFirstPage) {
        heldPage = packet;
      }
      else {
        rx.receive(packet);
      }
    });

  auto awaitRoot = [&] (ServiceProvider::ExactDataFetchOperation& read) {
    auto future = std::async(std::launch::async, [&] { return read.getVerifiedManifest(); });
    environment.pumpUntil([&] { return future.wait_for(0ms) == std::future_status::ready; });
    return future.get();
  };
  std::size_t progress = 0;
  std::size_t firstRead = 0;
  auto operation = ExactPublicationOracle::readManifest(consumer, scope, sourceCount,
    [&] { ++progress; return true; }, 2s, [&] (const std::exception_ptr& error) {
      BOOST_CHECK(!error);
      ++firstRead;
    });
  BOOST_CHECK(!operation.continueAfterManifest());
  const auto root = awaitRoot(operation);
  BOOST_REQUIRE(root);
  BOOST_CHECK(*root == metadata);
  BOOST_REQUIRE_EQUAL(issued.size(), 1U);
  BOOST_CHECK(issued.front() == scope.rootName());
  BOOST_CHECK_EQUAL(firstRead, 1U);
  BOOST_CHECK_EQUAL(progress, 0U);
  BOOST_REQUIRE(operation.continueAfterManifest());
  BOOST_CHECK(!operation.continueAfterManifest());
  environment.pumpUntil([&] { return heldPage.has_value() && progress == 1; });
  BOOST_REQUIRE(heldPage);
  BOOST_REQUIRE_EQUAL(progress, 1U);
  BOOST_CHECK(operation.wait_for(0ms) != std::future_status::ready);
  BOOST_CHECK(std::any_of(issued.cbegin(), issued.cend(), [&] (const ndn::Name& name) {
    return name.size() == scope.sourceName(sourceCount - 1).size() + 1 &&
      name.at(-1).isImplicitSha256Digest() && name.getPrefix(-1) == scope.sourceName(sourceCount - 1);
  }));
  holdFirstPage = false;
  rx.receive(*heldPage);
  environment.pumpUntil([&] { return operation.wait_for(0ms) == std::future_status::ready; });
  const auto result = operation.get();
  BOOST_REQUIRE(result);
  BOOST_REQUIRE_EQUAL(result->contents.size(), sourceCount);
  for (uint32_t i = 0; i < sourceCount; ++i) {
    const ndn::Buffer expected{static_cast<uint8_t>(2 * i + 1), static_cast<uint8_t>(2 * i + 2)};
    BOOST_CHECK(result->contents[i] == expected);
  }
  BOOST_CHECK_EQUAL(result->metrics.manifestCertificateValidations, 1U);
  BOOST_CHECK_EQUAL(result->metrics.manifestVerifiedMembers, sourceCount + 2);
  BOOST_CHECK_EQUAL(result->metrics.rootControlInterests, 3U);

  auto wrongBinding = scope;
  wrongBinding.bindingDigest[0] = 1;
  issued.clear();
  auto mismatched = ExactPublicationOracle::readManifest(consumer, wrongBinding, sourceCount, {});
  BOOST_CHECK(!awaitRoot(mismatched));
  BOOST_CHECK(!mismatched.get());
  BOOST_REQUIRE_EQUAL(issued.size(), 1U);

  issued.clear();
  holdRoot = true;
  auto expired = ExactPublicationOracle::readManifest(consumer, scope, sourceCount, {}, 100ms);
  BOOST_CHECK(!awaitRoot(expired));
  BOOST_CHECK(!expired.get());
  BOOST_REQUIRE(!issued.empty());
  BOOST_CHECK(std::all_of(issued.cbegin(), issued.cend(),
    [&] (const ndn::Name& name) { return name == scope.rootName(); }));
  holdRoot = false;

  issued.clear();
  auto revoked = ExactPublicationOracle::readManifest(consumer, scope, sourceCount, {});
  BOOST_REQUIRE(awaitRoot(revoked));
  current.store(false);
  BOOST_CHECK(!revoked.continueAfterManifest());
  environment.pumpUntil([&] { return revoked.wait_for(0ms) == std::future_status::ready; });
  BOOST_CHECK(!revoked.get());
  BOOST_REQUIRE_EQUAL(issued.size(), 1U);
}

namespace {

void
runSpec231ExactPublicationAuthorityCase(bool loseAtCommit)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  ndn::signal::ScopedConnection interests = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { producerFace.receive(interest); });
  ndn::signal::ScopedConnection data = producerFace.onSendData.connect(
    [&] (const ndn::Data& packet) { consumerFace.receive(packet); });
  ServiceProvider::CollaborationAssignment live;
  live.role = "exact-authority";
  live.service = profile.serviceName;
  live.workDeadline = std::chrono::steady_clock::now() + 90s;
  live.workCurrent = [] { return true; };
  const ndn::Name requestId("/spec231/exact-authority");
  ServiceProvider::CollaborationContext legal(
    producer, profile.userIdentity, requestId, RequestMessage(), live);
  ServiceProvider::CollaborationContext receiver(
    consumer, profile.userIdentity, requestId, RequestMessage(), live);
  auto fetch = [&] (const ndn::Name& name) {
    return std::async(std::launch::async, [&, name] {
      return receiver.fetchSignedExactData("exact-authority", name, producer.getName(), 150);
    });
  };
  const ndn::Buffer payload{'s', 'p', 'e', 'c', '2', '3', '1'};
  for (size_t fault = 0; fault < (loseAtCommit ? 1U : 3U); ++fault) {
    for (size_t api = 0; api < 3; ++api) {
      auto prefix = ndn::Name(producer.getName()).append("SPEC231")
        .appendNumber(fault).appendNumber(api);
      const auto seedName = ndn::Name(prefix).append("original");
      std::vector<std::shared_ptr<const ndn::Data>> originals;
      BOOST_REQUIRE(ExactPublicationOracle::publish(legal, seedName, payload, nullptr, originals));
      BOOST_REQUIRE_EQUAL(originals.size(), 1U);
      const auto seed = originals.front();
      const auto wire = seed->wireEncode();
      // An existing cache hit must not disguise a rejected replay.
      BOOST_REQUIRE_EQUAL(producer.evictImsPrefixForTest(seedName), 1U);
      const auto target = api == 2 ? seedName : ndn::Name(prefix).append("rejected");
      auto assignment = live;
      std::atomic<size_t> checks{0};
      if (loseAtCommit) assignment.workCurrent = [&] { return checks.fetch_add(1) == 0; };
      else if (fault == 0) assignment.workCurrent = {};
      else if (fault == 1) assignment.workDeadline = std::chrono::steady_clock::now() - 1ms;
      else assignment.workCurrent = [] { return false; };
      ServiceProvider::CollaborationContext rejected(
        producer, profile.userIdentity, requestId, RequestMessage(), assignment);
      CollaborationTransferMetrics metrics;
      metrics.actualDataName = "sentinel";
      metrics.transportPayloadBytes = 101;
      metrics.metadataBytes = 102;
      metrics.wireBytes = 103;
      metrics.interestCount = 104;
      metrics.retryCount = 105;
      metrics.localCopyBytes = 106;
      const auto before = std::make_tuple(metrics.actualDataName, metrics.transportPayloadBytes,
        metrics.metadataBytes, metrics.wireBytes, metrics.interestCount,
        metrics.retryCount, metrics.localCopyBytes);
      bool interestIssued = false;
      size_t replies = 0;
      std::optional<ndn::Block> observedWire;
      ndn::signal::ScopedConnection observeInterest =
        consumerFace.onSendInterest.connect([&] (const ndn::Interest& interest) {
          if (interest.getName() == target) interestIssued = true;
        });
      ndn::signal::ScopedConnection observeReply =
        producerFace.onSendData.connect([&] (const ndn::Data& packet) {
          if (packet.getName() == target) {
            ++replies;
            observedWire = packet.wireEncode();
          }
        });
      auto pending = fetch(target);
      environment.pumpUntil([&] { return interestIssued; });
      BOOST_REQUIRE(interestIssued);
      bool accepted = false;
      if (api == 0)
        accepted = rejected.publishSignedExactData(
          "exact-authority", {{target, payload}}, 60000, &metrics);
      else if (api == 1)
        accepted = ExactPublicationOracle::publish(rejected, target, payload, &metrics, originals);
      else
        accepted = ExactPublicationOracle::replay(rejected, originals, &metrics);
      BOOST_CHECK(!accepted);
      if (loseAtCommit) BOOST_CHECK_GE(checks.load(), 2U);
      BOOST_CHECK(std::make_tuple(metrics.actualDataName, metrics.transportPayloadBytes,
        metrics.metadataBytes, metrics.wireBytes, metrics.interestCount,
        metrics.retryCount, metrics.localCopyBytes) == before);
      BOOST_CHECK(!metrics.localWorkObserver.owner);
      BOOST_CHECK(metrics.localWorkObserver.observe == nullptr);
      BOOST_CHECK(metrics.localWorkObserver.addCount == nullptr);
      BOOST_REQUIRE_EQUAL(originals.size(), 1U);
      BOOST_CHECK(originals.front() == seed);
      BOOST_CHECK(originals.front()->wireEncode() == wire);
      environment.pumpUntil([&] { return pending.wait_for(0ms) == std::future_status::ready; });
      BOOST_REQUIRE(pending.wait_for(0ms) == std::future_status::ready);
      BOOST_CHECK(!pending.get());
      BOOST_CHECK_EQUAL(replies, 0U);
      BOOST_CHECK_EQUAL(producer.evictImsPrefixForTest(target), 0U);
      if (api == 2) {
        BOOST_REQUIRE(ExactPublicationOracle::replay(legal, originals, nullptr));
        BOOST_CHECK(originals.front()->wireEncode() == wire);
      }
      else {
        BOOST_REQUIRE(legal.publishSignedExactData("exact-authority", {{target, payload}}, 60000));
      }
      auto recovered = fetch(target);
      environment.pumpUntil([&] { return recovered.wait_for(0ms) == std::future_status::ready; });
      BOOST_REQUIRE(recovered.wait_for(0ms) == std::future_status::ready);
      const auto content = recovered.get();
      BOOST_REQUIRE(content);
      BOOST_CHECK_EQUAL_COLLECTIONS(content->begin(), content->end(), payload.begin(), payload.end());
      if (api == 2) {
        BOOST_REQUIRE(observedWire);
        BOOST_CHECK(*observedWire == wire);
      }
    }
  }
}

enum class Spec231ReadFault
{
  NORMAL,
  MISSING,
  EXPIRED,
  REVOKED,
  LOSE_WORK,
  EXTERNAL_CANCEL
};

using Spec231ReadResult = std::optional<std::vector<ndn::Buffer>>;

Spec231ReadResult
readSpec231ExactApi(ServiceProvider::CollaborationContext& context,
                    unsigned api, const ndn::Name& name, const ndn::Name& producer,
                    const std::function<bool()>& external,
                    const std::function<bool()>& progress)
{
  using Phase = ServiceProvider::ExactDataFetchPhase;
  if (api == 0) {
    auto value = context.fetchSignedExactData("read-authority", name, producer, 1500,
      external, nullptr, {}, Phase::Published);
    if (!value) { return std::nullopt; }
    return std::vector<ndn::Buffer>{std::move(*value)};
  }
  if (api == 1) {
    return context.fetchSignedExactDataBatch("read-authority", {name}, producer,
      1500, external, nullptr, {}, Phase::Published);
  }
  if (api == 2) {
    return context.fetchSignedExactDataBatch("read-authority", {name}, producer,
      1500, external, nullptr, {}, Phase::Published, progress);
  }
  auto operation = api == 3
    ? context.startSignedExactDataBatch("read-authority", {name}, producer,
        1500, external, {}, Phase::Published, progress)
    : context.startSignedExactDataBatch("read-authority", {name}, producer,
        1500, external, {}, Phase::Published, progress, 1);
  auto result = operation.get();
  if (!result) { return std::nullopt; }
  return std::move(result->contents);
}

/** Reap the one fixture worker before Context, Faces or callback inputs die. */
class Spec231ExactReadTask
{
public:
  Spec231ExactReadTask(NdnsfIntegrationEnvironment& environment,
                      std::shared_ptr<std::atomic<bool>> current,
                      std::function<Spec231ReadResult()> work)
    : m_environment(environment)
    , m_current(std::move(current))
    , m_future(std::async(std::launch::async, std::move(work)))
  {
  }

  ~Spec231ExactReadTask()
  {
    m_current->store(false);
    if (!m_future.valid()) { return; }
    m_environment.pumpUntil([this] { return ready(); });
    BOOST_CHECK_MESSAGE(ready(), "exact-read fixture worker failed to drain");
    if (ready()) {
      try { (void)m_future.get(); }
      catch (const std::exception& error) { BOOST_ERROR(error.what()); }
    }
  }

  bool ready() const
  {
    return m_future.wait_for(0ms) == std::future_status::ready;
  }

  Spec231ReadResult get() { return m_future.get(); }

private:
  NdnsfIntegrationEnvironment& m_environment;
  std::shared_ptr<std::atomic<bool>> m_current;
  std::future<Spec231ReadResult> m_future;
};

void
runSpec231ExactReadAuthorityCases(const std::vector<Spec231ReadFault>& faults)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  ndn::signal::ScopedConnection data = producerFace.onSendData.connect(
    [&] (const ndn::Data& packet) { consumerFace.receive(packet); });
  unsigned ordinal = 0;
  for (unsigned api = 0; api < 5; ++api) {
    for (const auto fault : faults) {
      auto current = std::make_shared<std::atomic<bool>>(true);
      auto external = std::make_shared<std::atomic<bool>>(false);
      auto progress = std::make_shared<std::atomic<size_t>>(0);
      ndn::Name name(producer.getName());
      name.append("SPEC231-READ").appendNumber(ordinal++);
      size_t interestsSeen = 0;
      ndn::signal::ScopedConnection interests = consumerFace.onSendInterest.connect(
        [&, name, current, external, fault] (const ndn::Interest& interest) {
          if (interest.getName() == name) {
            ++interestsSeen;
            if (fault == Spec231ReadFault::LOSE_WORK) { current->store(false); }
            if (fault == Spec231ReadFault::EXTERNAL_CANCEL) { external->store(true); }
          }
          producerFace.receive(interest);
        });
      ServiceProvider::CollaborationAssignment assignment;
      assignment.role = "read-authority";
      assignment.service = profile.serviceName;
      assignment.workDeadline = std::chrono::steady_clock::now() + 90s;
      assignment.workCurrent = [] { return true; };
      const ndn::Name requestId("/spec231/exact-read");
      ServiceProvider::CollaborationContext publisher(
        producer, profile.userIdentity, requestId, RequestMessage(), assignment);
      const ndn::Buffer payload{'c', 'u', 'r', 'r', 'e', 'n', 't'};
      BOOST_REQUIRE(publisher.publishSignedExactData("read-authority", {{name, payload}}, 60000));
      assignment.workCurrent = [current] { return current->load(); };
      if (fault == Spec231ReadFault::MISSING) {
        assignment.workCurrent = {};
        assignment.workDeadline = std::chrono::steady_clock::time_point::min();
      }
      if (fault == Spec231ReadFault::EXPIRED) {
        assignment.workDeadline = std::chrono::steady_clock::now() - 1s;
      }
      if (fault == Spec231ReadFault::REVOKED) { current->store(false); }
      ServiceProvider::CollaborationContext receiver(
        consumer, profile.userIdentity, requestId, RequestMessage(), assignment);
      Spec231ExactReadTask task(environment, current, [&] {
        return readSpec231ExactApi(receiver, api, name, producer.getName(),
          [external] { return external->load(); }, [progress] {
            ++*progress;
            return true;
          });
      });
      environment.pumpUntil([&] { return task.ready(); });
      BOOST_REQUIRE(task.ready());
      const auto result = task.get();
      if (fault == Spec231ReadFault::NORMAL) {
        BOOST_REQUIRE(result);
        BOOST_REQUIRE_EQUAL(result->size(), 1U);
        BOOST_CHECK_EQUAL_COLLECTIONS(result->front().begin(), result->front().end(),
                                      payload.begin(), payload.end());
        BOOST_CHECK_EQUAL(progress->load(), api < 2 ? 0U : 1U);
      }
      else {
        BOOST_CHECK(!result);
        BOOST_CHECK_EQUAL(progress->load(), 0U);
        if (fault == Spec231ReadFault::MISSING || fault == Spec231ReadFault::EXPIRED ||
            fault == Spec231ReadFault::REVOKED) {
          BOOST_CHECK_EQUAL(interestsSeen, 0U);
        }
        else {
          BOOST_CHECK_GT(interestsSeen, 0U);
        }
      }
    }
  }
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec181ExactDataWire)

BOOST_AUTO_TEST_CASE(ExactIndexCoalescesOnlyConsecutiveTypedNamesAndOrdinals)
{
  constexpr std::uint64_t budget = 512;
  constexpr std::uint64_t segments = 8400;
  const ndn::Name prefix("/producer/object/DATA");
  detail::ExactPacketIndex batch;
  for (std::uint64_t segment = 1; segment <= segments; ++segment)
    batch.add(ndn::Name(prefix).appendSegment(segment), segment - 1, budget);
  BOOST_CHECK_LT(batch.bytes(), budget);
  BOOST_TEST_MESSAGE("Exact index charged " << batch.bytes() << " B for " << segments << " names");
  BOOST_CHECK_THROW(batch.add(ndn::Name(prefix).appendSegment(2), segments, budget),
                    std::invalid_argument);
  const std::array<std::uint8_t, 2> paddedSegment{0, 2};
  const auto alias = ndn::Name(prefix).append(ndn::tlv::SegmentNameComponent, paddedSegment);
  batch.add(alias, segments, budget);
  BOOST_CHECK_THROW(batch.add(ndn::Name(prefix).append("too-much"), segments + 1, budget),
                    std::length_error);
  detail::ExactPacketIndex index;
  index.commit(std::move(batch), 31);
  BOOST_CHECK(index.find(ndn::Name(prefix).appendSegment(1)) == 31);
  BOOST_CHECK(index.find(ndn::Name(prefix).appendSegment(256)) == 286);
  BOOST_CHECK(index.find(ndn::Name(prefix).appendSegment(segments)) == segments + 30);
  BOOST_CHECK(index.find(alias) == segments + 31);
  BOOST_CHECK(!index.find(ndn::Name(prefix).appendSegment(0)));
  BOOST_CHECK(!index.find(ndn::Name(prefix).appendSegment(segments + 1)));
  BOOST_CHECK(!index.find(ndn::Name(prefix).append("foreign").appendSegment(2)));
  detail::ExactPacketIndex next;
  next.add(ndn::Name(prefix).appendSegment(segments + 5), 0, budget);
  next.add(ndn::Name(prefix).appendSegment(segments + 4), 1, budget);
  index.commit(std::move(next), segments + 32);
  BOOST_CHECK(index.find(ndn::Name(prefix).appendSegment(segments + 5)) == segments + 32);
  BOOST_CHECK(index.find(ndn::Name(prefix).appendSegment(segments + 4)) == segments + 33);
  BOOST_CHECK(!index.find(ndn::Name(prefix).appendSegment(segments + 3)));
}

BOOST_AUTO_TEST_CASE(FileBackedExactDataSurvivesLocalCompletionAndHonorsRevocation)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  ndn::signal::ScopedConnection interests = consumerFace.onSendInterest.connect(
    [&](const ndn::Interest& interest) { producerFace.receive(interest); });
  std::optional<ndn::Block> lastWire;
  ndn::signal::ScopedConnection data = producerFace.onSendData.connect(
    [&](const ndn::Data& packet) {
      lastWire = packet.wireEncode();
      consumerFace.receive(packet);
    });
  ServiceProvider::CollaborationAssignment live;
  live.role = "exact-authority";
  live.service = profile.serviceName;
  live.workDeadline = std::chrono::steady_clock::now() + 30s;
  live.workCurrent = [] { return true; };
  const ndn::Name requestId("/fixture/exact-file");
  ServiceProvider::CollaborationContext receiver(
    consumer, profile.userIdentity, requestId, RequestMessage(), live);
  const auto fetch = [&](const ndn::Name& name) {
    auto pending = std::async(std::launch::async, [&, name] {
      return receiver.fetchSignedExactData("exact-authority", name, producer.getName(), 200);
    });
    environment.pumpUntil([&] { return pending.wait_for(0ms) == std::future_status::ready; });
    BOOST_REQUIRE(pending.wait_for(0ms) == std::future_status::ready);
    return pending.get();
  };
  const auto name = ndn::Name(producer.getName()).append("exact-file").append("manifest");
  const ndn::Buffer payload{'o', 'r', 'i', 'g', 'i', 'n', 'a', 'l'};
  ndn::Block expected;
  {
    std::atomic<bool> working{true};
    auto local = live;
    local.workCurrent = [&] { return working.load(); };
    ServiceProvider::CollaborationContext sender(
      producer, profile.userIdentity, requestId, RequestMessage(), local);
    std::vector<std::shared_ptr<const ndn::Data>> originals;
    BOOST_CHECK_THROW(ExactPublicationOracle::publish(
      sender, name, payload, nullptr, originals, 60000, 1), std::runtime_error);
    BOOST_CHECK(originals.empty());
    BOOST_REQUIRE(ExactPublicationOracle::publish(
      sender, name, payload, nullptr, originals, 60000, 4096));
    BOOST_REQUIRE_EQUAL(originals.size(), 1);
    expected = originals.front()->wireEncode();
    working = false;
  }
  BOOST_CHECK_EQUAL(producer.evictImsPrefixForTest(name), 0);
  ServiceProvider::CollaborationContext conflicting(
    producer, profile.userIdentity, requestId, RequestMessage(), live);
  BOOST_CHECK_THROW(conflicting.publishSignedExactData(
    "exact-authority", {{name, ndn::Buffer{'x'}}}, 60000), std::invalid_argument);
  const auto restored = fetch(name);
  BOOST_REQUIRE(restored);
  BOOST_CHECK(*restored == payload);
  BOOST_REQUIRE(lastWire);
  BOOST_CHECK(*lastWire == expected);

  const auto dataPrefix = ndn::Name(name.getPrefix(-1)).append("DATA");
  const auto segmentedManifest = ndn::Name(name.getPrefix(-1)).append("segmented-manifest");
  const std::vector<std::pair<ndn::Name, ndn::Buffer>> batch{
    {segmentedManifest, {'m'}},
    {ndn::Name(dataPrefix).appendSegment(1), {'a'}},
    {ndn::Name(dataPrefix).appendSegment(2), {'b'}},
    {ndn::Name(dataPrefix).appendSegment(3), {'c'}}};
  auto xorAssignment = live;
  xorAssignment.dependencyTransport.redundancy = DependencyRedundancy::Xor;
  ServiceProvider::CollaborationContext segmented(
    producer, profile.userIdentity, requestId, RequestMessage(), xorAssignment);
  std::vector<std::shared_ptr<const ndn::Data>> segmentedOriginals;
  BOOST_REQUIRE(ExactPublicationOracle::publishBatch(segmented, batch, segmentedOriginals));
  BOOST_REQUIRE_GT(segmentedOriginals.size(), batch.size());
  const auto repairPrefix = ndn::Name(segmentedManifest).append("FEC");
  for (std::size_t i = 0; i < segmentedOriginals.size(); ++i) {
    const auto& packet = *segmentedOriginals[i];
    const auto& packetName = packet.getName();
    const auto& content = packet.getContent();
    if (i < batch.size()) {
      BOOST_CHECK_EQUAL(packetName, batch[i].first);
    }
    else {
      BOOST_CHECK(repairPrefix.isPrefixOf(packetName));
    }
    BOOST_CHECK_EQUAL(producer.evictImsPrefixForTest(packetName), 0U);
    lastWire.reset();
    const auto fetched = fetch(packetName);
    BOOST_REQUIRE(fetched);
    BOOST_CHECK_EQUAL_COLLECTIONS(fetched->begin(), fetched->end(),
                                  content.value_begin(), content.value_end());
    BOOST_REQUIRE(lastWire);
    BOOST_CHECK(*lastWire == segmentedOriginals[i]->wireEncode());
  }
  BOOST_CHECK(!fetch(ndn::Name(dataPrefix).appendSegment(0)));
  BOOST_CHECK(!fetch(ndn::Name(dataPrefix).appendSegment(4)));
  BOOST_CHECK_THROW(conflicting.publishSignedExactData(
    "exact-authority", {{batch[1].first, ndn::Buffer{'x'}}}, 60000), std::invalid_argument);
  BOOST_REQUIRE(segmented.revokeCurrentWorkAuthority());
  BOOST_CHECK(!fetch(batch[1].first));

  const auto cancelled = ndn::Name(name).append("cancelled");
  ServiceProvider::CollaborationContext sender(
    producer, profile.userIdentity, requestId, RequestMessage(), live);
  std::vector<std::shared_ptr<const ndn::Data>> originals;
  BOOST_REQUIRE(ExactPublicationOracle::publish(
    sender, cancelled, payload, nullptr, originals, 60000, 4096));
  BOOST_REQUIRE(sender.revokeCurrentWorkAuthority());
  BOOST_CHECK(!fetch(cancelled));
  BOOST_CHECK_THROW(sender.tryReleaseExactPacketRetention(), std::runtime_error);

  // Each logical reservation exceeds half the Provider budget. The fixture
  // writes a single packet to each file and holds the same ownership as an
  // outstanding reader while verifying the quota transitions.
  constexpr std::uint64_t DISK_RESERVATION_BYTES = 65'537ULL << 20;
  for (const bool revoke : {false, true}) {
    const auto terminal = revoke ? "revoked" : "settled";
    const auto firstName = ndn::Name(name).append(terminal).append("first");
    const auto nextName = ndn::Name(name).append(terminal).append("next");
    ServiceProvider::CollaborationContext first(producer, profile.userIdentity,
      ndn::Name(requestId).append(terminal).append("first"), RequestMessage(), live);
    ServiceProvider::CollaborationContext next(producer, profile.userIdentity,
      ndn::Name(requestId).append(terminal).append("next"), RequestMessage(), live);
    std::vector<std::shared_ptr<const ndn::Data>> firstOriginals, nextOriginals;
    BOOST_REQUIRE(ExactPublicationOracle::publish(
      first, firstName, payload, nullptr, firstOriginals, 60000, DISK_RESERVATION_BYTES));
    BOOST_REQUIRE(fetch(firstName));
    auto reader = ExactPublicationOracle::holdReadLease(first);
    BOOST_REQUIRE(reader);
    if (revoke) {
      BOOST_REQUIRE(first.revokeCurrentWorkAuthority());
      BOOST_CHECK(!first.hasCurrentWorkAuthority());
      BOOST_CHECK_THROW(first.tryReleaseExactPacketRetention(), std::runtime_error);
    }
    else {
      BOOST_CHECK(!first.tryReleaseExactPacketRetention());
      BOOST_CHECK(first.hasCurrentWorkAuthority());
      BOOST_CHECK(!ExactPublicationOracle::replay(first, firstOriginals, nullptr));
      BOOST_CHECK(!first.publishSignedExactData("exact-authority", {{firstName, payload}}, 60000));
      BOOST_CHECK(!ExactPublicationOracle::publish(
        first, firstName, payload, nullptr, firstOriginals, 60000, DISK_RESERVATION_BYTES));
    }
    BOOST_CHECK(!fetch(firstName));
    BOOST_CHECK_EXCEPTION(ExactPublicationOracle::publish(
      next, nextName, payload, nullptr, nextOriginals, 60000, DISK_RESERVATION_BYTES),
      std::length_error,
      [](const auto& error) {
        return std::string_view(error.what()) == "exact packet retention Provider capacity";
      });
    BOOST_CHECK(nextOriginals.empty());
    reader.reset();
    if (!revoke) {
      BOOST_REQUIRE(first.tryReleaseExactPacketRetention());
      BOOST_CHECK(first.tryReleaseExactPacketRetention());
    }
    BOOST_REQUIRE(ExactPublicationOracle::publish(
      next, nextName, payload, nullptr, nextOriginals, 60000, DISK_RESERVATION_BYTES));
    BOOST_REQUIRE(fetch(nextName));
    BOOST_REQUIRE(next.tryReleaseExactPacketRetention());
  }

  const auto expired = ndn::Name(name).append("expired");
  auto shortWork = live;
  shortWork.workDeadline = std::chrono::steady_clock::now() + 500ms;
  ServiceProvider::CollaborationContext expiring(
    producer, profile.userIdentity, requestId, RequestMessage(), shortWork);
  BOOST_REQUIRE(ExactPublicationOracle::publish(
    expiring, expired, payload, nullptr, originals, 60000, 4096));
  BOOST_REQUIRE(fetch(expired));
  environment.pumpUntil([&] { return std::chrono::steady_clock::now() >= shortWork.workDeadline; });
  BOOST_CHECK(!fetch(expired));
}

BOOST_AUTO_TEST_CASE(RetainedExactOriginalSurvivesItsCacheFreshnessWindow)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  bool requestedFresh = false;
  ndn::Name expectedName;
  ndn::signal::ScopedConnection interests = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      if (interest.getName() == expectedName) {
        requestedFresh = requestedFresh || interest.getMustBeFresh();
      }
      producerFace.receive(interest);
    });
  std::optional<ndn::Block> returnedWire;
  ndn::signal::ScopedConnection data = producerFace.onSendData.connect(
    [&] (const ndn::Data& packet) {
      if (packet.getName() == expectedName) {
        returnedWire = packet.wireEncode();
      }
      consumerFace.receive(packet);
    });
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "retained-exact";
  assignment.service = profile.serviceName;
  assignment.workDeadline = std::chrono::steady_clock::now() + 90s;
  assignment.workCurrent = [] { return true; };
  const ndn::Name requestId("/exact-original/late-peer");
  const ndn::Buffer payload{'o', 'r', 'i', 'g', 'i', 'n', 'a', 'l'};
  for (const bool replay : {false, true}) {
    const auto name = ndn::Name(producer.getName()).append("retained-exact").appendNumber(replay);
    expectedName = name;
    ServiceProvider::CollaborationContext publisher(
      producer, profile.userIdentity, requestId, RequestMessage(), assignment);
    std::vector<std::shared_ptr<const ndn::Data>> originals;
    BOOST_REQUIRE(ExactPublicationOracle::publish(publisher, name, payload, nullptr, originals, 100));
    BOOST_REQUIRE_EQUAL(originals.size(), 1U);
    const auto originalWire = originals.front()->wireEncode();
    BOOST_CHECK(originals.front()->getFreshnessPeriod() == ndn::time::milliseconds(100));
    if (replay) {
      BOOST_REQUIRE(ExactPublicationOracle::replay(publisher, originals, nullptr));
    }
    const auto readOriginal = [&] {
      auto current = std::make_shared<std::atomic<bool>>(true);
      auto readAssignment = assignment;
      readAssignment.workCurrent = [current] { return current->load(); };
      ServiceProvider::CollaborationContext receiver(
        consumer, profile.userIdentity, requestId, RequestMessage(), readAssignment);
      requestedFresh = false;
      returnedWire.reset();
      Spec231ExactReadTask task(environment, current, [&] {
        return receiver.fetchSignedExactDataBatch("exact-authority", {name}, producer.getName(),
          250, [current] { return !current->load(); }, nullptr, {},
          ServiceProvider::ExactDataFetchPhase::AwaitPublication);
      });
      environment.pumpUntil([&] { return task.ready(); });
      BOOST_REQUIRE(task.ready());
      const auto result = task.get();
      BOOST_REQUIRE(result);
      BOOST_REQUIRE_EQUAL(result->size(), 1U);
      BOOST_CHECK(result->front() == payload);
      BOOST_CHECK(requestedFresh);
      BOOST_REQUIRE(returnedWire);
      BOOST_CHECK(*returnedWire == originalWire);
    };
    readOriginal(); // Control: the ordinary production read works before expiry.
    const auto afterFreshness = std::chrono::steady_clock::now() + 150ms;
    environment.pumpUntil([&] { return std::chrono::steady_clock::now() >= afterFreshness; });
    readOriginal();
  }
}

BOOST_AUTO_TEST_CASE(SignedPacketLimitAndRejectedBatchVisibility)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();

  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  ndn::signal::ScopedConnection interests = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { producerFace.receive(interest); });
  ndn::signal::ScopedConnection data = producerFace.onSendData.connect(
    [&] (const ndn::Data& packet) { consumerFace.receive(packet); });

  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "producer";
  assignment.service = profile.serviceName;
  // This transport fixture owns bounded authority; it does not test Core dispatch.
  assignment.workDeadline = std::chrono::steady_clock::now() + 90s;
  assignment.workCurrent = [] { return true; };
  const ndn::Name requestId("/spec181/exact-wire");
  ServiceProvider::CollaborationContext publisher(
    producer, profile.userIdentity, requestId, RequestMessage(), assignment);
  ServiceProvider::CollaborationContext receiver(
    consumer, profile.userIdentity, requestId, RequestMessage(), assignment);

  auto makeName = [&] (const std::string& suffix) {
    ndn::Name name(producer.getName());
    return name.append("NDNSF-DI").append("TENSOR").append(std::string(500, 'x'))
      .append(suffix);
  };
  auto signedSize = [&] (const ndn::Name& name, const ndn::Buffer& content) {
    ndn::Data packet(name);
    packet.setFreshnessPeriod(ndn::time::milliseconds(60000));
    packet.setContent(content);
    environment.keyChain().sign(packet, ndn::security::signingByCertificate(
      producer.getSigningCertificateName()));
    return packet.wireEncode().size();
  };
  auto payloadForSize = [&] (const ndn::Name& name, std::size_t wanted) {
    // RSA signatures have fixed size. At these lengths the TLV header width
    // remains fixed, so adjust only content and verify the actual signed wire.
    ndn::Buffer content(7000, 0x5a);
    const auto measured = signedSize(name, content);
    BOOST_REQUIRE_LT(measured, wanted);
    content.resize(content.size() + wanted - measured, 0x5a);
    BOOST_REQUIRE_EQUAL(signedSize(name, content), wanted);
    return content;
  };
  auto fetch = [&] (const ndn::Name& name) {
    auto pending = std::async(std::launch::async, [&] {
      return receiver.fetchSignedExactData(
        "wire-limit", name, producer.getName(), 250);
    });
    environment.pumpUntil([&] {
      return pending.wait_for(0ms) == std::future_status::ready;
    });
    return pending.get();
  };

  for (const auto wanted : {ndn::MAX_NDN_PACKET_SIZE - 1,
                            ndn::MAX_NDN_PACKET_SIZE}) {
    const auto name = makeName(std::to_string(wanted));
    const auto content = payloadForSize(name, wanted);
    std::size_t observedSize = 0;
    ndn::signal::ScopedConnection observe =
      producerFace.onSendData.connect([&] (const ndn::Data& packet) {
        if (packet.getName() == name) observedSize = packet.wireEncode().size();
      });
    BOOST_REQUIRE(publisher.publishSignedExactData(
      "wire-limit", {{name, content}}, 60000));
    const auto result = fetch(name);
    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL_COLLECTIONS(result->begin(), result->end(),
                                  content.begin(), content.end());
    BOOST_CHECK_EQUAL(observedSize, wanted);
  }

  const auto tooLargeName = makeName("too-large");
  const auto tooLarge = payloadForSize(tooLargeName, ndn::MAX_NDN_PACKET_SIZE + 1);
  BOOST_CHECK(!publisher.publishSignedExactData(
    "wire-limit", {{tooLargeName, tooLarge}}, 60000));

  const auto firstName = makeName("batch-first");
  const ndn::Buffer first{'o', 'k'};
  BOOST_CHECK(!publisher.publishSignedExactData(
    "wire-limit", {{firstName, first}, {tooLargeName, tooLarge}}, 60000));
  // A late size failure must not leave an apparently ready manifest in IMS.
  BOOST_CHECK(!fetch(firstName));
  BOOST_REQUIRE(publisher.publishSignedExactData(
    "wire-limit", {{firstName, first}}, 60000));
  const auto recovered = fetch(firstName);
  BOOST_REQUIRE(recovered);
  BOOST_CHECK_EQUAL_COLLECTIONS(recovered->begin(), recovered->end(),
                                first.begin(), first.end());

  auto observed = std::make_shared<std::vector<std::string>>();
  CollaborationTransferMetrics metrics;
  metrics.localWorkObserver.owner = observed;
  metrics.localWorkObserver.observe = [] (const std::shared_ptr<void>& owner,
      std::string_view phase, CollaborationLocalWorkObserver::Operation operation, void* context) {
    std::static_pointer_cast<std::vector<std::string>>(owner)->emplace_back(phase);
    operation(context);
  };
  const auto observedName = makeName("observed-publication");
  BOOST_REQUIRE(publisher.publishSignedExactData("wire-limit", {{observedName, first}},
    60000, &metrics));
  BOOST_REQUIRE(metrics.transportPayloadBytes);
  BOOST_CHECK_EQUAL(*metrics.transportPayloadBytes, first.size());
  for (const auto* phase : {"core-tensor-data-construct", "core-tensor-data-sign",
      "core-tensor-data-producer-admission", "core-tensor-data-wire-encode",
      "core-tensor-ims-insert", "core-tensor-pending-satisfy"})
    BOOST_CHECK_EQUAL(std::count(observed->begin(), observed->end(), phase), 1U);
  const auto observedPayload = fetch(observedName);
  BOOST_REQUIRE(observedPayload);
  BOOST_CHECK_EQUAL_COLLECTIONS(observedPayload->begin(), observedPayload->end(),
                                first.begin(), first.end());

  // The asynchronous Context path must retain the observer for every packet,
  // including replacements dispatched after the initial bounded window.
  const auto batchSecondName = makeName("observed-batch-second");
  BOOST_REQUIRE(publisher.publishSignedExactData(
    "wire-limit", {{batchSecondName, first}}, 60000));
  observed->clear();
  auto batch = receiver.startSignedExactDataBatch("wire-limit",
    {observedName, batchSecondName}, producer.getName(), 250, {}, {},
    ServiceProvider::ExactDataFetchPhase::Published, {}, 1, metrics.localWorkObserver);
  environment.pumpUntil([&] { return batch.wait_for(0ms) == std::future_status::ready; });
  const auto batchResult = batch.get();
  BOOST_REQUIRE(batchResult);
  BOOST_REQUIRE_EQUAL(batchResult->contents.size(), 2U);
  for (const auto& content : batchResult->contents)
    BOOST_CHECK_EQUAL_COLLECTIONS(content.begin(), content.end(), first.begin(), first.end());
  BOOST_CHECK_EQUAL(std::count(observed->begin(), observed->end(),
                              "core-exact-data-validator-call"), 2U);
  BOOST_CHECK_EQUAL(std::count(observed->begin(), observed->end(),
                              "core-exact-data-admission"), 2U);
}

BOOST_AUTO_TEST_CASE(Spec231ExactPublicationRejectsMissingExpiredAndRevokedAuthority)
{
  runSpec231ExactPublicationAuthorityCase(false);
}

BOOST_AUTO_TEST_CASE(Spec231ExactPublicationChecksAuthorityAtCommit)
{
  runSpec231ExactPublicationAuthorityCase(true);
}

BOOST_AUTO_TEST_CASE(Spec231ExactReadRejectsMissingExpiredAndRevokedAuthority)
{
  runSpec231ExactReadAuthorityCases({Spec231ReadFault::MISSING,
    Spec231ReadFault::EXPIRED, Spec231ReadFault::REVOKED});
}

BOOST_AUTO_TEST_CASE(Spec231ExactReadPreservesExternalCancellationAndRejectsLateData)
{
  runSpec231ExactReadAuthorityCases({Spec231ReadFault::NORMAL,
    Spec231ReadFault::LOSE_WORK, Spec231ReadFault::NORMAL,
    Spec231ReadFault::EXTERNAL_CANCEL, Spec231ReadFault::NORMAL});
}

BOOST_AUTO_TEST_CASE(Spec231ExactReadOwnedCancellationOutlivesContext)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  auto current = std::make_shared<std::atomic<bool>>(true);
  auto firstRead = std::make_shared<std::atomic<bool>>(false);
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "read-authority";
  assignment.service = profile.serviceName;
  assignment.workDeadline = std::chrono::steady_clock::now() + 90s;
  assignment.workCurrent = [current] { return current->load(); };
  auto context = std::make_unique<ServiceProvider::CollaborationContext>(
    environment.provider(1), profile.userIdentity, ndn::Name("/spec231/read-owned"),
    RequestMessage(), assignment);
  const auto missing = ndn::Name(environment.provider(0).getName()).append("missing");
  auto operation = context->startSignedExactDataBatch("read-authority", {missing},
    environment.provider(0).getName(), 1500, [] { return false; },
    [firstRead] (std::exception_ptr error) { if (!error) firstRead->store(true); },
    ServiceProvider::ExactDataFetchPhase::AwaitPublication, {}, 1);
  struct ReadCleanup
  {
    NdnsfIntegrationEnvironment& environment;
    ServiceProvider::ExactDataFetchOperation& operation;
    std::shared_ptr<std::atomic<bool>> current;
    bool consumed = false;
    ~ReadCleanup()
    {
      current->store(false);
      if (!consumed) {
        operation.cancel();
        environment.pumpUntil([&] {
          return operation.wait_for(0ms) == std::future_status::ready;
        });
        BOOST_CHECK(operation.wait_for(0ms) == std::future_status::ready);
        if (operation.wait_for(0ms) == std::future_status::ready) {
          (void)operation.get();
        }
      }
    }
  } cleanup{environment, operation, current};
  environment.pumpUntil([&] { return firstRead->load(); });
  BOOST_REQUIRE(firstRead->load());
  context.reset();
  const auto cancelledAt = std::chrono::steady_clock::now();
  current->store(false);
  environment.pumpUntil([&] { return operation.wait_for(0ms) == std::future_status::ready; });
  BOOST_REQUIRE(operation.wait_for(0ms) == std::future_status::ready);
  cleanup.consumed = true;
  BOOST_CHECK(!operation.get());
  BOOST_CHECK(std::chrono::steady_clock::now() - cancelledAt < 1s);
}

BOOST_AUTO_TEST_CASE(Spec231ExactReadChecksCancellationAtValidatedAdmission)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  ndn::signal::ScopedConnection interests = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { producerFace.receive(interest); });
  ndn::signal::ScopedConnection data = producerFace.onSendData.connect(
    [&] (const ndn::Data& packet) { consumerFace.receive(packet); });
  struct AdmissionState
  {
    unsigned fault = 0;
    std::shared_ptr<std::atomic<bool>> current = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> external{false};
    std::atomic<bool> throwFromCancellation{false};
    std::atomic<size_t> admissions{0};
    std::atomic<size_t> operations{0};
    std::atomic<size_t> progress{0};
  };
  for (unsigned fault = 0; fault < 4; ++fault) {
    auto state = std::make_shared<AdmissionState>();
    state->fault = fault;
    ServiceProvider::CollaborationAssignment assignment;
    assignment.role = "read-admission";
    assignment.service = profile.serviceName;
    assignment.workDeadline = std::chrono::steady_clock::now() + 90s;
    assignment.workCurrent = [] { return true; };
    const ndn::Name requestId("/spec231/read-admission");
    ServiceProvider::CollaborationContext publisher(
      producer, profile.userIdentity, requestId, RequestMessage(), assignment);
    const auto name = ndn::Name(producer.getName()).append("READ-ADMIT").appendNumber(fault);
    const ndn::Buffer payload{'a', 'd', 'm', 'i', 't'};
    BOOST_REQUIRE(publisher.publishSignedExactData("read-admission", {{name, payload}}, 60000));
    assignment.workCurrent = [current = state->current] { return current->load(); };
    ServiceProvider::CollaborationContext receiver(
      consumer, profile.userIdentity, requestId, RequestMessage(), assignment);
    CollaborationTransferMetrics metrics;
    metrics.localWorkObserver.owner = state;
    metrics.localWorkObserver.observe = [] (const std::shared_ptr<void>& owner,
      std::string_view phase, CollaborationLocalWorkObserver::Operation operation, void* input) {
      const auto observed = std::static_pointer_cast<AdmissionState>(owner);
      if (phase == "core-exact-data-admission") {
        ++observed->admissions;
        if (observed->fault == 1) { observed->current->store(false); }
        if (observed->fault == 2) { observed->external.store(true); }
        if (observed->fault == 3) { observed->throwFromCancellation.store(true); }
        // Invoke the original admission exactly once, synchronously. A poll
        // cannot intervene between this fault and the production admission.
        operation(input);
        ++observed->operations;
        return;
      }
      operation(input);
    };
    Spec231ExactReadTask task(environment, state->current, [&] {
      return receiver.fetchSignedExactDataBatch("read-admission", {name}, producer.getName(),
        1500, [state] {
          if (state->throwFromCancellation.load()) {
            throw std::runtime_error("expected admission cancellation predicate failure");
          }
          return state->external.load();
        }, &metrics, {}, ServiceProvider::ExactDataFetchPhase::Published, [state] {
          ++state->progress;
          return true;
        });
    });
    environment.pumpUntil([&] { return task.ready(); });
    BOOST_REQUIRE(task.ready());
    const auto result = task.get();
    BOOST_CHECK_EQUAL(state->admissions.load(), 1U);
    BOOST_CHECK_EQUAL(state->operations.load(), 1U);
    BOOST_CHECK_EQUAL(state->progress.load(), fault == 0 ? 1U : 0U);
    if (fault == 0) {
      BOOST_REQUIRE(result);
      BOOST_REQUIRE_EQUAL(result->size(), 1U);
      BOOST_CHECK_EQUAL_COLLECTIONS(result->front().begin(), result->front().end(),
                                    payload.begin(), payload.end());
    }
    else {
      BOOST_CHECK(!result);
    }
  }
}

BOOST_AUTO_TEST_CASE(ExactReadWorkerDestructionDrainsSuccessAndCancellation)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  ndn::signal::ScopedConnection interests = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { producerFace.receive(interest); });
  ndn::signal::ScopedConnection data = producerFace.onSendData.connect(
    [&] (const ndn::Data& packet) { consumerFace.receive(packet); });
  const auto faceThread = std::this_thread::get_id();
  const ndn::Buffer payload(128, 0x57);
  for (unsigned iteration = 0; iteration < 32; ++iteration) {
    const bool cancel = iteration % 2 != 0;
    auto current = std::make_shared<std::atomic<bool>>(true);
    ServiceProvider::CollaborationAssignment assignment;
    assignment.role = "worker-read";
    assignment.service = profile.serviceName;
    assignment.workDeadline = std::chrono::steady_clock::now() + 90s;
    assignment.workCurrent = [current] { return current->load(); };
    const ndn::Name requestId("/spec223/worker-read/" + std::to_string(iteration));
    ServiceProvider::CollaborationContext publisher(
      producer, profile.userIdentity, requestId, RequestMessage(), assignment);
    ServiceProvider::CollaborationContext receiver(
      consumer, profile.userIdentity, requestId, RequestMessage(), assignment);
    const auto name = ndn::Name(producer.getName()).append("worker-read")
      .append(std::to_string(iteration));
    if (!cancel) {
      BOOST_REQUIRE(publisher.publishSignedExactData("worker-read", {{name, payload}}, 60000));
    }
    auto destroyedOnWorker = std::make_shared<std::atomic<bool>>(false);
    Spec231ExactReadTask task(environment, current, [&] {
      Spec231ReadResult result;
      {
        auto firstRead = std::make_shared<std::promise<void>>();
        auto started = firstRead->get_future();
        auto operation = receiver.startSignedExactDataBatch("worker-read", {name},
          producer.getName(), 1500, [] { return false; },
          [firstRead] (std::exception_ptr error) {
            if (error) firstRead->set_exception(error);
            else firstRead->set_value();
          },
          ServiceProvider::ExactDataFetchPhase::AwaitPublication, {}, 1);
        if (cancel) {
          if (started.wait_for(2s) != std::future_status::ready) {
            operation.cancel();
            throw std::runtime_error("worker exact-read dispatch did not start");
          }
          started.get();
          operation.cancel();
        }
        const auto fetched = operation.get();
        if (fetched) result = fetched->contents;
      }
      destroyedOnWorker->store(std::this_thread::get_id() != faceThread);
      return result;
    });
    environment.pumpUntil([&] { return task.ready(); });
    BOOST_REQUIRE(task.ready());
    const auto result = task.get();
    BOOST_CHECK(destroyedOnWorker->load());
    if (cancel) {
      BOOST_CHECK(!result);
    }
    else {
      BOOST_REQUIRE(result);
      BOOST_REQUIRE_EQUAL(result->size(), 1U);
      BOOST_CHECK(result->front() == payload);
    }
  }
}

BOOST_AUTO_TEST_CASE(SpeculativeBatchBacksOffUnpublishedReadinessRenewals)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  const auto prefix = ndn::Name(producer.getName()).append("delayed-batch");
  const std::vector<ndn::Name> names{ndn::Name(prefix).appendSegment(0),
                                   ndn::Name(prefix).appendSegment(1)};
  using Clock = std::chrono::steady_clock;
  std::array<std::vector<Clock::time_point>, 2> sends;
  ndn::signal::ScopedConnection interests = consumerFace.onSendInterest.connect(
    [&](const ndn::Interest& interest) {
      for (std::size_t i = 0; i < names.size(); ++i) {
        if (interest.getName() == names[i]) {
          sends[i].push_back(Clock::now());
          producerFace.receive(interest);
          return;
        }
      }
    });
  ndn::signal::ScopedConnection data = producerFace.onSendData.connect(
    [&](const ndn::Data& packet) { consumerFace.receive(packet); });
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "readiness";
  assignment.service = profile.serviceName;
  assignment.workDeadline = Clock::now() + 30s;
  assignment.workCurrent = [] { return true; };
  const ndn::Name requestId("/filesystem/readiness-batch");
  ServiceProvider::CollaborationContext publisher(
    producer, profile.userIdentity, requestId, RequestMessage(), assignment);
  ServiceProvider::CollaborationContext receiver(
    consumer, profile.userIdentity, requestId, RequestMessage(), assignment);
  auto batch = receiver.startSignedExactDataBatch(
    "exact-authority", names, producer.getName(), 4500, {}, {},
    ServiceProvider::ExactDataFetchPhase::AwaitPublication);
  environment.pumpUntil([&] {
    return (sends[0].size() >= 3 && sends[1].size() >= 3) ||
           batch.wait_for(0ms) == std::future_status::ready;
  });
  for (const auto& times : sends) {
    BOOST_REQUIRE_GE(times.size(), 3U);
    const auto first = times[1] - times[0];
    const auto second = times[2] - times[1];
    BOOST_CHECK(second >= first + first / 2);
  }
  const std::vector<std::pair<ndn::Name, ndn::Buffer>> objects{
    {names[0], {'a'}}, {names[1], {'b'}}};
  std::vector<std::shared_ptr<const ndn::Data>> originals;
  BOOST_REQUIRE(ExactPublicationOracle::publishBatch(publisher, objects, originals));
  batch.confirmPublication();
  environment.pumpUntil([&] { return batch.wait_for(0ms) == std::future_status::ready; });
  BOOST_REQUIRE(batch.wait_for(0ms) == std::future_status::ready);
  const auto result = batch.get();
  BOOST_REQUIRE(result);
  BOOST_REQUIRE_EQUAL(result->contents.size(), names.size());
  BOOST_CHECK(result->contents[0] == objects[0].second);
  BOOST_CHECK(result->contents[1] == objects[1].second);
}

BOOST_AUTO_TEST_CASE(AwaitPublicationSurvivesMoreThanSixReadinessNacks)
{
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  const auto name = ndn::Name(producer.getName()).append("DELAYED-READINESS");
  std::atomic<size_t> attempts{0};
  ndn::signal::ScopedConnection interests = consumerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      if (interest.getName() == name && ++attempts <= 7) {
        consumerFace.receive(ndn::lp::Nack(interest).setReason(ndn::lp::NackReason::NO_ROUTE));
      }
      else {
        producerFace.receive(interest);
      }
    });
  ndn::signal::ScopedConnection data = producerFace.onSendData.connect(
    [&] (const ndn::Data& packet) { consumerFace.receive(packet); });
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "readiness";
  assignment.service = profile.serviceName;
  assignment.workDeadline = std::chrono::steady_clock::now() + 90s;
  assignment.workCurrent = [] { return true; };
  const ndn::Name requestId("/filesystem/readiness");
  ServiceProvider::CollaborationContext publisher(
    producer, profile.userIdentity, requestId, RequestMessage(), assignment);
  const ndn::Buffer payload{'r', 'e', 'a', 'd', 'y'};
  BOOST_REQUIRE(publisher.publishSignedExactData("readiness", {{name, payload}}, 60000));
  ServiceProvider::CollaborationContext receiver(
    consumer, profile.userIdentity, requestId, RequestMessage(), assignment);
  auto current = std::make_shared<std::atomic<bool>>(true);
  Spec231ExactReadTask task(environment, current, [&] {
    return receiver.fetchSignedExactDataBatch("readiness", {name}, producer.getName(),
      15000, [current] { return !current->load(); }, nullptr, {},
      ServiceProvider::ExactDataFetchPhase::AwaitPublication, {});
  });
  environment.pumpUntil([&] { return task.ready(); });
  BOOST_REQUIRE(task.ready());
  const auto result = task.get();
  BOOST_REQUIRE(result);
  BOOST_REQUIRE_EQUAL(result->size(), 1U);
  BOOST_CHECK(result->front() == payload);
  BOOST_CHECK_GE(attempts.load(), 8U);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
