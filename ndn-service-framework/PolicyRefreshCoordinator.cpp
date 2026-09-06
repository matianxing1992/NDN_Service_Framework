#include "PolicyRefreshCoordinator.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace ndn_service_framework {

PolicyRefreshCoordinator::PolicyRefreshCoordinator(ndn::Name serviceName,
                                                   size_t maxAttempts,
                                                   uint64_t retryBackoffMs)
  : m_serviceName(std::move(serviceName))
  , m_maxAttempts(std::max<size_t>(1, maxAttempts))
  , m_retryBackoffMs(retryBackoffMs)
{
}

bool
PolicyRefreshCoordinator::sameStatus(const PolicyStatusData& left,
                                      const PolicyStatusData& right) const
{
  try {
    const auto lhs = left.wireEncode();
    const auto rhs = right.wireEncode();
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin());
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
PolicyRefreshCoordinator::installCurrentStatus(const PolicyStatusData& status,
                                                uint64_t nowMs,
                                                bool controllerSignatureValid)
{
  if (!controllerSignatureValid || !status.validate(nowMs) ||
      (!m_serviceName.empty() && status.getServiceName() != m_serviceName)) {
    return false;
  }
  if (m_hasStatus) {
    const int relation = status.getControllerVersion().compare(
        m_status.getControllerVersion());
    if (relation < 0 || (relation == 0 && !sameStatus(status, m_status))) {
      return false;
    }
  }
  m_status = status;
  m_hasStatus = true;
  m_inFlight = false;
  m_attempt = 0;
  m_nextRetryAtMs = 0;
  if (m_highestCandidate &&
      m_highestCandidate->compare(m_status.getControllerVersion()) <= 0) {
    m_highestCandidate.reset();
  }
  return true;
}

PolicyRefreshCoordinator::Result
PolicyRefreshCoordinator::startFetch(const ControllerVersion& version,
                                      uint64_t nowMs)
{
  m_inFlight = true;
  m_attempt = 1;
  m_nextRetryAtMs = nowMs;
  m_highestCandidate = version;
  return {Outcome::FETCH_STARTED, FetchRequest{version, m_attempt},
          "refresh_fetch_started"};
}

PolicyRefreshCoordinator::Result
PolicyRefreshCoordinator::observeHint(const ControllerVersion& hintedVersion,
                                      bool authenticated,
                                      uint64_t nowMs)
{
  if (!authenticated || !hintedVersion.isValid()) {
    return {Outcome::REJECTED_UNAUTHENTICATED_HINT, std::nullopt,
            "unauthenticated_or_invalid_version_hint"};
  }
  if (m_hasStatus && hintedVersion.compare(m_status.getControllerVersion()) <= 0) {
    return {Outcome::IGNORED_CURRENT, std::nullopt, "hint_not_newer"};
  }
  if (m_inFlight) {
    if (!m_highestCandidate ||
        hintedVersion.compare(*m_highestCandidate) > 0) {
      m_highestCandidate = hintedVersion;
    }
    return {Outcome::FETCH_COALESCED, std::nullopt,
            "refresh_fetch_already_in_flight"};
  }
  return startFetch(hintedVersion, nowMs);
}

PolicyRefreshCoordinator::Result
PolicyRefreshCoordinator::startScheduledRefresh(uint64_t nowMs,
                                                 uint64_t leadMs)
{
  if (m_inFlight) {
    return {Outcome::FETCH_COALESCED, std::nullopt,
            "refresh_fetch_already_in_flight"};
  }
  if (!m_hasStatus) {
    return {Outcome::IGNORED_CURRENT, std::nullopt, "no_current_status"};
  }
  const uint64_t expiry = m_status.getValidUntilMs();
  if (expiry > nowMs && expiry - nowMs > leadMs) {
    return {Outcome::IGNORED_CURRENT, std::nullopt,
            "status_not_near_expiry"};
  }
  return startFetch(m_status.getControllerVersion(), nowMs);
}

uint64_t
PolicyRefreshCoordinator::retryDelayMs(size_t attempt) const
{
  if (m_retryBackoffMs == 0 || attempt <= 1) {
    return m_retryBackoffMs;
  }
  const size_t shift = std::min<size_t>(attempt - 1, 62);
  const uint64_t factor = uint64_t{1} << shift;
  if (m_retryBackoffMs > std::numeric_limits<uint64_t>::max() / factor) {
    return std::numeric_limits<uint64_t>::max();
  }
  return m_retryBackoffMs * factor;
}

PolicyRefreshCoordinator::Result
PolicyRefreshCoordinator::rejectOrRetry(Outcome rejection,
                                        const char* reason,
                                        uint64_t nowMs)
{
  if (m_attempt >= m_maxAttempts) {
    m_inFlight = false;
    m_nextRetryAtMs = 0;
    return {Outcome::EXHAUSTED, std::nullopt, reason};
  }
  ++m_attempt;
  const auto delay = retryDelayMs(m_attempt);
  m_nextRetryAtMs = nowMs > std::numeric_limits<uint64_t>::max() - delay
      ? std::numeric_limits<uint64_t>::max() : nowMs + delay;
  return {rejection, FetchRequest{m_highestCandidate.value_or(
                                    m_status.getControllerVersion()), m_attempt},
          reason};
}

PolicyRefreshCoordinator::Result
PolicyRefreshCoordinator::completeFetch(const PolicyStatusData& status,
                                        uint64_t nowMs,
                                        bool controllerSignatureValid)
{
  if (!m_inFlight) {
    return {Outcome::REJECTED_STATUS, std::nullopt,
            "refresh_completion_without_fetch"};
  }
  const bool valid = controllerSignatureValid && status.validate(nowMs) &&
                     (m_serviceName.empty() || status.getServiceName() == m_serviceName) &&
                     (!m_highestCandidate ||
                      status.getControllerVersion().compare(*m_highestCandidate) >= 0);
  if (!valid) {
    return rejectOrRetry(Outcome::REJECTED_STATUS,
                         "invalid_or_stale_controller_status", nowMs);
  }
  if (m_hasStatus && status.getControllerVersion() == m_status.getControllerVersion() &&
      !sameStatus(status, m_status)) {
    return rejectOrRetry(Outcome::REJECTED_STATUS,
                         "conflicting_equal_version_status", nowMs);
  }
  m_status = status;
  m_hasStatus = true;
  m_inFlight = false;
  m_attempt = 0;
  m_nextRetryAtMs = 0;
  if (m_highestCandidate &&
      m_highestCandidate->compare(status.getControllerVersion()) <= 0) {
    m_highestCandidate.reset();
  }
  return {Outcome::ACCEPTED, std::nullopt, "controller_status_accepted"};
}

PolicyRefreshCoordinator::Result
PolicyRefreshCoordinator::failFetch(uint64_t nowMs)
{
  if (!m_inFlight) {
    return {Outcome::EXHAUSTED, std::nullopt, "refresh_failure_without_fetch"};
  }
  return rejectOrRetry(Outcome::RETRY_SCHEDULED,
                       "controller_status_fetch_failed", nowMs);
}

std::optional<PolicyRefreshCoordinator::FetchRequest>
PolicyRefreshCoordinator::retryIfDue(uint64_t nowMs) const
{
  if (!m_inFlight || m_attempt == 0 || nowMs < m_nextRetryAtMs ||
      !m_highestCandidate) {
    return std::nullopt;
  }
  return FetchRequest{*m_highestCandidate, m_attempt};
}

bool PolicyRefreshCoordinator::hasCurrentStatus() const { return m_hasStatus; }

const ControllerVersion& PolicyRefreshCoordinator::currentVersion() const
{
  static const ControllerVersion invalid;
  return m_hasStatus ? m_status.getControllerVersion() : invalid;
}

uint64_t PolicyRefreshCoordinator::currentExpiryMs() const
{
  return m_hasStatus ? m_status.getValidUntilMs() : 0;
}

bool PolicyRefreshCoordinator::hasExpiredStatus(uint64_t nowMs) const
{
  return m_hasStatus && nowMs >= m_status.getValidUntilMs();
}

bool PolicyRefreshCoordinator::inFlight() const { return m_inFlight; }
size_t PolicyRefreshCoordinator::attempt() const { return m_attempt; }
size_t PolicyRefreshCoordinator::maxAttempts() const { return m_maxAttempts; }

const std::optional<ControllerVersion>&
PolicyRefreshCoordinator::highestCandidate() const
{
  return m_highestCandidate;
}

} // namespace ndn_service_framework
