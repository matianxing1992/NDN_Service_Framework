/* Publishes one small object under the Controller's current ABE generation.
 *
 * Used by the historical-read acceptance: publish under generation G, rotate the Controller to G+1, then
 * read the object with `ndnsf-historical-read-probe`. The object is served by this process for as long as
 * it runs, so the read exercises the real fetch/verify/decrypt path rather than a local file.
 */
#include "ndn-service-framework/AbeHistoricalKey.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndn-service-framework/utils.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <nac-abe/algo/abe-support.hpp>

namespace {

std::string
readFile(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc != 2) {
    std::cerr << "usage: ndnsf-historical-fixture <config.json>\n"
                 "  keys: pib, identity, controller, service, attribute, payload\n";
    return 2;
  }
  try {
    const std::string text = readFile(argv[1]);
    auto field = [&text] (const std::string& key) {
      const auto at = text.find("\"" + key + "\"");
      if (at == std::string::npos)
        throw std::runtime_error("missing " + key);
      const auto quote = text.find('"', text.find(':', at));
      return text.substr(quote + 1, text.find('"', quote + 1) - quote - 1);
    };
    const std::string pib = field("pib");
    const ndn::Name identity(field("identity"));
    const ndn::Name controller(field("controller"));
    const ndn::Name service(field("service"));
    const std::string attribute = field("attribute");
    const std::string payload = field("payload");

    ndn::KeyChain keyChain("pib-sqlite3:" + pib, "tpm-file:", false);
    const auto certificate = keyChain.getPib().getIdentity(identity).getDefaultKey()
      .getDefaultCertificate();
    ndn::Face face;

    // The current generation's public parameters, published by the Controller under its own name.
    std::string paramsWire;
    ndn::Name paramsDataName;
    uint64_t paramsVersion = 0;
    // The Controller may still be in readiness when this starts, so keep asking for a bounded while.
    for (int attempt = 0; attempt < 30 && paramsWire.empty(); ++attempt) {
      ndn::Name paramsName(controller);
      paramsName.append("PUBPARAMS").append("KP-ABE");
      ndn::Interest interest(paramsName);
      interest.setCanBePrefix(true);
      interest.setMustBeFresh(true);
      interest.setInterestLifetime(ndn::time::milliseconds(4000));
      bool done = false;
      face.expressInterest(interest, [&] (const ndn::Interest&, const ndn::Data& data) {
        paramsWire.assign(reinterpret_cast<const char*>(data.getContent().value()),
                          data.getContent().value_size());
        // The generation is the version component of the PUBPARAMS name: generation*1000 + epoch.
        paramsDataName = data.getName();
        if (!data.getName().empty()) {
          auto versionText = data.getName().get(-1).toUri();
          if (versionText.rfind("v=", 0) == 0)
            versionText = versionText.substr(2);
          paramsVersion = std::strtoull(versionText.c_str(), nullptr, 10);
        }
        done = true;
      }, [&] (const ndn::Interest&, const ndn::lp::Nack&) { done = true; },
         [&] (const ndn::Interest&) { done = true; });
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!done && std::chrono::steady_clock::now() < until)
        face.processEvents(ndn::time::milliseconds(50));
      if (paramsWire.empty())
        face.processEvents(ndn::time::milliseconds(1000));
    }
    if (paramsWire.empty()) {
      std::cout << "FIXTURE_FAILED stage=params" << std::endl;
      return 1;
    }

    auto& abe = ndn::nacabe::algo::ABESupport::getInstance();
    ndn::nacabe::algo::PublicParams params;
    params.fromBuffer(ndn::Buffer(reinterpret_cast<const uint8_t*>(paramsWire.data()),
                                  paramsWire.size()));
    const auto contentKey = abe.kpContentKeyGen(params, {attribute});
    const auto cipher = abe.encrypt(contentKey,
      ndn::Buffer(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));

    const auto ckName = ndn_service_framework::makeHybridMessageKeyDataName(service, identity,
                                                                            attribute, "fixture-1");
    const ndn::Name payloadRoot = [&identity] {
      ndn::Name root = ndn_service_framework::makeProducerDataPrefix(identity, "OBJECT");
      root.append("fixture-1");
      return root;
    }();
    // The object is one packet under segment zero, while the reader is told the object's root: it appends
    // the segment component itself.
    ndn::Name payloadName(payloadRoot);
    payloadName.appendSegment(0);

    // The envelopes the reader parses: CK as outer 0x15 with 0x0259, payload with 0x025A/0x025B.
    ndn::Data ck(ckName);
    {
      ndn::Block outer(0x15);
      outer.push_back(ndn::encoding::makeBinaryBlock(0x0259,
        ndn::span<const uint8_t>(contentKey->m_encAesKey.data(), contentKey->m_encAesKey.size())));
      outer.encode();
      ck.setContent(ndn::Buffer(outer.begin(), outer.end()));
      ck.setFreshnessPeriod(ndn::time::milliseconds(0));
      keyChain.sign(ck, ndn::security::signingByCertificate(certificate));
    }
    ndn::Data object(payloadName);
    {
      const uint8_t sizeByte = static_cast<uint8_t>(cipher.m_plainTextSize);
      ndn::Block outer(0x15);
      outer.push_back(ndn::encoding::makeBinaryBlock(0x025A,
        ndn::span<const uint8_t>(cipher.m_content.data(), cipher.m_content.size())));
      outer.push_back(ndn::encoding::makeBinaryBlock(0x025B,
        ndn::span<const uint8_t>(&sizeByte, 1)));
      outer.encode();
      object.setContent(ndn::Buffer(outer.begin(), outer.end()));
      object.setFreshnessPeriod(ndn::time::milliseconds(0));
      keyChain.sign(object, ndn::security::signingByCertificate(certificate));
    }

    // Only the two exact names are served: the fixture is a producer for one object, not a namespace.
    // A failed registration must be visible, or the fixture looks alive while serving nothing.
    const auto onFailure = [] (const ndn::Name& name, const std::string& reason) {
      std::cout << "FIXTURE_REGISTER_FAILED name=" << name.toUri() << " reason=" << reason
                << std::endl << std::flush;
    };
    face.setInterestFilter(ck.getName(),
      [&face, ck] (const ndn::InterestFilter&, const ndn::Interest&) { face.put(ck); }, onFailure);
    face.setInterestFilter(payloadName,
      [&face, object] (const ndn::InterestFilter&, const ndn::Interest&) { face.put(object); },
      onFailure);

    ndn::util::Sha256 hash;
    hash << payload;
    std::cout << "FIXTURE_READY ck=" << ck.getName().toUri()
              << " payload=" << payloadRoot.toUri()
              << " service=" << service.toUri()
              << " attribute=" << attribute
              << " producer=" << identity.toUri()
              << " paramsName=" << paramsDataName.toUri()
              << " generation=" << paramsVersion / 1000
              << " epoch=" << paramsVersion % 1000
              << " plaintextDigest=sha256:" << hash.toString()
              << " bytes=" << payload.size() << std::endl
              << std::flush;
    face.processEvents();
    return 0;
  }
  catch (const std::exception& error) {
    std::cout << "FIXTURE_FAILED stage=config error=" << error.what() << std::endl;
    return 1;
  }
}
