#include "tests/boost-test.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-split.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-backend.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::di::tests {
namespace {

NativeTensorContract
splitTensor(const std::string& name, std::vector<std::int64_t> shape = {1, 2})
{
  NativeTensorContract result;
  result.name = name;
  result.dtype = "float32";
  for (const auto axis : shape) result.shape.emplace_back(axis);
  return result;
}

AutomaticAttentionState
splitState(AutomaticAttentionState::Role role, const std::string& layer)
{
  AutomaticAttentionState result;
  result.role = role;
  result.update = AutomaticAttentionState::Update::REPLACE_FULL;
  result.operatorIndex = 1;
  result.input = "graph/past." + layer + (role == AutomaticAttentionState::Role::KEY ?
    ".key" : ".value");
  result.output = "graph/present." + layer + (role == AutomaticAttentionState::Role::KEY ?
    ".key" : ".value");
  result.elementType = TensorElementType::Float32;
  result.inputAxes = {0, 1, 2, 3};
  result.outputAxes = {0, 1, 2, 3};
  result.heads = 2;
  result.headSize = 4;
  return result;
}

/** Two-producer graph whose only interior cut t2 leaves one state pair on
 * each side. Every edge is a declared legal cut unless a test removes one. */
AutomaticOnnxModel
splitFixture(bool singlePair = false, bool splitPair = false, bool dropT2 = false)
{
  NativeGraphSnapshot graph;
  for (unsigned index = 0; index < 6; ++index) {
    graph.nodes.push_back({"onnx-node-" + std::to_string(index), "Op", index});
    graph.topologicalOrder.push_back("onnx-node-" + std::to_string(index));
  }
  const auto edge = [&] (const std::string& name, unsigned producer,
                         unsigned consumer) {
    NativeGraphEdge value;
    value.id = name;
    value.producer = "onnx-node-" + std::to_string(producer);
    value.consumers = {"onnx-node-" + std::to_string(consumer)};
    value.tensor = splitTensor(name);
    graph.edges.push_back(std::move(value));
    if (!(dropT2 && name == "t2"))
      graph.legalCutEdges.push_back(name);
  };
  edge("t1", 0, 1);
  edge("present.0.key", 1, 2);
  edge("present.0.value", 1, 2);
  edge("t2", 2, 3);
  edge("present.1.key", 3, 4);
  edge("present.1.value", 3, 4);
  edge("logits", 4, 5);
  graph.modelInputs = {splitTensor("input"), splitTensor("past.0.key"),
                       splitTensor("past.0.value"), splitTensor("past.1.key"),
                       splitTensor("past.1.value")};
  graph.modelOutputs = {splitTensor("logits"), splitTensor("present.0.key"),
                        splitTensor("present.0.value"), splitTensor("present.1.key"),
                        splitTensor("present.1.value")};
  graph.graphDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"fixture", "automatic-split"}, {"nodes", dropT2 ? "drop-t2" : "full"}}));
  auto descriptor = fixture::completeModel(NativeModelDescriptor{
    "/automatic/split", nativePlanningDigest("content"), nativePlanningDigest("semantics"),
    graph.graphDigest, "onnx", "float32", "onnx", "1", {}, "rev-1"});
  AutomaticOnnxModel model;
  model.descriptor = std::move(descriptor);
  model.inspection.graph = std::move(graph);
  AutomaticGenerationContract generation;
  generation.tokenInput = "graph/input";
  generation.logitsOutput = "graph/logits";
  generation.tokenBatchAxis = 0;
  generation.tokenSequenceAxis = 1;
  generation.logitsBatchAxis = 0;
  generation.logitsSequenceAxis = 1;
  generation.logitsVocabularyAxis = 2;
  generation.vocabularySize = 16;
  generation.contextLimit = 32;
  generation.ruleId = "causal-gqa";
  if (splitPair) {
    auto key = splitState(AutomaticAttentionState::Role::KEY, "0");
    key.output = "graph/present.1.key";
    generation.states = {key, splitState(AutomaticAttentionState::Role::VALUE, "0")};
  }
  else if (singlePair) {
    generation.states = {splitState(AutomaticAttentionState::Role::KEY, "1"),
                         splitState(AutomaticAttentionState::Role::VALUE, "1")};
  }
  else {
    generation.states = {splitState(AutomaticAttentionState::Role::KEY, "0"),
                         splitState(AutomaticAttentionState::Role::VALUE, "0"),
                         splitState(AutomaticAttentionState::Role::KEY, "1"),
                         splitState(AutomaticAttentionState::Role::VALUE, "1")};
  }
  model.contract.generation = std::move(generation);
  return model;
}

