#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"

#include <boost/test/unit_test.hpp>

using namespace ndnsf::di;

BOOST_AUTO_TEST_SUITE(Spec182NativeInferenceClient)

BOOST_AUTO_TEST_CASE(EmptyHandleFailsClosedAndErrorKeepsStructuredIdentity)
{
  NativeInferenceHandle handle;
  BOOST_CHECK_THROW(handle.status(), NativeDiError);
  BOOST_CHECK_THROW(handle.result(std::chrono::milliseconds(0)), NativeDiError);

  NativeDiError error("INVALID_REQUEST", "local", "request", "bad input",
                      "/NDNSF/DI/REQUEST/1", 1);
  BOOST_CHECK_EQUAL(error.code(), "INVALID_REQUEST");
  BOOST_CHECK_EQUAL(error.domain(), "local");
  BOOST_CHECK_EQUAL(error.boundary(), "request");
  BOOST_CHECK_EQUAL(error.requestId(), "/NDNSF/DI/REQUEST/1");
  BOOST_CHECK_EQUAL(error.attempt(), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
