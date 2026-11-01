#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalPreparationCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"

#include <cstring>
#include <fstream>
#include <iterator>

namespace ndnsf::di::test::spec204 {

/** Actual source inspection and production preparation, shared by both hosts. */
struct NativeCausalRecoveryModel
{
  NativeInspectedModel model;
  NativeSplitCandidate candidate;
  NativeStateTensorMapping stateMapping;
  std::shared_ptr<NativeCanonicalPreparationCatalog> catalog;
  std::vector<NativeSelectionRoleV3> roles;
  const std::string role = "/LLM/Pipeline/Stage/0";
  const std::string secondRole = "/LLM/Pipeline/Stage/1";

  explicit NativeCausalRecoveryModel(const std::string& producerPrefix,
                                    const std::string& protectionEpoch,
                                     const NativeRequestControl& control,
                                     bool twoRole = false)
  {
    NativeCanonicalSource source;
    const auto sourcePath = twoRole
      ? "tests/fixtures/spec204/two-role-causal-source.onnx"
      : "tests/fixtures/spec175/tiny-causal-lm-v1/one-role/role-0.onnx";
    std::ifstream input(sourcePath, std::ios::binary);
    if (!input) throw std::runtime_error("Spec204 requires actual Spec175 causal ONNX source");
    source.modelBytes.assign(std::istreambuf_iterator<char>(input), {});
    const auto sourceDigest = nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size());
    const auto expectedSourceDigest = twoRole
      ? "sha256:b44b8adf8359f179a6bbcd55a5d208bc960b1c75837d061c89adde0def0b362b"
      : "sha256:fe41db5c2c8397b62fc8983307b0d755706b7008c56958882cb247ca46594f82";
    if (sourceDigest != expectedSourceDigest)
      throw std::runtime_error("Spec204 causal ONNX source differs from pinned fixture");
    model.descriptor = fixture::completeModel({twoRole ? "Spec212TwoRoleTinyCausal" : "Spec204TinyCausal", sourceDigest,
      nativePlanningDigest(twoRole ? "Spec175-two-role-causal-semantics-v1" : "Spec175-causal-semantics-v1"), nativePlanningDigest("inspection-pending"),
      "onnx", "fp32", "spec204-causal-fixture", "1"});
    model.canonicalSourceName = producerPrefix + "/NDNSF/DI/SOURCE/model";
    model.canonicalSourceDigest = sourceDigest;
    model.canonicalSourceBytes = source.modelBytes.size();
    model.modelManifestDigest = nativePlanningDigest("Spec204-pinned-Spec175-manifest-v1");
    NativeAssemblyControl assembly{control.deadline, [&] { control.requireActive(); }, 1U << 20, 1U << 20};
    auto inspection = inspectNativeOnnxSourceGraph(source, model.descriptor, assembly);
    model.graph = inspection.graph;
    model.descriptor.graphDigest = model.graph.graphDigest;
    model.canonicalGraphDigest = inspection.canonicalIdentity.graphDigest;
    model.validate();

