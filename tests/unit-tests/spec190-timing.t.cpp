#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"

#include <ndn-cxx/util/logging.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace ndnsf::di::test {

namespace {

struct LoggingCapture
{
  std::ostringstream output;
  LoggingCapture() { ndn::util::Logging::setDestination(output, true); }
  ~LoggingCapture() { ndn::util::Logging::setDestination(std::clog, true); }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec190Timing)

BOOST_AUTO_TEST_CASE(ParsesAndValidatesOneRequestAttempt)
{
  const auto first = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=user executionRole=user phase=submit steady_us=10 "
    "timestamp_us=100 requestId=req-a attempt=1 providerBootId=none sessionId=none "
    "conversationId=none inferenceEpoch=none contextEpoch=none");
  const auto second = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=user executionRole=user phase=requestPublished steady_us=20 "
    "timestamp_us=90 requestId=req-a attempt=1 providerBootId=none sessionId=none "
    "conversationId=none inferenceEpoch=none contextEpoch=none");
  BOOST_REQUIRE(first);
  BOOST_REQUIRE(second);
  std::string error;
  BOOST_CHECK(validateRuntimePhaseSequence({*first, *second}, &error));
  BOOST_CHECK(error.empty());
}

BOOST_AUTO_TEST_CASE(RejectsMissingTimestampAndCrossAttemptMerge)
{
  BOOST_CHECK(!parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=user executionRole=user phase=submit steady_us=0 "
    "timestamp_us=100 requestId=req-a attempt=1 providerBootId=none sessionId=none "
    "conversationId=none inferenceEpoch=none contextEpoch=none"));
  const auto first = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=user executionRole=user phase=submit steady_us=10 "
    "timestamp_us=100 requestId=req-a attempt=1 providerBootId=none sessionId=none "
    "conversationId=none inferenceEpoch=none contextEpoch=none");
  const auto retry = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=user executionRole=user phase=submit steady_us=20 "
    "timestamp_us=110 requestId=req-a attempt=2 providerBootId=none sessionId=none "
    "conversationId=none inferenceEpoch=none contextEpoch=none");
  BOOST_REQUIRE(first);
  BOOST_REQUIRE(retry);
  BOOST_CHECK(!validateRuntimePhaseSequence({*first, *retry}));
}

BOOST_AUTO_TEST_CASE(RejectsNonMonotonicPhaseOrder)
{
  const auto first = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-provider executionRole=stage-a phase=requestPublished steady_us=30 "
    "timestamp_us=100 requestId=req-b attempt=1 providerBootId=boot-a sessionId=session-b "
    "conversationId=none inferenceEpoch=1 contextEpoch=1");
  const auto second = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-provider executionRole=stage-a phase=submit steady_us=31 "
    "timestamp_us=101 requestId=req-b attempt=1 providerBootId=boot-a sessionId=session-b "
    "conversationId=none inferenceEpoch=1 contextEpoch=1");
  BOOST_REQUIRE(first);
  BOOST_REQUIRE(second);
  BOOST_CHECK(!validateRuntimePhaseSequence({*first, *second}));
}

BOOST_AUTO_TEST_CASE(RejectsDuplicateFieldsAndUnclosedOrRepeatedPhases)
{
  BOOST_CHECK(!parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=user component=duplicate executionRole=user phase=submit "
    "steady_us=1 timestamp_us=1 requestId=req attempt=1"));
  const auto begin = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-provider executionRole=stage phase=ortRunBegin "
    "steady_us=10 timestamp_us=1 requestId=req attempt=1 providerBootId=boot "
    "sessionId=session conversationId=none inferenceEpoch=1 contextEpoch=1");
  BOOST_REQUIRE(begin);
  std::string error;
  BOOST_CHECK(!validateRuntimePhaseSequence({*begin}, &error));
  BOOST_CHECK(error.find("unclosed") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AllowsSteadyTieAndIgnoresWallClockRollback)
{
  const auto begin = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-provider executionRole=stage phase=ortRunBegin "
    "steady_us=10 timestamp_us=200 requestId=req attempt=1 providerBootId=boot "
    "sessionId=session conversationId=none inferenceEpoch=1 contextEpoch=1");
  const auto end = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-provider executionRole=stage phase=ortRunEnd "
    "steady_us=10 timestamp_us=100 requestId=req attempt=1 providerBootId=boot "
    "sessionId=session conversationId=none inferenceEpoch=1 contextEpoch=1");
  BOOST_REQUIRE(begin);
  BOOST_REQUIRE(end);
  BOOST_CHECK(validateRuntimePhaseSequence({*begin, *end}));
}

