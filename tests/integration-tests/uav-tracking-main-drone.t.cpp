#include "NDNSF-UAV-APP/drone/drone-service-container.hpp"
#include "NDNSF-UAV-APP/shared/UavObservationRefVerifier.hpp"
#include "NDNSF-UAV-APP/shared/UavTrackingEvidenceStore.hpp"
#include "NDNSF-UAV-APP/tracking/UavTrackingCoordinator.hpp"
#include "tests/fixtures/uav-tracking-core-flow.hpp"
#include "tests/fixtures/uav-tracking-provider-access.hpp"

#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct DroneTrackingTestAccess
{
  static void
  attach(DroneServiceContainer& runtime, ndn_service_framework::ServiceProvider& provider)
  {
    runtime.installServiceInstances(provider);
    runtime.installTrackingCapability(provider);
    runtime.m_containerReady = true;
  }

  static UavTrackingProvider* capability(DroneServiceContainer& runtime)
  {
    return runtime.m_trackingCapability.get();
  }

  static void releaseCapability(DroneServiceContainer& runtime)
  {
    runtime.m_trackingCapability.reset();
  }

  static auto heldRequest(DroneServiceContainer& runtime,
                          ndn_service_framework::ServiceProvider::RequestHandler handler)
  {
    return runtime.guardRequest(std::move(handler));
  }

  static auto queuedFace(DroneServiceContainer& runtime, std::function<void()> body)
  {
    return runtime.guardFace(std::move(body));
  }

  static auto queuedResponse(DroneServiceContainer& runtime,
    std::function<void(const ndn_service_framework::ResponseMessage&)> body)
  {
    return runtime.guardResponse(std::move(body));
  }

  static void fail(DroneServiceContainer& runtime)
  {
    runtime.handleRuntimeFailure("test failure after capability installation");
  }
};

} // namespace

namespace ndnsf::test {
using namespace tracking_flow;

struct MainDroneFixture : JobFlowFixture
{
  MainDroneFixture()
    : JobFlowFixture(true, {UavRuntimeConfig{}.serviceTelemetryStatus,
                             UavRuntimeConfig{}.serviceMavlinkExecute})
  {
    stopAndDrainJob();
    config.droneIdentityPrefix = definition.sources[0].producerIdentity.getPrefix(-1);
    config.groupPrefix = environment->profile().groupPrefix;
    config.controllerPrefix = environment->profile().attributeAuthority;
    droneId = definition.sources[0].producerIdentity.get(-1).toUri();
    catalog.producerIdentity = definition.sources[0].producerIdentity;
    catalog.droneId = droneId;
    catalog.ownedWorkspace = root / "main-drone-work";
    TrackingProviderRunDefinition run;
    run.runId = definition.runId;
    run.mode = TrackingMode::VEHICLE;
    run.role = TrackingProviderRole::SOURCE;
    run.cameraId = "UAV1";
    for (size_t i = 0; i < 3; ++i) run.sourceProducers[i] = definition.sources[i].producerIdentity;
    run.computeProducer = definition.computeIdentity;
    run.windowCount = 1;
    run.requestTimeoutMs = definition.requestTimeoutMs;
    run.sourceGenerations = definition.sources[0].sourceManifest;
    run.observationStreamDirectory = root / "main-source";
    run.worldFrameId = "synthetic-local";
    run.acquisitionKind = "simulated";
    run.deliveryMode = "replay";
    std::filesystem::create_directory(run.observationStreamDirectory);
    // Lifecycle input only; the main GTK presentation fixture uses an actual JPEG.
    std::ofstream(run.observationStreamDirectory / "window-0.jsonl")
      << R"({"schema":"uav-source-event-v2","kind":"calibration","payload":{"schema":"uav-calibration-v2","cameraId":"UAV1","revision":"3","width":"640","height":"480","fx":"500","fy":"500","cx":"320","cy":"240","distortion":["0","0","0","0","0"],"imageTransform":"raw","validFromUs":"0","validUntilUs":"10000000"}})"
      << '\n'
      << "{\"schema\":\"uav-source-event-v2\",\"kind\":\"frame\",\"payload\":{\"key\":{\"sourceId\":\""
      << definition.sources[0].sourceId
      << R"(","cameraId":"UAV1","epoch":"1","sequence":"1"},"deviceTimeUs":"1000","clockBinding":{"revision":"1","clockId":"sim-clock","clockEpoch":"2","a":"1","bUs":"0","validFromUs":"0","validUntilUs":"10000000","uncertaintyUs":"0"},"width":"640","height":"480","encoding":"image/jpeg","calibrationRevision":"3","imageBase64":"/9j/2Q=="}})"
      << '\n';
    catalog.runs.push_back(std::move(run));
  }

  ~MainDroneFixture()
  {
    releaseCompute->store(true);
    releaseSource->store(true);
    stopAndDrainJob();
    if (runtime) {
      shutdown();
      DroneTrackingTestAccess::releaseCapability(*runtime);
      // Ordinary service bodies capture the actual container. Join Core pools first.
      environment.reset();
      runtime.reset();
    }
    if (writer >= 0) ::close(writer);
    if (watch >= 0) ::close(watch);
  }

  std::unique_ptr<DroneServiceContainer>
  makeRuntime(std::optional<UavTrackingProviderConfig> prepared)
  {
    return std::make_unique<DroneServiceContainer>(droneId, true, false, config, "", "mock",
      "127.0.0.1", "18570", "14550", "", "57600", false,
      VideoPublisher::CameraRuntimeOptions{}, std::move(prepared));
  }

  void start(bool tracking = true)
  {
    runtime = makeRuntime(tracking ? std::optional(catalog) : std::nullopt);
    DroneTrackingTestAccess::attach(*runtime, environment->provider());
  }

  Fields telemetry()
  {
    auto response = std::make_shared<std::optional<nsf::ResponseMessage>>();
    auto request = nsf::RequestMessage{};
    const auto id = environment->user().RequestServiceTargeted(
      environment->provider().getName(), config.serviceTelemetryStatus, request, 3000,
      [] (const ndn::Name&) {},
      [response] (const nsf::ResponseMessage& value) { *response = value; });
    BOOST_REQUIRE(!id.empty());
    environment->pumpUntilWithAttributeAuthority([response] { return response->has_value(); });
    BOOST_REQUIRE(response->has_value());
    BOOST_REQUIRE((*response)->getStatus());
    const auto payload = (*response)->getPayload();
    return decodeFields(std::string(payload.begin(), payload.end()));
  }

  Fields armControlledBackendThroughTheRegisteredService()
  {
    auto response = std::make_shared<std::optional<nsf::ResponseMessage>>();
    const auto frame = buildMockMavlinkFrame("arm", {{"arm", "true"}});
    auto request = nsf::RequestMessage{};
    auto payload = asFields({{"command", "arm"}, {"mavlink_hex", hexEncode(frame)}});
    request.setPayload(payload, payload.size());
    const auto started = std::chrono::steady_clock::now();
    const auto id = environment->user().RequestServiceTargeted(
      environment->provider().getName(), config.serviceMavlinkExecute, request, 2000,
      [] (const ndn::Name&) {},
      [response] (const nsf::ResponseMessage& value) { *response = value; });
    BOOST_REQUIRE(!id.empty());
    const auto deadline = started + std::chrono::milliseconds(2000);
    const auto done = [&] { return response->has_value() || std::chrono::steady_clock::now() >= deadline; };
    while (!done()) environment->pumpUntilWithAttributeAuthority(done);
    BOOST_REQUIRE(response->has_value());
    BOOST_CHECK_LE(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started).count(), 2000);
    BOOST_REQUIRE((*response)->getStatus());
    const auto bytes = (*response)->getPayload();
    const auto fields = decodeFields(std::string(bytes.begin(), bytes.end()));
    BOOST_CHECK_EQUAL(fields.at("accepted"), "true");
    BOOST_CHECK_EQUAL(fields.at("ack_source"), "mock");
    BOOST_CHECK_EQUAL(fields.at("fc_state"), "mock-armed");
    BOOST_CHECK_EQUAL(fields.at("forwarded_bytes"), std::to_string(frame.size()));
    return fields;
  }

  nsf::RequestMessage windowRequest(const TrackingProviderRunDefinition& run, uint64_t epoch = 1)
  {
    Fields fields{{"schema", "spec191-window-request-v1"}, {"run_id", run.runId},
      {"mission_id", "tracking-" + run.runId}, {"window_id", "window-0"},
      {"observation_schema", "uav-observation-v2"}, {"source_epoch", std::to_string(epoch)},
      {"tracking_request_timeout_ms", std::to_string(run.requestTimeoutMs)}};
    if (run.mode == TrackingMode::PERSON) fields["tracking_mode"] = "indoor-person-reid";
    for (size_t i = 0; i < 3; ++i) {
      fields["source_uav" + std::to_string(i + 1) + "_id"] = definition.sources[i].sourceId;
    }
    nsf::RequestMessage request;
    auto bytes = asFields(fields);
    request.setPayload(bytes, bytes.size());
    return request;
  }

  void shutdown()
  {
    if (shutDown) return;
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto ok = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([this, done, ok] { *ok = runtime->shutdownRuntime(); *done = true; });
    try {
      while (!done->load()) environment->pumpUntilWithAttributeAuthority(
        [done] { return done->load(); });
    }
    catch (...) {
      worker.join();
      throw;
    }
    worker.join();
    BOOST_REQUIRE(ok->load());
    shutDown = true;
  }

  UavRuntimeConfig config;
  UavTrackingProviderConfig catalog;
  std::string droneId;
  std::unique_ptr<DroneServiceContainer> runtime;
  bool shutDown = false;
  int writer = -1;
  int watch = -1;
};