    candidate.source = "PRE_SPLIT";
    candidate.splitter = {"spec204-actual-onnx", "1",
      nativePlanningDigest(twoRole ? "Spec212-two-role-source-split" : "Spec204-one-role-source-split")};
    candidate.model = model.descriptor; candidate.graphDigest = model.graph.graphDigest;
    candidate.executionPlan.modelName = model.descriptor.modelName;
    candidate.executionPlan.roles = twoRole ? std::vector<std::string>{role, secondRole}
                                             : std::vector<std::string>{role};
    if (twoRole) {
      candidate.executionPlan.dependencies = {NativeDependencySpec{
        {role}, {secondRole}, "spec212-hidden-state", "/LLM/Pipeline/Stage",
        "/LLM/Pipeline/Stage/{producerRole}/{sequence}", 1, 64, {"hidden_out"}}};
      candidate.crossPartitionTensors = {"hidden_out"};
    }
    candidate.tensorDegreesByRole = {{role, 1}};
    if (twoRole) candidate.tensorDegreesByRole.emplace(secondRole, 1);
    candidate.inputIngressRole = role;
    candidate.resultEgressRole = twoRole ? secondRole : role;
    candidate.artifactsByRole = {{role, {sourceDigest}}};
    if (twoRole) candidate.artifactsByRole.emplace(secondRole, std::vector<std::string>{sourceDigest});
    candidate.rankArtifactDigestsByRole = candidate.artifactsByRole;
    candidate.fragmentsByRole = {{role, sourceDigest}};
    candidate.requirementsByRole = {{role, {{"onnxruntime"}, 1, 0, 0, 0, 0, 1.0}}};
    if (twoRole) {
      candidate.fragmentsByRole.emplace(secondRole, sourceDigest);
      candidate.requirementsByRole.emplace(secondRole, candidate.requirementsByRole.at(role));
    }
    NativeCanonicalRolePreparer::NodeMap nodes;
    for (const auto& node : model.graph.nodes) {
      const auto owner = twoRole && node.ordinal >= 21 ? secondRole : role;
      candidate.nodeRoles[node.id] = owner;
      nodes[node.id] = {node.ordinal};
    }
    for (const auto& roleName : candidate.executionPlan.roles) {
      const auto prefix = roleName == secondRole ? "stage1__" : "";
      for (const std::string family : {"attention_kv", "recurrent_state", "convolution_state"}) {
        const auto inputName = prefix + family + "_in";
        const auto outputName = prefix + family + "_out";
        candidate.roleStateInputsByRole[roleName].push_back(
          {family + "_in", "float32", {std::string("source-bound")}, std::nullopt});
        candidate.roleStateOutputsByRole[roleName].push_back(
          {family + "_out", "float32", {std::string("source-bound")}, std::nullopt});
        stateMapping.inputs[roleName][family + "_in"] = {inputName};
        stateMapping.outputs[roleName][family + "_out"] = {outputName};
      }
    }
    candidate.candidateDigest = candidate.computedDigest();
    NativeCanonicalCatalogEntry entry;
    entry.model = model; entry.source = std::move(source); entry.nodes = std::move(nodes);
    entry.sourceGraphInspection = std::move(inspection);
    entry.recipe = {nativePlanningDigest("Spec204-causal-profile"),
      nativePlanningDigest("Spec204-production-onnx-assembler"), "onnxruntime-cpu-v1",
      "fp32", "none", "native", "none", protectionEpoch, 1U << 20, 1U << 20, 1024};
    entry.publication = {producerPrefix + "/NDNSF/DI/ARTIFACT", model.modelManifestDigest, {sourceDigest}};
    entry.publication.artifactProfileDigest = entry.recipe.artifactProfileDigest;
    entry.maxPayloadBytes = 1U << 20;
    entry.conversationTokenEncoder = [](const std::vector<std::uint8_t>& bytes) {
      const auto tensors = decodeTensorBundle(bytes);
      const auto& ids = findTensor(tensors, "input_ids");
      if (ids.elementType != TensorElementType::Int64 || ids.payload.empty() ||
          ids.payload.size() % sizeof(std::int64_t) != 0)
        throw std::invalid_argument("Spec204 causal input must contain int64 tokens");
      std::vector<std::int64_t> tokens(ids.payload.size() / sizeof(std::int64_t));
      std::memcpy(tokens.data(), ids.payload.data(), ids.payload.size());
      return tokens;
    };
    catalog = std::make_shared<NativeCanonicalPreparationCatalog>(
      std::vector<NativeCanonicalCatalogEntry>{std::move(entry)}, assembly);
    // Keep the splitter candidate semantic: the request planner binds it once
    // through this source catalog. Use a separate bound copy for role fixtures.
    const auto sourceBoundCandidate = catalog->bindStateContracts(model, candidate, stateMapping, control);
    roles = catalog->prepareRoles(model, sourceBoundCandidate, control);
    if (roles.size() != sourceBoundCandidate.executionPlan.roles.size() ||
        roles.front().expectedInputs.size() != 4 ||
        sourceBoundCandidate.roleStateInputsByRole.at(role).size() != 3 ||
        (twoRole && (roles.back().expectedInputs.size() != 4 ||
                     sourceBoundCandidate.roleStateInputsByRole.at(secondRole).size() != 3)))
      throw std::runtime_error("Spec204 actual causal preparation omitted state families");
  }
};

} // namespace ndnsf::di::test::spec204
