#pragma once

#include "Spec189MaterialFetchOracle.hpp"

namespace spec189::oracle {

// Validate causality within one Provider log. EXECUTION_ENTERED is handler
// admission, not the runner's compute-start event. This checker alone cannot
// prove input readiness at compute start or correctness of the output.
inline bool
validateProviderStages(const std::vector<Marker>& observed,
                       const PlacementObservation& placement,
                       const std::string& expectedProvider)
{
  const auto& selection = placement.selection;
  const auto fail = [](const std::string& stage, const std::string& reason) {
    throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=" + stage +
                             " reason=" + reason);
  };
  std::map<std::string, std::size_t> once;
  std::map<std::string, std::pair<std::size_t, std::size_t>> preparations;
  std::size_t dependencyBegin = 0;
  std::size_t dependencyComplete = 0;
  std::size_t pendingDependencies = 0;
  for (const auto& marker : observed) {
    const auto& stage = marker.stage;
    if (stage != "GRANT_VERIFIED" && stage != "EXECUTION_ENTERED" &&
        stage != "DEPENDENCY_FETCH" && stage != "ASSEMBLY_STARTED" &&
        stage != "RUNNER_READY" && stage != "EXECUTION_COMPLETED" &&
        stage != "TERMINAL")
      continue;
    if (marker.provider != expectedProvider || marker.role != selection.role ||
        marker.requestId != selection.requestId ||
        marker.attemptEpoch != selection.attemptEpoch ||
        marker.planDigest != selection.planDigest)
      fail(stage, "identity-mismatch");
    if (marker.line <= placement.selectionLine || marker.line <= placement.grantLine)
      fail(stage, "before-authorization");
    if (stage == "DEPENDENCY_FETCH") {
      if (marker.status == "begin") {
        ++pendingDependencies;
        if (dependencyBegin == 0) dependencyBegin = marker.line;
      }
      else if (marker.status == "complete" && pendingDependencies != 0) {
        --pendingDependencies;
        dependencyComplete = std::max(dependencyComplete, marker.line);
      }
      else {
        fail(stage, "invalid-dependency-sequence");
      }
    }
    else if (stage == "ASSEMBLY_STARTED" || stage == "RUNNER_READY") {
      if (marker.status != "observed") fail(stage, "unsuccessful-stage");
      if (marker.preparationId.empty() ||
          marker.preparationId.find_first_not_of("0123456789") != std::string::npos ||
          std::stoull(marker.preparationId) == 0)
        fail(stage, "invalid-preparation-id");
      auto& pair = preparations[marker.preparationId];
      if (stage == "ASSEMBLY_STARTED") {
        if (pair.first != 0) fail(stage, "duplicate-preparation");
        pair.first = marker.line;
      }
      else {
        if (pair.first == 0 || pair.second != 0 || pair.first >= marker.line)
          fail(stage, "invalid-preparation-sequence");
        pair.second = marker.line;
      }
    }
    else {
      if (marker.status != "observed") fail(stage, "unsuccessful-stage");
      if (!once.emplace(stage, marker.line).second) fail(stage, "duplicate-stage");
    }
  }
  const auto required = [&](const std::string& stage) {
    const auto it = once.find(stage);
    if (it == once.end()) fail(stage, "missing-stage");
    return it->second;
  };
  const auto grant = required("GRANT_VERIFIED");
  const auto entered = required("EXECUTION_ENTERED");
  const auto completed = required("EXECUTION_COMPLETED");
  if (!(grant < entered && entered < completed))
    fail("EXECUTION_COMPLETED", "invalid-runner-causality");
  if (preparations.empty()) fail("RUNNER_READY", "missing-preparation");
  for (const auto& [id, pair] : preparations) {
    if (pair.second == 0) fail("RUNNER_READY", "incomplete-preparation");
    if (!(entered < pair.first && pair.first < pair.second && pair.second < completed))
      fail("RUNNER_READY", "invalid-runner-causality");
  }
  // The first selected range has no upstream Provider. Later ranges must
  // finish an actual dependency fetch, independently of assembly ordering.
  if (selection.layerBegin.empty() ||
      selection.layerBegin.find_first_not_of("0123456789") != std::string::npos)
    fail("SELECTION", "invalid-layer-begin");
  const bool hasUpstream = std::stoull(selection.layerBegin) != 0;
  if (hasUpstream && dependencyComplete == 0)
    fail("DEPENDENCY_FETCH", "missing-upstream");
  if (pendingDependencies != 0 ||
      (dependencyBegin != 0 && (dependencyBegin <= entered || dependencyBegin >= completed)) ||
      (dependencyComplete != 0 && dependencyComplete >= completed))
    fail("DEPENDENCY_FETCH", "incomplete-or-late-dependency");
  const auto terminal = once.find("TERMINAL");
  if (terminal != once.end() && terminal->second <= completed)
    fail("TERMINAL", "terminal-before-completion");
  return terminal != once.end();
}

} // namespace spec189::oracle
