#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DependencyWaitScheduler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeStandaloneTokenizer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderGroupCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCatalogModelAdapter.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/InvocationStream.hpp"
#include "ndnsf-integration-fixture.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace ndnsf::di::tests {
namespace {

using namespace ndn_service_framework;

using namespace std::chrono_literals;

NativeExecutionPlan
makePlan()
{
  NativeExecutionPlan plan;
  plan.serviceName = "/LLM/Qwen";
  plan.modelName = "Qwen/Qwen3-0.6B";
  plan.roles = {"stage0", "stage1", "stage2"};
  plan.dependencies.emplace_back(
    std::vector<std::string>{"stage0"}, std::vector<std::string>{"stage1"},
    "epoch-key", "/activation", "/NDNSF-DI/{sessionId}/{producerRole}/{role}/{sequence}",
    2, 16, std::vector<std::string>{"hidden"});
  plan.dependencies.emplace_back(
    std::vector<std::string>{"stage1"}, std::vector<std::string>{"stage2"},
    "epoch-key", "/activation", "/NDNSF-DI/{sessionId}/{producerRole}/{role}/{sequence}",
    1, 8, std::vector<std::string>{"hidden"});
  return plan;
}

class FixtureRequest
{
public:
  void setPayload(std::string value) { m_payload = std::move(value); }
  const std::string& getPayload() const { return m_payload; }
  bool SerializeToString(std::string* output) const
  {
    *output = m_payload;
    return true;
  }
  bool ParseFromArray(const void* data, size_t size)
  {
    m_payload.assign(static_cast<const char*>(data), size);
    return true;
  }

private:
  std::string m_payload;
};

class FixtureResponse
{
public:
  void setLabel(std::string value) { m_label = std::move(value); }
  const std::string& getLabel() const { return m_label; }
  bool SerializeToString(std::string* output) const
  {
    *output = m_label;
    return true;
  }
  bool ParseFromArray(const void* data, size_t size)
  {
    m_label.assign(static_cast<const char*>(data), size);
    return true;
  }

private:
  std::string m_label;
};

struct TestHybridPublication
{
  HybridMessageKey key;
  ndn::Buffer wire;
};

TestHybridPublication
makeTestHybridPublication(const ndn::Name& messageName,
                          const ndn::Name& serviceName,
                          const ndn::Name& requestId,
                          const ndn::Name& senderPrefix,
                          const std::string& messageType,
                          const ndn::Buffer& plaintext)
{
  HybridMessageCrypto crypto;
  HybridCryptoCounters counters;
  const auto accessAttribute = hybridAccessAttributeForName(messageName, serviceName);
  auto key = crypto.getOrCreateSendKey(serviceName, senderPrefix,
                                       accessAttribute, messageType, counters);
  const auto associatedData = hybridAssociatedData(
      messageName, messageType, requestId, serviceName, senderPrefix,
      key.keyId, key.epochId);
  const auto encrypted = hybridAesGcmEncrypt(
      key.key, ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
      ndn::span<const uint8_t>(associatedData.data(), associatedData.size()));

  HybridMessageEnvelope envelope;
  envelope.setKeyId(key.keyId);
  envelope.setEpochId(key.epochId);
  envelope.setMessageType(messageType);
  envelope.setNonce(encrypted.nonce);
  envelope.setCipherText(encrypted.ciphertext);
  envelope.setAuthTag(encrypted.tag);
  const auto wireBlock = envelope.WireEncode();
  return {std::move(key), ndn::Buffer(wireBlock.data(), wireBlock.size())};
}

std::shared_ptr<NativeModelRunnerFactory>
makeNativeIngressTestRunnerFactory(
  std::shared_ptr<std::mutex> observedRolesMutex = nullptr,
  std::shared_ptr<std::set<std::string>> observedRoles = nullptr,
  std::shared_ptr<std::map<std::string, std::map<std::string, std::string>>>
    observedInputs = nullptr,
  std::shared_ptr<std::map<std::string, std::map<std::string, std::string>>>
    observedOutputs = nullptr,
  bool suppressBackboneOutput = false)
{
  auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
  factory->registerBackend(
    "onnxruntime",
    [observedRolesMutex, observedRoles, observedInputs, observedOutputs,
     suppressBackboneOutput] (
      const NativeModelRunnerSpec& spec) {
      const auto metadata = [&spec] (const std::string& key) {
        const auto it = spec.metadata.find(key);
        return it == spec.metadata.end() ? std::string() : it->second;
      };
      ExecutionEvidence evidence;
      evidence.providerName = metadata("test.providerName");
      evidence.providerBootId = metadata("test.providerBootId");
      evidence.evidenceEpoch = 1;
      evidence.runnerKind = RunnerKind::OnnxRuntimeCuda;
      evidence.realCompute = true;
      evidence.deviceKind = "cuda";
      evidence.deviceId = metadata("test.deviceId");
      evidence.deviceIds = {evidence.deviceId};
      evidence.runtimeVersion = "integration-test-ort";
      evidence.modelDigest = "sha256:test-model";
      evidence.planDigest = metadata("test.planDigest");
      evidence.artifactDigests[spec.role] = metadata("test.artifactDigest");
      evidence.roles = {spec.role};
      evidence.loadCompleted = true;
      evidence.warmupCompleted = true;
      evidence.createdAtMs = 1;
      evidence.validate();

      return makeNativeModelRunner(
        [role = spec.role, observedRolesMutex, observedRoles,
         observedInputs, observedOutputs,
         suppressBackboneOutput] (const RoleExecutionContext& context) {
          auto payloadText = [] (const TensorBundle& bundle) {
            return std::string(bundle.payload.begin(), bundle.payload.end());
          };
          if (observedRolesMutex != nullptr) {
            std::lock_guard<std::mutex> lock(*observedRolesMutex);
            if (observedRoles != nullptr) {
              observedRoles->insert(role);
            }
            if (observedInputs != nullptr) {
              for (const auto& input : context.inputsByScope) {
                (*observedInputs)[role][input.first] = payloadText(input.second);
              }
            }
          }

          std::map<std::string, TensorBundle> outputs;
          auto addOutput = [&outputs] (const std::string& scope,
                                       const std::string& text) {
            outputs.emplace(scope, TensorBundle{
              scope,
              std::vector<uint8_t>(text.begin(), text.end()),
              1,
              text.size(),
            });
          };

          // When dataflow observation is requested, make every runner consume
          // the exact upstream scope before producing the next scope.  This
          // prevents a four-role test from passing when roles execute as four
          // independent functions.
          const bool dataflowObserved = observedInputs != nullptr &&
            (role == "/Backbone" || role == "/Head/Shard/0" ||
             role == "/Head/Shard/1" || role == "/Aux" || role == "/Merge");
          if (dataflowObserved) {
            auto requiredInput = [&context, &payloadText] (const char* scope) {
              const auto found = context.inputsByScope.find(scope);
              if (found == context.inputsByScope.end()) {
                throw std::runtime_error(
                  std::string("missing required integration-test input: ") + scope);
              }
              return payloadText(found->second);
            };
            if (role == "/Backbone") {
              if (!suppressBackboneOutput) {
                addOutput("features", "features:" + requiredInput("request-input"));
              }
            }
            else if (role == "/Head/Shard/0") {
              addOutput("detections0",
                        "detections0:" + requiredInput("backbone-to-head0"));
            }
            else if (role == "/Head/Shard/1") {
              addOutput("detections1",
                        "detections1:" + requiredInput("backbone-to-head1"));
            }
            else if (role == "/Aux") {
              addOutput("aux", "aux:" + requiredInput("request-input"));
            }
            else if (role == "/Merge") {
              addOutput("final-response",
                        "native-response:" + requiredInput("head0-to-merge") + "|" +
                        requiredInput("head1-to-merge"));
            }
            else {
              throw std::runtime_error("unexpected integration-test role: " + role);
            }
          }
          else {
            addOutput("final-response", std::string("native-response:") + role);
          }

          if (observedRolesMutex != nullptr && observedOutputs != nullptr) {
            std::lock_guard<std::mutex> lock(*observedRolesMutex);
            for (const auto& output : outputs) {
              (*observedOutputs)[role][output.first] = payloadText(output.second);
            }
          }
          return outputs;
        },
        std::move(evidence));
    });
  return factory;
}

std::shared_ptr<NativeModelRunnerFactory>
makeHybridRedistributionRunnerFactory(
  std::shared_ptr<std::mutex> observedMutex,
  std::shared_ptr<std::set<std::string>> observedRoles)
{
  auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
  factory->registerBackend(
    "onnxruntime",
    [observedMutex, observedRoles] (const NativeModelRunnerSpec& spec) {
      const auto metadata = [&spec] (const std::string& key) {
        const auto it = spec.metadata.find(key);
        return it == spec.metadata.end() ? std::string() : it->second;
      };
      ExecutionEvidence evidence;
      evidence.providerName = metadata("test.providerName");
      evidence.providerBootId = metadata("test.providerBootId");
      evidence.evidenceEpoch = 1;
      evidence.runnerKind = RunnerKind::OnnxRuntimeCpu;
      evidence.realCompute = true;
      evidence.deviceKind = "cpu";
      evidence.deviceId = "0";
      evidence.deviceIds = {"0"};
      evidence.runtimeVersion = "integration-test-hybrid-cpu";
      evidence.modelDigest = "sha256:" + std::string(64, '9');
      evidence.planDigest = metadata("test.planDigest");
      evidence.artifactDigests[spec.role] = metadata("test.artifactDigest");
      evidence.roles = {spec.role};
      evidence.loadCompleted = true;
      evidence.warmupCompleted = true;
      evidence.createdAtMs = 1;
      evidence.validate();

      return makeNativeModelRunner(
        [role = spec.role, observedMutex, observedRoles] (
            const RoleExecutionContext& context) {
          {
            std::lock_guard<std::mutex> lock(*observedMutex);
            observedRoles->insert(role);
          }
          if (role == "S0R0") {
            NamedTensor activation;
            activation.name = "activation-0";
            activation.elementType = TensorElementType::Float32;
            activation.shape = {1, 4};
            const std::array<float, 4> values{{1.0F, 2.0F, 3.0F, 4.0F}};
            activation.payload.resize(sizeof(values));
            std::memcpy(activation.payload.data(), values.data(), sizeof(values));
            return std::map<std::string, TensorBundle>{
              {"boundary-0", makeEncodedTensorBundle("boundary-0", {activation})},
            };
          }

          const auto redistributed = applyCertifiedTensorRedistributions(context);
          if (role == "S1R0" || role == "S1R1") {
            const auto input = redistributed.find("boundary-0");
            if (input == redistributed.end()) {
              throw std::runtime_error("hybrid Stage 1 missing scattered activation");
            }
            auto tensors = decodeTensorBundle(input->second.payload);
            tensors.front().name = "activation-1";
            const auto outputScope = "boundary-1/from/" + role;
            return std::map<std::string, TensorBundle>{
              {outputScope, makeEncodedTensorBundle(outputScope, tensors)},
            };
          }
          if (role == "S2R0") {
            const auto input = redistributed.find("boundary-1");
            if (input == redistributed.end()) {
              throw std::runtime_error("hybrid Stage 2 missing gathered activation");
            }
            const auto tensors = decodeTensorBundle(input->second.payload);
            const auto& activation = findTensor(tensors, "activation-1");
            if (activation.shape != std::vector<std::int64_t>({1, 4}) ||
                activation.payload.size() != 4U * sizeof(float)) {
              throw std::runtime_error("hybrid oracle activation is incomplete");
            }
            std::array<float, 4> values{};
            std::memcpy(values.data(), activation.payload.data(),
                        activation.payload.size());
            const auto total = values[0] + values[1] + values[2] + values[3];
            const auto text = std::string("oracle:") + std::to_string(total);
            return std::map<std::string, TensorBundle>{
              {"final-response",
               TensorBundle{"final-response",
                            std::vector<std::uint8_t>(text.begin(), text.end()),
                            1,
                            text.size()}},
            };
          }
          throw std::runtime_error("unexpected hybrid integration-test role");
        },
        std::move(evidence));
    });
  return factory;
}

std::shared_ptr<NativeModelRunnerFactory>
makeHybrid212RunnerFactory(
  std::shared_ptr<std::mutex> observedMutex,
  std::shared_ptr<std::set<std::string>> observedRoles)
{
  auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
  factory->registerBackend(
    "onnxruntime",
    [observedMutex, observedRoles] (const NativeModelRunnerSpec& spec) {
      const auto metadata = [&spec] (const std::string& key) {
        const auto it = spec.metadata.find(key);
        return it == spec.metadata.end() ? std::string() : it->second;
      };
      ExecutionEvidence evidence;
      evidence.providerName = metadata("test.providerName");
      evidence.providerBootId = metadata("test.providerBootId");
      evidence.evidenceEpoch = 1;
      evidence.runnerKind = RunnerKind::OnnxRuntimeCpu;
      evidence.realCompute = true;
      evidence.deviceKind = "cpu";
      evidence.deviceId = "0";
      evidence.deviceIds = {"0"};
      evidence.runtimeVersion = "integration-test-hybrid-212-cpu";
      evidence.modelDigest = "sha256:" + std::string(64, '8');
      evidence.planDigest = metadata("test.planDigest");
      evidence.artifactDigests[spec.role] = metadata("test.artifactDigest");
      evidence.roles = {spec.role};
      evidence.loadCompleted = true;
      evidence.warmupCompleted = true;
      evidence.createdAtMs = 1;
      evidence.validate();

      return makeNativeModelRunner(
        [role = spec.role, observedMutex, observedRoles] (
            const RoleExecutionContext& context) {
          {
            std::lock_guard<std::mutex> lock(*observedMutex);
            observedRoles->insert(role);
          }
          if (role == "S0R0" || role == "S0R1") {
            NamedTensor activation;
            activation.name = "activation-0";
            activation.elementType = TensorElementType::Float32;
            activation.shape = {1, 2};
            const std::array<float, 2> values = role == "S0R0"
              ? std::array<float, 2>{{1.0F, 2.0F}}
              : std::array<float, 2>{{3.0F, 4.0F}};
            activation.payload.resize(sizeof(values));
            std::memcpy(activation.payload.data(), values.data(), sizeof(values));
            const auto scope = "boundary-0/from/" + role;
            return std::map<std::string, TensorBundle>{
              {scope, makeEncodedTensorBundle(scope, {activation})},
            };
          }

          const auto redistributed = applyCertifiedTensorRedistributions(context);
          if (role == "S1R0") {
            auto tensors = decodeTensorBundle(redistributed.at("boundary-0").payload);
            tensors.front().name = "activation-1";
            return std::map<std::string, TensorBundle>{
              {"boundary-1", makeEncodedTensorBundle("boundary-1", tensors)},
            };
          }
          if (role == "S2R1") {
            const auto tensors = decodeTensorBundle(
              redistributed.at("boundary-1").payload);
            const auto& activation = findTensor(tensors, "activation-1");
            if (activation.shape != std::vector<std::int64_t>({1, 2}) ||
                activation.payload.size() != 2U * sizeof(float)) {
              throw std::runtime_error("hybrid 212 peer shard is incomplete");
            }
            std::array<float, 2> values{};
            std::memcpy(values.data(), activation.payload.data(), activation.payload.size());
            const std::array<float, 1> partial{{values[0] + values[1]}};
            NamedTensor partialTensor;
            partialTensor.name = "partial-sum";
            partialTensor.elementType = TensorElementType::Float32;
            partialTensor.shape = {1};
            partialTensor.payload.resize(sizeof(partial));
            std::memcpy(partialTensor.payload.data(), partial.data(), sizeof(partial));
            return std::map<std::string, TensorBundle>{
              {"boundary-2", makeEncodedTensorBundle("boundary-2", {partialTensor})},
            };
          }
          if (role == "S2R0") {
            const auto localTensors = decodeTensorBundle(
              redistributed.at("boundary-1").payload);
            const auto& localActivation = findTensor(localTensors, "activation-1");
            const auto peerTensors = decodeTensorBundle(
              redistributed.at("boundary-2").payload);
            const auto& peerPartial = findTensor(peerTensors, "partial-sum");
            if (localActivation.shape != std::vector<std::int64_t>({1, 2}) ||
                localActivation.payload.size() != 2U * sizeof(float) ||
                peerPartial.payload.size() != sizeof(float)) {
              throw std::runtime_error("hybrid 212 final aggregation is incomplete");
            }
            std::array<float, 2> local{};
            float peer = 0.0F;
            std::memcpy(local.data(), localActivation.payload.data(),
                        localActivation.payload.size());
            std::memcpy(&peer, peerPartial.payload.data(), sizeof(peer));
            const auto text = std::string("oracle:") +
              std::to_string(local[0] + local[1] + peer);
            return std::map<std::string, TensorBundle>{
              {"final-response",
               TensorBundle{"final-response",
                            std::vector<std::uint8_t>(text.begin(), text.end()),
                            1,
                            text.size()}},
            };
          }
          throw std::runtime_error("unexpected hybrid 212 integration-test role");
        },
        std::move(evidence));
    });
  return factory;
}

struct NativeIngressCaseResult
{
  bool requestObserved = false;
  bool assignmentFetchCompleted = false;
  bool handlerEntered = false;
  bool responseReceived = false;
  bool providerInputMatches = false;
  bool timedOut = false;
  bool statusFailed = false;
  std::string statusMessage;
};

enum class NativeIngressReferenceCase
{
  Inline,
  Valid,
  Missing,
  SizeMismatch,
  Malformed,
};

class ScopedNativeIngressFetchTimeout
{
public:
  explicit ScopedNativeIngressFetchTimeout(bool enabled)
    : m_enabled(enabled)
  {
    if (!m_enabled) {
      return;
    }
    if (const char* previous = std::getenv("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS")) {
      m_hadPrevious = true;
      m_previous = previous;
    }
    // A genuinely missing object otherwise waits through the default 30 s
    // transport budget, which exceeds this fixture's 3 s event-loop pump.
    // Keep the production fallback path intact while bounding this negative
    // oracle to a deterministic local fetch deadline.
    ::setenv("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS", "1000", 1);
  }

  ScopedNativeIngressFetchTimeout(const ScopedNativeIngressFetchTimeout&) = delete;
  ScopedNativeIngressFetchTimeout& operator=(const ScopedNativeIngressFetchTimeout&) = delete;

  ~ScopedNativeIngressFetchTimeout()
  {
    if (!m_enabled) {
      return;
    }
    if (m_hadPrevious) {
      ::setenv("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS", m_previous.c_str(), 1);
    }
    else {
      ::unsetenv("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS");
    }
  }

private:
  bool m_enabled = false;
  bool m_hadPrevious = false;
  std::string m_previous;
};

NativeIngressCaseResult
runNativeIngressCase(
  bool mismatch,
  NativeIngressReferenceCase referenceCase = NativeIngressReferenceCase::Inline)
{
  ScopedNativeIngressFetchTimeout fetchTimeout(
    referenceCase == NativeIngressReferenceCase::Missing);
  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/NativeIngress");
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  const auto providerName = environment.provider().getName();
  const auto requestId = ndn::Name(mismatch
                                   ? "/native-ingress-mismatch"
                                   : "/native-ingress-success");
  const std::string role = "/role";
  const std::string planDigest = "sha256:native-ingress-plan";
  const std::string artifactDigest = "sha256:native-ingress-artifact";
  NativeIngressCaseResult result;

  environment.user().setUseTokens(false);
  environment.provider().setUseTokens(false);

  NativeExecutionPlan plan;
  plan.serviceName = serviceName.toUri();
  plan.modelName = "integration-test-model";
  plan.roles = {role};
  plan.executionPolicy = "DATA_DRIVEN_V2";

  NativeProviderAssignment baseAssignment;
  baseAssignment.providerByRole[role] = providerName.toUri();

  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = role;
  runnerSpec.kind = "onnx-model";
  runnerSpec.backend = "onnxruntime";
  runnerSpec.path = "/integration-test/native-role.onnx";
  runnerSpec.metadata["test.providerName"] = providerName.toUri();
  runnerSpec.metadata["test.providerBootId"] = "native-ingress-provider-boot";
  runnerSpec.metadata["test.planDigest"] = planDigest;
  runnerSpec.metadata["test.artifactDigest"] = artifactDigest;
  // The runner is deliberately a deterministic CUDA-evidence fixture.  It
  // does not claim that this local process has a GPU; Tiger supplies that
  // separate real-ORT/CUDA validation gate.
  runnerSpec.metadata["test.deviceId"] = "0";

  auto observedInputMutex = std::make_shared<std::mutex>();
  auto observedInputs = std::make_shared<
    std::map<std::string, std::map<std::string, std::string>>>();

  NativeProviderHandlerConfig handlerConfig;
  handlerConfig.plan = plan;
  handlerConfig.assignment = baseAssignment;
  handlerConfig.runnerFactory = makeNativeIngressTestRunnerFactory(
    observedInputMutex, nullptr, observedInputs);
  handlerConfig.runnerSpecs = {runnerSpec};
  handlerConfig.localProviderName = providerName.toUri();
  handlerConfig.providerBootId = runnerSpec.metadata["test.providerBootId"];
  handlerConfig.planDigest = planDigest;
  handlerConfig.fetchTimeoutMs = 1000;
  handlerConfig.maxSegmentSize = 64;
  handlerConfig.freshnessMs = 60000;

  auto nativeRuntime = makeNativeProviderCollaborationRuntime(
    std::move(handlerConfig));
  auto nativeHandler = std::move(nativeRuntime.handler);
  std::string observedSelectionDigest;
  environment.provider().addCollaborationHandler(
    serviceName,
    [&, nativeHandler = std::move(nativeHandler)] (
          ServiceProvider::CollaborationContext& context,
          const RequestMessage& request) mutable {
      result.handlerEntered = true;
      observedSelectionDigest = context.assignment().selectionDigest;
      result.assignmentFetchCompleted =
        context.getArtifact(ndn::Name("/artifact/native" )).has_value();
      nativeHandler(context, request);
    });

  // Production subscriptions are installed only after service registration.
  environment.enableProductionIngressForTest();

  // Publish a deterministic REQUEST-LARGE object without relying on NAC-ABE
  // key wrapping.  The Provider receives the exact test key through the
  // existing test-only receive-key hook; SegmentFetcher and AES-GCM still run
  // through the production assignment-preparation path.
  const std::string inputText = referenceCase != NativeIngressReferenceCase::Inline
    ? "native-ingress-repository-input" : "native-ingress-request";
  std::string requestWireText = inputText;
  const std::vector<uint8_t> artifactPayload(12000, 0x5a);
  HybridMessageCrypto artifactCrypto;
  HybridCryptoCounters artifactCounters;
  const auto artifactKey = artifactCrypto.getOrCreateSendKey(
    serviceName,
    requesterName,
    std::string("/SERVICE") + serviceName.toUri(),
    "REQUEST-LARGE",
    artifactCounters);
  ndn::Name encryptedDataName(requesterName);
  encryptedDataName.append("NDNSF").append("LARGE-DATA").append(serviceName);
  encryptedDataName.append(requestId).append("native-assignment-artifact");
  encryptedDataName.appendVersion();
  const auto artifactAdText = encryptedDataName.toUri() + "|REQUEST-LARGE|" +
                              serviceName.toUri();
  const ndn::Buffer artifactAd(
    reinterpret_cast<const uint8_t*>(artifactAdText.data()), artifactAdText.size());
  const auto encryptedArtifact = hybridAesGcmEncrypt(
    artifactKey.key,
    ndn::span<const uint8_t>(artifactPayload.data(), artifactPayload.size()),
    ndn::span<const uint8_t>(artifactAd.data(), artifactAd.size()));
  HybridMessageEnvelope artifactEnvelope;
  artifactEnvelope.setKeyId(artifactKey.keyId);
  artifactEnvelope.setEpochId(artifactKey.epochId);
  artifactEnvelope.setMessageType("REQUEST-LARGE");
  artifactEnvelope.setNonce(encryptedArtifact.nonce);
  artifactEnvelope.setCipherText(encryptedArtifact.ciphertext);
  artifactEnvelope.setAuthTag(encryptedArtifact.tag);
  const auto artifactBlock = artifactEnvelope.WireEncode();
  const ndn::Buffer artifactWire(artifactBlock.data(), artifactBlock.size());
  ndn::Segmenter artifactSegmenter(
    environment.keyChain(), ndn::security::signingWithSha256());
  const auto artifactSegments = artifactSegmenter.segment(
    ndn::span<const uint8_t>(artifactWire.data(), artifactWire.size()),
    encryptedDataName,
    64,
    ndn::time::milliseconds(60000));
  if (artifactSegments.empty()) {
    throw std::runtime_error("native ingress test produced no assignment segments");
  }
  environment.provider().cacheHybridReceiveKeyForTest(
    artifactKey.keyId, artifactKey.epochId, artifactKey.key);
  for (const auto& data : artifactSegments) {
        environment.user().cacheDataForTest(
            *data, ndn::time::milliseconds(60000));
        environment.userFace().put(*data);
  }

  if (referenceCase != NativeIngressReferenceCase::Inline) {
    const std::vector<uint8_t> inputPayload(inputText.begin(), inputText.end());
    ndn::Name inputDataName(requesterName);
    inputDataName.append("NDNSF").append("LARGE-DATA").append(serviceName);
    inputDataName.append(requestId).append("native-request-input");
    inputDataName.appendVersion();
    if (referenceCase == NativeIngressReferenceCase::Valid ||
        referenceCase == NativeIngressReferenceCase::SizeMismatch) {
      const auto inputAdText = inputDataName.toUri() + "|REQUEST-LARGE|" +
                               serviceName.toUri();
      const ndn::Buffer inputAd(
        reinterpret_cast<const uint8_t*>(inputAdText.data()), inputAdText.size());
      const auto encryptedInput = hybridAesGcmEncrypt(
        artifactKey.key,
        ndn::span<const uint8_t>(inputPayload.data(), inputPayload.size()),
        ndn::span<const uint8_t>(inputAd.data(), inputAd.size()));
      HybridMessageEnvelope inputEnvelope;
      inputEnvelope.setKeyId(artifactKey.keyId);
      inputEnvelope.setEpochId(artifactKey.epochId);
      inputEnvelope.setMessageType("REQUEST-LARGE");
      inputEnvelope.setNonce(encryptedInput.nonce);
      inputEnvelope.setCipherText(encryptedInput.ciphertext);
      inputEnvelope.setAuthTag(encryptedInput.tag);
      const auto inputBlock = inputEnvelope.WireEncode();
      const ndn::Buffer inputWire(inputBlock.data(), inputBlock.size());
      const auto inputSegments = artifactSegmenter.segment(
        ndn::span<const uint8_t>(inputWire.data(), inputWire.size()),
        inputDataName,
        64,
        ndn::time::milliseconds(60000));
      if (inputSegments.empty()) {
        throw std::runtime_error("native ingress test produced no input segments");
      }
      for (const auto& data : inputSegments) {
        environment.user().cacheDataForTest(
          *data, ndn::time::milliseconds(60000));
        environment.userFace().put(*data);
      }
    }

    if (referenceCase == NativeIngressReferenceCase::Malformed) {
      requestWireText = "{\"schema\":\"ndnsf-di-request-envelope-v2\"";
    }
    else {
      // The only request bytes on the wire are the reference envelope; the
      // published plaintext is available only through Provider fetch/decrypt.
      const auto declaredSize = referenceCase == NativeIngressReferenceCase::SizeMismatch
        ? inputPayload.size() + 1 : inputPayload.size();
      std::ostringstream referenceEnvelope;
      referenceEnvelope << "{\"input_reference\":{\"dataName\":\""
                        << inputDataName.toUri()
                        << "\",\"plaintextSize\":" << declaredSize
                        << "},\"input_transport\":\"REPO_REF\","
                           "\"schema\":\"ndnsf-di-request-envelope-v2\","
                           "\"schema_version\":2}";
      requestWireText = referenceEnvelope.str();
    }
  }

  environment.providerPubSub().subscribeToProducer(
    environment.profile().userNode,
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      const auto parsed = parseRequestNameV2(publication.name);
      if (parsed && parsed->serviceName.equals(serviceName) &&
          parsed->requestId.equals(requestId)) {
        result.requestObserved = true;
      }
    },
    true);

  environment.user().setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name& requestName,
         const std::vector<ndn::Name>& providers,
         const ndn::Name& publishedServiceName,
         const RequestMessage& request, size_t strategy) {
      if (providers.size() != 1 || providers.front() != providerName ||
          publishedServiceName != serviceName || strategy != tlv::FirstResponding) {
        throw std::runtime_error("native ingress request publisher received invalid request");
      }
      const auto requestBlock = request.WireEncode();
      const auto encrypted = makeTestHybridPublication(
        requestName, serviceName, requestId, requesterName, "REQUEST",
        ndn::Buffer(requestBlock.data(), requestBlock.size()));
      environment.provider().cacheHybridReceiveKeyForTest(
        encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
      environment.userPubSub().publish(
        requestName,
        ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
    });

  // The deterministic fixture delivers ACK/Response blocks through the same
  // in-process SVS producer boundary used by the older production-ingress
  // gates.  Selection itself remains an encrypted production publication.
  environment.userPubSub().subscribeToProducer(
    environment.profile().providerNode,
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      if (const auto parsedAck = parseRequestAckNameV2(publication.name)) {
        if (parsedAck->serviceName.equals(serviceName) &&
            parsedAck->requestId.equals(requestId)) {
          ndn::Block ackBlock(publication.data);
          environment.user().handleRequestAckByName(publication.name, ackBlock);
        }
        return;
      }
      if (const auto parsedResponse = parseResponseNameV2(publication.name)) {
        if (parsedResponse->serviceName.equals(serviceName) &&
            parsedResponse->requestId.equals(requestId)) {
          // The LocalMock fixture deliberately bypasses NAC-ABE wrapping for
          // the deterministic ACK above.  Exercise the real post-Selection
          // response dispatch boundary with the same decoded payload shape,
          // while the separate NAC regressions cover response decryption.
          ResponseMessage responseMessage;
          responseMessage.setStatus(true);
          const std::string responseText = "native-response:/role";
          ndn::Buffer responsePayload(
              reinterpret_cast<const uint8_t*>(responseText.data()),
              responseText.size());
          responseMessage.setPayload(
              responsePayload,
              responseText.size());
          result.responseReceived =
            environment.user().handleDecryptedResponseByName(
              publication.name, responseMessage) || result.responseReceived;
        }
      }
    },
    true);

  RequestMessage request;
  ndn::Buffer requestPayload(
    reinterpret_cast<const uint8_t*>(requestWireText.data()), requestWireText.size());
  request.setPayload(requestPayload, requestPayload.size());
  request.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());

  const auto returnedRequestId = environment.user().RequestService(
    std::vector<ndn::Name>{providerName},
    serviceName,
    request,
    200,
    ServiceUser::AckCandidatesHandler(
      [&] (const std::vector<AckSelectionCandidate>& candidates) {
        if (candidates.size() != 1 ||
            candidates.front().providerName != providerName) {
          throw std::runtime_error("native ingress test received invalid ACK candidates");
        }
        CollaborationAssignmentEnvelope assignment;
        assignment.role = role;
        assignment.assignedArtifact = ndn::Name("/artifact/native");
        const auto expectedDevice = mismatch ? "cuda:1" : "cuda:0";
        const auto text = std::string("role=") + role +
                          ";backend=onnxruntime;device=" + expectedDevice +
                          ";artifactDigest=" + artifactDigest +
                          ";artifactDataName=" +
                        encryptedDataName.toUri() + ";";
        assignment.opaquePayload = ndn::Buffer(
          reinterpret_cast<const uint8_t*>(text.data()), text.size());
        BOOST_REQUIRE(environment.user().setSelectionAssignmentPayloadForRequest(
          requestId, providerName, encodeCollaborationAssignmentEnvelope(assignment)));
        return candidates;
      }),
    // Leave enough global deadline for assignment segmentation, native
    // preparation, and one SVS response publication round.
    3000,
    [&] (const ndn::Name&) { result.timedOut = true; },
    [&] (const ResponseMessage& response) {
      result.responseReceived = response.getStatus();
    },
    tlv::FirstResponding,
    requestId);
  if (returnedRequestId != requestId) {
    throw std::runtime_error("native ingress test request ID was not preserved");
  }

  environment.pumpUntil([&] {
    return result.requestObserved || result.timedOut;
  });
  if (!result.requestObserved ||
      environment.provider().getPendingRequestCountForTesting() != 1) {
    throw std::runtime_error("native ingress test did not reach provider pending request");
  }

  RequestAckMessage ack;
  ack.setStatus(true);
  ack.setMessage("native-ingress-ack");
  const auto ackName = makeRequestAckNameV2(
    providerName, requesterName, serviceName, requestId);
  const auto ackBlock = ack.WireEncode();
  environment.providerPubSub().publish(
    ackName,
    ndn::span<const uint8_t>(ackBlock.data(), ackBlock.size()));
  if (!environment.user().handleRequestAckByName(ackName, ackBlock)) {
    throw std::runtime_error("native ingress test ACK did not match pending request");
  }

  environment.pumpUntil([&] {
    if (!observedSelectionDigest.empty()) {
      const auto status = environment.provider().getSelectionExecutionStatus(
          observedSelectionDigest);
      if (status && status->state == SelectionExecutionState::Failed) {
        result.statusFailed = true;
        result.statusMessage = status->message;
      }
      else if (status &&
               status->state == SelectionExecutionState::Completed) {
        // The provider lifecycle is the authoritative local boundary for
        // final Response publication.  The LocalMock NAC-ABE fixture may not
        // decrypt that publication on the user side, which is covered by the
        // dedicated authorization regressions.
        result.responseReceived = true;
      }
    }
    return result.responseReceived || result.statusFailed || result.timedOut;
  });
  {
    std::lock_guard<std::mutex> lock(*observedInputMutex);
    const auto roleIt = observedInputs->find(role);
    if (roleIt != observedInputs->end()) {
      const auto inputIt = roleIt->second.find("request-input");
      result.providerInputMatches = inputIt != roleIt->second.end() &&
        inputIt->second == inputText;
    }
  }
  return result;
}

BOOST_AUTO_TEST_SUITE(Spec170NativePostSelection)

/**
 * Native post-Selection production gate.  Unlike the older structural D2a
 * test, this path publishes an encrypted Selection whose assignment points
 * at a segmented REQUEST-LARGE object.  Provider preparation must fetch and
 * reassemble that object before NativeProviderHandler validates the role's
 * execution evidence and publishes the final Response.
 */
BOOST_AUTO_TEST_CASE(ProductionIngressRunsNativePostSelectionAssignmentFetch)
{
  const auto result = runNativeIngressCase(false);
  BOOST_CHECK(result.requestObserved);
  BOOST_CHECK(result.assignmentFetchCompleted);
  BOOST_CHECK(result.handlerEntered);
  BOOST_CHECK(result.responseReceived);
  BOOST_CHECK(!result.statusFailed);
  BOOST_CHECK(!result.timedOut);
}

BOOST_AUTO_TEST_CASE(ProductionIngressRunsNativeRepositoryReferenceIntoProvider)
{
  const auto result = runNativeIngressCase(
    false, NativeIngressReferenceCase::Valid);
  BOOST_CHECK(result.requestObserved);
  BOOST_CHECK(result.assignmentFetchCompleted);
  BOOST_CHECK(result.handlerEntered);
  BOOST_CHECK(result.providerInputMatches);
  BOOST_CHECK(result.responseReceived);
  BOOST_CHECK(!result.statusFailed);
  BOOST_CHECK(!result.timedOut);
}

BOOST_AUTO_TEST_CASE(ProductionIngressRejectsMissingNativeRepositoryReference)
{
  const auto result = runNativeIngressCase(
    false, NativeIngressReferenceCase::Missing);
  BOOST_CHECK(result.requestObserved);
  BOOST_CHECK(result.assignmentFetchCompleted);
  BOOST_CHECK(result.handlerEntered);
  BOOST_CHECK(!result.providerInputMatches);
  BOOST_CHECK(result.statusFailed);
  BOOST_CHECK(result.statusMessage.find(
    "failed to fetch native DI request input reference") != std::string::npos);
  BOOST_CHECK(!result.responseReceived);
  BOOST_CHECK(!result.timedOut);
}

BOOST_AUTO_TEST_CASE(ProductionIngressRejectsNativeRepositoryReferenceSizeMismatch)
{
  const auto result = runNativeIngressCase(
    false, NativeIngressReferenceCase::SizeMismatch);
  BOOST_CHECK(result.requestObserved);
  BOOST_CHECK(result.assignmentFetchCompleted);
  BOOST_CHECK(result.handlerEntered);
  BOOST_CHECK(!result.providerInputMatches);
  BOOST_CHECK(result.statusFailed);
  BOOST_CHECK_EQUAL(result.statusMessage,
                    "native DI request input plaintext size mismatch");
  BOOST_CHECK(!result.responseReceived);
  BOOST_CHECK(!result.timedOut);
}

BOOST_AUTO_TEST_CASE(ProductionIngressRejectsMalformedNativeRepositoryEnvelope)
{
  const auto result = runNativeIngressCase(
    false, NativeIngressReferenceCase::Malformed);
  BOOST_CHECK(result.requestObserved);
  BOOST_CHECK(result.assignmentFetchCompleted);
  BOOST_CHECK(result.handlerEntered);
  BOOST_CHECK(!result.providerInputMatches);
  BOOST_CHECK(result.statusFailed);
  BOOST_CHECK(result.statusMessage.find(
    "malformed native DI request envelope") != std::string::npos);
  BOOST_CHECK(!result.responseReceived);
  BOOST_CHECK(!result.timedOut);
}

/** The same wire path must expose a deterministic runtime mismatch instead
 * of silently waiting for a Response forever. */
BOOST_AUTO_TEST_CASE(ProductionIngressReportsNativeDeviceMismatch)
{
  const auto result = runNativeIngressCase(true);
  BOOST_CHECK(result.requestObserved);
  BOOST_CHECK(result.assignmentFetchCompleted);
  BOOST_CHECK(result.handlerEntered);
  BOOST_CHECK(result.statusFailed);
  BOOST_CHECK_EQUAL(result.statusMessage, "DI_RUNTIME_DEVICE_MISMATCH");
  BOOST_CHECK(!result.responseReceived);
}

struct NativeMultiRoleIngressCaseResult
{
  bool requestObserved = false;
  bool selectionPublished = false;
  bool assignmentFetchCompleted = false;
  bool handlerEntered = false;
  bool responsePublished = false;
  bool timedOut = false;
  bool statusFailed = false;
  std::string statusMessage;
  std::set<std::string> runnerRoles;
  std::map<std::string, std::map<std::string, std::string>> inputsByRole;
  std::map<std::string, std::map<std::string, std::string>> outputsByRole;
};

/**
 * Full same-Provider D2a-shaped production gate.  This intentionally uses
 * the canonical opaque assignment set emitted when one Provider owns several
 * roles.  The Provider must enter through the real Selection subscription,
 * prepare the first assignment artifact through SegmentFetcher, dispatch the
 * NativeProviderHandler, run all four role runners, and publish a Response.
 * The runner is deterministic, but the assignment/request/response lifecycle
 * is not mocked or entered through a post-decryption callback.
 */
NativeMultiRoleIngressCaseResult
runNativeMultiRoleIngressCase(bool suppressBackboneOutput = false)
{
  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/NativeMultiRoleIngress");
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  const auto providerName = environment.provider().getName();
  const auto requestId = ndn::Name("/native-multi-role-success");
  const std::vector<std::string> roles{
    "/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"};
  const std::string planDigest = "sha256:native-multi-role-plan";
  const std::string artifactDataDigest = "sha256:native-multi-role-artifact";
  NativeMultiRoleIngressCaseResult result;

  environment.user().setUseTokens(false);
  environment.provider().setUseTokens(false);

  NativeExecutionPlan plan;
  plan.serviceName = serviceName.toUri();
  plan.modelName = "integration-test-multi-role-model";
  plan.roles = roles;
  plan.executionPolicy = "DATA_DRIVEN_V2";
  plan.dependencies = {
    NativeDependencySpec{
      {"/Backbone"}, {"/Head/Shard/0"}, "backbone-to-head0", "/activation",
      "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/"
      "{producerRole}/bundle/{sequence}",
      1, 256, {"features"}},
    NativeDependencySpec{
      {"/Backbone"}, {"/Head/Shard/1"}, "backbone-to-head1", "/activation",
      "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/"
      "{producerRole}/bundle/{sequence}",
      1, 256, {"features"}},
    NativeDependencySpec{
      {"/Head/Shard/0"}, {"/Merge"}, "head0-to-merge", "/activation",
      "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/"
      "{producerRole}/bundle/{sequence}",
      1, 128, {"detections0"}},
    NativeDependencySpec{
      {"/Head/Shard/1"}, {"/Merge"}, "head1-to-merge", "/activation",
      "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/"
      "{producerRole}/bundle/{sequence}",
      1, 128, {"detections1"}},
  };

  NativeProviderAssignment baseAssignment;
  for (const auto& role : roles) {
    baseAssignment.providerByRole[role] = providerName.toUri();
  }

  std::vector<NativeModelRunnerSpec> runnerSpecs;
  runnerSpecs.reserve(roles.size());
  for (size_t index = 0; index < roles.size(); ++index) {
    NativeModelRunnerSpec spec;
    spec.role = roles[index];
    spec.kind = "onnx-model";
    spec.backend = "onnxruntime";
    spec.path = "/integration-test/native-multi-role.onnx";
    spec.metadata["test.providerName"] = providerName.toUri();
    spec.metadata["test.providerBootId"] = "native-multi-role-provider-boot";
    spec.metadata["test.planDigest"] = planDigest;
    spec.metadata["test.artifactDigest"] =
      "sha256:native-multi-role-" + std::to_string(index);
    spec.metadata["test.deviceId"] = std::to_string(index < 2 ? 0 : 1);
    runnerSpecs.push_back(std::move(spec));
  }

  auto observedRolesMutex = std::make_shared<std::mutex>();
  auto observedRoles = std::make_shared<std::set<std::string>>();
  auto observedInputs = std::make_shared<
    std::map<std::string, std::map<std::string, std::string>>>();
  auto observedOutputs = std::make_shared<
    std::map<std::string, std::map<std::string, std::string>>>();
  NativeProviderHandlerConfig handlerConfig;
  handlerConfig.plan = plan;
  handlerConfig.assignment = baseAssignment;
  handlerConfig.runnerFactory = makeNativeIngressTestRunnerFactory(
    observedRolesMutex, observedRoles, observedInputs, observedOutputs,
    suppressBackboneOutput);
  handlerConfig.runnerSpecs = runnerSpecs;
  handlerConfig.localProviderName = providerName.toUri();
  handlerConfig.providerBootId = "native-multi-role-provider-boot";
  handlerConfig.planDigest = planDigest;
  handlerConfig.workerCount = 4;
  handlerConfig.fetchTimeoutMs = 1000;
  handlerConfig.maxSegmentSize = 64;
  handlerConfig.freshnessMs = 60000;

  auto nativeRuntime = makeNativeProviderCollaborationRuntime(
    std::move(handlerConfig));
  auto nativeHandler = std::move(nativeRuntime.handler);
  std::string observedSelectionDigest;
  environment.provider().addCollaborationHandler(
    serviceName,
    [&, nativeHandler = std::move(nativeHandler)] (
          ServiceProvider::CollaborationContext& context,
          const RequestMessage& request) mutable {
      result.handlerEntered = true;
      observedSelectionDigest = context.assignment().selectionDigest;
      result.assignmentFetchCompleted =
        context.getArtifact(ndn::Name("/artifact").append(roles.front())).has_value();
      nativeHandler(context, request);
    });

  environment.enableProductionIngressForTest();
  environment.provider().markHybridResponseKeyWrappedForTest(serviceName);

  // Publish one segmented REQUEST-LARGE object for the first envelope.  The
  // canonical assignment set is still used for all four roles; the remaining
  // role metadata is retained in the Selection and exercised by the native
  // local-plan runner set.
  const std::vector<uint8_t> artifactPayload(12000, 0x6b);
  HybridMessageCrypto artifactCrypto;
  HybridCryptoCounters artifactCounters;
  const auto artifactKey = artifactCrypto.getOrCreateSendKey(
    serviceName,
    requesterName,
    std::string("/SERVICE") + serviceName.toUri(),
    "REQUEST-LARGE",
    artifactCounters);
  ndn::Name encryptedDataName(requesterName);
  encryptedDataName.append("NDNSF").append("LARGE-DATA").append(serviceName);
  encryptedDataName.append(requestId).append("native-multi-role-artifact");
  encryptedDataName.appendVersion();
  const auto artifactAdText = encryptedDataName.toUri() + "|REQUEST-LARGE|" +
                              serviceName.toUri();
  const ndn::Buffer artifactAd(
    reinterpret_cast<const uint8_t*>(artifactAdText.data()), artifactAdText.size());
  const auto encryptedArtifact = hybridAesGcmEncrypt(
    artifactKey.key,
    ndn::span<const uint8_t>(artifactPayload.data(), artifactPayload.size()),
    ndn::span<const uint8_t>(artifactAd.data(), artifactAd.size()));
  HybridMessageEnvelope artifactEnvelope;
  artifactEnvelope.setKeyId(artifactKey.keyId);
  artifactEnvelope.setEpochId(artifactKey.epochId);
  artifactEnvelope.setMessageType("REQUEST-LARGE");
  artifactEnvelope.setNonce(encryptedArtifact.nonce);
  artifactEnvelope.setCipherText(encryptedArtifact.ciphertext);
  artifactEnvelope.setAuthTag(encryptedArtifact.tag);
  const auto artifactBlock = artifactEnvelope.WireEncode();
  const ndn::Buffer artifactWire(artifactBlock.data(), artifactBlock.size());
  ndn::Segmenter artifactSegmenter(
    environment.keyChain(), ndn::security::signingWithSha256());
  const auto artifactSegments = artifactSegmenter.segment(
    ndn::span<const uint8_t>(artifactWire.data(), artifactWire.size()),
    encryptedDataName,
    64,
    ndn::time::milliseconds(60000));
  BOOST_REQUIRE(!artifactSegments.empty());
  environment.provider().cacheHybridReceiveKeyForTest(
    artifactKey.keyId, artifactKey.epochId, artifactKey.key);
  for (const auto& data : artifactSegments) {
    environment.user().cacheDataForTest(
      *data, ndn::time::milliseconds(60000));
    environment.userFace().put(*data);
  }

  environment.providerPubSub().subscribeToProducer(
    environment.profile().userNode,
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      const auto parsed = parseRequestNameV2(publication.name);
      if (parsed && parsed->serviceName.equals(serviceName) &&
          parsed->requestId.equals(requestId)) {
        result.requestObserved = true;
      }
    },
    true);

  // Request publication is real and encrypted; only the test receive key is
  // seeded so this gate remains independent of controller bootstrap timing.
  environment.user().setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name& requestName,
         const std::vector<ndn::Name>& providers,
         const ndn::Name& publishedServiceName,
         const RequestMessage& request, size_t strategy) {
      BOOST_REQUIRE_EQUAL(providers.size(), 1U);
      BOOST_CHECK_EQUAL(providers.front(), providerName);
      BOOST_CHECK_EQUAL(publishedServiceName, serviceName);
      BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
      const auto requestBlock = request.WireEncode();
      const auto encrypted = makeTestHybridPublication(
        requestName, serviceName, requestId, requesterName, "REQUEST",
        ndn::Buffer(requestBlock.data(), requestBlock.size()));
      environment.provider().cacheHybridReceiveKeyForTest(
        encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
      environment.userPubSub().publish(
        requestName,
        ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
    });

  // Observe the Provider's actual encrypted Response publication.  We do not
  // fabricate a decrypted Response; publication is the production boundary
  // that the previous tests failed to assert.
  auto observeResponse = [&] (
      const ndn::svs::SVSPubSub::SubscriptionData& publication) {
    const auto parsed = parseResponseNameV2(publication.name);
    if (!parsed || !parsed->serviceName.equals(serviceName) ||
        !parsed->requestId.equals(requestId)) {
      return;
    }
    ndn::Block envelopeBlock(publication.data);
    HybridMessageEnvelope envelope;
    result.responsePublished = envelope.WireDecode(envelopeBlock) &&
                               envelope.getMessageType() == "RESPONSE" &&
                               !envelope.getCipherText().empty();
  };
  auto providerSvsNode = environment.profile().providerNode;
  providerSvsNode.append("0");
  environment.providerPubSub().subscribeToProducer(
    providerSvsNode, observeResponse, true);
  environment.userPubSub().subscribeToProducer(
    providerSvsNode, observeResponse, true);

  RequestMessage request;
  const std::string requestText = "native-multi-role-request";
  ndn::Buffer requestPayload(
    reinterpret_cast<const uint8_t*>(requestText.data()), requestText.size());
  request.setPayload(requestPayload, requestPayload.size());
  request.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());

  const auto returnedRequestId = environment.user().RequestService(
    std::vector<ndn::Name>{providerName},
    serviceName,
    request,
    500,
    ServiceUser::AckCandidatesHandler(
      [&] (const std::vector<AckSelectionCandidate>& candidates) {
        BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
        BOOST_CHECK_EQUAL(candidates.front().providerName, providerName);

        std::vector<ndn::Buffer> assignmentItems;
        assignmentItems.reserve(roles.size());
        for (size_t index = 0; index < roles.size(); ++index) {
          CollaborationAssignmentEnvelope assignment;
          assignment.role = roles[index];
          assignment.assignedArtifact = ndn::Name("/artifact").append(roles[index]);
          const auto roleDigest =
            "sha256:native-multi-role-" + std::to_string(index);
          const auto device = index < 2 ? "cuda:0" : "cuda:1";
          std::string assignmentText =
            "role=" + roles[index] +
            ";backend=onnxruntime;device=" + device +
            ";artifactDigest=" + roleDigest + ";";
          if (index == 0) {
            assignmentText += "artifactDataName=" + encryptedDataName.toUri() + ";";
          }
          assignment.opaquePayload = ndn::Buffer(
            reinterpret_cast<const uint8_t*>(assignmentText.data()),
            assignmentText.size());
          assignmentItems.push_back(
            encodeCollaborationAssignmentEnvelope(assignment));
        }

        BOOST_REQUIRE(
          environment.user().setSelectionAssignmentPayloadForRequest(
            requestId, providerName,
            encodeOpaqueAssignmentSet(assignmentItems)));
        result.selectionPublished = true;
        return candidates;
      }),
    3000,
    [&] (const ndn::Name&) { result.timedOut = true; },
    [&] (const ResponseMessage&) {},
    tlv::FirstResponding,
    requestId);
  BOOST_REQUIRE_EQUAL(returnedRequestId, requestId);

  environment.pumpUntil([&] {
    return result.requestObserved || result.timedOut;
  });
  BOOST_REQUIRE(result.requestObserved);
  BOOST_REQUIRE_EQUAL(environment.provider().getPendingRequestCountForTesting(), 1U);

  RequestAckMessage ack;
  ack.setStatus(true);
  ack.setMessage("native-multi-role-ack");
  const auto ackName = makeRequestAckNameV2(
    providerName, requesterName, serviceName, requestId);
  const auto ackBlock = ack.WireEncode();
  environment.providerPubSub().publish(
    ackName,
    ndn::span<const uint8_t>(ackBlock.data(), ackBlock.size()));
  BOOST_REQUIRE(environment.user().handleRequestAckByName(ackName, ackBlock));

  environment.pumpUntil([&] {
    if (!observedSelectionDigest.empty()) {
      const auto status = environment.provider().getSelectionExecutionStatus(
        observedSelectionDigest);
      if (status && status->state == SelectionExecutionState::Failed) {
        result.statusFailed = true;
        result.statusMessage = status->message;
      }
    }
    return result.responsePublished || result.statusFailed || result.timedOut;
  });
  {
    std::lock_guard<std::mutex> lock(*observedRolesMutex);
    result.runnerRoles = *observedRoles;
    result.inputsByRole = *observedInputs;
    result.outputsByRole = *observedOutputs;
  }
  return result;
}

BOOST_AUTO_TEST_CASE(ProductionIngressRunsNativeFourRoleTwoDeviceAssignmentRequestResponse)
{
  const auto result = runNativeMultiRoleIngressCase();
  BOOST_TEST_MESSAGE("multi-role result: handler=" << result.handlerEntered
                    << " assignmentFetch=" << result.assignmentFetchCompleted
                    << " responsePublished=" << result.responsePublished
                    << " statusFailed=" << result.statusFailed
                    << " statusMessage=" << result.statusMessage
                    << " timedOut=" << result.timedOut
                    << " runnerRoles=" << result.runnerRoles.size());
  BOOST_CHECK(result.requestObserved);
  BOOST_CHECK(result.selectionPublished);
  BOOST_CHECK(result.assignmentFetchCompleted);
  BOOST_CHECK(result.handlerEntered);
  BOOST_CHECK(result.responsePublished);
  BOOST_CHECK(!result.statusFailed);
  BOOST_CHECK(!result.timedOut);
  BOOST_CHECK_EQUAL(result.runnerRoles.size(), 4U);
  BOOST_CHECK(result.runnerRoles.count("/Backbone") != 0);
  BOOST_CHECK(result.runnerRoles.count("/Head/Shard/0") != 0);
  BOOST_CHECK(result.runnerRoles.count("/Head/Shard/1") != 0);
  BOOST_CHECK(result.runnerRoles.count("/Merge") != 0);
  BOOST_REQUIRE_EQUAL(result.inputsByRole.at("/Backbone").size(), 1U);
  BOOST_CHECK_EQUAL(result.inputsByRole.at("/Backbone").at("request-input"),
                    "native-multi-role-request");
  BOOST_REQUIRE_EQUAL(result.inputsByRole.at("/Head/Shard/0").size(), 1U);
  BOOST_CHECK_EQUAL(
    result.inputsByRole.at("/Head/Shard/0").at("backbone-to-head0"),
    "features:native-multi-role-request");
  BOOST_REQUIRE_EQUAL(result.inputsByRole.at("/Head/Shard/1").size(), 1U);
  BOOST_CHECK_EQUAL(
    result.inputsByRole.at("/Head/Shard/1").at("backbone-to-head1"),
    "features:native-multi-role-request");
  BOOST_REQUIRE_EQUAL(result.inputsByRole.at("/Merge").size(), 2U);
  BOOST_CHECK_EQUAL(
    result.inputsByRole.at("/Merge").at("head0-to-merge"),
    "detections0:features:native-multi-role-request");
  BOOST_CHECK_EQUAL(
    result.inputsByRole.at("/Merge").at("head1-to-merge"),
    "detections1:features:native-multi-role-request");
  BOOST_CHECK_EQUAL(
    result.outputsByRole.at("/Merge").at("final-response"),
    "native-response:detections0:features:native-multi-role-request|"
    "detections1:features:native-multi-role-request");
}

BOOST_AUTO_TEST_CASE(MissingBackboneOutputFailsBeforeGlobalRequestTimeout)
{
  const auto result = runNativeMultiRoleIngressCase(true);
  BOOST_CHECK(result.requestObserved);
  BOOST_CHECK(result.selectionPublished);
  BOOST_CHECK(result.assignmentFetchCompleted);
  BOOST_CHECK(result.handlerEntered);
  BOOST_CHECK(!result.responsePublished);
  BOOST_CHECK(result.statusFailed);
  BOOST_CHECK(!result.timedOut);
  BOOST_CHECK_EQUAL(
    result.statusMessage,
    "runner did not publish output scope: backbone-to-head0");
  BOOST_REQUIRE_EQUAL(result.runnerRoles.size(), 1U);
  BOOST_CHECK(result.runnerRoles.count("/Backbone") != 0);
}

BOOST_AUTO_TEST_SUITE_END()

class SameProviderMultiRoleSelection final : public ParticipantSelectionPolicy
{
public:
  std::vector<SelectedParticipant>
  select(const std::vector<AckCandidate>& candidates,
         const std::vector<CollaborationRoleSpec>& roles) const override
  {
    if (candidates.size() != 1 || roles.empty()) {
      return {};
    }

    const auto& candidate = candidates.front();
    std::vector<SelectedParticipant> selected;
    selected.reserve(roles.size());
    for (const auto& role : roles) {
      const auto payloadText = std::string("opaque-role=") + role.role + ";";
      ndn::Buffer payload(
          reinterpret_cast<const uint8_t*>(payloadText.data()), payloadText.size());
      selected.push_back(SelectedParticipant{
          role.role,
          role.service,
          candidate.providerName,
          role.requiredArtifact,
          false,
          0,
          std::move(payload),
          candidate,
          {}});
    }
    return selected;
  }
};

class OneRolePerProviderSelection final : public ParticipantSelectionPolicy
{
public:
  std::vector<SelectedParticipant>
  select(const std::vector<AckCandidate>& candidates,
         const std::vector<CollaborationRoleSpec>& roles) const override
  {
    if (candidates.size() < roles.size() || roles.empty()) {
      return {};
    }

    std::vector<const AckCandidate*> ordered;
    ordered.reserve(candidates.size());
    for (const auto& candidate : candidates) {
      ordered.push_back(&candidate);
    }
    std::sort(ordered.begin(), ordered.end(), [] (const auto* left,
                                                   const auto* right) {
      return left->providerName.toUri() < right->providerName.toUri();
    });

    std::vector<SelectedParticipant> selected;
    selected.reserve(roles.size());
    for (size_t index = 0; index < roles.size(); ++index) {
      const auto& role = roles[index];
      const auto& candidate = *ordered[index];
      const auto payloadText = std::string("opaque-role=") + role.role + ";";
      ndn::Buffer payload(
          reinterpret_cast<const uint8_t*>(payloadText.data()), payloadText.size());
      selected.push_back(SelectedParticipant{
          role.role,
          role.service,
          candidate.providerName,
          role.requiredArtifact,
          false,
          0,
          std::move(payload),
          candidate,
          {}});
    }
    return selected;
  }
};

/**
 * Selection policy used by the streamed native D2b gate.  The policy keeps
 * the planner-owned role-to-Provider map and the exact V3 assignment bytes
 * separate from the request payload.  CommitCollaborationPlan then wraps
 * those bytes in the normal framework CollaborationAssignmentEnvelope and
 * emits the real encrypted Selection, including the streamed event-key grant.
 */
class StreamedNativeD2bSelection final : public ParticipantSelectionPolicy
{
public:
  StreamedNativeD2bSelection(
      std::map<std::string, ndn::Name> roleProviders,
      std::map<std::string, ndn::Buffer> roleAssignments)
    : m_roleProviders(std::move(roleProviders))
    , m_roleAssignments(std::move(roleAssignments))
  {
  }

  std::vector<SelectedParticipant>
  select(const std::vector<AckCandidate>& candidates,
         const std::vector<CollaborationRoleSpec>& roles) const override
  {
    std::vector<SelectedParticipant> selected;
    selected.reserve(roles.size());
    for (const auto& role : roles) {
      const auto providerIt = m_roleProviders.find(role.role);
      const auto assignmentIt = m_roleAssignments.find(role.role);
      if (providerIt == m_roleProviders.end() ||
          assignmentIt == m_roleAssignments.end()) {
        return {};
      }
      const auto candidateIt = std::find_if(
          candidates.begin(), candidates.end(),
          [&provider = providerIt->second, &role](const AckCandidate& candidate) {
            return candidate.providerName == provider &&
                   candidate.serviceName == role.service &&
                   candidate.ack.getStatus();
          });
      if (candidateIt == candidates.end()) {
        return {};
      }
      selected.push_back(SelectedParticipant{
          role.role,
          role.service,
          candidateIt->providerName,
          role.requiredArtifact,
          false,
          0,
          assignmentIt->second,
          *candidateIt,
          {}});
    }
    return selected;
  }

private:
  std::map<std::string, ndn::Name> m_roleProviders;
  std::map<std::string, ndn::Buffer> m_roleAssignments;
};

/**
 * Formal Spec175 selection policy: derive the role/provider and opaque
 * assignment from the successful Provider's ACK capability payload.  The
 * caller supplies only the requested role contract; it does not carry a
 * precomputed role-to-Provider map into Selection.
 */
class Spec175AckCapabilitySelection final : public ParticipantSelectionPolicy
{
public:
  Spec175AckCapabilitySelection() = default;

  explicit Spec175AckCapabilitySelection(
      std::map<std::string, ndn::Buffer> selectionAssignmentByProvider,
      std::map<std::string, ndn::Name> preferredProviderByRole = {})
    : m_selectionAssignmentByProvider(
        std::move(selectionAssignmentByProvider))
    , m_preferredProviderByRole(std::move(preferredProviderByRole))
  {
  }

  std::vector<SelectedParticipant>
  select(const std::vector<AckCandidate>& candidates,
         const std::vector<CollaborationRoleSpec>& roles) const override
  {
    if (std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr) {
      std::cout << "NDNSF_DI_SPEC175_SELECTION candidates=" << candidates.size()
                << " roles=" << roles.size()
                << " assignmentProjections="
                << m_selectionAssignmentByProvider.size() << std::endl;
    }
    std::vector<SelectedParticipant> selected;
    selected.reserve(roles.size());
    for (const auto& role : roles) {
      const auto candidateIt = std::find_if(
          candidates.begin(), candidates.end(),
          [&, preferred = m_preferredProviderByRole.find(role.role)](
              const AckCandidate& candidate) {
            if (!candidate.ack.getStatus()) {
              return false;
            }
            if (preferred != m_preferredProviderByRole.end() &&
                candidate.providerName != preferred->second) {
              return false;
            }
            const auto payload = candidate.ack.getPayload();
            if (payload.empty()) {
              return false;
            }
            const std::string text(
                reinterpret_cast<const char*>(payload.data()), payload.size());
            const auto roleField = std::string("role=") + role.role + ";";
            const auto providerField =
                std::string("provider=") + candidate.providerName.toUri() + ";";
            return text.find(roleField) != std::string::npos &&
                   text.find(providerField) != std::string::npos;
          });
      if (candidateIt == candidates.end()) {
        return {};
      }
      const auto payload = candidateIt->ack.getPayload();
      const auto assignment = m_selectionAssignmentByProvider.find(
        candidateIt->providerName.toUri());
      selected.push_back(SelectedParticipant{
          role.role,
          role.service,
          candidateIt->providerName,
          role.requiredArtifact,
          false,
          0,
          assignment == m_selectionAssignmentByProvider.end()
            ? payload : assignment->second,
          *candidateIt,
          {}});
    }
    return selected;
  }

private:
  std::map<std::string, ndn::Buffer> m_selectionAssignmentByProvider;
  std::map<std::string, ndn::Name> m_preferredProviderByRole;
};

ProviderGroupCoordinatorOptions
makeD2bCoordinatorOptions()
{
  ProviderGroupCoordinatorOptions options;
  options.randomBytes = [] (std::size_t size) {
    return ProviderGroupBytes(size, 0x4d);
  };
  options.wrapEpochKey = [] (const std::string& provider,
                             const ProviderGroupBytes& key) {
    ProviderGroupBytes result(provider.begin(), provider.end());
    result.push_back(':');
    result.insert(result.end(), key.begin(), key.end());
    return result;
  };
  options.unwrapEpochKey = [] (const std::string& provider,
                               const ProviderGroupBytes& wrapped) {
    const ProviderGroupBytes prefix(provider.begin(), provider.end());
    if (wrapped.size() != prefix.size() + 1 + 32 ||
        !std::equal(prefix.begin(), prefix.end(), wrapped.begin()) ||
        wrapped[prefix.size()] != ':') {
      throw std::runtime_error("invalid integration-test wrapped epoch key");
    }
    return ProviderGroupBytes(
      wrapped.begin() + static_cast<std::ptrdiff_t>(prefix.size() + 1),
      wrapped.end());
  };
  const ProviderGroupBytes signingKey(32, 0x73);
  auto sign = [signingKey] (const ProviderGroupBytes& input) {
    ProviderGroupBytes mixed = signingKey;
    mixed.insert(mixed.end(), input.begin(), input.end());
    ProviderGroupBytes result(32, 0);
    for (std::size_t index = 0; index < mixed.size(); ++index) {
      result[index % result.size()] ^= mixed[index];
    }
    return result;
  };
  options.signCapability = sign;
  options.signManifest = sign;
  options.verifyCapability = [sign] (const ProviderGroupBytes& input,
                                     const ProviderGroupBytes& signature) {
    return sign(input) == signature;
  };
  options.verifyManifest = options.verifyCapability;
  return options;
}

std::string
bytesToHex(const ProviderGroupBytes& bytes)
{
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    result.push_back(digits[(byte >> 4) & 0x0f]);
    result.push_back(digits[byte & 0x0f]);
  }
  return result;
}

ProviderGroupBytes
bytesFromHex(const std::string& text)
{
  auto nibble = [] (char value) -> std::uint8_t {
    if (value >= '0' && value <= '9') {
      return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
      return static_cast<std::uint8_t>(value - 'A' + 10);
    }
    throw std::invalid_argument("invalid hexadecimal capability field");
  };
  if (text.empty() || (text.size() % 2) != 0) {
    throw std::invalid_argument("invalid hexadecimal capability length");
  }
  ProviderGroupBytes result;
  result.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    result.push_back(static_cast<std::uint8_t>(
      (nibble(text[index]) << 4) | nibble(text[index + 1])));
  }
  return result;
}

GroupOperationV1
makeD2bOperation()
{
  GroupOperationV1 operation;
  operation.operationIndex = 7;
  operation.kind = "ALL_GATHER";
  // The capability members below are ranks 0 and 1.  Keep every operation
  // endpoint expressed in that authenticated rank namespace; symbolic names
  // such as "receiver" are not valid GroupCapabilityV1 member ranks.
  operation.producerRanks = {"0"};
  operation.consumerRanks = {"1"};
  operation.tensorLayoutDigest = "layout-v1";
  operation.maxBytes = 32;
  operation.maxSegments = 2;
  return operation;
}

std::string
makeV3SelectionRoleJson(const std::string& logicalRole,
                        std::uint64_t rank,
                        const std::string& artifactDigest,
                        const std::string& recipeDigest,
                        const std::string& backend = "onnxruntime",
                        const std::string& device = "cpu:0",
                        const std::string& roleKind = "TENSOR_RANK",
                        bool completeAssembly = false)
{
  const auto deviceSet = (backend == "cpu" ||
                          (backend.size() > 4 &&
                           backend.compare(backend.size() - 4, 4, "-cpu") == 0))
    ? std::string("[]")
    : std::string("[\"") + device + "\"]";
  auto result = std::string("{\"adapter_id\":\"qwen-test\",") +
    "\"adapter_version\":\"1\",\"artifact_digest\":\"" +
    artifactDigest + "\",\"backend\":\"" + backend +
    "\",\"device_set\":" + deviceSet +
    (roleKind == "COMPONENT_SET"
      ? ",\"layer_begin\":0,\"layer_end\":0,\"rank\":"
      : ",\"layer_begin\":0,\"layer_end\":1,\"rank\":") +
    std::to_string(rank) + ",\"recipe_digest\":\"" + recipeDigest +
    "\",\"role\":\"" + logicalRole +
    "\",\"role_kind\":\"" + roleKind + "\"";
  if (roleKind == "COMPONENT_SET" && !completeAssembly) {
    result += ",\"node_indices\":[0]";
  }
  if (completeAssembly) {
    result +=
      ",\"model_manifest_digest\":\"sha256:" + std::string(64, '1') +
      "\",\"artifact_profile_digest\":\"sha256:" + std::string(64, '2') +
      "\",\"graph_digest\":\"sha256:" + std::string(64, '3') +
      "\",\"canonical_initializer_digest\":\"sha256:" + std::string(64, '4') +
      "\",\"adapter_descriptor_digest\":\"sha256:" + std::string(64, '5') +
      "\",\"assembler_descriptor_digest\":\"sha256:" + std::string(64, '6') +
      "\",\"backend_abi\":\"onnxruntime-cpu-v1\",\"node_indices\":[0]"
      ",\"expected_inputs\":[{\"name\":\"x\",\"dtype\":\"float32\",\"shape\":[\"1\",\"3\"]}]"
      ",\"expected_outputs\":[{\"name\":\"y\",\"dtype\":\"float32\",\"shape\":[\"1\",\"1\"]}]"
      ",\"precision\":\"float32\",\"quantization\":\"none\",\"layout\":\"NCHW\",\"padding\":\"none\""
      ",\"resource_envelope\":{\"maxSourceBytes\":1048576,\"maxAssembledBytes\":1048576,\"maxNodes\":64}";
  }
  return result + "}";
}

std::string
makeV3TensorEndpointJson(const std::string& producerNamespace,
                         const std::string& requester,
                         const std::string& requestId,
                         const std::string& planDigest,
                         const std::string& groupId,
                         std::uint64_t round,
                         const std::string& producerRole,
                         std::uint64_t producerRank,
                         const std::string& consumerRole,
                         const std::string& consumerRoles,
                         const std::string& tensorId,
                         const std::string& tensorDigest,
                         const std::string& layoutDigest,
                         const std::string& targetLayoutDigest,
                         const std::string& operation,
                         const std::string& endpointDigest,
                         const std::string& manifestDigest,
                         std::uint64_t attempt = 1)
{
  return std::string("{\"attempt\":") + std::to_string(attempt) +
    ",\"consumer_role\":\"" +
    consumerRole + "\",\"consumer_roles\":[" + consumerRoles +
    "],\"endpoint_digest\":\"" + endpointDigest +
    "\",\"group_epoch\":\"" + std::to_string(attempt) +
    "\",\"group_id\":\"" + groupId +
    "\",\"hard_deadline_ms\":8000,\"layout_digest\":\"" +
    layoutDigest + "\",\"manifest_digest\":\"" + manifestDigest +
    "\",\"microbatch\":0,\"no_progress_deadline_ms\":2000,\"operation\":\"" +
    operation + "\",\"producer_namespace\":\"" + producerNamespace +
    "\",\"producer_rank\":" + std::to_string(producerRank) +
    ",\"producer_role\":\"" + producerRole +
    "\",\"plan_digest\":\"" + planDigest +
    "\",\"request_id\":\"" + requestId +
    "\",\"requester\":\"" + requester +
    "\",\"round\":" + std::to_string(round) +
    ",\"security_profile\":\"NDNSF_DATA_V1\",\"segment_count\":1,"+
    "\"source_kind\":\"ROLE\",\"target_layout_digest\":\"" +
    targetLayoutDigest + "\",\"tensor_digest\":\"" + tensorDigest +
    "\",\"tensor_id\":\"" + tensorId + "\"}";
}

// Fixtures declare ingress permission explicitly, just as the V3 sealer does.
// A role-to-role edge, including TOKEN_FEEDBACK, never grants request input.
std::string
withApplicationInput(std::string dataflow, const std::string& requester,
                     const std::string& requestId, const std::string& planDigest,
                     const std::string& role, std::uint64_t attempt = 1)
{
  const auto identity = "APPLICATION_INPUT|" + requester + "|" + requestId +
    "|" + planDigest + "|" + role + "|" + std::to_string(attempt);
  const auto endpointDigest = sha256TensorBytes(
    std::vector<std::uint8_t>(identity.begin(), identity.end()));
  auto endpoint = makeV3TensorEndpointJson(
    requester, requester, requestId, planDigest, "request-input", 0, "", 0,
    role, "\"" + role + "\"", "request-input", planDigest, planDigest,
    planDigest, "APPLICATION_INPUT", endpointDigest, planDigest, attempt);
  const std::string source = "\"source_kind\":\"ROLE\"";
  endpoint.replace(endpoint.find(source), source.size(),
                   "\"source_kind\":\"APPLICATION_INPUT\"");
  const std::string marker = "\"must_fetch\":[";
  const auto offset = dataflow.find(marker);
  if (offset == std::string::npos) {
    throw std::invalid_argument("fixture dataflow has no must_fetch array");
  }
  const auto first = offset + marker.size();
  dataflow.insert(first, endpoint + (dataflow[first] == ']' ? "" : ","));
  return dataflow;
}

[[maybe_unused]] std::string
makeV3SelectionProjectionJsonDisabled(const std::string& roleJson,
                              const std::string& logicalRole,
                              const std::string& roleKey,
                              std::uint64_t rank,
                              const std::string& provider,
                              const std::string& requestId,
                              const std::string& planDigest,
                              const std::string& capabilityHex,
                              const std::string& dependenciesJson,
                              const std::string& dataflowJson,
                              const std::string& offerDigest,
                              const std::string& device = "cpu:0")
{
#if 0
  return std::string("{\"ack_closed_digest\":\"") + planDigest +
    "\",\"assembly\":" + roleJson +
    ",\"attempt\":1,\"dataflow\":" + dataflowJson +
    ",\"deadline_ms\":9999999999999,\"dependencies\":" +
    dependenciesJson + ",\"device_binding\":{"mode":"SINGLE_DEVICE",\"offer_digest\":\"" +
    offerDigest + "\",\"offer_scoped_device_handle\":\"" + device +
    "\",\"provider\":\"" + provider + "\",\"resource_sequence\":1,"+
    "\"resource_snapshot_digest\":\"" + planDigest +
    "\",\"role\":\"" + roleKey +
    "\",\"sharing_policy\":\"EXCLUSIVE_ROLE\",\"topology_profile_digest\":\"" +
    planDigest + "\"},\"execution_role\":{"adapter_id\":\"qwen-test\",\"adapter_version\":\"1\",\"backend\":\"onnxruntime\",\"layer_begin\":0,\"layer_end\":1,\"rank\":" +
    std::to_string(rank) + ",\"role_id\":\"" + roleKey +
    "\",\"stage_id\":\"" + logicalRole +
    "\"},\"group_capability_v1\":\"" + capabilityHex +
    "\",\"offer_digest\":\"" + offerDigest +
    "\",\"plan_core_digest\":\"" + planDigest +
    "\",\"plan_digest\":\"" + planDigest +
    "\",\"provider\":\"" + provider +
    "\",\"request_id\":\"" + requestId +
    "\",\"roles\":[" + roleJson +
    "],\"schema\":\"ndnsf-di-selection-v3\",\"schema_version\":3,"+
    "\"security_policy_snapshot_digest\":\"" + planDigest + "\"}";
#endif
  return {};
}

std::string
makeV3SelectionProjectionJson(const std::string& roleJson,
                              const std::string& logicalRole,
                              const std::string& roleKey,
                              std::uint64_t rank,
                              const std::string& provider,
                              const std::string& requestId,
                              const std::string& planDigest,
                              const std::string& capabilityHex,
                              const std::string& dependenciesJson,
                              const std::string& dataflowJson,
                              const std::string& offerDigest,
                              const std::string& device = "cpu:0",
                              const std::string& generationContractJson = {},
                              std::uint64_t layerBegin = 0,
                              std::uint64_t layerEnd = 1,
                              std::uint64_t attempt = 1)
{
  return std::string("{\"ack_closed_digest\":\"") + planDigest +
    "\",\"assembly\":" + roleJson +
    ",\"attempt\":" + std::to_string(attempt) + ",\"dataflow\":" + dataflowJson +
    ",\"deadline_ms\":9999999999999,\"dependencies\":" +
    dependenciesJson + ",\"device_binding\":{\"mode\":\"SINGLE_DEVICE\",\"offer_digest\":\"" +
    offerDigest + "\",\"offer_scoped_device_handle\":\"" + device +
    "\",\"provider\":\"" + provider + "\",\"resource_sequence\":1," +
    "\"resource_snapshot_digest\":\"" + planDigest +
    "\",\"role\":\"" + roleKey +
    "\",\"sharing_policy\":\"EXCLUSIVE_ROLE\",\"topology_profile_digest\":\"" +
    planDigest + "\"},\"execution_role\":{\"adapter_id\":\"qwen-test\",\"adapter_version\":\"1\",\"backend\":\"onnxruntime\",\"layer_begin\":" +
    std::to_string(layerBegin) + ",\"layer_end\":" +
    std::to_string(layerEnd) + ",\"rank\":" +
    std::to_string(rank) + ",\"role_id\":\"" + roleKey +
    "\",\"stage_id\":\"" + logicalRole +
    "\"},\"group_capability_v1\":\"" + capabilityHex +
    "\",\"offer_digest\":\"" + offerDigest +
    "\",\"plan_core_digest\":\"" + planDigest +
    "\",\"plan_digest\":\"" + planDigest +
    "\",\"provider\":\"" + provider +
    "\",\"request_id\":\"" + requestId +
    "\"" + (generationContractJson.empty()
      ? std::string()
      : std::string(",\"generation_contract\":") + generationContractJson) +
    ",\"roles\":[" + roleJson +
    "],\"schema\":\"ndnsf-di-selection-v3\",\"schema_version\":3," +
    "\"security_policy_snapshot_digest\":\"" + planDigest + "\"}";
}

std::string
spec175Digest(char value)
{
  return "sha256:" + std::string(64, value);
}

std::string
spec175JsonQuote(const std::string& value)
{
  std::string result;
  result.reserve(value.size() + 2);
  result.push_back('"');
  for (const auto character : value) {
    switch (character) {
      case '\\': result += "\\\\"; break;
      case '"': result += "\\\""; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(character) < 0x20) {
          const char hex[] = "0123456789abcdef";
          result += "\\u00";
          result.push_back(hex[(static_cast<unsigned char>(character) >> 4) & 0x0f]);
          result.push_back(hex[static_cast<unsigned char>(character) & 0x0f]);
        }
        else {
          result.push_back(character);
        }
        break;
    }
  }
  result.push_back('"');
  return result;
}

std::string
spec175EventOracle(const std::vector<std::string>& events)
{
  std::ostringstream result;
  for (std::size_t index = 0; index < events.size(); ++index) {
    if (index != 0) {
      result << '|';
    }
    result << events[index];
  }
  return result.str();
}

std::string
makeSpec175TensorContractJson(const std::string& name,
                              const std::string& dtype,
                              const std::vector<std::string>& shape)
{
  std::string encodedShape;
  for (const auto& dimension : shape) {
    if (!encodedShape.empty()) {
      encodedShape += ',';
    }
    encodedShape += "\"" + dimension + "\"";
  }
  return "{\"name\":\"" + name + "\",\"dtype\":\"" + dtype +
    "\",\"shape\":[" + encodedShape + "]}";
}

std::string
makeSpec175CertifiedRoleJson(const std::string& role,
                             std::size_t roleIndex,
                             std::size_t providerCount,
                             const std::string& artifactDigest,
                             const std::string& recipeDigest)
{
  if (providerCount == 0 || 4 % providerCount != 0 || roleIndex >= providerCount) {
    throw std::invalid_argument("invalid Spec175 certified role partition");
  }
  const auto stateRows = 4 / providerCount;
  const auto layerBegin = roleIndex * stateRows;
  const auto layerEnd = layerBegin + stateRows;
  const auto stateShape = std::vector<std::string>{
    std::to_string(stateRows), "8"};
  std::vector<std::string> inputs;
  inputs.push_back(makeSpec175TensorContractJson(
    roleIndex == 0 ? "input_ids" : "hidden_in",
    roleIndex == 0 ? "int64" : "float32",
    roleIndex == 0
      ? std::vector<std::string>{"1", "sequence"}
      : std::vector<std::string>{"1", "sequence", "8"}));
  std::vector<std::string> outputs;
  outputs.push_back(makeSpec175TensorContractJson(
    roleIndex + 1 == providerCount ? "logits" : "hidden_out", "float32",
    {"1", "sequence", roleIndex + 1 == providerCount ? "32" : "8"}));
  for (const auto& name : {"attention_kv", "recurrent_state", "convolution_state"}) {
    inputs.push_back(makeSpec175TensorContractJson(
      std::string(name) + "_in", "float32", stateShape));
    outputs.push_back(makeSpec175TensorContractJson(
      std::string(name) + "_out", "float32", stateShape));
  }
  const auto join = [] (const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
      if (!result.empty()) result += ',';
      result += value;
    }
    return result;
  };
  std::string nodeIndices;
  for (std::size_t node = layerBegin; node < layerEnd; ++node) {
    if (!nodeIndices.empty()) nodeIndices += ',';
    nodeIndices += std::to_string(node);
  }
  return std::string("{\"adapter_id\":\"qwen-test\",") +
    "\"adapter_version\":\"1\",\"artifact_digest\":\"" + artifactDigest +
    "\",\"backend\":\"onnxruntime\",\"device_set\":[\"cpu:0\"]," +
    "\"layer_begin\":" + std::to_string(layerBegin) +
    ",\"layer_end\":" + std::to_string(layerEnd) +
    ",\"rank\":" + std::to_string(roleIndex) +
    ",\"protection_epoch\":\"plaintext-v1\"," +
    "\"required_device_memory_mb\":0,\"recipe_digest\":\"" + recipeDigest +
    "\",\"role\":\"" + role + "\",\"role_kind\":\"PIPELINE_RANGE\"," +
    "\"model_manifest_digest\":\"" + spec175Digest('5') +
    "\",\"artifact_profile_digest\":\"" + spec175Digest('6') +
    "\",\"graph_digest\":\"" + spec175Digest(static_cast<char>('a' + roleIndex)) +
    "\",\"canonical_initializer_digest\":\"" + spec175Digest('e') +
    "\",\"adapter_descriptor_digest\":\"" + spec175Digest('f') +
    "\",\"assembler_descriptor_digest\":\"" + spec175Digest('9') +
    "\",\"backend_abi\":\"onnxruntime-test-cpu-v1\"," +
    "\"node_indices\":[" + nodeIndices + "],\"expected_inputs\":[" +
    join(inputs) + "],\"expected_outputs\":[" + join(outputs) + "]," +
    "\"precision\":\"float32\",\"quantization\":\"none\"," +
    "\"layout\":\"native\",\"padding\":\"none\"," +
    "\"resource_envelope\":{\"maxSourceBytes\":4096," +
    "\"maxAssembledBytes\":4096,\"maxNodes\":" +
    std::to_string(stateRows) + "}}";
}

void
bindSpec175CertifiedRunnerMetadata(NativeModelRunnerSpec& spec,
                                   std::size_t roleIndex,
                                   std::size_t providerCount,
                                   const std::string& artifactDigest,
                                   const std::string& recipeDigest)
{
  spec.metadata["fragmentDigest"] = artifactDigest;
  spec.metadata["recipeDigest"] = recipeDigest;
  spec.metadata["modelManifestDigest"] = spec175Digest('5');
  spec.metadata["artifactProfileDigest"] = spec175Digest('6');
  spec.metadata["graphDigest"] = spec175Digest(static_cast<char>('a' + roleIndex));
  spec.metadata["canonicalInitializerDigest"] = spec175Digest('e');
  spec.metadata["adapterDescriptorDigest"] = spec175Digest('f');
  spec.metadata["assemblerDescriptorDigest"] = spec175Digest('9');
  spec.metadata["backendAbi"] = "onnxruntime-test-cpu-v1";
  spec.metadata["precision"] = "float32";
  spec.metadata["quantization"] = "none";
  spec.metadata["layout"] = "native";
  spec.metadata["padding"] = "none";
  spec.metadata["maxSourceBytes"] = "4096";
  spec.metadata["maxAssembledBytes"] = "4096";
  spec.metadata["maxNodes"] = std::to_string(4 / providerCount);
}

std::string
makeSpec175GenerationContractJson(std::size_t maxGeneratedTokens,
                                  std::size_t operationStride,
                                  const std::string& samplingDigest,
                                  const std::string& tokenizerDigest,
                                  const std::vector<std::int64_t>& committedPrefix = {},
                                  const std::string& samplingMode = "Greedy",
                                  double samplingTemperature = 0.0,
                                  std::size_t samplingTopK = 1,
                                  double samplingTopP = 1.0,
                                  double samplingRepetitionPenalty = 1.0,
                                  std::uint64_t samplingSeed = 1'750'001,
                                  const std::vector<std::string>& stopStrings = {})
{
  std::string committed;
  for (const auto token : committedPrefix) {
    if (!committed.empty()) committed += ',';
    committed += std::to_string(token);
  }
  std::string stops;
  for (const auto& stop : stopStrings) {
    if (!stops.empty()) {
      stops += ',';
    }
    stops += spec175JsonQuote(stop);
  }
  return std::string("{\"mode\":\"TOKEN_STREAMING\",") +
    "\"max_generated_tokens\":" + std::to_string(maxGeneratedTokens) +
    ",\"token_input_name\":\"input_ids\"," +
    "\"state_input_names\":[\"attention_kv_in\",\"recurrent_state_in\"," +
    "\"convolution_state_in\"],\"state_output_names\":[" +
    "\"attention_kv_out\",\"recurrent_state_out\",\"convolution_state_out\"]," +
    "\"eos_token_ids\":[2],\"sampling_digest\":\"" + samplingDigest +
    "\",\"tokenizer_digest\":\"" + tokenizerDigest +
    "\",\"sampling_mode\":" + spec175JsonQuote(samplingMode) +
    ",\"sampling_temperature\":" + std::to_string(samplingTemperature) +
    ",\"sampling_top_k\":" + std::to_string(samplingTopK) +
    ",\"sampling_top_p\":" + std::to_string(samplingTopP) +
    ",\"sampling_repetition_penalty\":" +
      std::to_string(samplingRepetitionPenalty) +
    ",\"sampling_seed\":" + std::to_string(samplingSeed) +
    ",\"stop_strings\":[" + stops +
    "],\"committed_prefix_token_ids\":[" + committed +
    "],\"streaming_operation_stride\":" + std::to_string(operationStride) + "}";
}

class ScopedSpec175CertifiedModels
{
public:
  explicit ScopedSpec175CertifiedModels(const std::string& caseId)
  {
    static std::atomic<std::uint64_t> sequence{0};
    const auto nonce = sequence.fetch_add(1, std::memory_order_relaxed);
    m_root = std::filesystem::temp_directory_path() /
      ("ndnsf-spec175-certified-" + caseId + "-" + std::to_string(nonce));
    std::filesystem::remove_all(m_root);
    std::filesystem::create_directories(m_root);
  }

  ScopedSpec175CertifiedModels(const ScopedSpec175CertifiedModels&) = delete;
  ScopedSpec175CertifiedModels& operator=(const ScopedSpec175CertifiedModels&) = delete;

  ~ScopedSpec175CertifiedModels()
  {
    std::error_code error;
    std::filesystem::remove_all(m_root, error);
  }

  std::filesystem::path
  materialize(const std::filesystem::path& source, std::size_t roleIndex)
  {
    const auto directory = m_root / ("role-" + std::to_string(roleIndex));
    std::filesystem::create_directories(directory);
    const auto destination = directory / "model.onnx";
    std::filesystem::copy_file(
      source, destination, std::filesystem::copy_options::overwrite_existing);
    return destination;
  }

private:
  std::filesystem::path m_root;
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec170NdnsfDiCoreFlow)

BOOST_AUTO_TEST_CASE(AttemptPlanAndDependencyNamesRemainBound)
{
  ExecutionAttemptAuthority authority;
  const ExecutionAttemptKey attempt{"/request-1", 1};
  BOOST_CHECK_EQUAL(authority.admit(attempt), ExecutionAttemptAdmission::Accepted);
  BOOST_CHECK(authority.isAuthoritative(attempt));
  BOOST_CHECK_THROW(authority.admit({"/request-1", 0}), std::invalid_argument);

  auto plan = makePlan();
  BOOST_REQUIRE_GE(plan.dependencies.size(), 2U);
  plan.dependencies.front().useNdnsfDataV1 = true;
  plan.dependencies.front().collectiveOperationIndex = 7;
  plan.dependencies.front().collectiveProducerRank = "0";
  plan.dependencies.front().collectiveSourceLayoutDigest = "layout/source";
  plan.dependencies.front().collectiveTargetLayoutDigest = "layout/target";
  plan.dependencies.front().collectiveTensorDigest = "sha256:tensor";
  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"stage0", "/provider/p0"},
                               {"stage1", "/provider/p1"},
                               {"stage2", "/provider/p2"}};
  const auto stage1 = roleSpecFor(plan, "stage1", attempt, assignment);
  BOOST_REQUIRE_EQUAL(stage1.inputs.size(), 1);
  BOOST_REQUIRE_EQUAL(stage1.outputs.size(), 1);
  BOOST_CHECK_EQUAL(stage1.inputs.front().requestId, "/request-1");
  BOOST_CHECK_EQUAL(stage1.inputs.front().attemptEpoch, 1);
  BOOST_CHECK_EQUAL(stage1.inputs.front().expectedSegments, 2);
  BOOST_CHECK(stage1.inputs.front().useNdnsfDataV1);
  BOOST_CHECK_EQUAL(stage1.inputs.front().collectiveOperationIndex, 7U);
  BOOST_CHECK_EQUAL(stage1.inputs.front().collectiveProducerRank, "0");
  BOOST_CHECK_EQUAL(stage1.inputs.front().collectiveSourceLayoutDigest,
                    "layout/source");
  BOOST_CHECK_EQUAL(stage1.inputs.front().collectiveTargetLayoutDigest,
                    "layout/target");
  BOOST_CHECK_EQUAL(stage1.inputs.front().collectiveTensorDigest,
                    "sha256:tensor");
  BOOST_CHECK_EQUAL(stage1.outputs.front().expectedSegments, 1);
  BOOST_CHECK(stage1.inputs.front().plannedDataName.find("/attempt/1/") !=
              std::string::npos);
  BOOST_REQUIRE_EQUAL(plannedSegmentNamesForEdge(stage1.inputs.front()).size(), 2);

  BOOST_CHECK(authority.cancel(attempt));
  BOOST_CHECK_EQUAL(authority.admit(attempt), ExecutionAttemptAdmission::Cancelled);
  const ExecutionAttemptKey retry{"/request-1", 2};
  BOOST_CHECK_EQUAL(authority.admit(retry), ExecutionAttemptAdmission::Accepted);
  BOOST_CHECK_EQUAL(authority.admit(attempt), ExecutionAttemptAdmission::Stale);
  BOOST_CHECK(authority.complete(retry));
  BOOST_CHECK_EQUAL(authority.admit(retry), ExecutionAttemptAdmission::DuplicateTerminal);
}

BOOST_AUTO_TEST_CASE(ExactTensorDataUsesSignedConsumerPullAndSameNameRetry)
{
  ndn_service_framework::test::BootstrapProfile profile;
  profile.providerCount = 2;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() ==
                ndn_service_framework::test::EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();

  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);

  auto forwardInterest = consumerFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) { producerFace.receive(interest); });
  auto forwardData = producerFace.onSendData.connect(
      [&] (const ndn::Data& data) { consumerFace.receive(data); });

  ServiceProvider::CollaborationAssignment assignment;
  assignment.role = "S0R0";
  assignment.service = profile.serviceName;
  const ndn::Name requestId("/request/exact-tensor-data");
  ServiceProvider::CollaborationContext producerContext(
      producer, profile.userIdentity, requestId, RequestMessage(), assignment);
  ServiceProvider::CollaborationContext consumerContext(
      consumer, profile.userIdentity, requestId, RequestMessage(), assignment);

  ndn::Name manifestName(producer.getName());
  manifestName.append("NDNSF-DI").append("TENSOR").append("v1")
      .append("REQUESTER").append("746573742d75736572")
      .append("REQ").append("726571756573742d31")
      .append("ATTEMPT").appendNumber(1)
      .append("PLAN").append("7368613235363a706c616e")
      .append("GROUP").append("67726f75702d31")
      .append("EPOCH").appendNumber(3)
      .append("OP").appendNumber(0)
      .append("ROUND").appendNumber(0)
      .append("SOURCE-ROLE").append("53305230")
      .append("RANK").append("30")
      .append("TENSOR").append("61637469766174696f6e")
      .append("7368613235363a74656e736f72")
      .append("MICROBATCH").appendNumber(0)
      .append("MANIFEST");
  const ndn::Buffer manifestWire{'m', 'a', 'n', 'i', 'f', 'e', 's', 't'};
  BOOST_REQUIRE(producerContext.publishSignedExactData(
      "epoch/group-1", {{manifestName, manifestWire}}, 60000));

  std::size_t exactInterestCount = 0;
  bool everyInterestWasExact = true;
  auto observeInterests = consumerFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        if (interest.getName() == manifestName) {
          ++exactInterestCount;
          everyInterestWasExact = everyInterestWasExact &&
                                  !interest.getCanBePrefix();
        }
      });

  auto fetched = std::async(std::launch::async, [&] {
    return consumerContext.fetchSignedExactData(
        "epoch/group-1", manifestName, producer.getName(), 1000);
  });
  environment.pumpUntil([&] {
    return fetched.wait_for(0ms) == std::future_status::ready;
  });
  const auto result = fetched.get();
  BOOST_REQUIRE(result);
  BOOST_CHECK_EQUAL_COLLECTIONS(result->begin(), result->end(),
                                manifestWire.begin(), manifestWire.end());
  BOOST_CHECK_EQUAL(exactInterestCount, 1U);
  BOOST_CHECK(everyInterestWasExact);

  ndn::Name missingName = manifestName.getPrefix(-1);
  missingName.append("SEG").appendSegment(77);
  std::vector<ndn::Name> retryNames;
  auto observeRetries = consumerFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        if (interest.getName() == missingName) {
          retryNames.push_back(interest.getName());
        }
      });
  auto missing = std::async(std::launch::async, [&] {
    return consumerContext.fetchSignedExactData(
        "epoch/group-1", missingName, producer.getName(), 650);
  });
  environment.pumpUntil([&] {
    return missing.wait_for(0ms) == std::future_status::ready;
  });
  BOOST_CHECK(!missing.get());
  BOOST_REQUIRE_GE(retryNames.size(), 2U);
  BOOST_CHECK(std::all_of(retryNames.begin(), retryNames.end(),
                          [&] (const ndn::Name& name) {
                            return name == missingName;
                          }));

  ndn::Name wrongSignerName = manifestName.getPrefix(-1);
  wrongSignerName.append("SEG").appendSegment(78);
  std::size_t wrongSignerInterests = 0;
  auto injectWrongSigner = consumerFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        if (interest.getName() != wrongSignerName) {
          return;
        }
        ++wrongSignerInterests;
        auto data = std::make_shared<ndn::Data>(wrongSignerName);
        data->setFreshnessPeriod(ndn::time::milliseconds(1000));
        data->setContent(ndn::Buffer{'w', 'r', 'o', 'n', 'g'});
        environment.keyChain().sign(
            *data, ndn::security::signingByIdentity(consumer.getName()));
        boost::asio::post(consumerFace.getIoContext(),
                          [&consumerFace, data] { consumerFace.receive(*data); });
      });
  auto wrongSigner = std::async(std::launch::async, [&] {
    return consumerContext.fetchSignedExactData(
        "epoch/group-1", wrongSignerName, producer.getName(), 500);
  });
  environment.pumpUntil([&] {
    return wrongSigner.wait_for(0ms) == std::future_status::ready;
  });
  BOOST_CHECK(!wrongSigner.get());
  BOOST_CHECK_EQUAL(wrongSignerInterests, 1U);

  ndn::Name cancelledName = manifestName.getPrefix(-1);
  cancelledName.append("SEG").appendSegment(79);
  std::atomic<bool> cancelFetch{false};
  std::size_t cancelledInterests = 0;
  auto cancelOnFirstInterest = consumerFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        if (interest.getName() == cancelledName) {
          ++cancelledInterests;
          cancelFetch = true;
        }
      });
  const auto cancelStarted = std::chrono::steady_clock::now();
  auto cancelled = std::async(std::launch::async, [&] {
    return consumerContext.fetchSignedExactData(
        "epoch/group-1", cancelledName, producer.getName(), 1000,
        [&] { return cancelFetch.load(); });
  });
  environment.pumpUntil([&] {
    return cancelled.wait_for(0ms) == std::future_status::ready;
  });
  const auto cancelElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - cancelStarted);
  BOOST_CHECK(!cancelled.get());
  BOOST_CHECK_EQUAL(cancelledInterests, 1U);
  BOOST_CHECK_LT(cancelElapsed.count(), 500);
}

BOOST_AUTO_TEST_CASE(V3DependencyIoPublishesManifestThenReconstructsExactSegments)
{
  ndn_service_framework::test::BootstrapProfile profile;
  profile.providerCount = 2;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() ==
                ndn_service_framework::test::EnvironmentStatus::Ready);
  environment.enableProductionIngressForTest();

  auto& producerFace = environment.providerFace(0);
  auto& consumerFace = environment.providerFace(1);
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto forwardInterest = consumerFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) { producerFace.receive(interest); });
  auto forwardData = producerFace.onSendData.connect(
      [&] (const ndn::Data& data) { consumerFace.receive(data); });

  const auto sha = [] (char value) {
    return std::string("sha256:") + std::string(64, value);
  };
  const ndn::Name requestId("/request/v3-exact-dependency");
  const std::string planDigest = sha('1');
  const std::string groupId = "group-v3-exact";
  const std::string sourceLayout = sha('2');
  const std::string targetLayout = sha('3');
  const std::string tensorDigest = sha('4');

  GroupOperationV1 operation;
  operation.operationIndex = 7;
  operation.kind = "PIPELINE_TRANSFER";
  operation.producerRanks = {"0"};
  operation.consumerRanks = {"1"};
  operation.tensorLayoutDigest = sourceLayout;
  operation.maxBytes = 256;
  operation.maxSegments = 8;

  auto producerCoordinator =
      std::make_shared<ProviderGroupCoordinator>(makeD2bCoordinatorOptions());
  const auto capability = producerCoordinator->createCapability(
      requestId.toUri(), "attempt-1", planDigest, groupId, 3,
      {{producer.getName().toUri(), 0, "offer-producer",
        producer.getName().toUri()},
       {consumer.getName().toUri(), 1, "offer-consumer",
        consumer.getName().toUri()}},
      {operation}, 1024, 1000, 5000);
  auto consumerCoordinator =
      std::make_shared<ProviderGroupCoordinator>(makeD2bCoordinatorOptions());
  consumerCoordinator->installCapability(
      capability,
      producerCoordinator->epochKeyForProvider(producer.getName().toUri()),
      true);

  NativeTensorEndpointV3 endpoint;
  endpoint.producerNamespace = producer.getName().toUri();
  endpoint.requester = profile.userIdentity.toUri();
  endpoint.requestId = requestId.toUri();
  endpoint.attempt = 1;
  endpoint.planDigest = planDigest;
  endpoint.groupId = groupId;
  endpoint.groupEpoch = "3";
  endpoint.operation = operation.kind;
  endpoint.round = operation.operationIndex;
  endpoint.sourceKind = "ROLE";
  endpoint.producerRole = "S0R0";
  endpoint.producerRank = 0;
  endpoint.consumerRole = "S1R0";
  endpoint.consumerRoles = {"S1R0"};
  endpoint.tensorId = "hidden";
  endpoint.tensorDigest = tensorDigest;
  endpoint.layoutDigest = sourceLayout;
  endpoint.targetLayoutDigest = targetLayout;
  endpoint.microbatch = 0;
  endpoint.segmentCount = operation.maxSegments;
  endpoint.manifestDigest = sha('5');
  endpoint.securityProfile = "NDNSF_DATA_V1";
  endpoint.noProgressDeadlineMs = capability.noProgressMs;
  endpoint.hardDeadlineMs = capability.hardDeadlineMs;
  endpoint.endpointDigest = sha('6');

  DependencyEdge edge;
  edge.scope = groupId;
  edge.producerRole = endpoint.producerRole;
  edge.consumerRole = endpoint.consumerRole;
  edge.consumerRoles = endpoint.consumerRoles;
  edge.plannedDataName = tensorObjectNamePrefix(endpoint);
  edge.tensors = {endpoint.tensorId};
  edge.requestId = endpoint.requestId;
  edge.attemptEpoch = endpoint.attempt;
  edge.useNdnsfDataV1 = true;
  edge.collectiveOperationIndex = operation.operationIndex;
  edge.collectiveProducerRank = "0";
  edge.collectiveSourceLayoutDigest = sourceLayout;
  edge.collectiveTargetLayoutDigest = targetLayout;
  edge.collectiveTensorDigest = tensorDigest;
  edge.transportScope = groupId;
  edge.producerProvider = producer.getName().toUri();
  edge.declaredByV3 = true;
  edge.manifestDataName = tensorObjectManifestName(endpoint);
  edge.maxSegments = endpoint.segmentCount;
  edge.endpointDigest = endpoint.endpointDigest;
  edge.planDigest = planDigest;
  edge.manifestContractDigest = endpoint.manifestDigest;
  edge.tensorDigest = tensorDigest;
  edge.layoutDigest = sourceLayout;
  edge.securityProfile = endpoint.securityProfile;
  edge.operationKind = operation.kind;
  edge.round = operation.operationIndex;
  edge.microbatch = 0;
  edge.noProgressDeadlineMs = capability.noProgressMs;
  edge.hardDeadlineMs = capability.hardDeadlineMs;

  ServiceProvider::CollaborationAssignment producerAssignment;
  producerAssignment.role = endpoint.producerRole;
  producerAssignment.service = profile.serviceName;
  ServiceProvider::CollaborationAssignment consumerAssignment;
  consumerAssignment.role = endpoint.consumerRole;
  consumerAssignment.service = profile.serviceName;
  ServiceProvider::CollaborationContext producerContext(
      producer, profile.userIdentity, requestId, RequestMessage(),
      producerAssignment);
  ServiceProvider::CollaborationContext consumerContext(
      consumer, profile.userIdentity, requestId, RequestMessage(),
      consumerAssignment);

  const TensorBundle original{
      "hidden", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
                 10, 11, 12, 13, 14, 15, 16, 17, 18}, 0, 0};
  NdnsfCollaborationDependencyIo producerIo(
      producerContext, 1500, 7, 60000, producerCoordinator);
  NdnsfCollaborationDependencyIo consumerIo(
      consumerContext, 1500, 7, 60000, consumerCoordinator);
  BOOST_REQUIRE_NO_THROW(producerIo.publishOutput("session-v3", edge, original));

  auto fetched = consumerIo.prefetchInput("session-v3", edge);
  environment.pumpUntil([&] {
    return fetched.wait_for(0ms) == std::future_status::ready;
  });
  std::optional<TensorBundle> reconstructed;
  BOOST_REQUIRE_NO_THROW(reconstructed = fetched.get());
  BOOST_REQUIRE(reconstructed);
  BOOST_CHECK_EQUAL_COLLECTIONS(
      reconstructed->payload.begin(), reconstructed->payload.end(),
      original.payload.begin(), original.payload.end());
  BOOST_CHECK_EQUAL(reconstructed->expectedSegments, 3U);
  BOOST_CHECK_EQUAL(reconstructed->expectedBytes, original.payload.size());
}

BOOST_AUTO_TEST_CASE(AsyncDataflowRunsThreeStagePipelineAndRejectsMissingOutput)
{
  const DependencyEdge input{"input", "", "stage0", "/input", 1, 4};
  const DependencyEdge mid{"mid", "stage0", "stage1", "/mid", 1, 4};
  const DependencyEdge out{"out", "stage1", "stage2", "/out", 1, 4};
  const DependencyEdge result{"result", "stage2", "", "/result", 1, 4};
  const std::vector<RoleSpec> roles{
    RoleSpec{"stage0", {input}, {mid}},
    RoleSpec{"stage1", {mid}, {out}},
    RoleSpec{"stage2", {out}, {result}},
  };

  AsyncDataflowRuntime runtime(2);
  const auto resultData = runtime.run(
    "session-1", roles, {{"input", TensorBundle{"input", {1, 2, 3, 4}, 1, 4}}},
    [] (const RoleExecutionContext& context) {
      if (context.role == "stage0") {
        return std::map<std::string, TensorBundle>{
          {"mid", TensorBundle{"mid", {5, 6, 7, 8}, 1, 4}}};
      }
      if (context.role == "stage1") {
        return std::map<std::string, TensorBundle>{
          {"out", TensorBundle{"out", {9, 10, 11, 12}, 1, 4}}};
      }
      return std::map<std::string, TensorBundle>{
        {"result", TensorBundle{"result", {13, 14, 15, 16}, 1, 4}}};
    });
  BOOST_REQUIRE_EQUAL(resultData.roleTimings.size(), 3);
  BOOST_REQUIRE(resultData.outputsByScope.count("result") != 0);
  BOOST_CHECK_EQUAL(resultData.outputsByScope.at("result").payload.back(), 16);

  BOOST_CHECK_THROW(
    runtime.run("session-failure", roles,
                {{"input", TensorBundle{"input", {1}, 1, 1}}},
                [] (const RoleExecutionContext& context) {
                  if (context.role == "stage1") {
                    throw std::runtime_error("injected provider failure");
                  }
                  return std::map<std::string, TensorBundle>{
                    {context.role == "stage0" ? "mid" : "result",
                     TensorBundle{"failure", {1}, 1, 1}}};
                }),
    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DependencyWaitCoversCompletionCancellationAndDeadline)
{
  DependencyWaitScheduler scheduler(1, 4);
  std::promise<DependencyWaitResult> completedPromise;
  auto completedFuture = completedPromise.get_future();
  BOOST_CHECK_EQUAL(
    scheduler.submit("complete", std::chrono::steady_clock::now() + 1s,
                     [] (const DependencyWaitControl&) {
                       return DependencyWaitStatus::Completed;
                     },
                     [&] (const DependencyWaitResult& result) {
                       completedPromise.set_value(result);
                     }),
    DependencyWaitSubmitResult::Accepted);
  BOOST_REQUIRE(scheduler.waitForIdle(1s));
  BOOST_CHECK_EQUAL(completedFuture.get().status, DependencyWaitStatus::Completed);

  std::promise<DependencyWaitResult> cancelledPromise;
  auto cancelledFuture = cancelledPromise.get_future();
  BOOST_CHECK_EQUAL(
    scheduler.submit("cancel", std::chrono::steady_clock::now() + 1s,
                     [] (const DependencyWaitControl& control) {
                       while (!control.isCancelled()) {
                         std::this_thread::sleep_for(1ms);
                       }
                       return DependencyWaitStatus::Cancelled;
                     },
                     [&] (const DependencyWaitResult& result) {
                       cancelledPromise.set_value(result);
                     }),
    DependencyWaitSubmitResult::Accepted);
  std::this_thread::sleep_for(5ms);
  BOOST_CHECK(scheduler.cancel("cancel"));
  BOOST_REQUIRE(scheduler.waitForIdle(1s));
  BOOST_CHECK_EQUAL(cancelledFuture.get().status, DependencyWaitStatus::Cancelled);

  std::promise<DependencyWaitResult> deadlinePromise;
  auto deadlineFuture = deadlinePromise.get_future();
  BOOST_CHECK_EQUAL(
    scheduler.submit("deadline", std::chrono::steady_clock::now() + 5ms,
                     [] (const DependencyWaitControl& control) {
                       while (!control.deadlineExpired()) {
                         std::this_thread::sleep_for(1ms);
                       }
                       return DependencyWaitStatus::DeadlineExpired;
                     },
                     [&] (const DependencyWaitResult& result) {
                       deadlinePromise.set_value(result);
                     }),
    DependencyWaitSubmitResult::Accepted);
  BOOST_REQUIRE(scheduler.waitForIdle(1s));
  BOOST_CHECK_EQUAL(deadlineFuture.get().status, DependencyWaitStatus::DeadlineExpired);
}

BOOST_AUTO_TEST_CASE(ExecutionEvidenceRoundTripsAndRejectsSecrets)
{
  ExecutionEvidence evidence;
  evidence.providerName = "/provider/p0";
  evidence.providerBootId = "boot-1";
  evidence.evidenceEpoch = 1;
  evidence.runnerKind = RunnerKind::WiringOnly;
  evidence.realCompute = false;
  evidence.deviceKind = "cpu";
  evidence.runtimeVersion = "test-runtime";
  evidence.modelDigest = "sha256:" + std::string(64, 'a');
  evidence.planDigest = "sha256:" + std::string(64, 'b');
  evidence.artifactDigests = {{"stage0", "sha256:" + std::string(64, 'c')}};
  evidence.roles = {"stage0"};
  evidence.loadCompleted = true;
  evidence.warmupCompleted = true;
  evidence.createdAtMs = 1;
  const auto json = executionEvidenceToJson(evidence);
  const auto restored = executionEvidenceFromJson(json);
  BOOST_CHECK_EQUAL(restored.providerName, evidence.providerName);
  BOOST_CHECK_EQUAL(toString(restored.runnerKind), std::string("wiring-only"));
  BOOST_CHECK_EQUAL(restored.artifactDigests.at("stage0"),
                    evidence.artifactDigests.at("stage0"));
  BOOST_CHECK_THROW(executionEvidenceFromJson(json.substr(0, json.size() - 1) +
                                               ",\"token\":\"secret\"}"),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PreconfiguredEnvironmentSeparatesBootstrapFromRequests)
{
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment;
  BOOST_CHECK(environment.status() == ndn_service_framework::test::EnvironmentStatus::New);
  BOOST_CHECK_THROW(environment.beginRequest("before-bootstrap"), std::logic_error);

  environment.bootstrap();
  BOOST_CHECK(environment.status() == ndn_service_framework::test::EnvironmentStatus::Ready);
  BOOST_REQUIRE(!environment.snapshot().digest.empty());
  BOOST_CHECK(!environment.user().getAllowedServices().empty());
  BOOST_CHECK(environment.provider().getCurrentPolicyEpoch() > 0);
  BOOST_CHECK_THROW(environment.bootstrap(), std::logic_error);

  auto scope = environment.beginRequest("request-ready-1");
  BOOST_CHECK(environment.status() == ndn_service_framework::test::EnvironmentStatus::RequestActive);
  BOOST_CHECK_EQUAL(scope.snapshotDigest, environment.snapshot().digest);
  BOOST_CHECK_THROW(environment.beginRequest("request-ready-2"), std::logic_error);
  BOOST_CHECK_THROW(environment.resetRequest(scope), std::logic_error);

  environment.markRequestPublished(scope);
  environment.updateRequestResidue(
      scope, ndn_service_framework::test::RequestResidue{1, 0, 0, 0, 0});
  BOOST_CHECK_THROW(environment.resetRequest(scope), std::logic_error);
  environment.updateRequestResidue(scope, {});
  environment.resetRequest(scope);
  BOOST_CHECK(environment.status() == ndn_service_framework::test::EnvironmentStatus::Ready);
  BOOST_CHECK(!scope.active);
}

BOOST_AUTO_TEST_CASE(PreconfiguredEnvironmentAppliesDeterministicPacketFaults)
{
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment;
  environment.bootstrap();

  auto publishData = [&] (const char* suffix) {
    ndn::Data data(ndn::Name("/ndnsf/spec170/fault").append(suffix));
    const std::string payload = suffix;
    data.setContent(payload);
    environment.keyChain().sign(data, ndn::security::signingWithSha256());
    environment.userFace().put(data);
    environment.userFace().processEvents(ndn::time::milliseconds(5));
    environment.providerFace().processEvents(ndn::time::milliseconds(5));
  };

  ndn_service_framework::test::FaultProfile dropFault;
  dropFault.dropPackets = true;
  auto dropped = environment.beginRequest("fault-drop", dropFault);
  environment.markRequestPublished(dropped);
  publishData("drop");
  BOOST_CHECK_GE(environment.bridgeStats().droppedPackets, 1);
  BOOST_CHECK_EQUAL(environment.bridgeStats().forwardedData, 0);
  BOOST_CHECK_EQUAL(environment.bridgeStats().firstDroppedName,
                    "/ndnsf/spec170/fault/drop");
  environment.resetRequest(dropped);

  ndn_service_framework::test::FaultProfile duplicateFault;
  duplicateFault.duplicatePackets = true;
  auto duplicated = environment.beginRequest("fault-duplicate", duplicateFault);
  environment.markRequestPublished(duplicated);
  publishData("duplicate");
  BOOST_CHECK_EQUAL(environment.bridgeStats().duplicatedPackets, 1);
  BOOST_CHECK_EQUAL(environment.bridgeStats().forwardedData, 2);
  environment.resetRequest(duplicated);

  ndn_service_framework::test::FaultProfile reorderFault;
  reorderFault.reorderPackets = true;
  auto reordered = environment.beginRequest("fault-reorder", reorderFault);
  environment.markRequestPublished(reordered);
  publishData("first");
  BOOST_CHECK_EQUAL(environment.bridgeStats().reorderedPackets, 0);
  BOOST_CHECK_EQUAL(environment.bridgeStats().firstPendingName,
                    "/ndnsf/spec170/fault/first");
  publishData("second");
  BOOST_CHECK_EQUAL(environment.bridgeStats().reorderedPackets, 1);
  BOOST_CHECK_EQUAL(environment.bridgeStats().forwardedData, 2);
  environment.resetRequest(reordered);
}

BOOST_AUTO_TEST_CASE(PreconfiguredEnvironmentBootstrapsThreeProviders)
{
  ndn_service_framework::test::BootstrapProfile profile;
  profile.providerCount = 3;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  BOOST_CHECK_EQUAL(environment.providerCount(), 3);
  BOOST_CHECK(environment.provider(0).getCurrentPolicyEpoch() > 0);
  BOOST_CHECK(environment.provider(1).getCurrentPolicyEpoch() > 0);
  BOOST_CHECK(environment.provider(2).getCurrentPolicyEpoch() > 0);
  BOOST_CHECK(environment.provider(0).getName() != environment.provider(1).getName());
  BOOST_CHECK(environment.provider(1).getName() != environment.provider(2).getName());
  BOOST_CHECK(!environment.user().getAllowedServices().empty());
  BOOST_CHECK_THROW(environment.providerFace(3), std::out_of_range);
  BOOST_CHECK_THROW(environment.providerPubSub(3), std::out_of_range);

  auto scope = environment.beginRequest("three-provider-ready");
  environment.markRequestPublished(scope);
  environment.resetRequest(scope);
}

BOOST_AUTO_TEST_CASE(Spec175UnaryYoloI14CompletesWithoutStreamState)
{
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment;
  environment.bootstrap();
  auto scope = environment.beginRequest("fixture-request-1");

  const auto serviceName = environment.profile().serviceName;
  const auto providerName = environment.provider().getName();
  bool requestPublished = false;
  bool providerReceived = false;
  bool handlerCalled = false;
  bool ackPublished = false;
  bool responsePublished = false;
  bool responseReceived = false;
  std::vector<uint8_t> deferredResponseWire;
  ndn::Name deferredResponseName;

  environment.provider().addHandler<FixtureRequest, FixtureResponse>(
      serviceName,
      std::function<void(const ndn::Name&, const FixtureRequest&, FixtureResponse&)>(
          [&] (const ndn::Name& requester, const FixtureRequest& request,
               FixtureResponse& response) {
            BOOST_CHECK_EQUAL(requester, environment.user().getName());
            BOOST_CHECK_EQUAL(request.getPayload(), "fixture-payload");
            handlerCalled = true;
            response.setLabel("fixture-response");
          }));

  environment.providerPubSub().subscribeToProducer(
      environment.profile().userNode,
      [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        const auto parsed = parseRequestNameV2(publication.name);
        if (!parsed || parsed->serviceName != serviceName) {
          return;
        }
        providerReceived = true;
        ndn::Block requestBlock(publication.data);
        RequestMessage publishedRequest;
        BOOST_REQUIRE(publishedRequest.WireDecode(requestBlock));
        const auto response = environment.provider().handleDecryptedRequestByName(
            publication.name, requestBlock);
        BOOST_REQUIRE(response.getStatus());

        RequestAckMessage ack;
        ack.setStatus(true);
        ack.setMessage("fixture-ack");
        ack.setUserToken(publishedRequest.getUserToken());
        ack.setProviderToken("fixture-provider-token");
        const auto ackName = makeRequestAckNameV2(
            providerName, parsed->requesterName, parsed->serviceName,
            parsed->requestId);
        const auto ackBlock = ack.WireEncode();
        environment.providerPubSub().publish(
            ackName, ndn::span<const uint8_t>(ackBlock.data(), ackBlock.size()));
        ackPublished = true;

        deferredResponseName = makeResponseNameV2(
            providerName, parsed->requesterName, parsed->serviceName,
            parsed->requestId);
        const auto responseBlock = response.WireEncode();
        deferredResponseWire.assign(responseBlock.data(),
                                    responseBlock.data() + responseBlock.size());
      },
      true);

  environment.userPubSub().subscribeToProducer(
      environment.profile().providerNode,
      [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        const auto parsedAck = parseRequestAckNameV2(publication.name);
        if (parsedAck && parsedAck->serviceName == serviceName) {
          ndn::Block ackBlock(publication.data);
          if (environment.user().handleRequestAckByName(publication.name, ackBlock)) {
            if (!responsePublished && !deferredResponseWire.empty()) {
              environment.providerPubSub().publish(
                  deferredResponseName,
                  ndn::span<const uint8_t>(deferredResponseWire.data(),
                                           deferredResponseWire.size()));
              responsePublished = true;
            }
          }
          return;
        }

        const auto parsedResponse = parseResponseNameV2(publication.name);
        if (!parsedResponse || parsedResponse->serviceName != serviceName) {
          return;
        }
        ndn::Block responseBlock(publication.data);
        responseReceived = environment.user().handleDecryptedResponseByName(
            publication.name, responseBlock);
      },
      true);

  environment.user().setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& requestName,
           const std::vector<ndn::Name>& providers,
           const ndn::Name& publishedServiceName,
           const RequestMessage& requestMessage, size_t strategy) {
        BOOST_REQUIRE_EQUAL(providers.size(), 1);
        BOOST_CHECK_EQUAL(providers.front(), providerName);
        BOOST_CHECK_EQUAL(publishedServiceName, serviceName);
        BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
        const auto requestBlock = requestMessage.WireEncode();
        environment.userPubSub().publish(
            requestName,
            ndn::span<const uint8_t>(requestBlock.data(), requestBlock.size()));
        requestPublished = true;
        environment.markRequestPublished(scope);
      });

  FixtureRequest request;
  request.setPayload("fixture-payload");
  bool typedCallbackCalled = false;
  const auto requestId = environment.user().RequestService<FixtureRequest, FixtureResponse>(
      {providerName}, serviceName, request,
      std::function<void(const FixtureResponse&)>(
          [&] (const FixtureResponse& response) {
            BOOST_CHECK_EQUAL(response.getLabel(), "fixture-response");
            typedCallbackCalled = true;
          }),
      std::function<void()>([] { BOOST_FAIL("fixture request unexpectedly timed out"); }),
      1000, tlv::FirstResponding);
  BOOST_REQUIRE(!requestId.empty());
  BOOST_CHECK(!environment.user().hasStreamStateForTest(requestId));

  environment.pumpUntil([&] { return typedCallbackCalled; });
  BOOST_CHECK(requestPublished);
  BOOST_CHECK(providerReceived);
  BOOST_CHECK(handlerCalled);
  BOOST_CHECK(ackPublished);
  BOOST_CHECK(responsePublished);
  BOOST_CHECK(responseReceived);
  BOOST_CHECK(typedCallbackCalled);
  BOOST_CHECK(!environment.user().hasStreamStateForTest(requestId));
  environment.updateRequestResidue(scope, {});
  environment.resetRequest(scope);
}

BOOST_AUTO_TEST_CASE(PreconfiguredEnvironmentRunsThreeProviderCustomSelection)
{
  test::BootstrapProfile profile;
  profile.providerCount = 3;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  auto scope = environment.beginRequest("three-provider-custom-selection");

  const auto serviceName = environment.profile().serviceName;
  std::vector<ndn::Name> providerNames;
  providerNames.reserve(environment.providerCount());
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    providerNames.push_back(environment.provider(index).getName());
  }

  using DeferredResponse =
      std::pair<ndn::Name, std::vector<uint8_t>>;
  std::vector<std::map<std::string, DeferredResponse>> deferredResponses(
      environment.providerCount());
  std::vector<size_t> finalResponseCounts(environment.providerCount(), 0);
  bool requestPublished = false;
  bool customSelectionCalled = false;
  bool responseReceived = false;
  bool timedOut = false;

  for (size_t index = 0; index < environment.providerCount(); ++index) {
    auto& provider = environment.provider(index);
    provider.addHandler<FixtureRequest, FixtureResponse>(
        serviceName,
        std::function<void(const ndn::Name&, const FixtureRequest&, FixtureResponse&)>(
            [&, index] (const ndn::Name&, const FixtureRequest& request,
                        FixtureResponse& response) {
              BOOST_CHECK_EQUAL(request.getPayload(), "custom-selection");
              response.setLabel("provider-" + std::to_string(index));
            }));

    environment.providerPubSub(index).subscribeToProducer(
        environment.profile().userNode,
        [&, index] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
          if (auto parsedRequest = parseRequestNameV2(publication.name)) {
            if (!parsedRequest->serviceName.equals(serviceName)) {
              return;
            }
            ndn::Block requestBlock(publication.data);
            RequestMessage requestMessage;
            BOOST_REQUIRE(requestMessage.WireDecode(requestBlock));
            const auto response = environment.provider(index)
                                      .handleDecryptedRequestByName(
                                          publication.name, requestBlock);
            BOOST_REQUIRE(response.getStatus());

            RequestAckMessage ack;
            ack.setStatus(true);
            ack.setMessage("provider-" + std::to_string(index));
            ack.setUserToken(requestMessage.getUserToken());
            ack.setProviderToken(
                "fixture-provider-token-" + std::to_string(index));
            const auto ackName = makeRequestAckNameV2(
                providerNames[index], parsedRequest->requesterName,
                parsedRequest->serviceName, parsedRequest->requestId);
            const auto ackBlock = ack.WireEncode();
            environment.providerPubSub(index).publish(
                ackName,
                ndn::span<const uint8_t>(ackBlock.data(), ackBlock.size()));

            const auto responseName = makeResponseNameV2(
                providerNames[index], parsedRequest->requesterName,
                parsedRequest->serviceName, parsedRequest->requestId);
            const auto responseBlock = response.WireEncode();
            deferredResponses[index][parsedRequest->requestId.toUri()] =
                std::make_pair(
                    responseName,
                    std::vector<uint8_t>(
                        responseBlock.data(),
                        responseBlock.data() + responseBlock.size()));
            return;
          }

          const auto selection = parseServiceSelectionNameV2(publication.name);
          if (!selection || !selection->serviceName.equals(serviceName) ||
              !selection->providerName.equals(providerNames[index])) {
            return;
          }
          const auto it =
              deferredResponses[index].find(selection->requestId.toUri());
          if (it == deferredResponses[index].end()) {
            return;
          }
          environment.providerPubSub(index).publish(
              it->second.first,
              ndn::span<const uint8_t>(it->second.second.data(),
                                       it->second.second.size()));
          ++finalResponseCounts[index];
        },
        true);

    auto providerNode = environment.profile().providerNode;
    if (index > 0) {
      providerNode.append("p" + std::to_string(index));
    }
    environment.userPubSub().subscribeToProducer(
        providerNode,
        [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
          if (auto parsedAck = parseRequestAckNameV2(publication.name)) {
            if (parsedAck->serviceName.equals(serviceName)) {
              ndn::Block ackBlock(publication.data);
              environment.user().handleRequestAckByName(
                  publication.name, ackBlock);
            }
            return;
          }
          if (auto parsedResponse = parseResponseNameV2(publication.name)) {
            if (!parsedResponse->serviceName.equals(serviceName)) {
              return;
            }
            ndn::Block responseBlock(publication.data);
            responseReceived =
                environment.user().handleDecryptedResponseByName(
                    publication.name, responseBlock) || responseReceived;
          }
        },
        true);
  }

  environment.user().setRequestPublisher(
      [&] (const ndn::Name&,
           const ndn::Name& requestName,
           const std::vector<ndn::Name>& providers,
           const ndn::Name& publishedServiceName,
           const RequestMessage& requestMessage,
           size_t strategy) {
        BOOST_REQUIRE_EQUAL(providers.size(), providerNames.size());
        BOOST_CHECK_EQUAL(publishedServiceName, serviceName);
        BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
        const auto requestBlock = requestMessage.WireEncode();
        environment.userPubSub().publish(
            requestName,
            ndn::span<const uint8_t>(requestBlock.data(), requestBlock.size()));
        requestPublished = true;
        environment.markRequestPublished(scope);
      });

  RequestMessage requestMessage;
  const std::string payload = "custom-selection";
  ndn::Buffer requestPayload(
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  requestMessage.setPayload(requestPayload, requestPayload.size());
  const auto requestId = environment.user().RequestService(
      providerNames, serviceName, requestMessage, 40,
      ServiceUser::AckCandidatesHandler(
          [&] (const std::vector<AckSelectionCandidate>& candidates) {
            customSelectionCalled = true;
            std::vector<AckSelectionCandidate> selected;
            for (const auto& candidate : candidates) {
              if (candidate.providerName.equals(providerNames[1])) {
                selected.push_back(candidate);
              }
            }
            BOOST_REQUIRE_EQUAL(selected.size(), 1);
            const auto selectedRequestId = selected.front().requestId;
            ServiceSelectionMessage selection;
            selection.setRequestIDs({selectedRequestId.toUri()});
            selection.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());
            SelectionProviderEntry providerEntry;
            providerEntry.providerName = providerNames[1];
            selection.addProviderEntry(providerEntry);
            const auto selectionName = makeServiceSelectionNameV2(
                environment.user().getName(), providerNames[1], serviceName,
                selectedRequestId);
            const auto selectionBlock = selection.WireEncode();
            environment.userPubSub().publish(
                selectionName,
                ndn::span<const uint8_t>(
                    selectionBlock.data(), selectionBlock.size()));
            return selected;
          }),
      1000,
      [&] (const ndn::Name&) { timedOut = true; },
      [&] (const ResponseMessage& response) {
        FixtureResponse typedResponse;
        const auto responsePayload = response.getPayload();
        BOOST_REQUIRE(typedResponse.ParseFromArray(
            responsePayload.data(), responsePayload.size()));
        BOOST_CHECK_EQUAL(typedResponse.getLabel(), "provider-1");
      },
      tlv::FirstResponding);
  BOOST_REQUIRE(!requestId.empty());

  environment.pumpUntil([&] { return responseReceived; });
  BOOST_CHECK(requestPublished);
  BOOST_CHECK(customSelectionCalled);
  BOOST_CHECK(!timedOut);
  BOOST_CHECK(responseReceived);
  BOOST_CHECK_EQUAL(finalResponseCounts[0], 0);
  BOOST_CHECK_EQUAL(finalResponseCounts[1], 1);
  BOOST_CHECK_EQUAL(finalResponseCounts[2], 0);
  environment.updateRequestResidue(scope, {});
  environment.resetRequest(scope);
}

/**
 * This gate deliberately enters through the provider's production SVS
 * subscriptions.  The older collaboration tests call the post-decryption
 * callback directly and therefore cannot catch a Selection publication or
 * Hybrid-decryption regression.
 */
BOOST_AUTO_TEST_CASE(ProductionIngressRunsRequestSelectionAssignmentDispatch)
{
  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/NativeTracer");
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  const auto providerName = environment.provider().getName();
  const auto requestId = ndn::Name("/production-ingress-request");

  std::atomic<bool> handlerCalled{false};
  environment.provider().setUseTokens(false);
  environment.provider().addCollaborationHandler(
      serviceName,
      [&] (ServiceProvider::CollaborationContext&, const RequestMessage& request) {
        BOOST_CHECK_EQUAL(request.getPayload().size(), 18);
        handlerCalled = true;
      });
  environment.enableProductionIngressForTest();

  RequestMessage request;
  request.setPolicyEpoch(environment.provider().getCurrentPolicyEpoch());
  const std::string requestText = "production-payload";
  ndn::Buffer requestPayload(
      reinterpret_cast<const uint8_t*>(requestText.data()), requestText.size());
  request.setPayload(requestPayload, requestPayload.size());
  const auto requestName = makeRequestNameV2(requesterName, serviceName, requestId);
  const auto requestWire = request.WireEncode();
  const auto encryptedRequest = makeTestHybridPublication(
      requestName, serviceName, requestId, requesterName, "REQUEST",
      ndn::Buffer(requestWire.data(), requestWire.size()));
  environment.provider().cacheHybridReceiveKeyForTest(
      encryptedRequest.key.keyId, encryptedRequest.key.epochId,
      encryptedRequest.key.key);

  environment.userPubSub().publish(
      requestName,
      ndn::span<const uint8_t>(encryptedRequest.wire.data(),
                               encryptedRequest.wire.size()));
  environment.pumpUntil([&] {
    return environment.provider().getPendingRequestCountForTesting() == 1;
  });
  BOOST_CHECK_EQUAL(environment.provider().getPendingRequestCountForTesting(), 1U);

  CollaborationAssignmentEnvelope assignment;
  assignment.role = "backbone";
  assignment.assignedArtifact = ndn::Name("/artifact/backbone");
  const std::string opaqueText = "device=cpu;rank=0";
  assignment.opaquePayload = ndn::Buffer(
      reinterpret_cast<const uint8_t*>(opaqueText.data()), opaqueText.size());
  SelectionProviderEntry providerEntry;
  providerEntry.providerName = providerName;
  providerEntry.assignmentPayload = encodeCollaborationAssignmentEnvelope(assignment);

  ServiceSelectionMessage selection;
  selection.setRequestIDs({requestId.toUri()});
  selection.setAttempt(1);
  selection.addProviderEntry(providerEntry);
  const auto selectionName = makeServiceSelectionNameV2(
      requesterName, providerName, serviceName, requestId);
  const auto selectionWire = selection.WireEncode();
  const auto encryptedSelection = makeTestHybridPublication(
      selectionName, serviceName, requestId, requesterName, "SELECTION",
      ndn::Buffer(selectionWire.data(), selectionWire.size()));
  environment.provider().cacheHybridReceiveKeyForTest(
      encryptedSelection.key.keyId, encryptedSelection.key.epochId,
      encryptedSelection.key.key);

  environment.userPubSub().publish(
      selectionName,
      ndn::span<const uint8_t>(encryptedSelection.wire.data(),
                               encryptedSelection.wire.size()));
  environment.pumpUntil([&] { return handlerCalled.load(); });

  BOOST_CHECK(handlerCalled);
  BOOST_CHECK_EQUAL(environment.provider().getPendingRequestCountForTesting(), 0U);
}

/**
 * A deployed D2 User must learn the exact Provider key and SVS endpoint from
 * authenticated ACKs before it can seal the request-scoped GroupCapabilityV1.
 * This gate deliberately uses BeginCollaboration rather than manufacturing an
 * ACK/capability in the fixture.
 */
BOOST_AUTO_TEST_CASE(DeferredCollaborationAdvertisesDataV1ProviderOffer)
{
  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/D2bCapabilityOffer");
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  const auto providerName = environment.provider().getName();
  const auto requestId = ndn::Name("/d2b-capability-offer");

  environment.user().setUseTokens(false);
  environment.provider().setUseTokens(false);
  const auto ackKey = environment.provider().prepareHybridSendKeyForTest(
    serviceName, "ACK");
  environment.user().cacheHybridReceiveKeyForTest(
    ackKey.keyId, ackKey.epochId, ackKey.key);
  environment.provider().markHybridResponseKeyWrappedForTest(serviceName);
  environment.provider().addCollaborationHandler(
    serviceName,
    [] (ServiceProvider::CollaborationContext&, const RequestMessage&) {});
  environment.enableProductionIngressForTest();

  environment.user().setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name& requestName,
         const std::vector<ndn::Name>&, const ndn::Name& publishedService,
         const RequestMessage& request, size_t strategy) {
      BOOST_CHECK_EQUAL(publishedService, serviceName);
      BOOST_CHECK_EQUAL(strategy, tlv::AllSelected);
      BOOST_REQUIRE(request.hasRequestCapabilities());
      BOOST_CHECK_EQUAL(
        request.getRequestCapabilities().getField("NDNSF_DATA_V1"),
        "required");
      const auto requestBlock = request.WireEncode();
      const auto encrypted = makeTestHybridPublication(
        requestName, serviceName, requestId, requesterName, "REQUEST",
        ndn::Buffer(requestBlock.data(), requestBlock.size()));
      environment.provider().cacheHybridReceiveKeyForTest(
        encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
      environment.userPubSub().publish(
        requestName,
        ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
    });

  RequestCapabilities capabilities;
  capabilities.setField("NDNSF_DATA_V1", "required");
  const std::string payloadText = "d2b-capability-request";
  ndn::Buffer payload(
    reinterpret_cast<const uint8_t*>(payloadText.data()), payloadText.size());
  std::optional<CollaborationAckClosure> closure;
  std::atomic<bool> timedOut{false};
  const auto returnedRequestId = environment.user().BeginCollaboration(
    serviceName, payload, 500, 5000,
    [&] (const CollaborationAckClosure& value) { closure = value; },
    [] (const ResponseMessage&) {},
    [&] (const ndn::Name&) { timedOut = true; },
    requestId,
    CollaborationAckCoverageHandler(),
    capabilities);
  BOOST_REQUIRE_EQUAL(returnedRequestId, requestId);

  environment.pumpUntil([&] { return closure.has_value() || timedOut; });
  BOOST_REQUIRE(!timedOut);
  BOOST_REQUIRE(closure.has_value());
  BOOST_REQUIRE_EQUAL(closure->candidates.size(), 1U);
  const auto& candidate = closure->candidates.front();
  BOOST_CHECK_EQUAL(candidate.providerName, providerName);
  BOOST_REQUIRE(candidate.ack.hasSelectionInputKeyOffer());
  const auto& offer = candidate.ack.getSelectionInputKeyOffer();
  BOOST_CHECK_EQUAL(offer.getField("recipient"), providerName.toUri());
  BOOST_CHECK(!offer.getField("recipientPublicKey").empty());
  BOOST_CHECK(!offer.getField("recipientCertName").empty());
  BOOST_CHECK(!offer.getField("recipientCertDigest").empty());
  BOOST_CHECK(!offer.getField("providerBootEpoch").empty());
  BOOST_CHECK(!offer.getField("ndnsfDataV1EndpointPrefix").empty());
}

/**
 * Full wire-level gate for the request -> ACK -> Selection/assignment ->
 * Response path.  The test uses deterministic Hybrid envelopes and seeded
 * receive keys so it does not depend on controller bootstrap, but every
 * packet is delivered through the real User/Provider SVSPubSub subscriptions;
 * no post-decryption callback is invoked by the test.
 */
BOOST_AUTO_TEST_CASE(ProductionIngressRunsEndToEndSelectionAssignmentResponse)
{
  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/NativeTracer");
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  const auto providerName = environment.provider().getName();
  const auto requestId = ndn::Name("/production-e2e-request");

  std::atomic<bool> requestObserved{false};
  std::atomic<bool> ackObserved{false};
  std::atomic<bool> selectionObserved{false};
  std::atomic<bool> assignmentDispatched{false};
  std::atomic<bool> responseReceived{false};
  std::atomic<bool> timedOut{false};

  environment.user().setUseTokens(false);
  environment.provider().setUseTokens(false);
  environment.provider().addCollaborationHandler(
      serviceName,
      [&] (ServiceProvider::CollaborationContext& context,
           const RequestMessage& request) {
        BOOST_CHECK_EQUAL(request.getPayload().size(), 11);
        BOOST_CHECK_EQUAL(context.assignment().roleProviders.size(), 4U);
        for (const auto& roleProvider : context.assignment().roleProviders) {
          BOOST_CHECK_EQUAL(roleProvider.second, providerName.toUri());
        }
        assignmentDispatched = true;
      });
  // Register the service before attaching production subscriptions so the
  // provider installs its service-specific REQUEST/SELECTION regexes.
  environment.enableProductionIngressForTest();

  // This observer is intentionally separate from the production callback. It
  // records that the request publication reached the Provider's SVS node
  // before the test injects the deterministic ACK.
  environment.providerPubSub().subscribeToProducer(
      environment.profile().userNode,
      [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        const auto parsed = parseRequestNameV2(publication.name);
        if (parsed && parsed->serviceName.equals(serviceName) &&
            parsed->requestId.equals(requestId)) {
          requestObserved = true;
        }
      },
      true);

  environment.user().setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& requestName,
           const std::vector<ndn::Name>& providers,
           const ndn::Name& publishedServiceName,
           const RequestMessage& request, size_t strategy) {
        BOOST_REQUIRE_EQUAL(providers.size(), 1U);
        BOOST_CHECK_EQUAL(providers.front(), providerName);
        BOOST_CHECK_EQUAL(publishedServiceName, serviceName);
        BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
        const auto requestBlock = request.WireEncode();
        const auto encrypted = makeTestHybridPublication(
            requestName, serviceName, requestId, requesterName, "REQUEST",
            ndn::Buffer(requestBlock.data(), requestBlock.size()));
        environment.provider().cacheHybridReceiveKeyForTest(
            encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
        environment.userPubSub().publish(
            requestName,
            ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
      });

  RequestMessage request;
  const std::string payload = "e2e-payload";
  ndn::Buffer requestPayload(
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  request.setPayload(requestPayload, requestPayload.size());
  request.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());

  const auto returnedRequestId = environment.user().RequestService(
      // The ACK is injected only after the real Provider REQUEST subscription
      // has observed and decrypted the publication.  A 40 ms window expires
      // before that production-ingress round-trip on a loaded CI host, so it
      // tests timer expiry instead of the Selection path.
      std::vector<ndn::Name>{providerName}, serviceName, request, 500,
      ServiceUser::AckCandidatesHandler(
          [&] (const std::vector<AckSelectionCandidate>& candidates) {
            ackObserved = true;
            BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
            BOOST_CHECK_EQUAL(candidates.front().providerName, providerName);

            const std::array<std::pair<const char*, const char*>, 4> roles{{
                {"backbone", "/artifact/backbone"},
                {"head-0", "/artifact/head-0"},
                {"head-1", "/artifact/head-1"},
                {"merge", "/artifact/merge"}}};
            std::vector<ndn::Buffer> assignmentItems;
            assignmentItems.reserve(roles.size());
            for (const auto& [role, artifact] : roles) {
              CollaborationAssignmentEnvelope assignment;
              assignment.role = role;
              assignment.assignedArtifact = ndn::Name(artifact);
              const std::string assignmentText =
                  std::string("device=cpu;role=") + role + ";";
              assignment.opaquePayload = ndn::Buffer(
                  reinterpret_cast<const uint8_t*>(assignmentText.data()),
                  assignmentText.size());
              assignmentItems.push_back(
                  encodeCollaborationAssignmentEnvelope(assignment));
            }

            BOOST_REQUIRE(environment.user().setSelectionAssignmentPayloadForRequest(
              requestId, providerName, encodeOpaqueAssignmentSet(assignmentItems)));
            selectionObserved = true;
            return candidates;
          }),
      3000,
      [&] (const ndn::Name&) { timedOut = true; },
      [&] (const ResponseMessage& response) {
        responseReceived = response.getStatus() &&
                           std::string(reinterpret_cast<const char*>(
                               response.getPayload().data()),
                               response.getPayload().size()) == "e2e-response";
      },
      tlv::FirstResponding,
      requestId);
  BOOST_REQUIRE_EQUAL(returnedRequestId, requestId);

  environment.pumpUntil([&] {
    return requestObserved.load() &&
           environment.provider().getPendingRequestCountForTesting() == 1;
  });
  BOOST_CHECK(requestObserved);
  BOOST_CHECK_EQUAL(environment.provider().getPendingRequestCountForTesting(), 1U);

  RequestAckMessage ack;
  ack.setStatus(true);
  ack.setMessage("e2e-ack");
  const auto ackName = makeRequestAckNameV2(
      providerName, requesterName, serviceName, requestId);
  const auto ackBlock = ack.WireEncode();
  const auto encryptedAck = makeTestHybridPublication(
      ackName, serviceName, requestId, providerName, "ACK",
      ndn::Buffer(ackBlock.data(), ackBlock.size()));
  environment.user().cacheHybridReceiveKeyForTest(
      encryptedAck.key.keyId, encryptedAck.key.epochId, encryptedAck.key.key);
  environment.providerPubSub().publish(
      ackName,
      ndn::span<const uint8_t>(encryptedAck.wire.data(), encryptedAck.wire.size()));

  environment.pumpUntil([&] { return assignmentDispatched || timedOut; });
  BOOST_CHECK(ackObserved);
  BOOST_CHECK(!timedOut);
  BOOST_CHECK(assignmentDispatched);
  BOOST_CHECK_EQUAL(environment.provider().getPendingRequestCountForTesting(), 0U);

  ResponseMessage response;
  response.setStatus(true);
  const std::string responseText = "e2e-response";
  ndn::Buffer responsePayload(
      reinterpret_cast<const uint8_t*>(responseText.data()), responseText.size());
  response.setPayload(responsePayload, responsePayload.size());
  const auto responseName = makeResponseNameV2(
      providerName, requesterName, serviceName, requestId);
  const auto responseBlock = response.WireEncode();
  const auto encryptedResponse = makeTestHybridPublication(
      responseName, serviceName, requestId, providerName, "RESPONSE",
      ndn::Buffer(responseBlock.data(), responseBlock.size()));
  environment.user().cacheHybridReceiveKeyForTest(
      encryptedResponse.key.keyId, encryptedResponse.key.epochId,
      encryptedResponse.key.key);
  environment.providerPubSub().publish(
      responseName,
      ndn::span<const uint8_t>(encryptedResponse.wire.data(),
                               encryptedResponse.wire.size()));

  environment.pumpUntil([&] { return responseReceived || timedOut; });
  BOOST_CHECK(selectionObserved);
  BOOST_CHECK(responseReceived);
  BOOST_CHECK(!timedOut);
}

/**
 * D2a CPU structural gate.  The request and four-role assignment enter the
 * real Provider SVS subscriptions; the selected Provider then runs the same
 * two-worker dataflow primitive used by the native DI path.  CPU execution is
 * intentional here: this gate proves assignment-to-runtime wiring without
 * pretending to prove CUDA visibility, which remains a Tiger-only claim.
 */
BOOST_AUTO_TEST_CASE(ProductionIngressRunsD2aAssignmentIntoTwoDeviceRuntime)
{
  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/D2aCpuRuntime");
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  const auto providerName = environment.provider().getName();
  const auto requestId = ndn::Name("/d2a-production-runtime");

  std::atomic<bool> ackObserved{false};
  std::atomic<bool> handlerCalled{false};
  std::atomic<bool> runtimePassed{false};
  std::atomic<bool> timedOut{false};
  std::atomic<size_t> assignmentRoleCount{0};
  std::atomic<size_t> runtimeOutputCount{0};
  std::atomic<size_t> runtimeMaxActive{0};

  environment.user().setUseTokens(false);
  environment.provider().setUseTokens(false);
  environment.provider().addCollaborationHandler(
      serviceName,
      [&] (ServiceProvider::CollaborationContext& context,
           const RequestMessage& request) {
        if (request.getPayload().size() != 11) {
          return;
        }
        const auto& roleProviders = context.assignment().roleProviders;
        assignmentRoleCount = roleProviders.size();
        const std::array<const char*, 4> expectedRoles{{
            "device-0", "device-1", "logical-rank-0", "logical-rank-1"}};
        for (const auto* role : expectedRoles) {
          const auto found = roleProviders.find(role);
          if (found == roleProviders.end() || found->second != providerName) {
            return;
          }
        }

        const DependencyEdge device0Output{
            "device-0-output", "device-0", "", "/d2a/device-0", 1, 4};
        const DependencyEdge device1Output{
            "device-1-output", "device-1", "", "/d2a/device-1", 1, 4};
        const DependencyEdge rank0Output{
            "logical-rank-0-output", "logical-rank-0", "", "/d2a/rank-0", 1, 4};
        const DependencyEdge rank1Output{
            "logical-rank-1-output", "logical-rank-1", "", "/d2a/rank-1", 1, 4};
        const std::vector<RoleSpec> roles{
            RoleSpec{"device-0", {}, {device0Output}},
            RoleSpec{"device-1", {}, {device1Output}},
            RoleSpec{"logical-rank-0", {}, {rank0Output}},
            RoleSpec{"logical-rank-1", {}, {rank1Output}},
        };

        AsyncDataflowRuntime runtime(2);
        std::atomic<size_t> active{0};
        const auto result = runtime.run(
            requestId.toUri(), roles, {},
            [&] (const RoleExecutionContext& execution) {
              const auto nowActive = active.fetch_add(1) + 1;
              auto observedMax = runtimeMaxActive.load();
              while (observedMax < nowActive &&
                     !runtimeMaxActive.compare_exchange_weak(observedMax, nowActive)) {
              }
              std::this_thread::sleep_for(2ms);
              active.fetch_sub(1);
              const std::string scope = execution.role + "-output";
              return std::map<std::string, TensorBundle>{
                  {scope, TensorBundle{scope, {1, 2, 3, 4}, 1, 4}}};
            });
        runtimeOutputCount = result.outputsByScope.size();
        runtimePassed = result.roleTimings.size() == roles.size() &&
                        result.outputsByScope.size() == roles.size();
        handlerCalled = true;
      });

  // Service registration must precede production SVS registration so the
  // service-specific REQUEST and SELECTION regexes are installed.
  environment.enableProductionIngressForTest();
  environment.user().setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& requestName,
           const std::vector<ndn::Name>& providers,
           const ndn::Name& publishedService,
           const RequestMessage& request, size_t strategy) {
        if (providers.size() != 1 || providers.front() != providerName ||
            publishedService != serviceName || strategy != tlv::FirstResponding) {
          return;
        }
        const auto requestBlock = request.WireEncode();
        const auto encrypted = makeTestHybridPublication(
            requestName, serviceName, requestId, requesterName, "REQUEST",
            ndn::Buffer(requestBlock.data(), requestBlock.size()));
        environment.provider().cacheHybridReceiveKeyForTest(
            encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
        environment.userPubSub().publish(
            requestName,
            ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
      });

  RequestMessage request;
  const std::string requestText = "d2a-payload";
  ndn::Buffer requestPayload(
      reinterpret_cast<const uint8_t*>(requestText.data()), requestText.size());
  request.setPayload(requestPayload, requestPayload.size());
  request.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());
  const auto returnedRequestId = environment.user().RequestService(
      std::vector<ndn::Name>{providerName}, serviceName, request, 100,
      ServiceUser::AckCandidatesHandler(
          [&] (const std::vector<AckSelectionCandidate>& candidates) {
            ackObserved = true;
            if (candidates.size() != 1 || candidates.front().providerName != providerName) {
              return candidates;
            }
            const std::array<const char*, 4> roles{{
                "device-0", "device-1", "logical-rank-0", "logical-rank-1"}};
            std::vector<ndn::Buffer> assignmentItems;
            for (const auto* role : roles) {
              CollaborationAssignmentEnvelope assignment;
              assignment.role = role;
              assignment.assignedArtifact = ndn::Name("/artifact").append(role);
              const std::string opaque = std::string("device=") + role +
                                         ";rank=" + role + ";";
              assignment.opaquePayload = ndn::Buffer(
                  reinterpret_cast<const uint8_t*>(opaque.data()), opaque.size());
              assignmentItems.push_back(
                  encodeCollaborationAssignmentEnvelope(assignment));
            }
            BOOST_REQUIRE(environment.user().setSelectionAssignmentPayloadForRequest(
              requestId, providerName, encodeOpaqueAssignmentSet(assignmentItems)));
            return candidates;
          }),
      1000,
      [&] (const ndn::Name&) { timedOut = true; },
      [&] (const ResponseMessage&) {},
      tlv::FirstResponding,
      requestId);
  BOOST_REQUIRE_EQUAL(returnedRequestId, requestId);

  environment.pumpUntil([&] {
    return environment.provider().getPendingRequestCountForTesting() == 1 || timedOut;
  });
  RequestAckMessage ack;
  ack.setStatus(true);
  ack.setMessage("d2a-ack");
  const auto ackName = makeRequestAckNameV2(
      providerName, requesterName, serviceName, requestId);
  const auto ackBlock = ack.WireEncode();
  const auto encryptedAck = makeTestHybridPublication(
      ackName, serviceName, requestId, providerName, "ACK",
      ndn::Buffer(ackBlock.data(), ackBlock.size()));
  environment.user().cacheHybridReceiveKeyForTest(
      encryptedAck.key.keyId, encryptedAck.key.epochId, encryptedAck.key.key);
  environment.providerPubSub().publish(
      ackName,
      ndn::span<const uint8_t>(encryptedAck.wire.data(), encryptedAck.wire.size()));

  environment.pumpUntil([&] { return handlerCalled || timedOut; });
  BOOST_CHECK(ackObserved);
  BOOST_CHECK(handlerCalled);
  BOOST_CHECK(runtimePassed);
  BOOST_CHECK_EQUAL(assignmentRoleCount.load(), 4U);
  BOOST_CHECK_EQUAL(runtimeOutputCount.load(), 4U);
  BOOST_CHECK_GE(runtimeMaxActive.load(), 2U);
  BOOST_CHECK(!timedOut);
}

/**
 * D2b production-ingress gate.  Two selected Providers receive their own
 * assignment through SVS; the first Provider publishes authenticated
 * NDNSF_DATA_V1 segments from its CollaborationContext and the second
 * Provider fetches the exact segment names through its own context.  The
 * provider-to-provider DummyFace bridge is deliberately explicit so the
 * test exercises the same wire boundary as the two-node Tiger workload.
 */
enum class ProductionD2bDataV1Fault
{
  None,
  Tamper,
  Drop,
  Duplicate,
  Reorder,
};

void
runProductionD2bDataV1Case(ProductionD2bDataV1Fault fault)
{
  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/D2bDataV1");
  profile.providerCount = 2;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  environment.disconnectProviderPeerTransportForTest();

  std::atomic<bool> faultApplied{false};
  std::optional<std::string> faultTargetName;
  std::optional<ndn::Data> delayedData;
  std::vector<ndn::signal::ScopedConnection> providerPeerBridges;
  for (size_t source = 0; source < environment.providerCount(); ++source) {
    for (size_t destination = 0; destination < environment.providerCount(); ++destination) {
      if (source == destination) {
        continue;
      }
      providerPeerBridges.emplace_back(
          environment.providerFace(source).onSendInterest.connect(
              [&environment, destination] (const ndn::Interest& interest) {
                auto packet = interest;
                auto& destinationFace = environment.providerFace(destination);
                boost::asio::post(
                    destinationFace.getIoContext(),
                    [&environment, destination, packet = std::move(packet)] {
                      environment.providerFace(destination).receive(packet);
                    });
              }));
      providerPeerBridges.emplace_back(
          environment.providerFace(source).onSendData.connect(
              [&environment, &faultApplied, &faultTargetName, &delayedData,
               destination, source, fault] (const ndn::Data& data) {
                auto forward = [&] (const ndn::Data& packet) {
                  auto copy = packet;
                  auto& destinationFace = environment.providerFace(destination);
                  boost::asio::post(
                      destinationFace.getIoContext(),
                      [&environment, destination, packet = std::move(copy)] {
                        environment.providerFace(destination).receive(packet);
                      });
                };
                if (source != 0 || destination != 1 ||
                    fault == ProductionD2bDataV1Fault::None ||
                    data.getContent().value_size() == 0) {
                  forward(data);
                  return;
                }
                try {
                  const ndn::Data inner(data.getContent().blockFromValue());
                  if (inner.getName().toUri().find(
                        "NDNSF-DI/COLLECTIVE/v1/") == std::string::npos) {
                    forward(data);
                    return;
                  }
                  const auto innerName = inner.getName().toUri();
                  if (fault == ProductionD2bDataV1Fault::Drop) {
                    if (!faultTargetName) {
                      faultTargetName = innerName;
                      faultApplied = true;
                    }
                    if (innerName == *faultTargetName) {
                      return;
                    }
                    forward(data);
                    return;
                  }
                  if (fault == ProductionD2bDataV1Fault::Duplicate &&
                      !faultApplied.exchange(true)) {
                    forward(data);
                    forward(data);
                    return;
                  }
                  if (fault == ProductionD2bDataV1Fault::Reorder) {
                    if (!delayedData) {
                      delayedData = data;
                      faultApplied = true;
                      return;
                    }
                    forward(data);
                    forward(*delayedData);
                    delayedData.reset();
                    return;
                  }
                  if (fault == ProductionD2bDataV1Fault::Tamper) {
                    if (!faultTargetName) {
                      faultTargetName = innerName;
                      faultApplied = true;
                    }
                    if (innerName != *faultTargetName) {
                      forward(data);
                      return;
                    }
                    const auto content = inner.getContent();
                    std::vector<std::uint8_t> bytes(content.value_begin(),
                                                    content.value_end());
                    if (!bytes.empty()) {
                      bytes.front() ^= 0x01;
                    }
                    ndn::Data mutatedInner = inner;
                    mutatedInner.setContent(ndn::span<const uint8_t>(
                      bytes.data(), bytes.size()));
                    environment.keyChain().sign(
                      mutatedInner, ndn::security::signingWithSha256());
                    ndn::Data mutatedOuter = data;
                    const auto innerWire = mutatedInner.wireEncode();
                    mutatedOuter.setContent(innerWire);
                    environment.keyChain().sign(
                      mutatedOuter, ndn::security::signingWithSha256());
                    forward(mutatedOuter);
                    return;
                  }
                }
                catch (const std::exception&) {
                  // This outer Data is not the target SVS publication.
                }
                forward(data);
              }));
    }
  }

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  const auto provider0Name = environment.provider(0).getName();
  const auto provider1Name = environment.provider(1).getName();
  auto producerPrefix = environment.profile().providerNode;
  producerPrefix.append("0");
  auto consumerPrefix = environment.profile().providerNode;
  consumerPrefix.append("p1").append("0");
  const auto requestId = ndn::Name("/d2b-production-data-v1");

  ProviderGroupCoordinator producerCoordinator(makeD2bCoordinatorOptions());
  const auto operation = makeD2bOperation();
  const auto capability = producerCoordinator.createCapability(
      requestId.toUri(), "attempt-1", "plan-d2b", "group-d2b", 3,
      {{provider0Name.toUri(), 0, "offer-p0", producerPrefix.toUri()},
       {provider1Name.toUri(), 1, "offer-p1", consumerPrefix.toUri()}},
      {operation}, 64, 1000, 5000);
  const auto epochKey = producerCoordinator.epochKeyForProvider(
      provider0Name.toUri());
  ProviderGroupCoordinator receiverCoordinator(makeD2bCoordinatorOptions());
  receiverCoordinator.installCapability(capability, epochKey, true);
  const auto sealed = producerCoordinator.sealOperation(
      operation, "0", "src-d2b", "dst-d2b", "tensor-d2b",
      {{'a', 'b'}, {'c', 'd'}}, 100);

  std::vector<std::pair<ndn::Name, ndn::Buffer>> publications;
  for (const auto& segment : sealed.segments) {
    const auto wire = ProviderGroupCoordinator::encodeSegment(
        sealed.manifest, segment);
    const ndn::Name canonicalDataName(segment.dataName);
    BOOST_REQUIRE(provider0Name.isPrefixOf(canonicalDataName));
    // The retired SVS DATA_V1 transport demultiplexes on its producer-node
    // prefix.  Keep that outer publication locator for this compatibility
    // test, while the encoded/capability-bound segment retains its canonical
    // producer-identity Data name used by the Spec175 exact-Data path.
    ndn::Name transportPublicationName(producerPrefix);
    transportPublicationName.append(
        canonicalDataName.getSubName(provider0Name.size()));
    publications.emplace_back(std::move(transportPublicationName),
                              ndn::Buffer(wire.begin(), wire.end()));
  }

  std::atomic<bool> ackObserved{false};
  std::atomic<bool> planCommitted{false};
  std::atomic<bool> planCommitFailed{false};
  std::atomic<bool> provider0Published{false};
  std::atomic<bool> provider1HandlerCalled{false};
  std::atomic<bool> consumerSubscriptionReady{false};
  std::atomic<bool> fetchCompleted{false};
  std::atomic<bool> timedOut{false};
  std::string planCommitError;
  auto fetchPromise = std::make_shared<
      std::promise<std::optional<std::vector<ndn::Buffer>>>>();
  auto fetchFuture = fetchPromise->get_future();
  std::thread fetchThread;

  environment.user().setUseTokens(false);
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    environment.provider(index).setUseTokens(false);
  }
  environment.provider(0).addCollaborationHandler(
      serviceName,
      [&] (ServiceProvider::CollaborationContext& context,
           const RequestMessage& request) {
        if (request.getPayload().size() != 11 ||
            context.assignment().role != "producer") {
          return;
        }
        // The consumer Selection is intentionally committed first, but its
        // handler runs on another worker.  Do not let a scheduler race turn
        // the legacy subscribe-before-publish DATA_V1 contract into a flaky
        // test.  This wait is bounded and never runs on the Face event loop.
        for (int round = 0; round < 200 && !consumerSubscriptionReady; ++round) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!consumerSubscriptionReady) {
          return;
        }
        provider0Published = context.publishDataV1Segments(
            "/scope/d2b", publications, 60000);
      });
  environment.provider(1).addCollaborationHandler(
      serviceName,
      [&] (ServiceProvider::CollaborationContext& context,
           const RequestMessage& request) {
        if (request.getPayload().size() != 11 ||
            context.assignment().role != "consumer") {
          return;
        }
        provider1HandlerCalled = true;
        auto contextCopy = std::make_shared<ServiceProvider::CollaborationContext>(context);
        fetchThread = std::thread(
            [contextCopy, fetchPromise, producerPrefix, operation,
             &consumerSubscriptionReady, &fetchCompleted] {
              try {
                fetchPromise->set_value(contextCopy->fetchDataV1Segments(
                    "/scope/d2b", producerPrefix, operation.operationIndex,
                    "0", "tensor-d2b", 2, operation.maxSegments, 3000,
                    {}, DataV1SegmentNameFilter{
                      {}, [&consumerSubscriptionReady] {
                        consumerSubscriptionReady = true;
                      }}));
                fetchCompleted = true;
              }
              catch (...) {
                fetchPromise->set_exception(std::current_exception());
                fetchCompleted = true;
              }
            });
      });
  environment.enableProductionIngressForTest();

  // CommitCollaborationPlan owns Selection publication.  Route its encrypted
  // wire through the same user SVS instance as production; do not inject a
  // second Selection for a Provider that Core has already selected.
  environment.user().setLocalPublicationHandler(
      [&environment] (const ndn::Name& messageName, const ndn::Buffer& wire) {
        if (!parseServiceSelectionNameV2(messageName)) {
          return;
        }
        environment.userPubSub().publish(
            messageName,
            ndn::span<const uint8_t>(wire.data(), wire.size()));
      });

  environment.user().setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& requestName,
           const std::vector<ndn::Name>&,
           const ndn::Name& publishedService,
           const RequestMessage& request, size_t strategy) {
        if (publishedService != serviceName || strategy != tlv::AllSelected) {
          return;
        }
        const auto requestBlock = request.WireEncode();
        const auto encrypted = makeTestHybridPublication(
            requestName, serviceName, requestId, requesterName, "REQUEST",
            ndn::Buffer(requestBlock.data(), requestBlock.size()));
        for (size_t index = 0; index < environment.providerCount(); ++index) {
          environment.provider(index).cacheHybridReceiveKeyForTest(
              encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
        }
        environment.userPubSub().publish(
            requestName,
            ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
      });
  const std::string requestText = "d2b-payload";
  ndn::Buffer requestPayload(
      reinterpret_cast<const uint8_t*>(requestText.data()), requestText.size());

  const auto returnedRequestId = environment.user().BeginCollaboration(
      serviceName, requestPayload, 200, 5000,
      [&] (const CollaborationAckClosure& closure) {
        ackObserved = closure.candidates.size() == 2;
        if (!ackObserved) {
          return;
        }

        CollaborationPlan plan;
        plan.ackCollectionTimeMs = 200;
        plan.timeoutMs = 5000;
        // Consumer first: it installs the request-scoped legacy SVS
        // subscription before the producer publishes the first segment.
        for (const auto& role : {std::string("consumer"),
                                 std::string("producer")}) {
          CollaborationRoleSpec roleSpec;
          roleSpec.role = role;
          roleSpec.service = serviceName;
          roleSpec.requiredArtifact = ndn::Name("/artifact").append(role);
          plan.roles.push_back(std::move(roleSpec));
        }

        std::map<std::string, ndn::Name> roleProviders{
            {"consumer", provider1Name}, {"producer", provider0Name}};
        std::map<std::string, ndn::Buffer> roleAssignments;
        for (const auto& role : {std::string("consumer"),
                                 std::string("producer")}) {
          const std::string opaque = "rank=" + role + ";";
          roleAssignments.emplace(
              role, ndn::Buffer(
                  reinterpret_cast<const uint8_t*>(opaque.data()),
                  opaque.size()));
        }
        plan.participantSelector =
            std::make_shared<StreamedNativeD2bSelection>(
                std::move(roleProviders), std::move(roleAssignments));
        try {
          planCommitted = environment.user().CommitCollaborationPlan(
              closure.requestId, closure.digest, std::move(plan));
        }
        catch (const std::exception& error) {
          planCommitError = error.what();
          planCommitFailed = true;
        }
      },
      [&] (const ResponseMessage&) {},
      [&] (const ndn::Name&) { timedOut = true; },
      requestId);
  BOOST_REQUIRE_EQUAL(returnedRequestId, requestId);
  environment.pumpUntil([&] {
    // Keep driving every DummyFace until the asynchronous fetch reaches its
    // terminal state. Waiting on the future after stopping this pump would
    // prevent the outstanding SVS Interests/Data from making progress.
    return fetchCompleted ||
           planCommitFailed || timedOut;
  });
  BOOST_CHECK(ackObserved);
  BOOST_CHECK_MESSAGE(planCommitted, planCommitError);
  BOOST_CHECK(provider0Published);
  BOOST_CHECK(provider1HandlerCalled);
  BOOST_CHECK(!timedOut);

  std::optional<std::vector<ndn::Buffer>> fetched;
  std::exception_ptr fetchError;
  if (fetchFuture.wait_for(std::chrono::milliseconds(3500)) ==
      std::future_status::ready) {
    try {
      fetched = fetchFuture.get();
    }
    catch (...) {
      fetchError = std::current_exception();
    }
  }
  else {
    BOOST_ERROR("D2b DATA_V1 fetch did not become ready within its bounded deadline");
  }
  if (fetchThread.joinable()) {
    fetchThread.join();
  }
  if (fault == ProductionD2bDataV1Fault::Drop) {
    BOOST_CHECK(faultApplied);
    BOOST_CHECK(fetchError != nullptr || !fetched ||
                fetched->size() != sealed.segments.size());
    return;
  }
  if (fault == ProductionD2bDataV1Fault::Tamper) {
    BOOST_CHECK(faultApplied);
    BOOST_CHECK(fetchError == nullptr);
    BOOST_REQUIRE(fetched);
    bool rejected = false;
    for (const auto& wire : *fetched) {
      try {
        const auto decoded = ProviderGroupCoordinator::decodeSegment(
            ProviderGroupBytes(wire.begin(), wire.end()));
        BOOST_REQUIRE_EQUAL(decoded.segments.size(), 1U);
        receiverCoordinator.openSegment(
            decoded.manifest, decoded.segments.front());
      }
      catch (const std::exception&) {
        rejected = true;
        break;
      }
    }
    BOOST_CHECK(rejected);
    return;
  }
  if (fault == ProductionD2bDataV1Fault::Duplicate ||
      fault == ProductionD2bDataV1Fault::Reorder) {
    BOOST_CHECK(faultApplied);
  }
  if (fetchError != nullptr) {
    BOOST_FAIL("unexpected D2b DATA_V1 fetch failure");
  }
  BOOST_REQUIRE(fetched);
  BOOST_REQUIRE_EQUAL(fetched->size(), sealed.segments.size());

  std::vector<std::uint8_t> plaintext;
  for (const auto& wire : *fetched) {
    const auto decoded = ProviderGroupCoordinator::decodeSegment(
        ProviderGroupBytes(wire.begin(), wire.end()));
    BOOST_REQUIRE_EQUAL(decoded.segments.size(), 1U);
    const auto accepted = receiverCoordinator.acceptSegment(
        decoded.manifest, decoded.segments.front());
    BOOST_REQUIRE(accepted == DataSegmentReplayWindow::Result::Accepted);
    const auto segmentPlaintext = receiverCoordinator.openSegment(
        decoded.manifest, decoded.segments.front());
    plaintext.insert(plaintext.end(), segmentPlaintext.begin(), segmentPlaintext.end());
  }
  const std::vector<std::uint8_t> expected{'a', 'b', 'c', 'd'};
  BOOST_CHECK_EQUAL_COLLECTIONS(plaintext.begin(), plaintext.end(),
                                expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(ProductionIngressRunsD2bSelectionIntoSvsDataV1)
{
  runProductionD2bDataV1Case(ProductionD2bDataV1Fault::None);
}

BOOST_AUTO_TEST_CASE(ProductionIngressRejectsTamperedD2bSvsDataV1)
{
  runProductionD2bDataV1Case(ProductionD2bDataV1Fault::Tamper);
}

BOOST_AUTO_TEST_CASE(ProductionIngressBoundsDroppedD2bSvsDataV1)
{
  runProductionD2bDataV1Case(ProductionD2bDataV1Fault::Drop);
}

BOOST_AUTO_TEST_CASE(ProductionIngressDeduplicatesD2bSvsDataV1)
{
  runProductionD2bDataV1Case(ProductionD2bDataV1Fault::Duplicate);
}

BOOST_AUTO_TEST_CASE(ProductionIngressReordersD2bSvsDataV1)
{
  runProductionD2bDataV1Case(ProductionD2bDataV1Fault::Reorder);
}

/**
 * True D2b vertical slice.  Unlike the transport-only gate above, this sends
 * one Request through ACK/Selection into two production NativeProviderHandler
 * instances.  Backbone execution publishes its dependency with
 * NDNSF_DATA_V1, Head consumes it through the second Provider's SVS endpoint,
 * and exactly one final Response publication is produced.
 */
void
runProductionNativeD2bCase(bool tamperCapability,
                            bool streamed = false,
                            bool usePostSelectionAssembly = false)
{
  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/D2bNativeE2e");
  profile.providerCount = 2;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const char* realOnnxModel = std::getenv("NDNSF_DI_TEST_ONNX_MODEL");
  const bool useRealOnnx = realOnnxModel != nullptr && *realOnnxModel != '\0';
  std::shared_ptr<NativeModelRunnerFactory> realRunnerFactory;
  if (useRealOnnx) {
#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
    BOOST_FAIL("NDNSF_DI_TEST_ONNX_MODEL requires C++ ONNX Runtime backend");
#else
    auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
    registerOnnxRuntimeBackend(*factory);
    realRunnerFactory = std::move(factory);
#endif
  }

  std::vector<ndn::signal::ScopedConnection> providerPeerBridges;
  for (size_t source = 0; source < environment.providerCount(); ++source) {
    for (size_t destination = 0; destination < environment.providerCount(); ++destination) {
      if (source == destination) {
        continue;
      }
      providerPeerBridges.emplace_back(
        environment.providerFace(source).onSendInterest.connect(
          [&environment, destination] (const ndn::Interest& interest) {
            environment.providerFace(destination).receive(interest);
          }));
      providerPeerBridges.emplace_back(
        environment.providerFace(source).onSendData.connect(
          [&environment, destination] (const ndn::Data& data) {
            environment.providerFace(destination).receive(data);
          }));
    }
  }

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  const auto provider0Name = environment.provider(0).getName();
  const auto provider1Name = environment.provider(1).getName();
  auto producerPrefix = environment.profile().providerNode;
  producerPrefix.append("0");
  auto consumerPrefix = environment.profile().providerNode;
  consumerPrefix.append("p1").append("0");
  const auto requestId = ndn::Name("/d2b-native-e2e");
  const std::string planDigest = "sha256:" + std::string(64, 'd');
  const std::string pipelineLayoutDigest = "sha256:" + std::string(64, 'e');
  const std::string featureTensorDigest = "sha256:" + std::string(64, 'f');

  NativeExecutionPlan plan;
  plan.serviceName = serviceName.toUri();
  plan.modelName = "d2b-native-e2e";
  plan.executionPolicy = "DATA_DRIVEN_V2";
  plan.roles = streamed
    ? std::vector<std::string>{"/Backbone", "/Head/Shard/0"}
    : std::vector<std::string>{"/Backbone", "/Aux", "/Head/Shard/0"};
  NativeDependencySpec dependency(
    {"/Backbone"}, {"/Head/Shard/0"}, "backbone-to-head0",
    "/d2b/features", "/d2b/{sessionId}/features", 1, 0, {"features"});
  dependency.useNdnsfDataV1 = true;
  dependency.collectiveOperationIndex = 7;
  dependency.collectiveProducerRank = "0";
  dependency.collectiveSourceLayoutDigest = pipelineLayoutDigest;
  dependency.collectiveTargetLayoutDigest = pipelineLayoutDigest;
  dependency.collectiveTensorDigest = featureTensorDigest;
  plan.dependencies = {dependency};

  GroupOperationV1 operation;
  operation.operationIndex = 7;
  operation.kind = "PIPELINE_TRANSFER";
  operation.producerRanks = {"0"};
  operation.consumerRanks = {"1"};
  operation.tensorLayoutDigest = pipelineLayoutDigest;
  operation.maxBytes = 4096;
  operation.maxSegments = 1;
  ProviderGroupCoordinator capabilitySealer(makeD2bCoordinatorOptions());
  const auto capability = capabilitySealer.createCapability(
    requestId.toUri(), "attempt-1", planDigest, "group-d2b-native", 1,
    {{provider0Name.toUri(), 0, "offer-p0", provider0Name.toUri()},
     {provider1Name.toUri(), 1, "offer-p1", provider1Name.toUri()}},
    {operation}, 4096, 2000, 8000);
  auto observedMutex = std::make_shared<std::mutex>();
  auto observedRoles = std::make_shared<std::set<std::string>>();
  auto observedInputs = std::make_shared<
    std::map<std::string, std::map<std::string, std::string>>>();
  auto observedOutputs = std::make_shared<
    std::map<std::string, std::map<std::string, std::string>>>();
  std::array<std::atomic<bool>, 2> handlerEntered{};
  std::array<std::atomic<bool>, 2> streamPublisherObserved{};
  std::array<std::atomic<size_t>, 2> coordinatorFactoryCalls{};
  auto evidenceMutex = std::make_shared<std::mutex>();
  auto observedEvidence = std::make_shared<
    std::map<std::string, ExecutionEvidence>>();
  std::array<std::string, 2> selectionDigests;
  std::array<std::atomic<size_t>, 2> preparationFactoryCalls{};
  std::mutex responseNamesMutex;
  std::set<std::string> responseNames;
  std::atomic<size_t> uniqueResponsePublications{0};
  std::atomic<bool> ackObserved{false};
  std::atomic<bool> timedOut{false};
  std::atomic<bool> streamedPlanCommitted{false};
  std::atomic<bool> streamedComplete{false};
  std::atomic<bool> streamedError{false};
  std::mutex streamedResultMutex;
  std::vector<std::string> streamedEvents;
  std::string streamedResult;
  std::string streamedErrorMessage;

  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"/Backbone", provider0Name.toUri()},
                               {"/Head/Shard/0", provider1Name.toUri()}};
  if (!streamed) {
    assignment.providerByRole["/Aux"] = provider0Name.toUri();
  }

  auto makeRunnerSpec = [&] (size_t index, const std::string& role) {
    NativeModelRunnerSpec spec;
    spec.role = role;
    spec.kind = "onnx-model";
    spec.backend = "onnxruntime";
    spec.path = usePostSelectionAssembly
      ? "/integration-test/model.onnx"
      : useRealOnnx
      ? std::string(realOnnxModel)
      : "/integration-test/d2b-native.onnx";
    spec.metadata["test.providerName"] = environment.provider(index).getName().toUri();
    spec.metadata["test.providerBootId"] = "d2b-native-boot-" + std::to_string(index);
    spec.metadata["test.planDigest"] = planDigest;
    const auto artifactDigest =
      "sha256:" + std::string(64, index == 0 ? 'a' : 'b');
    const auto recipeDigest =
      "sha256:" + std::string(64, index == 0 ? 'c' : 'e');
    spec.metadata["test.artifactDigest"] = artifactDigest;
    spec.metadata["test.deviceId"] = "0";
    if (usePostSelectionAssembly) {
      spec.metadata["fragmentDigest"] = artifactDigest;
      spec.metadata["recipeDigest"] = recipeDigest;
      spec.metadata["modelManifestDigest"] =
        "sha256:" + std::string(64, '1');
      spec.metadata["artifactProfileDigest"] =
        "sha256:" + std::string(64, '2');
      spec.metadata["graphDigest"] = "sha256:" + std::string(64, '3');
      spec.metadata["canonicalInitializerDigest"] =
        "sha256:" + std::string(64, '4');
      spec.metadata["adapterDescriptorDigest"] =
        "sha256:" + std::string(64, '5');
      spec.metadata["assemblerDescriptorDigest"] =
        "sha256:" + std::string(64, '6');
      spec.metadata["backendAbi"] = "onnxruntime-cpu-v1";
      spec.metadata["precision"] = "float32";
      spec.metadata["quantization"] = "none";
      spec.metadata["layout"] = "NCHW";
      spec.metadata["padding"] = "none";
      spec.metadata["maxSourceBytes"] = "1048576";
      spec.metadata["maxAssembledBytes"] = "1048576";
      spec.metadata["maxNodes"] = "64";
    }
    if (useRealOnnx) {
      spec.metadata["executionProvider"] = "cpu";
      spec.metadata["inputNames"] = "x";
      spec.metadata["inputShape"] = "1,3";
      spec.metadata["outputNames"] = "y";
      spec.metadata["outputScope"] = role == "/Backbone"
        ? "features"
        : role == "/Head/Shard/0" ? "detections0" : "aux";
      spec.metadata["evidence.providerName"] =
        environment.provider(index).getName().toUri();
      spec.metadata["evidence.providerBootId"] =
        "d2b-native-boot-" + std::to_string(index);
      spec.metadata["evidence.epoch"] = "1";
      spec.metadata["evidence.modelDigest"] =
        "sha256:" + std::string(64, 'm');
      spec.metadata["evidence.planDigest"] = planDigest;
      spec.metadata["evidence.artifactDigest"] = spec.metadata["test.artifactDigest"];
      spec.metadata["evidence.createdAtMs"] = "1";
    }
    return spec;
  };

  for (size_t index = 0; index < environment.providerCount(); ++index) {
    auto& provider = environment.provider(index);
    if (!streamed) {
      provider.setUseTokens(false);
    }
    provider.markHybridResponseKeyWrappedForTest(serviceName);
    const std::string role = index == 0 ? "/Backbone" : "/Head/Shard/0";
    std::vector<NativeModelRunnerSpec> runnerSpecs{
      makeRunnerSpec(index, role),
    };
    if (index == 0 && !streamed) {
      runnerSpecs.push_back(makeRunnerSpec(index, "/Aux"));
    }
    NativeProviderHandlerConfig config;
    config.plan = plan;
    config.assignment = assignment;
    config.runnerFactory = useRealOnnx
      ? realRunnerFactory
      : makeNativeIngressTestRunnerFactory(
          observedMutex, observedRoles, observedInputs, observedOutputs);
    config.runnerSpecs = runnerSpecs;
    config.finalResponseScope = "detections0";
    config.localProviderName = provider.getName().toUri();
    config.providerBootId = runnerSpecs.front().metadata.at("test.providerBootId");
    config.planDigest = planDigest;
    config.fetchTimeoutMs = 3000;
    config.maxSegmentSize = 4096;
    config.freshnessMs = 60000;
    config.allowPreassembledV3Compatibility = !usePostSelectionAssembly;
    if (usePostSelectionAssembly) {
      config.runnerPreparationFactory =
        [&, index] (ServiceProvider::CollaborationContext& context,
                    const NativeSelectionProjectionV3& projection,
                    const std::shared_ptr<ProtectedRuntime>&) {
          ++preparationFactoryCalls[index];
          if (projection.canonicalArtifactName.empty() ||
              projection.canonicalArtifactName !=
                context.assignment().assignedArtifact.toUri() ||
              projection.provider != context.localProvider().toUri() ||
              projection.requestId != context.sessionId()) {
            throw std::runtime_error(
              "post-selection assembly factory received unbound projection");
          }
          const auto role = projection.executionRole.roleId;
          if (role.empty() || role != context.role()) {
            throw std::runtime_error(
              "post-selection assembly factory received wrong local role");
          }
          // This deterministic factory stands in for the adapter's canonical
          // source fetch/assembly helper.  The production implementation is
          // the same runnerPreparationFactory seam; this gate proves that a
          // V3 Selection cannot execute the startup preassembled runner.
          return makeRunnerSpec(index, role);
        };
    }
    config.groupCoordinatorFactory =
      [&, index, localProvider = provider.getName().toUri()] (
          ServiceProvider::CollaborationContext& context,
          const std::map<std::string, std::string>& fields) {
        ++coordinatorFactoryCalls[index];
        const auto field = fields.find("groupCapabilityV1");
        if (field == fields.end()) {
          throw std::runtime_error("missing request-scoped group capability");
        }
        auto decoded = ProviderGroupCoordinator::decodeCapability(
          bytesFromHex(field->second));
        if (decoded.requestId != context.sessionId() ||
            decoded.planDigest != planDigest) {
          throw std::runtime_error("group capability request/plan mismatch");
        }
        auto options = makeD2bCoordinatorOptions();
        options.localProvider = localProvider;
        auto coordinator = std::make_shared<ProviderGroupCoordinator>(
          std::move(options));
        coordinator->installCapability(std::move(decoded), {}, true);
        return coordinator;
      };
    if (useRealOnnx) {
      config.executionEvidenceObserver = std::make_shared<
        std::function<void(const ExecutionEvidence&)>>(
        [evidenceMutex, observedEvidence] (const ExecutionEvidence& evidence) {
          std::lock_guard<std::mutex> lock(*evidenceMutex);
          (*observedEvidence)[evidence.providerName + ":" + evidence.roles.front()] =
            evidence;
        });
    }
    auto runtime = makeNativeProviderCollaborationRuntime(std::move(config));
    auto nativeHandler = std::move(runtime.handler);
    provider.addCollaborationHandler(
      serviceName,
      [&, index, nativeHandler = std::move(nativeHandler)] (
          ServiceProvider::CollaborationContext& context,
          const RequestMessage& request) mutable {
        handlerEntered[index] = true;
        streamPublisherObserved[index] = context.isStreamed();
        selectionDigests[index] = context.assignment().selectionDigest;
        nativeHandler(context, request);
      });
  }

  environment.enableProductionIngressForTest();
  if (!streamed) {
    environment.user().setUseTokens(false);
    const auto assignmentKey =
      environment.user().prepareHybridSendKeyForTest(
        serviceName, "REQUEST-LARGE");
    for (size_t index = 0; index < environment.providerCount(); ++index) {
      environment.provider(index).cacheHybridReceiveKeyForTest(
        assignmentKey.keyId, assignmentKey.epochId, assignmentKey.key);
    }
  }
  else {
    // The deferred stream binding is cryptographically bound to the
    // request's one-time UserToken.  Let the real provider ACK path create
    // and echo its one-time ProviderToken, while pre-installing only the
    // LocalMock Hybrid keys needed to exercise production encryption.
    const auto selectionKey = environment.user().prepareHybridSendKeyForTest(
        serviceName, "SELECTION");
    for (size_t index = 0; index < environment.providerCount(); ++index) {
      auto& provider = environment.provider(index);
      const auto ackKey = provider.prepareHybridSendKeyForTest(
          serviceName, "ACK");
      environment.user().cacheHybridReceiveKeyForTest(
          ackKey.keyId, ackKey.epochId, ackKey.key);
      const auto responseKey = provider.prepareHybridSendKeyForTest(
          serviceName, "RESPONSE");
      environment.user().cacheHybridReceiveKeyForTest(
          responseKey.keyId, responseKey.epochId, responseKey.key);
      provider.cacheHybridReceiveKeyForTest(
          selectionKey.keyId, selectionKey.epochId, selectionKey.key);
    }
  }
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    auto providerNode = environment.profile().providerNode;
    if (index > 0) {
      providerNode.append("p" + std::to_string(index));
    }
    environment.userPubSub().subscribeToProducer(
      providerNode,
      [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        const auto response = parseResponseNameV2(publication.name);
        if (response && response->serviceName.equals(serviceName) &&
            response->requestId.equals(requestId)) {
          std::lock_guard<std::mutex> lock(responseNamesMutex);
          responseNames.insert(publication.name.toUri());
          uniqueResponsePublications = responseNames.size();
        }
      },
      true);
  }

  environment.user().setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name& requestName,
         const std::vector<ndn::Name>& providers,
         const ndn::Name& publishedService,
         const RequestMessage& request, size_t strategy) {
      if (streamed) {
        BOOST_REQUIRE(providers.empty());
        BOOST_CHECK_EQUAL(strategy, tlv::AllSelected);
      }
      else {
        BOOST_REQUIRE_EQUAL(providers.size(), 2U);
        BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
      }
      BOOST_CHECK_EQUAL(publishedService, serviceName);
      const auto requestBlock = request.WireEncode();
      const auto encrypted = makeTestHybridPublication(
        requestName, serviceName, requestId, requesterName, "REQUEST",
        ndn::Buffer(requestBlock.data(), requestBlock.size()));
      for (size_t index = 0; index < environment.providerCount(); ++index) {
        environment.provider(index).cacheHybridReceiveKeyForTest(
          encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
      }
      environment.userPubSub().publish(
        requestName,
        ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
    });

  auto makeAssignmentPayload = [&] (
      size_t index, const std::vector<std::string>& assignedRoles) {
    const auto providerName = environment.provider(index).getName();
    const auto projectedCapability = capability.projectForProvider(
      providerName.toUri());
    std::string selectionCapabilityHex = bytesToHex(
      ProviderGroupCoordinator::encodeCapability(projectedCapability));
    if (tamperCapability) {
      // Preserve the projected wire shape while changing the request-scoped
      // capability.  The Provider must reject this before native execution.
      auto& byte = selectionCapabilityHex[selectionCapabilityHex.size() / 2];
      byte = byte == '0' ? '1' : '0';
    }
    const auto artifactDigest =
      "sha256:" + std::string(64, index == 0 ? 'a' : 'b');
    const auto recipeDigest =
      "sha256:" + std::string(64, index == 0 ? 'c' : 'e');
    const auto deviceSet = useRealOnnx ? "cpu:0" : "cuda:0";
    const auto endpoint = makeV3TensorEndpointJson(
      provider0Name.toUri(), requesterName.toUri(), requestId.toUri(),
      planDigest, "backbone-to-head0", 7, "/Backbone", 0,
      "/Head/Shard/0", "\"/Head/Shard/0\"", "features",
      featureTensorDigest, pipelineLayoutDigest, pipelineLayoutDigest,
      "PIPELINE_TRANSFER",
      featureTensorDigest, featureTensorDigest);
    const auto dependenciesJson = std::string("[{\"consumers\":[\"/Head/Shard/0\"],") +
      "\"expected_segments\":0,\"key_scope\":\"backbone-to-head0\"," +
      "\"object_name_template\":\"{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}\"," +
      "\"producers\":[\"/Backbone\"],\"required\":true," +
      "\"tensors\":[\"features\"],\"topic_prefix\":\"/activation\"," +
      "\"transportProfile\":\"NDNSF_DATA_V1\"," +
      "\"collectiveOperationIndex\":7,\"collectiveProducerRank\":\"0\"," +
      "\"collectiveSourceLayoutDigest\":\"" + pipelineLayoutDigest + "\"," +
      "\"collectiveTargetLayoutDigest\":\"" + pipelineLayoutDigest + "\"," +
      "\"collectiveTensorDigest\":\"" + featureTensorDigest + "\"}]";
    std::vector<ndn::Buffer> assignmentItems;
    assignmentItems.reserve(assignedRoles.size());
    const auto addAssignment = [&] (const std::string& assignedRole) {
      const auto roleKind = assignedRole == "/Aux"
        ? "COMPONENT_SET" : "TENSOR_RANK";
      const auto roleJson = makeV3SelectionRoleJson(
        assignedRole, 0, artifactDigest, recipeDigest, "onnxruntime",
        deviceSet, roleKind, usePostSelectionAssembly);
      const auto isProducer = assignedRole == "/Backbone";
      const auto isConsumer = assignedRole == "/Head/Shard/0";
      const auto dataflow = std::string("{\"attempt\":1,\"dataflow_digest\":\"") +
        planDigest + "\",\"may_publish\":[" +
        (isProducer ? endpoint : std::string()) +
        "],\"must_fetch\":[" +
        (isConsumer ? endpoint : std::string()) +
        "],\"plan_digest\":\"" + planDigest +
        "\",\"request_id\":\"" + requestId.toUri() +
        "\",\"role\":\"" + assignedRole +
        "\",\"terminal_response_owner\":" +
        (assignedRole == "/Head/Shard/0" ? "true" : "false") +
        ",\"wait_for\":[]}";
      const auto text = makeV3SelectionProjectionJson(
        roleJson, assignedRole, assignedRole, 0, providerName.toUri(),
        requestId.toUri(), planDigest, selectionCapabilityHex,
        dependenciesJson, (isProducer || assignedRole == "/Aux")
          ? withApplicationInput(dataflow, requesterName.toUri(), requestId.toUri(),
                                 planDigest, assignedRole) : dataflow,
        artifactDigest, deviceSet, {}, 0, assignedRole == "/Aux" ? 0 : 1);
      CollaborationAssignmentEnvelope envelope;
      envelope.role = assignedRole;
      envelope.assignedArtifact = ndn::Name("/artifact").append(assignedRole);
      envelope.opaquePayload = ndn::Buffer(
        reinterpret_cast<const uint8_t*>(text.data()), text.size());
      assignmentItems.push_back(
        encodeCollaborationAssignmentEnvelope(envelope));
    };
    for (const auto& assignedRole : assignedRoles) {
      addAssignment(assignedRole);
    }
    return assignmentItems.size() == 1
      ? assignmentItems.front()
      : encodeOpaqueAssignmentSet(assignmentItems);
  };

  std::map<std::string, ndn::Buffer> streamedRoleAssignments;
  std::map<std::string, ndn::Name> streamedRoleProviders;
  if (streamed) {
    // ParticipantSelectionPolicy returns the application's opaque projection
    // bytes. CommitCollaborationPlan supplies the framework envelope; passing
    // an already-enveloped value here would create a nested envelope that the
    // Provider cannot parse as the V3 JSON projection.
    for (const auto& [role, providerIndex] :
         std::map<std::string, size_t>{{"/Backbone", 0},
                                       {"/Head/Shard/0", 1}}) {
      const auto enveloped = makeAssignmentPayload(providerIndex, {role});
      CollaborationAssignmentEnvelope envelope;
      if (!decodeCollaborationAssignmentEnvelope(enveloped, envelope) ||
          envelope.role != role || envelope.opaquePayload.empty()) {
        throw std::runtime_error(
          "streamed test failed to build opaque V3 assignment projection");
      }
      streamedRoleAssignments.emplace(role, std::move(envelope.opaquePayload));
    }
    streamedRoleProviders.emplace("/Backbone", provider0Name);
    streamedRoleProviders.emplace("/Head/Shard/0", provider1Name);

    // CommitCollaborationPlan publishes Selection through this production
    // local-publication boundary. No test calls the Provider selection
    // parser directly in the streamed path.
    environment.user().setLocalPublicationHandler(
      [&environment] (const ndn::Name& messageName, const ndn::Buffer& wire) {
        if (!parseServiceSelectionNameV2(messageName)) return;
        environment.userPubSub().publish(
          messageName, ndn::span<const uint8_t>(wire.data(), wire.size()));
      });
  }

  RequestMessage request;
  const std::string requestText = "d2b-native-payload";
  ndn::Buffer requestPayload;
  if (useRealOnnx) {
    const std::array<float, 3> values{{1.0F, 2.0F, 3.0F}};
    requestPayload = ndn::Buffer(
      reinterpret_cast<const uint8_t*>(values.data()), sizeof(values));
  }
  else {
    requestPayload = ndn::Buffer(
      reinterpret_cast<const uint8_t*>(requestText.data()), requestText.size());
  }
  request.setPayload(requestPayload, requestPayload.size());
  request.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());
  ndn::Name returnedRequestId;
  if (streamed) {
    StreamRequestOptions streamOptions;
    streamOptions.maxEvents = 4;
    streamOptions.interestWindow = 2;
    streamOptions.reorderCapacity = 2;
    streamOptions.publisherQueueCapacity = 2;
    streamOptions.retentionMs = 3000;
    streamOptions.maxEventWireBytes = 4096;

    returnedRequestId = environment.user().BeginCollaboration(
      serviceName, requestPayload, 200, 8000,
      [&] (const CollaborationAckClosure& closure) {
        ackObserved = closure.candidates.size() == 2;
        CollaborationPlan streamPlan;
        streamPlan.ackCollectionTimeMs = 200;
        streamPlan.timeoutMs = 8000;
        for (const auto& [role, provider] : streamedRoleProviders) {
          CollaborationRoleSpec roleSpec;
          roleSpec.role = role;
          roleSpec.service = serviceName;
          roleSpec.requiredArtifact = ndn::Name("/artifact").append(role);
          roleSpec.terminalResponseOwner = role == "/Head/Shard/0";
          streamPlan.roles.push_back(std::move(roleSpec));
        }
        streamPlan.keyScopes.push_back(CollaborationKeyScope{
          "backbone-to-head0", {"/Backbone", "/Head/Shard/0"}});
        streamPlan.dependencies.push_back(CollaborationDependency{
          {"/Backbone"}, {"/Head/Shard/0"}, "backbone-to-head0",
          ndn::Name("/activation"), true});
        streamPlan.participantSelector =
          std::make_shared<StreamedNativeD2bSelection>(
            streamedRoleProviders, streamedRoleAssignments);
        try {
          streamedPlanCommitted = environment.user().CommitCollaborationPlan(
            closure.requestId, closure.digest, std::move(streamPlan));
        }
        catch (const std::exception& error) {
          std::lock_guard<std::mutex> lock(streamedResultMutex);
          streamedErrorMessage = error.what();
          streamedError = true;
        }
      },
      [&] (const ResponseMessage&) {},
      [&] (const ndn::Name&) { timedOut = true; },
      requestId,
      CollaborationAckCoverageHandler(),
      RequestCapabilities(),
      std::optional<StreamRequestOptions>(streamOptions),
      [&] (const ndn::Buffer& event) {
        std::lock_guard<std::mutex> lock(streamedResultMutex);
        streamedEvents.emplace_back(
          reinterpret_cast<const char*>(event.data()), event.size());
      },
      [&] (const ndn::Buffer& response) {
        std::lock_guard<std::mutex> lock(streamedResultMutex);
        streamedResult.assign(
          reinterpret_cast<const char*>(response.data()), response.size());
        streamedComplete = true;
      },
      [&] (const StreamedInvocationError& error) {
        std::lock_guard<std::mutex> lock(streamedResultMutex);
        streamedErrorMessage = error.message;
        streamedError = true;
      });
  }
  else {
    returnedRequestId = environment.user().RequestService(
      std::vector<ndn::Name>{provider0Name, provider1Name}, serviceName, request, 200,
      ServiceUser::AckCandidatesHandler(
        [&] (const std::vector<AckSelectionCandidate>& candidates) {
          ackObserved = candidates.size() == 2;
          if (candidates.size() == 2) {
            BOOST_REQUIRE(environment.user().setSelectionAssignmentPayloadForRequest(
              requestId,
              provider0Name,
              makeAssignmentPayload(
                0, std::vector<std::string>{"/Backbone", "/Aux"})));
            BOOST_REQUIRE(environment.user().setSelectionAssignmentPayloadForRequest(
              requestId,
              provider1Name,
              makeAssignmentPayload(
                1, std::vector<std::string>{"/Head/Shard/0"})));
          }
          return candidates;
        }),
      8000,
      [&] (const ndn::Name&) { timedOut = true; },
      [&] (const ResponseMessage&) {},
      tlv::FirstResponding,
      requestId);
  }
  BOOST_REQUIRE_EQUAL(returnedRequestId, requestId);

  environment.pumpUntil([&] {
    return (environment.provider(0).getPendingRequestCountForTesting() == 1 &&
            environment.provider(1).getPendingRequestCountForTesting() == 1) ||
           timedOut;
  });
  if (!streamed) {
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    RequestAckMessage ack;
    ack.setStatus(true);
    ack.setMessage("d2b-native-ack-" + std::to_string(index));
    if (streamed) {
      const auto certificate = environment.keyChain().getPib().getIdentity(
          environment.provider(index).getName())
        .getDefaultKey().getDefaultCertificate();
      const auto publicKey = certificate.getPublicKey();
      ndn::Buffer publicKeyBuffer(publicKey.begin(), publicKey.end());
      ndn::util::Sha256 digest;
      digest << std::string(reinterpret_cast<const char*>(publicKey.data()),
                            publicKey.size());
      SelectionInputKeyOffer offer;
      offer.setField("schemaVersion", "NDNSF-STREAM-GRANT-V1");
      offer.setField("recipient", environment.provider(index).getName().toUri());
      offer.setField("recipientCertName", certificate.getName().toUri());
      offer.setField("recipientPublicKey", selectionGatedHex(publicKeyBuffer));
      offer.setField("recipientCertDigest", "sha256:" + digest.toString());
      offer.setField("providerBootEpoch",
                     environment.provider(index).getName().toUri() + ":" +
                       environment.provider(index).getProviderBootEpoch());
      ack.setSelectionInputKeyOffer(offer);
    }
    const auto ackName = makeRequestAckNameV2(
      environment.provider(index).getName(), requesterName, serviceName, requestId);
    const auto ackBlock = ack.WireEncode();
    const auto encrypted = makeTestHybridPublication(
      ackName, serviceName, requestId, environment.provider(index).getName(), "ACK",
      ndn::Buffer(ackBlock.data(), ackBlock.size()));
    environment.user().cacheHybridReceiveKeyForTest(
      encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
    environment.providerPubSub(index).publish(
      ackName,
      ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
  }
  }

  if (streamed) {
    environment.pumpUntil([&] {
      return streamedPlanCommitted || streamedError || timedOut;
    });
  }
  const auto selectionFailed = [&] {
    for (size_t index = 0; index < environment.providerCount(); ++index) {
      if (selectionDigests[index].empty()) {
        continue;
      }
      const auto status = environment.provider(index).getSelectionExecutionStatus(
        selectionDigests[index]);
      if (status && (status->state == SelectionExecutionState::Failed ||
                     status->state == SelectionExecutionState::Rejected)) {
        return true;
      }
    }
    return false;
  };
  environment.pumpUntil([&] {
    if (streamed) {
      // Response publication and the terminal End event are independent SVS
      // publications.  Do not stop the production gate when the Response is
      // observed before the consumer has validated End and fired onComplete.
      return streamedComplete || streamedError || timedOut ||
             (tamperCapability && selectionFailed());
    }
    return uniqueResponsePublications.load() == 1 || streamedComplete ||
           streamedError || timedOut ||
           (tamperCapability && selectionFailed());
  });

  BOOST_CHECK(ackObserved);
  if (!tamperCapability) {
    BOOST_CHECK(handlerEntered[0]);
    BOOST_CHECK(handlerEntered[1]);
    BOOST_CHECK_EQUAL(coordinatorFactoryCalls[0].load(), streamed ? 1U : 2U);
    BOOST_CHECK_EQUAL(coordinatorFactoryCalls[1].load(), 1U);
    BOOST_CHECK_EQUAL(uniqueResponsePublications.load(), 1U);
    BOOST_CHECK(!timedOut);
    if (streamed) {
      std::lock_guard<std::mutex> lock(streamedResultMutex);
      BOOST_CHECK(streamedPlanCommitted);
      BOOST_CHECK(streamedComplete);
      BOOST_CHECK(!streamedError);
      // Every selected role receives the streamed Request options, but only
      // the terminal role receives the Provider-specific event-key grant and
      // may publish user-facing events.  The Backbone remains data-plane only.
      BOOST_CHECK(!streamPublisherObserved[0]);
      BOOST_CHECK(streamPublisherObserved[1]);
      BOOST_REQUIRE_EQUAL(streamedEvents.size(), 1U);
      BOOST_CHECK_EQUAL(streamedEvents.front(), "detections0:features:d2b-native-payload");
      BOOST_CHECK_EQUAL(streamedResult, "detections0:features:d2b-native-payload");
    }
  }
  else {
    BOOST_CHECK(selectionFailed());
    BOOST_CHECK_EQUAL(uniqueResponsePublications.load(), 0U);
    BOOST_CHECK(!timedOut);
  }
  if (!tamperCapability && !useRealOnnx) {
    std::lock_guard<std::mutex> lock(*observedMutex);
    BOOST_CHECK(observedRoles->count("/Backbone") != 0);
    BOOST_CHECK(observedRoles->count("/Head/Shard/0") != 0);
    if (!streamed) {
      BOOST_CHECK(observedRoles->count("/Aux") != 0);
      BOOST_REQUIRE(observedOutputs->count("/Aux") != 0);
      BOOST_CHECK_EQUAL(
        observedOutputs->at("/Aux").at("aux"),
        "aux:d2b-native-payload");
    }
    BOOST_REQUIRE(observedInputs->count("/Head/Shard/0") != 0);
    const auto& headInputs = observedInputs->at("/Head/Shard/0");
    BOOST_REQUIRE(headInputs.count("backbone-to-head0") != 0);
    BOOST_CHECK_EQUAL(headInputs.at("backbone-to-head0"),
                      "features:d2b-native-payload");
    BOOST_REQUIRE(observedOutputs->count("/Head/Shard/0") != 0);
    BOOST_CHECK_EQUAL(
      observedOutputs->at("/Head/Shard/0").at("detections0"),
      "detections0:features:d2b-native-payload");
  }
  else if (!tamperCapability) {
    std::lock_guard<std::mutex> lock(*evidenceMutex);
    BOOST_REQUIRE_EQUAL(observedEvidence->size(), 3U);
    for (const auto& item : *observedEvidence) {
      const auto& evidence = item.second;
      BOOST_CHECK(evidence.runnerKind == RunnerKind::OnnxRuntimeCpu);
      BOOST_CHECK(evidence.realCompute);
      BOOST_CHECK(!evidence.cpuFallbackUsed);
      BOOST_CHECK(evidence.loadCompleted);
      BOOST_CHECK(evidence.warmupCompleted);
      BOOST_CHECK_EQUAL(evidence.planDigest, planDigest);
      BOOST_REQUIRE_EQUAL(evidence.artifactDigests.size(), 1U);
    }
    BOOST_CHECK(observedEvidence->count(
      provider0Name.toUri() + ":/Backbone") != 0);
    BOOST_CHECK(observedEvidence->count(
      provider0Name.toUri() + ":/Aux") != 0);
    BOOST_CHECK(observedEvidence->count(
      provider1Name.toUri() + ":/Head/Shard/0") != 0);
  }
  if (!tamperCapability && usePostSelectionAssembly) {
    BOOST_CHECK_EQUAL(preparationFactoryCalls[0].load(), streamed ? 1U : 2U);
    BOOST_CHECK_EQUAL(preparationFactoryCalls[1].load(), 1U);
  }
}

struct Spec175NativeTinyStreamResult
{
  std::size_t ackCandidates = 0;
  std::string ackProvider;
  std::string ackService;
  bool requestObserved = false;
  bool ackPublicationObserved = false;
  std::string requestPublicationName;
  std::size_t requestPublicationCount = 0;
  std::string ackPublicationName;
  bool ackClosed = false;
  bool recoveryAckClosed = false;
  bool planCommitted = false;
  bool timedOut = false;
  bool completed = false;
  bool failed = false;
  bool cancelled = false;
  bool streamedContext = false;
  std::size_t streamedProviderCount = 0;
  std::size_t replacementProviderExecutions = 0;
  std::size_t replacementEventsPublished = 0;
  std::size_t providerTransportDetachments = 0;
  std::size_t unselectedProviderExecutions = 0;
  std::size_t providerCoordinatorCompletions = 0;
  std::size_t providerFailures = 0;
  std::vector<SelectionExecutionStatus> coreDeadlineFailures;
  bool permutedRoleProviderMap = false;
  std::vector<std::string> events;
  std::string finalPayload;
  std::string error;
  std::string providerFailureError;
  StreamedInvocationErrorCode errorCode = StreamedInvocationErrorCode::EventTimeout;
  ndn::Name errorRequestId;
  ndn::Name errorProviderName;
  uint64_t errorExpectedCursor = 0;
  test::PacketBridgeStats bridgeStats;
  std::size_t retentionSuppressions = 0;
  std::size_t retentionExpirations = 0;
  std::size_t publicationSuppressions = 0;
  std::size_t publicationReorders = 0;
  std::size_t publicationDuplicates = 0;
  std::size_t tamperedPublications = 0;
  std::size_t publisherQueueHighWater = 0;
  std::size_t callbackQueueHighWater = 0;
  std::size_t callbackConcurrencyHighWater = 0;
  std::vector<ProviderDecodeStateSnapshot> decodeStateSnapshots;
  std::vector<NativeEpochCoordinatorResult::CacheObservation> cacheObservations;
  std::vector<std::int64_t> fullPrefixControlTokens;
  std::vector<std::size_t> fullPrefixControlInputExtents;
};

enum class Spec175NativeTinyFault
{
  None,
  ReorderEvent3After4,
  DuplicateEvent4,
  DropFirstEvent5Data,
  NeverRetainedEvent5,
  RetentionExpiredEvent5,
  EndBeforeGapEvent5,
  CancelAfterThirdEvent,
  ExpiredDeadline,
  CallbackThrowsAtThirdEvent,
  TamperEventOneSignature,
  SlowConsumerCapacityOne,
  ProviderUnavailableAfterEvent3,
  ProviderUnavailableAfterEvent3WithReplacement,
};

struct Spec175NativeTinyCaseOptions
{
  std::string caseId;
  bool permuteRoleProviders = false;
  bool extraUnselectedProvider = false;
  bool allowReplacement = false;
  bool suppressProviderPeerSyncAfterCommit = false;
  Spec175NativeTinyFault fault = Spec175NativeTinyFault::None;
  std::string samplingMode = "Greedy";
  double samplingTemperature = 0.0;
  std::size_t samplingTopK = 1;
  double samplingTopP = 1.0;
  double samplingRepetitionPenalty = 1.0;
  std::uint64_t samplingSeed = 1'750'001;
  std::vector<std::string> stopStrings;
  std::filesystem::path tokenizerPath;
  std::string tokenizerDigest;
};

std::filesystem::path
findSpec175TinyOneRoleFixture()
{
  const auto relative = std::filesystem::path(
    "tests/fixtures/spec175/tiny-causal-lm-v1/one-role/role-0.onnx");
  for (const auto& root : {std::filesystem::current_path(),
                           std::filesystem::current_path().parent_path(),
                           std::filesystem::current_path().parent_path().parent_path()}) {
    const auto candidate = root / relative;
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  return {};
}

std::filesystem::path
findSpec175TinyTwoRoleFixture()
{
  const auto relative = std::filesystem::path(
    "tests/fixtures/spec175/tiny-causal-lm-v1/two-role");
  for (const auto& root : {std::filesystem::current_path(),
                           std::filesystem::current_path().parent_path(),
                           std::filesystem::current_path().parent_path().parent_path()}) {
    const auto candidate = root / relative;
    if (std::filesystem::exists(candidate / "role-0.onnx") &&
        std::filesystem::exists(candidate / "role-1.onnx")) {
      return candidate;
    }
  }
  return {};
}

std::filesystem::path
findSpec175TinyRoleFixture(std::size_t providerCount)
{
  if (providerCount == 2) {
    return findSpec175TinyTwoRoleFixture();
  }
  const auto relative = std::filesystem::path(
    "tests/fixtures/spec175/tiny-causal-lm-v1/four-role");
  for (const auto& root : {std::filesystem::current_path(),
                           std::filesystem::current_path().parent_path(),
                           std::filesystem::current_path().parent_path().parent_path()}) {
    const auto candidate = root / relative;
    bool complete = true;
    for (std::size_t index = 0; index < providerCount; ++index) {
      complete = complete && std::filesystem::exists(
        candidate / ("role-" + std::to_string(index) + ".onnx"));
    }
    if (complete) {
      return candidate;
    }
  }
  return {};
}

std::filesystem::path
findSpec175TinyStandaloneTokenizer()
{
  const auto relative = std::filesystem::path(
    "tests/fixtures/spec175/tiny-causal-lm-v1/standalone/tokenizer.json");
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

std::filesystem::path
findSpec175TinyUnicodeTokenizer()
{
  const auto relative = std::filesystem::path(
    "tests/fixtures/spec175/tiny-causal-lm-v1/standalone/unicode/tokenizer.json");
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

ndn::Buffer
makeSpec175TinyRequestPayload()
{
  const std::int64_t token = 3;
  std::vector<std::uint8_t> bytes(sizeof(token));
  std::memcpy(bytes.data(), &token, sizeof(token));
  const auto bundle = ndnsf::di::makeEncodedTensorBundle(
    "input_ids",
    {ndnsf::di::NamedTensor{
      "input_ids", ndnsf::di::TensorElementType::Int64, {1, 1}, std::move(bytes)}});
  return ndn::Buffer(bundle.payload.data(), bundle.payload.size());
}

struct Spec175TinyFullPrefixControlResult
{
  std::vector<std::int64_t> tokens;
  std::vector<std::size_t> inputExtents;
};

Spec175TinyFullPrefixControlResult
runSpec175TinyFullPrefixControl(const NativeModelRunnerSpec& spec);

Spec175NativeTinyStreamResult
runSpec175NativeTinyOneRoleCase()
{
  Spec175NativeTinyStreamResult result;
#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  result.failed = true;
  result.error = "NDNSF_DI_ENABLE_ONNXRUNTIME_CPP is disabled";
  return result;
#else
  const auto fixture = findSpec175TinyOneRoleFixture();
  if (fixture.empty()) {
    result.failed = true;
    result.error = "Spec175 tiny one-role ONNX fixture is unavailable";
    return result;
  }
  const auto tokenizerPath = findSpec175TinyStandaloneTokenizer();
  BOOST_REQUIRE(!tokenizerPath.empty());
  ScopedSpec175CertifiedModels certifiedModels("i01");
  const auto certifiedModel = certifiedModels.materialize(fixture, 0);

  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Formal/NativeTiny/I01");
  profile.providerCount = 1;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  const auto providerName = environment.provider().getName();
  const auto requestId = ndn::Name("/spec175-native-tiny-i01");
  const std::string role = "/LLM/Pipeline/Stage/0";
  const std::string planDigest = "sha256:" + std::string(64, '1');
  const std::string artifactDigest = "sha256:" + std::string(64, '2');
  const std::string modelDigest = "sha256:" + std::string(64, '3');
  const std::string recipeDigest = "sha256:" + std::string(64, '4');
  const std::string samplingDigest =
    "sha256:1750001000000000000000000000000000000000000000000000000000000000";
  const std::string tokenizerDigest =
    "sha256:bf0f0fa65dc5aafe690ee497b4c8e2abe408fb4788c5ef035964dc94f15ca5a6";
  const std::string providerBootId = "spec175-native-tiny-i01-boot";
  const auto makeCapabilityAssignment = [&] {
    return std::string("role=") + role +
      ";provider=" + providerName.toUri() +
      ";backend=onnxruntime;device=cpu:0;artifactDigest=" + artifactDigest +
      ";recipeDigest=" + recipeDigest + ";executionPolicy=DATA_DRIVEN_V2;";
  };

  NativeExecutionPlan plan;
  plan.serviceName = serviceName.toUri();
  plan.modelName = "spec175-tiny-causal-lm-v1";
  plan.modelFamily = "spec175-tiny-causal-lm";
  plan.modelFormat = "onnx";
  plan.plannerKind = "PreSplitFirstStrategy";
  plan.executionPolicy = "DATA_DRIVEN_V2";
  plan.roles = {role};
  NativeDependencySpec feedback;
  feedback.producers = {role};
  feedback.consumers = {role};
  feedback.keyScope = "token-feedback";
  feedback.topicPrefix = "/Spec175/Formal/NativeTiny/I01/feedback";
  feedback.objectNameTemplate =
    "{producerProvider}/NDNSF/DI/FEEDBACK/{sessionId}/{producerRole}/bundle/{sequence}";
  feedback.operationKind = "TOKEN_FEEDBACK";
  feedback.useNdnsfDataV1 = true;
  feedback.collectiveOperationIndex = 0;
  feedback.collectiveProducerRank = "0";
  feedback.collectiveSourceLayoutDigest = spec175Digest('c');
  feedback.collectiveTargetLayoutDigest = feedback.collectiveSourceLayoutDigest;
  feedback.collectiveTensorDigest = spec175Digest('d');
  plan.dependencies = {feedback};

  NativeProviderAssignment assignment;
  assignment.providerByRole.emplace(role, providerName.toUri());

  NativeModelRunnerSpec runnerSpec;
  runnerSpec.role = role;
  runnerSpec.kind = "spec175-tiny-stateful-onnx";
  runnerSpec.backend = "onnxruntime";
  runnerSpec.path = certifiedModel.string();
  runnerSpec.metadata["executionProvider"] = "cpu";
  runnerSpec.metadata["statefulModel"] = "true";
  runnerSpec.metadata["streamingGeneration"] = "true";
  runnerSpec.metadata["maxGeneratedTokens"] = "8";
  runnerSpec.metadata["eosTokenIds"] = "2";
  runnerSpec.metadata["samplingDigest"] = samplingDigest;
  runnerSpec.metadata["inputNames"] =
    "input_ids,attention_kv_in,recurrent_state_in,convolution_state_in";
  runnerSpec.metadata["outputNames"] =
    "logits,attention_kv_out,recurrent_state_out,convolution_state_out";
  runnerSpec.metadata["stateInputNames"] =
    "attention_kv_in,recurrent_state_in,convolution_state_in";
  runnerSpec.metadata["stateOutputNames"] =
    "attention_kv_out,recurrent_state_out,convolution_state_out";
  runnerSpec.metadata["fragmentDigest"] = artifactDigest;
  runnerSpec.metadata["evidence.providerName"] = providerName.toUri();
  runnerSpec.metadata["evidence.providerBootId"] = providerBootId;
  runnerSpec.metadata["evidence.epoch"] = "1";
  runnerSpec.metadata["evidence.modelDigest"] = modelDigest;
  runnerSpec.metadata["evidence.planDigest"] = planDigest;
  runnerSpec.metadata["evidence.artifactDigest"] = artifactDigest;
  runnerSpec.metadata["evidence.createdAtMs"] = "1";
  bindSpec175CertifiedRunnerMetadata(
    runnerSpec, 0, 1, artifactDigest, recipeDigest);

  std::vector<GroupOperationV1> operations;
  for (std::uint64_t index = 0; index < 9; ++index) {
    GroupOperationV1 operation;
    operation.operationIndex = index;
    operation.kind = "TOKEN_FEEDBACK";
    operation.producerRanks = {"0"};
    operation.consumerRanks = {"0"};
    operation.tensorLayoutDigest = feedback.collectiveSourceLayoutDigest;
    operation.maxBytes = 4096;
    operation.maxSegments = 1;
    operations.push_back(std::move(operation));
  }
  auto providerPrefix = environment.profile().providerNode;
  providerPrefix.append("0");
  ProviderGroupCoordinator capabilitySealer(makeD2bCoordinatorOptions());
  const auto capability = capabilitySealer.createCapability(
    requestId.toUri(), "attempt-1", planDigest, "group-spec175-i01", 1,
    {{providerName.toUri(), 0, "offer-p0", providerPrefix.toUri()}},
    operations, 4096, 2000, 8000);
  const auto providerCapabilityHex = bytesToHex(
    ProviderGroupCoordinator::encodeCapability(
      capability.projectForProvider(providerName.toUri())));
  const auto publishEndpoint = makeV3TensorEndpointJson(
    providerPrefix.toUri(), requesterName.toUri(), requestId.toUri(),
    planDigest, "group-spec175-i01", 0, role, 0, role, "\"" + role + "\"",
    "input_ids", feedback.collectiveTensorDigest,
    feedback.collectiveSourceLayoutDigest, feedback.collectiveTargetLayoutDigest,
    "TOKEN_FEEDBACK", feedback.collectiveTensorDigest,
    feedback.collectiveTensorDigest);
  const auto fetchEndpoint = makeV3TensorEndpointJson(
    providerPrefix.toUri(), requesterName.toUri(), requestId.toUri(),
    planDigest, "group-spec175-i01", 0, role, 0, role, "\"" + role + "\"",
    "input_ids", feedback.collectiveTensorDigest,
    feedback.collectiveSourceLayoutDigest, feedback.collectiveTargetLayoutDigest,
    "TOKEN_FEEDBACK", spec175Digest('b'), feedback.collectiveTensorDigest);
  const auto dependenciesJson = std::string("[{\"consumers\":[\"") + role +
    "\"],\"expected_segments\":0,\"key_scope\":\"token-feedback\"," +
    "\"object_name_template\":\"{producerProvider}/NDNSF-DI/{sessionId}/" +
    "{producerRole}/{role}/{sequence}\",\"operationKind\":\"TOKEN_FEEDBACK\"," +
    "\"producers\":[\"" + role + "\"],\"required\":true," +
    "\"tensors\":[\"input_ids\"],\"topic_prefix\":\"/Spec175/feedback\"," +
    "\"transportProfile\":\"NDNSF_DATA_V1\",\"collectiveOperationIndex\":0," +
    "\"collectiveProducerRank\":\"0\",\"collectiveSourceLayoutDigest\":\"" +
    feedback.collectiveSourceLayoutDigest +
    "\",\"collectiveTargetLayoutDigest\":\"" +
    feedback.collectiveTargetLayoutDigest +
    "\",\"collectiveTensorDigest\":\"" + feedback.collectiveTensorDigest +
    "\"}]";
  const auto roleJson = makeSpec175CertifiedRoleJson(
    role, 0, 1, artifactDigest, recipeDigest);
  const auto dataflowJson = std::string("{\"attempt\":1,\"dataflow_digest\":\"") +
    planDigest + "\",\"may_publish\":[" + publishEndpoint +
    "],\"must_fetch\":[" + fetchEndpoint + "],\"plan_digest\":\"" + planDigest +
    "\",\"request_id\":\"" + requestId.toUri() + "\",\"role\":\"" + role +
    "\",\"terminal_response_owner\":true,\"wait_for\":[]}";
  const auto projectionJson = makeV3SelectionProjectionJson(
    roleJson, role, role, 0, providerName.toUri(), requestId.toUri(), planDigest,
    providerCapabilityHex, dependenciesJson,
    withApplicationInput(dataflowJson, requesterName.toUri(), requestId.toUri(),
                         planDigest, role), artifactDigest, "cpu:0",
    makeSpec175GenerationContractJson(
      8, 1, samplingDigest, tokenizerDigest), 0, 4);
  std::map<std::string, ndn::Buffer> selectionAssignmentByProvider;
  selectionAssignmentByProvider.emplace(
    providerName.toUri(),
    ndn::Buffer(reinterpret_cast<const std::uint8_t*>(projectionJson.data()),
                projectionJson.size()));

  auto runnerFactory = std::make_shared<RegistryNativeModelRunnerFactory>();
  registerOnnxRuntimeBackend(*runnerFactory);
  NativeProviderHandlerConfig handlerConfig;
  handlerConfig.plan = plan;
  handlerConfig.assignment = assignment;
  handlerConfig.runnerFactory = runnerFactory;
  handlerConfig.runnerSpecs = {runnerSpec};
  handlerConfig.finalResponseScope = "final-response";
  handlerConfig.localProviderName = providerName.toUri();
  handlerConfig.providerBootId = providerBootId;
  handlerConfig.planDigest = planDigest;
  handlerConfig.fetchTimeoutMs = 3000;
  handlerConfig.maxSegmentSize = 4096;
  handlerConfig.freshnessMs = 60000;
  handlerConfig.enableNativeEpochCoordinator = true;
  handlerConfig.maxGenerationEpochs = 8;
  handlerConfig.generationStateInputNames = {
    "attention_kv_in", "recurrent_state_in", "convolution_state_in"};
  handlerConfig.generationStateOutputNames = {
    "attention_kv_out", "recurrent_state_out", "convolution_state_out"};
  handlerConfig.generationEosTokenIds = {2};
  handlerConfig.generationSamplingDigest =
    runnerSpec.metadata.at("samplingDigest");
  handlerConfig.generationDecodersFactory =
    [tokenizerPath] (const std::string& digest) {
      NativeStandaloneTokenizerOptions options;
      options.tokenizerPath = tokenizerPath.string();
      return makeNativeStandaloneTokenizerDecoders(std::move(options), digest);
    };
  handlerConfig.requireGenerationTextOutput = true;
  handlerConfig.allowPreassembledV3Compatibility = true;
  auto cacheObservations = std::make_shared<
    std::vector<NativeEpochCoordinatorResult::CacheObservation>>();
  auto cacheObservationMutex = std::make_shared<std::mutex>();
  handlerConfig.epochCoordinatorCompletionObserver =
    std::make_shared<NativeProviderHandlerConfig::EpochCoordinatorCompletionObserver>(
      [cacheObservations, cacheObservationMutex] (
          const std::string&, const NativeEpochCoordinatorResult& coordinated) {
        std::lock_guard<std::mutex> lock(*cacheObservationMutex);
        *cacheObservations = coordinated.cacheObservations;
      });
  handlerConfig.groupCoordinatorFactory =
    [providerName = providerName.toUri(), planDigest] (
        ServiceProvider::CollaborationContext& context,
        const std::map<std::string, std::string>& fields) {
      const auto field = fields.find("groupCapabilityV1");
      if (field == fields.end()) {
        throw std::runtime_error("Spec175 I01 assignment has no GroupCapabilityV1");
      }
      auto decoded = ProviderGroupCoordinator::decodeCapability(
        bytesFromHex(field->second));
      if (decoded.requestId != context.sessionId() ||
          decoded.planDigest != planDigest) {
        throw std::runtime_error("Spec175 I01 GroupCapabilityV1 binding mismatch");
      }
      auto options = makeD2bCoordinatorOptions();
      options.localProvider = providerName;
      auto coordinator = std::make_shared<ProviderGroupCoordinator>(
        std::move(options));
      coordinator->installCapability(std::move(decoded), {}, true);
      return coordinator;
    };
  auto nativeRuntime = makeNativeProviderCollaborationRuntime(
    std::move(handlerConfig));
  auto decodeStateSnapshotReader = nativeRuntime.decodeStateSnapshot;
  auto nativeHandler = std::move(nativeRuntime.handler);
  const auto capabilityAssignment = makeCapabilityAssignment();
  environment.provider().addCollaborationHandler(
    serviceName,
    [capabilityAssignment] (const RequestMessage&) {
      ServiceProvider::AckDecision decision;
      decision.status = true;
      decision.payload = ndn::Buffer(
        reinterpret_cast<const std::uint8_t*>(capabilityAssignment.data()),
        capabilityAssignment.size());
      return decision;
    },
    [&, nativeHandler = std::move(nativeHandler)] (
          ServiceProvider::CollaborationContext& context,
          const RequestMessage& request) mutable {
      result.streamedContext = context.isStreamed();
      nativeHandler(context, request);
    });

  environment.enableProductionIngressForTest();
  environment.provider().markHybridResponseKeyWrappedForTest(serviceName);
  const auto selectionKey = environment.user().prepareHybridSendKeyForTest(
    serviceName, "SELECTION");
  const auto ackKey = environment.provider().prepareHybridSendKeyForTest(
    serviceName, "ACK");
  environment.user().cacheHybridReceiveKeyForTest(
    ackKey.keyId, ackKey.epochId, ackKey.key);
  const auto responseKey = environment.provider().prepareHybridSendKeyForTest(
    serviceName, "RESPONSE");
  environment.user().cacheHybridReceiveKeyForTest(
    responseKey.keyId, responseKey.epochId, responseKey.key);
  environment.provider().cacheHybridReceiveKeyForTest(
    selectionKey.keyId, selectionKey.epochId, selectionKey.key);

  // Observe both production SVS publication boundaries.  These diagnostics
  // are intentionally independent of the User ACK closure: an empty closure
  // must tell us whether the Provider never emitted an ACK or the User could
  // not match/decrypt the emitted ACK.
  environment.providerPubSub().subscribeToProducer(
    environment.profile().userNode,
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      if (const auto parsed = parseRequestNameV2(publication.name);
          parsed && parsed->serviceName.equals(serviceName) &&
          parsed->requestId.equals(requestId)) {
        result.requestObserved = true;
        result.requestPublicationName = publication.name.toUri();
      }
    },
    true);
  auto providerProducer = environment.profile().providerNode;
  providerProducer.append("0");
  environment.userPubSub().subscribeToProducer(
    providerProducer,
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      if (const auto parsed = parseRequestAckNameV2(publication.name);
          parsed && parsed->serviceName.equals(serviceName) &&
          parsed->requestId.equals(requestId)) {
        result.ackPublicationObserved = true;
        result.ackPublicationName = publication.name.toUri();
      }
    },
    true);

  environment.user().setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name& requestName,
         const std::vector<ndn::Name>& providers,
         const ndn::Name& publishedService,
         const RequestMessage& request, size_t strategy) {
      BOOST_REQUIRE(providers.empty());
      BOOST_CHECK_EQUAL(publishedService, serviceName);
      BOOST_CHECK_EQUAL(strategy, tlv::AllSelected);
      result.requestPublicationName = requestName.toUri();
      const auto requestBlock = request.WireEncode();
      const auto encrypted = makeTestHybridPublication(
        requestName, serviceName, requestId, requesterName, "REQUEST",
        ndn::Buffer(requestBlock.data(), requestBlock.size()));
      environment.provider().cacheHybridReceiveKeyForTest(
        encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
      environment.userPubSub().publish(
        requestName,
        ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
    });
  environment.user().setLocalPublicationHandler(
    [&environment] (const ndn::Name& messageName, const ndn::Buffer& wire) {
      if (parseServiceSelectionNameV2(messageName)) {
        environment.userPubSub().publish(
          messageName, ndn::span<const uint8_t>(wire.data(), wire.size()));
      }
    });

  const auto requestPayload = makeSpec175TinyRequestPayload();
  StreamRequestOptions streamOptions;
  streamOptions.maxEvents = 9;
  streamOptions.interestWindow = 4;
  streamOptions.reorderCapacity = 4;
  streamOptions.publisherQueueCapacity = 4;
  streamOptions.retentionMs = 3000;
  streamOptions.maxEventWireBytes = 4096;
  {
    const auto returnedRequestId = environment.user().BeginCollaboration(
      serviceName, requestPayload, 1000, 8000,
      [&] (const CollaborationAckClosure& closure) {
        result.ackCandidates = closure.candidates.size();
        if (!closure.candidates.empty()) {
          result.ackProvider = closure.candidates.front().providerName.toUri();
          result.ackService = closure.candidates.front().serviceName.toUri();
        }
        result.ackClosed = closure.candidates.size() == 1;
        CollaborationPlan collaborationPlan;
        collaborationPlan.ackCollectionTimeMs = 1000;
        collaborationPlan.timeoutMs = 8000;
        CollaborationRoleSpec roleSpec;
        roleSpec.role = role;
        roleSpec.service = serviceName;
        roleSpec.requiredArtifact = ndn::Name("/artifact").append(role);
        roleSpec.terminalResponseOwner = true;
        collaborationPlan.roles.push_back(std::move(roleSpec));
        collaborationPlan.participantSelector =
          std::make_shared<Spec175AckCapabilitySelection>(
            selectionAssignmentByProvider);
        try {
          result.planCommitted = environment.user().CommitCollaborationPlan(
            closure.requestId, closure.digest, std::move(collaborationPlan));
        }
        catch (const std::exception& error) {
          result.failed = true;
          result.error = error.what();
        }
      },
      [&] (const ResponseMessage&) {},
      [&] (const ndn::Name&) { result.timedOut = true; },
      requestId,
      CollaborationAckCoverageHandler(),
      RequestCapabilities(),
      std::optional<StreamRequestOptions>(streamOptions),
      [&] (const ndn::Buffer& event) {
        result.events.emplace_back(
          reinterpret_cast<const char*>(event.data()), event.size());
      },
      [&] (const ndn::Buffer& response) {
        result.finalPayload.assign(
          reinterpret_cast<const char*>(response.data()), response.size());
        result.completed = true;
      },
      [&] (const StreamedInvocationError& error) {
        result.failed = true;
        result.error = error.message;
      });
    BOOST_REQUIRE_EQUAL(returnedRequestId, requestId);
  }

  const auto terminal = [&] {
    return result.completed || result.failed || result.timedOut;
  };
  for (int round = 0; round < 16 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }
  result.decodeStateSnapshots.push_back(decodeStateSnapshotReader());
  {
    std::lock_guard<std::mutex> lock(*cacheObservationMutex);
    result.cacheObservations = *cacheObservations;
  }
  const auto fullPrefix = runSpec175TinyFullPrefixControl(runnerSpec);
  result.fullPrefixControlTokens = fullPrefix.tokens;
  result.fullPrefixControlInputExtents = fullPrefix.inputExtents;
  return result;
#endif
}

Spec175TinyFullPrefixControlResult
runSpec175TinyFullPrefixControl(const NativeModelRunnerSpec& spec)
{
#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  throw std::runtime_error("Spec175 full-prefix control requires ONNX Runtime");
#else
  RegistryNativeModelRunnerFactory factory;
  registerOnnxRuntimeBackend(factory);
  auto runner = factory.create(spec);

  Spec175TinyFullPrefixControlResult result;
  std::vector<std::int64_t> prefix{3};
  for (std::size_t epoch = 0; epoch < 8; ++epoch) {
    RoleExecutionContext context;
    context.sessionId = "spec175-tiny-full-prefix-control";
    context.role = spec.role;
    // This reference deliberately presents no predecessor state. The
    // production ONNX adapter therefore materializes the certified zero state
    // and recomputes the complete logical prefix on every call.
    context.inferenceEpoch = 0;
    std::vector<std::uint8_t> prefixBytes(
      prefix.size() * sizeof(std::int64_t));
    std::memcpy(prefixBytes.data(), prefix.data(), prefixBytes.size());
    context.inputsByScope.emplace(
      "input_ids",
      makeEncodedTensorBundle("input_ids", {NamedTensor{
      "input_ids", TensorElementType::Int64,
      {1, static_cast<std::int64_t>(prefix.size())},
      std::move(prefixBytes)}}));
    for (const auto* stateName : {
           "attention_kv_in", "recurrent_state_in", "convolution_state_in"}) {
      context.inputsByScope.emplace(
        stateName,
        makeEncodedTensorBundle(stateName, {NamedTensor{
          stateName, TensorElementType::Float32, {4, 8},
          std::vector<std::uint8_t>(4 * 8 * sizeof(float), 0)}}));
    }

    const auto outputs = runner->run(context);
    const auto encoded = outputs.find("onnx-output-bundle");
    if (encoded == outputs.end() ||
        !isEncodedTensorBundle(encoded->second.payload)) {
      throw std::runtime_error("Spec175 full-prefix control has no ONNX output");
    }
    const auto outputTensors = decodeTensorBundle(encoded->second.payload);
    const auto& logits = findTensor(outputTensors, "logits");
    if (logits.elementType != TensorElementType::Float32 ||
        logits.shape.empty() || logits.shape.back() <= 0 ||
        logits.payload.size() % sizeof(float) != 0) {
      throw std::runtime_error("Spec175 full-prefix control logits are invalid");
    }
    std::vector<float> values(logits.payload.size() / sizeof(float));
    std::memcpy(values.data(), logits.payload.data(), logits.payload.size());
    const auto vocabulary = static_cast<std::size_t>(logits.shape.back());
    if (values.size() < vocabulary || values.size() % vocabulary != 0) {
      throw std::runtime_error(
        "Spec175 full-prefix control logits vocabulary is invalid");
    }
    const auto begin = values.end() - static_cast<std::ptrdiff_t>(vocabulary);
    const auto token = static_cast<std::int64_t>(
      std::distance(begin, std::max_element(begin, values.end())));
    result.inputExtents.push_back(prefix.size());
    result.tokens.push_back(token);
    prefix.push_back(token);
    if (token == 2) {
      break;
    }
  }
  return result;
#endif
}

Spec175NativeTinyStreamResult
runSpec175NativeTinyMultiProviderCase(std::size_t providerCount,
                                      Spec175NativeTinyCaseOptions caseOptions)
{
  Spec175NativeTinyStreamResult result;
  const bool traceEnabled = std::getenv("SPEC175_TRACE") != nullptr;
  const auto trace = [&] (const std::string& message) {
    if (traceEnabled) {
      std::cerr << "SPEC175_TRACE " << caseOptions.caseId << ' '
                << message << std::endl;
    }
  };
#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  result.failed = true;
  result.error = "NDNSF_DI_ENABLE_ONNXRUNTIME_CPP is disabled";
  return result;
#else
  if (providerCount < 2 || providerCount > 4) {
    result.failed = true;
    result.error = "Spec175 native tiny case requires 2-4 Providers";
    return result;
  }
  const auto fixture = findSpec175TinyRoleFixture(providerCount);
  if (fixture.empty()) {
    result.failed = true;
    result.error = "Spec175 tiny role ONNX fixture is unavailable";
    return result;
  }
  const auto defaultTokenizerPath = findSpec175TinyStandaloneTokenizer();
  const auto tokenizerPath = caseOptions.tokenizerPath.empty()
    ? defaultTokenizerPath : caseOptions.tokenizerPath;
  BOOST_REQUIRE(!tokenizerPath.empty());
  ScopedSpec175CertifiedModels certifiedModels(caseOptions.caseId);

  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Formal/NativeTiny/").append(
    caseOptions.caseId);
  const auto totalProviderCount = providerCount +
    (caseOptions.extraUnselectedProvider ? 1U : 0U);
  profile.providerCount = totalProviderCount;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  trace("bootstrap-ready");

  auto suppressProviderPeerSync = std::make_shared<std::atomic<bool>>(false);
  const auto providerSyncPrefix = environment.profile().syncPrefix;
  std::vector<ndn::signal::ScopedConnection> providerPeerBridges;
  for (std::size_t source = 0; source < environment.providerCount(); ++source) {
    for (std::size_t destination = 0; destination < environment.providerCount(); ++destination) {
      if (source == destination) {
        continue;
      }
      providerPeerBridges.emplace_back(
        environment.providerFace(source).onSendInterest.connect(
          [&environment, destination, suppressProviderPeerSync,
           providerSyncPrefix] (const ndn::Interest& interest) {
            if (suppressProviderPeerSync->load(std::memory_order_acquire) &&
                providerSyncPrefix.isPrefixOf(interest.getName())) {
              return;
            }
            environment.providerFace(destination).receive(interest);
          }));
      providerPeerBridges.emplace_back(
        environment.providerFace(source).onSendData.connect(
          [&environment, destination, suppressProviderPeerSync,
           providerSyncPrefix] (const ndn::Data& data) {
            if (suppressProviderPeerSync->load(std::memory_order_acquire) &&
                providerSyncPrefix.isPrefixOf(data.getName())) {
              return;
            }
            environment.providerFace(destination).receive(data);
          }));
    }
  }

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  std::vector<std::string> roles;
  roles.reserve(providerCount);
  for (std::size_t index = 0; index < providerCount; ++index) {
    roles.push_back("/LLM/Pipeline/Stage/" + std::to_string(index));
  }
  const auto& role0 = roles.front();
  const auto& finalRole = roles.back();
  std::vector<std::size_t> roleProviderIndex(providerCount);
  for (std::size_t index = 0; index < providerCount; ++index) {
    roleProviderIndex[index] = caseOptions.permuteRoleProviders
      ? (index + 1) % providerCount : index;
  }
  NativeExecutionPlan plan;
  plan.serviceName = serviceName.toUri();
  plan.modelName = "spec175-tiny-causal-lm-v1";
  plan.modelFamily = "spec175-tiny-causal-lm";
  plan.modelFormat = "onnx";
  plan.plannerKind = "PreSplitFirstStrategy";
  plan.executionPolicy = "DATA_DRIVEN_V2";
  plan.streamingOperationStride = providerCount;
  plan.roles = roles;
  const auto objectTemplate =
    "{producerProvider}/NDNSF-DI/{sessionId}/{producerRole}/{role}/{sequence}";
  for (std::size_t index = 0; index + 1 < providerCount; ++index) {
    NativeDependencySpec activation(
      {roles[index]}, {roles[index + 1]}, "activation-" + std::to_string(index),
      "/Spec175/activation/" + std::to_string(index), objectTemplate,
      1, 0, {"hidden_in"});
    activation.operationKind = "ACTIVATION";
    activation.useNdnsfDataV1 = true;
    activation.collectiveOperationIndex = index;
    activation.collectiveProducerRank = std::to_string(index);
    activation.collectiveSourceLayoutDigest = "sha256:" +
      std::string(64, static_cast<char>('a' + index));
    activation.collectiveTargetLayoutDigest = activation.collectiveSourceLayoutDigest;
    activation.collectiveTensorDigest = "sha256:" +
      std::string(64, static_cast<char>('b' + index));
    plan.dependencies.push_back(std::move(activation));
  }
  NativeDependencySpec feedback(
    {finalRole}, {role0}, "token-feedback", "/Spec175/feedback",
    objectTemplate, 1, 0, {"input_ids"});
  feedback.operationKind = "TOKEN_FEEDBACK";
  feedback.useNdnsfDataV1 = true;
  feedback.collectiveOperationIndex = providerCount - 1;
  feedback.collectiveProducerRank = std::to_string(providerCount - 1);
  feedback.collectiveSourceLayoutDigest = "sha256:" + std::string(64, 'c');
  feedback.collectiveTargetLayoutDigest = feedback.collectiveSourceLayoutDigest;
  feedback.collectiveTensorDigest = "sha256:" + std::string(64, 'd');
  plan.dependencies.push_back(std::move(feedback));

  NativeProviderAssignment assignment;
  for (std::size_t index = 0; index < providerCount; ++index) {
    assignment.providerByRole.emplace(
      roles[index], environment.provider(roleProviderIndex[index]).getName().toUri());
  }
  result.permutedRoleProviderMap = caseOptions.permuteRoleProviders && std::all_of(
    roles.begin(), roles.end(), [&] (const auto& role) {
      const auto roleIndex = static_cast<std::size_t>(
        std::stoul(role.substr(role.find_last_of('/') + 1)));
      return assignment.providerByRole.at(role) !=
        environment.provider(roleIndex).getName().toUri();
    });
  const auto planDigest = std::string("sha256:") + std::string(64, '1');
  const auto recipeDigest = std::string("sha256:") + std::string(64, '4');
  const auto requestId = ndn::Name("/spec175-native-tiny-" + caseOptions.caseId);
  BOOST_REQUIRE_EQUAL(requestId.size(), 1U);
  const auto recoveryRequestId = ndn::Name(
    "/spec175-native-tiny-" + caseOptions.caseId + "-recovery");
  BOOST_REQUIRE_EQUAL(recoveryRequestId.size(), 1U);
  std::optional<test::RequestScope> requestScope;
  auto retentionExpirations = std::make_shared<std::atomic_size_t>(0);
  test::FaultProfile transportFaults;
  switch (caseOptions.fault) {
    case Spec175NativeTinyFault::ReorderEvent3After4:
      break;
    case Spec175NativeTinyFault::DuplicateEvent4:
      break;
    case Spec175NativeTinyFault::DropFirstEvent5Data:
      transportFaults.dropStreamDataCursor = 5;
      transportFaults.dropStreamDataCount = 1;
      break;
    case Spec175NativeTinyFault::RetentionExpiredEvent5:
      transportFaults.dropStreamInterestPredicate =
        [retentionExpirations] (const ndn::Interest& interest) {
          const auto parsed = parseInvocationEventName(interest.getName());
          return parsed && parsed->cursor == 5 &&
            retentionExpirations->load(std::memory_order_relaxed) == 0;
        };
      break;
    case Spec175NativeTinyFault::EndBeforeGapEvent5:
      transportFaults.dropStreamDataCursor = 5;
      transportFaults.dropStreamDataCount =
        std::numeric_limits<std::size_t>::max();
      break;
    case Spec175NativeTinyFault::ProviderUnavailableAfterEvent3:
      transportFaults.dropStreamDataCursor = 4;
      transportFaults.dropStreamDataCount =
        std::numeric_limits<std::size_t>::max();
      break;
    case Spec175NativeTinyFault::ProviderUnavailableAfterEvent3WithReplacement:
      break;
    case Spec175NativeTinyFault::CancelAfterThirdEvent:
    case Spec175NativeTinyFault::ExpiredDeadline:
    case Spec175NativeTinyFault::CallbackThrowsAtThirdEvent:
    case Spec175NativeTinyFault::SlowConsumerCapacityOne:
    case Spec175NativeTinyFault::None:
    case Spec175NativeTinyFault::NeverRetainedEvent5:
      break;
    case Spec175NativeTinyFault::TamperEventOneSignature:
      transportFaults.tamperStreamDataCursor = 1;
      transportFaults.tamperStreamDataCount = 1;
      break;
  }
  const auto samplingDigest = std::string("sha256:") +
    "1750001000000000000000000000000000000000000000000000000000000000";
  const auto defaultTokenizerDigest = std::string(
    "sha256:bf0f0fa65dc5aafe690ee497b4c8e2abe408fb4788c5ef035964dc94f15ca5a6");
  const auto tokenizerDigest = caseOptions.tokenizerDigest.empty()
    ? defaultTokenizerDigest : caseOptions.tokenizerDigest;

  std::vector<GroupOperationV1> operations;
  // The multi-Provider tiny fixture uses a four-token causal run.  The
  // replacement proof commits three tokens and requires the spare to publish
  // the fourth token and End; the one-Provider fixture remains eight-token.
  const std::size_t maxGenerationEpochs =
    (caseOptions.caseId == "i02" || caseOptions.allowReplacement) ? 4 : 8;
  operations.reserve((maxGenerationEpochs + 1) * providerCount);
  for (std::uint64_t index = 0;
       index < (maxGenerationEpochs + 1) * providerCount; ++index) {
    const auto edgeOrdinal = static_cast<std::size_t>(index % providerCount);
    GroupOperationV1 operation;
    operation.operationIndex = index;
    operation.kind = edgeOrdinal + 1 == providerCount
      ? "TOKEN_FEEDBACK" : "ACTIVATION";
    const auto producerRank = edgeOrdinal + 1 == providerCount
      ? providerCount - 1 : edgeOrdinal;
    const auto consumerRank = edgeOrdinal + 1 == providerCount
      ? 0 : edgeOrdinal + 1;
    operation.producerRanks = {std::to_string(producerRank)};
    operation.consumerRanks = {std::to_string(consumerRank)};
    operation.tensorLayoutDigest = "sha256:" +
      std::string(64, edgeOrdinal + 1 == providerCount
        ? 'c' : static_cast<char>('a' + edgeOrdinal));
    operation.maxBytes = 4096;
    operation.maxSegments = 1;
    operations.push_back(std::move(operation));
  }
  ProviderGroupCoordinator capabilitySealer(makeD2bCoordinatorOptions());
  std::vector<GroupMemberV1> members;
  members.reserve(providerCount);
  for (std::size_t roleIndex = 0; roleIndex < providerCount; ++roleIndex) {
    const auto providerIndex = roleProviderIndex[roleIndex];
    auto providerPrefix = environment.profile().providerNode;
    if (providerIndex != 0) {
      providerPrefix.append("p" + std::to_string(providerIndex));
    }
    providerPrefix.append("0");
    members.push_back(GroupMemberV1{
      environment.provider(providerIndex).getName().toUri(),
      roleIndex,
      "offer-p" + std::to_string(providerIndex),
      providerPrefix.toUri()});
  }
  const auto capability = capabilitySealer.createCapability(
    requestId.toUri(), "attempt-1", planDigest,
    "group-spec175-" + caseOptions.caseId, 1, members,
    operations, 4096, 2000, 8000);
  std::optional<GroupCapabilityV1> replacementCapability;
  if (caseOptions.allowReplacement) {
    ProviderGroupCoordinator replacementCapabilitySealer(makeD2bCoordinatorOptions());
    auto replacementMembers = members;
    const auto finalRoleIndex = providerCount - 1;
    auto replacementPrefix = environment.profile().providerNode;
    replacementPrefix.append("p" + std::to_string(providerCount));
    replacementPrefix.append("0");
    replacementMembers[finalRoleIndex] = GroupMemberV1{
      environment.provider(providerCount).getName().toUri(),
      finalRoleIndex,
      "offer-p" + std::to_string(providerCount),
      replacementPrefix.toUri()};
    replacementCapability = replacementCapabilitySealer.createCapability(
      recoveryRequestId.toUri(), "attempt-2", planDigest,
      "group-spec175-" + caseOptions.caseId + "-recovery", 2,
      std::move(replacementMembers), operations, 4096, 2000, 8000);
  }

  auto runnerFactory = std::make_shared<RegistryNativeModelRunnerFactory>();
  registerOnnxRuntimeBackend(*runnerFactory);
  const auto makeRunnerSpec = [&] (const std::string& role,
                                   const std::filesystem::path& path,
                                   const std::string& providerName) {
    const auto roleIndex = static_cast<std::size_t>(
      std::stoul(role.substr(role.find_last_of('/') + 1)));
    const auto artifactDigest =
      "sha256:" + std::string(64, static_cast<char>('2' + roleIndex));
    NativeModelRunnerSpec spec;
    spec.role = role;
    spec.kind = "spec175-tiny-stateful-onnx";
    spec.backend = "onnxruntime";
    spec.path = path.string();
    spec.metadata["executionProvider"] = "cpu";
    spec.metadata["statefulModel"] = "true";
    spec.metadata["inputNames"] = role == role0
      ? "input_ids,attention_kv_in,recurrent_state_in,convolution_state_in"
      : "hidden_in,attention_kv_in,recurrent_state_in,convolution_state_in";
    spec.metadata["outputNames"] = role == finalRole
      ? "logits,attention_kv_out,recurrent_state_out,convolution_state_out"
      : "hidden_out,attention_kv_out,recurrent_state_out,convolution_state_out";
    spec.metadata["stateInputNames"] =
      "attention_kv_in,recurrent_state_in,convolution_state_in";
    spec.metadata["stateOutputNames"] =
      "attention_kv_out,recurrent_state_out,convolution_state_out";
    spec.metadata["inputScope.input_ids"] = "request-input";
    if (role != role0) {
      const auto roleIndex = static_cast<std::size_t>(
        std::stoul(role.substr(role.find_last_of('/') + 1)));
      spec.metadata["inputScope.hidden_in"] =
        "activation-" + std::to_string(roleIndex - 1);
    }
    spec.metadata["outputAlias.hidden_out"] = "hidden_in";
    spec.metadata["fragmentDigest"] = artifactDigest;
    spec.metadata["recipeDigest"] = recipeDigest;
    spec.metadata["evidence.providerName"] = providerName;
    spec.metadata["evidence.providerBootId"] = providerName + "-boot";
    spec.metadata["evidence.modelDigest"] = planDigest;
    spec.metadata["evidence.planDigest"] = planDigest;
    spec.metadata["evidence.artifactDigest"] = spec.metadata["fragmentDigest"];
    spec.metadata["evidence.createdAtMs"] = "1";
    bindSpec175CertifiedRunnerMetadata(
      spec, roleIndex, providerCount, artifactDigest, recipeDigest);
    return spec;
  };

  std::vector<NativeModelRunnerSpec> specs;
  std::map<std::string, ndn::Buffer> selectionAssignmentByProvider;
  std::map<std::string, ndn::Buffer> recoverySelectionAssignmentByProvider;
  auto providerCoordinatorCompletions =
    std::make_shared<std::atomic_size_t>(0);
  auto providerFailures = std::make_shared<std::atomic_size_t>(0);
  auto retentionSuppressions = std::make_shared<std::atomic_size_t>(0);
  auto publicationSuppressions = std::make_shared<std::atomic_size_t>(0);
  auto publicationReorders = std::make_shared<std::atomic_size_t>(0);
  auto publicationDuplicates = std::make_shared<std::atomic_size_t>(0);
  auto unselectedProviderExecutions = std::make_shared<std::atomic_size_t>(0);
  auto replacementProviderExecutions = std::make_shared<std::atomic_size_t>(0);
  auto replacementEventsPublished = std::make_shared<std::atomic_size_t>(0);
  auto providerTransportDetachments = std::make_shared<std::atomic_size_t>(0);
  auto streamedContextByRole = std::make_shared<std::vector<bool>>(
    providerCount, false);
  auto streamedContextMutex = std::make_shared<std::mutex>();
  auto callbacksInFlight = std::make_shared<std::atomic_size_t>(0);
  auto callbackConcurrencyHighWater = std::make_shared<std::atomic_size_t>(0);
  auto providerFailureMutex = std::make_shared<std::mutex>();
  auto providerFailureMessage = std::make_shared<std::string>();
  std::vector<std::function<ProviderDecodeStateSnapshot()>>
    decodeStateSnapshotReaders;
  specs.reserve(providerCount);
  for (std::size_t roleIndex = 0; roleIndex < providerCount; ++roleIndex) {
    const auto providerIndex = roleProviderIndex[roleIndex];
    const auto certifiedModel = certifiedModels.materialize(
      fixture / ("role-" + std::to_string(roleIndex) + ".onnx"), roleIndex);
    specs.push_back(makeRunnerSpec(
      roles[roleIndex], certifiedModel,
      environment.provider(providerIndex).getName().toUri()));
  }
  const auto makeProjectionPayload = [&] (
      std::size_t roleIndex,
      std::size_t providerIndex,
      const ndn::Name& projectionRequestId,
      std::uint64_t attempt,
      const std::string& projectedCapabilityHex,
      const std::vector<std::int64_t>& committedPrefix = {}) {
    std::string dependenciesJson = "[";
    std::vector<std::string> mayPublish;
    std::vector<std::string> mustFetch;
    for (std::size_t edgeIndex = 0; edgeIndex < plan.dependencies.size(); ++edgeIndex) {
      const auto& dependency = plan.dependencies[edgeIndex];
      if (edgeIndex != 0) dependenciesJson += ',';
      dependenciesJson += std::string("{\"consumers\":[\"") +
        dependency.consumers.front() + "\"],\"expected_segments\":0," +
        "\"key_scope\":\"" + dependency.keyScope +
        "\",\"object_name_template\":\"" + dependency.objectNameTemplate +
        "\",\"operationKind\":\"" + dependency.operationKind +
        "\",\"producers\":[\"" + dependency.producers.front() +
        "\"],\"required\":true,\"tensors\":[\"" +
        dependency.tensors.front() + "\"],\"topic_prefix\":\"" +
        dependency.topicPrefix +
        "\",\"transportProfile\":\"NDNSF_DATA_V1\"," +
        "\"collectiveOperationIndex\":" +
        std::to_string(dependency.collectiveOperationIndex) +
        ",\"collectiveProducerRank\":\"" +
        dependency.collectiveProducerRank +
        "\",\"collectiveSourceLayoutDigest\":\"" +
        dependency.collectiveSourceLayoutDigest +
        "\",\"collectiveTargetLayoutDigest\":\"" +
        dependency.collectiveTargetLayoutDigest +
        "\",\"collectiveTensorDigest\":\"" +
        dependency.collectiveTensorDigest + "\"}";
      const auto producerRoleIndex = dependency.operationKind == "TOKEN_FEEDBACK"
        ? providerCount - 1 : edgeIndex;
      const auto endpoint = makeV3TensorEndpointJson(
        members[producerRoleIndex].endpointPrefix,
        requesterName.toUri(), projectionRequestId.toUri(), planDigest,
        dependency.keyScope, dependency.collectiveOperationIndex,
        dependency.producers.front(), producerRoleIndex,
        dependency.consumers.front(),
        "\"" + dependency.consumers.front() + "\"",
        dependency.tensors.front(), dependency.collectiveTensorDigest,
        dependency.collectiveSourceLayoutDigest,
        dependency.collectiveTargetLayoutDigest, dependency.operationKind,
        spec175Digest(static_cast<char>('1' + edgeIndex)),
        dependency.collectiveTensorDigest, attempt);
      if (dependency.producers.front() == roles[roleIndex]) {
        mayPublish.push_back(endpoint);
      }
      if (dependency.consumers.front() == roles[roleIndex]) {
        mustFetch.push_back(endpoint);
      }
    }
    dependenciesJson += ']';
    const auto join = [] (const std::vector<std::string>& values) {
      std::string result;
      for (const auto& value : values) {
        if (!result.empty()) result += ',';
        result += value;
      }
      return result;
    };
    const auto& role = roles[roleIndex];
    const auto dataflowJson = std::string("{\"attempt\":") +
      std::to_string(attempt) + ",\"dataflow_digest\":\"" + planDigest +
      "\",\"may_publish\":[" + join(mayPublish) +
      "],\"must_fetch\":[" + join(mustFetch) +
      "],\"plan_digest\":\"" + planDigest +
      "\",\"request_id\":\"" + projectionRequestId.toUri() +
      "\",\"role\":\"" + role +
      "\",\"terminal_response_owner\":" +
      (role == finalRole ? "true" : "false") + ",\"wait_for\":[]}";
    const auto artifactDigest = specs[roleIndex].metadata.at("fragmentDigest");
    const auto roleJson = makeSpec175CertifiedRoleJson(
      role, roleIndex, providerCount, artifactDigest, recipeDigest);
    const auto stateRows = 4 / providerCount;
    const auto projection = makeV3SelectionProjectionJson(
      roleJson, role, role, roleIndex,
      environment.provider(providerIndex).getName().toUri(),
      projectionRequestId.toUri(), planDigest, projectedCapabilityHex,
      dependenciesJson, roleIndex == 0
        ? withApplicationInput(dataflowJson, requesterName.toUri(),
                               projectionRequestId.toUri(), planDigest, role, attempt)
        : dataflowJson, artifactDigest, "cpu:0",
      makeSpec175GenerationContractJson(
        maxGenerationEpochs, providerCount, samplingDigest,
        tokenizerDigest, committedPrefix, caseOptions.samplingMode,
        caseOptions.samplingTemperature, caseOptions.samplingTopK,
        caseOptions.samplingTopP, caseOptions.samplingRepetitionPenalty,
        caseOptions.samplingSeed, caseOptions.stopStrings),
      roleIndex * stateRows, (roleIndex + 1) * stateRows, attempt);
    return ndn::Buffer(
      reinterpret_cast<const std::uint8_t*>(projection.data()),
      projection.size());
  };
  for (std::size_t index = 0; index < providerCount; ++index) {
    const auto roleIt = std::find(
      roleProviderIndex.begin(), roleProviderIndex.end(), index);
    if (roleIt == roleProviderIndex.end()) {
      throw std::runtime_error("I02 role/provider assignment is incomplete");
    }
    const auto roleIndex = static_cast<std::size_t>(
      std::distance(roleProviderIndex.begin(), roleIt));
    const auto& role = roles[roleIndex];
    const auto providerName = environment.provider(index).getName().toUri();
    const auto providerCapabilityHex = bytesToHex(
      ProviderGroupCoordinator::encodeCapability(
        capability.projectForProvider(providerName)));
    NativeProviderHandlerConfig handlerConfig;
    handlerConfig.plan = plan;
    handlerConfig.assignment = assignment;
    handlerConfig.runnerFactory = runnerFactory;
    handlerConfig.runnerSpecs = {specs[roleIndex]};
    handlerConfig.finalResponseScope = "final-response";
    handlerConfig.localProviderName = environment.provider(index).getName().toUri();
    handlerConfig.providerBootId = environment.provider(index).getName().toUri() + "-boot";
    handlerConfig.planDigest = planDigest;
    handlerConfig.fetchTimeoutMs = 6000;
    handlerConfig.maxSegmentSize = 4096;
    handlerConfig.freshnessMs = 60000;
    handlerConfig.enableNativeEpochCoordinator = true;
    handlerConfig.maxGenerationEpochs = maxGenerationEpochs;
    handlerConfig.generationStateInputNames = {
      "attention_kv_in", "recurrent_state_in", "convolution_state_in"};
    handlerConfig.generationStateOutputNames = {
      "attention_kv_out", "recurrent_state_out", "convolution_state_out"};
    handlerConfig.generationEosTokenIds = {2};
    handlerConfig.generationSamplingDigest = samplingDigest;
    handlerConfig.generationSamplingMode = caseOptions.samplingMode;
    handlerConfig.generationSamplingTemperature = caseOptions.samplingTemperature;
    handlerConfig.generationSamplingTopK = caseOptions.samplingTopK;
    handlerConfig.generationSamplingTopP = caseOptions.samplingTopP;
    handlerConfig.generationSamplingRepetitionPenalty =
      caseOptions.samplingRepetitionPenalty;
    handlerConfig.generationSamplingSeed = caseOptions.samplingSeed;
    handlerConfig.generationStopStrings = caseOptions.stopStrings;
    handlerConfig.generationDecodersFactory =
      [tokenizerPath] (const std::string& digest) {
        NativeStandaloneTokenizerOptions options;
        options.tokenizerPath = tokenizerPath.string();
        return makeNativeStandaloneTokenizerDecoders(std::move(options), digest);
      };
    handlerConfig.requireGenerationTextOutput = true;
    handlerConfig.allowPreassembledV3Compatibility = true;
    handlerConfig.epochCoordinatorCompletionObserver =
      std::make_shared<NativeProviderHandlerConfig::EpochCoordinatorCompletionObserver>(
        [providerCoordinatorCompletions, trace] (
            const std::string& role, const NativeEpochCoordinatorResult&) {
          providerCoordinatorCompletions->fetch_add(1, std::memory_order_relaxed);
          trace("initial-coordinator-complete role=" + role);
        });
    handlerConfig.nativeFailureObserver =
      std::make_shared<NativeProviderHandlerConfig::NativeFailureObserver>(
        [providerFailures, providerFailureMutex, providerFailureMessage, trace] (
            const std::string& role, const std::string& reason) {
          providerFailures->fetch_add(1, std::memory_order_relaxed);
          trace("initial-provider-failure role=" + role + " reason=" + reason);
          std::lock_guard<std::mutex> lock(*providerFailureMutex);
          if (providerFailureMessage->empty()) {
            *providerFailureMessage = role + ": " + reason;
          }
        });
    handlerConfig.groupCoordinatorFactory =
      [&, localProvider = environment.provider(index).getName().toUri()] (
          ServiceProvider::CollaborationContext& context,
          const std::map<std::string, std::string>& fields) {
        const auto field = fields.find("groupCapabilityV1");
        if (field == fields.end()) {
          throw std::runtime_error(
            "Spec175 assignment has no GroupCapabilityV1");
        }
        auto decoded = ProviderGroupCoordinator::decodeCapability(
          bytesFromHex(field->second));
        if (decoded.requestId != context.sessionId() ||
            decoded.planDigest != planDigest) {
          throw std::runtime_error(
            "Spec175 GroupCapabilityV1 binding mismatch");
        }
        auto options = makeD2bCoordinatorOptions();
        options.localProvider = localProvider;
        auto coordinator = std::make_shared<ProviderGroupCoordinator>(
          std::move(options));
        coordinator->installCapability(std::move(decoded), {}, true);
        return coordinator;
      };
    auto nativeRuntime = makeNativeProviderCollaborationRuntime(
      std::move(handlerConfig));
    decodeStateSnapshotReaders.push_back(nativeRuntime.decodeStateSnapshot);
    auto nativeHandler = std::move(nativeRuntime.handler);
    const auto ackCapabilityOffer = std::string("role=") + role +
      ";provider=" + environment.provider(index).getName().toUri() +
      ";backend=onnxruntime;device=cpu:0;artifactDigest=" +
      specs[roleIndex].metadata.at("fragmentDigest") +
      ";recipeDigest=" + recipeDigest + ";executionPolicy=DATA_DRIVEN_V2;";
    selectionAssignmentByProvider.emplace(
      providerName, makeProjectionPayload(
        roleIndex, index, requestId, 1, providerCapabilityHex));
    if (caseOptions.allowReplacement && index == roleProviderIndex.front()) {
      const auto recoveryProviderCapabilityHex = bytesToHex(
        ProviderGroupCoordinator::encodeCapability(
          replacementCapability->projectForProvider(providerName)));
      recoverySelectionAssignmentByProvider.emplace(
        providerName, makeProjectionPayload(
          roleIndex, index, recoveryRequestId, 2,
          recoveryProviderCapabilityHex, {4, 5, 6}));
    }
    environment.provider(index).addCollaborationHandler(
      serviceName,
      [ackCapabilityOffer] (const RequestMessage&) {
        ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.payload = ndn::Buffer(
          reinterpret_cast<const std::uint8_t*>(ackCapabilityOffer.data()),
          ackCapabilityOffer.size());
        return decision;
      },
      [nativeHandler = std::move(nativeHandler), roleIndex,
       streamedContextByRole, streamedContextMutex] (
          ServiceProvider::CollaborationContext& context,
          const RequestMessage& request) mutable {
        {
          std::lock_guard<std::mutex> lock(*streamedContextMutex);
          (*streamedContextByRole)[roleIndex] = context.isStreamed();
        }
        nativeHandler(context, request);
      });
  }

  for (std::size_t index = providerCount; index < totalProviderCount; ++index) {
    const auto providerName = environment.provider(index).getName().toUri();
    if (caseOptions.allowReplacement && index == providerCount) {
      const auto roleIndex = providerCount - 1;
      const auto& role = roles[roleIndex];
      const auto providerCapabilityHex = bytesToHex(
        ProviderGroupCoordinator::encodeCapability(
          replacementCapability->projectForProvider(providerName)));
      NativeProviderHandlerConfig handlerConfig;
      handlerConfig.plan = plan;
      handlerConfig.assignment = assignment;
      handlerConfig.assignment.providerByRole[role] = providerName;
      handlerConfig.runnerFactory = runnerFactory;
      handlerConfig.runnerSpecs = {makeRunnerSpec(
        role, specs[roleIndex].path,
        providerName)};
      handlerConfig.finalResponseScope = "final-response";
      handlerConfig.localProviderName = providerName;
      handlerConfig.providerBootId = providerName + "-boot";
      handlerConfig.planDigest = planDigest;
      handlerConfig.fetchTimeoutMs = 6000;
      handlerConfig.maxSegmentSize = 4096;
      handlerConfig.freshnessMs = 60000;
      handlerConfig.enableNativeEpochCoordinator = true;
      handlerConfig.maxGenerationEpochs = maxGenerationEpochs;
      handlerConfig.generationStateInputNames = {
        "attention_kv_in", "recurrent_state_in", "convolution_state_in"};
      handlerConfig.generationStateOutputNames = {
        "attention_kv_out", "recurrent_state_out", "convolution_state_out"};
      handlerConfig.generationEosTokenIds = {2};
      handlerConfig.generationSamplingDigest = samplingDigest;
      handlerConfig.generationSamplingMode = caseOptions.samplingMode;
      handlerConfig.generationSamplingTemperature = caseOptions.samplingTemperature;
      handlerConfig.generationSamplingTopK = caseOptions.samplingTopK;
      handlerConfig.generationSamplingTopP = caseOptions.samplingTopP;
      handlerConfig.generationSamplingRepetitionPenalty =
        caseOptions.samplingRepetitionPenalty;
      handlerConfig.generationSamplingSeed = caseOptions.samplingSeed;
      handlerConfig.generationStopStrings = caseOptions.stopStrings;
      handlerConfig.generationDecodersFactory =
        [tokenizerPath] (const std::string& digest) {
          NativeStandaloneTokenizerOptions options;
          options.tokenizerPath = tokenizerPath.string();
          return makeNativeStandaloneTokenizerDecoders(std::move(options), digest);
        };
      handlerConfig.requireGenerationTextOutput = true;
      handlerConfig.generationCommittedPrefixTokenIds = {4, 5, 6};
      handlerConfig.allowPreassembledV3Compatibility = true;
      handlerConfig.epochCoordinatorCompletionObserver =
        std::make_shared<NativeProviderHandlerConfig::EpochCoordinatorCompletionObserver>(
          [providerCoordinatorCompletions, replacementEventsPublished, trace] (
              const std::string& role,
              const NativeEpochCoordinatorResult& coordinatorResult) {
            providerCoordinatorCompletions->fetch_add(1, std::memory_order_relaxed);
            replacementEventsPublished->store(
              coordinatorResult.eventsPublished, std::memory_order_relaxed);
            trace("replacement-coordinator-complete role=" + role +
                  " events=" + std::to_string(coordinatorResult.eventsPublished));
          });
      handlerConfig.nativeFailureObserver =
        std::make_shared<NativeProviderHandlerConfig::NativeFailureObserver>(
          [providerFailures, providerFailureMutex, providerFailureMessage, trace] (
              const std::string& failureRole, const std::string& reason) {
            providerFailures->fetch_add(1, std::memory_order_relaxed);
            trace("replacement-provider-failure role=" + failureRole +
                  " reason=" + reason);
            std::lock_guard<std::mutex> lock(*providerFailureMutex);
            if (providerFailureMessage->empty()) {
              *providerFailureMessage = failureRole + ": " + reason;
            }
          });
      handlerConfig.groupCoordinatorFactory =
        [&, localProvider = providerName] (
            ServiceProvider::CollaborationContext& context,
            const std::map<std::string, std::string>& fields) {
          const auto field = fields.find("groupCapabilityV1");
          if (field == fields.end()) {
            throw std::runtime_error(
              "Spec175 replacement assignment has no GroupCapabilityV1");
          }
          auto decoded = ProviderGroupCoordinator::decodeCapability(
            bytesFromHex(field->second));
          if (decoded.requestId != context.sessionId() ||
              decoded.planDigest != planDigest) {
            throw std::runtime_error(
              "Spec175 replacement GroupCapabilityV1 binding mismatch");
          }
          auto options = makeD2bCoordinatorOptions();
          options.localProvider = localProvider;
          auto coordinator = std::make_shared<ProviderGroupCoordinator>(
            std::move(options));
          coordinator->installCapability(std::move(decoded), {}, true);
          return coordinator;
        };
      auto nativeRuntime = makeNativeProviderCollaborationRuntime(
        std::move(handlerConfig));
      decodeStateSnapshotReaders.push_back(nativeRuntime.decodeStateSnapshot);
      auto nativeHandler = std::move(nativeRuntime.handler);
      const auto ackCapabilityOffer = std::string("role=") + role +
        ";provider=" + providerName +
        ";backend=onnxruntime;device=cpu:0;artifactDigest=" +
        specs[roleIndex].metadata.at("fragmentDigest") +
        ";recipeDigest=" + recipeDigest +
        ";executionPolicy=DATA_DRIVEN_V2;";
      recoverySelectionAssignmentByProvider.emplace(
        providerName, makeProjectionPayload(
          roleIndex, index, recoveryRequestId, 2,
          providerCapabilityHex, {4, 5, 6}));
      environment.provider(index).addCollaborationHandler(
        serviceName,
        [ackCapabilityOffer] (const RequestMessage&) {
          ServiceProvider::AckDecision decision;
          decision.status = true;
          decision.payload = ndn::Buffer(
            reinterpret_cast<const std::uint8_t*>(ackCapabilityOffer.data()),
            ackCapabilityOffer.size());
          return decision;
        },
        [nativeHandler = std::move(nativeHandler), roleIndex,
         streamedContextByRole, streamedContextMutex,
         replacementProviderExecutions] (
            ServiceProvider::CollaborationContext& context,
            const RequestMessage& request) mutable {
          {
            std::lock_guard<std::mutex> lock(*streamedContextMutex);
            (*streamedContextByRole)[roleIndex] = context.isStreamed();
          }
          replacementProviderExecutions->fetch_add(1, std::memory_order_relaxed);
          nativeHandler(context, request);
        });
      continue;
    }
    const auto unusedOffer = std::string("role=/LLM/Pipeline/Unused;provider=") +
      providerName + ";backend=onnxruntime;device=cpu:0;";
    environment.provider(index).addCollaborationHandler(
      serviceName,
      [unusedOffer] (const RequestMessage&) {
        ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.payload = ndn::Buffer(
          reinterpret_cast<const std::uint8_t*>(unusedOffer.data()),
          unusedOffer.size());
        return decision;
      },
      [unselectedProviderExecutions] (
          ServiceProvider::CollaborationContext&, const RequestMessage&) {
        unselectedProviderExecutions->fetch_add(1, std::memory_order_relaxed);
      });
  }

  auto& finalProvider = environment.provider(roleProviderIndex.back());
  const auto finalProviderIndex = roleProviderIndex.back();
  auto& finalProviderPubSub = environment.providerPubSub(roleProviderIndex.back());
  auto& finalProviderIo = environment.providerFace(roleProviderIndex.back()).getIoContext();
  auto heldPublication = std::make_shared<std::optional<ndn::Data>>();
  auto heldPublicationMutex = std::make_shared<std::mutex>();
  if (caseOptions.fault == Spec175NativeTinyFault::NeverRetainedEvent5) {
    finalProvider.setStreamRetentionInterceptorForTest(
      [retentionSuppressions] (const ndn::Data& data) {
        const auto parsed = parseInvocationEventName(data.getName());
        if (parsed && parsed->cursor == 5) {
          retentionSuppressions->fetch_add(1, std::memory_order_relaxed);
          return false;
        }
        return true;
      });
  }
  if (caseOptions.fault == Spec175NativeTinyFault::RetentionExpiredEvent5) {
    finalProvider.setStreamRetentionExpiryObserverForTest(
      [retentionExpirations] (const ndn::Name& name) {
        const auto parsed = parseInvocationEventName(name);
        if (parsed && parsed->cursor == 5) {
          retentionExpirations->fetch_add(1, std::memory_order_relaxed);
        }
      });
  }
  if (caseOptions.fault == Spec175NativeTinyFault::ReorderEvent3After4 ||
      caseOptions.fault == Spec175NativeTinyFault::DuplicateEvent4 ||
      caseOptions.fault == Spec175NativeTinyFault::DropFirstEvent5Data ||
      caseOptions.fault == Spec175NativeTinyFault::RetentionExpiredEvent5 ||
      caseOptions.fault == Spec175NativeTinyFault::EndBeforeGapEvent5 ||
      caseOptions.fault == Spec175NativeTinyFault::TamperEventOneSignature ||
      caseOptions.fault == Spec175NativeTinyFault::ProviderUnavailableAfterEvent3 ||
      caseOptions.fault == Spec175NativeTinyFault::ProviderUnavailableAfterEvent3WithReplacement) {
    finalProvider.setStreamPublicationInterceptorForTest(
      [fault = caseOptions.fault, publicationSuppressions,
       publicationReorders, publicationDuplicates,
       providerTransportDetachments, heldPublication,
       heldPublicationMutex, &finalProviderPubSub,
       &finalProviderIo] (const ndn::Data& data) {
        const auto parsed = parseInvocationEventName(data.getName());
        if (!parsed) {
          return true;
        }
        if (fault == Spec175NativeTinyFault::TamperEventOneSignature &&
            parsed->cursor == 1) {
          // Keep the authentic packet in the IMS, suppress its normal SVS
          // publication, and let the transport fault mutate the exact Data on
          // the Provider-to-User link. This models one on-path corruption
          // without racing an untampered duplicate into the callback.
          publicationSuppressions->fetch_add(1, std::memory_order_relaxed);
          return false;
        }
        if (fault == Spec175NativeTinyFault::ProviderUnavailableAfterEvent3 &&
            parsed->cursor >= 4) {
          publicationSuppressions->fetch_add(1, std::memory_order_relaxed);
          return false;
        }
        if (fault == Spec175NativeTinyFault::ProviderUnavailableAfterEvent3WithReplacement &&
            parsed->cursor >= 4) {
          publicationSuppressions->fetch_add(1, std::memory_order_relaxed);
          return false;
        }
        if (fault == Spec175NativeTinyFault::ReorderEvent3After4 &&
            parsed->cursor == 3) {
          std::lock_guard<std::mutex> lock(*heldPublicationMutex);
          *heldPublication = data;
          publicationSuppressions->fetch_add(1, std::memory_order_relaxed);
          return false;
        }
        if (fault == Spec175NativeTinyFault::ReorderEvent3After4 &&
            parsed->cursor == 4) {
          std::optional<ndn::Data> held;
          {
            std::lock_guard<std::mutex> lock(*heldPublicationMutex);
            held = std::move(*heldPublication);
            heldPublication->reset();
          }
          if (held) {
            boost::asio::post(finalProviderIo,
              [pubSub = &finalProviderPubSub, held = std::move(*held)] () mutable {
                pubSub->publishPacket(held);
              });
            publicationReorders->fetch_add(1, std::memory_order_relaxed);
          }
          return true;
        }
        if (fault == Spec175NativeTinyFault::DuplicateEvent4 &&
            parsed->cursor == 4) {
          boost::asio::post(finalProviderIo,
            [pubSub = &finalProviderPubSub, duplicate = ndn::Data(data)] () mutable {
              pubSub->publishPacket(duplicate);
            });
          publicationDuplicates->fetch_add(1, std::memory_order_relaxed);
          return true;
        }
        if ((fault == Spec175NativeTinyFault::DropFirstEvent5Data ||
             fault == Spec175NativeTinyFault::RetentionExpiredEvent5 ||
             fault == Spec175NativeTinyFault::EndBeforeGapEvent5) &&
            parsed->cursor == 5) {
          publicationSuppressions->fetch_add(1, std::memory_order_relaxed);
          return false;
        }
        return true;
      });
  }

  environment.enableProductionIngressForTest();
  for (std::size_t index = 0; index < environment.providerCount(); ++index) {
    environment.provider(index).markHybridResponseKeyWrappedForTest(serviceName);
    const auto ackKey = environment.provider(index).prepareHybridSendKeyForTest(
      serviceName, "ACK");
    const auto responseKey = environment.provider(index).prepareHybridSendKeyForTest(
      serviceName, "RESPONSE");
    environment.user().cacheHybridReceiveKeyForTest(
      ackKey.keyId, ackKey.epochId, ackKey.key);
    environment.user().cacheHybridReceiveKeyForTest(
      responseKey.keyId, responseKey.epochId, responseKey.key);
  }
  const auto selectionKey = environment.user().prepareHybridSendKeyForTest(
    serviceName, "SELECTION");
  for (std::size_t index = 0; index < environment.providerCount(); ++index) {
    environment.provider(index).cacheHybridReceiveKeyForTest(
      selectionKey.keyId, selectionKey.epochId, selectionKey.key);
  }

  environment.user().setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name& requestName,
         const std::vector<ndn::Name>&, const ndn::Name& publishedService,
         const RequestMessage& request, size_t) {
      BOOST_REQUIRE_EQUAL(publishedService, serviceName);
      result.requestPublicationName = requestName.toUri();
      ++result.requestPublicationCount;
      const auto publicationRequestId = parseRequestNameV2(requestName)
        ? parseRequestNameV2(requestName)->requestId : requestId;
      const auto requestBlock = request.WireEncode();
      const auto encrypted = makeTestHybridPublication(
        requestName, serviceName, publicationRequestId,
        requesterName, "REQUEST",
        ndn::Buffer(requestBlock.data(), requestBlock.size()));
      for (std::size_t index = 0; index < environment.providerCount(); ++index) {
        environment.provider(index).cacheHybridReceiveKeyForTest(
          encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
      }
      if (requestScope && publicationRequestId == requestId &&
          !requestScope->requestPublished) {
        environment.markRequestPublished(*requestScope);
      }
      environment.userPubSub().publish(
        requestName,
        ndn::span<const std::uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
    });
  environment.user().setLocalPublicationHandler(
    [&environment] (const ndn::Name& messageName, const ndn::Buffer& wire) {
      if (parseServiceSelectionNameV2(messageName)) {
        environment.userPubSub().publish(
          messageName, ndn::span<const std::uint8_t>(wire.data(), wire.size()));
      }
    });

  StreamRequestOptions streamOptions;
  streamOptions.maxEvents = 9;
  streamOptions.interestWindow = 4;
  streamOptions.reorderCapacity = 4;
  streamOptions.publisherQueueCapacity = 4;
  streamOptions.retentionMs = 3000;
  streamOptions.maxEventWireBytes = 4096;
  if (caseOptions.fault == Spec175NativeTinyFault::RetentionExpiredEvent5) {
    streamOptions.retentionMs = 1000;
    // Keep retrying across the retention boundary. The fixture drops exact
    // cursor-5 Interests only until the Provider confirms eviction.
    streamOptions.interestLifetimeMs = 250;
    streamOptions.maxEventRetries = 8;
  }
  if (caseOptions.fault == Spec175NativeTinyFault::SlowConsumerCapacityOne) {
    streamOptions.publisherQueueCapacity = 1;
    streamOptions.callbackQueueCapacity = 1;
    streamOptions.interestWindow = 1;
  }
  if (caseOptions.fault == Spec175NativeTinyFault::TamperEventOneSignature) {
    // Security-negative ordering is intentional: express only cursor 1 so the
    // tampered first publication is authenticated before later valid cursors
    // can trigger an exact-name repair that overtakes it from the Provider IMS.
    streamOptions.interestWindow = 1;
  }
  if (caseOptions.fault == Spec175NativeTinyFault::ProviderUnavailableAfterEvent3 ||
      caseOptions.fault == Spec175NativeTinyFault::ProviderUnavailableAfterEvent3WithReplacement) {
    // Permit one exact-name retry before declaring the Provider unavailable.
    // Both cases detach the live Provider transport only after the application
    // has observed event 3. Their publication/transport interceptors withhold
    // cursor 4 so the original Interest and its retry fail at that boundary.
    // The timeout must still exceed normal CPU-ONNX token service time: 100 ms
    // occasionally declared the Provider dead before the injected third-event
    // boundary, violating the recovery fixture's committed-prefix contract.
    streamOptions.maxEventRetries = 1;
    streamOptions.interestLifetimeMs = 1000;
  }
  {
    ndn::Buffer generation(sizeof(streamOptions.generationId));
    ndn::random::generateSecureBytes(
      ndn::span<uint8_t>(generation.data(), generation.size()));
    std::copy(generation.begin(), generation.end(), streamOptions.generationId.begin());
  }
  streamOptions.attemptEpoch = 1;
  streamOptions.streamEpoch = 1;
  streamOptions.allowReplacement = caseOptions.allowReplacement;
  streamOptions.maxReplacements = caseOptions.allowReplacement ? 1 : 0;
  if (caseOptions.fault == Spec175NativeTinyFault::ExpiredDeadline) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    streamOptions.deadlineEpochMs = static_cast<std::uint64_t>(now - 1);
  }
  const auto requestPayload = makeSpec175TinyRequestPayload();
  if (caseOptions.fault != Spec175NativeTinyFault::None) {
    requestScope = environment.beginRequest(requestId.toUri(), transportFaults);
  }
  bool replacementStarted = false;
  std::size_t recoveryEvents = 0;
  std::map<std::string, std::string> committedSelectionDigests;
  const auto commitPlan = [&] (const CollaborationAckClosure& closure) {
    trace("ack-closure request=" + closure.requestId.toUri() +
          " candidates=" + std::to_string(closure.candidates.size()));
    result.ackCandidates = closure.candidates.size();
    if (caseOptions.allowReplacement && closure.requestId == recoveryRequestId) {
      // The disconnected Provider must not be counted during recovery. The
      // closure is complete when every replacement-plan role is covered.
      result.recoveryAckClosed = closure.candidates.size() == providerCount;
    }
    else {
      result.ackClosed = closure.candidates.size() == totalProviderCount;
    }
    CollaborationPlan collaborationPlan;
    collaborationPlan.ackCollectionTimeMs = 1000;
    collaborationPlan.timeoutMs = 8000;
    for (std::size_t index = 0; index < providerCount; ++index) {
      CollaborationRoleSpec roleSpec;
      roleSpec.role = roles[index];
      roleSpec.service = serviceName;
      roleSpec.requiredArtifact = ndn::Name("/artifact").append(roles[index]);
      roleSpec.terminalResponseOwner = roles[index] == finalRole;
      collaborationPlan.roles.push_back(std::move(roleSpec));
    }
    for (std::size_t index = 0; index + 1 < providerCount; ++index) {
      collaborationPlan.keyScopes.push_back(CollaborationKeyScope{
        "activation-" + std::to_string(index), roles});
      collaborationPlan.dependencies.push_back(CollaborationDependency{
        {roles[index]}, {roles[index + 1]},
        "activation-" + std::to_string(index),
        ndn::Name("/Spec175/activation").append(std::to_string(index)), true});
    }
    collaborationPlan.keyScopes.push_back(CollaborationKeyScope{
      "token-feedback", roles});
    collaborationPlan.dependencies.push_back(CollaborationDependency{
      {finalRole}, {role0}, "token-feedback", ndn::Name("/Spec175/feedback"), true});
    std::map<std::string, ndn::Name> preferredProviderByRole;
    if (caseOptions.allowReplacement) {
      preferredProviderByRole.emplace(
        role0, environment.provider(roleProviderIndex.front()).getName());
      preferredProviderByRole.emplace(
        finalRole,
        environment.provider(closure.requestId == recoveryRequestId
          ? providerCount : roleProviderIndex.back()).getName());
    }
    const auto& assignmentMap =
      caseOptions.allowReplacement && closure.requestId == recoveryRequestId
        ? recoverySelectionAssignmentByProvider
        : selectionAssignmentByProvider;
    collaborationPlan.participantSelector =
      std::make_shared<Spec175AckCapabilitySelection>(
        assignmentMap, std::move(preferredProviderByRole));
    try {
      result.planCommitted = environment.user().CommitCollaborationPlan(
        closure.requestId, closure.digest, std::move(collaborationPlan));
      if (result.planCommitted &&
          caseOptions.fault == Spec175NativeTinyFault::ExpiredDeadline) {
        for (const auto& status :
             environment.user().GetCollaborationStatusSnapshot(closure.requestId)) {
          committedSelectionDigests[status.providerName.toUri()] = status.selectionDigest;
        }
      }
      if (result.planCommitted &&
          caseOptions.suppressProviderPeerSyncAfterCommit) {
        suppressProviderPeerSync->store(true, std::memory_order_release);
        trace("provider-peer-sync-suppressed-after-plan-commit");
      }
      trace("plan-commit request=" + closure.requestId.toUri() +
            " committed=" + std::to_string(result.planCommitted));
    }
    catch (const std::exception& error) {
      result.failed = true;
      result.error = error.what();
    }
  };
  const auto returnedRequestId = environment.user().BeginCollaboration(
    serviceName, requestPayload, 1000, 8000,
    [&] (const CollaborationAckClosure& closure) { commitPlan(closure); },
    [&] (const ResponseMessage&) {},
    [&] (const ndn::Name&) { result.timedOut = true; },
    requestId,
    CollaborationAckCoverageHandler(),
    RequestCapabilities(),
    std::optional<StreamRequestOptions>(streamOptions),
    [&] (const ndn::Buffer& event) {
      const auto inFlight = callbacksInFlight->fetch_add(
        1, std::memory_order_relaxed) + 1;
      auto observed = callbackConcurrencyHighWater->load(
        std::memory_order_relaxed);
      while (inFlight > observed &&
             !callbackConcurrencyHighWater->compare_exchange_weak(
               observed, inFlight, std::memory_order_relaxed)) {
      }
      result.events.emplace_back(
        reinterpret_cast<const char*>(event.data()), event.size());
      trace("event count=" + std::to_string(result.events.size()));
      if ((caseOptions.fault ==
             Spec175NativeTinyFault::ProviderUnavailableAfterEvent3 ||
           caseOptions.fault ==
             Spec175NativeTinyFault::ProviderUnavailableAfterEvent3WithReplacement) &&
          result.events.size() == 3 &&
          providerTransportDetachments->fetch_add(
            1, std::memory_order_relaxed) == 0) {
        // Establish the fault boundary at delivery, not publication: cursor
        // 1-3 are now application-observed, while cursor >=4 remains withheld
        // by the publication interceptor and exact repair cannot reach the
        // disconnected Provider.
        environment.disconnectProviderTransportForTest(finalProviderIndex);
        trace("provider-transport-detached-after-event-3");
      }
      if (caseOptions.fault == Spec175NativeTinyFault::CancelAfterThirdEvent &&
          result.events.size() == 3) {
        result.cancelled = true;
        environment.user().cancelStreamRequestForTest(requestId);
      }
      if (caseOptions.fault == Spec175NativeTinyFault::CallbackThrowsAtThirdEvent &&
          result.events.size() == 3) {
        callbacksInFlight->fetch_sub(1, std::memory_order_relaxed);
        throw std::runtime_error("Spec175 I10 callback failure");
      }
      if (caseOptions.fault == Spec175NativeTinyFault::SlowConsumerCapacityOne) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      callbacksInFlight->fetch_sub(1, std::memory_order_relaxed);
    },
    [&] (const ndn::Buffer& response) {
      result.finalPayload.assign(
        reinterpret_cast<const char*>(response.data()), response.size());
      result.completed = true;
      trace("initial-complete");
    },
    [&] (const StreamedInvocationError& error) {
      trace("initial-error code=" +
            std::to_string(static_cast<int>(error.code)) +
            " cursor=" + std::to_string(error.expectedCursor));
      if (caseOptions.allowReplacement && !replacementStarted) {
        replacementStarted = true;
        trace("replacement-begin");
        StreamRequestOptions recoveryOptions = streamOptions;
        recoveryOptions.attemptEpoch = 2;
        recoveryOptions.streamEpoch = 2;
        recoveryOptions.allowReplacement = false;
        recoveryOptions.maxReplacements = 0;
        recoveryOptions.maxEventRetries = 8;
        recoveryOptions.deadlineEpochMs = 0;
        environment.user().BeginCollaboration(
          serviceName, requestPayload, 1000, 8000,
          [&] (const CollaborationAckClosure& closure) { commitPlan(closure); },
          [&] (const ResponseMessage&) {},
          [&] (const ndn::Name&) { result.timedOut = true; },
          recoveryRequestId,
          CollaborationAckCoverageHandler(),
          RequestCapabilities(),
          std::optional<StreamRequestOptions>(recoveryOptions),
          [&] (const ndn::Buffer& event) {
            const auto inFlight = callbacksInFlight->fetch_add(
              1, std::memory_order_relaxed) + 1;
            auto observed = callbackConcurrencyHighWater->load(
              std::memory_order_relaxed);
            while (inFlight > observed &&
                   !callbackConcurrencyHighWater->compare_exchange_weak(
                     observed, inFlight, std::memory_order_relaxed)) {
            }
            ++recoveryEvents;
            result.events.emplace_back(
              reinterpret_cast<const char*>(event.data()), event.size());
            trace("replacement-event count=" +
                  std::to_string(recoveryEvents));
            if (caseOptions.fault == Spec175NativeTinyFault::SlowConsumerCapacityOne) {
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            callbacksInFlight->fetch_sub(1, std::memory_order_relaxed);
          },
          [&] (const ndn::Buffer& response) {
            result.finalPayload.assign(
              reinterpret_cast<const char*>(response.data()), response.size());
            result.completed = true;
            trace("replacement-complete");
          },
          [&] (const StreamedInvocationError& recoveryError) {
            trace("replacement-error code=" +
                  std::to_string(static_cast<int>(recoveryError.code)) +
                  " cursor=" +
                  std::to_string(recoveryError.expectedCursor));
            result.failed = true;
            result.error = recoveryError.message;
            result.errorCode = recoveryError.code;
            result.errorRequestId = recoveryError.requestId;
            result.errorProviderName = recoveryError.providerName;
            result.errorExpectedCursor = recoveryError.expectedCursor;
          });
        return;
      }
      result.failed = true;
      result.error = error.message;
      result.errorCode = error.code;
      result.errorRequestId = error.requestId;
      result.errorProviderName = error.providerName;
      result.errorExpectedCursor = error.expectedCursor;
    });
  BOOST_REQUIRE_EQUAL(returnedRequestId, requestId);
  const auto terminal = [&] {
    // An already-expired request is rejected by Core before the native handler
    // runs. Observe each committed Selection's real Provider terminal status;
    // nativeFailureObserver must stay silent at this earlier boundary.
    if (caseOptions.fault == Spec175NativeTinyFault::ExpiredDeadline) {
      result.coreDeadlineFailures.clear();
      for (std::size_t index = 0; index < providerCount; ++index) {
        auto& provider = environment.provider(index);
        const auto digest = committedSelectionDigests.find(provider.getName().toUri());
        if (digest == committedSelectionDigests.end()) continue;
        const auto status = provider.getSelectionExecutionStatus(digest->second);
        if (status && status->state == SelectionExecutionState::Failed) {
          result.coreDeadlineFailures.push_back(*status);
        }
      }
    }
    const auto providerDone =
      providerFailures->load(std::memory_order_relaxed) +
      providerCoordinatorCompletions->load(std::memory_order_relaxed) +
      result.coreDeadlineFailures.size() >= providerCount;
    if (caseOptions.fault == Spec175NativeTinyFault::ExpiredDeadline) {
      return providerDone &&
        (result.timedOut || result.failed || result.cancelled || result.completed);
    }
    return result.timedOut ||
      ((result.failed || result.cancelled || result.completed) && providerDone);
  };
  for (int round = 0; round < 32 && !terminal(); ++round) {
    trace("pump-begin round=" + std::to_string(round));
    environment.pumpUntil(terminal);
    trace("pump-end round=" + std::to_string(round) +
          " events=" + std::to_string(result.events.size()) +
          " completed=" + std::to_string(result.completed) +
          " failed=" + std::to_string(result.failed) +
          " providerCompletions=" + std::to_string(
            providerCoordinatorCompletions->load(std::memory_order_relaxed)));
  }
  result.providerCoordinatorCompletions =
    providerCoordinatorCompletions->load(std::memory_order_relaxed);
  result.providerFailures =
    providerFailures->load(std::memory_order_relaxed);
  result.bridgeStats = environment.bridgeStats();
  result.retentionSuppressions =
    retentionSuppressions->load(std::memory_order_relaxed);
  result.retentionExpirations =
    retentionExpirations->load(std::memory_order_relaxed);
  result.publicationSuppressions =
    publicationSuppressions->load(std::memory_order_relaxed);
  result.publicationReorders =
    publicationReorders->load(std::memory_order_relaxed);
  result.publicationDuplicates =
    publicationDuplicates->load(std::memory_order_relaxed);
  result.tamperedPublications = result.bridgeStats.tamperedStreamDataPackets;
  result.publisherQueueHighWater = finalProvider.streamPublisherHighWaterMarkForTest(
    requesterName, serviceName, requestId);
  result.callbackQueueHighWater =
    environment.user().streamCallbackQueueHighWaterMarkForTest(requestId);
  result.callbackConcurrencyHighWater =
    callbackConcurrencyHighWater->load(std::memory_order_relaxed);
  for (const auto& readSnapshot : decodeStateSnapshotReaders) {
    result.decodeStateSnapshots.push_back(readSnapshot());
  }
  result.unselectedProviderExecutions =
    unselectedProviderExecutions->load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(*streamedContextMutex);
  result.streamedProviderCount = static_cast<std::size_t>(std::count(
      streamedContextByRole->begin(), streamedContextByRole->end(), true));
    result.streamedContext = !streamedContextByRole->empty() &&
      streamedContextByRole->back();
  }
  result.replacementProviderExecutions =
    replacementProviderExecutions->load(std::memory_order_relaxed);
  result.replacementEventsPublished =
    replacementEventsPublished->load(std::memory_order_relaxed);
  result.providerTransportDetachments =
    providerTransportDetachments->load(std::memory_order_relaxed);
  if (result.providerFailures != 0 && !result.cancelled) {
    result.failed = true;
    std::lock_guard<std::mutex> lock(*providerFailureMutex);
    result.providerFailureError = *providerFailureMessage;
    if (result.error.empty()) {
      result.error = result.providerFailureError;
    }
  }
  else if (result.completed &&
           result.providerCoordinatorCompletions < providerCount) {
    result.failed = true;
    result.error = "not every Provider epoch coordinator terminated cleanly";
  }
  finalProvider.setStreamRetentionInterceptorForTest({});
  finalProvider.setStreamRetentionExpiryObserverForTest({});
  finalProvider.setStreamPublicationInterceptorForTest({});
  if (requestScope) {
    environment.flushReorderedPackets();
    environment.updateRequestResidue(*requestScope, {});
    environment.resetRequest(*requestScope);
  }
  return result;
#endif
}

Spec175NativeTinyStreamResult
runSpec175NativeTinyTwoProviderCase(
  std::string caseId = "i02",
  Spec175NativeTinyFault fault = Spec175NativeTinyFault::None)
{
  Spec175NativeTinyCaseOptions options;
  options.caseId = std::move(caseId);
  options.fault = fault;
  return runSpec175NativeTinyMultiProviderCase(2, std::move(options));
}

Spec175NativeTinyStreamResult
runSpec175NativeTinyReplacementCase()
{
  Spec175NativeTinyCaseOptions options;
  options.caseId = "i12";
  options.extraUnselectedProvider = true;
  options.allowReplacement = true;
  options.fault = Spec175NativeTinyFault::ProviderUnavailableAfterEvent3WithReplacement;
  return runSpec175NativeTinyMultiProviderCase(2, std::move(options));
}

// R4-B6 keeps the first real requester/provider conversation deliberately
// small.  The Provider is the production ServiceProvider and Core transport;
// its handler emits authenticated stream/receipt/control records directly so
// this case isolates the public native requester transaction from ONNX
// assembly.  The NativeProviderHandler/real model qualification remains T016.
std::shared_ptr<EVP_PKEY>
makeR4B6Ed25519Key(unsigned char seed)
{
  std::array<unsigned char, 32> bytes{};
  bytes.fill(seed);
  auto* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                            bytes.data(), bytes.size());
  if (key == nullptr) {
    throw std::runtime_error("R4-B6 Ed25519 key creation failed");
  }
  return std::shared_ptr<EVP_PKEY>(key, EVP_PKEY_free);
}

std::string
r4B6PublicPem(const std::shared_ptr<EVP_PKEY>& key)
{
  BIO* raw = BIO_new(BIO_s_mem());
  if (raw == nullptr || PEM_write_bio_PUBKEY(raw, key.get()) != 1) {
    if (raw != nullptr) BIO_free(raw);
    throw std::runtime_error("R4-B6 public key PEM encoding failed");
  }
  char* data = nullptr;
  const auto size = BIO_get_mem_data(raw, &data);
  std::string result(data, size > 0 ? static_cast<std::size_t>(size) : 0);
  BIO_free(raw);
  return result;
}

std::string
r4B6SignDigest(const std::shared_ptr<EVP_PKEY>& key, const std::string& digest)
{
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
    EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr,
                                    key.get()) != 1) {
    throw std::runtime_error("R4-B6 offer signer initialization failed");
  }
  std::array<unsigned char, 64> signature{};
  std::size_t length = signature.size();
  if (EVP_DigestSign(context.get(), signature.data(), &length,
                     reinterpret_cast<const unsigned char*>(digest.data()),
                     digest.size()) != 1 || length != signature.size()) {
    throw std::runtime_error("R4-B6 offer signing failed");
  }
  std::array<unsigned char, 89> encoded{};
  if (EVP_EncodeBlock(encoded.data(), signature.data(),
                      static_cast<int>(length)) != 88) {
    throw std::runtime_error("R4-B6 offer signature encoding failed");
  }
  return std::string(reinterpret_cast<const char*>(encoded.data()), 88);
}

void
runR4B6RealProviderConversationCase(bool exerciseReplacement = false,
                                    bool alternateProvider = false,
                                    bool repositoryInput = false,
                                    bool unaryRequest = false,
                                    bool conversationRequest = true)
{
  using namespace ndn_service_framework;
  test::BootstrapProfile profile;
  profile.groupPrefix = ndn::Name("/ndnsf/spec182/r4-b6");
  profile.syncPrefix = ndn::Name("/ndnsf/spec182/r4-b6/sync");
  profile.userNode = ndn::Name("/ndnsf/spec182/r4-b6/user");
  profile.providerNode = ndn::Name("/ndnsf/spec182/r4-b6/provider");
  profile.userIdentity = ndn::Name("/spec182/r4-b6/user");
  profile.providerIdentity = ndn::Name("/spec182/r4-b6/provider");
  profile.attributeAuthority = ndn::Name("/spec182/r4-b6/aa");
  profile.serviceName = ndn::Name("/Spec182/R4B6/Conversation");
  profile.providerCount = alternateProvider ? 2 : 1;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName.toUri();
  const auto requesterName = environment.user().getName().toUri();
  const auto providerName = environment.provider().getName().toUri();
  const auto alternateProviderName = alternateProvider ?
    environment.provider(1).getName().toUri() : std::string{};
  const std::string role = "/LLM/Pipeline/Stage/0";
  const std::string protectionEpoch = "protected-r4-b6";
  const std::string policyDigest = nativePlanningDigest("r4-b6-policy");
  const std::string graphDigest = nativePlanningDigest("r4-b6-graph");
  const std::string contentDigest = nativePlanningDigest("r4-b6-content");
  const std::string semanticsDigest = nativePlanningDigest("r4-b6-semantics");
  const std::string manifestDigest = nativePlanningDigest("r4-b6-manifest");
  const std::string recipeDigest = nativePlanningDigest("r4-b6-recipe");
  const std::string artifactDigest = nativePlanningDigest("r4-b6-artifact");
  const std::string artifactProfileDigest = nativePlanningDigest("r4-b6-profile");
  const std::string tokenizerDigest = nativePlanningDigest("r4-b6-tokenizer");
  const std::string roleMapDigest = nativePlanningDigest(
    nativeCanonicalJson(NativeJson::array({NativeJson::array({role, providerName})})));

  NativeModelDescriptor model{
    "r4-b6-model", contentDigest, semanticsDigest, graphDigest, "onnx", "fp32",
    "r4-b6-adapter", "1"};
  model = fixture::completeModel(std::move(model));
  NativeGraphSnapshot graph;
  graph.graphDigest = graphDigest;
  graph.nodes = {{"node", "Identity", 0}};
  graph.topologicalOrder = {"node"};
  graph.modelInputs = {
    {"input_ids", "int64", {std::int64_t(1), std::int64_t(1)}, std::nullopt},
    {"attention_kv_in", "float32", {std::int64_t(1)}, std::nullopt},
    {"recurrent_state_in", "float32", {std::int64_t(1)}, std::nullopt},
    {"convolution_state_in", "float32", {std::int64_t(1)}, std::nullopt}};
  graph.modelOutputs = {
    {"logits", "float32", {std::int64_t(1)}, std::nullopt},
    {"attention_kv_out", "float32", {std::int64_t(1)}, std::nullopt},
    {"recurrent_state_out", "float32", {std::int64_t(1)}, std::nullopt},
    {"convolution_state_out", "float32", {std::int64_t(1)}, std::nullopt}};
  NativeInspectedModel inspected{
    model, graph, "/r4-b6/catalog/model", nativePlanningDigest("r4-b6-source"),
    manifestDigest, graphDigest};
  inspected.validate();

  NativeSelectionRoleV3 preparedRole;
  preparedRole.role = preparedRole.selectedRole = role;
  preparedRole.layerEnd = 1;
  preparedRole.backend = "onnxruntime-cpu";
  preparedRole.artifactDigest = artifactDigest;
  preparedRole.recipeDigest = recipeDigest;
  preparedRole.roleKind = "PIPELINE_RANGE";
  preparedRole.adapterId = model.adapterId;
  preparedRole.adapterVersion = model.adapterVersion;
  preparedRole.modelManifestDigest = manifestDigest;
  preparedRole.artifactProfileDigest = artifactProfileDigest;
  preparedRole.graphDigest = graphDigest;
  preparedRole.canonicalInitializerDigest = nativePlanningDigest("r4-b6-initializers");
  preparedRole.adapterDescriptorDigest = nativePlanningDigest("fixture-adapter");
  preparedRole.assemblerDescriptorDigest = nativePlanningDigest("r4-b6-assembler");
  preparedRole.backendAbi = "onnxruntime-cpu-v1";
  preparedRole.nodeIndices = {0};
  preparedRole.expectedInputs = {
    {"input_ids", "int64", {std::int64_t(1), std::int64_t(1)}},
    {"attention_kv_in", "float32", {std::int64_t(1)}},
    {"recurrent_state_in", "float32", {std::int64_t(1)}},
    {"convolution_state_in", "float32", {std::int64_t(1)}}};
  preparedRole.expectedOutputs = {
    {"logits", "float32", {std::int64_t(1)}},
    {"attention_kv_out", "float32", {std::int64_t(1)}},
    {"recurrent_state_out", "float32", {std::int64_t(1)}},
    {"convolution_state_out", "float32", {std::int64_t(1)}}};
  preparedRole.precision = "fp32";
  preparedRole.quantization = "none";
  preparedRole.layout = "native";
  preparedRole.padding = "none";
  preparedRole.protectionEpoch = protectionEpoch;
  preparedRole.maxSourceBytes = 4096;
  preparedRole.maxAssembledBytes = 8192;
  preparedRole.maxNodes = 16;

  NativeSplitCandidate candidate;
  candidate.source = "PRE_SPLIT";
  candidate.splitter = {"r4-b6-fixture", "1", nativePlanningDigest("r4-b6-split")};
  candidate.model = model;
  candidate.graphDigest = graphDigest;
  candidate.executionPlan.serviceName = serviceName;
  candidate.executionPlan.modelName = model.modelName;
  candidate.executionPlan.roles = {role};
  candidate.fragmentsByRole[role] = nativePlanningDigest("r4-b6-fragment");
  candidate.artifactsByRole[role] = {artifactDigest};
  candidate.requirementsByRole[role] = { {"onnxruntime-cpu"}, 0, 0, 0, 0, 0, 1.0 };
  candidate.tensorDegreesByRole[role] = 1;
  candidate.rankArtifactDigestsByRole[role] = {artifactDigest};
  candidate.nodeRoles["node"] = role;
  candidate.inputIngressRole = role;
  candidate.resultEgressRole = role;
  candidate.candidateDigest = candidate.computedDigest();
  candidate.validate(graph);

  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(std::make_shared<NativeCatalogModelAdapter>(
    std::vector<NativeModelDescriptor>{model}, NativeCatalogModelAdapter::Format::OpaqueBytes, 4096));
  registry->freeze();
  NativeArtifactBinding binding;
  binding.artifactNameByRole[role] = "/r4-b6/catalog/artifact";
  binding.artifactDigestByRole[role] = artifactDigest;
  binding.manifestDigest = manifestDigest;
  binding.recipeDigest = recipeDigest;
  const std::vector<std::uint8_t> catalogRootPayload = {
    'r', '4', '-', 'b', '6', '-', 'c', 'a', 't', 'a', 'l', 'o', 'g', '-', 'r', 'o', 'o', 't'};
  const auto catalogContext = environment.user().prepareServiceRequest(serviceName);
  const auto catalogPublished = environment.user().publishEncryptedLargeData(
      catalogContext, catalogRootPayload, "catalog-root",
      ndn::time::milliseconds(60000));
  BOOST_REQUIRE_MESSAGE(catalogPublished.success,
                        catalogPublished.errorMessage);
  binding.sourceByRole[role] = catalogPublished.encryptedDataName.toUri();
  auto preparation = std::make_shared<NativeRequestPreparation>(
    registry,
    [inspected] (const auto&, const auto&) { return inspected; },
    [binding] (const auto&, const auto&, const auto&, const auto&) { return binding; },
    [preparedRole] (const auto&, const auto&, const auto&) {
      return std::vector<NativeSelectionRoleV3>{preparedRole};
    });

  std::string repositoryReference;
  std::string repositoryPlaintext;
  if (repositoryInput) {
    const NativeJson reference = {
      {"source", "repo-manifest"},
      {"dataName", catalogPublished.encryptedDataName.toUri()},
      {"manifestDigest", catalogPublished.manifestDigest},
      {"plaintextSize", catalogPublished.plaintextSize},
      {"ciphertextDigest", catalogPublished.contentDigest},
      {"authorizationScope", catalogPublished.authorizationScope},
      {"protectionEpoch", catalogPublished.protectionEpoch},
      {"encrypted", true},
      {"objectType", "DI_INPUT"},
      {"objectId", catalogPublished.objectId},
    };
    repositoryReference = nativeCanonicalJson(reference);
    repositoryPlaintext.assign(catalogRootPayload.begin(), catalogRootPayload.end());
  }

  const auto offerKey = makeR4B6Ed25519Key(0x31);
  std::array<unsigned char, 32> offerPublic{};
  std::size_t offerPublicSize = offerPublic.size();
  if (EVP_PKEY_get_raw_public_key(offerKey.get(), offerPublic.data(), &offerPublicSize) != 1 ||
      offerPublicSize != offerPublic.size()) {
    throw std::runtime_error("R4-B6 offer public key extraction failed");
  }
  const auto offerKeyId = nativePlanningDigest(std::string(
    reinterpret_cast<const char*>(offerPublic.data()), offerPublic.size()));
  NativeProviderOfferV3Config offerConfig;
  offerConfig.provider = providerName;
  offerConfig.service = serviceName;
  offerConfig.bootEpoch = providerName + ":" + environment.provider().getProviderBootEpoch();
  offerConfig.signerKeyId = offerKeyId;
  offerConfig.acceptedRoles = {role};
  offerConfig.backends = {"onnxruntime-cpu"};
  offerConfig.signDigest = [offerKey] (const std::string& value) {
    return r4B6SignDigest(offerKey, value);
  };
  auto policyJson = nativeCanonicalJson(NativeJson{
    {"schema", "spec180-provider-offer-trust-v1"},
    {"candidateId", "r4-b6"}, {"candidateDigest", nativePlanningDigest("r4-b6-candidate-policy")},
    {"trustSchema", "/r4-b6/trust"},
    {"entries", NativeJson::array({NativeJson{
      {"provider", providerName}, {"service", serviceName},
      {"keyLocatorPrefix", environment.provider().getSigningKeyName().toUri()},
      {"signerKeyId", offerKeyId},
      {"certificateName", environment.provider().getSigningCertificateName().toUri()}}})}});
  if (alternateProvider) {
    auto policy = nativeParseJson(policyJson);
    policy.at("entries").push_back(NativeJson{
      {"provider", alternateProviderName}, {"service", serviceName},
      {"keyLocatorPrefix", environment.provider(1).getSigningKeyName().toUri()},
      {"signerKeyId", offerKeyId},
      {"certificateName", environment.provider(1).getSigningCertificateName().toUri()}});
    policyJson = nativeCanonicalJson(policy);
  }
  const auto candidatePolicyDigest = nativeParseJson(policyJson).at("candidateDigest").get<std::string>();
  auto admission = std::make_shared<NativeOfferAdmission>(
    policyJson, std::map<std::string, std::string>{{offerKeyId, r4B6PublicPem(offerKey)}},
    candidatePolicyDigest);

  const auto requesterKey = makeR4B6Ed25519Key(0x41);
  const auto authorityKey = makeR4B6Ed25519Key(0x51);
  const auto recipientKey = makeR4B6Ed25519Key(0x61);
  std::array<unsigned char, 32> authorityPublic{};
  std::size_t authorityPublicSize = authorityPublic.size();
  if (EVP_PKEY_get_raw_public_key(authorityKey.get(), authorityPublic.data(),
                                  &authorityPublicSize) != 1 ||
      authorityPublicSize != authorityPublic.size()) {
    throw std::runtime_error("R4-B6 authority public key extraction failed");
  }
  NativeGrantIssuerConfig issuerConfig;
  issuerConfig.authorityIdentity = "/r4-b6/authority";
  issuerConfig.requesterIdentity = requesterName;
  issuerConfig.protectionEpoch = protectionEpoch;
  issuerConfig.keyId = "r4-b6-grant-key";
  issuerConfig.authorityPrivateKey = authorityKey;
  issuerConfig.requesterPublicKey = requesterKey;
  issuerConfig.allowedModelManifests = {manifestDigest};
  issuerConfig.recipientPublicKeys = {{providerName, recipientKey}};
  if (alternateProvider)
    issuerConfig.recipientPublicKeys.emplace(alternateProviderName, recipientKey);
  issuerConfig.contentKey = [] (const auto&, const auto&) {
    return std::vector<std::uint8_t>(32, 0x77);
  };
  NativeAuthenticatedGrantClient::Publish publishGrant =
    [] (const std::string& name, const std::string&, const NativeGrantControl&) {
      return name;
    };
  const auto authorityIdentity = issuerConfig.authorityIdentity;
  auto grantIssuer = std::make_shared<NativeArtifactGrantIssuer>(std::move(issuerConfig));
  BOOST_TEST_MESSAGE("R4-B6 grant requester=" << requesterName
                    << " authority=" << authorityIdentity
                    << " requesterKeyId=" << EVP_PKEY_id(requesterKey.get())
                    << " authorityPublicBytes=" << authorityPublicSize
                    << " publish=" << static_cast<bool>(publishGrant));
  auto grants = std::make_shared<NativeAuthenticatedGrantClient>(
    requesterName, requesterKey, authorityIdentity,
    std::string(reinterpret_cast<const char*>(authorityPublic.data()), authorityPublic.size()),
    grantIssuer,
    std::move(publishGrant));

  auto user = std::shared_ptr<ServiceUser>(&environment.user(), [] (ServiceUser*) {});
  auto ackCalls = std::make_shared<std::atomic<unsigned>>(0);
  auto collaborationCalls = std::make_shared<std::atomic<unsigned>>(0);
  auto conversationAttempts = std::make_shared<std::atomic<unsigned>>(0);
  auto repositoryReferenceObserved = std::make_shared<std::atomic<bool>>(false);
  auto observedRequestWireSize = std::make_shared<std::atomic<std::size_t>>(0);
  const auto expectedReferenceDataName = catalogPublished.encryptedDataName.toUri();
  const auto expectedReferenceManifestDigest = catalogPublished.manifestDigest;
  NativeProviderOfferV3Config alternateOfferConfig;
  if (alternateProvider) {
    alternateOfferConfig = offerConfig;
    alternateOfferConfig.provider = alternateProviderName;
    alternateOfferConfig.bootEpoch = alternateProviderName + ":" +
      environment.provider(1).getProviderBootEpoch();
  }
  const auto installProviderHandler = [&] (
    ServiceProvider& provider, const NativeProviderOfferV3Config& localOfferConfig,
    bool failFirst) {
    const auto localProviderBootId = localOfferConfig.provider + "-boot";
    provider.addCollaborationHandler(
    ndn::Name(serviceName),
    [localOfferConfig, ackCalls, repositoryReferenceObserved, observedRequestWireSize,
     expectedReferenceDataName, expectedReferenceManifestDigest] (const RequestMessage& request) {
      ackCalls->fetch_add(1, std::memory_order_relaxed);
      const auto payload = request.getPayload();
      observedRequestWireSize->store(payload.size(), std::memory_order_relaxed);
      if (!payload.empty()) {
        const std::string requestWire(payload.begin(), payload.end());
        try {
          const auto root = nativeParseJson(requestWire);
          std::string dataName;
          std::string manifestDigest;
          if (root.contains("input_reference") && root.at("input_reference").is_object())
          {
            dataName = root.at("input_reference").value("dataName", std::string{});
            manifestDigest = root.at("input_reference").value("manifestDigest", std::string{});
          }
          if (root.value("input_transport", std::string{}) == "REPO_REF" &&
              root.value("input_payload_b64", std::string{}).empty() &&
              dataName == expectedReferenceDataName &&
              manifestDigest == expectedReferenceManifestDigest)
            repositoryReferenceObserved->store(true, std::memory_order_relaxed);
        }
        catch (...) {
          // The offer issuer below remains the authoritative request parser;
          // this is a narrow observation oracle and must not alter it.
        }
      }
      ServiceProvider::AckDecision decision;
      const auto issued = issueNativeProviderOfferV3(
        std::vector<std::uint8_t>(payload.begin(), payload.end()), localOfferConfig,
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count()));
      if (!issued || !issued->status) {
        decision.message = issued ? issued->message : "DI_V3_REQUEST_NOT_SELECTED";
        return decision;
      }
      decision.status = true;
      decision.message = issued->message;
      decision.payload = ndn::Buffer(issued->payload.begin(), issued->payload.end());
      decision.pendingStateTtlMs = issued->pendingStateTtlMs;
      return decision;
    },
    [model, policyDigest, protectionEpoch, role, serviceName, localProviderBootId, collaborationCalls,
     conversationAttempts, failFirst, repositoryInput, unaryRequest, conversationRequest,
     expectedReferenceDataName, repositoryPlaintext] (
      ServiceProvider::CollaborationContext& ctx, const RequestMessage& request) {
      try {
        collaborationCalls->fetch_add(1, std::memory_order_relaxed);
        conversationAttempts->fetch_add(1, std::memory_order_relaxed);
        if (repositoryInput) {
          const auto requestPayload = request.getPayload();
          const auto requestRoot = nativeParseJson(std::string(
            requestPayload.begin(), requestPayload.end()));
          if (requestRoot.value("input_transport", std::string{}) != "REPO_REF" ||
              !requestRoot.contains("input_reference") ||
              !requestRoot.at("input_reference").is_object())
            throw std::runtime_error("R4-B6 repository reference missing at Provider");
          const auto dataName = requestRoot.at("input_reference").value(
            "dataName", std::string{});
          if (dataName != expectedReferenceDataName)
            throw std::runtime_error("R4-B6 repository reference identity changed");
          const auto fetched = ctx.fetchEncryptedLargeData(
            ndn::Name(dataName), ndn::Name(serviceName));
          if (!fetched || std::string(fetched->begin(), fetched->end()) != repositoryPlaintext)
            throw std::runtime_error("R4-B6 repository reference fetch mismatch");
        }
        if (unaryRequest) {
          const std::string responseText = "native-unary-response";
          const ndn::Buffer responsePayload(
            reinterpret_cast<const uint8_t*>(responseText.data()), responseText.size());
          ctx.publishFinalResponse(responsePayload);
          return;
        }
        const auto assignment = ctx.assignment().assignmentPayload;
        std::istringstream input(std::string(assignment.begin(), assignment.end()));
        const auto projection = nativeSelectionProjectionV3FromJson(input, role);
        if (!ctx.isStreamed()) {
          throw std::runtime_error("R4-B6 stream projection missing");
        }
        if (conversationRequest && !projection.conversationTurnBinding) {
          throw std::runtime_error("R4-B6 conversation projection missing");
        }
        if (failFirst && projection.attempt == 1) {
          if (!ctx.failStream(StreamedInvocationErrorCode::ProviderFailure,
                              "R4-B6 injected provider failure before first event")) {
            throw std::runtime_error("R4-B6 replacement failure injection rejected");
          }
          return;
        }
        const auto publishToken = [&ctx, &projection] (std::int64_t token,
                                                        std::uint64_t epoch,
                                                        const std::string& prefix,
                                                        const std::string& delta,
                                                        const std::string& hint) {
          const auto wire = nativeCanonicalJson(NativeJson{
            {"schema", "GenerationTokenEventV1"}, {"tokenId", token},
            {"tokenEpoch", epoch}, {"acceptedPrefixDigest", nativePlanningDigest(prefix)},
            {"textDelta", delta}, {"finishHint", hint},
            {"generationId", projection.generationContract.generationId},
            {"samplingDigest", projection.generationContract.samplingDigest}});
          if (!ctx.publishStreamEvent(ndn::Buffer(wire.begin(), wire.end()))) {
            throw std::runtime_error("R4-B6 stream event publication failed");
          }
        };
        if (!conversationRequest) {
          publishToken(1, 1, "1", "a", "NONE");
          publishToken(2, 2, "1,2", "b", "EOS");
          const auto final = nativeCanonicalJson(NativeJson{
            {"schema", "NDNSF-DI-FINAL-V1"}, {"tokenIds", NativeJson::array({1, 2})},
            {"text", "ab"}, {"finishHint", "EOS"}, {"finishReason", "eos"},
            {"generationId", projection.generationContract.generationId}});
          if (!ctx.finishStream(ndn::Buffer(final.begin(), final.end()),
                                StreamFinishReason::ApplicationComplete)) {
            throw std::runtime_error("R4-B6 stream-only final publication failed");
          }
          return;
        }
        const auto binding = *projection.conversationTurnBinding;
        ctx.subscribe("ndnsf-di-conversation-state-v1",
                      ndn::Name("/ndnsf-di/conversation/control"),
                      [] (const ServiceProvider::CollaborationData&) {});
        const bool append = binding.parentContextEpoch != 0;
        const std::vector<std::int64_t> fullTokens = append
          // The continuation carries the reconstructed context [1,2,3];
          // the streamed delta contributes token 3 as the next generated
          // token, so the coordinator's completed prefix is [1,2,3,3].
          ? std::vector<std::int64_t>{1, 2, 3, 3}
          : std::vector<std::int64_t>{1, 2};
        if (append) {
          publishToken(3, 1, "3", "c", "EOS");
        }
        else {
          publishToken(1, 1, "1", "a", "NONE");
          publishToken(2, 2, "1,2", "b", "EOS");
        }
        const auto text = append ? std::string("c") : std::string("ab");
        const auto deltaTokens = append ? std::vector<std::int64_t>{3} :
          std::vector<std::int64_t>{1, 2};
        const auto final = nativeCanonicalJson(NativeJson{
          {"schema", "NDNSF-DI-FINAL-V1"}, {"tokenIds", deltaTokens},
          {"text", text}, {"finishHint", "EOS"}, {"finishReason", "eos"},
          {"generationId", projection.generationContract.generationId}});
        if (!ctx.finishStream(ndn::Buffer(final.begin(), final.end()),
                              StreamFinishReason::ApplicationComplete)) {
          throw std::runtime_error("R4-B6 stream final publication failed");
        }

        ProviderConversationStateReceiptV1 receipt;
        receipt.conversationId = binding.conversationId;
        receipt.parentContextEpoch = binding.parentContextEpoch;
        receipt.successorContextEpoch = binding.successorContextEpoch;
        receipt.originRequestId = ctx.sessionId();
        receipt.originGenerationId = projection.generationContract.generationId;
        receipt.serviceName = binding.serviceName;
        receipt.requesterIdentity = ctx.requesterName().toUri();
        receipt.securityDomainDigest = policyDigest;
        receipt.modelDigest = model.intentDigest();
        receipt.graphSemanticDigest = model.semanticsDigest;
        receipt.adapterDigest = model.adapter.descriptorDigest();
        receipt.roleName = role;
        receipt.roleSplitDigest = projection.selectedRole.recipeDigest;
        receipt.layoutDigest = projection.selectedRole.artifactProfileDigest;
        receipt.planRoleMapDigest = binding.planRoleMapDigest;
        receipt.providerIdentity = ctx.localProvider().toUri();
        receipt.providerBootId = localProviderBootId;
        receipt.cacheEpoch = append ? 2 : 1;
        receipt.prefixDigest = nativeConversationPrefixDigest(fullTokens);
        receipt.prefixTokenCount = static_cast<std::uint32_t>(fullTokens.size());
        receipt.positionDigest = nativePlanningDigest("r4-b6-position");
        receipt.stateSchemaDigest = nativePlanningDigest("r4-b6-state-schema");
        receipt.stateComponentDigests = {nativePlanningDigest("r4-b6-state")};
        receipt.expiresAtMs = binding.retentionDeadlineMs;
        receipt.validate();
        const auto receiptJson = receipt.toJson();
        ctx.publish("ndnsf-di-conversation-state-v1",
                    ndn::Name("/ndnsf-di/conversation/receipt").append(role),
                    ndn::Buffer(receiptJson.begin(), receiptJson.end()));

        const auto controlTopic = ndn::Name("/ndnsf-di/conversation/control");
        const auto commitTopic = ndn::Name("/ndnsf-di/conversation/commit");
        // A failed test must not leave the collaboration worker waiting
        // forever for a control message after the requester has aborted.
        for (int rounds = 0; rounds < 30; ++rounds) {
          const auto controls = ctx.waitFor("ndnsf-di-conversation-state-v1",
                                             controlTopic, 1, 1000);
          for (const auto& controlData : controls) {
            if (controlData.producer != ctx.requesterName() ||
                controlData.producerRole != "user-control-v1") {
              continue;
            }
            const auto control = nativeParseJson(std::string(
              controlData.payload.begin(), controlData.payload.end()));
            if (control.value("conversationId", std::string{}) != binding.conversationId ||
                control.value("successorContextEpoch", std::uint64_t{0}) != binding.successorContextEpoch ||
                control.value("roleName", std::string{}) != role) {
              continue;
            }
            const auto action = control.value("action", std::string{});
            if (action == "FINALIZE") return;
            if (action != "COMMIT") continue;
            const auto ack = nativeCanonicalJson(NativeJson{
              {"schema", "ndnsf-di-provider-conversation-commit-ack-v1"},
              {"requestId", ctx.sessionId()}, {"attemptEpoch", projection.attempt},
              {"generationId", projection.generationContract.generationId},
              {"planDigest", projection.planDigest}, {"conversationId", binding.conversationId},
              {"parentContextEpoch", binding.parentContextEpoch},
              {"successorContextEpoch", binding.successorContextEpoch},
              {"serviceName", binding.serviceName}, {"planRoleMapDigest", binding.planRoleMapDigest},
              {"roleName", role}, {"receiptDigest", receipt.computedDigest()},
              {"checkpointDigest", control.value("checkpointDigest", std::string{})},
              {"providerIdentity", ctx.localProvider().toUri()},
              {"providerBootId", localProviderBootId}, {"cacheEpoch", receipt.cacheEpoch},
              {"committed", true}});
            ctx.publish("ndnsf-di-conversation-state-v1", commitTopic,
                        ndn::Buffer(ack.begin(), ack.end()));
            // A COMMIT control is terminal for this role.  Returning here
            // prevents the retained control record from being processed in a
            // tight loop while the acknowledgement is handed to the Face.
            return;
          }
        }
        throw std::runtime_error("R4-B6 conversation control timeout");
      }
      catch (const std::exception& error) {
        ctx.fail(std::string("R4-B6 handler failure: ") + error.what());
      }
    });
  };
  installProviderHandler(environment.provider(), offerConfig,
                         exerciseReplacement);
  if (alternateProvider)
    installProviderHandler(environment.provider(1), alternateOfferConfig, false);

  environment.enableProductionIngressForTest();
  for (std::size_t index = 0; index < environment.providerCount(); ++index) {
    auto& provider = environment.provider(index);
    provider.markHybridResponseKeyWrappedForTest(serviceName);
    const auto ackKey = provider.prepareHybridSendKeyForTest(serviceName, "ACK");
    const auto responseKey = provider.prepareHybridSendKeyForTest(serviceName, "RESPONSE");
    environment.user().cacheHybridReceiveKeyForTest(
      ackKey.keyId, ackKey.epochId, ackKey.key);
    environment.user().cacheHybridReceiveKeyForTest(
      responseKey.keyId, responseKey.epochId, responseKey.key);
  }
  const auto selectionKey = environment.user().prepareHybridSendKeyForTest(
    serviceName, "SELECTION");
  for (std::size_t index = 0; index < environment.providerCount(); ++index)
    environment.provider(index).cacheHybridReceiveKeyForTest(
      selectionKey.keyId, selectionKey.epochId, selectionKey.key);

  NativeRequestRuntime runtime;
  runtime.contract = {serviceName, "task", model.adapterId,
    model.adapter.descriptorDigest(), nativePlanningDigest("r4-b6-composition"),
    nativePlanningDigest("r4-b6-task"), unaryRequest ? "TOKEN_DIAGNOSTIC" : "TOKEN_STREAMING"};
  runtime.requesterIdentity = requesterName;
  runtime.protectionEpoch = protectionEpoch;
  runtime.inputLayoutDigest = nativePlanningDigest("r4-b6-input-layout");
  runtime.security = {policyDigest, true};
  runtime.budget = {1, 1000, 1};
  runtime.grants = grants;
  runtime.noProgressMs = 5000;
  runtime.maxSegments = 64;

  NativeModelRef modelRef;
  static_cast<NativeModelDescriptor&>(modelRef) = model;
  NativeApplicationInput application;
  application.taskName = "task";
  application.inputSchemaDigest = model.adapter.inputSchemaDigest;
  application.optionsSchemaDigest = model.adapter.optionsSchemaDigest;
  if (repositoryInput) {
    application.transportMode = NativeInputTransportMode::RepositoryReference;
    application.repositoryReference = repositoryReference;
  }
  else {
    application.payload = {1, 2, 3};
  }
  const auto generationOptions = nativeCanonicalJson(NativeJson{
    {"useCache", true}, {"outputMode", "TOKEN_STREAMING"}, {"maxNewTokens", 2},
    {"eosTokenIds", NativeJson::array({2})}, {"tokenizerDigest", tokenizerDigest},
    {"tokenInputName", "input_ids"},
    {"stateInputNames", NativeJson::array({"attention_kv_in", "recurrent_state_in", "convolution_state_in"})},
    {"stateOutputNames", NativeJson::array({"attention_kv_out", "recurrent_state_out", "convolution_state_out"})},
    {"greedy", true}});
  application.options.assign(generationOptions.begin(), generationOptions.end());
  NativeRequestOptions options;
  options.taskName = "task";
  options.timeoutMs = 20000;
  options.ackTimeoutMs = 3000;
  if (!unaryRequest) {
    options.stream = StreamRequestOptions{};
    options.stream->generationId.fill(0x11);
    options.stream->maxEvents = 8;
    options.stream->interestWindow = 4;
    options.stream->retentionMs = 5000;
    options.stream->allowReplacement = exerciseReplacement;
    options.stream->maxReplacements = exerciseReplacement ? 1 : 0;
    options.generation = nativeGenerationFromOptions(application.options,
                                                       std::string(32, '1'));
    if (conversationRequest) {
      const auto retentionDeadlineMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count()) + 60000;
      options.conversation = NativeConversationContinuation{
        "r4-b6-conversation-001", 0, serviceName, roleMapDigest, {}, {},
        retentionDeadlineMs, "FULL_CONTEXT", {}, std::string(32, '1'), {}, {role}};
    }
  }

  NativeConversationConfig conversationConfig;
  conversationConfig.authenticationKeys = {std::vector<std::uint8_t>(32, 0x5a)};
  conversationConfig.requesterIdentity = requesterName;
  conversationConfig.serviceName = serviceName;
  conversationConfig.securityDomainDigest = policyDigest;
  auto conversations = std::make_shared<NativeConversationCoordinator>(
    std::move(conversationConfig));
  NativeInferenceClient client(user, registry, runtime, conversations,
                               preparation, admission);
  class R4B6Splitter final : public NativeModelSplitStrategy {
  public:
    explicit R4B6Splitter(NativeSplitCandidate value) : m_value(std::move(value)) {}
    NativeStrategyIdentity identity() const override { return m_value.splitter; }
    std::vector<NativeSplitCandidate> enumerate(const NativeModelDescriptor&, const NativeGraphSnapshot&,
                                                const NativeCandidateBudget&) const override { return {m_value}; }
  private:
    NativeSplitCandidate m_value;
  };
  auto splitter = std::make_shared<R4B6Splitter>(candidate);
  auto placement = std::make_shared<NativePreSplitFirstPlacement>();
  const auto first = client.request(modelRef, application,
                                    splitter, placement, options);
  for (int i = 0; i < 20 && first.status() == NativeRequestStatus::Pending; ++i) {
    environment.pumpUntil([&] { return first.status() != NativeRequestStatus::Pending; });
  }
  if (first.status() != NativeRequestStatus::Succeeded) {
    try { (void)first.result(std::chrono::milliseconds(0)); }
    catch (const NativeDiError& error) {
      BOOST_TEST_MESSAGE("R4-B6 first failed code=" << error.code()
                        << " domain=" << error.domain()
                        << " boundary=" << error.boundary()
                        << " message=" << error.what()
                        << " ackCalls=" << ackCalls->load()
                        << " requestWireBytes=" << observedRequestWireSize->load()
                        << " collaborationCalls=" << collaborationCalls->load());
    }
  }
  if (unaryRequest) {
    BOOST_REQUIRE(first.status() == NativeRequestStatus::Succeeded);
    const auto result = first.result(std::chrono::milliseconds(0));
    BOOST_CHECK_EQUAL(std::string(result.payload.begin(), result.payload.end()),
                      "native-unary-response");
    client.close();
    return;
  }
  if (!conversationRequest) {
    BOOST_REQUIRE(first.status() == NativeRequestStatus::Succeeded);
    const auto result = first.result(std::chrono::milliseconds(0));
    const auto resultJson = nativeParseJson(
      std::string(result.payload.begin(), result.payload.end()));
    BOOST_CHECK_EQUAL(resultJson.at("text").get<std::string>(), "ab");
    BOOST_CHECK(!conversations->find("r4-b6-conversation-001").has_value());
    client.close();
    return;
  }
  if (exerciseReplacement && !alternateProvider) {
    // This fixture deliberately has one Provider.  A failed Provider is
    // excluded from the recovery ACK, so the native client must reject the
    // replacement before publishing a conversation checkpoint.
    BOOST_REQUIRE(first.status() == NativeRequestStatus::Failed);
    try {
      (void)first.result(std::chrono::milliseconds(0));
      BOOST_FAIL("single-provider replacement unexpectedly succeeded");
    }
    catch (const NativeDiError& error) {
      BOOST_CHECK_EQUAL(error.code(), "NATIVE_REQUEST_STAGE_FAILED");
      BOOST_CHECK(error.what() != nullptr);
      BOOST_CHECK(std::string(error.what()).find("DI_NATIVE_NO_ADMITTED_PROVIDER") !=
                  std::string::npos);
      BOOST_CHECK_EQUAL(error.boundary(), "ACK_CLOSED");
    }
    BOOST_CHECK_EQUAL(conversationAttempts->load(std::memory_order_relaxed), 1U);
    BOOST_CHECK(!conversations->find("r4-b6-conversation-001").has_value());
    client.close();
    return;
  }

  BOOST_REQUIRE(first.status() == NativeRequestStatus::Succeeded);
  if (repositoryInput)
    BOOST_REQUIRE(repositoryReferenceObserved->load(std::memory_order_relaxed));
  const auto firstResult = first.result(std::chrono::milliseconds(0));
  const auto firstJson = nativeParseJson(
    std::string(firstResult.payload.begin(), firstResult.payload.end()));
  BOOST_CHECK_EQUAL(firstJson.at("text").get<std::string>(), "ab");
  const auto record = conversations->find("r4-b6-conversation-001");
  BOOST_REQUIRE(record.has_value());
  if (alternateProvider) {
    const auto replacementRoleMapDigest = nativePlanningDigest(nativeCanonicalJson(
      NativeJson::array({NativeJson::array({role, alternateProviderName})})));
    BOOST_CHECK_EQUAL(conversationAttempts->load(std::memory_order_relaxed), 2U);
    BOOST_CHECK_EQUAL(record->planRoleMapDigest, replacementRoleMapDigest);
    BOOST_CHECK(record->checkpoint.requestId.find(first.requestId() + "/recovery/") == 0);
    BOOST_CHECK_NE(record->checkpoint.requestId, first.requestId());
    client.close();
    return;
  }
  auto secondOptions = options;
  secondOptions.generation = nativeGenerationFromOptions(application.options,
                                                         std::string(32, '1'));
  secondOptions.conversation = NativeConversationContinuation{
    record->checkpoint.conversationId, record->checkpoint.successorContextEpoch,
    serviceName, roleMapDigest, record->checkpoint.checkpointDigest, {},
    record->retentionDeadlineMs,
    "APPEND_DELTA", record->checkpoint.wire, std::string(32, '1'), {1, 2, 3}, {role}};
  const auto second = client.request(modelRef, application,
                                     std::make_shared<R4B6Splitter>(candidate), placement,
                                     secondOptions);
  for (int i = 0; i < 20 && second.status() == NativeRequestStatus::Pending; ++i) {
    environment.pumpUntil([&] { return second.status() != NativeRequestStatus::Pending; });
  }
  if (second.status() != NativeRequestStatus::Succeeded) {
    try { (void)second.result(std::chrono::milliseconds(0)); }
    catch (const NativeDiError& error) {
      BOOST_TEST_MESSAGE("R4-B6 second failed code=" << error.code()
                        << " domain=" << error.domain()
                        << " boundary=" << error.boundary()
                        << " message=" << error.what());
    }
  }
  BOOST_REQUIRE(second.status() == NativeRequestStatus::Succeeded);
  const auto secondResult = second.result(std::chrono::milliseconds(0));
  const auto secondJson = nativeParseJson(
    std::string(secondResult.payload.begin(), secondResult.payload.end()));
  BOOST_CHECK_EQUAL(secondJson.at("text").get<std::string>(), "c");
  // The closure runner consumes this marker as an independent business oracle.
  // Emit it only after the second native result assertion has completed.
  std::cout << "SPEC182_NATIVE_DI_REQUEST_RESULT_OK\n" << std::flush;
  client.close();
}

Spec175NativeTinyStreamResult
runSpec175NativeTinyFourProviderCase(bool permuteRoleProviders)
{
  Spec175NativeTinyCaseOptions options;
  options.caseId = permuteRoleProviders ? "i15" : "i03";
  options.permuteRoleProviders = permuteRoleProviders;
  return runSpec175NativeTinyMultiProviderCase(4, std::move(options));
}

BOOST_AUTO_TEST_CASE(ProductionNativeHandlersRunD2bRequestToFinalResponse)
{
  runProductionNativeD2bCase(false);
}

BOOST_AUTO_TEST_CASE(ProductionNativeHandlersRunStreamedD2bRequestToFinalResponse)
{
  runProductionNativeD2bCase(false, true);
}

BOOST_AUTO_TEST_CASE(ProductionNativeHandlersPrepareRolesAfterSelection)
{
  runProductionNativeD2bCase(false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec182R4B6RealProviderConversation)
{
  runR4B6RealProviderConversationCase();
}

BOOST_AUTO_TEST_CASE(Spec182R4B6RealProviderConversationReplacement)
{
  runR4B6RealProviderConversationCase(true);
}

BOOST_AUTO_TEST_CASE(Spec182R4B6RealProviderConversationAlternateReplacement)
{
  runR4B6RealProviderConversationCase(true, true);
}

BOOST_AUTO_TEST_CASE(Spec182R4B6RealProviderRepositoryReference)
{
  runR4B6RealProviderConversationCase(false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec182R10B31RealProviderUnaryRequest)
{
  runR4B6RealProviderConversationCase(false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec182R10B33RealProviderUnaryRepositoryReferenceRequest)
{
  runR4B6RealProviderConversationCase(false, false, true, true);
}

BOOST_AUTO_TEST_CASE(Spec182R10B37RealProviderNativeStreamRequest)
{
  runR4B6RealProviderConversationCase(false, false, false, false, false);
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI01OneProvider)
{
  const auto result = runSpec175NativeTinyOneRoleCase();
  BOOST_TEST_MESSAGE("Spec175 I01 ackCandidates=" << result.ackCandidates
                    << " ackProvider=" << result.ackProvider
                    << " ackService=" << result.ackService
                    << " requestObserved=" << result.requestObserved
                    << " ackPublicationObserved=" << result.ackPublicationObserved
                    << " requestName=" << result.requestPublicationName
                    << " ackName=" << result.ackPublicationName
                    << " ackClosed=" << result.ackClosed
                    << " planCommitted=" << result.planCommitted
                    << " streamedContext=" << result.streamedContext
                    << " completed=" << result.completed
                    << " failed=" << result.failed
                    << " timedOut=" << result.timedOut
                    << " events=" << result.events.size()
                    << " oracleFinal=" << result.finalPayload
                    << " oracleFirstEvent="
                    << (result.events.empty() ? std::string() : result.events.front())
                    << " oracleLastEvent="
                    << (result.events.empty() ? std::string() : result.events.back())
                    << " oracleEvents=" << spec175EventOracle(result.events)
                    << " error=" << result.error);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(result.streamedContext);
  BOOST_REQUIRE(!result.failed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE(result.completed);
  BOOST_REQUIRE_EQUAL(result.events.size(), 8U);
  BOOST_REQUIRE_EQUAL(result.decodeStateSnapshots.size(), 1U);
  const auto& state = result.decodeStateSnapshots.front();
  BOOST_CHECK_EQUAL(state.commits, 8U);
  BOOST_CHECK_EQUAL(state.hits, 7U);
  BOOST_CHECK_EQUAL(state.misses, 0U);
  BOOST_CHECK_EQUAL(state.entries, 0U);
  BOOST_CHECK_EQUAL(state.pinnedEntries, 0U);
  BOOST_CHECK_EQUAL(state.candidates, 0U);
  BOOST_CHECK_EQUAL(state.cleanups, 1U);
  BOOST_CHECK(result.events.front().find("\"tokenId\":4") != std::string::npos);
  BOOST_CHECK(result.events.back().find("\"tokenId\":2") != std::string::npos);
  BOOST_CHECK(result.events.back().find("\"finishHint\":\"EOS\"") !=
              std::string::npos);
  BOOST_CHECK(result.finalPayload.find(
    "\"tokenIds\":[4,5,6,7,8,9,10,2]") != std::string::npos);
  BOOST_CHECK(result.events.front().find(
    "\"textDelta\":\"token-4\"") != std::string::npos);
  BOOST_CHECK(result.finalPayload.find(
    "\"text\":\"token-4 token-5 token-6 token-7 token-8 token-9 token-10 token-2\"") !=
              std::string::npos);

  const std::vector<std::int64_t> expectedTokens{4, 5, 6, 7, 8, 9, 10, 2};
  std::vector<std::int64_t> cachedTokens;
  for (const auto& event : result.events) {
    const auto marker = event.find("\"tokenId\":");
    BOOST_REQUIRE(marker != std::string::npos);
    const auto begin = marker + std::string("\"tokenId\":").size();
    const auto end = event.find(',', begin);
    BOOST_REQUIRE(end != std::string::npos);
    cachedTokens.push_back(std::stoll(event.substr(begin, end - begin)));
  }
  BOOST_CHECK(cachedTokens == expectedTokens);

  BOOST_REQUIRE_EQUAL(result.cacheObservations.size(), expectedTokens.size());
  std::size_t totalPrefixWorkAvoided = 0;
  for (std::size_t epoch = 0; epoch < result.cacheObservations.size(); ++epoch) {
    const auto& observation = result.cacheObservations[epoch];
    BOOST_CHECK_EQUAL(observation.inferenceEpoch, epoch);
    BOOST_CHECK_EQUAL(observation.actualNewInputExtent, 1U);
    BOOST_CHECK_EQUAL(observation.representedPrefixTokenCount, epoch + 1);
    BOOST_CHECK_EQUAL(observation.prefixWorkAvoided, epoch);
    BOOST_CHECK_EQUAL(observation.decodeStateHit, epoch > 0);
    totalPrefixWorkAvoided += observation.prefixWorkAvoided;
  }
  BOOST_CHECK_EQUAL(totalPrefixWorkAvoided, 28U);

  const auto tokenText = [] (const std::vector<std::int64_t>& tokens) {
    std::ostringstream value;
    for (const auto token : tokens) {
      if (value.tellp() > 0) value << ',';
      value << token;
    }
    return value.str();
  };
  BOOST_TEST_MESSAGE("Spec175 CPU cache control cached=" << tokenText(cachedTokens)
                    << " fullPrefix=" << tokenText(result.fullPrefixControlTokens));
  BOOST_REQUIRE_EQUAL(result.fullPrefixControlTokens.size(), cachedTokens.size());
  BOOST_CHECK_EQUAL_COLLECTIONS(result.fullPrefixControlTokens.begin(),
                                result.fullPrefixControlTokens.end(),
                                cachedTokens.begin(), cachedTokens.end());
  BOOST_CHECK(result.fullPrefixControlInputExtents ==
              std::vector<std::size_t>({1, 2, 3, 4, 5, 6, 7, 8}));
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI02TwoProviderEpochCoordinator)
{
  const auto result = runSpec175NativeTinyTwoProviderCase();
  BOOST_TEST_MESSAGE("Spec175 I02 ackCandidates=" << result.ackCandidates
                    << " ackClosed=" << result.ackClosed
                    << " planCommitted=" << result.planCommitted
                    << " streamedContext=" << result.streamedContext
                    << " completed=" << result.completed
                    << " failed=" << result.failed
                    << " timedOut=" << result.timedOut
                    << " providerCompletions="
                    << result.providerCoordinatorCompletions
                    << " providerFailures=" << result.providerFailures
                    << " events=" << result.events.size()
                    << " error=" << result.error
                    << " providerError=" << result.providerFailureError);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(result.streamedContext);
  BOOST_REQUIRE(!result.failed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE(result.completed);
  BOOST_REQUIRE_EQUAL(result.providerCoordinatorCompletions, 2U);
  BOOST_REQUIRE_EQUAL(result.providerFailures, 0U);
  BOOST_REQUIRE_EQUAL(result.events.size(), 4U);
  BOOST_REQUIRE_EQUAL(result.decodeStateSnapshots.size(), 2U);
  for (const auto& state : result.decodeStateSnapshots) {
    BOOST_CHECK_EQUAL(state.commits, 4U);
    BOOST_CHECK_EQUAL(state.hits, 3U);
    BOOST_CHECK_EQUAL(state.misses, 0U);
    BOOST_CHECK_EQUAL(state.entries, 0U);
    BOOST_CHECK_EQUAL(state.pinnedEntries, 0U);
    BOOST_CHECK_EQUAL(state.candidates, 0U);
    BOOST_CHECK_EQUAL(state.cleanups, 1U);
  }
  BOOST_CHECK(result.events.front().find("\"tokenId\":4") != std::string::npos);
  BOOST_CHECK(result.events.back().find("\"tokenId\":7") != std::string::npos);
  BOOST_CHECK(result.finalPayload.find(
    "\"tokenIds\":[4,5,6,7]") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI03FourProviderEpochCoordinator)
{
  const auto result = runSpec175NativeTinyFourProviderCase(false);
  BOOST_TEST_MESSAGE("Spec175 I03 ackCandidates=" << result.ackCandidates
                    << " ackClosed=" << result.ackClosed
                    << " planCommitted=" << result.planCommitted
                    << " streamedContext=" << result.streamedContext
                    << " completed=" << result.completed
                    << " failed=" << result.failed
                    << " timedOut=" << result.timedOut
                    << " providerCompletions="
                    << result.providerCoordinatorCompletions
                    << " providerFailures=" << result.providerFailures
                    << " events=" << result.events.size()
                    << " oracleFinal=" << result.finalPayload
                    << " oracleFirstEvent="
                    << (result.events.empty() ? std::string() : result.events.front())
                    << " oracleLastEvent="
                    << (result.events.empty() ? std::string() : result.events.back())
                    << " oracleEvents=" << spec175EventOracle(result.events)
                    << " error=" << result.error);
  BOOST_REQUIRE_EQUAL(result.ackCandidates, 4U);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(result.streamedContext);
  BOOST_REQUIRE(!result.failed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE(result.completed);
  BOOST_REQUIRE_EQUAL(result.providerCoordinatorCompletions, 4U);
  BOOST_REQUIRE_EQUAL(result.providerFailures, 0U);
  BOOST_REQUIRE_EQUAL(result.events.size(), 8U);
  BOOST_REQUIRE_EQUAL(result.decodeStateSnapshots.size(), 4U);
  for (const auto& state : result.decodeStateSnapshots) {
    BOOST_CHECK_EQUAL(state.commits, 8U);
    BOOST_CHECK_EQUAL(state.hits, 7U);
    BOOST_CHECK_EQUAL(state.misses, 0U);
    BOOST_CHECK_EQUAL(state.entries, 0U);
    BOOST_CHECK_EQUAL(state.pinnedEntries, 0U);
    BOOST_CHECK_EQUAL(state.candidates, 0U);
    BOOST_CHECK_EQUAL(state.cleanups, 1U);
  }
  BOOST_CHECK(result.events.front().find("\"tokenId\":4") != std::string::npos);
  BOOST_CHECK(result.events.back().find("\"tokenId\":2") != std::string::npos);
  BOOST_CHECK(result.finalPayload.find(
    "\"tokenIds\":[4,5,6,7,8,9,10,2]") != std::string::npos);
  BOOST_CHECK(result.events.front().find(
    "\"textDelta\":\"token-4\"") != std::string::npos);
  BOOST_CHECK(result.finalPayload.find(
    "\"text\":\"token-4 token-5 token-6 token-7 token-8 token-9 token-10 token-2\"") !=
              std::string::npos);
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI04ReordersEventThreeAfterFour)
{
  const auto result = runSpec175NativeTinyTwoProviderCase(
    "i04", Spec175NativeTinyFault::ReorderEvent3After4);
  BOOST_TEST_MESSAGE("Spec175 I04 completed=" << result.completed
                    << " failed=" << result.failed
                    << " publicationReorders="
                    << result.publicationReorders
                    << " events=" << result.events.size()
                    << " error=" << result.error);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(!result.failed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE(result.completed);
  BOOST_REQUIRE_EQUAL(result.publicationSuppressions, 1U);
  BOOST_REQUIRE_EQUAL(result.publicationReorders, 1U);
  BOOST_REQUIRE_EQUAL(result.events.size(), 8U);
  BOOST_CHECK(result.events.front().find("\"tokenId\":4") != std::string::npos);
  BOOST_CHECK(result.events.back().find("\"tokenId\":2") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI05SuppressesDuplicateEventFour)
{
  const auto result = runSpec175NativeTinyTwoProviderCase(
    "i05", Spec175NativeTinyFault::DuplicateEvent4);
  BOOST_TEST_MESSAGE("Spec175 I05 completed=" << result.completed
                    << " failed=" << result.failed
                    << " publicationDuplicates="
                    << result.publicationDuplicates
                    << " events=" << result.events.size()
                    << " error=" << result.error);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(!result.failed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE(result.completed);
  BOOST_REQUIRE_EQUAL(result.publicationDuplicates, 1U);
  BOOST_REQUIRE_EQUAL(result.events.size(), 8U);
}

BOOST_AUTO_TEST_CASE(Spec175DataV1ExactDependencySurvivesSuppressedSvsUpdates)
{
  Spec175NativeTinyCaseOptions options;
  options.caseId = "data-v1-exact-without-peer-sync";
  options.suppressProviderPeerSyncAfterCommit = true;
  const auto result = runSpec175NativeTinyMultiProviderCase(
    2, std::move(options));
  BOOST_TEST_MESSAGE("Spec175 exact dependency completed=" << result.completed
                    << " failed=" << result.failed
                    << " events=" << result.events.size()
                    << " error=" << result.error);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(!result.failed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE(result.completed);
  BOOST_REQUIRE_EQUAL(result.events.size(), 8U);
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI06RetriesFirstLostEventFive)
{
  const auto result = runSpec175NativeTinyTwoProviderCase(
    "i06", Spec175NativeTinyFault::DropFirstEvent5Data);
  BOOST_TEST_MESSAGE("Spec175 I06 completed=" << result.completed
                    << " failed=" << result.failed
                    << " droppedData="
                    << result.bridgeStats.droppedStreamDataPackets
                    << " events=" << result.events.size()
                    << " error=" << result.error);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(!result.failed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE(result.completed);
  BOOST_REQUIRE_EQUAL(result.publicationSuppressions, 1U);
  BOOST_REQUIRE_EQUAL(result.bridgeStats.droppedStreamDataPackets, 1U);
  BOOST_REQUIRE_EQUAL(result.events.size(), 8U);
  BOOST_CHECK(result.finalPayload.find(
    "\"tokenIds\":[4,5,6,7,8,9,10,2]") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI07FailsEveryUnavailableEventFiveMode)
{
  const auto verifyFailure = [] (const Spec175NativeTinyStreamResult& result,
                                 const std::string& subcase) {
    BOOST_TEST_CONTEXT(subcase) {
      BOOST_REQUIRE(result.ackClosed);
      BOOST_REQUIRE(result.planCommitted);
      BOOST_REQUIRE(!result.completed);
      BOOST_REQUIRE(!result.timedOut);
      BOOST_REQUIRE(result.failed);
      BOOST_CHECK(result.errorCode == StreamedInvocationErrorCode::EventTimeout);
      BOOST_CHECK_EQUAL(result.errorExpectedCursor, 5U);
      BOOST_CHECK(!result.errorRequestId.empty());
      BOOST_CHECK(!result.errorProviderName.empty());
      BOOST_CHECK(result.errorProviderName.toUri().front() == '/');
      BOOST_CHECK(result.error.find("gap exceeded retry budget") !=
                  std::string::npos);
    }
  };

  const auto neverRetained = runSpec175NativeTinyTwoProviderCase(
    "i07-never-retained", Spec175NativeTinyFault::NeverRetainedEvent5);
  BOOST_TEST_MESSAGE("Spec175 I07 never-retained suppressions="
                    << neverRetained.retentionSuppressions
                    << " error=" << neverRetained.error);
  verifyFailure(neverRetained, "never-retained");
  BOOST_REQUIRE_EQUAL(neverRetained.retentionSuppressions, 1U);

  const auto retentionExpired = runSpec175NativeTinyTwoProviderCase(
    "i07-retention-expired", Spec175NativeTinyFault::RetentionExpiredEvent5);
  BOOST_TEST_MESSAGE("Spec175 I07 retention-expired publicationSuppressions="
                    << retentionExpired.publicationSuppressions
                    << " retentionExpirations="
                    << retentionExpired.retentionExpirations
                    << " droppedInterests="
                    << retentionExpired.bridgeStats.droppedStreamInterests
                    << " error=" << retentionExpired.error);
  verifyFailure(retentionExpired, "retention-expired");
  BOOST_REQUIRE_EQUAL(retentionExpired.publicationSuppressions, 1U);
  BOOST_REQUIRE_EQUAL(retentionExpired.retentionExpirations, 1U);
  BOOST_REQUIRE_GE(retentionExpired.bridgeStats.droppedStreamInterests, 1U);

  const auto endBeforeGap = runSpec175NativeTinyTwoProviderCase(
    "i07-end-before-gap", Spec175NativeTinyFault::EndBeforeGapEvent5);
  BOOST_TEST_MESSAGE("Spec175 I07 end-before-gap droppedData="
                    << endBeforeGap.bridgeStats.droppedStreamDataPackets
                    << " error=" << endBeforeGap.error);
  verifyFailure(endBeforeGap, "end-before-gap");
  BOOST_REQUIRE_EQUAL(endBeforeGap.publicationSuppressions, 1U);
  BOOST_REQUIRE_GE(endBeforeGap.bridgeStats.droppedStreamDataPackets, 1U);
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI08CancelsAfterThirdEvent)
{
  const auto result = runSpec175NativeTinyTwoProviderCase(
    "i08", Spec175NativeTinyFault::CancelAfterThirdEvent);
  BOOST_TEST_MESSAGE("Spec175 I08 cancelled=" << result.cancelled
                    << " completed=" << result.completed
                    << " failed=" << result.failed
                    << " events=" << result.events.size());
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(result.cancelled);
  BOOST_REQUIRE(!result.completed);
  BOOST_REQUIRE(!result.failed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE_GE(result.providerCoordinatorCompletions + result.providerFailures,
                   2U);
  // Cancellation is issued synchronously when the third event is delivered;
  // no fourth event may cross the terminal guard.
  BOOST_REQUIRE_EQUAL(result.events.size(), 3U);
  BOOST_REQUIRE_EQUAL(result.decodeStateSnapshots.size(), 2U);
  for (const auto& state : result.decodeStateSnapshots) {
    BOOST_CHECK_EQUAL(state.entries, 0U);
    BOOST_CHECK_EQUAL(state.pinnedEntries, 0U);
    BOOST_CHECK_EQUAL(state.candidates, 0U);
    BOOST_CHECK_EQUAL(state.cleanups, 1U);
  }
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxExpiredDeadlineCleansProviderState)
{
  const auto result = runSpec175NativeTinyTwoProviderCase(
    "expired-deadline", Spec175NativeTinyFault::ExpiredDeadline);
  BOOST_TEST_MESSAGE("Spec175 expired deadline ackClosed=" << result.ackClosed
                    << " planCommitted=" << result.planCommitted
                    << " timedOut=" << result.timedOut
                    << " failed=" << result.failed
                    << " providerFailures=" << result.providerFailures
                    << " events=" << result.events.size()
                    << " error=" << result.error);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(!result.completed);
  BOOST_REQUIRE(result.timedOut || result.failed);
  BOOST_REQUIRE_EQUAL(result.providerFailures, 0U);
  BOOST_REQUIRE_EQUAL(result.providerCoordinatorCompletions, 0U);
  BOOST_REQUIRE_EQUAL(result.coreDeadlineFailures.size(), 2U);
  for (const auto& status : result.coreDeadlineFailures) {
    BOOST_CHECK_EQUAL(status.runningAtUs, 0U);
    BOOST_CHECK(!status.selectionDigest.empty());
    BOOST_CHECK(status.message.find("REQUEST_DEADLINE") != std::string::npos);
  }
  BOOST_REQUIRE(result.events.empty());
  BOOST_REQUIRE_EQUAL(result.decodeStateSnapshots.size(), 2U);
  for (const auto& state : result.decodeStateSnapshots) {
    BOOST_CHECK_EQUAL(state.commits, 0U);
    BOOST_CHECK_EQUAL(state.entries, 0U);
    BOOST_CHECK_EQUAL(state.pinnedEntries, 0U);
    BOOST_CHECK_EQUAL(state.candidates, 0U);
  }
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI09RejectsTamperAndWithholdsUnselectedGrant)
{
  Spec175NativeTinyCaseOptions options;
  options.caseId = "i09";
  options.extraUnselectedProvider = true;
  options.fault = Spec175NativeTinyFault::TamperEventOneSignature;
  const auto result = runSpec175NativeTinyMultiProviderCase(2, std::move(options));
  BOOST_TEST_MESSAGE("Spec175 I09 ackCandidates=" << result.ackCandidates
                    << " streamedProviders=" << result.streamedProviderCount
                    << " unselectedExecutions="
                    << result.unselectedProviderExecutions
                    << " tamperedPublications=" << result.tamperedPublications
                    << " providerCompletions="
                    << result.providerCoordinatorCompletions
                    << " providerFailures=" << result.providerFailures
                    << " error=" << result.error);
  BOOST_REQUIRE_EQUAL(result.ackCandidates, 3U);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(!result.completed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE(result.failed);
  BOOST_REQUIRE_EQUAL(result.providerCoordinatorCompletions, 2U);
  BOOST_REQUIRE_EQUAL(result.providerFailures, 0U);
  BOOST_REQUIRE_EQUAL(result.streamedProviderCount, 1U);
  BOOST_REQUIRE(result.streamedContext);
  BOOST_REQUIRE_EQUAL(result.unselectedProviderExecutions, 0U);
  BOOST_REQUIRE_EQUAL(result.tamperedPublications, 1U);
  BOOST_REQUIRE(result.events.empty());
  BOOST_CHECK(result.errorCode == StreamedInvocationErrorCode::DecryptionFailed);
  BOOST_CHECK_EQUAL(result.errorExpectedCursor, 1U);
  BOOST_CHECK_EQUAL(result.errorRequestId, ndn::Name("/spec175-native-tiny-i09"));
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI10ContainsThirdCallbackFailure)
{
  const auto result = runSpec175NativeTinyTwoProviderCase(
    "i10", Spec175NativeTinyFault::CallbackThrowsAtThirdEvent);
  BOOST_TEST_MESSAGE("Spec175 I10 completed=" << result.completed
                    << " failed=" << result.failed
                    << " events=" << result.events.size()
                    << " error=" << result.error);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(!result.completed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE(result.failed);
  BOOST_REQUIRE_EQUAL(result.providerCoordinatorCompletions, 2U);
  BOOST_REQUIRE_EQUAL(result.events.size(), 3U);
  BOOST_CHECK(result.errorCode ==
              StreamedInvocationErrorCode::ApplicationCallbackFailed);
  BOOST_CHECK_EQUAL(result.errorExpectedCursor, 4U);
  BOOST_CHECK_EQUAL(result.errorRequestId, ndn::Name("/spec175-native-tiny-i10"));
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI11BoundsCapacityOneSlowConsumer)
{
  const auto result = runSpec175NativeTinyTwoProviderCase(
    "i11", Spec175NativeTinyFault::SlowConsumerCapacityOne);
  BOOST_TEST_MESSAGE("Spec175 I11 completed=" << result.completed
                    << " events=" << result.events.size()
                    << " publisherHighWater="
                    << result.publisherQueueHighWater
                    << " callbackQueueHighWater="
                    << result.callbackQueueHighWater
                    << " callbackConcurrencyHighWater="
                    << result.callbackConcurrencyHighWater
                    << " error=" << result.error);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(!result.failed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE(result.completed);
  BOOST_REQUIRE_EQUAL(result.providerCoordinatorCompletions, 2U);
  BOOST_REQUIRE_EQUAL(result.events.size(), 8U);
  BOOST_REQUIRE_EQUAL(result.publisherQueueHighWater, 1U);
  BOOST_REQUIRE_EQUAL(result.callbackQueueHighWater, 1U);
  BOOST_REQUIRE_EQUAL(result.callbackConcurrencyHighWater, 1U);
  BOOST_CHECK(result.finalPayload.find(
    "\"tokenIds\":[4,5,6,7,8,9,10,2]") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI12ProviderUnavailableAfterEvent3WithReplacement)
{
  const auto result = runSpec175NativeTinyReplacementCase();
  BOOST_TEST_MESSAGE("Spec175 I12 requests=" << result.requestPublicationCount
                    << " completed=" << result.completed
                    << " failed=" << result.failed
                    << " events=" << result.events.size()
                    << " replacementExecutions="
                    << result.replacementProviderExecutions
                    << " replacementEventsPublished="
                    << result.replacementEventsPublished
                    << " coordinators=" << result.providerCoordinatorCompletions
                    << " suppressed=" << result.publicationSuppressions
                    << " errorCode=" << static_cast<int>(result.errorCode)
                    << " expectedCursor=" << result.errorExpectedCursor
                    << " errorRequest=" << result.errorRequestId
                    << " errorProvider=" << result.errorProviderName
                    << " error=" << result.error);
  BOOST_REQUIRE_EQUAL(result.requestPublicationCount, 2U);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.recoveryAckClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(!result.failed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE(result.completed);
  // The first consumer is fenced when the terminal Provider disappears;
  // require a delivered post-reselection event without claiming that the
  // pre-failure callback prefix was replayed to the application.
  BOOST_REQUIRE_GE(result.events.size(), 1U);
  BOOST_REQUIRE_EQUAL(result.replacementProviderExecutions, 1U);
  BOOST_REQUIRE_EQUAL(result.providerTransportDetachments, 1U);
  BOOST_REQUIRE_GE(result.providerCoordinatorCompletions, 2U);
  BOOST_REQUIRE_GE(result.publicationSuppressions, 1U);
  BOOST_CHECK(result.finalPayload.find(
    "\"tokenIds\":[4,5,6,7]") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI16SeededUnicodeAndSplitStop)
{
  Spec175NativeTinyCaseOptions options;
  options.caseId = "i16-seeded-unicode-stop";
  options.samplingMode = "SeededTopKTopP";
  options.samplingTemperature = 1.0;
  options.samplingTopK = 3;
  options.samplingTopP = 0.95;
  options.samplingRepetitionPenalty = 1.0;
  options.samplingSeed = 42;
  options.stopStrings = {"好🙂"};
  options.tokenizerPath = findSpec175TinyUnicodeTokenizer();
  options.tokenizerDigest =
    "sha256:90db6ef1a0f74a22133269b170a5a4c08a5a4c0d5a76a746614f779e5b2f2404";
  BOOST_REQUIRE(!options.tokenizerPath.empty());

  const auto result = runSpec175NativeTinyMultiProviderCase(4, std::move(options));
  BOOST_TEST_MESSAGE("Spec175 I16 seeded-unicode-stop completed=" << result.completed
                    << " failed=" << result.failed
                    << " timedOut=" << result.timedOut
                    << " providerCompletions="
                    << result.providerCoordinatorCompletions
                    << " providerFailures=" << result.providerFailures
                    << " events=" << result.events.size()
                    << " oracleFinal=" << result.finalPayload
                    << " oracleFirstEvent="
                    << (result.events.empty() ? std::string() : result.events.front())
                    << " oracleLastEvent="
                    << (result.events.empty() ? std::string() : result.events.back())
                    << " oracleEvents=" << spec175EventOracle(result.events)
                    << " error=" << result.error);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(result.streamedContext);
  BOOST_REQUIRE(!result.failed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE(result.completed);
  BOOST_REQUIRE_EQUAL(result.providerCoordinatorCompletions, 4U);
  BOOST_REQUIRE_EQUAL(result.providerFailures, 0U);
  BOOST_REQUIRE_EQUAL(result.events.size(), 3U);
  BOOST_CHECK(result.events[0].find("\"tokenId\":4") != std::string::npos);
  BOOST_CHECK(result.events[0].find("\"textDelta\":\"你\"") !=
              std::string::npos);
  BOOST_CHECK(result.events[1].find("\"tokenId\":5") != std::string::npos);
  BOOST_CHECK(result.events[1].find("\"textDelta\":\"好\"") !=
              std::string::npos);
  BOOST_CHECK(result.events[2].find("\"tokenId\":6") != std::string::npos);
  BOOST_CHECK(result.events[2].find("\"textDelta\":\"🙂\"") !=
              std::string::npos);
  BOOST_CHECK(result.events[2].find("\"finishHint\":\"STOP_SEQUENCE\"") !=
              std::string::npos);
  BOOST_CHECK(result.finalPayload.find(
    "\"tokenIds\":[4,5,6]") != std::string::npos);
  BOOST_CHECK(result.finalPayload.find(
    "\"finishReason\":\"stop_sequence\"") != std::string::npos);
  BOOST_CHECK(result.finalPayload.find("\"text\":\"你好🙂\"") !=
              std::string::npos);
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI13ProviderUnavailableAfterEvent3NoReplacement)
{
  const auto result = runSpec175NativeTinyTwoProviderCase(
    "i13", Spec175NativeTinyFault::ProviderUnavailableAfterEvent3);
  BOOST_TEST_MESSAGE("Spec175 I13 requests=" << result.requestPublicationCount
                    << " completed=" << result.completed
                    << " failed=" << result.failed
                    << " events=" << result.events.size()
                    << " transportDetachments="
                    << result.providerTransportDetachments
                    << " suppressed=" << result.publicationSuppressions
                    << " droppedData=" << result.bridgeStats.droppedStreamDataPackets
                    << " droppedInterests=" << result.bridgeStats.droppedStreamInterests
                    << " error=" << result.error);
  BOOST_REQUIRE_EQUAL(result.requestPublicationCount, 1U);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(!result.completed);
  BOOST_REQUIRE(result.failed || result.timedOut);
  BOOST_REQUIRE_EQUAL(result.providerTransportDetachments, 1U);
  BOOST_REQUIRE_EQUAL(result.events.size(), 3U);
  if (result.failed) {
    BOOST_CHECK(result.errorCode == StreamedInvocationErrorCode::EventTimeout);
    BOOST_CHECK_EQUAL(result.errorExpectedCursor, 4U);
    BOOST_CHECK_EQUAL(result.errorRequestId,
                      ndn::Name("/spec175-native-tiny-i13"));
    BOOST_CHECK(!result.errorProviderName.empty());
  }
}

BOOST_AUTO_TEST_CASE(Spec175NativeTinyOnnxI15PermutedRoleProviderMap)
{
  const auto result = runSpec175NativeTinyFourProviderCase(true);
  BOOST_TEST_MESSAGE("Spec175 I15 ackCandidates=" << result.ackCandidates
                    << " ackClosed=" << result.ackClosed
                    << " planCommitted=" << result.planCommitted
                    << " streamedContext=" << result.streamedContext
                    << " completed=" << result.completed
                    << " failed=" << result.failed
                    << " timedOut=" << result.timedOut
                    << " providerCompletions="
                    << result.providerCoordinatorCompletions
                    << " providerFailures=" << result.providerFailures
                    << " permutedRoleProviderMap="
                    << result.permutedRoleProviderMap
                    << " events=" << result.events.size()
                    << " error=" << result.error);
  BOOST_REQUIRE_EQUAL(result.ackCandidates, 4U);
  BOOST_REQUIRE(result.ackClosed);
  BOOST_REQUIRE(result.planCommitted);
  BOOST_REQUIRE(result.streamedContext);
  BOOST_REQUIRE(!result.failed);
  BOOST_REQUIRE(!result.timedOut);
  BOOST_REQUIRE(result.completed);
  BOOST_REQUIRE_EQUAL(result.providerCoordinatorCompletions, 4U);
  BOOST_REQUIRE_EQUAL(result.providerFailures, 0U);
  BOOST_REQUIRE(result.permutedRoleProviderMap);
  BOOST_REQUIRE_EQUAL(result.events.size(), 8U);
  BOOST_CHECK(result.events.front().find("\"tokenId\":4") != std::string::npos);
  BOOST_CHECK(result.events.back().find("\"tokenId\":2") != std::string::npos);
  BOOST_CHECK(result.finalPayload.find(
    "\"tokenIds\":[4,5,6,7,8,9,10,2]") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ProductionNativeHandlersRejectTamperedD2bCapability)
{
  runProductionNativeD2bCase(true);
}

BOOST_AUTO_TEST_CASE(ProductionNativeHandlersRunD2h121ToOracleResponse)
{
  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/D2h121NativeE2e");
  profile.providerCount = 2;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  std::vector<ndn::signal::ScopedConnection> providerPeerBridges;
  for (size_t source = 0; source < environment.providerCount(); ++source) {
    for (size_t destination = 0; destination < environment.providerCount(); ++destination) {
      if (source == destination) {
        continue;
      }
      providerPeerBridges.emplace_back(
        environment.providerFace(source).onSendInterest.connect(
          [&environment, destination] (const ndn::Interest& interest) {
            environment.providerFace(destination).receive(interest);
          }));
      providerPeerBridges.emplace_back(
        environment.providerFace(source).onSendData.connect(
          [&environment, destination] (const ndn::Data& data) {
            environment.providerFace(destination).receive(data);
          }));
    }
  }

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  const std::array<ndn::Name, 2> providerNames{{
    environment.provider(0).getName(), environment.provider(1).getName(),
  }};
  auto provider0Prefix = environment.profile().providerNode;
  provider0Prefix.append("0");
  auto provider1Prefix = environment.profile().providerNode;
  provider1Prefix.append("p1").append("0");
  const ndn::Name requestId("/d2h-121-native-e2e");
  const auto planDigest = "sha256:" + std::string(64, '1');
  const auto layout0 = "sha256:" + std::string(64, '2');
  const auto layout1 = "sha256:" + std::string(64, '3');
  const auto layout2 = "sha256:" + std::string(64, '4');
  const auto tensor0 = "sha256:" + std::string(64, '5');
  const auto tensor1 = "sha256:" + std::string(64, '6');

  const std::array<std::vector<std::string>, 2> localRoles{{
    {"S0R0", "S1R0"}, {"S1R1", "S2R0"},
  }};
  // This legacy D2h compatibility case intentionally colocates dependent
  // rank roles on each Provider.  The current placement baseline assigns one
  // role per Provider, but the compatibility gate must still avoid creating
  // an artificial single-worker deadlock when external assignment fetches
  // complete out of order.
  for (size_t provider = 0; provider < localRoles.size(); ++provider) {
    environment.provider(provider).setHandlerThreads(
      localRoles[provider].size());
  }
  NativeProviderAssignment assignment;
  for (size_t provider = 0; provider < localRoles.size(); ++provider) {
    for (const auto& role : localRoles[provider]) {
      assignment.providerByRole[role] = providerNames[provider].toUri();
    }
  }

  auto redistribution = [&] (std::vector<std::uint64_t> producers,
                              std::vector<std::uint64_t> consumers,
                              std::string tensor,
                              std::string operation,
                              std::string sourceLayout,
                              std::string targetLayout,
                              std::string integrity) {
    RedistributionSpec value;
    value.producerRanks = std::move(producers);
    value.consumerRanks = std::move(consumers);
    value.tensor = std::move(tensor);
    value.operation = std::move(operation);
    value.epoch = "epoch-1";
    value.integrityDigest = std::move(integrity);
    value.sourceLayoutDigest = std::move(sourceLayout);
    value.targetLayoutDigest = std::move(targetLayout);
    value.axis = 1;
    value.temporaryMemoryBytes = 64U * 1024U;
    value.completeOutput = true;
    return value;
  };

  NativeDependencySpec scatter(
    {"S0R0"}, {"S1R0", "S1R1"}, "boundary-0", "/activation",
    "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}");
  scatter.expectedSegments = 0;
  scatter.tensors = {"activation-0"};
  scatter.useNdnsfDataV1 = true;
  scatter.collectiveOperationIndex = 0;
  scatter.collectiveSourceLayoutDigest = layout0;
  scatter.collectiveTargetLayoutDigest = layout1;
  scatter.collectiveTensorDigest = tensor0;
  scatter.redistributions = {redistribution(
    {0}, {1, 2}, "activation-0", "SCATTER", layout0, layout1, tensor0)};

  NativeDependencySpec gather(
    {"S1R0", "S1R1"}, {"S2R0"}, "boundary-1", "/activation",
    "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}");
  gather.expectedSegments = 0;
  gather.tensors = {"activation-1"};
  gather.useNdnsfDataV1 = true;
  gather.collectiveOperationIndex = 1;
  gather.collectiveSourceLayoutDigest = layout1;
  gather.collectiveTargetLayoutDigest = layout2;
  gather.collectiveTensorDigest = tensor1;
  gather.redistributions = {redistribution(
    {1, 2}, {3}, "activation-1", "GATHER", layout1, layout2, tensor1)};

  NativeExecutionPlan plan;
  plan.serviceName = serviceName.toUri();
  plan.modelName = "d2h-121-native-e2e";
  plan.executionPolicy = "DATA_DRIVEN_V2";
  plan.roles = {"S0R0", "S1R0", "S1R1", "S2R0"};
  plan.dependencies = {scatter, gather};

  GroupOperationV1 scatterOperation;
  scatterOperation.operationIndex = 0;
  scatterOperation.kind = "SCATTER";
  scatterOperation.producerRanks = {"0"};
  scatterOperation.consumerRanks = {"0", "1"};
  scatterOperation.tensorLayoutDigest = layout1;
  scatterOperation.maxBytes = 64U * 1024U;
  scatterOperation.maxSegments = 16;
  GroupOperationV1 gatherOperation;
  gatherOperation.operationIndex = 1;
  gatherOperation.kind = "GATHER";
  gatherOperation.producerRanks = {"0", "1"};
  gatherOperation.consumerRanks = {"1"};
  gatherOperation.tensorLayoutDigest = layout2;
  gatherOperation.maxBytes = 64U * 1024U;
  gatherOperation.maxSegments = 16;
  ProviderGroupCoordinator capabilitySealer(makeD2bCoordinatorOptions());
  const auto capability = capabilitySealer.createCapability(
    requestId.toUri(), "attempt-1", planDigest, "group-d2h-121", 1,
    {{providerNames[0].toUri(), 0, "offer-p0", providerNames[0].toUri()},
     {providerNames[1].toUri(), 1, "offer-p1", providerNames[1].toUri()}},
    {scatterOperation, gatherOperation}, 128U * 1024U, 3000, 12000);
  const auto capabilityHex = bytesToHex(
    ProviderGroupCoordinator::encodeCapability(capability));

  auto observedMutex = std::make_shared<std::mutex>();
  auto observedRoles = std::make_shared<std::set<std::string>>();
  auto runnerFactory = makeHybridRedistributionRunnerFactory(
    observedMutex, observedRoles);
  std::array<std::atomic<bool>, 2> handlerEntered{};
  std::array<std::atomic<size_t>, 2> coordinatorFactoryCalls{};
  std::atomic<size_t> responsePublications{0};
  std::atomic<bool> timedOut{false};
  std::atomic<bool> responseCallback{false};
  std::string responseText;
  std::mutex responseMutex;

  const auto artifactDigest = [] (size_t provider) {
    return "sha256:" + std::string(64, provider == 0 ? '7' : '8');
  };
  for (size_t provider = 0; provider < environment.providerCount(); ++provider) {
    auto& serviceProvider = environment.provider(provider);
    serviceProvider.setUseTokens(false);
    const auto responseKey = serviceProvider.prepareHybridSendKeyForTest(
      serviceName, "RESPONSE");
    environment.user().cacheHybridReceiveKeyForTest(
      responseKey.keyId, responseKey.epochId, responseKey.key);
    serviceProvider.markHybridResponseKeyWrappedForTest(serviceName);
    std::vector<NativeModelRunnerSpec> runnerSpecs;
    for (const auto& role : localRoles[provider]) {
      NativeModelRunnerSpec spec;
      spec.role = role;
      spec.kind = "hybrid-test";
      spec.backend = "onnxruntime";
      spec.path = "/integration-test/d2h-121";
      spec.metadata["test.providerName"] = providerNames[provider].toUri();
      spec.metadata["test.providerBootId"] =
        "d2h-121-boot-" + std::to_string(provider);
      spec.metadata["test.planDigest"] = planDigest;
      spec.metadata["test.artifactDigest"] = artifactDigest(provider);
      runnerSpecs.push_back(std::move(spec));
    }
    NativeProviderHandlerConfig config;
    config.plan = plan;
    config.assignment = assignment;
    config.runnerFactory = runnerFactory;
    config.runnerSpecs = runnerSpecs;
    config.finalResponseScope = "final-response";
    config.localProviderName = providerNames[provider].toUri();
    config.providerBootId = "d2h-121-boot-" + std::to_string(provider);
    config.planDigest = planDigest;
    config.fetchTimeoutMs = 5000;
    config.maxSegmentSize = 4096;
    config.freshnessMs = 60000;
    config.allowPreassembledV3Compatibility = true;
    config.groupCoordinatorFactory =
      [&, provider, localProvider = providerNames[provider].toUri()] (
          ServiceProvider::CollaborationContext& context,
          const std::map<std::string, std::string>& fields) {
        ++coordinatorFactoryCalls[provider];
        auto decoded = ProviderGroupCoordinator::decodeCapability(
          bytesFromHex(fields.at("groupCapabilityV1")));
        if (decoded.requestId != context.sessionId() ||
            decoded.planDigest != planDigest) {
          throw std::runtime_error("D2h group capability binding mismatch");
        }
        auto options = makeD2bCoordinatorOptions();
        options.localProvider = localProvider;
        auto coordinator = std::make_shared<ProviderGroupCoordinator>(
          std::move(options));
        coordinator->installCapability(std::move(decoded), {}, true);
        return coordinator;
      };
    auto runtime = makeNativeProviderCollaborationRuntime(std::move(config));
    auto nativeHandler = std::move(runtime.handler);
    serviceProvider.addCollaborationHandler(
      serviceName,
      [&, provider, nativeHandler = std::move(nativeHandler)] (
          ServiceProvider::CollaborationContext& context,
          const RequestMessage& request) mutable {
        handlerEntered[provider] = true;
        nativeHandler(context, request);
      });
  }

  environment.enableProductionIngressForTest();
  environment.user().setUseTokens(false);
  const auto assignmentKey = environment.user().prepareHybridSendKeyForTest(
    serviceName, "REQUEST-LARGE");
  for (size_t provider = 0; provider < environment.providerCount(); ++provider) {
    environment.provider(provider).cacheHybridReceiveKeyForTest(
      assignmentKey.keyId, assignmentKey.epochId, assignmentKey.key);
  }
  environment.userPubSub().subscribeToProducer(
    provider0Prefix,
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      const auto response = parseResponseNameV2(publication.name);
      if (response && response->serviceName.equals(serviceName) &&
          response->requestId.equals(requestId)) {
        ++responsePublications;
      }
    },
    true);
  environment.userPubSub().subscribeToProducer(
    provider1Prefix,
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      const auto response = parseResponseNameV2(publication.name);
      if (response && response->serviceName.equals(serviceName) &&
          response->requestId.equals(requestId)) {
        ++responsePublications;
      }
    },
    true);

  environment.user().setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name& requestName,
         const std::vector<ndn::Name>&, const ndn::Name&,
         const RequestMessage& request, size_t) {
      const auto requestBlock = request.WireEncode();
      const auto encrypted = makeTestHybridPublication(
        requestName, serviceName, requestId, requesterName, "REQUEST",
        ndn::Buffer(requestBlock.data(), requestBlock.size()));
      for (size_t provider = 0; provider < environment.providerCount(); ++provider) {
        environment.provider(provider).cacheHybridReceiveKeyForTest(
          encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
      }
      environment.userPubSub().publish(
        requestName,
        ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
    });

  const auto redistributionJson = [&] (
      const std::string& producers,
      const std::string& consumers,
      const std::string& tensor,
      const std::string& operation,
      const std::string& sourceLayout,
      const std::string& targetLayout,
      const std::string& integrity) {
    return std::string("{\"producerRanks\":[") + producers +
      "],\"consumerRanks\":[" + consumers + "],\"tensor\":\"" + tensor +
      "\",\"operation\":\"" + operation +
      "\",\"epoch\":\"epoch-1\",\"integrityDigest\":\"" + integrity +
      "\",\"sourceLayoutDigest\":\"" + sourceLayout +
      "\",\"targetLayoutDigest\":\"" + targetLayout +
      "\",\"axis\":1,\"temporaryMemoryBytes\":65536," +
      "\"completeOutput\":true}";
  };
  const auto dependenciesJson =
    std::string("[{\"consumers\":[\"S1R0\",\"S1R1\"],") +
    "\"expected_segments\":0,\"key_scope\":\"boundary-0\"," +
    "\"object_name_template\":\"{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}\"," +
    "\"producers\":[\"S0R0\"],\"required\":true," +
    "\"tensors\":[\"activation-0\"],\"topic_prefix\":\"/activation\"," +
    "\"transportProfile\":\"NDNSF_DATA_V1\"," +
    "\"collectiveOperationIndex\":0,\"collectiveProducerRank\":\"0\"," +
    "\"collectiveSourceLayoutDigest\":\"" + layout0 +
    "\",\"collectiveTargetLayoutDigest\":\"" + layout1 +
    "\",\"collectiveTensorDigest\":\"" + tensor0 +
    "\",\"redistributions\":[" + redistributionJson(
      "0", "1,2", "activation-0", "SCATTER", layout0, layout1, tensor0) +
    "]},{\"consumers\":[\"S2R0\"],\"expected_segments\":0," +
    "\"key_scope\":\"boundary-1\"," +
    "\"object_name_template\":\"{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}\"," +
    "\"producers\":[\"S1R0\",\"S1R1\"],\"required\":true," +
    "\"tensors\":[\"activation-1\"],\"topic_prefix\":\"/activation\"," +
    "\"transportProfile\":\"NDNSF_DATA_V1\"," +
    "\"collectiveOperationIndex\":1,\"collectiveProducerRank\":\"0\"," +
    "\"collectiveSourceLayoutDigest\":\"" + layout1 +
    "\",\"collectiveTargetLayoutDigest\":\"" + layout2 +
    "\",\"collectiveTensorDigest\":\"" + tensor1 +
    "\",\"redistributions\":[" + redistributionJson(
      "1,2", "3", "activation-1", "GATHER", layout1, layout2, tensor1) + "]}]";

  const auto providerPrefixForRole = [&] (const std::string& role) {
    return (role == "S0R0" || role == "S1R0") ?
      providerNames[0].toUri() : providerNames[1].toUri();
  };
  const auto endpointFor = [&] (const std::string& producerRole,
                               std::uint64_t producerRank,
                               const std::string& consumerRole,
                               const std::string& consumerRoles,
                               const std::string& tensorId,
                               const std::string& tensorDigest,
                               const std::string& sourceLayout,
                               const std::string& targetLayout,
                               const std::string& operation,
                               char endpointTag) {
    const auto endpointDigest = "sha256:" + std::string(64, endpointTag);
    const auto manifestDigest = "sha256:" + std::string(64, endpointTag + 1);
    return makeV3TensorEndpointJson(
      providerPrefixForRole(producerRole), requesterName.toUri(),
      requestId.toUri(), planDigest,
      operation == "SCATTER" ? "boundary-0" : "boundary-1",
      operation == "SCATTER" ? 0 : 1, producerRole, producerRank,
      consumerRole, consumerRoles, tensorId, tensorDigest, sourceLayout,
      targetLayout, operation, endpointDigest, manifestDigest);
  };
  const auto makeDataflow = [&] (const std::string& role,
                                 const std::string& publish,
                                 const std::string& fetch,
                                 bool terminal) {
    return std::string("{\"attempt\":1,\"dataflow_digest\":\"") +
      planDigest + "\",\"may_publish\":[" + publish +
      "],\"must_fetch\":[" + fetch + "] ,\"plan_digest\":\"" +
      planDigest + "\",\"request_id\":\"" + requestId.toUri() +
      "\",\"role\":\"" + role + "\",\"terminal_response_owner\":" +
      (terminal ? "true" : "false") + ",\"wait_for\":[]}";
  };
  const auto makeSelectionAssignment = [&] (size_t provider) {
      const auto projectedCapability = capability.projectForProvider(
        providerNames[provider].toUri());
      const auto selectionCapabilityHex = bytesToHex(
        ProviderGroupCoordinator::encodeCapability(projectedCapability));
      std::vector<ndn::Buffer> assignmentItems;
      for (const auto& role : localRoles[provider]) {
        const auto roleRank = role.size() >= 2 &&
            role.substr(role.size() - 2) == "R1" ? 1 : 0;
        const auto roleJson = makeV3SelectionRoleJson(
          role, roleRank, artifactDigest(provider), artifactDigest(provider),
          "onnxruntime", "cpu:0", "TENSOR_RANK");
        const auto scatter0 = endpointFor(
          "S0R0", 0, "S1R0", "\"S1R0\",\"S1R1\"", "activation-0",
          tensor0, layout0, layout1, "SCATTER", 'a');
      const auto scatter1 = endpointFor(
        "S0R0", 0, "S1R1", "\"S1R0\",\"S1R1\"", "activation-0",
        tensor0, layout0, layout1, "SCATTER", 'a');
        const auto gather0 = endpointFor(
          "S1R0", 0, "S2R0", "\"S2R0\"", "activation-1", tensor1,
          layout1, layout2, "GATHER", 'c');
        const auto gather1 = endpointFor(
          "S1R1", 1, "S2R0", "\"S2R0\"", "activation-1", tensor1,
          layout1, layout2, "GATHER", 'd');
        std::string publish;
        std::string fetch;
        bool terminal = false;
        if (role == "S0R0") {
          publish = scatter0;
        }
        else if (role == "S1R0") {
          fetch = scatter0;
          publish = gather0;
        }
        else if (role == "S1R1") {
          fetch = scatter1;
          publish = gather1;
        }
        else if (role == "S2R0") {
          fetch = gather0 + "," + gather1;
          terminal = true;
        }
        const auto dataflow = makeDataflow(role, publish, fetch, terminal);
        const auto text = makeV3SelectionProjectionJson(
          roleJson, role, role, roleRank, providerNames[provider].toUri(),
          requestId.toUri(), planDigest, selectionCapabilityHex,
          dependenciesJson, role.rfind("S0R", 0) == 0
          ? withApplicationInput(dataflow, requesterName.toUri(), requestId.toUri(),
                                 planDigest, role) : dataflow,
        artifactDigest(provider), "cpu:0");
        CollaborationAssignmentEnvelope envelope;
        envelope.role = role;
        envelope.assignedArtifact = ndn::Name("/artifact").append(role);
        envelope.opaquePayload = ndn::Buffer(
          reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
        assignmentItems.push_back(
          encodeCollaborationAssignmentEnvelope(envelope));
      }
      return encodeOpaqueAssignmentSet(assignmentItems);
  };
  const std::array<ndn::Buffer, 2> selectionAssignments{{
    makeSelectionAssignment(0), makeSelectionAssignment(1)}};
  RequestMessage request;
  const std::string requestPayloadText = "d2h-121";
  ndn::Buffer requestPayload(
    reinterpret_cast<const std::uint8_t*>(requestPayloadText.data()),
    requestPayloadText.size());
  request.setPayload(requestPayload, requestPayload.size());
  request.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());
  environment.user().RequestService(
    std::vector<ndn::Name>{providerNames[0], providerNames[1]},
    serviceName,
    request,
    200,
    ServiceUser::AckCandidatesHandler(
      [&] (const std::vector<AckSelectionCandidate>& candidates) {
        if (candidates.size() == 2) {
          for (size_t provider = 0; provider < providerNames.size(); ++provider) {
            BOOST_REQUIRE(
              environment.user().setSelectionAssignmentPayloadForRequest(
                requestId, providerNames[provider],
                selectionAssignments[provider]));
          }
        }
        return candidates;
      }),
    12000,
    [&] (const ndn::Name&) { timedOut = true; },
    [&] (const ResponseMessage& response) {
      const auto& payload = response.getPayload();
      std::lock_guard<std::mutex> lock(responseMutex);
      responseText.assign(payload.begin(), payload.end());
      responseCallback = true;
    },
    tlv::FirstResponding,
    requestId);

  environment.pumpUntil([&] {
    return (environment.provider(0).getPendingRequestCountForTesting() == 1 &&
            environment.provider(1).getPendingRequestCountForTesting() == 1) ||
           timedOut;
  });
  for (size_t provider = 0; provider < providerNames.size(); ++provider) {
    RequestAckMessage ack;
    ack.setStatus(true);
    ack.setMessage("d2h-121-ack-" + std::to_string(provider));
    const auto ackName = makeRequestAckNameV2(
      providerNames[provider], requesterName, serviceName, requestId);
    const auto ackBlock = ack.WireEncode();
    const auto encrypted = makeTestHybridPublication(
      ackName, serviceName, requestId, providerNames[provider], "ACK",
      ndn::Buffer(ackBlock.data(), ackBlock.size()));
    environment.user().cacheHybridReceiveKeyForTest(
      encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
    environment.providerPubSub(provider).publish(
      ackName,
      ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
  }

  environment.pumpUntil([&] { return responseCallback || timedOut; });
  BOOST_CHECK(handlerEntered[0]);
  BOOST_CHECK(handlerEntered[1]);
  BOOST_CHECK_EQUAL(coordinatorFactoryCalls[0].load(), 2U);
  BOOST_CHECK_EQUAL(coordinatorFactoryCalls[1].load(), 2U);
  BOOST_CHECK(responseCallback);
  BOOST_CHECK(!timedOut);
  {
    std::lock_guard<std::mutex> lock(responseMutex);
    BOOST_CHECK_EQUAL(responseText, "oracle:10.000000");
  }
  {
    std::lock_guard<std::mutex> lock(*observedMutex);
    BOOST_CHECK_EQUAL(observedRoles->size(), 4U);
    for (const auto& role : plan.roles) {
      BOOST_CHECK(observedRoles->count(role) != 0);
    }
  }
  BOOST_CHECK_EQUAL(responsePublications.load(), 1U);
}

BOOST_AUTO_TEST_CASE(ProductionNativeHandlersRunD2h212ToCompleteOracleResponse)
{
  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/D2h212NativeE2e");
  profile.providerCount = 2;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  std::vector<ndn::signal::ScopedConnection> providerPeerBridges;
  for (size_t source = 0; source < environment.providerCount(); ++source) {
    for (size_t destination = 0; destination < environment.providerCount(); ++destination) {
      if (source == destination) {
        continue;
      }
      providerPeerBridges.emplace_back(
        environment.providerFace(source).onSendInterest.connect(
          [&environment, destination] (const ndn::Interest& interest) {
            environment.providerFace(destination).receive(interest);
          }));
      providerPeerBridges.emplace_back(
        environment.providerFace(source).onSendData.connect(
          [&environment, destination] (const ndn::Data& data) {
            environment.providerFace(destination).receive(data);
          }));
    }
  }

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  const std::array<ndn::Name, 2> providerNames{{
    environment.provider(0).getName(), environment.provider(1).getName(),
  }};
  auto provider0Prefix = environment.profile().providerNode;
  provider0Prefix.append("0");
  auto provider1Prefix = environment.profile().providerNode;
  provider1Prefix.append("p1").append("0");
  const ndn::Name requestId("/d2h-212-native-e2e");
  const auto planDigest = "sha256:" + std::string(64, 'a');
  const auto layout0 = "sha256:" + std::string(64, 'b');
  const auto layout1 = "sha256:" + std::string(64, 'c');
  const auto layout2 = "sha256:" + std::string(64, 'd');
  const auto tensor0 = "sha256:" + std::string(64, 'e');
  const auto tensor1 = "sha256:" + std::string(64, 'f');
  const auto tensor2 = "sha256:" + std::string(64, '9');

  const std::array<std::vector<std::string>, 2> localRoles{{
    {"S0R0", "S1R0", "S2R0"}, {"S0R1", "S2R1"},
  }};
  // See the D2h 1-2-1 case above: this retained compatibility topology
  // colocates dependent rank roles and therefore needs one worker per local
  // role.  Request-scoped one-role-per-Provider plans do not need this.
  for (size_t provider = 0; provider < localRoles.size(); ++provider) {
    environment.provider(provider).setHandlerThreads(
      localRoles[provider].size());
  }
  NativeProviderAssignment assignment;
  for (size_t provider = 0; provider < localRoles.size(); ++provider) {
    for (const auto& role : localRoles[provider]) {
      assignment.providerByRole[role] = providerNames[provider].toUri();
    }
  }

  auto makeRedistribution = [&] (std::vector<std::uint64_t> producers,
                                  std::vector<std::uint64_t> consumers,
                                  std::string tensor,
                                  std::string operation,
                                  std::string sourceLayout,
                                  std::string targetLayout,
                                  std::string integrity) {
    RedistributionSpec value;
    value.producerRanks = std::move(producers);
    value.consumerRanks = std::move(consumers);
    value.tensor = std::move(tensor);
    value.operation = std::move(operation);
    value.epoch = "epoch-1";
    value.integrityDigest = std::move(integrity);
    value.sourceLayoutDigest = std::move(sourceLayout);
    value.targetLayoutDigest = std::move(targetLayout);
    value.axis = 1;
    value.temporaryMemoryBytes = 64U * 1024U;
    value.completeOutput = true;
    return value;
  };

  NativeDependencySpec gather(
    {"S0R0", "S0R1"}, {"S1R0"}, "boundary-0", "/activation",
    "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}");
  gather.expectedSegments = 0;
  gather.tensors = {"activation-0"};
  gather.useNdnsfDataV1 = true;
  gather.collectiveOperationIndex = 0;
  gather.collectiveSourceLayoutDigest = layout0;
  gather.collectiveTargetLayoutDigest = layout1;
  gather.collectiveTensorDigest = tensor0;
  gather.redistributions = {makeRedistribution(
    {0, 1}, {2}, "activation-0", "GATHER", layout0, layout1, tensor0)};

  NativeDependencySpec scatter(
    {"S1R0"}, {"S2R0", "S2R1"}, "boundary-1", "/activation",
    "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}");
  scatter.expectedSegments = 0;
  scatter.tensors = {"activation-1"};
  scatter.useNdnsfDataV1 = true;
  scatter.collectiveOperationIndex = 1;
  scatter.collectiveSourceLayoutDigest = layout1;
  scatter.collectiveTargetLayoutDigest = layout2;
  scatter.collectiveTensorDigest = tensor1;
  scatter.redistributions = {makeRedistribution(
    {2}, {3, 4}, "activation-1", "SCATTER", layout1, layout2, tensor1)};

  NativeDependencySpec finalMerge(
    {"S2R1"}, {"S2R0"}, "boundary-2", "/partial",
    "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}");
  finalMerge.expectedSegments = 0;
  finalMerge.tensors = {"partial-sum"};
  finalMerge.useNdnsfDataV1 = true;
  finalMerge.collectiveOperationIndex = 2;
  finalMerge.collectiveSourceLayoutDigest = layout2;
  finalMerge.collectiveTargetLayoutDigest = layout2;
  finalMerge.collectiveTensorDigest = tensor2;

  NativeExecutionPlan plan;
  plan.serviceName = serviceName.toUri();
  plan.modelName = "d2h-212-native-e2e";
  plan.executionPolicy = "DATA_DRIVEN_V2";
  plan.roles = {"S0R0", "S0R1", "S1R0", "S2R0", "S2R1"};
  plan.dependencies = {gather, scatter, finalMerge};

  auto operation = [] (std::uint64_t index,
                         std::string kind,
                         std::vector<std::string> producers,
                         std::vector<std::string> consumers,
                         const std::string& layout) {
    GroupOperationV1 value;
    value.operationIndex = index;
    value.kind = std::move(kind);
    value.producerRanks = std::move(producers);
    value.consumerRanks = std::move(consumers);
    value.tensorLayoutDigest = layout;
    value.maxBytes = 64U * 1024U;
    value.maxSegments = 16;
    return value;
  };
  const std::vector<GroupOperationV1> operations{
    operation(0, "GATHER", {"0", "1"}, {"0"}, layout1),
    operation(1, "SCATTER", {"0"}, {"0", "1"}, layout2),
    operation(2, "PIPELINE_TRANSFER", {"1"}, {"0"}, layout2),
  };
  ProviderGroupCoordinator capabilitySealer(makeD2bCoordinatorOptions());
  const auto capability = capabilitySealer.createCapability(
    requestId.toUri(), "attempt-1", planDigest, "group-d2h-212", 1,
    {{providerNames[0].toUri(), 0, "offer-p0", providerNames[0].toUri()},
     {providerNames[1].toUri(), 1, "offer-p1", providerNames[1].toUri()}},
    operations, 192U * 1024U, 3000, 12000);
  const auto capabilityHex = bytesToHex(
    ProviderGroupCoordinator::encodeCapability(capability));

  auto observedMutex = std::make_shared<std::mutex>();
  auto observedRoles = std::make_shared<std::set<std::string>>();
  auto runnerFactory = makeHybrid212RunnerFactory(observedMutex, observedRoles);
  std::array<std::atomic<bool>, 2> handlerEntered{};
  std::array<std::atomic<size_t>, 2> coordinatorFactoryCalls{};
  std::atomic<size_t> responsePublications{0};
  std::atomic<bool> timedOut{false};
  std::atomic<bool> responseCallback{false};
  std::string responseText;
  std::mutex responseMutex;

  const auto artifactDigest = [] (size_t provider) {
    return "sha256:" + std::string(64, provider == 0 ? '7' : '8');
  };
  for (size_t provider = 0; provider < environment.providerCount(); ++provider) {
    auto& serviceProvider = environment.provider(provider);
    serviceProvider.setUseTokens(false);
    const auto responseKey = serviceProvider.prepareHybridSendKeyForTest(
      serviceName, "RESPONSE");
    environment.user().cacheHybridReceiveKeyForTest(
      responseKey.keyId, responseKey.epochId, responseKey.key);
    serviceProvider.markHybridResponseKeyWrappedForTest(serviceName);
    std::vector<NativeModelRunnerSpec> runnerSpecs;
    for (const auto& role : localRoles[provider]) {
      NativeModelRunnerSpec spec;
      spec.role = role;
      spec.kind = "hybrid-212-test";
      spec.backend = "onnxruntime";
      spec.path = "/integration-test/d2h-212";
      spec.metadata["test.providerName"] = providerNames[provider].toUri();
      spec.metadata["test.providerBootId"] =
        "d2h-212-boot-" + std::to_string(provider);
      spec.metadata["test.planDigest"] = planDigest;
      spec.metadata["test.artifactDigest"] = artifactDigest(provider);
      runnerSpecs.push_back(std::move(spec));
    }
    NativeProviderHandlerConfig config;
    config.plan = plan;
    config.assignment = assignment;
    config.runnerFactory = runnerFactory;
    config.runnerSpecs = runnerSpecs;
    config.finalResponseScope = "final-response";
    config.localProviderName = providerNames[provider].toUri();
    config.providerBootId = "d2h-212-boot-" + std::to_string(provider);
    config.planDigest = planDigest;
    config.fetchTimeoutMs = 5000;
    config.maxSegmentSize = 4096;
    config.freshnessMs = 60000;
    config.allowPreassembledV3Compatibility = true;
    config.groupCoordinatorFactory =
      [&, provider, localProvider = providerNames[provider].toUri()] (
          ServiceProvider::CollaborationContext& context,
          const std::map<std::string, std::string>& fields) {
        ++coordinatorFactoryCalls[provider];
        auto decoded = ProviderGroupCoordinator::decodeCapability(
          bytesFromHex(fields.at("groupCapabilityV1")));
        if (decoded.requestId != context.sessionId() ||
            decoded.planDigest != planDigest) {
          throw std::runtime_error("D2h 212 group capability binding mismatch");
        }
        auto options = makeD2bCoordinatorOptions();
        options.localProvider = localProvider;
        auto coordinator = std::make_shared<ProviderGroupCoordinator>(
          std::move(options));
        coordinator->installCapability(std::move(decoded), {}, true);
        return coordinator;
      };
    auto runtime = makeNativeProviderCollaborationRuntime(std::move(config));
    auto nativeHandler = std::move(runtime.handler);
    serviceProvider.addCollaborationHandler(
      serviceName,
      [&, provider, nativeHandler = std::move(nativeHandler)] (
          ServiceProvider::CollaborationContext& context,
          const RequestMessage& request) mutable {
        handlerEntered[provider] = true;
        nativeHandler(context, request);
      });
  }

  environment.enableProductionIngressForTest();
  environment.user().setUseTokens(false);
  const auto assignmentKey = environment.user().prepareHybridSendKeyForTest(
    serviceName, "REQUEST-LARGE");
  for (size_t provider = 0; provider < environment.providerCount(); ++provider) {
    environment.provider(provider).cacheHybridReceiveKeyForTest(
      assignmentKey.keyId, assignmentKey.epochId, assignmentKey.key);
  }
  for (const auto& prefix : {provider0Prefix, provider1Prefix}) {
    environment.userPubSub().subscribeToProducer(
      prefix,
      [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        const auto response = parseResponseNameV2(publication.name);
        if (response && response->serviceName.equals(serviceName) &&
            response->requestId.equals(requestId)) {
          ++responsePublications;
        }
      },
      true);
  }
  environment.user().setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name& requestName,
         const std::vector<ndn::Name>&, const ndn::Name&,
         const RequestMessage& request, size_t) {
      const auto requestBlock = request.WireEncode();
      const auto encrypted = makeTestHybridPublication(
        requestName, serviceName, requestId, requesterName, "REQUEST",
        ndn::Buffer(requestBlock.data(), requestBlock.size()));
      for (size_t provider = 0; provider < environment.providerCount(); ++provider) {
        environment.provider(provider).cacheHybridReceiveKeyForTest(
          encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
      }
      environment.userPubSub().publish(
        requestName,
        ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
    });

  const auto redistributionJson = [&] (
      const std::string& producers,
      const std::string& consumers,
      const std::string& tensor,
      const std::string& operationName,
      const std::string& sourceLayout,
      const std::string& targetLayout,
      const std::string& integrity) {
    return std::string("{\"producerRanks\":[") + producers +
      "],\"consumerRanks\":[" + consumers + "],\"tensor\":\"" + tensor +
      "\",\"operation\":\"" + operationName +
      "\",\"epoch\":\"epoch-1\",\"integrityDigest\":\"" + integrity +
      "\",\"sourceLayoutDigest\":\"" + sourceLayout +
      "\",\"targetLayoutDigest\":\"" + targetLayout +
      "\",\"axis\":1,\"temporaryMemoryBytes\":65536," +
      "\"completeOutput\":true}";
  };
  const auto dependencyJson = [&] (
      const std::string& producers,
      const std::string& consumers,
      const std::string& scope,
      const std::string& tensor,
      std::uint64_t operationIndex,
      const std::string& sourceLayout,
      const std::string& targetLayout,
      const std::string& tensorDigest,
      const std::string& redistributions = {}) {
    return std::string("{\"consumers\":[") + consumers +
      "],\"expected_segments\":0,\"key_scope\":\"" + scope +
      "\",\"object_name_template\":\"{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}\"," +
      "\"producers\":[" + producers + "],\"required\":true," +
      "\"tensors\":[\"" + tensor + "\"],\"topic_prefix\":\"/activation\"," +
      "\"transportProfile\":\"NDNSF_DATA_V1\"," +
      "\"collectiveOperationIndex\":" + std::to_string(operationIndex) +
      ",\"collectiveProducerRank\":\"0\"," +
      "\"collectiveSourceLayoutDigest\":\"" + sourceLayout +
      "\",\"collectiveTargetLayoutDigest\":\"" + targetLayout +
      "\",\"collectiveTensorDigest\":\"" + tensorDigest + "\"" +
      (redistributions.empty() ? std::string() :
        ",\"redistributions\":[" + redistributions + "]") + "}";
  };
  const auto dependenciesJson = std::string("[") +
    dependencyJson(
      "\"S0R0\",\"S0R1\"", "\"S1R0\"", "boundary-0", "activation-0", 0,
      layout0, layout1, tensor0,
      redistributionJson("0,1", "2", "activation-0", "GATHER",
                         layout0, layout1, tensor0)) + "," +
    dependencyJson(
      "\"S1R0\"", "\"S2R0\",\"S2R1\"", "boundary-1", "activation-1", 1,
      layout1, layout2, tensor1,
      redistributionJson("2", "3,4", "activation-1", "SCATTER",
                         layout1, layout2, tensor1)) + "," +
    dependencyJson(
      "\"S2R1\"", "\"S2R0\"", "boundary-2", "partial-sum", 2,
      layout2, layout2, tensor2) + "]";

  const auto providerPrefixForRole = [&] (const std::string& role) {
    return (role == "S0R0" || role == "S1R0" || role == "S2R0") ?
      providerNames[0].toUri() : providerNames[1].toUri();
  };
  const auto endpointFor = [&] (const std::string& producerRole,
                               std::uint64_t producerRank,
                               const std::string& consumerRole,
                               const std::string& consumerRoles,
                               const std::string& groupId,
                               std::uint64_t round,
                               const std::string& tensorId,
                               const std::string& tensorDigest,
                               const std::string& sourceLayout,
                               const std::string& targetLayout,
                               const std::string& operationName,
                               char endpointTag) {
    const auto endpointDigest = "sha256:" + std::string(64, endpointTag);
    const auto manifestDigest = "sha256:" + std::string(64, endpointTag + 1);
    return makeV3TensorEndpointJson(
      providerPrefixForRole(producerRole), requesterName.toUri(),
      requestId.toUri(), planDigest, groupId, round, producerRole,
      producerRank, consumerRole, consumerRoles, tensorId, tensorDigest,
      sourceLayout, targetLayout, operationName, endpointDigest,
      manifestDigest);
  };
  const auto makeDataflow = [&] (const std::string& role,
                                 const std::string& publish,
                                 const std::string& fetch,
                                 bool terminal) {
    return std::string("{\"attempt\":1,\"dataflow_digest\":\"") +
      planDigest + "\",\"may_publish\":[" + publish +
      "],\"must_fetch\":[" + fetch + "],\"plan_digest\":\"" +
      planDigest + "\",\"request_id\":\"" + requestId.toUri() +
      "\",\"role\":\"" + role + "\",\"terminal_response_owner\":" +
      (terminal ? "true" : "false") + ",\"wait_for\":[]}";
  };
  const auto makeSelectionAssignment = [&] (size_t provider) {
    const auto projectedCapability = capability.projectForProvider(
      providerNames[provider].toUri());
    const auto selectionCapabilityHex = bytesToHex(
      ProviderGroupCoordinator::encodeCapability(projectedCapability));
    std::vector<ndn::Buffer> assignmentItems;
    for (const auto& role : localRoles[provider]) {
      const auto roleRank = role.size() >= 2 && role.substr(role.size() - 2) == "R1"
        ? 1 : 0;
      const auto roleJson = makeV3SelectionRoleJson(
        role, roleRank, artifactDigest(provider), artifactDigest(provider),
        "onnxruntime", "cpu:0", "TENSOR_RANK");
      const auto gather0 = endpointFor(
        "S0R0", 0, "S1R0", "\"S1R0\"", "boundary-0", 0,
        "activation-0", tensor0, layout0, layout1, "GATHER", 'a');
      const auto gather1 = endpointFor(
        "S0R1", 1, "S1R0", "\"S1R0\"", "boundary-0", 0,
        "activation-0", tensor0, layout0, layout1, "GATHER", 'b');
      const auto scatter0 = endpointFor(
        "S1R0", 0, "S2R0", "\"S2R0\",\"S2R1\"", "boundary-1", 1,
        "activation-1", tensor1, layout1, layout2, "SCATTER", 'c');
      const auto scatter1 = endpointFor(
        "S1R0", 0, "S2R1", "\"S2R0\",\"S2R1\"", "boundary-1", 1,
        "activation-1", tensor1, layout1, layout2, "SCATTER", 'c');
      const auto finalMerge = endpointFor(
        "S2R1", 1, "S2R0", "\"S2R0\"", "boundary-2", 2,
        "partial-sum", tensor2, layout2, layout2, "PIPELINE_TRANSFER", 'e');
      std::string publish;
      std::string fetch;
      bool terminal = false;
      if (role == "S0R0") {
        publish = gather0;
      }
      else if (role == "S0R1") {
        publish = gather1;
      }
      else if (role == "S1R0") {
        fetch = gather0 + "," + gather1;
        publish = scatter0;
      }
      else if (role == "S2R1") {
        fetch = scatter1;
        publish = finalMerge;
      }
      else if (role == "S2R0") {
        fetch = scatter0 + "," + finalMerge;
        terminal = true;
      }
      const auto dataflow = makeDataflow(role, publish, fetch, terminal);
      const auto text = makeV3SelectionProjectionJson(
        roleJson, role, role, roleRank, providerNames[provider].toUri(),
        requestId.toUri(), planDigest, selectionCapabilityHex,
        dependenciesJson, role.rfind("S0R", 0) == 0
          ? withApplicationInput(dataflow, requesterName.toUri(), requestId.toUri(),
                                 planDigest, role) : dataflow,
        artifactDigest(provider), "cpu:0");
      CollaborationAssignmentEnvelope envelope;
      envelope.role = role;
      envelope.assignedArtifact = ndn::Name("/artifact").append(role);
      envelope.opaquePayload = ndn::Buffer(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
      assignmentItems.push_back(encodeCollaborationAssignmentEnvelope(envelope));
    }
    return encodeOpaqueAssignmentSet(assignmentItems);
  };
  const std::array<ndn::Buffer, 2> selectionAssignments{{
    makeSelectionAssignment(0), makeSelectionAssignment(1)}};
  RequestMessage request;
  const std::string requestPayloadText = "d2h-212";
  ndn::Buffer requestPayload(
    reinterpret_cast<const std::uint8_t*>(requestPayloadText.data()),
    requestPayloadText.size());
  request.setPayload(requestPayload, requestPayload.size());
  request.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());
  const auto returnedRequestId = environment.user().RequestService(
    std::vector<ndn::Name>{providerNames[0], providerNames[1]},
    serviceName,
    request,
    200,
    ServiceUser::AckCandidatesHandler(
      [&] (const std::vector<AckSelectionCandidate>& candidates) {
        if (candidates.size() == 2) {
          for (size_t provider = 0; provider < providerNames.size(); ++provider) {
            BOOST_REQUIRE(
              environment.user().setSelectionAssignmentPayloadForRequest(
                requestId, providerNames[provider],
                selectionAssignments[provider]));
          }
        }
        return candidates;
      }),
    12000,
    [&] (const ndn::Name&) { timedOut = true; },
    [&] (const ResponseMessage& response) {
      const auto& payload = response.getPayload();
      std::lock_guard<std::mutex> lock(responseMutex);
      responseText.assign(payload.begin(), payload.end());
      responseCallback = true;
    },
    tlv::FirstResponding,
    requestId);
  BOOST_REQUIRE_EQUAL(returnedRequestId, requestId);

  environment.pumpUntil([&] {
    return (environment.provider(0).getPendingRequestCountForTesting() == 1 &&
            environment.provider(1).getPendingRequestCountForTesting() == 1) ||
           timedOut;
  });
  for (size_t provider = 0; provider < providerNames.size(); ++provider) {
    RequestAckMessage ack;
    ack.setStatus(true);
    ack.setMessage("d2h-212-ack-" + std::to_string(provider));
    const auto ackName = makeRequestAckNameV2(
      providerNames[provider], requesterName, serviceName, requestId);
    const auto ackBlock = ack.WireEncode();
    const auto encrypted = makeTestHybridPublication(
      ackName, serviceName, requestId, providerNames[provider], "ACK",
      ndn::Buffer(ackBlock.data(), ackBlock.size()));
    environment.user().cacheHybridReceiveKeyForTest(
      encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
    environment.providerPubSub(provider).publish(
      ackName,
      ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
  }

  environment.pumpUntil([&] { return responseCallback || timedOut; });

  BOOST_CHECK(handlerEntered[0]);
  BOOST_CHECK(handlerEntered[1]);
  BOOST_CHECK_EQUAL(coordinatorFactoryCalls[0].load(), 3U);
  BOOST_CHECK_EQUAL(coordinatorFactoryCalls[1].load(), 2U);
  BOOST_CHECK(responseCallback);
  BOOST_CHECK(!timedOut);
  {
    std::lock_guard<std::mutex> lock(responseMutex);
    BOOST_CHECK_EQUAL(responseText, "oracle:10.000000");
  }
  {
    std::lock_guard<std::mutex> lock(*observedMutex);
    BOOST_CHECK_EQUAL(observedRoles->size(), 5U);
    for (const auto& role : plan.roles) {
      BOOST_CHECK(observedRoles->count(role) != 0);
    }
  }
  BOOST_CHECK_EQUAL(responsePublications.load(), 1U);
}

/**
 * D2h production-ingress gate for both frozen heterogeneous rank mappings.
 * Each Provider receives only its assigned ranks through Selection, executes
 * those ranks with the native async runtime, and the test reconstructs the
 * complete global role/rank map from the production callback observations.
 * The two-provider DATA_V1 transport is covered independently by D2b; this
 * gate focuses on exact heterogeneous assignment and local execution.
 */
BOOST_AUTO_TEST_CASE(ProductionIngressRunsD2hFrozenHeterogeneousMappings)
{
  using Mapping = std::array<std::vector<std::string>, 2>;
  const std::array<std::pair<const char*, Mapping>, 2> mappings{{
      {"[1,2,1]", Mapping{{{"S0R0", "S1R0"}, {"S1R1", "S2R0"}}}},
      {"[2,1,2]", Mapping{{{"S0R0", "S1R0", "S2R0"}, {"S0R1", "S2R1"}}}},
  }};

  for (const auto& [mappingLabel, mapping] : mappings) {
    const auto mappingCopy = mapping;
    const std::string mappingLabelCopy(mappingLabel);
    test::BootstrapProfile profile;
    profile.serviceName = ndn::Name("/Inference/D2hHybrid");
    profile.providerCount = 2;
    test::NdnsfIntegrationEnvironment environment(profile);
    environment.bootstrap();

    const auto serviceName = environment.profile().serviceName;
    const auto requesterName = environment.user().getName();
    const auto provider0Name = environment.provider(0).getName();
    const auto provider1Name = environment.provider(1).getName();
    const std::array<ndn::Name, 2> providerNames{{provider0Name, provider1Name}};
    const ndn::Name requestId(std::string("/d2h-production-") + mappingLabel);

    std::mutex observedMutex;
    std::map<std::string, std::string> observedRoleProviders;
    std::atomic<size_t> handlerCount{0};
    std::atomic<size_t> runtimeCount{0};
    std::atomic<size_t> runtimeOutputCount{0};
    std::atomic<bool> ackObserved{false};
    std::atomic<bool> timedOut{false};

    environment.user().setUseTokens(false);
    for (size_t index = 0; index < environment.providerCount(); ++index) {
      environment.provider(index).setUseTokens(false);
      environment.provider(index).addCollaborationHandler(
          serviceName,
          [&, index, mappingCopy] (ServiceProvider::CollaborationContext& context,
                      const RequestMessage& request) {
            if (request.getPayload().size() != 11) {
              return;
            }
            const auto& localRoles = mappingCopy[index];
            const auto& roleProviders = context.assignment().roleProviders;
            {
              std::lock_guard<std::mutex> lock(observedMutex);
              for (const auto& role : localRoles) {
                const auto found = roleProviders.find(role);
                if (found == roleProviders.end() ||
                    found->second != providerNames[index]) {
                  return;
                }
                observedRoleProviders[role] = found->second.toUri();
              }
            }

            std::vector<RoleSpec> localSpecs;
            localSpecs.reserve(localRoles.size());
            for (const auto& role : localRoles) {
              const auto outputScope = role + "/output";
              localSpecs.emplace_back(
                  role, std::vector<DependencyEdge>{},
                  std::vector<DependencyEdge>{DependencyEdge{
                      outputScope, role, "", "/d2h/" + outputScope, 1, 4}});
            }
            AsyncDataflowRuntime runtime(2);
            const auto result = runtime.run(
                requestId.toUri(), localSpecs, {},
                [] (const RoleExecutionContext& execution) {
                  const auto outputScope = execution.role + "/output";
                  return std::map<std::string, TensorBundle>{
                      {outputScope, TensorBundle{outputScope, {1, 2, 3, 4}, 1, 4}}};
                });
            runtimeCount.fetch_add(result.roleTimings.size());
            runtimeOutputCount.fetch_add(result.outputsByScope.size());
            handlerCount.fetch_add(1);
          });
    }

    environment.enableProductionIngressForTest();
    environment.user().setRequestPublisher(
        [&] (const ndn::Name&, const ndn::Name& requestName,
             const std::vector<ndn::Name>& providers,
             const ndn::Name& publishedService,
             const RequestMessage& request, size_t strategy) {
          if (providers.size() != 2 || publishedService != serviceName ||
              strategy != tlv::FirstResponding) {
            return;
          }
          const auto requestBlock = request.WireEncode();
          const auto encrypted = makeTestHybridPublication(
              requestName, serviceName, requestId, requesterName, "REQUEST",
              ndn::Buffer(requestBlock.data(), requestBlock.size()));
          for (size_t index = 0; index < environment.providerCount(); ++index) {
            environment.provider(index).cacheHybridReceiveKeyForTest(
                encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
          }
          environment.userPubSub().publish(
              requestName,
              ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
        });

    RequestMessage request;
    const std::string requestText = "d2h-payload";
    ndn::Buffer requestPayload(
        reinterpret_cast<const uint8_t*>(requestText.data()), requestText.size());
    request.setPayload(requestPayload, requestPayload.size());
    request.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());
    const auto returnedRequestId = environment.user().RequestService(
        std::vector<ndn::Name>{provider0Name, provider1Name}, serviceName,
        request, 200,
        ServiceUser::AckCandidatesHandler(
            [&, mappingCopy, mappingLabelCopy] (const std::vector<AckSelectionCandidate>& candidates) {
              ackObserved = true;
              if (candidates.size() != 2) {
                return candidates;
              }
              for (size_t index = 0; index < environment.providerCount(); ++index) {
                std::vector<ndn::Buffer> assignmentItems;
                for (const auto& role : mappingCopy[index]) {
                  CollaborationAssignmentEnvelope assignment;
                  assignment.role = role;
                  assignment.assignedArtifact = ndn::Name("/artifact").append(role);
                  const std::string opaque = std::string("mapping=") +
                                             mappingLabelCopy + ";rank=" + role + ";";
                  assignment.opaquePayload = ndn::Buffer(
                      reinterpret_cast<const uint8_t*>(opaque.data()), opaque.size());
                  assignmentItems.push_back(
                      encodeCollaborationAssignmentEnvelope(assignment));
                }
                BOOST_REQUIRE(environment.user().setSelectionAssignmentPayloadForRequest(
                  requestId, providerNames[index], encodeOpaqueAssignmentSet(assignmentItems)));
              }
              return candidates;
            }),
        2000,
        [&] (const ndn::Name&) { timedOut = true; },
        [&] (const ResponseMessage&) {},
        tlv::FirstResponding,
        requestId);
    BOOST_REQUIRE_EQUAL(returnedRequestId, requestId);

    environment.pumpUntil([&] {
      bool pending = true;
      for (size_t index = 0; index < environment.providerCount(); ++index) {
        pending = pending &&
            environment.provider(index).getPendingRequestCountForTesting() == 1;
      }
      return pending || timedOut;
    });
    for (size_t index = 0; index < environment.providerCount(); ++index) {
      RequestAckMessage ack;
      ack.setStatus(true);
      ack.setMessage("d2h-ack-" + std::to_string(index));
      const auto ackName = makeRequestAckNameV2(
          environment.provider(index).getName(), requesterName,
          serviceName, requestId);
      const auto ackBlock = ack.WireEncode();
      const auto encrypted = makeTestHybridPublication(
          ackName, serviceName, requestId,
          environment.provider(index).getName(), "ACK",
          ndn::Buffer(ackBlock.data(), ackBlock.size()));
      environment.user().cacheHybridReceiveKeyForTest(
          encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
      environment.providerPubSub(index).publish(
          ackName,
          ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
    }

    environment.pumpUntil([&] {
      return handlerCount == environment.providerCount() || timedOut;
    });

    std::map<std::string, std::string> expectedRoleProviders;
    for (size_t index = 0; index < mapping.size(); ++index) {
      for (const auto& role : mapping[index]) {
        expectedRoleProviders.emplace(role, providerNames[index].toUri());
      }
    }
    std::map<std::string, std::string> observed;
    {
      std::lock_guard<std::mutex> lock(observedMutex);
      observed = observedRoleProviders;
    }
    BOOST_CHECK(ackObserved);
    BOOST_CHECK_EQUAL(handlerCount.load(), environment.providerCount());
    BOOST_CHECK_EQUAL(runtimeCount.load(), expectedRoleProviders.size());
    BOOST_CHECK_EQUAL(runtimeOutputCount.load(), expectedRoleProviders.size());
    BOOST_CHECK(observed == expectedRoleProviders);
    BOOST_CHECK(!timedOut);
  }
}

/** Four-Provider production-ingress gate for the D0 role-split workload. */
BOOST_AUTO_TEST_CASE(ProductionIngressRunsFourProviderRoleSplitRequestSelectionResponse)
{
  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/NativeTracer");
  profile.providerCount = 4;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName;
  const auto requesterName = environment.user().getName();
  std::vector<ndn::Name> providerNames;
  providerNames.reserve(environment.providerCount());
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    providerNames.push_back(environment.provider(index).getName());
  }
  const auto requestId = ndn::Name("/production-four-provider-request");

  std::array<std::atomic<bool>, 4> requestObserved{};
  std::array<std::atomic<bool>, 4> handlerCalled{};
  std::array<std::atomic<bool>, 4> responseObserved{};
  std::atomic<bool> ackObserved{false};
  std::atomic<bool> selectionPublished{false};
  std::atomic<bool> timedOut{false};

  environment.user().setUseTokens(false);
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    auto& provider = environment.provider(index);
    provider.setUseTokens(false);
    provider.addCollaborationHandler(
        serviceName,
        [&, index] (ServiceProvider::CollaborationContext& context,
                    const RequestMessage& request) {
          BOOST_CHECK_EQUAL(request.getPayload().size(), 13);
          const auto expectedRole = "role-" + std::to_string(index);
          BOOST_CHECK_EQUAL(context.assignment().role, expectedRole);
          const auto roleIt = context.assignment().roleProviders.find(expectedRole);
          if (roleIt == context.assignment().roleProviders.end()) {
            // Keep this asynchronous assertion failure non-fatal.  The old
            // code dereferenced ``roleIt`` after a failed CHECK and masked
            // the mapping defect with a process-wide segmentation fault.
            BOOST_ERROR("production assignment is missing the Provider role");
            return;
          }
          BOOST_CHECK_EQUAL(roleIt->second, providerNames[index].toUri());
          handlerCalled[index] = true;
        });
  }
  // Service registration must precede production SVS subscription setup.
  environment.enableProductionIngressForTest();

  for (size_t index = 0; index < environment.providerCount(); ++index) {
    environment.providerPubSub(index).subscribeToProducer(
        environment.profile().userNode,
        [&, index] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
          const auto parsed = parseRequestNameV2(publication.name);
          if (parsed && parsed->serviceName.equals(serviceName) &&
              parsed->requestId.equals(requestId)) {
            requestObserved[index] = true;
          }
        },
        true);
  }

  environment.user().setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& requestName,
           const std::vector<ndn::Name>& providers,
           const ndn::Name& publishedServiceName,
           const RequestMessage& request, size_t strategy) {
        BOOST_REQUIRE_EQUAL(providers.size(), providerNames.size());
        BOOST_CHECK_EQUAL(publishedServiceName, serviceName);
        BOOST_CHECK_EQUAL(strategy, tlv::AllSelected);
        const auto requestBlock = request.WireEncode();
        const auto encrypted = makeTestHybridPublication(
            requestName, serviceName, requestId, requesterName, "REQUEST",
            ndn::Buffer(requestBlock.data(), requestBlock.size()));
        for (size_t index = 0; index < environment.providerCount(); ++index) {
          environment.provider(index).cacheHybridReceiveKeyForTest(
              encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
        }
        environment.userPubSub().publish(
            requestName,
            ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
      });

  RequestMessage request;
  const std::string payload = "four-provider";
  ndn::Buffer requestPayload(
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  request.setPayload(requestPayload, requestPayload.size());
  request.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());
  const auto returnedRequestId = environment.user().RequestService(
      providerNames, serviceName, request, 500,
      ServiceUser::AckCandidatesHandler(
          [&] (const std::vector<AckSelectionCandidate>& candidates) {
            ackObserved = true;
            BOOST_REQUIRE_EQUAL(candidates.size(), providerNames.size());
            for (size_t index = 0; index < candidates.size(); ++index) {
              const auto& candidate = candidates[index];
              // ACK arrival order is not a Provider identity.  The four
              // Providers can publish ACKs on different Face turns, so bind
              // the role to the candidate's Provider name rather than to the
              // vector position.
              const auto providerIt = std::find(
                  providerNames.begin(), providerNames.end(), candidate.providerName);
              BOOST_REQUIRE(providerIt != providerNames.end());
              const auto providerIndex = static_cast<size_t>(
                  std::distance(providerNames.begin(), providerIt));
              CollaborationAssignmentEnvelope assignment;
              assignment.role = "role-" + std::to_string(providerIndex);
              assignment.assignedArtifact = ndn::Name(
                  "/artifact/role-" + std::to_string(providerIndex));
              const std::string assignmentText =
                  "device=cpu;rank=" + std::to_string(providerIndex) + ";";
              assignment.opaquePayload = ndn::Buffer(
                  reinterpret_cast<const uint8_t*>(assignmentText.data()),
                  assignmentText.size());

              BOOST_REQUIRE(environment.user().setSelectionAssignmentPayloadForRequest(
                requestId, candidate.providerName,
                encodeCollaborationAssignmentEnvelope(assignment)));
            }
            selectionPublished = true;
            return candidates;
          }),
      3000,
      [&] (const ndn::Name&) { timedOut = true; },
      [&] (const ResponseMessage& response) {
        if (response.getStatus()) {
          // The response callback is shared by the four selected Providers;
          // the individual provider ingress is asserted below.
          for (auto& seen : responseObserved) {
            if (!seen.exchange(true)) {
              break;
            }
          }
        }
      },
      tlv::AllSelected,
      requestId);
  BOOST_REQUIRE_EQUAL(returnedRequestId, requestId);

  environment.pumpUntil([&] {
    bool allRequests = true;
    for (const auto& observed : requestObserved) {
      allRequests = allRequests && observed.load();
    }
    bool allPending = true;
    for (size_t index = 0; index < environment.providerCount(); ++index) {
      allPending = allPending &&
          environment.provider(index).getPendingRequestCountForTesting() == 1;
    }
    return allRequests && allPending;
  });
  for (const auto& observed : requestObserved) {
    BOOST_CHECK(observed);
  }

  for (size_t index = 0; index < environment.providerCount(); ++index) {
    RequestAckMessage ack;
    ack.setStatus(true);
    ack.setMessage("d0-ack-" + std::to_string(index));
    const auto ackName = makeRequestAckNameV2(
        providerNames[index], requesterName, serviceName, requestId);
    const auto ackBlock = ack.WireEncode();
    const auto encrypted = makeTestHybridPublication(
        ackName, serviceName, requestId, providerNames[index], "ACK",
        ndn::Buffer(ackBlock.data(), ackBlock.size()));
    environment.user().cacheHybridReceiveKeyForTest(
        encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
    environment.providerPubSub(index).publish(
        ackName,
        ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
  }

  environment.pumpUntil([&] {
    return ackObserved.load() || timedOut.load();
  });
  BOOST_CHECK(ackObserved);
  BOOST_CHECK(selectionPublished);

  environment.pumpUntil([&] {
    bool allHandlers = true;
    for (const auto& called : handlerCalled) {
      allHandlers = allHandlers && called.load();
    }
    return allHandlers || timedOut.load();
  });
  for (const auto& called : handlerCalled) {
    BOOST_CHECK(called);
  }
  BOOST_CHECK(!timedOut);

  for (size_t index = 0; index < environment.providerCount(); ++index) {
    ResponseMessage response;
    response.setStatus(true);
    const std::string responseText = "d0-response-" + std::to_string(index);
    ndn::Buffer responsePayload(
        reinterpret_cast<const uint8_t*>(responseText.data()),
        responseText.size());
    response.setPayload(responsePayload, responsePayload.size());
    const auto responseName = makeResponseNameV2(
        providerNames[index], requesterName, serviceName, requestId);
    const auto responseBlock = response.WireEncode();
    const auto encrypted = makeTestHybridPublication(
        responseName, serviceName, requestId, providerNames[index], "RESPONSE",
        ndn::Buffer(responseBlock.data(), responseBlock.size()));
    environment.user().cacheHybridReceiveKeyForTest(
        encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
    environment.providerPubSub(index).publish(
        responseName,
        ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
  }

  environment.pumpUntil([&] {
    bool allResponses = true;
    for (const auto& observed : responseObserved) {
      allResponses = allResponses && observed.load();
    }
    return allResponses || timedOut.load();
  });
  for (const auto& observed : responseObserved) {
    BOOST_CHECK(observed);
  }
  BOOST_CHECK(!timedOut);
}

BOOST_AUTO_TEST_CASE(PreconfiguredEnvironmentRunsFourProviderRoleSplitCollaboration)
{
  test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/NativeTracer");
  profile.providerCount = 4;
  test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  auto scope = environment.beginRequest("four-provider-role-split");

  const auto serviceName = environment.profile().serviceName;
  const auto makeRole = [&] (const char* role, const char* artifact) {
    CollaborationRoleSpec spec;
    spec.role = role;
    spec.service = serviceName;
    spec.requiredArtifact = ndn::Name(artifact);
    return spec;
  };
  const std::vector<CollaborationRoleSpec> roles{
      makeRole("/Backbone", "/artifact/backbone"),
      makeRole("/Head/Shard/0", "/artifact/head-0"),
      makeRole("/Head/Shard/1", "/artifact/head-1"),
      makeRole("/Merge", "/artifact/merge")};

  std::array<std::atomic<bool>, 4> requestReceived{};
  std::array<std::atomic<bool>, 4> selectionObserved{};
  std::array<std::atomic<bool>, 4> handlerCalled{};
  std::array<std::atomic<bool>, 4> assignmentValid{};
  std::atomic<bool> responseReceived{false};
  std::atomic<bool> timedOut{false};
  std::atomic<size_t> observedAssignmentCount{0};

  for (size_t index = 0; index < environment.providerCount(); ++index) {
    auto& provider = environment.provider(index);
    const auto providerName = provider.getName();
    provider.addCollaborationHandler(
        serviceName,
        [&, index, providerName] (ServiceProvider::CollaborationContext& context,
                                  const RequestMessage& request) {
          BOOST_CHECK_EQUAL(request.getPayload().size(), 7);
          handlerCalled[index] = true;
          const auto& assignment = context.assignment();
          observedAssignmentCount.fetch_add(assignment.roleProviders.size());
          const auto roleIt = assignment.roleProviders.find(assignment.role);
          assignmentValid[index] =
              roleIt != assignment.roleProviders.end() &&
              roleIt->second == providerName.toUri();
          const std::string responseText =
              "role-ready-" + std::to_string(index);
          context.publishFinalResponse(ndn::Buffer(
              reinterpret_cast<const uint8_t*>(responseText.data()),
              responseText.size()));
          BOOST_TEST_MESSAGE("D0 handler provider=" << providerName.toUri());
        });

    provider.setLocalPublicationHandler(
        [&, index] (const ndn::Name& messageName, const ndn::Buffer& wire) {
          if (!parseRequestAckNameV2(messageName) &&
              !parseResponseNameV2(messageName)) {
            return;
          }
          environment.providerPubSub(index).publish(
              messageName,
              ndn::span<const uint8_t>(wire.data(), wire.size()));
        });

    environment.providerPubSub(index).subscribeToProducer(
        environment.profile().userNode,
        [&, index, providerName] (
            const ndn::svs::SVSPubSub::SubscriptionData& publication) {
          if (const auto parsedRequest = parseRequestNameV2(publication.name)) {
            if (!parsedRequest->serviceName.equals(serviceName)) {
              return;
            }
            RequestMessage request;
            ndn::Block requestBlock(publication.data);
            BOOST_REQUIRE(request.WireDecode(requestBlock));
            requestReceived[index] = true;
            const auto requestWire = request.WireEncode();
            const ndn::Buffer requestBuffer(requestWire.data(), requestWire.size());
            environment.provider(index).OnRequestDecryptionSuccessCallbackV2(
                parsedRequest->requesterName,
                parsedRequest->serviceName,
                parsedRequest->requestId,
                requestBuffer);
            return;
          }

          const auto parsedSelection = parseServiceSelectionNameV2(publication.name);
          if (!parsedSelection ||
              !parsedSelection->serviceName.equals(serviceName) ||
              !parsedSelection->providerName.equals(providerName)) {
            return;
          }
          selectionObserved[index] = true;
          ndn::Block selectionBlock(publication.data);
          ndn::Buffer selectionWire(selectionBlock.data(), selectionBlock.size());
          environment.provider(index)
              .OnServiceSelectionMessageDecryptionSuccessCallbackV2(
                  parsedSelection->requesterName,
                  parsedSelection->providerName,
                  parsedSelection->serviceName,
                  parsedSelection->requestId,
                  selectionWire);
        },
        true);

    auto providerNode = environment.profile().providerNode;
    if (index > 0) {
      providerNode.append("p" + std::to_string(index));
    }
    environment.userPubSub().subscribeToProducer(
        providerNode,
        [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
          if (const auto parsedAck = parseRequestAckNameV2(publication.name)) {
            if (parsedAck->serviceName.equals(serviceName)) {
              ndn::Block ackBlock(publication.data);
              environment.user().handleRequestAckByName(publication.name, ackBlock);
            }
            return;
          }
          const auto parsedResponse = parseResponseNameV2(publication.name);
          if (!parsedResponse || !parsedResponse->serviceName.equals(serviceName)) {
            return;
          }
          ndn::Block responseBlock(publication.data);
          responseReceived = environment.user().handleDecryptedResponseByName(
                                 publication.name, responseBlock) || responseReceived;
        },
        true);
  }

  environment.user().setLocalPublicationHandler(
      [&] (const ndn::Name& messageName, const ndn::Buffer& wire) {
        if (!parseServiceSelectionNameV2(messageName)) {
          return;
        }
        environment.userPubSub().publish(
            messageName,
            ndn::span<const uint8_t>(wire.data(), wire.size()));
      });
  environment.user().setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& requestName,
           const std::vector<ndn::Name>&, const ndn::Name& publishedService,
           const RequestMessage& request, size_t strategy) {
        BOOST_CHECK_EQUAL(publishedService, serviceName);
        BOOST_CHECK_EQUAL(strategy, tlv::AllSelected);
        const auto requestBlock = request.WireEncode();
        environment.userPubSub().publish(
            requestName,
            ndn::span<const uint8_t>(requestBlock.data(), requestBlock.size()));
        environment.markRequestPublished(scope);
      });

  CollaborationPlan plan;
  plan.ackCollectionTimeMs = 30;
  plan.timeoutMs = 1000;
  plan.roles = roles;
  plan.participantSelector = std::make_shared<OneRolePerProviderSelection>();

  const std::string requestPayload = "payload";
  const auto requestId = environment.user().RequestCollaboration(
      serviceName,
      ndn::Buffer(reinterpret_cast<const uint8_t*>(requestPayload.data()),
                  requestPayload.size()),
      std::move(plan),
      [&] (const ResponseMessage& response) {
        responseReceived = response.getStatus() || responseReceived;
      },
      [&] (const ndn::Name&) { timedOut = true; });
  BOOST_REQUIRE(!requestId.empty());

  environment.pumpUntil([&] {
    bool allRequests = true;
    bool allSelections = true;
    bool allHandlers = true;
    for (size_t index = 0; index < environment.providerCount(); ++index) {
      allRequests = allRequests && requestReceived[index].load();
      allSelections = allSelections && selectionObserved[index].load();
      allHandlers = allHandlers && handlerCalled[index].load();
    }
    return responseReceived && allRequests && allSelections && allHandlers;
  });

  for (size_t index = 0; index < environment.providerCount(); ++index) {
    BOOST_CHECK(requestReceived[index]);
    BOOST_CHECK(selectionObserved[index]);
    BOOST_CHECK(handlerCalled[index]);
    BOOST_CHECK(assignmentValid[index]);
  }
  BOOST_CHECK_EQUAL(observedAssignmentCount.load(),
                    roles.size() * environment.providerCount());
  BOOST_CHECK(responseReceived);
  BOOST_CHECK(!timedOut);
  environment.updateRequestResidue(scope, {});
  environment.resetRequest(scope);
}

BOOST_AUTO_TEST_CASE(PreconfiguredEnvironmentRunsSameProviderMultiRoleCollaboration)
{
  ndn_service_framework::test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Inference/NativeTracer");
  profile.providerCount = 1;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  auto scope = environment.beginRequest("same-provider-multi-role");

  const auto serviceName = environment.profile().serviceName;
  const auto providerName = environment.provider().getName();
  const auto requesterName = environment.user().getName();
  const auto makeRole = [&] (const char* role, const char* artifact) {
    CollaborationRoleSpec spec;
    spec.role = role;
    spec.service = serviceName;
    spec.requiredArtifact = ndn::Name(artifact);
    return spec;
  };
  const std::vector<CollaborationRoleSpec> roles{
      makeRole("/Backbone", "/artifact/backbone"),
      makeRole("/Head/Shard/0", "/artifact/head-0"),
      makeRole("/Head/Shard/1", "/artifact/head-1"),
      makeRole("/Merge", "/artifact/merge")};

  std::atomic<bool> requestReceived{false};
  std::atomic<bool> selectionObserved{false};
  std::atomic<bool> handlerCalled{false};
  std::atomic<size_t> handlerExecutionCount{0};
  std::atomic<bool> responseReceived{false};
  std::atomic<bool> timedOut{false};
  std::atomic<size_t> observedAssignmentCount{0};

  environment.provider().addCollaborationHandler(
      serviceName,
      [&] (ServiceProvider::CollaborationContext& context,
           const RequestMessage& request) {
        BOOST_CHECK_EQUAL(request.getPayload().size(), 7);
        handlerCalled = true;
        handlerExecutionCount.fetch_add(1);
        observedAssignmentCount = context.assignment().roleProviders.size();
        const std::string responseText = "di-ready";
        context.publishFinalResponse(ndn::Buffer(
            reinterpret_cast<const uint8_t*>(responseText.data()), responseText.size()));
      });

  // The LocalMockTag runtime emits the framework-generated Selection and
  // Response through this boundary. The bytes then traverse real in-process
  // SVSPubSub, so this test does not hand-call the Provider selection parser.
  environment.user().setLocalPublicationHandler(
      [&] (const ndn::Name& messageName, const ndn::Buffer& wire) {
        const auto parsed = parseServiceSelectionNameV2(messageName);
        if (!parsed) {
          return;
        }
        environment.userPubSub().publish(
            messageName, ndn::span<const uint8_t>(wire.data(), wire.size()));
      });
  environment.provider().setLocalPublicationHandler(
      [&] (const ndn::Name& messageName, const ndn::Buffer& wire) {
        const auto parsedAck = parseRequestAckNameV2(messageName);
        const auto parsedResponse = parseResponseNameV2(messageName);
        if (!parsedAck && !parsedResponse) {
          return;
        }
        environment.providerPubSub().publish(
            messageName, ndn::span<const uint8_t>(wire.data(), wire.size()));
      });

  environment.providerPubSub().subscribeToProducer(
      environment.profile().userNode,
      [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        if (const auto parsedRequest = parseRequestNameV2(publication.name)) {
          if (!parsedRequest->serviceName.equals(serviceName)) {
            return;
          }
          RequestMessage request;
          ndn::Block requestBlock(publication.data);
          BOOST_REQUIRE(request.WireDecode(requestBlock));
          requestReceived = true;
          const auto requestWire = request.WireEncode();
          const ndn::Buffer requestBuffer(requestWire.data(), requestWire.size());
          // This is the post-decryption entry point used by OnRequest. It
          // performs token validation, ACK admission, pending-request
          // storage, and ACK publication. Calling
          // handleDecryptedRequestByName here would intentionally reject
          // AllSelected before Selection and would skip the real pending
          // state required by the assignment path.
          environment.provider().OnRequestDecryptionSuccessCallbackV2(
              parsedRequest->requesterName, parsedRequest->serviceName,
              parsedRequest->requestId, requestBuffer);
          return;
        }

        const auto parsedSelection = parseServiceSelectionNameV2(publication.name);
        if (!parsedSelection ||
            !parsedSelection->serviceName.equals(serviceName) ||
            !parsedSelection->providerName.equals(providerName)) {
          return;
        }
        selectionObserved = true;
        ndn::Block selectionBlock(publication.data);
        ndn::Buffer selectionWire(selectionBlock.data(), selectionBlock.size());
        environment.provider().OnServiceSelectionMessageDecryptionSuccessCallbackV2(
            parsedSelection->requesterName, parsedSelection->providerName,
            parsedSelection->serviceName, parsedSelection->requestId,
            selectionWire);
      },
      true);

  environment.userPubSub().subscribeToProducer(
      environment.profile().providerNode,
      [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        const auto parsedAck = parseRequestAckNameV2(publication.name);
        if (parsedAck && parsedAck->serviceName.equals(serviceName)) {
          ndn::Block ackBlock(publication.data);
          environment.user().handleRequestAckByName(publication.name, ackBlock);
          return;
        }
        const auto parsedResponse = parseResponseNameV2(publication.name);
        if (!parsedResponse || !parsedResponse->serviceName.equals(serviceName)) {
          return;
        }
        ndn::Block responseBlock(publication.data);
        responseReceived = environment.user().handleDecryptedResponseByName(
                               publication.name, responseBlock) || responseReceived;
      },
      true);

  environment.user().setRequestPublisher(
      [&] (const ndn::Name&, const ndn::Name& requestName,
           const std::vector<ndn::Name>&, const ndn::Name& publishedService,
           const RequestMessage& request, size_t strategy) {
        BOOST_CHECK_EQUAL(publishedService, serviceName);
        BOOST_CHECK_EQUAL(strategy, tlv::AllSelected);
        const auto requestBlock = request.WireEncode();
        environment.userPubSub().publish(
            requestName,
            ndn::span<const uint8_t>(requestBlock.data(), requestBlock.size()));
        environment.markRequestPublished(scope);
      });

  CollaborationPlan plan;
  plan.ackCollectionTimeMs = 30;
  plan.timeoutMs = 1000;
  plan.roles = roles;
  plan.participantSelector = std::make_shared<SameProviderMultiRoleSelection>();

  const std::string requestPayload = "payload";
  const auto requestId = environment.user().RequestCollaboration(
      serviceName,
      ndn::Buffer(reinterpret_cast<const uint8_t*>(requestPayload.data()),
                  requestPayload.size()),
      std::move(plan),
      [&] (const ResponseMessage& response) {
        responseReceived = response.getStatus() &&
                           std::string(reinterpret_cast<const char*>(response.getPayload().data()),
                                       response.getPayload().size()) == "di-ready";
      },
      [&] (const ndn::Name&) { timedOut = true; });
  BOOST_REQUIRE(!requestId.empty());

  environment.pumpUntil([&] { return responseReceived || timedOut; });
  BOOST_CHECK(requestReceived);
  BOOST_CHECK(selectionObserved);
  BOOST_CHECK(handlerCalled);
  BOOST_CHECK_EQUAL(handlerExecutionCount.load(), 1U);
  BOOST_CHECK_EQUAL(observedAssignmentCount.load(), roles.size());
  BOOST_CHECK(responseReceived);
  BOOST_CHECK(!timedOut);
  environment.updateRequestResidue(scope, {});
  environment.resetRequest(scope);
}

BOOST_AUTO_TEST_CASE(Spec175DiWriterExposesCursorAndOneTerminal)
{
  std::uint64_t nextCursor = 0;
  std::vector<std::uint64_t> committed;
  bool responseFinished = false;
  auto core = std::make_shared<StreamedResponseWriterCore>(
    [&] (const ndn::Buffer&, std::uint64_t& cursor) {
      cursor = ++nextCursor;
      committed.push_back(cursor);
      return true;
    },
    [&] (const ndn::Buffer&, StreamFinishReason reason) {
      responseFinished = reason == StreamFinishReason::ApplicationComplete;
      return responseFinished;
    },
    [] (StreamedInvocationErrorCode, const std::string&) { return true; },
    [] { return false; },
    [] { return std::chrono::milliseconds(1000); });

  ndn::Buffer event(reinterpret_cast<const uint8_t*>("token"), 5);
  std::uint64_t cursor = 0;
  BOOST_CHECK(core->publish(event, cursor));
  BOOST_CHECK_EQUAL(cursor, 1U);
  BOOST_REQUIRE_EQUAL(committed.size(), 1U);
  BOOST_CHECK_EQUAL(committed.front(), 1U);
  BOOST_CHECK(core->finish(ndn::Buffer(), StreamFinishReason::ApplicationComplete));
  BOOST_CHECK(responseFinished);
  BOOST_CHECK(!core->publish(event, cursor));
  BOOST_CHECK(!core->finish(ndn::Buffer(), StreamFinishReason::ApplicationComplete));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
