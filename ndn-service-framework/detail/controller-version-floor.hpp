#ifndef NDN_SERVICE_FRAMEWORK_DETAIL_CONTROLLER_VERSION_FLOOR_HPP
#define NDN_SERVICE_FRAMEWORK_DETAIL_CONTROLLER_VERSION_FLOOR_HPP

#include "../ControllerVersion.hpp"

#include <charconv>
#include <cstdlib>
#include <string_view>

namespace ndn_service_framework::detail {

inline constexpr char CONTROLLER_VERSION_FLOOR_ENV[] = "NDNSF_CONTROLLER_MIN_VERSION";

/** Check the trusted deployment floor without changing exact wire query semantics.
 *  A supplied but malformed floor fails closed.
 */
inline bool
meetsControllerVersionFloor(const ControllerVersion& version,
                            const char* configured = std::getenv(CONTROLLER_VERSION_FLOOR_ENV))
{
  if (!configured) {
    return true;
  }
  const std::string_view text(configured);
  const auto separator = text.find(':');
  if (!version.isValid() || separator == std::string_view::npos) {
    return false;
  }
  const auto parse = [](std::string_view field, uint64_t& result) {
    if (field.empty() || field.size() > 20 || field.front() == '0') {
      return false;
    }
    const auto end = field.data() + field.size();
    const auto parsed = std::from_chars(field.data(), end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end && result != 0;
  };
  ControllerVersion minimum;
  return parse(text.substr(0, separator), minimum.controllerGenerationTimestamp) &&
         parse(text.substr(separator + 1), minimum.controllerEpoch) &&
         version.compare(minimum) >= 0;
}

} // namespace ndn_service_framework::detail

#endif // NDN_SERVICE_FRAMEWORK_DETAIL_CONTROLLER_VERSION_FLOOR_HPP
