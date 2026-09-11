// T005-B Requester Grant Publication (integration layer) — the real
// ServiceUser publication path under the canonical KEY-GRANT/v1 name plus
// an exact-name fetch from the Provider endpoint.  NativeGrantClient::acquire
// publishes through ServiceUser::publishSignedAppData (the Face publication
// port), the Provider expresses the exact-name fetch Interest and validates
// the returning Data signer and content.
//
// Scope boundary (recorded in the T005-B evidence): the authority issue port
// yields a frozen in-test grant because requester-signature/envelope/
// authority-signature *production* in C++ is wired at T010/T016; the
// verifier envelope and full Provider consumption therefore run at T016
// (manifest executeOwner).  This suite runs on the fixture's real
// DummyClientFace transport with the real keyChain and ServiceUser, exactly
// like the request-scoped exact-name flows.

#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantVerifier.hpp"
#include <openssl/evp.h>
#include <future>
#include "ndnsf-integration-fixture.hpp"

#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/pib/pib.hpp>
#include <openssl/sha.h>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ndnsf::di::tests {
namespace {

using namespace ndn_service_framework::test;

std::string
digest(const std::string& value)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);
  std::string result = "sha256:";
  for (const auto byte : hash) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}

std::uint64_t
nowMs()
{
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

NativeKeyGrant
frozenGrant(const NativeGrantRequest& request)
{
  return NativeKeyGrant{"unused", digest("grant"), request.providerIdentity,
                        "{\"grant\":1}", request.expiresAtMs};
}

// Provider-side stand-in for the exact-name fetch consumer: the same
// structural checks the real Provider consumption performs at T016
// (publication identity prefix, canonical KEY-GRANT layout, requester
// signer, byte-exact grant wire).
void
expectProviderConsumable(const ndn::Data& data, const std::string& publicationIdentity,
                         const ndn::Name& expectedSignerCert,
                         const std::string& expectedWire)
{
  const auto uri = data.getName().toUri();
  BOOST_CHECK_EQUAL(uri.substr(0, publicationIdentity.size()),
                    publicationIdentity);
  BOOST_CHECK(uri.find("/NDNSF-DI/KEY-GRANT/v1/PROVIDER/") != std::string::npos);
  BOOST_CHECK(uri.find("/REQ/") != std::string::npos);
  BOOST_CHECK(uri.find("/GRANT/" + digest("grant").substr(7)) != std::string::npos);
  BOOST_CHECK_EQUAL(data.getSignatureInfo().getKeyLocator().getName(),
                    expectedSignerCert);
  const auto& content = data.getContent();
  const std::string wire(reinterpret_cast<const char*>(content.value()),
                         content.value_size());
  BOOST_CHECK_EQUAL(wire, expectedWire);
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182GrantClientFlow)

BOOST_AUTO_TEST_CASE(RequesterAcquirePublishesSignedExactNameDataConsumedByProviderFetch)
{
  // Two endpoints over the fixture transport: the requester (ServiceUser)
  // acquires the grant and publishes it as signed APP Data under the exact
  // canonical name; the Provider endpoint fetches that exact name.
  NdnsfIntegrationEnvironment environment;
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);

  auto& requester = environment.user();
  auto& requesterFace = environment.userFace();
  auto& providerFace = environment.providerFace(0);

  // The requester identity ServiceUser is configured with; its default
  // certificate is the signer every signed APP Data must carry.
  const auto requesterIdentity = environment.profile().userIdentity;
  const auto requesterCertName = environment.keyChain()
    .getPib().getIdentity(requesterIdentity).getDefaultKey()
    .getDefaultCertificate().getName();
  const std::string requesterUri = requesterIdentity.toUri();

  // In-process policy authority + client whose Face publication port is the
  // real ServiceUser signed-APP-Data path (must stay under the requester's
  // /NDNSF/DI name space, which the canonical KEY-GRANT/v1 name satisfies).
  auto authority = std::make_shared<NativeArtifactPolicyAuthority>(frozenGrant);
  NativeGrantClient client(requesterUri, authority,
    [&] (const std::string& name, const std::string& wire) {
      requester.publishSignedAppData(ndn::Name(name),
        ndn::Buffer(reinterpret_cast<const uint8_t*>(wire.data()), wire.size()),
        ndn::time::milliseconds(60'000));
      return name;
    });

  NativeProviderGrantView view;
  view.provider = "/test/provider/spec170";
  view.role = "/role/0";
  view.planCoreDigest = digest("plan-core");
  view.policyDigest = digest("policy");
  view.modelDigest = digest("model");
  view.graphDigest = digest("graph");
  view.artifactDigest = digest("artifact");
  view.requesterIdentity = requesterUri;
  view.requestId = "/grant/request/1";
  view.attempt = 1;
  view.modelManifestDigest = digest("model-manifest");
  view.protectionEpoch = "epoch-1";
  view.expiresAtMs = nowMs() + 60'000;

  // publishSignedAppData puts the exact-name Data on the User face.
  std::optional<ndn::Data> publishedData;
  auto dataObserver = requesterFace.onSendData.connect(
    [&] (const ndn::Data& data) { publishedData = data; });
  const auto binding = client.acquire(
    view, std::chrono::system_clock::now() + std::chrono::seconds(5));
  // DummyClientFace emits the put only while its event loop runs.
  environment.pumpUntil([&] { return publishedData.has_value(); });
  BOOST_REQUIRE(publishedData.has_value());
  BOOST_CHECK_EQUAL(publishedData->getName().toUri(), binding.grantName);

  // Provider-side exact-name fetch.  DummyClientFace does not retain
  // unsolicited Data, so replay the captured packet when the fetch Interest
  // is expressed (the request-scoped exact flows use the same relay).
  std::optional<ndn::Data> fetchedData;
  bool fetchTimedOut = false;
  auto interestRelay = providerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) {
      if (!publishedData || interest.getName() != publishedData->getName()) {
        return;
      }
      providerFace.receive(*publishedData);
    });
  const ndn::Interest fetchInterest(binding.grantName);
  providerFace.expressInterest(fetchInterest,
    [&] (const ndn::Interest&, const ndn::Data& data) { fetchedData = data; },
    [] (const ndn::Interest&, const ndn::lp::Nack&) {},
    [&] (const ndn::Interest&) { fetchTimedOut = true; });
  environment.pumpUntil([&] { return fetchedData.has_value() || fetchTimedOut; });

  BOOST_REQUIRE_MESSAGE(fetchedData.has_value(), "exact-name grant fetch failed");
  BOOST_CHECK(!fetchTimedOut);
  BOOST_CHECK_EQUAL(fetchedData->getName().toUri(), binding.grantName);
  expectProviderConsumable(*fetchedData, requesterUri, requesterCertName,
                           binding.wireJson);
  BOOST_CHECK_EQUAL(binding.provider, view.provider);
  BOOST_CHECK_EQUAL(binding.grantDigest, digest("grant"));
}

