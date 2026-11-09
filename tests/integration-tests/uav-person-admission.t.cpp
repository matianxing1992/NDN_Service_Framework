#include "tests/boost-test.hpp"
#include "tests/fixtures/uav-person-window.hpp"
#include "ndnsf-integration-fixture.hpp"

#include "NDNSF-UAV-APP/shared/UavNames.hpp"
#include "NDNSF-UAV-APP/shared/UavProtocol.hpp"
#include "NDNSF-UAV-APP/shared/UavTrackingResultVerifier.hpp"
#include "NDNSF-UAV-APP/shared/UavTrackingWorkerTransaction.hpp"
#include "NDNSF-UAV-APP/shared/uav-person-computation.hpp"
#include "NDNSF-UAV-APP/shared/uav-tracking-worker-process.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include "ndn-service-framework/utils.hpp"

#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <ndn-svs/sync-protocol.hpp>
#include <ndn-svs/tlv.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::test {
namespace {

namespace nsf = ndn_service_framework;
using namespace examples::uav;
using namespace person_fixture;
using namespace std::chrono_literals;

struct TemporaryDirectory
{
  std::filesystem::path path;

  TemporaryDirectory()
  {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "ndnsf-person-admission-XXXXXX").string();
    const char* created = ::mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("cannot create person admission fixture directory");
    path = created;
  }