BOOST_AUTO_TEST_CASE(ValidatesTokenIndexAndRejectsReversedRunBoundary)
{
  const auto token1 = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-cli executionRole=requester phase=tokenReceived "
    "steady_us=10 timestamp_us=200 requestId=req attempt=1 providerBootId=none "
    "sessionId=session conversationId=conv inferenceEpoch=1 contextEpoch=0 tokenIndex=1");
  const auto delivered1 = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-cli executionRole=requester phase=tokenDelivered "
    "steady_us=10 timestamp_us=199 requestId=req attempt=1 providerBootId=none "
    "sessionId=session conversationId=conv inferenceEpoch=1 contextEpoch=0 tokenIndex=1");
  const auto emitted1 = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-cli executionRole=cli-output phase=tokenEmitted "
    "steady_us=10 timestamp_us=198 requestId=req attempt=1 providerBootId=none "
    "sessionId=session conversationId=conv inferenceEpoch=1 contextEpoch=0 tokenIndex=1");
  const auto token2 = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-cli executionRole=requester phase=tokenReceived "
    "steady_us=11 timestamp_us=198 requestId=req attempt=1 providerBootId=none "
    "sessionId=session conversationId=conv inferenceEpoch=1 contextEpoch=0 tokenIndex=2");
  const auto delivered2 = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-cli executionRole=requester phase=tokenDelivered "
    "steady_us=12 timestamp_us=197 requestId=req attempt=1 providerBootId=none "
    "sessionId=session conversationId=conv inferenceEpoch=1 contextEpoch=0 tokenIndex=2");
  const auto emitted2 = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-cli executionRole=cli-output phase=tokenEmitted "
    "steady_us=12 timestamp_us=197 requestId=req attempt=1 providerBootId=none "
    "sessionId=session conversationId=conv inferenceEpoch=1 contextEpoch=0 tokenIndex=2");
  BOOST_REQUIRE(token1);
  BOOST_REQUIRE(delivered1);
  BOOST_REQUIRE(emitted1);
  BOOST_REQUIRE(token2);
  BOOST_REQUIRE(delivered2);
  BOOST_REQUIRE(emitted2);
  BOOST_CHECK(validateRuntimePhaseSequence({*token1, *delivered1,
                                            *token2, *delivered2}));
  BOOST_CHECK(validateRuntimePhaseSequence({*emitted1, *emitted2}));

  const auto begin = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-provider executionRole=stage phase=ortRunBegin "
    "steady_us=20 timestamp_us=1 requestId=bad attempt=1 providerBootId=boot "
    "sessionId=session conversationId=none inferenceEpoch=1 contextEpoch=0");
  const auto end = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-provider executionRole=stage phase=ortRunEnd "
    "steady_us=19 timestamp_us=2 requestId=bad attempt=1 providerBootId=boot "
    "sessionId=session conversationId=none inferenceEpoch=1 contextEpoch=0");
  BOOST_REQUIRE(begin);
  BOOST_REQUIRE(end);
  BOOST_CHECK(!validateRuntimePhaseSequence({*begin, *end}));
}

BOOST_AUTO_TEST_CASE(RejectsTokenOverwriteAndAcceptsScopedDependencyPairs)
{
  const auto receive1 = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-cli executionRole=requester phase=tokenReceived "
    "steady_us=10 timestamp_us=10 requestId=req attempt=1 providerBootId=none "
    "sessionId=session conversationId=conv inferenceEpoch=1 contextEpoch=0 tokenIndex=1");
  const auto receive2 = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-cli executionRole=requester phase=tokenReceived "
    "steady_us=11 timestamp_us=11 requestId=req attempt=1 providerBootId=none "
    "sessionId=session conversationId=conv inferenceEpoch=1 contextEpoch=0 tokenIndex=2");
  const auto emit2 = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-cli executionRole=requester phase=tokenEmitted "
    "steady_us=12 timestamp_us=12 requestId=req attempt=1 providerBootId=none "
    "sessionId=session conversationId=conv inferenceEpoch=1 contextEpoch=0 tokenIndex=2");
  BOOST_REQUIRE(receive1);
  BOOST_REQUIRE(receive2);
  BOOST_REQUIRE(emit2);
  BOOST_CHECK(!validateRuntimePhaseSequence({*receive1, *receive2, *emit2}));

  const auto fetchA = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-provider executionRole=stage phase=stageInputFetchBegin "
    "scope=a steady_us=10 timestamp_us=10 requestId=req attempt=1 providerBootId=boot "
    "sessionId=session conversationId=none inferenceEpoch=1 contextEpoch=0");
  const auto fetchADone = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-provider executionRole=stage phase=stageInputFetchEnd "
    "scope=a steady_us=11 timestamp_us=11 requestId=req attempt=1 providerBootId=boot "
    "sessionId=session conversationId=none inferenceEpoch=1 contextEpoch=0");
  const auto fetchB = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-provider executionRole=stage phase=stageInputFetchBegin "
    "scope=b steady_us=12 timestamp_us=12 requestId=req attempt=1 providerBootId=boot "
    "sessionId=session conversationId=none inferenceEpoch=1 contextEpoch=0");
  const auto fetchBDone = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-provider executionRole=stage phase=stageInputFetchEnd "
    "scope=b steady_us=13 timestamp_us=13 requestId=req attempt=1 providerBootId=boot "
    "sessionId=session conversationId=none inferenceEpoch=1 contextEpoch=0");
  BOOST_REQUIRE(fetchA);
  BOOST_REQUIRE(fetchADone);
  BOOST_REQUIRE(fetchB);
  BOOST_REQUIRE(fetchBDone);
  BOOST_CHECK(validateRuntimePhaseSequence({*fetchA, *fetchADone, *fetchB, *fetchBDone}));
}

