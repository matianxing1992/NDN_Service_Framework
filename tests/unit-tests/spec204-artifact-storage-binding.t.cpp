#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <ndn-cxx/name.hpp>

#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <functional>

namespace {
using namespace ndnsf_distributed_repo;

ArtifactReference reference()
{
  ArtifactReference ref;
  ref.logicalName = "/producer/kv";
  ref.publisherIdentity = "/producer";
  ref.rootManifestName = "/producer/manifest";
  ref.policyEpoch = "p1";
  ref.contentDigest = std::string(64, 'a');
  ref.sizeBytes = 2;
  return ref.canonicalized();
}

ArtifactStoreRequest storeRequest()
{
  ArtifactStoreRequest request;
  request.schemaVersion = 3;
  request.operationId = "operation"; request.generation = 1;
  request.reference = reference(); request.bytes = {1, 2};
  request.operationOwner = RepoArtifactTarget{"/repo/seed", "/seed/provider", "/service/repo"};
  request.storageBinding = RepoUserStorageBinding{1, "/user/u", "grant", 7};
  return request;
}

PublishOperationStatus ownedStatus()
{
  PublishOperationStatus status;
  status.schemaVersion = 2; status.operationId = std::string(64, 'b');
  status.reference = reference(); status.generation = 1;
  status.seed = {"/repo/a", "/provider/a", "/service/repo"};
  const RepoArtifactTarget other{"/repo/b", "/provider/b", "/service/repo"};
  status.consistency = "ALL"; status.state = "IN_PROGRESS"; status.updatedAtMs = 101;
  RepoArtifactCommitReceipt receipt;
  receipt.repoNode = status.seed.repoNode; receipt.providerIdentity = status.seed.providerIdentity;
  receipt.operationId = status.operationId; receipt.receiptId = "seed-receipt";
  receipt.reference = status.reference; receipt.generation = 1; receipt.persistedBytes = 2;
  receipt.completedAtMs = 100; receipt.locationVersion = 1;
  receipt.routeState = "ROUTABLE"; receipt.forwardingHint = "/service/repo";
  status.targets = {{status.seed, "COMMITTED", 1, 100, receipt, {}},
                    {other, "IN_FLIGHT", 1, 101, std::nullopt, {}}};
  status.userStorageBindings = {
    {1, status.seed, {1, "/user/u", "seed-grant", 3}, "/producer"},
    {1, other, {1, "/user/u", "replica-grant", 7}, "/provider/a"}};
  return status;
}

ArtifactRangeWriteRequest rangeRequest()
{
  const auto store = storeRequest();
  ArtifactRangeWriteRequest request;
  request.schemaVersion = 3; request.operationId = store.operationId;
  request.generation = store.generation; request.reference = store.reference;
  request.operationOwner = store.operationOwner; request.storageBinding = store.storageBinding;
  request.offsetBytes = 1; request.lengthBytes = 1; request.bytes = {2};
  return request;
}

ArtifactRangeCommitRequest commitRequest()
{
  const auto store = storeRequest();
  ArtifactRangeCommitRequest request;
  request.schemaVersion = 3; request.operationId = store.operationId;
  request.generation = store.generation; request.reference = store.reference;
  request.operationOwner = store.operationOwner; request.storageBinding = store.storageBinding;
  return request;
}

ArtifactRangeAbortRequest abortRequest()
{
  const auto store = storeRequest();
  ArtifactRangeAbortRequest request;
  request.schemaVersion = 2; request.operationId = store.operationId;
  request.generation = store.generation; request.reference = store.reference;
  request.operationOwner = store.operationOwner; request.storageBinding = store.storageBinding;
  return request;
}

std::string replace(std::string header, const std::string& old, const std::string& value)
{
  const auto at = header.find(old);
  if (at == std::string::npos) throw std::logic_error("missing fixture mutation");
  header.replace(at, old.size(), value);
  return header;
}

// Independently change the encoded header while retaining the payload and
// declared big-endian header length; mutations must reach the production parser.
std::vector<uint8_t> mutate(const std::vector<uint8_t>& wire,
                          const std::function<std::string(std::string)>& change)
{
  if (wire.size() < 12) throw std::logic_error("short fixture frame");
  const uint32_t size = (uint32_t(wire[8]) << 24) | (uint32_t(wire[9]) << 16) |
                        (uint32_t(wire[10]) << 8) | wire[11];
  if (size > wire.size() - 12) throw std::logic_error("bad fixture header length");
  const auto header = change(std::string(wire.begin()+12, wire.begin()+12+size));
  std::vector<uint8_t> result(wire.begin(), wire.begin()+8);
  for (int shift : {24, 16, 8, 0}) result.push_back(static_cast<uint8_t>(header.size() >> shift));
  result.insert(result.end(), header.begin(), header.end());
  result.insert(result.end(), wire.begin()+12+size, wire.end());
  return result;
}

template<typename Request>
void checkBoundFrame(const Request& request, char expectedVersion)
{
  const auto wire = request.toWire();
  BOOST_CHECK_EQUAL(wire[7], expectedVersion);
  const auto decoded = Request::fromWire(wire);
  BOOST_CHECK_EQUAL(decoded.schemaVersion, request.schemaVersion);
  BOOST_CHECK_EQUAL(decoded.operationId, "operation");
  BOOST_CHECK_EQUAL(decoded.generation, 1U);
  BOOST_CHECK(ndn::Name(decoded.reference.publisherIdentity) == ndn::Name("/producer"));
  BOOST_CHECK(ndn::Name(decoded.reference.rootManifestName) == ndn::Name("/producer/manifest"));
  BOOST_CHECK_EQUAL(decoded.reference.toJson(), request.reference.toJson());
  BOOST_CHECK_EQUAL(artifactStorageObjectName(decoded.reference), artifactStorageObjectName(request.reference));
  BOOST_REQUIRE(decoded.storageBinding.has_value());
  BOOST_CHECK_EQUAL(decoded.storageBinding->ownerIdentity, "/user/u");
  BOOST_CHECK_EQUAL(decoded.storageBinding->grantId, "grant");
  BOOST_CHECK_EQUAL(decoded.storageBinding->ownerEpoch, 7U);
  BOOST_REQUIRE(decoded.operationOwner.has_value());
  BOOST_CHECK_EQUAL(decoded.operationOwner->providerIdentity, "/seed/provider");
  BOOST_CHECK(decoded.toWire() == wire);
}

template<typename Request>
void checkMalformedBinding(const Request& request)
{
  const auto wire = request.toWire();
  const auto binding = request.storageBinding->toJson();
  for (const auto& wrong : {std::string("null"),
      replace(binding, "\"ownerEpoch\":7", "\"ownerEpoch\":0"),
      replace(binding, "\"schemaVersion\":1", "\"schemaVersion\":4294967297"),
      replace(binding, "\"grantId\":\"grant\"", "\"grantId\":\"grant\",\"authenticatedWriter\":\"/attacker\""),
      replace(binding, "\"ownerIdentity\":\"/user/u\"", "\"ownerIdentity\":\"/user/u\",\"ownerIdentity\":\"/attacker\"")}) {
    const auto bad = mutate(wire, [&](std::string header) { return replace(header, binding, wrong); });
    BOOST_CHECK_THROW(Request::fromWire(bad), std::invalid_argument);
  }
  const auto missing = mutate(wire, [&](std::string header) { return replace(header, ",\"storageBinding\":" + binding, ""); });
  BOOST_CHECK_THROW(Request::fromWire(missing), std::invalid_argument);
  auto wrongMagic = wire; wrongMagic[7] = wire[7] == '3' ? '2' : '1';
  BOOST_CHECK_THROW(Request::fromWire(wrongMagic), std::invalid_argument);
  const auto extra = mutate(wire, [](std::string header) { header.pop_back(); return header + ",\"ownerIdentity\":\"/attacker\"}"; });
  BOOST_CHECK_THROW(Request::fromWire(extra), std::invalid_argument);
}
} // namespace

