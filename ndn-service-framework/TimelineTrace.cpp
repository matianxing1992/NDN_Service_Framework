#include "TimelineTrace.hpp"

#include <chrono>
#include <cstdlib>
#include <sstream>

namespace ndn_service_framework {

NDN_LOG_INIT(ndn_service_framework.TimelineTrace);

namespace {

bool
envFlagEnabled(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }
    const std::string text(value);
    return !(text.empty() || text == "0" || text == "false" ||
             text == "FALSE" || text == "no" || text == "NO");
}

uint64_t
wallMicroseconds()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

uint64_t
timelineSteadyMicroseconds()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool
timelineTraceEnvEnabled()
{
    return envFlagEnabled("NDNSF_TIMELINE_TRACE");
}

void
logTimelineTrace(const std::string& role,
                 const std::string& event,
                 const ndn::Name& requestId,
                 TimelineFields fields)
{
    if (!timelineTraceEnvEnabled()) {
        return;
    }

    std::ostringstream os;
    os << "NDNSF_TIMELINE"
       << " role=" << role
       << " event=" << event
       << " steady_us=" << timelineSteadyMicroseconds()
       << " timestamp_us=" << wallMicroseconds()
       << " requestId=" << requestId.toUri();
    for (const auto& field : fields) {
        os << " " << field.first << "=" << field.second;
    }
    NDN_LOG_DEBUG(os.str());
}

} // namespace ndn_service_framework
