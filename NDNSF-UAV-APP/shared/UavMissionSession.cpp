#include "UavMissionSession.hpp"
#include "UavNames.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <ndn-cxx/util/sha256.hpp>

namespace ndnsf::examples::uav {

namespace {

bool
parseUint(const Fields& fields, const std::string& key, uint64_t& value)
{
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) {
    return false;
  }
  const auto* first = it->second.data();
  const auto* last = first + it->second.size();
  const auto parsed = std::from_chars(first, last, value);
  return parsed.ec == std::errc{} && parsed.ptr == last;
}

std::string
joinUint(const std::vector<uint64_t>& values)
{
  std::ostringstream os;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) os << ',';
    os << values[i];
  }
  return os.str();
}

bool
parseUintList(const std::string& input, std::vector<uint64_t>& values)
{
  values.clear();
  if (input.empty()) return true;
  size_t start = 0;
  while (start < input.size()) {
    const auto end = input.find(',', start);
    const auto token = input.substr(start, end == std::string::npos ? end : end - start);
    uint64_t value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (token.empty() || parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
      return false;
    }
    values.push_back(value);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return true;
}

bool
parseBool(const Fields& fields, const std::string& key, bool& value)
{
  const auto it = fields.find(key);
  if (it == fields.end()) return false;
  if (it->second == "true") { value = true; return true; }
  if (it->second == "false") { value = false; return true; }
  return false;
}

std::string
digestSnapshotFields(const Fields& fields)
{
  const auto encoded = encodeFields(fields);
  ndn::util::Sha256 digest;
  digest << encoded;
  return "sha256:" + digest.toString();
}

std::string
field(const Fields& fields, const std::string& key)
{
  const auto it = fields.find(key);
  return it == fields.end() ? std::string() : it->second;
}

bool
sameName(const ndn::Name& a, const ndn::Name& b)
{
  return !a.empty() && a == b;
}

} // namespace

UavMissionSession
UavMissionSession::create(std::string missionId,
                           ndn::Name operatorIdentity,
                           uint64_t deadlineMs,
                           std::size_t maxHistory)
{
  if (missionId.empty() || operatorIdentity.empty() || deadlineMs == 0) {
    throw std::invalid_argument("mission session requires identity and deadline");
  }
  UavMissionSessionRecord record;
  record.missionId = std::move(missionId);
  record.operatorIdentity = std::move(operatorIdentity);
  record.createdAtMs = nowMilliseconds();
  record.updatedAtMs = record.createdAtMs;
  record.deadlineMs = deadlineMs;
  return UavMissionSession(std::move(record), maxHistory);
}

UavMissionSession::UavMissionSession(UavMissionSessionRecord record,
                                     std::size_t maxHistory)
  : m_record(std::move(record))
  , m_maxHistory(std::max<std::size_t>(1, maxHistory))
{
  if (m_record.missionId.empty() || m_record.operatorIdentity.empty() ||
      m_record.deadlineMs == 0 || m_record.parts.size() > m_maxHistory ||
      m_record.incidents.size() > m_maxHistory || m_record.jobs.size() > m_maxHistory) {
    throw std::invalid_argument("invalid or unbounded mission session record");
  }
}

bool
UavMissionSession::allowedTransition(UavMissionSessionState from,
                                     UavMissionSessionState to) noexcept
{
  if (from == to) return true;
  switch (from) {
  case UavMissionSessionState::Planned:
    return to == UavMissionSessionState::Starting || to == UavMissionSessionState::Failed;
  case UavMissionSessionState::Starting:
    return to == UavMissionSessionState::Active || to == UavMissionSessionState::Failed;
  case UavMissionSessionState::Active:
    return to == UavMissionSessionState::Degraded || to == UavMissionSessionState::Compensating ||
           to == UavMissionSessionState::Cancelling || to == UavMissionSessionState::Recovering ||
           to == UavMissionSessionState::Completed || to == UavMissionSessionState::Failed;
  case UavMissionSessionState::Degraded:
    return to == UavMissionSessionState::Active || to == UavMissionSessionState::Compensating ||
           to == UavMissionSessionState::Cancelling || to == UavMissionSessionState::Recovering ||
           to == UavMissionSessionState::Failed;
  case UavMissionSessionState::Compensating:
    return to == UavMissionSessionState::Active || to == UavMissionSessionState::Degraded ||
           to == UavMissionSessionState::Failed;
  case UavMissionSessionState::Cancelling:
    return to == UavMissionSessionState::Cancelled || to == UavMissionSessionState::Failed;
  case UavMissionSessionState::Recovering:
    return to == UavMissionSessionState::Active || to == UavMissionSessionState::Degraded ||
           to == UavMissionSessionState::Failed;
  case UavMissionSessionState::Completed:
  case UavMissionSessionState::Cancelled:
  case UavMissionSessionState::Failed:
    return false;
  }
  return false;
}

