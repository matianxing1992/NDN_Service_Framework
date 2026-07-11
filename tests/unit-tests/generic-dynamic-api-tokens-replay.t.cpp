#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)
BOOST_AUTO_TEST_SUITE(TokensAndReplay)

BOOST_AUTO_TEST_CASE(TokenHandshakeNegativeRegression)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:token-handshake-negative",
                                   "tpm-memory:token-handshake-negative");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-token-negative");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-token-negative"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  user.addPendingCallForTokenTest(requestId, serviceName, "user-token");

  auto ackName = makeRequestAckNameV2(providerName, requesterName, serviceName, requestId);
  auto wrongUserAck = makeSuccessAck();
  wrongUserAck.setUserToken("wrong-user-token");
  wrongUserAck.setProviderToken("provider-token");
  BOOST_CHECK(!user.handleRequestAckByName(ackName, wrongUserAck));
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 0);

  auto missingProviderTokenAck = makeSuccessAck();
  missingProviderTokenAck.setUserToken("user-token");
  BOOST_CHECK(!user.handleRequestAckByName(ackName, missingProviderTokenAck));
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 0);

  auto goodAck = makeSuccessAck();
  goodAck.setUserToken("user-token");
  goodAck.setProviderToken("provider-token");
  BOOST_CHECK(user.handleRequestAckByName(ackName, goodAck));
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  ResponseMessage wrongUserResponse;
  wrongUserResponse.setStatus(true);
  wrongUserResponse.setUserToken("wrong-user-token");
  BOOST_CHECK(!user.handleDecryptedResponse(requestId, wrongUserResponse));

  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  installPermissions(user, provider, requesterName, serviceName);

  int providerHandlerCallCount = 0;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  RequestMessage requestMessage;
  requestMessage.setUserToken("user-token");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         requestId,
                                         requestMessage,
                                         "provider-token");

  ServiceSelectionMessage wrongSelection;
  wrongSelection.setRequestIDs({requestId.toUri()});
  wrongSelection.setProviderToken("wrong-provider-token");
  auto wrongSelectionBlock = wrongSelection.WireEncode();
  ndn::Buffer wrongSelectionBuffer(wrongSelectionBlock.data(),
                                      wrongSelectionBlock.size());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    wrongSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 0);

  ServiceSelectionMessage goodSelection;
  goodSelection.setRequestIDs({requestId.toUri()});
  goodSelection.setProviderToken("provider-token");
  auto goodSelectionBlock = goodSelection.WireEncode();
  ndn::Buffer goodSelectionBuffer(goodSelectionBlock.data(),
                                     goodSelectionBlock.size());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    goodSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);

  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    goodSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);

  const ndn::Name replayedRequestId("/request-token-negative-new");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         replayedRequestId,
                                         requestMessage,
                                         "new-provider-token");
  ServiceSelectionMessage replayedOldTokenSelection;
  replayedOldTokenSelection.setRequestIDs({replayedRequestId.toUri()});
  replayedOldTokenSelection.setProviderToken("provider-token");
  auto replayedOldTokenBlock = replayedOldTokenSelection.WireEncode();
  ndn::Buffer replayedOldTokenBuffer(replayedOldTokenBlock.data(),
                                     replayedOldTokenBlock.size());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    replayedRequestId,
    replayedOldTokenBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);
}

BOOST_AUTO_TEST_CASE(CompactSelectionUsesProviderBoundTokenProof)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:compact-selection-token",
                                   "tpm-memory:compact-selection-token");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name otherProviderName("/test/provider/other");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-compact-selection");

  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-compact-selection"));

  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  int providerHandlerCallCount = 0;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  RequestMessage requestMessage;
  requestMessage.setUserToken("user-token");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         requestId,
                                         requestMessage,
                                         "provider-token");

  ServiceSelectionMessage compactSelection;
  compactSelection.setRequestIDs({requestId.toUri()});
  SelectionProviderEntry localEntry;
  localEntry.providerName = providerName;
  localEntry.providerTokenHash =
    computeSelectionProviderTokenProofHash(requesterName,
                                           providerName,
                                           serviceName,
                                           "provider-token");
  ndn::Buffer assignment(reinterpret_cast<const uint8_t*>("role=head"), 9);
  localEntry.assignmentPayload = assignment;
  compactSelection.addProviderEntry(localEntry);
  SelectionProviderEntry otherEntry;
  otherEntry.providerName = otherProviderName;
  otherEntry.providerTokenHash =
    computeSelectionProviderTokenProofHash(requesterName,
                                           otherProviderName,
                                           serviceName,
                                           "other-provider-token");
  compactSelection.addProviderEntry(otherEntry);

  auto compactBlock = compactSelection.WireEncode();
  ServiceSelectionMessage decodedCompact;
  BOOST_CHECK(decodedCompact.WireDecode(compactBlock));
  BOOST_REQUIRE_EQUAL(decodedCompact.getProviderEntries().size(), 2);
  BOOST_CHECK(decodedCompact.getProviderToken().empty());
  BOOST_CHECK(decodedCompact.getProviderEntries().front().providerName.equals(providerName));
  BOOST_CHECK_EQUAL(decodedCompact.getProviderEntries().front().providerTokenHash,
                    localEntry.providerTokenHash);

  ndn::Buffer compactBuffer(compactBlock.data(), compactBlock.size());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    compactBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);
}

