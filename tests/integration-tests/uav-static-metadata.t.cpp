#include "NDNSF-UAV-APP/shared/UavObservationRefVerifier.hpp"
#include "NDNSF-UAV-APP/shared/UavDigest.hpp"
#include "ndnsf-integration-fixture.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <boost/asio/post.hpp>
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::examples::uav::tests {

BOOST_AUTO_TEST_SUITE(UavStaticMetadata)

BOOST_AUTO_TEST_CASE(DigestBoundNamesPreserveLegacyAndRejectWrongIdentity)
{
  const ndn::Name producer("/example/uav/source");
  const std::string digest = "sha256:" + std::string(64, 'a');
  for (const std::string category : {"CLOCK", "CALIBRATION", "GROUND"}) {
    auto prefix = ndn::Name(producer).append("UAV").append(category);
    if (category == "CALIBRATION") prefix.append("UAV1");
    prefix.append("1");
    auto legacy = prefix;
    legacy.appendVersion(7);
    auto bound = makeUavStaticObservationPrefix(prefix, digest);
    BOOST_CHECK_EQUAL(bound.toUri(), prefix.toUri() + "/" + std::string(64, 'a'));
    bound.appendVersion(7);
    for (const auto& name : {legacy, bound}) {
      const auto ref = parseObservationReference(name.toUri(), digest, producer.toUri(), category);
      BOOST_CHECK_NO_THROW(requireObservationReferenceNameShape(ref, category, "UAV1", 2, 3));
      BOOST_CHECK_NO_THROW(validateObservationReference(ref, legacy, producer, digest, category));
      BOOST_CHECK_THROW(validateObservationReference(ref, legacy, ndn::Name("/wrong"), digest,
                                                      category), std::runtime_error);
      BOOST_CHECK_THROW(validateObservationReference(ref, legacy, producer,
        "sha256:" + std::string(64, 'b'), category), std::runtime_error);
    }
    for (const auto& suffix : {std::string(64, 'b') + "/v=7",
                               std::string(63, 'a') + "/v=7",
                               std::string(64, 'a') + "/extra/v=7"}) {
      const auto ref = parseObservationReference(prefix.toUri() + "/" + suffix, digest,
                                                 producer.toUri(), category);
      BOOST_CHECK_THROW(requireObservationReferenceNameShape(ref, category, "UAV1", 2, 3),
                        std::runtime_error);
    }
    auto wrongRevision = prefix.getPrefix(-1);
    wrongRevision.append("01").append(std::string(64, 'a')).appendVersion(7);
    const auto ref = parseObservationReference(wrongRevision.toUri(), digest, producer.toUri(), category);
    BOOST_CHECK_THROW(requireObservationReferenceNameShape(ref, category, "UAV1", 2, 3),
                      std::runtime_error);
  }
  auto pose = ndn::Name(producer.toUri() + "/UAV/OBS/UAV1/2/3/POSE");
  pose.append(std::string(64, 'a')).appendVersion(7);
  const auto ref = parseObservationReference(pose.toUri(), digest, producer.toUri(), "POSE");
  BOOST_CHECK_THROW(requireObservationReferenceNameShape(ref, "POSE", "UAV1", 2, 3),
                    std::runtime_error);
  BOOST_CHECK_THROW(makeUavStaticObservationPrefix(ndn::Name("/prefix"), "bad"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(SameRevisionDifferentModesReuseOriginalAAfterB)
{
  using Context = ndn_service_framework::ServiceProvider::CollaborationContext;
  using Assignment = ndn_service_framework::ServiceProvider::CollaborationAssignment;
  ndn_service_framework::test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/UAV/Observation");
  profile.providerIdentity = ndn::Name("/example/uav/static-provider");
  profile.syncPrefix = ndn::Name("/example/uav/static-group");
  profile.providerCount = 2;
  // Fixed identity anchors, actual AA parameters/DKEY and production ingress.
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE_MESSAGE(environment.status() ==
    ndn_service_framework::test::EnvironmentStatus::Ready, environment.failureReason());
  for (size_t i = 0; i < environment.providerCount(); ++i)
    environment.disconnectProviderTransportForTest(i);
  environment.disconnectProviderPeerTransportForTest();
  environment.enableProductionIngressForTest();
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  const auto producerName = producer.getName();
  const auto controllerCertificate = environment.keyChain().createIdentity(
    ndn::Name("/example/uav/static-controller"), ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
  auto installStatus = [&](uint64_t policyGeneration) {
    ndn_service_framework::PolicyStatusData status;
    const ndn_service_framework::ControllerVersion version{1000, policyGeneration};
    const auto now = static_cast<uint64_t>(ndn::time::duration_cast<ndn::time::milliseconds>(
      ndn::time::system_clock::now().time_since_epoch()).count());
    status.setServiceName(profile.serviceName);
    status.setControllerVersion(version);
    status.setValidity(now - 1000, now + 600000);
    status.setPolicyDigest("sha256:" + std::string(64, policyGeneration == 1 ? 'a' : 'b'));
    status.setAbePublicParametersName(environment.attributeAuthorityPublicParametersName());
    status.setAbePublicParametersDigest(environment.attributeAuthorityPublicParametersDigest());
    status.setControllerCertificate(controllerCertificate.getName());
    ndn::Data data(ndn_service_framework::makePolicyStatusName(
      controllerCertificate.getIdentity(), profile.serviceName, version));
    data.setContent(status.wireEncode());
    environment.keyChain().sign(data, ndn::security::signingByCertificate(controllerCertificate));
    const bool signatureValid = ndn::security::verifySignature(data, controllerCertificate);
    BOOST_REQUIRE(signatureValid);
    for (size_t i = 0; i < environment.providerCount(); ++i) {
      BOOST_REQUIRE(environment.provider(i).installControllerStatus(status, signatureValid));
      BOOST_REQUIRE(environment.provider(i).getControllerVersion(profile.serviceName));
      BOOST_CHECK(*environment.provider(i).getControllerVersion(profile.serviceName) == version);
    }
    environment.pumpUntilWithAttributeAuthority([&] {
      for (size_t i = 0; i < environment.providerCount(); ++i)
        if (!environment.provider(i).isRequestDecryptionReady() ||
            !environment.provider(i).isNacProducerReadyForTest()) return false;
      return true;
    });
  };
  installStatus(1);
  Assignment sourceAssignment;
  sourceAssignment.role = "EvidenceSource";
  sourceAssignment.service = profile.serviceName;
  sourceAssignment.workCurrent = [] { return true; };
  sourceAssignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  Assignment computeAssignment = sourceAssignment;
  computeAssignment.role = "DetectorReporter";
  const auto computeCurrent = std::make_shared<std::atomic<bool>>(true);
  computeAssignment.workCurrent = [computeCurrent] { return computeCurrent->load(); };
  Context firstSource(producer, profile.userIdentity, ndn::Name("/request/static-a"),
                      ndn_service_framework::RequestMessage(), sourceAssignment);
  Context nextSource(producer, profile.userIdentity, ndn::Name("/request/static-b"),
                     ndn_service_framework::RequestMessage(), sourceAssignment);
  Context firstCompute(consumer, profile.userIdentity, ndn::Name("/request/compute-a"),
                       ndn_service_framework::RequestMessage(), computeAssignment);
  auto execute = [&](auto operation) {
    auto result = std::async(std::launch::async, std::move(operation));
    environment.pumpUntilWithAttributeAuthority([&] {
      return result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    });
    BOOST_REQUIRE(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    return result.get();
  };
  std::map<ndn::Name, ndn::Block> wires;
  unsigned wireConflicts = 0, replayedA = 0;
  ndn::Name currentA;
  ndn::signal::ScopedConnection observe = environment.providerFace(0).onSendData.connect(
    [&](const ndn::Data& packet) {
      const auto found = wires.find(packet.getName());
      if (found != wires.end()) {
        if (found->second != packet.wireEncode()) ++wireConflicts;
        else if (!currentA.empty() && currentA.isPrefixOf(packet.getName())) ++replayedA;
      }
      else wires.emplace(packet.getName(), packet.wireEncode());
    });
  std::vector<ndn::signal::ScopedConnection> bridges;
  for (size_t source = 0; source < 2; ++source) {
    const size_t destination = 1 - source;
    bridges.emplace_back(environment.providerFace(source).onSendInterest.connect(
      [&, destination](const ndn::Interest& interest) {
        boost::asio::post(environment.providerFace(destination).getIoContext(),
          [&, destination, interest] { environment.providerFace(destination).receive(interest); });
      }));
    bridges.emplace_back(environment.providerFace(source).onSendData.connect(
      [&, destination](const ndn::Data& packet) {
        boost::asio::post(environment.providerFace(destination).getIoContext(),
          [&, destination, packet] { environment.providerFace(destination).receive(packet); });
      }));
  }
  for (const std::string category : {"CLOCK", "CALIBRATION", "GROUND"}) {
    auto revisionPrefix = ndn::Name(producerName).append("UAV").append(category);
    if (category == "CALIBRATION") revisionPrefix.append("UAV1");
    revisionPrefix.append("1");
    const ndn::Buffer a{'p', 'e', 'r', 's', 'o', 'n'};
    const ndn::Buffer b{'v', 'e', 'h', 'i', 'c', 'l', 'e'};
    auto hash = [](const ndn::Buffer& bytes) {
      ndn::util::Sha256 digest;
      digest << std::string(bytes.begin(), bytes.end());
      return canonicalSha256Digest(digest.toString());
    };
    const auto prefixA = makeUavStaticObservationPrefix(revisionPrefix, hash(a));
    const auto prefixB = makeUavStaticObservationPrefix(revisionPrefix, hash(b));
    BOOST_CHECK(prefixA != prefixB);
    const auto first = execute([&] { return firstSource.publishImmutableProtectedData(prefixA, a); });
    BOOST_REQUIRE_MESSAGE(first.success, first.errorMessage);
    const auto second = execute([&] { return nextSource.publishImmutableProtectedData(prefixB, b); });
    BOOST_REQUIRE_MESSAGE(second.success, second.errorMessage);
    BOOST_CHECK(first.dataName != second.dataName);
    const auto conflict = execute([&] { return nextSource.publishImmutableProtectedData(prefixA, b); });
    BOOST_CHECK(!conflict.success);
    BOOST_CHECK_EQUAL(conflict.errorMessage, "IDENTITY_CONFLICT");
    const auto fetch = [&](const ndn::Name& name) {
      return execute([&] { return firstCompute.fetchImmutableProtectedDataCurrent(
        name, producerName, std::chrono::steady_clock::now() + std::chrono::seconds(3)); });
    };
    currentA = first.dataName;
    const auto fetchedA = fetch(first.dataName);
    BOOST_REQUIRE(fetchedA);
    BOOST_CHECK(*fetchedA == a);
    const auto fetchedB = fetch(second.dataName);
    BOOST_REQUIRE(fetchedB);
    BOOST_CHECK(*fetchedB == b);
    const auto again = execute([&] { return nextSource.publishImmutableProtectedData(prefixA, a); });
    BOOST_REQUIRE_MESSAGE(again.success, again.errorMessage);
    BOOST_CHECK(again.dataName == first.dataName);
    BOOST_CHECK_EQUAL(again.contentSha256, first.contentSha256);
    const auto replaysBefore = replayedA;
    const auto fetchedAgain = execute([&] { return firstCompute.fetchImmutableProtectedDataCurrent(
      again.dataName, producerName, std::chrono::steady_clock::now() + std::chrono::seconds(3)); });
    BOOST_REQUIRE(fetchedAgain);
    BOOST_CHECK(*fetchedAgain == a);
    BOOST_CHECK_GT(replayedA, replaysBefore);
    BOOST_CHECK_EQUAL(first.dataName.getPrefix(-2).toUri(), revisionPrefix.toUri());
    BOOST_CHECK_EQUAL(first.dataName.get(-2).toUri(), hash(a).substr(7));
    BOOST_CHECK(first.dataName.get(-1).isVersion());
  }
  BOOST_CHECK(!wires.empty());
  BOOST_CHECK_EQUAL(wireConflicts, 0U);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::examples::uav::tests
