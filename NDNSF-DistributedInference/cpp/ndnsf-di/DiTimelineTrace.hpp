#ifndef NDNSF_DISTRIBUTED_INFERENCE_DI_TIMELINE_TRACE_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_DI_TIMELINE_TRACE_HPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>

namespace ndnsf::di {

using DiTimelineFields =
  std::initializer_list<std::pair<std::string, std::string>>;

inline bool
diTimelineEnvEnabled()
{
  const char* value = std::getenv("NDNSF_TIMELINE_TRACE");
  if (value == nullptr) return false;
  const std::string text(value);
  return !(text.empty() || text == "0" || text == "false" ||
           text == "FALSE" || text == "no" || text == "NO");
}

inline std::size_t
diTimelineSampleRate()
{
  const char* value = std::getenv("NDNSF_TIMELINE_TRACE_SAMPLE_RATE");
  if (value == nullptr || *value == '\0') return 100;
  try {
    return std::max<std::size_t>(1, static_cast<std::size_t>(std::stoull(value)));
  }
  catch (...) {
    return 100;
  }
}

inline bool
diTimelineSampleAllows(const std::string& requestId)
{
  const auto rate = diTimelineSampleRate();
  if (rate <= 1 || requestId.empty()) return true;
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char ch : requestId) {
    hash ^= ch;
    hash *= 1099511628211ULL;
  }
  return hash % rate == 0;
}

inline std::uint64_t
diTimelineSteadyMicroseconds()
{
  return std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline std::uint64_t
diTimelineWallMicroseconds()
{
  return std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

inline void
logDiTimelineTrace(const std::string& role,
                   const std::string& event,
                   const std::string& requestId,
                   DiTimelineFields fields = {})
{
  if (!diTimelineEnvEnabled() || !diTimelineSampleAllows(requestId)) return;
  static std::mutex outputMutex;
  std::lock_guard<std::mutex> lock(outputMutex);
  std::cout << "NDNSF_TIMELINE"
            << " role=" << role
            << " event=" << event
            << " steady_us=" << diTimelineSteadyMicroseconds()
            << " timestamp_us=" << diTimelineWallMicroseconds()
            << " requestId=" << requestId;
  for (const auto& field : fields) {
    std::cout << " " << field.first << "=" << field.second;
  }
  std::cout << std::endl;
}

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_DI_TIMELINE_TRACE_HPP
