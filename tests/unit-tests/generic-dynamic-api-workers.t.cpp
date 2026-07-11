#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)
BOOST_AUTO_TEST_SUITE(AllSelectedAndWorkers)

BOOST_AUTO_TEST_CASE(AllSelectedPublishesSelectionForEveryValidAckResponder)
{
  ndn::security::KeyChain keyChain("pib-memory:all-selected-selection",
                                   "tpm-memory:all-selected-selection");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name serviceName("/HELLO");
  const std::vector<ndn::Name> providers{
    ndn::Name("/test/provider/a"),
    ndn::Name("/test/provider/b"),
    ndn::Name("/test/provider/c")
  };
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-all-selected-selection"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  ndn::Name publishedRequestId;
  user.setRequestPublisher(
    [&] (const ndn::Name& requestId,
         const ndn::Name&,
         const std::vector<ndn::Name>&,
         const ndn::Name&,
         const RequestMessage& requestMessage,
         size_t strategy) {
      publishedRequestId = requestId;
      BOOST_CHECK_EQUAL(strategy, tlv::AllSelected);

      auto ackA = makeSuccessAckForRequest(requestMessage, "provider-token-a");
      auto ackB = makeSuccessAckForRequest(requestMessage, "provider-token-b");
      RequestAckMessage rejectedAck;
      rejectedAck.setStatus(false);
      rejectedAck.setMessage("busy");
      rejectedAck.setUserToken(requestMessage.getUserToken());

      BOOST_CHECK(user.handleRequestAckByName(
        makeRequestAckNameV2(providers[0], requesterName, serviceName, requestId), ackA));
      BOOST_CHECK(!user.handleRequestAckByName(
        makeRequestAckNameV2(providers[0], requesterName, serviceName, requestId), ackA));
      BOOST_CHECK(user.handleRequestAckByName(
        makeRequestAckNameV2(providers[1], requesterName, serviceName, requestId), ackB));
      BOOST_CHECK(!user.handleRequestAckByName(
        makeRequestAckNameV2(providers[1], requesterName, serviceName, requestId), ackB));
      BOOST_CHECK(user.handleRequestAckByName(
        makeRequestAckNameV2(providers[2], requesterName, serviceName, requestId), rejectedAck));
      BOOST_CHECK(!user.handleRequestAckByName(
        makeRequestAckNameV2(providers[2], requesterName, serviceName, requestId), rejectedAck));
    });

  RequestMessage request;
  auto requestId = user.RequestService(
    providers,
    serviceName,
    request,
    1,
    ServiceUser::AckSelectionStrategy::AllSelected,
    1000,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("AllSelected selection test should not time out");
    }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  BOOST_CHECK_EQUAL(requestId, publishedRequestId);
  pumpFace(face, ndn::time::milliseconds(5));

  const auto selected = user.getExpectedResponseProviders(requestId);
  BOOST_REQUIRE_EQUAL(selected.size(), 2);
  BOOST_CHECK(namesContain(selected, providers[0]));
  BOOST_CHECK(namesContain(selected, providers[1]));
  BOOST_CHECK(!namesContain(selected, providers[2]));

  const auto selectedForPublication = user.getSelectionPublishedProviders(requestId);
  BOOST_REQUIRE_EQUAL(selectedForPublication.size(), 2);
  BOOST_CHECK(namesContain(selectedForPublication, providers[0]));
  BOOST_CHECK(namesContain(selectedForPublication, providers[1]));
  BOOST_CHECK(!namesContain(selectedForPublication, providers[2]));
}

