#ifndef NDNSF_UAV_APP_UAV_TRACKING_WORKER_PROCESS_HPP
#define NDNSF_UAV_APP_UAV_TRACKING_WORKER_PROCESS_HPP

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

enum class UavTrackingWorkerOutcome
{
  SUCCESS,
  SPAWN_FAILED,
  CHILD_FAILURE,
  CANCELLED,
  TIMEOUT,
  OUTPUT_LIMIT,
};

struct UavTrackingWorkerProcessResult
{
  UavTrackingWorkerOutcome outcome = UavTrackingWorkerOutcome::SPAWN_FAILED;
  int exitCode = -1;
  int systemError = 0;
  std::string stdoutText;
  std::string stderrText;
};

/**
 * Run literal argv in an owned process group while the synchronous caller remains alive.
 * Each captured stream is independently bounded. Timeout/cancellation/output exhaustion stops
 * the group; all exits reap the leader and release pipes. OS/callback exceptions also clean up.
 * The caller retains Core authorization and transaction/commit ownership.
 */
UavTrackingWorkerProcessResult
runUavTrackingWorkerProcess(const std::vector<std::string>& argv,
                            std::chrono::milliseconds timeout,
                            size_t maxOutputBytes,
                            const std::function<bool()>& stillAuthorized);

/**
 * One lazily spawned, synchronous local backend. The caller serializes exchanges and destruction.
 * Success acknowledges a complete line, not process exit (exitCode remains -1 while alive).
 * Each stream has a cumulative session bound. Every failed exchange permanently stops the session.
 * No authorization callback or application state is retained between exchanges.
 */
class UavTrackingWorkerSession
{
public:
  UavTrackingWorkerSession(std::vector<std::string> argv, size_t maxOutputBytes);
  ~UavTrackingWorkerSession();
  UavTrackingWorkerSession(const UavTrackingWorkerSession&) = delete;
  UavTrackingWorkerSession& operator=(const UavTrackingWorkerSession&) = delete;

  UavTrackingWorkerProcessResult
  exchange(const std::string& requestLine, const std::string& completionLine,
           std::chrono::milliseconds timeout, const std::function<bool()>& stillAuthorized);

  void
  close() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_APP_UAV_TRACKING_WORKER_PROCESS_HPP
