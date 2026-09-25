/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */

#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/Provider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/RuntimeTestAccess.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedArtifactStore.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderArtifactCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include "ndnsf-integration-fixture.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"

#include <boost/test/unit_test.hpp>

#include <openssl/evp.h>
#include <ndn-cxx/util/sha256.hpp>

#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <algorithm>
#include <iterator>
#include <mutex>
#include <atomic>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <utility>
#include <vector>

using namespace ndnsf::di;
using namespace ndn_service_framework;

namespace {

class ScopedEnvironmentVariable
{
public:
  ScopedEnvironmentVariable(const char* name, const char* value)
    : m_name(name)
  {
    if (const char* previous = std::getenv(name); previous != nullptr) {
      m_previous = previous;
      m_hadPrevious = true;
    }
    if (::setenv(m_name.c_str(), value, 1) != 0)
      throw std::runtime_error("cannot set provider test environment variable");
  }

  ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
  ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

  ~ScopedEnvironmentVariable() noexcept
  {
    if (m_hadPrevious)
      (void)::setenv(m_name.c_str(), m_previous.c_str(), 1);
    else
      (void)::unsetenv(m_name.c_str());
  }

private:
  std::string m_name;
  std::string m_previous;
  bool m_hadPrevious = false;
};

std::filesystem::path
trustSchema()
{
  return std::filesystem::absolute("examples/trust-any.conf").lexically_normal();
}

ProviderConfig
makeConfig(const std::string& providerName = "/spec185/provider",
           const std::string& serviceName = "/Inference/Spec185Provider")
{
  const auto trust = trustSchema().string();
  std::vector<std::string> values{
    "provider", "--provider", providerName, "--group", "/spec185/group",
    "--controller", "/spec185/controller", "--trust-schema", trust,
    "--service", serviceName, "--role", "/Backbone",
    "--workers", "1"};
  std::vector<const char*> argv;
  argv.reserve(values.size());
  for (const auto& value : values)
    argv.push_back(value.c_str());
  return ProviderConfig::fromCommandLine(static_cast<int>(argv.size()), argv.data());
}

std::shared_ptr<NativeModelRunnerFactory>
makeProviderOracleRunnerFactory(std::shared_ptr<std::atomic<unsigned>> runs)
{
  auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
  factory->registerBackend(
    "onnxruntime",
    [runs] (const NativeModelRunnerSpec& spec) {
      ExecutionEvidence evidence;
      evidence.providerName = spec.metadata.at("provider");
      evidence.providerBootId = spec.metadata.at("boot");
      evidence.evidenceEpoch = 1;
      evidence.runnerKind = RunnerKind::OnnxRuntimeCpu;
      evidence.realCompute = true;
      evidence.deviceKind = "cpu";
      evidence.deviceId = "0";
      evidence.deviceIds = {"0"};
      evidence.runtimeVersion = "spec185-provider-oracle";
      evidence.modelDigest = spec.metadata.at("artifact");
      evidence.planDigest = spec.metadata.at("plan");
      evidence.artifactDigests[spec.role] = spec.metadata.at("artifact");
      evidence.roles = {spec.role};
      evidence.loadCompleted = true;
      evidence.warmupCompleted = true;
      evidence.createdAtMs = 1;
      evidence.validate();
      return makeNativeModelRunner(
        [runs] (const RoleExecutionContext&) {
          runs->fetch_add(1, std::memory_order_relaxed);
          const std::string response = "spec185-provider-response";
          return std::map<std::string, TensorBundle>{
            {"final-response", TensorBundle{
              "final-response",
              std::vector<std::uint8_t>(response.begin(), response.end()),
              1, response.size()}}};
        },
        std::move(evidence));
    });
  return factory;
}

using EvpKey = std::shared_ptr<EVP_PKEY>;

EvpKey
makeEd25519Key(char seed)
{
  const std::string raw(32, seed);
  return EvpKey(EVP_PKEY_new_raw_private_key(
    EVP_PKEY_ED25519, nullptr,
    reinterpret_cast<const unsigned char*>(raw.data()), raw.size()), EVP_PKEY_free);
}

std::string
publicKeyBytes(const EvpKey& key)
{
  std::string raw(32, '\0');
  std::size_t size = raw.size();
  if (!key || EVP_PKEY_get_raw_public_key(
        key.get(), reinterpret_cast<unsigned char*>(raw.data()), &size) != 1 || size != 32)
    throw std::runtime_error("provider test key extraction failed");
  return raw;
}

EvpKey
publicKey(const EvpKey& privateKey)
{
  const auto raw = publicKeyBytes(privateKey);
  return EvpKey(EVP_PKEY_new_raw_public_key(
    EVP_PKEY_ED25519, nullptr,
    reinterpret_cast<const unsigned char*>(raw.data()), raw.size()), EVP_PKEY_free);
}

std::filesystem::path
providerAssemblyFixture()
{
  const auto relative = std::filesystem::path(
    "tests/fixtures/spec175/tiny-causal-lm-v1/two-role/role-0.onnx");
  for (const auto& root : {std::filesystem::current_path(),
                           std::filesystem::current_path().parent_path(),
                           std::filesystem::current_path().parent_path().parent_path()}) {
    const auto candidate = root / relative;
    if (std::filesystem::is_regular_file(candidate))
      return candidate;
  }
  return {};
}

std::vector<std::uint8_t>
providerAssemblyRead(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>());
}

NativeOnnxIdentity
providerAssemblySourceIdentity(const std::vector<std::uint8_t>& source)
{
  NativeCanonicalSource canonicalSource;
  canonicalSource.modelBytes = source;
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {},
    1 << 20, 1 << 20};
  return canonicalOnnxSourceIdentity(canonicalSource, control);
}

struct ProviderRequestProbeState
{
  std::mutex mutex;
  bool requestPublished = false;
  bool selectionSeen = false;
  bool responseSeen = false;
  bool terminal = false;
  std::string failure;
};

std::string
providerAssemblyDigest(const std::vector<std::uint8_t>& bytes)
{
  ndn::util::Sha256 hash;
  hash.update(ndn::span<const std::uint8_t>(bytes.data(), bytes.size()));
  auto value = hash.toString();
  std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return "sha256:" + value;
}

std::string
zeroDigest(char value)
{
  return "sha256:" + std::string(64, value);
}

NativeOnnxWorkerLocation
providerAssemblyWorker()
{
  std::vector<std::string> candidates;
  if (const auto* dir = std::getenv("NDNSF_SPEC182_BIN_DIR"); dir && *dir)
    candidates.emplace_back(std::string(dir) + "/DI_NativeOnnxAssemblyWorker");
  candidates.emplace_back("build-nac182/DI_NativeOnnxAssemblyWorker");
  candidates.emplace_back("build/DI_NativeOnnxAssemblyWorker");
  candidates.emplace_back("build-spec185-b0c-normal/DI_NativeOnnxAssemblyWorker");
  candidates.emplace_back("../build-nac182/DI_NativeOnnxAssemblyWorker");
  candidates.emplace_back("../build-spec185-b0c-normal/DI_NativeOnnxAssemblyWorker");
  candidates.emplace_back("examples/DI_NativeOnnxAssemblyWorker");
  for (const auto& path : candidates) {
    if (std::filesystem::is_regular_file(path))
      return NativeOnnxWorkerLocation{path, ""};
  }
  BOOST_FAIL("DI_NativeOnnxAssemblyWorker binary not found");
  return {};
}

NativeSelectionProjectionV3
makeProviderAssemblyProjection(const std::string& rootDigest,
                               const std::string& profileDigest,
                               const std::string& graphDigest,
                               const std::string& initializerDigest)
{
  NativeSelectionProjectionV3 projection;
  projection.provider = "/spec185/provider/assembler";
  projection.requestId = "/spec185/provider/assembler-request";
  projection.canonicalArtifactName = "/spec185/provider/assembler/root";
  projection.plan.serviceName = "/LLM/Qwen";
  projection.plan.modelName = "spec175-tiny-causal-lm-v1";
  projection.planDigest = std::string("sha256:") + std::string(64, 'd');
  projection.deadlineMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 60'000;
  auto& role = projection.assembly;
  role.role = "/LLM/Pipeline/Stage/0";
  role.selectedRole = role.role;
  role.rank = 0;
  role.layerBegin = 0;
  role.layerEnd = 2;
  role.backend = "onnxruntime";
  role.adapterId = "onnx";
  role.adapterVersion = "1";
  role.deviceSet = {"cpu"};
  role.artifactDigest = std::string("sha256:") + std::string(64, 'c');
  role.roleKind = "PIPELINE_RANGE";
  role.modelManifestDigest = rootDigest;
  role.artifactProfileDigest = profileDigest;
  role.graphDigest = graphDigest;
  role.canonicalInitializerDigest = initializerDigest;
  role.adapterDescriptorDigest = std::string("sha256:") + std::string(64, '1');
  role.assemblerDescriptorDigest = std::string("sha256:") + std::string(64, '2');
  role.backendAbi = "onnxruntime-cpu-v1";
  for (std::uint64_t index = 0; index < 21; ++index)
    role.nodeIndices.push_back(index);
  role.expectedInputs = {
    {"input_ids", "int64", {"1", "sequence"}},
    {"attention_kv_in", "float32", {"2", "8"}},
    {"recurrent_state_in", "float32", {"2", "8"}},
    {"convolution_state_in", "float32", {"2", "8"}}};
  role.expectedOutputs = {
    {"hidden_out", "float32", {"1", "sequence", "8"}},
    {"attention_kv_out", "float32", {"2", "8"}},
    {"recurrent_state_out", "float32", {"2", "8"}},
    {"convolution_state_out", "float32", {"2", "8"}}};
  role.precision = "float32";
  role.quantization = "none";
  role.layout = "native";
  role.padding = "none";
  role.maxSourceBytes = 1024 * 1024;
  role.maxAssembledBytes = 1024 * 1024;
  role.maxNodes = 64;
  const auto canonicalRecipe = canonicalNativeOnnxRecipeJson(role);
  role.recipeDigest = providerAssemblyDigest(
    std::vector<std::uint8_t>(canonicalRecipe.begin(), canonicalRecipe.end()));
  projection.selectedRole = role;
  projection.plan.roles = {role.role};
  projection.executionRole.roleId = role.role;
  projection.executionRole.stageId = role.role;
  projection.executionRole.rank = role.rank;
  projection.executionRole.layerBegin = role.layerBegin;
  projection.executionRole.layerEnd = role.layerEnd;
  projection.executionRole.backend = role.backend;
  projection.executionRole.adapterId = role.adapterId;
  projection.executionRole.adapterVersion = role.adapterVersion;
  projection.dataflow.requestId = projection.requestId;
  projection.dataflow.attempt = projection.attempt;
  projection.dataflow.planDigest = projection.planDigest;
  projection.dataflow.role = role.role;
  projection.dataflow.terminalResponseOwner = true;
  projection.dataflow.dataflowDigest = graphDigest;
  projection.deviceBinding.mode = "SINGLE_DEVICE";
  projection.deviceBinding.provider = projection.provider;
  projection.deviceBinding.role = role.role;
  projection.deviceBinding.offerScopedDeviceHandle = "cpu";
  projection.deviceBinding.offerDigest = projection.planDigest;
  projection.deviceBinding.topologyProfileDigest = projection.planDigest;
  projection.deviceBinding.resourceSnapshotDigest = projection.planDigest;
  projection.deviceBinding.resourceSequence = 1;
  projection.deviceBinding.sharingPolicy = "exclusive";
  return projection;
}

std::string
providerProjection(const ndn::Name& provider,
                   const ndn::Name& requestId,
                   const std::string& planDigest,
                   const std::string& artifactDigest,
                   const std::string& protectionEpoch = "plaintext-v1",
                   const std::string& grantName = {},
                   const std::string& grantDigest = {})
{
  NativeSelectionProjectionV3 projection;
  projection.provider = provider.toUri();
  projection.requestId = requestId.toUri();
  projection.attempt = 1;
  projection.planCoreDigest = planDigest;
  projection.planDigest = planDigest;
  projection.ackClosedDigest = planDigest;
  projection.offerDigest = planDigest;
  projection.securityPolicySnapshotDigest = planDigest;
  projection.deadlineMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 60'000;
  projection.groupCapabilityV1 = "spec185-provider-capability";
  projection.plan.version = 3;
  projection.plan.executionPolicy = "DATA_DRIVEN_V2";
  projection.plan.roles = {"/Backbone"};
  projection.hasGrantBinding = !grantName.empty() || !grantDigest.empty();
  projection.grantName = grantName;
  projection.grantDigest = grantDigest;
  auto fillRole = [&] (NativeSelectionRoleV3& role) {
    role.role = "/Backbone";
    role.selectedRole = "/Backbone";
    role.rank = 0;
    role.layerBegin = 0;
    role.layerEnd = 1;
    role.backend = "onnxruntime";
    role.deviceSet = {"cpu"};
    role.artifactDigest = artifactDigest;
    role.recipeDigest = planDigest;
    role.roleKind = "PIPELINE_RANGE";
    role.adapterId = "onnx";
    role.adapterVersion = "1";
    role.modelManifestDigest = planDigest;
    role.artifactProfileDigest = planDigest;
    role.graphDigest = planDigest;
    role.canonicalInitializerDigest = planDigest;
    role.adapterDescriptorDigest = planDigest;
    role.assemblerDescriptorDigest = planDigest;
    role.backendAbi = "onnxruntime-cpu-v1";
    role.nodeIndices = {0};
    role.expectedInputs = {{"input_ids", "bytes", {"1"}}};
    role.expectedOutputs = {{"output", "bytes", {"1"}}};
    role.precision = "float32";
    role.quantization = "none";
    role.layout = "native";
    role.padding = "none";
    role.maxSourceBytes = 1024;
    role.maxAssembledBytes = 1024;
    role.maxNodes = 1;
    role.protectionEpoch = protectionEpoch;
  };
  fillRole(projection.selectedRole);
  projection.assembly = projection.selectedRole;
  projection.executionRole.roleId = "/Backbone";
  projection.executionRole.stageId = "/Backbone";
  projection.executionRole.rank = 0;
  projection.executionRole.layerBegin = 0;
  projection.executionRole.layerEnd = 1;
  projection.executionRole.backend = "onnxruntime";
  projection.executionRole.adapterId = "onnx";
  projection.executionRole.adapterVersion = "1";
  projection.dataflow.requestId = projection.requestId;
  projection.dataflow.attempt = projection.attempt;
  projection.dataflow.planDigest = projection.planDigest;
  projection.dataflow.role = "/Backbone";
  projection.dataflow.terminalResponseOwner = true;
  projection.dataflow.dataflowDigest = planDigest;
  projection.deviceBinding.mode = "SINGLE_DEVICE";
  projection.deviceBinding.provider = projection.provider;
  projection.deviceBinding.role = "/Backbone";
  projection.deviceBinding.offerScopedDeviceHandle = "cpu";
  projection.deviceBinding.offerDigest = planDigest;
  projection.deviceBinding.topologyProfileDigest = planDigest;
  projection.deviceBinding.resourceSnapshotDigest = planDigest;
  projection.deviceBinding.resourceSequence = 1;
  projection.deviceBinding.sharingPolicy = "exclusive";
  return nativeSelectionProjectionV3ToJson(projection);
}

struct ProviderHybridPublication
{
  HybridMessageKey key;
  ndn::Buffer wire;
};

ProviderHybridPublication
makeProviderPublication(const ndn::Name& messageName,
                        const ndn::Name& serviceName,
                        const ndn::Name& requestId,
                        const ndn::Name& senderPrefix,
                        const std::string& messageType,
                        const ndn::Buffer& plaintext)
{
  HybridMessageCrypto crypto;
  HybridCryptoCounters counters;
  const auto key = crypto.getOrCreateSendKey(
    serviceName, senderPrefix,
    hybridAccessAttributeForName(messageName, serviceName),
    messageType, counters);
  const auto associatedData = hybridAssociatedData(
    messageName, messageType, requestId, serviceName, senderPrefix,
    key.keyId, key.epochId);
  const auto encrypted = hybridAesGcmEncrypt(
    key.key, ndn::span<const std::uint8_t>(plaintext.data(), plaintext.size()),
    ndn::span<const std::uint8_t>(associatedData.data(), associatedData.size()));
  HybridMessageEnvelope envelope;
  envelope.setKeyId(key.keyId);
  envelope.setEpochId(key.epochId);
  envelope.setMessageType(messageType);
  envelope.setNonce(encrypted.nonce);
  envelope.setCipherText(encrypted.ciphertext);
  envelope.setAuthTag(encrypted.tag);
  const auto wire = envelope.WireEncode();
  return {key, ndn::Buffer(wire.data(), wire.size())};
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec185ProviderAssembly)

