// Spec189 C++ full-path oracle.
//
// The MiniNDN runner owns process and topology orchestration.  This executable
// owns the product assertion: it reads the immutable logs emitted by the real
// requester and Providers, checks the authenticated post-Selection stage
// sequence, and rejects a generic requester timeout as a successful result.

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGenerationLimits.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "Spec189MaterialFetchOracle.hpp"
#include "Spec189ProviderStageOracle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

using namespace spec189::oracle;
using ndnsf::di::NativeJson;
using namespace ndnsf_distributed_repo;

[[noreturn]] void chainFailure(const std::string& reason)
{
  throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=CONVERSATION reason=" + reason);
}

std::string roundJson(const std::string& stem, unsigned round)
{
  return stem + (round == 0 ? "" : "-" + std::to_string(round)) + ".json";
}

NativeJson readJson(const std::filesystem::path& path)
{
  if (!std::filesystem::is_regular_file(path)) chainFailure("missing-round-file");
  return ndnsf::di::nativeParseJson(readFile(path));
}

// Strict marker fields only for new round-routing evidence. The historical
// single-round parsers retain their existing contracts.
std::map<std::string, std::string> markerFields(const std::string& record,
                                              const std::string& marker)
{
  std::map<std::string, std::string> fields;
  std::istringstream input(record.substr(record.find(marker) + marker.size()));
  std::string token;
  while (input >> token) {
    const auto eq = token.find('=');
    if (eq == std::string::npos || eq == 0 ||
        !fields.emplace(token.substr(0, eq), token.substr(eq + 1)).second)
      chainFailure("ambiguous-marker-fields");
  }
  return fields;
}

std::uint64_t nonnegativeInteger(const NativeJson& value)
{
  if (!value.is_number_integer() ||
      (value.is_number_unsigned() ? value.get<std::uint64_t>() > INT64_MAX
                                 : value.get<std::int64_t>() < 0))
    chainFailure("invalid-integer");
  return value.get<std::uint64_t>();
}

std::size_t tokenCount(const NativeJson& value)
{
  if (!value.is_array() || value.empty()) chainFailure("invalid-input-tokens");
  for (const auto& token : value) nonnegativeInteger(token);
  return value.size();
}

