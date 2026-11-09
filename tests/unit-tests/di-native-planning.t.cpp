#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "tests/fixtures/spec182/native-catalog-test-access.hpp"
#include "tests/fixtures/spec182/native-qwen-config.hpp"
#include "tests/fixtures/spec182/native-candidate-json-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <fstream>
#include "tests/fixtures/spec182/native-sealing-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-preparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp"

#include <boost/test/unit_test.hpp>
#include <onnx/shape_inference/implementation.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <algorithm>
#include <limits>
#include <openssl/evp.h>

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
  return fixture::completeModel({name, digest(name + "-content"), digest(name + "-semantics"),
          graphDigest, "onnx", "float32", adapter, "1"});
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
  // Explicit chain fixture. Production graph inspection supplies real edges;
  // the planner never invents adjacency or tensor names.
  for (std::size_t i = 1; i < result.nodes.size(); ++i) {
    const auto id = result.nodes[i - 1].id.find("layer-") == 0 && result.nodes[i].id.find("layer-") == 0
      ? "hidden-layer-" + std::to_string(i - 2) + "-to-" + std::to_string(i - 1)
      : "cut-" + std::to_string(i - 1);
    result.edges.push_back({id, result.nodes[i - 1].id, {result.nodes[i].id},
      {id, "float32", {std::int64_t(1)}, 4}});
    result.legalCutEdges.push_back(id);
  }
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
    fixture::assignPlacementFixtureResourceBudgets(candidate);
    snapshot.offers = {
      {"provider-a", digest("offer-a"), {first}, {"onnxruntime"}, {},
        4ULL << 30, 1, true, true},
      {"provider-b", digest("offer-b"), {second}, {"onnxruntime"}, {},
        4ULL << 30, 1, true, true},
    };
  }
};

void
checkQwenPlannerMigration(NativeSplitCandidate& actual, const NativeJson& frozen,
                          const NativeGraphSnapshot& snapshot)
{
  // Preserve the historical full-identity oracle as a decoder/serialization
  // check. New abstract state defaults have their independent assertions below.
  auto legacy = fixture::candidateFromJson(frozen);
  legacy.candidateDigest = legacy.computedDigest();
  BOOST_CHECK_EQUAL(legacy.canonicalJson(), nativeCanonicalJson(frozen));
  BOOST_CHECK_NO_THROW(legacy.validate(snapshot));
  fixture::assignPlacementFixtureResourceBudgets(actual);
  BOOST_CHECK_NO_THROW(actual.validate(snapshot));
  auto currentFields = nativeParseJson(actual.canonicalJson());
  auto frozenFields = frozen;
  for (const auto* key : {"role_state_inputs_by_role", "role_state_outputs_by_role"}) {
    currentFields.erase(key);
    frozenFields.erase(key);
  }
  BOOST_CHECK(currentFields == frozenFields);
  BOOST_CHECK_EQUAL(actual.candidateDigest, actual.computedDigest());
  BOOST_CHECK(actual.candidateDigest != legacy.computedDigest());
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182NativePlanning)

BOOST_AUTO_TEST_CASE(SourceInitializerAnnotationsDoNotDisableShapeInference)
{
  for (const std::string mode : {"missing", "scalar", "known", "unresolved",
                                 "custom", "legacy", "interface"}) {
    onnx::ModelProto proto;
    proto.set_ir_version(8);
    proto.add_opset_import()->set_version(mode == "legacy" ? 13 : 14);
    auto* g = proto.mutable_graph();
    g->set_name("source-reshape-rank");
    const auto port = [](auto* value, const std::string& name, int dtype,
                         const std::vector<std::int64_t>& shape) {
      value->set_name(name);
      auto* type = value->mutable_type()->mutable_tensor_type();
      type->set_elem_type(dtype);
      type->mutable_shape();
      for (const auto dimension : shape)
        type->mutable_shape()->add_dim()->set_dim_value(dimension);
    };
    port(g->add_input(), "position", onnx::TensorProto::INT64, {1, 5});
    port(g->add_input(), "shape", onnx::TensorProto::INT64, {2});
    if (mode == "unresolved") {
      auto* dimension = g->mutable_input(1)->mutable_type()->mutable_tensor_type()
                          ->mutable_shape()->mutable_dim(0);
      dimension->clear_dim_value();
      dimension->set_dim_param("unknown_width");
    }
    auto* boundary = g->add_value_info();
    port(boundary, "boundary", onnx::TensorProto::INT64, mode == "known"
      ? std::vector<std::int64_t>{1, 5} : std::vector<std::int64_t>{});
    if (mode != "known" && mode != "scalar")
      boundary->mutable_type()->mutable_tensor_type()->clear_shape();
    port(g->add_output(), "result", onnx::TensorProto::INT64, {});
    g->mutable_output(0)->mutable_type()->mutable_tensor_type()->clear_shape();
    auto* reshape = g->add_node();
    reshape->set_op_type("Reshape");
    if (mode == "custom") {
      reshape->set_domain("custom");
      auto* imported = proto.add_opset_import();
      imported->set_domain("custom");
      imported->set_version(1);
    }
    reshape->add_input("position");
    reshape->add_input("shape");
    reshape->add_output("boundary");
    auto* consume = g->add_node();
    consume->set_op_type("Identity");
    consume->add_input("boundary");
    consume->add_output("result");
    // Initializer/type consistency is checked before per-node inference;
    // ordinary node inference errors may be suppressed by ONNX's default mode.
    if (mode == "interface")
      port(g->add_input(), "bad_input", onnx::TensorProto::FLOAT, {3});
    else
      port(g->add_value_info(), "bad_input", onnx::TensorProto::FLOAT, {3});
    port(g->add_output(), "bad_output", onnx::TensorProto::FLOAT, {2});
    auto* initializer = g->add_initializer();
    initializer->set_name("bad_input");
    initializer->set_data_type(onnx::TensorProto::FLOAT);
    initializer->add_dims(2);
    initializer->add_float_data(0.0F);
    initializer->add_float_data(0.0F);
    auto* bad = g->add_node();
    bad->set_op_type("Identity");
    bad->add_input("bad_input");
    bad->add_output("bad_output");
    auto inferenceProbe = proto;
    BOOST_REQUIRE_THROW(onnx::shape_inference::InferShapes(inferenceProbe), std::exception);
    const auto wire = proto.SerializeAsString();
    NativeCanonicalSource source;
    source.modelBytes.assign(wire.begin(), wire.end());
    NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(10),
      [] {}, 1U << 20, 1U << 20};
    const auto inspected = inspectNativeOnnxSourceGraph(source,
      model("qwen", "rank-fixture", digest("rank-graph")), control);
    const auto edge = std::find_if(inspected.graph.edges.begin(), inspected.graph.edges.end(),
      [](const auto& value) { return value.tensor.name == "boundary"; });
    BOOST_REQUIRE(edge != inspected.graph.edges.end());
    BOOST_CHECK_EQUAL(edge->tensor.dtype, "int64");
    const auto expectedRank = mode == "missing" || mode == "known" ? 2U : 0U;
    BOOST_CHECK_MESSAGE(edge->tensor.shape.size() == expectedRank,
                        mode << " rank=" << edge->tensor.shape.size() << " expected=" << expectedRank);
    if (mode == "known" && edge->tensor.shape.size() == 2) {
      BOOST_CHECK_EQUAL(std::get<std::int64_t>(edge->tensor.shape[0]), 1);
      BOOST_CHECK_EQUAL(std::get<std::int64_t>(edge->tensor.shape[1]), 5);
    }
    BOOST_CHECK_EQUAL(std::string(source.modelBytes.begin(), source.modelBytes.end()), wire);
    const auto originalIdentity = canonicalOnnxSourceIdentity(source, control);
    BOOST_CHECK_EQUAL(inspected.canonicalIdentity.graphDigest, originalIdentity.graphDigest);
    BOOST_CHECK_EQUAL(inspected.canonicalIdentity.initializerDigest, originalIdentity.initializerDigest);
  }
}

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
    BOOST_CHECK_EQUAL(candidate.fragmentsByRole.at(roles[i]), digest("qwen-artifact-" + std::to_string(i)));
    BOOST_CHECK(candidate.requirementsByRole.at(roles[i]).backends == std::vector<std::string>({"onnxruntime"}));
    const auto& inputs = candidate.roleStateInputsByRole.at(roles[i]);
    const auto& outputs = candidate.roleStateOutputsByRole.at(roles[i]);
    const std::vector<std::vector<std::string>> suffixes{
      {"key.0", "value.0", "key.1", "value.1"}, {"key.2", "value.2"}, {"key.3", "value.3"}};
    const std::vector<std::string> shape{"batch", "heads", "sequence", "head-dimension"};
    BOOST_REQUIRE_EQUAL(inputs.size(), suffixes[i].size());
    BOOST_REQUIRE_EQUAL(outputs.size(), suffixes[i].size());
    for (std::size_t state = 0; state < suffixes[i].size(); ++state) {
      BOOST_CHECK_EQUAL(inputs[state].name, "past_" + suffixes[i][state]);
      BOOST_CHECK_EQUAL(outputs[state].name, "present_" + suffixes[i][state]);
      BOOST_CHECK_EQUAL(inputs[state].dtype, modelDescriptor.precision);
      BOOST_CHECK_EQUAL(outputs[state].dtype, modelDescriptor.precision);
      BOOST_CHECK(!inputs[state].estimatedBytes && !outputs[state].estimatedBytes);
      BOOST_REQUIRE_EQUAL(inputs[state].shape.size(), shape.size());
      BOOST_REQUIRE_EQUAL(outputs[state].shape.size(), shape.size());
      for (std::size_t axis = 0; axis < shape.size(); ++axis) {
        BOOST_CHECK_EQUAL(std::get<std::string>(inputs[state].shape[axis]), shape[axis]);
        BOOST_CHECK_EQUAL(std::get<std::string>(outputs[state].shape[axis]), shape[axis]);
      }
    }
    const auto& resources = candidate.requirementsByRole.at(roles[i]);
    BOOST_CHECK(!resources.workspaceBytes);
    BOOST_CHECK(!resources.kvBytes);
    BOOST_CHECK(!resources.activationBytes);
    BOOST_CHECK(!resources.transientBytes);
    BOOST_CHECK(!resources.estimatedPeakGpuMemoryBytes());
  }
  for (const auto& name : {"Qwen3-0.6B", "Qwen3.6-27B", "Qwen-configuration-fixture"}) {
    auto renamed = modelDescriptor;
    renamed.modelName = name;
    const auto renamedCandidates = splitter.enumerate(renamed, graphSnapshot,
      NativeCandidateBudget{2, 100, 1});
    BOOST_REQUIRE_EQUAL(renamedCandidates.size(), 1U);
    for (const auto& role : roles) {
      const auto& expected = candidate.roleStateInputsByRole.at(role);
      const auto& actual = renamedCandidates.front().roleStateInputsByRole.at(role);
      BOOST_REQUIRE_EQUAL(expected.size(), actual.size());
      for (std::size_t index = 0; index < actual.size(); ++index) {
        BOOST_CHECK_EQUAL(actual[index].name, expected[index].name);
        BOOST_CHECK_EQUAL(actual[index].dtype, expected[index].dtype);
        BOOST_CHECK(actual[index].shape == expected[index].shape);
      }
    }
  }
  BOOST_CHECK_EQUAL(candidate.crossPartitionTensors.size(), 2U);
  BOOST_CHECK_EQUAL(candidate.tensorDegreesByRole.at(roles[1]), 1U);
  BOOST_CHECK(candidate.inputIngressRole.empty());
  BOOST_CHECK(candidate.resultEgressRole.empty());
  const std::map<std::string, std::string> expectedOwners{
    {"embedding", roles[0]}, {"layer-00", roles[0]}, {"layer-01", roles[0]},
    {"layer-02", roles[1]}, {"layer-03", roles[2]}, {"final-norm-head", roles[2]}};
  BOOST_CHECK(candidate.nodeRoles == expectedOwners);
  BOOST_CHECK_EQUAL(candidate.candidateDigest,
                    splitter.enumerate(modelDescriptor, graphSnapshot, {}).front().candidateDigest);
}

