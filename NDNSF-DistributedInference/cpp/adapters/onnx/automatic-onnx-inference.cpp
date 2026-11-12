#include "automatic-onnx-inference.hpp"
#include "onnx-tensor-type.hpp"

#include <onnx/defs/shape_inference.h>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace ndnsf::di {
namespace {

[[noreturn]] void
rejectInference(const char* reason)
{
  throw std::invalid_argument(reason);
}

class InferenceBudget
{
public:
  explicit InferenceBudget(const NativeAssemblyControl& control)
    : m_control(control)
    , m_limit(std::min(control.maxAssembledBytes, NativeCanonicalMaterialPublicationMaxBytes))
  {
    active();
  }

  void
  active() const
  {
    if (!m_control.requireActive || m_limit == 0)
      rejectInference("AUTOMATIC_INFERENCE_CONTROL");
    m_control.requireActive();
    if (std::chrono::steady_clock::now() >= m_control.deadline)
      throw std::runtime_error("AUTOMATIC_INFERENCE_DEADLINE");
  }

  void
  charge(std::uint64_t bytes)
  {
    active();
    if (bytes > m_limit - m_used) rejectInference("AUTOMATIC_INFERENCE_LIMIT");
    m_used += bytes;
  }

private:
  const NativeAssemblyControl& m_control;
  std::uint64_t m_limit = 0;
  std::uint64_t m_used = 0;
};

using InferenceTypes = std::map<std::string, onnx::TypeProto>;
using InferenceLiterals = std::map<std::string, onnx::TensorProto>;

// A declared symbol denotes the same extent everywhere in the root graph.
// Keep aliases until inference has finished so a fixed result constrains all uses.
class InferenceSymbols
{
public:
  explicit InferenceSymbols(InferenceBudget& budget)
    : m_budget(budget)
  {
  }

  std::string
  root(const std::string& name)
  {
    m_budget.active();
    if (name.compare(0, 6, "graph/") != 0 || name.size() == 6)
      rejectInference("AUTOMATIC_INFERENCE_SYMBOL");
    if (m_parents.find(name) == m_parents.end()) {
      m_budget.charge(256 + 3 * name.size());
      m_parents.emplace(name, name);
    }
    auto result = name;
    while (m_parents.at(result) != result) {
      m_budget.active();
      result = m_parents.at(result);
    }
    auto current = name;
    while (m_parents.at(current) != result) {
      m_budget.active();
      const auto next = m_parents.at(current);
      m_parents.at(current) = result;
      current = next;
    }
    return result;
  }

  void
  bind(const std::string& name, std::int64_t value)
  {
    if (value < 0) rejectInference("AUTOMATIC_INFERENCE_DIMENSION_CONFLICT");
    const auto key = root(name);
    const auto [found, inserted] = m_values.emplace(key, value);
    if (!inserted && found->second != value)
      rejectInference("AUTOMATIC_INFERENCE_DIMENSION_CONFLICT");
  }

  void
  unite(const std::string& first, const std::string& second)
  {
    auto left = root(first);
    auto right = root(second);
    if (left == right) return;
    if (right < left) std::swap(left, right);
    const auto value = m_values.find(right);
    if (value != m_values.end()) bind(left, value->second);
    m_parents.at(right) = left;
    m_values.erase(right);
  }

