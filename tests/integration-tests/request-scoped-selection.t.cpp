#include "tests/boost-test.hpp"

#include "ndnsf-integration-fixture.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include "ndn-service-framework/utils.hpp"
#include "ndn-service-framework/detail/ProtectedTargetedRequest.hpp"

#include <ndn-cxx/util/sha256.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-svs/store.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ndn_service_framework::integration_test {
namespace {

struct ProviderValidationFixture : ServiceProvider
{
  static void useProductionValidation(test::NdnsfIntegrationEnvironment& environment)
  {
    // Bind the production validation entry to the driven Face. The default
    // test trust-anchor "any" deliberately skips signature verification;
    // anchor this negative oracle at the fixture's actual User certificate.
    const auto member = &ProviderValidationFixture::validator;
    environment.provider().*member = std::make_shared<MessageValidator>(
      environment.profile().trustSchemaPath, environment.profile().groupPrefix,
      &environment.providerFace());
    const auto certificate = environment.keyChain().getPib()
      .getIdentity(environment.user().getName()).getDefaultKey().getDefaultCertificate();
    std::ostringstream encoded;
    ndn::io::save(certificate, encoded);
    auto base64 = encoded.str();
    base64.erase(std::remove(base64.begin(), base64.end(), '\n'), base64.end());
    const auto config = std::string(
      "rule\n{\n id inline-user-input\n for data\n checker\n {\n"
      "  type hierarchical\n  sig-type rsa-sha256\n }\n}\n"
      "trust-anchor\n{\n type base64\n base64-string \"") + base64 + "\"\n}\n";
    // The existing getter returns MessageValidator's concrete ValidatorConfig
    // member as Validator&, whose base class is deliberately non-polymorphic.
    auto& validator = static_cast<ndn::ValidatorConfig&>(
      (environment.provider().*member)->getConfiguredValidatorForSegmentFetcher());
    validator.load(config, "inline-user-input-fixture");
  }
};

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

std::size_t
checkStoredRequestPackets(test::NdnsfIntegrationEnvironment& environment,
                          const ndn::Name& requestId)
{
  auto& sync = environment.userPubSub().getSVSync();
  auto node = environment.profile().userNode;
  node.append("0");
  std::size_t matched = 0;
  for (uint64_t sequence = 1; sequence <= sync.getCore().getSeqNo(node); ++sequence) {
    const auto base = sync.getDataName(node, sequence);
    auto packet = sync.getDataStore().find(ndn::Interest(base));
    bool segmented = false;
    if (!packet) {
      auto first = base;
      first.appendVersion(0).appendSegment(0);
      packet = sync.getDataStore().find(ndn::Interest(first));
      segmented = true;
    }
    if (!packet) continue;
    ndn::Data inner(packet->getContent().blockFromValue());
    auto applicationName = inner.getName();
    if (segmented) applicationName = applicationName.getPrefix(-2);
    const auto parsed = parseRequestNameV2(applicationName);
    if (!parsed || parsed->requestId != requestId) continue;
    if (segmented) BOOST_REQUIRE(packet->getFinalBlock().has_value());
    const auto count = segmented ? packet->getFinalBlock()->toSegment() + 1 : 1;
    BOOST_REQUIRE_LE(count, 64U);
    for (uint64_t segment = 0; segment < count; ++segment) {
      if (segmented) {
        auto name = base;
        name.appendVersion(0).appendSegment(segment);
        packet = sync.getDataStore().find(ndn::Interest(name));
      }
      BOOST_REQUIRE(packet != nullptr);
      BOOST_CHECK_LE(packet->wireEncode().size(), 8192U);
      ++matched;
    }
  }
  BOOST_REQUIRE_GT(matched, 0U);
  return matched;
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

  auto status = makeCurrentStatus(serviceName, 1);
  status.setAbePublicParametersName(environment.attributeAuthorityPublicParametersName());
  status.setAbePublicParametersDigest(environment.attributeAuthorityPublicParametersDigest());
  BOOST_REQUIRE(environment.user().installControllerStatus(status));
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    BOOST_REQUIRE(environment.provider(index).installControllerStatus(status));
    environment.provider(index).setUseTokens(false);
  }
  environment.user().setUseTokens(false);
  // Status installation re-arms the authority parameter cache. The ordinary
  // request pump does not drive the authority face; finish that setup first.
  environment.pumpUntilWithAttributeAuthority([] { return false; });
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
          // The final decrypted-response check accepts inline plaintext/AEAD,
          // but does not resolve a large-response reference. Use the same
          // decode/resolve/finish entry point as production OnResponse; only
          // the application callback may declare exact payload delivery.
          environment.user().dispatchDecryptedResponseByName(
              publication.name, parsedResponse->requestId,
              ndn::Buffer(publication.data.begin(), publication.data.end()));
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

BOOST_AUTO_TEST_CASE(ProtectedTargetedExpectedBootRefreshesWithoutWeakeningCarrier)
{
  test::BootstrapProfile profile;
  profile.providerCount = 1;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  const auto service = environment.profile().serviceName;
  const auto providerName = environment.provider().getName();
  auto status = makeCurrentStatus(service, 1);
  status.setAbePublicParametersName(environment.attributeAuthorityPublicParametersName());
  status.setAbePublicParametersDigest(environment.attributeAuthorityPublicParametersDigest());
  BOOST_REQUIRE(environment.user().installControllerStatus(status));
  BOOST_REQUIRE(environment.provider().installControllerStatus(status));
  environment.user().setUseTokens(true);
  environment.provider().setUseTokens(true);
  environment.provider().setHandlerThreads(0);
  ndn::Buffer input{'b', 'o', 'o', 't'}, output{'o', 'k'};
  std::vector<uint64_t> modes;
  auto registration = environment.provider().addScopedTargetedService(service,
    [&] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
         const RequestMessage& request, ServiceProvider::AsyncTargetedCompletion complete) {
      modes.push_back(request.getRequestMode());
      BOOST_CHECK(request.getPayload() == input);
      BOOST_CHECK_EQUAL(request.getTokens().count("targeted.expected_provider_boot"), 0U);
      ResponseMessage response;
      response.setStatus(true);
      response.setPayload(output, output.size());
      complete(response);
    });
  environment.enableProductionIngressForTest();
  std::string currentBoot;
  for (unsigned round = 0; round < 5; ++round) {
    RequestCapabilities caps;
    caps.setField("RequestScopedConfidentialityV1", "required");
    RequestMessage request;
    request.setRequestCapabilities(caps);
    request.setPayload(input, input.size());
    // Cold, matching boot, mismatched boot, matching boot after refresh,
    // and absent hint: only the mismatch must restart bootstrap.
    if (round == 2)
      request.setTokens({{"targeted.expected_provider_boot", providerName.toUri() + ":changed"}});
    else if (round == 1 || round == 3)
      request.setTokens({{"targeted.expected_provider_boot", currentBoot}});
    bool terminal = false, timedOut = false;
    ResponseMessage received;
    const auto id = environment.user().RequestServiceTargeted(providerName, service, request, 5000,
      [&] (const ndn::Name&) { timedOut = true; terminal = true; },
      [&] (const ResponseMessage& response) { received = response; terminal = true; });
    BOOST_REQUIRE(!id.empty());
    environment.pumpUntil([&] { return terminal; });
    BOOST_REQUIRE(terminal);
    BOOST_REQUIRE(!timedOut);
    BOOST_REQUIRE(received.getStatus());
    BOOST_CHECK(received.getPayload() == output);
    BOOST_REQUIRE_EQUAL(modes.size(), round + 1);
    BOOST_CHECK_EQUAL(modes.back(), round == 0 || round == 2
      ? tlv::TargetedBootstrapRequest : tlv::TargetedRequest);
    BOOST_REQUIRE(received.getTokens().count("targeted.providerBootEpoch"));
    currentBoot = received.getTokens().at("targeted.providerBootEpoch");
    BOOST_CHECK_EQUAL(environment.user().getPendingCallCount(), 0U);
  }
}

