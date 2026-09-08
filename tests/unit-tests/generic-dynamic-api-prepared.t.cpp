#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"

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
  installUserPermissions(user, requesterName, serviceName, {providerName});

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
  installUserPermissions(user, requesterName, serviceName, {providerName});

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
  installUserPermissions(user, requesterName, serviceName, {providerName});

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
  BOOST_CHECK_EQUAL(image.contentDigest.substr(0, 7), "sha256:");
  BOOST_CHECK_EQUAL(config.contentDigest.substr(0, 7), "sha256:");
  BOOST_CHECK_EQUAL(image.contentDigest.find_first_of("ABCDEF"), std::string::npos);
  BOOST_CHECK_EQUAL(config.contentDigest.find_first_of("ABCDEF"), std::string::npos);
  BOOST_CHECK(user.hasCachedDataForTest(image.encryptedDataName));
  BOOST_CHECK(user.hasCachedDataForTest(config.encryptedDataName));
  BOOST_CHECK_NE(user.getCachedDataContentForTest(image.encryptedDataName),
                 ndn::Buffer(imageBytes.begin(), imageBytes.end()));
  BOOST_CHECK_NE(user.getCachedDataContentForTest(config.encryptedDataName),
                 ndn::Buffer(configBytes.begin(), configBytes.end()));
}

BOOST_AUTO_TEST_CASE(MissingLargeDataFetchFailsCleanly)
{
  ScopedEnvironmentValue fetchTimeout("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS", "100");
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

  const auto started = std::chrono::steady_clock::now();
  const auto result =
    provider.fetchAndDecryptLargeData(ndn::Name("/missing/large/data"), "/HELLO");
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - started).count();
  BOOST_CHECK(!result.success);
  BOOST_CHECK(!result.errorMessage.empty());
  BOOST_CHECK_LT(elapsed, 1000);
}

BOOST_AUTO_TEST_CASE(LargeDataReferencePayloadRoundTrips)
{
  LargeDataReference reference;
  reference.dataName = ndn::Name("/test/user/alice/NDNSF/LARGE-DATA/HELLO/request-1/image");
  reference.objectType = "image/tensor";
  reference.objectId = "image";
  reference.keyScope = "request";
  reference.plaintextSize = 2048;
  reference.encrypted = true;
  reference.digest = "sha256:test";

  const auto payload = encodeLargeDataReferencePayload(reference);
  BOOST_CHECK(isLargeDataReferencePayload(payload));
  const auto parsed = parseLargeDataReferencePayload(payload);
  BOOST_REQUIRE(parsed);
  BOOST_CHECK_EQUAL(parsed->dataName, reference.dataName);
  BOOST_CHECK_EQUAL(parsed->objectType, reference.objectType);
  BOOST_CHECK_EQUAL(parsed->objectId, reference.objectId);
  BOOST_CHECK_EQUAL(parsed->keyScope, reference.keyScope);
  BOOST_CHECK_EQUAL(parsed->plaintextSize, reference.plaintextSize);
  BOOST_CHECK(parsed->encrypted);
  BOOST_CHECK_EQUAL(parsed->digest, reference.digest);

  // Legacy references omit key_scope and remain parseable.
  reference.keyScope.clear();
  const auto legacyPayload = encodeLargeDataReferencePayload(reference);
  const auto legacyParsed = parseLargeDataReferencePayload(legacyPayload);
  BOOST_REQUIRE(legacyParsed);
  BOOST_CHECK(legacyParsed->keyScope.empty());

  // An explicit scope that the runtime does not understand must not be
  // treated as the legacy service-wide key path.
  reference.keyScope = "unknown";
  const auto unknownScopePayload = encodeLargeDataReferencePayload(reference);
  BOOST_CHECK(!parseLargeDataReferencePayload(unknownScopePayload));
}

