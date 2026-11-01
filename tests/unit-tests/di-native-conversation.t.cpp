#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "tests/fixtures/spec182/native-sampling-epoch.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationJournal.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedKvPublication.hpp"
#include <fstream>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <atomic>
#include <thread>

#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>

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
BOOST_AUTO_TEST_CASE(Spec204OwnerBoundaryGateWaitsForEverySourceProvider)
{
  using Gate = NativeConversationHandoffBoundaryGateV1;
  Gate gate({"/provider/source-a", "/provider/source-b"},
    std::chrono::steady_clock::now() + std::chrono::seconds(5));

  BOOST_CHECK(gate.reportBoundaryReady("/provider/source-a"));
  BOOST_CHECK(!gate.releaseCommit());
  BOOST_CHECK_THROW(gate.reportBoundaryReady("/provider/unknown"), std::invalid_argument);
  BOOST_CHECK(gate.reportBoundaryReady("/provider/source-a")); // retry is idempotent
  BOOST_CHECK(gate.reportBoundaryReady("/provider/source-b"));
  BOOST_CHECK(gate.releaseCommit());
  BOOST_CHECK(gate.releaseCommit());
  BOOST_CHECK(!gate.cancel());
  BOOST_CHECK(gate.state() == Gate::State::CommitReleased);

  Gate cancelled({"/provider/source-a", "/provider/source-b"},
    std::chrono::steady_clock::now() + std::chrono::seconds(5));
  BOOST_CHECK(cancelled.reportBoundaryReady("/provider/source-a"));
  BOOST_CHECK(cancelled.cancel());
  BOOST_CHECK(cancelled.cancel());
  BOOST_CHECK(!cancelled.reportBoundaryReady("/provider/source-b"));
  BOOST_CHECK(!cancelled.releaseCommit());
  BOOST_CHECK(cancelled.state() == Gate::State::Cancelled);

  Gate readyThenCancelled({"/provider/source-a", "/provider/source-b"},
    std::chrono::steady_clock::now() + std::chrono::seconds(5));
  BOOST_CHECK(readyThenCancelled.reportBoundaryReady("/provider/source-a"));
  BOOST_CHECK(readyThenCancelled.reportBoundaryReady("/provider/source-b"));
  BOOST_CHECK(readyThenCancelled.cancel());
  BOOST_CHECK(!readyThenCancelled.releaseCommit());
  BOOST_CHECK(readyThenCancelled.state() == Gate::State::Cancelled);

  const auto futureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  BOOST_CHECK_THROW(Gate({}, futureDeadline), std::invalid_argument);
  BOOST_CHECK_THROW(
    Gate({"/provider/source-a", "/provider/source-a"}, futureDeadline),
    std::invalid_argument);
  BOOST_CHECK_THROW(
    Gate(std::vector<std::string>(65, "/provider/source"), futureDeadline),
    std::invalid_argument);
  BOOST_CHECK_THROW(Gate({"/provider/source-a"}, std::chrono::steady_clock::now()), std::invalid_argument);

  const auto expiredDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  Gate expired({"/provider/source-a", "/provider/source-b"}, expiredDeadline);
  BOOST_REQUIRE(expired.reportBoundaryReady("/provider/source-a"));
  BOOST_REQUIRE(expired.reportBoundaryReady("/provider/source-b"));
  std::this_thread::sleep_until(expiredDeadline);
  BOOST_CHECK(!expired.releaseCommit());
  BOOST_CHECK(expired.state() == Gate::State::Cancelled);
}

BOOST_AUTO_TEST_CASE(Spec204OwnerBoundaryGateLinearizesCancelAgainstCommit)
{
  using Gate = NativeConversationHandoffBoundaryGateV1;
  for (int attempt = 0; attempt < 100; ++attempt) {
    Gate gate({"/provider/source-a", "/provider/source-b"},
      std::chrono::steady_clock::now() + std::chrono::seconds(5));
    BOOST_REQUIRE(gate.reportBoundaryReady("/provider/source-a"));
    BOOST_REQUIRE(gate.reportBoundaryReady("/provider/source-b"));

    std::atomic<bool> start{false};
    bool cancelAccepted = false;
    bool commitReleased = false;
    std::thread cancelThread([&] {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      cancelAccepted = gate.cancel();
    });
    std::thread commitThread([&] {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      commitReleased = gate.releaseCommit();
    });
    start.store(true, std::memory_order_release);
    cancelThread.join();
    commitThread.join();

    BOOST_CHECK(cancelAccepted != commitReleased);
    if (cancelAccepted)
      BOOST_CHECK(gate.state() == Gate::State::Cancelled);
    else
      BOOST_CHECK(gate.state() == Gate::State::CommitReleased);
  }
}

BOOST_AUTO_TEST_CASE(Spec204ProviderRoleBoundaryGateWaitsForEveryRole)
{
  using Gate = NativeConversationHandoffRoleBoundaryGateV1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  Gate gate({"/Stage/0", "/Stage/1"}, deadline);
  std::atomic<bool> firstCommitted{false};
  std::atomic<bool> secondCommitted{false};
  std::thread first([&] {
    firstCommitted.store(gate.parkRoleAndWait("/Stage/0"));
  });
  const auto waitForReadyCount = [](const Gate& candidate, std::size_t count) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (candidate.readyRoleCount() != count &&
           std::chrono::steady_clock::now() < limit)
      std::this_thread::yield();
    return candidate.readyRoleCount() == count;
  };

  const bool firstParked = waitForReadyCount(gate, 1);
  const bool readyTooEarly = gate.boundaryReady();
  const bool earlyCommit = gate.releaseCommit();
  std::thread second([&] {
    secondCommitted.store(gate.parkRoleAndWait("/Stage/1"));
  });
  const bool allParked = waitForReadyCount(gate, 2);
  const bool boundaryReady = gate.boundaryReady();
  const bool cancelled = gate.cancel();
  first.join();
  second.join();

  BOOST_CHECK(firstParked);
  BOOST_CHECK(!readyTooEarly);
  BOOST_CHECK(!earlyCommit);
  BOOST_CHECK(allParked);
  BOOST_CHECK(boundaryReady);
  BOOST_CHECK(cancelled);
  BOOST_CHECK(!firstCommitted.load());
  BOOST_CHECK(!secondCommitted.load());
  BOOST_CHECK(gate.state() == Gate::State::Cancelled);
  BOOST_CHECK(!gate.releaseCommit());

  std::atomic<bool> firstReleased{false};
  std::atomic<bool> secondReleased{false};
  const auto releaseDeadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(5);
  Gate released({"/Stage/0", "/Stage/1"}, releaseDeadline);
  std::thread releaseFirst([&] {
    firstReleased.store(released.parkRoleAndWait("/Stage/0"));
  });
  std::thread releaseSecond([&] {
    secondReleased.store(released.parkRoleAndWait("/Stage/1"));
  });
  const bool releaseBarrierReady = waitForReadyCount(released, 2);
  const bool releasedCommit = released.releaseCommit();
  if (!releasedCommit)
    released.cancel();
  releaseFirst.join();
  releaseSecond.join();
  BOOST_CHECK(releaseBarrierReady);
  BOOST_CHECK(releasedCommit);
  BOOST_CHECK(firstReleased.load());
  BOOST_CHECK(secondReleased.load());
  BOOST_CHECK(!released.cancel());
}

BOOST_AUTO_TEST_CASE(Spec204ProviderRoleBoundaryGateRejectsInvalidRoleAndExpires)
{
  using Gate = NativeConversationHandoffRoleBoundaryGateV1;
  const auto futureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  BOOST_CHECK_THROW(Gate({}, futureDeadline), std::invalid_argument);
  BOOST_CHECK_THROW(
    Gate({"/Stage/0", "/Stage/0"}, futureDeadline), std::invalid_argument);
  BOOST_CHECK_THROW(
    Gate(std::vector<std::string>(65, "/Stage/0"), futureDeadline),
    std::invalid_argument);
  BOOST_CHECK_THROW(Gate({"/Stage/0"}, std::chrono::steady_clock::now()), std::invalid_argument);

  const auto expiredDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
  Gate gate({"/Stage/0", "/Stage/1"}, expiredDeadline);
  BOOST_CHECK_THROW(gate.parkRoleAndWait("/Stage/unknown"), std::invalid_argument);
  BOOST_CHECK(!gate.parkRoleAndWait("/Stage/0"));
  BOOST_CHECK(gate.state() == Gate::State::Cancelled);
  BOOST_CHECK(!gate.boundaryReady());
  BOOST_CHECK(!gate.parkRoleAndWait("/Stage/1"));
}

