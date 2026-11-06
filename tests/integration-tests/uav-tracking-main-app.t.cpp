#include "NDNSF-UAV-APP/ground-station/uav-tracking-panel.hpp"
#include "NDNSF-UAV-APP/shared/uav-gtk-runtime.hpp"
#include "tests/fixtures/uav-tracking-core-flow.hpp"

namespace {

#include "NDNSF-UAV-APP/ground-station/GroundStationWindow.inc.hpp"

struct GroundStationTrackingPanelTestAccess
{
  static bool allDrawn(const GroundStationTrackingPanel& panel)
  {
    return panel.m_bundle && std::all_of(panel.m_drawn.begin(), panel.m_drawn.end(),
                                        [] (bool value) { return value; });
  }

  static void clickCancel(GroundStationTrackingPanel& panel)
  {
    gtk_button_clicked(panel.m_cancel.gobj());
  }
};

struct GroundStationWindowTestAccess
{
  static bool trackingDrawn(const GroundStationWindow& window)
  {
    return window.m_tabs.get_current_page() == 1 && window.m_trackingPanel &&
      window.m_trackingPanel->get_mapped() &&
      GroundStationTrackingPanelTestAccess::allDrawn(*window.m_trackingPanel);
  }

  static size_t telemetryPollCount(const GroundStationWindow& window)
  {
    return window.m_telemetryPollIndex;
  }

  static sigc::connection telemetryPoll(const GroundStationWindow& window)
  {
    return window.m_telemetryPoll;
  }
};

void
pumpTrackingGtk()
{
  for (size_t i = 0; i < 64 && g_main_context_pending(nullptr); ++i) {
    g_main_context_iteration(nullptr, false);
  }
}

/** Test-only borrowing seam; public commands retain production admission and authority. */
struct GroundStationTrackingTestAccess
{
  static void
  attach(GroundStationServiceContainer& runtime, ndn_service_framework::ServiceUser& user,
         ndn::Face& face)
  {
    runtime.installTrackingOwner(user, face);
    runtime.m_containerReady = true;
  }

  static void
  failRuntime(GroundStationServiceContainer& runtime)
  {
    runtime.handleRuntimeFailure("test startup failure after owner installation");
  }

  static bool
  ownedFaceStopped(const GroundStationServiceContainer& runtime)
  {
    return const_cast<ndn::Face&>(runtime.m_face).getIoContext().stopped();
  }
};

} // namespace

namespace ndnsf::test {
using namespace tracking_flow;

struct MainAppTrackingFixture : JobFlowFixture
{
  MainAppTrackingFixture()
    : JobFlowFixture(true, {UavRuntimeConfig{}.serviceTelemetryStatus})
  {
    stopAndDrainJob();
    releaseCompute->store(false);
    releaseSource->store(false);
    UavRuntimeConfig config;
    runtime = std::make_unique<GroundStationServiceContainer>(
      false, 1000, 10000, config, "UAV1", 1000, 10, 640, 0);
    GroundStationTrackingTestAccess::attach(*runtime, environment->user(), environment->userFace());
  }

  ~MainAppTrackingFixture()
  {
    releaseCompute->store(true);
    releaseSource->store(true);
    holdTelemetry->store(false);
    shutdown();
    runtime.reset();
  }

  void
  pumpUntilTracking(const std::function<bool()>& predicate)
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto done = [&] { return predicate() || std::chrono::steady_clock::now() >= deadline; };
    while (!done()) environment->pumpUntilWithAttributeAuthority(done);
    BOOST_REQUIRE(predicate());
  }

