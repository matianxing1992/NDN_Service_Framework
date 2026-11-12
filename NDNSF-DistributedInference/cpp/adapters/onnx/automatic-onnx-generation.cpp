#include "automatic-onnx-generation.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ndnsf::di {
namespace {

[[noreturn]] void
rejectState(const char* reason)
{
  throw AutomaticGenerationUnproven(reason);
}

using Axes = std::array<std::uint64_t, 4>;
constexpr Axes IDENTITY_AXES{0, 1, 2, 3};

struct IntegerShapeValue
{
  std::optional<std::int64_t> literal;
  TensorAxisReference axis;
};

struct IntegerShapeProgram
{
  std::vector<IntegerShapeValue> values;
  bool scalar = false;
  std::int32_t elementType = 7;
  std::uint64_t castLimit = std::numeric_limits<std::uint64_t>::max();
};

struct TokenAxes
{
  std::string input;
  std::array<std::uint64_t, 2> axes{};
  std::array<std::pair<std::uint64_t, std::uint64_t>, 2> factors{{{1, 1}, {1, 1}}};
  std::array<std::optional<std::int64_t>, 2> fixedAxes{};
};

struct MaskLengthProgram
{
  std::string mask;
  std::uint64_t batchAxis = 0;
  std::uint64_t contextAxis = 1;
  std::int64_t offset = 0;
  std::int32_t elementType = 7;
  std::optional<std::int64_t> fixedBatch;
  std::uint64_t castLimit = std::numeric_limits<std::uint64_t>::max();
};

class AttentionBoundaryProof
{
public:
  AttentionBoundaryProof(const AutomaticOnnxContract& contract,
                         const NativeAssemblyControl& control)
    : m_contract(contract)
    , m_control(control)
    , m_limit(std::min(control.maxAssembledBytes, NativeCanonicalMaterialPublicationMaxBytes))
  {
    active();
    for (const auto& tensor : contract.tensors) {
      charge(sizeof(tensor) + tensor.name.size());
      if (!m_tensors.emplace(tensor.name, &tensor).second)
        rejectState("AUTOMATIC_STATE_TENSOR_DUPLICATE");
    }
    for (const auto& input : contract.modelInputs) {
      charge(64 + input.size());
      if (!m_inputs.insert(input).second) rejectState("AUTOMATIC_STATE_PORT_DUPLICATE");
    }
    for (const auto& output : contract.modelOutputs) {
      charge(64 + output.size());
      if (!m_outputs.insert(output).second) rejectState("AUTOMATIC_STATE_PORT_DUPLICATE");
    }
    for (const auto& op : contract.operators) {
      if (op.scope != "graph") continue;
      charge(sizeof(op));
      for (const auto& output : op.outputs) {
        if (output.empty()) continue;
        const auto name = scoped(output);
        charge(96 + name.size());
        if (m_inputs.count(name) || !m_producers.emplace(name, &op).second)
          rejectState("AUTOMATIC_STATE_SSA");
      }
      for (const auto& input : op.inputs) {
        if (input.empty()) continue;
        const auto name = scoped(input);
        charge(96 + name.size());
        m_consumers[name].push_back(&op);
      }
    }
  }

  std::vector<AutomaticAttentionState>
  derive()
  {
    std::vector<AutomaticAttentionState> result;
    std::set<std::string> usedInputs, usedOutputs;
    for (std::size_t index = 0; index < m_contract.operators.size(); ++index) {
      active();
      const auto& op = m_contract.operators[index];
      if (op.scope != "graph" || op.domain != "com.microsoft" ||
          op.opType != "GroupQueryAttention" || op.opset != 1 || op.atomicBody ||
          !op.functionScope.empty() || !op.overload.empty()) continue;
      if (op.inputs.size() < 7 || op.outputs.size() < 3 ||
          !op.attributeReferences.empty()) rejectState("AUTOMATIC_STATE_GQA_PORTS");
      for (std::size_t port = 0; port < 7; ++port)
        if (op.inputs[port].empty()) rejectState("AUTOMATIC_STATE_GQA_PORTS");
      if (op.outputs[1].empty() || op.outputs[2].empty())
        rejectState("AUTOMATIC_STATE_GQA_PORTS");
      // Quantized KV has a different storage/scale contract. Weight quantization
      // does not enter this test and never changes the cache element type.
      if ((op.inputs.size() > 12 && !op.inputs[12].empty()) ||
          (op.inputs.size() > 13 && !op.inputs[13].empty()) ||
          (op.integerAttributes.count("kv_cache_bit_width") &&
           op.integerAttributes.at("kv_cache_bit_width") != 8))
        rejectState("AUTOMATIC_STATE_QUANTIZED_CACHE_UNPROVEN");
      const auto heads = positiveAttribute(op, "kv_num_heads");
      const auto queryHeads = positiveAttribute(op, "num_heads");
      if (queryHeads % heads != 0) rejectState("AUTOMATIC_STATE_HEADS");
      for (std::size_t role = 0; role < 2; ++role) {
        charge(sizeof(AutomaticAttentionState));
        auto past = inputBoundary(scoped(op.inputs[3 + role]));
        auto present = outputBoundary(scoped(op.outputs[1 + role]));
        if (!usedInputs.insert(past.first).second ||
            !usedOutputs.insert(present.first).second)
          rejectState("GENERATION_CONTRACT_AMBIGUOUS");
        const auto& input = boundaryTensor(past.first);
        const auto& output = boundaryTensor(present.first);
        if (!input.wireElementType || input.wireElementType != output.wireElementType ||
            (*input.wireElementType != TensorElementType::Float32 &&
             *input.wireElementType != TensorElementType::Float16 &&
             *input.wireElementType != TensorElementType::BFLOAT16))
          rejectState("AUTOMATIC_STATE_TYPE");
        const auto type = input.onnxElementType;
        validatePathTypes(scoped(op.inputs[3 + role]), past.first, type, input,
                          past.second, IDENTITY_AXES);
        validatePathTypes(present.first, scoped(op.outputs[1 + role]), type, output,
                          present.second, present.second);
        std::uint64_t headSize = 0;
        for (const auto formal : {1U, 3U}) {
          const auto a = fixedExtent(input, past.second[formal]);
          const auto b = fixedExtent(output, present.second[formal]);
          if (a == 0 || a != b || (formal == 1 && a != heads))
            rejectState("AUTOMATIC_STATE_STATIC_AXIS");
          if (formal == 3) headSize = a;
        }
        // A fixed-capacity cache requires a distinct initial-state policy.
        const auto& pastAxis = input.dimensions->at(past.second[2]);
        const auto& presentAxis = output.dimensions->at(present.second[2]);
        if ((pastAxis.kind == DimensionConstraint::Kind::FIXED && pastAxis.fixed != 0) ||
            presentAxis.kind == DimensionConstraint::Kind::FIXED)
          rejectState("AUTOMATIC_STATE_FIXED_CAPACITY_UNPROVEN");
        compatibleBatch(input.dimensions->at(past.second[0]),
                        output.dimensions->at(present.second[0]));
        for (std::size_t port = 0; port < 3; ++port) {
          const auto count = port == 0 ? queryHeads : heads;
          if (headSize > std::numeric_limits<std::uint64_t>::max() / count)
            rejectState("AUTOMATIC_STATE_WIDTH_OVERFLOW");
          projection(scoped(op.inputs[port]), type, count * headSize);
          if (scalarType(scoped(op.inputs[port])) != type)
            rejectState("AUTOMATIC_STATE_PROJECTION_TYPE");
        }
        AutomaticAttentionState state;
        state.role = role == 0 ? AutomaticAttentionState::Role::KEY :
                                 AutomaticAttentionState::Role::VALUE;
        state.operatorIndex = index;
        state.input = past.first; state.output = present.first;
        state.query = scoped(op.inputs[0]); state.key = scoped(op.inputs[1]);
        state.value = scoped(op.inputs[2]); state.sequenceLengths = scoped(op.inputs[5]);
        state.totalLength = scoped(op.inputs[6]);
        state.elementType = *input.wireElementType;
        state.inputAxes = past.second; state.outputAxes = present.second;
        state.heads = heads; state.headSize = headSize;
        for (const auto* name : {&state.input, &state.output, &state.query, &state.key,
                                 &state.value, &state.sequenceLengths, &state.totalLength})
          charge(name->size());
        if (role == 1) {
          const auto& keyState = result.back();
          if (keyState.headSize != state.headSize || keyState.elementType != state.elementType)
            rejectState("AUTOMATIC_STATE_KV_LAYOUT");
          compatibleBatch(boundaryTensor(keyState.input).dimensions->at(keyState.inputAxes[0]),
                          input.dimensions->at(past.second[0]));
        }
        result.push_back(std::move(state));
      }
    }
    active();
    return result;
  }

