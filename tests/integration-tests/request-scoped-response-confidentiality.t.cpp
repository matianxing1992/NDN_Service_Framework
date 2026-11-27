#include "tests/boost-test.hpp"

#include "ndnsf-integration-fixture.hpp"
#include "isolated-test-trust.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include "ndn-service-framework/RequestConfidentiality.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/common.hpp"
#include "ndn-service-framework/utils.hpp"

#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-cxx/util/scope.hpp>
#include <ndn-cxx/util/time-unit-test-clock.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <openssl/sha.h>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ndn_service_framework::test {

struct LargeResponseFetchOracle
{
  struct Observation
  {
    std::weak_ptr<void> owner;
    std::weak_ptr<void> fetcher;
    std::size_t authenticatedSegments = 0;
    std::size_t plaintextBytes = 0;
    std::size_t committedNonceKeys = 0;
  };

  static Observation observe(ServiceUser& user)
  {
    for (const auto& call : user.m_pendingCalls) {
      for (const auto& item : call.second.largeResponseFetches) {
        const std::weak_ptr<void> fetcher = item.second->manifestFetcher ?
          std::weak_ptr<void>(item.second->manifestFetcher) : item.second->fetcher;
        return {item.second, fetcher,
                item.second->nonces.size(), item.second->plaintext.size(),
                user.m_requestScopedNonceRegistry.size()};
      }
    }
    return {};
  }

  static void cancelThroughSharedTerminalCleanup(ServiceUser& user, const ndn::Name& requestId)
  {
    // CancelCollaboration's shared final step, without fabricating a collaboration
    // assignment for this unary transport fixture. Public cancellation is covered
    // separately by the MainApp production facade/native job fixtures.
    user.erasePendingCallWithTrace(requestId, user.m_pendingCalls.find(requestId),
                                  "caller_cancelled");
  }
};

} // namespace ndn_service_framework::test

namespace ndn_service_framework::integration_test {
namespace {

std::string
certificateDigest(const ndn::security::Certificate& certificate)
{
  const auto wire = certificate.wireEncode();
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(wire.data(), wire.size(), digest);
  std::ostringstream os;
  os << "sha256:" << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    os << std::setw(2) << static_cast<unsigned>(byte);
  }
  return os.str();
}

struct TestHybridPublication
{
  HybridMessageKey key;
  ndn::Buffer wire;
};

// Wrap a message wire in the same HybridMessageEnvelope the production
// publisher emits, so the Provider's production ingress decrypt path
// (decryptHybridMessage) can accept it after the receive key is seeded.
TestHybridPublication
makeTestHybridPublication(const ndn::Name& messageName,
                          const ndn::Name& serviceName,
                          const ndn::Name& requestId,
                          const ndn::Name& senderPrefix,
                          const std::string& messageType,
                          const ndn::Buffer& plaintext)
{
  HybridMessageCrypto crypto;
  HybridCryptoCounters counters;
  const auto accessAttribute = hybridAccessAttributeForName(messageName, serviceName);
  auto key = crypto.getOrCreateSendKey(serviceName, senderPrefix,
                                       accessAttribute, messageType, counters);
  const auto associatedData = hybridAssociatedData(
      messageName, messageType, requestId, serviceName, senderPrefix,
      key.keyId, key.epochId);
  const auto encrypted = hybridAesGcmEncrypt(
      key.key, ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
      ndn::span<const uint8_t>(associatedData.data(), associatedData.size()));

  HybridMessageEnvelope envelope;
  envelope.setKeyId(key.keyId);
  envelope.setEpochId(key.epochId);
  envelope.setMessageType(messageType);
  envelope.setNonce(encrypted.nonce);
  envelope.setCipherText(encrypted.ciphertext);
  envelope.setAuthTag(encrypted.tag);
  const auto wireBlock = envelope.WireEncode();
  return {std::move(key), ndn::Buffer(wireBlock.data(), wireBlock.size())};
}

} // namespace

BOOST_AUTO_TEST_SUITE(RequestScopedResponseConfidentiality)

