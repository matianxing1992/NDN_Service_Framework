#include "AbeHistoricalAuthorization.hpp"

#include "ProducerDataNaming.hpp"

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

bool
historicalCkBindsAttribute(const ndn::Name& ckName, const ndn::Name& producer,
                           const ndn::Name& service, const std::string& attribute)
{
  if (ckName.empty() || producer.empty() || service.empty() || attribute.empty())
    return false;
  const auto bytes = [] (const ndn::Name& value) {
    const auto& wire = value.wireEncode();
    return std::vector<uint8_t>(wire.data(), wire.data() + wire.size());
  };
  const auto expected = makeNamingDigestComponent(computeNamingDigest(
    "ndnsf-naming/hybrid-key-scope",
    {{1, 1, bytes(service)}, {2, 1, bytes(producer)},
     {3, 2, std::vector<uint8_t>(attribute.begin(), attribute.end())}}));
  // Walk to the component that follows SERVICE; the name is a structured one, so a name that does not
  // have that shape is simply not a key for this purpose.
  for (std::size_t i = 0; i + 1 < ckName.size(); ++i) {
    if (ckName[i].toUri() == "SERVICE")
      return ckName[i + 1] == expected;
  }
  return false;
}

} // namespace ndn_service_framework
