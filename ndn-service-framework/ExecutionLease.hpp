#ifndef NDNSF_EXECUTION_LEASE_HPP
#define NDNSF_EXECUTION_LEASE_HPP

#include <ndn-cxx/encoding/buffer.hpp>

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ndn_service_framework {

namespace execution_lease_reason {
inline constexpr char OK[] = "OK";
inline constexpr char UNAVAILABLE[] = "LEASE_UNAVAILABLE";
inline constexpr char NOT_FOUND[] = "LEASE_NOT_FOUND";
inline constexpr char EXPIRED[] = "LEASE_EXPIRED";
inline constexpr char STALE_EPOCH[] = "LEASE_STALE_EPOCH";
inline constexpr char IDEMPOTENCY_CONFLICT[] = "LEASE_IDEMPOTENCY_CONFLICT";
inline constexpr char INVALID_TRANSITION[] = "LEASE_INVALID_TRANSITION";
inline constexpr char REQUESTER_MISMATCH[] = "LEASE_REQUESTER_MISMATCH";
inline constexpr char REQUEST_MISMATCH[] = "LEASE_REQUEST_MISMATCH";
inline constexpr char SERVICE_MISMATCH[] = "LEASE_SERVICE_MISMATCH";
inline constexpr char PLAN_MISMATCH[] = "LEASE_PLAN_MISMATCH";
inline constexpr char BINDING_MISMATCH[] = "LEASE_BINDING_MISMATCH";
inline constexpr char CAPACITY_REJECTED[] = "LEASE_CAPACITY_REJECTED";
inline constexpr char INTERNAL_ERROR[] = "LEASE_INTERNAL_ERROR";
} // namespace execution_lease_reason

enum class ExecutionLeaseState
{
  Prepared,
  Committed,
  Executing,
  Aborted,
  Released,
  Expired,
};

const char*
toString(ExecutionLeaseState state) noexcept;

struct GenericExecutionLease
{
  std::string schema = "ndnsf-execution-lease-v1";
  std::string leaseId;
  std::string providerName;
  std::string providerEpoch;
  std::string requesterName;
  std::string requestId;
  std::string serviceName;
  std::string planDigest;
  std::string resourceBindingSchema;
  ndn::Buffer resourceBindingProof;
  std::vector<std::string> conflictKeys;
  ExecutionLeaseState state = ExecutionLeaseState::Prepared;
  uint64_t expiresAtMs = 0;
  uint64_t executionDeadlineMs = 0;
  std::string idempotencyKey;
};

struct ExecutionLeaseBinding
{
  std::string requesterName;
  std::string requestId;
  std::string serviceName;
  std::string planDigest;
  std::string resourceBindingSchema;
  ndn::Buffer resourceBindingProof;
};

struct ExecutionLeaseResult
{
  bool status = false;
  std::string operation;
  std::string reasonCode;
  GenericExecutionLease lease;
  uint64_t retryAfterMs = 0;
  bool idempotentReplay = false;
};

struct ExecutionLeaseCounters
{
  uint64_t prepared = 0;
  uint64_t committed = 0;
  uint64_t activated = 0;
  uint64_t aborted = 0;
  uint64_t released = 0;
  uint64_t expired = 0;
  uint64_t renewed = 0;
  uint64_t idempotentReplay = 0;
  uint64_t conflict = 0;
  uint64_t staleEpoch = 0;
  uint64_t cleanupTimeout = 0;
  std::map<std::string, uint64_t> rejectedByReason;
  uint64_t activePrepared = 0;
  uint64_t activeCommitted = 0;
  uint64_t activeExecuting = 0;
};

class ProviderExecutionLeaseTable
{
public:
  explicit ProviderExecutionLeaseTable(std::string providerEpoch = {});

  const std::string&
  providerEpoch() const noexcept;

  ExecutionLeaseResult
  prepare(GenericExecutionLease lease, uint64_t nowMs);

  ExecutionLeaseResult
  commit(const std::string& leaseId, const std::string& providerEpoch,
         const std::string& requesterName, const std::string& idempotencyKey,
         uint64_t nowMs);

  ExecutionLeaseResult
  validateAndActivate(const std::string& leaseId,
                      const std::string& providerEpoch,
                      const ExecutionLeaseBinding& binding,
                      const std::string& idempotencyKey,
                      uint64_t nowMs,
                      uint64_t executionDeadlineMs);

  ExecutionLeaseResult
  validate(const std::string& leaseId,
           const std::string& providerEpoch,
           const ExecutionLeaseBinding& binding,
           uint64_t nowMs);

  ExecutionLeaseResult
  abort(const std::string& leaseId, const std::string& providerEpoch,
        const std::string& requesterName, const std::string& idempotencyKey,
        uint64_t nowMs);

  ExecutionLeaseResult
  renew(const std::string& leaseId, const std::string& providerEpoch,
        const std::string& requesterName, const std::string& idempotencyKey,
        uint64_t nowMs,
        uint64_t expiresAtMs);

  ExecutionLeaseResult
  release(const std::string& leaseId, const std::string& providerEpoch,
          const std::string& requesterName, const std::string& idempotencyKey,
          uint64_t nowMs);

  size_t
  cleanupExpired(uint64_t nowMs);

  std::optional<GenericExecutionLease>
  find(const std::string& leaseId) const;

  bool
  hasActiveConflictKey(const std::string& conflictKey, uint64_t nowMs);

  bool
  hasPinnedBindingProof(const ndn::Buffer& resourceBindingProof, uint64_t nowMs);

  ExecutionLeaseCounters
  counters(uint64_t nowMs);

private:
  struct ReplayRecord
  {
    std::string fingerprint;
    ExecutionLeaseResult result;
  };

  static bool
  isActive(ExecutionLeaseState state) noexcept;

  static bool
  bindingMatches(const GenericExecutionLease& lease,
                 const ExecutionLeaseBinding& binding,
                 std::string& reason);

  ExecutionLeaseResult
  reject(const std::string& operation, const std::string& reason,
         const GenericExecutionLease* lease = nullptr);

  ExecutionLeaseResult
  replayOrConflict(const std::string& operation,
                   const std::string& idempotencyKey,
                   const std::string& fingerprint,
                   bool& handled);

  void
  rememberReplay(const std::string& operation,
                 const std::string& idempotencyKey,
                 const std::string& fingerprint,
                 const ExecutionLeaseResult& result);

  bool
  expireIfNeeded(GenericExecutionLease& lease, uint64_t nowMs);

  size_t
  cleanupExpiredLocked(uint64_t nowMs);

private:
  mutable std::mutex m_mutex;
  std::string m_providerEpoch;
  uint64_t m_nextLeaseId = 1;
  std::map<std::string, GenericExecutionLease> m_leases;
  std::map<std::string, ReplayRecord> m_replays;
  std::map<std::string, std::deque<std::string>> m_waitersByConflictKey;
  std::map<std::string, uint64_t> m_waiterExpiresAt;
  ExecutionLeaseCounters m_counters;
};

} // namespace ndn_service_framework

#endif // NDNSF_EXECUTION_LEASE_HPP