BOOST_AUTO_TEST_CASE(ReplayedRuntimeMessagesOnlyTakeEffectOnce)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:runtime-replay",
                                   "tpm-memory:runtime-replay");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-replay");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-runtime-replay"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  installPermissions(user, provider, requesterName, serviceName);

  int ackHandlerCalls = 0;
  int providerExecutions = 0;
  provider.addService(
    serviceName,
    ServiceProvider::AckStrategyHandler([&] (const RequestMessage&) {
      ++ackHandlerCalls;
      ServiceProvider::AckDecision decision;
      decision.status = true;
      decision.message = "ready";
      return decision;
    }),
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerExecutions;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  RequestMessage requestMessage = makeRequestMessageWithUserToken("hello");
  auto requestBlock = requestMessage.WireEncode();
  ndn::Buffer requestBuffer(requestBlock.data(), requestBlock.size());
  provider.OnRequestDecryptionSuccessCallbackV2(requesterName,
                                                serviceName,
                                                requestId,
                                                requestBuffer);
  provider.OnRequestDecryptionSuccessCallbackV2(requesterName,
                                                serviceName,
                                                requestId,
                                                requestBuffer);
  BOOST_CHECK_EQUAL(ackHandlerCalls, 1);
  BOOST_CHECK_EQUAL(providerExecutions, 0);
  BOOST_CHECK(provider.hasPendingRequestForTokenTest(requesterName, serviceName, requestId));

  user.addPendingCallForTokenTest(requestId, serviceName, "user-token");
  user.setPendingAckCandidatesHandlerForTest(
    requestId,
    [] (const std::vector<AckSelectionCandidate>& candidates) {
      return candidates;
    });
  auto ack = makeSuccessAck();
  ack.setUserToken("user-token");
  ack.setProviderToken("provider-token");
  const auto ackName = makeRequestAckNameV2(providerName, requesterName, serviceName, requestId);
  BOOST_CHECK(user.handleRequestAckByName(ackName, ack));
  BOOST_CHECK(!user.handleRequestAckByName(ackName, ack));
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  int responseCallbacks = 0;
  user.setPendingResponseHandlerForTest(requestId, [&] (const ResponseMessage&) {
    ++responseCallbacks;
  });
  ResponseMessage response;
  response.setStatus(true);
  response.setUserToken("user-token");
  const auto responseName = makeResponseNameV2(providerName, requesterName, serviceName, requestId);
  BOOST_CHECK(user.handleDecryptedResponseByName(responseName, response));
  BOOST_CHECK(!user.handleDecryptedResponseByName(responseName, response));
  BOOST_CHECK_EQUAL(responseCallbacks, 1);
}

BOOST_AUTO_TEST_CASE(TokenModeDefaultsToEnabledAndPreservesChecks)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:token-mode-default",
                                   "tpm-memory:token-mode-default");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-token-mode-default");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-token-mode-default"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  BOOST_CHECK(user.getUseTokens());
  BOOST_CHECK(provider.getUseTokens());

  user.addPendingCallForTokenTest(requestId, serviceName, "user-token");
  auto ackName = makeRequestAckNameV2(providerName, requesterName, serviceName, requestId);
  auto missingProviderTokenAck = makeSuccessAck();
  missingProviderTokenAck.setUserToken("user-token");
  BOOST_CHECK(!user.handleRequestAckByName(ackName, missingProviderTokenAck));
}

