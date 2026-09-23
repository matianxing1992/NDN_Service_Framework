#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include "ndn-service-framework/HybridMessageCrypto.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-svs/security-options.hpp>
#include <ndn-svs/svspubsub.hpp>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <stdexcept>
#include <vector>

namespace {

using ndn_service_framework::PreparedServiceRequest;
using ndn_service_framework::HybridMessageEnvelope;
using ndn_service_framework::hybridAesGcmDecrypt;
using ndn_service_framework::test::LocalServiceUser;
using ndn_service_framework::test::ScopedEnvironmentValue;
using ndn_service_framework::test::makeRsaIdentity;

std::shared_ptr<ndn::svs::SVSPubSub>
makeTestPubSub(ndn::Face& face, ndn::KeyChain& keyChain)
{
  ndn::svs::SecurityOptions security(keyChain);
  security.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
  security.dataSigner->signingInfo = ndn::security::signingWithSha256();
  security.pubSigner->signingInfo = ndn::security::signingWithSha256();
  security.validator = std::make_shared<ndn::svs::BaseValidator>();
  security.encapsulatedDataValidator = std::make_shared<ndn::svs::BaseValidator>();
  ndn::svs::SVSPubSubOptions options;
  options.useTimestamp = false;
  return std::make_shared<ndn::svs::SVSPubSub>(
    ndn::Name("/spec188/large-data/sync"),
    ndn::Name("/spec188/large-data/user"),
    face,
    [] (const std::vector<ndn::svs::MissingDataInfo>&) {},
    options,
    security);
}

std::vector<const ndn::Data*>
findSegments(const ndn::DummyClientFace& face, const ndn::Name& baseName)
{
  std::vector<const ndn::Data*> result;
  for (const auto& data : face.sentData) {
    if (baseName.isPrefixOf(data.getName()) &&
        data.getName().size() == baseName.size() + 1 &&
        data.getName().get(-1).isSegment()) {
      result.push_back(&data);
    }
  }
  std::sort(result.begin(), result.end(), [] (const auto* left, const auto* right) {
    return left->getName().get(-1).toSegment() < right->getName().get(-1).toSegment();
  });
  return result;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec188BoundedLargeDataPublisher)

BOOST_AUTO_TEST_CASE(FileBackedSegmentsAreExactAndWindowed)
{
  const auto dataDirectory = std::filesystem::temp_directory_path() /
                             "spec188-bounded-large-data-selector";
  const auto dataDirectoryValue = dataDirectory.string();
  std::error_code cleanupError;
  std::filesystem::remove_all(dataDirectory, cleanupError);
  ScopedEnvironmentValue fileBacked("NDNSF_REQUEST_LARGE_FILE_BACKED", "1");
  ScopedEnvironmentValue dataDir("NDNSF_REQUEST_LARGE_DATA_DIR", dataDirectoryValue.c_str());
  ScopedEnvironmentValue window("NDNSF_REQUEST_LARGE_WINDOW_SEGMENTS", "2");
  ScopedEnvironmentValue retention("NDNSF_REQUEST_LARGE_DATA_RETENTION_MS", "500");

  ndn::security::KeyChain keyChain("pib-memory:spec188-large-data",
                                   "tpm-memory:spec188-large-data");
  ndn::DummyClientFace face(keyChain);
  const auto userCert = makeRsaIdentity(keyChain, ndn::Name("/spec188/user"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec188/authority"));
  const auto expectedPath = dataDirectory;
  {
    LocalServiceUser user(face, ndn::Name("/spec188/group"), userCert,
                          authorityCert, "examples/trust-any.conf");
    user.useSigningKeyChainForSigningOnlyForTest(keyChain);
    user.attachLocalMockPubSubForTest(makeTestPubSub(face, keyChain));
    user.init();
    face.processEvents(ndn::time::milliseconds(1));

    const ndn::Name serviceName("/spec188/large-service");
    const auto context = user.prepareServiceRequest(serviceName.toUri());
    const auto sendKey = user.prepareHybridSendKeyForTest(serviceName, "REQUEST-LARGE");
    const std::vector<uint8_t> plaintext(40000, 0x6b);
    const auto before = face.sentData.size();
    const auto published = user.publishEncryptedLargeData(
      context, plaintext, "bounded-file-object", ndn::time::milliseconds(1));

    BOOST_REQUIRE_MESSAGE(published.success,
                          "file-backed large-data publication failed: " +
                          published.errorMessage);
    BOOST_CHECK_EQUAL(face.sentData.size(), before);
    const auto metricsBefore = user.getLargeDataServingMetricsForTest();
    BOOST_CHECK_EQUAL(metricsBefore.publicationCount, 1U);

    // Local file retention is independent of the wire FreshnessPeriod.  A
    // provider may receive the reference after the short wire freshness has
    // elapsed, while the file-backed publisher must still serve the object.
    face.processEvents(ndn::time::milliseconds(20));
    BOOST_CHECK_EQUAL(user.getLargeDataServingMetricsForTest().publicationCount, 1U);

    // SegmentFetcher first asks for the versioned base name with
    // CanBePrefix; the publisher must answer that discovery Interest with
    // segment zero before the explicit /seg=N requests begin.
    ndn::Interest discoveryInterest(published.encryptedDataName);
    discoveryInterest.setCanBePrefix(true);
    face.receive(discoveryInterest);
    face.processEvents(ndn::time::milliseconds(1));
    auto discoverySegments = findSegments(face, published.encryptedDataName);
    BOOST_REQUIRE_EQUAL(discoverySegments.size(), 1U);
    BOOST_CHECK_EQUAL(discoverySegments.front()->getName().get(-1).toSegment(), 0U);
    BOOST_CHECK_EQUAL(user.getLargeDataServingMetricsForTest().segmentReadCount, 1U);

    // An exact base-name Interest without CanBePrefix is not the
    // SegmentFetcher discovery form and must remain eligible for the normal
    // IMS fallback instead of being answered with a differently named Data.
    const auto sentBeforeExactBase = face.sentData.size();
    face.receive(ndn::Interest(published.encryptedDataName));
    face.processEvents(ndn::time::milliseconds(1));
    BOOST_CHECK_EQUAL(face.sentData.size(), sentBeforeExactBase);
    BOOST_CHECK_EQUAL(user.getLargeDataServingMetricsForTest().segmentReadCount, 1U);

    const auto firstSegment = ndn::Name(published.encryptedDataName).appendSegment(0);
    const auto secondSegment = ndn::Name(published.encryptedDataName).appendSegment(1);
    const auto thirdSegment = ndn::Name(published.encryptedDataName).appendSegment(2);
    face.receive(ndn::Interest(firstSegment));
    face.receive(ndn::Interest(secondSegment));
    face.receive(ndn::Interest(firstSegment)); // bounded-window hit
    face.receive(ndn::Interest(thirdSegment)); // evicts the oldest window entry
    face.processEvents(ndn::time::milliseconds(1));

    const auto metrics = user.getLargeDataServingMetricsForTest();
    BOOST_CHECK_EQUAL(metrics.publicationCount, 1U);
    BOOST_CHECK_EQUAL(metrics.segmentReadCount, 3U);
    BOOST_CHECK_EQUAL(metrics.retransmissionHitCount, 2U);
    BOOST_CHECK_LE(metrics.peakWindowSegments, 2U);

    auto segments = findSegments(face, published.encryptedDataName);
    BOOST_REQUIRE_GE(segments.size(), 4U);
    BOOST_REQUIRE(segments.front()->getFinalBlock());
    const auto finalSegment = segments.front()->getFinalBlock()->toSegment();
    std::set<uint64_t> observedSegments;
    for (const auto* data : segments) {
      observedSegments.insert(data->getName().get(-1).toSegment());
    }
    for (uint64_t segment = 0; segment <= finalSegment; ++segment) {
      if (observedSegments.count(segment) == 0) {
        face.receive(ndn::Interest(
          ndn::Name(published.encryptedDataName).appendSegment(segment)));
      }
    }
    face.processEvents(ndn::time::milliseconds(1));
    segments = findSegments(face, published.encryptedDataName);
    std::map<uint64_t, const ndn::Data*> uniqueSegments;
    for (const auto* data : segments) {
      uniqueSegments.emplace(data->getName().get(-1).toSegment(), data);
      BOOST_CHECK_LE(data->getContent().value_size(), 7000U);
      BOOST_CHECK_LE(data->wireEncode().size(), 8800U);
      BOOST_REQUIRE(data->getFinalBlock());
      BOOST_CHECK(data->getFinalBlock()->isSegment());
      BOOST_CHECK(ndn::security::verifySignature(*data, userCert));
    }
    BOOST_REQUIRE_EQUAL(uniqueSegments.size(), finalSegment + 1);
    BOOST_CHECK_EQUAL(segments.front()->getFinalBlock()->toSegment(),
                      segments.back()->getFinalBlock()->toSegment());

    ndn::Buffer assembled;
    for (uint64_t segment = 0; segment <= finalSegment; ++segment) {
      const auto* data = uniqueSegments.at(segment);
      const auto content = data->getContent();
      assembled.insert(assembled.end(), content.value(),
                       content.value() + content.value_size());
    }
    HybridMessageEnvelope envelope;
    BOOST_REQUIRE(envelope.WireDecode(ndn::Block(assembled)));
    BOOST_CHECK_EQUAL(envelope.getMessageType(), "REQUEST-LARGE");
    const std::string adText = published.encryptedDataName.toUri() +
                               "|REQUEST-LARGE|" + serviceName.toUri();
    const ndn::Buffer associatedData(reinterpret_cast<const uint8_t*>(adText.data()),
                                     adText.size());
    ndn::Buffer decrypted;
    BOOST_REQUIRE(hybridAesGcmDecrypt(sendKey.key, envelope, associatedData, decrypted));
    BOOST_CHECK_EQUAL_COLLECTIONS(decrypted.begin(), decrypted.end(),
                                  plaintext.begin(), plaintext.end());
    const auto metricsAfterAssembly = user.getLargeDataServingMetricsForTest();
    BOOST_CHECK_EQUAL(metricsAfterAssembly.segmentReadCount, uniqueSegments.size());
    BOOST_CHECK_EQUAL(metricsAfterAssembly.retransmissionHitCount, 2U);

    const auto fileCount = std::distance(
      std::filesystem::directory_iterator(dataDirectory),
      std::filesystem::directory_iterator());
    BOOST_CHECK_EQUAL(fileCount, 1);
  }
  // The ServiceUser owner removes its immutable wire file at scope exit.
  BOOST_CHECK_EQUAL(std::distance(std::filesystem::directory_iterator(expectedPath),
                                  std::filesystem::directory_iterator()), 0);
}

BOOST_AUTO_TEST_CASE(InvalidFileBackedRetentionHasNoPublicationSideEffects)
{
  const auto dataDirectory = std::filesystem::temp_directory_path() /
                             "spec188-invalid-retention-selector";
  std::error_code cleanupError;
  std::filesystem::remove_all(dataDirectory, cleanupError);
  const auto dataDirectoryValue = dataDirectory.string();
  ScopedEnvironmentValue fileBacked("NDNSF_REQUEST_LARGE_FILE_BACKED", "1");
  ScopedEnvironmentValue dataDir("NDNSF_REQUEST_LARGE_DATA_DIR", dataDirectoryValue.c_str());
  ScopedEnvironmentValue invalidRetention("NDNSF_REQUEST_LARGE_DATA_RETENTION_MS", "0");

  ndn::security::KeyChain keyChain("pib-memory:spec188-invalid-retention",
                                   "tpm-memory:spec188-invalid-retention");
  ndn::DummyClientFace face(keyChain);
  const auto userCert = makeRsaIdentity(keyChain, ndn::Name("/spec188/invalid-user"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec188/invalid-authority"));
  {
    LocalServiceUser user(face, ndn::Name("/spec188/invalid-group"), userCert,
                          authorityCert, "examples/trust-any.conf");
    user.useSigningKeyChainForSigningOnlyForTest(keyChain);
    user.attachLocalMockPubSubForTest(makeTestPubSub(face, keyChain));
    user.init();
    face.processEvents(ndn::time::milliseconds(1));
    const auto context = user.prepareServiceRequest("/spec188/invalid-retention-service");
    const std::vector<uint8_t> plaintext(40000, 0x4d);

    BOOST_CHECK_THROW(user.publishEncryptedLargeData(
                        context, plaintext, "invalid-retention",
                        ndn::time::milliseconds(1)),
                      std::invalid_argument);
    BOOST_CHECK_EQUAL(user.getLargeDataServingMetricsForTest().publicationCount, 0U);
    BOOST_CHECK(!std::filesystem::exists(dataDirectory));
  }
  BOOST_CHECK(!std::filesystem::exists(dataDirectory));
}

BOOST_AUTO_TEST_SUITE_END()
