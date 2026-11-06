#include "tests/fixtures/uav-tracking-core-flow.hpp"
#include "tests/fixtures/uav-tracking-provider-access.hpp"

#include <cerrno>
#include <csignal>
#include <iterator>
#include <future>
#include <boost/scope_exit.hpp>

#include "NDNSF-UAV-APP/shared/uav-person-published-frame.hpp"
#include "NDNSF-UAV-APP/shared/UavTrackingWorkerTransaction.hpp"
#include "NDNSF-UAV-APP/tracking/uav-tracking-provider.hpp"
#include "NDNSF-UAV-APP/tracking/UavTrackingCoordinator.hpp"

#include <sys/inotify.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace ndnsf::test {
namespace {

using namespace tracking_flow;

const std::string CALIBRATION_EVENT =
  R"({"schema":"uav-source-event-v2","kind":"calibration","payload":{"schema":"uav-calibration-v2","cameraId":"UAV1","revision":"3","width":"640","height":"480","fx":"500","fy":"500","cx":"320","cy":"240","distortion":["0","0","0","0","0"],"imageTransform":"raw","validFromUs":"0","validUntilUs":"10000000"}})";
const std::string FRAME_EVENT =
  R"({"schema":"uav-source-event-v2","kind":"frame","payload":{"key":{"sourceId":"camera-source-1","cameraId":"UAV1","epoch":"1","sequence":"1"},"deviceTimeUs":"1000","clockBinding":{"revision":"1","clockId":"sim-clock","clockEpoch":"2","a":"1","bUs":"0","validFromUs":"0","validUntilUs":"10000000","uncertaintyUs":"0"},"width":"640","height":"480","encoding":"image/jpeg","calibrationRevision":"3","imageBase64":"/9j/2Q=="}})";

struct ProviderFixture : JobFlowFixture
{
  ProviderFixture()
    : JobFlowFixture(true)
  {
    std::filesystem::create_directory(root / "streams");
    std::ofstream(root / "streams/window-0.jsonl") << CALIBRATION_EVENT << '\n' << FRAME_EVENT << '\n';
    providerConfig.producerIdentity = definition.sources[0].producerIdentity;
    providerConfig.droneId = "actual-source-drone";
    providerConfig.ownedWorkspace = root / "provider-work";
    TrackingProviderRunDefinition run;
    run.runId = definition.runId;
    run.mode = TrackingMode::VEHICLE;
    run.role = TrackingProviderRole::SOURCE;
    run.cameraId = "UAV1";
    for (size_t i = 0; i < 3; ++i) run.sourceProducers[i] = definition.sources[i].producerIdentity;
    run.computeProducer = definition.computeIdentity;
    run.windowCount = 1;
    run.sourceGenerations = definition.sources[0].sourceManifest;
    run.observationStreamDirectory = root / "streams";
    run.worldFrameId = "synthetic-local";
    run.acquisitionKind = "simulated";
    run.deliveryMode = "replay";
    providerConfig.runs.push_back(std::move(run));
  }

  ~ProviderFixture()
  {
    stopAndDrainJob();
    if (capability) {
      capability->stopAdmission();
      capability->cancelActiveWork();
      environment->pumpUntilWithAttributeAuthority([this] { return capability->isDrained(); });
      BOOST_REQUIRE(capability->isDrained());
      capability.reset();
    }
    for (auto& source : sourceCapabilities) {
      source->stopAdmission();
      source->cancelActiveWork();
      environment->pumpUntilWithAttributeAuthority([&source] { return source->isDrained(); });
      BOOST_REQUIRE(source->isDrained());
    }
    sourceCapabilities.clear();
    if (writer >= 0) ::close(writer);
    if (watch >= 0) ::close(watch);
  }

  void startProvider()
  {
    capability = std::make_unique<UavTrackingProvider>(providerConfig);
    capability->registerServices(environment->provider());
    install(false, false, false, false, {}, 1);
  }

  void startFailingCompute()
  {
    providerConfig.producerIdentity = definition.computeIdentity;
    auto& run = providerConfig.runs[0];
    run.role = TrackingProviderRole::COMPUTE;
    run.cameraId.clear();
    run.observationStreamDirectory.clear();
    const auto asset = root / "compute-placeholder";
    std::ofstream(asset) << "abc";
    run.model = run.workerPython = run.workerScript = {asset, digestOf("abc")};
    // These declared assets are valid; the existing vehicle initializer also
    // requires motion_estimator.py beside its worker. It is deliberately absent.
    capability = std::make_unique<UavTrackingProvider>(providerConfig);
    capability->registerServices(environment->provider(3));
    install(false, false, false, false, {}, 0, 3);
  }

  void holdSource()
  {
    const auto path = root / "streams/window-0.jsonl";
    std::filesystem::remove(path);
    BOOST_REQUIRE_EQUAL(::mkfifo(path.c_str(), 0600), 0);
    writer = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    BOOST_REQUIRE(writer >= 0);
    watch = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    BOOST_REQUIRE(watch >= 0);
    BOOST_REQUIRE(::inotify_add_watch(watch, path.c_str(), IN_OPEN) >= 0);
  }

