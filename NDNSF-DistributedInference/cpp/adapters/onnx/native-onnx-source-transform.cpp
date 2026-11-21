#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

#include <onnx/checker.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace ndnsf::di {
namespace {

onnx::NodeProto*
findNode(onnx::ModelProto& model, const std::string& name)
{
  for (auto& node : *model.mutable_graph()->mutable_node())
    if (node.name() == name) return &node;
  return nullptr;
}

onnx::TensorProto*
findInitializer(onnx::ModelProto& model, const std::string& name)
{
  for (auto& initializer : *model.mutable_graph()->mutable_initializer())
    if (initializer.name() == name) return &initializer;
  return nullptr;
}

std::uint64_t
elementBytes(int dataType)
{
  if (dataType == onnx::TensorProto::FLOAT) return 4;
  throw std::invalid_argument("source transform supports float32 initializers only");
}

std::uint64_t
countOf(const onnx::TensorProto& tensor)
{
  std::uint64_t count = 1;
  for (const auto dim : tensor.dims()) {
    if (dim < 0) throw std::invalid_argument("source transform initializer is dynamic");
    count *= static_cast<std::uint64_t>(dim);
  }
  return count;
}

std::vector<std::uint8_t>
rawBytes(const onnx::TensorProto& tensor, const NativeOnnxExternalReader& external)
{
  const auto bytes = countOf(tensor) * elementBytes(tensor.data_type());
  if (tensor.data_location() == onnx::TensorProto::EXTERNAL || !tensor.external_data().empty()) {
    std::string location, offset, length;
    for (const auto& entry : tensor.external_data()) {
      if (entry.key() == "location") location = entry.value();
      else if (entry.key() == "offset") offset = entry.value();
      else if (entry.key() == "length") length = entry.value();
    }
    if (location.empty() || !external)
      throw std::invalid_argument("source transform requires external initializer bytes");
    const auto start = offset.empty() ? 0 : std::stoull(offset);
    const auto want = length.empty() ? bytes : std::stoull(length);
    auto result = external(location, start, want);
    if (result.size() != bytes)
      throw std::invalid_argument("source transform external initializer length differs");
    return result;
  }
  if (!tensor.raw_data().empty()) {
    const auto& raw = tensor.raw_data();
    return {raw.begin(), raw.end()};
  }
  if (tensor.float_data_size() != 0) {
    std::vector<std::uint8_t> result(static_cast<std::size_t>(bytes));
    for (int i = 0; i < tensor.float_data_size(); ++i) {
      const auto value = tensor.float_data(i);
      std::memcpy(result.data() + static_cast<std::size_t>(i) * 4, &value, 4);
    }
    return result;
  }
  throw std::invalid_argument("source transform initializer has no bytes");
}

void
setInlineBytes(onnx::TensorProto& tensor, std::vector<std::uint8_t> bytes)
{
  tensor.clear_external_data();
  tensor.set_data_location(onnx::TensorProto::DEFAULT);
  tensor.clear_raw_data();
  tensor.clear_float_data();
  tensor.set_raw_data(bytes.data(), bytes.size());
}

bool
hasExternalInitializers(const onnx::ModelProto& model)
{
  for (const auto& initializer : model.graph().initializer())
    if (initializer.data_location() == onnx::TensorProto::EXTERNAL ||
        !initializer.external_data().empty())
      return true;
  return false;
}

std::uint64_t
parseExternalCoordinate(std::string_view value)
{
  std::uint64_t result = 0;
  const auto* end = value.data() + value.size();
  const auto parsed = std::from_chars(value.data(), end, result);
  if (parsed.ec != std::errc() || parsed.ptr != end)
    throw std::invalid_argument("invalid source transform external coordinate");
  return result;
}

std::uint64_t
canonicalExternalOffset(const onnx::TensorProto& tensor, std::uint64_t expectedBytes)
{
  if (tensor.data_location() != onnx::TensorProto::EXTERNAL ||
      tensor.external_data_size() != 3 || tensor.has_segment() || !tensor.raw_data().empty() ||
      tensor.float_data_size() != 0 || tensor.int32_data_size() != 0 ||
      tensor.int64_data_size() != 0 || tensor.uint64_data_size() != 0 ||
      tensor.double_data_size() != 0 || tensor.string_data_size() != 0)
    throw std::invalid_argument("invalid source transform external storage");
  std::string location, offset, length;
  for (const auto& field : tensor.external_data()) {
    if (field.key() == "location") location = field.value();
    else if (field.key() == "offset") offset = field.value();
    else if (field.key() == "length") length = field.value();
  }
  const auto start = parseExternalCoordinate(offset);
  if (location != "model.onnx.data" || parseExternalCoordinate(length) != expectedBytes ||
      start > std::numeric_limits<std::uint64_t>::max() - expectedBytes)
    throw std::invalid_argument("invalid source transform external range");
  return start;
}

void
sliceProjectionInitializer(onnx::TensorProto& tensor, std::uint64_t rows, std::uint64_t columns,
               std::uint64_t rowBegin, std::uint64_t rowEnd,
               std::uint64_t columnBegin, std::uint64_t columnEnd,
               const NativeOnnxExternalReader& external, const NativeOnnxExternalAppender& append,
               std::uint64_t& sidecarOffset)
{
  const bool isFloat = tensor.data_type() == onnx::TensorProto::FLOAT;
  const std::uint64_t itemBytes = isFloat ? 4 : 1;
  if ((!isFloat && tensor.data_type() != onnx::TensorProto::UINT8) ||
      rows == 0 || columns == 0 ||
      rows > std::numeric_limits<std::size_t>::max() / itemBytes / columns ||
      rowBegin >= rowEnd || rowEnd > rows || columnBegin >= columnEnd || columnEnd > columns)
    throw std::invalid_argument("invalid paired FFN initializer dimensions");
  const auto elements = rows * columns;
  const auto rowBytes = (columnEnd - columnBegin) * itemBytes;
  const auto sliceBytes = (rowEnd - rowBegin) * rowBytes;
  if (tensor.has_segment() || tensor.int32_data_size() != 0 ||
      tensor.int64_data_size() != 0 || tensor.uint64_data_size() != 0 ||
      tensor.double_data_size() != 0 || tensor.string_data_size() != 0)
    throw std::invalid_argument("invalid projection initializer storage fields");
  if (tensor.data_location() == onnx::TensorProto::EXTERNAL || !tensor.external_data().empty()) {
    if (!external || !append)
      throw std::invalid_argument("invalid paired FFN external storage");
    const auto start = canonicalExternalOffset(tensor, elements * itemBytes);
    if (sidecarOffset > std::numeric_limits<std::uint64_t>::max() - sliceBytes)
      throw std::invalid_argument("invalid paired FFN external range");
    const auto copyRange = [&] (std::uint64_t position, std::uint64_t remaining) {
      while (remaining != 0) {
        const auto window = std::min(remaining, NativeCanonicalMaterialBundleMaxBytes);
        const auto bytes = external("model.onnx.data", position, window);
        if (bytes.size() != window)
          throw std::invalid_argument("short paired FFN external read");
        append(bytes);
        position += window;
        remaining -= window;
      }
    };
    if (columnBegin == 0 && columnEnd == columns) {
      copyRange(start + rowBegin * rowBytes, sliceBytes);
    }
    else {
      for (auto row = rowBegin; row < rowEnd; ++row)
        copyRange(start + (row * columns + columnBegin) * itemBytes, rowBytes);
    }
    for (auto& field : *tensor.mutable_external_data()) {
      if (field.key() == "offset") field.set_value(std::to_string(sidecarOffset));
      else if (field.key() == "length") field.set_value(std::to_string(sliceBytes));
    }
    sidecarOffset += sliceBytes;
    return;
  }
  if ((!tensor.raw_data().empty() && (tensor.raw_data().size() != elements * itemBytes ||
                                     tensor.float_data_size() != 0)) ||
      (tensor.raw_data().empty() &&
       (!isFloat || static_cast<std::uint64_t>(tensor.float_data_size()) != elements)))
    throw std::invalid_argument("invalid paired FFN initializer storage");

  const auto typedFloatBytes = tensor.raw_data().empty() ? rawBytes(tensor, {})
                                                        : std::vector<std::uint8_t>{};
  const auto* source = tensor.raw_data().empty()
    ? typedFloatBytes.data() : reinterpret_cast<const std::uint8_t*>(tensor.raw_data().data());
  std::vector<std::uint8_t> slice;
  slice.reserve(sliceBytes);
  for (auto row = rowBegin; row < rowEnd; ++row) {
    const auto offset = (row * columns + columnBegin) * itemBytes;
    slice.insert(slice.end(), source + offset, source + offset + rowBytes);
  }
  setInlineBytes(tensor, std::move(slice));
}

void
sliceInt4Projection(onnx::ModelProto& model, onnx::NodeProto& node,
                    const NativeOnnxLinearProjection& layout, bool splitK,
                    std::uint64_t begin, std::uint64_t end,
                    const NativeOnnxExternalReader& external,
                    const NativeOnnxExternalAppender& append, std::uint64_t& sidecarOffset)
{
  constexpr auto block = NativeOnnxLinearProjection::BLOCK;
  constexpr auto blob = NativeOnnxLinearProjection::BLOB_BYTES;
  const auto k = static_cast<std::uint64_t>(layout.k);
  const auto n = static_cast<std::uint64_t>(layout.n);
  if (begin >= end || end > (splitK ? k : n) ||
      (splitK && (begin % block != 0 || end % block != 0)))
    throw std::invalid_argument("invalid or unaligned packed projection range");
  const auto blockBegin = splitK ? begin / block : 0;
  const auto blockEnd = splitK ? end / block : k / block;
  const auto rowBegin = splitK ? 0 : begin;
  const auto rowEnd = splitK ? n : end;
  auto& weight = *findInitializer(model, node.input(1));
  auto& scales = *findInitializer(model, node.input(2));
  // Copy the same (output row, K block) rectangle for weights and scales.
  sliceProjectionInitializer(weight, n, k / block * blob, rowBegin, rowEnd,
    blockBegin * blob, blockEnd * blob, external, append, sidecarOffset);
  sliceProjectionInitializer(scales, n, k / block, rowBegin, rowEnd,
    blockBegin, blockEnd, external, append, sidecarOffset);
  const auto slicedRows = static_cast<std::int64_t>(rowEnd - rowBegin);
  const auto slicedBlocks = static_cast<std::int64_t>(blockEnd - blockBegin);
  weight.set_dims(0, slicedRows);
  weight.set_dims(1, slicedBlocks);
  if (scales.dims_size() == 1) {
    scales.set_dims(0, slicedRows * slicedBlocks);
  }
  else {
    scales.set_dims(0, slicedRows);
    scales.set_dims(1, slicedBlocks);
  }
  for (auto& attr : *node.mutable_attribute()) {
    if (attr.name() == (splitK ? "K" : "N"))
      attr.set_i(static_cast<std::int64_t>(end - begin));
  }
}

void
lowerFfnPair(onnx::ModelProto& model, const NativeOnnxSourceTransformV1& transform,
             const NativeOnnxExternalReader& external, const NativeOnnxExternalAppender& append)
{
  auto& graph = *model.mutable_graph();
  const bool biased = !transform.bias.empty();
  if (graph.node_size() != (biased ? 4 : 3) || graph.input_size() != 1 ||
      graph.output_size() != 1 ||
      graph.sparse_initializer_size() != 0 || graph.input(0).name() != transform.input ||
      graph.output(0).name() != transform.output)
    throw std::invalid_argument("paired FFN region has an open or unsupported boundary");

  const auto requireOp = [] (const auto& node, std::string_view op, int inputs) {
    if (!node.domain().empty() || node.op_type() != op || node.input_size() != inputs ||
        node.output_size() != 1 || node.output(0).empty() ||
        (op != "Gelu" && node.attribute_size() != 0))
      throw std::invalid_argument("paired FFN region has an unsupported operator");
  };
  auto& up = *graph.mutable_node(0);
  const auto upShape = inspectNativeOnnxLinearProjection(model, up);
  if (up.name() != transform.node || up.input(0) != transform.input ||
      up.input(1) != transform.weight)
    throw std::invalid_argument("paired FFN up projection differs from recipe");
  auto activationInput = up.output(0);
  if (biased) {
    const auto& add = graph.node(1);
    requireOp(add, "Add", 2);
    if (!((add.input(0) == activationInput && add.input(1) == transform.bias) ||
          (add.input(1) == activationInput && add.input(0) == transform.bias)))
      throw std::invalid_argument("paired FFN up bias differs from recipe");
    activationInput = add.output(0);
  }
  const auto& activation = graph.node(biased ? 2 : 1);
  requireOp(activation, "Gelu", 1);
  if (activation.attribute_size() > 1 ||
      (activation.attribute_size() == 1 &&
       (activation.attribute(0).name() != "approximate" ||
        activation.attribute(0).type() != onnx::AttributeProto::STRING ||
        (activation.attribute(0).s() != "tanh" && activation.attribute(0).s() != "none"))))
    throw std::invalid_argument("paired FFN GELU has unsupported attributes");
  auto& down = *graph.mutable_node(graph.node_size() - 1);
  const auto downShape = inspectNativeOnnxLinearProjection(model, down);
  if (activation.input(0) != activationInput || down.name() != transform.downNode ||
      down.input(0) != activation.output(0) || down.input(1) != transform.downWeight ||
      down.output(0) != transform.output)
    throw std::invalid_argument("paired FFN down projection differs from recipe");
  const auto defaultOpset = std::count_if(model.opset_import().begin(),
    model.opset_import().end(), [] (const auto& opset) { return opset.domain().empty(); });
  const auto supportsGelu = std::any_of(model.opset_import().begin(),
    model.opset_import().end(), [] (const auto& opset) {
      return opset.domain().empty() && opset.version() >= 20;
    });
  if (defaultOpset != 1 || !supportsGelu || model.functions_size() != 0)
    throw std::invalid_argument("paired FFN requires standard ONNX GELU");
  if (graph.initializer_size() != 2 + biased + upShape.packed + downShape.packed)
    throw std::invalid_argument("paired FFN has extra or missing initializers");
  std::set<std::string> valueNames{transform.input};
  for (const auto& tensor : graph.initializer()) {
    if (!valueNames.insert(tensor.name()).second)
      throw std::invalid_argument("paired FFN initializer names overlap");
  }
  for (const auto& node : graph.node()) {
    if (!valueNames.insert(node.output(0)).second)
      throw std::invalid_argument("paired FFN source values overlap");
  }

  if (transform.extent > std::numeric_limits<std::int64_t>::max())
    throw std::invalid_argument("paired FFN extent exceeds ONNX dimension limit");
  const auto extent = static_cast<std::int64_t>(transform.extent);
  if (upShape.n != extent || downShape.k != extent || upShape.k <= 0 || downShape.n <= 0)
    throw std::invalid_argument("paired FFN initializer geometry differs from recipe");
  const auto& inputType = graph.input(0).type().tensor_type();
  const auto& outputType = graph.output(0).type().tensor_type();
  const auto& inputShape = inputType.shape();
  const auto& outputShape = outputType.shape();
  const auto rank = inputShape.dim_size();
  if (inputType.elem_type() != onnx::TensorProto::FLOAT ||
      outputType.elem_type() != onnx::TensorProto::FLOAT || rank < 2 ||
      outputShape.dim_size() != rank || inputShape.dim(rank - 1).dim_value() != upShape.k ||
      outputShape.dim(rank - 1).dim_value() != downShape.n)
    throw std::invalid_argument("paired FFN port shape differs from recipe");
  for (int dim = 0; dim < rank - 1; ++dim) {
    if (inputShape.dim(dim).dim_value() <= 0 ||
        inputShape.dim(dim).dim_value() != outputShape.dim(dim).dim_value())
      throw std::invalid_argument("paired FFN batch shape differs between ports");
  }
  const auto hasExternal = hasExternalInitializers(model);
  if (!hasExternal) onnx::checker::check_model(model);
  const auto width = static_cast<std::int64_t>(transform.end - transform.begin);
  const auto begin = transform.begin;
  const auto end = transform.end;
  std::uint64_t sidecarOffset = 0;
  sliceNativeOnnxLinearProjection(model, up, NativeOnnxProjectionAxis::OUTPUT,
                                  begin, end, external, append, sidecarOffset);
  sliceNativeOnnxLinearProjection(model, down, NativeOnnxProjectionAxis::INPUT,
                                  begin, end, external, append, sidecarOffset);
  if (biased) {
    auto* bias = findInitializer(model, transform.bias);
    if (bias == nullptr || bias->data_type() != onnx::TensorProto::FLOAT ||
        bias->dims_size() != 1 || bias->dims(0) != extent)
      throw std::invalid_argument("invalid paired FFN up bias geometry");
    sliceProjectionInitializer(*bias, 1, transform.extent, 0, 1, begin, end,
                               external, append, sidecarOffset);
    bias->set_dims(0, width);
  }
  // The copied output ABI is unchanged. Re-infer the derived intermediate shapes.
  graph.clear_value_info();
  if (!hasExternal) onnx::checker::check_model(model);
}

} // namespace

