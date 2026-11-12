#include "automatic-onnx-generation.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
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

struct HeadView
{
  std::string source;
  Axes axes{};
  std::uint64_t heads = 0;
  std::uint64_t headSize = 0;
};

struct HeadRepeat
{
  std::string source;
  std::uint64_t factor = 0;
};

enum class ExtentKind { UNKNOWN, FIXED, BATCH, SEQUENCE, PAST, TOTAL };

struct TensorHeadGeometry
{
  Axes axes = IDENTITY_AXES;
  std::uint64_t heads = 0;
  std::uint64_t headSize = 0;
  ExtentKind sequenceKind = ExtentKind::UNKNOWN;
};

struct AttentionEvidence
{
  bool primitive = false;
  std::array<std::string, 3> projections;
  std::array<std::uint64_t, 3> widths{};
  std::uint64_t queryHeads = 0;
  std::uint64_t outputWidth = 0;
  std::string mask;
};

struct IntegerRangeView
{
  const AutomaticOnnxOperatorFact* range = nullptr;
  std::uint64_t rank = 1;
  std::uint64_t axis = 0;
  std::uint64_t castLimit = std::numeric_limits<std::int64_t>::max();
};

struct MaskInputView
{
  std::string input;
  std::array<std::uint64_t, 2> axes{};
};

struct ExtentValue
{
  ExtentKind kind = ExtentKind::FIXED;
  std::int64_t fixed = 0;
  std::uint64_t castLimit = std::numeric_limits<std::int64_t>::max();
};

enum class ScalarKind { ZERO, ONE, TWO, POSITIVE, NEGATIVE_INFINITY };

struct PrimitiveRotaryEvidence
{
  std::string source;
  std::string positions;
};