  void startBlockingCompute(bool successfulCandidate = false)
  {
    // Real native results cross the Core's 6000-byte boundary. Configure the
    // maintained Controller default, never raise the inline threshold.
    if (successfulCandidate) {
      environment->user().fetchPermissionsFromController(environment->profile().attributeAuthority);
      BOOST_REQUIRE(environment->user().hasRequestScopedDefault());
    }
    for (size_t i = 0; i < 3; ++i) {
      auto config = providerConfig;
      config.producerIdentity = definition.sources[i].producerIdentity;
      config.droneId = "source-drone-" + std::to_string(i);
      config.ownedWorkspace = root / ("native-source-" + std::to_string(i));
      auto& run = config.runs[0];
      run.cameraId = "UAV" + std::to_string(i + 1);
      run.observationStreamDirectory = root / ("source-input-" + std::to_string(i));
      std::filesystem::create_directory(run.observationStreamDirectory);
      auto calibration = Json::parse(CALIBRATION_EVENT);
      calibration["payload"]["cameraId"] = run.cameraId;
      auto frame = Json::parse(FRAME_EVENT);
      frame["payload"]["key"]["cameraId"] = run.cameraId;
      frame["payload"]["key"]["sourceId"] = definition.sources[i].sourceId;
      std::ofstream(run.observationStreamDirectory / "window-0.jsonl")
        << calibration.dump() << '\n' << frame.dump() << '\n';
      auto source = std::make_unique<UavTrackingProvider>(config);
      source->registerServices(environment->provider(i));
      sourceCapabilities.push_back(std::move(source));
    }
    const auto fifo = root / "compute-release";
    BOOST_REQUIRE_EQUAL(::mkfifo(fifo.c_str(), 0600), 0);
    writer = ::open(fifo.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    BOOST_REQUIRE(writer >= 0);
    const auto script = root / "controlled-backend.sh";
    Json frames = Json::array();
    Json cameras = Json::object();
    for (const auto* camera : {"UAV1", "UAV2", "UAV3"}) {
      frames.push_back({{"cameraId", camera}, {"width", uint64_t(640)}, {"height", uint64_t(480)},
        {"detections", Json::array({{{"box", {10, 20, 50, 60}}, {"confidence", 0.9},
                                    {"class", "car"}, {"byteTrackId", nullptr}}})}});
      cameras[camera] = nullptr;
    }
    const auto candidate = Json{{"schema", "uav-vehicle-backend-window-v1"},
      {"runId", definition.runId}, {"missionId", "tracking-" + definition.runId},
      {"windowId", "window-0"}, {"sequence", uint64_t(1)}, {"actualDevice", "cpu"},
      {"frames", frames}, {"backendState", {{"schema", "uav-vehicle-backend-state-v1"},
                                            {"cameras", cameras}}}};
    std::ofstream(root / "controlled-candidate.json") << candidate.dump();
    auto scriptText = "#!/bin/sh\n: > '" + (root / "compute-entered").string() +
      "'\nIFS= read -r value < '" + fifo.string() + "'\n";
    scriptText += successfulCandidate
      ? "while [ \"$#\" -gt 0 ]; do case \"$1\" in --output-json) output=$2; shift 2;; *) shift;; esac; done\n"
        "cat '" + (root / "controlled-candidate.json").string() + "' > \"$output\"\n"
      : "exit 9\n";
    std::ofstream(script) << scriptText;
    for (const auto* helper : {"motion_estimator.py", "motion_schema.py", "observation_schema.py",
                               "byte_track_state.py"}) {
      std::ofstream(root / helper) << "controlled-backend-placeholder\n";
    }
    std::ofstream(root / "model") << "controlled-model\n";
    const auto shellPath = std::filesystem::canonical("/bin/sh");
    std::ifstream shell(shellPath, std::ios::binary);
    const std::string shellBytes((std::istreambuf_iterator<char>(shell)), {});
    providerConfig.producerIdentity = definition.computeIdentity;
    auto& run = providerConfig.runs[0];
    run.role = TrackingProviderRole::COMPUTE;
    run.cameraId.clear();
    run.observationStreamDirectory.clear();
    run.model = {root / "model", digestOf("controlled-model\n")};
    run.workerPython = {shellPath, digestOf(shellBytes)};
    run.workerScript = {script, digestOf(scriptText)};
    capability = std::make_unique<UavTrackingProvider>(providerConfig);
    capability->registerServices(environment->provider(3));
    environment->provider(3).addService(SERVICE_TRACKING_SOURCE,
      [] (const nsf::RequestMessage&) {
        nsf::ServiceProvider::AckDecision decision;
        decision.status = true;
        return decision;
      },
      [] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
          const nsf::RequestMessage&) {
        nsf::ResponseMessage response;
        response.setStatus(true);
        return response;
      }, nsf::ServiceProvider::ServiceInvocationMode::NormalAndTargeted);
    install(false, false, false, false, {}, 3, 3);
  }

  bool sourceOpened()
  {
    alignas(inotify_event) char events[4096];
    const auto count = ::read(watch, events, sizeof(events));
    return count > 0;
  }

