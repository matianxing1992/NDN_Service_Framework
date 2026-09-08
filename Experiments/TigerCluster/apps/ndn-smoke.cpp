// A deliberately small ndn-cxx transport probe; no NDNSF, model or GPU claims.
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/scheduler.hpp>
#include <boost/asio/io_context.hpp>
#include <iostream>
#include <functional>
#include <set>
#include <string>

int main(int argc, char** argv)
{
  try {
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout << "ndn-smoke producer|consumer PREFIX TOKEN\n";
      return 0;
    }
    if (argc != 4 || (std::string(argv[1]) != "producer" && std::string(argv[1]) != "consumer"))
      throw std::runtime_error("expected producer|consumer PREFIX TOKEN");
    const std::string mode(argv[1]), token(argv[3]);
    const ndn::Name prefix(argv[2]);
    if (prefix.empty() || token.empty()) throw std::runtime_error("empty probe identity");
    ndn::Face face;
    ndn::KeyChain keys;
    ndn::Scheduler scheduler(face.getIoContext());
    bool success = false;
    unsigned completed = 0;
    auto stop = [&] { face.shutdown(); face.getIoContext().stop(); };
    auto fail = [&](const std::string& reason) {
      success = false;
      std::cerr << "FAIL " << reason << std::endl;
      stop();
    };
    scheduler.schedule(ndn::time::seconds(15), [&] { fail("application deadline"); });
    std::set<uint64_t> served;
    std::function<void()> request;
    if (mode == "producer") {
      face.setInterestFilter(prefix,
        [&](const ndn::InterestFilter&, const ndn::Interest& interest) {
          const auto& name = interest.getName();
          if (name.size() != prefix.size() + 1) return;
          const auto sequence = name[-1].toSequenceNumber();
          if (sequence >= 3) return;
          const std::string content = token + ":" + std::to_string(sequence);
          ndn::Data data(name);
          data.setFreshnessPeriod(ndn::time::milliseconds(0));
          data.setContent(ndn::make_span(reinterpret_cast<const uint8_t*>(content.data()), content.size()));
          keys.sign(data, ndn::security::signingWithSha256());
          face.put(data);
          served.insert(sequence);
          std::cout << "SENT " << name << " " << content << std::endl;
          if (served.size() == 3) {
            success = true;
            scheduler.schedule(ndn::time::milliseconds(250), stop);
          }
        },
        [&](const ndn::Name& name) { std::cout << "READY " << name << std::endl; },
        [&](const ndn::Name&, const std::string& reason) { fail("registration " + reason); });
    }
    else {
      request = [&] {
        ndn::Name name(prefix);
        name.appendSequenceNumber(completed);
        ndn::Interest interest(name);
        interest.setCanBePrefix(false);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::seconds(2));
        face.expressInterest(interest,
          [&](const ndn::Interest& sent, const ndn::Data& data) {
            const auto& block = data.getContent();
            const std::string received(reinterpret_cast<const char*>(block.value()), block.value_size());
            if (data.getName() != sent.getName() || received != token + ":" + std::to_string(completed) ||
                !ndn::security::verifySignature(data, std::nullopt)) {
              fail("Data name/content/digest mismatch");
              return;
            }
            std::cout << "RECEIVED " << data.getName() << " " << received << std::endl;
            if (++completed == 3) { success = true; stop(); }
            else request();
          },
          [&](const ndn::Interest&, const ndn::lp::Nack&) { fail("Nack"); },
          [&](const ndn::Interest&) { fail("Interest timeout"); });
      };
      request();
    }
    face.processEvents();
    std::cout << (success ? "PASS " : "FAIL ") << mode
              << " count=" << (mode == "producer" ? served.size() : completed) << std::endl;
    return success ? 0 : 1;
  }
  catch (const std::exception& e) {
    std::cerr << "FAIL exception: " << e.what() << std::endl;
    return 2;
  }
}
