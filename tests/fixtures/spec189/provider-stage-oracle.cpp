#include "examples/Spec189ProviderStageOracle.hpp"

#include <iostream>

int main()
{
  using namespace spec189::oracle;
  try {
    PlacementObservation placement;
    placement.selection.requestId = "request";
    placement.selection.provider = "provider";
    placement.selection.role = "role";
    placement.selection.planDigest = "plan";
    placement.selection.attemptEpoch = "1";
    placement.selection.layerBegin = "0";
    placement.selectionLine = 1;
    placement.grantLine = 2;
    auto sequence = [&](const std::vector<std::string>& stages) {
      std::vector<Marker> result;
      unsigned preparation = 0;
      for (const auto& stage : stages) {
        auto marker = placement.selection;
        marker.stage = stage;
        marker.status = "observed";
        if (stage == "ASSEMBLY_STARTED") ++preparation;
        if (stage == "ASSEMBLY_STARTED" || stage == "RUNNER_READY")
          marker.preparationId = std::to_string(preparation);
        if (stage == "begin" || stage == "complete") {
          marker.stage = "DEPENDENCY_FETCH";
          marker.status = stage;
        }
        marker.line = result.size() + 3;
        result.push_back(std::move(marker));
      }
      return result;
    };
    unsigned cases = 0;
    auto check = [&](const std::vector<Marker>& observed, const std::string& reason,
                     bool terminal = false) {
      try {
        const auto result = validateProviderStages(observed, placement, "provider");
        if (!reason.empty()) throw std::runtime_error("accepted invalid fixture: " + reason);
        if (result != terminal) throw std::runtime_error("incorrect terminal classification");
      }
      catch (const std::runtime_error& error) {
        if (reason.empty() || std::string(error.what()).find("reason=" + reason) == std::string::npos)
          throw;
      }
      ++cases;
    };
    const auto first = sequence({"GRANT_VERIFIED", "EXECUTION_ENTERED", "ASSEMBLY_STARTED",
                                 "RUNNER_READY", "EXECUTION_COMPLETED"});
    check(first, ""); // First partition needs no upstream fetch or terminal.
    placement.selection.layerBegin = "14";
    check(first, "missing-upstream");
    const auto interleaved = sequence({"GRANT_VERIFIED", "EXECUTION_ENTERED", "begin",
      "ASSEMBLY_STARTED", "RUNNER_READY", "complete", "EXECUTION_COMPLETED", "TERMINAL"});
    check(interleaved, "", true);
    check(sequence({"GRANT_VERIFIED", "EXECUTION_ENTERED", "ASSEMBLY_STARTED", "begin",
      "complete", "RUNNER_READY", "EXECUTION_COMPLETED", "TERMINAL"}), "", true);
    check(sequence({"GRANT_VERIFIED", "EXECUTION_ENTERED", "begin", "complete",
      "ASSEMBLY_STARTED", "RUNNER_READY", "EXECUTION_COMPLETED", "TERMINAL"}), "", true);
    auto log = interleaved; log[0].line = 2;
    check(log, "before-authorization");
    log = interleaved; log[3].requestId = "other";
    check(log, "identity-mismatch");
    log = interleaved; log.erase(log.begin() + 4);
    check(log, "incomplete-preparation");
    log = interleaved; log.push_back(log[4]);
    check(log, "invalid-preparation-sequence");
    log = interleaved; log[4].status = "error";
    check(log, "unsuccessful-stage");
    log = interleaved; log[2].status = "complete";
    check(log, "invalid-dependency-sequence");
    log = interleaved; log.erase(log.begin() + 5);
    check(log, "missing-upstream");
    check(sequence({"GRANT_VERIFIED", "EXECUTION_ENTERED", "begin", "ASSEMBLY_STARTED",
      "RUNNER_READY", "EXECUTION_COMPLETED", "complete", "TERMINAL"}),
      "incomplete-or-late-dependency");
    check(sequence({"GRANT_VERIFIED", "EXECUTION_ENTERED", "begin", "ASSEMBLY_STARTED",
      "RUNNER_READY", "complete", "TERMINAL", "EXECUTION_COMPLETED"}),
      "terminal-before-completion");
    check(sequence({"GRANT_VERIFIED", "EXECUTION_ENTERED", "begin", "RUNNER_READY",
      "ASSEMBLY_STARTED", "complete", "EXECUTION_COMPLETED"}), "invalid-preparation-id");
    check(sequence({"GRANT_VERIFIED", "EXECUTION_ENTERED", "begin", "ASSEMBLY_STARTED",
      "RUNNER_READY", "complete", "begin", "ASSEMBLY_STARTED", "complete",
      "RUNNER_READY", "EXECUTION_COMPLETED", "TERMINAL"}), "", true);
    log = interleaved; log[4].preparationId = "2";
    check(log, "invalid-preparation-sequence");
    log = interleaved; log[4].preparationId.clear();
    check(log, "invalid-preparation-id");
    const auto parsed = markers("NDNSF_DI_PROVIDER_STAGE stage=RUNNER_READY preparationId=9\n");
    if (parsed.size() != 1 || parsed.front().preparationId != "9")
      throw std::runtime_error("preparation identity parser");
    std::cout << "Spec189 Provider stage oracle: " << cases << " cases passed\n";
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