BOOST_AUTO_TEST_CASE(OwnedResponsePayloadIsSharedUntilReplacement)
{
  ndn::Buffer bytes{0x23, 0x00, 0x71};
  const auto* originalStorage = bytes.data();
  ResponseMessage response;
  response.setStatus(true);
  response.setPayload(std::move(bytes));
  BOOST_CHECK(response.getPayloadBlock().value() == originalStorage);
  const auto copy = response;
  BOOST_CHECK(copy.getPayloadBlock().value() == originalStorage);
  response.Clear();
  ResponseMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(copy.WireEncode()));
  const auto actual = decoded.getPayload();
  const std::array<std::uint8_t, 3> expected{0x23, 0x00, 0x71};
  BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(LargeResponseUsesConfiguredTrustAndRequestBoundAead)
{
  test::IsolatedTestTrust trustOwner;
  ndn::KeyChain keyChain("pib-memory:spec179-response-confidentiality",
                         "tpm-memory:spec179-response-confidentiality");
  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enableRegistrationReply = true;
  ndn::DummyClientFace providerFace(keyChain, faceOptions);
  ndn::DummyClientFace consumerFace(keyChain, faceOptions);
  auto interestRelay = consumerFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        providerFace.receive(interest);
      });
  auto dataRelay = providerFace.onSendData.connect(
      [&] (const ndn::Data& data) {
        consumerFace.receive(data);
      });
  const ndn::Name requesterName("/spec179/user/alice");
  const ndn::Name providerName("/spec179/provider/camera");
  const ndn::Name serviceName("/ObjectDetection/YOLOv8");
  const ndn::Name requestId("/request/response-confidentiality");
  const auto userCert = trustOwner.createTestIdentityCertificate(keyChain, requesterName);
  const auto providerCert = trustOwner.createTestIdentityCertificate(keyChain, providerName);
  const auto aaCert = trustOwner.createTestIdentityCertificate(keyChain, ndn::Name("/spec179/aa"));

  ServiceProvider provider(ServiceProvider::LocalMockTag{}, providerFace,
                           ndn::Name("/spec179/group"), providerCert,
                           aaCert, trustOwner.schemaPath());
  // The production large-response path signs every segment with the Provider
  // certificate. LocalMock has an isolated keychain unless explicitly bound.
  provider.useSigningKeyChainForTest(keyChain);
  provider.installLocalMockDataIngressForTest();
  providerFace.processEvents(ndn::time::milliseconds(5));
  providerFace.getIoContext().restart();

  RequestSecurityBinding binding;
  binding.serviceName = serviceName;
  binding.requestId = requestId;
  binding.attempt = 1;
  binding.controllerVersion = ControllerVersion{1788285600123ULL, 7};
  binding.userEncryptionCertName = userCert.getName();
  binding.userEncryptionCertDigest = certificateDigest(userCert);
  binding.providerEncryptionCertName = providerCert.getName();
  binding.providerEncryptionCertDigest = certificateDigest(providerCert);
  binding.selectionDigest = "sha256:" + std::string(64, '1');
  binding.inputDataName = ndn::Name("/spec179/user/alice/NDNSF/INPUT/response-confidentiality");
  binding.segmentOrEventId = "response";
  BOOST_REQUIRE(binding.isValid());

  const auto now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const auto keys = generateRequestKeyBundle(now, now + 120000);
  const auto steady = std::make_shared<ndn::time::UnitTestSteadyClock>();
  ndn::time::setCustomClocks(steady);
  const auto restoreClock = ndn::make_scope_exit([] { ndn::time::setCustomClocks(); });
  const std::string plaintext(270 * 1024, 'r');
  ResponseMessage response;
  response.setStatus(true);
  ndn::Buffer responsePayload(
      reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size());
  response.setPayload(responsePayload, responsePayload.size());

  const auto published = provider.makeRequestScopedResponseWithLargeDataOptimization(
      requesterName, providerName, serviceName, requestId, response, keys,
      binding, 1024);
  BOOST_REQUIRE_MESSAGE(published.success, published.errorMessage);
  BOOST_REQUIRE(published.usedLargeDataReference);
  const auto reference = parseLargeDataReferencePayload(
      published.responseMessage.getPayload());
  BOOST_REQUIRE(reference);
  BOOST_CHECK_EQUAL(reference->keyScope, "request");
  BOOST_CHECK_EQUAL(reference->plaintextSize, plaintext.size());
  BOOST_CHECK(published.responseMessage.hasControllerVersion());
  BOOST_CHECK(published.responseMessage.getControllerVersion() ==
              binding.controllerVersion);

  // A wide response must survive an IMS miss from the first discovery
  // Interest. Both the root and segment name retrieve the same signed wire.
  BOOST_CHECK_EQUAL(provider.evictImsPrefixForTest(reference->dataName), 0U);
  const auto fetch = [&](ndn::Interest interest) {
    std::optional<ndn::Data> received;
    bool completed = false;
    interest.setInterestLifetime(ndn::time::milliseconds(500));
    const ndn::ScopedPendingInterestHandle pending = consumerFace.expressInterest(
      interest,
      [&](const ndn::Interest&, const ndn::Data& data) {
        received = data;
        completed = true;
      },
      [&](const ndn::Interest&, const ndn::lp::Nack&) { completed = true; },
      [&](const ndn::Interest&) { completed = true; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!completed && std::chrono::steady_clock::now() < deadline) {
      steady->advance(ndn::time::milliseconds(1));
      consumerFace.getIoContext().restart();
      consumerFace.getIoContext().poll();
      providerFace.getIoContext().restart();
      providerFace.getIoContext().poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BOOST_REQUIRE(completed);
    return received;
  };
  ndn::Interest discovery(reference->dataName);
  discovery.setCanBePrefix(true).setMustBeFresh(true);
  const auto first = fetch(discovery);
  BOOST_REQUIRE(first);
  const auto segmentName = ndn::Name(reference->dataName).appendSegment(0);
  BOOST_CHECK_EQUAL(first->getName(), segmentName);
  BOOST_REQUIRE(first->getFinalBlock());
  BOOST_CHECK_EQUAL(first->getFinalBlock()->toSegment(), 269U);
  const auto repeated = fetch(ndn::Interest(first->getFullName()));
  BOOST_REQUIRE(repeated);
  BOOST_CHECK(repeated->wireEncode() == first->wireEncode());
  const auto outside = ndn::Name(reference->dataName).appendSegment(270);
  BOOST_CHECK(!fetch(ndn::Interest(outside)));
  const auto rejected = provider.makeRequestScopedResponseWithLargeDataOptimization(
    requesterName, providerName, serviceName, ndn::Name(requestId).append("over-limit"),
    response, keys, [&] {
      auto tooFine = binding;
      tooFine.requestId.append("over-limit");
      return tooFine;
    }(), 1);
  BOOST_CHECK(!rejected.success);
  BOOST_CHECK(!rejected.usedLargeDataReference);
  BOOST_CHECK(rejected.errorMessage.find("response packet file capacity") != std::string::npos);
  const std::vector<ndn::Data> segments{*first};

  // Validate against the fixture's fixed certificate anchors.
  auto validator = std::make_shared<MessageValidator>(
      trustOwner.schemaPath(), std::nullopt, &consumerFace);
  size_t validated = 0;
  size_t validationFailures = 0;
  for (const auto& data : segments) {
    validator->validateWithConfiguredTrustSchema(
        data,
        [&] (const ndn::Data&) { ++validated; },
        [&] (const ndn::Data&, const ndn::security::ValidationError&) {
          ++validationFailures;
        });
    BOOST_CHECK(ndn::security::verifySignature(data, providerCert));
  }
  for (size_t round = 0; round < 100 && validated + validationFailures < segments.size(); ++round) {
    steady->advance(ndn::time::milliseconds(5));
    consumerFace.getIoContext().restart();
    providerFace.getIoContext().restart();
    consumerFace.getIoContext().poll();
    providerFace.getIoContext().poll();
  }
  BOOST_CHECK_EQUAL(validated, segments.size());
  BOOST_CHECK_EQUAL(validationFailures, 0U);

  std::set<std::string> nonceKeys;
  ndn::Buffer recovered;
  for (size_t index = 0; index < segments.size(); ++index) {
    const auto content = segments[index].getContent();
    auto [parsed, envelopeBlock] = ndn::Block::fromBuffer(
        ndn::span<const uint8_t>(content.value(), content.value_size()));
    BOOST_REQUIRE(parsed);
    AeadEnvelope envelope;
    BOOST_REQUIRE(envelope.wireDecode(envelopeBlock));
    const auto expectedId = "response/" + std::to_string(index);
    BOOST_CHECK_EQUAL(envelope.segmentOrEventId, expectedId);
    BOOST_CHECK_EQUAL(envelope.keyId, keys.keyId);
    BOOST_REQUIRE_EQUAL(envelope.nonce.size(), 12U);
    BOOST_CHECK(nonceKeys.insert(std::string(
        reinterpret_cast<const char*>(envelope.nonce.data()), envelope.nonce.size())).second);

    auto segmentBinding = binding;
    segmentBinding.segmentOrEventId = expectedId;
    ndn::Buffer clear;
    RequestCryptoFailure failure = RequestCryptoFailure::NONE;
    BOOST_REQUIRE(decryptRequestContent(keys.responseKey, keys.keyId,
                                         segmentBinding, envelope, clear,
                                         &failure));
    recovered.insert(recovered.end(), clear.begin(), clear.end());

    if (index == 0) {
      auto tampered = envelope;
      tampered.ciphertext[0] ^= 0x01;
      ndn::Buffer ignored;
      BOOST_CHECK(!decryptRequestContent(keys.responseKey, keys.keyId,
                                          segmentBinding, tampered, ignored,
                                          &failure));
      auto wrongProviderBinding = segmentBinding;
      wrongProviderBinding.providerEncryptionCertName =
          ndn::Name("/spec179/provider/other/KEY/default");
      BOOST_CHECK(!decryptRequestContent(keys.responseKey, keys.keyId,
                                          wrongProviderBinding, envelope, ignored,
                                          &failure));
      auto staleBinding = segmentBinding;
      staleBinding.controllerVersion.controllerEpoch++;
      BOOST_CHECK(!decryptRequestContent(keys.responseKey, keys.keyId,
                                          staleBinding, envelope, ignored,
                                          &failure));
    }
  }
  BOOST_REQUIRE(!recovered.empty());
  BOOST_CHECK(std::equal(recovered.begin(), recovered.end(), plaintext.begin()));
  BOOST_CHECK_EQUAL(nonceKeys.size(), segments.size());
  steady->advance(ndn::time::seconds(2 * REQUEST_SCOPED_RESPONSE_FETCH_TIMEOUT.count() + 1));
  BOOST_CHECK(!fetch(discovery));
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
  status.setControllerCertificate(ndn::Name("/controller/spec179/response"));
  return status;
}

void
publish(ndn::svs::SVSPubSub& pubSub, const ndn::Name& name,
        const ndn::Buffer& wire)
{
  pubSub.publish(name, ndn::span<const uint8_t>(wire.data(), wire.size()));
}

// Drive one request through the production ingress: the User's attached SVS
// publishes the Request, the Provider's production subscription dispatches it,
// and the response returns through the User's production OnResponse with the
// request-scoped decrypt path.  Large responses additionally traverse the
// Provider segments and the User's SegmentFetcher reconstruction.
enum class StagingPolicy { NONE, REFRESH, REVOKE };

void
runFullRuntimeResponseCase(bool useLargeResponse, bool revokeAfterExecution = false,
                           std::string terminalDuringFetch = {},
                           size_t largeResponseBytes = 9000,
                           std::string expectedResponseError = {},
                           StagingPolicy stagingPolicy = StagingPolicy::NONE,
                           bool signedManifest = false)
{
  // Callback dependencies outlive the environment and all of its worker teardown.
  std::atomic<bool> releaseSegment{terminalDuringFetch.empty()};
  std::atomic<size_t> heldSegments{0};
  std::atomic<size_t> segmentInterests{0};
  std::set<ndn::Name> manifestRoots;
  std::set<ndn::Name> manifestPages;
  std::set<ndn::Name> manifestSources;
  std::atomic<bool> responseReceived{false};
  std::atomic<bool> requestTimedOut{false};
  std::atomic<size_t> requestExecutions{0};
  std::atomic<size_t> responseCallbacks{0};
  std::atomic<size_t> timeoutCallbacks{0};
  bool policyInstalledWhileStaging = false;
  std::optional<ndn::Data> heldSecondSegment;
  std::optional<ndn::Data> heldRoot;
  std::string responseError;
  test::LargeResponseFetchOracle::Observation observed;
  const std::string inputText = "full-runtime-secret-input";
  const std::string responseText = useLargeResponse ?
      std::string(largeResponseBytes, 'R') : "full-runtime-inline-response";

  test::BootstrapProfile profile;
  profile.providerCount = 2;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  const ndn::Name largeResponseCategory("/NDNSF/LARGE-RESPONSE");
  const auto isLargeResponse = [&] (const ndn::Name& name) {
    return std::search(name.begin(), name.end(),
                       largeResponseCategory.begin(), largeResponseCategory.end()) != name.end();
  };
  test::FaultProfile faults;
  faults.dropDataPredicate = [&] (const ndn::Data& data) {
    const auto& name = data.getName();
    if (terminalDuringFetch == "bad-root" && !name.empty() &&
        name.at(-1).toUri() == "MANIFEST" && isLargeResponse(name)) {
      heldRoot = data;
      return true;
    }
    const bool hold = !releaseSegment.load() && !name.empty() &&
      name.at(-1).isSegment() && name.at(-1).toSegment() != 0 &&
      isLargeResponse(name);
    if (hold) {
      ++heldSegments;
      if ((terminalDuringFetch == "tamper" || terminalDuringFetch == "timeout") &&
          name.at(-1).toSegment() == 1)
        heldSecondSegment = data;
    }
    return hold;
  };
  auto scope = environment.beginRequest("request-scoped-response-runtime", faults);
  environment.user().setHandlerThreads(0);
  ndn::signal::ScopedConnection segmentInterestObserver =
    environment.userFace().onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      if (isLargeResponse(interest.getName())) {
        ++segmentInterests;
        if (!signedManifest) return;
        const auto& name = interest.getName();
        BOOST_CHECK(!interest.getCanBePrefix());
        if (name.at(-1).toUri() == "MANIFEST") {
          manifestRoots.insert(name);
        }
        else {
          BOOST_REQUIRE(name.at(-1).isImplicitSha256Digest());
          if (name.at(-3).toUri() == "DATA")
            manifestSources.insert(name);
          else
            manifestPages.insert(name);
        }
      }
    });

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  std::vector<ndn::Name> providers;
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    providers.push_back(environment.provider(index).getName());
  }

  // Bind the status to the fixture Attribute Authority's real public
  // parameters so the runtime NAC re-arm after the first status install can
  // re-establish the Producer CK wrap for the production ACK path.
  auto status = currentStatusFor(serviceName, 1);
  status.setAbePublicParametersName(
      environment.attributeAuthorityPublicParametersName());
  status.setAbePublicParametersDigest(
      environment.attributeAuthorityPublicParametersDigest());
  BOOST_REQUIRE(environment.user().installControllerStatus(status));
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    BOOST_REQUIRE(environment.provider(index).installControllerStatus(status));
    environment.provider(index).setUseTokens(false);
  }
  environment.user().setUseTokens(false);
  // The first status install clears the NAC caches and starts an async
  // public-parameter re-arm.  The normal request pump deliberately skips the
  // Attribute Authority face, so drive the re-arm to completion here before
  // any request is published; otherwise the production ACK CK wrap finds an
  // empty parameter cache and drops the ACK.
  environment.pumpUntilWithAttributeAuthority([] { return false; });

  for (size_t index = 0; index < environment.providerCount(); ++index) {
    auto& provider = environment.provider(index);
    auto* const io = &environment.providerFace(index).getIoContext();
    if (stagingPolicy != StagingPolicy::NONE)
      provider.setHandlerThreads(0);
    provider.addService(
        serviceName,
        ServiceProvider::RequestHandler(
            [&, index, io] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
                 const ndn::Name& invocationId, const RequestMessage& request) {
              ++requestExecutions;
              BOOST_CHECK_EQUAL(request.requiresResponseManifest(), signedManifest);
              const auto actualPayload = request.getPayload();
              BOOST_CHECK_EQUAL_COLLECTIONS(
                  actualPayload.begin(), actualPayload.end(),
                  inputText.begin(), inputText.end());
              ResponseMessage response;
              response.setStatus(true);
              ndn::Buffer payload(
                  reinterpret_cast<const uint8_t*>(responseText.data()),
                  responseText.size());
              response.setPayload(payload, payload.size());
              if (stagingPolicy != StagingPolicy::NONE) {
                boost::asio::post(*io, [&, index, io, invocationId] {
                  boost::asio::post(*io, [&, index, invocationId] {
                    auto& activeProvider = environment.provider(index);
                    const auto lifecycle = activeProvider.getProviderRequestStatus(invocationId);
                    BOOST_REQUIRE(lifecycle);
                    const bool preparing = lifecycle->state ==
                      ServiceProvider::ProviderRequestLifecycleState::EXECUTION_DONE;
                    BOOST_CHECK(preparing);
                    BOOST_CHECK_EQUAL(lifecycle->responsePublishedTimestampUs, 0U);
                    auto refreshed = status;
                    refreshed.setValidity(status.getValidFromMs(),
                                          status.getValidUntilMs() + 1);
                    if (stagingPolicy == StagingPolicy::REVOKE) {
                      auto version = refreshed.getControllerVersion();
                      ++version.controllerEpoch;
                      refreshed.setControllerVersion(version);
                      RevocationTarget target;
                      target.kind = RevocationKind::IDENTITY;
                      target.targetIdentity = activeProvider.getName();
                      refreshed.addRevocation(target);
                    }
                    policyInstalledWhileStaging =
                      preparing && activeProvider.installControllerStatus(refreshed, true, true);
                  });
                });
              }
              return response;
            }));
  }

  // Both roles use their production SVS subscriptions; the response must
  // reach the application only through the real OnResponse decrypt path.
  environment.enableProductionIngressForTest();

  environment.user().setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& requestName,
           const std::vector<ndn::Name>& selectedProviders,
           const ndn::Name& publishedService,
           const RequestMessage& request, size_t strategy) {
        BOOST_CHECK_EQUAL(selectedProviders.size(), providers.size());
        BOOST_CHECK_EQUAL(publishedService, serviceName);
        BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
        BOOST_CHECK(request.getPayload().empty());
        // Emit the same HybridMessageEnvelope the production publisher
        // emits; the Provider production ingress decrypts it through the
        // seeded receive key instead of a fixture-side decode.
        const auto parsed = parseRequestNameV2(requestName);
        BOOST_REQUIRE(parsed);
        const auto wire = request.WireEncode();
        const auto encrypted = makeTestHybridPublication(
            requestName, serviceName, parsed->requestId, requesterName,
            "REQUEST", ndn::Buffer(wire.data(), wire.size()));
        for (size_t index = 0; index < environment.providerCount(); ++index) {
          environment.provider(index).cacheHybridReceiveKeyForTest(
              encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
        }
        publish(environment.userPubSub(), requestName, encrypted.wire);
        environment.markRequestPublished(scope);
      });

  RequestCapabilities capabilities;
  capabilities.setField("RequestScopedConfidentialityV1", "required");
  if (signedManifest)
    capabilities.setField(RequestCapabilities::RESPONSE_MANIFEST_V1_FIELD, "required");
  RequestMessage request;
  request.setRequestCapabilities(capabilities);
  ndn::Buffer input(reinterpret_cast<const uint8_t*>(inputText.data()), inputText.size());
  request.setPayload(input, input.size());
  const auto requestId = environment.user().RequestService(
      providers, serviceName, request,
      (revokeAfterExecution || stagingPolicy == StagingPolicy::REVOKE ||
       terminalDuringFetch == "timeout") ? 800 : (signedManifest ? 20000 : 5000),
      [&] (const ndn::Name&) {
        if (terminalDuringFetch == "timeout") BOOST_CHECK(observed.owner.expired());
        ++timeoutCallbacks;
        requestTimedOut = true;
      },
      [&] (const ResponseMessage& response) {
        responseError = response.getErrorInfo();
        const auto payload = response.getPayload();
        responseReceived = response.getStatus() &&
                           std::string(reinterpret_cast<const char*>(payload.data()),
                                       payload.size()) == responseText;
        ++responseCallbacks;
      },
      tlv::FirstResponding);
  BOOST_REQUIRE(!requestId.empty());

  if (terminalDuringFetch == "bad-root") {
    environment.pumpUntil([&] { return heldRoot.has_value() || requestTimedOut.load(); });
    BOOST_REQUIRE(heldRoot);
    const auto content = heldRoot->getContent().value_bytes();
    ndn::Buffer changed(content.begin(), content.end());
    BOOST_REQUIRE(!changed.empty());
    changed.back() ^= 1;
    heldRoot->setContent(std::move(changed));
    environment.userFace().receive(*heldRoot);
    environment.pumpUntil([&] { return responseCallbacks.load() != 0 || requestTimedOut.load(); });
    BOOST_CHECK(!requestTimedOut);
    BOOST_CHECK(!responseReceived);
    BOOST_CHECK_EQUAL(responseCallbacks.load(), 1);
    BOOST_CHECK_EQUAL(responseError,
                      "large response reference fetch failed: signed response manifest root was rejected");
    BOOST_CHECK_EQUAL(manifestRoots.size(), 1);
    BOOST_CHECK(manifestPages.empty());
    BOOST_CHECK(manifestSources.empty());
    BOOST_CHECK_EQUAL(environment.user().getPendingCallCount(), 0);
    environment.updateRequestResidue(scope, {});
    environment.resetRequest(scope);
    return;
  }

  if (!terminalDuringFetch.empty()) {
    environment.pumpUntil([&] {
      observed = test::LargeResponseFetchOracle::observe(environment.user());
      return heldSegments.load() != 0 &&
             ((terminalDuringFetch != "tamper" && terminalDuringFetch != "timeout") ||
              heldSecondSegment.has_value()) &&
             !observed.fetcher.expired() &&
             observed.authenticatedSegments == 1;
    });
    BOOST_REQUIRE_GT(heldSegments.load(), 0);
    BOOST_REQUIRE(!observed.owner.expired());
    BOOST_REQUIRE(!observed.fetcher.expired());
    BOOST_CHECK_EQUAL(observed.authenticatedSegments, 1);
    BOOST_CHECK_GT(observed.plaintextBytes, 0);
    BOOST_CHECK_LT(observed.plaintextBytes, responseText.size());
    BOOST_CHECK_EQUAL(observed.committedNonceKeys, 0);
    if (terminalDuringFetch == "timeout") {
      environment.pumpUntil([&] { return requestTimedOut.load(); });
      BOOST_CHECK_EQUAL(timeoutCallbacks.load(), 1);
      BOOST_CHECK_EQUAL(environment.user().getPendingCallCount(), 0);
    }
    else if (terminalDuringFetch == "cancel") {
      test::LargeResponseFetchOracle::cancelThroughSharedTerminalCleanup(
        environment.user(), requestId);
      BOOST_CHECK_EQUAL(environment.user().getPendingCallCount(), 0);
    }
    else if (terminalDuringFetch == "tamper") {
      auto changed = *heldSecondSegment;
      const auto [parsed, content] = ndn::Block::fromBuffer(changed.getContent().value_bytes());
      AeadEnvelope envelope;
      BOOST_REQUIRE(parsed && envelope.wireDecode(content));
      BOOST_REQUIRE(!envelope.tag.empty());
      envelope.tag.front() ^= 1;
      changed.setContent(envelope.wireEncode());
      const auto& name = changed.getName();
      const auto producer = std::find_if(providers.begin(), providers.end(), [&](const auto& id) {
        return id.isPrefixOf(name) && name.at(id.size()).toUri() == "NDNSF";
      });
      BOOST_REQUIRE(producer != providers.end());
      environment.keyChain().sign(changed, ndn::security::signingByIdentity(*producer));
      environment.userFace().receive(changed);
      environment.pumpUntil([&] { return responseCallbacks.load() != 0 || requestTimedOut.load(); });
      BOOST_CHECK(!responseReceived);
      BOOST_CHECK_EQUAL(responseError,
        "large response reference fetch failed: "
        "request-scoped large response decryption failed: authentication_failed");
    }
    else {
      BOOST_REQUIRE_EQUAL(terminalDuringFetch, "destroy");
      environment.destroyUserForTest();
    }
    releaseSegment = true;
    const auto stoppedInterestCount = segmentInterests.load();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    environment.pumpUntil([&] { return std::chrono::steady_clock::now() >= until; });
    BOOST_CHECK(observed.owner.expired());
    BOOST_CHECK(observed.fetcher.expired());
    BOOST_CHECK_EQUAL(segmentInterests.load(), stoppedInterestCount);
    BOOST_CHECK_EQUAL(responseCallbacks.load(), terminalDuringFetch == "tamper" ? 1 : 0);
    BOOST_CHECK_EQUAL(timeoutCallbacks.load(), terminalDuringFetch == "timeout" ? 1 : 0);
    if (terminalDuringFetch == "timeout") {
      const auto stalePacket = *heldSecondSegment;
      releaseSegment = false;
      heldSecondSegment.reset();
      bool nextTimedOut = false;
      std::size_t nextCallbacks = 0;
      std::string nextPayload;
      ndn::Name nextResponseName;
      const auto nextId = environment.user().RequestService(
        providers, serviceName, request, 5000,
        [&](const ndn::Name&) { nextTimedOut = true; },
        [&](const ResponseMessage& response) {
          ++nextCallbacks;
          BOOST_CHECK(response.getStatus());
          nextResponseName = ndn::Name(response.getDataName());
          const auto payload = response.getPayload();
          nextPayload.assign(payload.begin(), payload.end());
        }, tlv::FirstResponding);
      BOOST_REQUIRE(!nextId.empty());
      BOOST_REQUIRE(nextId != requestId);
      environment.pumpUntil([&] {
        observed = test::LargeResponseFetchOracle::observe(environment.user());
        return (heldSecondSegment && observed.authenticatedSegments == 1) || nextTimedOut;
      });
      BOOST_REQUIRE(!nextTimedOut);
      BOOST_REQUIRE(heldSecondSegment);
      BOOST_REQUIRE(heldSecondSegment->getName() != stalePacket.getName());
      BOOST_CHECK_EQUAL(observed.authenticatedSegments, 1);
      const auto firstChunkBytes = observed.plaintextBytes;
      environment.userFace().receive(stalePacket);
      const auto afterReplay = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      environment.pumpUntil([&] { return std::chrono::steady_clock::now() >= afterReplay; });
      auto after = test::LargeResponseFetchOracle::observe(environment.user());
      BOOST_CHECK(after.owner.lock() == observed.owner.lock());
      BOOST_CHECK_EQUAL(after.authenticatedSegments, 1);
      BOOST_CHECK_EQUAL(after.plaintextBytes, firstChunkBytes);
      BOOST_CHECK_EQUAL(after.committedNonceKeys, 0);
      BOOST_CHECK_EQUAL(nextCallbacks, 0);
      BOOST_CHECK_EQUAL(responseCallbacks.load(), 0);
      releaseSegment = true;
      environment.userFace().receive(*heldSecondSegment);
      environment.pumpUntil([&] { return nextCallbacks != 0 || nextTimedOut; });
      BOOST_CHECK(!nextTimedOut);
      BOOST_CHECK_EQUAL(nextCallbacks, 1);
      BOOST_CHECK_EQUAL(nextPayload, responseText);
      BOOST_CHECK_EQUAL(responseCallbacks.load(), 0);
      const auto responseName = parseResponseNameV2(stripTrailingResponseSegments(nextResponseName));
      BOOST_REQUIRE(responseName);
      BOOST_CHECK_EQUAL(responseName->requestId, nextId);
    }
    environment.updateRequestResidue(scope, {});
    environment.resetRequest(scope);
    return;
  }

  if (revokeAfterExecution) {
    // The Provider executed with the current-version material.  Only after
    // execution does the User accept a revoking status; the arriving response
    // (reference or inline) must then be refused before delivery or segment
    // decryption, and the invocation ends with exactly one terminal outcome.
    environment.pumpUntil([&] { return requestExecutions.load() >= 1; });
    auto revokedStatus = status;
    auto revokedVersion = status.getControllerVersion();
    ++revokedVersion.controllerEpoch;
    revokedStatus.setControllerVersion(revokedVersion);
    RevocationTarget revokedUser;
    revokedUser.kind = RevocationKind::IDENTITY;
    revokedUser.targetIdentity = ndn::Name(requesterName);
    revokedStatus.addRevocation(revokedUser);
    BOOST_REQUIRE(environment.user().installControllerStatus(revokedStatus));
  }

  environment.pumpUntil([&] {
    return responseReceived.load() || requestTimedOut.load() ||
           (!expectedResponseError.empty() && responseCallbacks.load() != 0);
  });

  if (revokeAfterExecution) {
    BOOST_CHECK(requestTimedOut);
    BOOST_CHECK(!responseReceived);
    BOOST_CHECK_EQUAL(requestExecutions.load(), 1U);
  }
  else if (stagingPolicy == StagingPolicy::REVOKE) {
    BOOST_CHECK(requestTimedOut);
    BOOST_CHECK_EQUAL(responseCallbacks.load(), 0U);
    BOOST_CHECK_EQUAL(segmentInterests.load(), 0U);
    BOOST_CHECK_EQUAL(timeoutCallbacks.load(), 1U);
    for (size_t index = 0; index < environment.providerCount(); ++index) {
      BOOST_CHECK_EQUAL(environment.provider(index).getSelectedOutstandingRequestCountForTesting(),
                        0U);
    }
  }
  else if (!expectedResponseError.empty()) {
    BOOST_CHECK(!requestTimedOut);
    BOOST_CHECK(!responseReceived);
    BOOST_CHECK_EQUAL(responseCallbacks.load(), 1U);
    BOOST_CHECK_EQUAL(responseError, expectedResponseError);
    BOOST_CHECK_EQUAL(segmentInterests.load(), 0U);
  }
  else {
    BOOST_CHECK(!requestTimedOut);
    BOOST_CHECK(responseReceived);
    if (signedManifest) {
      const auto count = largeResponseBytes == 8194 ? 4097 : 3;
      BOOST_CHECK_EQUAL(manifestRoots.size(), count == 4097 ? 2 : 1);
      BOOST_CHECK_EQUAL(manifestSources.size(), count);
      BOOST_CHECK_EQUAL(manifestPages.size(), count == 4097 ? 32 : 0);
    }
  }
  if (stagingPolicy != StagingPolicy::NONE)
    BOOST_CHECK(policyInstalledWhileStaging);
  // The old service-wide response-key carrier and its
  // NDNSF_REQUEST_SCOPED_COMPATIBILITY switch were removed (Spec179 T012);
  // the default path cannot activate them, structurally.  These runs
  // decrypt the Selection-envelope K_response AEAD end to end, and the
  // post-migration default is pinned by the unit regression
  // RequestScopedDefaultActivationWithConfiguredController.
  environment.updateRequestResidue(scope, {});
  environment.resetRequest(scope);
}

} // namespace