// Reuse the unchanged shared file-based stage/material gates on request-scoped
// evidence. Never alter the run directory. Temporary views retain line order,
// include conflicting identities of the SAME request, and are RAII-cleaned.
struct RoundEvidence {
  std::filesystem::path root;
  RoundEvidence()
  {
    auto pattern = (std::filesystem::temp_directory_path() / "spec189-round-oracle-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const auto created = ::mkdtemp(name.data());
    if (!created) chainFailure("temporary-evidence-unavailable");
    root = created;
  }
  ~RoundEvidence() { std::error_code error; std::filesystem::remove_all(root, error); }
  RoundEvidence(const RoundEvidence&) = delete;
  RoundEvidence& operator=(const RoundEvidence&) = delete;
  void write(const std::string& name, const std::string& text) const
  {
    std::ofstream output(root / name);
    output << text;
    output.close();
    if (!output) chainFailure("temporary-evidence-write-failed");
  }
};

std::string scopeProvider(const std::string& text, const std::string& request)
{
  std::istringstream input(text);
  std::string line, scoped;
  while (std::getline(input, line)) {
    const bool complete = !input.eof();
    for (const auto* marker : {"NDNSF_DI_NATIVE_SELECTION_ACCEPTED", "NDNSF_DI_GRANT_VERIFIED",
           "NDNSF_DI_PROVIDER_STAGE", "NDNSF_DI_PROVIDER_MATERIAL_FETCH",
           "NDNSF_DI_PROVIDER_PREPARATION", "NDNSF_DI_CONVERSATION_KV_RESTORED"}) {
      if (line.find(marker) == std::string::npos) continue;
      if (!complete) chainFailure("partial-provider-record");
      const auto fields = markerFields(line, marker);
      const auto id = fields.find("requestId");
      if (id == fields.end() || id->second.empty()) chainFailure("provider-request-missing");
      if (id->second == request) scoped += line + '\n';
      break;
    }
  }
  return scoped;
}

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

void validatePlacementOnly(const std::filesystem::path& root, bool cacheCompatibility,
                           bool emitPass);
NativeJson validateMultiTokenOutput(const std::filesystem::path& root, unsigned round,
                                    bool requireMultiToken);

void
validateRevocationFailure(const std::filesystem::path& root)
{
  // Reuse the normal production-path checks for the successful first round;
  // this mode adds the C++ assertion for the expected post-revocation stop.
  const auto firstRequester = readFile(root / "requester-0.log");
  const auto firstSuccesses = recordsContaining(firstRequester, "NATIVE_REQUEST_SUCCEEDED");
  const auto firstCommits = recordsContaining(
    firstRequester, "NDNSF_DI_NATIVE_SELECTION_COMMITTED");
  if (firstSuccesses.size() != 1 || firstCommits.size() != 1) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=FIRST_ROUND_IDENTITY reason=success-or-commit-missing-or-duplicate");
  }
  const auto firstSuccess = firstSuccesses.front();
  const auto firstCommit = firstCommits.front();
  const auto firstRequestId = field(firstSuccess, "request");
  const auto firstPlanDigest = field(firstSuccess, "plan");
  const auto firstCommitRequestId = field(firstCommit, "requestId");
  const auto firstCommitPlanDigest = field(firstCommit, "planDigest");
  const auto firstAttemptEpoch = field(firstCommit, "attemptEpoch");
  if (firstRequestId.empty() || firstPlanDigest.empty() ||
      firstCommitRequestId != firstRequestId || firstCommitPlanDigest != firstPlanDigest ||
      firstAttemptEpoch.empty()) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=FIRST_ROUND_IDENTITY reason=success-commit-mismatch");
  }
  validatePlacementOnly(root, false, false);
  const auto generationText = readFile(root / "controller.log");
  const auto applied = recordsContaining(
    generationText, "NDNSF_REVOCATION_APPLIED success=1");
  const auto triggered = recordsContaining(
    generationText, "NDNSF_REVOCATION_TRIGGERED");
  if (applied.size() != 1 || triggered.size() != 1) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=REVOCATION reason=controller-marker-missing-or-duplicate");
  }
  const auto appliedLine = lineContaining(generationText, applied.front());
  const auto triggeredLine = lineContaining(generationText, triggered.front());
  const auto generation = field(applied.front(), "generation");
  const auto epoch = field(applied.front(), "epoch");
  const auto isPositiveInteger = [](const std::string& value) {
    return !value.empty() && value.find_first_not_of("0123456789") == std::string::npos &&
      std::stoull(value) > 0;
  };
  if (appliedLine == 0 || triggeredLine == 0 || appliedLine >= triggeredLine ||
      !isPositiveInteger(generation) || !isPositiveInteger(epoch)) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=REVOCATION reason=controller-marker-order-or-version-invalid");
  }

  const auto continuation = readFile(root / "requester-revoked.log");
  const auto preparationFailures = recordsContaining(
    continuation,
    "NATIVE_REQUEST_STAGE_FAILED code=PREPARATION_FAILED boundary=preparation");
  if (preparationFailures.size() != 1 ||
      continuation.find("PROTECTED_CONTROLLER_VERSION_UNAVAILABLE") == std::string::npos ||
      continuation.find("NATIVE_REQUEST_SUCCEEDED") != std::string::npos) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=REVOCATION reason=continuation-not-fail-closed");
  }

  std::string requestId = firstRequestId;
  std::string planDigest = firstPlanDigest;
  bool terminalSeen = false;
  for (unsigned provider = 0; provider < 2; ++provider) {
    const auto path = root / ("provider-" + std::to_string(provider) + ".log");
    const auto text = readFile(path);
    if (recordsContaining(text, "NDNSF_DI_NATIVE_SELECTION_ACCEPTED").size() != 1 ||
        recordsContaining(text, "stage=ASSEMBLY_STARTED").size() != 1 ||
        recordsContaining(text, "stage=RUNNER_READY").size() != 1 ||
        recordsContaining(text, "stage=EXECUTION_COMPLETED").size() != 1) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=REVOCATION reason=provider-execution-after-revoke log=" +
        path.string());
    }
    validateProvider(path, "/example/ndnsf-qwen06b/provider-" + std::to_string(provider),
                     requestId, planDigest, terminalSeen);
  }
  if (!terminalSeen) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=REVOCATION reason=first-round-terminal-missing");
  }
  const auto generationResult = validateMultiTokenOutput(root, 0, true);
  auto pass = NativeJson::object();
  pass["scope"] = "t007-resident-revocation";
  pass["controllerGeneration"] = generation;
  pass["controllerEpoch"] = epoch;
  pass["requestId"] = requestId;
  pass["providers"] = 2;
  pass["continuationBoundary"] = "PREPARATION_FAILED";
  pass["generatedTokens"] = generationResult.at("generatedTokens");
  std::cout << "SPEC189_CPP_REVOCATION_FAIL_CLOSED_PASS "
            << ndnsf::di::nativeCanonicalJson(pass)
            << '\n';
}

void
validateCacheCompatibilityMode(const std::filesystem::path& root, unsigned round = 0)
{
  // Mode declarations must not contain ambiguous duplicate keys. This is
  // evidence of an explicitly selected diagnostic mode, not cache hash proof.
  const auto modeFields = [](const std::string& record, const std::string& marker) {
    std::map<std::string, std::string> values;
    std::istringstream tokens(record.substr(record.find(marker) + marker.size()));
    std::string token;
    while (tokens >> token) {
      const auto equal = token.find('=');
      if (equal == std::string::npos || equal == 0 ||
          !values.emplace(token.substr(0, equal), token.substr(equal + 1)).second)
        throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=CACHE_MODE reason=ambiguous-mode-fields");
    }
    return values;
  };
  const auto requester = readFile(root / ("requester-" + std::to_string(round) + ".log"));
  const auto requests = recordsContaining(requester, "NDNSF_DI_CACHE_COMPATIBILITY_REQUESTER");
  if (requests.size() != 1)
    throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=CACHE_MODE reason=requester-mode-missing-or-mixed");
  auto requestMode = modeFields(requests.front(), "NDNSF_DI_CACHE_COMPATIBILITY_REQUESTER");
  if (requestMode["enabled"] != "true" || requestMode["protectedPublication"] != "skipped")
    throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=CACHE_MODE reason=requester-mode-missing-or-mixed");
  if (requester.find("NDNSF_DI_PROVIDER_MATERIAL_FETCH") != std::string::npos)
    throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=CACHE_MODE reason=material-events-in-cache-mode");
  for (const auto* name : {"provider-0.log", "provider-1.log"}) {
    const auto text = readFile(root / name);
    const auto configs = recordsContaining(text, "NDNSF_DI_CACHE_COMPATIBILITY_CONFIG");
    if (configs.size() != 1)
      throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=CACHE_MODE reason=provider-mode-missing-or-mixed");
    auto providerMode = modeFields(configs.front(), "NDNSF_DI_CACHE_COMPATIBILITY_CONFIG");
    if (providerMode["sourceDir"].empty() || providerMode["repoFetch"] != "skipped-after-selection")
      throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=CACHE_MODE reason=provider-mode-missing-or-mixed");
    if (text.find("NDNSF_DI_PROVIDER_MATERIAL_FETCH") != std::string::npos)
      throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=CACHE_MODE reason=material-events-in-cache-mode");
  }
}

