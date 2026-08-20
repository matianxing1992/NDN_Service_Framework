#include "NDNSF-DistributedInference/cpp/ndnsf-di/CollectiveRuntime.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {

const char*
toString(CollectiveRuntimeState state) noexcept
{
  switch (state) {
  case CollectiveRuntimeState::Pending: return "PENDING";
  case CollectiveRuntimeState::Running: return "RUNNING";
  case CollectiveRuntimeState::Completed: return "COMPLETED";
  case CollectiveRuntimeState::Failed: return "FAILED";
  case CollectiveRuntimeState::Cancelled: return "CANCELLED";
  case CollectiveRuntimeState::Stalled: return "STALLED";
  case CollectiveRuntimeState::HardDeadline: return "HARD_DEADLINE";
  }
  return "PENDING";
}

std::ostream&
operator<<(std::ostream& os, CollectiveRuntimeState state)
{
  return os << toString(state);
}

CollectiveRuntime::CollectiveRuntime(std::vector<std::string> ranks,
                                     std::uint64_t groupEpoch,
                                     std::string capabilityDigest,
                                     CollectiveRuntimeOptions options)
  : m_groupEpoch(groupEpoch)
  , m_capabilityDigest(std::move(capabilityDigest))
  , m_options(options)
{
  if (groupEpoch == 0 || m_capabilityDigest.empty()) {
    throw std::invalid_argument(
      "CollectiveRuntime requires a non-zero epoch and capability digest");
  }
  if (ranks.empty() || options.schedulerTickMs == 0 ||
      options.noProgressMs == 0 || options.hardDeadlineMs < options.noProgressMs) {
    throw std::invalid_argument("invalid CollectiveRuntime bounds");
  }
  for (auto& rank : ranks) {
    if (rank.empty() || !m_ranks.emplace(std::move(rank), RankState{}).second) {
      throw std::invalid_argument("CollectiveRuntime requires unique non-empty ranks");
    }
  }
}

CollectiveRuntime::RankState*
CollectiveRuntime::findRank(const std::string& rank)
{
  const auto found = m_ranks.find(rank);
  return found == m_ranks.end() ? nullptr : &found->second;
}

const CollectiveRuntime::RankState*
CollectiveRuntime::findRank(const std::string& rank) const
{
  const auto found = m_ranks.find(rank);
  return found == m_ranks.end() ? nullptr : &found->second;
}

bool
CollectiveRuntime::validateRankEvent(const std::string& rank,
                                      std::uint64_t epoch,
                                      const char* event)
{
  if (terminal()) {
    m_lastError = std::string(event) + ":terminal";
    return false;
  }
  auto* state = findRank(rank);
  if (state == nullptr) {
    m_lastError = std::string(event) + ":unknown-rank";
    return false;
  }
  if (epoch != m_groupEpoch) {
    m_lastError = std::string(event) + ":wrong-epoch";
    return false;
  }
  return true;
}

bool
CollectiveRuntime::authenticateRank(const std::string& rank,
                                     std::uint64_t epoch,
                                     const std::string& capabilityDigest)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!validateRankEvent(rank, epoch, "authenticate")) {
    return false;
  }
  auto* state = findRank(rank);
  if (capabilityDigest != m_capabilityDigest || capabilityDigest.empty()) {
    m_lastError = "authenticate:capability-mismatch";
    return false;
  }
  if (state->authenticated) {
    m_lastError = "authenticate:duplicate";
    return false;
  }
  state->epoch = epoch;
  state->authenticated = true;
  m_lastError.clear();
  return true;
}

bool
CollectiveRuntime::markLocalReady(const std::string& rank,
                                   std::uint64_t epoch,
                                   std::uint64_t nowMs)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!validateRankEvent(rank, epoch, "local-ready")) {
    return false;
  }
  auto* state = findRank(rank);
  if (!state->authenticated) {
    m_lastError = "local-ready:unauthenticated";
    return false;
  }
  if (state->localReady) {
    m_lastError = "local-ready:duplicate";
    return false;
  }
  state->localReady = true;
  m_lastError.clear();
  (void)nowMs;
  return true;
}

