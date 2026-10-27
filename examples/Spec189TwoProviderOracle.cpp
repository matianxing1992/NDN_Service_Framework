// Spec189 C++ full-path oracle.
//
// The MiniNDN runner owns process and topology orchestration.  This executable
// owns the product assertion: it reads the immutable logs emitted by the real
// requester and Providers, checks the authenticated post-Selection stage
// sequence, and rejects a generic requester timeout as a successful result.

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGenerationLimits.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "Spec189MaterialFetchOracle.hpp"
#include "Spec189ProviderStageOracle.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>
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
NativeJson validateMultiTokenOutput(const std::filesystem::path& root, unsigned round = 0,
                                    bool requireMultiToken = true,
                                    const NativeJson* requesterStatistics = nullptr);

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
    const auto liveRunnerHits = recordsContaining(text,
      "NDNSF_DI_PROVIDER_PREPARATION phase=RUNNER_REUSE_HIT");
    const auto cacheAcquire = acquireHits.empty() ? std::string{} :
      field(acquireHits.front(), "detail");
    // A cold provider has a legitimate two-marker cache sequence: the
    // recipe lookup misses, then the material fetch/assembly completes and
    // publishes an assembled-built result. That is not an ambiguous cache
    // outcome; it is the required cold path before runner publication.
    const bool coldAssembly = acquireHits.size() == 1 && misses.size() == 1 &&
      cacheAcquire == "assembled-built";
    if (liveRunnerHits.size() > 1 || acquireHits.size() > 1 ||
        legacyHits.size() > 1 || misses.size() > 1 ||
        (!liveRunnerHits.empty() &&
         (!acquireHits.empty() || !legacyHits.empty() || !misses.empty())) ||
        (!acquireHits.empty() && !misses.empty() && !coldAssembly) ||
        (acquireHits.empty() && !legacyHits.empty() && !misses.empty())) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=CACHE_LOOKUP reason=ambiguous-cache-result log=" +
        path.string());
    }
    if (!liveRunnerHits.empty()) {
      const auto& hit = liveRunnerHits.front();
      if (field(hit, "requestId") != placement.selection.requestId ||
          field(hit, "provider") != expectedProvider ||
          field(hit, "role") != placement.selection.role ||
          field(hit, "detail") != "live-runner") {
        throw std::runtime_error(
          "SPEC189_CPP_ORACLE_FAIL boundary=CACHE_LOOKUP reason=live-runner-identity-mismatch log=" +
          path.string());
      }
      if (text.find("NDNSF_DI_PROVIDER_MATERIAL_FETCH") != std::string::npos) {
        throw std::runtime_error(
          "SPEC189_CPP_ORACLE_FAIL boundary=CACHE_LOOKUP reason=material-events-on-live-runner-hit log=" +
          path.string());
      }
      // A live runner hit is stronger than an assembled/template hit: model
      // material, plaintext staging, ORT session and runner construction were
      // all completed by the prior turn. Only the current authorization and
      // execution path remain observable before inference.
      return MaterialFetchObservation{};
    }
    if (coldAssembly) {
      const auto& built = acquireHits.front();
      if (field(built, "requestId") != placement.selection.requestId ||
          field(built, "provider") != expectedProvider ||
          field(built, "role") != placement.selection.role) {
        throw std::runtime_error(
          "SPEC189_CPP_ORACLE_FAIL boundary=CACHE_LOOKUP reason=identity-mismatch log=" +
          path.string());
      }
      return validateMaterialFetches(path, placement, expectedProvider);
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

NativeJson
validateGenerationOutput(const NativeJson& request, const NativeJson& options,
                         const NativeJson& output, bool requireMultiToken,
                         const NativeJson* requesterStatistics)
{
  using ndnsf::di::NativeJson;
  const auto fail = [](const std::string& reason) {
    throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=GENERATION reason=" + reason);
  };
  if ((request.contains("allow_replacement") && request.at("allow_replacement") != false) ||
      (request.contains("max_replacements") && request.at("max_replacements") != 0))
    fail("unsupported-request-config");
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
  if (requesterStatistics) {
    if (requesterStatistics->at("streaming") != true ||
        requesterStatistics->at("terminalSeen") != true ||
        requesterStatistics->at("checkpointWritten") != true ||
        nonnegativeInteger(requesterStatistics->at("observedEvents")) != tokens.size())
      fail("stream-statistics-mismatch");
  }
  return NativeJson{{"generatedTokens", tokens.size()}, {"maxNewTokens", maximum},
                    {"finishReason", reason}};
}

NativeJson
validateMultiTokenOutput(const std::filesystem::path& root, unsigned round,
                         bool requireMultiToken, const NativeJson* requesterStatistics)
{
  const auto config = readJson(root / "requester" / roundJson("config", round));
  const auto& request = config.at("request");
  if (request.value("options_file", "") != roundJson("options", round))
    chainFailure("unsupported-request-config");
  const auto result = validateGenerationOutput(request,
    readJson(root / "requester" / roundJson("options", round)),
    readJson(root / "requester" / ("output-" + std::to_string(round) + ".bin")),
    requireMultiToken, requesterStatistics);
  if (!requesterStatistics) {
    const auto requester = readFile(root / ("requester-" + std::to_string(round) + ".log"));
    const auto eventRecords = recordsContaining(requester, "NATIVE_STREAM_EVENTS=");
    if (eventRecords.size() != 1) chainFailure("stream-count-missing-or-duplicate");
    const auto count = field(eventRecords.front(), "NATIVE_STREAM_EVENTS");
    if (count.empty() || count.find_first_not_of("0123456789") != std::string::npos ||
        std::stoull(count) != result.at("generatedTokens").get<std::size_t>())
      chainFailure("stream-count-mismatch");
    if (recordsContaining(requester, "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN").size() != 1)
      chainFailure("checkpoint-missing-or-duplicate");
  }
  return result;
}

struct StatisticsRows
{
  std::vector<NativeJson> requests;
  std::string provider;
  std::string boot;
  std::string actor;
  std::string host;
};

StatisticsRows
readStatistics(const std::filesystem::path& path, const std::string& schema,
               unsigned rounds, bool provider)
{
  if (!std::filesystem::is_regular_file(path) ||
      std::filesystem::file_size(path) > 64U * 1024U * 1024U)
    chainFailure("statistics-file-unavailable-or-oversize");
  std::ifstream input(path);
  StatisticsRows rows;
  std::string line;
  unsigned ready = 0;
  while (std::getline(input, line)) {
    if (input.eof()) chainFailure("statistics-truncated-row");
    NativeJson row;
    try {
      row = ndnsf::di::nativeParseJson(line);
    }
    catch (const std::invalid_argument&) {
      chainFailure("statistics-json");
    }
    if (!row.is_object() || row.value("schema", "") != schema)
      chainFailure("statistics-schema");
    if (provider && row.value("type", "") == "serve-ready") {
      if (++ready != 1) chainFailure("statistics-ready-count");
      rows.provider = row.at("provider").get<std::string>();
      rows.boot = row.at("providerBootId").get<std::string>();
      if (rows.provider.empty() || rows.boot.empty()) chainFailure("statistics-ready-binding");
    }
    else {
      if (row.value("type", "") != "request" || rows.requests.size() >= rounds)
        chainFailure("statistics-request-count");
      rows.requests.push_back(row);
    }
  }
  if (!input.eof() || rows.requests.size() != rounds || (provider && ready != 1))
    chainFailure("statistics-request-count");
  return rows;
}

using StatisticsInterval = std::pair<std::uint64_t, std::uint64_t>;

std::optional<StatisticsInterval>
statisticsInterval(const NativeJson& row, const char* begin, const char* end,
                   bool allowMissing = false)
{
  if (allowMissing && row.at(begin).is_null() && row.at(end).is_null()) return std::nullopt;
  const auto first = nonnegativeInteger(row.at(begin));
  const auto last = nonnegativeInteger(row.at(end));
  if (last < first || (!allowMissing && (first == 0 || first == last)))
    chainFailure("statistics-clock-order");
  return StatisticsInterval{first, last};
}

void
validateProviderStatistics(const StatisticsRows& source, const NativeJson& row,
                           const std::string& request, const std::string& plan,
                           const std::string& expectedProvider)
{
  if (source.provider != expectedProvider || row.at("provider") != expectedProvider ||
      row.at("providerBootId") != source.boot || row.at("requestId") != request ||
      row.at("planDigest") != plan || row.at("role").get<std::string>().empty() ||
      nonnegativeInteger(row.at("attemptEpoch")) == 0)
    chainFailure("statistics-binding");
  if (row.at("completionAdmitted") != true || !row.at("failureReason").is_null() ||
      row.at("hasEpochStatistics") != true)
    chainFailure("statistics-incomplete-request");
  if (row.at("counterSemantics") != "cumulative-adapter-snapshots")
    chainFailure("statistics-schema");
  const auto wall = *statisticsInterval(row, "startedSteadyNs", "finishedSteadyNs");
  const auto cpu = statisticsInterval(row, "startedProcessCpuNs", "finishedProcessCpuNs", true);
  const auto count = nonnegativeInteger(row.at("epochsExecuted"));
  const auto& metrics = row.at("runtimeMetrics");
  const auto& cache = row.at("cacheObservations");
  if (count == 0 || count > ndnsf::di::MAX_NATIVE_GENERATED_TOKENS + 32 ||
      !metrics.is_array() || metrics.size() != count ||
      !cache.is_array() || cache.size() != count)
    chainFailure("statistics-epoch-count");
  std::uint64_t previousEnd = wall.first;
  std::optional<std::uint64_t> previousCpuEnd;
  if (cpu) previousCpuEnd = cpu->first;
  std::string session;
  for (std::size_t epoch = 0; epoch < count; ++epoch) {
    const auto& observation = metrics.at(epoch);
    const auto& timing = observation.at("lastBackendExecution");
    if (timing.is_null()) chainFailure("statistics-missing-backend");
    if (observation.at("role") != row.at("role") ||
        nonnegativeInteger(observation.at("inferenceEpoch")) != epoch ||
        timing.at("requestId") != request || timing.at("role") != row.at("role") ||
        timing.at("providerBootId") != source.boot ||
        timing.at("attemptEpoch") != row.at("attemptEpoch") ||
        nonnegativeInteger(timing.at("inferenceEpoch")) != epoch ||
        timing.at("completed") != true ||
        nonnegativeInteger(cache.at(epoch).at("inferenceEpoch")) != epoch)
      chainFailure("statistics-backend-binding");
    const auto currentSession = timing.at("sessionId").get<std::string>();
    if (currentSession.empty() || (epoch && currentSession != session))
      chainFailure("statistics-backend-binding");
    session = currentSession;
    const auto callWall = *statisticsInterval(timing, "startedSteadyNs", "finishedSteadyNs");
    const auto callCpu = statisticsInterval(timing, "startedProcessCpuNs",
                                             "finishedProcessCpuNs", true);
    statisticsInterval(timing, "startedThreadCpuNs", "finishedThreadCpuNs", true);
    if (callWall.first < previousEnd || callWall.second > wall.second ||
        (cpu && callCpu && (callCpu->first < cpu->first || callCpu->second > cpu->second)) ||
        (callCpu && previousCpuEnd && callCpu->first < *previousCpuEnd))
      chainFailure("statistics-backend-window");
    previousEnd = callWall.second;
    if (callCpu) previousCpuEnd = callCpu->second;
  }
}

NativeJson
readPrivateJson(const std::filesystem::path& path)
{
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_uid != ::getuid() || (info.st_mode & 0777) != 0600 ||
      info.st_size > 64 * 1024 * 1024 ||
      std::filesystem::canonical(path) != path.lexically_normal())
    chainFailure("deployment-private-evidence");
  return readJson(path);
}