BOOST_AUTO_TEST_CASE(Spec204DurableStorageGrantWireHasCanonicalBoundedBindings)
{
  const auto tagged = [](char c) { return "sha256:" + std::string(64, c); };
  NativeDurableStorageGrantChallengeV1 challenge;
  challenge.operationId = "event-01J7KVCACHE";
  challenge.checkpointDigest = tagged('0');
  challenge.snapshotDigest = tagged('1');
  challenge.roleMapDigest = tagged('2');
  NativeDurableStorageGrantRoleChallengeV1 role;
  role.roleName = "/Stage/0";
  role.repoTarget = {"/repo/node-a", "/repo/provider-a", "/repo/service"};
  role.producerPrefix = "/provider/source-a";
  role.writerIdentity = role.producerPrefix;
  role.manifestName = "/provider/source-a/kv-cache/v1/root";
  role.publicationIdentity = tagged('3');
  role.protectionEpoch = "owner-protection-epoch-7";
  role.dataPrefix = "/provider/source-a/kv-cache/v1/data";
  role.packetWireBytes = 1234;
  role.auxiliaryPacketNames = {"/provider/source-a/kv-cache/v1/key-packet"};
  challenge.roles.push_back(role);

  const auto encodedChallenge = nativeDurableStorageGrantChallengeV1ToJson(challenge);
  const std::string expectedChallenge = R"JSON({"checkpointDigest":"sha256:0000000000000000000000000000000000000000000000000000000000000000","operationId":"event-01J7KVCACHE","roleMapDigest":"sha256:2222222222222222222222222222222222222222222222222222222222222222","roles":[{"auxiliaryPacketNames":["/provider/source-a/kv-cache/v1/key-packet"],"dataPrefix":"/provider/source-a/kv-cache/v1/data","manifestName":"/provider/source-a/kv-cache/v1/root","packetWireBytes":1234,"producerPrefix":"/provider/source-a","protectionEpoch":"owner-protection-epoch-7","publicationIdentity":"sha256:3333333333333333333333333333333333333333333333333333333333333333","repoTarget":{"providerIdentity":"/repo/provider-a","repoNode":"/repo/node-a","servicePrefix":"/repo/service"},"roleName":"/Stage/0","writerIdentity":"/provider/source-a"}],"schema":"ndnsf-di-kv-storage-grant-challenge-v1","snapshotDigest":"sha256:1111111111111111111111111111111111111111111111111111111111111111"})JSON";
  const auto challengeWire = nativeConversationCanonicalJson(encodedChallenge);
  BOOST_CHECK_EQUAL(challengeWire, expectedChallenge);
  const auto decodedChallenge = nativeDurableStorageGrantChallengeV1FromJson(challengeWire);
  BOOST_REQUIRE_EQUAL(decodedChallenge.roles.size(), 1U);
  BOOST_CHECK_EQUAL(decodedChallenge.roles.front().manifestName, role.manifestName);
  BOOST_CHECK_EQUAL(decodedChallenge.roles.front().repoTarget.providerIdentity,
                    role.repoTarget.providerIdentity);
  BOOST_CHECK_EQUAL(decodedChallenge.roles.front().auxiliaryPacketNames.front(),
                    role.auxiliaryPacketNames.front());

  auto typedChallenge = challenge;
  ndn::Name typedProducer("/provider/source-a");
  typedProducer.appendVersion(7);
  auto typedManifest = typedProducer;
  typedManifest.append("kv-cache").append("v1").append("root");
  auto typedDataPrefix = typedProducer;
  typedDataPrefix.append("kv-cache").append("v1").append("data");
  auto typedAuxiliaryName = typedProducer;
  typedAuxiliaryName.append("kv-cache").append("v1").append("key-packet");
  ndn::Name typedRepoNode("/repo/node-a");
  typedRepoNode.appendVersion(3);
  typedChallenge.roles[0].repoTarget.repoNode =
    typedRepoNode.toUri(ndn::name::UriFormat::ALTERNATE);
  typedChallenge.roles[0].producerPrefix =
    typedProducer.toUri(ndn::name::UriFormat::ALTERNATE);
  typedChallenge.roles[0].writerIdentity = typedChallenge.roles[0].producerPrefix;
  typedChallenge.roles[0].manifestName =
    typedManifest.toUri(ndn::name::UriFormat::ALTERNATE);
  typedChallenge.roles[0].dataPrefix =
    typedDataPrefix.toUri(ndn::name::UriFormat::ALTERNATE);
  typedChallenge.roles[0].auxiliaryPacketNames = {
    typedAuxiliaryName.toUri(ndn::name::UriFormat::ALTERNATE)};
  const auto typedValue = nativeDurableStorageGrantChallengeV1ToJson(typedChallenge);
  const auto typedWire = nativeConversationCanonicalJson(typedValue);
  const auto parsedTyped =
    nativeDurableStorageGrantChallengeV1FromJson(typedWire);
  BOOST_CHECK_EQUAL(parsedTyped.roles[0].producerPrefix,
    typedProducer.toUri(ndn::name::UriFormat::ALTERNATE));
  BOOST_CHECK_NE(typedProducer.toUri(ndn::name::UriFormat::ALTERNATE),
                 typedProducer.toUri(ndn::name::UriFormat::CANONICAL));
  auto nonAlternateNames = typedValue;
  nonAlternateNames["roles"][0]["producerPrefix"] =
    typedProducer.toUri(ndn::name::UriFormat::CANONICAL);
  nonAlternateNames["roles"][0]["writerIdentity"] =
    typedProducer.toUri(ndn::name::UriFormat::CANONICAL);
  nonAlternateNames["roles"][0]["manifestName"] =
    typedManifest.toUri(ndn::name::UriFormat::CANONICAL);
  nonAlternateNames["roles"][0]["dataPrefix"] =
    typedDataPrefix.toUri(ndn::name::UriFormat::CANONICAL);
  nonAlternateNames["roles"][0]["auxiliaryPacketNames"][0] =
    typedAuxiliaryName.toUri(ndn::name::UriFormat::CANONICAL);
  nonAlternateNames["roles"][0]["repoTarget"]["repoNode"] =
    typedRepoNode.toUri(ndn::name::UriFormat::CANONICAL);
  BOOST_CHECK_THROW(nativeDurableStorageGrantChallengeV1FromJson(
    nativeConversationCanonicalJson(nonAlternateNames)), std::invalid_argument);

  auto wrongChallenge = encodedChallenge;
  wrongChallenge["roles"][0]["manifestName"] = "/provider/other/kv-cache/v1/root";
  BOOST_CHECK_THROW(nativeDurableStorageGrantChallengeV1FromJson(
    nativeConversationCanonicalJson(wrongChallenge)), std::invalid_argument);
  wrongChallenge = encodedChallenge;
  wrongChallenge["roles"][0]["publicationIdentity"] = "sha256:bad";
  BOOST_CHECK_THROW(nativeDurableStorageGrantChallengeV1FromJson(
    nativeConversationCanonicalJson(wrongChallenge)), std::invalid_argument);
  wrongChallenge = encodedChallenge;
  wrongChallenge["unexpected"] = true;
  BOOST_CHECK_THROW(nativeDurableStorageGrantChallengeV1FromJson(
    nativeConversationCanonicalJson(wrongChallenge)), std::invalid_argument);
  auto duplicateChallenge = challengeWire;
  duplicateChallenge.insert(1, "\"schema\":\"duplicate\",");
  BOOST_CHECK_THROW(nativeDurableStorageGrantChallengeV1FromJson(duplicateChallenge),
                    std::invalid_argument);
  BOOST_CHECK_THROW(nativeDurableStorageGrantChallengeV1FromJson(challengeWire + "\n"),
                    std::invalid_argument);
  BOOST_CHECK_THROW(nativeDurableStorageGrantChallengeV1FromJson(
    std::string(64 * 1024 + 1, ' ')), std::invalid_argument);
  NativeJson deeplyNestedValue = 0;
  for (std::size_t depth = 0; depth != 33; ++depth)
    deeplyNestedValue = NativeJson::array({std::move(deeplyNestedValue)});
  auto deeplyNestedChallenge = encodedChallenge;
  deeplyNestedChallenge["untrusted"] = deeplyNestedValue;
  const auto deeplyNestedChallengeWire =
    nativeConversationCanonicalJson(deeplyNestedChallenge);
  BOOST_CHECK_EXCEPTION(
    nativeDurableStorageGrantChallengeV1FromJson(deeplyNestedChallengeWire),
    std::invalid_argument,
    [](const std::invalid_argument& error) {
      return std::string(error.what()) ==
        "durable grant JSON nesting exceeds bound";
    });
  NativeJson atDepthLimitValue = 0;
  for (std::size_t depth = 0; depth != 31; ++depth)
    atDepthLimitValue = NativeJson::array({std::move(atDepthLimitValue)});
  auto atDepthLimitChallenge = encodedChallenge;
  atDepthLimitChallenge["untrusted"] = atDepthLimitValue;
  BOOST_CHECK_EXCEPTION(
    nativeDurableStorageGrantChallengeV1FromJson(
      nativeConversationCanonicalJson(atDepthLimitChallenge)),
    std::invalid_argument,
    [](const std::invalid_argument& error) {
      return std::string(error.what()) == "conversation field set mismatch";
    });
  auto delimitersInString = encodedChallenge;
  delimitersInString["schema"] = "wrong-{[\\\"-]}-schema";
  BOOST_CHECK_EXCEPTION(
    nativeDurableStorageGrantChallengeV1FromJson(
      nativeConversationCanonicalJson(delimitersInString)),
    std::invalid_argument,
    [](const std::invalid_argument& error) {
      return std::string(error.what()) ==
        "durable grant challenge schema or role set is invalid";
    });
  BOOST_CHECK_EXCEPTION(
    nativeDurableStorageGrantChallengeV1FromJson("{]}"),
    std::invalid_argument,
    [](const std::invalid_argument& error) {
      return std::string(error.what()) ==
        "durable grant JSON delimiters do not match";
    });
  wrongChallenge = encodedChallenge;
  wrongChallenge["roles"][0]["dataPrefix"] = "/provider/source-a";
  BOOST_CHECK_THROW(nativeDurableStorageGrantChallengeV1FromJson(
    nativeConversationCanonicalJson(wrongChallenge)), std::invalid_argument);
  wrongChallenge = encodedChallenge;
  wrongChallenge["roles"][0]["packetWireBytes"] = 0;
  BOOST_CHECK_THROW(nativeDurableStorageGrantChallengeV1FromJson(
    nativeConversationCanonicalJson(wrongChallenge)), std::invalid_argument);

  NativeDurableStorageGrantSubmissionV1 submission;
  submission.operationId = challenge.operationId;
  submission.checkpointDigest = challenge.checkpointDigest;
  submission.snapshotDigest = challenge.snapshotDigest;
  submission.roleMapDigest = challenge.roleMapDigest;
  submission.grants.push_back({role.roleName, role.repoTarget.repoNode,
    "/user/owner", role.writerIdentity, role.manifestName, "repo-grant-opaque-id", 1});
  const auto encodedSubmission = nativeDurableStorageGrantSubmissionV1ToJson(submission);
  const std::string expectedSubmission = R"JSON({"checkpointDigest":"sha256:0000000000000000000000000000000000000000000000000000000000000000","grants":[{"grantId":"repo-grant-opaque-id","objectName":"/provider/source-a/kv-cache/v1/root","ownerEpoch":1,"ownerIdentity":"/user/owner","repoNode":"/repo/node-a","roleName":"/Stage/0","writerIdentity":"/provider/source-a"}],"operationId":"event-01J7KVCACHE","roleMapDigest":"sha256:2222222222222222222222222222222222222222222222222222222222222222","schema":"ndnsf-di-kv-storage-grant-submission-v1","snapshotDigest":"sha256:1111111111111111111111111111111111111111111111111111111111111111"})JSON";
  const auto submissionWire = nativeConversationCanonicalJson(encodedSubmission);
  BOOST_CHECK_EQUAL(submissionWire, expectedSubmission);
  const auto decodedSubmission = nativeDurableStorageGrantSubmissionV1FromJson(submissionWire);
  BOOST_REQUIRE_EQUAL(decodedSubmission.grants.size(), 1U);
  BOOST_CHECK_EQUAL(decodedSubmission.grants.front().objectName, role.manifestName);
  BOOST_CHECK_EQUAL(decodedSubmission.grants.front().ownerIdentity, "/user/owner");

  auto typedSubmission = submission;
  typedSubmission.grants[0].repoNode =
    typedRepoNode.toUri(ndn::name::UriFormat::ALTERNATE);
  typedSubmission.grants[0].writerIdentity =
    typedProducer.toUri(ndn::name::UriFormat::ALTERNATE);
  typedSubmission.grants[0].objectName =
    typedManifest.toUri(ndn::name::UriFormat::ALTERNATE);
  const auto typedSubmissionValue =
    nativeDurableStorageGrantSubmissionV1ToJson(typedSubmission);
  const auto typedSubmissionWire =
    nativeConversationCanonicalJson(typedSubmissionValue);
  const auto parsedTypedSubmission =
    nativeDurableStorageGrantSubmissionV1FromJson(typedSubmissionWire);
  BOOST_CHECK_EQUAL(parsedTypedSubmission.grants[0].objectName,
    typedManifest.toUri(ndn::name::UriFormat::ALTERNATE));
  auto aliasSubmission = typedSubmissionValue;
  aliasSubmission["grants"][0]["objectName"] =
    typedManifest.toUri(ndn::name::UriFormat::CANONICAL);
  BOOST_CHECK_THROW(nativeDurableStorageGrantSubmissionV1FromJson(
    nativeConversationCanonicalJson(aliasSubmission)), std::invalid_argument);

  auto deeplyNestedSubmission = typedSubmissionValue;
  deeplyNestedSubmission["untrusted"] = deeplyNestedValue;
  const auto deeplyNestedSubmissionWire =
    nativeConversationCanonicalJson(deeplyNestedSubmission);
  BOOST_CHECK_EXCEPTION(
    nativeDurableStorageGrantSubmissionV1FromJson(deeplyNestedSubmissionWire),
    std::invalid_argument,
    [](const std::invalid_argument& error) {
      return std::string(error.what()) ==
        "durable grant JSON nesting exceeds bound";
    });
  auto wrongSubmission = encodedSubmission;
  wrongSubmission["grants"][0]["ownerEpoch"] = 0;
  BOOST_CHECK_THROW(nativeDurableStorageGrantSubmissionV1FromJson(
    nativeConversationCanonicalJson(wrongSubmission)), std::invalid_argument);
  wrongSubmission = encodedSubmission;
  wrongSubmission["grants"][0]["extra"] = "not-allowed";
  BOOST_CHECK_THROW(nativeDurableStorageGrantSubmissionV1FromJson(
    nativeConversationCanonicalJson(wrongSubmission)), std::invalid_argument);
  auto unsorted = submission;
  auto second = submission.grants.front();
  second.roleName = "/Stage/1";
  unsorted.grants.push_back(second);
  std::swap(unsorted.grants[0], unsorted.grants[1]);
  BOOST_CHECK_THROW(nativeDurableStorageGrantSubmissionV1ToJson(unsorted),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Spec204RemoteRoleTrustPolicyIdentityBindsTargetProfileAndLimits)
{
  NativeRemoteProtectedKvRoleTrustPolicyV1 policy;
  policy.target = {"/repo/node-a", "/repo/provider-a", "/repo/service"};
  policy.producerVerifierPolicyDigest = digest("producer trust profile v1");
  policy.verifyProducerSignature = [](const ndn::Data&) { return true; };

  auto same = policy;
  BOOST_CHECK(same == policy);

  same.producerVerifierPolicyDigest = digest("producer trust profile v2");
  BOOST_CHECK(same != policy);

  same = policy;
  same.target.servicePrefix = "/repo/other-service";
  BOOST_CHECK(same != policy);

  same = policy;
  ++same.limits.maxPacketCount;
  BOOST_CHECK(same != policy);
}

BOOST_AUTO_TEST_CASE(Spec204DurableStorageGrantSubmissionMatchesEntireChallengeAndOwner)
{
  const auto tagged = [](char c) { return "sha256:" + std::string(64, c); };
  const auto makeRole = [&](const std::string& roleName, char suffix) {
    const auto tag = std::string(1, suffix);
    NativeDurableStorageGrantRoleChallengeV1 role;
    role.roleName = roleName;
    role.repoTarget = {"/repo/node-" + tag, "/repo/provider-" + tag, "/repo/service"};
    role.producerPrefix = "/provider/source-" + tag;
    role.writerIdentity = role.producerPrefix;
    role.manifestName = role.producerPrefix + "/kv-cache/v1/root";
    role.publicationIdentity = tagged(suffix);
    role.protectionEpoch = "owner-protection-epoch-7";
    role.dataPrefix = role.producerPrefix + "/kv-cache/v1/data";
    role.packetWireBytes = 1234;
    return role;
  };

  NativeDurableStorageGrantChallengeV1 challenge;
  challenge.operationId = "event-01J7KVCACHE";
  challenge.checkpointDigest = tagged('0');
  challenge.snapshotDigest = tagged('1');
  challenge.roleMapDigest = tagged('2');
  challenge.roles = {makeRole("/Stage/0", 'a'), makeRole("/Stage/1", 'b')};

  const auto makeGrant = [](const NativeDurableStorageGrantRoleChallengeV1& role) {
    return NativeDurableStorageGrantRoleSubmissionV1{
      role.roleName, role.repoTarget.repoNode, "/user/owner", role.writerIdentity,
      role.manifestName, "repo-grant-opaque-id", 1};
  };
  NativeDurableStorageGrantSubmissionV1 submission;
  submission.operationId = challenge.operationId;
  submission.checkpointDigest = challenge.checkpointDigest;
  submission.snapshotDigest = challenge.snapshotDigest;
  submission.roleMapDigest = challenge.roleMapDigest;
  for (const auto& role : challenge.roles)
    submission.grants.push_back(makeGrant(role));

  BOOST_CHECK_NO_THROW(nativeValidateDurableStorageGrantSubmissionV1(
    challenge, submission, "/user/owner"));

  auto wrong = submission;
  wrong.operationId = "event-01J7OTHER";
  BOOST_CHECK_THROW(nativeValidateDurableStorageGrantSubmissionV1(
    challenge, wrong, "/user/owner"), std::invalid_argument);
  wrong = submission;
  wrong.snapshotDigest = tagged('9');
  BOOST_CHECK_THROW(nativeValidateDurableStorageGrantSubmissionV1(
    challenge, wrong, "/user/owner"), std::invalid_argument);
  wrong = submission;
  wrong.checkpointDigest = tagged('9');
  BOOST_CHECK_THROW(nativeValidateDurableStorageGrantSubmissionV1(
    challenge, wrong, "/user/owner"), std::invalid_argument);
  wrong = submission;
  wrong.roleMapDigest = tagged('9');
  BOOST_CHECK_THROW(nativeValidateDurableStorageGrantSubmissionV1(
    challenge, wrong, "/user/owner"), std::invalid_argument);
  wrong = submission;
  wrong.grants.pop_back();
  BOOST_CHECK_THROW(nativeValidateDurableStorageGrantSubmissionV1(
    challenge, wrong, "/user/owner"), std::invalid_argument);
  wrong = submission;
  wrong.grants[1].roleName = "/Stage/2";
  BOOST_CHECK_THROW(nativeValidateDurableStorageGrantSubmissionV1(
    challenge, wrong, "/user/owner"), std::invalid_argument);
  wrong = submission;
  wrong.grants[0].repoNode = "/repo/other";
  BOOST_CHECK_THROW(nativeValidateDurableStorageGrantSubmissionV1(
    challenge, wrong, "/user/owner"), std::invalid_argument);
  wrong = submission;
  wrong.grants[0].ownerIdentity = "/user/other";
  BOOST_CHECK_THROW(nativeValidateDurableStorageGrantSubmissionV1(
    challenge, wrong, "/user/owner"), std::invalid_argument);
  wrong = submission;
  wrong.grants[0].writerIdentity = "/provider/other";
  BOOST_CHECK_THROW(nativeValidateDurableStorageGrantSubmissionV1(
    challenge, wrong, "/user/owner"), std::invalid_argument);
  wrong = submission;
  wrong.grants[0].objectName = "/provider/source-a/kv-cache/v1/other";
  BOOST_CHECK_THROW(nativeValidateDurableStorageGrantSubmissionV1(
    challenge, wrong, "/user/owner"), std::invalid_argument);
  BOOST_CHECK_THROW(nativeValidateDurableStorageGrantSubmissionV1(
    challenge, submission, "/user/other"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Spec204ConversationHandoffControlWireHasExactBoundedBindings)
{
  const auto tagged = [](char c) { return "sha256:" + std::string(64, c); };
  NativeConversationHandoffControlV1 control;
  control.operationId = "event-01J7KVCACHE";
  control.conversationId = "spec204-conversation-01";
  control.checkpointDigest = tagged('6');
  control.sourceRoleMapDigest = tagged('7');
  control.destinationRoleMapDigest = tagged('8');
  control.deadlineMs = 1'800'000'000'000ULL;

  const auto beginValue = nativeConversationHandoffControlV1ToJson(control);
  BOOST_REQUIRE_EQUAL(beginValue.size(), 8U);
  BOOST_CHECK_EQUAL(beginValue.at("action"), "BEGIN");
  BOOST_CHECK(beginValue.at("grantSubmission").is_null());
  const auto beginWire = nativeConversationCanonicalJson(beginValue);
  const std::string expectedBeginWire =
    R"JSON({"action":"BEGIN","checkpointDigest":"sha256:6666666666666666666666666666666666666666666666666666666666666666","conversationId":"spec204-conversation-01","deadlineMs":1800000000000,"destinationRoleMapDigest":"sha256:8888888888888888888888888888888888888888888888888888888888888888","grantSubmission":null,"operationId":"event-01J7KVCACHE","schema":"ndnsf-di-conversation-handoff-control-v1","sourceRoleMapDigest":"sha256:7777777777777777777777777777777777777777777777777777777777777777"})JSON";
  BOOST_CHECK_EQUAL(beginWire, expectedBeginWire);
  const auto decodedBegin = nativeConversationHandoffControlV1FromJson(beginWire);
  BOOST_CHECK(decodedBegin.action == NativeConversationHandoffActionV1::Begin);
  BOOST_CHECK_EQUAL(decodedBegin.operationId, control.operationId);
  BOOST_CHECK_EQUAL(decodedBegin.conversationId, control.conversationId);
  BOOST_CHECK_EQUAL(decodedBegin.sourceRoleMapDigest, control.sourceRoleMapDigest);
  BOOST_CHECK_EQUAL(decodedBegin.destinationRoleMapDigest,
                    control.destinationRoleMapDigest);

  for (const auto action : {NativeConversationHandoffActionV1::Poll,
                            NativeConversationHandoffActionV1::Commit,
                            NativeConversationHandoffActionV1::Cancel}) {
    control.action = action;
    const auto value = nativeConversationHandoffControlV1ToJson(control);
    const auto decoded = nativeConversationHandoffControlV1FromJson(
      nativeConversationCanonicalJson(value));
    BOOST_CHECK(decoded.action == action);
  }

  control.action = NativeConversationHandoffActionV1::Commit;
  const auto commitWire = nativeConversationCanonicalJson(
    nativeConversationHandoffControlV1ToJson(control));
  const auto beginActionOffset = expectedBeginWire.find("\"BEGIN\"");
  BOOST_REQUIRE(beginActionOffset != std::string::npos);
  auto expectedCommitWire = expectedBeginWire;
  expectedCommitWire.replace(beginActionOffset, 7, "\"COMMIT\"");
  BOOST_CHECK_EQUAL(commitWire, expectedCommitWire);
  BOOST_CHECK(nativeConversationHandoffControlV1FromJson(commitWire).action ==
              NativeConversationHandoffActionV1::Commit);

  NativeDurableStorageGrantSubmissionV1 submission;
  submission.operationId = control.operationId;
  submission.checkpointDigest = control.checkpointDigest;
  submission.snapshotDigest = tagged('9');
  submission.roleMapDigest = control.sourceRoleMapDigest;
  submission.grants.push_back({"/Stage/0", "/repo/node-a", "/user/owner",
    "/provider/source-a", "/provider/source-a/kv-cache/v1/root",
    "repo-grant-opaque-id", 1});
  control.action = NativeConversationHandoffActionV1::Grant;
  control.grantSubmission = submission;
  const auto grantValue = nativeConversationHandoffControlV1ToJson(control);
  BOOST_REQUIRE(grantValue.at("grantSubmission").is_object());
  const auto grantWire = nativeConversationCanonicalJson(grantValue);
  const auto decodedGrant = nativeConversationHandoffControlV1FromJson(grantWire);
  BOOST_REQUIRE(decodedGrant.grantSubmission.has_value());
  BOOST_CHECK_EQUAL(decodedGrant.grantSubmission->snapshotDigest,
                    submission.snapshotDigest);
  BOOST_CHECK_EQUAL(decodedGrant.grantSubmission->grants.front().objectName,
                    submission.grants.front().objectName);

  auto wrong = control;
  wrong.grantSubmission->operationId = "event-01J7OTHER";
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1ToJson(wrong),
                    std::invalid_argument);
  wrong = control;
  wrong.grantSubmission->checkpointDigest = tagged('4');
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1ToJson(wrong),
                    std::invalid_argument);
  wrong = control;
  wrong.grantSubmission->roleMapDigest = tagged('4');
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1ToJson(wrong),
                    std::invalid_argument);
  wrong = control;
  wrong.grantSubmission.reset();
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1ToJson(wrong),
                    std::invalid_argument);

  wrong = control;
  wrong.action = NativeConversationHandoffActionV1::Cancel;
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1ToJson(wrong),
                    std::invalid_argument);
  wrong = control;
  wrong.action = NativeConversationHandoffActionV1::Commit;
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1ToJson(wrong),
                    std::invalid_argument);
  wrong = control;
  wrong.operationId = "event/id";
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1ToJson(wrong),
                    std::invalid_argument);
  wrong = control;
  wrong.conversationId = "short";
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1ToJson(wrong),
                    std::invalid_argument);
  wrong = control;
  wrong.destinationRoleMapDigest = "sha256:bad";
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1ToJson(wrong),
                    std::invalid_argument);
  wrong = control;
  wrong.deadlineMs = 0;
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1ToJson(wrong),
                    std::invalid_argument);

  auto malformed = grantValue;
  malformed["action"] = "JOIN";
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1FromJson(
    nativeConversationCanonicalJson(malformed)), std::invalid_argument);
  malformed = grantValue;
  malformed["requesterIdentity"] = "/user/forged";
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1FromJson(
    nativeConversationCanonicalJson(malformed)), std::invalid_argument);
  malformed = grantValue;
  malformed["grantSubmission"]["roleMapDigest"] = tagged('4');
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1FromJson(
    nativeConversationCanonicalJson(malformed)), std::invalid_argument);
  malformed = grantValue;
  malformed["deadlineMs"] = -1;
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1FromJson(
    nativeConversationCanonicalJson(malformed)), std::invalid_argument);
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1FromJson(grantWire + "\n"),
                    std::invalid_argument);
  BOOST_CHECK_THROW(nativeConversationHandoffControlV1FromJson(
    std::string(128 * 1024 + 1, ' ')), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Spec204ConversationHandoffReplyHasBoundedStateAndPayloadBindings)
{
  const auto tagged = [](char c) { return "sha256:" + std::string(64, c); };
  const auto makeRole = [&](const std::string& roleName, char suffix) {
    const auto tag = std::string(1, suffix);
    NativeDurableStorageGrantRoleChallengeV1 role;
    role.roleName = roleName;
    role.repoTarget = {"/repo/node-" + tag, "/repo/provider-" + tag, "/repo/service"};
    role.producerPrefix = "/provider/source-" + tag;
    role.writerIdentity = role.producerPrefix;
    role.manifestName = role.producerPrefix + "/kv-cache/v1/root";
    role.publicationIdentity = tagged(suffix);
    role.protectionEpoch = "owner-protection-epoch-7";
    role.dataPrefix = role.producerPrefix + "/kv-cache/v1/data";
    role.packetWireBytes = 1234;
    return role;
  };

  NativeDurableStorageGrantChallengeV1 challenge;
  challenge.operationId = "event-01J7KVCACHE";
  challenge.checkpointDigest = tagged('6');
  challenge.snapshotDigest = tagged('5');
  challenge.roleMapDigest = tagged('7');
  challenge.roles = {makeRole("/Stage/0", 'a'), makeRole("/Stage/1", 'b')};

  NativeConversationHandoffReplyV1 reply;
  reply.state = NativeConversationHandoffStateV1::GrantRequired;
  reply.operationId = challenge.operationId;
  reply.conversationId = "spec204-conversation-01";
  reply.checkpointDigest = challenge.checkpointDigest;
  reply.sourceRoleMapDigest = challenge.roleMapDigest;
  reply.destinationRoleMapDigest = tagged('8');
  reply.deadlineMs = 12'000;
  reply.grantChallenge = challenge;

  const auto grantValue = nativeConversationHandoffReplyV1ToJson(reply);
  BOOST_REQUIRE_EQUAL(grantValue.size(), 9U);
  BOOST_CHECK_EQUAL(grantValue.at("state"), "GRANT_REQUIRED");
  BOOST_CHECK(grantValue.at("ownerReferencesByRole").empty());
  const auto grantWire = nativeConversationCanonicalJson(grantValue);
  const auto decodedGrant = nativeConversationHandoffReplyV1FromJson(grantWire);
  BOOST_CHECK(decodedGrant.state == NativeConversationHandoffStateV1::GrantRequired);
  BOOST_REQUIRE(decodedGrant.grantChallenge.has_value());
  BOOST_CHECK_EQUAL(decodedGrant.grantChallenge->snapshotDigest, challenge.snapshotDigest);

  NativeConversationHandoffControlV1 correlatedControl;
  correlatedControl.action = NativeConversationHandoffActionV1::Begin;
  correlatedControl.operationId = reply.operationId;
  correlatedControl.conversationId = reply.conversationId;
  correlatedControl.checkpointDigest = reply.checkpointDigest;
  correlatedControl.sourceRoleMapDigest = reply.sourceRoleMapDigest;
  correlatedControl.destinationRoleMapDigest = reply.destinationRoleMapDigest;
  correlatedControl.deadlineMs = reply.deadlineMs;
  BOOST_CHECK_NO_THROW(nativeValidateConversationHandoffReplyBindingV1(
    correlatedControl, decodedGrant));
  auto invalidReply = decodedGrant;
  invalidReply.deadlineMs += 1;
  BOOST_CHECK_THROW(nativeValidateConversationHandoffReplyBindingV1(
    correlatedControl, invalidReply), std::invalid_argument);
  invalidReply = decodedGrant;
  invalidReply.grantChallenge->operationId += "-other";
  BOOST_CHECK_THROW(nativeValidateConversationHandoffReplyBindingV1(
    correlatedControl, invalidReply), std::invalid_argument);
  auto invalidControl = correlatedControl;
  invalidControl.action = static_cast<NativeConversationHandoffActionV1>(99);
  BOOST_CHECK_THROW(nativeValidateConversationHandoffReplyBindingV1(
    invalidControl, decodedGrant), std::invalid_argument);
  auto mismatchedControl = correlatedControl;
  mismatchedControl.operationId += "-other";
  BOOST_CHECK_THROW(nativeValidateConversationHandoffReplyBindingV1(
    mismatchedControl, decodedGrant), std::invalid_argument);
  mismatchedControl = correlatedControl;
  mismatchedControl.conversationId += "-other";
  BOOST_CHECK_THROW(nativeValidateConversationHandoffReplyBindingV1(
    mismatchedControl, decodedGrant), std::invalid_argument);
  mismatchedControl = correlatedControl;
  mismatchedControl.checkpointDigest = tagged('4');
  BOOST_CHECK_THROW(nativeValidateConversationHandoffReplyBindingV1(
    mismatchedControl, decodedGrant), std::invalid_argument);
  mismatchedControl = correlatedControl;
  mismatchedControl.sourceRoleMapDigest = tagged('4');
  BOOST_CHECK_THROW(nativeValidateConversationHandoffReplyBindingV1(
    mismatchedControl, decodedGrant), std::invalid_argument);
  mismatchedControl = correlatedControl;
  mismatchedControl.destinationRoleMapDigest = tagged('4');
  BOOST_CHECK_THROW(nativeValidateConversationHandoffReplyBindingV1(
    mismatchedControl, decodedGrant), std::invalid_argument);
  mismatchedControl = correlatedControl;
  mismatchedControl.deadlineMs += 1;
  BOOST_CHECK_THROW(nativeValidateConversationHandoffReplyBindingV1(
    mismatchedControl, decodedGrant), std::invalid_argument);

  reply.state = NativeConversationHandoffStateV1::Saved;
  reply.grantChallenge.reset();
  reply.ownerReferencesByRole = {
    {"/Stage/0", NativeJson{{"reference", "source-a"}}},
    {"/Stage/1", NativeJson{{"reference", "source-b"}}}};
  const auto savedValue = nativeConversationHandoffReplyV1ToJson(reply);
  const auto savedWire = nativeConversationCanonicalJson(savedValue);
  const auto decodedSaved = nativeConversationHandoffReplyV1FromJson(savedWire);
  BOOST_CHECK(decodedSaved.state == NativeConversationHandoffStateV1::Saved);
  BOOST_REQUIRE_EQUAL(decodedSaved.ownerReferencesByRole.size(), 2U);
  BOOST_CHECK_EQUAL(decodedSaved.ownerReferencesByRole.at("/Stage/1").at("reference"),
                    "source-b");

  reply.state = NativeConversationHandoffStateV1::Pending;
  reply.ownerReferencesByRole.clear();
  const auto pendingWire = nativeConversationCanonicalJson(
    nativeConversationHandoffReplyV1ToJson(reply));
  const std::string expectedPendingWire =
    R"JSON({"checkpointDigest":"sha256:6666666666666666666666666666666666666666666666666666666666666666","conversationId":"spec204-conversation-01","deadlineMs":1800000000000,"destinationRoleMapDigest":"sha256:8888888888888888888888888888888888888888888888888888888888888888","grantChallenge":null,"operationId":"event-01J7KVCACHE","ownerReferencesByRole":{},"schema":"ndnsf-di-conversation-handoff-reply-v1","sourceRoleMapDigest":"sha256:7777777777777777777777777777777777777777777777777777777777777777","state":"PENDING"})JSON";
  BOOST_CHECK_EQUAL(pendingWire, expectedPendingWire);
  BOOST_CHECK(nativeConversationHandoffReplyV1FromJson(pendingWire).state ==
              NativeConversationHandoffStateV1::Pending);

  reply.state = NativeConversationHandoffStateV1::BoundaryReady;
  const auto boundaryReadyWire = nativeConversationCanonicalJson(
    nativeConversationHandoffReplyV1ToJson(reply));
  const std::string expectedBoundaryReadyWire =
    R"JSON({"checkpointDigest":"sha256:6666666666666666666666666666666666666666666666666666666666666666","conversationId":"spec204-conversation-01","deadlineMs":1800000000000,"destinationRoleMapDigest":"sha256:8888888888888888888888888888888888888888888888888888888888888888","grantChallenge":null,"operationId":"event-01J7KVCACHE","ownerReferencesByRole":{},"schema":"ndnsf-di-conversation-handoff-reply-v1","sourceRoleMapDigest":"sha256:7777777777777777777777777777777777777777777777777777777777777777","state":"BOUNDARY_READY"})JSON";
  BOOST_CHECK_EQUAL(boundaryReadyWire, expectedBoundaryReadyWire);
  const auto boundaryReady =
    nativeConversationHandoffReplyV1FromJson(boundaryReadyWire);
  BOOST_CHECK(boundaryReady.state == NativeConversationHandoffStateV1::BoundaryReady);
  correlatedControl.action = NativeConversationHandoffActionV1::Poll;
  BOOST_CHECK_NO_THROW(nativeValidateConversationHandoffReplyBindingV1(
    correlatedControl, boundaryReady));
  correlatedControl.action = NativeConversationHandoffActionV1::Begin;
  BOOST_CHECK_THROW(nativeValidateConversationHandoffReplyBindingV1(
    correlatedControl, boundaryReady), std::invalid_argument);
  correlatedControl.action = NativeConversationHandoffActionV1::Commit;
  BOOST_CHECK_THROW(nativeValidateConversationHandoffReplyBindingV1(
    correlatedControl, boundaryReady), std::invalid_argument);
  auto invalidBoundaryReady = boundaryReady;
  invalidBoundaryReady.grantChallenge = challenge;
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1ToJson(invalidBoundaryReady),
                    std::invalid_argument);
  invalidBoundaryReady = boundaryReady;
  invalidBoundaryReady.deadlineMs = 0;
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1ToJson(invalidBoundaryReady),
                    std::invalid_argument);
  invalidBoundaryReady = boundaryReady;
  invalidBoundaryReady.ownerReferencesByRole = {
    {"/Stage/0", NativeJson{{"reference", "not-yet-committed"}}}};
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1ToJson(invalidBoundaryReady),
                    std::invalid_argument);

  reply.state = NativeConversationHandoffStateV1::Cancelled;
  BOOST_CHECK(nativeConversationHandoffReplyV1FromJson(
    nativeConversationCanonicalJson(nativeConversationHandoffReplyV1ToJson(reply))).state ==
    NativeConversationHandoffStateV1::Cancelled);
  reply.state = NativeConversationHandoffStateV1::CancelClosed;
  BOOST_CHECK(nativeConversationHandoffReplyV1FromJson(
    nativeConversationCanonicalJson(nativeConversationHandoffReplyV1ToJson(reply))).state ==
    NativeConversationHandoffStateV1::CancelClosed);

  reply.state = NativeConversationHandoffStateV1::GrantRequired;
  reply.grantChallenge.reset();
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1ToJson(reply),
                    std::invalid_argument);
  reply.grantChallenge = challenge;
  reply.grantChallenge->checkpointDigest = tagged('4');
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1ToJson(reply),
                    std::invalid_argument);
  reply.grantChallenge = challenge;
  reply.grantChallenge->operationId = "event-01J7OTHER";
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1ToJson(reply),
                    std::invalid_argument);
  reply.grantChallenge = challenge;
  reply.grantChallenge->roleMapDigest = tagged('4');
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1ToJson(reply),
                    std::invalid_argument);
  reply.grantChallenge = challenge;
  reply.state = NativeConversationHandoffStateV1::Pending;
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1ToJson(reply),
                    std::invalid_argument);
  reply.grantChallenge.reset();
  reply.state = NativeConversationHandoffStateV1::Saved;
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1ToJson(reply),
                    std::invalid_argument);
  reply.ownerReferencesByRole = {{"/Stage/0", NativeJson("not-an-object")}};
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1ToJson(reply),
                    std::invalid_argument);
  reply.ownerReferencesByRole = {
    {"/Stage/0", NativeJson{{"large", std::string(128 * 1024, 'x')}}}};
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1ToJson(reply),
                    std::invalid_argument);

  auto malformed = grantValue;
  malformed["state"] = "COMPLETED";
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1FromJson(
    nativeConversationCanonicalJson(malformed)), std::invalid_argument);
  malformed = grantValue;
  malformed["ownerIdentity"] = "/user/forged";
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1FromJson(
    nativeConversationCanonicalJson(malformed)), std::invalid_argument);
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1FromJson(grantWire + "\n"),
                    std::invalid_argument);
  BOOST_CHECK_THROW(nativeConversationHandoffReplyV1FromJson(
    std::string(128 * 1024 + 1, ' ')), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Spec212WithdrawalQueryAndConfirmHaveExactCanonicalBindings)
{
  const auto tagged = [](char ch) { return "sha256:" + std::string(64, ch); };
  NativeWithdrawalQueryV1 query;
  query.conversationId = "conversation-a-0001";
  query.checkpointDigest = tagged('a');
  query.deadlineMs = 1'800'000'000'000;
  const auto queryValue = nativeWithdrawalQueryV1ToJson(query);
  BOOST_REQUIRE_EQUAL(queryValue.size(), 4U);
  const auto queryWire = nativeConversationCanonicalJson(queryValue);
  const auto expectedQuery = std::string("{\"checkpointDigest\":\"") + tagged('a') +
    "\",\"conversationId\":\"conversation-a-0001\",\"deadlineMs\":1800000000000,"
    "\"schema\":\"ndnsf-di-withdrawal-query-v1\"}";
  BOOST_CHECK_EQUAL(queryWire, expectedQuery);
  const auto parsedQuery = nativeWithdrawalQueryV1FromJson(queryWire);
  BOOST_CHECK_EQUAL(parsedQuery.conversationId, query.conversationId);
  BOOST_CHECK_EQUAL(parsedQuery.checkpointDigest, query.checkpointDigest);
  BOOST_CHECK_EQUAL(parsedQuery.deadlineMs, query.deadlineMs);

  NativeWithdrawalConfirmV1 confirm;
  confirm.withdrawalId = "withdrawal-001";
  confirm.registrationGeneration = 17;
  confirm.conversationId = query.conversationId;
  confirm.checkpointDigest = query.checkpointDigest;
  confirm.operationId = "event-01J7KVCACHE";
  confirm.completeRoleSetDigest = tagged('b');
  confirm.journalDigest = tagged('c');
  confirm.deadlineMs = query.deadlineMs;
  const auto confirmValue = nativeWithdrawalConfirmV1ToJson(confirm);
  BOOST_REQUIRE_EQUAL(confirmValue.size(), 9U);
  const auto confirmWire = nativeConversationCanonicalJson(confirmValue);
  const auto expectedConfirm = std::string("{\"checkpointDigest\":\"") + tagged('a') +
    "\",\"completeRoleSetDigest\":\"" + tagged('b') +
    "\",\"conversationId\":\"conversation-a-0001\",\"deadlineMs\":1800000000000,"
    "\"journalDigest\":\"" + tagged('c') +
    "\",\"operationId\":\"event-01J7KVCACHE\",\"registrationGeneration\":17,"
    "\"schema\":\"ndnsf-di-withdrawal-confirm-v1\",\"withdrawalId\":\"withdrawal-001\"}";
  BOOST_CHECK_EQUAL(confirmWire, expectedConfirm);
  const auto parsedConfirm = nativeWithdrawalConfirmV1FromJson(confirmWire);
  BOOST_CHECK_EQUAL(parsedConfirm.withdrawalId, confirm.withdrawalId);
  BOOST_CHECK_EQUAL(parsedConfirm.registrationGeneration, confirm.registrationGeneration);
  BOOST_CHECK_EQUAL(parsedConfirm.operationId, confirm.operationId);
  BOOST_CHECK_EQUAL(parsedConfirm.completeRoleSetDigest, confirm.completeRoleSetDigest);
  BOOST_CHECK_EQUAL(parsedConfirm.journalDigest, confirm.journalDigest);

  NativeWithdrawalConfirmReplyV1 confirmReply{confirm};
  const auto confirmReplyValue = nativeWithdrawalConfirmReplyV1ToJson(confirmReply);
  BOOST_REQUIRE_EQUAL(confirmReplyValue.size(), 10U);
  const auto confirmReplyWire = nativeConversationCanonicalJson(confirmReplyValue);
  const auto parsedConfirmReply = nativeWithdrawalConfirmReplyV1FromJson(confirmReplyWire);
  BOOST_CHECK_EQUAL(parsedConfirmReply.confirm.withdrawalId, confirm.withdrawalId);
  BOOST_CHECK_EQUAL(parsedConfirmReply.confirm.operationId, confirm.operationId);
  BOOST_CHECK_EQUAL(parsedConfirmReply.confirm.journalDigest, confirm.journalDigest);
  auto changedConfirmReply = confirmReplyValue;
  changedConfirmReply["status"] = "SAVED";
  BOOST_CHECK_THROW(nativeWithdrawalConfirmReplyV1FromJson(
    nativeConversationCanonicalJson(changedConfirmReply)), std::invalid_argument);
  changedConfirmReply = confirmReplyValue;
  changedConfirmReply["journalDigest"] = "sha256:bad";
  BOOST_CHECK_THROW(nativeWithdrawalConfirmReplyV1FromJson(
    nativeConversationCanonicalJson(changedConfirmReply)), std::invalid_argument);
  changedConfirmReply = confirmReplyValue;
  changedConfirmReply["extra"] = true;
  BOOST_CHECK_THROW(nativeWithdrawalConfirmReplyV1FromJson(
    nativeConversationCanonicalJson(changedConfirmReply)), std::invalid_argument);
  BOOST_CHECK_THROW(nativeWithdrawalConfirmReplyV1FromJson(confirmReplyWire + "\n"),
                    std::invalid_argument);

  auto changedQuery = queryValue;
  changedQuery["extra"] = true;
  BOOST_CHECK_THROW(nativeWithdrawalQueryV1FromJson(
    nativeConversationCanonicalJson(changedQuery)), std::invalid_argument);
  changedQuery = queryValue;
  changedQuery["checkpointDigest"] = "sha256:bad";
  BOOST_CHECK_THROW(nativeWithdrawalQueryV1FromJson(
    nativeConversationCanonicalJson(changedQuery)), std::invalid_argument);
  changedQuery = queryValue;
  changedQuery["deadlineMs"] = 0;
  BOOST_CHECK_THROW(nativeWithdrawalQueryV1FromJson(
    nativeConversationCanonicalJson(changedQuery)), std::invalid_argument);
  BOOST_CHECK_THROW(nativeWithdrawalQueryV1FromJson(queryWire + "\n"),
                    std::invalid_argument);
  BOOST_CHECK_THROW(nativeWithdrawalQueryV1FromJson(
    std::string(128 * 1024 + 1, ' ')), std::invalid_argument);

  auto changedConfirm = confirmValue;
  changedConfirm["registrationGeneration"] = 0;
  BOOST_CHECK_THROW(nativeWithdrawalConfirmV1FromJson(
    nativeConversationCanonicalJson(changedConfirm)), std::invalid_argument);
  changedConfirm = confirmValue;
  changedConfirm["journalDigest"] = "sha256:bad";
  BOOST_CHECK_THROW(nativeWithdrawalConfirmV1FromJson(
    nativeConversationCanonicalJson(changedConfirm)), std::invalid_argument);
  changedConfirm = confirmValue;
  changedConfirm["schema"] = "ndnsf-di-withdrawal-confirm-v2";
  BOOST_CHECK_THROW(nativeWithdrawalConfirmV1FromJson(
    nativeConversationCanonicalJson(changedConfirm)), std::invalid_argument);
  changedConfirm = confirmValue;
  changedConfirm["ownerIdentity"] = "/forged/owner";
  BOOST_CHECK_THROW(nativeWithdrawalConfirmV1FromJson(
    nativeConversationCanonicalJson(changedConfirm)), std::invalid_argument);
  BOOST_CHECK_THROW(nativeWithdrawalConfirmV1FromJson(confirmWire + "\n"),
                    std::invalid_argument);
  BOOST_CHECK_THROW(nativeWithdrawalConfirmV1FromJson(
    std::string(128 * 1024 + 1, ' ')), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Spec212WithdrawalIntentProofRetainsProviderSignedOriginalPacket)
{
  const auto tagged = [](char ch) { return "sha256:" + std::string(64, ch); };
  const ndn::Name provider("/spec212/provider/a");
  ndn::KeyChain keyChain("pib-memory:spec212-withdrawal-proof",
                         "tpm-memory:spec212-withdrawal-proof");
  const auto certificate = keyChain.createIdentity(provider).getDefaultKey()
                             .getDefaultCertificate();
  const std::map<std::string, std::string> sourceProvidersByRole{
    {"/Stage/0", "/spec212/provider/a"},
    {"/Stage/1", "/spec212/provider/a"},
    {"/Stage/2", "/spec212/provider/b"}};
  NativeJson sourceRoles = NativeJson::array();
  for (const auto& [role, source] : sourceProvidersByRole)
    sourceRoles.push_back(NativeJson::array({role, source}));
  BOOST_CHECK_EQUAL(digest(nativeConversationCanonicalJson(sourceRoles)),
                    nativePlanningDigest(nativeCanonicalJson(sourceRoles)));
  NativeWithdrawalIntentV1 intent;
  intent.withdrawalId = "withdrawal-001";
  intent.ownerIdentity = "/spec212/user";
  intent.serviceName = "/LLM/Pipeline/Generate";
  intent.providerBootId = "boot-a";
  intent.registrationGeneration = 17;
  intent.scope = NativeWithdrawalScopeV1::Service;
  intent.deadlineMs = 1'800'000'000'000;
  intent.conversationId = "conversation-a-0001";
  intent.checkpointDigest = tagged('a');
  intent.sourceRoleMapDigest = digest(nativeConversationCanonicalJson(sourceRoles));
  intent.localRoles = {"/Stage/0", "/Stage/1"};
  intent.policyDigest = tagged('c');
  const auto intentValue = nativeWithdrawalIntentV1ToJson(intent);
  BOOST_REQUIRE_EQUAL(intentValue.size(), 13U);
  const auto intentWire = nativeConversationCanonicalJson(intentValue);
  BOOST_CHECK_EQUAL(nativeWithdrawalIntentV1FromJson(intentWire).ownerIdentity,
                    intent.ownerIdentity);
  BOOST_CHECK_EQUAL(nativeWithdrawalIntentV1FromJson(intentWire).localRoles.size(), 2U);

  const auto signer = [&] (ndn::Data& data) {
    keyChain.sign(data, ndn::security::signingByIdentity(provider));
  };
  const auto verifier = [&] (const ndn::Data& data, const ndn::Name& expected) {
    return expected == provider && ndn::security::verifySignature(data, certificate);
  };
  const auto proof = nativeSignWithdrawalIntentProofV1(provider, intent, signer);
  BOOST_CHECK(provider.isPrefixOf(proof.getName()));
  BOOST_CHECK_EQUAL(proof.getName(), nativeWithdrawalIntentProofNameV1(provider, intent));
  const auto proofContent = std::string(
    reinterpret_cast<const char*>(proof.getContent().value()),
    proof.getContent().value_size());
  const auto proofPayload = nativeParseJson(proofContent);
  BOOST_REQUIRE_EQUAL(proofPayload.size(), 13U);
  BOOST_CHECK_EQUAL(proofPayload.at("ownerIdentityDigest"),
    digest(nativeConversationCanonicalJson(NativeJson(intent.ownerIdentity))));
  BOOST_CHECK_EQUAL(proofPayload.at("localRoleSetDigest"),
    digest(nativeConversationCanonicalJson(NativeJson(intent.localRoles))));
  BOOST_CHECK(!proofPayload.contains("ownerIdentity"));
  BOOST_CHECK(!proofPayload.contains("localRoles"));
  const auto encoded = proof.wireEncode();
  ndn::Buffer originalWire(encoded.data(), encoded.data() + encoded.size());
  BOOST_CHECK_NO_THROW(nativeValidateWithdrawalIntentProofV1(
    originalWire, provider, intent, intent.deadlineMs - 1, verifier));
  NativeWithdrawalQueryReplyV1 queryReply;
  queryReply.query.conversationId = intent.conversationId;
  queryReply.query.checkpointDigest = intent.checkpointDigest;
  queryReply.query.deadlineMs = intent.deadlineMs - 1'000;
  auto noIntentValue = nativeWithdrawalQueryReplyV1ToJson(queryReply);
  BOOST_CHECK(noIntentValue.at("intent").is_null());
  BOOST_CHECK(noIntentValue.at("proofWire").is_null());
  const auto noIntentWire = nativeConversationCanonicalJson(noIntentValue);
  BOOST_CHECK(nativeWithdrawalQueryReplyV1FromJson(noIntentWire).status ==
              NativeWithdrawalQueryStatusV1::NoIntent);
  queryReply.status = NativeWithdrawalQueryStatusV1::Busy;
  BOOST_CHECK(nativeWithdrawalQueryReplyV1FromJson(
    nativeConversationCanonicalJson(nativeWithdrawalQueryReplyV1ToJson(queryReply))).status ==
    NativeWithdrawalQueryStatusV1::Busy);
  queryReply.originalProofWire = originalWire;
  BOOST_CHECK_THROW(nativeWithdrawalQueryReplyV1ToJson(queryReply),
                    std::invalid_argument);
  queryReply.originalProofWire.clear();
  queryReply.status = NativeWithdrawalQueryStatusV1::Intent;
  queryReply.intent = intent;
  queryReply.originalProofWire = originalWire;
  const auto intentReplyValue = nativeWithdrawalQueryReplyV1ToJson(queryReply);
  const auto intentReplyWire = nativeConversationCanonicalJson(intentReplyValue);
  const auto parsedReply = nativeWithdrawalQueryReplyV1FromJson(intentReplyWire);
  BOOST_CHECK(parsedReply.status == NativeWithdrawalQueryStatusV1::Intent);
  BOOST_CHECK(parsedReply.originalProofWire == originalWire);
  BOOST_REQUIRE(parsedReply.intent);
  BOOST_CHECK_NO_THROW(nativeValidateWithdrawalIntentProofV1(
    parsedReply.originalProofWire, provider, *parsedReply.intent,
    intent.deadlineMs - 1, verifier));
  auto malformedReply = intentReplyValue;
  malformedReply["checkpointDigest"] = tagged('d');
  BOOST_CHECK_THROW(nativeWithdrawalQueryReplyV1FromJson(
    nativeConversationCanonicalJson(malformedReply)), std::invalid_argument);
  malformedReply = intentReplyValue;
  malformedReply["proofWire"] = nullptr;
  BOOST_CHECK_THROW(nativeWithdrawalQueryReplyV1FromJson(
    nativeConversationCanonicalJson(malformedReply)), std::invalid_argument);
  malformedReply = noIntentValue;
  malformedReply["intent"] = nativeWithdrawalIntentV1ToJson(intent);
  BOOST_CHECK_THROW(nativeWithdrawalQueryReplyV1FromJson(
    nativeConversationCanonicalJson(malformedReply)), std::invalid_argument);
  malformedReply = intentReplyValue;
  malformedReply["status"] = "SAVED";
  BOOST_CHECK_THROW(nativeWithdrawalQueryReplyV1FromJson(
    nativeConversationCanonicalJson(malformedReply)), std::invalid_argument);
  malformedReply = intentReplyValue;
  malformedReply["ownerIdentity"] = "/forged/owner";
  BOOST_CHECK_THROW(nativeWithdrawalQueryReplyV1FromJson(
    nativeConversationCanonicalJson(malformedReply)), std::invalid_argument);
  BOOST_CHECK_THROW(nativeWithdrawalQueryReplyV1FromJson(intentReplyWire + "\n"),
                    std::invalid_argument);
  auto duplicateReplyKey = intentReplyWire;
  duplicateReplyKey.insert(1, "\"status\":\"INTENT\",");
  BOOST_CHECK_THROW(nativeWithdrawalQueryReplyV1FromJson(duplicateReplyKey),
                    std::invalid_argument);
  BOOST_CHECK_THROW(nativeWithdrawalQueryReplyV1FromJson(
    std::string(128 * 1024 + 1, ' ')), std::invalid_argument);
  NativeConversationHandoffControlV2 saveControl;
  saveControl.control.action = NativeConversationHandoffActionV1::Begin;
  saveControl.control.operationId = "owner-save-001";
  saveControl.control.conversationId = intent.conversationId;
  saveControl.control.checkpointDigest = intent.checkpointDigest;
  saveControl.control.sourceRoleMapDigest = intent.sourceRoleMapDigest;
  saveControl.control.destinationRoleMapDigest = digest("[]");
  saveControl.control.deadlineMs = intent.deadlineMs - 1'000;
  saveControl.sourceProvidersByRole = sourceProvidersByRole;
  saveControl.trigger = NativeConversationSaveTriggerV2::WithdrawalSave;
  saveControl.withdrawingProviderIdentity = provider.toUri(ndn::name::UriFormat::ALTERNATE);
  saveControl.withdrawalIntent = intent;
  saveControl.withdrawalProofWire = originalWire;
  const auto saveValue = nativeConversationHandoffControlV2ToJson(saveControl);
  BOOST_REQUIRE_EQUAL(saveValue.size(), 13U);
  BOOST_CHECK_EQUAL(saveValue.at("schema"), "ndnsf-di-handoff-control-v2");
  const auto saveWire = nativeConversationCanonicalJson(saveValue);
  const auto parsedSave = nativeConversationHandoffControlV2FromJson(saveWire);
  BOOST_CHECK(parsedSave.trigger == NativeConversationSaveTriggerV2::WithdrawalSave);
  BOOST_CHECK(parsedSave.withdrawalProofWire == originalWire);
  BOOST_CHECK_NO_THROW(nativeValidateWithdrawalIntentProofV1(
    parsedSave.withdrawalProofWire, provider, *parsedSave.withdrawalIntent,
    intent.deadlineMs - 1, verifier));
  auto pollControl = saveControl;
  pollControl.control.action = NativeConversationHandoffActionV1::Poll;
  BOOST_CHECK_EQUAL(nativeConversationHandoffControlV2ToJson(pollControl).at("contextDigest"),
                    saveValue.at("contextDigest"));
  auto switchControl = saveControl;
  switchControl.trigger = NativeConversationSaveTriggerV2::UserSwitch;
  switchControl.withdrawingProviderIdentity.clear();
  switchControl.withdrawalIntent.reset();
  switchControl.withdrawalProofWire.clear();
  switchControl.control.destinationRoleMapDigest = tagged('d');
  const auto switchValue = nativeConversationHandoffControlV2ToJson(switchControl);
  BOOST_CHECK(switchValue.at("withdrawalBinding").is_null());
  BOOST_CHECK(nativeConversationHandoffControlV2FromJson(
    nativeConversationCanonicalJson(switchValue)).trigger ==
    NativeConversationSaveTriggerV2::UserSwitch);
  BOOST_CHECK(switchValue.at("contextDigest") != saveValue.at("contextDigest"));
  auto badControl = saveControl;
  badControl.trigger = NativeConversationSaveTriggerV2::UserSwitch;
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2ToJson(badControl),
                    std::invalid_argument);
  badControl = saveControl;
  badControl.withdrawalProofWire.clear();
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2ToJson(badControl),
                    std::invalid_argument);
  badControl = saveControl;
  badControl.control.destinationRoleMapDigest = tagged('d');
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2ToJson(badControl),
                    std::invalid_argument);
  badControl = saveControl;
  badControl.sourceProvidersByRole["/Stage/2"] = "/spec212/provider/c";
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2ToJson(badControl),
                    std::invalid_argument);
  badControl = saveControl;
  badControl.withdrawalIntent->localRoles = {"/Stage/0"};
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2ToJson(badControl),
                    std::invalid_argument);
  badControl = saveControl;
  badControl.control.deadlineMs = intent.deadlineMs + 1;
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2ToJson(badControl),
                    std::invalid_argument);
  auto changedSaveValue = saveValue;
  changedSaveValue["contextDigest"] = tagged('e');
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2FromJson(
    nativeConversationCanonicalJson(changedSaveValue)), std::invalid_argument);
  changedSaveValue = saveValue;
  changedSaveValue["withdrawalBinding"]["proofWire"] = "AA==";
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2FromJson(
    nativeConversationCanonicalJson(changedSaveValue)), std::invalid_argument);
  changedSaveValue = saveValue;
  changedSaveValue["sourceProvidersByRole"].push_back(
    changedSaveValue["sourceProvidersByRole"].at(0));
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2FromJson(
    nativeConversationCanonicalJson(changedSaveValue)), std::invalid_argument);
  changedSaveValue = saveValue;
  std::swap(changedSaveValue["sourceProvidersByRole"][0],
            changedSaveValue["sourceProvidersByRole"][1]);
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2FromJson(
    nativeConversationCanonicalJson(changedSaveValue)), std::invalid_argument);
  changedSaveValue = saveValue;
  for (unsigned i = 3; i < 65; ++i) {
    changedSaveValue["sourceProvidersByRole"].push_back(
      NativeJson::array({"/Stage/" + std::to_string(i), "/spec212/provider/b"}));
  }
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2FromJson(
    nativeConversationCanonicalJson(changedSaveValue)), std::invalid_argument);
  auto tooManySources = switchControl;
  tooManySources.sourceProvidersByRole.clear();
  NativeJson tooManySourceRoles = NativeJson::array();
  for (unsigned i = 0; i < 17; ++i) {
    const auto role = "/Stage/" + std::to_string(i);
    const auto source = "/spec212/provider/" + std::to_string(i);
    tooManySources.sourceProvidersByRole.emplace(role, source);
    tooManySourceRoles.push_back(NativeJson::array({role, source}));
  }
  tooManySources.control.sourceRoleMapDigest =
    digest(nativeConversationCanonicalJson(tooManySourceRoles));
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2ToJson(tooManySources),
                    std::invalid_argument);
  auto duplicateKeyWire = saveWire;
  duplicateKeyWire.insert(1, "\"schema\":\"ndnsf-di-handoff-control-v2\",");
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2FromJson(duplicateKeyWire),
                    std::invalid_argument);
  BOOST_CHECK_THROW(nativeConversationHandoffControlV2FromJson(
    std::string(128 * 1024 + 1, ' ')), std::invalid_argument);
  BOOST_CHECK_THROW(nativeValidateWithdrawalIntentProofV1(
    originalWire, provider, intent, intent.deadlineMs, verifier), std::invalid_argument);

  auto changed = intent;
  changed.ownerIdentity = "/spec212/other-user";
  BOOST_CHECK_THROW(nativeValidateWithdrawalIntentProofV1(
    originalWire, provider, changed, intent.deadlineMs - 1, verifier), std::invalid_argument);
  changed = intent;
  changed.checkpointDigest = tagged('d');
  BOOST_CHECK_THROW(nativeValidateWithdrawalIntentProofV1(
    originalWire, provider, changed, intent.deadlineMs - 1, verifier), std::invalid_argument);
  changed = intent;
  changed.localRoles = {"/Stage/1", "/Stage/0"};
  BOOST_CHECK_THROW(nativeWithdrawalIntentV1ToJson(changed), std::invalid_argument);
  changed = intent;
  changed.registrationGeneration = 18;
  BOOST_CHECK_THROW(nativeValidateWithdrawalIntentProofV1(
    originalWire, provider, changed, intent.deadlineMs - 1, verifier), std::invalid_argument);
  BOOST_CHECK_THROW(nativeValidateWithdrawalIntentProofV1(
    originalWire, ndn::Name("/spec212/provider/b"), intent,
    intent.deadlineMs - 1, verifier), std::invalid_argument);
  BOOST_CHECK_THROW(nativeValidateWithdrawalIntentProofV1(
    originalWire, provider, intent, intent.deadlineMs - 1,
    [](const ndn::Data&, const ndn::Name&) { return false; }), std::invalid_argument);
  const ndn::Name foreignSigner("/spec212/provider/foreign");
  const auto foreignCertificate = keyChain.createIdentity(foreignSigner)
                                    .getDefaultKey().getDefaultCertificate();
  const auto foreignProof = nativeSignWithdrawalIntentProofV1(provider, intent,
    [&] (ndn::Data& data) {
      keyChain.sign(data, ndn::security::signingByIdentity(foreignSigner));
    });
  const auto& foreignEncoded = foreignProof.wireEncode();
  const ndn::Buffer foreignWire(foreignEncoded.data(),
                                foreignEncoded.data() + foreignEncoded.size());
  BOOST_CHECK_THROW(nativeValidateWithdrawalIntentProofV1(
    foreignWire, provider, intent, intent.deadlineMs - 1,
    [&foreignCertificate](const ndn::Data& data, const ndn::Name&) {
      return ndn::security::verifySignature(data, foreignCertificate);
    }), std::invalid_argument);
  auto foreignControl = saveControl;
  foreignControl.withdrawalProofWire = foreignWire;
  const auto parsedForeignControl = nativeConversationHandoffControlV2FromJson(
    nativeConversationCanonicalJson(nativeConversationHandoffControlV2ToJson(foreignControl)));
  BOOST_CHECK_THROW(nativeValidateWithdrawalIntentProofV1(
    parsedForeignControl.withdrawalProofWire, provider,
    *parsedForeignControl.withdrawalIntent, intent.deadlineMs - 1, verifier),
    std::invalid_argument);
  BOOST_CHECK_THROW(nativeSignWithdrawalIntentProofV1(provider, intent, {}),
                    std::invalid_argument);
  auto changedValue = intentValue;
  changedValue["extra"] = true;
  BOOST_CHECK_THROW(nativeWithdrawalIntentV1FromJson(
    nativeConversationCanonicalJson(changedValue)), std::invalid_argument);
  BOOST_CHECK_THROW(nativeWithdrawalIntentV1FromJson(intentWire + "\n"),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Spec204RecoveryReadyHasFrozenUnicodeCommitmentAndExactTypes)
{
  NativeConversationStateReadyV2 ready;
  const auto tagged = [](char c) { return "sha256:" + std::string(64, c); };
  ready.requestId = "/request/1"; ready.attemptEpoch = 1;
  ready.generationId = "生成"; ready.planDigest = tagged('1');
  ready.parent.conversationId = "会话"; ready.parent.contextEpoch = 2;
  ready.parent.serviceName = "/LLM/Generate"; ready.parent.planRoleMapDigest = tagged('2');
  ready.parent.checkpointDigest = tagged('3'); ready.parent.roleName = "/Stage/0";
  ready.parent.roleReceiptDigest = tagged('4'); ready.parent.expiresAtMs = 10'000;
  ready.providerIdentity = "/provider/B"; ready.providerBootId = "boot-b";
  ready.cacheEpoch = 7; ready.reason = "repo-import";
  ready.originProviderIdentity = "/provider/A"; ready.protectedKvWireDigest = tagged('5');
  const auto encoded = nativeConversationStateReadyV2ToJson(ready);
  BOOST_REQUIRE_EQUAL(encoded.size(), 22U);
  // Independently frozen UTF-8 bytes and SHA256; not recomputed with the
  // production serializer as the expected-value oracle.
  const std::string unsignedWire = R"JSON({"attemptEpoch":1,"cacheEpoch":7,"checkpointDigest":"sha256:3333333333333333333333333333333333333333333333333333333333333333","contextEpoch":2,"conversationId":"会话","expiresAtMs":10000,"generationId":"生成","originProviderIdentity":"/provider/A","planDigest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","planRoleMapDigest":"sha256:2222222222222222222222222222222222222222222222222222222222222222","protectedKvWireDigest":"sha256:5555555555555555555555555555555555555555555555555555555555555555","providerBootId":"boot-b","providerIdentity":"/provider/B","ready":true,"reason":"repo-import","requestId":"/request/1","residency":"GPU_RESIDENT","roleName":"/Stage/0","roleReceiptDigest":"sha256:4444444444444444444444444444444444444444444444444444444444444444","schema":"ndnsf-di-conversation-state-ready-v2","serviceName":"/LLM/Generate"})JSON";
  auto unsignedValue = encoded; unsignedValue.erase("readyDigest");
  BOOST_CHECK_EQUAL(nativeConversationCanonicalJson(unsignedValue), unsignedWire);
  const std::string expectedDigest = "sha256:e129d08964be3a57e829c03496e8af3d8cc4423ea8162201f7a80090b2ff082f";
  BOOST_CHECK_EQUAL(encoded.at("readyDigest").get<std::string>(), expectedDigest);
  ready = nativeConversationStateReadyV2FromJson(nativeConversationCanonicalJson(encoded));
  BOOST_CHECK_EQUAL(ready.generationId, "生成");
  BOOST_CHECK_EQUAL(nativeConversationRecoveryReadySetDigest({{"/Stage/0", ready}}),
    "sha256:5a1ce7b4bda183bb41af13d81541727a46fa953b1d4c796ef697a56eacb154d2");
  BOOST_CHECK_THROW(nativeConversationRecoveryReadySetDigest({}), std::invalid_argument);
  BOOST_CHECK_THROW(nativeConversationRecoveryReadySetDigest({{"/Stage/1", ready}}), std::invalid_argument);
  const auto rejects = [](const NativeJson& value) {
    BOOST_CHECK_THROW(nativeConversationStateReadyV2FromJson(nativeConversationCanonicalJson(value)),
                      std::invalid_argument);
  };
  auto changed = encoded; changed["ready"] = "true"; rejects(changed);
  changed = encoded; changed["ready"] = false; rejects(changed);
  changed = encoded; changed["cacheEpoch"] = -1; rejects(changed);
  changed = encoded; changed["attemptEpoch"] = 1.5; rejects(changed);
  changed = encoded; changed["extra"] = 1; rejects(changed);
  changed = encoded; changed.erase("originProviderIdentity"); rejects(changed);
  changed = encoded; changed["providerBootId"] = "other-boot"; rejects(changed);
  changed = encoded; changed["protectedKvWireDigest"] = tagged('6'); rejects(changed);
  auto duplicate = nativeConversationCanonicalJson(encoded);
  duplicate.insert(1, "\"ready\":true,");
  BOOST_CHECK_THROW(nativeConversationStateReadyV2FromJson(duplicate), std::invalid_argument);
  BOOST_CHECK_THROW(nativeConversationStateReadyV2FromJson(std::string(32 * 1024 + 1, ' ')),
                    std::invalid_argument);
  auto tooLong = ready; tooLong.readyDigest.clear(); tooLong.providerBootId.assign(513, 'b');
  BOOST_CHECK_THROW(nativeConversationStateReadyV2ToJson(tooLong), std::invalid_argument);
  ndn::Name typedProducer("/provider/B");
  typedProducer.appendVersion(7);
  for (const auto format : {ndn::name::UriFormat::CANONICAL, ndn::name::UriFormat::ALTERNATE}) {
    auto spelled = ready;
    spelled.readyDigest.clear();
    spelled.providerIdentity = typedProducer.toUri(format);
    spelled.originProviderIdentity = ndn::Name("/provider/A").toUri(format);
    spelled.parent.serviceName = ndn::Name("/LLM/Generate").toUri(format);
    spelled.parent.roleName = ndn::Name("/Stage/0").toUri(format);
    const auto parsed = nativeConversationStateReadyV2FromJson(
      nativeConversationCanonicalJson(nativeConversationStateReadyV2ToJson(spelled)));
    BOOST_CHECK_EQUAL(parsed.providerIdentity, spelled.providerIdentity);
    BOOST_CHECK(ndn::Name(parsed.providerIdentity) == typedProducer);
  }
}

