#include "tests/boost-test.hpp"

#include "ndnsf-di/api.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using namespace ndnsf::di;

std::filesystem::path repositoryRoot()
{
  if (const char* value = std::getenv("SPEC185_REPO_ROOT"); value != nullptr && *value != '\0')
    return std::filesystem::absolute(value).lexically_normal();

  auto current = std::filesystem::current_path();
  for (auto candidate = current; !candidate.empty(); candidate = candidate.parent_path()) {
    if (std::filesystem::is_regular_file(candidate / "tests/standalone/run-spec182-native-unary-process.py"))
      return candidate;
    if (candidate == candidate.parent_path())
      break;
  }
  return current;
}

std::filesystem::path buildDirectory(const std::filesystem::path& root)
{
  if (const char* value = std::getenv("SPEC185_BUILD_DIR"); value != nullptr && *value != '\0')
    return std::filesystem::absolute(value).lexically_normal();
  return root / "build-spec185-b0c-normal";
}

// Cross-process Python drivers are lifecycle orchestration only.  Allow a
// sanitizer outer selector to use a separately qualified normal executable
// bundle for those external processes, while its in-process C++ selectors
// continue to resolve through SPEC185_BUILD_DIR.
std::filesystem::path externalBuildDirectory(const std::filesystem::path& root)
{
  if (const char* value = std::getenv("SPEC185_EXTERNAL_BUILD_DIR");
      value != nullptr && *value != '\0')
    return std::filesystem::absolute(value).lexically_normal();
  return buildDirectory(root);
}

std::string shellQuote(const std::string& value)
{
  std::string quoted("'");
  for (const char character : value) {
    if (character == '\'')
      quoted += "'\\''";
    else
      quoted += character;
  }
  quoted += '\'';
  return quoted;
}

std::string readFile(const std::filesystem::path& path)
{
  std::ifstream input(path);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

void requireMarker(const std::string& output, const std::string& marker,
                   const std::filesystem::path& log)
{
  BOOST_REQUIRE_MESSAGE(output.find(marker) != std::string::npos,
                        "missing native process marker '" + marker + "' in " + log.string());
}

void requireAbsentMarker(const std::string& output, const std::string& marker,
                         const std::filesystem::path& log)
{
  BOOST_REQUIRE_MESSAGE(output.find(marker) == std::string::npos,
                        "unexpected native process marker '" + marker + "' in " + log.string());
}

void requireLogMarker(const std::filesystem::path& runRoot, const std::string& name,
                      const std::string& marker)
{
  const auto log = runRoot / name;
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(log),
                        "missing native process log: " + log.string());
  requireMarker(readFile(log), marker, log);
}

void requireLogAbsentMarker(const std::filesystem::path& runRoot, const std::string& name,
                            const std::string& marker)
{
  const auto log = runRoot / name;
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(log),
                        "missing native process log: " + log.string());
  requireAbsentMarker(readFile(log), marker, log);
}

void requireProcessExit(const std::string& output, const std::string& label,
                        bool expectSuccess, const std::filesystem::path& driverLog)
{
  const std::string prefix = label + " rc ";
  std::optional<int> status;
  std::istringstream lines(output);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind(prefix, 0) != 0)
      continue;
    try {
      status = std::stoi(line.substr(prefix.size()));
    }
    catch (...) {
      BOOST_FAIL("invalid " + label + " exit status in " + driverLog.string());
    }
    break;
  }
  BOOST_REQUIRE_MESSAGE(status.has_value(),
                        "missing " + label + " exit status in " + driverLog.string());
  const bool normalSuccess = status.value() == 0;
  const bool normalFailure = status.value() > 0;
  BOOST_REQUIRE_MESSAGE(expectSuccess ? normalSuccess : normalFailure,
                        label + " exit status was " + std::to_string(status.value()) +
                        "; expected a normal " + (expectSuccess ? "zero" :
                                                    "positive nonzero") +
                        " exit in " + driverLog.string());
}

