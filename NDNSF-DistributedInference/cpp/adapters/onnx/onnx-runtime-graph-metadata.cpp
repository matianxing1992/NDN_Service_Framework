#include "onnx-runtime-graph-metadata.hpp"

#include <onnx/onnx_pb.h>
#include <onnx/shape_inference/implementation.h>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf::di::detail {
namespace {

using google::protobuf::io::CodedInputStream;

constexpr int MAX_METADATA_BYTES = 64 * 1024 * 1024;
constexpr int MAX_MESSAGE_BYTES = 1024 * 1024;
constexpr int MAX_TENSOR_ELEMENTS = 64;

std::array<std::uint64_t, 7>
fileStamp(const struct stat& status)
{
  return {static_cast<std::uint64_t>(status.st_dev),
          static_cast<std::uint64_t>(status.st_ino),
          static_cast<std::uint64_t>(status.st_size),
          static_cast<std::uint64_t>(status.st_mtim.tv_sec),
          static_cast<std::uint64_t>(status.st_mtim.tv_nsec),
          static_cast<std::uint64_t>(status.st_ctim.tv_sec),
          static_cast<std::uint64_t>(status.st_ctim.tv_nsec)};
}

class UnsupportedProjection : public std::runtime_error
{
public:
  UnsupportedProjection() : std::runtime_error("unsupported ONNX metadata projection") {}
};

void
require(bool condition)
{
  if (!condition) throw UnsupportedProjection();
}

int
readLength(CodedInputStream& input)
{
  std::uint32_t length = 0;
  require(input.ReadVarint32(&length));
  require(length <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()));
  const int remaining = input.BytesUntilLimit();
  require(remaining >= 0 && length <= static_cast<std::uint32_t>(remaining));
  return static_cast<int>(length);
}

void
skipField(CodedInputStream& input, std::uint32_t tag)
{
  std::uint64_t ignored = 0;
  switch (tag & 7) {
    case 0: require(input.ReadVarint64(&ignored)); break;
    case 1: require(input.Skip(8)); break;
    case 2: require(input.Skip(readLength(input))); break;
    case 5: require(input.Skip(4)); break;
    default: throw UnsupportedProjection();
  }
}

template<typename Message>
void
readMessage(CodedInputStream& input, Message& message, int& budget)
{
  const int length = readLength(input);
  require(length <= MAX_MESSAGE_BYTES && length <= budget);
  budget -= length;
  std::string bytes;
  require(input.ReadString(&bytes, length));
  require(message.ParseFromString(bytes));
}

// Retain official TensorProto headers and tiny integral constants. Large raw
// weight fields are skipped in the file stream, never copied into another graph.
void
readInitializer(CodedInputStream& input, onnx::TensorProto& tensor, int& budget)
{
  const auto limit = input.PushLimit(readLength(input));
  while (input.BytesUntilLimit() > 0) {
    const auto tag = input.ReadTag();
    require(tag != 0);
    const auto field = tag >> 3;
    const auto wire = tag & 7;
    if (field == onnx::TensorProto::kNameFieldNumber && wire == 2) {
      const int length = readLength(input);
      require(length <= MAX_MESSAGE_BYTES && length <= budget);
      budget -= length;
      require(input.ReadString(tensor.mutable_name(), length));
    }
    else if (field == onnx::TensorProto::kDataTypeFieldNumber && wire == 0) {
      std::uint32_t type = 0;
      require(input.ReadVarint32(&type));
      tensor.set_data_type(type);
    }
    else if (field == onnx::TensorProto::kDimsFieldNumber) {
      auto readDimension = [&] {
        std::uint64_t value = 0;
        require(tensor.dims_size() < MAX_TENSOR_ELEMENTS && input.ReadVarint64(&value));
        tensor.add_dims(static_cast<std::int64_t>(value));
      };
      if (wire == 0) readDimension();
      else if (wire == 2) {
        const auto packed = input.PushLimit(readLength(input));
        while (input.BytesUntilLimit() > 0) readDimension();
        input.PopLimit(packed);
      }
      else throw UnsupportedProjection();
    }
    else if (field == onnx::TensorProto::kRawDataFieldNumber && wire == 2) {
      const int length = readLength(input);
      if (length <= MAX_TENSOR_ELEMENTS * 8 && length <= budget) {
        budget -= length;
        require(input.ReadString(tensor.mutable_raw_data(), length));
      }
      else require(input.Skip(length));
    }
    else if ((field == onnx::TensorProto::kInt64DataFieldNumber ||
              field == onnx::TensorProto::kInt32DataFieldNumber) && (wire == 0 || wire == 2)) {
      auto readInteger = [&] {
        std::uint64_t value = 0;
        require(tensor.int64_data_size() + tensor.int32_data_size() < MAX_TENSOR_ELEMENTS);
        require(budget >= 8 && input.ReadVarint64(&value));
        budget -= 8;
        if (field == onnx::TensorProto::kInt64DataFieldNumber)
          tensor.add_int64_data(static_cast<std::int64_t>(value));
        else tensor.add_int32_data(static_cast<std::int32_t>(value));
      };
      if (wire == 0) readInteger();
      else {
        const auto packed = input.PushLimit(readLength(input));
        while (input.BytesUntilLimit() > 0) readInteger();
        input.PopLimit(packed);
      }
    }
    else skipField(input, tag);
  }
  input.PopLimit(limit);
}

