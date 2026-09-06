#ifndef NDN_SERVICE_FRAMEWORK_CONTROLLER_VERSION_HPP
#define NDN_SERVICE_FRAMEWORK_CONTROLLER_VERSION_HPP

#include "common.hpp"

#include <cstdint>
#include <string>

namespace ndn_service_framework {

/** Monotonic identity for a Controller authorization state.
 *
 * The generation timestamp is persisted by the Controller and is not a
 * receiver wall-clock timestamp.  Epoch changes describe authorization
 * changes within that generation.
 */
struct ControllerVersion
{
  static constexpr uint32_t TYPE = 0xF700;
  static constexpr uint32_t GenerationTimestampType = 0xF701;
  static constexpr uint32_t EpochType = 0xF702;

  uint64_t controllerGenerationTimestamp = 0;
  uint64_t controllerEpoch = 0;

  bool isValid() const;
  int compare(const ControllerVersion& other) const;
  bool operator==(const ControllerVersion& other) const;
  bool operator!=(const ControllerVersion& other) const;
  std::string toString() const;

  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_CONTROLLER_VERSION_HPP
