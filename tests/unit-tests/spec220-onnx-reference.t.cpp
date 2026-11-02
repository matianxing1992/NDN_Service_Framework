#include "examples/spec220-onnx-reference-oracle.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <onnx/onnx_pb.h>
#include <onnxruntime_cxx_api.h>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace ndnsf::di::tests {
namespace {

struct ReferenceFixture
{
  std::filesystem::path directory = std::filesystem::temp_directory_path() /
    ("ndnsf-unsplit-reference-" + std::to_string(getpid()) + "-" +
     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  NativeJson requester;
  onnx::ModelProto model;

  std::string
  write(const std::string& name, const NativeJson& value)
  {
    const auto path = directory / name;
    std::ofstream(path) << value.dump();
    return path.string();
  }

  void
  saveModel()
  {
    const auto bytes = model.SerializeAsString();
    const auto path = directory / "model.onnx";
    std::ofstream(path, std::ios::binary).write(bytes.data(), bytes.size());
    requester["catalog"]["source"] = {{"file", path.string()}, {"bytes", bytes.size()},
      {"digest", nativePlanningDigest(bytes)}};
  }

  ReferenceFixture()
  {
    std::filesystem::create_directory(directory);
    model.set_ir_version(8);
    model.add_opset_import()->set_version(18);
    auto* graph = model.mutable_graph();
    graph->set_name("fixed-logits-with-real-prefix-state");
    const auto addValue = [] (onnx::ValueInfoProto* value, const std::string& name,
                             int type, const std::vector<std::int64_t>& shape,
                             const std::string& symbol = "") {
      value->set_name(name);
      auto* tensor = value->mutable_type()->mutable_tensor_type();
      tensor->set_elem_type(type);
      for (auto size : shape) {
        auto* dimension = tensor->mutable_shape()->add_dim();
        if (size >= 0) dimension->set_dim_value(size);
        else dimension->set_dim_param(symbol);
      }
    };
    addValue(graph->add_input(), "input_ids", onnx::TensorProto::INT64, {1, -1}, "sequence");
    addValue(graph->add_input(), "attention_mask", onnx::TensorProto::INT64,
             {1, -1}, "total_sequence_length");
    addValue(graph->add_input(), "position_ids", onnx::TensorProto::INT64, {1, -1}, "sequence");
    addValue(graph->add_input(), "past", onnx::TensorProto::INT64, {1, -1}, "past_sequence");
    addValue(graph->add_output(), "present", onnx::TensorProto::INT64,
             {1, -1}, "total_sequence_length");
    addValue(graph->add_output(), "logits", onnx::TensorProto::FLOAT, {1, 1, 4});
    auto* concat = graph->add_node();
    concat->set_name("append-state");
    concat->set_op_type("Concat");
    concat->add_input("past");
    concat->add_input("input_ids");
    concat->add_output("present");
    auto* axis = concat->add_attribute();
    axis->set_name("axis"); axis->set_type(onnx::AttributeProto::INT); axis->set_i(1);
    auto* logits = graph->add_node();
    logits->set_name("known-logits"); logits->set_op_type("Constant"); logits->add_output("logits");
    auto* attribute = logits->add_attribute();
    attribute->set_name("value"); attribute->set_type(onnx::AttributeProto::TENSOR);
    auto* tensor = attribute->mutable_t();
    tensor->set_data_type(onnx::TensorProto::FLOAT);
    for (auto size : {1, 1, 4}) tensor->add_dims(size);
    for (float value : {0, 2, 0, 0}) tensor->add_float_data(value);
    requester = {{"limits", {{"max_source_bytes", 1024 * 1024}}},
      {"catalog", {{"splitter", {{"resources_by_role", {{"role", {{"kv_bytes", 4096}}}}}}}}},
      {"turns", NativeJson::array()}};
    saveModel();
    for (unsigned round = 0; round < 2; ++round) {
      const std::vector<std::int64_t> ids = round ? std::vector<std::int64_t>{3} :
                                                  std::vector<std::int64_t>{0, 2};
      std::vector<std::uint8_t> payload(ids.size() * 8);
      std::memcpy(payload.data(), ids.data(), payload.size());
      const auto bytes = encodeTensorBundle({{"input_ids", TensorElementType::Int64,
        {1, static_cast<std::int64_t>(ids.size())}, payload}});
      const auto input = directory / ("input-" + std::to_string(round));
      std::ofstream(input, std::ios::binary).write(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
      const NativeJson options{{"sampling", {{"mode", "Greedy"}, {"repetitionPenalty", 1}}},
        {"maxNewTokens", 2}, {"tokenInputName", "input_ids"},
        {"attentionMaskInputName", "attention_mask"}, {"positionIdsInputName", "position_ids"},
        {"positionInputPolicy", "qwen-causal-position-v1"}, {"stateSuccessorMap", "past=present"},
        {"stopStrings", NativeJson::array()}, {"eosTokenIds", {3}}};
      const auto suffix = std::to_string(round) + ".json";
      requester["turns"].push_back({{"input_file", input.string()},
        {"options_file", write("options-" + suffix, options)},
        {"output_file", write("output-" + suffix, {{"tokenIds", {1, 1}}})},
        {"checkpoint_output_file", write("checkpoint-" + suffix,
          {{"conversationId", "independent-reference-conversation"}, {"contextEpoch", round + 1},
           {"prefixTokenCount", round ? 7 : 4}})}});
    }
  }

  ~ReferenceFixture()
  {
    std::filesystem::remove_all(directory);
  }
};

bool
reason(const std::exception& error, const std::string& value)
{
  return std::string(error.what()).find("reason=" + value) != std::string::npos;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec220OnnxReference)

BOOST_AUTO_TEST_CASE(IndependentTokensAndColdWarmStateProgression)
{
  ReferenceFixture fixture;
  const auto result = spec220::oracle::validateOnnxReference(
    fixture.requester, Ort::GetVersionString());
  BOOST_REQUIRE_EQUAL(result.at("rounds").size(), 2U);
  BOOST_CHECK(result.at("rounds").at(0).at("tokenIds") == NativeJson({1, 1}));
  BOOST_CHECK_EQUAL(result.at("rounds").at(0).at("prefixTokens").get<unsigned>(), 4U);
  BOOST_CHECK_EQUAL(result.at("rounds").at(1).at("prefixTokens").get<unsigned>(), 7U);
  BOOST_CHECK_EQUAL(result.at("rounds").at(0).at("referenceStateBytes").get<unsigned>(), 32U);
  BOOST_CHECK_EQUAL(result.at("rounds").at(1).at("referenceStateBytes").get<unsigned>(), 56U);
  BOOST_CHECK_EQUAL(result.at("rounds").at(1).at("modelCalls").get<unsigned>(), 3U);
  const std::vector<std::int64_t> cold{0, 2, 1, 1}, warm{0, 2, 1, 1, 3, 1, 1};
  BOOST_CHECK_EQUAL(result.at("rounds").at(0).at("referenceStateDigests").at("past")
    .get<std::string>(), nativePlanningDigest(reinterpret_cast<const std::uint8_t*>(cold.data()),
                                            cold.size() * sizeof(std::int64_t)));
  BOOST_CHECK_EQUAL(result.at("rounds").at(1).at("referenceStateDigests").at("past")
    .get<std::string>(), nativePlanningDigest(reinterpret_cast<const std::uint8_t*>(warm.data()),
                                            warm.size() * sizeof(std::int64_t)));
  BOOST_CHECK(!result.at("providerStateValuesCompared").get<bool>());
}

BOOST_AUTO_TEST_CASE(RejectsTamperedTokenCheckpointSourceAndRuntime)
{
  ReferenceFixture fixture;
  auto changed = fixture.requester;
  changed["turns"][1]["output_file"] = fixture.write("wrong-output.json", {{"tokenIds", {1, 2}}});
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(changed, Ort::GetVersionString()),
    std::exception, [] (const auto& e) { return reason(e, "token-mismatch"); });
  changed = fixture.requester;
  changed["turns"][1]["checkpoint_output_file"] = fixture.write("wrong-checkpoint.json",
    {{"conversationId", "independent-reference-conversation"}, {"contextEpoch", 2},
     {"prefixTokenCount", 8}});
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(changed, Ort::GetVersionString()),
    std::exception, [] (const auto& e) { return reason(e, "checkpoint-state-progression"); });
  changed = fixture.requester;
  changed["catalog"]["source"]["digest"] = "sha256:" + std::string(64, '0');
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(changed, Ort::GetVersionString()),
    std::exception, [] (const auto& e) { return reason(e, "source-digest"); });
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(fixture.requester, "other-runtime"),
    std::exception, [] (const auto& e) { return reason(e, "runtime-version"); });
}

