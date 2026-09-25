#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

struct TrackingWindow
{
  std::string requestId;
  uint64_t epoch = 0;
  uint64_t index = 0;
  std::string inputDigest;
  uint64_t sourceStartUs = 0;
  uint64_t sourceEndUs = 0;
};

struct TrackingResult
{
  std::string requestId;
  uint64_t epoch = 0;
  uint64_t index = 0;
  std::string inputDigest;
  std::string resultDigest;
  bool cached = false;
};

enum class TrackingSessionState
{
  Created,
  Active,
  Completed,
  Aborted,
  Closed,
};

class UavTrackingSession
{
public:
  using Executor = std::function<std::string(const TrackingWindow&)>;

  explicit UavTrackingSession(std::string sessionId, size_t resultCacheLimit = 2,
                              uint64_t maxWindows = 60);
  std::optional<TrackingResult> process(const TrackingWindow& window,
                                        const Executor& executor,
                                        std::string* reason = nullptr);
  void abort(std::string reason);
  void close();
  TrackingSessionState state() const noexcept { return m_state; }
  uint64_t nextWindow() const noexcept { return m_nextWindow; }
  const std::string& abortReason() const noexcept { return m_abortReason; }
  size_t executedCount() const noexcept { return m_executedCount; }

private:
  bool reject(const std::string& reason, std::string* out) const;

private:
  std::string m_sessionId;
  size_t m_resultCacheLimit;
  uint64_t m_maxWindows;
  TrackingSessionState m_state = TrackingSessionState::Created;
  uint64_t m_epoch = 0;
  uint64_t m_nextWindow = 0;
  size_t m_executedCount = 0;
  std::string m_abortReason;
  std::map<std::string, TrackingResult> m_cache;
  std::deque<std::string> m_cacheOrder;
};

} // namespace ndnsf::examples::uav