BOOST_AUTO_TEST_CASE(ProviderConfigUsesOneValidatedCppGrammar)
{
  const auto config = makeConfig();
  BOOST_REQUIRE(config.valid());

  const auto path = std::filesystem::temp_directory_path() /
                    "spec185-provider-launch-test.json";
  {
    std::ofstream output(path);
    output << "{\n"
              "  \"schema\": \"ndnsf-di-native-provider-launch-v1\",\n"
              "  \"arguments\": [\n"
              "    \"--provider\", \"/spec185/file-provider\",\n"
              "    \"--group\", \"/spec185/group\",\n"
              "    \"--controller\", \"/spec185/controller\",\n"
              "    \"--trust-schema\", \"" << trustSchema().string() << "\",\n"
              "    \"--service\", \"/Inference/Spec185Provider\",\n"
              "    \"--role\", \"/Backbone\"\n"
              "  ],\n"
              "  \"cache\": {\"max_artifact_entries\": 2}\n"
              "}\n";
  }
  const auto fileConfig = ProviderConfig::fromFile(path);
  BOOST_CHECK(fileConfig.valid());
  std::filesystem::remove(path);

  {
    std::ofstream output(path);
    output << "{\"schema\":\"ndnsf-di-native-provider-launch-v1\","
              "\"arguments\":[],\"unexpected\":true}";
  }
  BOOST_CHECK_EXCEPTION(ProviderConfig::fromFile(path), std::invalid_argument,
                        [](const std::invalid_argument& error) {
                          return std::string(error.what()).find("unknown top-level") !=
                                 std::string::npos;
                        });
  std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(ProviderOnlyRuntimeServesAndDrainsNativeRegistration)
{
  // Runtime::open(ProviderConfig) still validates the production launch
  // grammar, but the unopened Provider owner is replaced before serve() with
  // the same borrowed-face fixture used by the authenticated assembly tests.
  // This keeps the public Provider-only Runtime path under test without
  // making the selector depend on an external NFD daemon.
  ScopedEnvironmentVariable allowLocalController(
    "NDNSF_SPEC185_ALLOW_LOCAL_CONTROLLER", "1");
  ndn_service_framework::test::BootstrapProfile profile;
  profile.groupPrefix = ndn::Name("/spec185/group");
  profile.syncPrefix = ndn::Name("/spec185/provider-only/sync");
  profile.userNode = ndn::Name("/spec185/provider-only/user");
  profile.providerNode = ndn::Name("/spec185/provider-only/provider");
  profile.userIdentity = ndn::Name("/spec185/user");
  profile.providerIdentity = ndn::Name("/spec185/provider");
  profile.attributeAuthority = ndn::Name("/spec185/aa");
  profile.serviceName = ndn::Name("/Inference/Spec185Provider");
  profile.providerRoles = {"/Backbone"};
  profile.providerFacesHaveDedicatedIoWorkers = true;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  environment.enableProviderProductionIngressForTest();
  const auto config = makeConfig();
  auto runtime = Runtime::open(config);
  BOOST_REQUIRE(runtime);
  const auto providerIdentity = environment.keyChain().getPib().getIdentity(
    environment.provider().getName());
  const auto providerCertificate = providerIdentity.getDefaultKey().getDefaultCertificate();
  auto inProcessProvider = Provider::fromServiceProviderForTest(
    environment.providerFace(), environment.provider(), environment.keyChain(),
    providerCertificate, providerCertificate, config,
    std::make_shared<RegistryNativeModelRunnerFactory>(), {}, {});
  ndnsf::di::detail::RuntimeTestAccess::bindProviderOnlyFixture(
    runtime, std::move(inProcessProvider));

  BOOST_CHECK_EXCEPTION(runtime->user(), DiError,
                        [](const DiError& error) {
                          return error.code() == "ROLE_UNAVAILABLE" &&
                                 error.boundary() == "user";
                        });

  auto provider = runtime->provider();
  BOOST_REQUIRE(provider.valid());
  const auto beforeSelection = provider.counters();
  BOOST_CHECK_EQUAL(beforeSelection.sourceFetches, 0U);
  BOOST_CHECK_EQUAL(beforeSelection.assemblies, 0U);
  BOOST_CHECK_EQUAL(beforeSelection.runnersCreated, 0U);
  auto conflicting = makeConfig("/spec185/other-provider");
  BOOST_CHECK_EXCEPTION(runtime->provider(conflicting), DiError,
                        [](const DiError& error) {
                          return error.code() == "CONFIG_CONFLICT" &&
                                 error.boundary() == "provider";
                        });
  const ServiceDefinition definition{
    "/Inference/Spec185Provider", {"/Backbone"}};
  auto registration = provider.serve(definition);
  BOOST_REQUIRE(registration.valid());
  BOOST_CHECK(!registration.closed());
  BOOST_CHECK_EQUAL(registration.serviceName(), definition.serviceName);

  BOOST_CHECK_EXCEPTION(provider.serve(definition), DiError,
                        [](const DiError& error) {
                          return error.code() == "SERVICE_ALREADY_REGISTERED" &&
                                 error.boundary() == "provider";
                         });
  BOOST_CHECK_EXCEPTION(provider.serve({definition.serviceName, {"/OtherRole"}}),
                        DiError,
                        [](const DiError& error) {
                          return error.code() == "INVALID_ARGUMENT" &&
                                 error.boundary() == "provider" &&
                                 std::string(error.what()).find("allow-list") !=
                                 std::string::npos;
                        });

  bool callbackCalled = false;
  bool drained = false;
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  auto drainSubscription = provider.drainAsync(std::chrono::milliseconds(100),
                      [&callbackCalled, &drained, &callbackMutex, &callbackCondition](
                        std::exception_ptr error, bool result) {
                        {
                          std::lock_guard<std::mutex> lock(callbackMutex);
                          callbackCalled = true;
                          drained = error == nullptr && result;
                        }
                        callbackCondition.notify_all();
                      });
  std::unique_lock<std::mutex> callbackLock(callbackMutex);
  BOOST_REQUIRE(callbackCondition.wait_for(
    callbackLock, std::chrono::seconds(2), [&callbackCalled] { return callbackCalled; }));
  (void)drainSubscription;
  BOOST_CHECK(callbackCalled);
  BOOST_CHECK(drained);
  const auto afterDrain = provider.counters();
  BOOST_CHECK_EQUAL(afterDrain.sourceFetches, 0U);
  BOOST_CHECK_EQUAL(afterDrain.assemblies, 0U);
  BOOST_CHECK_EQUAL(afterDrain.runnersCreated, 0U);
  BOOST_CHECK(registration.closed());
  // Provider-only close() is an admission fence; drain() is the bounded
  // lifecycle fence that joins the owned Face worker before the selector
  // exits.  The process qualification runs this fixture under LeakSanitizer.
  BOOST_REQUIRE(runtime->drain(std::chrono::seconds(2)));
}

// Keep the real Provider::serve -> authenticated Selection -> runner path in
// the Spec188 selector as well as the older Spec185 regression suite.  The
// body remains shared so this task cannot accidentally replace the production
// ingress with a direct assembler-only fixture.
BOOST_AUTO_TEST_SUITE(Spec188ProviderReferenceAssembly)

BOOST_AUTO_TEST_CASE(AuthenticatedSelectionRunsProviderAssemblyRunnerAndResponse)
{
  // This C++ ingress fixture uses the production Provider::serve path.  The
  // only test seam injects an already-bootstrapped Core owner and a
  // deterministic runner factory; Selection, registration, preparation and
  // Response still traverse the production CollaborationContext.
  ndn_service_framework::test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/Spec185ProviderOracle");
  profile.providerRoles = {"/Backbone"};
  profile.deferBridgeDelivery = true;
  profile.providerFacesHaveDedicatedIoWorkers = true;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  environment.user().setUseTokens(false);
  environment.provider().setUseTokens(false);

  const auto serviceName = environment.profile().serviceName;
  const auto providerName = environment.provider().getName();
  const auto requesterName = environment.user().getName();
  auto requestId = ndn::Name("/spec185-provider-positive");
  const auto planDigest = std::string("sha256:") + std::string(64, 'a');
  const auto artifactDigest = std::string("sha256:") + std::string(64, 'b');
  const auto bootId = environment.provider().getProviderBootEpoch();
  const auto protectionEpoch = std::string("spec185-provider-protected-v1");
  const auto authorityPrivate = makeEd25519Key('a');
  const auto requesterPrivate = makeEd25519Key('b');
  const auto recipientPrivate = makeEd25519Key('c');
  NativeGrantIssuerConfig issuerConfig;
  issuerConfig.authorityIdentity = "/spec185/authority";
  issuerConfig.requesterIdentity = requesterName.toUri();
  issuerConfig.protectionEpoch = protectionEpoch;
  issuerConfig.keyId = "spec185-provider-grant";
  issuerConfig.authorityPrivateKey = authorityPrivate;
  issuerConfig.requesterPublicKey = publicKey(requesterPrivate);
  issuerConfig.allowedModelManifests = {planDigest};
  issuerConfig.recipientPublicKeys.emplace(providerName.toUri(), publicKey(recipientPrivate));
  issuerConfig.contentKey = [] (const std::string&, const std::string&) {
    return std::vector<std::uint8_t>(32, 0x42);
  };
  NativeSignedGrantRequest grantRequest;
  grantRequest.providerIdentity = providerName.toUri();
  grantRequest.requesterIdentity = requesterName.toUri();
  grantRequest.requestId = requestId.toUri();
  grantRequest.attempt = 1;
  grantRequest.planCoreDigest = planDigest;
  grantRequest.grantViewDigest = planDigest;
  grantRequest.modelManifestDigest = planDigest;
  grantRequest.protectionEpoch = protectionEpoch;
  grantRequest.issuedAtMs = 1;
  const auto nowMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto issuedGrant = NativeArtifactGrantIssuer(issuerConfig).issue(
    grantRequest.sign(*requesterPrivate), nowMs, nowMs + 60'000);
  auto assignmentProjection = providerProjection(
    providerName, requestId, planDigest, artifactDigest, protectionEpoch,
    issuedGrant.grantName, issuedGrant.grantDigest);

  auto runs = std::make_shared<std::atomic<unsigned>>(0);
  auto preparationFactory =
    [planDigest, artifactDigest, bootId] (
      ndn_service_framework::ServiceProvider::CollaborationContext&,
      const NativeSelectionProjectionV3& projection,
      const std::shared_ptr<ProtectedRuntime>& protectedRuntime) {
      if (!protectedRuntime ||
          protectedRuntime->state() != ProtectedRuntimeState::GrantVerified ||
          projection.provider != projection.deviceBinding.provider)
        throw std::runtime_error("provider oracle preparation binding mismatch");
      NativeModelRunnerSpec spec;
      spec.role = "/Backbone";
      spec.kind = "onnx-model";
      spec.backend = "onnxruntime";
      spec.path = "/spec185/provider/model.onnx";
      const auto& assembly = projection.assembly;
      // Keep the deterministic runner factory's seam metadata alongside the
      // production validator identity fields.  The seam reads these values
      // with at(), so omission would turn a valid assembly into an exception.
      spec.metadata["provider"] = projection.provider;
      spec.metadata["boot"] = bootId;
      spec.metadata["plan"] = planDigest;
      spec.metadata["artifact"] = artifactDigest;
      spec.metadata["fragmentDigest"] = assembly.artifactDigest;
      spec.metadata["recipeDigest"] = assembly.recipeDigest;
      spec.metadata["modelManifestDigest"] = assembly.modelManifestDigest;
      spec.metadata["artifactProfileDigest"] = assembly.artifactProfileDigest;
      spec.metadata["graphDigest"] = assembly.graphDigest;
      spec.metadata["canonicalInitializerDigest"] =
        assembly.canonicalInitializerDigest;
      spec.metadata["adapterDescriptorDigest"] = assembly.adapterDescriptorDigest;
      spec.metadata["assemblerDescriptorDigest"] = assembly.assemblerDescriptorDigest;
      spec.metadata["backendAbi"] = assembly.backendAbi;
      spec.metadata["precision"] = assembly.precision;
      spec.metadata["quantization"] = assembly.quantization;
      spec.metadata["layout"] = assembly.layout;
      spec.metadata["padding"] = assembly.padding;
      spec.metadata["maxSourceBytes"] = std::to_string(assembly.maxSourceBytes);
      spec.metadata["maxAssembledBytes"] = std::to_string(assembly.maxAssembledBytes);
      spec.metadata["maxNodes"] = std::to_string(assembly.maxNodes);
      return spec;
    };
  const auto providerIdentity = environment.keyChain().getPib().getIdentity(providerName);
  const auto providerCertificate = providerIdentity.getDefaultKey().getDefaultCertificate();
  auto facade = Provider::fromServiceProviderForTest(
    environment.providerFace(), environment.provider(), environment.keyChain(),
    providerCertificate, providerCertificate,
      makeConfig(providerName.toUri(), serviceName.toUri()),
    makeProviderOracleRunnerFactory(runs), std::move(preparationFactory),
    [issuedGrant, authorityPublic = publicKeyBytes(authorityPrivate),
     recipientSeed = std::string(32, 'c'), protectionEpoch, bootId,
     verificationTimeMs = nowMs] (
      ndn_service_framework::ServiceProvider::CollaborationContext&,
      const NativeSelectionProjectionV3& projection,
      const std::shared_ptr<ProviderGroupCoordinator>&) {
      ProtectedRuntimeBindingV1 binding;
      binding.provider = projection.provider;
      binding.role = projection.executionRole.roleId;
      binding.requestId = projection.requestId;
      binding.attempt = projection.attempt;
      binding.planCoreDigest = projection.planCoreDigest;
      binding.planDigest = projection.planDigest;
      binding.securityPolicySnapshotDigest = projection.securityPolicySnapshotDigest;
      binding.protectionEpoch = protectionEpoch;
      binding.grantName = projection.grantName;
      binding.grantDigest = projection.grantDigest;
      binding.providerBootId = bootId;
      binding.fencingToken = ndnsf::di::nativeProtectedFencingToken(
        projection, bootId, {});
      binding.expiresAtMs = projection.deadlineMs;
      NativeProtectedGrantConfig grantConfig;
      grantConfig.authorityIdentity = "/spec185/authority";
      grantConfig.authorityPublicKeyRaw = authorityPublic;
      grantConfig.recipientKey = {
        NativeRecipientKey::Kind::Ed25519Seed, recipientSeed};
      grantConfig.modelManifestDigest = projection.selectedRole.modelManifestDigest;
      grantConfig.fetchGrant = [wire = issuedGrant.wireJson] (const std::string&) {
        return wire;
      };
      auto runtime = std::make_shared<ProtectedRuntime>(binding, std::move(grantConfig));
      runtime->verifyGrant(binding, verificationTimeMs);
      return runtime;
    });
  environment.enableProductionIngressForTest();
  auto registration = facade.serve({serviceName.toUri(), {"/Backbone"}});
  environment.provider().markHybridResponseKeyWrappedForTest(serviceName);
  const auto ackKey = environment.provider().prepareHybridSendKeyForTest(
    serviceName, "ACK");
  const auto responseKey = environment.provider().prepareHybridSendKeyForTest(
    serviceName, "RESPONSE");
  environment.user().cacheHybridReceiveKeyForTest(
    ackKey.keyId, ackKey.epochId, ackKey.key);
  environment.user().cacheHybridReceiveKeyForTest(
    responseKey.keyId, responseKey.epochId, responseKey.key);
  const auto selectionKey = environment.user().prepareHybridSendKeyForTest(
    serviceName, "SELECTION");
  environment.provider().cacheHybridReceiveKeyForTest(
    selectionKey.keyId, selectionKey.epochId, selectionKey.key);

  auto probe = std::make_shared<ProviderRequestProbeState>();
  std::mutex probeRegistryMutex;
  std::map<std::string, std::shared_ptr<ProviderRequestProbeState>> probeRegistry;
  const auto registerProbe = [&] (const ndn::Name& id,
                                  const std::shared_ptr<ProviderRequestProbeState>& state) {
    std::lock_guard<std::mutex> lock(probeRegistryMutex);
    probeRegistry[id.toUri()] = state;
  };
  const auto lookupProbe = [&] (const ndn::Name& id) {
    std::lock_guard<std::mutex> lock(probeRegistryMutex);
    const auto found = probeRegistry.find(id.toUri());
    return found == probeRegistry.end() ?
      std::shared_ptr<ProviderRequestProbeState>() : found->second;
  };
  const auto probeDone = [] (const std::shared_ptr<ProviderRequestProbeState>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->terminal;
  };
  const auto probeFailure = [] (const std::shared_ptr<ProviderRequestProbeState>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->failure;
  };
  const auto probeRequestPublished = [] (const std::shared_ptr<ProviderRequestProbeState>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->requestPublished;
  };
  const auto probeSelectionSeen = [] (const std::shared_ptr<ProviderRequestProbeState>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->selectionSeen;
  };
  const auto probeResponseSeen = [] (const std::shared_ptr<ProviderRequestProbeState>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->responseSeen;
  };
  registerProbe(requestId, probe);
  environment.user().setRequestPublisher(
    [&, lookupProbe] (const ndn::Name& requestIdForPublish,
         const ndn::Name& requestName,
         const std::vector<ndn::Name>& providers,
         const ndn::Name&, const ndn_service_framework::RequestMessage& request, std::size_t) {
      const auto requestProbe = lookupProbe(requestIdForPublish);
      if (!requestProbe)
        return;
      if (providers.size() != 1U) {
        std::lock_guard<std::mutex> lock(requestProbe->mutex);
        requestProbe->failure = "request publisher provider set mismatch";
        return;
      }
      const auto requestBlock = request.WireEncode();
      auto publication = makeProviderPublication(
        requestName, serviceName, requestIdForPublish, requesterName, "REQUEST",
        ndn::Buffer(requestBlock.data(), requestBlock.size()));
      environment.provider().cacheHybridReceiveKeyForTest(
        publication.key.keyId, publication.key.epochId, publication.key.key);
      environment.userPubSub().publish(
        requestName,
        ndn::span<const std::uint8_t>(publication.wire.data(), publication.wire.size()));
      {
        std::lock_guard<std::mutex> lock(requestProbe->mutex);
        requestProbe->requestPublished = true;
      }
    });
  environment.userPubSub().subscribeToProducer(
    environment.profile().providerNode,
    [&, serviceName] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      if (const auto ack = ndn_service_framework::parseRequestAckNameV2(publication.name)) {
        if (ack->serviceName == serviceName) {
          ndn::Block ackBlock(publication.data);
          environment.user().handleRequestAckByName(publication.name, ackBlock);
        }
        return;
      }
      // Response publications remain on ServiceUser's registered OnResponse
      // subscription, which performs the production HybridMessageEnvelope
      // decryption before dispatching the RequestService callback.
    }, true);

  ndn_service_framework::RequestMessage request;
  const std::string requestPayload = "provider-input";
  ndn::Buffer requestPayloadBuffer(
    reinterpret_cast<const std::uint8_t*>(requestPayload.data()),
    requestPayload.size());
  request.setPayload(requestPayloadBuffer, requestPayloadBuffer.size());
  request.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());
  const auto requestIdForCall = requestId;
  const auto assignmentProjectionForCall = assignmentProjection;
  environment.user().RequestService(
    std::vector<ndn::Name>{providerName}, serviceName, request, 200,
    ndn_service_framework::ServiceUser::AckCandidatesHandler(
      [&, probe, requestIdForCall, assignmentProjectionForCall] (
          const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
        if (candidates.size() != 1U || candidates.front().providerName != providerName) {
          std::lock_guard<std::mutex> lock(probe->mutex);
          probe->failure = "authenticated ACK candidate set mismatch";
          return std::vector<ndn_service_framework::AckSelectionCandidate>{};
        }
        {
          std::lock_guard<std::mutex> lock(probe->mutex);
          probe->selectionSeen = true;
        }
        ndn_service_framework::CollaborationAssignmentEnvelope assignmentEnvelope;
        assignmentEnvelope.role = "/Backbone";
        assignmentEnvelope.assignedArtifact = ndn::Name("/spec185/provider/oracle");
        assignmentEnvelope.opaquePayload = ndn::Buffer(
          reinterpret_cast<const std::uint8_t*>(assignmentProjectionForCall.data()),
          assignmentProjectionForCall.size());
        if (!environment.user().setSelectionAssignmentPayloadForRequest(
              requestIdForCall, providerName,
              ndn_service_framework::encodeCollaborationAssignmentEnvelope(assignmentEnvelope))) {
          std::lock_guard<std::mutex> lock(probe->mutex);
          probe->failure = "authenticated assignment payload rejected";
          return std::vector<ndn_service_framework::AckSelectionCandidate>{};
        }
        return candidates;
      }),
    4000,
    [probe] (const ndn::Name&) {
      std::lock_guard<std::mutex> lock(probe->mutex);
      if (probe->failure.empty()) probe->failure = "request timed out";
      probe->terminal = true;
    },
    [probe] (const ndn_service_framework::ResponseMessage& response) {
      std::lock_guard<std::mutex> lock(probe->mutex);
      if (!response.getStatus() && probe->failure.empty())
        probe->failure = response.getErrorInfo();
      probe->responseSeen = response.getStatus();
      probe->terminal = true;
    }, ndn_service_framework::tlv::FirstResponding, requestIdForCall);

  environment.pumpUntil([&] {
    if (!probeDone(probe) || !probeResponseSeen(probe))
      return false;
    const auto providerCounters = facade.counters();
    return runs->load(std::memory_order_relaxed) == 1U &&
           providerCounters.assemblies == 1U &&
           providerCounters.runnersCreated == 1U;
  });
  BOOST_CHECK(probeRequestPublished(probe));
  BOOST_CHECK(probeSelectionSeen(probe));
  const auto positiveFailure = probeFailure(probe);
  BOOST_CHECK_MESSAGE(positiveFailure.empty(), positiveFailure);
  BOOST_CHECK(probeResponseSeen(probe));
  BOOST_CHECK_EQUAL(runs->load(std::memory_order_relaxed), 1U);
  const auto counters = facade.counters();
  BOOST_CHECK_EQUAL(counters.sourceFetches, 0U);
  BOOST_CHECK_EQUAL(counters.assemblies, 1U);
  BOOST_CHECK_EQUAL(counters.runnersCreated, 1U);

  // The same production ingress is exercised with three authenticated
  // Selection substitutions.  Each must fail before preparation/runner
  // creation, leaving the positive counters unchanged.
  for (const auto mutation : {0, 1, 2}) {
    // Keep the request ID in one NDN name component.  RequestService uses
    // the final component boundary when deriving the service name; an
    // appended component would be interpreted as a service suffix.
    requestId = ndn::Name("/spec185-provider-reject-" + std::to_string(mutation));
    const auto requestIdForCall = requestId;
    const auto projectionProvider = mutation == 0
      ? ndn::Name("/spec185/other-provider") : providerName;
    const auto projectionEpoch = mutation == 1
      ? std::string("spec185-other-epoch") : protectionEpoch;
    const auto projectionGrantDigest = mutation == 2
      ? std::string("sha256:") + std::string(64, 'e') : issuedGrant.grantDigest;
    assignmentProjection = providerProjection(
      projectionProvider, requestId, planDigest, artifactDigest,
      projectionEpoch, issuedGrant.grantName, projectionGrantDigest);
    const auto assignmentProjectionForCall = assignmentProjection;
    probe = std::make_shared<ProviderRequestProbeState>();
    registerProbe(requestIdForCall, probe);
    ndn_service_framework::RequestMessage rejectedRequest;
    ndn::Buffer rejectedPayloadBuffer(
      reinterpret_cast<const std::uint8_t*>(requestPayload.data()),
      requestPayload.size());
    rejectedRequest.setPayload(rejectedPayloadBuffer, rejectedPayloadBuffer.size());
    rejectedRequest.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());
    environment.user().RequestService(
      std::vector<ndn::Name>{providerName}, serviceName, rejectedRequest, 200,
      ndn_service_framework::ServiceUser::AckCandidatesHandler(
        [&, probe, requestIdForCall, assignmentProjectionForCall] (
          const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
          if (candidates.size() != 1U || candidates.front().providerName != providerName) {
            std::lock_guard<std::mutex> lock(probe->mutex);
            probe->failure = "rejection ACK candidate set mismatch";
            return std::vector<ndn_service_framework::AckSelectionCandidate>{};
          }
          {
            std::lock_guard<std::mutex> lock(probe->mutex);
            probe->selectionSeen = true;
          }
          ndn_service_framework::CollaborationAssignmentEnvelope assignmentEnvelope;
          assignmentEnvelope.role = "/Backbone";
          assignmentEnvelope.assignedArtifact = ndn::Name("/spec185/provider/oracle");
          assignmentEnvelope.opaquePayload = ndn::Buffer(
            reinterpret_cast<const std::uint8_t*>(assignmentProjectionForCall.data()),
            assignmentProjectionForCall.size());
          if (!environment.user().setSelectionAssignmentPayloadForRequest(
                requestIdForCall, providerName,
                ndn_service_framework::encodeCollaborationAssignmentEnvelope(
                  assignmentEnvelope))) {
            std::lock_guard<std::mutex> lock(probe->mutex);
            probe->failure = "rejection assignment payload rejected";
            return std::vector<ndn_service_framework::AckSelectionCandidate>{};
          }
          return candidates;
        }),
      500,
      [probe] (const ndn::Name&) {
        std::lock_guard<std::mutex> lock(probe->mutex);
        if (probe->failure.empty()) probe->failure = "request timed out";
        probe->terminal = true;
      },
      [probe] (const ndn_service_framework::ResponseMessage& response) {
        std::lock_guard<std::mutex> lock(probe->mutex);
        if (!response.getStatus() && probe->failure.empty())
          probe->failure = response.getErrorInfo();
        probe->responseSeen = response.getStatus();
        probe->terminal = true;
      }, ndn_service_framework::tlv::FirstResponding, requestIdForCall);
    environment.pumpUntil([&] { return probeDone(probe); });
    BOOST_CHECK(probeRequestPublished(probe));
    BOOST_CHECK(probeSelectionSeen(probe));
    const auto rejectionFailure = probeFailure(probe);
    BOOST_CHECK_MESSAGE(!rejectionFailure.empty(), rejectionFailure);
    const auto rejectedCounters = facade.counters();
    BOOST_CHECK_EQUAL(rejectedCounters.assemblies, counters.assemblies);
    BOOST_CHECK_EQUAL(rejectedCounters.runnersCreated, counters.runnersCreated);
    BOOST_CHECK_EQUAL(runs->load(std::memory_order_relaxed), 1U);
  }

  registration.close();
  BOOST_CHECK(registration.closed());
  // Stop raises the Provider admission fence before the bounded drain.  This
  // is the public lifecycle sequence used when the native Face is owned by
  // the caller's integration environment.
  facade.stop();
  BOOST_REQUIRE(facade.drain(std::chrono::milliseconds(2000)));
}

