#include "tests/boost-test.hpp"
#include "ndnsf-integration-fixture.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/signal/scoped-connection.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <tuple>

namespace ndn_service_framework::test {
using namespace std::chrono_literals;

/** Test access to the guarded Context path, never the legacy Provider helper. */
struct ExactPublicationOracle
{
  static bool
  publish(ServiceProvider::CollaborationContext& context, const ndn::Name& name,
          const ndn::Buffer& payload, CollaborationTransferMetrics* metrics,
          std::vector<std::shared_ptr<const ndn::Data>>& originals)
  {
    return context.publishOwnedExactData(
      "exact-authority", {{name, payload}}, 60000, metrics, originals, 65536);
  }

  static bool
  replay(ServiceProvider::CollaborationContext& context,
         const std::vector<std::shared_ptr<const ndn::Data>>& originals,
         CollaborationTransferMetrics* metrics)
  {
    return context.replayOwnedExactData("exact-authority", originals, metrics);
  }
};

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

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
