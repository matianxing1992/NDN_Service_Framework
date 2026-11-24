#include "tests/boost-test.hpp"

#include "ndnsf-di/api.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-capability.hpp"
#include "ndn-service-framework/ControllerGenerationStore.hpp"

#include <boost/test/unit_test.hpp>
#include <ndn-cxx/util/scope.hpp>
#include <openssl/crypto.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
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
  const char* value = std::getenv("SPEC185_REPO_ROOT");
  if (value == nullptr) value = std::getenv("NDNSF_TEST_SUPPORT_ROOT");
  std::filesystem::path root;
  if (value != nullptr) root = value;
#ifdef NDNSF_TEST_SOURCE_DIR
  else root = NDNSF_TEST_SOURCE_DIR;
#endif
  if (!root.is_absolute() || !std::filesystem::is_directory(root))
    throw std::runtime_error("native process test support root is missing or not absolute");
  return root;
}

std::filesystem::path buildDirectory(const std::filesystem::path&)
{
  const char* value = std::getenv("SPEC185_BUILD_DIR");
  if (value == nullptr) value = std::getenv("NDNSF_SPEC182_BIN_DIR");
  std::filesystem::path root;
  if (value != nullptr) root = value;
#ifdef NDNSF_TEST_BINARY_DIR
  else root = NDNSF_TEST_BINARY_DIR;
#endif
  if (!root.is_absolute() || !std::filesystem::is_directory(root))
    throw std::runtime_error("native process test binary root is missing or not absolute");
  return root;
}

