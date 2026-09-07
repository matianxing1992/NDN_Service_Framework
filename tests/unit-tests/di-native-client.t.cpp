#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"

#include <boost/test/unit_test.hpp>
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include <future>
#include <deque>

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
  }
  NativeClientTestPort port() {
    return {[this] { return now; }, [this](std::function<void()> f) { work.push_back(std::move(f)); }};
  }
  NativeInferenceHandle request(NativeInferenceClient& client) {
    NativeApplicationInput input;
    input.payload = {1};
    return client.request(model, input, std::make_shared<ClientTestSplit>(),
                          std::make_shared<NativePreSplitFirstPlacement>(), {});
  }
};
}

BOOST_FIXTURE_TEST_SUITE(Spec182ClientState, ClientStateFixture)

BOOST_AUTO_TEST_CASE(SlowObserverDoesNotBlockCancelAndLateReplaySurvivesClientClose)
{
  auto client = std::make_unique<NativeInferenceClient>(port(), user, adapters);
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
  NativeInferenceClient client(port(), user, adapters);
  auto handle = request(client);
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
  NativeInferenceClient client(throwingPort, user, adapters);
  auto handle = request(client);
  BOOST_CHECK_EXCEPTION(handle.result(std::chrono::milliseconds(0)), NativeDiError,
                        [](const NativeDiError& e) { return e.code() == "NATIVE_REQUEST_DISPATCH_FAILED"; });
}

BOOST_AUTO_TEST_SUITE_END()