bool
CollectiveRuntime::markInputReady(const std::string& rank,
                                   std::uint64_t epoch,
                                   std::uint64_t sequence,
                                   std::uint64_t nowMs)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!validateRankEvent(rank, epoch, "input-ready")) {
    return false;
  }
  auto* state = findRank(rank);
  if (!state->authenticated || !state->localReady) {
    m_lastError = "input-ready:local-not-ready";
    return false;
  }
  if (sequence == 0 || state->inputReady || sequence <= state->lastInputSequence) {
    m_lastError = "input-ready:non-advancing";
    return false;
  }
  state->inputReady = true;
  state->lastInputSequence = sequence;
  if (allInputReady() && !m_eligibleAtMs.has_value()) {
    m_eligibleAtMs = nowMs;
  }
  m_lastError.clear();
  return true;
}

bool
CollectiveRuntime::allInputReady() const noexcept
{
  return std::all_of(m_ranks.begin(), m_ranks.end(), [] (const auto& entry) {
    return entry.second.authenticated && entry.second.localReady &&
           entry.second.inputReady;
  });
}

bool
CollectiveRuntime::allCompleted() const noexcept
{
  return std::all_of(m_ranks.begin(), m_ranks.end(), [] (const auto& entry) {
    return entry.second.completed;
  });
}

bool
CollectiveRuntime::start(std::uint64_t nowMs)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (terminal()) {
    m_lastError = "start:terminal";
    return false;
  }
  if (m_state == CollectiveRuntimeState::Running) {
    m_lastError = "start:duplicate";
    return false;
  }
  if (!allInputReady()) {
    m_lastError = "start:group-not-ready";
    return false;
  }
  if (!m_eligibleAtMs.has_value()) {
    m_eligibleAtMs = nowMs;
  }
  m_startedAtMs = nowMs;
  m_lastProgressAtMs = nowMs;
  m_state = CollectiveRuntimeState::Running;
  m_lastError.clear();
  return true;
}

bool
CollectiveRuntime::recordProgress(const std::string& rank,
                                   std::uint64_t epoch,
                                   std::uint64_t sequence,
                                   std::uint64_t nowMs)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!validateRankEvent(rank, epoch, "progress")) {
    return false;
  }
  if (m_state != CollectiveRuntimeState::Running) {
    m_lastError = "progress:not-running";
    return false;
  }
  if (poll(nowMs) != CollectiveRuntimeState::Running) {
    m_lastError = "progress:deadline";
    return false;
  }
  auto* state = findRank(rank);
  if (!state->authenticated || !state->inputReady || sequence == 0 ||
      sequence <= state->lastProgressSequence) {
    m_lastError = "progress:non-advancing-or-unready";
    return false;
  }
  state->lastProgressSequence = sequence;
  m_lastProgressAtMs = nowMs;
  m_lastError.clear();
  return true;
}

bool
CollectiveRuntime::completeRank(const std::string& rank,
                                 std::uint64_t epoch,
                                 std::uint64_t sequence,
                                 std::uint64_t nowMs)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!recordProgress(rank, epoch, sequence, nowMs)) {
    return false;
  }
  auto* state = findRank(rank);
  state->completed = true;
  if (allCompleted()) {
    m_state = CollectiveRuntimeState::Completed;
    m_terminalAtMs = nowMs;
    m_terminalReason = "NDNSF_COLLECTIVE_COMPLETED";
  }
  return true;
}

