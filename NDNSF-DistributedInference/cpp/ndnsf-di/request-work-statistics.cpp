#include "request-work-statistics.hpp"

#include "NativeCanonicalJson.hpp"
#include "NativeGenerationLimits.hpp"
#include "detail/NativePhaseTiming.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace ndnsf::di {
namespace {

constexpr std::size_t MAX_PHASES = 128;
// Bound total observation work across the native generation budget. Completed
// spans are folded immediately; live storage retains its separate bounds.
constexpr std::size_t MAX_SPANS = (MAX_NATIVE_GENERATED_TOKENS + 1) * MAX_PHASES;
constexpr std::size_t MAX_COUNTERS = 128;
constexpr std::size_t MAX_THREADS = 32;
constexpr std::size_t MAX_DEPTH = 32;
constexpr std::size_t MAX_TRANSFERS = 8192;
constexpr std::size_t MAX_TRANSFER_METADATA_BYTES = 4U << 20;

NativeWorkClock
readClock()
{
  return {std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(),
          detail::nativeThreadCpuTimeNs()};
}

bool
add(std::uint64_t& target, std::uint64_t value)
{
  if (value > std::numeric_limits<std::uint64_t>::max() - target) return false;
  target += value;
  return true;
}

const char*
kindName(NativeWorkKind kind)
{
  switch (kind) {
    case NativeWorkKind::LOCAL: return "local";
    case NativeWorkKind::WAIT: return "wait";
    case NativeWorkKind::BACKEND: return "backend";
    case NativeWorkKind::UNCLASSIFIED: return "unclassified";
  }
  return "unknown";
}

std::optional<std::string>
readClockDomain()
{
#ifdef __linux__
  std::ifstream bootFile("/proc/sys/kernel/random/boot_id");
  std::string boot;
  if (!std::getline(bootFile, boot) || boot.size() != 36 ||
      !std::all_of(boot.begin(), boot.end(), [] (unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || ch == '-';
      })) return std::nullopt;
  std::ifstream offsets("/proc/self/timens_offsets");
  std::string line;
  while (std::getline(offsets, line)) {
    if (line.size() > 128) return std::nullopt;
    std::istringstream fields(line);
    std::string clock, extra;
    std::int64_t seconds = 0, nanoseconds = 0;
    if (!(fields >> clock >> seconds >> nanoseconds) || (fields >> extra))
      return std::nullopt;
    if (clock == "monotonic") {
      if (nanoseconds < 0 || nanoseconds >= 1000000000) return std::nullopt;
      return "linux-monotonic:" + boot + ':' + std::to_string(seconds) + ':' +
        std::to_string(nanoseconds);
    }
  }
#endif
  return std::nullopt;
}

} // namespace

NativeWorkStatistics::NativeWorkStatistics(Clock clock, bool profileEpochs)
  : m_clock(clock ? std::move(clock) : Clock(readClock))
{
  m_statistics.epochProfiling = profileEpochs;
}