BOOST_AUTO_TEST_CASE(AllSelectedProvidersExecuteOnlyAfterSelection)
{
  ndn::security::KeyChain keyChain("pib-memory:all-selected-provider-selection",
                                   "tpm-memory:all-selected-provider-selection");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/a");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-all-selected-provider");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-all-selected-provider"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  int executions = 0;
  provider.addHandler<DynamicRequest, DynamicResponse>(
    serviceName,
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [&] (const ndn::Name&, const DynamicRequest&, DynamicResponse& response) {
        ++executions;
        response.setClassification(7);
      }));

  RequestMessage request = makeRequestMessageWithUserToken("hello");
  request.setStrategy(tlv::AllSelected);
  auto requestBlock = request.WireEncode();
  ndn::Buffer requestBuffer(requestBlock.data(), requestBlock.size());

  provider.OnRequestDecryptionSuccessCallbackV2(requesterName,
                                                serviceName,
                                                requestId,
                                                requestBuffer);
  BOOST_CHECK_EQUAL(executions, 0);
  BOOST_CHECK(provider.hasPendingRequestForTokenTest(requesterName, serviceName, requestId));

  auto selectionBuffer = makeSelectionBuffer(requestId, "provider-token");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(requesterName,
                                                                  providerName,
                                                                  serviceName,
                                                                  requestId,
                                                                  selectionBuffer);
  BOOST_CHECK_EQUAL(executions, 0);

  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         requestId,
                                         request,
                                         "provider-token");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(requesterName,
                                                                  providerName,
                                                                  serviceName,
                                                                  requestId,
                                                                  selectionBuffer);
  BOOST_CHECK_EQUAL(executions, 1);

  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(requesterName,
                                                                  providerName,
                                                                  serviceName,
                                                                  requestId,
                                                                  selectionBuffer);
  BOOST_CHECK_EQUAL(executions, 1);
}

BOOST_AUTO_TEST_CASE(AllSelectedHandlesMultipleSelectedProviderResponses)
{
  ndn::security::KeyChain keyChain("pib-memory:all-selected-responses",
                                   "tpm-memory:all-selected-responses");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name serviceName("/HELLO");
  const std::vector<ndn::Name> providers{
    ndn::Name("/test/provider/a"),
    ndn::Name("/test/provider/b")
  };
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-all-selected-responses"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  ndn::Name publishedRequestId;
  std::string userToken;
  user.setRequestPublisher(
    [&] (const ndn::Name& requestId,
         const ndn::Name&,
         const std::vector<ndn::Name>&,
         const ndn::Name&,
         const RequestMessage& requestMessage,
         size_t) {
      publishedRequestId = requestId;
      userToken = requestMessage.getUserToken();
      BOOST_CHECK(user.handleRequestAckByName(
        makeRequestAckNameV2(providers[0], requesterName, serviceName, requestId),
        makeSuccessAckForRequest(requestMessage, "provider-token-a")));
      BOOST_CHECK(user.handleRequestAckByName(
        makeRequestAckNameV2(providers[1], requesterName, serviceName, requestId),
        makeSuccessAckForRequest(requestMessage, "provider-token-b")));
    });

  int responseCallbacks = 0;
  RequestMessage request;
  auto requestId = user.RequestService(
    providers,
    serviceName,
    request,
    1,
    ServiceUser::AckSelectionStrategy::AllSelected,
    1000,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("AllSelected response test should not time out");
    }),
    ServiceUser::ResponseHandler([&] (const ResponseMessage&) {
      ++responseCallbacks;
    }));
  BOOST_CHECK_EQUAL(requestId, publishedRequestId);
  pumpFace(face, ndn::time::milliseconds(5));

  ResponseMessage responseA;
  responseA.setStatus(true);
  responseA.setUserToken(userToken);
  ResponseMessage responseB;
  responseB.setStatus(true);
  responseB.setUserToken(userToken);

  const auto selected = user.getExpectedResponseProviders(requestId);
  BOOST_REQUIRE_EQUAL(selected.size(), 2);
  BOOST_CHECK(user.handleDecryptedResponseByName(
    makeResponseNameV2(providers[0], requesterName, serviceName, requestId),
    responseA));
  BOOST_CHECK_EQUAL(responseCallbacks, 1);
  BOOST_CHECK(user.hasPendingCall(requestId));

  BOOST_CHECK(user.handleDecryptedResponseByName(
    makeResponseNameV2(providers[1], requesterName, serviceName, requestId),
    responseB));
  BOOST_CHECK_EQUAL(responseCallbacks, 2);
  BOOST_CHECK(!user.hasPendingCall(requestId));
}

