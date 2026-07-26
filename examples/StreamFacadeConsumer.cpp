#include "ndn-service-framework/ServiceUser.hpp"

#include <iostream>

namespace nsf = ndn_service_framework;

/**
 * The descriptor normally arrives in an application control response. This
 * helper is the entire high-level consumer setup after that response.
 */
std::shared_ptr<nsf::PredictiveStreamSubscriber>
subscribeToStream(nsf::ServiceUser& user,
                  const nsf::PredictiveStreamDescriptor& descriptor)
{
  nsf::StreamSubscriptionOptions options;
  options.onItem = [] (const nsf::VerifiedLiveStreamItem& item) {
    std::cout << item.cursor << " " << item.originalName << " "
              << item.content.size() << std::endl;
    return nsf::LiveStreamItemAdmission::acceptItem();
  };
  // Predictive naming, pacing, and recovery are derived from the descriptor.
  return user.subscribeStream(descriptor, std::move(options));
}

int
main()
{
  std::cout << "Pass the received PredictiveStreamDescriptor to "
               "subscribeToStream(user, descriptor)." << std::endl;
  return 0;
}
