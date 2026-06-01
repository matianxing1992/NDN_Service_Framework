#include "tests/boost-test.hpp"

#include "ndn-service-framework/NDNSFMessages.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/utils.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-svs/security-options.hpp>
#include <ndn-svs/svspubsub.hpp>

#include <boost/asio/io_context.hpp>

#include <string>
#include <vector>

namespace ndn_service_framework::test {
namespace {

ndn::svs::SecurityOptions
makeTestSecurityOptions(ndn::KeyChain& keyChain)
{
  ndn::svs::SecurityOptions options(keyChain);
  options.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
  options.dataSigner->signingInfo = ndn::security::signingWithSha256();
  options.pubSigner->signingInfo = ndn::security::signingWithSha256();
  options.validator = std::make_shared<ndn::svs::BaseValidator>();
  options.encapsulatedDataValidator = std::make_shared<ndn::svs::BaseValidator>();
  return options;
}

void
pumpFaces(ndn::DummyClientFace& faceA, ndn::DummyClientFace& faceB, const std::function<bool()>& done)
{
  for (int i = 0; i < 200 && !done(); ++i) {
    faceA.processEvents(ndn::time::milliseconds(5));
    faceB.processEvents(ndn::time::milliseconds(5));
    faceA.getIoContext().restart();
    faceB.getIoContext().restart();
  }
}

} // namespace

class DynamicRequest
{
public:
  void
  setPayload(std::string value)
  {
    payload = std::move(value);
  }

  const std::string&
  getPayload() const
  {
    return payload;
  }

  bool
  SerializeToString(std::string* out) const
  {
    *out = payload;
    return true;
  }

  bool
  ParseFromArray(const void* data, size_t size)
  {
    payload.assign(static_cast<const char*>(data), size);
    return true;
  }

private:
  std::string payload;
};

class DynamicResponse
{
public:
  void
  setLabel(std::string value)
  {
    label = std::move(value);
  }

  const std::string&
  getLabel() const
  {
    return label;
  }

  bool
  SerializeToString(std::string* out) const
  {
    *out = label;
    return true;
  }

  bool
  ParseFromArray(const void* data, size_t size)
  {
    label.assign(static_cast<const char*>(data), size);
    return true;
  }

private:
  std::string label;
};

class LocalSvsServiceUser : public ServiceUser
{
public:
  LocalSvsServiceUser(ndn::Face& face,
                      const ndn::Name& groupPrefix,
                      const ndn::security::Certificate& identityCert,
                      const ndn::security::Certificate& attrAuthorityCertificate,
                      const std::string& trustSchemaPath)
    : ServiceUser(LocalMockTag{},
                  face,
                  groupPrefix,
                  identityCert,
                  attrAuthorityCertificate,
                  trustSchemaPath)
  {
  }

  size_t
  getPendingRequestAckCount(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      return 0;
    }
    return pending->second.requestAcks.size();
  }

  ndn::Name
  getSelectedProvider(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      return ndn::Name();
    }
    return pending->second.selectedProvider;
  }
};

ndn::security::Certificate
makeRsaIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
{
  auto id = keyChain.createIdentity(identity, ndn::RsaKeyParams(2048));
  return id.getDefaultKey().getDefaultCertificate();
}

PermissionResponse
makePermissionResponse(const ndn::Name& targetIdentity,
                       size_t permissionKind,
                       const ndn::Name& providerName,
                       const ndn::Name& serviceName)
{
  PermissionEntry entry;
  entry.setProviderName(providerName.toUri());
  entry.setServiceName(serviceName.toUri());
  entry.setToken("");
  entry.setTtl(0);
  entry.setVersion(1);

  PermissionResponse response;
  response.setTargetIdentity(targetIdentity.toUri());
  response.setPermissionKind(permissionKind);
  response.addEntry(entry);
  return response;
}

void
installPermissions(LocalSvsServiceUser& user,
                   ServiceProvider& provider,
                   const ndn::Name& requesterName,
                   const ndn::Name& serviceName)
{
  const ndn::Name providerName = provider.getName();
  user.applyPermissionResponse(
    makePermissionResponse(requesterName,
                           tlv::UserPermission,
                           providerName,
                           serviceName));
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));
}