BOOST_AUTO_TEST_CASE(ValidatesContextEpochTransitionAtCheckpoint)
{
  const auto submit = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-cli executionRole=requester phase=submit "
    "steady_us=10 timestamp_us=10 requestId=req attempt=1 providerBootId=none "
    "sessionId=none conversationId=conv inferenceEpoch=1 contextEpoch=0");
  const auto checkpoint = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-cli executionRole=requester phase=checkpointCommitted "
    "steady_us=20 timestamp_us=20 requestId=req attempt=1 providerBootId=none "
    "sessionId=none conversationId=conv inferenceEpoch=1 contextEpoch=1");
  const auto ready = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=di-cli executionRole=requester phase=turnReady "
    "steady_us=30 timestamp_us=30 requestId=req attempt=1 providerBootId=none "
    "sessionId=none conversationId=conv inferenceEpoch=1 contextEpoch=1");
  BOOST_REQUIRE(submit);
  BOOST_REQUIRE(checkpoint);
  BOOST_REQUIRE(ready);
  BOOST_CHECK(validateRuntimePhaseSequence({*submit, *checkpoint, *ready}));

  auto badReady = *ready;
  badReady.contextEpoch = "2";
  BOOST_CHECK(!validateRuntimePhaseSequence({*submit, *checkpoint, badReady}));
}

BOOST_AUTO_TEST_CASE(ValidatesInterleavedProviderAckIdentity)
{
  const auto receivedA = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=user executionRole=user phase=ackReceived "
    "steady_us=10 timestamp_us=10 requestId=req attempt=request providerBootId=none "
    "sessionId=none conversationId=none inferenceEpoch=none contextEpoch=none "
    "providerName=provider-a");
  const auto verifiedA = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=user executionRole=user phase=ackVerified "
    "steady_us=11 timestamp_us=11 requestId=req attempt=request providerBootId=none "
    "sessionId=none conversationId=none inferenceEpoch=none contextEpoch=none "
    "providerName=provider-a");
  const auto receivedB = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=user executionRole=user phase=ackReceived "
    "steady_us=12 timestamp_us=12 requestId=req attempt=request providerBootId=none "
    "sessionId=none conversationId=none inferenceEpoch=none contextEpoch=none "
    "providerName=provider-b");
  const auto verifiedB = parseRuntimePhaseObservation(
    "NDNSF_PHASE_TIMING component=user executionRole=user phase=ackVerified "
    "steady_us=13 timestamp_us=13 requestId=req attempt=request providerBootId=none "
    "sessionId=none conversationId=none inferenceEpoch=none contextEpoch=none "
    "providerName=provider-b");
  BOOST_REQUIRE(receivedA);
  BOOST_REQUIRE(verifiedA);
  BOOST_REQUIRE(receivedB);
  BOOST_REQUIRE(verifiedB);
  BOOST_CHECK(validateRuntimePhaseSequence({*receivedA, *verifiedA, *receivedB, *verifiedB}));
  BOOST_CHECK(!validateRuntimePhaseSequence({*receivedA, *verifiedA, *verifiedA}));
}

BOOST_AUTO_TEST_CASE(EmitsCanonicalPhaseRecord)
{
  ::setenv("NDNSF_PHASE_TIMING", "1", 1);
  ndn::util::Logging::setLevel("*=WARN");
  LoggingCapture capture;
  logRuntimePhase("di-provider", "runnerPreparationEnd", "req-c", "1",
                  {{"executionRole", "stage"}, {"sessionId", "session-c"}});
  ndn::util::Logging::flush();
  ::unsetenv("NDNSF_PHASE_TIMING");
  const auto parsed = parseRuntimePhaseObservation(capture.output.str());
  BOOST_REQUIRE(parsed);
  BOOST_CHECK_EQUAL(parsed->role, "di-provider");
  BOOST_CHECK_EQUAL(parsed->executionRole, "stage");
  BOOST_CHECK_EQUAL(parsed->phase, "runnerPreparationEnd");
  BOOST_CHECK_EQUAL(parsed->requestId, "req-c");
  BOOST_CHECK_EQUAL(parsed->attempt, "1");
  BOOST_CHECK_EQUAL(parsed->sessionId, "session-c");
  BOOST_CHECK_EQUAL(parsed->providerBootId, "none");
  BOOST_CHECK(capture.output.str().find("sessionId=session-c") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::test
