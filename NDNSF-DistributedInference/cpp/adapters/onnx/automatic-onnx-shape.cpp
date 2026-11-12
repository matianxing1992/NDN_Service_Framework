#include "automatic-onnx-model.hpp"
#include "automatic-onnx-admission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "automatic-onnx-generation.hpp"
#include "onnx-tensor-type.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ndnsf::di {
namespace {

[[noreturn]] void
rejectShape(const char* code)
{
  throw std::invalid_argument(code);
}

void
requireActive(const NativeAssemblyControl& control)
{
  if (!control.requireActive || control.maxAssembledBytes == 0)
    rejectShape("AUTOMATIC_CONTRACT_CONTROL");
  control.requireActive();
  if (std::chrono::steady_clock::now() >= control.deadline)
    throw std::runtime_error("AUTOMATIC_CONTRACT_DEADLINE");
}

std::uint64_t
metadataLimit(const NativeAssemblyControl& control)
{
  requireActive(control);
  return std::min(control.maxAssembledBytes, NativeCanonicalMaterialPublicationMaxBytes);
}

bool
isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [](unsigned char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void
requireName(const std::string& value)
{
  if (value.empty() || value.size() > 4096 || value.find('\0') != std::string::npos)
    rejectShape("AUTOMATIC_CONTRACT_NAME");
}

std::uint64_t
unsignedInteger(const NativeJson& value)
{
  if (!value.is_number_integer() ||
      (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
    rejectShape("AUTOMATIC_CONTRACT_UNSIGNED");
  return value.get<std::uint64_t>();
}

std::int64_t
signedInteger(const NativeJson& value)
{
  if (!value.is_number_integer() || (value.is_number_unsigned() &&
      value.get<std::uint64_t>() > std::uint64_t(std::numeric_limits<std::int64_t>::max())))
    rejectShape("AUTOMATIC_CONTRACT_INTEGER");
  return value.get<std::int64_t>();
}

std::vector<std::string>
stringArray(const NativeJson& value)
{
  if (!value.is_array()) rejectShape("AUTOMATIC_CONTRACT_ARRAY");
  std::vector<std::string> result;
  for (const auto& item : value) result.push_back(item.get<std::string>());
  return result;
}

const char*
relationName(ShapeRelation::Kind kind)
{
  switch (kind) {
    case ShapeRelation::Kind::EQUAL: return "equal";
    case ShapeRelation::Kind::SUM: return "sum";
    case ShapeRelation::Kind::PRODUCT: return "product";
    case ShapeRelation::Kind::BROADCAST: return "broadcast";
    case ShapeRelation::Kind::AT_MOST: return "at_most";
  }
  rejectShape("AUTOMATIC_CONTRACT_RELATION_KIND");
}

NativeJson
axisReferencesJson(const std::vector<TensorAxisReference>& axes)
{
  auto result = NativeJson::array();
  for (const auto& axis : axes)
    result.push_back(NativeJson{{"tensor", axis.tensor}, {"axis", axis.axis}});
  return result;
}

NativeJson
relationsJson(const std::vector<ShapeRelation>& relations)
{
  auto result = NativeJson::array();
  for (const auto& relation : relations) {
    result.push_back(NativeJson{{"kind", relationName(relation.kind)},
      {"outputs", axisReferencesJson(relation.outputs)},
      {"inputs", axisReferencesJson(relation.inputs)},
      {"constant", relation.constant}, {"provenance", relation.provenance}});
  }
  return result;
}

NativeJson
tensorsJson(const std::vector<AutomaticTensorContract>& tensors)
{
  auto result = NativeJson::array();
  for (const auto& tensor : tensors) {
    NativeJson dimensions = nullptr;
    if (tensor.dimensions) {
      dimensions = NativeJson::array();
      for (const auto& axis : *tensor.dimensions) {
        const char* kind = nullptr;
        switch (axis.kind) {
          case DimensionConstraint::Kind::FIXED: kind = "fixed"; break;
          case DimensionConstraint::Kind::SYMBOL: kind = "symbol"; break;
          case DimensionConstraint::Kind::UNKNOWN: kind = "unknown"; break;
        }
        if (kind == nullptr) rejectShape("AUTOMATIC_CONTRACT_DIMENSION_KIND");
        dimensions.push_back(NativeJson{{"kind", kind}, {"fixed", axis.fixed},
          {"symbol", axis.symbol},
          {"upper", axis.upperBound ? NativeJson(*axis.upperBound) : NativeJson(nullptr)}});
      }
    }
    result.push_back(NativeJson{{"name", tensor.name}, {"onnxType", tensor.onnxElementType},
      {"wireType", tensor.wireElementType ? NativeJson(tensorElementTypeName(*tensor.wireElementType)) :
                                           NativeJson(nullptr)},
      {"dimensions", dimensions}, {"provenance", tensor.provenance}});
  }
  return result;
}

NativeJson
contractJson(const AutomaticOnnxContract& contract)
{
  return NativeJson{{"schema", "ndnsf-di-automatic-onnx-contract-v2"},
    {"facts", nativeParseJson(contract.factsJson())}, {"factsDigest", contract.factsDigest},
    {"tensors", tensorsJson(contract.tensors)}, {"modelInputs", contract.modelInputs},
    {"modelOutputs", contract.modelOutputs}, {"relations", relationsJson(contract.relations)},
    {"integerLiterals", contract.integerLiterals},
    {"generation", automaticOnnxGenerationJson(contract.generation)}};
}

} // namespace

std::string
AutomaticOnnxContract::factsJson() const
{
  auto operations = NativeJson::array();
  for (const auto& op : operators) {
    operations.push_back(NativeJson{{"scope", op.scope}, {"index", op.nodeIndex},
      {"domain", op.domain}, {"op", op.opType}, {"overload", op.overload},
      {"functionScope", op.functionScope}, {"opset", op.opset},
      {"inputs", op.inputs}, {"outputs", op.outputs}, {"integers", op.integerAttributes},
      {"integerLists", op.integerListAttributes}, {"attributeReferences", op.attributeReferences},
      {"atomicBody", op.atomicBody}});
  }
  auto encodedQuantization = NativeJson::array();
  const auto integer = [](const std::optional<std::int64_t>& value) {
    return value ? NativeJson(*value) : NativeJson(nullptr);
  };
  for (const auto& quant : quantization) {
    auto constants = NativeJson::array();
    for (const auto& value : quant.constantInputs) {
      constants.push_back(NativeJson{{"input", value.inputIndex}, {"name", value.valueName},
        {"type", value.onnxElementType}, {"shape", value.shape},
        {"storageBytes", value.storageBytes}, {"normalizedDigest", value.normalizedContentDigest}});
    }
    encodedQuantization.push_back(NativeJson{{"operator", quant.operatorIndex},
      {"bits", integer(quant.bits)}, {"axis", integer(quant.axis)},
      {"blockSize", integer(quant.blockSize)},
      {"implicitZeroPoint", integer(quant.implicitZeroPoint)},
      {"constants", constants}, {"staticParametersValidated", quant.staticParametersValidated}});
  }
  return nativeCanonicalJson(NativeJson{{"schema", "ndnsf-di-automatic-facts-v1"},
    {"sourceDigest", sourceDigest}, {"operators", operations},
    {"quantization", encodedQuantization}});
}

void
AutomaticOnnxContract::validateSourceFacts(const NativeAssemblyControl& control) const
{
  requireActive(control);
  if (!isDigest(sourceDigest) || !isDigest(factsDigest))
    rejectShape("AUTOMATIC_CONTRACT_IDENTITY");
  std::uint64_t charged = 0;
  const auto limit = metadataLimit(control);
  const auto charge = [&](std::uint64_t bytes) {
    requireActive(control);
    if (bytes > limit - charged) rejectShape("AUTOMATIC_CONTRACT_SIZE");
    charged += bytes;
  };
  std::set<std::pair<std::string, std::uint64_t>> operatorIds;
  for (const auto& op : operators) {
    charge(sizeof(op));
    requireName(op.scope);
    requireName(op.opType);
    if (op.opset <= 0 || !operatorIds.emplace(op.scope, op.nodeIndex).second)
      rejectShape("AUTOMATIC_CONTRACT_OPERATOR");
    for (const auto& name : op.inputs) charge(sizeof(std::string) + name.size());
    for (const auto& name : op.outputs) charge(sizeof(std::string) + name.size());
    charge(op.domain.size() + op.overload.size() + op.functionScope.size());
    for (const auto& [name, value] : op.integerAttributes) {
      requireName(name); charge(name.size() + sizeof(value) + 64);
    }
    for (const auto& [name, values] : op.integerListAttributes) {
      requireName(name); charge(name.size() + 64);
      for (const auto value : values) charge(sizeof(value));
    }
    for (const auto& [name, reference] : op.attributeReferences) {
      requireName(name); requireName(reference); charge(name.size() + reference.size() + 64);
    }
  }
  std::set<std::uint64_t> quantized;
  for (const auto& quant : quantization) {
    charge(sizeof(quant));
    if (quant.operatorIndex >= operators.size() ||
        !quantized.insert(quant.operatorIndex).second ||
        (quant.bits && (*quant.bits < 2 || *quant.bits > 8)) ||
        (quant.blockSize && *quant.blockSize <= 0))
      rejectShape("AUTOMATIC_CONTRACT_QUANTIZATION");
    std::set<std::uint64_t> inputs;
    for (const auto& value : quant.constantInputs) {
      charge(sizeof(value) + value.valueName.size());
      const auto& op = operators[quant.operatorIndex];
      if (value.inputIndex >= op.inputs.size() ||
          op.inputs[value.inputIndex] != value.valueName ||
          !inputs.insert(value.inputIndex).second || value.onnxElementType <= 0 ||
          !isDigest(value.normalizedContentDigest))
        rejectShape("AUTOMATIC_CONTRACT_CONSTANT");
      for (const auto extent : value.shape) {
        charge(sizeof(extent));
        if (extent < 0) rejectShape("AUTOMATIC_CONTRACT_CONSTANT_SHAPE");
      }
    }
  }
  std::map<std::string, const AutomaticTensorContract*> names;
  std::string previous;
  for (const auto& tensor : tensors) {
    charge(sizeof(tensor) + tensor.name.size());
    requireName(tensor.name);
    if ((!previous.empty() && previous >= tensor.name) || tensor.onnxElementType < 0 ||
        tensor.wireElementType != onnxTensorWireType(tensor.onnxElementType))
      rejectShape("AUTOMATIC_CONTRACT_TENSOR");
    previous = tensor.name;
    names.emplace(tensor.name, &tensor);
    if (tensor.provenance.empty()) rejectShape("AUTOMATIC_CONTRACT_PROVENANCE");
    for (const auto& provenance : tensor.provenance) {
      requireName(provenance);
      charge(provenance.size());
    }
    if (!tensor.dimensions) continue;
    for (const auto& axis : *tensor.dimensions) {
      charge(sizeof(axis) + axis.symbol.size());
      if (axis.kind == DimensionConstraint::Kind::FIXED) {
        if (axis.fixed < 0 || !axis.symbol.empty() || !axis.upperBound ||
            *axis.upperBound != static_cast<std::uint64_t>(axis.fixed))
          rejectShape("AUTOMATIC_CONTRACT_FIXED_DIMENSION");
      }
      else if (axis.kind == DimensionConstraint::Kind::SYMBOL) {
        requireName(axis.symbol);
        if (axis.fixed != 0) rejectShape("AUTOMATIC_CONTRACT_SYMBOL");
      }
      else if (axis.kind != DimensionConstraint::Kind::UNKNOWN || axis.fixed != 0 ||
               !axis.symbol.empty()) rejectShape("AUTOMATIC_CONTRACT_UNKNOWN_DIMENSION");
      if (axis.upperBound && *axis.upperBound > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
        rejectShape("AUTOMATIC_CONTRACT_DIMENSION_OVERFLOW");
    }
  }
  const auto requirePorts = [&](const auto& ports) {
    std::set<std::string> seen;
    for (const auto& name : ports) {
      charge(name.size());
      if (!names.count(name) || !seen.insert(name).second)
        rejectShape("AUTOMATIC_CONTRACT_PORT");
    }
  };
  requirePorts(modelInputs);
  requirePorts(modelOutputs);
  for (const auto& op : operators) {
    if (op.scope != "graph" || (!op.domain.empty() && op.domain != "ai.onnx") ||
        op.opType != "Cast" || op.opset < 6 || op.atomicBody || !op.functionScope.empty() ||
        !op.overload.empty() || op.attributeReferences.count("to") || op.outputs.size() != 1)
      continue;
    const auto to = op.integerAttributes.find("to");
    const auto output = names.find("graph/" + op.outputs[0]);
    if (to != op.integerAttributes.end() && output != names.end() &&
        output->second->onnxElementType != 0 &&
        to->second != output->second->onnxElementType)
      rejectShape("AUTOMATIC_CONTRACT_CAST_TYPE_CONFLICT");
  }
  for (const auto& [name, values] : integerLiterals) {
    charge(96 + name.size());
    const auto found = names.find(name);
    if (found == names.end() || name.compare(0, 6, "graph/") != 0 || values.size() > 64)
      rejectShape("AUTOMATIC_CONTRACT_LITERAL");
    const auto& tensor = *found->second;
    if ((tensor.onnxElementType != 6 && tensor.onnxElementType != 7) || !tensor.dimensions ||
        (tensor.provenance != std::vector<std::string>{"initializer-header-v1"} &&
         tensor.provenance != std::vector<std::string>{"constant-header-v1"}))
      rejectShape("AUTOMATIC_CONTRACT_LITERAL_SOURCE");
    bool zero = false;
    for (const auto& axis : *tensor.dimensions) {
      if (axis.kind != DimensionConstraint::Kind::FIXED)
        rejectShape("AUTOMATIC_CONTRACT_LITERAL_SHAPE");
      zero = zero || axis.fixed == 0;
    }
    std::uint64_t count = zero ? 0 : 1;
    if (!zero) for (const auto& axis : *tensor.dimensions) {
      const auto extent = static_cast<std::uint64_t>(axis.fixed);
      if (extent > 64 / count) rejectShape("AUTOMATIC_CONTRACT_LITERAL_COUNT");
      count *= extent;
    }
    if (count != values.size()) rejectShape("AUTOMATIC_CONTRACT_LITERAL_COUNT");
    for (const auto value : values) {
      charge(sizeof(value));
      if (tensor.onnxElementType == 6 &&
          (value < std::numeric_limits<std::int32_t>::min() ||
           value > std::numeric_limits<std::int32_t>::max()))
        rejectShape("AUTOMATIC_CONTRACT_LITERAL_INT32");
    }
  }
  for (const auto& relation : relations) {
    charge(sizeof(relation) + relation.provenance.size());
    requireName(relation.provenance);
    const auto requireAxes = [&](const auto& axes) {
      for (const auto& axis : axes) {
        charge(sizeof(axis) + axis.tensor.size());
        const auto found = names.find(axis.tensor);
        if (found == names.end() || !found->second->dimensions ||
            axis.axis >= found->second->dimensions->size())
          rejectShape("AUTOMATIC_CONTRACT_AXIS_REFERENCE");
      }
    };
    requireAxes(relation.inputs);
    requireAxes(relation.outputs);
    const auto outputs = relation.outputs.size(), inputs = relation.inputs.size();
    switch (relation.kind) {
      case ShapeRelation::Kind::EQUAL:
        if (outputs != 1 || inputs > 1 || (inputs == 1 && relation.constant != 0))
          rejectShape("AUTOMATIC_CONTRACT_EQUAL");
        break;
      case ShapeRelation::Kind::SUM:
        if (outputs != 1) rejectShape("AUTOMATIC_CONTRACT_SUM");
        break;
      case ShapeRelation::Kind::PRODUCT:
        if (relation.constant != 0) rejectShape("AUTOMATIC_CONTRACT_PRODUCT");
        break;
      case ShapeRelation::Kind::BROADCAST:
        if (outputs != 1 || inputs != 2 || relation.constant != 0)
          rejectShape("AUTOMATIC_CONTRACT_BROADCAST");
        break;
      case ShapeRelation::Kind::AT_MOST:
        if (outputs != 1 || inputs != 1 || relation.constant != 0)
          rejectShape("AUTOMATIC_CONTRACT_BOUND");
        break;
      default: rejectShape("AUTOMATIC_CONTRACT_RELATION_KIND");
    }
  }
  const auto facts = factsJson();
  if (facts.size() > limit || nativePlanningDigest(facts) != factsDigest)
    rejectShape("AUTOMATIC_CONTRACT_FACTS_DIGEST");
  const auto derived = deriveAutomaticOnnxShapeRelations(*this, control);
  if (relationsJson(derived) != relationsJson(relations))
    rejectShape("AUTOMATIC_CONTRACT_RELATION_PROOF");
  requireActive(control);
}

void
AutomaticOnnxContract::validate(const NativeAssemblyControl& control) const
{
  validateSourceFacts(control);
  if (!generation) return;
  const auto limit = metadataLimit(control);
  std::uint64_t bytes = 0;
  const auto charge = [&](std::uint64_t size) {
    requireActive(control);
    if (size > limit - bytes) rejectShape("AUTOMATIC_GENERATION_SIZE");
    bytes += size;
  };
  charge(sizeof(*generation));
  for (const auto* name : {&generation->tokenInput, &generation->logitsOutput,
                           &generation->maskInput, &generation->positionInput,
                           &generation->ruleId}) charge(name->size());
  for (const auto& state : generation->states) {
    charge(sizeof(state));
    for (const auto* name : {&state.input, &state.output, &state.query, &state.key,
                             &state.value, &state.sequenceLengths, &state.totalLength})
      charge(name->size());
  }
  const auto proven = deriveAutomaticOnnxGeneration(*this, control);
  if (!proven || automaticOnnxGenerationJson(proven) != automaticOnnxGenerationJson(generation))
    rejectShape("AUTOMATIC_GENERATION_PROOF");
}

std::string
AutomaticOnnxContract::canonicalJson(const NativeAssemblyControl& control) const
{
  validate(control);
  auto encoded = nativeCanonicalJson(contractJson(*this));
  if (encoded.size() > metadataLimit(control)) rejectShape("AUTOMATIC_CONTRACT_SIZE");
  return encoded;
}

std::string
AutomaticOnnxContract::digest(const NativeAssemblyControl& control) const
{
  return nativePlanningDigest(canonicalJson(control));
}

AutomaticOnnxContract
AutomaticOnnxContract::fromCanonicalJson(const std::string& encoded,
                                         const NativeAssemblyControl& control)
{
  if (encoded.size() > metadataLimit(control)) rejectShape("AUTOMATIC_CONTRACT_SIZE");
  const auto value = nativeParseJson(encoded);
  if (!value.is_object() ||
      value.at("schema") != NativeJson("ndnsf-di-automatic-onnx-contract-v2") ||
      (!value.at("generation").is_null() && !value.at("generation").is_object()) ||
      nativeCanonicalJson(value) != encoded)
    rejectShape("AUTOMATIC_CONTRACT_SCHEMA");
  AutomaticOnnxContract result;
  const auto& facts = value.at("facts");
  if (facts.at("schema") != NativeJson("ndnsf-di-automatic-facts-v1"))
    rejectShape("AUTOMATIC_CONTRACT_FACTS_SCHEMA");
  result.sourceDigest = facts.at("sourceDigest").get<std::string>();
  result.factsDigest = value.at("factsDigest").get<std::string>();
  for (const auto& op : facts.at("operators")) {
    requireActive(control);
    AutomaticOnnxOperatorFact item;
    item.scope = op.at("scope").get<std::string>();
    item.nodeIndex = unsignedInteger(op.at("index"));
    item.domain = op.at("domain").get<std::string>();
    item.opType = op.at("op").get<std::string>();
    item.overload = op.at("overload").get<std::string>();
    item.functionScope = op.at("functionScope").get<std::string>();
    item.opset = signedInteger(op.at("opset"));
    item.inputs = stringArray(op.at("inputs"));
    item.outputs = stringArray(op.at("outputs"));
    if (!op.at("integers").is_object() || !op.at("integerLists").is_object() ||
        !op.at("attributeReferences").is_object() || !op.at("atomicBody").is_boolean())
      rejectShape("AUTOMATIC_CONTRACT_OPERATOR_FIELDS");
    for (const auto& attr : op.at("integers").items())
      item.integerAttributes[attr.key()] = signedInteger(attr.value());
    for (const auto& attr : op.at("integerLists").items()) {
      if (!attr.value().is_array()) rejectShape("AUTOMATIC_CONTRACT_ARRAY");
      auto& list = item.integerListAttributes[attr.key()];
      for (const auto& integer : attr.value()) list.push_back(signedInteger(integer));
    }
    for (const auto& attr : op.at("attributeReferences").items())
      item.attributeReferences[attr.key()] = attr.value().get<std::string>();
    item.atomicBody = op.at("atomicBody").get<bool>();
    result.operators.push_back(std::move(item));
  }
  for (const auto& quant : facts.at("quantization")) {
    requireActive(control);
    QuantizedOperatorFact item;
    item.operatorIndex = unsignedInteger(quant.at("operator"));
    const auto optionalInteger = [&](const char* name) -> std::optional<std::int64_t> {
      return quant.at(name).is_null() ? std::nullopt :
        std::optional<std::int64_t>(signedInteger(quant.at(name)));
    };
    item.bits = optionalInteger("bits"); item.axis = optionalInteger("axis");
    item.blockSize = optionalInteger("blockSize");
    item.implicitZeroPoint = optionalInteger("implicitZeroPoint");
    if (!quant.at("staticParametersValidated").is_boolean())
      rejectShape("AUTOMATIC_CONTRACT_BOOLEAN");
    item.staticParametersValidated = quant.at("staticParametersValidated").get<bool>();
    for (const auto& constant : quant.at("constants")) {
      AutomaticOnnxConstantFact entry;
      entry.inputIndex = unsignedInteger(constant.at("input"));
      entry.valueName = constant.at("name").get<std::string>();
      const auto scalar = signedInteger(constant.at("type"));
      if (scalar < 0 || scalar > std::numeric_limits<std::int32_t>::max())
        rejectShape("AUTOMATIC_CONTRACT_SCALAR_TYPE");
      entry.onnxElementType = static_cast<std::int32_t>(scalar);
      if (!constant.at("shape").is_array()) rejectShape("AUTOMATIC_CONTRACT_ARRAY");
      for (const auto& dim : constant.at("shape")) entry.shape.push_back(signedInteger(dim));
      entry.storageBytes = unsignedInteger(constant.at("storageBytes"));
      entry.normalizedContentDigest = constant.at("normalizedDigest").get<std::string>();
      item.constantInputs.push_back(std::move(entry));
    }
    result.quantization.push_back(std::move(item));
  }
  for (const auto& tensor : value.at("tensors")) {
    requireActive(control);
    AutomaticTensorContract item;
    item.name = tensor.at("name").get<std::string>();
    const auto scalar = signedInteger(tensor.at("onnxType"));
    if (scalar < 0 || scalar > std::numeric_limits<std::int32_t>::max())
      rejectShape("AUTOMATIC_CONTRACT_SCALAR_TYPE");
    item.onnxElementType = static_cast<std::int32_t>(scalar);
    if (!tensor.at("wireType").is_null())
      item.wireElementType = tensorElementTypeFromName(tensor.at("wireType").get<std::string>());
    item.provenance = stringArray(tensor.at("provenance"));
    if (!tensor.at("dimensions").is_null()) {
      if (!tensor.at("dimensions").is_array()) rejectShape("AUTOMATIC_CONTRACT_ARRAY");
      item.dimensions.emplace();
      for (const auto& axis : tensor.at("dimensions")) {
        DimensionConstraint dim;
        const auto kind = axis.at("kind").get<std::string>();
        if (kind == "fixed") dim.kind = DimensionConstraint::Kind::FIXED;
        else if (kind == "symbol") dim.kind = DimensionConstraint::Kind::SYMBOL;
        else if (kind != "unknown") rejectShape("AUTOMATIC_CONTRACT_DIMENSION_KIND");
        dim.fixed = signedInteger(axis.at("fixed"));
        dim.symbol = axis.at("symbol").get<std::string>();
        if (!axis.at("upper").is_null()) dim.upperBound = unsignedInteger(axis.at("upper"));
        item.dimensions->push_back(std::move(dim));
      }
    }
    result.tensors.push_back(std::move(item));
  }
  result.modelInputs = stringArray(value.at("modelInputs"));
  result.modelOutputs = stringArray(value.at("modelOutputs"));
  if (!value.at("integerLiterals").is_object()) rejectShape("AUTOMATIC_CONTRACT_LITERALS");
  for (const auto& item : value.at("integerLiterals").items()) {
    requireActive(control);
    if (!item.value().is_array() || item.value().size() > 64)
      rejectShape("AUTOMATIC_CONTRACT_LITERAL");
    auto& values = result.integerLiterals[item.key()];
    for (const auto& element : item.value()) values.push_back(signedInteger(element));
  }
  for (const auto& relation : value.at("relations")) {
    requireActive(control);
    ShapeRelation item;
    const auto kind = relation.at("kind").get<std::string>();
    if (kind == "equal") item.kind = ShapeRelation::Kind::EQUAL;
    else if (kind == "sum") item.kind = ShapeRelation::Kind::SUM;
    else if (kind == "product") item.kind = ShapeRelation::Kind::PRODUCT;
    else if (kind == "broadcast") item.kind = ShapeRelation::Kind::BROADCAST;
    else if (kind == "at_most") item.kind = ShapeRelation::Kind::AT_MOST;
    else rejectShape("AUTOMATIC_CONTRACT_RELATION_KIND");
    const auto axes = [&](const NativeJson& array) {
      if (!array.is_array()) rejectShape("AUTOMATIC_CONTRACT_ARRAY");
      std::vector<TensorAxisReference> references;
      for (const auto& axis : array)
        references.push_back({axis.at("tensor").get<std::string>(), unsignedInteger(axis.at("axis"))});
      return references;
    };
    item.outputs = axes(relation.at("outputs"));
    item.inputs = axes(relation.at("inputs"));
    item.constant = unsignedInteger(relation.at("constant"));
    item.provenance = relation.at("provenance").get<std::string>();
    result.relations.push_back(std::move(item));
  }
  if (!value.at("generation").is_null()) {
    // Reconstruct from source facts rather than accepting an asserted role map.
    result.generation = deriveAutomaticOnnxGeneration(result, control);
    if (!result.generation) rejectShape("AUTOMATIC_GENERATION_UNPROVEN");
  }
  // Exact reconstruction also rejects unknown nested fields and lossy JSON coercions.
  if (result.canonicalJson(control) != encoded) rejectShape("AUTOMATIC_CONTRACT_FIELDS");
  return result;
}

std::vector<ShapeRelation>
deriveAutomaticOnnxShapeRelations(const AutomaticOnnxContract& contract,
                                  const NativeAssemblyControl& control)
{
  const auto limit = metadataLimit(control);
  std::map<std::string, const AutomaticTensorContract*> tensors;
  for (const auto& tensor : contract.tensors) {
    requireActive(control);
    if (!tensors.emplace(tensor.name, &tensor).second)
      rejectShape("AUTOMATIC_CONTRACT_TENSOR_DUPLICATE");
  }
  std::vector<ShapeRelation> result;
  std::uint64_t bytes = 0;
  for (const auto& op : contract.operators) {
    requireActive(control);
    // Local functions and control-flow bodies retain their original atomic boundary.
    if (op.scope != "graph" || op.atomicBody || !op.functionScope.empty() ||
        !op.overload.empty() || op.outputs.empty()) continue;
    const bool standard = op.domain.empty() || op.domain == "ai.onnx";
    const bool nBits = op.domain == "com.microsoft" && op.opType == "MatMulNBits" &&
                       op.opset == 1;
    const bool rotary = op.domain == "com.microsoft" && op.opType == "RotaryEmbedding" &&
                        op.opset == 1;
    const bool attention = op.domain == "com.microsoft" && op.opType == "GroupQueryAttention" &&
                           op.opset == 1;
    const bool skipNorm = op.domain == "com.microsoft" &&
                          op.opType == "SkipSimplifiedLayerNormalization" && op.opset == 1;
    if (!standard && !nBits && !rotary && !attention && !skipNorm) continue;
    const auto name = [](const std::string& value) { return "graph/" + value; };
    const auto rank = [&](const std::string& value) -> std::optional<std::uint64_t> {
      const auto found = tensors.find(name(value));
      if (found == tensors.end() || !found->second->dimensions) return std::nullopt;
      return found->second->dimensions->size();
    };
    const auto reference = [&](const std::string& value, std::uint64_t axis) {
      return TensorAxisReference{name(value), axis};
    };
    const auto input = [&](std::size_t port, std::uint64_t axis) {
      return reference(op.inputs.at(port), axis);
    };
    const auto output = [&](std::uint64_t axis) { return reference(op.outputs.front(), axis); };
    const auto attr = [&](const char* key, std::int64_t fallback) {
      if (op.attributeReferences.count(key))
        rejectShape("AUTOMATIC_SHAPE_ATTRIBUTE_UNRESOLVED");
      const auto found = op.integerAttributes.find(key);
      return found == op.integerAttributes.end() ? fallback : found->second;
    };
    const auto normalizedAxis = [&](std::int64_t axis, std::uint64_t count, bool end = false) {
      if (count > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
        rejectShape("AUTOMATIC_SHAPE_RANK");
      if (axis < 0) axis += static_cast<std::int64_t>(count);
      if (axis < 0 || std::uint64_t(axis) > count || (!end && std::uint64_t(axis) == count))
        rejectShape("AUTOMATIC_SHAPE_AXIS");
      return static_cast<std::uint64_t>(axis);
    };
    const auto add = [&](ShapeRelation::Kind kind, std::vector<TensorAxisReference> outputs,
                         std::vector<TensorAxisReference> inputs, std::uint64_t constant = 0) {
      if (kind == ShapeRelation::Kind::EQUAL && outputs.size() == 1 && inputs.size() == 1 &&
          outputs[0].tensor == inputs[0].tensor && outputs[0].axis == inputs[0].axis)
        return;
      requireActive(control);
      ShapeRelation relation{kind, std::move(outputs), std::move(inputs), constant,
        op.scope + "/node/" + std::to_string(op.nodeIndex) + "/shape-v1"};
      auto charge = sizeof(relation) + relation.provenance.size();
      for (const auto& axis : relation.inputs) charge += sizeof(axis) + axis.tensor.size();
      for (const auto& axis : relation.outputs) charge += sizeof(axis) + axis.tensor.size();
      if (bytes > limit || charge > limit - bytes) rejectShape("AUTOMATIC_RELATION_SIZE");
      bytes += charge;
      result.push_back(std::move(relation));
    };
    const auto equal = [&](TensorAxisReference left, TensorAxisReference right) {
      add(ShapeRelation::Kind::EQUAL, {std::move(left)}, {std::move(right)});
    };
    const auto outputRank = rank(op.outputs.front());
    if (!outputRank || op.inputs.empty()) continue;
    const auto inputRank = rank(op.inputs.front());
    if (!inputRank) continue;
    const auto unary = [&] {
      if (*inputRank != *outputRank) rejectShape("AUTOMATIC_SHAPE_UNARY_RANK");
      for (std::uint64_t axis = 0; axis < *inputRank; ++axis) equal(output(axis), input(0, axis));
    };
    if (standard && (op.opType == "Identity" || op.opType == "Cast" ||
        op.opType == "Relu" || op.opType == "Sigmoid" || op.opType == "Tanh" ||
        op.opType == "Softmax" || op.opType == "LogSoftmax" || op.opType == "Sqrt" ||
        op.opType == "Exp" || op.opType == "Log" || op.opType == "Abs" ||
        op.opType == "Neg" || op.opType == "Erf" || op.opType == "Sin" ||
        op.opType == "Cos" || op.opType == "Floor" || op.opType == "Ceil" ||
        op.opType == "Clip" || op.opType == "QuantizeLinear" ||
        op.opType == "DequantizeLinear" || op.opType == "DynamicQuantizeLinear" ||
        op.opType == "LayerNormalization" || op.opType == "SimplifiedLayerNormalization" ||
        op.opType == "Dropout")) {
      unary();
    }
    else if (rotary || skipNorm) {
      unary();
      if (rotary && op.inputs.size() >= 2 && rank(op.inputs[1]) == 2 &&
          !op.attributeReferences.count("is_packed_batching") &&
          attr("is_packed_batching", 0) == 0 && (*inputRank == 3 || *inputRank == 4)) {
        equal(input(1, 0), input(0, 0));
        equal(input(1, 1), input(0, *inputRank == 3 ? 1 : 2));
      }
      if (skipNorm) {
        if (op.outputs.size() > 3 && !op.outputs[3].empty() && rank(op.outputs[3]) == inputRank)
          for (std::uint64_t axis = 0; axis < *inputRank; ++axis)
            equal(reference(op.outputs[3], axis), input(0, axis));
      }
    }
    else if (attention) {
      // The query shape is the output shape only for unpacked Q/K/V.
      if (op.inputs.size() < 7 || op.inputs[1].empty() || op.inputs[2].empty() ||
          *inputRank != 3 || *outputRank != 3) continue;
      unary();
      for (std::size_t role = 0; role < 2; ++role) {
        if (op.inputs[3 + role].empty() || op.outputs.size() <= 1 + role ||
            op.outputs[1 + role].empty() || rank(op.inputs[3 + role]) != 4 ||
            rank(op.outputs[1 + role]) != 4) continue;
        for (const auto axis : {0U, 1U, 3U})
          equal(reference(op.outputs[1 + role], axis), input(3 + role, axis));
      }
      const auto lengthsRank = rank(op.inputs[5]);
      if (lengthsRank == 1) equal(input(5, 0), input(0, 0));
      else if (lengthsRank == 2) {
        add(ShapeRelation::Kind::PRODUCT, {input(5, 0), input(5, 1)}, {input(0, 0)});
        add(ShapeRelation::Kind::BROADCAST, {input(0, 0)}, {input(5, 0), input(5, 1)});
      }
      const auto totalRank = rank(op.inputs[6]);
      if (totalRank && *totalRank != 0) {
        std::vector<TensorAxisReference> dimensions;
        for (std::uint64_t axis = 0; axis < *totalRank; ++axis)
          dimensions.push_back(input(6, axis));
        add(ShapeRelation::Kind::PRODUCT, std::move(dimensions), {});
      }
    }
    else if (standard && op.opType == "Transpose") {
      if (op.attributeReferences.count("perm")) continue;
      if (*inputRank != *outputRank) rejectShape("AUTOMATIC_SHAPE_TRANSPOSE_RANK");
      auto permutation = op.integerListAttributes.find("perm");
      std::set<std::uint64_t> seen;
      if (permutation != op.integerListAttributes.end() && permutation->second.size() != *inputRank)
        rejectShape("AUTOMATIC_SHAPE_TRANSPOSE_PERMUTATION");
      for (std::uint64_t axis = 0; axis < *inputRank; ++axis) {
        if (permutation != op.integerListAttributes.end() && permutation->second[axis] < 0)
          rejectShape("AUTOMATIC_SHAPE_TRANSPOSE_PERMUTATION");
        const auto from = permutation == op.integerListAttributes.end() ? *inputRank - axis - 1 :
          normalizedAxis(permutation->second[axis], *inputRank);
        if (!seen.insert(from).second) rejectShape("AUTOMATIC_SHAPE_TRANSPOSE_PERMUTATION");
        equal(output(axis), input(0, from));
      }
    }
    else if (standard && op.opType == "Concat") {
      if (op.attributeReferences.count("axis")) continue;
      if (!op.integerAttributes.count("axis")) rejectShape("AUTOMATIC_SHAPE_CONCAT_AXIS");
      const auto axis = normalizedAxis(attr("axis", 0), *inputRank);
      if (*outputRank != *inputRank) rejectShape("AUTOMATIC_SHAPE_CONCAT_RANK");
      bool known = true;
      for (const auto& value : op.inputs) {
        const auto other = rank(value);
        if (other && other != inputRank) rejectShape("AUTOMATIC_SHAPE_CONCAT_RANK");
        known = known && other.has_value();
      }
      if (!known) continue;
      std::vector<TensorAxisReference> joined;
      for (std::size_t port = 0; port < op.inputs.size(); ++port) {
        joined.push_back(input(port, axis));
        for (std::uint64_t other = 0; other < *inputRank; ++other)
          if (other != axis) equal(output(other), input(port, other));
      }
      add(ShapeRelation::Kind::SUM, {output(axis)}, std::move(joined));
    }
    else if (standard && (op.opType == "Reshape" || op.opType == "Squeeze" ||
                          op.opType == "Unsqueeze")) {
      std::vector<TensorAxisReference> left, right;
      for (std::uint64_t axis = 0; axis < *outputRank; ++axis) left.push_back(output(axis));
      for (std::uint64_t axis = 0; axis < *inputRank; ++axis) right.push_back(input(0, axis));
      add(ShapeRelation::Kind::PRODUCT, std::move(left), std::move(right));
    }
    else if (standard && op.opType == "Flatten") {
      if (op.attributeReferences.count("axis")) continue;
      if (*outputRank != 2) rejectShape("AUTOMATIC_SHAPE_FLATTEN_RANK");
      const auto axis = normalizedAxis(attr("axis", 1), *inputRank, true);
      std::vector<TensorAxisReference> first, last;
      for (std::uint64_t i = 0; i < *inputRank; ++i)
        (i < axis ? first : last).push_back(input(0, i));
      add(ShapeRelation::Kind::PRODUCT, {output(0)}, std::move(first));
      add(ShapeRelation::Kind::PRODUCT, {output(1)}, std::move(last));
    }
    else if (standard && op.opType == "Slice") {
      if (*inputRank != *outputRank) rejectShape("AUTOMATIC_SHAPE_SLICE_RANK");
      for (std::uint64_t axis = 0; axis < *inputRank; ++axis)
        add(ShapeRelation::Kind::AT_MOST, {output(axis)}, {input(0, axis)});
    }
    else if (standard && op.opType == "Gather" && op.inputs.size() >= 2) {
      if (op.attributeReferences.count("axis")) continue;
      const auto indices = rank(op.inputs[1]);
      if (!indices) continue;
      const auto axis = normalizedAxis(attr("axis", 0), *inputRank);
      if (*outputRank != *inputRank - 1 + *indices) rejectShape("AUTOMATIC_SHAPE_GATHER_RANK");
      for (std::uint64_t i = 0; i < axis; ++i) equal(output(i), input(0, i));
      for (std::uint64_t i = 0; i < *indices; ++i) equal(output(axis + i), input(1, i));
      for (std::uint64_t i = axis + 1; i < *inputRank; ++i)
        equal(output(i - 1 + *indices), input(0, i));
    }
    else if (nBits) {
      if (op.attributeReferences.count("K") || op.attributeReferences.count("N")) continue;
      if (*inputRank < 2 || *outputRank != *inputRank || attr("K", 0) <= 0 || attr("N", 0) <= 0)
        rejectShape("AUTOMATIC_SHAPE_NBITS");
      add(ShapeRelation::Kind::EQUAL, {input(0, *inputRank - 1)}, {}, attr("K", 0));
      add(ShapeRelation::Kind::EQUAL, {output(*outputRank - 1)}, {}, attr("N", 0));
      for (std::uint64_t i = 0; i + 1 < *inputRank; ++i) equal(output(i), input(0, i));
    }
    else if (standard && (op.opType == "MatMul" || op.opType == "MatMulInteger" ||
                          op.opType == "QLinearMatMul")) {
      const std::size_t b = op.opType == "QLinearMatMul" ? 3 : 1;
      if (op.inputs.size() <= b) rejectShape("AUTOMATIC_SHAPE_MATMUL_INPUTS");
      const auto bRank = rank(op.inputs[b]);
      if (!bRank) continue;
      if (*inputRank == 0 || *bRank == 0) rejectShape("AUTOMATIC_SHAPE_MATMUL_RANK");
      const auto aBatch = *inputRank > 2 ? *inputRank - 2 : 0;
      const auto bBatch = *bRank > 2 ? *bRank - 2 : 0;
      const auto batch = std::max(aBatch, bBatch);
      if (*outputRank != batch + (*inputRank > 1) + (*bRank > 1))
        rejectShape("AUTOMATIC_SHAPE_MATMUL_OUTPUT_RANK");
      equal(input(0, *inputRank - 1), input(b, *bRank > 1 ? *bRank - 2 : 0));
      for (std::uint64_t axis = 0; axis < batch; ++axis) {
        const bool aPresent = axis >= batch - aBatch, bPresent = axis >= batch - bBatch;
        if (aPresent && bPresent)
          add(ShapeRelation::Kind::BROADCAST, {output(axis)},
              {input(0, axis - (batch - aBatch)), input(b, axis - (batch - bBatch))});
        else equal(output(axis), aPresent ? input(0, axis - (batch - aBatch)) :
                                           input(b, axis - (batch - bBatch)));
      }
      if (*inputRank > 1) equal(output(batch), input(0, *inputRank - 2));
      if (*bRank > 1) equal(output(batch + (*inputRank > 1)), input(b, *bRank - 1));
    }
    else if (standard && op.opType == "Gemm" && op.inputs.size() >= 2) {
      if (op.attributeReferences.count("transA") || op.attributeReferences.count("transB")) continue;
      const auto bRank = rank(op.inputs[1]);
      if (!bRank) continue;
      const auto ta = attr("transA", 0), tb = attr("transB", 0);
      if (*inputRank != 2 || *bRank != 2 || *outputRank != 2 ||
          (ta != 0 && ta != 1) || (tb != 0 && tb != 1)) rejectShape("AUTOMATIC_SHAPE_GEMM");
      equal(output(0), input(0, ta == 0 ? 0 : 1));
      equal(output(1), input(1, tb == 0 ? 1 : 0));
      equal(input(0, ta == 0 ? 1 : 0), input(1, tb == 0 ? 0 : 1));
      if (op.inputs.size() >= 3 && !op.inputs[2].empty()) {
        const auto cRank = rank(op.inputs[2]);
        if (!cRank) continue;
        if (*cRank > 2) rejectShape("AUTOMATIC_SHAPE_GEMM_BIAS");
        for (std::uint64_t axis = 0; axis < *cRank; ++axis)
          add(ShapeRelation::Kind::BROADCAST, {output(2 - *cRank + axis)},
              {output(2 - *cRank + axis), input(2, axis)});
      }
    }
    else if (standard && op.opset >= 7 && op.inputs.size() == 2 &&
        (op.opType == "Add" || op.opType == "Sub" || op.opType == "Mul" ||
         op.opType == "Div" || op.opType == "Pow")) {
      const auto bRank = rank(op.inputs[1]);
      if (!bRank) continue;
      const auto count = std::max(*inputRank, *bRank);
      if (*outputRank != count) rejectShape("AUTOMATIC_SHAPE_BROADCAST_RANK");
      for (std::uint64_t axis = 0; axis < count; ++axis) {
        const bool aPresent = axis >= count - *inputRank, bPresent = axis >= count - *bRank;
        if (aPresent && bPresent)
          add(ShapeRelation::Kind::BROADCAST, {output(axis)},
              {input(0, axis - (count - *inputRank)), input(1, axis - (count - *bRank))});
        else equal(output(axis), aPresent ? input(0, axis - (count - *inputRank)) :
                                           input(1, axis - (count - *bRank)));
      }
    }
  }
  return result;
}

namespace {

struct Extent
{
  std::uint64_t lower = 0;
  std::optional<std::uint64_t> upper;
};

std::uint64_t
addExtent(std::uint64_t left, std::uint64_t right)
{
  if (right > std::numeric_limits<std::uint64_t>::max() - left)
    rejectShape("AUTOMATIC_SHAPE_OVERFLOW");
  return left + right;
}

std::uint64_t
multiplyExtent(std::uint64_t left, std::uint64_t right)
{
  if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right)
    rejectShape("AUTOMATIC_SHAPE_OVERFLOW");
  return left * right;
}

using ShapeObservations = std::map<std::string, std::vector<Extent>>;

ShapeObservations
exactObservations(const std::map<std::string, std::vector<std::int64_t>>& shapes,
                   const NativeAssemblyControl& control)
{
  ShapeObservations result;
  std::uint64_t bytes = 0;
  const auto limit = metadataLimit(control);
  for (const auto& [name, dimensions] : shapes) {
    requireActive(control);
    bytes = addExtent(bytes, addExtent(name.size(),
      multiplyExtent(dimensions.size(), sizeof(Extent) + 64)));
    if (bytes > limit) rejectShape("AUTOMATIC_SHAPE_OBSERVATION_SIZE");
    auto& bounds = result[name];
    for (const auto dimension : dimensions) {
      if (dimension < 0) rejectShape("AUTOMATIC_SHAPE_INPUT_DIMENSION");
      const auto value = static_cast<std::uint64_t>(dimension);
      bounds.push_back({value, value});
    }
  }
  return result;
}

/** Monotone interval propagation, bounded by metadata, work count and owner control. */
class ShapeSolver
{
public:
  ShapeSolver(const AutomaticOnnxContract& contract,
              const std::map<std::string, std::vector<std::int64_t>>& observations,
              const NativeAssemblyControl& control)
    : ShapeSolver(contract, exactObservations(observations, control), control)
  {
  }

  ShapeSolver(const AutomaticOnnxContract& contract, const ShapeObservations& observations,
              const NativeAssemblyControl& control)
    : m_control(control)
  {
    std::map<std::string, std::size_t> symbols;
    for (const auto& tensor : contract.tensors) {
      requireActive(control);
      const auto observed = observations.find(tensor.name);
      if (!tensor.dimensions && observed == observations.end()) continue;
      const auto count = tensor.dimensions ? tensor.dimensions->size() : observed->second.size();
      if (observed != observations.end() && observed->second.size() != count)
        rejectShape("AUTOMATIC_SHAPE_INPUT_RANK");
      auto& axes = m_axes[tensor.name];
      for (std::size_t axis = 0; axis < count; ++axis) {
        requireActive(control);
        if (m_values.size() >= metadataLimit(control) / 256)
          rejectShape("AUTOMATIC_SHAPE_SOLVER_SIZE");
        const auto id = m_values.size();
        m_parent.push_back(id);
        Extent extent;
        if (tensor.dimensions) {
          const auto& dimension = tensor.dimensions->at(axis);
          extent.upper = dimension.upperBound;
          if (dimension.kind == DimensionConstraint::Kind::FIXED)
            extent.lower = static_cast<std::uint64_t>(dimension.fixed);
        }
        if (observed != observations.end()) {
          const auto& actual = observed->second[axis];
          if (!actual.upper || actual.lower > *actual.upper ||
              *actual.upper > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
            rejectShape("AUTOMATIC_SHAPE_INPUT_DIMENSION");
          if (actual.lower < extent.lower || (extent.upper && *actual.upper > *extent.upper))
            rejectShape("AUTOMATIC_SHAPE_INPUT_CONFLICT");
          extent = actual;
        }
        m_values.push_back(extent);
        axes.push_back(id);
        if (tensor.dimensions &&
            tensor.dimensions->at(axis).kind == DimensionConstraint::Kind::SYMBOL) {
          const auto [found, inserted] = symbols.emplace(tensor.dimensions->at(axis).symbol, id);
          if (!inserted) join(id, found->second);
        }
      }
    }
    const auto ids = [&](const auto& references) {
      std::vector<std::size_t> result;
      for (const auto& reference : references)
        result.push_back(m_axes.at(reference.tensor).at(reference.axis));
      return result;
    };
    for (const auto& relation : contract.relations) {
      requireActive(control);
      Rule rule{relation.kind, ids(relation.outputs), ids(relation.inputs), relation.constant};
      if (rule.kind == ShapeRelation::Kind::EQUAL) {
        if (rule.inputs.empty()) intersect(root(rule.outputs.front()), {rule.constant, rule.constant});
        else join(rule.outputs.front(), rule.inputs.front());
      }
      else m_rules.push_back(std::move(rule));
    }
    m_dependents.resize(m_values.size());
    m_queued.resize(m_rules.size(), false);
    for (auto& [name, axes] : m_axes)
      for (auto& axis : axes) axis = root(axis);
    for (std::size_t index = 0; index < m_rules.size(); ++index) {
      auto& rule = m_rules[index];
      for (auto& id : rule.outputs) {
        id = root(id);
        m_dependents[id].push_back(index);
      }
      for (auto& id : rule.inputs) {
        id = root(id);
        m_dependents[id].push_back(index);
      }
      enqueue(index);
    }
    const auto bound = multiplyExtent(addExtent(addExtent(m_values.size(), m_rules.size()), 1), 64);
    std::uint64_t references = 0;
    for (const auto& rule : m_rules)
      references = addExtent(references, addExtent(rule.inputs.size(), rule.outputs.size()));
    m_workLimit = multiplyExtent(addExtent(addExtent(m_values.size(), references), m_rules.size()) + 1, 128);
    std::uint64_t steps = 0;
    while (!m_pending.empty()) {
      requireActive(control);
      if (++steps > bound) rejectShape("AUTOMATIC_SHAPE_RELATION_BUDGET");
      const auto index = m_pending.front();
      m_pending.pop_front();
      m_queued[index] = false;
      apply(m_rules[index]);
    }
    // Required request ranges cannot silently shrink to a feasible subset.
    for (const auto& [name, bounds] : observations) {
      const auto found = m_axes.find(name);
      if (found == m_axes.end()) rejectShape("AUTOMATIC_SHAPE_OBSERVATION_REFERENCE");
      for (std::size_t axis = 0; axis < bounds.size(); ++axis) {
        const auto& solved = m_values[found->second.at(axis)];
        if (solved.lower != bounds[axis].lower || solved.upper != bounds[axis].upper)
          rejectShape("AUTOMATIC_SHAPE_REQUEST_DOMAIN_CONFLICT");
      }
    }
  }

  const std::vector<std::size_t>*
  axes(const std::string& name) const
  {
    const auto found = m_axes.find(name);
    return found == m_axes.end() ? nullptr : &found->second;
  }

  const Extent& value(std::size_t id) const { return m_values[id]; }

private:
  struct Rule
  {
    ShapeRelation::Kind kind;
    std::vector<std::size_t> outputs, inputs;
    std::uint64_t constant = 0;
  };

  std::size_t
  root(std::size_t id)
  {
    auto result = id;
    while (m_parent[result] != result) result = m_parent[result];
    while (id != result) {
      const auto next = m_parent[id];
      m_parent[id] = result;
      id = next;
    }
    return result;
  }

  void
  join(std::size_t left, std::size_t right)
  {
    left = root(left); right = root(right);
    if (left == right) return;
    if (left > right) std::swap(left, right);
    intersect(left, m_values[right]);
    m_parent[right] = left;
  }

  void
  enqueue(std::size_t rule)
  {
    if (!m_queued[rule]) {
      m_queued[rule] = true;
      m_pending.push_back(rule);
    }
  }

  void
  intersect(std::size_t id, Extent extent)
  {
    auto& current = m_values[id];
    extent.lower = std::max(extent.lower, current.lower);
    if (current.upper) extent.upper = extent.upper ?
      std::min(*current.upper, *extent.upper) : current.upper;
    if (extent.upper && extent.lower > *extent.upper)
      rejectShape("AUTOMATIC_SHAPE_CONFLICT");
    if (extent.lower == current.lower && extent.upper == current.upper) return;
    current = extent;
    if (!m_dependents.empty()) for (const auto rule : m_dependents[id]) enqueue(rule);
  }

  void
  work() const
  {
    if (++m_work > m_workLimit) rejectShape("AUTOMATIC_SHAPE_RELATION_BUDGET");
    if ((m_work & 63) == 0) requireActive(m_control);
  }

  Extent
  sum(const std::vector<std::size_t>& values, std::uint64_t constant = 0,
      std::optional<std::size_t> skipped = {}) const
  {
    Extent result{constant, constant};
    for (std::size_t position = 0; position < values.size(); ++position) {
      work();
      if (skipped == position) continue;
      const auto& extent = m_values[values[position]];
      result.lower = addExtent(result.lower, extent.lower);
      result.upper = result.upper && extent.upper ?
        std::optional<std::uint64_t>(addExtent(*result.upper, *extent.upper)) : std::nullopt;
    }
    return result;
  }

  Extent
  product(const std::vector<std::size_t>& values,
          std::optional<std::size_t> skipped = {}) const
  {
    Extent result{1, 1};
    // A known zero dominates unknown factors; it must not become an unknown byte cost.
    for (std::size_t position = 0; position < values.size(); ++position) {
      work();
      if (skipped != position && m_values[values[position]].upper == std::optional<std::uint64_t>(0))
        return {0, 0};
    }
    for (std::size_t position = 0; position < values.size(); ++position) {
      work();
      if (skipped == position) continue;
      const auto& extent = m_values[values[position]];
      result.lower = multiplyExtent(result.lower, extent.lower);
      result.upper = result.upper && extent.upper ?
        std::optional<std::uint64_t>(multiplyExtent(*result.upper, *extent.upper)) : std::nullopt;
    }
    return result;
  }

  void
  constrainProduct(const std::vector<std::size_t>& values, const Extent& target)
  {
    if (target.lower != 0)
      for (const auto value : values) intersect(value, {1, {}});
    const auto actual = product(values);
    if ((actual.upper && target.lower > *actual.upper) ||
        (target.upper && actual.lower > *target.upper)) rejectShape("AUTOMATIC_SHAPE_PRODUCT");
    for (std::size_t position = 0; position < values.size(); ++position) {
      const auto other = product(values, position);
      Extent bound;
      if (target.upper && other.lower != 0) bound.upper = *target.upper / other.lower;
      if (other.upper && *other.upper != 0)
        bound.lower = target.lower / *other.upper + (target.lower % *other.upper != 0);
      else if (other.upper && target.lower != 0) rejectShape("AUTOMATIC_SHAPE_PRODUCT");
      intersect(values[position], bound);
    }
  }

  void
  apply(const Rule& rule)
  {
    switch (rule.kind) {
      case ShapeRelation::Kind::SUM: {
        const auto output = rule.outputs.front();
        intersect(output, sum(rule.inputs, rule.constant));
        for (std::size_t position = 0; position < rule.inputs.size(); ++position) {
          const auto others = sum(rule.inputs, rule.constant, position);
          const auto target = m_values[output];
          Extent bound;
          if (target.upper) {
            if (*target.upper < others.lower) rejectShape("AUTOMATIC_SHAPE_SUM");
            bound.upper = *target.upper - others.lower;
          }
          if (others.upper && target.lower > *others.upper) bound.lower = target.lower - *others.upper;
          intersect(rule.inputs[position], bound);
        }
        break;
      }
      case ShapeRelation::Kind::PRODUCT:
        constrainProduct(rule.outputs, product(rule.inputs));
        constrainProduct(rule.inputs, product(rule.outputs));
        break;
      case ShapeRelation::Kind::AT_MOST:
        intersect(rule.outputs.front(), {0, m_values[rule.inputs.front()].upper});
        intersect(rule.inputs.front(), {m_values[rule.outputs.front()].lower, {}});
        break;
      case ShapeRelation::Kind::BROADCAST: {
        const auto left = rule.inputs[0], right = rule.inputs[1], output = rule.outputs[0];
        const auto a = m_values[left], b = m_values[right];
        const auto exact = [](const Extent& v) { return v.upper && *v.upper == v.lower; };
        const auto equal = [&](std::size_t first, std::size_t second) {
          intersect(first, m_values[second]); intersect(second, m_values[first]);
        };
        if (exact(a) && a.lower == 1) equal(output, right);
        else if (exact(b) && b.lower == 1) equal(output, left);
        else if (exact(a) && exact(b)) {
          if (a.lower != b.lower) rejectShape("AUTOMATIC_SHAPE_BROADCAST");
          intersect(output, a);
        }
        else if (a.lower >= 2 && b.lower >= 2) {
          equal(left, right); equal(output, left);
        }
        else {
          if (a.upper && b.upper) intersect(output, {0, std::max(*a.upper, *b.upper)});
          const auto target = m_values[output];
          if (target.upper) {
            const Extent bound{target.lower == 0 ? 0U : 1U, std::max<std::uint64_t>(1, *target.upper)};
            intersect(left, bound); intersect(right, bound);
          }
        }
        break;
      }
      case ShapeRelation::Kind::EQUAL: break; // Coalesced before propagation.
    }
  }

  const NativeAssemblyControl& m_control;
  mutable std::uint64_t m_work = 0;
  std::uint64_t m_workLimit = 0;
  std::map<std::string, std::vector<std::size_t>> m_axes;
  std::vector<std::size_t> m_parent;
  std::vector<Extent> m_values;
  std::vector<Rule> m_rules;
  std::vector<std::vector<std::size_t>> m_dependents;
  std::vector<bool> m_queued;
  std::deque<std::size_t> m_pending;
};

NativeJson
envelopeJson(const ExecutionShapeEnvelope& envelope)
{
  auto tensors = NativeJson::object();
  for (const auto& [name, bounds] : envelope.tensors) {
    NativeJson minimum = nullptr, maximum = nullptr;
    if (bounds.minimumDimensions) minimum = *bounds.minimumDimensions;
    if (bounds.maximumDimensions) {
      maximum = NativeJson::array();
      for (const auto& dim : *bounds.maximumDimensions)
        maximum.push_back(dim ? NativeJson(*dim) : NativeJson(nullptr));
    }
    tensors[name] = NativeJson{{"minimum", minimum}, {"maximum", maximum},
      {"maxBytes", bounds.maxBytes ? NativeJson(*bounds.maxBytes) : NativeJson(nullptr)},
      {"maxEncodedBytes", bounds.maxEncodedBytes ? NativeJson(*bounds.maxEncodedBytes) : NativeJson(nullptr)}};
  }
  return NativeJson{{"schema", "ndnsf-di-automatic-shape-envelope-v2"},
    {"contractDigest", envelope.contractDigest}, {"bindingDigest", envelope.bindingDigest},
    {"budgetDigest", envelope.budgetDigest}, {"maxContext", envelope.maxContext},
    {"maxNewTokens", envelope.maxNewTokens}, {"inputs", envelope.inputShapes},
    {"tensors", tensors}};
}

using TensorShapes = std::map<std::string, std::vector<std::int64_t>>;

std::int64_t
shapeDimension(std::uint64_t value)
{
  if (value > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
    rejectShape("AUTOMATIC_SHAPE_OVERFLOW");
  return static_cast<std::int64_t>(value);
}

TensorShapes
generationShapes(const AutomaticOnnxContract& contract, const TensorShapes& initial,
                  std::uint64_t predictions, std::uint64_t maxContext,
                  std::uint64_t round, const NativeAssemblyControl& control)
{
  requireActive(control);
  if (!contract.generation) rejectShape("GENERATION_CONTRACT_REQUIRED");
  if (predictions == 0 || round >= predictions) rejectShape("AUTOMATIC_GENERATION_ROUND");
  const auto& generation = *contract.generation;
  const auto tokens = initial.find(generation.tokenInput);
  if (tokens == initial.end() || tokens->second.size() != 2)
    rejectShape("AUTOMATIC_GENERATION_TOKEN_SHAPE");
  const auto batch = tokens->second.at(generation.tokenBatchAxis);
  const auto prompt = tokens->second.at(generation.tokenSequenceAxis);
  if (batch <= 0 || prompt <= 0) rejectShape("AUTOMATIC_GENERATION_TOKEN_SHAPE");
  const auto length = static_cast<std::uint64_t>(prompt);
  if (addExtent(length, predictions) > std::min(maxContext, generation.contextLimit))
    rejectShape("AUTOMATIC_GENERATION_CONTEXT_EXCEEDED");
  const auto sequence = round == 0 ? length : 1;
  const auto past = round == 0 ? 0 : addExtent(length, round - 1);
  const auto total = addExtent(past, sequence);
  const auto matrix = [&](std::uint64_t batchAxis, std::uint64_t sequenceAxis,
                           std::uint64_t extent) {
    std::vector<std::int64_t> result(2);
    result.at(batchAxis) = batch;
    result.at(sequenceAxis) = shapeDimension(extent);
    return result;
  };
  const auto cache = [&](const AutomaticAttentionState& state, bool output, std::uint64_t extent) {
    const auto& axes = output ? state.outputAxes : state.inputAxes;
    std::vector<std::int64_t> result(4);
    result.at(axes[0]) = batch;
    result.at(axes[1]) = shapeDimension(state.heads);
    result.at(axes[2]) = shapeDimension(extent);
    result.at(axes[3]) = shapeDimension(state.headSize);
    return result;
  };
  TensorShapes required;
  required[generation.tokenInput] = matrix(generation.tokenBatchAxis,
                                           generation.tokenSequenceAxis, length);
  required[generation.maskInput] = matrix(generation.maskBatchAxis,
                                          generation.maskContextAxis, length);
  if (!generation.positionInput.empty())
    required[generation.positionInput] = matrix(generation.positionBatchAxis,
                                                generation.positionSequenceAxis, length);
  for (const auto& state : generation.states) {
    requireActive(control);
    required[state.input] = cache(state, false, 0);
  }
  if (initial != required) rejectShape("AUTOMATIC_GENERATION_INITIAL_SHAPE");
  required[generation.tokenInput] = matrix(generation.tokenBatchAxis,
                                           generation.tokenSequenceAxis, sequence);
  required[generation.maskInput] = matrix(generation.maskBatchAxis,
                                          generation.maskContextAxis, total);
  if (!generation.positionInput.empty())
    required[generation.positionInput] = matrix(generation.positionBatchAxis,
                                                generation.positionSequenceAxis, sequence);
  for (const auto& state : generation.states) {
    requireActive(control);
    required[state.input] = cache(state, false, past);
    required[state.output] = cache(state, true, total);
  }
  std::vector<std::int64_t> logits(3);
  logits.at(generation.logitsBatchAxis) = batch;
  logits.at(generation.logitsSequenceAxis) = shapeDimension(sequence);
  logits.at(generation.logitsVocabularyAxis) = shapeDimension(generation.vocabularySize);
  required[generation.logitsOutput] = std::move(logits);
  return required;
}

std::map<std::string, AutomaticTensorBounds>
solveBounds(const AutomaticOnnxContract& contract, const ShapeObservations& observations,
             const NativeAssemblyControl& control)
{
  ShapeSolver solver(contract, observations, control);
  std::map<std::string, AutomaticTensorBounds> result;
  for (const auto& tensor : contract.tensors) {
    requireActive(control);
    auto& bounds = result[tensor.name];
    const auto* axes = solver.axes(tensor.name);
    if (!axes) continue;
    bounds.minimumDimensions.emplace();
    bounds.maximumDimensions.emplace();
    for (const auto axis : *axes) {
      const auto& value = solver.value(axis);
      bounds.minimumDimensions->push_back(value.lower);
      bounds.maximumDimensions->push_back(value.upper);
    }
  }
  return result;
}

void
mergeBounds(std::map<std::string, AutomaticTensorBounds>& bounds,
            const std::map<std::string, AutomaticTensorBounds>& next,
            const NativeAssemblyControl& control)
{
  for (auto& [name, value] : bounds) {
    requireActive(control);
    const auto& other = next.at(name);
    if (!value.maximumDimensions || !other.maximumDimensions) {
      value.minimumDimensions.reset(); value.maximumDimensions.reset();
      continue;
    }
    if (value.maximumDimensions->size() != other.maximumDimensions->size())
      rejectShape("AUTOMATIC_GENERATION_RANK_CHANGED");
    for (std::size_t axis = 0; axis < value.maximumDimensions->size(); ++axis) {
      value.minimumDimensions->at(axis) = std::min(value.minimumDimensions->at(axis),
                                                   other.minimumDimensions->at(axis));
      auto& upper = value.maximumDimensions->at(axis);
      const auto& maximum = other.maximumDimensions->at(axis);
      if (!upper || !maximum) upper.reset();
      else upper = std::max(*upper, *maximum);
    }
  }
}

ExecutionShapeEnvelope
bindShapeMetadata(const AutomaticOnnxContract& contract,
                  const std::map<std::string, std::vector<std::int64_t>>& inputShapes,
                  std::uint64_t maxNewTokens, std::uint64_t maxContext,
                  const NativeAssemblyControl& control)
{
  contract.validate(control);
  if (maxContext == 0) rejectShape("AUTOMATIC_SHAPE_CONTEXT_LIMIT");
  if (inputShapes.size() != contract.modelInputs.size()) rejectShape("AUTOMATIC_SHAPE_INPUT_SET");
  auto inputJson = NativeJson::object();
  for (const auto& name : contract.modelInputs) {
    if (!inputShapes.count(name)) rejectShape("AUTOMATIC_SHAPE_INPUT_SET");
    inputJson[name] = inputShapes.at(name);
  }
  ExecutionShapeEnvelope result;
  result.contractDigest = contract.digest(control);
  result.bindingDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-automatic-shape-binding-v1"}, {"contract", result.contractDigest},
    {"inputs", inputJson}}));
  result.budgetDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-automatic-shape-budget-v2"}, {"maxContext", maxContext},
    {"maxNewTokens", maxNewTokens}}));
  result.maxNewTokens = maxNewTokens; result.maxContext = maxContext;
  result.inputShapes = inputShapes;
  auto prefill = inputShapes;
  if (maxNewTokens != 0)
    prefill = generationShapes(contract, inputShapes, maxNewTokens, maxContext, 0, control);
  result.tensors = solveBounds(contract, exactObservations(prefill, control), control);
  if (maxNewTokens > 1) {
    const auto first = generationShapes(contract, inputShapes, maxNewTokens, maxContext, 1, control);
    const auto last = generationShapes(contract, inputShapes, maxNewTokens, maxContext,
                                       maxNewTokens - 1, control);
    auto decode = exactObservations(first, control);
    for (auto& [name, dimensions] : decode) {
      for (std::size_t axis = 0; axis < dimensions.size(); ++axis) {
        const auto maximum = last.at(name).at(axis);
        if (maximum < 0 || std::uint64_t(maximum) < dimensions[axis].lower)
          rejectShape("AUTOMATIC_GENERATION_SHAPE_RANGE");
        dimensions[axis].upper = static_cast<std::uint64_t>(maximum);
      }
    }
    mergeBounds(result.tensors, solveBounds(contract, decode, control), control);
  }
  for (const auto& tensor : contract.tensors) {
    requireActive(control);
    auto& bounds = result.tensors.at(tensor.name);
    if (!bounds.maximumDimensions) continue;
    bool zero = false, known = true;
    for (const auto& maximum : *bounds.maximumDimensions) {
      known = known && maximum.has_value();
      zero = zero || maximum == std::optional<std::uint64_t>(0);
    }
    if (tensor.wireElementType && (known || zero)) {
      std::uint64_t bytes = zero ? 0 : tensorElementByteSize(*tensor.wireElementType);
      if (!zero) for (const auto& dimension : *bounds.maximumDimensions)
        bytes = multiplyExtent(bytes, *dimension);
      bounds.maxBytes = bytes;
      // A nontransportable internal frame remains unknown; it is not a zero-cost cut.
      try {
        const auto separator = tensor.name.find('/');
        const auto rawName = separator == std::string::npos ? tensor.name : tensor.name.substr(separator + 1);
        bounds.maxEncodedBytes = tensorBundleEncodedSize(
          {{rawName.size(), bounds.maximumDimensions->size(), bytes}});
      }
      catch (const std::invalid_argument&) { bounds.maxEncodedBytes.reset(); }
    }
  }
  const auto encoded = nativeCanonicalJson(envelopeJson(result));
  if (encoded.size() > metadataLimit(control)) rejectShape("AUTOMATIC_SHAPE_ENVELOPE_SIZE");
  result.envelopeDigest = nativePlanningDigest(encoded);
  return result;
}

void
validateObservedTensor(const NamedTensor& tensor)
{
  validateNamedTensor(tensor);
}

void
validateObservedTensor(const AutomaticOnnxTensorObservation& tensor)
{
  if (tensorStorageByteSize(tensor.elementType, tensor.shape) != tensor.payloadBytes)
    rejectShape("AUTOMATIC_SHAPE_TENSOR_BYTES");
}

template<typename Tensor>
std::map<std::string, std::vector<std::int64_t>>
admitTensorShapes(const AutomaticOnnxContract& contract,
                  const std::vector<std::string>& expected,
                  const std::vector<Tensor>& tensors,
                  const NativeAssemblyControl& control)
{
  if (tensors.size() != expected.size()) rejectShape("AUTOMATIC_SHAPE_INPUT_SET");
  std::map<std::string, const AutomaticTensorContract*> contracts;
  for (const auto& tensor : contract.tensors) contracts.emplace(tensor.name, &tensor);
  const std::set<std::string> names(expected.begin(), expected.end());
  if (names.size() != expected.size()) rejectShape("AUTOMATIC_SHAPE_EXPECTED_SET");
  std::map<std::string, std::vector<std::int64_t>> result;
  for (const auto& tensor : tensors) {
    requireActive(control);
    validateObservedTensor(tensor);
    const auto name = "graph/" + tensor.name;
    const auto found = contracts.find(name);
    if (!names.count(name) || found == contracts.end() ||
        found->second->wireElementType != std::optional<TensorElementType>(tensor.elementType) ||
        !result.emplace(name, tensor.shape).second)
      rejectShape("AUTOMATIC_SHAPE_TENSOR_CONFLICT");
  }
  return result;
}

template<typename Tensor>
void
validateGenerationValues(const AutomaticOnnxContract& contract, const ExecutionShapeEnvelope& envelope,
                          const std::vector<Tensor>& tensors, std::uint64_t round,
                          const NativeAssemblyControl& control)
{
  if (envelope.maxNewTokens == 0) return;
  const auto& generation = *contract.generation;
  const auto& initial = envelope.inputShapes.at(generation.tokenInput);
  const auto prompt = static_cast<std::uint64_t>(initial.at(generation.tokenSequenceAxis));
  const auto past = round == 0 ? 0 : addExtent(prompt, round - 1);
  for (const auto& tensor : tensors) {
    requireActive(control);
    const auto name = "graph/" + tensor.name;
    const bool token = name == generation.tokenInput;
    const bool mask = name == generation.maskInput;
    const bool position = !generation.positionInput.empty() && name == generation.positionInput;
    if (!token && !mask && !position) continue;
    const auto width = tensor.elementType == TensorElementType::Int64 ? 8U :
                       tensor.elementType == TensorElementType::Int32 ? 4U : 0U;
    if (width == 0 || tensor.shape.size() != 2 || tensor.shape[0] <= 0 || tensor.shape[1] <= 0)
      rejectShape("AUTOMATIC_GENERATION_INTEGER_INPUT");
    for (std::size_t offset = 0; offset < tensor.payload.size(); offset += width) {
      if ((offset / width) % 1024 == 0) requireActive(control);
      std::uint64_t value = 0;
      for (unsigned byte = 0; byte < width; ++byte)
        value |= std::uint64_t(tensor.payload[offset + byte]) << (8 * byte);
      if (token && value >= generation.vocabularySize)
        rejectShape("AUTOMATIC_GENERATION_TOKEN_RANGE");
      if (mask && value != 1) rejectShape("AUTOMATIC_GENERATION_MASK_POLICY");
      if (position) {
        const auto index = static_cast<std::uint64_t>(offset / width);
        const auto columns = static_cast<std::uint64_t>(tensor.shape[1]);
        const auto sequence = generation.positionSequenceAxis == 0 ? index / columns :
                                                                      index % columns;
        if (value != addExtent(past, sequence))
          rejectShape("AUTOMATIC_GENERATION_POSITION_POLICY");
      }
    }
  }
}

template<typename Tensor>
void
validateAdmittedTensorShapes(const AutomaticOnnxContract& contract,
                            const ExecutionShapeEnvelope& envelope,
                            const std::vector<std::string>& expectedScopedNames,
                            const std::vector<Tensor>& tensors, std::uint64_t roundIndex,
                            const NativeAssemblyControl& control)
{
  const auto observed = admitTensorShapes(contract, expectedScopedNames, tensors, control);
  auto bindings = envelope.inputShapes;
  if (envelope.maxNewTokens != 0)
    bindings = generationShapes(contract, envelope.inputShapes, envelope.maxNewTokens,
                                  envelope.maxContext, roundIndex, control);
  else if (roundIndex != 0) rejectShape("AUTOMATIC_GENERATION_ROUND");
  for (const auto& [name, shape] : observed) {
    const auto& bounds = envelope.tensors.at(name);
    if (!bounds.maximumDimensions || shape.size() != bounds.maximumDimensions->size())
      rejectShape("SHAPE_ENVELOPE_EXCEEDED");
    for (std::size_t axis = 0; axis < shape.size(); ++axis) {
      const auto& maximum = bounds.maximumDimensions->at(axis);
      if (!maximum || std::uint64_t(shape[axis]) > *maximum ||
          std::uint64_t(shape[axis]) < bounds.minimumDimensions->at(axis))
        rejectShape("SHAPE_ENVELOPE_EXCEEDED");
    }
    const auto inserted = bindings.emplace(name, shape);
    if (!inserted.second && inserted.first->second != shape)
      rejectShape("AUTOMATIC_SHAPE_INPUT_CONFLICT");
  }
  // Actual outputs must also satisfy the cross-tensor relations, not only byte maxima.
  ShapeSolver solved(contract, bindings, control);
}

void
validateEnvelopeIdentity(const AutomaticOnnxContract& contract,
                          const ExecutionShapeEnvelope& envelope,
                          const NativeAssemblyControl& control)
{
  requireActive(control);
  const auto recomputed = rebindAutomaticOnnxShapeSeed(contract,
    automaticOnnxShapeSeed(envelope), control);
  if (envelope.envelopeDigest != recomputed.envelopeDigest ||
      nativeCanonicalJson(envelopeJson(envelope)) != nativeCanonicalJson(envelopeJson(recomputed)))
    rejectShape("AUTOMATIC_SHAPE_ENVELOPE_IDENTITY");
}

} // namespace

