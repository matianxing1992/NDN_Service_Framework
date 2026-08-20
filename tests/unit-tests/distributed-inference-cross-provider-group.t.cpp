#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollectiveControl.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderGroupCoordinator.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"

#include <boost/test/unit_test.hpp>

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <cstdint>
#include <algorithm>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::di::tests {
namespace {

CollectiveSegmentDescriptor
makeDescriptor()
{
  CollectiveSegmentDescriptor descriptor;
  descriptor.requestId = "/request/one";
  descriptor.attemptId = "attempt-1";
  descriptor.planDigest = "sha256:plan";
  descriptor.groupId = "group-1";
  descriptor.epoch = 7;
  descriptor.operationIndex = 3;
  descriptor.operationKind = "ALL_GATHER";
  descriptor.producerRank = "P0/R0";
  descriptor.tensorDigest = "sha256:tensor";
  descriptor.manifestDigest = "sha256:manifest";
  descriptor.segmentNo = 0;
  descriptor.segmentCount = 2;
  descriptor.totalBytes = 6;
  descriptor.segmentSize = 3;
  descriptor.noProgressMs = 1000;
  descriptor.hardDeadlineMs = 5000;
  return descriptor;
}

std::vector<uint8_t>
makeKey()
{
  return std::vector<uint8_t>(32, 0x42);
}

ProviderGroupCoordinatorOptions
makeCoordinatorOptions()
{
  ProviderGroupCoordinatorOptions options;
  options.randomBytes = [] (std::size_t size) {
    return std::vector<uint8_t>(size, 0x5a);
  };
  options.wrapEpochKey = [] (const std::string& provider,
                             const std::vector<uint8_t>& key) {
    std::vector<uint8_t> wrapped(provider.begin(), provider.end());
    wrapped.push_back(':');
    wrapped.insert(wrapped.end(), key.begin(), key.end());
    return wrapped;
  };
  const auto signingKey = std::vector<uint8_t>(32, 0x37);
  auto sign = [signingKey] (const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> signedBytes = signingKey;
    signedBytes.insert(signedBytes.end(), bytes.begin(), bytes.end());
    std::vector<uint8_t> result(32, 0);
    for (std::size_t i = 0; i < signedBytes.size(); ++i) {
      result[i % result.size()] ^= signedBytes[i];
    }
    return result;
  };
  options.signCapability = sign;
  options.signManifest = sign;
  options.verifyCapability = [sign] (const std::vector<uint8_t>& bytes,
                                     const std::vector<uint8_t>& signature) {
    return sign(bytes) == signature;
  };
  options.verifyManifest = options.verifyCapability;
  return options;
}

GroupOperationV1
makeGroupOperation()
{
  GroupOperationV1 operation;
  operation.operationIndex = 4;
  operation.kind = "ALL_GATHER";
  operation.producerRanks = {"0"};
  operation.consumerRanks = {"1"};
  operation.tensorLayoutDigest = "layout-digest";
  operation.maxBytes = 16;
  operation.maxSegments = 4;
  return operation;
}

} // namespace

BOOST_AUTO_TEST_SUITE(DistributedInferenceCrossProviderGroup)

BOOST_AUTO_TEST_CASE(SegmentRoundTripAuthenticatesAndDecrypts)
{
  const auto descriptor = makeDescriptor();
  const auto key = makeKey();
  const std::vector<uint8_t> plaintext{'a', 'b', 'c'};
  const auto segment = NdnsfCollectiveControl::seal(
    descriptor, "/provider/P0/NDNSF-DI/COLLECTIVE/v1/segment/0", key, plaintext);

  BOOST_CHECK_EQUAL(segment.magic, NDNSF_DATA_V1);
  BOOST_REQUIRE(!segment.nonce.empty());
  BOOST_REQUIRE(!segment.ciphertext.empty());
  BOOST_REQUIRE(!segment.authTag.empty());
  BOOST_REQUIRE(!segment.hmac.empty());

  const auto encoded = segment.wireEncode();
  const auto decoded = NdnsfDataV1Segment::wireDecode(encoded);
  BOOST_CHECK_EQUAL(decoded.dataName, segment.dataName);
  BOOST_CHECK_EQUAL(decoded.descriptor.segmentNo, 0);
  BOOST_CHECK_EQUAL_COLLECTIONS(decoded.ciphertext.begin(), decoded.ciphertext.end(),
                                segment.ciphertext.begin(), segment.ciphertext.end());

  const auto recovered = NdnsfCollectiveControl::open(
    decoded, key, segment.dataName);
  BOOST_CHECK_EQUAL_COLLECTIONS(recovered.begin(), recovered.end(),
                                plaintext.begin(), plaintext.end());
}

