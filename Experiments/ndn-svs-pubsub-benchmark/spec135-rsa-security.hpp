#ifndef SPEC135_RSA_SECURITY_HPP
#define SPEC135_RSA_SECURITY_HPP

#include <ndn-svs/security-options.hpp>

#include <ndn-cxx/encoding/tlv.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace spec135 {

inline uint32_t
configureRsa2048(ndn::KeyChain& keyChain, ndn::svs::SecurityOptions& options,
                 const ndn::Name& identityName)
{
  auto identity = keyChain.createIdentity(identityName, ndn::RsaKeyParams(2048));
  options.dataSigner->signingInfo = ndn::security::signingByIdentity(identity);

  ndn::Data probe(ndn::Name(identityName).append("signature-probe"));
  static const uint8_t content[] = {'S', 'P', 'E', 'C', '1', '3', '5'};
  probe.setContent(ndn::make_span(content));
  options.dataSigner->sign(probe);
  const auto signatureType = probe.getSignatureInfo().getSignatureType();
  if (signatureType != ndn::tlv::SignatureSha256WithRsa) {
    throw std::runtime_error("Spec 135 Data signer is not SignatureSha256WithRsa");
  }
  return signatureType;
}

inline size_t
readBoundedSize(const char* raw, const char* label, size_t minimum, size_t maximum)
{
  if (raw == nullptr || *raw == '\0')
    throw std::runtime_error(std::string("missing ") + label);
  size_t consumed = 0;
  const auto value = std::stoull(raw, &consumed);
  if (raw[consumed] != '\0' || value < minimum || value > maximum)
    throw std::runtime_error(std::string("invalid ") + label);
  return static_cast<size_t>(value);
}

} // namespace spec135

#endif // SPEC135_RSA_SECURITY_HPP