  nsf::RequestMessage frameRequest() const
  {
    UavPersonFrameReadBinding binding;
    binding.runId = definition.runId;
    binding.missionId = "tracking-" + definition.runId;
    binding.windowId = "window-0";
    binding.cameraId = "UAV1";
    binding.sourceId = definition.sources[0].sourceId;
    binding.sourceEpoch = 1;
    binding.sequence = 1;
    binding.frameName = makeUavObservationDataName(definition.sources[0].producerIdentity,
      "UAV1", 1, 1, "FRAME", 1).toUri();
    const std::string jpeg("\xff\xd8\xff\xd9", 4);
    binding.inputDigest = digestOf(jpeg);
    auto bytes = asFields(makeUavPersonFrameReadFields(binding));
    nsf::RequestMessage request;
    request.setPayload(bytes, bytes.size());
    return request;
  }

  UavTrackingProviderConfig providerConfig;
  std::unique_ptr<UavTrackingProvider> capability;
  std::vector<std::unique_ptr<UavTrackingProvider>> sourceCapabilities;
  int writer = -1;
  int watch = -1;
};

} // namespace

BOOST_AUTO_TEST_SUITE(UavTrackingProviderLifecycle)

BOOST_FIXTURE_TEST_CASE(NoncanonicalWindowsCannotAliasAnAdmittedSourceWindow, ProviderFixture)
{
  capability = std::make_unique<UavTrackingProvider>(providerConfig);
  Fields fields{{"schema", "spec191-window-request-v1"}, {"tracking_request_timeout_ms", "10000"}, {"run_id", definition.runId},
    {"mission_id", "tracking-" + definition.runId}, {"window_id", "window-0"},
    {"observation_schema", "uav-observation-v2"}, {"source_epoch", "1"}};
  for (size_t i = 0; i < 3; ++i) {
    fields["source_uav" + std::to_string(i + 1) + "_id"] = definition.sources[i].sourceId;
  }
  const auto decision = [&] {
    nsf::RequestMessage request;
    auto payload = asFields(fields);
    request.setPayload(payload, payload.size());
    return UavTrackingProviderTestAccess::acknowledge(*capability, request);
  };
  BOOST_REQUIRE(decision().status);
  for (const auto* invalid : {"window-00", "window-0suffix", "window-+0", "window--0"}) {
    fields["window_id"] = invalid;
    BOOST_CHECK(!decision().status);
  }
}

BOOST_FIXTURE_TEST_CASE(ProtectedAckReadsDiscoveryButCannotReplaceFormalInput, ProviderFixture)
{
  capability = std::make_unique<UavTrackingProvider>(providerConfig);
  Fields fields{{"schema", "spec191-window-request-v1"}, {"tracking_request_timeout_ms", "10000"}, {"run_id", definition.runId},
    {"mission_id", "tracking-" + definition.runId}, {"window_id", "window-0"},
    {"observation_schema", "uav-observation-v2"}, {"source_epoch", "1"}};
  for (size_t i = 0; i < 3; ++i) {
    fields["source_uav" + std::to_string(i + 1) + "_id"] = definition.sources[i].sourceId;
  }
  nsf::RequestCapabilities caps;
  caps.setField("RequestScopedConfidentialityV1", "required");
  caps.setField("UavTrackingDiscoveryV1", "required");
  for (const auto& [key, value] : fields) caps.setField("UavTracking." + key, value);
  nsf::RequestMessage request;
  request.setRequestCapabilities(caps);
  BOOST_CHECK(UavTrackingProviderTestAccess::acknowledge(*capability, request).status);
  BOOST_CHECK_THROW(validateUavTrackingDiscoveryBinding(request), std::invalid_argument);
  caps.setField("UavTracking.source_epoch", "2");
  request.setRequestCapabilities(caps);
  BOOST_CHECK(!UavTrackingProviderTestAccess::acknowledge(*capability, request).status);
  caps.setField("UavTracking.source_epoch", "1");
  caps.setField("UavTracking.run_id", "undeclared-run");
  request.setRequestCapabilities(caps);
  BOOST_CHECK(!UavTrackingProviderTestAccess::acknowledge(*capability, request).status);
}

BOOST_FIXTURE_TEST_CASE(RequestBudgetControlsAckRetentionAndRejectsUnpreparedWork, ProviderFixture)
{
  capability = std::make_unique<UavTrackingProvider>(providerConfig);
  Fields fields{{"schema", "spec191-window-request-v1"}, {"run_id", definition.runId},
    {"mission_id", "tracking-" + definition.runId}, {"window_id", "window-0"},
    {"observation_schema", "uav-observation-v2"}, {"source_epoch", "1"},
    {"tracking_request_timeout_ms", "10000"}};
  for (size_t i = 0; i < 3; ++i) {
    fields["source_uav" + std::to_string(i + 1) + "_id"] = definition.sources[i].sourceId;
  }
  const auto decide = [&] {
    nsf::RequestMessage request;
    auto payload = asFields(fields);
    request.setPayload(payload, payload.size());
    return UavTrackingProviderTestAccess::acknowledge(*capability, request);
  };
  BOOST_REQUIRE(decide().status);
  BOOST_CHECK_EQUAL(decide().pendingStateTtlMs, 10000);
  for (const auto* bad : {"0", "1000", "010001", "+10001", "10001tail", "30001", "1200001"}) {
    fields["tracking_request_timeout_ms"] = bad;
    BOOST_CHECK(!decide().status);
  }
  fields.erase("tracking_request_timeout_ms");
  BOOST_CHECK(!decide().status);
  auto overHost = providerConfig;
  overHost.runs[0].requestTimeoutMs = 30001;
  auto notReady = std::make_unique<UavTrackingProvider>(overHost);
  fields["tracking_request_timeout_ms"] = "30001";
  nsf::RequestMessage request;
  auto bytes = asFields(fields);
  request.setPayload(bytes, bytes.size());
  BOOST_CHECK(!UavTrackingProviderTestAccess::acknowledge(*notReady, request).status);
}

