#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-io.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeSessionCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <boost/test/unit_test.hpp>
#include <onnx/onnx_pb.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace ndnsf::di::test {
namespace {

const std::string TOKEN = " token,=雪 ";
const std::string PAST = "\tcache,=入力 ";
const std::string VALUE = " value,=雪\t";
const std::string PRESENT = " next,=出力 ";

NativeSelectionProjectionV3
ioProjection(bool stateful = true)
{
  NativeSelectionProjectionV3 result;
  NativeAutomaticShapeSeed seed;
  seed.contractDigest = nativePlanningDigest("io-source-contract");
  seed.bindingDigest = nativePlanningDigest("io-binding");
  seed.budgetDigest = nativePlanningDigest("io-budget");
  seed.envelopeDigest = nativePlanningDigest("io-envelope");
  seed.maxContext = 16;
  seed.maxNewTokens = stateful ? 3 : 0;
  seed.inputShapes.emplace("graph/" + TOKEN, std::vector<std::int64_t>{1});
  result.automaticShapeSeed = std::move(seed);
  auto& role = result.assembly;
  role.role = role.selectedRole = "/role";
  role.backend = "onnxruntime-cpu";
  role.automaticContractDigest = result.automaticShapeSeed->contractDigest;
  role.expectedInputs = {{TOKEN, "float32", {std::int64_t(1)}}};
  role.expectedOutputs = {{VALUE, "float32", {std::int64_t(1)}}};
  if (stateful) {
    role.expectedInputs.push_back({PAST, "float32", {std::int64_t(1)}});
    role.expectedOutputs.push_back({PRESENT, "float32", {std::int64_t(1)}});
    result.generationContract.enabled = true;
    result.generationContract.maxGeneratedTokens = 3;
    result.generationContract.stateInputNames = {PAST};
    result.generationContract.stateOutputNames = {PRESENT};
  }
  result.selectedRole = role;
  result.dataflow.terminalResponseOwner = true;
  return result;
}

struct IoModel
{
  explicit IoModel(const std::string& purpose, bool stateful = true, bool legacy = false,
                   bool passthrough = false, const std::vector<std::int64_t>& dimensions = {1})
    : directory(std::filesystem::temp_directory_path() /
        ("ndnsf-spec233-io-" + std::to_string(::getpid()) + "-" + purpose))
    , token(legacy ? "input" : TOKEN)
    , past(legacy ? "past" : PAST)
    , value(legacy ? "score" : VALUE)
    , present(legacy ? "present" : PRESENT)
  {
    if (!std::filesystem::create_directory(directory))
      throw std::runtime_error("IO fixture directory is already owned");
    try {
      onnx::ModelProto model;
      model.set_ir_version(8);
      model.add_opset_import()->set_version(13);
      auto& graph = *model.mutable_graph();
      graph.set_name("independent-io-state-oracle");
      const auto port = [&dimensions](onnx::ValueInfoProto* info, const std::string& name) {
        info->set_name(name);
        auto& type = *info->mutable_type()->mutable_tensor_type();
        type.set_elem_type(onnx::TensorProto::FLOAT);
        auto& shape = *type.mutable_shape();
        for (const auto dimension : dimensions)
          shape.add_dim()->set_dim_value(dimension);
      };
      port(graph.add_input(), token);
      if (stateful || passthrough)
        port(graph.add_input(), past);
      port(graph.add_output(), value);
      auto& add = *graph.add_node();
      add.set_op_type(stateful ? "Add" : "Identity");
      add.add_input(token);
      if (stateful)
        add.add_input(past);
      add.add_output(value);
      if (stateful) {
        port(graph.add_output(), present);
        auto& copy = *graph.add_node();
        copy.set_op_type("Identity");
        copy.add_input(value);
        copy.add_output(present);
      }
      std::ofstream file(path(), std::ios::binary);
      if (!file || !model.SerializeToOstream(&file))
        throw std::runtime_error("cannot write independent IO fixture");
      file.close();
      if (!file)
        throw std::runtime_error("cannot close independent IO fixture");
    }
    catch (...) {
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
      throw;
    }
  }