  std::optional<AutomaticGenerationContract>
  generation()
  {
    const auto states = derive();
    if (states.empty()) return std::nullopt;
    std::set<std::string> stateOutputs;
    for (const auto& state : states) stateOutputs.insert(state.output);
    const auto scoreCount = std::count_if(m_contract.modelOutputs.begin(), m_contract.modelOutputs.end(),
      [&](const auto& output) { return !stateOutputs.count(output); });
    if (scoreCount != 1) return std::nullopt;
    std::optional<AutomaticGenerationContract> result;
    for (const auto& output : m_contract.modelOutputs) {
      if (stateOutputs.count(output)) continue;
      const auto* scores = fact(output);
      if (!scores || !scores->dimensions || scores->dimensions->size() != 3 ||
          (scores->onnxElementType != 1 && scores->onnxElementType != 10 &&
           scores->onnxElementType != 16)) continue;
      for (std::uint64_t vocabularyAxis = 0; vocabularyAxis < 3; ++vocabularyAxis) {
        const auto& dimension = scores->dimensions->at(vocabularyAxis);
        if (dimension.kind != DimensionConstraint::Kind::FIXED || dimension.fixed <= 0) continue;
        const auto vocabulary = static_cast<std::uint64_t>(dimension.fixed);
        std::vector<std::uint64_t> other;
        for (std::uint64_t axis = 0; axis < 3; ++axis)
          if (axis != vocabularyAxis) other.push_back(axis);
        for (const auto swapped : {false, true}) {
          const std::array<std::uint64_t, 2> scoreAxes{
            other[swapped ? 1 : 0], other[swapped ? 0 : 1]};
          const auto tokens = tokenAxes(output, scoreAxes, vocabulary);
          if (!tokens || !unitFactors(*tokens)) continue;
          auto candidate = generationCandidate(states, output, scoreAxes, vocabulary, *tokens);
          if (!candidate) continue;
          candidate->logitsVocabularyAxis = vocabularyAxis;
          if (result) rejectState("GENERATION_CONTRACT_AMBIGUOUS");
          result = std::move(candidate);
        }
      }
    }
    return result;
  }

private:
  const AutomaticTensorContract*
  fact(const std::string& name) const
  {
    const auto found = m_tensors.find(name);
    return found == m_tensors.end() ? nullptr : found->second;
  }

  const AutomaticOnnxOperatorFact*
  producer(const std::string& name) const
  {
    const auto found = m_producers.find(name);
    return found == m_producers.end() ? nullptr : found->second;
  }

  static bool
  standard(const AutomaticOnnxOperatorFact& op)
  {
    return (op.domain.empty() || op.domain == "ai.onnx") && op.opset >= 1 && op.opset <= 21 &&
      !op.atomicBody && op.functionScope.empty() && op.overload.empty() &&
      op.attributeReferences.empty();
  }

  static std::int64_t
  attribute(const AutomaticOnnxOperatorFact& op, const char* key, std::int64_t fallback)
  {
    if (op.attributeReferences.count(key)) rejectState("AUTOMATIC_GENERATION_ATTRIBUTE");
    const auto found = op.integerAttributes.find(key);
    return found == op.integerAttributes.end() ? fallback : found->second;
  }

  static std::uint64_t
  axisIndex(std::int64_t axis, std::uint64_t rank)
  {
    if (rank > 64) rejectState("AUTOMATIC_GENERATION_RANK");
    if (axis < 0) axis += static_cast<std::int64_t>(rank);
    if (axis < 0 || static_cast<std::uint64_t>(axis) >= rank)
      rejectState("AUTOMATIC_GENERATION_AXIS");
    return static_cast<std::uint64_t>(axis);
  }

  std::optional<std::uint64_t>
  rank(const std::string& name) const
  {
    const auto* tensor = fact(name);
    if (!tensor || !tensor->dimensions) return std::nullopt;
    return tensor->dimensions->size();
  }

  std::optional<IntegerShapeProgram>
  shapeProgram(const std::string& name, unsigned depth = 0)
  {
    work();
    const auto found = m_shapeMemo.find(name);
    if (found != m_shapeMemo.end()) return found->second;
    charge(256 + 2 * name.size());
    if (!m_pendingShapes.insert(name).second) rejectState("AUTOMATIC_GENERATION_PROGRAM_CYCLE");
    auto result = computeShapeProgram(name, depth);
    if (result) for (const auto& value : result->values)
      charge(sizeof(IntegerShapeValue) + value.axis.tensor.size());
    m_pendingShapes.erase(name);
    m_shapeMemo.emplace(name, result);
    return result;
  }

  std::optional<IntegerShapeProgram>
  computeShapeProgram(const std::string& name, unsigned depth)
  {
    charge(128 + name.size());
    if (depth >= 64) rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
    const auto literal = m_contract.integerLiterals.find(name);
    if (literal != m_contract.integerLiterals.end()) {
      const auto count = rank(name);
      if (!count || *count > 1) return std::nullopt;
      IntegerShapeProgram result;
      result.scalar = *count == 0;
      result.elementType = fact(name)->onnxElementType;
      for (const auto value : literal->second) {
        charge(sizeof(IntegerShapeValue));
        result.values.push_back({value, {}});
      }
      return result;
    }
    const auto* op = producer(name);
    if (!op || !standard(*op) || op->outputs.size() != 1) return std::nullopt;
    if (op->opType == "Shape" && op->inputs.size() == 1) {
      const auto input = scoped(op->inputs[0]);
      const auto count = rank(input);
      if (!count || *count > 64) return std::nullopt;
      const auto normalize = [&](std::int64_t value) {
        if (value < 0) value += static_cast<std::int64_t>(*count);
        return std::max<std::int64_t>(0, std::min<std::int64_t>(value, *count));
      };
      if (op->opset < 15 && (op->integerAttributes.count("start") ||
                            op->integerAttributes.count("end"))) return std::nullopt;
      const auto start = normalize(attribute(*op, "start", 0));
      const auto end = normalize(attribute(*op, "end", static_cast<std::int64_t>(*count)));
      IntegerShapeProgram result;
      for (auto axis = start; axis < end; ++axis) {
        auto reference = canonicalAxis({input, static_cast<std::uint64_t>(axis)});
        charge(sizeof(IntegerShapeValue) + reference.tensor.size());
        result.values.push_back({{}, std::move(reference)});
      }
      return result;
    }
    if ((op->opType == "Identity" || op->opType == "Cast") && op->inputs.size() == 1) {
      auto result = shapeProgram(scoped(op->inputs[0]), depth + 1);
      if (!result) return std::nullopt;
      if (op->opType == "Cast") {
        if (op->opset < 6) return std::nullopt;
        const auto to = attribute(*op, "to", 0);
        if (to != 6 && to != 7) return std::nullopt;
        result->elementType = static_cast<std::int32_t>(to);
        if (to == 6) {
          result->castLimit = std::min(result->castLimit,
            std::uint64_t(std::numeric_limits<std::int32_t>::max()));
          for (const auto& value : result->values)
            if (value.literal && (*value.literal < std::numeric_limits<std::int32_t>::min() ||
                                  *value.literal > std::numeric_limits<std::int32_t>::max()))
              return std::nullopt;
        }
      }
      return result;
    }
    if (op->opType == "Gather" && op->inputs.size() == 2 &&
        (attribute(*op, "axis", 0) == 0 || attribute(*op, "axis", 0) == -1)) {
      const auto data = shapeProgram(scoped(op->inputs[0]), depth + 1);
      const auto indices = shapeProgram(scoped(op->inputs[1]), depth + 1);
      if (!data || !indices || data->scalar) return std::nullopt;
      IntegerShapeProgram result;
      result.scalar = indices->scalar;
      result.elementType = data->elementType;
      result.castLimit = std::min(data->castLimit, indices->castLimit);
      for (const auto& index : indices->values) {
        if (!index.literal) return std::nullopt;
        const auto axis = axisIndex(*index.literal, data->values.size());
        charge(sizeof(IntegerShapeValue) + data->values.at(axis).axis.tensor.size());
        result.values.push_back(data->values.at(axis));
      }
      return result;
    }
    if (op->opType == "Concat" && op->opset >= 4 && !op->inputs.empty() &&
        attribute(*op, "axis", -1) == 0) {
      IntegerShapeProgram result;
      bool first = true;
      for (const auto& input : op->inputs) {
        const auto part = shapeProgram(scoped(input), depth + 1);
        if (!part || part->scalar || part->values.size() > 64 - result.values.size())
          return std::nullopt;
        if (!first && result.elementType != part->elementType) return std::nullopt;
        result.elementType = part->elementType;
        first = false;
        result.castLimit = std::min(result.castLimit, part->castLimit);
        for (const auto& value : part->values)
          charge(sizeof(IntegerShapeValue) + value.axis.tensor.size());
        result.values.insert(result.values.end(), part->values.begin(), part->values.end());
      }
      return result;
    }
    if (op->opType == "Unsqueeze" && (op->inputs.size() == 1 || op->inputs.size() == 2)) {
      auto result = shapeProgram(scoped(op->inputs[0]), depth + 1);
      if (!result || !result->scalar) return std::nullopt;
      std::vector<std::int64_t> axes;
      if (op->opset >= 13 && op->inputs.size() == 2) {
        const auto encoded = shapeProgram(scoped(op->inputs[1]), depth + 1);
        if (!encoded || encoded->elementType != 7) return std::nullopt;
        for (const auto& axis : encoded->values) {
          if (!axis.literal) return std::nullopt;
          axes.push_back(*axis.literal);
        }
      }
      else if (op->opset <= 12) {
        const auto encoded = op->integerListAttributes.find("axes");
        if (encoded == op->integerListAttributes.end()) return std::nullopt;
        axes = encoded->second;
      }
      if (axes.size() != 1 || (axes[0] != 0 && axes[0] != -1)) return std::nullopt;
      result->scalar = false;
      return result;
    }
    return std::nullopt;
  }

  TensorAxisReference
  canonicalAxis(TensorAxisReference axis)
  {
    for (unsigned depth = 0; depth < 64; ++depth) {
      charge(64 + axis.tensor.size());
      if (m_inputs.count(axis.tensor)) return axis;
      const auto* op = producer(axis.tensor);
      if (!op || !standard(*op) || op->inputs.size() != 1 || op->outputs.size() != 1)
        return axis;
      if (op->opType == "Identity" || (op->opType == "Cast" && op->opset >= 6)) {
        axis.tensor = scoped(op->inputs[0]);
      }
      else if (op->opType == "Transpose") {
        const auto count = rank(scoped(op->inputs[0]));
        if (!count || *count > 64 || axis.axis >= *count) return axis;
        const auto permutation = op->integerListAttributes.find("perm");
        if (permutation == op->integerListAttributes.end()) axis.axis = *count - axis.axis - 1;
        else {
          if (permutation->second.size() != *count)
            rejectState("AUTOMATIC_GENERATION_PERMUTATION");
          std::set<std::uint64_t> seen;
          for (const auto from : permutation->second) {
            if (from < 0 || static_cast<std::uint64_t>(from) >= *count ||
                !seen.insert(static_cast<std::uint64_t>(from)).second)
              rejectState("AUTOMATIC_GENERATION_PERMUTATION");
          }
          axis.axis = static_cast<std::uint64_t>(permutation->second.at(axis.axis));
        }
        axis.tensor = scoped(op->inputs[0]);
      }
      else return axis;
    }
    rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
  }