std::string
managementRoleId(const std::string& kind, const std::string& node)
{
  const auto value = kind + "." + node;
  return value.size() <= 128 ? value : kind + "." + ndnsf::di::nativePlanningDigest(node).substr(7);
}

bool
matchesHost(const std::string& configured, const std::string& observed)
{
  return configured == observed || configured == observed.substr(0, observed.find('.'));
}

bool
matchesJob(const std::string& cgroup, const std::string& job, const std::string& step = "")
{
  if (job.empty() || job.find_first_not_of("0123456789") != std::string::npos ||
      (!step.empty() && step.find_first_not_of("0123456789") != std::string::npos))
    return false;
  return std::regex_search(cgroup, std::regex("(?:^|/)job[_-]" + job + "(?:/|\\.)")) &&
    (step.empty() || std::regex_search(cgroup,
      std::regex("(?:^|/)step[_-]" + step + "(?:/|\\.|$)")));
}

std::set<std::string>
validateDeploymentGroups(const NativeJson& state, const std::string& boot,
                         const std::string& digest, bool strictIdentity)
{
  std::set<std::string> launched, cleaned;
  for (const auto& launch : state.at("launches")) {
    const auto id = launch.at("id").get<std::string>();
    const auto& proof = launch.at("proof");
    const auto& bindings = proof.at("bindings");
    if (!launched.insert(id).second ||
        bindings.at("NDNSF_DEPLOYMENT_ACTOR_ID") != id ||
        bindings.at("NDNSF_DEPLOYMENT_BOOT_ID") != boot ||
        bindings.at("NDNSF_DEPLOYMENT_CONFIG_SHA256") != digest ||
        launch.at("bindings") != bindings)
      chainFailure("deployment-process-binding");
    if (strictIdentity && (nonnegativeInteger(proof.at("pid")) <= 1 ||
        nonnegativeInteger(proof.at("uid")) != ::getuid() ||
        nonnegativeInteger(proof.at("start_ticks")) == 0 ||
        nonnegativeInteger(proof.at("pgid")) != nonnegativeInteger(proof.at("pid")) ||
        !std::filesystem::path(proof.at("exe").get<std::string>()).is_absolute()))
      chainFailure("deployment-process-identity");
  }
  for (const auto& item : state.at("cleanup")) {
    if (item.at("status") != "ZERO" || item.at("forced") != false ||
        !cleaned.insert(item.at("id").get<std::string>()).second)
      chainFailure("deployment-cleanup");
  }
  if (launched.empty() || cleaned != launched)
    chainFailure("deployment-cleanup-coverage");
  return launched;
}