void requireStreamProcessOracle(const std::filesystem::path& runRoot,
                                const std::vector<std::string>& arguments)
{
  const auto has = [&arguments] (const std::string& value) {
    return std::find(arguments.begin(), arguments.end(), value) != arguments.end();
  };
  requireLogMarker(runRoot, "requester.log", "NATIVE_STREAM_ORACLE_PASS");
  requireLogMarker(runRoot, "requester.log", "NATIVE_REQUEST_SUCCEEDED");
  requireLogMarker(runRoot, "provider.log", "NDNSF_DI_GRANT_VERIFICATION");
  requireLogMarker(runRoot, "provider.log", "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED");

  if (has("--conversation")) {
    requireLogMarker(runRoot, "requester.log", "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN");
    requireLogMarker(runRoot, "requester-second.log", has("--recovery")
                       ? "NATIVE_STREAM_FAILED" : "NATIVE_REQUEST_SUCCEEDED");
    requireLogMarker(runRoot, "requester-wrong-parent.log",
                     "DI_NATIVE_CONVERSATION_PARENT_MISMATCH");
    if (has("--recovery")) {
      requireLogMarker(runRoot, "provider-restart.log", "NDNSF_DI_NATIVE_PROVIDER_READY");
      requireLogMarker(runRoot, "provider-restart.log", "PROVIDER_CONVERSATION_STATE_MISSING");
      requireLogAbsentMarker(runRoot, "requester-second.log", "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN");
      requireLogAbsentMarker(runRoot, "requester-second.log", "NATIVE_REQUEST_SUCCEEDED");
      requireLogAbsentMarker(runRoot, "provider-restart.log", "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED");
      requireLogAbsentMarker(runRoot, "provider-restart.log", "STREAM_EVENT_OBSERVED");
    }
    else {
      requireLogAbsentMarker(runRoot, "requester-wrong-parent.log", "NATIVE_REQUEST_SUCCEEDED");
      requireLogAbsentMarker(runRoot, "requester-wrong-parent.log",
                             "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN");
    }
  }
}

void requireReplacementProcessOracle(const std::filesystem::path& runRoot,
                                     const std::vector<std::string>& arguments)
{
  const bool noBackup = std::find(arguments.begin(), arguments.end(),
                                  "--replacement-no-backup") != arguments.end();
  requireLogAbsentMarker(runRoot, "provider.log", "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED");
  if (noBackup) {
    requireLogMarker(runRoot, "requester.log", "NATIVE_REQUEST_STAGE_FAILED");
    requireLogMarker(runRoot, "requester.log", "DI_NATIVE_NO_ADMITTED_PROVIDER");
    requireLogAbsentMarker(runRoot, "requester.log", "NATIVE_REQUEST_SUCCEEDED");
  }
  else {
    requireLogMarker(runRoot, "requester.log", "NATIVE_STREAM_ORACLE_PASS");
    requireLogMarker(runRoot, "requester.log", "NATIVE_REQUEST_SUCCEEDED");
    requireLogMarker(runRoot, "provider-b.log", "NDNSF_DI_GRANT_VERIFICATION");
    requireLogMarker(runRoot, "provider-b.log", "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED");
    const auto providerB = readFile(runRoot / "provider-b.log");
    BOOST_REQUIRE_MESSAGE(providerB.find("attempt-2") != std::string::npos ||
                            providerB.find("\"attemptEpoch\":\"2\"") != std::string::npos,
                          "replacement Provider did not expose attempt 2 in " +
                          (runRoot / "provider-b.log").string());
  }
}

int runProcessCase(const std::string& name, const std::vector<std::string>& arguments,
                   const std::vector<std::string>&,
                   bool requireRevocationQualification = false)
{
  const auto root = repositoryRoot();
  const auto build = buildDirectory(root);
  const auto externalBuild = externalBuildDirectory(root);
  const auto script = root / (std::string("tests/standalone/run-spec182-native-") +
                              (name == "unary" ? "unary" : "stream") + "-process.py");
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(script),
                        "missing process orchestrator: " + script.string());
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_directory(build),
                        "missing native candidate build: " + build.string());
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_directory(externalBuild),
                        "missing external process build: " + externalBuild.string());

  static unsigned sequence = 0;
  const auto runRoot = std::filesystem::temp_directory_path() /
    ("spec185-b7-" + name + "-" + std::to_string(static_cast<unsigned long>(::getpid())) +
     "-" + std::to_string(++sequence));
  std::filesystem::create_directories(runRoot);
  const auto log = runRoot / "cpp-driver.log";
  std::string libraryPath = externalBuild.string();
  if (const char* inherited = std::getenv("LD_LIBRARY_PATH");
      inherited != nullptr && *inherited != '\0') {
    libraryPath += ":";
    libraryPath += inherited;
  }
  std::string command = "env LD_LIBRARY_PATH=" + shellQuote(libraryPath) +
    " SPEC185_BUILD_DIR=" + shellQuote(externalBuild.string()) +
    " /usr/bin/timeout --kill-after=5s 180s python3 " +
    shellQuote(script.string()) +
    " --build " + shellQuote(externalBuild.string()) +
    " --run-root " + shellQuote(runRoot.string());
  for (const auto& argument : arguments)
    command += " " + argument;
  command += " > " + shellQuote(log.string()) + " 2>&1";

  const int status = std::system(command.c_str());