BOOST_FIXTURE_TEST_CASE(ExplicitHostAndPreparedBudgetPreserveLongerAckHorizon, ProviderFixture)
{
  const auto* configured = std::getenv("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS");
  const auto previous = configured ? std::optional<std::string>(configured) : std::nullopt;
  BOOST_SCOPE_EXIT_ALL(&, previous) {
    if (previous) ::setenv("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS", previous->c_str(), 1);
    else ::unsetenv("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS");
  };
  BOOST_REQUIRE_EQUAL(::setenv("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS", "60000", 1), 0);
  providerConfig.runs[0].requestTimeoutMs = 60000;
  capability = std::make_unique<UavTrackingProvider>(providerConfig);
  Fields fields{{"schema", "spec191-window-request-v1"}, {"run_id", definition.runId},
    {"mission_id", "tracking-" + definition.runId}, {"window_id", "window-0"},
    {"observation_schema", "uav-observation-v2"}, {"source_epoch", "1"},
    {"tracking_request_timeout_ms", "60000"}};
  for (size_t i = 0; i < 3; ++i) {
    fields["source_uav" + std::to_string(i + 1) + "_id"] = definition.sources[i].sourceId;
  }
  auto bytes = asFields(fields);
  nsf::RequestMessage request;
  request.setPayload(bytes, bytes.size());
  const auto decision = UavTrackingProviderTestAccess::acknowledge(*capability, request);
  BOOST_REQUIRE(decision.status);
  BOOST_CHECK_EQUAL(decision.pendingStateTtlMs, 60000);
}

BOOST_FIXTURE_TEST_CASE(CommitAuthorityUsesTheCompleteDispatchFence, ProviderFixture)
{
  for (const auto* scenario : {"valid", "expired", "not-current", "missing", "throw", "revoked"}) {
    nsf::ServiceProvider::CollaborationAssignment assignment;
    assignment.role = "Tracking-Compute";
    assignment.service = SERVICE_TRACKING_WINDOW;
    assignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    assignment.workCurrent = [] { return true; };
    assignment.scopeKeys["test-scope"] = ndn::Buffer(32, 0x42);
    const std::string name(scenario);
    if (name == "expired") {
      assignment.workDeadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    }
    if (name == "not-current") assignment.workCurrent = [] { return false; };
    if (name == "missing") assignment.workCurrent = {};
    if (name == "throw") {
      assignment.workCurrent = [] () -> bool { throw std::runtime_error("authority unavailable"); };
    }
    nsf::ServiceProvider::CollaborationContext context(environment->provider(3),
      environment->profile().userIdentity, ndn::Name("/test/authority").append(name),
      nsf::RequestMessage{}, assignment);
    if (name == "revoked") BOOST_CHECK(context.revokeCurrentWorkAuthority());
    const bool valid = name == "valid";
    BOOST_CHECK_EQUAL(context.hasCurrentWorkAuthority(), valid);
    UavTrackingWorkerTransaction transaction(root / ("authority-" + name));
    const bool committed = transaction.commit("window-0", tracking_fixture::ABC_DIGEST,
      asBuffer("result"), asBuffer("state"), [&context] { return context.hasCurrentWorkAuthority(); });
    BOOST_CHECK_EQUAL(committed, valid);
    BOOST_CHECK_EQUAL(transaction.committedStatePath("window-0").has_value(), valid);
    const auto dataName = ndn::Name(environment->provider(3).getName())
      .append("Result").append(name).appendVersion(1);
    if (name == "throw") {
      BOOST_CHECK_THROW(context.publishReusableLargeNamed("test-scope", dataName,
        asBuffer("result")), std::runtime_error);
    }
    else if (!valid) {
      BOOST_CHECK(context.publishReusableLargeNamed("test-scope", dataName,
        asBuffer("result")).empty());
    }
    else {
      auto publication = std::async(std::launch::async, [&] {
        return context.publishReusableLargeNamed("test-scope", dataName, asBuffer("result"));
      });
      environment->pumpUntilWithAttributeAuthority([&] {
        return publication.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
      });
      BOOST_CHECK(publication.get() == dataName);
    }
  }
}