BOOST_AUTO_TEST_SUITE(Spec204ArtifactStorageBinding)

BOOST_AUTO_TEST_CASE(BoundFramesRetainProducerPayloadAndSeparateSeedAccount)
{
  checkBoundFrame(storeRequest(), '3'); checkBoundFrame(rangeRequest(), '3');
  checkBoundFrame(commitRequest(), '3'); checkBoundFrame(abortRequest(), '2');
  BOOST_CHECK(ArtifactStoreRequest::fromWire(storeRequest().toWire()).bytes == std::vector<uint8_t>({1, 2}));
  const auto range = ArtifactRangeWriteRequest::fromWire(rangeRequest().toWire());
  BOOST_CHECK_EQUAL(range.offsetBytes, 1U); BOOST_CHECK_EQUAL(range.lengthBytes, 1U);
  BOOST_CHECK(range.bytes == std::vector<uint8_t>({2}));
}

BOOST_AUTO_TEST_CASE(BoundParsersRejectMissingAmbiguousAndMismatchedSelectors)
{
  checkMalformedBinding(storeRequest()); checkMalformedBinding(rangeRequest());
  checkMalformedBinding(commitRequest()); checkMalformedBinding(abortRequest());
}

BOOST_AUTO_TEST_CASE(SerializationCannotSilentlyDropOrInventBindings)
{
  auto store = storeRequest(); store.schemaVersion = 2;
  BOOST_CHECK_THROW(store.toWire(), std::invalid_argument);
  store.schemaVersion = 3; store.storageBinding.reset();
  BOOST_CHECK_THROW(store.toWire(), std::invalid_argument);
  auto range = rangeRequest(); range.schemaVersion = 2;
  BOOST_CHECK_THROW(range.toWire(), std::invalid_argument);
  range.schemaVersion = 3; range.storageBinding.reset();
  BOOST_CHECK_THROW(range.toWire(), std::invalid_argument);
  auto commit = commitRequest(); commit.schemaVersion = 2;
  BOOST_CHECK_THROW(commit.toWire(), std::invalid_argument);
  commit.schemaVersion = 3; commit.storageBinding.reset();
  BOOST_CHECK_THROW(commit.toWire(), std::invalid_argument);
  auto abort = abortRequest(); abort.schemaVersion = 1;
  BOOST_CHECK_THROW(abort.toWire(), std::invalid_argument);
  abort.storageBinding.reset();
  BOOST_CHECK_THROW(abort.toWire(), std::invalid_argument);
  abort.operationOwner.reset(); BOOST_CHECK_NO_THROW(abort.toWire());
  abort.schemaVersion = 2; BOOST_CHECK_THROW(abort.toWire(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(FrameBudgetsAndPayloadLengthsStillApply)
{
  auto store = storeRequest(); auto wire = store.toWire(); wire.pop_back();
  BOOST_CHECK_THROW(ArtifactStoreRequest::fromWire(wire), std::invalid_argument);
  auto range = rangeRequest(); wire = range.toWire(); wire.push_back(3);
  BOOST_CHECK_THROW(ArtifactRangeWriteRequest::fromWire(wire), std::invalid_argument);
  range.offsetBytes = 2; BOOST_CHECK_THROW(range.toWire(), std::invalid_argument);
  wire = commitRequest().toWire(); wire.push_back(0);
  BOOST_CHECK_THROW(ArtifactRangeCommitRequest::fromWire(wire), std::invalid_argument);
  wire = abortRequest().toWire(); wire.push_back(0);
  BOOST_CHECK_THROW(ArtifactRangeAbortRequest::fromWire(wire), std::invalid_argument);
  wire = mutate(store.toWire(), [](std::string header) { return header + std::string(65536, ' '); });
  BOOST_CHECK_THROW(ArtifactStoreRequest::fromWire(wire), std::invalid_argument);
  auto limits = ArtifactLimits{}; limits.maxChunkBytes = 1;
  BOOST_CHECK_THROW(ArtifactStoreRequest::fromWire(store.toWire(), limits), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(LegacyFramesKeepTheirExistingExplicitUpgrade)
{
  auto old = storeRequest(); old.schemaVersion = 2;
  old.storageBinding.reset(); old.operationOwner.reset();
  const auto legacy = ArtifactStoreRequest::fromWire(old.toWire());
  BOOST_CHECK_EQUAL(legacy.schemaVersion, 2U);
  BOOST_CHECK(!legacy.storageBinding); BOOST_CHECK(legacy.bytes == old.bytes);
  auto v1 = mutate(old.toWire(), [](std::string header) {
    header = replace(header, "\"schemaVersion\":2,\"operationId\"", "\"schemaVersion\":1,\"operationId\"");
    return replace(header, ",\"operationOwner\":null", "");
  });
  v1[7] = '1';
  const auto upgraded = ArtifactStoreRequest::fromWire(v1);
  BOOST_CHECK_EQUAL(upgraded.schemaVersion, 2U);
  BOOST_CHECK(!upgraded.storageBinding); BOOST_CHECK(!upgraded.operationOwner);
  BOOST_CHECK(upgraded.bytes == old.bytes);
  BOOST_CHECK_EQUAL(upgraded.reference.toJson(), old.reference.toJson());
}

BOOST_AUTO_TEST_CASE(OwnedIntentBindsEachExactGrantAndImmediateWriter)
{
  PublishOperationIntent intent;
  intent.schemaVersion = 2; intent.reference = reference(); intent.generation = 1;
  intent.idempotencyKeyDigest = digestPublishIdempotencyKey("owned-key");
  intent.seed = {"/repo/a", "/provider/a", "/service/repo"};
  const RepoArtifactTarget other{"/repo/b", "/provider/b", "/service/repo"};
  intent.targets = {intent.seed, other}; intent.consistency = "ALL";
  intent.userStorageBindings = {
    {1, intent.seed, {1, "/user/u", "seed-grant", 3}, "/producer"},
    {1, other, {1, "/user/u", "replica-grant", 9}, "/provider/a"}};
  const auto digestFor = [](const auto& value) {
    return makePublishRequestDigest(value.reference, value.generation, value.seed,
      value.consistency, value.targets, value.userStorageBindings);
  };
  intent.requestDigest = digestFor(intent);
  const auto wire = intent.toJson();
  const auto parsed = PublishOperationIntent::fromJson(wire);
  BOOST_CHECK_EQUAL(parsed.toJson(), wire);
  BOOST_CHECK_EQUAL(parsed.reference.toJson(), reference().toJson());
  BOOST_REQUIRE_EQUAL(parsed.userStorageBindings.size(), 2U);
  BOOST_CHECK_EQUAL(parsed.userStorageBindings[1].binding.ownerEpoch, 9U);
  BOOST_CHECK_EQUAL(parsed.userStorageBindings[0].writerIdentity, "/producer");
  BOOST_CHECK_EQUAL(parsed.userStorageBindings[1].writerIdentity, "/provider/a");
  auto reordered = intent;
  std::reverse(reordered.targets.begin(), reordered.targets.end());
  std::reverse(reordered.userStorageBindings.begin(), reordered.userStorageBindings.end());
  BOOST_CHECK_EQUAL(digestFor(reordered), intent.requestDigest);
  BOOST_CHECK_NE(intent.requestDigest, makePublishRequestDigest(intent.reference, 1,
    intent.seed, intent.consistency, intent.targets));
  auto changed = intent;
  changed.userStorageBindings[1].binding.grantId += "-new";
  BOOST_CHECK_NE(digestFor(changed), intent.requestDigest);
  BOOST_CHECK_THROW(changed.toJson(), ArtifactValidationError);
  changed = intent; ++changed.userStorageBindings[1].binding.ownerEpoch;
  BOOST_CHECK_NE(digestFor(changed), intent.requestDigest);
  changed = intent;
  for (auto& entry : changed.userStorageBindings) entry.binding.ownerIdentity = "/user/v";
  BOOST_CHECK_NE(digestFor(changed), intent.requestDigest);
  changed = intent; changed.userStorageBindings[0].writerIdentity = "/provider/a";
  BOOST_CHECK_THROW(digestFor(changed), ArtifactValidationError);
  changed = intent; changed.userStorageBindings[1].writerIdentity = "/producer";
  BOOST_CHECK_THROW(digestFor(changed), ArtifactValidationError);
  changed = intent; changed.userStorageBindings[1] = changed.userStorageBindings[0];
  BOOST_CHECK_THROW(digestFor(changed), ArtifactValidationError);
  changed = intent; changed.userStorageBindings.pop_back();
  BOOST_CHECK_THROW(digestFor(changed), ArtifactValidationError);
  changed = intent; changed.userStorageBindings[1].binding.ownerIdentity = "/user/v";
  BOOST_CHECK_THROW(digestFor(changed), ArtifactValidationError);
  changed = intent; changed.schemaVersion = 1;
  BOOST_CHECK_THROW(changed.toJson(), ArtifactValidationError);
  changed.userStorageBindings.clear();
  changed.requestDigest = makePublishRequestDigest(changed.reference, 1, changed.seed,
    changed.consistency, changed.targets);
  BOOST_CHECK_EQUAL(PublishOperationIntent::fromJson(changed.toJson()).schemaVersion, 1U);
}

BOOST_AUTO_TEST_CASE(OwnedIntentRejectsWireOverridesAndOverflow)
{
  PublishOperationIntent intent;
  intent.schemaVersion = 2; intent.reference = reference(); intent.generation = 1;
  intent.idempotencyKeyDigest = digestPublishIdempotencyKey("wire-key");
  intent.seed = {"/repo/a", "/provider/a", "/service/repo"};
  intent.targets = {intent.seed}; intent.consistency = "ONE";
  intent.userStorageBindings = {{1, intent.seed, {1, "/user/u", "grant", 1}, "/producer"}};
  intent.requestDigest = makePublishRequestDigest(intent.reference, 1, intent.seed,
    "ONE", intent.targets, intent.userStorageBindings);
  const auto wire = intent.toJson();
  for (const auto& malformed : {
    replace(wire, "\"schemaVersion\":2,\"operation\"", "\"schemaVersion\":4294967298,\"operation\""),
    replace(wire, "\"ownerEpoch\":1", "\"ownerEpoch\":0"),
    replace(wire, "\"ownerEpoch\":1", "\"ownerEpoch\":1,\"writerIdentity\":\"/spoof\""),
    replace(wire, "\"grantId\":\"grant\"", "\"grantId\":\"grant\",\"grantId\":\"other\""),
    replace(wire, "\"userStorageBindings\":[", "\"userStorageBindings\":null,\"ignored\":["),
    replace(wire, "\"userStorageBindings\"", "\"bindings\"")}) {
    BOOST_CHECK_THROW(PublishOperationIntent::fromJson(malformed), std::exception);
  }
  BOOST_CHECK_THROW(PublishOperationIntent::fromJson(wire + std::string(256 * 1024, ' ')), std::exception);
}

BOOST_AUTO_TEST_CASE(OwnedStatusPreservesExactDestinationAuthority)
{
  const auto status = ownedStatus();
  const auto wire = status.toJson();
  const auto parsed = PublishOperationStatus::fromJson(wire);
  BOOST_CHECK_EQUAL(parsed.schemaVersion, 2U);
  BOOST_REQUIRE_EQUAL(parsed.userStorageBindings.size(), 2U);
  BOOST_CHECK_EQUAL(parsed.userStorageBindings[1].target.repoNode, "/repo/b");
  BOOST_CHECK_EQUAL(parsed.userStorageBindings[1].binding.ownerIdentity, "/user/u");
  BOOST_CHECK_EQUAL(parsed.userStorageBindings[1].binding.grantId, "replica-grant");
  BOOST_CHECK_EQUAL(parsed.userStorageBindings[1].binding.ownerEpoch, 7U);
  BOOST_CHECK_EQUAL(parsed.userStorageBindings[1].writerIdentity, "/provider/a");
  BOOST_CHECK_EQUAL(parsed.toJson(), wire);
  auto legacy = status; legacy.schemaVersion = 1;
  BOOST_CHECK_THROW(legacy.toJson(), std::exception);
  legacy.userStorageBindings.clear();
  const auto oldWire = legacy.toJson();
  BOOST_CHECK_EQUAL(oldWire.find("userStorageBindings"), std::string::npos);
  BOOST_CHECK_EQUAL(PublishOperationStatus::fromJson(oldWire).toJson(), oldWire);
}

BOOST_AUTO_TEST_CASE(OwnedStatusRejectsIncompleteOrContradictoryAuthority)
{
  const auto status = ownedStatus();
  auto invalid = status; invalid.userStorageBindings.pop_back();
  BOOST_CHECK_THROW(invalid.toJson(), std::exception);
  invalid = status; invalid.userStorageBindings[1] = invalid.userStorageBindings[0];
  BOOST_CHECK_THROW(invalid.toJson(), std::exception);
  invalid = status; invalid.userStorageBindings[1].binding.ownerIdentity = "/user/v";
  BOOST_CHECK_THROW(invalid.toJson(), std::exception);
  invalid = status; invalid.userStorageBindings[1].writerIdentity = "/producer";
  BOOST_CHECK_THROW(invalid.toJson(), std::exception);
  invalid = status; invalid.userStorageBindings[1].target.repoNode = "/repo/unselected";
  BOOST_CHECK_THROW(invalid.toJson(), std::exception);
  const auto wire = status.toJson();
  for (const auto& malformed : {
    replace(wire, "\"schemaVersion\":2,\"operationId\"", "\"schemaVersion\":4294967298,\"operationId\""),
    replace(wire, "\"userStorageBindings\":[", "\"userStorageBindings\":null,\"ignored\":["),
    replace(wire, "\"ownerEpoch\":7", "\"ownerEpoch\":0"),
    replace(wire, "\"grantId\":\"replica-grant\"", "\"grantId\":\"replica-grant\",\"grantId\":\"other\""),
    replace(wire, "\"userStorageBindings\"", "\"bindings\"")})
    BOOST_CHECK_THROW(PublishOperationStatus::fromJson(malformed), std::exception);
  BOOST_CHECK_THROW(PublishOperationStatus::fromJson(wire + std::string(1024 * 1024, ' ')), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()