#if defined(__unix__) || defined(__APPLE__)
  const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
#else
  const int exitCode = status;
#endif
  BOOST_REQUIRE_MESSAGE(exitCode == 0,
                        "native process orchestrator failed for " + name +
                        " with exit code " + std::to_string(exitCode) +
                        "; see " + log.string());
  const auto output = readFile(log);
  if (name == "unary") {
    const bool revoked = std::find(arguments.begin(), arguments.end(), "--revoke") !=
      arguments.end();
    requireProcessExit(output, "requester", !revoked, log);
    if (revoked)
      requireProcessExit(output, "requester-baseline", true, log);
    if (!revoked) {
      requireLogMarker(runRoot, "requester.log", "NATIVE_NUMERICAL_ORACLE_PASS");
      requireLogMarker(runRoot, "requester.log", "NATIVE_REQUEST_SUCCEEDED");
      requireLogMarker(runRoot, "provider.log", "NDNSF_DI_GRANT_VERIFICATION");
      requireLogMarker(runRoot, "provider.log", "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED");
    }
  }
  else if (std::find(arguments.begin(), arguments.end(), "--replacement") != arguments.end()) {
    const bool noBackup = std::find(arguments.begin(), arguments.end(),
                                    "--replacement-no-backup") != arguments.end();
    requireProcessExit(output, "requester", !noBackup, log);
    requireReplacementProcessOracle(runRoot, arguments);
  }
  else {
    requireProcessExit(output, "requester", true, log);
    if (std::find(arguments.begin(), arguments.end(), "--conversation") != arguments.end()) {
      requireProcessExit(output, "requester-second",
                         std::find(arguments.begin(), arguments.end(), "--recovery") ==
                           arguments.end(), log);
      requireProcessExit(output, "requester-wrong-parent", false, log);
    }
    requireStreamProcessOracle(runRoot, arguments);
  }
  if (requireRevocationQualification) {
    requireMarker(output, "NATIVE_REQUEST_REVOKE_ORCHESTRATION_COMPLETE", log);
    const auto baselineRequesterLog = runRoot / "requester-baseline.log";
    const auto controllerLog = runRoot / "controller.log";
    const auto requesterLog = runRoot / "requester.log";
    const auto providerLog = runRoot / "provider.log";
    const auto boundaryFile = runRoot / "provider-baseline-boundary";
    const auto baselineRequester = readFile(baselineRequesterLog);
    const auto controller = readFile(controllerLog);
    const auto requester = readFile(requesterLog);
    const auto provider = readFile(providerLog);
    requireMarker(baselineRequester, "NATIVE_REQUEST_SUCCEEDED", baselineRequesterLog);
    requireMarker(controller, "NDNSF_REVOCATION_APPLIED success=1", controllerLog);
    requireMarker(requester, "DI_NATIVE_NO_ADMITTED_PROVIDER", requesterLog);
    requireAbsentMarker(requester, "NATIVE_REQUEST_SUCCEEDED", requesterLog);
    BOOST_REQUIRE_MESSAGE(requester.find("NATIVE_REQUEST_STAGE_FAILED") != std::string::npos ||
                            requester.find("NATIVE_REQUESTER_FAILED") != std::string::npos,
                          "revoked requester did not reach a native failure terminal in " +
                          requesterLog.string());
    BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(boundaryFile),
                          "missing Provider baseline log boundary under " + runRoot.string());
    std::size_t consumed = 0;
    std::uintmax_t boundary = 0;
    try {
      boundary = std::stoull(readFile(boundaryFile), &consumed);
    }
    catch (...) {
      BOOST_FAIL("invalid Provider baseline log boundary under " + runRoot.string());
    }
    BOOST_REQUIRE_MESSAGE(consumed > 0 && boundary <= provider.size(),
                          "Provider baseline log boundary is outside the retained log under " +
                          runRoot.string());
    const auto baselineProvider = provider.substr(0, static_cast<std::size_t>(boundary));
    const auto postRevocationProvider = provider.substr(static_cast<std::size_t>(boundary));
    // The Provider's native execution event is the qualification boundary.
    // Handler timing is an optional diagnostic and is not emitted by every
    // valid provider path.
    const std::string executionMarker = "event=PROVIDER_EXECUTE_DONE ";
    const auto countMarker = [&executionMarker] (const std::string& text) {
      std::size_t count = 0;
      std::size_t offset = 0;
      while ((offset = text.find(executionMarker, offset)) != std::string::npos) {
        ++count;
        offset += executionMarker.size();
      }
      return count;
    };
    const auto baselineExecutions = countMarker(baselineProvider);
    const auto postRevocationExecutions = countMarker(postRevocationProvider);
    BOOST_REQUIRE_MESSAGE(baselineExecutions > 0,
                          "baseline Provider log has no native execution marker under " +
                          runRoot.string());
    BOOST_REQUIRE_MESSAGE(postRevocationExecutions == 0,
                          "Provider execution marker appeared after revocation under " +
                          runRoot.string());
  }
  return exitCode;
}

