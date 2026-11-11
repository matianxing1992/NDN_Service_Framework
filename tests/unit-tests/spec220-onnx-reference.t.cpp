#include "examples/spec220-onnx-reference-oracle.hpp"
#include "examples/onnx-kv-state-probe.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <onnx/onnx_pb.h>
#include <onnxruntime_cxx_api.h>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
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

  NativeJson
  checkpoint(unsigned round, std::uint64_t prefixCount) const
  {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    NativeJson value{{"schema", "ndnsf-di-conversation-checkpoint-v1"}, {"version", 1},
      {"conversationId", "independent-reference-conversation"}, {"contextEpoch", round + 1},
      {"parentContextEpoch", round}, {"prefixTokenCount", prefixCount},
      {"requesterIdentity", "/reference-user"}, {"serviceName", "/reference-service"},
      {"securityDomainDigest", "sha256:" + std::string(64, 'a')},
      {"modelContractDigest", "sha256:" + std::string(64, 'b')},
      {"planRoleMapDigest", "sha256:" + std::string(64, 'c')},
      {"logicalPrefixDigest", "sha256:" + std::string(64, 'd')},
      {"roleReceiptDigests", {{"/role", "sha256:" + std::string(64, 'e')}}},
      {"issuedAtMs", now}, {"expiresAtMs", now + 60000}};
    return signCheckpoint(std::move(value));
  }

  static NativeJson
  signCheckpoint(NativeJson value)
  {
    value.erase("checkpointDigest");
    value.erase("signature");
    const auto keys = nativeConversationAuthenticationKeys("reference-fixture",
      {{"active", std::vector<std::uint8_t>(32, 'k')}});
    return nativeParseJson(nativeSignConversationCheckpoint(std::move(value), keys.front()));
  }

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
    const auto keyPath = directory / "conversation.key";
    std::ofstream(keyPath, std::ios::binary) << std::string(32, 'k');
    std::filesystem::permissions(keyPath, std::filesystem::perms::owner_read |
      std::filesystem::perms::owner_write);
    requester["conversation"] = {
      {"journal", {{"identity", "reference-fixture"},
        {"keys", {{{"id", "active"}, {"file", keyPath.string()}}}}}},
      {"owner", {{"requester_identity", "/reference-user"},
        {"service_name", "/reference-service"},
        {"security_domain_digest", "sha256:" + std::string(64, 'a')}}}};
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
          checkpoint(round, round ? 7 : 4))}});
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

void
makeIndependentReplayGroups(ReferenceFixture& fixture)
{
  const auto original = fixture.requester.at("turns");
  fixture.requester["conversation"]["turn"] = {{"mode", "FULL_CONTEXT"}};
  fixture.requester["turns"] = NativeJson::array();
  const std::vector<std::int64_t> replay{0, 2, 1, 1, 3};
  std::vector<std::uint8_t> payload(replay.size() * sizeof(std::int64_t));
  std::memcpy(payload.data(), replay.data(), payload.size());
  const auto wire = encodeTensorBundle({{"input_ids", TensorElementType::Int64,
    {1, static_cast<std::int64_t>(replay.size())}, payload}});
  const auto input = fixture.directory / "replay-input.bin";
  std::ofstream(input, std::ios::binary).write(
    reinterpret_cast<const char*>(wire.data()), wire.size());
  for (unsigned round = 0; round < 4; ++round) {
    auto turn = original.at(round % 2);
    const std::string id = round < 2 ? "reference-append-0001" : "reference-replay-0001";
    turn["conversation_id"] = id;
    if (round == 2) turn["input_file"] = input.string();
    const auto prefix = round == 0 ? 4U : round == 3 ? 10U : 7U;
    auto checkpoint = fixture.checkpoint(round % 2, prefix);
    checkpoint["conversationId"] = id;
    turn["checkpoint_output_file"] = fixture.write(
      "group-checkpoint-" + std::to_string(round) + ".json",
      ReferenceFixture::signCheckpoint(std::move(checkpoint)));
    turn["output_file"] = fixture.write("group-output-" + std::to_string(round) + ".json",
      {{"tokenIds", {1, 1}}});
    fixture.requester["turns"].push_back(std::move(turn));
  }
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
  BOOST_CHECK_EQUAL(result.at("checkpointAuthentication").get<std::string>(), "VERIFIED");
  const auto& warmRound = result.at("rounds").at(1);
  BOOST_CHECK_EQUAL(warmRound.at("initialStateTokens").get<unsigned>(), 4U);
  BOOST_CHECK_EQUAL(warmRound.at("inputTokens").get<unsigned>(), 1U);
  for (const auto* phase : {"prefill", "decode", "finalize"}) {
    BOOST_CHECK_EQUAL(warmRound.at("onnxPhaseCalls").at(phase).get<unsigned>(), 1U);
    BOOST_CHECK_GE(warmRound.at("onnxPhasesSeconds").at(phase).get<double>(), 0.0);
  }
}

