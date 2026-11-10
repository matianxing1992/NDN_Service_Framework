#ifndef NDN_SERVICE_FRAMEWORK_XOR_REPAIR_HPP
#define NDN_SERVICE_FRAMEWORK_XOR_REPAIR_HPP

#include <ndn-cxx/util/span.hpp>
#include <cstdint>

namespace ndn_service_framework::detail {
/** XOR a zero-padded source into an existing bounded vector. Geometry,
 * original-wire digests, signatures and authority belong to the caller. */
void xorInto(ndn::span<uint8_t> target, ndn::span<const uint8_t> source);
}
#endif