std::map<std::string, NativeJson>
validateAllocation(const std::filesystem::path& root, const NativeJson& state,
                   const NativeJson& config, const std::string& boot,
                   const std::string& digest)
{
  const auto job = state.at("job_id").get<std::string>();
  const auto release = state.at("release_sha256").get<std::string>();
  if (state.at("execution") != "COMPLETE" ||
      state.at("evidence_root") != (root / "supervisor" /
        config.at("deployment_id").get<std::string>()).string() ||
      ndnsf::di::nativePlanningDigest(readFile(config.at("release").at("manifest")
        .get<std::string>())) != "sha256:" + release)
    chainFailure("deployment-allocation-binding");
  const auto operation = readPrivateJson(root / "operation.json");
  const auto& reference = operation.at("job_reference");
  const auto& scheduler = operation.at("scheduler");
  if (operation.at("schema") != "ndnsf-di-slurm-operation-v1" ||
      operation.at("deployment_id") != config.at("deployment_id") ||
      operation.at("evidence_root") != root.string() ||
      operation.at("boot_id") != boot || operation.at("configuration_sha256") != digest ||
      reference.at("boot_id") != boot || reference.at("configuration_sha256") != digest ||
      reference.at("release_sha256") != release || reference.at("job_id") != job ||
      nonnegativeInteger(reference.at("owner_uid")) != ::getuid() ||
      scheduler.at("reference") != reference || scheduler.at("terminal") != true ||
      scheduler.at("status") != "TERMINAL" || scheduler.at("state") != "COMPLETED" ||
      scheduler.at("exit_code") != "0:0")
    chainFailure("deployment-scheduler-binding");
  const auto record = readJson(config.at("platform").at("build_record").get<std::string>());
  auto unsignedRecord = record;
  unsignedRecord.erase("recordDigest");
  if (record.at("schemaVersion") != "ndnsf-local-sif-build-v3" ||
      (record.at("status") != "BUILT_UNQUALIFIED" && record.at("status") != "PASS") ||
      nonnegativeInteger(record.at("sif").at("bytes")) == 0 ||
      record.at("sif").at("sha256") != config.at("platform").at("sif_sha256") ||
      record.at("recordDigest") != ndnsf::di::nativePlanningDigest(
        ndnsf::di::nativeCanonicalJson(unsignedRecord)))
    chainFailure("deployment-image-binding");

  const auto checkBinding = [&] (const NativeJson& value) {
    if (value.at("boot_id") != boot || value.at("configuration_sha256") != digest ||
        value.at("release_sha256") != release || value.at("job_id") != job)
      chainFailure("deployment-node-binding");
  };
  std::map<std::string, NativeJson> nodes, roles, receipts;
  for (const auto& node : config.at("nodes")) {
    const auto id = node.at("id").get<std::string>();
    if (!nodes.emplace(id, node).second) chainFailure("deployment-node-coverage");
    const auto audit = readPrivateJson(root / "nodes" / id / "host-audit-final.json");
    checkBinding(audit);
    if (audit.at("schema") != "ndnsf-di-host-node-audit-v1" || audit.at("node_id") != id ||
        audit.at("status") != "ZERO" || audit.at("cleanup_scope") != "HOST_UID_JOB" ||
        audit.at("processes") != NativeJson::array() || audit.at("errors") != NativeJson::array() ||
        !matchesHost(node.at("host"), audit.at("host")) ||
        !matchesJob(audit.at("auditor_cgroup"), job, audit.at("auditor_step_id")))
      chainFailure("deployment-node-cleanup");
    for (const auto* kind : {"nfd", "nfdc"}) {
      if (std::string(kind) == "nfd" && node.at("nfd").at("mode") != "managed") continue;
      const auto roleId = managementRoleId(kind, id);
      if (!roles.emplace(roleId, NativeJson{{"id", roleId}, {"kind", kind}, {"node", id}}).second)
        chainFailure("deployment-role-coverage");
    }
  }
  for (const auto& actor : config.at("actors")) {
    if (actor.at("external") != false ||
        !roles.emplace(actor.at("id").get<std::string>(), actor).second)
      chainFailure("deployment-external-actor-not-qualified");
  }
  std::set<std::string> seen;
  for (const auto& role : state.at("roles")) {
    const auto id = role.at("id").get<std::string>();
    const auto expected = roles.find(id);
    if (expected == roles.end() || !seen.insert(id).second ||
        role.at("node") != expected->second.at("node") ||
        role.at("kind") != expected->second.at("kind"))
      chainFailure("deployment-role-coverage");
  }
  if (seen.size() != roles.size()) chainFailure("deployment-role-coverage");
  for (const auto& [id, role] : roles) {
    const auto node = role.at("node").get<std::string>();
    const auto kind = role.at("kind").get<std::string>();
    const auto path = kind == "nfd" || kind == "nfdc"
      ? root / "nodes" / node / kind / "receipt.json"
      : root / "actors" / id / "receipt.json";
    const auto receipt = readPrivateJson(path);
    checkBinding(receipt);
    const bool finite = kind == "requester" || kind == "nfdc";
    if (receipt.at("schema") != "ndnsf-di-node-role-v1" || receipt.at("role_id") != id ||
        receipt.at("role_kind") != kind || receipt.at("node_id") != node ||
        receipt.at("cleanup_scope") != "PID_NAMESPACE" || receipt.at("status") != "STOPPED" ||
        !receipt.at("failure").is_null() ||
        !matchesHost(nodes.at(node).at("host"), receipt.at("host")) ||
        receipt.at("execution") != (finite ? "COMPLETE" : "SERVICE_STOPPED") ||
        (finite && nonnegativeInteger(receipt.at("exit_code")) != 0))
      chainFailure("deployment-role-terminal");
    validateDeploymentGroups(receipt, boot, digest, true);
    if (kind != "nfdc") {
      const auto audit = readPrivateJson(root / "nodes" / node / ("host-audit-" + id + ".json"));
      checkBinding(audit);
      if (audit.at("schema") != "ndnsf-di-host-node-audit-v1" || audit.at("node_id") != node ||
          audit.at("role_id") != id || audit.at("status") != "OBSERVED" ||
          !matchesHost(nodes.at(node).at("host"), audit.at("host")))
        chainFailure("deployment-native-host-binding");
      const auto& proof = audit.at("native_process");
      const auto launch = std::find_if(receipt.at("launches").begin(), receipt.at("launches").end(),
        [&] (const auto& row) { return row.at("id") == id; });
      if (launch == receipt.at("launches").end()) chainFailure("deployment-actor-not-launched");
      const auto& native = launch->at("proof");
      if (!proof.at("nspid").is_array() || proof.at("nspid").empty() ||
          nonnegativeInteger(proof.at("pid")) <= 1 ||
          nonnegativeInteger(proof.at("nspid").back()) != nonnegativeInteger(native.at("pid")) ||
          nonnegativeInteger(proof.at("start_ticks")) != nonnegativeInteger(native.at("start_ticks")) ||
          nonnegativeInteger(proof.at("uid")) != nonnegativeInteger(native.at("uid")) ||
          proof.at("exe") != native.at("exe") ||
          proof.at("bindings") != native.at("bindings") ||
          proof.at("pid_namespace") != receipt.at("pid_namespace") ||
          !matchesJob(proof.at("cgroup"), job, receipt.at("step_id")))
        chainFailure("deployment-native-host-proof");
    }
    receipts.emplace(id, receipt);
  }
  return receipts;
}

