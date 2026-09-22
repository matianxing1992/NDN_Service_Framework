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

std::string
timelineField(TimelineFields fields, const char* name, std::string fallback)
{
    for (const auto& field : fields) {
        if (field.first == name && !field.second.empty()) {
            return field.second;
        }
    }
    return fallback;
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

void
logPhaseTiming(const std::string& role,
               const std::string& phase,
               const ndn::Name& requestId,
               TimelineFields fields)
{
    if (requestId.empty() || phase.empty()) return;
    if (!envFlagEnabled("NDNSF_PHASE_TIMING")) return;
    const auto attempt = timelineField(
        fields, "attempt", timelineField(fields, "attemptEpoch", "request"));
    const auto canonicalAttempt = attempt.empty() || attempt == "0" ?
        std::string("request") : attempt;
    std::ostringstream os;
    os << "NDNSF_PHASE_TIMING"
       << " component=" << role
       << " executionRole=" << timelineField(fields, "executionRole", role)
       << " phase=" << phase
       << " steady_us=" << timelineSteadyMicroseconds()
       << " timestamp_us=" << wallMicroseconds()
       << " requestId=" << requestId.toUri()
       << " attempt=" << canonicalAttempt
       << " providerBootId=" << timelineField(fields, "providerBootId", "none")
       << " sessionId=" << timelineField(fields, "sessionId", "none")
       << " conversationId=" << timelineField(fields, "conversationId", "none")
       << " inferenceEpoch=" << timelineField(fields, "inferenceEpoch", "none")
       << " contextEpoch=" << timelineField(fields, "contextEpoch", "none");
    for (const auto& field : fields) {
        if (field.first.empty() || field.first == "role" ||
            field.first == "executionRole" || field.first == "attempt" ||
            field.first == "attemptEpoch" || field.first == "providerBootId" ||
            field.first == "sessionId" || field.first == "conversationId" ||
            field.first == "inferenceEpoch" || field.first == "contextEpoch" ||
            field.second.find_first_of("\r\n \t") != std::string::npos)
            continue;
        os << " " << field.first << "=" << field.second;
    }
    NDN_LOG_WARN(os.str());
}

bool
timelineTraceEnvEnabled()
{
    return envFlagEnabled("NDNSF_TIMELINE_TRACE");
}

bool
phaseTimingEnvEnabled()
{
    return envFlagEnabled("NDNSF_PHASE_TIMING");
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
    const auto emitPhase = [&] {
        const char* phase = nullptr;
        if (event == "request_created") phase = "submit";
        else if (event == "request_publish_done") phase = "requestPublished";
        else if (event == "first_ack_observed") phase = "ackReceived";
        if (phase != nullptr) logPhaseTiming(role, phase, requestId, fields);
    };
    emitPhase();
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
logStreamTimelineTrace(const std::string& role,
                       const std::string& event,
                       const std::string& streamId,
                       uint64_t sessionEpoch,
                       uint64_t cursor,
                       TimelineFields fields)
{
    // Per-packet Stream traces are substantially more frequent than request or
    // exact-frame traces.  A 12+1, 30-fps UAV stream emits hundreds of these
    // records per second and DEBUG logging can become the bottleneck being
    // measured.  Keep the diagnostic available, but allow latency campaigns
    // to retain exact source/decode/widget evidence without enabling this
    // high-rate probe.
    if (const char* value = std::getenv("NDNSF_STREAM_PACKET_TIMELINE_TRACE");
        value != nullptr) {
        const std::string text(value);
        if (text.empty() || text == "0" || text == "false" || text == "FALSE" ||
            text == "no" || text == "NO") {
            return;
        }
    }
    ndn::Name correlation("/NDNSF/STREAM/TIMELINE");
    correlation.append(streamId);
    correlation.appendNumber(sessionEpoch);
    correlation.appendNumber(cursor);
    logTimelineTrace(role, event, correlation, fields);
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
