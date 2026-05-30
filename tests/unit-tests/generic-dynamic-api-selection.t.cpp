#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)
BOOST_AUTO_TEST_SUITE(SelectionStrategies)

BOOST_AUTO_TEST_CASE(LateAckAfterAckTimeoutSelectsProviderBeforeRequestTimeout)
{
  ndn::security::KeyChain keyChain("pib-memory:late-ack-selects",
                                   "tpm-memory:late-ack-selects");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-late-ack-selects"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    100,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(20));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK(user.isAckWindowExpired(requestId));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerName);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(FirstRespondingSelectsFirstAckBeforeAckTimeout)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-before-ack-timeout",
                                   "tpm-memory:first-responding-before-ack-timeout");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-before-timeout"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 100,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    500,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  auto firstAck = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    firstAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  pumpFace(face, ndn::time::milliseconds(150));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(FirstRespondingSelectsFirstAckAfterNominalAckTimeout)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-late-ack",
                                   "tpm-memory:first-responding-late-ack");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-late-ack"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    100,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(20));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK(!user.isAckWindowExpired(requestId));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerName);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(FirstRespondingIgnoresAckTimeoutCompletely)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-ignores-ack-timeout",
                                   "tpm-memory:first-responding-ignores-ack-timeout");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-ignore-ack-timeout"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    200,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK(!user.isAckWindowExpired(requestId));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());
  BOOST_CHECK(!timeoutCalled);

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerName);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(FirstRespondingLateAckAfterRequestTimeoutIsIgnored)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-late-after-timeout",
                                   "tpm-memory:first-responding-late-after-timeout");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-late-timeout"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    20,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(timeoutCalled);
  BOOST_CHECK(!user.hasPendingCall(requestId));

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK(!user.hasPendingCall(requestId));
}

BOOST_AUTO_TEST_CASE(FirstRespondingAckAfterProviderSelectedIsIgnored)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-ack-after-selected",
                                   "tpm-memory:first-responding-ack-after-selected");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-ack-after-selected"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 100,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    500,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("selected request should not time out in this unit test");
    }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  auto firstAck = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    firstAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  auto secondAck = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requesterName, serviceName, requestId),
    secondAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);
}

BOOST_AUTO_TEST_CASE(FirstRespondingV2AckCallbackDoesNotFallThroughToLegacySelection)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-v2-no-legacy-fallback",
                                   "tpm-memory:first-responding-v2-no-legacy-fallback");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-v2-no-legacy"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 100,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    500,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  BOOST_CHECK(user.hasLegacyStrategyState(requestId));

  auto firstAck = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  auto firstAckBlock = firstAck.WireEncode();
  ndn::Buffer firstAckBuffer(firstAckBlock.data(), firstAckBlock.size());
  user.OnRequestAckDecryptionSuccessCallback(providerA,
                                             serviceName,
                                             ndn::Name(),
                                             requestId,
                                             firstAckBuffer);
  pumpFace(face, 50_ms);
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK(user.hasLegacyStrategyState(requestId));

  auto secondAck = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  auto secondAckBlock = secondAck.WireEncode();
  ndn::Buffer secondAckBuffer(secondAckBlock.data(), secondAckBlock.size());
  user.OnRequestAckDecryptionSuccessCallback(providerB,
                                             serviceName,
                                             ndn::Name(),
                                             requestId,
                                             secondAckBuffer);
  pumpFace(face, 50_ms);
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);
  BOOST_CHECK(user.hasLegacyStrategyState(requestId));
}

BOOST_AUTO_TEST_CASE(LateAckAfterRequestTimeoutIsIgnored)
{
  ndn::security::KeyChain keyChain("pib-memory:late-ack-after-timeout",
                                   "tpm-memory:late-ack-after-timeout");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-late-ack-timeout"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    20,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(timeoutCalled);
  BOOST_CHECK(!user.hasPendingCall(requestId));

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK(!user.hasPendingCall(requestId));
}

BOOST_AUTO_TEST_CASE(AckAfterProviderSelectedIsIgnored)
{
  ndn::security::KeyChain keyChain("pib-memory:ack-after-selected",
                                   "tpm-memory:ack-after-selected");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-ack-after-selected"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    100,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("selected request should not time out in this unit test");
    }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(20));
  auto firstAck = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    firstAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  auto secondAck = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requesterName, serviceName, requestId),
    secondAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);
}

BOOST_AUTO_TEST_CASE(RandomSelectionMultipleAcksWithinWindowSelectsOneCandidate)
{
  ndn::security::KeyChain keyChain("pib-memory:random-selection-normal",
                                   "tpm-memory:random-selection-normal");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-random-selection-normal"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 30,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    100,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("normal ACK selection should not time out in this unit test");
    }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  auto ackA = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    ackA));
  auto ackB = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requesterName, serviceName, requestId),
    ackB));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());

  pumpFace(face, ndn::time::milliseconds(50));
  const auto selectedProvider = user.getSelectedProvider(requestId);
  BOOST_CHECK(selectedProvider.equals(providerA) || selectedProvider.equals(providerB));
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 2);
}

BOOST_AUTO_TEST_CASE(RandomSelectionIgnoresFailedAcksAndKeepsPendingForLateSuccess)
{
  ndn::security::KeyChain keyChain("pib-memory:random-selection-valid-only",
                                   "tpm-memory:random-selection-valid-only");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-random-selection-valid-only"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 30,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    100,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  auto failedAck = makeSuccessAckForRequest(publishedRequest, "");
  failedAck.setStatus(false);
  failedAck.setMessage("busy");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    failedAck));

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK(user.isAckWindowExpired(requestId));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  auto successAck = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requesterName, serviceName, requestId),
    successAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerB);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(RandomSelectionDistributionSanity)
{
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/random-distribution");
  std::vector<AckSelectionCandidate> candidates;
  for (const auto& provider : {ndn::Name("/test/provider/A"),
                               ndn::Name("/test/provider/B"),
                               ndn::Name("/test/provider/C")}) {
    AckSelectionCandidate candidate;
    candidate.providerName = provider;
    candidate.serviceName = serviceName;
    candidate.requestId = requestId;
    candidate.ack = makeSuccessAck();
    candidate.ack.setUserToken("user-token");
    candidate.ack.setProviderToken("provider-token");
    candidates.push_back(candidate);
  }

  std::map<std::string, int> selectedCounts;
  for (int i = 0; i < 200; ++i) {
    const auto selected = ServiceUser::selectRandomAck(candidates);
    BOOST_REQUIRE_EQUAL(selected.size(), 1);
    ++selectedCounts[selected.front().providerName.toUri()];
  }

  BOOST_CHECK_GE(selectedCounts.size(), 2);
}


BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
