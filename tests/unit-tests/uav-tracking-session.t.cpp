#include "../../NDNSF-UAV-APP/tracking/UavTrackingSession.hpp"

#include <boost/test/unit_test.hpp>

#include <stdexcept>

using namespace ndnsf::examples::uav;

BOOST_AUTO_TEST_SUITE(UavTrackingSessionTests)

BOOST_AUTO_TEST_CASE(ProcessesSequentialWindowsAndDeduplicates)
{
  UavTrackingSession session("s", 2, 60);
  TrackingWindow first{"req-0", 1, 0, "hash-a", 0, 1000};
  int executions = 0;
  auto execute = [&] (const TrackingWindow&) { ++executions; return "result-a"; };
  auto result = session.process(first, execute);
  BOOST_REQUIRE(result);
  BOOST_CHECK_EQUAL(executions, 1);
  result = session.process(first, execute);
  BOOST_REQUIRE(result);
  BOOST_CHECK(result->cached);
  BOOST_CHECK_EQUAL(executions, 1);
  first.inputDigest = "hash-b";
  std::string reason;
  BOOST_CHECK(!session.process(first, execute, &reason));
  BOOST_CHECK_EQUAL(reason, "request ID was reused with different input");
}

BOOST_AUTO_TEST_CASE(RejectsOutOfOrderAndAbortsEpoch)
{
  UavTrackingSession session("s");
  TrackingWindow second{"req-1", 1, 1, "hash", 0, 1};
  std::string reason;
  BOOST_CHECK(!session.process(second, [] (const TrackingWindow&) { return "r"; }, &reason));
  BOOST_CHECK_EQUAL(reason, "window is not the next sequential window");
  session.abort("response outcome uncertain");
  BOOST_CHECK(static_cast<int>(session.state()) ==
              static_cast<int>(TrackingSessionState::Aborted));
  BOOST_CHECK(!session.process(TrackingWindow{"req-0", 1, 0, "h", 0, 1},
                              [] (const TrackingWindow&) { return "r"; }, &reason));
}

BOOST_AUTO_TEST_CASE(ExecutorFailureAbortsAndCacheEvictionIsBounded)
{
  UavTrackingSession session("s", 1, 3);
  const auto run = [] (const TrackingWindow& window) {
    if (window.index == 1) throw std::runtime_error("model stopped");
    return std::string("r-") + std::to_string(window.index);
  };
  BOOST_REQUIRE(session.process(TrackingWindow{"req-0", 1, 0, "h0", 0, 1}, run));
  std::string reason;
  BOOST_CHECK(!session.process(TrackingWindow{"req-1", 1, 1, "h1", 2, 3}, run, &reason));
  BOOST_CHECK_EQUAL(reason, "executor failed: model stopped");
  BOOST_CHECK(static_cast<int>(session.state()) ==
              static_cast<int>(TrackingSessionState::Aborted));
}

BOOST_AUTO_TEST_SUITE_END()
