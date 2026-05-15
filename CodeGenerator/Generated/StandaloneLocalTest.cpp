#include "Provider.hpp"
#include "User.hpp"
#include "messages.pb.h"

#include <cassert>
#include <functional>
#include <tuple>

int
main()
{
  ndn::Face face;
  ndn::security::Certificate identityCert;
  ndn::security::Certificate attrAuthorityCertificate;

  muas::Provider provider(face,
                          ndn::Name("/muas"),
                          identityCert,
                          attrAuthorityCertificate,
                          "trust-schema.conf");
  muas::User user(face,
                  ndn::Name("/muas"),
                  identityCert,
                  attrAuthorityCertificate,
                  "trust-schema.conf");

  bool ackHandled = false;

  provider.setRequestAckPublisher(
      [&user, &ackHandled](const ndn::Name&,
                           const ndn::Name&,
                           const ndn::Name&,
                           const ndn::Name&,
                           const ndn::Name& ackName,
                           const ndn_service_framework::RequestAckMessage& ack) {
        assert(!ackName.empty());
        assert(ack.getStatus());
        ackHandled = user.handleRequestAckByName(ackName, ack);
        assert(ackHandled);
      });

  provider.setResponsePublisher(
      [&user](const ndn::Name&,
              const ndn::Name&,
              const ndn::Name&,
              const ndn::Name& requestId,
              const ndn::Name& responseName,
              const ndn_service_framework::ResponseMessage& response) {
        assert(!responseName.empty());
        const bool handled = user.handleDecryptedResponseByName(responseName, response);
        assert(handled);
      });

  user.setRequestPublisher(
      [&provider, &user](const ndn::Name& requestId,
                         const ndn::Name& requestName,
                         const std::vector<ndn::Name>&,
                         const ndn::Name& serviceName,
                         const ndn_service_framework::RequestMessage& requestMessage,
                         size_t) {
        assert(!requestName.empty());
        auto parsed = ndn_service_framework::parseRequestName(requestName);
        assert(parsed);

        ndn::Name parsedRequester;
        ndn::Name parsedServiceName;
        ndn::Name parsedFunctionName;
        ndn::Name parsedBloomFilter;
        ndn::Name parsedRequestId;
        std::tie(parsedRequester,
                 parsedServiceName,
                 parsedFunctionName,
                 parsedBloomFilter,
                 parsedRequestId) = parsed.value();

        assert(parsedServiceName.equals(ndn::Name("ObjectDetection")));
        assert(parsedFunctionName.equals(ndn::Name("YOLOv8")));

        const auto ack = provider.publishRequestAckForName(requestName);
        assert(ack.getStatus());
        assert(user.getPendingRequestAckCount(requestId) == 1);
        assert(user.getSuccessfulAckProviders(requestId).size() <= 1);

        const auto response =
            provider.handleDecryptedRequestByNameAndPublish(requestName, requestMessage);
        assert(response.getStatus());
      });

  bool providerHandlerCalled = false;

  provider.addHandler<muas::ObjectDetection_YOLOv8_Request,
                      muas::ObjectDetection_YOLOv8_Response>(
      ndn::Name("/ObjectDetection/YOLOv8"),
      std::function<void(const ndn::Name&,
                         const muas::ObjectDetection_YOLOv8_Request&,
                         muas::ObjectDetection_YOLOv8_Response&)>(
          [&providerHandlerCalled](const ndn::Name&,
                                   const muas::ObjectDetection_YOLOv8_Request& request,
                                   muas::ObjectDetection_YOLOv8_Response& response) {
            providerHandlerCalled = true;
            assert(request.image_str() == "local-image-bytes");

            auto* result = response.add_results();
            result->set_classification(42);
            result->set_x_1(1.0F);
            result->set_y_1(2.0F);
            result->set_x_2(3.0F);
            result->set_y_2(4.0F);
          }));

  bool callbackCalled = false;
  muas::ObjectDetection_YOLOv8_Request request;
  request.set_image_str("local-image-bytes");

  const ndn::Name requestId =
      user.asyncCall<muas::ObjectDetection_YOLOv8_Request,
                     muas::ObjectDetection_YOLOv8_Response>(
          {ndn::Name("/muas/provider")},
          ndn::Name("/ObjectDetection/YOLOv8"),
          request,
          std::function<void(const muas::ObjectDetection_YOLOv8_Response&)>(
              [&callbackCalled](const muas::ObjectDetection_YOLOv8_Response& response) {
                assert(response.results_size() == 1);
                assert(response.results(0).classification() == 42);
                callbackCalled = true;
              }),
          std::function<void()>([] {
            assert(false && "local-only test should not time out");
          }),
          1000,
          ndn_service_framework::tlv::FirstResponding);

  assert(!requestId.empty());
  assert(ackHandled);
  assert(providerHandlerCalled);
  assert(callbackCalled);

  bool customAcksHandlerCalled = false;
  muas::User customUser(face,
                        ndn::Name("/muas"),
                        identityCert,
                        attrAuthorityCertificate,
                        "trust-schema.conf");

  customUser.setRequestPublisher(
      [&provider](const ndn::Name&,
                  const ndn::Name& requestName,
                  const std::vector<ndn::Name>&,
                  const ndn::Name&,
                  const ndn_service_framework::RequestMessage&,
                  size_t) {
        const auto ack = provider.publishRequestAckForName(requestName);
        assert(ack.getStatus());
      });

  ndn_service_framework::RequestMessage customRequest;
  const ndn::Name customRequestId =
      customUser.async_call(ndn::Name("/ObjectDetection/YOLOv8"),
                            customRequest,
                            100,
                            muas::User::AcksHandler(
                                [&customAcksHandlerCalled](
                                    const std::vector<ndn_service_framework::RequestAckMessage>& acks) {
                                  customAcksHandlerCalled = true;
                                  assert(acks.size() == 1);
                                  return acks;
                                }),
                            1000,
                            muas::User::TimeoutHandler([](const ndn::Name&) {
                              assert(false && "local-only custom ACK test should not time out");
                            }),
                            muas::User::ResponseHandler(
                                [](const ndn_service_framework::ResponseMessage&) {}));

  assert(!customRequestId.empty());
  assert(customAcksHandlerCalled);
  assert(customUser.getPendingRequestAckCount(customRequestId) == 1);

  return 0;
}
