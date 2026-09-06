#include "UavDiagnostics.hpp"

#include <algorithm>
#include <sstream>

namespace ndnsf::examples::uav {

const char*
to_string(UavTraceStage stage) noexcept
{
  switch (stage) {
  case UavTraceStage::MissionTrigger: return "MISSION_TRIGGER";
  case UavTraceStage::RequestPublished: return "REQUEST_PUBLISHED";
  case UavTraceStage::AckClosed: return "ACK_CLOSED";
  case UavTraceStage::PlanCommitted: return "PLAN_COMMITTED";
  case UavTraceStage::Selection: return "SELECTION";
  case UavTraceStage::EvidenceInterest: return "EVIDENCE_INTEREST";
  case UavTraceStage::EvidenceDataVerified: return "EVIDENCE_DATA_VERIFIED";
  case UavTraceStage::Execution: return "EXECUTION";
  case UavTraceStage::ReportPublished: return "REPORT_PUBLISHED";
  case UavTraceStage::TerminalAccepted: return "TERMINAL_ACCEPTED";
  case UavTraceStage::Failure: return "FAILURE";
  }
  return "UNKNOWN";
}

namespace {

bool
containsEndpointText(const std::string& value)
{
  if (value.find("://") != std::string::npos || value.find("socket") != std::string::npos ||
      value.find("host=") != std::string::npos || value.find("port=") != std::string::npos) {
    return true;
  }
  size_t digits = 0;
  for (const char ch : value) {
    if (ch >= '0' && ch <= '9') ++digits;
  }
  return digits >= 7 && value.find('.') != std::string::npos;
}

} // namespace

UavTraceRecorder::UavTraceRecorder(std::size_t maxEvents, uint32_t sampleRate)
  : m_maxEvents(std::max<std::size_t>(1, maxEvents))
  , m_sampleRate(std::max<uint32_t>(1, sampleRate))
{
}

void
UavTraceRecorder::registerIncident(const std::string& incidentId)
{
  if (!incidentId.empty()) m_registeredIncidents.insert(incidentId);
}

bool
UavTraceRecorder::record(UavTraceEvent event, std::string* reason)
{
  if (event.missionId.empty() || event.incidentId.empty() || event.requestId.empty() ||
      event.timestampMs == 0 || containsTransportEndpoint(event)) {
    if (reason) *reason = "invalid or endpoint-bearing UAV trace event";
    return false;
  }
  ++m_seen;
  const bool registered = m_registeredIncidents.count(event.incidentId) != 0;
  if (!registered && ((m_seen - 1) % m_sampleRate) != 0) return true;
  if (m_events.size() >= m_maxEvents) {
    if (reason) *reason = "UAV trace bound exceeded";
    return false;
  }
  m_events.push_back(std::move(event));
  return true;
}

std::string
UavTraceRecorder::snapshot() const
{
  std::ostringstream output;
  for (const auto& event : m_events) {
    output << event.timestampMs << '|' << event.missionId << '|' << event.incidentId << '|'
           << event.requestId.toUri() << '|' << to_string(event.stage) << '|'
           << event.requestedDataName.toUri() << '|' << event.returnedDataName.toUri() << '|'
           << event.producerIdentity.toUri() << '|' << event.signerIdentity.toUri() << '|'
           << event.version << '|' << event.contentDigest << '|' << event.detail << '\n';
  }
  return output.str();
}

bool
UavTraceRecorder::containsTransportEndpoint(const UavTraceEvent& event) noexcept
{
  for (const auto* value : {&event.requestedDataName, &event.returnedDataName,
                            &event.producerIdentity, &event.signerIdentity}) {
    if (value->toUri().find("://") != std::string::npos) return true;
  }
  return containsEndpointText(event.detail) || containsEndpointText(event.contentDigest);
}

bool
UavTraceRecorder::hasCompleteLineage(const UavTraceEvent& event) noexcept
{
  return !event.missionId.empty() && !event.incidentId.empty() &&
         !event.requestId.empty();
}

bool
UavBoundedWorkQueue::tryPush(std::string work)
{
  if (work.empty() || m_items.size() >= m_capacity) {
    ++m_dropped;
    return false;
  }
  m_items.push_back(std::move(work));
  return true;
}

std::optional<std::string>
UavBoundedWorkQueue::tryPop()
{
  if (m_items.empty()) {
    return std::nullopt;
  }
  auto value = std::move(m_items.front());
  m_items.pop_front();
  return value;
}

} // namespace ndnsf::examples::uav