NativeAutomaticShapeSeed
automaticOnnxShapeSeed(const ExecutionShapeEnvelope& envelope)
{
  return {envelope.contractDigest, envelope.bindingDigest, envelope.budgetDigest,
    envelope.envelopeDigest, envelope.maxContext, envelope.maxNewTokens, envelope.inputShapes};
}

ExecutionShapeEnvelope
rebindAutomaticOnnxShapeSeed(const AutomaticOnnxContract& contract,
                             const NativeAutomaticShapeSeed& seed,
                             const NativeAssemblyControl& control)
{
  requireActive(control);
  (void)nativeAutomaticShapeSeedToJson(seed, metadataLimit(control));
  const auto rebound = bindShapeMetadata(contract, seed.inputShapes,
    seed.maxNewTokens, seed.maxContext, control);
  if (seed.contractDigest != rebound.contractDigest || seed.bindingDigest != rebound.bindingDigest ||
      seed.budgetDigest != rebound.budgetDigest || seed.envelopeDigest != rebound.envelopeDigest)
    rejectShape("AUTOMATIC_SHAPE_SEED_IDENTITY");
  return rebound;
}

ExecutionShapeEnvelope
bindAutomaticOnnxShapes(const AutomaticOnnxContract& contract,
                        const std::vector<NamedTensor>& inputs,
                        std::uint64_t maxNewTokens, std::uint64_t maxContext,
                        const NativeAssemblyControl& control)
{
  contract.validate(control);
  const auto shapes = admitTensorShapes(contract, contract.modelInputs, inputs, control);
  auto result = bindShapeMetadata(contract, shapes, maxNewTokens, maxContext, control);
  validateGenerationValues(contract, result, inputs, 0, control);
  return result;
}

