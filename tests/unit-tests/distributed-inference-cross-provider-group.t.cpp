#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollectiveControl.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderGroupCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"
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

// Independent V1 reference writer. Do not call the production serializer or
// its compact-run iterator: this fixes the legacy field order and integer wire.
ProviderGroupBytes
legacyCapabilityBytes(const GroupCapabilityV1& value,
                      const std::vector<GroupOperationV1>& operations)
{
  ProviderGroupBytes bytes;
  const auto word = [&](std::uint64_t number) {
    for (int shift = 56; shift >= 0; shift -= 8)
      bytes.push_back(static_cast<std::uint8_t>(number >> shift));
  };
  const auto text = [&](const std::string& string) {
    word(string.size());
    bytes.insert(bytes.end(), string.begin(), string.end());
  };
  text("GroupCapabilityV1");
  text(value.requestId); text(value.attemptId); text(value.planDigest); text(value.groupId);
  word(value.epoch); word(value.orderedMembers.size());
  for (const auto& member : value.orderedMembers) {
    text(member.provider); word(member.rank); text(member.offerDigest); text(member.endpointPrefix);
  }
  word(operations.size());
  for (const auto& operation : operations) {
    word(operation.operationIndex); text(operation.kind);
    word(operation.producerRanks.size());
    for (const auto& rank : operation.producerRanks) text(rank);
    word(operation.consumerRanks.size());
    for (const auto& rank : operation.consumerRanks) text(rank);
    text(operation.tensorLayoutDigest); word(operation.maxBytes); word(operation.maxSegments);
  }
  word(value.maxInflightBytes); word(value.noProgressMs); word(value.hardDeadlineMs);
  text(value.epochKeyId); word(value.wrappedEpochKeyDigestByProvider.size());
  for (const auto& commitment : value.wrappedEpochKeyDigestByProvider) {
    text(commitment.first); text(commitment.second);
  }
  text(value.capabilityDigest);
  return bytes;
}

} // namespace


// These explicit names exercise the embedded crypto codec only. No Data is
// published here; the C04 publication oracle uses real native endpoints.
SealedCollectiveOperationV1
sealCryptoFixture(ProviderGroupCoordinator& owner, const GroupOperationV1& operation,
                  const std::string& rank, const std::string& sourceLayout,
                  const std::string& targetLayout, const std::string& tensor,
                  const std::vector<ProviderGroupBytes>& plaintext, std::uint64_t now,
                  std::vector<std::string> names = {})
{
  if (names.empty()) {
    const auto& members = owner.capability().orderedMembers;
    const auto member = std::find_if(members.begin(), members.end(),
      [&](const auto& value) { return std::to_string(value.rank) == rank; });
    if (member == members.end()) throw std::invalid_argument("fixture rank has no Producer");
    for (std::size_t index = 0; index < plaintext.size(); ++index) {
      ndn::Name name(member->provider);
      name.append("NDNSF-DI").append("crypto-fixture");
      name.append(owner.capability().capabilityDigest).append(std::to_string(now));
      name.append(ndn::name::Component(ndn::tlv::GenericNameComponent,
                                      tensor.begin(), tensor.end()));
      name.appendSegment(index);
      names.push_back(name.toUri(ndn::name::UriFormat::CANONICAL));
    }
  }
  return owner.sealOperation(operation, rank, sourceLayout, targetLayout, tensor,
                             plaintext, now, names);
}

BOOST_AUTO_TEST_SUITE(DistributedInferenceCrossProviderGroup)

