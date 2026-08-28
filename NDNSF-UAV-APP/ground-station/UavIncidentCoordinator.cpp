#include "UavIncidentCoordinator.hpp"
#include "../shared/UavNames.hpp"

#include <algorithm>
#include <stdexcept>

namespace ndnsf::examples::uav {

namespace {

size_t
countTerminalRoles(const std::vector<UavRoleAssignment>& assignments)
{
  return std::count_if(assignments.begin(), assignments.end(),
                       [](const auto& assignment) {
                         return assignment.terminalResponseOwner;
                       });
}

} // namespace

const char*
to_string(UavCollaborationFailureStage stage) noexcept
{
  switch (stage) {
  case UavCollaborationFailureStage::Discovery: return "discovery";
  case UavCollaborationFailureStage::AckClosure: return "ack-closure";
  case UavCollaborationFailureStage::Plan: return "plan";
  case UavCollaborationFailureStage::Selection: return "selection";
  case UavCollaborationFailureStage::Evidence: return "evidence";
  case UavCollaborationFailureStage::Execution: return "execution";
  case UavCollaborationFailureStage::Report: return "report";
  case UavCollaborationFailureStage::Delivery: return "delivery";
  }
  return "unknown";
}

bool
isIdempotentAnalysisFailure(UavCollaborationFailureStage stage) noexcept
{
  // A retry reuses immutable named evidence and creates a new request/attempt.
  // Discovery and ACK closure are not retried here: repeating those stages can
  // create duplicate provider work before an evidence-backed plan exists.
  return stage == UavCollaborationFailureStage::Plan ||
         stage == UavCollaborationFailureStage::Selection ||
         stage == UavCollaborationFailureStage::Evidence ||
         stage == UavCollaborationFailureStage::Execution ||
         stage == UavCollaborationFailureStage::Report ||
         stage == UavCollaborationFailureStage::Delivery;
}

UavIncidentCoordinator::UavIncidentCoordinator(UavMissionSession& mission,
                                               UavIncidentRecord incident,
                                               UavIncidentCoordinatorConfig config)
  : m_mission(mission)
  , m_incident(std::move(incident))
  , m_config(std::move(config))
{
  if (m_config.ackTimeoutMs == 0 || m_config.globalDeadlineMs == 0 ||
      m_config.ackTimeoutMs > m_config.globalDeadlineMs) {
    throw std::invalid_argument("invalid incident coordinator deadlines");
  }
}

bool
UavIncidentCoordinator::begin(const ndn::Name& requestId, uint64_t nowMs,
                              std::string* reason)
{
  if (m_started || requestId.empty() || m_incident.incidentId.empty() ||
      m_incident.missionId != m_mission.record().missionId ||
      m_incident.currentAttemptId.empty()) {
    if (reason) *reason = "invalid or duplicate incident start";
    return false;
  }
  m_job.requestId = requestId;
  m_job.missionId = m_incident.missionId;
  m_job.incidentId = m_incident.incidentId;
  m_job.attemptId = m_incident.currentAttemptId;
  m_job.ackDeadlineMs = nowMs + m_config.ackTimeoutMs;
  m_job.globalDeadlineMs = nowMs + m_config.globalDeadlineMs;
  m_job.state = UavCollaborationJobState::AckCollecting;
  // The MissionSession materializes the job at commitPlan, once the ACK
  // snapshot has yielded a bounded role plan.  Until then this object retains
  // only the finite request lineage and deadline.
  m_started = true;
  return true;
}

bool
UavIncidentCoordinator::withinDeadline(uint64_t nowMs, std::string* reason) const
{
  if (!m_started || nowMs > m_job.globalDeadlineMs) {
    if (reason) *reason = "incident collaboration deadline exceeded";
    return false;
  }
  return true;
}

bool
UavIncidentCoordinator::requireState(UavCollaborationJobState expected,
                                     std::string* reason) const
{
  if (m_job.state != expected) {
    if (reason) *reason = "unexpected incident collaboration state";
    return false;
  }
  return true;
}

bool
UavIncidentCoordinator::update(UavCollaborationJobState next, std::string* reason)
{
  m_job.state = next;
  if (m_mission.hasJob(m_job.requestId)) {
    return m_mission.updateJob(m_job.requestId, next, {}, {}, reason);
  }
  return true;
}

bool
UavIncidentCoordinator::closeAcks(uint64_t nowMs, std::string* reason)
{
  if (!requireState(UavCollaborationJobState::AckCollecting, reason) ||
      !withinDeadline(nowMs, reason)) return false;
  if (nowMs < m_job.ackDeadlineMs) {
    if (reason) *reason = "ACK window is still open";
    return false;
  }
  return update(UavCollaborationJobState::AckClosed, reason);
}

bool
UavIncidentCoordinator::commitPlan(std::vector<UavRoleAssignment> assignments,
                                   ndn::Name terminalOwner,
                                   std::string planDigest,
                                   std::string* reason)
{
  if (!requireState(UavCollaborationJobState::AckClosed, reason) || terminalOwner.empty() ||
      planDigest.empty() || assignments.empty() || assignments.size() > m_config.maxAssignments ||
      countTerminalRoles(assignments) != 1) {
    if (reason && reason->empty()) *reason = "invalid bounded collaboration plan";
    return false;
  }
  for (const auto& assignment : assignments) {
    if (assignment.providerIdentity.empty() ||
        (assignment.role != "EvidenceSource" && assignment.role != "DetectorReporter") ||
        assignment.evidence.size() > m_config.maxEvidence) {
      if (reason) *reason = "invalid role assignment";
      return false;
    }
    for (const auto& evidence : assignment.evidence) {
      if (!evidence.isValid(reason) ||
          !isUavProducerDataNameForContext(evidence.producerIdentity,
                                           evidence.exactDataName, "EVIDENCE",
                                           m_job.missionId, m_job.incidentId)) {
        if (reason && reason->empty()) *reason = "assignment evidence name is not bound to job lineage";
        return false;
      }
    }
    if (assignment.terminalResponseOwner &&
        (assignment.role != "DetectorReporter" || assignment.providerIdentity != terminalOwner)) {
      if (reason) *reason = "terminal owner must be the sole DetectorReporter";
      return false;
    }
  }
  m_job.assignments = std::move(assignments);
  m_job.terminalOwner = std::move(terminalOwner);
  m_job.planDigest = std::move(planDigest);
  const auto terminalAssignment = std::find_if(
    m_job.assignments.begin(), m_job.assignments.end(), [&](const auto& assignment) {
      return assignment.terminalResponseOwner && assignment.providerIdentity == m_job.terminalOwner;
    });
  if (terminalAssignment == m_job.assignments.end() || terminalAssignment->evidence.empty()) {
    if (reason) *reason = "terminal detector role requires named evidence";
    return false;
  }
  // The ACK window is closed before the job becomes visible in MissionSession;
  // this preserves the same monotonic lifecycle when the record is inserted.
  m_job.state = UavCollaborationJobState::AckClosed;
  if (!m_mission.addJob(m_job, reason)) return false;
  return update(UavCollaborationJobState::PlanCommitted, reason);
}

bool
UavIncidentCoordinator::selectProvider(const ndn::Name& provider, std::string* reason)
{
  if (!requireState(UavCollaborationJobState::PlanCommitted, reason) || provider.empty()) {
    if (reason && reason->empty()) *reason = "invalid provider selection";
    return false;
  }
  const auto it = std::find_if(m_job.assignments.begin(), m_job.assignments.end(),
                               [&](const auto& assignment) {
                                 return assignment.providerIdentity == provider &&
                                        assignment.role == "DetectorReporter";
                               });
  if (it == m_job.assignments.end()) {
    if (reason) *reason = "selected provider is not the detector reporter";
    return false;
  }
  m_selectedProvider = provider;
  return update(UavCollaborationJobState::Selected, reason);
}

bool
UavIncidentCoordinator::markEvidenceReady(std::string* reason)
{
  if (!requireState(UavCollaborationJobState::Selected, reason)) return false;
  const auto it = std::find_if(m_job.assignments.begin(), m_job.assignments.end(),
                               [&](const auto& assignment) {
                                 return assignment.providerIdentity == m_selectedProvider &&
                                        assignment.role == "DetectorReporter";
                               });
  if (it == m_job.assignments.end() || it->evidence.empty()) {
    if (reason) *reason = "selected detector has no named evidence";
    return false;
  }
  return update(UavCollaborationJobState::EvidenceReady, reason);
}

bool
UavIncidentCoordinator::markExecuting(std::string* reason)
{
  return requireState(UavCollaborationJobState::EvidenceReady, reason) &&
         update(UavCollaborationJobState::Executing, reason);
}

bool
UavIncidentCoordinator::markReporting(std::string* reason)
{
  return requireState(UavCollaborationJobState::Executing, reason) &&
         update(UavCollaborationJobState::Reporting, reason);
}

bool
UavIncidentCoordinator::acceptReport(const UavTerminalReport& report, uint64_t nowMs,
                                     std::string* reason)
{
  if (!requireState(UavCollaborationJobState::Reporting, reason) ||
      !withinDeadline(nowMs, reason)) return false;
  if (!m_mission.acceptTerminalReport(report, reason)) return false;
  m_job.state = report.status == "success" ? UavCollaborationJobState::Succeeded :
                                               UavCollaborationJobState::Failed;
  return true;
}

bool
UavIncidentCoordinator::fail(const std::string& stage, const std::string& detail,
                             bool timedOut, std::string* reason)
{
  if (!m_started || stage.empty() || stage.size() > 128 || detail.size() > 512) {
    if (reason) *reason = "invalid collaboration failure";
    return false;
  }
  m_job.failureStage = stage;
  m_job.failureReason = detail;
  const auto next = timedOut ? UavCollaborationJobState::TimedOut :
                               UavCollaborationJobState::Failed;
  if (m_mission.hasJob(m_job.requestId)) {
    if (!m_mission.updateJob(m_job.requestId, next, stage, detail, reason)) return false;
  }
  m_job.state = next;
  return true;
}

bool
UavIncidentCoordinator::retryAnalysis(UavCollaborationFailureStage stage,
                                       const ndn::Name& newRequestId,
                                       const std::string& newAttemptId,
                                       uint64_t nowMs, std::string* reason)
{
  if (!m_started || (m_job.state != UavCollaborationJobState::Failed &&
                     m_job.state != UavCollaborationJobState::TimedOut) ||
      !isIdempotentAnalysisFailure(stage) || newRequestId.empty() ||
      newAttemptId.empty() || m_retriesUsed >= m_config.maxAnalysisRetries ||
      m_incident.evidence.empty()) {
    if (reason) *reason = "analysis retry is not permitted for this failure";
    return false;
  }
  if (!withinDeadline(nowMs, reason)) {
    return false;
  }
  if (!m_mission.beginIncidentAttempt(m_incident.incidentId, newAttemptId, reason)) {
    return false;
  }
  ++m_retriesUsed;
  m_incident.currentAttemptId = newAttemptId;
  m_job = UavCollaborationJobRecord();
  m_started = false;
  return begin(newRequestId, nowMs, reason);
}

} // namespace ndnsf::examples::uav