onnx::ModelProto
readProjection(CodedInputStream& input)
{
  onnx::ModelProto model;
  int budget = MAX_METADATA_BYTES;
  bool foundGraph = false;
  while (input.BytesUntilLimit() > 0) {
    const auto tag = input.ReadTag();
    require(tag != 0);
    const auto field = tag >> 3;
    if (field == onnx::ModelProto::kIrVersionFieldNumber && (tag & 7) == 0) {
      std::uint64_t version = 0;
      require(input.ReadVarint64(&version));
      model.set_ir_version(version);
    }
    else if (field == onnx::ModelProto::kOpsetImportFieldNumber && (tag & 7) == 2) {
      readMessage(input, *model.add_opset_import(), budget);
    }
    else if (field == onnx::ModelProto::kFunctionsFieldNumber) {
      throw UnsupportedProjection();
    }
    else if (field == onnx::ModelProto::kGraphFieldNumber && (tag & 7) == 2) {
      require(!foundGraph);
      foundGraph = true;
      auto& graph = *model.mutable_graph();
      const auto limit = input.PushLimit(readLength(input));
      while (input.BytesUntilLimit() > 0) {
        const auto graphTag = input.ReadTag();
        require(graphTag != 0);
        const auto graphField = graphTag >> 3;
        if ((graphTag & 7) != 2) {
          skipField(input, graphTag);
          continue;
        }
        switch (graphField) {
          case onnx::GraphProto::kNodeFieldNumber:
            require(graph.node_size() < 100000);
            readMessage(input, *graph.add_node(), budget);
            break;
          case onnx::GraphProto::kInitializerFieldNumber:
            require(graph.initializer_size() < 100000);
            readInitializer(input, *graph.add_initializer(), budget);
            break;
          case onnx::GraphProto::kInputFieldNumber:
            readMessage(input, *graph.add_input(), budget);
            break;
          case onnx::GraphProto::kOutputFieldNumber:
            readMessage(input, *graph.add_output(), budget);
            break;
          case onnx::GraphProto::kValueInfoFieldNumber:
            readMessage(input, *graph.add_value_info(), budget);
            break;
          case onnx::GraphProto::kSparseInitializerFieldNumber:
            throw UnsupportedProjection();
          default: skipField(input, graphTag); break;
        }
      }
      input.PopLimit(limit);
    }
    else skipField(input, tag);
  }
  require(foundGraph && model.graph().node_size() != 0);
  return model;
}

bool
isIntegral(int type)
{
  return type == onnx::TensorProto::INT64 || type == onnx::TensorProto::INT32 ||
         type == onnx::TensorProto::BOOL;
}

