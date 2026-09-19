#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeYoloMergeRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "ndnsf-integration-fixture.hpp"

#include <ndn-cxx/util/sha256.hpp>
#include <onnx/onnx_pb.h>

#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

namespace ndnsf::di::tests {
namespace {

using namespace ndn_service_framework;
namespace fixture = ndn_service_framework::test;

std::filesystem::path
findFixture()
{
  const auto relative = std::filesystem::path(
    "tests/fixtures/spec175/tiny-causal-lm-v1/two-role/role-0.onnx");
  for (const auto& root : {std::filesystem::current_path(),
                           std::filesystem::current_path().parent_path(),
                           std::filesystem::current_path().parent_path().parent_path()}) {
    const auto candidate = root / relative;
    if (std::filesystem::is_regular_file(candidate)) {
      return candidate;
    }
  }
  return {};
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
// real installed DI_NativeOnnxAssemblyWorker from the build root (or the
// NDNSF_SPEC182_BIN_DIR override).  sha256 stays empty here (no rehash
// pinning); the Spec182OnnxActivation suite covers pinned preflight.
NativeOnnxWorkerLocation
testWorkerLocation()
{
  std::vector<std::string> candidates;
  const char* dir = std::getenv("NDNSF_SPEC182_BIN_DIR");
  if (dir != nullptr && *dir != '\0') {
    candidates.push_back(std::string(dir) + "/DI_NativeOnnxAssemblyWorker");
  }
  candidates.push_back("build-nac182/DI_NativeOnnxAssemblyWorker");
  candidates.push_back("build/DI_NativeOnnxAssemblyWorker");
  candidates.push_back("../build-nac182/DI_NativeOnnxAssemblyWorker");
  candidates.push_back("examples/DI_NativeOnnxAssemblyWorker");
  for (const auto& path : candidates) {
    if (std::filesystem::is_regular_file(path)) {
      return NativeOnnxWorkerLocation{path, ""};
    }
  }
  BOOST_FAIL("DI_NativeOnnxAssemblyWorker binary not found: run the full waf "
             "build (with examples) before the integration suites");
  return {};
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
  projection.planDigest = zeroDigest('d');

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
  const auto projection = makeProjection(
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

  const auto first = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  BOOST_REQUIRE(std::filesystem::is_regular_file(first.path));
  BOOST_CHECK_EQUAL(first.metadata.at("assembledFrom"),
                    "canonical-root-post-selection");
  BOOST_CHECK_EQUAL(first.metadata.at("modelManifestDigest"), rootDigest);
  const auto second = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  BOOST_CHECK_EQUAL(second.path, first.path);
  BOOST_CHECK(std::filesystem::is_regular_file(second.path));

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
    "\",\"modelIdentityDigest\":\"" + zeroDigest('a') +
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
  const ndn::Name sourceName("/spec189/material/source");

  std::map<std::string, std::vector<std::uint8_t>> payloads;
  std::map<std::string, std::string> payloadNames;
  for (const auto& payload : materialSource.materialManifest->payloads) {
    payloads.emplace(payload.payloadId, payload.bytes);
    payloadNames.emplace(payload.payloadId,
      "/spec189/material/payload/" + payload.payloadId);
  }
  std::ostringstream metadata;
  metadata << "{\"canonicalSourceBytes\":" << source.size()
           << ",\"canonicalSourceDataName\":" << jsonQuote(sourceName.toUri())
           << ",\"canonicalSourceDigest\":" << jsonQuote(sourceDigest)
           << ",\"materialIdentityDigest\":"
           << jsonQuote(materialSource.materialManifest->manifestDigest)
           << ",\"materialManifestBytes\":" << manifestBytes.size()
           << ",\"materialManifestDataName\":" << jsonQuote(manifestName.toUri())
           << ",\"materialManifestDigest\":" << jsonQuote(digest(manifestBytes))
           << ",\"materialObjects\":[";
  for (std::size_t index = 0; index < materialSource.materialManifest->payloads.size(); ++index) {
    if (index != 0) metadata << ',';
    const auto& payload = materialSource.materialManifest->payloads[index];
    metadata << "{\"bytes\":" << payload.bytes.size()
             << ",\"dataName\":" << jsonQuote(payloadNames.at(payload.payloadId))
             << ",\"digest\":" << jsonQuote(payload.digest)
             << ",\"payloadId\":" << jsonQuote(payload.payloadId) << '}';
  }
  metadata << "]}";
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
  projection.assembly.maxAssembledBytes = manifestBytes.size() + firstPayload;
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
  // The role-0 fixture owns all of its 21 production nodes.  Add one
  // well-formed, unreachable node so the receipt test has a real unselected
  // payload while the selected role remains a checker-valid graph.
  onnx::ModelProto augmentedModel;
  BOOST_REQUIRE(augmentedModel.ParseFromArray(source.data(),
                                               static_cast<int>(source.size())));
  auto* unselectedNode = augmentedModel.mutable_graph()->add_node();
  unselectedNode->set_name("spec189-unselected-material");
  unselectedNode->set_op_type("Identity");
  unselectedNode->add_input("float_zero");
  unselectedNode->add_output("spec189-unselected-material");
  std::string augmentedWire;
  BOOST_REQUIRE(augmentedModel.SerializeToString(&augmentedWire));
  source.assign(augmentedWire.begin(), augmentedWire.end());
  NativeCanonicalSource materialSource;
  NativeAssemblyControl manifestControl{
    std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {},
    1U << 20, 8U << 20};
  materialSource.modelBytes = source;
  materialSource.materialManifest = deriveNativeCanonicalMaterialManifest(
    materialSource, manifestControl);
  const auto identity = canonicalOnnxSourceIdentity(materialSource, manifestControl);
  const auto manifestText = materialSource.materialManifest->canonicalJson();
  const std::vector<std::uint8_t> manifestBytes(manifestText.begin(), manifestText.end());
  const auto profileDigest = zeroDigest('b');
  const ndn::Name rootName("/spec189/material/bundle-root");
  const ndn::Name manifestName("/spec189/material/bundle-manifest");
  const ndn::Name receiptName("/spec189/material/bundle-receipt");
  const ndn::Name selectedBundleName("/spec189/material/bundle/selected");
  const ndn::Name unselectedBundleName("/spec189/material/bundle/unselected");
  // Use the fixture's certified role-0 node set.  The selected graph must be
  // independently valid for the production worker's full ONNX checker; a
  // prefix such as nodes {0, 1} is only a transport subset and is not a valid
  // role model because it does not produce the declared role outputs.
  const std::vector<std::uint64_t> certifiedRoleNodes{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
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
      if (reference.kind == "shared-initializer" && reference.logicalName == dependency)
        selectedIds.insert(reference.payloadId);
    }
  }
  std::vector<std::vector<std::uint8_t>> bundles(2);
  std::vector<std::size_t> bundleIndexByPayload;
  std::map<std::string, std::size_t> offsets;
  bundleIndexByPayload.reserve(materialSource.materialManifest->payloads.size());
  for (const auto& payload : materialSource.materialManifest->payloads) {
    const auto bundleIndex = selectedIds.count(payload.payloadId) != 0 ? 0U : 1U;
    bundleIndexByPayload.push_back(bundleIndex);
    offsets.emplace(payload.payloadId, bundles.at(bundleIndex).size());
    bundles.at(bundleIndex).insert(bundles.at(bundleIndex).end(),
                                   payload.bytes.begin(), payload.bytes.end());
  }
  BOOST_REQUIRE(!bundles.at(0).empty());
  BOOST_REQUIRE(!bundles.at(1).empty());
  for (const auto& bundle : bundles)
    BOOST_REQUIRE_LE(bundle.size(), NativeCanonicalMaterialBundleMaxBytes);
  const std::array<ndn::Name, 2> bundleNames{selectedBundleName, unselectedBundleName};
  const std::array<std::string, 2> bundleDigests{digest(bundles.at(0)), digest(bundles.at(1))};
  std::ostringstream materialObjects;
  materialObjects << '[';
  for (std::size_t index = 0; index < materialSource.materialManifest->payloads.size(); ++index) {
    if (index != 0) materialObjects << ',';
    const auto& payload = materialSource.materialManifest->payloads.at(index);
    const auto bundleIndex = bundleIndexByPayload.at(index);
    materialObjects << "{\"bundleBytes\":" << bundles.at(bundleIndex).size()
                    << ",\"bundleDigest\":" << jsonQuote(bundleDigests.at(bundleIndex))
                    << ",\"bundleOffset\":" << offsets.at(payload.payloadId)
                    << ",\"bytes\":" << payload.bytes.size()
                    << ",\"dataName\":" << jsonQuote(bundleNames.at(bundleIndex).toUri())
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
  projection.assembly.canonicalInitializerDigest = identity.initializerDigest;
  projection.assembly.maxAssembledBytes = manifestBytes.size() + bundles.at(0).size() + (1U << 20);
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
    if (name == bundleNames.at(0))
      return ndn::Buffer(bundles.at(0).data(), bundles.at(0).size());
    if (name == bundleNames.at(1))
      return ndn::Buffer(bundles.at(1).data(), bundles.at(1).size());
    return std::nullopt;
  };
  NativeCanonicalOnnxAssemblerOptions options;
  options.workerLocation = testWorkerLocation();
  options.cacheDir = (std::filesystem::temp_directory_path() /
                      "spec189-material-consumer-bundle").string();
  options.providerIdentity = projection.provider;
  options.signManifest = [] (const std::string& bytes) {
    return std::string("spec189-material-bundle-signature-") + digest(bytes);
  };
  std::error_code cleanupError;
  std::filesystem::remove_all(options.cacheDir, cleanupError);
  const auto prepared = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  BOOST_REQUIRE(std::filesystem::is_regular_file(prepared.path));
  BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), manifestName.toUri()), 1U);
  BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), receiptName.toUri()), 1U);
  BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), selectedBundleName.toUri()), 1U);
  BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), unselectedBundleName.toUri()), 0U);
  BOOST_CHECK(std::all_of(fetches->begin(), fetches->end(), [&] (const auto& name) {
    return name == manifestName.toUri() || name == receiptName.toUri() ||
      name == selectedBundleName.toUri();
  }));
  // The receipt is an authenticated continuation of the root binding, not a
  // merely reachable index.  A root that points at a receipt with a foreign
  // graph identity must fail before any selected bundle is assembled.
  auto badReceiptJson = NativeJson::parse(receiptText);
  badReceiptJson["graphDigest"] = zeroDigest('c');
  const auto badReceiptText = nativeCanonicalJson(badReceiptJson);
  const std::vector<std::uint8_t> badReceiptBytes(badReceiptText.begin(), badReceiptText.end());
  auto badRootText = rootText;
  const auto receiptDigestText = digest(receiptBytes);
  const auto receiptDigestPosition = badRootText.find(receiptDigestText);
  BOOST_REQUIRE_NE(receiptDigestPosition, std::string::npos);
  badRootText.replace(receiptDigestPosition, receiptDigestText.size(), digest(badReceiptBytes));
  const std::vector<std::uint8_t> badRootBytes(badRootText.begin(), badRootText.end());
  auto badProjection = makeProjection(digest(badRootBytes), profileDigest,
                                      identity.graphDigest, identity.initializerDigest);
  badProjection.canonicalArtifactName = rootName.toUri();
  badProjection.assembly.nodeIndices = projection.assembly.nodeIndices;
  badProjection.assembly.canonicalInitializerDigest = identity.initializerDigest;
  badProjection.assembly.maxAssembledBytes = projection.assembly.maxAssembledBytes;
  badProjection.assembly.recipeDigest = recipeDigestFor(badProjection.assembly);
  NativeCanonicalOnnxFetchers badFetchers = fetchers;
  badFetchers.getArtifact = [rootName, badRootBytes] (const ndn::Name& name)
    -> std::optional<ndn::Buffer> {
    if (name != rootName) return std::nullopt;
    return ndn::Buffer(badRootBytes.data(), badRootBytes.size());
  };
  badFetchers.fetchEncryptedLargeData = [=] (const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (service != ndn::Name("/LLM/Qwen")) return std::nullopt;
    if (name == manifestName)
      return ndn::Buffer(manifestBytes.data(), manifestBytes.size());
    if (name == receiptName)
      return ndn::Buffer(badReceiptBytes.data(), badReceiptBytes.size());
    if (name == bundleNames.at(0))
      return ndn::Buffer(bundles.at(0).data(), bundles.at(0).size());
    return std::nullopt;
  };
  auto badOptions = options;
  badOptions.cacheDir = (std::filesystem::temp_directory_path() /
                         "spec189-material-consumer-bad-receipt").string();
  std::filesystem::remove_all(badOptions.cacheDir, cleanupError);
  BOOST_CHECK_EXCEPTION(
    prepareNativeCanonicalOnnxRole(badFetchers, badProjection, badOptions), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("DI_CANONICAL_MATERIAL_RECEIPT_IDENTITY_MISMATCH") !=
             std::string::npos;
    });
  std::filesystem::remove_all(badOptions.cacheDir, cleanupError);
  std::filesystem::remove_all(options.cacheDir, cleanupError);

  projection.assembly.maxAssembledBytes = manifestBytes.size() + receiptBytes.size() +
                                          bundles.at(0).size() - 1;
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
  BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), selectedBundleName.toUri()), 0U);
  BOOST_CHECK_EQUAL(std::count(fetches->begin(), fetches->end(), unselectedBundleName.toUri()), 0U);
  std::filesystem::remove_all(options.cacheDir, cleanupError);
}

BOOST_AUTO_TEST_CASE(RegisteredOneProviderAssemblyLoadsOrt)
{
  runRegisteredProviderAssemblyCase(1);
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
