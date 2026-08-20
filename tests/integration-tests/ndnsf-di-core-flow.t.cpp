#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DependencyWaitScheduler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderGroupCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndnsf-integration-fixture.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
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
          if (observedInputs != nullptr) {
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
  bool timedOut = false;
  bool statusFailed = false;
  std::string statusMessage;
};

NativeIngressCaseResult
runNativeIngressCase(bool mismatch)
{
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

  NativeProviderHandlerConfig handlerConfig;
  handlerConfig.plan = plan;
  handlerConfig.assignment = baseAssignment;
  handlerConfig.runnerFactory = makeNativeIngressTestRunnerFactory();
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
  const std::string requestText = "native-ingress-request";
  ndn::Buffer requestPayload(
    reinterpret_cast<const uint8_t*>(requestText.data()), requestText.size());
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
        ServiceSelectionMessage selection;
        selection.setRequestIDs({requestId.toUri()});
        selection.setAttempt(1);
        SelectionProviderEntry entry;
        entry.providerName = providerName;
        entry.assignmentPayload = encodeCollaborationAssignmentEnvelope(assignment);
        selection.addProviderEntry(entry);
        const auto selectionName = makeServiceSelectionNameV2(
          requesterName, providerName, serviceName, requestId);
        const auto selectionBlock = selection.WireEncode();
        const auto encryptedSelection = makeTestHybridPublication(
          selectionName, serviceName, requestId, requesterName, "SELECTION",
          ndn::Buffer(selectionBlock.data(), selectionBlock.size()));
        environment.provider().cacheHybridReceiveKeyForTest(
          encryptedSelection.key.keyId,
          encryptedSelection.key.epochId,
          encryptedSelection.key.key);
        environment.userPubSub().publish(
          selectionName,
          ndn::span<const uint8_t>(encryptedSelection.wire.data(),
                                   encryptedSelection.wire.size()));
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

        ServiceSelectionMessage selection;
        selection.setRequestIDs({requestId.toUri()});
        selection.setAttempt(1);
        SelectionProviderEntry entry;
        entry.providerName = providerName;
        entry.assignmentPayload = encodeOpaqueAssignmentSet(assignmentItems);
        selection.addProviderEntry(entry);
        const auto selectionName = makeServiceSelectionNameV2(
          requesterName, providerName, serviceName, requestId);
        const auto selectionBlock = selection.WireEncode();
        const auto encrypted = makeTestHybridPublication(
          selectionName, serviceName, requestId, requesterName, "SELECTION",
          ndn::Buffer(selectionBlock.data(), selectionBlock.size()));
        environment.provider().cacheHybridReceiveKeyForTest(
          encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
        environment.userPubSub().publish(
          selectionName,
          ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
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
          candidate});
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
          candidate});
    }
    return selected;
  }
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
                        const std::string& roleKind = "TENSOR_RANK")
{
  const auto deviceSet = (backend == "cpu" ||
                          (backend.size() > 4 &&
                           backend.compare(backend.size() - 4, 4, "-cpu") == 0))
    ? std::string("[]")
    : std::string("[\"") + device + "\"]";
  return std::string("{\"adapter_id\":\"qwen-test\",") +
    "\"adapter_version\":\"1\",\"artifact_digest\":\"" +
    artifactDigest + "\",\"backend\":\"" + backend +
    "\",\"device_set\":" + deviceSet +
    ",\"layer_begin\":0,\"layer_end\":1,\"rank\":" +
    std::to_string(rank) + ",\"recipe_digest\":\"" + recipeDigest +
    "\",\"role\":\"" + logicalRole +
    "\",\"role_kind\":\"" + roleKind + "\"}";
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
                         const std::string& manifestDigest)
{
  return std::string("{\"attempt\":1,\"consumer_role\":\"") +
    consumerRole + "\",\"consumer_roles\":[" + consumerRoles +
    "],\"endpoint_digest\":\"" + endpointDigest +
    "\",\"group_epoch\":\"1\",\"group_id\":\"" + groupId +
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
                              const std::string& device = "cpu:0")
{
  return std::string("{\"ack_closed_digest\":\"") + planDigest +
    "\",\"assembly\":" + roleJson +
    ",\"attempt\":1,\"dataflow\":" + dataflowJson +
    ",\"deadline_ms\":9999999999999,\"dependencies\":" +
    dependenciesJson + ",\"device_binding\":{\"mode\":\"SINGLE_DEVICE\",\"offer_digest\":\"" +
    offerDigest + "\",\"offer_scoped_device_handle\":\"" + device +
    "\",\"provider\":\"" + provider + "\",\"resource_sequence\":1," +
    "\"resource_snapshot_digest\":\"" + planDigest +
    "\",\"role\":\"" + roleKey +
    "\",\"sharing_policy\":\"EXCLUSIVE_ROLE\",\"topology_profile_digest\":\"" +
    planDigest + "\"},\"execution_role\":{\"adapter_id\":\"qwen-test\",\"adapter_version\":\"1\",\"backend\":\"onnxruntime\",\"layer_begin\":0,\"layer_end\":1,\"rank\":" +
    std::to_string(rank) + ",\"role_id\":\"" + roleKey +
    "\",\"stage_id\":\"" + logicalRole +
    "\"},\"group_capability_v1\":\"" + capabilityHex +
    "\",\"offer_digest\":\"" + offerDigest +
    "\",\"plan_core_digest\":\"" + planDigest +
    "\",\"plan_digest\":\"" + planDigest +
    "\",\"provider\":\"" + provider +
    "\",\"request_id\":\"" + requestId +
    "\",\"roles\":[" + roleJson +
    "],\"schema\":\"ndnsf-di-selection-v3\",\"schema_version\":3," +
    "\"security_policy_snapshot_digest\":\"" + planDigest + "\"}";
}

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

  auto dropped = environment.beginRequest(
      "fault-drop", ndn_service_framework::test::FaultProfile{true, false, false});
  environment.markRequestPublished(dropped);
  publishData("drop");
  BOOST_CHECK_GE(environment.bridgeStats().droppedPackets, 1);
  BOOST_CHECK_EQUAL(environment.bridgeStats().forwardedData, 0);
  BOOST_CHECK_EQUAL(environment.bridgeStats().firstDroppedName,
                    "/ndnsf/spec170/fault/drop");
  environment.resetRequest(dropped);

  auto duplicated = environment.beginRequest(
      "fault-duplicate", ndn_service_framework::test::FaultProfile{false, true, false});
  environment.markRequestPublished(duplicated);
  publishData("duplicate");
  BOOST_CHECK_EQUAL(environment.bridgeStats().duplicatedPackets, 1);
  BOOST_CHECK_EQUAL(environment.bridgeStats().forwardedData, 2);
  environment.resetRequest(duplicated);

  auto reordered = environment.beginRequest(
      "fault-reorder", ndn_service_framework::test::FaultProfile{false, false, true});
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