bool
isSmallLiteral(const onnx::TensorProto& tensor)
{
  if (!isIntegral(tensor.data_type())) return false;
  std::uint64_t elements = 1;
  for (auto dimension : tensor.dims()) {
    if (dimension <= 0 || dimension > MAX_TENSOR_ELEMENTS ||
        elements > MAX_TENSOR_ELEMENTS / static_cast<std::uint64_t>(dimension)) return false;
    elements *= dimension;
  }
  const auto width = tensor.data_type() == onnx::TensorProto::INT64 ? 8 :
                     tensor.data_type() == onnx::TensorProto::INT32 ? 4 : 1;
  return tensor.raw_data().size() == elements * width ||
         tensor.int64_data_size() == static_cast<int>(elements) ||
         tensor.int32_data_size() == static_cast<int>(elements);
}

bool
isStructuralInput(const std::string& op, int index)
{
  if (op == "Reshape" || op == "Expand" || op == "Tile" || op == "Squeeze" ||
      op == "Unsqueeze") return index == 1;
  if (op == "Slice") return index >= 1 && index <= 4;
  if (op == "ConstantOfShape") return index == 0;
  if (op == "Range") return index >= 0 && index <= 2;
  if (op == "Pad") return index == 1;
  if (op == "Resize") return index == 3;
  return false;
}

std::set<std::string>
attentionWarmupLengthInputs(const onnx::ModelProto& model)
{
  std::size_t imports = 0;
  for (const auto& entry : model.opset_import()) {
    if (entry.domain() == "com.microsoft") {
      if (entry.version() != 1) return {};
      ++imports;
    }
  }
  if (imports != 1) return {};
  std::set<std::string> scalarInputs;
  for (const auto& input : model.graph().input()) {
    if (!input.has_type() || !input.type().has_tensor_type()) continue;
    const auto& type = input.type().tensor_type();
    if (type.elem_type() == onnx::TensorProto::INT32 && type.has_shape() &&
        type.shape().dim_size() == 0)
      scalarInputs.insert(input.name());
  }
  std::set<std::string> result;
  for (const auto& node : model.graph().node()) {
    if (node.domain() == "com.microsoft" && node.op_type() == "GroupQueryAttention" &&
        node.input_size() > 6 && scalarInputs.count(node.input(6)))
      result.insert(node.input(6));
  }
  return result;
}

