#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"

#include <boost/test/unit_test.hpp>
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include <future>
#include <deque>
#include <limits>
#include <thread>

namespace ndnsf::di {
// This friend is defined only in the unit test; no configurable production
// constructor or installed test-clock factory is exposed by the library.
class NativeClientTestAccess {
public:
  using Port = NativeInferenceClient::TestPort;
  static std::unique_ptr<NativeInferenceClient> create(
      const Port& port, std::shared_ptr<ndn_service_framework::ServiceUser> user,
      std::shared_ptr<const NativeAdapterRegistry> adapters) {
    return std::unique_ptr<NativeInferenceClient>(
      new NativeInferenceClient(port, std::move(user), std::move(adapters)));
  }
};
}

using namespace ndnsf::di;

BOOST_AUTO_TEST_SUITE(Spec182NativeInferenceClient)

BOOST_AUTO_TEST_CASE(EmptyHandleFailsClosedAndErrorKeepsStructuredIdentity)
{
  NativeInferenceHandle handle;
  BOOST_CHECK_THROW(handle.status(), NativeDiError);
  BOOST_CHECK_THROW(handle.result(std::chrono::milliseconds(0)), NativeDiError);

  NativeDiError error("INVALID_REQUEST", "local", "request", "bad input",
                      "/NDNSF/DI/REQUEST/1", 1);
  BOOST_CHECK_EQUAL(error.code(), "INVALID_REQUEST");
  BOOST_CHECK_EQUAL(error.domain(), "local");
  BOOST_CHECK_EQUAL(error.boundary(), "request");
  BOOST_CHECK_EQUAL(error.requestId(), "/NDNSF/DI/REQUEST/1");
  BOOST_CHECK_EQUAL(error.attempt(), 1U);
}

BOOST_AUTO_TEST_CASE(ClientRejectsMissingCoreOwner)
{
  auto adapters = std::make_shared<NativeAdapterRegistry>();
  BOOST_CHECK_THROW(
    NativeInferenceClient(nullptr, adapters), NativeDiError);
  BOOST_CHECK_THROW(
    NativeInferenceClient(nullptr, nullptr), NativeDiError);
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
class ClientTestAdapter final : public NativeModelAdapter {
public:
  std::string adapterId() const override { return "client-test"; }
  std::string adapterVersion() const override { return "1"; }
  NativeModelDescriptor inspect(const std::string&, const std::string&) const override
  { return {}; }
  std::vector<std::uint8_t> encodeInput(const std::vector<std::uint8_t>& x) const override
  { return x; }
  std::vector<std::uint8_t> decodeResult(const std::vector<std::uint8_t>& x) const override
  { return x; }
};
class ClientTestSplit final : public NativeModelSplitStrategy {
public:
  NativeStrategyIdentity identity() const override { return {"test", "1", nativePlanningDigest("{}")} ; }
  std::vector<NativeSplitCandidate> enumerate(const NativeModelDescriptor&,
      const NativeGraphSnapshot&, const NativeCandidateBudget&) const override { return {}; }
};
struct ClientStateFixture {
  ndn::security::KeyChain keyChain{"pib-memory:", "tpm-memory:"};
  ndn::DummyClientFace face{keyChain};
  std::shared_ptr<ndn_service_framework::test::LocalServiceUser> user;
  std::shared_ptr<NativeAdapterRegistry> adapters = std::make_shared<NativeAdapterRegistry>();
  std::deque<std::function<void()>> work;
  struct Timer {
    std::chrono::steady_clock::time_point deadline;
    std::function<void()> fire;
    bool cancelled = false;
  };
  std::vector<std::shared_ptr<Timer>> timers;
  std::chrono::steady_clock::time_point now{};
  NativeModelRef model;
  ClientStateFixture() {
    using namespace ndn_service_framework::test;
    auto cert = makeRsaIdentity(keyChain, ndn::Name("/client-test/user"));
    auto aa = makeRsaIdentity(keyChain, ndn::Name("/client-test/aa"));
    user = std::make_shared<LocalServiceUser>(face, ndn::Name("/client-test"), cert, aa,
                                             "examples/trust-any.conf");
    adapters->registerAdapter(std::make_shared<ClientTestAdapter>());
    adapters->freeze();
    model.modelName = "client-model";
    model.contentDigest = nativePlanningDigest("model");
    model.semanticsDigest = nativePlanningDigest("semantics");
    model.graphDigest = nativePlanningDigest("graph");
    model.modelFormat = "onnx";
    model.precision = "float32";
    model.adapterId = "client-test";
    model.adapterVersion = "1";
    model.adapter = fixture::modelAdapter(model.adapterId, model.adapterVersion, model.modelFormat, model.precision);
  }
  NativeClientTestAccess::Port port() {
    return {[this] { return now; },
      [this](std::function<void()> f) { work.push_back(std::move(f)); },
      [this](std::chrono::steady_clock::time_point deadline, std::function<void()> fire) {
        auto timer = std::make_shared<Timer>(Timer{deadline, std::move(fire)});
        timers.push_back(timer);
        return [timer] { timer->cancelled = true; timer->fire = {}; };
      }};
  }
  NativeInferenceHandle request(NativeInferenceClient& client, NativeRequestOptions options = {}) {
    NativeApplicationInput input;
    input.payload = {1};
    return client.request(model, input, std::make_shared<ClientTestSplit>(),
                          std::make_shared<NativePreSplitFirstPlacement>(), options);
  }
};
}