BOOST_AUTO_TEST_CASE(LargeDataOptimizationKeepsSmallPayloadInline)
{
  ndn::security::KeyChain keyChain("pib-memory:large-data-inline",
                                   "tpm-memory:large-data-inline");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name serviceName("/HELLO");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-large-data-inline"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  const auto ctx = user.prepareServiceRequest(serviceName.toUri());
  const std::vector<uint8_t> smallPayload = {'s', 'm', 'a', 'l', 'l'};
  const auto request = user.makeRequestWithLargeDataOptimization(
    ctx, smallPayload, "small", "text/plain", 1024);

  BOOST_REQUIRE(request.success);
  BOOST_CHECK(!request.usedLargeDataReference);
  const auto payload = request.requestMessage.getPayload();
  BOOST_REQUIRE_EQUAL(payload.size(), smallPayload.size());
  BOOST_CHECK(std::equal(payload.begin(), payload.end(), smallPayload.begin()));
  BOOST_CHECK(!isLargeDataReferencePayload(payload));
}

BOOST_AUTO_TEST_CASE(LargeDataOptimizationPublishesReferenceForLargePayload)
{
  ndn::security::KeyChain keyChain("pib-memory:large-data-reference",
                                   "tpm-memory:large-data-reference");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name serviceName("/HELLO");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-large-data-reference"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  const auto ctx = user.prepareServiceRequest(serviceName.toUri());
  const std::vector<uint8_t> largePayload(2048, static_cast<uint8_t>('x'));
  const auto request = user.makeRequestWithLargeDataOptimization(
    ctx, largePayload, "image", "application/octet-stream", 1024);
  if (!request.success) {
    BOOST_TEST_MESSAGE("NAC-ABE large-data production unavailable in local mock: "
                       << request.errorMessage);
    BOOST_CHECK(!request.errorMessage.empty());
    return;
  }

  BOOST_CHECK(request.usedLargeDataReference);
  const auto payload = request.requestMessage.getPayload();
  const auto reference = parseLargeDataReferencePayload(payload);
  BOOST_REQUIRE(reference);
  BOOST_CHECK_EQUAL(reference->dataName, request.largeData.encryptedDataName);
  BOOST_CHECK_EQUAL(reference->objectType, "application/octet-stream");
  BOOST_CHECK_EQUAL(reference->objectId, request.largeData.objectId);
  BOOST_CHECK_EQUAL(reference->plaintextSize, largePayload.size());
  BOOST_CHECK(reference->encrypted);
}

BOOST_AUTO_TEST_CASE(ProviderResolveLargeDataReferenceLeavesInlinePayload)
{
  ndn::security::KeyChain keyChain("pib-memory:large-data-provider-inline",
                                   "tpm-memory:large-data-provider-inline");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name providerName("/test/provider/camera");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-large-data-provider-inline"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  const std::vector<uint8_t> inlinePayload = {'i', 'n', 'l', 'i', 'n', 'e'};
  const ndn::Buffer payload(inlinePayload.data(), inlinePayload.size());
  const auto result = provider.resolveLargeDataReferencePayload(payload, "/HELLO");
  BOOST_REQUIRE(result.success);
  BOOST_CHECK_EQUAL_COLLECTIONS(result.plaintext.begin(), result.plaintext.end(),
                                inlinePayload.begin(), inlinePayload.end());
}