OnnxRuntimeGraphMetadata
classify(onnx::ModelProto& model)
{
  auto& graph = *model.mutable_graph();
  std::set<std::string> nodeNames;
  std::set<std::string> definitions;
  std::map<std::string, int> types;
  std::set<std::string> knownGeometry;
  std::set<std::string> shapeDerivedValues;
  for (const auto& input : graph.input()) {
    require(definitions.insert(input.name()).second);
  }
  for (const auto& initializer : graph.initializer()) {
    // Old ONNX permits initializers to also be graph inputs. Such a value is
    // externally replaceable, so it must never seed a metadata exception.
    const bool immutable = definitions.insert(initializer.name()).second;
    types[initializer.name()] = initializer.data_type();
    if (immutable && isSmallLiteral(initializer)) knownGeometry.insert(initializer.name());
  }
  for (const auto& node : graph.node()) {
    require(!node.name().empty() && nodeNames.insert(node.name()).second);
    require(node.domain().empty() || node.domain() == "ai.onnx");
    require(node.output_size() != 0);
    for (const auto& input : node.input()) {
      require(input.empty() || definitions.count(input) != 0);
    }
    for (const auto& attribute : node.attribute()) {
      require(attribute.type() != onnx::AttributeProto::GRAPH &&
              attribute.type() != onnx::AttributeProto::GRAPHS);
    }
    for (const auto& output : node.output()) {
      require(!output.empty() && definitions.insert(output).second);
    }
  }
  onnx::shape_inference::InferShapes(model);
  auto rememberType = [&types] (const auto& value) {
    if (value.has_type() && value.type().has_tensor_type())
      types[value.name()] = value.type().tensor_type().elem_type();
  };
  for (const auto& value : graph.input()) rememberType(value);
  for (const auto& value : graph.output()) rememberType(value);
  for (const auto& value : graph.value_info()) rememberType(value);

  const std::set<std::string> geometryOps{
    "Add", "And", "Cast", "Concat", "ConstantOfShape", "Div", "Equal", "Expand",
    "Gather", "Greater", "GreaterOrEqual", "Identity", "Less", "LessOrEqual", "Mul",
    "Not", "Or", "Range", "Reshape", "Slice", "Squeeze", "Sub", "Unsqueeze", "Where"};
  std::set<std::string> candidates;
  std::set<std::string> shapeDerivedNodes;
  std::set<std::string> noOpNodes;
  std::map<std::string, std::vector<std::pair<std::string, int>>> consumers;
  std::map<std::string, const onnx::NodeProto*> nodes;
  std::set<std::string> graphOutputs;
  for (const auto& output : graph.output()) graphOutputs.insert(output.name());

  // ONNX graphs are topologically ordered. An unresolved/nonstandard value
  // simply prevents an exception; integer model inputs are never geometry seeds.
  for (const auto& node : graph.node()) {
    nodes[node.name()] = &node;
    for (int i = 0; i < node.input_size(); ++i) {
      if (!node.input(i).empty()) consumers[node.input(i)].emplace_back(node.name(), i);
    }
    bool geometry = false;
    bool shapeDerived = false;
    if (node.op_type() == "Constant") {
      for (const auto& attribute : node.attribute()) {
        if (attribute.name() == "value" && attribute.has_t())
          geometry = isSmallLiteral(attribute.t());
      }
    }
    else if (node.op_type() == "Shape" || node.op_type() == "Size") {
      geometry = node.input_size() == 1;
      shapeDerived = true;
    }
    else if (geometryOps.count(node.op_type()) && node.input_size() > 0) {
      geometry = std::all_of(node.input().begin(), node.input().end(), [&] (const auto& name) {
        return name.empty() || knownGeometry.count(name) != 0;
      });
      shapeDerived = std::any_of(node.input().begin(), node.input().end(), [&] (const auto& name) {
        return shapeDerivedValues.count(name) != 0;
      });
    }
    for (const auto& output : node.output()) {
      const auto type = types.find(output);
      geometry = geometry && type != types.end() && isIntegral(type->second);
    }
    if (geometry) {
      candidates.insert(node.name());
      knownGeometry.insert(node.output().begin(), node.output().end());
      if (shapeDerived) {
        shapeDerivedNodes.insert(node.name());
        shapeDerivedValues.insert(node.output().begin(), node.output().end());
      }
    }
    if (node.op_type() == "Identity" && node.input_size() == 1 && node.output_size() == 1)
      noOpNodes.insert(node.name());
    if (node.op_type() == "Cast" && node.input_size() == 1) {
      const auto inputType = types.find(node.input(0));
      for (const auto& attribute : node.attribute()) {
        if (attribute.name() == "to" && inputType != types.end() &&
            attribute.i() == inputType->second) noOpNodes.insert(node.name());
      }
    }
  }

  // Shape-derived integer values remain metadata even when consumed by model
  // arithmetic (for example a dimension scaling position IDs). Literal-only
  // arithmetic needs a structural use, so integer weights are not an exception.
  // Reverse topological order propagates removals in one bounded pass.
  for (auto it = graph.node().rbegin(); it != graph.node().rend(); ++it) {
    const auto& node = *it;
    if (!candidates.count(node.name())) continue;
    bool structuralOnly = true;
    for (const auto& output : node.output()) {
      if (graphOutputs.count(output)) structuralOnly = false;
      for (const auto& use : consumers[output]) {
        const auto& consumer = *nodes.at(use.first);
        if (!shapeDerivedNodes.count(node.name()) && !candidates.count(use.first) &&
            !isStructuralInput(consumer.op_type(), use.second)) structuralOnly = false;
      }
    }
    if (!structuralOnly) candidates.erase(node.name());
  }

  OnnxRuntimeGraphMetadata result;
  for (const auto& node : graph.node()) {
    if (candidates.count(node.name())) result.hostMetadataNodes[node.name()] = node.op_type();
    else if (node.op_type() != "Constant") {
      auto& inventory = noOpNodes.count(node.name()) ? result.optionalModelNodes :
                                                     result.modelNodes;
      inventory[node.name()] = node.op_type();
    }
  }
  std::map<std::string, const onnx::NodeProto*> producers;
  auto castParameters = [] (const onnx::NodeProto& node) {
    std::vector<std::string> attributes;
    for (const auto& attribute : node.attribute()) attributes.push_back(attribute.SerializeAsString());
    std::sort(attributes.begin(), attributes.end());
    return attributes;
  };
  for (const auto& node : graph.node()) {
    if (node.op_type() == "Cast" && node.input_size() == 1 &&
        result.optionalModelNodes.count(node.name())) {
      const auto parent = producers.find(node.input(0));
      if (parent != producers.end() && parent->second->op_type() == "Cast" &&
          castParameters(*parent->second) == castParameters(node)) {
        const auto& parentName = parent->second->name();
        if (result.modelNodes.count(parentName)) result.modelNodeAliases[node.name()] = parentName;
        else {
          const auto alias = result.modelNodeAliases.find(parentName);
          if (alias != result.modelNodeAliases.end()) result.modelNodeAliases[node.name()] = alias->second;
        }
      }
    }
    for (const auto& output : node.output()) producers[output] = &node;
  }
  return result;
}

} // namespace

