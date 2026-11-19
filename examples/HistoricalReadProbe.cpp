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
field(const std::string& text, const std::string& key)
{
  const auto marker = "\"" + key + "\"";
  const auto at = text.find(marker);
  if (at == std::string::npos)
    throw std::runtime_error("missing " + key);
  const auto colon = text.find(':', at);
  const auto quote = text.find('"', colon);
  const auto end = text.find('"', quote + 1);
  return text.substr(quote + 1, end - quote - 1);
}

ndn::security::Certificate
loadCertificate(const std::string& pib, const std::string& identity)
{
  ndn::KeyChain keyChain("pib-sqlite3:" + pib, "tpm-file:" + pib, false);
  return keyChain.getPib().getIdentity(ndn::Name(identity)).getDefaultKey()
    .getDefaultCertificate();
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc != 2) {
    std::cerr << "usage: ndnsf-historical-read-probe <config.json>\n"
                 "  keys: pib, controller_pib, producer_pib, controller, generation, epoch, object,"
                 " producer, service, ck, payload, policy, identity\n";
    return 2;
  }
  try {
    const std::string text = readFile(argv[1]);
    const std::string pib = field(text, "pib");
    ndn::KeyChain keyChain("pib-sqlite3:" + pib, "tpm-file:" + pib, false);
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
    request.policy = field(text, "policy");
    request.producerCertificate =
      loadCertificate(field(text, "producer_pib"), field(text, "producer"));
    const auto controllerCertificate =
      loadCertificate(field(text, "controller_pib"), field(text, "controller"));
    request.recipientCertificate = loadCertificate(pib, field(text, "identity"));

    const auto parameters = ndn_service_framework::fetchHistoricalPublicParameters(
      face, request.controllerPrefix, request.generation, request.epoch, controllerCertificate, 5000);
    if (!parameters) {
      std::cout << "HISTORICAL_READ_FAILED stage=parameters" << std::endl;
      return 1;
    }
    request.publicParameters = *parameters;

    const auto plaintext = ndn_service_framework::readHistoricalObject(face, keyChain, request);
    if (!plaintext) {
      std::cout << "HISTORICAL_READ_FAILED stage=object" << std::endl;
      return 1;
    }
    ndn::util::Sha256 hash;
    hash << *plaintext;
    std::cout << "HISTORICAL_READ_OK generation=" << request.generation << " epoch=" << request.epoch
              << " bytes=" << plaintext->size() << " plaintextDigest=sha256:" << hash.toString()
              << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cout << "HISTORICAL_READ_FAILED stage=config error=" << error.what() << std::endl;
    return 1;
  }
}