NativeOnnxLinearProjection
inspectNativeOnnxLinearProjection(const onnx::ModelProto& model, const onnx::NodeProto& node)
{
  const auto& graph = model.graph();
  const auto initializer = [&] (int index) -> const onnx::TensorProto& {
    const auto it = std::find_if(graph.initializer().begin(), graph.initializer().end(),
      [&] (const auto& tensor) { return tensor.name() == node.input(index); });
    if (it == graph.initializer().end())
      throw std::invalid_argument("linear projection operand is not a constant initializer");
    return *it;
  };
  if (node.domain().empty() && node.op_type() == "MatMul") {
    if (node.input_size() != 2 || node.input(0).empty() || node.input(1).empty() ||
        node.output_size() != 1 || node.output(0).empty() || node.attribute_size() != 0)
      throw std::invalid_argument("unsupported dense MatMul operands");
    const auto& weight = initializer(1);
    if (weight.data_type() != onnx::TensorProto::FLOAT || weight.dims_size() != 2 ||
        weight.dims(0) <= 0 || weight.dims(1) <= 0)
      throw std::invalid_argument("dense MatMul requires a static FP32 matrix");
    return {weight.dims(0), weight.dims(1), false};
  }
  if (node.domain() != "com.microsoft" || node.op_type() != "MatMulNBits" ||
      node.input_size() < 3 || node.input_size() > 6 ||
      node.output_size() != 1 || node.output(0).empty() ||
      std::any_of(node.input().begin(), node.input().begin() + 3,
                  [] (const auto& name) { return name.empty(); }) ||
      std::any_of(node.input().begin() + 3, node.input().end(),
                  [] (const auto& name) { return !name.empty(); }))
    throw std::invalid_argument("unsupported symmetric MatMulNBits operands");
  const auto packedOpsets = std::count_if(model.opset_import().begin(), model.opset_import().end(),
    [] (const auto& opset) { return opset.domain() == "com.microsoft"; });
  const auto supportsPacked = std::any_of(model.opset_import().begin(), model.opset_import().end(),
    [] (const auto& opset) { return opset.domain() == "com.microsoft" && opset.version() == 1; });
  if (packedOpsets != 1 || !supportsPacked)
    throw std::invalid_argument("packed projection requires com.microsoft opset 1");
  std::map<std::string, std::int64_t> attrs;
  for (const auto& attr : node.attribute()) {
    const auto& name = attr.name();
    if (attr.type() != onnx::AttributeProto::INT ||
        (name != "K" && name != "N" && name != "bits" && name != "block_size" &&
         name != "accuracy_level") || !attrs.emplace(name, attr.i()).second)
      throw std::invalid_argument("unsupported MatMulNBits attribute");
  }
  const NativeOnnxLinearProjection result{attrs["K"], attrs["N"], true};
  constexpr auto block = NativeOnnxLinearProjection::BLOCK;
  const auto blocks = result.k / block;
  if (attrs["bits"] != 4 || attrs["block_size"] != block ||
      attrs["accuracy_level"] < 0 || attrs["accuracy_level"] > 4 ||
      result.k <= 0 || result.n <= 0 || result.k % block != 0 ||
      result.n > std::numeric_limits<std::int64_t>::max() / blocks)
    throw std::invalid_argument("invalid block-32 MatMulNBits geometry");
  const auto& weight = initializer(1);
  const auto& scales = initializer(2);
  if (weight.data_type() != onnx::TensorProto::UINT8 || weight.dims_size() != 3 ||
      weight.dims(0) != result.n || weight.dims(1) != blocks ||
      weight.dims(2) != NativeOnnxLinearProjection::BLOB_BYTES ||
      scales.data_type() != onnx::TensorProto::FLOAT ||
      !((scales.dims_size() == 1 && scales.dims(0) == result.n * blocks) ||
        (scales.dims_size() == 2 && scales.dims(0) == result.n && scales.dims(1) == blocks)))
    throw std::invalid_argument("MatMulNBits weight or scale shape differs from K/N");
  return result;
}

