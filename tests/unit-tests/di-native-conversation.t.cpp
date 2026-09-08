#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "tests/fixtures/spec182/native-sampling-epoch.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
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
