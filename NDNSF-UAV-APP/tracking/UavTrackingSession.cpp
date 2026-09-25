#include "UavTrackingSession.hpp"

#include <stdexcept>
#include <utility>

namespace ndnsf::examples::uav {

UavTrackingSession::UavTrackingSession(std::string sessionId, size_t resultCacheLimit,
                                       uint64_t maxWindows)
  : m_sessionId(std::move(sessionId))
  , m_resultCacheLimit(resultCacheLimit)
  , m_maxWindows(maxWindows)
{
  if (m_sessionId.empty() || m_resultCacheLimit == 0 || m_maxWindows == 0) {
    throw std::invalid_argument("invalid tracking session bounds");
  }
}

bool
UavTrackingSession::reject(const std::string& reason, std::string* out) const
{
  if (out) *out = reason;
  return false;
}

std::optional<TrackingResult>
UavTrackingSession::process(const TrackingWindow& window, const Executor& executor,
                            std::string* reason)
{
  if (m_state == TrackingSessionState::Aborted) {
    reject("session is aborted", reason);
    return std::nullopt;
  }
  if (m_state == TrackingSessionState::Closed) {
    reject("session is closed", reason);
    return std::nullopt;
  }
  if (!executor || window.requestId.empty() || window.inputDigest.empty() ||
      window.epoch == 0 || window.sourceStartUs > window.sourceEndUs) {
    reject("invalid tracking window", reason);
    return std::nullopt;
  }
  if (m_state == TrackingSessionState::Completed) {
    reject("session reached its window limit", reason);
    return std::nullopt;
  }
  if (m_epoch == 0) m_epoch = window.epoch;
  if (window.epoch != m_epoch) {
    reject("window epoch mismatch", reason);
    return std::nullopt;
  }
  const auto cached = m_cache.find(window.requestId);
  if (cached != m_cache.end()) {
    if (cached->second.inputDigest != window.inputDigest) {
      reject("request ID was reused with different input", reason);
      return std::nullopt;
    }
    auto result = cached->second;
    result.cached = true;
    return result;
  }
  if (window.index != m_nextWindow) {
    reject("window is not the next sequential window", reason);
    return std::nullopt;
  }
  m_state = TrackingSessionState::Active;
  std::string resultDigest;
  try {
    resultDigest = executor(window);
  }
  catch (const std::exception& error) {
    abort(std::string("executor failed: ") + error.what());
    reject(m_abortReason, reason);
    return std::nullopt;
  }
  catch (...) {
    abort("executor failed with an unknown exception");
    reject(m_abortReason, reason);
    return std::nullopt;
  }
  TrackingResult result{window.requestId, window.epoch, window.index, window.inputDigest,
                        std::move(resultDigest), false};
  if (result.resultDigest.empty()) {
    abort("executor returned an empty result digest");
    reject(m_abortReason, reason);
    return std::nullopt;
  }
  ++m_executedCount;
  ++m_nextWindow;
  m_cache.emplace(window.requestId, result);
  m_cacheOrder.push_back(window.requestId);
  while (m_cacheOrder.size() > m_resultCacheLimit) {
    m_cache.erase(m_cacheOrder.front());
    m_cacheOrder.pop_front();
  }
  if (m_nextWindow >= m_maxWindows) m_state = TrackingSessionState::Completed;
  return result;
}

void
UavTrackingSession::abort(std::string reason)
{
  if (m_state == TrackingSessionState::Closed) return;
  m_state = TrackingSessionState::Aborted;
  m_abortReason = std::move(reason);
}

void
UavTrackingSession::close()
{
  m_state = TrackingSessionState::Closed;
}

} // namespace ndnsf::examples::uav