std::vector<std::string>
validateGpuAdmission(const std::filesystem::path& root, const NativeJson& state,
                     const NativeJson& config, const NativeJson& actor,
                     const NativeJson& receipt)
{
  const auto node = actor.at("node").get<std::string>();
  const auto pool = readPrivateJson(root / "nodes" / node / "gpu-allocation.json");
  if (pool != state.at("gpu_allocations").at(node))
    chainFailure("deployment-gpu-pool-binding");
  const auto nodeConfig = std::find_if(config.at("nodes").begin(), config.at("nodes").end(),
    [&] (const auto& item) { return item.at("id") == node; });
  if (nodeConfig == config.at("nodes").end()) chainFailure("deployment-gpu-node");
  const auto check = [&] (const NativeJson& value, std::size_t expectedCount) {
    const auto& probe = value.at("native_probe");
    if (value.at("schema") != "ndnsf-di-gpu-allocation-v1" || value.at("node_id") != node ||
        value.at("boot_id") != state.at("boot_id") ||
        value.at("configuration_sha256") != state.at("configuration_sha256") ||
        value.at("release_sha256") != state.at("release_sha256") ||
        value.at("job_id") != state.at("job_id") ||
        !matchesHost(nodeConfig->at("host"), value.at("host")) ||
        probe.at("schema") != "ndnsf-di-gpu-probe-v1" ||
        probe.at("event") != "NDNSF_DI_GPU_PROBE" || probe.at("device_order") != "PCI_BUS_ID" ||
        probe.at("step_id").get<std::string>().empty() ||
        probe.at("step_id").get<std::string>().find_first_not_of("0123456789") != std::string::npos ||
        value.at("devices").size() != expectedCount ||
        probe.at("visible_uuids").size() != expectedCount)
      chainFailure("deployment-gpu-probe-binding");
    for (const auto* key : {"boot_id", "configuration_sha256", "release_sha256", "job_id", "host", "visibility"}) {
      if (probe.at(key) != value.at(key)) chainFailure("deployment-gpu-probe-binding");
    }
    std::vector<std::string> uuids;
    std::set<std::string> physicalIds, identities;
    for (const auto& item : value.at("devices")) {
      const auto uuid = item.at("uuid").get<std::string>();
      const auto physical = item.at("physical_id").get<std::string>();
      if (!identities.insert(uuid).second || !physicalIds.insert(physical).second ||
          uuid.compare(0, 4, "GPU-") != 0 || physical.empty() ||
          physical.find_first_not_of("0123456789") != std::string::npos ||
          probe.at("visible_uuids").at(uuids.size()) != uuid ||
          std::count_if(probe.at("inventory").begin(), probe.at("inventory").end(),
            [&] (const auto& row) { return row.at("physical_id") == physical && row.at("uuid") == uuid; }) != 1)
        chainFailure("deployment-gpu-uuid-inventory");
      uuids.push_back(uuid);
    }
    return uuids;
  };
  const auto count = nonnegativeInteger(config.at("platform").at("slurm").at("gpu").at("count"));
  if (count == 0 || count > 64) chainFailure("deployment-gpu-count");
  check(pool, count);
  std::string fullVisibility;
  for (std::uint64_t index = 0; index < count; ++index)
    fullVisibility += (index ? "," : "") + std::to_string(index);
  if (pool.at("visibility") != fullVisibility)
    chainFailure("deployment-gpu-pool-binding");
  if (pool.at("native_probe").at("role_id") != managementRoleId("gpu-probe", node))
    chainFailure("deployment-gpu-pool-binding");
  std::set<std::string> declared;
  for (const auto& selector : actor.at("resources").at("gpu_devices")) {
    const auto selected = selector.get<std::string>();
    std::vector<std::string> matches;
    if (selected.compare(0, 10, "allocated:") == 0) {
      const auto index = selected.substr(10);
      if (index.empty() || index.size() > 2 ||
          (index.size() > 1 && index.front() == '0') ||
          index.find_first_not_of("0123456789") != std::string::npos)
        chainFailure("deployment-gpu-selector");
      const auto slot = std::stoul(index);
      if (slot >= count)
        chainFailure("deployment-gpu-selector");
      matches.push_back(pool.at("devices").at(slot).at("uuid").get<std::string>());
    }
    else {
      for (const auto& item : pool.at("devices")) {
        if (item.at("physical_id") == selected || item.at("uuid") == selected)
          matches.push_back(item.at("uuid").get<std::string>());
      }
    }
    if (matches.size() != 1 || !declared.insert(matches.front()).second)
      chainFailure("deployment-gpu-selector");
  }
  const auto& admission = receipt.at("gpu_admission");
  const auto selectedUuids = check(admission, declared.size());
  const auto& probe = admission.at("native_probe");
  const auto query = std::find_if(receipt.at("launches").begin(), receipt.at("launches").end(),
    [&] (const auto& row) { return row.at("id") == probe.at("role_id"); });
  const NativeJson command = NativeJson::array({
    (std::filesystem::path(config.at("release").at("prefix").get<std::string>()) /
      "libexec/ndnsf-di/ndnsf-di-device-query").string(), "--count", std::to_string(declared.size())});
  if (std::set<std::string>(selectedUuids.begin(), selectedUuids.end()) != declared ||
      probe.at("step_id") != receipt.at("step_id") ||
      admission.at("visibility") != receipt.at("gpu_visibility") ||
      query == receipt.at("launches").end() || query->at("argv") != command ||
      nonnegativeInteger(query->at("exit_code")) != 0)
    chainFailure("deployment-gpu-role-binding");
  return selectedUuids;
}