bool
UavMissionSession::fail(std::string* reason, const std::string& message) const
{
  if (reason != nullptr) *reason = message;
  return false;
}

void
UavMissionSession::touch()
{
  m_record.updatedAtMs = nowMilliseconds();
}

bool
UavMissionSession::transition(UavMissionSessionState next, std::string* reason)
{
  if (!allowedTransition(m_record.state, next)) {
    return fail(reason, "illegal mission session transition");
  }
  m_record.state = next;
  touch();
  return true;
}

bool
UavMissionSession::start(std::string* reason)
{
  if (!transition(UavMissionSessionState::Starting, reason) ||
      !transition(UavMissionSessionState::Active, reason)) {
    return false;
  }
  return true;
}

bool
UavMissionSession::markRecovering(std::string* reason)
{
  if (!canIssueFlightControl() && m_record.state != UavMissionSessionState::Active &&
      m_record.state != UavMissionSessionState::Degraded) {
    return fail(reason, "mission is not recoverable from its current state");
  }
  return transition(UavMissionSessionState::Recovering, reason);
}

bool
UavMissionSession::reconcileVehicleAndStreams(std::string* reason)
{
  if (m_record.state != UavMissionSessionState::Recovering) {
    return fail(reason, "reconciliation requires RECOVERING state");
  }
  for (const auto& stream : m_record.streams) {
    if (stream.active && stream.producerIdentity.empty()) {
      return fail(reason, "active stream has no producer identity");
    }
  }
  return transition(UavMissionSessionState::Active, reason);
}

bool
UavMissionSession::cancel(std::string* reason)
{
  if (!transition(UavMissionSessionState::Cancelling, reason)) return false;
  return transition(UavMissionSessionState::Cancelled, reason);
}

bool
UavMissionSession::addPart(UavMissionPartRecord part, std::string* reason)
{
  if (part.partId.empty() || hasPart(part.partId)) return fail(reason, "duplicate or empty mission part");
  if (m_record.parts.size() >= m_maxHistory) return fail(reason, "mission part history bound exceeded");
  m_record.parts.push_back(std::move(part));
  touch();
  return true;
}

bool
UavMissionSession::assignPart(const std::string& partId, const ndn::Name& provider,
                              const std::string& attemptId, std::string* reason)
{
  if (provider.empty() || attemptId.empty()) return fail(reason, "assignment requires provider and attempt");
  auto it = std::find_if(m_record.parts.begin(), m_record.parts.end(),
                         [&](const auto& part) { return part.partId == partId; });
  if (it == m_record.parts.end()) return fail(reason, "unknown mission part");
  if (it->state != UavMissionPartState::Pending && it->state != UavMissionPartState::Missing) {
    return fail(reason, "mission part is not assignable");
  }
  it->assignedProvider = provider;
  it->attemptId = attemptId;
  it->state = UavMissionPartState::Accepted;
  touch();
  return true;
}

bool
UavMissionSession::markPartExecuting(const std::string& partId, std::string* reason)
{
  auto it = std::find_if(m_record.parts.begin(), m_record.parts.end(),
                         [&](const auto& part) { return part.partId == partId; });
  if (it == m_record.parts.end() || it->state != UavMissionPartState::Accepted) {
    return fail(reason, "mission part is not accepted");
  }
  it->state = UavMissionPartState::Executing;
  touch();
  return true;
}

