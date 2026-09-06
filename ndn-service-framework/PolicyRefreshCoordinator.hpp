#ifndef NDN_SERVICE_FRAMEWORK_POLICY_REFRESH_COORDINATOR_HPP
#define NDN_SERVICE_FRAMEWORK_POLICY_REFRESH_COORDINATOR_HPP

#include "PolicyStatus.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ndn_service_framework {

/**
 * Bounded, single-flight coordination for Controller-signed PolicyStatus
 * refreshes.  This class deliberately does not authenticate a Data packet;
 * the caller supplies the result of its configured trust-schema check.
 *
 * It owns only refresh bookkeeping.  ControllerVersion authority remains the
 * accepted PolicyStatus and never comes from an unauthenticated peer hint.
 */
class PolicyRefreshCoordinator
{
public:
  enum class Outcome
  {
    REJECTED_UNAUTHENTICATED_HINT,
    IGNORED_CURRENT,
    FETCH_STARTED,
    FETCH_COALESCED,
    ACCEPTED,
    REJECTED_STATUS,
    RETRY_SCHEDULED,
    EXHAUSTED,
  };

  struct FetchRequest
  {
    ControllerVersion version;
    size_t attempt = 0;
  };

  struct Result
  {
    Outcome outcome = Outcome::IGNORED_CURRENT;
    std::optional<FetchRequest> request;
    const char* reason = "";
  };

  explicit PolicyRefreshCoordinator(ndn::Name serviceName,
                                    size_t maxAttempts = 3,
                                    uint64_t retryBackoffMs = 1000);

  /** Install the first/current signed status before processing hints. */
  bool installCurrentStatus(const PolicyStatusData& status,
                            uint64_t nowMs,
                            bool controllerSignatureValid = true);

  /** Process a version hint only after authenticating the enclosing message. */
  Result observeHint(const ControllerVersion& hintedVersion,
                     bool authenticated,
                     uint64_t nowMs);

  /** Start a hintless refresh when the accepted status is near expiry. */
  Result startScheduledRefresh(uint64_t nowMs, uint64_t leadMs);

  /** Complete the current fetch with a Controller-signed status snapshot. */
  Result completeFetch(const PolicyStatusData& status,
                       uint64_t nowMs,
                       bool controllerSignatureValid = true);

  /** Record a timeout/transport failure and return a bounded retry request. */
  Result failFetch(uint64_t nowMs);

  /** Return the retry request once its bounded backoff has elapsed. */
  std::optional<FetchRequest> retryIfDue(uint64_t nowMs) const;

  bool hasCurrentStatus() const;
  const ControllerVersion& currentVersion() const;
  uint64_t currentExpiryMs() const;
  bool hasExpiredStatus(uint64_t nowMs) const;
  bool inFlight() const;
  size_t attempt() const;
  size_t maxAttempts() const;
  const std::optional<ControllerVersion>& highestCandidate() const;

private:
  Result startFetch(const ControllerVersion& version, uint64_t nowMs);
  Result rejectOrRetry(Outcome rejection, const char* reason, uint64_t nowMs);
  bool sameStatus(const PolicyStatusData& left,
                  const PolicyStatusData& right) const;
  uint64_t retryDelayMs(size_t attempt) const;

private:
  ndn::Name m_serviceName;
  bool m_hasStatus = false;
  PolicyStatusData m_status;
  size_t m_maxAttempts = 3;
  uint64_t m_retryBackoffMs = 1000;
  bool m_inFlight = false;
  size_t m_attempt = 0;
  uint64_t m_nextRetryAtMs = 0;
  std::optional<ControllerVersion> m_highestCandidate;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_POLICY_REFRESH_COORDINATOR_HPP