BOOST_AUTO_TEST_CASE(BaseServiceProviderDefaultsAreSafe)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:generic-provider-defaults",
                                   "tpm-memory:generic-provider-defaults");
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/test/provider/default"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-default"));

  ServiceProvider provider(ServiceProvider::LocalMockTag{},
                           face,
                           ndn::Name("/test/group"),
                           providerCert,
                           aaCert,
                           "examples/trust-any.conf");

  provider.registerServiceInfo();

  BOOST_CHECK(!provider.getName().empty());
}

BOOST_AUTO_TEST_CASE(SerializedWorkerQueueRunsTasksOffCallerThreadAndSerializes)
{
  SerializedWorkerQueue queue("unit-test serialized queue", 16);
  const auto callerThread = std::this_thread::get_id();
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::thread::id> workerThreads;
  std::atomic<int> active{0};
  std::atomic<int> maxActive{0};
  int completed = 0;

  for (int i = 0; i < 8; ++i) {
    BOOST_REQUIRE(queue.post([&] {
      const int nowActive = ++active;
      int observedMax = maxActive.load();
      while (nowActive > observedMax &&
             !maxActive.compare_exchange_weak(observedMax, nowActive)) {
      }

      {
        std::lock_guard<std::mutex> lock(mutex);
        workerThreads.push_back(std::this_thread::get_id());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      --active;

      {
        std::lock_guard<std::mutex> lock(mutex);
        ++completed;
      }
      cv.notify_one();
    }));
  }

  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return completed == 8;
    }));
  }

  BOOST_CHECK_EQUAL(maxActive.load(), 1);
  BOOST_REQUIRE(!workerThreads.empty());
  BOOST_CHECK(std::all_of(workerThreads.begin(), workerThreads.end(),
                          [&] (const std::thread::id& id) {
                            return id != callerThread;
                          }));
}

BOOST_AUTO_TEST_CASE(SerializedWorkerQueueShutdownDrainsPendingWorkAndRejectsNewWork)
{
  SerializedWorkerQueue queue("unit-test serialized shutdown", 8);
  std::atomic<int> completed{0};

  for (int i = 0; i < 5; ++i) {
    BOOST_REQUIRE(queue.post([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      ++completed;
    }));
  }

  queue.shutdown();

  BOOST_CHECK_EQUAL(completed.load(), 5);
  BOOST_CHECK(!queue.post([] {}));
}

BOOST_AUTO_TEST_CASE(BoundedWorkerPoolRunsConcurrentlyAndRejectsWhenFull)
{
  BoundedWorkerPool pool("unit-test bounded pool", 8);
  pool.setThreadCount(2);
  const auto callerThread = std::this_thread::get_id();
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<int> active{0};
  std::atomic<int> maxActive{0};
  int started = 0;
  int completed = 0;

  for (int i = 0; i < 2; ++i) {
    BOOST_REQUIRE(pool.post([&] {
      {
        std::lock_guard<std::mutex> lock(mutex);
        ++started;
      }
      cv.notify_one();

      const int nowActive = ++active;
      int observedMax = maxActive.load();
      while (nowActive > observedMax &&
             !maxActive.compare_exchange_weak(observedMax, nowActive)) {
      }
      BOOST_CHECK(std::this_thread::get_id() != callerThread);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      --active;

      {
        std::lock_guard<std::mutex> lock(mutex);
        ++completed;
      }
      cv.notify_one();
    }));
  }

  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] {
      return completed == 2;
    }));
  }
  BOOST_CHECK_GE(maxActive.load(), 2);
  pool.shutdown();

  BOOST_CHECK(!pool.post([] {}));

  BoundedWorkerPool fullPool("unit-test bounded full", 1);
  fullPool.setThreadCount(1);
  std::mutex gateMutex;
  std::condition_variable gateCv;
  bool firstStarted = false;
  bool releaseFirst = false;
  BOOST_REQUIRE(fullPool.post([&] {
    {
      std::lock_guard<std::mutex> lock(gateMutex);
      firstStarted = true;
    }
    gateCv.notify_one();
    std::unique_lock<std::mutex> lock(gateMutex);
    gateCv.wait(lock, [&] { return releaseFirst; });
  }));
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    BOOST_REQUIRE(gateCv.wait_for(lock, std::chrono::seconds(1), [&] {
      return firstStarted;
    }));
  }
  BOOST_CHECK(fullPool.post([] {}));
  BOOST_CHECK(!fullPool.post([] {}));
  {
    std::lock_guard<std::mutex> lock(gateMutex);
    releaseFirst = true;
  }
  gateCv.notify_one();
  fullPool.shutdown();
}