  ~TemporaryDirectory()
  {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

ndn::Buffer
bufferOf(const std::string& text)
{
  return ndn::Buffer(text.begin(), text.end());
}

bool
isOneBitDamage(const ndn::Block& original, const ndn::Block& changed)
{
  if (original.size() != changed.size()) return false;
  size_t changedBytes = 0;
  for (size_t i = 0; i < original.size(); ++i) {
    const auto difference = original.data()[i] ^ changed.data()[i];
    if (difference != 0) {
      if (difference != 1) return false;
      ++changedBytes;
    }
  }
  return changedBytes == 1;
}

std::string
readBounded(const std::filesystem::path& path, size_t bound)
{
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read person admission fixture output");
  std::string result(bound + 1, '\0');
  stream.read(result.data(), static_cast<std::streamsize>(result.size()));
  result.resize(static_cast<size_t>(stream.gcount()));
  if (result.size() > bound || (!stream.eof() && stream.fail())) {
    throw std::runtime_error("person admission fixture output exceeds bound or read failed");
  }
  return result;
}

UavPersonTrackingProfile
cpuProfile()
{
  auto wire = Json::parse(std::string(PERSON_PROFILE_GOLDEN));
  wire["requestedDevice"] = "cpu";
  return parseUavPersonTrackingProfile(wire.dump());
}

UavPersonComputationManifest
fixtureManifest(const UavPersonTrackingProfile& profile)
{
  // Synthetic inventory declarations; no model or installed-artifact qualification.
  std::map<std::string, std::string> backend;
  for (const auto key : {"tracking_worker.py", "person_reid.py", "person_profile.py",
                        "observation_schema.py", "motion_schema.py", "motion_estimator.py",
                        "byte_track_state.py", "person_runtime.py", "requirements-person.lock",
                        "bytetrack.yaml", "runtime/fingerprint", "runtime/torchreid-source"}) {
    backend.emplace(key, "sha256:" + std::string(64, 'a'));
  }
  return makeUavPersonComputationManifest(profile, backend,
    {{"executable:/fixture/UavTrackingNode", "sha256:" + std::string(64, 'b')}});
}

class SingleRoleSelection final : public nsf::ParticipantSelectionPolicy
{
public:
  std::vector<nsf::SelectedParticipant>
  select(const std::vector<nsf::AckCandidate>& candidates,
         const std::vector<nsf::CollaborationRoleSpec>& roles) const override
  {
    if (candidates.size() != 1 || roles.size() != 1 || !candidates[0].ack.getStatus()) return {};
    const auto& candidate = candidates[0];
    return {{roles[0].role, candidate.serviceName, candidate.providerName, {}, false, 0,
             roles[0].assignmentPayload, candidate, {}}};
  }
};

struct AdmissionState
{
  TemporaryDirectory temporary;
  UavPersonTrackingProfile profile = cpuProfile();
  UavPersonComputationManifest manifest = fixtureManifest(profile);
  UavPersonWindowContext context = windowContext();
  UavTrackingWorkerTransaction transactions{temporary.path / "transactions"};
  std::filesystem::path input = temporary.path / "input.json";
  std::filesystem::path output = temporary.path / "output.json";
  std::atomic<unsigned> handlerCalls{0};
  std::atomic<unsigned> contextAdmissions{0};
  std::atomic<unsigned> scopedAdmissions{0};
  std::atomic<unsigned> bindingRefusals{0};
  std::atomic<unsigned> executionCalls{0};
  std::atomic<unsigned> childSuccesses{0};
  std::atomic<unsigned> commitCalls{0};
  std::atomic<unsigned> requestEnvelopes{0};
  std::atomic<unsigned> invalidEnvelopes{0};
  std::atomic<unsigned> ackClosures{0};
  std::atomic<size_t> ackCandidates{0};
  std::atomic<unsigned> responseCallbacks{0};
  std::atomic<unsigned> validOriginalTransports{0};
  std::atomic<unsigned> invalidOriginalTransports{0};
  std::atomic<unsigned> validWrongSignerEnvelopes{0};
  std::atomic<unsigned> invalidWrongSignerEnvelopes{0};
  std::atomic<unsigned> validDamagedInnerTransports{0};
  std::atomic<unsigned> invalidDamagedInnerTransports{0};
  std::atomic<size_t> requestInnerWireBytes{0};
  std::atomic<bool> completed{false};
  std::atomic<bool> timedOut{false};
  std::atomic<bool> responseStatus{false};
  std::mutex mutex;
  ndn::Buffer originalEnvelope;
  ndn::Buffer originalInnerWire;
  ndn::Buffer observedRequestWire;
  ndn::Buffer validatedRequestWire;
  std::atomic<unsigned> observedPiggyRequests{0};
  std::atomic<unsigned> observedRetrievedRequests{0};
  std::atomic<unsigned> invalidTransportObservations{0};
  std::atomic<unsigned> validPiggyMutations{0};
  std::atomic<unsigned> invalidPiggyMutations{0};
  ndn::signal::ScopedConnection requestInterestObservation;
  ndn::signal::ScopedConnection requestDataObservation;
  std::string unexpectedError;
  std::string refusal;
  std::string responsePayload;
  std::string responseError;

  explicit AdmissionState(const std::string& label)
  {
    context.runId = "native-person-admission";
    context.missionId = label;
    context.windowId = "window-0";
    context.computationDigest = manifest.computationDigest;
  }

  std::string
  execute(nsf::ServiceProvider::CollaborationContext& authority, const ndn::Name& producer)
  {
    auto wire = backendFixture();
    wire["profileDigest"] = profile.profileDigest;
    wire["runId"] = context.runId;
    wire["missionId"] = context.missionId;
    wire["windowId"] = context.windowId;
    wire["computationDigest"] = context.computationDigest;
    for (const auto key : {"requestedDevice", "actualDetectorDevice", "detectorModelDevice",
                          "detectorInputDevice", "actualReidDevice", "reidModelDevice",
                          "reidInputDevice"}) {
      wire[key] = "cpu";
    }
    {
      std::ofstream stream(input, std::ios::binary);
      stream << wire.dump();
      stream.flush();
      if (!stream) throw std::runtime_error("cannot write literal native backend input");
    }
    ++executionCalls;
    const auto child = runUavTrackingWorkerProcess(
      {"/bin/cp", "--", input.string(), output.string()}, 2s, 4096,
      [&authority] { return authority.hasCurrentWorkAuthority(); });
    if (child.outcome != UavTrackingWorkerOutcome::SUCCESS || child.exitCode != 0) {
      throw std::runtime_error("native admission child did not succeed");
    }
    ++childSuccesses;
    const auto backend = parseUavPersonBackendWindow(
      readBounded(output, profile.limits.at("maxResultBytes")), context, profile);
    UavPersonTrackingEngine engine(profile);
    const auto decision = engine.processWindow(context, backend);
    std::array<std::string, 3> names;
    std::array<std::string, 3> digests;
    UavTrackingResultExpectation expected;
    expected.runId = context.runId;
    expected.missionId = context.missionId;
    expected.windowId = context.windowId;
    expected.sequence = context.sequence;
    expected.sourceEpoch = 1;
    expected.computeProvider = producer;
    expected.personTracking = UavPersonTrackingResultExpectation{profile, manifest.computationDigest};
    for (size_t i = 0; i < context.frames.size(); ++i) {
      const auto& frame = context.frames[i];
      names[i] = makeUavObservationDataName(droneIdentity(frame.cameraId), frame.cameraId,
        frame.sourceEpoch, frame.sequence, "OBSERVATION", 1).toUri();
      digests[i] = "sha256:" + std::string(64, 'c');
      expected.sourceIds[i] = frame.sourceId;
      expected.inputDigests[i] = frame.inputDigest;
    }
    auto result = makeUavPersonTrackingResultJson(decision, backend, profile, names, digests);
    const auto name = makeUavMultiViewResultName(producer, context.missionId,
      context.windowId, context.sequence, 1);
    validateUavTrackingResultContext(name.toUri(), producer.toUri(), result, expected);
    ++commitCalls;
    if (!transactions.commit(context.windowId, context.computationDigest, bufferOf(result),
        bufferOf(decision.candidateStateJson),
        [&authority] { return authority.hasCurrentWorkAuthority(); })) {
      throw std::runtime_error("native admission transaction lost authority");
    }
    return result;
  }
};

enum class RequestSignatureFault
{
  NONE,
  DAMAGED_TRANSPORT,
  WRONG_INNER_SIGNER,
  DAMAGED_INNER
};

void
checkAdmissionCase(const std::string& label, const std::string& wrongKey,
                   RequestSignatureFault signatureFault = RequestSignatureFault::NONE,
                   bool productionTransport = false)
{
  const bool damageSignature = signatureFault != RequestSignatureFault::NONE;
  const bool wrongSigner = signatureFault == RequestSignatureFault::WRONG_INNER_SIGNER;
  const bool damagedInner = signatureFault == RequestSignatureFault::DAMAGED_INNER;
  // Captured owners outlive environment teardown, including an early assertion failure.
  AdmissionState state(label);
  nsf::test::BootstrapProfile config;
  config.groupPrefix = ndn::Name("/example/uav");
  config.syncPrefix = ndn::Name("/example/uav/sync");
  config.userIdentity = ndn::Name("/example/uav/gs");
  config.providerIdentity = ndn::Name("/example/uav/compute");
  config.userNode = ndn::Name(config.userIdentity).append("user");
  config.providerNode = ndn::Name(config.providerIdentity).append("provider");
  config.serviceName = SERVICE_TRACKING_WINDOW;
  config.providerRoles = {"Tracking-Compute"};
  config.validateSvsPackets = true;
  config.useProductionSvsTransportOptions = productionTransport;
  config.deferBridgeDelivery = true;
  if (productionTransport) {
    for (const auto* key : {"NDNSF_SVS_PROTOCOL_VERSION", "NDNSF_SVS_MAX_APP_PARAMS_BYTES",
                            "NDNSF_SVS_MAX_PIGGYDATA_BYTES", "NDNSF_SVS_PERIODIC_SYNC_MS",
                            "NDNSF_SVS_MAX_SUPPRESSION_MS"}) {
      BOOST_REQUIRE_MESSAGE(std::getenv(key) == nullptr, "Unexpected SVS override: " << key);
    }
    BOOST_REQUIRE(wrongKey.empty());
  }
  const auto requestId = ndn::Name("/spec219/admission").append(label).appendVersion(1);
  const auto requestName = nsf::makeRequestNameV2(config.userIdentity, config.serviceName, requestId);
  auto fields = makeUavPersonRequestBindingFields(state.profile, state.manifest);
  if (!wrongKey.empty()) fields.at(wrongKey) = "sha256:" + std::string(64, 'f');
  const auto payload = bufferOf(encodeFields(fields));
  nsf::RequestCapabilities capabilities;
  capabilities.setField("RequestScopedConfidentialityV1", "required");
  nsf::CollaborationPlan plan;
  plan.ackCollectionTimeMs = 1000;
  plan.timeoutMs = 10000;
  nsf::CollaborationRoleSpec role;
  role.role = "Tracking-Compute";
  role.service = config.serviceName;
  role.assignmentPayload = payload;
  role.terminalResponseOwner = true;
  plan.roles.push_back(std::move(role));
  plan.participantSelector = std::make_shared<SingleRoleSelection>();
  nsf::PolicyStatusData status;
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  status.setServiceName(config.serviceName);
  status.setControllerVersion(nsf::ControllerVersion{now, 1});
  status.setValidity(now - 1000, now + 120000);
  status.setPolicyDigest("sha256:" + std::string(64, '0'));
  nsf::test::NdnsfIntegrationEnvironment environment(config);
  environment.bootstrap();
  auto& user = environment.user();
  auto& provider = environment.provider();
  user.setUseTokens(false);
  provider.setUseTokens(false);
  // Trusted fixture setup only; this does not establish controller network delivery.
  const auto controller = environment.createTestIdentityCertificate(
    ndn::Name(config.groupPrefix).append("controller"));
  status.setControllerCertificate(controller.getName());
  status.setAbePublicParametersName(environment.attributeAuthorityPublicParametersName());
  status.setAbePublicParametersDigest(environment.attributeAuthorityPublicParametersDigest());
  BOOST_REQUIRE(user.installControllerStatus(status));
  BOOST_REQUIRE(provider.installControllerStatus(status));
  auto registration = provider.addScopedCollaborationHandler(config.serviceName,
    {"Tracking-Compute"},
    nsf::ServiceProvider::AckStrategyHandler([] (const nsf::RequestMessage&) {
      nsf::ServiceProvider::AckDecision decision;
      decision.status = true;
      return decision;
    }),
    [&] (nsf::ServiceProvider::CollaborationContext& authority, const nsf::RequestMessage& request) {
      ++state.handlerCalls;
      try {
        if (authority.role() != "Tracking-Compute" ||
            authority.requesterName() != config.userIdentity ||
            authority.sessionId() != requestId.toUri() || !authority.hasCurrentWorkAuthority()) {
          throw std::runtime_error("native collaboration context was not admitted");
        }
        ++state.contextAdmissions;
        if (!request.hasRequestCapabilities() ||
            request.getRequestCapabilities().getField("RequestScopedConfidentialityV1") !=
              "required" || !request.hasControllerVersion() ||
            !request.getControllerVersion().isValid() || !request.hasUserEncryptionCertificate()) {
          throw std::runtime_error("person admission request lacks scoped confidentiality");
        }
        ++state.scopedAdmissions;
        const auto bytes = request.getPayload();
        const auto received = decodeFields(std::string(bytes.begin(), bytes.end()));
        try {
          requireUavPersonRequestBindingFields(received, state.profile, state.manifest);
        }
        catch (const std::invalid_argument& error) {
          ++state.bindingRefusals;
          {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.refusal = error.what();
          }
          authority.publishFinalFailureResponse(bufferOf(error.what()), error.what());
          return;
        }
        authority.publishFinalResponse(bufferOf(state.execute(authority, config.providerIdentity)));
      }
      catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.unexpectedError = error.what();
        authority.fail(error.what());
      }
    });
  const auto userCertificate = environment.keyChain().getPib().getIdentity(config.userIdentity)
    .getDefaultKey().getDefaultCertificate();
  const auto providerCertificate = environment.keyChain().getPib().getIdentity(config.providerIdentity)
    .getDefaultKey().getDefaultCertificate();
  if (productionTransport) {
    BOOST_REQUIRE_EQUAL(environment.userPubSub().getMaxPiggyDataSize(), 4096);
    BOOST_REQUIRE_EQUAL(environment.providerPubSub().getMaxPiggyDataSize(), 4096);
    const auto version = environment.userPubSub().getSyncProtocolOptions().version;
    BOOST_REQUIRE(version == ndn::svs::SvsProtocolVersion::V3);
    BOOST_REQUIRE(environment.providerPubSub().getSyncProtocolOptions().version == version);
  }
  if (productionTransport && !damageSignature) {
    const auto version = environment.userPubSub().getSyncProtocolOptions().version;
    const auto observe = [&state, &requestName, userCertificate] (
      const ndn::Data& inner, bool piggy, bool validCarrier) {
      if (inner.getName() != requestName) return;
      const auto& wire = inner.wireEncode();
      if (wire.size() > 65536 || !validCarrier ||
          !ndn::security::verifySignature(inner, userCertificate)) {
        ++state.invalidTransportObservations;
        return;
      }
      std::lock_guard<std::mutex> lock(state.mutex);
      const ndn::Buffer bytes(wire.begin(), wire.end());
      if (state.observedRequestWire.empty()) state.observedRequestWire = bytes;
      else if (state.observedRequestWire != bytes) ++state.invalidTransportObservations;
      if (piggy) ++state.observedPiggyRequests;
      else ++state.observedRetrievedRequests;
    };
    state.requestInterestObservation = environment.userFace().onSendInterest.connect(
      [&state, observe, version, syncPrefix = config.syncPrefix, userCertificate] (
        const ndn::Interest& interest) {
        const auto prefix = ndn::svs::SyncProtocolCodec::makeSyncName(syncPrefix, version);
        if (!prefix.isPrefixOf(interest.getName())) return;
        try {
          const auto decoded = ndn::svs::SyncProtocolCodec::decode(
            interest, syncPrefix, version, false);
          const bool validCarrier = decoded.stateVectorData &&
            ndn::security::verifySignature(*decoded.stateVectorData, userCertificate);
          for (auto extension : decoded.extensions) {
            if (extension.type() != ndn::svs::tlv::MappingData) continue;
            extension.parse();
            for (const auto& child : extension.elements()) {
              if (child.type() == ndn::tlv::Data) observe(ndn::Data(child), true, validCarrier);
            }
          }
        }
        catch (const std::exception&) {
          ++state.invalidTransportObservations;
        }
      });
    state.requestDataObservation = environment.userFace().onSendData.connect(
      [&state, observe, userCertificate] (const ndn::Data& outer) {
        if (outer.getContentType() != ndn::tlv::Data) return;
        try {
          observe(ndn::Data(outer.getContent().blockFromValue()), false,
                  ndn::security::verifySignature(outer, userCertificate));
        }
        catch (const std::exception&) {
          ++state.invalidTransportObservations;
        }
      });
  }
  const auto subscription = environment.providerPubSub().subscribeToProducer(config.userNode,
    [&, userCertificate, providerCertificate] (
      const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      if (publication.name != requestName) return;
      try {
        nsf::HybridMessageEnvelope envelope;
        if (!envelope.WireDecode(ndn::Block(publication.data)) ||
            envelope.getMessageType() != "REQUEST") {
          ++state.invalidEnvelopes;
          return;
        }
        ++state.requestEnvelopes;
        if (productionTransport && !damageSignature) {
          const auto& packet = publication.packet;
          if (!packet || packet->wireEncode().size() > 65536 ||
              publication.producerPrefix != ndn::Name(config.userNode).append("0") ||
              !ndn::security::verifySignature(*packet, userCertificate)) {
            ++state.invalidTransportObservations;
          }
          else {
            const auto& wire = packet->wireEncode();
            std::lock_guard<std::mutex> lock(state.mutex);
            const ndn::Buffer bytes(wire.begin(), wire.end());
            if (state.validatedRequestWire.empty()) state.validatedRequestWire = bytes;
            else if (state.validatedRequestWire != bytes) ++state.invalidTransportObservations;
          }
        }
        if (wrongSigner) {
          const auto& packet = publication.packet;
          bool unchangedPayload = false;
          {
            std::lock_guard<std::mutex> lock(state.mutex);
            unchangedPayload = state.originalEnvelope ==
              ndn::Buffer(publication.data.begin(), publication.data.end());
          }
          if (packet && packet->getName() == requestName &&
              packet->wireEncode().size() > 800 &&
              publication.producerPrefix == ndn::Name(config.userNode).append("0") &&
              ndn::security::verifySignature(*packet, providerCertificate) &&
              !ndn::security::verifySignature(*packet, userCertificate) && unchangedPayload) {
            ++state.validWrongSignerEnvelopes;
          }
          else {
            ++state.invalidWrongSignerEnvelopes;
          }
        }
      }
      catch (const std::exception&) {
        ++state.invalidEnvelopes;
      }
    }, false);
  environment.enableProductionIngressForTest();
  const auto decryptions = provider.getHybridCryptoCounters().symmetric_decrypt_count.load();
  const auto validationFailures = environment.providerValidationFailureCountForTest();
  std::optional<nsf::test::RequestScope> faultScope;
  if (damageSignature) {
    nsf::test::FaultProfile faults;
    faults.tamperSignatureDataCount = 16;
    faults.signatureDataFault = wrongSigner ? nsf::test::SignatureDataFault::INNER_PROVIDER_SIGNER :
      damagedInner ? nsf::test::SignatureDataFault::INNER_SIGNATURE :
                     nsf::test::SignatureDataFault::OUTER_SIGNATURE;
    if (damagedInner) {
      faults.inspectSignatureDataMutation = [&state, &requestName, userCertificate] (
        const ndn::Data& data) {
        try {
          const ndn::Data inner(data.getContent().blockFromValue());
          const auto& wire = inner.wireEncode();
          size_t changedBytes = 0;
          bool singleBitChange = true;
          {
            std::lock_guard<std::mutex> lock(state.mutex);
            singleBitChange = wire.size() == state.originalInnerWire.size();
            if (singleBitChange) {
              for (size_t i = 0; i < wire.size(); ++i) {
                const auto difference = wire.data()[i] ^ state.originalInnerWire[i];
                if (difference != 0) {
                  ++changedBytes;
                  singleBitChange = singleBitChange && difference == 1;
                }
              }
            }
          }
          if (inner.getName() == requestName && wire.size() > 800 &&
              changedBytes == 1 && singleBitChange &&
              ndn::security::verifySignature(data, userCertificate) &&
              !ndn::security::verifySignature(inner, userCertificate)) {
            ++state.validDamagedInnerTransports;
          }
          else {
            ++state.invalidDamagedInnerTransports;
          }
        }
        catch (const std::exception&) {
          ++state.invalidDamagedInnerTransports;
        }
      };
    }
    const auto observeOriginal = [&state, &requestName, userCertificate] (
      const ndn::Data& inner, bool validCarrier) {
      if (inner.getName() != requestName) return false;
      const auto& wire = inner.wireEncode();
      state.requestInnerWireBytes.store(wire.size());
      if (wire.size() > 65536) {
        ++state.invalidOriginalTransports;
        return true;
      }
      if (validCarrier && ndn::security::verifySignature(inner, userCertificate))
        ++state.validOriginalTransports;
      else
        ++state.invalidOriginalTransports;
      std::lock_guard<std::mutex> lock(state.mutex);
      const auto content = inner.getContent().value_bytes();
      const ndn::Buffer envelope(content.begin(), content.end());
      const ndn::Buffer original(wire.begin(), wire.end());
      if (state.originalInnerWire.empty()) state.originalInnerWire = original;
      else if (state.originalInnerWire != original) ++state.invalidOriginalTransports;
      if (state.originalEnvelope.empty()) state.originalEnvelope = envelope;
      else if (state.originalEnvelope != envelope) ++state.invalidOriginalTransports;
      return true;
    };
    faults.tamperSignatureDataPredicate = [observeOriginal, userCertificate] (
      const ndn::Data& data) {
      if (data.getContentType() != ndn::tlv::Data) return false;
      try {
        return observeOriginal(ndn::Data(data.getContent().blockFromValue()),
                               ndn::security::verifySignature(data, userCertificate));
      }
      catch (const std::exception&) {
        return false;
      }
    };
    if (productionTransport) {
      faults.tamperSignaturePiggyPredicate = [&requestName] (const ndn::Data& inner) {
        return inner.getName() == requestName;
      };
      faults.inspectSignaturePiggyMutation = [&, observeOriginal, userCertificate,
                                              providerCertificate] (
        const ndn::Interest& original, const ndn::Interest& changed) {
        try {
          const auto version = ndn::svs::SvsProtocolVersion::V3;
          // The maintained decoder rejects an invalid namespace or ParametersDigest.
          const auto before = ndn::svs::SyncProtocolCodec::decode(
            original, config.syncPrefix, version);
          const auto after = ndn::svs::SyncProtocolCodec::decode(
            changed, config.syncPrefix, version);
          bool valid = before.stateVectorData && after.stateVectorData &&
            before.stateVector.encode() == after.stateVector.encode() &&
            before.extensions.size() == after.extensions.size() &&
            original.hasNonce() == changed.hasNonce() &&
            (!original.hasNonce() || original.getNonce() == changed.getNonce()) &&
            original.getInterestLifetime() == changed.getInterestLifetime() &&
            original.getCanBePrefix() == changed.getCanBePrefix() &&
            original.getMustBeFresh() == changed.getMustBeFresh() &&
            ndn::security::verifySignature(*before.stateVectorData, userCertificate);
          size_t matched = 0;
          if (valid) {
            if (signatureFault == RequestSignatureFault::DAMAGED_TRANSPORT) {
              valid = isOneBitDamage(before.stateVectorData->wireEncode(),
                                     after.stateVectorData->wireEncode()) &&
                !ndn::security::verifySignature(*after.stateVectorData, userCertificate);
            }
            else {
              valid = ndn::security::verifySignature(*after.stateVectorData, userCertificate);
            }
            for (size_t i = 0; i < before.extensions.size(); ++i) {
              auto left = before.extensions[i];
              auto right = after.extensions[i];
              if (left.type() != ndn::svs::tlv::MappingData) {
                valid = valid && left == right;
                continue;
              }
              left.parse();
              right.parse();
              if (right.type() != left.type() || left.elements_size() != right.elements_size()) {
                valid = false;
                continue;
              }
              for (size_t j = 0; j < left.elements_size(); ++j) {
                const auto& a = left.elements()[j];
                const auto& b = right.elements()[j];
                if (a.type() != ndn::tlv::Data || ndn::Data(a).getName() != requestName) {
                  valid = valid && a == b;
                  continue;
                }
                ++matched;
                const ndn::Data innerBefore(a);
                const ndn::Data innerAfter(b);
                observeOriginal(innerBefore, true);
                valid = valid && innerBefore.getName() == innerAfter.getName() &&
                  innerBefore.getContent() == innerAfter.getContent() &&
                  innerBefore.wireEncode().size() <= 4096;
                if (wrongSigner) {
                  valid = valid && ndn::security::verifySignature(innerAfter, providerCertificate) &&
                    !ndn::security::verifySignature(innerAfter, userCertificate);
                }
                else if (damagedInner) {
                  valid = valid && isOneBitDamage(a, b) &&
                    !ndn::security::verifySignature(innerAfter, userCertificate);
                }
                else {
                  valid = valid && a == b;
                }
              }
            }
          }
          if (valid && matched == 1) {
            ++state.validPiggyMutations;
            if (damagedInner) ++state.validDamagedInnerTransports;
          }
          else {
            ++state.invalidPiggyMutations;
          }
        }
        catch (const std::exception&) {
          ++state.invalidPiggyMutations;
        }
      };
    }
    faultScope.emplace(environment.beginRequest(requestId.toUri(), std::move(faults)));
  }
  const auto started = user.BeginCollaboration(config.serviceName, payload,
    plan.ackCollectionTimeMs, plan.timeoutMs,
    [&user, &state, plan, damageSignature] (const nsf::CollaborationAckClosure& closure) {
      ++state.ackClosures;
      state.ackCandidates.fetch_add(closure.candidates.size());
      if (damageSignature) {
        state.completed.store(true);
        return;
      }
      if (!user.CommitCollaborationPlan(closure.requestId, closure.digest, plan)) {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.unexpectedError = "native person collaboration plan was not committed";
        state.completed.store(true);
      }
    },
    [&] (const nsf::ResponseMessage& response) {
      ++state.responseCallbacks;
      {
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto bytes = response.getPayload();
        state.responsePayload.assign(bytes.begin(), bytes.end());
        state.responseError = response.getErrorInfo();
      }
      state.responseStatus.store(response.getStatus());
      state.completed.store(true);
    },
    [&] (const ndn::Name&) { state.timedOut.store(true); state.completed.store(true); },
    requestId, nsf::CollaborationAckCoverageHandler{}, capabilities);
  BOOST_REQUIRE(started == requestId);
  if (faultScope) environment.markRequestPublished(*faultScope);
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (!state.completed.load() && std::chrono::steady_clock::now() < deadline) {
    environment.pumpUntilWithAttributeAuthority([&] { return state.completed.load(); });
  }
  BOOST_TEST_MESSAGE("person admission " << label << " envelopes=" << state.requestEnvelopes.load()
    << " handlers=" << state.handlerCalls.load() << " executions=" << state.executionCalls.load());
  BOOST_REQUIRE(state.completed.load());
  BOOST_CHECK(!state.timedOut.load());
  BOOST_CHECK_EQUAL(state.ackClosures.load(), 1);
  if (damageSignature) {
    const auto checkMutationStats = [&] {
      const auto current = environment.bridgeStats();
      if (productionTransport) {
        BOOST_CHECK_GT(current.tamperedSignaturePiggyPackets, 0);
        BOOST_CHECK_EQUAL(state.validPiggyMutations.load(), current.tamperedSignaturePiggyPackets);
        BOOST_CHECK_EQUAL(state.invalidPiggyMutations.load(), 0);
        BOOST_CHECK_LE(state.requestInnerWireBytes.load(), 4096);
      }
      else {
        BOOST_CHECK_GT(current.tamperedSignatureDataPackets, 0);
      }
      BOOST_CHECK_LE(current.tamperedSignatureDataPackets + current.tamperedSignaturePiggyPackets,
                     16);
      BOOST_CHECK_EQUAL(current.signatureFaultBudgetDrops, 0);
    };
    checkMutationStats();
    BOOST_CHECK_GT(state.validOriginalTransports.load(), 0);
    BOOST_CHECK_EQUAL(state.invalidOriginalTransports.load(), 0);
    // The older cases use 800 bytes; production-budget cases independently prove piggyback.
    BOOST_CHECK_GT(state.requestInnerWireBytes.load(), 800);
    if (wrongSigner) {
      BOOST_CHECK_GT(state.requestEnvelopes.load(), 0);
      BOOST_CHECK_GT(state.validWrongSignerEnvelopes.load(), 0);
      BOOST_CHECK_EQUAL(state.invalidWrongSignerEnvelopes.load(), 0);
    }
    else {
      BOOST_CHECK_GT(environment.providerValidationFailureCountForTest(), validationFailures);
      BOOST_CHECK_EQUAL(state.requestEnvelopes.load(), 0);
    }
    if (damagedInner) {
      BOOST_CHECK_GT(state.validDamagedInnerTransports.load(), 0);
      BOOST_CHECK_EQUAL(state.invalidDamagedInnerTransports.load(), 0);
    }
    BOOST_CHECK_EQUAL(state.invalidEnvelopes.load(), 0);
    BOOST_CHECK_EQUAL(provider.getHybridCryptoCounters().symmetric_decrypt_count.load(),
                      decryptions);
    BOOST_CHECK_EQUAL(state.ackCandidates.load(), 0);
    BOOST_CHECK_EQUAL(state.handlerCalls.load(), 0);
    BOOST_CHECK_EQUAL(state.contextAdmissions.load(), 0);
    BOOST_CHECK_EQUAL(state.scopedAdmissions.load(), 0);
    BOOST_CHECK_EQUAL(state.bindingRefusals.load(), 0);
    BOOST_CHECK_EQUAL(state.executionCalls.load(), 0);
    BOOST_CHECK_EQUAL(state.childSuccesses.load(), 0);
    BOOST_CHECK_EQUAL(state.commitCalls.load(), 0);
    BOOST_CHECK_EQUAL(state.responseCallbacks.load(), 0);
    BOOST_CHECK(!state.transactions.loadCommittedResult(
      state.context.windowId, state.context.computationDigest).has_value());
    BOOST_CHECK(!state.transactions.committedStatePath(state.context.windowId).has_value());
    BOOST_CHECK(!std::filesystem::exists(state.input));
    BOOST_CHECK(!std::filesystem::exists(state.output));
    BOOST_CHECK(!std::filesystem::exists(state.temporary.path / "transactions"));
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      BOOST_CHECK_MESSAGE(state.unexpectedError.empty(), state.unexpectedError);
    }
    BOOST_REQUIRE(user.CancelCollaboration(requestId));
    BOOST_CHECK(!user.CancelCollaboration(requestId));
    // Keep admission observers and the fault active while queued Face work drains.
    environment.pumpUntilWithAttributeAuthority([] { return false; });
    checkMutationStats();
    BOOST_CHECK_EQUAL(state.ackClosures.load(), 1);
    BOOST_CHECK_EQUAL(state.ackCandidates.load(), 0);
    if (wrongSigner) {
      BOOST_CHECK_GT(state.validWrongSignerEnvelopes.load(), 0);
      BOOST_CHECK_EQUAL(state.invalidWrongSignerEnvelopes.load(), 0);
    }
    else {
      BOOST_CHECK_EQUAL(state.requestEnvelopes.load(), 0);
    }
    if (damagedInner) {
      BOOST_CHECK_GT(state.validDamagedInnerTransports.load(), 0);
      BOOST_CHECK_EQUAL(state.invalidDamagedInnerTransports.load(), 0);
    }
    BOOST_CHECK_EQUAL(state.invalidEnvelopes.load(), 0);
    BOOST_CHECK_EQUAL(provider.getHybridCryptoCounters().symmetric_decrypt_count.load(),
                      decryptions);
    BOOST_CHECK_EQUAL(state.handlerCalls.load(), 0);
    BOOST_CHECK_EQUAL(state.contextAdmissions.load(), 0);
    BOOST_CHECK_EQUAL(state.scopedAdmissions.load(), 0);
    BOOST_CHECK_EQUAL(state.bindingRefusals.load(), 0);
    BOOST_CHECK_EQUAL(state.responseCallbacks.load(), 0);
    BOOST_CHECK(!state.timedOut.load());
    BOOST_CHECK_EQUAL(state.executionCalls.load(), 0);
    BOOST_CHECK_EQUAL(state.childSuccesses.load(), 0);
    BOOST_CHECK_EQUAL(state.commitCalls.load(), 0);
    BOOST_CHECK(!state.transactions.loadCommittedResult(
      state.context.windowId, state.context.computationDigest).has_value());
    BOOST_CHECK(!state.transactions.committedStatePath(state.context.windowId).has_value());
    BOOST_CHECK(!std::filesystem::exists(state.input));
    BOOST_CHECK(!std::filesystem::exists(state.output));
    BOOST_CHECK(!std::filesystem::exists(state.temporary.path / "transactions"));
    environment.providerPubSub().unsubscribe(subscription);
    registration.close();
    environment.resetRequest(*faultScope);
    return;
  }
  BOOST_CHECK_EQUAL(state.responseCallbacks.load(), 1);
  BOOST_CHECK_EQUAL(state.handlerCalls.load(), 1);
  BOOST_CHECK_EQUAL(state.contextAdmissions.load(), 1);
  BOOST_CHECK_EQUAL(state.scopedAdmissions.load(), 1);
  BOOST_CHECK_GT(state.requestEnvelopes.load(), 0);
  BOOST_CHECK_EQUAL(state.invalidEnvelopes.load(), 0);
  BOOST_CHECK_GT(provider.getHybridCryptoCounters().symmetric_decrypt_count.load(), decryptions);
  const auto committed = state.transactions.loadCommittedResult(
    state.context.windowId, state.context.computationDigest);
  const auto snapshot = state.transactions.committedStatePath(state.context.windowId);
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    BOOST_CHECK_MESSAGE(state.unexpectedError.empty(), state.unexpectedError);
    if (wrongKey.empty()) {
      BOOST_REQUIRE(committed.has_value());
      BOOST_REQUIRE(snapshot.has_value());
      BOOST_CHECK_MESSAGE(state.responseStatus.load(), state.responseError);
      BOOST_CHECK_EQUAL(state.bindingRefusals.load(), 0);
      BOOST_CHECK_EQUAL(state.executionCalls.load(), 1);
      BOOST_CHECK_EQUAL(state.childSuccesses.load(), 1);
      BOOST_CHECK_EQUAL(state.commitCalls.load(), 1);
      BOOST_CHECK_EQUAL(state.responsePayload, std::string(committed->begin(), committed->end()));
      UavPersonTrackingEngine restored(state.profile);
      auto next = state.context;
      next.windowId = "window-1";
      ++next.sequence;
      for (auto& frame : next.frames) {
        ++frame.sequence;
        frame.commonPtsUs += 500000;
      }
      BOOST_CHECK_NO_THROW(restored.restoreState(
        readBounded(*snapshot, state.profile.limits.at("maxSnapshotBytes")), next));
    }
    else {
      BOOST_CHECK(!committed.has_value());
      BOOST_CHECK(!snapshot.has_value());
      BOOST_CHECK(!state.responseStatus.load());
      BOOST_CHECK_EQUAL(state.bindingRefusals.load(), 1);
      BOOST_CHECK_EQUAL(state.executionCalls.load(), 0);
      BOOST_CHECK_EQUAL(state.childSuccesses.load(), 0);
      BOOST_CHECK_EQUAL(state.commitCalls.load(), 0);
      BOOST_CHECK_EQUAL(state.refusal, "person request binding mismatch: " + wrongKey);
      BOOST_CHECK_EQUAL(state.responsePayload, state.refusal);
      BOOST_CHECK(!std::filesystem::exists(state.input));
      BOOST_CHECK(!std::filesystem::exists(state.output));
      BOOST_CHECK(!std::filesystem::exists(state.temporary.path / "transactions"));
    }
  }
  if (productionTransport) {
    std::lock_guard<std::mutex> lock(state.mutex);
    BOOST_CHECK_EQUAL(state.invalidTransportObservations.load(), 0);
    BOOST_REQUIRE(!state.observedRequestWire.empty());
    BOOST_REQUIRE(!state.validatedRequestWire.empty());
    BOOST_CHECK(state.observedRequestWire == state.validatedRequestWire);
    BOOST_CHECK_GT(state.observedPiggyRequests.load() + state.observedRetrievedRequests.load(), 0);
    const auto budget = environment.userPubSub().getMaxPiggyDataSize();
    const bool eligible = state.observedRequestWire.size() <= budget;
    if (!eligible) BOOST_CHECK_EQUAL(state.observedPiggyRequests.load(), 0);
    ndn::util::Sha256 hash;
    hash << std::string(state.observedRequestWire.begin(), state.observedRequestWire.end());
    const Json probe = {{"schema", "spec219-person-transport-probe-v1"},
      {"requestName", requestName.toUri()}, {"innerWireBytes", state.observedRequestWire.size()},
      {"innerWireDigest", canonicalSha256Digest(hash.toString())},
      {"userBudget", budget}, {"providerBudget", environment.providerPubSub().getMaxPiggyDataSize()},
      {"protocol", "v3"}, {"piggyEligible", eligible},
      {"observedPiggyPackets", state.observedPiggyRequests.load()},
      {"observedRetrievedPackets", state.observedRetrievedRequests.load()},
      {"validatedWireEqualsObserved", true}, {"scope", "six-binding-field native fixture"}};
    BOOST_TEST_MESSAGE("SPEC219_TRANSPORT_PROBE " << probe.dump());
  }
  state.requestInterestObservation.disconnect();
  state.requestDataObservation.disconnect();
  environment.providerPubSub().unsubscribe(subscription);
  registration.close();
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavPersonAdmission)

