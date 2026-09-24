#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationJournal.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
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

struct ConversationFixture
{
  ConversationFixture()
  {
    std::ifstream input("tests/fixtures/spec182/conversation-oracle.json");
    oracle = NativeJson::parse(input);
    char directory[] = "/tmp/spec190-terminal-drain-XXXXXX";
    if (!::mkdtemp(directory)) throw std::runtime_error("terminal fixture temp root failed");
    root = directory;
    std::vector<std::uint8_t> key(32);
    for (std::size_t i = 0; i != key.size(); ++i) key[i] = static_cast<std::uint8_t>(i);
    config.journal = std::make_shared<NativeConversationJournal>(NativeConversationJournalConfig{
      root, "fixture-owner", {{"fixture-key", key}}, 64 * 1024 * 1024, true});
    config.authenticationKeys = {key};
    config.serviceName = "/service/会话";
    config.requesterIdentity = "/requester/A";
    config.securityDomainDigest = digest("security");
    config.nowMs = [&] { return now; };
  }

  ~ConversationFixture()
  {
    config.journal.reset();
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  NativeConversationContinuation continuation() const
  {
    NativeConversationContinuation value;
    value.conversationId = "0123456789abcdef0123456789abcdef";
    value.parentContextEpoch = 0;
    value.serviceName = config.serviceName;
    value.planRoleMapDigest = digest("roles");
    value.requestContractDigest = digest("request-contract");
    value.retentionDeadlineMs = now + 900'000;
    value.generationId = std::string(32, '1');
    value.expectedRoles = {"/role/A", "/role/B"};
    value.canonicalTokenIds = {10};
    return value;
  }

  NativeCompletedAttempt completed(std::vector<std::string>& order,
                                  bool failFinalize = false,
                                  bool failCommit = false,
                                  bool oneSidedCommitAckLoss = false)
  {
    const auto& transcript = oracle.at("cases")[2].at("transcript");
    NativeCompletedAttempt result;
    result.requestId = "/request/1";
    result.attempt = 1;
    result.complete = true;
    result.tokenIds = transcript.at("canonicalTokenIds").get<std::vector<std::int64_t>>();
    result.generationId = std::string(32, '1');
    result.modelContractDigest = digest("model");
    result.tokenizerDigest = digest("tokenizer");
    result.chatTemplateDigest = digest("template");
    result.applicationMessages = nativeConversationBase64Decode(
      transcript.at("applicationMessages").get<std::string>());
    for (const auto& encoded : transcript.at("providerRoleReceipts")) {
      result.authenticatedReceipts.push_back(nativeParseJson(
        nativeConversationBase64Decode(encoded.get<std::string>())));
    }
    result.commitProviderState = [&order, failCommit, oneSidedCommitAckLoss](const std::string&) {
      order.push_back("COMMIT");
      if (oneSidedCommitAckLoss) {
        order.push_back("COMMIT/provider-a");
        order.push_back("COMMIT_ACK_LOST/provider-b");
        throw std::runtime_error("provider-b commit acknowledgement lost");
      }
      if (failCommit) throw std::runtime_error("commit control unavailable");
    };
    result.rollbackProviderState = [&order, oneSidedCommitAckLoss] {
      order.push_back(oneSidedCommitAckLoss ? "ROLLBACK/provider-a" : "ROLLBACK");
    };
    result.durableCommitGate = [&order](const std::function<void()>& publish) {
      order.push_back("JOURNAL");
      publish();
    };
    result.finalizeProviderState = [&order, failFinalize] {
      order.push_back("FINALIZE");
      if (failFinalize) throw std::runtime_error("finalize control unavailable");
    };
    return result;
  }

  NativeJson oracle;
  std::filesystem::path root;
  NativeConversationConfig config;
  std::uint64_t now = 2'000'000'000'001ULL;
};

class PublishTestUser : public ndn_service_framework::test::LocalServiceUser
{
public:
  using LocalServiceUser::LocalServiceUser;