// Cross-process Python drivers are lifecycle orchestration only.  Allow a
// sanitizer outer selector to use a separately qualified normal executable
// bundle for those external processes, while its in-process C++ selectors
// continue to resolve through SPEC185_BUILD_DIR.
std::filesystem::path externalBuildDirectory(const std::filesystem::path& root)
{
  if (const char* value = std::getenv("SPEC185_EXTERNAL_BUILD_DIR")) {
    const std::filesystem::path selected(value);
    if (!selected.is_absolute() || !std::filesystem::is_directory(selected))
      throw std::runtime_error("external native process binary root is missing or not absolute");
    return selected;
  }
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

void
requireStoredProcessOracle(const std::filesystem::path& runRoot, const std::string& output,
                           const std::filesystem::path& driverLog, bool sourcePreparation)
{
  requireProcessExit(output, "preparer", true, driverLog);
  requireProcessExit(output, "requester", true, driverLog);
  requireProcessExit(output, "requester-c", true, driverLog);
  requireLogMarker(runRoot, "preparer.log", sourcePreparation
    ? "ndnsf-di-native-preparation" : "ndnsf-di-stored-model-preparation");
  requireLogMarker(runRoot, "repo.log", "NDNSF_REPO_READY");
  requireLogMarker(runRoot, "repo-restart.log", "NDNSF_REPO_READY");
  requireLogMarker(runRoot, "controller-restart.log", "ServiceController started...");
  requireLogMarker(runRoot, "provider.log", "NDNSF_DI_GRANT_VERIFICATION");
  requireLogMarker(runRoot, "provider.log", "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED");
  const auto measurements = nativeParseJson(readFile(runRoot / "stored-process-measurements.json"));
  if (sourcePreparation) {
    requireProcessExit(output, "preparer-ready", true, driverLog);
    const auto& source = measurements.at("source_preparation");
    BOOST_CHECK_EQUAL(source.at("ready_exit_code").get<int>(), 0);
    BOOST_CHECK(source.at("source_absent").get<bool>());
    BOOST_CHECK(source.at("same_private_handle").get<bool>());
  }
  BOOST_REQUIRE(measurements.at("first_failure").is_null());
  BOOST_REQUIRE(measurements.at("forced_cleanup").empty());
  BOOST_REQUIRE(measurements.at("cleanup_errors").empty());
  BOOST_REQUIRE(measurements.at("live_processes").empty());
  BOOST_REQUIRE(measurements.at("preparer_offline").get<bool>());
  BOOST_REQUIRE(measurements.at("source_removed").get<bool>());
  BOOST_REQUIRE(measurements.at("credential_store_removed").get<bool>());
  const auto& restart = measurements.at("infrastructure_restart");
  BOOST_REQUIRE(restart.at("same_stores_and_credentials").get<bool>());
  BOOST_REQUIRE_EQUAL(restart.at("exits").size(), 3U);
  for (const auto& actor : restart.at("exits"))
    BOOST_REQUIRE_EQUAL(actor.at("exit_code").get<int>(), 0);
  {
    const auto owner = ndn::Name("/example/hello/controller").toUri(
      ndn::name::UriFormat::CANONICAL);
    ndn_service_framework::ControllerGenerationStore previous(
      runRoot / "shared/controller-before-restart.state");
    ndn_service_framework::ControllerGenerationStore current(
      runRoot / "shared/controller-generation.state");
    BOOST_REQUIRE(previous.acquireWriter(owner));
    BOOST_REQUIRE(current.acquireWriter(owner));
    const auto previousVersion = previous.load();
    const auto currentVersion = current.load();
    BOOST_REQUIRE(previousVersion.has_value());
    BOOST_REQUIRE(currentVersion.has_value());
    BOOST_CHECK(currentVersion->controllerGenerationTimestamp >
                previousVersion->controllerGenerationTimestamp);
    const auto previousMaterial = previous.loadAbeMaterial();
    const auto currentMaterial = current.loadAbeMaterial();
    BOOST_REQUIRE(previousMaterial.has_value());
    BOOST_REQUIRE(currentMaterial.has_value());
    BOOST_CHECK_EQUAL(currentMaterial->owner, owner);
    BOOST_CHECK_EQUAL(previousMaterial->owner, owner);
    BOOST_CHECK_EQUAL(previousMaterial->signingKeyDigest, currentMaterial->signingKeyDigest);
    BOOST_CHECK_EQUAL(previousMaterial->policyDigest, currentMaterial->policyDigest);
    // Assert booleans so a failure cannot print private key bytes.
    const bool samePublicParameters = !previousMaterial->publicParameters.empty() &&
      previousMaterial->publicParameters == currentMaterial->publicParameters;
    const bool sameMasterSecret = !previousMaterial->masterSecret.empty() &&
      previousMaterial->masterSecret == currentMaterial->masterSecret;
    BOOST_CHECK(samePublicParameters);
    BOOST_CHECK(sameMasterSecret);
  }
  BOOST_CHECK(!std::filesystem::exists(runRoot / "requester/model.onnx"));
  BOOST_CHECK(!std::filesystem::exists(runRoot / "requester-store"));
  BOOST_CHECK(!std::filesystem::exists(runRoot / "bootstrap-store"));
  requireMarker(readFile(runRoot / "nfd.conf"), "cs_max_packets 0", runRoot / "nfd.conf");

  const ndn::Name producer("/example/hello/user");
  const ndn::Name repoProvider("/example/hello/repo-provider");
  const ndn::Name repoNode("/example/hello/repo-node");
  BOOST_REQUIRE(repoProvider != repoNode);
  bool foundRepoRoute = false;
  std::istringstream routes(readFile(runRoot / "routes-after-preparer.txt"));
  std::string token;
  while (routes >> token) {
    if (token.rfind("prefix=", 0) != 0)
      continue;
    const ndn::Name prefix(token.substr(7));
    BOOST_CHECK(!prefix.empty());
    BOOST_CHECK(!producer.isPrefixOf(prefix));
    foundRepoRoute = foundRepoRoute || prefix == repoProvider;
  }
  BOOST_REQUIRE(foundRepoRoute);

  const auto firstHandle = nativeLoadStoredModelHandle(runRoot / "consumer-b/stored-handle.json");
  BOOST_CHECK(firstHandle.producer == producer);
  BOOST_CHECK(firstHandle.repository == repoNode);
  const ndn::Name packageStem(
    "/example/hello/user/NDNSF-DI/model/qwen/unspecified/unspecified/revision/pinned-r1/stored/rep");
  BOOST_REQUIRE_EQUAL(firstHandle.rootName.size(), packageStem.size() + 2);
  BOOST_CHECK(packageStem.isPrefixOf(firstHandle.rootName));
  BOOST_CHECK_EQUAL(firstHandle.rootName[-2].type(), ndn::tlv::GenericNameComponent);
  BOOST_CHECK_EQUAL(firstHandle.rootName[-2].value_size(), 32U);
  BOOST_CHECK(firstHandle.rootName[-1] == ndn::name::Component("root"));
  std::optional<std::string> modelContract;
  for (const auto& name : {std::string("consumer-b"), std::string("consumer-c")}) {
    const auto directory = runRoot / name;
    const auto log = name == "consumer-b" ? "requester.log" : "requester-c.log";
    requireLogMarker(runRoot, log, "NATIVE_STREAM_ORACLE_PASS");
    requireLogMarker(runRoot, log, "NATIVE_REQUEST_SUCCEEDED");
    requireLogMarker(runRoot, log, "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN");
    requireLogAbsentMarker(runRoot, log, "ndnsf-di-stored-model-preparation");
    BOOST_REQUIRE(std::filesystem::file_size(directory / "output.bin") > 0);
    const auto handle = nativeLoadStoredModelHandle(directory / "stored-handle.json");
    BOOST_CHECK(handle.producer == firstHandle.producer);
    BOOST_CHECK(handle.repository == firstHandle.repository);
    BOOST_CHECK(handle.rootName == firstHandle.rootName);
    BOOST_CHECK(handle.rootWireDigest == firstHandle.rootWireDigest);
    BOOST_CHECK(handle.masterKey == firstHandle.masterKey);

    std::ifstream input(directory / "keys/conversation.key", std::ios::binary);
    std::vector<NativeConversationJournalKey> journalKeys{{"active",
      std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {})}};
    std::vector<std::vector<std::uint8_t>> authenticationKeys;
    const auto eraseKeys = ndn::make_scope_exit([&] {
      for (auto& key : journalKeys)
        OPENSSL_cleanse(key.bytes.data(), key.bytes.size());
      for (auto& key : authenticationKeys)
        OPENSSL_cleanse(key.data(), key.size());
    });
    authenticationKeys = nativeConversationAuthenticationKeys(name, journalKeys);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    const auto checkpoint = nativeReadConversationCheckpoint(
      readFile(directory / "conversation-state.json"), authenticationKeys,
      static_cast<std::uint64_t>(now));
    BOOST_CHECK(ndn::Name(checkpoint.at("requesterIdentity").get<std::string>()) ==
                ndn::Name("/example/hello/" + name));
    BOOST_CHECK_EQUAL(checkpoint.at("conversationId").get<std::string>(), "spec222-stored-" + name);
    BOOST_CHECK_EQUAL(checkpoint.at("prefixTokenCount").get<std::uint64_t>(), 9U);
    BOOST_CHECK_EQUAL(checkpoint.at("roleReceiptDigests").size(), 1U);
    if (modelContract)
      BOOST_CHECK_EQUAL(checkpoint.at("modelContractDigest").get<std::string>(), *modelContract);
    modelContract = checkpoint.at("modelContractDigest").get<std::string>();
  }
}

