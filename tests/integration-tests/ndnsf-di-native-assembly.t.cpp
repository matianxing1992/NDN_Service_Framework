#include "tests/fixtures/test-resource-paths.hpp"
#include "tests/fixtures/native-worker-location.hpp"
#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeYoloMergeRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-entry-runner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "ndnsf-integration-fixture.hpp"

#include <ndn-cxx/util/sha256.hpp>
#include <onnx/onnx_pb.h>
#include <openssl/evp.h>

#include <boost/test/unit_test.hpp>
#include <boost/scope_exit.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <vector>

namespace ndnsf::di::tests {
namespace {

using namespace ndn_service_framework;
namespace fixture = ndn_service_framework::test;

std::filesystem::path
findFixture()
{
  return ndnsf::test::testFixturePath(
    "spec175/tiny-causal-lm-v1/two-role/role-0.onnx");
}

std::vector<std::uint8_t>
readBytes(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                  std::istreambuf_iterator<char>());
}

// OA02 worker used by every post-Selection assembly in this suite.  The
// integration binary is not a shell of the worker: the spawned child is the
// real installed worker resolved by the production preflight, with an explicit
// NDNSF_DI_WORKER_BINARY or legacy bin-directory override when requested.
NativeOnnxWorkerLocation
testWorkerLocation()
{
  return ndnsf::test::testNativeWorkerLocation();
}

std::string
digest(const std::vector<std::uint8_t>& bytes)
{
  ndn::util::Sha256 hash;
  hash.update(ndn::span<const std::uint8_t>(bytes.data(), bytes.size()));
  auto hex = hash.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return "sha256:" + hex;
}

std::string
digest(const std::string& text)
{
  return digest(std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::string
zeroDigest(char value = '0')
{
  return "sha256:" + std::string(64, value);
}

std::string
jsonQuote(const std::string& value)
{
  std::string escaped = value;
  std::string output = "\"";
  for (const auto ch : escaped) {
    if (ch == '\\' || ch == '\"') {
      output.push_back('\\');
    }
    output.push_back(ch);
  }
  output.push_back('\"');
  return output;
}

std::string
recipeDigestFor(const NativeSelectionRoleV3& role)
{
  // The tiny fixture has concrete integer dimensions and one symbolic axis.
  // Hash JSON integers for concrete axes, as required by the Python recipe.
  const auto dimensionJson = [] (const auto& dimension) {
    if (std::holds_alternative<std::string>(dimension)) {
      return jsonQuote(std::get<std::string>(dimension));
    }
    const auto value = std::get<std::int64_t>(dimension);
    BOOST_REQUIRE(value >= 0);
    return std::to_string(value);
  };
  std::ostringstream wire;
  wire << "{\"adapterDescriptorDigest\":" << jsonQuote(role.adapterDescriptorDigest)
       << ",\"artifactProfileDigest\":" << jsonQuote(role.artifactProfileDigest)
       << ",\"assemblerDescriptorDigest\":" << jsonQuote(role.assemblerDescriptorDigest)
       << ",\"backendAbi\":" << jsonQuote(role.backendAbi)
       << ",\"canonicalInitializerDigest\":"
       << jsonQuote(role.canonicalInitializerDigest) << ",\"expectedInputs\":[";
  for (std::size_t index = 0; index < role.expectedInputs.size(); ++index) {
    if (index != 0) wire << ',';
    const auto& item = role.expectedInputs[index];
    wire << "{\"dtype\":" << jsonQuote(item.dtype)
         << ",\"name\":" << jsonQuote(item.name) << ",\"shape\":[";
    for (std::size_t dimension = 0; dimension < item.shape.size(); ++dimension) {
      if (dimension != 0) wire << ',';
      wire << dimensionJson(item.shape[dimension]);
    }
    wire << "]}";
  }
  wire << "],\"expectedOutputs\":[";
  for (std::size_t index = 0; index < role.expectedOutputs.size(); ++index) {
    if (index != 0) wire << ',';
    const auto& item = role.expectedOutputs[index];
    wire << "{\"dtype\":" << jsonQuote(item.dtype)
         << ",\"name\":" << jsonQuote(item.name) << ",\"shape\":[";
    for (std::size_t dimension = 0; dimension < item.shape.size(); ++dimension) {
      if (dimension != 0) wire << ',';
      wire << dimensionJson(item.shape[dimension]);
    }
    wire << "]}";
  }
  wire << "],\"graphDigest\":" << jsonQuote(role.graphDigest)
       << ",\"inputNames\":[";
  // The production canonical serializer binds inputNames/outputNames to the
  // contract order.  Do not sort this helper independently: that would make
  // the fixture's certified recipe digest disagree with the worker payload.
  std::vector<std::string> inputNames;
  for (const auto& item : role.expectedInputs) inputNames.push_back(item.name);
  for (std::size_t index = 0; index < inputNames.size(); ++index) {
    if (index != 0) wire << ',';
    wire << jsonQuote(inputNames[index]);
  }
  wire << "],\"layerBegin\":" << role.layerBegin
       << ",\"layerEnd\":" << role.layerEnd
       << ",\"layout\":" << jsonQuote(role.layout)
       << ",\"maxAssembledBytes\":" << role.maxAssembledBytes
       << ",\"maxNodes\":" << role.maxNodes
       << ",\"maxSourceBytes\":" << role.maxSourceBytes
       << ",\"modelManifestDigest\":"
       << jsonQuote(role.modelManifestDigest) << ",\"nodeIndices\":[";
  for (std::size_t index = 0; index < role.nodeIndices.size(); ++index) {
    if (index != 0) wire << ',';
    wire << role.nodeIndices[index];
  }
  wire << "],\"outputNames\":[";
  std::vector<std::string> outputNames;
  for (const auto& item : role.expectedOutputs) outputNames.push_back(item.name);
  for (std::size_t index = 0; index < outputNames.size(); ++index) {
    if (index != 0) wire << ',';
    wire << jsonQuote(outputNames[index]);
  }
  wire << "],\"padding\":" << jsonQuote(role.padding)
       << ",\"precision\":" << jsonQuote(role.precision)
       << ",\"quantization\":" << jsonQuote(role.quantization)
       << ",\"roleKind\":" << jsonQuote(role.roleKind)
       << ",\"schema\":\"ndnsf-di-certified-onnx-assembly-v1\"}";
  return digest(wire.str());
}

std::string
candidateTensorDtype(const onnx::ValueInfoProto& value)
{
  if (!value.has_type() || !value.type().has_tensor_type()) {
    throw std::invalid_argument("Spec190 candidate tensor has no tensor type: " + value.name());
  }
  switch (value.type().tensor_type().elem_type()) {
    case onnx::TensorProto::FLOAT:
      return "float32";
    case onnx::TensorProto::FLOAT16:
      return "float16";
    case onnx::TensorProto::INT64:
      return "int64";
    default:
      throw std::invalid_argument("Spec190 candidate tensor dtype is unsupported: " + value.name());
  }
}

std::vector<std::variant<std::int64_t, std::string>>
candidateTensorShape(const onnx::ValueInfoProto& value)
{
  const auto& tensor = value.type().tensor_type();
  if (!tensor.has_shape()) {
    throw std::invalid_argument("Spec190 candidate tensor has no shape: " + value.name());
  }
  std::vector<std::variant<std::int64_t, std::string>> shape;
  for (const auto& dimension : tensor.shape().dim()) {
    if (dimension.has_dim_value()) {
      shape.emplace_back(dimension.dim_value());
    }
    else if (dimension.has_dim_param() && !dimension.dim_param().empty()) {
      shape.emplace_back(dimension.dim_param());
    }
    else {
      throw std::invalid_argument("Spec190 candidate tensor has an unnamed dynamic dimension: " +
                                  value.name());
    }
  }
  return shape;
}

std::vector<NativeAssemblyTensorContractV3>
candidateTensorContracts(const google::protobuf::RepeatedPtrField<onnx::ValueInfoProto>& values)
{
  std::vector<NativeAssemblyTensorContractV3> result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.push_back({value.name(), candidateTensorDtype(value), candidateTensorShape(value)});
  }
  return result;
}

std::string
joinCandidateNames(const std::vector<std::string>& names)
{
  std::ostringstream result;
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (index != 0) result << ',';
    result << names[index];
  }
  return result.str();
}

NativeSelectionProjectionV3
makeDirectInt8CandidateProjection(const onnx::ModelProto& model,
                                   const NativeOnnxIdentity& identity,
                                   const std::string& sourceDigest,
                                   const std::string& rootDigest,
                                   std::size_t sourceBytes)
{
  constexpr const char* role = "/LLM/Pipeline/Stage/0";
  NativeSelectionProjectionV3 projection;
  projection.provider = "/provider/spec190/int8/p0";
  projection.requestId = "/request/spec190-int8-native";
  projection.canonicalArtifactName = "/spec190/int8/candidate/root";
  projection.attempt = 1;
  projection.planDigest = zeroDigest('d');
  projection.plan.serviceName = "/LLM/Qwen";
  projection.plan.modelName = "Qwen3-0.6B-ONNX-weight-only-int8";
  projection.plan.modelFamily = "qwen";
  projection.plan.modelFormat = "onnx";
  projection.plan.executionPolicy = "DATA_DRIVEN_V2";
  projection.plan.roles = {role};

  auto& assembly = projection.assembly;
  assembly.role = role;
  assembly.selectedRole = role;
  assembly.rank = 0;
  assembly.layerBegin = 0;
  assembly.layerEnd = static_cast<std::uint64_t>(model.graph().node_size());
  assembly.backend = "onnxruntime";
  assembly.adapterId = "onnx";
  assembly.deviceSet = {"cpu"};
  assembly.artifactDigest = sourceDigest;
  assembly.roleKind = "PIPELINE_RANGE";
  assembly.modelManifestDigest = rootDigest;
  assembly.artifactProfileDigest = zeroDigest('b');
  assembly.graphDigest = identity.graphDigest;
  assembly.canonicalInitializerDigest = identity.initializerDigest;
  assembly.adapterDescriptorDigest = zeroDigest('1');
  assembly.assemblerDescriptorDigest = zeroDigest('2');
  assembly.backendAbi = "onnxruntime-cpu-v1";
  assembly.precision = "float32";
  assembly.quantization = "weight_only_int8";
  assembly.layout = "native";
  assembly.padding = "none";
  assembly.maxSourceBytes = static_cast<std::uint64_t>(sourceBytes) + (64U << 20);
  assembly.maxAssembledBytes = static_cast<std::uint64_t>(sourceBytes) * 4U;
  assembly.maxNodes = static_cast<std::uint64_t>(model.graph().node_size());
  for (std::uint64_t index = 0; index < assembly.maxNodes; ++index)
    assembly.nodeIndices.push_back(index);
  assembly.expectedInputs = candidateTensorContracts(model.graph().input());
  assembly.expectedOutputs = candidateTensorContracts(model.graph().output());
  assembly.recipeDigest = recipeDigestFor(assembly);
  projection.selectedRole = assembly;
  projection.executionRole.roleId = role;
  projection.executionRole.stageId = "stage-0";
  projection.executionRole.rank = 0;
  projection.executionRole.layerBegin = assembly.layerBegin;
  projection.executionRole.layerEnd = assembly.layerEnd;
  projection.executionRole.backend = assembly.backend;
  projection.executionRole.adapterId = assembly.adapterId;

  projection.dataflow.role = role;
  projection.dataflow.requestId = projection.requestId;
  projection.dataflow.attempt = projection.attempt;
  projection.dataflow.planDigest = projection.planDigest;

  auto& generation = projection.generationContract;
  generation.enabled = true;
  generation.mode = "qwen-causal";
  generation.maxGeneratedTokens = 2;
  generation.tokenInputName = "input_ids";
  generation.positionInputPolicy = "qwen-causal-position-v1";
  generation.attentionMaskInputName = "attention_mask";
  generation.positionIdsInputName = "position_ids";
  generation.samplingDigest = zeroDigest('e');
  generation.tokenizerDigest = zeroDigest('f');
  generation.streamingOperationStride = 1;
  std::vector<std::string> successorPairs;
  for (const auto& input : assembly.expectedInputs) {
    if (input.name.rfind("past_key_values.", 0) != 0)
      continue;
    generation.stateInputNames.push_back(input.name);
    const auto outputName = "present." + input.name.substr(std::string("past_key_values.").size());
    generation.stateSuccessorMap += (generation.stateSuccessorMap.empty() ? "" : ",") +
      input.name + "=" + outputName;
  }
  for (const auto& output : assembly.expectedOutputs) {
    if (output.name.rfind("present.", 0) == 0)
      generation.stateOutputNames.push_back(output.name);
  }
  if (generation.stateInputNames.empty() ||
      generation.stateInputNames.size() != generation.stateOutputNames.size()) {
    throw std::invalid_argument("Spec190 candidate state contract is incomplete");
  }
  return projection;
}

NamedTensor
makeInt64CandidateTensor(const std::string& name, std::int64_t value)
{
  std::vector<std::uint8_t> bytes(sizeof(value));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return {name, TensorElementType::Int64, {1, 1}, std::move(bytes)};
}

NamedTensor
makeInt64CandidateVector(const std::string& name, const std::vector<std::int64_t>& values)
{
  std::vector<std::uint8_t> bytes(values.size() * sizeof(std::int64_t));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return {name, TensorElementType::Int64,
          {1, static_cast<std::int64_t>(values.size())}, std::move(bytes)};
}

const NamedTensor*
findCandidateTensor(const std::vector<NamedTensor>& tensors, const std::string& name)
{
  const auto found = std::find_if(tensors.begin(), tensors.end(), [&name] (const auto& tensor) {
    return tensor.name == name;
  });
  return found == tensors.end() ? nullptr : &*found;
}

class ScopedEnv
{
public:
  ScopedEnv(const char* name, const std::string& value)
    : m_name(name)
  {
    if (const auto* previous = std::getenv(name)) {
      m_previous = previous;
    }
    ::setenv(name, value.c_str(), 1);
  }

