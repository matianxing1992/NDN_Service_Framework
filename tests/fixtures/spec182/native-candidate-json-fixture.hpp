#ifndef SPEC182_NATIVE_CANDIDATE_JSON_FIXTURE_HPP
#define SPEC182_NATIVE_CANDIDATE_JSON_FIXTURE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

namespace ndnsf::di::fixture {

// Parse independent SDK oracle inputs into public native values. This is a
// test reader, not a runtime adapter, model inspector or expected hash builder.
inline NativeSplitCandidate candidateFromJson(const NativeJson& j)
{
  NativeSplitCandidate c;
  const auto& m = j.at("model");
  const auto& a = m.at("adapter");
  auto& adapter = c.model.adapter;
  adapter.name = a.at("name"); adapter.version = a.at("version");
  adapter.stateDigest = a.at("state_digest"); adapter.abi = a.at("abi");
  adapter.modelFormats = a.at("model_formats").get<std::vector<std::string>>();
  adapter.tasks = a.at("tasks").get<std::vector<std::string>>();
  adapter.backends = a.at("backends").get<std::vector<std::string>>();
  adapter.precisions = a.at("precisions").get<std::vector<std::string>>();
  adapter.inputSchemaDigest = a.at("input_schema_digest"); adapter.optionsSchemaDigest = a.at("options_schema_digest");
  adapter.resultSchemaDigest = a.at("result_schema_digest"); adapter.graphSchemaDigest = a.at("graph_schema_digest");
  adapter.splitSchemaDigest = a.at("split_schema_digest"); adapter.stateSchemaDigest = a.at("state_schema_digest");
  adapter.graphInspectable = a.at("graph_inspectable"); adapter.splittable = a.at("splittable");
  adapter.deterministicAnalysis = a.at("deterministic_analysis");
  c.model.modelName = m.at("model_name"); c.model.contentDigest = m.at("content_digest");
  c.model.semanticsDigest = m.at("semantics_digest"); c.model.graphDigest = m.at("graph_digest");
  c.model.modelFormat = m.at("model_format"); c.model.precision = m.at("precision");
  c.model.adapterId = adapter.name; c.model.adapterVersion = adapter.version;
  c.model.sourceRevision = m.at("source_revision");
  c.source = j.at("source"); c.graphDigest = j.at("graph_digest");
  const auto& s = j.at("splitter");
  c.splitter = {s.at("name"), s.at("version"), s.at("state_digest"), s.at("deterministic")};
  const auto& plan = j.at("execution_plan");
  c.executionPlan.roles = plan.at("roles").get<std::vector<std::string>>();
  c.nodeRoles = plan.at("node_roles").get<std::map<std::string, std::string>>();
  for (const auto& dep : plan.at("dependencies")) {
    NativeDependencySpec d;
    d.producers = {dep.at("producer")}; d.consumers = {dep.at("consumer")};
    d.tensors = dep.at("tensor_edges").get<std::vector<std::string>>();
    c.executionPlan.dependencies.push_back(std::move(d));
  }
  c.fragmentsByRole = j.at("fragments_by_role").get<decltype(c.fragmentsByRole)>();
  c.artifactsByRole = j.at("artifacts_by_role").get<decltype(c.artifactsByRole)>();
  for (const auto& row : j.at("requirements_by_role").items()) {
    const auto& value = row.value();
    const auto bytes = [&](const char* key) -> std::optional<std::uint64_t> {
      if (value.at(key).is_null()) return std::nullopt;
      return value.at(key).get<std::uint64_t>();
    };
    c.requirementsByRole[row.key()] = {value.at("backends").get<std::vector<std::string>>(),
      bytes("weight_bytes"), bytes("workspace_bytes"), bytes("kv_bytes"), bytes("activation_bytes"),
      bytes("transient_bytes"), value.at("safety_margin").get<double>()};
  }
  c.crossPartitionTensors = j.at("cross_partition_tensors").get<std::vector<std::string>>();
  c.tensorDegreesByRole = j.at("tensor_degrees_by_role").get<decltype(c.tensorDegreesByRole)>();
  c.rankArtifactDigestsByRole = j.at("rank_artifact_digests_by_role").get<decltype(c.rankArtifactDigestsByRole)>();
  for (const auto& row : j.at("estimated_costs").items()) {
    const auto& v = row.value();
    if (v.is_null()) c.estimatedCosts[row.key()] = std::monostate{};
    else if (v.is_number_unsigned()) c.estimatedCosts[row.key()] = v.get<std::uint64_t>();
    else if (v.is_number_integer()) c.estimatedCosts[row.key()] = v.get<std::int64_t>();
    else c.estimatedCosts[row.key()] = v.get<double>();
  }
  const auto states = [](const auto& values, auto& result) {
    for (const auto& row : values.items()) {
      for (const auto& t : row.value()) {
        NativeTensorContract tensor;
        tensor.name = t.at("name"); tensor.dtype = t.at("dtype");
        for (const auto& dim : t.at("shape")) {
          if (dim.is_string()) tensor.shape.push_back(dim.template get<std::string>());
          else tensor.shape.push_back(dim.template get<std::int64_t>());
        }
        if (!t.at("estimated_bytes").is_null()) tensor.estimatedBytes = t.at("estimated_bytes").template get<std::uint64_t>();
        result[row.key()].push_back(std::move(tensor));
      }
    }
  };
  states(j.at("role_state_inputs_by_role"), c.roleStateInputsByRole);
  states(j.at("role_state_outputs_by_role"), c.roleStateOutputsByRole);
  if (!j.at("hybrid_plan").is_null()) {
    const auto& h = j.at("hybrid_plan");
    NativeHybridPlan hybrid;
    hybrid.stages = h.at("stages");
    hybrid.tensorDegrees = h.at("tensor_degrees").get<std::vector<std::uint64_t>>();
    hybrid.rankLabels = h.at("rank_labels").get<std::vector<std::string>>();
    for (const auto& e : h.at("redistributions")) {
      RedistributionSpec edge;
      edge.producerRanks = e.at("producer_ranks").get<std::vector<std::uint64_t>>();
      edge.consumerRanks = e.at("consumer_ranks").get<std::vector<std::uint64_t>>();
      edge.tensor = e.at("tensor"); edge.operation = e.at("operation"); edge.epoch = e.at("epoch");
      edge.integrityDigest = e.at("integrity_digest"); edge.sourceLayoutDigest = e.at("source_layout_digest");
      edge.targetLayoutDigest = e.at("target_layout_digest"); edge.temporaryMemoryBytes = e.at("temporary_memory_bytes");
      edge.completeOutput = e.at("complete_output"); edge.axis = e.at("axis");
      hybrid.redistributions.push_back(std::move(edge));
    }
    c.hybridPlan = std::move(hybrid);
  }
  c.selectionPriority = j.at("selection_priority"); c.inputIngressRole = j.at("input_ingress_role");
  c.resultEgressRole = j.at("result_egress_role"); c.mergeKind = j.at("merge_kind");
  c.postprocessingJson = j.at("postprocessing").dump();
  return c;
}

} // namespace ndnsf::di::fixture
#endif
