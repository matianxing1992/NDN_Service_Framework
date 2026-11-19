#ifndef NDNSF_ABE_HISTORICAL_AUTHORIZATION_HPP
#define NDNSF_ABE_HISTORICAL_AUTHORIZATION_HPP

#include <set>
#include <string>

namespace ndn_service_framework {

/** The attributes an identity held when a generation was current, by identity.
 *
 * Recorded at rotation so a historical request can be judged against what the object's generation
 * actually allowed, not against the attributes the requester holds today.
 */
using HistoricalAttributeSnapshot = std::set<std::string>;

/** Decide whether one historical policy attribute is still authorized.
 *
 * The key is minted only for an attribute that the identity held when the generation was published
 * **and** still holds now, which is the intersection the review asked for. The policy must name a single
 * attribute: a compound expression would let a requester assemble a scope the Controller never granted,
 * and there is no way to prove the old generation's policy expressed exactly that scope.
 */
bool
historicalAttributeIsAuthorized(const std::string& policy,
                                const std::set<std::string>& heldAtPublication,
                                const std::set<std::string>& heldNow);

} // namespace ndn_service_framework

#endif // NDNSF_ABE_HISTORICAL_AUTHORIZATION_HPP
