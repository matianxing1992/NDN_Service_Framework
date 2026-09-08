#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <boost/test/unit_test.hpp>
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.hpp"
#include "tests/fixtures/spec182/native-sampling-epoch.hpp"
#include <fstream>
#include <unistd.h>

namespace Spec182V3Placement { void runPublicClientScenario(int scenario); }

namespace {
struct EpochTokenizer
{
  EpochTokenizer()
  {
    std::ifstream input("tests/fixtures/spec182/dependency-probes/tokenizer/stable-vectors.json");
    NativeFixtureJsonRead(input);
  }
  void NativeFixtureJsonRead(std::istream& input)
  {
    const auto fixtures = ndnsf::di::NativeJson::parse(input);
    for (const auto& fixture : fixtures.at("fixtures")) {
      if (fixture.at("name") != "byte-fallback-special") continue;
      char path[] = "/tmp/spec182-epoch-tokenizer-XXXXXX";
      const auto fd = ::mkstemp(path);
      if (fd < 0) throw std::runtime_error("cannot create tokenizer fixture");
      ::close(fd);
      try {
        {
          std::ofstream output(path, std::ios::binary);
          output.exceptions(std::ios::failbit | std::ios::badbit);
          output << fixture.at("tokenizerJson").get<std::string>();
        }
        tokenizer = std::make_unique<ndnsf::di::qwen::NativeTokenizer>(
          path, "sha256:" + fixture.at("sha256").get<std::string>());
      }
      catch (...) { ::unlink(path); throw; }
      ::unlink(path);
      return;
    }
    throw std::runtime_error("missing frozen ByteFallback fixture");
  }
  std::unique_ptr<ndnsf::di::qwen::NativeTokenizer> tokenizer;
};

std::vector<std::vector<float>> epochLogits(std::initializer_list<std::size_t> ids)
{
  std::vector<std::vector<float>> result;
  for (const auto id : ids) {
    result.emplace_back(261, -100.0f);
    result.back().at(id) = 100.0f;
  }
  return result;
}
}

BOOST_AUTO_TEST_SUITE(Spec182EpochText)
BOOST_AUTO_TEST_CASE(TerminalFlushMatchesFinalForMaxEosStopAndReplay)
{
  using namespace ndnsf::di;
  EpochTokenizer fixture;
  for (int scenario = 0; scenario != 4; ++scenario) {
    std::vector<NativeJson> events;
    const auto result = test::runSamplingEpochs(
      scenario == 1 ? epochLogits({102, 2}) : epochLogits({102, 260}),
      [&](NativeEpochCoordinatorConfig& config) {
        config.requireTextOutput = true;
        config.textDecoder = [&](const auto& ids) { return fixture.tokenizer->decode(ids); };
        config.stableTextDecoder = [&](const auto& ids, bool final) {
          return fixture.tokenizer->decodeStable(ids, true, final);
        };
        if (scenario == 1) config.eosTokenIds = {2};
        if (scenario == 2) config.stopStrings = {"a"};
        if (scenario == 3) {
          config.attemptEpoch = 2;
          config.committedPrefixTokenIds = {102};
        }
        config.eventSink = [&](const auto& bytes) {
          events.push_back(NativeJson::parse(bytes));
          return true;
        };
      });
    BOOST_REQUIRE(result.finalPayload.has_value());
    const auto final = NativeJson::parse(*result.finalPayload);
    BOOST_REQUIRE_EQUAL(events.size(), scenario >= 2 ? 1 : 2);
    std::string joined;
    for (const auto& event : events) joined += event.at("textDelta").get<std::string>();
    const std::string expected = scenario == 1 || scenario == 2 ? "a" : "\xef\xbf\xbd\xef\xbf\xbd";
    BOOST_CHECK_EQUAL(joined, expected);
    BOOST_CHECK_EQUAL(final.at("text").get<std::string>(), expected);
    BOOST_CHECK_EQUAL(events.back().at("finishHint").get<std::string>(),
      scenario == 1 ? "EOS" : scenario == 2 ? "STOP_SEQUENCE" : "MAX_TOKENS");
    BOOST_CHECK_EQUAL(result.prefixTokensRecomputed, scenario == 3 ? 1 : 0);
    if (events.size() == 2) BOOST_CHECK_EQUAL(events.front().at("textDelta"), "");
  }
}

