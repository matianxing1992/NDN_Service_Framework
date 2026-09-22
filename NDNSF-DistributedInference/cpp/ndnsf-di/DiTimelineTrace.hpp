#ifndef NDNSF_DISTRIBUTED_INFERENCE_DI_TIMELINE_TRACE_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_DI_TIMELINE_TRACE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <sstream>
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
  const auto emitPhase = [&] {
    const char* phase = nullptr;
    // These events surround the named production boundaries.  Validation is
    // not session construction, and role_compute includes dependency/state
    // handling around the runner; the adapter emits the narrower ORT Run
    // boundary separately.
    if (event == "role_validation_start") phase = "roleValidationBegin";
    else if (event == "role_validation_done") phase = "roleValidationEnd";
    else if (event == "role_preparation_start") phase = "runnerPreparationBegin";
    else if (event == "role_preparation_done") phase = "runnerPreparationEnd";
    else if (event == "role_compute_start") phase = "roleExecutionBegin";
    else if (event == "role_compute_done") phase = "roleExecutionEnd";
    else if (event == "dependency_fetch_start") phase = "stageInputFetchBegin";
    else if (event == "dependency_fetch_done" ||
             event == "dependency_fetch_pre_satisfied") phase = "stageInputFetchEnd";
    else if (event == "dependency_publish_start") phase = "stageOutputPublishBegin";
    else if (event == "dependency_publish_done") phase = "stageOutputPublishEnd";
    if (phase == nullptr) return;
    std::string attempt;
    std::vector<std::pair<std::string, std::string>> phaseFields;
    for (const auto& field : fields) {
      if (field.first == "role") {
        phaseFields.emplace_back("executionRole", field.second);
      }
      else {
        phaseFields.push_back(field);
      }
      if (field.first == "attemptEpoch" || field.first == "attempt") attempt = field.second;
    }
    logRuntimePhase(role, phase, requestId, attempt, phaseFields);
  };
  emitPhase();
  if (!diTimelineEnvEnabled() || !diTimelineSampleAllows(requestId)) return;
  std::ostringstream record;
  record << "NDNSF_TIMELINE"
         << " role=" << role
         << " event=" << event
         << " steady_us=" << diTimelineSteadyMicroseconds()
         << " timestamp_us=" << diTimelineWallMicroseconds()
         << " requestId=" << requestId;
  for (const auto& field : fields) {
    record << " " << field.first << "=" << field.second;
  }
  logRuntimeEvidence(record.str());
}

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_DI_TIMELINE_TRACE_HPP