void runNativeSelector(const std::string& target, const std::string& filter,
                       const std::vector<std::string>& cases)
{
  const auto root = repositoryRoot();
  const auto build = buildDirectory(root);
  const auto executable = build / target;
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(executable),
                        "missing native selector: " + executable.string());

  static unsigned selectorSequence = 0;
  const auto log = std::filesystem::temp_directory_path() /
    ("spec185-b7-selector-" + target + "-" +
     std::to_string(static_cast<unsigned long>(::getpid())) + "-" +
     std::to_string(++selectorSequence) + ".log");
  std::string libraryPath = build.string();
  if (const char* inherited = std::getenv("LD_LIBRARY_PATH");
      inherited != nullptr && *inherited != '\0') {
    libraryPath += ":";
    libraryPath += inherited;
  }
  std::string command = "env LD_LIBRARY_PATH=" + shellQuote(libraryPath) +
    " /usr/bin/timeout --kill-after=5s 180s " +
    shellQuote(executable.string()) + " --log_level=test_suite";
  if (!filter.empty())
    command += " --run_test=" + shellQuote(filter);
  command += " > " + shellQuote(log.string()) + " 2>&1";
  const int status = std::system(command.c_str());
#if defined(__unix__) || defined(__APPLE__)
  const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
#else
  const int exitCode = status;
#endif
  BOOST_REQUIRE_MESSAGE(exitCode == 0,
                        "native selector failed for " + target + "; see " + log.string());
  const auto output = readFile(log);
  requireMarker(output, "*** No errors detected", log);
  for (const auto& testCase : cases)
    requireMarker(output, testCase, log);
}

// Keep each C-04 case in its own process.  A wildcard selector would execute
// the entire suite and let an earlier long provider/conversation fixture
// consume the later case's lifecycle budget.  The case still runs twice, but
// its timing and failure boundary are independent and the marker is exact.
void runNativeCasesTwice(const std::string& target, const std::string& suite,
                         const std::vector<std::string>& cases)
{
  for (const auto& testCase : cases) {
    const auto filter = suite + "/" + testCase;
    runNativeSelector(target, filter, {testCase});
    runNativeSelector(target, filter, {testCase});
  }
}

void runTwice(const std::string& name, const std::vector<std::string>& arguments,
              const std::vector<std::string>& markers,
              bool requireRevocationQualification = false)
{
  runProcessCase(name, arguments, markers, requireRevocationQualification);
  runProcessCase(name, arguments, markers, requireRevocationQualification);
}

BOOST_AUTO_TEST_SUITE(Spec185Process)

BOOST_AUTO_TEST_CASE(NativeRuntimeConfigurationRemainsCxxOwned)
{
  RuntimeConfig config;
  config.nativeConfigPath =
    (std::filesystem::temp_directory_path() / "spec185-b7-missing-runtime.json").string();
  BOOST_CHECK_EXCEPTION(
    Runtime::open(config), DiError,
    [] (const DiError& error) {
      return error.code() == "INVALID_RUNTIME_CONFIGURATION" &&
             error.boundary() == "configuration";
    });
}

