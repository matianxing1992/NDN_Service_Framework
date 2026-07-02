#pragma once

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>

#include <string>

namespace ndn_service_framework {

namespace bootstrap_tlv {
enum {
  CertificateBootstrapRequest = 0xF510,
  CertificateBootstrapResponse = 0xF511,
  CertificateRequest = 0xF512,
  IssuedCertificate = 0xF513,
};
}

struct CertificateBootstrapRequest
{
  std::string token;
  ndn::security::Certificate certificateRequest;

  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
};

struct CertificateBootstrapResponse
{
  bool status = false;
  std::string message;
  ndn::security::Certificate issuedCertificate;
  bool hasIssuedCertificate = false;

  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
};

ndn::Name
makeCertificateBootstrapName(const ndn::Name& controllerPrefix,
                             const ndn::Name& identity);

bool
isCertificateSignedByIdentity(const ndn::security::Certificate& certificate,
                              const ndn::Name& signerIdentity);

ndn::security::Certificate
requestControllerSignedCertificate(ndn::Face& face,
                                   ndn::security::KeyChain& keyChain,
                                   const ndn::Name& controllerPrefix,
                                   const ndn::Name& identity,
                                   const std::string& token,
                                   ndn::time::milliseconds timeout =
                                     ndn::time::milliseconds(5000));

ndn::security::Certificate
ensureControllerSignedCertificate(ndn::Face& face,
                                  ndn::security::KeyChain& keyChain,
                                  const ndn::Name& controllerPrefix,
                                  const ndn::Name& identity,
                                  const std::string& token,
                                  ndn::time::milliseconds timeout =
                                    ndn::time::milliseconds(5000));

} // namespace ndn_service_framework
