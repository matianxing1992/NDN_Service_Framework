#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/vendor/nlohmann/json.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace ndn_service_framework::tests {
namespace {

struct QueryResult
{
  int status;
  std::string output;
};

QueryResult
runQuery(const std::string& count, const std::string& scenario = "nominal",
         const std::string& order = "PCI_BUS_ID", bool missingBinding = false)
{
  const auto* binary = std::getenv("NDNSF_DEVICE_QUERY_EXECUTABLE");
  const auto* fixture = std::getenv("NDNSF_DEVICE_QUERY_FIXTURE_DIR");
  if (!binary || !fixture || !std::filesystem::path(binary).is_absolute() ||
      !std::filesystem::path(fixture).is_absolute()) {
    throw std::runtime_error("declare absolute device-query executable and fixture directory");
  }
  int descriptors[2];
  if (::pipe(descriptors) != 0) {
    throw std::runtime_error("device-query fixture pipe failed");
  }
  const auto child = ::fork();
  if (child < 0) {
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    throw std::runtime_error("device-query fixture fork failed");
  }
  if (child == 0) {
    ::close(descriptors[0]);
    std::signal(SIGALRM, SIG_DFL);
    ::alarm(5);
    if (::dup2(descriptors[1], STDOUT_FILENO) < 0 ||
        ::dup2(descriptors[1], STDERR_FILENO) < 0) {
      ::_exit(126);
    }
    ::close(descriptors[1]);
    const auto put = [](const char* key, const char* value) {
      if (::setenv(key, value, 1) != 0) {
        ::_exit(126);
      }
    };
    put("LD_LIBRARY_PATH", fixture);
    put("SPEC217_GPU_FIXTURE_SCENARIO", scenario.c_str());
    put("CUDA_DEVICE_ORDER", order.c_str());
    put("CUDA_VISIBLE_DEVICES", "0,1");
    put("NDNSF_DEPLOYMENT_BOOT_ID", "test-boot");
    put("NDNSF_DEPLOYMENT_CONFIG_SHA256", "test-config");
    put("NDNSF_DEPLOYMENT_RELEASE_SHA256", "test-release");
    put("NDNSF_DEPLOYMENT_ACTOR_ID", "gpu-probe.test-node");
    put("SLURM_JOB_ID", "12345");
    put("SLURM_STEP_ID", "3");
    put("SLURM_STEP_GPUS", "2,6");
    if (missingBinding) {
      ::unsetenv("NDNSF_DEPLOYMENT_BOOT_ID");
    }
    ::execl(binary, binary, "--count", count.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  ::close(descriptors[1]);
  QueryResult result{};
  std::array<char, 1024> buffer;
  ssize_t size;
  while ((size = ::read(descriptors[0], buffer.data(), buffer.size())) > 0) {
    result.output.append(buffer.data(), static_cast<std::size_t>(size));
    if (result.output.size() > 65536) {
      ::kill(child, SIGKILL);
      break;
    }
  }
  ::close(descriptors[0]);
  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      throw std::runtime_error("device-query fixture wait failed");
    }
  }
  result.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  return result;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec217DeviceQuery)

BOOST_AUTO_TEST_CASE(VisibleCudaOrderDiffersFromNvmlIndices)
{
  const auto result = runQuery("2");
  BOOST_REQUIRE_EQUAL(result.status, 0);
  const auto record = nlohmann::json::parse(result.output);
  BOOST_CHECK_EQUAL(record.at("schema"), "ndnsf-di-gpu-probe-v1");
  BOOST_CHECK_EQUAL(record.at("job_id"), "12345");
  BOOST_CHECK_EQUAL(record.at("step_gpus"), "2,6");
  BOOST_CHECK_EQUAL(record.at("device_order"), "PCI_BUS_ID");
  BOOST_REQUIRE_EQUAL(record.at("visible_uuids").size(), 2);
  BOOST_CHECK_EQUAL(record.at("visible_uuids").at(0),
                    "GPU-33333333-3333-3333-3333-333333333333");
  BOOST_CHECK_EQUAL(record.at("visible_uuids").at(1),
                    "GPU-11111111-1111-1111-1111-111111111111");
  BOOST_CHECK_EQUAL(record.at("inventory").at(3).at("physical_id"), "3");
  BOOST_CHECK_EQUAL(record.at("inventory").at(3).at("uuid"), record.at("visible_uuids").at(0));
}

BOOST_AUTO_TEST_CASE(RejectsInvalidCountsBeforeGpuAccess)
{
  for (const auto* count : {"0", "65", "-1", "01", "2x", "999999999999999999999"}) {
    const auto result = runQuery(count);
    BOOST_CHECK_EQUAL(result.status, 1);
    BOOST_CHECK_NE(result.output.find("GPU_QUERY_COUNT_INVALID"), std::string::npos);
  }
}

BOOST_AUTO_TEST_CASE(RejectsUnboundOrAmbiguousObservation)
{
  for (const auto* scenario : {"count-mismatch", "duplicate-visible", "missing-inventory"}) {
    BOOST_CHECK_EQUAL(runQuery("2", scenario).status, 1);
  }
  BOOST_CHECK_EQUAL(runQuery("2", "nominal", "FASTEST_FIRST").status, 1);
  BOOST_CHECK_EQUAL(runQuery("2", "nominal", "PCI_BUS_ID", true).status, 1);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::tests