  void seedScope(const ndn::Name& requestId, const std::string& scope,
                 const ndn::Buffer& key)
  {
    std::lock_guard<std::mutex> lock(m_verifiedCollaborationMutex);
    m_userCollaborationScopeKeys[requestId][scope] = key;
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec190TerminalDrain)

BOOST_AUTO_TEST_CASE(HealthyFinalizeFollowsDurableJournalAndClosesEarly)
{
  for (unsigned repeat = 0; repeat != 20; ++repeat) {
    ConversationFixture fixture;
    NativeConversationCoordinator coordinator(fixture.config);
    const auto turn = coordinator.beginTurn(fixture.continuation(), "/request/1");
    coordinator.acceptTokenPrefix(turn, {11});
    std::vector<std::string> order;
    const auto checkpoint = coordinator.prepareCheckpoint(
      turn, fixture.completed(order));
    const auto record = coordinator.commitTurn(turn, checkpoint);
    BOOST_CHECK_EQUAL(record.checkpoint.wire, checkpoint.wire);
    BOOST_REQUIRE_EQUAL(order.size(), 3U);
    BOOST_CHECK_EQUAL(order[0], "COMMIT");
    BOOST_CHECK_EQUAL(order[1], "JOURNAL");
    BOOST_CHECK_EQUAL(order[2], "FINALIZE");
    BOOST_REQUIRE(coordinator.find(turn.parent.conversationId));
    BOOST_CHECK_EQUAL(fixture.config.journal->readConversations(fixture.now).size(), 1U);
  }
}

BOOST_AUTO_TEST_CASE(LostFinalizeRetainsDurableCheckpointWithoutRollback)
{
  ConversationFixture fixture;
  NativeConversationCoordinator coordinator(fixture.config);
  const auto turn = coordinator.beginTurn(fixture.continuation(), "/request/1");
  coordinator.acceptTokenPrefix(turn, {11});
  std::vector<std::string> order;
  const auto checkpoint = coordinator.prepareCheckpoint(
    turn, fixture.completed(order, true));
  BOOST_CHECK_NO_THROW(coordinator.commitTurn(turn, checkpoint));
  BOOST_REQUIRE(coordinator.find(turn.parent.conversationId));
  BOOST_CHECK_EQUAL(fixture.config.journal->readConversations(fixture.now).size(), 1U);
  BOOST_CHECK(std::find(order.begin(), order.end(), "ROLLBACK") == order.end());
}

BOOST_AUTO_TEST_CASE(UncommittedPromotionFailureRollsBackBeforeDrain)
{
  ConversationFixture fixture;
  NativeConversationCoordinator coordinator(fixture.config);
  const auto turn = coordinator.beginTurn(fixture.continuation(), "/request/1");
  coordinator.acceptTokenPrefix(turn, {11});
  std::vector<std::string> order;
  const auto checkpoint = coordinator.prepareCheckpoint(
    turn, fixture.completed(order, false, true));
  BOOST_CHECK_THROW(coordinator.commitTurn(turn, checkpoint), std::runtime_error);
  BOOST_CHECK(!coordinator.find(turn.parent.conversationId));
  BOOST_CHECK_EQUAL(fixture.config.journal->readConversations(fixture.now).size(), 0U);
  BOOST_REQUIRE_EQUAL(order.size(), 2U);
  BOOST_CHECK_EQUAL(order[0], "COMMIT");
  BOOST_CHECK_EQUAL(order[1], "ROLLBACK");
}

BOOST_AUTO_TEST_CASE(OneSidedCommitAckLossCompensatesCommittedProvider)
{
  ConversationFixture fixture;
  NativeConversationCoordinator coordinator(fixture.config);
  const auto turn = coordinator.beginTurn(fixture.continuation(), "/request/1");
  coordinator.acceptTokenPrefix(turn, {11});
  std::vector<std::string> order;
  const auto checkpoint = coordinator.prepareCheckpoint(
    turn, fixture.completed(order, false, false, true));
  BOOST_CHECK_THROW(coordinator.commitTurn(turn, checkpoint), std::runtime_error);
  BOOST_CHECK(!coordinator.find(turn.parent.conversationId));
  BOOST_CHECK_EQUAL(fixture.config.journal->readConversations(fixture.now).size(), 0U);
  BOOST_REQUIRE_EQUAL(order.size(), 4U);
  BOOST_CHECK_EQUAL(order[0], "COMMIT");
  BOOST_CHECK_EQUAL(order[1], "COMMIT/provider-a");
  BOOST_CHECK_EQUAL(order[2], "COMMIT_ACK_LOST/provider-b");
  BOOST_CHECK_EQUAL(order[3], "ROLLBACK/provider-a");
}

BOOST_AUTO_TEST_CASE(FacePublicationIsCountedUntilItsIoCallbackRuns)
{
  ndn::security::KeyChain keyChain("pib-memory:", "tpm-memory:");
  ndn::DummyClientFace face(keyChain);
  const auto userCertificate = ndn_service_framework::test::makeRsaIdentity(
    keyChain, ndn::Name("/spec190/user"));
  const auto authorityCertificate = ndn_service_framework::test::makeRsaIdentity(
    keyChain, ndn::Name("/spec190/authority"));
  PublishTestUser user(face, ndn::Name("/spec190/group"), userCertificate,
                       authorityCertificate, "examples/trust-any.conf");
  ndn::svs::SecurityOptions security(keyChain);
  security.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
  security.dataSigner->signingInfo = ndn::security::signingWithSha256();
  security.pubSigner->signingInfo = ndn::security::signingWithSha256();
  security.validator = std::make_shared<ndn::svs::BaseValidator>();
  security.encapsulatedDataValidator = std::make_shared<ndn::svs::BaseValidator>();
  ndn::svs::SVSPubSubOptions svsOptions;
  svsOptions.useTimestamp = false;
  user.attachLocalMockPubSubForTest(std::make_shared<ndn::svs::SVSPubSub>(
    ndn::Name("/spec190/control/sync"), ndn::Name("/spec190/user/0"), face,
    [] (const std::vector<ndn::svs::MissingDataInfo>&) {}, svsOptions, security));
  const ndn::Name requestId("/NDNSF/DI/REQUEST/spec190");
  const std::string scope = "ndnsf-di-conversation-state-v1";
  user.seedScope(requestId, scope, ndn::Buffer(32, 7));
  BOOST_REQUIRE(user.publishCollaborationData(
    ndn::Name("/spec190/provider"), requestId, scope,
    ndn::Name("/ndnsf-di/conversation/control"),
    ndn::Buffer("finalize", sizeof("finalize") - 1)));
  BOOST_CHECK(!user.waitForCollaborationPublishIdle(std::chrono::milliseconds(0)));
  face.getIoContext().poll();
  BOOST_CHECK(user.waitForCollaborationPublishIdle(std::chrono::milliseconds(100)));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndnsf::di