void
sliceNativeOnnxLinearProjection(onnx::ModelProto& model, onnx::NodeProto& node,
  NativeOnnxProjectionAxis axis, std::uint64_t begin, std::uint64_t end,
  const NativeOnnxExternalReader& external, const NativeOnnxExternalAppender& append,
  std::uint64_t& sidecarOffset)
{
  const auto geometry = inspectNativeOnnxLinearProjection(model, node);
  const bool splitK = axis == NativeOnnxProjectionAxis::INPUT;
  if (geometry.packed) {
    sliceInt4Projection(model, node, geometry, splitK, begin, end, external, append, sidecarOffset);
    return;
  }
  auto& weight = *findInitializer(model, node.input(1));
  const auto k = static_cast<std::uint64_t>(geometry.k);
  const auto n = static_cast<std::uint64_t>(geometry.n);
  sliceProjectionInitializer(weight, k, n, splitK ? begin : 0, splitK ? end : k,
    splitK ? 0 : begin, splitK ? n : end, external, append, sidecarOffset);
  weight.set_dims(splitK ? 0 : 1, static_cast<std::int64_t>(end - begin));
}

NativeOnnxRmsNormEntries
lowerNativeOnnxRmsNorm(const onnx::ModelProto& source, std::uint64_t begin,
                      std::uint64_t end, const std::string& globalSquares,
                      const NativeOnnxExternalReader& external)
{
  const auto& graph = source.graph();
  if (source.functions_size() != 0 || graph.node_size() != 6 || graph.input_size() != 1 ||
      graph.output_size() != 1 || graph.initializer_size() != 3 ||
      graph.sparse_initializer_size() != 0 || globalSquares.empty())
    throw std::invalid_argument("RMSNorm requires a complete source region with constant operands");
  const auto opset = std::find_if(source.opset_import().begin(), source.opset_import().end(),
    [] (const auto& entry) { return entry.domain().empty(); });
  if (opset == source.opset_import().end() || opset->version() < 18)
    throw std::invalid_argument("RMSNorm requires axes inputs on the standard ONNX opset");

  const auto& input = graph.input(0);
  const auto& output = graph.output(0);
  const auto& inputType = input.type().tensor_type();
  const auto& outputType = output.type().tensor_type();
  const auto rank = inputType.shape().dim_size();
  if (rank < 1 || outputType.shape().dim_size() != rank ||
      inputType.elem_type() != onnx::TensorProto::FLOAT ||
      outputType.elem_type() != onnx::TensorProto::FLOAT)
    throw std::invalid_argument("RMSNorm requires a static FP32 boundary");
  for (int axis = 0; axis < rank; ++axis) {
    const auto extent = inputType.shape().dim(axis).dim_value();
    if (extent <= 0 || outputType.shape().dim(axis).dim_value() != extent)
      throw std::invalid_argument("RMSNorm input and output shapes differ");
  }
  const auto width = static_cast<std::uint64_t>(inputType.shape().dim(rank - 1).dim_value());
  if (begin >= end || end > width || width > (1U << 24))
    throw std::invalid_argument("RMSNorm channel range is outside its exact FP32 width");

  const auto& square = graph.node(0);
  const auto& mean = graph.node(1);
  const auto& shift = graph.node(2);
  const auto& root = graph.node(3);
  const auto& divide = graph.node(4);
  const auto& scale = graph.node(5);
  const auto op = [] (const auto& node, const char* name, int inputs) {
    return node.domain().empty() && node.op_type() == name && node.input_size() == inputs &&
           node.output_size() == 1 && !node.output(0).empty() &&
           (node.op_type() == "ReduceMean" || node.attribute_size() == 0);
  };
  if (!op(square, "Mul", 2) || !op(mean, "ReduceMean", 2) || !op(shift, "Add", 2) ||
      !op(root, "Sqrt", 1) || !op(divide, "Div", 2) || !op(scale, "Mul", 2) ||
      square.input(0) != input.name() || square.input(1) != input.name() ||
      mean.input(0) != square.output(0) || root.input(0) != shift.output(0) ||
      divide.input(0) != input.name() || divide.input(1) != root.output(0) ||
      scale.output(0) != output.name())
    throw std::invalid_argument("source nodes do not form the supported RMSNorm");
  const auto otherOperand = [] (const auto& node, const std::string& value) -> const std::string& {
    if (node.input(0) == value) return node.input(1);
    if (node.input(1) == value) return node.input(0);
    throw std::invalid_argument("source RMSNorm operand has foreign lineage");
  };
  std::set<std::string> names{input.name(), globalSquares};
  if (names.size() != 2 || input.name().empty())
    throw std::invalid_argument("RMSNorm statistics overlap its source input");
  for (const auto& node : graph.node()) {
    if (!names.insert(node.output(0)).second)
      throw std::invalid_argument("RMSNorm source values overlap");
  }
  for (const auto& init : graph.initializer()) {
    if (init.name().empty() || !names.insert(init.name()).second)
      throw std::invalid_argument("RMSNorm source constants overlap");
  }
  std::set<std::string> attributes;
  for (const auto& attr : mean.attribute()) {
    if (attr.type() != onnx::AttributeProto::INT ||
        !attributes.insert(attr.name()).second ||
        !((attr.name() == "keepdims" && attr.i() == 1) ||
          (attr.name() == "noop_with_empty_axes" && attr.i() == 0)))
      throw std::invalid_argument("RMSNorm does not retain a single statistic per token");
  }
  if (!hasExternalInitializers(source)) onnx::checker::check_model(source);
  const auto constant = [&] (const std::string& name) -> const onnx::TensorProto& {
    const auto it = std::find_if(graph.initializer().begin(), graph.initializer().end(),
      [&] (const auto& tensor) { return tensor.name() == name; });
    if (it == graph.initializer().end())
      throw std::invalid_argument("RMSNorm operand is not a source constant");
    return *it;
  };
  auto axes = constant(mean.input(1));
  auto epsilon = constant(otherOperand(shift, mean.output(0)));
  const auto& gamma = constant(otherOperand(scale, divide.output(0)));
  if (axes.data_type() != onnx::TensorProto::INT64 || axes.dims_size() != 1 ||
      axes.dims(0) != 1 || epsilon.data_type() != onnx::TensorProto::FLOAT ||
      (epsilon.dims_size() != 0 && (epsilon.dims_size() != 1 || epsilon.dims(0) != 1)) ||
      gamma.data_type() != onnx::TensorProto::FLOAT || gamma.dims_size() != 1 ||
      gamma.dims(0) != static_cast<std::int64_t>(width))
    throw std::invalid_argument("RMSNorm constant shapes differ from its width");
  const auto inlineScalar = [&] (onnx::TensorProto& tensor, std::uint64_t bytes) {
    if (tensor.data_location() != onnx::TensorProto::EXTERNAL && tensor.external_data().empty())
      return;
    if (!external)
      throw std::invalid_argument("RMSNorm requires its source constant range reader");
    auto data = external("model.onnx.data", canonicalExternalOffset(tensor, bytes), bytes);
    if (data.size() != bytes)
      throw std::invalid_argument("short RMSNorm constant read");
    setInlineBytes(tensor, std::move(data));
  };
  inlineScalar(axes, sizeof(std::int64_t));
  inlineScalar(epsilon, sizeof(float));
  std::int64_t normalizedAxis = 0;
  if (axes.raw_data().size() == sizeof(normalizedAxis) && axes.int64_data_size() == 0) {
    std::memcpy(&normalizedAxis, axes.raw_data().data(), sizeof(normalizedAxis));
  }
  else if (axes.raw_data().empty() && axes.int64_data_size() == 1) {
    normalizedAxis = axes.int64_data(0);
  }
  else {
    throw std::invalid_argument("RMSNorm axes has invalid storage");
  }
  if (normalizedAxis != -1 && normalizedAxis != rank - 1)
    throw std::invalid_argument("RMSNorm statistics must reduce the channel axis");
  float epsilonValue = 0;
  if (epsilon.raw_data().size() == sizeof(epsilonValue) && epsilon.float_data_size() == 0) {
    std::memcpy(&epsilonValue, epsilon.raw_data().data(), sizeof(epsilonValue));
  }
  else if (epsilon.raw_data().empty() && epsilon.float_data_size() == 1) {
    epsilonValue = epsilon.float_data(0);
  }
  if (!std::isfinite(epsilonValue) || epsilonValue <= 0)
    throw std::invalid_argument("RMSNorm epsilon must be a positive finite FP32 scalar");

  const auto setWidth = [rank] (onnx::ValueInfoProto& port, std::uint64_t extent) {
    port.mutable_type()->mutable_tensor_type()->mutable_shape()->
      mutable_dim(rank - 1)->set_dim_value(static_cast<std::int64_t>(extent));
  };
  const auto emptyEntry = [&] (const char* suffix) {
    auto entry = source;
    auto* derived = entry.mutable_graph();
    derived->set_name(graph.name() + suffix);
    derived->clear_node();
    derived->clear_initializer();
    derived->clear_value_info();
    setWidth(*derived->mutable_input(0), end - begin);
    return entry;
  };
  NativeOnnxRmsNormEntries result{emptyEntry("-statistics"), emptyEntry("-normalization")};
  auto& statistics = *result.statistics.mutable_graph();
  *statistics.add_node() = square;
  *statistics.add_node() = mean;
  statistics.mutable_node(1)->set_op_type("ReduceSum");
  *statistics.add_initializer() = axes;
  statistics.mutable_output(0)->set_name(mean.output(0));
  setWidth(*statistics.mutable_output(0), 1);

  auto& normalization = *result.normalization.mutable_graph();
  *normalization.add_input() = statistics.output(0);
  normalization.mutable_input(1)->set_name(globalSquares);
  auto widthName = mean.output(0) + "/full-width";
  while (names.count(widthName) != 0) widthName += "_";
  auto& widthConstant = *normalization.add_initializer();
  widthConstant.set_name(widthName);
  widthConstant.set_data_type(onnx::TensorProto::FLOAT);
  widthConstant.add_float_data(static_cast<float>(width));
  auto& globalMean = *normalization.add_node();
  globalMean.set_name(mean.name());
  globalMean.set_op_type("Div");
  globalMean.add_input(globalSquares);
  globalMean.add_input(widthName);
  globalMean.add_output(mean.output(0));
  for (int index = 2; index < graph.node_size(); ++index)
    *normalization.add_node() = graph.node(index);
  *normalization.add_initializer() = epsilon;
  auto& narrowGamma = *normalization.add_initializer();
  narrowGamma = gamma;
  std::uint64_t offset = 0;
  std::vector<std::uint8_t> gammaBytes;
  sliceProjectionInitializer(narrowGamma, 1, width, 0, 1, begin, end, external,
    [&] (const auto& bytes) { gammaBytes.insert(gammaBytes.end(), bytes.begin(), bytes.end()); },
    offset);
  if (offset != 0) setInlineBytes(narrowGamma, std::move(gammaBytes));
  narrowGamma.set_dims(0, static_cast<std::int64_t>(end - begin));
  setWidth(*normalization.mutable_output(0), end - begin);
  onnx::checker::check_model(result.statistics);
  onnx::checker::check_model(result.normalization);
  return result;
}