  std::optional<MaskLengthProgram>
  maskProgram(const std::string& name, unsigned depth = 0)
  {
    charge(128 + name.size());
    if (depth >= 64) rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
    const auto* op = producer(name);
    if (!op || !standard(*op) || op->outputs.size() != 1) return std::nullopt;
    if ((op->opType == "Identity" || op->opType == "Cast") && op->inputs.size() == 1) {
      auto result = maskProgram(scoped(op->inputs[0]), depth + 1);
      if (!result) return std::nullopt;
      if (op->opType == "Cast") {
        if (op->opset < 6) return std::nullopt;
        const auto to = attribute(*op, "to", 0);
        if (to != 6 && to != 7) return std::nullopt;
        result->elementType = static_cast<std::int32_t>(to);
        if (to == 6) result->castLimit = std::min(result->castLimit,
          std::uint64_t(std::numeric_limits<std::int32_t>::max()));
      }
      return result;
    }
    if (op->opType == "Sub" && op->opset >= 7 && op->inputs.size() == 2) {
      auto result = maskProgram(scoped(op->inputs[0]), depth + 1);
      const auto constant = shapeProgram(scoped(op->inputs[1]));
      if (!result || result->offset != 0 || !constant || constant->values.size() != 1 ||
          constant->elementType != result->elementType ||
          constant->values[0].literal != std::optional<std::int64_t>(1)) return std::nullopt;
      result->offset = -1;
      result->castLimit = std::min(result->castLimit, constant->castLimit);
      return result;
    }
    if (op->opType != "ReduceSum" || op->inputs.empty() || op->inputs.size() > 2)
      return std::nullopt;
    const auto input = scoped(op->inputs[0]);
    if (rank(input) != std::optional<std::uint64_t>(2)) return std::nullopt;
    const auto* reduced = fact(input);
    if (!reduced || (reduced->onnxElementType != 6 && reduced->onnxElementType != 7))
      return std::nullopt;
    std::vector<std::int64_t> axes;
    if (op->opset >= 13 && op->inputs.size() == 2 && !op->inputs[1].empty()) {
      const auto encoded = shapeProgram(scoped(op->inputs[1]));
      if (!encoded || encoded->scalar || encoded->elementType != 7) return std::nullopt;
      for (const auto& value : encoded->values) {
        if (!value.literal) return std::nullopt;
        axes.push_back(*value.literal);
      }
    }
    else if (op->opset <= 12) {
      const auto encoded = op->integerListAttributes.find("axes");
      if (encoded == op->integerListAttributes.end()) return std::nullopt;
      axes = encoded->second;
    }
    const auto keep = attribute(*op, "keepdims", 1);
    if (axes.size() != 1 || (keep != 0 && keep != 1)) return std::nullopt;
    const auto context = axisIndex(axes[0], 2);
    const auto root = integerInputAxes(input, {1 - context, context}, 1);
    if (!root || integerPortType(input) != reduced->onnxElementType) return std::nullopt;
    const auto* mask = fact(root->input);
    if (!mask || (mask->onnxElementType != 6 && mask->onnxElementType != 7))
      return std::nullopt;
    // A fixed public mask cannot grow through prefill and subsequent decode.
    if (root->fixedAxes[1])
      return std::nullopt;
    MaskLengthProgram result;
    result.mask = root->input;
    result.batchAxis = root->axes[0];
    result.contextAxis = root->axes[1];
    result.elementType = reduced->onnxElementType;
    result.fixedBatch = root->fixedAxes[0];
    if (reduced->onnxElementType == 6) result.castLimit =
      std::numeric_limits<std::int32_t>::max();
    return result;
  }

  static bool
  constantHeader(const AutomaticTensorContract& tensor)
  {
    return tensor.provenance == std::vector<std::string>{"initializer-header-v1"} ||
      tensor.provenance == std::vector<std::string>{"constant-header-v1"};
  }

  static bool
  unitFactors(const TokenAxes& axes)
  {
    return axes.factors[0] == std::make_pair(std::uint64_t{1}, std::uint64_t{1}) &&
      axes.factors[1] == std::make_pair(std::uint64_t{1}, std::uint64_t{1});
  }

  static void
  mergeFixed(std::optional<std::int64_t>& known, std::optional<std::int64_t> value)
  {
    if (!value) return;
    if (*value <= 0) rejectState("AUTOMATIC_GENERATION_AUXILIARY_SHAPE");
    if (known && *known != *value) rejectState("AUTOMATIC_GENERATION_AUXILIARY_SHAPE");
    known = value;
  }

  static void
  multiplyFactor(std::pair<std::uint64_t, std::uint64_t>& factor,
                   std::uint64_t numerator, std::uint64_t denominator)
  {
    if (numerator == 0 || denominator == 0) rejectState("AUTOMATIC_GENERATION_RESHAPE_ZERO");
    auto divisor = std::gcd(numerator, denominator);
    numerator /= divisor; denominator /= divisor;
    divisor = std::gcd(factor.first, denominator);
    factor.first /= divisor; denominator /= divisor;
    divisor = std::gcd(factor.second, numerator);
    factor.second /= divisor; numerator /= divisor;
    if (factor.first > std::numeric_limits<std::uint64_t>::max() / numerator ||
        factor.second > std::numeric_limits<std::uint64_t>::max() / denominator)
      rejectState("AUTOMATIC_GENERATION_RESHAPE_OVERFLOW");
    factor.first *= numerator; factor.second *= denominator;
  }

  std::optional<std::uint64_t>
  featureWidth(const std::string& name, unsigned depth = 0)
  {
    work();
    const auto found = m_widthMemo.find(name);
    if (found != m_widthMemo.end()) return found->second;
    charge(256 + 2 * name.size());
    if (!m_pendingWidths.insert(name).second) rejectState("AUTOMATIC_GENERATION_DATA_CYCLE");
    const auto result = computeFeatureWidth(name, depth);
    m_pendingWidths.erase(name);
    m_widthMemo.emplace(name, result);
    return result;
  }

  std::optional<std::array<std::uint64_t, 2>>
  constantMatrixShape(std::string name)
  {
    std::array<std::uint64_t, 2> axes{0, 1};
    std::array<std::optional<std::int64_t>, 2> fixed;
    for (unsigned depth = 0; depth < 64; ++depth) {
      work();
      charge(128 + name.size());
      const auto* tensor = fact(name);
      if (tensor && tensor->dimensions) {
        if (tensor->dimensions->size() != 2) return std::nullopt;
        for (std::size_t axis = 0; axis < axes.size(); ++axis) {
          const auto& dimension = tensor->dimensions->at(axes[axis]);
          if (dimension.kind == DimensionConstraint::Kind::FIXED)
            mergeFixed(fixed[axis], dimension.fixed);
        }
      }
      if (tensor && constantHeader(*tensor) && tensor->dimensions) {
        const std::array<std::uint64_t, 2> shape{
          fixedExtent(*tensor, axes[0]), fixedExtent(*tensor, axes[1])};
        if (shape[0] == 0 || shape[1] == 0) return std::nullopt;
        return shape;
      }
      const auto* op = producer(name);
      if (!op || !standard(*op) || op->atomicBody || !op->functionScope.empty() ||
          !op->overload.empty() || !op->attributeReferences.empty() ||
          op->inputs.empty() || op->outputs.size() != 1) return std::nullopt;
      if (op->opType == "DequantizeLinear" && op->opset >= 10 &&
          op->inputs.size() >= 2 && op->inputs.size() <= 3) {
        if (op->inputs[1].empty()) return std::nullopt;
        for (std::size_t port = 1; port < op->inputs.size(); ++port) {
          if (op->inputs[port].empty()) continue;
          const auto* parameter = fact(scoped(op->inputs[port]));
          if (!parameter || !constantHeader(*parameter)) return std::nullopt;
        }
      }
      else if (op->inputs.size() != 1) return std::nullopt;
      else if (op->opType == "Transpose") {
        const auto permutation = op->integerListAttributes.find("perm");
        if (permutation == op->integerListAttributes.end() ||
            permutation->second == std::vector<std::int64_t>{1, 0}) {
          for (auto& axis : axes) axis = 1 - axis;
        }
        else if (permutation->second != std::vector<std::int64_t>{0, 1})
          rejectState("AUTOMATIC_GENERATION_PERMUTATION");
      }
      else if (op->opType != "Identity") return std::nullopt;
      name = scoped(op->inputs[0]);
    }
    rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
  }

  std::int32_t
  scalarType(const std::string& name, unsigned depth = 0)
  {
    work();
    const auto found = m_typeMemo.find(name);
    if (found != m_typeMemo.end()) return found->second;
    charge(256 + 2 * name.size());
    if (!m_pendingTypes.insert(name).second) rejectState("AUTOMATIC_GENERATION_DATA_CYCLE");
    const auto result = computeScalarType(name, depth);
    const auto* tensor = fact(name);
    if (result != 0 && tensor && tensor->onnxElementType != 0 &&
        result != tensor->onnxElementType) rejectState("AUTOMATIC_GENERATION_TYPE_CONFLICT");
    m_pendingTypes.erase(name);
    m_typeMemo.emplace(name, result);
    return result;
  }