BOOST_AUTO_TEST_CASE(RejectsStateThatDidNotAdvanceAndInsufficientBudget)
{
  ReferenceFixture fixture;
  auto* state = fixture.model.mutable_graph()->mutable_node(0);
  state->set_op_type("Identity"); state->clear_attribute(); state->clear_input();
  state->add_input("past");
  fixture.saveModel();
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(
    fixture.requester, Ort::GetVersionString()),
    std::exception, [] (const auto& e) { return reason(e, "state-extents"); });
  fixture.requester["catalog"]["splitter"]["resources_by_role"]["role"]["kv_bytes"] = 1;
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(
    fixture.requester, Ort::GetVersionString()),
    std::exception, [] (const auto& e) { return reason(e, "state-budget"); });
}

BOOST_AUTO_TEST_CASE(RejectsUnknownStateGeometryAndUnsupportedSampling)
{
  ReferenceFixture fixture;
  auto options = NativeJson{{"sampling", {{"mode", "Random"}}}};
  auto changed = fixture.requester;
  changed["turns"][0]["options_file"] = fixture.write("random-options.json", options);
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(changed, Ort::GetVersionString()),
    std::exception, [] (const auto& e) { return reason(e, "sampling-policy"); });
  fixture.model.mutable_graph()->mutable_input(3)->mutable_type()->mutable_tensor_type()
    ->mutable_shape()->mutable_dim(1)->set_dim_param("unknown_state_axis");
  fixture.saveModel();
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(
    fixture.requester, Ort::GetVersionString()),
    std::exception, [] (const auto& e) { return reason(e, "unknown-state-dimension"); });
}

BOOST_AUTO_TEST_CASE(KernelFailureDoesNotBecomeReferenceSuccess)
{
  ReferenceFixture fixture;
  auto* graph = fixture.model.mutable_graph();
  auto* table = graph->add_initializer();
  table->set_name("table"); table->set_data_type(onnx::TensorProto::FLOAT);
  table->add_dims(1); table->add_float_data(0);
  auto* gather = graph->add_node();
  gather->set_name("bad-index"); gather->set_op_type("Gather");
  gather->add_input("table"); gather->add_input("input_ids"); gather->add_output("fault");
  auto* value = graph->add_output();
  value->set_name("fault");
  auto* type = value->mutable_type()->mutable_tensor_type();
  type->set_elem_type(onnx::TensorProto::FLOAT);
  type->mutable_shape()->add_dim()->set_dim_value(1);
  type->mutable_shape()->add_dim()->set_dim_param("sequence");
  fixture.saveModel();
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    BOOST_CHECK_THROW(spec220::oracle::validateOnnxReference(
      fixture.requester, Ort::GetVersionString()), Ort::Exception);
  }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
