#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "tests/fixtures/test-resource-paths.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "tests/fixtures/spec182/native-catalog-test-access.hpp"
#include "tests/fixtures/spec182/native-qwen-config.hpp"
#include "tests/fixtures/spec182/native-candidate-json-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <fstream>
#include <cstdio>
#include "tests/fixtures/spec182/native-sealing-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeRequestOptionsTransport.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-preparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeSourcePartitionStrategy.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModel.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include <onnxruntime_cxx_api.h>
#include <onnx/checker.h>
#include <onnx/defs/tensor_proto_util.h>
#include <array>
#include <map>
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <boost/test/unit_test.hpp>
#include <onnx/shape_inference/implementation.h>

#include <chrono>
#include <cmath>
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
  BOOST_TEST_CONTEXT("candidate field migration: " << NativeJson::diff(frozenFields, currentFields)) {
    BOOST_CHECK(currentFields == frozenFields);
  }
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
  // An omitted prefix lets the journal allocate the N4 package root from the verified facts.
  const auto semanticRoot = std::filesystem::temp_directory_path() /
    ("ndnsf-semantic-root-" + std::to_string(::getpid()));
  std::filesystem::remove_all(semanticRoot);
  std::filesystem::create_directories(semanticRoot);
  std::filesystem::permissions(semanticRoot, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  const auto semanticState = semanticRoot / "package.state";
  struct SemanticStateCleanup {
    std::filesystem::path path;
    ~SemanticStateCleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
  } semanticCleanup{semanticRoot};
  const auto semanticStored = nativePublishStoredPreparedModel(storedKeys, ndn::Name("/model-preparer"),
    ndn::Name("/repo"), ndn::Name{}, loaded, nativeCanonicalJson(storedConfiguration), storedIo,
    semanticState);
  const auto packagePrefix = semanticStored.handle.rootName.getPrefix(-1);
  BOOST_REQUIRE_GE(packagePrefix.size(), 3U);
  BOOST_CHECK(packagePrefix[-3] == ndn::name::Component("stored"));
  BOOST_CHECK(packagePrefix[-2] == ndn::name::Component("rep"));
  BOOST_CHECK_EQUAL(packagePrefix[-1].value_size(), 32U);
  const auto semanticResumed = nativePublishStoredPreparedModel(storedKeys, ndn::Name("/model-preparer"),
    ndn::Name("/repo"), ndn::Name{}, loaded, nativeCanonicalJson(storedConfiguration), storedIo,
    semanticState);
  BOOST_CHECK(semanticResumed.handle.rootName.getPrefix(-1) == packagePrefix);

  // Persistence: a fresh read-only catalogue load from the same stored packets must recover the package by its
  // recorded root and index names.
  auto readOnly = storedIo;
  readOnly.commit = {};
  const auto reloaded = nativeLoadStoredModelCatalog(semanticResumed.handle, readOnly);
  BOOST_CHECK_EQUAL(reloaded.catalogId(), "catalog-" + nativePlanningDigest(
    nativeStoredModelConfigurationIdentity(nativeCanonicalJson(storedConfiguration))).substr(7));
  BOOST_CHECK(reloaded.manifestId() == "business-root");
  BOOST_REQUIRE(!reloaded.objects().empty());
  for (const auto& item : reloaded.objects()) {
    const auto expected = nativeStoredModelObjectName(packagePrefix, item.first);
    BOOST_CHECK(item.second.object.name == expected);
    BOOST_CHECK(expected.getPrefix(packagePrefix.size()) == packagePrefix);
  }

  // A package published under an explicit prefix keeps the legacy object names, and still reloads by them.
  {
    auto legacyRead = storedIo;
    legacyRead.commit = {};
    const auto legacyCatalog = nativeLoadStoredModelCatalog(stored.handle, legacyRead);
    const auto legacyPrefix = stored.handle.rootName.getPrefix(-1);
    BOOST_CHECK(!nativeStoredModelSemanticPrefix(legacyPrefix));
    for (const auto& item : legacyCatalog.objects()) {
      BOOST_CHECK(item.second.object.name ==
                  ndn::Name(legacyPrefix).append("objects").append(item.first).appendVersion(0));
    }
  }

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
  const auto scratch = ndnsf::test::makeTestScratchTemplate("spec222-prepared");
  char preparationDirectory[4096];
  std::snprintf(preparationDirectory, sizeof(preparationDirectory), "%s",
                scratch.c_str());
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
    const auto state = privateDirectory.path / (interrupted ? "partial" : "ready");
    auto initialIo = writerIo;
    if (interrupted) {
      initialIo.commit = [&](auto wires, const std::string& digest) {
        writerIo.commit(std::move(wires), digest);
        throw std::runtime_error("lost commit reply");
      };
      BOOST_REQUIRE_EXCEPTION(nativePublishStoredPreparedModel(storedKeys, stored.handle.producer,
        stored.handle.repository, {}, loaded, nativeCanonicalJson(storedConfiguration),
        initialIo, state), std::runtime_error, [] (const std::runtime_error& error) {
          return std::string(error.what()) == "lost commit reply";
        });
    }
    else {
      BOOST_CHECK_NO_THROW(nativePublishStoredPreparedModel(storedKeys, stored.handle.producer,
        stored.handle.repository, {}, loaded, nativeCanonicalJson(storedConfiguration),
        initialIo, state));
    }
    const auto packetsBeforeMismatch = originalPackets.size();
    BOOST_CHECK_THROW(nativePublishStoredPreparedModel(storedKeys, stored.handle.producer,
      stored.handle.repository, {}, changedCatalog, nativeCanonicalJson(changedConfiguration),
      writerIo, state), std::exception);
    BOOST_CHECK_EQUAL(originalPackets.size(), packetsBeforeMismatch);
    const auto recovered = nativePublishStoredPreparedModel(storedKeys, stored.handle.producer,
      stored.handle.repository, {}, loaded, nativeCanonicalJson(storedConfiguration),
      writerIo, state);
    BOOST_CHECK(nativeStoredModelSemanticPrefix(recovered.handle.rootName.getPrefix(-1)));
    BOOST_CHECK_NO_THROW(nativeOpenStoredPreparedModel(recovered.handle, storedIo, control));
    const auto packetsBeforeReadyHit = originalPackets.size();
    const auto readyHit = nativePublishStoredPreparedModel(storedKeys, stored.handle.producer,
      stored.handle.repository, {}, loaded, nativeCanonicalJson(storedConfiguration),
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
  std::ifstream file(ndnsf::test::testFixturePath("spec182/model-descriptor-oracle.json").string());
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
  std::ifstream file(ndnsf::test::testFixturePath("spec182/yolo-fragment-oracle.json").string());
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
  std::ifstream input(ndnsf::test::testFixturePath("spec182/resource-budget-oracle.json").string());
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
  std::ifstream file(ndnsf::test::testFixturePath("spec182/candidate-oracle.json").string());
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
  std::ifstream file(ndnsf::test::testFixturePath("spec182/candidate-oracle.json").string());
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
  std::ifstream file(ndnsf::test::testFixturePath("spec182/candidate-oracle.json").string());
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
  std::ifstream file(ndnsf::test::testFixturePath("spec182/qwen-metadata-oracle.json").string());
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
  std::ifstream file(ndnsf::test::testFixturePath("spec182/yolo-semantic-oracle.json").string());
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
  std::ifstream file(ndnsf::test::testFixturePath("spec182/yolo-semantic-oracle.json").string());
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
    // Spec182 compares against the historical YOLO route on purpose.
    {"legacy_yolo_retired", false},
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
  BOOST_REQUIRE(loaded.planningCache);
  BOOST_REQUIRE_EQUAL(loaded.planningCache->candidates.size(), 1U);
  const auto& preparedRoles = loaded.planningCache->candidates.front().roles;
  BOOST_REQUIRE_EQUAL(preparedRoles.size(), 3U);
  BOOST_CHECK_EQUAL(preparedRoles[0].artifactCoordinate,
                    "/graph/semantic/component/Front/entry/region/profile/whole");
  BOOST_CHECK_EQUAL(preparedRoles[1].artifactCoordinate,
                    "/graph/semantic/component/Branch/entry/region/profile/whole");
  BOOST_CHECK(preparedRoles[2].artifactCoordinate.empty());
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
  BOOST_CHECK_EQUAL(loaded.preparation->maxPayloadBytesFor(descriptor), 32U);
  auto foreignDescriptor = descriptor;
  foreignDescriptor.adapterVersion += "-unregistered";
  BOOST_CHECK_THROW(loaded.preparation->maxPayloadBytesFor(foreignDescriptor),
                    std::invalid_argument);
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
  std::ifstream file(ndnsf::test::testFixturePath("spec182/yolo-semantic-oracle.json").string());
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
    // Spec182 compares against the historical YOLO route on purpose.
    {"legacy_yolo_retired", false},
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
  std::ifstream file(ndnsf::test::testFixturePath("spec182/onnx-planning-graph-oracle.json").string());
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
  std::ifstream file(ndnsf::test::testFixturePath("spec182/onnx-planning-graph-oracle.json").string());
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

BOOST_AUTO_TEST_CASE(AutomaticSeedSealsEveryRoleAndChangesOnlyCurrentCoreIdentity)
{
  // Structural sealing fixture; actual source proof remains the ONNX adapter's owner.
  TwoRolePlacement fixture;
  const auto proposal = NativePreSplitFirstPlacement().propose(fixture.snapshot, fixture.candidate);
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
  inputs.requesterIdentity = "/requester"; inputs.protectionEpoch = "protected-v1";
  inputs.expiresAtMs = 2000000000000ULL;
  inputs.requestContractDigest = digest("request-wire");
  ndnsf::di::fixture::assemblies(inputs);
  NativeAutomaticShapeSeed seed;
  seed.contractDigest = digest("source-contract"); seed.bindingDigest = digest("binding");
  seed.budgetDigest = digest("budget"); seed.envelopeDigest = digest("envelope");
  seed.maxContext = 16; seed.inputShapes.emplace("graph/x", std::vector<std::int64_t>{1, 4});
  inputs.automaticShapeSeed = seed;
  for (auto& item : inputs.assemblyByRole) {
    item.second.automaticContractDigest = seed.contractDigest;
    item.second.recipeDigest = nativePlanningDigest(canonicalNativeOnnxRecipeJson(item.second));
  }
  const auto core = NativePlanSealer::sealCore(fixture.snapshot, proposal, inputs);
  BOOST_REQUIRE(core.automaticShapeSeed);
  BOOST_CHECK_EQUAL(core.automaticShapeSeed->envelopeDigest, seed.envelopeDigest);
  const NativeSecurityPolicySnapshot security{digest("automatic-policy"), true};
  std::vector<NativeGrantBinding> grants;
  for (const auto& offer : fixture.snapshot.offers) {
    const auto view = NativePlanSealer::grantView(core, offer, security);
    grants.push_back({view.provider, view.role, "/grant/automatic-" + std::to_string(grants.size()),
                      digest("grant-" + view.role), view.provider});
  }
  const auto sealed = NativePlanSealer::finalizeSecurity(core, grants, security);
  for (const auto& offer : fixture.snapshot.offers) {
    const auto projected = NativePlanSealer::project(sealed, offer.provider,
      ndnsf::di::fixture::projection(sealed, offer.provider));
    BOOST_REQUIRE(projected.automaticShapeSeed);
    BOOST_CHECK_EQUAL(projected.automaticShapeSeed->envelopeDigest, seed.envelopeDigest);
    BOOST_CHECK_EQUAL(projected.selectedRoleContract().automaticContractDigest, seed.contractDigest);
  }
  auto nextInputs = inputs;
  nextInputs.automaticShapeSeed->maxContext = 32;
  nextInputs.automaticShapeSeed->budgetDigest = digest("next-budget");
  nextInputs.automaticShapeSeed->envelopeDigest = digest("next-envelope");
  const auto next = NativePlanSealer::sealCore(fixture.snapshot, proposal, nextInputs);
  BOOST_CHECK_NE(core.coreDigest, next.coreDigest);
  const auto fixed = NativePlanSealer::shareFixedCore(core);
  const auto warm = NativePlanSealer::shareFixedCore(next, fixed.fixedCoreOwner);
  BOOST_CHECK_EQUAL(warm.fixedCoreOwner.get(), fixed.fixedCoreOwner.get());
  BOOST_CHECK_EQUAL(warm.automaticShapeSeed->envelopeDigest, digest("next-envelope"));
  for (int failure = 0; failure != 4; ++failure) {
    auto changed = inputs;
    if (failure == 0) changed.automaticShapeSeed.reset();
    if (failure == 1) changed.assemblyByRole.begin()->second.automaticContractDigest = digest("foreign");
    if (failure == 2) changed.requestContractDigest.clear();
    if (failure == 3) changed.automaticShapeSeed->maxNewTokens = 1;
    BOOST_CHECK_THROW(NativePlanSealer::sealCore(fixture.snapshot, proposal, changed), std::exception);
  }
}

BOOST_AUTO_TEST_CASE(ModelPathFactsComeFromVerifiedFactsOnly)
{
  NativeModelDescriptor descriptor;
  descriptor.modelName = "qwen3-0.6b";
  descriptor.sourceRevision = "r3";
  auto facts = nativeModelPathFacts(descriptor, "qwen3", "int4");
  BOOST_CHECK_EQUAL(facts.family, "qwen3");
  BOOST_CHECK_EQUAL(facts.size, "0.6b");
  BOOST_CHECK_EQUAL(facts.quantization, "int4");
  BOOST_CHECK_EQUAL(facts.revisionLabel, "r3");

  // A family that does not prefix the label cannot supply a size: it stays explicit instead of being guessed.
  auto unknownFamily = nativeModelPathFacts(descriptor, "llama", "");
  BOOST_CHECK_EQUAL(unknownFamily.size, "unspecified");
  BOOST_CHECK_EQUAL(unknownFamily.quantization, "unspecified");

  // No family, no revision and no quantization remain explicit words.
  NativeModelDescriptor bare;
  bare.modelName = "some-model";
  const auto bareFacts = nativeModelPathFacts(bare, "", "");
  BOOST_CHECK_EQUAL(bareFacts.family, "unspecified");
  BOOST_CHECK_EQUAL(bareFacts.size, "unspecified");
  BOOST_CHECK_EQUAL(bareFacts.revisionLabel, "unspecified");

  // The plan-side overload produces the same facts when no descriptor is in hand.
  const auto fromPlan = nativeModelPathFacts("qwen3-0.6b", "r3", "qwen3", "int4");
  BOOST_CHECK_EQUAL(fromPlan.family, "qwen3");
  BOOST_CHECK_EQUAL(fromPlan.size, "0.6b");
  BOOST_CHECK_EQUAL(fromPlan.quantization, "int4");
  BOOST_CHECK_EQUAL(fromPlan.revisionLabel, "r3");
  BOOST_CHECK_EQUAL(nativeModelPathFacts("", "", "generic-onnx", "").size, "unspecified");

  descriptor.modelName = "Qwen/Qwen3-0.6B";
  descriptor.precision = "float32";
  descriptor.quantizationSubtype = "weight_only_int4";
  const auto stored = nativeModelPathFacts(descriptor, "qwen", {});
  BOOST_CHECK_EQUAL(stored.family, "qwen");
  BOOST_CHECK_EQUAL(stored.size, "0.6b");
  BOOST_CHECK_EQUAL(stored.quantization, "int4");
  BOOST_CHECK_EQUAL(nativeModelPathFacts(descriptor, "qwen", "weight_only_int4").quantization,
                    "int4");
  BOOST_CHECK_EQUAL(nativeModelPathFacts(descriptor, "llama", {}).size, "unspecified");

  // A fact that is not an atom is refused rather than escaped.
  NativeModelDescriptor hostile;
  hostile.modelName = "qwen3-0.6b";
  BOOST_CHECK_THROW(nativeModelPathFacts(hostile, "qwen3", "int4/e4m3"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(InlineGenerationOptionsRemainInRequestAndOutOfAckDiscovery)
{
  const auto descriptor = model("qwen", "Qwen/Qwen3-0.6B", digest("graph"));
  NativeRequestContract contract{"/di/generation", "task", descriptor.adapterId,
    descriptor.adapter.descriptorDigest(), digest("composition"), digest("task")};
  contract.generationMode = "TOKEN_STREAMING";
  contract.tokenizerDigest = digest("tokenizer");
  NativeApplicationInput input;
  input.taskName = contract.taskName;
  input.inputSchemaDigest = descriptor.adapter.inputSchemaDigest;
  input.optionsSchemaDigest = descriptor.adapter.optionsSchemaDigest;
  input.payload = {1, 2, 3};
  NativeJson stateMap = NativeJson::object();
  for (int layer = 0; layer < 56; ++layer) {
    const auto suffix = std::to_string(layer);
    stateMap["present_key_values." + suffix] = "past_key_values." + suffix;
  }
  const auto options = nativeCanonicalJson(NativeJson{
    {"stateInputNames", stateMap}, {"stateSuccessorMap", stateMap}, {"maxNewTokens", 32}});
  input.options.assign(options.begin(), options.end());

  const auto encoded = detail::encodeNativeRequestEnvelopeForRuntime(
    descriptor, input, contract, "/request/large-options", 1, 10000, std::nullopt,
    {}, std::nullopt, std::nullopt, {}, "INLINE", {});
  const auto request = encodeNativeRequestEnvelope(
    descriptor, input, contract, "/request/large-options", 1, 10000);
  BOOST_CHECK(encoded.wire == request.wire);
  BOOST_CHECK_EQUAL(encoded.requestContractDigest, request.requestContractDigest);
  BOOST_CHECK_GT(encoded.wire.size(), 4096U);
  BOOST_REQUIRE(encoded.discoveryPayload);
  BOOST_CHECK_LE(encoded.discoveryPayload->size(), 4096U);
  const std::string discovery(encoded.discoveryPayload->begin(), encoded.discoveryPayload->end());
  const auto offer = parseNativeRequestOffer(discovery);
  BOOST_REQUIRE(offer);
  BOOST_CHECK(offer->at("options_payload_b64") == "");
  BOOST_CHECK(offer->at("input_payload_b64") == "");
  BOOST_CHECK(offer->at("input_manifest_digest") == encoded.inputManifestDigest);
  BOOST_CHECK(!detail::requiresSelectionGenerationOptions(discovery, false));
}

BOOST_AUTO_TEST_CASE(SourcePartitionStrategyBuildsFullAndComponentPlans)
{
  const auto graphDigest = digest("generic-source-partition");
  auto sourceGraph = graph(graphDigest, {"input", "layer-00", "layer-01", "head"});
  auto sourceModel = model("onnx", "GenericSource", graphDigest);

  NativeSourcePartitionStrategy full({NativeSourcePartitionMode::FULL, {}, {"FullModel"},
                                      "FullModel", "FullModel", "{}"});
  auto fullCandidates = full.enumerate(sourceModel, sourceGraph, {});
  BOOST_REQUIRE_EQUAL(fullCandidates.size(), 1U);
  BOOST_CHECK(fullCandidates.front().nodeRoles.size() == sourceGraph.nodes.size());
  BOOST_CHECK_NO_THROW(fullCandidates.front().validate(sourceGraph));

  NativeSourcePartitionStrategy component({NativeSourcePartitionMode::COMPONENT, "layer-01",
                                           {"Prefix", "Suffix"}, "Prefix", "Suffix", "{}"});
  auto componentCandidates = component.enumerate(sourceModel, sourceGraph, {});
  BOOST_REQUIRE_EQUAL(componentCandidates.size(), 1U);
  const auto& candidate = componentCandidates.front();
  BOOST_CHECK_EQUAL(candidate.nodeRoles.at("input"), "Prefix");
  BOOST_CHECK_EQUAL(candidate.nodeRoles.at("layer-00"), "Prefix");
  BOOST_CHECK_EQUAL(candidate.nodeRoles.at("layer-01"), "Suffix");
  BOOST_CHECK_EQUAL(candidate.nodeRoles.at("head"), "Suffix");
  BOOST_CHECK_EQUAL(candidate.crossPartitionTensors.size(), 1U);
  BOOST_CHECK_NO_THROW(candidate.validate(sourceGraph));

  NativeSourcePartitionStrategy bad({NativeSourcePartitionMode::COMPONENT, "missing",
                                     {"Prefix", "Suffix"}, "Prefix", "Suffix", "{}"});
  BOOST_CHECK_THROW(bad.enumerate(sourceModel, sourceGraph, {}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SourcePartitionStrategyBuildsHybridGeometry)
{
  const auto graphDigest = digest("generic-source-hybrid");
  auto sourceGraph = graph(graphDigest, {"input", "pre", "conv", "post", "head"});
  sourceGraph.nodes[2].opType = "Conv";
  sourceGraph.nodes[3].opType = "Add";
  auto sourceModel = model("onnx", "GenericHybrid", graphDigest);
  NativeSourcePartitionProfile hybridProfile{NativeSourcePartitionMode::HYBRID, "conv",
    {"Prefix", "Rank0", "Rank1", "Suffix"}, "Prefix", "Suffix", "{}", 16, "input-channel",
    {1, 16, 4, 4}, {1, 32, 4, 4}, "float32", ""};
  hybridProfile.convInputTensor = "scaled";
  hybridProfile.convWeightInitializer = "w";
  hybridProfile.convOutputTensor = "conv_out";
  hybridProfile.convBiasInitializer = "b_conv";
  NativeSourcePartitionStrategy hybrid(hybridProfile);
  auto candidates = hybrid.enumerate(sourceModel, sourceGraph, {});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  const auto& candidate = candidates.front();
  BOOST_REQUIRE_EQUAL(candidate.executionPlan.stageLayout.size(), 3U);
  BOOST_CHECK_EQUAL(candidate.executionPlan.stageLayout[1].degree(), 2U);
  BOOST_REQUIRE_EQUAL(candidate.tensorIdentityByRole.size(), 2U);
  BOOST_CHECK_EQUAL(candidate.tensorIdentityByRole.at("Rank0").rank, 0U);
  BOOST_CHECK_EQUAL(candidate.tensorIdentityByRole.at("Rank1").rank, 1U);
  BOOST_CHECK_EQUAL(candidate.executionPlan.steps.size(), 5U);
  BOOST_CHECK(std::any_of(candidate.executionPlan.steps.begin(), candidate.executionPlan.steps.end(),
    [] (const auto& step) { return step.operation == "SUM"; }));
  BOOST_CHECK_NO_THROW(candidate.validate(sourceGraph));
  BOOST_REQUIRE_EQUAL(candidate.sourceTransformByRole.size(), 3U);
  for (const auto& role : {"Rank0", "Rank1"}) {
    const auto transform = nativeParseJson(candidate.sourceTransformByRole.at(role));
    BOOST_CHECK_EQUAL(transform.at("kind").get<std::string>(), "conv-input-channel-partial");
    BOOST_CHECK_EQUAL(transform.at("extent").get<std::uint64_t>(), 16U);
  }
  const auto suffixTransform = nativeParseJson(candidate.sourceTransformByRole.at("Suffix"));
  BOOST_CHECK_EQUAL(suffixTransform.at("kind").get<std::string>(), "add-conv-bias");
  BOOST_CHECK_EQUAL(suffixTransform.at("bias").get<std::string>(), "b_conv");

  // The two rank shards must describe contiguous, disjoint halves of the
  // admitted Conv input-channel axis; a gap, overlap or wrong extent would
  // change the reduction result while still looking like a TP plan.
  const auto rank0 = nativeParseJson(candidate.tensorIdentityByRole.at("Rank0").tensorShardV1);
  const auto rank1 = nativeParseJson(candidate.tensorIdentityByRole.at("Rank1").tensorShardV1);
  const auto axis0 = rank0.at("axes").at(0);
  const auto axis1 = rank1.at("axes").at(0);
  BOOST_CHECK_EQUAL(axis0.at("axis").get<std::string>(), "input-channel");
  BOOST_CHECK_EQUAL(axis0.at("begin").get<std::int64_t>(), 0);
  BOOST_CHECK_EQUAL(axis0.at("end").get<std::int64_t>(), 8);
  BOOST_CHECK_EQUAL(axis1.at("begin").get<std::int64_t>(), 8);
  BOOST_CHECK_EQUAL(axis1.at("end").get<std::int64_t>(), 16);
  BOOST_CHECK_EQUAL(axis1.at("extent").get<std::int64_t>(), 16);
  BOOST_CHECK_EQUAL(rank0.at("stage_layout_digest"), rank1.at("stage_layout_digest"));

  BOOST_CHECK_THROW(NativeSourcePartitionStrategy({NativeSourcePartitionMode::HYBRID, "conv",
    {"Prefix", "Rank0", "Rank1", "Suffix"}, "Prefix", "Suffix", "{}", 1, "input-channel",
    {1, 1, 4, 4}, {1, 1, 4, 4}, "float32", ""}), std::invalid_argument);

  // A fused Conv bias has no following Add in the source graph; the suffix
  // transform must still be emitted so the bias is applied exactly once.
  {
    auto fusedGraph = sourceGraph;
    fusedGraph.nodes[3].opType = "Relu";
    auto fusedProfile = hybridProfile;
    fusedProfile.convBiasBytes = 128;
    NativeSourcePartitionStrategy fused(std::move(fusedProfile));
    const auto plans = fused.enumerate(sourceModel, fusedGraph, {});
    BOOST_REQUIRE_EQUAL(plans.size(), 1U);
    BOOST_REQUIRE(plans.front().sourceTransformByRole.count("Suffix") == 1);
    const auto transform = nativeParseJson(plans.front().sourceTransformByRole.at("Suffix"));
    BOOST_CHECK_EQUAL(transform.at("kind").get<std::string>(), "add-conv-bias");
    BOOST_CHECK_EQUAL(transform.at("sourceConv").get<std::string>(), "conv");
  }
}

BOOST_AUTO_TEST_CASE(RequestCatalogLoadsOnnxSourceFullProfile)
{
  std::ifstream file(ndnsf::test::testFixturePath("spec182/yolo-semantic-oracle.json").string());
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
    // Spec182 compares against the historical YOLO route on purpose.
    {"legacy_yolo_retired", false},
    {"model", nativeParseJson(descriptor.canonicalJson())},
    {"source", {{"data_name", "/fixture/source"},
      {"digest", nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size())},
      {"model_manifest_digest", digest("fixture-manifest")},
      {"canonical_graph_digest", identity.graphDigest}}},
    {"recipe", {{"artifact_profile_digest", digest("fixture-profile")},
      {"assembler_descriptor_digest", digest("fixture-assembler-v1")}, {"backend_abi", "fixture-abi"},
      {"precision", descriptor.precision}, {"quantization", "none"}, {"layout", "NCHW"},
      {"padding", "none"}, {"protection_epoch", "fixture-epoch"}, {"max_source_bytes", 1048576},
      {"max_assembled_bytes", 1048576}, {"max_nodes", 100}}},
    {"publication", {{"artifact_root", "/fixture/artifacts"}}},
    {"input_format", "JSON"}, {"max_payload_bytes", 32},
    {"splitter", {{"kind", "ONNX_SOURCE"}, {"mode", "full"}, {"roles", {"FullModel"}},
      {"input_ingress_role", "FullModel"}, {"result_egress_role", "FullModel"},
      {"postprocessing", NativeJson::object()}}}
  };
  const auto loaded = NativeRequestCatalog::load(nativeCanonicalJson(configuration), source, control);
  BOOST_REQUIRE(loaded.splitter);
  const auto candidates = loaded.splitter->enumerate(loaded.model.descriptor, loaded.model.graph, {});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  BOOST_CHECK_EQUAL(candidates.front().executionPlan.roles.size(), 1U);
  BOOST_CHECK_NO_THROW(candidates.front().validate(loaded.model.graph));
  NativeRequestControl requestControl{"/fixture/request", 1,
    std::chrono::steady_clock::now() + std::chrono::seconds(10), {}};
  const auto roles = loaded.preparation->prepareRoles(loaded.model,
                                                       candidates.front(), requestControl);
  BOOST_CHECK_EQUAL(roles.size(), 1U);

  {
    onnx::ModelProto onnxModel;
    onnxModel.set_ir_version(9);
    onnxModel.add_opset_import()->set_version(20);
    auto& graph = *onnxModel.mutable_graph();
    graph.set_name("multitensor-full");
    const auto configurePort = [] (auto* port, const auto& name) {
      port->set_name(name);
      auto* type = port->mutable_type()->mutable_tensor_type();
      type->set_elem_type(onnx::TensorProto::FLOAT);
      type->mutable_shape()->add_dim()->set_dim_value(2);
    };
    configurePort(graph.add_input(), "latent");
    configurePort(graph.add_input(), "conditioning");
    configurePort(graph.add_output(), "result");
    auto* add = graph.add_node();
    add->set_op_type("Add");
    add->add_input("latent");
    add->add_input("conditioning");
    add->add_output("result");

    NativeCanonicalSource multiportSource;
    const auto wire = onnxModel.SerializeAsString();
    multiportSource.modelBytes.assign(wire.begin(), wire.end());
    auto multiportDescriptor = descriptor;
    multiportDescriptor.graphDigest =
      inspectNativeOnnxSourceGraph(multiportSource, descriptor, control).graph.graphDigest;
    auto multiport = configuration;
    multiport["model"] = nativeParseJson(multiportDescriptor.canonicalJson());
    multiport["source"]["digest"] = nativePlanningDigest(wire);
    multiport["source"]["canonical_graph_digest"] =
      canonicalOnnxSourceIdentity(multiportSource, control).graphDigest;
    const auto full = NativeRequestCatalog::load(nativeCanonicalJson(multiport),
                                                 multiportSource, control);
    const auto plans = full.splitter->enumerate(full.model.descriptor, full.model.graph, {});
    BOOST_REQUIRE_EQUAL(plans.size(), 1U);
    const auto& step = plans.front().executionPlan.steps.front();
    BOOST_REQUIRE_EQUAL(step.inputSlots.size(), 2U);
    BOOST_CHECK_EQUAL(step.inputSlots[0].slotId, "latent");
    BOOST_CHECK_EQUAL(step.inputSlots[1].slotId, "conditioning");
    for (const auto& input : step.inputSlots) {
      BOOST_CHECK_EQUAL(input.source, "EPOCH_INPUT");
      BOOST_CHECK_EQUAL(input.dtype, "float32");
      BOOST_CHECK(input.shape == decltype(input.shape){std::int64_t{2}});
    }
    BOOST_CHECK_EQUAL(step.outputSlots.front().slotId, "result");
    BOOST_CHECK_NO_THROW(full.preparation->prepareRoles(full.model, plans.front(), requestControl));
  }

  {
    auto alias = configuration;
    alias["splitter"].erase("mode");
    alias["splitter"]["selection_mode"] = "full";
    const auto aliased = NativeRequestCatalog::load(nativeCanonicalJson(alias), source, control);
    BOOST_REQUIRE(aliased.splitter);
    auto conflicting = alias;
    conflicting["splitter"]["mode"] = "component";
    BOOST_CHECK_THROW(NativeRequestCatalog::load(nativeCanonicalJson(conflicting), source, control),
                      std::invalid_argument);
  }

  for (const auto& mutation : std::vector<std::pair<std::string, NativeJson>>{
         {"mode", NativeJson("foreign")},
         {"roles", NativeJson::array()},
         {"input_ingress_role", NativeJson("Missing")},
         {"result_egress_role", NativeJson("Missing")}}) {
    auto broken = configuration;
    broken["splitter"][mutation.first] = mutation.second;
    BOOST_TEST_CONTEXT("ONNX_SOURCE mutation " << mutation.first) {
      BOOST_CHECK_THROW(NativeRequestCatalog::load(nativeCanonicalJson(broken), source, control),
                        std::invalid_argument);
    }
  }
}

BOOST_AUTO_TEST_CASE(OnnxSourceHybridPlanPreparesTransformedRoles)
{
  std::ifstream file(ndnsf::test::testFixturePath("spec261/conv-bypass.onnx").string(),
                     std::ios::binary);
  BOOST_REQUIRE(file.good());
  NativeCanonicalSource source;
  source.modelBytes.assign(std::istreambuf_iterator<char>(file), {});
  auto descriptor = model("fixture", "ConvBypass", digest("conv-bypass-placeholder"));
  NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(30),
    [] {}, 4 * 1024 * 1024, 4 * 1024 * 1024};
  const auto inspected = inspectNativeOnnxSourceGraph(source, descriptor, control);
  descriptor.graphDigest = inspected.graph.graphDigest;
  const auto identity = canonicalOnnxSourceIdentity(source, control);
  BOOST_REQUIRE_EQUAL(inspected.graph.topologicalOrder.size(), 7U);
  const auto boundary = inspected.graph.topologicalOrder.at(1);
  const NativeJson configuration{
    {"schema", "ndnsf-di-native-request-catalog-v1"},
    // Spec182 compares against the historical YOLO route on purpose.
    {"legacy_yolo_retired", false},
    {"model", nativeParseJson(descriptor.canonicalJson())},
    {"source", {{"data_name", "/fixture/conv"},
      {"digest", nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size())},
      {"model_manifest_digest", digest("conv-manifest")},
      {"canonical_graph_digest", identity.graphDigest}}},
    {"recipe", {{"artifact_profile_digest", digest("conv-profile")},
      {"assembler_descriptor_digest", digest("conv-assembler")}, {"backend_abi", "fixture-abi"},
      {"precision", descriptor.precision}, {"quantization", "none"}, {"layout", "NCHW"},
      {"padding", "none"}, {"protection_epoch", "epoch-1"}, {"max_source_bytes", 4194304},
      {"max_assembled_bytes", 4194304}, {"max_nodes", 100}}},
    {"publication", {{"artifact_root", "/fixture/conv/artifacts"}}},
    {"input_format", "OPAQUE"}, {"max_payload_bytes", 4096},
    {"splitter", {{"kind", "ONNX_SOURCE"}, {"mode", "hybrid"},
      {"roles", {"Prefix", "Rank0", "Rank1", "Suffix"}},
      {"boundary_node", boundary}, {"input_ingress_role", "Prefix"},
      {"result_egress_role", "Suffix"}, {"postprocessing", NativeJson::object()},
      {"input_channels", 16}, {"conv_input_axis", "input-channel"},
      {"activation_shape", {1, 16, 8, 8}}, {"partial_shape", {1, 32, 8, 8}},
      {"activation_dtype", "float32"},
      {"conv_weight_bytes", 18432}, {"conv_bias_bytes", 128},
      {"conv_input_tensor", "scaled"}, {"conv_weight_initializer", "w_conv"},
      {"conv_bias_initializer", "b_conv"}, {"conv_output_tensor", "conv_out"},
      {"conv_pads", {1, 1, 1, 1}}, {"conv_strides", {1, 1}},
      {"conv_dilations", {1, 1}}}}
  };
  const auto loaded = NativeRequestCatalog::load(nativeCanonicalJson(configuration), source, control);
  BOOST_REQUIRE(loaded.splitter);
  const auto candidates = loaded.splitter->enumerate(loaded.model.descriptor, loaded.model.graph, {});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  BOOST_REQUIRE_EQUAL(candidates.front().sourceTransformByRole.size(), 3U);
  BOOST_REQUIRE(candidates.front().requirementsByRole.at("Rank0").weightBytes);
  BOOST_CHECK_EQUAL(*candidates.front().requirementsByRole.at("Rank0").weightBytes, 9216U);
  BOOST_CHECK_EQUAL(*candidates.front().requirementsByRole.at("Rank1").weightBytes, 9216U);
  BOOST_CHECK_EQUAL(*candidates.front().requirementsByRole.at("Suffix").weightBytes, 128U);
  const auto& dependencies = candidates.front().executionPlan.dependencies;
  const auto partial = std::find_if(dependencies.begin(), dependencies.end(),
    [] (const auto& edge) { return edge.dependencyId == "rank1-to-sum"; });
  BOOST_REQUIRE(partial != dependencies.end());
  BOOST_CHECK_EQUAL(partial->operationKind, "TENSOR_PARTIAL");
  BOOST_CHECK_EQUAL(partial->producerStepId, "Rank1/RUN");
  BOOST_CHECK_EQUAL(partial->consumerStepId, "Rank0/SUM");
  BOOST_CHECK(partial->useNdnsfDataV1);
  const auto rank0Transform = nativeParseJson(
    candidates.front().sourceTransformByRole.at("Rank0"));
  BOOST_CHECK_EQUAL(rank0Transform.at("input").get<std::string>(), "scaled");
  BOOST_CHECK_EQUAL(rank0Transform.at("weight").get<std::string>(), "w_conv");
  BOOST_CHECK_EQUAL(rank0Transform.at("output").get<std::string>(), "conv_out");
  BOOST_CHECK_EQUAL(rank0Transform.at("pads").at(0).get<std::int64_t>(), 1);
  const auto capabilities = nativePlanRequiredCapabilities(candidates.front());
  BOOST_CHECK(std::find(capabilities.begin(), capabilities.end(),
    NATIVE_SOURCE_TRANSFORM_CAPABILITY_V1) != capabilities.end());
  BOOST_CHECK_NO_THROW(candidates.front().validate(loaded.model.graph));
  NativeRequestControl requestControl{"/fixture/conv/request", 1,
    std::chrono::steady_clock::now() + std::chrono::seconds(10), {}};
  const auto roles = loaded.preparation->prepareRoles(loaded.model, candidates.front(), requestControl);
  BOOST_REQUIRE_EQUAL(roles.size(), 4U);
  for (const auto& role : roles) {
    BOOST_TEST_CONTEXT(role.selectedRole) {
      BOOST_CHECK_NO_THROW(validateNativeAssembly(role));
    }
  }
  std::size_t transformed = 0;
  for (const auto& role : roles)
    if (!role.sourceTransformV1.empty()) ++transformed;
  BOOST_CHECK_EQUAL(transformed, 3U);
  auto insufficient = candidates.front();
  insufficient.requirementsByRole.at("Suffix").weightBytes = 127;
  insufficient.candidateDigest = insufficient.computedDigest();
  BOOST_CHECK_EXCEPTION(
    loaded.preparation->prepareRoles(loaded.model, insufficient, requestControl),
    std::invalid_argument, [] (const std::invalid_argument& failure) {
      return std::string(failure.what()) ==
        "native role weight budget is below its consumed initializers";
    });
}

BOOST_AUTO_TEST_CASE(NativeConvInputChannelRangesAreContiguous)
{
  const auto halves = nativeConvInputChannelRanges(16, 2);
  BOOST_REQUIRE_EQUAL(halves.size(), 2U);
  BOOST_CHECK_EQUAL(halves[0].first, 0U); BOOST_CHECK_EQUAL(halves[0].second, 8U);
  BOOST_CHECK_EQUAL(halves[1].first, 8U); BOOST_CHECK_EQUAL(halves[1].second, 16U);
  const auto thirds = nativeConvInputChannelRanges(10, 3);
  BOOST_REQUIRE_EQUAL(thirds.size(), 3U);
  BOOST_CHECK_EQUAL(thirds[0].second, 4U);
  BOOST_CHECK_EQUAL(thirds[1].first, 4U); BOOST_CHECK_EQUAL(thirds[1].second, 7U);
  BOOST_CHECK_EQUAL(thirds[2].first, 7U); BOOST_CHECK_EQUAL(thirds[2].second, 10U);
  BOOST_CHECK_THROW(nativeConvInputChannelRanges(1, 2), std::invalid_argument);
  BOOST_CHECK_THROW(nativeConvInputChannelRanges(4, 0), std::invalid_argument);
  BOOST_CHECK_THROW(nativeConvInputChannelRanges(4, 5), std::invalid_argument);

  // The bias codec writes `partial`; parsing must round-trip that name rather
  // than requiring the channel-partial `output` key.
  NativeOnnxSourceTransformV1 bias;
  bias.kind = "add-conv-bias";
  bias.sourceConv = "conv0"; bias.node = "bias0";
  bias.bias = "b_conv"; bias.output = "conv_out";
  const auto parsed = NativeOnnxSourceTransformV1::parse(bias.canonicalJson());
  BOOST_CHECK_EQUAL(parsed.kind, "add-conv-bias");
  BOOST_CHECK_EQUAL(parsed.output, "conv_out");
  BOOST_CHECK_EQUAL(parsed.bias, "b_conv");
}

namespace {

std::vector<float>
readNpyFloats(const std::filesystem::path& path, std::vector<std::int64_t>* shape = nullptr)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("npy missing");
  std::string raw((std::istreambuf_iterator<char>(input)), {});
  if (raw.size() < 128 || raw.compare(0, 6, "\x93NUMPY") != 0)
    throw std::runtime_error("npy signature");
  const auto major = static_cast<unsigned char>(raw[6]);
  std::size_t offset = 0, length = 0;
  if (major == 1) {
    std::uint16_t value = 0; std::memcpy(&value, raw.data() + 8, 2);
    offset = 10; length = value;
  } else {
    std::uint32_t value = 0; std::memcpy(&value, raw.data() + 8, 4);
    offset = 12; length = value;
  }
  const auto header = raw.substr(offset, length);
  if (header.find("'<f4'") == std::string::npos) throw std::runtime_error("npy dtype");
  std::vector<std::int64_t> dims;
  const auto open = header.find('(');
  const auto close = header.find(')', open);
  std::size_t index = open + 1;
  while (index < close) {
    while (index < close && (header[index] == ' ' || header[index] == ',')) ++index;
    std::size_t end = index;
    while (end < close && header[end] >= '0' && header[end] <= '9') ++end;
    if (end == index) break;
    dims.push_back(std::stoll(header.substr(index, end - index)));
    index = end;
  }
  std::size_t count = 1;
  for (const auto dim : dims) count *= static_cast<std::size_t>(dim);
  if (raw.size() != offset + length + count * 4) throw std::runtime_error("npy size");
  std::vector<float> result(count);
  std::memcpy(result.data(), raw.data() + offset + length, count * 4);
  if (shape) *shape = dims;
  return result;
}

onnx::ModelProto
subgraph(const onnx::ModelProto& source, const std::vector<std::string>& nodeNames,
         const std::vector<std::string>& inputs, const std::vector<std::string>& outputs,
         const std::vector<std::string>& initializers)
{
  onnx::ModelProto result;
  result.set_ir_version(source.ir_version());
  for (const auto& opset : source.opset_import()) *result.add_opset_import() = opset;
  result.mutable_graph()->set_name("spec261-transform");
  for (const auto& node : source.graph().node())
    if (std::find(nodeNames.begin(), nodeNames.end(), node.name()) != nodeNames.end())
      *result.mutable_graph()->add_node() = node;
  for (const auto& initializer : source.graph().initializer())
    if (std::find(initializers.begin(), initializers.end(), initializer.name()) != initializers.end())
      *result.mutable_graph()->add_initializer() = initializer;
  // The reference graph declares only full-model I/O, so an internal handoff
  // (for example the prefix output) has no ValueInfo in the source bytes. Give
  // the role boundary the exact typed contract the caller provides.
  const auto shapeOf = [&] (const std::string& name) -> std::optional<std::vector<std::int64_t>> {
    if (name == "scaled") return std::vector<std::int64_t>{1, 16, 8, 8};
    if (name == "conv_out") return std::vector<std::int64_t>{1, 32, 8, 8};
    return std::nullopt;
  };
  const auto addValue = [&] (const std::string& name, bool input) {
    const auto& values = input ? source.graph().input() : source.graph().output();
    for (const auto& value : values)
      if (value.name() == name) {
        if (input) *result.mutable_graph()->add_input() = value;
        else *result.mutable_graph()->add_output() = value;
        return;
      }
    const auto shape = shapeOf(name);
    if (!shape) throw std::runtime_error("subgraph value missing: " + name);
    auto* value = input ? result.mutable_graph()->add_input()
                         : result.mutable_graph()->add_output();
    value->set_name(name);
    auto* tensor = value->mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    for (const auto dim : *shape) tensor->mutable_shape()->add_dim()->set_dim_value(dim);
  };
  for (const auto& name : inputs) addValue(name, true);
  for (const auto& name : outputs) addValue(name, false);
  onnx::checker::check_model(result);
  return result;
}

std::vector<float>
runOrt(const onnx::ModelProto& model, const std::map<std::string, std::vector<float>>& inputs,
       const std::map<std::string, std::vector<std::int64_t>>& shapes,
       const std::string& output, const std::vector<std::int64_t>& outputShape,
       GraphOptimizationLevel optimization)
{
  std::string bytes;
  model.SerializeToString(&bytes);
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "spec261-transform");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  options.SetGraphOptimizationLevel(optimization);
  Ort::Session session(env, bytes.data(), bytes.size(), options);
  Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::vector<Ort::Value> values;
  std::vector<const char*> names;
  for (const auto& item : inputs) {
    const auto& shape = shapes.at(item.first);
    values.push_back(Ort::Value::CreateTensor<float>(memory,
      const_cast<float*>(item.second.data()), item.second.size(), shape.data(), shape.size()));
    names.push_back(item.first.c_str());
  }
  const char* outputName = output.c_str();
  auto result = session.Run(Ort::RunOptions{nullptr}, names.data(), values.data(), values.size(),
                            &outputName, 1);
  const auto outputInfo = result.front().GetTensorTypeAndShapeInfo();
  BOOST_REQUIRE(outputInfo.GetShape() == outputShape);
  const auto count = outputInfo.GetElementCount();
  const auto* data = result.front().GetTensorData<float>();
  return {data, data + count};
}

constexpr int CONV_INPUT_CHANNELS = 16;
constexpr int CONV_OUTPUT_CHANNELS = 32;
constexpr int CONV_SIDE = 8;
constexpr int CONV_PLANE = CONV_SIDE * CONV_SIDE;
const std::vector<std::int64_t> CONV_INPUT_SHAPE{1, CONV_INPUT_CHANNELS, CONV_SIDE, CONV_SIDE};
const std::vector<std::int64_t> CONV_OUTPUT_SHAPE{1, CONV_OUTPUT_CHANNELS, CONV_SIDE, CONV_SIDE};

onnx::ModelProto
loadConvFixture()
{
  std::ifstream file(ndnsf::test::testFixturePath("spec261/conv-bypass.onnx").string(),
                     std::ios::binary);
  BOOST_REQUIRE(file.good());
  const std::string bytes((std::istreambuf_iterator<char>(file)), {});
  onnx::ModelProto source;
  BOOST_REQUIRE(source.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
  return source;
}

struct HybridConvResult
{
  std::array<std::vector<float>, 2> ranks;
  std::vector<float> sum;
  std::vector<float> output;
};

HybridConvResult
runHybridConv(const onnx::ModelProto& source, const std::vector<float>& images,
              const std::vector<float>& residual, GraphOptimizationLevel optimization,
              bool synthesizeBias = false)
{
  const auto prefix = subgraph(source, {"pre0"}, {"images"}, {"scaled"}, {"scale"});
  const auto scaled = runOrt(prefix, {{"images", images}}, {{"images", CONV_INPUT_SHAPE}},
                             "scaled", CONV_INPUT_SHAPE, optimization);
  BOOST_REQUIRE_EQUAL(scaled.size(), images.size());

  const auto rankModel = subgraph(source, {"conv0"}, {"scaled"}, {"conv_out"}, {"w_conv"});
  NativeOnnxSourceTransformV1 transform;
  transform.kind = "conv-input-channel-partial";
  transform.node = "conv0";
  transform.axis = "input-channel";
  transform.input = "scaled";
  transform.weight = "w_conv";
  transform.output = "conv_out";
  transform.extent = CONV_INPUT_CHANNELS;
  transform.pads = {1, 1, 1, 1};
  transform.strides = {1, 1};
  transform.dilations = {1, 1};

  HybridConvResult result;
  std::vector<std::vector<NamedTensorView>> contributions;
  for (std::size_t rank = 0; rank < result.ranks.size(); ++rank) {
    transform.begin = rank * (CONV_INPUT_CHANNELS / 2);
    transform.end = transform.begin + CONV_INPUT_CHANNELS / 2;
    auto& partial = result.ranks[rank];
    partial = runOrt(applyNativeOnnxSourceTransform(rankModel, transform), {{"scaled", scaled}},
                     {{"scaled", CONV_INPUT_SHAPE}}, "conv_out", CONV_OUTPUT_SHAPE, optimization);
    BOOST_REQUIRE_EQUAL(partial.size(), residual.size());
    const auto* data = reinterpret_cast<const std::uint8_t*>(partial.data());
    contributions.push_back({{"conv_out", TensorElementType::Float32, CONV_OUTPUT_SHAPE,
                              {data, partial.size() * sizeof(float)}}});
  }
  const auto summed = sumTensorBundleViews("conv_out", contributions);
  const auto views = decodeTensorBundleViews(summed);
  BOOST_REQUIRE_EQUAL(views.size(), 1U);
  BOOST_REQUIRE_EQUAL(views.front().payload.size(), residual.size() * sizeof(float));
  result.sum.resize(residual.size());
  std::memcpy(result.sum.data(), views.front().payload.data(), views.front().payload.size());

  auto suffix = subgraph(source, {"bias0", "relu0", "add0", "relu1", "head0"},
                         {"conv_out", "residual"}, {"predictions"}, {"b_conv"});
  if (synthesizeBias) {
    // Model the suffix after extraction from a source Conv with a fused bias.
    auto& graph = *suffix.mutable_graph();
    BOOST_REQUIRE_EQUAL(graph.node(0).name(), "bias0");
    BOOST_REQUIRE_EQUAL(graph.node(1).name(), "relu0");
    graph.mutable_node(1)->set_input(0, "conv_out");
    graph.mutable_node()->DeleteSubrange(0, 1);
    auto* bias = graph.mutable_initializer(0);
    bias->clear_dims();
    bias->add_dims(CONV_OUTPUT_CHANNELS);
  }
  NativeOnnxSourceTransformV1 bias;
  bias.kind = "add-conv-bias";
  bias.sourceConv = "conv0";
  bias.node = "bias0";
  bias.bias = "b_conv";
  bias.output = "conv_out";
  const auto derived = applyNativeOnnxSourceTransform(suffix, bias);
  result.output = runOrt(derived, {{"conv_out", result.sum}, {"residual", residual}},
    {{"conv_out", CONV_OUTPUT_SHAPE}, {"residual", CONV_OUTPUT_SHAPE}},
    "predictions", CONV_OUTPUT_SHAPE, optimization);
  return result;
}

struct ConvReferenceValue
{
  double value;
  double magnitude;
};

std::vector<ConvReferenceValue>
referenceConvFp64(const onnx::ModelProto& source, const std::vector<float>& images,
                  const std::vector<float>& residual)
{
  std::map<std::string, std::vector<float>> initializers;
  for (const auto& tensor : source.graph().initializer())
    initializers.emplace(tensor.name(), onnx::ParseData<float>(&tensor));
  const auto& scale = initializers.at("scale");
  const auto& weights = initializers.at("w_conv");
  const auto& bias = initializers.at("b_conv");
  BOOST_REQUIRE_EQUAL(scale.size(), CONV_INPUT_CHANNELS);
  BOOST_REQUIRE_EQUAL(weights.size(), CONV_OUTPUT_CHANNELS * CONV_INPUT_CHANNELS * 9);
  BOOST_REQUIRE_EQUAL(bias.size(), CONV_OUTPUT_CHANNELS);

  std::vector<ConvReferenceValue> result;
  for (int output = 0; output < CONV_OUTPUT_CHANNELS; ++output) {
    for (int y = 0; y < CONV_SIDE; ++y) {
      for (int x = 0; x < CONV_SIDE; ++x) {
        double sum = 0;
        double magnitude = std::abs(bias[output]);
        for (int input = 0; input < CONV_INPUT_CHANNELS; ++input) {
          for (int ky = 0; ky < 3; ++ky) {
            for (int kx = 0; kx < 3; ++kx) {
              const int iy = y + ky - 1;
              const int ix = x + kx - 1;
              if (iy < 0 || iy >= CONV_SIDE || ix < 0 || ix >= CONV_SIDE)
                continue;
              const auto weight = weights[(output * CONV_INPUT_CHANNELS + input) * 9 + ky * 3 + kx];
              const double pixel = images[input * CONV_PLANE + iy * CONV_SIDE + ix];
              const double term = pixel * scale[input] * weight;
              sum += term;
              magnitude += std::abs(term);
            }
          }
        }
        const double skip = residual[output * CONV_PLANE + y * CONV_SIDE + x];
        const double outputValue = std::max(0.0, std::max(0.0, sum + bias[output]) + skip);
        result.push_back({outputValue, magnitude + std::abs(skip)});
      }
    }
  }
  return result;
}

void
checkConvFp32Error(const std::vector<float>& actual, const std::vector<ConvReferenceValue>& expected)
{
  BOOST_REQUIRE_EQUAL(actual.size(), expected.size());
  // Budget four FP32 epsilons of the input-term magnitudes for this CPU fixture.
  // The sum of magnitudes keeps the check meaningful when terms nearly cancel.
  constexpr double BUDGET = 4 * std::numeric_limits<float>::epsilon();
  double maxError = 0;
  double maxBudgetFraction = 0;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const double error = std::abs(actual[i] - expected[i].value);
    const double bound = BUDGET * expected[i].magnitude;
    BOOST_TEST_CONTEXT("element " << i) {
      BOOST_CHECK_LE(error, bound);
    }
    maxError = std::max(maxError, error);
    if (bound > 0)
      maxBudgetFraction = std::max(maxBudgetFraction, error / bound);
  }
  BOOST_TEST_MESSAGE("max absolute error=" << maxError << ", max budget fraction="
                                             << maxBudgetFraction);
}

} // namespace

BOOST_AUTO_TEST_CASE(NativeOnnxSourceEntryProgramBindsOrderedCallsAndRank)
{
  NativeExecutionPlan plan;
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
  plan.stageLayout = {{"middle", nativePlanningDigest("fixture-middle"), {"r0", "r1"}}};
  const auto makeProgram = [&plan] (std::uint64_t rank) {
    const std::string role = "r" + std::to_string(rank);
    NativeOnnxSourceEntryProgramV1 program;
    for (std::size_t block = 0; block < 2; ++block) {
      const std::string tag = std::to_string(block);
      NativeOnnxSourceEntryRecipeV1 entry;
      auto& transform = entry.transform;
      entry.entryId = role + "/ffn/" + tag;
      transform.kind = "ffn-pair-partial";
      transform.axis = "ffn-channel";
      transform.node = "up-" + tag;
      transform.downNode = "down-" + tag;
      entry.sourceNodes = {transform.node, "gelu-" + tag, transform.downNode};
      transform.weight = "up-weight-" + tag;
      transform.downWeight = "down-weight-" + tag;
      transform.input = "h" + tag;
      transform.output = "h" + std::to_string(block + 1);
      transform.extent = 7;
      transform.begin = rank == 0 ? 0 : 4;
      transform.end = rank == 0 ? 4 : 7;
      NativeExecutionStepSpec call;
      call.stepId = entry.entryId + "/call";
      call.unitId = role;
      call.operation = "CALL_SUBGRAPH";
      call.artifactEntry = entry.entryId;
      NativeStepSlotSpec port;
      port.dtype = "float32";
      port.shape = {std::int64_t{1}, std::int64_t{3}};
      port.slotId = transform.input;
      call.inputSlots.push_back(port);
      port.slotId = transform.output;
      call.outputSlots.push_back(port);
      plan.steps.push_back(std::move(call));
      program.entries.push_back(std::move(entry));
    }
    return program;
  };
  const auto left = makeProgram(0);
  const auto right = makeProgram(1);
  const auto wire = left.canonicalJson();
  const auto parsed = NativeOnnxSourceEntryProgramV1::parse(wire);
  BOOST_CHECK_EQUAL(parsed.canonicalJson(), wire);
  BOOST_CHECK_NO_THROW(parsed.validateRank(plan, "r0", 0));
  BOOST_CHECK_NO_THROW(right.validateRank(plan, "r1", 1));
  BOOST_CHECK_THROW(parsed.validateRank(plan, "r0", 1), std::invalid_argument);
  auto wrong = parsed;
  std::swap(wrong.entries[0], wrong.entries[1]);
  BOOST_CHECK_THROW(wrong.validateRank(plan, "r0", 0), std::invalid_argument);
  wrong = parsed;
  wrong.entries[1].transform.end = 3;
  BOOST_CHECK_THROW(wrong.validateRank(plan, "r0", 0), std::invalid_argument);
  wrong = parsed;
  wrong.entries[1].sourceNodes[1] = wrong.entries[0].sourceNodes[1];
  BOOST_CHECK_THROW(NativeOnnxSourceEntryProgramV1::parse(wrong.canonicalJson()),
                    std::invalid_argument);
  auto wrongPlan = plan;
  wrongPlan.steps[1].inputSlots.front().shape[1] = std::string("unbound");
  BOOST_CHECK_THROW(parsed.validateRank(wrongPlan, "r0", 0), std::invalid_argument);
  wrongPlan.steps.erase(wrongPlan.steps.begin() + 1);
  BOOST_CHECK_THROW(parsed.validateRank(wrongPlan, "r0", 0), std::invalid_argument);

  NativeSplitCandidate candidate;
  candidate.executionPlan = plan;
  candidate.sourceTransformByRole = {{"r0", wire}, {"r1", right.canonicalJson()}};
  const auto pairs = nativeDistinctProviderPairs(candidate);
  BOOST_REQUIRE_EQUAL(pairs.size(), 1u);
  BOOST_CHECK_EQUAL(pairs.front().first, "r0");
  BOOST_CHECK_EQUAL(pairs.front().second, "r1");
  const auto capabilities = nativePlanRequiredCapabilities(candidate);
  BOOST_CHECK(std::find(capabilities.begin(), capabilities.end(),
    NATIVE_SOURCE_ENTRY_PROGRAM_CAPABILITY_V1) != capabilities.end());
  wrong = right;
  wrong.entries[1].transform.downWeight = "foreign-weight";
  candidate.sourceTransformByRole["r1"] = wrong.canonicalJson();
  BOOST_CHECK_THROW(nativeDistinctProviderPairs(candidate), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeOnnxAttentionEntryProgramBindsSourceAndRankPorts)
{
  NativeExecutionPlan plan;
  plan.roles = {"r0", "r1"};
  plan.tensorDegree = 2;
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
  plan.stageLayout = {{"attention", nativePlanningDigest("attention-stage"), plan.roles}};
  const auto slot = [] (std::string name, std::vector<std::int64_t> shape) {
    NativeStepSlotSpec port;
    port.slotId = std::move(name);
    port.dtype = "float32";
    port.shape.assign(shape.begin(), shape.end());
    return port;
  };
  std::array<NativeOnnxSourceEntryProgramV1, 2> programs;
  for (std::uint64_t rank = 0; rank < 2; ++rank) {
    NativeOnnxSourceEntryProgramV1 program;
    for (bool qkv : {true, false}) {
      NativeOnnxSourceEntryRecipeV1 entry;
      entry.entryId = plan.roles[rank] + (qkv ? "/project" : "/heads");
      entry.sourceNodes = qkv ? std::vector<std::string>{"onnx-node-0", "onnx-node-1"}
                              : std::vector<std::string>{"onnx-node-2", "onnx-node-3"};
      auto& transform = entry.transform;
      transform.kind = qkv ? "attention-qkv" : "attention-heads";
      transform.axis = "attention-head";
      transform.node = entry.sourceNodes.front();
      transform.begin = rank;
      transform.end = rank + 1;
      transform.extent = 2;
      transform.headWidth = 32;
      transform.attentionPorts = {"q", "k", "v"};
      NativeExecutionStepSpec call;
      call.stepId = entry.entryId + "/call";
      call.artifactEntry = entry.entryId;
      call.unitId = plan.roles[rank];
      call.operation = "CALL_SUBGRAPH";
      const std::vector<NativeStepSlotSpec> local{
        slot("q", {1, 2, 32}), slot("k", {1, 3, 32}), slot("v", {1, 3, 32})};
      if (qkv) {
        call.inputSlots = {slot("hidden", {1, 2, 64}), slot("text", {1, 3, 64})};
        call.outputSlots = local;
      }
      else {
        call.inputSlots = local;
        call.inputSlots.push_back(slot("mask", {1, 1, 2, 3}));
        call.outputSlots = {slot("partial", {1, 2, 64})};
      }
      auto original = qkv ? transform.originalPorts(call.outputSlots, true)
                          : transform.originalPorts(call.inputSlots, false);
      for (std::size_t port = 0; port < 3; ++port) {
        BOOST_CHECK_EQUAL(original[port].slotId, local[port].slotId);
        BOOST_CHECK_EQUAL(std::get<std::int64_t>(original[port].shape.back()), 64);
        BOOST_CHECK_EQUAL(std::get<std::int64_t>(original[port].shape[1]), port == 0 ? 2 : 3);
      }
      if (!qkv) BOOST_CHECK(original.back().shape == call.inputSlots.back().shape);
      for (const auto& port : call.inputSlots) entry.inputPorts.push_back(port.slotId);
      for (const auto& port : call.outputSlots) entry.outputPorts.push_back(port.slotId);
      plan.steps.push_back(std::move(call));
      program.entries.push_back(std::move(entry));
    }
    const auto canonical = program.canonicalJson();
    auto parsed = NativeOnnxSourceEntryProgramV1::parse(canonical);
    BOOST_CHECK_EQUAL(parsed.canonicalJson(), canonical);
    BOOST_CHECK(parsed.bindSourceOrdinals({0, 1, 2, 3}) ==
                (std::vector<std::vector<std::uint64_t>>{{0, 1}, {2, 3}}));
    programs[rank] = std::move(parsed);
  }
  const auto& left = programs.front();
  BOOST_CHECK_THROW(left.validateRank(plan, "r0", 0), std::invalid_argument);
  for (const auto& role : plan.roles) {
    auto& sum = plan.steps.emplace_back();
    sum.stepId = role + "/sum";
    sum.unitId = role;
    sum.operation = "SUM";
    sum.outputSlots = {slot("partial", {1, 2, 64})};
    for (const auto& producer : plan.roles) {
      auto& operand = sum.inputSlots.emplace_back(slot(producer, {1, 2, 64}));
      operand.source = producer == role ? "LOCAL_STEP" : "REMOTE_STEP";
      operand.producerStepId = producer + "/heads/call";
      operand.producerSlotId = "partial";
    }
  }
  for (std::size_t rank = 0; rank < 2; ++rank)
    BOOST_CHECK_NO_THROW(programs[rank].validateRank(plan, plan.roles[rank], rank));
  auto badPlan = plan;
  badPlan.steps.front().outputSlots[0].shape.back() = std::int64_t{64};
  BOOST_CHECK_THROW(left.validateRank(badPlan, "r0", 0), std::invalid_argument);
  badPlan = plan;
  badPlan.steps[1].inputSlots[1].slotId = "foreign-k";
  BOOST_CHECK_THROW(left.validateRank(badPlan, "r0", 0), std::invalid_argument);
  badPlan = plan;
  std::swap(badPlan.steps.back().inputSlots[0], badPlan.steps.back().inputSlots[1]);
  BOOST_CHECK_THROW(left.validateRank(badPlan, "r0", 0), std::invalid_argument);
  badPlan = plan;
  badPlan.steps.back().operation = "CALL_SUBGRAPH";
  BOOST_CHECK_THROW(left.validateRank(badPlan, "r0", 0), std::invalid_argument);
  BOOST_CHECK_THROW(left.validateRank(plan, "r1", 1), std::invalid_argument);
  NativeSplitCandidate candidate;
  candidate.executionPlan = plan;
  candidate.sourceTransformByRole = {{"r0", left.canonicalJson()}};
  const auto needed = nativePlanRequiredCapabilities(candidate);
  BOOST_CHECK(std::find(needed.begin(), needed.end(), NATIVE_SOURCE_ATTENTION_ENTRY_CAPABILITY_V1)
              != needed.end());
}

BOOST_AUTO_TEST_CASE(NativeOnnxSignedRmsEntriesShareOneAuthenticatedRegion)
{
  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(20);
  auto& graph = *model.mutable_graph();
  graph.set_name("sealed-source-rms");
  for (auto* port : {graph.add_input(), graph.add_output()}) {
    auto& type = *port->mutable_type()->mutable_tensor_type();
    type.set_elem_type(onnx::TensorProto::FLOAT);
    for (int dimension : {1, 2, 4}) type.mutable_shape()->add_dim()->set_dim_value(dimension);
  }
  graph.mutable_input(0)->set_name("q");
  graph.mutable_output(0)->set_name("normalized");
  auto& axes = *graph.add_initializer();
  axes.set_name("axes");
  axes.set_data_type(onnx::TensorProto::INT64);
  axes.add_dims(1);
  axes.add_int64_data(-1);
  auto& epsilon = *graph.add_initializer();
  epsilon.set_name("epsilon");
  epsilon.set_data_type(onnx::TensorProto::FLOAT);
  epsilon.add_float_data(0.125f);
  auto& gamma = *graph.add_initializer();
  gamma.set_name("gamma");
  gamma.set_data_type(onnx::TensorProto::FLOAT);
  gamma.add_dims(4);
  for (float value : {0.75f, 0.5f, 1.25f, 1.5f}) gamma.add_float_data(value);
  const auto add = [&] (const char* op, std::initializer_list<const char*> inputs, const char* out) {
    auto& node = *graph.add_node();
    node.set_name(out);
    node.set_op_type(op);
    for (const auto input : inputs) node.add_input(input);
    node.add_output(out);
  };
  add("Mul", {"q", "q"}, "square");
  add("ReduceMean", {"square", "axes"}, "stat");
  add("Add", {"stat", "epsilon"}, "shift");
  add("Sqrt", {"shift"}, "root");
  add("Div", {"q", "root"}, "divided");
  add("Mul", {"divided", "gamma"}, "normalized");

  const std::vector<std::uint64_t> ordinals{0, 1, 2, 3, 4, 5};
  const auto slot = [] (std::string name, std::int64_t width) {
    NativeStepSlotSpec port;
    port.slotId = std::move(name);
    port.dtype = "float32";
    port.shape = {std::int64_t{1}, std::int64_t{2}, width};
    port.source = "LOCAL_STEP";
    port.layoutDigest = nativePlanningDigest("rms-width-" + std::to_string(width));
    return port;
  };
  NativeExecutionPlan plan;
  plan.roles = {"r0", "r1"};
  plan.tensorDegree = 2;
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
  plan.stageLayout = {{"middle", nativePlanningDigest("rms-stage"), plan.roles}};
  std::array<NativeOnnxSourceEntryProgramV1, 2> programs;
  for (std::size_t rank = 0; rank < 2; ++rank) {
    auto& program = programs[rank];
    const auto& role = plan.roles[rank];
    for (const bool computeStat : {true, false}) {
      auto& entry = program.entries.emplace_back();
      entry.entryId = role + (computeStat ? "/stat" : "/normalize");
      for (auto ordinal : ordinals) entry.sourceNodes.push_back("onnx-node-" + std::to_string(ordinal));
      auto& transform = entry.transform;
      transform.kind = computeStat ? "attention-rms-stat" : "attention-rms-normalize";
      transform.axis = "attention-head";
      transform.node = entry.sourceNodes.front();
      transform.input = "q";
      transform.output = "normalized";
      transform.statistic = "stat";
      transform.globalStatistic = "global";
      transform.begin = rank;
      transform.end = rank + 1;
      transform.extent = 2;
      transform.headWidth = 2;
      auto& call = plan.steps.emplace_back();
      call.stepId = entry.entryId;
      call.unitId = role;
      call.artifactEntry = entry.entryId;
      call.operation = "CALL_SUBGRAPH";
      call.inputSlots = {slot("q", 2)};
      call.inputSlots.front().source = "EPOCH_INPUT";
      if (computeStat) {
        call.outputSlots = {slot("stat", 1)};
      }
      else {
        auto global = slot("global", 1);
        global.source = "LOCAL_STEP";
        global.producerStepId = role + "/sum";
        global.producerSlotId = "global";
        call.inputSlots.push_back(std::move(global));
        call.outputSlots = {slot("normalized", 2)};
      }
      for (const auto& input : call.inputSlots) entry.inputPorts.push_back(input.slotId);
      for (const auto& output : call.outputSlots) entry.outputPorts.push_back(output.slotId);
    }
    auto& sum = plan.steps.emplace_back();
    sum.stepId = role + "/sum";
    sum.unitId = role;
    sum.operation = "SUM";
    sum.outputSlots = {slot("global", 1)};
    for (const auto& contributor : plan.roles) {
      auto operand = slot(contributor, 1);
      operand.producerStepId = contributor + "/stat";
      operand.producerSlotId = "stat";
      if (contributor != role) {
        operand.source = "REMOTE_STEP";
        NativeDependencySpec edge;
        edge.producers = {contributor};
        edge.consumers = {role};
        edge.tensors = {operand.producerSlotId};
        edge.keyScope = role + "/peer-stat";
        edge.dependencyId = edge.keyScope;
        edge.topicPrefix = "/NDNSF/DI/RMS";
        edge.objectNameTemplate =
          "{producerProvider}/NDNSF/DI/RMS/{sessionId}/{producerRole}/{keyScope}/{sequence}";
        edge.operationKind = "TENSOR_PARTIAL";
        edge.producerStepId = operand.producerStepId;
        edge.producerSlotId = operand.producerSlotId;
        edge.consumerStepId = sum.stepId;
        edge.consumerSlotId = operand.slotId;
        plan.dependencies.push_back(std::move(edge));
      }
      sum.inputSlots.push_back(std::move(operand));
    }
  }

  const auto phaseOf = [] (const auto& step) {
    if (step.operation == "SUM") return 1;
    return step.stepId == step.unitId + "/stat" ? 0 : 2;
  };
  std::stable_sort(plan.steps.begin(), plan.steps.end(), [&] (const auto& left, const auto& right) {
    return phaseOf(left) < phaseOf(right);
  });
  BOOST_REQUIRE_NO_THROW(plan.validateSteps());
  const auto byStep = [] (auto& steps, const std::string& id) -> auto& {
    const auto found = std::find_if(steps.begin(), steps.end(),
      [&] (const auto& step) { return step.stepId == id; });
    if (found == steps.end()) throw std::logic_error("fixture step is missing");
    return *found;
  };

  NativeCanonicalSource original;
  const auto wire = model.SerializeAsString();
  original.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(15),
                                       [] {}, 1U << 20, 2U << 20};
  NativeCanonicalSource material;
  material.materialManifest = deriveNativeCanonicalMaterialManifest(original, control);
  material.materialPayloads = material.materialManifest->payloads;
  const std::vector<std::int64_t> fullShape{1, 2, 4}, localShape{1, 2, 2}, statShape{1, 2, 1};
  const std::vector<float> x{1, -2, 3, -4, 4, 3, -2, 1};
  const auto originalOutput = runOrt(model, {{"q", x}}, {{"q", fullShape}}, "normalized",
                                     fullShape, ORT_DISABLE_ALL);
  std::array<onnx::ModelProto, 2> normalization;
  std::array<std::vector<float>, 2> local;
  std::vector<float> global(2);
  for (std::size_t rank = 0; rank < 2; ++rank) {
    const auto canonical = programs[rank].canonicalJson();
    const auto parsed = NativeOnnxSourceEntryProgramV1::parse(canonical);
    BOOST_CHECK_NO_THROW(parsed.validateRank(plan, plan.roles[rank], rank));
    const auto windows = parsed.bindSourceOrdinals(ordinals);
    BOOST_CHECK(windows == (std::vector<std::vector<std::uint64_t>>{ordinals, ordinals}));
    BOOST_CHECK(parsed.sourceNodeCover() == parsed.entries.front().sourceNodes);
    for (std::size_t token = 0; token < 2; ++token) {
      const auto start = x.begin() + token * 4 + rank * 2;
      local[rank].insert(local[rank].end(), start, start + 2);
    }
    for (std::size_t phase = 0; phase < 2; ++phase) {
      const auto& entry = parsed.entries[phase];
      const auto raw = materializeNativeOnnxEntrySource(material, entry, windows[phase],
                                                       byStep(plan.steps, entry.entryId), control, {});
      onnx::ModelProto admitted;
      BOOST_REQUIRE(admitted.ParseFromArray(raw.modelBytes.data(), raw.modelBytes.size()));
      auto derived = applyNativeOnnxSourceTransform(admitted, entry.transform);
      if (phase == 0) {
        const auto localStat = runOrt(derived, {{"q", local[rank]}}, {{"q", localShape}}, "stat",
                                      statShape, ORT_DISABLE_ALL);
        for (std::size_t index = 0; index < global.size(); ++index)
          global[index] += localStat.at(index);
      }
      else {
        normalization[rank] = std::move(derived);
      }
    }
  }
  for (std::size_t rank = 0; rank < 2; ++rank) {
    const auto output = runOrt(normalization[rank], {{"q", local[rank]}, {"global", global}},
      {{"q", localShape}, {"global", statShape}}, "normalized", localShape, ORT_DISABLE_ALL);
    for (std::size_t token = 0; token < 2; ++token) {
      for (std::size_t channel = 0; channel < 2; ++channel) {
        BOOST_CHECK_SMALL(static_cast<double>(output.at(token * 2 + channel)) -
                          originalOutput.at(token * 4 + rank * 2 + channel), 1e-6);
      }
    }
  }
  auto bad = programs.front();
  bad.entries.back().sourceNodes[4] = "onnx-node-8";
  BOOST_CHECK_THROW(bad.validate(), std::invalid_argument);
  bad = programs.front();
  bad.entries.pop_back();
  BOOST_CHECK_THROW(bad.validate(), std::invalid_argument);
  auto crossed = plan;
  byStep(crossed.steps, "r0/sum").inputSlots[1].producerSlotId = "foreign-stat";
  BOOST_CHECK_THROW(programs.front().validateRank(crossed, "r0", 0), std::invalid_argument);
  crossed = plan;
  byStep(crossed.steps, "r0/normalize").inputSlots[1].producerStepId = "r1/normalize";
  BOOST_CHECK_THROW(programs.front().validateRank(crossed, "r0", 0), std::invalid_argument);
  crossed = plan;
  auto& normalizedInput = byStep(crossed.steps, "r0/normalize").inputSlots.front();
  normalizedInput.source = "LOCAL_STEP";
  normalizedInput.producerStepId = "foreign-projection";
  normalizedInput.producerSlotId = "q";
  BOOST_CHECK_THROW(programs.front().validateRank(crossed, "r0", 0), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeOnnxSourceComponentKeepsOriginalBiasAndResidual)
{
  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(20);
  auto& graph = *model.mutable_graph();
  graph.set_name("source-finalize");
  const std::vector<std::int64_t> dims{1, 2, 3};
  for (auto* value : {graph.add_input(), graph.add_input(), graph.add_output()}) {
    auto& type = *value->mutable_type()->mutable_tensor_type();
    type.set_elem_type(onnx::TensorProto::FLOAT);
    for (const auto dimension : dims) type.mutable_shape()->add_dim()->set_dim_value(dimension);
  }
  graph.mutable_input(0)->set_name("projection");
  graph.mutable_input(1)->set_name("residual");
  graph.mutable_output(0)->set_name("result");
  auto& bias = *graph.add_initializer();
  bias.set_name("output-bias");
  bias.set_data_type(onnx::TensorProto::FLOAT);
  bias.add_dims(3);
  for (float value : {0.25f, -0.5f, 0.75f}) bias.add_float_data(value);
  auto& gate = *graph.add_initializer();
  gate.set_name("gate");
  gate.set_data_type(onnx::TensorProto::FLOAT);
  gate.add_float_data(0.5f);
  const auto append = [&] (const char* op, const char* left, const char* right, const char* result) {
    auto& node = *graph.add_node();
    node.set_op_type(op);
    node.add_input(left);
    node.add_input(right);
    node.add_output(result);
  };
  append("Add", "projection", "output-bias", "biased");
  append("Mul", "biased", "gate", "gated");
  append("Add", "gated", "residual", "result");

  NativeCanonicalSource source;
  const auto wire = model.SerializeAsString();
  source.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(15),
                                       [] {}, 1U << 20, 2U << 20};
  NativeCanonicalSource material;
  material.materialManifest = deriveNativeCanonicalMaterialManifest(source, control);
  material.materialPayloads = material.materialManifest->payloads;
  NativeOnnxSourceEntryRecipeV1 entry;
  entry.entryId = "finalize";
  entry.sourceNodes = {"onnx-node-0", "onnx-node-1", "onnx-node-2"};
  entry.transform.node = entry.sourceNodes.front();
  entry.transform.kind = "attention-source-component";
  entry.transform = NativeOnnxSourceTransformV1::parse(entry.transform.canonicalJson());
  const auto slot = [&] (const char* name) {
    NativeStepSlotSpec value;
    value.slotId = name;
    value.dtype = "float32";
    value.shape.assign(dims.begin(), dims.end());
    return value;
  };
  NativeExecutionStepSpec call;
  call.operation = "CALL_SUBGRAPH";
  call.artifactEntry = entry.entryId;
  call.inputSlots = {slot("projection"), slot("residual")};
  call.outputSlots = {slot("result")};
  const auto files = materializeNativeOnnxEntrySource(material, entry, {0, 1, 2}, call, control, {});
  BOOST_REQUIRE_EQUAL(files.initializerBytes, 0u);
  onnx::ModelProto rebuilt;
  BOOST_REQUIRE(rebuilt.ParseFromArray(files.modelBytes.data(), files.modelBytes.size()));
  const auto admitted = applyNativeOnnxSourceTransform(rebuilt, entry.transform);
  const auto data = runOrt(admitted, {{"projection", {0, 2, -4, 8, 5, 1}},
                                      {"residual", {1, 0, -1, 2, 3, 4}}},
    {{"projection", dims}, {"residual", dims}}, "result", dims, ORT_DISABLE_ALL);
  const std::vector<float> expected{1.125f, 0.75f, -2.625f, 6.125f, 5.25f, 4.875f};
  BOOST_CHECK(data == expected);
}

BOOST_AUTO_TEST_CASE(NativeOnnxCrossAttentionTailKeepsCompleteHeadsAndMask)
{
  onnx::ModelProto source;
  source.set_ir_version(9);
  source.add_opset_import()->set_version(20);
  auto& graph = *source.mutable_graph();
  graph.set_name("cross-attention-tail");
  const auto port = [] (onnx::ValueInfoProto& value, const std::string& name,
                        const std::vector<std::int64_t>& dims) {
    value.set_name(name);
    auto& type = *value.mutable_type()->mutable_tensor_type();
    type.set_elem_type(onnx::TensorProto::FLOAT);
    for (const auto dim : dims) type.mutable_shape()->add_dim()->set_dim_value(dim);
  };
  std::map<std::string, std::vector<std::int64_t>> shapes{
    {"q", {1, 2, 4}}, {"k", {1, 3, 4}}, {"v", {1, 3, 4}}, {"mask", {1, 1, 2, 3}}};
  for (const auto& [name, dims] : shapes) port(*graph.add_input(), name, dims);
  const std::vector<std::int64_t> outputShape{1, 2, 4};
  port(*graph.add_output(), "y", outputShape);
  const auto dims = [&] (const char* name, const std::vector<std::int64_t>& values) {
    auto& tensor = *graph.add_initializer();
    tensor.set_name(name);
    tensor.set_data_type(onnx::TensorProto::INT64);
    tensor.add_dims(values.size());
    for (const auto dim : values) tensor.add_int64_data(dim);
  };
  dims("query-heads", {1, 2, 2, 2});
  dims("key-heads", {1, 3, 2, 2});
  dims("merge-shape", outputShape);
  auto& scale = *graph.add_initializer();
  scale.set_name("scale");
  scale.set_data_type(onnx::TensorProto::FLOAT);
  scale.add_float_data(0.5f);
  auto& matrix = *graph.add_initializer();
  matrix.set_name("O");
  matrix.set_data_type(onnx::TensorProto::FLOAT);
  matrix.add_dims(4);
  matrix.add_dims(4);
  for (int i = 0; i < 16; ++i) matrix.add_float_data(((i * 11) % 17 - 8) / 8.0f);

  const auto op = [&] (const char* type, std::initializer_list<const char*> inputs,
                       const char* output) -> onnx::NodeProto& {
    auto& node = *graph.add_node();
    node.set_name(output);
    node.set_op_type(type);
    for (const auto input : inputs) node.add_input(input);
    node.add_output(output);
    return node;
  };
  const auto transpose = [&] (const char* input, const char* output, bool keys) {
    auto& attr = *op("Transpose", {input}, output).add_attribute();
    attr.set_name("perm");
    attr.set_type(onnx::AttributeProto::INTS);
    for (const int dim : {0, 2, keys ? 3 : 1, keys ? 1 : 3}) attr.add_ints(dim);
  };
  op("Reshape", {"q", "query-heads"}, "qh");
  op("Reshape", {"k", "key-heads"}, "kh");
  op("Reshape", {"v", "key-heads"}, "vh");
  transpose("qh", "qT", false);
  transpose("kh", "kT", true);
  transpose("vh", "vT", false);
  op("MatMul", {"qT", "kT"}, "score");
  op("Mul", {"score", "scale"}, "scaled");
  op("Add", {"scaled", "mask"}, "masked");
  auto& axis = *op("Softmax", {"masked"}, "probability").add_attribute();
  axis.set_name("axis");
  axis.set_type(onnx::AttributeProto::INT);
  axis.set_i(-1);
  op("MatMul", {"probability", "vT"}, "weighted");
  transpose("weighted", "ordered", false);
  op("Reshape", {"ordered", "merge-shape"}, "flattened");
  op("MatMul", {"flattened", "O"}, "y");

  const std::map<std::string, std::vector<float>> values{
    {"q", {1, -2, 3, -4, 2, 3, 4, 5}},
    {"k", {-1, 2, 3, -4, 2, -1, 1, 0, 3, 2, 0, -2}},
    {"v", {2, 1, 8, 9, 3, 4, 10, 11, -5, 0, -9, -7}},
    {"mask", {0, -8, 0, -9, 0, 0}},
  };
  const auto ref = runOrt(source, values, shapes, "y", outputShape, ORT_DISABLE_ALL);
  NativeCanonicalSource original;
  const auto wire = source.SerializeAsString();
  original.modelBytes.assign(wire.begin(), wire.end());
  const NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(15),
                                       [] {}, 1U << 20, 2U << 20};
  NativeCanonicalSource material;
  material.materialManifest = deriveNativeCanonicalMaterialManifest(original, control);
  material.materialPayloads = material.materialManifest->payloads;
  NativeOnnxSourceEntryRecipeV1 entry;
  entry.entryId = "rank-attention";
  entry.transform.kind = "attention-heads";
  entry.transform.axis = "attention-head";
  entry.transform.node = "onnx-node-0";
  entry.transform.attentionPorts = {"q", "k", "v"};
  entry.transform.headWidth = 2;
  entry.transform.extent = 2;
  std::vector<std::uint64_t> ordinals;
  for (int node = 0; node < graph.node_size(); ++node) {
    ordinals.push_back(node);
    entry.sourceNodes.push_back("onnx-node-" + std::to_string(node));
  }
  NativeExecutionStepSpec call;
  call.operation = "CALL_SUBGRAPH";
  call.artifactEntry = entry.entryId;
  NativeStepSlotSpec output;
  output.slotId = "y";
  output.dtype = "float32";
  output.shape.assign(outputShape.begin(), outputShape.end());
  call.outputSlots.push_back(output);
  std::vector<double> reduced(ref.size());
  for (std::size_t rank = 0; rank < 2; ++rank) {
    auto input = values;
    auto localShapes = shapes;
    for (const auto* name : {"q", "k", "v"}) {
      localShapes.at(name).back() = 2;
      const auto& full = values.at(name);
      auto& shard = input.at(name);
      shard.clear();
      for (std::size_t index = rank * 2; index < full.size(); index += 4)
        shard.insert(shard.end(), full.begin() + index, full.begin() + index + 2);
    }
    call.inputSlots.clear();
    for (const auto& [name, shape] : localShapes) {
      NativeStepSlotSpec slot;
      slot.slotId = name;
      slot.dtype = "float32";
      slot.shape.assign(shape.begin(), shape.end());
      call.inputSlots.push_back(std::move(slot));
    }
    entry.transform.begin = rank;
    entry.transform.end = rank + 1;
    entry.transform = NativeOnnxSourceTransformV1::parse(entry.transform.canonicalJson());
    const auto raw = materializeNativeOnnxEntrySource(material, entry, ordinals, call, control, {});
    onnx::ModelProto authenticated;
    BOOST_REQUIRE(authenticated.ParseFromArray(raw.modelBytes.data(), raw.modelBytes.size()));
    const auto local = applyNativeOnnxSourceTransform(authenticated, entry.transform);
    const auto partial = runOrt(local, input, localShapes, "y", outputShape, ORT_DISABLE_ALL);
    BOOST_REQUIRE_EQUAL(partial.size(), ref.size());
    for (std::size_t index = 0; index < partial.size(); ++index) reduced[index] += partial[index];
  }
  for (std::size_t index = 0; index < ref.size(); ++index)
    BOOST_CHECK_SMALL(reduced[index] - ref[index], 1e-5);

  auto wrong = source;
  for (auto& node : *wrong.mutable_graph()->mutable_node())
    if (node.op_type() == "Softmax") node.mutable_attribute(0)->set_i(1);
  BOOST_CHECK_EXCEPTION(lowerNativeOnnxAttentionHeads(wrong, "q", "k", "v", 0, 1),
    std::invalid_argument, [] (const auto& error) {
      return std::string(error.what()) == "attention operation must use its last axis";
    });
  auto wrongGeometry = entry.transform;
  wrongGeometry.headWidth = 1;
  wrongGeometry.extent = 4;
  wrongGeometry.begin = 2;
  wrongGeometry.end = 4;
  BOOST_CHECK_THROW(applyNativeOnnxSourceTransform(source, wrongGeometry), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeOnnxSourceTransformShardsPackedFfnProjections)
{
  onnx::ModelProto source;
  source.set_ir_version(9);
  source.add_opset_import()->set_version(20);
  auto& contrib = *source.add_opset_import();
  contrib.set_domain("com.microsoft");
  contrib.set_version(1);
  auto& graph = *source.mutable_graph();
  graph.set_name("packed-ffn");
  const auto port = [] (onnx::ValueInfoProto& value, const char* name, int width) {
    value.set_name(name);
    auto& type = *value.mutable_type()->mutable_tensor_type();
    type.set_elem_type(onnx::TensorProto::FLOAT);
    for (int dim : {1, 1, 1, width})
      type.mutable_shape()->add_dim()->set_dim_value(dim);
  };
  port(*graph.add_input(), "x", 32);
  port(*graph.add_output(), "y", 3);
  const auto projection = [&] (const char* name, const char* input, const char* output,
                               int k, int n, int seed) {
    auto& node = *graph.add_node();
    node.set_name(name);
    node.set_op_type("MatMulNBits");
    node.set_domain("com.microsoft");
    node.add_input(input);
    node.add_input(std::string(name) + ".weight");
    node.add_input(std::string(name) + ".scale");
    node.add_output(output);
    for (const auto& [key, value] : std::map<std::string, int>{
           {"K", k}, {"N", n}, {"bits", 4}, {"block_size", 32}, {"accuracy_level", 1}}) {
      auto& attr = *node.add_attribute();
      attr.set_name(key);
      attr.set_type(onnx::AttributeProto::INT);
      attr.set_i(value);
    }
    auto& packed = *graph.add_initializer();
    packed.set_name(node.input(1));
    packed.set_data_type(onnx::TensorProto::UINT8);
    for (const auto dim : {n, k / 32, 16}) packed.add_dims(dim);
    auto& scales = *graph.add_initializer();
    scales.set_name(node.input(2));
    scales.set_data_type(onnx::TensorProto::FLOAT);
    scales.add_dims(n * k / 32);
    std::string bytes;
    for (int row = 0; row < n; ++row) {
      for (int block = 0; block < k / 32; ++block) {
        scales.add_float_data(static_cast<float>(1 + (row + seed + block) % 5) / 128);
        for (int byte = 0; byte < 16; ++byte) {
          const auto low = (row + block * 5 + byte * 3 + seed) % 16;
          const auto high = (row * 3 + block * 3 + byte * 5 + seed + 1) % 16;
          bytes.push_back(static_cast<char>(low | (high << 4)));
        }
      }
    }
    packed.set_raw_data(bytes);
  };
  projection("up", "x", "expanded", 32, 64, 2);
  auto& gelu = *graph.add_node();
  gelu.set_name("activation");
  gelu.set_op_type("Gelu");
  gelu.add_input("expanded");
  gelu.add_output("activated");
  projection("down", "activated", "y", 64, 3, 7);

  NativeOnnxSourceTransformV1 transform;
  transform.kind = "ffn-pair-partial";
  transform.axis = "ffn-channel";
  transform.node = "up";
  transform.downNode = "down";
  transform.input = "x";
  transform.output = "y";
  transform.weight = "up.weight";
  transform.downWeight = "down.weight";
  transform.extent = 64;
  std::vector<float> x(32);
  for (int i = 0; i < 32; ++i) x[i] = static_cast<float>((i * 9) % 17 - 8) / 16;
  const auto run = [&] (const auto& model) {
    return runOrt(model, {{"x", x}}, {{"x", {1, 1, 1, 32}}}, "y", {1, 1, 1, 3},
                  ORT_DISABLE_ALL);
  };
  const auto weight = [&] (int pair, int row, int channel) {
    const int blocks = pair == 0 ? 1 : 2;
    const int block = row * blocks + channel / 32;
    const auto& bytes = graph.initializer(pair * 2).raw_data();
    const auto packed = static_cast<unsigned char>(bytes.at(block * 16 + (channel % 32) / 2));
    const auto nibble = (packed >> ((channel % 2) * 4)) & 15;
    return (nibble - 8) * static_cast<double>(graph.initializer(pair * 2 + 1).float_data(block));
  };
  const auto full = run(source);
  BOOST_REQUIRE_EQUAL(full.size(), 3);
  std::array<double, 3> sum{};
  for (int rank = 0; rank < 2; ++rank) {
    transform.begin = rank * 32;
    transform.end = (rank + 1) * 32;
    const auto derived = applyNativeOnnxSourceTransform(source, transform);
    const auto& init = derived.graph().initializer();
    BOOST_CHECK_EQUAL(init.Get(0).raw_data(),
                      graph.initializer(0).raw_data().substr(rank * 32 * 16, 32 * 16));
    std::string expectedDown;
    for (int row = 0; row < 3; ++row) {
      const auto block = row * 2 + rank;
      expectedDown += graph.initializer(2).raw_data().substr(block * 16, 16);
    }
    BOOST_CHECK_EQUAL(init.Get(2).raw_data(), expectedDown);
    const auto partial = run(derived);
    BOOST_REQUIRE_EQUAL(partial.size(), sum.size());
    for (int out = 0; out < 3; ++out) {
      double expected = 0;
      for (int channel = rank * 32; channel < (rank + 1) * 32; ++channel) {
        double up = 0;
        for (int in = 0; in < 32; ++in) up += x[in] * weight(0, channel, in);
        expected += up * 0.5 * (1 + std::erf(up / std::sqrt(2))) * weight(1, out, channel);
      }
      BOOST_CHECK_SMALL(static_cast<double>(partial[out]) - expected, 1e-6);
      sum[out] += partial[out];
    }
  }
  for (std::size_t out = 0; out < sum.size(); ++out)
    BOOST_CHECK_SMALL(sum[out] - full[out], 1e-6);
  auto external = source;
  std::vector<std::uint8_t> sidecar;
  for (auto& tensor : *external.mutable_graph()->mutable_initializer()) {
    const auto offset = sidecar.size();
    if (tensor.data_type() == onnx::TensorProto::UINT8) {
      sidecar.insert(sidecar.end(), tensor.raw_data().begin(), tensor.raw_data().end());
    }
    else {
      for (float value : tensor.float_data()) {
        std::array<std::uint8_t, sizeof(value)> bytes;
        std::memcpy(bytes.data(), &value, bytes.size());
        sidecar.insert(sidecar.end(), bytes.begin(), bytes.end());
      }
    }
    tensor.clear_raw_data();
    tensor.clear_float_data();
    tensor.set_data_location(onnx::TensorProto::EXTERNAL);
    for (const auto& [key, value] : std::map<std::string, std::string>{
           {"location", "model.onnx.data"}, {"offset", std::to_string(offset)},
           {"length", std::to_string(sidecar.size() - offset)}}) {
      auto& field = *tensor.add_external_data();
      field.set_key(key);
      field.set_value(value);
    }
  }
  std::vector<std::uint8_t> result;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
  applyNativeOnnxSourceTransform(external, transform,
    [&] (const auto& path, std::uint64_t offset, std::uint64_t length) {
      BOOST_CHECK_EQUAL(path, "model.onnx.data");
      BOOST_REQUIRE_LE(offset, sidecar.size());
      BOOST_REQUIRE_LE(length, sidecar.size() - offset);
      reads.emplace_back(offset, length);
      return std::vector<std::uint8_t>(sidecar.begin() + offset, sidecar.begin() + offset + length);
    }, [&] (const auto& bytes) { result.insert(result.end(), bytes.begin(), bytes.end()); });
  const std::vector<std::pair<std::uint64_t, std::uint64_t>> expectedReads{
    {512, 512}, {1152, 128}, {1296, 16}, {1328, 16}, {1360, 16},
    {1380, 4}, {1388, 4}, {1396, 4}};
  BOOST_CHECK(reads == expectedReads);
  BOOST_CHECK_EQUAL(result.size(), 700);
  auto invalid = source;
  invalid.mutable_graph()->mutable_node(0)->add_input("zero-points");
  BOOST_CHECK_THROW(applyNativeOnnxSourceTransform(invalid, transform), std::invalid_argument);
  invalid = source;
  invalid.mutable_graph()->mutable_initializer(2)->mutable_raw_data()->pop_back();
  BOOST_CHECK_THROW(applyNativeOnnxSourceTransform(invalid, transform), std::exception);
  ++transform.begin;
  BOOST_CHECK_THROW(applyNativeOnnxSourceTransform(source, transform), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeOnnxSourceTransformShardsBothFfnProjections)
{
  onnx::ModelProto source;
  source.set_ir_version(9);
  source.add_opset_import()->set_version(20);
  auto& graph = *source.mutable_graph();
  graph.set_name("paired-ffn");
  const auto port = [] (onnx::ValueInfoProto& value, const std::string& name, int width) {
    value.set_name(name);
    auto* type = value.mutable_type()->mutable_tensor_type();
    type->set_elem_type(onnx::TensorProto::FLOAT);
    for (int dim : {1, 1, 1, width})
      type->mutable_shape()->add_dim()->set_dim_value(dim);
  };
  port(*graph.add_input(), "x", 3);
  port(*graph.add_output(), "y", 4);
  const auto weight = [&] (const std::string& name, const std::vector<int>& shape,
                           const std::vector<float>& values) {
    auto& tensor = *graph.add_initializer();
    tensor.set_name(name);
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    for (const auto dim : shape) tensor.add_dims(dim);
    for (const auto value : values) tensor.add_float_data(value);
  };
  std::vector<float> w1(3 * 7), w2(7 * 4), b1(7);
  for (int i = 0; i < 21; ++i) w1[i] = static_cast<float>(i - 10) / 16;
  for (int i = 0; i < 28; ++i) w2[i] = static_cast<float>((i * 11) % 23 - 11) / 16;
  for (int i = 0; i < 7; ++i) b1[i] = static_cast<float>(i - 3) / 8;
  weight("W1", {3, 7}, w1);
  weight("W2", {7, 4}, w2);
  weight("b1", {7}, b1);
  const auto op = [&] (const char* name, const char* opType,
                       std::initializer_list<const char*> inputs, const char* output) {
    auto* node = graph.add_node();
    node->set_name(name);
    node->set_op_type(opType);
    for (const auto input : inputs) node->add_input(input);
    node->add_output(output);
    return node;
  };
  op("up", "MatMul", {"x", "W1"}, "expanded");
  op("bias", "Add", {"expanded", "b1"}, "biased");
  auto* approximate = op("activation", "Gelu", {"biased"}, "activated")->add_attribute();
  approximate->set_name("approximate");
  approximate->set_type(onnx::AttributeProto::STRING);
  approximate->set_s("tanh");
  op("down", "MatMul", {"activated", "W2"}, "y");

  NativeOnnxSourceTransformV1 transform;
  transform.kind = "ffn-pair-partial";
  transform.axis = "ffn-channel";
  transform.node = "up";
  transform.downNode = "down";
  transform.input = "x";
  transform.output = "y";
  transform.weight = "W1";
  transform.downWeight = "W2";
  transform.bias = "b1";
  transform.extent = 7;
  const std::vector<float> x{0.5f, -0.25f, 0.75f};
  const auto run = [&] (const auto& model) {
    return runOrt(model, {{"x", x}}, {{"x", {1, 1, 1, 3}}}, "y", {1, 1, 1, 4},
                  ORT_DISABLE_ALL);
  };
  const auto full = run(source);
  std::array<double, 4> sum{};
  for (const auto& [begin, end] : nativeConvInputChannelRanges(7, 2)) {
    transform.begin = begin;
    transform.end = end;
    const auto admitted = NativeOnnxSourceTransformV1::parse(transform.canonicalJson());
    const auto derived = applyNativeOnnxSourceTransform(source, admitted);
    BOOST_CHECK_EQUAL(derived.graph().initializer(0).dims(1), end - begin);
    BOOST_CHECK_EQUAL(derived.graph().initializer(1).dims(0), end - begin);
    BOOST_CHECK_EQUAL(derived.graph().initializer(2).dims(0), end - begin);
    const auto partial = run(derived);
    BOOST_REQUIRE_EQUAL(partial.size(), sum.size());
    for (std::size_t out = 0; out < sum.size(); ++out) {
      double expected = 0;
      for (auto channel = begin; channel < end; ++channel) {
        double up = b1[channel];
        for (std::size_t in = 0; in < x.size(); ++in)
          up += static_cast<double>(x[in]) * w1[in * 7 + channel];
        const auto gelu = up * 0.5 * (1 + std::tanh(std::sqrt(2.0 / std::acos(-1.0)) *
                                                  (up + 0.044715 * up * up * up)));
        expected += gelu * w2[channel * 4 + out];
      }
      BOOST_CHECK_SMALL(static_cast<double>(partial[out]) - expected, 1e-6);
      sum[out] += partial[out];
    }
  }
  BOOST_REQUIRE_EQUAL(full.size(), sum.size());
  for (std::size_t out = 0; out < sum.size(); ++out)
    BOOST_CHECK_SMALL(sum[out] - full[out], 1e-6);

  auto externalGraph = source;
  auto& external = *externalGraph.mutable_graph();
  external.mutable_node()->DeleteSubrange(1, 1);
  external.mutable_node(1)->set_input(0, "expanded");
  external.mutable_initializer()->DeleteSubrange(2, 1);
  constexpr std::uint64_t EXTENT = 524296;
  constexpr auto HALF = EXTENT / 2;
  constexpr auto MATRIX1_BYTES = 3 * EXTENT * 4;
  const auto setStorage = [&] (int index, std::uint64_t start, std::uint64_t length) {
    auto& tensor = *external.mutable_initializer(index);
    tensor.clear_float_data();
    tensor.set_data_location(onnx::TensorProto::EXTERNAL);
    const auto set = [&] (const char* key, const std::string& value) {
      auto* field = tensor.add_external_data();
      field->set_key(key);
      field->set_value(value);
    };
    set("location", "model.onnx.data");
    set("offset", std::to_string(start));
    set("length", std::to_string(length));
  };
  external.mutable_initializer(0)->set_dims(1, EXTENT);
  external.mutable_initializer(1)->set_dims(0, EXTENT);
  setStorage(0, 0, MATRIX1_BYTES);
  setStorage(1, MATRIX1_BYTES, EXTENT * 4 * 4);
  auto externalTransform = transform;
  externalTransform.bias.clear();
  externalTransform.extent = EXTENT;
  externalTransform.begin = HALF;
  externalTransform.end = EXTENT;
  constexpr auto UP_BYTES = 3 * HALF * 4;
  std::uint64_t emitted = 0;
  std::uint64_t largestRead = 0;
  const auto streamed = applyNativeOnnxSourceTransform(externalGraph, externalTransform,
    [&] (const auto& path, std::uint64_t offset, std::uint64_t length) {
      BOOST_CHECK_EQUAL(path, "model.onnx.data");
      largestRead = std::max(largestRead, length);
      const auto expected = emitted < UP_BYTES
        ? (emitted / (HALF * 4) * EXTENT + HALF) * 4 + emitted % (HALF * 4)
        : MATRIX1_BYTES + HALF * 4 * 4 + emitted - UP_BYTES;
      BOOST_CHECK_EQUAL(offset, expected);
      return std::vector<std::uint8_t>(length, 11);
    }, [&] (const auto& bytes) { emitted += bytes.size(); });
  BOOST_CHECK_EQUAL(largestRead, 1U << 20);
  BOOST_CHECK_EQUAL(emitted, UP_BYTES + HALF * 4 * 4);
  BOOST_CHECK_EQUAL(streamed.graph().initializer(0).external_data(1).value(), "0");
  BOOST_CHECK_EQUAL(streamed.graph().initializer(1).external_data(1).value(),
                    std::to_string(UP_BYTES));

  auto wrongWeight = transform;
  wrongWeight.downWeight = "other";
  BOOST_CHECK_THROW(applyNativeOnnxSourceTransform(source, wrongWeight), std::invalid_argument);
  auto wrongAxis = transform;
  wrongAxis.axis = "input-channel";
  BOOST_CHECK_THROW(applyNativeOnnxSourceTransform(source, wrongAxis), std::invalid_argument);
  auto wrongRange = transform;
  wrongRange.begin = wrongRange.end;
  BOOST_CHECK_THROW(applyNativeOnnxSourceTransform(source, wrongRange), std::invalid_argument);
  auto unsupported = source;
  auto* foreignActivation = unsupported.mutable_graph()->mutable_node(2);
  foreignActivation->set_op_type("Relu");
  foreignActivation->clear_attribute();
  BOOST_CHECK_THROW(applyNativeOnnxSourceTransform(unsupported, transform), std::invalid_argument);
  auto openBoundary = source;
  *openBoundary.mutable_graph()->add_output() = graph.output(0);
  openBoundary.mutable_graph()->mutable_output(1)->set_name("activated");
  BOOST_CHECK_THROW(applyNativeOnnxSourceTransform(openBoundary, transform), std::invalid_argument);

  op("prefix", "Identity", {"x"}, "hidden");
  for (int i = graph.node_size() - 1; i > 0; --i)
    graph.mutable_node()->SwapElements(i, i - 1);
  graph.mutable_node(1)->set_input(0, "hidden");
  graph.mutable_node(4)->set_output(0, "partial");
  const std::vector<float> b2{0.125f, -0.25f, 0.5f, -0.625f};
  weight("b2", {4}, b2);
  op("suffix", "Add", {"partial", "b2"}, "y");
  const auto biasedFull = run(source);
  for (std::size_t out = 0; out < sum.size(); ++out)
    BOOST_CHECK_SMALL(sum[out] + b2[out] - biasedFull[out], 1e-6);

  NativeCanonicalSource canonicalSource;
  const auto modelWire = source.SerializeAsString();
  canonicalSource.modelBytes.assign(modelWire.begin(), modelWire.end());
  auto descriptor = model("fixture", "PairedFFN", digest("ffn-placeholder"));
  NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(30),
    [] {}, 4 * 1024 * 1024, 4 * 1024 * 1024};
  const auto inspected = inspectNativeOnnxSourceGraph(canonicalSource, descriptor, control);
  descriptor.graphDigest = inspected.graph.graphDigest;
  const auto identity = canonicalOnnxSourceIdentity(canonicalSource, control);
  const NativeJson configuration{
    {"schema", "ndnsf-di-native-request-catalog-v1"},
    {"legacy_yolo_retired", false},
    {"model", nativeParseJson(descriptor.canonicalJson())},
    {"source", {{"data_name", "/fixture/ffn"},
      {"digest", nativePlanningDigest(canonicalSource.modelBytes.data(),
                                      canonicalSource.modelBytes.size())},
      {"model_manifest_digest", digest("ffn-manifest")},
      {"canonical_graph_digest", identity.graphDigest}}},
    {"recipe", {{"artifact_profile_digest", digest("ffn-profile")},
      {"assembler_descriptor_digest", digest("ffn-assembler")}, {"backend_abi", "fixture-abi"},
      {"precision", descriptor.precision}, {"quantization", "none"}, {"layout", "NCHW"},
      {"padding", "none"}, {"protection_epoch", "epoch-1"}, {"max_source_bytes", 4194304},
      {"max_assembled_bytes", 4194304}, {"max_nodes", 100}}},
    {"publication", {{"artifact_root", "/fixture/ffn/artifacts"}}},
    {"input_format", "OPAQUE"}, {"max_payload_bytes", 4096},
    {"splitter", {{"kind", "ONNX_SOURCE"}, {"mode", "hybrid"},
      {"roles", {"Prefix", "Rank0", "Rank1", "Suffix"}},
      {"boundary_node", "onnx-node-1"}, {"ffn_down_node", "onnx-node-4"},
      {"input_ingress_role", "Prefix"}, {"result_egress_role", "Suffix"}}}
  };
  const auto loaded = NativeRequestCatalog::load(nativeCanonicalJson(configuration),
                                                  canonicalSource, control);
  BOOST_REQUIRE(loaded.splitter);
  const auto candidates = loaded.splitter->enumerate(loaded.model.descriptor, loaded.model.graph, {});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  const auto& candidate = candidates.front();
  BOOST_CHECK_EQUAL(candidate.executionPlan.stageLayout.size(), 3U);
  BOOST_CHECK_EQUAL(candidate.roleNodeIdsByRole.at("Rank0").size(), 4U);
  BOOST_CHECK(candidate.roleNodeIdsByRole.at("Rank0") == candidate.roleNodeIdsByRole.at("Rank1"));
  BOOST_REQUIRE_EQUAL(candidate.sourceTransformByRole.size(), 2U);
  const auto requirements = nativePlanRequiredCapabilities(candidate);
  BOOST_CHECK(std::find(requirements.begin(), requirements.end(),
                        NATIVE_FFN_PAIR_CAPABILITY_V1) != requirements.end());
  const auto pairs = nativeDistinctProviderPairs(candidate);
  BOOST_REQUIRE_EQUAL(pairs.size(), 1U);
  BOOST_CHECK_EQUAL(pairs.front().first, "Rank0");
  BOOST_CHECK_EQUAL(pairs.front().second, "Rank1");
  const auto rank0 = NativeOnnxSourceTransformV1::parse(candidate.sourceTransformByRole.at("Rank0"));
  const auto rank1 = NativeOnnxSourceTransformV1::parse(candidate.sourceTransformByRole.at("Rank1"));
  BOOST_CHECK_EQUAL(rank0.end, 4U);
  BOOST_CHECK_EQUAL(rank1.begin, 4U);
  BOOST_CHECK_EQUAL(rank1.end, 7U);
  BOOST_CHECK_EQUAL(*candidate.requirementsByRole.at("Rank0").weightBytes, 128U);
  BOOST_CHECK_EQUAL(*candidate.requirementsByRole.at("Rank1").weightBytes, 96U);
  BOOST_CHECK_EQUAL(*candidate.requirementsByRole.at("Suffix").weightBytes, 16U);
  const NativeRequestControl request{"/fixture/ffn/request", 1,
    std::chrono::steady_clock::now() + std::chrono::seconds(10), {}};
  const auto roles = loaded.preparation->prepareRoles(loaded.model, candidate, request);
  BOOST_REQUIRE_EQUAL(roles.size(), 4U);
  for (const auto& role : roles) BOOST_CHECK_NO_THROW(validateNativeAssembly(role));
  const auto plannedRank = std::find_if(roles.begin(), roles.end(), [] (const auto& role) {
    return role.selectedRole == "Rank1";
  });
  BOOST_REQUIRE(plannedRank != roles.end());
  BOOST_CHECK(!plannedRank->artifactCoordinate.empty());
  BOOST_CHECK_EQUAL(plannedRank->expectedInputs.at(0).name, "hidden");
  BOOST_CHECK_EQUAL(plannedRank->expectedOutputs.at(0).name, "partial");
  auto wrongRank = *plannedRank;
  wrongRank.tensorParallelRank = 0;
  BOOST_CHECK_THROW(validateNativeAssembly(wrongRank), std::invalid_argument);

  // Build a second source window with a different FFN width. The prefix result
  // feeds both the middle stage and a real suffix bypass.
  graph.mutable_initializer()->RemoveLast();
  weight("scale", {3}, {0.5f, 1.25f, -0.75f});
  graph.mutable_node(0)->set_op_type("Mul");
  graph.mutable_node(0)->add_input("scale");
  weight("W3", {4, 5}, std::vector<float>(20, 0.25f));
  weight("W4", {5, 3}, std::vector<float>(15, -0.125f));
  op("up-next", "MatMul", {"partial", "W3"}, "expanded-next");
  op("activation-next", "Gelu", {"expanded-next"}, "activated-next");
  op("down-next", "MatMul", {"activated-next", "W4"}, "next");
  for (int i = 5; i + 1 < graph.node_size(); ++i)
    graph.mutable_node()->SwapElements(i, i + 1);
  auto* suffix = graph.mutable_node(graph.node_size() - 1);
  suffix->set_input(0, "next");
  suffix->set_input(1, "hidden");
  graph.mutable_output(0)->mutable_type()->mutable_tensor_type()->mutable_shape()
    ->mutable_dim(3)->set_dim_value(3);

  const auto multiWire = source.SerializeAsString();
  canonicalSource.modelBytes.assign(multiWire.begin(), multiWire.end());
  const auto multiInspection = inspectNativeOnnxSourceGraph(canonicalSource, descriptor, control);
  descriptor.graphDigest = multiInspection.graph.graphDigest;
  auto multiConfig = configuration;
  multiConfig["model"] = nativeParseJson(descriptor.canonicalJson());
  multiConfig["source"]["digest"] = nativePlanningDigest(multiWire);
  multiConfig["source"]["canonical_graph_digest"] = multiInspection.canonicalIdentity.graphDigest;
  auto& split = multiConfig["splitter"];
  split.erase("boundary_node");
  split.erase("ffn_down_node");
  split["ffn_windows"] = {{{"up_node", "onnx-node-1"}, {"down_node", "onnx-node-4"}},
                           {{"up_node", "onnx-node-5"}, {"down_node", "onnx-node-7"}}};
  const auto multi = NativeRequestCatalog::load(nativeCanonicalJson(multiConfig),
                                                 canonicalSource, control);
  const auto multiCandidates = multi.splitter->enumerate(multi.model.descriptor, multi.model.graph, {});
  BOOST_REQUIRE_EQUAL(multiCandidates.size(), 1U);
  const auto& composed = multiCandidates.front();
  BOOST_REQUIRE_EQUAL(composed.executionPlan.stageLayout.size(), 3U);
  BOOST_CHECK_EQUAL(composed.executionPlan.stageLayout.at(1).degree(), 2U);
  BOOST_CHECK_EQUAL(composed.executionPlan.steps.size(), 9U);
  BOOST_CHECK_EQUAL(composed.executionPlan.dependencies.size(), 7U);
  BOOST_CHECK_EQUAL(*composed.requirementsByRole.at("Rank0").weightBytes, 212U);
  BOOST_CHECK_EQUAL(*composed.requirementsByRole.at("Rank1").weightBytes, 152U);
  const auto bypass = std::find_if(composed.executionPlan.dependencies.begin(),
    composed.executionPlan.dependencies.end(), [] (const auto& edge) {
      return edge.producerStepId == "Prefix/RUN" && edge.producerSlotId == "hidden" &&
             edge.consumerStepId == "Suffix/RUN" && edge.consumerSlotId == "hidden";
    });
  BOOST_REQUIRE(bypass != composed.executionPlan.dependencies.end());
  const auto nextCall = std::find_if(composed.executionPlan.steps.begin(),
    composed.executionPlan.steps.end(), [] (const auto& step) {
      return step.stepId == "Rank1/RUN/1";
    });
  BOOST_REQUIRE(nextCall != composed.executionPlan.steps.end());
  BOOST_CHECK_EQUAL(nextCall->inputSlots.front().producerStepId, "Rank1/SUM/0");
  const auto multiRoles = multi.preparation->prepareRoles(multi.model, composed, request);
  BOOST_REQUIRE_EQUAL(multiRoles.size(), 4U);
  const auto rejectsSourceProgram = [&] (auto candidate, const std::string& expected) {
    candidate.candidateDigest = candidate.computedDigest();
    BOOST_CHECK_EXCEPTION(multi.preparation->prepareRoles(multi.model, candidate, request),
      std::invalid_argument, [&] (const auto& error) { return error.what() == expected; });
  };
  const auto bypassIndex = std::distance(composed.executionPlan.dependencies.begin(), bypass);
  auto omitted = composed;
  omitted.executionPlan.dependencies.erase(omitted.executionPlan.dependencies.begin() + bypassIndex);
  auto& suffixInputs = omitted.executionPlan.steps.back().inputSlots;
  suffixInputs.erase(std::remove_if(suffixInputs.begin(), suffixInputs.end(), [] (const auto& slot) {
    return slot.slotId == "hidden";
  }), suffixInputs.end());
  rejectsSourceProgram(omitted, "source program outer CALL differs from its canonical frontier");
  auto wrongLineage = composed;
  auto& incoming = wrongLineage.executionPlan.steps.back().inputSlots;
  auto input = std::find_if(incoming.begin(), incoming.end(), [] (const auto& slot) {
    return slot.slotId == "hidden";
  });
  BOOST_REQUIRE(input != incoming.end());
  input->producerStepId = "Rank0/SUM/1";
  input->producerSlotId = "next";
  auto& wrongBypass = wrongLineage.executionPlan.dependencies.at(bypassIndex);
  wrongBypass.producers = {"Rank0"};
  wrongBypass.producerStepId = input->producerStepId;
  wrongBypass.producerSlotId = input->producerSlotId;
  wrongBypass.tensors = {input->producerSlotId};
  rejectsSourceProgram(wrongLineage, "source program outer CALL changes source tensor lineage");
  auto wrongTerminal = composed;
  wrongTerminal.resultEgressRole = "Rank0";
  rejectsSourceProgram(wrongTerminal, "source program terminal owner does not produce the model output");
  auto strippedRecipe = composed;
  strippedRecipe.sourceTransformByRole.clear();
  rejectsSourceProgram(strippedRecipe, "source program role is missing its entry recipe");
  for (const auto& role : multiRoles) {
    BOOST_TEST_CONTEXT(role.selectedRole) {
      validateNativeAssembly(role);
      if (role.selectedRole == "Rank0" || role.selectedRole == "Rank1") {
        const auto expected = "program-" + nativePlanningDigest(role.sourceTransformV1).substr(7);
        BOOST_CHECK(ndn::Name(role.artifactCoordinate)[-1] == ndn::name::Component(expected));
      }
    }
  }

  NativeCanonicalSource materials;
  materials.materialManifest = deriveNativeCanonicalMaterialManifest(canonicalSource, control);
  materials.materialPayloads = materials.materialManifest->payloads;
  const std::vector<float> hidden{x[0] * 0.5f, x[1] * 1.25f, x[2] * -0.75f};
  auto activation = hidden;
  for (std::size_t index = 0; index < 2; ++index) {
    std::vector<float> reduced;
    for (const auto& role : multiRoles) {
      if (role.sourceTransformV1.empty()) continue;
      const auto program = NativeOnnxSourceEntryProgramV1::parse(role.sourceTransformV1);
      const auto ordinals = program.bindSourceOrdinals(role.nodeIndices);
      const auto& entry = program.entries.at(index);
      const auto call = std::find_if(composed.executionPlan.steps.begin(),
        composed.executionPlan.steps.end(), [&] (const auto& step) {
          return step.unitId == role.selectedRole && step.artifactEntry == entry.entryId;
        });
      BOOST_REQUIRE(call != composed.executionPlan.steps.end());
      const auto files = materializeNativeOnnxEntrySource(materials, entry, ordinals.at(index),
        *call, control, [] (const auto&) { BOOST_FAIL("fixture uses inline initializers"); });
      onnx::ModelProto materialized;
      BOOST_REQUIRE(materialized.ParseFromArray(files.modelBytes.data(), files.modelBytes.size()));
      auto derived = lowerNativeMaterializedFfnEntry(std::move(materialized), entry.transform,
                                                      ordinals.at(index));
      const std::vector<std::int64_t> inShape{1, 1, 1, static_cast<std::int64_t>(activation.size())};
      const std::vector<std::int64_t> outShape{1, 1, 1, index == 0 ? 4 : 3};
      const auto partialResult = runOrt(derived, {{entry.transform.input, activation}},
        {{entry.transform.input, inShape}}, entry.transform.output, outShape, ORT_DISABLE_ALL);
      if (reduced.empty()) reduced.resize(partialResult.size());
      BOOST_REQUIRE_EQUAL(partialResult.size(), reduced.size());
      for (std::size_t i = 0; i < reduced.size(); ++i)
        reduced[i] += partialResult[i];

      if (index == 0) {
        auto missing = materials;
        missing.materialPayloads.erase(std::remove_if(missing.materialPayloads.begin(),
          missing.materialPayloads.end(), [] (const auto& payload) {
            return payload.payloadId == "node-1";
          }), missing.materialPayloads.end());
        BOOST_CHECK_EXCEPTION(materializeNativeOnnxEntrySource(missing, entry, ordinals.at(index),
          *call, control, {}), std::runtime_error, [] (const auto& error) {
            return std::string(error.what()) == "DI_CANONICAL_SOURCE_ENTRY_MATERIAL_MISSING";
          });
        auto misplaced = role;
        std::swap(misplaced.nodeIndices[0], misplaced.nodeIndices[1]);
        BOOST_CHECK_THROW(validateNativeAssembly(misplaced), std::invalid_argument);
      }
    }
    activation = std::move(reduced);
  }
  const auto fullMulti = runOrt(source, {{"x", x}}, {{"x", {1, 1, 1, 3}}}, "y", {1, 1, 1, 3},
                                ORT_DISABLE_ALL);
  BOOST_REQUIRE_EQUAL(activation.size(), fullMulti.size());
  BOOST_REQUIRE_EQUAL(activation.size(), hidden.size());
  for (std::size_t i = 0; i < activation.size(); ++i)
    BOOST_CHECK_SMALL(activation[i] + hidden[i] - fullMulti[i], 1e-6f);

  auto fullConfig = multiConfig;
  fullConfig["splitter"] = {{"kind", "ONNX_SOURCE"}, {"mode", "full"}, {"roles", {"Prefix"}},
    {"input_ingress_role", "Prefix"}, {"result_egress_role", "Prefix"}};
  const auto fullCatalog = NativeRequestCatalog::load(nativeCanonicalJson(fullConfig),
                                                        canonicalSource, control);
  const auto dense = fullCatalog.splitter->enumerate(fullCatalog.model.descriptor,
                                                     fullCatalog.model.graph, {});
  BOOST_REQUIRE_EQUAL(dense.size(), 1U);
  BOOST_CHECK_EQUAL(*dense.front().requirementsByRole.at("Prefix").weightBytes, 376U);
  BOOST_CHECK_NO_THROW(fullCatalog.preparation->prepareRoles(fullCatalog.model, dense.front(),
                                                            request));

  std::swap(split["ffn_windows"][0], split["ffn_windows"][1]);
  BOOST_CHECK_THROW(NativeRequestCatalog::load(nativeCanonicalJson(multiConfig),
                                              canonicalSource, control), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NativeOnnxSourceTransformPreservesExactConvAndResidual)
{
  auto source = loadConvFixture();
  // Every input channel is distinguishable, including the channels on both sides of the cut.
  std::vector<float> images(CONV_INPUT_CHANNELS * CONV_PLANE);
  for (int c = 0; c < CONV_INPUT_CHANNELS; ++c)
    std::fill_n(images.begin() + c * CONV_PLANE, CONV_PLANE, static_cast<float>(c + 1));
  for (auto& tensor : *source.mutable_graph()->mutable_initializer()) {
    tensor.clear_raw_data();
    tensor.clear_float_data();
    if (tensor.name() == "scale") {
      for (int c = 0; c < CONV_INPUT_CHANNELS; ++c)
        tensor.add_float_data(0.5F);
    }
    else if (tensor.name() == "w_conv") {
      for (int oc = 0; oc < CONV_OUTPUT_CHANNELS; ++oc) {
        const int sign = oc == 0 ? 1 : oc == 1 ? -1 : 0;
        for (int ic = 0; ic < CONV_INPUT_CHANNELS; ++ic)
          for (int k = 0; k < 9; ++k)
            tensor.add_float_data(k == 4 ? static_cast<float>(sign * (ic + 1)) : 0.0F);
      }
    }
    else {
      BOOST_REQUIRE_EQUAL(tensor.name(), "b_conv");
      for (int c = 0; c < CONV_OUTPUT_CHANNELS; ++c)
        tensor.add_float_data(0.5F);
    }
  }
  std::vector<float> residual(CONV_OUTPUT_CHANNELS * CONV_PLANE);
  std::array<std::vector<float>, 2> ranks;
  std::vector<float> sum;
  std::vector<float> expected;
  // 0.5 * sum(c*c) on channels 1..8 and 9..16, respectively.
  const std::array<float, 3> signs{1, -1, 0};
  for (std::size_t i = 0; i < residual.size(); ++i) {
    const auto sign = signs[std::min<std::size_t>(i / CONV_PLANE, 2)];
    residual[i] = i % 2 == 0 ? -1.0F : 2.0F;
    ranks[0].push_back(sign * 102.0F);
    ranks[1].push_back(sign * 646.0F);
    sum.push_back(sign * 748.0F);
    expected.push_back(std::max(0.0F, std::max(0.0F, sum.back() + 0.5F) + residual[i]));
  }
  const auto full = runOrt(source, {{"images", images}, {"residual", residual}},
    {{"images", CONV_INPUT_SHAPE}, {"residual", CONV_OUTPUT_SHAPE}},
    "predictions", CONV_OUTPUT_SHAPE, ORT_ENABLE_ALL);
  BOOST_CHECK(full == expected);
  for (const bool fusedBias : {false, true}) {
    const auto hybrid = runHybridConv(source, images, residual, ORT_ENABLE_ALL, fusedBias);
    BOOST_TEST_CONTEXT("synthesize bias " << fusedBias) {
      BOOST_CHECK(hybrid.ranks[0] == ranks[0]);
      BOOST_CHECK(hybrid.ranks[1] == ranks[1]);
      BOOST_CHECK(hybrid.sum == sum);
      BOOST_CHECK(hybrid.output == expected);
    }
  }
}

BOOST_AUTO_TEST_CASE(NativeOnnxSourceTransformMatchesTheFullConvModel)
{
  const auto source = loadConvFixture();
  const auto frozen = readNpyFloats(ndnsf::test::testFixturePath("spec261/conv-input.npy"));
  BOOST_REQUIRE_EQUAL(frozen.size(), (CONV_INPUT_CHANNELS + CONV_OUTPUT_CHANNELS) * CONV_PLANE);
  const auto inputEnd = frozen.begin() + CONV_INPUT_CHANNELS * CONV_PLANE;
  const std::vector<float> images(frozen.begin(), inputEnd);
  const std::vector<float> residual(inputEnd, frozen.end());
  const auto reference = referenceConvFp64(source, images, residual);
  BOOST_TEST_CONTEXT("frozen oracle") {
    const auto expected = readNpyFloats(ndnsf::test::testFixturePath("spec261/conv-oracle.npy"));
    checkConvFp32Error(expected, reference);
  }
  for (const auto optimization : {ORT_DISABLE_ALL, ORT_ENABLE_ALL}) {
    const auto full = runOrt(source, {{"images", images}, {"residual", residual}},
      {{"images", CONV_INPUT_SHAPE}, {"residual", CONV_OUTPUT_SHAPE}},
      "predictions", CONV_OUTPUT_SHAPE, optimization);
    const auto hybrid = runHybridConv(source, images, residual, optimization);
    BOOST_TEST_CONTEXT("full graph, ORT optimization " << optimization) {
      checkConvFp32Error(full, reference);
    }
    BOOST_TEST_CONTEXT("hybrid graph, ORT optimization " << optimization) {
      checkConvFp32Error(hybrid.output, reference);
    }
  }
}

BOOST_AUTO_TEST_CASE(NativeOnnxSourceTransformRejectsAnUnboundConv)
{
  std::ifstream file(ndnsf::test::testFixturePath("spec261/conv-single.onnx").string(),
                     std::ios::binary);
  BOOST_REQUIRE(file.good());
  std::string bytes((std::istreambuf_iterator<char>(file)), {});
  onnx::ModelProto source;
  BOOST_REQUIRE(source.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
  NativeOnnxSourceTransformV1 rank;
  rank.kind = "conv-input-channel-partial";
  rank.node = "conv0"; rank.axis = "input-channel";
  rank.input = "scaled"; rank.weight = "w_conv"; rank.output = "conv_out";
  rank.bias = "b_conv";
  rank.begin = 0; rank.end = 8; rank.extent = 16;
  rank.pads = {1, 1, 1, 1}; rank.strides = {1, 1}; rank.dilations = {1, 1};
  BOOST_CHECK_NO_THROW(applyNativeOnnxSourceTransform(source, rank));
  auto wrongWeight = rank; wrongWeight.weight = "other_weight";
  BOOST_CHECK_THROW(applyNativeOnnxSourceTransform(source, wrongWeight),
                    std::invalid_argument);
  auto droppedBias = rank; droppedBias.bias.clear();
  BOOST_CHECK_THROW(applyNativeOnnxSourceTransform(source, droppedBias),
                    std::invalid_argument);
  auto wrongStride = rank; wrongStride.strides = {2, 2};
  BOOST_CHECK_THROW(applyNativeOnnxSourceTransform(source, wrongStride),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(DetectionOracleRejectsChangedClassesAndPadding)
{
  const std::vector<float> expected{
    1.0F, 2.0F, 3.0F, 4.0F, 0.90F, 5.0F,
    10.0F, 20.0F, 30.0F, 40.0F, 0.50F, 7.0F,
    0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
  auto payloadOf = [] (const std::vector<float>& values,
                       const std::vector<std::int64_t>& shape) {
    ndnsf::di::NamedTensor tensor;
    tensor.name = "det";
    tensor.elementType = ndnsf::di::TensorElementType::Float32;
    tensor.shape = shape;
    tensor.payload.resize(values.size() * sizeof(float));
    std::memcpy(tensor.payload.data(), values.data(), tensor.payload.size());
    return ndnsf::di::encodeTensorBundle({tensor});
  };
  ndnsf::di::Result result;
  // Rows are permuted, and the effective values differ only by noise inside the
  // coordinate/score tolerances.
  const std::vector<float> permuted{
    10.0001F, 20.0001F, 30.0001F, 40.0001F, 0.50001F, 7.0F,
    1.0001F, 2.0001F, 3.0001F, 4.0001F, 0.90001F, 5.0F,
    0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
  result.payload = payloadOf(permuted, {1, 3, 6});
  BOOST_CHECK(result.matchesFloat32Detections("det", expected, 6, 0.01, 1e-4, 0.999));

  auto classChanged = permuted;
  classChanged[11] = 9.0F;
  result.payload = payloadOf(classChanged, {1, 3, 6});
  BOOST_CHECK(!result.matchesFloat32Detections("det", expected, 6, 0.01, 1e-4, 0.999));

  // Losing a real detection (its score becomes zero) must fail; leftover
  // coordinates on a zero-score row are padding and are covered separately.
  auto detectionLost = permuted;
  detectionLost[4] = 0.0F;
  result.payload = payloadOf(detectionLost, {1, 3, 6});
  BOOST_CHECK(!result.matchesFloat32Detections("det", expected, 6, 0.01, 1e-4, 0.999));

  auto shifted = permuted;
  shifted[6] += 0.5F;
  result.payload = payloadOf(shifted, {1, 3, 6});
  BOOST_CHECK(!result.matchesFloat32Detections("det", expected, 6, 0.01, 1e-4, 0.999));

  result.payload = payloadOf(permuted, {3, 6});
  BOOST_CHECK(!result.matchesFloat32Detections("det", expected, 6, 0.01, 1e-4, 0.999));

  // A zero-score row with leftover coordinates is padding, not a detection; it
  // must not be matched element-wise.
  auto zeroScoreTail = permuted;
  zeroScoreTail[12] = 123.0F;   // x on a row whose score stays zero
  zeroScoreTail[13] = -7.0F;
  result.payload = payloadOf(zeroScoreTail, {1, 3, 6});
  BOOST_CHECK(result.matchesFloat32Detections("det", expected, 6, 0.01, 1e-4, 0.999));

  // At the NMS max-det boundary two candidates with exactly the same score are
  // interchangeable; only their count is part of the contract.
  auto boundaryTie = permuted;
  boundaryTie[0] = 111.0F; boundaryTie[1] = 222.0F;
  boundaryTie[2] = 333.0F; boundaryTie[3] = 444.0F;
  boundaryTie[5] = 9.0F;
  result.payload = payloadOf(boundaryTie, {1, 3, 6});
  BOOST_CHECK(result.matchesFloat32Detections("det", expected, 6, 0.01, 1e-4, 0.999));

  auto boundaryCountLost = permuted;
  boundaryCountLost[10] = 0.0F;
  result.payload = payloadOf(boundaryCountLost, {1, 3, 6});
  BOOST_CHECK(!result.matchesFloat32Detections("det", expected, 6, 0.01, 1e-4, 0.999));
}

BOOST_AUTO_TEST_CASE(RequestCatalogRejectsRetiredLegacyYolo)
{
  std::ifstream file(ndnsf::test::testFixturePath("spec182/yolo-semantic-oracle.json").string());
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
  NativeJson configuration{
    {"schema", "ndnsf-di-native-request-catalog-v1"},
    {"model", nativeParseJson(descriptor.canonicalJson())},
    {"source", {{"data_name", "/fixture/source"},
      {"digest", nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size())},
      {"model_manifest_digest", digest("fixture-manifest")},
      {"canonical_graph_digest", identity.graphDigest}}},
    {"recipe", {{"artifact_profile_digest", digest("fixture-profile")},
      {"assembler_descriptor_digest", digest("fixture-assembler-v1")}, {"backend_abi", "fixture-abi"},
      {"precision", descriptor.precision}, {"quantization", "none"}, {"layout", "NCHW"},
      {"padding", "none"}, {"protection_epoch", "fixture-epoch"}, {"max_source_bytes", 1048576},
      {"max_assembled_bytes", 1048576}, {"max_nodes", 100}}},
    {"publication", {{"artifact_root", "/fixture/artifacts"}}},
    {"input_format", "JSON"}, {"max_payload_bytes", 32},
    {"splitter", {{"kind", "YOLO"}}}
  };
  // Retired by default.
  BOOST_CHECK_EXCEPTION(
    NativeRequestCatalog::load(nativeCanonicalJson(configuration), source, control),
    std::runtime_error, [] (const std::runtime_error& error) {
      return std::string(error.what()).find("DI_NATIVE_LEGACY_YOLO_SPLITTER_RETIRED") == 0;
    });
  // A historical comparison run may opt back in; it then fails on its own
  // missing components rather than on the retirement gate.
  NativeJson legacy = configuration;
  legacy["legacy_yolo_retired"] = false;
  BOOST_CHECK_EXCEPTION(
    NativeRequestCatalog::load(nativeCanonicalJson(legacy), source, control),
    std::exception, [] (const std::exception& error) {
      return std::string(error.what()).find("DI_NATIVE_LEGACY_YOLO_SPLITTER_RETIRED") ==
        std::string::npos;
    });
}

BOOST_AUTO_TEST_SUITE_END()
