#include "tests/boost-test.hpp"

#include "ndn-service-framework/exact-data-congestion-window.hpp"

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
  BOOST_CHECK(window.rto() == backedOff);
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

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
