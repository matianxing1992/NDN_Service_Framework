#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp"

#include <boost/test/unit_test.hpp>

#include <chrono>

namespace {

using namespace ndnsf::di;

std::string digest(const std::string& value)
{
  return nativePlanningDigest(value);
}

NativeModelDescriptor model(const std::string& adapter,
                            const std::string& name,
                            const std::string& graphDigest)
{
  return {name, digest(name + "-content"), digest(name + "-semantics"),
          graphDigest, "onnx", "float32", adapter, "1"};
}

NativeGraphSnapshot graph(const std::string& graphDigest,
                          std::vector<std::string> nodeIds)
{
  NativeGraphSnapshot result;
  result.graphDigest = graphDigest;
  for (std::size_t i = 0; i < nodeIds.size(); ++i) {
    result.nodes.push_back({nodeIds[i], "op", static_cast<std::uint64_t>(i)});
  }
  result.topologicalOrder = std::move(nodeIds);
  result.legalCutEdges = {"cut-0", "cut-1"};
  return result;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182NativePlanning)

BOOST_AUTO_TEST_CASE(QwenLayerSplitProducesCanonicalRankOneCandidate)
{
  const auto graphDigest = digest("qwen-graph");
  auto graphSnapshot = graph(graphDigest,
    {"embedding", "layer-00", "layer-01", "layer-02", "layer-03",
     "final-norm-head"});
  auto modelDescriptor = model("qwen-three-stage-pipeline", "QwenFixture", graphDigest);
  const std::vector<std::string> roles = {
    "/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1", "/LLM/Pipeline/Stage/2"};
  qwen::NativeQwenLayerSplit splitter(
    {{0, 2}, {2, 3}, {3, 4}},
    {{roles[0], digest("qwen-artifact-0")},
     {roles[1], digest("qwen-artifact-1")},
     {roles[2], digest("qwen-artifact-2")}},
    {{roles[0], 1}, {roles[1], 1}, {roles[2], 1}}, roles);
  const auto candidates = splitter.enumerate(modelDescriptor, graphSnapshot,
                                             NativeCandidateBudget{2, 100, 1});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  const auto& candidate = candidates.front();
  BOOST_REQUIRE_EQUAL(candidate.executionPlan.roles.size(), roles.size());
  for (std::size_t i = 0; i < roles.size(); ++i) {
    BOOST_CHECK_EQUAL(candidate.executionPlan.roles[i], roles[i]);
  }
  BOOST_CHECK_EQUAL(candidate.crossPartitionTensors.size(), 2U);
  BOOST_CHECK_EQUAL(candidate.tensorDegreesByRole.at(roles[1]), 1U);
  BOOST_CHECK_EQUAL(candidate.inputIngressRole, roles.front());
  BOOST_CHECK_EQUAL(candidate.resultEgressRole, roles.back());
  BOOST_CHECK_EQUAL(candidate.candidateDigest,
                    splitter.enumerate(modelDescriptor, graphSnapshot, {}).front().candidateDigest);
}

BOOST_AUTO_TEST_CASE(YoloComponentSplitRejectsUncoveredGraphAndSortsPriority)
{
  const auto graphDigest = digest("yolo-graph");
  auto graphSnapshot = graph(graphDigest, {"backbone", "neck", "detect", "output"});
  auto modelDescriptor = model("yolo26n", "YOLO26n", graphDigest);
  yolo::NativeYoloComponentSpec atomic{
    "atomic-v1", 10, {"FullModel"}, {}, "FullModel", "FullModel", "", {}};
  yolo::NativeYoloComponentSpec split{
    "split-v1", 1, {"Backbone", "Head"},
    {{"Backbone", {"backbone", "neck"}}, {"Head", {"detect", "output"}}},
    "Backbone", "Head", "NATIVE_POSTPROCESS", {}};
  yolo::NativeYoloComponentSplit splitter({atomic, split});
  const auto candidates = splitter.enumerate(modelDescriptor, graphSnapshot,
                                             NativeCandidateBudget{2, 100, 1});
  BOOST_REQUIRE_EQUAL(candidates.size(), 2U);
  BOOST_CHECK_EQUAL(candidates.front().selectionPriority, 1);
  BOOST_CHECK_EQUAL(candidates.front().executionPlan.roles.front(), "Backbone");
  BOOST_CHECK_EQUAL(candidates.back().executionPlan.roles.front(), "FullModel");

  yolo::NativeYoloComponentSpec invalid{
    "invalid", 0, {"Backbone", "Head"},
    {{"Backbone", {"backbone"}}, {"Head", {"output"}}},
    "Backbone", "Head", "", {}};
  yolo::NativeYoloComponentSplit bad({invalid});
  BOOST_CHECK_THROW(bad.enumerate(modelDescriptor, graphSnapshot, {}),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PreSplitPlacementFiltersAndDeterministicallyBindsOneProvider)
{
  const auto graphDigest = digest("placement-graph");
  auto graphSnapshot = graph(graphDigest, {"embedding", "layer-00", "final-norm-head"});
  auto modelDescriptor = model("qwen", "QwenFixture", graphDigest);
  const std::string role = "/LLM/Pipeline/Stage/0";
  qwen::NativeQwenLayerSplit splitter(
    {{0, 1}}, {{role, digest("artifact")}}, {{role, 1}}, {role}, {1});
  const auto candidate = splitter.enumerate(modelDescriptor, graphSnapshot,
                                             NativeCandidateBudget{1, 100, 1}).front();
  NativePlanningSnapshot snapshot;
  snapshot.model = modelDescriptor;
  snapshot.graph = graphSnapshot;
  snapshot.requestId = "request";
  snapshot.attempt = 1;
  snapshot.ackClosedDigest = digest("ack-closed");
  snapshot.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  snapshot.offers = {
    {"provider-z", digest("offer-z"), {role}, {"onnxruntime"}, {}, 4ULL * 1024 * 1024 * 1024, 1, true, true},
    {"provider-a", digest("offer-a"), {role}, {"onnxruntime"}, {digest("resident")},
     4ULL * 1024 * 1024 * 1024, 2, true, true},
  };
  NativePreSplitFirstPlacement placement;
  const auto proposal = placement.propose(snapshot, candidate);
  BOOST_CHECK_EQUAL(proposal.assignment.providerByRole.at(role), "provider-a");
  BOOST_CHECK_EQUAL(proposal.strategy.name, "native-pre-split-first");
  BOOST_CHECK_NO_THROW(proposal.validate(snapshot, candidate));

  const auto core = NativePlanSealer::sealCore(snapshot, proposal);
  const NativeSecurityPolicySnapshot security{digest("security-policy"), true};
  const auto view = NativePlanSealer::grantView(core, snapshot.offers[1], security);
  const NativeGrantBinding grant{view.provider, view.role, "/grant/1",
                                 digest("grant-1"), view.provider};
  // The single-role fixture needs one grant; finalizeSecurity verifies exact
  // role/provider coverage before exposing a projection.
  const auto sealed = NativePlanSealer::finalizeSecurity(core, {grant}, security);
  const auto projection = NativePlanSealer::project(sealed, view.provider);
  BOOST_REQUIRE(!NativePlanSealer::encode(projection).empty());
}

BOOST_AUTO_TEST_SUITE_END()