BOOST_AUTO_TEST_CASE(CapabilityWorkObserversExcludeCallbacksAndReleaseOwner)
{
  NativeWorkClock now{0, 0};
  auto statistics = std::make_shared<NativeWorkStatistics>([&] { return now; });
  const auto advance = [&] (std::int64_t wall) {
    now.wallNs += wall;
    now.threadCpuNs += wall / 10;
  };
  auto options = makeCoordinatorOptions();
  const auto random = options.randomBytes;
  const auto wrap = options.wrapEpochKey;
  const auto sign = options.signCapability;
  options.randomBytes = [&, random] (std::size_t size) {
    advance(100);
    return random(size);
  };
  options.wrapEpochKey = [&, wrap] (const auto& provider, const auto& key) {
    advance(200);
    return wrap(provider, key);
  };
  options.signCapability = [&, sign] (const auto& bytes) {
    advance(300);
    return sign(bytes);
  };
  const auto create = [] (ProviderGroupCoordinator& owner) {
    return owner.createCapabilityFromRuns(
      "/request/observed", "attempt-1", "sha256:plan", "group-1", 9,
      {{"/provider/P0", 0, "offer-0", "/provider/P0/NDNSF-DI"},
       {"/provider/P1", 1, "offer-1", "/provider/P1/NDNSF-DI"}},
      {{makeGroupOperation(), 2, 8}}, 32, 100, 500);
  };
  ProviderGroupCoordinator plain(options);
  const auto expected = create(plain);
  options.workStatistics = statistics;
  ProviderGroupCoordinator producer(options);
  const auto actual = create(producer);
  BOOST_CHECK(ProviderGroupCoordinator::encodeCapability(actual) ==
              ProviderGroupCoordinator::encodeCapability(expected));

  auto consumerOptions = makeCoordinatorOptions();
  consumerOptions.workStatistics = statistics;
  consumerOptions.localProvider = "/provider/P1";
  consumerOptions.unwrapEpochKey = [&] (const auto&, const auto& wrapped) {
    advance(400);
    return ProviderGroupBytes(wrapped.end() - 32, wrapped.end());
  };
  const auto verify = consumerOptions.verifyCapability;
  consumerOptions.verifyCapability = [&, verify] (const auto& bytes, const auto& signature) {
    advance(500);
    return verify(bytes, signature);
  };
  ProviderGroupCoordinator consumer(consumerOptions);
  consumer.installCapability(actual.projectForProvider("/provider/P1"), {}, true);
  const auto operation = actual.operation(12);
  const auto sealed = sealCryptoFixture(producer, operation, "0", "source", "target",
                                        "tensor", {{1, 2, 3}}, 1000);
  BOOST_REQUIRE_EQUAL(sealed.segments.size(), 1U);
  BOOST_CHECK(consumer.openSegment(sealed.manifest, sealed.segments.front(),
                                  sealed.segments.front().dataName) == ProviderGroupBytes({1, 2, 3}));
  const auto observed = statistics->snapshot();
  BOOST_CHECK(observed.complete);
  BOOST_CHECK_EQUAL(observed.activeSpans, 0U);
  BOOST_CHECK_EQUAL(observed.phases.at("group-key-random").exclusiveWallNs, 100U);
  BOOST_CHECK_EQUAL(observed.phases.at("group-key-wrap").exclusiveWallNs, 400U);
  BOOST_CHECK_EQUAL(observed.phases.at("group-capability-sign").exclusiveWallNs, 300U);
  BOOST_CHECK_EQUAL(observed.phases.at("group-key-unwrap").exclusiveWallNs, 400U);
  BOOST_CHECK_EQUAL(observed.phases.at("group-capability-verify").exclusiveWallNs, 500U);
  BOOST_CHECK_EQUAL(observed.phases.at("group-capability-build").exclusiveWallNs, 0U);
  BOOST_CHECK_EQUAL(observed.phases.at("group-capability-install").exclusiveWallNs, 0U);
  BOOST_CHECK(observed.phases.at("group-key-lock").kind == NativeWorkKind::WAIT);
  BOOST_CHECK_GT(observed.phases.at("group-operation-lookup").calls, 0U);
  for (const auto& phase : observed.phases) {
    if (phase.second.kind == NativeWorkKind::LOCAL) {
      BOOST_CHECK_EQUAL(phase.second.exclusiveWallNs, 0U);
      BOOST_REQUIRE(phase.second.exclusiveThreadCpuNs);
      BOOST_CHECK_EQUAL(*phase.second.exclusiveThreadCpuNs, 0U);
    }
  }
  const std::weak_ptr<NativeWorkStatistics> weak = statistics;
  statistics.reset();
  BOOST_CHECK(weak.expired());
  const auto later = sealCryptoFixture(producer, actual.operation(4), "0", "source", "target",
                                       "after-collector", {{4, 5, 6}}, 1001);
  BOOST_REQUIRE_EQUAL(later.segments.size(), 1U);
  BOOST_CHECK(consumer.openSegment(later.manifest, later.segments.front(),
                                  later.segments.front().dataName) == ProviderGroupBytes({4, 5, 6}));

  auto failedStatistics = std::make_shared<NativeWorkStatistics>([&] { return now; });
  auto rejecting = options;
  rejecting.workStatistics = failedStatistics;
  rejecting.wrapEpochKey = [&] (const auto&, const auto&) -> ProviderGroupBytes {
    advance(700);
    throw std::runtime_error("intentional wrap failure");
  };
  ProviderGroupCoordinator failure(rejecting);
  BOOST_CHECK_EXCEPTION(create(failure), std::runtime_error, [] (const auto& error) {
    return std::string(error.what()) == "intentional wrap failure";
  });
  const auto failed = failedStatistics->snapshot();
  BOOST_CHECK(failed.complete);
  BOOST_CHECK_EQUAL(failed.activeSpans, 0U);
  BOOST_CHECK_EQUAL(failed.phases.at("group-key-wrap").exclusiveWallNs, 700U);
  BOOST_CHECK_EQUAL(failed.phases.at("group-capability-build").exclusiveWallNs, 0U);
}

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