  ~ScopedEnv()
  {
    if (m_previous) {
      ::setenv(m_name.c_str(), m_previous->c_str(), 1);
    }
    else {
      ::unsetenv(m_name.c_str());
    }
  }

private:
  std::string m_name;
  std::optional<std::string> m_previous;
};

NativeSelectionProjectionV3
makeProjection(const std::string& rootDigest,
               const std::string& profileDigest,
               const std::string& graphDigest,
               const std::string& initializerDigest)
{
  NativeSelectionProjectionV3 projection;
  projection.provider = "/provider/p0";
  projection.requestId = "/request/spec175-assembly";
  projection.canonicalArtifactName = "/spec175/native/root";
  projection.plan.serviceName = "/LLM/Qwen";
  projection.plan.modelName = "spec175-tiny-causal-lm-v1";

  auto& role = projection.assembly;
  role.role = "/LLM/Pipeline/Stage/0";
  role.selectedRole = role.role;
  role.rank = 0;
  role.layerBegin = 0;
  role.layerEnd = 2;
  role.backend = "onnxruntime";
  role.adapterId = "onnx";
  role.deviceSet = {"cpu"};
  role.artifactDigest = zeroDigest('c');
  role.roleKind = "PIPELINE_RANGE";
  role.modelManifestDigest = rootDigest;
  role.artifactProfileDigest = profileDigest;
  role.graphDigest = graphDigest;
  role.canonicalInitializerDigest = initializerDigest;
  role.adapterDescriptorDigest = zeroDigest('1');
  role.assemblerDescriptorDigest = zeroDigest('2');
  role.backendAbi = "onnxruntime-cpu-v1";
  for (std::uint64_t index = 0; index < 21; ++index) {
    role.nodeIndices.push_back(index);
  }
  role.expectedInputs = {
    {"input_ids", "int64", {"1", "sequence"}},
    {"attention_kv_in", "float32", {"2", "8"}},
    {"recurrent_state_in", "float32", {"2", "8"}},
    {"convolution_state_in", "float32", {"2", "8"}},
  };
  role.expectedOutputs = {
    {"hidden_out", "float32", {"1", "sequence", "8"}},
    {"attention_kv_out", "float32", {"2", "8"}},
    {"recurrent_state_out", "float32", {"2", "8"}},
    {"convolution_state_out", "float32", {"2", "8"}},
  };
  role.precision = "float32";
  role.quantization = "none";
  role.layout = "native";
  role.padding = "none";
  role.maxSourceBytes = 1024 * 1024;
  role.maxAssembledBytes = 1024 * 1024;
  role.maxNodes = 64;
  role.recipeDigest = recipeDigestFor(role);
  projection.attempt = 1;
  projection.planDigest = zeroDigest('d');
  projection.executionRole.roleId = role.role;
  projection.dataflow.role = role.role;
  projection.dataflow.requestId = projection.requestId;
  projection.dataflow.attempt = projection.attempt;
  projection.dataflow.planDigest = projection.planDigest;
  return projection;
}

void
runRegisteredProviderAssemblyCase(std::size_t providerCount)
{
  BOOST_REQUIRE(providerCount == 1 || providerCount == 2 || providerCount == 4);
  const auto fixture = findFixture();
  BOOST_REQUIRE(!fixture.empty());
  const auto source = readBytes(fixture);
  BOOST_REQUIRE(!source.empty());
  const auto sourceDigest = digest(source);
  const auto profileDigest = zeroDigest('b');
  const auto graphDigest =
    "sha256:39bea16fd2b8d6163cda5e9c3875a05a83936fc163dff2c0406b92c22364103d";
  const auto initializerDigest =
    "sha256:074d3acd4acd13d94c4d27e9a201255ed9fd72f1783e4a056b7c6509deb6be9b";
  const auto suffix = std::to_string(providerCount);
  const auto rootName = ndn::Name("/spec175/native/registered/root/" + suffix);
  const auto sourceName = ndn::Name("/spec175/native/registered/source/" + suffix);
  const auto rootJson = std::string(
    "{\"artifactProfileDigest\":\"") + profileDigest +
    "\",\"metadata\":{\"canonicalSourceBytes\":" +
    std::to_string(source.size()) +
    ",\"canonicalSourceDataName\":\"" + sourceName.toUri() +
    "\",\"canonicalSourceDigest\":\"" + sourceDigest +
    "\"},\"modelIdentityDigest\":\"" + zeroDigest('a') +
    "\",\"modelName\":\"spec175-tiny-causal-lm-v1\","
    "\"schema\":\"ndnsf-di-canonical-model-manifest-v1\","
    "\"state\":\"ACTIVE\"}";
  const auto rootPayload = std::vector<std::uint8_t>(rootJson.begin(), rootJson.end());
  auto projection = makeProjection(
    digest(rootPayload), profileDigest, graphDigest, initializerDigest);
  projection.canonicalArtifactName = rootName.toUri();
  projection.requestId = "/request/spec175-registered/" + suffix;
  // The production role-spec projector requires the authenticated V3
  // dataflow envelope to bind the local role to this exact request attempt
  // and sealed plan.  This fixture exercises the real assembler/runner path,
  // so keep those bindings explicit instead of relying on empty defaults.
  projection.attempt = 1;
  projection.planDigest = zeroDigest('d');
  projection.executionRole.roleId = projection.assembly.role;
  projection.dataflow.role = projection.executionRole.roleId;
  projection.dataflow.requestId = projection.requestId;
  projection.dataflow.attempt = projection.attempt;
  projection.dataflow.planDigest = projection.planDigest;

  ScopedEnv pythonPath(
    "PYTHONPATH", "NDNSF-DistributedInference:NDNSF-DistributedRepo/pythonWrapper");
  const auto baseCacheDir = std::filesystem::temp_directory_path() /
    ("spec175-native-assembly-registered-" + suffix);
  std::error_code cleanupError;
  std::filesystem::remove_all(baseCacheDir, cleanupError);

  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [rootName, rootPayload] (const ndn::Name& name)
    -> std::optional<ndn::Buffer> {
    if (name != rootName) {
      return std::nullopt;
    }
    return ndn::Buffer(rootPayload.data(), rootPayload.size());
  };
  fetchers.fetchEncryptedLargeData = [sourceName, source] (
      const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (name != sourceName || service != ndn::Name("/LLM/Qwen")) {
      return std::nullopt;
    }
    return ndn::Buffer(source.data(), source.size());
  };

  NativeCanonicalOnnxAssemblerOptions options;
  options.workerLocation = testWorkerLocation();
  options.signManifest = [] (const std::string& manifestBytes) {
    return "fixture-signature-" + digest(manifestBytes);
  };

  // Each simulated registered Provider runs this same post-Selection path in
  // its own cache namespace.  The Python oracle invokes each Boost case in a
  // separate process, so no startup-time role artifact can satisfy the check.
  for (std::size_t index = 0; index < providerCount; ++index) {
    auto providerProjection = projection;
    providerProjection.provider = "/provider/registered/" + suffix +
                                  "/p" + std::to_string(index);
    options.providerIdentity = providerProjection.provider;
    options.cacheDir = (baseCacheDir / ("p" + std::to_string(index))).string();
    auto prepared = prepareNativeCanonicalOnnxRole(
      fetchers, providerProjection, options);
    BOOST_REQUIRE(std::filesystem::is_regular_file(prepared.path));
    BOOST_CHECK_EQUAL(prepared.metadata.at("assembledFrom"),
                      "canonical-root-post-selection");
    BOOST_CHECK_EQUAL(prepared.metadata.at("modelManifestDigest"),
                      providerProjection.assembly.modelManifestDigest);

    // Construction performs the real C++ ORT session load and shape-valid
    // warmup; a synthetic runner or a ready-made startup file cannot satisfy it.
    bindNativeRunnerPreparationContext(prepared, providerProjection,
      {providerProjection.provider, "registered-provider-boot", 1, options.cacheDir});
    OnnxRuntimeModelRunner runner(prepared);
    const auto evidence = runner.executionEvidenceSnapshot();
    BOOST_REQUIRE(evidence);
    BOOST_CHECK(evidence->runnerKind == RunnerKind::OnnxRuntimeCpu);
    BOOST_CHECK(evidence->realCompute);
    BOOST_CHECK(evidence->loadCompleted);
    BOOST_CHECK(evidence->warmupCompleted);
    BOOST_CHECK_EQUAL(evidence->deviceKind, "cpu");
    BOOST_CHECK_EQUAL(evidence->providerName, providerProjection.provider);
    BOOST_CHECK_EQUAL(evidence->modelDigest, providerProjection.assembly.modelManifestDigest);
    BOOST_CHECK_EQUAL(evidence->planDigest, providerProjection.planDigest);
  }
  std::filesystem::remove_all(baseCacheDir, cleanupError);
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec175NativeAssembly)

static void
checkPairedFfnMaterialAssembly(bool multiEntry)
{
  constexpr std::uint64_t WIDTH = 524296;
  constexpr auto HALF = WIDTH / 2;
  constexpr auto MATRIX_BYTES = WIDTH * 4;
  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(20);
  auto& graph = *model.mutable_graph();
  graph.set_name("spec263-external-ffn");
  for (auto* value : {graph.add_input(), graph.add_output()}) {
    auto* type = value->mutable_type()->mutable_tensor_type();
    type->set_elem_type(onnx::TensorProto::FLOAT);
    type->mutable_shape()->add_dim()->set_dim_value(1);
    type->mutable_shape()->add_dim()->set_dim_value(1);
  }
  graph.mutable_input(0)->set_name("hidden");
  graph.mutable_output(0)->set_name("partial");
  const auto addMatrix = [&] (const char* name, std::uint64_t rows, std::uint64_t columns,
                              std::uint64_t offset) {
    auto& tensor = *graph.add_initializer();
    tensor.set_name(name);
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    tensor.add_dims(rows);
    tensor.add_dims(columns);
    tensor.set_data_location(onnx::TensorProto::EXTERNAL);
    for (const auto& [key, value] : std::map<std::string, std::string>{
           {"location", "source.data"}, {"offset", std::to_string(offset)},
           {"length", std::to_string(MATRIX_BYTES)}}) {
      auto* item = tensor.add_external_data();
      item->set_key(key);
      item->set_value(value);
    }
  };
  addMatrix("W1", 1, WIDTH, 0);
  addMatrix("W2", WIDTH, 1, MATRIX_BYTES);
  const auto addOp = [&] (const char* kind, std::initializer_list<const char*> in,
                          const char* out) {
    auto* node = graph.add_node();
    node->set_op_type(kind);
    for (const auto name : in) node->add_input(name);
    node->add_output(out);
  };
  addOp("MatMul", {"hidden", "W1"}, "expanded");
  addOp("Gelu", {"expanded"}, "activated");
  addOp("MatMul", {"activated", "W2"}, "partial");

  if (multiEntry) {
    addMatrix("W3", 1, WIDTH, 2 * MATRIX_BYTES);
    addMatrix("W4", WIDTH, 1, 3 * MATRIX_BYTES);
    addOp("MatMul", {"partial", "W3"}, "expanded-next");
    addOp("Gelu", {"expanded-next"}, "activated-next");
    addOp("MatMul", {"activated-next", "W4"}, "next");
    graph.mutable_output(0)->set_name("next");
  }
  NativeCanonicalSource source;
  const auto wire = model.SerializeAsString();
  source.modelBytes.assign(wire.begin(), wire.end());
  source.initializerBytes.emplace((multiEntry ? 4 : 2) * MATRIX_BYTES);
  for (std::uint64_t index = 0; index < source.initializerBytes->size(); ++index)
    (*source.initializerBytes)[index] = (index ^ (index >> 12) ^ (index >> 21)) & 0xff;
  const NativeAssemblyControl control{std::chrono::steady_clock::now() + std::chrono::seconds(45),
    [] {}, 16U << 20, 32U << 20};
  const auto material = deriveNativeCanonicalMaterialManifest(source, control);
  const auto identity = canonicalOnnxSourceIdentity(source, control);
  const auto manifest = material->canonicalJson();
  const std::string manifestName = "/spec263/ffn/material/manifest";
  const std::string receiptName = "/spec263/ffn/material/receipt";
  const std::string rootName = "/spec263/ffn/root";
  auto objects = NativeJson::array();
  std::map<std::string, std::vector<std::uint8_t>> payloads;
  for (const auto& payload : material->payloads) {
    const auto name = "/spec263/ffn/material/" + payload.payloadId;
    payloads.emplace(name, payload.copyBytes());
    objects.push_back({{"payloadId", payload.payloadId}, {"dataName", name},
                       {"bytes", payload.byteSize()}, {"digest", payload.digest}});
  }
  const auto receipt = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-canonical-material-receipt-v1"}, {"sourceDigest", material->sourceDigest},
    {"graphDigest", material->graphDigest}, {"materialIdentityDigest", material->manifestDigest},
    {"materialManifestDigest", digest(manifest)}, {"materialObjects", objects}});
  payloads.emplace(manifestName, std::vector<std::uint8_t>(manifest.begin(), manifest.end()));
  payloads.emplace(receiptName, std::vector<std::uint8_t>(receipt.begin(), receipt.end()));
  const auto profile = zeroDigest('b');
  const auto root = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-canonical-model-manifest-v1"}, {"state", "ACTIVE"},
    {"artifactProfileDigest", profile}, {"modelIdentityDigest", zeroDigest('a')},
    {"modelName", "spec263-external-ffn"},
    {"metadata", {{"canonicalSourceDigest", material->sourceDigest},
                  {"canonicalSourceBytes", source.modelBytes.size()},
                  {"materialIdentityDigest", material->manifestDigest},
                  {"materialManifestDataName", manifestName},
                  {"materialManifestDigest", digest(manifest)},
                  {"materialManifestBytes", manifest.size()},
                  {"materialReceiptDataName", receiptName},
                  {"materialReceiptDigest", digest(receipt)},
                  {"materialReceiptBytes", receipt.size()}}}});
  auto projection = makeProjection(digest(root), profile, identity.graphDigest,
                                   identity.initializerDigest);
  projection.plan.modelName = "spec263-external-ffn";
  projection.canonicalArtifactName = rootName;
  auto& role = projection.assembly;
  role.roleKind = "COMPONENT_SET";
  role.layerEnd = 0;
  role.backend = "onnxruntime-cpu";
  role.deviceSet.clear();
  role.adapterVersion = "1";
  role.nodeIndices = {0, 1, 2};
  role.expectedInputs = candidateTensorContracts(graph.input());
  role.expectedOutputs = candidateTensorContracts(graph.output());
  role.maxSourceBytes = control.maxSourceBytes;
  role.maxAssembledBytes = control.maxAssembledBytes;
  role.tensorParallelDegree = 2;
  role.tensorParallelRank = 1;
  role.protectionEpoch = "spec263-ffn-epoch";
  role.tensorShardV1 = nativeCanonicalJson(NativeJson{
    {"stage_layout_digest", zeroDigest('f')},
    {"axes", NativeJson::array({{{"axis", "ffn-channel"}, {"begin", HALF},
                                 {"end", WIDTH}, {"extent", WIDTH}}})}});
  NativeOnnxSourceTransformV1 pair;
  pair.kind = "ffn-pair-partial";
  pair.axis = "ffn-channel";
  pair.node = "onnx-node-0";
  pair.downNode = "onnx-node-2";
  pair.input = "hidden";
  pair.output = "partial";
  pair.weight = "W1";
  pair.downWeight = "W2";
  pair.begin = HALF;
  pair.end = pair.extent = WIDTH;
  role.sourceTransformV1 = pair.canonicalJson();
  role.recipeDigest = nativePlanningDigest(canonicalNativeOnnxRecipeJson(role));
  projection.selectedRole = role;
  projection.plan.roles = {"Rank0", role.selectedRole};
  projection.plan.tensorDegree = 2;
  projection.plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
  projection.plan.stageLayout = {{"ffn", zeroDigest('e'), projection.plan.roles}};
  projection.executionRole.stageId = "ffn";
  projection.executionRole.rank = 1;
  projection.executionRole.backend = role.backend;
  projection.executionRole.adapterId = role.adapterId;
  projection.executionRole.adapterVersion = role.adapterVersion;
  NativeExecutionStepSpec call;
  call.stepId = role.selectedRole + "/RUN";
  call.unitId = role.selectedRole;
  call.operation = "CALL_SUBGRAPH";
  call.artifactEntry = role.selectedRole + "/artifact";
  NativeStepSlotSpec inputSlot;
  inputSlot.slotId = "hidden";
  inputSlot.dtype = "float32";
  inputSlot.shape = {1, 1};
  inputSlot.layoutDigest = zeroDigest('a');
  inputSlot.source = "EPOCH_INPUT";
  auto outputSlot = inputSlot;
  outputSlot.slotId = "partial";
  outputSlot.source = "LOCAL_STEP";
  call.inputSlots = {inputSlot};
  call.outputSlots = {outputSlot};
  projection.plan.steps = {call};
  if (multiEntry) {
    auto next = pair;
    next.node = "onnx-node-3";
    next.downNode = "onnx-node-5";
    next.input = "partial";
    next.output = "next";
    next.weight = "W3";
    next.downWeight = "W4";
    auto nextCall = call;
    nextCall.stepId += "/1";
    nextCall.artifactEntry += "/1";
    nextCall.inputSlots.front().slotId = "partial";
    nextCall.inputSlots.front().source = "LOCAL_STEP";
    nextCall.inputSlots.front().producerStepId = call.stepId;
    nextCall.inputSlots.front().producerSlotId = "partial";
    nextCall.outputSlots.front().slotId = "next";
    projection.plan.steps.push_back(nextCall);
    NativeOnnxSourceEntryProgramV1 program;
    program.entries = {{call.artifactEntry, {"onnx-node-0", "onnx-node-1", "onnx-node-2"}, pair},
      {nextCall.artifactEntry, {"onnx-node-3", "onnx-node-4", "onnx-node-5"}, next}};
    role.roleKind = "HYBRID_RANK";
    role.layerEnd = 6;
    role.nodeIndices = {0, 1, 2, 3, 4, 5};
    role.sourceTransformV1 = program.canonicalJson();
    role.recipeDigest = nativePlanningDigest(canonicalNativeOnnxRecipeJson(role));
    projection.selectedRole = role;
  }

  projection.planCoreDigest = projection.planDigest;
  projection.securityPolicySnapshotDigest = projection.planDigest;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  projection.deadlineMs = now + 90000;
  const auto privateKey = [] (char seed) {
    const std::string raw(32, seed);
    return std::shared_ptr<EVP_PKEY>(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
      reinterpret_cast<const unsigned char*>(raw.data()), raw.size()), EVP_PKEY_free);
  };
  const auto rawPublicKey = [] (const auto& key) {
    std::string raw(32, '\0');
    auto size = raw.size();
    BOOST_REQUIRE(key);
    BOOST_REQUIRE_EQUAL(EVP_PKEY_get_raw_public_key(
      key.get(), reinterpret_cast<unsigned char*>(raw.data()), &size), 1);
    BOOST_REQUIRE_EQUAL(size, raw.size());
    return raw;
  };
  const auto publicKey = [&] (const auto& key) {
    const auto raw = rawPublicKey(key);
    return std::shared_ptr<EVP_PKEY>(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
      reinterpret_cast<const unsigned char*>(raw.data()), raw.size()), EVP_PKEY_free);
  };
  const auto authority = privateKey('a');
  const auto requester = privateKey('b');
  const auto recipient = privateKey('c');
  NativeGrantIssuerConfig issuer;
  issuer.authorityIdentity = "/spec263/authority";
  issuer.requesterIdentity = "/spec263/requester";
  issuer.protectionEpoch = role.protectionEpoch;
  issuer.keyId = "spec263-ffn-key";
  issuer.authorityPrivateKey = authority;
  issuer.requesterPublicKey = publicKey(requester);
  issuer.allowedModelManifests = {role.modelManifestDigest};
  issuer.recipientPublicKeys.emplace(projection.provider, publicKey(recipient));
  issuer.contentKey = [] (const auto&, const auto&) { return std::vector<std::uint8_t>(32, 0x42); };
  NativeSignedGrantRequest grantRequest;
  grantRequest.providerIdentity = projection.provider;
  grantRequest.requesterIdentity = issuer.requesterIdentity;
  grantRequest.requestId = projection.requestId;
  grantRequest.attempt = projection.attempt;
  grantRequest.planCoreDigest = projection.planCoreDigest;
  grantRequest.grantViewDigest = projection.planDigest;
  grantRequest.modelManifestDigest = role.modelManifestDigest;
  grantRequest.protectionEpoch = role.protectionEpoch;
  grantRequest.issuedAtMs = now;
  const auto grant = NativeArtifactGrantIssuer(issuer).issue(
    grantRequest.sign(*requester), now, projection.deadlineMs);
  projection.hasGrantBinding = true;
  projection.grantName = grant.grantName;
  projection.grantDigest = grant.grantDigest;
  const auto makeRuntime = [&] {
    ProtectedRuntimeBindingV1 binding;
    binding.provider = projection.provider;
    binding.role = role.selectedRole;
    binding.requestId = projection.requestId;
    binding.attempt = projection.attempt;
    binding.planCoreDigest = projection.planCoreDigest;
    binding.planDigest = projection.planDigest;
    binding.securityPolicySnapshotDigest = projection.securityPolicySnapshotDigest;
    binding.protectionEpoch = role.protectionEpoch;
    binding.grantName = projection.grantName;
    binding.grantDigest = projection.grantDigest;
    binding.providerBootId = "spec263-test-boot";
    binding.fencingToken = nativeProtectedFencingToken(projection, binding.providerBootId, {});
    binding.expiresAtMs = projection.deadlineMs;
    NativeProtectedGrantConfig config;
    config.publicationIdentity = issuer.requesterIdentity;
    config.authorityIdentity = issuer.authorityIdentity;
    config.authorityPublicKeyRaw = rawPublicKey(authority);
    config.recipientKey = {NativeRecipientKey::Kind::Ed25519Seed, std::string(32, 'c')};
    config.modelManifestDigest = role.modelManifestDigest;
    config.fetchGrant = [&] (const std::string&) { return grant.wireJson; };
    auto runtime = std::make_shared<ProtectedRuntime>(binding, std::move(config));
    runtime->verifyGrant(binding, now);
    return runtime;
  };
  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [&] (const ndn::Name& name) -> std::optional<ndn::Buffer> {
    if (name.toUri() != rootName) return std::nullopt;
    return ndn::Buffer(root.begin(), root.end());
  };
  fetchers.fetchEncryptedLargeData = [&] (const ndn::Name& name, const ndn::Name&)
    -> std::optional<ndn::Buffer> {
    const auto item = payloads.find(name.toUri());
    if (item == payloads.end()) return std::nullopt;
    return ndn::Buffer(item->second.begin(), item->second.end());
  };
  auto pattern = (std::filesystem::temp_directory_path() / "spec263-ffn-cache-XXXXXX").string();
  BOOST_REQUIRE(::mkdtemp(pattern.data()) != nullptr);
  BOOST_SCOPE_EXIT_ALL(&pattern) {
    std::error_code error;
    std::filesystem::remove_all(pattern, error);
  };
  NativeCanonicalOnnxAssemblerOptions options;
  options.cacheDir = pattern;
  options.providerIdentity = projection.provider;
  options.workerLocation = testWorkerLocation();
  options.signManifest = [] (const std::string& bytes) { return digest(bytes); };
  options.verifyManifest = [] (const std::string& bytes, const std::string& signature) {
    return digest(bytes) == signature;
  };
  ndn::KeyChain receiptKeys("pib-memory:", "tpm-memory:");
  if (multiEntry) {
    const auto provider = receiptKeys.createIdentity(ndn::Name(projection.provider));
    BOOST_REQUIRE(bindNativeSourceProgramManifestAuthentication(projection, options, receiptKeys,
      provider.getDefaultKey().getDefaultCertificate()));
  }
  options.roleAssemblySpecDigest = zeroDigest('e');
  options.protectedRuntime = makeRuntime();
  const auto built = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  if (multiEntry) {
    const auto entries = decodeNativeSourceEntryFiles(built.metadata.at("sourceEntryFiles"));
    BOOST_REQUIRE_EQUAL(entries.size(), 2u);
    const auto programRoot = std::filesystem::path(built.path).parent_path();
    const auto secondGraph = nativeSourceEntryGraphPath(programRoot, 1);
    const auto secondSidecar = secondGraph.parent_path() / "model.onnx.data";
    BOOST_CHECK(std::filesystem::is_regular_file(secondGraph));
    const auto bytes = readBytes(secondSidecar);
    BOOST_REQUIRE_EQUAL(bytes.size(), MATRIX_BYTES);
    const auto& original = source.initializerBytes->asVector();
    BOOST_CHECK(std::equal(bytes.begin(), bytes.begin() + HALF * 4,
                           original.begin() + 2 * MATRIX_BYTES + HALF * 4));
    BOOST_CHECK(std::equal(bytes.begin() + HALF * 4, bytes.end(),
                           original.begin() + 3 * MATRIX_BYTES + HALF * 4));
    options.protectedRuntime->complete();
    options.protectedRuntime = makeRuntime();
    const auto warm = tryLoadNativeCanonicalOnnxRoleFromCache(fetchers, projection, options, {}, {});
    BOOST_REQUIRE(warm);
    BOOST_CHECK_EQUAL(warm->metadata.at("sourceEntryFiles"), built.metadata.at("sourceEntryFiles"));
    {
      RegistryNativeModelRunnerFactory factory;
      registerOnnxRuntimeBackend(factory);
      const auto runner = std::dynamic_pointer_cast<NativeEntryRunner>(factory.create(*warm));
      BOOST_REQUIRE(runner);
      BOOST_CHECK(runner->checkTemplate(projection.plan.steps, role.selectedRole).empty());
    }
    {
      std::fstream tamper(secondSidecar, std::ios::binary | std::ios::in | std::ios::out);
      BOOST_REQUIRE(tamper.good());
      tamper.put(static_cast<char>(bytes.front() ^ 1));
    }
    BOOST_CHECK(!tryLoadNativeCanonicalOnnxRoleFromCache(fetchers, projection, options, {}, {}));
    const auto rebuilt = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
    const auto signaturePath = std::filesystem::path(rebuilt.path).parent_path() /
      "manifest.signature";
    {
      std::fstream tamper(signaturePath, std::ios::binary | std::ios::in | std::ios::out);
      BOOST_REQUIRE(tamper.good());
      const auto first = tamper.get();
      BOOST_REQUIRE(first != std::char_traits<char>::eof());
      tamper.seekp(0);
      tamper.put(static_cast<char>(first ^ 1));
    }
    BOOST_CHECK(!tryLoadNativeCanonicalOnnxRoleFromCache(fetchers, projection, options, {}, {}));
    options.protectedRuntime->complete();
    return;
  }
  BOOST_REQUIRE(built.entryContract);
  BOOST_CHECK_EQUAL(built.entryContract->entryId, call.artifactEntry);
  const auto sidecar = std::filesystem::path(built.path).parent_path() / "model.onnx.data";
  const auto bytes = readBytes(sidecar);
  const auto& original = source.initializerBytes->asVector();
  BOOST_REQUIRE_EQUAL(bytes.size(), MATRIX_BYTES);
  BOOST_CHECK(std::equal(bytes.begin(), bytes.begin() + HALF * 4,
                         original.begin() + HALF * 4));
  BOOST_CHECK(std::equal(bytes.begin() + HALF * 4, bytes.end(),
                         original.begin() + MATRIX_BYTES + HALF * 4));
  options.protectedRuntime->complete();
  options.protectedRuntime.reset();
  options.protectedRuntime = makeRuntime();
  const auto reopened = tryLoadNativeCanonicalOnnxRoleFromCache(
    fetchers, projection, options, {}, {});
  BOOST_REQUIRE(reopened);
  BOOST_CHECK_EQUAL(reopened->metadata.at("protectedCacheHit"), "true");
  BOOST_REQUIRE(reopened->entryContract);
  BOOST_CHECK(*reopened->entryContract == *built.entryContract);
  BOOST_CHECK_EQUAL(reopened->metadata.at("assembledFileSetDigest"),
                    built.metadata.at("assembledFileSetDigest"));
  {
    std::fstream tamper(sidecar, std::ios::binary | std::ios::in | std::ios::out);
    BOOST_REQUIRE(tamper.good());
    tamper.put(static_cast<char>(bytes.front() ^ 1));
  }
  BOOST_CHECK(!tryLoadNativeCanonicalOnnxRoleFromCache(fetchers, projection, options, {}, {}));
  options.protectedRuntime->complete();

  const auto initializer = std::find_if(material->references.begin(), material->references.end(),
    [] (const auto& reference) { return reference.logicalName == "W1"; });
  BOOST_REQUIRE(initializer != material->references.end());
  BOOST_REQUIRE(!initializer->chunkPayloadIds.empty());
  const auto chunkName = "/spec263/ffn/material/" + initializer->chunkPayloadIds.front();
  auto corrupt = fetchers;
  corrupt.fetchEncryptedLargeData = [&] (const ndn::Name& name, const ndn::Name& service) {
    auto payload = fetchers.fetchEncryptedLargeData(name, service);
    if (payload && name.toUri() == chunkName) payload->front() ^= 1;
    return payload;
  };
  auto rejected = options;
  rejected.cacheDir = (std::filesystem::path(pattern) / "bad-source").string();
  rejected.protectedRuntime = makeRuntime();
  BOOST_CHECK_EXCEPTION(prepareNativeCanonicalOnnxRole(corrupt, projection, rejected),
    std::runtime_error, [] (const std::runtime_error& error) {
      return std::string(error.what()) == "DI_CANONICAL_material-payload_DIGEST_MISMATCH";
    });
  rejected.protectedRuntime->complete();
  BOOST_CHECK(!std::filesystem::exists(rejected.cacheDir + "/assembled"));

  auto cancelled = options;
  cancelled.cacheDir = (std::filesystem::path(pattern) / "cancelled").string();
  cancelled.protectedRuntime = makeRuntime();
  bool staged = false;
  cancelled.reportProgress = [&] (const std::string& phase, double) {
    if (phase == "MODEL_TRANSFORMED") staged = true;
  };
  cancelled.shouldCancel = [&] { return staged; };
  BOOST_CHECK_EXCEPTION(prepareNativeCanonicalOnnxRole(fetchers, projection, cancelled),
    std::runtime_error, [] (const std::runtime_error& error) {
      return std::string(error.what()) == "DI_NATIVE_ASSEMBLY_CANCELLED";
    });
  BOOST_CHECK(staged);
  cancelled.protectedRuntime->complete();
  BOOST_CHECK(!std::filesystem::exists(cancelled.cacheDir + "/assembled"));
}