NativeWorkStatistics::Token
NativeWorkStatistics::begin(std::string_view phaseName, NativeWorkKind kind,
                            std::optional<std::uint64_t> inferenceEpoch) noexcept
{
  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (phaseName.empty() || phaseName.size() > 96 ||
        (m_statistics.epochProfiling && inferenceEpoch &&
         *inferenceEpoch > MAX_NATIVE_GENERATED_TOKENS)) {
      m_statistics.complete = false;
      ++m_statistics.droppedSpans;
      return 0;
    }
    const std::string phase(phaseName);
    const auto thread = std::this_thread::get_id();
    auto stack = m_stacks.find(thread);
    const auto found = m_statistics.phases.find(phase);
    if (phase.empty() || phase.size() > 96 || m_nextToken > MAX_SPANS ||
        (found == m_statistics.phases.end() && m_statistics.phases.size() >= MAX_PHASES) ||
        (found != m_statistics.phases.end() && found->second.kind != kind) ||
        (stack == m_stacks.end() && m_stacks.size() >= MAX_THREADS) ||
        (stack != m_stacks.end() && stack->second.size() >= MAX_DEPTH)) {
      m_statistics.complete = false;
      ++m_statistics.droppedSpans;
      return 0;
    }
    const auto now = m_clock();
    if (now.wallNs < 0) {
      m_statistics.complete = false;
      ++m_statistics.droppedSpans;
      return 0;
    }
    if (found == m_statistics.phases.end()) {
      NativeWorkPhaseStatistics statistics;
      statistics.kind = kind;
      m_statistics.phases.emplace(phase, std::move(statistics));
    }
    if (stack == m_stacks.end()) stack = m_stacks.emplace(thread, std::vector<Token>{}).first;
    // Reserve before installing the frame so allocation failure cannot leave
    // an uncloseable token absent from its owner's stack.
    stack->second.reserve(stack->second.size() + 1);
    if (!m_statistics.epochProfiling)
      inferenceEpoch.reset();
    else if (!inferenceEpoch && !stack->second.empty())
      inferenceEpoch = m_frames.at(stack->second.back()).inferenceEpoch;
    // Allocate optional summary slots before admitting a live frame. Finish
    // must not strand an active token if profiling storage is exhausted.
    if (m_statistics.epochProfiling && inferenceEpoch)
      m_statistics.epochPhases[*inferenceEpoch].try_emplace(phase).first->second.kind = kind;
    const auto token = m_nextToken++;
    m_frames.emplace(token, Frame{phase, thread, now, 0, 0, inferenceEpoch});
    stack->second.push_back(token);
    return token;
  }
  catch (...) {
    // Allocation/clock failures cannot replace an inference result.
    try {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_statistics.complete = false;
      ++m_statistics.droppedSpans;
    }
    catch (...) {
    }
    return 0;
  }
}

void
NativeWorkStatistics::finish(Token token) noexcept
{
  if (token == 0) return;
  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_frames.find(token);
    if (found == m_frames.end()) {
      m_statistics.complete = false;
      return;
    }
    auto& frame = found->second;
    auto stack = m_stacks.find(frame.thread);
    if (frame.thread != std::this_thread::get_id() || stack == m_stacks.end() ||
        stack->second.empty() || stack->second.back() != token) {
      m_statistics.complete = false;
      return;
    }
    const auto now = m_clock();
    if (now.wallNs < frame.started.wallNs) {
      m_statistics.complete = false;
      stack->second.pop_back();
      m_frames.erase(found);
      return;
    }
    const auto wall = static_cast<std::uint64_t>(now.wallNs - frame.started.wallNs);
    std::optional<std::uint64_t> cpu;
    if (frame.started.threadCpuNs >= 0 && now.threadCpuNs >= frame.started.threadCpuNs)
      cpu = static_cast<std::uint64_t>(now.threadCpuNs - frame.started.threadCpuNs);
    const auto fold = [&] (NativeWorkPhaseStatistics& phase) {
      if (!phase.firstStartedSteadyNs || frame.started.wallNs < *phase.firstStartedSteadyNs)
        phase.firstStartedSteadyNs = frame.started.wallNs;
      if (!phase.lastFinishedSteadyNs || now.wallNs > *phase.lastFinishedSteadyNs)
        phase.lastFinishedSteadyNs = now.wallNs;
      ++phase.calls;
      if (frame.childWallNs > wall || !add(phase.exclusiveWallNs, wall - frame.childWallNs))
        m_statistics.complete = false;
      if (!cpu || !frame.childCpuNs || *frame.childCpuNs > *cpu ||
          !phase.exclusiveThreadCpuNs ||
          !add(*phase.exclusiveThreadCpuNs, *cpu - *frame.childCpuNs))
        phase.exclusiveThreadCpuNs.reset();
    };
    auto& phase = m_statistics.phases.at(frame.phase);
    fold(phase);
    if (m_statistics.epochProfiling && frame.inferenceEpoch) {
      auto& epochPhase = m_statistics.epochPhases.at(*frame.inferenceEpoch).at(frame.phase);
      fold(epochPhase);
    }
    stack->second.pop_back();
    if (!stack->second.empty()) {
      auto& parent = m_frames.at(stack->second.back());
      if (!add(parent.childWallNs, wall)) m_statistics.complete = false;
      if (!cpu || !parent.childCpuNs || !add(*parent.childCpuNs, *cpu))
        parent.childCpuNs.reset();
    }
    m_frames.erase(found);
  }
  catch (...) {
    try {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_statistics.complete = false;
    }
    catch (...) {
    }
  }
}

