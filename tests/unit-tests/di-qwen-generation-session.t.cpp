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
  spec.attemptEpoch = 1;
  spec.tokenEpoch = 0;
  spec.inputTokenCount = 32;
  spec.maxGeneratedTokens = 2;
  spec.deadlineEpochMs = 10'000;
  spec.contextReference = "/repo/qwen/context/sha256-abc";
  spec.feedbackTopic = "/AI/LLM/Pipeline/Qwen/feedback/generation-1";
  spec.roles = {
    {"/LLM/Pipeline/Stage/0", "/provider/0", "boot-0"},
    {"/LLM/Pipeline/Stage/1", "/provider/1", "boot-1"},
    {"/LLM/Pipeline/Stage/2", "/provider/2", "boot-2"},
  };
  return spec;
}

DecodeStateBundleV1
validDecodeState(std::uint32_t epoch = 0)
{
  DecodeStateIdentityV1 identity;
  identity.modelDigest = digest('1');
  identity.graphSemanticDigest = digest('2');
  identity.artifactDigest = digest('3');
  identity.adapterDigest = digest('4');
  identity.tokenizerDigest = digest('5');
  identity.runnerDigest = digest('6');
  identity.roleName = "/LLM/Pipeline/Stage/0";
  identity.roleSplitDigest = digest('7');
  identity.layerBegin = 0;
  identity.layerEnd = 21;
  identity.prefixDigest = digest('8');
  identity.prefixTokenCount = epoch;
  identity.positionDigest = digest('9');
  identity.precision = "fp16";
  identity.layoutDigest = digest('a');
  identity.stateSchemaDigest = digest('b');
  identity.runtimeAbiDigest = digest('c');
  identity.securityDomainDigest = digest('d');
  identity.providerIdentity = "/provider/0";
  identity.providerBootId = "boot-0";
  identity.requestId = "request-1";
  identity.attemptEpoch = 1;
  identity.generationId = "generation-1";
  identity.stateComponentDigests = {digest('e'), digest('f')};
  DecodeStateBundleV1 bundle;
  bundle.identity = identity;
  bundle.fullAttentionKv = {{"attention_kv", "float16", {1, 2}, digest('e')}};
  bundle.recurrentConvolution = {{"recurrent_state", "float16", {1, 2}, digest('f')}};
  bundle.tokenEpoch = epoch;
  return bundle;
}

} // namespace

BOOST_AUTO_TEST_SUITE(DiQwenGenerationSession)