BOOST_AUTO_TEST_CASE(ProductionSvsBudgetObservesPersonRequestTransport)
{
  checkAdmissionCase("production-budget", "", RequestSignatureFault::NONE, true);
}

BOOST_AUTO_TEST_CASE(ProductionSvsBudgetRejectsPiggybackSignatureFaults)
{
  checkAdmissionCase("piggy-control", "", RequestSignatureFault::NONE, true);
  checkAdmissionCase("piggy-bad-carrier", "", RequestSignatureFault::DAMAGED_TRANSPORT, true);
  checkAdmissionCase("piggy-wrong-signer", "", RequestSignatureFault::WRONG_INNER_SIGNER, true);
  checkAdmissionCase("piggy-bad-inner", "", RequestSignatureFault::DAMAGED_INNER, true);
}

BOOST_AUTO_TEST_CASE(ValidatedCoreIngressAdmitsControlAndRejectsWrongPersonBindings)
{
  checkAdmissionCase("control", "");
  checkAdmissionCase("wrong-profile", "tracking_profile_digest");
  checkAdmissionCase("wrong-computation", "tracking_computation_digest");
}

BOOST_AUTO_TEST_CASE(ValidatedCoreIngressRejectsDamagedRequestTransportSignature)
{
  checkAdmissionCase("signature-control", "");
  checkAdmissionCase("damaged-signature", "", RequestSignatureFault::DAMAGED_TRANSPORT);
}

BOOST_AUTO_TEST_CASE(ValidatedCoreIngressRejectsTrustedWrongInnerRequestSigner)
{
  checkAdmissionCase("signer-control", "");
  checkAdmissionCase("wrong-inner-signer", "", RequestSignatureFault::WRONG_INNER_SIGNER);
}

BOOST_AUTO_TEST_CASE(ValidatedCoreIngressRejectsDamagedInnerRequestSignature)
{
  checkAdmissionCase("inner-signature-control", "");
  checkAdmissionCase("damaged-inner-signature", "", RequestSignatureFault::DAMAGED_INNER);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test