BOOST_FIXTURE_TEST_CASE(IdleChildShutdownDoesNotBlockTheHostFaceAndReapsItsChild, ProviderFixture)
{
  providerConfig.runs.clear();
  capability = std::make_unique<UavTrackingProvider>(providerConfig);
  auto session = std::make_shared<UavTrackingWorkerSession>(std::vector<std::string>{
    "/bin/sh", "-c",
    "trap '' TERM; read request; echo $$; echo READY; while :; do sleep 1; done"}, 4096);
  const auto ready = session->exchange("start\n", "READY\n", std::chrono::milliseconds(2000),
                                       [] { return true; });
  BOOST_REQUIRE(ready.outcome == UavTrackingWorkerOutcome::SUCCESS);
  const auto child = static_cast<pid_t>(std::stol(ready.stdoutText));
  BOOST_REQUIRE(child > 1);
  BOOST_REQUIRE_EQUAL(::kill(child, 0), 0);
  UavTrackingProviderTestAccess::adoptIdleSession(*capability, std::move(session));
  capability->stopAdmission();
  capability->cancelActiveWork();
  capability->cancelActiveWork();
  auto callbackRan = std::make_shared<bool>(false);
  auto childStillAlive = std::make_shared<bool>(false);
  boost::asio::post(environment->userFace().getIoContext(), [=] {
    *childStillAlive = ::kill(child, 0) == 0;
    *callbackRan = true;
  });
  environment->pumpUntilWithAttributeAuthority([=] { return *callbackRan; });
  BOOST_REQUIRE(*callbackRan);
  BOOST_CHECK(*childStillAlive);
  environment->pumpUntilWithAttributeAuthority([this] { return capability->isDrained(); });
  BOOST_REQUIRE(capability->isDrained());
  errno = 0;
  BOOST_CHECK_EQUAL(::kill(child, 0), -1);
  BOOST_CHECK_EQUAL(errno, ESRCH);
}

BOOST_FIXTURE_TEST_CASE(InitialAssetAndHandlerInspectionHonorsTheSynchronousAuthorityPredicate,
                        ProviderFixture)
{
  startFailingCompute();
  const auto cancelled = [] (const std::exception& error) {
    return std::string(error.what()).find("inspection cancelled") != std::string::npos;
  };
  BOOST_CHECK_EXCEPTION(UavTrackingProviderTestAccess::verifyAssets(
    *capability, definition.runId, [] { return false; }), std::runtime_error, cancelled);
  auto assetChecks = std::make_shared<unsigned>(0);
  BOOST_CHECK_EXCEPTION(UavTrackingProviderTestAccess::verifyAssets(
    *capability, definition.runId, [assetChecks] { return (*assetChecks)++ == 0; }),
    std::runtime_error, cancelled);
  BOOST_CHECK(*assetChecks >= 2);
  BOOST_CHECK_NO_THROW(UavTrackingProviderTestAccess::verifyAssets(
    *capability, definition.runId, [] { return true; }));
  BOOST_CHECK_EXCEPTION(UavTrackingProviderTestAccess::initializeHandlers(
    *capability, definition.runId, [] { return false; }), std::runtime_error, cancelled);
  auto handlerChecks = std::make_shared<unsigned>(0);
  BOOST_CHECK_EXCEPTION(UavTrackingProviderTestAccess::initializeHandlers(
    *capability, definition.runId, [handlerChecks] { return (*handlerChecks)++ == 0; }),
    std::runtime_error, cancelled);
  BOOST_CHECK(*handlerChecks >= 2);
  // Positive authority reaches the actual existing helper dependency check.
  BOOST_CHECK_EXCEPTION(UavTrackingProviderTestAccess::initializeHandlers(
    *capability, definition.runId, [] { return true; }), std::runtime_error,
    [] (const std::exception& error) {
      return std::string(error.what()).find("motion_estimator.py") != std::string::npos;
    });
}

BOOST_FIXTURE_TEST_CASE(FirstHandlerInitializationFailurePublishesWorkDrained, ProviderFixture)
{
  startFailingCompute();
  const auto payload = asFields({{"schema", "spec191-window-request-v1"}, {"tracking_request_timeout_ms", "3000"},
    {"run_id", definition.runId}, {"mission_id", "tracking-" + definition.runId},
    {"window_id", "window-0"}, {"observation_schema", "uav-observation-v2"},
    {"source_epoch", "1"}, {"source_uav1_id", definition.sources[0].sourceId},
    {"source_uav2_id", definition.sources[1].sourceId},
    {"source_uav3_id", definition.sources[2].sourceId}});
  std::vector<ndn::Name> sources;
  for (const auto& source : definition.sources) sources.push_back(source.producerIdentity);
  auto committed = std::make_shared<bool>(false);
  auto responseReceived = std::make_shared<bool>(false);
  UavTrackingCoordinatorCallbacks callbacks;
  callbacks.onPlanCommitted = [committed] (const auto&, const auto&) { *committed = true; };
  callbacks.onFailure = [] (const auto& reason) { BOOST_FAIL(reason); };
  callbacks.onTimeout = [] (const auto&) { BOOST_FAIL("initialization failure response missing"); };
  callbacks.onResponse = [responseReceived] (const nsf::ResponseMessage& response) {
    BOOST_CHECK(!response.getStatus());
    const auto bytes = response.getPayload();
    const auto fields = decodeFields(std::string(bytes.begin(), bytes.end()));
    BOOST_CHECK(fieldOr(fields, "failure_reason", "").find("motion_estimator.py") !=
                std::string::npos);
    *responseReceived = true;
  };
  const auto request = UavTrackingCoordinator{}.begin(environment->user(), SERVICE_TRACKING_WINDOW,
    payload, UavTrackingCoordinator::makePlan(sources, definition.computeIdentity, payload, 1000, 3000),
    std::move(callbacks));
  BOOST_REQUIRE(!request.empty());
  environment->pumpUntilWithAttributeAuthority([responseReceived] { return *responseReceived; });
  BOOST_REQUIRE(*committed);
  BOOST_REQUIRE(*responseReceived);
  environment->pumpUntilWithAttributeAuthority([&] {
    const auto available = environment->user().waitForVerifiedCollaborationData(request,
      TRACKING_INPUT_KEY_SCOPE, TRACKING_WORK_DRAINED_TOPIC, 1, 1, false);
    return std::any_of(available.begin(), available.end(), [this] (const auto& record) {
      return record.producer == definition.computeIdentity && record.producerRole == "Tracking-Compute";
    });
  });
  const auto records = environment->user().waitForVerifiedCollaborationData(request,
    TRACKING_INPUT_KEY_SCOPE, TRACKING_WORK_DRAINED_TOPIC, 1, 1, false);
  const auto proof = std::find_if(records.begin(), records.end(), [this] (const auto& record) {
    return record.producer == definition.computeIdentity && record.producerRole == "Tracking-Compute";
  });
  BOOST_REQUIRE(proof != records.end());
  const auto fields = decodeFields(std::string(proof->payload.begin(), proof->payload.end()));
  BOOST_CHECK_EQUAL(fieldOr(fields, "work_drained", ""), "true");
  BOOST_CHECK_EQUAL(fieldOr(fields, "request_id", ""), request.toUri());
  BOOST_CHECK_EQUAL(fieldOr(fields, "run_id", ""), definition.runId);
  BOOST_CHECK_EQUAL(fieldOr(fields, "mission_id", ""), "tracking-" + definition.runId);
  BOOST_CHECK_EQUAL(fieldOr(fields, "source_epoch", ""), "1");
  environment->user().CancelCollaboration(request);
}

