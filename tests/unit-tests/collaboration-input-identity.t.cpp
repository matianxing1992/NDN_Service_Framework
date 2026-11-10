#include "tests/boost-test.hpp"
#include "ndn-service-framework/CollaborationDataflow.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"
#include "ndn-service-framework/collaboration-input.hpp"
#include "tests/integration-tests/ndnsf-integration-fixture.hpp"
#include "tests/integration-tests/isolated-test-trust.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "tests/unit-tests/noncanonical-tlv-fixture.hpp"
#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <type_traits>

namespace ndn_service_framework::test {
namespace {
class CapabilityUser : public LocalServiceUser
{
public:
  using LocalServiceUser::LocalServiceUser;
  void addAck(const ndn::Name& request, const ndn::Name& provider, bool capabilities)
  {
    addDeferredAckForTest(request, provider, "worker");
    if (capabilities) {
      ProviderCapabilityOffer offer;
      offer.setField("collaborationInputProfiles", "data-admission-v1");
      offer.setField("dependencyRecoveryProfiles", "arq-v1,xor-packet-set-v1");
      m_pendingCalls.at(request).requestAcks.back().message.setProviderCapabilityOffer(offer);
    }
  }
};

class IdentitySelector : public ParticipantSelectionPolicy
{
public:
  bool forgeCapability = false;
  std::vector<SelectedParticipant> select(const std::vector<AckCandidate>& candidates,
      const std::vector<CollaborationRoleSpec>& roles) const override
  {
    std::vector<SelectedParticipant> result;
    for (auto candidate : candidates) {
      if (forgeCapability) {
        ProviderCapabilityOffer offer;
        offer.setField("collaborationInputProfiles", "data-admission-v1");
        offer.setField("dependencyRecoveryProfiles", "arq-v1,xor-packet-set-v1");
        candidate.ack.setProviderCapabilityOffer(offer);
      }
      result.push_back({roles.front().role, candidate.serviceName, candidate.providerName,
        {}, false, 0, ndn::Buffer{'x'}, candidate, {}});
    }
    return result;
  }
};
}
static_assert(!std::is_default_constructible_v<VerifiedInputObject>);
static_assert(!std::is_constructible_v<VerifiedInputObject, ndn::Name, ndn::Name,
  std::shared_ptr<const std::vector<std::shared_ptr<const ndn::Data>>>,
  std::string, std::function<bool()>>);
BOOST_AUTO_TEST_SUITE(Spec226InputIdentity)

BOOST_AUTO_TEST_CASE(DefaultTransportHasNoRedundancy)
{
  DependencyTransportOptions options;
  BOOST_CHECK(options.redundancy == DependencyRedundancy::None);
}

BOOST_AUTO_TEST_CASE(MultipleProvidersSurviveProtectedSelectionMetadata)
{
  CollaborationDataflowMetadataV1 metadata;
  metadata.roleBindings["sensor"] = {ndn::Name("/site/a/sensor"),
                                     ndn::Name("/site/b/sensor")};
  ServiceSelectionMessage selection;
  selection.setCollaborationDataflowMetadata(metadata);
  ServiceSelectionMessage parsed;
  BOOST_REQUIRE(parsed.WireDecode(selection.WireEncode()));
  BOOST_REQUIRE(parsed.hasCollaborationDataflowMetadata());
  const auto& providers = parsed.getCollaborationDataflowMetadata().roleBindings.at("sensor");
  BOOST_REQUIRE_EQUAL(providers.size(), 2);
  BOOST_CHECK_EQUAL(providers[0], ndn::Name("/site/a/sensor"));
  BOOST_CHECK_EQUAL(providers[1], ndn::Name("/site/b/sensor"));
  ServiceSelectionMessage copy(parsed);
  parsed.Clear();
  BOOST_CHECK(!parsed.hasCollaborationDataflowMetadata());
  BOOST_CHECK(copy.hasCollaborationDataflowMetadata());
}

BOOST_AUTO_TEST_CASE(RejectDuplicateBindingAndUnsupportedProfile)
{
  CollaborationDataflowMetadataV1 metadata;
  metadata.roleBindings["sensor"] = {ndn::Name("/site/a"), ndn::Name("/site/a")};
  BOOST_CHECK_THROW(metadata.wireEncode(), std::invalid_argument);
  metadata.roleBindings["sensor"].pop_back();
  metadata.profile = "unrecognized";
  BOOST_CHECK_THROW(metadata.wireEncode(), std::invalid_argument);
  metadata.profile = "data-admission-v1";
  metadata.roleBindings["sensor"] = {ndn::Name("/site/a"), ndn::Name("/8=site/8=a")};
  BOOST_CHECK_THROW(metadata.wireEncode(), std::invalid_argument);
  metadata.roleBindings.clear();
  metadata.roleBindings[std::string("\xC0\x80", 2)] = {ndn::Name("/site/a")};
  BOOST_CHECK_THROW(metadata.wireEncode(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RejectMalformedAndDuplicateMetadata)
{
  CollaborationDataflowMetadataV1 metadata;
  metadata.roleBindings["sensor"] = {ndn::Name("/site/a")};
  const auto wire = metadata.wireEncode();
  ndn::Block selection(tlv::ServiceSelectionMessageType);
  selection.push_back(wire);
  selection.push_back(wire);
  selection.encode();
  ServiceSelectionMessage decoded;
  BOOST_CHECK(!decoded.WireDecode(selection));
  ndn::Block malformed(0xF400);
  malformed.push_back(ndn::makeNonNegativeIntegerBlock(0xF402, 2));
  malformed.encode();
  BOOST_CHECK_THROW(CollaborationDataflowMetadataV1::wireDecode(malformed), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(InBandXorNegotiationCannotBeMistakenForTheOldSidecarFormat)
{
  CollaborationDataflowMetadataV1 metadata;
  metadata.roleBindings["source"] = {ndn::Name("/site/a")};
  metadata.dependencyRecoveryProfile = "xor-packet-set-v1";
  const auto decoded = CollaborationDataflowMetadataV1::wireDecode(metadata.wireEncode());
  BOOST_CHECK_EQUAL(decoded.dependencyRecoveryProfile, "xor-packet-set-v1");
  metadata.dependencyRecoveryProfile = "xor-wire-v1";
  BOOST_CHECK_THROW(metadata.wireEncode(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RejectOverlongMetadataAndProducerNameHeaders)
{
  CollaborationDataflowMetadataV1 metadata;
  metadata.roleBindings["sensor"] = {ndn::Name("/site/a")};
  const auto wire = metadata.wireEncode();
  const auto overlongType = withOverlongTlvHeader(wire, true);
  const auto overlongLength = withOverlongTlvHeader(wire, false);
  BOOST_CHECK(overlongType == wire); // Block equality omits outer wire headers.
  BOOST_CHECK(overlongLength == wire);
  BOOST_CHECK_THROW(CollaborationDataflowMetadataV1::wireDecode(overlongType), std::invalid_argument);
  BOOST_CHECK_THROW(CollaborationDataflowMetadataV1::wireDecode(overlongLength), std::invalid_argument);
  wire.parse();
  for (bool component : {false, true}) {
    auto fields = wire.elements();
    const ndn::Name name("/site/a");
    const auto producer = withExactTlvChildren(0xF40A, {
      component ? nameWithOverlongComponent(name) : withOverlongTlvHeader(name.wireEncode(), false)});
    fields.at(3) = withExactTlvChildren(0xF406, {ndn::makeStringBlock(0xF408, "sensor"), producer});
    const auto malformed = withExactTlvChildren(0xF400, fields);
    BOOST_REQUIRE_GT(malformed.size(), wire.size());
    BOOST_CHECK_THROW(CollaborationDataflowMetadataV1::wireDecode(malformed), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(EnforceLimitsAndPreserveLegacyWire)
{
  CollaborationDataflowMetadataV1 metadata;
  metadata.roleBindings[std::string(129, 'r')] = {ndn::Name("/site/a")};
  BOOST_CHECK_THROW(metadata.wireEncode(), std::invalid_argument);
  ServiceSelectionMessage legacy;
  legacy.setProviderToken("old-token");
  const auto wire = legacy.WireEncode();
  ServiceSelectionMessage parsed;
  BOOST_REQUIRE(parsed.WireDecode(wire));
  BOOST_CHECK(!parsed.hasCollaborationDataflowMetadata());
  BOOST_CHECK(parsed.WireEncode() == wire);
}

BOOST_AUTO_TEST_CASE(ProductionExactReadRetainsOriginalWireAndRejectsWrongSigner)
{
  using namespace std::chrono_literals;
  BootstrapProfile profile;
  profile.providerCount = 2;
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  auto interests = consumerFace.onSendInterest.connect(
    [&](const ndn::Interest& interest) { producerFace.receive(interest); });
  std::optional<ndn::Buffer> original;
  const ndn::Name name = ndn::Name(producer.getName()).append("NDNSF").append("INPUT").appendVersion(1);
  auto packets = producerFace.onSendData.connect([&](const ndn::Data& data) {
    if (data.getName() == name) {
      const auto& wire = data.wireEncode();
      original.emplace(wire.begin(), wire.end());
    }
    consumerFace.receive(data);
  });
  const auto active = std::make_shared<std::atomic<bool>>(true);
  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "consumer";
  assignment.service = profile.serviceName;
  assignment.inputAdmissionProfile = "data-admission-v1";
  assignment.selectionDigest = "test-selected-dispatch";
  assignment.scopeKeys["input"] = ndn::Buffer(32, 0x51);
  assignment.workDeadline = std::chrono::steady_clock::now() + 3s;
  assignment.workCurrent = [active] { return active->load(); };
  const ndn::Name request("/spec226/identity/read");
  ServiceProvider::CollaborationContext publisher(producer, profile.userIdentity,
    request, RequestMessage(), assignment);
  ServiceProvider::CollaborationContext receiver(consumer, profile.userIdentity,
    request, RequestMessage(), assignment);
  BOOST_REQUIRE(publisher.publishSignedExactData("input", {{name, ndn::Buffer{1, 2, 3}}}));
  auto operation = receiver.startVerifiedSignedExactDataBatch("input", {name},
    producer.getName(), 1000, {}, {}, ServiceProvider::ExactDataFetchPhase::Published, {}, 1);
  environment.pumpUntil([&] { return operation.wait_for(0ms) == std::future_status::ready; });
  auto result = operation.get();
  BOOST_REQUIRE(result);
  BOOST_REQUIRE(original);
  BOOST_REQUIRE_EQUAL(result->object.packets().size(), 1);
  const auto& wire = result->object.packets()[0]->wireEncode();
  BOOST_CHECK_EQUAL_COLLECTIONS(wire.begin(), wire.end(), original->begin(), original->end());
  BOOST_CHECK_EQUAL(result->object.producer(), producer.getName());
  BOOST_CHECK_THROW(result->object.content(), std::logic_error);
  auto delayedGet = receiver.startVerifiedSignedExactDataBatch("input", {name},
    producer.getName(), 50, {}, {}, ServiceProvider::ExactDataFetchPhase::Published, {}, 1);
  environment.pumpUntil([&] { return delayedGet.wait_for(0ms) == std::future_status::ready; });
  consumerFace.processEvents(ndn::time::milliseconds(100));
  BOOST_CHECK(delayedGet.get());

  const auto wrongName = ndn::Name(producer.getName()).append("NDNSF").append("INPUT").appendVersion(2);
  ndn::Data wrong(wrongName);
  wrong.setContent(ndn::Buffer{1, 2, 3});
  environment.keyChain().sign(wrong, ndn::security::signingByCertificate(consumer.getSigningCertificateName()));
  auto wrongPacket = consumerFace.onSendInterest.connect([&](const ndn::Interest& interest) {
    if (interest.getName() == wrongName) consumerFace.receive(wrong);
  });
  auto rejected = receiver.startVerifiedSignedExactDataBatch("input", {wrongName},
    producer.getName(), 500, {}, {}, ServiceProvider::ExactDataFetchPhase::Published, {}, 1);
  environment.pumpUntil([&] { return rejected.wait_for(0ms) == std::future_status::ready; });
  BOOST_CHECK(!rejected.get());
  const auto tamperedName = ndn::Name(producer.getName()).append("NDNSF").append("INPUT").appendVersion(3);
  ndn::Data tampered(tamperedName);
  tampered.setContent(ndn::Buffer{1, 2, 3});
  environment.keyChain().sign(tampered, ndn::security::signingByCertificate(producer.getSigningCertificateName()));
  tampered.setContent(ndn::Buffer{9, 8, 7});
  auto tamperedPacket = consumerFace.onSendInterest.connect([&](const ndn::Interest& interest) {
    if (interest.getName() == tamperedName) consumerFace.receive(tampered);
  });
  auto invalidSignature = receiver.startVerifiedSignedExactDataBatch("input", {tamperedName},
    producer.getName(), 500, {}, {}, ServiceProvider::ExactDataFetchPhase::Published, {}, 1);
  environment.pumpUntil([&] { return invalidSignature.wait_for(0ms) == std::future_status::ready; });
  BOOST_CHECK(!invalidSignature.get());
  BOOST_CHECK_THROW(receiver.startVerifiedSignedExactDataBatch("input", {name},
    producer.getName(), 500, {}, {}, ServiceProvider::ExactDataFetchPhase::Published, {}, 0),
    std::invalid_argument);
  auto cancelled = receiver.startVerifiedSignedExactDataBatch("input", {wrongName},
    producer.getName(), 500, {}, {}, ServiceProvider::ExactDataFetchPhase::Published, {}, 1);
  cancelled.cancel();
  environment.pumpUntil([&] { return cancelled.wait_for(0ms) == std::future_status::ready; });
  BOOST_CHECK(!cancelled.get());
}

BOOST_AUTO_TEST_CASE(SelectionUsesCompleteCommittedBindingsAndBindsTransportChoice)
{
  using namespace std::chrono_literals;
  ndn::security::KeyChain keyChain("pib-memory:spec226-plan", "tpm-memory:spec226-plan");
  ndn::DummyClientFace face(keyChain);
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/spec226/user"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec226/aa"));
  IsolatedTestTrust trust{userCert, aaCert};
  CapabilityUser user(face, ndn::Name("/spec226/group"), userCert, aaCert, trust.schemaPath());
  CollaborationAckClosure closed;
  const ndn::Name request("/spec226/selected-pair");
  user.prepareDeferredCollaborationForTest(request, [&](const auto& value) { closed = value; });
  user.addAck(request, ndn::Name("/spec226/provider/a"), true);
  user.addAck(request, ndn::Name("/spec226/provider/b"), true);
  BOOST_REQUIRE(user.closeDeferredAcksForTest(request));
  CollaborationPlan plan;
  plan.ackCollectionTimeMs = 100;
  plan.timeoutMs = 1000;
  CollaborationRoleSpec role;
  role.role = "worker";
  role.service = ndn::Name("/generic/work");
  role.minProviders = role.maxProviders = 2;
  role.inputAdmissionProfile = "data-admission-v1";
  plan.roles = {role};
  plan.participantSelector = std::make_shared<IdentitySelector>();
  std::vector<ServiceSelectionMessage> projections;
  user.setLocalPublicationHandler([&](const ndn::Name&, const ndn::Buffer& wire) {
    ServiceSelectionMessage message;
    if (message.WireDecode(ndn::Block(wire))) projections.push_back(std::move(message));
  });
  BOOST_REQUIRE(user.CommitCollaborationPlan(request, closed.digest, plan));
  face.processEvents(ndn::time::milliseconds(20));
  BOOST_REQUIRE_EQUAL(projections.size(), 2);
  for (const auto& message : projections) {
    BOOST_REQUIRE(message.hasCollaborationDataflowMetadata());
    const auto& metadata = message.getCollaborationDataflowMetadata();
    BOOST_CHECK_EQUAL(metadata.dependencyRecoveryProfile, "arq-v1");
    BOOST_REQUIRE_EQUAL(metadata.roleBindings.at("worker").size(), 2);
    BOOST_CHECK_EQUAL(metadata.roleBindings.at("worker")[0], ndn::Name("/spec226/provider/a"));
    BOOST_CHECK_EQUAL(metadata.roleBindings.at("worker")[1], ndn::Name("/spec226/provider/b"));
  }
  plan.dependencyTransport.redundancy = DependencyRedundancy::Xor;
  BOOST_CHECK_THROW(user.CommitCollaborationPlan(request, closed.digest, plan), std::logic_error);
}

BOOST_AUTO_TEST_CASE(MissingAndForgedCapabilitiesFailBeforeSelection)
{
  ndn::security::KeyChain keyChain("pib-memory:spec226-cap", "tpm-memory:spec226-cap");
  ndn::DummyClientFace face(keyChain);
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/spec226/user"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec226/aa"));
  IsolatedTestTrust trust{userCert, aaCert};
  CapabilityUser user(face, ndn::Name("/spec226/group"), userCert, aaCert, trust.schemaPath());
  CollaborationAckClosure closed;
  const ndn::Name request("/spec226/no-capability");
  user.prepareDeferredCollaborationForTest(request, [&](const auto& value) { closed = value; });
  user.addAck(request, ndn::Name("/spec226/provider/a"), false);
  BOOST_REQUIRE(user.closeDeferredAcksForTest(request));
  CollaborationPlan plan;
  plan.ackCollectionTimeMs = 100;
  plan.timeoutMs = 1000;
  CollaborationRoleSpec role;
  role.role = "worker";
  role.service = ndn::Name("/generic/work");
  role.inputAdmissionProfile = "data-admission-v1";
  plan.roles = {role};
  auto selector = std::make_shared<IdentitySelector>();
  plan.participantSelector = selector;
  BOOST_CHECK_EXCEPTION(user.CommitCollaborationPlan(request, closed.digest, plan), std::invalid_argument,
    [](const std::exception& error) { return std::string(error.what()).find("INPUT_PROFILE_UNSUPPORTED") != std::string::npos; });
  selector->forgeCapability = true;
  BOOST_CHECK_EXCEPTION(user.CommitCollaborationPlan(request, closed.digest, plan), std::invalid_argument,
    [](const std::exception& error) { return std::string(error.what()).find("outside ACK_CLOSED") != std::string::npos; });
  BOOST_CHECK(user.getSelectionPublishedProviders(request).empty());
  auto legacy = role;
  legacy.role = "legacy";
  legacy.inputAdmissionProfile.clear();
  legacy.minProviders = 0;
  plan.roles.push_back(legacy);
  BOOST_CHECK_THROW(user.CommitCollaborationPlan(request, closed.digest, plan), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(LegacyRoleValidationKeepsItsExistingCompatibility)
{
  ndn::security::KeyChain keyChain("pib-memory:spec226-legacy", "tpm-memory:spec226-legacy");
  ndn::DummyClientFace face(keyChain);
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/spec226/user"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/spec226/aa"));
  IsolatedTestTrust trust{userCert, aaCert};
  CapabilityUser user(face, ndn::Name("/spec226/group"), userCert, aaCert, trust.schemaPath());
  CollaborationAckClosure closed;
  const ndn::Name request("/spec226/legacy-role-compatibility");
  user.prepareDeferredCollaborationForTest(request, [&](const auto& value) { closed = value; });
  user.addAck(request, ndn::Name("/spec226/provider/a"), false);
  BOOST_REQUIRE(user.closeDeferredAcksForTest(request));
  CollaborationPlan plan;
  plan.ackCollectionTimeMs = 100;
  plan.timeoutMs = 1000;
  CollaborationRoleSpec legacy;
  legacy.role = "worker";
  legacy.service = ndn::Name("/generic/work");
  plan.roles = {legacy, legacy};
  plan.participantSelector = std::make_shared<IdentitySelector>();
  size_t publications = 0;
  user.setLocalPublicationHandler([&](const ndn::Name&, const ndn::Buffer& wire) {
    ServiceSelectionMessage message;
    if (message.WireDecode(ndn::Block(wire))) {
      ++publications;
      BOOST_CHECK(!message.hasCollaborationDataflowMetadata());
    }
  });
  BOOST_CHECK(user.CommitCollaborationPlan(request, closed.digest, plan));
  BOOST_CHECK_GT(publications, 0);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
