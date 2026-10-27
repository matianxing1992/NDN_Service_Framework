#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <sstream>
#include <string_view>

namespace ndnsf::di::detail {

struct NativePhaseTiming {
  bool enabled = false;
  std::chrono::steady_clock::time_point wall;
  std::int64_t threadCpuNs = -1;
};

#if defined(CLOCK_THREAD_CPUTIME_ID) || defined(CLOCK_PROCESS_CPUTIME_ID)
inline std::int64_t nativeClockCpuTimeNs(clockid_t clock) noexcept
{
  timespec value{};
  if (clock_gettime(clock, &value) == 0 &&
      value.tv_sec >= 0 && value.tv_nsec >= 0 && value.tv_nsec < 1000000000L &&
      value.tv_sec <= (std::numeric_limits<std::int64_t>::max() - value.tv_nsec) / 1000000000LL)
    return static_cast<std::int64_t>(value.tv_sec) * 1000000000LL + value.tv_nsec;
  return -1;
}
#endif

inline std::int64_t nativeThreadCpuTimeNs() noexcept
{
#ifdef CLOCK_THREAD_CPUTIME_ID
  return nativeClockCpuTimeNs(CLOCK_THREAD_CPUTIME_ID);
#else
  return -1;
#endif
}

inline std::int64_t nativeProcessCpuTimeNs() noexcept
{
#ifdef CLOCK_PROCESS_CPUTIME_ID
  return nativeClockCpuTimeNs(CLOCK_PROCESS_CPUTIME_ID);
#else
  return -1;
#endif
}

inline NativePhaseTiming nativePhaseStarted() noexcept
{
  const auto* enabled = std::getenv("NDNSF_DI_RUNTIME_TIMING");
  if (enabled == nullptr || *enabled == '\0' || *enabled == '0') return {};
  return {true, std::chrono::steady_clock::now(), nativeThreadCpuTimeNs()};
}

// Begin/end must run on the same worker. Authority/Core callback CPU is not
// included in waits; wall minus CPU is not a measurement of network latency.
// RequestId also accepts ndn::Name: its formatting stays inside this try.
template<typename RequestId>
inline void logNativeWorkPhase(const RequestId& requestId, const char* phase,
                               NativePhaseTiming started,
                               std::string_view role = {}, std::uint64_t attempt = 0) noexcept
{
  if (!started.enabled) return;
  try {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started.wall).count();
    const auto cpuEnded = nativeThreadCpuTimeNs();
    std::ostringstream record;
    record << "NDNSF_DI_NATIVE_WORK_PHASE requestId=" << requestId
           << " phase=" << phase << " duration_us=" << elapsed;
    if (!role.empty()) record << " role=" << role;
    if (attempt != 0) record << " attempt=" << attempt;
    if (started.threadCpuNs >= 0 && cpuEnded >= started.threadCpuNs)
      record << " thread_cpu_us=" << (cpuEnded - started.threadCpuNs) / 1000;
    logRuntimeEvidence(record.str());
  }
  catch (...) {
    // Diagnostics never replace the production result or error.
  }
}

} // namespace ndnsf::di::detail
