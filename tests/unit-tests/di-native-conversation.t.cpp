#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "tests/fixtures/spec182/native-sampling-epoch.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationJournal.hpp"
#include <fstream>
#include <cmath>
#include <limits>

#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>

#include <filesystem>
#include <unistd.h>

namespace ndnsf::di {
namespace {
std::string digest(const std::string& value)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);
  std::string result = "sha256:";
  for (const auto byte : hash) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}
} // namespace

BOOST_AUTO_TEST_SUITE(Spec182ConversationWire)
BOOST_AUTO_TEST_CASE(RuntimeStateCommitmentAndLogicalTranscriptAreBothVerified)
{
  std::ifstream input("tests/fixtures/spec182/conversation-oracle.json");
  const auto oracle = NativeJson::parse(input);
  for (const auto& fixture : oracle.at("nativeStateCases")) {
    const auto tokens = fixture.at("canonicalTokenIds").get<std::vector<std::int64_t>>();
    const auto& transcript = fixture.at("transcript");
    const auto cp = NativeJson::parse(fixture.at("checkpointWire").get<std::string>());
    BOOST_CHECK_EQUAL(nativeGenerationStatePrefixDigest(tokens, 1, transcript.at("tokenizerDigest").get<std::string>()),
                      fixture.at("runtimePrefixDigest").get<std::string>());
    BOOST_CHECK_NE(fixture.at("runtimePrefixDigest").get<std::string>(), cp.at("logicalPrefixDigest").get<std::string>());
    BOOST_CHECK_NO_THROW(nativeValidateConversationTranscript(transcript, cp, 1));
    BOOST_CHECK_THROW(nativeValidateConversationTranscript(transcript, cp), std::invalid_argument);
    BOOST_CHECK_THROW(nativeValidateConversationTranscript(transcript, cp, 0), std::invalid_argument);
    BOOST_CHECK_THROW(nativeValidateConversationTranscript(transcript, cp, 2), std::invalid_argument);
  }
}
BOOST_AUTO_TEST_CASE(LegacyEncryptedEnvelopeAndSubkeyParity)
{
  std::ifstream input("tests/fixtures/spec182/conversation-oracle.json");
  const auto oracle = NativeJson::parse(input);
  std::vector<std::uint8_t> key(32);
  for (std::size_t i = 0; i != key.size(); ++i) key[i] = static_cast<std::uint8_t>(i);
  const std::vector<NativeConversationJournalKey> keys{{"fixture-key", key}};
  const auto derived = nativeConversationAuthenticationKeys("fixture-owner", keys);
  BOOST_REQUIRE_EQUAL(derived.size(), 1);
  std::string hex;
  for (const auto byte : derived.front()) {
    hex += "0123456789abcdef"[byte >> 4]; hex += "0123456789abcdef"[byte & 15];
  }
  BOOST_CHECK_EQUAL(hex, oracle.at("journal").at("authenticationSubkeyHex").get<std::string>());
  for (const auto& envelope : oracle.at("journal").at("envelopes")) {
    const auto id = envelope.at("envelopeId").get<std::string>();
    const auto encoded = envelope.at("encoded").get<std::string>();
    BOOST_CHECK_EQUAL(nativeReadConversationEnvelope(encoded, id, keys, 2'000'000'000'003ULL),
                      envelope.at("plaintext").get<std::string>());
    BOOST_CHECK_THROW(nativeReadConversationEnvelope(encoded, id, {{"fixture-key", std::vector<std::uint8_t>(32, 9)}},
                                                    2'000'000'000'003ULL), std::invalid_argument);
    BOOST_CHECK_THROW(nativeReadConversationEnvelope(encoded, id + "x", keys, 2'000'000'000'003ULL), std::invalid_argument);
    BOOST_CHECK_THROW(nativeReadConversationEnvelope(encoded, id, keys, 2'000'000'100'003ULL), std::invalid_argument);
    auto changed = NativeJson::parse(encoded);
    changed["keyId"] = "unknown";
    BOOST_CHECK_THROW(nativeReadConversationEnvelope(nativeCanonicalJson(changed), id, keys, 2'000'000'000'003ULL), std::invalid_argument);
    changed = NativeJson::parse(encoded);
    auto ciphertext = changed.at("ciphertext").get<std::string>();
    ciphertext[8] = ciphertext[8] == 'A' ? 'B' : 'A'; changed["ciphertext"] = ciphertext;
    BOOST_CHECK_THROW(nativeReadConversationEnvelope(nativeCanonicalJson(changed), id, keys, 2'000'000'000'003ULL), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(LegacyCheckpointAndTranscriptWireParity)
{
  std::ifstream input("tests/fixtures/spec182/conversation-oracle.json");
  const auto oracle = NativeJson::parse(input);
  std::vector<std::uint8_t> key(32);
  for (std::size_t i = 0; i != key.size(); ++i) key[i] = static_cast<std::uint8_t>(i);
  const std::vector<std::uint8_t> wrongKey(32, 0xff);
  BOOST_REQUIRE_EQUAL(oracle.at("cases").size(), 4);
  for (const auto& fixture : oracle.at("cases")) {
    const auto wire = fixture.at("checkpointWire").get<std::string>();
    const auto checkpoint = nativeReadConversationCheckpoint(wire, {key}, 2'000'000'000'003ULL);
    BOOST_CHECK_EQUAL(checkpoint.at("checkpointDigest").get<std::string>(),
                      fixture.at("checkpointDigest").get<std::string>());
    auto unsignedValue = checkpoint;
    unsignedValue.erase("checkpointDigest"); unsignedValue.erase("signature");
    BOOST_CHECK_EQUAL(nativeSignConversationCheckpoint(unsignedValue, key), wire);
    BOOST_CHECK_EQUAL(nativeConversationPrefixDigest(fixture.at("canonicalTokenIds").get<std::vector<std::int64_t>>()),
                      checkpoint.at("logicalPrefixDigest").get<std::string>());
    BOOST_CHECK_THROW(nativeReadConversationCheckpoint(wire, {wrongKey}, 2'000'000'000'003ULL), std::invalid_argument);
    BOOST_CHECK_NO_THROW(nativeReadConversationCheckpoint(wire, {wrongKey, key}, 2'000'000'000'003ULL));
    BOOST_CHECK_THROW(nativeReadConversationCheckpoint(wire, {key}, 2'000'000'100'003ULL), std::invalid_argument);
    auto changed = checkpoint;
    changed["prefixTokenCount"] = changed.at("prefixTokenCount").get<std::uint64_t>() + 1;
    BOOST_CHECK_THROW(nativeReadConversationCheckpoint(nativeCanonicalJson(changed), {key}, 2'000'000'000'003ULL), std::invalid_argument);
    BOOST_CHECK_THROW(nativeReadConversationCheckpoint(wire + " ", {key}, 2'000'000'000'003ULL), std::invalid_argument);
    unsignedValue["contextEpoch"] = 1.5;
    BOOST_CHECK_THROW(nativeSignConversationCheckpoint(unsignedValue, key), std::invalid_argument);
    if (fixture.contains("transcript")) {
      const auto transcript = fixture.at("transcript");
      BOOST_CHECK_NO_THROW(nativeValidateConversationTranscript(transcript, checkpoint));
      changed = transcript; changed["canonicalTokenIds"][0] = 99;
      BOOST_CHECK_THROW(nativeValidateConversationTranscript(changed, checkpoint), std::invalid_argument);
      changed = transcript; changed["providerRoleReceipts"].erase(0);
      BOOST_CHECK_THROW(nativeValidateConversationTranscript(changed, checkpoint), std::invalid_argument);
      changed = transcript; changed["providerRoleReceipts"][1] = changed["providerRoleReceipts"][0];
      BOOST_CHECK_THROW(nativeValidateConversationTranscript(changed, checkpoint), std::invalid_argument);
      changed = transcript; changed["applicationMessages"] = "!@#$";
      BOOST_CHECK_THROW(nativeValidateConversationTranscript(changed, checkpoint), std::invalid_argument);
      changed = transcript; changed["contextEpoch"] = 99;
      BOOST_CHECK_THROW(nativeValidateConversationTranscript(changed, checkpoint), std::invalid_argument);
    }
  }
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(Spec182ConversationJournal)
BOOST_AUTO_TEST_CASE(LegacyReadNativeAppendAndWriterLease)
{
  std::ifstream input("tests/fixtures/spec182/conversation-oracle.json");
  const auto oracle = NativeJson::parse(input);
  char directory[] = "/tmp/spec182-native-journal-XXXXXX";
  BOOST_REQUIRE(::mkdtemp(directory) != nullptr);
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); }
  } cleanup{directory};
  std::vector<std::uint8_t> key(32);
  for (std::size_t i = 0; i != key.size(); ++i) key[i] = static_cast<std::uint8_t>(i);
  NativeConversationJournalConfig config{directory, "fixture-owner", {{"fixture-key", key}},
                                         64 * 1024 * 1024, true};
  const auto journalPath = std::filesystem::path(directory) / "fixture-owner" / "journal.jsonl";
  std::filesystem::create_directories(journalPath.parent_path());
  { std::ofstream output(journalPath); output << oracle.at("journal").at("journalWire").get<std::string>(); }
  {
    NativeConversationJournal journal(config);
    BOOST_CHECK_THROW(NativeConversationJournal{config}, std::runtime_error);
    const auto restored = journal.readConversations(2'000'000'000'003ULL);
    BOOST_REQUIRE_EQUAL(restored.size(), 2);
    for (std::size_t i = 0; i != restored.size(); ++i) {
      BOOST_CHECK_EQUAL(restored[i].at("checkpointWire").get<std::string>(),
        oracle.at("cases")[i + 2].at("checkpointWire").get<std::string>());
      BOOST_CHECK(restored[i].at("transcript") == oracle.at("cases")[i + 2].at("transcript"));
    }
    BOOST_CHECK(journal.readConversations(2'000'000'100'003ULL).empty());
  }
  // Fresh native write must survive reopen without a spool mirror.
  std::filesystem::remove(journalPath);
  {
    NativeConversationJournal journal(config);
    for (std::size_t i = 2; i != 4; ++i) {
      const auto& fixture = oracle.at("cases")[i];
      journal.appendConversation(fixture.at("checkpointWire").get<std::string>(),
                                 fixture.at("transcript"), 2'000'000'000'003ULL);
    }
    BOOST_CHECK_EQUAL(journal.readConversations(2'000'000'000'003ULL).size(), 2);
  }
  const auto committedSize = std::filesystem::file_size(journalPath);
  { std::ofstream output(journalPath, std::ios::app); output << "{\"schema\":"; }
  {
    NativeConversationJournal recovered(config);
    BOOST_CHECK_EQUAL(recovered.readConversations(2'000'000'000'003ULL).size(), 2);
    BOOST_CHECK_EQUAL(std::filesystem::file_size(journalPath), committedSize);
  }
  config.identity = "quota-owner";
  const auto quotaPath = std::filesystem::path(directory) / config.identity / "journal.jsonl";
  {
    NativeConversationJournal initial(config);
    const auto& fixture = oracle.at("cases")[2];
    initial.appendConversation(fixture.at("checkpointWire").get<std::string>(),
                               fixture.at("transcript"), 2'000'000'000'003ULL);
  }
  const auto quotaSize = std::filesystem::file_size(quotaPath);
  config.quotaBytes = quotaSize;
  {
    NativeConversationJournal bounded(config);
    const auto& fixture = oracle.at("cases")[3];
    BOOST_CHECK_THROW(bounded.appendConversation(fixture.at("checkpointWire").get<std::string>(),
                      fixture.at("transcript"), 2'000'000'000'003ULL), std::runtime_error);
    BOOST_CHECK_EQUAL(std::filesystem::file_size(quotaPath), quotaSize);
    BOOST_CHECK_EQUAL(bounded.readConversations(2'000'000'000'003ULL).size(), 1);
  }
  // Corruption of a complete record is never treated as a torn append.
  {
    std::fstream file(quotaPath, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(20); file.put('!');
  }
  BOOST_CHECK_THROW(NativeConversationJournal{config}, std::exception);
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(Spec182ConversationConfig)
BOOST_AUTO_TEST_CASE(SharedNativeConfigLoaderEnforcesOwnerAndPathIdentity)
{
  char directory[] = "/tmp/spec182-conversation-config-XXXXXX";
  BOOST_REQUIRE(::mkdtemp(directory) != nullptr);
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); }
  } cleanup{directory};
  const auto root = std::filesystem::path(directory);
  std::filesystem::create_directories(root / "keys");
  const auto keyPath = root / "keys" / "conversation.key";
  {
    std::ofstream output(keyPath, std::ios::binary);
    const std::string key(32, '\x01');
    output.write(key.data(), key.size());
  }
  BOOST_REQUIRE(::chmod(keyPath.c_str(), 0600) == 0);
  const auto configuration = NativeJson{
    {"schema", "ndnsf-di-native-conversation-v1"},
    {"journal", {
      {"state_root", "state"}, {"identity", "fixture-owner"},
      {"keys", NativeJson::array({NativeJson{{"id", "active"}, {"file", "keys/conversation.key"}}})},
      {"quota_bytes", 64 * 1024 * 1024}, {"test_only_allow_ephemeral_state_root", true},
    }},
    {"owner", {
      {"requester_identity", "/requester/A"}, {"service_name", "/service/test"},
      {"security_domain_digest", digest("security")},
    }},
  };
  auto coordinator = nativeConversationCoordinatorFromConfig(
    nativeCanonicalJson(configuration), root, "/requester/A");
  BOOST_REQUIRE(coordinator != nullptr);
  BOOST_CHECK_THROW(nativeConversationCoordinatorFromConfig(
    nativeCanonicalJson(configuration), root, "/requester/B"), std::invalid_argument);
  auto escaped = configuration;
  escaped["journal"]["keys"][0]["file"] = "../outside.key";
  BOOST_CHECK_THROW(nativeConversationCoordinatorFromConfig(
    nativeCanonicalJson(escaped), root, "/requester/A"), std::invalid_argument);
}
BOOST_AUTO_TEST_SUITE_END()

