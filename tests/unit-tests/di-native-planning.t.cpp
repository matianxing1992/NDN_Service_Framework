#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "tests/fixtures/spec182/native-sealing-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp"

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <algorithm>
#include <limits>

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

struct TwoRolePlacement
{
  const std::string first = "/LLM/Pipeline/Stage/0";
  const std::string second = "/LLM/Pipeline/Stage/1";
  NativePlanningSnapshot snapshot;
  NativeSplitCandidate candidate;

  TwoRolePlacement()
  {
    const auto graphDigest = digest("two-role-graph");
    snapshot.graph = graph(graphDigest, {"embedding", "layer-00", "layer-01", "final-norm-head"});
    snapshot.model = model("qwen", "QwenFixture", graphDigest);
    snapshot.requestId = "two-role-request";
    snapshot.attempt = 1;
    snapshot.ackClosedDigest = digest("two-role-ack");
    snapshot.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    qwen::NativeQwenLayerSplit splitter({{0, 1}, {1, 2}},
      {{first, digest("first-artifact")}, {second, digest("second-artifact")}},
      {{first, 1}, {second, 1}}, {first, second}, {1, 1});
    candidate = splitter.enumerate(snapshot.model, snapshot.graph, {}).front();
    snapshot.offers = {
      {"provider-a", digest("offer-a"), {first}, {"onnxruntime"}, {},
        4ULL << 30, 1, true, true},
      {"provider-b", digest("offer-b"), {second}, {"onnxruntime"}, {},
        4ULL << 30, 1, true, true},
    };
  }
};

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

