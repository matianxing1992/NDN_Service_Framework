#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/common.hpp"
#include "ndn-service-framework/collaboration-input.hpp"
#include "ndn-service-framework/dependency-transfer.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <ndn-cxx/util/time.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace ndn_service_framework;

const ndn::Name GROUP_PREFIX("/example/uav");
const ndn::Name CONTROLLER_PREFIX("/example/uav/controller");
const ndn::Name SERVICE_NAME("/Spec226/Admission");
const ndn::Name SOURCE_A("/example/uav/source-adapter/UAV1");
const ndn::Name SOURCE_B("/example/uav/source-adapter/UAV2");
const ndn::Name CONSUMER("/example/uav/compute");
const ndn::Name USER("/example/uav/gs");
const auto BOOTSTRAP_TIMEOUT = std::chrono::seconds(45);
const auto REQUEST_TIMEOUT = std::chrono::seconds(18);
const auto SOURCE_A_DELAY = std::chrono::milliseconds(1500);
constexpr size_t CORE_LOSS_OBJECT_BYTES = 256 * 1024;
constexpr uint64_t CORE_LOSS_DATASET_SEED = 226;

uint64_t
steadyMicros()
{
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string
wireSha256(const ndn::Data& data)
{
  const auto& wire = data.wireEncode();
  const std::string bytes(reinterpret_cast<const char*>(wire.data()), wire.size());
  std::istringstream input(bytes, std::ios::in | std::ios::binary);
  ndn::util::Sha256 digest(input);
  const auto value = digest.computeDigest();
  std::ostringstream hex;
  hex << std::hex << std::setfill('0');
  for (const auto byte : *value) {
    hex << std::setw(2) << static_cast<unsigned>(byte);
  }
  return hex.str();
}

struct ActorOptions
{
  std::string mode;
  std::string trustSchema;
  std::string requestRow;
  std::string inputAdmissionGate;
  std::filesystem::path wireOutputDir;
  std::filesystem::path shutdownFile;
};

std::string
getOption(int argc, char** argv, const std::string& option,
          const std::string& fallback)
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == option) {
      return argv[i + 1];
    }
  }
  return fallback;
}

ActorOptions
parseOptions(int argc, char** argv)
{
  ActorOptions options;
  options.mode = getOption(argc, argv, "--mode", "");
  options.trustSchema = getOption(argc, argv, "--trust-schema", "");
  options.requestRow = getOption(argc, argv, "--request-row", "");
  options.inputAdmissionGate = getOption(argc, argv, "--input-admission-gate", "");
  options.wireOutputDir = getOption(argc, argv, "--wire-output-dir", "");
  options.shutdownFile = getOption(argc, argv, "--shutdown-file", "");
  if (options.mode != "source-a" && options.mode != "source-b" &&
      options.mode != "consumer" && options.mode != "user") {
    throw std::invalid_argument("--mode must name a Spec226 Q1 actor");
  }
  if (options.trustSchema.empty()) {
    throw std::invalid_argument("--trust-schema is required");
  }
  if (options.mode == "user") {
    if (options.requestRow != "q1-cold" && options.requestRow != "q1-warm" &&
        options.requestRow != "q1-missing" &&
        options.requestRow.rfind("core-loss-", 0) != 0) {
      throw std::invalid_argument(
          "--request-row must name a Q1 or Core-loss workload for --mode user");
    }
  }
  else if (!options.requestRow.empty()) {
    throw std::invalid_argument("--request-row is valid only for --mode user");
  }
  if (!options.inputAdmissionGate.empty() && options.mode != "consumer") {
    throw std::invalid_argument("--input-admission-gate is valid only for --mode consumer");
  }
  if (!options.wireOutputDir.empty() && options.mode != "source-a" &&
      options.mode != "consumer") {
    throw std::invalid_argument("--wire-output-dir is valid only for source-a or consumer");
  }
  if (!options.shutdownFile.empty() && options.mode != "source-a" &&
      options.mode != "source-b" && options.mode != "consumer") {
    throw std::invalid_argument("--shutdown-file is valid only for Provider actors");
  }
  return options;
}