std::map<std::string, std::vector<std::int64_t>>
automaticOnnxRoundInputShapes(const AutomaticOnnxContract& contract,
                              const ExecutionShapeEnvelope& envelope,
                              std::uint64_t roundIndex,
                              const NativeAssemblyControl& control)
{
  validateEnvelopeIdentity(contract, envelope, control);
  if (envelope.maxNewTokens == 0) {
    if (roundIndex != 0) rejectShape("AUTOMATIC_GENERATION_ROUND");
    return envelope.inputShapes;
  }
  auto shapes = generationShapes(contract, envelope.inputShapes, envelope.maxNewTokens,
                                  envelope.maxContext, roundIndex, control);
  // Exact round constraints may be stronger than an interval hull.
  ShapeSolver solved(contract, shapes, control);
  std::map<std::string, std::vector<std::int64_t>> result;
  for (const auto& name : contract.modelInputs) result.emplace(name, shapes.at(name));
  return result;
}

void
validateAutomaticOnnxTensors(const AutomaticOnnxContract& contract,
                             const ExecutionShapeEnvelope& envelope,
                             const std::vector<std::string>& expectedScopedNames,
                             const std::vector<NamedTensor>& tensors,
                             const NativeAssemblyControl& control)
{
  validateAutomaticOnnxTensors(contract, envelope, expectedScopedNames, tensors, 0, control);
}