BOOST_AUTO_TEST_SUITE(UavTrackingMainDrone)

BOOST_FIXTURE_TEST_CASE(DisabledTrackingKeepsTheActualTelemetryService, MainDroneFixture)
{
  start(false);
  environment->enableProductionIngressForTest();
  BOOST_CHECK(DroneTrackingTestAccess::capability(*runtime) == nullptr);
  const auto fields = telemetry();
  BOOST_CHECK_EQUAL(fields.at("drone_id"), droneId);
  BOOST_CHECK_EQUAL(fields.at("flight_controller_backend"), "mock");
  BOOST_CHECK_EQUAL(fields.at("flight_controller_available"), "true");
}

BOOST_FIXTURE_TEST_CASE(CatalogCannotClaimAnotherMainActor, MainDroneFixture)
{
  auto bad = catalog;
  bad.producerIdentity = ndn::Name("/another/producer");
  BOOST_CHECK_THROW(makeRuntime(bad), std::invalid_argument);
  bad = catalog;
  bad.droneId = "another-drone";
  BOOST_CHECK_THROW(makeRuntime(bad), std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(OneMainCapabilityAdvertisesBothPreparedModes, MainDroneFixture)
{
  auto person = catalog.runs[0];
  person.runId += "-person";
  person.mode = TrackingMode::PERSON;
  auto generations = Json::parse(tracking_fixture::SOURCE_MANIFEST);
  generations["windowCount"] = 1;
  generations["generations"][0]["sourceEpoch"] = 2;
  const auto text = generations.dump();
  const auto path = root / "person-generations.json";
  std::ofstream(path) << text;
  person.sourceGenerations = {path, digestOf(text)};
  catalog.runs.push_back(person);
  start();
  environment->enableProductionIngressForTest();
  auto* capability = DroneTrackingTestAccess::capability(*runtime);
  BOOST_REQUIRE(capability != nullptr);
  for (size_t i = 0; i < 2; ++i) {
    const auto ack = UavTrackingProviderTestAccess::acknowledge(
      *capability, windowRequest(catalog.runs[i], i + 1));
    BOOST_REQUIRE_MESSAGE(ack.status, ack.message);
    const auto feedback = decodeFields(std::string(ack.payload.begin(), ack.payload.end()));
    BOOST_CHECK_EQUAL(feedback.at("tracking_mode"), i == 0 ? "vehicle" : "person");
    BOOST_CHECK_EQUAL(feedback.at("source_epoch"), std::to_string(i + 1));
  }
  BOOST_CHECK_EQUAL(telemetry().at("drone_id"), droneId);
}

BOOST_FIXTURE_TEST_CASE(MissingComputeAssetRejectsTrackingButTelemetryStillReplies, MainDroneFixture)
{
  auto& run = catalog.runs[0];
  run.role = TrackingProviderRole::COMPUTE;
  run.cameraId.clear();
  run.observationStreamDirectory.clear();
  run.computeProducer = catalog.producerIdentity;
  run.sourceProducers[0] = definition.computeIdentity;
  run.model = {root / "missing-model", digestOf("missing")};
  run.workerPython = run.workerScript = run.model;
  start();
  environment->enableProductionIngressForTest();
  auto fields = Fields{{"schema", "spec191-window-request-v1"}, {"run_id", run.runId},
    {"tracking_request_timeout_ms", "10000"}};
  nsf::RequestMessage request;
  auto bytes = asFields(fields);
  request.setPayload(bytes, bytes.size());
  auto* capability = DroneTrackingTestAccess::capability(*runtime);
  BOOST_REQUIRE(capability != nullptr);
  const auto ack = UavTrackingProviderTestAccess::acknowledge(*capability, request);
  BOOST_CHECK(!ack.status);
  BOOST_CHECK(ack.message.find("provider executable/model digest mismatch") != std::string::npos);
  BOOST_CHECK(ack.message.find("missing-model") != std::string::npos);
  BOOST_CHECK_EQUAL(telemetry().at("drone_id"), droneId);
}

BOOST_FIXTURE_TEST_CASE(QueuedCallbacksCannotTouchReleasedContainerDependencies, MainDroneFixture)
{
  start();
  auto calls = std::make_shared<size_t>(0);
  auto faceBody = DroneTrackingTestAccess::queuedFace(*runtime, [calls] { ++*calls; });
  auto responseBody = DroneTrackingTestAccess::queuedResponse(*runtime,
    [calls] (const nsf::ResponseMessage&) { ++*calls; });
  DroneTrackingTestAccess::fail(*runtime);
  shutdown();
  DroneTrackingTestAccess::releaseCapability(*runtime);
  environment.reset();
  runtime.reset();
  faceBody();
  responseBody(nsf::ResponseMessage{});
  BOOST_CHECK_EQUAL(*calls, 0);
}

BOOST_FIXTURE_TEST_CASE(ActiveSourceKeepsTelemetryResponsiveAndDrainsBeforeHostShutdown,
                        MainDroneFixture)
{
  start();
  const auto path = catalog.runs[0].observationStreamDirectory / "window-0.jsonl";
  std::filesystem::remove(path);
  BOOST_REQUIRE_EQUAL(::mkfifo(path.c_str(), 0600), 0);
  writer = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  watch = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  BOOST_REQUIRE(writer >= 0 && watch >= 0);
  BOOST_REQUIRE(::inotify_add_watch(watch, path.c_str(), IN_OPEN) >= 0);
  job = std::make_unique<UavTrackingJob>(environment->user(), environment->userFace(),
    [] (const TrackingJobDefinition&, std::string*) { return true; },
    [this] (const TrackingSnapshot& value) { latest = value; });
  install(false, false, false, false, {}, 1);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  bool opened = false;
  environment->pumpUntilWithAttributeAuthority([&] {
    char events[4096];
    if (::read(watch, events, sizeof(events)) > 0) opened = true;
    return opened;
  });
  BOOST_REQUIRE(opened);
  auto* capability = DroneTrackingTestAccess::capability(*runtime);
  BOOST_REQUIRE(capability != nullptr);
  const auto busy = UavTrackingProviderTestAccess::acknowledge(
    *capability, windowRequest(catalog.runs[0]));
  BOOST_CHECK(!busy.status);
  BOOST_CHECK(busy.message.find("BUSY") != std::string::npos);
  BOOST_CHECK_EQUAL(telemetry().at("flight_controller_backend"), "mock");
  BOOST_CHECK_EQUAL(armControlledBackendThroughTheRegisteredService().at("command"), "arm");
  BOOST_CHECK_EQUAL(telemetry().at("armed"), "true");
  shutdown();
  BOOST_CHECK(capability->isDrained());
  bool stillPumps = false;
  boost::asio::post(environment->userFace().getIoContext(), [&] { stillPumps = true; });
  environment->pumpUntilWithAttributeAuthority([&] { return stillPumps; });
  BOOST_CHECK(stillPumps);
}

BOOST_FIXTURE_TEST_CASE(AdmittedOrdinaryBodyDelaysDependencyReleaseAndLateBodyIsRejected,
                        MainDroneFixture)
{
  start();
  auto entered = std::make_shared<std::atomic<bool>>(false);
  auto release = std::make_shared<std::atomic<bool>>(false);
  auto body = DroneTrackingTestAccess::heldRequest(*runtime,
    [entered, release] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
                       const ndn::Name&, const nsf::RequestMessage&) {
      *entered = true;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!release->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
      }
      return makeResponse(release->load(), "");
    });
  auto requestDone = std::make_shared<std::atomic<bool>>(false);
  std::thread request([body, requestDone] {
    body({}, {}, {}, {}, nsf::RequestMessage{});
    *requestDone = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!entered->load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  auto stopped = std::make_shared<std::atomic<bool>>(false);
  auto ok = std::make_shared<std::atomic<bool>>(false);
  std::thread shutdownWorker([this, stopped, ok] {
    *ok = runtime->shutdownRuntime();
    *stopped = true;
  });
  // The second ordinary body is rejected only once close has actually taken effect.
  auto lateCalls = std::make_shared<std::atomic<size_t>>(0);
  auto late = DroneTrackingTestAccess::queuedFace(*runtime, [lateCalls] { ++*lateCalls; });
  bool closedSeen = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto before = lateCalls->load();
    late();
    if (lateCalls->load() == before) {
      closedSeen = true;
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  const bool blockedByBody = closedSeen && entered->load() &&
                             !stopped->load() && !requestDone->load();
  *release = true;
  request.join();
  shutdownWorker.join();
  BOOST_CHECK(blockedByBody);
  BOOST_CHECK(ok->load());
  BOOST_CHECK(!body({}, {}, {}, {}, nsf::RequestMessage{}).getStatus());
  shutDown = ok->load();
}

BOOST_AUTO_TEST_SUITE_END()


namespace {

enum class AdmissionFault
{
  NONE,
  HOLD_THIRD,
  WRONG_CLOCK_PRODUCER,
  WRONG_CAMERA,
  OLD_EPOCH,
  WRONG_SEQUENCE,
  CLOCK_DIGEST,
};

/** Actual Main compute capability; only sensor inputs and detector outputs are fixtures. */
struct NativeAdmissionFixture : MainDroneFixture
{
  NativeAdmissionFixture()
  {
    for (auto& ready : clockPrepared) ready.store(false);
    for (auto& count : clockFetches) count.store(0);
    for (auto& source : tracking_fixture::ConfigFixture::config["sources"]) {
      source["sourceEpoch"] = 2;
    }
    configureWindows(1);
    config.droneIdentityPrefix = definition.computeIdentity.getPrefix(-1);
    droneId = definition.computeIdentity.get(-1).toUri();
    catalog.producerIdentity = definition.computeIdentity;
    catalog.droneId = droneId;
    catalog.ownedWorkspace = root / "admission-main-compute";
    environment->user().setUseTokens(true);
    for (size_t i = 0; i < 4; ++i) environment->provider(i).setUseTokens(true);
    backendDirectory = root / "detector-fixture";
    std::filesystem::create_directory(backendDirectory);
    std::ofstream(backendDirectory / "model") << "fixed empty detector fixture";
    for (const char* helper : {"motion_estimator.py", "motion_schema.py",
                               "observation_schema.py", "byte_track_state.py"}) {
      std::ofstream(backendDirectory / helper) << "# hashed fixture companion\n";
    }
    std::ofstream(backendDirectory / "tracking_worker.py") << R"PY(import argparse, json, pathlib, time
p = argparse.ArgumentParser()
p.add_argument('--vehicle-window', required=True)
p.add_argument('--output-json', required=True)
a, _ = p.parse_known_args()
with pathlib.Path(__file__).with_name('backend-calls').open('a') as f:
    f.write('one-model-request\n')
hold = pathlib.Path(__file__).with_name('hold-backend')
deadline = time.monotonic() + 15
while hold.exists() and time.monotonic() < deadline:
    time.sleep(.01)
if hold.exists():
    raise RuntimeError('fixture backend hold deadline')
x = json.loads(pathlib.Path(a.vehicle_window).read_text())
y = {'schema':'uav-vehicle-backend-window-v1', 'runId':x['runId'],
     'missionId':x['missionId'], 'windowId':x['windowId'], 'sequence':x['sequence'],
     'actualDevice':'cpu', 'backendState':x['previousBackendState'],
     'frames':[{'cameraId':f['cameraId'], 'width':f['width'], 'height':f['height'],
                'detections':[]} for f in x['frames']]}
pathlib.Path(a.output_json).write_text(json.dumps(y))
)PY";
    auto& run = catalog.runs.front();
    run.role = TrackingProviderRole::COMPUTE;
    run.cameraId.clear();
    run.observationStreamDirectory.clear();
    run.computeProducer = definition.computeIdentity;
    run.sourceGenerations = definition.sources[0].sourceManifest;
    for (size_t i = 0; i < 3; ++i) run.sourceProducers[i] = definition.sources[i].producerIdentity;
    run.model = asset(backendDirectory / "model");
    run.workerPython = asset(std::filesystem::canonical("/usr/bin/python3"));
    run.workerScript = asset(backendDirectory / "tracking_worker.py");
  }

  ~NativeAdmissionFixture()
  {
    releaseThird.store(true);
    std::error_code ignored;
    std::filesystem::remove(backendDirectory / "hold-backend", ignored);
    try {
      if (job) stopAndDrainJob();
      if (!activeRequest.empty() && environment) environment->user().CancelCollaboration(activeRequest);
      if (runtime) {
        shutdown();
        DroneTrackingTestAccess::releaseCapability(*runtime);
      }
      interestObservation.disconnect();
      // All fixture captures remain alive until every production worker joins.
      environment.reset();
      runtime.reset();
    }
    catch (...) { std::terminate(); }
  }

  TrackingAssetReference asset(const std::filesystem::path& path)
  {
    std::ifstream input(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    return {path, digestOf(bytes)};
  }

  void await(const std::function<bool()>& predicate, std::chrono::milliseconds budget = 15000ms)
  {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    const auto done = [&] { return predicate() || std::chrono::steady_clock::now() >= deadline; };
    while (!done()) environment->pumpUntilWithAttributeAuthority(done);
    BOOST_REQUIRE_MESSAGE(predicate(), "native admission observation deadline; participants="
      << planParticipants << " planFailure=" << planFailure << " timeout=" << timedOut
      << " thirdHeld=" << thirdHeld.load() << " thirdFetch=" << thirdFetch.load()
      << " duplicateFirst=" << duplicateFirst.load() << " backendCalls=" << backendCalls()
      << " response=" << (response ? response->getErrorInfo() : "none"));
  }

  size_t backendCalls() const
  {
    std::ifstream input(backendDirectory / "backend-calls");
    return static_cast<size_t>(std::count(std::istreambuf_iterator<char>(input),
                                        std::istreambuf_iterator<char>(), '\n'));
  }

  bool committed() const
  {
    const auto directory = catalog.ownedWorkspace / "vehicle/native-default" / definition.runId /
      "transactions" / digestOf("tracking-" + definition.runId).substr(7) / "window-0/manifest.json";
    return std::filesystem::is_regular_file(directory);
  }

  void install(AdmissionFault inputFault, int timeoutMs = 10000, bool startNativeJob = false)
  {
    useNativeJob = startNativeJob;
    fault = inputFault;
    catalog.runs[0].requestTimeoutMs = timeoutMs;
    definition.requestTimeoutMs = timeoutMs;
    runtime = makeRuntime(catalog);
    DroneTrackingTestAccess::attach(*runtime, environment->provider(3));
    const auto thirdName = makeUavObservationDataName(definition.sources[2].producerIdentity,
      "UAV3", 2, 1, "FRAME", 1);
    interestObservation = environment->providerFace(3).onSendInterest.connect(
      [this, thirdName](const ndn::Interest& interest) {
        if (thirdName.isPrefixOf(interest.getName())) thirdFetch.store(true);
        for (size_t i = 0; i < 3; ++i) {
          if (clockPrepared[i].load() && clocks[i].isPrefixOf(interest.getName())) {
            ++clockFetches[i];
          }
        }
      });
    for (size_t i = 0; i < 3; ++i) {
      environment->provider(i).addCollaborationHandler(SERVICE_TRACKING_WINDOW,
        [this, i](const nsf::RequestMessage&) {
          nsf::ServiceProvider::AckDecision ack;
          ack.status = true;
          ack.payload = asFields({{"tracking_mode", "vehicle"},
            {"source_epoch", "2"}, {"source_id", definition.sources[i].sourceId},
            {"tracking_work_drained_schema", TRACKING_WORK_DRAINED_SCHEMA}});
          return ack;
        }, [this, i](nsf::ServiceProvider::CollaborationContext& context,
                     const nsf::RequestMessage& request) {
          const auto payload = request.getPayload();
          const auto fields = decodeFields(std::string(payload.begin(), payload.end()));
          if (i == 2 && fault == AdmissionFault::HOLD_THIRD) {
            thirdHeld.store(true);
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            if (useNativeJob)
              context.allowData(TRACKING_INPUT_KEY_SCOPE, TRACKING_CANCEL_TOPIC);
            while (!releaseThird.load() && context.hasCurrentWorkAuthority() &&
                   std::chrono::steady_clock::now() < deadline) {
              if (!useNativeJob) {
                std::this_thread::sleep_for(1ms);
                continue;
              }
              const auto controls = context.waitFor(
                TRACKING_INPUT_KEY_SCOPE, TRACKING_CANCEL_TOPIC, 1, 50);
              for (const auto& control : controls) {
                if (control.producer != context.requesterName() ||
                    control.producerRole != "user-control-v1" ||
                    control.sessionId != context.sessionId() ||
                    control.keyScope != TRACKING_INPUT_KEY_SCOPE ||
                    control.topic != TRACKING_CANCEL_TOPIC) continue;
                const auto value = decodeFields(std::string(
                  control.payload.begin(), control.payload.end()));
                if (fieldOr(value, "schema", "") != TRACKING_CANCEL_SCHEMA ||
                    fieldOr(value, "request_id", "") != context.sessionId() ||
                    fieldOr(value, "run_id", "") != fieldOr(fields, "run_id", "") ||
                    fieldOr(value, "mission_id", "") != fieldOr(fields, "mission_id", "") ||
                    fieldOr(value, "window_id", "") != fieldOr(fields, "window_id", "")) continue;
                context.revokeCurrentWorkAuthority();
                sourceCancelObserved.store(true);
              }
            }
            if (sourceCancelObserved.load()) {
              // The test owns this source-work hold; an ACK cannot release it.
              while (!releaseThird.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
              if (!releaseThird.load()) return;
              try { publishSource(context, fields, i); }
              catch (const std::exception&) { oldSourceRejected.store(true); }
              publishDrained(context, fields);
              context.completeRole();
              return;
            }
            if (!releaseThird.load() || !context.hasCurrentWorkAuthority()) return;
          }
          publishSource(context, fields, i);
          publishDrained(context, fields);
          context.completeRole();
        });
    }
    // Production init subscribes the handlers already registered on each actor.
    environment->enableProductionIngressForTest();
    environment->user().fetchPermissionsFromController(environment->profile().attributeAuthority);
    BOOST_REQUIRE(environment->user().hasRequestScopedDefault());
    if (useNativeJob) {
      job = std::make_unique<UavTrackingJob>(environment->user(), environment->userFace(),
        [] (const TrackingJobDefinition&, std::string*) { return true; },
        [this](const TrackingSnapshot& value) {
          latest = value;
          if (!value.activeRequestId.empty()) activeRequest = ndn::Name(value.activeRequestId);
        });
      std::string reason;
      BOOST_REQUIRE_MESSAGE(job->start(definition) == TrackingAdmission::ACCEPTED, reason);
      return;
    }
    auto request = windowRequest(catalog.runs[0], 2);
    auto plan = UavTrackingCoordinator::makePlan(
      {definition.sources[0].producerIdentity, definition.sources[1].producerIdentity,
       definition.sources[2].producerIdentity}, definition.computeIdentity,
      asFields({{"schema", "spec191-assignment-v1"}, {"run_id", definition.runId},
        {"mission_id", "tracking-" + definition.runId}, {"window_id", "window-0"}}),
      1000, timeoutMs);
    UavTrackingCoordinatorCallbacks callbacks;
    callbacks.onPlanCommitted = [this](const nsf::CollaborationAckClosure&,
                                      const std::vector<nsf::SelectedParticipant>& selected) {
      planParticipants = selected.size();
    };
    callbacks.onResponse = [this](const nsf::ResponseMessage& value) {
      if (!value.getStatus()) {
        std::cerr << "SPEC231_ADMISSION_RESPONSE_ERROR " << value.getErrorInfo() << std::endl;
      }
      response = value;
    };
    callbacks.onTimeout = [this](const ndn::Name&) { timedOut = true; };
    callbacks.onFailure = [this](const std::string& reason) { planFailure = reason; };
    const auto bytes = request.getPayload();
    activeRequest = coordinator.begin(environment->user(), SERVICE_TRACKING_WINDOW,
      bytes, std::move(plan), std::move(callbacks));
    BOOST_REQUIRE(!activeRequest.empty());
  }

  void publishSource(nsf::ServiceProvider::CollaborationContext& context,
                     const Fields& fields, size_t i)
  {
    const auto& binding = definition.sources[i];
    const Json clock = {{"revision", "1"}, {"clockId", "clock-" + binding.cameraId},
      {"clockEpoch", "2"}, {"a", "1"}, {"bUs", "0"}, {"validFromUs", "0"},
      {"validUntilUs", "10000000"}, {"uncertaintyUs", "0"}};
    const auto clockBytes = asBuffer(clock.dump());
    auto prefix = ndn::Name(binding.producerIdentity).append("UAV").append("CLOCK").append("1");
    if (fault != AdmissionFault::CLOCK_DIGEST || i != 0) {
      prefix = makeUavStaticObservationPrefix(prefix, digestOf(clock.dump()));
    }
    const auto original = context.publishImmutableProtectedData(prefix, clockBytes);
    if (!original.success) throw std::runtime_error(original.errorMessage);
    clocks[i] = original.dataName;
    clockPrepared[i].store(true);
    const auto imageName = makeUavObservationDataName(binding.producerIdentity,
      binding.cameraId, 2, 1, "FRAME", 1);
    const auto& jpeg = tracking_fixture::trackingFixtureJpeg();
    const auto imageDigest = digestOf(std::string(jpeg.begin(), jpeg.end()));
    Json reference = {{"exactName", original.dataName.toUri()},
      {"producerIdentity", binding.producerIdentity.toUri()},
      {"contentSha256", original.contentSha256}};
    if (i == 0 && fault == AdmissionFault::WRONG_CLOCK_PRODUCER) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!clockPrepared[1].load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
      }
      if (!clockPrepared[1].load()) throw std::runtime_error("B CLOCK preparation deadline");
      auto payloadB = clock;
      payloadB["clockId"] = "clock-UAV2";
      reference = {{"exactName", clocks[1].toUri()},
        {"producerIdentity", definition.sources[1].producerIdentity.toUri()},
        {"contentSha256", digestOf(payloadB.dump())}};
    }
    if (i == 0 && fault == AdmissionFault::CLOCK_DIGEST) {
      reference["contentSha256"] = "sha256:" + std::string(64, 'b');
    }
    Json sidecar = {{"schema", "uav-observation-v2"},
      {"identity", {{"sourceId", binding.sourceId}, {"droneId", binding.droneId},
        {"cameraId", binding.cameraId}, {"epoch", 2}, {"worldFrameId", "synthetic-local"},
        {"acquisitionKind", "simulated"}, {"deliveryMode", "replay"}}},
      {"key", {{"sourceId", binding.sourceId}, {"cameraId", binding.cameraId},
        {"epoch", 2}, {"sequence", 1}}},
      {"captureTimeUs", 1000}, {"timeUncertaintyUs", 0}, {"width", 640}, {"height", 480},
      {"imageRef", {{"exactName", imageName.toUri()},
        {"producerIdentity", binding.producerIdentity.toUri()}, {"contentSha256", imageDigest}}},
      {"clockRef", reference}, {"poseSampleRefs", Json::array()}, {"poseStatus", "unknown"},
      {"geometryValidity", "illustrative"}, {"reason", "fixture has no pose"}};
    if (i == 0 && fault == AdmissionFault::WRONG_CAMERA) sidecar["identity"]["cameraId"] = "UAV2";
    if (i == 0 && fault == AdmissionFault::OLD_EPOCH) sidecar["identity"]["epoch"] = 1;
    if (i == 0 && fault == AdmissionFault::WRONG_SEQUENCE) sidecar["key"]["sequence"] = 2;
    FrameEnvelope frame;
    frame.cameraId = binding.cameraId;
    frame.sourceEpoch = 2;
    frame.sequence = 1;
    frame.ptsUs = 1000;
    frame.width = 640;
    frame.height = 480;
    frame.bytes.assign(jpeg.begin(), jpeg.end());
    frame.observationMetadata = sidecar.dump();
    UavTrackingEvidenceStore store;
    std::string reason;
    const auto staged = store.stage(binding.producerIdentity, fields.at("mission_id"),
                                    fields.at("window_id"), frame, &reason);
    if (!staged) throw std::runtime_error(reason);
    if (store.publish(context, TRACKING_INPUT_KEY_SCOPE, *staged) != imageName) {
      throw std::runtime_error("fixture actual FRAME publication failed");
    }
    if (i == 0 && fault == AdmissionFault::HOLD_THIRD) {
      if (store.publish(context, TRACKING_INPUT_KEY_SCOPE, *staged) != imageName) {
        throw std::runtime_error("duplicate source publication changed identity");
      }
      duplicateFirst.store(true);
    }
  }

  void verifyOriginalBSignature()
  {
    await([this] { return clockPrepared[1].load(); });
    std::shared_ptr<ndn::Data> original;
    ndn::Interest interest(clocks[1]);
    interest.setCanBePrefix(true);
    interest.setInterestLifetime(ndn::time::milliseconds(2000));
    ndn::ScopedPendingInterestHandle pending = environment->userFace().expressInterest(interest,
      [&original](const ndn::Interest&, const ndn::Data& data) {
        original = std::make_shared<ndn::Data>(data);
      }, [](const ndn::Interest&, const ndn::lp::Nack&) {}, [](const ndn::Interest&) {});
    await([&original] { return original != nullptr; }, 3000ms);
    const auto certificate = environment->keyChain().getPib()
      .getIdentity(definition.sources[1].producerIdentity).getDefaultKey().getDefaultCertificate();
    BOOST_CHECK(clocks[1].isPrefixOf(original->getName()));
    BOOST_CHECK(ndn::security::verifySignature(*original, certificate));
  }

  void awaitTerminal()
  {
    await([this] { return response.has_value() || timedOut || !planFailure.empty(); });
    BOOST_REQUIRE_MESSAGE(planFailure.empty(), planFailure);
    BOOST_CHECK_EQUAL(planParticipants, 4U);
  }

  void rejected(const std::string& reason)
  {
    awaitTerminal();
    BOOST_REQUIRE(response.has_value());
    BOOST_CHECK(!response->getStatus());
    const auto bytes = response->getPayload();
    const std::string detail(bytes.begin(), bytes.end());
    BOOST_CHECK_MESSAGE(response->getErrorInfo().find(reason) != std::string::npos ||
                        detail.find(reason) != std::string::npos, detail);
    BOOST_CHECK_EQUAL(backendCalls(), 0U);
    BOOST_CHECK(!committed());
  }

  bool useNativeJob = false;
  std::atomic<bool> sourceCancelObserved{false};
  std::atomic<bool> oldSourceRejected{false};
  std::filesystem::path backendDirectory;
  AdmissionFault fault = AdmissionFault::NONE;
  UavTrackingCoordinator coordinator;
  ndn::Name activeRequest;
  std::optional<nsf::ResponseMessage> response;
  std::string planFailure;
  bool timedOut = false;
  size_t planParticipants = 0;
  std::atomic<bool> releaseThird{false};
  std::atomic<bool> thirdHeld{false};
  std::atomic<bool> thirdFetch{false};
  std::atomic<bool> duplicateFirst{false};
  std::array<ndn::Name, 3> clocks;
  std::array<std::atomic<bool>, 3> clockPrepared{};
  std::array<std::atomic<size_t>, 3> clockFetches{};
  ndn::signal::ScopedConnection interestObservation;
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec231UavTerminal)

BOOST_FIXTURE_TEST_CASE(MissingSourceCancelFencesMainAndWaitsForSourceDrain, NativeAdmissionFixture)
{
  install(AdmissionFault::HOLD_THIRD, 10000, true);
  await([this] { return thirdHeld.load() && thirdFetch.load() && duplicateFirst.load(); });
  BOOST_REQUIRE_EQUAL(backendCalls(), 0U);
  BOOST_REQUIRE(!committed());
  BOOST_REQUIRE(job->requestCancel(definition.jobId) == TrackingCancelResult::REQUESTED);
  await([this] { return latest.cancelState == "REMOTE_CONFIRMED" && sourceCancelObserved.load(); });
  BOOST_CHECK(latest.cleanupState != "CLEAN");
  BOOST_CHECK(job->start(definition) == TrackingAdmission::BUSY);
  releaseThird.store(true);
  await([this] { return job->isDrained(); });
  BOOST_REQUIRE(latest.state == TrackingJobState::CANCELLED);
  BOOST_CHECK_EQUAL(latest.cleanupState, "CLEAN");
  BOOST_CHECK(oldSourceRejected.load());
  BOOST_CHECK_EQUAL(latest.completedWindows, 0U);
  BOOST_CHECK_EQUAL(backendCalls(), 0U);
  BOOST_CHECK(!committed());
}

BOOST_FIXTURE_TEST_CASE(EnteredBackendCancelCannotCommitAndChildDrains, NativeAdmissionFixture)
{
  std::ofstream(backendDirectory / "hold-backend") << "owned fixture hold";
  const auto started = std::chrono::steady_clock::now();
  install(AdmissionFault::NONE, 10000, true);
  await([this] { return backendCalls() == 1; }, 5000ms);
  BOOST_REQUIRE(std::chrono::steady_clock::now() - started < 5000ms);
  BOOST_REQUIRE(!committed());
  BOOST_REQUIRE(job->requestCancel(definition.jobId) == TrackingCancelResult::REQUESTED);
  // This bound ends before both the original 10-second request deadline
  // and the backend's 15-second self-exit, so neither can satisfy the oracle.
  await([this] { return job->isDrained(); }, 3000ms);
  BOOST_REQUIRE(latest.state == TrackingJobState::CANCELLED);
  BOOST_CHECK_EQUAL(latest.cancelState, "REMOTE_CONFIRMED");
  BOOST_CHECK_EQUAL(latest.cleanupState, "CLEAN");
  BOOST_CHECK_EQUAL(latest.completedWindows, 0U);
  BOOST_CHECK_EQUAL(backendCalls(), 1U);
  BOOST_CHECK(!committed());
  // The production process owner must stop the held backend. This test never
  // removes its hold until fixture teardown, after the drain assertion.
  BOOST_CHECK(std::filesystem::exists(backendDirectory / "hold-backend"));
}

BOOST_FIXTURE_TEST_CASE(AcceptedMainTransactionWinsLateLocalCancel, NativeAdmissionFixture)
{
  install(AdmissionFault::NONE);
  awaitTerminal();
  BOOST_REQUIRE(response.has_value());
  BOOST_REQUIRE(response->getStatus());
  const auto payload = response->getPayload();
  BOOST_REQUIRE(committed());
  (void)environment->user().CancelCollaboration(activeRequest);
  const auto barrier = std::make_shared<std::atomic<bool>>(false);
  boost::asio::post(environment->userFace().getIoContext(), [barrier] { barrier->store(true); });
  await([barrier] { return barrier->load(); });
  BOOST_CHECK(response->getStatus());
  BOOST_CHECK(response->getPayload() == payload);
  BOOST_CHECK_EQUAL(backendCalls(), 1U);
  BOOST_CHECK(committed());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(Spec231UavAdmission)

BOOST_FIXTURE_TEST_CASE(AllThreeSourcesAreRequiredBeforeBackendDispatch, NativeAdmissionFixture)
{
  install(AdmissionFault::HOLD_THIRD);
  await([this] { return thirdHeld.load() && thirdFetch.load() && duplicateFirst.load(); });
  BOOST_CHECK_EQUAL(backendCalls(), 0U);
  BOOST_CHECK(!committed());
  releaseThird.store(true);
  awaitTerminal();
  BOOST_REQUIRE(response.has_value());
  BOOST_REQUIRE_MESSAGE(response->getStatus(), response->getErrorInfo());
  BOOST_CHECK_EQUAL(backendCalls(), 1U);
  BOOST_CHECK(committed());
  const auto bytes = response->getPayload();
  const auto fields = decodeFields(std::string(bytes.begin(), bytes.end()));
  const auto result = Json::parse(fields.at("result_json"));
  BOOST_CHECK_EQUAL(result.at("frames").size(), 3U);
}

BOOST_FIXTURE_TEST_CASE(OtherClockProducerCannotReplaceAssignedSource, NativeAdmissionFixture)
{
  install(AdmissionFault::WRONG_CLOCK_PRODUCER);
  rejected("CLOCK reference is not owned by the requested source");
  BOOST_CHECK_EQUAL(clockFetches[1].load(), 0U);
  verifyOriginalBSignature();
}

BOOST_FIXTURE_TEST_CASE(SidecarCannotClaimAnotherCameraRole, NativeAdmissionFixture)
{
  install(AdmissionFault::WRONG_CAMERA);
  rejected("identity does not match the requested frame");
}

BOOST_FIXTURE_TEST_CASE(CurrentNamedSidecarWithOldEpochIsRejected, NativeAdmissionFixture)
{
  install(AdmissionFault::OLD_EPOCH);
  rejected("identity does not match the requested frame");
}

BOOST_FIXTURE_TEST_CASE(CurrentNamedSidecarWithWrongSequenceIsRejected, NativeAdmissionFixture)
{
  install(AdmissionFault::WRONG_SEQUENCE);
  rejected("identity does not match the requested frame");
}

BOOST_FIXTURE_TEST_CASE(FetchedClockDigestMismatchCannotStartBackend, NativeAdmissionFixture)
{
  install(AdmissionFault::CLOCK_DIGEST);
  rejected("observation dependency content digest mismatch");
  BOOST_CHECK_GT(clockFetches[0].load(), 0U);
}

BOOST_FIXTURE_TEST_CASE(MissingThirdFrameCannotBeReplacedByDuplicateFirst, NativeAdmissionFixture)
{
  install(AdmissionFault::HOLD_THIRD, 2500);
  await([this] { return thirdHeld.load() && thirdFetch.load() && duplicateFirst.load(); });
  awaitTerminal();
  BOOST_CHECK(!response || !response->getStatus());
  BOOST_CHECK_EQUAL(backendCalls(), 0U);
  BOOST_CHECK(!committed());
  releaseThird.store(true);
}

BOOST_FIXTURE_TEST_CASE(PermissionLossWhileWaitingCannotStartBackend, NativeAdmissionFixture)
{
  install(AdmissionFault::HOLD_THIRD, 2500);
  await([this] { return thirdHeld.load() && thirdFetch.load(); });
  auto& compute = environment->provider(3);
  const auto previous = compute.getControllerVersion(SERVICE_TRACKING_WINDOW);
  BOOST_REQUIRE(previous);
  const nsf::ControllerVersion next{previous->controllerGenerationTimestamp, previous->controllerEpoch + 1};
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  const auto certificate = environment->keyChain().getPib()
    .getIdentity(environment->profile().attributeAuthority)
    .getDefaultKey().getDefaultCertificate();
  nsf::PolicyStatusData status;
  status.setServiceName(SERVICE_TRACKING_WINDOW);
  status.setControllerVersion(next);
  status.setValidity(now - 1000, now + 120000);
  status.setPolicyDigest("sha256:" + std::string(64, 'b'));
  status.setAbePublicParametersName(environment->attributeAuthorityPublicParametersName());
  status.setAbePublicParametersDigest(environment->attributeAuthorityPublicParametersDigest());
  status.setControllerCertificate(certificate.getName());
  nsf::RevocationTarget target;
  target.kind = nsf::RevocationKind::IDENTITY;
  target.targetIdentity = definition.computeIdentity;
  status.addRevocation(target);
  ndn::Data packet(nsf::makePolicyStatusName(certificate.getIdentity(),
                                           SERVICE_TRACKING_WINDOW, next));
  packet.setContent(status.wireEncode());
  environment->keyChain().sign(packet, ndn::security::signingByCertificate(certificate));
  const auto signatureValid = ndn::security::verifySignature(packet, certificate);
  BOOST_REQUIRE(signatureValid);
  // The actual work fence checks this installed status/version, not a renewed table alone.
  BOOST_REQUIRE(compute.installControllerStatus(status, signatureValid));
  BOOST_REQUIRE(compute.getControllerVersion(SERVICE_TRACKING_WINDOW));
  BOOST_CHECK(*compute.getControllerVersion(SERVICE_TRACKING_WINDOW) == next);
  releaseThird.store(true);
  awaitTerminal();
  BOOST_CHECK(!response || !response->getStatus());
  BOOST_CHECK_EQUAL(backendCalls(), 0U);
  BOOST_CHECK(!committed());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