BOOST_AUTO_TEST_CASE(RepeatedEpochCapabilityRetainsCanonicalAuthorityInCompactWire)
{
  for (const std::size_t epochs : {33U, 129U, 257U}) {
    std::vector<GroupOperationV1> operations;
    for (std::uint64_t direction = 0; direction < 2; ++direction) {
      for (std::uint64_t epoch = 0; epoch < epochs; ++epoch) {
        auto operation = makeGroupOperation();
        operation.operationIndex = direction + epoch * 4;
        if (direction != 0) {
          operation.kind = "TOKEN_FEEDBACK";
          operation.producerRanks = {"1"};
          operation.consumerRanks = {"0"};
        }
        operations.push_back(std::move(operation));
      }
    }
    ProviderGroupCoordinator producer(makeCoordinatorOptions());
    const auto capability = producer.createCapability(
      "/request/compact", "attempt-1", "sha256:plan", "group-1", 9,
      {{"/provider/P0", 0, "offer-0", "/provider/P0/NDNSF-DI"},
       {"/provider/P1", 1, "offer-1", "/provider/P1/NDNSF-DI"}},
      std::move(operations), 32, 100, 500).projectForProvider("/provider/P0");
    const auto wire = ProviderGroupCoordinator::encodeCapability(capability);
    const std::string marker = "GroupCapabilityV2";
    BOOST_REQUIRE_GT(wire.size(), 8 + marker.size());
    BOOST_CHECK_EQUAL(std::string(wire.begin() + 8, wire.begin() + 8 + marker.size()), marker);
    BOOST_CHECK_LT(wire.size() * 4, capability.canonicalBytes(true).size());
    const auto decoded = ProviderGroupCoordinator::decodeCapability(wire);
    BOOST_CHECK(decoded.canonicalBytes(true) == capability.canonicalBytes(true));
    BOOST_CHECK(decoded.sealerSignature == capability.sealerSignature);
    BOOST_CHECK_EQUAL(decoded.operationCount(), epochs * 2);
    BOOST_CHECK(decoded.permittedOperations.empty());
    BOOST_REQUIRE_EQUAL(decoded.operationRuns.size(), 2U);
    ProviderGroupCoordinator runProducer(makeCoordinatorOptions());
    const auto runCapability = runProducer.createCapabilityFromRuns(
      capability.requestId, capability.attemptId, capability.planDigest, capability.groupId,
      capability.epoch, capability.orderedMembers, decoded.operationRuns,
      capability.maxInflightBytes, capability.noProgressMs, capability.hardDeadlineMs)
        .projectForProvider("/provider/P0");
    BOOST_CHECK(runCapability.canonicalBytes(true) == capability.canonicalBytes(true));
    BOOST_CHECK(ProviderGroupCoordinator::encodeCapability(runCapability) == wire);
    ProviderGroupCoordinator runConsumer(makeCoordinatorOptions());
    runConsumer.installCapability(decoded, std::vector<uint8_t>(32, 0x5a), true);
    for (const auto epoch : {0U, static_cast<unsigned>(epochs / 2),
                             static_cast<unsigned>(epochs - 1)}) {
      for (unsigned direction = 0; direction != 2; ++direction) {
        const auto index = direction + epoch * 4;
        BOOST_CHECK_EQUAL(decoded.operation(index).operationIndex, index);
        BOOST_CHECK_EQUAL(decoded.operation(index).kind,
                          direction ? "TOKEN_FEEDBACK" : "ALL_GATHER");
        const auto operation = decoded.operation(index);
        const auto sealed = sealCryptoFixture(runProducer, operation,
          direction ? "1" : "0", "src-layout", "dst-layout",
          "tensor-" + std::to_string(index), {{1, 2, 3}}, 1000);
        BOOST_REQUIRE_EQUAL(sealed.segments.size(), 1U);
        const auto expectedName = ndn::Name(direction ? "/provider/P1" : "/provider/P0")
          .append("NDNSF-DI").append("crypto-fixture").append(runCapability.capabilityDigest)
          .append("1000").append("tensor-" + std::to_string(index)).appendSegment(0);
        BOOST_CHECK(ndn::Name(sealed.segments.front().dataName) == expectedName);
        BOOST_CHECK(runConsumer.openSegment(sealed.manifest, sealed.segments.front(),
                      expectedName.toUri(ndn::name::UriFormat::CANONICAL)) ==
                    ProviderGroupBytes({1, 2, 3}));
      }
    }
    BOOST_CHECK_THROW(decoded.operation(2), std::invalid_argument);
    std::size_t cursor = 0;
    const auto readWord = [&] {
      BOOST_REQUIRE_LE(cursor + 8, wire.size());
      std::uint64_t value = 0;
      for (unsigned byte = 0; byte < 8; ++byte) value = (value << 8) | wire[cursor++];
      return value;
    };
    const auto skipString = [&] {
      const auto size = readWord();
      BOOST_REQUIRE_LE(size, wire.size() - cursor);
      cursor += size;
    };
    for (unsigned field = 0; field < 5; ++field) skipString();
    readWord(); // epoch
    const auto members = readWord();
    for (std::uint64_t member = 0; member < members; ++member) {
      skipString();
      readWord();
      skipString();
      skipString();
    }
    const auto countOffset = cursor;
    readWord(); // expanded operation count
    readWord(); // run count
    const auto firstRun = cursor;
    const auto replaceWord = [](auto& bytes, std::size_t offset, std::uint64_t value) {
      for (unsigned byte = 0; byte < 8; ++byte)
        bytes.at(offset + byte) = static_cast<std::uint8_t>(value >> (56 - byte * 8));
    };
    for (const auto count : {std::uint64_t{0}, std::uint64_t{1} << 40}) {
      auto malformed = wire;
      replaceWord(malformed, firstRun, count);
      BOOST_CHECK_THROW(ProviderGroupCoordinator::decodeCapability(malformed), std::invalid_argument);
    }
    auto malformed = wire;
    replaceWord(malformed, firstRun + 8, 0);
    BOOST_CHECK_THROW(ProviderGroupCoordinator::decodeCapability(malformed), std::invalid_argument);
    malformed = wire;
    replaceWord(malformed, firstRun + 16, UINT64_MAX);
    BOOST_CHECK_THROW(ProviderGroupCoordinator::decodeCapability(malformed), std::invalid_argument);
    malformed = wire;
    replaceWord(malformed, countOffset, 1U << 20);
    replaceWord(malformed, firstRun, 1U << 20);
    BOOST_CHECK_THROW(ProviderGroupCoordinator::decodeCapability(malformed), std::invalid_argument);
    ProviderGroupCoordinator consumer(makeCoordinatorOptions());
    BOOST_CHECK_NO_THROW(consumer.installCapability(decoded, std::vector<uint8_t>(32, 0x5a), true));
    auto tampered = decoded;
    tampered.sealerSignature[0] ^= 1;
    BOOST_CHECK_THROW(consumer.installCapability(tampered, std::vector<uint8_t>(32, 0x5a), true),
                      std::runtime_error);
    tampered = decoded;
    tampered.requestId = "/request/replayed";
    BOOST_CHECK_THROW(consumer.installCapability(tampered, std::vector<uint8_t>(32, 0x5a), true),
                      std::runtime_error);
    auto truncated = wire;
    truncated.resize(truncated.size() / 2);
    BOOST_CHECK_THROW(ProviderGroupCoordinator::decodeCapability(truncated), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(LazyCapabilityRejectsAmbiguousRunsAndPreservesIrregularOrder)
{
  auto first = makeGroupOperation();
  first.operationIndex = 40;
  auto second = first;
  second.operationIndex = 4;
  std::vector<GroupOperationV1> expanded{first};
  for (unsigned offset = 0; offset != 3; ++offset) {
    auto operation = second;
    operation.operationIndex += 12 * offset;
    expanded.push_back(operation);
  }
  const std::vector<GroupMemberV1> members{
    {"/provider/P0", 0, "offer-0", "/provider/P0/NDNSF-DI"},
    {"/provider/P1", 1, "offer-1", "/provider/P1/NDNSF-DI"}};
  ProviderGroupCoordinator explicitOwner(makeCoordinatorOptions());
  const auto explicitCapability = explicitOwner.createCapability("/request/irregular",
    "attempt-1", "sha256:plan", "group-1", 9, members, expanded, 32, 100, 500);
  ProviderGroupCoordinator runOwner(makeCoordinatorOptions());
  const auto capability = runOwner.createCapabilityFromRuns("/request/irregular",
    "attempt-1", "sha256:plan", "group-1", 9, members,
    {{first, 1, 0}, {second, 3, 12}}, 32, 100, 500);
  BOOST_CHECK(capability.canonicalBytes(true) == explicitCapability.canonicalBytes(true));
  const auto independent = legacyCapabilityBytes(explicitCapability, expanded);
  BOOST_CHECK(explicitCapability.canonicalBytes(true) == independent);
  BOOST_CHECK(capability.canonicalBytes(true) == independent);
  for (const auto& operation : expanded) {
    const auto actual = capability.operation(operation.operationIndex);
    BOOST_CHECK_EQUAL(actual.operationIndex, operation.operationIndex);
    BOOST_CHECK_EQUAL(actual.tensorLayoutDigest, operation.tensorLayoutDigest);
  }
  BOOST_CHECK_THROW(capability.operation(5), std::invalid_argument);
  for (unsigned invalid = 0; invalid != 7; ++invalid) {
    auto malformed = capability;
    auto& run = malformed.operationRuns.back();
    switch (invalid) {
      case 0: run.count = 0; break;
      case 1: run.stride = 0; break;
      case 2: run.first.operationIndex = UINT64_MAX; break;
      case 3: run.count = (1U << 20) + 1ULL; break;
      case 4: malformed.permittedOperations = expanded; break;
      case 5: run.first.operationIndex = 40; break;
      case 6: run.first.producerRanks = {"foreign-rank"}; break;
    }
    BOOST_CHECK_THROW(malformed.validate(), std::invalid_argument);
  }
  auto singletonStride = capability;
  singletonStride.operationRuns.front().stride = 1;
  BOOST_CHECK_THROW(singletonStride.validate(), std::invalid_argument);
  auto oversized = capability;
  oversized.operationRuns = {{second, 1U << 20, 1}};
  BOOST_CHECK_THROW(oversized.canonicalBytes(), std::invalid_argument);
  auto oversizedHeader = capability;
  oversizedHeader.requestId.assign((16U << 20) + 1, 'x');
  BOOST_CHECK_THROW(oversizedHeader.canonicalBytes(), std::invalid_argument);
  const auto headerBytes = legacyCapabilityBytes(capability, {}).size();
  const auto operationBytes = legacyCapabilityBytes(capability, {second}).size() - headerBytes;
  auto boundary = capability;
  const auto count = ((16U << 20) - headerBytes) / operationBytes;
  boundary.operationRuns = {{second, count, 1}};
  BOOST_CHECK_EQUAL(boundary.canonicalBytes(true).size(), headerBytes + count * operationBytes);
  ++boundary.operationRuns.front().count;
  BOOST_CHECK_THROW(boundary.canonicalBytes(true), std::invalid_argument);
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

  const auto sealed = sealCryptoFixture(producer,
    makeGroupOperation(), "0", "src-layout", "dst-layout", "tensor-digest",
    {{'a', 'b', 'c'}, {'d', 'e'}}, 1000);
  BOOST_REQUIRE_EQUAL(sealed.segments.size(), 2);
  BOOST_CHECK_EQUAL(sealed.manifest.segmentCount, 2);
  BOOST_CHECK(!sealed.manifest.producerSignature.empty());
  BOOST_CHECK_EQUAL(sealed.segments[0].nonce.size(), 12);
  BOOST_CHECK(sealed.segments[0].nonce != sealed.segments[1].nonce);
  const auto expectedName = ndn::Name("/provider/P0/NDNSF-DI/crypto-fixture")
    .append(capability.capabilityDigest).append("1000").append("tensor-digest").appendSegment(0);
  BOOST_CHECK(ndn::Name(sealed.segments[0].dataName) == expectedName);

  BOOST_CHECK(consumer.acceptSegment(sealed.manifest, sealed.segments[1], sealed.segments[1].dataName) ==
              DataSegmentReplayWindow::Result::Accepted);
  BOOST_CHECK(consumer.acceptSegment(sealed.manifest, sealed.segments[0], sealed.segments[0].dataName) ==
              DataSegmentReplayWindow::Result::Accepted);
  BOOST_CHECK(consumer.acceptSegment(sealed.manifest, sealed.segments[0], sealed.segments[0].dataName) ==
              DataSegmentReplayWindow::Result::Duplicate);
  BOOST_CHECK(sealed.segments[0].dataName.find("//") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(ExplicitMemberReferencesRejectMissingForeignAndRankAliasesBeforeAdmission)
{
  ProviderGroupCoordinator producer(makeCoordinatorOptions());
  const auto operation = makeGroupOperation();
  const auto capability = producer.createCapability(
    "/request/exact-owner", "attempt-1", "sha256:plan", "group-1", 9,
    {{"/provider/P0", 0, "offer-0", "/provider/P0/transport"},
     {"/provider/P1", 1, "offer-1", "/provider/P1/transport"}},
    {operation}, 32, 100, 500);
  const std::vector<ProviderGroupBytes> plaintext{{'x'}};
  const std::vector<std::string> names{"/provider/P0/crypto-fixture/seg=0"};
  BOOST_CHECK_THROW(producer.sealOperation(operation, "0", "src", "dst", "tensor", plaintext, 100),
                    std::invalid_argument);
  BOOST_CHECK_THROW(producer.sealOperation(operation, "0", "src", "dst", "tensor", plaintext, 100,
                    {"/foreign/crypto-fixture/seg=0"}), std::invalid_argument);
  BOOST_CHECK_THROW(producer.sealOperation(operation, "/provider/P0", "src", "dst", "tensor", plaintext, 100,
                    names), std::invalid_argument);
  BOOST_CHECK_THROW(ProviderGroupCoordinator::makeDataName(capability, CollectiveOperationManifestV1{}, 0),
                    std::invalid_argument);
  const auto sealed = producer.sealOperation(operation, "0", "src", "dst", "tensor", plaintext, 100, names);
  ProviderGroupCoordinator consumer(makeCoordinatorOptions());
  consumer.installCapability(capability, std::vector<std::uint8_t>(32, 0x5a), true);
  BOOST_CHECK_THROW(consumer.openSegment(sealed.manifest, sealed.segments.front()), std::invalid_argument);
  BOOST_CHECK_THROW(consumer.acceptSegment(sealed.manifest, sealed.segments.front()), std::invalid_argument);
  BOOST_CHECK_THROW(consumer.acceptAndOpenSegment(sealed.manifest, sealed.segments.front(),
                    "/foreign/crypto-fixture/seg=0"), std::invalid_argument);
  BOOST_CHECK(!consumer.terminal());
  BOOST_CHECK(consumer.acceptSegment(sealed.manifest, sealed.segments.front(), names.front()) ==
              DataSegmentReplayWindow::Result::Accepted);
  BOOST_CHECK(consumer.acceptSegment(sealed.manifest, sealed.segments.front(), names.front()) ==
              DataSegmentReplayWindow::Result::Duplicate);
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
  const auto first = sealCryptoFixture(producer,
    makeGroupOperation(), "0", "src", "dst", "tensor-a",
    {{'a', 'b'}}, 1000, {"/provider/P0/tensor-a/seg=0"});
  const auto second = sealCryptoFixture(producer,
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
    sealCryptoFixture(producer,
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
  const auto sealed = sealCryptoFixture(producer,
    makeGroupOperation(), "0", "src-layout", "dst-layout", "tensor-digest",
    {{'a', 'b', 'c'}, {'d', 'e'}}, 1000, names);
  BOOST_REQUIRE_EQUAL(sealed.segments.size(), names.size());
  BOOST_CHECK(ndn::Name(sealed.segments[0].dataName) == ndn::Name(names[0]));
  BOOST_CHECK(ndn::Name(sealed.segments[1].dataName) == ndn::Name(names[1]));

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
  ProviderGroupCoordinator compactConsumer(options);
  compactConsumer.installCapability(
    capability.projectForProvider("/provider/P1"), {}, true);
  const auto unwrapped = consumer.epochKeyForProvider("/provider/P1");
  BOOST_REQUIRE_EQUAL(unwrapped.size(), 32U);
  BOOST_CHECK_THROW(consumer.epochKeyForProvider("/provider/P0"),
                    std::runtime_error);

  const auto sealed = sealCryptoFixture(producer,
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

    const auto compactWire = ProviderGroupCoordinator::encodeSegmentCompact(
      sealed.manifest, sealed.segments[index]);
    BOOST_CHECK_LT(compactWire.size(), ndn::MAX_NDN_PACKET_SIZE);
    const auto compactDecoded = ProviderGroupCoordinator::decodeSegment(
      compactWire, sealed.segments[index].dataName);
    BOOST_REQUIRE_EQUAL(compactDecoded.segments.size(), 1U);
    BOOST_CHECK(compactDecoded.manifest.externalSegmentDigests);
    BOOST_CHECK_EQUAL(compactDecoded.manifest.transportManifestDigest,
                      sealed.manifest.digest());
    BOOST_CHECK_EQUAL(compactDecoded.segments.front().dataName,
                      sealed.segments[index].dataName);
    BOOST_CHECK_EQUAL(compactDecoded.segments.front().descriptor.segmentNo,
                      sealed.segments[index].descriptor.segmentNo);

    auto external = compactDecoded;
    external.manifest.capabilityDigest = sealed.manifest.capabilityDigest;
    external.manifest.epochKeyId = sealed.manifest.epochKeyId;
    external.manifest.requestId = sealed.manifest.requestId;
    external.manifest.attemptId = sealed.manifest.attemptId;
    external.manifest.planDigest = sealed.manifest.planDigest;
    external.manifest.groupId = sealed.manifest.groupId;
    external.manifest.epoch = sealed.manifest.epoch;
    external.manifest.operationIndex = sealed.manifest.operationIndex;
    external.manifest.producerRank = sealed.manifest.producerRank;
    external.manifest.operationKind = sealed.manifest.operationKind;
    external.manifest.segmentCount = sealed.manifest.segmentCount;
    external.manifest.totalBytes = sealed.manifest.totalBytes;
    external.manifest.segmentSize = sealed.manifest.segmentSize;
    external.manifest.sourceLayoutDigest = sealed.manifest.sourceLayoutDigest;
    external.manifest.targetLayoutDigest = sealed.manifest.targetLayoutDigest;
    external.manifest.tensorDigest = sealed.manifest.tensorDigest;
    external.manifest.createdAtMs = sealed.manifest.createdAtMs;
    external.manifest.noProgressMs = sealed.manifest.noProgressMs;
    external.manifest.hardDeadlineMs = sealed.manifest.hardDeadlineMs;
    auto& externalSegment = external.segments.front();
    externalSegment.descriptor.requestId = sealed.segments[index].descriptor.requestId;
    externalSegment.descriptor.attemptId = sealed.segments[index].descriptor.attemptId;
    externalSegment.descriptor.planDigest = sealed.segments[index].descriptor.planDigest;
    externalSegment.descriptor.groupId = sealed.segments[index].descriptor.groupId;
    externalSegment.descriptor.epoch = sealed.segments[index].descriptor.epoch;
    externalSegment.descriptor.operationKind = sealed.segments[index].descriptor.operationKind;
    externalSegment.descriptor.tensorDigest = sealed.segments[index].descriptor.tensorDigest;
    externalSegment.descriptor.operationIndex = sealed.segments[index].descriptor.operationIndex;
    externalSegment.descriptor.producerRank = sealed.segments[index].descriptor.producerRank;
    externalSegment.descriptor.segmentCount = sealed.segments[index].descriptor.segmentCount;
    externalSegment.descriptor.totalBytes = sealed.segments[index].descriptor.totalBytes;
    externalSegment.descriptor.segmentSize = sealed.segments[index].descriptor.segmentSize;
    externalSegment.descriptor.noProgressMs = sealed.segments[index].descriptor.noProgressMs;
    externalSegment.descriptor.hardDeadlineMs = sealed.segments[index].descriptor.hardDeadlineMs;
    BOOST_CHECK(compactConsumer.acceptSegment(
      external.manifest, externalSegment, externalSegment.dataName) ==
      DataSegmentReplayWindow::Result::Accepted);
  }
  BOOST_CHECK(consumer.acceptSegment(decoded.manifest, decoded.segments[1], decoded.segments[1].dataName) ==
              DataSegmentReplayWindow::Result::Accepted);
  BOOST_CHECK(consumer.acceptSegment(decoded.manifest, decoded.segments[0], decoded.segments[0].dataName) ==
              DataSegmentReplayWindow::Result::Accepted);

  auto tampered = decoded;
  tampered.segments.front().ciphertext.front() ^= 0x01;
  BOOST_CHECK_THROW(consumer.openSegment(tampered.manifest,
                                         tampered.segments.front(), tampered.segments.front().dataName),
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
  const auto hmacSealed = sealCryptoFixture(hmacProducer,
    makeGroupOperation(), "0", "src", "dst", "tensor",
    {{'h', 'm'}, {'a', 'c'}}, 1000);
  BOOST_CHECK(hmacConsumer.acceptSegment(
                hmacSealed.manifest, hmacSealed.segments.front(), hmacSealed.segments.front().dataName) ==
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
  const auto sealed = sealCryptoFixture(producer,
    operation, "0", "src", "dst", "tensor", {{'x', 'y'}}, 100);
  auto consumerOptions = makeCoordinatorOptions();
  ProviderGroupCoordinator consumer(consumerOptions);
  consumer.installCapability(capability, std::vector<uint8_t>(32, 0x5a), true);

  auto altered = sealed.manifest;
  altered.operationKind = "REDUCE_SCATTER";
  BOOST_CHECK_THROW(consumer.openSegment(altered, sealed.segments.front(), sealed.segments.front().dataName),
                    std::runtime_error);

  consumer.cancel("test-cancel");
  BOOST_CHECK(consumer.cancelled());
  BOOST_CHECK(consumer.terminal());
  BOOST_CHECK_THROW(consumer.epochKeyForProvider("/provider/P0"),
                    std::runtime_error);
  BOOST_CHECK_THROW(consumer.openSegment(sealed.manifest, sealed.segments.front(), sealed.segments.front().dataName),
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
    const auto sealed = sealCryptoFixture(producer,
      operation, "0", "src", "dst", "tensor", {{'x', 'y'}}, 100);

    consumer.installCapability(capability, std::vector<std::uint8_t>(32, 0x5a), true);
    BOOST_REQUIRE(consumer.acceptSegment(
                    sealed.manifest, sealed.segments.front(), sealed.segments.front().dataName) ==
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

BOOST_AUTO_TEST_CASE(CoordinatorScopesNoProgressToEachOperation)
{
  auto operation = makeGroupOperation();
  operation.consumerRanks = {"0"};
  ProviderGroupCoordinator producer(makeCoordinatorOptions());
  const auto capability = producer.createCapability(
    "request", "attempt", "plan", "group", 1,
    {{"/provider/P0", 0, "offer", "/p0"}},
    {operation}, 32, 10, 1000);
  const auto sealed = sealCryptoFixture(producer,
    operation, "0", "src", "dst", "tensor", {{'x', 'y'}}, 100);

  ProviderGroupCoordinator consumer(makeCoordinatorOptions());
  consumer.installCapability(capability, std::vector<uint8_t>(32, 0x5a), true);
  BOOST_REQUIRE(consumer.acceptSegment(
                  sealed.manifest, sealed.segments.front(), sealed.segments.front().dataName) ==
                DataSegmentReplayWindow::Result::Accepted);

  // A long runner-preparation/idle interval starts a new transport operation
  // but does not extend the authenticated hard deadline.
  BOOST_CHECK(consumer.beginOperation(500));
  BOOST_CHECK(!consumer.terminal());
  BOOST_CHECK(consumer.recordProgress(509));

  // Once the operation is active, the existing no-progress failure remains.
  BOOST_CHECK(!consumer.recordProgress(520));
  BOOST_CHECK(consumer.failed());
  BOOST_CHECK_EQUAL(consumer.terminalReason(),
                    "NDNSF_DATA_V1_NO_PROGRESS");
}

BOOST_AUTO_TEST_CASE(CoordinatorAllowsInitialProducerReadinessWait)
{
  auto operation = makeGroupOperation();
  operation.consumerRanks = {"0"};
  ProviderGroupCoordinator producer(makeCoordinatorOptions());
  const auto capability = producer.createCapability(
    "request", "attempt", "plan", "group", 1,
    {{"/provider/P0", 0, "offer", "/p0"}},
    {operation}, 32, 10, 1000);

  ProviderGroupCoordinator consumer(makeCoordinatorOptions());
  consumer.installCapability(capability, std::vector<uint8_t>(32, 0x5a), true);

  // The first manifest may become available after the normal no-progress
  // window while the producer is still preparing its runner.  Re-baselining
  // at manifest availability must keep the group live without extending the
  // authenticated hard deadline.
  BOOST_CHECK(consumer.beginOperation(100));
  BOOST_CHECK(consumer.beginOperation(500));
  BOOST_CHECK(!consumer.terminal());
  BOOST_CHECK(consumer.recordProgress(509));
  BOOST_CHECK(!consumer.terminal());
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
    const auto sealed = sealCryptoFixture(producer,
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
        BOOST_CHECK_THROW(consumer.acceptSegment(sealed.manifest, candidate, candidate.dataName),
                          std::exception);
        candidate = sealed.segments[index];
      }
      BOOST_CHECK(consumer.acceptSegment(sealed.manifest, candidate, candidate.dataName) ==
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
                    sealed.manifest, sealed.segments[order.front()], sealed.segments[order.front()].dataName) ==
                  DataSegmentReplayWindow::Result::Duplicate);
      BOOST_CHECK(!consumer.terminal());
    }
  }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