BOOST_FIXTURE_TEST_SUITE(Spec182ClientState, ClientStateFixture)

BOOST_AUTO_TEST_CASE(SlowObserverDoesNotBlockCancelAndLateReplaySurvivesClientClose)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  auto handle = request(*client);
  std::promise<void> entered, release;
  auto enteredFuture = entered.get_future();
  auto gate = release.get_future().share();
  handle.observe([&](const NativeInferenceEvent&) { entered.set_value(); gate.wait(); });
  auto cancelling = std::async(std::launch::async, [&] { handle.cancel(); });
  auto enteredState = enteredFuture.wait_for(std::chrono::seconds(2));
  auto cancelState = cancelling.wait_for(std::chrono::milliseconds(200));
  release.set_value();
  cancelling.get();
  BOOST_CHECK(enteredState == std::future_status::ready);
  BOOST_CHECK(cancelState == std::future_status::ready);
  BOOST_CHECK(handle.status() == NativeRequestStatus::Cancelled);
  client.reset();
  auto replay = std::make_shared<std::promise<std::string>>();
  auto replayed = replay->get_future();
  handle.observe([replay](const NativeInferenceEvent& e) { replay->set_value(e.requestId); });
  BOOST_REQUIRE(replayed.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_CHECK_EQUAL(replayed.get(), handle.requestId());
  while (!work.empty()) { auto f = std::move(work.front()); work.pop_front(); f(); }
  BOOST_CHECK(handle.status() == NativeRequestStatus::Cancelled);
}

BOOST_AUTO_TEST_CASE(LocalWaitDoesNotTerminateRequestAndExpiredDispatchFails)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  auto handle = request(*client);
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "LOCAL_WAIT_TIMEOUT"; });
  BOOST_CHECK(handle.status() == NativeRequestStatus::Pending);
  now += std::chrono::seconds(31);
  work.front()();
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "NATIVE_REQUEST_TIMEOUT"; });
  handle.cancel();
  BOOST_CHECK(handle.status() == NativeRequestStatus::Failed);
}

BOOST_AUTO_TEST_CASE(SubmissionFailureReturnsFailedHandle)
{
  auto throwingPort = port();
  throwingPort.submitHook = [](std::function<void()>) { throw std::runtime_error("queue unavailable"); };
  auto client = NativeClientTestAccess::create(throwingPort, user, adapters);
  auto handle = request(*client);
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "NATIVE_REQUEST_DISPATCH_FAILED"; });
  BOOST_REQUIRE_EQUAL(timers.size(), 1U);
  BOOST_CHECK(timers.front()->cancelled);
}