BOOST_AUTO_TEST_CASE(ProductionAssemblerFetchesCanonicalSourceAfterSelection)
{
  const auto fixture = providerAssemblyFixture();
  BOOST_REQUIRE(!fixture.empty());
  const auto source = providerAssemblyRead(fixture);
  BOOST_REQUIRE(!source.empty());
  const auto sourceDigest = providerAssemblyDigest(source);
  const auto profileDigest = std::string("sha256:") + std::string(64, 'b');
  const auto sourceIdentity = providerAssemblySourceIdentity(source);
  const auto sourceName = ndn::Name("/spec185/provider/assembler/source");
  const auto rootName = ndn::Name("/spec185/provider/assembler/root");
  const auto rootText = std::string(
    "{\"artifactProfileDigest\":\"") + profileDigest +
    "\",\"metadata\":{\"canonicalSourceBytes\":" +
    std::to_string(source.size()) +
    ",\"canonicalSourceDataName\":\"" + sourceName.toUri() +
    "\",\"canonicalSourceDigest\":\"" + sourceDigest +
    "\"},\"modelIdentityDigest\":\"" +
    std::string("sha256:") + std::string(64, 'a') +
    "\",\"modelName\":\"spec175-tiny-causal-lm-v1\","
    "\"schema\":\"ndnsf-di-canonical-model-manifest-v1\","
    "\"state\":\"ACTIVE\"}";
  const std::vector<std::uint8_t> rootPayload(rootText.begin(), rootText.end());
  const auto rootDigest = providerAssemblyDigest(rootPayload);
  auto projection = makeProviderAssemblyProjection(
    rootDigest, profileDigest, sourceIdentity.graphDigest,
    sourceIdentity.initializerDigest);
  projection.canonicalArtifactName = rootName.toUri();
  std::atomic<unsigned> rootFetches{0};
  std::atomic<unsigned> sourceFetches{0};
  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [&] (const ndn::Name& name) -> std::optional<ndn::Buffer> {
    if (name != rootName)
      return std::nullopt;
    rootFetches.fetch_add(1, std::memory_order_relaxed);
    return ndn::Buffer(rootPayload.data(), rootPayload.size());
  };
  fetchers.fetchEncryptedLargeData = [&] (
      const ndn::Name& name, const ndn::Name& service) -> std::optional<ndn::Buffer> {
    if (name != sourceName || service != ndn::Name("/LLM/Qwen"))
      return std::nullopt;
    sourceFetches.fetch_add(1, std::memory_order_relaxed);
    return ndn::Buffer(source.data(), source.size());
  };
  NativeCanonicalOnnxAssemblerOptions options;
  options.cacheDir = (std::filesystem::temp_directory_path() /
                      "spec185-provider-production-assembler").string();
  std::error_code cleanupError;
  std::filesystem::remove_all(options.cacheDir, cleanupError);
  options.providerIdentity = projection.provider;
  options.workerLocation = providerAssemblyWorker();
  options.signManifest = [] (const std::string& manifest) {
    return "spec185-provider-signature-" + providerAssemblyDigest(
      std::vector<std::uint8_t>(manifest.begin(), manifest.end()));
  };
  const auto prepared = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  BOOST_CHECK(std::filesystem::is_regular_file(prepared.path));
  BOOST_CHECK_EQUAL(rootFetches.load(std::memory_order_relaxed), 1U);
  BOOST_CHECK_EQUAL(sourceFetches.load(std::memory_order_relaxed), 1U);
  BOOST_CHECK_EQUAL(prepared.metadata.at("assembledFrom"),
                    "canonical-root-post-selection");
  BOOST_CHECK_EQUAL(prepared.metadata.at("modelManifestDigest"), rootDigest);
  BOOST_CHECK(!prepared.metadata.at("assembledModelDigest").empty());

  const auto missingCacheDir = options.cacheDir + "-missing-source";
  std::filesystem::remove_all(missingCacheDir, cleanupError);
  auto missingSource = fetchers;
  missingSource.fetchEncryptedLargeData = [] (
      const ndn::Name&, const ndn::Name&) -> std::optional<ndn::Buffer> {
    return std::nullopt;
  };
  auto missingOptions = options;
  missingOptions.cacheDir = missingCacheDir;
  BOOST_CHECK_EXCEPTION(
    prepareNativeCanonicalOnnxRole(missingSource, projection, missingOptions),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("DI_CANONICAL_SOURCE_UNAVAILABLE") !=
             std::string::npos;
    });
  std::filesystem::remove_all(missingCacheDir, cleanupError);
  std::filesystem::remove_all(options.cacheDir, cleanupError);
}

BOOST_AUTO_TEST_CASE(ProviderArtifactCachePinsEvictsAndSeparatesIdentity)
{
  ProviderArtifactCache cache(ProviderArtifactCacheConfig{128 * 1024, 1,
                                                           std::chrono::milliseconds(2000)});
  NativeSelectionProjectionV3 projection;
  projection.provider = "/spec185/provider/cache";
  projection.requestId = "/spec185/provider/cache-request";
  projection.assembly.selectedRole = "/Backbone";
  projection.assembly.recipeDigest = "sha256:" + std::string(64, '3');
  projection.assembly.backendAbi = "onnxruntime-cpu-v1";
  projection.assembly.precision = "float32";
  projection.assembly.quantization = "none";
  projection.assembly.protectionEpoch = "epoch-1";
  projection.assembly.maxSourceBytes = 8;
  projection.assembly.maxAssembledBytes = 8;
  NativeRequestControl control;
  control.requestId = projection.requestId;
  control.attempt = 1;
  control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  std::atomic<unsigned> builds{0};
  const auto makeArtifact = [&] (const NativeRequestControl&) {
    builds.fetch_add(1, std::memory_order_relaxed);
    auto artifact = std::make_shared<PreparedProviderArtifact>();
    artifact->encryptedObjectName = "ciphertext/object";
    artifact->ciphertextDigest = "sha256:" + std::string(64, 'a');
    artifact->formatVersion = "test-v1";
    artifact->canonicalMetadataJson = "{\"schema\":\"test\"}";
    artifact->ciphertextBytes = 8;
    auto runner = std::make_shared<NativeModelRunnerSpec>();
    runner->role = "/Backbone";
    runner->kind = "immutable-template";
    return ProviderArtifactCache::BuildResult{std::move(artifact), std::move(runner)};
  };
  ProviderArtifactKey key;
  key.sourceDigest = "sha256:" + std::string(64, '1');
  key.canonicalGraphDigest = "sha256:" + std::string(64, '2');
  key.role = "/Backbone";
  key.recipeDigest = projection.assembly.recipeDigest;
  key.backendAbi = "onnxruntime-cpu-v1";
  key.device = "cpu";
  key.precision = "float32";
  key.quantization = "none";
  key.layoutDigest = "sha256:" + std::string(64, '4');
  key.artifactProfile = "sha256:" + std::string(64, '5');
  key.securityDomain = projection.securityPolicySnapshotDigest;
  key.protectionEpoch = "epoch-1";
  key.protectionIdentity = projection.provider;

  auto first = cache.acquireWithRunner(key, projection, control, makeArtifact);
  BOOST_REQUIRE(first);
  BOOST_CHECK(!first.cacheHit());
  auto second = cache.acquireWithRunner(key, projection, control, makeArtifact);
  BOOST_REQUIRE(second);
  BOOST_CHECK(second.cacheHit());
  BOOST_CHECK_EQUAL(builds.load(std::memory_order_relaxed), 1U);
  BOOST_CHECK_EQUAL(cache.counters().activeLeases, 2U);
  BOOST_CHECK(second.runnerSpec());
  BOOST_CHECK_EQUAL(second.runnerSpec()->role, "/Backbone");

  auto otherRole = key;
  otherRole.role = "/Head";
  auto otherProjection = projection;
  otherProjection.assembly.selectedRole = "/Head";
  BOOST_CHECK_EXCEPTION(cache.acquireWithRunner(otherRole, otherProjection, control, makeArtifact),
                        std::runtime_error,
                        [] (const std::runtime_error& error) {
                          return std::string(error.what()).find("CACHE_BUDGET_EXCEEDED") !=
                                 std::string::npos;
                        });
  second = {};
  first = {};
  BOOST_CHECK_EQUAL(cache.counters().activeLeases, 0U);
  auto replacement = cache.acquireWithRunner(otherRole, otherProjection, control, makeArtifact);
  BOOST_REQUIRE(replacement);
  BOOST_CHECK(!replacement.cacheHit());
  BOOST_CHECK_EQUAL(builds.load(std::memory_order_relaxed), 2U);
  replacement = {};
  cache.stop();
  BOOST_CHECK_EQUAL(cache.counters().activeLeases, 0U);
  BOOST_CHECK_EXCEPTION(cache.acquireWithRunner(key, projection, control, makeArtifact),
                        std::runtime_error,
                        [] (const std::runtime_error& error) {
                          return std::string(error.what()).find("RUNTIME_CLOSED") !=
                                 std::string::npos;
                        });
}