BOOST_AUTO_TEST_CASE(TokenModeDisabledKeepsFirstRespondingAckSelectionResponsePath)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:token-mode-disabled",
                                   "tpm-memory:token-mode-disabled");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-token-mode-disabled"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  user.setUseTokens(false);
  provider.setUseTokens(false);
  BOOST_CHECK(!user.getUseTokens());
  BOOST_CHECK(!provider.getUseTokens());

  installPermissions(user, provider, requesterName, serviceName);

  int providerHandlerCallCount = 0;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  ndn::Name publishedRequestId;
  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name& requestId,
         const ndn::Name& requestName,
         const std::vector<ndn::Name>&,
         const ndn::Name&,
         const RequestMessage& requestMessage,
         size_t strategy) {
      publishedRequestId = requestId;
      publishedRequest = requestMessage;
      BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
      BOOST_CHECK(requestMessage.getUserToken().empty());

      auto requestBlock = requestMessage.WireEncode();
      ndn::Buffer requestBuffer(requestBlock.data(), requestBlock.size());
      provider.OnRequestDecryptionSuccessCallbackV2(requesterName,
                                                    serviceName,
                                                    requestId,
                                                    requestBuffer);
      BOOST_CHECK_EQUAL(providerHandlerCallCount, 0);
      BOOST_CHECK(provider.hasPendingRequestForTokenTest(requesterName, serviceName, requestId));
      BOOST_CHECK(!provider.hasPendingProviderTokenForTest(requesterName, serviceName, requestId));

      auto ack = makeSuccessAck();
      BOOST_CHECK(ack.getUserToken().empty());
      BOOST_CHECK(ack.getProviderToken().empty());
      BOOST_CHECK(user.handleRequestAckByName(
        makeRequestAckNameV2(providerName, requesterName, serviceName, requestId), ack));
      BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerName);
      BOOST_CHECK(!requestName.empty());
    });

  int responseCallbacks = 0;
  RequestMessage request;
  const auto requestId = user.RequestService(
    {providerName},
    serviceName,
    request,
    1000,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("token-disabled FirstResponding request should not time out");
    }),
    ServiceUser::ResponseHandler([&] (const ResponseMessage& response) {
      BOOST_CHECK(response.getUserToken().empty());
      ++responseCallbacks;
    }),
    tlv::FirstResponding);

  BOOST_CHECK_EQUAL(requestId, publishedRequestId);
  auto selectionBuffer = makeSelectionBuffer(requestId, "");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(requesterName,
                                                                  providerName,
                                                                  serviceName,
                                                                  requestId,
                                                                  selectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);

  ResponseMessage response;
  response.setStatus(true);
  BOOST_CHECK(user.handleDecryptedResponseByName(
    makeResponseNameV2(providerName, requesterName, serviceName, requestId),
    response));
  BOOST_CHECK_EQUAL(responseCallbacks, 1);
  BOOST_CHECK(!user.hasPendingCall(requestId));
  BOOST_CHECK(publishedRequest.getUserToken().empty());
}

BOOST_AUTO_TEST_CASE(TokenModeMismatchFailsClearly)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:token-mode-mismatch",
                                   "tpm-memory:token-mode-mismatch");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-token-mode-mismatch");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-token-mode-mismatch"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  user.setUseTokens(false);
  installPermissions(user, provider, requesterName, serviceName);

  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [] (const ndn::Name&,
          const ndn::Name&,
          const ndn::Name&,
          const ndn::Name&,
          const RequestMessage&) {
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  RequestMessage tokenlessRequest = makeRequestMessageWithUserToken("payload", "");
  auto requestBlock = tokenlessRequest.WireEncode();
  ndn::Buffer requestBuffer(requestBlock.data(), requestBlock.size());
  provider.OnRequestDecryptionSuccessCallbackV2(requesterName,
                                                serviceName,
                                                requestId,
                                                requestBuffer);
  BOOST_CHECK(!provider.hasPendingRequestForTokenTest(requesterName, serviceName, requestId));

  LocalServiceUser secureUser(face,
                              ndn::Name("/test/group"),
                              userCert,
                              aaCert,
                              "examples/trust-any.conf");
  secureUser.addPendingCallForTokenTest(requestId, serviceName, "user-token");
  auto tokenlessAck = makeSuccessAck();
  BOOST_CHECK(!secureUser.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    tokenlessAck));
}

