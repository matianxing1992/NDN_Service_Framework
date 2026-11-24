#ifndef NDNSF_ABE_HISTORICAL_KEY_HPP
#define NDNSF_ABE_HISTORICAL_KEY_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace ndn_service_framework {

/** The material of one superseded KP-ABE generation. */
struct HistoricalAbeMaterial
{
  std::string publicParameters;
  std::string masterSecret;
};

/** Mint a decryption key for `policy` under a superseded generation.
 *
 * Returns the key in NAC-ABE's own private-key wire form (`algo::PrivateKey::toBuffer`), so the
 * Controller can deliver a historical DKEY without inventing an encoding. The caller decides
 * whether the identity is still authorized; this function only proves the generation can sign
 * for a policy.
 */
std::vector<std::uint8_t>
issueHistoricalDecryptionKey(const HistoricalAbeMaterial& material, const std::string& policy);

} // namespace ndn_service_framework

#endif // NDNSF_ABE_HISTORICAL_KEY_HPP
