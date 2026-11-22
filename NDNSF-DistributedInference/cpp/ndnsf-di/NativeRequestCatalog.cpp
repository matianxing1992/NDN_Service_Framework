#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-rank-transfers.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeSourcePartitionStrategy.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/prepared-onnx-metadata.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-split.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeQwenPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-split.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-preparation.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/FixedAssignmentWork.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativePhaseTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-state-role-mapping.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-step-template-json.hpp"

#include <onnx/onnx_pb.h>

#include <algorithm>
#include <cfenv>
#include <cstring>
#include <limits>
#include <locale>
#include <set>
#include <string_view>

namespace ndnsf::di {
namespace {
enum class CatalogRoute { QWEN, LLAMA, YOLO, ONNX_SOURCE, AUTOMATIC };

CatalogRoute
parseCatalogRoute(std::string_view kind)
{
  if (kind == "QWEN") return CatalogRoute::QWEN;
  if (kind == "LLAMA") return CatalogRoute::LLAMA;
  if (kind == "YOLO") return CatalogRoute::YOLO;
  if (kind == "ONNX_SOURCE") return CatalogRoute::ONNX_SOURCE;
  if (kind == "AUTOMATIC") return CatalogRoute::AUTOMATIC;
  throw std::invalid_argument("unsupported native request splitter");
}

std::map<std::string, NativeRoleResourceRequirement>
resourceProfilesFromCatalog(const NativeJson& split, const std::vector<std::string>& roles,
                            const std::vector<std::string>& allowedBackends)
{
  const auto profiles = split.find("resources_by_role");
  if (roles.empty() || std::set<std::string>(roles.begin(), roles.end()).size() != roles.size() ||
      profiles == split.end() || !profiles->is_object() || profiles->size() != roles.size())
    throw std::invalid_argument("native transformer requires explicit per-role resource profiles");
  std::map<std::string, NativeRoleResourceRequirement> result;
  for (const auto& role : roles) {
    if (role.empty()) throw std::invalid_argument("native role resource identity is empty");
    const auto entry = profiles->find(role);
    if (entry == profiles->end() || !entry->is_object() || entry->size() != 7 ||
        !entry->contains("backends") || !entry->at("backends").is_array() ||
        !entry->contains("safety_margin") || !entry->at("safety_margin").is_number())
      throw std::invalid_argument("native role resource profile is incomplete");
    NativeRoleResourceRequirement profile;
    for (const auto& backend : entry->at("backends")) {
      if (!backend.is_string())
        throw std::invalid_argument("native role resource backend must be a string");
      const auto name = backend.get<std::string>();
      if (name.empty() || std::find(allowedBackends.begin(), allowedBackends.end(), name) ==
                            allowedBackends.end() ||
          std::find(profile.backends.begin(), profile.backends.end(), name) != profile.backends.end())
        throw std::invalid_argument("native role resource backend is unsupported or repeated");
      profile.backends.push_back(name);
    }
    const auto bytes = [&](const char* key) {
      const auto value = entry->find(key);
      if (value == entry->end() || !value->is_number_unsigned())
        throw std::invalid_argument("native role resource budget is unknown or not uint64");
      return value->get<std::uint64_t>();
    };
    profile.weightBytes = bytes("weight_bytes");
    profile.workspaceBytes = bytes("workspace_bytes");
    profile.kvBytes = bytes("kv_bytes");
    profile.activationBytes = bytes("activation_bytes");
    profile.transientBytes = bytes("transient_bytes");
    profile.safetyMargin = entry->at("safety_margin").get<double>();
    if (!profile.estimatedPeakGpuMemoryBytes())
      throw std::invalid_argument("native role resource peak is unknown");
    result.emplace(role, std::move(profile));
  }
  return result;
}


void
buildNativeSourceBlockEntries(NativeSourcePartitionProfile& profile,
                              const NativeJson& attention,
                              const NativeOnnxGraphInspection& inspection,
                              const NativeGraphSnapshot& snapshot, const NativeJson& metadata,
                              const onnx::ModelProto& original,
                              const NativeAssemblyControl& control)
{
  const auto& order = snapshot.topologicalOrder;
  const auto& graph = original.graph();
  const auto ordinal = [&] (const std::string& id) -> std::uint64_t {
    const auto found = inspection.canonicalNodeIndices.find(id);
    if (found == inspection.canonicalNodeIndices.end() ||
        found->second >= static_cast<std::uint64_t>(graph.node_size()) ||
        order.at(found->second) != id)
      throw std::invalid_argument("source block anchor is not a canonical node: " + id);
    return found->second;
  };
  const auto first = ordinal(profile.boundaryNode);
  const auto last = ordinal(profile.suffixNode);
  if (first == 0 || first >= last || attention.size() != 2 * profile.ffnWindows.size())
    throw std::invalid_argument("source block program requires two ordered attentions per FFN");
  const auto& tensors = metadata.at("tensors");
  const auto& uses = inspection.initializerConsumers;
  const auto region = [&] (std::uint64_t begin, std::uint64_t end) {
    if (begin >= end || end > order.size())
      throw std::invalid_argument("source block window has invalid bounds");
    NativeSourceBlockEntry result;
    result.recipe.sourceNodes.assign(order.begin() + begin, order.begin() + end);
    std::set<std::uint64_t> indices;
    for (auto i = begin; i < end; ++i) indices.insert(i);
    const auto frontier = inspectNativeSourceBoundary(snapshot, metadata, indices, {},
                                                      control.requireActive);
    const auto append = [&] (const auto& ports, auto& destination) {
      for (const auto& [name, port] : ports) {
        const auto& shape = port.shape;
        const auto bytes = tensors.at(name).at("sizeBytes");
        if (port.dtype != "float32" || !bytes.is_number_unsigned() || shape.empty() ||
            std::any_of(shape.begin(), shape.end(), [] (const auto& dim) {
              const auto* size = std::get_if<std::int64_t>(&dim);
              return size == nullptr || *size <= 0;
            }))
          throw std::invalid_argument("source block frontier requires exact FP32 shapes: " + name);
        destination.push_back({name, port.dtype, shape, bytes.template get<std::uint64_t>()});
      }
    };
    append(frontier.inputs, result.inputs);
    append(frontier.outputs, result.outputs);
    for (const auto& [name, consumers] : uses) {
      control.requireActive();
      if (std::none_of(consumers.begin(), consumers.end(), [&] (auto node) {
            return node >= begin && node < end;
          }))
        continue;
      const auto bytes = tensors.at(name).at("sizeBytes").get<std::uint64_t>();
      if (bytes > std::numeric_limits<std::uint64_t>::max() - result.initializerBytes)
        throw std::overflow_error("source block initializer memory exceeds uint64");
      result.initializerBytes += bytes;
    }
    return result;
  };
  auto cursor = first;
  const auto plainUntil = [&] (std::uint64_t end) {
    if (end < cursor)
      throw std::invalid_argument("source block declarations overlap or are out of order");
    if (cursor == end) return;
    // Each selected projection must be admitted through a named TP region.
    for (auto i = cursor; i < end; ++i) {
      const auto& node = graph.node(static_cast<int>(i));
      if (node.op_type() == "MatMul" || node.op_type() == "MatMulNBits" ||
          node.op_type() == "Gemm" || node.op_type() == "Attention" ||
          node.op_type() == "MultiHeadAttention")
        throw std::invalid_argument("unassigned projection in the source block window");
    }
    auto plain = region(cursor, end);
    plain.recipe.transform.kind = "attention-source-component";
    plain.recipe.transform.node = order.at(cursor);
    profile.blockEntries.push_back(std::move(plain));
    cursor = end;
  };
  for (std::size_t block = 0; block < profile.ffnWindows.size(); ++block) {
    control.requireActive();
    for (std::size_t branch = 0; branch < 2; ++branch) {
      const auto& entry = attention.at(2 * block + branch);
      if (!entry.is_object() || entry.size() != 7 ||
          entry.at("kind") != (branch == 0 ? "self" : "cross"))
        throw std::invalid_argument("source block requires ordered self and cross attention");
      const auto anchor = [&] (const auto* key) {
        return ordinal(entry.at(key).template get<std::string>());
      };
      const auto q = anchor("q"), k = anchor("k"), v = anchor("v");
      const auto qMean = anchor("q_stat"), kMean = anchor("k_stat"), o = anchor("out");
      if (!(q < k && k < v && v + 1 < qMean && qMean + 6 == kMean && kMean + 5 < o))
        throw std::invalid_argument("source attention anchors are not in canonical order");
      const auto& qNode = graph.node(static_cast<int>(q));
      const auto& kNode = graph.node(static_cast<int>(k));
      const auto& vNode = graph.node(static_cast<int>(v));
      const auto& oNode = graph.node(static_cast<int>(o));
      const auto qDims = inspectNativeOnnxLinearProjection(original, qNode);
      const auto kDims = inspectNativeOnnxLinearProjection(original, kNode);
      const auto vDims = inspectNativeOnnxLinearProjection(original, vNode);
      const auto oDims = inspectNativeOnnxLinearProjection(original, oNode);
      if (qDims.n != kDims.n || qDims.n != vDims.n || qDims.n != oDims.k ||
          (qNode.input(0) == kNode.input(0)) != (branch == 0) ||
          kNode.input(0) != vNode.input(0))
        throw std::invalid_argument("source attention projections do not share their declared geometry");
      plainUntil(q);
      auto projections = region(q, qMean - 1);
      auto qNorm = region(qMean - 1, kMean - 1);
      auto kNorm = region(kMean - 1, kMean + 5);
      auto tail = region(kMean + 5, o + 1);
      if (projections.outputs.size() != 3 || qNorm.inputs.size() != 1 ||
          qNorm.outputs.size() != 1 || kNorm.inputs.size() != 1 || kNorm.outputs.size() != 1 ||
          tail.outputs.size() != 1 || tail.outputs.front().name != oNode.output(0))
        throw std::invalid_argument("source attention window has unexpected original ports");
      const auto& qInput = qNorm.inputs.front().name;
      const auto& kInput = kNorm.inputs.front().name;
      const auto& qOutput = qNorm.outputs.front().name;
      const auto& kOutput = kNorm.outputs.front().name;
      const auto vInput = std::find_if(projections.outputs.begin(), projections.outputs.end(),
        [&] (const auto& port) { return port.name != qInput && port.name != kInput; });
      std::vector<std::int64_t> geometry;
      for (auto i = kMean + 5; i < o; ++i) {
        const auto& node = graph.node(static_cast<int>(i));
        if (node.op_type() == "Reshape" && node.domain().empty() && node.input_size() == 2 &&
            node.output_size() == 1 && node.input(0) == qOutput)
          geometry = tensors.at(node.output(0)).at("shape").get<std::vector<std::int64_t>>();
      }
      const auto projectionPort = [&] (const auto& projection, const std::string& target) {
        if (projection.output(0) == target) return true;
        const auto producer = metadata.at("tensorProducers").at(target).get<std::uint64_t>();
        const auto& add = graph.node(static_cast<int>(producer));
        return producer >= q && producer < qMean - 1 && add.op_type() == "Add" &&
               add.domain().empty() && add.input_size() == 2 &&
               std::find(add.input().begin(), add.input().end(), projection.output(0)) !=
                 add.input().end();
      };
      if (vInput == projections.outputs.end() || !projectionPort(qNode, qInput) ||
          !projectionPort(kNode, kInput) || !projectionPort(vNode, vInput->name) ||
          geometry.size() != 4 || geometry[2] < 2 || geometry[3] <= 0 ||
          geometry[2] > qDims.n || qDims.n % geometry[2] != 0 ||
          qDims.n / geometry[2] != geometry[3])
        throw std::invalid_argument("source attention head layout or projection ports differ");
      const auto heads = static_cast<std::uint64_t>(geometry[2]);
      const auto width = static_cast<std::uint64_t>(geometry[3]);
      const auto split = nativeConvInputChannelRanges(heads, 2).front().second;
      if (oDims.packed && split * width % NativeOnnxLinearProjection::BLOCK != 0)
        throw std::invalid_argument("source attention output rank splits a packed K block");
      const auto bind = [&] (NativeSourceBlockEntry& window, const char* kind) -> auto& {
        auto& transform = window.recipe.transform;
        transform.kind = kind;
        transform.node = window.recipe.sourceNodes.front();
        transform.axis = "attention-head";
        transform.end = split;
        transform.extent = heads;
        transform.headWidth = width;
        return transform;
      };
      bind(projections, "attention-qkv").attentionPorts = {qInput, kInput, vInput->name};
      bind(tail, "attention-heads").attentionPorts = {qOutput, kOutput, vInput->name};
      // The order of the Q/K/V projection graph outputs is part of the sealed ABI.
      const auto projectionOrder = projections.recipe.transform.attentionPorts;
      std::sort(projections.outputs.begin(), projections.outputs.end(),
        [&] (const auto& a, const auto& b) {
          return std::find(projectionOrder.begin(), projectionOrder.end(), a.name) <
                 std::find(projectionOrder.begin(), projectionOrder.end(), b.name);
        });
      profile.blockEntries.push_back(std::move(projections));
      const auto emitNorm = [&] (NativeSourceBlockEntry window, std::uint64_t mean) {
        const auto& node = graph.node(static_cast<int>(mean));
        if (node.op_type() != "ReduceMean" || node.output_size() != 1)
          throw std::invalid_argument("source attention RMS statistic is not ReduceMean");
        auto& transform = bind(window, "attention-rms-stat");
        transform.input = window.inputs.front().name;
        transform.output = window.outputs.front().name;
        transform.statistic = node.output(0);
        transform.globalStatistic = transform.statistic + "/global";
        if (tensors.contains(transform.globalStatistic) ||
            window.initializerBytes > std::numeric_limits<std::uint64_t>::max() - sizeof(float))
          throw std::invalid_argument("source attention RMS synthetic port or memory collides");
        window.initializerBytes += sizeof(float);
        profile.blockEntries.push_back(window);
        transform.kind = "attention-rms-normalize";
        profile.blockEntries.push_back(std::move(window));
      };
      emitNorm(std::move(qNorm), qMean);
      emitNorm(std::move(kNorm), kMean);
      profile.blockEntries.push_back(std::move(tail));
      cursor = o + 1;
    }
    const auto& ffn = profile.ffnWindows[block];
    const auto up = ordinal(ffn.transform.node);
    const auto down = ordinal(ffn.transform.downNode);
    plainUntil(up);
    auto window = region(up, down + 1);
    if (window.inputs.size() != 1 || window.inputs.front().name != ffn.transform.input ||
        window.outputs.size() != 1 || window.outputs.front().name != ffn.transform.output)
      throw std::invalid_argument("source block FFN has extra original ports");
    window.recipe.transform = ffn.transform;
    profile.blockEntries.push_back(std::move(window));
    cursor = down + 1;
  }
  if (cursor >= last)
    throw std::invalid_argument("source block has no final post-FFN operations");
  plainUntil(last);
  profile.outerEntries = {region(0, first), region(last, order.size())};
  const auto qkv = std::find_if(profile.blockEntries.begin(), profile.blockEntries.end(),
    [] (const auto& entry) { return entry.recipe.transform.kind == "attention-qkv"; });
  profile.inputChannels = qkv == profile.blockEntries.end() ? 0 : qkv->recipe.transform.extent;
  if (profile.inputChannels == 0)
    throw std::invalid_argument("source block has no admitted head geometry");
  profile.convInputAxis = "attention-head";
  profile.denseWeightBytesByRole[profile.roles.front()] = profile.outerEntries.front().initializerBytes;
  profile.denseWeightBytesByRole[profile.roles.back()] = profile.outerEntries.back().initializerBytes;
}

} // namespace

NativeStateTensorMapping
detail::remapNativeStateRoles(const NativeStateTensorMapping& source,
                             const std::map<std::string, std::string>& sourceRoleByUnit)
{
  if (sourceRoleByUnit.empty())
    throw std::invalid_argument("native state role expansion has no units");
  NativeStateTensorMapping result;
  for (const auto& [unit, sourceRole] : sourceRoleByUnit) {
    const auto inputs = source.inputs.find(sourceRole);
    const auto outputs = source.outputs.find(sourceRole);
    if (unit.empty() || sourceRole.empty() || inputs == source.inputs.end() ||
        outputs == source.outputs.end() ||
        inputs->second.empty() != outputs->second.empty())
      throw std::invalid_argument("native state role expansion omits a source state contract");
    result.inputs.emplace(unit, inputs->second);
    result.outputs.emplace(unit, outputs->second);
  }
  return result;
}

NativePreparedPlanningTemplate::NativePreparedPlanningTemplate(
  std::shared_ptr<const NativeInspectedModel> model,
  std::shared_ptr<const CooperativeModelSplitStrategy> splitter,
  std::vector<NativePreparedPlanningCandidate> candidates)
  : m_model(std::move(model)), m_splitter(std::move(splitter)),
    m_candidates(std::move(candidates)), m_retainedBytes(sizeof(*this))
{
  if (!m_model || !m_splitter || m_candidates.empty())
    throw std::invalid_argument("prepared planning template is incomplete");
  const auto charge = [this](std::size_t bytes) {
    // Include both the compatibility copy and owned static facts, plus
    // conservative parser/container overhead, in the existing package budget.
    if (bytes > (std::numeric_limits<std::size_t>::max() - m_retainedBytes) / 8)
      throw std::runtime_error("DI_NATIVE_PREPARATION_SIZE_OVERFLOW");
    m_retainedBytes += bytes * 8;
  };
  m_execution.reserve(m_candidates.size());
  m_roleJson.reserve(m_candidates.size());
  m_snapshotFixedCanonical.reserve(m_candidates.size());
  for (const auto& prepared : m_candidates) {
    NativeRequestPreparation::validateRoles(*m_model, prepared.candidate, prepared.roles);
    charge(prepared.candidate.canonicalJson().size());
    auto execution = prepared.candidate.executionPlan;
    execution.roles.clear();
    std::set<std::string> selected;
    std::map<std::string, std::vector<std::string>> selectedByStage;
    NativeJson roleValues = NativeJson::array();
    for (const auto& role : prepared.roles) {
      if (role.selectedRole.empty() || !selected.insert(role.selectedRole).second)
        throw std::invalid_argument("prepared planning template role cover is invalid");
      auto roleValue = nativeAssemblyJson(role);
      const auto roleBytes = nativeCanonicalJson(roleValue).size();
      charge(roleBytes);
      charge(roleBytes); // retained JSON fields in addition to typed roles
      roleValues.push_back(std::move(roleValue));
      execution.roles.push_back(role.selectedRole);
      selectedByStage[role.role].push_back(role.selectedRole);
    }
    m_roleJson.push_back(std::move(roleValues));
    std::map<std::string, std::string> fixedCanonical;
    if (std::fegetround() == FE_TONEAREST && std::locale() == std::locale::classic()) {
      const NativeJson fixedFields{
        {"candidate_digest", prepared.candidate.candidateDigest},
        {"graph_digest", prepared.candidate.graphDigest},
        {"node_roles", prepared.candidate.nodeRoles},
        {"roles", m_roleJson.back()},
        {"stage_order", prepared.candidate.executionPlan.roles}};
      for (auto field = fixedFields.begin(); field != fixedFields.end(); ++field) {
        ++detail::fixedAssignmentWork().snapshotFixedEncodes;
        auto bytes = nativeCanonicalJson(field.value());
        charge(bytes.size());
        fixedCanonical.emplace(field.key(), std::move(bytes));
      }
    }
    m_snapshotFixedCanonical.push_back(std::move(fixedCanonical));
    const auto expand = [&selectedByStage](const std::vector<std::string>& stages) {
      std::vector<std::string> result;
      for (const auto& stage : stages) {
        const auto& roles = selectedByStage.at(stage);
        result.insert(result.end(), roles.begin(), roles.end());
      }
      return result;
    };
    for (auto& dependency : execution.dependencies) {
      dependency.producers = expand(dependency.producers);
      dependency.consumers = expand(dependency.consumers);
    }
    // A stage's steps run on every rank of that stage, so one stage step becomes one
    // step per rank. Rewriting only the dependencies would leave each step naming the
    // stage, which is no longer one of the plan's roles -- and the worker looks a step's
    // unit up in that list. Slot references between steps follow the same rename, per
    // rank, so an intra-unit edge keeps pointing at its own rank's producer rather than
    // at whichever rank was rewritten last.
    const bool canonicalEntries = std::any_of(execution.steps.begin(), execution.steps.end(),
      [] (const auto& step) { return !step.onnxNodeIndices.empty(); });
    if (canonicalEntries) {
      // These step IDs, producer references and dependencies already identify
      // physical ONNX CALL owners. Stage expansion would rename only the steps.
      for (const auto& unit : prepared.candidate.executionPlan.roles) {
        if (selectedByStage.at(unit) != std::vector<std::string>{unit})
          throw std::invalid_argument("prepared canonical ONNX roles require physical unit IDs");
      }
      execution.validateSteps();
    }
    else if (!execution.steps.empty()) {
      std::vector<NativeExecutionStepSpec> expandedSteps;
      expandedSteps.reserve(execution.steps.size() * selectedByStage.size());
      for (const auto& step : execution.steps) {
        const auto stageStep = selectedByStage.find(step.unitId);
        if (stageStep == selectedByStage.end())
          throw std::invalid_argument(
            "prepared step names a unit with no selected roles: " + step.unitId);
        const auto renamed = [&stageStep] (const std::string& stepId) {
          // Step ids are "<stage>/<kind>/<index>"; a rank's copy is
          // "<rank>/<kind>/<index>". Only ids that begin with this stage's prefix
          // belong to it, so an edge from another stage is left alone.
          const std::string prefix = stageStep->first + "/";
          if (stepId.rfind(prefix, 0) != 0) return stepId;
          return stepId.substr(prefix.size());
        };
        for (const auto& selected : stageStep->second) {
          auto copy = step;
          copy.unitId = selected;
          copy.stepId = selected + "/" + renamed(step.stepId);
          for (auto& slot : copy.inputSlots) {
            if (slot.producerStepId.empty()) continue;
            // The producer is in the same stage: its copy on this rank. A producer in
            // another stage keeps its own expansion, which that stage's loop renames.
            if (slot.producerStepId.rfind(stageStep->first + "/", 0) == 0) {
              slot.producerStepId = selected + "/" + renamed(slot.producerStepId);
            }
          }
          expandedSteps.push_back(std::move(copy));
        }
      }
      execution.steps = std::move(expandedSteps);
    }
    m_execution.push_back(std::move(execution));
  }
}

const NativeJson* NativePreparedPlanningCache::preparedRoleJson(std::size_t index) const
{
  if (!m_template || index >= candidates.size() || index >= m_template->candidates().size())
    return nullptr;
  const auto& current = candidates[index].roles;
  const auto& prepared = m_template->candidates()[index].roles;
  if (current.size() != prepared.size() ||
      !std::equal(current.begin(), current.end(), prepared.begin(),
        [](const auto& left, const auto& right) {
          return nativeSelectionRoleContractsEqualV3(left, right);
        }))
    return nullptr;
  return &m_template->roleJson(index);
}

std::string nativePlacementObservationCanonicalJson(const NativeJson& snapshot,
  const NativePreparedPlanningTemplate* prepared)
{
  if (!snapshot.is_object()) return nativeCanonicalJson(snapshot);
  const bool canReuse = prepared && std::fegetround() == FE_TONEAREST &&
    std::locale() == std::locale::classic();
  const auto fixedField = [](const std::string& key) {
    return key == "candidate_digest" || key == "graph_digest" ||
      key == "node_roles" || key == "roles" || key == "stage_order";
  };
  // Match the actual request-owned JSON after every callback. The owner is
  // an optimization hint; it never certifies mutable snapshot contents.
  const auto matches = [&](const NativeJson& graph, std::size_t index) {
    if (!canReuse || index >= prepared->m_candidates.size() ||
        prepared->m_snapshotFixedCanonical[index].empty()) return false;
    const auto& expected = prepared->m_candidates[index];
    const auto stringMatches = [&](const char* field, const std::string& text) {
      const auto found = graph.find(field);
      return found != graph.end() && found->is_string() &&
        found->get_ref<const NativeJson::string_t&>() == text;
    };
    if (!stringMatches("candidate_digest", expected.candidate.candidateDigest) ||
        !stringMatches("graph_digest", expected.candidate.graphDigest)) return false;
    const auto nodes = graph.find("node_roles");
    if (nodes == graph.end() || !nodes->is_object() ||
        nodes->size() != expected.candidate.nodeRoles.size()) return false;
    for (const auto& entry : expected.candidate.nodeRoles) {
      const auto found = nodes->find(entry.first);
      if (found == nodes->end() || !found->is_string() ||
          found->get_ref<const NativeJson::string_t&>() != entry.second) return false;
    }
    const auto stages = graph.find("stage_order");
    const auto& expectedStages = expected.candidate.executionPlan.roles;
    if (stages == graph.end() || !stages->is_array() ||
        stages->size() != expectedStages.size()) return false;
    for (std::size_t i = 0; i < expectedStages.size(); ++i) {
      if (!(*stages)[i].is_string() ||
          (*stages)[i].get_ref<const NativeJson::string_t&>() != expectedStages[i]) return false;
    }
    const auto roles = graph.find("roles");
    if (roles == graph.end() || !roles->is_array() ||
        roles->size() != expected.roles.size()) return false;
    for (std::size_t i = 0; i < expected.roles.size(); ++i) {
      if (!nativeAssemblyJsonMatches(expected.roles[i], (*roles)[i])) return false;
    }
    return true;
  };
  std::string output;
  const auto appendKey = [&](const std::string& key) {
    canonical_json_detail::append(output, NativeJson(key));
    output += ':';
  };
  output += '{';
  bool firstRoot = true;
  for (auto root = snapshot.begin(); root != snapshot.end(); ++root) {
    if (!firstRoot) output += ',';
    firstRoot = false;
    appendKey(root.key());
    if (root.key() != "prepared_role_graph" || !root.value().is_array()) {
      canonical_json_detail::append(output, root.value());
      continue;
    }
    output += '[';
    for (std::size_t index = 0; index < root.value().size(); ++index) {
      if (index) output += ',';
      const auto& graph = root.value()[index];
      if (!graph.is_object()) {
        canonical_json_detail::append(output, graph);
        continue;
      }
      const auto* cached = matches(graph, index)
        ? &prepared->m_snapshotFixedCanonical[index] : nullptr;
      output += '{';
      bool firstGraph = true;
      for (auto field = graph.begin(); field != graph.end(); ++field) {
        if (!firstGraph) output += ',';
        firstGraph = false;
        appendKey(field.key());
        if (cached && fixedField(field.key())) output += cached->at(field.key());
        else {
          if (fixedField(field.key())) ++detail::fixedAssignmentWork().snapshotFixedEncodes;
          canonical_json_detail::append(output, field.value());
        }
      }
      output += '}';
    }
    output += ']';
  }
  output += '}';
  return output;
}

NativeRequestCatalog NativeRequestCatalog::load(const std::string& configurationJson,
  NativeCanonicalSource source, const NativeAssemblyControl& control)
{
  return loadImpl(configurationJson, std::move(source), control, nullptr);
}

NativeRequestCatalog
NativeRequestCatalog::loadWithTensorLayouts(const std::string& configurationJson,
  NativeCanonicalSource source, const NativeAssemblyControl& control,
  std::vector<NativeOnnxNodeLayout>& layouts)
{
  std::vector<NativeOnnxNodeLayout> checkedLayouts;
  auto result = loadImpl(configurationJson, std::move(source), control, &checkedLayouts);
  layouts = std::move(checkedLayouts);
  return result;
}

NativeRequestCatalog
NativeRequestCatalog::loadImpl(const std::string& configurationJson, NativeCanonicalSource source,
  const NativeAssemblyControl& control, std::vector<NativeOnnxNodeLayout>* layouts)
{
  control.requireActive();
  if (configurationJson.size() > NATIVE_REQUEST_CATALOG_MAX_BYTES)
    throw std::invalid_argument("request catalog configuration exceeds limit: actual=" +
      std::to_string(configurationJson.size()) + " limit=" +
      std::to_string(NATIVE_REQUEST_CATALOG_MAX_BYTES));
  const auto root = nativeParseJson(configurationJson);
  if (root.at("schema") != "ndnsf-di-native-request-catalog-v1")
    throw std::invalid_argument("unsupported native request catalog schema");
  NativeCanonicalCatalogEntry entry;
  auto& model = entry.model;
  model.descriptor = NativeModelDescriptor::fromCanonicalJson(nativeCanonicalJson(root.at("model")));
  const auto& split = root.at("splitter");
  const auto route = parseCatalogRoute(split.at("kind").get<std::string>());
  if (layouts != nullptr && route != CatalogRoute::ONNX_SOURCE)
    throw std::invalid_argument("tensor layout preflight requires ONNX_SOURCE");
  const bool canonicalSteps = route == CatalogRoute::ONNX_SOURCE &&
    split.contains("step_template_v2");
  const bool sourceDefined = route == CatalogRoute::QWEN || route == CatalogRoute::LLAMA;
  using RankArtifacts = std::map<std::string, std::vector<std::string>>;
  const auto rankArtifacts = sourceDefined
    ? split.value("rank_artifact_digests_by_role", RankArtifacts{}) : RankArtifacts{};
  const auto resourceProfiles = sourceDefined
    ? resourceProfilesFromCatalog(split, split.at("roles").get<std::vector<std::string>>(),
                                  model.descriptor.adapter.backends)
    : std::map<std::string, NativeRoleResourceRequirement>{};
  const auto& sourceConfig = root.at("source");
  std::shared_ptr<const AutomaticOnnxContract> automaticContract;
  std::shared_ptr<const AutomaticOnnxModel> automaticModel;
  const bool referenceOnly = source.modelBytes.empty() && !source.preparedMetadataJson.empty();
  model.canonicalSourceName = sourceConfig.at("data_name").get<std::string>();
  model.canonicalSourceDigest = sourceConfig.at("digest").get<std::string>();
  model.modelManifestDigest = sourceConfig.at("model_manifest_digest").get<std::string>();
  model.canonicalGraphDigest = sourceConfig.at("canonical_graph_digest").get<std::string>();
  if (referenceOnly) {
    if (source.preparedMetadataJson.size() > NATIVE_PREPARED_METADATA_MAX_BYTES)
      throw std::invalid_argument("native prepared metadata exceeds publication bound");
    const auto metadata = nativeParseJson(source.preparedMetadataJson);
    auto decodedMetadata = decodeNativePreparedOnnxMetadata(metadata, control);
    const auto& facts = metadata.at("source");
    if (facts.at("name").get<std::string>() != model.canonicalSourceName ||
        facts.at("digest").get<std::string>() != model.canonicalSourceDigest ||
        facts.at("manifest_digest").get<std::string>() != model.modelManifestDigest ||
        facts.at("graph_digest").get<std::string>() != model.canonicalGraphDigest ||
        metadata.at("descriptor") != nativeParseJson(model.descriptor.canonicalJson()))
      throw std::invalid_argument("native prepared metadata differs from catalog identity");
    model.canonicalSourceBytes = facts.at("bytes").get<std::uint64_t>();
    model.canonicalInitializerBytes = facts.value("initializer_bytes", std::uint64_t{0});
    model.canonicalInitializerObjectDigest = facts.value("initializer_object_digest", std::string{});
    model.canonicalInitializerDigest = facts.value("initializer_digest", std::string{});
    entry.sourceGraphInspection = std::move(decodedMetadata.graph);
    automaticContract = std::move(decodedMetadata.automaticContract);
  }
  else {
    model.canonicalSourceBytes = source.modelBytes.size();
    if (nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size()) != model.canonicalSourceDigest)
      throw std::invalid_argument("request source bytes differ from pinned digest");
    if (source.initializerBytes || source.initializerRangeSource) {
      model.canonicalInitializerBytes = nativeCanonicalInitializerByteSize(source);
      model.canonicalInitializerObjectDigest = sourceConfig.at("initializer_digest").get<std::string>();
      if (nativeCanonicalInitializerDigest(source, control) !=
          model.canonicalInitializerObjectDigest)
        throw std::invalid_argument("request initializer bytes differ from pinned digest");
    }
    else if (sourceConfig.contains("initializer_digest"))
      throw std::invalid_argument("pinned initializer object is missing");
    if (model.descriptor.modelFormat == "onnx" && !canonicalSteps) {
      // Internal initializers live in the model bytes; external ones are pinned
      // above. Both need the normalized initializer identity for prepared
      // metadata and the assembly recipe.
      model.canonicalInitializerDigest = canonicalOnnxSourceIdentity(
        source, control).initializerDigest;
    }
  }
  const auto& recipe = root.at("recipe");
  entry.recipe = {recipe.at("artifact_profile_digest"), recipe.at("assembler_descriptor_digest"),
    recipe.at("backend_abi"), recipe.at("precision"), recipe.at("quantization"), recipe.at("layout"),
    recipe.at("padding"), recipe.at("protection_epoch"), recipe.at("max_source_bytes"),
    recipe.at("max_assembled_bytes"), recipe.at("max_nodes")};
  if (entry.recipe.quantization != model.descriptor.quantizationSubtype)
    throw std::invalid_argument("request recipe quantization differs from model identity");
  const auto& publication = root.at("publication");
  if (publication.contains("material_bundle_policy") &&
      (!canonicalSteps || publication.at("material_bundle_policy") != "prepared-role-v1"))
    throw std::invalid_argument("native catalog material bundle policy is unsupported");
  entry.publication.artifactRoot = publication.at("artifact_root").get<std::string>();
  entry.publication.packageManifestDigest = publication.value("package_manifest_digest", model.modelManifestDigest);
  if (entry.publication.packageManifestDigest != model.modelManifestDigest)
    throw std::invalid_argument("publication package differs from pinned manifest");
  entry.publication.layerManifestDigests = publication.value("layer_manifest_digests", std::vector<std::string>{});
  entry.publication.maxPublicationBytes = publication.value(
    "max_publication_bytes", std::uint64_t{0});
  // Bind model/source/recipe/split changes while keeping advisory transfer
  // geometry from forcing an identical model publication and assembly again.
  entry.publication.publicationIdentityDigest = nativeCanonicalPublicationIdentity(root);
  entry.nodes = root.value("node_mapping", NativeCanonicalRolePreparer::NodeMap{});
  entry.maxPayloadBytes = root.at("max_payload_bytes").get<std::size_t>();
  const auto format = root.at("input_format").get<std::string>();
  if (format == "JSON") entry.format = NativeCatalogModelAdapter::Format::JsonBytes;
  else if (format == "OPAQUE") entry.format = NativeCatalogModelAdapter::Format::OpaqueBytes;
  else throw std::invalid_argument("unsupported request input format");
  if (root.contains("conversation_input")) {
    const auto& conversationInput = root.at("conversation_input");
    if (!conversationInput.is_object())
      throw std::invalid_argument("unsupported native conversation input encoder");
    const auto kind = conversationInput.value("kind", std::string{});
    if (kind == "OPAQUE_BYTE_TOKEN_IDS") {
      // This operator-pinned fixture contract derives one canonical token from
      // each encoded byte.  Callers cannot supply or replace this encoder; it
      // is captured by the immutable adapter registry during preparation.
      entry.conversationTokenEncoder = [] (const std::vector<std::uint8_t>& bytes) {
        std::vector<std::int64_t> tokens;
        tokens.reserve(bytes.size());
        for (const auto byte : bytes)
          tokens.push_back(static_cast<std::int64_t>(byte));
        return tokens;
      };
    }
    else if (kind == "TENSOR_BUNDLE_TOKEN_IDS") {
      const auto tensorName = conversationInput.value("tensor_name", "input_ids");
      if (tensorName.empty())
        throw std::invalid_argument("tensor-bundle conversation input tensor name is empty");
      // The native request payload is an authenticated tensor bundle.  A
      // tensor-bundle encoder must derive the same token suffix as the
      // execution coordinator, rather than hashing transport framing bytes.
      entry.conversationTokenEncoder = [tensorName] (const std::vector<std::uint8_t>& bytes) {
        if (!isEncodedTensorBundle(bytes))
          throw std::invalid_argument("conversation input is not an encoded tensor bundle");
        const auto tensors = decodeTensorBundle(bytes);
        const auto found = std::find_if(tensors.begin(), tensors.end(),
          [&tensorName] (const auto& tensor) { return tensor.name == tensorName; });
        if (found == tensors.end() || found->elementType != TensorElementType::Int64 ||
            found->shape.empty() || found->payload.empty() ||
            found->payload.size() % sizeof(std::int64_t) != 0)
          throw std::invalid_argument("conversation input token tensor is invalid");
        std::vector<std::int64_t> tokens(found->payload.size() / sizeof(std::int64_t));
        std::memcpy(tokens.data(), found->payload.data(), found->payload.size());
        if (std::any_of(tokens.begin(), tokens.end(), [] (const auto token) { return token < 0; }))
          throw std::invalid_argument("conversation input token ID is negative");
        return tokens;
      };
    }
    else {
      throw std::invalid_argument("unsupported native conversation input encoder");
    }
  }
  NativeRequestCatalog result;
  result.automaticContract = std::move(automaticContract);
  // Candidates built from a projected plan skip the splitter's per-stage enumeration, so
  // they are collected separately and prepared through the same state binding and role
  // preparation as an enumerated candidate. A projected candidate with empty prepared
  // roles would fail role binding later, which is why it is not pushed alone.
  std::optional<NativeSplitCandidate> projectedCandidate;
  std::map<std::string, std::string> sourceRoleByUnit;
  std::shared_ptr<qwen::NativeQwenLayerSplit> transformerStrategy;
  if (sourceDefined) {
    const auto modelFamily = route == CatalogRoute::LLAMA ? "llama" : "qwen";
    // A sharded stage needs one artifact per rank. The catalog accepts the
    // per-rank map when present and leaves it empty otherwise, in which case the
    // splitter keeps the legacy single-artifact path for degree-one stages only.
    transformerStrategy = std::make_shared<qwen::NativeQwenLayerSplit>(
      split.at("layer_ranges").get<std::vector<qwen::NativeQwenLayerSplit::LayerRange>>(),
      split.at("artifact_digests_by_role").get<std::map<std::string, std::string>>(),
      split.at("weight_bytes_by_role").get<std::map<std::string, std::uint64_t>>(),
      split.at("roles").get<std::vector<std::string>>(), split.at("tensor_degrees").get<std::vector<std::uint64_t>>(),
      split.value("input_ingress_role", std::string{}), split.value("result_egress_role", std::string{}),
      modelFamily, rankArtifacts);
    model.graph = transformerStrategy->inspectGraph(
      model.descriptor, model.descriptor.sourceRevision, entry.recipe.maxNodes);
    result.cooperativeSplitter = transformerStrategy;
    result.splitter = transformerStrategy;
  }
  else if (route == CatalogRoute::YOLO) {
    // The legacy YOLO splitter is retired by default. A historical comparison
    // run may opt back in explicitly with `legacy_yolo_retired: false`; every
    // other catalog gets the typed retirement error instead of silently
    // falling back to the old component model.
    if (root.value("legacy_yolo_retired", true))
      throw std::runtime_error(
        "DI_NATIVE_LEGACY_YOLO_SPLITTER_RETIRED: kind=YOLO is retired; "
        "use splitter.kind=ONNX_SOURCE");
    if (referenceOnly)
      throw std::invalid_argument("DI_NATIVE_PREPARATION_REFERENCE_RESTORE_UNSUPPORTED");
    model.graph = inspectNativeOnnxPlanningGraph(source, model.descriptor, control).graph;
    std::vector<yolo::NativeYoloCatalogComponent> components;
    for (const auto& item : split.at("components")) {
      yolo::NativeYoloComponentSpec component;
      component.candidateId = item.at("candidate_id").get<std::string>();
      component.priority = item.at("priority").get<int>();
      component.roles = item.at("roles").get<std::vector<std::string>>();
      component.nodeNamesByRole = item.at("node_names_by_role").get<std::map<std::string, std::vector<std::string>>>();
      component.inputIngressRole = item.at("input_ingress_role").get<std::string>();
      component.resultEgressRole = item.at("result_egress_role").get<std::string>();
      component.mergeKind = item.at("merge_kind").get<std::string>();
      component.candidateDigest = item.at("candidate_digest").get<std::string>();
      components.push_back({std::move(component), nativeCanonicalJson(item.at("semantic_partition"))});
    }
    auto strategy = std::make_shared<yolo::NativeYoloComponentSplit>(
      yolo::NativeYoloComponentSplit::fromOnnxCatalog(model.descriptor, source, control,
        std::move(components), nativeCanonicalJson(split.value("postprocessing", NativeJson::object()))));
    result.cooperativeSplitter = strategy;
    result.splitter = std::move(strategy);
  }
  else if (canonicalSteps) {
    if (model.descriptor.modelFormat != "onnx" || model.descriptor.adapterId != "wan" ||
        root.contains("conversation_input") ||
        !root.value("state_inputs", NativeJson::object()).empty() ||
        !root.value("state_outputs", NativeJson::object()).empty())
      throw std::invalid_argument("canonical ONNX step graph requires a stateless ONNX source");
    const auto graphWork = detail::nativePhaseStarted();
    if (!referenceOnly) {
      entry.sourceGraphInspection = inspectNativeOnnxPlanningGraph(source, model.descriptor,
                                                                   control);
      // Inspection already derives the normalized identity from the source bytes.
      model.canonicalInitializerDigest =
        entry.sourceGraphInspection->canonicalIdentity.initializerDigest;
    }
    auto& inspection = *entry.sourceGraphInspection;
    if (referenceOnly) {
      if (automaticContract || inspection.canonicalIdentity.graphDigest !=
          model.canonicalGraphDigest || inspection.canonicalIdentity.initializerDigest !=
          model.canonicalInitializerDigest)
        throw std::invalid_argument("prepared canonical ONNX source identity differs");
      auto graph = restoreNativeOnnxPlanningGraph(inspection.graphMetadataJson,
                                                 model.descriptor, control);
      if (graph.graphDigest != inspection.graph.graphDigest ||
          graph.topologicalOrder != inspection.nodeNames)
        throw std::invalid_argument("prepared canonical ONNX node index differs");
      inspection.graph = std::move(graph);
    }
    if (entry.nodes != wan::makeWanMaterialNodeMap(inspection))
      throw std::invalid_argument("canonical ONNX step graph requires exact source node mapping");
    const auto preparationId = "/NDNSF/DI/PREPARE/" + model.canonicalSourceDigest.substr(7);
    detail::logNativeWorkPhase(preparationId, "catalog-graph-inspection", graphWork);
    const auto candidateWork = detail::nativePhaseStarted();
    model.graph = inspection.graph;
    wan::OnnxParallelPlan layout;
    layout.plan = nativeCanonicalOnnxStepPlanFromJson(split.at("step_template_v2"),
                                                       split.at("dependencies"));
    const auto ports = [] (const auto& slots) {
      std::vector<NativeAssemblyTensorContractV3> result;
      for (const auto& slot : slots)
        result.push_back({slot.slotId, slot.dtype, slot.shape});
      return result;
    };
    for (const auto& step : layout.plan.steps) {
      if (step.operation != "CALL_SUBGRAPH") continue;
      if (!layout.entries.emplace(step.artifactEntry, wan::OnnxEntryCut{
            step.onnxNodeIndices, ports(step.inputSlots), ports(step.outputSlots)}).second)
        throw std::invalid_argument("canonical ONNX step graph repeats a CALL entry id");
    }
    const NativeRequestControl planningControl{
      preparationId, 1, control.deadline,
      [&control] { control.requireActive(); return false; }};
    auto resources = resourceProfilesFromCatalog(split, layout.plan.roles,
                                                  model.descriptor.adapter.backends);
    auto candidate = wan::makeWanSplitCandidate(model, inspection, layout,
      split.at("service_name").get<std::string>(), std::move(resources), planningControl);
    detail::logNativeWorkPhase(preparationId, "catalog-split-candidate", candidateWork);
    auto strategy = std::make_shared<wan::NativeWanSplitStrategy>(std::move(candidate));
    result.cooperativeSplitter = strategy;
    result.splitter = std::move(strategy);
  }
  else if (route == CatalogRoute::ONNX_SOURCE) {
    if (referenceOnly)
      throw std::invalid_argument("DI_NATIVE_PREPARATION_REFERENCE_RESTORE_UNSUPPORTED");
    // The historical design names this field selection_mode; keep accepting the
    // shorter alias for existing configurations but refuse an ambiguous pair.
    const auto modeValue = split.find("selection_mode");
    const auto aliasValue = split.find("mode");
    if (modeValue == split.end() && aliasValue == split.end())
      throw std::invalid_argument("ONNX_SOURCE splitter has no selection mode");
    if (modeValue != split.end() && aliasValue != split.end() &&
        *modeValue != *aliasValue)
      throw std::invalid_argument("ONNX_SOURCE mode and selection_mode disagree");
    const auto& modeNode = modeValue != split.end() ? *modeValue : *aliasValue;
    if (!modeNode.is_string())
      throw std::invalid_argument("ONNX_SOURCE selection mode is not a string");
    const auto modeText = modeNode.get<std::string>();
    NativeSourcePartitionMode mode;
    if (modeText == "full") mode = NativeSourcePartitionMode::FULL;
    else if (modeText == "component") mode = NativeSourcePartitionMode::COMPONENT;
    else if (modeText == "hybrid") mode = NativeSourcePartitionMode::HYBRID;
    else if (modeText == "tensor_parallel") mode = NativeSourcePartitionMode::TENSOR_PARALLEL;
    else throw std::invalid_argument("unsupported ONNX_SOURCE mode");
    std::vector<NativeOnnxNodeLayout> tensorLayouts;
    auto* inspectedLayouts = mode == NativeSourcePartitionMode::TENSOR_PARALLEL
      ? &tensorLayouts : layouts;
    auto inspection = inspectedLayouts == nullptr
      ? inspectNativeOnnxPlanningGraph(source, model.descriptor, control)
      : inspectNativeOnnxTensorParallelGraph(source, model.descriptor, control, *inspectedLayouts);
    model.graph = inspection.graph;
    if (layouts != nullptr && inspectedLayouts == &tensorLayouts) *layouts = tensorLayouts;
    NativeSourcePartitionProfile profile;
    profile.mode = mode;
    profile.roles = split.at("roles").get<std::vector<std::string>>();
    profile.boundaryNode = split.value("boundary_node", std::string{});
    profile.inputIngressRole = split.at("input_ingress_role").get<std::string>();
    profile.resultEgressRole = split.at("result_egress_role").get<std::string>();
    profile.postprocessingJson = nativeCanonicalJson(
      split.value("postprocessing", NativeJson::object()));
    profile.inputChannels = split.value("input_channels", std::uint64_t{0});
    profile.convInputAxis = split.value("conv_input_axis", std::string("input-channel"));
    profile.activationShape = split.value("activation_shape", std::vector<std::int64_t>{});
    profile.partialShape = split.value("partial_shape", std::vector<std::int64_t>{});
    profile.activationDtype = split.value("activation_dtype", std::string("float32"));
    profile.activationLayoutDigest = split.value("activation_layout_digest", std::string{});
    profile.convWeightBytes = split.value("conv_weight_bytes", std::uint64_t{0});
    profile.convBiasBytes = split.value("conv_bias_bytes", std::uint64_t{0});
    profile.convInputTensor = split.value("conv_input_tensor", std::string{});
    profile.convWeightInitializer = split.value("conv_weight_initializer", std::string{});
    profile.convBiasInitializer = split.value("conv_bias_initializer", std::string{});
    profile.convOutputTensor = split.value("conv_output_tensor", std::string{});
    profile.convPads = split.value("conv_pads", std::vector<std::int64_t>{});
    profile.convStrides = split.value("conv_strides", std::vector<std::int64_t>{});
    profile.convDilations = split.value("conv_dilations", std::vector<std::int64_t>{});
    std::vector<std::pair<std::string, std::string>> ffnAnchors;
    const auto pairedDown = split.find("ffn_down_node");
    const auto pairedWindows = split.find("ffn_windows");
    const auto attention = split.find("attention_windows");
    const bool blockProgram = attention != split.end();
    if (blockProgram && (mode != NativeSourcePartitionMode::HYBRID ||
        pairedWindows == split.end() || !attention->is_array() ||
        !split.contains("boundary_node") || !split.contains("suffix_node")))
      throw std::invalid_argument("source attention windows require a bounded hybrid block program");
    if (blockProgram) profile.suffixNode = split.at("suffix_node").get<std::string>();
    if (pairedWindows != split.end()) {
      if ((!blockProgram && split.contains("boundary_node")) || pairedDown != split.end() ||
          !pairedWindows->is_array() || pairedWindows->size() < 2)
        throw std::invalid_argument("ffn_windows requires at least two exclusive window declarations");
      for (const auto& window : *pairedWindows) {
        if (!window.is_object() || window.size() != 2 ||
            !window.contains("up_node") || !window.contains("down_node"))
          throw std::invalid_argument("paired FFN window requires up_node and down_node");
        ffnAnchors.emplace_back(window.at("up_node").get<std::string>(),
                                window.at("down_node").get<std::string>());
      }
      if (!blockProgram) profile.boundaryNode = ffnAnchors.front().first;
    }
    else if (pairedDown != split.end()) {
      ffnAnchors.emplace_back(profile.boundaryNode, pairedDown->get<std::string>());
    }
    if (!ffnAnchors.empty() && mode != NativeSourcePartitionMode::HYBRID)
      throw std::invalid_argument("paired FFN window requires source hybrid mode");
    const auto metadata = nativeParseJson(inspection.graphMetadataJson);
    const auto& tensors = metadata.at("tensors");
    const auto initializerBytes = [&] (const std::string& name) {
      const auto& size = tensors.at(name).at("sizeBytes");
      if (!size.is_number_unsigned())
        throw std::invalid_argument("ONNX_SOURCE initializer memory is unknown");
      return size.get<std::uint64_t>();
    };
    if (mode == NativeSourcePartitionMode::TENSOR_PARALLEL) {
      if (profile.roles.size() != 2 || !profile.boundaryNode.empty() || !ffnAnchors.empty())
        throw std::invalid_argument("tensor_parallel needs two source-derived roles without a cut");
      profile.tensorPlan = std::make_shared<NativeOnnxTensorStepPlan>(
        buildNativeOnnxTensorStepPlan(inspection, tensorLayouts,
          {profile.roles[0], profile.roles[1]}, model.descriptor.modelName));
      for (const auto& step : profile.tensorPlan->plan.steps) {
        auto& total = profile.denseWeightBytesByRole[step.unitId];
        for (const auto& [name, users] : inspection.initializerConsumers) {
          control.requireActive();
          if (std::none_of(users.begin(), users.end(), [&] (auto ordinal) {
                return ordinal >= step.sourceLayers.first && ordinal < step.sourceLayers.second;
              }))
            continue;
          const auto bytes = initializerBytes(name);
          if (bytes > std::numeric_limits<std::uint64_t>::max() - total)
            throw std::overflow_error("tensor source role initializer bound overflows");
          total += bytes;
        }
      }
    }
    else if (mode == NativeSourcePartitionMode::HYBRID) {
      const auto& order = model.graph.topologicalOrder;
      const auto boundary = std::find(order.begin(), order.end(), profile.boundaryNode);
      if (boundary == order.end())
        throw std::invalid_argument("hybrid source boundary is absent from the graph");
      auto suffixStart = std::next(boundary);
      if (!ffnAnchors.empty()) {
        if ((!blockProgram && model.graph.modelInputs.size() != 1) ||
            model.graph.modelOutputs.size() != 1 || profile.roles.size() != 4 || profile.inputIngressRole != profile.roles.front() ||
            profile.resultEgressRole != profile.roles.back() || boundary == order.begin())
          throw std::invalid_argument("paired FFN source ports need prefix and suffix owners");
        onnx::ModelProto original;
        if (source.modelBytes.size() > std::numeric_limits<int>::max() ||
            !original.ParseFromArray(source.modelBytes.data(),
                                     static_cast<int>(source.modelBytes.size())))
          throw std::invalid_argument("paired FFN source graph does not match its inspection");
        const auto& graph = original.graph();
        auto next = boundary;
        for (const auto& [upId, downId] : ffnAnchors) {
          control.requireActive();
          const auto upPosition = blockProgram ? std::find(next, order.end(), upId) : next;
          if (upPosition == order.end() || *upPosition != upId)
            throw std::invalid_argument("paired FFN windows must be contiguous and ordered");
          const auto downPosition = std::find(std::next(upPosition), order.end(), downId);
          const auto distance = std::distance(upPosition, downPosition);
          if (downPosition == order.end() || (distance != 2 && distance != 3))
            throw std::invalid_argument("paired FFN must delimit a closed middle window");
          const auto start = inspection.canonicalNodeIndices.at(*upPosition);
          const auto finish = inspection.canonicalNodeIndices.at(*downPosition);
          if (finish >= static_cast<std::uint64_t>(graph.node_size()) ||
              finish < start || finish - start != static_cast<std::uint64_t>(distance))
            throw std::invalid_argument("paired FFN source graph does not match its inspection");
          next = std::next(downPosition);
          const auto& up = graph.node(static_cast<int>(start));
          const auto& down = graph.node(static_cast<int>(finish));
          const auto requireOp = [] (const auto& node, std::string_view op, int inputs) {
            if (!node.domain().empty() || node.op_type() != op || node.input_size() != inputs ||
                node.output_size() != 1 || (op != "Gelu" && node.attribute_size() != 0))
              throw std::invalid_argument("paired FFN window has an unsupported source operator");
          };
          const auto upShape = inspectNativeOnnxLinearProjection(original, up);
          const auto downShape = inspectNativeOnnxLinearProjection(original, down);
          auto activationInput = up.output(0);
          NativeSourceFfnWindow window;
          auto& pair = window.transform;
          pair.kind = "ffn-pair-partial";
          pair.node = *upPosition;
          pair.downNode = *downPosition;
          pair.axis = "ffn-channel";
          pair.input = up.input(0);
          pair.weight = up.input(1);
          pair.downWeight = down.input(1);
          pair.output = down.output(0);
          if (finish - start == 3) {
            const auto& add = graph.node(static_cast<int>(start + 1));
            requireOp(add, "Add", 2);
            if (add.input(0) == activationInput) pair.bias = add.input(1);
            else if (add.input(1) == activationInput) pair.bias = add.input(0);
            else throw std::invalid_argument("paired FFN bias is not fed by the up projection");
            activationInput = add.output(0);
          }
          const auto& gelu = graph.node(static_cast<int>(finish - 1));
          requireOp(gelu, "Gelu", 1);
          if (gelu.input(0) != activationInput || down.input(0) != gelu.output(0))
            throw std::invalid_argument("paired FFN projections do not share their activation");
          const auto floatShape = [&] (const std::string& name) {
            const auto& tensor = tensors.at(name);
            if (tensor.at("dtype") != "float32" || !tensor.at("sizeBytes").is_number_unsigned())
              throw std::invalid_argument("paired FFN requires static FP32 tensors");
            auto shape = tensor.at("shape").get<std::vector<std::int64_t>>();
            if (shape.empty() || std::any_of(shape.begin(), shape.end(), [](auto n) { return n <= 0; }))
              throw std::invalid_argument("paired FFN requires positive source dimensions");
            return shape;
          };
          window.inputShape = floatShape(pair.input);
          window.outputShape = floatShape(pair.output);
          if (upShape.n < 2 || upShape.n != downShape.k || window.inputShape.size() < 2 ||
              window.inputShape.size() != window.outputShape.size() ||
              !std::equal(window.inputShape.begin(), window.inputShape.end() - 1,
                          window.outputShape.begin()) ||
              window.inputShape.back() != upShape.k || window.outputShape.back() != downShape.n ||
              (!pair.bias.empty() && floatShape(pair.bias) != std::vector<std::int64_t>{upShape.n}))
            throw std::invalid_argument("paired FFN source shapes do not contract");
          if (downShape.packed && upShape.n % (2 * NativeOnnxLinearProjection::BLOCK) != 0)
            throw std::invalid_argument("paired FFN rank boundary splits a packed K block");
          for (const auto& [name, owner] :
               {std::make_pair(pair.weight, start), std::make_pair(pair.downWeight, finish),
                std::make_pair(pair.bias, start + 1),
                std::make_pair(upShape.packed ? up.input(2) : std::string{}, start),
                std::make_pair(downShape.packed ? down.input(2) : std::string{}, finish)}) {
            if (name.empty()) continue;
            const auto users = inspection.initializerConsumers.find(name);
            if (users == inspection.initializerConsumers.end() ||
                users->second != std::vector<std::uint64_t>{owner})
              throw std::invalid_argument("paired FFN initializer has foreign consumers");
            const auto bytes = initializerBytes(name);
            if (bytes > std::numeric_limits<std::uint64_t>::max() - window.weightBytes)
              throw std::overflow_error("paired FFN initializer bytes exceed uint64");
            window.weightBytes += bytes;
          }
          pair.extent = static_cast<std::uint64_t>(upShape.n);
          pair.end = pair.extent / 2 + pair.extent % 2;
          pair.validate();
          window.nodes.assign(upPosition, next);
          if (!blockProgram && !profile.ffnWindows.empty()) {
            const auto& previous = profile.ffnWindows.back();
            if (previous.transform.output != pair.input || previous.outputShape != window.inputShape)
              throw std::invalid_argument("paired FFN windows do not share an exact activation");
          }
          profile.ffnWindows.push_back(std::move(window));
        }
        if (next == order.end())
          throw std::invalid_argument("paired FFN windows leave no suffix");
        if (blockProgram) {
          buildNativeSourceBlockEntries(profile, *attention, inspection, model.graph,
                                        metadata, original, control);
          suffixStart = std::find(next, order.end(), profile.suffixNode);
        }
        else {
          suffixStart = next;
          const auto& first = profile.ffnWindows.front();
          profile.activationShape = first.inputShape;
          profile.partialShape = profile.ffnWindows.back().outputShape;
          profile.inputChannels = first.transform.extent;
          profile.activationDtype = "float32";
          profile.convBiasBytes = 0;
          const auto start = inspection.canonicalNodeIndices.at(*boundary);
          const auto last = inspection.canonicalNodeIndices.at(profile.ffnWindows.back().nodes.back());
          const auto& sourceInput = model.graph.modelInputs.front().name;
          const auto& sourceOutput = model.graph.modelOutputs.front().name;
          bool suffixProducesOutput = false;
          for (auto i = start; i < static_cast<std::uint64_t>(graph.node_size()); ++i) {
            const auto& node = graph.node(static_cast<int>(i));
            if (std::find(node.input().begin(), node.input().end(), sourceInput) != node.input().end())
              throw std::invalid_argument("paired FFN source input has users outside the prefix");
            if (i > last &&
                std::find(node.output().begin(), node.output().end(), sourceOutput) != node.output().end())
              suffixProducesOutput = true;
          }
          if (!suffixProducesOutput)
            throw std::invalid_argument("paired FFN source suffix does not own terminal output");
          std::uint64_t prefixBytes = 0;
          for (const auto& [name, users] : inspection.initializerConsumers) {
            if (std::none_of(users.begin(), users.end(), [start](auto node) { return node < start; }))
              continue;
            const auto bytes = initializerBytes(name);
            if (bytes > std::numeric_limits<std::uint64_t>::max() - prefixBytes)
              throw std::overflow_error("paired FFN prefix initializer bytes exceed uint64");
            prefixBytes += bytes;
          }
          profile.ffnPrefixWeightBytes = prefixBytes;
        }
      }
      else {
        profile.convWeightBytes = initializerBytes(profile.convWeightInitializer);
        profile.convBiasBytes = profile.convBiasInitializer.empty() ? 0 :
          initializerBytes(profile.convBiasInitializer);
      }
      std::set<std::uint64_t> suffixNodes;
      for (auto node = suffixStart; node != order.end(); ++node)
        suffixNodes.insert(inspection.canonicalNodeIndices.at(*node));
      std::uint64_t suffixBytes = profile.convBiasBytes;
      for (const auto& [name, users] : inspection.initializerConsumers) {
        if ((profile.ffnWindows.empty() && name == profile.convBiasInitializer) ||
            std::none_of(users.begin(), users.end(),
                         [&] (auto node) { return suffixNodes.count(node) != 0; }))
          continue;
        const auto bytes = initializerBytes(name);
        if (bytes > std::numeric_limits<std::uint64_t>::max() - suffixBytes)
          throw std::overflow_error("ONNX_SOURCE suffix initializer memory exceeds uint64");
        suffixBytes += bytes;
      }
      profile.suffixWeightBytes = suffixBytes;
    }
    else {
      const auto& order = model.graph.topologicalOrder;
      const auto boundary = mode == NativeSourcePartitionMode::FULL ? order.end() :
        std::find(order.begin(), order.end(), profile.boundaryNode);
      if (mode == NativeSourcePartitionMode::COMPONENT &&
          (boundary == order.begin() || boundary == order.end() || profile.roles.size() != 2))
        throw std::invalid_argument("component source boundary does not cover two regions");
      if (profile.roles.empty())
        throw std::invalid_argument("source partition has no roles");
      std::map<std::uint64_t, std::size_t> regionByNode;
      for (auto node = order.begin(); node != order.end(); ++node)
        regionByNode.emplace(inspection.canonicalNodeIndices.at(*node), node < boundary ? 0 : 1);
      for (const auto& role : profile.roles) profile.denseWeightBytesByRole.emplace(role, 0);
      for (const auto& [name, users] : inspection.initializerConsumers) {
        std::set<std::size_t> regions;
        for (const auto user : users) regions.insert(regionByNode.at(user));
        for (const auto region : regions) {
          auto& total = profile.denseWeightBytesByRole.at(profile.roles.at(region));
          const auto bytes = initializerBytes(name);
          if (bytes > std::numeric_limits<std::uint64_t>::max() - total)
            throw std::overflow_error("ONNX_SOURCE dense initializer memory exceeds uint64");
          total += bytes;
        }
      }
    }
    auto strategy = std::make_shared<NativeSourcePartitionStrategy>(std::move(profile));
    result.cooperativeSplitter = strategy;
    result.splitter = std::move(strategy);
  }
  else {
    if (referenceOnly || source.externalFileRanges.size() > 1)
      throw std::invalid_argument("DI_NATIVE_PREPARATION_AUTOMATIC_SOURCE_UNSUPPORTED");
    const auto initializerFile = sourceConfig.value("initializer_file", std::string{});
    OpenedAutomaticOnnxSource opened;
    opened.source = source;
    opened.modelDigest = nativePlanningDigest(opened.source.modelBytes.data(),
                                              opened.source.modelBytes.size());
    if (source.initializerBytes || source.initializerRangeSource) {
      // One external object is supported; its location comes from the pinned
      // catalog entry and its identity was already checked by the load above.
      if (initializerFile.empty() || entry.model.canonicalInitializerObjectDigest.empty() ||
          entry.model.canonicalInitializerBytes == 0)
        throw std::invalid_argument("automatic external initializer identity is incomplete");
      // The catalog's initializer_file is the on-disk path; the source
      // inventory keeps the model's external location, which is the file name
      // for maintained exports (a single external object is accepted here).
      opened.externalFiles.push_back(AutomaticOnnxSourceFile{
        std::filesystem::path(initializerFile).filename().string(),
        entry.model.canonicalInitializerBytes,
        entry.model.canonicalInitializerObjectDigest});
    }
    else if (!initializerFile.empty())
      throw std::invalid_argument("pinned automatic initializer object is missing");
    opened.sourceDigest = automaticOnnxSourceDigest(opened.modelDigest,
                                                    opened.externalFiles, control);
    auto automatic = inspectAutomaticOnnxModel(std::move(opened), control);
    automaticModel = std::make_shared<const AutomaticOnnxModel>(std::move(automatic));
    auto strategy = std::make_shared<AutomaticOnnxSplitStrategy>(automaticModel);
    model.graph = strategy->inspectGraph(model.descriptor, model.descriptor.sourceRevision,
                                         entry.recipe.maxNodes);
    // result.automaticContract was already assigned (empty) before this branch;
    // set it here as well so the automatic catalog carries its contract.
    result.automaticContract = std::make_shared<const AutomaticOnnxContract>(
      automaticModel->contract);
    result.cooperativeSplitter = strategy;
    result.splitter = std::move(strategy);
  }
  result.stateMapping.inputs = root.value("state_inputs", NativeStateTensorMapping::Roles{});
  result.stateMapping.outputs = root.value("state_outputs", NativeStateTensorMapping::Roles{});
  result.model = model;
  if (!referenceOnly) {
    // The automatic splitter's planning graph is the automatic inspection's
    // source graph. Reusing it keeps the role preparer's source/planning
    // identity check meaningful without an explicit ONNX node mapping.
    if (route == CatalogRoute::AUTOMATIC && automaticModel)
      entry.sourceGraphInspection = automaticModel->inspection;
    else if (!entry.sourceGraphInspection)
      entry.sourceGraphInspection = inspectNativeOnnxSourceGraph(source, model.descriptor, control);
    // Only adapters with a reference-only restore contract publish this
    // metadata. Unsupported adapters remain on the source-backed path and
    // therefore cannot create a receipt that a later prepare cannot restore.
    if (sourceDefined || canonicalSteps)
      source.preparedMetadataJson = encodeNativePreparedOnnxMetadata(
        model, *entry.sourceGraphInspection, nullptr, control);
    else if (route == CatalogRoute::AUTOMATIC) {
      if (!automaticModel)
        throw std::logic_error("automatic source model was released before metadata");
      // The automatic inspection owns the source graph facts that back the
      // automatic contract. The legacy source-graph pass derives a different
      // graph digest for the same bytes, so reuse the automatic inspection
      // rather than comparing two representations of one source.
      source.preparedMetadataJson = encodeNativePreparedOnnxMetadata(
        model, automaticModel->inspection, automaticModel.get(), control);
    }
  }
  // The projected-plan branch runs after the source is moved into the catalog entry, so
  // the bytes the shape derivation needs are captured here. `deriveQwenTpShape` parses
  // the serialized graph, which only exists before the move.
  const auto weights = split.find("tensor_rank_weights_by_role");
  const bool sourceBoundTensorPlan = route == CatalogRoute::QWEN &&
    (split.at("roles").size() > 1 || weights != split.end()) &&
    std::any_of(split.at("tensor_degrees").begin(), split.at("tensor_degrees").end(),
      [] (const auto& degree) { return degree.template get<std::uint64_t>() > 1; });
  std::map<std::string, std::vector<std::uint64_t>> rankWeights;
  if (weights != split.end()) {
    if (!sourceBoundTensorPlan || !weights->is_object() || weights->empty())
      throw std::invalid_argument("tensor rank weights require a source-bound sharded Qwen plan");
    for (const auto& stage : weights->items()) {
      if (!stage.value().is_array() || stage.value().empty())
        throw std::invalid_argument("tensor rank weights require a nonempty array per stage");
      auto& values = rankWeights[stage.key()];
      for (const auto& weight : stage.value()) {
        if (!weight.is_number_integer() || weight < 1 ||
            weight > std::numeric_limits<std::int32_t>::max())
          throw std::invalid_argument("tensor rank weights must be bounded positive integers");
        values.push_back(weight.get<std::uint64_t>());
      }
    }
  }
  std::optional<qwen::QwenTpShape> hybridColdShape;
  if (sourceBoundTensorPlan && !source.modelBytes.empty()) {
    // Preserve full byte-based operator/quantization admission before releasing the bytes.
    // Only the small geometry survives the move; port descriptions use inspected facts.
    hybridColdShape = qwen::deriveQwenTpShape(source.modelBytes,
      split.at("layer_ranges").back().at(1).get<std::uint64_t>());
  }
  // Hybrid cold/restore planning consumes the same inspected facts, without retaining
  // another model-sized byte copy after the source moves into the preparation catalog.
  const std::vector<std::uint8_t> planningGraphBytes =
    route == CatalogRoute::QWEN && !sourceBoundTensorPlan ? source.modelBytes :
      std::vector<std::uint8_t>{};
  // The same holds for the recorded graph facts: the sharded branch runs after the entry
  // has moved into the preparation catalog, so a reference-only restore could not reach
  // them there.
  const std::string planningGraphMetadata =
    entry.sourceGraphInspection ? entry.sourceGraphInspection->graphMetadataJson
                                : std::string{};
  const auto planningGraphInspection = sourceBoundTensorPlan
    ? entry.sourceGraphInspection : std::optional<NativeOnnxGraphInspection>{};
  entry.source = std::move(source);
  const auto assemblyByteLimit = entry.recipe.maxAssembledBytes;
  std::vector<NativeCanonicalCatalogEntry> entries;
  entries.push_back(std::move(entry));
  const auto materialWork = detail::nativePhaseStarted();
  result.preparation = std::make_shared<const NativeCanonicalPreparationCatalog>(
    std::move(entries), control,
    sourceDefined ? NativeStateContractBinding::SOURCE : NativeStateContractBinding::DECLARED);