BOOST_AUTO_TEST_CASE(PairedFfnMaterialSidecarReopensAndRejectsCorruption)
{
  checkPairedFfnMaterialAssembly(false);
}

BOOST_AUTO_TEST_CASE(SourceEntryProgramReopensAllFilesAndRejectsCorruptSecondSidecar)
{
  checkPairedFfnMaterialAssembly(true);
}

BOOST_AUTO_TEST_CASE(CanonicalCallGraphsUseCertifiedRoleOnColdAndWarmAssembly)
{
  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(17);
  auto& graph = *model.mutable_graph();
  graph.set_name("canonical-call-fixture");
  const auto port = [] (onnx::ValueInfoProto& value, const char* id) {
    value.set_name(id);
    auto* tensor = value.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    tensor->mutable_shape()->add_dim()->set_dim_value(2);
  };
  port(*graph.add_input(), "X");
  port(*graph.add_output(), "Y");
  const auto declareWeight = [&](const char* name, std::size_t offset) {
    auto& weight = *graph.add_initializer();
    weight.set_name(name);
    weight.set_data_type(onnx::TensorProto::FLOAT);
    weight.add_dims(2);
    weight.set_data_location(onnx::TensorProto::EXTERNAL);
    for (const auto& [key, value] : std::map<std::string, std::string>{
           {"location", "source.data"}, {"offset", std::to_string(offset)},
           {"length", std::to_string(2 * sizeof(float))}}) {
      auto& field = *weight.add_external_data();
      field.set_key(key);
      field.set_value(value);
    }
  };
  declareWeight("W", 0);
  declareWeight("foreign", 2 * sizeof(float));
  const auto add = [&](const char* op, const char* input, const char* weight, const char* output) {
    auto& node = *graph.add_node();
    node.set_op_type(op);
    node.add_input(input);
    node.add_input(weight);
    node.add_output(output);
  };
  add("Add", "X", "W", "H");
  add("Add", "X", "foreign", "unselected");
  add("Mul", "H", "W", "Y");
  const auto wire = model.SerializeAsString();
  NativeCanonicalSource source;
  source.modelBytes.assign(wire.begin(), wire.end());
  const std::array<float, 4> weights{2.0f, 3.0f, 7.0f, 11.0f};
  source.initializerBytes.emplace(sizeof(weights));
  std::memcpy(&(*source.initializerBytes)[0], weights.data(), sizeof(weights));
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(15), [] {}, 1U << 20, 1U << 20};
  const auto assembly = canonicalOnnxSourceIdentity(source, control);
  const auto material = deriveNativeCanonicalMaterialManifest(source, control);
  const auto manifest = material->canonicalJson();
  const auto profile = zeroDigest('b');
  const ndn::Name rootName("/spec175/native/root");
  const ndn::Name manifestName("/spec175/native/manifest");
  const ndn::Name receiptName("/spec175/native/receipt");
  const ndn::Name materialPrefix("/spec175/native/material");
  auto rows = NativeJson::array();
  for (const auto& payload : material->payloads) {
    rows.push_back({{"payloadId", payload.payloadId},
                    {"dataName", ndn::Name(materialPrefix).append(payload.payloadId).toUri()},
                    {"digest", payload.digest}, {"bytes", payload.byteSize()}});
  }
  const auto receipt = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-canonical-material-receipt-v1"},
    {"graphDigest", assembly.graphDigest}, {"sourceDigest", material->sourceDigest},
    {"materialIdentityDigest", material->manifestDigest},
    {"materialManifestDigest", digest(manifest)}, {"materialObjects", std::move(rows)}});
  const auto root = nativeCanonicalJson(NativeJson{
    {"artifactProfileDigest", profile}, {"modelIdentityDigest", zeroDigest('a')},
    {"modelName", "canonical-call-fixture"}, {"schema", "ndnsf-di-canonical-model-manifest-v1"},
    {"state", "ACTIVE"}, {"metadata", {
      {"canonicalSourceBytes", source.modelBytes.size()},
      {"canonicalSourceDigest", material->sourceDigest},
      {"materialIdentityDigest", material->manifestDigest},
      {"materialManifestDataName", manifestName.toUri()},
      {"materialManifestBytes", manifest.size()}, {"materialManifestDigest", digest(manifest)},
      {"materialReceiptDataName", receiptName.toUri()},
      {"materialReceiptBytes", receipt.size()}, {"materialReceiptDigest", digest(receipt)}}}});
  auto projection = makeProjection(digest(root), profile, assembly.graphDigest,
                                   assembly.initializerDigest);
  auto& role = projection.assembly;
  const std::vector<std::variant<std::int64_t, std::string>> dims{std::int64_t{2}};
  role.roleKind = "COMPONENT_SET";
  role.layerEnd = 0;
  role.nodeIndices = {0, 2};
  role.expectedInputs = {{"X", "float32", dims}};
  role.expectedOutputs = {{"Y", "float32", dims}};
  role.recipeDigest = nativePlanningDigest(canonicalNativeOnnxRecipeJson(role));
  auto& plan = projection.plan;
  plan.modelName = "canonical-call-fixture";
  plan.roles = {role.selectedRole};
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
  plan.stageLayout = {{role.role, zeroDigest('d'), plan.roles}};
  NativeExecutionStepSpec first;
  first.stepId = first.artifactEntry = "add";
  first.operation = "CALL_SUBGRAPH";
  first.unitId = role.selectedRole;
  first.onnxNodeIndices = {0};
  first.inputSlots = {{"X", "float32", dims, profile, "EPOCH_INPUT", {}, {}}};
  first.outputSlots = {{"H", "float32", dims, profile, "LOCAL_STEP", {}, {}}};
  auto second = first;
  second.stepId = second.artifactEntry = "mul";
  second.onnxNodeIndices = {2};
  second.inputSlots = {{"H", "float32", dims, profile, "LOCAL_STEP", "add", "H"}};
  second.outputSlots = {{"Y", "float32", dims, profile, "LOCAL_STEP", {}, {}}};
  plan.steps = {first, second};

  std::vector<ndn::Name> reads;
  const auto buffer = [](const std::string& bytes) {
    return ndn::Buffer(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
  };
  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [&](const ndn::Name& name) -> std::optional<ndn::Buffer> {
    return name == rootName ? std::optional(buffer(root)) : std::nullopt;
  };
  fetchers.fetchEncryptedLargeData = [&](const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (service.toUri() != plan.serviceName) return std::nullopt;
    reads.push_back(name);
    if (name == manifestName) return buffer(manifest);
    if (name == receiptName) return buffer(receipt);
    for (const auto& payload : material->payloads) {
      if (name != ndn::Name(materialPrefix).append(payload.payloadId)) continue;
      const auto bytes = payload.copyBytes();
      return ndn::Buffer(bytes.data(), bytes.size());
    }
    return std::nullopt;
  };
  auto scratch = ndnsf::test::makeTestScratchTemplate("ndnsf-canonical-call-assembly");
  BOOST_REQUIRE(::mkdtemp(scratch.data()) != nullptr);
  const std::filesystem::path dir(scratch.data());
  BOOST_SCOPE_EXIT_ALL(&dir) {
    std::error_code error;
    std::filesystem::remove_all(dir, error);
  };
  NativeCanonicalOnnxAssemblerOptions options;
  options.cacheDir = dir.string();
  options.providerIdentity = projection.provider;
  options.workerLocation = testWorkerLocation();
  options.verifyCachedArtifactDigest = true;
  options.signManifest = [] (const auto& manifest) { return digest(manifest); };

  const auto prepared = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  const auto coldReads = reads;
  BOOST_CHECK_EQUAL(coldReads.size(), 7U);
  for (const auto& ref : material->references) {
    if (ref.logicalName != "foreign" && ref.logicalName != "node/1") continue;
    BOOST_CHECK(std::find(reads.begin(), reads.end(),
                         ndn::Name(materialPrefix).append(ref.payloadId)) == reads.end());
    for (const auto& chunk : ref.chunkPayloadIds)
      BOOST_CHECK(std::find(reads.begin(), reads.end(),
                           ndn::Name(materialPrefix).append(chunk)) == reads.end());
  }
  validateNativeOnnxEntryTable(projection, prepared);
  const auto table = readNativeOnnxEntryTable(prepared);
  BOOST_REQUIRE_EQUAL(table.size(), 2U);
  const auto parent = std::filesystem::path(prepared.path).parent_path();
  const auto localWeights = readBytes(parent / "model.onnx.data");
  BOOST_REQUIRE_EQUAL(localWeights.size(), 2 * sizeof(float));
  BOOST_CHECK(std::memcmp(localWeights.data(), weights.data(), localWeights.size()) == 0);
  auto charged = std::filesystem::file_size(prepared.path) + localWeights.size();
  for (const auto& entry : table) {
    const auto bytes = readBytes(parent / entry.relativeGraphPath);
    BOOST_CHECK_EQUAL(digest(bytes), entry.graphDigest);
    charged += bytes.size();
    onnx::ModelProto child;
    BOOST_REQUIRE(child.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
    BOOST_REQUIRE_EQUAL(child.graph().node_size(), 1);
    BOOST_CHECK_EQUAL(child.graph().node(0).output(0), entry.outputNames.front());
  }
  BOOST_CHECK_EQUAL(validateNativeAssembledArtifactFiles(prepared, dir, role.maxAssembledBytes,
                                                        true), charged);
  const auto warm = tryLoadNativeCanonicalOnnxRoleFromCache(fetchers, projection, options, {}, {});
  BOOST_REQUIRE(warm);
  BOOST_CHECK(reads == coldReads);
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  const auto execute = [&](const NativeModelRunnerSpec& spec) {
    const auto runner = factory.create(spec);
    const std::array<float, 2> input{2.0f, 3.0f};
    NamedTensor value;
    value.name = "X";
    value.elementType = TensorElementType::Float32;
    value.shape = {2};
    value.payload.resize(sizeof(input));
    std::memcpy(value.payload.data(), input.data(), sizeof(input));
    RoleExecutionContext ctx;
    ctx.sessionId = "canonical-call-fixture";
    ctx.requestId = projection.requestId;
    ctx.attemptEpoch = projection.attempt;
    ctx.role = role.selectedRole;
    ctx.roleSteps = std::make_shared<const std::vector<NativeExecutionStepSpec>>(plan.steps);
    ctx.activeStep = std::shared_ptr<const NativeExecutionStepSpec>(ctx.roleSteps,
                                                                   &ctx.roleSteps->at(0));
    ctx.stepInputsBySlot.emplace("X", makeEncodedTensorBundle("X", {std::move(value)}));
    const auto intermediate = runner->run(ctx);
    ctx.activeStep = std::shared_ptr<const NativeExecutionStepSpec>(ctx.roleSteps,
                                                                   &ctx.roleSteps->at(1));
    ctx.stepInputsBySlot.clear();
    ctx.stepInputsBySlot.emplace("H", intermediate.at("H"));
    const auto outputs = runner->run(ctx);
    const auto result = decodeTensorBundleViews(outputs.at("Y"));
    BOOST_REQUIRE_EQUAL(result.size(), 1U);
    BOOST_CHECK(result.front().elementType == TensorElementType::Float32);
    BOOST_CHECK(result.front().shape == std::vector<std::int64_t>{2});
    const std::array<float, 2> expected{8.0f, 18.0f};
    BOOST_REQUIRE_EQUAL(result.front().payload.size(), sizeof(expected));
    BOOST_CHECK(std::memcmp(result.front().payload.data(), expected.data(), sizeof(expected)) == 0);
  };
  execute(prepared);
  execute(*warm);
  BOOST_CHECK_EQUAL(warm->metadata.at(NATIVE_ONNX_ENTRY_TABLE),
                    prepared.metadata.at(NATIVE_ONNX_ENTRY_TABLE));
  {
    std::ofstream changed(parent / table.back().relativeGraphPath,
                          std::ios::binary | std::ios::app);
    changed.put('\0');
    BOOST_REQUIRE(changed.good());
  }
  BOOST_CHECK(!tryLoadNativeCanonicalOnnxRoleFromCache(fetchers, projection, options, {}, {}));
}

BOOST_AUTO_TEST_CASE(SyntheticWanSizedEntryRunsWithExternalWeights)
{
  const auto* probe = std::getenv("NDNSF_DI_WAN_ENTRY_MEMORY_PROBE");
  if (!probe) {
    BOOST_TEST_MESSAGE("Set NDNSF_DI_WAN_ENTRY_MEMORY_PROBE to 1 or 2 to measure 314 MB entries");
    return;
  }
  const std::string_view requested(probe);
  BOOST_REQUIRE(requested == "1" || requested == "2");
  const std::size_t entryCount = requested == "2" ? 2 : 1;

  // Match the maximum selected DiT entry's parameter bytes with a synthetic
  // MatMul and bias. Only one token is evaluated by this residency probe.
  constexpr std::int64_t INPUT = 5120;
  constexpr std::int64_t OUTPUT = 15360;
  constexpr std::uint64_t WEIGHT_BYTES = INPUT * OUTPUT * sizeof(float);
  constexpr std::uint64_t BIAS_BYTES = OUTPUT * sizeof(float);
  constexpr char SIDECAR[] = "model.onnx.data";
  auto scratch = ndnsf::test::makeTestScratchTemplate("ndnsf-wan-entry-memory");
  BOOST_REQUIRE(::mkdtemp(scratch.data()));
  const std::filesystem::path dir(scratch.data());
  BOOST_SCOPE_EXIT_ALL(&dir) {
    std::error_code error;
    std::filesystem::remove_all(dir, error);
  };
  const auto writeValue = [] (std::ofstream& out, std::uint64_t element, float value) {
    out.seekp(static_cast<std::streamoff>(element * sizeof(float)));
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
  };
  {
    std::ofstream out(dir / SIDECAR, std::ios::binary);
    writeValue(out, 0, 2.0f);
    writeValue(out, OUTPUT + 1, 3.0f);
    writeValue(out, INPUT * OUTPUT - 1, 5.0f);
    writeValue(out, INPUT * OUTPUT + 2, 4.0f);
    writeValue(out, INPUT * OUTPUT + OUTPUT - 1, 1.0f);
    out.close();
    BOOST_REQUIRE(out.good());
  }
  BOOST_REQUIRE_EQUAL(std::filesystem::file_size(dir / SIDECAR), WEIGHT_BYTES + BIAS_BYTES);

  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(17);
  auto& graph = *model.mutable_graph();
  const auto declarePort = [] (onnx::ValueInfoProto& port, const char* name, std::int64_t width) {
    port.set_name(name);
    auto& tensor = *port.mutable_type()->mutable_tensor_type();
    tensor.set_elem_type(onnx::TensorProto::FLOAT);
    tensor.mutable_shape()->add_dim()->set_dim_value(1);
    tensor.mutable_shape()->add_dim()->set_dim_value(width);
  };
  declarePort(*graph.add_input(), "X", INPUT);
  declarePort(*graph.add_output(), "Y", OUTPUT);
  const auto weight = [&](const char* name, std::initializer_list<std::int64_t> shape,
                          std::uint64_t offset, std::uint64_t bytes) {
    auto& tensor = *graph.add_initializer();
    tensor.set_name(name);
    tensor.set_data_type(onnx::TensorProto::FLOAT);
    for (auto dim : shape) tensor.add_dims(dim);
    tensor.set_data_location(onnx::TensorProto::EXTERNAL);
    for (const auto& [key, value] : std::map<std::string, std::string>{
           {"location", SIDECAR}, {"offset", std::to_string(offset)},
           {"length", std::to_string(bytes)}}) {
      auto& field = *tensor.add_external_data();
      field.set_key(key);
      field.set_value(value);
    }
  };
  weight("W", {INPUT, OUTPUT}, 0, WEIGHT_BYTES);
  weight("B", {OUTPUT}, WEIGHT_BYTES, BIAS_BYTES);
  const auto addNode = [&](const char* op, const char* input, const char* weight,
                           const char* output) {
    auto& node = *graph.add_node();
    node.set_op_type(op);
    node.add_input(input);
    node.add_input(weight);
    node.add_output(output);
  };
  addNode("MatMul", "X", "W", "H");
  addNode("Add", "H", "B", "Y");
  NativeModelRunnerSpec spec;
  spec.role = "/fixture/wan/rank";
  spec.backend = "onnxruntime";
  spec.path = (dir / "model.onnx").string();
  auto table = NativeJson::array();
  auto steps = std::make_shared<std::vector<NativeExecutionStepSpec>>();
  for (std::size_t index = 0; index < entryCount; ++index) {
    const auto entryId = "projection-" + std::to_string(index);
    graph.set_name(entryId);
    const auto wire = model.SerializeAsString();
    const auto graphDigest = digest(wire);
    const auto entryFile = "entry-" + graphDigest.substr(7) + ".onnx";
    std::ofstream out(dir / entryFile, std::ios::binary);
    out.write(wire.data(), static_cast<std::streamsize>(wire.size()));
    out.close();
    BOOST_REQUIRE(out.good());
    table.push_back({{"id", entryId}, {"file", entryFile}, {"digest", graphDigest},
                     {"inputs", {"X"}}, {"outputs", {"Y"}}});
    auto& step = steps->emplace_back();
    step.unitId = spec.role;
    step.stepId = step.artifactEntry = entryId;
    step.operation = "CALL_SUBGRAPH";
    step.inputSlots = {{"X", "float32", {std::int64_t{1}, INPUT}, graphDigest,
                        "EPOCH_INPUT", {}, {}}};
    step.outputSlots = {{"Y", "float32", {std::int64_t{1}, OUTPUT}, graphDigest,
                         "LOCAL_STEP", {}, {}}};
  }
  spec.metadata[NATIVE_ONNX_ENTRY_TABLE] = nativeCanonicalJson(table);

  const auto memory = [](const char* phase) {
    std::ifstream status("/proc/self/status");
    BOOST_REQUIRE(status.good());
    unsigned fields = 0;
    for (std::string line; std::getline(status, line);) {
      if (line.rfind("VmRSS:", 0) == 0 || line.rfind("RssAnon:", 0) == 0 ||
          line.rfind("VmHWM:", 0) == 0) {
        BOOST_TEST_MESSAGE("Wan entry " << phase << " " << line);
        ++fields;
      }
    }
    BOOST_REQUIRE_EQUAL(fields, 3U);
  };
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  memory("before-create");
  auto runner = factory.create(spec);
  memory("after-create");
  RoleExecutionContext ctx;
  ctx.sessionId = "synthetic-wan-sized-entry";
  ctx.role = spec.role;
  ctx.roleSteps = steps;
  std::vector<float> input(INPUT, 0.0f), expected(OUTPUT, 0.0f);
  input[0] = 3.0f;
  input[1] = -1.0f;
  input.back() = 2.0f;
  expected[0] = 6.0f;
  expected[1] = -3.0f;
  expected[2] = 4.0f;
  expected.back() = 11.0f;
  NamedTensor tensor;
  tensor.name = "X";
  tensor.elementType = TensorElementType::Float32;
  tensor.shape = {1, INPUT};
  tensor.payload.resize(input.size() * sizeof(float));
  std::memcpy(tensor.payload.data(), input.data(), tensor.payload.size());
  ctx.stepInputsBySlot.emplace("X", makeEncodedTensorBundle("X", {std::move(tensor)}));
  for (const auto* phase : {"first-run", "repeat-run"}) {
    for (const auto& step : *ctx.roleSteps) {
      ctx.activeStep = std::shared_ptr<const NativeExecutionStepSpec>(ctx.roleSteps, &step);
      const auto output = runner->run(ctx);
      const auto result = decodeTensorBundleViews(output.at("Y"));
      BOOST_REQUIRE_EQUAL(result.size(), 1U);
      BOOST_CHECK(result.front().elementType == TensorElementType::Float32);
      BOOST_CHECK(result.front().shape == (std::vector<std::int64_t>{1, OUTPUT}));
      BOOST_REQUIRE_EQUAL(result.front().payload.size(), expected.size() * sizeof(float));
      BOOST_CHECK(std::memcmp(result.front().payload.data(), expected.data(),
                             result.front().payload.size()) == 0);
    }
    memory(phase);
  }
  runner.reset();
  memory("after-release");
}

BOOST_AUTO_TEST_CASE(AssignmentBoundRootSourceAndCachePath)
{
  const auto fixture = findFixture();
  BOOST_REQUIRE(!fixture.empty());
  const auto source = readBytes(fixture);
  BOOST_REQUIRE(!source.empty());
  const auto sourceDigest = digest(source);
  const auto profileDigest = zeroDigest('b');
  const auto graphDigest =
    "sha256:39bea16fd2b8d6163cda5e9c3875a05a83936fc163dff2c0406b92c22364103d";
  const auto initializerDigest =
    "sha256:074d3acd4acd13d94c4d27e9a201255ed9fd72f1783e4a056b7c6509deb6be9b";
  const auto rootJson = std::string(
    "{\"artifactProfileDigest\":\"") + profileDigest +
    "\",\"metadata\":{\"canonicalSourceBytes\":" +
    std::to_string(source.size()) +
    ",\"canonicalSourceDataName\":\"/spec175/native/source\","
    "\"canonicalSourceDigest\":\"" + sourceDigest +
    "\"},\"modelIdentityDigest\":\"" + zeroDigest('a') +
    "\",\"modelName\":\"spec175-tiny-causal-lm-v1\","
    "\"schema\":\"ndnsf-di-canonical-model-manifest-v1\","
    "\"state\":\"ACTIVE\"}";
  const auto rootPayload = std::vector<std::uint8_t>(rootJson.begin(), rootJson.end());
  const auto rootDigest = digest(rootPayload);
  auto projection = makeProjection(
    rootDigest, profileDigest, graphDigest, initializerDigest);

  ScopedEnv pythonPath(
    "PYTHONPATH", "NDNSF-DistributedInference:NDNSF-DistributedRepo/pythonWrapper");
  const auto cacheDir = std::filesystem::temp_directory_path() /
    "spec175-native-assembly-fixture";
  std::error_code cleanupError;
  std::filesystem::remove_all(cacheDir, cleanupError);

  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [rootPayload] (const ndn::Name& name)
    -> std::optional<ndn::Buffer> {
    if (name == ndn::Name("/spec175/native/root")) {
      return ndn::Buffer(rootPayload.data(), rootPayload.size());
    }
    return std::nullopt;
  };
  fetchers.fetchEncryptedLargeData = [source] (
      const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (name == ndn::Name("/spec175/native/source") &&
        service == ndn::Name("/LLM/Qwen")) {
      return ndn::Buffer(source.data(), source.size());
    }
    return std::nullopt;
  };

  NativeCanonicalOnnxAssemblerOptions options;
  options.workerLocation = testWorkerLocation();
  options.cacheDir = cacheDir.string();
  options.providerIdentity = "/provider/p0";
  options.signManifest = [] (const std::string&) {
    return std::string("fixture-signature-v1");
  };
  const std::string coordinate = "/pp/stage/0/layers/0/2/entry/region/profile/whole";
  projection.assembly.artifactCoordinate = coordinate;
  projection.selectedRole.artifactCoordinate = coordinate;
  std::vector<std::string> progressPhases;
  bool progressRangeValid = true;
  options.reportProgress = [&progressPhases, &progressRangeValid](
      const std::string& phase, double progress) {
    progressRangeValid = progressRangeValid && progress >= 0.0 && progress <= 1.0;
    progressPhases.push_back(phase);
  };

  const auto first = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  BOOST_REQUIRE(std::filesystem::is_regular_file(first.path));
  BOOST_CHECK_EQUAL(first.metadata.at("assembledFrom"),
                    "canonical-root-post-selection");
  BOOST_CHECK_EQUAL(first.metadata.at("modelManifestDigest"), rootDigest);
  BOOST_CHECK_EQUAL(first.metadata.at("artifactCoordinate"), coordinate);
  const auto second = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  BOOST_CHECK_EQUAL(second.path, first.path);
  BOOST_CHECK_EQUAL(second.metadata.at("artifactCoordinate"), coordinate);
  std::ifstream manifestInput(std::filesystem::path(first.path).parent_path() / "manifest.json");
  BOOST_REQUIRE(manifestInput);
  BOOST_CHECK_EQUAL(NativeJson::parse(manifestInput).at("artifactCoordinate").get<std::string>(),
                    coordinate);
  BOOST_CHECK(std::filesystem::is_regular_file(second.path));
  BOOST_REQUIRE(!progressPhases.empty());
  BOOST_CHECK(progressRangeValid);
  BOOST_CHECK(std::find(progressPhases.begin(), progressPhases.end(),
                        "ASSEMBLY_STARTED") != progressPhases.end());
  BOOST_CHECK(std::find(progressPhases.begin(), progressPhases.end(),
                        "ROOT_VERIFIED") != progressPhases.end());
  BOOST_CHECK(std::find(progressPhases.begin(), progressPhases.end(),
                        "SOURCE_VERIFIED") != progressPhases.end());
  BOOST_CHECK(std::find(progressPhases.begin(), progressPhases.end(),
                        "WORKER_ASSEMBLY_VERIFIED") != progressPhases.end());

  auto tamperedFetchers = fetchers;
  tamperedFetchers.fetchEncryptedLargeData = [source] (
      const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (name != ndn::Name("/spec175/native/source") ||
        service != ndn::Name("/LLM/Qwen")) {
      return std::nullopt;
    }
    auto mutated = source;
    mutated.front() ^= 0x01;
    return ndn::Buffer(mutated.data(), mutated.size());
  };
  BOOST_CHECK_THROW(
    prepareNativeCanonicalOnnxRole(tamperedFetchers, projection, options),
    std::runtime_error);

  auto unsignedOptions = options;
  unsignedOptions.signManifest = {};
  BOOST_CHECK_THROW(
    prepareNativeCanonicalOnnxRole(fetchers, projection, unsignedOptions),
    std::runtime_error);

  auto missingRootFetchers = fetchers;
  missingRootFetchers.getArtifact = [] (const ndn::Name&)
    -> std::optional<ndn::Buffer> {
    return std::nullopt;
  };
  BOOST_CHECK_THROW(
    prepareNativeCanonicalOnnxRole(missingRootFetchers, projection, options),
    std::runtime_error);

  auto mutatedProjection = projection;
  mutatedProjection.assembly.graphDigest = zeroDigest('f');
  BOOST_CHECK_THROW(
    prepareNativeCanonicalOnnxRole(fetchers, mutatedProjection, options),
    std::runtime_error);

  mutatedProjection = projection;
  mutatedProjection.assembly.canonicalInitializerDigest = zeroDigest('f');
  BOOST_CHECK_THROW(
    prepareNativeCanonicalOnnxRole(fetchers, mutatedProjection, options),
    std::runtime_error);

  mutatedProjection = projection;
  mutatedProjection.assembly.recipeDigest = zeroDigest('f');
  BOOST_CHECK_THROW(
    prepareNativeCanonicalOnnxRole(fetchers, mutatedProjection, options),
    std::runtime_error);

  auto cachedModel = readBytes(first.path);
  BOOST_REQUIRE(!cachedModel.empty());
  cachedModel.front() ^= 0x01;
  {
    std::ofstream output(first.path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(cachedModel.data()),
                 static_cast<std::streamsize>(cachedModel.size()));
  }
  BOOST_CHECK_THROW(
    prepareNativeCanonicalOnnxRole(fetchers, projection, options),
    std::runtime_error);
  std::filesystem::remove_all(cacheDir, cleanupError);
}

BOOST_AUTO_TEST_CASE(CollaborationContextBindsAssignmentRootBeforeSourceFetch)
{
  // This is intentionally a no-network boundary check.  The assignment
  // payload is supplied through CollaborationContext, but its ACTIVE root
  // omits the canonical source name.  The context overload must therefore
  // reach the context-owned root first and fail at source-name validation; a
  // detached or fake fetcher would report root-unavailable instead.
  fixture::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/LLM/Qwen");
  fixture::NdnsfIntegrationEnvironment environment(profile);

  const auto profileDigest = zeroDigest('b');
  const auto rootJson = std::string(
    "{\"artifactProfileDigest\":\"") + profileDigest +
    "\",\"metadata\":{\"canonicalSourceDigest\":\"" + zeroDigest('c') +
    "\",\"canonicalSourceBytes\":1},"
    "\"modelIdentityDigest\":\"" + zeroDigest('a') +
    "\",\"modelName\":\"spec175-tiny-causal-lm-v1\","
    "\"schema\":\"ndnsf-di-canonical-model-manifest-v1\","
    "\"state\":\"ACTIVE\"}";
  const auto rootBytes = std::vector<std::uint8_t>(rootJson.begin(), rootJson.end());
  const auto projection = makeProjection(
    digest(rootBytes), profileDigest, zeroDigest('d'), zeroDigest('e'));

  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = projection.assembly.role;
  assignment.service = projection.plan.serviceName;
  assignment.assignedArtifact = ndn::Name(projection.canonicalArtifactName);
  assignment.artifactPayload = ndn::Buffer(rootBytes.data(), rootBytes.size());
  RequestMessage request;
  ServiceProvider::CollaborationContext context(
    environment.provider(), environment.user().getName(), projection.requestId,
    request, assignment);
  BOOST_REQUIRE(context.fetchArtifact(assignment.assignedArtifact, 1));

  NativeCanonicalOnnxAssemblerOptions options;
  options.workerLocation = testWorkerLocation();
  options.cacheDir = (std::filesystem::temp_directory_path() /
                      "spec175-native-context-wiring").string();
  options.providerIdentity = environment.provider().getName().toUri();
  options.signManifest = [] (const std::string&) {
    return std::string("fixture-signature-v1");
  };

  try {
    prepareNativeCanonicalOnnxRole(context, projection, options);
    BOOST_FAIL("missing canonical source metadata was accepted");
  }
  catch (const std::runtime_error& error) {
    BOOST_CHECK_EQUAL(error.what(),
                      std::string("DI_CANONICAL_SOURCE_NAME_MISSING"));
  }
}

BOOST_AUTO_TEST_CASE(Spec189MaterialConsumerBoundsSelectedPayloadFetches)
{
  const auto fixture = findFixture();
  BOOST_REQUIRE(!fixture.empty());
  const auto source = readBytes(fixture);
  BOOST_REQUIRE(!source.empty());
  const auto sourceDigest = digest(source);
  NativeCanonicalSource materialSource;
  materialSource.modelBytes = source;
  NativeAssemblyControl manifestControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {},
    1U << 20, 1U << 20};
  materialSource.materialManifest = deriveNativeCanonicalMaterialManifest(
    materialSource, manifestControl);
  const auto identity = canonicalOnnxSourceIdentity(materialSource, manifestControl);
  const auto manifestText = materialSource.materialManifest->canonicalJson();
  const std::vector<std::uint8_t> manifestBytes(manifestText.begin(), manifestText.end());
  const auto profileDigest = zeroDigest('b');
  const ndn::Name rootName("/spec189/material/root");
  const ndn::Name manifestName("/spec189/material/manifest");
  const ndn::Name receiptName("/spec189/material/receipt");
  const ndn::Name sourceName("/spec189/material/source");

  std::map<std::string, std::vector<std::uint8_t>> payloads;
  std::map<std::string, std::string> payloadNames;
  for (const auto& payload : materialSource.materialManifest->payloads) {
    payloads.emplace(payload.payloadId, payload.copyBytes());
    payloadNames.emplace(payload.payloadId,
      "/spec189/material/payload/" + payload.payloadId);
  }
  std::ostringstream materialObjects;
  materialObjects << '[';
  for (std::size_t index = 0; index < materialSource.materialManifest->payloads.size(); ++index) {
    if (index != 0) materialObjects << ',';
    const auto& payload = materialSource.materialManifest->payloads[index];
    materialObjects << "{\"bytes\":" << payload.byteSize()
                    << ",\"dataName\":" << jsonQuote(payloadNames.at(payload.payloadId))
                    << ",\"digest\":" << jsonQuote(payload.digest)
                    << ",\"payloadId\":" << jsonQuote(payload.payloadId) << '}';
  }
  materialObjects << ']';
  const auto receiptText = std::string("{\"graphDigest\":") +
    jsonQuote(materialSource.materialManifest->graphDigest) +
    ",\"materialIdentityDigest\":" +
    jsonQuote(materialSource.materialManifest->manifestDigest) +
    ",\"materialManifestDigest\":" + jsonQuote(digest(manifestBytes)) +
    ",\"materialObjects\":" + materialObjects.str() +
    ",\"schema\":\"ndnsf-di-canonical-material-receipt-v1\",\"sourceDigest\":" +
    jsonQuote(materialSource.materialManifest->sourceDigest) + '}';
  const std::vector<std::uint8_t> receiptBytes(receiptText.begin(), receiptText.end());
  std::ostringstream metadata;
  metadata << "{\"canonicalSourceBytes\":" << source.size()
           << ",\"canonicalSourceDataName\":" << jsonQuote(sourceName.toUri())
           << ",\"canonicalSourceDigest\":" << jsonQuote(sourceDigest)
           << ",\"materialIdentityDigest\":"
           << jsonQuote(materialSource.materialManifest->manifestDigest)
           << ",\"materialManifestBytes\":" << manifestBytes.size()
           << ",\"materialManifestDataName\":" << jsonQuote(manifestName.toUri())
           << ",\"materialManifestDigest\":" << jsonQuote(digest(manifestBytes))
           << ",\"materialReceiptBytes\":" << receiptBytes.size()
           << ",\"materialReceiptDataName\":" << jsonQuote(receiptName.toUri())
           << ",\"materialReceiptDigest\":" << jsonQuote(digest(receiptBytes))
           << '}';
  const auto rootText = std::string("{\"artifactProfileDigest\":") +
    jsonQuote(profileDigest) + ",\"metadata\":" + metadata.str() +
    ",\"modelIdentityDigest\":" + jsonQuote(zeroDigest('a')) +
    ",\"modelName\":\"spec175-tiny-causal-lm-v1\","
    "\"schema\":\"ndnsf-di-canonical-model-manifest-v1\",\"state\":\"ACTIVE\"}";
  const std::vector<std::uint8_t> rootBytes(rootText.begin(), rootText.end());
  auto projection = makeProjection(digest(rootBytes), profileDigest,
                                   identity.graphDigest, identity.initializerDigest);
  projection.canonicalArtifactName = rootName.toUri();
  projection.assembly.canonicalInitializerDigest = identity.initializerDigest;
  projection.assembly.recipeDigest = recipeDigestFor(projection.assembly);

  auto fetches = std::make_shared<std::vector<std::string>>();
  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [rootName, rootBytes] (const ndn::Name& name)
    -> std::optional<ndn::Buffer> {
    if (name != rootName) return std::nullopt;
    return ndn::Buffer(rootBytes.data(), rootBytes.size());
  };
  fetchers.fetchEncryptedLargeData = [=] (const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (service != ndn::Name("/LLM/Qwen")) return std::nullopt;
    fetches->push_back(name.toUri());
    if (name == manifestName)
      return ndn::Buffer(manifestBytes.data(), manifestBytes.size());
    if (name == receiptName)
      return ndn::Buffer(receiptBytes.data(), receiptBytes.size());
    for (const auto& [payloadId, payloadName] : payloadNames) {
      if (name == ndn::Name(payloadName)) {
        const auto& bytes = payloads.at(payloadId);
        return ndn::Buffer(bytes.data(), bytes.size());
      }
    }
    // A material consumer must never fall back to the complete canonical source.
    return std::nullopt;
  };
  NativeCanonicalOnnxAssemblerOptions options;
  options.workerLocation = testWorkerLocation();
  options.cacheDir = (std::filesystem::temp_directory_path() /
                      "spec189-material-consumer-positive").string();
  options.providerIdentity = projection.provider;
  options.signManifest = [] (const std::string& bytes) {
    return std::string("spec189-material-signature-") + digest(bytes);
  };
  std::error_code cleanupError;
  std::filesystem::remove_all(options.cacheDir, cleanupError);

  const auto prepared = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  BOOST_REQUIRE(std::filesystem::is_regular_file(prepared.path));
  BOOST_CHECK(std::find(fetches->begin(), fetches->end(), sourceName.toUri()) == fetches->end());
  BOOST_CHECK_EQUAL(prepared.metadata.at("assembledFrom"), "canonical-root-post-selection");
  BOOST_CHECK_EQUAL(prepared.metadata.at("modelManifestDigest"), projection.assembly.modelManifestDigest);
  std::filesystem::remove_all(options.cacheDir, cleanupError);

  std::set<std::string> selectedIds{materialSource.materialManifest->templatePayloadId};
  std::set<std::string> dependencies;
  for (const auto nodeIndex : projection.assembly.nodeIndices) {
    const auto logicalName = "node/" + std::to_string(nodeIndex);
    for (const auto& reference : materialSource.materialManifest->references) {
      if (reference.kind == "graph-node" && reference.logicalName == logicalName) {
        selectedIds.insert(reference.payloadId);
        dependencies.insert(reference.dependencies.begin(), reference.dependencies.end());
      }
    }
  }
  for (const auto& dependency : dependencies) {
    for (const auto& reference : materialSource.materialManifest->references) {
      if (reference.kind == "shared-initializer" && reference.logicalName == dependency)
        selectedIds.insert(reference.payloadId);
    }
  }
  BOOST_REQUIRE(selectedIds.size() > 1);
  const auto first = selectedIds.begin();
  const auto second = std::next(first);
  const auto firstPayload = payloads.at(*first).size();
  const auto parseReservation = 8U * manifestBytes.size() + 16U * receiptBytes.size();
  projection.assembly.maxAssembledBytes = parseReservation + firstPayload;
  projection.assembly.recipeDigest = recipeDigestFor(projection.assembly);
  fetches->clear();
  options.cacheDir = (std::filesystem::temp_directory_path() /
                      "spec189-material-consumer-budget").string();
  std::filesystem::remove_all(options.cacheDir, cleanupError);
  BOOST_CHECK_EXCEPTION(
    prepareNativeCanonicalOnnxRole(fetchers, projection, options), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("DI_CANONICAL_MATERIAL_BUDGET_EXCEEDED") !=
             std::string::npos;
  });
  BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), manifestName.toUri()), 1U);
  BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), receiptName.toUri()), 1U);
  BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), payloadNames.at(*first)), 1U);
  BOOST_CHECK(std::find(fetches->begin(), fetches->end(), sourceName.toUri()) == fetches->end());
  for (auto it = second; it != selectedIds.end(); ++it) {
    BOOST_CHECK(std::find(fetches->begin(), fetches->end(), payloadNames.at(*it)) ==
                fetches->end());
  }
  std::filesystem::remove_all(options.cacheDir, cleanupError);
}