BOOST_AUTO_TEST_CASE(SmolLm2LlamaLayerSplitProducesDynamicPastContract)
{
  const auto graphDigest = digest("smollm2-graph");
  auto graphSnapshot = graph(graphDigest,
    {"embedding", "layer-00", "layer-01", "final-norm-head"});
  auto modelDescriptor = model("llama", "SmolLM2-135M", graphDigest);
  const std::vector<std::string> roles = {
    "/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1"};
  qwen::NativeQwenLayerSplit splitter(
    {{0, 1}, {1, 2}},
    {{roles[0], digest("smollm2-artifact-0")},
     {roles[1], digest("smollm2-artifact-1")}},
    {{roles[0], 1}, {roles[1], 1}}, roles, {1, 1}, {}, {}, "llama");
  const auto candidates = splitter.enumerate(modelDescriptor, graphSnapshot,
                                             NativeCandidateBudget{2, 100, 1});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  const auto& candidate = candidates.front();
  BOOST_CHECK_EQUAL(candidate.executionPlan.modelFamily, "llama");
  BOOST_CHECK_EQUAL(candidate.executionPlan.plannerKind, "native-llama-layer");
  BOOST_REQUIRE_EQUAL(candidate.roleStateInputsByRole.at(roles[0]).size(), 2U);
  BOOST_CHECK_EQUAL(candidate.roleStateInputsByRole.at(roles[0])[0].name, "past_key.0");
  BOOST_CHECK_EQUAL(candidate.roleStateOutputsByRole.at(roles[0])[1].name, "present_value.0");
  BOOST_CHECK_EQUAL(candidate.executionPlan.dependencies.front().topicPrefix,
                    "/NDNSF/DI/LLAMA");
}

