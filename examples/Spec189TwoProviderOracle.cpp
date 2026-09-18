// Spec189 C++ full-path oracle.
//
// The MiniNDN runner owns process and topology orchestration.  This executable
// owns the product assertion: it reads the immutable logs emitted by the real
// requester and Providers, checks the authenticated post-Selection stage
// sequence, and rejects a generic requester timeout as a successful result.

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Marker
{
  std::string stage;
  std::string status;
  std::string requestId;
  std::string attemptEpoch;
  std::string provider;
  std::string role;
  std::string planDigest;
  std::size_t line = 0;
};

std::string
readFile(const std::filesystem::path& path)
{
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("missing Spec189 log: " + path.string());
  }
  std::ostringstream content;
  content << input.rdbuf();
  return content.str();
}

std::string
field(const std::string& line, const std::string& key)
{
  const auto prefix = key + "=";
  const auto begin = line.find(prefix);
  if (begin == std::string::npos) {
    return {};
  }
  const auto valueBegin = begin + prefix.size();
  const auto valueEnd = line.find_first_of(" \t\r\n", valueBegin);
  return line.substr(valueBegin, valueEnd == std::string::npos
                                 ? std::string::npos : valueEnd - valueBegin);
}

std::vector<Marker>
markers(const std::string& text)
{
  std::vector<Marker> result;
  std::istringstream lines(text);
  std::string line;
  std::size_t lineNumber = 0;
  while (std::getline(lines, line)) {
    ++lineNumber;
    if (line.find("NDNSF_DI_PROVIDER_STAGE") == std::string::npos) {
      continue;
    }
    Marker marker;
    marker.stage = field(line, "stage");
    marker.status = field(line, "status");
    marker.requestId = field(line, "requestId");
    marker.attemptEpoch = field(line, "attemptEpoch");
    marker.provider = field(line, "provider");
    marker.role = field(line, "role");
    marker.planDigest = field(line, "planDigest");
    marker.line = lineNumber;
    if (!marker.stage.empty()) {
      result.push_back(std::move(marker));
    }
  }
  return result;
}

std::size_t
lineContaining(const std::string& text, const std::string& needle)
{
  std::istringstream lines(text);
  std::string line;
  std::size_t lineNumber = 0;
  while (std::getline(lines, line)) {
    ++lineNumber;
    if (line.find(needle) != std::string::npos) {
      return lineNumber;
    }
  }
  return 0;
}

std::string
recordContaining(const std::string& text, const std::string& needle)
{
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.find(needle) != std::string::npos) {
      return line;
    }
  }
  return {};
}

Marker
requireStage(const std::vector<Marker>& observed,
             const std::string& stage,
             const std::string& log,
             std::size_t afterLine = 0,
             const std::string& expectedStatus = {})
{
  const auto found = std::find_if(observed.begin(), observed.end(),
    [&] (const Marker& marker) {
      return marker.stage == stage && marker.line > afterLine &&
        (expectedStatus.empty() || marker.status == expectedStatus);
    });
  if (found == observed.end()) {
    throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=" + stage +
                             " log=" + log);
  }
  return *found;
}