CollectiveRuntimeState
CollectiveRuntime::poll(std::uint64_t nowMs)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (terminal() || m_state != CollectiveRuntimeState::Running) {
    return m_state;
  }
  const auto started = m_startedAtMs.value_or(nowMs);
  if (nowMs >= started + m_options.hardDeadlineMs) {
    transitionTerminal(CollectiveRuntimeState::HardDeadline,
                       "NDNSF_COLLECTIVE_HARD_DEADLINE", nowMs);
  }
  else if (m_lastProgressAtMs.has_value() &&
           nowMs >= *m_lastProgressAtMs + m_options.noProgressMs) {
    transitionTerminal(CollectiveRuntimeState::Stalled,
                       "NDNSF_COLLECTIVE_NO_PROGRESS", nowMs);
  }
  return m_state;
}

bool
CollectiveRuntime::transitionTerminal(CollectiveRuntimeState state,
                                       std::string reason,
                                       std::uint64_t nowMs)
{
  if (terminal()) {
    return false;
  }
  m_state = state;
  m_terminalReason = std::move(reason);
  m_terminalAtMs = nowMs;
  return true;
}

bool
CollectiveRuntime::cancel(std::string reason, std::uint64_t nowMs)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (reason.empty()) {
    reason = "NDNSF_COLLECTIVE_CANCELLED";
  }
  return transitionTerminal(CollectiveRuntimeState::Cancelled,
                            std::move(reason), nowMs);
}

bool
CollectiveRuntime::fail(std::string reason, std::uint64_t nowMs)
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (reason.empty()) {
    reason = "NDNSF_COLLECTIVE_FAILED";
  }
  return transitionTerminal(CollectiveRuntimeState::Failed,
                            std::move(reason), nowMs);
}

CollectiveRuntimeState
CollectiveRuntime::state() const noexcept
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  return m_state;
}

bool
CollectiveRuntime::terminal() const noexcept
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  return m_state == CollectiveRuntimeState::Completed ||
         m_state == CollectiveRuntimeState::Failed ||
         m_state == CollectiveRuntimeState::Cancelled ||
         m_state == CollectiveRuntimeState::Stalled ||
         m_state == CollectiveRuntimeState::HardDeadline;
}

const std::string&
CollectiveRuntime::terminalReason() const noexcept
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  return m_terminalReason;
}

const std::string&
CollectiveRuntime::lastError() const noexcept
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  return m_lastError;
}

CollectiveRuntimeSnapshot
CollectiveRuntime::snapshot() const
{
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  CollectiveRuntimeSnapshot result;
  result.state = m_state;
  result.groupEpoch = m_groupEpoch;
  result.schedulerTickMs = m_options.schedulerTickMs;
  result.noProgressMs = m_options.noProgressMs;
  result.hardDeadlineMs = m_options.hardDeadlineMs;
  result.eligibleAtMs = m_eligibleAtMs;
  result.startedAtMs = m_startedAtMs;
  result.lastProgressAtMs = m_lastProgressAtMs;
  result.terminalAtMs = m_terminalAtMs;
  result.terminalReason = m_terminalReason;
  result.lastError = m_lastError;
  result.usedGlobalModelReadyBarrier = false;
  for (const auto& [rank, state] : m_ranks) {
    CollectiveRankSnapshot snapshot;
    snapshot.rank = rank;
    snapshot.epoch = state.epoch;
    snapshot.authenticated = state.authenticated;
    snapshot.localReady = state.localReady;
    snapshot.inputReady = state.inputReady;
    snapshot.completed = state.completed;
    snapshot.lastInputSequence = state.lastInputSequence;
    snapshot.lastProgressSequence = state.lastProgressSequence;
    result.ranks.emplace(rank, snapshot);
    result.authenticatedRanks += state.authenticated ? 1 : 0;
    result.localReadyRanks += state.localReady ? 1 : 0;
    result.inputReadyRanks += state.inputReady ? 1 : 0;
    result.completedRanks += state.completed ? 1 : 0;
  }
  return result;
}

std::uint64_t
CollectiveRuntime::groupEpoch() const noexcept
{
  return m_groupEpoch;
}

} // namespace ndnsf::di
