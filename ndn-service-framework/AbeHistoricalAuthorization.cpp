#include "AbeHistoricalAuthorization.hpp"

#include <algorithm>

namespace ndn_service_framework {

bool
historicalAttributeIsAuthorized(const std::string& policy,
                                const std::set<std::string>& heldAtPublication,
                                const std::set<std::string>& heldNow)
{
  // Only a plain attribute name is decidable here. Anything carrying an operator is refused rather
  // than interpreted: an "A OR B" a requester wrote would widen what the Controller granted.
  if (policy.empty() || policy.size() > 512)
    return false;
  const bool compound = policy.find(" AND ") != std::string::npos ||
                        policy.find(" OR ") != std::string::npos ||
                        policy.find('|') != std::string::npos ||
                        policy.find('&') != std::string::npos ||
                        policy.find('(') != std::string::npos ||
                        policy.find(')') != std::string::npos;
  if (compound)
    return false;
  if (std::any_of(policy.begin(), policy.end(),
                  [] (unsigned char ch) { return ch < 0x20 || ch == 0x7f; }))
    return false;
  return heldAtPublication.count(policy) != 0 && heldNow.count(policy) != 0;
}

} // namespace ndn_service_framework