BOOST_FIXTURE_TEST_CASE(InitializationFailureLeavesTheJobFailedButClean, ProviderFixture)
{
  startFailingCompute();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] {
    return latest.state == TrackingJobState::FAILED && latest.cleanupState == "CLEAN";
  });
  BOOST_REQUIRE(latest.state == TrackingJobState::FAILED);
  BOOST_CHECK_EQUAL(latest.cleanupState, "CLEAN");
  BOOST_CHECK_EQUAL(latest.completedWindows, 0);
}

BOOST_FIXTURE_TEST_CASE(SourceUsesInjectedProducerAndServesOnlyItsPublishedFrame, ProviderFixture)
{
  startProvider();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_REQUIRE(latest.state == TrackingJobState::COMPLETED);
  auto done = std::make_shared<bool>(false);
  auto received = std::make_shared<ndn::Buffer>();
  auto success = std::make_shared<bool>(false);
  const auto request = environment->user().RequestServiceTargeted(definition.sources[0].producerIdentity,
    SERVICE_TRACKING_SOURCE, frameRequest(), 3000,
    [done] (const ndn::Name&) { *done = true; },
    [done, received, success] (const nsf::ResponseMessage& response) {
      *success = response.getStatus();
      *received = response.getPayload();
      *done = true;
    });
  BOOST_REQUIRE(!request.empty());
  environment->pumpUntilWithAttributeAuthority([done] { return *done; });
  BOOST_REQUIRE(*done);
  BOOST_REQUIRE(*success);
  const ndn::Buffer expected{0xff, 0xd8, 0xff, 0xd9};
  BOOST_CHECK_EQUAL_COLLECTIONS(received->begin(), received->end(), expected.begin(), expected.end());
}

BOOST_FIXTURE_TEST_CASE(ShutdownCancelsAnOwnedSourceReaderAndPreservesTheHostFace, ProviderFixture)
{
  holdSource();
  startProvider();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  bool opened = false;
  environment->pumpUntilWithAttributeAuthority([&] { return opened = sourceOpened(); });
  BOOST_REQUIRE(opened);
  capability->stopAdmission();
  capability->cancelActiveWork();
  environment->pumpUntilWithAttributeAuthority([this] { return capability->isDrained(); });
  BOOST_REQUIRE(capability->isDrained());
  auto alive = std::make_shared<bool>(false);
  boost::asio::post(environment->userFace().getIoContext(), [alive] { *alive = true; });
  environment->pumpUntilWithAttributeAuthority([alive] { return *alive; });
  BOOST_CHECK(*alive);
}

BOOST_FIXTURE_TEST_CASE(OrdinaryServiceRepliesWhileNativeComputeOwnsItsWorker, ProviderFixture)
{
  startBlockingCompute();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] {
    return std::filesystem::exists(root / "compute-entered");
  });
  BOOST_REQUIRE(std::filesystem::exists(root / "compute-entered"));
  BOOST_REQUIRE(latest.state == TrackingJobState::RUNNING);
  BOOST_CHECK(!capability->isDrained());
  auto done = std::make_shared<bool>(false);
  auto replied = std::make_shared<bool>(false);
  nsf::RequestMessage request;
  auto payload = asBuffer("ordinary-service-isolation");
  request.setPayload(payload, payload.size());
  const auto requestId = environment->user().RequestServiceTargeted(definition.computeIdentity,
    SERVICE_TRACKING_SOURCE, request, 2000,
    [done] (const ndn::Name&) { *done = true; },
    [done, replied] (const nsf::ResponseMessage& response) {
      *replied = response.getStatus();
      *done = true;
    });
  BOOST_REQUIRE(!requestId.empty());
  environment->pumpUntilWithAttributeAuthority([done] { return *done; });
  BOOST_REQUIRE(*replied);
  BOOST_CHECK(latest.state == TrackingJobState::RUNNING);
  BOOST_CHECK(!capability->isDrained());
  const std::string release = "release\n";
  BOOST_REQUIRE_EQUAL(::write(writer, release.data(), release.size()), static_cast<ssize_t>(release.size()));
  environment->pumpUntilWithAttributeAuthority([this] {
    return latest.state == TrackingJobState::FAILED && latest.cleanupState == "CLEAN";
  });
  BOOST_CHECK(latest.state == TrackingJobState::FAILED);
  BOOST_CHECK_EQUAL(latest.cleanupState, "CLEAN");
}