// Authored during R2-B4; run by T016, after final implementation convergence.
BOOST_AUTO_TEST_CASE(ConcreteIssuerUsesCoreWorkerPublicationAndProviderUnwrap)
{
  NdnsfIntegrationEnvironment environment;
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  const auto key = [](char value) {
    const std::string seed(32, value);
    return std::shared_ptr<EVP_PKEY>(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
      reinterpret_cast<const unsigned char*>(seed.data()), seed.size()), EVP_PKEY_free);
  };
  const auto requester = environment.profile().userIdentity.toUri();
  NativeGrantIssuerConfig config;
  config.requesterIdentity = requester; config.authorityIdentity = "/authority";
  config.protectionEpoch = "epoch-1"; config.keyId = "public-test-key";
  config.authorityPrivateKey = key('a'); config.requesterPublicKey = key('b');
  config.allowedModelManifests = {digest("model-manifest")};
  config.recipientPublicKeys = {{"/provider/test", key('c')}};
  config.contentKey = [](const auto&, const auto&) { return std::vector<std::uint8_t>(32, 42); };
  NativeSignedGrantRequest request;
  request.requesterIdentity = requester; request.providerIdentity = "/provider/test";
  request.requestId = "/grant/actual"; request.attempt = 1;
  request.planCoreDigest = digest("core"); request.grantViewDigest = digest("view");
  request.modelManifestDigest = digest("model-manifest"); request.protectionEpoch = "epoch-1";
  request.issuedAtMs = nowMs(); request = request.sign(*key('b'));
  const auto grant = NativeArtifactGrantIssuer(config).issue(request, nowMs(), nowMs() + 60000);
  // The fixture owns ServiceUser until the bounded future is joined below.
  auto user = std::shared_ptr<ndn_service_framework::ServiceUser>(&environment.user(), [](auto*) {});
  const auto publish = NativeAuthenticatedGrantClient::publishThroughCore(user);
  NativeGrantControl control{std::chrono::system_clock::now() + std::chrono::seconds(3), {}};
  std::optional<ndn::Data> data;
  auto observer = environment.userFace().onSendData.connect([&](const ndn::Data& value) {
    if (value.getName().toUri() == grant.grantName) data = value;
  });
  auto result = std::async(std::launch::async, [&] { return publish(grant.grantName, grant.wireJson, control); });
  environment.pumpUntil([&] {
    return data && result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  BOOST_CHECK_EQUAL(result.get(), grant.grantName);
  BOOST_REQUIRE(data);
  BOOST_CHECK(data->getFreshnessPeriod() > ndn::time::milliseconds::zero());
  const auto& content = data->getContent();
  const std::string wire(reinterpret_cast<const char*>(content.value()), content.value_size());
  std::string authorityPublic(32, '\0'); std::size_t size = authorityPublic.size();
  BOOST_REQUIRE_EQUAL(EVP_PKEY_get_raw_public_key(config.authorityPrivateKey.get(),
    reinterpret_cast<unsigned char*>(authorityPublic.data()), &size), 1);
  const auto opened = verifyAndUnwrapNativeGrant(wire, authorityPublic,
    {NativeRecipientKey::Kind::Ed25519Seed, std::string(32, 'c')}, request.providerIdentity,
    request.requestId, request.attempt, request.planCoreDigest, request.modelManifestDigest,
    request.protectionEpoch, nowMs(), config.authorityIdentity, grant.grantDigest);
  BOOST_REQUIRE_MESSAGE(opened.verified, opened.reason);
  BOOST_CHECK(opened.contentKey == std::vector<std::uint8_t>(32, 42));
}

BOOST_AUTO_TEST_CASE(Spec184AuthorityIoOwnership)
{
  NdnsfIntegrationEnvironment environment;
  environment.bootstrap();
  environment.user().setUseTokens(false);
  environment.provider().setUseTokens(false);

  const auto authorityIdentity = environment.provider().getName();
  const auto authorityService = environment.profile().serviceName;
  const NativeKeyGrant expectedGrant{
    "/grant/spec184/1", "sha256:spec184-grant", "/provider/spec184",
    "{\"grant\":1}", nowMs() + 60'000};
  std::mutex captureMutex;
  std::optional<NativeGrantAuthorityRequest> receivedRequest;
  environment.provider().addTargetedService(
    authorityService,
    [&] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
         const ndn::Name&, const ndn_service_framework::RequestMessage& request) {
      const auto payload = request.getPayload();
      const std::string wire(reinterpret_cast<const char*>(payload.data()), payload.size());
      NativeGrantAuthorityRequest decoded;
      try {
        decoded = nativeGrantAuthorityRequestFromJson(wire);
      }
      catch (...) {
        throw std::runtime_error("Spec184 authority request was not canonical");
      }
      {
        std::lock_guard<std::mutex> lock(captureMutex);
        receivedRequest = decoded;
      }
      ndn_service_framework::ResponseMessage response;
      response.setStatus(true);
      const auto responseWire = nativeKeyGrantJson(expectedGrant);
      ndn::Buffer responsePayload(
        reinterpret_cast<const uint8_t*>(responseWire.data()), responseWire.size());
      response.setPayload(responsePayload, responsePayload.size());
      return response;
    });
  // Register the targeted handler before init() installs the provider's
  // Interest filters; production ingress snapshots the service table at init.
  environment.enableProductionIngressForTest();

  std::mutex sendMutex;
  std::thread::id sendThread;
  std::atomic<bool> authorityInterestSeen{false};
  auto interestObserver = environment.userFace().onSendInterest.connect(
    [&] (const ndn::Interest&) {
      if (authorityInterestSeen.exchange(true)) return;
      std::lock_guard<std::mutex> lock(sendMutex);
      sendThread = std::this_thread::get_id();
      BOOST_CHECK(environment.user().isOnIoThread());
    });

  auto user = std::shared_ptr<ndn_service_framework::ServiceUser>(
    &environment.user(), [] (ndn_service_framework::ServiceUser*) {});
  auto issue = NativeAuthenticatedGrantClient::issueThroughCore(
    user, authorityIdentity.toUri(), authorityService.toUri());
  NativeSignedGrantRequest request;
  request.requesterIdentity = environment.profile().userIdentity.toUri();
  request.providerIdentity = "/provider/spec184";
  request.requestId = "/request/spec184/1";
  request.attempt = 1;
  request.planCoreDigest = digest("spec184-plan");
  request.grantViewDigest = digest("spec184-view");
  request.modelManifestDigest = digest("spec184-model");
  request.protectionEpoch = "spec184-epoch";
  request.issuedAtMs = nowMs();
  NativeGrantControl control{std::chrono::system_clock::now() + std::chrono::seconds(5), {}};
  std::thread::id workerThread;
  auto result = std::async(std::launch::async, [&] {
    workerThread = std::this_thread::get_id();
    return issue(request, "{\"manifest\":1}", nowMs() + 60'000, control);
  });

  environment.pumpUntil([&] {
    std::lock_guard<std::mutex> lock(captureMutex);
    return receivedRequest.has_value() &&
           result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  const auto granted = result.get();
  BOOST_CHECK_EQUAL(granted.grantName, expectedGrant.grantName);
  BOOST_CHECK_EQUAL(granted.grantDigest, expectedGrant.grantDigest);
  BOOST_CHECK(authorityInterestSeen.load());
  {
    std::lock_guard<std::mutex> lock(captureMutex);
    BOOST_REQUIRE(receivedRequest.has_value());
    BOOST_CHECK_EQUAL(receivedRequest->publishedManifestJson, "{\"manifest\":1}");
    BOOST_CHECK_EQUAL(receivedRequest->request.requestId, request.requestId);
  }
  {
    std::lock_guard<std::mutex> lock(sendMutex);
    BOOST_CHECK_NE(sendThread, workerThread);
  }
  BOOST_CHECK_EQUAL(environment.user().getPendingCallCount(), 0);
}

BOOST_AUTO_TEST_CASE(Spec184AuthorityDispatchCancellationAndException)
{
  NdnsfIntegrationEnvironment environment;
  environment.bootstrap();
  environment.enableProductionIngressForTest();
  environment.user().setUseTokens(false);

  auto user = std::shared_ptr<ndn_service_framework::ServiceUser>(
    &environment.user(), [] (ndn_service_framework::ServiceUser*) {});
  NativeSignedGrantRequest request;
  request.requesterIdentity = environment.profile().userIdentity.toUri();
  request.providerIdentity = "/provider/spec184";
  request.requestId = "/request/spec184/dispatch";
  request.attempt = 1;
  request.planCoreDigest = digest("spec184-plan");
  request.grantViewDigest = digest("spec184-view");
  request.modelManifestDigest = digest("spec184-model");
  request.protectionEpoch = "spec184-epoch";
  request.issuedAtMs = nowMs();

  auto cancelled = std::make_shared<std::atomic<bool>>(true);
  auto issue = NativeAuthenticatedGrantClient::issueThroughCore(
    user, environment.provider().getName().toUri(),
    environment.profile().serviceName.toUri());
  auto cancelledResult = std::async(std::launch::async, [&] {
    return issue(request, "{\"manifest\":1}", nowMs() + 60'000,
                 NativeGrantControl{std::chrono::system_clock::now() + std::chrono::seconds(5), cancelled});
  });
  BOOST_CHECK_EXCEPTION(cancelledResult.get(), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("cancelled") != std::string::npos;
    });
  BOOST_CHECK_EQUAL(environment.user().getPendingCallCount(), 0);

  // An invalid authority identity is rejected by RequestServiceTargeted on
  // the Core IO owner (the local mock returns an empty request ID). The worker
  // must observe that rejection through Pending rather than waiting until its
  // outer deadline.
  auto malformedIssue = NativeAuthenticatedGrantClient::issueThroughCore(
    user, "/bad/%ZZ", environment.profile().serviceName.toUri());
  NativeGrantControl control{std::chrono::system_clock::now() + std::chrono::seconds(5), {}};
  auto malformedResult = std::async(std::launch::async, [&] {
    return malformedIssue(request, "{\"manifest\":1}", nowMs() + 60'000, control);
  });
  environment.pumpUntil([&] {
    return malformedResult.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  bool malformedExceptionObserved = false;
  try {
    static_cast<void>(malformedResult.get());
  }
  catch (const std::exception& error) {
    malformedExceptionObserved = true;
    BOOST_TEST_MESSAGE("malformed authority identity propagated: " << error.what());
  }
  BOOST_CHECK(malformedExceptionObserved);
  BOOST_CHECK_EQUAL(environment.user().getPendingCallCount(), 0);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
