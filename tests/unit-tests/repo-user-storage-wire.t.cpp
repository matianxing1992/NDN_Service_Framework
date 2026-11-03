#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <boost/test/unit_test.hpp>

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