namespace {
struct ConversationOwnerFixture
{
  ConversationOwnerFixture()
  {
    std::ifstream input("tests/fixtures/spec182/conversation-oracle.json");
    oracle = NativeJson::parse(input);
    char directory[] = "/tmp/spec182-conversation-owner-XXXXXX";
    if (!::mkdtemp(directory)) throw std::runtime_error("test temporary directory failed");
    root = directory;
    std::vector<std::uint8_t> key(32);
    for (std::size_t i = 0; i != key.size(); ++i) key[i] = static_cast<std::uint8_t>(i);
    config.journal = std::make_shared<NativeConversationJournal>(NativeConversationJournalConfig{
      root, "fixture-owner", {{"fixture-key", key}}, 64 * 1024 * 1024, true});
    config.authenticationKeys = {key};
    config.serviceName = "/service/会话"; config.requesterIdentity = "/requester/A";
    config.securityDomainDigest = digest("security");
    config.nowMs = [&] { return now; };
  }
  ~ConversationOwnerFixture()
  {
    config.journal.reset();
    std::error_code ec; std::filesystem::remove_all(root, ec);
  }
  NativeConversationContinuation continuation(std::size_t epoch, const std::string& parent = {}) const
  {
    NativeConversationContinuation value;
    value.conversationId = "0123456789abcdef0123456789abcdef";
    value.parentContextEpoch = epoch - 1; value.serviceName = config.serviceName;
    value.planRoleMapDigest = digest("roles"); value.requestContractDigest = digest("request-contract");
    value.retentionDeadlineMs = 2'000'000'100'000ULL + epoch;
    value.generationId = std::string(32, '1'); value.expectedRoles = {"/role/A", "/role/B"};
    value.canonicalTokenIds = epoch == 1 ? std::vector<std::int64_t>{10} : std::vector<std::int64_t>{10, 11, 12};
    if (epoch > 1) {
      value.mode = "APPEND_DELTA"; value.parentCheckpointWire = parent;
      value.parentCheckpointDigest = NativeJson::parse(parent).at("checkpointDigest").get<std::string>();
    }
    return value;
  }
  NativeCompletedAttempt completed(std::size_t epoch)
  {
    const auto& value = nativeState ? oracle.at("nativeStateCases")[epoch - 1].at("transcript") :
                                     oracle.at("cases")[epoch + 1].at("transcript");
    NativeCompletedAttempt result;
    result.requestId = "/request/" + std::to_string(epoch); result.attempt = 1; result.complete = true;
    result.tokenIds = value.at("canonicalTokenIds").get<std::vector<std::int64_t>>();
    result.generationId = std::string(32, '1'); result.modelContractDigest = digest("model");
    result.tokenizerDigest = digest("tokenizer"); result.chatTemplateDigest = digest("template");
    result.applicationMessages = nativeConversationBase64Decode(value.at("applicationMessages").get<std::string>());
    for (const auto& encoded : value.at("providerRoleReceipts"))
      result.authenticatedReceipts.push_back(nativeParseJson(nativeConversationBase64Decode(encoded.get<std::string>())));
    result.commitProviderState = [&](const auto&) { ++promotions; if (duringPromotion) duringPromotion(); };
    result.rollbackProviderState = [&] { ++rollbacks; };
    return result;
  }
  NativeJson oracle;
  std::filesystem::path root;
  NativeConversationConfig config;
  std::uint64_t now = 2'000'000'000'001ULL;
  std::size_t promotions = 0, rollbacks = 0;
  std::function<void()> duringPromotion;
  bool nativeState = false;
};
}