BOOST_AUTO_TEST_CASE(ProviderHandlerExecutionCanRunOffEventLoopAndInParallel)
{
  ndn::security::KeyChain keyChain("pib-memory:provider-handler-parallel",
                                   "tpm-memory:provider-handler-parallel");
  ndn::DummyClientFace face(keyChain);
  const auto eventLoopThread = std::this_thread::get_id();
  const ndn::Name requesterName("/test/user/parallel");
  const ndn::Name providerName("/test/provider/parallel");
  const ndn::Name serviceName("/HELLO");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-provider-parallel"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.setHandlerThreads(2);
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<int> active{0};
  std::atomic<int> maxActive{0};
  int executed = 0;
  bool handlerOffEventLoop = false;
  provider.addHandler<DynamicRequest, DynamicResponse>(
    serviceName,
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [&] (const ndn::Name&, const DynamicRequest&, DynamicResponse& response) {
        handlerOffEventLoop =
          handlerOffEventLoop || std::this_thread::get_id() != eventLoopThread;
        const int nowActive = ++active;
        int observedMax = maxActive.load();
        while (nowActive > observedMax &&
               !maxActive.compare_exchange_weak(observedMax, nowActive)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        response.setClassification(7);
        --active;
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++executed;
        }
        cv.notify_one();
      }));

  const std::vector<ndn::Name> requestIds{ndn::Name("/request-parallel-a"),
                                          ndn::Name("/request-parallel-b")};
  for (size_t i = 0; i < requestIds.size(); ++i) {
    const auto& requestId = requestIds[i];
    RequestMessage request = makeRequestMessageWithUserToken(
      "hello", "user-token-parallel-" + std::to_string(i));
    const std::string providerToken = "provider-token-parallel-" + std::to_string(i);
    provider.addPendingRequestForTokenTest(requesterName,
                                           serviceName,
                                           requestId,
                                           request,
                                           providerToken);
    auto selectionBuffer = makeSelectionBuffer(requestId, providerToken);
    provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(requesterName,
                                                                    providerName,
                                                                    serviceName,
                                                                    requestId,
                                                                    selectionBuffer);
  }

  for (int i = 0; i < 20 && executed < 2; ++i) {
    pumpFace(face, ndn::time::milliseconds(10));
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return executed == 2;
    }));
  }
  pumpFace(face, ndn::time::milliseconds(50));

  BOOST_CHECK(handlerOffEventLoop);
  BOOST_CHECK_GE(maxActive.load(), 2);
  for (const auto& requestId : requestIds) {
    auto status = provider.getProviderRequestStatus(requestId);
    BOOST_REQUIRE(status.has_value());
    BOOST_CHECK(status->state ==
                ServiceProvider::ProviderRequestLifecycleState::RESPONSE_PUBLISHED);
  }
}