BOOST_AUTO_TEST_CASE(IndependentConversationReplayStartsWithEmptyState)
{
  ReferenceFixture fixture;
  makeIndependentReplayGroups(fixture);
  const auto result = spec220::oracle::validateOnnxReference(
    fixture.requester, Ort::GetVersionString());
  const auto& rounds = result.at("rounds");
  BOOST_REQUIRE_EQUAL(rounds.size(), 4U);
  BOOST_CHECK_EQUAL(rounds.at(1).at("initialStateTokens").get<unsigned>(), 4U);
  BOOST_CHECK_EQUAL(rounds.at(2).at("initialStateTokens").get<unsigned>(), 0U);
  BOOST_CHECK_EQUAL(rounds.at(1).at("startingContextTokens").get<unsigned>(), 5U);
  BOOST_CHECK_EQUAL(rounds.at(2).at("startingContextTokens").get<unsigned>(), 5U);
  BOOST_CHECK(rounds.at(1).at("tokenIds") == rounds.at(2).at("tokenIds"));
  BOOST_CHECK(rounds.at(1).at("referenceStateDigests") ==
              rounds.at(2).at("referenceStateDigests"));
  BOOST_CHECK_EQUAL(rounds.at(3).at("prefixTokens").get<unsigned>(), 10U);
  BOOST_CHECK_EQUAL(rounds.at(2).at("conversationTurn").get<unsigned>(), 0U);
}

BOOST_AUTO_TEST_CASE(IndependentGroupsRejectMixedRepeatedAndIncompleteDeclarations)
{
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    ReferenceFixture fixture;
    makeIndependentReplayGroups(fixture);
    auto& turns = fixture.requester["turns"];
    if (scenario == 0) turns.at(1).erase("conversation_id");
    else if (scenario == 1) {
      turns.at(2)["conversation_id"] = turns.at(0).at("conversation_id");
      turns.at(3)["conversation_id"] = turns.at(0).at("conversation_id");
    }
    else turns.erase(turns.end() - 1);
    BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(
      fixture.requester, Ort::GetVersionString()), std::exception,
      [] (const auto& error) { return reason(error, "conversation-groups"); });
  }
}

BOOST_AUTO_TEST_CASE(IndependentGroupsRejectAuthenticatedWrongIdentityAndEpoch)
{
  for (unsigned scenario = 0; scenario < 2; ++scenario) {
    ReferenceFixture fixture;
    makeIndependentReplayGroups(fixture);
    auto checkpoint = fixture.checkpoint(scenario == 0 ? 0 : 2, 7);
    checkpoint["conversationId"] = scenario == 0 ?
      "reference-wrong-group-0001" : "reference-replay-0001";
    fixture.requester["turns"].at(2)["checkpoint_output_file"] = fixture.write(
      "wrong-group-checkpoint.json", ReferenceFixture::signCheckpoint(std::move(checkpoint)));
    BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(
      fixture.requester, Ort::GetVersionString()), std::exception,
      [] (const auto& error) { return reason(error, "checkpoint-state-progression"); });
  }
}

BOOST_AUTO_TEST_CASE(IndependentReferenceAcceptsOneColdRequest)
{
  ReferenceFixture fixture;
  fixture.requester["turns"].erase(fixture.requester["turns"].begin() + 1);
  const auto result = spec220::oracle::validateOnnxReference(
    fixture.requester, Ort::GetVersionString());
  BOOST_REQUIRE_EQUAL(result.at("rounds").size(), 1U);
  BOOST_CHECK_EQUAL(result.at("rounds").at(0).at("modelCalls").get<unsigned>(), 3U);
  BOOST_CHECK_EQUAL(result.at("rounds").at(0).at("prefixTokens").get<unsigned>(), 4U);
  BOOST_CHECK_EQUAL(result.at("checkpointAuthentication").get<std::string>(), "VERIFIED");
}