bool
UavMissionSession::completePart(const std::string& partId, const std::string& attemptId,
                                const std::string& responseDigest,
                                std::vector<uint64_t> completedWaypoints,
                                std::string* reason)
{
  auto it = std::find_if(m_record.parts.begin(), m_record.parts.end(),
                         [&](const auto& part) { return part.partId == partId; });
  if (it == m_record.parts.end() ||
      (it->state != UavMissionPartState::Accepted && it->state != UavMissionPartState::Executing) ||
      attemptId.empty() || it->attemptId != attemptId ||
      responseDigest.rfind("sha256:", 0) != 0) {
    return fail(reason, "mission part completion is invalid or stale");
  }
  std::sort(completedWaypoints.begin(), completedWaypoints.end());
  completedWaypoints.erase(std::unique(completedWaypoints.begin(), completedWaypoints.end()),
                           completedWaypoints.end());
  if (!std::includes(completedWaypoints.begin(), completedWaypoints.end(),
                     it->completedWaypoints.begin(), it->completedWaypoints.end())) {
    return fail(reason, "mission progress is not monotonic");
  }
  it->completedWaypoints = std::move(completedWaypoints);
  it->responseDigest = responseDigest;
  it->state = UavMissionPartState::Completed;
  touch();
  return true;
}

bool
UavMissionSession::completePart(const std::string& partId, const std::string& responseDigest,
                                std::vector<uint64_t> completedWaypoints,
                                std::string* reason)
{
  const auto it = std::find_if(m_record.parts.begin(), m_record.parts.end(),
                               [&](const auto& part) { return part.partId == partId; });
  if (it == m_record.parts.end()) {
    return fail(reason, "unknown mission part");
  }
  return completePart(partId, it->attemptId, responseDigest,
                      std::move(completedWaypoints), reason);
}

bool
UavMissionSession::markPartMissing(const std::string& partId, std::string* reason)
{
  auto it = std::find_if(m_record.parts.begin(), m_record.parts.end(),
                         [&](const auto& part) { return part.partId == partId; });
  if (it == m_record.parts.end() || it->state == UavMissionPartState::Completed ||
      it->state == UavMissionPartState::Compensated) {
    return fail(reason, "completed mission work cannot be marked missing");
  }
  it->state = UavMissionPartState::Missing;
  touch();
  return true;
}

bool
UavMissionSession::compensateMissingParts(std::string* reason)
{
  bool changed = false;
  for (auto& part : m_record.parts) {
    if (part.state == UavMissionPartState::Missing) {
      part.state = UavMissionPartState::Compensated;
      changed = true;
    }
  }
  if (!changed) return fail(reason, "no missing mission parts to compensate");
  touch();
  return true;
}

bool
UavMissionSession::bindStream(UavStreamBinding binding, std::string* reason)
{
  if (binding.streamId.empty() || binding.producerIdentity.empty() || binding.sessionEpoch == 0) {
    return fail(reason, "invalid stream binding");
  }
  const auto it = std::find_if(m_record.streams.begin(), m_record.streams.end(),
                               [&](const auto& stream) { return stream.streamId == binding.streamId; });
  if (it == m_record.streams.end()) {
    if (m_record.streams.size() >= m_maxHistory) return fail(reason, "stream history bound exceeded");
    m_record.streams.push_back(std::move(binding));
  }
  else {
    if (binding.sessionEpoch < it->sessionEpoch) return fail(reason, "stream session moved backwards");
    *it = std::move(binding);
  }
  touch();
  return true;
}

bool
UavMissionSession::recordIncident(UavIncidentRecord incident, std::string* reason)
{
  if (incident.incidentId.empty() || incident.missionId != m_record.missionId ||
      incident.evidence.size() > m_maxHistory ||
      hasIncident(incident.incidentId)) return fail(reason, "invalid or duplicate incident");
  if (m_record.incidents.size() >= m_maxHistory) return fail(reason, "incident history bound exceeded");
  for (const auto& evidence : incident.evidence) {
    if (!evidence.isValid(reason) ||
        !isUavProducerDataNameForContext(evidence.producerIdentity,
                                         evidence.exactDataName, "EVIDENCE",
                                         incident.missionId, incident.incidentId)) {
      return fail(reason, "incident evidence name is not bound to mission and incident");
    }
  }
  m_record.incidents.push_back(std::move(incident));
  touch();
  return true;
}