BOOST_AUTO_TEST_CASE(ProviderTokenCleanupExpiresUnselectedAckState)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:provider-token-cleanup",
                                   "tpm-memory:provider-token-cleanup");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-token-cleanup");

  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-provider-token-cleanup"));

  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  int providerHandlerCallCount = 0;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  RequestMessage requestMessage;
  requestMessage.setUserToken("user-token");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         requestId,
                                         requestMessage,
                                         "provider-token");

  BOOST_CHECK(provider.hasPendingRequestForTokenTest(requesterName, serviceName, requestId));
  BOOST_CHECK(provider.hasPendingProviderTokenForTest(requesterName, serviceName, requestId));

  provider.cleanupPendingRequestStateForTest(requesterName, serviceName, requestId);

  BOOST_CHECK(!provider.hasPendingRequestForTokenTest(requesterName, serviceName, requestId));
  BOOST_CHECK(!provider.hasPendingProviderTokenForTest(requesterName, serviceName, requestId));

  ServiceSelectionMessage staleSelection;
  staleSelection.setRequestIDs({requestId.toUri()});
  staleSelection.setProviderToken("provider-token");
  auto staleSelectionBlock = staleSelection.WireEncode();
  ndn::Buffer staleSelectionBuffer(staleSelectionBlock.data(),
                                      staleSelectionBlock.size());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    staleSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 0);

  const ndn::Name consumedRequestId("/request-token-cleanup-consumed");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         consumedRequestId,
                                         requestMessage,
                                         "fresh-provider-token");
  ServiceSelectionMessage goodSelection;
  goodSelection.setRequestIDs({consumedRequestId.toUri()});
  goodSelection.setProviderToken("fresh-provider-token");
  auto goodSelectionBlock = goodSelection.WireEncode();
  ndn::Buffer goodSelectionBuffer(goodSelectionBlock.data(),
                                     goodSelectionBlock.size());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    consumedRequestId,
    goodSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);

  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    consumedRequestId,
    goodSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);
}

BOOST_AUTO_TEST_CASE(ProviderTokenAdversarialRejectionAndRestartBehavior)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:provider-token-adversarial",
                                   "tpm-memory:provider-token-adversarial");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-token-adversarial");

  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-provider-token-adversarial"));

  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  int providerHandlerCallCount = 0;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  RequestMessage requestMessage;
  requestMessage.setUserToken("user-token");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         requestId,
                                         requestMessage,
                                         "expected-token");

  auto randomTokenBuffer = makeSelectionBuffer(requestId, "random-token");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    randomTokenBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 0);
  BOOST_CHECK_EQUAL(provider.getTokenConsumeCountForTesting(), 0);
  BOOST_CHECK(provider.hasPendingProviderTokenForTest(requesterName, serviceName, requestId));

  BOOST_CHECK(provider.expirePendingRequestStateForTest(requesterName, serviceName, requestId));
  BOOST_CHECK_EQUAL(provider.getCleanupInvocationCountForTesting(), 1);

  auto expiredTokenBuffer = makeSelectionBuffer(requestId, "expected-token");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    expiredTokenBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 0);
  BOOST_CHECK_EQUAL(provider.getTokenConsumeCountForTesting(), 0);

  LocalServiceProvider restartedProvider(face,
                                         ndn::Name("/test/group"),
                                         providerCert,
                                         aaCert,
                                         "examples/trust-any.conf");
  restartedProvider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));
  restartedProvider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    requestId,
    expiredTokenBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 0);
  BOOST_CHECK_EQUAL(restartedProvider.getPendingRequestCountForTesting(), 0);
  BOOST_CHECK_EQUAL(restartedProvider.getPendingProviderTokenCountForTesting(), 0);
}