BOOST_AUTO_TEST_CASE(NormalResponseCompletesThroughProductionOnResponse)
{
  runFullRuntimeResponseCase(false);
}

BOOST_AUTO_TEST_CASE(LargeResponseCompletesThroughProductionOnResponse)
{
  runFullRuntimeResponseCase(true);
}

BOOST_AUTO_TEST_CASE(SignedManifestResponseAuthenticatesMultipleGroupsThroughProductionOnResponse)
{
  constexpr auto variable = "NDNSF_REQUEST_SCOPED_RESPONSE_CHUNK_BYTES";
  const char* current = std::getenv(variable);
  const auto previous = current ? std::optional<std::string>(current) : std::nullopt;
  BOOST_REQUIRE_EQUAL(::setenv(variable, "2", 1), 0);
  const auto restore = ndn::make_scope_exit([&] {
    if (previous)
      ::setenv(variable, previous->c_str(), 1);
    else
      ::unsetenv(variable);
  });
  runFullRuntimeResponseCase(true, false, {}, 8194, {}, StagingPolicy::NONE, true);
}

BOOST_AUTO_TEST_CASE(SignedManifestResponseCancelsWithPendingOriginals)
{
  runFullRuntimeResponseCase(true, false, "cancel", 9000, {}, StagingPolicy::NONE, true);
}