BOOST_AUTO_TEST_CASE(QwenLayerSplitRejectsInvalidRankAndGraph)
{
  const auto graphDigest = digest("qwen-invalid-graph");
  auto validGraph = graph(graphDigest,
    {"embedding", "layer-00", "layer-01", "layer-02", "layer-03",
     "final-norm-head"});
  auto qwenModel = model("qwen-three-stage-pipeline", "QwenFixture", graphDigest);
  const std::vector<std::string> roles = {
    "/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1", "/LLM/Pipeline/Stage/2"};
  const std::vector<qwen::NativeQwenLayerSplit::LayerRange> ranges = {
    {0, 2}, {2, 3}, {3, 4}};
  const std::map<std::string, std::string> artifacts = {
    {roles[0], digest("qwen-artifact-0")},
    {roles[1], digest("qwen-artifact-1")},
    {roles[2], digest("qwen-artifact-2")}};
  const std::map<std::string, std::uint64_t> weights = {
    {roles[0], 1}, {roles[1], 1}, {roles[2], 1}};

  // Invalid tensor ranks: native keeps the frozen rank-one support range and
  // refuses hybrid degrees instead of fabricating candidates (Python hybrid
  // requires explicit rank artifacts, which the native slice does not model).
  BOOST_CHECK_THROW(qwen::NativeQwenLayerSplit(ranges, artifacts, weights, roles,
                                                {1, 2, 1}),
                    std::invalid_argument);

  // Invalid construction: non-zero start, discontinuous ranges, duplicate
  // roles, incomplete artifact/weight cover, empty weights, non-digest values.
  BOOST_CHECK_THROW(qwen::NativeQwenLayerSplit({{1, 3}, {3, 4}, {4, 5}},
                                               artifacts, weights, roles),
                    std::invalid_argument);
  BOOST_CHECK_THROW(qwen::NativeQwenLayerSplit({{0, 1}, {2, 3}, {3, 4}},
                                               artifacts, weights, roles),
                    std::invalid_argument);
  BOOST_CHECK_THROW(qwen::NativeQwenLayerSplit(ranges, artifacts, weights,
                                               {roles[0], roles[0], roles[1]}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(qwen::NativeQwenLayerSplit(ranges,
                                               {{roles[0], digest("a")},
                                                {roles[1], digest("b")}},
                                               weights, roles),
                    std::invalid_argument);
  BOOST_CHECK_THROW(qwen::NativeQwenLayerSplit(ranges, artifacts,
                                               {{roles[0], 1}, {roles[1], 1}},
                                               roles),
                    std::invalid_argument);
  BOOST_CHECK_THROW(qwen::NativeQwenLayerSplit(ranges,
                                               {{roles[0], "not-a-digest"},
                                                {roles[1], digest("b")},
                                                {roles[2], digest("c")}},
                                               weights, roles),
                    std::invalid_argument);
  BOOST_CHECK_THROW(qwen::NativeQwenLayerSplit(ranges, artifacts,
                                               {{roles[0], 0},
                                                {roles[1], 1},
                                                {roles[2], 1}},
                                               roles),
                    std::invalid_argument);

  // Wrong adapter, wrong node count, missing boundaries, non-canonical layer
  // order, and a graph digest mismatch are rejected by enumerate.
  qwen::NativeQwenLayerSplit splitter(ranges, artifacts, weights, roles);
  auto yoloModel = model("yolo26n", "YOLO26n", graphDigest);
  BOOST_CHECK_THROW(splitter.enumerate(yoloModel, validGraph, {}),
                    std::invalid_argument);
  auto shortGraph = graph(graphDigest,
    {"embedding", "layer-00", "layer-01", "layer-02", "final-norm-head"});
  BOOST_CHECK_THROW(splitter.enumerate(qwenModel, shortGraph, {}),
                    std::invalid_argument);
  auto noEmbedding = graph(graphDigest,
    {"tok", "layer-00", "layer-01", "layer-02", "layer-03", "final-norm-head"});
  BOOST_CHECK_THROW(splitter.enumerate(qwenModel, noEmbedding, {}),
                    std::invalid_argument);
  auto noHead = graph(graphDigest,
    {"embedding", "layer-00", "layer-01", "layer-02", "layer-03", "tail"});
  BOOST_CHECK_THROW(splitter.enumerate(qwenModel, noHead, {}),
                    std::invalid_argument);
  auto scrambled = graph(graphDigest,
    {"embedding", "layer-02", "layer-03", "layer-00", "layer-01",
     "final-norm-head"});
  BOOST_CHECK_THROW(splitter.enumerate(qwenModel, scrambled, {}),
                    std::invalid_argument);
  auto digestMismatch = model("qwen-three-stage-pipeline", "QwenFixture",
                              digest("other-graph"));
  BOOST_CHECK_THROW(splitter.enumerate(digestMismatch, validGraph, {}),
                    std::invalid_argument);
  // A legal input still enumerates after all rejections.
  BOOST_CHECK_EQUAL(splitter.enumerate(qwenModel, validGraph, {}).size(), 1U);
}

BOOST_AUTO_TEST_CASE(QwenLayerSplitEnforcesBudgetBoundaries)
{
  const auto graphDigest = digest("qwen-budget-graph");
  auto graphSnapshot = graph(graphDigest,
    {"embedding", "layer-00", "layer-01", "layer-02", "layer-03",
     "final-norm-head"});
  auto modelDescriptor = model("qwen-three-stage-pipeline", "QwenFixture",
                               graphDigest);
  const std::vector<std::string> roles = {
    "/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1", "/LLM/Pipeline/Stage/2"};
  qwen::NativeQwenLayerSplit splitter(
    {{0, 2}, {2, 3}, {3, 4}},
    {{roles[0], digest("qwen-artifact-0")},
     {roles[1], digest("qwen-artifact-1")},
     {roles[2], digest("qwen-artifact-2")}},
    {{roles[0], 1}, {roles[1], 1}, {roles[2], 1}}, roles);

  // Out-of-range budgets are rejected before any graph work happens.
  BOOST_CHECK_THROW(splitter.enumerate(modelDescriptor, graphSnapshot,
                                       NativeCandidateBudget{0, 100, 1}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(splitter.enumerate(modelDescriptor, graphSnapshot,
                                       NativeCandidateBudget{1025, 100, 1}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(splitter.enumerate(modelDescriptor, graphSnapshot,
                                       NativeCandidateBudget{2, 0, 1}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(splitter.enumerate(modelDescriptor, graphSnapshot,
                                       NativeCandidateBudget{2, 60001, 1}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(splitter.enumerate(modelDescriptor, graphSnapshot,
                                       NativeCandidateBudget{2, 100, 17}),
                    std::invalid_argument);
  // Inclusive limits still yield the deterministic single candidate.
  BOOST_REQUIRE_EQUAL(splitter.enumerate(modelDescriptor, graphSnapshot,
                                         NativeCandidateBudget{1024, 60000, 16})
                        .size(), 1U);
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

BOOST_AUTO_TEST_CASE(YoloComponentSplitRejectsInvalidComponentsAndForeignModel)
{
  const auto graphDigest = digest("yolo-invalid-graph");
  auto graphSnapshot = graph(graphDigest, {"backbone", "neck", "detect", "output"});
  auto yoloModel = model("yolo26n", "YOLO26n", graphDigest);
  const yolo::NativeYoloComponentSpec atomic{
    "atomic-v1", 10, {"FullModel"}, {}, "FullModel", "FullModel", "", {}};

  // Invalid construction: no candidates, duplicate ids, empty roles,
  // duplicate roles, undeclared ingress/egress, malformed candidate digest.
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({}), std::invalid_argument);
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({atomic, atomic}),
                    std::invalid_argument);
  yolo::NativeYoloComponentSpec emptyRoles{
    "e", 1, {}, {}, "FullModel", "FullModel", "", {}};
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({emptyRoles}),
                    std::invalid_argument);
  yolo::NativeYoloComponentSpec dupRoles{
    "d", 1, {"A", "A"}, {{"A", {"backbone"}}}, "A", "A", "", {}};
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({dupRoles}),
                    std::invalid_argument);
  yolo::NativeYoloComponentSpec noIngress{
    "n", 1, {"A"}, {{"A", {"backbone"}}}, "", "A", "", {}};
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({noIngress}),
                    std::invalid_argument);
  yolo::NativeYoloComponentSpec badDigest{
    "b", 1, {"A"}, {{"A", {"backbone"}}}, "A", "A", "", "not-a-digest"};
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({badDigest}),
                    std::invalid_argument);

  // Enumerate rejects a foreign model before any candidate work.
  auto qwenModel = model("qwen-three-stage-pipeline", "QwenFixture", graphDigest);
  yolo::NativeYoloComponentSplit splitter({atomic});
  BOOST_CHECK_THROW(splitter.enumerate(qwenModel, graphSnapshot, {}),
                    std::invalid_argument);

  // Component-level defects that survive construction are rejected per
  // candidate during enumerate: undeclared ingress/egress, an empty semantic
  // node set, a node assigned twice, and a partition whose names do not match
  // the graph.
  const std::vector<std::string> rolesA = {"A", "B"};
  yolo::NativeYoloComponentSpec undeclared{
    "u", 1, rolesA, {{"A", {"backbone"}}, {"B", {"neck"}}}, "C", "B", "", {}};
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({undeclared})
                      .enumerate(yoloModel, graphSnapshot, {}),
                    std::invalid_argument);
  yolo::NativeYoloComponentSpec emptySet{
    "s", 1, rolesA, {{"A", {}}, {"B", {"neck"}}}, "A", "B", "", {}};
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({emptySet})
                      .enumerate(yoloModel, graphSnapshot, {}),
                    std::invalid_argument);
  yolo::NativeYoloComponentSpec doubleAssign{
    "t", 1, rolesA,
    {{"A", {"backbone", "neck"}}, {"B", {"neck", "detect"}}},
    "A", "B", "", {}};
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({doubleAssign})
                      .enumerate(yoloModel, graphSnapshot, {}),
                    std::invalid_argument);
  yolo::NativeYoloComponentSpec wrongNames{
    "w", 1, rolesA,
    {{"A", {"backbone", "neck"}}, {"B", {"detect", "extra"}}},
    "A", "B", "", {}};
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({wrongNames})
                      .enumerate(yoloModel, graphSnapshot, {}),
                    std::invalid_argument);
  // The same legal component still enumerates after all rejections.
  BOOST_CHECK_EQUAL(splitter.enumerate(yoloModel, graphSnapshot, {}).size(), 1U);
}

BOOST_AUTO_TEST_CASE(YoloComponentSplitIsDeterministicAndBudgetTruncates)
{
  const auto graphDigest = digest("yolo-order-graph");
  auto graphSnapshot = graph(graphDigest, {"backbone", "neck", "detect", "output"});
  auto yoloModel = model("yolo26n", "YOLO26n", graphDigest);
  const auto atomic = [] (std::string id, int priority) {
    return yolo::NativeYoloComponentSpec{
      std::move(id), priority, {"FullModel"}, {}, "FullModel", "FullModel", "", {}};
  };
  yolo::NativeYoloComponentSplit splitter({atomic("z", 9), atomic("a", 1),
                                           atomic("m", 5), atomic("b", 1),
                                           atomic("k", 3)});

  // Stable order: priority ascending, then candidateId ascending.
  const auto full = splitter.enumerate(yoloModel, graphSnapshot,
                                       NativeCandidateBudget{1024, 100, 16});
  BOOST_REQUIRE_EQUAL(full.size(), 5U);
  std::vector<std::string> fullDigests;
  for (const auto& candidate : full) fullDigests.push_back(candidate.candidateDigest);

  // Repeated calls are byte-identical (deterministic candidate digest).
  const auto again = splitter.enumerate(yoloModel, graphSnapshot,
                                        NativeCandidateBudget{1024, 100, 16});
  BOOST_REQUIRE_EQUAL(again.size(), 5U);
  for (std::size_t i = 0; i < full.size(); ++i) {
    BOOST_CHECK_EQUAL(again[i].candidateDigest, fullDigests[i]);
  }

  // Budget truncation keeps the head of the same stable order.
  const auto head = splitter.enumerate(yoloModel, graphSnapshot,
                                       NativeCandidateBudget{3, 100, 1});
  BOOST_REQUIRE_EQUAL(head.size(), 3U);
  for (std::size_t i = 0; i < head.size(); ++i) {
    BOOST_CHECK_EQUAL(head[i].candidateDigest, fullDigests[i]);
  }
  const auto one = splitter.enumerate(yoloModel, graphSnapshot,
                                      NativeCandidateBudget{1, 100, 1});
  BOOST_REQUIRE_EQUAL(one.size(), 1U);
  BOOST_CHECK_EQUAL(one.front().candidateDigest, fullDigests.front());
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

  NativePlanSealingInputs inputs;
  inputs.artifacts = {{{role, "/canonical/model"}}, {{role, digest("artifact")}},
                       digest("manifest"), digest("recipe"), snapshot.requestId,
                       snapshot.attempt, modelDescriptor.contentDigest, graphDigest};
  inputs.requesterIdentity = "/requester";
  inputs.protectionEpoch = "protected-v1";
  inputs.expiresAtMs = 2000000000000ULL;
  ndnsf::di::fixture::assemblies(inputs);
  const auto core = NativePlanSealer::sealCore(snapshot, proposal, inputs);
  const NativeSecurityPolicySnapshot security{digest("security-policy"), true};
  const auto view = NativePlanSealer::grantView(core, snapshot.offers[1], security);
  const NativeGrantBinding grant{view.provider, view.role, "/grant/1",
                                 digest("grant-1"), view.provider};
  // The single-role fixture needs one grant; finalizeSecurity verifies exact
  // role/provider coverage before exposing a projection.
  const auto sealed = NativePlanSealer::finalizeSecurity(core, {grant}, security);
  const auto projection = NativePlanSealer::project(sealed, view.provider,
    ndnsf::di::fixture::projection(sealed, view.provider));
  BOOST_REQUIRE(!NativePlanSealer::encode(projection).empty());
}

BOOST_AUTO_TEST_CASE(PreSplitPlacementTieBreakResidencyThenBytesThenProvider)
{
  const auto graphDigest = digest("placement-tiebreak-graph");
  auto graphSnapshot = graph(graphDigest, {"embedding", "layer-00", "final-norm-head"});
  auto modelDescriptor = model("qwen", "QwenFixture", graphDigest);
  const std::string role = "/LLM/Pipeline/Stage/0";
  qwen::NativeQwenLayerSplit splitter(
    {{0, 1}}, {{role, digest("artifact")}}, {{role, 1}}, {role}, {1});
  const auto candidate = splitter.enumerate(modelDescriptor, graphSnapshot,
                                             NativeCandidateBudget{1, 100, 1}).front();
  // qwen requirements fix workspace+activation+transient at ~2.36 GiB after
  // the 1.10 safety margin; offers below that are filtered out, not ranked.
  const auto gb = [] (std::uint64_t value) {
    return value * 1024ULL * 1024ULL * 1024ULL;
  };
  NativePlanningSnapshot snapshot;
  snapshot.model = modelDescriptor;
  snapshot.graph = graphSnapshot;
  snapshot.requestId = "request";
  snapshot.attempt = 1;
  snapshot.ackClosedDigest = digest("ack-closed");
  snapshot.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  // provider-c: the target artifact digest wins the residency key regardless of
  // smaller free bytes; provider-b: same zero residency, larger free bytes
  // wins the budget key; provider-a: lowest provider name is the ref
  // tie-break only when residency and free bytes are identical.
  snapshot.offers = {
    {"provider-a", digest("offer-a"), {role}, {"onnxruntime"}, {},
     gb(4), 1, true, true},
    {"provider-b", digest("offer-b"), {role}, {"onnxruntime"}, {},
     gb(5), 1, true, true},
    {"provider-c", digest("offer-c"), {role}, {"onnxruntime"}, {digest("artifact")},
     gb(3), 1, true, true},
  };
  NativePreSplitFirstPlacement placement;
  auto proposal = placement.propose(snapshot, candidate);
  BOOST_CHECK_EQUAL(proposal.assignment.providerByRole.at(role), "provider-c");
  proposal.validate(snapshot, candidate);
  BOOST_CHECK_EQUAL(proposal.strategy.name, "native-pre-split-first");
  BOOST_CHECK_EQUAL(proposal.strategy.configurationDigest,
                    NativePreSplitFirstPlacement().identity().configurationDigest);

  // Unrelated or repeated cache hints confer no advantage, however numerous.
  snapshot.offers[2].residencyDigests = {digest("other"), digest("other"), digest("third")};
  BOOST_CHECK_EQUAL(placement.propose(snapshot, candidate).assignment.providerByRole.at(role),
                    "provider-b");

  // Same inputs, same proposal: the deterministic ref tie-break picks the
  // smallest provider name when residency and budget keys are tied.
  snapshot.offers = {
    {"provider-x", digest("offer-x"), {role}, {"onnxruntime"}, {},
     gb(4), 1, true, true},
    {"provider-a", digest("offer-a"), {role}, {"onnxruntime"}, {},
     gb(4), 1, true, true},
  };
  const auto first = placement.propose(snapshot, candidate);
  BOOST_CHECK_EQUAL(first.assignment.providerByRole.at(role), "provider-a");
  const auto second = placement.propose(snapshot, candidate);
  BOOST_CHECK_EQUAL(second.assignment.providerByRole.at(role), "provider-a");
  BOOST_CHECK_EQUAL(second.requestId, first.requestId);
  BOOST_CHECK_EQUAL(second.candidateDigest, first.candidateDigest);
  BOOST_CHECK_EQUAL(second.strategy.configurationDigest,
                    first.strategy.configurationDigest);
}

BOOST_AUTO_TEST_CASE(PreSplitPlacementFiltersIneligibleOffersAndRejectsEmpty)
{
  const auto graphDigest = digest("placement-filter-graph");
  auto graphSnapshot = graph(graphDigest, {"embedding", "layer-00", "final-norm-head"});
  auto modelDescriptor = model("qwen", "QwenFixture", graphDigest);
  const std::string role = "/LLM/Pipeline/Stage/0";
  qwen::NativeQwenLayerSplit splitter(
    {{0, 1}}, {{role, digest("artifact")}}, {{role, 1}}, {role}, {1});
  const auto candidate = splitter.enumerate(modelDescriptor, graphSnapshot,
                                             NativeCandidateBudget{1, 100, 1}).front();
  const auto gb = [] (std::uint64_t value) {
    return value * 1024ULL * 1024ULL * 1024ULL;
  };
  NativePreSplitFirstPlacement placement;

  // Each incompatible offer is filtered on its own: an unaccepted role, a
  // backend that does not cover onnxruntime, and free bytes below the
  // required ~2.36 GiB (1.10 safety margin included).
  NativePlanningSnapshot snapshot;
  snapshot.model = modelDescriptor;
  snapshot.graph = graphSnapshot;
  snapshot.requestId = "request";
  snapshot.attempt = 1;
  snapshot.ackClosedDigest = digest("ack-closed");
  snapshot.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  snapshot.offers = {
    {"provider-role", digest("offer-role"), {"/other/role"}, {"onnxruntime"}, {},
     gb(8), 1, true, true},
  };
  BOOST_CHECK_THROW(placement.propose(snapshot, candidate), std::runtime_error);
  snapshot.offers = {
    {"provider-backend", digest("offer-backend"), {role}, {"tensorrt"}, {},
     gb(8), 1, true, true},
  };
  BOOST_CHECK_THROW(placement.propose(snapshot, candidate), std::runtime_error);
  snapshot.offers = {
    {"provider-bytes", digest("offer-bytes"), {role}, {"onnxruntime"}, {},
     gb(1), 1, true, true},
  };
  BOOST_CHECK_THROW(placement.propose(snapshot, candidate), std::runtime_error);
  // A deadline that already expired makes the whole snapshot invalid.
  snapshot.offers = {
    {"provider-a", digest("offer-a"), {role}, {"onnxruntime"}, {},
     gb(8), 1, true, true},
  };
  snapshot.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  BOOST_CHECK_THROW(placement.propose(snapshot, candidate), std::invalid_argument);
  // An invalid offer (execution not allowed) is rejected by snapshot
  // validation before any placement work happens.
  snapshot.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  snapshot.offers = {
    {"provider-a", digest("offer-a"), {role}, {"onnxruntime"}, {},
     gb(8), 1, true, false},
  };
  BOOST_CHECK_THROW(placement.propose(snapshot, candidate), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PlacementAssignsDistinctRoleSpecificProvidersAndSeals)
{
  TwoRolePlacement fixture;
  NativePreSplitFirstPlacement placement;
  const auto proposal = placement.propose(fixture.snapshot, fixture.candidate);
  BOOST_CHECK_EQUAL(proposal.assignment.providerByRole.at(fixture.first), "provider-a");
  BOOST_CHECK_EQUAL(proposal.assignment.providerByRole.at(fixture.second), "provider-b");
  NativePlanSealingInputs inputs;
  inputs.artifacts = {{{fixture.first, "/canonical/first"}, {fixture.second, "/canonical/second"}},
    {{fixture.first, digest("first-artifact")}, {fixture.second, digest("second-artifact")}},
    digest("manifest"), digest("recipe"), fixture.snapshot.requestId, fixture.snapshot.attempt,
    fixture.snapshot.model.contentDigest, fixture.snapshot.graph.graphDigest};
  inputs.requesterIdentity = "/requester";
  inputs.protectionEpoch = "protected-v1";
  inputs.expiresAtMs = 2000000000000ULL;
  ndnsf::di::fixture::assemblies(inputs);
  const auto core = NativePlanSealer::sealCore(fixture.snapshot, proposal, inputs);
  BOOST_CHECK_EQUAL(core.assignment.providerByRole.size(), 2U);
  for (const auto& offer : fixture.snapshot.offers) {
    const auto view = NativePlanSealer::grantView(core, offer, {digest("policy"), true});
    BOOST_CHECK_EQUAL(core.assignment.providerByRole.at(view.role), offer.provider);
    BOOST_CHECK_EQUAL(view.artifactDigest, inputs.artifacts.artifactDigestByRole.at(view.role));
  }
}

BOOST_AUTO_TEST_CASE(PlacementNeverReusesProviderAndIsOrderIndependent)
{
  TwoRolePlacement fixture;
  for (auto& offer : fixture.snapshot.offers) offer.acceptedRoles = {fixture.first, fixture.second};
  const auto first = NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate);
  BOOST_CHECK_EQUAL(first.assignment.providerByRole.at(fixture.first), "provider-a");
  BOOST_CHECK_EQUAL(first.assignment.providerByRole.at(fixture.second), "provider-b");
  std::reverse(fixture.snapshot.offers.begin(), fixture.snapshot.offers.end());
  std::reverse(fixture.candidate.executionPlan.roles.begin(), fixture.candidate.executionPlan.roles.end());
  const auto reversed = NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate);
  BOOST_CHECK(first.assignment.providerByRole == reversed.assignment.providerByRole);
  fixture.snapshot.offers.resize(1);
  BOOST_CHECK_THROW(NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(PlacementRejectsForgedAssignmentsAndDuplicateOffers)
{
  TwoRolePlacement fixture;
  auto proposal = NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate);
  proposal.assignment.providerByRole[fixture.second] = "provider-a";
  BOOST_CHECK_THROW(proposal.validate(fixture.snapshot, fixture.candidate), std::invalid_argument);
  proposal.assignment.providerByRole[fixture.second] = "foreign";
  BOOST_CHECK_THROW(proposal.validate(fixture.snapshot, fixture.candidate), std::invalid_argument);
  proposal.assignment.providerByRole[fixture.second] = "provider-b";
  fixture.snapshot.offers[1].freeBytes = 1;
  BOOST_CHECK_THROW(proposal.validate(fixture.snapshot, fixture.candidate), std::invalid_argument);
  fixture.snapshot.offers.push_back(fixture.snapshot.offers.front());
  BOOST_CHECK_THROW(NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PlacementRejectsInvalidAndOverflowingResourceBudgets)
{
  TwoRolePlacement fixture;
  for (const auto margin : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
    auto candidate = fixture.candidate;
    candidate.requirementsByRole.at(fixture.first).safetyMargin = margin;
    BOOST_CHECK_THROW(NativePreSplitFirstPlacement().propose(fixture.snapshot, candidate),
                      std::invalid_argument);
  }
  auto& requirement = fixture.candidate.requirementsByRole.at(fixture.first);
  requirement.weightBytes = std::numeric_limits<std::uint64_t>::max();
  requirement.workspaceBytes = 2;
  requirement.safetyMargin = 1.0;
  BOOST_CHECK_THROW(NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate),
                    std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