BOOST_AUTO_TEST_CASE(Spec189MaterialConsumerFetchesOneSelectedBundle)
{
  const auto fixture = findFixture();
  BOOST_REQUIRE(!fixture.empty());
  auto source = readBytes(fixture);
  BOOST_REQUIRE(!source.empty());
  // The role-0 fixture owns all of its 21 production nodes.  Add one selected
  // Identity over an external >1 MiB initializer so the production
  // post-Selection path must fetch the header and every bounded raw chunk.
  // Add one separate unreachable node so the receipt test also has a real
  // unselected payload while the selected role remains checker-valid.
  onnx::ModelProto augmentedModel;
  BOOST_REQUIRE(augmentedModel.ParseFromArray(source.data(),
                                               static_cast<int>(source.size())));
  constexpr std::size_t externalBytes = 2U * 1024U * 1024U + 16U;
  auto* externalInitializer = augmentedModel.mutable_graph()->add_initializer();
  externalInitializer->set_name("spec189_large_weight");
  externalInitializer->set_data_type(onnx::TensorProto::FLOAT16);
  externalInitializer->add_dims(static_cast<std::int64_t>(externalBytes / 2));
  externalInitializer->set_data_location(onnx::TensorProto::EXTERNAL);
  auto* location = externalInitializer->add_external_data();
  location->set_key("location");
  location->set_value("spec189-large.bin");
  auto* offset = externalInitializer->add_external_data();
  offset->set_key("offset");
  offset->set_value("0");
  auto* length = externalInitializer->add_external_data();
  length->set_key("length");
  length->set_value(std::to_string(externalBytes));
  auto* selectedNode = augmentedModel.mutable_graph()->add_node();
  selectedNode->set_name("spec189-selected-large-material");
  selectedNode->set_op_type("Identity");
  selectedNode->add_input("spec189_large_weight");
  selectedNode->add_output("spec189_selected_large_material");
  auto* unselectedNode = augmentedModel.mutable_graph()->add_node();
  unselectedNode->set_name("spec189-unselected-material");
  unselectedNode->set_op_type("Identity");
  auto* unselectedWeight = augmentedModel.mutable_graph()->add_initializer();
  unselectedWeight->set_name("spec220_unselected_weight");
  unselectedWeight->set_data_type(onnx::TensorProto::INT64);
  unselectedWeight->add_dims(2);
  unselectedWeight->add_int64_data(29);
  unselectedWeight->add_int64_data(31);
  unselectedNode->add_input("spec220_unselected_weight");
  unselectedNode->add_output("spec189-unselected-material");
  std::string augmentedWire;
  BOOST_REQUIRE(augmentedModel.SerializeToString(&augmentedWire));
  source.assign(augmentedWire.begin(), augmentedWire.end());
  NativeCanonicalSource materialSource;
  NativeAssemblyControl manifestControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {},
    8U << 20, 8U << 20};
  materialSource.modelBytes = source;
  materialSource.initializerBytes.emplace(externalBytes);
  for (std::size_t index = 0; index < externalBytes; ++index)
    (*materialSource.initializerBytes)[index] = static_cast<std::uint8_t>(index * 13U + 7U);
  materialSource.materialManifest = deriveNativeCanonicalMaterialManifest(
    materialSource, manifestControl);
  const auto identity = canonicalOnnxSourceIdentity(materialSource, manifestControl);
  const auto manifestText = materialSource.materialManifest->canonicalJson();
  const std::vector<std::uint8_t> manifestBytes(manifestText.begin(), manifestText.end());
  const auto profileDigest = zeroDigest('b');
  const ndn::Name rootName("/spec189/material/bundle-root");
  const ndn::Name manifestName("/spec189/material/bundle-manifest");
  const ndn::Name receiptName("/spec189/material/bundle-receipt");
  // Use the fixture's certified role-0 node set.  The selected graph must be
  // independently valid for the production worker's full ONNX checker; a
  // prefix such as nodes {0, 1} is only a transport subset and is not a valid
  // role model because it does not produce the declared role outputs.
  const std::vector<std::uint64_t> certifiedRoleNodes{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21};
  const std::set<std::uint64_t> selectedNodes(
    certifiedRoleNodes.begin(), certifiedRoleNodes.end());
  std::set<std::string> selectedIds{materialSource.materialManifest->templatePayloadId};
  std::set<std::string> dependencies;
  for (const auto nodeIndex : selectedNodes) {
    const auto logicalName = "node/" + std::to_string(nodeIndex);
    for (const auto& reference : materialSource.materialManifest->references) {
      if (reference.kind == "graph-node" && reference.logicalName == logicalName) {
        selectedIds.insert(reference.payloadId);
        dependencies.insert(reference.dependencies.begin(), reference.dependencies.end());
      }
    }
  }
  for (const auto& dependency : dependencies) {
    for (const auto& reference : materialSource.materialManifest->references) {
      if (reference.kind == "shared-initializer" && reference.logicalName == dependency) {
        selectedIds.insert(reference.payloadId);
        selectedIds.insert(reference.chunkPayloadIds.begin(), reference.chunkPayloadIds.end());
      }
    }
  }
  std::map<std::string, std::string> payloadNames;
  for (const auto& payload : materialSource.materialManifest->payloads) {
    payloadNames.emplace(payload.payloadId,
      "/spec189/material/payload/" + payload.payloadId);
  }
  // Independently authenticated bounded serving objects include shared
  // slices and full chunks. Padding must never enter the selected spool.
  std::map<std::string, std::vector<std::uint8_t>> bundles;
  std::map<std::string, std::uint64_t> bundleOffsets;
  std::size_t bundleIndex = 0;
  for (const auto& payload : materialSource.materialManifest->payloads) {
    if (selectedIds.count(payload.payloadId) == 0) continue;
    auto name = "/spec189/material/packed/" + std::to_string(bundleIndex);
    if (!bundles[name].empty() && payload.byteSize() >
        NativeCanonicalMaterialPublicationMaxBytes - bundles[name].size())
      name = "/spec189/material/packed/" + std::to_string(++bundleIndex);
    auto& bundle = bundles[name];
    payloadNames.at(payload.payloadId) = name;
    bundleOffsets[payload.payloadId] = bundle.size();
    const auto bytes = payload.copyBytes();
    bundle.insert(bundle.end(), bytes.begin(), bytes.end());
  }
  BOOST_REQUIRE_EQUAL(bundles.size(), 1U);
  for (auto& item : bundles) {
    if (item.second.size() <= NativeCanonicalMaterialPublicationMaxBytes - 4)
      item.second.insert(item.second.end(), {0xde, 0xad, 0xbe, 0xef});
    BOOST_REQUIRE_GT(item.second.size(), NativeCanonicalMaterialBundleMaxBytes);
    BOOST_REQUIRE_LE(item.second.size(), NativeCanonicalMaterialPublicationMaxBytes);
  }
  std::ostringstream materialObjects;
  materialObjects << '[';
  for (std::size_t index = 0; index < materialSource.materialManifest->payloads.size(); ++index) {
    if (index != 0) materialObjects << ',';
    const auto& payload = materialSource.materialManifest->payloads.at(index);
    materialObjects << "{\"bytes\":" << payload.byteSize()
                    << ",\"dataName\":" << jsonQuote(payloadNames.at(payload.payloadId))
                    << ",\"digest\":" << jsonQuote(payload.digest)
                    << ",\"payloadId\":" << jsonQuote(payload.payloadId);
    if (selectedIds.count(payload.payloadId) != 0) {
      const auto& bundle = bundles.at(payloadNames.at(payload.payloadId));
      materialObjects << ",\"bundleDigest\":" << jsonQuote(digest(bundle))
                      << ",\"bundleBytes\":" << bundle.size()
                      << ",\"bundleOffset\":" << bundleOffsets.at(payload.payloadId);
    }
    materialObjects << '}';
  }
  materialObjects << ']';
  const auto receiptText = std::string("{\"graphDigest\":") +
    jsonQuote(materialSource.materialManifest->graphDigest) +
    ",\"materialIdentityDigest\":" +
    jsonQuote(materialSource.materialManifest->manifestDigest) +
    ",\"materialManifestDigest\":" + jsonQuote(digest(manifestBytes)) +
    ",\"materialObjects\":" + materialObjects.str() +
    ",\"schema\":\"ndnsf-di-canonical-material-receipt-v1\",\"sourceDigest\":" +
    jsonQuote(materialSource.materialManifest->sourceDigest) + '}';
  const std::vector<std::uint8_t> receiptBytes(receiptText.begin(), receiptText.end());
  std::ostringstream metadata;
  metadata << "{\"canonicalSourceBytes\":" << source.size()
           << ",\"canonicalSourceDigest\":" << jsonQuote(
             digest(source))
           << ",\"materialIdentityDigest\":" << jsonQuote(
             materialSource.materialManifest->manifestDigest)
           << ",\"materialManifestBytes\":" << manifestBytes.size()
           << ",\"materialManifestDataName\":" << jsonQuote(manifestName.toUri())
           << ",\"materialManifestDigest\":" << jsonQuote(digest(manifestBytes))
           << ",\"materialReceiptBytes\":" << receiptBytes.size()
           << ",\"materialReceiptDataName\":" << jsonQuote(receiptName.toUri())
           << ",\"materialReceiptDigest\":" << jsonQuote(digest(receiptBytes))
           << '}';
  const auto rootText = std::string("{\"artifactProfileDigest\":") +
    jsonQuote(profileDigest) + ",\"metadata\":" + metadata.str() +
    ",\"modelIdentityDigest\":" + jsonQuote(zeroDigest('a')) +
    ",\"modelName\":\"spec175-tiny-causal-lm-v1\"," +
    "\"schema\":\"ndnsf-di-canonical-model-manifest-v1\",\"state\":\"ACTIVE\"}";
  const std::vector<std::uint8_t> rootBytes(rootText.begin(), rootText.end());
  auto projection = makeProjection(digest(rootBytes), profileDigest,
                                   identity.graphDigest, identity.initializerDigest);
  projection.canonicalArtifactName = rootName.toUri();
  projection.assembly.nodeIndices = certifiedRoleNodes;
  projection.assembly.maxSourceBytes = 8U << 20;
  projection.assembly.canonicalInitializerDigest = identity.initializerDigest;
  std::uint64_t selectedPayloadBytes = 0;
  for (const auto& payload : materialSource.materialManifest->payloads)
    if (selectedIds.count(payload.payloadId) != 0)
    selectedPayloadBytes += payload.byteSize();
  // Mirror the production material-backed peak reservation: the selected
  // payloads stay owned while the chunked initializer is copied into a
  // TensorProto and the final model is serialized.  Compute the model/header
  // terms from the same manifest objects instead of hiding a large safety
  // margin in this selector.
  const auto templatePayload = std::find_if(
    materialSource.materialManifest->payloads.begin(),
    materialSource.materialManifest->payloads.end(),
    [&] (const auto& payload) {
      return payload.payloadId == materialSource.materialManifest->templatePayloadId;
    });
  BOOST_REQUIRE(templatePayload != materialSource.materialManifest->payloads.end());
  onnx::ModelProto assemblyModel;
  BOOST_REQUIRE(assemblyModel.ParseFromArray(templatePayload->bytes.data(),
                                               static_cast<int>(templatePayload->bytes.size())));
  for (const auto nodeIndex : certifiedRoleNodes) {
    const auto nodeReference = std::find_if(
      materialSource.materialManifest->references.begin(),
      materialSource.materialManifest->references.end(),
      [&] (const auto& reference) {
        return reference.kind == "graph-node" &&
          reference.logicalName == "node/" + std::to_string(nodeIndex);
      });
    BOOST_REQUIRE(nodeReference != materialSource.materialManifest->references.end());
    const auto nodePayload = std::find_if(
      materialSource.materialManifest->payloads.begin(),
      materialSource.materialManifest->payloads.end(),
      [&] (const auto& payload) { return payload.payloadId == nodeReference->payloadId; });
    BOOST_REQUIRE(nodePayload != materialSource.materialManifest->payloads.end());
    onnx::NodeProto node;
    BOOST_REQUIRE(node.ParseFromArray(nodePayload->data(),
                                      static_cast<int>(nodePayload->byteSize())));
    *assemblyModel.mutable_graph()->add_node() = std::move(node);
  }
  const auto initializerReference = std::find_if(
    materialSource.materialManifest->references.begin(),
    materialSource.materialManifest->references.end(),
    [] (const auto& reference) {
      return reference.kind == "shared-initializer" &&
        reference.logicalName == "spec189_large_weight";
    });
  BOOST_REQUIRE(initializerReference != materialSource.materialManifest->references.end());
  const auto initializerHeaderPayload = std::find_if(
    materialSource.materialManifest->payloads.begin(),
    materialSource.materialManifest->payloads.end(),
    [&] (const auto& payload) { return payload.payloadId == initializerReference->payloadId; });
  BOOST_REQUIRE(initializerHeaderPayload != materialSource.materialManifest->payloads.end());
  onnx::TensorProto initializerHeader;
  BOOST_REQUIRE(initializerHeader.ParseFromArray(initializerHeaderPayload->bytes.data(),
                                                 static_cast<int>(initializerHeaderPayload->bytes.size())));
  const auto finalModelWithoutRaw = static_cast<std::uint64_t>(assemblyModel.ByteSizeLong()) +
                                    static_cast<std::uint64_t>(initializerHeader.ByteSizeLong());
  const auto finalModelUpper = finalModelWithoutRaw + externalBytes + 128U;
  const auto retainedMaterialBytes = manifestBytes.size() + selectedPayloadBytes;
  const auto materialPeak = retainedMaterialBytes + externalBytes + 2U * finalModelUpper;
  const auto parseReservation = 8U * manifestBytes.size() + 16U * receiptBytes.size();
  const auto fetchBudget = std::max<std::uint64_t>(
    manifestBytes.size() + receiptBytes.size(), parseReservation) + selectedPayloadBytes;
  const auto requiredBudget = std::max<std::uint64_t>(fetchBudget, materialPeak);
  projection.assembly.maxAssembledBytes = requiredBudget + 1024U;
  projection.assembly.recipeDigest = recipeDigestFor(projection.assembly);

  auto fetches = std::make_shared<std::vector<std::string>>();
  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [rootName, rootBytes] (const ndn::Name& name)
    -> std::optional<ndn::Buffer> {
    if (name != rootName) return std::nullopt;
    return ndn::Buffer(rootBytes.data(), rootBytes.size());
  };
  fetchers.fetchEncryptedLargeData = [=] (const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (service != ndn::Name("/LLM/Qwen")) return std::nullopt;
    fetches->push_back(name.toUri());
    if (name == manifestName)
      return ndn::Buffer(manifestBytes.data(), manifestBytes.size());
    if (name == receiptName)
      return ndn::Buffer(receiptBytes.data(), receiptBytes.size());
    const auto bundle = bundles.find(name.toUri());
    if (bundle != bundles.end())
      return ndn::Buffer(bundle->second.data(), bundle->second.size());
    for (const auto& [payloadId, payloadName] : payloadNames) {
      if (name != ndn::Name(payloadName))
        continue;
      const auto payload = std::find_if(
        materialSource.materialManifest->payloads.begin(),
        materialSource.materialManifest->payloads.end(),
        [&] (const auto& item) { return item.payloadId == payloadId; });
      if (payload == materialSource.materialManifest->payloads.end())
        return std::nullopt;
      return ndn::Buffer(payload->data(), payload->byteSize());
    }
    return std::nullopt;
  };
  NativeCanonicalOnnxAssemblerOptions options;
  options.workerLocation = testWorkerLocation();
  options.cacheDir = (std::filesystem::temp_directory_path() /
                      "spec189-material-consumer-bundle").string();
  options.providerIdentity = projection.provider;
  std::uint64_t observedSpoolBytes = 0;
  const auto spoolPaths = [&] (const std::string& cacheDir,
                                const std::string& filename = "material.spool") {
    std::vector<std::filesystem::path> result;
    if (std::filesystem::exists(cacheDir)) {
      for (const auto& item : std::filesystem::recursive_directory_iterator(cacheDir))
        if (item.path().filename() == filename) result.push_back(item.path());
    }
    return result;
  };
  options.reportProgress = [&] (const std::string& phase, double) {
    if (phase != "MATERIAL_PAYLOAD_VERIFIED") return;
    const auto paths = spoolPaths(options.cacheDir);
    BOOST_REQUIRE_EQUAL(paths.size(), 1U);
    observedSpoolBytes = std::max(observedSpoolBytes,
      static_cast<std::uint64_t>(std::filesystem::file_size(paths.front())));
  };
  options.signManifest = [] (const std::string& bytes) {
    return std::string("spec189-material-bundle-signature-") + digest(bytes);
  };
  std::error_code cleanupError;
  std::filesystem::remove_all(options.cacheDir, cleanupError);
  const auto prepared = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  BOOST_REQUIRE(std::filesystem::is_regular_file(prepared.path));
  BOOST_CHECK_EQUAL(observedSpoolBytes, selectedPayloadBytes);
  BOOST_CHECK(spoolPaths(options.cacheDir).empty());
  onnx::ModelProto assembledRole;
  const auto assembledBytes = readBytes(prepared.path);
  BOOST_REQUIRE(assembledRole.ParseFromArray(assembledBytes.data(),
                                            static_cast<int>(assembledBytes.size())));
  const auto weight = std::find_if(assembledRole.graph().initializer().begin(),
    assembledRole.graph().initializer().end(), [] (const auto& initializer) {
      return initializer.name() == "spec189_large_weight";
    });
  BOOST_REQUIRE(weight != assembledRole.graph().initializer().end());
  BOOST_CHECK(std::none_of(assembledRole.graph().initializer().begin(),
    assembledRole.graph().initializer().end(), [] (const auto& initializer) {
      return initializer.name() == "spec220_unselected_weight";
    }));
  BOOST_CHECK(weight->raw_data().empty());
  BOOST_CHECK_EQUAL(weight->data_location(), onnx::TensorProto::EXTERNAL);
  const auto sidecarPath = std::filesystem::path(prepared.path).parent_path() / "model.onnx.data";
  const auto sidecar = readBytes(sidecarPath);
  BOOST_REQUIRE_EQUAL(sidecar.size(), externalBytes);
  BOOST_CHECK(sidecar == materialSource.initializerBytes->asVector());
  BOOST_CHECK_EQUAL(prepared.metadata.at("externalInitializerDigest"), digest(sidecar));
  BOOST_CHECK_EQUAL(validateNativeAssembledArtifactFiles(
    prepared, options.cacheDir, projection.assemblyContract().maxAssembledBytes, true),
    assembledBytes.size() + externalBytes);
  unsigned scanFences = 0;
  validateNativeAssembledArtifactFiles(prepared, options.cacheDir,
    projection.assemblyContract().maxAssembledBytes, true, [&] { ++scanFences; });
  BOOST_REQUIRE(scanFences >= 8U);
  for (unsigned stop = 1; stop <= scanFences; ++stop) {
    unsigned visited = 0;
    BOOST_CHECK_EXCEPTION(validateNativeAssembledArtifactFiles(prepared, options.cacheDir,
      projection.assemblyContract().maxAssembledBytes, true, [&] {
        if (++visited == stop) throw std::runtime_error("fixture file-set scan cancelled");
      }), std::runtime_error, [] (const auto& error) {
        return std::string(error.what()) == "fixture file-set scan cancelled";
      });
    BOOST_CHECK_EQUAL(visited, stop);
    BOOST_CHECK(readBytes(sidecarPath) == sidecar);
  }
  BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), manifestName.toUri()), 1U);
  BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), receiptName.toUri()), 1U);
  for (const auto& payloadId : selectedIds)
    BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), payloadNames.at(payloadId)), 1U);
  for (const auto& payload : materialSource.materialManifest->payloads)
    if (selectedIds.count(payload.payloadId) == 0)
      BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(),
                                   payloadNames.at(payload.payloadId)), 0U);
  for (const auto& bundle : bundles)
    BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), bundle.first), 1U);

  auto strictOptions = options;
  strictOptions.verifyCachedArtifactDigest = true;
  const auto cacheHit = tryLoadNativeCanonicalOnnxRoleFromCache(
    fetchers, projection, strictOptions, {}, {});
  BOOST_REQUIRE(cacheHit);
  BOOST_CHECK_EQUAL(cacheHit->metadata.at("assembledFileSetDigest"),
                    prepared.metadata.at("assembledFileSetDigest"));
  for (int mutation = 0; mutation < 4; ++mutation) {
    auto invalidOptions = strictOptions;
    invalidOptions.cacheDir = options.cacheDir + "-file-pair-invalid";
    std::filesystem::remove_all(invalidOptions.cacheDir, cleanupError);
    std::filesystem::copy(options.cacheDir, invalidOptions.cacheDir,
                          std::filesystem::copy_options::recursive);
    const auto relativeSidecar = std::filesystem::relative(sidecarPath, options.cacheDir);
    const auto invalidSidecar = std::filesystem::path(invalidOptions.cacheDir) / relativeSidecar;
    if (mutation == 0) {
      std::filesystem::remove(invalidSidecar);
    }
    else if (mutation == 1) {
      std::fstream tamper(invalidSidecar, std::ios::in | std::ios::out | std::ios::binary);
      tamper.put(static_cast<char>(sidecar.front() ^ 1));
    }
    else if (mutation == 2) {
      const auto foreign = std::filesystem::path(invalidOptions.cacheDir) / "foreign.bin";
      std::filesystem::copy_file(sidecarPath, foreign);
      std::filesystem::remove(invalidSidecar);
      std::filesystem::create_symlink(foreign, invalidSidecar);
    }
    else {
      std::filesystem::remove(invalidSidecar);
      std::filesystem::create_symlink(sidecarPath, invalidSidecar);
    }
    BOOST_CHECK(!tryLoadNativeCanonicalOnnxRoleFromCache(
      fetchers, projection, invalidOptions, {}, {}));
    BOOST_CHECK(readBytes(sidecarPath) == sidecar);
    std::filesystem::remove_all(invalidOptions.cacheDir, cleanupError);
  }

  auto corruptFetchers = fetchers;
  const auto fetch = fetchers.fetchEncryptedLargeData;
  const auto corruptName = bundles.begin()->first;
  corruptFetchers.fetchEncryptedLargeData = [fetch, corruptName]
    (const ndn::Name& name, const ndn::Name& service) {
      auto bytes = fetch(name, service);
      if (name.toUri() == corruptName && bytes && !bytes->empty()) bytes->front() ^= 1;
      return bytes;
    };
  auto corruptOptions = options;
  corruptOptions.cacheDir = options.cacheDir + "-corrupt";
  corruptOptions.reportProgress = {};
  std::filesystem::remove_all(corruptOptions.cacheDir, cleanupError);
  BOOST_CHECK_EXCEPTION(
    prepareNativeCanonicalOnnxRole(corruptFetchers, projection, corruptOptions),
    std::runtime_error, [] (const auto& error) {
      return std::string(error.what()).find("material-bundle_DIGEST_MISMATCH") !=
             std::string::npos;
    });
  BOOST_CHECK(spoolPaths(corruptOptions.cacheDir).empty());
  std::filesystem::remove_all(corruptOptions.cacheDir, cleanupError);

  bool cancelled = false;
  auto cancelledOptions = options;
  cancelledOptions.cacheDir = options.cacheDir + "-cancelled";
  cancelledOptions.shouldCancel = [&] { return cancelled; };
  cancelledOptions.reportProgress = [&] (const std::string& phase, double) {
    if (phase == "MATERIAL_PAYLOAD_VERIFIED") cancelled = true;
  };
  std::filesystem::remove_all(cancelledOptions.cacheDir, cleanupError);
  BOOST_CHECK_EXCEPTION(
    prepareNativeCanonicalOnnxRole(fetchers, projection, cancelledOptions),
    std::runtime_error, [] (const auto& error) {
      return std::string(error.what()).find("DI_NATIVE_ASSEMBLY_CANCELLED") != std::string::npos;
    });
  BOOST_CHECK(cancelled);
  BOOST_CHECK(spoolPaths(cancelledOptions.cacheDir).empty());
  std::filesystem::remove_all(cancelledOptions.cacheDir, cleanupError);
  bool lateCancelled = false;
  auto lateOptions = options;
  lateOptions.cacheDir = options.cacheDir + "-late-cancelled";
  lateOptions.shouldCancel = [&] { return lateCancelled; };
  lateOptions.reportProgress = [&] (const std::string& phase, double) {
    if (phase == "WORKER_ASSEMBLY_VERIFIED") lateCancelled = true;
  };
  std::filesystem::remove_all(lateOptions.cacheDir, cleanupError);
  BOOST_CHECK_EXCEPTION(prepareNativeCanonicalOnnxRole(fetchers, projection, lateOptions),
    std::runtime_error, [] (const auto& error) {
      return std::string(error.what()).find("DI_NATIVE_ASSEMBLY_CANCELLED") != std::string::npos;
    });
  BOOST_CHECK(lateCancelled);
  BOOST_CHECK(spoolPaths(lateOptions.cacheDir).empty());
  BOOST_CHECK(spoolPaths(lateOptions.cacheDir, "model.onnx.data").empty());
  std::filesystem::remove_all(lateOptions.cacheDir, cleanupError);
  const auto expectReceiptRejected = [&](const NativeJson& receipt, std::string_view code) {
    const auto badReceipt = nativeCanonicalJson(receipt);
    auto root = nativeParseJson(rootText);
    root.at("metadata")["materialReceiptDigest"] = digest(badReceipt);
    root.at("metadata")["materialReceiptBytes"] = badReceipt.size();
    const auto badRoot = nativeCanonicalJson(root);
    auto badProjection = projection;
    badProjection.assembly.modelManifestDigest = digest(badRoot);
    badProjection.assembly.recipeDigest = recipeDigestFor(badProjection.assembly);
    NativeCanonicalOnnxFetchers badFetchers;
    badFetchers.getArtifact = [&](const ndn::Name& name) -> std::optional<ndn::Buffer> {
      if (name != rootName) return std::nullopt;
      return ndn::Buffer(reinterpret_cast<const std::uint8_t*>(badRoot.data()), badRoot.size());
    };
    badFetchers.fetchEncryptedLargeData = [&](const ndn::Name& name, const ndn::Name& service)
      -> std::optional<ndn::Buffer> {
      if (service != ndn::Name("/LLM/Qwen")) return std::nullopt;
      if (name == manifestName)
        return ndn::Buffer(manifestBytes.data(), manifestBytes.size());
      if (name == receiptName)
        return ndn::Buffer(reinterpret_cast<const std::uint8_t*>(badReceipt.data()),
                           badReceipt.size());
      BOOST_ERROR("receipt should be rejected before reading " + name.toUri());
      return std::nullopt;
    };
    auto badOptions = options;
    badOptions.cacheDir = options.cacheDir + "-bad-receipt";
    std::filesystem::remove_all(badOptions.cacheDir, cleanupError);
    BOOST_CHECK_EXCEPTION(
      prepareNativeCanonicalOnnxRole(badFetchers, badProjection, badOptions), std::runtime_error,
      [&](const auto& error) { return std::string_view(error.what()).find(code) !=
                                      std::string_view::npos; });
    BOOST_CHECK(spoolPaths(badOptions.cacheDir).empty());
    std::filesystem::remove_all(badOptions.cacheDir, cleanupError);
  };
  auto badReceipt = nativeParseJson(receiptText);
  badReceipt["graphDigest"] = zeroDigest('c');
  expectReceiptRejected(badReceipt, "DI_CANONICAL_MATERIAL_RECEIPT_IDENTITY_MISMATCH");

  badReceipt = nativeParseJson(receiptText);
  auto& receiptRows = badReceipt.at("materialObjects");
  const auto row = std::find_if(receiptRows.begin(), receiptRows.end(), [&](const auto& item) {
    return item.at("payloadId") == materialSource.materialManifest->templatePayloadId;
  });
  BOOST_REQUIRE(row != receiptRows.end());
  (*row)["digest"] = zeroDigest('d');
  expectReceiptRejected(badReceipt, "DI_CANONICAL_MATERIAL_RECEIPT_MISMATCH");
  auto largeBundleReceipt = NativeJson::parse(receiptText);
  for (auto& object : largeBundleReceipt["materialObjects"]) {
    if (object.contains("bundleBytes"))
      object["bundleBytes"] = NativeCanonicalMaterialPublicationMaxBytes + 1;
  }
  const auto largeBundleText = nativeCanonicalJson(largeBundleReceipt);
  const std::vector<std::uint8_t> largeBundleBytes(largeBundleText.begin(), largeBundleText.end());
  auto largeBundleRoot = NativeJson::parse(rootText);
  largeBundleRoot["metadata"]["materialReceiptBytes"] = largeBundleBytes.size();
  largeBundleRoot["metadata"]["materialReceiptDigest"] = digest(largeBundleBytes);
  const auto largeBundleRootText = nativeCanonicalJson(largeBundleRoot);
  const std::vector<std::uint8_t> largeBundleRootBytes(
    largeBundleRootText.begin(), largeBundleRootText.end());
  auto largeBundleProjection = projection;
  largeBundleProjection.assembly.modelManifestDigest = digest(largeBundleRootBytes);
  largeBundleProjection.assembly.recipeDigest = recipeDigestFor(largeBundleProjection.assembly);
  auto largeBundleFetchers = fetchers;
  largeBundleFetchers.getArtifact = [=] (const ndn::Name& name) -> std::optional<ndn::Buffer> {
    if (name != rootName) return std::nullopt;
    return ndn::Buffer(largeBundleRootBytes.data(), largeBundleRootBytes.size());
  };
  largeBundleFetchers.fetchEncryptedLargeData = [=] (const ndn::Name& name,
                                                   const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (name == receiptName)
      return ndn::Buffer(largeBundleBytes.data(), largeBundleBytes.size());
    return fetchers.fetchEncryptedLargeData(name, service);
  };
  auto largeBundleOptions = options;
  largeBundleOptions.reportProgress = {};
  largeBundleOptions.cacheDir = options.cacheDir + "-oversized-bundle";
  std::filesystem::remove_all(largeBundleOptions.cacheDir, cleanupError);
  BOOST_CHECK_EXCEPTION(
    prepareNativeCanonicalOnnxRole(largeBundleFetchers, largeBundleProjection, largeBundleOptions),
    std::runtime_error, [] (const auto& error) {
      return std::string(error.what()).find("DI_CANONICAL_MATERIAL_BUNDLE_RECEIPT_INVALID") !=
             std::string::npos;
    });
  BOOST_CHECK(spoolPaths(largeBundleOptions.cacheDir).empty());
  std::filesystem::remove_all(largeBundleOptions.cacheDir, cleanupError);
  std::filesystem::remove_all(options.cacheDir, cleanupError);

  projection.assembly.maxAssembledBytes = fetchBudget - 1U;
  projection.assembly.recipeDigest = recipeDigestFor(projection.assembly);
  fetches->clear();
  BOOST_CHECK_EXCEPTION(
    prepareNativeCanonicalOnnxRole(fetchers, projection, options), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("DI_CANONICAL_MATERIAL_BUDGET_EXCEEDED") !=
             std::string::npos;
    });
  BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), manifestName.toUri()), 1U);
  BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), receiptName.toUri()), 1U);
  // All selected slices share one serving object. Its complete size must fit
  // before retrieval; a rejected budget cannot fetch a partial bundle.
  BOOST_CHECK(std::none_of(selectedIds.begin(), selectedIds.end(), [&] (const auto& payloadId) {
    return std::count(fetches->begin(), fetches->end(), payloadNames.at(payloadId)) != 0;
  }));
  for (const auto& payload : materialSource.materialManifest->payloads)
    if (selectedIds.count(payload.payloadId) == 0)
      BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(),
                                   payloadNames.at(payload.payloadId)), 0U);

  // A receipt may contain many unselected records.  Reject its transient
  // parse/index reservation before fetching the manifest or any payload; this
  // keeps the advertised working-set ceiling meaningful during validation.
  auto oversizedReceiptJson = NativeJson::parse(receiptText);
  auto& oversizedObjects = oversizedReceiptJson["materialObjects"];
  for (std::size_t index = 0; index < 256; ++index) {
    oversizedObjects.push_back({
      {"bytes", 1U},
      {"dataName", "/spec189/material/unused/" + std::to_string(index)},
      {"digest", zeroDigest('f')},
      {"payloadId", "unused-receipt-payload-" + std::to_string(index)}});
  }
  oversizedObjects.front()["unexpected"] = NativeJson{{"nested", "value"}};
  const auto oversizedReceiptText = nativeCanonicalJson(oversizedReceiptJson);
  const std::vector<std::uint8_t> oversizedReceiptBytes(
    oversizedReceiptText.begin(), oversizedReceiptText.end());
  auto oversizedRootJson = NativeJson::parse(rootText);
  oversizedRootJson["metadata"]["materialReceiptBytes"] = oversizedReceiptBytes.size();
  oversizedRootJson["metadata"]["materialReceiptDigest"] = digest(oversizedReceiptBytes);
  const auto oversizedRootText = nativeCanonicalJson(oversizedRootJson);
  const std::vector<std::uint8_t> oversizedRootBytes(
    oversizedRootText.begin(), oversizedRootText.end());
  auto oversizedProjection = makeProjection(
    digest(oversizedRootBytes), profileDigest, identity.graphDigest, identity.initializerDigest);
  oversizedProjection.canonicalArtifactName = rootName.toUri();
  oversizedProjection.assembly.nodeIndices = projection.assembly.nodeIndices;
  oversizedProjection.assembly.maxSourceBytes = projection.assembly.maxSourceBytes;
  oversizedProjection.assembly.canonicalInitializerDigest = identity.initializerDigest;
  const auto oversizedParseReservation = 8U * manifestBytes.size() +
    16U * oversizedReceiptBytes.size();
  oversizedProjection.assembly.maxAssembledBytes = oversizedParseReservation - 1U;
  oversizedProjection.assembly.recipeDigest = recipeDigestFor(oversizedProjection.assembly);
  auto oversizedFetches = std::make_shared<std::vector<std::string>>();
  NativeCanonicalOnnxFetchers oversizedFetchers;
  oversizedFetchers.getArtifact = [rootName, oversizedRootBytes] (const ndn::Name& name)
    -> std::optional<ndn::Buffer> {
    if (name != rootName) return std::nullopt;
    return ndn::Buffer(oversizedRootBytes.data(), oversizedRootBytes.size());
  };
  oversizedFetchers.fetchEncryptedLargeData =
    [=] (const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (service != ndn::Name("/LLM/Qwen")) return std::nullopt;
    oversizedFetches->push_back(name.toUri());
    if (name == manifestName)
      return ndn::Buffer(manifestBytes.data(), manifestBytes.size());
    if (name == receiptName)
      return ndn::Buffer(oversizedReceiptBytes.data(), oversizedReceiptBytes.size());
    return std::nullopt;
  };
  auto oversizedOptions = options;
  oversizedOptions.cacheDir = (std::filesystem::temp_directory_path() /
                                "spec189-material-consumer-large-receipt").string();
  std::filesystem::remove_all(oversizedOptions.cacheDir, cleanupError);
  BOOST_CHECK_EXCEPTION(
    prepareNativeCanonicalOnnxRole(oversizedFetchers, oversizedProjection, oversizedOptions),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("DI_CANONICAL_MATERIAL_METADATA_UNAVAILABLE") !=
             std::string::npos;
    });
  BOOST_CHECK(oversizedFetches->empty());
  std::filesystem::remove_all(oversizedOptions.cacheDir, cleanupError);

  auto schemaProjection = oversizedProjection;
  schemaProjection.assembly.maxAssembledBytes = oversizedParseReservation + 1024U;
  schemaProjection.assembly.recipeDigest = recipeDigestFor(schemaProjection.assembly);
  oversizedFetches->clear();
  auto schemaOptions = options;
  schemaOptions.cacheDir = (std::filesystem::temp_directory_path() /
                            "spec189-material-consumer-receipt-schema").string();
  std::filesystem::remove_all(schemaOptions.cacheDir, cleanupError);
  BOOST_CHECK_EXCEPTION(
    prepareNativeCanonicalOnnxRole(oversizedFetchers, schemaProjection, schemaOptions),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("DI_CANONICAL_MATERIAL_RECEIPT_INVALID") !=
             std::string::npos;
    });
  BOOST_CHECK_EQUAL(std::count(oversizedFetches->begin(), oversizedFetches->end(),
                               manifestName.toUri()), 1U);
  BOOST_CHECK_EQUAL(std::count(oversizedFetches->begin(), oversizedFetches->end(),
                               receiptName.toUri()), 1U);
  std::filesystem::remove_all(schemaOptions.cacheDir, cleanupError);

  const auto rootForReceipt = [&] (const std::vector<std::uint8_t>& candidateReceipt) {
    auto json = NativeJson::parse(rootText);
    json["metadata"]["materialReceiptBytes"] = candidateReceipt.size();
    json["metadata"]["materialReceiptDigest"] = digest(candidateReceipt);
    const auto wire = nativeCanonicalJson(json);
    return std::vector<std::uint8_t>(wire.begin(), wire.end());
  };
  const auto runInvalidReceipt = [&] (const std::string& candidateReceiptText,
                                     const std::string& cacheSuffix) {
    const std::vector<std::uint8_t> candidateReceipt(
      candidateReceiptText.begin(), candidateReceiptText.end());
    const auto candidateRoot = rootForReceipt(candidateReceipt);
    auto candidateProjection = makeProjection(
      digest(candidateRoot), profileDigest, identity.graphDigest, identity.initializerDigest);
    candidateProjection.canonicalArtifactName = rootName.toUri();
    candidateProjection.assembly.nodeIndices = projection.assembly.nodeIndices;
    candidateProjection.assembly.maxSourceBytes = projection.assembly.maxSourceBytes;
    candidateProjection.assembly.canonicalInitializerDigest = identity.initializerDigest;
    candidateProjection.assembly.maxAssembledBytes =
      8U * manifestBytes.size() + 16U * candidateReceipt.size() +
      selectedPayloadBytes + 1024U;
    candidateProjection.assembly.recipeDigest = recipeDigestFor(candidateProjection.assembly);
    oversizedFetches->clear();
    NativeCanonicalOnnxFetchers candidateFetchers;
    candidateFetchers.getArtifact = [rootName, candidateRoot] (const ndn::Name& name)
      -> std::optional<ndn::Buffer> {
      if (name != rootName) return std::nullopt;
      return ndn::Buffer(candidateRoot.data(), candidateRoot.size());
    };
    candidateFetchers.fetchEncryptedLargeData =
      [=] (const ndn::Name& name, const ndn::Name& service)
      -> std::optional<ndn::Buffer> {
      if (service != ndn::Name("/LLM/Qwen")) return std::nullopt;
      oversizedFetches->push_back(name.toUri());
      if (name == manifestName)
        return ndn::Buffer(manifestBytes.data(), manifestBytes.size());
      if (name == receiptName)
        return ndn::Buffer(candidateReceipt.data(), candidateReceipt.size());
      return std::nullopt;
    };
    auto candidateOptions = options;
    candidateOptions.cacheDir = (std::filesystem::temp_directory_path() /
                                 ("spec189-material-consumer-" + cacheSuffix)).string();
    std::filesystem::remove_all(candidateOptions.cacheDir, cleanupError);
    BOOST_CHECK_EXCEPTION(
      prepareNativeCanonicalOnnxRole(candidateFetchers, candidateProjection, candidateOptions),
      std::runtime_error,
      [] (const std::runtime_error& error) {
        return std::string(error.what()).find("DI_CANONICAL_MATERIAL_RECEIPT_INVALID") !=
               std::string::npos;
      });
    BOOST_CHECK_EQUAL(std::count(oversizedFetches->begin(), oversizedFetches->end(),
                                 manifestName.toUri()), 1U);
    BOOST_CHECK_EQUAL(std::count(oversizedFetches->begin(), oversizedFetches->end(),
                                 receiptName.toUri()), 1U);
    std::filesystem::remove_all(candidateOptions.cacheDir, cleanupError);
  };
  auto duplicateRootReceipt = receiptText;
  duplicateRootReceipt.insert(duplicateRootReceipt.rfind('}'),
                              ",\"materialObjects\":[]");
  runInvalidReceipt(duplicateRootReceipt, "duplicate-root-key");
  auto duplicateObjectReceipt = receiptText;
  const auto objectsStart = duplicateObjectReceipt.find("\"materialObjects\":[");
  const auto firstObjectEnd = duplicateObjectReceipt.find('}', objectsStart);
  BOOST_REQUIRE_NE(objectsStart, std::string::npos);
  BOOST_REQUIRE_NE(firstObjectEnd, std::string::npos);
  duplicateObjectReceipt.insert(firstObjectEnd, ",\"bytes\":1");
  runInvalidReceipt(duplicateObjectReceipt, "duplicate-object-key");

  auto oversizedInlineRootJson = NativeJson::parse(rootText);
  auto& inlineMetadata = oversizedInlineRootJson["metadata"];
  inlineMetadata.erase("materialReceiptBytes");
  inlineMetadata.erase("materialReceiptDataName");
  inlineMetadata.erase("materialReceiptDigest");
  auto inlineObjects = NativeJson::array();
  for (std::size_t index = 0; index < 32 && index < oversizedObjects.size(); ++index)
    inlineObjects.push_back(oversizedObjects.at(index));
  inlineMetadata["materialObjects"] = std::move(inlineObjects);
  const auto oversizedInlineRootText = nativeCanonicalJson(oversizedInlineRootJson);
  const std::vector<std::uint8_t> oversizedInlineRootBytes(
    oversizedInlineRootText.begin(), oversizedInlineRootText.end());
  auto oversizedInlineProjection = makeProjection(
    digest(oversizedInlineRootBytes), profileDigest,
    identity.graphDigest, identity.initializerDigest);
  oversizedInlineProjection.canonicalArtifactName = rootName.toUri();
  oversizedInlineProjection.assembly.nodeIndices = projection.assembly.nodeIndices;
  oversizedInlineProjection.assembly.maxSourceBytes = 8U << 20;
  oversizedInlineProjection.assembly.maxAssembledBytes = 16U << 20;
  oversizedInlineProjection.assembly.canonicalInitializerDigest = identity.initializerDigest;
  oversizedInlineProjection.assembly.recipeDigest =
    recipeDigestFor(oversizedInlineProjection.assembly);
  oversizedFetches->clear();
  NativeCanonicalOnnxFetchers oversizedInlineFetchers;
  oversizedInlineFetchers.getArtifact = [rootName, oversizedInlineRootBytes] (
      const ndn::Name& name) -> std::optional<ndn::Buffer> {
    if (name != rootName) return std::nullopt;
    return ndn::Buffer(oversizedInlineRootBytes.data(), oversizedInlineRootBytes.size());
  };
  oversizedInlineFetchers.fetchEncryptedLargeData =
    [=] (const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (service != ndn::Name("/LLM/Qwen")) return std::nullopt;
    oversizedFetches->push_back(name.toUri());
    return std::nullopt;
  };
  auto oversizedInlineOptions = options;
  oversizedInlineOptions.cacheDir = (std::filesystem::temp_directory_path() /
                                     "spec189-material-consumer-inline-root").string();
  std::filesystem::remove_all(oversizedInlineOptions.cacheDir, cleanupError);
  BOOST_CHECK_EXCEPTION(
    prepareNativeCanonicalOnnxRole(oversizedInlineFetchers, oversizedInlineProjection,
                                   oversizedInlineOptions),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find(
               "DI_CANONICAL_INLINE_MATERIAL_METADATA_TOO_LARGE") != std::string::npos;
    });
  BOOST_CHECK(oversizedFetches->empty());
  std::filesystem::remove_all(oversizedInlineOptions.cacheDir, cleanupError);
  std::filesystem::remove_all(options.cacheDir, cleanupError);
}