NativeWorkStatisticsSnapshot
NativeWorkStatistics::snapshot() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto result = m_statistics;
  result.activeSpans = m_frames.size();
  result.complete = result.complete && result.activeSpans == 0;
  return result;
}

void
NativeWorkStatistics::addCount(std::string_view name, std::uint64_t value) noexcept
{
  (void)tryAddCount(name, value);
}

bool
NativeWorkStatistics::tryAddCount(std::string_view name, std::uint64_t value) noexcept
{
  return observeCount(name, value, false);
}

void
NativeWorkStatistics::observeMaximum(std::string_view name, std::uint64_t value) noexcept
{
  observeCount(name, value, true);
}

bool
NativeWorkStatistics::observeCount(std::string_view name, std::uint64_t value, bool maximum) noexcept
{
  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (name.empty() || name.size() > 96) {
      m_statistics.complete = false;
      return false;
    }
    const std::string key(name);
    const auto found = m_statistics.counts.find(key);
    if (key.empty() || key.size() > 96 ||
        (found == m_statistics.counts.end() && m_statistics.counts.size() >= MAX_COUNTERS)) {
      m_statistics.complete = false;
      return false;
    }
    auto& total = m_statistics.counts[key];
    if (maximum) total = std::max(total, value);
    else if (!add(total, value)) {
      m_statistics.complete = false;
      return false;
    }
    return true;
  }
  catch (...) {
    try {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_statistics.complete = false;
    }
    catch (...) {
    }
    return false;
  }
}

NativeWorkScope::NativeWorkScope(std::shared_ptr<NativeWorkStatistics> statistics,
                               std::string_view phase, NativeWorkKind kind,
                               std::optional<std::uint64_t> inferenceEpoch) noexcept
  : m_statistics(std::move(statistics))
  , m_token(m_statistics ? m_statistics->begin(phase, kind, inferenceEpoch) : 0)
{
}

void
NativeWorkStatistics::observeTransfer(const StageTransferObservation& observation,
                                     std::uint64_t epoch, std::int64_t startedNs,
                                     std::int64_t finishedNs, std::int64_t admittedNs,
                                     NativeTransferPoint point,
                                     std::int64_t outputReadyNs) noexcept
{
  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (observation.plannedDataName.empty() || observation.plannedDataName.size() > 4096 ||
        observation.direction.size() > 32 || observation.phase.size() > 64 ||
        (startedNs > 0 && finishedNs > 0 && finishedNs < startedNs) ||
        (admittedNs > 0 && finishedNs > 0 && admittedNs < finishedNs) ||
        (outputReadyNs > 0 && startedNs > 0 && outputReadyNs > startedNs) ||
        m_statistics.transfers.size() >= MAX_TRANSFERS) {
      m_statistics.complete = false;
      return;
    }
    const auto bytes = observation.plannedDataName.size() + observation.direction.size() +
      observation.phase.size();
    if (bytes > MAX_TRANSFER_METADATA_BYTES - m_transferMetadataBytes) {
      m_statistics.complete = false;
      return;
    }
    m_statistics.transfers.push_back({observation.plannedDataName, observation.direction,
      observation.phase, epoch, observation.tensorBytes, observation.encodedPayloadBytes,
      observation.wireBytes, observation.interestCount, observation.retryCount,
      startedNs, finishedNs, admittedNs, point, outputReadyNs,
      observation.transportPayloadBytes, observation.metadataBytes,
      observation.localCopyBytes, observation.transportLocalCopyBytes});
    m_transferMetadataBytes += bytes;
  }
  catch (...) {
    try {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_statistics.complete = false;
    }
    catch (...) {
    }
  }
}

NativeWorkScope::~NativeWorkScope()
{
  finish();
}

void
NativeWorkScope::finish() noexcept
{
  if (m_statistics && m_token != 0) m_statistics->finish(std::exchange(m_token, 0));
}

std::optional<std::string>
nativeWorkClockDomain()
{
  // Immutable process-local metadata; no request or protocol state is global.
  static const auto domain = readClockDomain();
  return domain;
}

std::shared_ptr<NativeWorkStatistics>
makeNativeWorkStatistics() noexcept
{
  try {
    return std::make_shared<NativeWorkStatistics>(
      NativeWorkStatistics::Clock{}, detail::nativePhaseStarted().enabled);
  }
  catch (...) {
    return nullptr;
  }
}

