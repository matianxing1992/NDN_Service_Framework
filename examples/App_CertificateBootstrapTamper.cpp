#include "ndn-service-framework/CertificateBootstrap.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"

#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/random.hpp>

#include <iostream>
#include <stdexcept>

namespace {

const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
const ndn::Name USER_IDENTITY("/example/hello/user");

ndn::span<uint8_t>
mutableBufferToSpan(ndn::Buffer& buffer)
{
  return ndn::span<uint8_t>(buffer.data(), buffer.size());
}

std::string
getOption(int argc, char** argv, const std::string& option, const std::string& fallback)
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == option) {
      return argv[i + 1];
    }
  }
  return fallback;
}

bool
hasFlag(int argc, char** argv, const std::string& option)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == option) {
      return true;
    }
  }
  return false;
}

ndn::security::Certificate
getOrCreateIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
{
  try {
    return keyChain.getPib()
      .getIdentity(identity)
      .getDefaultKey()
      .getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey()
      .getDefaultCertificate();
  }
}

ndn::security::Certificate
getControllerCertificate(ndn::security::KeyChain& keyChain, const ndn::Name& controllerPrefix)
{
  return keyChain.getPib()
    .getIdentity(controllerPrefix)
    .getDefaultKey()
    .getDefaultCertificate();
}

ndn::Block
makeProofData(const ndn_service_framework::CertificateBootstrapRequest& request)
{
  using namespace ndn_service_framework;

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

} // namespace

int
main(int argc, char** argv)
{
  using namespace ndn_service_framework;

  try {
    const ndn::Name identity(getOption(argc, argv, "--identity", USER_IDENTITY.toUri()));
    const ndn::Name controllerPrefix(getOption(argc, argv, "--controller", CONTROLLER_PREFIX.toUri()));
    const std::string token = getOption(argc, argv, "--bootstrap-token", "user-token-045");
    const bool validRequest = hasFlag(argc, argv, "--valid-request");
    const auto expected = getOption(argc, argv, "--expect-message",
                                    validRequest ? "issued" : "certificate bootstrap proof invalid");

    ndn::Face face;
    ndn::security::KeyChain keyChain;
    auto localCert = getOrCreateIdentity(keyChain, identity);
    auto key = keyChain.getPib().getIdentity(identity).getDefaultKey();

    CertificateBootstrapRequest request;
    request.identity = identity;
    request.token = token;
    request.certificateRequest = localCert;
    request.proofNonce = ndn::Buffer(32);
    ndn::random::generateSecureBytes(mutableBufferToSpan(request.proofNonce));

    auto proofData = makeProofData(request);
    proofData.encode();
    auto proofSignature = keyChain.getTpm().sign(
      ndn::InputBuffers{ndn::span<const uint8_t>(proofData.data(), proofData.size())},
      key.getName(),
      ndn::DigestAlgorithm::SHA256);
    if (proofSignature == nullptr || proofSignature->empty()) {
      throw std::runtime_error("failed to create bootstrap proof signature");
    }

    request.proofSignature = ndn::Buffer(proofSignature->begin(), proofSignature->end());
    if (!validRequest) {
      request.proofSignature[0] ^= 0x01;
    }

    auto controllerCert = getControllerCertificate(keyChain, controllerPrefix);
    auto encryptedRequest = encryptCertificateBootstrapRequestForCertificate(request, controllerCert);

    ndn::Interest interest(makeCertificateBootstrapName(controllerPrefix, identity));
    interest.setMustBeFresh(true);
    interest.setCanBePrefix(false);
    interest.setInterestLifetime(ndn::time::milliseconds(5000));
    interest.setApplicationParameters(encryptedRequest.wireEncode());
    keyChain.sign(interest, ndn::security::signingByIdentity(identity));

    bool done = false;
    bool matched = false;
    std::string message = "no response";

    face.expressInterest(
      interest,
      [&] (const ndn::Interest&, const ndn::Data& data) {
        CertificateBootstrapResponse response;
        if (response.wireDecode(data.getContent())) {
          message = response.message;
          matched = (response.status == validRequest) &&
                    message.find(expected) != std::string::npos;
        }
        else {
          message = "malformed response";
        }
        done = true;
      },
      [&] (const ndn::Interest&, const ndn::lp::Nack&) {
        message = "Nack";
        done = true;
      },
      [&] (const ndn::Interest&) {
        message = "timeout";
        done = true;
      });

    const auto deadline = ndn::time::steady_clock::now() + ndn::time::seconds(6);
    while (!done && ndn::time::steady_clock::now() < deadline) {
      face.processEvents(ndn::time::milliseconds(50));
    }

    std::cout << (validRequest ? "VALID_BOOTSTRAP_RESPONSE=" : "TAMPERED_BOOTSTRAP_RESPONSE=")
              << message << std::endl;
    if (!matched) {
      std::cerr << "Expected response containing: " << expected << std::endl;
      return 1;
    }
    if (validRequest) {
      std::cout << "PRECONFIGURED_TOKEN_BOOTSTRAP_ACCEPTED=OK" << std::endl;
    }
    else {
      std::cout << "TAMPERED_BOOTSTRAP_PROOF_REJECTED=OK" << std::endl;
    }
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "App_CertificateBootstrapTamper error: " << e.what() << std::endl;
    return 1;
  }
}