BOOST_AUTO_TEST_CASE(ActualResidentStateProbeCapturesPrefillDecodeAndFinalize)
{
  for (const auto stateType : {onnx::TensorProto::INT64, onnx::TensorProto::FLOAT,
                               onnx::TensorProto::FLOAT16}) {
    ReferenceFixture fixture;
    if (stateType != onnx::TensorProto::INT64) {
      auto* graph = fixture.model.mutable_graph();
      graph->mutable_input(3)->mutable_type()->mutable_tensor_type()->set_elem_type(stateType);
      graph->mutable_output(0)->mutable_type()->mutable_tensor_type()->set_elem_type(stateType);
      graph->mutable_node(0)->set_input(1, "state-input-tokens");
      auto* cast = graph->add_node();
      cast->set_name("state-token-cast");
      cast->set_op_type("Cast");
      cast->add_input("input_ids");
      cast->add_output("state-input-tokens");
      auto* type = cast->add_attribute();
      type->set_name("to"); type->set_type(onnx::AttributeProto::INT); type->set_i(stateType);
      graph->mutable_node()->SwapElements(0, graph->node_size() - 1);
      fixture.saveModel();
    }
    for (const auto initialSize : {2U, 20U}) {
      const auto destination = fixture.directory /
        ("private-kv-" + std::to_string(initialSize) + ".json");
      auto probe = std::make_shared<OnnxKvStateProbe>(destination, "/reference-provider");
      RegistryNativeModelRunnerFactory factory;
      registerOnnxRuntimeBackend(factory, nullptr, probe);
      NativeModelRunnerSpec spec;
      spec.role = "/role";
      spec.kind = "onnx";
      spec.backend = "onnxruntime";
      spec.path = (fixture.directory / "model.onnx").string();
      spec.metadata = {{"inputNames", "input_ids,attention_mask,position_ids,past"},
        {"providerIdentity", "/reference-provider"}, {"providerBootId", "probe-boot"},
        {"outputNames", "present,logits"}, {"stateInputNames", "past"},
        {"stateOutputNames", "present"}, {"stateSuccessorMap", "past=present"},
        {"positionInputPolicy", "qwen-causal-position-v1"}, {"tokenInputName", "input_ids"},
        {"attentionMaskInputName", "attention_mask"}, {"positionIdsInputName", "position_ids"}};
      auto runner = factory.create(spec);
      BOOST_CHECK(!std::filesystem::exists(destination));
      OnnxRuntimeModelRunner baseline(spec);
      std::vector<std::int64_t> prefix;
      for (std::uint64_t epoch = 0; epoch < 3; ++epoch) {
        std::vector<std::int64_t> ids(epoch ? 1 : initialSize, 1);
        if (!epoch)
          std::iota(ids.begin(), ids.end(), 0);
        prefix.insert(prefix.end(), ids.begin(), ids.end());
        RoleExecutionContext context;
        context.requestId = "probe-request";
        context.sessionId = "probe-session";
        context.providerBootId = "probe-boot";
        context.role = "/role";
        context.attemptEpoch = 1;
        context.inferenceEpoch = epoch;
        context.streamingStateExecution = true;
        context.generationInputTokenCount = ids.size();
        GenerationEpochLineageV1 lineage;
        lineage.requestId = context.requestId;
        lineage.attemptEpoch = 1;
        lineage.planDigest = "sha256:" + std::string(64, 'b');
        lineage.generationId = std::string(32, '1');
        lineage.streamEpoch = epoch + 1;
        lineage.inferenceEpoch = epoch;
        lineage.transitionKind = epoch == 0 ? GenerationEpochLineageV1::PREFILL :
          epoch == 1 ? GenerationEpochLineageV1::DECODE :
                       GenerationEpochLineageV1::CHECKPOINT_FINALIZE;
        lineage.logicalPrefixDigest = nativeConversationPrefixDigest(prefix);
        lineage.logicalPrefixTokenCount = prefix.size();
        lineage.positionDigest = "sha256:" + std::string(64, 'c');
        context.generationLineage = lineage;
        std::vector<std::uint8_t> data(ids.size() * sizeof(std::int64_t));
        std::memcpy(data.data(), ids.data(), data.size());
        TensorBundle input;
        input.name = "input_ids";
        input.payload = encodeTensorBundle({{"input_ids", TensorElementType::Int64,
          {1, static_cast<std::int64_t>(ids.size())}, std::move(data)}});
        context.inputsByScope.emplace("input_ids", std::move(input));
        const auto observed = runner->run(context);
        const auto expected = baseline.run(context);
        BOOST_REQUIRE_EQUAL(observed.size(), expected.size());
        for (const auto& output : observed) {
          BOOST_REQUIRE(expected.count(output.first));
          BOOST_CHECK(output.second.payload == expected.at(output.first).payload);
        }
        std::ifstream file(destination);
        NativeJson samples;
        file >> samples;
        BOOST_CHECK_EQUAL(samples.at("records").size(), epoch + 1);
        const auto& record = samples.at("records").back();
        BOOST_CHECK_EQUAL(record.at("inferenceEpoch").get<std::uint64_t>(), epoch);
        BOOST_CHECK_EQUAL(record.at("logicalPrefixTokenCount").get<std::uint32_t>(), prefix.size());
        const auto& tensor = record.at("tensors").at(0);
        BOOST_CHECK_EQUAL(tensor.at("inputName").get<std::string>(), "past");
        const std::string expectedType = stateType == onnx::TensorProto::FLOAT ? "float32" :
          stateType == onnx::TensorProto::FLOAT16 ? "float16" : "int64";
        BOOST_CHECK_EQUAL(tensor.at("elementType").get<std::string>(), expectedType);
        BOOST_CHECK(tensor.at("shape") == NativeJson({1, prefix.size()}));
        const auto points = std::min<std::size_t>(16, prefix.size());
        std::vector<std::uint64_t> indices;
        std::vector<std::int64_t> values;
        for (std::size_t i = 0; i < points; ++i) {
          const auto index = points == 1 ? 0 : i * (prefix.size() - 1) / (points - 1);
          indices.push_back(index);
          values.push_back(prefix.at(index));
        }
        BOOST_CHECK(tensor.at("values") == NativeJson(values));
        BOOST_CHECK(tensor.at("indices") == NativeJson(indices));
      }
      const auto mode = std::filesystem::status(destination).permissions();
      BOOST_CHECK((mode & std::filesystem::perms::group_all) == std::filesystem::perms::none);
      BOOST_CHECK((mode & std::filesystem::perms::others_all) == std::filesystem::perms::none);
      BOOST_CHECK_THROW(OnnxKvStateProbe(destination, "/reference-provider"), std::exception);
      runner->releaseSessionState("probe-session");
      baseline.releaseSessionState("probe-session");
    }
  }
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
    fixture.checkpoint(1, 8));
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(changed, Ort::GetVersionString()),
    std::exception, [] (const auto& e) { return reason(e, "checkpoint-state-progression"); });
  changed = fixture.requester;
  changed["catalog"]["source"]["digest"] = "sha256:" + std::string(64, '0');
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(changed, Ort::GetVersionString()),
    std::exception, [] (const auto& e) { return reason(e, "source-digest"); });
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(fixture.requester, "other-runtime"),
    std::exception, [] (const auto& e) { return reason(e, "runtime-version"); });
}