BOOST_AUTO_TEST_CASE(ProtectedArtifactCacheColdHitBindsGrantAndRetainsCiphertext)
{
  ProviderArtifactCache cache(ProviderArtifactCacheConfig{128 * 1024, 2,
                                                           std::chrono::milliseconds(2000)});
  NativeSelectionProjectionV3 projection;
  projection.provider = "/spec185/provider/protected-cache";
  projection.requestId = "/spec185/provider/protected-cache-request";
  projection.hasGrantBinding = true;
  projection.grantName = "/spec185/grant/epoch-1";
  projection.grantDigest = "sha256:" + std::string(64, 'a');
  projection.canonicalArtifactName = "/spec185/protected/root";
  projection.assembly.selectedRole = "/Backbone";
  projection.assembly.modelManifestDigest = "sha256:" + std::string(64, 'b');
  projection.assembly.recipeDigest = "sha256:" + std::string(64, 'c');
  projection.assembly.backendAbi = "onnxruntime-cpu-v1";
  projection.assembly.precision = "float32";
  projection.assembly.quantization = "none";
  projection.assembly.protectionEpoch = "epoch-1";
  projection.assembly.maxSourceBytes = 8;
  projection.assembly.maxAssembledBytes = 8;
  projection.deadlineMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 60'000;
  NativeRequestControl control;
  control.requestId = projection.requestId;
  control.attempt = 1;
  control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  ProviderArtifactKey key;
  key.sourceDigest = "sha256:" + std::string(64, 'd');
  key.canonicalSourceName = "/spec185/protected/source";
  key.canonicalRootName = projection.canonicalArtifactName;
  key.canonicalRootDigest = projection.assembly.modelManifestDigest;
  key.canonicalGraphDigest = "sha256:" + std::string(64, 'e');
  key.role = projection.assembly.selectedRole;
  key.recipeDigest = projection.assembly.recipeDigest;
  key.backendAbi = projection.assembly.backendAbi;
  key.device = "cpu";
  key.precision = projection.assembly.precision;
  key.quantization = projection.assembly.quantization;
  key.protectionEpoch = projection.assembly.protectionEpoch;
  key.protectionIdentity = projection.provider + "|" + projection.grantName + "|" +
    projection.grantDigest;
  std::atomic<unsigned> builds{0};
  const auto build = [&] (const NativeRequestControl&) {
    builds.fetch_add(1, std::memory_order_relaxed);
    auto artifact = std::make_shared<PreparedProviderArtifact>();
    artifact->encryptedObjectName = "protected-ciphertext";
    artifact->ciphertextDigest = "sha256:" + std::string(64, 'f');
    artifact->formatVersion = "test-protected-v1";
    artifact->canonicalMetadataJson = "{\"schema\":\"protected\"}";
    artifact->ciphertext = std::make_shared<const std::vector<std::uint8_t>>(
      std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8});
    artifact->ciphertextBytes = artifact->ciphertext->size();
    auto runner = std::make_shared<NativeModelRunnerSpec>();
    runner->role = projection.assembly.selectedRole;
    runner->kind = "protected-template";
    runner->backend = projection.assembly.backend;
    runner->path = "/must-not-be-cached/plaintext.onnx";
    return ProviderArtifactCache::BuildResult{std::move(artifact), std::move(runner)};
  };
  auto cold = cache.acquireWithRunner(key, projection, control, build);
  BOOST_REQUIRE(cold);
  BOOST_CHECK(!cold.cacheHit());
  BOOST_CHECK(cold->ciphertext);
  auto hot = cache.acquireWithRunner(key, projection, control, build);
  BOOST_REQUIRE(hot);
  BOOST_CHECK(hot.cacheHit());
  BOOST_CHECK(hot.runnerSpec());
  BOOST_CHECK(hot.runnerSpec()->path.empty());
  BOOST_CHECK_EQUAL(builds.load(std::memory_order_relaxed), 1U);
  BOOST_CHECK_EQUAL(cache.counters().templateHits, 1U);

  auto wrongGrant = projection;
  wrongGrant.grantDigest = "sha256:" + std::string(64, '1');
  BOOST_CHECK_EXCEPTION(cache.acquireWithRunner(key, wrongGrant, control, build),
                        std::runtime_error,
                        [] (const std::runtime_error& error) {
                          return std::string(error.what()).find("KEY_MISMATCH") !=
                                 std::string::npos;
                        });
  auto nextEpoch = projection;
  nextEpoch.assembly.protectionEpoch = "epoch-2";
  auto nextKey = key;
  nextKey.protectionEpoch = "epoch-2";
  auto next = cache.acquireWithRunner(nextKey, nextEpoch, control, build);
  BOOST_REQUIRE(next);
  BOOST_CHECK(!next.cacheHit());
  BOOST_CHECK_EQUAL(builds.load(std::memory_order_relaxed), 2U);
  hot = {};
  cold = {};
  next = {};
  cache.stop();
  BOOST_CHECK_EQUAL(cache.counters().activeLeases, 0U);
}

BOOST_AUTO_TEST_CASE(ProtectedArtifactCacheSeparatesIndependentGrantIdentities)
{
  const auto grantOne = std::string("sha256:") + std::string(64, 'a');
  const auto grantTwo = std::string("sha256:") + std::string(64, 'b');
  std::istringstream projectionInput(providerProjection(
    ndn::Name("/spec185/provider/cache-grant"),
    ndn::Name("/spec185/provider/cache-grant-request"),
    std::string("sha256:") + std::string(64, '1'),
    std::string("sha256:") + std::string(64, '2'),
    "epoch-1", "/spec185/grant/one", grantOne));
  auto projection = nativeSelectionProjectionV3FromJson(projectionInput, "/Backbone");
  projection.canonicalArtifactName = "/spec185/cache-grant/root";

  ProviderArtifactCache cache(ProviderArtifactCacheConfig{128 * 1024, 3,
                                                           std::chrono::milliseconds(2000)});
  NativeRequestControl control;
  control.requestId = projection.requestId;
  control.attempt = projection.attempt;
  control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  ProviderArtifactKey key;
  key.sourceDigest = "sha256:" + std::string(64, '3');
  key.canonicalSourceName = "/spec185/cache-grant/source";
  key.canonicalRootName = projection.canonicalArtifactName;
  key.canonicalRootDigest = projection.assembly.modelManifestDigest;
  key.canonicalGraphDigest = projection.assembly.graphDigest;
  key.role = projection.assembly.selectedRole;
  key.candidateDigest = projection.offerDigest;
  key.recipeDigest = projection.assembly.recipeDigest;
  key.backendAbi = projection.assembly.backendAbi;
  key.device = "cpu";
  key.precision = projection.assembly.precision;
  key.quantization = projection.assembly.quantization;
  key.layoutDigest = projection.assembly.layout;
  key.artifactProfile = projection.assembly.artifactProfileDigest;
  key.securityDomain = projection.securityPolicySnapshotDigest;
  key.protectionEpoch = projection.assembly.protectionEpoch;
  key.protectionIdentity = projection.provider + "|" + projection.grantName + "|" +
                           projection.grantDigest;
  std::atomic<unsigned> builds{0};
  const auto build = [&] (const NativeRequestControl&) {
    builds.fetch_add(1, std::memory_order_relaxed);
    auto artifact = std::make_shared<PreparedProviderArtifact>();
    artifact->encryptedObjectName = "protected-ciphertext";
    artifact->ciphertextDigest = "sha256:" + std::string(64, 'c');
    artifact->formatVersion = "test-protected-v1";
    artifact->canonicalMetadataJson = "{\"schema\":\"protected\"}";
    artifact->ciphertextBytes = 8;
    auto runner = std::make_shared<NativeModelRunnerSpec>();
    runner->role = projection.assembly.selectedRole;
    runner->kind = "protected-template";
    return ProviderArtifactCache::BuildResult{std::move(artifact), std::move(runner)};
  };

  auto first = cache.acquireWithRunner(key, projection, control, build);
  BOOST_REQUIRE(first);
  BOOST_CHECK(!first.cacheHit());
  auto sameGrant = cache.acquireWithRunner(key, projection, control, build);
  BOOST_REQUIRE(sameGrant);
  BOOST_CHECK(sameGrant.cacheHit());

  auto independent = projection;
  independent.grantName = "/spec185/grant/two";
  independent.grantDigest = grantTwo;
  auto independentKey = key;
  independentKey.protectionIdentity = independent.provider + "|" + independent.grantName +
                                      "|" + independent.grantDigest;
  auto second = cache.acquireWithRunner(independentKey, independent, control, build);
  BOOST_REQUIRE(second);
  BOOST_CHECK(!second.cacheHit());
  BOOST_CHECK_EQUAL(builds.load(std::memory_order_relaxed), 2U);
  BOOST_CHECK_EQUAL(cache.counters().templateHits, 1U);

  second = {};
  sameGrant = {};
  first = {};
  cache.stop();
  BOOST_CHECK_EQUAL(cache.counters().activeLeases, 0U);
}

BOOST_AUTO_TEST_CASE(ProductionAssemblerCacheColdHitUsesExactArtifact)
{
  const auto fixture = providerAssemblyFixture();
  BOOST_REQUIRE(!fixture.empty());
  const auto source = providerAssemblyRead(fixture);
  BOOST_REQUIRE(!source.empty());
  const auto sourceDigest = providerAssemblyDigest(source);
  const auto profileDigest = std::string("sha256:") + std::string(64, 'b');
  const auto sourceIdentity = providerAssemblySourceIdentity(source);
  const auto sourceName = ndn::Name("/spec185/provider/cache/source");
  const auto rootName = ndn::Name("/spec185/provider/cache/root");
  const auto rootText = std::string(
    "{\"artifactProfileDigest\":\"") + profileDigest +
    "\",\"metadata\":{\"canonicalSourceBytes\":" +
    std::to_string(source.size()) +
    ",\"canonicalSourceDataName\":\"" + sourceName.toUri() +
    "\",\"canonicalSourceDigest\":\"" + sourceDigest +
    "\"},\"modelIdentityDigest\":\"" +
    std::string("sha256:") + std::string(64, 'a') +
    "\",\"modelName\":\"spec175-tiny-causal-lm-v1\","
    "\"schema\":\"ndnsf-di-canonical-model-manifest-v1\","
    "\"state\":\"ACTIVE\"}";
  const std::vector<std::uint8_t> rootPayload(rootText.begin(), rootText.end());
  auto projection = makeProviderAssemblyProjection(
    providerAssemblyDigest(rootPayload), profileDigest, sourceIdentity.graphDigest,
    sourceIdentity.initializerDigest);
  projection.canonicalArtifactName = rootName.toUri();
  const auto cacheDir = (std::filesystem::temp_directory_path() /
                         "spec185-provider-assembler-cache").string();
  std::error_code cleanupError;
  std::filesystem::remove_all(cacheDir, cleanupError);
  NativeCanonicalOnnxFetchers fetchers;
  std::atomic<unsigned> sourceFetches{0};
  fetchers.getArtifact = [rootName, rootPayload] (const ndn::Name& name)
    -> std::optional<ndn::Buffer> {
    if (name != rootName)
      return std::nullopt;
    return ndn::Buffer(rootPayload.data(), rootPayload.size());
  };
  fetchers.fetchEncryptedLargeData = [sourceName, source, &sourceFetches] (
      const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (name != sourceName || service != ndn::Name("/LLM/Qwen"))
      return std::nullopt;
    sourceFetches.fetch_add(1, std::memory_order_relaxed);
    return ndn::Buffer(source.data(), source.size());
  };
  NativeCanonicalOnnxAssemblerOptions options;
  options.cacheDir = cacheDir;
  options.providerIdentity = projection.provider;
  options.workerLocation = providerAssemblyWorker();
  options.signManifest = [] (const std::string&) { return std::string("fixture-signature-v1"); };
  ProviderArtifactCache cache(ProviderArtifactCacheConfig{8ULL * 1024 * 1024, 2,
                                                           std::chrono::milliseconds(30000)});
  NativeRequestControl control;
  control.requestId = projection.requestId;
  control.attempt = 1;
  control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  ProviderArtifactKey key;
  key.sourceDigest = sourceDigest;
  key.canonicalSourceName = sourceName.toUri();
  key.canonicalRootName = projection.canonicalArtifactName;
  key.canonicalRootDigest = projection.assembly.modelManifestDigest;
  key.initializerDigest = projection.assembly.canonicalInitializerDigest;
  key.canonicalGraphDigest = projection.assembly.graphDigest;
  key.role = projection.assembly.selectedRole;
  key.candidateDigest = projection.offerDigest;
  key.recipeDigest = projection.assembly.recipeDigest;
  key.backendAbi = projection.assembly.backendAbi;
  key.device = "cpu";
  key.precision = projection.assembly.precision;
  key.quantization = projection.assembly.quantization;
  key.layoutDigest = projection.assembly.layout;
  key.artifactProfile = projection.assembly.artifactProfileDigest;
  key.protectionEpoch = projection.assembly.protectionEpoch;
  key.protectionIdentity = projection.provider;
  std::atomic<unsigned> builds{0};
  const auto build = [&] (const NativeRequestControl&) {
    builds.fetch_add(1, std::memory_order_relaxed);
    auto built = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
    auto artifact = std::make_shared<PreparedProviderArtifact>();
    artifact->encryptedObjectName = "local-immutable-assembled-artifact";
    artifact->ciphertextDigest = built.metadata.at("assembledModelDigest");
    artifact->formatVersion = "ndnsf-di-native-assembled-artifact-v1";
    artifact->canonicalMetadataJson = "{\"schema\":\"ndnsf-di-provider-artifact-v1\"}";
    artifact->ciphertextBytes = std::filesystem::file_size(built.path);
    return ProviderArtifactCache::BuildResult{
      std::move(artifact), std::make_shared<const NativeModelRunnerSpec>(std::move(built))};
  };
  auto cold = cache.acquireWithRunner(key, projection, control, build);
  BOOST_REQUIRE(cold);
  BOOST_CHECK(!cold.cacheHit());
  auto hot = cache.acquireWithRunner(key, projection, control, build);
  BOOST_REQUIRE(hot);
  BOOST_CHECK(hot.cacheHit());
  BOOST_CHECK_EQUAL(builds.load(std::memory_order_relaxed), 1U);
  BOOST_CHECK_EQUAL(sourceFetches.load(std::memory_order_relaxed), 1U);
  BOOST_CHECK(cold.runnerSpec());
  BOOST_CHECK(hot.runnerSpec());
  BOOST_CHECK(hot.runnerSpec()->path.empty());
  BOOST_CHECK_EQUAL(cache.counters().activeLeases, 2U);
  hot = {};
  cold = {};
  BOOST_CHECK_EQUAL(cache.counters().activeLeases, 0U);
  std::filesystem::remove_all(cacheDir, cleanupError);
}

BOOST_AUTO_TEST_CASE(ProductionAssemblerCacheScansStableRootAndVerifiesFileDigest)
{
  auto projection = makeProviderAssemblyProjection(
    zeroDigest('a'), zeroDigest('b'), zeroDigest('c'), zeroDigest('d'));
  projection.plan.modelName = "spec189-cache-fixture";
  projection.assembly.artifactDigest = zeroDigest('e');
  projection.assembly.protectionEpoch = "plaintext-v1";

  const auto root = std::filesystem::temp_directory_path() /
                    "spec189-stable-provider-cache";
  std::error_code cleanupError;
  std::filesystem::remove_all(root, cleanupError);
  const std::vector<std::uint8_t> modelBytes{'c', 'a', 'c', 'h', 'e', 'd'};
  const auto modelDigest = providerAssemblyDigest(modelBytes);
  const auto roleDirectory = root / "assembled" / "_LLM_Pipeline_Stage_0" /
                             projection.assembly.recipeDigest.substr(7);
  std::filesystem::create_directories(roleDirectory);
  std::ofstream(roleDirectory / "model.onnx", std::ios::binary)
    .write(reinterpret_cast<const char*>(modelBytes.data()),
           static_cast<std::streamsize>(modelBytes.size()));
  std::ofstream(roleDirectory / "manifest.json")
    << "{\"assembledModelDigest\":\"" << modelDigest
    << "\",\"recipeDigest\":\"" << projection.assembly.recipeDigest
    << "\",\"modelManifestDigest\":\"" << projection.assembly.modelManifestDigest
    << "\",\"graphDigest\":\"" << projection.assembly.graphDigest
    << "\",\"canonicalInitializerDigest\":\""
    << projection.assembly.canonicalInitializerDigest
    << "\",\"artifactProfileDigest\":\""
    << projection.assembly.artifactProfileDigest
    << "\",\"role\":\"" << projection.assembly.selectedRole
    << "\",\"roleKind\":\"" << projection.assembly.roleKind
    << "\",\"adapterDescriptorDigest\":\""
    << projection.assembly.adapterDescriptorDigest
    << "\",\"assemblerDescriptorDigest\":\""
    << projection.assembly.assemblerDescriptorDigest
    << "\",\"backendAbi\":\"" << projection.assembly.backendAbi
    << "\",\"precision\":\"" << projection.assembly.precision
    << "\",\"quantization\":\"" << projection.assembly.quantization
    << "\",\"layout\":\"" << projection.assembly.layout
    << "\",\"padding\":\"" << projection.assembly.padding << "\"}";
  NativeCanonicalOnnxAssemblerOptions options;
  options.cacheDir = root.string();
  options.providerIdentity = projection.provider;
  options.verifyCachedArtifactDigest = true;
  std::string progressPhase;
  options.reportProgress = [&progressPhase] (const std::string& phase, double) {
    progressPhase = phase;
  };
  const auto loaded = tryLoadNativeCanonicalOnnxRoleFromCache(projection, options);
  BOOST_REQUIRE(loaded);
  BOOST_CHECK_EQUAL(loaded->path, (roleDirectory / "model.onnx").string());
  BOOST_CHECK_EQUAL(loaded->metadata.at("assembledModelDigest"), modelDigest);
  BOOST_CHECK_EQUAL(loaded->metadata.at("assembledFrom"),
                    "canonical-root-post-selection-cache");
  BOOST_CHECK_EQUAL(progressPhase, "CACHE_HIT");

  std::ofstream(roleDirectory / "model.onnx", std::ios::binary | std::ios::trunc)
    << "tampered";
  BOOST_CHECK(!tryLoadNativeCanonicalOnnxRoleFromCache(projection, options));
  BOOST_CHECK(!std::filesystem::exists(roleDirectory));
  std::filesystem::remove_all(root, cleanupError);
}