onnx::ModelProto
lowerNativeOnnxAttentionProjections(onnx::ModelProto source, std::uint64_t begin,
  std::uint64_t end, const NativeOnnxExternalReader& external,
  const NativeOnnxExternalAppender& append)
{
  auto& graph = *source.mutable_graph();
  if (graph.output_size() != 3 || graph.sparse_initializer_size() != 0 ||
      source.functions_size() != 0 || begin >= end)
    throw std::invalid_argument("invalid Q/K/V projection source region");
  std::map<std::string, int> producers;
  for (int index = 0; index < graph.node_size(); ++index) {
    const auto& node = graph.node(index);
    if (node.output_size() != 1 || node.output(0).empty() ||
        !producers.emplace(node.output(0), index).second)
      throw std::invalid_argument("ambiguous Q/K/V projection source values");
  }

  const auto producer = [&] (const std::string& name) {
    const auto it = producers.find(name);
    if (it == producers.end())
      throw std::invalid_argument("Q/K/V projection producer is missing");
    return it->second;
  };
  struct Projection
  {
    int index;
    std::string bias;
    std::uint64_t width;
  };
  std::vector<Projection> projections;
  std::set<int> sourceNodes;
  std::set<std::string> sourceInputs;
  std::set<std::string> sourceConstants;
  for (const auto& output : graph.output()) {
    auto index = producer(output.name());
    const auto& last = graph.node(index);
    std::string bias;
    if (last.op_type() == "Add") {
      if (!last.domain().empty() || last.input_size() != 2 || last.attribute_size() != 0 ||
          !sourceNodes.insert(index).second)
        throw std::invalid_argument("invalid separate Q/K/V projection bias");
      bias = last.input(producers.count(last.input(0)) != 0 ? 1 : 0);
      const auto& projected = last.input(producers.count(last.input(0)) != 0 ? 0 : 1);
      if (bias.empty())
        throw std::invalid_argument("Q/K/V projection bias is missing");
      index = producer(projected);
    }
    const auto& node = graph.node(index);
    const auto layout = inspectNativeOnnxLinearProjection(source, node);
    const auto width = static_cast<std::uint64_t>(layout.n);
    if (end > width || (!projections.empty() && width != projections.front().width) ||
        !sourceNodes.insert(index).second)
      throw std::invalid_argument("Q/K/V do not share a complete projection channel range");
    if (!sourceConstants.insert(node.input(1)).second ||
        (layout.packed && !sourceConstants.insert(node.input(2)).second))
      throw std::invalid_argument("Q/K/V source projection constants overlap");
    if (!bias.empty()) {
      const auto* initializer = findInitializer(source, bias);
      if (initializer == nullptr || initializer->data_type() != onnx::TensorProto::FLOAT ||
          initializer->dims_size() != 1 || initializer->dims(0) != layout.n ||
          !sourceConstants.insert(bias).second)
        throw std::invalid_argument("Q/K/V bias does not match its source projection");
    }

    const auto input = std::find_if(graph.input().begin(), graph.input().end(),
      [&] (const auto& port) { return port.name() == node.input(0); });
    if (input == graph.input().end() || producers.count(input->name()) != 0)
      throw std::invalid_argument("Q/K/V projection does not consume a boundary input");
    sourceInputs.insert(input->name());
    const auto& in = input->type().tensor_type();
    const auto& out = output.type().tensor_type();
    if (in.elem_type() != onnx::TensorProto::FLOAT ||
        out.elem_type() != onnx::TensorProto::FLOAT ||
        in.shape().dim_size() != 3 || out.shape().dim_size() != 3 ||
        in.shape().dim(2).dim_value() != layout.k || out.shape().dim(2).dim_value() != layout.n)
      throw std::invalid_argument("Q/K/V projection ports have unsupported geometry");
    for (int axis = 0; axis < 2; ++axis) {
      if (in.shape().dim(axis).dim_value() <= 0 ||
          in.shape().dim(axis).dim_value() != out.shape().dim(axis).dim_value())
        throw std::invalid_argument("Q/K/V projection changed batch or token geometry");
    }
    projections.push_back({index, std::move(bias), width});
  }
  if (sourceNodes.size() != static_cast<std::size_t>(graph.node_size()) ||
      sourceInputs.size() != static_cast<std::size_t>(graph.input_size()) ||
      sourceConstants.size() != static_cast<std::size_t>(graph.initializer_size()))
    throw std::invalid_argument("Q/K/V projection has unaccounted source material");

  std::uint64_t offset = 0;
  for (const auto& projection : projections) {
    auto& node = *graph.mutable_node(projection.index);
    sliceNativeOnnxLinearProjection(source, node, NativeOnnxProjectionAxis::OUTPUT,
                                    begin, end, external, append, offset);
    if (!projection.bias.empty()) {
      auto& bias = *findInitializer(source, projection.bias);
      sliceProjectionInitializer(bias, 1, projection.width, 0, 1, begin, end,
                                 external, append, offset);
      bias.set_dims(0, static_cast<std::int64_t>(end - begin));
    }
  }
  for (auto& output : *graph.mutable_output()) {
    output.mutable_type()->mutable_tensor_type()->mutable_shape()->
      mutable_dim(2)->set_dim_value(static_cast<std::int64_t>(end - begin));
  }
  graph.clear_value_info();
  if (!hasExternalInitializers(source)) onnx::checker::check_model(source);
  return source;
}

