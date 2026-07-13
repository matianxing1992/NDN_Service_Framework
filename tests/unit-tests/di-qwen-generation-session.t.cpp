#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/QwenGenerationSession.hpp"

#include <stdexcept>
#include <string>

namespace ndnsf::di::test {
namespace {

std::string
digest(char value)
{
  return "sha256:" + std::string(64, value);
}

QwenGenerationSessionSpec
validSpec()
{
  QwenGenerationSessionSpec spec;
  spec.candidateId =
    "spec107-c1-111111111111-222222222222-333333333333-"
    "444444444444-555555555555-666666666666";
  spec.planDigest = digest('1');
  spec.modelDigest = digest('2');
  spec.artifactDigest = digest('3');
  spec.logicalSessionId = "generation-1";
  spec.requestId = "request-1";
  spec.serviceName = "/AI/LLM/Pipeline/Qwen";
  spec.attemptEpoch = 0;
  spec.tokenEpoch = 0;
  spec.inputTokenCount = 32;
  spec.maxGeneratedTokens = 2;
  spec.deadlineEpochMs = 10'000;
  spec.contextReference = "/repo/qwen/context/sha256-abc";
  spec.feedbackTopic = "/AI/LLM/Pipeline/Qwen/feedback/generation-1";
  spec.roles = {
    {"/LLM/Stage/0", "/provider/0", "boot-0"},
    {"/LLM/Stage/1", "/provider/1", "boot-1"},
    {"/LLM/Stage/2", "/provider/2", "boot-2"},
  };
  return spec;
}

} // namespace

BOOST_AUTO_TEST_SUITE(DiQwenGenerationSession)

BOOST_AUTO_TEST_CASE(SpecCodecRoundTripPreservesIdentityBindings)
{
  const auto spec = validSpec();
  const auto json = qwenGenerationSessionSpecToJson(spec);
  BOOST_CHECK(json.find("prompt") == std::string::npos);
  BOOST_CHECK(json.find("payload") == std::string::npos);
  const auto decoded = qwenGenerationSessionSpecFromJson(json);
  BOOST_CHECK_EQUAL(decoded.candidateId, spec.candidateId);
  BOOST_CHECK_EQUAL(decoded.planDigest, spec.planDigest);
  BOOST_CHECK_EQUAL(decoded.modelDigest, spec.modelDigest);
  BOOST_CHECK_EQUAL(decoded.artifactDigest, spec.artifactDigest);
  BOOST_CHECK_EQUAL(decoded.logicalSessionId, spec.logicalSessionId);
  BOOST_CHECK_EQUAL(decoded.requestId, spec.requestId);
  BOOST_CHECK_EQUAL(decoded.serviceName, spec.serviceName);
  BOOST_CHECK_EQUAL(decoded.attemptEpoch, 0);
  BOOST_CHECK_EQUAL(decoded.tokenEpoch, 0);
  BOOST_CHECK_EQUAL(decoded.inputTokenCount, 32);
  BOOST_CHECK_EQUAL(decoded.maxGeneratedTokens, 2);
  BOOST_CHECK_EQUAL(decoded.deadlineEpochMs, 10'000);
  BOOST_REQUIRE_EQUAL(decoded.roles.size(), 3);
  BOOST_CHECK_EQUAL(decoded.roles[2].role, "/LLM/Stage/2");
  BOOST_CHECK_EQUAL(decoded.roles[2].provider, "/provider/2");
  BOOST_CHECK_EQUAL(decoded.roles[2].providerBootId, "boot-2");
}

BOOST_AUTO_TEST_CASE(SpecValidationRejectsUnboundOrUnboundedValues)
{
  auto checkInvalid = [] (QwenGenerationSessionSpec spec) {
    BOOST_CHECK_THROW(spec.validate(), std::invalid_argument);
  };
  auto spec = validSpec();
  spec.candidateId = "spec105-local-minindn-candidate-r2";
  checkInvalid(spec);
  spec = validSpec();
  spec.planDigest = "sha256:short";
  checkInvalid(spec);
  spec = validSpec();
  spec.inputTokenCount = 0;
  checkInvalid(spec);
  spec = validSpec();
  spec.inputTokenCount = 513;
  checkInvalid(spec);
  spec = validSpec();
  spec.maxGeneratedTokens = 0;
  checkInvalid(spec);
  spec = validSpec();
  spec.maxGeneratedTokens = 33;
  checkInvalid(spec);
  spec = validSpec();
  spec.attemptEpoch = 2;
  checkInvalid(spec);
  spec = validSpec();
  spec.tokenEpoch = spec.maxGeneratedTokens;
  checkInvalid(spec);
  spec = validSpec();
  spec.roles.pop_back();
  checkInvalid(spec);
  spec = validSpec();
  spec.roles[1].role = "/LLM/Stage/0";
  checkInvalid(spec);
  spec = validSpec();
  spec.roles[1].provider = spec.roles[0].provider;
  checkInvalid(spec);
}

BOOST_AUTO_TEST_CASE(CandidateIdentityAcceptsOnlyVersionedSpec107AndSpec110)
{
  auto spec107 = validSpec();
  BOOST_CHECK_NO_THROW(spec107.validate());

  auto spec110 = validSpec();
  spec110.candidateId =
    "spec110-c1-111111111111-222222222222-333333333333-"
    "444444444444-555555555555-666666666666";
  BOOST_CHECK_NO_THROW(spec110.validate());

  for (const auto& forbidden : {
         "spec105-c1-111111111111-222222222222-333333333333-"
         "444444444444-555555555555-666666666666",
         "spec109-c1-111111111111-222222222222-333333333333-"
         "444444444444-555555555555-666666666666",
       }) {
    auto invalid = validSpec();
    invalid.candidateId = forbidden;
    BOOST_CHECK_THROW(invalid.validate(), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(CodecRejectsUnknownSchemaSecretsAndMalformedJson)
{
  BOOST_CHECK_THROW(qwenGenerationSessionSpecFromJson("{}"), std::invalid_argument);
  BOOST_CHECK_THROW(qwenGenerationSessionSpecFromJson(
    "{\"schema\":\"ndnsf-di-qwen-generation-session-v2\"}"),
    std::invalid_argument);
  BOOST_CHECK_THROW(qwenGenerationSessionSpecFromJson(
    "{\"schema\":\"ndnsf-di-qwen-generation-session-v1\","
    "\"prompt\":\"secret\"}"),
    std::invalid_argument);
  BOOST_CHECK_THROW(qwenGenerationSessionSpecFromJson("not-json"),
                    std::exception);
}

BOOST_AUTO_TEST_CASE(StateMachineAllowsOneBoundedReplacementAndOneTerminal)
{
  QwenGenerationSessionStateMachine state(validSpec());
  BOOST_CHECK(state.state() == QwenGenerationState::Created);
  state.beginSelection();
  BOOST_CHECK(state.state() == QwenGenerationState::Selecting);
  state.activate();
  BOOST_CHECK(state.state() == QwenGenerationState::Active);
  BOOST_CHECK_EQUAL(state.completeTokenEpoch(), 1);
  BOOST_CHECK_EQUAL(state.generatedTokenCount(), 1);
  state.beginReplacement();
  BOOST_CHECK(state.state() == QwenGenerationState::Rebuilding);
  BOOST_CHECK_EQUAL(state.attemptEpoch(), 1);
  state.activate();
  BOOST_CHECK_EQUAL(state.completeTokenEpoch(), 2);
  state.complete();
  BOOST_CHECK(state.state() == QwenGenerationState::Completed);
  BOOST_CHECK(state.isTerminal());
  BOOST_CHECK_THROW(state.complete(), std::logic_error);
  BOOST_CHECK_THROW(state.beginReplacement(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(StateMachineRejectsInvalidTransitionsAndSecondReplacement)
{
  QwenGenerationSessionStateMachine state(validSpec());
  BOOST_CHECK_THROW(state.activate(), std::logic_error);
  BOOST_CHECK_THROW(state.completeTokenEpoch(), std::logic_error);
  state.beginSelection();
  state.activate();
  state.beginReplacement();
  state.activate();
  BOOST_CHECK_THROW(state.beginReplacement(), std::logic_error);
  state.terminate(QwenGenerationTerminal::NoCompatibleReplacement);
  BOOST_CHECK(state.state() == QwenGenerationState::Terminal);
  BOOST_CHECK(state.terminalReason() ==
              QwenGenerationTerminal::NoCompatibleReplacement);
  BOOST_CHECK_THROW(state.cancel(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(CompletionRequiresExactBoundAndCancellationIsTerminal)
{
  QwenGenerationSessionStateMachine incomplete(validSpec());
  incomplete.beginSelection();
  incomplete.activate();
  BOOST_CHECK_THROW(incomplete.complete(), std::logic_error);
  incomplete.cancel();
  BOOST_CHECK(incomplete.state() == QwenGenerationState::Cancelled);
  BOOST_CHECK(incomplete.isTerminal());

  QwenGenerationSessionStateMachine overflow(validSpec());
  overflow.beginSelection();
  overflow.activate();
  overflow.completeTokenEpoch();
  overflow.completeTokenEpoch();
  BOOST_CHECK_THROW(overflow.completeTokenEpoch(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(DeadlineExpiresSessionExactlyAtBound)
{
  auto spec = validSpec();
  spec.deadlineEpochMs = 10'000;
  QwenGenerationSessionStateMachine state(spec);
  BOOST_CHECK(!state.expireIfDeadlineReached(9'999));
  BOOST_CHECK(state.state() == QwenGenerationState::Created);
  BOOST_CHECK(state.expireIfDeadlineReached(10'000));
  BOOST_CHECK(state.state() == QwenGenerationState::Terminal);
  BOOST_CHECK(state.terminalReason() == QwenGenerationTerminal::RequestDeadline);
  BOOST_CHECK(!state.expireIfDeadlineReached(10'001));
}

BOOST_AUTO_TEST_CASE(StaleAttemptCannotAdvanceTokenEpochAfterReplacement)
{
  QwenGenerationSessionStateMachine state(validSpec());
  state.beginSelection();
  state.activate();
  BOOST_CHECK_EQUAL(state.completeTokenEpoch(0), 1);
  state.beginReplacement();
  state.activate();
  BOOST_CHECK_THROW(state.completeTokenEpoch(0), std::logic_error);
  BOOST_CHECK_EQUAL(state.generatedTokenCount(), 1);
  BOOST_CHECK_EQUAL(state.completeTokenEpoch(1), 2);
}

BOOST_AUTO_TEST_CASE(TerminalResponseCanBeClaimedExactlyOnce)
{
  QwenGenerationSessionStateMachine state(validSpec());
  BOOST_CHECK_THROW(state.claimTerminalResponse(), std::logic_error);
  state.beginSelection();
  state.activate();
  state.completeTokenEpoch();
  state.completeTokenEpoch();
  state.complete();
  BOOST_CHECK(state.claimTerminalResponse());
  BOOST_CHECK(!state.claimTerminalResponse());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::test
