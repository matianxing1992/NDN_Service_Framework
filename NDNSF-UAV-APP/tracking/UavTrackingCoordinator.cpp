#include "UavTrackingCoordinator.hpp"

#include "../shared/UavNames.hpp"
#include "../shared/UavProtocol.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ndnsf::examples::uav {

namespace {

constexpr size_t MAX_TRACKING_REQUEST_BYTES = 64 * 1024;
constexpr const char* DISCOVERY_MARKER = "UavTrackingDiscoveryV1";
constexpr const char* DISCOVERY_PREFIX = "UavTracking.";
constexpr std::array<const char*, 15> DISCOVERY_KEYS = {
  "schema", "run_id", "mission_id", "window_id", "observation_schema", "source_epoch",
  "source_uav1_id", "source_uav2_id", "source_uav3_id", "tracking_mode",
  "tracking_profile_digest", "tracking_backend_closure_digest", "tracking_native_closure_digest",
  "tracking_computation_digest", "tracking_request_timeout_ms"};

Fields
formalFields(const ndn_service_framework::RequestMessage& request)
{
  const auto payload = request.getPayload();
  if (payload.empty() || payload.size() > MAX_TRACKING_REQUEST_BYTES) {
    throw std::invalid_argument("tracking formal request byte bound");
  }
  return decodeFields(std::string(payload.begin(), payload.end()));
}

class FixedUavParticipantSelection final
  : public ndn_service_framework::ParticipantSelectionPolicy
{
public:
  FixedUavParticipantSelection(std::vector<ndn::Name> sourceProviders,
                               ndn::Name computeProvider,
                               ndn::Name calibrationProvider,
                               UavTrackingAckRequirements ackRequirements)
    : m_sourceProviders(std::move(sourceProviders))
    , m_computeProvider(std::move(computeProvider))
    , m_calibrationProvider(std::move(calibrationProvider))
    , m_ackRequirements(std::move(ackRequirements))
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
                 candidate.serviceName == role.service &&
                 matchesFeedback(role.role, candidate.ack) &&
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
  bool
  matchesFeedback(const std::string& role,
                  const ndn_service_framework::RequestAckMessage& ack) const
  {
    const auto required = m_ackRequirements.find(role);
    if (required == m_ackRequirements.end()) return true;
    try {
      const auto payload = ack.getPayload();
      if (payload.empty() || payload.size() > 16384) return false;
      const auto fields = decodeFields(std::string(payload.begin(), payload.end()));
      for (const auto& [key, value] : required->second) {
        const auto found = fields.find(key);
        if (found == fields.end() || found->second != value) return false;
      }
      return true;
    }
    catch (const std::exception&) {
      return false;
    }
  }

  ndn::Name expectedProvider(const std::string& role) const
  {
    if (role == "Camera-UAV1") return m_sourceProviders[0];
    if (role == "Camera-UAV2") return m_sourceProviders[1];
    if (role == "Camera-UAV3") return m_sourceProviders[2];
    if (role == "Tracking-Compute") return m_computeProvider;
    if (role == "Calibration-Provider") return m_calibrationProvider;
    return ndn::Name();
  }

  std::vector<ndn::Name> m_sourceProviders;
  ndn::Name m_computeProvider;
  ndn::Name m_calibrationProvider;
  UavTrackingAckRequirements m_ackRequirements;
};

ndn::Buffer textBuffer(const std::string& text)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

} // namespace

uint64_t
readUavTrackingRequestTimeout(const Fields& fields, bool required)
{
  const auto found = fields.find("tracking_request_timeout_ms");
  if (found == fields.end()) {
    if (!required) return 30000;
    throw std::invalid_argument("missing tracking request budget");
  }
  const auto& text = found->second;
  uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || text.front() == '0' || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() || value <= 1000 || value > 1200000) {
    throw std::invalid_argument("invalid tracking request budget");
  }
  return value;
}

