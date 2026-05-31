#include "tests/boost-test.hpp"

#include "generic-dynamic-api-fixture.hpp"
#include "ndn-service-framework/LocalServiceRegistry.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace ndn_service_framework::test {
namespace {

BOOST_AUTO_TEST_SUITE(GenericDynamicApiLocalInvocation)

BOOST_AUTO_TEST_CASE(UnregisteredServiceFailsClosed)
{
  LocalServiceRegistry registry;
  DynamicRequest request;
  request.setPayload("hello");

  auto result = registry.localInvoke<DynamicRequest, DynamicResponse>(
    ndn::Name("/Local/Only"), request);

  BOOST_CHECK(!result.success);
  BOOST_CHECK(result.error.find("not registered") != std::string::npos);
  BOOST_CHECK(!registry.hasService(ndn::Name("/Local/Only")));
}

BOOST_AUTO_TEST_CASE(SynchronousLocalInvocationUsesTypedHandler)
{
  LocalServiceRegistry registry;
  std::atomic<int> calls{0};
  ndn::Name requesterSeen;

  registry.registerLocalService<DynamicRequest, DynamicResponse>(
    ndn::Name("/Telemetry/GetStatus"),
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [&] (const ndn::Name& requester,
           const DynamicRequest& request,
           DynamicResponse& response) {
        ++calls;
        requesterSeen = requester;
        BOOST_CHECK_EQUAL(request.getPayload(), "altitude");
        response.setClassification(42);
      }));

  DynamicRequest request;
  request.setPayload("altitude");
  auto result = registry.localInvoke<DynamicRequest, DynamicResponse>(
    ndn::Name("/Telemetry/GetStatus"), request, ndn::Name("/local/container"));

  BOOST_REQUIRE(result.success);
  BOOST_CHECK_EQUAL(result.response.getClassification(), 42);
  BOOST_CHECK_EQUAL(calls.load(), 1);
  BOOST_CHECK_EQUAL(requesterSeen.toUri(), "/local/container");
  BOOST_CHECK(registry.hasService(ndn::Name("/Telemetry/GetStatus")));
}

BOOST_AUTO_TEST_CASE(FutureAsyncLocalInvocationWorks)
{
  LocalServiceRegistry registry;
  registry.registerLocalService<DynamicRequest, DynamicResponse>(
    ndn::Name("/ObjectDetection/Local"),
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [] (const ndn::Name&,
          const DynamicRequest& request,
          DynamicResponse& response) {
        response.setClassification(static_cast<int>(request.getPayload().size()));
      }));

  DynamicRequest request;
  request.setPayload("car");
  auto future = registry.localInvokeAsync<DynamicRequest, DynamicResponse>(
    ndn::Name("/ObjectDetection/Local"), request);

  auto result = future.get();
  BOOST_REQUIRE(result.success);
  BOOST_CHECK_EQUAL(result.response.getClassification(), 3);
}

BOOST_AUTO_TEST_CASE(CallbackAsyncLocalInvocationWorks)
{
  LocalServiceRegistry registry;
  registry.registerLocalService<DynamicRequest, DynamicResponse>(
    ndn::Name("/FlightControl/Check"),
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [] (const ndn::Name&,
          const DynamicRequest&,
          DynamicResponse& response) {
        response.setClassification(7);
      }));

  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool errorCalled = false;
  int classification = 0;

  DynamicRequest request;
  request.setPayload("ready");
  registry.localInvokeAsync<DynamicRequest, DynamicResponse>(
    ndn::Name("/FlightControl/Check"), request,
    [&] (const DynamicResponse& response) {
      std::lock_guard<std::mutex> lock(mutex);
      classification = response.getClassification();
      done = true;
      cv.notify_one();
    },
    [&] (const std::string&) {
      std::lock_guard<std::mutex> lock(mutex);
      errorCalled = true;
      done = true;
      cv.notify_one();
    });

  std::unique_lock<std::mutex> lock(mutex);
  BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] { return done; }));
  BOOST_CHECK(!errorCalled);
  BOOST_CHECK_EQUAL(classification, 7);
}

BOOST_AUTO_TEST_CASE(LocalInvocationDoesNotUseRemotePublisher)
{
  LocalServiceRegistry registry;
  registry.registerLocalService<DynamicRequest, DynamicResponse>(
    ndn::Name("/Local/Computation"),
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [] (const ndn::Name&,
          const DynamicRequest&,
          DynamicResponse& response) {
        response.setClassification(100);
      }));

  ndn::DummyClientFace face;
  ndn::security::KeyChain keyChain;
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/local/user"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/local/aa"));
  LocalServiceUser user(face, ndn::Name("/local/group"), userCert, aaCert,
                        "examples/trust-any.conf");
  int publisherCalls = 0;
  user.setRequestPublisher([&] (const ndn::Name&,
                                const ndn::Name&,
                                const std::vector<ndn::Name>&,
                                const ndn::Name&,
                                const RequestMessage&,
                                size_t) {
    ++publisherCalls;
  });

  DynamicRequest request;
  request.setPayload("local-only");
  auto result = registry.localInvoke<DynamicRequest, DynamicResponse>(
    ndn::Name("/Local/Computation"), request);

  BOOST_REQUIRE(result.success);
  BOOST_CHECK_EQUAL(result.response.getClassification(), 100);
  BOOST_CHECK_EQUAL(publisherCalls, 0);
  BOOST_CHECK_EQUAL(user.getPendingCallCount(), 0);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndn_service_framework::test
