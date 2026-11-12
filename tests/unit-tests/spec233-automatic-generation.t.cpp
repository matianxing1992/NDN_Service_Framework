#include "tests/boost-test.hpp"
#include "tests/fixtures/spec233/automatic-source-projection.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-generation.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-admission.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-inference.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/onnx-tensor-type.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"

#include <onnx/onnx_pb.h>
#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
#include <onnxruntime_cxx_api.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <unistd.h>

namespace ndnsf::di::tests {
namespace {

NativeAssemblyControl
stateControl()
{
  return {std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {},
          8U << 20, 8U << 20};
}

DimensionConstraint
stateFixed(std::int64_t size)
{
  return {DimensionConstraint::Kind::FIXED, size, {}, static_cast<std::uint64_t>(size)};
}

DimensionConstraint
stateSymbol(const std::string& name)
{
  return {DimensionConstraint::Kind::SYMBOL, 0, "graph/" + name, {}};
}

AutomaticTensorContract
stateTensor(const std::string& name, std::vector<DimensionConstraint> dimensions,
            std::int32_t type = 1)
{
  return {"graph/" + name, type, onnxTensorWireType(type), std::move(dimensions), {"fixture"}};
}

AutomaticOnnxOperatorFact
stateOp(const std::string& type, std::vector<std::string> inputs,
        std::vector<std::string> outputs)
{
  AutomaticOnnxOperatorFact result;
  result.scope = "graph"; result.opType = type; result.opset = 13;
  result.inputs = std::move(inputs); result.outputs = std::move(outputs);
  return result;
}

void
sealStateFixture(AutomaticOnnxContract& contract)
{
  std::sort(contract.tensors.begin(), contract.tensors.end(),
    [](const auto& a, const auto& b) { return a.name < b.name; });
  for (std::size_t i = 0; i < contract.operators.size(); ++i) contract.operators[i].nodeIndex = i;
  contract.factsDigest = nativePlanningDigest(contract.factsJson());
  contract.relations = deriveAutomaticOnnxShapeRelations(contract, stateControl());
}

AutomaticOnnxContract
attentionFixture()
{
  AutomaticOnnxContract result;
  result.sourceDigest = "sha256:" + std::string(64, '0');
  // Projection width 24 for three Q heads of size8; unrelated hidden width7.
  // Deliberately use punctuation that cannot be encoded losslessly in CSV maps.
  result.tensors = {
    stateTensor("q", {stateSymbol("B"), stateSymbol("S"), stateFixed(24)}),
    stateTensor("k", {stateSymbol("B"), stateSymbol("S"), stateFixed(8)}),
    stateTensor("v", {stateSymbol("B"), stateSymbol("S"), stateFixed(8)}),
    stateTensor("old,key=0", {stateSymbol("B"), stateFixed(1), stateSymbol("P"), stateFixed(8)}),
    stateTensor("old,value=0", {stateSymbol("B"), stateFixed(1), stateSymbol("P"), stateFixed(8)}),
    stateTensor("new,key=0", {stateSymbol("B"), stateFixed(1), {}, stateFixed(8)}),
    stateTensor("new,value=0", {stateSymbol("B"), stateFixed(1), {}, stateFixed(8)}),
    stateTensor("lengths", {stateSymbol("B")}, 6), stateTensor("total", {}, 6),
    stateTensor("attended", {stateSymbol("B"), stateSymbol("S"), stateFixed(24)}),
    stateTensor("hidden", {stateSymbol("B"), stateSymbol("S"), stateFixed(7)})};
  result.modelInputs = {"graph/q", "graph/k", "graph/v", "graph/old,key=0",
                        "graph/old,value=0", "graph/lengths", "graph/total", "graph/hidden"};
  result.modelOutputs = {"graph/attended", "graph/new,key=0", "graph/new,value=0"};
  auto op = stateOp("GroupQueryAttention", {"q", "k", "v", "old,key=0", "old,value=0",
                                          "lengths", "total"},
                    {"attended", "new,key=0", "new,value=0"});
  op.domain = "com.microsoft"; op.opset = 1;
  op.integerAttributes = {{"num_heads", 3}, {"kv_num_heads", 1}};
  result.operators.push_back(std::move(op));
  sealStateFixture(result);
  return result;
}

AutomaticTensorContract&
stateFact(AutomaticOnnxContract& contract, const std::string& name)
{
  const auto found = std::find_if(contract.tensors.begin(), contract.tensors.end(),
    [&](const auto& value) { return value.name == "graph/" + name; });
  if (found == contract.tensors.end()) throw std::logic_error("fixture missing tensor");
  return *found;
}

AutomaticOnnxContract
generationFixture(bool rotary = true)
{
  auto result = attentionFixture();
  const auto b = stateSymbol("B"), s = stateSymbol("S"), t = stateSymbol("T");
  result.modelInputs = {"graph/ids", "graph/mask", "graph/old,key=0", "graph/old,value=0"};
  result.modelOutputs = {"graph/scores", "graph/new,key=0", "graph/new,value=0"};
  result.tensors.push_back(stateTensor("ids", {b, s}, 7));
  result.tensors.push_back(stateTensor("mask", {b, t}, 7));
  result.tensors.push_back(stateTensor("embedding", {b, s, stateFixed(7)}));
  result.tensors.push_back(stateTensor("scores", {b, s, stateFixed(11)}));
  result.tensors.push_back(stateTensor("mask-shape", {stateFixed(2)}, 7));
  result.tensors.push_back(stateTensor("sum", {b, stateFixed(1)}, 7));
  result.tensors.push_back(stateTensor("sum-minus-one", {b, stateFixed(1)}, 7));
  result.tensors.push_back(stateTensor("total64", {}, 7));
  stateFact(result, "lengths").dimensions = std::vector<DimensionConstraint>{b, stateFixed(1)};
  const auto header = [&](const std::string& name, std::vector<DimensionConstraint> dims,
                           std::int32_t type = 1) {
    auto tensor = stateTensor(name, std::move(dims), type);
    tensor.provenance = {"initializer-header-v1"};
    result.tensors.push_back(std::move(tensor));
  };
  header("words", {stateFixed(11), stateFixed(7)});
  header("wq", {stateFixed(7), stateFixed(24)});
  header("wk", {stateFixed(7), stateFixed(8)});
  header("wv", {stateFixed(7), stateFixed(8)});
  header("wo", {stateFixed(24), stateFixed(11)});
  header("one", {}, 7); header("axes", {stateFixed(1)}, 7);
  result.integerLiterals = {{"graph/one", {1}}, {"graph/axes", {1}}};
  result.operators.push_back(stateOp("Gather", {"words", "ids"}, {"embedding"}));
  for (const auto& role : {std::string("q"), std::string("k"), std::string("v")})
    result.operators.push_back(stateOp("MatMul", {"embedding", "w" + role}, {role}));
  result.operators.push_back(stateOp("ReduceSum", {"mask", "axes"}, {"sum"}));
  result.operators.push_back(stateOp("Sub", {"sum", "one"}, {"sum-minus-one"}));
  auto lengths = stateOp("Cast", {"sum-minus-one"}, {"lengths"});
  lengths.integerAttributes["to"] = 6; result.operators.push_back(lengths);
  result.operators.push_back(stateOp("Shape", {"mask"}, {"mask-shape"}));
  result.operators.push_back(stateOp("Gather", {"mask-shape", "one"}, {"total64"}));
  auto total = stateOp("Cast", {"total64"}, {"total"});
  total.integerAttributes["to"] = 6; result.operators.push_back(total);
  result.operators.push_back(stateOp("MatMul", {"attended", "wo"}, {"scores"}));
  if (rotary) {
    result.modelInputs.push_back("graph/positions");
    result.tensors.push_back(stateTensor("positions", {b, s}, 7));
    result.tensors.push_back(stateTensor("q-raw", {b, s, stateFixed(24)}));
    result.tensors.push_back(stateTensor("k-raw", {b, s, stateFixed(8)}));
    header("cosine", {stateFixed(64), stateFixed(4)});
    header("sine", {stateFixed(64), stateFixed(4)});
    for (auto& op : result.operators) {
      if (op.opType == "MatMul" && (op.outputs[0] == "q" || op.outputs[0] == "k"))
        op.outputs[0] += "-raw";
    }
    for (const auto& role : {std::string("q"), std::string("k")}) {
      auto op = stateOp("RotaryEmbedding", {role + "-raw", "positions", "cosine", "sine"},
                        {role});
      op.domain = "com.microsoft"; op.opset = 1;
      result.operators.push_back(op);
    }
  }
  auto attention = result.operators.front();
  result.operators.erase(result.operators.begin());
  const auto projection = std::find_if(result.operators.begin(), result.operators.end(),
    [](const auto& op) { return op.outputs == std::vector<std::string>{"scores"}; });
  auto scores = *projection;
  result.operators.erase(projection);
  result.operators.push_back(std::move(attention));
  result.operators.push_back(std::move(scores));
  sealStateFixture(result);
  return result;
}

void
assignGeneration(AutomaticOnnxContract& contract)
{
  contract.generation = deriveAutomaticOnnxGeneration(contract, stateControl());
  if (!contract.generation) throw std::runtime_error("fixture generation was not proved");
}

bool
generationUnproven(const AutomaticOnnxContract& contract)
{
  try {
    return !deriveAutomaticOnnxGeneration(contract, stateControl());
  }
  catch (const std::invalid_argument&) {
    return true;
  }
}

NamedTensor
generationInteger(const std::string& name, std::vector<std::int64_t> shape,
                    const std::vector<std::int64_t>& values)
{
  std::vector<std::uint8_t> bytes;
  for (const auto value : values) {
    const auto bits = static_cast<std::uint64_t>(value);
    for (unsigned byte = 0; byte < 8; ++byte)
      bytes.push_back(static_cast<std::uint8_t>(bits >> (8 * byte)));
  }
  return {name, TensorElementType::Int64, std::move(shape), std::move(bytes)};
}

NamedTensor
generationFloat(const std::string& name, std::vector<std::int64_t> shape)
{
  std::uint64_t count = 1;
  for (const auto extent : shape) count *= static_cast<std::uint64_t>(extent);
  if (count > 100000) throw std::invalid_argument("fixture allocation");
  return {name, TensorElementType::Float32, std::move(shape),
          std::vector<std::uint8_t>(count * sizeof(float))};
}

std::vector<NamedTensor>
generationInputs(std::int64_t batch, std::int64_t sequence, std::int64_t past = 0)
{
  std::vector<std::int64_t> tokens, positions;
  for (std::int64_t b = 0; b < batch; ++b) {
    for (std::int64_t s = 0; s < sequence; ++s) {
      tokens.push_back((b + s + 1) % 11);
      positions.push_back(past + s);
    }
  }
  return {generationInteger("ids", {batch, sequence}, tokens),
    generationInteger("mask", {batch, past + sequence},
                      std::vector<std::int64_t>(batch * (past + sequence), 1)),
    generationInteger("positions", {batch, sequence}, positions),
    generationFloat("old,key=0", {batch, 1, past, 8}),
    generationFloat("old,value=0", {batch, 1, past, 8})};
}

onnx::ModelProto
generationSourceModel(bool normalization = true)
{
  auto fixture = generationFixture();
  if (normalization) {
    auto gamma = stateTensor("query-gamma", {stateFixed(24)});
    gamma.provenance = {"initializer-header-v1"}; fixture.tensors.push_back(gamma);
    fixture.tensors.push_back(stateTensor("q-unnormalized",
      {stateSymbol("B"), stateSymbol("S"), stateFixed(24)}));
    for (auto& op : fixture.operators)
      if (op.opType == "MatMul" && op.outputs[0] == "q-raw") op.outputs[0] = "q-unnormalized";
    const auto rotary = std::find_if(fixture.operators.begin(), fixture.operators.end(),
      [](const auto& op) { return op.opType == "RotaryEmbedding"; });
    fixture.operators.insert(rotary, stateOp("SimplifiedLayerNormalization",
      {"q-unnormalized", "query-gamma"}, {"q-raw"}));
    for (const auto& name : {"post", "post-gamma"}) {
      auto tensor = stateTensor(name, std::string(name) == "post" ?
        std::vector<DimensionConstraint>{stateFixed(24), stateFixed(7)} :
        std::vector<DimensionConstraint>{stateFixed(7)});
      tensor.provenance = {"initializer-header-v1"}; fixture.tensors.push_back(tensor);
    }
    stateFact(fixture, "wo").dimensions->at(0) = stateFixed(7);
    for (const auto& name : {"post-hidden", "normalized", "residual-sum"})
      fixture.tensors.push_back(stateTensor(name,
        {stateSymbol("B"), stateSymbol("S"), stateFixed(7)}));
    const auto scores = std::find_if(fixture.operators.begin(), fixture.operators.end(),
      [](const auto& op) { return op.outputs == std::vector<std::string>{"scores"}; });
    const auto offset = scores - fixture.operators.begin();
    scores->inputs[0] = "normalized";
    auto norm = stateOp("SkipSimplifiedLayerNormalization",
      {"embedding", "post-hidden", "post-gamma"}, {"normalized", "", "", "residual-sum"});
    norm.domain = "com.microsoft"; norm.opset = 1;
    fixture.operators.insert(fixture.operators.begin() + offset,
      {stateOp("MatMul", {"attended", "post"}, {"post-hidden"}), norm});
  }
  onnx::ModelProto model;
  model.set_ir_version(8);
  model.add_opset_import()->set_version(13);
  auto* microsoft = model.add_opset_import();
  microsoft->set_domain("com.microsoft"); microsoft->set_version(1);
  auto* graph = model.mutable_graph(); graph->set_name("independent-small-stateful-source");
  const auto port = [&](const std::string& name, bool output) {
    const auto& tensor = stateFact(fixture, name.substr(6));
    auto* value = output ? graph->add_output() : graph->add_input();
    value->set_name(name.substr(6));
    auto* type = value->mutable_type()->mutable_tensor_type();
    type->set_elem_type(tensor.onnxElementType);
    for (const auto& dimension : *tensor.dimensions) {
      auto* axis = type->mutable_shape()->add_dim();
      if (dimension.kind == DimensionConstraint::Kind::FIXED) axis->set_dim_value(dimension.fixed);
      else if (dimension.kind == DimensionConstraint::Kind::SYMBOL)
        axis->set_dim_param(dimension.symbol.substr(6));
    }
  };
  for (const auto& name : fixture.modelInputs) port(name, false);
  for (const auto& name : fixture.modelOutputs) port(name, true);
  for (const auto& tensor : fixture.tensors) {
    if (tensor.provenance != std::vector<std::string>{"initializer-header-v1"}) continue;
    auto* value = graph->add_initializer(); value->set_name(tensor.name.substr(6));
    value->set_data_type(tensor.onnxElementType);
    std::uint64_t count = 1;
    for (const auto& dimension : *tensor.dimensions) {
      value->add_dims(dimension.fixed); count *= dimension.fixed;
    }
    const auto literal = fixture.integerLiterals.find(tensor.name);
    if (literal != fixture.integerLiterals.end()) {
      for (const auto integer : literal->second) value->add_int64_data(integer);
    }
    else {
      const auto name = tensor.name.substr(6);
      for (std::uint64_t index = 0; index < count; ++index) {
        const float number = name == "cosine" || name.find("gamma") != std::string::npos ? 1.0F :
          name == "sine" ? 0.0F : name == "wk" ? 0.02F : name == "wv" ? 0.03F :
          name == "words" ? (index % 11 + 1) * 0.01F : 0.05F;
        value->add_float_data(number);
      }
    }
  }
  for (const auto& op : fixture.operators) {
    auto* node = graph->add_node(); node->set_op_type(op.opType); node->set_domain(op.domain);
    for (const auto& name : op.inputs) node->add_input(name);
    for (const auto& name : op.outputs) node->add_output(name);
    for (const auto& [name, value] : op.integerAttributes) {
      auto* attr = node->add_attribute(); attr->set_name(name);
      attr->set_type(onnx::AttributeProto::INT); attr->set_i(value);
    }
    for (const auto& [name, values] : op.integerListAttributes) {
      auto* attr = node->add_attribute(); attr->set_name(name);
      attr->set_type(onnx::AttributeProto::INTS);
      for (const auto value : values) attr->add_ints(value);
    }
  }
  // No intermediate value_info: standard inference must cross the contrib nodes.
  return model;
}

struct GenerationModelFile
{
  explicit GenerationModelFile(const onnx::ModelProto& model)
    : root(std::filesystem::temp_directory_path() /
           ("ndnsf-spec233-generation-model-" + std::to_string(::getpid())))
    , path(root / "model.onnx")
  {
    if (!std::filesystem::create_directory(root)) throw std::runtime_error("fixture directory exists");
    try {
      std::ofstream output(path, std::ios::binary);
      if (!model.SerializeToOstream(&output)) throw std::runtime_error("fixture serialization");
      output.close();
      if (!output) throw std::runtime_error("fixture write");
    }
    catch (...) {
      std::filesystem::remove_all(root);
      throw;
    }
  }