BOOST_AUTO_TEST_CASE(PreconfiguredEnvironmentRunsGenericRequestLifecycle)
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

  environment.pumpUntil([&] { return typedCallbackCalled; });
  BOOST_CHECK(requestPublished);
  BOOST_CHECK(providerReceived);
  BOOST_CHECK(handlerCalled);
  BOOST_CHECK(ackPublished);
  BOOST_CHECK(responsePublished);
  BOOST_CHECK(responseReceived);
  BOOST_CHECK(typedCallbackCalled);
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
        [&, index] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
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

            ServiceSelectionMessage selection;
            selection.setRequestIDs({requestId.toUri()});
            selection.setAttempt(1);
            SelectionProviderEntry entry;
            entry.providerName = providerName;
            entry.assignmentPayload = encodeOpaqueAssignmentSet(assignmentItems);
            selection.addProviderEntry(entry);
            const auto selectionName = makeServiceSelectionNameV2(
                requesterName, providerName, serviceName, requestId);
            selectionObserved = true;
            const auto selectionBlock = selection.WireEncode();
            const auto encrypted = makeTestHybridPublication(
                selectionName, serviceName, requestId, requesterName,
                "SELECTION",
                ndn::Buffer(selectionBlock.data(), selectionBlock.size()));
            environment.provider().cacheHybridReceiveKeyForTest(
                encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
            environment.userPubSub().publish(
                selectionName,
                ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
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
            ServiceSelectionMessage selection;
            selection.setRequestIDs({requestId.toUri()});
            selection.setAttempt(1);
            selection.addProviderEntry(SelectionProviderEntry{
                providerName, {}, encodeOpaqueAssignmentSet(assignmentItems)});
            const auto selectionName = makeServiceSelectionNameV2(
                requesterName, providerName, serviceName, requestId);
            const auto selectionBlock = selection.WireEncode();
            const auto encrypted = makeTestHybridPublication(
                selectionName, serviceName, requestId, requesterName, "SELECTION",
                ndn::Buffer(selectionBlock.data(), selectionBlock.size()));
            environment.provider().cacheHybridReceiveKeyForTest(
                encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
            environment.userPubSub().publish(
                selectionName,
                ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
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
                environment.providerFace(destination).receive(interest);
              }));
      providerPeerBridges.emplace_back(
          environment.providerFace(source).onSendData.connect(
              [&environment, &faultApplied, &faultTargetName, &delayedData,
               destination, source, fault] (const ndn::Data& data) {
                auto forward = [&] (const ndn::Data& packet) {
                  environment.providerFace(destination).receive(packet);
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
    publications.emplace_back(ndn::Name(segment.dataName),
                              ndn::Buffer(wire.begin(), wire.end()));
  }

  std::atomic<bool> ackObserved{false};
  std::atomic<bool> provider0Published{false};
  std::atomic<bool> provider1HandlerCalled{false};
  std::atomic<bool> timedOut{false};
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
            [contextCopy, fetchPromise, producerPrefix, operation] {
              try {
                fetchPromise->set_value(contextCopy->fetchDataV1Segments(
                    "/scope/d2b", producerPrefix, operation.operationIndex,
                    "0", "tensor-d2b", 2, operation.maxSegments, 3000));
              }
              catch (...) {
                fetchPromise->set_exception(std::current_exception());
              }
            });
      });
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

  // Publish the consumer Selection first. Its handler installs the
  // request-scoped DATA_V1 subscription before the producer is selected and
  // publishes the first segment; otherwise an already-published SVS object
  // would be invisible to this fetch API by design.
  auto publishSelection = [&] (const ndn::Name& providerName, const char* role) {
    CollaborationAssignmentEnvelope assignment;
    assignment.role = role;
    assignment.assignedArtifact = ndn::Name("/artifact").append(role);
    const std::string opaque = std::string("rank=") + role + ";";
    assignment.opaquePayload = ndn::Buffer(
        reinterpret_cast<const uint8_t*>(opaque.data()), opaque.size());
    ServiceSelectionMessage selection;
    selection.setRequestIDs({requestId.toUri()});
    selection.setAttempt(1);
    selection.addProviderEntry(SelectionProviderEntry{
        providerName, {}, encodeCollaborationAssignmentEnvelope(assignment)});
    const auto selectionName = makeServiceSelectionNameV2(
        requesterName, providerName, serviceName, requestId);
    const auto selectionBlock = selection.WireEncode();
    const auto encrypted = makeTestHybridPublication(
        selectionName, serviceName, requestId, requesterName, "SELECTION",
        ndn::Buffer(selectionBlock.data(), selectionBlock.size()));
    const auto providerIndex = providerName == provider0Name ? 0U : 1U;
    environment.provider(providerIndex).cacheHybridReceiveKeyForTest(
        encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
    environment.userPubSub().publish(
        selectionName,
        ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
  };

  RequestMessage request;
  const std::string requestText = "d2b-payload";
  ndn::Buffer requestPayload(
      reinterpret_cast<const uint8_t*>(requestText.data()), requestText.size());
  request.setPayload(requestPayload, requestPayload.size());
  request.setPolicyEpoch(environment.user().getCurrentPolicyEpoch());
  const auto returnedRequestId = environment.user().RequestService(
      std::vector<ndn::Name>{provider0Name, provider1Name}, serviceName, request, 200,
      ServiceUser::AckCandidatesHandler(
          [&] (const std::vector<AckSelectionCandidate>& candidates) {
            ackObserved = true;
            if (candidates.size() != 2) {
              return candidates;
            }
            publishSelection(provider1Name, "consumer");
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
    ack.setMessage("d2b-ack-" + std::to_string(index));
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

  environment.pumpUntil([&] {
    return provider1HandlerCalled || timedOut;
  });
  if (provider1HandlerCalled && !timedOut) {
    publishSelection(provider0Name, "producer");
  }
  environment.pumpUntil([&] {
    return provider0Published || timedOut;
  });
  BOOST_CHECK(ackObserved);
  BOOST_CHECK(provider0Published);
  BOOST_CHECK(provider1HandlerCalled);
  BOOST_CHECK(!timedOut);

  std::optional<std::vector<ndn::Buffer>> fetched;
  std::exception_ptr fetchError;
  try {
    fetched = fetchFuture.get();
  }
  catch (...) {
    fetchError = std::current_exception();
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
runProductionNativeD2bCase(bool tamperCapability)
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
  plan.roles = {"/Backbone", "/Aux", "/Head/Shard/0"};
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
  std::array<std::atomic<size_t>, 2> coordinatorFactoryCalls{};
  auto evidenceMutex = std::make_shared<std::mutex>();
  auto observedEvidence = std::make_shared<
    std::map<std::string, ExecutionEvidence>>();
  std::array<std::string, 2> selectionDigests;
  std::mutex responseNamesMutex;
  std::set<std::string> responseNames;
  std::atomic<size_t> uniqueResponsePublications{0};
  std::atomic<bool> ackObserved{false};
  std::atomic<bool> timedOut{false};

  NativeProviderAssignment assignment;
  assignment.providerByRole = {{"/Backbone", provider0Name.toUri()},
                               {"/Aux", provider0Name.toUri()},
                               {"/Head/Shard/0", provider1Name.toUri()}};

  auto makeRunnerSpec = [&] (size_t index, const std::string& role) {
    NativeModelRunnerSpec spec;
    spec.role = role;
    spec.kind = "onnx-model";
    spec.backend = "onnxruntime";
    spec.path = useRealOnnx
      ? std::string(realOnnxModel)
      : "/integration-test/d2b-native.onnx";
    spec.metadata["test.providerName"] = environment.provider(index).getName().toUri();
    spec.metadata["test.providerBootId"] = "d2b-native-boot-" + std::to_string(index);
    spec.metadata["test.planDigest"] = planDigest;
    spec.metadata["test.artifactDigest"] =
      "sha256:" + std::string(64, index == 0 ? 'a' : 'b');
    spec.metadata["test.deviceId"] = "0";
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
    provider.setUseTokens(false);
    provider.markHybridResponseKeyWrappedForTest(serviceName);
    const std::string role = index == 0 ? "/Backbone" : "/Head/Shard/0";
    std::vector<NativeModelRunnerSpec> runnerSpecs{
      makeRunnerSpec(index, role),
    };
    if (index == 0) {
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
    config.allowPreassembledV3Compatibility = true;
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
        selectionDigests[index] = context.assignment().selectionDigest;
        nativeHandler(context, request);
      });
  }

  environment.enableProductionIngressForTest();
  environment.user().setUseTokens(false);
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
      BOOST_REQUIRE_EQUAL(providers.size(), 2U);
      BOOST_CHECK_EQUAL(publishedService, serviceName);
      BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
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

  auto publishSelection = [&] (size_t index, const std::string& role) {
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
    const auto addAssignment = [&] (const std::string& assignedRole) {
      const auto roleKind = assignedRole == "/Aux"
        ? "COMPONENT_SET" : "TENSOR_RANK";
      const auto roleJson = makeV3SelectionRoleJson(
        assignedRole, 0, artifactDigest, recipeDigest, "onnxruntime",
        deviceSet, roleKind);
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
        (assignedRole == "/Aux" ? "true" : "false") +
        ",\"wait_for\":[]}";
      const auto text = makeV3SelectionProjectionJson(
        roleJson, assignedRole, assignedRole, 0, providerName.toUri(),
        requestId.toUri(), planDigest, selectionCapabilityHex,
        dependenciesJson, dataflow, artifactDigest, deviceSet);
      CollaborationAssignmentEnvelope envelope;
      envelope.role = assignedRole;
      envelope.assignedArtifact = ndn::Name("/artifact").append(assignedRole);
      envelope.opaquePayload = ndn::Buffer(
        reinterpret_cast<const uint8_t*>(text.data()), text.size());
      assignmentItems.push_back(
        encodeCollaborationAssignmentEnvelope(envelope));
    };
    addAssignment(role);
    if (index == 0) {
      addAssignment("/Aux");
    }
    const auto assignmentPayload = assignmentItems.size() == 1
      ? assignmentItems.front()
      : encodeOpaqueAssignmentSet(assignmentItems);
    ServiceSelectionMessage selection;
    selection.setRequestIDs({requestId.toUri()});
    selection.setAttempt(1);
    selection.addProviderEntry(SelectionProviderEntry{
      providerName, {}, assignmentPayload});
    const auto selectionName = makeServiceSelectionNameV2(
      requesterName, providerName, serviceName, requestId);
    const auto selectionBlock = selection.WireEncode();
    const auto encrypted = makeTestHybridPublication(
      selectionName, serviceName, requestId, requesterName, "SELECTION",
      ndn::Buffer(selectionBlock.data(), selectionBlock.size()));
    environment.provider(index).cacheHybridReceiveKeyForTest(
      encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
    environment.userPubSub().publish(
      selectionName,
      ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
  };

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
  const auto returnedRequestId = environment.user().RequestService(
    std::vector<ndn::Name>{provider0Name, provider1Name}, serviceName, request, 200,
    ServiceUser::AckCandidatesHandler(
      [&] (const std::vector<AckSelectionCandidate>& candidates) {
        ackObserved = candidates.size() == 2;
        if (candidates.size() == 2) {
          publishSelection(1, "/Head/Shard/0");
        }
        return candidates;
      }),
    8000,
    [&] (const ndn::Name&) { timedOut = true; },
    [&] (const ResponseMessage&) {},
    tlv::FirstResponding,
    requestId);
  BOOST_REQUIRE_EQUAL(returnedRequestId, requestId);

  environment.pumpUntil([&] {
    return (environment.provider(0).getPendingRequestCountForTesting() == 1 &&
            environment.provider(1).getPendingRequestCountForTesting() == 1) ||
           timedOut;
  });
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    RequestAckMessage ack;
    ack.setStatus(true);
    ack.setMessage("d2b-native-ack-" + std::to_string(index));
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

  environment.pumpUntil([&] { return handlerEntered[1] || timedOut; });
  const auto subscriptionDeadline = std::chrono::steady_clock::now() + 100ms;
  environment.pumpUntil([&] {
    return std::chrono::steady_clock::now() >= subscriptionDeadline || timedOut;
  });
  if (!timedOut) {
    publishSelection(0, "/Backbone");
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
    return uniqueResponsePublications.load() == 1 || timedOut ||
           (tamperCapability && selectionFailed());
  });

  BOOST_CHECK(ackObserved);
  if (!tamperCapability) {
    BOOST_CHECK(handlerEntered[0]);
    BOOST_CHECK(handlerEntered[1]);
    BOOST_CHECK_EQUAL(coordinatorFactoryCalls[0].load(), 2U);
    BOOST_CHECK_EQUAL(coordinatorFactoryCalls[1].load(), 1U);
    BOOST_CHECK_EQUAL(uniqueResponsePublications.load(), 1U);
    BOOST_CHECK(!timedOut);
  }
  else {
    BOOST_CHECK(selectionFailed());
    BOOST_CHECK_EQUAL(uniqueResponsePublications.load(), 0U);
    BOOST_CHECK(!timedOut);
  }
  if (!tamperCapability && !useRealOnnx) {
    std::lock_guard<std::mutex> lock(*observedMutex);
    BOOST_CHECK(observedRoles->count("/Backbone") != 0);
    BOOST_CHECK(observedRoles->count("/Aux") != 0);
    BOOST_CHECK(observedRoles->count("/Head/Shard/0") != 0);
    BOOST_REQUIRE(observedOutputs->count("/Aux") != 0);
    BOOST_CHECK_EQUAL(
      observedOutputs->at("/Aux").at("aux"),
      "aux:d2b-native-payload");
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
}

BOOST_AUTO_TEST_CASE(ProductionNativeHandlersRunD2bRequestToFinalResponse)
{
  runProductionNativeD2bCase(false);
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
  const auto publishSelection = [&] (size_t provider) {
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
          dependenciesJson, dataflow, artifactDigest(provider), "cpu:0");
        CollaborationAssignmentEnvelope envelope;
        envelope.role = role;
        envelope.assignedArtifact = ndn::Name("/artifact").append(role);
        envelope.opaquePayload = ndn::Buffer(
          reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
        assignmentItems.push_back(
          encodeCollaborationAssignmentEnvelope(envelope));
      }
      ServiceSelectionMessage selection;
      selection.setRequestIDs({requestId.toUri()});
      selection.setAttempt(1);
      selection.addProviderEntry(SelectionProviderEntry{
        providerNames[provider], {}, encodeOpaqueAssignmentSet(assignmentItems)});
      const auto selectionName = makeServiceSelectionNameV2(
        requesterName, providerNames[provider], serviceName, requestId);
      const auto selectionBlock = selection.WireEncode();
      const auto encrypted = makeTestHybridPublication(
        selectionName, serviceName, requestId, requesterName, "SELECTION",
        ndn::Buffer(selectionBlock.data(), selectionBlock.size()));
      environment.provider(provider).cacheHybridReceiveKeyForTest(
        encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
      environment.userPubSub().publish(
        selectionName,
        ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
  };

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
          publishSelection(1);
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

  environment.pumpUntil([&] { return handlerEntered[1] || timedOut; });
  const auto consumerReadyDeadline = std::chrono::steady_clock::now() + 100ms;
  environment.pumpUntil([&] {
    return std::chrono::steady_clock::now() >= consumerReadyDeadline || timedOut;
  });
  if (!timedOut) {
    publishSelection(0);
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
  const auto publishSelection = [&] (size_t provider) {
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
        dependenciesJson, dataflow, artifactDigest(provider), "cpu:0");
      CollaborationAssignmentEnvelope envelope;
      envelope.role = role;
      envelope.assignedArtifact = ndn::Name("/artifact").append(role);
      envelope.opaquePayload = ndn::Buffer(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
      assignmentItems.push_back(encodeCollaborationAssignmentEnvelope(envelope));
    }
    ServiceSelectionMessage selection;
    selection.setRequestIDs({requestId.toUri()});
    selection.setAttempt(1);
    selection.addProviderEntry(SelectionProviderEntry{
      providerNames[provider], {}, encodeOpaqueAssignmentSet(assignmentItems)});
    const auto selectionName = makeServiceSelectionNameV2(
      requesterName, providerNames[provider], serviceName, requestId);
    const auto selectionBlock = selection.WireEncode();
    const auto encrypted = makeTestHybridPublication(
      selectionName, serviceName, requestId, requesterName, "SELECTION",
      ndn::Buffer(selectionBlock.data(), selectionBlock.size()));
    environment.provider(provider).cacheHybridReceiveKeyForTest(
      encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
    environment.userPubSub().publish(
      selectionName,
      ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
  };

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
          publishSelection(0);
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

  environment.pumpUntil([&] { return handlerEntered[0] || timedOut; });
  const auto consumerReadyDeadline = std::chrono::steady_clock::now() + 100ms;
  environment.pumpUntil([&] {
    return std::chrono::steady_clock::now() >= consumerReadyDeadline || timedOut;
  });
  if (!timedOut) {
    publishSelection(1);
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
          [&, index] (ServiceProvider::CollaborationContext& context,
                      const RequestMessage& request) {
            if (request.getPayload().size() != 11) {
              return;
            }
            const auto& localRoles = mapping[index];
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
            [&] (const std::vector<AckSelectionCandidate>& candidates) {
              ackObserved = true;
              if (candidates.size() != 2) {
                return candidates;
              }
              for (size_t index = 0; index < environment.providerCount(); ++index) {
                std::vector<ndn::Buffer> assignmentItems;
                for (const auto& role : mapping[index]) {
                  CollaborationAssignmentEnvelope assignment;
                  assignment.role = role;
                  assignment.assignedArtifact = ndn::Name("/artifact").append(role);
                  const std::string opaque = std::string("mapping=") +
                                             mappingLabel + ";rank=" + role + ";";
                  assignment.opaquePayload = ndn::Buffer(
                      reinterpret_cast<const uint8_t*>(opaque.data()), opaque.size());
                  assignmentItems.push_back(
                      encodeCollaborationAssignmentEnvelope(assignment));
                }
                ServiceSelectionMessage selection;
                selection.setRequestIDs({requestId.toUri()});
                selection.setAttempt(1);
                selection.addProviderEntry(SelectionProviderEntry{
                    providerNames[index], {}, encodeOpaqueAssignmentSet(assignmentItems)});
                const auto selectionName = makeServiceSelectionNameV2(
                    requesterName, providerNames[index], serviceName, requestId);
                const auto selectionBlock = selection.WireEncode();
                const auto encrypted = makeTestHybridPublication(
                    selectionName, serviceName, requestId, requesterName,
                    "SELECTION", ndn::Buffer(selectionBlock.data(), selectionBlock.size()));
                environment.provider(index).cacheHybridReceiveKeyForTest(
                    encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
                environment.userPubSub().publish(
                    selectionName,
                    ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
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
          BOOST_CHECK(roleIt != context.assignment().roleProviders.end());
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
              CollaborationAssignmentEnvelope assignment;
              assignment.role = "role-" + std::to_string(index);
              assignment.assignedArtifact = ndn::Name(
                  "/artifact/role-" + std::to_string(index));
              const std::string assignmentText =
                  "device=cpu;rank=" + std::to_string(index) + ";";
              assignment.opaquePayload = ndn::Buffer(
                  reinterpret_cast<const uint8_t*>(assignmentText.data()),
                  assignmentText.size());

              ServiceSelectionMessage selection;
              selection.setRequestIDs({requestId.toUri()});
              selection.setAttempt(1);
              SelectionProviderEntry entry;
              entry.providerName = candidate.providerName;
              entry.assignmentPayload =
                  encodeCollaborationAssignmentEnvelope(assignment);
              selection.addProviderEntry(entry);
              const auto selectionName = makeServiceSelectionNameV2(
                  requesterName, candidate.providerName, serviceName, requestId);
              const auto selectionBlock = selection.WireEncode();
              const auto encrypted = makeTestHybridPublication(
                  selectionName, serviceName, requestId, requesterName,
                  "SELECTION",
                  ndn::Buffer(selectionBlock.data(), selectionBlock.size()));
              const auto providerIt = std::find(
                  providerNames.begin(), providerNames.end(), candidate.providerName);
              BOOST_REQUIRE(providerIt != providerNames.end());
              const auto providerIndex = static_cast<size_t>(
                  std::distance(providerNames.begin(), providerIt));
              environment.provider(providerIndex).cacheHybridReceiveKeyForTest(
                  encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
              environment.userPubSub().publish(
                  selectionName,
                  ndn::span<const uint8_t>(encrypted.wire.data(),
                                           encrypted.wire.size()));
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
        [&, index] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
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

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
