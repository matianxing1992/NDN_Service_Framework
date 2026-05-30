#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)
BOOST_AUTO_TEST_SUITE(PreparedAndMessages)


BOOST_AUTO_TEST_CASE(PreparedServiceRequestOnlyCreatesContext)
{
  ndn::security::KeyChain keyChain("pib-memory:prepared-context",
                                   "tpm-memory:prepared-context");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name serviceName("/HELLO");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-prepared-context"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  bool published = false;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage&, size_t) {
      published = true;
    });

  const auto ctx = user.prepareServiceRequest(serviceName.toUri());
  BOOST_CHECK(!ctx.requestId.empty());
  BOOST_CHECK_EQUAL(ctx.serviceName, serviceName);
  BOOST_CHECK(!published);
  BOOST_CHECK_EQUAL(user.getPendingCallCount(), 0);
  BOOST_CHECK(!user.hasPendingCall(ctx.requestId));
}

BOOST_AUTO_TEST_CASE(PreparedRequestServicePreservesOpaquePayloadAndRequestId)
{
  ndn::security::KeyChain keyChain("pib-memory:prepared-opaque",
                                   "tpm-memory:prepared-opaque");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-prepared-opaque"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  const auto ctx = user.prepareServiceRequest(serviceName.toUri());
  const std::string payloadText =
    "{imageDataName:\"/encrypted/image\",configDataName:\"/encrypted/config\"}";
  ndn::Buffer payload(reinterpret_cast<const uint8_t*>(payloadText.data()),
                      payloadText.size());
  RequestMessage request;
  request.setPayload(payload, payload.size());

  bool published = false;
  user.setRequestPublisher(
    [&] (const ndn::Name& requestId,
         const ndn::Name& requestName,
         const std::vector<ndn::Name>& providers,
         const ndn::Name& publishedServiceName,
         const RequestMessage& requestMessage,
         size_t) {
      published = true;
      BOOST_CHECK_EQUAL(requestId, ctx.requestId);
      BOOST_REQUIRE_EQUAL(providers.size(), 1);
      BOOST_CHECK_EQUAL(providers.front(), providerName);
      BOOST_CHECK_EQUAL(publishedServiceName, serviceName);
      const auto parsed = parseRequestNameV2(requestName);
      BOOST_REQUIRE(parsed);
      BOOST_CHECK_EQUAL(parsed->requestId, ctx.requestId);
      const auto publishedPayload = requestMessage.getPayload();
      BOOST_REQUIRE_EQUAL(publishedPayload.size(), payloadText.size());
      BOOST_CHECK(std::equal(publishedPayload.begin(),
                             publishedPayload.end(),
                             payloadText.begin()));
    });

  const auto requestId = user.RequestService(
    ctx,
    {providerName},
    request,
    100,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  BOOST_CHECK_EQUAL(requestId, ctx.requestId);
  BOOST_CHECK(published);
  BOOST_CHECK(user.hasPendingCall(ctx.requestId));
  BOOST_CHECK(user.RequestService(ctx,
                              RequestMessage(),
                              100,
                              ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
                              ServiceUser::ResponseHandler([] (const ResponseMessage&) {})).empty());
}

BOOST_AUTO_TEST_CASE(AdaptiveAdmissionControlWarnsAtSoftLimitAndRejectsAtHardLimit)
{
  ndn::security::KeyChain keyChain("pib-memory:adaptive-admission-queue",
                                   "tpm-memory:adaptive-admission-queue");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/admission");
  const ndn::Name providerName("/test/provider/admission");
  const ndn::Name serviceName("/HELLO");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-admission"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  ServiceUser::AdaptiveAdmissionOptions options;
  options.enabled = true;
  options.minWindow = 1;
  options.maxWindow = 1;
  options.initialWindow = 1;
  options.hardInflightLimit = 1;
  options.softQueueLimit = 1;
  options.hardQueueLimit = 1;
  user.setAdaptiveAdmissionControl(options);

  size_t published = 0;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage&, size_t) {
      ++published;
    });

  size_t admissionWarnings = 0;
  size_t admissionRejects = 0;
  size_t lastRemainingHardSlots = 99;
  user.setAdmissionControlWarningHandler(
    [&] (const ServiceUser::AdmissionControlStatus& status) {
      lastRemainingHardSlots = status.remainingHardSlots;
      ++admissionWarnings;
    });
  user.setAdmissionControlRejectHandler(
    [&] (const ServiceUser::AdmissionControlStatus& status) {
      lastRemainingHardSlots = status.remainingHardSlots;
      ++admissionRejects;
    });

  RequestMessage request;
  const std::string payloadText = "HELLO";
  ndn::Buffer payload(reinterpret_cast<const uint8_t*>(payloadText.data()),
                      payloadText.size());
  request.setPayload(payload, payload.size());

  size_t timeoutCallbacks = 0;
  auto timeout = [&] (const ndn::Name&) { ++timeoutCallbacks; };
  auto response = [] (const ResponseMessage&) {};

  const auto first = user.RequestService({providerName}, serviceName, request, 100, timeout, response);
  const auto second = user.RequestService({providerName}, serviceName, request, 100, timeout, response);
  const auto third = user.RequestService({providerName}, serviceName, request, 100, timeout, response);

  BOOST_CHECK(!first.empty());
  BOOST_CHECK(!second.empty());
  BOOST_CHECK(!third.empty());
  BOOST_CHECK_EQUAL(published, 1);
  BOOST_CHECK_EQUAL(user.getAdaptiveAdmissionInflight(), 1);
  BOOST_CHECK_EQUAL(user.getAdaptiveAdmissionQueueDepth(), 1);
  BOOST_CHECK_EQUAL(admissionWarnings, 1);
  BOOST_CHECK_EQUAL(admissionRejects, 1);
  BOOST_CHECK_EQUAL(lastRemainingHardSlots, 0);
  BOOST_CHECK_EQUAL(timeoutCallbacks, 0);

  const auto rejectedStatus = user.getRequestStatus(third);
  BOOST_REQUIRE(rejectedStatus);
  BOOST_CHECK_EQUAL(ServiceUser::requestLifecycleStateToString(rejectedStatus->state),
                    std::string("ADMISSION_REJECTED"));
  BOOST_CHECK_EQUAL(rejectedStatus->finalCleanupReason, "admission_queue_full");
}