void
validateAutomaticOnnxTensors(const AutomaticOnnxContract& contract,
                             const ExecutionShapeEnvelope& envelope,
                             const std::vector<std::string>& expectedScopedNames,
                             const std::vector<NamedTensor>& tensors,
                             std::uint64_t roundIndex,
                             const NativeAssemblyControl& control)
{
  validateEnvelopeIdentity(contract, envelope, control);
  validateAdmittedTensorShapes(contract, envelope, expectedScopedNames, tensors, roundIndex, control);
  validateGenerationValues(contract, envelope, tensors, roundIndex, control);
  requireActive(control);
}

void
AutomaticOnnxRequestBinding::validateInputs(
  const std::vector<AutomaticOnnxTensorObservation>& observed,
  const std::vector<NamedTensorView>& actualCpuControls, const RoleExecutionContext& ctx) const
{
  const auto control = validationControl(ctx);
  requireActive(control);
  validateAdmittedTensorShapes(*m_source, m_envelope, m_inputs, observed,
                              ctx.inferenceEpoch, control);
  if (actualCpuControls.size() != m_controlInputs.size())
    rejectShape("AUTOMATIC_GENERATION_CONTROL_SET");
  std::set<std::string> seen;
  for (const auto& value : actualCpuControls) {
    requireActive(control);
    validateNamedTensorView(value);
    const auto metadata = std::find_if(observed.begin(), observed.end(),
      [&] (const auto& item) { return item.name == value.name; });
    if (!seen.insert(value.name).second ||
        std::find(m_controlInputs.begin(), m_controlInputs.end(), value.name) == m_controlInputs.end() ||
        metadata == observed.end() || metadata->elementType != value.elementType ||
        metadata->shape != value.shape || metadata->payloadBytes != value.payload.size())
      rejectShape("AUTOMATIC_GENERATION_CONTROL_BINDING");
  }
  validateGenerationValues(*m_source, m_envelope, actualCpuControls, ctx.inferenceEpoch, control);
  requireActive(control);
}

void
AutomaticOnnxRequestBinding::validateOutputs(
  const std::vector<AutomaticOnnxTensorObservation>& observed, const RoleExecutionContext& ctx) const
{
  const auto control = validationControl(ctx);
  requireActive(control);
  validateAdmittedTensorShapes(*m_source, m_envelope, m_outputs, observed,
                              ctx.inferenceEpoch, control);
  requireActive(control);
}

} // namespace ndnsf::di
