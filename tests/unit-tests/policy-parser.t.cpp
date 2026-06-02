#include "tests/boost-test.hpp"

#include "ndn-service-framework/PolicyParser.hpp"

#include <filesystem>
#include <fstream>

namespace ndn_service_framework::test {
namespace {

std::filesystem::path
writePolicyFile()
{
  auto path = std::filesystem::temp_directory_path() /
              "ndnsf-policy-parser-exact-test.policies";
  std::ofstream out(path);
  out << R"POLICY(
name /example/controller/NDNSF/ControllerPolicy/v1

provider-policies
{
    provider-policy
    {
        for /example/provider/A
        allow
        {
            /AI/YOLO/Detect
        }
    }
    provider-policy
    {
        for /example/provider/B
        allow
        {
            /AI/Shared/Runtime
        }
    }
}

user-policies
{
    user-policy
    {
        for /example/users/alice
        allow
        {
            /AI/YOLO/Detect
        }
    }
    user-policy
    {
        for /example/users/bob
        allow
        {
            /AI/Shared/Runtime
        }
    }
}
)POLICY";
  return path;
}

} // namespace

BOOST_AUTO_TEST_SUITE(PolicyParser)

BOOST_AUTO_TEST_CASE(ParsesExactRules)
{
  const auto path = writePolicyFile();
  ndn_service_framework::PolicyParser parser;
  const auto [providers, users] = parser.parsePolicyFile(path.string());

  BOOST_REQUIRE_EQUAL(providers.size(), 2);
  BOOST_CHECK_EQUAL(providers[0].providerName, "/example/provider/A");
  BOOST_REQUIRE_EQUAL(providers[0].allowedServices.size(), 1);
  BOOST_CHECK_EQUAL(providers[0].allowedServices[0], "/AI/YOLO/Detect");

  BOOST_CHECK_EQUAL(providers[1].providerName, "/example/provider/B");
  BOOST_REQUIRE_EQUAL(providers[1].allowedServices.size(), 1);
  BOOST_CHECK_EQUAL(providers[1].allowedServices[0], "/AI/Shared/Runtime");

  BOOST_REQUIRE_EQUAL(users.size(), 2);
  BOOST_CHECK_EQUAL(users[0].userName, "/example/users/alice");
  BOOST_REQUIRE_EQUAL(users[0].allowedServices.size(), 1);
  BOOST_CHECK_EQUAL(users[0].allowedServices[0], "/AI/YOLO/Detect");

  BOOST_CHECK_EQUAL(users[1].userName, "/example/users/bob");
  BOOST_REQUIRE_EQUAL(users[1].allowedServices.size(), 1);
  BOOST_CHECK_EQUAL(users[1].allowedServices[0], "/AI/Shared/Runtime");

  std::filesystem::remove(path);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
