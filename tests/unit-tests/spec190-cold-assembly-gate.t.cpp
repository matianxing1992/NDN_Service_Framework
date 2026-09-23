#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/file.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace ndnsf::di {
namespace {

NativeSelectionProjectionV3
testProjection()
{
  NativeSelectionProjectionV3 projection;
  projection.requestId = "spec190-cold-assembly-gate";
  projection.provider = "provider-a";
  projection.assembly.selectedRole = "role-a";
  projection.assembly.protectionEpoch = "plaintext-v1";
  projection.assembly.maxSourceBytes = 1024 * 1024;
  projection.assembly.maxAssembledBytes = 1024 * 1024;
  projection.assembly.maxNodes = 16;
  return projection;
}

NativeCanonicalOnnxAssemblerOptions
testOptions(const std::filesystem::path& cacheDir,
            const std::filesystem::path& lockPath,
            int reportFd)
{
  NativeCanonicalOnnxAssemblerOptions options;
  options.cacheDir = cacheDir.string();
  options.coldAssemblyLockPath = lockPath;
  options.providerIdentity = "provider-a";
  options.assemblyTimeoutMs = 250;
  options.workerLocation.path = "/definitely/missing/assembly-worker";
  options.signManifest = [] (const std::string&) {
    return std::string("test-signature");
  };
  const auto report = [reportFd] (char value) {
    (void)::write(reportFd, &value, 1);
  };
  options.reportProgress = [report] (const std::string& phase, double) {
    if (phase == "COLD_ASSEMBLY_WAIT") report('W');
    if (phase == "COLD_ASSEMBLY_ENTERED") report('E');
  };
  return options;
}

NativeCanonicalOnnxFetchers
testFetchers(int reportFd)
{
  NativeCanonicalOnnxFetchers fetchers;
  const auto report = [reportFd] (char value) {
    (void)::write(reportFd, &value, 1);
  };
  fetchers.getArtifact = [report] (const ndn::Name&) {
    report('F');
    return std::optional<ndn::Buffer>{};
  };
  fetchers.fetchEncryptedLargeData = [report] (const ndn::Name&, const ndn::Name&) {
    report('F');
    return std::optional<ndn::Buffer>{};
  };
  return fetchers;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec190ColdAssemblyGate)

BOOST_AUTO_TEST_CASE(CrossProcessAdmissionTimesOutBeforeMaterialFetch)
{
  const auto root = std::filesystem::temp_directory_path() /
    ("spec190-cold-assembly-gate-" + std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  const auto cacheDir = root / "cache";
  const auto lockPath = root / "cold-assembly.lock";

  const int holder = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                            0600);
  BOOST_REQUIRE(holder >= 0);
  BOOST_REQUIRE_EQUAL(::flock(holder, LOCK_EX | LOCK_NB), 0);

  int reportPipe[2] = {-1, -1};
  BOOST_REQUIRE_EQUAL(::pipe(reportPipe), 0);
  const auto projection = testProjection();
  const auto options = testOptions(cacheDir, lockPath, reportPipe[1]);
  const auto fetchers = testFetchers(reportPipe[1]);
  const auto child = ::fork();
  BOOST_REQUIRE(child >= 0);
  if (child == 0) {
    // Do not inherit the holder descriptor: the parent must be the sole lock
    // owner while this independent Provider-like process waits for admission.
    ::close(holder);
    char result = 'U';
    try {
      (void)prepareNativeCanonicalOnnxRole(fetchers, projection, options);
    }
    catch (const std::exception& error) {
      result = std::string(error.what()) ==
        "DI_NATIVE_COLD_ASSEMBLY_ADMISSION_TIMEOUT" ? 'T' : 'X';
    }
    (void)::write(reportPipe[1], &result, 1);
    ::close(reportPipe[1]);
    ::close(reportPipe[0]);
    ::_exit(result == 'T' ? 0 : 2);
  }

  ::close(reportPipe[1]);
  int status = 0;
  const auto waitDeadline = std::chrono::steady_clock::now() +
    std::chrono::seconds(5);
  bool reaped = false;
  while (std::chrono::steady_clock::now() < waitDeadline) {
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      reaped = true;
      break;
    }
    BOOST_REQUIRE_MESSAGE(waited == 0, "waitpid failed while watching child");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!reaped) {
    (void)::kill(child, SIGKILL);
    (void)::waitpid(child, &status, 0);
  }
  BOOST_REQUIRE_MESSAGE(reaped, "cold assembly admission child did not terminate");
  BOOST_REQUIRE(WIFEXITED(status));
  BOOST_CHECK_EQUAL(WEXITSTATUS(status), 0);

  std::string reports;
  char buffer[32] = {};
  for (;;) {
    const auto count = ::read(reportPipe[0], buffer, sizeof(buffer));
    if (count <= 0) break;
    reports.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(reportPipe[0]);
  BOOST_CHECK(reports.find('T') != std::string::npos);
  BOOST_CHECK_EQUAL(reports.find('F'), std::string::npos);
  BOOST_CHECK_EQUAL(reports.find('E'), std::string::npos);

  BOOST_CHECK_EQUAL(::flock(holder, LOCK_UN), 0);
  ::close(holder);
  std::filesystem::remove_all(root, ignored);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di