BOOST_AUTO_TEST_CASE(ProtectedTargetedWarmCallsReplenishAndFenceClosedRegistration)
{
  test::BootstrapProfile profile;
  profile.providerCount = 1;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  const auto service = environment.profile().serviceName;
  const auto providerName = environment.provider().getName();
  std::size_t inputInterests = 0;
  auto inputInterestObserver = environment.providerFace().onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      if (interest.getName().toUri().find("REQUEST-INPUT") != std::string::npos)
        ++inputInterests;
    });
  auto status = makeCurrentStatus(service, 1);
  status.setAbePublicParametersName(environment.attributeAuthorityPublicParametersName());
  status.setAbePublicParametersDigest(environment.attributeAuthorityPublicParametersDigest());
  BOOST_REQUIRE(environment.user().installControllerStatus(status));
  BOOST_REQUIRE(environment.provider().installControllerStatus(status));
  environment.user().setUseTokens(true);
  environment.provider().setUseTokens(true);
  environment.provider().setHandlerThreads(0);

  using Completion = ServiceProvider::AsyncTargetedCompletion;
  std::optional<Completion> deferred;
  std::vector<uint64_t> modes;
  std::set<ndn::Name> executedIds;
  std::optional<RequestMessage> lastExecutedRequest;
  ndn::Name lastExecutedId;
  bool deferResponse = false;
  ndn::Buffer input{'w', 'a', 'r', 'm'};
  ndn::Buffer output{'o', 'k'};
  auto registration = environment.provider().addScopedTargetedService(service,
    [&] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
         const ndn::Name& id, const RequestMessage& request, Completion completion) {
      modes.push_back(request.getRequestMode());
      BOOST_CHECK(executedIds.insert(id).second);
      BOOST_CHECK(request.getPayload() == input);
      lastExecutedRequest = request;
      lastExecutedId = id;
      if (deferResponse) {
        deferred = std::move(completion);
        return;
      }
      ResponseMessage response;
      response.setStatus(true);
      response.setPayload(output, output.size());
      completion(response);
    });
  environment.enableProductionIngressForTest();

  auto submit = [&] (bool& terminal, bool& timedOut, ResponseMessage& received) {
    RequestCapabilities caps;
    caps.setField("RequestScopedConfidentialityV1", "required");
    RequestMessage request;
    request.setRequestCapabilities(caps);
    request.setPayload(input, input.size());
    return environment.user().RequestServiceTargeted(providerName, service, request, 5000,
      [&] (const ndn::Name&) { timedOut = true; terminal = true; },
      [&] (const ResponseMessage& response) { received = response; terminal = true; });
  };
  for (unsigned round = 0; round < 3; ++round) {
    const auto inputInterestsBefore = inputInterests;
    bool terminal = false, timedOut = false;
    ResponseMessage received;
    const auto id = submit(terminal, timedOut, received);
    BOOST_REQUIRE(!id.empty());
    environment.pumpUntil([&] { return terminal; });
    BOOST_REQUIRE(terminal);
    BOOST_REQUIRE(!timedOut);
    BOOST_REQUIRE(received.getStatus());
    BOOST_CHECK(received.getPayload() == output);
    BOOST_CHECK_EQUAL(checkStoredRequestPackets(environment, id), 1U);
    BOOST_REQUIRE_EQUAL(modes.size(), round + 1);
    BOOST_CHECK_EQUAL(modes.back(), round == 0 ? tlv::TargetedBootstrapRequest : tlv::TargetedRequest);
    if (round == 0) BOOST_CHECK_GT(inputInterests, inputInterestsBefore);
    else BOOST_CHECK_EQUAL(inputInterests, inputInterestsBefore);
    const auto& tokens = received.getTokens();
    BOOST_REQUIRE(tokens.count("targeted.count") != 0);
    BOOST_CHECK_EQUAL(tokens.at("targeted.count"), "1");
    BOOST_REQUIRE(tokens.count("targeted.scopedKeyV1") != 0);
    BOOST_CHECK_EQUAL(tokens.at("targeted.scopedKeyV1"), "1");
    BOOST_REQUIRE(tokens.count("targeted.inlineSignedInputV1") != 0);
    BOOST_CHECK_EQUAL(tokens.at("targeted.inlineSignedInputV1"), "1");
    BOOST_CHECK_EQUAL(environment.user().getPendingCallCount(), 0U);
  }

  // Restore the original sanitized wire payload from the handler's decoded
  // request. Replay both its ID and its spent token under a fresh targeted ID;
  // neither may trigger a second input fetch or handler execution.
  BOOST_REQUIRE(lastExecutedRequest.has_value());
  auto replayRequest = *lastExecutedRequest;
  ndn::Buffer emptyPayload;
  replayRequest.setPayload(emptyPayload, 0);
  const auto replayBlock = replayRequest.WireEncode();
  const ndn::Buffer replayWire(replayBlock.data(), replayBlock.size());
  for (const auto& replayId : {lastExecutedId,
       detail::makeTargetedRequestId(providerName, ndn::Name("/spent-protected-token"))}) {
    environment.provider().OnRequestDecryptionSuccessCallbackV2(
      environment.user().getName(), service, replayId, replayWire);
    BOOST_CHECK_EQUAL(modes.size(), 3U);
  }

  bool terminal = false, timedOut = false;
  ResponseMessage received;
  input.assign(8192, 'l'); // Negotiated mode retains segmented external fallback.
  const auto fallbackInputInterestsBefore = inputInterests;
  deferResponse = true;
  const auto id = submit(terminal, timedOut, received);
  BOOST_REQUIRE(!id.empty());
  environment.pumpUntil([&] { return deferred.has_value() || terminal; });
  BOOST_REQUIRE(deferred.has_value());
  BOOST_CHECK_GT(inputInterests, fallbackInputInterestsBefore);
  BOOST_REQUIRE_EQUAL(modes.size(), 4U);
  BOOST_CHECK_EQUAL(modes.back(), tlv::TargetedRequest);
  registration.close();
  ResponseMessage lateSuccess;
  lateSuccess.setStatus(true);
  lateSuccess.setPayload(output, output.size());
  (*deferred)(lateSuccess);
  (*deferred)(lateSuccess);
  environment.pumpUntil([&] { return terminal; });
  BOOST_REQUIRE(terminal);
  BOOST_CHECK(!timedOut);
  BOOST_CHECK(!received.getStatus());
  BOOST_CHECK_EQUAL(environment.user().getPendingCallCount(), 0U);
  BOOST_CHECK_EQUAL(modes.size(), 4U);
  deferred.reset();
}