BOOST_AUTO_TEST_CASE(FinalDecoderMismatchRejectsBeforeEventAcceptance)
{
  using namespace ndnsf::di;
  std::size_t accepted = 0;
  BOOST_CHECK_EXCEPTION(test::runSamplingEpochs(epochLogits({102}),
    [&](NativeEpochCoordinatorConfig& config) {
      config.textDecoder = [](const auto&) { return "a"; };
      config.stableTextDecoder = [](const auto&, bool final) { return final ? "wrong" : ""; };
      config.eventSink = [&](const auto&) { ++accepted; return true; };
    }), std::runtime_error, [](const auto& error) {
      return std::string(error.what()) == "NATIVE_FINAL_TEXT_DECODE_MISMATCH";
    });
  BOOST_CHECK_EQUAL(accepted, 0);
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(Spec182StreamAcceptance)
BOOST_AUTO_TEST_CASE(Spec182StreamAcceptsCompleteTranscript)
{ Spec182V3Placement::runPublicClientScenario(3); }
BOOST_AUTO_TEST_CASE(Spec182StreamRejectBeforeAcceptKeepsPrefix)
{
  for (const auto scenario : {4, 10, 11, 12, 13})
    Spec182V3Placement::runPublicClientScenario(scenario);
}
BOOST_AUTO_TEST_CASE(Spec182StreamTerminalTokenNoReplacement)
{ Spec182V3Placement::runPublicClientScenario(5); }
BOOST_AUTO_TEST_CASE(Spec182StreamFinalTextMismatch)
{ Spec182V3Placement::runPublicClientScenario(6); }
BOOST_AUTO_TEST_CASE(Spec182StreamCallbackFailureDoesNotReplay)
{ Spec182V3Placement::runPublicClientScenario(7); }
BOOST_AUTO_TEST_CASE(Spec182StreamProviderCommitFailureRecomputesAcceptedPrefix)
{ Spec182V3Placement::runPublicClientScenario(8); }
BOOST_AUTO_TEST_CASE(Spec182StreamUnreceivedTokenExcludedFromReplacement)
{ Spec182V3Placement::runPublicClientScenario(9); }
BOOST_AUTO_TEST_SUITE_END()

namespace {
using namespace ndnsf::di;
const std::string generationId(32, '1');
NativeJson generationOptions()
{
  return {{"useCache", true}, {"outputMode", "TOKEN_STREAMING"},
    {"maxNewTokens", 8}, {"eosTokenIds", {2}},
    {"tokenizerDigest", "sha256:" + std::string(64, 'a')}};
}
NativeGenerationExecutionContractV1 parseGeneration(const NativeJson& value)
{
  const auto wire = nativeCanonicalJson(value);
  return nativeGenerationFromOptions({wire.begin(), wire.end()}, generationId);
}
}

BOOST_AUTO_TEST_SUITE(Spec182GenerationOptions)
BOOST_AUTO_TEST_CASE(GenerationDerivesSamplingFromBoundOptions)
{
  const auto result = parseGeneration(generationOptions());
  BOOST_CHECK_EQUAL(result.maxGeneratedTokens, 8);
  BOOST_CHECK_EQUAL(result.generationId, generationId);
  BOOST_CHECK_EQUAL(result.tokenInputName, "input_ids");
  BOOST_CHECK_EQUAL(result.samplingMode, "Greedy");
  // Independent Python json.dumps(sort_keys=True,separators=(',',':')) /
  // hashlib.sha256 reference, including double defaults 0.0 and 1.0.
  BOOST_CHECK_EQUAL(result.samplingDigest,
    "sha256:f924aa62a0ced09ee7e05e43971f95cdae88f7cc116b2dd9bf6ca8d6661d224b");
  auto options = generationOptions();
  options["sampling"] = {{"mode", "top_k_top_p"}, {"temperature", 1.0},
    {"top_k", 4}, {"top_p", .8}, {"repetition_penalty", 1.2}, {"seed", 0}};
  const auto random = parseGeneration(options);
  BOOST_CHECK_EQUAL(random.samplingMode, "SeededTopKTopP");
  BOOST_CHECK_EQUAL(random.samplingTopK, 4);
  BOOST_CHECK_EQUAL(random.samplingSeed, 0);
  BOOST_CHECK_NE(random.samplingDigest, result.samplingDigest);
}

BOOST_AUTO_TEST_CASE(GenerationRejectsUnboundOrMalformedOptions)
{
  for (int invalid = 0; invalid < 12; ++invalid) {
    auto value = generationOptions();
    switch (invalid) {
      case 0: value["useCache"] = false; break;
      case 1: value["maxNewTokens"] = true; break;
      case 2: value["maxNewTokens"] = 65; break;
      case 3: value["maxNewTokens"] = 1.5; break;
      case 4: value["eosTokenIds"] = {-1}; break;
      case 5: value["eosTokenIds"] = {true}; break;
      case 6: value["generationId"] = std::string(32, '2'); break;
      case 7: value["sampling"] = {{"topK", 0}}; break;
      case 8: value["sampling"] = {{"temperature", 1.0}}; break;
      case 9: value["sampling"] = {{"repetitionPenalty", .09}}; break;
      case 10: value["sampling"] = {{"seed", -1}}; break;
      case 11: value["sampling"] = {{"mode", "unknown"}}; break;
    }
    BOOST_TEST_CONTEXT("invalid options " << invalid) {
      BOOST_CHECK_THROW(parseGeneration(value), std::exception);
    }
  }
}
BOOST_AUTO_TEST_SUITE_END()
