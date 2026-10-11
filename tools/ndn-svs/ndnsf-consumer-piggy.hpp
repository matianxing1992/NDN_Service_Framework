#pragma once

#include <ndn-cxx/name.hpp>
#include <cstring>

namespace ndn::svs::detail {

inline bool consumerPiggyFilterEnabled(const char* value) noexcept
{
  return value != nullptr && std::strcmp(value, "1") == 0;
}

// Opt-in negative admission only. Custom subscription sets and an active
// fetch retain the original opportunistic cache/validation behavior. No
// matching Data is accepted or delivered by this predicate.
template<class Subscriptions, class Matcher>
bool skipUnsubscribedConsumerPiggy(const Name& name, bool enabled,
                                  bool hasPrefix, bool hasProducer,
                                  bool hasPendingFetch,
                                  const Subscriptions& subscriptions,
                                  const Matcher& matches)
{
  if (!enabled || hasPrefix || hasProducer || hasPendingFetch ||
      subscriptions.size() != 4)
    return false;
  static constexpr const char* expressions[] = {
    "^(<>*)<NDNSF><ACK>(<>*)$",
    "^(<>*)<NDNSF><RESPONSE>(<>*)$",
    "^(<>*)<NDNSF><EVENT>(<>*)$",
    "^(<>*)<NDNSF><COLLAB>(<>*)$",
  };
  unsigned mask = 0;
  for (const auto& sub : subscriptions) {
    if (!sub.regex || !sub.autofetch)
      return false;
    unsigned bit = 0;
    for (unsigned i = 0; i < 4; ++i)
      if (sub.regex->getExpr() == expressions[i]) {
        bit = 1U << i;
        break;
      }
    if (bit == 0 || (mask & bit) != 0)
      return false;
    mask |= bit;
  }
  if (mask != 15)
    return false;
  for (const auto& sub : subscriptions)
    if (matches(*sub.regex, name))
      return false;
  return true;
}

} // namespace ndn::svs::detail