BOOST_AUTO_TEST_CASE(Spec204RecoveryActivationBindsOwnReadyAndShorterDeadline)
{
  const auto tagged = [](char c) { return "sha256:" + std::string(64, c); };
  NativeConversationStateReadyV2 ready;
  ready.requestId = "/request/1"; ready.attemptEpoch = 1;
  ready.generationId = "generation"; ready.planDigest = tagged('1');
  ready.parent = {"conversation", 2, "/LLM/Generate", tagged('2'), tagged('3'),
                  "/Stage/0", tagged('4'), 10'000};
  ready.providerIdentity = "/provider/B"; ready.providerBootId = "boot-b";
  ready.cacheEpoch = 7; ready.reason = "state-hit";
  ready.originProviderIdentity = "/provider/A"; ready.protectedKvWireDigest = tagged('5');
  ready = nativeConversationStateReadyV2FromJson(
    nativeConversationCanonicalJson(nativeConversationStateReadyV2ToJson(ready)));
  NativeConversationRecoveryActivationV1 activation;
  activation.requestId = ready.requestId; activation.attemptEpoch = ready.attemptEpoch;
  activation.generationId = ready.generationId; activation.planDigest = ready.planDigest;
  activation.conversationId = ready.parent.conversationId; activation.contextEpoch = ready.parent.contextEpoch;
  activation.serviceName = ready.parent.serviceName; activation.planRoleMapDigest = ready.parent.planRoleMapDigest;
  activation.checkpointDigest = ready.parent.checkpointDigest; activation.roleName = ready.parent.roleName;
  activation.readyDigest = ready.readyDigest;
  activation.readySetDigest = nativeConversationRecoveryReadySetDigest({{ready.parent.roleName, ready}});
  activation.expiresAtMs = 9'000;
  const auto value = nativeConversationRecoveryActivationV1ToJson(activation);
  BOOST_REQUIRE_EQUAL(value.size(), 14U);
  const auto parsed = nativeConversationRecoveryActivationV1FromJson(nativeConversationCanonicalJson(value));
  BOOST_CHECK_NO_THROW(nativeValidateConversationRecoveryActivationV1(parsed, ready, 8'999));
  BOOST_CHECK_THROW(nativeValidateConversationRecoveryActivationV1(parsed, ready, 9'000), std::invalid_argument);
  auto changed = activation; changed.expiresAtMs = 10'001;
  BOOST_CHECK_THROW(nativeValidateConversationRecoveryActivationV1(changed, ready, 100), std::invalid_argument);
  changed = activation; changed.readyDigest = tagged('6');
  BOOST_CHECK_THROW(nativeValidateConversationRecoveryActivationV1(changed, ready, 100), std::invalid_argument);
  changed = activation; changed.attemptEpoch = 2;
  BOOST_CHECK_THROW(nativeValidateConversationRecoveryActivationV1(changed, ready, 100), std::invalid_argument);
  auto changedWire = value; changedWire["extra"] = true;
  BOOST_CHECK_THROW(nativeConversationRecoveryActivationV1FromJson(nativeConversationCanonicalJson(changedWire)),
                    std::invalid_argument);
  BOOST_CHECK_THROW(nativeConversationRecoveryActivationV1FromJson(std::string(16 * 1024 + 1, ' ')),
                    std::invalid_argument);
  changed = activation; changed.requestId.assign(2049, 'x');
  BOOST_CHECK_THROW(nativeConversationRecoveryActivationV1ToJson(changed), std::invalid_argument);
  changed = activation; changed.conversationId.assign(1024, '\1');
  changed.requestId.assign(2048, '\1'); changed.generationId.assign(512, '\1');
  BOOST_CHECK_THROW(nativeConversationRecoveryActivationV1ToJson(changed), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Spec204RecoveryCancelHasExactBoundedWireShape)
{
  const auto tagged = [](char c) { return "sha256:" + std::string(64, c); };
  NativeConversationRecoveryCancelV1 cancel;
  cancel.requestId = "/request/1"; cancel.attemptEpoch = 2;
  cancel.generationId = "generation"; cancel.planDigest = tagged('1');
  cancel.parent = {"conversation", 3, "/LLM/Generate", tagged('2'), tagged('3'),
                   "/Stage/0", tagged('4'), 12'000};
  cancel.providerIdentity = "/provider/B"; cancel.providerBootId = "boot-b";
  auto projection = NativeSelectionProjectionV3{};
  projection.provider = cancel.providerIdentity;
  projection.requestId = cancel.requestId;
  projection.attempt = cancel.attemptEpoch;
  projection.planDigest = cancel.planDigest;
  projection.deadlineMs = 11'000;
  projection.generationContract.enabled = true;
  projection.generationContract.generationId = cancel.generationId;
  projection.conversationStateReference = cancel.parent;
  projection.conversationTurnBinding.emplace();
  projection.conversationTurnBinding->retentionDeadlineMs = 9'500;
  projection.protectedKvRestore.emplace();
  projection.protectedKvRestore->recipient.expiresAtMs = 10'000;
  cancel.parent.expiresAtMs = 9'000;
  BOOST_CHECK_NO_THROW(nativeValidateConversationRecoveryCancelV1(cancel, projection, "boot-b", 8'000));
  const auto checkRejected = [&](const NativeConversationRecoveryCancelV1& wrong) {
    BOOST_CHECK_THROW(nativeValidateConversationRecoveryCancelV1(wrong, projection, "boot-b", 8'000),
                      std::invalid_argument);
  };
  auto wrong = cancel; wrong.requestId += "/other"; checkRejected(wrong);
  wrong = cancel; ++wrong.attemptEpoch; checkRejected(wrong);
  wrong = cancel; wrong.generationId += "-other"; checkRejected(wrong);
  wrong = cancel; wrong.planDigest = tagged('5'); checkRejected(wrong);
  wrong = cancel; wrong.parent.conversationId += "-other"; checkRejected(wrong);
  wrong = cancel; ++wrong.parent.contextEpoch; checkRejected(wrong);
  wrong = cancel; wrong.parent.serviceName = "/LLM/Other"; checkRejected(wrong);
  wrong = cancel; wrong.parent.planRoleMapDigest = tagged('5'); checkRejected(wrong);
  wrong = cancel; wrong.parent.checkpointDigest = tagged('6'); checkRejected(wrong);
  wrong = cancel; wrong.parent.roleName = "/Stage/Other"; checkRejected(wrong);
  wrong = cancel; wrong.parent.roleReceiptDigest = tagged('5'); checkRejected(wrong);
  wrong = cancel; wrong.providerIdentity = "/provider/C"; checkRejected(wrong);
  wrong = cancel; wrong.providerBootId = "old-boot"; checkRejected(wrong);
  wrong = cancel; wrong.parent.expiresAtMs = 9'501; checkRejected(wrong);
  wrong = cancel; wrong.parent.expiresAtMs = 7'999; checkRejected(wrong);
  const auto value = nativeConversationRecoveryCancelV1ToJson(cancel);
  BOOST_REQUIRE_EQUAL(value.size(), 15U);
  const auto wire = nativeConversationCanonicalJson(value);
  const auto decoded = nativeConversationRecoveryCancelV1FromJson(wire);
  BOOST_CHECK_EQUAL(decoded.requestId, cancel.requestId);
  BOOST_CHECK_EQUAL(decoded.attemptEpoch, cancel.attemptEpoch);
  BOOST_CHECK_EQUAL(decoded.parent.roleReceiptDigest, cancel.parent.roleReceiptDigest);
  BOOST_CHECK_EQUAL(decoded.providerBootId, cancel.providerBootId);
  auto changed = value; changed["extra"] = true;
  BOOST_CHECK_THROW(nativeConversationRecoveryCancelV1FromJson(
    nativeConversationCanonicalJson(changed)), std::invalid_argument);
  changed = value; changed["attemptEpoch"] = 2.5;
  BOOST_CHECK_THROW(nativeConversationRecoveryCancelV1FromJson(
    nativeConversationCanonicalJson(changed)), std::invalid_argument);
  BOOST_CHECK_THROW(nativeConversationRecoveryCancelV1FromJson(std::string(16 * 1024 + 1, ' ')),
                    std::invalid_argument);
  cancel.providerBootId.assign(513, 'b');
  BOOST_CHECK_THROW(nativeConversationRecoveryCancelV1ToJson(cancel), std::invalid_argument);
}

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
  const std::function<void(NativeEpochCoordinatorConfig&)>& configure = [](auto&) {},
  std::size_t rowsPerEpoch = 1)
{
  const auto result = test::runSamplingEpochs(std::move(logits), configure, rowsPerEpoch);
  if (!result.finalPayload) throw std::runtime_error("sampling epoch did not finish");
  return nativeParseJson(std::string(result.finalPayload->begin(), result.finalPayload->end()))
    .at("tokenIds").get<std::vector<std::int64_t>>();
}
}

BOOST_AUTO_TEST_SUITE(Spec182Sampling)
BOOST_AUTO_TEST_CASE(LargePrefillSamplesOnlyTheFinalRow)
{
  constexpr std::size_t vocabulary = 4096;
  constexpr std::size_t rows = 128;
  std::vector<float> logits(vocabulary * rows, -1.0f);
  logits[0] = 1000.0f;
  logits[(rows - 1) * vocabulary + 17] = 2.0f;
  BOOST_CHECK(sampled({std::move(logits)}, [](auto& c) {
    c.samplingTopK = 1;
  }, rows) == std::vector<std::int64_t>{17});
}
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
