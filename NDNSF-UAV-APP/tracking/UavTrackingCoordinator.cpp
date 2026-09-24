#include "UavTrackingCoordinator.hpp"

#include "../shared/UavNames.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ndnsf::examples::uav {

namespace {

class FixedUavParticipantSelection final
  : public ndn_service_framework::ParticipantSelectionPolicy
{
public:
  FixedUavParticipantSelection(std::vector<ndn::Name> sourceProviders,
                               ndn::Name computeProvider)
    : m_sourceProviders(std::move(sourceProviders))
    , m_computeProvider(std::move(computeProvider))
  {
  }

  std::vector<ndn_service_framework::SelectedParticipant>
  select(const std::vector<ndn_service_framework::AckCandidate>& candidates,
         const std::vector<ndn_service_framework::CollaborationRoleSpec>& roles) const override
  {
    std::vector<ndn_service_framework::SelectedParticipant> selected;
    std::set<ndn::Name> used;
    for (const auto& role : roles) {
      const auto expected = expectedProvider(role.role);
      const auto match = std::find_if(candidates.begin(), candidates.end(),
        [&] (const auto& candidate) {
          return candidate.ack.getStatus() && candidate.providerName == expected &&
                 !used.count(candidate.providerName);
        });
      if (match == candidates.end()) continue;
      ndn_service_framework::SelectedParticipant participant;
      participant.role = role.role;
      participant.service = role.service;
      participant.provider = match->providerName;
      participant.assignmentPayload = role.assignmentPayload;
      participant.ack = *match;
      selected.push_back(std::move(participant));
      used.insert(match->providerName);
    }
    return selected;
  }

private:
  ndn::Name expectedProvider(const std::string& role) const
  {
    if (role == "Camera-UAV1") return m_sourceProviders[0];
    if (role == "Camera-UAV2") return m_sourceProviders[1];
    if (role == "Camera-UAV3") return m_sourceProviders[2];
    if (role == "Tracking-Compute") return m_computeProvider;
    return ndn::Name();
  }

  std::vector<ndn::Name> m_sourceProviders;
  ndn::Name m_computeProvider;
};

ndn::Buffer textBuffer(const std::string& text)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

} // namespace

ndn_service_framework::CollaborationPlan
UavTrackingCoordinator::makePlan(const std::vector<ndn::Name>& sourceProviders,
                                 const ndn::Name& computeProvider,
                                 const ndn::Buffer& assignmentPayload,
                                 int ackCollectionTimeMs, int timeoutMs)
{
  if (sourceProviders.size() != 3 || computeProvider.empty() ||
      ackCollectionTimeMs <= 0 || timeoutMs <= ackCollectionTimeMs) {
    throw std::invalid_argument("Spec191 requires three sources and one compute provider");
  }
  std::set<ndn::Name> uniqueSources;
  for (const auto& provider : sourceProviders) {
    if (provider.empty() || provider == computeProvider || !uniqueSources.insert(provider).second) {
      throw std::invalid_argument("Spec191 requires three distinct source providers");
    }
  }
  ndn_service_framework::CollaborationPlan plan;
  plan.ackCollectionTimeMs = ackCollectionTimeMs;
  plan.timeoutMs = timeoutMs;
  for (const auto& camera : {std::string("UAV1"), std::string("UAV2"), std::string("UAV3")}) {
    ndn_service_framework::CollaborationRoleSpec role;
    role.role = "Camera-" + camera;
    role.service = SERVICE_TRACKING_WINDOW;
    role.appRequirement = textBuffer("camera=" + camera);
    role.assignmentPayload = assignmentPayload;
    role.minProviders = 1;
    role.maxProviders = 1;
    role.terminalResponseOwner = false;
    plan.roles.push_back(std::move(role));
  }
  ndn_service_framework::CollaborationRoleSpec compute;
  compute.role = "Tracking-Compute";
  compute.service = SERVICE_TRACKING_WINDOW;
  compute.appRequirement = textBuffer("role=compute;provider=" + computeProvider.toUri());
  compute.assignmentPayload = assignmentPayload;
  compute.minProviders = 1;
  compute.maxProviders = 1;
  compute.terminalResponseOwner = true;
  plan.roles.push_back(std::move(compute));
  plan.keyScopes.push_back({"uav-tracking-input", {"Camera-UAV1", "Camera-UAV2", "Camera-UAV3", "Tracking-Compute"}});
  // The compute role publishes the encrypted, segmented recognition result
  // under its producer-owned name.  Keep this output key separate from the
  // input scope so only the role that emits the result receives the publish
  // capability for this invocation.
  plan.keyScopes.push_back({"uav-tracking-result", {"Tracking-Compute"}});
  plan.dependencies.push_back({{"Camera-UAV1", "Camera-UAV2", "Camera-UAV3"},
                               {"Tracking-Compute"}, "uav-tracking-input",
                               ndn::Name("/example/uav/tracking"), true});
  plan.sharedAssignmentMetadata = assignmentPayload;
  plan.participantSelector = std::make_shared<FixedUavParticipantSelection>(
    sourceProviders, computeProvider);
  return plan;
}

bool
UavTrackingCoordinator::hasRequiredParticipants(
  const std::vector<ndn_service_framework::SelectedParticipant>& selected,
  std::string* reason)
{
  std::set<std::string> roles;
  std::set<ndn::Name> providers;
  for (const auto& participant : selected) {
    roles.insert(participant.role);
    providers.insert(participant.provider);
  }
  const std::set<std::string> expected{
    "Camera-UAV1", "Camera-UAV2", "Camera-UAV3", "Tracking-Compute"};
  if (roles != expected || providers.size() != 4) {
    if (reason) *reason = "selection must contain three camera roles and one compute role";
    return false;
  }
  return true;
}

ndn::Name
UavTrackingCoordinator::begin(
  ndn_service_framework::ServiceUser& user, const ndn::Name& service,
  const ndn::Buffer& initialRequest,
  ndn_service_framework::CollaborationPlan plan,
  UavTrackingCoordinatorCallbacks callbacks, const ndn::Name& requestId)
{
  if (service.empty() || initialRequest.empty() || !plan.participantSelector) {
    throw std::invalid_argument("invalid Spec191 collaboration inputs");
  }
  return user.BeginCollaboration(
    service, initialRequest, plan.ackCollectionTimeMs, plan.timeoutMs,
    [&user, plan = std::move(plan), callbacks = std::move(callbacks)]
    (const ndn_service_framework::CollaborationAckClosure& closure) mutable {
      const auto selected = plan.participantSelector->select(closure.candidates, plan.roles);
      std::string reason;
      if (!hasRequiredParticipants(selected, &reason)) {
        if (callbacks.onFailure) callbacks.onFailure(reason);
        return;
      }
      if (!user.CommitCollaborationPlan(closure.requestId, closure.digest, plan)) {
        if (callbacks.onFailure) callbacks.onFailure("CommitCollaborationPlan rejected");
        return;
      }
      if (callbacks.onPlanCommitted) callbacks.onPlanCommitted(closure, selected);
    },
    std::move(callbacks.onResponse), std::move(callbacks.onTimeout), requestId);
}

} // namespace ndnsf::examples::uav
