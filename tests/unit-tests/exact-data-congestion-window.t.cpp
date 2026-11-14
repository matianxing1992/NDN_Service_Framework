#include "tests/boost-test.hpp"

#include "ndn-service-framework/exact-data-congestion-window.hpp"

#include <limits>

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(ExactDataCongestionWindowTests)

using Window = detail::ExactDataCongestionWindow;

BOOST_AUTO_TEST_CASE(StartsAtSixteenAndGrowsWithinResourceBudget)
{
  Window window(40, ndn::time::milliseconds(500));
  BOOST_CHECK_EQUAL(window.size(), 16U);
  for (std::size_t index = 0; index < 16; ++index) {
    window.noteSent(index);
    window.acknowledge(index, 0, std::chrono::milliseconds(12), 16);
  }
  BOOST_CHECK_EQUAL(window.size(), 32U);
  BOOST_CHECK_EQUAL(window.rttSamples(), 16U);
  BOOST_CHECK(window.rto() >= std::chrono::milliseconds(50));
  for (std::size_t index = 16; index < 100; ++index) {
    window.noteSent(index);
    window.acknowledge(index, 0, std::nullopt, 16);
  }
  BOOST_CHECK_EQUAL(window.size(), 40U);
  Window small(3, ndn::time::milliseconds(20));
  BOOST_CHECK_EQUAL(small.size(), 3U);
  BOOST_CHECK(small.rto() == std::chrono::milliseconds(20));
  BOOST_CHECK_THROW(Window(0, ndn::time::milliseconds(500)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OwnerThresholdBoundsStartupButNotLaterWindowGrowth)
{
  Window finite(4096, ndn::time::milliseconds(500), 64);
  Window unchanged(4096, ndn::time::milliseconds(500));
  for (std::size_t index = 0; index < 512; ++index) {
    finite.noteSent(index);
    finite.acknowledge(index, 0, std::nullopt, 16);
    unchanged.noteSent(index);
    unchanged.acknowledge(index, 0, std::nullopt, 16);
  }
  BOOST_CHECK_GT(finite.size(), 64U);
  BOOST_CHECK_LT(finite.size(), 128U);
  BOOST_CHECK_EQUAL(unchanged.size(), 528U);
  for (std::size_t index = 512; index < 32000; ++index) {
    finite.noteSent(index);
    finite.acknowledge(index, 0, std::nullopt, 64);
  }
  BOOST_CHECK_GT(finite.size(), 256U);
  BOOST_CHECK_LE(finite.size(), 4096U);
  const auto beforeMark = finite.size();
  finite.acknowledge(32000, 1, std::nullopt, 64);
  BOOST_CHECK_LE(finite.size(), beforeMark / 2 + 1);
  BOOST_CHECK_EQUAL(finite.decreases(), 1U);
  BOOST_CHECK_THROW(Window(4096, ndn::time::milliseconds(500), 0), std::invalid_argument);
  BOOST_CHECK_THROW(Window(4096, ndn::time::milliseconds(500), -1), std::invalid_argument);
  BOOST_CHECK_THROW(Window(4096, ndn::time::milliseconds(500),
    std::numeric_limits<double>::infinity()), std::invalid_argument);
  BOOST_CHECK_THROW(Window(4096, ndn::time::milliseconds(500),
    std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RecoveryFlightDecreasesOnceAndThenUsesCongestionAvoidance)
{
  Window window(64, ndn::time::milliseconds(500));
  for (std::size_t index = 0; index < 16; ++index) window.noteSent(index);
  window.acknowledge(0, 0, std::chrono::milliseconds(12), 16);
  window.loss();
  BOOST_CHECK_EQUAL(window.size(), 8U);
  const auto backedOff = window.rto();
  window.loss();
  BOOST_CHECK_EQUAL(window.size(), 8U);
  BOOST_CHECK_EQUAL(window.decreases(), 1U);
  BOOST_CHECK(window.rto() == backedOff * 2);
  window.noteSent(16);
  window.acknowledge(16, 0, std::nullopt, 8);
  BOOST_CHECK_EQUAL(window.size(), 8U);
  window.loss();
  BOOST_CHECK_EQUAL(window.size(), 4U);
  BOOST_CHECK_EQUAL(window.decreases(), 2U);
  BOOST_CHECK_EQUAL(window.losses(), 3U);
}

BOOST_AUTO_TEST_CASE(ValidatedMarksReduceWindowWithoutFabricatingRttSamples)
{
  Window window(64, ndn::time::milliseconds(500));
  for (std::size_t index = 0; index < 16; ++index) window.noteSent(index);
  window.acknowledge(0, 1, std::nullopt, 16);
  BOOST_CHECK_EQUAL(window.size(), 8U);
  window.acknowledge(1, 1, std::nullopt, 15);
  BOOST_CHECK_EQUAL(window.size(), 8U);
  BOOST_CHECK_EQUAL(window.marks(), 2U);
  BOOST_CHECK_EQUAL(window.decreases(), 1U);
  BOOST_CHECK_EQUAL(window.rttSamples(), 0U);
  BOOST_CHECK(window.rto() == std::chrono::milliseconds(500));
}

BOOST_AUTO_TEST_CASE(MarkedRecoveryStillBacksOffEveryLossWithinTheLifetimeCap)
{
  Window window(64, ndn::time::milliseconds(500));
  for (std::size_t index = 0; index < 16; ++index) window.noteSent(index);
  window.acknowledge(0, 1, std::chrono::milliseconds(12), 16);
  const auto initialRto = window.rto();
  const auto reducedWindow = window.size();
  window.loss();
  BOOST_CHECK(window.rto() == initialRto * 2);
  BOOST_CHECK_EQUAL(window.size(), reducedWindow);
  BOOST_CHECK_EQUAL(window.decreases(), 1U);
  window.loss();
  BOOST_CHECK(window.rto() == initialRto * 4);
  BOOST_CHECK_EQUAL(window.size(), reducedWindow);
  BOOST_CHECK_EQUAL(window.decreases(), 1U);
  for (unsigned i = 0; i < 8; ++i) window.loss();
  BOOST_CHECK(window.rto() == std::chrono::milliseconds(500));
  BOOST_CHECK_EQUAL(window.decreases(), 1U);
  window.noteSent(16);
  window.acknowledge(16, 0, std::nullopt, 8);
  window.loss();
  BOOST_CHECK_EQUAL(window.decreases(), 2U);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