BOOST_AUTO_TEST_CASE(ProtectedAssembledCacheRequiresAuthorizedRuntime)
{
  auto projection = makeProviderAssemblyProjection(
    zeroDigest('a'), zeroDigest('b'), zeroDigest('c'), zeroDigest('d'));
  projection.assembly.protectionEpoch = "spec189-protected-v1";

  NativeCanonicalOnnxAssemblerOptions options;
  options.cacheDir = (std::filesystem::temp_directory_path() /
                      "spec189-protected-cache-runtime-guard").string();
  options.cacheCompatibilitySourceDir = options.cacheDir;
  options.providerIdentity = projection.provider;

  BOOST_CHECK_THROW(
    tryLoadNativeCanonicalOnnxRoleFromCache(projection, options),
    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ProtectedAssembledCacheUsesStablePlaintextAfterNewAuthorization)
{
  if (const char* child = std::getenv("NDNSF_SPEC190_PROTECTED_EXEC_CHILD");
      child != nullptr && std::string(child) == "1") {
    const char* rawRoot = std::getenv("NDNSF_SPEC190_PROTECTED_EXEC_ROOT");
    BOOST_REQUIRE(rawRoot != nullptr);
    const std::filesystem::path root(rawRoot);
    const auto readText = [] (const std::filesystem::path& path) {
      std::ifstream input(path, std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>());
    };
    const auto readBytes = [&] (const std::filesystem::path& path) {
      const auto value = readText(path);
      return std::vector<std::uint8_t>(value.begin(), value.end());
    };

    std::istringstream projectionInput(readText(root / "projection.json"));
    const auto childProjection = nativeSelectionProjectionV3FromJson(
      projectionInput, "/LLM/Pipeline/Stage/0");
    const auto providerBootId = readText(root / "provider-boot-id");
    const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

    ProtectedRuntimeBindingV1 binding;
    binding.provider = childProjection.provider;
    binding.role = childProjection.executionRole.roleId;
    binding.requestId = childProjection.requestId;
    binding.attempt = childProjection.attempt;
    binding.planCoreDigest = childProjection.planCoreDigest;
    binding.planDigest = childProjection.planDigest;
    binding.securityPolicySnapshotDigest = childProjection.securityPolicySnapshotDigest;
    binding.protectionEpoch = childProjection.selectedRole.protectionEpoch;
    binding.grantName = childProjection.grantName;
    binding.grantDigest = childProjection.grantDigest;
    binding.providerBootId = providerBootId;
    binding.fencingToken = nativeProtectedFencingToken(
      childProjection, providerBootId, {});
    binding.expiresAtMs = childProjection.deadlineMs;

    NativeProtectedGrantConfig grantConfig;
    grantConfig.authorityIdentity = readText(root / "authority-identity");
    grantConfig.authorityPublicKeyRaw = readText(root / "authority-public-key");
    const auto recipientSeed = readBytes(root / "recipient-seed");
    grantConfig.recipientKey = {
      NativeRecipientKey::Kind::Ed25519Seed,
      std::string(recipientSeed.begin(), recipientSeed.end())};
    grantConfig.modelManifestDigest =
      childProjection.selectedRole.modelManifestDigest;
    const auto grantWire = readText(root / "grant.wire.json");
    grantConfig.fetchGrant = [grantWire] (const std::string&) {
      return grantWire;
    };
    auto runtime = std::make_shared<ProtectedRuntime>(
      binding, std::move(grantConfig));
    runtime->verifyGrant(binding, now);

    NativeCanonicalOnnxAssemblerOptions options;
    options.cacheDir = readText(root / "cache-dir");
    options.providerIdentity = childProjection.provider;
    options.roleAssemblySpecDigest = readText(root / "role-assembly-spec-digest");
    options.protectedRuntime = runtime;
    const auto hot = tryLoadNativeCanonicalOnnxRoleFromCache(
      childProjection, options, readText(root / "source-name"),
      readText(root / "source-digest"));
    BOOST_REQUIRE(hot);
    BOOST_REQUIRE(!hot->path.empty());
    BOOST_CHECK(hot->path.find(readText(root / "cache-dir") + "/assembled/") == 0);
    BOOST_CHECK_EQUAL(hot->metadata.at("protectedCacheHit"), "true");
    BOOST_CHECK_EQUAL(hot->metadata.at("protectedArtifactPersistent"), "true");
    BOOST_CHECK_EQUAL(hot->metadata.at("protectedPlaintextPersistent"), "true");

    const auto plaintext = providerAssemblyRead(hot->path);
    BOOST_CHECK_EQUAL(providerAssemblyDigest(plaintext),
                      hot->metadata.at("assembledModelDigest"));
    runtime->complete();
    std::ofstream(root / "child-success")
      << providerBootId << "\n" << childProjection.grantDigest << "\n"
      << hot->metadata.at("assembledModelDigest") << "\n";
    return;
  }

  const auto fixture = providerAssemblyFixture();
  BOOST_REQUIRE(!fixture.empty());
  const auto source = providerAssemblyRead(fixture);
  BOOST_REQUIRE(!source.empty());
  const auto sourceDigest = providerAssemblyDigest(source);
  const auto profileDigest = zeroDigest('b');
  const auto sourceIdentity = providerAssemblySourceIdentity(source);
  const auto sourceName = ndn::Name("/spec190/provider/protected/source");
  const auto rootName = ndn::Name("/spec190/provider/protected/root");
  const auto rootText = std::string(
    "{\"artifactProfileDigest\":\"") + profileDigest +
    "\",\"metadata\":{\"canonicalSourceBytes\":" +
    std::to_string(source.size()) +
    ",\"canonicalSourceDataName\":\"" + sourceName.toUri() +
    "\",\"canonicalSourceDigest\":\"" + sourceDigest +
    "\"},\"modelIdentityDigest\":\"" + zeroDigest('a') +
    "\",\"modelName\":\"spec190-protected-fixture\","
    "\"schema\":\"ndnsf-di-canonical-model-manifest-v1\","
    "\"state\":\"ACTIVE\"}";
  const std::vector<std::uint8_t> rootPayload(rootText.begin(), rootText.end());
  const auto rootDigest = providerAssemblyDigest(rootPayload);
  auto projection = makeProviderAssemblyProjection(
    rootDigest, profileDigest, sourceIdentity.graphDigest,
    sourceIdentity.initializerDigest);
  projection.canonicalArtifactName = rootName.toUri();
  projection.attempt = 1;
  projection.dataflow.attempt = projection.attempt;
  const auto protectionEpoch = std::string("spec190-protected-v1");
  projection.assembly.protectionEpoch = protectionEpoch;
  projection.selectedRole = projection.assembly;
  const auto recipe = canonicalNativeOnnxRecipeJson(projection.assembly);
  projection.assembly.recipeDigest = providerAssemblyDigest(
    std::vector<std::uint8_t>(recipe.begin(), recipe.end()));
  projection.selectedRole.recipeDigest = projection.assembly.recipeDigest;
  projection.planCoreDigest = projection.planDigest;
  projection.ackClosedDigest = projection.planDigest;
  projection.offerDigest = projection.planDigest;
  projection.securityPolicySnapshotDigest = projection.planDigest;

  const auto requesterIdentity = std::string("/spec190/requester");
  const auto authorityPrivate = makeEd25519Key('a');
  const auto requesterPrivate = makeEd25519Key('b');
  const auto recipientPrivate = makeEd25519Key('c');
  const auto recipientSeed = std::string(32, 'c');
  NativeGrantIssuerConfig issuerConfig;
  issuerConfig.authorityIdentity = "/spec190/authority";
  issuerConfig.requesterIdentity = requesterIdentity;
  issuerConfig.protectionEpoch = protectionEpoch;
  issuerConfig.keyId = "spec190-protected-key";
  issuerConfig.authorityPrivateKey = authorityPrivate;
  issuerConfig.requesterPublicKey = publicKey(requesterPrivate);
  issuerConfig.allowedModelManifests = {rootDigest};
  issuerConfig.recipientPublicKeys.emplace(projection.provider, publicKey(recipientPrivate));
  issuerConfig.contentKey = [] (const std::string&, const std::string&) {
    return std::vector<std::uint8_t>(32, 0x42);
  };
  NativeSignedGrantRequest grantRequest;
  grantRequest.providerIdentity = projection.provider;
  grantRequest.requesterIdentity = requesterIdentity;
  grantRequest.requestId = projection.requestId;
  grantRequest.attempt = projection.attempt;
  grantRequest.planCoreDigest = projection.planCoreDigest;
  grantRequest.grantViewDigest = projection.planDigest;
  grantRequest.modelManifestDigest = rootDigest;
  grantRequest.protectionEpoch = protectionEpoch;
  grantRequest.issuedAtMs = 1;
  const auto now = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto issuedGrant = NativeArtifactGrantIssuer(issuerConfig).issue(
    grantRequest.sign(*requesterPrivate), now, now + 60'000);
  projection.hasGrantBinding = true;
  projection.grantName = issuedGrant.grantName;
  projection.grantDigest = issuedGrant.grantDigest;

  const auto roleAssemblySpecDigest = zeroDigest('e');
  const auto providerBootId = std::string("spec190-protected-boot");
  const auto makeRuntime = [&] {
    ProtectedRuntimeBindingV1 binding;
    binding.provider = projection.provider;
    binding.role = projection.executionRole.roleId;
    binding.requestId = projection.requestId;
    binding.attempt = projection.attempt;
    binding.planCoreDigest = projection.planCoreDigest;
    binding.planDigest = projection.planDigest;
    binding.securityPolicySnapshotDigest = projection.securityPolicySnapshotDigest;
    binding.protectionEpoch = protectionEpoch;
    binding.grantName = projection.grantName;
    binding.grantDigest = projection.grantDigest;
    binding.providerBootId = providerBootId;
    binding.fencingToken = nativeProtectedFencingToken(projection, providerBootId, {});
    binding.expiresAtMs = projection.deadlineMs;
    NativeProtectedGrantConfig grantConfig;
    grantConfig.authorityIdentity = issuerConfig.authorityIdentity;
    grantConfig.authorityPublicKeyRaw = publicKeyBytes(authorityPrivate);
    grantConfig.recipientKey = {NativeRecipientKey::Kind::Ed25519Seed, recipientSeed};
    grantConfig.modelManifestDigest = rootDigest;
    grantConfig.fetchGrant = [wire = issuedGrant.wireJson] (const std::string&) {
      return wire;
    };
    auto runtime = std::make_shared<ProtectedRuntime>(binding, std::move(grantConfig));
    runtime->verifyGrant(binding, now);
    return runtime;
  };

  const auto cacheDir = (std::filesystem::temp_directory_path() /
                         "spec190-protected-assembled-cache").string();
  std::error_code cleanupError;
  std::filesystem::remove_all(cacheDir, cleanupError);
  std::atomic<unsigned> rootFetches{0};
  std::atomic<unsigned> sourceFetches{0};
  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [rootName, rootPayload, &rootFetches] (const ndn::Name& name)
    -> std::optional<ndn::Buffer> {
    if (name != rootName)
      return std::nullopt;
    rootFetches.fetch_add(1, std::memory_order_relaxed);
    return ndn::Buffer(rootPayload.data(), rootPayload.size());
  };
  fetchers.fetchEncryptedLargeData = [sourceName, source, &sourceFetches] (
      const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (name != sourceName || service != ndn::Name("/LLM/Qwen"))
      return std::nullopt;
    sourceFetches.fetch_add(1, std::memory_order_relaxed);
    return ndn::Buffer(source.data(), source.size());
  };
  NativeCanonicalOnnxAssemblerOptions options;
  options.cacheDir = cacheDir;
  options.providerIdentity = projection.provider;
  options.roleAssemblySpecDigest = roleAssemblySpecDigest;
  options.workerLocation = providerAssemblyWorker();
  options.signManifest = [] (const std::string&) {
    return std::string("spec190-protected-signature-v1");
  };
  options.protectedRuntime = makeRuntime();
  const auto cold = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  BOOST_REQUIRE(std::filesystem::is_regular_file(cold.path));
  BOOST_CHECK_EQUAL(rootFetches.load(std::memory_order_relaxed), 1U);
  BOOST_CHECK_EQUAL(sourceFetches.load(std::memory_order_relaxed), 1U);
  BOOST_CHECK_EQUAL(cold.metadata.at("protectedArtifactPersistent"), "true");
  BOOST_CHECK_EQUAL(cold.metadata.at("protectedPlaintextPersistent"), "true");
  BOOST_CHECK_EQUAL(cold.metadata.at("protectedCacheHit"), "false");
  BOOST_CHECK(cold.path.find(cacheDir + "/assembled/") == 0);
  BOOST_CHECK(std::filesystem::is_regular_file(cold.path));
  BOOST_CHECK_EQUAL(
    providerAssemblyDigest(providerAssemblyRead(cold.path)),
    cold.metadata.at("assembledModelDigest"));
  options.protectedRuntime->complete();

  options.protectedRuntime = makeRuntime();
  auto hot = tryLoadNativeCanonicalOnnxRoleFromCache(
    projection, options, sourceName.toUri(), sourceDigest);
  BOOST_REQUIRE(hot);
  BOOST_REQUIRE(!hot->path.empty());
  BOOST_CHECK_EQUAL(hot->path, cold.path);
  BOOST_CHECK_EQUAL(hot->metadata.at("protectedCacheHit"), "true");
  BOOST_CHECK_EQUAL(hot->metadata.at("protectedArtifactPersistent"), "true");
  BOOST_CHECK_EQUAL(hot->metadata.at("protectedPlaintextPersistent"), "true");
  BOOST_CHECK_EQUAL(rootFetches.load(std::memory_order_relaxed), 1U);
  BOOST_CHECK_EQUAL(sourceFetches.load(std::memory_order_relaxed), 1U);

  const auto plaintext = providerAssemblyRead(hot->path);
  BOOST_CHECK_EQUAL(providerAssemblyDigest(plaintext),
                    hot->metadata.at("assembledModelDigest"));
  options.protectedRuntime->complete();

  // A new Provider boot must be able to use the immutable plaintext with a
  // newly issued grant.  The child receives only the current Selection/grant
  // fixture and non-secret cache identity; it does not inherit the parent
  // ProtectedRuntime, content key, plaintext, or request/session KV.
  auto childProjection = projection;
  childProjection.requestId = "/spec190/provider/protected/exec-child";
  childProjection.dataflow.requestId = childProjection.requestId;
  const auto childNow = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  childProjection.deadlineMs = childNow + 60'000;
  auto childGrantRequest = grantRequest;
  childGrantRequest.requestId = childProjection.requestId;
  childGrantRequest.issuedAtMs = 1;
  const auto childGrant = NativeArtifactGrantIssuer(issuerConfig).issue(
    childGrantRequest.sign(*requesterPrivate), childNow, childNow + 60'000);
  childProjection.grantName = childGrant.grantName;
  childProjection.grantDigest = childGrant.grantDigest;

  const auto execRoot = std::filesystem::path(cacheDir + "-exec");
  std::filesystem::remove_all(execRoot, cleanupError);
  std::filesystem::create_directories(execRoot);
  std::filesystem::permissions(
    execRoot, std::filesystem::perms::owner_all,
    std::filesystem::perm_options::replace);
  const auto writeText = [] (const std::filesystem::path& path,
                             const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
      throw std::runtime_error("cannot create protected exec fixture file");
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!output.good())
      throw std::runtime_error("cannot write protected exec fixture file");
  };
  writeText(execRoot / "projection.json",
            nativeSelectionProjectionV3ToJson(childProjection));
  writeText(execRoot / "provider-boot-id", "spec190-protected-boot-after-exec");
  writeText(execRoot / "cache-dir", cacheDir);
  writeText(execRoot / "role-assembly-spec-digest", roleAssemblySpecDigest);
  writeText(execRoot / "source-name", sourceName.toUri());
  writeText(execRoot / "source-digest", sourceDigest);
  writeText(execRoot / "authority-identity", issuerConfig.authorityIdentity);
  writeText(execRoot / "authority-public-key", publicKeyBytes(authorityPrivate));
  writeText(execRoot / "recipient-seed", recipientSeed);
  writeText(execRoot / "grant.wire.json", childGrant.wireJson);

  ScopedEnvironmentVariable childMode(
    "NDNSF_SPEC190_PROTECTED_EXEC_CHILD", "1");
  ScopedEnvironmentVariable childRoot(
    "NDNSF_SPEC190_PROTECTED_EXEC_ROOT", execRoot.string().c_str());
  const auto executable = boost::unit_test::framework::master_test_suite().argv[0];
  BOOST_REQUIRE(executable != nullptr);
  const auto child = ::fork();
  BOOST_REQUIRE(child >= 0);
  if (child == 0) {
    ::execl(executable, executable,
            "--run_test=Spec185ProviderAssembly/Spec188ProviderReferenceAssembly/ProtectedAssembledCacheUsesStablePlaintextAfterNewAuthorization",
            "--log_level=error", static_cast<char*>(nullptr));
    ::_exit(127);
  }
  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR)
      BOOST_FAIL("waitpid failed for protected assembled exec child");
  }
  BOOST_REQUIRE(WIFEXITED(status));
  BOOST_CHECK_EQUAL(WEXITSTATUS(status), 0);
  BOOST_CHECK(std::filesystem::is_regular_file(execRoot / "child-success"));
  std::filesystem::remove_all(execRoot, cleanupError);
  std::filesystem::remove_all(cacheDir, cleanupError);
}