BOOST_AUTO_TEST_CASE(SignedManifestResponseRejectsChangedRootBeforeReadingMembers)
{
  runFullRuntimeResponseCase(true, false, "bad-root", 9000, {}, StagingPolicy::NONE, true);
}

BOOST_AUTO_TEST_CASE(RevokedUserCannotReceiveLargeResponseAfterProviderExecution)
{
  runFullRuntimeResponseCase(true, true);
}

BOOST_AUTO_TEST_CASE(LargeResponseAtActualFrameSizeCompletesWithZeroHandlerWorkers)
{
  runFullRuntimeResponseCase(true, false, {}, 1259436, {}, StagingPolicy::REFRESH);
}

BOOST_AUTO_TEST_CASE(LargeResponseCannotCommitAfterProviderRevocationDuringStaging)
{
  runFullRuntimeResponseCase(true, false, {}, 1259436, {}, StagingPolicy::REVOKE);
}

BOOST_AUTO_TEST_CASE(LargeResponsePreparationFailureCompletesThroughProductionOnResponse)
{
  constexpr auto chunkVariable = "NDNSF_REQUEST_SCOPED_RESPONSE_CHUNK_BYTES";
  const auto* value = std::getenv(chunkVariable);
  const auto previous = value ? std::optional<std::string>(value) : std::nullopt;
  BOOST_REQUIRE_EQUAL(::setenv(chunkVariable, "1", 1), 0);
  const auto restore = ndn::make_scope_exit([&] {
    if (previous) {
      ::setenv(chunkVariable, previous->c_str(), 1);
    }
    else {
      ::unsetenv(chunkVariable);
    }
  });
  runFullRuntimeResponseCase(true, false, {}, MAX_REQUEST_RESPONSE_SEGMENTS + 1,
                            "large response reference preparation failed: "
                            "request-scoped response packet file capacity");
}

BOOST_AUTO_TEST_CASE(LargeResponseTimeoutStopsFetcherBeforeApplicationTerminalCallback)
{
  runFullRuntimeResponseCase(true, false, "timeout");
}

BOOST_AUTO_TEST_CASE(LargeResponseCancellationCleanupStopsFetcherBeforeKeyErasure)
{
  runFullRuntimeResponseCase(true, false, "cancel");
}

BOOST_AUTO_TEST_CASE(LargeResponseUserDestructionStopsFetcherWithExternalFaceStillAlive)
{
  runFullRuntimeResponseCase(true, false, "destroy");
}

BOOST_AUTO_TEST_CASE(AuthenticatedLaterSegmentWithInvalidAeadCannotDeliverPartialResult)
{
  runFullRuntimeResponseCase(true, false, "tamper");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::integration_test
