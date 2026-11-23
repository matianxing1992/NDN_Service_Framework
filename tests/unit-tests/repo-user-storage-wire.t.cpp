#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace ndnsf_distributed_repo;

namespace {

RepoUserStorageInsertRequest
sampleInsert()
{
  RepoUserStorageInsertRequest request;
  request.operationId = "kv-save-1";
  request.generation = 1;
  request.storageBinding = {1, "/owner", "grant-1", 7};
  auto& reference = request.reference;
  reference.objectName = "/producer/kv/manifest";
  reference.dataPrefix = "/producer";
  reference.forwardingHint = "/producer";
  reference.expectedSha256 = std::string(64, 'a');
  reference.expectedSize = 1024;
  reference.packetSetMetadata.emplace();
  auto& metadata = *reference.packetSetMetadata;
  metadata.objectName = reference.objectName;
  metadata.objectType = "original-packet-set";
  metadata.sha256 = reference.expectedSha256;
  metadata.size = reference.expectedSize;
  metadata.envelopeBytes = 512;
  metadata.dataPrefix = "/producer/kv";
  metadata.packetNames = {reference.objectName, "/producer/kv/seg=0"};
  return request;
}

RepoUserStorageGrantReply
sampleGrant()
{
  RepoUserStorageGrantReply reply;
  reply.grant = {"grant-1", "/repo", "/owner", "/producer",
                 "/producer/kv/manifest", 7, 100, 10};
  return reply;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec221RepoUserStorageWire)

BOOST_AUTO_TEST_CASE(StoredModelUploadGeometryFitsBinaryInsertAndRecoveryManifest)
{
  // Exercise both independent limits: packet count and aggregate Name text.
  for (const unsigned width : {96U, 2000U}) {
    auto request = sampleInsert();
    request.schemaVersion = 2;
    auto& reference = request.reference;
    auto& metadata = *reference.packetSetMetadata;
    metadata.packetNames.clear();
    metadata.packetOffsets = {0};
    metadata.envelopeBytes = 0;
    metadata.dataPrefix.clear();
    std::size_t nameBytes = 0;
    std::uint64_t wireBytes = 0;
    for (unsigned i = 0; i < 4096; ++i) {
      const auto name = ndn::Name("/producer").append(std::string(width, 'p'))
        .appendSegment(i).toUri(ndn::name::UriFormat::ALTERNATE);
      if (nameBytes + name.size() > (512U << 10)) break;
      nameBytes += name.size();
      wireBytes += 8192;
      metadata.packetNames.push_back(name);
      metadata.packetOffsets.push_back(wireBytes);
    }
    BOOST_REQUIRE_GT(metadata.packetNames.size(), width == 96 ? 1024U : 64U);
    reference.objectName = metadata.objectName = metadata.packetNames.front();
    reference.expectedSize = metadata.size = wireBytes;
    reference.validateExactPacketSet();
    BOOST_CHECK_LE(wireBytes, 32U << 20);
    BOOST_CHECK_LE(reference.toJson().size(), 1U << 20);
    const auto encoded = encodeUserStorageInsertRequest(request);
    const auto restored = decodeUserStorageInsertRequest(encoded);
    BOOST_CHECK(restored.reference.toJson() == reference.toJson());
    metadata.operationId = request.operationId;
    metadata.generation = request.generation;
    metadata.lifecycleState = "COMMITTED";
    const auto manifest = metadata.toJson();
    BOOST_CHECK_LE(manifest.size(), 1U << 20);
    BOOST_CHECK(parseManifestJson(manifest).packetNames == metadata.packetNames);
  }
}

BOOST_AUTO_TEST_CASE(ManifestInsertBindsBinaryScopeAndCanonicalPacketOrder)
{
  for (const uint32_t count : {2U, 129U}) {
    auto request = sampleInsert();
    request.schemaVersion = 3;
    auto& digest = request.segmentManifestBinding.emplace();
    digest[1] = 0xff;
    digest[5] = '\n';
    auto& reference = request.reference;
    reference.dataPrefix = count == 129 ? "/producer/8=kv%2Fcache/54=%01%00" : "/producer/kv";
    reference.objectName = reference.dataPrefix + "/MANIFEST";
    auto& metadata = *reference.packetSetMetadata;
    metadata.objectName = reference.objectName;
    metadata.dataPrefix.clear();
    metadata.envelopeBytes = 0;
    metadata.packetNames = {reference.objectName};
    if (count == 129) {
      metadata.packetNames.push_back(reference.objectName + "/PAGE/0");
      metadata.packetNames.push_back(reference.objectName + "/PAGE/1");
    }
    const ndn::Name dataPrefix(reference.dataPrefix + "/DATA");
    for (uint32_t i = 0; i < count; ++i) {
      metadata.packetNames.push_back(ndn::Name(dataPrefix).appendSegment(i).toUri());
    }
    const auto wire = encodeUserStorageInsertRequest(request);
    BOOST_CHECK_EQUAL(wire.at(4), '3');
    const auto restored = decodeUserStorageInsertRequest(wire);
    BOOST_CHECK(restored.segmentManifestBinding == request.segmentManifestBinding);
    BOOST_CHECK(encodeUserStorageInsertRequest(restored) == wire);

    auto wrongOrder = request;
    auto& names = wrongOrder.reference.packetSetMetadata->packetNames;
    std::iter_swap(names.end() - 2, names.end() - 1);
    BOOST_CHECK_THROW(encodeUserStorageInsertRequest(wrongOrder), std::invalid_argument);
    auto compact = request;
    compact.schemaVersion = 4;
    auto& compactMetadata = *compact.reference.packetSetMetadata;
    compactMetadata.packetOffsets = {0, compact.reference.expectedSize};
    const auto compactWire = encodeUserStorageInsertRequest(compact);
    BOOST_CHECK_EQUAL(compactWire.at(4), '4');
    const auto expanded = decodeUserStorageInsertRequest(compactWire);
    BOOST_CHECK(expanded.segmentManifestBinding == request.segmentManifestBinding);
    BOOST_REQUIRE(expanded.reference.packetSetMetadata);
    const auto& derived = *expanded.reference.packetSetMetadata;
    BOOST_CHECK_EQUAL(derived.segmentCount, metadata.packetNames.size());
    BOOST_CHECK(derived.packetOffsets.empty());
    BOOST_REQUIRE_EQUAL(derived.packetNames.size(), metadata.packetNames.size());
    for (size_t i = 0; i < metadata.packetNames.size(); ++i) {
      BOOST_CHECK_EQUAL(derived.packetNames[i],
        ndn::Name(metadata.packetNames[i]).toUri(ndn::name::UriFormat::CANONICAL));
    }
    BOOST_CHECK_LT(compactWire.size(), wire.size());
    BOOST_CHECK(encodeUserStorageInsertRequest(expanded) == compactWire);
    const std::string envelope = R"(envelopeBytes\":0)";
    const auto envelopeField = std::search(compactWire.begin(), compactWire.end(),
                                           envelope.begin(), envelope.end());
    BOOST_REQUIRE(envelopeField != compactWire.end());
    auto contradictoryEnvelope = compactWire;
    contradictoryEnvelope.at(std::distance(compactWire.begin(), envelopeField) +
                              envelope.size() - 1) = '1';
    BOOST_CHECK_THROW(decodeUserStorageInsertRequest(contradictoryEnvelope), std::invalid_argument);
    auto noncanonicalCompact = compactWire;
    noncanonicalCompact.insert(noncanonicalCompact.begin() + 6, '0');
    BOOST_CHECK_THROW(decodeUserStorageInsertRequest(noncanonicalCompact), std::invalid_argument);
    wrongOrder.schemaVersion = 4;
    BOOST_CHECK_THROW(encodeUserStorageInsertRequest(wrongOrder), std::invalid_argument);
    auto invalidCompact = wire;
    invalidCompact[4] = '4';
    BOOST_CHECK_THROW(decodeUserStorageInsertRequest(invalidCompact), std::invalid_argument);

    auto truncated = wire;
    truncated.pop_back();
    BOOST_CHECK_THROW(decodeUserStorageInsertRequest(truncated), std::invalid_argument);
    auto wrongVersion = wire;
    wrongVersion[4] = '2';
    BOOST_CHECK_THROW(decodeUserStorageInsertRequest(wrongVersion), std::invalid_argument);
    request.segmentManifestBinding.reset();
    BOOST_CHECK_THROW(encodeUserStorageInsertRequest(request), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(OriginalPacketInsertRoundTripPreservesNamesAndBinding)
{
  const auto original = sampleInsert();
  const auto wire = encodeUserStorageInsertRequest(original);
  const auto restored = decodeUserStorageInsertRequest(wire);
  BOOST_CHECK_EQUAL(restored.operationId, original.operationId);
  BOOST_CHECK_EQUAL(restored.generation, original.generation);
  BOOST_CHECK_EQUAL(restored.reference.toJson(), original.reference.toJson());
  BOOST_CHECK_EQUAL(restored.storageBinding.toJson(), original.storageBinding.toJson());
  BOOST_CHECK(encodeUserStorageInsertRequest(restored) == wire);
  BOOST_REQUIRE(restored.reference.packetSetMetadata);
  BOOST_CHECK(restored.reference.packetSetMetadata->packetNames ==
              original.reference.packetSetMetadata->packetNames);
  auto trailing = wire;
  trailing.push_back('x');
  BOOST_CHECK_THROW(decodeUserStorageInsertRequest(trailing), std::invalid_argument);
  auto badMagic = wire;
  badMagic[0] ^= 1;
  BOOST_CHECK_THROW(decodeUserStorageInsertRequest(badMagic), std::invalid_argument);
  for (const auto size : {std::size_t(0), std::size_t(6), wire.size() / 2, wire.size() - 1}) {
    const std::vector<uint8_t> truncated(wire.begin(), wire.begin() + size);
    BOOST_CHECK_THROW(decodeUserStorageInsertRequest(truncated), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(InsertRejectsUnboundAndReboundOriginalPacketIndexes)
{
  const auto valid = sampleInsert();
  const std::vector<std::function<void(RepoUserStorageInsertRequest&)>> mutations = {
    [] (auto& r) { r.generation = 0; },
    [] (auto& r) { r.operationId = std::string(257, 'x'); },
    [] (auto& r) { r.operationId = std::string("bad\0id", 6); },
    [] (auto& r) { r.storageBinding.ownerEpoch = 0; },
    [] (auto& r) { r.storageBinding.ownerIdentity = "owner"; },
    [] (auto& r) { r.reference.packetSetMetadata.reset(); },
    [] (auto& r) { r.reference.expectedSize = 0; },
    [] (auto& r) { r.reference.expectedSha256.clear(); },
    [] (auto& r) { r.reference.packetSetMetadata->sha256.clear(); },
    [] (auto& r) { r.reference.expectedSha256 = std::string(64, 'b'); },
    [] (auto& r) { r.reference.packetSetMetadata->sha256 = std::string(64, 'b'); },
    [] (auto& r) { r.reference.packetSetMetadata->sha256 = std::string(64, 'Z'); },
    [] (auto& r) { r.reference.hasFinalSegment = true; },
    [] (auto& r) { r.reference.storeWirePackets = false; },
    [] (auto& r) { r.reference.packetSetMetadata->size++; },
    [] (auto& r) { r.reference.packetSetMetadata->objectName += "/other"; },
    [] (auto& r) { r.reference.packetSetMetadata->packetNames.clear(); },
    [] (auto& r) { r.reference.packetSetMetadata->packetNames[0] += "/other"; },
    [] (auto& r) { r.reference.packetSetMetadata->packetNames[1] = "/foreign/seg=0"; },
    [] (auto& r) {
      auto& names = r.reference.packetSetMetadata->packetNames;
      names.push_back(names.back());
    },
  };
  for (const auto& mutate : mutations) {
    auto changed = valid;
    mutate(changed);
    BOOST_CHECK_THROW(encodeUserStorageInsertRequest(changed), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(DecoderRejectsReboundDigestCommitmentsAndNulOperation)
{
  const auto encoded = encodeUserStorageInsertRequest(sampleInsert());
  const std::string wire(encoded.begin(), encoded.end());
  const auto first = wire.find(std::string(64, 'a'));
  BOOST_REQUIRE(first != std::string::npos);
  const auto second = wire.find(std::string(64, 'a'), first + 64);
  BOOST_REQUIRE(second != std::string::npos);
  for (const auto position : {first, second}) {
    auto changed = encoded;
    changed[position] = 'b';
    BOOST_CHECK_THROW(decodeUserStorageInsertRequest(changed), std::invalid_argument);
  }
  const auto operation = wire.find("kv-save-1");
  BOOST_REQUIRE(operation != std::string::npos);
  auto changed = encoded;
  changed[operation] = 0;
  BOOST_CHECK_THROW(decodeUserStorageInsertRequest(changed), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ExactPacketIndexComparesTypedNamesAcrossUriSpellings)
{
  auto request = sampleInsert();
  auto& reference = request.reference;
  reference.dataPrefix = "/producer/kv";
  auto& names = reference.packetSetMetadata->packetNames;
  const ndn::Name prefix(reference.dataPrefix);
  const auto canonicalPrefix = prefix.toUri(ndn::name::UriFormat::CANONICAL);
  const auto segment = ndn::Name(prefix).appendSegment(0);
  names[1] = segment.toUri(ndn::name::UriFormat::CANONICAL);
  names.push_back("ndn://authority/producer/kv/seg=2");
  names.push_back(canonicalPrefix + "/");
  BOOST_CHECK(decodeUserStorageInsertRequest(encodeUserStorageInsertRequest(request))
                .reference.packetSetMetadata->packetNames == names);

  names.push_back(segment.toUri(ndn::name::UriFormat::ALTERNATE));
  BOOST_CHECK_THROW(encodeUserStorageInsertRequest(request), std::invalid_argument);
  names.back() = reference.dataPrefix;
  BOOST_CHECK_THROW(encodeUserStorageInsertRequest(request), std::invalid_argument);
  names.back() = canonicalPrefix + "-other/seg=3";
  BOOST_CHECK_THROW(encodeUserStorageInsertRequest(request), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(DecoderRejectsNoncanonicalAndMalformedFieldFrames)
{
  const auto request = sampleInsert();
  const auto reference = request.reference.toJson();
  const auto binding = request.storageBinding.toJson();
  const auto frame = [&] (const std::string& ref, const std::string& ownerBinding) {
    const auto wire = std::string("RPOI1\n") + std::to_string(request.operationId.size()) + "\n" +
      request.operationId + "1\n" + std::to_string(ref.size()) + "\n" + ref +
      std::to_string(ownerBinding.size()) + "\n" + ownerBinding;
    return std::vector<uint8_t>(wire.begin(), wire.end());
  };
  BOOST_REQUIRE(frame(reference, binding) == encodeUserStorageInsertRequest(request));
  const std::vector<std::pair<std::string, std::string>> fields = {
    {" " + reference, binding},
    {reference, " " + binding},
    {reference.substr(0, reference.size() - 1) + ",\"unknown\":1}", binding},
    {reference.substr(0, reference.size() - 1) + ",\"expectedSize\":1024}", binding},
    {reference, binding.substr(0, binding.size() - 1) + ",\"unknown\":1}"},
    {reference, binding.substr(0, binding.size() - 1) + ",\"ownerEpoch\":7}"},
    {reference, std::string(16 * 1024 + 1, 'x')},
  };
  for (const auto& changed : fields) {
    BOOST_CHECK_THROW(decodeUserStorageInsertRequest(frame(changed.first, changed.second)),
                      std::invalid_argument);
  }
  auto leadingZero = encodeUserStorageInsertRequest(request);
  leadingZero.insert(leadingZero.begin() + 6, '0');
  BOOST_CHECK_THROW(decodeUserStorageInsertRequest(leadingZero), std::invalid_argument);
  const std::string overflowing = "RPOI1\n9999999999999999999999999999\n";
  BOOST_CHECK_THROW(decodeUserStorageInsertRequest(
    std::vector<uint8_t>(overflowing.begin(), overflowing.end())), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StorageBindingsUseStrictBoundedFields)
{
  const RepoUserStorageBinding binding{1, "/owner", "grant-1", 7};
  BOOST_CHECK_EQUAL(RepoUserStorageBinding::fromJson(binding.toJson()).toJson(), binding.toJson());
  const std::vector<std::string> malformed = {
    "{}",
    "{\"schemaVersion\":1,\"ownerIdentity\":\"/owner\",\"grantId\":\"g\",\"ownerEpoch\":0}",
    "{\"schemaVersion\":1,\"ownerIdentity\":\"/owner\",\"grantId\":\"g\",\"ownerEpoch\":-1}",
    "{\"schemaVersion\":1,\"ownerIdentity\":\"/owner\",\"grantId\":\"g\",\"ownerEpoch\":true}",
    "{\"schemaVersion\":1,\"schemaVersion\":1,\"ownerIdentity\":\"/owner\",\"grantId\":\"g\",\"ownerEpoch\":7}",
  };
  for (const auto& wire : malformed) {
    BOOST_CHECK_THROW(RepoUserStorageBinding::fromJson(wire), std::invalid_argument);
  }
  auto invalid = binding;
  invalid.ownerEpoch = std::numeric_limits<uint64_t>::max();
  BOOST_CHECK_THROW(invalid.toJson(), std::invalid_argument);
  invalid = binding;
  invalid.grantId = std::string(257, 'x');
  BOOST_CHECK_THROW(invalid.toJson(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(GrantRepliesBindExactOwnerWriterObjectAndExpiry)
{
  const RepoArtifactTarget target{"/repo", "/repo-provider", "/SERVICE/repo"};
  const RepoUserStorageGrantRequest request{1, "grant-1", "/producer",
                                           "/producer/kv/manifest", 100};
  const auto original = sampleGrant();
  const auto restored = RepoUserStorageGrantReply::fromJson(original.toJson());
  BOOST_CHECK_EQUAL(restored.toJson(), original.toJson());
  BOOST_CHECK_NO_THROW(restored.validateExpected(target, "/owner", request));
  const std::vector<std::function<void(RepoUserStorageGrantReply&)>> mutations = {
    [] (auto& r) { r.grant.repoNode += "/other"; },
    [] (auto& r) { r.grant.ownerIdentity += "/other"; },
    [] (auto& r) { r.grant.writerIdentity += "/other"; },
    [] (auto& r) { r.grant.objectName += "/other"; },
    [] (auto& r) { r.grant.grantId += "other"; },
    [] (auto& r) { r.grant.expiresAtMs++; },
  };
  for (const auto& mutate : mutations) {
    auto changed = original;
    mutate(changed);
    BOOST_CHECK_THROW(changed.validateExpected(target, "/owner", request), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(ClaimAndCleanupRepliesKeepOwnerEpochAndPageBoundaries)
{
  const RepoArtifactTarget target{"/repo", "/repo-provider", "/SERVICE/repo"};
  RepoUserStorageListReply list;
  list.repoNode = "/repo";
  list.ownerIdentity = "/owner";
  RepoUserStorageClaim claim;
  claim.repoNode = list.repoNode;
  claim.ownerIdentity = list.ownerIdentity;
  claim.grantId = "grant-1";
  claim.ownerEpoch = 7;
  claim.object = {"/producer/kv/manifest", std::string(64, 'a'), 1, 1024, 128};
  claim.committedAtMs = 10;
  list.objects = {claim};
  list.nextAfterObjectName = claim.object.objectName;
  const auto restored = RepoUserStorageListReply::fromJson(list.toJson());
  BOOST_CHECK_EQUAL(restored.toJson(), list.toJson());
  BOOST_CHECK_NO_THROW(restored.validateExpected(target, "/owner", {1, "", 1}));
  BOOST_CHECK_THROW(restored.validateExpected(target, "/other", {1, "", 1}), std::invalid_argument);
  BOOST_CHECK_THROW(restored.validateExpected(target, "/owner", {1, claim.object.objectName, 1}),
                    std::invalid_argument);
  list.objects.push_back(claim);
  BOOST_CHECK_THROW(list.toJson(), std::invalid_argument);
  const RepoUserStorageDeleteRequest request{1, "cleanup-1", claim.object.objectName, 7};
  const RepoUserStorageDeleteReply reply{1, "/repo", "/owner", "cleanup-1",
                                        claim.object.objectName, 7, "DELETED", 1152};
  const auto cleanup = RepoUserStorageDeleteReply::fromJson(reply.toJson());
  BOOST_CHECK_NO_THROW(cleanup.validateExpected(target, "/owner", request));
  auto wrong = request;
  wrong.ownerEpoch++;
  BOOST_CHECK_THROW(cleanup.validateExpected(target, "/owner", wrong), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()