void
validateProvider(const std::filesystem::path& path,
                 const std::string& expectedProvider,
                 std::string& requestId,
                 std::string& planDigest,
                 bool& terminalSeen)
{
  const auto text = readFile(path);
  const auto selectionLine = lineContaining(text, "NDNSF_DI_NATIVE_SELECTION_ACCEPTED");
  if (selectionLine == 0) {
    throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=SELECTION log=" +
                             path.string());
  }
  const auto observed = markers(text);
  const auto grantLine = lineContaining(text, "NDNSF_DI_GRANT_VERIFIED");
  if (grantLine == 0 || grantLine < selectionLine) {
    throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=GRANT_VERIFIED log=" +
                             path.string());
  }
  const auto selectionRecord = recordContaining(
    text, "NDNSF_DI_NATIVE_SELECTION_ACCEPTED");
  const auto grantRecord = recordContaining(text, "NDNSF_DI_GRANT_VERIFIED");
  const auto selectedRequestId = field(selectionRecord, "requestId");
  const auto selectedAttemptEpoch = field(selectionRecord, "attemptEpoch");
  const auto selectedPlanDigest = field(selectionRecord, "planDigest");
  const auto selectedRole = field(selectionRecord, "role");
  const auto selectedProvider = field(selectionRecord, "provider");
  const auto grantedRequestId = field(grantRecord, "requestId");
  const auto grantedAttemptEpoch = field(grantRecord, "attemptEpoch");
  const auto grantedProvider = field(grantRecord, "provider");
  const auto grantedPlanDigest = field(grantRecord, "planDigest");
  if (selectedRequestId.empty() || selectedAttemptEpoch.empty() ||
      selectedPlanDigest.empty() || selectedRole.empty() ||
      selectedProvider != expectedProvider ||
      grantedRequestId.empty() || grantedAttemptEpoch.empty() ||
      grantedProvider != expectedProvider || grantedPlanDigest.empty() ||
      grantedRequestId != selectedRequestId ||
      grantedAttemptEpoch != selectedAttemptEpoch ||
      grantedPlanDigest != selectedPlanDigest) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=IDENTITY reason=selection-grant-mismatch log=" +
      path.string());
  }

  const auto grantMarker = requireStage(
    observed, "GRANT_VERIFIED", path.string(), grantLine, "observed");
  if (grantMarker.provider != expectedProvider ||
      grantMarker.role != selectedRole ||
      grantMarker.requestId != selectedRequestId ||
      grantMarker.attemptEpoch != selectedAttemptEpoch ||
      grantMarker.planDigest != selectedPlanDigest) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=GRANT_VERIFIED reason=identity-mismatch log=" +
      path.string());
  }

  const std::vector<std::string> sequence{
    "EXECUTION_ENTERED", "DEPENDENCY_FETCH", "ASSEMBLY_STARTED",
    "RUNNER_READY", "EXECUTION_COMPLETED"};
  std::size_t previousLine = grantMarker.line;
  for (const auto& stage : sequence) {
    const auto marker = requireStage(
      observed, stage, path.string(), previousLine,
      stage == "DEPENDENCY_FETCH" ? "complete" : "observed");
    if (marker.provider != expectedProvider || marker.role != selectedRole) {
      throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=" + stage +
                               " reason=provider-role-identity log=" + path.string());
    }
    if (stage == "EXECUTION_ENTERED" && marker.line == 0) {
      throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=EXECUTION_ENTERED");
    }
    previousLine = marker.line;
    if (marker.requestId.empty() || marker.attemptEpoch.empty() ||
        marker.planDigest.empty()) {
      throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=" + stage +
                               " reason=missing-identity log=" + path.string());
    }
    if (requestId.empty()) requestId = selectedRequestId;
    if (planDigest.empty()) planDigest = selectedPlanDigest;
    if (marker.requestId != requestId ||
        marker.attemptEpoch != selectedAttemptEpoch ||
        marker.planDigest != planDigest ||
        marker.requestId != selectedRequestId ||
        marker.planDigest != selectedPlanDigest) {
      throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=" + stage +
                               " reason=identity-mismatch log=" + path.string());
    }
  }
  const auto terminalIt = std::find_if(
    observed.begin(), observed.end(), [&] (const Marker& marker) {
      return marker.stage == "TERMINAL" && marker.line > previousLine;
    });
  if (terminalIt != observed.end()) {
    const auto& terminal = *terminalIt;
    if (terminal.provider != expectedProvider ||
        terminal.role != selectedRole ||
        terminal.requestId != selectedRequestId ||
        terminal.attemptEpoch != selectedAttemptEpoch ||
        terminal.planDigest != selectedPlanDigest) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=TERMINAL reason=identity-mismatch log=" +
        path.string());
    }
    terminalSeen = terminalSeen || terminal.status == "observed";
  }
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "usage: " << argv[0] << " --run-root DIRECTORY\n";
    return 0;
  }
  if (argc != 3 || std::string(argv[1]) != "--run-root") {
    std::cerr << "usage: " << argv[0] << " --run-root DIRECTORY\n";
    return 2;
  }
  try {
    const auto root = std::filesystem::absolute(argv[2]);
    const auto requester = readFile(root / "requester-0.log");
    const auto requesterRecord = recordContaining(
      requester, "NATIVE_REQUEST_SUCCEEDED");
    if (requesterRecord.empty()) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=REQUEST_TERMINAL reason=requester-not-success");
    }
    const auto requesterId = field(requesterRecord, "request");
    const auto requesterPlan = field(requesterRecord, "plan");
    if (requesterId.empty() || requesterPlan.empty()) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=REQUEST_TERMINAL reason=requester-identity-missing");
    }
    std::string requestId;
    std::string planDigest;
    bool terminalSeen = false;
    validateProvider(root / "provider-0.log", "/example/ndnsf-qwen06b/provider-0",
                     requestId, planDigest, terminalSeen);
    validateProvider(root / "provider-1.log", "/example/ndnsf-qwen06b/provider-1",
                     requestId, planDigest, terminalSeen);
    if (requesterId != requestId || requesterPlan != planDigest) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=REQUEST_TERMINAL reason=identity-mismatch");
    }
    if (!terminalSeen) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=TERMINAL reason=no-terminal-provider");
    }
    // Exercise the production canonical JSON boundary so this selector is
    // linked to the same DI library used by the native request path.
    ndnsf::di::NativeJson receipt{{"schema", "spec189-cpp-oracle-v1"},
                                  {"requestId", requestId},
                                  {"planDigest", planDigest},
                                  {"providers", 2},
                                  {"terminal", true}};
    std::cout << "SPEC189_CPP_ORACLE_PASS "
              << ndnsf::di::nativeCanonicalJson(receipt) << '\n';
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