BOOST_AUTO_TEST_CASE(DeadlineFiresWithoutDispatchAndCannotBeRearmedByWait)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  auto handle = request(*client);
  BOOST_REQUIRE_EQUAL(timers.size(), 1U);
  BOOST_CHECK(timers.front()->deadline == now + std::chrono::seconds(30));
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "LOCAL_WAIT_TIMEOUT"; });
  auto lateDeadline = timers.front()->fire;
  now += std::chrono::seconds(30);
  lateDeadline();
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "NATIVE_REQUEST_TIMEOUT"; });
  BOOST_CHECK(timers.front()->cancelled);
  BOOST_CHECK_EQUAL(timers.size(), 1U);
  // Neither the overdue queued request nor an already-dispatched timer may
  // resurrect this operation or replace its first terminal error.
  work.front()();
  lateDeadline();
  handle.cancel();
  BOOST_CHECK(handle.status() == NativeRequestStatus::Failed);
}

BOOST_AUTO_TEST_CASE(CancelAndCloseRemoveTimersAndIgnoreLateExpiry)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  auto cancelled = request(*client);
  auto closing = request(*client);
  auto lateFirst = timers.at(0)->fire;
  auto lateSecond = timers.at(1)->fire;
  cancelled.cancel();
  client.reset();
  BOOST_CHECK(timers.at(0)->cancelled);
  BOOST_CHECK(timers.at(1)->cancelled);
  lateFirst();
  lateSecond();
  for (auto& task : work) task();
  BOOST_CHECK(cancelled.status() == NativeRequestStatus::Cancelled);
  BOOST_CHECK(closing.status() == NativeRequestStatus::Cancelled);
}

BOOST_AUTO_TEST_CASE(RealDeadlineDoesNotWaitForWorkOrSlowObserver)
{
  auto realPort = port();
  realPort.now = {};
  realPort.scheduleHook = {};
  auto client = NativeClientTestAccess::create(realPort, user, adapters);
  auto first = request(*client);
  std::promise<void> entered, release;
  auto enteredFuture = entered.get_future();
  auto gate = release.get_future().share();
  first.observe([&](const NativeInferenceEvent&) { entered.set_value(); gate.wait(); });
  first.cancel();
  const auto enteredState = enteredFuture.wait_for(std::chrono::seconds(2));
  NativeRequestOptions options;
  options.timeoutMs = 50;
  options.ackTimeoutMs = 10;
  auto pending = request(*client, options);
  std::string errorCode;
  try { pending.result(std::chrono::seconds(2)); }
  catch (const NativeDiError& error) { errorCode = error.code(); }
  // Always release the worker before any fatal test assertion/unwinding.
  release.set_value();
  BOOST_CHECK(enteredState == std::future_status::ready);
  BOOST_CHECK_EQUAL(errorCode, "NATIVE_REQUEST_TIMEOUT");
  BOOST_CHECK(pending.status() == NativeRequestStatus::Failed);
  BOOST_CHECK_EQUAL(work.size(), 2U); // No DI work was pumped to cause expiry.
  // Queue a barrier to ensure the callback's referenced promises are no
  // longer used when the test returns.
  auto barrier = std::make_shared<std::promise<void>>();
  auto done = barrier->get_future();
  pending.observe([barrier](const NativeInferenceEvent&) { barrier->set_value(); });
  BOOST_REQUIRE(done.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
}

BOOST_AUTO_TEST_CASE(InvalidDeadlinesAndNegativeWaitAreRejectedWithoutSubmission)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  NativeRequestOptions options;
  options.timeoutMs = options.ackTimeoutMs;
  BOOST_CHECK_EXCEPTION(request(*client, options), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "INVALID_REQUEST"; });
  options.timeoutMs = std::numeric_limits<std::uint64_t>::max();
  BOOST_CHECK_EXCEPTION(request(*client, options), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "INVALID_REQUEST"; });
  BOOST_CHECK(work.empty());
  BOOST_CHECK(timers.empty());
  auto handle = request(*client);
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(-1)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "INVALID_WAIT_TIMEOUT"; });
  BOOST_CHECK(handle.status() == NativeRequestStatus::Pending);
}