  void
  install(bool rejectCompute = false, bool holdCompute = false, bool wrongFeedback = false,
          bool omitSourceDrain = false, std::string wrongDrainField = {},
          size_t firstSyntheticProvider = 0, size_t lastSyntheticProvider = 4,
          bool holdSourceDrain = false)
  {
    for (size_t i = 0; i < 4; ++i) {
      const auto droneId = i < 3 ? definition.sources[i].droneId : definition.computeDroneId;
      environment->provider(i).addService(UavRuntimeConfig{}.serviceTelemetryStatus,
        [] (const nsf::RequestMessage&) {
          nsf::ServiceProvider::AckDecision decision;
          decision.status = true;
          return decision;
        },
        [calls = telemetryCalls, wrong = wrongTelemetryPayload, hold = holdTelemetry,
         entered = telemetryEntered, droneId, i] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
                         const nsf::RequestMessage&) {
          ++*calls;
          if (i == 3 && hold->load()) {
            entered->store(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (hold->load() && std::chrono::steady_clock::now() < deadline) {
              std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
          }
          nsf::ResponseMessage response;
          response.setStatus(true);
          auto payload = asFields({{"drone_id", wrong->load() ? "wrong" : droneId},
            {"timestamp_ms", std::to_string(nowMilliseconds())}, {"flight_controller_backend", "mock"}});
          response.setPayload(payload, payload.size());
          return response;
        }, nsf::ServiceProvider::ServiceInvocationMode::NormalAndTargeted);
    }
    JobFlowFixture::install(rejectCompute, holdCompute, wrongFeedback, omitSourceDrain,
                            std::move(wrongDrainField), firstSyntheticProvider, lastSyntheticProvider,
                            holdSourceDrain);
  }

  TrackingAdmission
  startMain()
  {
    auto result = std::make_shared<std::optional<TrackingAdmission>>();
    const bool queued = runtime->startTracking(definition,
      [result] (TrackingAdmission admission, std::string) { *result = admission; });
    BOOST_REQUIRE(queued);
    environment->pumpUntilWithAttributeAuthority([result] { return result->has_value(); });
    BOOST_REQUIRE(result->has_value());
    return **result;
  }

  void
  shutdown()
  {
    if (shutDown) return;
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto ok = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([this, done, ok] {
      *ok = runtime->shutdownRuntime();
      *done = true;
    });
    try {
      while (!done->load()) {
        environment->pumpUntilWithAttributeAuthority([done] { return done->load(); });
      }
    }
    catch (...) {
      releaseCompute->store(true);
      releaseSource->store(true);
      worker.join();
      throw;
    }
    worker.join();
    BOOST_CHECK(ok->load());
    if (!ok->load()) std::terminate();
    shutDown = true;
  }

  std::unique_ptr<GroundStationServiceContainer> runtime;
  bool shutDown = false;
  std::shared_ptr<std::atomic<size_t>> telemetryCalls = std::make_shared<std::atomic<size_t>>(0);
  std::shared_ptr<std::atomic<bool>> wrongTelemetryPayload =
    std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> holdTelemetry = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> telemetryEntered = std::make_shared<std::atomic<bool>>(false);
};

BOOST_AUTO_TEST_SUITE(UavTrackingMainGroundStation)

BOOST_FIXTURE_TEST_CASE(MonitorWrongDroneExpiredAndRevokedCannotPublishRequests,
                        MainAppTrackingFixture)
{
  install();
  auto lease = runtime->operatorAuthorityLease();
  const auto permitted = lease;
  lease.scope = "monitor";
  runtime->setOperatorAuthorityLease(lease);
  BOOST_CHECK(startMain() == TrackingAdmission::UNAUTHORIZED);
  lease = permitted;
  lease.droneId = definition.sources[0].droneId;
  runtime->setOperatorAuthorityLease(lease);
  BOOST_CHECK(startMain() == TrackingAdmission::UNAUTHORIZED);
  lease = permitted;
  lease.expiresMs = nowMilliseconds() - 1;
  runtime->setOperatorAuthorityLease(lease);
  BOOST_CHECK(startMain() == TrackingAdmission::UNAUTHORIZED);
  lease = permitted;
  lease.revoked = true;
  runtime->setOperatorAuthorityLease(lease);
  BOOST_CHECK(startMain() == TrackingAdmission::UNAUTHORIZED);
  BOOST_CHECK_EQUAL(executions.load(), 0);
  BOOST_CHECK(runtime->trackingSnapshot().state == TrackingJobState::IDLE);
  runtime->setOperatorAuthorityLease(permitted);
  BOOST_CHECK(startMain() == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] {
    return runtime->trackingSnapshot().cleanupState == "CLEAN";
  });
  BOOST_CHECK(runtime->trackingSnapshot().state == TrackingJobState::COMPLETED);
}

