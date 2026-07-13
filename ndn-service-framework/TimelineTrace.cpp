#include "TimelineTrace.hpp"

#include <chrono>
#include <cstdlib>
#include <functional>
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

size_t
envSizeValue(const char* name, size_t defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return defaultValue;
    }
    try {
        return std::max<size_t>(1, static_cast<size_t>(std::stoull(value)));
    }
    catch (...) {
        return defaultValue;
    }
}

bool
timelineTraceSampleAllows(const ndn::Name& requestId)
{
    const size_t sampleRate = envSizeValue("NDNSF_TIMELINE_TRACE_SAMPLE_RATE", 100);
    if (sampleRate <= 1 || requestId.empty()) {
        return true;
    }
    // FNV-1a is deliberately shared with Spec 107 Python diagnostics. Unlike
    // std::hash, it is stable across processes, binaries, and language runtimes.
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : requestId.toUri()) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return (hash % sampleRate) == 0;
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

bool
hybridCryptoTimingEnvEnabled()
{
    return envFlagEnabled("NDNSF_HYBRID_CRYPTO_TIMING");
}

bool
controlTimingEnvEnabled()
{
    return envFlagEnabled("NDNSF_CONTROL_TIMING");
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
    if (!timelineTraceSampleAllows(requestId)) {
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

void
logHybridCryptoTiming(const std::string& role,
                      const std::string& event,
                      const ndn::Name& requestId,
                      TimelineFields fields)
{
    if (!hybridCryptoTimingEnvEnabled()) {
        return;
    }
    if (!timelineTraceSampleAllows(requestId)) {
        return;
    }

    std::ostringstream os;
    os << "NDNSF_CRYPTO_TIMING"
       << " role=" << role
       << " event=" << event
       << " steady_us=" << timelineSteadyMicroseconds()
       << " timestamp_us=" << wallMicroseconds()
       << " requestId=" << requestId.toUri();
    for (const auto& field : fields) {
        os << " " << field.first << "=" << field.second;
    }
    NDN_LOG_WARN(os.str());
}

void
logControlTiming(const std::string& role,
                 const std::string& event,
                 const ndn::Name& requestId,
                 TimelineFields fields)
{
    if (!controlTimingEnvEnabled()) {
        return;
    }
    if (!timelineTraceSampleAllows(requestId)) {
        return;
    }

    std::ostringstream os;
    os << "NDNSF_CONTROL_TIMING"
       << " role=" << role
       << " event=" << event
       << " steady_us=" << timelineSteadyMicroseconds()
       << " timestamp_us=" << wallMicroseconds()
       << " requestId=" << requestId.toUri();
    for (const auto& field : fields) {
        os << " " << field.first << "=" << field.second;
    }
    NDN_LOG_WARN(os.str());
}

} // namespace ndn_service_framework