onnx::ModelProto
lowerNativeOnnxAttentionHeads(onnx::ModelProto source, const std::string& query,
  const std::string& key, const std::string& value, std::uint64_t headBegin,
  std::uint64_t headEnd, const NativeOnnxExternalReader& external,
  const NativeOnnxExternalAppender& append, std::uint64_t sealedHeadWidth)
{
  auto& graph = *source.mutable_graph();
  if (graph.output_size() != 1 || graph.sparse_initializer_size() != 0 ||
      source.functions_size() != 0 || query.empty() || key.empty() || value.empty() ||
      query == key || query == value || key == value)
    throw std::invalid_argument("attention tail requires distinct Q/K/V inputs and one output");
  const auto opset = std::find_if(source.opset_import().begin(), source.opset_import().end(),
    [] (const auto& entry) { return entry.domain().empty(); });
  if (opset == source.opset_import().end() || opset->version() < 18)
    throw std::invalid_argument("unsupported attention tail opset");

  std::map<std::string, int> producers;
  for (int index = 0; index < graph.node_size(); ++index) {
    for (const auto& output : graph.node(index).output()) {
      if (output.empty() || !producers.emplace(output, index).second)
        throw std::invalid_argument("attention tail has ambiguous source values");
    }
  }
  const auto producer = [&] (const std::string& name) {
    const auto it = producers.find(name);
    if (it == producers.end()) throw std::invalid_argument("attention source operand is missing");
    return it->second;
  };
  std::set<int> usedNodes;
  std::set<std::string> usedInputs;
  std::set<std::string> usedConstants;
  const auto take = [&] (const std::string& id, const char* op, int inputs, int outputs = 1)
      -> const onnx::NodeProto& {
    const auto index = producer(id);
    const auto& node = graph.node(index);
    if (!node.domain().empty() || node.op_type() != op ||
        node.input_size() != inputs || node.output_size() != outputs)
      throw std::invalid_argument("unexpected operation in the attention tail");
    usedNodes.insert(index);
    return node;
  };
  const auto noAttributes = [] (const onnx::NodeProto& node) {
    if (node.attribute_size() != 0)
      throw std::invalid_argument("unsupported attribute in the attention tail");
  };
  const auto lastAxis = [] (const onnx::NodeProto& node) {
    if (node.attribute_size() != 1 || node.attribute(0).name() != "axis" ||
        node.attribute(0).type() != onnx::AttributeProto::INT || node.attribute(0).i() != -1)
      throw std::invalid_argument("attention operation must use its last axis");
  };
  const auto dimensions = [] (const onnx::ValueInfoProto& port) {
    const auto& tensor = port.type().tensor_type();
    std::vector<std::int64_t> dims;
    for (const auto& dim : tensor.shape().dim()) dims.push_back(dim.dim_value());
    if (tensor.elem_type() != onnx::TensorProto::FLOAT || dims.empty() ||
        std::any_of(dims.begin(), dims.end(), [] (auto dim) { return dim <= 0; }))
      throw std::invalid_argument("attention port must be static FP32");
    return dims;
  };
  const auto inputShape = [&] (const std::string& name) {
    const auto input = std::find_if(graph.input().begin(), graph.input().end(),
      [&] (const auto& port) { return port.name() == name; });
    if (input == graph.input().end() || producers.count(name) != 0)
      throw std::invalid_argument("attention operand is not a source input");
    usedInputs.insert(name);
    return dimensions(*input);
  };
  const auto q = inputShape(query);
  const auto k = inputShape(key);
  if (q.size() != 3 || k.size() != 3 || k != inputShape(value) ||
      q[0] != k[0] || q[2] != k[2] || dimensions(graph.output(0)) != q)
    throw std::invalid_argument("attention Q/K/V and output geometry disagree");
  const auto batch = q[0];
  const auto width = q[2];
  const auto constant = [&] (const std::string& name) -> onnx::TensorProto& {
    auto* tensor = findInitializer(source, name);
    if (tensor == nullptr || producers.count(name) != 0)
      throw std::invalid_argument("attention operand is not a source constant");
    usedConstants.insert(name);
    return *tensor;
  };
  const auto inlineConstant = [&] (onnx::TensorProto& tensor, std::uint64_t length) {
    if (tensor.data_location() != onnx::TensorProto::EXTERNAL && tensor.external_data().empty())
      return;
    if (!external) throw std::invalid_argument("attention source constant reader is missing");
    auto bytes = external("model.onnx.data", canonicalExternalOffset(tensor, length), length);
    if (bytes.size() != length) throw std::invalid_argument("short attention source constant read");
    setInlineBytes(tensor, std::move(bytes));
  };
  const auto integerList = [&] (const std::string& name, int count) {
    auto& tensor = constant(name);
    if (tensor.data_type() != onnx::TensorProto::INT64 || tensor.dims_size() != 1 ||
        tensor.dims(0) != count)
      throw std::invalid_argument("attention shape constant has unexpected geometry");
    const auto bytes = static_cast<std::size_t>(count) * sizeof(std::int64_t);
    inlineConstant(tensor, bytes);
    std::vector<std::int64_t> values(count);
    if (tensor.raw_data().size() == bytes && tensor.int64_data_size() == 0)
      std::memcpy(values.data(), tensor.raw_data().data(), bytes);
    else if (tensor.raw_data().empty() && tensor.int64_data_size() == count)
      std::copy(tensor.int64_data().begin(), tensor.int64_data().end(), values.begin());
    else
      throw std::invalid_argument("attention shape constant has invalid storage");
    return values;
  };
  const auto transpose = [&] (const std::string& id, std::vector<std::int64_t> permutation)
      -> const std::string& {
    const auto& node = take(id, "Transpose", 1);
    if (node.attribute_size() != 1 || node.attribute(0).name() != "perm" ||
        node.attribute(0).type() != onnx::AttributeProto::INTS ||
        !std::equal(permutation.begin(), permutation.end(), node.attribute(0).ints().begin(),
                    node.attribute(0).ints().end()))
      throw std::invalid_argument("attention transpose changes the head or token order");
    return node.input(0);
  };

  const auto outputIndex = producer(graph.output(0).name());
  auto& outputProjection = *graph.mutable_node(outputIndex);
  const auto geometry = inspectNativeOnnxLinearProjection(source, outputProjection);
  if (geometry.k != width || geometry.n != width)
    throw std::invalid_argument("attention output projection geometry disagrees with the heads");
  usedNodes.insert(outputIndex);
  constant(outputProjection.input(1));
  if (geometry.packed) constant(outputProjection.input(2));
  const auto& merge = take(outputProjection.input(0), "Reshape", 2);
  noAttributes(merge);
  if (integerList(merge.input(1), 3) != q)
    throw std::invalid_argument("attention merge must preserve the original token order");
  const auto& weighted = take(transpose(merge.input(0), {0, 2, 1, 3}), "MatMul", 2);
  noAttributes(weighted);
  const auto& softmax = take(weighted.input(0), "Softmax", 1);
  lastAxis(softmax);
  const auto& masked = take(softmax.input(0), "Add", 2);
  noAttributes(masked);
  const auto maskShape = inputShape(masked.input(1));
  if (maskShape.size() != 4 || (maskShape[0] != 1 && maskShape[0] != batch) ||
      maskShape[1] != 1 || (maskShape[2] != 1 && maskShape[2] != q[1]) || maskShape[3] != k[1])
    throw std::invalid_argument("attention mask must broadcast across complete heads");
  const auto& scaled = take(masked.input(0), "Mul", 2);
  noAttributes(scaled);
  auto& factor = constant(scaled.input(1));
  if (factor.data_type() != onnx::TensorProto::FLOAT ||
      (factor.dims_size() != 0 && (factor.dims_size() != 1 || factor.dims(0) != 1)))
    throw std::invalid_argument("attention score scale must be an FP32 scalar");
  inlineConstant(factor, sizeof(float));
  const auto& scores = take(scaled.input(0), "MatMul", 2);
  noAttributes(scores);
  const auto& qHeads = transpose(scores.input(0), {0, 2, 1, 3});
  const auto& kHeads = transpose(scores.input(1), {0, 2, 3, 1});
  const auto& vHeads = transpose(weighted.input(1), {0, 2, 1, 3});

  std::vector<std::string> positions;
  std::vector<std::string> splitSizes;
  const auto beforePosition = [&] (const std::string& id) -> const std::string& {
    if (graph.node(producer(id)).op_type() == "Reshape") return id;
    const auto& rotate = take(id, "Add", 2);
    const auto& direct = take(rotate.input(0), "Mul", 2);
    const auto& shifted = take(rotate.input(1), "Mul", 2);
    const auto& joined = take(shifted.input(0), "Concat", 2);
    const auto& negated = take(joined.input(0), "Neg", 1);
    const auto& halves = take(joined.input(1), "Split", 2, 2);
    noAttributes(rotate); noAttributes(direct); noAttributes(shifted); noAttributes(negated);
    lastAxis(joined); lastAxis(halves);
    if (halves.input(0) != direct.input(0) ||
        joined.input(1) != halves.output(0) || negated.input(0) != halves.output(1))
      throw std::invalid_argument("attention position transform crosses head boundaries");
    positions.push_back(direct.input(1));
    positions.push_back(shifted.input(1));
    splitSizes.push_back(halves.input(1));
    return direct.input(0);
  };
  const auto& qReshape = take(beforePosition(qHeads), "Reshape", 2);
  const auto& kReshape = take(beforePosition(kHeads), "Reshape", 2);
  const auto& vReshape = take(vHeads, "Reshape", 2);
  const auto shape = integerList(qReshape.input(1), 4);
  const auto heads = shape[2];
  const auto headWidth = shape[3];
  if (heads < 2 || headWidth <= 0 || width % headWidth != 0 || width / headWidth != heads ||
      headBegin >= headEnd || headEnd > static_cast<std::uint64_t>(heads) ||
      (sealedHeadWidth != 0 && sealedHeadWidth != static_cast<std::uint64_t>(headWidth)))
    throw std::invalid_argument("invalid complete-head range");
  if (!positions.empty()) {
    if (positions.size() != 4 || q[1] != k[1] || positions[0] != positions[2] ||
        positions[1] != positions[3] || positions[0] == positions[1] || headWidth % 2 != 0)
      throw std::invalid_argument("attention RoPE must match on Q and K");
    for (const auto& position : {positions[0], positions[1]}) {
      const auto dims = inputShape(position);
      if (dims.size() != 4 || (dims[0] != 1 && dims[0] != batch) ||
          dims[1] != q[1] || dims[2] != 1 || dims[3] != headWidth)
        throw std::invalid_argument("attention position must broadcast across complete heads");
    }
    for (const auto& name : splitSizes)
      if (integerList(name, 2) != std::vector<std::int64_t>{headWidth / 2, headWidth / 2})
        throw std::invalid_argument("attention position does not rotate within one head");
  }

  const auto localHeads = static_cast<std::int64_t>(headEnd - headBegin);
  const auto localWidth = localHeads * headWidth;
  std::map<std::string, std::vector<std::int64_t>> replacements;
  const auto reshape = [&] (const auto& node, const std::string& name, std::int64_t tokens) {
    noAttributes(node);
    if (node.input(0) != name ||
        integerList(node.input(1), 4) != std::vector<std::int64_t>{batch, tokens, heads, headWidth})
      throw std::invalid_argument("attention reshape differs from its projected source operand");
    replacements.emplace(node.input(1),
                         std::vector<std::int64_t>{batch, tokens, localHeads, headWidth});
  };
  reshape(qReshape, query, q[1]);
  reshape(kReshape, key, k[1]);
  reshape(vReshape, value, k[1]);
  replacements.emplace(merge.input(1), std::vector<std::int64_t>{batch, q[1], localWidth});
  if (usedNodes.size() != static_cast<std::size_t>(graph.node_size()) ||
      usedInputs.size() != static_cast<std::size_t>(graph.input_size()) ||
      usedConstants.size() != static_cast<std::size_t>(graph.initializer_size()))
    throw std::invalid_argument("attention tail has unaccounted source material");

  for (const auto& [name, values] : replacements) {
    auto& tensor = constant(name);
    tensor.clear_raw_data();
    tensor.clear_int64_data();
    for (const auto dim : values) tensor.add_int64_data(dim);
  }
  for (auto& port : *graph.mutable_input()) {
    if (port.name() == query || port.name() == key || port.name() == value) {
      port.mutable_type()->mutable_tensor_type()->mutable_shape()->
        mutable_dim(2)->set_dim_value(localWidth);
    }
  }
  std::uint64_t offset = 0;
  sliceNativeOnnxLinearProjection(source, outputProjection, NativeOnnxProjectionAxis::INPUT,
    headBegin * headWidth, headEnd * headWidth, external, append, offset);
  graph.clear_value_info();
  if (!hasExternalInitializers(source)) onnx::checker::check_model(source);
  return source;
}

