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

void logTimelineTrace(const std::string& role,
                      const std::string& event,
                      const ndn::Name& requestId,
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
