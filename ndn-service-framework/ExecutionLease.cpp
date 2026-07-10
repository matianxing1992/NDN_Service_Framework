#include "ExecutionLease.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <sstream>
#include <utility>

namespace ndn_service_framework {
namespace {

std::string
makeEpoch()
{
  static std::atomic<uint64_t> sequence{0};
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  return "epoch-" + std::to_string(ticks) + "-" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

std::string
bufferFingerprint(const ndn::Buffer& value)
{
  static constexpr char hex[] = "0123456789abcdef";
  std::string output;
  output.reserve(value.size() * 2);
  for (uint8_t byte : value) {
    output.push_back(hex[byte >> 4]);
    output.push_back(hex[byte & 0x0f]);
  }
  return output;
}

std::string
join(const std::vector<std::string>& values)
{
  std::ostringstream output;
  for (const auto& value : values) {
    output << value.size() << ':' << value << ';';
  }
  return output.str();
}

std::string
prepareFingerprint(const GenericExecutionLease& lease)
{
  std::ostringstream output;
  output << lease.providerName << '\n' << lease.requesterName << '\n'
         << lease.requestId << '\n' << lease.serviceName << '\n'
         << lease.planDigest << '\n' << lease.resourceBindingSchema << '\n'
         << bufferFingerprint(lease.resourceBindingProof) << '\n'
         << join(lease.conflictKeys) << '\n' << lease.expiresAtMs;
  return output.str();
}

std::string
operationFingerprint(const std::string& leaseId,
                     const std::string& providerEpoch,
                     uint64_t value = 0)
{
  return leaseId + '\n' + providerEpoch + '\n' + std::to_string(value);
}

std::string
bindingFingerprint(const ExecutionLeaseBinding& binding)
{
  return binding.requesterName + '\n' + binding.requestId + '\n' +
         binding.serviceName + '\n' + binding.planDigest + '\n' +
         binding.resourceBindingSchema + '\n' +
         bufferFingerprint(binding.resourceBindingProof);
}

ExecutionLeaseResult
success(std::string operation, const GenericExecutionLease& lease)
{
  ExecutionLeaseResult result;
  result.status = true;
  result.operation = std::move(operation);
  result.reasonCode = "OK";
  result.lease = lease;
  return result;
}

} // namespace

const char*
toString(ExecutionLeaseState state) noexcept
{
  switch (state) {
    case ExecutionLeaseState::Prepared: return "PREPARED";
    case ExecutionLeaseState::Committed: return "COMMITTED";
    case ExecutionLeaseState::Executing: return "EXECUTING";
    case ExecutionLeaseState::Aborted: return "ABORTED";
    case ExecutionLeaseState::Released: return "RELEASED";
    case ExecutionLeaseState::Expired: return "EXPIRED";
  }
  return "UNKNOWN";
}

ProviderExecutionLeaseTable::ProviderExecutionLeaseTable(std::string providerEpoch)
  : m_providerEpoch(providerEpoch.empty() ? makeEpoch() : std::move(providerEpoch))
{
}

const std::string&
ProviderExecutionLeaseTable::providerEpoch() const noexcept
{
  return m_providerEpoch;
}

bool
ProviderExecutionLeaseTable::isActive(ExecutionLeaseState state) noexcept
{
  return state == ExecutionLeaseState::Prepared ||
         state == ExecutionLeaseState::Committed ||
         state == ExecutionLeaseState::Executing;
}

bool
ProviderExecutionLeaseTable::bindingMatches(const GenericExecutionLease& lease,
                                            const ExecutionLeaseBinding& binding,
                                            std::string& reason)
{
  if (lease.requesterName != binding.requesterName) {
    reason = "LEASE_REQUESTER_MISMATCH";
  }
  else if (lease.requestId != binding.requestId) {
    reason = "LEASE_REQUEST_MISMATCH";
  }
  else if (lease.serviceName != binding.serviceName) {
    reason = "LEASE_SERVICE_MISMATCH";
  }
  else if (lease.planDigest != binding.planDigest) {
    reason = "LEASE_PLAN_MISMATCH";
  }
  else if (lease.resourceBindingSchema != binding.resourceBindingSchema ||
           lease.resourceBindingProof != binding.resourceBindingProof) {
    reason = "LEASE_BINDING_MISMATCH";
  }
  else {
    return true;
  }
  return false;
}

ExecutionLeaseResult
ProviderExecutionLeaseTable::reject(const std::string& operation,
                                    const std::string& reason,
                                    const GenericExecutionLease* lease)
{
  ++m_counters.rejectedByReason[reason];
  if (reason == "LEASE_CAPACITY_REJECTED") {
    ++m_counters.conflict;
  }
  if (reason == "LEASE_STALE_EPOCH") {
    ++m_counters.staleEpoch;
  }
  ExecutionLeaseResult result;
  result.operation = operation;
  result.reasonCode = reason;
  if (lease != nullptr) {
    result.lease = *lease;
  }
  return result;
}

ExecutionLeaseResult
ProviderExecutionLeaseTable::replayOrConflict(const std::string& operation,
                                              const std::string& idempotencyKey,
                                              const std::string& fingerprint,
                                              bool& handled)
{
  handled = true;
  if (idempotencyKey.empty()) {
    return reject(operation, "LEASE_IDEMPOTENCY_CONFLICT");
  }
  const auto key = operation + '\n' + idempotencyKey;
  const auto it = m_replays.find(key);
  if (it == m_replays.end()) {
    handled = false;
    return {};
  }
  if (it->second.fingerprint != fingerprint) {
    return reject(operation, "LEASE_IDEMPOTENCY_CONFLICT",
                  &it->second.result.lease);
  }
  auto result = it->second.result;
  result.idempotentReplay = true;
  ++m_counters.idempotentReplay;
  return result;
}

void
ProviderExecutionLeaseTable::rememberReplay(const std::string& operation,
                                            const std::string& idempotencyKey,
                                            const std::string& fingerprint,
                                            const ExecutionLeaseResult& result)
{
  m_replays[operation + '\n' + idempotencyKey] = {fingerprint, result};
}

bool
ProviderExecutionLeaseTable::expireIfNeeded(GenericExecutionLease& lease,
                                            uint64_t nowMs)
{
  if (!isActive(lease.state)) {
    return false;
  }
  const bool timedOut = lease.state == ExecutionLeaseState::Executing
                          ? lease.executionDeadlineMs != 0 &&
                              nowMs >= lease.executionDeadlineMs
                          : lease.expiresAtMs != 0 && nowMs >= lease.expiresAtMs;
  if (!timedOut) {
    return false;
  }
  lease.state = ExecutionLeaseState::Expired;
  ++m_counters.expired;
  ++m_counters.cleanupTimeout;
  return true;
}

size_t
ProviderExecutionLeaseTable::cleanupExpiredLocked(uint64_t nowMs)
{
  size_t expired = 0;
  for (auto& item : m_leases) {
    if (expireIfNeeded(item.second, nowMs)) {
      ++expired;
    }
  }
  return expired;
}

ExecutionLeaseResult
ProviderExecutionLeaseTable::prepare(GenericExecutionLease lease, uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  cleanupExpiredLocked(nowMs);
  const auto fingerprint = prepareFingerprint(lease);
  bool handled = false;
  auto replay = replayOrConflict("PREPARE", lease.idempotencyKey,
                                 fingerprint, handled);
  if (handled) {
    return replay;
  }
  if (lease.providerName.empty() || lease.requesterName.empty() ||
      lease.requestId.empty() || lease.serviceName.empty() ||
      lease.planDigest.empty() || lease.resourceBindingSchema.empty() ||
      lease.resourceBindingProof.empty() || lease.expiresAtMs <= nowMs) {
    return reject("PREPARE", "LEASE_UNAVAILABLE");
  }
  for (const auto& conflictKey : lease.conflictKeys) {
    if (conflictKey.empty()) {
      return reject("PREPARE", "LEASE_CAPACITY_REJECTED");
    }
    for (const auto& item : m_leases) {
      if (isActive(item.second.state) &&
          std::find(item.second.conflictKeys.begin(), item.second.conflictKeys.end(),
                    conflictKey) != item.second.conflictKeys.end()) {
        return reject("PREPARE", "LEASE_CAPACITY_REJECTED", &item.second);
      }
    }
  }
  if (lease.leaseId.empty()) {
    lease.leaseId = m_providerEpoch + "-lease-" + std::to_string(m_nextLeaseId++);
  }
  else if (m_leases.count(lease.leaseId) != 0) {
    return reject("PREPARE", "LEASE_IDEMPOTENCY_CONFLICT",
                  &m_leases.at(lease.leaseId));
  }
  lease.providerEpoch = m_providerEpoch;
  lease.state = ExecutionLeaseState::Prepared;
  m_leases.emplace(lease.leaseId, lease);
  ++m_counters.prepared;
  auto result = success("PREPARE", lease);
  rememberReplay("PREPARE", lease.idempotencyKey, fingerprint, result);
  return result;
}

ExecutionLeaseResult
ProviderExecutionLeaseTable::commit(const std::string& leaseId,
                                    const std::string& providerEpoch,
                                    const std::string& idempotencyKey,
                                    uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  cleanupExpiredLocked(nowMs);
  const auto fingerprint = operationFingerprint(leaseId, providerEpoch);
  bool handled = false;
  auto replay = replayOrConflict("COMMIT", idempotencyKey, fingerprint, handled);
  if (handled) return replay;
  auto it = m_leases.find(leaseId);
  if (it == m_leases.end()) return reject("COMMIT", "LEASE_NOT_FOUND");
  auto& lease = it->second;
  if (providerEpoch != m_providerEpoch || lease.providerEpoch != providerEpoch) {
    return reject("COMMIT", "LEASE_STALE_EPOCH", &lease);
  }
  if (lease.state == ExecutionLeaseState::Expired) {
    return reject("COMMIT", "LEASE_EXPIRED", &lease);
  }
  if (lease.state != ExecutionLeaseState::Prepared) {
    return reject("COMMIT", "LEASE_INVALID_TRANSITION", &lease);
  }
  lease.state = ExecutionLeaseState::Committed;
  ++m_counters.committed;
  auto result = success("COMMIT", lease);
  rememberReplay("COMMIT", idempotencyKey, fingerprint, result);
  return result;
}

ExecutionLeaseResult
ProviderExecutionLeaseTable::validateAndActivate(
  const std::string& leaseId, const std::string& providerEpoch,
  const ExecutionLeaseBinding& binding, const std::string& idempotencyKey,
  uint64_t nowMs, uint64_t executionDeadlineMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  cleanupExpiredLocked(nowMs);
  const auto fingerprint = operationFingerprint(leaseId, providerEpoch,
                                                executionDeadlineMs) + '\n' +
                           bindingFingerprint(binding);
  bool handled = false;
  auto replay = replayOrConflict("VALIDATE_AND_ACTIVATE", idempotencyKey,
                                 fingerprint, handled);
  if (handled) return replay;
  auto it = m_leases.find(leaseId);
  if (it == m_leases.end()) {
    return reject("VALIDATE_AND_ACTIVATE", "LEASE_NOT_FOUND");
  }
  auto& lease = it->second;
  if (providerEpoch != m_providerEpoch || lease.providerEpoch != providerEpoch) {
    return reject("VALIDATE_AND_ACTIVATE", "LEASE_STALE_EPOCH", &lease);
  }
  if (lease.state == ExecutionLeaseState::Expired) {
    return reject("VALIDATE_AND_ACTIVATE", "LEASE_EXPIRED", &lease);
  }
  if (lease.state != ExecutionLeaseState::Committed) {
    return reject("VALIDATE_AND_ACTIVATE", "LEASE_INVALID_TRANSITION", &lease);
  }
  std::string reason;
  if (!bindingMatches(lease, binding, reason)) {
    return reject("VALIDATE_AND_ACTIVATE", reason, &lease);
  }
  if (executionDeadlineMs <= nowMs) {
    return reject("VALIDATE_AND_ACTIVATE", "LEASE_EXPIRED", &lease);
  }
  lease.state = ExecutionLeaseState::Executing;
  lease.executionDeadlineMs = executionDeadlineMs;
  ++m_counters.activated;
  auto result = success("VALIDATE_AND_ACTIVATE", lease);
  rememberReplay("VALIDATE_AND_ACTIVATE", idempotencyKey, fingerprint, result);
  return result;
}

ExecutionLeaseResult
ProviderExecutionLeaseTable::validate(const std::string& leaseId,
                                      const std::string& providerEpoch,
                                      const ExecutionLeaseBinding& binding,
                                      uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  cleanupExpiredLocked(nowMs);
  auto it = m_leases.find(leaseId);
  if (it == m_leases.end()) return reject("VALIDATE", "LEASE_NOT_FOUND");
  auto& lease = it->second;
  if (providerEpoch != m_providerEpoch || lease.providerEpoch != providerEpoch) {
    return reject("VALIDATE", "LEASE_STALE_EPOCH", &lease);
  }
  if (lease.state == ExecutionLeaseState::Expired) {
    return reject("VALIDATE", "LEASE_EXPIRED", &lease);
  }
  if (lease.state != ExecutionLeaseState::Committed) {
    return reject("VALIDATE", "LEASE_INVALID_TRANSITION", &lease);
  }
  std::string reason;
  if (!bindingMatches(lease, binding, reason)) {
    return reject("VALIDATE", reason, &lease);
  }
  return success("VALIDATE", lease);
}

ExecutionLeaseResult
ProviderExecutionLeaseTable::abort(const std::string& leaseId,
                                   const std::string& providerEpoch,
                                   const std::string& idempotencyKey,
                                   uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  cleanupExpiredLocked(nowMs);
  const auto fingerprint = operationFingerprint(leaseId, providerEpoch);
  bool handled = false;
  auto replay = replayOrConflict("ABORT", idempotencyKey, fingerprint, handled);
  if (handled) return replay;
  auto it = m_leases.find(leaseId);
  if (it == m_leases.end()) return reject("ABORT", "LEASE_NOT_FOUND");
  auto& lease = it->second;
  if (providerEpoch != m_providerEpoch || lease.providerEpoch != providerEpoch) {
    return reject("ABORT", "LEASE_STALE_EPOCH", &lease);
  }
  if (lease.state != ExecutionLeaseState::Prepared &&
      lease.state != ExecutionLeaseState::Committed) {
    return reject("ABORT", "LEASE_INVALID_TRANSITION", &lease);
  }
  lease.state = ExecutionLeaseState::Aborted;
  ++m_counters.aborted;
  auto result = success("ABORT", lease);
  rememberReplay("ABORT", idempotencyKey, fingerprint, result);
  return result;
}

ExecutionLeaseResult
ProviderExecutionLeaseTable::renew(const std::string& leaseId,
                                   const std::string& providerEpoch,
                                   const std::string& idempotencyKey,
                                   uint64_t nowMs, uint64_t expiresAtMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  cleanupExpiredLocked(nowMs);
  const auto fingerprint = operationFingerprint(leaseId, providerEpoch,
                                                expiresAtMs);
  bool handled = false;
  auto replay = replayOrConflict("RENEW", idempotencyKey, fingerprint, handled);
  if (handled) return replay;
  auto it = m_leases.find(leaseId);
  if (it == m_leases.end()) return reject("RENEW", "LEASE_NOT_FOUND");
  auto& lease = it->second;
  if (providerEpoch != m_providerEpoch || lease.providerEpoch != providerEpoch) {
    return reject("RENEW", "LEASE_STALE_EPOCH", &lease);
  }
  if (!isActive(lease.state)) {
    return reject("RENEW", lease.state == ExecutionLeaseState::Expired
                             ? "LEASE_EXPIRED" : "LEASE_INVALID_TRANSITION",
                  &lease);
  }
  if (expiresAtMs <= nowMs ||
      (lease.state == ExecutionLeaseState::Executing &&
       lease.executionDeadlineMs != 0 && expiresAtMs > lease.executionDeadlineMs)) {
    return reject("RENEW", "LEASE_EXPIRED", &lease);
  }
  lease.expiresAtMs = expiresAtMs;
  ++m_counters.renewed;
  auto result = success("RENEW", lease);
  rememberReplay("RENEW", idempotencyKey, fingerprint, result);
  return result;
}

ExecutionLeaseResult
ProviderExecutionLeaseTable::release(const std::string& leaseId,
                                     const std::string& providerEpoch,
                                     const std::string& idempotencyKey,
                                     uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  cleanupExpiredLocked(nowMs);
  const auto fingerprint = operationFingerprint(leaseId, providerEpoch);
  bool handled = false;
  auto replay = replayOrConflict("RELEASE", idempotencyKey, fingerprint, handled);
  if (handled) return replay;
  auto it = m_leases.find(leaseId);
  if (it == m_leases.end()) return reject("RELEASE", "LEASE_NOT_FOUND");
  auto& lease = it->second;
  if (providerEpoch != m_providerEpoch || lease.providerEpoch != providerEpoch) {
    return reject("RELEASE", "LEASE_STALE_EPOCH", &lease);
  }
  if (lease.state != ExecutionLeaseState::Committed &&
      lease.state != ExecutionLeaseState::Executing) {
    return reject("RELEASE", "LEASE_INVALID_TRANSITION", &lease);
  }
  lease.state = ExecutionLeaseState::Released;
  ++m_counters.released;
  auto result = success("RELEASE", lease);
  rememberReplay("RELEASE", idempotencyKey, fingerprint, result);
  return result;
}

size_t
ProviderExecutionLeaseTable::cleanupExpired(uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return cleanupExpiredLocked(nowMs);
}

std::optional<GenericExecutionLease>
ProviderExecutionLeaseTable::find(const std::string& leaseId) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_leases.find(leaseId);
  if (it == m_leases.end()) return std::nullopt;
  return it->second;
}

bool
ProviderExecutionLeaseTable::hasActiveConflictKey(const std::string& conflictKey,
                                                  uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  cleanupExpiredLocked(nowMs);
  for (const auto& item : m_leases) {
    if (isActive(item.second.state) &&
        std::find(item.second.conflictKeys.begin(), item.second.conflictKeys.end(),
                  conflictKey) != item.second.conflictKeys.end()) {
      return true;
    }
  }
  return false;
}

bool
ProviderExecutionLeaseTable::hasPinnedBindingProof(
  const ndn::Buffer& resourceBindingProof, uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  cleanupExpiredLocked(nowMs);
  for (const auto& item : m_leases) {
    if ((item.second.state == ExecutionLeaseState::Committed ||
         item.second.state == ExecutionLeaseState::Executing) &&
        item.second.resourceBindingProof == resourceBindingProof) {
      return true;
    }
  }
  return false;
}

ExecutionLeaseCounters
ProviderExecutionLeaseTable::counters(uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  cleanupExpiredLocked(nowMs);
  auto output = m_counters;
  for (const auto& item : m_leases) {
    switch (item.second.state) {
      case ExecutionLeaseState::Prepared: ++output.activePrepared; break;
      case ExecutionLeaseState::Committed: ++output.activeCommitted; break;
      case ExecutionLeaseState::Executing: ++output.activeExecuting; break;
      default: break;
    }
  }
  return output;
}

} // namespace ndn_service_framework