BOOST_AUTO_TEST_CASE(RegisteredOneProviderAssemblyLoadsOrt)
{
  runRegisteredProviderAssemblyCase(1);
}

BOOST_AUTO_TEST_CASE(DirectInt8CandidateAssemblyRunsOrtAndContinuation)
{
#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  BOOST_TEST_MESSAGE("Spec190 direct INT8 C++ gate skipped: ONNX Runtime backend is disabled");
  return;
#else
  const char* runGate = std::getenv("NDNSF_SPEC190_RUN_INT8");
  const char* modelPath = std::getenv("NDNSF_SPEC190_INT8_ONNX");
  // This case is only selected by the explicit qualification profile, so running it without its inputs is a
  // configuration error and must fail by name instead of returning a "passed" case that asserted nothing.
  if (runGate == nullptr || std::string(runGate) != "1") {
    BOOST_FAIL("DIRECT_INT8_INPUT_MISSING: set NDNSF_SPEC190_RUN_INT8=1 and NDNSF_SPEC190_INT8_ONNX");
  }
  // Gate open but file missing is the other half of the same contract: name it, do not abort anonymously.
  if (modelPath == nullptr || *modelPath == '\0') {
    BOOST_FAIL("DIRECT_INT8_MODEL_MISSING: NDNSF_SPEC190_RUN_INT8=1 requires NDNSF_SPEC190_INT8_ONNX");
  }
  const auto candidate = std::filesystem::path(modelPath);
  if (!std::filesystem::is_regular_file(candidate)) {
    BOOST_FAIL("DIRECT_INT8_MODEL_MISSING: not a regular file: " + candidate.string());
  }
  const auto source = readBytes(candidate);
  BOOST_REQUIRE(!source.empty());
  onnx::ModelProto model;
  BOOST_REQUIRE(model.ParseFromArray(source.data(), static_cast<int>(source.size())));
  const auto sourceDigest = digest(source);
  const auto sourceBytes = source.size();

  NativeCanonicalSource canonicalSource;
  canonicalSource.modelBytes = source;
  NativeAssemblyControl identityControl;
  identityControl.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  identityControl.requireActive = [] {};
  identityControl.maxSourceBytes = static_cast<std::uint64_t>(sourceBytes) + (64U << 20);
  identityControl.maxAssembledBytes = static_cast<std::uint64_t>(sourceBytes) * 4U;
  const auto identity = canonicalOnnxSourceIdentity(canonicalSource, identityControl);
  BOOST_REQUIRE(!identity.graphDigest.empty());
  BOOST_REQUIRE(!identity.initializerDigest.empty());

  const ndn::Name rootName("/spec190/int8/candidate/root");
  const ndn::Name sourceName("/spec190/int8/candidate/source");
  const auto rootJson = std::string(
    "{\"artifactProfileDigest\":\"") + zeroDigest('b') +
    "\",\"metadata\":{\"canonicalSourceBytes\":" +
    std::to_string(sourceBytes) +
    ",\"canonicalSourceDataName\":\"" + sourceName.toUri() +
    "\",\"canonicalSourceDigest\":\"" + sourceDigest +
    "\"},\"modelIdentityDigest\":\"" + zeroDigest('a') +
    "\",\"modelName\":\"Qwen3-0.6B-ONNX-weight-only-int8\","
    "\"schema\":\"ndnsf-di-canonical-model-manifest-v1\","
    "\"state\":\"ACTIVE\"}";
  const std::vector<std::uint8_t> rootPayload(rootJson.begin(), rootJson.end());
  const auto rootDigest = digest(rootPayload);
  auto projection = makeDirectInt8CandidateProjection(
    model, identity, sourceDigest, rootDigest, sourceBytes);

  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [rootName, rootPayload] (const ndn::Name& name)
    -> std::optional<ndn::Buffer> {
    if (name != rootName) return std::nullopt;
    return ndn::Buffer(rootPayload.data(), rootPayload.size());
  };
  fetchers.fetchEncryptedLargeData = [sourceName, source] (
      const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (name != sourceName || service != ndn::Name("/LLM/Qwen"))
      return std::nullopt;
    return ndn::Buffer(source.data(), source.size());
  };

  NativeCanonicalOnnxAssemblerOptions options;
  options.workerLocation = testWorkerLocation();
  options.cacheDir = (std::filesystem::temp_directory_path() /
                       "spec190-direct-int8-cpp-gate").string();
  options.providerIdentity = projection.provider;
  options.assemblyTimeoutMs = 600000;
  options.signManifest = [] (const std::string& bytes) {
    return std::string("spec190-int8-cpp-gate-") + digest(bytes);
  };
  std::error_code cleanupError;
  std::filesystem::remove_all(options.cacheDir, cleanupError);
  const auto prepared = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  BOOST_REQUIRE(std::filesystem::is_regular_file(prepared.path));

  auto runnerSpec = prepared;
  bindNativeRunnerPreparationContext(
    runnerSpec, projection,
    {projection.provider, "spec190-int8-cpp-boot", 1, options.cacheDir});
  std::vector<std::string> inputNames;
  inputNames.reserve(projection.assembly.expectedInputs.size());
  for (const auto& input : projection.assembly.expectedInputs) {
    inputNames.push_back(input.name);
  }
  std::vector<std::string> outputNames;
  outputNames.reserve(projection.assembly.expectedOutputs.size());
  for (const auto& output : projection.assembly.expectedOutputs)
    outputNames.push_back(output.name);
  runnerSpec.metadata["inputNames"] = joinCandidateNames(inputNames);
  runnerSpec.metadata["outputNames"] = joinCandidateNames(outputNames);

  {
    OnnxRuntimeModelRunner runner(std::move(runnerSpec));
    const auto evidence = runner.executionEvidenceSnapshot();
    BOOST_REQUIRE(evidence);
    BOOST_CHECK(evidence->loadCompleted);
    BOOST_CHECK(evidence->warmupCompleted);
    BOOST_CHECK(evidence->realCompute);
    BOOST_CHECK_EQUAL(evidence->deviceKind, "cpu");

    RoleExecutionContext first;
    first.sessionId = "spec190-int8-cpp-session";
    first.requestId = projection.requestId;
    first.role = projection.assembly.role;
    first.providerBootId = "spec190-int8-cpp-boot";
    first.attemptEpoch = 1;
    first.inferenceEpoch = 0;
    GenerationEpochLineageV1 firstLineage;
    firstLineage.requestId = first.requestId;
    firstLineage.attemptEpoch = first.attemptEpoch;
    firstLineage.planDigest = projection.planDigest;
    firstLineage.generationId = "spec190-int8-generation";
    firstLineage.streamEpoch = 1;
    firstLineage.inferenceEpoch = first.inferenceEpoch;
    firstLineage.transitionKind = GenerationEpochLineageV1::PREFILL;
    firstLineage.logicalPrefixDigest = zeroDigest('e');
    firstLineage.logicalPrefixTokenCount = 1;
    firstLineage.positionDigest = zeroDigest('f');
    firstLineage.validateCore();
    first.generationLineage = firstLineage;
    first.generationInputTokenCount = 1;
    first.inputsByScope.emplace(
      "input_ids", makeEncodedTensorBundle(
        "input_ids", {makeInt64CandidateTensor("input_ids", 3)}));
    first.inputsByScope.emplace(
      "attention_mask", makeEncodedTensorBundle(
        "attention_mask", {makeInt64CandidateVector("attention_mask", {1})}));
    first.inputsByScope.emplace(
      "position_ids", makeEncodedTensorBundle(
        "position_ids", {makeInt64CandidateTensor("position_ids", 0)}));
    const auto firstOutputs = runner.run(first);
    const auto firstBundle = firstOutputs.find("onnx-output-bundle");
    BOOST_REQUIRE(firstBundle != firstOutputs.end());
    const auto firstTensors = decodeTensorBundle(firstBundle->second.payload);
    const auto* firstLogits = findCandidateTensor(firstTensors, "logits");
    BOOST_REQUIRE(firstLogits != nullptr);
    BOOST_CHECK(firstLogits->elementType == TensorElementType::Float32);
    BOOST_CHECK(firstLogits->shape ==
                (std::vector<std::int64_t>{1, 1, 151936}));

    RoleExecutionContext continuation;
    continuation.sessionId = first.sessionId;
    continuation.requestId = projection.requestId;
    continuation.role = projection.assembly.role;
    continuation.providerBootId = first.providerBootId;
    continuation.attemptEpoch = 1;
    continuation.inferenceEpoch = 1;
    GenerationEpochLineageV1 continuationLineage = firstLineage;
    continuationLineage.inferenceEpoch = continuation.inferenceEpoch;
    continuationLineage.transitionKind = GenerationEpochLineageV1::DECODE;
    continuationLineage.logicalPrefixTokenCount = 2;
    continuationLineage.validateCore();
    continuation.generationLineage = continuationLineage;
    continuation.generationInputTokenCount = 1;
    continuation.inputsByScope.emplace(
      "input_ids", makeEncodedTensorBundle(
        "input_ids", {makeInt64CandidateTensor("input_ids", 4)}));
    continuation.inputsByScope.emplace(
      "attention_mask", makeEncodedTensorBundle(
        "attention_mask", {makeInt64CandidateVector("attention_mask", {1, 1})}));
    continuation.inputsByScope.emplace(
      "position_ids", makeEncodedTensorBundle(
        "position_ids", {makeInt64CandidateTensor("position_ids", 1)}));
    for (std::size_t index = 0; index < projection.generationContract.stateInputNames.size(); ++index) {
      const auto& inputName = projection.generationContract.stateInputNames.at(index);
      const auto& outputName = projection.generationContract.stateOutputNames.at(index);
      const auto* predecessor = findCandidateTensor(firstTensors, outputName);
      BOOST_REQUIRE(predecessor != nullptr);
      auto successor = *predecessor;
      successor.name = inputName;
      continuation.inputsByScope.emplace(
        inputName, makeEncodedTensorBundle(inputName, {std::move(successor)}));
    }
    const auto secondOutputs = runner.run(continuation);
    const auto secondBundle = secondOutputs.find("onnx-output-bundle");
    BOOST_REQUIRE(secondBundle != secondOutputs.end());
    const auto secondTensors = decodeTensorBundle(secondBundle->second.payload);
    const auto* secondLogits = findCandidateTensor(secondTensors, "logits");
    BOOST_REQUIRE(secondLogits != nullptr);
    BOOST_CHECK(secondLogits->elementType == TensorElementType::Float32);
    BOOST_CHECK(secondLogits->shape ==
                (std::vector<std::int64_t>{1, 1, 151936}));
    const auto* secondPresent = findCandidateTensor(secondTensors, "present.0.key");
    BOOST_REQUIRE(secondPresent != nullptr);
    BOOST_CHECK(secondPresent->shape ==
                (std::vector<std::int64_t>{1, 8, 2, 128}));
  }
  std::filesystem::remove_all(options.cacheDir, cleanupError);
#endif
}

