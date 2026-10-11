#pragma once

#include <cstdint>

namespace ndn_service_framework {

// For a full five-second sample lifetime, leave one second for response fan-in
// and planning. Shorter TTL/expiry, boot and quality checks remain authoritative;
// this admission policy does not extend lifetime or guarantee timely completion.
inline constexpr std::uint64_t NETWORK_PROBE_CACHE_MAX_AGE_MS = 4000;

} // namespace ndn_service_framework