NativeCanonicalMaterializedFiles
materializeNativeOnnxEntrySource(const NativeCanonicalSource& source,
  const NativeOnnxSourceEntryRecipeV1& entry, const std::vector<std::uint64_t>& sourceOrdinals,
  const NativeExecutionStepSpec& call, const NativeAssemblyControl& control,
  const NativeOnnxExternalAppender& append)
{
  control.requireActive();
  entry.transform.validate();
  if (!source.materialManifest ||
      (entry.transform.kind != "ffn-pair-partial" && !entry.transform.isAttention()) ||
      call.operation != "CALL_SUBGRAPH" || !call.localStateSlots.empty() ||
      call.artifactEntry != entry.entryId || call.inputSlots.empty() ||
      call.outputSlots.empty() || entry.sourceNodes.size() != sourceOrdinals.size())
    throw std::invalid_argument("source entry has no admitted material or CALL");
  for (std::size_t index = 0; index < sourceOrdinals.size(); ++index) {
    if (entry.sourceNodes[index] != "onnx-node-" + std::to_string(sourceOrdinals[index]))
      throw std::invalid_argument("source entry ordinals differ from admitted nodes");
  }
  if (entry.transform.kind == "ffn-pair-partial" &&
      (call.inputSlots.size() != 1 || call.inputSlots.front().slotId != entry.transform.input ||
       call.outputSlots.size() != 1 || call.outputSlots.front().slotId != entry.transform.output))
    throw std::invalid_argument("paired FFN CALL port differs from the source recipe");
  const auto ports = [] (const std::vector<NativeStepSlotSpec>& slots) {
    std::vector<NativeAssemblyTensorContractV3> result;
    result.reserve(slots.size());
    for (const auto& slot : slots) result.push_back({slot.slotId, slot.dtype, slot.shape});
    return result;
  };
  const auto inputs = ports(entry.transform.originalPorts(call.inputSlots, false));
  const auto outputs = ports(entry.transform.originalPorts(call.outputSlots, true));
  auto selected = selectNativeCanonicalMaterialPayloads(*source.materialManifest, sourceOrdinals,
    {}, control.requireActive);
  NativeCanonicalSource entrySource;
  entrySource.materialManifest = source.materialManifest;
  std::uint64_t selectedBytes = 0;
  for (const auto& payload : source.materialPayloads) {
    control.requireActive();
    if (selected.erase(payload.payloadId) == 0) continue;
    const auto bytes = payload.byteSize();
    if (bytes > control.maxAssembledBytes - selectedBytes)
      throw std::runtime_error("DI_CANONICAL_SOURCE_ENTRY_MATERIAL_LIMIT");
    selectedBytes += bytes;
    // Provider payloads refer to authenticated ranges in the same spool.
    // Releasing this entry leaves the next entry's range owner intact.
    entrySource.materialPayloads.push_back(payload);
  }
  if (!selected.empty())
    throw std::runtime_error("DI_CANONICAL_SOURCE_ENTRY_MATERIAL_MISSING");
  return materializeNativeCanonicalModelFiles(entrySource, sourceOrdinals, inputs, outputs,
                                               control, append);
}