struct CacheEvidence
{
  AutomaticAttentionState state;
  HeadView current;
  std::string currentValue;
  std::uint64_t expandedHeads = 0;
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
        if (role == 0) {
          AttentionEvidence evidence;
          evidence.projections = {state.query, state.key, state.value};
          evidence.widths = {queryHeads * headSize, heads * headSize, heads * headSize};
          evidence.queryHeads = queryHeads; evidence.outputWidth = queryHeads * headSize;
          charge(sizeof(AttentionEvidence) + 128);
          for (const auto& name : evidence.projections) charge(name.size());
          m_attentions.emplace(index, std::move(evidence));
        }
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
    for (std::size_t index = 0; index < m_contract.operators.size(); ++index) {
      active();
      const auto candidate = primitiveAttention(index);
      if (!candidate) continue;
      for (const auto& state : *candidate) {
        if (!usedInputs.insert(state.input).second || !usedOutputs.insert(state.output).second)
          rejectState("GENERATION_CONTRACT_AMBIGUOUS");
        result.push_back(state);
      }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
      return std::tie(a.operatorIndex, a.role) < std::tie(b.operatorIndex, b.role);
    });
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
    const auto* tensor = fact(name);
    if (result && tensor && tensor->onnxElementType != 0 &&
        tensor->onnxElementType != result->elementType) result.reset();
    if (result) for (const auto& value : result->values)
      charge(sizeof(IntegerShapeValue) + value.axis.tensor.size());
    m_pendingShapes.erase(name);
    m_shapeMemo.emplace(name, result);
    return result;
  }

  bool
  safeShapeValue(const IntegerShapeValue& value, const IntegerShapeProgram& program) const
  {
    const auto limit = std::min(program.castLimit, program.elementType == 6 ?
      std::uint64_t(std::numeric_limits<std::int32_t>::max()) :
      std::uint64_t(std::numeric_limits<std::int64_t>::max()));
    if (value.literal) {
      return *value.literal <= static_cast<std::int64_t>(limit) &&
        *value.literal >= -1 - static_cast<std::int64_t>(limit);
    }
    if (value.axis.tensor.empty()) return false;
    if (limit == std::uint64_t(std::numeric_limits<std::int64_t>::max())) return true;
    const auto* tensor = fact(value.axis.tensor);
    if (!tensor || !tensor->dimensions || value.axis.axis >= tensor->dimensions->size())
      return false;
    const auto& dimension = tensor->dimensions->at(value.axis.axis);
    return dimension.kind == DimensionConstraint::Kind::FIXED
      ? dimension.fixed >= 0 && static_cast<std::uint64_t>(dimension.fixed) <= limit
      : dimension.upperBound && *dimension.upperBound <= limit;
  }

  std::optional<std::int64_t>
  fixedShapeValue(const IntegerShapeValue& value, const IntegerShapeProgram& program)
  {
    if (!safeShapeValue(value, program)) return std::nullopt;
    if (value.literal) return value.literal;
    const auto* tensor = fact(value.axis.tensor);
    if (tensor && tensor->dimensions) {
      if (value.axis.axis >= tensor->dimensions->size()) return std::nullopt;
      const auto& dimension = tensor->dimensions->at(value.axis.axis);
      if (dimension.kind == DimensionConstraint::Kind::FIXED) return dimension.fixed;
      if (tensor->dimensions->size() != 4) return std::nullopt;
    }
    const auto cache = m_headGeometry.find(value.axis.tensor);
    if (cache != m_headGeometry.end()) {
      if (value.axis.axis == cache->second.axes[1]) return static_cast<std::int64_t>(cache->second.heads);
      if (value.axis.axis == cache->second.axes[3]) return static_cast<std::int64_t>(cache->second.headSize);
    }
    const auto view = headView(value.axis.tensor);
    if (view) {
      const auto extent = value.axis.axis == view->axes[1] ? view->heads :
                          value.axis.axis == view->axes[3] ? view->headSize : 0;
      if (extent > 0 && extent <= static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) return static_cast<std::int64_t>(extent);
    }
    return std::nullopt;
  }

  static bool
  integerProgram(const IntegerShapeProgram& program)
  {
    return (program.elementType == 6 || program.elementType == 7) &&
      program.values.size() <= 64 && (!program.scalar || program.values.size() == 1);
  }

  using ProgramShape = std::pair<bool, std::size_t>;

  static ProgramShape
  programShape(const IntegerShapeProgram& program)
  {
    return {program.scalar, program.values.size()};
  }

  static std::optional<ProgramShape>
  broadcastProgramShape(ProgramShape a, ProgramShape b)
  {
    if (a.first) return b;
    if (b.first) return a;
    if (a.second == b.second) return a;
    if (a.second == 1) return b;
    if (b.second == 1) return a;
    return std::nullopt;
  }

  static const IntegerShapeValue&
  broadcastValue(const IntegerShapeProgram& program, std::size_t index)
  {
    return program.values.at(program.scalar || program.values.size() == 1 ? 0 : index);
  }

  std::optional<IntegerShapeValue>
  multiplyShapeValues(const IntegerShapeValue& a, const IntegerShapeProgram& left,
                      const IntegerShapeValue& b, const IntegerShapeProgram& right)
  {
    const auto x = fixedShapeValue(a, left), y = fixedShapeValue(b, right);
    if (x == std::optional<std::int64_t>(1)) return b;
    if (y == std::optional<std::int64_t>(1)) return a;
    if (x == std::optional<std::int64_t>(0) || y == std::optional<std::int64_t>(0))
      return IntegerShapeValue{0, {}};
    if (!x || !y) return std::nullopt;
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if ((*x > 0 && ((*y > 0 && *x > maximum / *y) ||
                    (*y < 0 && *y < minimum / *x))) ||
        (*x < 0 && ((*y > 0 && *x < minimum / *y) ||
                    (*y < 0 && *x < maximum / *y)))) return std::nullopt;
    const auto product = *x * *y;
    if (left.elementType == 6 && (product < std::numeric_limits<std::int32_t>::min() ||
                                 product > std::numeric_limits<std::int32_t>::max()))
      return std::nullopt;
    return IntegerShapeValue{product, {}};
  }

  std::optional<IntegerShapeValue>
  divideShapeValues(const IntegerShapeValue& a, const IntegerShapeProgram& left,
                    const IntegerShapeValue& b, const IntegerShapeProgram& right)
  {
    const auto x = fixedShapeValue(a, left), y = fixedShapeValue(b, right);
    if (!x || !y || *y == 0 ||
        (*x == std::numeric_limits<std::int64_t>::min() && *y == -1)) return std::nullopt;
    const auto quotient = *x / *y;
    if (left.elementType == 6 && (quotient < std::numeric_limits<std::int32_t>::min() ||
                                 quotient > std::numeric_limits<std::int32_t>::max()))
      return std::nullopt;
    return IntegerShapeValue{quotient, {}};
  }

  std::optional<bool>
  equalShapeValues(const IntegerShapeValue& a, const IntegerShapeProgram& left,
                   const IntegerShapeValue& b, const IntegerShapeProgram& right)
  {
    if (!safeShapeValue(a, left) || !safeShapeValue(b, right)) return std::nullopt;
    if (!a.literal && !b.literal && a.axis.tensor == b.axis.tensor &&
        a.axis.axis == b.axis.axis) return true;
    const auto x = fixedShapeValue(a, left), y = fixedShapeValue(b, right);
    if (x && y) return *x == *y;
    if ((!a.literal && y && *y < 0) || (!b.literal && x && *x < 0)) return false;
    return std::nullopt;
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
      auto count = rank(input);
      if (!count && (m_headGeometry.count(input) || headView(input))) count = 4;
      if (!count && tokenAxes(input, {0, 1}, 0)) count = 3;
      if (!count && integerInputAxes(input, {0, 1}, 0)) count = 2;
      if (!count) {
        if (const auto range = rangeView(input)) count = range->rank;
      }
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
    if (op->opType == "ConstantOfShape" && op->opset >= 9 && op->inputs.size() == 1) {
      const auto shape = shapeProgram(scoped(op->inputs[0]), depth + 1);
      if (!shape || !integerProgram(*shape) || shape->elementType != 7 || shape->scalar ||
          shape->values.size() > 1) return std::nullopt;
      IntegerShapeProgram result;
      result.castLimit = shape->castLimit;
      result.scalar = shape->values.empty();
      std::int64_t count = 1;
      if (!result.scalar) {
        const auto extent = fixedShapeValue(shape->values[0], *shape);
        if (!extent || *extent < 0 || *extent > 64) return std::nullopt;
        count = *extent;
      }
      const auto i32 = op->integerListAttributes.find("tensor.value.int32");
      const auto i64 = op->integerListAttributes.find("tensor.value.int64");
      if ((i32 == op->integerListAttributes.end()) ==
          (i64 == op->integerListAttributes.end())) return std::nullopt;
      const auto& fill = i32 != op->integerListAttributes.end() ? i32->second : i64->second;
      if (fill.size() != 1) return std::nullopt;
      result.elementType = i32 != op->integerListAttributes.end() ? 6 : 7;
      if (result.elementType == 6 && (fill[0] < std::numeric_limits<std::int32_t>::min() ||
                                    fill[0] > std::numeric_limits<std::int32_t>::max()))
        return std::nullopt;
      for (std::int64_t i = 0; i < count; ++i) {
        charge(sizeof(IntegerShapeValue));
        result.values.push_back({fill[0], {}});
      }
      return result;
    }
    if (op->opType == "Reshape" && op->opset >= 5 && op->inputs.size() == 2) {
      auto data = shapeProgram(scoped(op->inputs[0]), depth + 1);
      const auto shape = shapeProgram(scoped(op->inputs[1]), depth + 1);
      if (!data || !shape || !integerProgram(*data) || !integerProgram(*shape) ||
          shape->elementType != 7 || shape->scalar || shape->values.size() > 1 ||
          (op->opset < 14 && op->integerAttributes.count("allowzero"))) return std::nullopt;
      const auto allowZero = attribute(*op, "allowzero", 0);
      if (allowZero != 0 && allowZero != 1) return std::nullopt;
      if (shape->values.empty()) {
        if (data->values.size() != 1) return std::nullopt;
        data->scalar = true;
      }
      else {
        const auto extent = fixedShapeValue(shape->values[0], *shape);
        if (!extent || *extent < -1 || *extent > 64) return std::nullopt;
        auto count = *extent;
        if (count == -1) count = static_cast<std::int64_t>(data->values.size());
        else if (count == 0 && allowZero == 0) {
          if (data->scalar) return std::nullopt;
          count = static_cast<std::int64_t>(data->values.size());
        }
        if (static_cast<std::uint64_t>(count) != data->values.size()) return std::nullopt;
        data->scalar = false;
      }
      data->castLimit = std::min(data->castLimit, shape->castLimit);
      return data;
    }
    if ((op->opType == "Mul" || op->opType == "Div") && op->opset >= 7 &&
        op->inputs.size() == 2) {
      const auto left = shapeProgram(scoped(op->inputs[0]), depth + 1);
      const auto right = shapeProgram(scoped(op->inputs[1]), depth + 1);
      if (!left || !right || !integerProgram(*left) || !integerProgram(*right) ||
          left->elementType != right->elementType) return std::nullopt;
      const auto shape = broadcastProgramShape(programShape(*left), programShape(*right));
      if (!shape) return std::nullopt;
      IntegerShapeProgram result;
      result.scalar = shape->first; result.elementType = left->elementType;
      result.castLimit = std::min(left->castLimit, right->castLimit);
      for (std::size_t i = 0; i < shape->second; ++i) {
        const auto value = op->opType == "Mul" ?
          multiplyShapeValues(broadcastValue(*left, i), *left, broadcastValue(*right, i), *right) :
          divideShapeValues(broadcastValue(*left, i), *left, broadcastValue(*right, i), *right);
        if (!value) return std::nullopt;
        charge(sizeof(IntegerShapeValue) + value->axis.tensor.size());
        result.values.push_back(*value);
      }
      return result;
    }
    if (op->opType == "Where" && op->opset >= 9 && op->inputs.size() == 3) {
      work();
      const auto conditionName = scoped(op->inputs[0]);
      const auto* equal = producer(conditionName);
      if (!equal || !standard(*equal) || equal->opType != "Equal" || equal->opset < 7 ||
          equal->inputs.size() != 2 || equal->outputs.size() != 1) return std::nullopt;
      const auto* condition = fact(conditionName);
      if (condition && condition->onnxElementType != 0 && condition->onnxElementType != 9)
        return std::nullopt;
      const auto left = shapeProgram(scoped(equal->inputs[0]), depth + 2);
      const auto right = shapeProgram(scoped(equal->inputs[1]), depth + 2);
      const auto yes = shapeProgram(scoped(op->inputs[1]), depth + 1);
      const auto no = shapeProgram(scoped(op->inputs[2]), depth + 1);
      if (!left || !right || !yes || !no || !integerProgram(*left) ||
          !integerProgram(*right) || !integerProgram(*yes) || !integerProgram(*no) ||
          left->elementType != right->elementType || yes->elementType != no->elementType)
        return std::nullopt;
      const auto comparison = broadcastProgramShape(programShape(*left), programShape(*right));
      const auto arms = broadcastProgramShape(programShape(*yes), programShape(*no));
      if (!comparison || !arms) return std::nullopt;
      const auto shape = broadcastProgramShape(*comparison, *arms);
      if (!shape) return std::nullopt;
      IntegerShapeProgram result;
      result.scalar = shape->first; result.elementType = yes->elementType;
      result.castLimit = std::min({left->castLimit, right->castLimit, yes->castLimit, no->castLimit});
      for (std::size_t i = 0; i < shape->second; ++i) {
        const auto ci = comparison->first || comparison->second == 1 ? 0 : i;
        const auto decision = equalShapeValues(broadcastValue(*left, ci), *left,
                                                broadcastValue(*right, ci), *right);
        const auto& a = broadcastValue(*yes, i);
        const auto& b = broadcastValue(*no, i);
        if (!decision && equalShapeValues(a, *yes, b, *no) != std::optional<bool>(true))
          return std::nullopt;
        const auto& value = decision.value_or(true) ? a : b;
        charge(sizeof(IntegerShapeValue) + value.axis.tensor.size());
        result.values.push_back(value);
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
      if (mainOutput && op->outputs.size() == 1) {
        if (op->opType == "Where" && op->opset >= 9 && op->inputs.size() == 3) {
          const auto a = scalarType(scoped(op->inputs[1]), depth + 1);
          const auto b = scalarType(scoped(op->inputs[2]), depth + 1);
          return scalarType(first, depth + 1) == 9 && a != 0 && a == b ? a : 0;
        }
        if ((op->opType == "Equal" && op->opset >= 11) ||
            (op->opType == "LessOrEqual" && op->opset >= 12)) {
          if (op->inputs.size() != 2) return 0;
          const auto a = scalarType(first, depth + 1);
          return a != 0 && a == scalarType(scoped(op->inputs[1]), depth + 1) ? 9 : 0;
        }
        if (op->opType == "And" && op->opset >= 7 && op->inputs.size() == 2)
          return scalarType(first, depth + 1) == 9 &&
            scalarType(scoped(op->inputs[1]), depth + 1) == 9 ? 9 : 0;
        if (op->opType == "IsNaN" && op->opset >= 9 && op->inputs.size() == 1) {
          const auto type = scalarType(first, depth + 1);
          return type == 1 || type == 10 || type == 16 ? 9 : 0;
        }
        if (op->opType == "Softmax" && op->opset >= 13 && op->inputs.size() == 1)
          return scalarType(first, depth + 1);
        if (op->opType == "Range" && op->opset >= 11 && op->inputs.size() == 3) {
          const auto type = scalarType(first, depth + 1);
          return type != 0 && type == scalarType(scoped(op->inputs[1]), depth + 1) &&
            type == scalarType(scoped(op->inputs[2]), depth + 1) ? type : 0;
        }
        if (op->opType == "ConstantOfShape" && op->opset >= 9 && op->inputs.size() == 1) {
          if (scalarType(first, depth + 1) != 7) return 0;
          if (op->integerListAttributes.count("tensor.value.int64")) return 7;
          if (op->integerListAttributes.count("tensor.value.int32")) return 6;
          if (op->integerListAttributes.count("tensor.value.bool")) return 9;
        }
        if ((op->opType == "Flatten" && op->inputs.size() == 1) ||
            (op->opType == "Slice" && op->opset >= 10 && op->inputs.size() >= 3 &&
             op->inputs.size() <= 5)) return scalarType(first, depth + 1);
      }
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
      if (op->opType == "Expand" && op->opset >= 8 && op->inputs.size() == 2)
        return scalarType(first, depth + 1);
      if (op->opType == "Identity" || op->opType == "Transpose" || op->opType == "Reshape" ||
          op->opType == "Squeeze" || op->opType == "Unsqueeze" || op->opType == "Gather" ||
          op->opType == "ReduceSum" || op->opType == "Relu" || op->opType == "Sigmoid" ||
          op->opType == "Tanh" || op->opType == "Sqrt" || op->opType == "Exp" ||
          op->opType == "Log" || op->opType == "Erf" || op->opType == "Abs" ||
          op->opType == "Neg" || op->opType == "Cos" || op->opType == "Sin" ||
          op->opType == "ReduceMean" || op->opType == "Slice")
        return scalarType(first, depth + 1);
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

  const AutomaticOnnxOperatorFact*
  standardProducer(const std::string& name, const char* type, std::size_t inputs,
                   std::int64_t minimumVersion = 1) const
  {
    const auto* op = producer(name);
    return op && standard(*op) && op->opType == type && op->opset >= minimumVersion &&
      op->inputs.size() == inputs && op->outputs.size() == 1 ? op : nullptr;
  }

  bool
  scalarMatches(const std::string& name, ScalarKind kind, std::int32_t type)
  {
    work();
    const auto* tensor = fact(name);
    const auto found = m_contract.floatingLiterals.find(name);
    if (!tensor || !tensor->dimensions || tensor->dimensions->size() > 4 ||
        tensor->onnxElementType != type ||
        found == m_contract.floatingLiterals.end()) return false;
    const auto& bytes = found->second;
    if ((type != 1 && type != 10 && type != 16) || bytes.size() != (type == 1 ? 4U : 2U))
      return false;
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) bits |= std::uint32_t(bytes[i]) << (8 * i);
    const std::uint32_t sign = type == 1 ? 0x80000000U : 0x8000U;
    const std::uint32_t exponent = type == 1 ? 0x7f800000U : type == 10 ? 0x7c00U : 0x7f80U;
    switch (kind) {
      case ScalarKind::ZERO: return (bits & ~sign) == 0;
      case ScalarKind::ONE:
        return bits == (type == 1 ? 0x3f800000U : type == 10 ? 0x3c00U : 0x3f80U);
      case ScalarKind::TWO:
        return bits == (type == 1 ? 0x40000000U : 0x4000U);
      case ScalarKind::POSITIVE:
        return (bits & sign) == 0 && bits != 0 && (bits & exponent) != exponent;
      case ScalarKind::NEGATIVE_INFINITY: return bits == (sign | exponent);
    }
    return false;
  }

  std::string
  dataView(std::string name, bool scales = false)
  {
    for (unsigned depth = 0; depth < 64; ++depth) {
      work();
      const auto* op = producer(name);
      if (!op || !standard(*op) || op->outputs.size() != 1) return name;
      if ((op->opType == "Identity" || (op->opType == "Cast" && op->opset >= 6)) &&
          op->inputs.size() == 1 && scalarType(name) != 0 &&
          scalarType(name) == scalarType(scoped(op->inputs[0]))) {
        name = scoped(op->inputs[0]);
        continue;
      }
      if (scales && op->opType == "Mul" && op->opset >= 7 && op->inputs.size() == 2) {
        bool found = false;
        for (std::size_t port = 0; port < 2; ++port) {
          const auto input = scoped(op->inputs[port]);
          if (!scalarMatches(scoped(op->inputs[1 - port]), ScalarKind::POSITIVE,
                             scalarType(input))) continue;
          name = input; found = true; break;
        }
        if (found) continue;
      }
      return name;
    }
    rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
  }

  void
  addHeadGeometry(const std::string& name, Axes axes, std::uint64_t heads,
                  std::uint64_t headSize, ExtentKind sequenceKind = ExtentKind::UNKNOWN)
  {
    if (heads == 0 || headSize == 0 || heads > std::uint64_t(INT64_MAX) ||
        headSize > std::uint64_t(INT64_MAX)) rejectState("AUTOMATIC_STATE_STATIC_AXIS");
    const auto* tensor = fact(name);
    if (tensor && tensor->dimensions) {
      if (tensor->dimensions->size() != 4) rejectState("AUTOMATIC_STATE_PATH_TYPE");
      for (const auto logical : {1U, 3U}) {
        const auto& dimension = tensor->dimensions->at(axes[logical]);
        if (dimension.kind == DimensionConstraint::Kind::FIXED &&
            (dimension.fixed < 0 || static_cast<std::uint64_t>(dimension.fixed) !=
              (logical == 1 ? heads : headSize)))
          rejectState("AUTOMATIC_GENERATION_HEAD_VIEW_DIMENSION");
      }
    }
    const auto found = m_headGeometry.find(name);
    if (found != m_headGeometry.end()) {
      if (found->second.axes != axes || found->second.heads != heads ||
          found->second.headSize != headSize) rejectState("GENERATION_CONTRACT_AMBIGUOUS");
      if (found->second.sequenceKind != ExtentKind::UNKNOWN &&
          sequenceKind != ExtentKind::UNKNOWN && found->second.sequenceKind != sequenceKind)
        rejectState("GENERATION_CONTRACT_AMBIGUOUS");
      if (sequenceKind != ExtentKind::UNKNOWN) found->second.sequenceKind = sequenceKind;
      return;
    }
    charge(sizeof(TensorHeadGeometry) + 128 + name.size());
    m_headGeometry.emplace(name, TensorHeadGeometry{axes, heads, headSize, sequenceKind});
  }

  std::optional<CacheEvidence>
  cacheConcat(const std::string& value, std::uint64_t index, AutomaticAttentionState::Role role)
  {
    work();
    auto base = dataView(value);
    bool repeated = false;
    if (const auto* reshape = standardProducer(base, "Reshape", 2, 5)) {
      const auto* expand = standardProducer(scoped(reshape->inputs[0]), "Expand", 2, 8);
      const auto* insert = expand ? producer(scoped(expand->inputs[0])) : nullptr;
      if (!insert || !standard(*insert) || insert->opType != "Unsqueeze" ||
          insert->inputs.empty() || insert->outputs.size() != 1) return std::nullopt;
      base = scoped(insert->inputs[0]); repeated = true;
    }
    const auto canonical = dataView(base);
    const auto* concat = standardProducer(canonical, "Concat", 2, 4);
    if (!concat || axisIndex(attribute(*concat, "axis", 0), 4) != 2) return std::nullopt;
    const auto current = headView(scoped(concat->inputs[1]));
    if (!current || current->axes != IDENTITY_AXES) return std::nullopt;
    const auto past = inputBoundary(scoped(concat->inputs[0]));
    const auto full = findOutputBoundary(canonical);
    const auto delta = findOutputBoundary(scoped(concat->inputs[1]));
    if (full && delta) rejectState("GENERATION_CONTRACT_AMBIGUOUS");
    if (!full && !delta) rejectState("AUTOMATIC_STATE_OUTPUT_UNPROVEN");
    const auto present = full ? *full : *delta;
    const auto& input = boundaryTensor(past.first);
    const auto& output = boundaryTensor(present.first);
    if (!input.wireElementType || input.wireElementType != output.wireElementType ||
        (*input.wireElementType != TensorElementType::Float32 &&
         *input.wireElementType != TensorElementType::Float16 &&
         *input.wireElementType != TensorElementType::BFLOAT16)) return std::nullopt;
    const auto type = input.onnxElementType;
    if (scalarType(scoped(concat->inputs[1])) != type || scalarType(canonical) != type)
      return std::nullopt;
    validatePathTypes(scoped(concat->inputs[0]), past.first, type, input, past.second, IDENTITY_AXES);
    validatePathTypes(present.first, full ? canonical : scoped(concat->inputs[1]),
                      type, output, present.second, present.second);
    const auto heads = fixedExtent(input, past.second[1]);
    const auto size = fixedExtent(input, past.second[3]);
    if (heads == 0 || size == 0 || heads != current->heads || size != current->headSize ||
        fixedExtent(output, present.second[1]) != heads ||
        fixedExtent(output, present.second[3]) != size) return std::nullopt;
    const auto& before = input.dimensions->at(past.second[2]);
    if ((before.kind == DimensionConstraint::Kind::FIXED && before.fixed != 0) ||
        (full && output.dimensions->at(present.second[2]).kind == DimensionConstraint::Kind::FIXED))
      return std::nullopt;
    compatibleBatch(input.dimensions->at(past.second[0]),
                    output.dimensions->at(present.second[0]));
    addHeadGeometry(past.first, past.second, heads, size, ExtentKind::PAST);
    addHeadGeometry(scoped(concat->inputs[0]), IDENTITY_AXES, heads, size, ExtentKind::PAST);
    addHeadGeometry(present.first, present.second, heads, size,
                    full ? ExtentKind::TOTAL : ExtentKind::SEQUENCE);
    addHeadGeometry(canonical, IDENTITY_AXES, heads, size, ExtentKind::TOTAL);
    addHeadGeometry(base, IDENTITY_AXES, heads, size, ExtentKind::TOTAL);
    std::uint64_t expandedHeads = heads;
    if (repeated) {
      const auto repeat = headRepeat(dataView(value), 0);
      if (!repeat || repeat->source != base || heads > std::uint64_t(INT64_MAX) / repeat->factor)
        return std::nullopt;
      expandedHeads *= repeat->factor;
    }
    addHeadGeometry(value, IDENTITY_AXES, expandedHeads, size, ExtentKind::TOTAL);
    AutomaticAttentionState state;
    state.role = role; state.operatorIndex = index;
    state.update = full ? AutomaticAttentionState::Update::REPLACE_FULL :
                          AutomaticAttentionState::Update::APPEND_DELTA;
    state.input = past.first; state.output = present.first;
    state.inputAxes = past.second; state.outputAxes = present.second;
    state.heads = heads; state.headSize = size; state.elementType = *input.wireElementType;
    charge(sizeof(CacheEvidence) + state.input.size() + state.output.size() + current->source.size());
    return CacheEvidence{std::move(state), *current, scoped(concat->inputs[1]), expandedHeads};
  }

  void
  attentionMerge(std::uint64_t index, const std::string& output)
  {
    const auto& evidence = m_attentions.at(index);
    struct View { std::string name; Axes axes; };
    std::vector<View> pending{{output, IDENTITY_AXES}};
    std::set<std::string> visited;
    for (std::size_t i = 0; i < pending.size(); ++i) {
      work();
      const auto current = pending[i];
      if (!visited.insert(current.name).second) continue;
      const auto size = evidence.outputWidth / evidence.queryHeads;
      addHeadGeometry(current.name, current.axes, evidence.queryHeads, size, ExtentKind::SEQUENCE);
      const auto found = m_consumers.find(current.name);
      if (found == m_consumers.end()) continue;
      for (const auto* op : found->second) {
        work();
        if (!standard(*op) || op->outputs.size() != 1 || op->inputs.empty() ||
            scoped(op->inputs[0]) != current.name) continue;
        const auto target = scoped(op->outputs[0]);
        if (const auto permutation = transparent(*op)) {
          Axes inverse{};
          for (std::size_t axis = 0; axis < 4; ++axis) inverse[permutation->at(axis)] = axis;
          auto axes = current.axes;
          for (auto& axis : axes) axis = inverse[axis];
          if (scalarType(target) != scalarType(current.name)) continue;
          charge(sizeof(View) + 128 + target.size()); pending.push_back({target, axes});
          continue;
        }
        if (op->opType != "Reshape" || op->opset < 5 || op->inputs.size() != 2 ||
            current.axes != Axes{0, 2, 1, 3} || attribute(*op, "allowzero", 0) != 0 ||
            (op->opset < 14 && op->integerAttributes.count("allowzero"))) continue;
        const auto shape = shapeProgram(scoped(op->inputs[1]));
        if (!shape || !safeHeadShape(*shape) || shape->values.size() != 3) continue;
        const auto last = fixedShapeValue(shape->values[2], *shape);
        if (!last || (*last != -1 && (*last < 0 || std::uint64_t(*last) != evidence.outputWidth)))
          continue;
        bool preserves = true;
        for (const auto axis : {0U, 1U}) {
          if (shape->values[axis].literal == std::optional<std::int64_t>(0) ||
              (axis == 1 && shape->values[axis].literal == std::optional<std::int64_t>(-1))) continue;
          if (!shapeAxisMatches(shape->values[axis], *shape, current.name, axis) &&
              !shapeAxisMatches(shape->values[axis], *shape, evidence.projections[0], axis))
            preserves = false;
        }
        if (!preserves || scalarType(target) != scalarType(current.name)) continue;
        const auto knownRank = rank(target);
        if (knownRank && *knownRank != 3) continue;
        charge(128 + target.size());
        const auto inserted = m_attentionOutputs.emplace(target, index);
        if (!inserted.second && inserted.first->second != index)
          rejectState("GENERATION_CONTRACT_AMBIGUOUS");
      }
    }
  }

  std::optional<std::array<AutomaticAttentionState, 2>>
  primitiveAttention(std::uint64_t index)
  {
    const auto& op = m_contract.operators.at(index);
    if (op.scope != "graph" || !standard(op) || op.opType != "MatMul" ||
        op.inputs.size() != 2 || op.outputs.size() != 1) return std::nullopt;
    auto probabilities = dataView(scoped(op.inputs[0]));
    if (const auto* where = standardProducer(probabilities, "Where", 3, 9)) {
      const auto softmax = scoped(where->inputs[2]);
      const auto* test = standardProducer(scoped(where->inputs[0]), "IsNaN", 1, 9);
      if (!test || scoped(test->inputs[0]) != softmax ||
          !scalarMatches(scoped(where->inputs[1]), ScalarKind::ZERO, scalarType(softmax)))
        return std::nullopt;
      probabilities = softmax;
    }
    const auto* softmax = standardProducer(probabilities, "Softmax", 1, 13);
    if (!softmax || axisIndex(attribute(*softmax, "axis", -1), 4) != 3) return std::nullopt;
    const auto* add = standardProducer(scoped(softmax->inputs[0]), "Add", 2, 7);
    if (!add) return std::nullopt;
    const AutomaticOnnxOperatorFact* dot = nullptr;
    const AutomaticOnnxOperatorFact* mask = nullptr;
    for (std::size_t port = 0; port < 2; ++port) {
      const auto* candidate = standardProducer(dataView(scoped(add->inputs[port]), true), "MatMul", 2);
      const auto* condition = standardProducer(scoped(add->inputs[1 - port]), "Where", 3, 9);
      if (!candidate || !condition ||
          !scalarMatches(scoped(condition->inputs[1]), ScalarKind::ZERO,
                         scalarType(scoped(candidate->outputs[0]))) ||
          !scalarMatches(scoped(condition->inputs[2]), ScalarKind::NEGATIVE_INFINITY,
                         scalarType(scoped(candidate->outputs[0])))) continue;
      if (dot) rejectState("GENERATION_CONTRACT_AMBIGUOUS");
      dot = candidate; mask = condition;
    }
    if (!dot || !mask) return std::nullopt;
    const auto query = dataView(scoped(dot->inputs[0]), true);
    const auto keyTranspose = dataView(scoped(dot->inputs[1]), true);
    const auto* transpose = standardProducer(keyTranspose, "Transpose", 1);
    if (!transpose || transpose->integerListAttributes.count("perm") == 0 ||
        transpose->integerListAttributes.at("perm") != std::vector<std::int64_t>{0, 1, 3, 2})
      return std::nullopt;
    const auto q = headView(query);
    if (!q || q->axes != IDENTITY_AXES) return std::nullopt;
    auto key = cacheConcat(scoped(transpose->inputs[0]), index, AutomaticAttentionState::Role::KEY);
    auto value = cacheConcat(scoped(op.inputs[1]), index, AutomaticAttentionState::Role::VALUE);
    if (!key || !value || key->state.heads != value->state.heads ||
        key->expandedHeads != q->heads || value->expandedHeads != q->heads ||
        key->state.headSize != q->headSize || key->state.elementType != value->state.elementType ||
        scalarType(query) != static_cast<std::int32_t>(fact(key->state.input)->onnxElementType) ||
        scalarType(probabilities) != scalarType(scoped(op.inputs[1]))) return std::nullopt;
    for (auto* state : {&key->state, &value->state}) {
      state->query = query; state->key = key->currentValue; state->value = value->currentValue;
    }
    if (q->heads > std::uint64_t(INT64_MAX) / value->state.headSize ||
        q->heads > std::uint64_t(INT64_MAX) / q->headSize) return std::nullopt;
    AttentionEvidence evidence;
    evidence.primitive = true; evidence.queryHeads = q->heads;
    evidence.projections = {q->source, key->current.source, value->current.source};
    for (std::size_t port = 0; port < 3; ++port) {
      const auto width = featureWidth(evidence.projections[port]);
      if (!width || *width == 0) return std::nullopt;
      evidence.widths[port] = *width;
    }
    evidence.outputWidth = q->heads * value->state.headSize;
    evidence.mask = scoped(mask->inputs[0]);
    charge(sizeof(AttentionEvidence) + evidence.mask.size());
    for (const auto& name : evidence.projections) charge(name.size());
    m_attentions.emplace(index, std::move(evidence));
    attentionMerge(index, scoped(op.outputs[0]));
    return std::array<AutomaticAttentionState, 2>{key->state, value->state};
  }

  std::optional<std::vector<std::int64_t>>
  unsqueezeAxes(const AutomaticOnnxOperatorFact& op)
  {
    if (!standard(op) || op.opType != "Unsqueeze" || op.outputs.size() != 1)
      return std::nullopt;
    if (op.opset >= 13 && op.inputs.size() == 2) {
      const auto program = shapeProgram(scoped(op.inputs[1]));
      if (!program || !safeHeadShape(*program)) return std::nullopt;
      std::vector<std::int64_t> result;
      for (const auto& value : program->values) {
        if (!value.literal) return std::nullopt;
        result.push_back(*value.literal);
      }
      return result;
    }
    if (op.opset <= 12 && op.inputs.size() == 1) {
      const auto found = op.integerListAttributes.find("axes");
      if (found != op.integerListAttributes.end()) return found->second;
    }
    return std::nullopt;
  }

  std::optional<IntegerRangeView>
  rangeView(const std::string& name, unsigned depth = 0)
  {
    work();
    if (depth >= 64) rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
    const auto type = scalarType(name);
    if (type != 6 && type != 7) return std::nullopt;
    const auto* op = producer(name);
    if (!op || !standard(*op) || op->outputs.size() != 1) return std::nullopt;
    if (op->opType == "Range" && op->opset >= 11 && op->inputs.size() == 3) {
      const auto declared = rank(name);
      if (declared && *declared != 1) return std::nullopt;
      return IntegerRangeView{op, 1, 0, type == 6 ? std::uint64_t(INT32_MAX) :
                                                               std::uint64_t(INT64_MAX)};
    }
    if (op->inputs.empty()) return std::nullopt;
    if ((op->opType == "Identity" || (op->opType == "Cast" && op->opset >= 6)) &&
        op->inputs.size() == 1) {
      auto result = rangeView(scoped(op->inputs[0]), depth + 1);
      if (result && type == 6) result->castLimit = std::min(result->castLimit,
                                                          std::uint64_t(INT32_MAX));
      return result;
    }
    const auto inserted = unsqueezeAxes(*op);
    if (!inserted) return std::nullopt;
    auto result = rangeView(scoped(op->inputs[0]), depth + 1);
    if (!result || inserted->size() > 4 - result->rank) return std::nullopt;
    const auto count = result->rank + inserted->size();
    std::set<std::uint64_t> axes;
    for (const auto axis : *inserted)
      if (!axes.insert(axisIndex(axis, count)).second) return std::nullopt;
    std::uint64_t before = 0;
    for (std::uint64_t axis = 0; axis < count; ++axis) {
      if (axes.count(axis)) continue;
      if (before++ == result->axis) { result->axis = axis; break; }
    }
    result->rank = count;
    const auto declared = rank(name);
    if (declared && *declared != count) return std::nullopt;
    return result;
  }

  bool
  batchWithin(const TokenAxes& tokens, std::uint64_t limit) const
  {
    if (limit >= std::uint64_t(INT64_MAX)) return true;
    const auto* tensor = fact(tokens.input);
    if (!tensor || !tensor->dimensions) return false;
    const auto& dimension = tensor->dimensions->at(tokens.axes[0]);
    return dimension.kind == DimensionConstraint::Kind::FIXED ?
      dimension.fixed >= 0 && std::uint64_t(dimension.fixed) <= limit :
      dimension.upperBound && *dimension.upperBound <= limit;
  }

  std::optional<ExtentValue>
  extentAtom(const IntegerShapeValue& value, std::uint64_t castLimit,
             const TokenAxes& tokens, const MaskInputView& mask,
             std::uint64_t vocabulary, unsigned depth)
  {
    work();
    if (depth >= 64) rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
    if (value.literal) return ExtentValue{ExtentKind::FIXED, *value.literal, castLimit};
    const auto axis = canonicalAxis(value.axis);
    ExtentKind kind = ExtentKind::UNKNOWN;
    if (axis.tensor == tokens.input) {
      if (axis.axis == tokens.axes[0]) kind = ExtentKind::BATCH;
      if (axis.axis == tokens.axes[1]) kind = ExtentKind::SEQUENCE;
    }
    if (axis.tensor == mask.input) {
      if (axis.axis == mask.axes[0]) kind = ExtentKind::BATCH;
      if (axis.axis == mask.axes[1]) kind = ExtentKind::TOTAL;
    }
    const auto geometry = m_headGeometry.find(axis.tensor);
    if (geometry != m_headGeometry.end()) {
      if (axis.axis == geometry->second.axes[0]) kind = ExtentKind::BATCH;
      if (axis.axis == geometry->second.axes[2]) kind = geometry->second.sequenceKind;
    }
    if (kind == ExtentKind::UNKNOWN && axis.axis < 2) {
      const auto traced = tokenAxes(axis.tensor, {0, 1}, vocabulary);
      if (traced && traced->input == tokens.input && unitFactors(*traced)) {
        if (traced->axes[axis.axis] == tokens.axes[0]) kind = ExtentKind::BATCH;
        if (traced->axes[axis.axis] == tokens.axes[1]) kind = ExtentKind::SEQUENCE;
      }
    }
    if (kind != ExtentKind::UNKNOWN) {
      if (kind == ExtentKind::BATCH && !batchWithin(tokens, castLimit))
        return std::nullopt;
      return ExtentValue{kind, 0, castLimit};
    }
    if (geometry != m_headGeometry.end()) {
      if (axis.axis == geometry->second.axes[1])
        return ExtentValue{ExtentKind::FIXED, std::int64_t(geometry->second.heads), castLimit};
      if (axis.axis == geometry->second.axes[3])
        return ExtentValue{ExtentKind::FIXED, std::int64_t(geometry->second.headSize), castLimit};
    }
    const auto* axisTensor = fact(axis.tensor);
    if (axisTensor && (constantHeader(*axisTensor) || m_inputs.count(axis.tensor))) {
      const auto fixed = fixedShapeValue(value, IntegerShapeProgram{{value}, true, 7, castLimit});
      if (fixed) return ExtentValue{ExtentKind::FIXED, *fixed, castLimit};
    }
    if (axis.axis == 0) {
      const auto range = rangeView(axis.tensor, depth + 1);
      if (range && range->rank == 1) {
        const auto a = extentValue(scoped(range->range->inputs[0]), tokens, mask, vocabulary,
                                   depth + 1);
        const auto b = extentValue(scoped(range->range->inputs[1]), tokens, mask, vocabulary,
                                   depth + 1);
        const auto step = extentValue(scoped(range->range->inputs[2]), tokens, mask, vocabulary,
                                      depth + 1);
        if (a && b && step && a->kind == ExtentKind::PAST && b->kind == ExtentKind::TOTAL &&
            step->kind == ExtentKind::FIXED && step->fixed == 1)
          return ExtentValue{ExtentKind::SEQUENCE, 0,
            std::min({castLimit, a->castLimit, b->castLimit, range->castLimit})};
      }
    }
    return std::nullopt;
  }

  std::optional<ExtentValue>
  extentValue(const std::string& name, const TokenAxes& tokens,
               const MaskInputView& mask, std::uint64_t vocabulary, unsigned depth = 0)
  {
    work();
    if (depth >= 64) rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
    const auto program = shapeProgram(name);
    if (program && program->scalar && integerProgram(*program) && program->values.size() == 1)
      return extentAtom(program->values[0], program->castLimit, tokens, mask, vocabulary, depth + 1);
    const auto* view = producer(name);
    if (view && standard(*view) && view->outputs.size() == 1 && view->inputs.size() == 1 &&
        (view->opType == "Identity" || (view->opType == "Cast" && view->opset >= 6))) {
      const auto type = scalarType(name), before = scalarType(scoped(view->inputs[0]));
      if ((type != 6 && type != 7) || (before != 6 && before != 7)) return std::nullopt;
      auto result = extentValue(scoped(view->inputs[0]), tokens, mask, vocabulary, depth + 1);
      if (!result) return std::nullopt;
      if (type == 6) {
        if (result->kind == ExtentKind::FIXED &&
            (result->fixed < INT32_MIN || result->fixed > INT32_MAX)) return std::nullopt;
        result->castLimit = std::min(result->castLimit, std::uint64_t(INT32_MAX));
      }
      if (result->kind == ExtentKind::BATCH && !batchWithin(tokens, result->castLimit))
        return std::nullopt;
      return result;
    }
    const auto* add = standardProducer(name, "Add", 2, 7);
    if (!add || (scalarType(name) != 6 && scalarType(name) != 7)) return std::nullopt;
    const auto a = extentValue(scoped(add->inputs[0]), tokens, mask, vocabulary, depth + 1);
    const auto b = extentValue(scoped(add->inputs[1]), tokens, mask, vocabulary, depth + 1);
    if (!a || !b) return std::nullopt;
    const auto limit = std::min({a->castLimit, b->castLimit,
      scalarType(name) == 6 ? std::uint64_t(INT32_MAX) : std::uint64_t(INT64_MAX)});
    if ((a->kind == ExtentKind::PAST && b->kind == ExtentKind::SEQUENCE) ||
        (b->kind == ExtentKind::PAST && a->kind == ExtentKind::SEQUENCE))
      return ExtentValue{ExtentKind::TOTAL, 0, limit};
    if (a->kind == ExtentKind::FIXED && a->fixed == 0) {
      auto result = *b; result.castLimit = limit; return result;
    }
    if (b->kind == ExtentKind::FIXED && b->fixed == 0) {
      auto result = *a; result.castLimit = limit; return result;
    }
    if (a->kind == ExtentKind::FIXED && b->kind == ExtentKind::FIXED &&
        !(b->fixed > 0 && a->fixed > INT64_MAX - b->fixed) &&
        !(b->fixed < 0 && a->fixed < INT64_MIN - b->fixed)) {
      const auto sum = a->fixed + b->fixed;
      if (scalarType(name) == 6 && (sum < INT32_MIN || sum > INT32_MAX)) return std::nullopt;
      return ExtentValue{ExtentKind::FIXED, sum, limit};
    }
    return std::nullopt;
  }

  std::optional<std::vector<ExtentValue>>
  extentVector(const std::string& name, const TokenAxes& tokens,
                const MaskInputView& mask, std::uint64_t vocabulary, unsigned depth = 0)
  {
    work();
    if (depth >= 64) rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
    if (scalarType(name) != 7) return std::nullopt;
    const auto program = shapeProgram(name);
    if (program && !program->scalar && integerProgram(*program)) {
      std::vector<ExtentValue> result;
      for (const auto& value : program->values) {
        const auto extent = extentAtom(value, program->castLimit, tokens, mask, vocabulary,
                                        depth + 1);
        if (!extent) return std::nullopt;
        result.push_back(*extent);
      }
      charge(128 + result.size() * sizeof(ExtentValue));
      return result;
    }
    const auto* op = producer(name);
    if (!op || !standard(*op) || op->outputs.size() != 1) return std::nullopt;
    if ((op->opType == "Identity" || (op->opType == "Cast" && op->opset >= 6)) &&
        op->inputs.size() == 1 && scalarType(scoped(op->inputs[0])) == 7)
      return extentVector(scoped(op->inputs[0]), tokens, mask, vocabulary, depth + 1);
    if (op->opType == "Concat" && attribute(*op, "axis", 0) == 0) {
      std::vector<ExtentValue> result;
      for (const auto& input : op->inputs) {
        const auto part = extentVector(scoped(input), tokens, mask, vocabulary, depth + 1);
        if (!part || part->size() > 64 - result.size()) return std::nullopt;
        result.insert(result.end(), part->begin(), part->end());
      }
      charge(128 + result.size() * sizeof(ExtentValue)); return result;
    }
    if (const auto axes = unsqueezeAxes(*op)) {
      if (axes->size() != 1 || axisIndex(axes->front(), 1) != 0) return std::nullopt;
      const auto value = extentValue(scoped(op->inputs[0]), tokens, mask, vocabulary, depth + 1);
      if (value) return std::vector<ExtentValue>{*value};
      return std::nullopt;
    }
    if (op->opType == "Reshape" && op->opset >= 5 && op->inputs.size() == 2 &&
        attribute(*op, "allowzero", 0) == 0 &&
        !(op->opset < 14 && op->integerAttributes.count("allowzero"))) {
      const auto source = extentVector(scoped(op->inputs[0]), tokens, mask, vocabulary, depth + 1);
      const auto shape = shapeProgram(scoped(op->inputs[1]));
      if (!source || !shape || !safeHeadShape(*shape) || shape->values.size() != 1)
        return std::nullopt;
      const auto count = fixedShapeValue(shape->values[0], *shape);
      if (count && (*count == -1 || (*count >= 0 && std::uint64_t(*count) == source->size())))
        return source;
      return std::nullopt;
    }
    if (op->opType == "ConstantOfShape" && op->opset >= 9 && op->inputs.size() == 1) {
      const auto fill = op->integerListAttributes.find("tensor.value.int64");
      const auto* shape = standardProducer(scoped(op->inputs[0]), "Shape", 1);
      if (fill == op->integerListAttributes.end() || fill->second.size() != 1 || !shape)
        return std::nullopt;
      const auto source = extentVector(scoped(shape->inputs[0]), tokens, mask, vocabulary, depth + 1);
      if (!source || source->size() > 64) return std::nullopt;
      return std::vector<ExtentValue>(source->size(), ExtentValue{ExtentKind::FIXED,
                                                                fill->second[0], INT64_MAX});
    }
    if (op->opType == "Mul" && op->opset >= 7 && op->inputs.size() == 2) {
      for (const auto port : {0U, 1U}) {
        const auto scalar = extentValue(scoped(op->inputs[port]), tokens, mask, vocabulary,
                                          depth + 1);
        const auto vector = extentVector(scoped(op->inputs[1 - port]), tokens, mask, vocabulary,
                                          depth + 1);
        if (!scalar || scalar->kind != ExtentKind::FIXED || scalar->fixed != -1 || !vector ||
            !std::all_of(vector->begin(), vector->end(), [](const auto& value) {
              return value.kind == ExtentKind::FIXED && value.fixed == 1;
            })) continue;
        auto result = *vector;
        for (auto& value : result) {
          value.fixed = -1; value.castLimit = std::min(value.castLimit, scalar->castLimit);
        }
        return result;
      }
    }
    if (op->opType == "Where" && op->opset >= 9 && op->inputs.size() == 3) {
      const auto* equal = standardProducer(scoped(op->inputs[0]), "Equal", 2, 11);
      if (!equal) return std::nullopt;
      const auto sourceName = scoped(op->inputs[2]);
      const auto side = scoped(equal->inputs[0]) == sourceName ? 0 :
                        scoped(equal->inputs[1]) == sourceName ? 1 : -1;
      if (side < 0) return std::nullopt;
      auto source = extentVector(sourceName, tokens, mask, vocabulary, depth + 1);
      const auto negative = extentVector(scoped(equal->inputs[1 - side]), tokens, mask,
                                          vocabulary, depth + 1);
      const auto ones = extentVector(scoped(op->inputs[1]), tokens, mask, vocabulary, depth + 1);
      if (!source || !negative || !ones || negative->size() != source->size() ||
          ones->size() != source->size()) return std::nullopt;
      for (std::size_t i = 0; i < source->size(); ++i) {
        if (negative->at(i).kind != ExtentKind::FIXED || negative->at(i).fixed != -1 ||
            ones->at(i).kind != ExtentKind::FIXED || ones->at(i).fixed != 1) return std::nullopt;
        if (source->at(i).kind == ExtentKind::FIXED && source->at(i).fixed == -1)
          source->at(i) = ones->at(i);
      }
      return source;
    }
    return std::nullopt;
  }

  bool
  rangeMatches(const std::string& name, std::uint64_t axis, ExtentKind start, ExtentKind end,
                const TokenAxes& tokens, const MaskInputView& mask, std::uint64_t vocabulary,
                std::uint64_t& limit)
  {
    const auto view = rangeView(name);
    if (!view || view->rank != 4 || view->axis != axis) return false;
    const auto& op = *view->range;
    const auto type = scalarType(scoped(op.inputs[0]));
    if ((type != 6 && type != 7) || scalarType(scoped(op.inputs[1])) != type ||
        scalarType(scoped(op.inputs[2])) != type) return false;
    const auto a = extentValue(scoped(op.inputs[0]), tokens, mask, vocabulary);
    const auto b = extentValue(scoped(op.inputs[1]), tokens, mask, vocabulary);
    const auto step = extentValue(scoped(op.inputs[2]), tokens, mask, vocabulary);
    if (!a || !b || !step || a->kind != start || b->kind != end ||
        (start == ExtentKind::FIXED && a->fixed != 0) ||
        step->kind != ExtentKind::FIXED || step->fixed != 1) return false;
    if (end == ExtentKind::BATCH)
      return batchWithin(tokens, std::min(b->castLimit, view->castLimit));
    limit = std::min({limit, b->castLimit, view->castLimit});
    if (a->kind != ExtentKind::FIXED) limit = std::min(limit, a->castLimit);
    return true;
  }

  std::optional<MaskInputView>
  directMaskView(std::string name)
  {
    if (scalarType(name) != 9) return std::nullopt;
    std::uint64_t count = 4;
    std::array<std::uint64_t, 2> axes{0, 3};
    for (unsigned depth = 0; depth < 64; ++depth) {
      work();
      const auto declared = rank(name);
      if (declared && *declared != count) return std::nullopt;
      if (count == 2 && (scalarType(name) == 6 || scalarType(name) == 7)) {
        const auto root = integerInputAxes(name, axes, 1);
        if (root) return MaskInputView{root->input, root->axes};
        return std::nullopt;
      }
      const auto* op = producer(name);
      if (!op || !standard(*op) || op->outputs.size() != 1 || op->inputs.empty())
        return std::nullopt;
      if ((op->opType == "Identity" || (op->opType == "Cast" && op->opset >= 6 &&
            attribute(*op, "to", 0) == 9)) && op->inputs.size() == 1) {
        name = scoped(op->inputs[0]); continue;
      }
      const auto inserted = unsqueezeAxes(*op);
      if (!inserted || inserted->size() > count || count - inserted->size() < 2)
        return std::nullopt;
      std::set<std::uint64_t> positions;
      for (const auto position : *inserted)
        if (!positions.insert(axisIndex(position, count)).second) return std::nullopt;
      for (auto& axis : axes) {
        if (positions.count(axis)) return std::nullopt;
        axis -= std::distance(positions.begin(), positions.lower_bound(axis));
      }
      count -= positions.size(); name = scoped(op->inputs[0]);
    }
    rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
  }

  std::optional<MaskInputView>
  flattenedMaskView(const std::string& name, const TokenAxes& tokens,
                    std::uint64_t vocabulary, std::uint64_t& limit)
  {
    auto restored = dataView(name);
    const auto* restore = standardProducer(restored, "Reshape", 2, 5);
    if (!restore || attribute(*restore, "allowzero", 0) != 0) return std::nullopt;
    auto gathered = dataView(scoped(restore->inputs[0]));
    if (const auto* flatten = standardProducer(gathered, "Reshape", 2, 5)) {
      const auto shape = shapeProgram(scoped(flatten->inputs[1]));
      if (!shape || !safeHeadShape(*shape) || shape->values.size() != 1 ||
          shape->values[0].literal != std::optional<std::int64_t>(-1) ||
          attribute(*flatten, "allowzero", 0) != 0) return std::nullopt;
      gathered = dataView(scoped(flatten->inputs[0]));
    }
    const auto* gather = standardProducer(gathered, "Gather", 2);
    const auto* flat = gather ? standardProducer(scoped(gather->inputs[0]), "Flatten", 1) : nullptr;
    if (!gather || !flat || axisIndex(attribute(*gather, "axis", 0), 2) != 1 ||
        attribute(*flat, "axis", 1) != 0) return std::nullopt;
    auto original = scoped(flat->inputs[0]);
    const auto* cast = standardProducer(original, "Cast", 1, 6);
    if (!cast || attribute(*cast, "to", 0) != 9) return std::nullopt;
    const auto root = integerInputAxes(scoped(cast->inputs[0]), {0, 1}, 1);
    if (!root) return std::nullopt;
    const MaskInputView result{root->input, root->axes};
    const auto indices = scoped(gather->inputs[1]);
    const auto* add = standardProducer(indices, "Add", 2, 7);
    const auto declared = rank(indices);
    if (!add || (declared && *declared != 4)) return std::nullopt;
    auto shapeName = scoped(restore->inputs[1]);
    for (unsigned depth = 0; ; ++depth) {
      work();
      if (depth >= 64) rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
      const auto* tensor = fact(shapeName);
      const auto count = rank(shapeName);
      if (scalarType(shapeName) != 7 || (count && *count != 1) ||
          (tensor && tensor->onnxElementType != 0 && tensor->onnxElementType != 7))
        return std::nullopt;
      const auto* concatenate = standardProducer(shapeName, "Concat", 1, 4);
      if (!concatenate || attribute(*concatenate, "axis", 0) != 0) break;
      shapeName = scoped(concatenate->inputs[0]);
    }
    const auto* exactShape = standardProducer(shapeName, "Shape", 1);
    const bool sameShape = exactShape && scoped(exactShape->inputs[0]) == indices &&
      attribute(*exactShape, "start", 0) == 0 && attribute(*exactShape, "end", 4) >= 4 &&
      (exactShape->opset >= 15 || (exactShape->integerAttributes.count("start") == 0 &&
                                  exactShape->integerAttributes.count("end") == 0));
    if (!sameShape) {
      const auto shape = shapeProgram(scoped(restore->inputs[1]));
      if (!shape || !safeHeadShape(*shape) || shape->values.size() != 4) return std::nullopt;
      for (std::uint64_t axis = 0; axis < 4; ++axis)
        if (!shapeAxisMatches(shape->values[axis], *shape, indices, axis)) return std::nullopt;
    }
    for (std::size_t port = 0; port < 2; ++port) {
      const auto* multiply = standardProducer(scoped(add->inputs[port]), "Mul", 2, 7);
      if (!multiply) continue;
      for (std::size_t side = 0; side < 2; ++side) {
        auto bound = limit;
        const auto stride = extentValue(scoped(multiply->inputs[1 - side]), tokens, result,
                                         vocabulary);
        if (!stride || stride->kind != ExtentKind::TOTAL ||
            !rangeMatches(scoped(multiply->inputs[side]), 0, ExtentKind::FIXED,
                          ExtentKind::BATCH, tokens, result, vocabulary, bound) ||
            !rangeMatches(scoped(add->inputs[1 - port]), 3, ExtentKind::FIXED,
                          ExtentKind::TOTAL, tokens, result, vocabulary, bound)) continue;
        limit = std::min(bound, stride->castLimit); return result;
      }
    }
    return std::nullopt;
  }

  bool
  trueBoolean(const std::string& name)
  {
    const auto* op = standardProducer(name, "ConstantOfShape", 1, 9);
    if (!op) return false;
    const auto fill = op->integerListAttributes.find("tensor.value.bool");
    if (fill == op->integerListAttributes.end() || fill->second != std::vector<std::int64_t>{1})
      return false;
    const auto shape = shapeProgram(scoped(op->inputs[0]));
    return shape && safeHeadShape(*shape) && shape->values.size() <= 4 &&
      std::all_of(shape->values.begin(), shape->values.end(), [](const auto& value) {
        return value.literal == std::optional<std::int64_t>(1);
      });
  }

  std::string
  booleanView(std::string name)
  {
    for (unsigned depth = 0; depth < 64; ++depth) {
      work();
      const auto* op = producer(name);
      if (!op || !standard(*op) || op->outputs.size() != 1 || op->inputs.empty()) return name;
      if ((op->opType == "Identity" || (op->opType == "Cast" && op->opset >= 6 &&
            attribute(*op, "to", 0) == 9 && scalarType(scoped(op->inputs[0])) == 9)) &&
          op->inputs.size() == 1) { name = scoped(op->inputs[0]); continue; }
      if (op->opType == "And" && op->inputs.size() == 2) {
        if (trueBoolean(scoped(op->inputs[0]))) { name = scoped(op->inputs[1]); continue; }
        if (trueBoolean(scoped(op->inputs[1]))) { name = scoped(op->inputs[0]); continue; }
      }
      return name;
    }
    rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
  }

  std::optional<MaskLengthProgram>
  primitiveMask(const std::string& value, const TokenAxes& tokens, std::uint64_t vocabulary)
  {
    auto name = booleanView(value);
    const AutomaticOnnxOperatorFact* expansion = nullptr;
    const AutomaticOnnxOperatorFact* slice = standardProducer(name, "Slice", 5, 10);
    if (slice) name = booleanView(scoped(slice->inputs[0]));
    if ((expansion = standardProducer(name, "Expand", 2, 8)))
      name = booleanView(scoped(expansion->inputs[0]));
    const auto* both = standardProducer(name, "And", 2, 7);
    if (!both) return std::nullopt;
    for (std::size_t port = 0; port < 2; ++port) {
      std::uint64_t limit = INT64_MAX;
      auto mask = directMaskView(scoped(both->inputs[port]));
      if (!mask) mask = flattenedMaskView(scoped(both->inputs[port]), tokens, vocabulary, limit);
      const auto* causal = standardProducer(booleanView(scoped(both->inputs[1 - port])),
                                            "LessOrEqual", 2, 12);
      if (!mask || !causal ||
          !rangeMatches(scoped(causal->inputs[0]), 3, ExtentKind::FIXED, ExtentKind::TOTAL,
                        tokens, *mask, vocabulary, limit) ||
          !rangeMatches(scoped(causal->inputs[1]), 2, ExtentKind::PAST, ExtentKind::TOTAL,
                        tokens, *mask, vocabulary, limit)) continue;
      if (expansion) {
        const auto shape = extentVector(scoped(expansion->inputs[1]), tokens, *mask, vocabulary);
        if (!shape || shape->size() != 4 || shape->at(1).kind != ExtentKind::FIXED ||
            shape->at(1).fixed != 1) continue;
        bool valid = true;
        for (const auto axis : {0U, 2U, 3U}) {
          const auto expected = axis == 0 ? ExtentKind::BATCH : axis == 2 ?
            ExtentKind::SEQUENCE : ExtentKind::TOTAL;
          const auto& extent = shape->at(axis);
          if (extent.kind == ExtentKind::FIXED && extent.fixed == 1) continue;
          if (extent.kind != expected) { valid = false; break; }
          if (axis != 0) limit = std::min(limit, extent.castLimit);
        }
        if (!valid) continue;
      }
      if (slice) {
        const auto starts = shapeProgram(scoped(slice->inputs[1]));
        const auto ends = extentVector(scoped(slice->inputs[2]), tokens, *mask, vocabulary);
        const auto axes = shapeProgram(scoped(slice->inputs[3]));
        const auto steps = shapeProgram(scoped(slice->inputs[4]));
        if (!starts || !ends || !axes || !steps || !safeHeadShape(*starts) ||
            !safeHeadShape(*axes) || !safeHeadShape(*steps) ||
            starts->values.size() != 1 || ends->size() != 1 || axes->values.size() != 1 ||
            steps->values.size() != 1 || starts->values[0].literal != std::optional<std::int64_t>(0) ||
            steps->values[0].literal != std::optional<std::int64_t>(1) || !axes->values[0].literal ||
            axisIndex(*axes->values[0].literal, 4) != 3) continue;
        if (ends->front().kind != ExtentKind::TOTAL) continue;
        limit = std::min(limit, ends->front().castLimit);
      }
      MaskLengthProgram result;
      result.mask = mask->input; result.batchAxis = mask->axes[0];
      result.contextAxis = mask->axes[1]; result.castLimit = limit;
      return result;
    }
    return std::nullopt;
  }

  bool
  safeHeadShape(const IntegerShapeProgram& program) const
  {
    return integerProgram(program) && program.elementType == 7 && !program.scalar &&
      std::all_of(program.values.begin(), program.values.end(), [&](const auto& value) {
        return safeShapeValue(value, program);
      });
  }

  std::optional<std::pair<TokenAxes, std::uint64_t>>
  tokenDimension(TensorAxisReference axis)
  {
    work();
    if (m_inputs.count(axis.tensor) && axis.axis < 2) {
      const auto* tensor = fact(axis.tensor);
      if (tensor && tensor->dimensions && tensor->dimensions->size() == 2 &&
          (tensor->onnxElementType == 6 || tensor->onnxElementType == 7)) {
        TokenAxes root; root.input = axis.tensor; root.axes = {0, 1};
        return std::make_pair(std::move(root), axis.axis);
      }
    }
    if (axis.axis < 2) {
      const auto traced = tokenAxes(axis.tensor, {0, 1}, 0);
      if (traced) return std::make_pair(*traced, axis.axis);
    }
    const auto view = headView(axis.tensor);
    if (!view) return std::nullopt;
    const auto logical = axis.axis == view->axes[0] ? 0 : axis.axis == view->axes[2] ? 1 : -1;
    if (logical < 0) return std::nullopt;
    const auto traced = tokenAxes(view->source, {0, 1}, 0);
    if (!traced) return std::nullopt;
    return std::make_pair(*traced, static_cast<std::uint64_t>(logical));
  }

  bool
  shapeAxisMatches(const IntegerShapeValue& value, const IntegerShapeProgram& program,
                   const std::string& tensor, std::uint64_t axis,
                   std::optional<std::uint64_t> fixed = {})
  {
    if (!safeShapeValue(value, program)) return false;
    if (fixed) {
      const auto extent = fixedShapeValue(value, program);
      return extent && *extent >= 0 && static_cast<std::uint64_t>(*extent) == *fixed;
    }
    if (value.literal) {
      if (*value.literal < 0) return false;
      const auto* source = fact(tensor);
      return source && source->dimensions && axis < source->dimensions->size() &&
        source->dimensions->at(axis).kind == DimensionConstraint::Kind::FIXED &&
        source->dimensions->at(axis).fixed == *value.literal;
    }
    const auto actual = canonicalAxis(value.axis);
    const auto expected = canonicalAxis({tensor, axis});
    if (sameDimension(actual.tensor, actual.axis, expected.tensor, expected.axis)) return true;
    const auto a = tokenDimension(actual), b = tokenDimension(expected);
    return a && b && a->first.input == b->first.input &&
      a->first.axes[a->second] == b->first.axes[b->second] &&
      a->first.factors[a->second] == b->first.factors[b->second];
  }

  std::optional<HeadRepeat>
  headRepeat(const std::string& name, unsigned depth)
  {
    work();
    if (depth >= 64) rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
    const auto* merge = producer(name);
    if (!merge || !standard(*merge) || merge->opType != "Reshape" || merge->opset < 5 ||
        merge->inputs.size() != 2 || merge->outputs.size() != 1 ||
        (merge->opset < 14 && merge->integerAttributes.count("allowzero")) ||
        attribute(*merge, "allowzero", 0) != 0) return std::nullopt;
    const auto* expand = producer(scoped(merge->inputs[0]));
    if (!expand || !standard(*expand) || expand->opType != "Expand" || expand->opset < 8 ||
        expand->inputs.size() != 2 || expand->outputs.size() != 1) return std::nullopt;
    const auto* insert = producer(scoped(expand->inputs[0]));
    if (!insert || !standard(*insert) || insert->opType != "Unsqueeze" ||
        insert->outputs.size() != 1 || insert->inputs.empty()) return std::nullopt;
    for (const auto* view : {expand, insert}) {
      const auto count = rank(scoped(view->outputs[0]));
      if (count && *count != 5) return std::nullopt;
    }
    std::vector<std::int64_t> axes;
    if (insert->opset >= 13 && insert->inputs.size() == 2) {
      const auto encoded = shapeProgram(scoped(insert->inputs[1]));
      if (!encoded || !safeHeadShape(*encoded)) return std::nullopt;
      for (const auto& value : encoded->values) {
        if (!value.literal) return std::nullopt;
        axes.push_back(*value.literal);
      }
    }
    else if (insert->opset <= 12 && insert->inputs.size() == 1) {
      const auto found = insert->integerListAttributes.find("axes");
      if (found == insert->integerListAttributes.end()) return std::nullopt;
      axes = found->second;
    }
    if (axes.size() != 1 || axisIndex(axes[0], 5) != 2) return std::nullopt;
    const auto source = scoped(insert->inputs[0]);
    const auto* tensor = fact(source);
    std::uint64_t heads = 0, width = 0;
    const auto cache = m_headGeometry.find(source);
    const auto geometry = cache == m_headGeometry.end() ? headView(source, depth + 1) :
                                                         std::optional<HeadView>{};
    if (cache != m_headGeometry.end()) {
      if (cache->second.axes != IDENTITY_AXES) return std::nullopt;
      heads = cache->second.heads; width = cache->second.headSize;
    }
    else if (geometry && geometry->axes == IDENTITY_AXES) {
      heads = geometry->heads; width = geometry->headSize;
    }
    else {
      if (!tensor || !tensor->dimensions || tensor->dimensions->size() != 4)
        return std::nullopt;
      for (const auto axis : {1U, 3U}) {
        const auto& dimension = tensor->dimensions->at(axis);
        if (dimension.kind != DimensionConstraint::Kind::FIXED || dimension.fixed <= 0)
          return std::nullopt;
        (axis == 1 ? heads : width) = static_cast<std::uint64_t>(dimension.fixed);
      }
    }
    if (heads == 0 || width == 0) return std::nullopt;
    const auto expanded = shapeProgram(scoped(expand->inputs[1]));
    const auto merged = shapeProgram(scoped(merge->inputs[1]));
    if (!expanded || !merged || !safeHeadShape(*expanded) || !safeHeadShape(*merged) ||
        expanded->values.size() != 5 || merged->values.size() != 4) return std::nullopt;
    const auto factor = fixedShapeValue(expanded->values[2], *expanded);
    if (!factor || *factor <= 0 || heads >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
          static_cast<std::uint64_t>(*factor)) return std::nullopt;
    for (const auto axis : {0U, 1U, 2U, 3U}) {
      const auto expected = axis == 1 ? std::optional<std::uint64_t>(heads) :
                            axis == 3 ? std::optional<std::uint64_t>(width) : std::nullopt;
      if (!shapeAxisMatches(expanded->values[axis < 2 ? axis : axis + 1], *expanded,
                            source, axis, expected) ||
          !shapeAxisMatches(merged->values[axis], *merged, source, axis,
            axis == 1 ? std::optional<std::uint64_t>(heads * *factor) : expected))
        return std::nullopt;
    }
    const auto type = scalarType(source);
    if (type == 0 || scalarType(name) != type) return std::nullopt;
    charge(sizeof(HeadRepeat) + source.size());
    return HeadRepeat{source, static_cast<std::uint64_t>(*factor)};
  }

  std::optional<HeadView>
  headView(const std::string& name, unsigned depth = 0)
  {
    work();
    if (depth >= 64 || m_pendingHeads.size() >= 64)
      rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
    const auto found = m_headMemo.find(name);
    if (found != m_headMemo.end()) return found->second;
    charge(256 + name.size());
    if (!m_pendingHeads.insert(name).second) rejectState("AUTOMATIC_GENERATION_DATA_CYCLE");
    const auto result = computeHeadView(name, depth);
    if (result) {
      const auto* tensor = fact(name);
      if (tensor && tensor->dimensions) {
        for (const auto logical : {1U, 3U}) {
          const auto& dimension = tensor->dimensions->at(result->axes[logical]);
          const auto expected = logical == 1 ? result->heads : result->headSize;
          if (dimension.kind == DimensionConstraint::Kind::FIXED &&
              (dimension.fixed < 0 || static_cast<std::uint64_t>(dimension.fixed) != expected))
            rejectState("AUTOMATIC_GENERATION_HEAD_VIEW_DIMENSION");
        }
      }
    }
    if (result) charge(sizeof(HeadView) + result->source.size());
    m_pendingHeads.erase(name);
    m_headMemo.emplace(name, result);
    return result;
  }

  std::optional<HeadView>
  headRms(const AutomaticOnnxOperatorFact& op, unsigned depth)
  {
    if (!standard(op) || op.opType != "Mul" || op.inputs.size() != 2) return std::nullopt;
    for (const auto gammaPort : {0U, 1U}) {
      const auto* gamma = fact(scoped(op.inputs[gammaPort]));
      if (!gamma || !constantHeader(*gamma) || !gamma->dimensions ||
          gamma->dimensions->size() != 1) continue;
      const auto normalized = floatCasts(scoped(op.inputs[1 - gammaPort]));
      const auto* multiply = standardProducer(normalized, "Mul", 2, 7);
      if (!multiply) continue;
      for (const auto dataPort : {0U, 1U}) {
        const auto data = scoped(multiply->inputs[dataPort]);
        const auto type = scalarType(data);
        const auto* divide = standardProducer(scoped(multiply->inputs[1 - dataPort]), "Div", 2, 7);
        if (!divide || !scalarMatches(scoped(divide->inputs[0]), ScalarKind::ONE, type)) continue;
        const auto* squareRoot = standardProducer(scoped(divide->inputs[1]), "Sqrt", 1, 6);
        const auto* add = squareRoot ?
          standardProducer(scoped(squareRoot->inputs[0]), "Add", 2, 7) : nullptr;
        if (!add) continue;
        for (const auto statisticsPort : {0U, 1U}) {
          if (!scalarMatches(scoped(add->inputs[1 - statisticsPort]), ScalarKind::POSITIVE, type))
            continue;
          const auto* mean = producer(scoped(add->inputs[statisticsPort]));
          if (!mean || !standard(*mean) || mean->opType != "ReduceMean" ||
              mean->outputs.size() != 1 || attribute(*mean, "keepdims", 1) != 1 ||
              attribute(*mean, "noop_with_empty_axes", 0) != 0) continue;
          const auto* power = mean->inputs.empty() ? nullptr :
            standardProducer(scoped(mean->inputs[0]), "Pow", 2, 7);
          if (!power || scoped(power->inputs[0]) != data ||
              !scalarMatches(scoped(power->inputs[1]), ScalarKind::TWO, type)) continue;
          const auto source = floatCasts(data);
          const auto head = headView(source, depth + 1);
          if (!head || head->axes[3] != 3 || scalarType(source) != gamma->onnxElementType ||
              scalarType(scoped(op.outputs[0])) != gamma->onnxElementType ||
              fixedExtent(*gamma, 0) != head->headSize) continue;
          std::optional<std::vector<std::int64_t>> axes;
          if (mean->opset >= 18 && mean->inputs.size() == 2)
            axes = fixedVector(scoped(mean->inputs[1]));
          else if (mean->opset < 18 && mean->inputs.size() == 1) {
            const auto found = mean->integerListAttributes.find("axes");
            if (found != mean->integerListAttributes.end()) axes = found->second;
          }
          if (axes && axes->size() == 1 && (axes->front() == 3 || axes->front() == -1))
            return head;
        }
      }
    }
    return std::nullopt;
  }

  /** Remove only shape-preserving floating casts; integer casts are semantic controls. */
  std::string
  floatCasts(std::string name)
  {
    for (unsigned depth = 0; depth < 64; ++depth) {
      work();
      const auto* op = producer(name);
      if (!op || !standard(*op) || op->inputs.size() != 1 || op->outputs.size() != 1)
        return name;
      const auto first = scoped(op->inputs[0]);
      const auto floating = [] (std::int32_t type) {
        return type == 1 || type == 10 || type == 16;
      };
      if (op->opType != "Identity" && !(op->opType == "Cast" && op->opset >= 6 &&
          floating(scalarType(name)) && floating(scalarType(first)))) return name;
      name = first;
    }
    rejectState("AUTOMATIC_GENERATION_PROGRAM_DEPTH");
  }

  std::optional<std::vector<std::int64_t>>
  fixedVector(const std::string& name)
  {
    const auto program = shapeProgram(name);
    if (!program || program->scalar || program->elementType != 7 || !safeHeadShape(*program))
      return std::nullopt;
    std::vector<std::int64_t> result;
    for (const auto& value : program->values) {
      const auto fixed = fixedShapeValue(value, *program);
      if (!fixed) return std::nullopt;
      charge(sizeof(std::int64_t));
      result.push_back(*fixed);
    }
    return result;
  }

  std::optional<std::string>
  rotaryPhase(const std::string& value, const char* operation, const HeadView& head)
  {
    const auto name = floatCasts(value);
    const auto* unsqueeze = producer(name);
    if (!unsqueeze) return std::nullopt;
    const auto axes = unsqueezeAxes(*unsqueeze);
    if (!axes || axes->size() != 1 ||
        (axes->front() != static_cast<std::int64_t>(head.axes[1]) &&
         axes->front() != static_cast<std::int64_t>(head.axes[1]) - 4)) return std::nullopt;
    // The remaining dimensions must be [B,S,D]; inserting H may use any physical slot.
    for (const auto logical : {0U, 2U, 3U}) {
      const auto physical = head.axes[logical] - (head.axes[logical] > head.axes[1] ? 1 : 0);
      if (physical != (logical == 0 ? 0U : logical == 2 ? 1U : 2U)) return std::nullopt;
    }
    const auto trig = floatCasts(dataView(floatCasts(scoped(unsqueeze->inputs[0])), true));
    const auto* op = standardProducer(trig, operation, 1, 7);
    return op ? std::optional<std::string>(scoped(op->inputs[0])) : std::nullopt;
  }

  std::optional<std::string>
  primitivePositions(const std::string& phase, const HeadView& head)
  {
    const auto* duplicate = standardProducer(floatCasts(phase), "Concat", 2, 4);
    if (!duplicate || (attribute(*duplicate, "axis", 0) != 2 &&
                       attribute(*duplicate, "axis", 0) != -1) ||
        duplicate->inputs[0] != duplicate->inputs[1]) return std::nullopt;
    const auto* transpose = standardProducer(scoped(duplicate->inputs[0]), "Transpose", 1);
    if (!transpose || transpose->integerListAttributes.count("perm") == 0 ||
        transpose->integerListAttributes.at("perm") != std::vector<std::int64_t>{0, 2, 1})
      return std::nullopt;
    const auto* product = standardProducer(scoped(transpose->inputs[0]), "MatMul", 2);
    if (!product) return std::nullopt;
    const auto* expand = standardProducer(floatCasts(scoped(product->inputs[0])), "Expand", 2, 8);
    if (!expand) return std::nullopt;
    const auto* frequency = fact(floatCasts(scoped(expand->inputs[0])));
    if (!frequency || !constantHeader(*frequency) || !frequency->dimensions ||
        frequency->dimensions->size() != 3 || fixedExtent(*frequency, 0) != 1 ||
        fixedExtent(*frequency, 1) != head.headSize / 2 || fixedExtent(*frequency, 2) != 1)
      return std::nullopt;
    auto plane = floatCasts(scoped(product->inputs[1]));
    const auto* cast = standardProducer(plane, "Cast", 1, 6);
    if (!cast || (attribute(*cast, "to", 0) != 1 && attribute(*cast, "to", 0) != 10 &&
                  attribute(*cast, "to", 0) != 16)) return std::nullopt;
    const auto* unsqueeze = producer(scoped(cast->inputs[0]));
    if (!unsqueeze) return std::nullopt;
    const auto axes = unsqueezeAxes(*unsqueeze);
    if (!axes || axes->size() != 1 || (axes->front() != 1 && axes->front() != -2))
      return std::nullopt;
    const auto positions = scoped(unsqueeze->inputs[0]);
    const auto type = scalarType(positions);
    const auto dimensions = rank(positions);
    if ((type != 6 && type != 7) || (dimensions && *dimensions != 2) ||
        (!dimensions && !integerInputAxes(positions, {0, 1}, 0))) return std::nullopt;
    const auto shape = shapeProgram(scoped(expand->inputs[1]));
    if (!shape || !safeHeadShape(*shape) || shape->values.size() != 3 ||
        !shapeAxisMatches(shape->values[0], *shape, positions, 0) ||
        fixedShapeValue(shape->values[1], *shape) !=
          std::optional<std::int64_t>(head.headSize / 2) ||
        fixedShapeValue(shape->values[2], *shape) != std::optional<std::int64_t>(1))
      return std::nullopt;
    return positions;
  }

  bool
  rotarySlice(const std::string& name, const std::string& source,
              const HeadView& head, bool second)
  {
    const auto* slice = producer(name);
    if (!slice || !standard(*slice) || slice->opType != "Slice" || slice->opset < 10 ||
        slice->inputs.size() < 4 || slice->inputs.size() > 5 || slice->outputs.size() != 1 ||
        scoped(slice->inputs[0]) != source) return false;
    const auto starts = fixedVector(scoped(slice->inputs[1]));
    const auto ends = fixedVector(scoped(slice->inputs[2]));
    const auto axes = fixedVector(scoped(slice->inputs[3]));
    const auto steps = slice->inputs.size() == 5 ? fixedVector(scoped(slice->inputs[4])) :
      std::optional<std::vector<std::int64_t>>(std::vector<std::int64_t>{1});
    if (!starts || starts->size() != 1 || !ends || ends->size() != 1 ||
        !axes || axes->size() != 1 || !steps || *steps != std::vector<std::int64_t>{1})
      return false;
    const auto dimension = static_cast<std::int64_t>(head.headSize);
    const auto axis = axes->front() < 0 ? axes->front() + 4 : axes->front();
    return axis == static_cast<std::int64_t>(head.axes[3]) &&
      starts->front() == (second ? dimension / 2 : 0) &&
      (second ? ends->front() >= dimension : ends->front() == dimension / 2);
  }

  std::optional<HeadView>
  primitiveRotary(const AutomaticOnnxOperatorFact& op, unsigned depth)
  {
    if (!standard(op) || op.opType != "Add" || op.inputs.size() != 2) return std::nullopt;
    for (const auto cosinePort : {0U, 1U}) {
      const auto* cosineMultiply = standardProducer(scoped(op.inputs[cosinePort]), "Mul", 2, 7);
      const auto* sineMultiply = standardProducer(scoped(op.inputs[1 - cosinePort]), "Mul", 2, 7);
      if (!cosineMultiply || !sineMultiply) continue;
      for (const auto dataPort : {0U, 1U}) {
        const auto source = scoped(cosineMultiply->inputs[dataPort]);
        const auto head = headView(source, depth + 1);
        if (!head || head->headSize == 0 || head->headSize % 2 != 0) continue;
        const auto cosine = rotaryPhase(scoped(cosineMultiply->inputs[1 - dataPort]), "Cos", *head);
        if (!cosine) continue;
        for (const auto rotatedPort : {0U, 1U}) {
          const auto* concatenate = standardProducer(scoped(sineMultiply->inputs[rotatedPort]),
                                                     "Concat", 2, 4);
          if (!concatenate || (attribute(*concatenate, "axis", 0) !=
                static_cast<std::int64_t>(head->axes[3]) &&
              attribute(*concatenate, "axis", 0) !=
                static_cast<std::int64_t>(head->axes[3]) - 4)) continue;
          const auto* negate = standardProducer(scoped(concatenate->inputs[0]), "Neg", 1, 6);
          if (!negate || !rotarySlice(scoped(negate->inputs[0]), source, *head, true) ||
              !rotarySlice(scoped(concatenate->inputs[1]), source, *head, false)) continue;
          const auto sine = rotaryPhase(scoped(sineMultiply->inputs[1 - rotatedPort]),
                                        "Sin", *head);
          if (!sine || *sine != *cosine) continue;
          const auto positions = primitivePositions(*cosine, *head);
          if (!positions || scalarType(scoped(op.outputs[0])) != scalarType(source)) continue;
          const auto name = scoped(op.outputs[0]);
          charge(sizeof(PrimitiveRotaryEvidence) + name.size() + head->source.size() +
                 positions->size());
          m_primitiveRotary.emplace(name, PrimitiveRotaryEvidence{head->source, *positions});
          return head;
        }
      }
    }
    return std::nullopt;
  }

  std::optional<HeadView>
  computeHeadView(const std::string& name, unsigned depth)
  {
    const auto count = rank(name);
    if (count && *count != 4) return std::nullopt;
    const auto* op = producer(name);
    if (!op || !standard(*op) || op->inputs.empty() || op->outputs.size() != 1)
      return std::nullopt;
    if (op->opType == "Mul") return headRms(*op, depth);
    if (op->opType == "Add") return primitiveRotary(*op, depth);
    const auto first = scoped(op->inputs[0]);
    const auto type = scalarType(first);
    if (type == 0 || scalarType(name) != type) return std::nullopt;
    if ((op->opType == "Identity" || (op->opType == "Cast" && op->opset >= 6)) &&
        op->inputs.size() == 1)
      return headView(first, depth + 1);
    if (op->opType == "Transpose" && op->inputs.size() == 1) {
      auto source = headView(first, depth + 1);
      if (!source) return std::nullopt;
      const auto permutation = op->integerListAttributes.find("perm");
      const std::vector<std::int64_t> order = permutation == op->integerListAttributes.end() ?
        std::vector<std::int64_t>{3, 2, 1, 0} : permutation->second;
      if (order.size() != 4) return std::nullopt;
      Axes inverse{};
      std::set<std::int64_t> seen;
      for (std::size_t i = 0; i < 4; ++i) {
        if (order[i] < 0 || order[i] >= 4 || !seen.insert(order[i]).second)
          rejectState("AUTOMATIC_GENERATION_PERMUTATION");
        inverse[order[i]] = i;
      }
      for (auto& axis : source->axes) axis = inverse[axis];
      return source;
    }
    if (op->opType != "Reshape" || op->opset < 5 || op->inputs.size() != 2 ||
        (op->opset < 14 && op->integerAttributes.count("allowzero")) ||
        attribute(*op, "allowzero", 0) != 0) return std::nullopt;
    if (const auto repeated = headRepeat(name, depth + 1)) {
      auto source = headView(repeated->source, depth + 1);
      if (!source || source->axes != IDENTITY_AXES || source->heads >
          std::numeric_limits<std::uint64_t>::max() / repeated->factor) return std::nullopt;
      source->heads *= repeated->factor;
      return source;
    }
    const auto sourceRank = rank(first);
    if ((sourceRank && *sourceRank != 3) ||
        (!sourceRank && !tokenAxes(first, {0, 1}, 0))) return std::nullopt;
    const auto shape = shapeProgram(scoped(op->inputs[1]));
    const auto width = featureWidth(first);
    if (!shape || !safeHeadShape(*shape) || shape->values.size() != 4 || !width || *width == 0 ||
        *width > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return std::nullopt;
    for (const auto axis : {0U, 1U}) {
      if (shape->values[axis].literal == std::optional<std::int64_t>(0)) continue;
      if (!shapeAxisMatches(shape->values[axis], *shape, first, axis)) return std::nullopt;
    }
    auto heads = fixedShapeValue(shape->values[2], *shape);
    auto size = fixedShapeValue(shape->values[3], *shape);
    if (!heads || !size) return std::nullopt;
    if (*heads == -1 && *size > 0 && *width % *size == 0) heads = *width / *size;
    else if (*size == -1 && *heads > 0 && *width % *heads == 0) size = *width / *heads;
    if (*heads <= 0 || *size <= 0 || static_cast<std::uint64_t>(*heads) >
        std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(*size) ||
        static_cast<std::uint64_t>(*heads) * *size != *width) return std::nullopt;
    charge(sizeof(HeadView) + first.size());
    return HeadView{first, {0, 2, 1, 3}, static_cast<std::uint64_t>(*heads),
      static_cast<std::uint64_t>(*size)};
  }

  std::optional<HeadView>
  mergedHeadView(const AutomaticOnnxOperatorFact& op, unsigned depth)
  {
    if (!standard(op) || op.opType != "Reshape" || op.opset < 5 || op.inputs.size() != 2 ||
        (op.opset < 14 && op.integerAttributes.count("allowzero")) ||
        attribute(op, "allowzero", 0) != 0) return std::nullopt;
    const auto input = scoped(op.inputs[0]);
    auto source = headView(input, depth + 1);
    const auto shape = shapeProgram(scoped(op.inputs[1]));
    if (!source || source->axes != Axes{0, 2, 1, 3} || !shape || !safeHeadShape(*shape) ||
        shape->values.size() != 3 || source->heads >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
            source->headSize) return std::nullopt;
    for (const auto axis : {0U, 1U}) {
      if (shape->values[axis].literal == std::optional<std::int64_t>(0)) continue;
      if (!shapeAxisMatches(shape->values[axis], *shape, input, axis)) return std::nullopt;
    }
    const auto width = source->heads * source->headSize;
    const auto last = fixedShapeValue(shape->values[2], *shape);
    if (!last || (*last != -1 && (*last < 0 || static_cast<std::uint64_t>(*last) != width)))
      return std::nullopt;
    return source;
  }

  std::optional<std::uint64_t>
  computeFeatureWidth(const std::string& name, unsigned depth)
  {
    charge(96 + name.size());
    if (depth >= 512) rejectState("AUTOMATIC_GENERATION_DATA_DEPTH");
    const auto attentionOutput = m_attentionOutputs.find(name);
    if (attentionOutput != m_attentionOutputs.end())
      return m_attentions.at(attentionOutput->second).outputWidth;
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
      if (const auto merged = mergedHeadView(*op, 0)) return merged->heads * merged->headSize;
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
    const auto attentionOutput = m_attentionOutputs.find(name);
    if (attentionOutput != m_attentionOutputs.end() && axes[0] < 2 && axes[1] < 2)
      return tokenAxes(m_attentions.at(attentionOutput->second).projections[0], axes,
                       vocabulary, depth + 1);
    const auto* op = producer(name);
    if (!op || op->inputs.empty() || op->atomicBody || !op->functionScope.empty() ||
        !op->overload.empty() || !op->attributeReferences.empty()) return std::nullopt;
    const auto first = scoped(op->inputs[0]);
    const bool normal = standard(*op);
    if (normal && op->opType == "Gather" && op->inputs.size() == 2 &&
        attribute(*op, "axis", 0) == 0) {
      const auto table = constantMatrixShape(first);
      if (!table || (vocabulary != 0 && table->at(0) != vocabulary) ||
          axes[0] > 1 || axes[1] > 1) return std::nullopt;
      return integerInputAxes(scoped(op->inputs[1]), axes, table->at(0) - 1);
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
      if (const auto merged = mergedHeadView(*op, 0))
        return tokenAxes(merged->source, axes, vocabulary, depth + 1);
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
      const auto primitive = m_primitiveRotary.find(name);
      std::string source;
      std::string positionValue;
      if (primitive != m_primitiveRotary.end()) {
        source = primitive->second.source;
        positionValue = primitive->second.positions;
      }
      else {
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
        source = scoped(op->inputs[0]);
        positionValue = scoped(op->inputs[1]);
      }
      const auto local = tokenAxes(source, {0, 1}, generation.vocabularySize);
      if (!local || local->input != tokens.input || !unitFactors(*local))
        rejectState("AUTOMATIC_GENERATION_POSITION_AXES_UNPROVEN");
      const auto positions = positionInput(positionValue, *local, generation.contextLimit);
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
    const auto primitive = std::any_of(states.begin(), states.end(), [&](const auto& state) {
      return m_attentions.at(state.operatorIndex).primitive;
    });
    result.contextLimit = primitive ? std::uint64_t(INT64_MAX) : std::uint64_t(INT32_MAX);
    result.ruleId = primitive ? "causal-attention" : "causal-gqa";
    result.ruleVersion = primitive ? 2 : 1;
    if (std::any_of(states.begin(), states.end(), [] (const auto& state) {
          return state.update == AutomaticAttentionState::Update::APPEND_DELTA;
        })) result.ruleVersion = 3;
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
      if (state.update == AutomaticAttentionState::Update::REPLACE_FULL && produced.upperBound)
        result.contextLimit =
        std::min(result.contextLimit, *produced.upperBound);
      if (result.contextLimit == 0) return std::nullopt;
      if (!checked.insert(state.operatorIndex).second) continue;
      const auto& op = m_contract.operators.at(state.operatorIndex);
      const auto& evidence = m_attentions.at(state.operatorIndex);
      if (!upstream.count(&op) || (!evidence.primitive && attribute(op, "do_rotary", 0) != 0))
        return std::nullopt;
      for (std::size_t port = 0; port < 3; ++port) {
        const auto& activation = evidence.projections[port];
        const auto traced = tokenAxes(activation, {0, 1}, vocabulary);
        if (!traced || traced->input != tokens.input || traced->axes != tokens.axes ||
            !unitFactors(*traced)) return std::nullopt;
        mergeFixed(tokens.fixedAxes[0], traced->fixedAxes[0]);
        if (traced->fixedAxes[1] && *traced->fixedAxes[1] != 1) return std::nullopt;
        const auto width = featureWidth(activation);
        if (!width || *width != evidence.widths[port]) return std::nullopt;
        const auto* shape = fact(activation);
        if (shape && shape->dimensions && shape->dimensions->size() == 3) {
          compatibleBatch(shape->dimensions->at(0), batch);
          compatibleBatch(shape->dimensions->at(1), sequence);
        }
      }
      auto lengths = evidence.primitive ? primitiveMask(evidence.mask, tokens, vocabulary) :
                                          maskProgram(state.sequenceLengths);
      std::uint64_t totalLimit = INT64_MAX;
      if (!evidence.primitive) {
        const auto total = shapeProgram(state.totalLength);
        if (!lengths || lengths->offset != -1 || lengths->elementType != 6 || !total ||
            total->elementType != 6 || total->values.size() != 1 || total->values[0].literal ||
            total->values[0].axis.tensor != lengths->mask ||
            total->values[0].axis.axis != lengths->contextAxis) return std::nullopt;
        totalLimit = total->castLimit;
        const auto* sequencePort = fact(state.sequenceLengths);
        const auto* totalPort = fact(state.totalLength);
        if ((sequencePort && sequencePort->onnxElementType != 0 &&
             sequencePort->onnxElementType != 6) ||
            (totalPort && totalPort->onnxElementType != 0 && totalPort->onnxElementType != 6))
          return std::nullopt;
      }
      if (!lengths) return std::nullopt;
      if (!result.maskInput.empty() && (result.maskInput != lengths->mask ||
          result.maskBatchAxis != lengths->batchAxis ||
          result.maskContextAxis != lengths->contextAxis)) return std::nullopt;
      mergeFixed(tokens.fixedAxes[0], lengths->fixedBatch);
      result.maskInput = lengths->mask; result.maskBatchAxis = lengths->batchAxis;
      result.maskContextAxis = lengths->contextAxis;
      result.contextLimit = std::min({result.contextLimit, lengths->castLimit, totalLimit});
    }
    for (const auto* op : upstream) {
      const bool fused = op->domain == "com.microsoft" && op->opType == "RotaryEmbedding";
      const bool primitive = op->outputs.size() == 1 &&
        m_primitiveRotary.count(scoped(op->outputs[0]));
      if (!fused && !primitive) continue;
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

  std::optional<std::pair<std::string, Axes>>
  findOutputBoundary(const std::string& start)
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
    return result;
  }

  std::pair<std::string, Axes>
  outputBoundary(const std::string& start)
  {
    const auto result = findOutputBoundary(start);
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
  std::map<std::string, std::optional<HeadView>> m_headMemo;
  std::set<std::string> m_pendingHeads;
  std::map<std::string, TensorHeadGeometry> m_headGeometry;
  std::map<std::string, PrimitiveRotaryEvidence> m_primitiveRotary;
  std::map<std::uint64_t, AttentionEvidence> m_attentions;
  std::map<std::string, std::uint64_t> m_attentionOutputs;
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
  const char* schema = nullptr;
  switch (value.ruleVersion) {
    case 1: schema = "ndnsf-di-automatic-generation-v1"; break;
    case 2: schema = "ndnsf-di-automatic-generation-v2"; break;
    case 3: schema = "ndnsf-di-automatic-generation-v3"; break;
    default: rejectState("AUTOMATIC_GENERATION_RULE_VERSION");
  }
  auto states = NativeJson::array();
  for (const auto& state : value.states) {
    const char* role = nullptr;
    switch (state.role) {
      case AutomaticAttentionState::Role::KEY: role = "key"; break;
      case AutomaticAttentionState::Role::VALUE: role = "value"; break;
      default: rejectState("AUTOMATIC_GENERATION_STATE_ROLE");
    }
    const char* update = nullptr;
    switch (state.update) {
      case AutomaticAttentionState::Update::REPLACE_FULL: update = "replace_full"; break;
      case AutomaticAttentionState::Update::APPEND_DELTA: update = "append_delta"; break;
      default: rejectState("AUTOMATIC_GENERATION_STATE_UPDATE");
    }
    states.push_back(NativeJson{{"role", role}, {"operatorIndex", state.operatorIndex},
      {"input", state.input}, {"output", state.output}, {"query", state.query},
      {"key", state.key}, {"value", state.value}, {"sequenceLengths", state.sequenceLengths},
      {"totalLength", state.totalLength}, {"elementType", tensorElementTypeName(state.elementType)},
      {"inputAxes", state.inputAxes}, {"outputAxes", state.outputAxes},
      {"heads", state.heads}, {"headSize", state.headSize},
      {"initial", "empty"}, {"update", update}});
  }
  return NativeJson{{"schema", schema},
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