void
validatePlacementOnly(const std::filesystem::path& root, bool cacheCompatibility = false,
                      bool emitPass = true)
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
  // Explicit diagnostic scope only: all placement/grant checks above remain,
  // but neither material qualification nor a placement PASS is claimed.
  if (cacheCompatibility) return;
  const auto validateProviderMaterial = [](const std::filesystem::path& path,
                                           const PlacementObservation& placement,
                                           const std::string& expectedProvider) {
    const auto text = readFile(path);
    // CACHE_LOOKUP_HIT is the historical material-cache marker. The native
    // provider now reports the complete cache contract with CACHE_ACQUIRE_DONE
    // so the oracle must accept both without requiring a redundant material
    // fetch. A modern acquire record takes precedence when both are present.
    const auto legacyHits = recordsContaining(text,
      "NDNSF_DI_PROVIDER_PREPARATION phase=CACHE_LOOKUP_HIT");
    const auto acquireHits = recordsContaining(text,
      "NDNSF_DI_PROVIDER_PREPARATION phase=CACHE_ACQUIRE_DONE");
    const auto misses = recordsContaining(text,
      "NDNSF_DI_PROVIDER_PREPARATION phase=CACHE_LOOKUP_MISS");
    if (acquireHits.size() > 1 || legacyHits.size() > 1 || misses.size() > 1 ||
        (!acquireHits.empty() && !misses.empty()) ||
        (acquireHits.empty() && !legacyHits.empty() && !misses.empty())) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=CACHE_LOOKUP reason=ambiguous-cache-result log=" +
        path.string());
    }
    if (acquireHits.empty() && legacyHits.empty()) {
      return validateMaterialFetches(path, placement, expectedProvider);
    }
    const auto cacheHit = acquireHits.empty() ? legacyHits.front() : acquireHits.front();
    const auto detail = field(cacheHit, "detail");
    if (field(cacheHit, "requestId") != placement.selection.requestId ||
        field(cacheHit, "provider") != expectedProvider ||
        field(cacheHit, "role") != placement.selection.role || detail.empty() ||
        (!acquireHits.empty() && detail != "assembled-disk-hit" && detail != "template-hit")) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=CACHE_LOOKUP reason=identity-mismatch log=" +
        path.string());
    }
    const auto rebinds = recordsContaining(text,
      "NDNSF_DI_PROVIDER_PREPARATION phase=CACHE_TEMPLATE_REBIND");
    if (rebinds.size() > 1 ||
        (!rebinds.empty() && (detail != "template-hit" ||
          field(rebinds.front(), "requestId") != placement.selection.requestId ||
          field(rebinds.front(), "provider") != expectedProvider ||
          field(rebinds.front(), "role") != placement.selection.role ||
          field(rebinds.front(), "detail") != "current-grant-ciphertext"))) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=CACHE_LOOKUP reason=template-rebind-mismatch log=" +
        path.string());
    }
    if (text.find("NDNSF_DI_PROVIDER_MATERIAL_FETCH") != std::string::npos) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=CACHE_LOOKUP reason=material-events-on-cache-hit log=" +
        path.string());
    }
    return MaterialFetchObservation{};
  };
  const auto leftFetches = validateProviderMaterial(
    root / "provider-0.log", first, left.provider);
  const auto rightFetches = validateProviderMaterial(
    root / "provider-1.log", second, right.provider);
  if (emitPass) std::cout << "SPEC189_CPP_PLACEMENT_PASS "
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