bool
UavMissionSession::beginIncidentAttempt(const std::string& incidentId,
                                        const std::string& attemptId,
                                        std::string* reason)
{
  if (m_record.state != UavMissionSessionState::Active &&
      m_record.state != UavMissionSessionState::Degraded) {
    return fail(reason, "incident retry requires an active mission");
  }
  if (incidentId.empty() || attemptId.empty()) {
    return fail(reason, "incident retry requires incident and attempt identities");
  }
  auto it = std::find_if(m_record.incidents.begin(), m_record.incidents.end(),
                         [&](const auto& incident) {
                           return incident.incidentId == incidentId;
                         });
  if (it == m_record.incidents.end()) {
    return fail(reason, "unknown incident for retry");
  }
  if (!it->acceptedTerminalReportDigest.empty() || it->currentAttemptId == attemptId) {
    return fail(reason, "incident attempt is already terminal or duplicate");
  }
  it->currentAttemptId = attemptId;
  touch();
  return true;
}

bool
UavMissionSession::addJob(UavCollaborationJobRecord job, std::string* reason)
{
  if (job.requestId.empty() || job.missionId != m_record.missionId || job.incidentId.empty() ||
      !hasIncident(job.incidentId) || job.attemptId.empty() || hasJob(job.requestId)) {
    return fail(reason, "invalid or duplicate collaboration job");
  }
  if (job.assignments.empty() || job.assignments.size() > m_maxHistory) {
    return fail(reason, "collaboration assignment bound exceeded");
  }
  size_t terminalOwners = 0;
  for (const auto& assignment : job.assignments) {
    if (assignment.providerIdentity.empty() ||
        (assignment.role != "EvidenceSource" && assignment.role != "DetectorReporter")) {
      return fail(reason, "invalid collaboration assignment");
    }
    terminalOwners += assignment.terminalResponseOwner ? 1 : 0;
    if (assignment.terminalResponseOwner && assignment.providerIdentity != job.terminalOwner) {
      return fail(reason, "terminal owner does not match terminal assignment");
    }
    if (assignment.evidence.size() > m_maxHistory) {
      return fail(reason, "assignment evidence bound exceeded");
    }
    for (const auto& evidence : assignment.evidence) {
      if (!evidence.isValid(reason) ||
          !isUavProducerDataNameForContext(evidence.producerIdentity,
                                           evidence.exactDataName, "EVIDENCE",
                                           job.missionId, job.incidentId)) {
        return fail(reason, "assignment evidence name is not bound to job lineage");
      }
    }
  }
  if (terminalOwners != 1 || job.terminalOwner.empty() || job.globalDeadlineMs == 0 ||
      job.ackDeadlineMs > job.globalDeadlineMs) {
    return fail(reason, "collaboration job must have one bounded terminal owner");
  }
  if (m_record.jobs.size() >= m_maxHistory) return fail(reason, "job history bound exceeded");
  m_record.jobs.push_back(std::move(job));
  touch();
  return true;
}