BOOST_AUTO_TEST_CASE(RequestScopedLargeResponseKeepsSmallPayloadInline)
{
  ndn::security::KeyChain keyChain("pib-memory:large-response-inline",
                                   "tpm-memory:large-response-inline");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-1");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-large-response-inline"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  const auto userOffer = makeSelectionInputKeyOffer(userCert);
  const auto providerOffer = makeSelectionInputKeyOffer(providerCert);
  RequestSecurityBinding binding;
  binding.serviceName = serviceName;
  binding.requestId = requestId;
  binding.attempt = 1;
  binding.controllerVersion = ControllerVersion{1788285600123ULL, 7};
  binding.userEncryptionCertName = userOffer.getField("recipientCertName");
  binding.userEncryptionCertDigest = userOffer.getField("recipientCertDigest");
  binding.providerEncryptionCertName = providerOffer.getField("recipientCertName");
  binding.providerEncryptionCertDigest = providerOffer.getField("recipientCertDigest");
  binding.selectionDigest = "sha256:" + std::string(64, '1');
  binding.inputDataName = ndn::Name("/test/user/alice/NDNSF/INPUT/request-1");
  binding.segmentOrEventId = "response";
  BOOST_REQUIRE(binding.isValid());

  const auto now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const auto keys = generateRequestKeyBundle(now, now + 60000);
  const std::vector<uint8_t> smallPayload = {'o', 'k'};
  ndn::Buffer payload(smallPayload.data(), smallPayload.size());
  ResponseMessage response;
  response.setStatus(true);
  response.setErrorInfo("No error");
  response.setPayload(payload, payload.size());

  auto optimized = provider.makeRequestScopedResponseWithLargeDataOptimization(
    requesterName, providerName, serviceName, requestId,
    response, keys, binding, 1024);

  BOOST_REQUIRE(optimized.success);
  BOOST_CHECK(!optimized.usedLargeDataReference);
  const auto optimizedPayload = optimized.responseMessage.getPayload();
  BOOST_CHECK_EQUAL_COLLECTIONS(optimizedPayload.begin(), optimizedPayload.end(),
                                smallPayload.begin(), smallPayload.end());
  BOOST_CHECK(!isLargeDataReferencePayload(optimizedPayload));
}