BOOST_AUTO_TEST_CASE(UnaryAndStreamProcessesUseNativeOracles)
{
  runTwice("unary", {}, {
    "NATIVE_NUMERICAL_ORACLE_PASS",
    "NATIVE_REQUEST_SUCCEEDED",
    "NDNSF_DI_GRANT_VERIFICATION",
    "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED"});
  runTwice("stream", {}, {
    "NATIVE_STREAM_ORACLE_PASS",
    "NATIVE_REQUEST_SUCCEEDED",
    "NDNSF_DI_GRANT_VERIFICATION",
    "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED"});
}

BOOST_AUTO_TEST_CASE(ConversationRecoveryAndReplacementRemainTerminal)
{
  runTwice("stream", {"--conversation"}, {
    "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN",
    "requester-second rc 0",
    "DI_NATIVE_CONVERSATION_PARENT_MISMATCH"});
  runTwice("stream", {"--conversation", "--recovery"}, {
    "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN",
    "NATIVE_STREAM_FAILED",
    "PROVIDER_CONVERSATION_STATE_MISSING"});
  runTwice("stream", {"--replacement"}, {
    "NATIVE_STREAM_ORACLE_PASS",
    "NATIVE_REQUEST_SUCCEEDED",
    "attempt-2"});
  runTwice("stream", {"--replacement", "--replacement-no-backup"}, {
    "NATIVE_REQUEST_STAGE_FAILED",
    "DI_NATIVE_NO_ADMITTED_PROVIDER"});
}

BOOST_AUTO_TEST_CASE(NativeNegativeCacheDeadlineRevokeAndCleanupMatrix)
{
  // These selectors keep the business oracle in C++ and are invoked here as
  // native child processes.  The process scripts above supply only external
  // NFD/PIB/TPM lifecycle; they do not replace these C++ assertions.
  runNativeCasesTwice("spec185-prepared-request", "Spec185PreparedRequest", {
    "StreamingRequestReaderPreservesTerminalEventAcrossCancel",
    "PreparedRequestRejectsUnsupportedTextBeforeNativeSubmission",
    "PreparedRequestRejectsUnsupportedGenerationContract",
    "PreparedRequestValidatesProtectedReferenceMetadata",
    "PreparedRequestCompletesThroughCoreCollaborationFixture",
    "PreparedRequestCompletesThroughServedProvider",
    "PreparedConversationCommitsTwoNativeTurns",
    "RuntimeDrainAsyncTracksMultiplePreparedClients",
    "RuntimeDrainAsyncWakesAfterLastClientTimerRetires",
    "RuntimeDrainAsyncIncludesNativeClientWork"});
  runNativeCasesTwice("spec185-provider-assembly", "Spec185ProviderAssembly", {
    "AuthenticatedSelectionRunsProviderAssemblyRunnerAndResponse",
    "ProductionAssemblerFetchesCanonicalSourceAfterSelection",
    "ProviderArtifactCachePinsEvictsAndSeparatesIdentity",
    "ProtectedArtifactCacheColdHitBindsGrantAndRetainsCiphertext",
    "ProductionAssemblerCacheColdHitUsesExactArtifact",
    "ProtectedSelectionBindingRejectsProviderEpochAndGrantSubstitution",
    "ProviderOnlyRuntimeServesAndDrainsNativeRegistration"});
  // Controller revocation and deadline/cleanup cases are already registered
  // in the complete C++ integration target; run their named cases through a
  // bounded native child so B7 records them in this matrix as well.
  runNativeCasesTwice("integration-tests", "ControllerRevocationFlow", {
    "RealControllerStatusRevokesEveryTargetKindAtEveryCutPoint"});
  runNativeCasesTwice("integration-tests", "Spec170NdnsfDiCoreFlow", {
    "Spec175NativeTinyOnnxExpiredDeadlineCleansProviderState"});
}

BOOST_AUTO_TEST_CASE(RealCrossProcessRevokeFailsClosed)
{
  runTwice("unary", {"--revoke"}, {
    "NATIVE_REQUEST_REVOKE_ORCHESTRATION_COMPLETE"}, true);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
