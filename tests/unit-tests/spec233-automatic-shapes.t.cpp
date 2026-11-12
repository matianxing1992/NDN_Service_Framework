#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-model.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/onnx-tensor-type.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::di::tests {
namespace {

NativeAssemblyControl
shapeControl()
{
  return {std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {},
          8U << 20, 8U << 20};
}

DimensionConstraint
fixed(std::int64_t extent)
{
  return {DimensionConstraint::Kind::FIXED, extent, {}, static_cast<std::uint64_t>(extent)};
}

DimensionConstraint
symbol(const std::string& name)
{
  return {DimensionConstraint::Kind::SYMBOL, 0, "graph/" + name, {}};
}

AutomaticTensorContract
tensor(const std::string& name, std::vector<DimensionConstraint> dimensions)
{
  return {"graph/" + name, 1, TensorElementType::Float32, std::move(dimensions), {"fixture"}};
}

AutomaticTensorContract
integerTensor(const std::string& name, std::vector<DimensionConstraint> dimensions)
{
  auto result = tensor(name, std::move(dimensions));
  result.onnxElementType = 7; result.wireElementType = TensorElementType::Int64;
  return result;
}

AutomaticOnnxOperatorFact
operation(const std::string& type, std::vector<std::string> inputs,
          std::vector<std::string> outputs)
{
  AutomaticOnnxOperatorFact result;
  result.scope = "graph"; result.opType = type; result.opset = 13;
  result.inputs = std::move(inputs); result.outputs = std::move(outputs);
  return result;
}

AutomaticOnnxContract
contract(std::vector<AutomaticTensorContract> tensors,
         std::vector<AutomaticOnnxOperatorFact> operators,
         std::vector<std::string> inputs, std::vector<std::string> outputs)
{
  AutomaticOnnxContract result;
  result.sourceDigest = "sha256:" + std::string(64, '0');
  std::sort(tensors.begin(), tensors.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  result.tensors = std::move(tensors); result.operators = std::move(operators);
  for (std::size_t i = 0; i < result.operators.size(); ++i) result.operators[i].nodeIndex = i;
  for (const auto& name : inputs) result.modelInputs.push_back("graph/" + name);
  for (const auto& name : outputs) result.modelOutputs.push_back("graph/" + name);
  result.factsDigest = nativePlanningDigest(result.factsJson());
  result.relations = deriveAutomaticOnnxShapeRelations(result, shapeControl());
  return result;
}

AutomaticOnnxContract
concatContract()
{
  auto op = operation("Concat", {"a", "b"}, {"joined"});
  op.integerAttributes["axis"] = -2;
  return contract({tensor("a", {symbol("B"), fixed(8), symbol("P"), fixed(128)}),
                   tensor("b", {symbol("B"), fixed(8), symbol("S"), fixed(128)}),
                   tensor("joined", {symbol("B"), fixed(8), {}, fixed(128)})},
                  {op}, {"a", "b"}, {"joined"});
}

NamedTensor
value(const std::string& name, std::vector<std::int64_t> shape)
{
  std::uint64_t count = 1;
  for (const auto dim : shape) {
    if (dim < 0 || dim > 4096) throw std::invalid_argument("fixture extent");
    count *= static_cast<std::uint64_t>(dim);
  }
  if (count > (1U << 18)) throw std::invalid_argument("fixture allocation");
  return {name, TensorElementType::Float32, std::move(shape), std::vector<std::uint8_t>(count * 4)};
}

ExecutionShapeEnvelope
bind(const AutomaticOnnxContract& model, const std::vector<NamedTensor>& inputs)
{
  return bindAutomaticOnnxShapes(model, inputs, 0, 2048, shapeControl());
}

void
checkExtent(const ExecutionShapeEnvelope& envelope, const std::string& name,
            std::size_t axis, std::uint64_t expected)
{
  const auto& bounds = envelope.tensors.at("graph/" + name);
  BOOST_REQUIRE(bounds.maximumDimensions);
  BOOST_REQUIRE(bounds.maximumDimensions->at(axis));
  BOOST_CHECK_EQUAL(*bounds.maximumDimensions->at(axis), expected);
  BOOST_REQUIRE(bounds.minimumDimensions);
  BOOST_CHECK_EQUAL(bounds.minimumDimensions->at(axis), expected);
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec233AutomaticShapes)

BOOST_AUTO_TEST_CASE(PreservesTheOriginalFactCanonicalBytesAndContractRoundTrip)
{
  const auto model = contract({tensor("x", {symbol("B"), fixed(128)}),
                               tensor("y", {symbol("B"), fixed(128)})},
    {operation("Identity", {"x"}, {"y"})}, {"x"}, {"y"});
  const auto golden = std::string(R"({"operators":[{"atomicBody":false,"attributeReferences":{},"domain":"","functionScope":"","index":0,"inputs":["x"],"integerLists":{},"integers":{},"op":"Identity","opset":13,"outputs":["y"],"overload":"","scope":"graph"}],"quantization":[],"schema":"ndnsf-di-automatic-facts-v1","sourceDigest":"sha256:)") +
    std::string(64, '0') + R"("})";
  BOOST_CHECK_EQUAL(model.factsJson(), golden);
  const auto encoded = model.canonicalJson(shapeControl());
  const auto restored = AutomaticOnnxContract::fromCanonicalJson(encoded, shapeControl());
  BOOST_CHECK_EQUAL(restored.canonicalJson(shapeControl()), encoded);
  BOOST_CHECK_EQUAL(restored.digest(shapeControl()), nativePlanningDigest(encoded));
}

