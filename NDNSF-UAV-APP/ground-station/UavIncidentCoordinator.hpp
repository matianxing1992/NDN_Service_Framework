#ifndef NDNSF_EXAMPLES_UAV_INCIDENT_COORDINATOR_HPP
#define NDNSF_EXAMPLES_UAV_INCIDENT_COORDINATOR_HPP

#include "../shared/UavCollaborationPolicy.hpp"
#include "../shared/UavMissionSession.hpp"
#include "../shared/UavMultiViewRecognition.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

struct UavIncidentCoordinatorConfig
{
  uint64_t ackTimeoutMs = 1000;
  uint64_t globalDeadlineMs = 5000;
  std::size_t maxAssignments = 8;
  std::size_t maxEvidence = 8;
  std::size_t maxAnalysisRetries = 1;
  ndn::Name explicitGroundFallback;
};

enum class UavCollaborationFailureStage
{
  Discovery,
  AckClosure,
  Plan,
  Selection,
  Evidence,
  Execution,
  Report,
  Delivery,
};

const char* to_string(UavCollaborationFailureStage stage) noexcept;

bool isIdempotentAnalysisFailure(UavCollaborationFailureStage stage) noexcept;

/**
 * Request-scoped coordinator for one finite incident analysis.  It is an
 * application adapter around NDNSF's existing request/ACK/Selection/Response
 * API; it does not create a long-lived network session or accept endpoints.
 */
class UavIncidentCoordinator
{
public:
  UavIncidentCoordinator(UavMissionSession& mission,
                         UavIncidentRecord incident,
                         UavIncidentCoordinatorConfig config = {});

  bool begin(const ndn::Name& requestId, uint64_t nowMs,
             std::string* reason = nullptr);
  bool closeAcks(uint64_t nowMs, std::string* reason = nullptr);
  bool commitPlan(std::vector<UavRoleAssignment> assignments,
                  ndn::Name terminalOwner, std::string planDigest,
                  std::string* reason = nullptr);
  bool selectProvider(const ndn::Name& provider, std::string* reason = nullptr);
  bool markEvidenceReady(std::string* reason = nullptr);
  bool markExecuting(std::string* reason = nullptr);
  bool markReporting(std::string* reason = nullptr);
  bool acceptReport(const UavTerminalReport& report, uint64_t nowMs,
                    std::string* reason = nullptr);
  bool beginMultiViewJob(const MultiViewRecognitionJob& job, uint64_t nowMs,
                         std::string* reason = nullptr);
  bool acceptMultiViewResult(const FusedRecognitionResult& result, uint64_t nowMs,
                             std::string* reason = nullptr);
  bool fail(const std::string& stage, const std::string& detail,
            bool timedOut = false, std::string* reason = nullptr);
  bool retryAnalysis(UavCollaborationFailureStage stage,
                     const ndn::Name& newRequestId,
                     const std::string& newAttemptId,
                     uint64_t nowMs, std::string* reason = nullptr);

  std::size_t retriesUsed() const noexcept { return m_retriesUsed; }

  const UavCollaborationJobRecord& job() const noexcept { return m_job; }
  const UavIncidentRecord& incident() const noexcept { return m_incident; }
  const ndn::Name& selectedProvider() const noexcept { return m_selectedProvider; }

private:
  bool requireState(UavCollaborationJobState expected, std::string* reason) const;
  bool withinDeadline(uint64_t nowMs, std::string* reason) const;
  bool update(UavCollaborationJobState next, std::string* reason = nullptr);

private:
  UavMissionSession& m_mission;
  UavIncidentRecord m_incident;
  UavIncidentCoordinatorConfig m_config;
  UavCollaborationJobRecord m_job;
  ndn::Name m_selectedProvider;
  bool m_started = false;
  std::size_t m_retriesUsed = 0;
  std::optional<MultiViewRecognitionJob> m_multiViewJob;
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_INCIDENT_COORDINATOR_HPP
