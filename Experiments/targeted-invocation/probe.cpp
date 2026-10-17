#include "common.hpp"
#include <ndn-cxx/security/validator-config.hpp>
#include <iostream>

int main(int argc, char** argv) try {
  using namespace sim203;
  Options options(argc, argv);
  Records records(options.get("--records"));
  ndn::Face face;
  ndn::ValidatorConfig validator(face);
  validator.load(options.get("--trust"));
  const ndn::Name target(options.get("--target", "/example/hello/provider/A"));
  const ndn::Name prefix(ndn::Name(target).append("NDNSF").append("experiment").append("rtt"));
  uint64_t sequence = 0, successes = 0, invalid = 0;
  std::function<void()> next;
  next = [&] {
    if (sequence == 10) { shutdownFace(face); return; }
    ++sequence;
    const auto begin = nowNs();
    ndn::Name name(prefix); name.append(std::to_string(begin)); name.appendNumber(sequence);
    ndn::Interest interest(name);
    interest.setMustBeFresh(true); interest.setCanBePrefix(false);
    interest.setInterestLifetime(ndn::time::seconds(5));
    face.expressInterest(interest,
      [&, begin, name](const ndn::Interest&, const ndn::Data& data) {
        validator.validate(data,
          [&, begin, name](const ndn::Data& verified) {
            const bool identity = verified.getSignatureInfo().hasKeyLocator() &&
              verified.getSignatureInfo().getKeyLocator().getType() == ndn::tlv::Name &&
              target.isPrefixOf(verified.getSignatureInfo().getKeyLocator().getName());
            const bool valid = identity && verified.getName() == name && verified.getContent().value_size() == 64;
            if (valid) ++successes;
            else ++invalid;
            records.write({{"event", "rtt_terminal"}, {"name", name.toUri()},
                           {"target", target.toUri()}, {"success", valid}, {"invalid", !valid},
                           {"begin_ns", begin}, {"terminal_ns", nowNs()}});
            next();
          }, [&, begin, name](const ndn::Data&, const ndn::security::ValidationError& error) {
            const bool bad = error.getCode() != ndn::security::ValidationError::CANNOT_RETRIEVE_CERT;
            if (bad) ++invalid;
            records.write({{"event", "rtt_error"}, {"name", name.toUri()}, {"begin_ns", begin},
                           {"terminal_ns", nowNs()}, {"invalid", bad}, {"error", error.getInfo()}});
            next();
          });
      }, [&, begin, name](const ndn::Interest&, const ndn::lp::Nack&) {
        records.write({{"event", "rtt_error"}, {"name", name.toUri()}, {"begin_ns", begin},
                       {"terminal_ns", nowNs()}, {"invalid", false}, {"error", "nack"}}); next();
      }, [&, begin, name](const ndn::Interest&) {
        records.write({{"event", "rtt_error"}, {"name", name.toUri()}, {"begin_ns", begin},
                       {"terminal_ns", nowNs()}, {"invalid", false}, {"error", "timeout"}}); next();
      });
  };
  next(); face.processEvents();
  return sequence == 10 && successes > 0 && invalid == 0 ? 0 : 2;
} catch (const std::exception& error) {
  std::cerr << "SPEC203_PROBE_ERROR " << error.what() << std::endl; return 2;
}