BOOST_AUTO_TEST_CASE(ProtectedTargetedInlineLegacyOfferAndRequestBudgetFallback)
{
  for (bool legacyOffer : {true, false}) {
    test::BootstrapProfile profile;
    profile.providerCount = 1;
    test::NdnsfIntegrationEnvironment environment(profile);
    environment.bootstrap();
    const auto service = environment.profile().serviceName;
    const auto provider = environment.provider().getName();
    auto status = makeCurrentStatus(service, 1);
    status.setAbePublicParametersName(environment.attributeAuthorityPublicParametersName());
    status.setAbePublicParametersDigest(environment.attributeAuthorityPublicParametersDigest());
    BOOST_REQUIRE(environment.user().installControllerStatus(status));
    BOOST_REQUIRE(environment.provider().installControllerStatus(status));
    environment.user().setUseTokens(true);
    environment.provider().setUseTokens(true);
    environment.provider().setHandlerThreads(0);
    std::size_t executions = 0, inputInterests = 0;
    std::optional<ndn::Data> publishedInput;
    auto inputObserver = environment.providerFace().onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        if (interest.getName().toUri().find("REQUEST-INPUT") != std::string::npos) {
          ++inputInterests;
          // DummyFace does not cache unsolicited Data. Replay only the
          // original exact packet when the production fetch expresses it.
          if (publishedInput && interest.getName() == publishedInput->getName())
            environment.providerFace().receive(*publishedInput);
        }
      });
    auto wireObserver = environment.userFace().onSendData.connect(
      [&] (const ndn::Data& data) {
        // Observe actual signed packets transferred by the SVS fixture,
        // rather than treating the inner Request size as a wire budget.
        BOOST_CHECK_LE(data.wireEncode().size(), 8192U);
        if (data.getName().toUri().find("REQUEST-INPUT") != std::string::npos)
          publishedInput = data;
      });
    auto registration = environment.provider().addScopedTargetedService(service,
      [&] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
           const RequestMessage& request, ServiceProvider::AsyncTargetedCompletion complete) {
        ++executions;
        BOOST_CHECK(request.getPayload() == ndn::Buffer({'s', 'm', 'a', 'l', 'l'}));
        ResponseMessage response;
        response.setStatus(true);
        ndn::Buffer result{'o', 'k'};
        response.setPayload(result, result.size());
        complete(response);
      });
    // Existing explicit decrypted publication cut points allow a real minted
    // response to model an older peer before the User caches its offer.
    environment.providerPubSub().subscribeToProducer(profile.userNode,
      [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        ndn::Buffer wire(publication.data.begin(), publication.data.end());
        if (const auto parsed = parseRequestNameV2(publication.name))
          environment.provider().OnRequestDecryptionSuccessCallbackV2(
            parsed->requesterName, parsed->serviceName, parsed->requestId, wire);
        else if (const auto parsed = parseServiceSelectionNameV2(publication.name))
          environment.provider().OnServiceSelectionMessageDecryptionSuccessCallbackV2(
            parsed->requesterName, parsed->providerName, parsed->serviceName,
            parsed->requestId, wire);
      }, true);
    environment.provider().setLocalPublicationHandler(
      [&] (const ndn::Name& name, const ndn::Buffer& wire) {
        publish(environment.providerPubSub(), name, wire);
      });
    environment.userPubSub().subscribeToProducer(profile.providerNode,
      [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        if (parseRequestAckNameV2(publication.name)) {
          environment.user().handleRequestAckByName(publication.name, ndn::Block(publication.data));
        }
        else if (const auto parsed = parseResponseNameV2(publication.name)) {
          ResponseMessage response;
          BOOST_REQUIRE(response.WireDecode(ndn::Block(publication.data)));
          if (legacyOffer) {
            auto tokens = response.getTokens();
            BOOST_REQUIRE(tokens.erase("targeted.inlineSignedInputV1") == 1);
            response.setTokens(tokens);
          }
          const auto block = response.WireEncode();
          environment.user().dispatchDecryptedResponseByName(publication.name,
            parsed->requestId, ndn::Buffer(block.begin(), block.end()));
        }
      }, true);
    environment.provider().installLocalMockDataIngressForTest();
    environment.user().setLocalPublicationHandler(
      [&] (const ndn::Name& name, const ndn::Buffer& wire) {
        publish(environment.userPubSub(), name, wire);
      });
    std::optional<RequestMessage> lastPublished;
    environment.user().setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& name, const std::vector<ndn::Name>&,
           const ndn::Name&, const RequestMessage& request, size_t) {
        lastPublished = request;
        const auto block = request.WireEncode();
        publish(environment.userPubSub(), name, ndn::Buffer(block.begin(), block.end()));
      });
    environment.pumpUntilWithAttributeAuthority([] { return false; });
    for (unsigned round = 0; round < 2; ++round) {
      bool terminal = false, timedOut = false;
      ResponseMessage received;
      RequestMessage request;
      RequestCapabilities caps;
      caps.setField("RequestScopedConfidentialityV1", "required");
      if (!legacyOffer && round == 1) {
        // The input remains a tiny single signed Data; only the complete
        // Request exceeds its budget and must drop the optional payload.
        caps.setField("largeContextA", std::string(4096, 'a'));
        caps.setField("largeContextB", std::string(4096, 'b'));
      }
      request.setRequestCapabilities(caps);
      ndn::Buffer input{'s', 'm', 'a', 'l', 'l'};
      request.setPayload(input, input.size());
      const auto before = inputInterests;
      const auto id = environment.user().RequestServiceTargeted(provider, service, request, 5000,
        [&] (const ndn::Name&) { terminal = true; timedOut = true; },
        [&] (const ResponseMessage& response) { terminal = true; received = response; });
      BOOST_REQUIRE(!id.empty());
      environment.pumpUntilWithAttributeAuthority([&] { return terminal; });
      BOOST_REQUIRE(terminal && !timedOut && received.getStatus());
      BOOST_REQUIRE(lastPublished.has_value());
      const auto packets = checkStoredRequestPackets(environment, id);
      if (!legacyOffer && round == 1) BOOST_CHECK_GT(packets, 1U);
      BOOST_CHECK_GT(inputInterests, before);
      if (round == 1) {
        BOOST_CHECK_EQUAL(lastPublished->getRequestMode(), tlv::TargetedRequest);
        BOOST_CHECK(detail::hasProtectedTargetedCarrier(*lastPublished));
        BOOST_CHECK_EQUAL(detail::hasOptionalInlineTargetedInput(*lastPublished), !legacyOffer);
        BOOST_CHECK(lastPublished->getPayload().empty());
        if (!legacyOffer)
          BOOST_CHECK_GT(lastPublished->WireEncode().size(), detail::TARGETED_INLINE_REQUEST_MAX_BYTES);
      }
    }
    BOOST_CHECK_EQUAL(executions, 2U);
  }
}