BOOST_FIXTURE_TEST_CASE(NativeComputeCommitsOnlyCppDecisionsAndExportsTheSameMeasurements, ProviderFixture)
{
  startBlockingCompute(true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] {
    return std::filesystem::exists(root / "compute-entered");
  });
  BOOST_REQUIRE(std::filesystem::exists(root / "compute-entered"));
  const std::string release = "release\n";
  BOOST_REQUIRE_EQUAL(::write(writer, release.data(), release.size()), static_cast<ssize_t>(release.size()));
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_REQUIRE(latest.state == TrackingJobState::COMPLETED);
  BOOST_REQUIRE(latest.latestResult);
  const auto result = Json::parse(*latest.latestResult);
  BOOST_CHECK_EQUAL(result["decisionSchema"], "uav-vehicle-native-decision-v1");
  BOOST_CHECK_EQUAL(latest.actualDevice, "cpu");
  BOOST_CHECK_EQUAL(latest.sourcePtsUs, 1000);
  for (size_t i = 0; i < 3; ++i) {
    BOOST_CHECK_EQUAL(result["frames"][i]["tracks"][0]["localId"], 1);
    BOOST_CHECK_EQUAL(result["frames"][i]["tracks"][0]["globalId"], i + 1);
    BOOST_CHECK_EQUAL(result["frames"][i]["inputDigest"], digestOf(std::string("\xff\xd8\xff\xd9", 4)));
  }
  const auto worker = providerConfig.ownedWorkspace / "vehicle/native-default" / definition.runId;
  UavTrackingWorkerTransaction transactions(worker / "transactions" / digestOf("tracking-" + definition.runId).substr(7));
  const auto committed = transactions.committedStatePath("window-0");
  BOOST_REQUIRE(committed);
  std::ifstream stateFile(*committed);
  const auto state = Json::parse(stateFile);
  BOOST_CHECK_EQUAL(state["schema"], "uav-vehicle-native-state-v1");
  BOOST_CHECK_EQUAL(state["nativeDecisionDigest"], result["nativeDecisionDigest"]);
  std::ifstream csv(worker / "annotated/motion-measurements-000001.csv");
  BOOST_REQUIRE(csv);
  const std::string text((std::istreambuf_iterator<char>(csv)), {});
  BOOST_CHECK(text.find("schema,sourceId,cameraId,sourceEpoch,sequence,localTrackId,trackGeneration,globalId,") == 0);
  BOOST_CHECK(text.find("camera-source-1,UAV1,1,1,1,1,1,") != std::string::npos);
  BOOST_CHECK(text.find(",unknown,unknown,unknown,") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(CancelledNativeCandidateNeverPromotesATransactionMarker, ProviderFixture)
{
  startBlockingCompute(true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] {
    return std::filesystem::exists(root / "compute-entered");
  });
  BOOST_REQUIRE(std::filesystem::exists(root / "compute-entered"));
  BOOST_REQUIRE(job->requestCancel(definition.jobId) == TrackingCancelResult::REQUESTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_CHECK(latest.state == TrackingJobState::CANCELLED);
  BOOST_CHECK_EQUAL(latest.completedWindows, 0);
  const auto worker = providerConfig.ownedWorkspace / "vehicle/native-default" / definition.runId;
  UavTrackingWorkerTransaction transactions(worker / "transactions" / digestOf("tracking-" + definition.runId).substr(7));
  BOOST_CHECK(!transactions.committedStatePath("window-0"));
}

BOOST_FIXTURE_TEST_CASE(BackendCannotInjectGlobalDecisionsIntoCommittedState, ProviderFixture)
{
  startBlockingCompute(true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] {
    return std::filesystem::exists(root / "compute-entered");
  });
  BOOST_REQUIRE(std::filesystem::exists(root / "compute-entered"));
  const auto path = root / "controlled-candidate.json";
  std::ifstream input(path);
  auto candidate = Json::parse(input);
  input.close();
  candidate["frames"][0]["detections"][0]["globalId"] = 999;
  std::ofstream(path) << candidate.dump();
  const std::string release = "release\n";
  BOOST_REQUIRE_EQUAL(::write(writer, release.data(), release.size()),
                      static_cast<ssize_t>(release.size()));
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_CHECK(latest.state == TrackingJobState::FAILED);
  BOOST_CHECK_EQUAL(latest.completedWindows, 0);
  const auto worker = providerConfig.ownedWorkspace / "vehicle/native-default" / definition.runId;
  UavTrackingWorkerTransaction transactions(
    worker / "transactions" / digestOf("tracking-" + definition.runId).substr(7));
  BOOST_CHECK(!transactions.committedStatePath("window-0"));
}