BOOST_FIXTURE_TEST_CASE(PublicQueueIsBoundedAndIncidentViewRemainsIndependent,
                        MainAppTrackingFixture)
{
  install();
  const auto before = runtime->runtimeSnapshot().collaboration;
  auto calls = std::make_shared<size_t>(0);
  BOOST_REQUIRE(runtime->startTracking(definition,
    [calls] (TrackingAdmission admission, std::string) {
      BOOST_CHECK(admission == TrackingAdmission::ACCEPTED);
      ++*calls;
    }));
  BOOST_CHECK(!runtime->startTracking(definition, {}));
  BOOST_CHECK(!runtime->cancelTracking(definition.jobId, {}));
  environment->pumpUntilWithAttributeAuthority([this] {
    return runtime->trackingSnapshot().cleanupState == "CLEAN";
  });
  BOOST_CHECK_EQUAL(*calls, 1);
  auto snapshot = runtime->runtimeSnapshot();
  BOOST_CHECK(snapshot.tracking.state == TrackingJobState::COMPLETED);
  BOOST_CHECK_EQUAL(snapshot.tracking.completedWindows, 1);
  BOOST_CHECK_EQUAL(snapshot.collaboration.stage, before.stage);
  BOOST_CHECK_EQUAL(snapshot.collaboration.requestId, before.requestId);
  BOOST_CHECK_EQUAL(snapshot.collaboration.missionId, before.missionId);
  BOOST_CHECK(startMain() == TrackingAdmission::INVALID);
  config["runId"] = "unused-run";
  config["jobId"] = "unused-job";
  definition = load();
  BOOST_CHECK(startMain() == TrackingAdmission::INVALID); // Same Producer/epoch cannot be republished.
}

BOOST_FIXTURE_TEST_CASE(RevocationDuringWorkStopsNewWindowsAndRejectsPublicCancel,
                        MainAppTrackingFixture)
{
  configureWindows(2);
  install(false, true);
  BOOST_REQUIRE(startMain() == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return computeEntered->load(); });
  BOOST_REQUIRE(computeEntered->load());
  BOOST_CHECK(startMain() == TrackingAdmission::BUSY);
  auto lease = runtime->operatorAuthorityLease();
  lease.revoked = true;
  runtime->setOperatorAuthorityLease(lease);
  auto cancellation = std::make_shared<std::optional<TrackingCancelResult>>();
  BOOST_REQUIRE(runtime->cancelTracking(definition.jobId,
    [cancellation] (TrackingCancelResult result, std::string) { *cancellation = result; }));
  environment->pumpUntilWithAttributeAuthority([cancellation] { return cancellation->has_value(); });
  BOOST_CHECK(**cancellation == TrackingCancelResult::UNAUTHORIZED);
  environment->pumpUntilWithAttributeAuthority([this] {
    return runtime->trackingSnapshot().cancelState == "REMOTE_CONFIRMED";
  });
  releaseCompute->store(true);
  environment->pumpUntilWithAttributeAuthority([this] {
    return runtime->trackingSnapshot().cleanupState == "CLEAN";
  });
  const auto snapshot = runtime->trackingSnapshot();
  BOOST_CHECK_EQUAL(snapshot.completedWindows, 0);
  BOOST_CHECK(snapshot.latestResult == nullptr);
  BOOST_CHECK_EQUAL(executions.load(), 4);
}

BOOST_FIXTURE_TEST_CASE(ReceivedResultCannotAdvanceAfterLiveLeaseRevocation,
                        MainAppTrackingFixture)
{
  configureWindows(2);
  install(false, false, false, false, {}, 0, 4, true);
  BOOST_REQUIRE(startMain() == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] {
    return runtime->trackingSnapshot().waitingForWorkDrain;
  });
  BOOST_REQUIRE(runtime->trackingSnapshot().waitingForWorkDrain);
  BOOST_REQUIRE(computeResponseReady->load());
  auto lease = runtime->operatorAuthorityLease();
  lease.revoked = true;
  runtime->setOperatorAuthorityLease(lease);
  releaseSource->store(true);
  environment->pumpUntilWithAttributeAuthority([this] {
    return runtime->trackingSnapshot().cleanupState == "CLEAN";
  });
  BOOST_CHECK_EQUAL(runtime->trackingSnapshot().completedWindows, 0);
  BOOST_CHECK(runtime->trackingSnapshot().latestResult == nullptr);
  BOOST_CHECK_EQUAL(executions.load(), 4);
}

