#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"

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

BOOST_AUTO_TEST_CASE(NativeConversation_commits_atomically_and_restores)
{
  const auto root = std::filesystem::temp_directory_path() /
    ("ndnsf-spec182-conversation-" + std::to_string(::getpid()));
  NativeConversationCoordinator coordinator(root);
  NativeConversationContinuation continuation{
    "conversation", 1, "/service", digest("roles"), digest("parent"),
    digest("request-contract"), 4'000'000'000ULL};
  // Seed a record through the same public path used by the requester.
  NativeConversationTurn seed{continuation, "/request/0", 1, 2, {}, {}, false};
  NativeCompletedAttempt completed{"/request/0", 1, {10, 11}, digest("state"), true};
  auto checkpoint = coordinator.prepareCheckpoint(seed, completed);
  coordinator.commitTurn(seed, checkpoint);
  const auto restored = coordinator.find("conversation");
  BOOST_REQUIRE(restored);
  BOOST_CHECK_EQUAL(restored->checkpoint.successorContextEpoch, 2);

  NativeConversationCoordinator recovered(root);
  recovered.restore(root);
  const auto next = recovered.beginTurn(
    NativeConversationContinuation{"conversation", 2, "/service", digest("roles"),
                                  restored->checkpoint.checkpointDigest,
                                  digest("request-contract"), 4'000'000'000ULL},
    "/request/1", 1);
  BOOST_CHECK_EQUAL(next.successorContextEpoch, 3);
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

BOOST_AUTO_TEST_CASE(NativeConversation_rejects_wrong_parent)
{
  NativeConversationCoordinator coordinator(
    std::filesystem::temp_directory_path() / "ndnsf-spec182-no-journal");
  BOOST_CHECK_THROW(coordinator.beginTurn(
    NativeConversationContinuation{"conversation", 1, "/service", digest("roles"),
                                  digest("parent"), digest("request"), 4'000'000'000ULL},
    "/request", 1), std::runtime_error);
}

} // namespace ndnsf::di