void
requireHostStreamProcessOracle(const std::filesystem::path& runRoot, bool cancelled)
{
  const auto receipt = nativeParseJson(readFile(runRoot / "requester/host-result.json"));
  BOOST_REQUIRE(receipt.at("first_failure").is_null());
  BOOST_REQUIRE(receipt.at("cleanup_errors").empty());
  BOOST_REQUIRE(receipt.at("drained").get<bool>());
  BOOST_CHECK_EQUAL(receipt.at("status").get<std::string>(),
                    cancelled ? "CANCELLED" : "SUCCEEDED");
  const std::vector<std::int64_t> expected{4, 5, 6, 7, 8, 9, 10, 2};
  const auto& events = receipt.at("events");
  BOOST_REQUIRE_EQUAL(events.size(), cancelled ? 3U : expected.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    BOOST_CHECK_EQUAL(events[i].at("schema").get<std::string>(), "GenerationTokenEventV1");
    BOOST_CHECK_EQUAL(events[i].at("tokenEpoch").get<std::uint64_t>(), i + 1);
    BOOST_CHECK_EQUAL(events[i].at("tokenId").get<std::int64_t>(), expected[i]);
  }
  if (cancelled) {
    BOOST_REQUIRE(receipt.at("completed").empty());
    BOOST_REQUIRE_EQUAL(receipt.at("errors").size(), 1U);
    BOOST_CHECK_EQUAL(receipt.at("errors")[0].at("code").get<std::string>(), "CANCELLED");
    requireLogMarker(runRoot, "requester.log",
      "NDNSF_DI_NATIVE_CANCELLATION_RECEIPT");
    requireLogMarker(runRoot, "requester.log", "expected=4 fenced=4 drained=4");
  }
  else {
    BOOST_REQUIRE(receipt.at("errors").empty());
    BOOST_CHECK_EQUAL(receipt.at("terminal_events").get<unsigned>(), 1U);
    BOOST_REQUIRE_EQUAL(receipt.at("completed").size(), 1U);
    const auto& final = receipt.at("completed")[0];
    BOOST_CHECK_EQUAL(final.at("schema").get<std::string>(), "NDNSF-DI-FINAL-V1");
    BOOST_CHECK(final.at("tokenIds").get<std::vector<std::int64_t>>() == expected);
  }
  for (std::size_t i = 0; i < 4; ++i) {
    const auto name = i == 0 ? std::string("provider") : "provider-" + std::to_string(i);
    requireLogMarker(runRoot, name + ".log", "NDNSF_DI_GRANT_VERIFICATION");
    requireLogMarker(runRoot, name + ".log", "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED");
    if (!cancelled) {
      const auto log = readFile(runRoot / (name + ".log"));
      const auto role = "phase=role_done role=/LLM/Pipeline/Stage/" + std::to_string(i) + " epoch=";
      std::size_t count = 0;
      for (auto offset = log.find(role); offset != std::string::npos;
           offset = log.find(role, offset + role.size()))
        ++count;
      BOOST_REQUIRE_EQUAL(count, expected.size());
      for (std::size_t epoch = 0; epoch < expected.size(); ++epoch)
        requireMarker(log, role + std::to_string(epoch), runRoot / (name + ".log"));
    }
  }
  const auto cleanup = nativeParseJson(readFile(runRoot / "four-stage-process-measurements.json"));
  BOOST_REQUIRE(cleanup.at("first_failure").is_null());
  BOOST_REQUIRE(cleanup.at("forced_cleanup").empty());
  BOOST_REQUIRE(cleanup.at("cleanup_errors").empty());
  BOOST_REQUIRE(cleanup.at("live_processes").empty());
  BOOST_REQUIRE_EQUAL(cleanup.at("actor_exit_codes").size(), 8U);
  for (const auto& actor : cleanup.at("actor_exit_codes").items())
    BOOST_CHECK_EQUAL(actor.value().get<int>(), 0);

  const auto* runtime = std::getenv("NDNSF_TEST_RUNTIME_PREFIX");
  const auto* site = std::getenv("NDNSF_TEST_PYTHON_SITE");
  BOOST_REQUIRE(runtime != nullptr);
  BOOST_REQUIRE(site != nullptr);
  const auto prefix = std::filesystem::canonical(runtime);
  const auto pythonSite = std::filesystem::canonical(site).string() + "/";
  for (const auto& module : receipt.at("modules").items())
    BOOST_CHECK_EQUAL(module.value().get<std::string>().find(pythonSite), 0U);
  bool coreFound = false;
  bool diFound = false;
  for (const auto& item : receipt.at("libraries")) {
    const auto library = std::filesystem::canonical(item.get<std::string>());
    BOOST_REQUIRE(library.parent_path() == prefix / "lib");
    coreFound = coreFound || library.filename().string().find("libndn-service-framework.so") == 0;
    diFound = diFound || library.filename().string() == "libndnsf-distributed-inference.so";
    BOOST_CHECK_EQUAL(nativePlanningDigest(readFile(library)),
      "sha256:" + receipt.at("library_sha256").at(item.get<std::string>()).get<std::string>());
  }
  BOOST_CHECK(coreFound);
  BOOST_CHECK(diFound);
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
  if (std::find(arguments.begin(), arguments.end(), "--host-requester") != arguments.end()) {
    const auto* runtime = std::getenv("NDNSF_TEST_RUNTIME_PREFIX");
    const auto* site = std::getenv("NDNSF_TEST_PYTHON_SITE");
    BOOST_REQUIRE(runtime != nullptr);
    BOOST_REQUIRE(site != nullptr);
    BOOST_REQUIRE(std::filesystem::path(runtime).is_absolute());
    BOOST_REQUIRE(std::filesystem::is_directory(runtime));
    BOOST_REQUIRE(std::filesystem::path(site).is_absolute());
    BOOST_REQUIRE(std::filesystem::is_directory(site));
    BOOST_REQUIRE(std::filesystem::is_regular_file(root / "tests/installed-api/native-host-stream.py"));
    for (const auto* relative : {"bin/App_ServiceController", "bin/DI_NativeArtifactAuthority",
                                "bin/di-native-provider", "bin/DI_NativeRequester",
                                "libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker",
                                "libexec/ndnsf-tests/native-source-inspect"}) {
      const auto program = std::filesystem::path(runtime) / relative;
      BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(program),
                            "missing installed process prerequisite: " + program.string());
    }
  }
  const auto script = root / (name == "unary"
    ? "tests/standalone/run-spec261-source-full-process.py"
    : "tests/standalone/run-spec182-native-stream-process.py");
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(script),
                        "missing process orchestrator: " + script.string());
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_directory(build),
                        "missing native candidate build: " + build.string());
  BOOST_REQUIRE_MESSAGE(std::filesystem::is_directory(externalBuild),
                        "missing external process build: " + externalBuild.string());

  static unsigned sequence = 0;
  const auto runRoot = std::filesystem::temp_directory_path() /
    ("di-b7-" + name + "-" + std::to_string(static_cast<unsigned long>(::getpid())) +
     "-" + std::to_string(++sequence));
  std::filesystem::create_directories(runRoot);
  const auto log = runRoot / "cpp-driver.log";
  std::string libraryPath = externalBuild.string();
  if (std::find(arguments.begin(), arguments.end(), "--host-requester") != arguments.end())
    libraryPath = (std::filesystem::path(std::getenv("NDNSF_TEST_RUNTIME_PREFIX")) / "lib").string();
  if (const char* inherited = std::getenv("LD_LIBRARY_PATH");
      inherited != nullptr && *inherited != '\0') {
    libraryPath += ":";
    libraryPath += inherited;
  }
  std::string command = "env LD_LIBRARY_PATH=" + shellQuote(libraryPath) +
    " SPEC185_BUILD_DIR=" + shellQuote(externalBuild.string()) +
    " /usr/bin/timeout --kill-after=5s 180s " +
    shellQuote(std::getenv("NDNSF_TEST_PYTHON") ? std::getenv("NDNSF_TEST_PYTHON") :
                                             "/usr/bin/python3") + " " +
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
    const auto measurements = nativeParseJson(readFile(runRoot / "unary-process-measurements.json"));
    BOOST_REQUIRE(measurements.at("first_failure").is_null());
    BOOST_REQUIRE(measurements.at("forced_cleanup").empty());
    BOOST_REQUIRE(measurements.at("cleanup_errors").empty());
    BOOST_REQUIRE(measurements.at("live_processes").empty());
    const auto& exits = measurements.at("actor_exit_codes");
    BOOST_REQUIRE_EQUAL(exits.size(), revoked ? 6U : 5U);
    for (const auto& actor : exits.items()) {
      if (revoked && actor.key() == "requester")
        BOOST_REQUIRE_NE(actor.value().get<int>(), 0);
      else
        BOOST_REQUIRE_EQUAL(actor.value().get<int>(), 0);
    }
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
  else if (std::find(arguments.begin(), arguments.end(), "--host-requester") != arguments.end()) {
    requireProcessExit(output, "requester", true, log);
    requireHostStreamProcessOracle(runRoot,
      std::find(arguments.begin(), arguments.end(), "--host-cancel-after-event") != arguments.end());
  }
  else if (std::find(arguments.begin(), arguments.end(), "--stored-model-reuse") != arguments.end()) {
    requireStoredProcessOracle(runRoot, output, log,
      std::find(arguments.begin(), arguments.end(), "--request-time-model-preparation") !=
        arguments.end());
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
    ("di-b7-selector-" + target + "-" +
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

BOOST_AUTO_TEST_SUITE(DiProcess)

BOOST_AUTO_TEST_CASE(NativeRuntimeConfigurationRemainsCxxOwned)
{
  RuntimeConfig config;
  config.nativeConfigPath =
    (std::filesystem::temp_directory_path() / "di-b7-missing-runtime.json").string();
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

// Explicitly selected with a matching installed host SDK/site, outside the
// native-only default suite. Missing prerequisites fail the selected case.
BOOST_AUTO_TEST_CASE(InstalledHostFourStageStream, *boost::unit_test::disabled())
{
  runProcessCase("stream", {"--four-stage", "--host-requester"}, {});
}

BOOST_AUTO_TEST_CASE(InstalledHostFourStageCancel, *boost::unit_test::disabled())
{
  runProcessCase("stream", {"--four-stage", "--host-requester",
                            "--host-cancel-after-event", "3"}, {});
}

BOOST_AUTO_TEST_CASE(StoredModelSurvivesPreparerExitForIndependentUsers)
{
  runProcessCase("stream", {"--stored-model-reuse"}, {});
}

BOOST_AUTO_TEST_CASE(RequestTimePreparationUsesReusableStoredModelAndReadyRecovery)
{
  runProcessCase("stream", {"--stored-model-reuse", "--request-time-model-preparation"}, {});
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
  runNativeCasesTwice("di-prepared-request", "DiPreparedRequest", {
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
  runNativeCasesTwice("di-provider-assembly", "DiProviderAssembly/Spec188ProviderReferenceAssembly", {
    "AuthenticatedSelectionRunsProviderAssemblyRunnerAndResponse",
    "ProductionAssemblerFetchesCanonicalSourceAfterSelection",
    "ProviderArtifactCachePinsEvictsAndSeparatesIdentity",
    "ProtectedArtifactCacheColdHitBindsGrantAndRetainsCiphertext",
    "ProductionAssemblerCacheColdHitUsesExactArtifact",
    "ProductionAssemblerCacheScansStableRootAndVerifiesFileDigest",
    "ProtectedSelectionBindingRejectsProviderEpochAndGrantSubstitution",
    "ProviderArtifactCacheStopCancelsInFlightBuild",
    "ProviderArtifactCacheStopPreservesActiveLeaseUntilRelease",
    "MemoryLifecycleAlternatesModelsAndReclaimsEvictedArtifacts"});
  runNativeCasesTwice("di-provider-assembly", "DiProviderAssembly", {
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