  ~GenerationModelFile()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  const std::filesystem::path root;
  const std::filesystem::path path;
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec233AutomaticGeneration)

BOOST_AUTO_TEST_CASE(FormalPortsProvePairsWithoutHiddenHeadDivision)
{
  const auto states = deriveAutomaticOnnxAttentionStates(attentionFixture(), stateControl());
  BOOST_REQUIRE_EQUAL(states.size(), 2);
  BOOST_CHECK(states[0].role == AutomaticAttentionState::Role::KEY);
  BOOST_CHECK(states[1].role == AutomaticAttentionState::Role::VALUE);
  BOOST_CHECK_EQUAL(states[0].input, "graph/old,key=0");
  BOOST_CHECK_EQUAL(states[0].output, "graph/new,key=0");
  BOOST_CHECK_EQUAL(states[1].input, "graph/old,value=0");
  BOOST_CHECK_EQUAL(states[1].output, "graph/new,value=0");
  BOOST_CHECK_EQUAL(states[0].headSize, 8);
  BOOST_CHECK_EQUAL(states[0].heads, 1);
  BOOST_CHECK_EQUAL(states[0].sequenceLengths, "graph/lengths");
  BOOST_CHECK_EQUAL(states[0].totalLength, "graph/total");
  BOOST_CHECK_EQUAL(states[0].inputAxes[2], 2);
  BOOST_CHECK_EQUAL(states[0].outputAxes[2], 2);
}

BOOST_AUTO_TEST_CASE(RenamedGraphAndAbsentIntermediateAnnotationsRemainProvable)
{
  auto contract = attentionFixture();
  auto& gqa = contract.operators[0];
  gqa.inputs[3] = "bridge";
  auto identity = stateOp("Identity", {"old,key=0"}, {"bridge"});
  contract.operators.insert(contract.operators.begin(), identity);
  // Rename every graph value consistently without preserving any name hint.
  std::map<std::string, std::string> names;
  for (const auto& tensor : contract.tensors)
    names[tensor.name.substr(6)] = "renamed/" + std::to_string(names.size());
  names["bridge"] = "renamed/only-use-def";
  for (auto& tensor : contract.tensors) tensor.name = "graph/" + names.at(tensor.name.substr(6));
  for (auto& op : contract.operators) {
    for (auto& name : op.inputs) name = names.at(name);
    for (auto& name : op.outputs) name = names.at(name);
  }
  for (auto* ports : {&contract.modelInputs, &contract.modelOutputs})
    for (auto& name : *ports) name = "graph/" + names.at(name.substr(6));
  sealStateFixture(contract);
  const auto states = deriveAutomaticOnnxAttentionStates(contract, stateControl());
  BOOST_REQUIRE_EQUAL(states.size(), 2);
  BOOST_CHECK_EQUAL(states[0].input, "graph/" + names.at("old,key=0"));
  BOOST_CHECK_EQUAL(states[0].output, "graph/" + names.at("new,key=0"));
  BOOST_CHECK_EQUAL(states[0].headSize, 8);
}

BOOST_AUTO_TEST_CASE(InvertibleBoundaryLayoutsAreMappedInBothDirections)
{
  auto contract = attentionFixture();
  for (const auto& role : {std::string("key"), std::string("value")}) {
    stateFact(contract, "old," + role + "=0").dimensions =
      std::vector<DimensionConstraint>{stateSymbol("B"), stateSymbol("P"), stateFixed(1),
                                       stateFixed(8)};
    stateFact(contract, "new," + role + "=0").dimensions =
      std::vector<DimensionConstraint>{{}, stateSymbol("B"), stateFixed(8), stateFixed(1)};
  }
  contract.operators[0].inputs[3] = "formalK";
  contract.operators[0].inputs[4] = "formalV";
  contract.operators[0].outputs[1] = "formalNextK";
  contract.operators[0].outputs[2] = "formalNextV";
  for (const auto& role : {std::string("K"), std::string("V")}) {
    const auto label = role == "K" ? "key" : "value";
    auto input = stateOp("Transpose", {std::string("old,") + label + "=0"}, {"formal" + role});
    input.integerListAttributes["perm"] = {0, 2, 1, 3};
    auto output = stateOp("Transpose", {"formalNext" + role},
                          {std::string("new,") + label + "=0"});
    output.integerListAttributes["perm"] = {2, 0, 3, 1};
    contract.operators.push_back(input); contract.operators.push_back(output);
  }
  sealStateFixture(contract);
  const auto states = deriveAutomaticOnnxAttentionStates(contract, stateControl());
  BOOST_REQUIRE_EQUAL(states.size(), 2);
  const std::array<std::uint64_t, 4> expectedInput{0, 2, 1, 3};
  const std::array<std::uint64_t, 4> expectedOutput{1, 3, 0, 2};
  BOOST_CHECK(states[0].inputAxes == expectedInput);
  BOOST_CHECK(states[0].outputAxes == expectedOutput);
  BOOST_CHECK(states[1].inputAxes == expectedInput);
  BOOST_CHECK(states[1].outputAxes == expectedOutput);
}

BOOST_AUTO_TEST_CASE(SameTypeCastIsLosslessButInputOrOutputConversionIsRejected)
{
  for (const bool input : {true, false}) {
    auto contract = attentionFixture();
    auto cast = input ? stateOp("Cast", {"old,key=0"}, {"bridge"}) :
                        stateOp("Cast", {"bridge"}, {"new,key=0"});
    cast.integerAttributes["to"] = 1;
    if (input) contract.operators[0].inputs[3] = "bridge";
    else contract.operators[0].outputs[1] = "bridge";
    contract.operators.push_back(cast); sealStateFixture(contract);
    BOOST_CHECK_EQUAL(deriveAutomaticOnnxAttentionStates(contract, stateControl()).size(), 2);
    contract.operators.back().integerAttributes["to"] = 10;
    sealStateFixture(contract);
    BOOST_CHECK_THROW(deriveAutomaticOnnxAttentionStates(contract, stateControl()),
                      std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(AliasedOutputsOrSharedPastAreAmbiguous)
{
  auto contract = attentionFixture();
  contract.operators.push_back(stateOp("Identity", {"new,key=0"}, {"alias"}));
  contract.modelOutputs.push_back("graph/alias");
  contract.tensors.push_back(stateTensor("alias", {stateSymbol("B"), stateFixed(1), {},
                                                  stateFixed(8)}));
  sealStateFixture(contract);
  BOOST_CHECK_THROW(deriveAutomaticOnnxAttentionStates(contract, stateControl()),
                    std::invalid_argument);
  contract = attentionFixture(); contract.operators[0].inputs[4] = "old,key=0";
  sealStateFixture(contract);
  BOOST_CHECK_THROW(deriveAutomaticOnnxAttentionStates(contract, stateControl()),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ProjectionHeadAndStaticAxisConflictsAreRejected)
{
  for (const auto& name : {"q", "k", "old,key=0", "new,value=0"}) {
    auto contract = attentionFixture();
    stateFact(contract, name).dimensions->back() = stateFixed(7);
    sealStateFixture(contract);
    BOOST_CHECK_THROW(deriveAutomaticOnnxAttentionStates(contract, stateControl()),
                      std::invalid_argument);
  }
  auto contract = attentionFixture();
  contract.operators[0].integerAttributes["num_heads"] = 2;
  contract.operators[0].integerAttributes["kv_num_heads"] = 3;
  sealStateFixture(contract);
  BOOST_CHECK_THROW(deriveAutomaticOnnxAttentionStates(contract, stateControl()),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ConflictingIntermediateHeaderIsNotSilentlyIgnored)
{
  auto contract = attentionFixture();
  contract.operators[0].inputs[3] = "bridge";
  contract.operators.push_back(stateOp("Identity", {"old,key=0"}, {"bridge"}));
  contract.tensors.push_back(stateTensor("bridge", {stateSymbol("B"), stateFixed(1),
                                                    stateSymbol("P"), stateFixed(9)}));
  sealStateFixture(contract);
  BOOST_CHECK_THROW(deriveAutomaticOnnxAttentionStates(contract, stateControl()),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(FloatingCachesAreIndependentOfWeightPrecision)
{
  for (const auto type : {1, 10, 16}) {
    auto contract = attentionFixture();
    for (auto& tensor : contract.tensors) {
      if (tensor.onnxElementType != 1) continue;
      tensor.onnxElementType = type; tensor.wireElementType = onnxTensorWireType(type);
    }
    sealStateFixture(contract);
    const auto states = deriveAutomaticOnnxAttentionStates(contract, stateControl());
    BOOST_REQUIRE_EQUAL(states.size(), 2);
    BOOST_CHECK(states[0].elementType == *onnxTensorWireType(type));
  }
}

BOOST_AUTO_TEST_CASE(SymbolicBoundaryCannotHideConflictingIntermediateFixedExtents)
{
  auto contract = attentionFixture();
  contract.operators[0].inputs[3] = "second";
  contract.operators.push_back(stateOp("Identity", {"old,key=0"}, {"first"}));
  contract.operators.push_back(stateOp("Identity", {"first"}, {"second"}));
  for (const auto& item : {std::pair<std::string, int>{"first", 2}, {"second", 3}})
    contract.tensors.push_back(stateTensor(item.first, {stateSymbol("B"), stateFixed(1),
                                                        stateFixed(item.second), stateFixed(8)}));
  sealStateFixture(contract);
  BOOST_CHECK_THROW(deriveAutomaticOnnxAttentionStates(contract, stateControl()),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(UnknownRankCapacityAndQuantizedCacheDoNotAcquireGenerationMeaning)
{
  for (const auto failure : {0, 1, 2, 3}) {
    auto contract = attentionFixture();
    if (failure == 0) stateFact(contract, "old,key=0").dimensions.reset();
    if (failure == 1) stateFact(contract, "old,key=0").dimensions->at(2) = stateFixed(64);
    if (failure == 2) contract.operators[0].integerAttributes["kv_cache_bit_width"] = 4;
    if (failure == 3) contract.operators[0].inputs[3] = "missing-state";
    sealStateFixture(contract);
    BOOST_CHECK_THROW(deriveAutomaticOnnxAttentionStates(contract, stateControl()),
                      std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(OpaqueOrUnrecognizedOperatorsHaveNoInventedStateProof)
{
  for (const auto change : {0, 1, 2, 3}) {
    auto contract = attentionFixture();
    if (change == 0) contract.operators[0].domain = "private";
    if (change == 1) contract.operators[0].opset = 2;
    if (change == 2) contract.operators[0].atomicBody = true;
    if (change == 3) contract.operators[0].functionScope = "function/0";
    sealStateFixture(contract);
    BOOST_CHECK(deriveAutomaticOnnxAttentionStates(contract, stateControl()).empty());
  }
}

BOOST_AUTO_TEST_CASE(CancellationDeadlineAndMetadataBudgetRemainOwned)
{
  const auto contract = attentionFixture();
  auto control = stateControl();
  control.requireActive = [] { throw std::runtime_error("cancelled"); };
  BOOST_CHECK_THROW(deriveAutomaticOnnxAttentionStates(contract, control), std::runtime_error);
  control = stateControl(); control.deadline = std::chrono::steady_clock::now();
  BOOST_CHECK_THROW(deriveAutomaticOnnxAttentionStates(contract, control), std::runtime_error);
  control = stateControl(); control.maxAssembledBytes = 1;
  BOOST_CHECK_THROW(deriveAutomaticOnnxAttentionStates(contract, control), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ProvesTokenFeedbackMaskPositionAndVocabularyFromActualConnections)
{
  auto contract = generationFixture();
  assignGeneration(contract);
  const auto& generation = *contract.generation;
  BOOST_CHECK_EQUAL(generation.tokenInput, "graph/ids");
  BOOST_CHECK_EQUAL(generation.tokenBatchAxis, 0);
  BOOST_CHECK_EQUAL(generation.tokenSequenceAxis, 1);
  BOOST_CHECK_EQUAL(generation.logitsOutput, "graph/scores");
  BOOST_CHECK_EQUAL(generation.logitsBatchAxis, 0);
  BOOST_CHECK_EQUAL(generation.logitsSequenceAxis, 1);
  BOOST_CHECK_EQUAL(generation.logitsVocabularyAxis, 2);
  BOOST_CHECK_EQUAL(generation.vocabularySize, 11);
  BOOST_CHECK_EQUAL(generation.maskInput, "graph/mask");
  BOOST_CHECK_EQUAL(generation.maskBatchAxis, 0);
  BOOST_CHECK_EQUAL(generation.maskContextAxis, 1);
  BOOST_CHECK_EQUAL(generation.positionInput, "graph/positions");
  BOOST_CHECK_EQUAL(generation.contextLimit, 64);
  BOOST_REQUIRE_EQUAL(generation.states.size(), 2);
  BOOST_CHECK_EQUAL(generation.states[0].operatorIndex, 12);
  BOOST_CHECK_EQUAL(generation.states[0].headSize, 8);
  const auto expectedState = [](const char* role, const char* input, const char* output) {
    return NativeJson{{"role", role}, {"operatorIndex", 12}, {"input", input}, {"output", output},
      {"query", "graph/q"}, {"key", "graph/k"}, {"value", "graph/v"},
      {"sequenceLengths", "graph/lengths"}, {"totalLength", "graph/total"},
      {"elementType", "float32"}, {"inputAxes", {0, 1, 2, 3}}, {"outputAxes", {0, 1, 2, 3}},
      {"heads", 1}, {"headSize", 8}, {"initial", "empty"}, {"update", "replace_full"}};
  };
  const auto expected = NativeJson{{"schema", "ndnsf-di-automatic-generation-v1"},
    {"tokenInput", "graph/ids"}, {"tokenBatchAxis", 0}, {"tokenSequenceAxis", 1},
    {"logitsOutput", "graph/scores"}, {"logitsBatchAxis", 0}, {"logitsSequenceAxis", 1},
    {"logitsVocabularyAxis", 2}, {"vocabularySize", 11},
    {"states", NativeJson::array({expectedState("key", "graph/old,key=0", "graph/new,key=0"),
                                 expectedState("value", "graph/old,value=0",
                                               "graph/new,value=0")})},
    {"maskInput", "graph/mask"}, {"maskBatchAxis", 0}, {"maskContextAxis", 1},
    {"maskPolicy", "all_ones"}, {"positionInput", "graph/positions"},
    {"positionBatchAxis", 0}, {"positionSequenceAxis", 1}, {"positionPolicy", "contiguous"},
    {"contextLimit", 64}, {"ruleId", "causal-gqa"}, {"ruleVersion", 1}};
  BOOST_CHECK_EQUAL(nativeCanonicalJson(automaticOnnxGenerationJson(contract.generation)),
                    nativeCanonicalJson(expected));
  const auto encoded = contract.canonicalJson(stateControl());
  const auto restored = AutomaticOnnxContract::fromCanonicalJson(encoded, stateControl());
  BOOST_REQUIRE(restored.generation);
  BOOST_CHECK_EQUAL(restored.canonicalJson(stateControl()), encoded);
}

BOOST_AUTO_TEST_CASE(PositionlessCausalGraphHasNoInventedPositionPort)
{
  auto contract = generationFixture(false);
  assignGeneration(contract);
  BOOST_CHECK(contract.generation->positionInput.empty());
  BOOST_CHECK_EQUAL(contract.generation->contextLimit,
                    std::uint64_t(std::numeric_limits<std::int32_t>::max()));
  BOOST_CHECK_EQUAL(contract.generation->vocabularySize, 11);
}

BOOST_AUTO_TEST_CASE(TokenAndScoreTransposesRecoverDistinctRootAxes)
{
  auto contract = generationFixture();
  const auto b = stateSymbol("B"), s = stateSymbol("S");
  stateFact(contract, "ids").dimensions = std::vector<DimensionConstraint>{s, b};
  contract.tensors.push_back(stateTensor("ids-bs", {b, s}, 7));
  for (auto& op : contract.operators)
    if (op.opType == "Gather" && op.inputs[0] == "words") op.inputs[1] = "ids-bs";
  auto input = stateOp("Transpose", {"ids"}, {"ids-bs"});
  input.integerListAttributes["perm"] = {1, 0};
  contract.operators.insert(contract.operators.begin(), input);
  for (auto& op : contract.operators)
    if (op.outputs == std::vector<std::string>{"scores"}) op.outputs[0] = "scores-before";
  contract.tensors.push_back(stateTensor("scores-before", {b, s, stateFixed(11)}));
  stateFact(contract, "scores").dimensions =
    std::vector<DimensionConstraint>{s, stateFixed(11), b};
  auto output = stateOp("Transpose", {"scores-before"}, {"scores"});
  output.integerListAttributes["perm"] = {1, 2, 0};
  contract.operators.push_back(output);
  sealStateFixture(contract); assignGeneration(contract);
  BOOST_CHECK_EQUAL(contract.generation->tokenBatchAxis, 1);
  BOOST_CHECK_EQUAL(contract.generation->tokenSequenceAxis, 0);
  BOOST_CHECK_EQUAL(contract.generation->logitsBatchAxis, 2);
  BOOST_CHECK_EQUAL(contract.generation->logitsSequenceAxis, 0);
  BOOST_CHECK_EQUAL(contract.generation->logitsVocabularyAxis, 1);
}

BOOST_AUTO_TEST_CASE(PostRotaryTransposesMapPositionsIntoTheGlobalTokenRoles)
{
  auto contract = generationFixture();
  const auto b = stateSymbol("B"), s = stateSymbol("S");
  for (const auto* name : {"mask", "sum", "sum-minus-one", "lengths", "attended", "scores",
                           "old,key=0", "old,value=0", "new,key=0", "new,value=0"})
    stateFact(contract, name).dimensions->at(0) = s;
  for (const auto* name : {"attended", "scores"})
    stateFact(contract, name).dimensions->at(1) = b;
  for (const auto* role : {"q", "k", "v"}) {
    const std::string name = role, before = name + "-before-swap";
    auto tensor = stateFact(contract, name);
    tensor.name = "graph/" + before;
    contract.tensors.push_back(tensor);
    stateFact(contract, name).dimensions->at(0) = s;
    stateFact(contract, name).dimensions->at(1) = b;
    for (auto& op : contract.operators)
      if (op.outputs == std::vector<std::string>{name}) op.outputs[0] = before;
    auto swap = stateOp("Transpose", {before}, {name});
    swap.integerListAttributes["perm"] = {1, 0, 2};
    const auto attention = std::find_if(contract.operators.begin(), contract.operators.end(),
      [](const auto& op) { return op.opType == "GroupQueryAttention"; });
    contract.operators.insert(attention, swap);
  }
  sealStateFixture(contract); assignGeneration(contract);
  BOOST_CHECK_EQUAL(contract.generation->tokenBatchAxis, 1);
  BOOST_CHECK_EQUAL(contract.generation->tokenSequenceAxis, 0);
  BOOST_CHECK_EQUAL(contract.generation->positionBatchAxis, 1);
  BOOST_CHECK_EQUAL(contract.generation->positionSequenceAxis, 0);
  const auto encoded = contract.canonicalJson(stateControl());
  BOOST_CHECK_EQUAL(AutomaticOnnxContract::fromCanonicalJson(encoded, stateControl())
                      .canonicalJson(stateControl()), encoded);
  auto inputs = generationInputs(2, 3);
  inputs[0] = generationInteger("ids", {3, 2}, {1, 2, 3, 4, 5, 6});
  inputs[2] = generationInteger("positions", {3, 2}, {0, 0, 1, 1, 2, 2});
  const auto envelope = bindAutomaticOnnxShapes(contract, inputs, 3, 16, stateControl());
  const auto decode = automaticOnnxRoundInputShapes(contract, envelope, 1, stateControl());
  BOOST_CHECK(decode.at("graph/ids") == std::vector<std::int64_t>({1, 2}));
  BOOST_CHECK(decode.at("graph/positions") == std::vector<std::int64_t>({1, 2}));
  inputs[2] = generationInteger("positions", {3, 2}, {0, 1, 2, 0, 1, 2});
  BOOST_CHECK_THROW(bindAutomaticOnnxShapes(contract, inputs, 3, 16, stateControl()),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SharedTransposedEmbeddingWeightsProveTheLogitsProjection)
{
  for (const bool explicitPermutation : {false, true}) {
    auto contract = generationFixture();
    stateFact(contract, "wo").dimensions->at(1) = stateFixed(7);
    contract.tensors.push_back(stateTensor("hidden-for-logits",
      {stateSymbol("B"), stateSymbol("S"), stateFixed(7)}));
    auto view = stateTensor("shared-matrix-view", {stateFixed(7), stateFixed(11)});
    view.dimensions.reset(); view.onnxElementType = 0; view.wireElementType.reset();
    contract.tensors.push_back(view);
    for (auto& op : contract.operators)
      if (op.outputs == std::vector<std::string>{"scores"}) op.outputs[0] = "hidden-for-logits";
    auto transpose = stateOp("Transpose", {"words"}, {"shared-matrix-view"});
    if (explicitPermutation) transpose.integerListAttributes["perm"] = {1, 0};
    contract.operators.push_back(transpose);
    contract.operators.push_back(stateOp("MatMul", {"hidden-for-logits", "shared-matrix-view"},
                                         {"scores"}));
    sealStateFixture(contract); assignGeneration(contract);
    BOOST_CHECK_EQUAL(contract.generation->vocabularySize, 11);
    BOOST_CHECK_EQUAL(contract.generation->states[0].headSize, 8);
    contract.generation.reset();
    stateFact(contract, "shared-matrix-view").dimensions =
      std::vector<DimensionConstraint>{stateFixed(8), stateFixed(11)};
    sealStateFixture(contract);
    BOOST_CHECK(generationUnproven(contract));
  }
}

BOOST_AUTO_TEST_CASE(ProjectionAndEmbeddingMatrixAliasesUseActualHeaderGeometry)
{
  auto contract = generationFixture();
  auto table = stateTensor("table-alias", {});
  table.dimensions.reset(); contract.tensors.push_back(table);
  auto weight = stateTensor("query-weight-view", {});
  weight.dimensions.reset(); contract.tensors.push_back(weight);
  stateFact(contract, "wq").dimensions =
    std::vector<DimensionConstraint>{stateFixed(24), stateFixed(7)};
  for (auto& op : contract.operators) {
    if (op.opType == "Gather" && op.inputs[0] == "words") op.inputs[0] = "table-alias";
    if (op.opType == "MatMul" && op.inputs[1] == "wq") op.inputs[1] = "query-weight-view";
  }
  auto transpose = stateOp("Transpose", {"wq"}, {"query-weight-view"});
  transpose.integerListAttributes["perm"] = {1, 0};
  contract.operators.insert(contract.operators.begin(),
    {stateOp("Identity", {"words"}, {"table-alias"}), transpose});
  sealStateFixture(contract); assignGeneration(contract);
  BOOST_CHECK_EQUAL(contract.generation->tokenInput, "graph/ids");
  BOOST_CHECK_EQUAL(contract.generation->vocabularySize, 11);
  BOOST_CHECK_EQUAL(contract.generation->states[0].headSize, 8);
}

BOOST_AUTO_TEST_CASE(QuantizedEmbeddingAndDequantizedWeightViewsRetainSourceGeometry)
{
  auto contract = generationFixture();
  for (const auto& item : {std::pair<std::string, std::int32_t>{"words", 2}, {"wo", 3}}) {
    auto& tensor = stateFact(contract, item.first);
    tensor.onnxElementType = item.second;
    tensor.wireElementType = onnxTensorWireType(item.second);
  }
  for (const auto* name : {"embedding-scale", "projection-scale"}) {
    auto tensor = stateTensor(name, {});
    tensor.provenance = {"initializer-header-v1"}; contract.tensors.push_back(tensor);
  }
  contract.tensors.push_back(stateTensor("embedding-u8",
    {stateSymbol("B"), stateSymbol("S"), stateFixed(7)}, 2));
  contract.tensors.push_back(stateTensor("float-weights", {stateFixed(24), stateFixed(11)}));
  for (auto& op : contract.operators) {
    if (op.outputs == std::vector<std::string>{"embedding"}) op.outputs[0] = "embedding-u8";
    if (op.opType == "MatMul" && op.inputs[1] == "wo") op.inputs[1] = "float-weights";
  }
  auto gather = std::find_if(contract.operators.begin(), contract.operators.end(),
    [](const auto& op) { return op.opType == "Gather" && op.inputs[0] == "words"; });
  contract.operators.insert(gather + 1,
    stateOp("DequantizeLinear", {"embedding-u8", "embedding-scale"}, {"embedding"}));
  contract.operators.insert(contract.operators.begin(),
    stateOp("DequantizeLinear", {"wo", "projection-scale"}, {"float-weights"}));
  sealStateFixture(contract); assignGeneration(contract);
  BOOST_CHECK_EQUAL(contract.generation->vocabularySize, 11);
  BOOST_CHECK(contract.generation->states[0].elementType == TensorElementType::Float32);
  for (const auto type : {10, 16}) {
    auto modern = contract;
    modern.generation.reset();
    for (auto& tensor : modern.tensors) {
      if (tensor.onnxElementType != 1) continue;
      tensor.onnxElementType = type; tensor.wireElementType = onnxTensorWireType(type);
    }
    for (auto& op : modern.operators)
      if (op.opType == "DequantizeLinear") op.opset = 19;
    sealStateFixture(modern); assignGeneration(modern);
    BOOST_CHECK(modern.generation->states[0].elementType == *onnxTensorWireType(type));
    modern.generation.reset();
    for (auto& op : modern.operators)
      if (op.opType == "DequantizeLinear") op.opset = 13;
    sealStateFixture(modern);
    BOOST_CHECK(generationUnproven(modern));
  }
  contract.generation.reset();
  stateFact(contract, "projection-scale").provenance = {"fixture"};
  sealStateFixture(contract);
  BOOST_CHECK(generationUnproven(contract));
}

BOOST_AUTO_TEST_CASE(DynamicIntegerProjectionPreservesTokenRolesThroughScaleProducts)
{
  auto contract = generationFixture();
  const auto b = stateSymbol("B"), s = stateSymbol("S");
  auto& weights = stateFact(contract, "wq");
  weights.onnxElementType = 3; weights.wireElementType = onnxTensorWireType(3);
  contract.tensors.push_back(stateTensor("quantized-activation", {b, s, stateFixed(7)}, 2));
  contract.tensors.push_back(stateTensor("activation-scale", {}));
  contract.tensors.push_back(stateTensor("activation-zero", {}, 2));
  auto scale = stateTensor("weight-scale", {stateFixed(24)});
  scale.provenance = {"initializer-header-v1"}; contract.tensors.push_back(scale);
  contract.tensors.push_back(stateTensor("combined-scales", {stateFixed(24)}));
  contract.tensors.push_back(stateTensor("query-int32", {b, s, stateFixed(24)}, 6));
  contract.tensors.push_back(stateTensor("query-float", {b, s, stateFixed(24)}));
  const auto query = std::find_if(contract.operators.begin(), contract.operators.end(),
    [](const auto& op) { return op.opType == "MatMul" && op.inputs[1] == "wq"; });
  BOOST_REQUIRE(query != contract.operators.end());
  auto cast = stateOp("Cast", {"query-int32"}, {"query-float"});
  cast.integerAttributes["to"] = 1;
  const std::vector<AutomaticOnnxOperatorFact> projection{
    stateOp("DynamicQuantizeLinear", {"embedding"},
      {"quantized-activation", "activation-scale", "activation-zero"}),
    stateOp("MatMulInteger", {"quantized-activation", "wq", "activation-zero"},
      {"query-int32"}), cast,
    stateOp("Mul", {"activation-scale", "weight-scale"}, {"combined-scales"}),
    stateOp("Mul", {"query-float", "combined-scales"}, {"q-raw"})};
  const auto offset = query - contract.operators.begin();
  contract.operators.erase(query);
  contract.operators.insert(contract.operators.begin() + offset, projection.begin(), projection.end());
  sealStateFixture(contract); assignGeneration(contract);
  BOOST_CHECK_EQUAL(contract.generation->tokenInput, "graph/ids");
  BOOST_CHECK_EQUAL(contract.generation->states[0].headSize, 8);
}

BOOST_AUTO_TEST_CASE(ShapeConstructedPositionsAndMaskAliasesHaveTheSameRootRoles)
{
  auto contract = generationFixture();
  const auto b = stateSymbol("B"), s = stateSymbol("S"), t = stateSymbol("T");
  for (const auto& item : {std::pair<std::string, std::vector<std::int64_t>>{"negative", {-1}},
                           {"zero-axes", {0}}}) {
    auto literal = stateTensor(item.first, {stateFixed(1)}, 7);
    literal.provenance = {"initializer-header-v1"};
    contract.tensors.push_back(literal);
    contract.integerLiterals["graph/" + item.first] = item.second;
  }
  contract.tensors.push_back(stateTensor("mask-alias", {b, t}, 7));
  contract.tensors.push_back(stateTensor("ids-shape", {stateFixed(2)}, 7));
  contract.tensors.push_back(stateTensor("s-scalar", {}, 7));
  contract.tensors.push_back(stateTensor("s-vector", {stateFixed(1)}, 7));
  contract.tensors.push_back(stateTensor("position-shape", {stateFixed(2)}, 7));
  contract.tensors.push_back(stateTensor("position-view", {b, s}, 7));
  for (auto& op : contract.operators) {
    if ((op.opType == "ReduceSum" || op.opType == "Shape") && op.inputs[0] == "mask")
      op.inputs[0] = "mask-alias";
    if (op.opType == "RotaryEmbedding") op.inputs[1] = "position-view";
  }
  std::vector<AutomaticOnnxOperatorFact> prefix{
    stateOp("Identity", {"mask"}, {"mask-alias"}),
    stateOp("Shape", {"ids"}, {"ids-shape"}),
    stateOp("Gather", {"ids-shape", "one"}, {"s-scalar"}),
    stateOp("Unsqueeze", {"s-scalar", "zero-axes"}, {"s-vector"}),
    stateOp("Concat", {"negative", "s-vector"}, {"position-shape"}),
    stateOp("Reshape", {"positions", "position-shape"}, {"position-view"})};
  prefix[4].integerAttributes["axis"] = 0;
  contract.operators.insert(contract.operators.begin(), prefix.begin(), prefix.end());
  sealStateFixture(contract); assignGeneration(contract);
  BOOST_CHECK_EQUAL(contract.generation->maskInput, "graph/mask");
  BOOST_CHECK_EQUAL(contract.generation->positionInput, "graph/positions");
  BOOST_CHECK_EQUAL(contract.generation->contextLimit, 64);
}

BOOST_AUTO_TEST_CASE(ReaderReprovesRolesInsteadOfTrustingAnAssertedGenerationObject)
{
  auto contract = generationFixture(); assignGeneration(contract);
  const auto original = nativeParseJson(contract.canonicalJson(stateControl()));
  for (const auto failure : {0, 1, 2, 3, 4, 5, 6, 7}) {
    auto value = original;
    if (failure == 0) value["generation"]["tokenInput"] = "graph/mask";
    if (failure == 1) value["generation"]["contextLimit"] = 65;
    if (failure == 2) value["generation"]["vocabularySize"] = 11.0;
    if (failure == 3) value["generation"]["extra"] = true;
    if (failure == 4) value["generation"]["states"][0]["update"] = "append_delta";
    if (failure == 5) value["generation"]["states"][0]["inputAxes"][2] = 1;
    if (failure == 6) value["generation"]["positionPolicy"] = "zero";
    if (failure == 7) value["generation"]["states"][0]["role"] = "value";
    BOOST_CHECK_THROW(AutomaticOnnxContract::fromCanonicalJson(nativeCanonicalJson(value),
                                                              stateControl()), std::exception);
  }
  contract.generation->contextLimit = 65;
  BOOST_CHECK_THROW(contract.canonicalJson(stateControl()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RejectsUnprovenTemporalProgramsAndHiddenRequiredPorts)
{
  for (const auto failure : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}) {
    auto contract = generationFixture();
    if (failure == 0) contract.integerLiterals["graph/axes"] = {0};
    if (failure == 1) contract.integerLiterals["graph/one"] = {-1};
    if (failure == 2) {
      contract.modelInputs.push_back("graph/extra");
      contract.tensors.push_back(stateTensor("extra", {stateFixed(1)}));
    }
    if (failure == 3) stateFact(contract, "old,key=0").dimensions->at(2) = stateFixed(0);
    if (failure == 4) stateFact(contract, "scores").dimensions->at(2) = stateFixed(12);
    if (failure == 5) {
      for (auto& op : contract.operators)
        if (op.opType == "GroupQueryAttention") op.integerAttributes["do_rotary"] = 1;
    }
    if (failure == 6) {
      for (auto& op : contract.operators)
        if (op.opType == "Cast" && op.outputs[0] == "total") op.integerAttributes["to"] = 7;
    }
    if (failure == 7) stateFact(contract, "cosine").dimensions->at(0) = stateFixed(0);
    if (failure == 8) {
      auto& tensor = stateFact(contract, "positions");
      tensor.onnxElementType = 1; tensor.wireElementType = TensorElementType::Float32;
    }
    if (failure == 9) {
      for (auto& op : contract.operators)
        if (op.outputs == std::vector<std::string>{"scores"}) op.inputs[0] = "embedding";
    }
    if (failure == 10) {
      stateFact(contract, "ids").dimensions->at(0) = stateFixed(1);
      stateFact(contract, "scores").dimensions->at(0) = stateFixed(2);
    }
    sealStateFixture(contract);
    BOOST_CHECK_MESSAGE(generationUnproven(contract), "unproven program class " << failure);
  }
}

BOOST_AUTO_TEST_CASE(SameShapePositionInputsRemainSemanticallyAmbiguous)
{
  auto contract = generationFixture();
  contract.modelInputs.push_back("graph/other-positions");
  contract.tensors.push_back(stateTensor("other-positions",
                                         {stateSymbol("B"), stateSymbol("S")}, 7));
  for (auto& op : contract.operators)
    if (op.opType == "RotaryEmbedding" && op.outputs[0] == "k") op.inputs[1] = "other-positions";
  sealStateFixture(contract);
  BOOST_CHECK_EXCEPTION(deriveAutomaticOnnxGeneration(contract, stateControl()),
    std::invalid_argument, [](const auto& error) {
      return std::string(error.what()) == "GENERATION_CONTRACT_AMBIGUOUS";
    });
}

BOOST_AUTO_TEST_CASE(OnlyProvenIntegerTokenCastsRetainTheTokenRole)
{
  for (const auto to : {6, 1}) {
    auto contract = generationFixture();
    const auto shape = std::vector<DimensionConstraint>{stateSymbol("B"), stateSymbol("S")};
    contract.tensors.push_back(stateTensor("intermediate-ids", shape, to));
    contract.tensors.push_back(stateTensor("cast-ids", shape, 7));
    for (auto& op : contract.operators)
      if (op.opType == "Gather" && op.inputs[0] == "words") op.inputs[1] = "cast-ids";
    auto narrow = stateOp("Cast", {"ids"}, {"intermediate-ids"});
    narrow.integerAttributes["to"] = to;
    auto widen = stateOp("Cast", {"intermediate-ids"}, {"cast-ids"});
    widen.integerAttributes["to"] = 7;
    contract.operators.insert(contract.operators.begin(), {narrow, widen});
    sealStateFixture(contract);
    if (to == 6) {
      assignGeneration(contract);
      BOOST_CHECK_EQUAL(contract.generation->tokenInput, "graph/ids");
    }
    else BOOST_CHECK(generationUnproven(contract));
  }
}

BOOST_AUTO_TEST_CASE(RenamesAllValuesAndDimensionSymbolsWithoutChangingRoles)
{
  auto contract = generationFixture();
  std::map<std::string, std::string> names;
  for (const auto& tensor : contract.tensors)
    names[tensor.name.substr(6)] = "opaque," + std::to_string(names.size()) + "=value";
  std::map<std::string, std::string> dimensions;
  for (auto& tensor : contract.tensors) {
    tensor.name = "graph/" + names.at(tensor.name.substr(6));
    if (!tensor.dimensions) continue;
    for (auto& axis : *tensor.dimensions) {
      if (axis.kind != DimensionConstraint::Kind::SYMBOL) continue;
      if (!dimensions.count(axis.symbol))
        dimensions[axis.symbol] = "graph/opaque-dimension-" + std::to_string(dimensions.size());
      axis.symbol = dimensions.at(axis.symbol);
    }
  }
  for (auto& op : contract.operators) {
    for (auto& name : op.inputs) if (!name.empty()) name = names.at(name);
    for (auto& name : op.outputs) if (!name.empty()) name = names.at(name);
  }
  for (auto* ports : {&contract.modelInputs, &contract.modelOutputs})
    for (auto& name : *ports) name = "graph/" + names.at(name.substr(6));
  std::map<std::string, std::vector<std::int64_t>> literals;
  for (const auto& [name, values] : contract.integerLiterals)
    literals["graph/" + names.at(name.substr(6))] = values;
  contract.integerLiterals = std::move(literals);
  sealStateFixture(contract); assignGeneration(contract);
  BOOST_CHECK_EQUAL(contract.generation->tokenInput, "graph/" + names.at("ids"));
  BOOST_CHECK_EQUAL(contract.generation->logitsOutput, "graph/" + names.at("scores"));
  BOOST_CHECK_EQUAL(contract.generation->positionInput, "graph/" + names.at("positions"));
  BOOST_CHECK_EQUAL(contract.generation->vocabularySize, 11);
  BOOST_CHECK_EQUAL(contract.generation->contextLimit, 64);
}

BOOST_AUTO_TEST_CASE(ProjectionHeadFoldUnfoldCancelsWithoutRedundantValueInfo)
{
  auto contract = generationFixture();
  const auto b = stateSymbol("B");
  for (const auto& item : {std::pair<std::string, std::vector<std::int64_t>>{
                           "fold-shape", {0, -1, 8}}, {"unfold-shape", {0, -1, 24}}}) {
    auto tensor = stateTensor(item.first, {stateFixed(3)}, 7);
    tensor.provenance = {"initializer-header-v1"};
    contract.tensors.push_back(tensor);
    contract.integerLiterals["graph/" + item.first] = item.second;
  }
  auto scale = stateTensor("norm-scale", {stateFixed(8)});
  scale.provenance = {"initializer-header-v1"}; contract.tensors.push_back(scale);
  contract.tensors.push_back(stateTensor("q-projection", {b, stateSymbol("S"), stateFixed(24)}));
  contract.tensors.push_back(stateTensor("q-folded", {b, {}, stateFixed(8)}));
  contract.tensors.push_back(stateTensor("q-normalized", {b, {}, stateFixed(8)}));
  for (auto& op : contract.operators)
    if (op.opType == "MatMul" && op.outputs[0] == "q-raw") op.outputs[0] = "q-projection";
  const auto rotary = std::find_if(contract.operators.begin(), contract.operators.end(),
    [](const auto& op) { return op.opType == "RotaryEmbedding"; });
  const std::vector<AutomaticOnnxOperatorFact> normalization{
    stateOp("Reshape", {"q-projection", "fold-shape"}, {"q-folded"}),
    stateOp("SimplifiedLayerNormalization", {"q-folded", "norm-scale"}, {"q-normalized"}),
    stateOp("Reshape", {"q-normalized", "unfold-shape"}, {"q-raw"})};
  contract.operators.insert(rotary, normalization.begin(), normalization.end());
  const std::set<std::string> ports(contract.modelInputs.begin(), contract.modelInputs.end());
  const std::set<std::string> outputs(contract.modelOutputs.begin(), contract.modelOutputs.end());
  for (auto& tensor : contract.tensors) {
    if (ports.count(tensor.name) || outputs.count(tensor.name) ||
        tensor.provenance == std::vector<std::string>{"initializer-header-v1"}) continue;
    tensor.dimensions.reset(); tensor.onnxElementType = 0; tensor.wireElementType.reset();
  }
  sealStateFixture(contract); assignGeneration(contract);
  BOOST_CHECK_EQUAL(contract.generation->tokenInput, "graph/ids");
  BOOST_CHECK_EQUAL(contract.generation->states[0].headSize, 8);
  BOOST_CHECK_EQUAL(contract.generation->vocabularySize, 11);
  auto constrained = contract;
  constrained.generation.reset();
  stateFact(constrained, "q-folded").dimensions =
    std::vector<DimensionConstraint>{b, stateFixed(3), stateFixed(8)};
  sealStateFixture(constrained); assignGeneration(constrained);
  BOOST_CHECK_EQUAL(constrained.generation->tokenInput, "graph/ids");
  // Changing the final feature width leaves a non-unit sequence factor.
  contract.generation.reset();
  contract.integerLiterals["graph/unfold-shape"] = {0, -1, 8};
  sealStateFixture(contract);
  BOOST_CHECK(generationUnproven(contract));
}

BOOST_AUTO_TEST_CASE(FloatingScoreAndCacheFactsPreserveOriginalScalarTypes)
{
  for (const auto type : {1, 10, 16}) {
    auto contract = generationFixture();
    for (auto& tensor : contract.tensors) {
      if (tensor.onnxElementType != 1) continue;
      tensor.onnxElementType = type; tensor.wireElementType = onnxTensorWireType(type);
    }
    sealStateFixture(contract); assignGeneration(contract);
    BOOST_CHECK(contract.generation->states[0].elementType == *onnxTensorWireType(type));
    const auto encoded = contract.canonicalJson(stateControl());
    const auto restored = AutomaticOnnxContract::fromCanonicalJson(encoded, stateControl());
    BOOST_REQUIRE(restored.generation);
    BOOST_CHECK_EQUAL(restored.canonicalJson(stateControl()), encoded);
  }
}

BOOST_AUTO_TEST_CASE(CompleteSourceProofRemainsBoundedAndCancellable)
{
  const auto contract = generationFixture();
  auto control = stateControl();
  control.deadline = std::chrono::steady_clock::now();
  BOOST_CHECK_THROW(deriveAutomaticOnnxGeneration(contract, control), std::runtime_error);
  control = stateControl(); control.maxAssembledBytes = 1;
  BOOST_CHECK_THROW(deriveAutomaticOnnxGeneration(contract, control), std::invalid_argument);
  for (const auto fence : {1, 30, 100, 300}) {
    int calls = 0; control = stateControl();
    control.requireActive = [&] { if (++calls == fence) throw std::runtime_error("cancelled"); };
    BOOST_CHECK_THROW(deriveAutomaticOnnxGeneration(contract, control), std::runtime_error);
  }
}

BOOST_AUTO_TEST_CASE(TransposedMaskAndOneElementVectorTotalHaveFormalRoles)
{
  auto contract = generationFixture();
  stateFact(contract, "mask").dimensions =
    std::vector<DimensionConstraint>{stateSymbol("T"), stateSymbol("B")};
  for (const auto* name : {"sum", "sum-minus-one", "lengths"})
    stateFact(contract, name).dimensions =
      std::vector<DimensionConstraint>{stateFixed(1), stateSymbol("B")};
  contract.integerLiterals["graph/axes"] = {0};
  auto index = stateTensor("zero-index", {stateFixed(1)}, 7);
  index.provenance = {"initializer-header-v1"}; contract.tensors.push_back(index);
  contract.integerLiterals["graph/zero-index"] = {0};
  for (auto& op : contract.operators)
    if (op.opType == "Gather" && op.outputs[0] == "total64") op.inputs[1] = "zero-index";
  for (const auto* name : {"total64", "total"})
    stateFact(contract, name).dimensions = std::vector<DimensionConstraint>{stateFixed(1)};
  sealStateFixture(contract); assignGeneration(contract);
  BOOST_CHECK_EQUAL(contract.generation->maskBatchAxis, 1);
  BOOST_CHECK_EQUAL(contract.generation->maskContextAxis, 0);
  BOOST_CHECK_EQUAL(contract.generation->tokenInput, "graph/ids");
}

BOOST_AUTO_TEST_CASE(DistinctInputPoliciesCannotShareOneRootPort)
{
  for (const auto& alias : {std::pair<std::string, std::string>{"positions", "ids"},
                            {"mask", "ids"}, {"positions", "mask"}}) {
    auto contract = generationFixture();
    for (auto& op : contract.operators)
      for (auto& input : op.inputs) if (input == alias.first) input = alias.second;
    const auto port = "graph/" + alias.first;
    contract.modelInputs.erase(std::remove(contract.modelInputs.begin(),
                                           contract.modelInputs.end(), port),
                               contract.modelInputs.end());
    sealStateFixture(contract);
    BOOST_CHECK_EXCEPTION(deriveAutomaticOnnxGeneration(contract, stateControl()),
      AutomaticGenerationUnproven, [](const auto& error) {
        return std::string(error.what()) == "AUTOMATIC_GENERATION_ROLE_ALIAS";
      });
  }
}

BOOST_AUTO_TEST_CASE(AuxiliaryFixedDimensionsMustAgreeWithEveryRequiredBatchAndSequence)
{
  for (const auto failure : {0, 1, 2, 3, 4}) {
    auto contract = generationFixture();
    stateFact(contract, "ids").dimensions->at(0) = stateFixed(1);
    if (failure == 0) stateFact(contract, "mask").dimensions->at(0) = stateFixed(2);
    if (failure == 1) stateFact(contract, "positions").dimensions->at(0) = stateFixed(2);
    if (failure == 2) stateFact(contract, "positions").dimensions->at(1) = stateFixed(2);
    if (failure == 3) {
      contract.tensors.push_back(stateTensor("mask-view",
        {stateFixed(2), stateSymbol("T")}, 7));
      for (auto& op : contract.operators)
        for (auto& input : op.inputs) if (input == "mask") input = "mask-view";
      contract.operators.insert(contract.operators.begin(),
        stateOp("Identity", {"mask"}, {"mask-view"}));
    }
    if (failure == 4) {
      stateFact(contract, "ids").dimensions->at(0) = stateSymbol("B");
      stateFact(contract, "old,key=0").dimensions->at(0) = stateFixed(1);
      stateFact(contract, "positions").dimensions->at(0) = stateFixed(2);
    }
    sealStateFixture(contract);
    BOOST_CHECK_MESSAGE(generationUnproven(contract), "fixed auxiliary conflict " << failure);
  }
}

BOOST_AUTO_TEST_CASE(SymbolicRootCannotHideFixedActivationAndCacheConflicts)
{
  for (const auto& name : {"q", "q-raw", "k-raw", "v", "embedding"}) {
    auto contract = generationFixture();
    for (const auto* cache : {"old,key=0", "old,value=0"})
      stateFact(contract, cache).dimensions->at(0) = stateFixed(1);
    stateFact(contract, name).dimensions->at(0) = stateFixed(2);
    sealStateFixture(contract);
    BOOST_CHECK_MESSAGE(generationUnproven(contract), "activation batch conflict " << name);
  }
  auto contract = generationFixture();
  stateFact(contract, "q-raw").dimensions->at(1) = stateFixed(2);
  sealStateFixture(contract);
  BOOST_CHECK(generationUnproven(contract));
}

BOOST_AUTO_TEST_CASE(SharedActivationDiamondsUseBoundedPerProofWork)
{
  auto contract = generationFixture();
  std::string previous = "embedding";
  std::vector<AutomaticOnnxOperatorFact> diamonds;
  for (int i = 0; i < 22; ++i) {
    const auto gate = "gate-" + std::to_string(i);
    const auto product = "product-" + std::to_string(i);
    diamonds.push_back(stateOp("Sigmoid", {previous}, {gate}));
    diamonds.push_back(stateOp("Mul", {previous, gate}, {product}));
    for (const auto& name : {gate, product}) {
      auto tensor = stateTensor(name, {});
      tensor.dimensions.reset(); tensor.onnxElementType = 0; tensor.wireElementType.reset();
      contract.tensors.push_back(tensor);
    }
    previous = product;
  }
  for (auto& op : contract.operators)
    if (op.opType == "MatMul" && op.inputs[0] == "embedding") op.inputs[0] = previous;
  const auto embedding = std::find_if(contract.operators.begin(), contract.operators.end(),
    [](const auto& op) { return op.outputs == std::vector<std::string>{"embedding"}; });
  contract.operators.insert(embedding + 1, diamonds.begin(), diamonds.end());
  sealStateFixture(contract);
  for (int repeat = 0; repeat < 2; ++repeat) {
    std::uint64_t calls = 0;
    auto control = stateControl();
    control.requireActive = [&] {
      if (++calls > 60000) throw std::runtime_error("shared DAG work exceeded bound");
    };
    const auto proof = deriveAutomaticOnnxGeneration(contract, control);
    BOOST_REQUIRE(proof);
    BOOST_CHECK_EQUAL(proof->tokenInput, "graph/ids");
    BOOST_CHECK_LT(calls, 60000);
  }
}

BOOST_AUTO_TEST_CASE(MissingAnnotationsCannotHideActualActivationAndCacheTypeMismatch)
{
  auto contract = generationFixture();
  const std::set<std::string> cachePorts{"graph/old,key=0", "graph/old,value=0",
                                        "graph/new,key=0", "graph/new,value=0"};
  for (auto& tensor : contract.tensors) {
    if (tensor.onnxElementType != 1 || cachePorts.count(tensor.name)) continue;
    if (tensor.provenance == std::vector<std::string>{"initializer-header-v1"} ||
        tensor.name == "graph/scores") {
      tensor.onnxElementType = 10; tensor.wireElementType = onnxTensorWireType(10);
    }
    else {
      tensor.onnxElementType = 0; tensor.wireElementType.reset(); tensor.dimensions.reset();
    }
  }
  sealStateFixture(contract);
  BOOST_CHECK(generationUnproven(contract));
}

BOOST_AUTO_TEST_CASE(SchemaFixedResultsConstrainEveryDeclaredSymbolUse)
{
  const auto fixture = [] {
    onnx::ModelProto source;
    auto* graph = source.mutable_graph();
    auto* shape = graph->add_node(); shape->set_op_type("Shape");
    shape->add_input("matrix"); shape->add_output("shape");
    auto* transpose = graph->add_node(); transpose->set_op_type("Transpose");
    transpose->add_input("shared"); transpose->add_output("copied");
    auto* declared = graph->add_value_info(); declared->set_name("shape");
    auto* type = declared->mutable_type()->mutable_tensor_type();
    type->set_elem_type(7); type->mutable_shape()->add_dim()->set_dim_param("Length");
    AutomaticOnnxContract contract;
    contract.tensors = {
      stateTensor("matrix", {stateFixed(4), stateFixed(5)}),
      stateTensor("shape", {stateSymbol("Length")}, 7),
      stateTensor("shared", {stateSymbol("Length")}),
      stateTensor("copied", {stateSymbol("Alias")})};
    contract.operators = {stateOp("Shape", {"matrix"}, {"shape"}),
                          stateOp("Transpose", {"shared"}, {"copied"})};
    for (std::size_t index = 0; index < contract.operators.size(); ++index)
      contract.operators[index].nodeIndex = index;
    return std::make_pair(source, contract);
  };
  for (const bool alreadyRefined : {false, true}) {
    auto [source, contract] = fixture();
    // Simulate an earlier standard inference pass that replaced the original
    // declaration. The original symbol must still constrain all other uses.
    if (alreadyRefined) stateFact(contract, "shape").dimensions->at(0) = stateFixed(2);
    refineAutomaticOnnxTensorFacts(source, *onnx::OpSchemaRegistry::Instance(),
                                   contract, stateControl());
    for (const auto& name : {"shape", "shared", "copied"}) {
      const auto& axis = stateFact(contract, name).dimensions->at(0);
      BOOST_CHECK(axis.kind == DimensionConstraint::Kind::FIXED);
      BOOST_CHECK_EQUAL(axis.fixed, 2);
    }
  }
  {
    auto [source, contract] = fixture();
    source.mutable_graph()->mutable_node()->SwapElements(0, 1);
    std::swap(contract.operators[0], contract.operators[1]);
    for (std::size_t index = 0; index < contract.operators.size(); ++index)
      contract.operators[index].nodeIndex = index;
    // Infer Transpose's dimensions independently so Length == Alias remains
    // visible before the later Shape callback fixes Length to two.
    refineAutomaticOnnxTensorFacts(source, *onnx::OpSchemaRegistry::Instance(),
                                   contract, stateControl());
    BOOST_CHECK_EQUAL(stateFact(contract, "copied").dimensions->at(0).fixed, 2);
  }
  {
    auto [source, contract] = fixture();
    source.mutable_graph()->mutable_node()->SwapElements(0, 1);
    std::swap(contract.operators[0], contract.operators[1]);
    for (std::size_t index = 0; index < contract.operators.size(); ++index)
      contract.operators[index].nodeIndex = index;
    auto* shape = source.mutable_graph()->add_node(); shape->set_op_type("Shape");
    shape->add_input("cube"); shape->add_output("other-shape");
    contract.tensors.push_back(stateTensor("cube", {stateFixed(1), stateFixed(2), stateFixed(3)}));
    contract.tensors.push_back(stateTensor("other-shape", {stateSymbol("Alias")}, 7));
    auto op = stateOp("Shape", {"cube"}, {"other-shape"}); op.nodeIndex = 2;
    contract.operators.push_back(op);
    BOOST_CHECK_THROW(refineAutomaticOnnxTensorFacts(source, *onnx::OpSchemaRegistry::Instance(),
                                                    contract, stateControl()), std::invalid_argument);
  }
  {
    auto [source, contract] = fixture();
    stateFact(contract, "shared").dimensions->at(0).upperBound = 1;
    BOOST_CHECK_THROW(refineAutomaticOnnxTensorFacts(source, *onnx::OpSchemaRegistry::Instance(),
                                                    contract, stateControl()), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(FullCycleBoundsReserveEveryRoundAndUsePortablePolicyIdentity)
{
  const GenerationModelFile file(generationSourceModel());
  const auto model = inspectAutomaticOnnxModel({file.path}, stateControl());
  BOOST_REQUIRE(model.contract.generation);
  const auto inputs = generationInputs(2, 3);
  const auto envelope = bindAutomaticOnnxShapes(model.contract, inputs, 9, 16, stateControl());
  const auto check = [&](const std::string& name, std::size_t axis,
                          std::uint64_t minimum, std::uint64_t maximum) {
    const auto& bounds = envelope.tensors.at("graph/" + name);
    BOOST_REQUIRE(bounds.minimumDimensions);
    BOOST_REQUIRE(bounds.maximumDimensions);
    BOOST_CHECK_EQUAL(bounds.minimumDimensions->at(axis), minimum);
    BOOST_REQUIRE(bounds.maximumDimensions->at(axis));
    BOOST_CHECK_EQUAL(*bounds.maximumDimensions->at(axis), maximum);
  };
  check("ids", 1, 1, 3);
  check("mask", 1, 3, 11);
  check("old,key=0", 2, 0, 10);
  check("new,key=0", 2, 3, 11);
  check("scores", 1, 1, 3);
  check("attended", 1, 1, 3);
  check("attended", 2, 24, 24);
  check("normalized", 2, 7, 7);
  check("residual-sum", 2, 7, 7);
  BOOST_REQUIRE(envelope.tensors.at("graph/new,key=0").maxBytes);
  BOOST_CHECK_EQUAL(*envelope.tensors.at("graph/new,key=0").maxBytes, 2 * 11 * 8 * 4);
  const auto legacy = inspectNativeOnnxSourceGraph(model.opened.source, model.descriptor,
                                                  stateControl());
  BOOST_CHECK_EQUAL(legacy.graph.graphDigest, model.inspection.graph.graphDigest);
  BOOST_CHECK_EQUAL(legacy.canonicalIdentity.graphDigest, model.inspection.canonicalIdentity.graphDigest);
  auto otherControl = stateControl(); otherControl.maxAssembledBytes /= 2;
  const auto other = bindAutomaticOnnxShapes(model.contract, inputs, 9, 16, otherControl);
  BOOST_CHECK_EQUAL(other.envelopeDigest, envelope.envelopeDigest);
  BOOST_CHECK_EQUAL(other.budgetDigest, envelope.budgetDigest);
  for (std::uint64_t round = 0; round < 9; ++round) {
    const auto expected = automaticOnnxRoundInputShapes(model.contract, envelope, round, stateControl());
    BOOST_CHECK_EQUAL(expected.at("graph/ids")[1], round == 0 ? 3 : 1);
    BOOST_CHECK_EQUAL(expected.at("graph/old,key=0")[2], round == 0 ? 0 : 2 + round);
    BOOST_CHECK_EQUAL(expected.at("graph/mask")[1], 3 + round);
  }
  BOOST_CHECK_THROW(automaticOnnxRoundInputShapes(model.contract, envelope, 9, stateControl()),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CompactShapeSeedPreservesAllOwnedGenerationRounds)
{
  const GenerationModelFile file(generationSourceModel());
  const auto model = inspectAutomaticOnnxModel({file.path}, stateControl());
  const auto envelope = bindAutomaticOnnxShapes(model.contract, generationInputs(2, 3),
                                               9, 16, stateControl());
  const auto original = automaticOnnxShapeSeed(envelope);
  const auto seed = nativeAutomaticShapeSeedFromJson(nativeParseJson(
    nativeCanonicalJson(nativeAutomaticShapeSeedToJson(original))));
  BOOST_CHECK(seed.inputShapes == original.inputShapes);
  BOOST_CHECK(seed.inputShapes.at("graph/old,key=0") ==
              std::vector<std::int64_t>({2, 1, 0, 8}));
  const auto rebound = rebindAutomaticOnnxShapeSeed(model.contract, seed, stateControl());
  BOOST_CHECK_EQUAL(rebound.envelopeDigest, envelope.envelopeDigest);
  for (std::uint64_t round = 0; round != 9; ++round)
    BOOST_CHECK(automaticOnnxRoundInputShapes(model.contract, rebound, round, stateControl()) ==
                automaticOnnxRoundInputShapes(model.contract, envelope, round, stateControl()));
}

BOOST_AUTO_TEST_CASE(RoundValidationRejectsWithinHullButWrongRoundStateAndBadInputPolicies)
{
  auto contract = generationFixture(); assignGeneration(contract);
  const auto initial = generationInputs(1, 3);
  const auto envelope = bindAutomaticOnnxShapes(contract, initial, 9, 16, stateControl());
  const std::vector<NamedTensor> outputs{generationFloat("scores", {1, 1, 11}),
    generationFloat("new,key=0", {1, 1, 4, 8}), generationFloat("new,value=0", {1, 1, 4, 8})};
  BOOST_CHECK_NO_THROW(validateAutomaticOnnxTensors(contract, envelope, contract.modelOutputs,
    outputs, 1, stateControl()));
  BOOST_CHECK_THROW(validateAutomaticOnnxTensors(contract, envelope, contract.modelOutputs,
    outputs, 2, stateControl()), std::invalid_argument);
  for (const auto failure : {0, 1, 2, 3, 4}) {
    auto inputs = initial;
    if (failure == 0) inputs[0].payload[0] = 11;
    if (failure == 1) std::fill(inputs[0].payload.begin(), inputs[0].payload.begin() + 8, 255);
    if (failure == 2) inputs[1].payload[0] = 0;
    if (failure == 3) inputs[2].payload[0] = 1;
    if (failure == 4) inputs[3] = generationFloat("old,key=0", {1, 1, 1, 8});
    BOOST_CHECK_THROW(bindAutomaticOnnxShapes(contract, inputs, 9, 16, stateControl()),
                      std::invalid_argument);
  }
  auto decode = generationInputs(1, 1, 3);
  BOOST_CHECK_NO_THROW(validateAutomaticOnnxTensors(contract, envelope, contract.modelInputs,
    decode, 1, stateControl()));
  decode[2].payload[0] = 2;
  BOOST_CHECK_THROW(validateAutomaticOnnxTensors(contract, envelope, contract.modelInputs,
    decode, 1, stateControl()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(GenerationContextAndInitialEmptyStateHaveExactRequestBoundaries)
{
  auto contract = generationFixture(); assignGeneration(contract);
  const auto inputs = generationInputs(1, 3);
  BOOST_CHECK_NO_THROW(bindAutomaticOnnxShapes(contract, inputs, 9, 12, stateControl()));
  BOOST_CHECK_THROW(bindAutomaticOnnxShapes(contract, inputs, 9, 11, stateControl()),
                    std::invalid_argument);
  BOOST_CHECK_THROW(bindAutomaticOnnxShapes(contract, inputs, UINT64_MAX, UINT64_MAX, stateControl()),
                    std::invalid_argument);
  BOOST_CHECK_THROW(bindAutomaticOnnxShapes(contract, generationInputs(1, 0), 9, 16, stateControl()),
                    std::invalid_argument);
  BOOST_CHECK_THROW(bindAutomaticOnnxShapes(contract, generationInputs(1, 3, 1), 9, 16, stateControl()),
                    std::invalid_argument);
  BOOST_CHECK_NO_THROW(bindAutomaticOnnxShapes(contract, generationInputs(1, 1), 63, 64,
                                               stateControl()));
  BOOST_CHECK_THROW(bindAutomaticOnnxShapes(contract, generationInputs(1, 1), 64, 65,
                                              stateControl()), std::invalid_argument);
  const auto single = bindAutomaticOnnxShapes(contract, inputs, 1, 4, stateControl());
  BOOST_CHECK_EQUAL(*single.tensors.at("graph/old,key=0").maxBytes, 0);
  BOOST_CHECK_EQUAL(*single.tensors.at("graph/new,key=0").maxBytes, 3 * 8 * 4);
  BOOST_CHECK_THROW(automaticOnnxRoundInputShapes(contract, single, 1, stateControl()),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(Int32RootInputsUseTheSameDeclaredIntegerPolicies)
{
  auto contract = generationFixture();
  for (const auto* name : {"ids", "mask", "positions", "sum", "sum-minus-one"}) {
    auto& tensor = stateFact(contract, name);
    tensor.onnxElementType = 6; tensor.wireElementType = TensorElementType::Int32;
  }
  auto one = stateTensor("one-i32", {}, 6);
  one.provenance = {"initializer-header-v1"}; contract.tensors.push_back(one);
  contract.integerLiterals["graph/one-i32"] = {1};
  contract.tensors.push_back(stateTensor("wide-positions",
    {stateSymbol("B"), stateSymbol("S")}, 7));
  for (auto& op : contract.operators) {
    if (op.opType == "Sub" && op.outputs[0] == "sum-minus-one") op.inputs[1] = "one-i32";
    if (op.opType == "RotaryEmbedding") op.inputs[1] = "wide-positions";
  }
  auto cast = stateOp("Cast", {"positions"}, {"wide-positions"});
  cast.integerAttributes["to"] = 7;
  contract.operators.insert(contract.operators.begin(), cast);
  sealStateFixture(contract); assignGeneration(contract);
  auto inputs = generationInputs(2, 3);
  for (std::size_t index = 0; index < 3; ++index) {
    auto& tensor = inputs[index];
    std::vector<std::uint8_t> bytes;
    for (std::size_t offset = 0; offset < tensor.payload.size(); offset += 8)
      bytes.insert(bytes.end(), tensor.payload.begin() + offset, tensor.payload.begin() + offset + 4);
    tensor.payload = std::move(bytes); tensor.elementType = TensorElementType::Int32;
  }
  BOOST_CHECK_NO_THROW(bindAutomaticOnnxShapes(contract, inputs, 3, 16, stateControl()));
  std::fill(inputs[0].payload.begin(), inputs[0].payload.begin() + 4, 255);
  BOOST_CHECK_THROW(bindAutomaticOnnxShapes(contract, inputs, 3, 16, stateControl()),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(FullCycleCannotIgnoreInteriorFixedAxesOrShrinkRequiredInputRanges)
{
  for (const auto axis : {0, 1}) {
    auto contract = generationFixture();
    stateFact(contract, "q").dimensions->at(axis) = stateFixed(axis == 0 ? 2 : 1);
    sealStateFixture(contract); assignGeneration(contract);
    BOOST_CHECK_THROW(bindAutomaticOnnxShapes(contract, generationInputs(1, 3), 3, 16,
                                                stateControl()), std::invalid_argument);
  }
  auto contract = generationFixture();
  stateFact(contract, "mask").dimensions->at(1).upperBound = 4;
  sealStateFixture(contract); assignGeneration(contract);
  BOOST_CHECK_THROW(bindAutomaticOnnxShapes(contract, generationInputs(1, 3), 4, 16,
                                              stateControl()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(FullCycleRemainsCancellableBoundedAndSealed)
{
  auto contract = generationFixture(); assignGeneration(contract);
  const auto inputs = generationInputs(1, 3);
  auto envelope = bindAutomaticOnnxShapes(contract, inputs, 3, 16, stateControl());
  envelope.tensors.at("graph/new,key=0").maximumDimensions->at(2) = 100;
  BOOST_CHECK_THROW(automaticOnnxRoundInputShapes(contract, envelope, 1, stateControl()),
                    std::invalid_argument);
  for (const auto fence : {1, 30, 300}) {
    auto control = stateControl(); int calls = 0;
    control.requireActive = [&] { if (++calls == fence) throw std::runtime_error("cancelled"); };
    BOOST_CHECK_THROW(bindAutomaticOnnxShapes(contract, inputs, 3, 16, control), std::runtime_error);
  }
  auto control = stateControl(); control.maxAssembledBytes = 1;
  BOOST_CHECK_THROW(bindAutomaticOnnxShapes(contract, inputs, 3, 16, control), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(FullCycleUsesTheActualInputAndOutputStateLayouts)
{
  auto contract = generationFixture();
  std::vector<AutomaticOnnxOperatorFact> inputs;
  for (const auto& role : {std::string("key"), std::string("value")}) {
    const auto past = "old," + role + "=0", present = "new," + role + "=0";
    auto inside = stateFact(contract, past); inside.name += "/inside";
    contract.tensors.push_back(inside);
    stateFact(contract, past).dimensions = std::vector<DimensionConstraint>{
      stateSymbol("B"), stateSymbol("P"), stateFixed(1), stateFixed(8)};
    auto source = stateOp("Transpose", {past}, {past + "/inside"});
    source.integerListAttributes["perm"] = {0, 2, 1, 3}; inputs.push_back(source);
    auto output = stateFact(contract, present); output.name += "/inside";
    contract.tensors.push_back(output);
    stateFact(contract, present).dimensions = std::vector<DimensionConstraint>{
      stateSymbol("B"), {}, stateFixed(8), stateFixed(1)};
    for (auto& op : contract.operators) {
      if (op.opType != "GroupQueryAttention") continue;
      for (auto& name : op.inputs) if (name == past) name += "/inside";
      for (auto& name : op.outputs) if (name == present) name += "/inside";
    }
    auto target = stateOp("Transpose", {present + "/inside"}, {present});
    target.integerListAttributes["perm"] = {0, 2, 3, 1};
    contract.operators.push_back(target);
  }
  contract.operators.insert(contract.operators.begin(), inputs.begin(), inputs.end());
  sealStateFixture(contract); assignGeneration(contract);
  auto values = generationInputs(1, 3);
  values[3].shape = {1, 0, 1, 8}; values[4].shape = {1, 0, 1, 8};
  const auto envelope = bindAutomaticOnnxShapes(contract, values, 3, 16, stateControl());
  const auto decode = automaticOnnxRoundInputShapes(contract, envelope, 1, stateControl());
  BOOST_CHECK(decode.at("graph/old,key=0") == std::vector<std::int64_t>({1, 3, 1, 8}));
  const std::vector<NamedTensor> outputs{generationFloat("scores", {1, 1, 11}),
    generationFloat("new,key=0", {1, 4, 8, 1}), generationFloat("new,value=0", {1, 4, 8, 1})};
  BOOST_CHECK_NO_THROW(validateAutomaticOnnxTensors(contract, envelope, contract.modelOutputs,
    outputs, 1, stateControl()));
}

BOOST_AUTO_TEST_CASE(DecodeMustSatisfyIntermediateIntegerReshapeConstraints)
{
  auto source = generationSourceModel(false);
  auto* graph = source.mutable_graph();
  for (const auto& item : {std::pair<std::string, std::int64_t>{"fold-target", 16},
                           {"unfold-target", 24}}) {
    auto* shape = graph->add_initializer(); shape->set_name(item.first);
    shape->set_data_type(onnx::TensorProto::INT64); shape->add_dims(3);
    for (const auto value : {std::int64_t{0}, std::int64_t{-1}, item.second})
      shape->add_int64_data(value);
  }
  std::vector<onnx::NodeProto> nodes;
  for (auto node : graph->node()) {
    if (node.op_type() == "MatMul" && node.output(0) == "q-raw")
      node.set_output(0, "q-projection");
    if (node.op_type() == "RotaryEmbedding" && node.input(0) == "q-raw") {
      for (const auto& item : {std::array<std::string, 3>{"q-projection", "fold-target", "q-fold"},
                               {"q-fold", "unfold-target", "q-raw"}}) {
        onnx::NodeProto reshape; reshape.set_op_type("Reshape");
        reshape.add_input(item[0]); reshape.add_input(item[1]); reshape.add_output(item[2]);
        nodes.push_back(std::move(reshape));
      }
    }
    nodes.push_back(std::move(node));
  }
  graph->clear_node();
  for (const auto& node : nodes) graph->add_node()->CopyFrom(node);
  const GenerationModelFile file(source);
  const auto model = inspectAutomaticOnnxModel({file.path}, stateControl());
  BOOST_REQUIRE(model.contract.generation);
  // Prefill S=2 gives an integral folded length3; decode S=1 would require1.5.
  BOOST_CHECK_NO_THROW(bindAutomaticOnnxShapes(model.contract, generationInputs(1, 2), 1, 16,
                                               stateControl()));
  BOOST_CHECK_THROW(bindAutomaticOnnxShapes(model.contract, generationInputs(1, 2), 2, 16,
                                              stateControl()), std::invalid_argument);
}

#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
BOOST_AUTO_TEST_CASE(IndependentOrtPrefillAndEightDecodeOutputsFitTheSealedFullCycle)
{
  const GenerationModelFile file(generationSourceModel());
  const auto model = inspectAutomaticOnnxModel({file.path}, stateControl());
  BOOST_REQUIRE(model.contract.generation);
  Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "spec233-full-cycle-reference");
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1); options.SetInterOpNumThreads(1);
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  Ort::Session session(environment, file.path.c_str(), options);
  const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  const std::array<const char*, 3> outputNames{"scores", "new,key=0", "new,value=0"};
  for (const std::int64_t batch : {1, 2}) for (const std::int64_t prompt : {1, 3}) {
    const auto envelope = bindAutomaticOnnxShapes(model.contract, generationInputs(batch, prompt),
                                                   9, 16, stateControl());
    std::vector<NamedTensor> previous;
    std::vector<std::int64_t> next(batch);
    std::vector<std::vector<std::int64_t>> consumed(batch);
    for (std::uint64_t round = 0; round < 9; ++round) {
      const auto sequence = round == 0 ? prompt : 1;
      const auto past = round == 0 ? 0 : prompt + static_cast<std::int64_t>(round) - 1;
      auto inputs = generationInputs(batch, sequence, past);
      std::vector<std::int64_t> tokens;
      for (std::int64_t b = 0; b < batch; ++b) for (std::int64_t s = 0; s < sequence; ++s) {
        const auto token = round == 0 ? (b + s + 1) % 11 : next[b];
        tokens.push_back(token); consumed[b].push_back(token);
      }
      inputs[0] = generationInteger("ids", {batch, sequence}, tokens);
      if (round != 0) {
        inputs[3].payload = previous[1].payload;
        inputs[4].payload = previous[2].payload;
      }
      validateAutomaticOnnxTensors(model.contract, envelope, model.contract.modelInputs,
                                    inputs, round, stateControl());
      std::vector<const char*> inputNames;
      std::vector<Ort::Value> values;
      std::uint64_t emptyStorage = 0;
      for (auto& input : inputs) {
        inputNames.push_back(input.name.c_str());
        const auto type = input.elementType == TensorElementType::Int64 ?
          ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 : ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
        values.push_back(Ort::Value::CreateTensor(memory,
          input.payload.empty() ? static_cast<void*>(&emptyStorage) : input.payload.data(),
          input.payload.size(), input.shape.data(), input.shape.size(), type));
      }
      auto actual = session.Run(Ort::RunOptions{nullptr}, inputNames.data(), values.data(),
        values.size(), outputNames.data(), outputNames.size());
      previous.clear();
      for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto info = actual[index].GetTensorTypeAndShapeInfo();
        BOOST_REQUIRE(info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        NamedTensor output{outputNames[index], TensorElementType::Float32, info.GetShape(),
          std::vector<std::uint8_t>(info.GetElementCount() * sizeof(float))};
        std::memcpy(output.payload.data(), actual[index].GetTensorData<float>(), output.payload.size());
        previous.push_back(std::move(output));
      }
      validateAutomaticOnnxTensors(model.contract, envelope, model.contract.modelOutputs,
                                    previous, round, stateControl());
      BOOST_CHECK(previous[0].shape == std::vector<std::int64_t>({batch, sequence, 11}));
      for (std::size_t role = 1; role < 3; ++role) {
        BOOST_CHECK(previous[role].shape ==
          std::vector<std::int64_t>({batch, 1, past + sequence, 8}));
        const auto* cache = actual[role].GetTensorData<float>();
        for (std::int64_t b = 0; b < batch; ++b) {
          for (std::size_t token = 0; token < consumed[b].size(); ++token) {
            float expected = 0;
            for (std::uint64_t hidden = 0; hidden < 7; ++hidden)
              expected += ((consumed[b][token] * 7 + hidden) % 11 + 1) * 0.01F *
                (role == 1 ? 0.02F : 0.03F);
            for (std::size_t dimension = 0; dimension < 8; ++dimension) {
              const auto offset = (b * consumed[b].size() + token) * 8 + dimension;
              BOOST_CHECK_SMALL(cache[offset] - expected, 1.0e-6F);
            }
          }
        }
      }
      const auto* scores = actual[0].GetTensorData<float>();
      for (std::int64_t b = 0; b < batch; ++b) {
        const auto* row = scores + (b * sequence + sequence - 1) * 11;
        for (std::size_t vocabulary = 0; vocabulary < 11; ++vocabulary)
          BOOST_CHECK(std::isfinite(row[vocabulary]));
        next[b] = std::max_element(row, row + 11) - row;
      }
    }
  }
}
#endif

BOOST_AUTO_TEST_CASE(ProviderSourceAdmissionRequiresActualGenerationPairsAndSingleBatch)
{
  const GenerationModelFile file(generationSourceModel());
  const auto model = inspectAutomaticOnnxModel({file.path}, stateControl());
  BOOST_REQUIRE(model.contract.generation);
  BOOST_REQUIRE_EQUAL(model.contract.generation->states.size(), 2U);
  BOOST_CHECK_EQUAL(model.contract.generation->vocabularySize, 11U);
  const auto binding = AutomaticOnnxSourceBinding::fromSourceMetadata(
    NativeJson{{"contract", nativeParseJson(model.contract.canonicalJson(stateControl()))},
               {"externalFiles", NativeJson::array()}},
    model.opened.modelDigest, 0, "", stateControl());
  const auto projectionFor = [&] (std::int64_t batch) {
    auto projection = fixture::automaticSourceProjection(model, automaticOnnxShapeSeed(
      bindAutomaticOnnxShapes(model.contract, generationInputs(batch, 3), 9, 16, stateControl())));
    auto& generation = projection.generationContract;
    generation.enabled = true;
    generation.mode = "TOKEN_STREAMING";
    generation.maxGeneratedTokens = 9;
    generation.streamingOperationStride = 1;
    generation.tokenInputName = "ids";
    generation.attentionMaskInputName = "mask";
    generation.positionIdsInputName = "positions";
    generation.stateInputNames = {"old,key=0", "old,value=0"};
    generation.stateOutputNames = {"new,key=0", "new,value=0"};
    generation.eosTokenIds = {10};
    return projection;
  };
  const auto projection = projectionFor(1);
  BOOST_REQUIRE_NO_THROW(validateNativeAutomaticSelectionBinding(projection));
  BOOST_CHECK_NO_THROW(admitAutomaticOnnxProjection(binding, projection, stateControl()));
  NativeRequestControl request;
  request.requestId = projection.requestId; request.attempt = projection.attempt;
  request.deadline = stateControl().deadline;
  const auto admitted = AutomaticOnnxRequestBinding::admit(binding, projection, request);
  RoleExecutionContext context;
  context.requestId = projection.requestId; context.attemptEpoch = projection.attempt;
  context.role = projection.executionRole.roleId;
  // Exercise the admitted shape/value path without re-encoding/rebinding the
  // envelope each round. This is not automatic backend GEN qualification.
  for (std::uint64_t round = 0; round < 9; ++round) {
    context.inferenceEpoch = round;
    const std::int64_t sequence = round == 0 ? 3 : 1;
    const std::int64_t past = round == 0 ? 0 : 3 + round - 1;
    auto inputs = generationInputs(1, sequence, past);
    std::vector<AutomaticOnnxTensorObservation> observed;
    std::vector<NamedTensorView> controls;
    for (const auto& input : inputs) {
      observed.push_back({input.name, input.elementType, input.shape, input.payload.size()});
      const auto& names = admitted->controlInputs();
      if (std::find(names.begin(), names.end(), input.name) != names.end())
        controls.push_back({input.name, input.elementType, input.shape,
          ndn::span<const std::uint8_t>(input.payload.data(), input.payload.size())});
    }
    BOOST_CHECK_NO_THROW(admitted->validateInputs(observed, controls, context));
    BOOST_CHECK_THROW(admitted->validateInputs(observed, {}, context), std::invalid_argument);
    inputs[1].payload[0] = 0;
    BOOST_CHECK_THROW(admitted->validateInputs(observed, controls, context), std::invalid_argument);
    inputs[1].payload[0] = 1;
    auto malformed = observed;
    ++malformed[0].payloadBytes;
    BOOST_CHECK_THROW(admitted->validateInputs(malformed, controls, context), std::invalid_argument);
    const auto total = past + sequence;
    std::vector<AutomaticOnnxTensorObservation> output{
      {"scores", TensorElementType::Float32, {1, sequence, 11},
        static_cast<std::uint64_t>(sequence * 11 * 4)},
      {"new,key=0", TensorElementType::Float32, {1, 1, total, 8},
        static_cast<std::uint64_t>(total * 8 * 4)},
      {"new,value=0", TensorElementType::Float32, {1, 1, total, 8},
        static_cast<std::uint64_t>(total * 8 * 4)}};
    BOOST_CHECK_NO_THROW(admitted->validateOutputs(output, context));
    ++output.back().shape[2];
    output.back().payloadBytes += 8 * 4;
    BOOST_CHECK_THROW(admitted->validateOutputs(output, context), std::invalid_argument);
  }
  for (unsigned failure = 0; failure < 7; ++failure) {
    auto changed = projection;
    auto& generation = changed.generationContract;
    if (failure == 0) std::swap(generation.stateOutputNames[0], generation.stateOutputNames[1]);
    if (failure == 1) generation.tokenInputName = "guessed-input";
    if (failure == 2) generation.eosTokenIds = {11};
    if (failure == 3) generation.attentionMaskInputName = "guessed-mask";
    if (failure == 4) generation.positionIdsInputName = "guessed-position";
    if (failure == 5) generation.cachePositionInputName = "unproven-cache-position";
    if (failure == 6) generation.stateInputNames.pop_back();
    BOOST_TEST_CONTEXT("source generation mutation=" << failure) {
      BOOST_CHECK_THROW(admitAutomaticOnnxProjection(binding, changed, stateControl()), std::exception);
    }
  }
  const auto batchTwo = projectionFor(2);
  BOOST_REQUIRE_NO_THROW(validateNativeAutomaticSelectionBinding(batchTwo));
  BOOST_CHECK_THROW(admitAutomaticOnnxProjection(binding, batchTwo, stateControl()),
                    std::invalid_argument);
  // The source binder's B=2 proof above is intentionally not a public batched
  // sampling protocol. No backend GEN or distributed result is claimed here.
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
