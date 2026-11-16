#ifndef NDN_SERVICE_FRAMEWORK_CONFIGURED_ACTOR_NAME_HPP
#define NDN_SERVICE_FRAMEWORK_CONFIGURED_ACTOR_NAME_HPP

#include "ProducerDataNaming.hpp"

#include <stdexcept>
#include <string>

namespace ndn_service_framework {

/** Validate an explicitly supplied deployment name before creating an actor.
 * Protocol suffixes and logical service identifiers are not deployment names.
 */
inline void
validateConfiguredActorName(const ndn::Name& name, const char* field)
{
  if (name.empty()) {
    throw std::invalid_argument(std::string(field) + ": an explicit non-root NDN name is required");
  }
  try {
    checkNamingNameSize(name);
  }
  catch (const std::length_error& error) {
    throw std::invalid_argument(std::string(field) + ": " + error.what());
  }
}

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_CONFIGURED_ACTOR_NAME_HPP
