#pragma once

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>

#include <vector>

namespace ndn_service_framework {

class CertificatePublisher
{
public:
  CertificatePublisher(ndn::Face& face,
                       ndn::security::KeyChain& keyChain,
                       const ndn::Name& identityOrCertName,
                       bool registerKeyPrefix = true);

  const ndn::Name& getCertificateName() const;
  const ndn::Name& getRegisteredPrefix() const;

private:
  static ndn::security::Certificate findCertificate(ndn::security::KeyChain& keyChain,
                                                    const ndn::Name& identityOrCertName);
  static std::vector<ndn::security::Certificate>
  findIdentityCertificates(ndn::security::KeyChain& keyChain,
                           const ndn::security::Certificate& primaryCertificate);

  void onInterest(const ndn::InterestFilter& filter, const ndn::Interest& interest);

private:
  ndn::Face& m_face;
  ndn::security::Certificate m_certificate;
  std::vector<ndn::security::Certificate> m_certificates;
  ndn::Name m_registeredPrefix;
  std::vector<ndn::Name> m_registeredPrefixes;
};

} // namespace ndn_service_framework
