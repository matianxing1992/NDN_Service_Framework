#include "../integration-tests/isolated-test-trust.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "ndn-service-framework/utils.hpp"
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>

namespace ndn_service_framework::test {
namespace {
class CollaborationUser : public LocalServiceUser
{
public:
  using LocalServiceUser::LocalServiceUser;
  using ServiceUser::OnCollaborationData;

  void seed(const ndn::Name& request, const std::string& scope, const ndn::Buffer& key)
  {
    std::lock_guard<std::mutex> lock(m_verifiedCollaborationMutex);
    m_userCollaborationScopeKeys[request][scope] = key;
  }
};

class CollaborationProvider : public LocalServiceProvider
{
public:
  using LocalServiceProvider::LocalServiceProvider;
  using ServiceProvider::publishCollaborationData;

  void seed(const ndn::Name& request, const std::string& scope, const ndn::Buffer& key)
  {
    std::lock_guard<std::mutex> lock(m_collaborationMutex);
    m_collaborationScopeKeysByRequest[request][scope] = key;
  }
};
} // namespace

BOOST_AUTO_TEST_SUITE(Spec201CollaborationCrypto)
BOOST_AUTO_TEST_CASE(ActualProviderPublicationAndUserDecryptionKeepCanonicalKeyBinding)
{
  ndn::KeyChain keys("pib-memory:spec201-collab-crypto", "tpm-memory:spec201-collab-crypto");
  ndn::DummyClientFace face(keys);
  const ndn::Name producer("/org/NDNSF/COLLAB/provider/p0");
  const ndn::Name requester("/org/user/u0");
  const ndn::Name request("/session/request/7");
  const ndn::Name topic("/hidden-state/output");
  const ndn::Name group("/org/group");
  const auto providerCert = makeRsaIdentity(keys, producer);
  const auto userCert = makeRsaIdentity(keys, requester);
  const auto authorityCert = makeRsaIdentity(keys, ndn::Name("/org/controller"));
  const auto node = ndn::Name(producer).append("provider").append("17");
  ndn_service_framework::test::IsolatedTestTrust fixedTrust1{
    providerCert, userCert, authorityCert};
  CollaborationProvider provider(face, group, providerCert, authorityCert, fixedTrust1.schemaPath());
  ndn_service_framework::test::IsolatedTestTrust fixedTrust2{
    providerCert, userCert, authorityCert};
  CollaborationUser user(face, group, userCert, authorityCert, fixedTrust2.schemaPath());
  ndn::svs::SecurityOptions security(keys);
  security.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
  security.dataSigner->signingInfo = ndn::security::signingByCertificate(providerCert);
  security.pubSigner->signingInfo = ndn::security::signingByCertificate(providerCert);
  security.validator = std::make_shared<ndn::svs::BaseValidator>();
  security.encapsulatedDataValidator = std::make_shared<ndn::svs::BaseValidator>();
  ndn::svs::SVSPubSubOptions options;
  options.useTimestamp = false;
  provider.attachLocalMockPubSubForTest(std::make_shared<ndn::svs::SVSPubSub>(
    group, node, face, [](const std::vector<ndn::svs::MissingDataInfo>&) {}, options, security));
  const std::string scope = "activation-scope";
  const ndn::Buffer key(32, 7);
  const ndn::Buffer plaintext{1, 2, 3, 4};
  provider.seed(request, scope, key);
  user.seed(request, scope, key);
  std::optional<ndn::Data> packet;
  auto capture = face.onSendData.connect([&](const ndn::Data& outer) {
    if (outer.getContentType() != ndn::tlv::Data || !node.isPrefixOf(outer.getName())) return;
    BOOST_REQUIRE(ndn::security::verifySignature(outer, keys.getPib().getIdentity(producer).getDefaultKey()));
    BOOST_CHECK_LE(outer.wireEncode().size(), 8800);
    ndn::Data inner(outer.getContent().blockFromValue());
    if (parseCollaborationDataName(inner.getName(), producer)) packet = std::move(inner);
  });
  provider.publishCollaborationData(requester, request, "stage0", scope, topic, plaintext);
  face.processEvents(ndn::time::milliseconds(20));
  face.getIoContext().restart();
  BOOST_REQUIRE(packet);
  const auto name = packet->getName();
  BOOST_CHECK(producer.isPrefixOf(name));
  BOOST_CHECK_LE(name.wireEncode().size(), 1024);
  BOOST_CHECK_LE(packet->wireEncode().size(), 8800);
  BOOST_CHECK_EQUAL(packet->getSignatureType(), ndn::tlv::SignatureSha256WithRsa);
  BOOST_REQUIRE(ndn::security::verifySignature(*packet, keys.getPib().getIdentity(producer).getDefaultKey()));
  CollaborationDataMessage message;
  BOOST_REQUIRE(message.WireDecode(packet->getContent().blockFromValue()));
  HybridMessageEnvelope envelope;
  BOOST_REQUIRE(envelope.WireDecode(ndn::Block(message.getPayload())));
  BOOST_CHECK_EQUAL(envelope.getKeyId(), hybridCompactKeyId(
    "collab|" + request.toUri(ndn::name::UriFormat::CANONICAL) + "|" + scope));
  const auto receive = [&](const ndn::Name& advertisedName, const ndn::Name& advertisedNode,
                           const std::optional<ndn::Data>& original) {
    const auto& content = original->getContent();
    user.OnCollaborationData({advertisedName,
      ndn::span<const uint8_t>(content.value(), content.value_size()), advertisedNode, 1, original});
    return user.waitForVerifiedCollaborationData(request, scope, topic, 1, 5, true);
  };
  auto records = receive(name, node, packet);
  BOOST_REQUIRE_EQUAL(records.size(), 1);
  BOOST_CHECK(records.front().payload == plaintext);
  BOOST_CHECK_EQUAL(records.front().producer, producer);
  BOOST_CHECK_EQUAL(records.front().requestId, request);
  BOOST_CHECK_EQUAL(records.front().dataName, name);
  BOOST_CHECK(receive(ndn::Name(name).append("extra"), node, packet).empty());
  BOOST_CHECK(receive(name, ndn::Name("/org/evil/provider/17"), packet).empty());
  auto wrongSigner = packet;
  keys.sign(*wrongSigner, ndn::security::signingByCertificate(userCert));
  BOOST_CHECK(receive(name, node, wrongSigner).empty());
  auto changed = packet;
  message.setKeyScope("different-scope");
  changed->setContent(message.WireEncode());
  keys.sign(*changed, ndn::security::signingByCertificate(providerCert));
  BOOST_CHECK(receive(name, node, changed).empty());
  BOOST_REQUIRE_EQUAL(receive(name, node, packet).size(), 1);
}
BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
