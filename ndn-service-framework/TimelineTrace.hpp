#ifndef NDN_SERVICE_FRAMEWORK_TIMELINE_TRACE_HPP
#define NDN_SERVICE_FRAMEWORK_TIMELINE_TRACE_HPP

#include "common.hpp"

#include <initializer_list>
#include <string>
#include <utility>

namespace ndn_service_framework {

using TimelineFields = std::initializer_list<std::pair<std::string, std::string>>;

uint64_t timelineSteadyMicroseconds();

bool timelineTraceEnvEnabled();

bool phaseTimingEnvEnabled();

void logTimelineTrace(const std::string& role,
                      const std::string& event,
                      const ndn::Name& requestId,
                      TimelineFields fields = {});

// Normalized Spec190 phase evidence. This is an observation channel only;
// lifecycle state remains owned by ServiceUser and the DI runtime.
void logPhaseTiming(const std::string& role,
                    const std::string& phase,
                    const ndn::Name& requestId,
                    TimelineFields fields = {});

void logStreamTimelineTrace(const std::string& role,
                            const std::string& event,
                            const std::string& streamId,
                            uint64_t sessionEpoch,
                            uint64_t cursor,
                            TimelineFields fields = {});

bool hybridCryptoTimingEnvEnabled();

void logHybridCryptoTiming(const std::string& role,
                           const std::string& event,
                           const ndn::Name& requestId,
                           TimelineFields fields = {});

bool controlTimingEnvEnabled();

void logControlTiming(const std::string& role,
                      const std::string& event,
                      const ndn::Name& requestId,
                      TimelineFields fields = {});

} // namespace ndn_service_framework

#endif
