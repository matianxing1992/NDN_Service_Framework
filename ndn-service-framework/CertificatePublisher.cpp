#include "CertificatePublisher.hpp"

#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/security/pib/identity.hpp>
#include <ndn-cxx/security/pib/key.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <iostream>
#include <set>
#include <stdexcept>

namespace ndn_service_framework {

NDN_LOG_INIT(ndn_service_framework.CertificatePublisher);

CertificatePublisher::CertificatePublisher(ndn::Face& face,
                                           ndn::security::KeyChain& keyChain,
                                           const ndn::Name& identityOrCertName,
                                           bool registerKeyPrefix)
  : m_face(face)
  , m_certificate(findCertificate(keyChain, identityOrCertName))
  , m_certificates(findIdentityCertificates(keyChain, m_certificate))
  , m_registeredPrefix(registerKeyPrefix ? m_certificate.getKeyName() : m_certificate.getName())
{
  std::set<ndn::Name> prefixes;
  if (registerKeyPrefix) {
    for (const auto& certificate : m_certificates) {
      prefixes.insert(certificate.getKeyName());
    }
  }
  else {
    prefixes.insert(m_certificate.getName());
  }

  for (const auto& prefix : prefixes) {
    m_registeredPrefixes.push_back(prefix);
    m_face.setInterestFilter(
      prefix,
      [this](const ndn::InterestFilter& filter, const ndn::Interest& interest) {
        this->onInterest(filter, interest);
      },
      [] (const ndn::Name& prefix) {
        NDN_LOG_INFO("NDNSF_CERT_PUBLISHER_REGISTERED prefix=" << prefix.toUri());
      },
      [] (const ndn::Name& prefix, const std::string& reason) {
        NDN_LOG_ERROR("NDNSF_CERT_PUBLISHER_REGISTER_FAILED prefix=" << prefix.toUri()
                  << " reason=" << reason);
      });
  }

  NDN_LOG_INFO("Serving certificate name=" << m_certificate.getName()
               << " prefix=" << m_registeredPrefix);
  NDN_LOG_INFO("NDNSF_CERT_PUBLISHER_READY prefix=" << m_registeredPrefix.toUri()
            << " name=" << m_certificate.getName().toUri()
            << " certCount=" << m_certificates.size());
}

const ndn::Name&
CertificatePublisher::getCertificateName() const
{
  return m_certificate.getName();
}

const ndn::Name&
CertificatePublisher::getRegisteredPrefix() const
{
  return m_registeredPrefix;
}

ndn::security::Certificate
CertificatePublisher::findCertificate(ndn::security::KeyChain& keyChain,
                                      const ndn::Name& identityOrCertName)
{
  try {
    return keyChain.getPib()
      .getIdentity(identityOrCertName)
      .getDefaultKey()
      .getDefaultCertificate();
  }
  catch (const std::exception&) {
  }

  try {
    const auto identityName = ndn::security::extractIdentityFromCertName(identityOrCertName);
    const auto keyName = ndn::security::extractKeyNameFromCertName(identityOrCertName);
    return keyChain.getPib()
      .getIdentity(identityName)
      .getKey(keyName)
      .getCertificate(identityOrCertName);
  }
  catch (const std::exception& e) {
    throw std::runtime_error("Cannot locate identity certificate for " +
                             identityOrCertName.toUri() + ": " + e.what());
  }
}

std::vector<ndn::security::Certificate>
CertificatePublisher::findIdentityCertificates(ndn::security::KeyChain& keyChain,
                                               const ndn::security::Certificate& primaryCertificate)
{
  std::vector<ndn::security::Certificate> certificates;
  std::set<ndn::Name> names;

  auto addCertificate = [&] (const ndn::security::Certificate& certificate) {
    if (names.insert(certificate.getName()).second) {
      certificates.push_back(certificate);
    }
  };

  addCertificate(primaryCertificate);
  try {
    const auto identityName = primaryCertificate.getIdentity();
    auto identity = keyChain.getPib().getIdentity(identityName);
    for (const auto& key : identity.getKeys()) {
      try {
        addCertificate(key.getDefaultCertificate());
      }
      catch (const std::exception&) {
      }
    }
  }
  catch (const std::exception&) {
  }

  return certificates;
}

void
CertificatePublisher::onInterest(const ndn::InterestFilter&, const ndn::Interest& interest)
{
  NDN_LOG_INFO("NDNSF_CERT_PUBLISHER_INTEREST interest=" << interest.getName().toUri()
            << " cert=" << m_certificate.getName().toUri()
            << " certCount=" << m_certificates.size());

  for (const auto& certificate : m_certificates) {
    if (!interest.matchesData(certificate)) {
      continue;
    }

    m_face.put(certificate);
    NDN_LOG_INFO("NDNSF_CERT_PUBLISHER_DATA interest=" << interest.getName().toUri()
              << " cert=" << certificate.getName().toUri()
              << " bytes=" << certificate.wireEncode().size());
    return;
  }

  NDN_LOG_INFO("Ignoring certificate Interest that does not match identity cert set "
               << m_certificate.getIdentity() << ": " << interest.getName());
}

} // namespace ndn_service_framework