  DimensionConstraint
  resolve(DimensionConstraint axis)
  {
    if (axis.kind != DimensionConstraint::Kind::SYMBOL) return axis;
    axis.symbol = root(axis.symbol);
    const auto value = m_values.find(axis.symbol);
    if (value != m_values.end()) {
      if (axis.upperBound && static_cast<std::uint64_t>(value->second) > *axis.upperBound)
        rejectInference("AUTOMATIC_INFERENCE_DIMENSION_CONFLICT");
      axis = {DimensionConstraint::Kind::FIXED, value->second, {},
              static_cast<std::uint64_t>(value->second)};
    }
    return axis;
  }

private:
  InferenceBudget& m_budget;
  std::map<std::string, std::string> m_parents;
  std::map<std::string, std::int64_t> m_values;
};

onnx::TypeProto
inferenceType(const AutomaticTensorContract& tensor, InferenceSymbols& symbols,
              InferenceBudget& budget)
{
  budget.charge(sizeof(onnx::TypeProto) + tensor.name.size());
  onnx::TypeProto result;
  if (tensor.onnxElementType == 0 && !tensor.dimensions) return result;
  auto* type = result.mutable_tensor_type();
  type->set_elem_type(tensor.onnxElementType);
  if (!tensor.dimensions) return result;
  for (const auto& declared : *tensor.dimensions) {
    const auto dimension = symbols.resolve(declared);
    budget.charge(128 + dimension.symbol.size());
    auto* axis = type->mutable_shape()->add_dim();
    if (dimension.kind == DimensionConstraint::Kind::FIXED) axis->set_dim_value(dimension.fixed);
    else if (dimension.kind == DimensionConstraint::Kind::SYMBOL) {
      if (dimension.symbol.compare(0, 6, "graph/") != 0)
        rejectInference("AUTOMATIC_INFERENCE_SYMBOL");
      axis->set_dim_param(dimension.symbol.substr(6));
    }
  }
  // An empty shape is a scalar, whereas absence of shape means unknown rank.
  type->mutable_shape();
  return result;
}

class NodeInferenceContext final : public onnx::InferenceContext
{
public:
  NodeInferenceContext(const onnx::NodeProto& node, const InferenceTypes& types,
                       const InferenceLiterals& literals, InferenceBudget& budget)
    : m_node(node)
    , m_types(types)
    , m_literals(literals)
  {
    for (const auto& name : node.output()) {
      budget.active();
      budget.charge(name.size() + sizeof(onnx::TypeProto));
      // Infer the producer's result independently. SDK shape merging may prefer
      // an existing target symbol and hide an equality with the source symbol.
      // Original output declarations are merged strictly after this callback.
      m_outputs.emplace_back();
    }
  }

  const onnx::AttributeProto*
  getAttribute(const std::string& name) const override
  {
    for (const auto& attribute : m_node.attribute())
      if (attribute.name() == name) return &attribute;
    return nullptr;
  }

  std::size_t getNumInputs() const override { return m_node.input_size(); }
  std::size_t getNumOutputs() const override { return m_outputs.size(); }

  const onnx::TypeProto*
  getInputType(std::size_t index) const override
  {
    if (index >= getNumInputs()) return nullptr;
    const auto found = m_types.find(m_node.input(static_cast<int>(index)));
    return found == m_types.end() || !found->second.has_tensor_type() ? nullptr : &found->second;
  }

  bool
  hasInput(std::size_t index) const override
  {
    return index < getNumInputs() && !m_node.input(static_cast<int>(index)).empty();
  }

  const onnx::TensorProto*
  getInputData(std::size_t index) const override
  {
    if (index >= getNumInputs()) return nullptr;
    const auto found = m_literals.find(m_node.input(static_cast<int>(index)));
    return found == m_literals.end() ? nullptr : &found->second;
  }

  onnx::TypeProto*
  getOutputType(std::size_t index) override
  {
    return &m_outputs.at(index);
  }

  onnx::GraphInferencer*
  getGraphAttributeInferencer(const std::string&) override
  {
    return nullptr;
  }