void
validateGpuExecution(const NativeJson& row, const StatisticsRows& source, unsigned round,
                     const NativeJson& receipt, const std::vector<std::string>& admitted)
{
  if (!row.contains("finalExecutionEvidence") || row.at("finalExecutionEvidence").is_null())
    chainFailure("deployment-gpu-execution-missing");
  const auto evidence = ndnsf::di::executionEvidenceFromJson(row.at("finalExecutionEvidence").dump());
  const auto device = evidence.deviceId;
  if (device.empty() || device.size() > 2 || (device.size() > 1 && device.front() == '0') ||
      device.find_first_not_of("0123456789") != std::string::npos)
    chainFailure("deployment-gpu-logical-device");
  const auto ordinal = std::stoul(device);
  const auto role = row.at("role").get<std::string>();
  const auto admittedModel = row.at("admittedModelDigest").get<std::string>();
  const auto admittedArtifact = row.at("admittedArtifactDigest").get<std::string>();
  const auto artifact = evidence.artifactDigests.find(role);
  if (admittedModel.empty() || admittedArtifact.empty() || evidence.modelDigest != admittedModel ||
      artifact == evidence.artifactDigests.end() || artifact->second != admittedArtifact)
    chainFailure("deployment-gpu-model-binding");
  const auto native = std::find_if(receipt.at("launches").begin(), receipt.at("launches").end(),
    [&] (const auto& item) { return item.at("id") == source.actor; });
  if (native == receipt.at("launches").end() || ordinal >= admitted.size() ||
      evidence.runnerKind != ndnsf::di::RunnerKind::OnnxRuntimeCuda || !evidence.realCompute ||
      evidence.deviceKind != "cuda" || evidence.cpuFallbackUsed || !evidence.loadCompleted ||
      !evidence.warmupCompleted || !evidence.executionCompleted || evidence.exactForwardCacheHit ||
      evidence.gpuIdentitySource != "cuda-runtime-pci+driver-uuid" ||
      evidence.gpuUuid != admitted.at(ordinal) || evidence.gpuUuids != std::vector<std::string>{evidence.gpuUuid} ||
      evidence.cudaVisibleDevices != receipt.at("gpu_visibility").get<std::string>() ||
      evidence.processId != nonnegativeInteger(native->at("proof").at("pid")) ||
      evidence.providerName != source.provider || evidence.providerBootId != source.boot ||
      evidence.requestId != row.at("requestId").get<std::string>() ||
      evidence.attemptEpoch != nonnegativeInteger(row.at("attemptEpoch")) ||
      evidence.planDigest != row.at("planDigest").get<std::string>() ||
      evidence.roles != std::vector<std::string>{role})
    chainFailure("deployment-gpu-execution-binding");
  bool profileBound = false;
  for (unsigned index = 0; index <= round; ++index) {
    if (source.requests.at(index).at("requestId") == evidence.profileRequestId &&
        nonnegativeInteger(source.requests.at(index).at("attemptEpoch")) == evidence.profileAttemptEpoch)
      profileBound = true;
  }
  unsigned kernels = 0;
  for (const auto& node : evidence.getNodeProviderAssignments()) {
    if (!node.modelNode) continue;
    if (node.role != role || node.nodeName.empty() || node.provider != "CUDAExecutionProvider")
      chainFailure("deployment-gpu-kernel-provider");
    ++kernels;
  }
  if (!profileBound || evidence.providerProfilePath.empty() || kernels == 0)
    chainFailure("deployment-gpu-kernel-proof");
  if (round && !source.requests.front().at("finalExecutionEvidence").is_null()) {
    const auto first = ndnsf::di::executionEvidenceFromJson(
      source.requests.front().at("finalExecutionEvidence").dump());
    if (first.modelDigest != evidence.modelDigest || first.artifactDigests != evidence.artifactDigests)
      chainFailure("deployment-gpu-model-binding");
  }
}

