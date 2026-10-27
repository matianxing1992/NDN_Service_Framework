#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/deployment-readiness.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <algorithm>
#include <string>

#include <unistd.h>

namespace ndn_service_framework::tests {
namespace {

class ReadinessOutput
{
public:
  ReadinessOutput()
  {
    if (::pipe(m_pipe) != 0)
      throw std::runtime_error("cannot create readiness fixture pipe");
    m_saved = ::dup(STDOUT_FILENO);
    if (m_saved < 0 || ::dup2(m_pipe[1], STDOUT_FILENO) < 0) {
      if (m_saved >= 0)
        ::close(m_saved);
      ::close(m_pipe[0]);
      ::close(m_pipe[1]);
      throw std::runtime_error("cannot capture readiness stdout");
    }
    ::close(m_pipe[1]);
  }

  ~ReadinessOutput()
  {
    restore();
    ::close(m_pipe[0]);
  }

  std::string
  read()
  {
    restore();
    char bytes[4096];
    const auto count = ::read(m_pipe[0], bytes, sizeof(bytes));
    if (count < 0)
      throw std::runtime_error("cannot read readiness fixture pipe");
    return std::string(bytes, static_cast<std::size_t>(count));
  }

private:
  void
  restore()
  {
    if (m_saved >= 0) {
      ::dup2(m_saved, STDOUT_FILENO);
      ::close(m_saved);
      m_saved = -1;
    }
  }

  int m_pipe[2];
  int m_saved = -1;
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec217DeploymentReadiness)

BOOST_AUTO_TEST_CASE(LegacyExecutionEmitsNoManagedRecord)
{
  test::ScopedEnvironmentValue actor("NDNSF_DEPLOYMENT_ACTOR_ID", nullptr);
  test::ScopedEnvironmentValue boot("NDNSF_DEPLOYMENT_BOOT_ID", nullptr);
  test::ScopedEnvironmentValue digest("NDNSF_DEPLOYMENT_CONFIG_SHA256", nullptr);
  ReadinessOutput output;
  ndnsf::di::detail::emitDeploymentReadiness("/provider", "native-boot");
  const auto bytes = output.read();
  BOOST_CHECK(bytes.empty());
}

BOOST_AUTO_TEST_CASE(ManagedRecordRetainsExactNativeAndLaunchBindings)
{
  const std::string sha(64, 'a');
  test::ScopedEnvironmentValue actor("NDNSF_DEPLOYMENT_ACTOR_ID", "provider-1");
  test::ScopedEnvironmentValue boot("NDNSF_DEPLOYMENT_BOOT_ID", "launch-123");
  test::ScopedEnvironmentValue digest("NDNSF_DEPLOYMENT_CONFIG_SHA256", sha.c_str());
  ReadinessOutput output;
  ndnsf::di::detail::emitDeploymentReadiness("/provider", "native-boot");
  const auto bytes = output.read();
  const auto record = ndnsf::di::NativeJson::parse(bytes);
  BOOST_CHECK_EQUAL(record.at("event").get<std::string>(), "NDNSF_DEPLOYMENT_READY");
  BOOST_CHECK_EQUAL(record.at("actor_id").get<std::string>(), "provider-1");
  BOOST_CHECK_EQUAL(record.at("boot_id").get<std::string>(), "launch-123");
  BOOST_CHECK_EQUAL(record.at("config_sha256").get<std::string>(), sha);
  BOOST_CHECK_EQUAL(record.at("identity").get<std::string>(), "/provider");
  BOOST_CHECK_EQUAL(record.at("native_boot_id").get<std::string>(), "native-boot");
  BOOST_CHECK_EQUAL(record.at("pid").get<int>(), ::getpid());
  BOOST_CHECK_EQUAL(std::count(bytes.begin(), bytes.end(), '\n'), 1);
}

BOOST_AUTO_TEST_CASE(PartialAndInvalidBindingsEmitNoReadyRecord)
{
  test::ScopedEnvironmentValue actor("NDNSF_DEPLOYMENT_ACTOR_ID", "provider-1");
  test::ScopedEnvironmentValue boot("NDNSF_DEPLOYMENT_BOOT_ID", nullptr);
  test::ScopedEnvironmentValue digest("NDNSF_DEPLOYMENT_CONFIG_SHA256", "invalid");
  ReadinessOutput output;
  BOOST_CHECK_THROW(ndnsf::di::detail::emitDeploymentReadiness("/provider", "native-boot"),
                    std::invalid_argument);
  const auto bytes = output.read();
  BOOST_CHECK(bytes.empty());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::tests
