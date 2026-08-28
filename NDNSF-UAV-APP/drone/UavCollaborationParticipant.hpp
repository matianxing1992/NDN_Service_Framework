#ifndef NDNSF_EXAMPLES_UAV_COLLABORATION_PARTICIPANT_HPP
#define NDNSF_EXAMPLES_UAV_COLLABORATION_PARTICIPANT_HPP

#include "../shared/UavDetectorProvider.hpp"

#include <optional>

namespace ndnsf::examples::uav {

class UavCollaborationParticipant
{
public:
  UavCollaborationParticipant(ndn::Name providerIdentity,
                             std::string role,
                             std::optional<UavDetectorProvider> detector = std::nullopt);

  const ndn::Name& providerIdentity() const noexcept { return m_providerIdentity; }
  const std::string& role() const noexcept { return m_role; }

  /** Freeze a bounded producer-owned evidence object; bytes remain outside the job payload. */
  std::optional<UavEvidenceReference>
  freezeEvidence(const std::string& missionId, const std::string& incidentId,
                 const std::string& evidenceId, uint64_t version,
                 const std::string& streamId, uint64_t streamSessionEpoch,
                 uint64_t firstSequence, uint64_t lastSequence,
                 uint64_t windowStartMs, uint64_t windowEndMs,
                 const ndn::Buffer& bytes, uint64_t retentionDeadlineMs,
                 std::string contentType = "application/octet-stream",
                 std::string* reason = nullptr) const;

  bool validateAssignment(const UavCollaborationJobRecord& job,
                          const UavRoleAssignment& assignment,
                          std::string* reason = nullptr) const;

  std::optional<UavTerminalReport>
  executeDetector(const UavCollaborationJobRecord& job,
                  const UavVerifiedEvidence& evidence,
                  uint64_t reportVersion = 1,
                  std::string* reason = nullptr) const;

private:
  ndn::Name m_providerIdentity;
  std::string m_role;
  std::optional<UavDetectorProvider> m_detector;
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_COLLABORATION_PARTICIPANT_HPP