BOOST_AUTO_TEST_SUITE(NdnSvsSmoke)

BOOST_AUTO_TEST_CASE(DummyFacesDeliverV2RequestPublication)
{
  boost::asio::io_context ioA;
  boost::asio::io_context ioB;
  ndn::KeyChain keyChain("pib-memory:ndn-svs-smoke", "tpm-memory:ndn-svs-smoke");

  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;

  ndn::DummyClientFace faceA(ioA, keyChain, faceOptions);
  ndn::DummyClientFace faceB(ioB, keyChain, faceOptions);

  auto securityOptions = makeTestSecurityOptions(keyChain);
  ndn::svs::SVSPubSubOptions svsOptions;
  svsOptions.useTimestamp = false;

  const ndn::Name syncPrefix("/ndnsf/svs-smoke/sync");
  const ndn::Name userNode("/ndnsf/svs-smoke/user-node");
  const ndn::Name providerNode("/ndnsf/svs-smoke/provider-node");

  ndn::svs::SVSPubSub userPubSub(syncPrefix,
                                 userNode,
                                 faceA,
                                 [] (const std::vector<ndn::svs::MissingDataInfo>&) {},
                                 svsOptions,
                                 securityOptions);
  ndn::svs::SVSPubSub providerPubSub(syncPrefix,
                                     providerNode,
                                     faceB,
                                     [] (const std::vector<ndn::svs::MissingDataInfo>&) {},
                                     svsOptions,
                                     securityOptions);

  auto forwardAInterest = faceA.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { faceB.receive(interest); });
  auto forwardBInterest = faceB.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { faceA.receive(interest); });
  auto forwardAData = faceA.onSendData.connect(
    [&] (const ndn::Data& data) { faceB.receive(data); });
  auto forwardBData = faceB.onSendData.connect(
    [&] (const ndn::Data& data) { faceA.receive(data); });

  const ndn::Name requester("/test/user/alice");
  const ndn::Name serviceName("/ObjectDetection/YOLOv8");
  const ndn::Name requestId("/request-1");
  const auto requestName = makeRequestNameV2(requester, serviceName, requestId);

  RequestMessage requestMessage;
  ndn::Buffer requestPayload;
  const std::string payloadText = "frame-bytes";
  requestPayload.insert(requestPayload.end(), payloadText.begin(), payloadText.end());
  requestMessage.setUserToken("user-token");
  requestMessage.setPayload(requestPayload, requestPayload.size());
  requestMessage.setStrategy(ndn_service_framework::tlv::FirstResponding);

  const auto requestBlock = requestMessage.WireEncode();
  const std::vector<uint8_t> expectedPayload(requestBlock.data(),
                                             requestBlock.data() + requestBlock.size());

  bool received = false;
  ndn::Name receivedName;
  ndn::Name receivedProducerPrefix;
  std::vector<uint8_t> receivedPayload;

  providerPubSub.subscribeToProducer(
    userNode,
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      received = true;
      receivedName = publication.name;
      receivedProducerPrefix = publication.producerPrefix;
      receivedPayload.assign(publication.data.begin(), publication.data.end());
    },
    false);

  userPubSub.publish(requestName,
                     ndn::span<const uint8_t>(expectedPayload.data(), expectedPayload.size()));

  pumpFaces(faceA, faceB, [&] { return received; });

  if (!received) {
    BOOST_TEST_MESSAGE("faceA sent Interests: " << faceA.sentInterests.size());
    for (const auto& interest : faceA.sentInterests) {
      BOOST_TEST_MESSAGE("  A Interest " << interest.getName());
    }
    BOOST_TEST_MESSAGE("faceA sent Data: " << faceA.sentData.size());
    for (const auto& data : faceA.sentData) {
      BOOST_TEST_MESSAGE("  A Data " << data.getName());
    }
    BOOST_TEST_MESSAGE("faceB sent Interests: " << faceB.sentInterests.size());
    for (const auto& interest : faceB.sentInterests) {
      BOOST_TEST_MESSAGE("  B Interest " << interest.getName());
    }
    BOOST_TEST_MESSAGE("faceB sent Data: " << faceB.sentData.size());
    for (const auto& data : faceB.sentData) {
      BOOST_TEST_MESSAGE("  B Data " << data.getName());
    }
  }

  BOOST_REQUIRE(received);
  BOOST_CHECK_EQUAL(receivedName, requestName);
  BOOST_CHECK_EQUAL_COLLECTIONS(receivedPayload.begin(), receivedPayload.end(),
                                expectedPayload.begin(), expectedPayload.end());
  BOOST_CHECK_EQUAL(receivedProducerPrefix, userNode);
}

