// Spec189 C++ full-path oracle.
//
// The MiniNDN runner owns process and topology orchestration.  This executable
// owns the product assertion: it reads the immutable logs emitted by the real
// requester and Providers, checks the authenticated post-Selection stage
// sequence, and rejects a generic requester timeout as a successful result.

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "Spec189MaterialFetchOracle.hpp"
#include "Spec189ProviderStageOracle.hpp"

#include <algorithm>
#include <cstdint>
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

using namespace spec189::oracle;

std::vector<std::string>
recordsContaining(const std::string& text, const std::string& needle)
{
  std::vector<std::string> result;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.find(needle) != std::string::npos)
      result.push_back(std::move(line));
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

PlacementObservation
validatePlacementProvider(const std::filesystem::path& path,
                          const std::string& expectedProvider)
{
  const auto text = readFile(path);
  const auto selectionLine = lineContaining(
    text, "NDNSF_DI_NATIVE_SELECTION_ACCEPTED");
  if (selectionLine == 0) {
    throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=SELECTION log=" +
                             path.string());
  }
  const auto selectionRecord = recordContaining(
    text, "NDNSF_DI_NATIVE_SELECTION_ACCEPTED");
  Marker selection;
  selection.stage = "SELECTION";
  selection.requestId = field(selectionRecord, "requestId");
  selection.attemptEpoch = field(selectionRecord, "attemptEpoch");
  selection.provider = field(selectionRecord, "provider");
  selection.role = field(selectionRecord, "role");
  selection.planDigest = field(selectionRecord, "planDigest");
  selection.manifestDigest = field(selectionRecord, "manifestDigest");
  selection.graphDigest = field(selectionRecord, "graphDigest");
  selection.initializerDigest = field(selectionRecord, "initializerDigest");
  selection.artifactDigest = field(selectionRecord, "artifactDigest");
  selection.layerBegin = field(selectionRecord, "layerBegin");
  selection.layerEnd = field(selectionRecord, "layerEnd");
  selection.line = selectionLine;
  for (const auto* value : {&selection.requestId, &selection.attemptEpoch,
                            &selection.provider, &selection.role,
                            &selection.planDigest, &selection.manifestDigest,
                            &selection.graphDigest, &selection.initializerDigest,
                            &selection.artifactDigest, &selection.layerBegin,
                            &selection.layerEnd}) {
    if (value->empty()) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=SELECTION reason=missing-binding log=" +
        path.string());
    }
  }
  if (selection.provider != expectedProvider) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=SELECTION reason=provider-mismatch log=" +
      path.string());
  }
  std::uint64_t layerBegin = 0;
  std::uint64_t layerEnd = 0;
  try {
    layerBegin = std::stoull(selection.layerBegin);
    layerEnd = std::stoull(selection.layerEnd);
  }
  catch (const std::exception&) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=SELECTION reason=invalid-range log=" +
      path.string());
  }
  if (layerEnd <= layerBegin) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=SELECTION reason=empty-range log=" +
      path.string());
  }

  const auto observed = markers(text);
  const std::vector<std::string> forbiddenBeforeSelection{
    "EXECUTION_ENTERED", "DEPENDENCY_FETCH", "ASSEMBLY_STARTED",
    "RUNNER_READY", "EXECUTION_COMPLETED", "TERMINAL"};
  for (const auto& marker : observed) {
    if (marker.line >= selectionLine)
      break;
    if (std::find(forbiddenBeforeSelection.begin(), forbiddenBeforeSelection.end(),
                  marker.stage) != forbiddenBeforeSelection.end()) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=PRE_SELECTION_SIDE_EFFECT reason=" +
        marker.stage + " log=" + path.string());
    }
  }
  const auto grantRecords = recordsContaining(text, "NDNSF_DI_GRANT_VERIFIED");
  std::size_t grantLine = 0;
  for (const auto& record : grantRecords) {
    const auto candidateLine = lineContaining(text, record);
    if (candidateLine <= selectionLine ||
        field(record, "requestId") != selection.requestId ||
        field(record, "provider") != selection.provider ||
        field(record, "planDigest") != selection.planDigest ||
        field(record, "attemptEpoch") != selection.attemptEpoch)
      continue;
    grantLine = candidateLine;
    break;
  }
  if (grantLine == 0) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=GRANT_VERIFIED reason=missing-or-before-selection log=" +
      path.string());
  }
  return {std::move(selection), selectionLine, grantLine};
}

void
validateProvider(const std::filesystem::path& path,
                 const std::string& expectedProvider,
                 std::string& requestId,
                 std::string& planDigest,
                 bool& terminalSeen)
{
  const auto placement = validatePlacementProvider(path, expectedProvider);
  const auto& selection = placement.selection;
  if ((!requestId.empty() && requestId != selection.requestId) ||
      (!planDigest.empty() && planDigest != selection.planDigest)) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=IDENTITY reason=cross-provider-mismatch");
  }
  const bool terminal = validateProviderStages(
    markers(readFile(path)), placement, expectedProvider);
  // Spec189 freezes the Qwen3-0.6B candidate at 28 layers. Only its last
  // selected range can provide the terminal model response.
  if (selection.layerEnd == "28" && !terminal) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=TERMINAL reason=tail-terminal-missing");
  }
  if (selection.layerEnd != "28" && terminal) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=TERMINAL reason=non-tail-terminal");
  }
  requestId = selection.requestId;
  planDigest = selection.planDigest;
  terminalSeen = terminalSeen || terminal;
}

