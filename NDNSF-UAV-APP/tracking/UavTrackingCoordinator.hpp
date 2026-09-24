#pragma once

#include "ndn-service-framework/ServiceUser.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

struct UavTrackingCoordinatorCallbacks
{
  std::function<void(const ndn_service_framework::CollaborationAckClosure&,
                     const std::vector<ndn_service_framework::SelectedParticipant>&)> onPlanCommitted;
  ndn_service_framework::ServiceUser::ResponseHandler onResponse;
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout;
  std::function<void(const std::string&)> onFailure;
};

class UavTrackingCoordinator
{
public:
  static ndn_service_framework::CollaborationPlan
  makePlan(const std::vector<ndn::Name>& sourceProviders,
           const ndn::Name& computeProvider,
           const ndn::Buffer& assignmentPayload,
           int ackCollectionTimeMs = 1000,
           int timeoutMs = 30000);

  static bool
  hasRequiredParticipants(
    const std::vector<ndn_service_framework::SelectedParticipant>& selected,
    std::string* reason = nullptr);

  /**
   * Start the Core collaboration lifecycle. The callback is invoked only
   * after ACK closure, participant validation, and a successful
   * CommitCollaborationPlan call.
   */
  ndn::Name begin(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& service,
    const ndn::Buffer& initialRequest,
    ndn_service_framework::CollaborationPlan plan,
    UavTrackingCoordinatorCallbacks callbacks,
    const ndn::Name& requestId = ndn::Name());
};

} // namespace ndnsf::examples::uav
