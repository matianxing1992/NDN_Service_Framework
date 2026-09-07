#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <boost/test/unit_test.hpp>
#include <fstream>
#include <limits>

using namespace ndnsf::di;

BOOST_AUTO_TEST_SUITE(Spec182CanonicalJson)

BOOST_AUTO_TEST_CASE(FrozenPythonIdentityBytes)
{
  std::ifstream file("tests/fixtures/spec182/canonical-json-vectors.json");
  BOOST_REQUIRE(file.good());
  const auto fixture = NativeJson::parse(file);
  BOOST_REQUIRE_EQUAL(fixture.at("schema").get<std::string>(), "spec182-canonical-json-v1");
  for (const auto& row : fixture.at("rows")) {
    BOOST_CHECK_EQUAL(nativeCanonicalJson(row.at("value")), row.at("canonical").get<std::string>());
  }
}

BOOST_AUTO_TEST_CASE(NonfiniteAndInvalidUnicodeFailClosed)
{
  for (const auto value : {std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()}) {
    BOOST_CHECK_THROW(nativeCanonicalJson(NativeJson(value)), std::invalid_argument);
  }
  BOOST_CHECK_THROW(nativeCanonicalJson(NativeJson(std::string("\xff", 1))), NativeJson::type_error);
}

BOOST_AUTO_TEST_CASE(ParseRejectsDuplicateKeysWithoutRejectingIndependentObjects)
{
  BOOST_CHECK_THROW(nativeParseJson("{\"a\":1,\"a\":2}"), std::invalid_argument);
  BOOST_CHECK_THROW(nativeParseJson("{\"a\":{\"b\":1,\"b\":2}}"), std::invalid_argument);
  BOOST_CHECK_NO_THROW(nativeParseJson("[{\"a\":1},{\"a\":2}]"));
  BOOST_CHECK_NO_THROW(nativeParseJson("{\"a\":{\"a\":1}}"));
}

BOOST_AUTO_TEST_SUITE_END()