BOOST_FIXTURE_TEST_CASE(HeavySourceRejectsAnotherRunAndNextPreparedRunExecutes, ProviderFixture)
{
  auto nextGeneration = Json::parse(tracking_fixture::SOURCE_MANIFEST);
  nextGeneration["windowCount"] = 1;
  nextGeneration["generations"][0]["sourceEpoch"] = 2;
  const auto manifest = nextGeneration.dump();
  std::ofstream(root / "next-generation") << manifest;
  auto nextRun = providerConfig.runs[0];
  nextRun.runId = "run-2";
  nextRun.sourceGenerations = {root / "next-generation", digestOf(manifest)};
  nextRun.observationStreamDirectory = root / "next-streams";
  std::filesystem::create_directory(nextRun.observationStreamDirectory);
  auto nextFrame = Json::parse(FRAME_EVENT);
  nextFrame["payload"]["key"]["epoch"] = "2";
  std::ofstream(nextRun.observationStreamDirectory / "window-0.jsonl")
    << CALIBRATION_EVENT << '\n' << nextFrame.dump() << '\n';
  providerConfig.runs.push_back(nextRun);
  holdSource();
  startProvider();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  bool opened = false;
  environment->pumpUntilWithAttributeAuthority([&] { return opened = sourceOpened(); });
  BOOST_REQUIRE(opened);
  std::vector<ndn::Name> sources;
  for (const auto& source : definition.sources) sources.push_back(source.producerIdentity);
  const auto payload = asFields({{"schema", "spec191-window-request-v1"}, {"tracking_request_timeout_ms", "10000"}, {"run_id", "run-2"},
    {"mission_id", "tracking-run-2"}, {"window_id", "window-0"},
    {"observation_schema", "uav-observation-v2"}, {"source_epoch", "2"},
    {"source_uav1_id", definition.sources[0].sourceId},
    {"source_uav2_id", definition.sources[1].sourceId},
    {"source_uav3_id", definition.sources[2].sourceId}});
  nsf::RequestMessage busyRequest;
  auto busyPayload = payload;
  busyRequest.setPayload(busyPayload, busyPayload.size());
  // Invoke the same native ACK decision used by the registered Core wrapper.
  // A short ACK collection timer alone could reject for network latency and
  // would not prove that this specific capability rejected heavy admission.
  const auto decision = UavTrackingProviderTestAccess::acknowledge(*capability, busyRequest);
  BOOST_CHECK(!decision.status);
  BOOST_CHECK_EQUAL(decision.message, "TRACKING_BUSY");
  BOOST_REQUIRE(latest.state == TrackingJobState::RUNNING);
  const auto stream = CALIBRATION_EVENT + '\n' + FRAME_EVENT + '\n';
  BOOST_REQUIRE_EQUAL(::write(writer, stream.data(), stream.size()), static_cast<ssize_t>(stream.size()));
  ::close(writer);
  writer = -1;
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_REQUIRE(latest.state == TrackingJobState::COMPLETED);
  const auto previousPresentation = latest.presentation;
  definition.runId = "run-2";
  definition.jobId = "job-2";
  definition.outputDirectory = root / "output-run-2";
  for (auto& source : definition.sources) {
    source.sourceEpoch = 2;
    source.sourceManifest = nextRun.sourceGenerations;
  }
  std::string admissionReason;
  BOOST_REQUIRE_MESSAGE(job->start(definition, &admissionReason) == TrackingAdmission::ACCEPTED,
                        admissionReason);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_CHECK(latest.state == TrackingJobState::COMPLETED);
  BOOST_REQUIRE(latest.presentation);
  BOOST_CHECK(latest.presentation != previousPresentation);
  BOOST_CHECK_EQUAL(latest.presentation->runId, "run-2");
  BOOST_CHECK_EQUAL(latest.presentation->generation, 2);
  for (const auto& image : latest.presentation->frames) BOOST_CHECK_EQUAL(image.sourceEpoch, 2);
}

BOOST_FIXTURE_TEST_CASE(BadAssetIsNotReadyWhileItsHostStillProcessesCallbacks, ProviderFixture)
{
  providerConfig.runs[0].sourceGenerations.digest = "sha256:" + std::string(64, '0');
  startProvider();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.state == TrackingJobState::FAILED; });
  BOOST_CHECK(latest.state == TrackingJobState::FAILED);
  BOOST_CHECK_EQUAL(executions.load(), 0);
}

BOOST_FIXTURE_TEST_CASE(RejectsConflictingProducerAndReusedEpochCatalogs, ProviderFixture)
{
  auto wrong = providerConfig;
  wrong.runs[0].sourceProducers[0] = wrong.runs[0].computeProducer;
  BOOST_CHECK_THROW(static_cast<void>(UavTrackingProvider{wrong}),
                    std::invalid_argument);
  auto reused = providerConfig.runs[0];
  reused.runId = "another-run";
  providerConfig.runs.push_back(std::move(reused));
  BOOST_CHECK_THROW(static_cast<void>(UavTrackingProvider{providerConfig}),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
