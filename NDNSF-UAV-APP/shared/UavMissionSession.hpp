#ifndef NDNSF_EXAMPLES_UAV_MISSION_SESSION_HPP
#define NDNSF_EXAMPLES_UAV_MISSION_SESSION_HPP

#include "UavProtocol.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ndnsf::examples::uav {

/**
 * Application-owned patrol lifecycle.  This object deliberately does not
 * create an NDNSF session or a network endpoint: each job/request remains
 * finite and is recorded here only after an accepted result.
 */
class UavMissionSession
{
public:
  explicit UavMissionSession(UavMissionSessionRecord record,
                             std::size_t maxHistory = 256);

  static UavMissionSession create(std::string missionId,
                                  ndn::Name operatorIdentity,
                                  uint64_t deadlineMs,
                                  std::size_t maxHistory = 256);

  const UavMissionSessionRecord& record() const noexcept { return m_record; }

  bool transition(UavMissionSessionState next, std::string* reason = nullptr);
  bool start(std::string* reason = nullptr);
  bool markRecovering(std::string* reason = nullptr);
  bool reconcileVehicleAndStreams(std::string* reason = nullptr);
  bool cancel(std::string* reason = nullptr);

  bool addPart(UavMissionPartRecord part, std::string* reason = nullptr);
  bool assignPart(const std::string& partId, const ndn::Name& provider,
                  const std::string& attemptId, std::string* reason = nullptr);
  bool markPartExecuting(const std::string& partId, std::string* reason = nullptr);
  bool completePart(const std::string& partId, const std::string& attemptId,
                    const std::string& responseDigest,
                    std::vector<uint64_t> completedWaypoints,
                    std::string* reason = nullptr);
  bool completePart(const std::string& partId, const std::string& responseDigest,
                    std::vector<uint64_t> completedWaypoints,
                    std::string* reason = nullptr);
  bool markPartMissing(const std::string& partId, std::string* reason = nullptr);
  bool compensateMissingParts(std::string* reason = nullptr);

  bool bindStream(UavStreamBinding binding, std::string* reason = nullptr);
  bool recordIncident(UavIncidentRecord incident, std::string* reason = nullptr);
  bool beginIncidentAttempt(const std::string& incidentId,
                           const std::string& attemptId,
                           std::string* reason = nullptr);
  bool addJob(UavCollaborationJobRecord job, std::string* reason = nullptr);
  bool updateJob(const ndn::Name& requestId, UavCollaborationJobState state,
                 std::string failureStage = {}, std::string failureReason = {},
                 std::string* reason = nullptr);
  bool acceptTerminalReport(const UavTerminalReport& report,
                            std::string* reason = nullptr);

  bool canIssueFlightControl() const noexcept;
  bool hasPart(const std::string& partId) const noexcept;
  bool hasIncident(const std::string& incidentId) const noexcept;
  bool hasJob(const ndn::Name& requestId) const noexcept;

  /** Versioned, bounded application snapshot. */
  std::string snapshot() const;
  static std::optional<UavMissionSession>
  restore(const std::string& snapshot,
          std::size_t maxHistory = 256,
          std::string* reason = nullptr);

private:
  static bool allowedTransition(UavMissionSessionState from,
                                UavMissionSessionState to) noexcept;
  bool fail(std::string* reason, const std::string& message) const;
  void touch();

private:
  UavMissionSessionRecord m_record;
  std::size_t m_maxHistory = 256;
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_MISSION_SESSION_HPP