ndnsf::di::NativeJson
validateMultiTokenOutput(const std::filesystem::path& root, unsigned round = 0,
                         bool requireMultiToken = true)
{
  using ndnsf::di::NativeJson;
  const auto fail = [](const std::string& reason) {
    throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=GENERATION reason=" + reason);
  };
  const auto config = readJson(root / "requester" / roundJson("config", round));
  const auto& request = config.at("request");
  // Qualify only the launcher's per-round, non-replacement recipe.
  // Reject a different options source instead of silently inspecting unused data.
  if (request.value("options_file", "") != roundJson("options", round) ||
      (request.contains("allow_replacement") && request.at("allow_replacement") != false) ||
      (request.contains("max_replacements") && request.at("max_replacements") != 0))
    fail("unsupported-request-config");
  const auto options = readJson(root / "requester" / roundJson("options", round));
  const auto output = readJson(root / "requester" / ("output-" + std::to_string(round) + ".bin"));
  const auto validToken = [](const NativeJson& value) {
    return value.is_number_integer() &&
      (value.is_number_unsigned() ? value.get<std::uint64_t>() <= INT64_MAX : value.get<std::int64_t>() >= 0);
  };
  if (!options.contains("maxNewTokens") || !validToken(options.at("maxNewTokens")) ||
      !options.contains("eosTokenIds") || !options.at("eosTokenIds").is_array() ||
      options.at("eosTokenIds").empty()) fail("invalid-generation-options");
  const auto maximum = options.at("maxNewTokens").get<std::uint64_t>();
  if (maximum < (requireMultiToken ? 2U : 1U) ||
      maximum > ndnsf::di::MAX_NATIVE_GENERATED_TOKENS)
    fail("invalid-generation-options");
  if (request.contains("max_new_tokens") &&
      (!validToken(request.at("max_new_tokens")) ||
       request.at("max_new_tokens").get<std::uint64_t>() != maximum))
    fail("generation-budget-override-mismatch");
  std::vector<std::int64_t> eos;
  for (const auto& value : options.at("eosTokenIds")) {
    if (!validToken(value)) fail("invalid-generation-options");
    eos.push_back(value.get<std::int64_t>());
  }
  if (output.value("schema", "") != "NDNSF-DI-FINAL-V1" ||
      !output.contains("text") || !output.at("text").is_string() ||
      output.at("text").get<std::string>().empty() ||
      !output.contains("tokenIds") || !output.at("tokenIds").is_array())
    fail("invalid-final-output");
  const auto& tokens = output.at("tokenIds");
  if (tokens.size() < (requireMultiToken ? 2U : 1U) || tokens.size() > maximum) fail("not-bounded-multi-token");
  for (std::size_t i = 0; i != tokens.size(); ++i) {
    if (!validToken(tokens[i])) fail("invalid-final-token");
    if (i + 1 < tokens.size() &&
        std::find(eos.begin(), eos.end(), tokens[i].get<std::int64_t>()) != eos.end())
      fail("tokens-after-eos");
  }
  const bool ended = std::find(eos.begin(), eos.end(), tokens.back().get<std::int64_t>()) != eos.end();
  const auto reason = output.value("finishReason", "");
  const auto hint = output.value("finishHint", "");
  if (!((reason == "eos" && hint == "EOS" && ended) ||
        (reason == "max_tokens" && hint == "MAX_TOKENS" && !ended && tokens.size() == maximum)))
    fail("invalid-stop-reason");
  const auto requester = readFile(root / ("requester-" + std::to_string(round) + ".log"));
  const auto eventRecords = recordsContaining(requester, "NATIVE_STREAM_EVENTS=");
  if (eventRecords.size() != 1) fail("stream-count-missing-or-duplicate");
  const auto count = field(eventRecords.front(), "NATIVE_STREAM_EVENTS");
  if (count.empty() || count.find_first_not_of("0123456789") != std::string::npos ||
      std::stoull(count) != tokens.size()) fail("stream-count-mismatch");
  if (recordsContaining(requester, "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN").size() != 1)
    fail("checkpoint-missing-or-duplicate");
  return NativeJson{{"generatedTokens", tokens.size()}, {"maxNewTokens", maximum},
                    {"finishReason", reason}};
}