BOOST_AUTO_TEST_CASE(RegisteredTwoProviderAssemblyLoadsOrt)
{
  runRegisteredProviderAssemblyCase(2);
}

BOOST_AUTO_TEST_CASE(RegisteredFourProviderAssemblyLoadsOrt)
{
  runRegisteredProviderAssemblyCase(4);
}

BOOST_AUTO_TEST_CASE(NativeYoloMergeDecodesAndOrdersDependencyTensors)
{
  const std::array<std::string, 6> names{
    "/model/model.23/one2one_cv2.0/one2one_cv2.0.2/Conv_output_0",
    "/model/model.23/one2one_cv2.1/one2one_cv2.1.2/Conv_output_0",
    "/model/model.23/one2one_cv2.2/one2one_cv2.2.2/Conv_output_0",
    "/model/model.23/one2one_cv3.0/one2one_cv3.0.2/Conv_output_0",
    "/model/model.23/one2one_cv3.1/one2one_cv3.1.2/Conv_output_0",
    "/model/model.23/one2one_cv3.2/one2one_cv3.2.2/Conv_output_0",
  };
  const std::array<std::size_t, 3> grids{80, 40, 20};
  const std::array<std::string, 6> scopes{
    "merge-box-0", "merge-box-1", "merge-box-2",
    "merge-class-0", "merge-class-1", "merge-class-2",
  };
  const auto digestValue = zeroDigest('a');
  NativeModelRunnerSpec spec;
  spec.role = "Merge";
  spec.kind = "native-yolo-postprocess";
  spec.backend = "native-yolo-postprocess";
  spec.metadata = {
    {"mergeKind", "NATIVE_POSTPROCESS"},
    {"postprocessIdentity", "YOLO26n-canonical-detection-rows"},
    {"postprocessOutputName", "predictions"},
    {"postprocessConfidenceThreshold", "0.001000"},
    {"postprocessSort", "confidence-desc,class-asc,xyxy-asc"},
    {"expectedOutputShape", "1,2,6"},
    {"evidence.providerName", "/provider/merge"},
    {"evidence.providerBootId", "boot-merge"},
    {"evidence.epoch", "1"},
    {"evidence.createdAtMs", "1"},
    {"evidence.modelDigest", digestValue},
    {"evidence.planDigest", digestValue},
    {"evidence.artifactDigest", digestValue},
  };
  auto runner = makeNativeYoloMergeRunner(spec);
  BOOST_REQUIRE(runner->executionEvidence());
  BOOST_CHECK(!runner->executionEvidence()->realCompute);
  BOOST_CHECK_EQUAL(
    validateNativeProviderRuntimeReadiness(
      *runner->executionEvidence(), "Merge", "onnxruntime-cpu", "cpu", digestValue)
      .value_or(""), "");
  auto invalidEvidence = *runner->executionEvidence();
  invalidEvidence.realCompute = true;
  BOOST_CHECK_EQUAL(
    validateNativeProviderRuntimeReadiness(
      invalidEvidence, "Merge", "onnxruntime-cpu", "cpu", digestValue)
      .value_or(""), "DI_RUNTIME_EVIDENCE_INVALID");

  RoleExecutionContext context;
  context.role = "Merge";
  for (std::size_t scale = 0; scale < grids.size(); ++scale) {
    const auto cellCount = grids[scale] * grids[scale];
    std::vector<float> boxValues(cellCount * 4, 0.0f);
    for (std::size_t cell = 0; cell < cellCount; ++cell) {
      boxValues[cell] = 1.0f;
      boxValues[cellCount + cell] = 2.0f;
      boxValues[2 * cellCount + cell] = 3.0f;
      boxValues[3 * cellCount + cell] = 4.0f;
    }
    std::vector<float> classValues(cellCount * 80, -20.0f);
    if (scale == 0) {
      classValues[7 * cellCount + 2 * grids[scale] + 1] = 4.0f;
    }
    if (scale == 2) {
      classValues[2 * cellCount + 4 * grids[scale] + 3] = 3.0f;
    }
    auto bytes = [] (const std::vector<float>& values) {
      std::vector<std::uint8_t> result(values.size() * sizeof(float));
      std::memcpy(result.data(), values.data(), result.size());
      return result;
    };
    const auto boxBundle = makeEncodedTensorBundle(
      scopes[scale], {makeFloat32Tensor(
        names[scale], {1, 4, static_cast<std::int64_t>(grids[scale]),
                       static_cast<std::int64_t>(grids[scale])}, bytes(boxValues))});
    const auto classBundle = makeEncodedTensorBundle(
      scopes[scale + 3], {makeFloat32Tensor(
        names[scale + 3], {1, 80, static_cast<std::int64_t>(grids[scale]),
                           static_cast<std::int64_t>(grids[scale])}, bytes(classValues))});
    context.inputsByScope.emplace(scopes[scale], boxBundle);
    context.inputsByScope.emplace(scopes[scale + 3], classBundle);
    context.inputEdgesByScope.emplace(
      scopes[scale], DependencyEdge{scopes[scale], "DetectShard0", "Merge",
                                    "/planned/" + scopes[scale], 0, 0,
                                    {names[scale]}});
    context.inputEdgesByScope.emplace(
      scopes[scale + 3], DependencyEdge{scopes[scale + 3], "DetectShard0", "Merge",
                                        "/planned/" + scopes[scale + 3], 0, 0,
                                        {names[scale + 3]}});
  }
  const auto outputs = runner->run(context);
  BOOST_REQUIRE_EQUAL(outputs.size(), 1U);
  const auto& result = outputs.at("final-response");
  const auto decoded = decodeTensorBundle(result.payload);
  BOOST_REQUIRE_EQUAL(decoded.size(), 1U);
  BOOST_REQUIRE_EQUAL(decoded.front().name, "predictions");
  BOOST_REQUIRE(decoded.front().shape ==
                (std::vector<std::int64_t>{1, 2, 6}));
  std::vector<float> actual(decoded.front().payload.size() / sizeof(float));
  std::memcpy(actual.data(), decoded.front().payload.data(), decoded.front().payload.size());
  const auto firstConfidence = 1.0f / (1.0f + std::exp(-4.0f));
  const auto secondConfidence = 1.0f / (1.0f + std::exp(-3.0f));
  const std::vector<float> expected{
    4.0f, 4.0f, 36.0f, 52.0f, firstConfidence, 7.0f,
    80.0f, 80.0f, 208.0f, 272.0f, secondConfidence, 2.0f,
  };
  BOOST_REQUIRE_EQUAL(actual.size(), expected.size());
  for (std::size_t index = 0; index < actual.size(); ++index) {
    BOOST_CHECK_CLOSE(actual[index], expected[index], 0.001);
  }
}