ndn::security::Certificate
getCertificate(ndn::KeyChain& keyChain, const ndn::Name& identity)
{
  return keyChain.getPib().getIdentity(identity)
    .getDefaultKey().getDefaultCertificate();
}

ndn::Name
inputName(const ndn::Name& producer, uint64_t version)
{
  return ndn::Name(producer).append("NDNSF").append("INPUT").appendVersion(version);
}

bool
isCoreLossRequest(const std::string& requestLabel)
{
  return requestLabel.rfind("core-loss-", 0) == 0;
}

bool
isCoreLossXorRequest(const std::string& requestLabel)
{
  return isCoreLossRequest(requestLabel) &&
         requestLabel.rfind("core-loss-Xor-", 0) == 0;
}

ndn::Name
coreLossInputName(const ndn::Name& producer, bool useXor, size_t objectIndex)
{
  ndn::Name name(producer);
  name.append("NDNSF").append("INPUT").append("core-loss")
      .append(useXor ? "xor-packet-set-v1" : "arq-v1")
      .append("dataset-226").append("object").appendNumber(objectIndex);
  return name;
}

size_t
coreLossObjectIndex(const std::string& requestLabel)
{
  const auto marker = requestLabel.rfind("-object-");
  if (!isCoreLossRequest(requestLabel) || marker == std::string::npos ||
      marker + 8 >= requestLabel.size()) {
    throw std::invalid_argument("malformed Core-loss request label");
  }
  const auto text = requestLabel.substr(marker + 8);
  size_t consumed = 0;
  const auto value = std::stoul(text, &consumed);
  if (consumed != text.size() || value >= 32) {
    throw std::invalid_argument("Core-loss object index is out of range");
  }
  return static_cast<size_t>(value);
}

ndn::Buffer
makeCoreLossPayload(size_t objectIndex)
{
  ndn::Buffer payload(CORE_LOSS_OBJECT_BYTES);
  for (size_t i = 0; i < payload.size(); ++i) {
    const auto mixed = static_cast<uint64_t>(i) * 0x9e3779b1ULL +
      CORE_LOSS_DATASET_SEED * 0x85ebca6bULL + objectIndex * 0xc2b2ae35ULL;
    payload[i] = static_cast<uint8_t>((mixed ^ (mixed >> 11) ^ (i >> 7)) & 0xff);
  }
  return payload;
}

std::string
sha256Text(const std::string& text)
{
  std::istringstream input(text, std::ios::in | std::ios::binary);
  ndn::util::Sha256 digest(input);
  const auto value = digest.computeDigest();
  std::ostringstream hex;
  hex << std::hex << std::setfill('0');
  for (const auto byte : *value) {
    hex << std::setw(2) << static_cast<unsigned>(byte);
  }
  return hex.str();
}

std::string
bufferSha256(const ndn::Buffer& buffer)
{
  const std::string bytes(reinterpret_cast<const char*>(buffer.data()), buffer.size());
  return "sha256:" + sha256Text(bytes);
}

void
saveOriginalWire(const std::filesystem::path& outputDir, const ndn::Data& data,
                 std::mutex& outputMutex, std::set<std::string>& savedNames)
{
  if (outputDir.empty()) return;
  const auto name = data.getName().toUri();
  std::lock_guard<std::mutex> lock(outputMutex);
  if (!savedNames.insert(name).second) return;
  const auto nameDigest = sha256Text(name);
  const auto& wire = data.wireEncode();
  const auto packetPath = outputDir / (nameDigest + ".wire");
  std::filesystem::create_directories(outputDir);
  {
    std::ofstream packet(packetPath, std::ios::binary | std::ios::trunc);
    if (!packet || !packet.write(reinterpret_cast<const char*>(wire.data()),
                                 static_cast<std::streamsize>(wire.size()))) {
      throw std::runtime_error("cannot save captured original Data wire");
    }
  }
  std::ofstream index(outputDir / "packet-index.tsv", std::ios::app);
  if (!index || !(index << name << '\t' << nameDigest << '\t'
                         << sha256Text(std::string(
                              reinterpret_cast<const char*>(wire.data()), wire.size()))
                         << '\n')) {
    throw std::runtime_error("cannot append captured original Data index");
  }
}

