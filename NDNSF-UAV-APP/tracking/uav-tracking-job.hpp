#ifndef NDNSF_UAV_TRACKING_JOB_HPP
#define NDNSF_UAV_TRACKING_JOB_HPP

#include "uav-tracking-config.hpp"
#include "uav-tracking-presentation.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace ndn { class Face; }
namespace ndn_service_framework { class ServiceUser; }

namespace ndnsf::examples::uav {

enum class TrackingAdmission { ACCEPTED, BUSY, INVALID, UNAUTHORIZED, NOT_READY };
enum class TrackingJobState { IDLE, PREPARING, RUNNING, CANCELLING, COMPLETED, FAILED, CANCELLED };
enum class TrackingCancelResult {
  REQUESTED, ALREADY_REQUESTED, ALREADY_TERMINAL, WRONG_JOB, UNAUTHORIZED
};

struct TrackingSnapshot
{
  std::string jobId;
  std::string runId;
  std::string missionId;
  std::string wireMissionId;
  TrackingMode mode = TrackingMode::VEHICLE;
  uint64_t generation = 0;
  TrackingJobState state = TrackingJobState::IDLE;
  uint64_t completedWindows = 0;
  uint64_t totalWindows = 0;
  uint64_t updatedMonotonicMs = 0;
  int64_t sourcePtsUs = 0;
  std::string activeRequestId;
  bool waitingForWorkDrain = false;
  std::vector<ndn::Name> selectedProviders;
  std::string lastResultDigest;
  std::shared_ptr<const std::string> latestResult;
  std::shared_ptr<const TrackingPresentation> presentation;
  std::string actualDevice = "unknown";
  std::string failureStage;
  std::string reason;
  std::string cancelState = "NONE";
  std::string cleanupState = "NOT_STARTED";
};

struct TrackingCleanupResult
{
  std::string jobId;
  std::string cancelState;
  std::string cleanupState;
  std::string reason;
};

/** Face-thread owner of one job; borrowed communication objects outlive stop completion. */
class UavTrackingJob
{
public:
  using SnapshotHandler = std::function<void(const TrackingSnapshot&)>;
  using AuthorityCheck = std::function<bool(const TrackingJobDefinition&, std::string*)>;
  UavTrackingJob(ndn_service_framework::ServiceUser& user, ndn::Face& face,
                 AuthorityCheck checkAuthority, SnapshotHandler onSnapshot);
  ~UavTrackingJob();

  UavTrackingJob(const UavTrackingJob&) = delete;
  UavTrackingJob& operator=(const UavTrackingJob&) = delete;

  TrackingAdmission start(const TrackingJobDefinition& definition, std::string* reason = nullptr);
  TrackingCancelResult requestCancel(const std::string& jobId);
  TrackingSnapshot snapshot() const;
  /** Local dependency drain; INCOMPLETE remote cleanup can still prohibit Start. */
  bool isDrained() const;
  void stop(std::function<void(const TrackingCleanupResult&)> onDrained);

private:
  friend int runTrackingGroundStationCli(int argc, char** argv);
  struct Impl;
  ndn_service_framework::ServiceUser& m_user;
  ndn::Face& m_face;
  AuthorityCheck m_checkAuthority;
  SnapshotHandler m_onSnapshot;
  std::shared_ptr<Impl> m_impl;
  uint64_t m_generation = 0;
};

/** Compatibility process entry; the executable itself owns process shutdown. */
int
runTrackingGroundStationCli(int argc, char** argv);

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_TRACKING_JOB_HPP