BOOST_FIXTURE_TEST_CASE(FailedRuntimeWithInstalledOwnerDrainsBeforeFaceShutdown,
                        MainAppTrackingFixture)
{
  auto entered = std::make_shared<bool>(false);
  boost::asio::post(environment->userFace().getIoContext(), [this, entered] {
    GroundStationTrackingTestAccess::failRuntime(*runtime);
    *entered = true;
  });
  environment->pumpUntilWithAttributeAuthority([entered] { return *entered; });
  BOOST_CHECK(!runtime->waitUntilReady(std::chrono::seconds(1)));
  BOOST_CHECK(!runtime->startTracking(definition, {}));
  BOOST_CHECK(!GroundStationTrackingTestAccess::ownedFaceStopped(*runtime));
  shutdown();
  BOOST_CHECK(GroundStationTrackingTestAccess::ownedFaceStopped(*runtime));
}

BOOST_FIXTURE_TEST_CASE(ShutdownKeepsFaceAliveThroughAuthorityAckAndNativeWorkDrain,
                        MainAppTrackingFixture)
{
  install(false, true);
  BOOST_REQUIRE(startMain() == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return computeEntered->load(); });
  BOOST_REQUIRE(computeEntered->load());
  auto done = std::make_shared<std::atomic<bool>>(false);
  auto ok = std::make_shared<std::atomic<bool>>(false);
  std::thread worker([this, done, ok] {
    *ok = runtime->shutdownRuntime();
    *done = true;
  });
  try {
    environment->pumpUntilWithAttributeAuthority([this] {
      return runtime->trackingSnapshot().cancelState == "REMOTE_CONFIRMED";
    });
    BOOST_CHECK(!done->load());
    BOOST_CHECK(!GroundStationTrackingTestAccess::ownedFaceStopped(*runtime));
    BOOST_CHECK(!runtime->startTracking(definition, {}));
    releaseCompute->store(true);
    while (!done->load()) {
      environment->pumpUntilWithAttributeAuthority([done] { return done->load(); });
    }
  }
  catch (...) {
    releaseCompute->store(true);
    worker.join();
    throw;
  }
  worker.join();
  BOOST_CHECK(ok->load());
  if (!ok->load()) std::terminate();
  shutDown = true;
  BOOST_CHECK_EQUAL(runtime->trackingSnapshot().cleanupState, "CLEAN");
  auto unrelated = std::make_shared<bool>(false);
  boost::asio::post(environment->userFace().getIoContext(), [unrelated] { *unrelated = true; });
  environment->pumpUntilWithAttributeAuthority([unrelated] { return *unrelated; });
  BOOST_CHECK(*unrelated);
}

