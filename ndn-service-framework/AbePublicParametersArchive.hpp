#ifndef NDNSF_ABE_PUBLIC_PARAMETERS_ARCHIVE_HPP
#define NDNSF_ABE_PUBLIC_PARAMETERS_ARCHIVE_HPP

#include "ControllerGenerationStore.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/key-locator.hpp>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>

namespace ndn_service_framework {

/** Name a generation's public parameters were published under.
 *
 * The version component is derived from the generation, so an archived generation keeps the
 * exact name a consumer would ask for - which is what makes a historical fetch resolvable.
 */
ndn::Name
archivedPublicParametersName(const ndn::Name& identity, uint64_t generation, uint64_t epoch);

/** Build the Data that answers a historical public-parameter Interest.
 *
 * Returns false when the archived entry does not belong to `identity` or carries no parameters,
 * so a caller never publishes an empty or mismatched answer. The Data is signed by `cert`; no
 * freshness is asserted, because these bytes are immutable for their generation.
 */
/** The signer is supplied by the caller so the naming and content rules stay testable without
 * a live key chain; the Controller passes a certificate signer. */
bool
buildArchivedPublicParametersData(const ControllerGenerationStore::AbeHistoryEntry& entry,
                                  const ndn::Name& identity,
                                  const std::function<void(ndn::Data&)>& sign,
                                  ndn::Data& result);

/** Build the answers a Controller can serve, skipping entries it cannot answer for.
 *
 * Returns one Data per usable archived generation; a mismatched name or missing parameters drops
 * that generation instead of publishing a wrong answer. The count is the caller's evidence that
 * something was actually registered.
 */
std::vector<std::shared_ptr<ndn::Data>>
collectArchivedPublicParameterAnswers(
  const std::vector<ControllerGenerationStore::AbeHistoryEntry>& entries,
  const ndn::Name& identity,
  const std::function<void(ndn::Data&)>& sign);

} // namespace ndn_service_framework

#endif // NDNSF_ABE_PUBLIC_PARAMETERS_ARCHIVE_HPP