BOOST_AUTO_TEST_SUITE(Spec182Conversation)
BOOST_AUTO_TEST_CASE(PlacementSurvivesJournalReopenAndExplicitLongRetention)
{
  ConversationOwnerFixture fixture;
  const std::map<std::string, std::string> placement{
    {"/role/A", "/provider/A"}, {"/role/B", "/provider/B"}};
  auto first = fixture.continuation(1);
  first.planRoleMapDigest.clear(); first.expectedRoles.clear();
  first.retentionDeadlineMs = fixture.now + 900'000;
  std::string wire;
  std::string roleDigest;
  {
    NativeConversationCoordinator owner(fixture.config);
    const auto initial = owner.beginTurn(first, "/request/1");
    const auto turn = owner.bindInitialPlanRoleMap(initial, placement);
    BOOST_CHECK(turn.providersByRole == placement);
    roleDigest = turn.parent.planRoleMapDigest;
    auto completed = fixture.completed(1);
    for (auto& receipt : completed.authenticatedReceipts) {
      receipt["planRoleMapDigest"] = roleDigest;
      receipt["expiresAtMs"] = first.retentionDeadlineMs;
      receipt.erase("receiptDigest"); receipt.erase("signature");
      receipt["receiptDigest"] = digest(nativeConversationCanonicalJson(receipt));
    }
    owner.acceptTokenPrefix(turn, {11});
    const auto prepared = owner.prepareCheckpoint(turn, completed);
    BOOST_CHECK_EQUAL(NativeJson::parse(prepared.wire).at("expiresAtMs").get<std::uint64_t>(),
                      first.retentionDeadlineMs);
    wire = owner.commitTurn(turn, prepared).checkpoint.wire;
  }
  // Reopen after the old implicit five-minute cap, not merely a new request
  // against a still-live in-memory coordinator. The fixture removes its journal.
  fixture.now += 400'000;
  fixture.config.journal.reset();
  fixture.config.journal = std::make_shared<NativeConversationJournal>(NativeConversationJournalConfig{
    fixture.root, "fixture-owner", {{"fixture-key", fixture.config.authenticationKeys.front()}},
    64 * 1024 * 1024, true});
  {
    NativeConversationCoordinator recovered(fixture.config);
    auto next = fixture.continuation(2, wire);
    next.planRoleMapDigest = roleDigest;
    next.retentionDeadlineMs = first.retentionDeadlineMs;
    const auto turn = recovered.beginTurn(next, "/request/2");
    BOOST_CHECK(turn.providersByRole == placement);
    auto completed = fixture.completed(2);
    const auto receiptDeadline = fixture.now + 100'000;
    for (auto& receipt : completed.authenticatedReceipts) {
      receipt["planRoleMapDigest"] = roleDigest;
      receipt["expiresAtMs"] = receiptDeadline;
      receipt.erase("receiptDigest"); receipt.erase("signature");
      receipt["receiptDigest"] = digest(nativeConversationCanonicalJson(receipt));
    }
    recovered.acceptTokenPrefix(turn, {13});
    const auto prepared = recovered.prepareCheckpoint(turn, completed);
    BOOST_CHECK_EQUAL(NativeJson::parse(prepared.wire).at("expiresAtMs").get<std::uint64_t>(),
                      receiptDeadline);
    BOOST_CHECK(prepared.providersByRole == placement);
    recovered.commitTurn(turn, prepared);
    fixture.now = receiptDeadline;
  }
  NativeConversationCoordinator expired(fixture.config);
  auto next = fixture.continuation(2, wire);
  next.planRoleMapDigest = roleDigest; next.retentionDeadlineMs = first.retentionDeadlineMs;
  // Even a still-unexpired old parent cannot replace the committed successor.
  BOOST_CHECK_THROW(expired.beginTurn(next, "/request/stale"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(AuthenticatedJournalPlacementMustMatchCheckpointDigest)
{
  ConversationOwnerFixture fixture;
  NativeConversationCheckpoint checkpoint;
  {
    auto config = fixture.config;
    config.journal.reset(); // Produce a valid checkpoint without appending yet.
    NativeConversationCoordinator owner(config);
    const auto turn = owner.beginTurn(fixture.continuation(1), "/request/1");
    owner.acceptTokenPrefix(turn, {11});
    checkpoint = owner.commitTurn(turn, owner.prepareCheckpoint(turn, fixture.completed(1))).checkpoint;
  }
  // Authentic encryption alone cannot bless a role mapping that does not hash
  // to the parent checkpoint. This intentionally malformed test journal is RAII-owned.
  fixture.config.journal->appendConversation(checkpoint.wire, checkpoint.transcript, fixture.now,
    checkpoint.nativeInitialPromptTokenCount, {{"/role/A", "/wrong/A"}, {"/role/B", "/wrong/B"}});
  BOOST_CHECK_THROW(NativeConversationCoordinator(fixture.config), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ExplicitOwnerConfigurationFencesPathOnlyConstruction)
{
  ConversationOwnerFixture fixture;
  BOOST_CHECK_THROW(NativeConversationCoordinator(fixture.root), std::invalid_argument);
  NativeConversationCoordinator owner(fixture.config);
  BOOST_CHECK(owner.find("0123456789abcdef0123456789abcdef") == std::nullopt);
  auto invalid = fixture.config;
  invalid.requesterIdentity.clear();
  BOOST_CHECK_THROW(NativeConversationCoordinator(std::move(invalid)), std::exception);
}

BOOST_AUTO_TEST_CASE(NativeStateReceiptsRetainOriginalPromptBoundaryAcrossRestore)
{
  ConversationOwnerFixture fixture;
  fixture.nativeState = true;
  std::string parent;
  for (std::size_t epoch = 1; epoch != 3; ++epoch) {
    // Recreate the owner each turn so the original prefill count must come
    // from the durable journal, rather than a surviving in-memory turn.
    NativeConversationCoordinator owner(fixture.config);
    const auto turn = owner.beginTurn(fixture.continuation(epoch, parent), "/request/" + std::to_string(epoch));
    owner.acceptTokenPrefix(turn, {epoch == 1 ? 11 : 13});
    const auto result = owner.commitTurn(turn, owner.prepareCheckpoint(turn, fixture.completed(epoch)));
    parent = result.checkpoint.wire;
    BOOST_CHECK_EQUAL(parent, fixture.oracle.at("nativeStateCases")[epoch - 1].at("checkpointWire").get<std::string>());
    BOOST_REQUIRE(result.checkpoint.nativeInitialPromptTokenCount);
    BOOST_CHECK_EQUAL(*result.checkpoint.nativeInitialPromptTokenCount, 1);
    ++fixture.now;
  }
}
BOOST_AUTO_TEST_CASE(FullContextAppendAndAuthenticatedRestore)
{
  ConversationOwnerFixture fixture;
  std::string first, second;
  {
    NativeConversationCoordinator owner(fixture.config);
    const auto turn = owner.beginTurn(fixture.continuation(1), "/request/1");
    owner.acceptTokenPrefix(turn, {11});
    const auto prepared = owner.prepareCheckpoint(turn, fixture.completed(1));
    BOOST_CHECK(!owner.find(turn.parent.conversationId));
    first = owner.commitTurn(turn, prepared).checkpoint.wire;
    BOOST_CHECK_EQUAL(first, fixture.oracle.at("cases")[2].at("checkpointWire").get<std::string>());
    BOOST_CHECK_THROW(owner.commitTurn(turn, prepared), std::runtime_error);
    fixture.now += 1;
    auto badParent = fixture.continuation(2, first); badParent.parentCheckpointDigest = digest("wrong-parent");
    BOOST_CHECK_THROW(owner.beginTurn(badParent, "/request/2"), std::runtime_error);
    const auto next = owner.beginTurn(fixture.continuation(2, first), "/request/2");
    owner.acceptTokenPrefix(next, {13});
    auto incomplete = fixture.completed(2); incomplete.authenticatedReceipts.pop_back();
    BOOST_CHECK_THROW(owner.prepareCheckpoint(next, incomplete), std::runtime_error);
    second = owner.commitTurn(next, owner.prepareCheckpoint(next, fixture.completed(2))).checkpoint.wire;
    BOOST_CHECK_EQUAL(second, fixture.oracle.at("cases")[3].at("checkpointWire").get<std::string>());
  }
  NativeConversationCoordinator recovered(fixture.config);
  const auto record = recovered.find("0123456789abcdef0123456789abcdef");
  BOOST_REQUIRE(record);
  BOOST_CHECK_EQUAL(record->checkpoint.wire, second);
  BOOST_CHECK_EQUAL(fixture.promotions, 2);
  BOOST_CHECK_EQUAL(fixture.rollbacks, 0);
}

BOOST_AUTO_TEST_CASE(InitialFullContextBindsDynamicPlanAfterPlacement)
{
  ConversationOwnerFixture fixture;
  NativeConversationCoordinator owner(fixture.config);
  auto continuation = fixture.continuation(1);
  continuation.planRoleMapDigest.clear();
  continuation.expectedRoles.clear();
  const auto turn = owner.beginTurn(continuation, "/request/dynamic");
  const auto rebound = owner.bindInitialPlanRoleMap(
    turn, {{"/role/A", "/provider/A"}, {"/role/B", "/provider/B"}});
  BOOST_CHECK(!rebound.parent.planRoleMapDigest.empty());
  BOOST_CHECK_EQUAL(rebound.parent.expectedRoles.size(), 2);
  BOOST_CHECK_EQUAL(rebound.parent.expectedRoles.at(0), "/role/A");
  BOOST_CHECK_EQUAL(rebound.parent.expectedRoles.at(1), "/role/B");
  BOOST_CHECK_THROW(owner.bindInitialPlanRoleMap(
    rebound, {{"/role/A", "/provider/A"}, {"/role/B", "/provider/B"}}), std::runtime_error);
  NativeDiError cancelled("CANCELLED", "conversation", "test", "cancel");
  owner.abortTurn(rebound, cancelled);
}

BOOST_AUTO_TEST_CASE(AbortFencesCopiesAndCancellationDuringPromotion)
{
  ConversationOwnerFixture fixture;
  NativeConversationCoordinator owner(fixture.config);
  const auto continuation = fixture.continuation(1);
  auto turn = owner.beginTurn(continuation, "/request/1");
  const auto stale = turn;
  NativeDiError cancelled("CANCELLED", "conversation", "test", "cancel");
  owner.abortTurn(turn, cancelled); owner.abortTurn(turn, cancelled);
  BOOST_CHECK_THROW(owner.acceptTokenPrefix(stale, {11}), std::runtime_error);
  BOOST_CHECK_THROW(owner.prepareCheckpoint(stale, fixture.completed(1)), std::runtime_error);
  turn = owner.beginTurn(continuation, "/request/1");
  owner.acceptTokenPrefix(turn, {11});
  BOOST_CHECK_THROW(owner.acceptTokenPrefix(turn, {12}), std::runtime_error);
  const auto prepared = owner.prepareCheckpoint(turn, fixture.completed(1));
  fixture.duringPromotion = [&] { owner.abortTurn(turn, cancelled); };
  BOOST_CHECK_THROW(owner.commitTurn(turn, prepared), std::runtime_error);
  BOOST_CHECK(!owner.find(continuation.conversationId));
  BOOST_CHECK(fixture.config.journal->readConversations(fixture.now).empty());
  BOOST_CHECK_EQUAL(fixture.promotions, 1);
  BOOST_CHECK_EQUAL(fixture.rollbacks, 1);
}

BOOST_AUTO_TEST_CASE(ReplacementFencesOldAttemptAndPreservesAcceptedPrefix)
{
  ConversationOwnerFixture fixture;
  NativeConversationCoordinator owner(fixture.config);
  const auto turn = owner.beginTurn(fixture.continuation(1), "/request/1");
  owner.acceptTokenPrefix(turn, {11});
  const auto replacement = owner.replaceAttempt(turn, "/request/replacement");
  BOOST_CHECK_EQUAL(replacement.attempt, 2);
  BOOST_CHECK(replacement.acceptedTokenIds == std::vector<std::int64_t>{11});
  BOOST_CHECK_THROW(owner.bindAttemptPlanRoleMap(
    replacement, {{"/role/A", "/provider/replacement"}}), std::runtime_error);
  const auto rebound = owner.bindAttemptPlanRoleMap(
    replacement, {{"/role/A", "/provider/replacement"}, {"/role/B", "/provider/B"}});
  BOOST_CHECK_NE(rebound.parent.planRoleMapDigest, turn.parent.planRoleMapDigest);
  BOOST_CHECK_THROW(owner.bindAttemptPlanRoleMap(
    rebound, {{"/role/A", "/provider/another"}, {"/role/B", "/provider/B"}}),
    std::runtime_error);
  BOOST_CHECK_THROW(owner.prepareCheckpoint(turn, fixture.completed(1)), std::runtime_error);
  BOOST_CHECK_THROW(owner.prepareCheckpoint(replacement, fixture.completed(1)), std::runtime_error);
  BOOST_CHECK_THROW(owner.replaceAttempt(rebound, "/request/third"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DurableGateRejectsCancellationAndNeverRollsBackPublishedParent)
{
  for (bool allowPublish : {false, true}) {
    ConversationOwnerFixture fixture;
    NativeConversationCoordinator owner(fixture.config);
    const auto turn = owner.beginTurn(fixture.continuation(1), "/request/1");
    owner.acceptTokenPrefix(turn, {11});
    auto completed = fixture.completed(1);
    std::size_t finalizations = 0;
    completed.durableCommitGate = [allowPublish](const std::function<void()>& publish) {
      if (!allowPublish) throw std::runtime_error("operation cancelled before publication");
      publish();
      throw std::runtime_error("notification failed after publication");
    };
    completed.finalizeProviderState = [&] {
      ++finalizations;
      throw std::runtime_error("lost finalize transport");
    };
    const auto checkpoint = owner.prepareCheckpoint(turn, completed);
    if (!allowPublish) {
      BOOST_CHECK_THROW(owner.commitTurn(turn, checkpoint), std::runtime_error);
      BOOST_CHECK(!owner.find(turn.parent.conversationId));
      BOOST_CHECK(fixture.config.journal->readConversations(fixture.now).empty());
      BOOST_CHECK_EQUAL(fixture.rollbacks, 1);
      BOOST_CHECK_EQUAL(finalizations, 0);
    }
    else {
      BOOST_CHECK_EQUAL(owner.commitTurn(turn, checkpoint).checkpoint.wire, checkpoint.wire);
      BOOST_REQUIRE(owner.find(turn.parent.conversationId));
      BOOST_CHECK_EQUAL(fixture.config.journal->readConversations(fixture.now).size(), 1);
      BOOST_CHECK_EQUAL(fixture.rollbacks, 0);
      BOOST_CHECK_EQUAL(finalizations, 1);
    }
  }
}
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di

#ifndef SPEC189_CONVERSATION_ONLY
// The standalone conversation target does not link the unrelated sampling
// fixture owned by the aggregate asynchronous-runtime test suite.
namespace {
using namespace ndnsf::di;
std::vector<std::int64_t> sampled(std::vector<std::vector<float>> logits,
  const std::function<void(NativeEpochCoordinatorConfig&)>& configure = [](auto&) {})
{
  const auto result = test::runSamplingEpochs(std::move(logits), configure);
  if (!result.finalPayload) throw std::runtime_error("sampling epoch did not finish");
  return nativeParseJson(std::string(result.finalPayload->begin(), result.finalPayload->end()))
    .at("tokenIds").get<std::vector<std::int64_t>>();
}
}

BOOST_AUTO_TEST_SUITE(Spec182Sampling)
BOOST_AUTO_TEST_CASE(Spec182SamplingTopPRetainedMass)
{
  const auto tokens = sampled({{float(std::log(.6)), float(std::log(.3)), float(std::log(.1))}}, [](auto& c) {
    c.samplingMode = "SeededTopKTopP"; c.samplingTemperature = 1;
    c.samplingTopK = 3; c.samplingTopP = .8; c.samplingSeed = 8;
  });
  BOOST_CHECK(tokens == std::vector<std::int64_t>{0});
}
BOOST_AUTO_TEST_CASE(Spec182SamplingPenaltyOncePerToken)
{
  BOOST_CHECK(sampled({{100, -100}, {100, -100}, {4, 1.5}}, [](auto& c) {
    c.samplingRepetitionPenalty = 2;
  }) == std::vector<std::int64_t>({0, 0, 0}));
  BOOST_CHECK(sampled({{100, -100}, {100, -100}, {-1, -1.5}}, [](auto& c) {
    c.samplingRepetitionPenalty = 2;
  }) == std::vector<std::int64_t>({0, 0, 1}));
}
BOOST_AUTO_TEST_CASE(Spec182SamplingValidationParity)
{
  for (const auto& mode : {std::string("Greedy"), std::string("SeededTopKTopP")}) {
    for (int bad = 0; bad != 13; ++bad) {
      BOOST_TEST_CONTEXT(mode << " invalid parameter " << bad) {
        BOOST_CHECK_THROW(sampled({{1, 0}}, [&](auto& c) {
          c.samplingMode = mode; c.samplingTemperature = mode == "Greedy" ? 0 : 1;
          switch (bad) {
            case 0: c.samplingTopK = 0; break;
            case 1: c.samplingTopK = 3; break;
            case 2: c.samplingTopP = 0; break;
            case 3: c.samplingTopP = std::numeric_limits<double>::quiet_NaN(); break;
            case 4: c.samplingTopP = 1.1; break;
            case 5: c.samplingRepetitionPenalty = .09; break;
            case 6: c.samplingRepetitionPenalty = 2.1; break;
            case 7: c.samplingRepetitionPenalty = std::numeric_limits<double>::infinity(); break;
            case 8: c.samplingTemperature = std::numeric_limits<double>::quiet_NaN(); break;
            case 9: c.samplingMode = "unknown"; break;
            case 10: c.samplingTemperature = mode == "Greedy" ? 1 : 0; break;
            case 11: c.samplingTemperature = 5.1; break;
            case 12: c.samplingRepetitionPenalty = std::numeric_limits<double>::quiet_NaN(); break;
          }
        }), std::invalid_argument);
      }
    }
  }
  for (const auto value : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()})
    BOOST_CHECK_THROW(sampled({{value, 0}}), std::invalid_argument);
  BOOST_CHECK_THROW(sampled({{}}), std::exception);
  for (double penalty : {.1, 2.0})
    BOOST_CHECK(sampled({{1, 0}}, [&](auto& c) {
      c.samplingRepetitionPenalty = penalty; c.samplingTopK = 2;
    }) == std::vector<std::int64_t>{0});
}
BOOST_AUTO_TEST_CASE(Spec182SamplingDoublePrecisionAndTies)
{
  // float32(1.1)/double(1.1) is greater than 1; float division rounds to a tie.
  BOOST_CHECK(sampled({{0, 100}, {0, 100}, {1, 1.1F}}, [](auto& c) {
    c.samplingRepetitionPenalty = 1.1;
  }) == std::vector<std::int64_t>({1, 1, 1}));
  BOOST_CHECK(sampled({{1, 1}}) == std::vector<std::int64_t>{0});
  BOOST_CHECK(sampled({{1, 1}}, [](auto& c) {
    c.samplingMode = "SeededTopKTopP"; c.samplingTemperature = 1;
    c.samplingTopK = 1; c.samplingTopP = 1;
  }) == std::vector<std::int64_t>{0});
  for (const auto seed : {std::uint64_t(0), std::numeric_limits<std::uint64_t>::max()})
    BOOST_CHECK(sampled({{100, -100}, {100, -100}, {1, 1}}, [&](auto& c) {
      c.samplingMode = "SeededTopKTopP"; c.samplingTemperature = 1;
      c.samplingTopK = 2; c.samplingSeed = seed;
    }) == std::vector<std::int64_t>({0, 0, 1}));
}
BOOST_AUTO_TEST_SUITE_END()
#endif // SPEC189_CONVERSATION_ONLY