BOOST_AUTO_TEST_CASE(LargeDataNamePayloadRemainsOpaqueAcrossPreparedRequestService)
{
  ndn::security::KeyChain keyChain("pib-memory:large-data-name-opaque",
                                   "tpm-memory:large-data-name-opaque");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-large-data-name-opaque"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  const auto ctx = user.prepareServiceRequest(serviceName.toUri());
  const std::string encryptedDataNameUri =
    "/test/user/alice/NDNSF/LARGE-DATA/1/HELLO/" + ctx.requestId.toUri() + "/image";
  ndn::Buffer payload(reinterpret_cast<const uint8_t*>(encryptedDataNameUri.data()),
                      encryptedDataNameUri.size());
  RequestMessage request;
  request.setPayload(payload, payload.size());

  bool preparedPublished = false;
  user.setRequestPublisher(
    [&] (const ndn::Name& requestId,
         const ndn::Name&,
         const std::vector<ndn::Name>&,
         const ndn::Name&,
         const RequestMessage& requestMessage,
         size_t) {
      if (requestId != ctx.requestId) {
        return;
      }
      preparedPublished = true;
      const auto publishedPayload = requestMessage.getPayload();
      BOOST_REQUIRE_EQUAL(publishedPayload.size(), encryptedDataNameUri.size());
      BOOST_CHECK(std::equal(publishedPayload.begin(),
                             publishedPayload.end(),
                             encryptedDataNameUri.begin()));
    });

  const auto preparedRequestId = user.RequestService(
    ctx,
    {providerName},
    request,
    100,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  BOOST_CHECK_EQUAL(preparedRequestId, ctx.requestId);
  BOOST_CHECK(preparedPublished);

  RequestMessage legacyRequest;
  legacyRequest.setPayload(payload, payload.size());
  const auto legacyRequestId = user.RequestService(
    {providerName},
    serviceName,
    legacyRequest,
    100,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  BOOST_CHECK(!legacyRequestId.empty());
}

BOOST_AUTO_TEST_CASE(MultipleLargeDataObjectsUseOnePreparedRequestScope)
{
  ndn::security::KeyChain keyChain("pib-memory:large-data-multiple",
                                   "tpm-memory:large-data-multiple");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name serviceName("/HELLO");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-large-data-multiple"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  const auto ctx = user.prepareServiceRequest(serviceName.toUri());
  const std::vector<uint8_t> imageBytes = {'i', 'm', 'a', 'g', 'e'};
  const std::vector<uint8_t> configBytes = {'c', 'o', 'n', 'f', 'i', 'g'};

  const auto image = user.publishEncryptedLargeData(ctx, imageBytes, "image");
  const auto config = user.publishEncryptedLargeData(ctx, configBytes, "config");
  if (!image.success || !config.success) {
    BOOST_TEST_MESSAGE("NAC-ABE large-data production unavailable in local mock: "
                       << image.errorMessage << " " << config.errorMessage);
    BOOST_CHECK(!image.errorMessage.empty());
    BOOST_CHECK(!config.errorMessage.empty());
    return;
  }

  BOOST_CHECK(image.success);
  BOOST_CHECK(config.success);
  BOOST_CHECK(!image.encryptedDataName.get(-1).isSegment());
  BOOST_CHECK(!config.encryptedDataName.get(-1).isSegment());
  BOOST_CHECK_NE(image.encryptedDataName, config.encryptedDataName);
  BOOST_CHECK(image.encryptedDataName.toUri().find(ctx.requestId.toUri()) != std::string::npos);
  BOOST_CHECK(config.encryptedDataName.toUri().find(ctx.requestId.toUri()) != std::string::npos);
  BOOST_CHECK_NE(image.objectId, config.objectId);
  BOOST_CHECK(user.hasCachedDataForTest(image.encryptedDataName));
  BOOST_CHECK(user.hasCachedDataForTest(config.encryptedDataName));
  BOOST_CHECK_NE(user.getCachedDataContentForTest(image.encryptedDataName),
                 ndn::Buffer(imageBytes.begin(), imageBytes.end()));
  BOOST_CHECK_NE(user.getCachedDataContentForTest(config.encryptedDataName),
                 ndn::Buffer(configBytes.begin(), configBytes.end()));
}

BOOST_AUTO_TEST_CASE(MissingLargeDataFetchFailsCleanly)
{
  ndn::security::KeyChain keyChain("pib-memory:missing-large-data",
                                   "tpm-memory:missing-large-data");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/camera");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-missing-large-data"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  const auto result =
    provider.fetchAndDecryptLargeData(ndn::Name("/missing/large/data"), "/HELLO");
  BOOST_CHECK(!result.success);
  BOOST_CHECK(!result.errorMessage.empty());
}

BOOST_AUTO_TEST_CASE(V2RequestAndResponseNames)
{
  const ndn::Name requester("/test/user/alice");
  const ndn::Name provider("/test/provider/camera");
  const ndn::Name serviceName("/ObjectDetection/YOLOv8");
  const ndn::Name requestId("/request-1");

  const auto requestName = makeRequestNameV2(requester, serviceName, requestId);
  const auto parsedRequest = parseRequestNameV2(requestName);
  BOOST_REQUIRE(parsedRequest);
  BOOST_CHECK_EQUAL(parsedRequest->requesterName, requester);
  BOOST_CHECK_EQUAL(parsedRequest->serviceName, serviceName);
  BOOST_CHECK_EQUAL(parsedRequest->requestId, requestId);

  const auto responseName = makeResponseNameV2(provider, requester, serviceName, requestId);
  const auto parsedResponse = parseResponseNameV2(responseName);
  BOOST_REQUIRE(parsedResponse);
  BOOST_CHECK_EQUAL(parsedResponse->providerName, provider);
  BOOST_CHECK_EQUAL(parsedResponse->requesterName, requester);
  BOOST_CHECK_EQUAL(parsedResponse->serviceName, serviceName);
  BOOST_CHECK_EQUAL(parsedResponse->requestId, requestId);
}

BOOST_AUTO_TEST_CASE(AddHandlerRequestServiceDispatchResponseAndAck)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:generic-dynamic-api", "tpm-memory:generic-dynamic-api");
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/test/user/alice"));
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/test/provider/camera"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{},
                           face,
                           ndn::Name("/test/group"),
                           providerCert,
                           aaCert,
                           "examples/trust-any.conf");

  runLocalFlow(user, provider, ndn::Name("/ObjectDetection/YOLOv8"), "local-image-bytes", 42);
  runLocalFlow(user, provider, ndn::Name("/LLM/Llama3/Prefill"), "prompt-tokens", 7);
}


BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
