/* Historical read acceptance probe.
 *
 * Reads one object published under a generation the Controller has since retired, using only published
 * material: the Controller's signed index and proof, the object's segments and content key, and a
 * historical key request answered by the Controller. Prints the plaintext digest and size so a
 * qualification run can compare it with what the producer wrote.
 *
 * Certificates come from the actors' own PIBs on this host; the probe never trusts a key it did not load
 * from a named identity.
 */
#include "ndn-service-framework/HistoricalReadClient.hpp"
#include "ndn-service-framework/detail/historical-policy-status.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/sha256.hpp>

namespace {

std::string
readFile(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read " + path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::string
field(const std::string& text, const std::string& key, bool required = true)
{
  const auto marker = "\"" + key + "\"";
  const auto at = text.find(marker);
  if (at == std::string::npos) {
    if (required)
      throw std::runtime_error("missing " + key);
    return {};
  }
  const auto colon = text.find(':', at);
  const auto quote = text.find('"', colon);
  const auto end = text.find('"', quote + 1);
  return text.substr(quote + 1, end - quote - 1);
}

ndn::security::Certificate
loadCertificate(const std::string& pib, const std::string& identity)
{
  ndn::KeyChain keyChain("pib-sqlite3:" + pib, "tpm-file:", false);
  return keyChain.getPib().getIdentity(ndn::Name(identity)).getDefaultKey()
    .getDefaultCertificate();
}

const char*
failureName(ndn_service_framework::HistoricalReadFailure failure)
{
  using Failure = ndn_service_framework::HistoricalReadFailure;
  switch (failure) {
    case Failure::INVALID_DATA: return "invalid-data";
    case Failure::ORIGINAL_UNAVAILABLE: return "original-unavailable";
    case Failure::ARCHIVE_UNAVAILABLE: return "archive-proof-unavailable";
    case Failure::CONTROLLER_UNAVAILABLE: return "controller-unavailable";
    case Failure::DEADLINE_EXPIRED: return "deadline-expired";
    case Failure::KEY_TIMEOUT: return "key-timeout";
    case Failure::KEY_REPLY_INVALID: return "key-reply-invalid";
    case Failure::REQUEST_REJECTED: return "request-rejected";
    case Failure::NOT_AUTHORIZED: return "not-authorised";
    case Failure::EVIDENCE_REQUIRED: return "evidence-required";
    case Failure::KEY_UNAVAILABLE: return "key-unavailable";
    case Failure::AUTHORITY_CHANGED: return "authority-changed";
    case Failure::DECRYPTION_FAILED: return "decryption-failed";
    case Failure::NONE: return "none";
  }
  return "unknown";
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc < 2) {
    std::cerr << "usage: ndnsf-historical-read-probe "
                 "[--current|--prime|--without-ck-evidence|--parameters-only] "
                 "<config.json> [more.json ...]\n"
                 "  keys: pib, controller_pib, producer_pib, controller, generation, epoch, object,"
                 " producer, service, ck, payload, publication, policy, identity\n";
    return 2;
  }
  // `--prime` performs only the permission exchange and exits: the grant record has to exist before the rotation
  // retires the version it is written under, and only the caller of the exchange can decide when that is.
  const bool primeOnly = std::string(argv[1]) == "--prime";
  const bool withoutEvidence = std::string(argv[1]) == "--without-ck-evidence";
  const bool parametersOnly = std::string(argv[1]) == "--parameters-only";
  const bool current = std::string(argv[1]) == "--current";
  const int firstConfig = (current || primeOnly || withoutEvidence || parametersOnly) ? 2 : 1;
  if (firstConfig == argc)
    return 2;
  int failures = 0;
  for (int index = firstConfig; index < argc; ++index) {
  try {
    const std::string text = readFile(argv[index]);
    const std::string pib = field(text, "pib");
    ndn::KeyChain keyChain("pib-sqlite3:" + pib, "tpm-file:", false);
    ndn::Face face;

    ndn_service_framework::HistoricalReadRequest request;
    request.controllerPrefix = ndn::Name(field(text, "controller"));
    request.generation = std::strtoull(field(text, "generation").c_str(), nullptr, 10);
    request.epoch = std::strtoull(field(text, "epoch").c_str(), nullptr, 10);
    request.objectName = ndn::Name(field(text, "object"));
    request.producerName = ndn::Name(field(text, "producer"));
    request.serviceName = ndn::Name(field(text, "service"));
    request.ckName = ndn::Name(field(text, "ck"));
    request.payloadName = ndn::Name(field(text, "payload"));
    request.publicationName = ndn::Name(field(text, "publication"));
    request.originalDataForwardingHint =
      ndn::Name(field(text, "original_data_forwarding_hint", false));
    request.policy = field(text, "policy");
    request.producerCertificate =
      loadCertificate(field(text, "producer_pib"), field(text, "producer"));
    const auto controllerCertificate =
      loadCertificate(field(text, "controller_pib"), field(text, "controller"));
    request.recipientCertificate = loadCertificate(pib, field(text, "identity"));
    request.controllerCertificate = controllerCertificate;
    if (current) {
      const auto plaintext = ndn_service_framework::detail::readCurrentNacObject(
        face, keyChain, request, field(text, "trust_schema"));
      if (!plaintext) {
        std::cout << "CURRENT_NAC_READ_FAILED config=" << argv[index] << std::endl;
        ++failures;
        continue;
      }
      ndn::util::Sha256 hash;
      hash << *plaintext;
      std::cout << "CURRENT_NAC_READ_OK generation=" << request.generation
                << " epoch=" << request.epoch << " bytes=" << plaintext->size()
                << " plaintextDigest=sha256:" << hash.toString() << std::endl;
      continue;
    }
    if (primeOnly) {
      const bool primed = ndn_service_framework::primePermissions(
        face, keyChain, request, 10000);
      std::cout << (primed ? "HISTORICAL_PRIME_OK" : "HISTORICAL_PRIME_FAILED")
                << " config=" << argv[index] << std::endl;
      if (!primed)
        ++failures;
      continue;
    }
    if (withoutEvidence) {
      const auto key = ndn_service_framework::fetchHistoricalKey(face, keyChain, request, 5000);
      std::cout << "HISTORICAL_NO_EVIDENCE_KEY_YIELDED=" << (key ? 1 : 0)
                << " config=" << argv[index] << std::endl;
      if (key)
        ++failures;
      continue;
    }

    if (parametersOnly) {
      const auto parameters = ndn_service_framework::fetchHistoricalPublicParameters(
        face, request.controllerPrefix, request.generation, request.epoch, controllerCertificate,
        5000, request.serviceName);
      if (!parameters) {
        std::cout << "HISTORICAL_READ_FAILED stage=parameters config=" << argv[index] << std::endl;
        ++failures;
        continue;
      }
      ndn::util::Sha256 digest;
      digest << *parameters;
      std::cout << "HISTORICAL_PARAMETERS_OK generation=" << request.generation
                << " epoch=" << request.epoch << " digest=sha256:" << digest.toString()
                << std::endl;
      continue;
    }

    ndn_service_framework::HistoricalReadFailure failure;
    const auto plaintext = ndn_service_framework::readHistoricalObject(face, keyChain, request,
                                                                       failure);
    if (!plaintext) {
      std::cout << "HISTORICAL_READ_FAILED stage=object reason=" << failureName(failure)
                << " config=" << argv[index] << std::endl;
      ++failures;
      continue;
    }
    ndn::util::Sha256 hash;
    hash << *plaintext;
    std::cout << "HISTORICAL_READ_OK generation=" << request.generation << " epoch=" << request.epoch
              << " bytes=" << plaintext->size() << " plaintextDigest=sha256:" << hash.toString()
              << " attribute=" << request.policy << " config=" << argv[index] << std::endl;
  }
  catch (const std::exception& error) {
    std::cout << (current ? "CURRENT_NAC_READ_FAILED" : "HISTORICAL_READ_FAILED")
              << " stage=config error=" << error.what() << std::endl;
    ++failures;
  }
  }
  return failures == 0 ? 0 : 1;
}
