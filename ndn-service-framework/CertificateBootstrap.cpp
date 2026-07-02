#include "CertificateBootstrap.hpp"

#include "NDNSFMessages.hpp"

#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <stdexcept>

namespace ndn_service_framework {

NDN_LOG_INIT(ndn_service_framework.CertificateBootstrap);

ndn::Block
CertificateBootstrapRequest::wireEncode() const
{
  ndn::Block block(bootstrap_tlv::CertificateBootstrapRequest);
  block.push_back(ndn::makeStringBlock(tlv::TokenType, token));
  ndn::Block certBlock(bootstrap_tlv::CertificateRequest);
  certBlock.push_back(certificateRequest.wireEncode());
  certBlock.encode();
  block.push_back(certBlock);
  block.encode();
  return block;
}

bool
CertificateBootstrapRequest::wireDecode(const ndn::Block& block)
{
  auto parsed = block.type() == bootstrap_tlv::CertificateBootstrapRequest ?
    block : block.blockFromValue();
  if (parsed.type() != bootstrap_tlv::CertificateBootstrapRequest) {
    return false;
  }

  parsed.parse();
  token.clear();
  bool hasCertificate = false;

  for (const auto& element : parsed.elements()) {
    if (element.type() == tlv::TokenType) {
      token = ndn::readString(element);
    }
    else if (element.type() == bootstrap_tlv::CertificateRequest) {
      auto certWrapper = element;
      certWrapper.parse();
      if (certWrapper.elements().empty()) {
        return false;
      }
      certificateRequest = ndn::security::Certificate(certWrapper.elements().front());
      hasCertificate = true;
    }
  }

  return !token.empty() && hasCertificate;
}

ndn::Block
CertificateBootstrapResponse::wireEncode() const
{
  ndn::Block block(bootstrap_tlv::CertificateBootstrapResponse);
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StatusType, status ? 1 : 0));
  block.push_back(ndn::makeStringBlock(tlv::ErrorInfoType, message));
  if (status && hasIssuedCertificate) {
    ndn::Block certBlock(bootstrap_tlv::IssuedCertificate);
    certBlock.push_back(issuedCertificate.wireEncode());
    certBlock.encode();
    block.push_back(certBlock);
  }
  block.encode();
  return block;
}

bool
CertificateBootstrapResponse::wireDecode(const ndn::Block& block)
{
  auto parsed = block.type() == bootstrap_tlv::CertificateBootstrapResponse ?
    block : block.blockFromValue();
  if (parsed.type() != bootstrap_tlv::CertificateBootstrapResponse) {
    return false;
  }

  parsed.parse();
  status = false;
  message.clear();
  hasIssuedCertificate = false;

  for (const auto& element : parsed.elements()) {
    if (element.type() == tlv::StatusType) {
      status = ndn::readNonNegativeInteger(element) != 0;
    }
    else if (element.type() == tlv::ErrorInfoType) {
      message = ndn::readString(element);
    }
    else if (element.type() == bootstrap_tlv::IssuedCertificate) {
      auto certWrapper = element;
      certWrapper.parse();
      if (certWrapper.elements().empty()) {
        return false;
      }
      issuedCertificate = ndn::security::Certificate(certWrapper.elements().front());
      hasIssuedCertificate = true;
    }
  }

  return !status || hasIssuedCertificate;
}

ndn::Name
makeCertificateBootstrapName(const ndn::Name& controllerPrefix,
                             const ndn::Name& identity)
{
  ndn::Name name(controllerPrefix);
  name.append("NDNSF").append("CERTBOOTSTRAP");
  name.append(identity);
  return name;
}

bool
isCertificateSignedByIdentity(const ndn::security::Certificate& certificate,
                              const ndn::Name& signerIdentity)
{
  try {
    const auto keyLocator = certificate.getKeyLocator();
    if (!keyLocator || keyLocator->getType() != ndn::tlv::Name) {
      return false;
    }
    return signerIdentity.isPrefixOf(keyLocator->getName());
  }
  catch (const std::exception&) {
    return false;
  }
}

ndn::security::Certificate
requestControllerSignedCertificate(ndn::Face& face,
                                   ndn::security::KeyChain& keyChain,
                                   const ndn::Name& controllerPrefix,
                                   const ndn::Name& identity,
                                   const std::string& token,
                                   ndn::time::milliseconds timeout)
{
  if (token.empty()) {
    throw std::invalid_argument("bootstrap token is empty");
  }

  auto pibIdentity = keyChain.getPib().getIdentity(identity);
  auto key = pibIdentity.getDefaultKey();
  auto localCertificate = key.getDefaultCertificate();

  CertificateBootstrapRequest request;
  request.token = token;
  request.certificateRequest = localCertificate;

  ndn::Interest interest(makeCertificateBootstrapName(controllerPrefix, identity));
  interest.setMustBeFresh(true);
  interest.setCanBePrefix(false);
  interest.setInterestLifetime(timeout);
  interest.setApplicationParameters(request.wireEncode());

  bool done = false;
  bool ok = false;
  std::string failure = "certificate bootstrap timed out";
  ndn::security::Certificate issuedCertificate;

  face.expressInterest(
    interest,
    [&] (const ndn::Interest&, const ndn::Data& data) {
      try {
        CertificateBootstrapResponse response;
        if (!response.wireDecode(data.getContent())) {
          failure = "malformed certificate bootstrap response";
        }
        else if (!response.status) {
          failure = response.message.empty() ? "certificate bootstrap refused" : response.message;
        }
        else {
          issuedCertificate = response.issuedCertificate;
          keyChain.setDefaultCertificate(key, issuedCertificate);
          ok = true;
          NDN_LOG_INFO("NDNSF_CERT_BOOTSTRAP_INSTALLED identity=" << identity.toUri()
                       << " cert=" << issuedCertificate.getName().toUri());
        }
      }
      catch (const std::exception& e) {
        failure = e.what();
      }
      done = true;
    },
    [&] (const ndn::Interest&, const ndn::lp::Nack& nack) {
      failure = "certificate bootstrap Nack reason=" + std::to_string(static_cast<int>(nack.getReason()));
      done = true;
    },
    [&] (const ndn::Interest&) {
      failure = "certificate bootstrap timed out";
      done = true;
    });

  const auto deadline = ndn::time::steady_clock::now() + timeout + ndn::time::milliseconds(1000);
  while (!done && ndn::time::steady_clock::now() < deadline) {
    face.processEvents(ndn::time::milliseconds(50));
  }

  if (!ok) {
    throw std::runtime_error(failure);
  }

  return issuedCertificate;
}

ndn::security::Certificate
ensureControllerSignedCertificate(ndn::Face& face,
                                  ndn::security::KeyChain& keyChain,
                                  const ndn::Name& controllerPrefix,
                                  const ndn::Name& identity,
                                  const std::string& token,
                                  ndn::time::milliseconds timeout)
{
  auto pibIdentity = keyChain.getPib().getIdentity(identity);
  auto key = pibIdentity.getDefaultKey();
  auto localCertificate = key.getDefaultCertificate();
  if (isCertificateSignedByIdentity(localCertificate, controllerPrefix)) {
    NDN_LOG_INFO("NDNSF_CERT_BOOTSTRAP_REUSED identity=" << identity.toUri()
                 << " cert=" << localCertificate.getName().toUri());
    return localCertificate;
  }

  return requestControllerSignedCertificate(face, keyChain, controllerPrefix,
                                            identity, token, timeout);
}

} // namespace ndn_service_framework
