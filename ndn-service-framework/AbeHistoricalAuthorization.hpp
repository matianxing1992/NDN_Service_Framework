#ifndef NDNSF_ABE_HISTORICAL_AUTHORIZATION_HPP
#define NDNSF_ABE_HISTORICAL_AUTHORIZATION_HPP

#include "ControllerVersion.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/data.hpp>

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

/** Encode the canonical identity and Controller version of a historical grant. */
std::string
makeHistoricalGrantRecordHeader(const ndn::Name& identity, uint64_t generation, uint64_t epoch);

struct HistoricalGrantRecord
{
  ndn::Name identity;
  ControllerVersion version;
  std::set<ndn::Name> services;
};

std::optional<HistoricalGrantRecord>
decodeHistoricalGrantRecord(std::string_view content);

/** Check that a signed grant record covers the expected identity, version and policy.
 *
 * The record is the Controller's own statement of what it granted, at the version the object was published
 * under. Its content is line oriented:
 *
 *   NDNSF-GRANT-v1
 *   identity /name
 *   generation <n>
 *   epoch <n>
 *   granted <service>
 *
 * The policy must be `/PERMISSION/<service>`, and the record must list that same service.
 */
bool
historicalGrantRecordCoversPolicy(const std::string& recordContent, const ndn::Name& identity,
                                  uint64_t generation, uint64_t epoch, const std::string& policy);

/** Check that a CK name binds its producer, service and attribute. */
bool
historicalCkBindsAttribute(const ndn::Name& ckName, const ndn::Name& producer,
                           const ndn::Name& service, const std::string& attribute);

/** Resolve the payload's CK reference from its exact authenticated CK packet. */
std::optional<ndn::Name>
historicalCkLocator(const ndn::Data& packet, const ndn::Name& producer);

} // namespace ndn_service_framework

#endif // NDNSF_ABE_HISTORICAL_AUTHORIZATION_HPP
