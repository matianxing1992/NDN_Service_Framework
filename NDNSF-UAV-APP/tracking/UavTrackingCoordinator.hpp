#pragma once

#include "ndn-service-framework/ServiceUser.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

using UavTrackingAckRequirements =
  std::map<std::string, std::map<std::string, std::string>>;

struct UavTrackingCoordinatorCallbacks
{
  std::function<void(const ndn_service_framework::CollaborationAckClosure&,
                     const std::vector<ndn_service_framework::SelectedParticipant>&)> onPlanCommitted;
  ndn_service_framework::ServiceUser::ResponseHandler onResponse;
  ndn_service_framework::ServiceUser::TimeoutHandler onTimeout;
  std::function<void(const std::string&)> onFailure;
};

/** Canonical whole-window request budget; only historical unmarked callers may omit it. */
uint64_t
readUavTrackingRequestTimeout(const std::map<std::string, std::string>& fields, bool required);

/** Public selection metadata only; formal input is independently admitted after Selection. */
std::map<std::string, std::string>
makeUavTrackingDiscoveryFields(const std::map<std::string, std::string>& initialFields);

/** Read signed discovery fields for ACK admission, never for executing a task. */
std::map<std::string, std::string>
readUavTrackingAckFields(const ndn_service_framework::RequestMessage& request);

/** Reject disagreement between public selection metadata and decrypted formal input. */
void
validateUavTrackingDiscoveryBinding(const ndn_service_framework::RequestMessage& request);

class UavTrackingCoordinator
{
public:
  static ndn_service_framework::CollaborationPlan
  makePlan(const std::vector<ndn::Name>& sourceProviders,
           const ndn::Name& computeProvider,
           const ndn::Buffer& assignmentPayload,
           int ackCollectionTimeMs = 1000,
           int timeoutMs = 30000,
           ndn::Name calibrationProvider = ndn::Name(),
           UavTrackingAckRequirements ackRequirements = {});

  static bool
  hasRequiredParticipants(
    const std::vector<ndn_service_framework::SelectedParticipant>& selected,
    std::string* reason = nullptr,
    bool calibrationProviderRequired = false);

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