Fields
makeUavTrackingDiscoveryFields(const Fields& initialFields)
{
  if (fieldOr(initialFields, "schema", "") != "spec191-window-request-v1") {
    throw std::invalid_argument("tracking discovery schema");
  }
  readUavTrackingRequestTimeout(initialFields, true);
  Fields result;
  for (const auto* key : DISCOVERY_KEYS) {
    const auto found = initialFields.find(key);
    if (found == initialFields.end()) continue;
    if (found->second.size() > ndn_service_framework::RequestCapabilities::MAX_FIELD_VALUE) {
      throw std::invalid_argument("tracking discovery field byte bound");
    }
    result.emplace(*found);
  }
  if (encodeFields(result).size() > MAX_TRACKING_REQUEST_BYTES) {
    throw std::invalid_argument("tracking discovery total byte bound");
  }
  return result;
}

Fields
readUavTrackingAckFields(const ndn_service_framework::RequestMessage& request)
{
  if (!request.hasRequestCapabilities()) return formalFields(request);
  const auto& capabilities = request.getRequestCapabilities();
  Fields result;
  for (const auto& [key, value] : capabilities.getFields()) {
    if (key.rfind(DISCOVERY_PREFIX, 0) != 0) continue;
    const auto name = key.substr(std::char_traits<char>::length(DISCOVERY_PREFIX));
    if (std::find(DISCOVERY_KEYS.begin(), DISCOVERY_KEYS.end(), name) == DISCOVERY_KEYS.end()) {
      throw std::invalid_argument("unknown tracking discovery field");
    }
    result.emplace(name, value);
  }
  if (!capabilities.hasField(DISCOVERY_MARKER)) {
    if (!result.empty() || (capabilities.hasField("RequestScopedConfidentialityV1") &&
        capabilities.getField("RequestScopedConfidentialityV1") == "required")) {
      throw std::invalid_argument("protected tracking request lacks discovery marker");
    }
    return formalFields(request);
  }
  if (capabilities.getField(DISCOVERY_MARKER) != "required" ||
      !capabilities.hasField("RequestScopedConfidentialityV1") ||
      capabilities.getField("RequestScopedConfidentialityV1") != "required") {
    throw std::invalid_argument("tracking discovery protection capability mismatch");
  }
  return makeUavTrackingDiscoveryFields(result);
}

void
validateUavTrackingDiscoveryBinding(const ndn_service_framework::RequestMessage& request)
{
  if (!request.hasRequestCapabilities()) return;
  const auto& capabilities = request.getRequestCapabilities();
  const bool hasDiscovery = capabilities.hasField(DISCOVERY_MARKER) || std::any_of(
    capabilities.getFields().begin(), capabilities.getFields().end(), [] (const auto& item) {
      return item.first.rfind(DISCOVERY_PREFIX, 0) == 0;
    });
  if (hasDiscovery && readUavTrackingAckFields(request) !=
                      makeUavTrackingDiscoveryFields(formalFields(request))) {
    throw std::invalid_argument("tracking discovery disagrees with formal input");
  }
}

ndn_service_framework::CollaborationPlan
UavTrackingCoordinator::makePlan(const std::vector<ndn::Name>& sourceProviders,
                                 const ndn::Name& computeProvider,
                                 const ndn::Buffer& assignmentPayload,
                                 int ackCollectionTimeMs, int timeoutMs,
                                 ndn::Name calibrationProvider,
                                 UavTrackingAckRequirements ackRequirements)
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
  if (!calibrationProvider.empty() &&
      (calibrationProvider == computeProvider || uniqueSources.count(calibrationProvider) != 0)) {
    throw std::invalid_argument("calibration provider must be distinct from source and compute providers");
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

  if (!calibrationProvider.empty()) {
    ndn_service_framework::CollaborationRoleSpec calibration;
    calibration.role = "Calibration-Provider";
    calibration.service = SERVICE_TRACKING_WINDOW;
    calibration.appRequirement = textBuffer(
      "role=calibration-provider;provider=" + calibrationProvider.toUri());
    calibration.assignmentPayload = assignmentPayload;
    calibration.minProviders = 1;
    calibration.maxProviders = 1;
    calibration.terminalResponseOwner = false;
    plan.roles.push_back(std::move(calibration));
  }

  std::vector<std::string> inputRoles{
    "Camera-UAV1", "Camera-UAV2", "Camera-UAV3", "Tracking-Compute"};
  std::vector<std::string> observationRoles = inputRoles;
  if (!calibrationProvider.empty()) {
    observationRoles.push_back("Calibration-Provider");
  }
  plan.keyScopes.push_back({TRACKING_INPUT_KEY_SCOPE,
                            inputRoles});
  plan.keyScopes.push_back({"uav-tracking-observation", observationRoles});
  // The compute role publishes the encrypted, segmented recognition result
  // under its producer-owned name.  Keep this output key separate from the
  // input scope so only the role that emits the result receives the publish
  // capability for this invocation.
  plan.keyScopes.push_back({"uav-tracking-result", {"Tracking-Compute"}});
  plan.dependencies.push_back({{"Camera-UAV1", "Camera-UAV2", "Camera-UAV3"},
                               {"Tracking-Compute"}, TRACKING_INPUT_KEY_SCOPE,
                               ndn::Name("/example/uav/tracking"), true});
  plan.dependencies.push_back({{"Camera-UAV1", "Camera-UAV2", "Camera-UAV3"},
                               {"Tracking-Compute"}, "uav-tracking-observation",
                               ndn::Name("/example/uav/observation"), true});
  if (!calibrationProvider.empty()) {
    plan.dependencies.push_back({{"Calibration-Provider"}, {"Tracking-Compute"},
                                 "uav-tracking-observation",
                                 ndn::Name("/example/uav/observation"), true});
  }
  plan.sharedAssignmentMetadata = assignmentPayload;
  plan.participantSelector = std::make_shared<FixedUavParticipantSelection>(
    sourceProviders, computeProvider, calibrationProvider, std::move(ackRequirements));
  return plan;
}