namespace {

bool
allowedJobTransition(UavCollaborationJobState from, UavCollaborationJobState to) noexcept
{
  if (from == to) return true;
  switch (from) {
  case UavCollaborationJobState::Created:
    return to == UavCollaborationJobState::AckCollecting ||
           to == UavCollaborationJobState::Failed ||
           to == UavCollaborationJobState::TimedOut ||
           to == UavCollaborationJobState::Cancelled;
  case UavCollaborationJobState::AckCollecting:
    return to == UavCollaborationJobState::AckClosed ||
           to == UavCollaborationJobState::Failed ||
           to == UavCollaborationJobState::TimedOut ||
           to == UavCollaborationJobState::Cancelled;
  case UavCollaborationJobState::AckClosed:
    return to == UavCollaborationJobState::PlanCommitted ||
           to == UavCollaborationJobState::Failed ||
           to == UavCollaborationJobState::TimedOut ||
           to == UavCollaborationJobState::Cancelled;
  case UavCollaborationJobState::PlanCommitted:
    return to == UavCollaborationJobState::Selected ||
           to == UavCollaborationJobState::Failed ||
           to == UavCollaborationJobState::TimedOut ||
           to == UavCollaborationJobState::Cancelled;
  case UavCollaborationJobState::Selected:
    return to == UavCollaborationJobState::EvidenceReady ||
           to == UavCollaborationJobState::Failed ||
           to == UavCollaborationJobState::TimedOut ||
           to == UavCollaborationJobState::Cancelled;
  case UavCollaborationJobState::EvidenceReady:
    return to == UavCollaborationJobState::Executing ||
           to == UavCollaborationJobState::Failed ||
           to == UavCollaborationJobState::TimedOut ||
           to == UavCollaborationJobState::Cancelled;
  case UavCollaborationJobState::Executing:
    return to == UavCollaborationJobState::Reporting ||
           to == UavCollaborationJobState::Failed ||
           to == UavCollaborationJobState::TimedOut ||
           to == UavCollaborationJobState::Cancelled;
  case UavCollaborationJobState::Reporting:
    return to == UavCollaborationJobState::Succeeded ||
           to == UavCollaborationJobState::Failed ||
           to == UavCollaborationJobState::TimedOut ||
           to == UavCollaborationJobState::Cancelled;
  case UavCollaborationJobState::Succeeded:
  case UavCollaborationJobState::Failed:
  case UavCollaborationJobState::TimedOut:
  case UavCollaborationJobState::Cancelled:
    return false;
  }
  return false;
}

} // namespace

bool
UavMissionSession::updateJob(const ndn::Name& requestId, UavCollaborationJobState state,
                             std::string failureStage, std::string failureReason,
                             std::string* reason)
{
  auto it = std::find_if(m_record.jobs.begin(), m_record.jobs.end(),
                         [&](const auto& job) { return job.requestId == requestId; });
  if (it == m_record.jobs.end()) return fail(reason, "unknown collaboration job");
  if (!allowedJobTransition(it->state, state)) {
    return fail(reason, "illegal collaboration job transition");
  }
  if (failureStage.size() > 128 || failureReason.size() > 512) {
    return fail(reason, "collaboration failure detail exceeds bound");
  }
  if (state == UavCollaborationJobState::Failed ||
      state == UavCollaborationJobState::TimedOut ||
      state == UavCollaborationJobState::Cancelled) {
    if (failureStage.empty()) return fail(reason, "terminal failure requires failure stage");
    it->failureStage = std::move(failureStage);
    it->failureReason = std::move(failureReason);
  }
  it->state = state;
  touch();
  return true;
}

bool
UavMissionSession::acceptTerminalReport(const UavTerminalReport& report, std::string* reason)
{
  if (!report.isValid(reason) || report.missionId != m_record.missionId) return false;
  auto incident = std::find_if(m_record.incidents.begin(), m_record.incidents.end(),
                               [&](const auto& value) { return value.incidentId == report.incidentId; });
  if (incident == m_record.incidents.end() || incident->currentAttemptId != report.attemptId ||
      !incident->acceptedTerminalReportDigest.empty()) {
    return fail(reason, "report incident lineage is stale or already accepted");
  }
  auto job = std::find_if(m_record.jobs.begin(), m_record.jobs.end(),
                          [&](const auto& value) { return value.requestId == report.requestId; });
  if (job == m_record.jobs.end() || job->incidentId != report.incidentId ||
      job->attemptId != report.attemptId || job->planDigest != report.planDigest ||
      job->state != UavCollaborationJobState::Reporting ||
      !sameName(job->terminalOwner, report.terminalOwner) ||
      !sameName(report.selectedProvider, report.terminalOwner)) {
    return fail(reason, "report job or terminal owner mismatch");
  }
  for (const auto& expected : incident->evidence) {
    const auto found = std::find_if(report.evidence.begin(), report.evidence.end(),
                                    [&](const auto& actual) {
                                      return actual.toFields() == expected.toFields() &&
                                             isUavProducerDataNameForContext(
                                               actual.producerIdentity, actual.exactDataName,
                                               "EVIDENCE", report.missionId, report.incidentId);
                                    });
    if (found == report.evidence.end()) return fail(reason, "report evidence mismatch");
  }
  incident->acceptedTerminalReportDigest = report.resultDigest;
  job->terminalReportDigest = report.resultDigest;
  job->state = report.status == "success" ? UavCollaborationJobState::Succeeded :
                                              UavCollaborationJobState::Failed;
  touch();
  return true;
}

