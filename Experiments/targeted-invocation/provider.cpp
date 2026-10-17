#include "common.hpp"
#include <iostream>
#include <thread>

int main(int argc, char** argv) try {
  using namespace sim203;
  Options options(argc, argv);
  const ndn::Name identity(options.get("--identity", "/example/hello/provider/A"));
  const ndn::Name controller(options.get("--controller", "/example/hello/controller"));
  const ndn::Name service(options.get("--service", "/HELLO"));
  const int delay = options.number("--delay-ms", 5);
  Records records(options.get("--records"));
  ndn::Face face; ndn::KeyChain keys;
  ndn::Scheduler scheduler(face.getIoContext());
  const auto cert = certificate(keys, identity);
  keys.setDefaultIdentity(keys.getPib().getIdentity(identity));
  nsf::CertificatePublisher publisher(face, keys, identity);
  const ndn::Name probePrefix(ndn::Name(identity).append("NDNSF").append("experiment").append("rtt"));
  const auto probeRegistration = face.setInterestFilter(probePrefix,
    [&](const ndn::InterestFilter&, const ndn::Interest& interest) {
      ndn::Data data(interest.getName());
      data.setFreshnessPeriod(ndn::time::milliseconds(0));
      const std::string content(64, 'r');
      data.setContent(std::string_view(content));
      keys.sign(data);
      face.put(data);
      records.write({{"event", "rtt_probe"}, {"provider", identity.toUri()},
                     {"name", interest.getName().toUri()}, {"mono_ns", nowNs()}});
    }, [&](const ndn::Name&, const std::string& error) {
      throw std::runtime_error("probe prefix registration: " + error);
    });
  nsf::ServiceProvider provider(face, ndn::Name(options.get("--group", "/example/hello/group")), cert,
                                certificate(keys, controller), options.get("--trust"));
  provider.setUseTokens(true);
  provider.setTimelineTrace(options.number("--timeline-trace", 1) != 0);
  const auto registration = provider.addScopedService(service,
    nsf::ServiceProvider::AckStrategyHandler([&](const nsf::RequestMessage& request) {
      records.write({{"event", "ack_handler"}, {"provider", identity.toUri()},
                     {"command", stringPayload(request.getPayload())}, {"mono_ns", nowNs()}});
      nsf::ServiceProvider::AckDecision decision; decision.status = true;
      decision.message = "simulated service ready"; return decision;
    }),
    nsf::ServiceProvider::RequestHandler([&](const ndn::Name&, const ndn::Name&,
        const ndn::Name&, const ndn::Name& requestId, const nsf::RequestMessage& request) {
      const auto begin = nowNs();
      auto payload = request.getPayload();
      const bool valid = payload.size() == 64;
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
      records.write({{"event", "execution"}, {"provider", identity.toUri()},
                     {"request_id", requestId.toUri()}, {"command", stringPayload(payload)},
                     {"actual_mode", request.getRequestMode()}, {"begin_ns", begin},
                     {"end_ns", nowNs()}, {"payload_valid", valid}});
      nsf::ResponseMessage response; response.setStatus(valid);
      response.setErrorInfo(valid ? "" : "expected 64-byte command");
      response.setPayload(payload, payload.size()); return response;
    }), nsf::ServiceProvider::ServiceInvocationMode::NormalAndTargeted);
  provider.init();
  provider.fetchPermissionsFromController(controller);
  bool ready = false;
  const auto started = Clock::now();
  std::function<void()> check;
  check = [&] {
    if (provider.hasProviderPermissionForService(service) && provider.isRequestDecryptionReady()) {
      ready = true;
      records.write({{"event", "ready"}, {"provider", identity.toUri()}, {"mono_ns", nowNs()}});
      std::cout << "SPEC203_PROVIDER_READY " << identity.toUri() << std::endl;
    } else if (Clock::now() - started > std::chrono::seconds(45)) {
      records.write({{"event", "readiness_timeout"}}); shutdownFace(face);
    } else scheduler.schedule(ndn::time::milliseconds(100), check);
  };
  scheduler.schedule(ndn::time::milliseconds(100), check);
  boost::asio::signal_set signals(face.getIoContext(), SIGINT, SIGTERM);
  signals.async_wait([&](const auto&, int) { shutdownFace(face); });
  face.processEvents();
  records.write({{"event", "shutdown"}, {"ready", ready}});
  return ready ? 0 : 2;
} catch (const std::exception& error) {
  std::cerr << "SPEC203_PROVIDER_ERROR " << error.what() << std::endl; return 2;
}