BOOST_FIXTURE_TEST_CASE(ExplicitProducerTelemetryIsBoundedAndCancelRemainsAvailable,
                        MainAppTrackingFixture)
{
  install(false, true);
  BOOST_REQUIRE(startMain() == TrackingAdmission::ACCEPTED);
  pumpUntilTracking([&] { return computeEntered->load(); });
  auto replies = std::make_shared<std::vector<bool>>();
  auto completion = [replies] (bool verified, std::string, uint64_t start, uint64_t end) {
    BOOST_CHECK_GE(end, start);
    BOOST_CHECK_LE(end - start, 2000);
    replies->push_back(verified);
  };
  for (const auto& source : definition.sources) {
    BOOST_REQUIRE(runtime->requestTrackingTelemetry(definition.jobId,
      source.producerIdentity, source.droneId, completion));
  }
  BOOST_REQUIRE(runtime->requestTrackingTelemetry(definition.jobId,
    definition.computeIdentity, definition.computeDroneId, completion));
  BOOST_CHECK(!runtime->requestTrackingTelemetry(definition.jobId,
    definition.computeIdentity, definition.computeDroneId, completion));
  auto cancellation = std::make_shared<std::optional<TrackingCancelResult>>();
  BOOST_REQUIRE(runtime->cancelTracking(definition.jobId,
    [cancellation] (TrackingCancelResult result, std::string) { *cancellation = result; }));
  pumpUntilTracking([&] {
    return replies->size() == 4 && cancellation->has_value();
  });
  BOOST_CHECK(std::all_of(replies->begin(), replies->end(), [] (bool value) { return value; }));
  BOOST_CHECK(**cancellation == TrackingCancelResult::REQUESTED);
  BOOST_CHECK_EQUAL(telemetryCalls->load(), 4);
  BOOST_REQUIRE(runtime->requestTrackingTelemetry(definition.jobId,
    ndn::Name("/wrong/provider"), definition.computeDroneId, completion));
  pumpUntilTracking([&] { return replies->size() == 5; });
  BOOST_CHECK(!replies->back());
  BOOST_CHECK_EQUAL(telemetryCalls->load(), 4);
  releaseCompute->store(true);
  pumpUntilTracking([&] {
    return runtime->trackingSnapshot().cleanupState == "CLEAN";
  });
  shutdown();
}

BOOST_FIXTURE_TEST_CASE(WrongOrdinaryPayloadCannotQualifyACompletedMainJob, MainAppTrackingFixture)
{
  install();
  wrongTelemetryPayload->store(true);
  GroundStationTrackingControls controls(*runtime, {definition}, true);
  pumpUntilTracking([&] {
    controls.tick();
    return controls.finished();
  });
  BOOST_CHECK(controls.failed());
  BOOST_CHECK(controls.view().state == TrackingJobState::COMPLETED);
  shutdown();
  BOOST_CHECK(!controls.finalize(true));
  std::ifstream input(definition.outputDirectory / "tracking-telemetry.json");
  const auto report = Json::parse(input);
  BOOST_CHECK(report.at("failed").get<bool>());
  BOOST_CHECK_EQUAL(report.at("reports").size(), 8);
}

BOOST_FIXTURE_TEST_CASE(TelemetryTimeoutAndThrowingCompletionDrainThroughTheFaceBarrier,
                        MainAppTrackingFixture)
{
  install();
  holdTelemetry->store(true);
  BOOST_REQUIRE(startMain() == TrackingAdmission::ACCEPTED);
  auto calls = std::make_shared<std::atomic<size_t>>(0);
  BOOST_REQUIRE(runtime->requestTrackingTelemetry(definition.jobId,
    definition.computeIdentity, definition.computeDroneId,
    [calls] (bool verified, std::string, uint64_t, uint64_t) {
      BOOST_CHECK(!verified);
      ++*calls;
      throw std::runtime_error("controlled completion failure");
    }));
  pumpUntilTracking([&] { return telemetryEntered->load(); });
  BOOST_REQUIRE(telemetryEntered->load());
  auto done = std::make_shared<std::atomic<bool>>(false);
  auto drained = std::make_shared<std::atomic<bool>>(false);
  std::thread worker([this, done, drained] { *drained = runtime->shutdownRuntime(); *done = true; });
  try {
    const auto early = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    pumpUntilTracking([&] { return std::chrono::steady_clock::now() >= early; });
    BOOST_CHECK(!done->load());
    pumpUntilTracking([&] { return done->load(); });
    holdTelemetry->store(false);
    worker.join();
  }
  catch (...) {
    holdTelemetry->store(false);
    worker.join();
    throw;
  }
  BOOST_CHECK(drained->load());
  if (!drained->load()) std::terminate();
  BOOST_CHECK_EQUAL(calls->load(), 1);
  shutDown = true;
  bool live = false;
  boost::asio::post(environment->userFace().getIoContext(), [&] { live = true; });
  pumpUntilTracking([&] { return live; });
  BOOST_CHECK(live);
}