BOOST_AUTO_TEST_CASE(RejectsUnknownVersionUnknownFieldsNoncanonicalAndLossyNumbers)
{
  const auto model = concatContract();
  const auto encoded = model.canonicalJson(shapeControl());
  const auto original = nativeParseJson(encoded);
  std::vector<NativeJson> bad;
  auto item = original; item["schema"] = "ndnsf-di-automatic-onnx-contract-v99"; bad.push_back(item);
  item = original; item["extra"] = true; bad.push_back(item);
  item = original; item["tensors"][0]["extra"] = true; bad.push_back(item);
  item = original; item["facts"]["operators"][0]["index"] = -1; bad.push_back(item);
  item = original; item["facts"]["operators"][0]["opset"] = 13.0; bad.push_back(item);
  item = original; item["tensors"][0]["onnxType"] = 4294967297ULL; bad.push_back(item);
  item = original; item["tensors"][0]["wireType"] = "bool"; bad.push_back(item);
  item = original; item["tensors"][0]["dimensions"][0]["kind"] = "fixed"; bad.push_back(item);
  item = original; item["relations"][0]["outputs"][0]["axis"] = 99; bad.push_back(item);
  item = original; item["relations"][0]["constant"] = 1; bad.push_back(item);
  item = original; item["factsDigest"] = "sha256:" + std::string(64, '1'); bad.push_back(item);
  item = original; item["generation"] = NativeJson::object(); bad.push_back(item);
  for (const auto& mutation : bad) {
    BOOST_CHECK_THROW(AutomaticOnnxContract::fromCanonicalJson(nativeCanonicalJson(mutation),
                                                              shapeControl()), std::exception);
  }
  BOOST_CHECK_THROW(AutomaticOnnxContract::fromCanonicalJson(encoded + "\n", shapeControl()),
                    std::exception);
  BOOST_CHECK_THROW(AutomaticOnnxContract::fromCanonicalJson("{\"schema\":1,\"schema\":2}",
                                                            shapeControl()), std::exception);
}