BOOST_AUTO_TEST_CASE(ProtectedSelectionBindingRejectsProviderEpochAndGrantSubstitution)
{
  const auto planDigest = std::string("sha256:") + std::string(64, 'a');
  const auto grantDigest = std::string("sha256:") + std::string(64, 'd');
  const auto text = providerProjection(
    ndn::Name("/spec185/provider"), ndn::Name("/spec185/request"),
    planDigest, std::string("sha256:") + std::string(64, 'b'),
    "spec185-provider-protected-v1", "/spec185/grants/one", grantDigest);
  std::istringstream input(text);
  const auto projection = nativeSelectionProjectionV3FromJson(input, "/Backbone");

  ProtectedRuntimeBindingV1 binding;
  binding.provider = projection.provider;
  binding.role = projection.executionRole.roleId;
  binding.requestId = projection.requestId;
  binding.attempt = projection.attempt;
  binding.planCoreDigest = projection.planCoreDigest;
  binding.planDigest = projection.planDigest;
  binding.securityPolicySnapshotDigest = projection.securityPolicySnapshotDigest;
  binding.protectionEpoch = projection.selectedRole.protectionEpoch;
  binding.grantName = projection.grantName;
  binding.grantDigest = projection.grantDigest;
  binding.providerBootId = "spec185-provider-boot";
  binding.fencingToken = nativeProtectedFencingToken(
    projection, binding.providerBootId, {});
  binding.expiresAtMs = projection.deadlineMs;
  ProtectedRuntime runtime(binding);

  auto expectMismatch = [&] (const NativeSelectionProjectionV3& candidate) {
    const auto error = validateProtectedRuntimeBinding(
      candidate, runtime, {}, binding.providerBootId, binding.fencingToken);
    BOOST_REQUIRE(error);
    BOOST_CHECK_EQUAL(*error, "DI_PROTECTED_RUNTIME_BINDING_MISMATCH");
  };
  auto wrongProvider = projection;
  wrongProvider.provider = "/spec185/other-provider";
  expectMismatch(wrongProvider);
  auto wrongEpoch = projection;
  wrongEpoch.selectedRole.protectionEpoch = "spec185-other-epoch";
  expectMismatch(wrongEpoch);
  auto wrongGrant = projection;
  wrongGrant.grantDigest = std::string("sha256:") + std::string(64, 'e');
  expectMismatch(wrongGrant);
}

BOOST_AUTO_TEST_CASE(ProductionProtectedProviderCacheReusesStableCiphertextAcrossIndependentGrants)
{
  // This fixture deliberately leaves RunnerPreparationFactory empty so the
  // Provider facade executes its production canonical-root/source assembler
  // and ProviderArtifactCache path.  The runner itself remains a deterministic
  // C++ oracle so this selector measures preparation and cache behavior without
  // requiring a local ONNX Runtime model execution.
  ndn_service_framework::test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/Spec185ProviderCache");
  profile.providerRoles = {"/Backbone"};
  profile.deferBridgeDelivery = true;
  profile.providerFacesHaveDedicatedIoWorkers = true;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  environment.user().setUseTokens(false);
  environment.provider().setUseTokens(false);

  const auto serviceName = environment.profile().serviceName;
  const auto providerName = environment.provider().getName();
  const auto requesterName = environment.user().getName();
  const auto authorityPrivate = makeEd25519Key('p');
  const auto requesterPrivate = makeEd25519Key('q');
  const auto recipientPrivate = makeEd25519Key('r');
  const auto providerBootId = environment.provider().getProviderBootEpoch();
  const auto protectionEpoch = std::string("spec185-provider-cache-protected-v1");

  const auto fixture = providerAssemblyFixture();
  BOOST_REQUIRE(!fixture.empty());
  const auto source = providerAssemblyRead(fixture);
  BOOST_REQUIRE(!source.empty());
  const auto sourceDigest = providerAssemblyDigest(source);
  const auto sourceIdentity = providerAssemblySourceIdentity(source);
  const auto profileDigest = std::string("sha256:") + std::string(64, 'b');
  const auto rootName = ndn::Name("/spec185/provider/cache/root");
  const auto sourceDataName = ndn::Name(requesterName)
    .append("NDNSF").append("LARGE-DATA").append(serviceName)
    .append("spec185-provider-cache-source").appendVersion();
  const auto sourceName = sourceDataName;
  const auto rootText = std::string(
    "{\"artifactProfileDigest\":\"") + profileDigest +
    "\",\"metadata\":{\"canonicalSourceBytes\":" +
    std::to_string(source.size()) +
    ",\"canonicalSourceDataName\":\"" + sourceName.toUri() +
    "\",\"canonicalSourceDigest\":\"" + sourceDigest +
    "\"},\"modelIdentityDigest\":\"sha256:" + std::string(64, 'a') +
    "\",\"modelName\":\"spec175-tiny-causal-lm-v1\","
    "\"schema\":\"ndnsf-di-canonical-model-manifest-v1\",\"state\":\"ACTIVE\"}";
  const std::vector<std::uint8_t> rootPayload(rootText.begin(), rootText.end());
  const auto rootDigest = providerAssemblyDigest(rootPayload);

  // Publish one exact-name encrypted source object through the same DummyFace
  // and Segmenter path used by the existing assignment-fetch tests.
  auto publishLarge = [&] (const ndn::Name& dataName,
                           const std::vector<std::uint8_t>& payload) {
    HybridMessageCrypto crypto;
    HybridCryptoCounters counters;
    const auto key = crypto.getOrCreateSendKey(
      serviceName, requesterName, std::string("/SERVICE") + serviceName.toUri(),
      "REQUEST-LARGE", counters);
    const auto associatedDataText = dataName.toUri() + "|REQUEST-LARGE|" +
                                    serviceName.toUri();
    const ndn::Buffer associatedData(
      reinterpret_cast<const std::uint8_t*>(associatedDataText.data()),
      associatedDataText.size());
    const auto encrypted = hybridAesGcmEncrypt(
      key.key,
      ndn::span<const std::uint8_t>(payload.data(), payload.size()),
      ndn::span<const std::uint8_t>(associatedData.data(), associatedData.size()));
    HybridMessageEnvelope envelope;
    envelope.setKeyId(key.keyId);
    envelope.setEpochId(key.epochId);
    envelope.setMessageType("REQUEST-LARGE");
    envelope.setNonce(encrypted.nonce);
    envelope.setCipherText(encrypted.ciphertext);
    envelope.setAuthTag(encrypted.tag);
    const auto wireBlock = envelope.WireEncode();
    ndn::Segmenter segmenter(
      environment.keyChain(), ndn::security::signingWithSha256());
    const auto segments = segmenter.segment(
      ndn::span<const std::uint8_t>(wireBlock.data(), wireBlock.size()),
      dataName, 4096, ndn::time::milliseconds(60000));
    BOOST_REQUIRE(!segments.empty());
    environment.provider().cacheHybridReceiveKeyForTest(
      key.keyId, key.epochId, key.key);
    for (const auto& data : segments) {
      environment.user().cacheDataForTest(*data, ndn::time::milliseconds(60000));
      environment.userFace().put(*data);
    }
  };
  publishLarge(sourceDataName, source);

  struct IssuedGrant
  {
    NativeKeyGrant grant;
    std::uint64_t verificationTimeMs = 0;
  };
  NativeGrantIssuerConfig issuerConfig;
  issuerConfig.authorityIdentity = "/spec185/cache-authority";
  issuerConfig.requesterIdentity = requesterName.toUri();
  issuerConfig.protectionEpoch = protectionEpoch;
  issuerConfig.keyId = "spec185-provider-cache-grant";
  issuerConfig.authorityPrivateKey = authorityPrivate;
  issuerConfig.requesterPublicKey = publicKey(requesterPrivate);
  issuerConfig.allowedModelManifests = {rootDigest};
  issuerConfig.recipientPublicKeys.emplace(providerName.toUri(), publicKey(recipientPrivate));
  issuerConfig.contentKey = [] (const std::string&, const std::string&) {
    return std::vector<std::uint8_t>(32, 0x52);
  };
  const auto issueGrant = [&] (const ndn::Name& requestId) {
    NativeSignedGrantRequest request;
    request.providerIdentity = providerName.toUri();
    request.requesterIdentity = requesterName.toUri();
    request.requestId = requestId.toUri();
    request.attempt = 1;
    request.planCoreDigest = rootDigest;
    request.grantViewDigest = rootDigest;
    request.modelManifestDigest = rootDigest;
    request.protectionEpoch = protectionEpoch;
    request.issuedAtMs = 1;
    const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    return IssuedGrant{NativeArtifactGrantIssuer(issuerConfig).issue(
      request.sign(*requesterPrivate), now, now + 60000), now};
  };

  const auto requestOne = ndn::Name("/spec185-provider-cache-one");
  const auto requestTwo = ndn::Name("/spec185-provider-cache-two");
  const auto firstGrant = issueGrant(requestOne);
  const auto independentGrant = issueGrant(requestTwo);
  BOOST_REQUIRE_NE(firstGrant.grant.grantDigest, independentGrant.grant.grantDigest);
  auto grantWires = std::make_shared<std::map<std::string, std::string>>();
  (*grantWires)[firstGrant.grant.grantDigest] = firstGrant.grant.wireJson;
  (*grantWires)[independentGrant.grant.grantDigest] = independentGrant.grant.wireJson;

  auto runnerRuns = std::make_shared<std::atomic<unsigned>>(0);
  auto runnerFactory = std::make_shared<RegistryNativeModelRunnerFactory>();
  runnerFactory->registerBackend(
    "onnxruntime",
    [runnerRuns] (const NativeModelRunnerSpec& spec) {
      ExecutionEvidence evidence;
      evidence.providerName = spec.metadata.at("evidence.providerName");
      evidence.providerBootId = spec.metadata.at("evidence.providerBootId");
      evidence.evidenceEpoch = 1;
      evidence.runnerKind = RunnerKind::OnnxRuntimeCpu;
      evidence.realCompute = true;
      evidence.deviceKind = "cpu";
      evidence.deviceId = "0";
      evidence.deviceIds = {"0"};
      evidence.runtimeVersion = "spec185-provider-cache-oracle";
      evidence.modelDigest = spec.metadata.at("evidence.modelDigest");
      evidence.planDigest = spec.metadata.at("evidence.planDigest");
      evidence.artifactDigests[spec.role] = spec.metadata.at("evidence.artifactDigest");
      evidence.roles = {spec.role};
      evidence.loadCompleted = true;
      evidence.warmupCompleted = true;
      evidence.createdAtMs = 1;
      evidence.validate();
      return makeNativeModelRunner(
        [runnerRuns] (const RoleExecutionContext&) {
          runnerRuns->fetch_add(1, std::memory_order_relaxed);
          const std::string response = "spec185-provider-cache-response";
          return std::map<std::string, TensorBundle>{{
            "final-response", TensorBundle{
              "final-response",
              std::vector<std::uint8_t>(response.begin(), response.end()),
              1, response.size()}}};
        }, std::move(evidence));
    });

  const auto providerIdentity = environment.keyChain().getPib().getIdentity(providerName);
  const auto providerCertificate = providerIdentity.getDefaultKey().getDefaultCertificate();
  auto protectedRuntimeFactory =
    [grantWires, authorityPublic = publicKeyBytes(authorityPrivate),
     recipientSeed = std::string(32, 'r'), protectionEpoch, providerBootId] (
      ndn_service_framework::ServiceProvider::CollaborationContext&,
      const NativeSelectionProjectionV3& projection,
      const std::shared_ptr<ProviderGroupCoordinator>&) {
      const auto grant = grantWires->find(projection.grantDigest);
      if (grant == grantWires->end())
        throw std::runtime_error("provider cache test grant is not registered");
      ProtectedRuntimeBindingV1 binding;
      binding.provider = projection.provider;
      binding.role = projection.executionRole.roleId;
      binding.requestId = projection.requestId;
      binding.attempt = projection.attempt;
      binding.planCoreDigest = projection.planCoreDigest;
      binding.planDigest = projection.planDigest;
      binding.securityPolicySnapshotDigest = projection.securityPolicySnapshotDigest;
      binding.protectionEpoch = protectionEpoch;
      binding.grantName = projection.grantName;
      binding.grantDigest = projection.grantDigest;
      binding.providerBootId = providerBootId;
      binding.fencingToken = nativeProtectedFencingToken(projection, providerBootId, {});
      binding.expiresAtMs = projection.deadlineMs;
      NativeProtectedGrantConfig grantConfig;
      grantConfig.authorityIdentity = "/spec185/cache-authority";
      grantConfig.authorityPublicKeyRaw = authorityPublic;
      grantConfig.recipientKey = {NativeRecipientKey::Kind::Ed25519Seed, recipientSeed};
      grantConfig.modelManifestDigest = projection.selectedRole.modelManifestDigest;
      grantConfig.fetchGrant = [wire = grant->second] (const std::string&) { return wire; };
      auto runtime = std::make_shared<ProtectedRuntime>(binding, std::move(grantConfig));
      runtime->verifyGrant(binding, static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count()));
      return runtime;
    };

  auto facade = Provider::fromServiceProviderForTest(
    environment.providerFace(), environment.provider(), environment.keyChain(),
    providerCertificate, providerCertificate,
    makeConfig(providerName.toUri(), serviceName.toUri()),
    runnerFactory, {}, std::move(protectedRuntimeFactory));
  environment.enableProductionIngressForTest();
  auto registration = facade.serve({serviceName.toUri(), {"/Backbone"}});
  environment.provider().markHybridResponseKeyWrappedForTest(serviceName);
  const auto ackKey = environment.provider().prepareHybridSendKeyForTest(serviceName, "ACK");
  const auto responseKey = environment.provider().prepareHybridSendKeyForTest(serviceName, "RESPONSE");
  environment.user().cacheHybridReceiveKeyForTest(ackKey.keyId, ackKey.epochId, ackKey.key);
  environment.user().cacheHybridReceiveKeyForTest(responseKey.keyId, responseKey.epochId, responseKey.key);
  const auto selectionKey = environment.user().prepareHybridSendKeyForTest(serviceName, "SELECTION");
  environment.provider().cacheHybridReceiveKeyForTest(selectionKey.keyId, selectionKey.epochId, selectionKey.key);

  struct Probe
  {
    std::mutex mutex;
    bool published = false;
    bool selected = false;
    bool response = false;
    bool terminal = false;
    std::string failure;
  };
  std::mutex probeMutex;
  std::map<std::string, std::shared_ptr<Probe>> probes;
  environment.user().setRequestPublisher(
    [&] (const ndn::Name& requestId,
         const ndn::Name& requestName,
         const std::vector<ndn::Name>& providers,
         const ndn::Name&, const ndn_service_framework::RequestMessage& request, std::size_t) {
      std::shared_ptr<Probe> probe;
      {
        std::lock_guard<std::mutex> lock(probeMutex);
        const auto it = probes.find(requestId.toUri());
        if (it == probes.end()) return;
        probe = it->second;
      }
      if (providers.size() != 1U) {
        std::lock_guard<std::mutex> lock(probe->mutex);
        probe->failure = "provider cache request provider set mismatch";
        return;
      }
      const auto block = request.WireEncode();
      const auto publication = makeProviderPublication(
        requestName, serviceName, requestId, requesterName, "REQUEST",
        ndn::Buffer(block.data(), block.size()));
      environment.provider().cacheHybridReceiveKeyForTest(
        publication.key.keyId, publication.key.epochId, publication.key.key);
      environment.userPubSub().publish(
        requestName,
        ndn::span<const std::uint8_t>(publication.wire.data(), publication.wire.size()));
      std::lock_guard<std::mutex> lock(probe->mutex);
      probe->published = true;
    });
  environment.userPubSub().subscribeToProducer(
    environment.profile().providerNode,
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      if (const auto ack = parseRequestAckNameV2(publication.name)) {
        if (ack->serviceName == serviceName) {
          ndn::Block block(publication.data);
          environment.user().handleRequestAckByName(publication.name, block);
        }
      }
    }, true);

  const auto makeProjection = [&] (const ndn::Name& requestId,
                                   const NativeKeyGrant& grant) {
    auto projection = makeProviderAssemblyProjection(
      rootDigest, profileDigest, sourceIdentity.graphDigest,
      sourceIdentity.initializerDigest);
    projection.provider = providerName.toUri();
    projection.requestId = requestId.toUri();
    projection.attempt = 1;
    projection.plan.serviceName = serviceName.toUri();
    projection.planDigest = rootDigest;
    projection.planCoreDigest = rootDigest;
    projection.ackClosedDigest = rootDigest;
    projection.offerDigest = rootDigest;
    projection.securityPolicySnapshotDigest = rootDigest;
    projection.groupCapabilityV1 = "spec185-provider-cache-capability";
    projection.hasGrantBinding = true;
    projection.grantName = grant.grantName;
    projection.grantDigest = grant.grantDigest;
    projection.assembly.role = "/Backbone";
    projection.assembly.selectedRole = "/Backbone";
    projection.assembly.protectionEpoch = protectionEpoch;
    projection.selectedRole = projection.assembly;
    projection.executionRole.roleId = "/Backbone";
    projection.executionRole.stageId = "/Backbone";
    projection.dataflow.requestId = requestId.toUri();
    projection.dataflow.attempt = projection.attempt;
    projection.dataflow.planDigest = rootDigest;
    projection.dataflow.role = "/Backbone";
    projection.deviceBinding.provider = providerName.toUri();
    projection.deviceBinding.role = "/Backbone";
    projection.deviceBinding.offerDigest = rootDigest;
    projection.plan.roles = {"/Backbone"};
    projection.assembly.recipeDigest.clear();
    const auto recipe = canonicalNativeOnnxRecipeJson(projection.assembly);
    projection.assembly.recipeDigest = providerAssemblyDigest(
      std::vector<std::uint8_t>(recipe.begin(), recipe.end()));
    projection.selectedRole.recipeDigest = projection.assembly.recipeDigest;
    return projection;
  };

  const auto runRequest = [&] (const ndn::Name& requestId,
                               const IssuedGrant& issued,
                               unsigned expectedAssemblies,
                               unsigned expectedSourceFetches,
                               unsigned expectedTemplateHits,
                               unsigned expectedRunners) {
    auto projection = makeProjection(requestId, issued.grant);
    const auto projectionText = nativeSelectionProjectionV3ToJson(projection);
    auto rootDataName = ndn::Name(requesterName);
    rootDataName.append("NDNSF").append("LARGE-DATA").append(serviceName)
      .append(requestId).append("root").appendVersion();
    publishLarge(rootDataName, rootPayload);
    auto probe = std::make_shared<Probe>();
    {
      std::lock_guard<std::mutex> lock(probeMutex);
      probes[requestId.toUri()] = probe;
    }
    ndn_service_framework::RequestMessage request;
    const std::string payload = "provider-cache-input";
    ndn::Buffer payloadBuffer(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    request.setPayload(payloadBuffer, payloadBuffer.size());
    request.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());
    environment.user().RequestService(
      std::vector<ndn::Name>{providerName}, serviceName, request, 200,
      ndn_service_framework::ServiceUser::AckCandidatesHandler(
        [&, probe, requestId, projectionText, rootDataName] (
          const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
          if (candidates.size() != 1U || candidates.front().providerName != providerName) {
            std::lock_guard<std::mutex> lock(probe->mutex);
            probe->failure = "provider cache ACK candidate mismatch";
            return std::vector<ndn_service_framework::AckSelectionCandidate>{};
          }
          ndn_service_framework::CollaborationAssignmentEnvelope envelope;
          envelope.role = "/Backbone";
          envelope.assignedArtifact = rootName;
          envelope.artifactDataName = rootDataName;
          envelope.opaquePayload = ndn::Buffer(
            reinterpret_cast<const std::uint8_t*>(projectionText.data()), projectionText.size());
          if (!environment.user().setSelectionAssignmentPayloadForRequest(
                requestId, providerName,
                ndn_service_framework::encodeCollaborationAssignmentEnvelope(envelope))) {
            std::lock_guard<std::mutex> lock(probe->mutex);
            probe->failure = "provider cache assignment rejected";
            return std::vector<ndn_service_framework::AckSelectionCandidate>{};
          }
          std::lock_guard<std::mutex> lock(probe->mutex);
          probe->selected = true;
          return candidates;
        }),
      4000,
      [probe] (const ndn::Name&) {
        std::lock_guard<std::mutex> lock(probe->mutex);
        if (probe->failure.empty()) probe->failure = "provider cache request timed out";
        probe->terminal = true;
      },
      [probe] (const ndn_service_framework::ResponseMessage& response) {
        std::lock_guard<std::mutex> lock(probe->mutex);
        if (!response.getStatus() && probe->failure.empty())
          probe->failure = response.getErrorInfo();
        probe->response = response.getStatus();
        probe->terminal = true;
      }, ndn_service_framework::tlv::FirstResponding, requestId);
    environment.pumpUntil([&] {
      std::lock_guard<std::mutex> lock(probe->mutex);
      if (!probe->terminal || !probe->response) return false;
      const auto counters = facade.counters();
      return counters.assemblies == expectedAssemblies &&
             counters.sourceFetches == expectedSourceFetches &&
             counters.templateHits == expectedTemplateHits &&
             counters.runnersCreated == expectedRunners;
    });
    {
      std::lock_guard<std::mutex> lock(probe->mutex);
      BOOST_CHECK(probe->published);
      BOOST_CHECK(probe->selected);
      BOOST_CHECK(probe->response);
      BOOST_CHECK_MESSAGE(probe->failure.empty(), probe->failure);
    }
    {
      std::lock_guard<std::mutex> lock(probeMutex);
      probes.erase(requestId.toUri());
    }
  };

  {
    const auto firstProjection = makeProjection(requestOne, firstGrant.grant);
    const auto secondProjection = makeProjection(requestTwo, independentGrant.grant);
    BOOST_REQUIRE(firstProjection.hasGrantBinding);
    BOOST_REQUIRE(secondProjection.hasGrantBinding);
    BOOST_REQUIRE_NE(firstProjection.grantDigest, secondProjection.grantDigest);
    BOOST_REQUIRE_NE(firstProjection.grantName, secondProjection.grantName);
    const auto firstWire = nativeSelectionProjectionV3ToJson(firstProjection);
    const auto secondWire = nativeSelectionProjectionV3ToJson(secondProjection);
    std::istringstream firstInput(firstWire);
    std::istringstream secondInput(secondWire);
    const auto parsedFirst = nativeSelectionProjectionV3FromJson(firstInput, "/Backbone");
    const auto parsedSecond = nativeSelectionProjectionV3FromJson(secondInput, "/Backbone");
    BOOST_REQUIRE(parsedFirst.hasGrantBinding);
    BOOST_REQUIRE(parsedSecond.hasGrantBinding);
    BOOST_REQUIRE_NE(parsedFirst.grantDigest, parsedSecond.grantDigest);
  }

  // Grant verification remains request-bound. The current production order
  // checks the live-runner cache before the immutable Provider artifact
  // template, so the second independent grant reuses the admitted runner
  // instead of creating a second runner or reaching templateHits. The direct
  // ProviderArtifactCache selectors cover the lower template layer separately.
  runRequest(requestOne, firstGrant, 1, 1, 0, 1);
  runRequest(requestTwo, independentGrant, 1, 1, 0, 1);
  BOOST_CHECK_EQUAL(runnerRuns->load(std::memory_order_relaxed), 2U);
  const auto counters = facade.counters();
  BOOST_CHECK_EQUAL(counters.sourceFetches, 1U);
  BOOST_CHECK_EQUAL(counters.assemblies, 1U);
  // Grant verification remains per request, while the live runner is reused
  // across independent grants after the current Selection and residency
  // checks. The artifact-template hit remains a separate lower-layer test.
  BOOST_CHECK_EQUAL(counters.templateHits, 0U);
  BOOST_CHECK_EQUAL(counters.runnersCreated, 1U);
  registration.close();
  facade.stop();
  BOOST_REQUIRE(facade.drain(std::chrono::milliseconds(2000)));
}