  ~IoModel()
  {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }

  std::string path() const { return (directory / "model.onnx").string(); }

  std::filesystem::path directory;
  std::string token;
  std::string past;
  std::string value;
  std::string present;
};

NativeModelRunnerSpec
ioSpec(const IoModel& model, bool stateful = true)
{
  NativeModelRunnerSpec result;
  result.role = "/role";
  result.kind = "onnx";
  result.backend = "onnxruntime-cpu";
  result.path = model.path();
  result.metadata = {{"providerIdentity", "/spec233/io-provider"},
    {"providerBootId", "io-boot"}, {"backendAbi", "io-fixture-cpu"}};
  for (const auto* field : {"artifactDigest", "graphDigest", "canonicalInitializerDigest",
                            "recipeDigest"})
    result.metadata[field] = nativePlanningDigest(std::string("io-fixture-") + field);
  bindAutomaticOnnxRunnerIo(result, ioProjection(stateful));
  return result;
}

NamedTensor
scalar(const std::string& name, float value)
{
  std::vector<std::uint8_t> bytes(sizeof(value));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return {name, TensorElementType::Float32, {1}, std::move(bytes)};
}

RoleExecutionContext
ioRound(const IoModel& model, const std::string& session, std::uint64_t epoch,
        float token, bool streaming = true, bool providePast = false, float past = 0)
{
  RoleExecutionContext result;
  result.sessionId = session;
  result.requestId = session;
  result.role = "/role";
  result.providerBootId = "io-boot";
  result.attemptEpoch = 1;
  result.inferenceEpoch = epoch;
  result.streamingStateExecution = streaming;
  std::vector<NamedTensor> inputs{scalar(model.token, token)};
  if (providePast)
    inputs.push_back(scalar(model.past, past));
  result.inputsByScope.emplace("input", makeEncodedTensorBundle("input", inputs));
  return result;
}

float
observedScalar(const std::map<std::string, TensorBundle>& outputs, const std::string& name)
{
  for (const auto& item : outputs) {
    if (!isEncodedTensorBundle(item.second.payload))
      continue;
    for (const auto& tensor : decodeTensorBundle(item.second.payload)) {
      if (tensor.name != name)
        continue;
      if (tensor.elementType != TensorElementType::Float32 ||
          tensor.shape != std::vector<std::int64_t>{1} || tensor.payload.size() != sizeof(float))
        throw std::runtime_error("independent IO result type/shape differs");
      float result;
      std::memcpy(&result, tensor.payload.data(), sizeof(result));
      return result;
    }
  }
  throw std::runtime_error("exact original IO name was not returned");
}

std::string
rejection(const std::function<void()>& action)
{
  try {
    action();
  }
  catch (const std::exception& error) {
    return error.what();
  }
  BOOST_FAIL("expected IO admission failure");
  return {};
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec233AutomaticIo)

BOOST_AUTO_TEST_CASE(CanonicalRawNamesMatchIndependentLiteral)
{
  const auto value = automaticOnnxIoContractForProjection(ioProjection());
  const std::string expected =
    R"({"inputNames":[" token,=\u96ea ","\tcache,=\u5165\u529b "],)"
    R"("outputNames":[" value,=\u96ea\t"," next,=\u51fa\u529b "],"passthroughTensors":[],)"
    R"("schema":"ndnsf-di-automatic-onnx-io-v1","stateInputNames":["\tcache,=\u5165\u529b "],)"
    R"("stateOutputNames":[" next,=\u51fa\u529b "],"unrankedInputNames":[]})";
  BOOST_CHECK_EQUAL(value.canonicalJson(), expected);
  const auto restored = AutomaticOnnxIoContract::fromCanonicalJson(expected);
  BOOST_CHECK_EQUAL(restored.io.inputNames.front(), TOKEN);
  BOOST_CHECK_EQUAL(restored.io.stateInputForOutput(PRESENT), PAST);
  BOOST_CHECK(restored.io.stateFamilies.empty());
}

BOOST_AUTO_TEST_CASE(StrictCodecRejectsMalformedAndMixedMetadata)
{
  const auto wire = automaticOnnxIoContractForProjection(ioProjection()).canonicalJson();
  const auto original = nativeParseJson(wire);
  for (unsigned failure = 0; failure != 11; ++failure) {
    auto value = original;
    if (failure == 0) value["schema"] = "legacy";
    if (failure == 1) value["unknown"] = true;
    if (failure == 2) value.erase("stateInputNames");
    if (failure == 3) value["inputNames"][0] = 4;
    if (failure == 4) value["stateOutputNames"] = NativeJson::array();
    if (failure == 5) value["stateInputNames"][0] = "foreign";
    if (failure == 6) value["inputNames"].push_back(TOKEN);
    if (failure == 7) value["passthroughTensors"] = NativeJson::array({PAST});
    if (failure == 8) value["unrankedInputNames"] = NativeJson::array({"foreign"});
    if (failure == 9) value["inputNames"][0] = std::string(1025, 'x');
    if (failure == 10) value["inputNames"][0] = std::string("x\0y", 3);
    BOOST_TEST_CONTEXT("mutation=" << failure) {
      BOOST_CHECK_THROW(AutomaticOnnxIoContract::fromCanonicalJson(nativeCanonicalJson(value)),
                        std::exception);
    }
  }
  BOOST_CHECK_THROW(AutomaticOnnxIoContract::fromCanonicalJson(original.dump(2)), std::exception);
  BOOST_CHECK_THROW(AutomaticOnnxIoContract::fromCanonicalJson(
    wire.substr(0, wire.size() - 1) + ",\"schema\":\"other\"}"), std::exception);
  auto boundary = original;
  boundary["inputNames"][0] = std::string(1024, 'x');
  BOOST_CHECK_NO_THROW(AutomaticOnnxIoContract::fromCanonicalJson(nativeCanonicalJson(boundary)));
  boundary = original;
  for (unsigned index = 0; index < 254; ++index)
    boundary["inputNames"].push_back("port-" + std::to_string(index));
  BOOST_CHECK_NO_THROW(AutomaticOnnxIoContract::fromCanonicalJson(nativeCanonicalJson(boundary)));
  boundary["inputNames"].push_back("overflow");
  BOOST_CHECK_THROW(AutomaticOnnxIoContract::fromCanonicalJson(nativeCanonicalJson(boundary)),
                    std::exception);
  BOOST_CHECK_EQUAL(rejection([&] {
    AutomaticOnnxIoContract::fromCanonicalJson(
      std::string(NATIVE_AUTOMATIC_ONNX_IO_MAX_BYTES + 1, ' '));
  }), "AUTOMATIC_IO_LIMIT");
  BOOST_CHECK_THROW(AutomaticOnnxIoContract::fromCanonicalJson(
    std::string(128, '[') + "0" + std::string(128, ']')), std::exception);
  NativeModelRunnerSpec spec;
  spec.metadata["automaticIoContract"] = wire;
  for (const auto* key : {"inputNames", "state_successor_map", "kvOutputTensors",
                          "positionInputPolicy", "stateful"}) {
    spec.metadata[key] = "legacy";
    BOOST_CHECK_THROW(readAutomaticOnnxRunnerIo(spec), std::exception);
    spec.metadata.erase(key);
  }
  spec.metadata["statefulModel"] = "false";
  BOOST_CHECK_THROW(readAutomaticOnnxRunnerIo(spec), std::exception);
  BOOST_CHECK_THROW(bindAutomaticOnnxRunnerIo(spec, NativeSelectionProjectionV3{}), std::exception);
}

BOOST_AUTO_TEST_CASE(GlobalPairsAreSelectedTogetherBeforePreparation)
{
  auto projection = ioProjection();
  projection.generationContract.stateInputNames.push_back("remote-input");
  projection.generationContract.stateOutputNames.push_back("remote-output");
  const auto local = automaticOnnxIoContractForProjection(projection);
  BOOST_CHECK(local.io.stateInputNames == std::vector<std::string>{PAST});
  BOOST_CHECK(local.io.stateOutputNames == std::vector<std::string>{PRESENT});
  NativeModelRunnerSpec spec;
  spec.metadata["residentSession"] = "true";
  spec.metadata["stateSuccessorMap"] = "stale=map";
  bindNativeRunnerPreparationContext(spec, projection, {"/provider", "boot", 1, "/tmp"});
  BOOST_REQUIRE(readAutomaticOnnxRunnerIo(spec));
  BOOST_CHECK_EQUAL(spec.metadata.at("automaticIoContract"), local.canonicalJson());
  BOOST_CHECK_EQUAL(spec.metadata.count("stateInputNames"), 0U);
  BOOST_CHECK_EQUAL(spec.metadata.count("stateSuccessorMap"), 0U);
  BOOST_CHECK_EQUAL(spec.metadata.count("providerProfilePrefix"), 0U);
  std::swap(projection.generationContract.stateOutputNames[0],
            projection.generationContract.stateOutputNames[1]);
  BOOST_CHECK_EQUAL(rejection([&] { automaticOnnxIoContractForProjection(projection); }),
                    "AUTOMATIC_IO_CROSSED_PAIR");
  BOOST_CHECK_THROW(bindNativeRunnerPreparationContext(spec, projection,
    {"/provider", "boot", 1, "/tmp"}), std::exception);
  projection = ioProjection();
  projection.generationContract.stateInputNames.push_back(PAST);
  projection.generationContract.stateOutputNames.push_back("remote-output");
  BOOST_CHECK_THROW(automaticOnnxIoContractForProjection(projection), std::exception);
}

BOOST_AUTO_TEST_CASE(StatelessSingleOutputRetainsNamedTensorFrame)
{
  IoModel model("single", false);
  auto spec = ioSpec(model, false);
  OnnxRuntimeModelRunner runner(spec);
  BOOST_CHECK(!runner.supportsOpaqueStateHandles());
  const auto output = runner.run(ioRound(model, "/io/stateless", 0, 7, false));
  BOOST_REQUIRE_EQUAL(output.size(), 1U);
  BOOST_CHECK(isEncodedTensorBundle(output.begin()->second.payload));
  BOOST_CHECK_EQUAL(observedScalar(output, model.value), 7);
  auto bare = ioRound(model, "/io/bare", 0, 7, false);
  bare.inputsByScope.clear();
  TensorBundle raw;
  raw.name = model.token;
  raw.payload = scalar(model.token, 7).payload;
  raw.expectedBytes = raw.payload.size();
  bare.inputsByScope.emplace("input", std::move(raw));
  BOOST_CHECK_EQUAL(rejection([&] { runner.run(bare); }),
                    "AUTOMATIC_IO_NAMED_INPUT_REQUIRED: " + model.token);
  auto scalarInput = scalar(model.token, 7);
  scalarInput.shape.clear();
  auto wrongRank = ioRound(model, "/io/wrong-rank", 0, 7, false);
  wrongRank.inputsByScope.at("input") = makeEncodedTensorBundle("input", {scalarInput});
  BOOST_CHECK_THROW(runner.run(wrongRank), std::exception);
}

BOOST_AUTO_TEST_CASE(ScalarRankIsPreservedWithoutFallbackDimensions)
{
  IoModel model("scalar", false, false, false, {});
  auto projection = ioProjection(false);
  projection.assembly.expectedInputs.at(0).shape.clear();
  projection.assembly.expectedOutputs.at(0).shape.clear();
  projection.selectedRole = projection.assembly;
  auto spec = ioSpec(model, false);
  bindAutomaticOnnxRunnerIo(spec, projection);
  OnnxRuntimeModelRunner runner(spec);
  auto tensor = scalar(model.token, 7);
  tensor.shape.clear();
  auto context = ioRound(model, "/io/scalar", 0, 7, false);
  context.inputsByScope.at("input") = makeEncodedTensorBundle("input", {tensor});
  const auto outputs = runner.run(context);
  BOOST_REQUIRE_EQUAL(outputs.size(), 1U);
  const auto tensors = decodeTensorBundle(outputs.begin()->second.payload);
  BOOST_REQUIRE_EQUAL(tensors.size(), 1U);
  BOOST_CHECK_EQUAL(tensors.front().name, model.value);
  BOOST_CHECK(tensors.front().shape.empty());
  BOOST_CHECK(tensors.front().elementType == TensorElementType::Float32);
  BOOST_REQUIRE_EQUAL(tensors.front().payload.size(), sizeof(float));
  float value;
  std::memcpy(&value, tensors.front().payload.data(), sizeof(value));
  BOOST_CHECK_EQUAL(value, 7);
}

BOOST_AUTO_TEST_CASE(ResidentRoundsKeepRawPairsAndFreshRequestsIsolated)
{
  IoModel model("resident");
  OnnxRuntimeModelRunner runner(ioSpec(model));
  BOOST_REQUIRE(runner.supportsOpaqueStateHandles());
  BOOST_CHECK_EQUAL(rejection([&] {
    runner.run(ioRound(model, "/io/missing-state", 0, 1));
  }), "AUTOMATIC_IO_EXPLICIT_INPUT_REQUIRED: " + model.past);
  BOOST_CHECK(!runner.stateHandleSnapshot("/io/missing-state").has_value());
  BOOST_CHECK_EQUAL(observedScalar(runner.run(
    ioRound(model, "/io/A", 0, 1, true, true)), model.value), 1);
  BOOST_CHECK_EQUAL(observedScalar(runner.run(
    ioRound(model, "/io/A", 1, 2)), model.value), 3);
  BOOST_CHECK_EQUAL(observedScalar(runner.run(
    ioRound(model, "/io/B", 0, 1, true, true)), model.value), 1);
  BOOST_CHECK(runner.stateHandleSnapshot("/io/A").has_value());
  runner.releaseSessionState("/io/A");
  BOOST_CHECK(!runner.stateHandleSnapshot("/io/A").has_value());
  BOOST_CHECK_THROW(runner.run(ioRound(model, "/io/A", 1, 2)), std::exception);
  runner.releaseSessionState("/io/B");
}

BOOST_AUTO_TEST_CASE(PassthroughAndUnrankedListsPreserveRawNames)
{
  IoModel model("passthrough", false, false, true);
  auto projection = ioProjection(false);
  projection.assembly.expectedInputs.push_back({PAST, "float32", {}});
  projection.selectedRole = projection.assembly;
  projection.dataflow.terminalResponseOwner = false;
  NativeTensorEndpointV3 output;
  output.bundleTensorNames = {PAST};
  projection.dataflow.mayPublish.push_back(output);
  auto spec = ioSpec(model, false);
  bindAutomaticOnnxRunnerIo(spec, projection);
  const auto io = readAutomaticOnnxRunnerIo(spec);
  BOOST_REQUIRE(io);
  BOOST_CHECK(io->unrankedInputNames == std::vector<std::string>{PAST});
  BOOST_CHECK(io->passthroughTensors == std::vector<std::string>{PAST});
  OnnxRuntimeModelRunner runner(spec);
  const auto outputs = runner.run(ioRound(model, "/io/pass", 0, 2, false, true, 7));
  BOOST_CHECK_EQUAL(observedScalar(outputs, model.value), 2);
  BOOST_CHECK_EQUAL(observedScalar(outputs, model.past), 7);
}

BOOST_AUTO_TEST_CASE(ActualSessionRejectsWrongPortOrderAndMalformedMetadataFirst)
{
  IoModel model("signature");
  auto spec = ioSpec(model);
  const auto original = *readAutomaticOnnxRunnerIo(spec);
  for (const bool inputs : {true, false}) {
    auto io = original;
    auto& names = inputs ? io.io.inputNames : io.io.outputNames;
    std::swap(names[0], names[1]);
    spec.metadata["automaticIoContract"] = io.canonicalJson();
    BOOST_CHECK_EQUAL(rejection([&] { OnnxRuntimeModelRunner runner(spec); }),
                      "AUTOMATIC_IO_SESSION_SIGNATURE");
  }
  spec.path = "/missing/spec233/model.onnx";
  spec.metadata["automaticIoContract"] = "{}";
  BOOST_CHECK_EQUAL(rejection([&] { OnnxRuntimeModelRunner runner(spec); }),
                    "AUTOMATIC_IO_SCHEMA");
}

BOOST_AUTO_TEST_CASE(SessionCacheSeparatesIoIncludingProtectedIdentity)
{
  IoModel model("session-cache");
  for (const bool protectedIdentity : {false, true}) {
    OnnxRuntimeSessionCache::Config config;
    config.maxEntries = 4;
    auto cache = std::make_shared<OnnxRuntimeSessionCache>(config);
    auto spec = ioSpec(model);
    if (protectedIdentity)
      spec.metadata["protectedResidentIdentity"] = "fixed-protected-identity";
    {
      OnnxRuntimeModelRunner first(spec, cache);
      OnnxRuntimeModelRunner second(spec, cache);
      BOOST_CHECK_EQUAL(cache->counters().loads, 1U);
      BOOST_CHECK_EQUAL(cache->counters().hits, 1U);
      auto other = spec;
      auto io = *readAutomaticOnnxRunnerIo(other);
      io.io.stateInputNames.clear(); io.io.stateOutputNames.clear();
      io.io.successorInputByOutput.clear();
      other.metadata["automaticIoContract"] = io.canonicalJson();
      other.metadata["statefulModel"] = "false";
      OnnxRuntimeModelRunner distinct(other, cache);
      BOOST_CHECK_EQUAL(cache->counters().loads, 2U);
      BOOST_CHECK(!distinct.supportsOpaqueStateHandles());
      auto mixed = spec;
      mixed.metadata["stateInputNames"] = "bad";
      BOOST_CHECK_THROW(OnnxRuntimeModelRunner(mixed, cache), std::exception);
      BOOST_CHECK_EQUAL(cache->counters().hits, 1U);
      BOOST_CHECK_EQUAL(cache->counters().loads, 2U);
    }
    cache->close();
    BOOST_CHECK(cache->drain(std::chrono::milliseconds(50)));
  }
}

BOOST_AUTO_TEST_CASE(LegacyCsvResidentStateStillRunsWithoutAutomaticMetadata)
{
  IoModel model("legacy", true, true);
  NativeModelRunnerSpec spec;
  spec.role = "/role"; spec.kind = "onnx"; spec.backend = "onnxruntime-cpu";
  spec.path = model.path();
  spec.metadata = {{"inputNames", "input,past"}, {"outputNames", "score,present"},
    {"stateInputNames", "past"}, {"stateOutputNames", "present"},
    {"stateSuccessorMap", "past=present"}, {"providerIdentity", "/spec233/io-provider"},
    {"providerBootId", "io-boot"}};
  OnnxRuntimeModelRunner runner(spec);
  BOOST_CHECK_EQUAL(observedScalar(runner.run(
    ioRound(model, "/io/legacy", 0, 1, true, true)), model.value), 1);
  BOOST_CHECK_EQUAL(observedScalar(runner.run(
    ioRound(model, "/io/legacy", 1, 2)), model.value), 3);
  runner.releaseSessionState("/io/legacy");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::test
