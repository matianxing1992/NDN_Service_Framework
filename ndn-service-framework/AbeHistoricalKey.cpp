#include "AbeHistoricalKey.hpp"

#include <stdexcept>

#include <openssl/crypto.h>
#include <nac-abe/algo/abe-support.hpp>
#include <nac-abe/algo/master-key.hpp>
#include <nac-abe/algo/private-key.hpp>
#include <nac-abe/algo/public-params.hpp>

namespace ndn_service_framework {

std::vector<std::uint8_t>
issueHistoricalDecryptionKey(const HistoricalAbeMaterial& material, const std::string& policy)
{
  if (material.publicParameters.empty() || material.masterSecret.empty() || policy.empty())
    throw std::invalid_argument("historical ABE material is incomplete");
  // The stored parameters keep their trailing NUL; fromBuffer expects the exact wire form.
  if (material.publicParameters.back() != '\0' ||
      material.publicParameters.find('\0') != material.publicParameters.size() - 1)
    throw std::runtime_error("historical ABE public parameters are malformed");
  ndn::nacabe::algo::PublicParams params;
  params.fromBuffer(ndn::Buffer(material.publicParameters.begin(),
                                material.publicParameters.end()));
  ndn::nacabe::algo::MasterKey master;
  master.m_msk = material.masterSecret;
  try {
    auto& abe = ndn::nacabe::algo::ABESupport::getInstance();
    auto key = abe.kpPrvKeyGen(params, master, policy);
    const auto wire = key.toBuffer();
    if (!key.m_prv.empty())
      OPENSSL_cleanse(&key.m_prv[0], key.m_prv.size());
    OPENSSL_cleanse(&master.m_msk[0], master.m_msk.size());
    return std::vector<std::uint8_t>(wire.begin(), wire.end());
  }
  catch (...) {
    OPENSSL_cleanse(&master.m_msk[0], master.m_msk.size());
    throw;
  }
}

} // namespace ndn_service_framework