BOOST_AUTO_TEST_CASE(ProtectedTargetedInlineRejectsUntrustedSignedInput)
{
  for (unsigned variant = 0; variant < 7; ++variant) {
    BOOST_TEST_CONTEXT("inline negative variant " << variant) {
    test::BootstrapProfile profile;
    profile.providerCount = 1;
    test::NdnsfIntegrationEnvironment environment(profile);
    environment.bootstrap();
    const auto service = environment.profile().serviceName;
    const auto provider = environment.provider().getName();
    const auto requester = environment.user().getName();
    auto status = makeCurrentStatus(service, 1);
    status.setAbePublicParametersName(environment.attributeAuthorityPublicParametersName());
    status.setAbePublicParametersDigest(environment.attributeAuthorityPublicParametersDigest());
    BOOST_REQUIRE(environment.user().installControllerStatus(status));
    BOOST_REQUIRE(environment.provider().installControllerStatus(status));
    environment.user().setUseTokens(true);
    environment.provider().setUseTokens(true);
    environment.provider().setHandlerThreads(0);
    ProviderValidationFixture::useProductionValidation(environment);
    std::size_t executions = 0, inputInterests = 0;
    auto inputObserver = environment.providerFace().onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        if (interest.getName().toUri().find("REQUEST-INPUT") != std::string::npos)
          ++inputInterests;
      });
    auto registration = environment.provider().addScopedTargetedService(service,
      [&] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
           const RequestMessage&, ServiceProvider::AsyncTargetedCompletion complete) {
        ++executions;
        ResponseMessage response;
        response.setStatus(true);
        ndn::Buffer result{'o', 'k'};
        response.setPayload(result, result.size());
        complete(response);
      });
    environment.enableProductionIngressForTest();
    bool terminal = false, timedOut = false;
    ResponseMessage received;
    const auto submit = [&] {
      terminal = false;
      timedOut = false;
      RequestMessage request;
      RequestCapabilities caps;
      caps.setField("RequestScopedConfidentialityV1", "required");
      request.setRequestCapabilities(caps);
      ndn::Buffer input{'t', 'e', 's', 't'};
      request.setPayload(input, input.size());
      return environment.user().RequestServiceTargeted(provider, service, request, 5000,
        [&] (const ndn::Name&) { terminal = true; timedOut = true; },
        [&] (const ResponseMessage& response) { terminal = true; received = response; });
    };
    BOOST_REQUIRE(!submit().empty());
    environment.pumpUntil([&] { return terminal; });
    BOOST_REQUIRE(terminal && !timedOut && received.getStatus());
    BOOST_REQUIRE_EQUAL(executions, 1U);
    std::optional<RequestMessage> captured;
    ndn::Name capturedId;
    environment.user().setRequestPublisher(
      [&] (const ndn::Name& id, const ndn::Name&, const std::vector<ndn::Name>&,
           const ndn::Name&, const RequestMessage& request, size_t) {
        capturedId = id;
        captured = request; // Hold the fresh unused token at the ingress cut point.
      });
    BOOST_REQUIRE(!submit().empty());
    environment.pumpUntil([&] { return captured.has_value(); });
    BOOST_REQUIRE(captured.has_value());
    BOOST_REQUIRE(detail::hasOptionalInlineTargetedInput(*captured));
    auto payload = captured->getPayload();
    BOOST_REQUIRE(!payload.empty());
    if (variant == 0) payload.push_back(0); // More than one complete packet.
    else if (variant == 1) payload.assign(detail::TARGETED_INLINE_INPUT_MAX_BYTES + 1, 0);
    else if (variant < 6) {
      ndn::Data packet{ndn::Block(ndn::span<const uint8_t>(payload.data(), payload.size()))};
      if (variant == 2) packet.setName(ndn::Name("/other/input"));
      if (variant == 4) {
        AeadEnvelope envelope;
        BOOST_REQUIRE(envelope.wireDecode(packet.getContent().blockFromValue()));
        BOOST_REQUIRE(!envelope.ciphertext.empty());
        envelope.ciphertext.front() ^= 1;
        packet.setContent(envelope.wireEncode());
      }
      environment.keyChain().sign(packet, ndn::security::signingByIdentity(
        variant == 3 ? provider : requester));
      const auto& wire = packet.wireEncode();
      payload.assign(wire.begin(), wire.end());
      if (variant == 5) payload.back() ^= 1; // Signature fails before AEAD admission.
    }
    captured->setPayload(payload, payload.size());
    if (variant == 6) {
      // Change an authenticated Request field after wrapping the input key.
      // A valid User-signed packet cannot repair a different Request AAD.
      auto capabilities = captured->getRequestCapabilities();
      capabilities.setField("callerContext", "changed-after-key-wrap");
      captured->setRequestCapabilities(capabilities);
    }
    const auto before = inputInterests;
    const auto wire = captured->WireEncode();
    environment.provider().OnRequestDecryptionSuccessCallbackV2(
      requester, service, capturedId, ndn::Buffer(wire.begin(), wire.end()));
    for (unsigned pump = 0; pump < 3 && !terminal; ++pump)
      environment.pumpUntil([&] { return terminal; });
    BOOST_REQUIRE(terminal);
    if (variant == 6) {
      // The changed key AAD prevents recovery of the response key as well.
      // User rejects the unauthenticated error; its original deadline ends
      // the pending call. No plaintext or successful execution is admitted.
      BOOST_CHECK(timedOut);
    }
    else {
      BOOST_REQUIRE(!timedOut);
      BOOST_CHECK(!received.getStatus());
    }
    BOOST_CHECK_EQUAL(executions, 1U);
    BOOST_CHECK_EQUAL(inputInterests, before); // Invalid inline data must not fetch a substitute.
    }
  }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::integration_test