  const onnx::SparseTensorProto* getInputSparseData(std::size_t) const override { return nullptr; }
  const onnx::TensorShapeProto* getSymbolicInput(std::size_t) const override { return nullptr; }
  std::string getDisplayName() const override { return m_node.name(); }

private:
  const onnx::NodeProto& m_node;
  const InferenceTypes& m_types;
  const InferenceLiterals& m_literals;
  std::vector<onnx::TypeProto> m_outputs;
};

void
propagateType(NodeInferenceContext& context, std::size_t input, std::size_t output)
{
  if (output >= context.getNumOutputs()) return;
  const auto* type = context.getInputType(input);
  if (type && type->tensor_type().elem_type() != 0)
    onnx::propagateElemTypeFromInputToOutput(context, input, output);
}

bool
inferAdditionalSchema(NodeInferenceContext& context, const AutomaticOnnxOperatorFact& op)
{
  if (op.domain == "com.microsoft" && op.opset == 1) {
    if (op.opType == "RotaryEmbedding") {
      if (context.getNumInputs() != 4 || context.getNumOutputs() != 1)
        rejectInference("AUTOMATIC_INFERENCE_ARITY");
      propagateType(context, 0, 0);
      onnx::propagateShapeFromInputToOutput(context, 0, 0);
      return true;
    }
    if (op.opType == "GroupQueryAttention") {
      if (context.getNumInputs() < 7 || context.getNumOutputs() < 3)
        rejectInference("AUTOMATIC_INFERENCE_ARITY");
      propagateType(context, 0, 0);
      // Packed QKV has a different feature width. Leave that shape opaque.
      if (context.hasInput(1) && context.hasInput(2) && onnx::hasInputShape(context, 0) &&
          onnx::getInputShape(context, 0).dim_size() == 3)
        onnx::propagateShapeFromInputToOutput(context, 0, 0);
      for (std::size_t role = 0; role < 2; ++role) {
        propagateType(context, 3 + role, 1 + role);
        if (!onnx::hasInputShape(context, 3 + role)) continue;
        auto shape = onnx::getInputShape(context, 3 + role);
        if (shape.dim_size() != 4) continue;
        // Capacity depends on total length and buffer policy, not just past.
        shape.mutable_dim(2)->Clear();
        onnx::updateOutputShape(context, 1 + role, shape);
      }
      if (context.getNumOutputs() > 3) propagateType(context, 0, 3);
      return true;
    }
    if (op.opType == "SkipSimplifiedLayerNormalization") {
      if (context.getNumInputs() < 3 || context.getNumOutputs() == 0)
        rejectInference("AUTOMATIC_INFERENCE_ARITY");
      for (const auto output : {std::size_t{0}, std::size_t{3}}) {
        if (output >= context.getNumOutputs()) continue;
        propagateType(context, 0, output);
        onnx::propagateShapeFromInputToOutput(context, 0, output);
      }
      for (std::size_t output = 1; output < std::min(context.getNumOutputs(), std::size_t{3});
           ++output)
        context.getOutputType(output)->mutable_tensor_type()->set_elem_type(onnx::TensorProto::FLOAT);
      return true;
    }
  }
  if ((op.domain.empty() || op.domain == "ai.onnx") && op.opset >= 1 && op.opset <= 21 &&
      op.opType == "SimplifiedLayerNormalization") {
    if (context.getNumInputs() < 2 || context.getNumOutputs() == 0)
      rejectInference("AUTOMATIC_INFERENCE_ARITY");
    propagateType(context, 1, 0);
    onnx::propagateShapeFromInputToOutput(context, 0, 0);
    if (context.getNumOutputs() > 1) {
      const auto* stash = context.getAttribute("stash_type");
      if (stash && (stash->type() != onnx::AttributeProto::INT || stash->i() <= 0 ||
                    stash->i() > std::numeric_limits<int>::max()))
        rejectInference("AUTOMATIC_INFERENCE_ATTRIBUTE");
      context.getOutputType(1)->mutable_tensor_type()->set_elem_type(
        stash ? static_cast<int>(stash->i()) : onnx::TensorProto::FLOAT);
    }
    return true;
  }
  return false;
}

void
mergeInferredType(AutomaticTensorContract& tensor, const onnx::TypeProto& inferred,
                  InferenceSymbols& symbols, InferenceBudget& budget)
{
  if (!inferred.has_tensor_type()) return;
  const auto& type = inferred.tensor_type();
  bool changed = false;
  if (type.elem_type() != 0) {
    if (tensor.onnxElementType != 0 && tensor.onnxElementType != type.elem_type())
      rejectInference("AUTOMATIC_INFERENCE_TYPE_CONFLICT");
    if (tensor.onnxElementType == 0) {
      tensor.onnxElementType = type.elem_type();
      tensor.wireElementType = onnxTensorWireType(type.elem_type());
      changed = true;
    }
  }
  if (type.has_shape()) {
    if (tensor.dimensions && tensor.dimensions->size() !=
        static_cast<std::size_t>(type.shape().dim_size()))
      rejectInference("AUTOMATIC_INFERENCE_RANK_CONFLICT");
    if (!tensor.dimensions) {
      budget.charge(sizeof(DimensionConstraint) * type.shape().dim_size());
      tensor.dimensions.emplace(type.shape().dim_size());
      changed = true;
    }
    for (int index = 0; index < type.shape().dim_size(); ++index) {
      budget.active();
      auto& axis = tensor.dimensions->at(index);
      const auto& value = type.shape().dim(index);
      if (value.has_dim_value()) {
        if (value.dim_value() < 0 ||
            (axis.kind == DimensionConstraint::Kind::FIXED && axis.fixed != value.dim_value()) ||
            (axis.upperBound && static_cast<std::uint64_t>(value.dim_value()) > *axis.upperBound))
          rejectInference("AUTOMATIC_INFERENCE_DIMENSION_CONFLICT");
        if (axis.kind == DimensionConstraint::Kind::SYMBOL)
          symbols.bind(axis.symbol, value.dim_value());
        if (axis.kind == DimensionConstraint::Kind::UNKNOWN) {
          axis = {DimensionConstraint::Kind::FIXED, value.dim_value(), {},
                  static_cast<std::uint64_t>(value.dim_value())};
          changed = true;
        }
      }
      else if (value.has_dim_param() && !value.dim_param().empty()) {
        budget.charge(value.dim_param().size() + 6);
        const auto symbol = "graph/" + value.dim_param();
        if (axis.kind == DimensionConstraint::Kind::FIXED) symbols.bind(symbol, axis.fixed);
        else if (axis.kind == DimensionConstraint::Kind::SYMBOL)
          symbols.unite(axis.symbol, symbol);
        else {
          axis.kind = DimensionConstraint::Kind::SYMBOL;
          axis.symbol = symbol;
          changed = true;
        }
      }
    }
  }
  if (changed) tensor.provenance = {"automatic-inference-v1"};
}

} // namespace

