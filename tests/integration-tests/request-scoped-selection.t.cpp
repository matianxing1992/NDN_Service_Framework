#include "tests/boost-test.hpp"

#include "ndnsf-integration-fixture.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include "ndn-service-framework/utils.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ndn_service_framework::integration_test {
namespace {

uint64_t
nowMilliseconds()
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

PolicyStatusData
makeCurrentStatus(const ndn::Name& serviceName, uint64_t epoch)
{
  const auto now = nowMilliseconds();
  PolicyStatusData status;
  status.setServiceName(serviceName);
  status.setControllerVersion(ControllerVersion{now, epoch});
  status.setValidity(now - 1000, now + 120000);
  status.setPolicyDigest("sha256:" + std::string(64, '0'));
  status.setControllerCertificate(ndn::Name("/controller/spec179/test"));
  return status;
}

void
publish(ndn::svs::SVSPubSub& pubSub, const ndn::Name& name,
        const ndn::Buffer& wire)
{
  pubSub.publish(name, ndn::span<const uint8_t>(wire.data(), wire.size()));
}

} // namespace

BOOST_AUTO_TEST_SUITE(RequestScopedSelection)

void
runRequestScopedResponseCase(bool useLargeResponse, bool tamperResponse,
                             bool revokeBeforeResponse,
                             std::size_t inputBytes = 0)
{
  test::BootstrapProfile profile;
  profile.providerCount = 2;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  auto scope = environment.beginRequest("request-scoped-selection");

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  std::vector<ndn::Name> providers;
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    providers.push_back(environment.provider(index).getName());
  }

  const auto status = makeCurrentStatus(serviceName, 1);
  BOOST_REQUIRE(environment.user().installControllerStatus(status));
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    BOOST_REQUIRE(environment.provider(index).installControllerStatus(status));
    environment.provider(index).setUseTokens(false);
  }
  environment.user().setUseTokens(false);
  std::vector<std::atomic<size_t>> requestExecutions(environment.providerCount());
  std::vector<std::atomic<bool>> requestObserved(environment.providerCount());
  std::atomic<bool> responseReceived{false};
  std::atomic<bool> requestTimedOut{false};
  std::atomic<bool> selectionSeen{false};
  std::atomic<bool> discoveryPayloadWasEmpty{false};
  std::atomic<bool> inputDataWasOpaque{false};
  std::atomic<bool> responseWasOpaque{false};
  std::atomic<bool> unselectedProviderExecuted{false};
  std::atomic<bool> wrongProviderRejected{false};
  std::atomic<bool> wrongRecipientRejected{false};
  std::atomic<bool> wrongSignerRejected{false};
  std::atomic<bool> mismatchedVersionRejected{false};
  std::atomic<bool> tamperedResponseRejected{false};
  std::atomic<bool> revocationResponseRejected{false};
  std::atomic<bool> selectionReplayRejected{false};
  std::atomic<std::size_t> inputPrefixInterests{0};
  std::atomic<std::size_t> inputExactSegmentInterests{0};
  std::atomic<std::size_t> inputSegmentsDelivered{0};
  std::optional<ndn::Block> capturedResponseBlock;
  ndn::Name capturedResponseName;
  ndn::Name capturedSelectionName;
  ndn::Buffer capturedSelectionWire;
  const std::string inputText = inputBytes == 0
      ? "request-scoped-secret-input"
      : std::string(inputBytes, 'i');
  // Force the production request-scoped large-response path.  The User must
  // receive only the compact reference and reconstruct the plaintext from
  // per-segment AEAD envelopes fetched from the Provider IMS.
  const std::string responseText = useLargeResponse ?
      std::string(9000, 'r') : "request-scoped-inline-response";

  for (size_t index = 0; index < environment.providerCount(); ++index) {
    environment.provider(index).addService(
        serviceName,
        ServiceProvider::RequestHandler(
            [&, index] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
                        const ndn::Name&, const RequestMessage& request) {
              ++requestExecutions[index];
              if (index != 0) {
                unselectedProviderExecuted = true;
              }
              const auto actualPayload = request.getPayload();
              if (inputBytes > 4096) {
                const auto expectedSegments = (inputBytes + 4096 - 1) / 4096;
                BOOST_CHECK_GE(inputSegmentsDelivered.load(), expectedSegments);
              }
              BOOST_CHECK_EQUAL_COLLECTIONS(
                  actualPayload.begin(), actualPayload.end(),
                  inputText.begin(), inputText.end());
              ResponseMessage response;
              response.setStatus(true);
              ndn::Buffer payload(
                  reinterpret_cast<const uint8_t*>(responseText.data()),
                  responseText.size());
              response.setPayload(payload, payload.size());
              return response;
            }));

    const auto providerNode = [&] {
      auto node = environment.profile().providerNode;
      if (index != 0) {
        node.append("p" + std::to_string(index));
      }
      return node;
    }();
    environment.providerPubSub(index).subscribeToProducer(
        environment.profile().userNode,
        [&, index] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
          if (const auto parsed = parseRequestNameV2(publication.name)) {
            if (!parsed->serviceName.equals(serviceName)) {
              return;
            }
            RequestMessage request;
            ndn::Block block(publication.data);
            BOOST_REQUIRE(request.WireDecode(block));
            requestObserved[index] = true;
            discoveryPayloadWasEmpty = discoveryPayloadWasEmpty ||
                                       request.getPayload().empty();
            const auto wire = request.WireEncode();
            environment.provider(index).OnRequestDecryptionSuccessCallbackV2(
                parsed->requesterName, parsed->serviceName, parsed->requestId,
                ndn::Buffer(wire.data(), wire.size()));
            return;
          }

          const auto parsedSelection = parseServiceSelectionNameV2(publication.name);
          if (!parsedSelection || !parsedSelection->serviceName.equals(serviceName) ||
              !parsedSelection->providerName.equals(environment.provider(index).getName())) {
            return;
          }
          selectionSeen = true;
          ndn::Block block(publication.data);
          const auto wire = ndn::Buffer(block.data(), block.size());
          if (index == 0 && capturedSelectionWire.empty()) {
            capturedSelectionName = publication.name;
            capturedSelectionWire = wire;
          }
          environment.provider(index).OnServiceSelectionMessageDecryptionSuccessCallbackV2(
              parsedSelection->requesterName, parsedSelection->providerName,
              parsedSelection->serviceName, parsedSelection->requestId, wire);
        },
        true);

    environment.provider(index).setLocalPublicationHandler(
        [&, index] (const ndn::Name& name, const ndn::Buffer& wire) {
          if (parseRequestAckNameV2(name) || parseResponseNameV2(name)) {
            publish(environment.providerPubSub(index), name, wire);
          }
        });

    // Provider responses are inspected at the publication boundary before the
    // User decrypts them.  The request-scoped path must carry no plaintext
    // payload and must contain an AEAD envelope.
    environment.userPubSub().subscribeToProducer(
        providerNode,
        [&, index] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
          if (const auto parsedAck = parseRequestAckNameV2(publication.name)) {
            if (parsedAck->serviceName.equals(serviceName)) {
              ndn::Block ackBlock(publication.data);
              environment.user().handleRequestAckByName(publication.name, ackBlock);
            }
            return;
          }
          const auto parsedResponse = parseResponseNameV2(publication.name);
          if (!parsedResponse || !parsedResponse->serviceName.equals(serviceName)) {
            return;
          }
          ndn::Block responseBlock(publication.data);
          if (index == 0) {
            capturedResponseBlock = responseBlock;
            capturedResponseName = publication.name;
          }
          ResponseMessage response;
          const bool responseDecoded = response.WireDecode(responseBlock);
          if (responseDecoded) {
            const auto reference = parseLargeDataReferencePayload(response.getPayload());
            responseWasOpaque = (response.hasAeadEnvelope() &&
                                 response.getPayload().empty()) ||
                                (reference && reference->keyScope == "request");
          }
          if (index == 0 && !wrongProviderRejected.exchange(true)) {
            const auto wrongProviderName = makeResponseNameV2(
                environment.provider(1).getName(), requesterName,
                serviceName, parsedResponse->requestId);
            BOOST_CHECK(!environment.user().handleDecryptedResponseByName(
                wrongProviderName, responseBlock));
          }
          if (index == 0 && !wrongRecipientRejected.exchange(true)) {
            // A valid ciphertext must still be rejected when the Response
            // name targets a different User.  This is the request-scoped
            // recipient boundary, independent of the service-wide ABE key.
            const auto wrongRecipientName = makeResponseNameV2(
                environment.provider(0).getName(), ndn::Name("/other-user"),
                serviceName, parsedResponse->requestId);
            BOOST_CHECK(!environment.user().handleDecryptedResponseByName(
                wrongRecipientName, responseBlock));
          }
          if (index == 0 && !wrongSignerRejected.exchange(true)) {
            ResponseMessage wrongSigner(response);
            wrongSigner.setAuthenticatedTransportEvidence(
                publication.name.toUri(),
                environment.provider(1).getSigningCertificateName().toUri(),
                "sha256:" + std::string(64, '1'));
            // The ciphertext may be valid and the packet may have arrived
            // under the expected response name, but authenticated transport
            // evidence from another Provider must not be accepted as the
            // selected Provider's response.
            BOOST_CHECK(!environment.user().handleDecryptedResponseByName(
                publication.name, wrongSigner));
          }
          if (index == 0 && !mismatchedVersionRejected.exchange(true)) {
            ResponseMessage mismatchedVersion;
            BOOST_REQUIRE(mismatchedVersion.WireDecode(responseBlock));
            const auto currentVersion = status.getControllerVersion();
            mismatchedVersion.setControllerVersion(ControllerVersion{
                currentVersion.controllerGenerationTimestamp,
                currentVersion.controllerEpoch + 1});
            const auto mismatchedWire = mismatchedVersion.WireEncode();
            BOOST_CHECK(!environment.user().handleDecryptedResponseByName(
                publication.name, mismatchedWire));
          }
          if (index == 0 && tamperResponse &&
              !tamperedResponseRejected.exchange(true)) {
            ResponseMessage tamperedResponseMessage;
            BOOST_REQUIRE(tamperedResponseMessage.WireDecode(responseBlock));
            // The short-response variant carries the result directly in an
            // AEAD envelope.  Flip ciphertext after publication and verify
            // that authentication fails before delivery or nonce commit.
            BOOST_REQUIRE(tamperedResponseMessage.hasAeadEnvelope());
            AeadEnvelope envelope;
            BOOST_REQUIRE(envelope.wireDecode(
                tamperedResponseMessage.getAeadEnvelope()));
            BOOST_REQUIRE(!envelope.ciphertext.empty());
            envelope.ciphertext[0] ^= 0x01;
            tamperedResponseMessage.setAeadEnvelope(envelope.wireEncode());
            const auto tamperedWire = tamperedResponseMessage.WireEncode();
            BOOST_CHECK(!environment.user().handleDecryptedResponseByName(
                publication.name, tamperedWire));
          }
          if (index == 0 && revokeBeforeResponse &&
              !revocationResponseRejected.exchange(true)) {
            auto revokedStatus = status;
            auto revokedVersion = status.getControllerVersion();
            ++revokedVersion.controllerEpoch;
            revokedStatus.setControllerVersion(revokedVersion);
            RevocationTarget revokedUser;
            revokedUser.kind = RevocationKind::IDENTITY;
            revokedUser.targetIdentity = ndn::Name(requesterName);
            revokedStatus.addRevocation(revokedUser);
            BOOST_REQUIRE(environment.user().installControllerStatus(revokedStatus));
            // The Provider already executed before this cut point.  Once the
            // User accepts the newer revoking status, response delivery must
            // fail closed and the valid ciphertext must not reach the app.
            BOOST_CHECK(!environment.user().handleDecryptedResponseByName(
                publication.name, responseBlock));
            return;
          }
          responseReceived = environment.user().handleDecryptedResponseByName(
              publication.name, responseBlock) || responseReceived;
        },
        true);
  }

  // Request/ACK/Selection are deliberately driven through the explicit SVS
  // callbacks above.  The large-response reference is fetched as ordinary
  // exact-name Data, so install only the Provider content filters (without
  // attaching PubSub, which would bypass the local publication callback).
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    environment.provider(index).installLocalMockDataIngressForTest();
  }

  environment.user().setLocalPublicationHandler(
      [&] (const ndn::Name& name, const ndn::Buffer& wire) {
        if (parseServiceSelectionNameV2(name)) {
          publish(environment.userPubSub(), name, wire);
        }
      });

  // Exact-name input Data is put on the User face by publishSignedAppData;
  // capture it before the fixture forwards it to the Provider.
  std::optional<ndn::Data> capturedInputData;
  std::vector<ndn::Data> capturedInputSegments;
  std::size_t largestInputDataWire = 0;
  auto inputObserver = environment.userFace().onSendData.connect(
      [&] (const ndn::Data& data) {
        if (data.getName().toUri().find("REQUEST-INPUT") == std::string::npos) {
          return;
        }
        AeadEnvelope envelope;
        bool decoded = false;
        try {
          const auto content = data.getContent();
          auto [ok, block] = ndn::Block::fromBuffer(
              ndn::span<const uint8_t>(content.value(), content.value_size()));
          decoded = ok && envelope.wireDecode(block);
        }
        catch (const std::exception&) {
          decoded = false;
        }
        inputDataWasOpaque = decoded && !envelope.ciphertext.empty();
        if (decoded) {
          largestInputDataWire = std::max(largestInputDataWire,
                                          data.wireEncode().size());
          if (data.getName().size() > 0 &&
              data.getName().at(-1).isSegment()) {
            capturedInputSegments.push_back(data);
          }
          else {
            capturedInputData = data;
          }
        }
      });

  // publishSignedAppData() legitimately sends the exact-name input before the
  // selected Provider starts its Interest fetch.  DummyClientFace does not
  // retain unsolicited Data, so replay the captured packet when the exact
  // fetch Interest is expressed.  This keeps the test on the real
  // User-face -> Provider-face path instead of bypassing the fetch callback.
  std::vector<ndn::signal::ScopedConnection> inputFetchRelays;
  inputFetchRelays.reserve(environment.providerCount());
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    inputFetchRelays.emplace_back(environment.providerFace(index).onSendInterest.connect(
        [&, index] (const ndn::Interest& interest) {
          if (!capturedInputData || interest.getName() != capturedInputData->getName()) {
            if (capturedInputSegments.empty()) {
              return;
            }
            const auto baseName = capturedInputSegments.front().getName().getPrefix(-1);
            if (interest.getName() == baseName) {
              ++inputPrefixInterests;
              auto segmentZero = std::find_if(
                  capturedInputSegments.begin(), capturedInputSegments.end(),
                  [&] (const ndn::Data& data) {
                    return data.getName().get(-1).isSegment() &&
                           data.getName().get(-1).toSegment() == 0;
                  });
              if (segmentZero != capturedInputSegments.end()) {
                ++inputSegmentsDelivered;
                environment.providerFace(index).receive(*segmentZero);
              }
              return;
            }
            if (!baseName.isPrefixOf(interest.getName()) ||
                interest.getName().size() != baseName.size() + 1 ||
                !interest.getName().at(-1).isSegment()) {
              return;
            }
            auto segment = std::find_if(
                capturedInputSegments.begin(), capturedInputSegments.end(),
                [&] (const ndn::Data& data) {
                  return data.getName() == interest.getName();
                });
            if (segment != capturedInputSegments.end()) {
              ++inputExactSegmentInterests;
              ++inputSegmentsDelivered;
              environment.providerFace(index).receive(*segment);
            }
            return;
          }
          environment.providerFace(index).receive(*capturedInputData);
        }));
  }

  environment.user().setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& requestName,
           const std::vector<ndn::Name>& selectedProviders,
           const ndn::Name& publishedService,
           const RequestMessage& request, size_t strategy) {
        BOOST_REQUIRE_EQUAL(selectedProviders.size(), providers.size());
        BOOST_CHECK_EQUAL(publishedService, serviceName);
        BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
        BOOST_CHECK(request.getPayload().empty());
        const auto wire = request.WireEncode();
        publish(environment.userPubSub(), requestName,
                ndn::Buffer(wire.data(), wire.size()));
        environment.markRequestPublished(scope);
      });

  RequestCapabilities capabilities;
  capabilities.setField("RequestScopedConfidentialityV1", "required");
  RequestMessage request;
  request.setRequestCapabilities(capabilities);
  ndn::Buffer input(reinterpret_cast<const uint8_t*>(inputText.data()), inputText.size());
  request.setPayload(input, input.size());
  const int responseTimeoutMs = revokeBeforeResponse ? 100 :
      (inputBytes > 4096 ? 30000 : 5000);
  const auto requestId = environment.user().RequestService(
      // SegmentFetcher must retrieve and validate the per-segment response
      // after the compact Response reference arrives; keep this integration
      // deadline separate from the one-second ACK attempt budget.
      providers, serviceName, request, responseTimeoutMs,
      [&] (const ndn::Name&) { requestTimedOut = true; },
      [&] (const ResponseMessage& response) {
        const auto payload = response.getPayload();
        responseReceived = response.getStatus() &&
                           std::string(reinterpret_cast<const char*>(payload.data()),
                                       payload.size()) == responseText;
      },
      tlv::FirstResponding);
  BOOST_REQUIRE(!requestId.empty());

  environment.pumpUntil([&] {
    return responseReceived || requestTimedOut;
  });

  if (revokeBeforeResponse) {
    BOOST_CHECK(revocationResponseRejected);
    BOOST_CHECK(requestTimedOut);
    BOOST_CHECK(!responseReceived);
  }
  else {
    BOOST_CHECK(!requestTimedOut);
  }
  BOOST_CHECK(selectionSeen);
  BOOST_CHECK(discoveryPayloadWasEmpty);
  BOOST_CHECK(inputDataWasOpaque);
  BOOST_CHECK(responseWasOpaque);
  if (inputBytes > 4096) {
    const auto expectedSegments = (inputBytes + 4096 - 1) / 4096;
    std::set<std::uint64_t> segmentNumbers;
    for (const auto& data : capturedInputSegments) {
      BOOST_REQUIRE(data.getName().at(-1).isSegment());
      segmentNumbers.insert(data.getName().at(-1).toSegment());
      BOOST_REQUIRE(data.getFinalBlock());
      BOOST_REQUIRE(data.getFinalBlock()->isSegment());
      BOOST_CHECK_EQUAL(data.getFinalBlock()->toSegment(), expectedSegments - 1);
    }
    BOOST_CHECK_EQUAL(capturedInputSegments.size(), expectedSegments);
    BOOST_CHECK_EQUAL(segmentNumbers.size(), expectedSegments);
    BOOST_CHECK(!capturedInputData.has_value());
    BOOST_CHECK_LE(largestInputDataWire, 8800U);
    BOOST_CHECK_GE(inputPrefixInterests.load(), 1U);
    BOOST_CHECK_GE(inputExactSegmentInterests.load(), expectedSegments - 1);
    BOOST_CHECK_GE(inputSegmentsDelivered.load(), expectedSegments);
  }
  BOOST_CHECK(wrongProviderRejected);
  BOOST_CHECK(wrongRecipientRejected);
  BOOST_CHECK(wrongSignerRejected);
  BOOST_CHECK(mismatchedVersionRejected);
  if (tamperResponse) {
    BOOST_CHECK(tamperedResponseRejected);
  }
  BOOST_REQUIRE(!capturedResponseName.empty());
  BOOST_REQUIRE(capturedResponseBlock.has_value());
  BOOST_REQUIRE(!capturedSelectionName.empty());
  BOOST_REQUIRE(!capturedSelectionWire.empty());
  // Once the valid response owns the terminal state, replaying its exact wire
  // must not invoke the callback or decrypt the payload a second time.
  BOOST_CHECK(!environment.user().handleDecryptedResponseByName(
      capturedResponseName, *capturedResponseBlock));
  // The selected Provider must consume a SelectionKeyEnvelope at most once.
  // Replaying the exact Selection after the invocation terminal state is
  // claimed must not execute the handler or create a second response.
  const auto parsedSelection = parseServiceSelectionNameV2(capturedSelectionName);
  BOOST_REQUIRE(parsedSelection);
  environment.provider(0).OnServiceSelectionMessageDecryptionSuccessCallbackV2(
      parsedSelection->requesterName, parsedSelection->providerName,
      parsedSelection->serviceName, parsedSelection->requestId,
      capturedSelectionWire);
  environment.pumpUntil([&] { return requestExecutions[0].load() > 1; });
  selectionReplayRejected = requestExecutions[0].load() == 1;
  BOOST_CHECK(requestObserved[0]);
  BOOST_CHECK(requestObserved[1]);
  BOOST_CHECK_EQUAL(requestExecutions[0].load(), 1U);
  BOOST_CHECK_EQUAL(requestExecutions[1].load(), 0U);
  BOOST_CHECK(selectionReplayRejected);
  BOOST_CHECK(!unselectedProviderExecuted);
  environment.updateRequestResidue(scope, {});
  environment.resetRequest(scope);
}

BOOST_AUTO_TEST_CASE(SelectedProviderReceivesExactEncryptedInputOnly)
{
  runRequestScopedResponseCase(true, false, false);
}

BOOST_AUTO_TEST_CASE(SelectedProviderReceivesSegmentedEncryptedInput)
{
  // Regression for the 6.55 MB YOLO request: the User must publish bounded
  // signed Data segments and the Provider must reconstruct them through its
  // production SegmentFetcher before invoking the handler.
  runRequestScopedResponseCase(false, false, false, 6555271);
}

BOOST_AUTO_TEST_CASE(ModifiedInlineResponseCiphertextIsRejectedBeforeDelivery)
{
  runRequestScopedResponseCase(false, true, false);
}

BOOST_AUTO_TEST_CASE(RevokedUserCannotReceiveResponseAfterProviderExecution)
{
  runRequestScopedResponseCase(false, false, true);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::integration_test