BOOST_AUTO_TEST_CASE(SmolLm2LlamaCatalogRoutesToNativePlanner)
{
  // This is a routing fixture, not a real SmolLM inference artifact. Give
  // both decoder roles their own source-defined state boundary.
  onnx::ModelProto nativeSource;
  BOOST_REQUIRE(nativeSource.ParseFromString(fixture::makeQwenNativeConfigOnnxSource()));
  // This routing source otherwise has no weights. Give one owned node a
  // real four-byte scalar so the restored weight-floor negative has an oracle.
  BOOST_REQUIRE_EQUAL(nativeSource.graph().node(1).name(), "layer-00");
  auto* weight = nativeSource.mutable_graph()->add_initializer();
  weight->set_name("route_weight");
  weight->set_data_type(onnx::TensorProto::FLOAT);
  weight->add_float_data(1.0F);
  nativeSource.mutable_graph()->mutable_node(1)->set_op_type("Add");
  nativeSource.mutable_graph()->mutable_node(1)->add_input("route_weight");
  for (const std::string family : {"attention_kv", "recurrent_state", "convolution_state"}) {
    for (const bool output : {false, true}) {
      auto* value = output ? nativeSource.mutable_graph()->add_output()
                           : nativeSource.mutable_graph()->add_input();
      value->set_name("stage0_" + family + (output ? "_out" : "_in"));
      auto* tensor = value->mutable_type()->mutable_tensor_type();
      tensor->set_elem_type(onnx::TensorProto::FLOAT);
      tensor->mutable_shape()->add_dim()->set_dim_value(1);
    }
    auto* node = nativeSource.mutable_graph()->add_node();
    node->set_op_type("Identity");
    node->add_input("stage0_" + family + "_in");
    node->add_output("stage0_" + family + "_out");
  }
  const auto sourceText = nativeSource.SerializeAsString();
  const std::vector<std::uint8_t> sourceBytes(sourceText.begin(), sourceText.end());
  const auto graphDigest = digest("smollm2-catalog-graph");
  auto descriptor = model("llama", "SmolLM2-135M", graphDigest);
  descriptor.sourceRevision = "1";
  const std::vector<std::string> roles{
    "/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1"};
  qwen::NativeQwenLayerSplit planner(
    {{0, 1}, {1, 2}},
    {{roles[0], digest("smollm2-artifact-0")},
     {roles[1], digest("smollm2-artifact-1")}},
    {{roles[0], 1}, {roles[1], 1}}, roles, {1, 1}, {}, {}, "llama");
  const auto graphNodes = NativeJson{"embedding", "layer-00", "layer-01", "final-norm-head"};
  const auto graphEdges = NativeJson{"hidden-embedding-to-layer-00", "hidden-layer-0-to-1",
                                     "hidden-layer-1-to-final"};
  descriptor.graphDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"model", descriptor.modelName}, {"revision", descriptor.sourceRevision},
    {"precision", descriptor.precision}, {"decode_mode", "single-token-autoregressive"},
    {"modality", "text-only"}, {"mtp_enabled", false}, {"thinking_mode", "disabled"},
    {"layer_ranges", NativeJson::array({NativeJson::array({0, 1}), NativeJson::array({1, 2})})},
    {"nodes", graphNodes}, {"edges", graphEdges}, {"legal_cuts", graphEdges}}));
  NativeJson configuration = NativeJson::object();
  configuration["schema"] = "ndnsf-di-native-request-catalog-v1";
  configuration["model"] = nativeParseJson(descriptor.canonicalJson());
  NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(10),
    [] {}, 1024 * 1024, 1024 * 1024};
  const auto sourceIdentity = canonicalOnnxSourceIdentity(
    NativeCanonicalSource{sourceBytes, {}}, control);
  configuration["source"] = NativeJson{
    {"data_name", "/Model/SmolLM2/135M/source"},
    {"digest", nativePlanningDigest(sourceBytes.data(), sourceBytes.size())},
    {"model_manifest_digest", digest("smollm2-manifest")},
    {"canonical_graph_digest", sourceIdentity.graphDigest}};
  configuration["recipe"] = NativeJson{
    {"artifact_profile_digest", digest("smollm2-profile")},
    {"assembler_descriptor_digest", digest("smollm2-assembler")},
    {"backend_abi", "llama-native-onnxruntime-cpu-v1"},
    {"precision", "float32"}, {"quantization", "none"}, {"layout", "NCHW"},
    {"padding", "none"}, {"protection_epoch", "smollm2-epoch"},
    {"max_source_bytes", 1 << 20}, {"max_assembled_bytes", 1 << 20}, {"max_nodes", 16}};
  configuration["publication"] = NativeJson{{"artifact_root", "/Artifact/SmolLM2"}};
  configuration["input_format"] = "OPAQUE";
  configuration["max_payload_bytes"] = 1024;
  configuration["splitter"] = NativeJson{
    {"kind", "LLAMA"}, {"layer_ranges", {{0, 1}, {1, 2}}},
    {"artifact_digests_by_role", {{roles[0], digest("smollm2-artifact-0")},
                                   {roles[1], digest("smollm2-artifact-1")}}},
    {"weight_bytes_by_role", {{roles[0], 1}, {roles[1], 1}}},
    {"resources_by_role", {
      {roles[0], nativeParseJson(NativeRoleResourceRequirement{
        {"onnxruntime"}, 1U << 20, 0, 0, 0, 0, 1.0}.canonicalJson())},
      {roles[1], nativeParseJson(NativeRoleResourceRequirement{
        {"onnxruntime"}, 1U << 20, 0, 0, 0, 0, 1.0}.canonicalJson())}}},
    {"roles", roles}, {"tensor_degrees", {1, 1}},
    {"input_ingress_role", roles[0]}, {"result_egress_role", roles[1]}};
  configuration["node_mapping"] = NativeJson{
    {"embedding", {0}}, {"layer-00", {1, 10, 11, 12}}, {"layer-01", {2}},
    {"final-norm-head", {3, 4, 5, 6, 7, 8, 9}}};
  for (const std::string family : {"attention_kv", "recurrent_state", "convolution_state"}) {
    for (std::size_t stage = 0; stage != 2; ++stage) {
      const auto prefix = stage == 0 ? "stage0_" : "";
      configuration["state_inputs"][roles[stage]][family + "_in"] =
        NativeJson::array({prefix + family + "_in"});
      configuration["state_outputs"][roles[stage]][family + "_out"] =
        NativeJson::array({prefix + family + "_out"});
    }
  }
  NativeCanonicalSource source;
  source.modelBytes = sourceBytes;
  configuration["source"]["file"] = "/absent-preparing-user/model.onnx";
  BOOST_TEST_CHECKPOINT("before Smol catalog load");
  const auto loaded = NativeRequestCatalog::load(configuration.dump(), source, control);
  BOOST_TEST_CHECKPOINT("after Smol catalog load");
  BOOST_REQUIRE(loaded.splitter);
  BOOST_CHECK_EQUAL(loaded.splitter->identity().name, "native-llama-layer-split");
  const auto candidates = loaded.splitter->enumerate(loaded.model.descriptor, loaded.model.graph,
                                                      NativeCandidateBudget{2, 1024, 1});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  BOOST_CHECK_EQUAL(candidates.front().executionPlan.modelFamily, "llama");
  BOOST_CHECK_EQUAL(candidates.front().executionPlan.dependencies.front().topicPrefix,
                    "/NDNSF/DI/LLAMA");
  NativeCanonicalSource reference;
  const auto verifiedSource = loaded.preparation->sourceRefFor(loaded.model.descriptor);
  BOOST_REQUIRE(verifiedSource);
  BOOST_REQUIRE(verifiedSource->materialManifest);
  reference.preparedMetadataJson = verifiedSource->preparedMetadataJson;
  auto referenceMaterials = std::make_shared<NativeCanonicalSource::MaterialManifest>(
    *verifiedSource->materialManifest);
  referenceMaterials->payloads.clear();
  referenceMaterials->payloadsComplete = false;
  referenceMaterials->validate();
  reference.materialManifest = referenceMaterials;
  BOOST_REQUIRE(!reference.preparedMetadataJson.empty());
  // Exercise the production stored-package writer and actual reference-only catalog restore.
  // This is metadata/planner qualification, not a real SmolLM inference result.
  ndn::KeyChain storedKeys("pib-memory:", "tpm-memory:");
  std::map<ndn::Name, std::vector<std::uint8_t>> originalPackets;
  std::map<ndn::Name, std::pair<std::string, std::uint64_t>> committedGroups;
  NativeStoredModelPacketIo storedIo;
  storedIo.requireCurrent = [] {};
  storedIo.commit = [&](auto wires, const std::string& digest) {
    BOOST_REQUIRE(!wires.empty());
    const ndn::Data first{ndn::Block(ndn::span<const std::uint8_t>(wires.front()))};
    std::uint64_t wireBytes = 0;
    for (auto& wire : wires) {
      wireBytes += wire.size();
      const ndn::Data packet{ndn::Block(ndn::span<const std::uint8_t>(wire))};
      const auto prior = originalPackets.find(packet.getName());
      if (prior != originalPackets.end()) BOOST_REQUIRE(prior->second == wire);
      else originalPackets.emplace(packet.getName(), std::move(wire));
    }
    committedGroups[first.getName()] = {digest, wireBytes};
  };
  storedIo.requireCommitted = [&](const ndn::Name& name, const std::string& digest,
                                  std::uint64_t size) {
    BOOST_REQUIRE_EQUAL(committedGroups.at(name).first, digest);
    BOOST_REQUIRE_EQUAL(committedGroups.at(name).second, size);
  };
  std::set<ndn::Name> fetched;
  storedIo.fetch = [&](const std::vector<ndn::Name>& names) {
    NativeStoredModelPacketIo::PacketWires result;
    for (const auto& name : names) {
      fetched.insert(name);
      result.push_back(originalPackets.at(name));
    }
    return result;
  };
  auto storedConfiguration = configuration;
  storedConfiguration["source"]["file"] = "/absent-preparing-user/model.onnx";
  const auto stored = nativePublishStoredPreparedModel(storedKeys, ndn::Name("/model-preparer"),
    ndn::Name("/repo"), ndn::Name("/model-preparer/models").appendVersion(1), loaded,
    nativeCanonicalJson(storedConfiguration), storedIo);
  const auto writerIo = storedIo;
  const auto packetsBeforeBadMapping = originalPackets.size();
  auto badMapping = storedConfiguration;
  badMapping["node_mapping"]["layer-00"] = NativeJson::array({0, 1, 10, 11, 12});
  BOOST_CHECK_THROW(nativePublishStoredPreparedModel(storedKeys, ndn::Name("/model-preparer"),
    ndn::Name("/repo"), ndn::Name("/model-preparer/models").appendVersion(2), loaded,
    nativeCanonicalJson(badMapping), storedIo), std::invalid_argument);
  BOOST_CHECK_EQUAL(originalPackets.size(), packetsBeforeBadMapping);
  auto badRecipe = storedConfiguration;
  badRecipe["recipe"]["padding"] = "foreign-padding";
  BOOST_CHECK_THROW(nativePublishStoredPreparedModel(storedKeys, ndn::Name("/model-preparer"),
    ndn::Name("/repo"), ndn::Name("/model-preparer/models").appendVersion(3), loaded,
    nativeCanonicalJson(badRecipe), storedIo), std::invalid_argument);
  BOOST_CHECK_EQUAL(originalPackets.size(), packetsBeforeBadMapping);
  storedIo.commit = {}; // A is absent; the consumer has no write callback or source callback.
  storedIo.requireCommitted = {};
  const auto reopened = nativeOpenStoredPreparedModel(stored.handle, storedIo, control);
  BOOST_CHECK_EQUAL(reopened.catalog.model.descriptor.modelDigest(), descriptor.modelDigest());
  BOOST_CHECK(!nativeParseJson(reopened.configurationJson).at("source").contains("file"));
  BOOST_REQUIRE(reopened.catalog.planningCache);
  BOOST_CHECK_EQUAL(reopened.catalog.planningCache->candidates.size(), candidates.size());
  BOOST_CHECK(!reopened.publication.ownerRecoverableObjects);
  std::map<std::string, std::string> bundleByPayload;
  for (std::size_t i = 0; i < reopened.publication.materialPayloadIds.size(); ++i)
    bundleByPayload.emplace(reopened.publication.materialPayloadIds[i],
                           reopened.publication.materialDataNames[i]);
  const auto& templateBundle = bundleByPayload.at(referenceMaterials->templatePayloadId);
  std::map<std::uint64_t, std::string> nodeBundles;
  for (const auto& item : referenceMaterials->references) {
    if (item.kind == "graph-node") {
      nodeBundles[item.nodeIndex] = bundleByPayload.at(item.payloadId);
      BOOST_CHECK(nodeBundles.at(item.nodeIndex) != templateBundle);
    }
  }
  BOOST_CHECK(nodeBundles.at(1) == nodeBundles.at(10)); // same semantic layer
  BOOST_CHECK(nodeBundles.at(1) != nodeBundles.at(2));  // another semantic layer
  for (const auto& packetName : fetched)
    BOOST_CHECK(packetName.toUri().find("material-bundle-") == std::string::npos);
  const auto materialName = reopened.objects.objects().begin()->second.object.name;
  BOOST_CHECK(stored.handle.producer.isPrefixOf(materialName));
  auto wrongHandle = stored.handle;
  wrongHandle.masterKey.at(0) ^= 1;
  BOOST_CHECK_THROW(nativeOpenStoredPreparedModel(wrongHandle, storedIo, control), std::exception);
  const auto rejectsMalformedPackage = [&](const auto& mutate) {
    // A fresh isolated publication is internally hash/GCM-valid. Only the semantic package
    // inconsistency should make open fail, rather than stale outer hashes or absent Data.
    std::map<std::string, std::vector<std::uint8_t>> inputs;
    for (const auto& item : reopened.objects.objects())
      inputs[item.first] = nativeReadStoredModelObject(stored.handle, reopened.objects,
                                                     item.first, storedIo);
    auto json = [&](const std::string& id) {
      return nativeParseJson(std::string(inputs.at(id).begin(), inputs.at(id).end()));
    };
    auto material = json("material-manifest");
    auto receipt = json("material-receipt");
    auto businessRoot = json("business-root");
    mutate(material, receipt, businessRoot, inputs);
    auto storeJson = [&](const std::string& id, const NativeJson& value) {
      const auto text = nativeCanonicalJson(value);
      inputs[id] = std::vector<std::uint8_t>(text.begin(), text.end());
      return nativePlanningDigest(text);
    };
    const auto materialDigest = storeJson("material-manifest", material);
    receipt["materialIdentityDigest"] = materialDigest;
    receipt["materialManifestDigest"] = materialDigest;
    const auto receiptDigest = storeJson("material-receipt", receipt);
    auto& facts = businessRoot["metadata"];
    facts["materialIdentityDigest"] = materialDigest;
    facts["materialManifestDigest"] = materialDigest;
    facts["materialManifestBytes"] = inputs.at("material-manifest").size();
    facts["materialReceiptDigest"] = receiptDigest;
    facts["materialReceiptBytes"] = inputs.at("material-receipt").size();
    storeJson("business-root", businessRoot);
    std::map<ndn::Name, std::vector<std::uint8_t>> isolatedPackets;
    auto io = storedIo;
    io.commit = [&](auto wires, const std::string&) {
      for (auto& wire : wires) {
        const ndn::Data packet{ndn::Block(ndn::span<const std::uint8_t>(wire))};
        BOOST_REQUIRE(isolatedPackets.emplace(packet.getName(), std::move(wire)).second);
      }
    };
    io.fetch = [&](const std::vector<ndn::Name>& names) {
      NativeStoredModelPacketIo::PacketWires result;
      for (const auto& name : names) result.push_back(isolatedPackets.at(name));
      return result;
    };
    auto it = inputs.begin();
    const auto broken = nativePublishStoredModel(storedKeys, stored.handle.producer,
      stored.handle.repository, ndn::Name("/model-preparer/models").appendVersion(1),
      reopened.objects.modelDigest(), reopened.objects.catalogId(), "business-root",
      [&]() -> std::optional<NativeStoredModelInput> {
        if (it == inputs.end()) return std::nullopt;
        NativeStoredModelInput value{it->first, it->second};
        ++it;
        return value;
      }, io);
    io.commit = {};
    BOOST_CHECK_THROW(nativeOpenStoredPreparedModel(broken.handle, io, control), std::exception);
  };
  for (const std::string kind : {"graph-template", "graph-node", "shared-initializer"}) {
    rejectsMalformedPackage([&](auto& material, auto& receipt, auto&, auto&) {
      auto& refs = material["references"];
      const auto found = std::find_if(refs.begin(), refs.end(),
        [&](const auto& value) { return value.at("kind") == kind; });
      BOOST_REQUIRE(found != refs.end());
      const auto removedId = found->at("payloadId");
      refs.erase(found);
      auto& rows = receipt["materialObjects"];
      rows.erase(std::remove_if(rows.begin(), rows.end(),
        [&](const auto& row) { return row.at("payloadId") == removedId; }), rows.end());
    });
  }
  rejectsMalformedPackage([](auto&, auto&, auto& root, auto&) {
    root["metadata"]["canonicalSourceBytes"] =
      root["metadata"]["canonicalSourceBytes"].template get<std::uint64_t>() + 1;
  });
  rejectsMalformedPackage([&](auto&, auto& receipt, auto&, auto& inputs) {
    const auto& firstName = nodeBundles.at(1);
    const auto& secondName = nodeBundles.at(2);
    const auto objectId = [&](const std::string& name) {
      const auto found = std::find_if(reopened.objects.objects().begin(),
        reopened.objects.objects().end(), [&](const auto& object) {
          return object.second.object.name == ndn::Name(name);
        });
      BOOST_REQUIRE(found != reopened.objects.objects().end());
      return found->first;
    };
    const auto firstId = objectId(firstName);
    const auto secondId = objectId(secondName);
    const auto offset = inputs.at(firstId).size();
    auto& combined = inputs.at(firstId);
    const auto& second = inputs.at(secondId);
    combined.insert(combined.end(), second.begin(), second.end());
    const auto digest = nativePlanningDigest(combined.data(), combined.size());
    for (auto& row : receipt["materialObjects"]) {
      if (row.at("dataName") != firstName && row.at("dataName") != secondName) continue;
      if (row.at("dataName") == secondName)
        row["bundleOffset"] = row["bundleOffset"].template get<std::uint64_t>() + offset;
      row["dataName"] = firstName;
      row["bundleBytes"] = combined.size();
      row["bundleDigest"] = digest;
    }
    inputs.erase(secondId);
  });
  char preparationDirectory[] = "/var/tmp/spec222-prepared-XXXXXX";
  BOOST_REQUIRE(::mkdtemp(preparationDirectory));
  struct PreparationDirectory
  {
    std::filesystem::path path;
    ~PreparationDirectory()
    {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } privateDirectory{preparationDirectory};
  auto changedConfiguration = storedConfiguration;
  changedConfiguration["max_payload_bytes"] = 2048;
  const auto changedCatalog = NativeRequestCatalog::load(
    nativeCanonicalJson(changedConfiguration), source, control);
  BOOST_CHECK_EQUAL(changedCatalog.model.descriptor.modelDigest(),
                    loaded.model.descriptor.modelDigest());
  for (const bool interrupted : {false, true}) {
    const auto prefix = ndn::Name("/model-preparer/models").appendVersion(interrupted ? 5 : 4);
    const auto state = privateDirectory.path / (interrupted ? "partial" : "ready");
    auto initialIo = writerIo;
    if (interrupted) {
      initialIo.commit = [&](auto wires, const std::string& digest) {
        writerIo.commit(std::move(wires), digest);
        throw std::runtime_error("lost commit reply");
      };
      BOOST_CHECK_THROW(nativePublishStoredPreparedModel(storedKeys, stored.handle.producer,
        stored.handle.repository, prefix, loaded, nativeCanonicalJson(storedConfiguration),
        initialIo, state), std::runtime_error);
    }
    else {
      BOOST_CHECK_NO_THROW(nativePublishStoredPreparedModel(storedKeys, stored.handle.producer,
        stored.handle.repository, prefix, loaded, nativeCanonicalJson(storedConfiguration),
        initialIo, state));
    }
    const auto packetsBeforeMismatch = originalPackets.size();
    BOOST_CHECK_THROW(nativePublishStoredPreparedModel(storedKeys, stored.handle.producer,
      stored.handle.repository, prefix, changedCatalog, nativeCanonicalJson(changedConfiguration),
      writerIo, state), std::exception);
    BOOST_CHECK_EQUAL(originalPackets.size(), packetsBeforeMismatch);
    const auto recovered = nativePublishStoredPreparedModel(storedKeys, stored.handle.producer,
      stored.handle.repository, prefix, loaded, nativeCanonicalJson(storedConfiguration),
      writerIo, state);
    BOOST_CHECK_NO_THROW(nativeOpenStoredPreparedModel(recovered.handle, storedIo, control));
    const auto packetsBeforeReadyHit = originalPackets.size();
    const auto readyHit = nativePublishStoredPreparedModel(storedKeys, stored.handle.producer,
      stored.handle.repository, prefix, loaded, nativeCanonicalJson(storedConfiguration),
      writerIo, state);
    BOOST_CHECK_EQUAL(readyHit.handle.rootWireDigest, recovered.handle.rootWireDigest);
    BOOST_CHECK_EQUAL(readyHit.wireBytes, 0);
    BOOST_CHECK_EQUAL(readyHit.sourceBytes, 0);
    BOOST_CHECK_EQUAL(originalPackets.size(), packetsBeforeReadyHit);
  }
  std::size_t lazySourceCalls = 0;
  const std::function<NativeRequestCatalog()> lazySource = [&] {
    ++lazySourceCalls;
    return loaded;
  };
  const auto lazyState = privateDirectory.path / "lazy-source";
  const auto lazyPublication = nativePublishStoredPreparedModel(storedKeys, stored.handle.producer,
    stored.handle.repository, {}, lazySource, nativeCanonicalJson(storedConfiguration),
    writerIo, lazyState);
  BOOST_CHECK_EQUAL(lazySourceCalls, 1);
  BOOST_CHECK_NO_THROW(nativeOpenStoredPreparedModel(lazyPublication.handle, storedIo, control));
  const std::function<NativeRequestCatalog()> unavailableSource = []() -> NativeRequestCatalog {
    throw std::runtime_error("original source is offline");
  };
  auto changedAdvisory = storedConfiguration;
  changedAdvisory["request_transfer_profiles"] = NativeJson::array();
  const auto writesBeforeLazyHit = originalPackets.size();
  const auto lazyReady = nativePublishStoredPreparedModel(storedKeys, stored.handle.producer,
    stored.handle.repository, {}, unavailableSource, nativeCanonicalJson(changedAdvisory),
    writerIo, lazyState);
  BOOST_CHECK_EQUAL(lazyReady.handle.rootWireDigest, lazyPublication.handle.rootWireDigest);
  BOOST_CHECK_EQUAL(originalPackets.size(), writesBeforeLazyHit);
  BOOST_CHECK_EQUAL(lazyReady.sourceBytes, 0);
  BOOST_CHECK_EQUAL(lazyReady.wireBytes, 0);
  NativeCanonicalCatalogTestAccess::releaseSourceBytes(*loaded.preparation);
  const auto reducedSource = loaded.preparation->sourceRefFor(loaded.model.descriptor);
  BOOST_REQUIRE(reducedSource);
  BOOST_CHECK(reducedSource->modelBytes.empty());
  BOOST_CHECK_EQUAL(reducedSource->preparedMetadataJson, reference.preparedMetadataJson);
  BOOST_CHECK(!verifiedSource->modelBytes.empty());
  NativeCanonicalSource reducedReference;
  reducedReference.preparedMetadataJson = reducedSource->preparedMetadataJson;
  reducedReference.materialManifest = referenceMaterials;
  BOOST_CHECK_NO_THROW(NativeRequestCatalog::load(configuration.dump(), reducedReference, control));
  NativeCanonicalCatalogTestAccess::releaseSourceBytes(*loaded.preparation);
  BOOST_CHECK_EQUAL(loaded.preparation->sourceRefFor(loaded.model.descriptor)->preparedMetadataJson,
                    reference.preparedMetadataJson);
  BOOST_CHECK_NO_THROW(NativeRequestCatalog::load(configuration.dump(), reference, control));
  auto underweight = configuration;
  for (const auto& role : roles)
    underweight["splitter"]["resources_by_role"][role]["weight_bytes"] = 3;
  BOOST_CHECK_THROW(NativeRequestCatalog::load(underweight.dump(), reference, control),
                    std::invalid_argument);
  const auto prepared = nativeParseJson(reference.preparedMetadataJson);
  BOOST_REQUIRE(!prepared.at("sourceGraph").at("initializerConsumers").empty());
  BOOST_CHECK(prepared.at("sourceGraph").at("initializerConsumers").at("route_weight") ==
              NativeJson::array({1}));
  for (int failure = 0; failure < 4; ++failure) {
    auto bad = prepared;
    auto& graph = bad["sourceGraph"];
    if (failure == 0) graph.erase("initializerConsumers");
    if (failure == 1) graph["initializerConsumers"] = NativeJson::object();
    if (failure == 2) graph["initializerConsumers"].begin().value() = {-1};
    if (failure == 3) graph["initializerConsumers"].begin().value() = {1000000};
    auto broken = reference;
    broken.preparedMetadataJson = nativeCanonicalJson(bad);
    BOOST_CHECK_THROW(NativeRequestCatalog::load(configuration.dump(), broken, control),
                      std::invalid_argument);
  }
  for (int failure = 0; failure != 11; ++failure) {
    auto bad = configuration;
    auto& profiles = bad["splitter"]["resources_by_role"];
    auto& profile = profiles[roles.front()];
    if (failure == 0) bad["splitter"].erase("resources_by_role");
    if (failure == 1) profiles["foreign-role"] = profile;
    if (failure == 2) profile.erase("workspace_bytes");
    if (failure == 3) profile["workspace_bytes"] = nullptr;
    if (failure == 4) profile["weight_bytes"] = -1;
    if (failure == 5) profile["weight_bytes"] = true;
    if (failure == 6) profile["weight_bytes"] = 1.5;
    if (failure == 7) {
      profile["weight_bytes"] = std::numeric_limits<std::uint64_t>::max();
      profile["workspace_bytes"] = 1;
    }
    if (failure == 8) profile["backends"] = {"onnxruntime", "onnxruntime"};
    if (failure == 9) profile["backends"] = {"unsupported-backend"};
    if (failure == 10) profile["backends"] = {""};
    if (failure == 7)
      BOOST_CHECK_THROW(NativeRequestCatalog::load(bad.dump(), source, control), std::overflow_error);
    else
      BOOST_CHECK_THROW(NativeRequestCatalog::load(bad.dump(), source, control), std::invalid_argument);
  }
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
    "atomic-v1", 10, {"FullModel"}, {}, "FullModel", "FullModel", "", digest("registered-atomic")};
  yolo::NativeYoloComponentSpec split{
    "split-v1", 1, {"Backbone", "Head"},
    {{"Backbone", {"backbone", "neck"}}, {"Head", {"detect", "output"}}},
    "Backbone", "Head", "NATIVE_POSTPROCESS", digest("registered-split")};
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
    "Backbone", "Head", "", digest("registered-invalid")};
  yolo::NativeYoloComponentSplit bad({invalid});
  BOOST_CHECK_THROW(bad.enumerate(modelDescriptor, graphSnapshot, {}),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(YoloNamespacedTerminalRoleKeepsPostprocessingButAtomicDoesNot)
{
  const auto graphDigest = digest("yolo-namespaced-terminal");
  const auto graphSnapshot = graph(graphDigest, {"backbone", "head"});
  const auto descriptor = model("yolo26n", "YOLO26n", graphDigest);
  const std::string postprocessing = R"({"identity":"YOLO26n-canonical-detection-rows"})";
  const yolo::NativeYoloComponentSpec split{
    "split-v1", 1, {"/BackboneNeck", "/Merge"},
    {{"/BackboneNeck", {"backbone"}}, {"/Merge", {"head"}}},
    "/BackboneNeck", "/Merge", "NATIVE_POSTPROCESS", digest("registered-split")};
  const auto splitCandidate = yolo::NativeYoloComponentSplit({split}, postprocessing)
    .enumerate(descriptor, graphSnapshot, {}).front();
  BOOST_CHECK_EQUAL(splitCandidate.mergeKind, "NATIVE_POSTPROCESS");
  BOOST_CHECK_EQUAL(splitCandidate.postprocessingJson, postprocessing);

  const yolo::NativeYoloComponentSpec noMerge{
    "split-no-merge-v1", 2, {"/BackboneNeck", "/Tail"},
    {{"/BackboneNeck", {"backbone"}}, {"/Tail", {"head"}}},
    "/BackboneNeck", "/Tail", "", digest("registered-no-merge")};
  const auto noMergeCandidate = yolo::NativeYoloComponentSplit({noMerge}, postprocessing)
    .enumerate(descriptor, graphSnapshot, {}).front();
  BOOST_CHECK(noMergeCandidate.mergeKind.empty());
  BOOST_CHECK_EQUAL(noMergeCandidate.postprocessingJson, "{}");

  const yolo::NativeYoloComponentSpec atomic{
    "atomic-v1", 3, {"FullModel"}, {}, "FullModel", "FullModel",
    "NATIVE_POSTPROCESS", digest("registered-atomic")};
  const auto atomicCandidate = yolo::NativeYoloComponentSplit({atomic}, postprocessing)
    .enumerate(descriptor, graphSnapshot, {}).front();
  BOOST_CHECK(atomicCandidate.mergeKind.empty());
  BOOST_CHECK_EQUAL(atomicCandidate.postprocessingJson, "{}");
}

BOOST_AUTO_TEST_CASE(YoloUsesRealBranchTensorsAndKnownByteEstimates)
{
  const auto identity = digest("branch-graph");
  auto snapshot = graph(identity, {"a", "b", "c", "d", "e"});
  const auto descriptor = model("yolo26n", "YOLO26n", identity);
  snapshot.edges = {
    {"x", "a", {"b", "c", "e"}, {"x", "float32", {std::int64_t(2), std::int64_t(2)}, 16}},
    {"y", "b", {"d"}, {"y", "float32", {std::int64_t(2), std::int64_t(4)}, 32}},
    {"latent", "b", {"c"}, {"latent", "float32", {std::string("batch"), std::int64_t(4)}, std::nullopt}}
  };
  snapshot.legalCutEdges = {"x", "y"};
  snapshot.modelInputs = {{"input", "float32", {std::int64_t(-1), std::string("batch")}, std::nullopt}};
  BOOST_CHECK_NO_THROW(snapshot.validate(descriptor));
  BOOST_CHECK_EQUAL(std::get<std::int64_t>(snapshot.modelInputs[0].shape[0]), -1);
  const yolo::NativeYoloComponentSpec partition{"branch", 1, {"Left", "Right", "Merge"},
    {{"Left", {"a"}}, {"Right", {"b", "c"}}, {"Merge", {"d", "e"}}}, "Left", "Merge", "", digest("registered")};
  const yolo::NativeYoloComponentSplit splitter({partition});
  const auto candidate = splitter.enumerate(descriptor, snapshot, {}).front();
  const std::map<std::string, std::string> expectedOwners{
    {"a", "Left"}, {"b", "Right"}, {"c", "Right"}, {"d", "Merge"}, {"e", "Merge"}};
  BOOST_CHECK(candidate.nodeRoles == expectedOwners);
  BOOST_CHECK(candidate.roleStateInputsByRole.empty() && candidate.roleStateOutputsByRole.empty());
  // Python Yolo26Splitter._candidate's actual tensor semantics: x fans out to
  // two roles (b/c share Right), y reaches non-adjacent d, latent stays local.
  std::vector<std::string> actual;
  for (const auto& dependency : candidate.executionPlan.dependencies) {
    BOOST_REQUIRE_EQUAL(dependency.producers.size(), 1);
    BOOST_REQUIRE_EQUAL(dependency.consumers.size(), 1);
    BOOST_REQUIRE_EQUAL(dependency.tensors.size(), 1);
    actual.push_back(dependency.producers[0] + "->" + dependency.consumers[0] + ":" + dependency.tensors[0]);
  }
  const std::vector<std::string> expected{"Left->Merge:x", "Left->Right:x", "Right->Merge:y"};
  BOOST_CHECK(actual == expected);
  BOOST_CHECK(candidate.crossPartitionTensors == std::vector<std::string>({"x", "y"}));
  for (const auto& role : partition.roles) BOOST_CHECK_EQUAL(candidate.requirementsByRole.at(role).weightBytes.value(), 16);
  BOOST_CHECK_EQUAL(std::get<std::string>(snapshot.edges[2].tensor.shape[0]), "batch");
  BOOST_CHECK(!snapshot.edges[2].tensor.estimatedBytes);
  snapshot.legalCutEdges = {"x"};
  BOOST_CHECK_THROW(splitter.enumerate(descriptor, snapshot, {}), std::invalid_argument);
  snapshot.legalCutEdges = {"x", "y"};
  snapshot.edges[0].tensor.estimatedBytes = std::numeric_limits<std::uint64_t>::max();
  BOOST_CHECK_THROW(splitter.enumerate(descriptor, snapshot, {}), std::invalid_argument);
  snapshot.edges.clear(); snapshot.legalCutEdges.clear();
  const auto independent = splitter.enumerate(descriptor, snapshot, {}).front();
  BOOST_CHECK(independent.executionPlan.dependencies.empty());
  BOOST_CHECK(independent.crossPartitionTensors.empty());
  BOOST_CHECK_EQUAL(independent.requirementsByRole.at("Right").weightBytes.value(), 1);
}

BOOST_AUTO_TEST_CASE(GraphRejectsForeignTensorReferencesAndInvalidTopology)
{
  const auto identity = digest("edge-validation");
  const auto original = graph(identity, {"a", "b", "c"});
  const auto descriptor = model("yolo26n", "YOLO26n", identity);
  BOOST_CHECK_NO_THROW(original.validate(descriptor));
  for (unsigned mutation = 0; mutation != 9; ++mutation) {
    auto broken = original;
    if (mutation == 0) broken.edges[0].producer = "foreign";
    if (mutation == 1) broken.edges[0].consumers = {"foreign"};
    if (mutation == 2) broken.edges[0].consumers = {"b", "b"};
    if (mutation == 3) broken.edges[0].consumers.clear();
    if (mutation == 4) { broken.edges[0].producer = "c"; broken.edges[0].consumers = {"a"}; }
    if (mutation == 5) broken.edges.push_back(broken.edges[0]);
    if (mutation == 6) broken.edges[0].tensor.name = "foreign";
    if (mutation == 7) broken.legalCutEdges.push_back("foreign");
    if (mutation == 8) broken.edges[0].tensor.dtype.clear();
    BOOST_CHECK_THROW(broken.validate(descriptor), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(YoloComponentSplitRejectsInvalidComponentsAndForeignModel)
{
  const auto graphDigest = digest("yolo-invalid-graph");
  auto graphSnapshot = graph(graphDigest, {"backbone", "neck", "detect", "output"});
  auto yoloModel = model("yolo26n", "YOLO26n", graphDigest);
  const yolo::NativeYoloComponentSpec atomic{
    "atomic-v1", 10, {"FullModel"}, {}, "FullModel", "FullModel", "", digest("registered-atomic")};

  // Invalid construction: no candidates, duplicate ids, empty roles,
  // duplicate roles, undeclared ingress/egress, malformed candidate digest.
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({}), std::invalid_argument);
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({atomic, atomic}),
                    std::invalid_argument);
  yolo::NativeYoloComponentSpec emptyRoles{
    "e", 1, {}, {}, "FullModel", "FullModel", "", digest("registered")};
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({emptyRoles}),
                    std::invalid_argument);
  yolo::NativeYoloComponentSpec dupRoles{
    "d", 1, {"A", "A"}, {{"A", {"backbone"}}}, "A", "A", "", digest("registered")};
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({dupRoles}),
                    std::invalid_argument);
  yolo::NativeYoloComponentSpec noIngress{
    "n", 1, {"A"}, {{"A", {"backbone"}}}, "", "A", "", digest("registered")};
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
    "u", 1, rolesA, {{"A", {"backbone"}}, {"B", {"neck"}}}, "C", "B", "", digest("registered")};
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({undeclared})
                      .enumerate(yoloModel, graphSnapshot, {}),
                    std::invalid_argument);
  yolo::NativeYoloComponentSpec emptySet{
    "s", 1, rolesA, {{"A", {}}, {"B", {"neck"}}}, "A", "B", "", digest("registered")};
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({emptySet})
                      .enumerate(yoloModel, graphSnapshot, {}),
                    std::invalid_argument);
  yolo::NativeYoloComponentSpec doubleAssign{
    "t", 1, rolesA,
    {{"A", {"backbone", "neck"}}, {"B", {"neck", "detect"}}},
    "A", "B", "", digest("registered")};
  BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({doubleAssign})
                      .enumerate(yoloModel, graphSnapshot, {}),
                    std::invalid_argument);
  yolo::NativeYoloComponentSpec wrongNames{
    "w", 1, rolesA,
    {{"A", {"backbone", "neck"}}, {"B", {"detect", "extra"}}},
    "A", "B", "", digest("registered")};
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
    const auto registered = digest("registered-" + id);
    return yolo::NativeYoloComponentSpec{
      std::move(id), priority, {"FullModel"}, {}, "FullModel", "FullModel", "", registered};
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
  auto candidate = splitter.enumerate(modelDescriptor, graphSnapshot,
                                             NativeCandidateBudget{1, 100, 1}).front();
  fixture::assignPlacementFixtureResourceBudgets(candidate);
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
  inputs.artifacts.sourceByRole = {{role, "/canonical/model"}};
  inputs.artifacts.artifactDigestByRole = {{role, digest("artifact")}};
  inputs.artifacts.manifestDigest = digest("manifest");
  inputs.artifacts.recipeDigest = digest("recipe");
  inputs.artifacts.requestId = snapshot.requestId;
  inputs.artifacts.attempt = snapshot.attempt;
  inputs.artifacts.modelDigest = modelDescriptor.contentDigest;
  inputs.artifacts.graphDigest = graphDigest;
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
  auto candidate = splitter.enumerate(modelDescriptor, graphSnapshot,
                                             NativeCandidateBudget{1, 100, 1}).front();
  fixture::assignPlacementFixtureResourceBudgets(candidate);
  // This fixture pins workspace+activation+transient at ~2.36 GiB after
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
  auto candidate = splitter.enumerate(modelDescriptor, graphSnapshot,
                                             NativeCandidateBudget{1, 100, 1}).front();
  fixture::assignPlacementFixtureResourceBudgets(candidate);
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
  inputs.artifacts.sourceByRole = {{fixture.first, "/canonical/first"},
                                   {fixture.second, "/canonical/second"}};
  inputs.artifacts.artifactDigestByRole = {{fixture.first, digest("first-artifact")},
                                           {fixture.second, digest("second-artifact")}};
  inputs.artifacts.manifestDigest = digest("manifest");
  inputs.artifacts.recipeDigest = digest("recipe");
  inputs.artifacts.requestId = fixture.snapshot.requestId;
  inputs.artifacts.attempt = fixture.snapshot.attempt;
  inputs.artifacts.modelDigest = fixture.snapshot.model.contentDigest;
  inputs.artifacts.graphDigest = fixture.snapshot.graph.graphDigest;
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

