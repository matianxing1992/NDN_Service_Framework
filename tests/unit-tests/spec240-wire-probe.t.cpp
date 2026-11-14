// Spec 240: a genuine Interest/Data round trip between two ServiceUsers over a
// forwarder, used to check that the project can host wire-level facade evidence.
#include "tests/boost-test.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <ndn-cxx/face.hpp>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace ndn_service_framework::test {

namespace {

const ndn::Name SERVICE("/Spec240Wire/probe");

std::string
trustSchemaPath()
{
  if (const char* configured = std::getenv("NDNSF_TRUST_SCHEMA"))
    return configured;
  // __FILE__ points into the source tree, so the examples fixture is找到 relative to it.
  std::string source(__FILE__);
  const auto marker = source.find("/tests/unit-tests/");
  if (marker == std::string::npos)
    return "examples/trust-any.conf";
  return source.substr(0, marker) + "/examples/trust-any.conf";
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec240WireProbe)

BOOST_AUTO_TEST_CASE(RequestTravelsThroughTheForwarder)
{
  // A real wire exchange needs both a forwarder and a trust schema anchored at a
  // concrete certificate: the framework rejects `type any` outside mock mode, so this
  // probe only runs when the caller supplies a real schema.
  const char* socket = std::getenv("NDNSF_WIRE_NFD_SOCKET");
  const char* trustSchema = std::getenv("NDNSF_WIRE_TRUST_SCHEMA");
  if (socket == nullptr || *socket == '\0' || trustSchema == nullptr ||
      *trustSchema == '\0')
    BOOST_TEST_MESSAGE("wire probe skipped: set NDNSF_WIRE_NFD_SOCKET and "
                       "NDNSF_WIRE_TRUST_SCHEMA to run it");
  else {
    ndn::Face userFace(socket);
    ndn::Face providerFace(socket);
    ndn::security::KeyChain keyChain("pib-memory:spec240-wire",
                                     "tpm-memory:spec240-wire");
    const ndn::Name requesterName("/test/user/spec240-wire");
    const ndn::Name providerName("/test/provider/spec240-wire");
    auto userCert = makeRsaIdentity(keyChain, requesterName);
    auto providerCert = makeRsaIdentity(keyChain, providerName);
    auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa/spec240-wire"));

    LocalServiceUser user(userFace, ndn::Name("/test/group"), userCert, aaCert,
                          trustSchema);
    ServiceProvider provider(ServiceProvider::LocalMockTag{}, providerFace,
                             ndn::Name("/test/group"), providerCert, aaCert,
                             trustSchema);
    installPermissions(user, provider, requesterName, SERVICE);

    bool called = false;
    provider.addHandler<DynamicRequest, DynamicResponse>(
      SERVICE,
      std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
        [&called] (const ndn::Name&, const DynamicRequest&, DynamicResponse& response) {
          called = true;
          response.setClassification(7);
        }));

    bool responded = false;
    user.RequestService(
      std::vector<ndn::Name>{providerName}, SERVICE, makeRequestMessageWithUserToken("x"),
      5000,
      [] (const ndn::Name&) {},
      [&responded] (const ndn_service_framework::ResponseMessage& response) {
        if (response.getStatus())
          responded = true;
      });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!responded && std::chrono::steady_clock::now() < deadline) {
      userFace.processEvents(ndn::time::milliseconds(20));
      providerFace.processEvents(ndn::time::milliseconds(20));
    }
    BOOST_CHECK_MESSAGE(called, "provider handler ran over the wire");
    BOOST_CHECK_MESSAGE(responded, "response arrived over the wire");
  }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
