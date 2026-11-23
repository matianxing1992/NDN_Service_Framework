// An installed acceptance-only actor. Both attempts enter the ordinary Request/ACK path.
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/io.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using namespace ndn_service_framework;
using Clock = std::chrono::steady_clock;

template<typename Predicate>
void
waitFor(ndn::Face& face, Predicate ready, std::chrono::seconds budget, const char* reason)
{
  const auto end = Clock::now() + budget;
  while (!ready()) {
    if (Clock::now() >= end)
      throw std::runtime_error(reason);
    face.processEvents(ndn::time::milliseconds(20));
  }
}

struct Reply
{
  bool done = false;
  bool success = false;
  std::string payload;
};

// This copy comes from the live User's normal opt-in store, written only after admission.
// Read the deadline from the originally signed Data independently of the expiry timer.
void
captureLease(const ndn::Name& identity, const ndn::Name& service, const ControllerVersion& old,
             const ndn::security::Certificate& controller, const std::string& path)
{
  RuntimeStatusStore store(RuntimeStatusStore::defaultStorePath("user", identity));
  std::vector<RuntimeStatusStore::Record> rows;
  if (store.load(rows)) {
    for (const auto& row : rows) {
      if (row.serviceName != service || row.controllerVersion != old)
        continue;
      const ndn::Data data(ndn::Block(ndn::span<const uint8_t>(row.statusDataWire)));
      PolicyStatusData status;
      if (!ndn::security::verifySignature(data, controller) ||
          !controller.getIdentity().isPrefixOf(data.getName()) ||
          !status.wireDecode(data.getContent().blockFromValue()) ||
          status.getControllerCertificate() != controller.getName() ||
          status.getServiceName() != service || status.getControllerVersion() != old)
        throw std::runtime_error("signed lease identity or content mismatch");
      ndn::io::save(data, path, ndn::io::NO_ENCODING);
      std::cout << "LEASE_SIGNED version=" << old.toString()
                << " fromMs=" << status.getValidFromMs() << " expiryMs=" << status.getValidUntilMs()
                << std::endl;
      return;
    }
  }
  throw std::runtime_error("missing accepted signed lease for target");
}

void
invoke(ServiceUser& user, ndn::Face& face, const ndn::Name& provider, const ndn::Name& service,
       const std::string& phase, bool expectRefusal)
{
  auto reply = std::make_shared<Reply>();
  const auto version = user.getControllerVersion(service);
  if (!version)
    throw std::runtime_error("no installed service version");
  const auto wall = std::chrono::system_clock::now().time_since_epoch();
  std::cout << "LEASE_ATTEMPT phase=" << phase << " version=" << version->toString()
            << " atMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(wall).count()
            << std::endl;
  RequestMessage request;
  ndn::Buffer empty;
  request.setPayload(empty, 0);
  const auto id = user.RequestService({provider}, service, request, 6000,
    [reply] (const ndn::Name&) { reply->done = true; },
    [reply] (const ResponseMessage& data) {
      reply->success = data.getStatus();
      const auto bytes = data.getPayload();
      reply->payload.assign(bytes.begin(), bytes.end());
      reply->done = true;
    }, tlv::FirstResponding);
  if (expectRefusal) {
    if (!id.empty())
      throw std::runtime_error("expired request admitted by local User");
    // The driver also requires the native gate's exact rejection reason on this call.
    std::cout << "LEASE_REFUSED phase=" << phase << std::endl;
    return;
  }
  if (id.empty())
    throw std::runtime_error("expected live request was locally refused");
  waitFor(face, [reply] { return reply->done; }, std::chrono::seconds(8), "no Repo response");
  if (!reply->success)
    throw std::runtime_error("Repo operation timed out or failed");
  std::cout << "LEASE_RESPONSE phase=" << phase << " id=" << id << " payload="
            << reply->payload << std::endl;
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    if (argc != 9)
      throw std::invalid_argument("usage: IDENTITY GROUP PROVIDER SERVICE CONTROLLER-CERT "
                                  "TRUST-SCHEMA RELEASE-FILE isolated|control");
    const std::string mode = argv[8];
    if (mode != "isolated" && mode != "control")
      throw std::invalid_argument("expected isolated or control");
    const ndn::Name identity(argv[1]), group(argv[2]), provider(argv[3]), service(argv[4]);
    auto controller = ndn::io::load<ndn::security::Certificate>(argv[5]);
    if (!controller)
      throw std::runtime_error("invalid Controller certificate");
    ndn::KeyChain keys;
    const auto cert = keys.getPib().getIdentity(identity).getDefaultKey().getDefaultCertificate();
    ndn::Face face;
    ServiceUser user(face, group, cert, *controller, argv[6]);
    user.init();
    user.fetchPermissionsFromController(controller->getIdentity());
    const auto ready = [&] { return user.isRequestBootstrapReady(service, {provider}); };
    waitFor(face, ready, std::chrono::seconds(35), "initial bootstrap timed out");
    invoke(user, face, provider, service, "before", false);
    const auto old = user.getControllerVersion(service);
    if (!old)
      throw std::runtime_error("initial leased version is absent");
    std::cout << "LEASE_READY version=" << old->toString() << std::endl;
    if (mode == "isolated") {
      const std::string cut = std::string(argv[7]) + ".cut";
      waitFor(face, [&] { return std::filesystem::exists(cut); }, std::chrono::seconds(30),
              "missing target isolation");
      captureLease(identity, service, *old, *controller, cut + ".status.ndn");
    }
    waitFor(face, [&] { return std::filesystem::exists(argv[7]); }, std::chrono::seconds(45),
            "post-partition observation deadline");
    if (mode == "control") {
      waitFor(face, [&] {
        const auto version = user.getControllerVersion(service);
        return version && version->compare(*old) > 0 && ready();
      }, std::chrono::seconds(12), "control never installed live advanced authority");
    }
    invoke(user, face, provider, service, "after", mode == "isolated");
    if (mode == "isolated")
      face.processEvents(ndn::time::milliseconds(50));
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "LEASE_FAILED " << error.what() << std::endl;
    return 1;
  }
}