BOOST_FIXTURE_TEST_CASE(ThreeWindowControlsRecordActualStartDuringAndEndTelemetry,
                        MainAppTrackingFixture)
{
  configureWindows(3);
  install();
  GroundStationTrackingControls controls(*runtime, {definition}, true);
  pumpUntilTracking([&] { controls.tick(); return controls.finished(); });
  BOOST_CHECK(!controls.failed());
  BOOST_CHECK_EQUAL(controls.view().completedWindows, 3);
  std::ifstream input(definition.outputDirectory / "tracking-telemetry.json");
  const auto report = Json::parse(input);
  BOOST_REQUIRE_EQUAL(report.at("reports").size(), 12);
  std::array<size_t, 3> phases{};
  for (const auto& reply : report.at("reports")) {
    const auto phase = reply.at("phase").get<size_t>();
    BOOST_REQUIRE_LT(phase, phases.size());
    ++phases[phase];
    BOOST_CHECK(reply.at("verified").get<bool>());
    if (phase == 1) {
      BOOST_CHECK_EQUAL(reply.at("nativeStateAtRequest").get<std::string>(), "RUNNING");
      BOOST_CHECK_GT(reply.at("verifiedWindowsAtRequest").get<uint64_t>(), 0);
      BOOST_CHECK_LT(reply.at("verifiedWindowsAtRequest").get<uint64_t>(), 3);
    }
  }
  for (const auto count : phases) BOOST_CHECK_EQUAL(count, 4);
}

BOOST_FIXTURE_TEST_CASE(MissedDuringPhaseIsFailureAndCannotBeRelabelledAfterTerminal,
                        MainAppTrackingFixture)
{
  configureWindows(3);
  install();
  GroundStationTrackingControls controls(*runtime, {definition}, true);
  controls.tick();
  pumpUntilTracking([&] {
    return runtime->trackingSnapshot().state == TrackingJobState::COMPLETED;
  });
  pumpUntilTracking([&] { controls.tick(); return controls.finished(); });
  BOOST_CHECK(controls.failed());
  BOOST_CHECK(controls.view().state == TrackingJobState::COMPLETED);
  std::ifstream input(definition.outputDirectory / "tracking-telemetry.json");
  const auto report = Json::parse(input);
  BOOST_CHECK(report.at("failed").get<bool>());
  BOOST_REQUIRE_EQUAL(report.at("reports").size(), 4);
  for (const auto& reply : report.at("reports")) {
    BOOST_CHECK_EQUAL(reply.at("phase").get<size_t>(), 2);
    BOOST_CHECK_EQUAL(reply.at("nativeStateAtRequest").get<std::string>(), "COMPLETED");
  }
}

BOOST_FIXTURE_TEST_CASE(CancelAndFailedTelemetryAllowAFreshManualRunAfterActualDrain,
                        MainAppTrackingFixture)
{
  install(false, true);
  auto second = definition;
  second.jobId = "job-2";
  second.runId = "run-2";
  second.outputDirectory = root / "output-run-2";
  std::ifstream original(root / "asset");
  auto schedule = Json::parse(original);
  schedule["generations"][0]["sourceEpoch"] = 2;
  for (auto& source : second.sources) {
    source.sourceEpoch = 2;
    source.sourceId += "-second";
    schedule["generations"][0]["sourceIds"][source.cameraId] = source.sourceId;
  }
  const auto bytes = schedule.dump();
  const auto path = root / "second-source-generations.json";
  std::ofstream(path) << bytes;
  for (auto& source : second.sources) source.sourceManifest = {path, digestOf(bytes)};
  GroundStationTrackingControls controls(*runtime, {definition, second}, false);
  wrongTelemetryPayload->store(true);
  BOOST_REQUIRE(controls.start(0));
  pumpUntilTracking([&] {
    controls.tick();
    return computeEntered->load() && telemetryCalls->load() == 4;
  });
  BOOST_REQUIRE(controls.cancel());
  releaseCompute->store(true);
  pumpUntilTracking([&] { controls.tick(); return controls.canStart(1); });
  BOOST_CHECK(controls.view().state == TrackingJobState::CANCELLED);
  BOOST_CHECK_EQUAL(controls.view().cleanupState, "CLEAN");
  wrongTelemetryPayload->store(false);
  definition = second;
  BOOST_REQUIRE(controls.start(1));
  pumpUntilTracking([&] {
    controls.tick();
    const auto status = second.outputDirectory / "tracking-status.json";
    if (controls.view().state != TrackingJobState::COMPLETED || !std::filesystem::exists(status)) return false;
    std::ifstream input(status);
    return Json::parse(input).at("ordinaryTelemetryComplete").get<bool>();
  });
  BOOST_CHECK(!controls.failed());
  BOOST_CHECK_EQUAL(controls.view().generation, 2);
  BOOST_CHECK_EQUAL(controls.view().runId, second.runId);
  shutdown();
  BOOST_CHECK(controls.finalize(true));
}