BOOST_AUTO_TEST_CASE(NativeProviderIssuesCanonicalPreparationOfferV3)
{
  const auto signerKeyId = zeroDigest('2');
  NativeProviderOfferV3Config config;
  config.provider = "/provider/A";
  config.service = "/AI/YOLO/YOLO26n";
  config.bootEpoch = "boot-1";
  config.signerKeyId = signerKeyId;
  config.acceptedRoles = {"BackboneNeck"};
  config.backends = {"onnxruntime-cpu"};
  config.canProvision = true;
  config.hasModel = false;
  std::string signedDigest;
  config.signDigest = [&signedDigest] (const std::string& value) {
    signedDigest = value;
    return "fixture-signature";
  };
  const std::string request =
    "{\"attempt\":1,\"model_identity_hash\":\"" + zeroDigest('1') +
    "\",\"plan_deadline_ms\":1700000060000,\"request_id\":\"/request/1\","
    "\"schema\":\"ndnsf-di-request-envelope-v2\","
    "\"service\":\"/AI/YOLO/YOLO26n\","
    "\"task\":{\"placement_profile\":\"DI_PLACEMENT_V3\"}}";
  const auto decision = issueNativeProviderOfferV3(
    std::vector<std::uint8_t>(request.begin(), request.end()),
    config, 1700000000000ULL);
  BOOST_REQUIRE(decision);
  BOOST_CHECK(decision->status);
  BOOST_CHECK_EQUAL(decision->message, "DI_PLACEMENT_V3_OFFER");
  BOOST_CHECK_EQUAL(decision->pendingStateTtlMs, 60000U);
  BOOST_CHECK_EQUAL(
    signedDigest,
    "sha256:6a0b22af7b13bb44b7664a71c8b94c28267951184c1ea2a912704f748b1ef444");
  BOOST_CHECK_EQUAL(
    decision->payload,
    "{\"accepted_roles\":[\"BackboneNeck\"],\"ack_reservation\":false,"
    "\"attempt\":1,\"backends\":[\"onnxruntime-cpu\"],\"bandwidth_mbps\":0.0,"
    "\"boot_epoch\":\"boot-1\",\"can_provision\":true,"
    "\"captured_at_ms\":1700000000000,\"estimated_wait_ms\":0.0,"
    "\"execution_disposition\":\"ACCEPT_WITH_PREPARATION\","
    "\"expires_at_ms\":1700000060000,\"graph_digest\":\"" + zeroDigest() +
    "\",\"has_model\":false,\"model_digest\":\"" + zeroDigest('1') +
    "\",\"preparation_accepted\":true,\"provider\":\"/provider/A\","
    "\"queue_depth\":0,\"request_id\":\"/request/1\",\"residency\":[],"
    "\"resources\":[],\"rtt_ms\":0.0,\"schema\":\"DI_PLACEMENT_V3\","
    "\"schema_version\":3,\"service\":\"/AI/YOLO/YOLO26n\","
    "\"signature\":\"fixture-signature\",\"signer_key_id\":\"" + signerKeyId +
    "\",\"status\":true,\"topology\":{\"backend\":\"cpu\",\"devices\":[],"
    "\"provider\":\"/provider/A\",\"topology_digest\":\"\"}}"
  );

  bool signerCalled = false;
  config.signDigest = [&signerCalled] (const std::string&) {
    signerCalled = true;
    return "must-not-be-used";
  };
  const auto expired = issueNativeProviderOfferV3(
    std::vector<std::uint8_t>(request.begin(), request.end()),
    config, 1700000060000ULL);
  BOOST_REQUIRE(expired);
  BOOST_CHECK(!expired->status);
  BOOST_CHECK_EQUAL(expired->message, "DI_V3_REQUEST_EXPIRED");
  BOOST_CHECK(!signerCalled);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