BOOST_AUTO_TEST_CASE(TamperingAndEpochChangeFailClosed)
{
  const auto descriptor = makeDescriptor();
  const auto key = makeKey();
  auto segment = NdnsfCollectiveControl::seal(
    descriptor, "/provider/P0/NDNSF-DI/COLLECTIVE/v1/segment/0", key,
    std::vector<uint8_t>{'a', 'b', 'c'});

  segment.ciphertext.front() ^= 0x01;
  BOOST_CHECK_THROW(NdnsfCollectiveControl::open(segment, key, segment.dataName),
                    std::runtime_error);

  segment = NdnsfCollectiveControl::seal(
    descriptor, "/provider/P0/NDNSF-DI/COLLECTIVE/v1/segment/0", key,
    std::vector<uint8_t>{'a', 'b', 'c'});
  segment.descriptor.epoch++;
  BOOST_CHECK_THROW(NdnsfCollectiveControl::open(segment, key, segment.dataName),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ReplayWindowMakesIdenticalDuplicateIdempotent)
{
  const auto descriptor = makeDescriptor();
  const auto key = makeKey();
  const auto dataName = std::string("/provider/P0/NDNSF-DI/COLLECTIVE/v1/segment/0");
  const auto segment = NdnsfCollectiveControl::seal(
    descriptor, dataName, key, std::vector<uint8_t>{'a', 'b', 'c'});
  auto alteredDescriptor = descriptor;
  alteredDescriptor.segmentNo = 1;
  const auto second = NdnsfCollectiveControl::seal(
    alteredDescriptor, "/provider/P0/NDNSF-DI/COLLECTIVE/v1/segment/1", key,
    std::vector<uint8_t>{'d', 'e', 'f'});

  DataSegmentReplayWindow window(2, 6);
  BOOST_CHECK(window.accept(segment, key, dataName) ==
              DataSegmentReplayWindow::Result::Accepted);
  BOOST_CHECK(window.accept(segment, key, dataName) ==
              DataSegmentReplayWindow::Result::Duplicate);
  BOOST_CHECK(window.accept(second, key, second.dataName) ==
              DataSegmentReplayWindow::Result::Accepted);
  BOOST_CHECK(window.complete());
  BOOST_CHECK_EQUAL(window.acceptedSegments(), 2);

  auto conflicting = NdnsfCollectiveControl::seal(
    descriptor, dataName, key, std::vector<uint8_t>{'x', 'y', 'z'});
  BOOST_CHECK_THROW(window.accept(conflicting, key, dataName), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(BoundsAreValidatedBeforeAllocation)
{
  auto descriptor = makeDescriptor();
  descriptor.segmentCount = (1U << 20) + 1;
  BOOST_CHECK_THROW(
    NdnsfCollectiveControl::seal(descriptor, "/data/0", makeKey(),
                                  std::vector<uint8_t>{'a', 'b', 'c'}),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CoordinatorSealsAndVerifiesBoundedOperation)
{
  auto options = makeCoordinatorOptions();
  ProviderGroupCoordinator producer(options);
  const auto capability = producer.createCapability(
    "/request/one", "attempt-1", "sha256:plan", "group-1", 9,
    {{"/provider/P0", 0, "offer-0", "/provider/P0/NDNSF-DI"},
     {"/provider/P1", 1, "offer-1", "/provider/P1/NDNSF-DI"}},
    {makeGroupOperation()}, 32, 100, 500);
  BOOST_CHECK_EQUAL(capability.wrappedEpochKeyByProvider.size(), 2);
  BOOST_CHECK(!capability.capabilityDigest.empty());
  BOOST_CHECK(!capability.sealerSignature.empty());

  auto consumerOptions = makeCoordinatorOptions();
  ProviderGroupCoordinator consumer(consumerOptions);
  BOOST_CHECK_THROW(consumer.installCapability(capability, makeKey(), false),
                    std::runtime_error);
  // The deterministic test CSPRNG creates a known producer key. Install the
  // same key in the receiver to model successful Provider certificate unwrap.
  consumer.installCapability(capability, std::vector<uint8_t>(32, 0x5a), true);

  const auto sealed = producer.sealOperation(
    makeGroupOperation(), "0", "src-layout", "dst-layout", "tensor-digest",
    {{'a', 'b', 'c'}, {'d', 'e'}}, 1000);
  BOOST_REQUIRE_EQUAL(sealed.segments.size(), 2);
  BOOST_CHECK_EQUAL(sealed.manifest.segmentCount, 2);
  BOOST_CHECK(!sealed.manifest.producerSignature.empty());
  BOOST_CHECK_EQUAL(sealed.segments[0].nonce.size(), 12);
  BOOST_CHECK(sealed.segments[0].nonce != sealed.segments[1].nonce);
  BOOST_CHECK_EQUAL(sealed.segments[0].dataName,
                    ProviderGroupCoordinator::makeDataName(
                      capability, sealed.manifest, 0));

  BOOST_CHECK(consumer.acceptSegment(sealed.manifest, sealed.segments[1]) ==
              DataSegmentReplayWindow::Result::Accepted);
  BOOST_CHECK(consumer.acceptSegment(sealed.manifest, sealed.segments[0]) ==
              DataSegmentReplayWindow::Result::Accepted);
  BOOST_CHECK(consumer.acceptSegment(sealed.manifest, sealed.segments[0]) ==
              DataSegmentReplayWindow::Result::Duplicate);
  BOOST_CHECK(sealed.segments[0].dataName.find("//") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(CapabilityProjectionDisclosesOnlyLocalWrappedKey)
{
  ProviderGroupCoordinator producer(makeCoordinatorOptions());
  const auto capability = producer.createCapability(
    "/request/projected", "attempt-1", "sha256:plan", "group-1", 9,
    {{"/provider/P0", 0, "offer-0", "/provider/P0/NDNSF-DI"},
     {"/provider/P1", 1, "offer-1", "/provider/P1/NDNSF-DI"}},
    {makeGroupOperation()}, 32, 100, 500);

  const auto p0 = capability.projectForProvider("/provider/P0");
  const auto p1 = capability.projectForProvider("/provider/P1");
  BOOST_REQUIRE_EQUAL(p0.wrappedEpochKeyByProvider.size(), 1U);
  BOOST_REQUIRE_EQUAL(p1.wrappedEpochKeyByProvider.size(), 1U);
  BOOST_CHECK(p0.wrappedEpochKeyByProvider.count("/provider/P0") == 1U);
  BOOST_CHECK(p1.wrappedEpochKeyByProvider.count("/provider/P1") == 1U);
  BOOST_CHECK(p0.wrappedEpochKeyDigestByProvider ==
              p1.wrappedEpochKeyDigestByProvider);
  BOOST_CHECK_EQUAL(p0.capabilityDigest, p1.capabilityDigest);
  BOOST_CHECK_EQUAL_COLLECTIONS(
    p0.sealerSignature.begin(), p0.sealerSignature.end(),
    p1.sealerSignature.begin(), p1.sealerSignature.end());
  const auto p0Canonical = p0.canonicalBytes(true);
  const auto p1Canonical = p1.canonicalBytes(true);
  BOOST_CHECK_EQUAL_COLLECTIONS(
    p0Canonical.begin(), p0Canonical.end(),
    p1Canonical.begin(), p1Canonical.end());

  const auto decoded = ProviderGroupCoordinator::decodeCapability(
    ProviderGroupCoordinator::encodeCapability(p0));
  BOOST_CHECK(decoded.wrappedEpochKeyByProvider.count("/provider/P0") == 1U);
  BOOST_CHECK(decoded.wrappedEpochKeyByProvider.count("/provider/P1") == 0U);
  BOOST_CHECK(decoded.wrappedEpochKeyDigestByProvider ==
              capability.wrappedEpochKeyDigestByProvider);
}

BOOST_AUTO_TEST_CASE(OperationKeyAndNonceBindExactTensorAndDataName)
{
  ProviderGroupCoordinator producer(makeCoordinatorOptions());
  const auto capability = producer.createCapability(
    "/request/crypto-domain", "attempt-1", "sha256:plan", "group-1", 9,
    {{"/provider/P0", 0, "offer-0", "/provider/P0/NDNSF-DI"},
     {"/provider/P1", 1, "offer-1", "/provider/P1/NDNSF-DI"}},
    {makeGroupOperation()}, 32, 100, 500);
  const auto first = producer.sealOperation(
    makeGroupOperation(), "0", "src", "dst", "tensor-a",
    {{'a', 'b'}}, 1000, {"/provider/P0/tensor-a/seg=0"});
  const auto second = producer.sealOperation(
    makeGroupOperation(), "0", "src", "dst", "tensor-b",
    {{'c', 'd'}}, 1001, {"/provider/P0/tensor-b/seg=0"});
  const auto epochKey = std::vector<std::uint8_t>(32, 0x5a);

  BOOST_CHECK(
    ProviderGroupCoordinator::deriveOperationKey(
      epochKey, capability, first.manifest) !=
    ProviderGroupCoordinator::deriveOperationKey(
      epochKey, capability, second.manifest));
  BOOST_CHECK(
    ProviderGroupCoordinator::deriveNonce(
      capability, first.manifest, first.segments[0].dataName, 0) !=
    ProviderGroupCoordinator::deriveNonce(
      capability, second.manifest, second.segments[0].dataName, 0));
  BOOST_CHECK(
    ProviderGroupCoordinator::deriveNonce(
      capability, first.manifest, "/provider/P0/other-name/seg=0", 0) !=
    ProviderGroupCoordinator::deriveNonce(
      capability, first.manifest, first.segments[0].dataName, 0));
}

BOOST_AUTO_TEST_CASE(UndeclaredProducerRankNeverProducesTensorData)
{
  ProviderGroupCoordinator producer(makeCoordinatorOptions());
  producer.createCapability(
    "/request/wrong-rank", "attempt-1", "sha256:plan", "group-1", 9,
    {{"/provider/P0", 0, "offer-0", "/provider/P0/NDNSF-DI"},
     {"/provider/P1", 1, "offer-1", "/provider/P1/NDNSF-DI"}},
    {makeGroupOperation()}, 32, 100, 500);

  BOOST_CHECK_THROW(
    producer.sealOperation(
      makeGroupOperation(), "1", "src", "dst", "tensor-a",
      {{'a', 'b'}}, 1000),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CoordinatorBindsPlanDeclaredExactNamesAndManifestAuthenticator)
{
  auto options = makeCoordinatorOptions();
  ProviderGroupCoordinator producer(options);
  const auto capability = producer.createCapability(
    "/request/one", "attempt-1", "sha256:plan", "group-1", 9,
    {{"/provider/P0", 0, "offer-0", "/provider/P0/NDNSF-DI"},
     {"/provider/P1", 1, "offer-1", "/provider/P1/NDNSF-DI"}},
    {makeGroupOperation()}, 32, 100, 500);
  const std::vector<std::string> names{
    "/provider/P0/NDNSF-DI/TENSOR/v1/object/SEG/seg=0",
    "/provider/P0/NDNSF-DI/TENSOR/v1/object/SEG/seg=1",
  };
  const auto sealed = producer.sealOperation(
    makeGroupOperation(), "0", "src-layout", "dst-layout", "tensor-digest",
    {{'a', 'b', 'c'}, {'d', 'e'}}, 1000, names);
  BOOST_REQUIRE_EQUAL(sealed.segments.size(), names.size());
  BOOST_CHECK_EQUAL(sealed.segments[0].dataName, names[0]);
  BOOST_CHECK_EQUAL(sealed.segments[1].dataName, names[1]);

  const std::vector<std::uint8_t> manifestBytes{'m', 'a', 'n', 'i', 'f', 'e', 's', 't'};
  const auto signature = producer.signTensorObjectManifest(manifestBytes);
  BOOST_CHECK(producer.verifyTensorObjectManifest(manifestBytes, signature));
  auto changed = manifestBytes;
  changed.front() ^= 1;
  BOOST_CHECK(!producer.verifyTensorObjectManifest(changed, signature));

  auto consumerOptions = makeCoordinatorOptions();
  ProviderGroupCoordinator consumer(consumerOptions);
  consumer.installCapability(capability, std::vector<uint8_t>(32, 0x5a), true);
  BOOST_CHECK_NO_THROW(consumer.openSegment(
    sealed.manifest, sealed.segments[0], names[0]));
  BOOST_CHECK_THROW(consumer.openSegment(
    sealed.manifest, sealed.segments[0], names[1]), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(CoordinatorUsesRsaWrappedEpochKeyAndWireBundle)
{
  ndn::security::KeyChain keyChain("pib-memory:provider-group-rsa",
                                   "tpm-memory:provider-group-rsa");
  const auto p0 = keyChain.createIdentity(ndn::Name("/provider/P0"),
                                          ndn::RsaKeyParams(2048))
                    .getDefaultKey().getDefaultCertificate();
  const auto p1 = keyChain.createIdentity(ndn::Name("/provider/P1"),
                                          ndn::RsaKeyParams(2048))
                    .getDefaultKey().getDefaultCertificate();
  const std::map<std::string, ndn::security::Certificate> certificates{
    {"/provider/P0", p0}, {"/provider/P1", p1}};

  auto options = makeCoordinatorOptions();
  options.localProvider = "/provider/P1";
  options.wrapEpochKey = [certificates] (const std::string& provider,
                                         const std::vector<uint8_t>& key) {
    const auto publicKey = certificates.at(provider).getPublicKey();
    const auto wrapped = ndn_service_framework::wrapSelectionGatedInputKey(
      ndn::Buffer(key.data(), key.size()),
      ndn::span<const uint8_t>(publicKey.data(), publicKey.size()));
    return std::vector<uint8_t>(wrapped.begin(), wrapped.end());
  };
  options.unwrapEpochKey = [&keyChain, certificates] (
                             const std::string& provider,
                             const std::vector<uint8_t>& wrapped) {
    const auto plain = ndn_service_framework::unwrapSelectionGatedInputKey(
      ndn::Buffer(wrapped.data(), wrapped.size()),
      certificates.at(provider).getName(), keyChain);
    return std::vector<uint8_t>(plain.begin(), plain.end());
  };

  ProviderGroupCoordinator producer(options);
  const auto capability = producer.createCapability(
    "/request/one", "attempt-1", "sha256:plan", "group-1", 11,
    {{"/provider/P0", 0, "offer-0", "/provider/P0/NDNSF-DI"},
     {"/provider/P1", 1, "offer-1", "/provider/P1/NDNSF-DI"}},
    {makeGroupOperation()}, 32, 100, 500);
  BOOST_REQUIRE_EQUAL(capability.wrappedEpochKeyByProvider.at("/provider/P0").size(),
                      256U);
  BOOST_REQUIRE_EQUAL(capability.wrappedEpochKeyByProvider.at("/provider/P1").size(),
                      256U);

  const auto capabilityWire =
    ProviderGroupCoordinator::encodeCapability(capability);
  const auto decodedCapability =
    ProviderGroupCoordinator::decodeCapability(capabilityWire);
  BOOST_CHECK_EQUAL(decodedCapability.capabilityDigest,
                    capability.capabilityDigest);
  BOOST_CHECK_EQUAL(decodedCapability.requestId, capability.requestId);
  BOOST_CHECK_EQUAL(decodedCapability.orderedMembers.size(), 2U);
  BOOST_CHECK_EQUAL(decodedCapability.permittedOperations.size(), 1U);
  BOOST_CHECK_EQUAL_COLLECTIONS(
    decodedCapability.sealerSignature.begin(),
    decodedCapability.sealerSignature.end(),
    capability.sealerSignature.begin(),
    capability.sealerSignature.end());
  auto trailingCapabilityWire = capabilityWire;
  trailingCapabilityWire.push_back(0xff);
  BOOST_CHECK_THROW(
    ProviderGroupCoordinator::decodeCapability(trailingCapabilityWire),
    std::invalid_argument);

  ProviderGroupCoordinator consumer(options);
  // An empty local key forces the Provider-specific RSA unwrap callback.
  consumer.installCapability(
    capability.projectForProvider("/provider/P1"), {}, true);
  const auto unwrapped = consumer.epochKeyForProvider("/provider/P1");
  BOOST_REQUIRE_EQUAL(unwrapped.size(), 32U);
  BOOST_CHECK_THROW(consumer.epochKeyForProvider("/provider/P0"),
                    std::runtime_error);

  const auto sealed = producer.sealOperation(
    makeGroupOperation(), "0", "src", "dst", "tensor",
    {{'a', 'b', 'c'}, {'d', 'e'}}, 1000);
  const auto wire = ProviderGroupCoordinator::encodeOperation(sealed);
  const auto decoded = ProviderGroupCoordinator::decodeOperation(wire);
  BOOST_REQUIRE_EQUAL(decoded.segments.size(), 2U);
  BOOST_CHECK_EQUAL(decoded.manifest.digest(), sealed.manifest.digest());
  for (size_t index = 0; index < sealed.segments.size(); ++index) {
    const auto segmentWire = ProviderGroupCoordinator::encodeSegment(
      sealed.manifest, sealed.segments[index]);
    const auto decodedSegment = ProviderGroupCoordinator::decodeSegment(segmentWire);
    BOOST_REQUIRE_EQUAL(decodedSegment.segments.size(), 1U);
    BOOST_CHECK_EQUAL(decodedSegment.manifest.digest(), sealed.manifest.digest());
    BOOST_CHECK_EQUAL(decodedSegment.segments.front().dataName,
                      sealed.segments[index].dataName);
  }
  BOOST_CHECK(consumer.acceptSegment(decoded.manifest, decoded.segments[1]) ==
              DataSegmentReplayWindow::Result::Accepted);
  BOOST_CHECK(consumer.acceptSegment(decoded.manifest, decoded.segments[0]) ==
              DataSegmentReplayWindow::Result::Accepted);

  auto tampered = decoded;
  tampered.segments.front().ciphertext.front() ^= 0x01;
  BOOST_CHECK_THROW(consumer.openSegment(tampered.manifest,
                                         tampered.segments.front()),
                    std::exception);

  // Production default: RSA delivers the request-scoped epoch key to each
  // selected Provider; the inner capability/manifest authenticators use that
  // key when no application-specific public-key signer is configured.
  auto hmacOptions = options;
  hmacOptions.signCapability = {};
  hmacOptions.verifyCapability = {};
  hmacOptions.signManifest = {};
  hmacOptions.verifyManifest = {};
  ProviderGroupCoordinator hmacProducer(hmacOptions);
  const auto hmacCapability = hmacProducer.createCapability(
    "/request/hmac", "attempt-1", "sha256:plan", "group-hmac", 12,
    {{"/provider/P0", 0, "offer-0", "/provider/P0/NDNSF-DI"},
     {"/provider/P1", 1, "offer-1", "/provider/P1/NDNSF-DI"}},
    {makeGroupOperation()}, 32, 100, 500);
  ProviderGroupCoordinator hmacConsumer(hmacOptions);
  hmacConsumer.installCapability(
    hmacCapability.projectForProvider("/provider/P1"), {}, true);
  const auto hmacSealed = hmacProducer.sealOperation(
    makeGroupOperation(), "0", "src", "dst", "tensor",
    {{'h', 'm'}, {'a', 'c'}}, 1000);
  BOOST_CHECK(hmacConsumer.acceptSegment(
                hmacSealed.manifest, hmacSealed.segments.front()) ==
              DataSegmentReplayWindow::Result::Accepted);
  auto badCapability = hmacCapability.projectForProvider("/provider/P1");
  badCapability.sealerSignature.front() ^= 0x01;
  ProviderGroupCoordinator rejectingConsumer(hmacOptions);
  BOOST_CHECK_THROW(rejectingConsumer.installCapability(
                      std::move(badCapability), {}, true),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(CoordinatorRejectsManifestMutationAndTerminalReuse)
{
  auto operation = makeGroupOperation();
  operation.consumerRanks = {"0"};
  ProviderGroupCoordinator producer(makeCoordinatorOptions());
  const auto capability = producer.createCapability(
    "request", "attempt", "plan", "group", 1,
    {{"/provider/P0", 0, "offer", "/p0"}},
    {operation}, 32, 10, 20);
  const auto sealed = producer.sealOperation(
    operation, "0", "src", "dst", "tensor", {{'x', 'y'}}, 100);
  auto consumerOptions = makeCoordinatorOptions();
  ProviderGroupCoordinator consumer(consumerOptions);
  consumer.installCapability(capability, std::vector<uint8_t>(32, 0x5a), true);

  auto altered = sealed.manifest;
  altered.operationKind = "REDUCE_SCATTER";
  BOOST_CHECK_THROW(consumer.openSegment(altered, sealed.segments.front()),
                    std::runtime_error);

  consumer.cancel("test-cancel");
  BOOST_CHECK(consumer.cancelled());
  BOOST_CHECK(consumer.terminal());
  BOOST_CHECK_THROW(consumer.epochKeyForProvider("/provider/P0"),
                    std::runtime_error);
  BOOST_CHECK_THROW(consumer.openSegment(sealed.manifest, sealed.segments.front()),
                    std::runtime_error);

  ProviderGroupCoordinator failedConsumer(consumerOptions);
  failedConsumer.installCapability(capability, std::vector<uint8_t>(32, 0x5a), true);
  failedConsumer.fail("test-fail");
  BOOST_CHECK(failedConsumer.failed());
  BOOST_CHECK_THROW(failedConsumer.epochKeyForProvider("/provider/P0"),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(CoordinatorEnforcesProgressAndHardDeadline)
{
  const auto prepare = [] (ProviderGroupCoordinator& consumer,
                           std::uint64_t noProgressMs,
                           std::uint64_t hardDeadlineMs) {
    auto operation = makeGroupOperation();
    operation.consumerRanks = {"0"};
    ProviderGroupCoordinator producer(makeCoordinatorOptions());
    const auto capability = producer.createCapability(
      "request", "attempt", "plan", "group", 1,
      {{"/provider/P0", 0, "offer", "/p0"}},
      {operation}, 32, noProgressMs, hardDeadlineMs);
    const auto sealed = producer.sealOperation(
      operation, "0", "src", "dst", "tensor", {{'x', 'y'}}, 100);

    consumer.installCapability(capability, std::vector<std::uint8_t>(32, 0x5a), true);
    BOOST_REQUIRE(consumer.acceptSegment(
                    sealed.manifest, sealed.segments.front()) ==
                  DataSegmentReplayWindow::Result::Accepted);
    return sealed;
  };

  ProviderGroupCoordinator noProgress(makeCoordinatorOptions());
  prepare(noProgress, 10, 100);
  BOOST_CHECK(!noProgress.recordProgress(111));
  BOOST_CHECK(noProgress.failed());
  BOOST_CHECK_EQUAL(noProgress.terminalReason(),
                    "NDNSF_DATA_V1_NO_PROGRESS");

  ProviderGroupCoordinator hardDeadline(makeCoordinatorOptions());
  prepare(hardDeadline, 100, 200);
  BOOST_CHECK(!hardDeadline.recordProgress(300));
  BOOST_CHECK(hardDeadline.failed());
  BOOST_CHECK_EQUAL(hardDeadline.terminalReason(),
                    "NDNSF_DATA_V1_HARD_DEADLINE");
}

BOOST_AUTO_TEST_CASE(FixedFiftySeedDataV1FaultMatrix)
{
  for (std::uint64_t seed = 0; seed < 50; ++seed) {
    auto operation = makeGroupOperation();
    operation.consumerRanks = {"0"};
    operation.maxBytes = 64;
    operation.maxSegments = 3;

    ProviderGroupCoordinator producer(makeCoordinatorOptions());
    const auto capability = producer.createCapability(
      "/request/fault-matrix/" + std::to_string(seed),
      "attempt-1", "plan-fault-matrix", "group-fault-matrix", seed + 1,
      {{"/provider/P0", 0, "offer", "/p0"}}, {operation}, 128, 10, 1000);
    ProviderGroupCoordinator consumer(makeCoordinatorOptions());
    consumer.installCapability(capability, std::vector<std::uint8_t>(32, 0x5a), true);

    const std::vector<ProviderGroupBytes> plaintextSegments{
      {'p', 'l', 'a', 'i', 'n', 't', 'e', 'x', 't', '-', '0', '0'},
      {'p', 'l', 'a', 'i', 'n', 't', 'e', 'x', 't', '-', '1', '1'},
      {'p', 'l', 'a', 'i', 'n', 't', 'e', 'x', 't', '-', '2', '2'}};
    const auto sealed = producer.sealOperation(
      operation, "0", "src", "dst", "tensor-fault-matrix-" +
      std::to_string(seed),
      plaintextSegments, 100);
    BOOST_REQUIRE_EQUAL(sealed.segments.size(), 3U);
    BOOST_CHECK(sealed.segments[0].nonce != sealed.segments[1].nonce);
    BOOST_CHECK(sealed.segments[0].nonce != sealed.segments[2].nonce);
    BOOST_CHECK(sealed.segments[1].nonce != sealed.segments[2].nonce);

    std::vector<std::size_t> order{0, 1, 2};
    std::rotate(order.begin(), order.begin() + (seed % order.size()), order.end());
    if ((seed & 1U) != 0) {
      std::swap(order[0], order[1]);
    }

    const auto dropped = static_cast<std::size_t>(seed % sealed.segments.size());
    const bool shouldDrop = (seed % 4U) == 0;
    std::size_t accepted = 0;
    for (const auto index : order) {
      const auto wire = ProviderGroupCoordinator::encodeSegment(
        sealed.manifest, sealed.segments[index]);
      BOOST_CHECK(std::search(wire.begin(), wire.end(),
                              plaintextSegments[index].begin(),
                              plaintextSegments[index].end()) == wire.end());

      if (shouldDrop && index == dropped) {
        continue;
      }

      auto candidate = sealed.segments[index];
      if ((seed % 5U) == 0 && index == order.front()) {
        candidate.ciphertext.front() ^= 0x01;
        BOOST_CHECK_THROW(consumer.acceptSegment(sealed.manifest, candidate),
                          std::exception);
        candidate = sealed.segments[index];
      }
      BOOST_CHECK(consumer.acceptSegment(sealed.manifest, candidate) ==
                  DataSegmentReplayWindow::Result::Accepted);
      ++accepted;
    }

    if (shouldDrop) {
      BOOST_CHECK_EQUAL(accepted, 2U);
      BOOST_CHECK(!consumer.recordProgress(111));
      BOOST_CHECK(consumer.failed());
      BOOST_CHECK_EQUAL(consumer.terminalReason(),
                        "NDNSF_DATA_V1_NO_PROGRESS");
    }
    else {
      BOOST_CHECK_EQUAL(accepted, 3U);
      BOOST_CHECK(consumer.acceptSegment(
                    sealed.manifest, sealed.segments[order.front()]) ==
                  DataSegmentReplayWindow::Result::Duplicate);
      BOOST_CHECK(!consumer.terminal());
    }
  }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
