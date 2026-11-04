#ifndef NDNSF_UAV_APP_SHARED_UAV_PERSON_TRACE_HPP
#define NDNSF_UAV_APP_SHARED_UAV_PERSON_TRACE_HPP

#include "uav-person-json.hpp"

#include <cerrno>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <system_error>

namespace ndnsf::examples::uav {

/** Read the common Linux host clock used by native and Python experiment actors. */
inline uint64_t
uavPersonMonotonicNs()
{
  timespec value{};
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    throw std::system_error(errno, std::generic_category(), "CLOCK_MONOTONIC");
  }
  return static_cast<uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
}

/** Emit local diagnostics only; this record never admits input or authorizes execution. */
inline void
emitUavPersonTrace(person_json::Json scope, const char* event)
{
  scope["schema"] = "uav-person-runtime-trace-v1";
  scope["clockDomain"] = "CLOCK_MONOTONIC_NS";
  scope["event"] = event;
  try {
    scope["monotonicNs"] = uavPersonMonotonicNs();
  }
  catch (const std::system_error&) {
    std::cerr << "SPEC219_TRACE_DROPPED reason=clock-unavailable" << std::endl;
    return;
  }
  const auto serialized = scope.dump();
  if (serialized.size() > 4096) {
    std::cerr << "SPEC219_TRACE_DROPPED reason=oversize" << std::endl;
    return;
  }
  std::cout << ("SPEC219_TRACE " + serialized + '\n') << std::flush;
}

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_APP_SHARED_UAV_PERSON_TRACE_HPP
