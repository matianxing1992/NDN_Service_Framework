#include "UavCollaborationParticipant.hpp"

#include "../shared/UavNames.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <stdexcept>

namespace ndnsf::examples::uav {

UavCollaborationParticipant::UavCollaborationParticipant(
  ndn::Name providerIdentity, std::string role,
  std::optional<UavDetectorProvider> detector)
  : m_providerIdentity(std::move(providerIdentity))
  , m_role(std::move(role))
  , m_detector(std::move(detector))
{
  if (m_providerIdentity.empty() || m_role.empty()) {
    throw std::invalid_argument("collaboration participant requires identity and role");
  }
}

std::optional<UavEvidenceReference>
UavCollaborationParticipant::freezeEvidence(
  const std::string& missionId, const std::string& incidentId,
  const std::string& evidenceId, uint64_t version,
  const std::string& streamId, uint64_t streamSessionEpoch,
  uint64_t firstSequence, uint64_t lastSequence,
  uint64_t windowStartMs, uint64_t windowEndMs,
  const ndn::Buffer& bytes, uint64_t retentionDeadlineMs,
  std::string contentType, std::string* reason) const
{
  if (m_role != "EvidenceSource" || missionId.empty() || incidentId.empty() ||
      evidenceId.empty() || streamId.empty() || streamSessionEpoch == 0 ||
      firstSequence > lastSequence || windowStartMs > windowEndMs ||
      retentionDeadlineMs < windowEndMs || contentType.empty()) {
    if (reason) *reason = "invalid bounded evidence window";
    return std::nullopt;
  }
  UavEvidenceReference evidence;
  evidence.producerIdentity = m_providerIdentity;
  evidence.streamId = streamId;
  evidence.streamSessionEpoch = streamSessionEpoch;
  evidence.firstSequence = firstSequence;
  evidence.lastSequence = lastSequence;
  evidence.windowStartMs = windowStartMs;
  evidence.windowEndMs = windowEndMs;
  evidence.version = version;
  evidence.exactDataName = makeUavEvidenceName(
    m_providerIdentity, missionId, incidentId, evidenceId, version);
  ndn::util::Sha256 digest;
  if (!bytes.empty()) {
    digest << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  evidence.contentDigest = "sha256:" + digest.toString();
  evidence.contentType = std::move(contentType);
  evidence.retentionDeadlineMs = retentionDeadlineMs;
  if (!evidence.isValid(reason)) return std::nullopt;
  return evidence;
}

bool
UavCollaborationParticipant::validateAssignment(const UavCollaborationJobRecord& job,
                                                const UavRoleAssignment& assignment,
                                                std::string* reason) const
{
  if (assignment.providerIdentity != m_providerIdentity || assignment.role != m_role ||
      job.requestId.empty() || job.missionId.empty() || job.incidentId.empty() ||
      job.attemptId.empty() || assignment.evidence.size() > 8) {
    if (reason) *reason = "participant assignment mismatch";
    return false;
  }
  if (assignment.terminalResponseOwner != (m_role == "DetectorReporter") ||
      (assignment.terminalResponseOwner && job.terminalOwner != m_providerIdentity)) {
    if (reason) *reason = "invalid terminal role ownership";
    return false;
  }
  for (const auto& evidence : assignment.evidence) {
    if (!evidence.isValid(reason)) return false;
    if (evidence.producerIdentity.empty()) {
      if (reason) *reason = "evidence producer identity missing";
      return false;
    }
  }
  return true;
}

std::optional<UavTerminalReport>
UavCollaborationParticipant::executeDetector(
  const UavCollaborationJobRecord& job, const UavVerifiedEvidence& verifiedEvidence,
  uint64_t reportVersion,
  std::string* reason) const
{
  const auto& evidence = verifiedEvidence.reference;
  if (m_role != "DetectorReporter" || !m_detector ||
      !m_detector->ready() || job.terminalOwner != m_providerIdentity ||
      reportVersion == 0 || !evidence.isValid(reason)) {
    if (reason && reason->empty()) *reason = "detector participant is not ready or authorized";
    return std::nullopt;
  }
  const auto assignment = std::find_if(job.assignments.begin(), job.assignments.end(),
                                       [&](const auto& value) {
                                         return value.providerIdentity == m_providerIdentity &&
                                                value.role == "DetectorReporter";
                                       });
  if (assignment == job.assignments.end() ||
      std::find_if(assignment->evidence.begin(), assignment->evidence.end(),
                   [&](const auto& value) {
                     return value.exactDataName == evidence.exactDataName &&
                            value.contentDigest == evidence.contentDigest;
                   }) == assignment->evidence.end()) {
    if (reason) *reason = "evidence is not assigned to detector participant";
    return std::nullopt;
  }
  const auto execution = m_detector->execute(verifiedEvidence);
  if (!execution.success) {
    if (reason) *reason = execution.detail;
    return std::nullopt;
  }
  UavTerminalReport report;
  report.missionId = job.missionId;
  report.incidentId = job.incidentId;
  report.attemptId = job.attemptId;
  report.requestId = job.requestId;
  report.planDigest = job.planDigest;
  report.terminalOwner = m_providerIdentity;
  report.selectedProvider = m_providerIdentity;
  report.modelId = m_detector->config().modelId;
  report.modelDigest = m_detector->config().modelDigest;
  report.evidence.push_back(evidence);
  report.reportName = makeUavReportName(m_providerIdentity, job.missionId,
                                        job.incidentId, job.attemptId, reportVersion);
  report.resultDigest = execution.resultDigest;
  report.status = "success";
  if (!report.isValid(reason)) return std::nullopt;
  return report;
}

} // namespace ndnsf::examples::uav