bool
UavMissionSession::canIssueFlightControl() const noexcept
{
  return m_record.state == UavMissionSessionState::Active ||
         m_record.state == UavMissionSessionState::Degraded;
}

bool
UavMissionSession::hasPart(const std::string& partId) const noexcept
{
  return std::any_of(m_record.parts.begin(), m_record.parts.end(),
                     [&](const auto& part) { return part.partId == partId; });
}

bool
UavMissionSession::hasIncident(const std::string& incidentId) const noexcept
{
  return std::any_of(m_record.incidents.begin(), m_record.incidents.end(),
                     [&](const auto& incident) { return incident.incidentId == incidentId; });
}

bool
UavMissionSession::hasJob(const ndn::Name& requestId) const noexcept
{
  return std::any_of(m_record.jobs.begin(), m_record.jobs.end(),
                     [&](const auto& job) { return job.requestId == requestId; });
}

std::string
UavMissionSession::snapshot() const
{
  Fields fields{
    {"schema", "1"}, {"mission_id", m_record.missionId},
    {"plan_digest", m_record.planDigest}, {"operator", m_record.operatorIdentity.toUri()},
    {"state", to_string(m_record.state)}, {"created_ms", std::to_string(m_record.createdAtMs)},
    {"deadline_ms", std::to_string(m_record.deadlineMs)}, {"updated_ms", std::to_string(m_record.updatedAtMs)},
    {"parts_count", std::to_string(m_record.parts.size())},
    {"streams_count", std::to_string(m_record.streams.size())},
    {"incidents_count", std::to_string(m_record.incidents.size())},
    {"jobs_count", std::to_string(m_record.jobs.size())},
  };
  for (size_t i = 0; i < m_record.parts.size(); ++i) {
    const auto& part = m_record.parts[i];
    const auto p = "part." + std::to_string(i) + ".";
    fields[p + "id"] = part.partId;
    fields[p + "sector"] = part.sector;
    fields[p + "waypoint_digest"] = part.waypointDigest;
    fields[p + "attempt"] = part.attemptId;
    fields[p + "provider"] = part.assignedProvider.toUri();
    fields[p + "state"] = to_string(part.state);
    fields[p + "completed"] = joinUint(part.completedWaypoints);
    fields[p + "response_digest"] = part.responseDigest;
    fields[p + "vehicle_state"] = part.authoritativeVehicleState;
  }
  for (size_t i = 0; i < m_record.streams.size(); ++i) {
    const auto& stream = m_record.streams[i];
    const auto p = "stream." + std::to_string(i) + ".";
    fields[p + "id"] = stream.streamId;
    fields[p + "producer"] = stream.producerIdentity.toUri();
    fields[p + "epoch"] = std::to_string(stream.sessionEpoch);
    fields[p + "first"] = std::to_string(stream.firstCursor);
    fields[p + "last"] = std::to_string(stream.lastCursor);
    fields[p + "active"] = stream.active ? "true" : "false";
  }
  for (size_t i = 0; i < m_record.incidents.size(); ++i) {
    const auto& incident = m_record.incidents[i];
    const auto p = "incident." + std::to_string(i) + ".";
    fields[p + "id"] = incident.incidentId;
    fields[p + "mission"] = incident.missionId;
    fields[p + "trigger_kind"] = incident.triggerKind;
    fields[p + "trigger_ms"] = std::to_string(incident.triggerTimeMs);
    fields[p + "location"] = incident.location;
    fields[p + "capability"] = incident.requestedCapability;
    fields[p + "attempt"] = incident.currentAttemptId;
    fields[p + "accepted_digest"] = incident.acceptedTerminalReportDigest;
    fields[p + "evidence_count"] = std::to_string(incident.evidence.size());
    for (size_t j = 0; j < incident.evidence.size(); ++j) {
      const auto evidenceFields = incident.evidence[j].toFields(p + "evidence." + std::to_string(j));
      fields.insert(evidenceFields.begin(), evidenceFields.end());
    }
  }
  for (size_t i = 0; i < m_record.jobs.size(); ++i) {
    const auto& job = m_record.jobs[i];
    const auto p = "job." + std::to_string(i) + ".";
    fields[p + "request"] = job.requestId.toUri();
    fields[p + "mission"] = job.missionId;
    fields[p + "incident"] = job.incidentId;
    fields[p + "attempt"] = job.attemptId;
    fields[p + "ack_deadline"] = std::to_string(job.ackDeadlineMs);
    fields[p + "global_deadline"] = std::to_string(job.globalDeadlineMs);
    fields[p + "plan_digest"] = job.planDigest;
    fields[p + "terminal_owner"] = job.terminalOwner.toUri();
    fields[p + "state"] = to_string(job.state);
    fields[p + "failure_stage"] = job.failureStage;
    fields[p + "failure_reason"] = job.failureReason;
    fields[p + "fallback"] = job.fallbackMode;
    fields[p + "terminal_report_digest"] = job.terminalReportDigest;
  }
  fields["snapshot_digest"] = digestSnapshotFields(fields);
  return encodeFields(fields);
}