BOOST_AUTO_TEST_CASE(ServiceUserRequestServiceReachesProviderAndReturnsResponse)
{
  boost::asio::io_context ioA;
  boost::asio::io_context ioB;
  ndn::KeyChain keyChain("pib-memory:ndnsf-face-svs-flow", "tpm-memory:ndnsf-face-svs-flow");

  ndn::DummyClientFace::Options faceOptions;
  faceOptions.enablePacketLogging = true;
  faceOptions.enableRegistrationReply = true;

  ndn::DummyClientFace userFace(ioA, keyChain, faceOptions);
  ndn::DummyClientFace providerFace(ioB, keyChain, faceOptions);

  auto securityOptions = makeTestSecurityOptions(keyChain);
  ndn::svs::SVSPubSubOptions svsOptions;
  svsOptions.useTimestamp = false;

  const ndn::Name syncPrefix("/ndnsf/svs-flow/sync");
  const ndn::Name userNode("/ndnsf/svs-flow/user-node");
  const ndn::Name providerNode("/ndnsf/svs-flow/provider-node");

  ndn::svs::SVSPubSub userPubSub(syncPrefix,
                                 userNode,
                                 userFace,
                                 [] (const std::vector<ndn::svs::MissingDataInfo>&) {},
                                 svsOptions,
                                 securityOptions);
  ndn::svs::SVSPubSub providerPubSub(syncPrefix,
                                     providerNode,
                                     providerFace,
                                     [] (const std::vector<ndn::svs::MissingDataInfo>&) {},
                                     svsOptions,
                                     securityOptions);

  auto forwardUserInterest = userFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { providerFace.receive(interest); });
  auto forwardProviderInterest = providerFace.onSendInterest.connect(
    [&] (const ndn::Interest& interest) { userFace.receive(interest); });
  auto forwardUserData = userFace.onSendData.connect(
    [&] (const ndn::Data& data) { providerFace.receive(data); });
  auto forwardProviderData = providerFace.onSendData.connect(
    [&] (const ndn::Data& data) { userFace.receive(data); });

  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/test/user/alice"));
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/test/provider/camera"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));

  LocalSvsServiceUser user(userFace,
                           ndn::Name("/test/group"),
                           userCert,
                           aaCert,
                           "examples/trust-any.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{},
                           providerFace,
                           ndn::Name("/test/group"),
                           providerCert,
                           aaCert,
                           "examples/trust-any.conf");

  const ndn::Name serviceName("/ObjectDetection/YOLOv8");
  installPermissions(user,
                     provider,
                     ndn::Name("/test/user/alice"),
                     serviceName);
  bool requestPublished = false;
  bool providerReceived = false;
  bool handlerCalled = false;
  bool ackPublished = false;
  bool userReceivedAck = false;
  bool responsePublished = false;
  bool userReceived = false;
  bool typedCallbackCalled = false;
  size_t ackCountBeforeAck = 0;
  size_t ackCountAfterAck = 0;
  ndn::Name selectedProviderAfterAck;

  provider.addHandler<DynamicRequest, DynamicResponse>(
    serviceName,
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [&] (const ndn::Name& requester, const DynamicRequest& request, DynamicResponse& response) {
        BOOST_CHECK_EQUAL(requester, ndn::Name("/test/user/alice"));
        BOOST_CHECK_EQUAL(request.getPayload(), "frame-bytes");
        handlerCalled = true;
        response.setLabel("person");
      }));

  providerPubSub.subscribeToProducer(
    userNode,
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      auto parsedRequest = parseRequestNameV2(publication.name);
      if (!parsedRequest || parsedRequest->serviceName != serviceName) {
        return;
      }

      providerReceived = true;
      ndn::Block requestBlock(publication.data);
      RequestMessage publishedRequest;
      BOOST_CHECK(publishedRequest.WireDecode(requestBlock));
      auto response = provider.handleDecryptedRequestByName(publication.name, requestBlock);
      BOOST_CHECK(response.getStatus());

      RequestAckMessage ack;
      ack.setStatus(true);
      ack.setMessage("Permission Granted");
      ack.setUserToken(publishedRequest.getUserToken());
      ack.setProviderToken("provider-token");
      const auto ackName = makeRequestAckNameV2(provider.getName(),
                                                parsedRequest->requesterName,
                                                parsedRequest->serviceName,
                                                parsedRequest->requestId);
      auto ackBlock = ack.WireEncode();
      providerPubSub.publish(ackName,
                             ndn::span<const uint8_t>(ackBlock.data(), ackBlock.size()));
      ackPublished = true;

      const auto responseName = makeResponseNameV2(provider.getName(),
                                                   parsedRequest->requesterName,
                                                   parsedRequest->serviceName,
                                                   parsedRequest->requestId);
      auto responseBlock = response.WireEncode();
      providerPubSub.publish(responseName,
                             ndn::span<const uint8_t>(responseBlock.data(), responseBlock.size()));
      responsePublished = true;
    },
    true);

  userPubSub.subscribeToProducer(
    providerNode,
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      auto parsedAck = parseRequestAckNameV2(publication.name);
      if (parsedAck && parsedAck->serviceName == serviceName) {
        userReceivedAck = true;
        ackCountBeforeAck = user.getPendingRequestAckCount(parsedAck->requestId);
        ndn::Block ackBlock(publication.data);
        BOOST_CHECK(user.handleRequestAckByName(publication.name, ackBlock));
        ackCountAfterAck = user.getPendingRequestAckCount(parsedAck->requestId);
        selectedProviderAfterAck = user.getSelectedProvider(parsedAck->requestId);
        return;
      }

      auto parsedResponse = parseResponseNameV2(publication.name);
      if (!parsedResponse || parsedResponse->serviceName != serviceName) {
        return;
      }

      userReceived = true;
      ndn::Block responseBlock(publication.data);
      BOOST_CHECK(user.handleDecryptedResponseByName(publication.name, responseBlock));
    },
    true);

  user.setRequestPublisher(
    [&] (const ndn::Name&,
         const ndn::Name& requestName,
         const std::vector<ndn::Name>& providers,
         const ndn::Name& publishedServiceName,
         const RequestMessage& requestMessage,
         size_t strategy) {
      BOOST_REQUIRE_EQUAL(providers.size(), 1);
      BOOST_CHECK_EQUAL(providers.front(), provider.getName());
      BOOST_CHECK_EQUAL(publishedServiceName, serviceName);
      BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);

      auto requestBlock = requestMessage.WireEncode();
      userPubSub.publish(requestName,
                         ndn::span<const uint8_t>(requestBlock.data(), requestBlock.size()));
      requestPublished = true;
    });

  DynamicRequest request;
  request.setPayload("frame-bytes");

  const auto requestId = user.RequestService<DynamicRequest, DynamicResponse>(
    {provider.getName()},
    serviceName,
    request,
    std::function<void(const DynamicResponse&)>(
      [&] (const DynamicResponse& response) {
        BOOST_CHECK_EQUAL(response.getLabel(), "person");
        typedCallbackCalled = true;
      }),
    std::function<void()>([] {
      BOOST_FAIL("Face/SVS dynamic API test should not time out");
    }),
    1000,
    tlv::FirstResponding);

  BOOST_CHECK(!requestId.empty());
  pumpFaces(userFace, providerFace, [&] { return typedCallbackCalled; });

  BOOST_CHECK(requestPublished);
  BOOST_CHECK(providerReceived);
  BOOST_CHECK(handlerCalled);
  BOOST_CHECK(ackPublished);
  BOOST_CHECK(userReceivedAck);
  BOOST_CHECK_EQUAL(ackCountAfterAck, ackCountBeforeAck + 1);
  BOOST_CHECK_EQUAL(selectedProviderAfterAck, provider.getName());
  BOOST_CHECK(responsePublished);
  BOOST_CHECK(userReceived);
  BOOST_CHECK(typedCallbackCalled);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
