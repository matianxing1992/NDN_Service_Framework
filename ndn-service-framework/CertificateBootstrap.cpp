#include "CertificateBootstrap.hpp"

#include "NDNSFMessages.hpp"

#include <ndn-cxx/encoding/buffer-stream.hpp>
#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/transform.hpp>
#include <ndn-cxx/util/logger.hpp>
#include <ndn-cxx/util/random.hpp>

#include <stdexcept>

namespace ndn_service_framework {

NDN_LOG_INIT(ndn_service_framework.CertificateBootstrap);

namespace {

ndn::span<const uint8_t>
bufferToSpan(const ndn::Buffer& buffer)
{
  return ndn::span<const uint8_t>(buffer.data(), buffer.size());
}

ndn::span<uint8_t>
mutableBufferToSpan(ndn::Buffer& buffer)
{
  return ndn::span<uint8_t>(buffer.data(), buffer.size());
}

ndn::Buffer
runAesCbc(ndn::span<const uint8_t> input,
          ndn::span<const uint8_t> key,
          ndn::span<const uint8_t> iv,
          ndn::CipherOperator op)
{
  ndn::OBufferStream output;
  ndn::security::transform::bufferSource(input) >>
    ndn::security::transform::blockCipher(ndn::BlockCipherAlgorithm::AES_CBC,
                                          op,
                                          key,
                                          iv) >>
    ndn::security::transform::streamSink(output);

  const auto result = output.buf();
  return ndn::Buffer(result->begin(), result->end());
}

ndn::Block
makeCertificateBootstrapProofData(const CertificateBootstrapRequest& request)
{
  ndn::Block block(bootstrap_tlv::CertificateBootstrapProofData);
  block.push_back(request.identity.wireEncode());
  block.push_back(ndn::makeStringBlock(tlv::TokenType, request.token));
  ndn::Block certBlock(bootstrap_tlv::CertificateRequest);
  certBlock.push_back(request.certificateRequest.wireEncode());
  certBlock.encode();
  block.push_back(certBlock);
  block.push_back(ndn::makeBinaryBlock(bootstrap_tlv::ProofNonce,
                                       request.proofNonce.begin(),
                                       request.proofNonce.end()));
  block.encode();
  return block;
}

ndn::security::Certificate
getLocalControllerCertificateForBootstrap(ndn::security::KeyChain& keyChain,
                                          const ndn::Name& controllerPrefix)
{
  try {
    auto cert = keyChain.getPib()
      .getIdentity(controllerPrefix)
      .getDefaultKey()
      .getDefaultCertificate();
    if (!cert.isValid()) {
      throw std::runtime_error("controller certificate is not valid");
    }
    return cert;
  }
  catch (const std::exception& e) {
    throw std::runtime_error("Cannot encrypt certificate bootstrap request because "
                             "the local KeyChain has no usable Controller certificate for " +
                             controllerPrefix.toUri() + ": " + e.what());
  }
}

} // namespace

ndn::Block
CertificateBootstrapRequest::wireEncode() const
{
  ndn::Block block(bootstrap_tlv::CertificateBootstrapRequest);
  block.push_back(identity.wireEncode());
  block.push_back(ndn::makeStringBlock(tlv::TokenType, token));
  ndn::Block certBlock(bootstrap_tlv::CertificateRequest);
  certBlock.push_back(certificateRequest.wireEncode());
  certBlock.encode();
  block.push_back(certBlock);
  if (!proofNonce.empty()) {
    block.push_back(ndn::makeBinaryBlock(bootstrap_tlv::ProofNonce,
                                         proofNonce.begin(),
                                         proofNonce.end()));
  }
  if (!proofSignature.empty()) {
    block.push_back(ndn::makeBinaryBlock(bootstrap_tlv::ProofSignature,
                                         proofSignature.begin(),
                                         proofSignature.end()));
  }
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
  identity.clear();
  token.clear();
  proofNonce.clear();
  proofSignature.clear();
  bool hasIdentity = false;
  bool hasCertificate = false;

  for (const auto& element : parsed.elements()) {
    if (element.type() == ndn::tlv::Name) {
      identity.wireDecode(element);
      hasIdentity = !identity.empty();
    }
    else if (element.type() == tlv::TokenType) {
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
    else if (element.type() == bootstrap_tlv::ProofNonce) {
      proofNonce = ndn::Buffer(element.value(), element.value_size());
    }
    else if (element.type() == bootstrap_tlv::ProofSignature) {
      proofSignature = ndn::Buffer(element.value(), element.value_size());
    }
  }

  return hasIdentity && !token.empty() && hasCertificate;
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

ndn::Block
EncryptedCertificateBootstrapRequest::wireEncode() const
{
  ndn::Block block(bootstrap_tlv::EncryptedCertificateBootstrapRequest);
  block.push_back(ndn::makeStringBlock(tlv::RecipientCertNameType, recipientCertName));
  block.push_back(ndn::makeStringBlock(tlv::AlgorithmType, algorithm));
  block.push_back(ndn::makeBinaryBlock(tlv::EncryptedAesKeyType,
                                       encryptedAesKey.begin(),
                                       encryptedAesKey.end()));
  block.push_back(ndn::makeBinaryBlock(tlv::IvType, iv.begin(), iv.end()));
  block.push_back(ndn::makeBinaryBlock(tlv::CipherTextType,
                                       cipherText.begin(),
                                       cipherText.end()));
  block.encode();
  return block;
}

bool
EncryptedCertificateBootstrapRequest::wireDecode(const ndn::Block& block)
{
  auto parsed = block.type() == bootstrap_tlv::EncryptedCertificateBootstrapRequest ?
    block : block.blockFromValue();
  if (parsed.type() != bootstrap_tlv::EncryptedCertificateBootstrapRequest) {
    return false;
  }

  parsed.parse();
  recipientCertName.clear();
  algorithm.clear();
  encryptedAesKey.clear();
  iv.clear();
  cipherText.clear();

  for (const auto& element : parsed.elements()) {
    if (element.type() == tlv::RecipientCertNameType) {
      recipientCertName = ndn::readString(element);
    }
    else if (element.type() == tlv::AlgorithmType) {
      algorithm = ndn::readString(element);
    }
    else if (element.type() == tlv::EncryptedAesKeyType) {
      encryptedAesKey = ndn::Buffer(element.value(), element.value_size());
    }
    else if (element.type() == tlv::IvType) {
      iv = ndn::Buffer(element.value(), element.value_size());
    }
    else if (element.type() == tlv::CipherTextType) {
      cipherText = ndn::Buffer(element.value(), element.value_size());
    }
  }

  return !recipientCertName.empty() &&
         algorithm == "RSA-WRAPPED-AES-CBC" &&
         !encryptedAesKey.empty() &&
         !iv.empty() &&
         !cipherText.empty();
}

EncryptedCertificateBootstrapRequest
encryptCertificateBootstrapRequestForCertificate(const CertificateBootstrapRequest& request,
                                                 const ndn::security::Certificate& recipientCert)
{
  ndn::security::transform::PublicKey recipientPublicKey;
  recipientPublicKey.loadPkcs8(recipientCert.getPublicKey());
  if (recipientPublicKey.getKeyType() != ndn::KeyType::RSA) {
    throw std::invalid_argument("Certificate bootstrap encryption requires an RSA Controller certificate");
  }

  ndn::Block plaintext = request.wireEncode();
  plaintext.encode();

  ndn::Buffer aesKey(32);
  ndn::Buffer iv(16);
  ndn::random::generateSecureBytes(mutableBufferToSpan(aesKey));
  ndn::random::generateSecureBytes(mutableBufferToSpan(iv));

  ndn::Buffer cipherText = runAesCbc(ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
                                     bufferToSpan(aesKey),
                                     bufferToSpan(iv),
                                     ndn::CipherOperator::ENCRYPT);
  auto encryptedAesKey = recipientPublicKey.encrypt(bufferToSpan(aesKey));

  EncryptedCertificateBootstrapRequest encrypted;
  encrypted.recipientCertName = recipientCert.getName().toUri();
  encrypted.algorithm = "RSA-WRAPPED-AES-CBC";
  encrypted.encryptedAesKey = ndn::Buffer(encryptedAesKey->begin(), encryptedAesKey->end());
  encrypted.iv = iv;
  encrypted.cipherText = cipherText;
  return encrypted;
}

CertificateBootstrapRequest
decryptCertificateBootstrapRequestWithKeyChain(const EncryptedCertificateBootstrapRequest& encryptedRequest,
                                               const ndn::security::KeyChain& keyChain)
{
  if (encryptedRequest.algorithm != "RSA-WRAPPED-AES-CBC") {
    throw std::invalid_argument("Unsupported encrypted certificate bootstrap algorithm: " +
                                encryptedRequest.algorithm);
  }

  const ndn::Name recipientCertName(encryptedRequest.recipientCertName);
  const ndn::Name recipientKeyName = ndn::security::extractKeyNameFromCertName(recipientCertName);
  auto aesKey = keyChain.getTpm().decrypt(bufferToSpan(encryptedRequest.encryptedAesKey),
                                          recipientKeyName);
  if (aesKey == nullptr) {
    throw std::runtime_error("Cannot decrypt certificate bootstrap AES key with Controller KeyChain");
  }

  ndn::Buffer plaintext = runAesCbc(bufferToSpan(encryptedRequest.cipherText),
                                    ndn::span<const uint8_t>(aesKey->data(), aesKey->size()),
                                    bufferToSpan(encryptedRequest.iv),
                                    ndn::CipherOperator::DECRYPT);

  auto [ok, block] = ndn::Block::fromBuffer(bufferToSpan(plaintext));
  if (!ok) {
    throw std::runtime_error("Decrypted certificate bootstrap request is not a valid TLV block");
  }

  CertificateBootstrapRequest request;
  if (!request.wireDecode(block)) {
    throw std::runtime_error("Decrypted TLV block is not a CertificateBootstrapRequest");
  }
  return request;
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
  return requestControllerSignedCertificate(face, keyChain, controllerPrefix,
                                            identity, identity, token, timeout);
}

ndn::security::Certificate
requestControllerSignedCertificate(ndn::Face& face,
                                   ndn::security::KeyChain& keyChain,
                                   const ndn::Name& controllerPrefix,
                                   const ndn::Name& certificateIdentity,
                                   const ndn::Name& bootstrapIdentity,
                                   const std::string& token,
                                   ndn::time::milliseconds timeout)
{
  if (token.empty()) {
    throw std::invalid_argument("bootstrap token is empty");
  }
  if (bootstrapIdentity.empty()) {
    throw std::invalid_argument("bootstrap identity is empty");
  }

  auto pibIdentity = keyChain.getPib().getIdentity(certificateIdentity);
  auto key = pibIdentity.getDefaultKey();
  auto localCertificate = key.getDefaultCertificate();

  CertificateBootstrapRequest request;
  request.identity = bootstrapIdentity;
  request.token = token;
  request.certificateRequest = localCertificate;
  request.proofNonce = ndn::Buffer(32);
  ndn::random::generateSecureBytes(mutableBufferToSpan(request.proofNonce));
  auto proofData = makeCertificateBootstrapProofData(request);
  proofData.encode();
  auto proofSignature = keyChain.getTpm().sign(
    ndn::InputBuffers{ndn::span<const uint8_t>(proofData.data(), proofData.size())},
    key.getName(),
    ndn::DigestAlgorithm::SHA256);
  if (proofSignature == nullptr) {
    throw std::runtime_error("Cannot sign certificate bootstrap proof with local KeyChain");
  }
  request.proofSignature = ndn::Buffer(proofSignature->begin(), proofSignature->end());
  auto controllerCert = getLocalControllerCertificateForBootstrap(keyChain, controllerPrefix);
  auto encryptedRequest = encryptCertificateBootstrapRequestForCertificate(request, controllerCert);

  ndn::Interest interest(makeCertificateBootstrapName(controllerPrefix, bootstrapIdentity));
  interest.setMustBeFresh(true);
  interest.setCanBePrefix(false);
  interest.setInterestLifetime(timeout);
  interest.setApplicationParameters(encryptedRequest.wireEncode());
  keyChain.sign(interest, ndn::security::signingByIdentity(certificateIdentity));

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
          NDN_LOG_INFO("NDNSF_CERT_BOOTSTRAP_INSTALLED identity=" << certificateIdentity.toUri()
                       << " cert=" << issuedCertificate.getName().toUri()
                       << " encryptedRequest=true requesterProof=true");
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
  return ensureControllerSignedCertificate(face, keyChain, controllerPrefix,
                                           identity, identity, token, timeout);
}

ndn::security::Certificate
ensureControllerSignedCertificate(ndn::Face& face,
                                  ndn::security::KeyChain& keyChain,
                                  const ndn::Name& controllerPrefix,
                                  const ndn::Name& certificateIdentity,
                                  const ndn::Name& bootstrapIdentity,
                                  const std::string& token,
                                  ndn::time::milliseconds timeout)
{
  auto pibIdentity = keyChain.getPib().getIdentity(certificateIdentity);
  auto key = pibIdentity.getDefaultKey();
  auto localCertificate = key.getDefaultCertificate();
  if (isCertificateSignedByIdentity(localCertificate, controllerPrefix)) {
    NDN_LOG_INFO("NDNSF_CERT_BOOTSTRAP_REUSED identity=" << certificateIdentity.toUri()
                 << " cert=" << localCertificate.getName().toUri());
    return localCertificate;
  }

  return requestControllerSignedCertificate(face, keyChain, controllerPrefix,
                                            certificateIdentity, bootstrapIdentity,
                                            token, timeout);
}

} // namespace ndn_service_framework