NativeJson validateRounds(const std::filesystem::path& root, unsigned rounds,
                         bool cacheCompatibility, bool requireMultiToken)
{
  std::set<std::string> requestIds;
  NativeJson receipt{{"schema", "spec189-cpp-oracle-v1"}, {"providers", 2},
                     {"terminal", true}, {"rounds", rounds}, {"roundResults", NativeJson::array()}};
  NativeJson previous, initial;
  std::map<std::string, NativeJson> roleBindings;
  RoundEvidence view;
  for (unsigned round = 0; round < rounds; ++round) {
    const auto logPath = root / ("requester-" + std::to_string(round) + ".log");
    if (!std::filesystem::is_regular_file(logPath)) chainFailure("missing-round-file");
    const auto requester = readFile(logPath);
    const auto successes = recordsContaining(requester, "NATIVE_REQUEST_SUCCEEDED");
    if (successes.size() != 1) chainFailure("requester-not-success");
    auto success = markerFields(successes.front(), "NATIVE_REQUEST_SUCCEEDED");
    const auto requestId = success["request"], planDigest = success["plan"];
    if (requestId.empty() || planDigest.empty()) chainFailure("requester-identity-missing");
    if (!requestIds.insert(requestId).second) chainFailure("duplicate-request-id");
    if (requester.find("NATIVE_REQUEST_FAILED") != std::string::npos ||
        requester.find("NATIVE_REQUEST_STAGE_FAILED") != std::string::npos)
      chainFailure("requester-failed");
    const auto commits = recordsContaining(requester, "NDNSF_DI_NATIVE_SELECTION_COMMITTED");
    if (commits.size() != 1) chainFailure("selection-commit-missing-or-duplicate");
    auto commit = markerFields(commits.front(), "NDNSF_DI_NATIVE_SELECTION_COMMITTED");
    if (commit["requestId"] != requestId || commit["planDigest"] != planDigest ||
        commit["attemptEpoch"].empty()) chainFailure("identity-mismatch");
    if (cacheCompatibility) validateCacheCompatibilityMode(root, round);
    view.write("requester-0.log", requester);
    for (unsigned provider = 0; provider < 2; ++provider) {
      const auto name = "provider-" + std::to_string(provider) + ".log";
      const auto scoped = scopeProvider(readFile(root / name), requestId);
      const auto selections = recordsContaining(scoped, "NDNSF_DI_NATIVE_SELECTION_ACCEPTED");
      if (selections.size() != 1) chainFailure("round-selection-missing-or-duplicate");
      auto selected = markerFields(selections.front(), "NDNSF_DI_NATIVE_SELECTION_ACCEPTED");
      if (selected["planDigest"] != planDigest || selected["attemptEpoch"] != commit["attemptEpoch"])
        chainFailure("identity-mismatch");
      view.write(name, scoped);
    }
    // All existing full-path material and authorization gates remain in force;
    // suppress intermediate PASS so a bad later round cannot yield any PASS.
    validatePlacementOnly(view.root, cacheCompatibility, false);
    std::string observedRequest = requestId, observedPlan = planDigest;
    bool terminal = false;
    for (unsigned provider = 0; provider < 2; ++provider)
      validateProvider(view.root / ("provider-" + std::to_string(provider) + ".log"),
        "/example/ndnsf-qwen06b/provider-" + std::to_string(provider),
        observedRequest, observedPlan, terminal);
    if (!terminal) chainFailure("tail-terminal-missing");

    const auto generation = validateMultiTokenOutput(root, round, requireMultiToken);
    const auto config = readJson(root / "requester" / roundJson("config", round));
    const auto checkpoint = readJson(root / "requester" / roundJson("conversation-state", round));
    const auto& conversation = config.at("conversation");
    const auto& turn = conversation.at("turn");
    if (conversation.value("checkpoint_output_file", "") != roundJson("conversation-state", round))
      chainFailure("checkpoint-output-path-mismatch");
    if (checkpoint.value("schema", "") != "ndnsf-di-conversation-checkpoint-v1" ||
        nonnegativeInteger(checkpoint.at("version")) != 1 ||
        nonnegativeInteger(checkpoint.at("contextEpoch")) != round + 1 ||
        nonnegativeInteger(checkpoint.at("parentContextEpoch")) != round)
      chainFailure("checkpoint-epoch-mismatch");
    for (const auto* key : {"conversationId", "serviceName", "requesterIdentity", "modelContractDigest",
                            "securityDomainDigest", "planRoleMapDigest", "checkpointDigest"}) {
      if (!checkpoint.at(key).is_string() || checkpoint.at(key).get<std::string>().empty())
        chainFailure("checkpoint-binding-missing");
      if (round && std::string(key) != "checkpointDigest" && checkpoint.at(key) != initial.at(key))
        chainFailure("checkpoint-binding-mismatch");
    }
    const auto& owner = conversation.at("owner");
    if (owner.at("requester_identity") != checkpoint.at("requesterIdentity") ||
        owner.at("service_name") != checkpoint.at("serviceName") ||
        owner.at("security_domain_digest") != checkpoint.at("securityDomainDigest") ||
        config.at("request").at("service") != checkpoint.at("serviceName") ||
        config.at("request").at("security_policy_digest") != checkpoint.at("securityDomainDigest"))
      chainFailure("config-binding-mismatch");
    std::uint64_t parentCount = 0;
    std::size_t inputCount = 0;
    if (round == 0) {
      if (turn.value("mode", "") != "FULL_CONTEXT" || turn.contains("parent_state_file") ||
          nonnegativeInteger(turn.at("parent_context_epoch")) != 0 ||
          turn.at("conversation_id") != checkpoint.at("conversationId"))
        chainFailure("initial-context-mismatch");
      inputCount = tokenCount(turn.at("canonical_token_ids"));
      initial = checkpoint;
    }
    else {
      if (turn.value("mode", "") != "APPEND_DELTA" ||
          turn.value("parent_state_file", "") != roundJson("conversation-state", round - 1) ||
          (turn.contains("parent_context_epoch") && nonnegativeInteger(turn.at("parent_context_epoch")) != round) ||
          (turn.contains("conversation_id") && turn.at("conversation_id") != initial.at("conversationId")) ||
          (turn.contains("parent_checkpoint_digest") && turn.at("parent_checkpoint_digest") != previous.at("checkpointDigest")))
        chainFailure("parent-config-mismatch");
      parentCount = nonnegativeInteger(previous.at("prefixTokenCount"));
      inputCount = tokenCount(turn.at("delta_token_ids"));
    }
    const auto count = nonnegativeInteger(checkpoint.at("prefixTokenCount"));
    if (count < parentCount || count - parentCount != inputCount + generation.at("generatedTokens").get<std::size_t>())
      chainFailure("checkpoint-prefix-mismatch");
    const auto& roleReceipts = checkpoint.at("roleReceiptDigests");
    if (!roleReceipts.is_object() || roleReceipts.size() != 2) chainFailure("checkpoint-roles-mismatch");
    for (unsigned provider = 0; provider < 2; ++provider) {
      const auto path = view.root / ("provider-" + std::to_string(provider) + ".log");
      const auto text = readFile(path);
      const auto placement = validatePlacementProvider(path,
        "/example/ndnsf-qwen06b/provider-" + std::to_string(provider));
      const auto& s = placement.selection;
      if (!roleReceipts.contains(s.role) || !roleReceipts.at(s.role).is_string() ||
          roleReceipts.at(s.role).get<std::string>().empty()) chainFailure("checkpoint-roles-mismatch");
      const NativeJson binding{{"role", s.role}, {"manifest", s.manifestDigest}, {"graph", s.graphDigest},
        {"initializer", s.initializerDigest}, {"artifact", s.artifactDigest}, {"begin", s.layerBegin}, {"end", s.layerEnd}};
      if (!round) roleBindings[s.provider] = binding;
      else if (roleBindings.at(s.provider) != binding) chainFailure("placement-binding-changed");
      if (!round) continue;
      const auto restores = recordsContaining(text, "NDNSF_DI_CONVERSATION_KV_RESTORED");
      if (restores.size() != 1) chainFailure("restore-missing-or-duplicate");
      auto restore = markerFields(restores.front(), "NDNSF_DI_CONVERSATION_KV_RESTORED");
      if (restore["requestId"] != requestId || restore["role"] != s.role ||
          restore["parentContextEpoch"] != std::to_string(round) ||
          restore["prefixTokenCount"] != std::to_string(parentCount))
        chainFailure("restore-binding-mismatch");
      const auto restoredAt = lineContaining(text, restores.front());
      const auto stages = markers(text);
      const auto completed = std::find_if(stages.begin(), stages.end(), [](const Marker& marker) {
        return marker.stage == "EXECUTION_COMPLETED";
      });
      if (restoredAt <= placement.grantLine || completed == stages.end() || restoredAt >= completed->line)
        chainFailure("restore-outside-execution");
    }
    receipt["roundResults"].push_back(NativeJson{{"round", round}, {"requestId", requestId},
      {"planDigest", planDigest}, {"generation", generation}, {"contextEpoch", round + 1},
      {"parentContextEpoch", round}, {"prefixTokenCount", count}});
    previous = checkpoint;
  }
  return receipt;
}