std::string
nativeWorkStatisticsToJson(const NativeWorkStatisticsSnapshot& statistics)
{
  NativeJson result{{"schema", "ndnsf-di-work-statistics-v1"},
    {"complete", statistics.complete}, {"droppedSpans", statistics.droppedSpans},
    {"activeSpans", statistics.activeSpans}, {"coverage", "instrumented-phases"},
    {"localWorkComplete", false}, {"phases", NativeJson::object()},
    {"counts", NativeJson::object()}, {"transfers", NativeJson::array()},
    {"transferCoverage", "completed-worker-and-coordinator-transfers"}};
  for (const auto& transfer : statistics.transfers) {
    const auto point = [&] {
      switch (transfer.point) {
        case NativeTransferPoint::WORKER_INPUT: return "worker-input";
        case NativeTransferPoint::COORDINATOR_FETCH: return "coordinator-fetch";
        case NativeTransferPoint::OUTPUT_PUBLICATION: return "output-publication";
        case NativeTransferPoint::UNSPECIFIED: return "unspecified";
      }
      return "unknown";
    }();
    const auto optionalValue = [] (const auto& value) -> NativeJson {
      return value ? NativeJson(*value) : NativeJson(nullptr);
    };
    const auto clockValue = [] (std::int64_t value) -> NativeJson {
      return value > 0 ? NativeJson(value) : NativeJson(nullptr);
    };
    result["transfers"].push_back(NativeJson{
      {"plannedDataName", transfer.plannedDataName}, {"direction", transfer.direction},
      {"phase", transfer.phase}, {"inferenceEpoch", transfer.inferenceEpoch},
      {"point", point},
      {"tensorBytes", transfer.tensorBytes}, {"encodedPayloadBytes", transfer.encodedPayloadBytes},
      {"wireBytes", optionalValue(transfer.wireBytes)},
      {"transportPayloadBytes", optionalValue(transfer.transportPayloadBytes)},
      {"metadataBytes", optionalValue(transfer.metadataBytes)},
      {"localCopyBytes", optionalValue(transfer.localCopyBytes)},
      {"transportLocalCopyBytes", optionalValue(transfer.transportLocalCopyBytes)},
      {"interestCount", optionalValue(transfer.interestCount)},
      {"retryCount", optionalValue(transfer.retryCount)},
      {"startedSteadyNs", clockValue(transfer.startedSteadyNs)},
      {"finishedSteadyNs", clockValue(transfer.finishedSteadyNs)},
      {"admittedSteadyNs", clockValue(transfer.admittedSteadyNs)},
      {"outputReadySteadyNs", clockValue(transfer.outputReadySteadyNs)}});
  }
  for (const auto& [name, value] : statistics.counts)
    result["counts"][name] = statistics.complete ? NativeJson(value) : NativeJson(nullptr);
  const auto phaseJson = [&] (const NativeWorkPhaseStatistics& phase) {
    return NativeJson{
      {"kind", kindName(phase.kind)}, {"calls", phase.calls},
      {"firstStartedSteadyNs", statistics.complete && phase.firstStartedSteadyNs
        ? NativeJson(*phase.firstStartedSteadyNs) : NativeJson(nullptr)},
      {"lastFinishedSteadyNs", statistics.complete && phase.lastFinishedSteadyNs
        ? NativeJson(*phase.lastFinishedSteadyNs) : NativeJson(nullptr)},
      {"exclusiveWallNs", statistics.complete ? NativeJson(phase.exclusiveWallNs)
                                               : NativeJson(nullptr)},
      {"exclusiveThreadCpuNs", statistics.complete && phase.exclusiveThreadCpuNs
        ? NativeJson(*phase.exclusiveThreadCpuNs) : NativeJson(nullptr)}};
  };
  for (const auto& [name, phase] : statistics.phases)
    result["phases"][name] = phaseJson(phase);
  if (statistics.epochProfiling) {
    result["epochProfiling"] = true;
    result["epochs"] = NativeJson::array();
    for (const auto& [epoch, phases] : statistics.epochPhases) {
      auto encoded = NativeJson::object();
      for (const auto& [name, phase] : phases)
        encoded[name] = phaseJson(phase);
      result["epochs"].push_back({{"inferenceEpoch", epoch}, {"phases", std::move(encoded)}});
    }
  }
  return nativeCanonicalJson(result);
}

} // namespace ndnsf::di
