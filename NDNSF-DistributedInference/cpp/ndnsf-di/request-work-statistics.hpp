#ifndef NDNSF_DI_REQUEST_WORK_STATISTICS_HPP
#define NDNSF_DI_REQUEST_WORK_STATISTICS_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ndnsf::di {

struct StageTransferObservation;

enum class NativeWorkKind { LOCAL, WAIT, BACKEND, UNCLASSIFIED };

enum class NativeTransferPoint {
  UNSPECIFIED, WORKER_INPUT, COORDINATOR_FETCH, OUTPUT_PUBLICATION
};

struct NativeWorkClock
{
  std::int64_t wallNs = -1;
  std::int64_t threadCpuNs = -1;
};

struct NativeWorkPhaseStatistics
{
  NativeWorkKind kind = NativeWorkKind::UNCLASSIFIED;
  std::uint64_t calls = 0;
  std::uint64_t exclusiveWallNs = 0;
  std::optional<std::uint64_t> exclusiveThreadCpuNs = 0;
  // Envelope endpoints across observed calls, not an additive phase duration.
  std::optional<std::int64_t> firstStartedSteadyNs;
  std::optional<std::int64_t> lastFinishedSteadyNs;
};

struct NativeWorkStatisticsSnapshot
{
  bool complete = true;
  std::size_t droppedSpans = 0;
  std::size_t activeSpans = 0;
  std::map<std::string, NativeWorkPhaseStatistics> phases;
  bool epochProfiling = false;
  std::map<std::uint64_t, std::map<std::string, NativeWorkPhaseStatistics>> epochPhases;
  std::map<std::string, std::uint64_t> counts;
  struct Transfer
  {
    std::string plannedDataName;
    std::string direction;
    std::string phase;
    std::uint64_t inferenceEpoch = 0;
    std::uint64_t tensorBytes = 0;
    std::uint64_t encodedPayloadBytes = 0;
    std::optional<std::size_t> wireBytes;
    std::optional<std::size_t> interestCount;
    std::optional<std::size_t> retryCount;
    std::int64_t startedSteadyNs = 0;
    std::int64_t finishedSteadyNs = 0;
    std::int64_t admittedSteadyNs = 0;
    NativeTransferPoint point = NativeTransferPoint::UNSPECIFIED;
    std::int64_t outputReadySteadyNs = 0;
    std::optional<std::size_t> transportPayloadBytes;
    std::optional<std::size_t> metadataBytes;
    std::optional<std::size_t> localCopyBytes;
    std::optional<std::size_t> transportLocalCopyBytes;
  };
  std::vector<Transfer> transfers;
};

/**
 * Request-owned, bounded service-work accounting. Completed spans are folded
 * immediately; no token trace or process-global request registry is retained.
 * Same-thread children are subtracted once from their parent. Different
 * threads report service work, not an additive request-wall partition.
 */
class NativeWorkStatistics
{
public:
  using Clock = std::function<NativeWorkClock()>;
  using Token = std::uint64_t;

  explicit NativeWorkStatistics(Clock clock = {}, bool profileEpochs = false);

  /** Zero means unavailable; statistics never change the execution outcome. */
  Token
  begin(std::string_view phase, NativeWorkKind kind,
        std::optional<std::uint64_t> inferenceEpoch = std::nullopt) noexcept;

  bool
  epochProfilingEnabled() const noexcept { return m_statistics.epochProfiling; }

  /** Must close the innermost span on the thread that opened it. */
  void
  finish(Token token) noexcept;

  NativeWorkStatisticsSnapshot
  snapshot() const;

  /** Add a bounded caller-defined count; overflow is incomplete, never saturated. */
  void
  addCount(std::string_view name, std::uint64_t value) noexcept;

  /** Report whether this scalar was recorded, for complete inventory transfers. */
  bool
  tryAddCount(std::string_view name, std::uint64_t value) noexcept;

  /** Observe a high-water value in the same bounded namespace as counters. */
  void
  observeMaximum(std::string_view name, std::uint64_t value) noexcept;

  /** Copy bounded scalar transport observations, never tensor contents. */
  void
  observeTransfer(const StageTransferObservation& observation, std::uint64_t epoch,
                  std::int64_t startedNs, std::int64_t finishedNs,
                  std::int64_t admittedNs = 0,
                  NativeTransferPoint point = NativeTransferPoint::UNSPECIFIED,
                  std::int64_t outputReadyNs = 0) noexcept;

private:
  bool
  observeCount(std::string_view name, std::uint64_t value, bool maximum) noexcept;

  struct Frame
  {
    std::string phase;
    std::thread::id thread;
    NativeWorkClock started;
    std::uint64_t childWallNs = 0;
    std::optional<std::uint64_t> childCpuNs = 0;
    std::optional<std::uint64_t> inferenceEpoch;
  };

  Clock m_clock;
  mutable std::mutex m_mutex;
  NativeWorkStatisticsSnapshot m_statistics;
  std::map<Token, Frame> m_frames;
  std::map<std::thread::id, std::vector<Token>> m_stacks;
  Token m_nextToken = 1;
  std::size_t m_transferMetadataBytes = 0;
};

/** A request-local observation lease; closing it never throws into the caller. */
class NativeWorkScope
{
public:
  NativeWorkScope(std::shared_ptr<NativeWorkStatistics> statistics,
                  std::string_view phase, NativeWorkKind kind,
                  std::optional<std::uint64_t> inferenceEpoch = std::nullopt) noexcept;
  ~NativeWorkScope();

  void
  finish() noexcept;

  NativeWorkScope(const NativeWorkScope&) = delete;
  NativeWorkScope& operator=(const NativeWorkScope&) = delete;
  NativeWorkScope(NativeWorkScope&&) = delete;
  NativeWorkScope& operator=(NativeWorkScope&&) = delete;

private:
  std::shared_ptr<NativeWorkStatistics> m_statistics;
  NativeWorkStatistics::Token m_token = 0;
};

/** Allocate an optional observation owner without affecting execution on failure. */
std::shared_ptr<NativeWorkStatistics>
makeNativeWorkStatistics() noexcept;

/** Return a verified monotonic clock-domain identity, otherwise nullopt. */
std::optional<std::string>
nativeWorkClockDomain();

/** Render aggregate metadata, preserving incomplete/missing observations. */
std::string
nativeWorkStatisticsToJson(const NativeWorkStatisticsSnapshot& statistics);

} // namespace ndnsf::di

#endif // NDNSF_DI_REQUEST_WORK_STATISTICS_HPP
