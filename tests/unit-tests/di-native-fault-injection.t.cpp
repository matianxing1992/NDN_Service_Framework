#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeFaultInjection.hpp"

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <stdexcept>

namespace ndnsf::di::tests {

BOOST_AUTO_TEST_SUITE(DiNativeFaultInjection)

BOOST_AUTO_TEST_CASE(ConfigurationFailsClosed)
{
  NativeFaultConfig invalid{"unknown", "/LLM/Pipeline/Stage/1", 0};
  BOOST_CHECK_THROW(NativeFaultInjection::instance().configure(invalid),
                    std::invalid_argument);
  invalid = {"straggler", "/LLM/Pipeline/Stage/1", 0};
  BOOST_CHECK_THROW(NativeFaultInjection::instance().configure(invalid),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RoleMatchedFaultInjectsExactlyOnce)
{
  auto& faults = NativeFaultInjection::instance();
  faults.configure({"missing-segment", "/LLM/Pipeline/Stage/1", 0});
  BOOST_CHECK_NO_THROW(faults.checkpoint(
    NativeFaultPoint::DependencyFetched, "/LLM/Pipeline/Stage/0", "session-a"));
  BOOST_CHECK(!faults.injected());
  BOOST_CHECK_EXCEPTION(
    faults.checkpoint(NativeFaultPoint::DependencyFetched,
                      "/LLM/Pipeline/Stage/1", "session-a"),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()) ==
             "NDNSF_DI_EXPERIMENT_FAULT:missing-segment";
    });
  BOOST_CHECK(faults.injected());
  BOOST_CHECK_NO_THROW(faults.checkpoint(
    NativeFaultPoint::DependencyFetched, "/LLM/Pipeline/Stage/1", "session-a"));
}

BOOST_AUTO_TEST_CASE(StragglerDelayIsBoundedAndOneShot)
{
  auto& faults = NativeFaultInjection::instance();
  faults.configure({"straggler", "/LLM/Pipeline/Stage/1", 10});
  const auto start = std::chrono::steady_clock::now();
  faults.checkpoint(NativeFaultPoint::BeforeCompute,
                    "/LLM/Pipeline/Stage/1", "session-b");
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start);
  BOOST_CHECK_GE(elapsed.count(), 8);
  const auto secondStart = std::chrono::steady_clock::now();
  faults.checkpoint(NativeFaultPoint::BeforeCompute,
                    "/LLM/Pipeline/Stage/1", "session-b");
  const auto second = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - secondStart);
  BOOST_CHECK_LT(second.count(), 8);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