BOOST_AUTO_TEST_CASE(NoAuthenticatedSelectionDoesNotFetchOrAssemble)
{
  ProviderArtifactCache cache(ProviderArtifactCacheConfig{
    1024 * 1024, 2, std::chrono::milliseconds(2000)});
  NativeSelectionProjectionV3 projection;
  NativeRequestControl control;
  control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  std::atomic<unsigned> fetches{0};
  std::atomic<unsigned> assemblies{0};
  ProviderArtifactKey key;
  const auto builder = [&] (const NativeRequestControl&) {
    fetches.fetch_add(1, std::memory_order_relaxed);
    assemblies.fetch_add(1, std::memory_order_relaxed);
    auto artifact = std::make_shared<PreparedProviderArtifact>();
    artifact->encryptedObjectName = "must-not-be-built";
    artifact->ciphertextDigest = zeroDigest('a');
    artifact->formatVersion = "spec188-test-v1";
    artifact->canonicalMetadataJson = "{}";
    artifact->ciphertextBytes = 1;
    return ProviderArtifactCache::BuildResult{std::move(artifact), {}};
  };

  // ProviderArtifactCache rejects an unbound request before invoking the
  // fetch/assembly builder.  This is the native no-Selection gate; a caller
  // cannot turn a missing authenticated projection into a source fetch.
  BOOST_CHECK_EXCEPTION(
    cache.acquireWithRunner(key, projection, control, builder),
    std::invalid_argument,
    [] (const std::invalid_argument& error) {
      return std::string(error.what()).find("authenticated projection") !=
             std::string::npos;
    });
  BOOST_CHECK_EQUAL(fetches.load(std::memory_order_relaxed), 0U);
  BOOST_CHECK_EQUAL(assemblies.load(std::memory_order_relaxed), 0U);
  cache.stop();
}

BOOST_AUTO_TEST_CASE(AuthenticatedSelectionFetchesAndVerifiesBeforeAssembly)
{
  const auto fixture = providerAssemblyFixture();
  BOOST_REQUIRE(!fixture.empty());
  const auto source = providerAssemblyRead(fixture);
  BOOST_REQUIRE(!source.empty());
  const auto sourceDigest = providerAssemblyDigest(source);
  const auto profileDigest = std::string("sha256:") + std::string(64, 'b');
  const auto sourceIdentity = providerAssemblySourceIdentity(source);
  const auto sourceName = ndn::Name("/spec188/provider/source");
  const auto rootName = ndn::Name("/spec188/provider/root");
  const auto rootText = std::string(
    "{\"artifactProfileDigest\":\"") + profileDigest +
    "\",\"metadata\":{\"canonicalSourceBytes\":" +
    std::to_string(source.size()) +
    ",\"canonicalSourceDataName\":\"" + sourceName.toUri() +
    "\",\"canonicalSourceDigest\":\"" + sourceDigest +
    "\"},\"modelIdentityDigest\":\"" + zeroDigest('a') +
    "\",\"modelName\":\"spec175-tiny-causal-lm-v1\","
    "\"schema\":\"ndnsf-di-canonical-model-manifest-v1\","
    "\"state\":\"ACTIVE\"}";
  const std::vector<std::uint8_t> rootPayload(rootText.begin(), rootText.end());
  auto projection = makeProviderAssemblyProjection(
    providerAssemblyDigest(rootPayload), profileDigest,
    sourceIdentity.graphDigest, sourceIdentity.initializerDigest);
  projection.canonicalArtifactName = rootName.toUri();

  std::atomic<unsigned> rootFetches{0};
  std::atomic<unsigned> sourceFetches{0};
  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [&] (const ndn::Name& name)
    -> std::optional<ndn::Buffer> {
    if (name != rootName)
      return std::nullopt;
    rootFetches.fetch_add(1, std::memory_order_relaxed);
    return ndn::Buffer(rootPayload.data(), rootPayload.size());
  };
  fetchers.fetchEncryptedLargeData = [&] (
      const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (name != sourceName || service != ndn::Name("/LLM/Qwen"))
      return std::nullopt;
    sourceFetches.fetch_add(1, std::memory_order_relaxed);
    return ndn::Buffer(source.data(), source.size());
  };
  NativeCanonicalOnnxAssemblerOptions options;
  options.cacheDir = (std::filesystem::temp_directory_path() /
                      "spec188-provider-reference-assembly").string();
  options.providerIdentity = projection.provider;
  options.workerLocation = providerAssemblyWorker();
  options.signManifest = [] (const std::string& manifest) {
    return std::string("spec188-provider-signature-") +
      providerAssemblyDigest(std::vector<std::uint8_t>(manifest.begin(), manifest.end()));
  };
  std::error_code cleanupError;
  std::filesystem::remove_all(options.cacheDir, cleanupError);

  const auto prepared = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  BOOST_REQUIRE(std::filesystem::is_regular_file(prepared.path));
  BOOST_CHECK_EQUAL(rootFetches.load(std::memory_order_relaxed), 1U);
  BOOST_CHECK_EQUAL(sourceFetches.load(std::memory_order_relaxed), 1U);
  BOOST_CHECK_EQUAL(prepared.metadata.at("modelManifestDigest"),
                   projection.assembly.modelManifestDigest);
  BOOST_CHECK_EQUAL(prepared.metadata.at("assembledFrom"),
                    "canonical-root-post-selection");

  auto tampered = fetchers;
  tampered.fetchEncryptedLargeData = [source, sourceName] (
      const ndn::Name& name, const ndn::Name& service)
    -> std::optional<ndn::Buffer> {
    if (name != sourceName || service != ndn::Name("/LLM/Qwen"))
      return std::nullopt;
    auto mutated = source;
    mutated.front() ^= 0x01;
    return ndn::Buffer(mutated.data(), mutated.size());
  };
  BOOST_CHECK_EXCEPTION(
    prepareNativeCanonicalOnnxRole(tampered, projection, options),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("SOURCE_DIGEST_MISMATCH") !=
             std::string::npos;
    });
  std::filesystem::remove_all(options.cacheDir, cleanupError);
}

BOOST_AUTO_TEST_CASE(SelectionIdentityChangesRejectProviderArtifactReuse)
{
  auto projection = makeProviderAssemblyProjection(
    zeroDigest('a'), zeroDigest('b'), zeroDigest('c'), zeroDigest('d'));
  projection.canonicalArtifactName = "/spec188/provider/root";
  projection.assembly.selectedRole = "/Backbone";
  projection.assembly.role = "/Backbone";
  projection.assembly.recipeDigest = zeroDigest('e');
  projection.assembly.backendAbi = "onnxruntime-cpu-v1";
  projection.assembly.precision = "float32";
  projection.assembly.quantization = "none";
  projection.assembly.protectionEpoch = "epoch-1";
  projection.assembly.maxSourceBytes = 8;
  projection.assembly.maxAssembledBytes = 8;
  projection.requestId = "/spec188/provider/request";
  NativeRequestControl control;
  control.requestId = projection.requestId;
  control.attempt = 1;
  control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);

  ProviderArtifactKey key;
  key.canonicalRootName = projection.canonicalArtifactName;
  key.canonicalRootDigest = projection.assembly.modelManifestDigest;
  key.role = projection.assembly.selectedRole;
  key.recipeDigest = projection.assembly.recipeDigest;
  key.backendAbi = projection.assembly.backendAbi;
  key.precision = projection.assembly.precision;
  key.quantization = projection.assembly.quantization;
  key.protectionEpoch = projection.assembly.protectionEpoch;
  key.protectionIdentity = projection.provider;
  key.sourceDigest = zeroDigest('f');

  const auto builder = [] (const NativeRequestControl&) {
    auto artifact = std::make_shared<PreparedProviderArtifact>();
    artifact->encryptedObjectName = "spec188-artifact";
    artifact->ciphertextDigest = zeroDigest('1');
    artifact->formatVersion = "spec188-test-v1";
    artifact->canonicalMetadataJson = "{}";
    artifact->ciphertextBytes = 1;
    return ProviderArtifactCache::BuildResult{std::move(artifact), {}};
  };
  ProviderArtifactCache cache(ProviderArtifactCacheConfig{
    1024 * 1024, 2, std::chrono::milliseconds(2000)});
  auto lease = cache.acquireWithRunner(key, projection, control, builder);
  BOOST_REQUIRE(lease);

  auto changed = projection;
  changed.assembly.recipeDigest = zeroDigest('6');
  BOOST_CHECK_EXCEPTION(
    cache.acquireWithRunner(key, changed, control, builder),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("KEY_MISMATCH") != std::string::npos;
    });
  changed = projection;
  changed.assembly.protectionEpoch = "epoch-2";
  BOOST_CHECK_EXCEPTION(
    cache.acquireWithRunner(key, changed, control, builder),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("KEY_MISMATCH") != std::string::npos;
    });
  lease = {};
  cache.stop();
}

BOOST_AUTO_TEST_CASE(ProviderArtifactCacheStopCancelsInFlightBuild)
{
  auto projection = makeProviderAssemblyProjection(
    zeroDigest('a'), zeroDigest('b'), zeroDigest('c'), zeroDigest('d'));
  projection.canonicalArtifactName = "/spec188/provider/lifecycle/root";
  projection.requestId = "/spec188/provider/lifecycle/request";
  NativeRequestControl control;
  control.requestId = projection.requestId;
  control.attempt = 1;
  control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

  ProviderArtifactKey key;
  key.sourceDigest = zeroDigest('e');
  key.canonicalSourceName = "/spec188/provider/lifecycle/source";
  key.canonicalRootName = projection.canonicalArtifactName;
  key.canonicalRootDigest = projection.assembly.modelManifestDigest;
  key.canonicalGraphDigest = projection.assembly.graphDigest;
  key.role = projection.assembly.selectedRole;
  key.recipeDigest = projection.assembly.recipeDigest;
  key.backendAbi = projection.assembly.backendAbi;
  key.device = "cpu";
  key.precision = projection.assembly.precision;
  key.quantization = projection.assembly.quantization;
  key.protectionEpoch = projection.assembly.protectionEpoch;
  key.protectionIdentity = projection.provider;

  ProviderArtifactCache cache(ProviderArtifactCacheConfig{
    4 * 1024 * 1024, 2, std::chrono::milliseconds(5000)});
  std::mutex gateMutex;
  std::condition_variable gateCondition;
  bool builderStarted = false;
  std::string failure;
  std::thread creator([&] {
    try {
      (void)cache.acquireWithRunner(
        key, projection, control,
        [&] (const NativeRequestControl& request) {
          {
            std::lock_guard<std::mutex> lock(gateMutex);
            builderStarted = true;
          }
          gateCondition.notify_all();
          std::unique_lock<std::mutex> lock(gateMutex);
          gateCondition.wait(lock, [&] {
            return request.cancelled && request.cancelled();
          });
          throw std::runtime_error("DI_PROVIDER_ARTIFACT_CANCELLED: stop fence");
          return ProviderArtifactCache::BuildResult{};
        });
    }
    catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(gateMutex);
      failure = error.what();
    }
  });
  bool started = false;
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    started = gateCondition.wait_for(
      lock, std::chrono::seconds(2), [&] { return builderStarted; });
  }

  // stop() is the shared cancellation fence.  Wake the fixture only after
  // the cache has published its stopped state; the creator must then leave
  // without publishing an entry or retaining a lease.
  cache.stop();
  gateCondition.notify_all();
  creator.join();
  BOOST_REQUIRE(started);
  BOOST_CHECK_MESSAGE(
    failure.find("DI_PROVIDER_ARTIFACT_CANCELLED") != std::string::npos ||
      failure.find("RUNTIME_CLOSED") != std::string::npos,
    failure);
  BOOST_CHECK_EQUAL(cache.counters().activeLeases, 0U);
  BOOST_CHECK_EXCEPTION(
    cache.acquireWithRunner(key, projection, control,
      [] (const NativeRequestControl&) {
        return ProviderArtifactCache::BuildResult{};
      }),
    std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("RUNTIME_CLOSED") !=
             std::string::npos;
    });
}

