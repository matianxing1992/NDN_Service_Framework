#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeStandaloneTokenizer.hpp"

#include <boost/test/unit_test.hpp>

#include <filesystem>

namespace {
const auto FIXTURE = std::filesystem::path(
  "tests/fixtures/spec175/tiny-causal-lm-v1/standalone/tokenizer.json");
}

BOOST_AUTO_TEST_SUITE(Spec182NativeTokenizer)

BOOST_AUTO_TEST_CASE(MissingArtifactIsRejectedBeforeBridgeLookup)
{
  BOOST_CHECK_THROW(
    ndnsf::di::qwen::NativeTokenizer(
      "/tmp/spec182-tokenizer-does-not-exist.json",
      "sha256:" + std::string(64, '0')),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(DigestMismatchIsRejectedBeforeBridgeLookup)
{
  BOOST_REQUIRE(std::filesystem::is_regular_file(FIXTURE));
  BOOST_CHECK_THROW(
    ndnsf::di::qwen::NativeTokenizer(
      FIXTURE.string(), "sha256:" + std::string(64, '0')),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StandaloneFactoryRequiresIdentity)
{
  BOOST_CHECK_THROW(
    ndnsf::di::makeNativeStandaloneTokenizerDecoders(
      ndnsf::di::NativeStandaloneTokenizerOptions{}, {}),
    std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()