BOOST_FIXTURE_TEST_CASE(ControlsUseActualFacadeAndPersistNativeTerminalStatus, MainAppTrackingFixture)
{
  install();
  GroundStationTrackingControls controls(*runtime, {definition}, true);
  controls.tick();
  environment->pumpUntilWithAttributeAuthority([&] {
    controls.tick();
    return controls.finished();
  });
  BOOST_REQUIRE(controls.finished());
  BOOST_CHECK(!controls.failed());
  BOOST_CHECK(controls.view().state == TrackingJobState::COMPLETED);
  BOOST_CHECK(controls.view().presentation);
  BOOST_CHECK(!controls.canStart(0));
  shutdown();
  BOOST_REQUIRE(controls.finalize(true));
  std::ifstream input(definition.outputDirectory / "tracking-status.json");
  const auto status = Json::parse(input);
  BOOST_CHECK_EQUAL(status.at("state").get<std::string>(), "COMPLETED");
  BOOST_CHECK_EQUAL(status.at("hostShutdown").get<std::string>(), "CLEAN");
  BOOST_CHECK(status.at("ordinaryTelemetryComplete").get<bool>());
  BOOST_CHECK(!status.at("ordinaryTelemetryFailed").get<bool>());
  BOOST_CHECK_EQUAL(telemetryCalls->load(), 8);
}

BOOST_FIXTURE_TEST_CASE(ControlsCloseWeakCompletionSlotBeforeLateFaceAdmission, MainAppTrackingFixture)
{
  {
    GroundStationTrackingControls controls(*runtime, {definition}, false);
    BOOST_REQUIRE(controls.start(0));
    BOOST_CHECK(!controls.start(0));
  }
  environment->pumpUntilWithAttributeAuthority([this] {
    return runtime->trackingSnapshot().state != TrackingJobState::IDLE;
  });
  // A released UI slot cannot retain the owner or receive a late callback.
  BOOST_CHECK(runtime->trackingSnapshot().state != TrackingJobState::IDLE);
}