  std::int32_t
  computeScalarType(const std::string& name, unsigned depth)
  {
    if (depth >= 512) rejectState("AUTOMATIC_GENERATION_DATA_DEPTH");
    const auto* tensor = fact(name);
    if (tensor && (constantHeader(*tensor) || m_inputs.count(name))) return tensor->onnxElementType;
    const auto* op = producer(name);
    const auto fallback = [&] {
      return tensor && tensor->provenance == std::vector<std::string>{"onnx-inference-v1"}
        ? tensor->onnxElementType : 0;
    };
    if (!op || op->atomicBody || !op->functionScope.empty() || !op->overload.empty() ||
        !op->attributeReferences.empty() || op->inputs.empty()) return fallback();
    const auto first = scoped(op->inputs[0]);
    const auto mainOutput = !op->outputs.empty() && scoped(op->outputs[0]) == name;
    const auto combine = [&](std::int32_t a, std::int32_t b) {
      if (a != 0 && b != 0 && a != b) rejectState("AUTOMATIC_GENERATION_TYPE_CONFLICT");
      return a == 0 ? b : a;
    };
    if (standard(*op)) {
      if (op->opType == "Cast" && op->opset >= 6) {
        const auto to = attribute(*op, "to", 0);
        if (to > 0 && to <= std::numeric_limits<std::int32_t>::max())
          return static_cast<std::int32_t>(to);
        return 0;
      }
      if (op->opType == "Shape") return 7;
      if (op->opType == "MatMulInteger" && op->opset >= 10) return 6;
      if (op->opType == "DequantizeLinear" && op->opset >= 10 &&
          op->inputs.size() >= 2 && op->inputs.size() <= 3) {
        const auto scale = scalarType(scoped(op->inputs[1]), depth + 1);
        if (op->opset < 19) {
          if (scale != 0 && scale != 1) rejectState("AUTOMATIC_GENERATION_TYPE_CONFLICT");
          return 1;
        }
        return scale;
      }
      if (op->opType == "DynamicQuantizeLinear" && op->opset >= 11) {
        if (mainOutput || (op->outputs.size() >= 3 && scoped(op->outputs[2]) == name)) return 2;
        if (op->outputs.size() >= 2 && scoped(op->outputs[1]) == name) return 1;
        return 0;
      }
      if (op->opType == "SimplifiedLayerNormalization" || op->opType == "LayerNormalization") {
        if (!mainOutput) return static_cast<std::int32_t>(attribute(*op, "stash_type", 1));
        const auto input = op->opType == "SimplifiedLayerNormalization" || op->opset < 17 ? 1U : 0U;
        return op->inputs.size() > input ? scalarType(scoped(op->inputs[input]), depth + 1) : 0;
      }
      if (op->opType == "MatMul" || op->opType == "Mul" || op->opType == "Add" ||
          op->opType == "Sub" || op->opType == "Div" || op->opType == "Pow") {
        if (op->inputs.size() != 2) return 0;
        return combine(scalarType(first, depth + 1),
                       scalarType(scoped(op->inputs[1]), depth + 1));
      }
      if (op->opType == "Concat") {
        std::int32_t result = 0;
        for (const auto& input : op->inputs)
          result = combine(result, scalarType(scoped(input), depth + 1));
        return result;
      }
      if (op->opType == "Identity" || op->opType == "Transpose" || op->opType == "Reshape" ||
          op->opType == "Squeeze" || op->opType == "Unsqueeze" || op->opType == "Gather" ||
          op->opType == "ReduceSum" || op->opType == "Relu" || op->opType == "Sigmoid" ||
          op->opType == "Tanh" || op->opType == "Sqrt" || op->opType == "Exp" ||
          op->opType == "Log" || op->opType == "Erf" || op->opType == "Abs" ||
          op->opType == "Neg") return scalarType(first, depth + 1);
    }
    if (op->domain == "com.microsoft" && op->opset == 1) {
      if (op->opType == "MatMulNBits" && op->inputs.size() >= 3)
        return combine(scalarType(first, depth + 1),
                       scalarType(scoped(op->inputs[2]), depth + 1));
      if (op->opType == "RotaryEmbedding" && op->inputs.size() == 4) {
        if (scalarType(scoped(op->inputs[1]), depth + 1) != 7)
          rejectState("AUTOMATIC_GENERATION_POSITION_TYPE");
        return combine(combine(scalarType(first, depth + 1),
                               scalarType(scoped(op->inputs[2]), depth + 1)),
                       scalarType(scoped(op->inputs[3]), depth + 1));
      }
      if (op->opType == "SkipSimplifiedLayerNormalization" && op->inputs.size() >= 3) {
        const auto residual = op->outputs.size() >= 4 && scoped(op->outputs[3]) == name;
        if (!mainOutput && !residual) return 1;
        return combine(combine(scalarType(first, depth + 1),
                               scalarType(scoped(op->inputs[1]), depth + 1)),
                       scalarType(scoped(op->inputs[2]), depth + 1));
      }
      if (op->opType == "GroupQueryAttention") {
        if (mainOutput) return scalarType(first, depth + 1);
        if (op->outputs.size() >= 3 && op->inputs.size() >= 5) {
          if (scoped(op->outputs[1]) == name) return scalarType(scoped(op->inputs[3]), depth + 1);
          if (scoped(op->outputs[2]) == name) return scalarType(scoped(op->inputs[4]), depth + 1);
        }
      }
    }
    return fallback();
  }

  std::optional<std::uint64_t>
  computeFeatureWidth(const std::string& name, unsigned depth)
  {
    charge(96 + name.size());
    if (depth >= 512) rejectState("AUTOMATIC_GENERATION_DATA_DEPTH");
    const auto* tensor = fact(name);
    if (tensor && constantHeader(*tensor) && tensor->dimensions) {
      if (tensor->dimensions->empty()) return 1;
      return fixedExtent(*tensor, tensor->dimensions->size() - 1);
    }
    const auto* op = producer(name);
    if (!op || op->atomicBody || !op->functionScope.empty() || !op->overload.empty() ||
        !op->attributeReferences.empty() || op->inputs.empty()) return std::nullopt;
    const auto first = scoped(op->inputs[0]);
    if (standard(*op) && op->opType == "Transpose" && op->inputs.size() == 1) {
      const auto permutation = op->integerListAttributes.find("perm");
      if (permutation == op->integerListAttributes.end()) return std::nullopt;
      const auto& order = permutation->second;
      if (order == std::vector<std::int64_t>{0, 1, 2} ||
          order == std::vector<std::int64_t>{1, 0, 2})
        return featureWidth(first, depth + 1);
      return std::nullopt;
    }
    if (standard(*op) && op->opType == "DynamicQuantizeLinear" && op->opset >= 11 &&
        op->outputs.size() == 3 && (scoped(op->outputs[1]) == name ||
                                  scoped(op->outputs[2]) == name)) return 1;
    if (standard(*op) && op->opType == "Gather" && op->inputs.size() == 2 &&
        attribute(*op, "axis", 0) == 0) {
      const auto table = constantMatrixShape(first);
      if (table) return table->at(1);
    }
    if (standard(*op) && (op->opType == "MatMul" || op->opType == "MatMulInteger") &&
        op->inputs.size() >= 2) {
      const auto weights = constantMatrixShape(scoped(op->inputs[1]));
      if (weights) return weights->at(1);
      return std::nullopt;
    }
    if (op->domain == "com.microsoft" && op->opType == "MatMulNBits" && op->opset == 1) {
      const auto width = attribute(*op, "N", 0);
      if (width > 0) return static_cast<std::uint64_t>(width);
      return std::nullopt;
    }
    if (standard(*op) && op->opType == "Reshape" && op->inputs.size() == 2) {
      const auto target = shapeProgram(scoped(op->inputs[1]));
      if (!target || target->scalar || target->elementType != 7 || target->values.size() != 3)
        return std::nullopt;
      const auto& last = target->values[2];
      if (last.literal && *last.literal > 0) return static_cast<std::uint64_t>(*last.literal);
      if (last.literal == std::optional<std::int64_t>(0) && attribute(*op, "allowzero", 0) == 0)
        return featureWidth(first, depth + 1);
      return std::nullopt;
    }
    if (standard(*op) && (op->opType == "Mul" || op->opType == "Add" ||
        op->opType == "Sub" || op->opType == "Div") && op->inputs.size() == 2) {
      const auto a = featureWidth(first, depth + 1);
      const auto b = featureWidth(scoped(op->inputs[1]), depth + 1);
      if (a && b) {
        if (*a == *b) return a;
        if (*a == 1) return b;
        if (*b == 1) return a;
        return std::nullopt;
      }
      if (a && *a > 1) return a;
      if (b && *b > 1) return b;
      return std::nullopt;
    }
    const bool unary = standard(*op) && (op->opType == "Identity" || op->opType == "Cast" ||
      op->opType == "Relu" || op->opType == "Sigmoid" || op->opType == "Tanh" ||
      op->opType == "Sqrt" || op->opType == "Exp" || op->opType == "Log" ||
      op->opType == "Erf" || op->opType == "Abs" || op->opType == "Neg" ||
      op->opType == "DynamicQuantizeLinear" || op->opType == "LayerNormalization" ||
      op->opType == "SimplifiedLayerNormalization" ||
      (op->opType == "DequantizeLinear" && op->opset >= 10 &&
       op->inputs.size() >= 2 && op->inputs.size() <= 3));
    const bool fused = op->domain == "com.microsoft" && op->opset == 1 &&
      (op->opType == "RotaryEmbedding" || op->opType == "SkipSimplifiedLayerNormalization" ||
       op->opType == "GroupQueryAttention");
    if (unary || fused) {
      if (op->outputs.empty()) return std::nullopt;
      const bool mainOutput = !op->outputs[0].empty() && scoped(op->outputs[0]) == name;
      const bool residual = op->opType == "SkipSimplifiedLayerNormalization" &&
        op->outputs.size() >= 4 && !op->outputs[3].empty() && scoped(op->outputs[3]) == name;
      if (mainOutput || residual) return featureWidth(first, depth + 1);
    }
    return std::nullopt;
  }