BOOST_AUTO_TEST_CASE(ProviderArtifactCacheStopPreservesActiveLeaseUntilRelease)
{
  auto projection = makeProviderAssemblyProjection(
    zeroDigest('a'), zeroDigest('b'), zeroDigest('c'), zeroDigest('d'));
  projection.canonicalArtifactName = "/spec188/provider/lifecycle/lease-root";
  projection.requestId = "/spec188/provider/lifecycle/lease-request";
  NativeRequestControl control;
  control.requestId = projection.requestId;
  control.attempt = 1;
  control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

  ProviderArtifactKey key;
  key.sourceDigest = zeroDigest('e');
  key.canonicalSourceName = "/spec188/provider/lifecycle/lease-source";
  key.canonicalRootName = projection.canonicalArtifactName;
  key.canonicalRootDigest = projection.assembly.modelManifestDigest;
  key.canonicalGraphDigest = projection.assembly.graphDigest;
  key.role = projection.assembly.selectedRole;
  key.recipeDigest = projection.assembly.recipeDigest;
  key.backendAbi = projection.assembly.backendAbi;
  key.device = "cpu";
  key.precision = projection.assembly.precision;
  key.quantization = projection.assembly.quantization;
  key.protectionEpoch = projection.assembly.protectionEpoch;
  key.protectionIdentity = projection.provider;

  ProviderArtifactCache cache(ProviderArtifactCacheConfig{
    4 * 1024 * 1024, 2, std::chrono::milliseconds(2000)});
  auto lease = cache.acquireWithRunner(
    key, projection, control,
    [] (const NativeRequestControl&) {
      auto artifact = std::make_shared<PreparedProviderArtifact>();
      artifact->encryptedObjectName = "spec188-lifecycle-artifact";
      artifact->ciphertextDigest = zeroDigest('f');
      artifact->formatVersion = "spec188-test-v1";
      artifact->canonicalMetadataJson = "{}";
      artifact->ciphertextBytes = 1;
      return ProviderArtifactCache::BuildResult{std::move(artifact), {}};
    });
  BOOST_REQUIRE(lease);
  BOOST_CHECK_EQUAL(cache.counters().activeLeases, 1U);

  // A stop fence may evict idle entries, but an active request lease keeps its
  // immutable artifact alive until the request releases it.
  cache.stop();
  BOOST_CHECK_EQUAL(cache.counters().activeLeases, 1U);
  BOOST_CHECK(lease);
  lease = {};
  BOOST_CHECK_EQUAL(cache.counters().activeLeases, 0U);
}

BOOST_AUTO_TEST_CASE(MemoryLifecycleAlternatesModelsAndReclaimsEvictedArtifacts)
{
  // This is a native owner probe: every artifact carries a custom deleter so
  // the assertion observes actual immutable source destruction instead of
  // inferring cleanup from cache counters alone.
  std::atomic<unsigned> destroyed{0};
  std::atomic<unsigned> builds{0};
  ProviderArtifactCache cache(ProviderArtifactCacheConfig{
    4 * 1024 * 1024, 1, std::chrono::milliseconds(2000)});

  auto makeIdentity = [] (char tag) {
    auto projection = makeProviderAssemblyProjection(
      zeroDigest(tag), zeroDigest('b'), zeroDigest('c'), zeroDigest('d'));
    projection.canonicalArtifactName =
      "/spec188/provider/memory/root-" + std::string(1, tag);
    projection.requestId =
      "/spec188/provider/memory/request-" + std::string(1, tag);
    ProviderArtifactKey key;
    key.sourceDigest = zeroDigest('e');
    key.canonicalSourceName =
      "/spec188/provider/memory/source-" + std::string(1, tag);
    key.canonicalRootName = projection.canonicalArtifactName;
    key.canonicalRootDigest = projection.assembly.modelManifestDigest;
    key.canonicalGraphDigest = projection.assembly.graphDigest;
    key.role = projection.assembly.selectedRole;
    key.recipeDigest = projection.assembly.recipeDigest;
    key.backendAbi = projection.assembly.backendAbi;
    key.device = "cpu";
    key.precision = projection.assembly.precision;
    key.quantization = projection.assembly.quantization;
    key.protectionEpoch = projection.assembly.protectionEpoch;
    key.protectionIdentity = projection.provider;
    return std::make_pair(std::move(projection), std::move(key));
  };

  auto acquire = [&] (char tag) {
    auto identity = makeIdentity(tag);
    NativeRequestControl control;
    control.requestId = identity.first.requestId;
    control.attempt = 1;
    control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    return cache.acquireWithRunner(
      identity.second, identity.first, control,
      [&destroyed, &builds, tag] (const NativeRequestControl&) {
        builds.fetch_add(1, std::memory_order_relaxed);
        auto bytes = std::shared_ptr<const std::vector<std::uint8_t>>(
          new std::vector<std::uint8_t>(64 * 1024, static_cast<std::uint8_t>(tag)),
          [&destroyed] (const std::vector<std::uint8_t>* value) {
            destroyed.fetch_add(1, std::memory_order_relaxed);
            delete value;
          });
        auto artifact = std::make_shared<PreparedProviderArtifact>();
        artifact->encryptedObjectName = "spec188-memory-artifact-" + std::string(1, tag);
        artifact->ciphertextDigest = zeroDigest(tag);
        artifact->formatVersion = "spec188-memory-v1";
        artifact->canonicalMetadataJson = "{}";
        artifact->ciphertextBytes = bytes->size();
        artifact->ciphertext = std::move(bytes);
        return ProviderArtifactCache::BuildResult{std::move(artifact), {}};
      });
  };

  auto first = acquire('a');
  BOOST_REQUIRE(first);
  first = {};
  auto second = acquire('b');
  BOOST_REQUIRE(second);
  BOOST_CHECK_EQUAL(destroyed.load(std::memory_order_relaxed), 1U);
  second = {};
  auto third = acquire('c');
  BOOST_REQUIRE(third);
  BOOST_CHECK_EQUAL(destroyed.load(std::memory_order_relaxed), 2U);
  BOOST_CHECK_EQUAL(builds.load(std::memory_order_relaxed), 3U);
  third = {};
  cache.stop();
  BOOST_CHECK_EQUAL(destroyed.load(std::memory_order_relaxed), 3U);
  BOOST_CHECK_EQUAL(cache.counters().activeLeases, 0U);
}

BOOST_AUTO_TEST_CASE(ProviderArtifactCacheEvictsDiskBackedProtectedDescriptor)
{
  const auto root = std::filesystem::temp_directory_path() /
                    "spec189-disk-backed-provider-cache";
  std::error_code cleanupError;
  std::filesystem::remove_all(root, cleanupError);
  std::filesystem::create_directories(root);

  auto makeIdentity = [] (char tag) {
    auto projection = makeProviderAssemblyProjection(
      zeroDigest(tag), zeroDigest('b'), zeroDigest('c'), zeroDigest('d'));
    projection.canonicalArtifactName =
      "/spec189/provider/disk/root-" + std::string(1, tag);
    projection.requestId =
      "/spec189/provider/disk/request-" + std::string(1, tag);
    ProviderArtifactKey key;
    key.sourceDigest = zeroDigest('e');
    key.canonicalSourceName =
      "/spec189/provider/disk/source-" + std::string(1, tag);
    key.canonicalRootName = projection.canonicalArtifactName;
    key.canonicalRootDigest = projection.assembly.modelManifestDigest;
    key.canonicalGraphDigest = projection.assembly.graphDigest;
    key.role = projection.assembly.selectedRole;
    key.recipeDigest = projection.assembly.recipeDigest;
    key.backendAbi = projection.assembly.backendAbi;
    key.device = "cpu";
    key.precision = projection.assembly.precision;
    key.quantization = projection.assembly.quantization;
    key.protectionEpoch = projection.assembly.protectionEpoch;
    key.protectionIdentity = projection.provider;
    return std::make_pair(std::move(projection), std::move(key));
  };

  ProviderArtifactCache cache(ProviderArtifactCacheConfig{
    4 * 1024 * 1024, 1, std::chrono::milliseconds(2000)});
  auto first = makeIdentity('a');
  NativeRequestControl firstControl;
  firstControl.requestId = first.first.requestId;
  firstControl.attempt = 1;
  firstControl.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  const auto firstPath = root / "a" / "model.onnx.cipher";
  std::filesystem::create_directories(firstPath.parent_path());
  std::ofstream(firstPath, std::ios::binary) << "cipher-a";
  std::atomic<unsigned> cleanups{0};
  auto build = [&] (const std::filesystem::path& path, char tag) {
    auto artifact = std::make_shared<PreparedProviderArtifact>();
    artifact->encryptedObjectName = "disk-backed";
    artifact->ciphertextDigest = zeroDigest(tag);
    artifact->formatVersion = "spec189-disk-backed-v1";
    artifact->canonicalMetadataJson = "{}";
    artifact->ciphertextBytes = std::filesystem::file_size(path);
    auto runner = std::make_shared<NativeModelRunnerSpec>();
    runner->role = "/LLM/Pipeline/Stage/0";
    runner->kind = "protected-template";
    runner->backend = "onnxruntime";
    runner->metadata["encryptedArtifactPath"] = path.string();
    return ProviderArtifactCache::BuildResult{
      std::move(artifact), std::move(runner), [&cleanups, path] {
        ++cleanups;
        std::error_code ignored;
        std::filesystem::remove_all(path.parent_path(), ignored);
      }};
  };
  auto lease = cache.acquireWithRunner(
    first.second, first.first, firstControl,
    [&] (const NativeRequestControl&) { return build(firstPath, 'a'); });
  BOOST_REQUIRE(lease);
  lease = {};

  auto second = makeIdentity('b');
  NativeRequestControl secondControl = firstControl;
  secondControl.requestId = second.first.requestId;
  const auto secondPath = root / "b" / "model.onnx.cipher";
  std::filesystem::create_directories(secondPath.parent_path());
  std::ofstream(secondPath, std::ios::binary) << "cipher-b";
  auto secondLease = cache.acquireWithRunner(
    second.second, second.first, secondControl,
    [&] (const NativeRequestControl&) { return build(secondPath, 'b'); });
  BOOST_REQUIRE(secondLease);
  BOOST_CHECK_EQUAL(cleanups.load(), 1U);
  BOOST_CHECK(!std::filesystem::exists(firstPath));
  secondLease = {};
  cache.stop();
  BOOST_CHECK_EQUAL(cleanups.load(), 2U);
  BOOST_CHECK(!std::filesystem::exists(secondPath));
}

BOOST_AUTO_TEST_CASE(ProviderArtifactCacheDefersCleanupForStoppedActiveLease)
{
  auto projection = makeProviderAssemblyProjection(
    zeroDigest('a'), zeroDigest('b'), zeroDigest('c'), zeroDigest('d'));
  projection.canonicalArtifactName = "/spec189/provider/deferred/root";
  projection.requestId = "/spec189/provider/deferred/request";
  ProviderArtifactKey key;
  key.sourceDigest = zeroDigest('e');
  key.canonicalSourceName = "/spec189/provider/deferred/source";
  key.canonicalRootName = projection.canonicalArtifactName;
  key.canonicalRootDigest = projection.assembly.modelManifestDigest;
  key.canonicalGraphDigest = projection.assembly.graphDigest;
  key.role = projection.assembly.selectedRole;
  key.recipeDigest = projection.assembly.recipeDigest;
  key.backendAbi = projection.assembly.backendAbi;
  key.device = "cpu";
  key.precision = projection.assembly.precision;
  key.quantization = projection.assembly.quantization;
  key.protectionEpoch = projection.assembly.protectionEpoch;
  key.protectionIdentity = projection.provider;
  NativeRequestControl control;
  control.requestId = projection.requestId;
  control.attempt = 1;
  control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

  const auto path = std::filesystem::temp_directory_path() /
                    "spec189-deferred-cleanup" / "model.onnx.cipher";
  std::error_code ignored;
  std::filesystem::remove_all(path.parent_path(), ignored);
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << "ciphertext";
  unsigned cleanups = 0;
  ProviderArtifactCache cache;
  auto lease = cache.acquireWithRunner(
    key, projection, control,
    [&] (const NativeRequestControl&) {
      auto artifact = std::make_shared<PreparedProviderArtifact>();
      artifact->encryptedObjectName = "deferred";
      artifact->ciphertextDigest = zeroDigest('f');
      artifact->formatVersion = "spec189-deferred-v1";
      artifact->canonicalMetadataJson = "{}";
      artifact->ciphertextBytes = std::filesystem::file_size(path);
      auto runner = std::make_shared<NativeModelRunnerSpec>();
      runner->metadata["encryptedArtifactPath"] = path.string();
      return ProviderArtifactCache::BuildResult{
        std::move(artifact), std::move(runner), [&] {
          ++cleanups;
          std::error_code error;
          std::filesystem::remove_all(path.parent_path(), error);
        }};
    });
  BOOST_REQUIRE(lease);
  cache.stop();
  BOOST_CHECK(std::filesystem::exists(path));
  lease = {};
  BOOST_CHECK_EQUAL(cleanups, 1U);
  BOOST_CHECK(!std::filesystem::exists(path));

  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << "ciphertext-again";
  ProviderArtifactCache cache2;
  auto lease2 = cache2.acquireWithRunner(
    key, projection, control,
    [&] (const NativeRequestControl&) {
      auto artifact = std::make_shared<PreparedProviderArtifact>();
      artifact->encryptedObjectName = "invalidated";
      artifact->ciphertextDigest = zeroDigest('f');
      artifact->formatVersion = "spec189-deferred-v1";
      artifact->canonicalMetadataJson = "{}";
      artifact->ciphertextBytes = std::filesystem::file_size(path);
      auto runner = std::make_shared<NativeModelRunnerSpec>();
      runner->metadata["encryptedArtifactPath"] = path.string();
      return ProviderArtifactCache::BuildResult{
        std::move(artifact), std::move(runner), [&] {
          ++cleanups;
          std::error_code error;
          std::filesystem::remove_all(path.parent_path(), error);
        }};
    });
  cache2.invalidate(key);
  BOOST_CHECK(std::filesystem::exists(path));

  // An invalidated generation must not be returned to a new request while
  // its old lease is still active.  The replacement build gets a distinct
  // immutable path; the old path remains pinned until lease2 is released.
  const auto replacementPath = std::filesystem::temp_directory_path() /
                               "spec189-deferred-cleanup-replacement" /
                               "model.onnx.cipher";
  std::filesystem::remove_all(replacementPath.parent_path(), ignored);
  std::filesystem::create_directories(replacementPath.parent_path());
  std::ofstream(replacementPath, std::ios::binary) << "replacement";
  NativeRequestControl replacementControl = control;
  replacementControl.requestId = projection.requestId + "/replacement";
  unsigned replacementBuilds = 0;
  auto replacementLease = cache2.acquireWithRunner(
    key, projection, replacementControl,
    [&] (const NativeRequestControl&) {
      ++replacementBuilds;
      auto artifact = std::make_shared<PreparedProviderArtifact>();
      artifact->encryptedObjectName = "replacement";
      artifact->ciphertextDigest = zeroDigest('f');
      artifact->formatVersion = "spec189-deferred-v1";
      artifact->canonicalMetadataJson = "{}";
      artifact->ciphertextBytes = std::filesystem::file_size(replacementPath);
      auto runner = std::make_shared<NativeModelRunnerSpec>();
      runner->metadata["encryptedArtifactPath"] = replacementPath.string();
      return ProviderArtifactCache::BuildResult{
        std::move(artifact), std::move(runner), [&] {
          ++cleanups;
          std::error_code error;
          std::filesystem::remove_all(replacementPath.parent_path(), error);
        }};
    });
  BOOST_REQUIRE(replacementLease);
  BOOST_CHECK_EQUAL(replacementBuilds, 1U);
  BOOST_CHECK(std::filesystem::exists(path));
  BOOST_CHECK(std::filesystem::exists(replacementPath));
  replacementLease = {};
  lease2 = {};
  cache2.stop();
  BOOST_CHECK_EQUAL(cleanups, 3U);
  BOOST_CHECK(!std::filesystem::exists(path));
BOOST_CHECK(!std::filesystem::exists(replacementPath));
}

BOOST_AUTO_TEST_CASE(ProtectedPlaintextCacheReusesAndErasesLeasedModel)
{
  const auto root = std::filesystem::temp_directory_path() /
    ("spec190-protected-plaintext-cache-" + std::to_string(::getpid()));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  NativeProtectedPlaintextCache cache(root, 2);
  NativeProtectedPlaintextCacheKey key;
  key.encryptedArtifactDigest = zeroDigest('a');
  key.modelManifestDigest = zeroDigest('b');
  key.graphDigest = zeroDigest('c');
  key.initializerDigest = zeroDigest('d');
  key.role = "/role/P0";
  key.recipeDigest = zeroDigest('e');
  key.backendAbi = "onnxruntime-cpu";
  key.roleAssemblySpecDigest = zeroDigest('f');
  key.keyReferenceDigest = zeroDigest('0');
  NativeRequestControl control;
  control.requestId = "/spec190/cache/request";
  control.attempt = 1;
  control.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  unsigned builds = 0;
  const auto build = [&] (const std::filesystem::path& path) {
    ++builds;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "tiny-onnx";
  };
  auto first = cache.acquire(key, control, 1024, build);
  BOOST_REQUIRE(first);
  BOOST_CHECK(!first.cacheHit());
  const auto cachedPath = first.path();
  BOOST_CHECK(std::filesystem::is_regular_file(cachedPath));
  auto second = cache.acquire(key, control, 1024, build);
  BOOST_REQUIRE(second);
  BOOST_CHECK(second.cacheHit());
  BOOST_CHECK_EQUAL(second.path().string(), cachedPath.string());
  BOOST_CHECK_EQUAL(builds, 1U);
  second = {};
  BOOST_CHECK(std::filesystem::exists(cachedPath));
  cache.stop();
  BOOST_CHECK(std::filesystem::exists(cachedPath));
  first = {};
  BOOST_CHECK(!std::filesystem::exists(cachedPath));
  std::filesystem::remove_all(root, ignored);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()