BOOST_AUTO_TEST_CASE(ConcatProvesPastPlusSequenceWithoutParsingSymbolSpelling)
{
  auto model = concatContract();
  for (auto& t : model.tensors) for (auto& d : *t.dimensions)
    if (d.kind == DimensionConstraint::Kind::SYMBOL && d.symbol != "graph/B")
      d.symbol = t.name + "/unrelated-label";
  const auto result = bind(model, {value("a", {1, 8, 0, 128}), value("b", {1, 8, 3, 128})});
  checkExtent(result, "joined", 2, 3);
  BOOST_REQUIRE(result.tensors.at("graph/joined").maxBytes);
  BOOST_CHECK_EQUAL(*result.tensors.at("graph/joined").maxBytes, 12288);
  validateAutomaticOnnxTensors(model, result, model.modelOutputs,
                               {value("joined", {1, 8, 3, 128})}, shapeControl());
  BOOST_CHECK_THROW(validateAutomaticOnnxTensors(model, result, model.modelOutputs,
    {value("joined", {1, 8, 2, 128})}, shapeControl()), std::invalid_argument);
  BOOST_CHECK_THROW(validateAutomaticOnnxTensors(model, result, model.modelOutputs,
    {value("joined", {1, 8, 4, 128})}, shapeControl()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RejectsSymbolConflictsMissingDuplicateExtraAndWrongTypeInputs)
{
  const auto model = concatContract();
  const auto a = value("a", {1, 8, 0, 128}), b = value("b", {1, 8, 1, 128});
  BOOST_CHECK_THROW(bind(model, {a}), std::invalid_argument);
  BOOST_CHECK_THROW(bind(model, {a, a}), std::invalid_argument);
  BOOST_CHECK_THROW(bind(model, {a, b, value("extra", {})}), std::invalid_argument);
  BOOST_CHECK_THROW(bind(model, {a, value("b", {2, 8, 1, 128})}), std::invalid_argument);
  auto wrong = b; wrong.elementType = TensorElementType::Int32;
  BOOST_CHECK_THROW(bind(model, {a, wrong}), std::invalid_argument);
  wrong = b; wrong.payload.pop_back();
  BOOST_CHECK_THROW(bind(model, {a, wrong}), std::invalid_argument);
  BOOST_CHECK_THROW(bind(model, {a, value("b", {1, 8, 128})}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(MatMulUsesActualProjectionWidthAndExactCodecFraming)
{
  const auto model = contract({tensor("a", {symbol("B"), symbol("S"), fixed(32)}),
    tensor("b", {fixed(32), fixed(128)}), tensor("out", {symbol("B"), {}, fixed(128)})},
    {operation("MatMul", {"a", "b"}, {"out"})}, {"a"}, {"out"});
  const auto result = bind(model, {value("a", {1, 3, 32})});
  checkExtent(result, "out", 1, 3); checkExtent(result, "out", 2, 128);
  const auto& bounds = result.tensors.at("graph/out");
  BOOST_REQUIRE(bounds.maxBytes); BOOST_CHECK_EQUAL(*bounds.maxBytes, 1536);
  BOOST_REQUIRE(bounds.maxEncodedBytes);
  BOOST_CHECK_EQUAL(*bounds.maxEncodedBytes, 12 + 20 + 3 + 3 * 8 + 1536);
  BOOST_CHECK_THROW(bind(model, {value("a", {1, 3, 16})}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(GemmRespectsTransposedOperandsAndUnidirectionalBias)
{
  auto gemm = operation("Gemm", {"a", "b", "c"}, {"y"});
  gemm.integerAttributes["transA"] = 1; gemm.integerAttributes["transB"] = 1;
  const auto model = contract({tensor("a", {fixed(4), fixed(3)}),
    tensor("b", {fixed(5), fixed(4)}), tensor("c", {fixed(5)}), tensor("y", {{}, {}})},
    {gemm}, {"a"}, {"y"});
  const auto result = bind(model, {value("a", {4, 3})});
  checkExtent(result, "y", 0, 3); checkExtent(result, "y", 1, 5);
  auto bad = model;
  bad.tensors[2].dimensions->front() = fixed(6);
  BOOST_CHECK_THROW(bind(bad, {value("a", {4, 3})}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NBitsReadsLogicalProjectionWidthsInsteadOfPackedStorageAxes)
{
  auto op = operation("MatMulNBits", {"a", "packed", "scales"}, {"y"});
  op.domain = "com.microsoft"; op.opset = 1;
  op.integerAttributes = {{"K", 33}, {"N", 3}, {"bits", 4}, {"block_size", 32}};
  auto packed = tensor("packed", {fixed(3), fixed(2), fixed(16)});
  packed.onnxElementType = 2; packed.wireElementType = TensorElementType::UInt8;
  auto model = contract({tensor("a", {symbol("B"), symbol("S"), fixed(33)}), packed,
    tensor("scales", {fixed(6)}), tensor("y", {{}, {}, {}})}, {op}, {"a"}, {"y"});
  QuantizedOperatorFact quant;
  quant.bits = 4; quant.blockSize = 32; quant.implicitZeroPoint = 8;
  model.quantization.push_back(quant); model.factsDigest = nativePlanningDigest(model.factsJson());
  const auto result = bind(model, {value("a", {1, 2, 33})});
  checkExtent(result, "y", 2, 3);
  BOOST_CHECK_EQUAL(*result.tensors.at("graph/y").maxBytes, 24);
  BOOST_CHECK_EQUAL(*result.tensors.at("graph/packed").maxBytes, 96);
  const auto restored = AutomaticOnnxContract::fromCanonicalJson(model.canonicalJson(shapeControl()),
                                                               shapeControl());
  BOOST_REQUIRE_EQUAL(restored.quantization.size(), 1);
  BOOST_CHECK_EQUAL(*restored.quantization[0].bits, 4);
}

BOOST_AUTO_TEST_CASE(MatMulVectorAndZeroContractionHaveCorrectOutputRankAndBytes)
{
  const auto scalar = contract({tensor("a", {fixed(4)}), tensor("b", {fixed(4)}), tensor("y", {})},
    {operation("MatMul", {"a", "b"}, {"y"})}, {"a", "b"}, {"y"});
  auto result = bind(scalar, {value("a", {4}), value("b", {4})});
  BOOST_REQUIRE(result.tensors.at("graph/y").maxBytes);
  BOOST_CHECK_EQUAL(*result.tensors.at("graph/y").maxBytes, 4);
  const auto empty = contract({tensor("a", {fixed(2), fixed(0)}),
    tensor("b", {fixed(0), fixed(3)}), tensor("y", {{}, {}})},
    {operation("MatMul", {"a", "b"}, {"y"})}, {"a"}, {"y"});
  result = bind(empty, {value("a", {2, 0})});
  checkExtent(result, "y", 0, 2); checkExtent(result, "y", 1, 3);
  BOOST_CHECK_EQUAL(*result.tensors.at("graph/y").maxBytes, 24);
}

BOOST_AUTO_TEST_CASE(BroadcastZeroWithOneStaysZeroAndIncompatibleBatchesFail)
{
  const auto zero = contract({tensor("a", {fixed(0), fixed(8)}),
    tensor("b", {fixed(1), fixed(8)}), tensor("y", {{}, fixed(8)})},
    {operation("Add", {"a", "b"}, {"y"})}, {"a", "b"}, {"y"});
  const auto result = bind(zero, {value("a", {0, 8}), value("b", {1, 8})});
  checkExtent(result, "y", 0, 0);
  BOOST_CHECK_EQUAL(*result.tensors.at("graph/y").maxBytes, 0);
  const auto bad = contract({tensor("a", {fixed(2), fixed(3), fixed(4)}),
    tensor("b", {fixed(5), fixed(4), fixed(6)}), tensor("y", {{}, fixed(3), fixed(6)})},
    {operation("MatMul", {"a", "b"}, {"y"})}, {"a"}, {"y"});
  BOOST_CHECK_THROW(bind(bad, {value("a", {2, 3, 4})}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ReshapeSolvesTheElementProductAndRejectsNondivisibleShapes)
{
  const auto model = contract({tensor("a", {symbol("B"), symbol("S"), fixed(128)}),
    integerTensor("target", {fixed(2)}), tensor("y", {{}, fixed(32)})},
    {operation("Reshape", {"a", "target"}, {"y"})}, {"a"}, {"y"});
  const auto result = bind(model, {value("a", {1, 3, 128})});
  checkExtent(result, "y", 0, 12);
  validateAutomaticOnnxTensors(model, result, model.modelOutputs, {value("y", {12, 32})}, shapeControl());
  auto impossible = model;
  impossible.tensors.back().dimensions->back() = fixed(5);
  BOOST_CHECK_THROW(bind(impossible, {value("a", {1, 3, 128})}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ZeroProductsDoNotInventBoundsForAmbiguousAxes)
{
  const auto model = contract({tensor("a", {fixed(0), fixed(8)}), integerTensor("target", {fixed(2)}),
    tensor("y", {{}, fixed(0)})}, {operation("Reshape", {"a", "target"}, {"y"})}, {"a"}, {"y"});
  const auto result = bind(model, {value("a", {0, 8})});
  const auto& bounds = result.tensors.at("graph/y");
  BOOST_REQUIRE(bounds.maximumDimensions); BOOST_CHECK(!bounds.maximumDimensions->front());
  BOOST_REQUIRE(bounds.maxBytes); BOOST_CHECK_EQUAL(*bounds.maxBytes, 0);
  BOOST_CHECK_THROW(validateAutomaticOnnxTensors(model, result, model.modelOutputs,
    {value("y", {3, 0})}, shapeControl()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TransposeGatherFlattenAndSliceRespectTheirActualAxes)
{
  auto trans = operation("Transpose", {"a"}, {"t"}); trans.integerListAttributes["perm"] = {0, 2, 1};
  auto gather = operation("Gather", {"t", "idx"}, {"g"}); gather.integerAttributes["axis"] = -1;
  auto flatten = operation("Flatten", {"g"}, {"f"}); flatten.integerAttributes["axis"] = 1;
  const auto model = contract({tensor("a", {symbol("B"), fixed(4), fixed(8)}),
    tensor("t", {{}, {}, {}}), integerTensor("idx", {fixed(2)}), tensor("g", {{}, {}, {}}),
    tensor("f", {{}, {}}), tensor("s", {{}, {}}), integerTensor("starts", {fixed(2)}),
    integerTensor("ends", {fixed(2)})},
    {trans, gather, flatten, operation("Slice", {"f", "starts", "ends"}, {"s"})}, {"a"}, {"s"});
  const auto result = bind(model, {value("a", {3, 4, 8})});
  checkExtent(result, "t", 1, 8); checkExtent(result, "g", 2, 2);
  checkExtent(result, "f", 0, 3); checkExtent(result, "f", 1, 16);
  BOOST_CHECK_EQUAL(*result.tensors.at("graph/s").maximumDimensions->at(1), 16);
  BOOST_CHECK_THROW(validateAutomaticOnnxTensors(model, result, model.modelOutputs,
    {value("s", {3, 17})}, shapeControl()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RejectsInvalidPermutationAndKeepsUnknownRankAndAtomicBodiesUnknown)
{
  auto trans = operation("Transpose", {"a"}, {"y"}); trans.integerListAttributes["perm"] = {-1, 0};
  BOOST_CHECK_THROW(contract({tensor("a", {fixed(2), fixed(3)}), tensor("y", {{}, {}})},
    {trans}, {"a"}, {"y"}), std::invalid_argument);
  auto x = tensor("a", {}), y = tensor("y", {}); x.dimensions.reset(); y.dimensions.reset();
  const auto unknown = contract({x, y}, {operation("Identity", {"a"}, {"y"})}, {"a"}, {"y"});
  auto result = bind(unknown, {value("a", {2, 3})});
  BOOST_CHECK(!result.tensors.at("graph/y").maximumDimensions);
  auto atomic = operation("MatMul", {"a"}, {"y"});
  atomic.domain = "local"; atomic.atomicBody = true; atomic.functionScope = "function/0";
  const auto opaque = contract({tensor("a", {fixed(2)}), tensor("y", {{}})}, {atomic}, {"a"}, {"y"});
  BOOST_CHECK(opaque.relations.empty());
  result = bind(opaque, {value("a", {2})});
  BOOST_CHECK(!result.tensors.at("graph/y").maxBytes);
}

BOOST_AUTO_TEST_CASE(RejectsOverflowTamperedEnvelopeAndUnsupportedGeneration)
{
  const auto model = concatContract();
  const std::vector<NamedTensor> inputs{value("a", {1, 8, 0, 128}), value("b", {1, 8, 1, 128})};
  auto result = bind(model, inputs);
  result.tensors.at("graph/joined").maxBytes = 1;
  BOOST_CHECK_THROW(validateAutomaticOnnxTensors(model, result, model.modelOutputs,
    {value("joined", {1, 8, 1, 128})}, shapeControl()), std::invalid_argument);
  BOOST_CHECK_THROW(bindAutomaticOnnxShapes(model, inputs, 8, 2048, shapeControl()), std::invalid_argument);
  const auto huge = contract({tensor("a", {fixed(INT64_MAX), fixed(INT64_MAX)}),
    tensor("y", {fixed(INT64_MAX), fixed(INT64_MAX)})},
    {operation("Identity", {"a"}, {"y"})}, {}, {"y"});
  BOOST_CHECK_THROW(bind(huge, {}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ChangesOnlyRequestIdentitiesWhenOrdinaryInputShapeChanges)
{
  const auto model = concatContract(); const auto identity = model.digest(shapeControl());
  const auto first = bind(model, {value("a", {1, 8, 0, 128}), value("b", {1, 8, 2, 128})});
  const auto next = bind(model, {value("a", {1, 8, 0, 128}), value("b", {1, 8, 3, 128})});
  BOOST_CHECK_EQUAL(first.contractDigest, next.contractDigest);
  BOOST_CHECK_NE(first.bindingDigest, next.bindingDigest);
  BOOST_CHECK_NE(first.envelopeDigest, next.envelopeDigest);
  BOOST_CHECK_EQUAL(model.digest(shapeControl()), identity);
}

BOOST_AUTO_TEST_CASE(ChecksMetadataBudgetDeadlineAndCancellationWithoutRetainingCallbacks)
{
  const auto model = concatContract(); const auto encoded = model.canonicalJson(shapeControl());
  auto control = shapeControl(); control.maxAssembledBytes = encoded.size() - 1;
  BOOST_CHECK_THROW(AutomaticOnnxContract::fromCanonicalJson(encoded, control), std::invalid_argument);
  control = shapeControl(); control.deadline = std::chrono::steady_clock::now();
  BOOST_CHECK_THROW(model.canonicalJson(control), std::runtime_error);
  for (const auto fence : {1, 5, 30}) {
    int calls = 0; control = shapeControl();
    control.requireActive = [&] { if (++calls == fence) throw std::runtime_error("cancelled"); };
    BOOST_CHECK_THROW(AutomaticOnnxContract::fromCanonicalJson(encoded, control), std::runtime_error);
  }
}

BOOST_AUTO_TEST_CASE(IntegerLiteralValuesRoundTripExactlyInVersionTwo)
{
  auto literal = integerTensor("literal", {fixed(2)});
  literal.provenance = {"initializer-header-v1"};
  auto model = contract({literal}, {}, {}, {"literal"});
  model.integerLiterals["graph/literal"] = {std::numeric_limits<std::int64_t>::min(),
                                           std::numeric_limits<std::int64_t>::max()};
  const auto encoded = model.canonicalJson(shapeControl());
  const auto value = nativeParseJson(encoded);
  BOOST_CHECK_EQUAL(value.at("schema").get<std::string>(),
                    "ndnsf-di-automatic-onnx-contract-v2");
  BOOST_CHECK_EQUAL(value.at("integerLiterals").size(), 1);
  const auto restored = AutomaticOnnxContract::fromCanonicalJson(encoded, shapeControl());
  BOOST_CHECK(restored.integerLiterals == model.integerLiterals);
  BOOST_CHECK_EQUAL(restored.canonicalJson(shapeControl()), encoded);
  auto changed = model; changed.integerLiterals["graph/literal"][0] += 1;
  BOOST_CHECK_NE(changed.digest(shapeControl()), model.digest(shapeControl()));
  BOOST_CHECK_EQUAL(changed.factsJson(), model.factsJson());
  auto legacy = value; legacy["schema"] = "ndnsf-di-automatic-onnx-contract-v1";
  legacy.erase("integerLiterals");
  BOOST_CHECK_THROW(AutomaticOnnxContract::fromCanonicalJson(nativeCanonicalJson(legacy),
                                                            shapeControl()), std::exception);
}

BOOST_AUTO_TEST_CASE(IntegerLiteralsRequireRealHeadersExactShapeAndIntegerRange)
{
  auto literal = integerTensor("literal", {fixed(2)});
  literal.onnxElementType = 6; literal.wireElementType = TensorElementType::Int32;
  literal.provenance = {"initializer-header-v1"};
  const auto base = contract({literal}, {}, {}, {"literal"});
  for (const auto failure : {0, 1, 2, 3, 4, 5}) {
    auto model = base; model.integerLiterals["graph/literal"] = {-1, 2};
    if (failure == 0) model.integerLiterals["graph/missing"] = {1};
    if (failure == 1) model.tensors[0].provenance = {"graph-annotation-v1"};
    if (failure == 2) model.integerLiterals["graph/literal"].pop_back();
    if (failure == 3) model.integerLiterals["graph/literal"][0] = 2147483648LL;
    if (failure == 4) model.integerLiterals["graph/literal"].resize(65);
    if (failure == 5) model.tensors[0].dimensions->at(0) = symbol("unknown");
    BOOST_CHECK_THROW(model.canonicalJson(shapeControl()), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(IntegerLiteralReaderRejectsCoerciveOrOversizedValues)
{
  auto literal = integerTensor("literal", {});
  literal.provenance = {"constant-header-v1"};
  auto model = contract({literal}, {}, {}, {"literal"});
  model.integerLiterals["graph/literal"] = {0};
  const auto original = nativeParseJson(model.canonicalJson(shapeControl()));
  for (const auto failure : {0, 1, 2, 3, 4}) {
    auto value = original;
    if (failure == 0) value["integerLiterals"]["graph/literal"][0] = 0.0;
    if (failure == 1) value["integerLiterals"]["graph/literal"][0] = "0";
    if (failure == 2) value["integerLiterals"]["graph/literal"][0] = 9223372036854775808ULL;
    if (failure == 3) value["integerLiterals"]["graph/literal"] = NativeJson::object();
    if (failure == 4) value["integerLiterals"] = NativeJson::array();
    BOOST_CHECK_THROW(AutomaticOnnxContract::fromCanonicalJson(nativeCanonicalJson(value),
                                                              shapeControl()), std::exception);
  }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