void
refineAutomaticOnnxTensorFacts(const onnx::ModelProto& source,
                              const onnx::ISchemaRegistry& schemas,
                              AutomaticOnnxContract& contract,
                              const NativeAssemblyControl& control)
{
  InferenceBudget budget(control);
  InferenceSymbols symbols(budget);
  std::set<std::tuple<std::string, std::string, std::string>> functions;
  for (const auto& function : source.functions()) {
    budget.charge(function.domain().size() + function.name().size() + function.overload().size() + 96);
    functions.emplace(function.domain(), function.name(), function.overload());
  }
  InferenceTypes types;
  std::map<std::string, AutomaticTensorContract*> tensors;
  for (auto& tensor : contract.tensors) {
    if (tensor.name.compare(0, 6, "graph/") != 0)
      rejectInference("AUTOMATIC_INFERENCE_SCOPE");
    const auto name = tensor.name.substr(6);
    budget.charge(name.size() + sizeof(AutomaticTensorContract*));
    tensors.emplace(name, &tensor);
  }
  // Legacy inference can refine a public symbol before this metadata pass.
  // Recover that symbol's equality contract from the original declarations.
  const auto declaredSymbols = [&](const auto& values) {
    for (const auto& value : values) {
      budget.active();
      const auto found = tensors.find(value.name());
      if (found == tensors.end() || !value.type().has_tensor_type() ||
          !value.type().tensor_type().has_shape()) continue;
      auto& tensor = *found->second;
      if (tensor.provenance == std::vector<std::string>{"initializer-header-v1"} ||
          tensor.provenance == std::vector<std::string>{"constant-header-v1"}) continue;
      if (!tensor.dimensions) continue;
      const auto& shape = value.type().tensor_type().shape();
      if (tensor.dimensions->size() != static_cast<std::size_t>(shape.dim_size()))
        rejectInference("AUTOMATIC_INFERENCE_RANK_CONFLICT");
      for (int index = 0; index < shape.dim_size(); ++index) {
        const auto& original = shape.dim(index);
        if (!original.has_dim_param() || original.dim_param().empty()) continue;
        auto& axis = tensor.dimensions->at(index);
        const auto symbol = "graph/" + original.dim_param();
        if (axis.kind == DimensionConstraint::Kind::FIXED) symbols.bind(symbol, axis.fixed);
        else if (axis.kind == DimensionConstraint::Kind::SYMBOL)
          symbols.unite(symbol, axis.symbol);
        else {
          axis.kind = DimensionConstraint::Kind::SYMBOL;
          axis.symbol = symbol;
        }
      }
    }
  };
  declaredSymbols(source.graph().input());
  declaredSymbols(source.graph().output());
  declaredSymbols(source.graph().value_info());
  for (const auto& [name, tensor] : tensors)
    types.emplace(name, inferenceType(*tensor, symbols, budget));
  InferenceLiterals literals;
  for (const auto& [name, values] : contract.integerLiterals) {
    budget.charge(name.size() + 512 + sizeof(onnx::TensorProto));
    if (name.compare(0, 6, "graph/") != 0 || values.size() > 64)
      rejectInference("AUTOMATIC_INFERENCE_LITERAL");
    const auto found = tensors.find(name.substr(6));
    if (found == tensors.end() || !found->second->dimensions)
      rejectInference("AUTOMATIC_INFERENCE_LITERAL");
    const auto& tensor = *found->second;
    if (tensor.onnxElementType != 6 && tensor.onnxElementType != 7)
      rejectInference("AUTOMATIC_INFERENCE_LITERAL");
    auto& literal = literals[name.substr(6)];
    literal.set_data_type(tensor.onnxElementType);
    for (const auto& axis : *tensor.dimensions) {
      if (axis.kind != DimensionConstraint::Kind::FIXED)
        rejectInference("AUTOMATIC_INFERENCE_LITERAL");
      literal.add_dims(axis.fixed);
    }
    for (const auto value : values) {
      if (tensor.onnxElementType == 7) literal.add_int64_data(value);
      else {
        if (value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max())
          rejectInference("AUTOMATIC_INFERENCE_LITERAL");
        literal.add_int32_data(static_cast<std::int32_t>(value));
      }
    }
  }
  for (const auto& op : contract.operators) {
    budget.active();
    if (op.scope != "graph" || op.atomicBody || !op.functionScope.empty() ||
        !op.overload.empty() || !op.attributeReferences.empty()) continue;
    if (op.nodeIndex >= static_cast<std::uint64_t>(source.graph().node_size()))
      rejectInference("AUTOMATIC_INFERENCE_NODE");
    const auto& node = source.graph().node(static_cast<int>(op.nodeIndex));
    if (node.op_type() != op.opType || node.domain() != op.domain)
      rejectInference("AUTOMATIC_INFERENCE_NODE");
    if (functions.count({node.domain(), node.op_type(), node.overload()}) ||
        std::any_of(node.attribute().begin(), node.attribute().end(), [](const auto& attribute) {
          return !attribute.ref_attr_name().empty() ||
            attribute.type() == onnx::AttributeProto::GRAPH ||
            attribute.type() == onnx::AttributeProto::GRAPHS;
        })) continue;
    // Resolve fresh symbol bindings only for this node's ports, avoiding a
    // quadratic rebuild of every tensor after each inferred extent.
    for (const auto* ports : {&node.input(), &node.output()}) {
      for (const auto& name : *ports) {
        const auto found = tensors.find(name);
        if (found != tensors.end()) types[name] = inferenceType(*found->second, symbols, budget);
      }
    }
    NodeInferenceContext context(node, types, literals, budget);
    bool knownInputs = true;
    for (std::size_t index = 0; index < context.getNumInputs(); ++index) {
      if (!context.hasInput(index)) continue;
      const auto* type = context.getInputType(index);
      if (!type || type->tensor_type().elem_type() == 0) knownInputs = false;
    }
    if (!knownInputs) continue;
    if (!inferAdditionalSchema(context, op)) {
      if (op.opset < 0 || op.opset > std::numeric_limits<int>::max()) continue;
      const auto* schema = schemas.GetSchema(op.opType, static_cast<int>(op.opset), op.domain);
      if (!schema) continue;
      if (op.domain == "com.microsoft" && op.opType == "MatMulNBits" &&
          !onnx::hasInputShape(context, 0)) continue;
      const auto infer = schema->GetTypeAndShapeInferenceFunction();
      if (!infer) continue;
      infer(context);
    }
    for (int index = 0; index < node.output_size(); ++index) {
      const auto& name = node.output(index);
      if (name.empty()) continue;
      const auto found = tensors.find(name);
      if (found == tensors.end()) rejectInference("AUTOMATIC_INFERENCE_OUTPUT");
      const auto* output = context.getOutputType(index);
      budget.charge(output->ByteSizeLong());
      mergeInferredType(*found->second, *output, symbols, budget);
    }
  }
  for (auto& tensor : contract.tensors) {
    budget.active();
    if (!tensor.dimensions) continue;
    for (auto& axis : *tensor.dimensions) {
      const auto resolved = symbols.resolve(axis);
      if (axis.kind != resolved.kind || axis.fixed != resolved.fixed ||
          axis.symbol != resolved.symbol || axis.upperBound != resolved.upperBound)
        tensor.provenance = {"automatic-inference-v1"};
      axis = resolved;
    }
  }
  budget.active();
}

} // namespace ndnsf::di