BOOST_AUTO_TEST_CASE(SelectionStatusQueryIsOptInPerService)
{
  ndn::security::KeyChain keyChain("pib-memory:selection-status-query",
                                   "tpm-memory:selection-status-query");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/status");
  const ndn::Name providerName("/test/provider/status");
  const ndn::Name serviceName("/TextToImage/Generate");
  const ndn::Name requestId("/request-selection-status");
  const std::string providerToken = "provider-token-selection-status";
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-selection-status"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.setHandlerThreads(0);
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  provider.addHandler<DynamicRequest, DynamicResponse>(
    serviceName,
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [] (const ndn::Name&, const DynamicRequest&, DynamicResponse& response) {
        response.setClassification(42);
      }));

  RequestMessage request = makeRequestMessageWithUserToken("prompt",
                                                           "user-token-selection-status");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         requestId,
                                         request,
                                         providerToken);

  ServiceSelectionMessage selection;
  selection.setRequestIDs({requestId.toUri()});
  selection.setProviderToken(providerToken);
  const auto selectionDigest = computeSelectionDigest(selection);
  auto selectionBlock = selection.WireEncode();
  ndn::Buffer selectionBuffer(selectionBlock.data(), selectionBlock.size());

  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(requesterName,
                                                                  providerName,
                                                                  serviceName,
                                                                  requestId,
                                                                  selectionBuffer);

  auto executionStatus = provider.getSelectionExecutionStatus(selectionDigest);
  BOOST_REQUIRE(executionStatus.has_value());
  BOOST_CHECK(executionStatus->state == SelectionExecutionState::Completed);

  const ndn::Interest query(makeSelectionStatusQueryName(providerName,
                                                        serviceName,
                                                        selectionDigest));
  BOOST_CHECK(!provider.replySelectionStatusForTest(query));

  bool receivedStatus = false;
  std::string payload;
  face.expressInterest(
    query,
    [&] (const ndn::Interest&, const ndn::Data& data) {
      receivedStatus = true;
      const auto& content = data.getContent();
      payload.assign(reinterpret_cast<const char*>(content.value()),
                     content.value_size());
    },
    [] (const ndn::Interest&, const ndn::lp::Nack&) {
      BOOST_FAIL("selection status query should not receive a Nack");
    },
    [] (const ndn::Interest&) {
      BOOST_FAIL("selection status query should not time out");
    });
  provider.setSelectionStatusQueryable(serviceName, true);
  BOOST_CHECK(provider.replySelectionStatusForTest(query));
  pumpFace(face, ndn::time::milliseconds(1));
  BOOST_REQUIRE(receivedStatus);

  BOOST_CHECK(payload.find("state=Completed") != std::string::npos);
  BOOST_CHECK(payload.find("service=" + serviceName.toUri()) != std::string::npos);
  BOOST_CHECK(payload.find("selection_digest=" + selectionDigest) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(UserResponseCallbackCanRunOffEventLoopAfterStateUpdate)
{
  ndn::security::KeyChain keyChain("pib-memory:user-callback-parallel",
                                   "tpm-memory:user-callback-parallel");
  ndn::DummyClientFace face(keyChain);
  const auto eventLoopThread = std::this_thread::get_id();
  const ndn::Name requesterName("/test/user/callback");
  const ndn::Name providerName("/test/provider/callback");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-callback");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-user-callback"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  user.setHandlerThreads(1);

  bool callbackOffEventLoop = false;
  bool pendingGoneBeforeCallback = false;
  std::mutex mutex;
  std::condition_variable cv;
  bool callbackDone = false;

  user.addPendingCallForTokenTest(requestId, serviceName, "user-token");
  user.setPendingResponseHandlerForTest(requestId, [&] (const ResponseMessage&) {
    callbackOffEventLoop = std::this_thread::get_id() != eventLoopThread;
    pendingGoneBeforeCallback = !user.hasPendingCall(requestId);
    {
      std::lock_guard<std::mutex> lock(mutex);
      callbackDone = true;
    }
    cv.notify_one();
  });

  ResponseMessage response;
  response.setStatus(true);
  response.setUserToken("user-token");
  BOOST_CHECK(user.handleDecryptedResponseByName(
    makeResponseNameV2(providerName, requesterName, serviceName, requestId),
    response));

  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return callbackDone;
    }));
  }

  BOOST_CHECK(callbackOffEventLoop);
  BOOST_CHECK(pendingGoneBeforeCallback);
}


BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