NativeJson
validateDeployment(const std::filesystem::path& statePath, bool requireMultiToken,
                   bool requireDistributedGpu = false)
{
  const auto state = readJson(statePath);
  const bool slurm = state.contains("job_id");
  const std::filesystem::path root = slurm ? statePath.parent_path()
    : std::filesystem::path(state.at("evidence_root").get<std::string>());
  const auto config = readJson(root / "configuration.json");
  const auto boot = state.at("boot_id").get<std::string>();
  const auto digest = state.at("configuration_sha256").get<std::string>();
  if (state.at("schema") != "ndnsf-di-host-operation-v1" || boot.empty() ||
      state.at("deployment_id") != config.at("deployment_id") ||
      state.at("status") != "STOPPED" || (!slurm && state.value("needs_closeout", true)) ||
      config.at("platform").at("kind") != (slurm ? "slurm-apptainer" : "host") ||
      (!slurm && (config.at("nodes").size() != 1 || requireDistributedGpu)) ||
      (state.contains("failure") && !state.at("failure").is_null()) ||
      ndnsf::di::nativePlanningDigest(ndnsf::di::nativeCanonicalJson(config)) !=
        "sha256:" + digest)
    chainFailure("deployment-state-or-configuration");
  auto launched = validateDeploymentGroups(state, boot, digest, slurm);
  std::map<std::string, NativeJson> roleReceipts;
  if (slurm) {
    readPrivateJson(statePath);
    readPrivateJson(root / "configuration.json");
    roleReceipts = validateAllocation(root, state, config, boot, digest);
    for (const auto& [id, receipt] : roleReceipts) launched.insert(id);
  }
  else {
    const auto& terminal = state.at("requester");
    if (terminal.at("completion") != "EXITED" ||
        nonnegativeInteger(terminal.at("exit_code")) != 0)
      chainFailure("deployment-requester-exit");
  }
  if (!state.at("external_references").empty())
    chainFailure("deployment-external-actor-not-qualified");

  std::string requesterId;
  for (const auto& actor : config.at("actors")) {
    if (actor.at("kind") != "requester") continue;
    if (!requesterId.empty()) chainFailure("deployment-requester-count");
    requesterId = actor.at("id").get<std::string>();
  }
  if (requesterId.empty()) chainFailure("deployment-requester-count");
  const auto actorRoot = [&] (const std::string& id) {
    return slurm ? root / "actors" / id / "runtime" /
      config.at("deployment_id").get<std::string>() : root;
  };
  const auto requesterRoot = actorRoot(requesterId);
  const auto requesterConfig = readJson(requesterRoot / (requesterId + ".json"));
  const auto& turns = requesterConfig.at("turns");
  const auto& conversation = requesterConfig.at("conversation");
  const auto& owner = conversation.at("owner");
  const auto& request = requesterConfig.at("request");
  const auto& placement = requesterConfig.at("fixed_placement").at("role_providers");
  NativeJson roleMap = NativeJson::array();
  if (!placement.is_object() || placement.empty()) chainFailure("deployment-role-map");
  for (auto item = placement.begin(); item != placement.end(); ++item)
    roleMap.push_back(NativeJson::array({item.key(), item.value()}));
  const auto roleMapDigest = ndnsf::di::nativePlanningDigest(
    ndnsf::di::nativeCanonicalJson(roleMap));
  auto boundRequester = requesterConfig;
  const auto requesterStatisticsPath = requesterConfig.at("statistics_file").get<std::string>();
  if (!config.at("requester").contains("statistics_file")) {
    if (std::filesystem::path(requesterStatisticsPath) !=
        requesterRoot / (requesterId + "-statistics.jsonl"))
      chainFailure("deployment-statistics-path");
    boundRequester.erase("statistics_file");
  }
  if (!turns.is_array() || turns.size() < 2 || turns.size() > 8 ||
      conversation.at("turn").at("mode") != "FULL_CONTEXT" ||
      request.at("generation_mode") != "TOKEN_STREAMING" ||
      config.at("requester") != boundRequester)
    chainFailure("deployment-turn-configuration");
  const auto rounds = static_cast<unsigned>(turns.size());
  const auto requester = readStatistics(requesterStatisticsPath,
    "ndnsf-di-native-requester-statistics-v1", rounds, false);
  std::vector<StatisticsRows> providers;
  std::map<std::string, std::vector<std::string>> gpuUuids;
  std::set<std::string> gpuNodes;
  const auto requesterHost = slurm ? roleReceipts.at(requesterId).at("host").get<std::string>() : "";
  std::set<std::string> identities;
  unsigned requesters = 0;
  for (const auto& actor : config.at("actors")) {
    const auto id = actor.at("id").get<std::string>();
    if (!launched.count(id)) chainFailure("deployment-actor-not-launched");
    if (actor.at("kind") == "requester") {
      ++requesters;
      if (actor.at("identity") != owner.at("requester_identity"))
        chainFailure("deployment-requester-identity");
    }
    if (actor.at("kind") != "provider") continue;
    const auto identity = actor.at("identity").get<std::string>();
    if (!identities.insert(identity).second) chainFailure("deployment-provider-identity");
    const auto statisticsPath = actor.contains("parameters")
      ? actor.at("parameters").value("statistics-file", (actorRoot(id) / (id + "-statistics.jsonl")).string())
      : (actorRoot(id) / (id + "-statistics.jsonl")).string();
    providers.push_back(readStatistics(statisticsPath,
      "ndnsf-di-native-request-statistics-v1", rounds, true));
    providers.back().actor = id;
    if (slurm) {
      providers.back().host = roleReceipts.at(id).at("host").get<std::string>();
      if (!actor.at("resources").at("gpu_devices").empty()) {
        gpuUuids.emplace(id, validateGpuAdmission(root, state, config, actor, roleReceipts.at(id)));
        gpuNodes.insert(actor.at("node").get<std::string>());
      }
    }
    if (providers.back().provider != identity)
      chainFailure("deployment-provider-identity");
  }
  if (providers.size() < 2 || requesters != 1)
    chainFailure("deployment-role-coverage");
  if (requireDistributedGpu && gpuNodes.size() < 2)
    chainFailure("deployment-distributed-gpu-required");

  NativeJson results = NativeJson::array();
  NativeJson initial;
  std::vector<std::int64_t> prefix;
  std::set<std::string> requests;
  std::uint64_t previousFinish = 0;
  for (unsigned round = 0; round < rounds; ++round) {
    const auto& row = requester.requests.at(round);
    const auto requestId = row.at("requestId").get<std::string>();
    const auto plan = row.at("planDigest").get<std::string>();
    if (nonnegativeInteger(row.at("turn")) != round || row.at("completed") != true || plan.empty() ||
        requestId.empty() || !requests.insert(requestId).second)
      chainFailure("deployment-request-incomplete");
    const auto interval = *statisticsInterval(row, "startedSteadyNs", "finishedSteadyNs");
    if (interval.first < previousFinish) chainFailure("deployment-turn-order");
    previousFinish = interval.second;
    const auto& turn = turns.at(round);
    const auto output = readJson(turn.at("output_file").get<std::string>());
    const auto generation = validateGenerationOutput(request,
      readJson(turn.at("options_file").get<std::string>()), output,
      requireMultiToken, &row);
    const auto tokenTotal = generation.at("generatedTokens").get<std::uint64_t>();
    const std::filesystem::path inputPath(turn.at("input_file").get<std::string>());
    if (!std::filesystem::is_regular_file(inputPath) ||
        std::filesystem::file_size(inputPath) > 16U * 1024U * 1024U)
      chainFailure("deployment-input-bound");
    const auto bytes = readFile(inputPath);
    const auto tensors = ndnsf::di::decodeTensorBundle(
      std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    unsigned inputTensors = 0;
    for (const auto& tensor : tensors) {
      if (tensor.name != "input_ids") continue;
      ++inputTensors;
      if (tensor.elementType != ndnsf::di::TensorElementType::Int64 ||
          tensor.shape.size() != 2 || tensor.shape.front() != 1 ||
          tensor.shape.back() <= 0 || tensor.payload.size() / 8 !=
            static_cast<std::size_t>(tensor.shape.back()) || tensor.payload.size() % 8)
        chainFailure("deployment-input-token-layout");
      for (std::size_t offset = 0; offset < tensor.payload.size(); offset += 8) {
        std::uint64_t token = 0;
        for (unsigned byte = 0; byte < 8; ++byte)
          token |= std::uint64_t(tensor.payload[offset + byte]) << (byte * 8);
        if (token > INT64_MAX) chainFailure("deployment-input-token");
        prefix.push_back(static_cast<std::int64_t>(token));
      }
    }
    if (inputTensors != 1) chainFailure("deployment-input-token-count");
    for (const auto& token : output.at("tokenIds")) prefix.push_back(token.get<std::int64_t>());
    const auto checkpoint = readJson(turn.at("checkpoint_output_file").get<std::string>());
    if (checkpoint.at("schema") != "ndnsf-di-conversation-checkpoint-v1" ||
        nonnegativeInteger(checkpoint.at("version")) != 1 ||
        nonnegativeInteger(checkpoint.at("contextEpoch")) != round + 1 ||
        nonnegativeInteger(checkpoint.at("parentContextEpoch")) != round ||
        nonnegativeInteger(checkpoint.at("prefixTokenCount")) != prefix.size() ||
        checkpoint.at("logicalPrefixDigest") != ndnsf::di::nativeConversationPrefixDigest(prefix) ||
        checkpoint.at("conversationId") != conversation.at("turn").at("conversation_id") ||
        checkpoint.at("serviceName") != owner.at("service_name") ||
        checkpoint.at("serviceName") != request.at("service") ||
        checkpoint.at("requesterIdentity") != owner.at("requester_identity") ||
        checkpoint.at("securityDomainDigest") != owner.at("security_domain_digest") ||
        checkpoint.at("securityDomainDigest") != request.at("security_policy_digest") ||
        checkpoint.at("planRoleMapDigest") != roleMapDigest ||
        nonnegativeInteger(checkpoint.at("issuedAtMs")) >=
          nonnegativeInteger(checkpoint.at("expiresAtMs")))
      chainFailure("deployment-checkpoint-binding");
    auto unsignedCheckpoint = checkpoint;
    unsignedCheckpoint.erase("checkpointDigest");
    unsignedCheckpoint.erase("signature");
    if (checkpoint.at("checkpointDigest") != ndnsf::di::nativePlanningDigest(
          ndnsf::di::nativeConversationCanonicalJson(unsignedCheckpoint)))
      chainFailure("deployment-checkpoint-digest");
    if (round == 0) initial = checkpoint;
    for (const auto* key : {"modelContractDigest", "securityDomainDigest", "planRoleMapDigest"}) {
      if (!checkpoint.at(key).is_string() || checkpoint.at(key).get<std::string>().empty() ||
          checkpoint.at(key) != initial.at(key))
        chainFailure("deployment-checkpoint-contract");
    }
    const auto& receipts = checkpoint.at("roleReceiptDigests");
    std::set<std::string> roles;
    unsigned publishers = 0;
    std::optional<std::uint64_t> attempt;
    for (const auto& provider : providers) {
      const auto& observation = provider.requests.at(round);
      validateProviderStatistics(provider, observation, requestId, plan, provider.provider);
      const auto devices = gpuUuids.find(provider.actor);
      if (devices != gpuUuids.end())
        validateGpuExecution(observation, provider, round, roleReceipts.at(provider.actor), devices->second);
      const auto role = observation.at("role").get<std::string>();
      if (!roles.insert(role).second || !receipts.contains(role) ||
          !placement.contains(role) || placement.at(role) != provider.provider ||
          !receipts.at(role).is_string() || receipts.at(role).get<std::string>().empty() ||
          nonnegativeInteger(observation.at("epochsExecuted")) != tokenTotal + 1)
        chainFailure("deployment-checkpoint-role-coverage");
      const auto currentAttempt = nonnegativeInteger(observation.at("attemptEpoch"));
      if (attempt && *attempt != currentAttempt) chainFailure("deployment-attempt-binding");
      attempt = currentAttempt;
      const auto providerInterval = *statisticsInterval(
        observation, "startedSteadyNs", "finishedSteadyNs");
      if ((!slurm || matchesHost(requesterHost, provider.host)) &&
          (providerInterval.first < interval.first || providerInterval.second > interval.second))
        chainFailure("deployment-provider-request-window");
      const auto events = nonnegativeInteger(observation.at("eventsPublished"));
      if (events) {
        ++publishers;
        if (events != tokenTotal) chainFailure("deployment-stream-event-count");
      }
    }
    if (publishers != 1 || !receipts.is_object() || receipts.size() != roles.size())
      chainFailure("deployment-stream-role-coverage");
    results.push_back({{"requestId", requestId}, {"planDigest", plan},
      {"generatedTokens", tokenTotal}, {"contextEpoch", round + 1},
      {"prefixTokens", prefix.size()}, {"finishReason", generation.at("finishReason")}});
  }
  return NativeJson{{"scope", "deployment-quiet-execution"}, {"bootId", boot},
    {"platform", slurm ? "slurm-apptainer" : "single-host"},
    {"gpuNodes", gpuNodes}, {"distributedGpu", requireDistributedGpu ? "VERIFIED" : "NOT_REQUIRED"},
    {"configurationSha256", digest}, {"rounds", results}, {"cleanup", "ZERO_NONFORCED"},
    {"materialFetch", "NOT_EVALUATED"}, {"grantAudit", "NOT_EVALUATED"},
    {"placementAudit", "NOT_EVALUATED"}, {"restoreAudit", "NOT_EVALUATED"},
    {"checkpointAuthentication", "NOT_EVALUATED"}, {"fullPathQualification", "NOT_RUN"}};
}

NativeJson validateRounds(const std::filesystem::path& root, unsigned rounds,
                         bool cacheCompatibility, bool requireMultiToken,
                         bool quietPerformance = false)
{
  std::set<std::string> requestIds;
  NativeJson receipt{{"schema", "spec189-cpp-oracle-v1"}, {"providers", 2},
                     {"terminal", true}, {"rounds", rounds}, {"roundResults", NativeJson::array()}};
  NativeJson previous, initial;
  std::map<std::string, NativeJson> roleBindings;
  RoundEvidence view;
  StatisticsRows requesterStatistics;
  std::array<StatisticsRows, 2> providerStatistics;
  if (quietPerformance) {
    requesterStatistics = readStatistics(root / "requester-statistics.jsonl",
      "ndnsf-di-native-requester-statistics-v1", rounds, false);
    for (unsigned provider = 0; provider < 2; ++provider)
      providerStatistics[provider] = readStatistics(
        root / ("provider-" + std::to_string(provider) + "-statistics.jsonl"),
        "ndnsf-di-native-request-statistics-v1", rounds, true);
  }
  for (unsigned round = 0; round < rounds; ++round) {
    std::string requestId, planDigest;
    if (quietPerformance) {
      const auto& requester = requesterStatistics.requests.at(round);
      requestId = requester.at("requestId").get<std::string>();
      planDigest = requester.at("planDigest").get<std::string>();
      if (requestId.empty() || planDigest.empty() || requester.at("completed") != true)
        chainFailure("statistics-incomplete-request");
      if (!requestIds.insert(requestId).second) chainFailure("duplicate-request-id");
      const auto requestWall = *statisticsInterval(requester,
                                                   "startedSteadyNs", "finishedSteadyNs");
      statisticsInterval(requester, "startedProcessCpuNs", "finishedProcessCpuNs", true);
      for (unsigned provider = 0; provider < 2; ++provider) {
        validateProviderStatistics(providerStatistics[provider],
          providerStatistics[provider].requests.at(round), requestId, planDigest,
          "/example/ndnsf-qwen06b/provider-" + std::to_string(provider));
        const auto& metrics = providerStatistics[provider].requests.at(round).at("runtimeMetrics");
        for (const auto& sample : metrics) {
          const auto call = *statisticsInterval(sample.at("lastBackendExecution"),
                                                "startedSteadyNs", "finishedSteadyNs");
          if (call.first < requestWall.first || call.second > requestWall.second)
            chainFailure("statistics-request-window");
        }
      }
      if (providerStatistics[0].requests.at(round).at("role") ==
            providerStatistics[1].requests.at(round).at("role") ||
          providerStatistics[0].requests.at(round).at("attemptEpoch") !=
            providerStatistics[1].requests.at(round).at("attemptEpoch"))
        chainFailure("statistics-binding");
    }
    else {
      const auto logPath = root / ("requester-" + std::to_string(round) + ".log");
      if (!std::filesystem::is_regular_file(logPath)) chainFailure("missing-round-file");
      const auto requester = readFile(logPath);
      const auto successes = recordsContaining(requester, "NATIVE_REQUEST_SUCCEEDED");
      if (successes.size() != 1) chainFailure("requester-not-success");
      auto success = markerFields(successes.front(), "NATIVE_REQUEST_SUCCEEDED");
      requestId = success["request"];
      planDigest = success["plan"];
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
    }
    const auto generation = validateMultiTokenOutput(root, round, requireMultiToken,
      quietPerformance ? &requesterStatistics.requests.at(round) : nullptr);
    if (quietPerformance) {
      unsigned publishingProviders = 0;
      for (const auto& provider : providerStatistics) {
        const auto& observation = provider.requests.at(round);
        // Conversation completion includes one state-only checkpoint pass.
        if (nonnegativeInteger(observation.at("epochsExecuted")) !=
              generation.at("generatedTokens").get<std::uint64_t>() + 1)
          chainFailure("statistics-generation-coverage");
        const auto events = nonnegativeInteger(observation.at("eventsPublished"));
        if (events != 0) {
          ++publishingProviders;
          if (events != generation.at("generatedTokens"))
            chainFailure("statistics-event-count");
        }
      }
      if (publishingProviders != 1) chainFailure("statistics-event-count");
    }
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
      if (quietPerformance) {
        const auto& observation = providerStatistics[provider].requests.at(round);
        const auto role = observation.at("role").get<std::string>();
        if (!roleReceipts.contains(role) || !roleReceipts.at(role).is_string() ||
            roleReceipts.at(role).get<std::string>().empty())
          chainFailure("checkpoint-roles-mismatch");
        const auto owner = observation.at("provider").get<std::string>();
        if (round && roleBindings.at(owner) != role) chainFailure("placement-binding-changed");
        roleBindings[owner] = role;
        continue;
      }
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
              << " [--placement-only | --cache-compatibility | --expect-revocation-failure |"
                 " --quiet-performance] [--require-multi-token] [--rounds 1..8]"
                 " --run-root DIRECTORY\n"
              << "       " << argv[0] << " --deployment-state FILE [--require-multi-token]"
                 " [--require-distributed-gpu]\n"
              << "       " << argv[0] << " --repo-lifecycle --repo-root DIRECTORY\n";
    return 0;
  }
  bool placementOnly = false, cacheCompatibility = false, expectedRevocationFailure = false,
       multiToken = false, repoLifecycle = false, invalidOptions = false,
       quietPerformance = false, distributedGpu = false;
  unsigned rounds = 1;
  bool roundsSeen = false;
  const char* runRootArg = nullptr;
  const char* repoRootArg = nullptr;
  const char* deploymentStateArg = nullptr;
  for (int i = 1; i < argc; ++i) {
    const std::string option(argv[i]);
    if (option == "--placement-only" && !placementOnly) placementOnly = true;
    else if (option == "--cache-compatibility" && !cacheCompatibility) cacheCompatibility = true;
    else if (option == "--quiet-performance" && !quietPerformance) quietPerformance = true;
    else if (option == "--expect-revocation-failure" && !expectedRevocationFailure)
      expectedRevocationFailure = true;
    else if (option == "--require-multi-token" && !multiToken) multiToken = true;
    else if (option == "--require-distributed-gpu" && !distributedGpu) distributedGpu = true;
    else if (option == "--repo-lifecycle" && !repoLifecycle) repoLifecycle = true;
    else if (option == "--run-root" && !runRootArg && i + 1 < argc) runRootArg = argv[++i];
    else if (option == "--repo-root" && !repoRootArg && i + 1 < argc) repoRootArg = argv[++i];
    else if (option == "--deployment-state" && !deploymentStateArg && i + 1 < argc)
      deploymentStateArg = argv[++i];
    else if (option == "--rounds" && !roundsSeen && i + 1 < argc) {
      roundsSeen = true;
      const std::string count(argv[++i]);
      if (count.size() != 1 || count[0] < '1' || count[0] > '8') invalidOptions = true;
      else rounds = static_cast<unsigned>(count[0] - '0');
    }
    else invalidOptions = true;
  }
  if (invalidOptions || (distributedGpu && !deploymentStateArg) ||
      (deploymentStateArg && (repoLifecycle || repoRootArg || runRootArg || placementOnly ||
        cacheCompatibility || expectedRevocationFailure || quietPerformance || roundsSeen)) ||
      (repoLifecycle && (repoRootArg == nullptr || runRootArg != nullptr ||
                         placementOnly || cacheCompatibility || expectedRevocationFailure ||
                         multiToken || roundsSeen || quietPerformance)) ||
      (!repoLifecycle && !deploymentStateArg &&
        (runRootArg == nullptr || repoRootArg != nullptr)) ||
      (placementOnly && (cacheCompatibility || expectedRevocationFailure || multiToken || rounds > 1)) ||
      (expectedRevocationFailure && (placementOnly || cacheCompatibility || multiToken || roundsSeen)) ||
      (quietPerformance && (placementOnly || cacheCompatibility || expectedRevocationFailure))) {
    std::cerr << "usage: " << argv[0]
              << " [--placement-only | --cache-compatibility | --expect-revocation-failure |"
                 " --quiet-performance] [--require-multi-token] [--rounds 1..8]"
                 " --run-root DIRECTORY\n"
              << "       " << argv[0] << " --deployment-state FILE [--require-multi-token]"
                 " [--require-distributed-gpu]\n"
              << "       " << argv[0] << " --repo-lifecycle --repo-root DIRECTORY\n";
    return 2;
  }
  try {
    if (deploymentStateArg) {
      const auto receipt = validateDeployment(
        std::filesystem::absolute(deploymentStateArg), multiToken, distributedGpu);
      std::cout << "SPEC217_CPP_DEPLOYMENT_EXECUTION_PASS "
                << ndnsf::di::nativeCanonicalJson(receipt) << '\n';
      return 0;
    }
    if (repoLifecycle) {
      validateRepoLifecycle(std::filesystem::absolute(repoRootArg));
      return 0;
    }
    const auto root = std::filesystem::absolute(runRootArg);
    if (expectedRevocationFailure) {
      validateRevocationFailure(root);
      return 0;
    }
    if (rounds > 1 || quietPerformance) {
      auto receipt = validateRounds(root, rounds, cacheCompatibility, multiToken, quietPerformance);
      if (quietPerformance) {
        receipt["scope"] = "quiet-performance";
        receipt["fullPathQualification"] = "NOT_RUN";
        for (const auto* check : {"materialFetch", "grantAudit", "placementAudit", "restoreAudit"})
          receipt[check] = "NOT_EVALUATED";
        std::cout << "SPEC189_CPP_QUIET_PERFORMANCE_PASS "
                  << ndnsf::di::nativeCanonicalJson(receipt) << '\n';
        return 0;
      }
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
