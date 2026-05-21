#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace {

class ObjectDetectionRequest
{
public:
  void
  setImage(std::string image)
  {
    m_image = std::move(image);
  }

  const std::string&
  getImage() const
  {
    return m_image;
  }

  bool
  SerializeToString(std::string* out) const
  {
    *out = m_image;
    return true;
  }

  bool
  ParseFromArray(const void* data, size_t size)
  {
    m_image.assign(static_cast<const char*>(data), size);
    return true;
  }

private:
  std::string m_image;
};

class ObjectDetectionResponse
{
public:
  void
  setLabel(std::string label)
  {
    m_label = std::move(label);
  }

  const std::string&
  getLabel() const
  {
    return m_label;
  }

  bool
  SerializeToString(std::string* out) const
  {
    *out = m_label;
    return true;
  }

  bool
  ParseFromArray(const void* data, size_t size)
  {
    m_label.assign(static_cast<const char*>(data), size);
    return true;
  }

private:
  std::string m_label;
};

ndn_service_framework::PermissionResponse
makePermissionResponse(const ndn::Name& targetIdentity,
                       size_t permissionKind,
                       const ndn::Name& providerName,
                       const ndn::Name& serviceName)
{
  ndn_service_framework::PermissionEntry entry;
  entry.setProviderName(providerName.toUri());
  entry.setServiceName(serviceName.toUri());
  entry.setToken("");
  entry.setTtl(0);
  entry.setVersion(1);

  ndn_service_framework::PermissionResponse response;
  response.setTargetIdentity(targetIdentity.toUri());
  response.setPermissionKind(permissionKind);
  response.addEntry(entry);
  return response;
}

ndn::security::Certificate
makeIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
{
  auto id = keyChain.createIdentity(identity, ndn::RsaKeyParams(2048));
  return id.getDefaultKey().getDefaultCertificate();
}

} // namespace

int
main()
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:generic-dynamic-example",
                                   "tpm-memory:generic-dynamic-example");

  auto userCert = makeIdentity(keyChain, ndn::Name("/example/user"));
  auto providerCert = makeIdentity(keyChain, ndn::Name("/example/provider"));
  auto aaCert = makeIdentity(keyChain, ndn::Name("/example/aa"));

  ndn_service_framework::ServiceUser user(
    ndn_service_framework::ServiceUser::LocalMockTag{},
    face,
    ndn::Name("/example/group"),
    userCert,
    aaCert,
    "examples/trust-any.conf");

  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{},
    face,
    ndn::Name("/example/group"),
    providerCert,
    aaCert,
    "examples/trust-any.conf");
  user.setHandlerThreads(0);

  const ndn::Name serviceName("/ObjectDetection/YOLOv8");
  user.applyPermissionResponse(
    makePermissionResponse(user.getName(),
                           ndn_service_framework::tlv::UserPermission,
                           provider.getName(),
                           serviceName));
  provider.applyPermissionResponse(
    makePermissionResponse(provider.getName(),
                           ndn_service_framework::tlv::ProviderPermission,
                           provider.getName(),
                           serviceName));

  provider.RegisterService<ObjectDetectionRequest, ObjectDetectionResponse>(
    serviceName,
    std::function<void(const ndn::Name&,
                       const ObjectDetectionRequest&,
                       ObjectDetectionResponse&)>(
      [](const ndn::Name& requester,
         const ObjectDetectionRequest& request,
         ObjectDetectionResponse& response) {
        std::cout << "provider received " << request.getImage()
                  << " from " << requester << "\n";
        response.setLabel("person");
      }));

  user.setRequestPublisher(
    [&](const ndn::Name& requestId,
        const ndn::Name& requestName,
        const std::vector<ndn::Name>&,
        const ndn::Name&,
        const ndn_service_framework::RequestMessage& requestMessage,
        size_t) {
      auto response = provider.handleDecryptedRequestByName(requestName, requestMessage);

      auto parsedRequest = ndn_service_framework::parseRequestNameV2(requestName);
      if (!parsedRequest) {
        std::cerr << "failed to parse request name " << requestName << "\n";
        return;
      }

      auto responseName = ndn_service_framework::makeResponseNameV2(
        provider.getName(),
        parsedRequest->requesterName,
        parsedRequest->serviceName,
        requestId);

      response.setUserToken(requestMessage.getUserToken());
      user.handleDecryptedResponseByName(responseName, response);
    });

  bool gotResponse = false;
  ObjectDetectionRequest request;
  request.setImage("local-frame");

  user.AsyncCall<ObjectDetectionRequest, ObjectDetectionResponse>(
    serviceName,
    request,
    100,
    ndnsf::strategy::FirstResponding,
    1000,
    std::function<void(const ObjectDetectionResponse&)>(
      [&](const ObjectDetectionResponse& response) {
        gotResponse = true;
        std::cout << "user received label " << response.getLabel() << "\n";
      }),
    std::function<void(const ndn::Name&)>([](const ndn::Name&) {
      std::cerr << "request timed out\n";
    }));

  return gotResponse ? 0 : 1;
}
