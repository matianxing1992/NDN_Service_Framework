#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"

#include <ndn-cxx/util/logging.hpp>

#include <boost/test/unit_test.hpp>

namespace ndnsf::di::tests {

BOOST_AUTO_TEST_SUITE(DiNativeRuntimeLogging)

BOOST_AUTO_TEST_CASE(EmitsBoundedRecordsForFilterRegression)
{
  logRuntimeTrace("NDNSF_DI_LOG_FILTER_TEST level=TRACE");
  logRuntimeInfo("NDNSF_DI_LOG_FILTER_TEST level=INFO");
  logRuntimeWarn("NDNSF_DI_LOG_FILTER_TEST level=WARN");
  logRuntimeError("NDNSF_DI_LOG_FILTER_TEST level=ERROR");
  // ndn-cxx uses an asynchronous sink by default.  Flush before the process
  // exits so the subprocess filter gate observes a complete, deterministic
  // record set rather than racing sink shutdown.
  ndn::util::Logging::flush();
  BOOST_CHECK(true);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