BOOST_FIXTURE_TEST_CASE(ActualGtkPanelDrawsThreeVerifiedFramesAndReleasesItsSources,
                        MainAppTrackingFixture)
{
  int argc = 1;
  char name[] = "spec228-gtk-fixture";
  char* arguments[] = {name, nullptr};
  char** argv = arguments;
  {
    Gtk::Main gtk(argc, argv);
    install();
    GroundStationTrackingControls controls(*runtime, {definition}, true);
    {
      GroundStationTrackingPanel panel(controls, [] { return false; });
      Gtk::Window window;
      window.set_default_size(1200, 700);
      window.add(panel);
      window.show_all();
      environment->pumpUntilWithAttributeAuthority([&] {
        pumpTrackingGtk();
        return controls.finished() && GroundStationTrackingPanelTestAccess::allDrawn(panel);
      });
      BOOST_CHECK(controls.finished());
      BOOST_CHECK(GroundStationTrackingPanelTestAccess::allDrawn(panel));
      BOOST_CHECK(controls.view().state == TrackingJobState::COMPLETED);
      window.hide();
      window.remove();
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < until) {
      pumpTrackingGtk();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_CHECK(controls.view().state == TrackingJobState::COMPLETED);
  }
  BOOST_REQUIRE(finishUavGtkRuntime());
}

BOOST_FIXTURE_TEST_CASE(ActualGtkCancelButtonUsesFacadeAndWaitsForNativeDrain,
                        MainAppTrackingFixture)
{
  int argc = 1;
  char name[] = "spec228-gtk-cancel-fixture";
  char* arguments[] = {name, nullptr};
  char** argv = arguments;
  {
    Gtk::Main gtk(argc, argv);
    install(false, true);
    GroundStationTrackingControls controls(*runtime, {definition}, true);
    {
      GroundStationTrackingPanel panel(controls, [] { return false; });
      Gtk::Window window;
      window.add(panel);
      window.show_all();
      environment->pumpUntilWithAttributeAuthority([&] {
        pumpTrackingGtk();
        return computeEntered->load() && controls.canCancel();
      });
      BOOST_REQUIRE(computeEntered->load());
      BOOST_REQUIRE(controls.canCancel());
      GroundStationTrackingPanelTestAccess::clickCancel(panel);
      environment->pumpUntilWithAttributeAuthority([&] {
        pumpTrackingGtk();
        return runtime->trackingSnapshot().cancelState == "REMOTE_CONFIRMED";
      });
      BOOST_CHECK_EQUAL(runtime->trackingSnapshot().cleanupState, "DRAINING");
      releaseCompute->store(true);
      environment->pumpUntilWithAttributeAuthority([&] {
        pumpTrackingGtk();
        return controls.finished();
      });
      BOOST_CHECK(controls.view().state == TrackingJobState::CANCELLED);
      BOOST_CHECK_EQUAL(controls.view().cleanupState, "CLEAN");
      window.hide();
      window.remove();
    }
    pumpTrackingGtk();
  }
  BOOST_REQUIRE(finishUavGtkRuntime());
}

BOOST_FIXTURE_TEST_CASE(ActualGtkWholeWindowDrawsAndDisconnectsTelemetryBeforeDestruction,
                        MainAppTrackingFixture)
{
  int argc = 1;
  char name[] = "spec228-whole-window-fixture";
  char* arguments[] = {name, nullptr};
  char** argv = arguments;
  {
    Gtk::Main gtk(argc, argv);
    install();
    struct RestoreWorkingDirectory
    {
      std::filesystem::path original = std::filesystem::current_path();
      ~RestoreWorkingDirectory() { std::filesystem::current_path(original); }
    } restoreWorkingDirectory;
    // Real production map loading uses fixture-owned local tiles, never the network.
    auto tile = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, false, 8, 256, 256);
    tile->fill(0xe0e0e0ff);
    for (int x = 16383; x <= 16384; ++x) {
      const auto directory = root / "NDNSF-UAV-APP/maps/osm/15" / std::to_string(x);
      std::filesystem::create_directories(directory);
      for (int y = 16383; y <= 16384; ++y) {
        tile->save((directory / (std::to_string(y) + ".png")).string(), "png");
      }
    }
    tile.reset();
    std::filesystem::current_path(root);
    GroundStationTrackingControls controls(*runtime, {definition}, true);
    sigc::connection telemetryPoll;
    {
      GroundStationWindow window(*runtime, false, 10, 0,
        false, false, false, false, false, false, false, false,
        false, false, false, false, false, false, false,
        {"UAV1", "UAV2", "UAV3"}, 0.0, 0.0, 1000, &controls);
      telemetryPoll = GroundStationWindowTestAccess::telemetryPoll(window);
      BOOST_REQUIRE(telemetryPoll.connected());
      window.show_all();
      environment->pumpUntilWithAttributeAuthority([&] {
        pumpTrackingGtk();
        return controls.finished() && GroundStationWindowTestAccess::trackingDrawn(window);
      });
      BOOST_CHECK(controls.finished());
      BOOST_CHECK(GroundStationWindowTestAccess::trackingDrawn(window));
      BOOST_CHECK(controls.view().state == TrackingJobState::COMPLETED);
      const auto pollDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (GroundStationWindowTestAccess::telemetryPollCount(window) == 0 &&
             std::chrono::steady_clock::now() < pollDeadline) {
        pumpTrackingGtk();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      BOOST_CHECK_GT(GroundStationWindowTestAccess::telemetryPollCount(window), 0);
      shutdown();
      window.hide();
    }
    BOOST_CHECK(!telemetryPoll.connected());
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1600);
    while (std::chrono::steady_clock::now() < until) {
      pumpTrackingGtk();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  BOOST_REQUIRE(finishUavGtkRuntime());
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndnsf::test