struct ConsumerObservations
{
  std::atomic<size_t> resolverCount{0};
  std::atomic<size_t> validatorCount{0};
  std::atomic<size_t> computeCount{0};
  InputAdmissionResult result;
};

class Q1ParticipantSelector final : public ParticipantSelectionPolicy
{
public:
  std::vector<SelectedParticipant>
  select(const std::vector<AckCandidate>& candidates,
         const std::vector<CollaborationRoleSpec>& roles) const override
  {
    const auto sourceRole = std::find_if(roles.begin(), roles.end(),
      [] (const CollaborationRoleSpec& role) { return role.role == "/source"; });
    const bool selectOneSource = sourceRole != roles.end() && sourceRole->maxProviders == 1;
    std::vector<SelectedParticipant> selected;
    for (const auto& candidate : candidates) {
      if (candidate.serviceName != SERVICE_NAME) {
        continue;
      }
      if (candidate.providerName == SOURCE_A ||
          (!selectOneSource && candidate.providerName == SOURCE_B)) {
        selected.push_back({"/source", candidate.serviceName,
                            candidate.providerName, {}, false, 0,
                            ndn::Buffer{'q', '1'}, candidate, {}});
      }
      else if (candidate.providerName == CONSUMER) {
        selected.push_back({"/consumer", candidate.serviceName,
                            candidate.providerName, {}, false, 0,
                            ndn::Buffer{'q', '1'}, candidate, {}});
      }
    }
    return selected;
  }
};

ServiceProvider::AckStrategyHandler
makeAckHandler()
{
  return [](const RequestMessage&) {
    ServiceProvider::AckDecision decision;
    decision.status = true;
    decision.message = "spec226-data-admission-v1";
    return decision;
  };
}