void
validatePlacementOnly(const std::filesystem::path& root)
{
  const auto requester = readFile(root / "requester-0.log");
  if (lineContaining(requester, "NDNSF_DI_NATIVE_SELECTION_COMMITTED") == 0) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=SELECTION_COMMIT reason=missing-core-selection");
  }
  const auto assignmentRecords = recordsContaining(
    requester, "NDNSF_COLLAB_ASSIGNMENT_SELECTED");
  if (assignmentRecords.size() < 2) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=ACK_SELECTION reason=two-provider-selection-missing");
  }
  const auto first = validatePlacementProvider(
    root / "provider-0.log", "/example/ndnsf-qwen06b/provider-0");
  const auto second = validatePlacementProvider(
    root / "provider-1.log", "/example/ndnsf-qwen06b/provider-1");
  const auto& left = first.selection;
  const auto& right = second.selection;
  if (left.requestId != right.requestId || left.attemptEpoch != right.attemptEpoch ||
      left.planDigest != right.planDigest || left.manifestDigest != right.manifestDigest ||
      left.graphDigest != right.graphDigest ||
      left.initializerDigest != right.initializerDigest ||
      left.provider == right.provider || left.role == right.role) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=SELECTION reason=two-provider-binding-mismatch");
  }
  const auto committedRecord = recordContaining(
    requester, "NDNSF_DI_NATIVE_SELECTION_COMMITTED");
  if (committedRecord.empty() ||
      field(committedRecord, "requestId") != left.requestId ||
      field(committedRecord, "attemptEpoch") != left.attemptEpoch ||
      field(committedRecord, "planDigest") != left.planDigest) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=SELECTION_COMMIT reason=identity-mismatch");
  }
  std::map<std::string, std::string> assignments;
  for (const auto& record : assignmentRecords) {
    if (field(record, "requestId") != left.requestId)
      continue;
    const auto provider = field(record, "providerName");
    if (provider != left.provider && provider != right.provider)
      continue;
    const auto role = field(record, "role");
    if (role.empty()) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=ACK_SELECTION reason=assignment-binding-fields-missing");
    }
    const auto [assignmentIt, inserted] = assignments.emplace(provider, role);
    if (!inserted && assignmentIt->second != role) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=ACK_SELECTION reason=assignment-role-conflict");
    }
  }
  if (assignments.size() != 2 || assignments[left.provider] != left.role ||
      assignments[right.provider] != right.role) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=ACK_SELECTION reason=assignment-binding-mismatch");
  }
  const auto leftBegin = std::stoull(left.layerBegin);
  const auto leftEnd = std::stoull(left.layerEnd);
  const auto rightBegin = std::stoull(right.layerBegin);
  const auto rightEnd = std::stoull(right.layerEnd);
  const auto& firstRange = leftBegin <= rightBegin ? left : right;
  const auto& secondRange = leftBegin <= rightBegin ? right : left;
  const auto firstBegin = std::stoull(firstRange.layerBegin);
  const auto firstEnd = std::stoull(firstRange.layerEnd);
  const auto secondBegin = std::stoull(secondRange.layerBegin);
  const auto secondEnd = std::stoull(secondRange.layerEnd);
  constexpr std::uint64_t expectedLayerCount = 28;
  if (firstBegin != 0 || firstEnd != secondBegin || secondEnd != expectedLayerCount) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=SELECTION reason=range-coverage-mismatch");
  }
  const auto leftFetches = validateMaterialFetches(
    root / "provider-0.log", first, left.provider);
  const auto rightFetches = validateMaterialFetches(
    root / "provider-1.log", second, right.provider);
  std::cout << "SPEC189_CPP_PLACEMENT_PASS "
            << ndnsf::di::nativeCanonicalJson(ndnsf::di::NativeJson{
                 {"requestId", left.requestId},
                 {"attemptEpoch", left.attemptEpoch},
                 {"planDigest", left.planDigest},
                 {"manifestDigest", left.manifestDigest},
                 {"graphDigest", left.graphDigest},
                 {"providers", 2},
                 {"preSelectionFetches", leftFetches.preSelection + rightFetches.preSelection},
                 {"postSelectionFetches", leftFetches.postSelection + rightFetches.postSelection},
                 {"layerCount", expectedLayerCount},
                 {"provider0Range", ndnsf::di::NativeJson::array({leftBegin, leftEnd})},
                 {"provider1Range", ndnsf::di::NativeJson::array({rightBegin, rightEnd})}})
            << '\n';
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "usage: " << argv[0]
              << " [--placement-only] --run-root DIRECTORY\n";
    return 0;
  }
  const bool placementOnly = argc == 4 && std::string(argv[1]) == "--placement-only";
  const char* runRootArg = nullptr;
  if (placementOnly && std::string(argv[2]) == "--run-root")
    runRootArg = argv[3];
  else if (!placementOnly && argc == 3 && std::string(argv[1]) == "--run-root")
    runRootArg = argv[2];
  if (runRootArg == nullptr) {
    std::cerr << "usage: " << argv[0]
              << " [--placement-only] --run-root DIRECTORY\n";
    return 2;
  }
  try {
    const auto root = std::filesystem::absolute(runRootArg);
    if (placementOnly) {
      validatePlacementOnly(root);
      return 0;
    }
    // Full execution verdicts include the same authenticated placement and
    // material checks as the diagnostic placement-only entry.
    validatePlacementOnly(root);
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
