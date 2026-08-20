#ifndef NDNSF_DISTRIBUTED_INFERENCE_COLLECTIVE_RUNTIME_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_COLLECTIVE_RUNTIME_HPP

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace ndnsf::di {

/**
 * A small, transport-independent state machine for one authenticated tensor
 * group.  It deliberately owns no model, device, or socket.  The Provider
 * adapter supplies local readiness and progress events; the state machine
 * decides whether the group may start and whether the whole group must stop.
 */
enum class CollectiveRuntimeState
{
  Pending,
  Running,
  Completed,
  Failed,
  Cancelled,
  Stalled,
  HardDeadline,
};

const char*
toString(CollectiveRuntimeState state) noexcept;

std::ostream&
operator<<(std::ostream& os, CollectiveRuntimeState state);

struct CollectiveRuntimeOptions
{
  std::uint64_t schedulerTickMs = 5;
  std::uint64_t noProgressMs = 1000;
  std::uint64_t hardDeadlineMs = 5000;
};

struct CollectiveRankSnapshot
{
  std::string rank;
  std::uint64_t epoch = 0;
  bool authenticated = false;
  bool localReady = false;
  bool inputReady = false;
  bool completed = false;
  std::uint64_t lastInputSequence = 0;
  std::uint64_t lastProgressSequence = 0;
};

struct CollectiveRuntimeSnapshot
{
  CollectiveRuntimeState state = CollectiveRuntimeState::Pending;
  std::uint64_t groupEpoch = 0;
  std::uint64_t schedulerTickMs = 0;
  std::uint64_t noProgressMs = 0;
  std::uint64_t hardDeadlineMs = 0;
  std::size_t authenticatedRanks = 0;
  std::size_t localReadyRanks = 0;
  std::size_t inputReadyRanks = 0;
  std::size_t completedRanks = 0;
  std::optional<std::uint64_t> eligibleAtMs;
  std::optional<std::uint64_t> startedAtMs;
  std::optional<std::uint64_t> lastProgressAtMs;
  std::optional<std::uint64_t> terminalAtMs;
  std::string terminalReason;
  std::string lastError;
  // This is a contract marker: group startup never consults unrelated model
  // readiness or a process-global barrier.
  bool usedGlobalModelReadyBarrier = false;
  std::map<std::string, CollectiveRankSnapshot> ranks;
};

class CollectiveRuntime
{
public:
  CollectiveRuntime(std::vector<std::string> ranks,
                    std::uint64_t groupEpoch,
                    std::string capabilityDigest,
                    CollectiveRuntimeOptions options = {});

  /** Return the immutable epoch bound to this collective group. */
  std::uint64_t
  groupEpoch() const noexcept;

  /** Authenticate a rank for this exact group epoch and capability. */
  bool
  authenticateRank(const std::string& rank,
                   std::uint64_t epoch,
                   const std::string& capabilityDigest);

  /** Mark the rank's local runtime ready.  Authentication is mandatory. */
  bool
  markLocalReady(const std::string& rank,
                 std::uint64_t epoch,
                 std::uint64_t nowMs);

  /**
   * Mark one authenticated, local-ready rank's direct input ready.  The last
   * rank to become input-ready records group eligibility; no global model
   * ready flag is consulted.
   */
  bool
  markInputReady(const std::string& rank,
                 std::uint64_t epoch,
                 std::uint64_t sequence,
                 std::uint64_t nowMs);

  /** Start on the same or next scheduler wake once every group rank is ready. */
  bool
  start(std::uint64_t nowMs);

  /** Admit strictly advancing authenticated progress from one rank. */
  bool
  recordProgress(const std::string& rank,
                 std::uint64_t epoch,
                 std::uint64_t sequence,
                 std::uint64_t nowMs);

  /** Mark one rank complete; completion of one rank never releases the others. */
  bool
  completeRank(const std::string& rank,
               std::uint64_t epoch,
               std::uint64_t sequence,
               std::uint64_t nowMs);

  /** Poll idle and hard deadlines.  The first terminal state is retained. */
  CollectiveRuntimeState
  poll(std::uint64_t nowMs);

  /** Whole-group terminal transitions; later events are rejected. */
  bool
  cancel(std::string reason, std::uint64_t nowMs);

  bool
  fail(std::string reason, std::uint64_t nowMs);

  CollectiveRuntimeState
  state() const noexcept;

  bool
  terminal() const noexcept;

  const std::string&
  terminalReason() const noexcept;

  const std::string&
  lastError() const noexcept;

  CollectiveRuntimeSnapshot
  snapshot() const;

private:
  struct RankState
  {
    std::uint64_t epoch = 0;
    bool authenticated = false;
    bool localReady = false;
    bool inputReady = false;
    bool completed = false;
    std::uint64_t lastInputSequence = 0;
    std::uint64_t lastProgressSequence = 0;
  };

  RankState*
  findRank(const std::string& rank);

  const RankState*
  findRank(const std::string& rank) const;

  bool
  validateRankEvent(const std::string& rank,
                    std::uint64_t epoch,
                    const char* event);

  bool
  allInputReady() const noexcept;

  bool
  allCompleted() const noexcept;

  bool
  transitionTerminal(CollectiveRuntimeState state,
                     std::string reason,
                     std::uint64_t nowMs);

private:
  std::map<std::string, RankState> m_ranks;
  std::uint64_t m_groupEpoch = 0;
  std::string m_capabilityDigest;
  CollectiveRuntimeOptions m_options;
  CollectiveRuntimeState m_state = CollectiveRuntimeState::Pending;
  std::optional<std::uint64_t> m_eligibleAtMs;
  std::optional<std::uint64_t> m_startedAtMs;
  std::optional<std::uint64_t> m_lastProgressAtMs;
  std::optional<std::uint64_t> m_terminalAtMs;
  std::string m_terminalReason;
  std::string m_lastError;
  mutable std::recursive_mutex m_mutex;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_COLLECTIVE_RUNTIME_HPP