BOOST_AUTO_TEST_CASE(PlacementSpreadsWhenPossibleAndCoLocatesWhenNecessary)
{
  TwoRolePlacement fixture;
  for (auto& offer : fixture.snapshot.offers) offer.acceptedRoles = {fixture.first, fixture.second};
  const auto first = NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate);
  BOOST_CHECK_EQUAL(first.assignment.providerByRole.at(fixture.first), "provider-a");
  BOOST_CHECK_EQUAL(first.assignment.providerByRole.at(fixture.second), "provider-b");
  std::reverse(fixture.snapshot.offers.begin(), fixture.snapshot.offers.end());
  std::reverse(fixture.candidate.executionPlan.roles.begin(), fixture.candidate.executionPlan.roles.end());
  fixture.candidate.candidateDigest = fixture.candidate.computedDigest();
  const auto reversed = NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate);
  BOOST_CHECK(first.assignment.providerByRole == reversed.assignment.providerByRole);
  fixture.snapshot.offers.resize(1);
  fixture.snapshot.offers.front().freeBytes = 8ULL << 30;
  const auto colocated = NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate);
  BOOST_CHECK_EQUAL(colocated.assignment.providerByRole.at(fixture.first),
                    fixture.snapshot.offers.front().provider);
  BOOST_CHECK_EQUAL(colocated.assignment.providerByRole.at(fixture.second),
                    fixture.snapshot.offers.front().provider);
  BOOST_CHECK_NO_THROW(colocated.validate(fixture.snapshot, fixture.candidate));
}