BOOST_AUTO_TEST_CASE(DecodeStateRequiresBothStateFamiliesAndExactIdentity)
{
  auto state = validDecodeState();
  BOOST_CHECK_NO_THROW(state.validate());
  BOOST_CHECK(state.digest().find("sha256:") == 0);
  state.recurrentConvolution.clear();
  BOOST_CHECK_THROW(state.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(DecodeStateTransactionAdmitsOnlyContiguousBoundState)
{
  DecodeStateTransactionV1 transaction(validDecodeState());
  auto next = validDecodeState(1);
  BOOST_CHECK_NO_THROW(transaction.apply(next, [] (const auto&) { return true; }));
  BOOST_CHECK_EQUAL(transaction.committed().tokenEpoch, 1);

  auto gap = validDecodeState(3);
  BOOST_CHECK_THROW(transaction.apply(gap), std::invalid_argument);
  auto changed = validDecodeState(2);
  changed.identity.providerBootId = "different-boot";
  BOOST_CHECK_THROW(transaction.apply(changed), std::invalid_argument);
}

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
  BOOST_CHECK_EQUAL(decoded.attemptEpoch, 1);
  BOOST_CHECK_EQUAL(decoded.tokenEpoch, 0);
  BOOST_CHECK_EQUAL(decoded.inputTokenCount, 32);
  BOOST_CHECK_EQUAL(decoded.maxGeneratedTokens, 2);
  BOOST_CHECK_EQUAL(decoded.deadlineEpochMs, 10'000);
  BOOST_REQUIRE_EQUAL(decoded.roles.size(), 3);
  BOOST_CHECK_EQUAL(decoded.roles[2].role, "/LLM/Pipeline/Stage/2");
  BOOST_CHECK_EQUAL(decoded.roles[2].provider, "/provider/2");
  BOOST_CHECK_EQUAL(decoded.roles[2].providerBootId, "boot-2");
}

BOOST_AUTO_TEST_CASE(SpecAcceptsNativeGenerationBudget1024)
{
  for (const auto budget : {64U, 1024U}) {
    auto spec = validSpec();
    spec.maxGeneratedTokens = budget;
    spec.tokenEpoch = budget - 1;
    BOOST_CHECK_NO_THROW(spec.validate());
    const auto decoded = qwenGenerationSessionSpecFromJson(
      qwenGenerationSessionSpecToJson(spec));
    BOOST_CHECK_EQUAL(decoded.maxGeneratedTokens, budget);
    BOOST_CHECK_EQUAL(decoded.tokenEpoch, budget - 1);
  }
}

BOOST_AUTO_TEST_CASE(SpecAllowsThreeStagesToShareAProvider)
{
  auto spec = validSpec();
  spec.roles[1].provider = spec.roles[0].provider;
  spec.roles[1].providerBootId = spec.roles[0].providerBootId;
  BOOST_CHECK_NO_THROW(spec.validate());
  const auto decoded = qwenGenerationSessionSpecFromJson(
    qwenGenerationSessionSpecToJson(spec));
  BOOST_REQUIRE_EQUAL(decoded.roles.size(), 3);
  BOOST_CHECK_EQUAL(decoded.roles[0].provider, decoded.roles[1].provider);
  BOOST_CHECK_EQUAL(decoded.roles[0].providerBootId, decoded.roles[1].providerBootId);
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
  spec.maxGeneratedTokens = 1025;
  checkInvalid(spec);
  spec = validSpec();
  spec.attemptEpoch = 3;
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
  spec.roles[1].role = spec.roles[0].role;
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
  BOOST_CHECK(state.state() == QwenGenerationState::Prefilling);
  state.completePrefill();
  BOOST_CHECK(state.state() == QwenGenerationState::Decoding);
  BOOST_CHECK_EQUAL(state.completeTokenEpoch(), 1);
  BOOST_CHECK_EQUAL(state.generatedTokenCount(), 1);
  state.beginReplacement();
  BOOST_CHECK(state.state() == QwenGenerationState::Rebuilding);
  BOOST_CHECK_EQUAL(state.attemptEpoch(), 2);
  state.activate();
  state.completePrefill();
  BOOST_CHECK_EQUAL(state.completeTokenEpoch(), 2);
  state.beginDrain(QwenGenerationFinishReason::MaxTokens);
  BOOST_CHECK(state.state() == QwenGenerationState::Draining);
  BOOST_CHECK(state.finishReason() == QwenGenerationFinishReason::MaxTokens);
  state.complete(QwenGenerationFinishReason::MaxTokens);
  BOOST_CHECK(state.state() == QwenGenerationState::Completed);
  BOOST_CHECK(state.isTerminal());
  BOOST_CHECK_THROW(state.complete(QwenGenerationFinishReason::MaxTokens), std::logic_error);
  BOOST_CHECK_THROW(state.beginReplacement(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(StateMachineRejectsInvalidTransitionsAndSecondReplacement)
{
  QwenGenerationSessionStateMachine state(validSpec());
  BOOST_CHECK_THROW(state.activate(), std::logic_error);
  BOOST_CHECK_THROW(state.completeTokenEpoch(), std::logic_error);
  state.beginSelection();
  state.activate();
  state.completePrefill();
  state.beginReplacement();
  state.activate();
  state.completePrefill();
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
  incomplete.completePrefill();
  BOOST_CHECK_THROW(incomplete.complete(QwenGenerationFinishReason::MaxTokens), std::logic_error);
  incomplete.cancel();
  BOOST_CHECK(incomplete.state() == QwenGenerationState::Cancelled);
  BOOST_CHECK(incomplete.isTerminal());

  QwenGenerationSessionStateMachine overflow(validSpec());
  overflow.beginSelection();
  overflow.activate();
  overflow.completePrefill();
  overflow.completeTokenEpoch();
  overflow.completeTokenEpoch();
  BOOST_CHECK_THROW(overflow.completeTokenEpoch(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(EosAndStopDrainRequireTheirOwnEvidence)
{
  auto eos = validSpec();
  eos.maxGeneratedTokens = 4;
  QwenGenerationSessionStateMachine eosState(eos);
  eosState.beginSelection();
  eosState.activate();
  eosState.completePrefill();
  eosState.completeTokenEpoch();
  BOOST_CHECK_THROW(eosState.beginDrain(QwenGenerationFinishReason::Eos),
                    std::logic_error);
  eosState.observeEosToken();
  eosState.beginDrain(QwenGenerationFinishReason::Eos);
  BOOST_CHECK_EQUAL(toString(QwenGenerationFinishReason::Eos),
                    std::string("EOS"));
  eosState.complete(QwenGenerationFinishReason::Eos);

  auto stop = validSpec();
  stop.maxGeneratedTokens = 4;
  QwenGenerationSessionStateMachine stopState(stop);
  stopState.beginSelection();
  stopState.activate();
  stopState.completePrefill();
  stopState.completeTokenEpoch();
  BOOST_CHECK_THROW(stopState.beginDrain(QwenGenerationFinishReason::StopSequence),
                    std::logic_error);
  stopState.observeStopSequence();
  stopState.beginDrain(QwenGenerationFinishReason::StopSequence);
  stopState.complete(QwenGenerationFinishReason::StopSequence);
}

BOOST_AUTO_TEST_CASE(MaxTokenDrainRejectsEarlyCompletion)
{
  QwenGenerationSessionStateMachine state(validSpec());
  state.beginSelection();
  state.activate();
  state.completePrefill();
  state.completeTokenEpoch();
  BOOST_CHECK_THROW(state.beginDrain(QwenGenerationFinishReason::MaxTokens),
                    std::logic_error);
}

BOOST_AUTO_TEST_CASE(CompletionRejectsReasonThatDiffersFromDrainingEvidence)
{
  auto spec = validSpec();
  spec.maxGeneratedTokens = 4;
  QwenGenerationSessionStateMachine state(spec);
  state.beginSelection();
  state.activate();
  state.completePrefill();
  state.completeTokenEpoch();
  state.observeEosToken();
  state.beginDrain(QwenGenerationFinishReason::Eos);
  BOOST_CHECK_THROW(
    state.complete(QwenGenerationFinishReason::ApplicationComplete),
    std::logic_error);
  BOOST_CHECK(state.state() == QwenGenerationState::Draining);
  state.complete(QwenGenerationFinishReason::Eos);
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
  state.completePrefill();
  BOOST_CHECK_EQUAL(state.completeTokenEpoch(1), 1);
  state.beginReplacement();
  state.activate();
  state.completePrefill();
  BOOST_CHECK_THROW(state.completeTokenEpoch(1), std::logic_error);
  BOOST_CHECK_EQUAL(state.generatedTokenCount(), 1);
  BOOST_CHECK_EQUAL(state.completeTokenEpoch(2), 2);
}

BOOST_AUTO_TEST_CASE(Spec111GenerationSessionRestoresEpochAndFencesStaleAttempts)
{
  auto nonInitial = validSpec();
  nonInitial.attemptEpoch = 2;
  BOOST_CHECK_NO_THROW(nonInitial.validate());

  QwenGenerationSessionStateMachine state(nonInitial);
  BOOST_CHECK_EQUAL(state.attemptEpoch(), 2);
  state.beginSelection();
  state.activate();
  state.completePrefill();
  BOOST_CHECK_THROW(state.completeTokenEpoch(1), std::logic_error);
  BOOST_CHECK_EQUAL(state.completeTokenEpoch(2), 1);
  BOOST_CHECK_THROW(state.beginReplacement(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(TerminalResponseCanBeClaimedExactlyOnce)
{
  QwenGenerationSessionStateMachine state(validSpec());
  BOOST_CHECK_THROW(state.claimTerminalResponse(), std::logic_error);
  state.beginSelection();
  state.activate();
  state.completePrefill();
  state.completeTokenEpoch();
  state.completeTokenEpoch();
  state.beginDrain(QwenGenerationFinishReason::MaxTokens);
  state.complete(QwenGenerationFinishReason::MaxTokens);
  BOOST_CHECK(state.claimTerminalResponse());
  BOOST_CHECK(!state.claimTerminalResponse());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::test