  std::optional<TokenAxes>
  integerInputAxes(std::string name, std::array<std::uint64_t, 2> axes,
                    std::uint64_t maximumValue)
  {
    std::array<std::optional<std::int64_t>, 2> fixed;
    for (unsigned depth = 0; depth < 64; ++depth) {
      charge(128 + name.size());
      if (axes[0] >= 2 || axes[1] >= 2 || axes[0] == axes[1]) return std::nullopt;
      const auto* tensor = fact(name);
      if (tensor && tensor->onnxElementType != 0 && tensor->onnxElementType != 6 &&
          tensor->onnxElementType != 7) return std::nullopt;
      if (tensor && tensor->dimensions) {
        if (tensor->dimensions->size() != 2) return std::nullopt;
        for (std::size_t logical = 0; logical < 2; ++logical) {
          const auto& dim = tensor->dimensions->at(axes[logical]);
          if (dim.kind != DimensionConstraint::Kind::FIXED) continue;
          if (fixed[logical] && *fixed[logical] != dim.fixed)
            rejectState("AUTOMATIC_GENERATION_ALIAS_DIMENSION");
          fixed[logical] = dim.fixed;
        }
      }
      if (m_inputs.count(name)) {
        if (!tensor || !tensor->dimensions || tensor->dimensions->size() != 2 ||
            (tensor->onnxElementType != 6 && tensor->onnxElementType != 7) ||
            (tensor->onnxElementType == 6 && maximumValue >
              std::uint64_t(std::numeric_limits<std::int32_t>::max()))) return std::nullopt;
        TokenAxes result{std::move(name), axes};
        result.fixedAxes = fixed;
        return result;
      }
      const auto* op = producer(name);
      if (!op || !standard(*op) || op->inputs.size() != 1 || op->outputs.size() != 1)
        return std::nullopt;
      if (op->opType == "Cast" && op->opset >= 6) {
        const auto to = attribute(*op, "to", 0);
        if ((to != 6 && to != 7) || (to == 6 && maximumValue >
              std::uint64_t(std::numeric_limits<std::int32_t>::max()))) return std::nullopt;
      }
      else if (op->opType == "Transpose") {
        const auto perm = op->integerListAttributes.find("perm");
        std::array<std::uint64_t, 2> order{1, 0};
        if (perm != op->integerListAttributes.end()) {
          if (perm->second == std::vector<std::int64_t>{0, 1}) order = {0, 1};
          else if (perm->second != std::vector<std::int64_t>{1, 0})
            rejectState("AUTOMATIC_GENERATION_PERMUTATION");
        }
        for (auto& axis : axes) axis = order[axis];
      }
      else if (op->opType != "Identity") return std::nullopt;
      name = scoped(op->inputs[0]);
    }
    rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
  }

  std::int32_t
  integerPortType(std::string name)
  {
    for (unsigned depth = 0; depth < 64; ++depth) {
      charge(64 + name.size());
      if (m_inputs.count(name)) return fact(name) ? fact(name)->onnxElementType : 0;
      const auto* op = producer(name);
      if (!op || !standard(*op) || op->inputs.size() != 1) return 0;
      if (op->opType == "Cast" && op->opset >= 6) {
        const auto to = attribute(*op, "to", 0);
        return to == 6 || to == 7 ? static_cast<std::int32_t>(to) : 0;
      }
      if (op->opType != "Identity" && op->opType != "Transpose") return 0;
      name = scoped(op->inputs[0]);
    }
    rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
  }

  std::optional<TokenAxes>
  tokenAxes(const std::string& name, std::array<std::uint64_t, 2> axes,
             std::uint64_t vocabulary, unsigned depth = 0)
  {
    work();
    const auto key = std::make_tuple(name, axes, vocabulary);
    const auto found = m_tokenMemo.find(key);
    if (found != m_tokenMemo.end()) return found->second;
    charge(384 + 2 * name.size());
    if (!m_pendingTokens.insert(key).second) rejectState("AUTOMATIC_GENERATION_DATA_CYCLE");
    auto result = computeTokenAxes(name, axes, vocabulary, depth);
    const auto* tensor = fact(name);
    if (result && tensor && tensor->dimensions) {
      for (std::size_t logical = 0; logical < axes.size(); ++logical) {
        const auto& dimension = tensor->dimensions->at(axes[logical]);
        if (dimension.kind != DimensionConstraint::Kind::FIXED) continue;
        if (dimension.fixed <= 0) rejectState("AUTOMATIC_GENERATION_AUXILIARY_SHAPE");
        // A folded feature axis may scale the sequence extent. Translate the
        // actual fixed extent back to the root before merging its constraint.
        const auto [numerator, denominator] = result->factors[logical];
        const auto value = static_cast<std::uint64_t>(dimension.fixed);
        if (value % numerator != 0 ||
            value / numerator > static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()) / denominator)
          rejectState("AUTOMATIC_GENERATION_AUXILIARY_SHAPE");
        mergeFixed(result->fixedAxes[logical],
                   static_cast<std::int64_t>((value / numerator) * denominator));
      }
    }
    if (result) charge(sizeof(TokenAxes) + result->input.size());
    m_pendingTokens.erase(key);
    m_tokenMemo.emplace(key, result);
    return result;
  }

  std::optional<TokenAxes>
  computeTokenAxes(const std::string& name, std::array<std::uint64_t, 2> axes,
                    std::uint64_t vocabulary, unsigned depth)
  {
    charge(128 + name.size());
    if (depth >= 512) rejectState("AUTOMATIC_GENERATION_DATA_DEPTH");
    if (axes[0] == axes[1] || axes[0] >= 3 || axes[1] >= 3) return std::nullopt;
    const auto count = rank(name);
    if (count && *count != 3) return std::nullopt;
    const auto* op = producer(name);
    if (!op || op->inputs.empty() || op->atomicBody || !op->functionScope.empty() ||
        !op->overload.empty() || !op->attributeReferences.empty()) return std::nullopt;
    const auto first = scoped(op->inputs[0]);
    const bool normal = standard(*op);
    if (normal && op->opType == "Gather" && op->inputs.size() == 2 &&
        attribute(*op, "axis", 0) == 0) {
      const auto table = constantMatrixShape(first);
      if (!table || table->at(0) != vocabulary ||
          axes[0] > 1 || axes[1] > 1) return std::nullopt;
      if (vocabulary == 0) return std::nullopt;
      return integerInputAxes(scoped(op->inputs[1]), axes, vocabulary - 1);
    }
    if (normal && op->opType == "Transpose" && op->inputs.size() == 1) {
      const auto permutation = op->integerListAttributes.find("perm");
      std::array<std::uint64_t, 3> order{2, 1, 0};
      if (permutation != op->integerListAttributes.end()) {
        if (permutation->second.size() != 3) return std::nullopt;
        std::set<std::int64_t> seen;
        for (std::size_t i = 0; i < 3; ++i) {
          const auto from = permutation->second[i];
          if (from < 0 || from >= 3 || !seen.insert(from).second)
            rejectState("AUTOMATIC_GENERATION_PERMUTATION");
          order[i] = static_cast<std::uint64_t>(from);
        }
      }
      for (auto& axis : axes) axis = order[axis];
      return tokenAxes(first, axes, vocabulary, depth + 1);
    }
    if (normal && op->opType == "Reshape" && op->inputs.size() == 2 &&
        attribute(*op, "allowzero", 0) == 0 && axes[0] < 2 && axes[1] < 2) {
      const auto target = shapeProgram(scoped(op->inputs[1]));
      if (!target || target->scalar || target->elementType != 7 || target->values.size() != 3 ||
          target->values[0].literal != std::optional<std::int64_t>(0) ||
          target->values[1].literal != std::optional<std::int64_t>(-1)) return std::nullopt;
      const auto before = featureWidth(first);
      const auto after = featureWidth(name);
      if (!before || !after || *before == 0 || *after == 0) return std::nullopt;
      auto result = tokenAxes(first, axes, vocabulary, depth + 1);
      if (!result) return std::nullopt;
      for (std::size_t logical = 0; logical < 2; ++logical)
        if (axes[logical] == 1) multiplyFactor(result->factors[logical], *before, *after);
      return result;
    }
    const bool nBits = op->domain == "com.microsoft" && op->opType == "MatMulNBits" &&
                       op->opset == 1;
    if (op->domain == "com.microsoft" && op->opType == "GroupQueryAttention" &&
        op->opset == 1 && !op->outputs.empty() && scoped(op->outputs[0]) == name &&
        axes[0] < 2 && axes[1] < 2)
      return tokenAxes(first, axes, vocabulary, depth + 1);
    if ((normal && (op->opType == "MatMul" || op->opType == "MatMulInteger")) || nBits) {
      if (op->opType == "MatMulInteger" && op->opset < 10) return std::nullopt;
      if (op->inputs.size() < 2 || axes[0] > 1 || axes[1] > 1) return std::nullopt;
      if (!nBits && !constantMatrixShape(scoped(op->inputs[1]))) return std::nullopt;
      if (nBits) {
        const auto* weights = fact(scoped(op->inputs[1]));
        if (!weights || !constantHeader(*weights) || !weights->dimensions) return std::nullopt;
      }
      return tokenAxes(first, axes, vocabulary, depth + 1);
    }
    if (normal && (op->opType == "Identity" || op->opType == "Cast" ||
        op->opType == "Relu" || op->opType == "Sigmoid" || op->opType == "Tanh" ||
        op->opType == "Sqrt" || op->opType == "Exp" || op->opType == "Log" ||
        op->opType == "Erf" || op->opType == "Abs" || op->opType == "Neg" ||
        op->opType == "DynamicQuantizeLinear" || op->opType == "LayerNormalization" ||
        op->opType == "SimplifiedLayerNormalization" ||
        (op->opType == "DequantizeLinear" && op->opset >= 10 &&
         op->inputs.size() >= 2 && op->inputs.size() <= 3))) {
      if (op->opType == "DynamicQuantizeLinear" && op->opset < 11) return std::nullopt;
      if (op->outputs.empty() || op->outputs[0].empty() || scoped(op->outputs[0]) != name)
        return std::nullopt;
      return tokenAxes(first, axes, vocabulary, depth + 1);
    }
    if (op->domain == "com.microsoft" && op->opType == "RotaryEmbedding" &&
        op->opset == 1 && op->inputs.size() == 4 && axes[0] < 2 && axes[1] < 2)
      return tokenAxes(first, axes, vocabulary, depth + 1);
    if (op->domain == "com.microsoft" && op->opType == "SkipSimplifiedLayerNormalization" &&
        op->opset == 1 && op->inputs.size() >= 3 &&
        ((!op->outputs.empty() && scoped(op->outputs[0]) == name) ||
         (op->outputs.size() >= 4 && !op->outputs[3].empty() &&
          scoped(op->outputs[3]) == name)))
      return tokenAxes(first, axes, vocabulary, depth + 1);
    if (normal && (op->opType == "Mul" || op->opType == "Add" || op->opType == "Sub" ||
                   op->opType == "Div") && op->opset >= 7 && op->inputs.size() == 2) {
      const std::array<std::optional<TokenAxes>, 2> candidates{
        tokenAxes(scoped(op->inputs[0]), axes, vocabulary, depth + 1),
        tokenAxes(scoped(op->inputs[1]), axes, vocabulary, depth + 1)};
      std::optional<TokenAxes> result;
      for (std::size_t port = 0; port < 2; ++port) {
        const auto& candidate = candidates[port];
        if (!candidate) continue;
        const auto& sibling = candidates[1 - port];
        const auto otherName = scoped(op->inputs[1 - port]);
        const auto* other = fact(otherName);
        bool preserves = sibling && sibling->input == candidate->input &&
          sibling->axes == candidate->axes && sibling->factors == candidate->factors;
        if (!preserves && other && other->dimensions && other->dimensions->size() <= 3) {
          preserves = true;
          const auto leading = 3 - other->dimensions->size();
          for (const auto axis : axes) {
            if (axis < leading) continue;
            const auto& dim = other->dimensions->at(axis - leading);
            if (!(dim.kind == DimensionConstraint::Kind::FIXED && dim.fixed == 1) &&
                !sameDimension(scoped(op->inputs[port]), axis, otherName, axis - leading))
              preserves = false;
          }
        }
        if (!preserves) continue;
        if (result && (result->input != candidate->input || result->axes != candidate->axes ||
                       result->factors != candidate->factors))
          rejectState("GENERATION_CONTRACT_AMBIGUOUS");
        if (!result) result = candidate;
        else for (std::size_t logical = 0; logical < 2; ++logical)
          mergeFixed(result->fixedAxes[logical], candidate->fixedAxes[logical]);
      }
      return result;
    }
    return std::nullopt;
  }