void
requireRepo(bool condition, const std::string& reason)
{
  if (!condition) {
    throw std::runtime_error("SPEC190_CPP_REPO_ORACLE_FAIL boundary=REPO reason=" + reason);
  }
}

std::vector<uint8_t>
repoPayload(std::size_t size, uint8_t seed)
{
  std::vector<uint8_t> payload(size);
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<uint8_t>((index * 29U + seed) & 0xffU);
  }
  return payload;
}

std::shared_ptr<RepoCore>
openRepoOracle(const std::filesystem::path& root,
               const std::string& owner,
               std::shared_ptr<FilesystemRepoStoreBackend>& backend)
{
  StorageCapability capability;
  capability.repoNode = "/spec190/t010/repo-oracle";
  capability.freeBytes = 16U * 1024U * 1024U;
  capability.repoMode = "persistent";
  capability.storageClasses = {"model", "intermediate"};
  backend = std::make_shared<FilesystemRepoStoreBackend>(
    root.string(), 4U * 1024U * 1024U, 1U * 1024U * 1024U, owner);
  return std::make_shared<RepoCore>(std::move(capability), backend);
}

void
validateRepoLifecycle(const std::filesystem::path& repoRoot)
{
  requireRepo(repoRoot.is_absolute(), "repo-root-must-be-absolute");
  requireRepo(repoRoot != repoRoot.root_path(), "repo-root-must-not-be-filesystem-root");
  std::error_code error;
  std::filesystem::create_directories(repoRoot, error);
  requireRepo(!error, "repo-root-create-failed");
  std::filesystem::permissions(repoRoot, std::filesystem::perms::owner_all,
                                std::filesystem::perm_options::replace, error);
  requireRepo(!error, "repo-root-permissions-failed");

  // This marker is outside the disposable workload tree. Its survival proves
  // the fixed Repo root is not treated as run-scoped cleanup staging.
  const auto fixedRootMarker = repoRoot / ".spec190-fixed-repo-root";
  if (!std::filesystem::exists(fixedRootMarker)) {
    std::ofstream marker(fixedRootMarker);
    marker << "spec190-fixed-repo-root-v1\n";
    requireRepo(static_cast<bool>(marker), "repo-root-marker-write-failed");
  }

  const auto uniqueTicks = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto uniqueSuffix = std::to_string(::getpid()) + "-" +
    std::to_string(uniqueTicks);
  const std::string objectName =
    "/example/ndnsf/spec190/t010/repo-oracle/" + uniqueSuffix;
  const std::string unrelatedName = objectName + "/unrelated";
  const auto unrelatedPayload = repoPayload(17, 3);
  const auto payloadSeed = static_cast<uint8_t>(uniqueTicks & 0xff);
  const auto firstPayload = repoPayload(4096, payloadSeed);
  const auto replacementPayload = repoPayload(4224, static_cast<uint8_t>(payloadSeed ^ 0x5a));
  std::shared_ptr<FilesystemRepoStoreBackend> backend;
  auto repo = openRepoOracle(repoRoot, "spec190-t010-oracle", backend);
  const auto baselineBytes = backend->usedBytes();

  const auto unrelated = repo->put(unrelatedName, unrelatedPayload, "stale-fixture");
  requireRepo(unrelated.objectName == unrelatedName &&
                repo->get(unrelatedName) == unrelatedPayload,
              "unrelated-existing-data-round-trip-failed");
  const auto beforeCandidateBytes = backend->usedBytes();

  RepoObjectManifest first;
  first.objectName = objectName;
  first.objectType = "model-stage";
  first.sha256 = sha256Hex(firstPayload);
  first.size = firstPayload.size();
  first.segmentCount = 1;
  first.policyEpoch = "spec190-t010";
  const auto storeWire = encodeStoreRequest(first, firstPayload);
  RepoObjectManifest decodedFirst;
  std::vector<uint8_t> decodedPayload;
  decodeStoreRequest(storeWire, decodedFirst, decodedPayload);
  requireRepo(decodedFirst.objectName == first.objectName &&
                decodedFirst.sha256 == first.sha256 && decodedPayload == firstPayload,
              "store-wire-decode-mismatch");
  const auto firstReply = parseManifestJson(toString(repo->handleStore(storeWire)));
  requireRepo(firstReply.objectName == objectName &&
                firstReply.sha256 == first.sha256 && firstReply.size == firstPayload.size(),
              "store-wire-commit-mismatch");
  requireRepo(repo->handleFetch(toBytes(objectName)) == firstPayload,
              "store-wire-fetch-mismatch");
  const auto firstManifest = parseManifestJson(
    toString(repo->handleManifest(toBytes(objectName))));
  requireRepo(firstManifest.toJson() == firstReply.toJson(),
              "manifest-query-mismatch");
  const auto catalog = parseCatalogEntryJson(toString(repo->handleCatalogLookup(
    encodeCatalogLookupRequest(objectName))));
  requireRepo(catalog.manifest.objectName == objectName &&
                catalog.manifest.sha256 == firstReply.sha256 && catalog.state == "AVAILABLE",
              "catalog-query-mismatch");
  requireRepo(backend->usedBytes() == beforeCandidateBytes + firstPayload.size(),
              "initial-payload-accounting-mismatch");

  auto replacement = first;
  replacement.sha256 = sha256Hex(replacementPayload);
  replacement.size = replacementPayload.size();
  replacement.generation = 0;
  const auto replacementReply = parseManifestJson(toString(repo->handleStore(
    encodeStoreRequest(replacement, replacementPayload))));
  requireRepo(replacementReply.objectName == objectName &&
                replacementReply.sha256 == replacement.sha256 &&
                replacementReply.generation > firstReply.generation,
              "replacement-manifest-mismatch");
  requireRepo(repo->handleFetch(toBytes(objectName)) == replacementPayload,
              "replacement-wire-fetch-mismatch");
  requireRepo(backend->usedBytes() == beforeCandidateBytes + replacementPayload.size(),
              "payload-delta-double-counted");
  const auto inventory = toString(repo->handleInventory());
  requireRepo(inventory.find(objectName) != std::string::npos &&
                inventory.find(unrelatedName) != std::string::npos,
              "inventory-lost-existing-data");

  bool competingWriterRejected = false;
  try {
    auto competing = std::make_shared<FilesystemRepoStoreBackend>(
      repoRoot.string(), 4U * 1024U * 1024U, 1U * 1024U * 1024U, "spec190-t010-competing");
    competing.reset();
  }
  catch (const std::exception& exception) {
    competingWriterRejected = std::string(exception.what()).find(
      "repo-persistence-owned") != std::string::npos;
  }
  requireRepo(competingWriterRejected, "single-writer-owner-not-enforced");

  repo.reset();
  backend.reset();
  requireRepo(std::filesystem::is_regular_file(fixedRootMarker),
              "fixed-repo-root-marker-removed");

  std::shared_ptr<FilesystemRepoStoreBackend> restartedBackend;
  auto restarted = openRepoOracle(repoRoot, "spec190-t010-restarted", restartedBackend);
  const auto restartedManifest = parseManifestJson(
    toString(restarted->handleManifest(toBytes(objectName))));
  requireRepo(restartedManifest.sha256 == replacementReply.sha256 &&
                restarted->handleFetch(toBytes(objectName)) == replacementPayload,
              "restart-read-mismatch");
  const auto restartedCatalog = parseCatalogEntryJson(toString(
    restarted->handleCatalogLookup(encodeCatalogLookupRequest(objectName))));
  requireRepo(restartedCatalog.manifest.sha256 == replacementReply.sha256 &&
                restartedCatalog.state == "AVAILABLE",
              "restart-query-mismatch");
  const auto restartedInventory = toString(restarted->handleInventory());
  requireRepo(restartedInventory.find(objectName) != std::string::npos &&
                restartedInventory.find(unrelatedName) != std::string::npos &&
                restartedBackend->usedBytes() == beforeCandidateBytes + replacementPayload.size(),
              "restart-payload-accounting-mismatch");
  requireRepo(restarted->get(unrelatedName) == unrelatedPayload,
              "restart-existing-data-mismatch");

  const auto receipt = NativeJson{
    {"schema", "spec190-cpp-repo-oracle-v1"},
    {"repoRoot", repoRoot.string()},
    {"objectName", objectName},
    {"baselineBytes", baselineBytes},
    {"beforeCandidateBytes", beforeCandidateBytes},
    {"firstPayloadBytes", firstPayload.size()},
    {"replacementPayloadBytes", replacementPayload.size()},
    {"replacementGeneration", replacementReply.generation},
    {"restartedBytes", restartedBackend->usedBytes()},
    {"wireStoreBytes", storeWire.size()},
    {"singleWriter", "PASS"},
    {"restartRead", "PASS"},
    {"payloadDelta", "PASS"},
  };
  std::cout << "SPEC190_CPP_REPO_ORACLE_PASS "
            << ndnsf::di::nativeCanonicalJson(receipt) << '\n';
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "usage: " << argv[0]
              << " [--placement-only | --cache-compatibility | --expect-revocation-failure] [--require-multi-token] [--rounds 1..8] --run-root DIRECTORY\n"
              << "       " << argv[0] << " --repo-lifecycle --repo-root DIRECTORY\n";
    return 0;
  }
  bool placementOnly = false, cacheCompatibility = false, expectedRevocationFailure = false,
       multiToken = false, repoLifecycle = false, invalidOptions = false;
  unsigned rounds = 1;
  bool roundsSeen = false;
  const char* runRootArg = nullptr;
  const char* repoRootArg = nullptr;
  for (int i = 1; i < argc; ++i) {
    const std::string option(argv[i]);
    if (option == "--placement-only" && !placementOnly) placementOnly = true;
    else if (option == "--cache-compatibility" && !cacheCompatibility) cacheCompatibility = true;
    else if (option == "--expect-revocation-failure" && !expectedRevocationFailure)
      expectedRevocationFailure = true;
    else if (option == "--require-multi-token" && !multiToken) multiToken = true;
    else if (option == "--repo-lifecycle" && !repoLifecycle) repoLifecycle = true;
    else if (option == "--run-root" && !runRootArg && i + 1 < argc) runRootArg = argv[++i];
    else if (option == "--repo-root" && !repoRootArg && i + 1 < argc) repoRootArg = argv[++i];
    else if (option == "--rounds" && !roundsSeen && i + 1 < argc) {
      roundsSeen = true;
      const std::string count(argv[++i]);
      if (count.size() != 1 || count[0] < '1' || count[0] > '8') invalidOptions = true;
      else rounds = static_cast<unsigned>(count[0] - '0');
    }
    else invalidOptions = true;
  }
  if (invalidOptions ||
      (repoLifecycle && (repoRootArg == nullptr || runRootArg != nullptr ||
                         placementOnly || cacheCompatibility || expectedRevocationFailure ||
                         multiToken || roundsSeen)) ||
      (!repoLifecycle && (runRootArg == nullptr || repoRootArg != nullptr)) ||
      (placementOnly && (cacheCompatibility || expectedRevocationFailure || multiToken || rounds > 1)) ||
      (expectedRevocationFailure && (placementOnly || cacheCompatibility || multiToken || roundsSeen))) {
    std::cerr << "usage: " << argv[0]
              << " [--placement-only | --cache-compatibility | --expect-revocation-failure] [--require-multi-token] [--rounds 1..8] --run-root DIRECTORY\n"
              << "       " << argv[0] << " --repo-lifecycle --repo-root DIRECTORY\n";
    return 2;
  }
  try {
    if (repoLifecycle) {
      validateRepoLifecycle(std::filesystem::absolute(repoRootArg));
      return 0;
    }
    const auto root = std::filesystem::absolute(runRootArg);
    if (expectedRevocationFailure) {
      validateRevocationFailure(root);
      return 0;
    }
    if (rounds > 1) {
      auto receipt = validateRounds(root, rounds, cacheCompatibility, multiToken);
      if (cacheCompatibility) {
        receipt["scope"] = "cache-compatible-execution";
        receipt["materialFetch"] = "NOT_EVALUATED";
        receipt["fullPathQualification"] = "NOT_RUN";
      }
      std::cout << (cacheCompatibility ? "SPEC189_CPP_CACHE_DIAGNOSTIC_PASS " : "SPEC189_CPP_ORACLE_PASS ")
                << ndnsf::di::nativeCanonicalJson(receipt) << '\n';
      return 0;
    }
    if (placementOnly) {
      validatePlacementOnly(root);
      return 0;
    }
    if (cacheCompatibility) validateCacheCompatibilityMode(root);
    // The default full execution path retains the placement material gate.
    validatePlacementOnly(root, cacheCompatibility);
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
    if (multiToken) receipt["generation"] = validateMultiTokenOutput(root);
    if (cacheCompatibility) {
      receipt["scope"] = "cache-compatible-execution";
      receipt["materialFetch"] = "NOT_EVALUATED";
      receipt["fullPathQualification"] = "NOT_RUN";
    }
    std::cout << (cacheCompatibility ? "SPEC189_CPP_CACHE_DIAGNOSTIC_PASS " : "SPEC189_CPP_ORACLE_PASS ")
              << ndnsf::di::nativeCanonicalJson(receipt) << '\n';
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
