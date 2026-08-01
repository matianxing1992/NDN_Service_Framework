#include "tests/boost-test.hpp"

#include "ndn-service-framework/GenericSelectionTxnStore.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

namespace ndn_service_framework::test {
namespace {

ndn::Buffer
bytes(const std::string& value)
{
  return ndn::Buffer(
      reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

class NonDiParticipant final : public OpaqueSelectionParticipant
{
public:
  std::string participantId() const override
  {
    return "generic-non-di-fixture";
  }

  uint32_t participantVersion() const override
  {
    return 7;
  }

  OpaqueSelectionPrepareResult
  prepare(const AuthenticatedSelectionContext&,
          ndn::span<const uint8_t> payload) override
  {
    ++prepareCount;
    if (prepareDelay.count() > 0)
      std::this_thread::sleep_for(prepareDelay);
    if (rejectPrepare)
      throw std::runtime_error("fixture rejected payload");
    const std::string input(
        reinterpret_cast<const char*>(payload.data()), payload.size());
    lastPreparedPayload = input;
    auto commitBlob = bytes("opaque-commit:" + input);
    auto acceptance = bytes("opaque-accepted:" + input);
    OpaqueSelectionPrepareResult result;
    result.participantId = participantId();
    result.participantVersion = participantVersion();
    result.commitBlob = commitBlob;
    result.commitBlobDigest = GenericSelectionTxnStore::digest(
        {commitBlob.data(), commitBlob.size()});
    result.acceptancePayload = acceptance;
    result.acceptancePayloadDigest = GenericSelectionTxnStore::digest(
        {acceptance.data(), acceptance.size()});
    if (badDigest)
      result.commitBlobDigest = "sha256:" + std::string(64, '0');
    if (oversized) {
      result.commitBlob = ndn::Buffer(2048);
      std::fill(result.commitBlob.begin(), result.commitBlob.end(), 0x41);
    }
    return result;
  }

  void
  onCommitted(const GenericCommittedSelectionView& view) override
  {
    ++committedCount;
    lastTransaction = view.transactionId;
    if (failProjection)
      throw std::runtime_error("projection failed after commit");
  }

  void
  onAborted(const std::string&, const std::string&) override
  {
    ++abortedCount;
  }

  std::atomic<size_t> prepareCount{0};
  std::atomic<size_t> committedCount{0};
  std::atomic<size_t> abortedCount{0};
  bool rejectPrepare = false;
  bool badDigest = false;
  bool oversized = false;
  bool failProjection = false;
  std::chrono::milliseconds prepareDelay{0};
  std::string lastTransaction;
  std::string lastPreparedPayload;
};

AuthenticatedSelectionContext
contextFor(const std::string& transactionId,
           const ndn::Buffer& payload,
           const std::string& token = "token-record-1",
           const std::string& boot = "boot-epoch-0001")
{
  AuthenticatedSelectionContext context;
  context.transactionId = transactionId;
  context.serviceName = ndn::Name("/generic/service");
  context.requestId = ndn::Name("/request/1");
  context.attempt = 1;
  context.selectionIdentity = "/selection/1";
  context.selectionPayloadDigest = GenericSelectionTxnStore::digest(
      {payload.data(), payload.size()});
  context.providerIdentity = ndn::Name("/provider/a");
  context.providerBootEpoch = boot;
  context.localDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  context.expiresAtUnixMs =
      static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count()) +
      5000;
  context.providerTokenRecordRef = token;
  return context;
}

std::filesystem::path
uniqueRoot(const std::string& suffix)
{
  return std::filesystem::temp_directory_path() /
      ("ndnsf-generic-selection-" + suffix + "-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
}

ndn::Buffer
key(uint8_t value = 0x42)
{
  ndn::Buffer result(32);
  std::fill(result.begin(), result.end(), value);
  return result;
}

} // namespace

BOOST_AUTO_TEST_SUITE(GenericOpaqueSelection)

BOOST_AUTO_TEST_CASE(SelectionAttemptAndMultiAssignmentTupleRoundTrip)
{
  const std::vector<ndn::Buffer> assignments{
      bytes(std::string("first\0opaque", 12)),
      bytes("second-assignment"),
  };
  const auto encoded = encodeOpaqueAssignmentSet(assignments);
  const auto decoded = decodeOpaqueAssignmentSet(encoded);
  BOOST_REQUIRE_EQUAL(decoded.size(), 2);
  BOOST_CHECK(decoded[0] == assignments[0]);
  BOOST_CHECK(decoded[1] == assignments[1]);

  ServiceSelectionMessage selection;
  selection.setAttempt(4);
  selection.setAssignmentPayload(encoded);
  const auto wire = selection.WireEncode();
  ServiceSelectionMessage roundTrip;
  BOOST_REQUIRE(roundTrip.WireDecode(wire));
  BOOST_CHECK_EQUAL(roundTrip.getAttempt(), 4);
  BOOST_CHECK(roundTrip.getAssignmentPayload() == encoded);
}

BOOST_AUTO_TEST_CASE(CollaborationAssignmentEnvelopePreservesOpaqueBytes)
{
  CollaborationAssignmentEnvelope assignment;
  assignment.role = "artifact-replica-0";
  assignment.assignedArtifact = ndn::Name("/publisher/model/root");
  assignment.requiresProvisioning = true;
  assignment.provisioningTimeoutMs = 45000;
  assignment.scopeKeys.emplace(
      "pipeline-stage-0-to-1",
      bytes("01234567890123456789012345678901"));
  assignment.opaquePayload = bytes(std::string("{\"lease\":1}\0opaque", 18));

  const auto encoded = encodeCollaborationAssignmentEnvelope(assignment);
  CollaborationAssignmentEnvelope decoded;
  BOOST_REQUIRE(decodeCollaborationAssignmentEnvelope(encoded, decoded));
  BOOST_CHECK_EQUAL(decoded.role, assignment.role);
  BOOST_CHECK(decoded.assignedArtifact == assignment.assignedArtifact);
  BOOST_CHECK_EQUAL(decoded.requiresProvisioning, true);
  BOOST_CHECK_EQUAL(decoded.provisioningTimeoutMs, 45000);
  BOOST_REQUIRE_EQUAL(decoded.scopeKeys.size(), 1);
  BOOST_CHECK(decoded.scopeKeys.at("pipeline-stage-0-to-1") ==
              assignment.scopeKeys.at("pipeline-stage-0-to-1"));
  BOOST_CHECK(decoded.opaquePayload == assignment.opaquePayload);

  CollaborationAssignmentEnvelope untouched;
  BOOST_CHECK(!decodeCollaborationAssignmentEnvelope(
      bytes("role=legacy;"), untouched));
  BOOST_CHECK_THROW(
      encodeCollaborationAssignmentEnvelope(CollaborationAssignmentEnvelope{}),
      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CommitIsDurableEncryptedAndIdenticalReplayIsIdempotent)
{
  const auto root = uniqueRoot("commit");
  std::filesystem::create_directories(root);
  const auto wal = (root / "selection.wal").string();
  const auto payload = bytes("opaque-non-di-selection");
  NonDiParticipant participant;
  {
    GenericSelectionTxnStore store(wal, key(), "key-epoch-1");
    const auto context = contextFor("txn-1", payload);
    const auto first = store.commit(
        context, {payload.data(), payload.size()}, participant, true);
    const auto replay = store.commit(
        context, {payload.data(), payload.size()}, participant, true);
    BOOST_CHECK_EQUAL(first.acceptancePayloadDigest,
                      replay.acceptancePayloadDigest);
    BOOST_CHECK_EQUAL(store.size(), 1);
    BOOST_CHECK_EQUAL(participant.prepareCount.load(), 1);
    BOOST_CHECK_EQUAL(participant.committedCount.load(), 1);

    auto conflict = context;
    conflict.selectionPayloadDigest =
        GenericSelectionTxnStore::digest(
            {reinterpret_cast<const uint8_t*>("other"), 5});
    const auto other = bytes("other");
    BOOST_CHECK_THROW(
        store.commit(
            conflict, {other.data(), other.size()}, participant, true),
        std::runtime_error);
  }
  std::ifstream file(wal, std::ios::binary);
  const std::string wire(
      (std::istreambuf_iterator<char>(file)),
      std::istreambuf_iterator<char>());
  BOOST_CHECK(wire.find("opaque-commit") == std::string::npos);
  BOOST_CHECK(wire.find("opaque-accepted") == std::string::npos);

  NonDiParticipant recovered;
  auto recoveredPtr = std::shared_ptr<OpaqueSelectionParticipant>(
      &recovered, [](OpaqueSelectionParticipant*) {});
  GenericSelectionTxnStore reopened(wal, key(), "key-epoch-1");
  BOOST_REQUIRE(reopened.findCommitted("txn-1"));
  reopened.replayCommitted(
      {{recovered.participantId(), recoveredPtr}}, "boot-epoch-0001");
  BOOST_CHECK_EQUAL(recovered.committedCount.load(), 1);
  reopened.replayCommitted(
      {{recovered.participantId(), recoveredPtr}}, "new-boot-epoch");
  BOOST_CHECK_EQUAL(recovered.committedCount.load(), 1);
  std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(PrepareBoundsDigestDeadlineAndProjectionCrashFailSafely)
{
  const auto root = uniqueRoot("failures");
  std::filesystem::create_directories(root);
  const auto payload = bytes("payload");
  GenericSelectionTxnOptions options;
  options.maxCommitBlobBytes = 1024;
  GenericSelectionTxnStore store(
      (root / "selection.wal").string(), key(), "key-epoch-1", options);

  NonDiParticipant rejecting;
  rejecting.rejectPrepare = true;
  BOOST_CHECK_THROW(
      store.commit(
          contextFor("txn-reject", payload),
          {payload.data(), payload.size()}, rejecting, true),
      std::runtime_error);
  BOOST_CHECK_EQUAL(rejecting.abortedCount.load(), 1);
  BOOST_CHECK(!store.findCommitted("txn-reject"));

  NonDiParticipant badDigest;
  badDigest.badDigest = true;
  BOOST_CHECK_THROW(
      store.commit(
          contextFor("txn-digest", payload, "token-2"),
          {payload.data(), payload.size()}, badDigest, true),
      std::runtime_error);
  BOOST_CHECK(!store.findCommitted("txn-digest"));

  NonDiParticipant oversized;
  oversized.oversized = true;
  BOOST_CHECK_THROW(
      store.commit(
          contextFor("txn-large", payload, "token-3"),
          {payload.data(), payload.size()}, oversized, true),
      std::length_error);
  BOOST_CHECK(!store.findCommitted("txn-large"));

  NonDiParticipant expired;
  auto expiredContext = contextFor("txn-expired", payload, "token-4");
  expiredContext.localDeadline =
      std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
  BOOST_CHECK_THROW(
      store.commit(
          expiredContext, {payload.data(), payload.size()}, expired, true),
      std::runtime_error);
  BOOST_CHECK_EQUAL(expired.prepareCount.load(), 0);

  NonDiParticipant projectionFailure;
  projectionFailure.failProjection = true;
  BOOST_CHECK_THROW(
      store.commit(
          contextFor("txn-projection", payload, "token-5"),
          {payload.data(), payload.size()}, projectionFailure, true),
      std::runtime_error);
  BOOST_CHECK(store.findCommitted("txn-projection"));
  std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(ConcurrentDuplicateCommitsOneRecordAndOneProjection)
{
  const auto root = uniqueRoot("concurrent");
  std::filesystem::create_directories(root);
  const auto payload = bytes("concurrent-payload");
  GenericSelectionTxnStore store(
      (root / "selection.wal").string(), key(), "key-epoch-1");
  NonDiParticipant participant;
  const auto context = contextFor("txn-concurrent", payload);
  std::vector<std::thread> threads;
  std::atomic<size_t> successes{0};
  for (size_t i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      try {
        store.commit(
            context, {payload.data(), payload.size()}, participant, true);
        ++successes;
      }
      catch (...) {
      }
    });
  }
  for (auto& thread : threads)
    thread.join();
  BOOST_CHECK_EQUAL(successes.load(), 8);
  BOOST_CHECK_EQUAL(store.size(), 1);
  BOOST_CHECK_EQUAL(participant.prepareCount.load(), 1);
  BOOST_CHECK_EQUAL(participant.committedCount.load(), 1);
  std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(PrepareDeadlineIsEnforcedWithoutWaitingForCallbackReturn)
{
  const auto root = uniqueRoot("prepare-timeout");
  std::filesystem::create_directories(root);
  const auto payload = bytes("slow-payload");
  GenericSelectionTxnOptions options;
  options.maxPrepareTime = std::chrono::milliseconds(15);
  GenericSelectionTxnStore store(
      (root / "selection.wal").string(), key(), "key-epoch-1", options);
  NonDiParticipant participant;
  participant.prepareDelay = std::chrono::milliseconds(100);
  const auto started = std::chrono::steady_clock::now();
  BOOST_CHECK_THROW(
      store.commit(
          contextFor("txn-timeout", payload),
          {payload.data(), payload.size()}, participant, true),
      std::runtime_error);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  BOOST_CHECK_LT(elapsed.count(), 80);
  BOOST_CHECK_EQUAL(participant.abortedCount.load(), 1);
  BOOST_CHECK(!store.findCommitted("txn-timeout"));
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(TornTailTruncatesButWrongKeyFailsClosed)
{
  const auto root = uniqueRoot("recovery");
  std::filesystem::create_directories(root);
  const auto wal = (root / "selection.wal").string();
  const auto payload = bytes("payload");
  NonDiParticipant participant;
  {
    GenericSelectionTxnStore store(wal, key(), "key-epoch-1");
    store.commit(
        contextFor("txn-1", payload),
        {payload.data(), payload.size()}, participant, true);
  }
  const auto validSize = std::filesystem::file_size(wal);
  {
    std::ofstream output(wal, std::ios::binary | std::ios::app);
    output.write("NDNT", 4);
  }
  GenericSelectionTxnStore recovered(wal, key(), "key-epoch-1");
  BOOST_CHECK_EQUAL(std::filesystem::file_size(wal), validSize);
  BOOST_CHECK_THROW(
      GenericSelectionTxnStore(wal, key(0x55), "key-epoch-1"),
      std::runtime_error);
  std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(ServiceProviderSeamCommitsBeforeProjectionAndReplays)
{
  ndn::security::KeyChain keyChain(
      "pib-memory:opaque-provider", "tpm-memory:opaque-provider");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requester("/test/user/opaque");
  const ndn::Name providerName("/test/provider/opaque");
  const ndn::Name service("/Generic/ImageTransform");
  const ndn::Name requestId("request-opaque-1");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto authorityCert =
      makeRsaIdentity(keyChain, ndn::Name("/test/opaque-authority"));
  LocalServiceProvider provider(
      face, ndn::Name("/test/group"), providerCert, authorityCert,
      "examples/trust-any.conf");
  const auto root = uniqueRoot("provider-seam");
  std::filesystem::create_directories(root);
  auto store = std::make_shared<GenericSelectionTxnStore>(
      (root / "selection.wal").string(), key(), "key-epoch-1");
  auto participant = std::make_shared<NonDiParticipant>();
  provider.setGenericSelectionTxnStore(store);
  provider.registerOpaqueSelectionParticipant(service, participant);

  RequestMessage request;
  auto requestPayload = bytes("request");
  request.setPayload(requestPayload, requestPayload.size());
  provider.addPendingRequestForTokenTest(
      requester, service, requestId, request, "provider-token");
  provider.schedulePendingRequestCleanupForTest(
      requester, service, requestId, ndn::time::seconds(5));
  const auto originalExpiry = provider.pendingCleanupExpiryUnixMsForTest(
      requester, service, requestId);
  provider.schedulePendingRequestCleanupForTest(
      requester, service, requestId, ndn::time::seconds(30));
  BOOST_CHECK_EQUAL(
      provider.pendingCleanupExpiryUnixMsForTest(
          requester, service, requestId),
      originalExpiry);
  provider.schedulePendingRequestCleanupForTest(
      requester, service, requestId, ndn::time::seconds(60), true);
  const auto authoritativeExpiry =
      provider.pendingCleanupExpiryUnixMsForTest(
          requester, service, requestId);
  BOOST_CHECK_GT(authoritativeExpiry, originalExpiry + 50000);
  provider.schedulePendingRequestCleanupForTest(
      requester, service, requestId, ndn::time::seconds(120), true);
  BOOST_CHECK_EQUAL(
      provider.pendingCleanupExpiryUnixMsForTest(
          requester, service, requestId),
      authoritativeExpiry);
  ServiceSelectionMessage selection;
  selection.setProviderToken("provider-token");
  selection.setAssignmentPayload(bytes("opaque-work-order"));
  const auto wire = selection.WireEncode();
  const auto deliver = [&] {
    provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
        requester, providerName, service, requestId,
        ndn::Buffer(wire.data(), wire.size()));
  };
  deliver();
  BOOST_CHECK_EQUAL(store->size(), 1);
  BOOST_CHECK_EQUAL(participant->prepareCount.load(), 1);
  BOOST_CHECK_EQUAL(participant->committedCount.load(), 1);
  BOOST_CHECK(!provider.hasPendingRequestForTokenTest(
      requester, service, requestId));
  BOOST_CHECK_EQUAL(provider.getTokenConsumeCountForTesting(), 1);

  deliver();
  BOOST_CHECK_EQUAL(store->size(), 1);
  BOOST_CHECK_EQUAL(participant->prepareCount.load(), 1);
  BOOST_CHECK_EQUAL(participant->committedCount.load(), 2);
  BOOST_CHECK_EQUAL(provider.getTokenConsumeCountForTesting(), 1);
  std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(
    ServiceProviderOpaqueParticipantReceivesExactDeferredAssignmentBytes)
{
  ndn::security::KeyChain keyChain(
      "pib-memory:opaque-deferred-provider",
      "tpm-memory:opaque-deferred-provider");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requester("/test/user/opaque-deferred");
  const ndn::Name providerName("/test/provider/opaque-deferred");
  const ndn::Name service("/Generic/Pipeline");
  const ndn::Name requestId("request-opaque-deferred-1");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto authorityCert =
      makeRsaIdentity(keyChain, ndn::Name("/test/opaque-deferred-authority"));
  LocalServiceProvider provider(
      face, ndn::Name("/test/group"), providerCert, authorityCert,
      "examples/trust-any.conf");
  const auto root = uniqueRoot("provider-deferred-envelope");
  std::filesystem::create_directories(root);
  auto store = std::make_shared<GenericSelectionTxnStore>(
      (root / "selection.wal").string(), key(), "key-epoch-1");
  auto participant = std::make_shared<NonDiParticipant>();
  provider.setGenericSelectionTxnStore(store);
  provider.registerOpaqueSelectionParticipant(service, participant);

  RequestMessage request;
  auto requestPayload = bytes("request");
  request.setPayload(requestPayload, requestPayload.size());
  provider.addPendingRequestForTokenTest(
      requester, service, requestId, request, "provider-token");
  provider.schedulePendingRequestCleanupForTest(
      requester, service, requestId, ndn::time::seconds(5));

  const std::string canonicalAssignment =
      R"({"attempt":1,"provider":"/test/provider/opaque-deferred","schema":"ndnsf-di-selection-assignment-v2"})";
  CollaborationAssignmentEnvelope envelope;
  envelope.role = "stage-0";
  envelope.assignedArtifact = ndn::Name("/models/qwen/stage-0");
  envelope.requiresProvisioning = true;
  envelope.provisioningTimeoutMs = 180000;
  envelope.opaquePayload = bytes(canonicalAssignment);

  SelectionProviderEntry entry;
  entry.providerName = providerName;
  entry.providerTokenHash = computeSelectionProviderTokenProofHash(
      requester, providerName, service, "provider-token");
  entry.assignmentPayload = encodeCollaborationAssignmentEnvelope(envelope);
  ServiceSelectionMessage selection;
  selection.setAttempt(1);
  selection.addProviderEntry(entry);
  const auto wire = selection.WireEncode();
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
      requester, providerName, service, requestId,
      ndn::Buffer(wire.data(), wire.size()));

  BOOST_CHECK_EQUAL(store->size(), 1);
  BOOST_CHECK_EQUAL(participant->prepareCount.load(), 1);
  BOOST_CHECK_EQUAL(participant->committedCount.load(), 1);
  BOOST_CHECK_EQUAL(participant->lastPreparedPayload, canonicalAssignment);
  std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
