#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <boost/test/unit_test.hpp>
#include <cfenv>
#include <fstream>
#include <future>
#include <limits>
#include <unordered_map>
#include <vector>

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

BOOST_AUTO_TEST_CASE(IntegralFloatsRetainExactFixedIdentityAcrossRoundingModes)
{
  struct RestoreRounding {
    int saved = std::fegetround();
    ~RestoreRounding() { std::fesetround(saved); }
  } restore;
  for (const auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    BOOST_REQUIRE_EQUAL(std::fesetround(mode), 0);
    for (const std::int64_t value : {std::int64_t{1}, std::int64_t{-1},
         std::int64_t{10}, std::int64_t{-1000}, std::int64_t{999999999999999},
         std::int64_t{1000000000000000}, std::int64_t{9007199254740991},
         std::int64_t{-9007199254740991}}) {
      BOOST_CHECK_EQUAL(nativeCanonicalJson(NativeJson(static_cast<double>(value))),
                        std::to_string(value) + ".0");
    }
    BOOST_CHECK_EQUAL(nativeCanonicalJson(NativeJson(-0.0)), "-0.0");
    BOOST_CHECK_EQUAL(nativeCanonicalJson(NativeJson(0.0)), "0.0");
    BOOST_CHECK_EQUAL(nativeCanonicalJson(NativeJson(9007199254740992.0)),
                      "9007199254740992.0");
    BOOST_CHECK_EQUAL(nativeCanonicalJson(NativeJson(1e16)), "1e+16");
  }
}

BOOST_AUTO_TEST_CASE(IntegralShortcutPreservesJsonTypesAndFractionalFallback)
{
  BOOST_CHECK_EQUAL(nativeCanonicalJson(NativeJson::array({1, 1.0, -0.0, 1.5})),
                    "[1,1.0,-0.0,1.5]");
  BOOST_CHECK_EQUAL(nativeCanonicalJson(NativeJson(1e-4)), "0.0001");
  BOOST_CHECK_EQUAL(nativeCanonicalJson(NativeJson(1e-5)), "1e-05");
}

BOOST_AUTO_TEST_CASE(FloatingCachePreservesCollisionsSaturationAndRounding)
{
  struct RestoreRounding {
    int saved = std::fegetround();
    ~RestoreRounding() { std::fesetround(saved); }
  } restore;
  const auto check = [](double value) {
    std::string expected;
    try { expected = canonical_json_detail::floatingUncached(value); }
    catch (const std::invalid_argument&) {
      BOOST_CHECK_THROW(canonical_json_detail::floating(value), std::invalid_argument);
      return;
    }
    BOOST_CHECK_EQUAL(canonical_json_detail::floating(value), expected);
  };
  BOOST_REQUIRE_EQUAL(std::fesetround(FE_TONEAREST), 0);
  std::unordered_map<std::size_t, double> buckets;
  bool collided = false;
  std::vector<double> values;
  for (unsigned i = 0; i < 256; ++i) {
    const double value = 0.125 + i / 17.0;
    values.push_back(value);
    const auto entry = buckets.emplace(std::hash<double>{}(value) % 64, value);
    if (!entry.second && entry.first->second != value) {
      check(entry.first->second); check(value); check(entry.first->second);
      collided = true;
    }
    check(value); check(value);
  }
  BOOST_REQUIRE(collided);
  for (int mode : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO, FE_TONEAREST}) {
    BOOST_REQUIRE_EQUAL(std::fesetround(mode), 0);
    for (auto it = values.rbegin(); it != values.rend(); ++it) check(*it);
    for (double value : {0.1, -0.1, 0.35, -0.35, 1e-5, 1e16,
                        std::numeric_limits<double>::denorm_min()}) check(value);
  }
}

BOOST_AUTO_TEST_CASE(FloatingCacheOwnsValuesAcrossThreadLifetimes)
{
  for (unsigned wave = 0; wave < 3; ++wave) {
    std::vector<std::future<bool>> workers;
    for (unsigned thread = 0; thread < 4; ++thread) {
      workers.emplace_back(std::async(std::launch::async, [thread] {
        for (unsigned i = 0; i < 512; ++i) {
          const double value = i % 2 ? 0.35 : 0.125 + (i + thread) / 17.0;
          if (canonical_json_detail::floating(value) !=
              canonical_json_detail::floatingUncached(value)) return false;
        }
        return true;
      }));
    }
    for (auto& worker : workers) BOOST_CHECK(worker.get());
  }
}

BOOST_AUTO_TEST_SUITE_END()