  bool
  sameDimension(const std::string& left, std::uint64_t leftAxis,
                 const std::string& right, std::uint64_t rightAxis) const
  {
    if (left == right && leftAxis == rightAxis) return true;
    const auto* a = fact(left);
    const auto* b = fact(right);
    if (!a || !b || !a->dimensions || !b->dimensions || leftAxis >= a->dimensions->size() ||
        rightAxis >= b->dimensions->size()) return false;
    const auto& x = a->dimensions->at(leftAxis);
    const auto& y = b->dimensions->at(rightAxis);
    return (x.kind == DimensionConstraint::Kind::FIXED &&
            y.kind == DimensionConstraint::Kind::FIXED && x.fixed == y.fixed) ||
      (x.kind == DimensionConstraint::Kind::SYMBOL &&
       y.kind == DimensionConstraint::Kind::SYMBOL && x.symbol == y.symbol);
  }

  std::set<const AutomaticOnnxOperatorFact*>
  ancestors(const std::string& output)
  {
    std::set<const AutomaticOnnxOperatorFact*> operators;
    std::vector<std::string> pending{output};
    std::set<std::string> visited;
    for (std::size_t i = 0; i < pending.size(); ++i) {
      const auto name = pending[i];
      charge(96 + name.size());
      if (!visited.insert(name).second) continue;
      const auto* op = producer(name);
      if (!op || !operators.insert(op).second) continue;
      for (const auto& input : op->inputs) {
        if (input.empty()) continue;
        charge(32 + input.size());
        pending.push_back(scoped(input));
      }
    }
    return operators;
  }

  std::optional<TokenAxes>
  positionInput(const std::string& value, const TokenAxes& tokens, std::uint64_t& limit)
  {
    if (limit == 0) return std::nullopt;
    const auto direct = integerInputAxes(value, {0, 1}, limit - 1);
    if (direct) return direct;
    const auto* reshape = producer(value);
    if (!reshape || !standard(*reshape) || reshape->opType != "Reshape" ||
        reshape->inputs.size() != 2 || attribute(*reshape, "allowzero", 0) != 0)
      return std::nullopt;
    const auto target = shapeProgram(scoped(reshape->inputs[1]));
    if (!target || target->scalar || target->elementType != 7 || target->values.size() != 2 ||
        target->values[0].literal != std::optional<std::int64_t>(-1) ||
        target->values[1].literal || target->values[1].axis.tensor != tokens.input ||
        target->values[1].axis.axis != tokens.axes[1]) return std::nullopt;
    limit = std::min(limit, target->castLimit);
    return integerInputAxes(scoped(reshape->inputs[0]), {0, 1}, limit - 1);
  }

  void
  rotaryInputs(const std::string& activation, TokenAxes& tokens,
                AutomaticGenerationContract& generation)
  {
    std::string name = activation;
    for (unsigned depth = 0; depth < 64; ++depth) {
      charge(128 + name.size());
      const auto* op = producer(name);
      if (!op || op->atomicBody || !op->functionScope.empty() || !op->overload.empty()) return;
      if (standard(*op) && (op->opType == "Identity" || op->opType == "Cast" ||
                            op->opType == "Transpose") && op->inputs.size() == 1) {
        name = scoped(op->inputs[0]);
        continue;
      }
      if (op->domain != "com.microsoft" || op->opType != "RotaryEmbedding" ||
          op->opset != 1) return;
      if (op->inputs.size() != 4 || !op->attributeReferences.empty() ||
          attribute(*op, "is_packed_batching", 0) != 0)
        rejectState("AUTOMATIC_GENERATION_ROTARY_UNPROVEN");
      for (const auto port : {2U, 3U}) {
        const auto* table = fact(scoped(op->inputs[port]));
        if (!table || !constantHeader(*table) || !table->dimensions ||
            table->dimensions->size() != 2)
          rejectState("AUTOMATIC_GENERATION_POSITION_CAPACITY");
        const auto capacity = fixedExtent(*table, 0);
        if (capacity == 0) rejectState("AUTOMATIC_GENERATION_POSITION_CAPACITY");
        generation.contextLimit = std::min(generation.contextLimit, capacity);
      }
      const auto local = tokenAxes(scoped(op->inputs[0]), {0, 1}, generation.vocabularySize);
      if (!local || local->input != tokens.input || !unitFactors(*local))
        rejectState("AUTOMATIC_GENERATION_POSITION_AXES_UNPROVEN");
      const auto positions = positionInput(scoped(op->inputs[1]), *local,
                                            generation.contextLimit);
      if (!positions) rejectState("AUTOMATIC_GENERATION_POSITION_UNPROVEN");
      std::array<std::uint64_t, 2> positionAxes{};
      std::array<std::optional<std::int64_t>, 2> fixedPositions{};
      for (std::size_t logical = 0; logical < 2; ++logical) {
        const auto found = std::find(local->axes.begin(), local->axes.end(), tokens.axes[logical]);
        if (found == local->axes.end())
          rejectState("AUTOMATIC_GENERATION_POSITION_AXES_UNPROVEN");
        const auto physical = static_cast<std::size_t>(found - local->axes.begin());
        positionAxes[logical] = positions->axes[physical];
        fixedPositions[logical] = positions->fixedAxes[physical];
      }
      if (fixedPositions[1] && *fixedPositions[1] != 1)
        rejectState("AUTOMATIC_GENERATION_AUXILIARY_SHAPE");
      mergeFixed(tokens.fixedAxes[0], fixedPositions[0]);
      if (!generation.positionInput.empty() &&
          (generation.positionInput != positions->input ||
           generation.positionBatchAxis != positionAxes[0] ||
           generation.positionSequenceAxis != positionAxes[1]))
        rejectState("GENERATION_CONTRACT_AMBIGUOUS");
      generation.positionInput = positions->input;
      generation.positionBatchAxis = positionAxes[0];
      generation.positionSequenceAxis = positionAxes[1];
      return;
    }
    rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
  }