BOOST_AUTO_TEST_CASE(DispatchExceptionIsContainedAndRemovesTimer)
{
  auto failingClock = port();
  unsigned calls = 0;
  failingClock.now = [&] {
    if (calls++ > 0) throw std::runtime_error("private diagnostic detail");
    return now;
  };
  auto client = NativeClientTestAccess::create(failingClock, user, adapters);
  auto handle = request(*client);
  BOOST_CHECK_NO_THROW(work.front()());
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                        [](const NativeDiError& e) {
                          return e.code() == "NATIVE_REQUEST_DISPATCH_FAILED" &&
                            std::string(e.what()).find("private diagnostic") == std::string::npos;
                        });
  BOOST_CHECK(timers.at(0)->cancelled);
}

BOOST_AUTO_TEST_CASE(CorePostUsesSharedFaceAndNeverRunsInline)
{
  BOOST_CHECK(!user->isOnIoThread());
  BOOST_CHECK_THROW(user->postToIo({}), std::invalid_argument);
  bool outerRan = false, outerReturned = false, innerRan = false;
  const auto ioThread = std::this_thread::get_id();
  auto submitter = std::async(std::launch::async, [&] {
    user->postToIo([&] {
      outerRan = true;
      BOOST_CHECK(user->isOnIoThread());
      BOOST_CHECK(std::this_thread::get_id() == ioThread);
      user->postToIo([&] {
        innerRan = true;
        BOOST_CHECK(outerReturned);
        BOOST_CHECK(user->isOnIoThread());
      });
      BOOST_CHECK(!innerRan);
      outerReturned = true;
    });
  });
  submitter.get();
  BOOST_CHECK(!outerRan);
  face.getIoContext().restart();
  face.getIoContext().poll();
  BOOST_CHECK(outerRan);
  BOOST_CHECK(innerRan);
  BOOST_CHECK(!user->isOnIoThread());
}

BOOST_AUTO_TEST_CASE(CoreIoRejectsBlockingResultButAllowsPollAndCancel)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  auto handle = request(*client);
  bool checked = false;
  user->postToIo([&] {
    checked = true;
    BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(1)), NativeDiError,
                          [&](const NativeDiError& e) {
                            return e.code() == "CORE_IO_WAIT_FORBIDDEN" &&
                              e.requestId() == handle.requestId();
                          });
    BOOST_CHECK(handle.status() == NativeRequestStatus::Pending);
    BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                          [](const NativeDiError& e) { return e.code() == "LOCAL_WAIT_TIMEOUT"; });
    handle.cancel();
    BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                          [](const NativeDiError& e) { return e.code() == "CANCELLED"; });
  });
  face.getIoContext().restart();
  face.getIoContext().poll();
  BOOST_CHECK(checked);
  BOOST_CHECK(handle.status() == NativeRequestStatus::Cancelled);
}

BOOST_AUTO_TEST_CASE(HandleRetainsCoreOwnerAfterClientClose)
{
  auto client = NativeClientTestAccess::create(port(), user, adapters);
  auto handle = request(*client);
  std::weak_ptr<ndn_service_framework::ServiceUser> weakUser = user;
  client.reset();
  user.reset();
  BOOST_CHECK(!weakUser.expired());
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(1)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "CANCELLED"; });
  work.clear();
  handle = NativeInferenceHandle{};
  BOOST_CHECK(weakUser.expired());
}

BOOST_AUTO_TEST_SUITE_END()