BOOST_AUTO_TEST_CASE(RequestScopedLargeResponseUsesPerSegmentAeadReference)
{
  ndn::security::KeyChain keyChain("pib-memory:request-scoped-large-response",
                                   "tpm-memory:request-scoped-large-response");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-large-1");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-request-scoped-large-response"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  // The request-scoped large-response path signs each encrypted segment with
  // the Provider certificate.  LocalMock owns a separate in-memory keychain
  // by default, so explicitly bind it to the fixture keychain before testing
  // the production publication path.
  provider.useSigningKeyChainForTest(keyChain);

  const auto userOffer = makeSelectionInputKeyOffer(userCert);
  const auto providerOffer = makeSelectionInputKeyOffer(providerCert);
  RequestSecurityBinding binding;
  binding.serviceName = serviceName;
  binding.requestId = requestId;
  binding.attempt = 1;
  binding.controllerVersion = ControllerVersion{1788285600123ULL, 7};
  binding.userEncryptionCertName = userOffer.getField("recipientCertName");
  binding.userEncryptionCertDigest = userOffer.getField("recipientCertDigest");
  binding.providerEncryptionCertName = providerOffer.getField("recipientCertName");
  binding.providerEncryptionCertDigest = providerOffer.getField("recipientCertDigest");
  binding.selectionDigest = "sha256:" + std::string(64, '1');
  binding.inputDataName = ndn::Name("/test/user/alice/NDNSF/INPUT/request-large-1");
  binding.segmentOrEventId = "response";
  BOOST_REQUIRE(binding.isValid());

  const auto now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const auto keys = generateRequestKeyBundle(now, now + 60000);
  const std::vector<uint8_t> largePayload(9000, static_cast<uint8_t>('x'));
  ndn::Buffer payload(largePayload.data(), largePayload.size());
  ResponseMessage response;
  response.setStatus(true);
  response.setPayload(payload, payload.size());

  auto optimized = provider.makeRequestScopedResponseWithLargeDataOptimization(
      requesterName, providerName, serviceName, requestId,
      response, keys, binding, 1024);
  BOOST_REQUIRE_MESSAGE(optimized.success, optimized.errorMessage);
  BOOST_REQUIRE(optimized.usedLargeDataReference);
  const auto reference = parseLargeDataReferencePayload(
      optimized.responseMessage.getPayload());
  BOOST_REQUIRE(reference);
  BOOST_CHECK_EQUAL(reference->keyScope, "request");
  BOOST_CHECK_EQUAL(reference->plaintextSize, largePayload.size());
  BOOST_CHECK_EQUAL(reference->digest, optimized.largeData.digest);
  BOOST_CHECK(optimized.responseMessage.hasControllerVersion());
  BOOST_CHECK(!optimized.responseMessage.hasAeadEnvelope());
  BOOST_CHECK_GT(optimized.largeData.encryptedDataName.size(), 0U);
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

  auto segmentedResponseName = responseName;
  segmentedResponseName.appendVersion(0).appendSegment(0);
  const auto parsedSegmentedResponse = parseResponseNameV2(segmentedResponseName);
  BOOST_REQUIRE(parsedSegmentedResponse);
  BOOST_CHECK_EQUAL(parsedSegmentedResponse->providerName, provider);
  BOOST_CHECK_EQUAL(parsedSegmentedResponse->requesterName, requester);
  BOOST_CHECK_EQUAL(parsedSegmentedResponse->serviceName, serviceName);
  BOOST_CHECK_EQUAL(parsedSegmentedResponse->requestId, requestId);

  // Native DI request identities are structured names. They must remain a
  // requestId suffix rather than being absorbed into serviceName by V2
  // parsing; the legacy one-component form above remains unchanged.
  const ndn::Name structuredRequestId("/NDNSF/DI/REQUEST/7");
  const auto structuredRequest = makeRequestNameV2(
    requester, serviceName, structuredRequestId);
  const auto parsedStructuredRequest = parseRequestNameV2(structuredRequest);
  BOOST_REQUIRE(parsedStructuredRequest);
  BOOST_CHECK_EQUAL(parsedStructuredRequest->serviceName, serviceName);
  BOOST_CHECK_EQUAL(parsedStructuredRequest->requestId, structuredRequestId);

  const auto structuredResponse = makeResponseNameV2(
    provider, requester, serviceName, structuredRequestId);
  const auto parsedStructuredResponse = parseResponseNameV2(structuredResponse);
  BOOST_REQUIRE(parsedStructuredResponse);
  BOOST_CHECK_EQUAL(parsedStructuredResponse->serviceName, serviceName);
  BOOST_CHECK_EQUAL(parsedStructuredResponse->requestId, structuredRequestId);

  const auto structuredAck = makeRequestAckNameV2(
    provider, requester, serviceName, structuredRequestId);
  const auto parsedStructuredAck = parseRequestAckNameV2(structuredAck);
  BOOST_REQUIRE(parsedStructuredAck);
  BOOST_CHECK_EQUAL(parsedStructuredAck->serviceName, serviceName);
  BOOST_CHECK_EQUAL(parsedStructuredAck->requestId, structuredRequestId);

  const auto structuredSelection = makeServiceSelectionNameV2(
    requester, provider, serviceName, structuredRequestId);
  const auto parsedStructuredSelection = parseServiceSelectionNameV2(
    structuredSelection);
  BOOST_REQUIRE(parsedStructuredSelection);
  BOOST_CHECK_EQUAL(parsedStructuredSelection->serviceName, serviceName);
  BOOST_CHECK_EQUAL(parsedStructuredSelection->requestId, structuredRequestId);

  // A plain provider-bound Selection name ends with the structured request
  // counter; it must not be mistaken for a decision attempt.
  BOOST_CHECK(!parseServiceSelectionDecisionNameV2(structuredSelection));

  const auto structuredDecision = makeServiceSelectionDecisionNameV2(
    requester, provider, serviceName, structuredRequestId, 2);
  const auto parsedStructuredDecision = parseServiceSelectionDecisionNameV2(
    structuredDecision);
  BOOST_REQUIRE(parsedStructuredDecision);
  BOOST_CHECK_EQUAL(parsedStructuredDecision->serviceName, serviceName);
  BOOST_CHECK_EQUAL(parsedStructuredDecision->requestId, structuredRequestId);
  BOOST_CHECK_EQUAL(parsedStructuredDecision->attempt, 2U);

  const auto compactSelectionName =
    makeCompactServiceSelectionNameV2(requester, serviceName, requestId);
  BOOST_CHECK(compactSelectionName.toUri().find("%2FNDNSF%2FCOMPACT") ==
              std::string::npos);
  BOOST_CHECK(!parseServiceSelectionNameV2(compactSelectionName));
  const auto parsedCompactSelection =
    parseCompactServiceSelectionNameV2(compactSelectionName);
  BOOST_REQUIRE(parsedCompactSelection);
  BOOST_CHECK_EQUAL(parsedCompactSelection->requesterName, requester);
  BOOST_CHECK_EQUAL(parsedCompactSelection->serviceName, serviceName);
  BOOST_CHECK_EQUAL(parsedCompactSelection->requestId, requestId);

  const auto compactSelectionAttributes = GetAttributesByName(compactSelectionName);
  BOOST_REQUIRE(compactSelectionAttributes);
  BOOST_REQUIRE_EQUAL(compactSelectionAttributes->size(), 1);
  BOOST_CHECK_EQUAL(compactSelectionAttributes->at(0), "/SERVICE/ObjectDetection/YOLOv8");

  const auto legacySelectionName =
    makeServiceSelectionNameV2(requester, provider, serviceName, requestId);
  const auto parsedLegacySelection = parseServiceSelectionNameV2(legacySelectionName);
  BOOST_REQUIRE(parsedLegacySelection);
  BOOST_CHECK_EQUAL(parsedLegacySelection->providerName, provider);
  BOOST_CHECK_EQUAL(parsedLegacySelection->serviceName, serviceName);
  BOOST_CHECK(!parseCompactServiceSelectionNameV2(legacySelectionName));
}