onnx::ModelProto
lowerNativeMaterializedFfnEntry(onnx::ModelProto source,
                                const NativeOnnxSourceTransformV1& transform,
                                const std::vector<std::uint64_t>& sourceOrdinals,
                                const NativeOnnxExternalReader& external,
                                const NativeOnnxExternalAppender& append)
{
  transform.validate();
  const auto expectedNodes = transform.bias.empty() ? 3U : 4U;
  if (transform.kind != "ffn-pair-partial" || sourceOrdinals.size() != expectedNodes ||
      static_cast<std::size_t>(source.graph().node_size()) != expectedNodes ||
      transform.node != "onnx-node-" + std::to_string(sourceOrdinals.front()) ||
      transform.downNode != "onnx-node-" + std::to_string(sourceOrdinals.back()))
    throw std::invalid_argument("paired FFN projection ordinals differ from recipe");
  for (std::size_t index = 1; index < sourceOrdinals.size(); ++index) {
    if (sourceOrdinals[index] <= sourceOrdinals[index - 1] ||
        sourceOrdinals[index] - sourceOrdinals[index - 1] != 1)
      throw std::invalid_argument("paired FFN projection source window is not contiguous");
  }
  auto& graph = *source.mutable_graph();
  graph.mutable_node(0)->set_name(transform.node);
  graph.mutable_node(graph.node_size() - 1)->set_name(transform.downNode);
  if (graph.name().empty()) graph.set_name("source-transform");
  lowerFfnPair(source, transform, external, append);
  return source;
}