bool
UavTrackingCoordinator::hasRequiredParticipants(
  const std::vector<ndn_service_framework::SelectedParticipant>& selected,
  std::string* reason, bool calibrationProviderRequired)
{
  std::set<std::string> roles;
  std::set<ndn::Name> providers;
  for (const auto& participant : selected) {
    roles.insert(participant.role);
    providers.insert(participant.provider);
  }
  std::set<std::string> expected{
    "Camera-UAV1", "Camera-UAV2", "Camera-UAV3", "Tracking-Compute"};
  if (calibrationProviderRequired || roles.count("Calibration-Provider") != 0) {
    expected.insert("Calibration-Provider");
  }
  if (roles != expected || providers.size() != expected.size()) {
    if (reason) *reason = "selection must contain three camera roles, one compute role, and at most one calibration provider";
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
  ndn_service_framework::RequestCapabilities capabilities;
  if (user.hasRequestScopedDefault()) {
    ndn_service_framework::RequestMessage input;
    auto bytes = initialRequest;
    input.setPayload(bytes, bytes.size());
    const auto discovery = makeUavTrackingDiscoveryFields(formalFields(input));
    capabilities.setField("RequestScopedConfidentialityV1", "required");
    capabilities.setField(DISCOVERY_MARKER, "required");
    for (const auto& [key, value] : discovery) {
      capabilities.setField(std::string(DISCOVERY_PREFIX) + key, value);
    }
  }
  auto onPlanCommitted = std::move(callbacks.onPlanCommitted);
  auto onFailure = std::move(callbacks.onFailure);
  return user.BeginCollaboration(
    service, initialRequest, plan.ackCollectionTimeMs, plan.timeoutMs,
    [&user, plan = std::move(plan), onPlanCommitted = std::move(onPlanCommitted),
     onFailure = std::move(onFailure)]
    (const ndn_service_framework::CollaborationAckClosure& closure) mutable {
      const auto selected = plan.participantSelector->select(closure.candidates, plan.roles);
      std::string reason;
      const bool calibrationProviderRequired = std::any_of(
        plan.roles.begin(), plan.roles.end(), [] (const auto& role) {
          return role.role == "Calibration-Provider";
        });
      if (!hasRequiredParticipants(selected, &reason, calibrationProviderRequired)) {
        if (onFailure) onFailure(reason);
        return;
      }
      if (!user.CommitCollaborationPlan(closure.requestId, closure.digest, plan)) {
        if (onFailure) onFailure("CommitCollaborationPlan rejected");
        return;
      }
      if (onPlanCommitted) onPlanCommitted(closure, selected);
    },
    std::move(callbacks.onResponse), std::move(callbacks.onTimeout), requestId, {}, capabilities);
}

} // namespace ndnsf::examples::uav