  // Freeze the graph-dependent planning facts at prepare time.  The request
  // planner still rechecks the candidate/roles and performs all request-bound
  // admission, placement, grant, and projection work, but it no longer walks
  // the ONNX semantic graph after ACK closure.
  NativeRequestControl planningControl{
    "/NDNSF/DI/PREPARE/" + result.model.canonicalSourceDigest.substr(7), 1,
    control.deadline, [&control] { control.requireActive(); return false; }};
  detail::logNativeWorkPhase(planningControl.requestId, "catalog-material-preparation", materialWork);
  const auto rolesWork = detail::nativePhaseStarted();
  NativeCandidateBudget preparationBudget{1024, 60'000, 16};
  ExtensionControl extensionControl{
    control.deadline, [&control] { control.requireActive(); return false; }};
  std::vector<NativePreparedPlanningCandidate> preparedCandidates;
  // A sharded Qwen catalog produces a projected plan rather than a pipeline candidate.
  // The stage list still describes the topology, but each stage's ranks are separate
  // units with their own plan, so the candidate is built from the projection instead of
  // from the splitter's per-stage enumeration.
  if (route == CatalogRoute::QWEN &&
      std::any_of(split.at("tensor_degrees").begin(), split.at("tensor_degrees").end(),
                  [] (const auto& degree) {
                    return degree.template get<std::uint64_t>() > 1;
                  })) {
    // Existing single-stage catalogs retain the V1 projection. Explicit rank weights
    // use the source-bound V2 plan, even when the catalog declares one stage.
    const auto& degrees = split.at("tensor_degrees");
    const auto& layerRanges = split.at("layer_ranges");
    const auto& stageRoles = split.at("roles");
    if (sourceBoundTensorPlan) {
      const auto totalLayers = layerRanges.back().at(1).get<std::uint64_t>();
      const auto hybridShape = hybridColdShape ? *hybridColdShape
        : qwen::deriveQwenTpShapeFromGraphMetadata(planningGraphMetadata, totalLayers);
      if (!planningGraphInspection)
        throw std::invalid_argument("hybrid Qwen catalog requires inspected source port facts");
      const qwen::QwenTpSourceRegionPort region = [&] (const auto& nodes, const auto& outputs) {
        return result.preparation->sourceRegion(result.model, nodes, outputs, planningControl);
      };
      auto [candidate, unitStages] = transformerStrategy->lowerSourceHybrid(
        result.model.descriptor, result.model.graph, hybridShape, resourceProfiles,
        *planningGraphInspection, region, result.stateMapping, extensionControl, rankWeights);
      projectedCandidate = std::move(candidate);
      sourceRoleByUnit = std::move(unitStages);
    }
    else {
    const auto layers = layerRanges.at(0).at(1).get<std::uint64_t>();
    const auto degree = degrees.at(0).get<std::uint64_t>();
    const auto stageRole = stageRoles.at(0).get<std::string>();
    // A complete preparation-cache hit restores bounded graph facts instead of the
    // serialized graph, so the byte-based derivation has nothing to parse there. The
    // recorded metadata carries the same facts -- attention attributes, projection widths
    // and the embedding table -- and deriving from those is what the tensor-parallel
    // contract asks for: read recorded facts instead of guessing dimensions.
    const auto shape = planningGraphBytes.empty()
      ? qwen::deriveQwenTpShapeFromGraphMetadata(planningGraphMetadata, layers)
      : qwen::deriveQwenTpShape(planningGraphBytes, layers);

    const auto ranked = rankArtifacts.find(stageRole);
    if (ranked == rankArtifacts.end() || ranked->second.size() != degree) {
      throw std::invalid_argument(
        "sharded Qwen catalog needs one artifact digest per rank");
    }
    const auto profile = resourceProfiles.find(stageRole);
    if (profile == resourceProfiles.end()) {
      throw std::invalid_argument(
        "sharded Qwen catalog has no resource profile for its stage");
    }

    std::vector<qwen::QwenTpUnit> units;
    std::map<std::string, std::string> artifactByUnit;
    std::map<std::string, NativeRoleResourceRequirement> requirementByUnit;
    for (std::uint64_t rank = 0; rank < degree; ++rank) {
      qwen::QwenTpUnit unit;
      // The role name decides the prepared role's kind: a name with no stage or rank
      // component is treated as a component set, and a component set is not given a layer
      // range. A sharded unit does cover a layer range, so the name says so.
      unit.roleId = stageRole + "/Tensor/Rank/" + std::to_string(rank);
      unit.rank = rank;
      // Every unit covers every layer: a shard is a weight slice, not a layer slice.
      unit.layerBegin = 0;
      unit.layerEnd = layers;
      artifactByUnit[unit.roleId] = ranked->second.at(rank);
      requirementByUnit[unit.roleId] = profile->second;
      sourceRoleByUnit.emplace(unit.roleId, stageRole);
      units.push_back(std::move(unit));
    }
    // The head lives on exactly one unit, and which one is configuration, not convention: the
    // catalog's `result_egress_role` names the role that owns the terminal response, and the
    // projection derives `terminalResponseOwner` from it. Picking the last unit instead made the
    // head's owner a function of the member count, so adding a rank would silently move it --
    // and C6, which says rank 0 samples, disagreed with both. The configured role is the one
    // authority here; the last unit is accepted only when the configured value names a role the
    // plan does not contain, which is a configuration error and is reported as one.
    const auto egress = split.value("result_egress_role", std::string{});
    if (egress.empty())
      throw std::invalid_argument("sharded Qwen catalog has no result egress role");
    // The configured value may name the unit outright, or name the stage -- which is what the
    // maintained catalogs do, and which maps to that stage's last rank because a stage's egress
    // is the rank that owns its head. Both forms resolve here rather than one being assumed, so a
    // catalog that names a rank explicitly is honoured and one that names a stage keeps working.
    const auto egressMember = std::find_if(units.begin(), units.end(),
      [&egress] (const auto& unit) { return unit.roleId == egress; });
    const auto egressUnit = egressMember != units.end()
      ? egressMember->roleId
      : [&] {
          const auto stagePrefix = egress + "/";
          std::optional<std::string> lastOfStage;
          std::string known;
          for (const auto& unit : units) {
            if (!known.empty()) known += ',';
            known += unit.roleId;
            if (unit.roleId.rfind(stagePrefix, 0) == 0) lastOfStage = unit.roleId;
          }
          if (!lastOfStage)
            throw std::invalid_argument(
              "sharded Qwen catalog names a result egress role the plan does not contain: " +
              egress + " (known: " + known + ")");
          return *lastOfStage;
        }();
    projectedCandidate = qwen::buildQwenTpCandidate(
      result.model.graph, shape, units, "entry", egressUnit, result.model.descriptor,
      artifactByUnit, requirementByUnit);
    // The candidate belongs to the strategy that built it, and the planner compares that
    // identity against the registered strategy before it places anything. A projected
    // candidate carries its own builder name, which is not the registered strategy, so the
    // request was refused at ACK_CLOSED as a foreign identity. Adopt the catalog's identity
    // here, where the candidate is produced under that strategy.
    projectedCandidate->splitter = result.cooperativeSplitter->identity();
    }
  }
  const auto enumerated = projectedCandidate
    ? std::vector<NativeSplitCandidate>{}
    : result.cooperativeSplitter->enumerate(
        result.model.descriptor, result.model.graph, preparationBudget, extensionControl);
  if (route == CatalogRoute::AUTOMATIC && !enumerated.empty() &&
      result.stateMapping.inputs.empty()) {
    // The catalog did not declare a state map; derive it from the checked
    // automatic projection instead of inventing one.
    const auto familyMap = [] (const auto& byRole) {
      // One mapping entry per declared state contract: the semantic name is the
      // contract name and the value is its physical tensor (usually itself).
      NativeStateTensorMapping::Roles result;
      for (const auto& [role, tensors] : byRole) {
        auto& entry = result[role];
        for (const auto& tensor : tensors) entry[tensor.name] = {tensor.name};
      }
      return result;
    };
    result.stateMapping.inputs = familyMap(enumerated.front().roleStateInputsByRole);
    result.stateMapping.outputs = familyMap(enumerated.front().roleStateOutputsByRole);
  }
  // The automatic splitter may return many cuts with different state covers.
  // Preparation binds one state map, so it prepares the first (lowest-transfer)
  // candidate only instead of mixing maps across candidates.
  const std::vector<NativeSplitCandidate> selectedCandidates =
    route == CatalogRoute::AUTOMATIC && enumerated.size() > 1 ?
      std::vector<NativeSplitCandidate>{enumerated.front()} : enumerated;
  preparedCandidates.reserve(selectedCandidates.size());
  for (auto candidate : selectedCandidates) {
    if (route == CatalogRoute::AUTOMATIC) {
      // The assembly budget is an explicit upper bound, not a measurement:
      // ORT workspace and activation peaks stay within it or preparation fails
      // at the actual stage owner. Transient memory is declared inapplicable
      // for this CPU profile instead of being treated as free.
      for (auto& [role, requirement] : candidate.requirementsByRole) {
        if (!requirement.workspaceBytes)
          requirement.workspaceBytes = assemblyByteLimit;
        if (!requirement.transientBytes) requirement.transientBytes = std::uint64_t{0};
        if (!requirement.activationBytes)
          requirement.activationBytes = assemblyByteLimit;
      }
      candidate.candidateDigest = candidate.computedDigest();
    }
    if (sourceDefined) {
      candidate.requirementsByRole = resourceProfiles;
      candidate.candidateDigest = candidate.computedDigest();
    }
    // A declared stateless candidate has no tensor-to-source mapping to bind.
    // prepareRoles below still validates the candidate and its source boundary.
    const bool needsStateBinding = sourceDefined || !result.stateMapping.inputs.empty() ||
      !result.stateMapping.outputs.empty() || !candidate.roleStateInputsByRole.empty() ||
      !candidate.roleStateOutputsByRole.empty();
    auto bound = needsStateBinding
      ? result.preparation->bindCatalogStateContracts(
          result.model, candidate, result.stateMapping, planningControl)
      : std::move(candidate);
    auto roles = result.preparation->prepareRoles(result.model, bound, planningControl);
    preparedCandidates.push_back({std::move(bound), std::move(roles)});
  }
  if (projectedCandidate) {
    auto candidate = std::move(*projectedCandidate);
    // Every unit inherits only its logical stage's source tensors. The role preparer
    // still validates these names and their shapes against the actual source boundary.
    if (sourceRoleByUnit.size() != candidate.executionPlan.roles.size() ||
        std::any_of(candidate.executionPlan.roles.begin(), candidate.executionPlan.roles.end(),
          [&] (const auto& unit) { return sourceRoleByUnit.count(unit) != 1; }))
      throw std::invalid_argument("native state role expansion has an incomplete unit cover");
    result.stateMapping = detail::remapNativeStateRoles(result.stateMapping, sourceRoleByUnit);
    // No resource override here. `resourceProfiles` is keyed by the catalog's stage
    // roles, while this candidate's roles are the expanded unit names, so assigning it
    // would replace a complete per-unit cover with a per-stage one and the candidate
    // would be refused for an incomplete role cover. The unit requirements were derived
    // from each stage's profile when the units were built.
    candidate.candidateDigest = candidate.computedDigest();
    auto bound = result.preparation->bindCatalogStateContracts(
      result.model, candidate, result.stateMapping, planningControl);
    auto roles = result.preparation->prepareRoles(result.model, bound, planningControl);
    preparedCandidates.push_back({std::move(bound), std::move(roles)});
  }
  if (preparedCandidates.empty())
    throw std::runtime_error("DI_NATIVE_PREPARATION_NO_PLANNING_CANDIDATE");
  detail::logNativeWorkPhase(planningControl.requestId, "catalog-role-preparation", rolesWork);
  auto transferProfiles = root.value("request_transfer_profiles", NativeJson::array());
  if (!transferProfiles.is_array() || transferProfiles.size() > 8)
    throw std::invalid_argument("invalid request transfer profile collection");
  for (const auto& profile : transferProfiles) {
    if (!profile.is_object() ||
        profile.value("schema", std::string{}) != "ndnsf-di-request-transfer-profile-v1" ||
        profile.value("source_digest", std::string{}) != result.model.canonicalSourceDigest ||
        !profile.value("initial_context", false) ||
        !profile.contains("dependencies") || !profile.at("dependencies").is_array() ||
        profile.at("dependencies").size() > 64)
      throw std::invalid_argument("request transfer profile differs from pinned source");
  }
  auto ownedTemplate = std::shared_ptr<const NativePreparedPlanningTemplate>(
    new const NativePreparedPlanningTemplate(
      result.preparation->modelRefFor(result.model.descriptor), result.cooperativeSplitter,
      preparedCandidates));
  result.planningCache = std::shared_ptr<const NativePreparedPlanningCache>(
    new const NativePreparedPlanningCache(result.cooperativeSplitter->identity(),
      std::move(preparedCandidates), std::move(transferProfiles), std::move(ownedTemplate)));
  control.requireActive();
  return result;
}
} // namespace ndnsf::di