std::shared_ptr<const AutomaticOnnxModel>
splitSource(std::shared_ptr<AutomaticOnnxModel> model)
{
  return model;
}

NativeCandidateBudget
splitBudget(std::size_t candidates = 4)
{
  NativeCandidateBudget budget;
  budget.maxCandidates = candidates;
  budget.maxPolicyMs = 100;
  budget.maxReentries = 1;
  return budget;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec233AutomaticSplit)

BOOST_AUTO_TEST_CASE(InspectsOnlyItsOwnPlanningGraph)
{
  auto model = std::make_shared<AutomaticOnnxModel>(splitFixture());
  const auto graph = model->inspection.graph;
  AutomaticOnnxSplitStrategy strategy(splitSource(model));
  BOOST_CHECK(strategy.identity().deterministic);
  BOOST_CHECK_EQUAL(strategy.identity().name, "native-automatic-onnx-stage");
  const auto inspected = strategy.inspectGraph(model->descriptor, "rev-1", 100);
  BOOST_CHECK_EQUAL(inspected.graphDigest, graph.graphDigest);
  BOOST_CHECK_EQUAL(inspected.nodes.size(), graph.nodes.size());
  BOOST_CHECK_THROW(strategy.inspectGraph(model->descriptor, "rev-2", 100),
                    std::invalid_argument);
  BOOST_CHECK_THROW(strategy.inspectGraph(model->descriptor, "rev-1", 2),
                    std::invalid_argument);
  BOOST_CHECK_THROW(AutomaticOnnxSplitStrategy(nullptr), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CutsOnlyWhereBothSidesOwnACompleteStatePair)
{
  auto model = std::make_shared<AutomaticOnnxModel>(splitFixture());
  AutomaticOnnxSplitStrategy strategy(splitSource(model));
  const auto candidates = strategy.enumerate(model->descriptor, model->inspection.graph,
                                             splitBudget());
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  const auto& candidate = candidates.front();
  BOOST_CHECK_EQUAL(candidate.graphDigest, model->inspection.graph.graphDigest);
  BOOST_REQUIRE_EQUAL(candidate.executionPlan.roles.size(), 2U);
  BOOST_CHECK_EQUAL(candidate.nodeRoles.size(), model->inspection.graph.nodes.size());
  BOOST_CHECK_EQUAL(candidate.crossPartitionTensors.size(), 1U);
  BOOST_CHECK_EQUAL(candidate.crossPartitionTensors.front(), "t2");
  BOOST_REQUIRE_EQUAL(candidate.executionPlan.dependencies.size(), 1U);
  BOOST_CHECK_EQUAL(candidate.executionPlan.dependencies.front().tensors.front(), "t2");
  BOOST_REQUIRE_EQUAL(candidate.fragmentsByRole.size(), 2U);
  BOOST_REQUIRE_EQUAL(candidate.requirementsByRole.size(), 2U);
  BOOST_CHECK_EQUAL(candidate.roleStateInputsByRole.size(), 2U);
  BOOST_CHECK_EQUAL(candidate.roleStateOutputsByRole.size(), 2U);
  for (const auto& [role, inputs] : candidate.roleStateInputsByRole) {
    BOOST_CHECK(!inputs.empty());
    BOOST_REQUIRE(!candidate.roleStateOutputsByRole.at(role).empty());
  }
  BOOST_CHECK_EQUAL(candidate.candidateDigest, candidate.computedDigest());
  BOOST_CHECK_EQUAL(candidate.executionPlan.dependencies.front().producers.front(),
                    candidate.inputIngressRole);
  BOOST_CHECK_EQUAL(candidate.executionPlan.dependencies.front().consumers.front(),
                    candidate.resultEgressRole);
  BOOST_CHECK_EQUAL(candidate.roleStateOutputsByRole.at(candidate.resultEgressRole).size(), 2U);
  BOOST_CHECK_EQUAL(candidate.roleStateOutputsByRole.at(candidate.inputIngressRole).size(), 2U);
  BOOST_CHECK_NO_THROW(candidate.validate(model->inspection.graph));
  for (const auto& [role, requirement] : candidate.requirementsByRole) {
    BOOST_CHECK(requirement.weightBytes.has_value());
    BOOST_CHECK(requirement.kvBytes.has_value());
    BOOST_CHECK_GT(*requirement.kvBytes, 0U);
  }
  const auto prefixKv = *candidate.requirementsByRole.at(
    candidate.inputIngressRole).kvBytes;
  const auto suffixKv = *candidate.requirementsByRole.at(
    candidate.resultEgressRole).kvBytes;
  BOOST_CHECK_EQUAL(prefixKv, suffixKv);
}

BOOST_AUTO_TEST_CASE(ReturnsNoCandidateWithoutALegalStatePreservingCut)
{
  auto single = std::make_shared<AutomaticOnnxModel>(splitFixture(true));
  AutomaticOnnxSplitStrategy singleStrategy(splitSource(single));
  BOOST_CHECK(singleStrategy.enumerate(single->descriptor, single->inspection.graph,
                                       splitBudget()).empty());
  auto mixed = std::make_shared<AutomaticOnnxModel>(splitFixture(false, true));
  AutomaticOnnxSplitStrategy mixedStrategy(splitSource(mixed));
  BOOST_CHECK(mixedStrategy.enumerate(mixed->descriptor, mixed->inspection.graph,
                                      splitBudget()).empty());
  auto undeclared = std::make_shared<AutomaticOnnxModel>(splitFixture(false, false, true));
  AutomaticOnnxSplitStrategy undeclaredStrategy(splitSource(undeclared));
  BOOST_CHECK(undeclaredStrategy.enumerate(undeclared->descriptor,
                                           undeclared->inspection.graph,
                                           splitBudget()).empty());
  auto stateless = std::make_shared<AutomaticOnnxModel>(splitFixture());
  stateless->contract.generation.reset();
  AutomaticOnnxSplitStrategy statelessStrategy(splitSource(stateless));
  BOOST_CHECK(statelessStrategy.enumerate(stateless->descriptor,
                                          stateless->inspection.graph,
                                          splitBudget()).empty());
}

BOOST_AUTO_TEST_CASE(RespectsCandidateBudgetAndRejectsAnotherGraph)
{
  auto model = std::make_shared<AutomaticOnnxModel>(splitFixture());
  AutomaticOnnxSplitStrategy strategy(splitSource(model));
  BOOST_CHECK_EQUAL(strategy.enumerate(model->descriptor, model->inspection.graph,
                                       splitBudget(1)).size(), 1U);
  auto other = model->inspection.graph;
  other.graphDigest = nativePlanningDigest("another-graph");
  BOOST_CHECK_THROW(strategy.enumerate(model->descriptor, other, splitBudget()),
                    std::invalid_argument);
}


BOOST_AUTO_TEST_CASE(DerivesOneOwnerStageProjection)
{
  auto model = std::make_shared<AutomaticOnnxModel>(splitFixture());
  AutomaticOnnxSplitStrategy strategy(splitSource(model));
  const auto candidates = strategy.enumerate(model->descriptor, model->inspection.graph,
                                             splitBudget());
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  const auto bindings = deriveAutomaticOnnxStageBindings(*model, candidates.front());
  BOOST_REQUIRE_EQUAL(bindings.size(), 2U);
  BOOST_CHECK_EQUAL(bindings.front().role, candidates.front().inputIngressRole);
  BOOST_CHECK(!bindings.front().terminal);
  BOOST_CHECK(bindings.back().terminal);
  BOOST_REQUIRE_EQUAL(bindings.front().activationOutputs.size(), 1U);
  BOOST_CHECK_EQUAL(bindings.front().activationOutputs.front(), "t2");
  BOOST_REQUIRE_EQUAL(bindings.back().activationInputs.size(), 1U);
  BOOST_CHECK_EQUAL(bindings.back().activationInputs.front(), "t2");
  BOOST_CHECK_EQUAL(bindings.front().stateOutputs.size(), 2U);
  BOOST_CHECK_EQUAL(bindings.back().stateOutputs.size(), 2U);
  auto broken = candidates.front();
  broken.nodeRoles.erase(model->inspection.graph.nodes.front().id);
  BOOST_CHECK_THROW(deriveAutomaticOnnxStageBindings(*model, broken), std::invalid_argument);
  auto& states = model->contract.generation->states;
  const auto saved = states[3].output;
  states[3].output = "graph/present.9.value";
  BOOST_CHECK_THROW(deriveAutomaticOnnxStageBindings(*model, candidates.front()),
                    std::invalid_argument);
  states[3].output = saved;
  auto nonTerminal = candidates.front();
  nonTerminal.resultEgressRole = nonTerminal.inputIngressRole;
  BOOST_CHECK_THROW(deriveAutomaticOnnxStageBindings(*model, nonTerminal),
                    std::invalid_argument);
  auto extraCut = candidates.front();
  extraCut.crossPartitionTensors.push_back("t1");
  BOOST_CHECK_THROW(deriveAutomaticOnnxStageBindings(*model, extraCut),
                    std::invalid_argument);
}


BOOST_AUTO_TEST_CASE(PlacesTwoStagesOnTwoProvidersAndRejectsUnknownResources)
{
  auto model = std::make_shared<AutomaticOnnxModel>(splitFixture());
  AutomaticOnnxSplitStrategy strategy(splitSource(model));
  const auto candidates = strategy.enumerate(model->descriptor, model->inspection.graph,
                                             splitBudget());
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  auto candidate = candidates.front();
  const auto roles = candidate.executionPlan.roles;
  BOOST_REQUIRE_EQUAL(roles.size(), 2U);
  NativePlanningSnapshot snapshot;
  snapshot.model = model->descriptor;
  snapshot.graph = model->inspection.graph;
  snapshot.requestId = "/placement/request";
  snapshot.attempt = 1;
  snapshot.ackClosedDigest = nativePlanningDigest("ack");
  snapshot.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  const auto planningView = [&] (const std::string& provider, const std::string& digest) {
    NativeProviderPlanningView view;
    view.provider = provider;
    view.offerDigest = nativePlanningDigest(digest);
    view.resourceSequence = 1;
    view.backends = {"onnxruntime"};
    view.acceptedRoles = roles;
    view.freeBytes = 1ULL << 30;
    view.preparationAccepted = true;
    view.executionAllowed = true;
    return view;
  };
  snapshot.offers = {planningView("/provider/a", "offer-a"),
                     planningView("/provider/b", "offer-b")};
  NativePreSplitFirstPlacement placement;
  // An unknown workspace/transient keeps the candidate out of an executable plan.
  BOOST_CHECK_THROW(placement.propose(snapshot, candidate), std::runtime_error);
  for (auto& [role, requirement] : candidate.requirementsByRole) {
    requirement.workspaceBytes = 1ULL << 20;
    requirement.transientBytes = 1ULL << 20;
    if (!requirement.activationBytes) requirement.activationBytes = 1ULL << 20;
  }
  candidate.candidateDigest = candidate.computedDigest();
  const auto proposal = placement.propose(snapshot, candidate);
  BOOST_REQUIRE_EQUAL(proposal.assignment.providerByRole.size(), 2U);
  BOOST_CHECK_NE(proposal.assignment.providerByRole.at(roles[0]),
                 proposal.assignment.providerByRole.at(roles[1]));
  snapshot.offers.pop_back();
  const auto coLocated = placement.propose(snapshot, candidate);
  BOOST_CHECK_EQUAL(coLocated.assignment.providerByRole.at(roles[0]), "/provider/a");
  BOOST_CHECK_EQUAL(coLocated.assignment.providerByRole.at(roles[1]), "/provider/a");
  snapshot.offers = {planningView("/provider/a", "offer-a"),
                     planningView("/provider/b", "offer-b")};
  for (auto& offer : snapshot.offers) offer.freeBytes = 1024;
  BOOST_CHECK_THROW(placement.propose(snapshot, candidate), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(EmptyFloat16StateKeepsItsWireType)
{
  NamedTensor empty{"past_key_values.0.key", TensorElementType::Float16, {1, 8, 0, 128}, {}};
  const auto bundle = makeEncodedTensorBundle("input", {empty});
  const auto decoded = decodeTensorBundle(bundle.payload);
  BOOST_REQUIRE_EQUAL(decoded.size(), 1U);
  BOOST_CHECK(decoded.front().elementType == TensorElementType::Float16);
  BOOST_CHECK(decoded.front().shape == empty.shape);
}

BOOST_AUTO_TEST_CASE(BackendCapabilitiesAreRequiredNotAssumed)
{
  auto model = std::make_shared<AutomaticOnnxModel>(splitFixture());
  const auto requirement = automaticOnnxBackendRequirement(*model,
    "ndnsf-di-automatic-onnx-contract-v3");
  BOOST_CHECK(!requirement.capabilities.empty());
  BOOST_CHECK(std::find(requirement.capabilities.begin(), requirement.capabilities.end(),
                        "automatic-dtype:float32") != requirement.capabilities.end());
  NativeProviderPlanningView offer;
  offer.provider = "/provider/a";
  offer.offerDigest = nativePlanningDigest("backend-offer");
  offer.resourceSequence = 1;
  offer.backends = {"onnxruntime"};
  offer.preparationAccepted = true;
  offer.executionAllowed = true;
  offer.capabilities = requirement.capabilities;
  BOOST_CHECK(!checkAutomaticOnnxBackend(requirement, offer));
  auto missing = offer;
  missing.capabilities.pop_back();
  const auto error = checkAutomaticOnnxBackend(requirement, missing);
  BOOST_REQUIRE(error);
  BOOST_CHECK(error->find("AUTOMATIC_BACKEND_CAPABILITY_MISSING:") == 0);
  auto wrongDtype = offer;
  wrongDtype.capabilities.erase(std::remove(wrongDtype.capabilities.begin(),
    wrongDtype.capabilities.end(), "automatic-dtype:float32"), wrongDtype.capabilities.end());
  const auto dtypeError = checkAutomaticOnnxBackend(requirement, wrongDtype);
  BOOST_REQUIRE(dtypeError);
  BOOST_CHECK_EQUAL(*dtypeError, "AUTOMATIC_BACKEND_CAPABILITY_MISSING:automatic-dtype:float32");
  auto unknown = splitFixture();
  unknown.contract.generation.reset();
  auto stateless = std::make_shared<AutomaticOnnxModel>(std::move(unknown));
  BOOST_CHECK_THROW(automaticOnnxBackendRequirement(*stateless, "ndnsf-di-automatic-onnx-contract-v3"),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StageRunnerExposesExactlyItsProjection)
{
  auto model = std::make_shared<AutomaticOnnxModel>(splitFixture());
  AutomaticOnnxSplitStrategy strategy(splitSource(model));
  const auto candidates = strategy.enumerate(model->descriptor, model->inspection.graph,
                                             splitBudget());
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  const auto& candidate = candidates.front();
  const auto bindings = deriveAutomaticOnnxStageBindings(*model, candidate);
  BOOST_REQUIRE_EQUAL(bindings.size(), 2U);
  const auto makeSpec = [] (const AutomaticOnnxStageBinding& binding,
                            std::vector<std::string> inputs,
                            std::vector<std::string> outputs, bool terminal) {
    AutomaticOnnxStageRunnerContract value;
    value.role = binding.role;
    value.terminal = terminal;
    value.inputs = std::move(inputs);
    value.outputs = std::move(outputs);
    NativeModelRunnerSpec spec;
    spec.metadata["automaticStageBinding"] = value.canonicalJson();
    return spec;
  };
  const auto ingress = bindings.front();
  std::vector<std::string> ingressInputs = ingress.stateInputs;
  ingressInputs.insert(ingressInputs.end(), ingress.activationInputs.begin(),
                       ingress.activationInputs.end());
  ingressInputs.push_back("input");
  std::vector<std::string> ingressOutputs = ingress.stateOutputs;
  ingressOutputs.insert(ingressOutputs.end(), ingress.activationOutputs.begin(),
                        ingress.activationOutputs.end());
  const auto ingressSpec = makeSpec(ingress, ingressInputs, ingressOutputs, false);
  const auto ingressError = checkAutomaticOnnxStageRunner(ingressSpec, ingress, candidate,
                                                          model->contract);
  BOOST_CHECK_MESSAGE(!ingressError,
    "ingress error=" << (ingressError ? *ingressError : "none"));
  const auto terminal = bindings.back();
  std::vector<std::string> terminalInputs = terminal.stateInputs;
  terminalInputs.insert(terminalInputs.end(), terminal.activationInputs.begin(),
                        terminal.activationInputs.end());
  std::vector<std::string> terminalOutputs = terminal.stateOutputs;
  terminalOutputs.insert(terminalOutputs.end(), terminal.activationOutputs.begin(),
                         terminal.activationOutputs.end());
  terminalOutputs.push_back("logits");
  const auto terminalSpec = makeSpec(terminal, terminalInputs, terminalOutputs, true);
  const auto terminalError = checkAutomaticOnnxStageRunner(terminalSpec, terminal, candidate,
                                                           model->contract);
  BOOST_CHECK_MESSAGE(!terminalError,
    "terminal error=" << (terminalError ? *terminalError : "none"));
  auto extraInput = makeSpec(ingress, ingressInputs, ingress.activationOutputs, false);
  auto extraContract = AutomaticOnnxStageRunnerContract::fromJson(
    extraInput.metadata.at("automaticStageBinding"));
  extraContract.inputs.push_back("past_key_values.99.key");
  extraInput.metadata["automaticStageBinding"] = extraContract.canonicalJson();
  BOOST_CHECK_EQUAL(*checkAutomaticOnnxStageRunner(extraInput, ingress, candidate,
    model->contract), "AUTOMATIC_STAGE_RUNNER_INPUTS");
  auto wrongTerminal = makeSpec(terminal, terminalInputs, terminalOutputs, false);
  BOOST_CHECK_EQUAL(*checkAutomaticOnnxStageRunner(wrongTerminal, terminal, candidate,
    model->contract), "AUTOMATIC_STAGE_RUNNER_TERMINAL");
  auto missingLogits = makeSpec(terminal, terminalInputs, terminal.stateOutputs, true);
  BOOST_CHECK_EQUAL(*checkAutomaticOnnxStageRunner(missingLogits, terminal, candidate,
    model->contract), "AUTOMATIC_STAGE_RUNNER_OUTPUTS");
  auto missingFrontier = makeSpec(terminal, terminal.stateInputs, terminalOutputs, true);
  BOOST_CHECK_EQUAL(*checkAutomaticOnnxStageRunner(missingFrontier, terminal, candidate,
    model->contract), "AUTOMATIC_STAGE_RUNNER_INPUTS");
  auto none = makeSpec(ingress, ingressInputs, ingress.activationOutputs, false);
  none.metadata.erase("automaticStageBinding");
  BOOST_CHECK_EQUAL(*checkAutomaticOnnxStageRunner(none, ingress, candidate,
    model->contract), "AUTOMATIC_STAGE_RUNNER_CONTRACT_MISSING");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