BOOST_AUTO_TEST_CASE(PlacementRejectsForgedAssignmentsAndDuplicateOffers)
{
  TwoRolePlacement fixture;
  auto proposal = NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate);
  proposal.assignment.providerByRole[fixture.second] = "provider-a";
  fixture.snapshot.offers.front().acceptedRoles = {fixture.first, fixture.second};
  fixture.snapshot.offers.front().freeBytes = 8ULL << 30;
  BOOST_CHECK_NO_THROW(proposal.validate(fixture.snapshot, fixture.candidate));
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
  fixture.candidate.candidateDigest = fixture.candidate.computedDigest();
  BOOST_CHECK_THROW(NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(CandidateRejectsIncompleteDependenciesAndRankArtifacts)
{
  const TwoRolePlacement fixture;
  BOOST_CHECK_NO_THROW(fixture.candidate.validate(fixture.snapshot.graph));
  for (unsigned mutation = 0; mutation != 16; ++mutation) {
    auto candidate = fixture.candidate;
    const auto& role = fixture.first;
    switch (mutation) {
      case 0: candidate.candidateDigest = "not-a-digest"; break;
      case 1: candidate.fragmentsByRole[role] = "not-a-digest"; break;
      case 2: candidate.inputIngressRole = "foreign"; break;
      case 3: candidate.inputIngressRole = role; candidate.resultEgressRole.clear(); break;
      case 4: candidate.crossPartitionTensors.push_back(candidate.crossPartitionTensors.front()); break;
      case 5: candidate.crossPartitionTensors = {"foreign"}; break;
      case 6: candidate.executionPlan.dependencies.clear(); break;
      case 7: candidate.executionPlan.dependencies.front().tensors = {"foreign"}; break;
      case 8: candidate.executionPlan.dependencies.front().producers = {"foreign"}; break;
      case 9: candidate.executionPlan.dependencies.front().consumers.clear(); break;
      case 10: candidate.rankArtifactDigestsByRole.clear(); break;
      case 11: candidate.tensorDegreesByRole.clear(); break;
      case 12: candidate.tensorDegreesByRole[role] = 0; break;
      case 13: candidate.rankArtifactDigestsByRole[role] = {digest("foreign-artifact")}; break;
      case 14:
        candidate.tensorDegreesByRole[role] = 2;
        candidate.rankArtifactDigestsByRole[role].push_back(candidate.rankArtifactDigestsByRole[role].front());
        break;
      case 15:
        candidate.rankArtifactDigestsByRole.erase(role);
        candidate.rankArtifactDigestsByRole["foreign"] = {digest("foreign-artifact")};
        break;
    }
    BOOST_CHECK_THROW(candidate.validate(fixture.snapshot.graph), std::invalid_argument);
  }
  auto malformed = fixture.snapshot.graph;
  malformed.edges.front().producer = "foreign";
  BOOST_CHECK_THROW(fixture.candidate.validate(malformed), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CandidateRejectsMissingNodeOwnersStateContractsAndRoleCycles)
{
  const TwoRolePlacement fixture;
  for (unsigned mutation = 0; mutation != 10; ++mutation) {
    auto candidate = fixture.candidate;
    switch (mutation) {
      case 0: candidate.nodeRoles.erase("embedding"); break;
      case 1: candidate.nodeRoles["embedding"] = "foreign"; break;
      case 2:
        candidate.nodeRoles.erase("embedding"); candidate.nodeRoles["foreign"] = fixture.first; break;
      case 3:
        for (auto& node : candidate.nodeRoles) node.second = fixture.first;
        break;
      case 4: candidate.roleStateInputsByRole.clear(); break;
      case 5: candidate.roleStateOutputsByRole.erase(fixture.first); break;
      case 6: candidate.roleStateInputsByRole[fixture.first].clear(); break;
      case 7:
        candidate.roleStateInputsByRole[fixture.first].push_back(candidate.roleStateInputsByRole[fixture.first].front());
        break;
      case 8: candidate.roleStateOutputsByRole[fixture.first].front().dtype.clear(); break;
      case 9: {
        auto reverse = candidate.executionPlan.dependencies.front();
        std::swap(reverse.producers, reverse.consumers);
        candidate.executionPlan.dependencies.push_back(reverse);
        break;
      }
    }
    BOOST_CHECK_THROW(candidate.validate(fixture.snapshot.graph), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(CompleteModelDescriptorMatchesMaintainedPythonCanonicalIdentity)
{
  std::ifstream file("tests/fixtures/spec182/model-descriptor-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto cases = NativeJson::parse(file);
  BOOST_REQUIRE_EQUAL(cases.size(), 6);
  std::set<std::string> identities;
  for (const auto& row : cases) {
    const auto& m = row.at("model");
    const auto& a = m.at("adapter");
    NativeAdapterDescriptor adapter;
    adapter.name = a.at("name"); adapter.version = a.at("version");
    adapter.stateDigest = a.at("state_digest"); adapter.abi = a.at("abi");
    adapter.modelFormats = a.at("model_formats").get<std::vector<std::string>>();
    adapter.tasks = a.at("tasks").get<std::vector<std::string>>();
    adapter.backends = a.at("backends").get<std::vector<std::string>>();
    adapter.precisions = a.at("precisions").get<std::vector<std::string>>();
    adapter.inputSchemaDigest = a.at("input_schema_digest");
    adapter.optionsSchemaDigest = a.at("options_schema_digest");
    adapter.resultSchemaDigest = a.at("result_schema_digest");
    adapter.graphSchemaDigest = a.at("graph_schema_digest");
    adapter.splitSchemaDigest = a.at("split_schema_digest");
    adapter.stateSchemaDigest = a.at("state_schema_digest");
    adapter.graphInspectable = a.at("graph_inspectable");
    adapter.splittable = a.at("splittable");
    adapter.deterministicAnalysis = a.at("deterministic_analysis");
    NativeModelDescriptor value{m.at("model_name"), m.at("content_digest"), m.at("semantics_digest"),
      m.at("graph_digest"), m.at("model_format"), m.at("precision"), adapter.name, adapter.version,
      adapter, m.at("source_revision")};
    BOOST_CHECK_EQUAL(adapter.canonicalJson(), row.at("adapter_json").get<std::string>());
    BOOST_CHECK_EQUAL(adapter.descriptorDigest(), row.at("adapter_digest").get<std::string>());
    BOOST_CHECK_EQUAL(value.canonicalJson(), row.at("model_json").get<std::string>());
    BOOST_CHECK_EQUAL(value.modelDigest(), row.at("model_digest").get<std::string>());
    const auto loaded = NativeModelDescriptor::fromCanonicalJson(row.at("model_json").get<std::string>());
    BOOST_CHECK_EQUAL(loaded.modelDigest(), row.at("model_digest").get<std::string>());
    BOOST_CHECK_EQUAL(loaded.adapterId, adapter.name);
    auto unknown = m; unknown["unrecognized_policy"] = true;
    BOOST_CHECK_THROW(NativeModelDescriptor::fromCanonicalJson(nativeCanonicalJson(unknown)), std::invalid_argument);
    unknown = m; unknown["adapter"]["splittable"] = 1;
    BOOST_CHECK_THROW(NativeModelDescriptor::fromCanonicalJson(nativeCanonicalJson(unknown)), std::exception);
    identities.insert(value.modelDigest());
  }
  BOOST_CHECK_EQUAL(identities.size(), cases.size());
  const auto valid = model("fixture", "model", digest("graph"));
  for (unsigned mutation = 0; mutation != 6; ++mutation) {
    auto broken = valid;
    if (mutation == 0) broken.adapter.abi.clear();
    if (mutation == 1) broken.adapter.stateSchemaDigest = "not-a-digest";
    if (mutation == 2) broken.adapter.tasks.clear();
    if (mutation == 3) broken.adapterId = "foreign";
    if (mutation == 4) broken.adapterVersion = "foreign";
    if (mutation == 5) broken.precision = "unsupported";
    BOOST_CHECK_THROW(broken.canonicalJson(), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(YoloFragmentMatchesMaintainedSplitterAndBindsRegistration)
{
  std::ifstream file("tests/fixtures/spec182/yolo-fragment-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto rows = NativeJson::parse(file);
  BOOST_REQUIRE_EQUAL(rows.size(), 2);
  std::set<std::string> fragments;
  for (const auto& row : rows) {
    const auto graphDigest = row.at("graph_digest").get<std::string>();
    auto snapshot = graph(graphDigest, row.at("nodes").get<std::vector<std::string>>());
    snapshot.edges.clear(); snapshot.legalCutEdges.clear();
    const auto descriptor = model("yolo26n", "YOLOFixture", graphDigest);
    yolo::NativeYoloComponentSpec registered{"atomic-v1", 1, {"FullModel"}, {},
      "FullModel", "FullModel", "NATIVE_POSTPROCESS", row.at("registered_digest")};
    const auto candidate = yolo::NativeYoloComponentSplit({registered}).enumerate(descriptor, snapshot, {}).front();
    BOOST_CHECK_EQUAL(candidate.canonicalJson(), row.at("candidate_json").get<std::string>());
    BOOST_CHECK_EQUAL(candidate.candidateDigest, row.at("candidate_digest").get<std::string>());
    BOOST_CHECK_EQUAL(candidate.fragmentsByRole.at("FullModel"), row.at("fragment_digest").get<std::string>());
    BOOST_CHECK(candidate.artifactsByRole.at("FullModel") == std::vector<std::string>({row.at("fragment_digest")}));
    const auto& requirement = candidate.requirementsByRole.at("FullModel");
    BOOST_CHECK(requirement.backends == row.at("backends").get<std::vector<std::string>>());
    BOOST_CHECK_EQUAL(requirement.weightBytes.value(), row.at("weight_bytes").get<std::uint64_t>());
    BOOST_CHECK_EQUAL(requirement.safetyMargin, row.at("safety_margin").get<double>());
    BOOST_CHECK_EQUAL(candidate.mergeKind, row.at("merge_kind").get<std::string>());
    fragments.insert(candidate.fragmentsByRole.at("FullModel"));
    registered.candidateDigest.clear();
    BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit({registered}), std::invalid_argument);
  }
  // Both rows have the same candidate ID, role and graph. Registration changes
  // must change the fragment identity independently of these shared fields.
  BOOST_CHECK_EQUAL(fragments.size(), 2);
}

BOOST_AUTO_TEST_CASE(ResourceBudgetsMatchMaintainedPythonContract)
{
  std::ifstream input("tests/fixtures/spec182/resource-budget-oracle.json");
  BOOST_REQUIRE(input.good());
  const auto rows = NativeJson::parse(input);
  BOOST_REQUIRE_EQUAL(rows.size(), 12);
  for (const auto& row : rows) {
    BOOST_TEST_CONTEXT(row.at("name").get<std::string>()) {
      const auto& value = row.at("input");
      const auto bytes = [&](const char* field) -> std::optional<std::uint64_t> {
        if (value.at(field).is_null()) return std::nullopt;
        return value.at(field).get<std::uint64_t>();
      };
      NativeRoleResourceRequirement budget{
        value.at("backends").get<std::vector<std::string>>(), bytes("weight_bytes"),
        bytes("workspace_bytes"), bytes("kv_bytes"), bytes("activation_bytes"),
        bytes("transient_bytes"), value.at("safety_margin").get<double>()};
      BOOST_CHECK_EQUAL(budget.canonicalJson(), row.at("canonical_json").get<std::string>());
      if (row.at("native_overflow").get<bool>()) {
        BOOST_CHECK_THROW(budget.estimatedPeakGpuMemoryBytes(), std::overflow_error);
      }
      else if (row.at("peak").is_null()) {
        BOOST_CHECK(!budget.estimatedPeakGpuMemoryBytes());
      }
      else {
        BOOST_CHECK_EQUAL(budget.estimatedPeakGpuMemoryBytes().value(), row.at("peak").get<std::uint64_t>());
      }
    }
  }
  NativeRoleResourceRequirement defaults;
  defaults.backends = {"onnxruntime"};
  BOOST_CHECK_EQUAL(defaults.safetyMargin, 1.1);
  BOOST_CHECK(!defaults.estimatedPeakGpuMemoryBytes());
}

BOOST_AUTO_TEST_CASE(PlacementAccountsForKvAndRejectsUnknownPeak)
{
  TwoRolePlacement fixture;
  auto& budget = fixture.candidate.requirementsByRole.at(fixture.first);
  budget = {{"onnxruntime"}, 1, 0, 0, 0, 0, 1.0};
  fixture.candidate.candidateDigest = fixture.candidate.computedDigest();
  fixture.snapshot.offers.front().freeBytes = 1;
  BOOST_CHECK_NO_THROW(NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate));
  budget.kvBytes = 1;
  fixture.candidate.candidateDigest = fixture.candidate.computedDigest();
  BOOST_CHECK_THROW(NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate), std::runtime_error);
  fixture.snapshot.offers.front().freeBytes = 2;
  BOOST_CHECK_NO_THROW(NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate));
  budget.kvBytes.reset();
  fixture.candidate.candidateDigest = fixture.candidate.computedDigest();
  BOOST_CHECK_THROW(NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(CompleteCandidateIdentityMatchesMaintainedPython)
{
  std::ifstream file("tests/fixtures/spec182/candidate-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto rows = NativeJson::parse(file);
  BOOST_REQUIRE_EQUAL(rows.size(), 9);
  for (const auto& row : rows) {
    BOOST_TEST_CONTEXT(row.at("name").get<std::string>()) {
      auto candidate = fixture::candidateFromJson(row.at("input"));
      candidate.candidateDigest = row.at("candidate_digest");
      auto snapshot = graph(candidate.graphDigest, {"z-node", "a-node"});
      snapshot.edges.front().id = "hidden"; snapshot.edges.front().tensor.name = "hidden";
      snapshot.legalCutEdges = {"hidden"};
      if (row.at("name") == "qwen-default") {
        snapshot = graph(candidate.graphDigest, {"embedding", "layer-00", "layer-01", "final-norm-head"});
        qwen::NativeQwenLayerSplit splitter({{0, 1}, {1, 2}},
          {{"front", digest("qwen-front")}, {"end", digest("qwen-end")}},
          {{"front", 1}, {"end", 1}}, {"front", "end"}, {1, 1});
        auto actual = splitter.enumerate(candidate.model, snapshot, {}).front();
        checkQwenPlannerMigration(actual,
          nativeParseJson(row.at("canonical_json").get<std::string>()), snapshot);
      }
      BOOST_CHECK_NO_THROW(candidate.validate(snapshot));
      BOOST_CHECK_EQUAL(candidate.canonicalJson(), row.at("canonical_json").get<std::string>());
      BOOST_CHECK_EQUAL(candidate.computedDigest(), candidate.candidateDigest);
      candidate.candidateDigest = digest("old-subset-identity");
      BOOST_CHECK_THROW(candidate.validate(snapshot), std::invalid_argument);
    }
  }
}

BOOST_AUTO_TEST_CASE(CandidateIdentityRejectsStaleDigestForValidContractChanges)
{
  std::ifstream file("tests/fixtures/spec182/candidate-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto row = NativeJson::parse(file).at(0);
  auto base = fixture::candidateFromJson(row.at("input"));
  base.candidateDigest = row.at("candidate_digest");
  auto snapshot = graph(base.graphDigest, {"z-node", "a-node"});
  snapshot.edges.front().id = "hidden"; snapshot.edges.front().tensor.name = "hidden";
  snapshot.legalCutEdges = {"hidden"};
  BOOST_REQUIRE_NO_THROW(base.validate(snapshot));
  for (unsigned mutation = 0; mutation < 20; ++mutation) {
    auto c = base;
    switch (mutation) {
      case 0: c.source = "GENERATED"; break;
      case 1: c.splitter.name += "-other"; break;
      case 2: c.splitter.version = "2"; break;
      case 3: c.splitter.configurationDigest = digest("other-state"); break;
      case 4: c.splitter.deterministic = false; break;
      case 5: c.model.sourceRevision = "revision-2"; break;
      case 6: c.fragmentsByRole.at("front") = digest("other-fragment"); break;
      case 7: c.artifactsByRole.at("front").push_back(digest("extra-artifact")); break;
      case 8: c.requirementsByRole.at("front").kvBytes = 123; break;
      case 9: c.requirementsByRole.at("front").weightBytes.reset(); break;
      case 10: c.estimatedCosts["unknown"] = 2.5; break;
      case 11: c.selectionPriority = 6; break;
      case 12: c.postprocessingJson = "{\"threshold\":0.5}"; break;
      case 13: c.roleStateInputsByRole.at("front").front().shape[0] = std::string("batch2"); break;
      case 14: c.roleStateOutputsByRole.at("end").front().estimatedBytes = 64; break;
      case 15: c.mergeKind = "ONNX_MERGE_GRAPH"; break;
      case 16: std::reverse(c.executionPlan.roles.begin(), c.executionPlan.roles.end()); break;
      case 17: c.tensorDegreesByRole.clear(); c.rankArtifactDigestsByRole.clear(); break;
      case 18: c.inputIngressRole.clear(); c.resultEgressRole.clear(); break;
      case 19: c.hybridPlan = NativeHybridPlan{2, {1, 1}, {"S0R0", "S1R0"}, {}}; break;
    }
    BOOST_TEST_CONTEXT(mutation) {
      BOOST_CHECK(c.computedDigest() != base.candidateDigest);
      BOOST_CHECK_THROW(c.validate(snapshot), std::invalid_argument);
      c.candidateDigest = c.computedDigest();
      BOOST_CHECK_NO_THROW(c.validate(snapshot));
    }
  }
  for (const auto* malformed : {"[]", "{\"a\":1,\"a\":2}", "{\"x\":NaN}"}) {
    auto c = base; c.postprocessingJson = malformed;
    BOOST_CHECK_THROW(c.computedDigest(), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(HybridCandidateRejectsIncompleteRankAndRedistributionContracts)
{
  std::ifstream file("tests/fixtures/spec182/candidate-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto row = NativeJson::parse(file).at(1);
  auto base = fixture::candidateFromJson(row.at("input"));
  base.candidateDigest = row.at("candidate_digest");
  BOOST_REQUIRE(base.hybridPlan);
  for (unsigned mutation = 0; mutation < 14; ++mutation) {
    auto h = *base.hybridPlan;
    switch (mutation) {
      case 0: h.stages = 1; break;
      case 1: h.tensorDegrees = {1, 0}; break;
      case 2: h.rankLabels[1] = "S0R0"; break;
      case 3: h.redistributions.clear(); break;
      case 4: h.redistributions.push_back(h.redistributions.front()); break;
      case 5: h.redistributions.front().consumerRanks = {1}; break;
      case 6: h.redistributions.front().consumerRanks = {1, 3}; break;
      case 7: h.redistributions.front().producerRanks = {1}; break;
      case 8: h.redistributions.front().operation = "GATHER"; break;
      case 9: h.redistributions.front().completeOutput = false; break;
      case 10: h.redistributions.front().axis = 16; break;
      case 11: h.redistributions.front().epoch.clear(); break;
      case 12: h.redistributions.front().integrityDigest = "bad"; break;
      case 13: h.redistributions.front().consumerRanks = {1, 1}; break;
    }
    BOOST_CHECK_THROW(h.validate(), std::invalid_argument);
  }
  auto snapshot = graph(base.graphDigest, {"z-node", "a-node"});
  snapshot.edges.front().id = "hidden"; snapshot.edges.front().tensor.name = "hidden";
  snapshot.legalCutEdges = {"hidden"};
  base.hybridPlan.reset();
  base.candidateDigest = base.computedDigest();
  BOOST_CHECK_THROW(base.validate(snapshot), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(QwenMetadataBuildsMaintainedGraphAndCandidate)
{
  std::ifstream file("tests/fixtures/spec182/qwen-metadata-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto rows = NativeJson::parse(file);
  BOOST_REQUIRE_EQUAL(rows.size(), 2);
  for (const auto& row : rows) {
    std::vector<qwen::NativeQwenLayerSplit::LayerRange> ranges;
    for (const auto& range : row.at("ranges"))
      ranges.emplace_back(range.at(0).get<std::uint64_t>(), range.at(1).get<std::uint64_t>());
    qwen::NativeQwenLayerSplit splitter(ranges,
      {{"front", digest("qwen-front")}, {"end", digest("qwen-end")}},
      {{"front", 1}, {"end", 1}}, {"front", "end"}, {1, 1});
    const auto& expected = row.at("graph");
    auto descriptor = model("qwen", "QwenFixture", expected.at("graph_digest").get<std::string>());
    const auto inspected = splitter.inspectGraph(descriptor, "pinned-r1", 100);
    BOOST_CHECK(inspected.topologicalOrder == expected.at("topological_order").get<std::vector<std::string>>());
    BOOST_CHECK(inspected.legalCutEdges == expected.at("legal_cut_edges").get<std::vector<std::string>>());
    BOOST_REQUIRE_EQUAL(inspected.nodes.size(), expected.at("nodes").size());
    for (std::size_t i = 0; i < inspected.nodes.size(); ++i)
      BOOST_CHECK_EQUAL(inspected.nodes[i].opType, expected.at("nodes").at(i).at("operation").get<std::string>());
    BOOST_REQUIRE_EQUAL(inspected.modelInputs.size(), 1);
    BOOST_REQUIRE_EQUAL(inspected.modelOutputs.size(), 1);
    BOOST_CHECK_EQUAL(inspected.modelInputs.front().name, "input_ids");
    BOOST_CHECK_EQUAL(inspected.modelOutputs.front().name, "logits");
    const auto tensorJson = [](const NativeTensorContract& tensor) {
      auto shape = NativeJson::array();
      for (const auto& dim : tensor.shape) std::visit([&](const auto& v) { shape.push_back(v); }, dim);
      return NativeJson{{"name", tensor.name}, {"dtype", tensor.dtype}, {"shape", shape},
        {"estimated_bytes", tensor.estimatedBytes ? NativeJson(*tensor.estimatedBytes) : NativeJson(nullptr)}};
    };
    BOOST_CHECK(tensorJson(inspected.modelInputs.front()) == expected.at("model_inputs").at(0));
    BOOST_CHECK(tensorJson(inspected.modelOutputs.front()) == expected.at("model_outputs").at(0));
    BOOST_REQUIRE_EQUAL(inspected.edges.size(), expected.at("edges").size());
    for (std::size_t i = 0; i < inspected.edges.size(); ++i) {
      const auto& edge = inspected.edges[i];
      auto value = tensorJson(edge.tensor);
      value.erase("name");
      value["edge_id"] = edge.id; value["producer"] = edge.producer; value["consumers"] = edge.consumers;
      BOOST_CHECK(value == expected.at("edges").at(i));
    }
    auto candidates = splitter.enumerateFromMetadata(descriptor, "pinned-r1", 100, {});
    BOOST_REQUIRE_EQUAL(candidates.size(), 1);
    const auto frozen = nativeParseJson(row.at("candidate_json").get<std::string>());
    BOOST_CHECK_EQUAL(fixture::candidateFromJson(frozen).computedDigest(),
                      row.at("candidate_digest").get<std::string>());
    checkQwenPlannerMigration(candidates.front(), frozen, inspected);
    BOOST_CHECK_THROW(splitter.inspectGraph(descriptor, "foreign", 100), std::invalid_argument);
    BOOST_CHECK_THROW(splitter.inspectGraph(descriptor, "", 100), std::invalid_argument);
    BOOST_CHECK_THROW(splitter.inspectGraph(descriptor, "pinned-r1", inspected.nodes.size() - 1), std::invalid_argument);
    BOOST_CHECK_THROW(splitter.enumerateFromMetadata(descriptor, "pinned-r1", 100, {0, 100, 1}), std::invalid_argument);
    descriptor.graphDigest = digest("foreign-graph");
    BOOST_CHECK_THROW(splitter.inspectGraph(descriptor, "pinned-r1", 100), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(YoloCatalogConsumesActualOnnxSemanticPartition)
{
  std::ifstream file("tests/fixtures/spec182/yolo-semantic-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto row = NativeJson::parse(file);
  NativeCanonicalSource source;
  const auto hex = row.at("model_hex").get<std::string>();
  for (std::size_t i = 0; i < hex.size(); i += 2)
    source.modelBytes.push_back(std::stoul(hex.substr(i, 2), nullptr, 16));
  auto descriptor = model("yolo26n", "YOLOFixture", row.at("graph_digest").get<std::string>());
  NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(30),
    [] {}, 1024 * 1024, 1024 * 1024};
  yolo::NativeYoloComponentSpec component{"semantic-v1", 1, {"Front", "Branch", "Merge"}, {},
    "Front", "Merge", "NATIVE_POSTPROCESS", row.at("registered_digest").get<std::string>()};
  const auto partition = row.at("partition");
  auto splitter = yolo::NativeYoloComponentSplit::fromOnnxCatalog(descriptor, source, control,
    {{component, nativeCanonicalJson(partition)}});
  const auto inspected = inspectNativeOnnxPlanningGraph(source, descriptor, control);
  const auto actual = splitter.enumerate(descriptor, inspected.graph, {});
  BOOST_REQUIRE_EQUAL(actual.size(), 1);
  BOOST_CHECK(actual.front().canonicalJson() == row.at("candidate_json").get<std::string>());
  BOOST_CHECK_EQUAL(actual.front().computedDigest(), row.at("candidate_digest").get<std::string>());
  const auto expectedOwners = row.at("node_roles").get<std::map<std::string, std::string>>();
  BOOST_CHECK(actual.front().nodeRoles == expectedOwners);
  // A caller's reconstructed graph cannot replace source-derived facts.
  auto supplied = inspected.graph;
  supplied.nodes.clear(); supplied.edges.clear();
  BOOST_CHECK_EQUAL(splitter.enumerate(descriptor, supplied, {}).front().candidateDigest,
                    actual.front().candidateDigest);
  auto foreign = descriptor; foreign.sourceRevision = "foreign";
  BOOST_CHECK_THROW(splitter.enumerate(foreign, inspected.graph, {}), std::invalid_argument);
  supplied.graphDigest = digest("foreign");
  BOOST_CHECK_THROW(splitter.enumerate(descriptor, supplied, {}), std::invalid_argument);
  for (int mutation = 0; mutation < 9; ++mutation) {
    auto broken = partition;
    switch (mutation) {
      case 0: broken["roleNodeSets"]["Front"][0] = "onnx-node-0"; break;
      case 1: broken["tensorInterfaces"][0]["producerNode"] = "foreign"; break;
      case 2: broken["tensorInterfaces"][0]["consumerRoles"] = NativeJson::array({"Front"}); break;
      case 3: broken["tensorInterfaces"][0]["dtype"] = "int64"; break;
      case 4: broken["tensorInterfaces"][0]["shape"] = NativeJson::array({99}); break;
      case 5: broken["dependencyEdges"] = NativeJson::array(); break;
      case 6: broken["safeCuts"] = NativeJson::array(); break;
      case 7: broken["roleInterfaces"]["Front"]["inputs"] = NativeJson::array(); break;
      case 8: broken["roleInterfaces"]["Merge"]["outputs"][0]["name"] = "foreign"; break;
    }
    BOOST_TEST_CONTEXT("semantic mutation " << mutation) {
      BOOST_CHECK_THROW(yolo::NativeYoloComponentSplit::fromOnnxCatalog(descriptor, source, control,
        {{component, nativeCanonicalJson(broken)}}), std::invalid_argument);
    }
  }
}

BOOST_AUTO_TEST_CASE(RequestCatalogLoadsPinnedSourceAndRejectsConfigurationDrift)
{
  std::ifstream file("tests/fixtures/spec182/yolo-semantic-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto oracle = NativeJson::parse(file);
  NativeCanonicalSource source;
  const auto hex = oracle.at("model_hex").get<std::string>();
  for (std::size_t i = 0; i < hex.size(); i += 2)
    source.modelBytes.push_back(std::stoul(hex.substr(i, 2), nullptr, 16));
  const auto descriptor = model("yolo26n", "YOLOFixture", oracle.at("graph_digest").get<std::string>());
  NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(30),
    [] {}, 1024 * 1024, 1024 * 1024};
  const auto identity = canonicalOnnxSourceIdentity(source, control);
  const NativeJson configuration{
    {"schema", "ndnsf-di-native-request-catalog-v1"},
    {"model", nativeParseJson(descriptor.canonicalJson())},
    {"source", {{"data_name", "/fixture/source"},
      {"digest", nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size())},
      {"model_manifest_digest", digest("fixture-manifest")}, {"canonical_graph_digest", identity.graphDigest}}},
    {"recipe", {{"artifact_profile_digest", digest("fixture-profile")},
      {"assembler_descriptor_digest", digest("fixture-assembler-v1")}, {"backend_abi", "fixture-abi"},
      {"precision", descriptor.precision}, {"quantization", "none"}, {"layout", "NCHW"},
      {"padding", "none"}, {"protection_epoch", "fixture-epoch"}, {"max_source_bytes", 1048576},
      {"max_assembled_bytes", 1048576}, {"max_nodes", 100}}},
    {"publication", {{"artifact_root", "/fixture/artifacts"}}},
    {"input_format", "JSON"}, {"max_payload_bytes", 32},
    {"splitter", {{"kind", "YOLO"}, {"components", NativeJson::array({{
      {"candidate_id", "semantic-v1"}, {"priority", 1}, {"roles", {"Front", "Branch", "Merge"}},
      {"node_names_by_role", NativeJson::object()}, {"input_ingress_role", "Front"},
      {"result_egress_role", "Merge"}, {"merge_kind", "NATIVE_POSTPROCESS"},
      {"candidate_digest", oracle.at("registered_digest")}, {"semantic_partition", oracle.at("partition")}
    }})}}}};
  const auto loaded = NativeRequestCatalog::load(nativeCanonicalJson(configuration), source, control);
  auto profiledConfiguration = configuration;
  const auto transferProfile = NativeJson{{"schema", "ndnsf-di-request-transfer-profile-v1"},
    {"source_digest", configuration.at("source").at("digest")},
    {"input_digest", digest("initial-input")}, {"initial_context", true},
    {"dependencies", NativeJson::array()}};
  profiledConfiguration["request_transfer_profiles"] = NativeJson::array({transferProfile});
  const auto profiledCatalog = NativeRequestCatalog::load(
    nativeCanonicalJson(profiledConfiguration), source, control);
  BOOST_REQUIRE(profiledCatalog.planningCache);
  BOOST_CHECK(profiledCatalog.planningCache->requestTransferProfiles ==
    NativeJson::array({transferProfile}));
  profiledConfiguration["request_transfer_profiles"][0]["source_digest"] = digest("foreign-source");
  BOOST_CHECK_THROW(NativeRequestCatalog::load(
    nativeCanonicalJson(profiledConfiguration), source, control), std::invalid_argument);
  BOOST_REQUIRE(loaded.preparation);
  BOOST_REQUIRE(loaded.splitter);
  BOOST_CHECK(loaded.preparation->adapters()->frozen());
  const auto candidates = loaded.splitter->enumerate(loaded.model.descriptor, loaded.model.graph, {});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1);
  BOOST_CHECK_EQUAL(candidates.front().canonicalJson(), oracle.at("candidate_json").get<std::string>());
  const auto adapter = loaded.preparation->adapters()->find(descriptor.adapterId);
  BOOST_REQUIRE(adapter);
  const std::vector<std::uint8_t> payload{'{', '}'};
  BOOST_CHECK(adapter->encodeInput(payload) == payload);
  BOOST_CHECK_EQUAL(adapter->inspect(descriptor.modelName, descriptor.contentDigest).canonicalJson(), descriptor.canonicalJson());
  BOOST_CHECK_THROW(adapter->encodeInput(std::vector<std::uint8_t>{'{'}), std::exception);
  BOOST_CHECK_THROW(adapter->encodeInput(std::vector<std::uint8_t>(33, ' ')), std::exception);
  for (int mutation = 0; mutation != 7; ++mutation) {
    auto broken = configuration;
    switch (mutation) {
      case 0: broken["source"]["digest"] = digest("foreign-source"); break;
      case 1: broken["source"]["canonical_graph_digest"] = digest("foreign-graph"); break;
      case 2: broken["source"]["initializer_digest"] = digest("missing-initializers"); break;
      case 3: broken["publication"]["package_manifest_digest"] = digest("foreign-package"); break;
      case 4: broken["input_format"] = "foreign-format"; break;
      case 5: broken["splitter"]["kind"] = "foreign-splitter"; break;
      case 6: broken["model"]["unknown_field"] = true; break;
    }
    BOOST_TEST_CONTEXT("request catalog mutation " << mutation) {
      BOOST_CHECK_THROW(NativeRequestCatalog::load(nativeCanonicalJson(broken), source, control), std::invalid_argument);
    }
  }
  auto corrupt = source;
  corrupt.modelBytes.front() ^= 1;
  BOOST_CHECK_THROW(NativeRequestCatalog::load(nativeCanonicalJson(configuration), corrupt, control), std::invalid_argument);
  auto cancelled = control;
  cancelled.requireActive = [] { throw std::runtime_error("cancelled"); };
  BOOST_CHECK_THROW(NativeRequestCatalog::load(nativeCanonicalJson(configuration), source, cancelled), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(NativeRequestRuntimeLoadsPinnedPolicyAndRejectsDrift)
{
  std::ifstream file("tests/fixtures/spec182/yolo-semantic-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto oracle = NativeJson::parse(file);
  NativeCanonicalSource source;
  const auto hex = oracle.at("model_hex").get<std::string>();
  for (std::size_t i = 0; i < hex.size(); i += 2)
    source.modelBytes.push_back(std::stoul(hex.substr(i, 2), nullptr, 16));
  const auto descriptor = model("yolo26n", "YOLOFixture", oracle.at("graph_digest").get<std::string>());
  NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(30),
    [] {}, 1024 * 1024, 1024 * 1024};
  const auto identity = canonicalOnnxSourceIdentity(source, control);
  const NativeJson catalogConfiguration{
    {"schema", "ndnsf-di-native-request-catalog-v1"},
    {"model", nativeParseJson(descriptor.canonicalJson())},
    {"source", {{"data_name", "/fixture/source"},
      {"digest", nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size())},
      {"model_manifest_digest", digest("fixture-manifest")}, {"canonical_graph_digest", identity.graphDigest}}},
    {"recipe", {{"artifact_profile_digest", digest("fixture-profile")},
      {"assembler_descriptor_digest", digest("fixture-assembler-v1")}, {"backend_abi", "fixture-abi"},
      {"precision", descriptor.precision}, {"quantization", "none"}, {"layout", "NCHW"},
      {"padding", "none"}, {"protection_epoch", "fixture-epoch"}, {"max_source_bytes", 1048576},
      {"max_assembled_bytes", 1048576}, {"max_nodes", 100}}},
    {"publication", {{"artifact_root", "/fixture/artifacts"}}},
    {"input_format", "JSON"}, {"max_payload_bytes", 32},
    {"splitter", {{"kind", "YOLO"}, {"components", NativeJson::array({{
      {"candidate_id", "semantic-v1"}, {"priority", 1}, {"roles", {"Front", "Branch", "Merge"}},
      {"node_names_by_role", NativeJson::object()}, {"input_ingress_role", "Front"},
      {"result_egress_role", "Merge"}, {"merge_kind", "NATIVE_POSTPROCESS"},
      {"candidate_digest", oracle.at("registered_digest")}, {"semantic_partition", oracle.at("partition")}
    }})}}}};
  const auto catalog = NativeRequestCatalog::load(nativeCanonicalJson(catalogConfiguration), source, control);

  const auto key = [] (char seed) {
    const std::string bytes(32, seed);
    return std::shared_ptr<EVP_PKEY>(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
      reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()), EVP_PKEY_free);
  };
  NativeGrantIssuerConfig issuer;
  issuer.authorityIdentity = "/authority";
  issuer.requesterIdentity = "/requester";
  issuer.protectionEpoch = "fixture-epoch";
  issuer.keyId = "fixture-key";
  issuer.authorityPrivateKey = key('a');
  issuer.requesterPublicKey = key('b');
  issuer.allowedModelManifests = {digest("fixture-manifest")};
  issuer.contentKey = [] (const auto&, const auto&) { return std::vector<std::uint8_t>(32, 42); };
  std::string authorityPublic(32, '\0');
  std::size_t authorityPublicSize = authorityPublic.size();
  BOOST_REQUIRE_EQUAL(EVP_PKEY_get_raw_public_key(issuer.authorityPrivateKey.get(),
    reinterpret_cast<unsigned char*>(authorityPublic.data()), &authorityPublicSize), 1);
  const auto grants = std::make_shared<NativeAuthenticatedGrantClient>(
    "/requester", key('b'), "/authority", authorityPublic,
    std::make_shared<NativeArtifactGrantIssuer>(issuer),
    [] (const auto& name, const auto&, const auto&) { return name; });

  const NativeJson runtimeConfiguration{
    {"schema", "ndnsf-di-native-request-runtime-v1"},
    {"contract", {{"service_name", "/service"}, {"task_name", "task"},
      {"adapter_name", descriptor.adapterId},
      {"adapter_descriptor_digest", descriptor.adapter.descriptorDigest()},
      {"adapter_composition_digest", digest("composition")},
      {"task_descriptor_digest", digest("task")}, {"generation_mode", "TOKEN_STREAMING"},
      {"tokenizer_digest", digest("tokenizer")}}},
    {"requester_identity", "/requester"}, {"protection_epoch", "fixture-epoch"},
    {"input_layout_digest", digest("layout")},
    {"security", {{"policy_digest", digest("policy")}, {"require_protected_artifacts", true}}},
    {"budget", {{"max_candidates", 1}, {"max_policy_ms", 100}, {"max_reentries", 1}}},
    {"state_mapping", {{"inputs", NativeJson::object()}, {"outputs", NativeJson::object()}}},
    {"no_progress_ms", 5000}, {"max_segments", 4096}};
  const auto runtime = nativeRequestRuntimeFromJson(
    nativeCanonicalJson(runtimeConfiguration), catalog, grants);
  BOOST_CHECK_EQUAL(runtime.contract.adapterName, descriptor.adapterId);
  BOOST_CHECK_EQUAL(runtime.contract.tokenizerDigest, digest("tokenizer"));
  BOOST_CHECK_EQUAL(runtime.requesterIdentity, "/requester");
  BOOST_CHECK_EQUAL(runtime.catalog.get(), catalog.preparation.get());
  BOOST_CHECK_EQUAL(runtime.grants.get(), grants.get());

  for (const auto& mutation : std::vector<std::function<void(NativeJson&)>>{
         [] (auto& value) { value["unknown"] = true; },
         [] (auto& value) { value["contract"]["adapter_name"] = "foreign"; },
         [] (auto& value) { value["requester_identity"] = "/foreign"; },
         [] (auto& value) { value["security"]["require_protected_artifacts"] = false; },
         [] (auto& value) { value["budget"]["max_candidates"] = 0; },
         [] (auto& value) { value["state_mapping"]["inputs"] = {{"role", {{"state", {"x"}}}}}; },
         [] (auto& value) { value["contract"]["service_name"] = 7; },
         [] (auto& value) { value["security"]["require_protected_artifacts"] = "true"; },
         [] (auto& value) { value["contract"]["generation_mode"] = "UNSUPPORTED"; },
         [] (auto& value) { value["contract"]["tokenizer_digest"] = ""; },
         [] (auto& value) { value["protection_epoch"] = "foreign-epoch"; }}) {
    auto broken = runtimeConfiguration;
    mutation(broken);
    BOOST_CHECK_THROW(nativeRequestRuntimeFromJson(nativeCanonicalJson(broken), catalog, grants),
                      std::invalid_argument);
  }
  BOOST_CHECK_THROW(nativeRequestRuntimeFromJson(nativeCanonicalJson(runtimeConfiguration), catalog, {}),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OwnedOnnxGraphMatchesMaintainedPlanningAndCanonicalIdentities)
{
  std::ifstream file("tests/fixtures/spec182/onnx-planning-graph-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto rows = NativeJson::parse(file);
  BOOST_REQUIRE_EQUAL(rows.size(), 6);
  const auto tensorJson = [](const NativeTensorContract& tensor) {
    auto shape = NativeJson::array();
    for (const auto& dim : tensor.shape) std::visit([&](const auto& v) { shape.push_back(v); }, dim);
    return NativeJson{{"name", tensor.name}, {"dtype", tensor.dtype}, {"shape", shape},
      {"estimated_bytes", tensor.estimatedBytes ? NativeJson(*tensor.estimatedBytes) : NativeJson(nullptr)}};
  };
  for (const auto& row : rows) {
    BOOST_TEST_CONTEXT(row.at("name").get<std::string>()) {
      NativeCanonicalSource source;
      const auto hex = row.at("model_hex").get<std::string>();
      for (std::size_t i = 0; i < hex.size(); i += 2)
        source.modelBytes.push_back(std::stoul(hex.substr(i, 2), nullptr, 16));
      const auto& expected = row.at("snapshot");
      const auto descriptor = model("fixture", "OnnxFixture", expected.at("graph_digest"));
      NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1 << 20, 1 << 20};
      const auto actual = inspectNativeOnnxPlanningGraph(source, descriptor, control);
      BOOST_CHECK_EQUAL(actual.graph.graphDigest, expected.at("graph_digest").get<std::string>());
      BOOST_CHECK_EQUAL(actual.graphMetadataJson, row.at("metadata_json").get<std::string>());
      BOOST_CHECK_EQUAL(actual.canonicalIdentity.graphDigest, row.at("canonical_graph_digest").get<std::string>());
      BOOST_CHECK_EQUAL(actual.canonicalIdentity.initializerDigest, row.at("initializer_digest").get<std::string>());
      BOOST_CHECK(actual.canonicalIdentity.graphDigest != actual.graph.graphDigest);
      BOOST_CHECK(actual.graph.topologicalOrder == expected.at("topological_order").get<std::vector<std::string>>());
      BOOST_CHECK(actual.graph.legalCutEdges == expected.at("legal_cut_edges").get<std::vector<std::string>>());
      BOOST_REQUIRE_EQUAL(actual.graph.nodes.size(), expected.at("nodes").size());
      const auto metadata = nativeParseJson(row.at("metadata_json"));
      for (std::size_t i = 0; i < actual.graph.nodes.size(); ++i) {
        const auto& node = actual.graph.nodes[i];
        BOOST_CHECK_EQUAL(node.id, expected.at("nodes")[i].at("node_id").get<std::string>());
        BOOST_CHECK_EQUAL(node.opType, expected.at("nodes")[i].at("operation").get<std::string>());
        BOOST_CHECK_EQUAL(node.ordinal, i);
        BOOST_CHECK_EQUAL(actual.nodeNames[i], metadata.at("nodes")[i].at("name").get<std::string>());
        BOOST_CHECK_EQUAL(actual.canonicalNodeIndices.at(node.id), i);
      }
      BOOST_REQUIRE_EQUAL(actual.graph.edges.size(), expected.at("edges").size());
      for (std::size_t i = 0; i < actual.graph.edges.size(); ++i) {
        const auto& edge = actual.graph.edges[i]; const auto& want = expected.at("edges")[i];
        BOOST_CHECK_EQUAL(edge.id, want.at("edge_id").get<std::string>());
        BOOST_CHECK_EQUAL(edge.producer, want.at("producer").get<std::string>());
        BOOST_CHECK(edge.consumers == want.at("consumers").get<std::vector<std::string>>());
        auto contract = tensorJson(edge.tensor);
        BOOST_CHECK_EQUAL(contract.at("name").get<std::string>(), edge.id);
        for (const auto* key : {"dtype", "shape", "estimated_bytes"}) BOOST_CHECK(contract.at(key) == want.at(key));
      }
      auto inputs = NativeJson::array(), outputs = NativeJson::array();
      for (const auto& tensor : actual.graph.modelInputs) inputs.push_back(tensorJson(tensor));
      for (const auto& tensor : actual.graph.modelOutputs) outputs.push_back(tensorJson(tensor));
      BOOST_CHECK(inputs == expected.at("model_inputs"));
      BOOST_CHECK(outputs == expected.at("model_outputs"));
    }
  }
}

BOOST_AUTO_TEST_CASE(OwnedOnnxGraphRejectsForeignAdapterSourceAndExpiredControl)
{
  std::ifstream file("tests/fixtures/spec182/onnx-planning-graph-oracle.json");
  BOOST_REQUIRE(file.good());
  const auto rows = NativeJson::parse(file);
  const auto sourceFor = [](const NativeJson& row) {
    NativeCanonicalSource source;
    const auto hex = row.at("model_hex").get<std::string>();
    for (std::size_t i = 0; i < hex.size(); i += 2)
      source.modelBytes.push_back(std::stoul(hex.substr(i, 2), nullptr, 16));
    return source;
  };
  const auto source = sourceFor(rows[0]);
  const auto descriptor = model("fixture", "OnnxFixture", rows[0].at("snapshot").at("graph_digest"));
  NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(10), [] {}, 1 << 20, 1 << 20};
  auto graph = inspectNativeOnnxPlanningGraph(source, descriptor, control).graph;
  auto foreign = descriptor; foreign.adapter.abi = "different-abi";
  BOOST_CHECK_THROW(inspectNativeOnnxPlanningGraph(source, foreign, control), std::invalid_argument);
  foreign = descriptor; foreign.graphDigest = digest("foreign-graph");
  BOOST_CHECK_THROW(inspectNativeOnnxPlanningGraph(source, foreign, control), std::invalid_argument);
  BOOST_CHECK_THROW(inspectNativeOnnxPlanningGraph(sourceFor(rows[1]), descriptor, control), std::invalid_argument);
  BOOST_CHECK_THROW(inspectNativeOnnxPlanningGraph(NativeCanonicalSource{{255, 255}, {}}, descriptor, control), std::runtime_error);
  graph.nodes.front().opType.clear();
  BOOST_CHECK_THROW(graph.validate(descriptor), std::invalid_argument);
  auto limited = control; limited.maxSourceBytes = 1;
  BOOST_CHECK_THROW(inspectNativeOnnxPlanningGraph(source, descriptor, limited), std::runtime_error);
  limited = control; limited.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  BOOST_CHECK_THROW(inspectNativeOnnxPlanningGraph(source, descriptor, limited), std::runtime_error);
  limited = control; limited.requireActive = {};
  BOOST_CHECK_THROW(inspectNativeOnnxPlanningGraph(source, descriptor, limited), std::runtime_error);
  limited = control; limited.requireActive = [] { throw std::runtime_error("fixture-cancelled"); };
  BOOST_CHECK_EXCEPTION(inspectNativeOnnxPlanningGraph(source, descriptor, limited), std::runtime_error,
    [](const auto& error) { return std::string(error.what()) == "fixture-cancelled"; });
}

BOOST_AUTO_TEST_SUITE_END()
