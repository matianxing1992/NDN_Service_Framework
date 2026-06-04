#include "ndn-service-framework/ServiceContainer.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace {

class ImageRequest
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

class LabelResponse
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

class PreprocessResponse
{
public:
  void
  setNormalizedImage(std::string value)
  {
    m_normalizedImage = std::move(value);
  }

  const std::string&
  getNormalizedImage() const
  {
    return m_normalizedImage;
  }

  bool
  SerializeToString(std::string* out) const
  {
    *out = m_normalizedImage;
    return true;
  }

  bool
  ParseFromArray(const void* data, size_t size)
  {
    m_normalizedImage.assign(static_cast<const char*>(data), size);
    return true;
  }

private:
  std::string m_normalizedImage;
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
  ndn::security::KeyChain keyChain("pib-memory:service-container-example",
                                   "tpm-memory:service-container-example");

  auto userCert = makeIdentity(keyChain, ndn::Name("/example/user"));
  auto providerCert = makeIdentity(keyChain, ndn::Name("/example/provider"));
  auto aaCert = makeIdentity(keyChain, ndn::Name("/example/aa"));

  auto user = std::make_shared<ndn_service_framework::ServiceUser>(
    ndn_service_framework::ServiceUser::LocalMockTag{},
    face,
    ndn::Name("/example/group"),
    userCert,
    aaCert,
    "examples/trust-schema.conf");

  auto provider = std::make_shared<ndn_service_framework::ServiceProvider>(
    ndn_service_framework::ServiceProvider::LocalMockTag{},
    face,
    ndn::Name("/example/group"),
    providerCert,
    aaCert,
    "examples/trust-schema.conf");

  ndn_service_framework::ServiceContainer container({
    ndn::Name("/example/container"),
    ndn::Name("/example/group"),
    ndn::Name("/example/controller"),
    "examples/trust-schema.conf"
  });
  container.addUser("client", user);
  container.addProvider("vision-provider", provider);

  const ndn::Name remoteServiceName("/ObjectDetection/YOLOv8");
  const ndn::Name localPreprocessName("/Local/Image/Preprocess");

  user->applyPermissionResponse(
    makePermissionResponse(user->getName(),
                           ndn_service_framework::tlv::UserPermission,
                           provider->getName(),
                           remoteServiceName));
  provider->applyPermissionResponse(
    makePermissionResponse(provider->getName(),
                           ndn_service_framework::tlv::ProviderPermission,
                           provider->getName(),
                           remoteServiceName));

  container.addLocalService<ImageRequest, PreprocessResponse>(
    localPreprocessName,
    [] (const ndn::Name& requester,
        const ImageRequest& request,
        PreprocessResponse& response) {
      response.setNormalizedImage("normalized(" + requester.toUri() + ":" +
                                  request.getImage() + ")");
    });

  container.provider("vision-provider").addHandler<ImageRequest, LabelResponse>(
    remoteServiceName,
    std::function<void(const ndn::Name&, const ImageRequest&, LabelResponse&)>(
      [&container, localPreprocessName] (const ndn::Name& requester,
                                         const ImageRequest& request,
                                         LabelResponse& response) {
        auto helperResult =
          container.localRegistry().localInvoke<ImageRequest, PreprocessResponse>(
            localPreprocessName,
            request,
            ndn::Name("/example/provider/local-helper"));
        if (!helperResult.success) {
          response.setLabel("preprocess-error:" + helperResult.error);
          return;
        }
        std::cout << "remote provider handled " << request.getImage()
                  << " from " << requester
                  << " using hidden local helper output "
                  << helperResult.response.getNormalizedImage() << "\n";
        response.setLabel("person");
      }));

  user->setRequestPublisher(
    [&](const ndn::Name& requestId,
        const ndn::Name& requestName,
        const std::vector<ndn::Name>&,
        const ndn::Name&,
        const ndn_service_framework::RequestMessage& requestMessage,
        size_t) {
      auto response =
        container.provider("vision-provider").handleDecryptedRequestByName(
          requestName, requestMessage);
      auto parsedRequest = ndn_service_framework::parseRequestNameV2(requestName);
      if (!parsedRequest) {
        std::cerr << "failed to parse request name " << requestName << "\n";
        return;
      }

      auto responseName = ndn_service_framework::makeResponseNameV2(
        provider->getName(),
        parsedRequest->requesterName,
        parsedRequest->serviceName,
        requestId);
      response.setUserToken(requestMessage.getUserToken());
      user->handleDecryptedResponseByName(responseName, response);
    });

  container.start();

  bool gotResponse = false;
  ImageRequest request;
  request.setImage("frame-1");
  container.user("client").RequestService<ImageRequest, LabelResponse>(
    remoteServiceName,
    request,
    100,
    ndnsf::strategy::FirstResponding,
    1000,
    std::function<void(const LabelResponse&)>(
      [&] (const LabelResponse& response) {
        gotResponse = response.getLabel() == "person";
        std::cout << "remote user received label " << response.getLabel() << "\n";
      }),
    std::function<void(const ndn::Name&)>([] (const ndn::Name&) {
      std::cerr << "request timed out\n";
    }));

  container.stop();
  return gotResponse ? 0 : 1;
}
