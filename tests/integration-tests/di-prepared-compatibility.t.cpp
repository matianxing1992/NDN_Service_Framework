#include "tests/boost-test.hpp"

#include "ndnsf-di/api.hpp"
#include "ndnsf-di/provider.hpp"

#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <exception>

namespace {

using namespace ndnsf::di;

BOOST_AUTO_TEST_SUITE(Spec185Compatibility)

BOOST_AUTO_TEST_CASE(PublicUmbrellasExposeOwningRequestValues)
{
  const auto input = Input::inlineBytes({0x01, 0x02}, {0x7b, 0x7d});
  (void)input;
  RequestOptions options;
  options.providerNames = {"/spec185/provider/0", "/spec185/provider/1"};
  options.outputMode = "TOKEN_STREAMING";
  options.stream = StreamOptions{true};
  options.generation = GenerationOptions{4};
  BOOST_CHECK_EQUAL(options.providerNames.size(), 2);
  BOOST_CHECK_EQUAL(options.outputMode, "TOKEN_STREAMING");
  BOOST_CHECK(options.stream->enabled);
  BOOST_CHECK_EQUAL(options.generation->maxNewTokens, 4);
}

BOOST_AUTO_TEST_CASE(RuntimeOpenRejectsMissingNativeConfiguration)
{
  RuntimeConfig config;
  config.nativeConfigPath =
    (std::filesystem::temp_directory_path() / "spec185-missing-requester.json").string();
  BOOST_CHECK_EXCEPTION(
    Runtime::open(config), DiError,
    [] (const DiError& error) {
      return error.code() == "INVALID_RUNTIME_CONFIGURATION" &&
             error.boundary() == "configuration";
    });
}

BOOST_AUTO_TEST_CASE(ProviderCliParserRemainsCxxOwned)
{
  const char* values[] = {"provider", "--provider"};
  BOOST_CHECK_THROW(ProviderConfig::fromCommandLine(2, values), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