onnx::ModelProto
applyNativeOnnxSourceTransform(const onnx::ModelProto& source,
                               const NativeOnnxSourceTransformV1& transform,
                               const NativeOnnxExternalReader& external,
                               const NativeOnnxExternalAppender& append)
{
  transform.validate();
  onnx::ModelProto result = source;
  auto* graph = result.mutable_graph();
  if (graph->name().empty()) graph->set_name("source-transform");

  if (transform.kind == "ffn-pair-partial") {
    lowerFfnPair(result, transform, external, append);
    return result;
  }
  if (transform.kind == "attention-qkv") {
    std::set<std::string> outputs;
    for (const auto& port : graph->output()) outputs.insert(port.name());
    if (outputs != std::set<std::string>(transform.attentionPorts.begin(),
                                         transform.attentionPorts.end()))
      throw std::invalid_argument("source Q/K/V output names differ from the sealed recipe");
    return lowerNativeOnnxAttentionProjections(std::move(result),
      transform.begin * transform.headWidth, transform.end * transform.headWidth, external, append);
  }
  if (transform.kind == "attention-heads") {
    return lowerNativeOnnxAttentionHeads(std::move(result), transform.attentionPorts[0],
      transform.attentionPorts[1], transform.attentionPorts[2], transform.begin, transform.end,
      external, append, transform.headWidth);
  }
  if (transform.isRmsNorm()) {
    if (graph->input_size() != 1 || graph->output_size() != 1 ||
        graph->input(0).name() != transform.input || graph->output(0).name() != transform.output)
      throw std::invalid_argument("source RMSNorm boundary differs from the sealed region");
    const auto& shape = graph->input(0).type().tensor_type().shape();
    if (shape.dim_size() != 3 ||
        shape.dim(2).dim_value() != static_cast<std::int64_t>(transform.extent * transform.headWidth))
      throw std::invalid_argument("source RMSNorm width differs from the sealed heads");
    auto derived = lowerNativeOnnxRmsNorm(result, transform.begin * transform.headWidth,
      transform.end * transform.headWidth, transform.globalStatistic, external);
    if (derived.statistics.graph().output(0).name() != transform.statistic)
      throw std::invalid_argument("source RMSNorm statistic differs from the sealed port");
    return transform.kind == "attention-rms-stat" ? std::move(derived.statistics)
                                                   : std::move(derived.normalization);
  }
  if (transform.kind == "attention-source-component") return result;

  if (transform.kind == "conv-input-channel-partial") {
    auto* conv = findNode(result, transform.node);
    if (conv == nullptr || conv->op_type() != "Conv")
      throw std::invalid_argument("source transform Conv node is missing");
    if (conv->input_size() < 2 || conv->input(0) != transform.input)
      throw std::invalid_argument("source transform Conv input differs");
    // The transform must describe this exact Conv: operand order is part of the
    // source binding, not a catalog hint. A different weight or a dropped fused
    // bias would compute a different function while looking plausible.
    if (conv->input(1) != transform.weight)
      throw std::invalid_argument("source transform weight is not the Conv operand");
    if (!transform.bias.empty()) {
      if (conv->input_size() < 3 || conv->input(2) != transform.bias)
        throw std::invalid_argument("source transform bias is not the Conv operand");
    }
    else if (conv->input_size() > 2) {
      throw std::invalid_argument("source transform drops an undeclared Conv bias");
    }
    int group = 1;
    for (const auto& attribute : conv->attribute())
      if (attribute.name() == "group") group = static_cast<int>(attribute.i());
    if (group != 1)
      throw std::invalid_argument("source transform requires group-1 Conv");
    if (conv->output_size() != 1 || conv->output(0) != transform.output)
      throw std::invalid_argument("source transform Conv output differs");
    auto* weight = findInitializer(result, transform.weight);
    if (weight == nullptr || weight->dims_size() != 4)
      throw std::invalid_argument("source transform Conv weight is missing or not 4-D");
    if (static_cast<std::uint64_t>(weight->dims(1)) != transform.extent)
      throw std::invalid_argument("source transform extent differs from the Conv weight");
    const auto inChannels = static_cast<std::uint64_t>(weight->dims(1));
    if (transform.end > inChannels || transform.begin >= transform.end)
      throw std::invalid_argument("source transform channel range is outside the weight");

    // Insert the rank-local channel slice immediately before its Conv.
    const std::string prefix = transform.node + "/source-transform/";
    const std::string startsName = prefix + "starts";
    const std::string endsName = prefix + "ends";
    const std::string axesName = prefix + "axes";
    const std::string sliceName = prefix + "input";
    auto addScalar = [&] (const std::string& name, std::int64_t value) {
      auto* initializer = graph->add_initializer();
      initializer->set_name(name);
      initializer->set_data_type(onnx::TensorProto::INT64);
      initializer->add_dims(1);
      initializer->add_int64_data(value);
    };
    addScalar(startsName, static_cast<std::int64_t>(transform.begin));
    addScalar(endsName, static_cast<std::int64_t>(transform.end));
    addScalar(axesName, 1);

    onnx::NodeProto slice;
    slice.set_name(prefix + "slice");
    slice.set_op_type("Slice");
    slice.add_input(transform.input);
    slice.add_input(startsName);
    slice.add_input(endsName);
    slice.add_input(axesName);
    slice.add_output(sliceName);

    std::vector<onnx::NodeProto> nodes;
    nodes.reserve(graph->node_size() + 1);
    bool inserted = false;
    for (const auto& node : graph->node()) {
      if (node.name() == transform.node && !inserted) {
        nodes.push_back(slice);
        inserted = true;
      }
      nodes.push_back(node);
    }
    if (!inserted) throw std::invalid_argument("source transform Conv node was not copied");
    graph->clear_node();
    for (auto& node : nodes) *graph->add_node() = std::move(node);

    conv = findNode(result, transform.node);
    const auto biasName = conv->input_size() > 2 ? conv->input(2) : std::string{};
    conv->set_input(0, sliceName);
    if (!biasName.empty()) {
      conv->clear_input();
      conv->add_input(sliceName);
      conv->add_input(transform.weight);
    }
    // Do not rewrite the source geometry: verify the declaration matches the
    // Conv that will actually execute.
    const auto verifyInts = [&] (const char* name, const std::vector<std::int64_t>& expected) {
      if (expected.empty()) return;
      // ONNX defaults apply when the attribute is absent: pads to zero, strides
      // and dilations to one. Compare against the effective geometry.
      std::vector<std::int64_t> actual;
      bool present = false;
      for (const auto& attribute : conv->attribute())
        if (attribute.name() == name) {
          actual.assign(attribute.ints().begin(), attribute.ints().end());
          present = true;
        }
      if (!present) {
        if (std::string(name) == "pads") actual.assign(expected.size(), 0);
        else actual.assign(expected.size(), 1);
      }
      if (actual != expected)
        throw std::invalid_argument(std::string("source transform ") + name +
                                    " differs from the Conv attribute");
    };
    verifyInts("pads", transform.pads);
    verifyInts("strides", transform.strides);
    verifyInts("dilations", transform.dilations);

    // Slice the weight along the input-channel axis (axis 1) into rank bytes.
    weight = findInitializer(result, transform.weight);
    auto full = rawBytes(*weight, external);
    const auto channels = static_cast<std::uint64_t>(weight->dims(1));
    const auto kernel = static_cast<std::uint64_t>(weight->dims(2)) *
                        static_cast<std::uint64_t>(weight->dims(3));
    const auto outChannels = static_cast<std::uint64_t>(weight->dims(0));
    const auto width = transform.end - transform.begin;
    const auto block = kernel * 4;
    std::vector<std::uint8_t> sliced(static_cast<std::size_t>(outChannels * width * block));
    for (std::uint64_t channel = 0; channel < outChannels; ++channel) {
      const auto base = (channel * channels + transform.begin) * block;
      std::memcpy(sliced.data() + static_cast<std::size_t>(channel * width * block),
                  full.data() + static_cast<std::size_t>(base),
                  static_cast<std::size_t>(width * block));
    }
    weight->set_dims(1, static_cast<std::int64_t>(width));
    setInlineBytes(*weight, std::move(sliced));
    return result;
  }

  if (transform.kind == "add-conv-bias") {
    auto* bias = findInitializer(result, transform.bias);
    if (bias == nullptr)
      throw std::invalid_argument("source transform bias initializer is missing");
    if (bias->dims_size() == 1) {
      // A fused Conv bias is commonly exported as [C]. Reshape it in place to
      // the [1,C,1,1] broadcast Add needs; the bytes are unchanged.
      const auto channels = bias->dims(0);
      bias->clear_dims();
      bias->add_dims(1);
      bias->add_dims(channels);
      bias->add_dims(1);
      bias->add_dims(1);
    }
    if (bias->dims_size() != 4 || bias->dims(0) != 1)
      throw std::invalid_argument("source transform bias is not [1,C,1,1]");
    // A source graph that already has an explicit bias Add needs no rewrite;
    // only a fused-bias Conv export requires the synthesized Add.
    for (const auto& node : graph->node()) {
      if (node.op_type() == "Add" && node.input_size() == 2 &&
          node.input(0) == transform.output && node.input(1) == transform.bias)
        return result;
    }
    const std::string rewritten = transform.output + "/bias-added";
    onnx::NodeProto add;
    add.set_name(transform.node + "/source-transform/bias");
    add.set_op_type("Add");
    add.add_input(transform.output);
    add.add_input(transform.bias);
    add.add_output(rewritten);
    // Insert immediately before the first consumer so the derived graph stays
    // topologically sorted; appending it last would place the producer after
    // its readers and ONNX rejects the model.
    std::vector<onnx::NodeProto> nodes;
    nodes.reserve(graph->node_size() + 1);
    bool inserted = false;
    for (const auto& node : graph->node()) {
      bool consumes = false;
      for (int input = 0; input < node.input_size(); ++input)
        if (node.input(input) == transform.output) consumes = true;
      if (consumes && !inserted) {
        nodes.push_back(add);
        inserted = true;
      }
      auto copy = node;
      for (int input = 0; input < copy.input_size(); ++input)
        if (copy.input(input) == transform.output) copy.set_input(input, rewritten);
      nodes.push_back(std::move(copy));
    }
    if (!inserted) throw std::invalid_argument(
      "source transform bias has no consumer to precede");
    graph->clear_node();
    for (auto& node : nodes) *graph->add_node() = std::move(node);
    for (auto& output : *graph->mutable_output())
      if (output.name() == transform.output) output.set_name(rewritten);
    // Do not rename the source value_info: `transform.output` may be a graph
    // input, in which case the synthesized Add still consumes that exact name.
    // External initializers live in the protected sidecar, not on disk at the
    // path their metadata names. The structural rewrite above is complete; a
    // checker that insists on loading external bytes would reject a valid role.
    if (!hasExternalInitializers(result)) onnx::checker::check_model(result);
    return result;
  }

  throw std::invalid_argument("source transform kind is not supported");
}

} // namespace ndnsf::di
