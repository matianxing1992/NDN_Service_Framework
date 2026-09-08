#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <boost/test/unit_test.hpp>

namespace Spec182V3Placement { void runPublicClientScenario(int scenario); }

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
