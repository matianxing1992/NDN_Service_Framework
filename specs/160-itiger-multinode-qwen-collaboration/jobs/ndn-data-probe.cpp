#include <ndn-cxx/data.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int
main(int argc, char** argv)
{
  if (argc != 4 && argc != 5) {
    std::cerr << "usage: ndn-data-probe producer|consumer NAME PAYLOAD [READY_FILE]\n";
    return 2;
  }
  const std::string mode(argv[1]);
  const ndn::Name name(argv[2]);
  const std::string payload(argv[3]);
  const std::string readyFile = argc == 5 ? argv[4] : "";
  ndn::Face face;

  if (mode == "producer") {
    ndn::KeyChain keyChain;
    ndn::Data data(name);
    data.setFreshnessPeriod(ndn::time::seconds(30));
    data.setContent(payload);
    keyChain.sign(data, ndn::security::signingWithSha256());
    face.put(data);
    face.processEvents(ndn::time::milliseconds(200));
    std::cout << "SPEC160_PROBE_PRELOADED name=" << data.getName()
              << " bytes=" << payload.size() << std::endl;
    if (!readyFile.empty()) {
      std::ofstream marker(readyFile);
      marker << "preloaded\n";
    }
    return 0;
  }

  if (mode == "consumer") {
    bool done = false;
    bool passed = false;
    ndn::Interest interest(name);
    interest.setCanBePrefix(false);
    interest.setMustBeFresh(true);
    interest.setInterestLifetime(ndn::time::seconds(8));
    const auto started = std::chrono::steady_clock::now();
    face.expressInterest(
      interest,
      [&](const auto&, const ndn::Data& data) {
        const auto content = data.getContent();
        const std::string received(
          reinterpret_cast<const char*>(content.value()), content.value_size());
        const auto elapsed = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started).count();
        passed = received == payload;
        done = true;
        std::cout << "SPEC160_PROBE_CONSUMED name=" << data.getName()
                  << " bytes=" << received.size()
                  << " payloadMatch=" << (passed ? "true" : "false")
                  << " rttMs=" << elapsed << std::endl;
      },
      [&](const auto&, const ndn::lp::Nack& nack) {
        done = true;
        std::cerr << "SPEC160_PROBE_NACK reason="
                  << static_cast<int>(nack.getReason()) << std::endl;
      },
      [&](const auto&) {
        done = true;
        std::cerr << "SPEC160_PROBE_TIMEOUT name=" << name << std::endl;
      });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done && std::chrono::steady_clock::now() < deadline) {
      face.processEvents(ndn::time::milliseconds(100));
    }
    return passed ? 0 : 4;
  }

  throw std::invalid_argument("unknown probe mode: " + mode);
}
