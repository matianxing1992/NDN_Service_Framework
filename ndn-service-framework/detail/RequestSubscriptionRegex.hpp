#pragma once

#include "ndn-service-framework/utils.hpp"

namespace ndn_service_framework::detail {

inline std::string
requestSubscriptionRegex(ndn::Name service)
{
  return "^<>*<NDNSF><REQUEST>" + NameToRegexString(service) + "<>*$";
}

} // namespace ndn_service_framework::detail