BOOST_AUTO_TEST_CASE(RejectsUnauthenticatedExpiredAndForeignOwnerCheckpoints)
{
  ReferenceFixture fixture;
  for (const auto& kind : {"signature", "expiry", "key-identity", "owner"}) {
    auto changed = fixture.requester;
    auto checkpoint = fixture.checkpoint(0, 4);
    std::string boundary = "checkpoint-authentication";
    if (std::string(kind) == "signature")
      checkpoint["signature"] = std::string(64, '0');
    else if (std::string(kind) == "expiry") {
      checkpoint["issuedAtMs"] = 1;
      checkpoint["expiresAtMs"] = 2;
      checkpoint = fixture.signCheckpoint(std::move(checkpoint));
    }
    else if (std::string(kind) == "key-identity")
      changed["conversation"]["journal"]["identity"] = "wrong-reference-fixture";
    else {
      changed["conversation"]["owner"]["requester_identity"] = "/other-user";
      boundary = "checkpoint-owner-binding";
    }
    changed["turns"][0]["checkpoint_output_file"] =
      fixture.write(std::string(kind) + ".json", checkpoint);
    BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(changed, Ort::GetVersionString()),
      std::exception, [&] (const auto& e) { return reason(e, boundary); });
  }
  std::ofstream(fixture.directory / "conversation.key", std::ios::binary) << std::string(32, 'x');
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(
    fixture.requester, Ort::GetVersionString()), std::exception,
    [] (const auto& e) { return reason(e, "checkpoint-authentication"); });
}

BOOST_AUTO_TEST_CASE(RejectsNonprivateAndSymlinkCheckpointKeys)
{
  ReferenceFixture fixture;
  auto changed = fixture.requester;
  const auto link = fixture.directory / "key-link";
  std::filesystem::create_symlink(fixture.directory / "conversation.key", link);
  changed["conversation"]["journal"]["keys"][0]["file"] = link.string();
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(changed, Ort::GetVersionString()),
    std::exception, [] (const auto& e) { return reason(e, "checkpoint-private-key"); });
  std::filesystem::permissions(fixture.directory / "conversation.key",
    std::filesystem::perms::group_read, std::filesystem::perm_options::add);
  BOOST_CHECK_EXCEPTION(spec220::oracle::validateOnnxReference(
    fixture.requester, Ort::GetVersionString()), std::exception,
    [] (const auto& e) { return reason(e, "checkpoint-private-key"); });
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