OnnxRuntimeGraphMetadata
inspectOnnxRuntimeGraphMetadata(const std::string& modelPath, bool collectHostMetadata)
{
  std::unique_ptr<std::FILE, decltype(&std::fclose)> file(
    std::fopen(modelPath.c_str(), "rb"), &std::fclose);
  if (!file) throw std::runtime_error("cannot open ONNX graph metadata: " + modelPath);
  struct stat before{};
  if (fstat(fileno(file.get()), &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_size <= 0 || before.st_size > std::numeric_limits<int>::max()) return {};
  OnnxRuntimeGraphMetadata result;
  try {
    google::protobuf::io::FileInputStream stream(fileno(file.get()));
    CodedInputStream input(&stream);
    input.SetTotalBytesLimit(std::numeric_limits<int>::max());
    const auto limit = input.PushLimit(static_cast<int>(before.st_size));
    auto model = readProjection(input);
    require(input.BytesUntilLimit() == 0);
    input.PopLimit(limit);
    const auto warmupInputs = attentionWarmupLengthInputs(model);
    if (collectHostMetadata) {
      try {
        result = classify(model);
      }
      catch (const std::exception&) {
        // A warmup fact cannot relax CUDA placement for a nonstandard graph.
        result = {};
      }
    }
    result.attentionWarmupLengthInputs = warmupInputs;
  }
  catch (const std::exception&) {
    // Unsupported graphs retain the original strict CPU-fallback rejection.
    result = {};
  }
  struct stat after{};
  if (fstat(fileno(file.get()), &after) != 0 || fileStamp(before) != fileStamp(after))
    throw std::runtime_error("ONNX graph changed during metadata inspection");
  result.sourceFileStamp = fileStamp(after);
  return result;
}

void
verifyOnnxRuntimeGraphMetadataSource(const std::string& modelPath,
                                    const OnnxRuntimeGraphMetadata& metadata)
{
  if (metadata.hostMetadataNodes.empty() && metadata.attentionWarmupLengthInputs.empty()) return;
  struct stat status{};
  if (stat(modelPath.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
      fileStamp(status) != metadata.sourceFileStamp)
    throw std::runtime_error("ONNX graph changed before session admission");
}

} // namespace ndnsf::di::detail