  std::optional<AutomaticGenerationContract>
  generationCandidate(const std::vector<AutomaticAttentionState>& states,
                        const std::string& output, std::array<std::uint64_t, 2> scoreAxes,
                        std::uint64_t vocabulary, const TokenAxes& tokenSource)
  {
    auto tokens = tokenSource;
    const auto* token = fact(tokens.input);
    if (!token || !token->dimensions) return std::nullopt;
    const auto& sequence = token->dimensions->at(tokens.axes[1]);
    const auto& batch = token->dimensions->at(tokens.axes[0]);
    if ((tokens.fixedAxes[1] && *tokens.fixedAxes[1] != 1) ||
        (tokens.fixedAxes[0] && *tokens.fixedAxes[0] == 0)) return std::nullopt;
    const auto* scores = fact(output);
    for (std::size_t logical = 0; logical < 2; ++logical) {
      const auto& dimension = scores->dimensions->at(scoreAxes[logical]);
      if (dimension.kind == DimensionConstraint::Kind::FIXED)
        mergeFixed(tokens.fixedAxes[logical], dimension.fixed);
    }
    compatibleBatch(scores->dimensions->at(scoreAxes[0]), batch);
    compatibleBatch(scores->dimensions->at(scoreAxes[1]), sequence);
    const auto& scoreSequence = scores->dimensions->at(scoreAxes[1]);
    if (scoreSequence.kind == DimensionConstraint::Kind::FIXED && scoreSequence.fixed != 1)
      return std::nullopt;
    AutomaticGenerationContract result;
    result.tokenInput = tokens.input;
    result.tokenBatchAxis = tokens.axes[0]; result.tokenSequenceAxis = tokens.axes[1];
    result.logitsOutput = output;
    result.logitsBatchAxis = scoreAxes[0]; result.logitsSequenceAxis = scoreAxes[1];
    result.vocabularySize = vocabulary; result.states = states;
    result.contextLimit = std::numeric_limits<std::int32_t>::max();
    result.ruleId = "causal-gqa";
    const auto upstream = ancestors(output);
    std::set<std::uint64_t> checked;
    for (const auto& state : states) {
      const auto& past = boundaryTensor(state.input);
      const auto& present = boundaryTensor(state.output);
      const auto& growth = past.dimensions->at(state.inputAxes[2]);
      if (growth.kind == DimensionConstraint::Kind::FIXED ||
          (growth.upperBound && *growth.upperBound == 0)) return std::nullopt;
      compatibleBatch(past.dimensions->at(state.inputAxes[0]), batch);
      compatibleBatch(present.dimensions->at(state.outputAxes[0]), batch);
      for (const auto* dimension : {&past.dimensions->at(state.inputAxes[0]),
                                    &present.dimensions->at(state.outputAxes[0])})
        if (dimension->kind == DimensionConstraint::Kind::FIXED)
          mergeFixed(tokens.fixedAxes[0], dimension->fixed);
      if (growth.upperBound) result.contextLimit =
        std::min(result.contextLimit, *growth.upperBound + 1);
      const auto& produced = present.dimensions->at(state.outputAxes[2]);
      if (produced.upperBound) result.contextLimit =
        std::min(result.contextLimit, *produced.upperBound);
      if (result.contextLimit == 0) return std::nullopt;
      if (!checked.insert(state.operatorIndex).second) continue;
      const auto& op = m_contract.operators.at(state.operatorIndex);
      if (!upstream.count(&op) || attribute(op, "do_rotary", 0) != 0) return std::nullopt;
      const auto values = tokenAxes(state.value, {0, 1}, vocabulary);
      if (!values || values->input != tokens.input || values->axes != tokens.axes ||
          !unitFactors(*values))
        return std::nullopt;
      mergeFixed(tokens.fixedAxes[0], values->fixedAxes[0]);
      if (values->fixedAxes[1] && *values->fixedAxes[1] != 1) return std::nullopt;
      for (const auto* activation : {&state.query, &state.key, &state.value}) {
        const auto traced = tokenAxes(*activation, {0, 1}, vocabulary);
        if (!traced || traced->input != tokens.input || traced->axes != tokens.axes ||
            !unitFactors(*traced)) return std::nullopt;
        mergeFixed(tokens.fixedAxes[0], traced->fixedAxes[0]);
        if (traced->fixedAxes[1] && *traced->fixedAxes[1] != 1) return std::nullopt;
        const auto width = featureWidth(*activation);
        const auto heads = activation == &state.query ? positiveAttribute(op, "num_heads") :
                                                       state.heads;
        if (!width || *width != heads * state.headSize) return std::nullopt;
        const auto* shape = fact(*activation);
        if (shape && shape->dimensions && shape->dimensions->size() == 3) {
          compatibleBatch(shape->dimensions->at(0), batch);
          compatibleBatch(shape->dimensions->at(1), sequence);
        }
      }
      const auto lengths = maskProgram(state.sequenceLengths);
      const auto total = shapeProgram(state.totalLength);
      if (!lengths || lengths->offset != -1 || lengths->elementType != 6 || !total ||
          total->elementType != 6 ||
          total->values.size() != 1 || total->values[0].literal ||
          total->values[0].axis.tensor != lengths->mask ||
          total->values[0].axis.axis != lengths->contextAxis) return std::nullopt;
      if (!result.maskInput.empty() && (result.maskInput != lengths->mask ||
          result.maskBatchAxis != lengths->batchAxis ||
          result.maskContextAxis != lengths->contextAxis)) return std::nullopt;
      mergeFixed(tokens.fixedAxes[0], lengths->fixedBatch);
      const auto* sequencePort = fact(state.sequenceLengths);
      const auto* totalPort = fact(state.totalLength);
      if ((sequencePort && sequencePort->onnxElementType != 0 &&
           sequencePort->onnxElementType != 6) ||
          (totalPort && totalPort->onnxElementType != 0 && totalPort->onnxElementType != 6))
        return std::nullopt;
      result.maskInput = lengths->mask; result.maskBatchAxis = lengths->batchAxis;
      result.maskContextAxis = lengths->contextAxis;
      result.contextLimit = std::min({result.contextLimit, lengths->castLimit, total->castLimit});
    }
    for (const auto* op : upstream) {
      if (op->domain != "com.microsoft" || op->opType != "RotaryEmbedding") continue;
      if (op->outputs.size() != 1 || op->outputs[0].empty()) return std::nullopt;
      rotaryInputs(scoped(op->outputs[0]), tokens, result);
    }
    std::set<std::string> covered{tokens.input, result.maskInput};
    if (!result.positionInput.empty()) covered.insert(result.positionInput);
    if (result.maskInput == tokens.input || result.positionInput == tokens.input ||
        (!result.positionInput.empty() && result.positionInput == result.maskInput))
      rejectState("AUTOMATIC_GENERATION_ROLE_ALIAS");
    for (const auto& state : states) covered.insert(state.input);
    if (covered != m_inputs || result.contextLimit == 0) return std::nullopt;
    std::set<std::string> outputs{output};
    for (const auto& state : states) outputs.insert(state.output);
    if (outputs != m_outputs) return std::nullopt;
    return result;
  }

  static std::string
  scoped(const std::string& value)
  {
    return "graph/" + value;
  }

  void
  active() const
  {
    if (!m_control.requireActive || m_limit == 0)
      throw std::invalid_argument("AUTOMATIC_STATE_CONTROL");
    m_control.requireActive();
    if (std::chrono::steady_clock::now() >= m_control.deadline)
      throw std::runtime_error("AUTOMATIC_STATE_DEADLINE");
  }

  void
  charge(std::uint64_t bytes)
  {
    active();
    if (bytes > m_limit - m_charged) throw std::invalid_argument("AUTOMATIC_STATE_LIMIT");
    m_charged += bytes;
  }

  void
  work()
  {
    active();
    if (m_work >= m_limit / 16)
      throw std::invalid_argument("AUTOMATIC_GENERATION_WORK_LIMIT");
    ++m_work;
  }

  static std::uint64_t
  positiveAttribute(const AutomaticOnnxOperatorFact& op, const char* key)
  {
    const auto found = op.integerAttributes.find(key);
    if (found == op.integerAttributes.end() || found->second <= 0)
      rejectState("AUTOMATIC_STATE_HEADS");
    return static_cast<std::uint64_t>(found->second);
  }

  const AutomaticTensorContract&
  boundaryTensor(const std::string& name) const
  {
    const auto found = m_tensors.find(name);
    if (found == m_tensors.end() || !found->second->dimensions ||
        found->second->dimensions->size() != 4)
      rejectState("AUTOMATIC_STATE_BOUNDARY_RANK");
    return *found->second;
  }

  static std::uint64_t
  fixedExtent(const AutomaticTensorContract& tensor, std::uint64_t axis)
  {
    const auto& dim = tensor.dimensions->at(axis);
    if (dim.kind != DimensionConstraint::Kind::FIXED || dim.fixed < 0)
      rejectState("AUTOMATIC_STATE_STATIC_AXIS");
    return static_cast<std::uint64_t>(dim.fixed);
  }

  static void
  compatibleBatch(const DimensionConstraint& a, const DimensionConstraint& b)
  {
    if (a.kind == DimensionConstraint::Kind::FIXED &&
        b.kind == DimensionConstraint::Kind::FIXED && a.fixed != b.fixed)
      rejectState("AUTOMATIC_STATE_BATCH");
  }

  void
  projection(const std::string& name, std::int32_t type, std::uint64_t width) const
  {
    const auto found = m_tensors.find(name);
    if (found == m_tensors.end()) return;
    const auto& tensor = *found->second;
    if (tensor.onnxElementType != 0 && tensor.onnxElementType != type)
      rejectState("AUTOMATIC_STATE_PROJECTION_TYPE");
    if (!tensor.dimensions) return;
    if (tensor.dimensions->size() != 3) rejectState("AUTOMATIC_STATE_PROJECTION_RANK");
    const auto& last = tensor.dimensions->back();
    if (last.kind == DimensionConstraint::Kind::FIXED &&
        (last.fixed < 0 || static_cast<std::uint64_t>(last.fixed) != width))
      rejectState("AUTOMATIC_STATE_PROJECTION_WIDTH");
  }