void
runProvider(const ActorOptions& options, ndn::Face& face,
            ndn::KeyChain& keyChain,
            const ndn::security::Certificate& controllerCert)
{
  ndn::Name identity;
  std::string role;
  if (options.mode == "source-a") {
    identity = SOURCE_A;
    role = "source-a";
  }
  else if (options.mode == "source-b") {
    identity = SOURCE_B;
    role = options.mode;
  }
  else {
    identity = CONSUMER;
    role = "consumer";
  }

  const auto identityCert = getCertificate(keyChain, identity);
  CertificatePublisher certificatePublisher(face, keyChain, identityCert.getName());
  ServiceProvider provider(face, GROUP_PREFIX, identityCert, controllerCert,
                           options.trustSchema);
  const auto ackHandler = makeAckHandler();
  CollaborationAdmissionLimits limits;
  limits.maxInputAcquisitionMs = 5000;
  limits.maxInFlightReads = 2;

  std::mutex wireOutputMutex;
  std::set<std::string> savedWireNames;

  ServiceProvider::CollaborationHandler handler;
  if (options.mode == "source-a" || options.mode == "source-b") {
    const auto delay = options.mode == "source-a" ? SOURCE_A_DELAY
                                                   : std::chrono::milliseconds(0);
    handler = [delay, identity](
        ServiceProvider::CollaborationContext& context,
        const RequestMessage& request) {
      const auto payload = request.getPayload();
      const std::string requestLabel(payload.begin(), payload.end());
      const auto requestDelay = isCoreLossRequest(requestLabel) ?
          std::chrono::milliseconds(0) : delay;
      if (isCoreLossRequest(requestLabel)) {
        if (identity != SOURCE_A) {
          throw std::runtime_error("Core-loss request selected the wrong Producer");
        }
        const auto objectIndex = coreLossObjectIndex(requestLabel);
        const auto exactName = coreLossInputName(
            identity, isCoreLossXorRequest(requestLabel), objectIndex);
        const auto objectPayload = makeCoreLossPayload(objectIndex);
        CollaborationTransferMetrics metrics;
        const auto publishedName = context.publishLargeNamed(
            "input", exactName, objectPayload, 7000, 60000, &metrics);
        if (publishedName != exactName) {
          throw std::runtime_error("Core-loss object publication failed");
        }
        std::cout << "SPEC226_CORE_LOSS_SOURCE row=" << requestLabel
                  << " index=" << objectIndex
                  << " name=" << exactName.toUri()
                  << " bytes=" << objectPayload.size()
                  << " recoveryMode=" << metrics.recoveryMode
                  << " repairPublishedWireBytes=" << metrics.repairPublishedWireBytes
                  << " publishedAtUs=" << steadyMicros() << std::endl;
        context.completeRole();
        std::cout << "SPEC226_CORE_LOSS_SOURCE_DONE row=" << requestLabel
                  << " index=" << objectIndex << std::endl;
        return;
      }
      if (requestLabel != "q1-cold" && requestLabel != "q1-warm" &&
          requestLabel != "q1-missing") {
        throw std::runtime_error("Spec226 Q1 source received an unknown row");
      }
      const auto exactName = inputName(
          identity, requestLabel == "q1-missing" ? 2 : 1);
      const bool publish = identity != SOURCE_B || requestLabel != "q1-missing";
      if (requestDelay.count() > 0) {
        std::this_thread::sleep_for(requestDelay);
      }
      if (publish && !context.publishSignedExactData(
              "input", {{exactName, ndn::Buffer{
                static_cast<uint8_t>(identity == SOURCE_A ? 3 : 7)}}})) {
        throw std::runtime_error("Spec226 Q1 source publication failed");
      }
      std::cout << "SPEC226_Q1_SOURCE row=" << requestLabel
                << " role="
                << (identity == SOURCE_A ? "source-a" : "source-b")
                << " published=" << (publish ? "true" : "false")
                << " name=" << exactName.toUri()
                << " publishedAtUs=" << steadyMicros() << std::endl;
      context.completeRole();
      std::cout << "SPEC226_Q1_SOURCE_DONE row=" << requestLabel
                << " role="
                << (identity == SOURCE_A ? "source-a" : "source-b")
                << std::endl;
    };
  }
  else {
    handler = [inputAdmissionGate = options.inputAdmissionGate,
               wireOutputDir = options.wireOutputDir,
               &wireOutputMutex, &savedWireNames](
        ServiceProvider::CollaborationContext& context,
        const RequestMessage& request) {
      const auto payload = request.getPayload();
      const std::string requestLabel(payload.begin(), payload.end());
      if (isCoreLossRequest(requestLabel)) {
        const auto objectIndex = coreLossObjectIndex(requestLabel);
        const auto expectedName = coreLossInputName(
            SOURCE_A, isCoreLossXorRequest(requestLabel), objectIndex);
        const auto expectedPayload = makeCoreLossPayload(objectIndex);
        const auto inputObservations = std::make_shared<ConsumerObservations>();
        CollaborationInputSpec input;
        input.slotId = "core-loss-object-" + std::to_string(objectIndex);
        input.producerRole = "/source";
        input.object.name = expectedName;
        input.object.producerName = SOURCE_A;
        input.object.serviceName = SERVICE_NAME;
        input.object.sizeBytes = expectedPayload.size();
        input.object.digest = bufferSha256(expectedPayload);
        input.keyScope = "input";
        input.encoding = InputObjectEncoding::CollaborationLarge;
        input.digestKind = InputDigestKind::PlaintextSha256;
        input.maxBytes = 1024 * 1024;
        input.maxPackets = 64;
        const auto resolve = [](const CollaborationInputSpec& spec,
                                const ndn::Data&) {
          DependencyPacketPlan plan;
          plan.rootName = spec.object.name;
          plan.producer = spec.object.producerName;
          plan.maxWireBytes = spec.maxBytes;
          plan.maxPackets = spec.maxPackets;
          return plan;
        };
        const auto validate = [&, expectedName, expectedPayload, objectIndex](
            const std::vector<VerifiedInputObject>& objects) {
          inputObservations->validatorCount.fetch_add(1);
          if (objects.size() != 1 || objects.front().name() != expectedName ||
              objects.front().producer() != SOURCE_A ||
              objects.front().content() != expectedPayload ||
              objects.front().packets().empty()) {
            throw std::runtime_error("Core-loss admitted object mismatch");
          }
          for (const auto& packet : objects.front().packets()) {
            saveOriginalWire(wireOutputDir, *packet,
                             wireOutputMutex, savedWireNames);
          }
          std::cout << "SPEC226_CORE_LOSS_VALIDATED row=" << requestLabel
                    << " index=" << objectIndex
                    << " name=" << expectedName.toUri()
                    << " bytes=" << objects.front().content().size()
                    << " sourcePackets=" << objects.front().packets().size()
                    << " wireSha256="
                    << wireSha256(*objects.front().packets().front())
                    << std::endl;
        };
        auto operation = context.startInputAdmission({input}, validate, resolve);
        auto result = operation.get();
        const bool ready = result.status == InputAdmissionStatus::Ready &&
          result.inputs && result.inputs->isCurrent();
        const auto admittedPackets = ready ?
          result.inputs->objects().front().packets().size() : 0;
        const auto computeCount = ready ? inputObservations->computeCount.fetch_add(1) + 1 : 0;
        std::cout << "SPEC226_CORE_LOSS_ADMISSION row=" << requestLabel
                  << " index=" << objectIndex
                  << " status=" << (ready ? "READY" : "REJECTED")
                  << " reason=" << (result.reasonCode.empty() ? "none" : result.reasonCode)
                  << " originalInterests=" << result.metrics.originalInterests
                  << " originalRetries=" << result.metrics.originalRetries
                  << " repairInterests=" << result.metrics.repairInterests
                  << " fecRecoveredPackets=" << result.metrics.fecRecoveredPackets
                  << " admittedPackets=" << admittedPackets
                  << " computeCount=" << computeCount << std::endl;
        if (ready) {
          context.publishFinalResponse(ndn::Buffer{10});
        }
        else {
          context.publishFinalFailureResponse({}, result.reasonCode.empty()
            ? "INPUT_ADMISSION_REJECTED" : result.reasonCode);
        }
        std::cout << "SPEC226_CORE_LOSS_CONSUMER_DONE row=" << requestLabel
                  << " index=" << objectIndex << std::endl;
        return;
      }
      if (requestLabel != "q1-cold" && requestLabel != "q1-warm" &&
          requestLabel != "q1-missing") {
        throw std::runtime_error("Spec226 Q1 consumer received an unknown row");
      }
      const auto version = requestLabel == "q1-missing" ? 2 : 1;
      const auto rootA = inputName(SOURCE_A, version);
      const auto rootB = inputName(SOURCE_B, version);
      const auto observations = std::make_shared<ConsumerObservations>();
      const auto makeInput = [](const ndn::Name& name,
                                const ndn::Name& producer,
                                const std::string& slot) {
        CollaborationInputSpec input;
        input.slotId = slot;
        input.producerRole = "/source";
        input.object.name = name;
        input.object.producerName = producer;
        input.object.serviceName = SERVICE_NAME;
        input.keyScope = "input";
        input.encoding = InputObjectEncoding::SignedExactPackets;
        input.digestKind = InputDigestKind::OrderedWireSha256;
        input.maxBytes = 4096;
        input.maxPackets = 1;
        return input;
      };
      const std::vector<CollaborationInputSpec> inputs{
        makeInput(rootB, SOURCE_B, "source-b"),
        makeInput(rootA, SOURCE_A, "source-a"),
      };
      const auto resolve = [observations, requestLabel, inputAdmissionGate](
          const CollaborationInputSpec& input, const ndn::Data& authenticated) {
        if (authenticated.getName() != input.object.name) {
          throw std::runtime_error("Spec226 Q1 authenticated name mismatch");
        }
        const auto sequence = observations->resolverCount.fetch_add(1) + 1;
        std::cout << "SPEC226_Q1_INPUT_RESOLVED row=" << requestLabel
                  << " sequence=" << sequence
                  << " slot=" << input.slotId
                  << " name=" << authenticated.getName().toUri()
                  << " producer=" << input.object.producerName.toUri()
                  << " resolvedAtUs=" << steadyMicros()
                  << " wireSha256=" << wireSha256(authenticated) << std::endl;
        if (!inputAdmissionGate.empty() && requestLabel == "q1-cold" &&
            input.slotId == "source-b") {
          std::cout << "SPEC226_Q1_INPUT_READY row=q1-cold after=source-b"
                    << std::endl;
          const auto gate = std::filesystem::path(inputAdmissionGate);
          const auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(10);
          while (!std::filesystem::exists(gate) &&
                 std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
          if (!std::filesystem::exists(gate)) {
            throw std::runtime_error("Spec226 Q1 input admission gate timed out");
          }
        }
        DependencyPacketPlan plan;
        plan.rootName = input.object.name;
        plan.producer = input.object.producerName;
        plan.packets = {{input.object.name, {}, {}}};
        plan.maxWireBytes = input.maxBytes;
        plan.maxPackets = input.maxPackets;
        return plan;
      };
      const auto validate = [observations, rootA, rootB, requestLabel](
          const std::vector<VerifiedInputObject>& objects) {
        observations->validatorCount.fetch_add(1);
        if (objects.size() != 2) {
          throw std::runtime_error("Spec226 Q1 dependency set is incomplete");
        }
        const auto hasValue = [&objects](const ndn::Name& name, uint8_t value) {
          return std::any_of(objects.begin(), objects.end(),
              [&name, value](const VerifiedInputObject& object) {
            return object.name() == name && object.packets().size() == 1 &&
              object.packets().front()->getContent().value_size() == 1 &&
              object.packets().front()->getContent().value()[0] == value;
          });
        };
        if (!hasValue(rootA, 3) || !hasValue(rootB, 7)) {
          throw std::runtime_error("Spec226 Q1 dependency payload mismatch");
        }
        const std::array<std::string, 2> slots{"source-b", "source-a"};
        for (size_t i = 0; i < objects.size(); ++i) {
          if (objects[i].packets().size() != 1) {
            throw std::runtime_error("Spec226 Q1 admitted packet count mismatch");
          }
          const auto& packet = *objects[i].packets().front();
          const auto& content = packet.getContent();
          if (content.value_size() != 1) {
            throw std::runtime_error("Spec226 Q1 admitted payload size mismatch");
          }
          std::cout << "SPEC226_Q1_VALIDATED_OBJECT row=" << requestLabel
                    << " slot=" << slots[i]
                    << " name=" << objects[i].name().toUri()
                    << " producer=" << objects[i].producer().toUri()
                    << " contentByte=" << static_cast<unsigned>(content.value()[0])
                    << " wireSha256=" << wireSha256(packet)
                    << std::endl;
        }
      };

      std::optional<AdmittedInputSet> admitted;
      std::string reason = "none";
      try {
        admitted.emplace(context.requireInputs(inputs, validate, resolve));
      }
      catch (const std::exception& error) {
        reason = error.what();
      }
      const auto ready = admitted && admitted->isCurrent();
      if (!ready && reason == "none") {
        reason = "INPUT_AUTHORITY_EXPIRED";
      }
      std::cout << "SPEC226_Q1_ADMISSION status="
                << (ready ? "READY" : "REJECTED")
                << " row=" << requestLabel
                << " reason=" << reason
                << " resolverCount=" << observations->resolverCount.load()
                << " validatorCount=" << observations->validatorCount.load()
                << std::endl;
      if (ready) {
        const auto& sourceA = admitted->at("source-a");
        const auto& sourceB = admitted->at("source-b");
        const auto& sourceAPacket = sourceA.packets().front();
        const auto& sourceBPacket = sourceB.packets().front();
        const auto& sourceAContent = sourceAPacket->getContent();
        const auto& sourceBContent = sourceBPacket->getContent();
        if (sourceAContent.value_size() != 1 || sourceBContent.value_size() != 1) {
          throw std::runtime_error("Spec226 Q1 admitted slot content mismatch");
        }
        const auto result = static_cast<uint8_t>(sourceAContent.value()[0] +
                                                 sourceBContent.value()[0]);
        const auto count = observations->computeCount.fetch_add(1) + 1;
        std::cout << "SPEC226_Q1_COMPUTE row=" << requestLabel
                  << " count=" << count
                  << " result=" << static_cast<unsigned>(result) << std::endl;
        context.publishFinalResponse(ndn::Buffer{result});
      }
      else {
        std::cout << "SPEC226_Q1_COMPUTE row=" << requestLabel
                  << " count=0 result=none" << std::endl;
        context.publishFinalFailureResponse({}, reason);
      }
      std::cout << "SPEC226_Q1_CONSUMER_DONE row=" << requestLabel << std::endl;
    };
  }

  auto registration = provider.addScopedCollaborationHandler(
      SERVICE_NAME, {"/source", "/consumer"}, ackHandler,
      std::move(handler), limits);
  provider.init();
  provider.fetchPermissionsFromController(CONTROLLER_PREFIX);

  const auto deadline = std::chrono::steady_clock::now() + BOOTSTRAP_TIMEOUT;
  while (std::chrono::steady_clock::now() < deadline &&
         (!provider.hasProviderPermissionForService(SERVICE_NAME) ||
          !provider.isRequestDecryptionReady())) {
    face.processEvents(ndn::time::milliseconds(100));
  }
  if (!provider.hasProviderPermissionForService(SERVICE_NAME) ||
      !provider.isRequestDecryptionReady()) {
    throw std::runtime_error("Spec226 Q1 Provider authorization bootstrap timed out");
  }
  std::cout << "SPEC226_Q1_ACTOR_READY role=" << role
            << " identity=" << identity.toUri() << std::endl;
  if (options.shutdownFile.empty()) {
    face.processEvents();
  }
  else {
    while (!std::filesystem::exists(options.shutdownFile)) {
      face.processEvents(ndn::time::milliseconds(100));
    }
    std::cout << "SPEC226_Q1_ACTOR_SHUTDOWN role=" << role << std::endl;
  }
}

int
runUser(const ActorOptions& options, ndn::Face& face,
        ndn::KeyChain& keyChain,
        const ndn::security::Certificate& controllerCert)
{
  const auto userCert = getCertificate(keyChain, USER);
  CertificatePublisher certificatePublisher(face, keyChain, userCert.getName());
  ServiceUser user(face, GROUP_PREFIX, userCert, userCert, controllerCert,
                   options.trustSchema, keyChain);
  user.init();
  user.fetchPermissionsFromController(CONTROLLER_PREFIX);

  const std::vector<ndn::Name> providers{SOURCE_A, SOURCE_B, CONSUMER};
  const auto bootstrapDeadline = std::chrono::steady_clock::now() + BOOTSTRAP_TIMEOUT;
  auto nextRefresh = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < bootstrapDeadline &&
         !user.isRequestBootstrapReady(SERVICE_NAME, providers)) {
    face.processEvents(ndn::time::milliseconds(100));
    if (std::chrono::steady_clock::now() >= nextRefresh) {
      user.fetchPermissionsFromController(CONTROLLER_PREFIX);
      nextRefresh = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }
  }
  if (!user.isRequestBootstrapReady(SERVICE_NAME, providers)) {
    std::cerr << "SPEC226_Q1_USER_BOOTSTRAP_FAIL" << std::endl;
    return 2;
  }
  std::cout << "SPEC226_Q1_USER_READY identity=" << USER.toUri() << std::endl;

  struct RequestState
  {
    std::atomic<bool> done{false};
    std::atomic<bool> committed{false};
    std::atomic<size_t> responses{0};
    std::atomic<bool> success{false};
    std::string error;
  };
  const auto& requestLabel = options.requestRow;
  const bool coreLossRequest = isCoreLossRequest(requestLabel);
  const bool expectSuccess = coreLossRequest || requestLabel != "q1-missing";
  CollaborationPlan plan;
  plan.ackCollectionTimeMs = 1500;
  plan.timeoutMs = static_cast<int>(std::chrono::duration_cast<
      std::chrono::milliseconds>(REQUEST_TIMEOUT).count());
  CollaborationRoleSpec sourceRole;
  sourceRole.role = "/source";
  sourceRole.service = SERVICE_NAME;
  sourceRole.minProviders = coreLossRequest ? 1 : 2;
  sourceRole.maxProviders = coreLossRequest ? 1 : 2;
  sourceRole.inputAdmissionProfile = "data-admission-v1";
  CollaborationRoleSpec consumerRole;
  consumerRole.role = "/consumer";
  consumerRole.service = SERVICE_NAME;
  consumerRole.terminalResponseOwner = true;
  consumerRole.inputAdmissionProfile = "data-admission-v1";
  plan.roles = {sourceRole, consumerRole};
  plan.keyScopes.push_back({"input", {"/source", "/consumer"}});
  plan.participantSelector = std::make_shared<Q1ParticipantSelector>();
  if (isCoreLossXorRequest(requestLabel)) {
    plan.dependencyTransport.redundancy = DependencyRedundancy::Xor;
  }
  if (coreLossRequest) {
    plan.ackCollectionTimeMs = 300;
  }

  auto state = std::make_shared<RequestState>();
  ndn::Buffer requestPayload(requestLabel.size());
  std::copy(requestLabel.begin(), requestLabel.end(), requestPayload.begin());
  const auto requestId = user.BeginCollaborationWithProviders(
      SERVICE_NAME, requestPayload, plan.ackCollectionTimeMs,
      plan.timeoutMs,
      [&user, state, plan = std::move(plan)](
          const CollaborationAckClosure& closed) mutable {
        try {
          const bool committed = user.CommitCollaborationPlan(
              closed.requestId, closed.digest, std::move(plan));
          state->committed.store(committed);
          if (!committed) {
            state->error = "COLLABORATION_PLAN_REJECTED";
            state->done.store(true);
          }
        }
        catch (const std::exception& error) {
          state->error = error.what();
          state->done.store(true);
        }
      },
      [state](const ResponseMessage& response) {
        state->responses.fetch_add(1);
        state->success.store(response.getStatus() &&
                             response.getPayload() == ndn::Buffer{10});
        state->error = response.getErrorInfo();
        state->done.store(true);
      },
      [state](const ndn::Name&) {
        state->error = "REQUEST_TIMEOUT";
        state->done.store(true);
      },
      {}, {}, RequestCapabilities(), std::nullopt, {}, {}, {}, providers);
  if (requestId.empty()) {
    throw std::runtime_error("Spec226 Q1 request did not start");
  }
  std::cout << "SPEC226_Q1_REQUEST_STARTED row=" << requestLabel
            << " requestId=" << requestId.toUri() << std::endl;

  const auto deadline = std::chrono::steady_clock::now() + REQUEST_TIMEOUT +
    std::chrono::seconds(3);
  while (!state->done.load() && std::chrono::steady_clock::now() < deadline) {
    face.processEvents(ndn::time::milliseconds(100));
  }
  const bool expectedOutcome = state->responses.load() == 1 &&
    state->success.load() == expectSuccess &&
    (!expectSuccess || state->error.empty());
  std::cout << "SPEC226_Q1_RESULT row=" << requestLabel
            << " success=" << (state->success.load() ? "true" : "false")
            << " responses=" << state->responses.load()
            << " committed=" << (state->committed.load() ? "true" : "false")
            << " expectedOutcome=" << (expectedOutcome ? "true" : "false")
            << " error=" << (state->error.empty() ? "none" : state->error)
            << std::endl;
  return expectedOutcome && state->committed.load() ? 0 : 1;
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    const auto options = parseOptions(argc, argv);
    ndn::Face face;
    ndn::KeyChain keyChain;
    TrustSchemaValidator validator(face);
    loadTrustSchema(validator, options.trustSchema);
    const auto controllerCert = getCertificate(keyChain, CONTROLLER_PREFIX);
    if (options.mode == "user") {
      return runUser(options, face, keyChain, controllerCert);
    }
    runProvider(options, face, keyChain, controllerCert);
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "SPEC226_Q1_ACTOR_FAIL error=" << error.what() << std::endl;
    return 1;
  }
}
