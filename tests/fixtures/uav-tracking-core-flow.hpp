#ifndef NDNSF_TESTS_UAV_TRACKING_CORE_FLOW_HPP
#define NDNSF_TESTS_UAV_TRACKING_CORE_FLOW_HPP

#include "tests/boost-test.hpp"
#include "tests/fixtures/uav-tracking-job-config.hpp"
#include "tests/fixtures/uav-tracking-image.hpp"
#include "NDNSF-UAV-APP/shared/uav-person-published-frame.hpp"

#include "NDNSF-UAV-APP/shared/UavDigest.hpp"
#include "NDNSF-UAV-APP/shared/UavNames.hpp"
#include "NDNSF-UAV-APP/shared/UavProtocol.hpp"
#include "NDNSF-UAV-APP/tracking/uav-tracking-job.hpp"
#include "tests/integration-tests/ndnsf-integration-fixture.hpp"

#include <ndn-cxx/util/sha256.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>

#include <atomic>
#include <set>
#include <thread>

namespace ndnsf::test::tracking_flow {

namespace nsf = ndn_service_framework;
using namespace examples::uav;
using person_json::Json;

inline ndn::Buffer
asBuffer(const std::string& text)
{
  return ndn::Buffer(text.begin(), text.end());
}

inline ndn::Buffer
asFields(const Fields& fields)
{
  return asBuffer(encodeFields(fields));
}

inline std::string
digestOf(const std::string& text)
{
  ndn::util::Sha256 hash;
  hash << std::string_view(text);
  return canonicalSha256Digest(hash.toString());
}

inline void
publishDrained(nsf::ServiceProvider::CollaborationContext& context, const Fields& fields)
{
  context.publish(TRACKING_INPUT_KEY_SCOPE, TRACKING_WORK_DRAINED_TOPIC,
    asFields({{"schema", TRACKING_WORK_DRAINED_SCHEMA}, {"request_id", context.sessionId()},
      {"run_id", fieldOr(fields, "run_id", "")}, {"mission_id", fieldOr(fields, "mission_id", "")},
      {"window_id", fieldOr(fields, "window_id", "")}, {"tracking_mode", "vehicle"},
      {"source_epoch", fieldOr(fields, "source_epoch", "")}, {"work_drained", "true"}}));
}

/** Synthetic vision outputs test the production owner/Core path, never model accuracy. */
struct JobFlowFixture : tracking_fixture::ConfigFixture
{
  explicit JobFlowFixture(bool sourceService = true,
                          std::vector<ndn::Name> additionalServices = {})
  {
    nsf::test::BootstrapProfile profile;
    profile.serviceName = SERVICE_TRACKING_WINDOW;
    profile.providerCount = 4;
    profile.additionalServices = std::move(additionalServices);
    if (sourceService) profile.additionalServices.push_back(SERVICE_TRACKING_SOURCE);
    profile.providerRoles = {"Camera-UAV1", "Camera-UAV2", "Camera-UAV3", "Tracking-Compute"};
    profile.validateSvsPackets = true;
    profile.useProductionSvsTransportOptions = true;
    environment = std::make_unique<nsf::test::NdnsfIntegrationEnvironment>(profile);
    environment->bootstrap();
    // One fixture policy applies to real and synthetic capability owners.
    for (size_t i = 0; i < 4; ++i) {
      environment->provider(i).setUseTokens(false);
      environment->provider(i).setHandlerThreads(2);
    }
    // The shared Core fixture installs permissions but no service policy status.
    // Install an independently signed status, as the standalone owner requires.
    const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
    const auto certificate = environment->keyChain().getPib()
      .getIdentity(profile.attributeAuthority).getDefaultKey().getDefaultCertificate();
    const nsf::ControllerVersion version{now, 1};
    std::set<ndn::Name> statusServices{SERVICE_TRACKING_WINDOW};
    statusServices.insert(profile.additionalServices.begin(), profile.additionalServices.end());
    for (const auto& service : statusServices) {
      nsf::PolicyStatusData status;
      status.setServiceName(service);
      status.setControllerVersion(version);
      status.setValidity(now - 1000, now + 120000);
      status.setPolicyDigest("sha256:" + std::string(64, 'a'));
      status.setAbePublicParametersName(environment->attributeAuthorityPublicParametersName());
      status.setAbePublicParametersDigest(environment->attributeAuthorityPublicParametersDigest());
      status.setControllerCertificate(certificate.getName());
      ndn::Data statusData(nsf::makePolicyStatusName(certificate.getIdentity(), service, version));
      statusData.setContent(status.wireEncode());
      environment->keyChain().sign(statusData, ndn::security::signingByCertificate(certificate));
      const bool signatureValid = ndn::security::verifySignature(statusData, certificate);
      BOOST_REQUIRE(signatureValid);
      BOOST_REQUIRE(environment->user().installControllerStatus(status, signatureValid));
      for (size_t i = 0; i < 4; ++i) {
        BOOST_REQUIRE(environment->provider(i).installControllerStatus(status, signatureValid));
      }
    }
    const auto ready = [&] {
      for (const auto& service : statusServices) {
        if (environment->user().getControllerVersion(service) != version) return false;
        for (size_t i = 0; i < 4; ++i) {
          if (environment->provider(i).getControllerVersion(service) != version) return false;
        }
      }
      if (!environment->user().isNacConsumerReadyForTest() ||
          !environment->user().isNacProducerReadyForTest()) return false;
      for (size_t i = 0; i < 4; ++i) {
        if (!environment->provider(i).isRequestDecryptionReady() ||
            !environment->provider(i).isNacProducerReadyForTest()) return false;
      }
      return true;
    };
    environment->pumpUntilWithAttributeAuthority(ready);
    BOOST_REQUIRE(ready());
    environment->user().setHandlerThreads(0);
    environment->user().setUseTokens(false);
    auto source = Json::parse(tracking_fixture::SOURCE_MANIFEST);
    source["windowCount"] = 1;
    const auto text = source.dump();
    std::ofstream(root / "asset") << text;
    config["windowCount"] = 1;
    config["deadlineMs"] = 15000;
    config["requestTimeoutMs"] = 10000;
    for (size_t i = 0; i < 3; ++i) {
      config["sources"][i]["sourceManifest"]["digest"] = digestOf(text);
      config["sources"][i]["producerIdentity"] = environment->provider(i).getName().toUri();
    }
    config["compute"]["producerIdentity"] = environment->provider(3).getName().toUri();
    definition = load();
    job = std::make_unique<UavTrackingJob>(environment->user(), environment->userFace(),
      [] (const TrackingJobDefinition&, std::string*) { return true; },
      [this] (const TrackingSnapshot& snapshot) {
        if (snapshot.completedWindows > latest.completedWindows && !snapshot.activeRequestId.empty()) {
          completedRequests.emplace_back(snapshot.activeRequestId);
        }
        latest = snapshot;
      });
  }