  std::optional<Axes>
  transparent(const AutomaticOnnxOperatorFact& op) const
  {
    if ((!op.domain.empty() && op.domain != "ai.onnx") || op.opset > 21 || op.atomicBody ||
        !op.functionScope.empty() || !op.overload.empty() || !op.attributeReferences.empty() ||
        op.inputs.size() != 1 || op.outputs.size() != 1 || op.inputs[0].empty() ||
        op.outputs[0].empty()) return std::nullopt;
    if (op.opType == "Identity" && op.opset >= 1) return IDENTITY_AXES;
    if (op.opType == "Cast" && op.opset >= 6) {
      // Cast is traversed only after a later path-wide same-type check.
      if (!op.integerAttributes.count("to")) return std::nullopt;
      return IDENTITY_AXES;
    }
    if (op.opType != "Transpose" || op.opset < 1) return std::nullopt;
    Axes permutation{3, 2, 1, 0};
    const auto found = op.integerListAttributes.find("perm");
    if (found == op.integerListAttributes.end()) return permutation;
    if (found->second.size() != 4) rejectState("AUTOMATIC_STATE_PERMUTATION");
    std::set<std::int64_t> seen;
    for (std::size_t axis = 0; axis < 4; ++axis) {
      const auto from = found->second[axis];
      if (from < 0 || from >= 4 || !seen.insert(from).second)
        rejectState("AUTOMATIC_STATE_PERMUTATION");
      permutation[axis] = static_cast<std::uint64_t>(from);
    }
    return permutation;
  }

  std::pair<std::string, Axes>
  inputBoundary(std::string name)
  {
    Axes axes = IDENTITY_AXES;
    std::set<std::string> visited;
    while (!m_inputs.count(name)) {
      charge(64 + name.size());
      if (!visited.insert(name).second) rejectState("AUTOMATIC_STATE_CYCLE");
      const auto found = m_producers.find(name);
      if (found == m_producers.end()) rejectState("AUTOMATIC_STATE_INPUT_UNPROVEN");
      const auto permutation = transparent(*found->second);
      if (!permutation) rejectState("AUTOMATIC_STATE_INPUT_UNPROVEN");
      for (auto& axis : axes) axis = permutation->at(axis);
      name = scoped(found->second->inputs[0]);
    }
    return {std::move(name), axes};
  }

  std::pair<std::string, Axes>
  outputBoundary(const std::string& start)
  {
    std::vector<std::pair<std::string, Axes>> pending{{start, IDENTITY_AXES}};
    std::optional<std::pair<std::string, Axes>> result;
    std::set<std::string> visited;
    for (std::size_t i = 0; i < pending.size(); ++i) {
      const auto current = pending[i];
      charge(128 + current.first.size());
      if (!visited.insert(current.first).second) rejectState("AUTOMATIC_STATE_CYCLE");
      if (m_outputs.count(current.first)) {
        if (result) rejectState("GENERATION_CONTRACT_AMBIGUOUS");
        result = current;
      }
      const auto consumers = m_consumers.find(current.first);
      if (consumers == m_consumers.end()) continue;
      for (const auto* consumer : consumers->second) {
        active();
        const auto permutation = transparent(*consumer);
        if (!permutation) continue;
        Axes inverse{};
        for (std::size_t j = 0; j < 4; ++j) inverse[permutation->at(j)] = j;
        auto axes = current.second;
        for (auto& axis : axes) axis = inverse[axis];
        pending.emplace_back(scoped(consumer->outputs[0]), axes);
      }
    }
    if (!result) rejectState("AUTOMATIC_STATE_OUTPUT_UNPROVEN");
    return *result;
  }

  void
  checkValueType(const std::string& name, std::int32_t type) const
  {
    const auto found = m_tensors.find(name);
    if (found != m_tensors.end() &&
        ((found->second->onnxElementType != 0 && found->second->onnxElementType != type) ||
         (found->second->dimensions && found->second->dimensions->size() != 4)))
      rejectState("AUTOMATIC_STATE_PATH_TYPE");
  }

  void
  validatePathTypes(const std::string& start, const std::string& stop,
                         std::int32_t type, const AutomaticTensorContract& boundary,
                         const Axes& boundaryAxes, Axes axes)
  {
    std::string name = start;
    std::set<std::string> visited;
    std::array<std::optional<std::int64_t>, 4> fixed;
    for (std::size_t formal = 0; formal < 4; ++formal) {
      const auto& dim = boundary.dimensions->at(boundaryAxes[formal]);
      if (dim.kind == DimensionConstraint::Kind::FIXED) fixed[formal] = dim.fixed;
    }
    while (true) {
      charge(64 + name.size());
      if (!visited.insert(name).second) rejectState("AUTOMATIC_STATE_CYCLE");
      checkValueType(name, type);
      const auto annotation = m_tensors.find(name);
      if (annotation != m_tensors.end() && annotation->second->dimensions) {
        for (std::size_t formal = 0; formal < 4; ++formal) {
          const auto& actual = annotation->second->dimensions->at(axes[formal]);
          if (actual.kind == DimensionConstraint::Kind::FIXED) {
            if (fixed[formal] && *fixed[formal] != actual.fixed)
              rejectState("AUTOMATIC_STATE_PATH_DIMENSION");
            fixed[formal] = actual.fixed;
          }
        }
      }
      if (name == stop) return;
      const auto found = m_producers.find(name);
      if (found == m_producers.end() || !transparent(*found->second))
        rejectState("AUTOMATIC_STATE_PATH_UNPROVEN");
      if (found->second->opType == "Cast" &&
          found->second->integerAttributes.at("to") != type)
        rejectState("AUTOMATIC_STATE_LOSSY_CAST");
      const auto permutation = *transparent(*found->second);
      for (auto& axis : axes) axis = permutation.at(axis);
      name = scoped(found->second->inputs[0]);
    }
  }

private:
  const AutomaticOnnxContract& m_contract;
  const NativeAssemblyControl& m_control;
  const std::uint64_t m_limit;
  std::uint64_t m_charged = 0;
  std::uint64_t m_work = 0;
  std::map<std::string, const AutomaticTensorContract*> m_tensors;
  std::map<std::string, const AutomaticOnnxOperatorFact*> m_producers;
  std::map<std::string, std::vector<const AutomaticOnnxOperatorFact*>> m_consumers;
  std::set<std::string> m_inputs;
  std::set<std::string> m_outputs;
  using TokenKey = std::tuple<std::string, std::array<std::uint64_t, 2>, std::uint64_t>;
  std::map<TokenKey, std::optional<TokenAxes>> m_tokenMemo;
  std::set<TokenKey> m_pendingTokens;
  std::map<std::string, std::optional<std::uint64_t>> m_widthMemo;
  std::set<std::string> m_pendingWidths;
  std::map<std::string, std::optional<IntegerShapeProgram>> m_shapeMemo;
  std::set<std::string> m_pendingShapes;
  std::map<std::string, std::int32_t> m_typeMemo;
  std::set<std::string> m_pendingTypes;
};

} // namespace

std::vector<AutomaticAttentionState>
deriveAutomaticOnnxAttentionStates(const AutomaticOnnxContract& contract,
                                   const NativeAssemblyControl& control)
{
  contract.validate(control);
  return AttentionBoundaryProof(contract, control).derive();
}

std::optional<AutomaticGenerationContract>
deriveAutomaticOnnxGeneration(const AutomaticOnnxContract& contract,
                               const NativeAssemblyControl& control)
{
  contract.validateSourceFacts(control);
  return AttentionBoundaryProof(contract, control).generation();
}

NativeJson
automaticOnnxGenerationJson(const std::optional<AutomaticGenerationContract>& generation)
{
  if (!generation) return nullptr;
  const auto& value = *generation;
  auto states = NativeJson::array();
  for (const auto& state : value.states) {
    const char* role = nullptr;
    switch (state.role) {
      case AutomaticAttentionState::Role::KEY: role = "key"; break;
      case AutomaticAttentionState::Role::VALUE: role = "value"; break;
      default: rejectState("AUTOMATIC_GENERATION_STATE_ROLE");
    }
    states.push_back(NativeJson{{"role", role}, {"operatorIndex", state.operatorIndex},
      {"input", state.input}, {"output", state.output}, {"query", state.query},
      {"key", state.key}, {"value", state.value}, {"sequenceLengths", state.sequenceLengths},
      {"totalLength", state.totalLength}, {"elementType", tensorElementTypeName(state.elementType)},
      {"inputAxes", state.inputAxes}, {"outputAxes", state.outputAxes},
      {"heads", state.heads}, {"headSize", state.headSize},
      {"initial", "empty"}, {"update", "replace_full"}});
  }
  return NativeJson{{"schema", "ndnsf-di-automatic-generation-v1"},
    {"tokenInput", value.tokenInput}, {"tokenBatchAxis", value.tokenBatchAxis},
    {"tokenSequenceAxis", value.tokenSequenceAxis}, {"logitsOutput", value.logitsOutput},
    {"logitsBatchAxis", value.logitsBatchAxis},
    {"logitsSequenceAxis", value.logitsSequenceAxis},
    {"logitsVocabularyAxis", value.logitsVocabularyAxis}, {"vocabularySize", value.vocabularySize},
    {"states", std::move(states)}, {"maskInput", value.maskInput},
    {"maskBatchAxis", value.maskBatchAxis}, {"maskContextAxis", value.maskContextAxis},
    {"maskPolicy", "all_ones"}, {"positionInput", value.positionInput},
    {"positionBatchAxis", value.positionBatchAxis},
    {"positionSequenceAxis", value.positionSequenceAxis}, {"positionPolicy", "contiguous"},
    {"contextLimit", value.contextLimit}, {"ruleId", value.ruleId},
    {"ruleVersion", value.ruleVersion}};
}

} // namespace ndnsf::di
