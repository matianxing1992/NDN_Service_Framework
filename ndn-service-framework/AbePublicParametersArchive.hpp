#ifndef NDNSF_ABE_PUBLIC_PARAMETERS_ARCHIVE_HPP
#define NDNSF_ABE_PUBLIC_PARAMETERS_ARCHIVE_HPP

#include "ControllerGenerationStore.hpp"

#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>

namespace ndn_service_framework {

/** Name of the historical proof a Controller publishes for an archived generation.
 *
 * The original `PUBPARAMS` packet keeps its name and its original wire; a Controller that no longer
 * holds that key must never sign different bytes under the same name. A historical answer therefore
 * lives under a name of its own that binds the generation and the digest of the original content:
 *
 *   <identity>/NDNSF/HISTORY/PUBPARAMS/<generation>/<epoch>/<sha256-hex>
 */
ndn::Name
historicalPublicParametersName(const ndn::Name& identity, uint64_t generation, uint64_t epoch,
                               const std::string& digestHex);

/** Build the historical proof for one archived generation.
 *
 * The content is a small text envelope so a reader can check the binding before using it:
 *
 *   NDNSF-HIST-PUBPARAMS-v1
 *   original <the name the generation published under>
 *   digest <sha256 of the original content>
 *   length <bytes>
 *   <the archived public parameters>
 *
 * Returns false when the entry has no generation, no parameters, no original name, or a digest that
 * does not match the stored parameters, so a caller never publishes an unbound proof.
 */
bool
buildHistoricalPublicParametersData(const ControllerGenerationStore::AbeHistoryEntry& entry,
                                    const ndn::Name& identity,
                                    const std::function<void(ndn::Data&)>& sign,
                                    ndn::Data& result);

/** Build the signed index a cold reader uses to find the proofs.
 *
 * The proof names carry the digest of the original content, which a reader cannot guess. This index
 * lists what exists, and is the only place a reader learns the digest to ask under:
 *
 *   NDNSF-HIST-PUBPARAMS-INDEX-v1
 *   <generation> <epoch> <sha256-hex> <original-name-uri>
 */
bool
buildHistoricalPublicParametersIndex(
  const std::vector<ControllerGenerationStore::AbeHistoryEntry>& entries,
  const ndn::Name& identity, const std::function<void(ndn::Data&)>& sign, ndn::Data& result);

/** Parse the index; returns one record per line and fails on any malformed line. */
bool
parseHistoricalPublicParametersIndex(const std::string& content,
                                     std::vector<std::tuple<uint64_t, uint64_t, std::string,
                                                            std::string>>& records);

/** The parsed proof envelope. */
struct HistoricalPublicParameters
{
  std::string originalName;
  std::string digest;
  uint64_t length = 0;
  std::string publicParameters;
};

/** Parse and check a proof: the digest must match the bytes it claims to describe.
 *
 * A reader can then use the parameters only if the envelope is self-consistent and the Data that carried
 * it was signed by the Controller.
 */
bool
parseHistoricalPublicParameters(const std::string& content, HistoricalPublicParameters& proof);

/** Build the proofs a Controller can serve, skipping entries it cannot bind. */
std::vector<std::shared_ptr<ndn::Data>>
collectHistoricalPublicParameterAnswers(
  const std::vector<ControllerGenerationStore::AbeHistoryEntry>& entries,
  const ndn::Name& identity,
  const std::function<void(ndn::Data&)>& sign);

} // namespace ndn_service_framework

#endif // NDNSF_ABE_PUBLIC_PARAMETERS_ARCHIVE_HPP