std::optional<UavMissionSession>
UavMissionSession::restore(const std::string& encoded,
                           std::size_t maxHistory,
                           std::string* reason)
{
  try {
    const auto fields = decodeFields(encoded);
    if (field(fields, "schema") != "1") return std::nullopt;
    const auto digest = field(fields, "snapshot_digest");
    if (digest.empty()) return std::nullopt;
    auto unsignedFields = fields;
    unsignedFields.erase("snapshot_digest");
    if (digestSnapshotFields(unsignedFields) != digest) {
      if (reason) *reason = "snapshot digest mismatch";
      return std::nullopt;
    }
    UavMissionSessionRecord record;
    record.missionId = field(fields, "mission_id");
    record.planDigest = field(fields, "plan_digest");
    record.operatorIdentity = ndn::Name(field(fields, "operator"));
    const auto state = parseUavMissionSessionState(field(fields, "state"));
    if (!state) return std::nullopt;
    record.state = *state;
    if (!parseUint(fields, "created_ms", record.createdAtMs) ||
        !parseUint(fields, "deadline_ms", record.deadlineMs) ||
        !parseUint(fields, "updated_ms", record.updatedAtMs)) return std::nullopt;
    uint64_t parts = 0, streams = 0, incidents = 0, jobs = 0;
    if (!parseUint(fields, "parts_count", parts) || !parseUint(fields, "streams_count", streams) ||
        !parseUint(fields, "incidents_count", incidents) || !parseUint(fields, "jobs_count", jobs) ||
        parts > maxHistory || streams > maxHistory || incidents > maxHistory || jobs > maxHistory) {
      return std::nullopt;
    }
    for (uint64_t i = 0; i < parts; ++i) {
      const auto p = "part." + std::to_string(i) + ".";
      UavMissionPartRecord part;
      part.partId = field(fields, p + "id");
      part.sector = field(fields, p + "sector");
      part.waypointDigest = field(fields, p + "waypoint_digest");
      part.attemptId = field(fields, p + "attempt");
      part.assignedProvider = ndn::Name(field(fields, p + "provider"));
      const auto partState = parseUavMissionPartState(field(fields, p + "state"));
      if (!partState || !parseUintList(field(fields, p + "completed"), part.completedWaypoints)) return std::nullopt;
      part.state = *partState;
      part.responseDigest = field(fields, p + "response_digest");
      part.authoritativeVehicleState = field(fields, p + "vehicle_state");
      record.parts.push_back(std::move(part));
    }
    for (uint64_t i = 0; i < streams; ++i) {
      const auto p = "stream." + std::to_string(i) + ".";
      UavStreamBinding stream;
      stream.streamId = field(fields, p + "id");
      stream.producerIdentity = ndn::Name(field(fields, p + "producer"));
      if (!parseUint(fields, p + "epoch", stream.sessionEpoch) ||
          !parseUint(fields, p + "first", stream.firstCursor) ||
          !parseUint(fields, p + "last", stream.lastCursor) || !parseBool(fields, p + "active", stream.active)) return std::nullopt;
      record.streams.push_back(std::move(stream));
    }
    for (uint64_t i = 0; i < incidents; ++i) {
      const auto p = "incident." + std::to_string(i) + ".";
      UavIncidentRecord incident;
      incident.incidentId = field(fields, p + "id");
      incident.missionId = field(fields, p + "mission");
      incident.triggerKind = field(fields, p + "trigger_kind");
      if (!parseUint(fields, p + "trigger_ms", incident.triggerTimeMs)) return std::nullopt;
      incident.location = field(fields, p + "location");
      incident.requestedCapability = field(fields, p + "capability");
      incident.currentAttemptId = field(fields, p + "attempt");
      incident.acceptedTerminalReportDigest = field(fields, p + "accepted_digest");
      uint64_t evidenceCount = 0;
      if (!parseUint(fields, p + "evidence_count", evidenceCount) || evidenceCount > maxHistory) return std::nullopt;
      for (uint64_t j = 0; j < evidenceCount; ++j) {
        const auto ep = p + "evidence." + std::to_string(j);
        UavEvidenceReference evidence;
        evidence.producerIdentity = ndn::Name(field(fields, ep + ".producer"));
        evidence.streamId = field(fields, ep + ".stream_id");
        if (!parseUint(fields, ep + ".stream_session", evidence.streamSessionEpoch) ||
            !parseUint(fields, ep + ".first_sequence", evidence.firstSequence) ||
            !parseUint(fields, ep + ".last_sequence", evidence.lastSequence) ||
            !parseUint(fields, ep + ".window_start_ms", evidence.windowStartMs) ||
            !parseUint(fields, ep + ".window_end_ms", evidence.windowEndMs)) return std::nullopt;
        evidence.exactDataName = ndn::Name(field(fields, ep + ".exact_name"));
        if (!parseUint(fields, ep + ".version", evidence.version) ||
            !parseUint(fields, ep + ".retention_deadline_ms", evidence.retentionDeadlineMs)) return std::nullopt;
        evidence.contentDigest = field(fields, ep + ".content_digest");
        evidence.contentType = field(fields, ep + ".content_type");
        if (!evidence.isValid(reason)) return std::nullopt;
        incident.evidence.push_back(std::move(evidence));
      }
      record.incidents.push_back(std::move(incident));
    }
    for (uint64_t i = 0; i < jobs; ++i) {
      const auto p = "job." + std::to_string(i) + ".";
      UavCollaborationJobRecord job;
      job.requestId = ndn::Name(field(fields, p + "request"));
      job.missionId = field(fields, p + "mission");
      job.incidentId = field(fields, p + "incident");
      job.attemptId = field(fields, p + "attempt");
      if (!parseUint(fields, p + "ack_deadline", job.ackDeadlineMs) ||
          !parseUint(fields, p + "global_deadline", job.globalDeadlineMs)) return std::nullopt;
      job.planDigest = field(fields, p + "plan_digest");
      job.terminalOwner = ndn::Name(field(fields, p + "terminal_owner"));
      const auto jobState = parseUavCollaborationJobState(field(fields, p + "state"));
      if (!jobState) return std::nullopt;
      job.state = *jobState;
      job.failureStage = field(fields, p + "failure_stage");
      job.failureReason = field(fields, p + "failure_reason");
      job.fallbackMode = field(fields, p + "fallback");
      job.terminalReportDigest = field(fields, p + "terminal_report_digest");
      // The assignment list is reconstructed by the coordinator; snapshots
      // retain its terminal owner and job lineage even if no role callback is
      // still active after restart.
      record.jobs.push_back(std::move(job));
    }
    if (record.state != UavMissionSessionState::Recovering) {
      record.state = UavMissionSessionState::Recovering;
    }
    return UavMissionSession(std::move(record), maxHistory);
  }
  catch (const std::exception& error) {
    if (reason) *reason = error.what();
    return std::nullopt;
  }
}

} // namespace ndnsf::examples::uav