  ~JobFlowFixture()
  {
    stopAndDrainJob();
    syntheticRegistrations.clear();
    // Join production handler workers before captured fixture inputs are destroyed.
    environment.reset();
  }

  void stopAndDrainJob()
  {
    releaseCompute->store(true);
    releaseSource->store(true);
    if (!job) return;
    auto reported = std::make_shared<bool>(false);
    job->stop([reported] (const TrackingCleanupResult&) { *reported = true; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    const auto done = [&] {
      return (*reported && job->isDrained()) || std::chrono::steady_clock::now() >= deadline;
    };
    while (!done()) environment->pumpUntilWithAttributeAuthority(done);
    BOOST_CHECK(*reported);
    // Never destroy borrowed dependencies with an outstanding stop callback.
    BOOST_CHECK(job->isDrained());
    if (!*reported || !job->isDrained()) std::terminate();
    job.reset();
  }

  void configureWindows(size_t count)
  {
    auto source = Json::parse(tracking_fixture::SOURCE_MANIFEST);
    source["windowCount"] = count;
    source["generations"][0]["sourceEpoch"] = config["sources"][0]["sourceEpoch"];
    const auto text = source.dump();
    std::ofstream(root / "asset") << text;
    config["windowCount"] = count;
    for (auto& binding : config["sources"]) {
      binding["sourceManifest"]["digest"] = digestOf(text);
    }
    definition = load();
  }

  void install(bool rejectCompute = false, bool holdCompute = false, bool wrongFeedback = false,
               bool omitSourceDrain = false, std::string wrongDrainField = {},
               size_t firstSyntheticProvider = 0, size_t lastSyntheticProvider = 4,
               bool holdSourceDrain = false)
  {
    for (size_t i = firstSyntheticProvider; i < lastSyntheticProvider; ++i) {
      auto& provider = environment->provider(i);
      std::shared_ptr<UavPersonPublishedFrameStore> frames;
      if (i < 3) {
        frames = std::make_shared<UavPersonPublishedFrameStore>(
          provider.getName(), definition.sources[i].cameraId);
        provider.addService(SERVICE_TRACKING_SOURCE,
          [] (const nsf::RequestMessage&) { nsf::ServiceProvider::AckDecision ack; ack.status = true; return ack; },
          [frames] (const ndn::Name& requester, const ndn::Name&, const ndn::Name&,
                    const ndn::Name&, const nsf::RequestMessage& request) {
            const auto payload = request.getPayload();
            const auto fields = decodeFields(std::string(payload.begin(), payload.end()));
            auto bytes = frames->read(fields, requester);
            nsf::ResponseMessage response;
            response.setStatus(true);
            response.setPayload(bytes, bytes.size());
            return response;
          }, nsf::ServiceProvider::ServiceInvocationMode::NormalAndTargeted);
      }
      std::vector<ndn_service_framework::CollaborationRole> allowedRoles;
      allowedRoles.push_back(i < 3 ? "Camera-UAV" + std::to_string(i + 1) :
                                      "Tracking-Compute");
      syntheticRegistrations.push_back(provider.addScopedCollaborationHandler(
        SERVICE_TRACKING_WINDOW, std::move(allowedRoles),
        [this, rejectCompute, wrongFeedback, i] (const nsf::RequestMessage&) {
          syntheticAcknowledgements.fetch_add(1, std::memory_order_relaxed);
          nsf::ServiceProvider::AckDecision decision;
          decision.status = !(rejectCompute && i == 3);
          Fields feedback{{"tracking_mode", wrongFeedback && i == 3 ? "person" : "vehicle"},
            {"tracking_work_drained_schema", TRACKING_WORK_DRAINED_SCHEMA},
            {"source_epoch", std::to_string(definition.sources[0].sourceEpoch)}};
          if (i < 3) feedback["source_id"] = definition.sources[i].sourceId;
          decision.payload = asFields(feedback);
          return decision;
        },
        [this, i, frames, holdCompute, omitSourceDrain, wrongDrainField, holdSourceDrain,
         realSource = firstSyntheticProvider == 1, release = releaseCompute,
         heldRunId = definition.runId]
        (nsf::ServiceProvider::CollaborationContext& context, const nsf::RequestMessage& request) {
          ++executions;
          const auto payload = request.getPayload();
          const auto fields = decodeFields(std::string(payload.begin(), payload.end()));
          if (i < 3) {
            const auto sequence = std::stoull(fieldOr(fields, "window_id", "window-0").substr(7)) + 1;
            const auto& source = definition.sources[i];
            const auto& jpeg = tracking_fixture::trackingFixtureJpeg();
            const auto name = makeUavObservationDataName(source.producerIdentity, source.cameraId,
              source.sourceEpoch, sequence, "FRAME", 1);
            const auto published = context.publishLargeNamed(TRACKING_INPUT_KEY_SCOPE, name, jpeg);
            if (published != name) throw std::runtime_error("fixture FRAME publication failed");
            UavPersonFrameReadBinding binding;
            binding.runId = definition.runId;
            binding.missionId = fieldOr(fields, "mission_id", "");
            binding.windowId = fieldOr(fields, "window_id", "");
            binding.cameraId = source.cameraId;
            binding.sourceId = source.sourceId;
            binding.sourceEpoch = source.sourceEpoch;
            binding.sequence = sequence;
            binding.frameName = name.toUri();
            binding.inputDigest = digestOf(std::string(jpeg.begin(), jpeg.end()));
            frames->retainPublished(binding, jpeg, context.requesterName());
            context.publish(TRACKING_INPUT_KEY_SCOPE, ndn::Name("/example/uav/tracking"),
                            asBuffer("abc"));
            context.publish("uav-tracking-observation", ndn::Name("/example/uav/observation"),
                            asBuffer("abc"));
            if (holdSourceDrain && i == 0) {
              sourceHeld->store(true);
              const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
              while (!releaseSource->load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
              if (!releaseSource->load()) return;
            }
            if (!omitSourceDrain || i != 0) publishDrained(context, fields);
            context.completeRole();
            return;
          }
          computeEntered->store(true);
          if (holdCompute && fieldOr(fields, "run_id", "") == heldRunId) {
            const auto notices = context.waitFor(TRACKING_INPUT_KEY_SCOPE, TRACKING_CANCEL_TOPIC,
                                                1, 5000);
            if (notices.empty()) throw std::runtime_error("fixture cancellation notice missing");
            context.revokeCurrentWorkAuthority();
            context.publish(TRACKING_INPUT_KEY_SCOPE, TRACKING_CANCEL_ACK_TOPIC,
              asFields({{"schema", TRACKING_CANCEL_ACK_SCHEMA},
                {"request_id", context.sessionId()}, {"run_id", fieldOr(fields, "run_id", "")},
                {"mission_id", fieldOr(fields, "mission_id", "")},
                {"window_id", fieldOr(fields, "window_id", "")}, {"authority_revoked", "true"}}));
            // A valid authority ACK deliberately precedes the end of native work.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (!release->load() && std::chrono::steady_clock::now() < deadline) {
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (release->load()) {
              auto proof = fields;
              if (!wrongDrainField.empty()) proof[wrongDrainField] = "invalid-binding";
              publishDrained(context, proof);
            }
            return;
          }
          const size_t syntheticCount = realSource ? 2 : 3;
          const auto inputs = context.waitFor(TRACKING_INPUT_KEY_SCOPE,
            ndn::Name("/example/uav/tracking"), syntheticCount, 3000);
          if (inputs.size() != syntheticCount) {
            throw std::runtime_error("fixture synthetic source dependency missing");
          }
          if (realSource) {
            // A real source publishes named large Data, not the synthetic
            // fixture's arbitrary abc topic. Consume its actual production object.
            const auto sequence = std::stoull(fieldOr(fields, "window_id", "window-0").substr(7)) + 1;
            const auto& source = definition.sources[0];
            const auto name = makeUavObservationDataName(source.producerIdentity, "UAV1",
              source.sourceEpoch, sequence, "FRAME", 1);
            const auto frame = context.fetchLargeFromProducerCurrent(
              name, TRACKING_INPUT_KEY_SCOPE, source.producerIdentity, 3000);
            const ndn::Buffer expected{0xff, 0xd8, 0xff, 0xd9};
            if (!frame || *frame != expected) {
              throw std::runtime_error("fixture real source frame missing or corrupted");
            }
          }
          const auto sequence = std::stoull(fieldOr(fields, "window_id", "window-0").substr(7)) + 1;
          Json frames = Json::array();
          std::array<std::string, 3> imageDigests;
          for (size_t camera = 0; camera < 3; ++camera) {
            const auto& source = definition.sources[camera];
            const auto name = makeUavObservationDataName(source.producerIdentity,
              source.cameraId, source.sourceEpoch, sequence, "OBSERVATION", 1).toUri();
            const auto& jpeg = tracking_fixture::trackingFixtureJpeg();
            const auto imageDigest = realSource && camera == 0 ?
              digestOf(std::string("\xff\xd8\xff\xd9", 4)) : digestOf(std::string(jpeg.begin(), jpeg.end()));
            imageDigests[camera] = imageDigest;
            frames.push_back({{"tracks", Json::array()}, {"cameraId", source.cameraId}, {"sequence", sequence},
              {"ptsUs", sequence * 1000}, {"imageWidth", uint64_t(640)}, {"imageHeight", uint64_t(480)},
              {"sourceId", source.sourceId}, {"sourceEpoch", source.sourceEpoch},
              {"inputDigest", imageDigest}, {"observationName", name},
              {"imageRef", {{"name", makeUavObservationDataName(source.producerIdentity,
                source.cameraId, source.sourceEpoch, sequence, "FRAME", 1).toUri()},
                {"digest", imageDigest}}},
              {"observationDigest", tracking_fixture::ABC_DIGEST},
              {"observation", {{"schema", "uav-observation-v2"}, {"sourceId", source.sourceId},
                {"sourceEpoch", source.sourceEpoch}, {"sequence", sequence},
                {"observationRef", {{"name", name}, {"digest", tracking_fixture::ABC_DIGEST}}}}}});
          }
          const auto mission = fieldOr(fields, "mission_id", "");
          const auto window = fieldOr(fields, "window_id", "");
          const auto result = Json({{"schema", "spec210-tracking-result-v2"},
            {"runId", definition.runId}, {"missionId", mission}, {"windowId", window},
            {"sequence", sequence}, {"terminal", true}, {"frames", frames},
            {"decisionSchema", "uav-vehicle-native-decision-v1"},
            {"actualDevice", "cpu"},
            {"nativeDecisionDigest", "sha256:" + std::string(64, 'a')}}).dump();
          publishDrained(context, fields);
          computeResponseReady->store(true);
          context.publishFinalResponse(asFields({{"accepted", "true"},
            {"result_json", result}, {"result_digest", digestOf(result)},
            {"result_name", makeUavMultiViewResultName(
              context.localProvider(), mission, window, sequence, 1).toUri()},
            {"provider", context.localProvider().toUri()},
            {"input_uav1_digest", imageDigests[0]},
            {"input_uav2_digest", imageDigests[1]},
            {"input_uav3_digest", imageDigests[2]}}));
        }, ndn_service_framework::CollaborationAdmissionLimits{}));
    }
    // Legacy registrations install Request subscriptions during init().
    environment->enableProductionIngressForTest();
  }

  std::shared_ptr<std::atomic<bool>> releaseCompute = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> computeEntered = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> releaseSource = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> sourceHeld = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> computeResponseReady = std::make_shared<std::atomic<bool>>(false);
  std::atomic<size_t> executions{0};
  std::atomic<size_t> syntheticAcknowledgements{0};
  std::unique_ptr<nsf::test::NdnsfIntegrationEnvironment> environment;
  std::vector<nsf::ServiceProvider::ServiceRegistration> syntheticRegistrations;
  TrackingJobDefinition definition;
  TrackingSnapshot latest;
  std::vector<ndn::Name> completedRequests;
  std::unique_ptr<UavTrackingJob> job;
};


} // namespace ndnsf::test::tracking_flow

#endif // NDNSF_TESTS_UAV_TRACKING_CORE_FLOW_HPP