BOOST_AUTO_TEST_CASE(ProviderStateCleanupEdgeCases)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:provider-cleanup-edge",
                                   "tpm-memory:provider-cleanup-edge");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-provider-cleanup-edge"));

  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  int providerHandlerCallCount = 0;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  RequestMessage requestMessage;
  requestMessage.setUserToken("user-token");

  for (int i = 0; i < 25; ++i) {
    provider.addPendingRequestForTokenTest(requesterName,
                                           serviceName,
                                           ndn::Name("/expire-" + std::to_string(i)),
                                           requestMessage,
                                           "token-" + std::to_string(i));
  }
  BOOST_CHECK_EQUAL(provider.getPendingRequestCountForTesting(), 25);
  BOOST_CHECK_EQUAL(provider.getPendingProviderTokenCountForTesting(), 25);

  for (int i = 0; i < 25; ++i) {
    BOOST_CHECK(provider.expirePendingRequestStateForTest(
      requesterName,
      serviceName,
      ndn::Name("/expire-" + std::to_string(i))));
  }
  BOOST_CHECK_EQUAL(provider.getPendingRequestCountForTesting(), 0);
  BOOST_CHECK_EQUAL(provider.getPendingProviderTokenCountForTesting(), 0);
  BOOST_CHECK_EQUAL(provider.getCleanupInvocationCountForTesting(), 25);

  const ndn::Name selectedRequestId("/cleanup-during-success");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         selectedRequestId,
                                         requestMessage,
                                         "live-token");
  auto liveSelectionBuffer = makeSelectionBuffer(selectedRequestId, "live-token");
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName,
    providerName,
    serviceName,
    selectedRequestId,
    liveSelectionBuffer);
  BOOST_CHECK_EQUAL(providerHandlerCallCount, 1);
  BOOST_CHECK_EQUAL(provider.getTokenConsumeCountForTesting(), 1);
  BOOST_CHECK_EQUAL(provider.getPendingRequestCountForTesting(), 0);
  BOOST_CHECK_EQUAL(provider.getPendingProviderTokenCountForTesting(), 0);
  BOOST_CHECK(!provider.expirePendingRequestStateForTest(
    requesterName,
    serviceName,
    selectedRequestId));
  BOOST_CHECK_EQUAL(provider.getCleanupInvocationCountForTesting(), 26);

  for (int cycle = 0; cycle < 5; ++cycle) {
    for (int i = 0; i < 20; ++i) {
      provider.addPendingRequestForTokenTest(requesterName,
                                             serviceName,
                                             ndn::Name("/cycle-" + std::to_string(cycle) +
                                                       "-" + std::to_string(i)),
                                             requestMessage,
                                             "cycle-token");
    }
    for (int i = 0; i < 20; ++i) {
      provider.expirePendingRequestStateForTest(
        requesterName,
        serviceName,
        ndn::Name("/cycle-" + std::to_string(cycle) + "-" + std::to_string(i)));
    }
    BOOST_CHECK_EQUAL(provider.getPendingRequestCountForTesting(), 0);
    BOOST_CHECK_EQUAL(provider.getPendingProviderTokenCountForTesting(), 0);
  }
}

BOOST_AUTO_TEST_CASE(ProviderStateLightweightStressIsDeterministic)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:provider-state-stress",
                                   "tpm-memory:provider-state-stress");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-provider-state-stress"));

  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");

  int providerHandlerCallCount = 0;
  provider.addService(
    serviceName,
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++providerHandlerCallCount;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  RequestMessage requestMessage;
  requestMessage.setUserToken("user-token");

  constexpr int requestCount = 250;
  std::vector<int> order;
  for (int i = 0; i < requestCount; ++i) {
    order.push_back(i);
    provider.addPendingRequestForTokenTest(requesterName,
                                           serviceName,
                                           ndn::Name("/stress-" + std::to_string(i)),
                                           requestMessage,
                                           "stress-token-" + std::to_string(i));
  }

  std::mt19937 rng(1337);
  std::shuffle(order.begin(), order.end(), rng);

  int expectedCompletions = 0;
  int expectedExpirations = 0;
  for (int i : order) {
    const ndn::Name requestId("/stress-" + std::to_string(i));
    if (i % 5 == 0) {
      auto wrongBuffer = makeSelectionBuffer(requestId, "wrong-token");
      provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
        requesterName,
        providerName,
        serviceName,
        requestId,
        wrongBuffer);
    }

    if (i % 2 == 0) {
      BOOST_CHECK(provider.expirePendingRequestStateForTest(requesterName,
                                                            serviceName,
                                                            requestId));
      ++expectedExpirations;
      auto expiredBuffer = makeSelectionBuffer(requestId,
                                                  "stress-token-" + std::to_string(i));
      provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
        requesterName,
        providerName,
        serviceName,
        requestId,
        expiredBuffer);
      continue;
    }

    auto buffer = makeSelectionBuffer(requestId, "stress-token-" + std::to_string(i));
    provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
      requesterName,
      providerName,
      serviceName,
      requestId,
      buffer);
    ++expectedCompletions;
    provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
      requesterName,
      providerName,
      serviceName,
      requestId,
      buffer);
  }

  BOOST_CHECK_EQUAL(providerHandlerCallCount, expectedCompletions);
  BOOST_CHECK_EQUAL(provider.getTokenConsumeCountForTesting(), expectedCompletions);
  BOOST_CHECK_EQUAL(provider.getCleanupInvocationCountForTesting(),
                    expectedCompletions + expectedExpirations);
  BOOST_CHECK_EQUAL(provider.getPendingRequestCountForTesting(), 0);
BOOST_CHECK_EQUAL(provider.getPendingProviderTokenCountForTesting(), 0);
}


BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