BOOST_AUTO_TEST_CASE(CollaborationNamePreservesStructuredRequestId)
{
  const ndn::Name producer("/test/provider/camera");
  const ndn::Name requester("/test/user/alice");
  const ndn::Name structuredRequestId("/NDNSF/DI/REQUEST/7");
  const ndn::Name topic("/ndnsf-di/conversation/receipt/LLM/Pipeline/Stage/0");

  const auto name = makeCollaborationDataName(
    producer, requester, structuredRequestId,
    "ndnsf-di-conversation-state-v1", topic, 3);
  const auto parsed = parseCollaborationDataName(name);
  BOOST_REQUIRE(parsed);
  BOOST_CHECK_EQUAL(parsed->producerName, producer);
  BOOST_CHECK_EQUAL(parsed->requesterName, requester);
  BOOST_CHECK_EQUAL(parsed->requestId, structuredRequestId);
  BOOST_CHECK_EQUAL(parsed->keyScope, "ndnsf-di-conversation-state-v1");
  BOOST_CHECK_EQUAL(parsed->topic, topic);
  BOOST_CHECK_EQUAL(parsed->sequence, 3U);

  // Names written by the pre-structured format remain readable.
  ndn::Name legacy(producer);
  legacy.append("NDNSF").append("COLLAB").append("3").append(requester)
    .append(structuredRequestId).append("ndnsf-di-conversation-state-v1")
    .append(std::to_string(topic.size())).append(topic).append("3");
  const auto parsedLegacy = parseCollaborationDataName(legacy);
  BOOST_REQUIRE(parsedLegacy);
  BOOST_CHECK_EQUAL(parsedLegacy->requestId, structuredRequestId);
  BOOST_CHECK_EQUAL(parsedLegacy->keyScope, "ndnsf-di-conversation-state-v1");
  BOOST_CHECK_EQUAL(parsedLegacy->topic, topic);
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

namespace {

PolicyStatusData
currentStatusFor(const ndn::Name& serviceName, uint64_t epoch)
{
  const auto now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  PolicyStatusData status;
  status.setServiceName(serviceName);
  status.setControllerVersion(ControllerVersion{now, epoch});
  status.setValidity(now - 1000, now + 120000);
  status.setPolicyDigest("sha256:" + std::string(64, '0'));
  status.setControllerCertificate(ndn::Name("/controller/spec179/compat"));
  return status;
}

} // namespace

// Spec179 T012: the temporary NDNSF_REQUEST_SCOPED_COMPATIBILITY switch and
// its counters were removed together with the old service-wide response-key
// carrier once the migration MiniNDN and streaming gates passed.  These
// assertions pin the post-migration contract: on a configured Controller
// runtime a plain V2 request is still auto-activated to the request-scoped
// path, and a stale NDNSF_REQUEST_SCOPED_COMPATIBILITY=1 environment no
// longer re-enables the removed old-path behavior.
BOOST_AUTO_TEST_CASE(RequestScopedDefaultActivationWithConfiguredController)
{
  ndn::security::KeyChain keyChain("pib-memory:compat-switch",
                                   "tpm-memory:compat-switch");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/compat");
  const ndn::Name providerName("/test/provider/compat");
  const ndn::Name serviceName("/HELLO");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-compat"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert,
                        "examples/trust-any.conf");
  installUserPermissions(user, requesterName, serviceName, {providerName});

  // The request-scoped default only exists on a configured Controller runtime.
  user.fetchPermissionsFromController(ndn::Name("/controller/test/compat"));
  BOOST_REQUIRE(user.installControllerStatus(currentStatusFor(serviceName, 1)));

  const std::string payloadText = "compat-switch-payload";
  std::optional<RequestMessage> publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  // (a) Plain V2 request: the request-scoped capability is injected by the
  // default path and the plaintext payload leaves the request.
  {
    RequestMessage request;
    ndn::Buffer payload(reinterpret_cast<const uint8_t*>(payloadText.data()),
                        payloadText.size());
    request.setPayload(payload, payload.size());
    const auto requestId = user.RequestService(
        {providerName}, serviceName, request, 500,
        ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
        ServiceUser::ResponseHandler([] (const ResponseMessage&) {}),
        tlv::FirstResponding);
    BOOST_REQUIRE(!requestId.empty());
    BOOST_REQUIRE(publishedRequest.has_value());
    BOOST_CHECK(publishedRequest->getPayload().empty());
    BOOST_REQUIRE(publishedRequest->hasRequestCapabilities());
    BOOST_CHECK_EQUAL(
        publishedRequest->getRequestCapabilities().getField(
            "RequestScopedConfidentialityV1"), "required");
    publishedRequest.reset();
  }

  // (b) A stale NDNSF_REQUEST_SCOPED_COMPATIBILITY=1 must be inert: the
  // removed old-path behavior must not resurface through the environment.
  ::setenv("NDNSF_REQUEST_SCOPED_COMPATIBILITY", "1", 1);
  {
    RequestMessage request;
    ndn::Buffer payload(reinterpret_cast<const uint8_t*>(payloadText.data()),
                        payloadText.size());
    request.setPayload(payload, payload.size());
    const auto requestId = user.RequestService(
        {providerName}, serviceName, request, 500,
        ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
        ServiceUser::ResponseHandler([] (const ResponseMessage&) {}),
        tlv::FirstResponding);
    BOOST_REQUIRE(!requestId.empty());
    BOOST_REQUIRE(publishedRequest.has_value());
    BOOST_CHECK(publishedRequest->getPayload().empty());
    BOOST_REQUIRE(publishedRequest->hasRequestCapabilities());
    BOOST_CHECK_EQUAL(
        publishedRequest->getRequestCapabilities().getField(
            "RequestScopedConfidentialityV1"), "required");
    publishedRequest.reset();
  }
  ::unsetenv("NDNSF_REQUEST_SCOPED_COMPATIBILITY");
}


BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
