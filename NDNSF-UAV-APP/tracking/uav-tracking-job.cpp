#include "uav-tracking-job.hpp"

#include "UavTrackingCoordinator.hpp"

#include "../shared/UavNames.hpp"
#include "../shared/UavProtocol.hpp"
#include "../shared/UavSourceGeneration.hpp"
#include "../shared/UavTrackingResultVerifier.hpp"
#include "../shared/uav-person-computation.hpp"
#include "../shared/uav-person-json.hpp"
#include "../shared/uav-person-published-frame.hpp"
#include "../shared/uav-person-trace.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <boost/asio/post.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <ndn-cxx/util/scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <set>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <string_view>
#include <utility>
#include <vector>

namespace nsf = ndn_service_framework;
namespace uav = ndnsf::examples::uav;

namespace {

std::atomic<bool> g_cancel_requested{false};
std::atomic<bool> g_replay_requested{false};

void
onReplaySignal(int)
{
  g_replay_requested.store(true);
}

void
onSignal(int)
{
  g_cancel_requested.store(true);
}

void
onCancelSignal(int)
{
  g_cancel_requested.store(true);
}

std::string
option(int argc, char** argv, const std::string& name, const std::string& fallback = {})
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == name) {
      return argv[i + 1];
    }
  }
  return fallback;
}

void
requireOptionValue(int argc, char** argv, const std::string& name)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] != name) continue;
    if (i + 1 >= argc || argv[i + 1][0] == '\0' ||
        (argv[i + 1][0] == '-' && argv[i + 1][1] == '-')) {
      throw std::invalid_argument(name + " requires a value");
    }
    return;
  }
}

ndn::security::Certificate
getOrCreateIdentity(ndn::KeyChain& keyChain, const ndn::Name& identity)
{
  try {
    return keyChain.getPib().getIdentity(identity).getDefaultKey().getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
  }
}

ndn::Buffer
fieldsBuffer(const uav::Fields& fields)
{
  const auto encoded = uav::encodeFields(fields);
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size());
}

uav::Fields
parseFields(const ndn::Buffer& payload)
{
  return uav::decodeFields(std::string(reinterpret_cast<const char*>(payload.data()),
                                        payload.size()));
}

std::string
sha256Text(std::string_view value)
{
  ndn::util::Sha256 digest;
  digest << value;
  return uav::canonicalSha256Digest(digest.toString());
}

std::string
readMotionWindow(const std::string& directory, const std::string& window)
{
  if (directory.empty()) return {};
  const auto path = std::filesystem::path(directory) / (window + ".json");
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("motion window is not available: " + path.string());
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size <= 0 || size > 256 * 1024) {
    throw std::runtime_error("motion window is empty or exceeds 256 KiB");
  }
  input.seekg(0, std::ios::beg);
  std::string payload(static_cast<size_t>(size), '\0');
  input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (input.gcount() != static_cast<std::streamsize>(payload.size())) {
    throw std::runtime_error("short read from motion window: " + path.string());
  }
  if (!payload.empty() && payload.back() == '\n') payload.pop_back();
  return payload;
}

std::string
readPersonConfiguration(const std::string& path, size_t maxBytes)
{
  if (!std::filesystem::is_regular_file(path)) {
    throw std::invalid_argument("person configuration is not a regular file: " + path);
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read person configuration: " + path);
  std::string text(maxBytes + 1, '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(input.gcount()));
  if (input.bad() || text.size() > maxBytes) {
    throw std::invalid_argument("person configuration exceeds byte bound or cannot be read");
  }
  return text;
}

void
writePersonArtifact(const std::filesystem::path& path, std::string_view bytes, bool replace = false)
{
  if (bytes.size() > 16 * 1024 * 1024 || std::filesystem::is_symlink(path)) {
    throw std::invalid_argument("invalid person presentation artifact bound/path");
  }
  if (!replace && std::filesystem::exists(path)) {
    if (std::string_view(readPersonConfiguration(path.string(), 16 * 1024 * 1024)) != bytes) {
      throw std::runtime_error("refusing to replace a different person presentation artifact");
    }
    return;
  }
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  if (std::filesystem::is_symlink(temporary)) {
    throw std::invalid_argument("person presentation temporary path is a symlink");
  }
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("cannot write person presentation artifact");
  }
  std::filesystem::rename(temporary, path);
}

struct PersonFrameReadBatch
{
  std::array<uav::UavPersonFrameReadBinding, 3> bindings;
  std::array<ndn::Buffer, 3> images;
  std::array<std::pair<uint64_t, uint64_t>, 3> dimensions;
  std::array<bool, 3> received{};
  std::string resultJson;
  std::string resultDigest;
  std::string resultName;
  std::string responseProvider;
  size_t remaining = 3;
  bool terminal = false;
};

} // namespace

namespace ndnsf::examples::uav {

struct UavTrackingJob::Impl : std::enable_shared_from_this<UavTrackingJob::Impl>
{
  std::string runId{};
  std::string missionId{};
  std::string windowPrefix{};
  size_t windowCount{};
  std::string fault{};
  std::string motionDir{};
  std::string observationSchema{};
  std::string observationSourceEpoch{};
  std::string observationSourceUav1{};
  std::string observationSourceUav2{};
  std::string observationSourceUav3{};
  std::string sourceGenerationsPath{};
  std::string trackingProfilePath{};
  std::string computationManifestPath{};
  std::string personPresentationDir{};
  bool wrongPersonProfile{};
  bool wrongPersonComputation{};
  std::string calibrationProviderText{};
  ndn::Name calibrationProvider{};
  uint64_t observationEpoch{};
  ndn::Name groupPrefix{};
  ndn::Name controllerPrefix{};
  std::string trustSchema{};
  ndn::Name userIdentity{};
  int startDelayMs{};
  int controllerReadyTimeoutMs{};
  int controllerReadyRetryMs{};
  int requestTimeoutMs{};
  std::array<std::string, 3> initialSourceIds{};
  std::filesystem::path presentationRoot{};

  using PersonJson = uav::person_json::Json;
  nsf::ServiceUser& user;
  ndn::Face& face;
  ndn::Scheduler scheduler;
  AuthorityCheck authority;
  SnapshotHandler onSnapshot;
  TrackingJobDefinition definition;
  TrackingSnapshot view;
  std::function<void(const TrackingCleanupResult&)> onDrained;
  std::map<std::string, std::string> personRequestFields;
  std::optional<uav::UavPersonTrackingResultExpectation> personExpectation;
  std::optional<uav::UavSourceGenerationSchedule> sourceGenerations;
  std::vector<ndn::Name> sourceProviders;
  ndn::Name computeProvider;
  int exitCode = 0;
  std::string lastGoodPresentation;
  std::function<void(size_t, const std::string&)> publishPresentationFailure;
  std::function<void(const ndn::Name&, size_t)> cancelActiveCollaboration;
  std::optional<ndn::Name> activeRequestId;
  std::optional<ndn::Name> scopeRequestId;
  std::optional<ndn::Name> activeComputeProvider;
  size_t activeWindowIndex = 0;
  bool cancellationInProgress = false;
  uint64_t resultDrainGeneration = 0;
  bool cancellationCompletionPending = false;
  std::optional<std::pair<bool, ndn::Buffer>> deferredCancellationResponse;
  std::shared_ptr<std::function<void(bool, const ndn::Buffer&)>> cancellationResponseHandler;
  std::thread cancellationAckThread;
  std::shared_ptr<std::atomic<int>> currentAckState;
  std::shared_ptr<std::atomic<bool>> revocationSeen;
  std::shared_ptr<std::atomic<bool>> workDrainedSeen;
  std::chrono::steady_clock::time_point bootstrapStarted;
  std::optional<int64_t> presentationMediaOriginUs;
  bool committedReplaySent = false;
  bool alteredReplaySent = false;
  bool replayWaiting = false;
  std::chrono::steady_clock::time_point replayDeadline;
  std::optional<std::string> committedReplayResult;
  std::shared_ptr<std::function<void(size_t)>> beginWindow;
  std::atomic<bool> halted{false};
  size_t pendingReads = 0;
  bool finished = false;
  bool drained = false;
  bool remoteWorkMayExist = false;
  bool remoteCancelConfirmed = false;
  bool remoteWorkDrained = false;
  bool authorityLost = false;
  bool stopRequested = false;
  bool compatibility = false;
  bool drainReported = false;
  bool drainCompletionQueued = false;
  std::chrono::steady_clock::time_point drainDeadline;

  Impl(nsf::ServiceUser& borrowedUser, ndn::Face& borrowedFace,
       AuthorityCheck liveAuthority, SnapshotHandler handler)
    : user(borrowedUser), face(borrowedFace), scheduler(face.getIoContext())
    , authority(std::move(liveAuthority)), onSnapshot(std::move(handler))
  {
  }

  void
  notify()
  {
    view.updatedMonotonicMs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    view.activeRequestId = activeRequestId ? activeRequestId->toUri() : "";
    if (onSnapshot) {
      try {
        onSnapshot(view);
      }
      catch (const std::exception& error) {
        std::cerr << "TRACKING_SNAPSHOT_DELIVERY_FAILED reason=" << error.what() << std::endl;
      }
    }
  }

  bool
  checkLiveAuthority()
  {
    if (compatibility || (!authorityLost && authority && authority(definition, &view.reason))) {
      return true;
    }
    authorityLost = true;
    view.failureStage = "authority";
    if (activeRequestId) cancelActiveCollaboration(*activeRequestId, activeWindowIndex);
    else { exitCode = 1; finish(); }
    return false;
  }

  void
  pollAuthority()
  {
    scheduler.schedule(ndn::time::milliseconds(100), [self = shared_from_this()] {
      if (self->finished) return;
      if (!self->cancellationInProgress) self->checkLiveAuthority();
      if (!self->finished && std::chrono::steady_clock::now() - self->bootstrapStarted >=
          std::chrono::milliseconds(self->definition.deadlineMs) && !self->compatibility) {
        self->view.reason = "tracking job deadline";
        self->view.failureStage = "deadline";
        if (self->activeRequestId) {
          self->cancelActiveCollaboration(*self->activeRequestId, self->activeWindowIndex);
        }
        else { self->exitCode = 1; self->finish(); }
      }
      if (!self->finished) self->pollAuthority();
    });
  }

  void
  settleRead(const std::shared_ptr<bool>& settled)
  {
    if (*settled) return;
    *settled = true;
    --pendingReads;
    if (finished && drainReported) pollDrain();
  }

  void
  releaseWindowScope()
  {
    if (!scopeRequestId) return;
    for (const auto* scope : {TRACKING_INPUT_KEY_SCOPE, "uav-tracking-observation",
                              "uav-tracking-result"}) {
      user.clearVerifiedCollaborationData(*scopeRequestId, scope);
    }
    scopeRequestId.reset();
  }

  size_t
  expectedDrainCount() const
  {
    return sourceProviders.size() + 1 + (calibrationProvider.empty() ? 0 : 1);
  }

  void
  collectRoleDrains(const ndn::Name& requestId, size_t windowIndex,
                    std::set<ndn::Name>& drainedProviders)
  {
    std::map<std::string, std::map<ndn::Name, std::string>> expected;
    auto& inputs = expected[TRACKING_INPUT_KEY_SCOPE];
    for (size_t i = 0; i < sourceProviders.size(); ++i) {
      inputs.emplace(sourceProviders[i], "Camera-UAV" + std::to_string(i + 1));
    }
    inputs.emplace(computeProvider, "Tracking-Compute");
    if (!calibrationProvider.empty()) {
      expected["uav-tracking-observation"].emplace(calibrationProvider, "Calibration-Provider");
    }
    const auto epoch = uavSourceGenerationForWindow(*sourceGenerations, windowIndex).sourceEpoch;
    for (const auto& [scope, peers] : expected) {
      // A bounded cache query; yield to Face between polls so incoming packets progress.
      const auto records = user.waitForVerifiedCollaborationData(
        requestId, scope, TRACKING_WORK_DRAINED_TOPIC, 1, 1, false);
      for (const auto& record : records) {
        const auto peer = peers.find(record.producer);
        if (record.requestId != requestId || record.keyScope != scope ||
            record.topic != TRACKING_WORK_DRAINED_TOPIC || peer == peers.end() ||
            record.producerRole != peer->second || record.payload.size() > 16384) continue;
        const auto fields = parseFields(record.payload);
        if (fieldOr(fields, "schema", "") == TRACKING_WORK_DRAINED_SCHEMA &&
            fieldOr(fields, "request_id", "") == requestId.toUri() &&
            fieldOr(fields, "run_id", "") == runId &&
            fieldOr(fields, "mission_id", "") == missionId &&
            fieldOr(fields, "window_id", "") == windowPrefix + "-" + std::to_string(windowIndex) &&
            fieldOr(fields, "tracking_mode", "") == (personExpectation ? "person" : "vehicle") &&
            fieldOr(fields, "source_epoch", "") == std::to_string(epoch) &&
            fieldOr(fields, "work_drained", "") == "true") drainedProviders.insert(record.producer);
      }
    }
  }

  struct ResultDrainWait
  {
    ndn::Name requestId;
    size_t windowIndex;
    uint64_t generation;
    std::set<ndn::Name> producers;
    std::chrono::steady_clock::time_point deadline;
    std::function<void()> completion;
  };

  void
  pollResultDrain(const std::shared_ptr<ResultDrainWait>& wait)
  {
    if (finished || cancellationInProgress || wait->generation != resultDrainGeneration) return;
    collectRoleDrains(wait->requestId, wait->windowIndex, wait->producers);
    if (wait->producers.size() == expectedDrainCount()) {
      remoteWorkDrained = true;
      remoteWorkMayExist = false;
      view.waitingForWorkDrain = false;
      wait->completion();
      return;
    }
    if (std::chrono::steady_clock::now() >= wait->deadline) {
      view.reason = "tracking result arrived without every selected role's work-drained proof";
      view.failureStage = "drain";
      exitCode = 1;
      cancelActiveCollaboration(wait->requestId, wait->windowIndex);
      return;
    }
    scheduler.schedule(ndn::time::milliseconds(10), [self = shared_from_this(), wait] {
      self->pollResultDrain(wait);
    });
  }

  void
  afterResultDrain(size_t windowIndex, bool responseStatus, const ndn::Buffer& responsePayload,
                   const std::shared_ptr<std::function<void(bool, const ndn::Buffer&)>>& handler)
  {
    if (compatibility) { (*handler)(responseStatus, responsePayload); return; }
    if (!activeRequestId || activeWindowIndex != windowIndex) {
      view.reason = "tracking result has no active request/window binding";
      exitCode = 1;
      finish();
      return;
    }
    // There is one active request. Preserve its already received response for
    // cancellation's terminal-response race until the proof barrier completes.
    deferredCancellationResponse.emplace(responseStatus, responsePayload);
    view.waitingForWorkDrain = true;
    notify();
    const auto requestId = *activeRequestId;
    auto completion = [self = shared_from_this(), handler, requestId, windowIndex,
                       responseStatus, responsePayload] {
      if (!self->activeRequestId || *self->activeRequestId != requestId ||
          self->activeWindowIndex != windowIndex) return;
      self->deferredCancellationResponse.reset();
      (*handler)(responseStatus, responsePayload);
    };
    auto wait = std::make_shared<ResultDrainWait>(ResultDrainWait{requestId, windowIndex, ++resultDrainGeneration,
      {}, std::chrono::steady_clock::now() + std::chrono::seconds(5), std::move(completion)});
    pollResultDrain(wait);
  }

  void
  finish()
  {
    if (finished) return;
    finished = true;
    halted.store(true);
    view.state = cancellationInProgress && !authorityLost && view.failureStage.empty()
      ? TrackingJobState::CANCELLED
      : (exitCode == 0 ? TrackingJobState::COMPLETED : TrackingJobState::FAILED);
    view.cancelState = cancellationInProgress && remoteWorkMayExist
      ? (remoteCancelConfirmed ? "REMOTE_CONFIRMED" : "REMOTE_UNCONFIRMED") : "NONE";
    view.cleanupState = "DRAINING";
    if (drainDeadline == std::chrono::steady_clock::time_point{}) {
      drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
    if (activeRequestId) {
      const bool committed = !user.GetCollaborationStatusSnapshot(*activeRequestId).empty();
      if (user.CancelCollaboration(*activeRequestId) && !committed) remoteWorkMayExist = false;
      activeRequestId.reset();
    }
    notify();
    scheduler.cancelAllEvents();
    pollDrain();
  }

  void
  pollDrain()
  {
    if (drained || drainCompletionQueued) return;
    if (pendingReads != 0 || (currentAckState && currentAckState->load() == 0)) {
      if (!drainReported && std::chrono::steady_clock::now() >= drainDeadline) {
        drainReported = true;
        view.cleanupState = "INCOMPLETE";
        view.reason = "tracking drain deadline exceeded; dependencies remain owned";
        notify();
        auto completion = std::move(onDrained);
        if (completion) completion({view.jobId, view.cancelState, view.cleanupState, view.reason});
      }
      if (drainReported) return;
      scheduler.schedule(ndn::time::milliseconds(25), [self = shared_from_this()] {
        self->pollDrain();
      });
      return;
    }
    if (cancellationAckThread.joinable()) cancellationAckThread.join();
    drainCompletionQueued = true;
    // Drop member-held callbacks after the current callback has returned.
    boost::asio::post(face.getIoContext(), [self = shared_from_this()] {
      self->releaseWindowScope();
      self->beginWindow.reset();
      self->cancellationResponseHandler.reset();
      self->deferredCancellationResponse.reset();
      self->view.waitingForWorkDrain = false;
      self->publishPresentationFailure = {};
      self->cancelActiveCollaboration = {};
      self->view.cleanupState = (self->compatibility && self->exitCode == 0) ||
        !self->remoteWorkMayExist || self->remoteWorkDrained ? "CLEAN" : "INCOMPLETE";
      self->drained = true;
      self->notify();
      auto completion = std::move(self->onDrained);
      if (completion) completion({self->view.jobId, self->view.cancelState,
                                 self->view.cleanupState, self->view.reason});
    });
  }

  void
  configure(int argc, char** argv, const TrackingJobInputs* inputs = nullptr)
  {
    runId = option(argc, argv, "--run-id", "spec191");
    missionId = option(argc, argv, "--mission-id", runId);
    windowPrefix = option(argc, argv, "--window-prefix", "window");
    windowCount = static_cast<size_t>(std::stoul(
      option(argc, argv, "--window-count", "1")));
    fault = option(argc, argv, "--fault", "none");
    motionDir = option(argc, argv, "--motion-dir");
    observationSchema = option(argc, argv, "--observation-schema");
    observationSourceEpoch = option(argc, argv, "--source-epoch", "0");
    observationSourceUav1 = option(argc, argv, "--source-uav1-id");
    observationSourceUav2 = option(argc, argv, "--source-uav2-id");
    observationSourceUav3 = option(argc, argv, "--source-uav3-id");
    sourceGenerationsPath = option(argc, argv, "--source-generations");
    requireOptionValue(argc, argv, "--tracking-profile");
    requireOptionValue(argc, argv, "--person-computation-manifest");
    trackingProfilePath = option(argc, argv, "--tracking-profile");
    computationManifestPath = option(argc, argv, "--person-computation-manifest");
    requireOptionValue(argc, argv, "--person-presentation-dir");
    personPresentationDir = option(argc, argv, "--person-presentation-dir");
    if (!trackingProfilePath.empty() || !computationManifestPath.empty()) {
      if (trackingProfilePath.empty() || computationManifestPath.empty() ||
          observationSchema != "uav-observation-v2" || !motionDir.empty()) {
        throw std::invalid_argument("person tracking requires profile, computation manifest and "
                                    "observation-v2 inputs without motion-dir");
      }
      const auto profile = inputs ? inputs->personProfile.value() : uav::parseUavPersonTrackingProfile(
        readPersonConfiguration(trackingProfilePath, uav::MAX_PERSON_PROFILE_BYTES));
      const auto manifest = inputs ? inputs->personComputation.value() :
        uav::parseUavPersonComputationManifest(
          readPersonConfiguration(computationManifestPath, uav::MAX_PERSON_COMPUTATION_BYTES), profile);
      personRequestFields = uav::makeUavPersonRequestBindingFields(profile, manifest);
      personExpectation = uav::UavPersonTrackingResultExpectation{profile, manifest.computationDigest};
    }
    wrongPersonProfile = fault == "wrong-person-profile-binding";
    wrongPersonComputation = fault == "wrong-person-computation-binding";
    if (wrongPersonProfile || wrongPersonComputation) {
      if (!personExpectation) {
        throw std::invalid_argument("person binding fault requires person tracking");
      }
      auto& digest = personRequestFields.at(wrongPersonProfile
        ? "tracking_profile_digest" : "tracking_computation_digest");
      // Change only the first hex character after the validated "sha256:" prefix.
      digest.at(7) = digest.at(7) == '0' ? '1' : '0';
    }
    if (!personPresentationDir.empty() && !personExpectation) {
      throw std::invalid_argument("person presentation requires the explicit person profile and manifest");
    }
    if ((fault == "replay-committed-window" || fault == "alter-committed-window") &&
        (!personExpectation || personPresentationDir.empty())) {
      throw std::invalid_argument("committed window replay requires person presentation");
    }
    requireOptionValue(argc, argv, "--calibration-provider");
    calibrationProviderText = option(argc, argv, "--calibration-provider");
    calibrationProvider = ndn::Name(calibrationProviderText);
    if (!calibrationProviderText.empty() &&
        (calibrationProvider.empty() || calibrationProviderText.front() != '/')) {
      throw std::invalid_argument("--calibration-provider must be an absolute NDN identity name");
    }
    observationEpoch = 0;
    groupPrefix = ndn::Name(option(argc, argv, "--group-prefix", "/example/uav"));
    controllerPrefix = ndn::Name(
      option(argc, argv, "--controller-prefix", "/example/uav/controller"));
    trustSchema = option(argc, argv, "--trust-schema",
                                           "examples/trust-any.conf");
    userIdentity = ndn::Name(
      option(argc, argv, "--user-identity", "/example/uav/gs"));
    startDelayMs = std::stoi(option(argc, argv, "--start-delay-ms", "2000"));
    controllerReadyTimeoutMs = std::stoi(
      option(argc, argv, "--controller-ready-timeout-ms", "30000"));
    controllerReadyRetryMs = std::stoi(
      option(argc, argv, "--controller-ready-retry-ms", "250"));
    requestTimeoutMs = std::stoi(
      option(argc, argv, "--request-timeout-ms", "60000"));
    if (missionId.empty() || windowPrefix.empty() || windowCount == 0 || windowCount > 600 ||
        startDelayMs < 0 ||
        controllerReadyTimeoutMs <= 0 || controllerReadyRetryMs <= 0 ||
        requestTimeoutMs <= 1000 || requestTimeoutMs > 1200000) {
      throw std::invalid_argument(
        "mission/window and timing values must be valid; request timeout must exceed ACK timeout");
    }
    if (!observationSchema.empty() &&
        (observationSchema != "uav-observation-v2" || observationSourceEpoch == "0" ||
         observationSourceUav1.empty() || observationSourceUav2.empty() ||
         observationSourceUav3.empty())) {
      throw std::invalid_argument("observation request identity is incomplete");
    }
    if (!observationSchema.empty()) {
      observationEpoch = std::stoull(observationSourceEpoch);
      if (observationEpoch == 0 || std::to_string(observationEpoch) != observationSourceEpoch) {
        throw std::invalid_argument("observation source epoch must be a positive canonical integer");
      }
    }
    else if (!sourceGenerationsPath.empty()) {
      throw std::invalid_argument("source generation manifest requires the observation request schema");
    }

    initialSourceIds = {
      observationSourceUav1, observationSourceUav2, observationSourceUav3};
    if (!observationSchema.empty()) {
      sourceGenerations = inputs ? inputs->sourceGenerations : sourceGenerationsPath.empty()
        ? uav::makeUavSourceGenerationSchedule(observationEpoch, initialSourceIds, windowCount)
        : uav::loadUavSourceGenerationSchedule(sourceGenerationsPath, windowCount);
      const auto& initialGeneration = sourceGenerations->generations.front();
      if (initialGeneration.sourceEpoch != observationEpoch ||
          initialGeneration.sourceIds != initialSourceIds) {
        throw std::invalid_argument("source generation manifest initial identity does not match GroundStation options");
      }
    }

    presentationRoot = std::filesystem::path(personPresentationDir);
    if (!personPresentationDir.empty()) {
      const auto contextPath = presentationRoot / "context.json";
      if (std::filesystem::exists(presentationRoot) &&
          !std::filesystem::is_empty(presentationRoot) && !std::filesystem::exists(contextPath)) {
        throw std::invalid_argument("person presentation directory is nonempty and has no owner context");
      }
      PersonJson generations = PersonJson::array();
      for (const auto& generation : sourceGenerations->generations) {
        generations.push_back({{"firstWindowIndex", generation.firstWindowIndex},
          {"sourceEpoch", generation.sourceEpoch}, {"sourceIds", generation.sourceIds}});
      }
      const auto& profile = personExpectation->profile;
      const PersonJson context = {{"schema", "uav-person-display-context-v1"}, {"runId", runId},
        {"missionId", missionId}, {"profileDigest", profile.profileDigest},
        {"computationDigest", personExpectation->computationDigest},
        {"requestedDevice", profile.requestedDevice}, {"windowCount", windowCount},
        {"windowPrefix", windowPrefix},
        {"maxFrameBytes", profile.limits.at("maxFrameBytes")},
        {"maxFrameWidth", profile.limits.at("maxFrameWidth")},
        {"maxFrameHeight", profile.limits.at("maxFrameHeight")},
        {"maxPersonsPerFrame", profile.limits.at("maxPersonsPerFrame")},
        {"generations", generations}};
      writePersonArtifact(contextPath, context.dump());
    }
  }

  void
  begin()
  {
    publishPresentationFailure = [&, self = shared_from_this()] (size_t windowIndex, const std::string& reason) {
      view.reason = reason;
      if (compatibility && personPresentationDir.empty()) return;
      halted.store(true);
      try {
        PersonJson failure = {{"schema", personExpectation ? "uav-person-display-v1" :
                                                          "uav-tracking-display-v1"},
          {"state", "failure"}, {"runId", runId}, {"missionId", missionId},
          {"windowId", windowPrefix + "-" + std::to_string(windowIndex)},
          {"reason", reason.substr(0, 1024)}, {"lastGoodPresentation", lastGoodPresentation}};
        if (personExpectation) {
          failure["profileDigest"] = personExpectation->profile.profileDigest;
          failure["computationDigest"] = personExpectation->computationDigest;
        }
        writePersonArtifact(presentationRoot / "latest.json", failure.dump(), true);
      }
      catch (const std::exception& error) {
        std::cerr << "SPEC219_PRESENTATION_WRITE_FAILED reason=" << error.what() << std::endl;
      }
    };
    cancelActiveCollaboration = [&, self = shared_from_this()] (const ndn::Name& requestId,
                                                 size_t windowIndex) {
      if (cancellationInProgress) return;
      cancellationInProgress = true;
      ++resultDrainGeneration;
      view.state = TrackingJobState::CANCELLING;
      view.cancelState = "REQUESTED";
      view.cleanupState = "DRAINING";
      if (drainDeadline == std::chrono::steady_clock::time_point{}) {
        drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      }
      notify();
      // Core records committed participants before publishing any Selection.
      // On the same Face thread, a successful pre-commit cancellation fences
      // the pending plan without claiming a remote-stop receipt.
      if (user.GetCollaborationStatusSnapshot(requestId).empty() &&
          user.CancelCollaboration(requestId)) {
        remoteWorkMayExist = false;
        activeRequestId.reset();
        exitCode = 1;
        finish();
        return;
      }
      bool cancelNoticeQueued = false;
      if (!activeComputeProvider) activeComputeProvider = computeProvider;
      if (activeComputeProvider) {
        cancelNoticeQueued = user.publishCollaborationData(
          *activeComputeProvider, requestId, uav::TRACKING_INPUT_KEY_SCOPE,
          uav::TRACKING_CANCEL_TOPIC, fieldsBuffer({
            {"schema", uav::TRACKING_CANCEL_SCHEMA},
            {"request_id", requestId.toUri()},
            {"run_id", runId},
            {"mission_id", missionId},
            {"window_id", windowPrefix + "-" + std::to_string(windowIndex)},
          }));
      }
      if (!compatibility) {
        for (const auto& source : sourceProviders) {
          user.publishCollaborationData(source, requestId, TRACKING_INPUT_KEY_SCOPE,
            TRACKING_CANCEL_TOPIC, fieldsBuffer({{"schema", TRACKING_CANCEL_SCHEMA},
              {"request_id", requestId.toUri()}, {"run_id", runId}, {"mission_id", missionId},
              {"window_id", windowPrefix + "-" + std::to_string(windowIndex)}}));
        }
      }
      std::cerr << "SPEC210_CANCEL_NOTICE_QUEUED window=" << windowIndex
                << " accepted=" << (cancelNoticeQueued ? "true" : "false") << std::endl;
      auto ackState = std::make_shared<std::atomic<int>>(0);
      revocationSeen = std::make_shared<std::atomic<bool>>(false);
      workDrainedSeen = std::make_shared<std::atomic<bool>>(false);
      currentAckState = ackState;
      if (cancelNoticeQueued && activeComputeProvider) {
        const ndn::Name expectedProvider = *activeComputeProvider;
        const auto expectedRunId = runId;
        const auto expectedMissionId = missionId;
        const auto expectedWindowId = windowPrefix + "-" + std::to_string(windowIndex);
        const auto ackTimeout = fault == "user-cancel-terminal-response-race" ||
          fault == "user-cancel-terminal-response-cancel-wins-race"
          ? std::chrono::seconds(20) : std::chrono::seconds(5);
        try {
          cancellationAckThread = std::thread(
            [this, self = shared_from_this(), ackState, requestId, expectedProvider, expectedRunId,
             expectedMissionId, expectedWindowId, windowIndex, ackTimeout] {
              try {
                const auto deadline = std::chrono::steady_clock::now() +
                  ackTimeout;
                size_t nextCount = 1;
                std::set<ndn::Name> drainedProviders;
                const auto waitBound = [&] {
                  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                  return static_cast<int>(std::max<int64_t>(1, std::min<int64_t>(50, remaining)));
                };
                while (std::chrono::steady_clock::now() < deadline) {
                  const auto records = user.waitForVerifiedCollaborationData(
                    requestId, TRACKING_INPUT_KEY_SCOPE,
                    TRACKING_CANCEL_ACK_TOPIC, nextCount, waitBound(), false);
                  for (const auto& record : records) {
                    if (record.payload.size() > 16384 || record.requestId != requestId ||
                        record.keyScope != uav::TRACKING_INPUT_KEY_SCOPE ||
                        record.topic != uav::TRACKING_CANCEL_ACK_TOPIC ||
                        record.producer != expectedProvider ||
                        record.producerRole != "Tracking-Compute") {
                      continue;
                    }
                    const auto fields = uav::decodeFields(std::string(
                      reinterpret_cast<const char*>(record.payload.data()),
                      record.payload.size()));
                    if (uav::fieldOr(fields, "schema", "") ==
                          uav::TRACKING_CANCEL_ACK_SCHEMA &&
                        uav::fieldOr(fields, "request_id", "") == requestId.toUri() &&
                        uav::fieldOr(fields, "run_id", "") == expectedRunId &&
                        uav::fieldOr(fields, "mission_id", "") == expectedMissionId &&
                        uav::fieldOr(fields, "window_id", "") == expectedWindowId &&
                        uav::fieldOr(fields, "authority_revoked", "") == "true") {
                      if (!revocationSeen->exchange(true, std::memory_order_acq_rel)) {
                        boost::asio::post(face.getIoContext(), [self] {
                          if (self->finished) return;
                          self->view.cancelState = "REMOTE_CONFIRMED";
                          self->notify();
                        });
                      }
                    }
                  }
                  nextCount = records.size() + 1;
                  if (compatibility && revocationSeen->load(std::memory_order_acquire)) {
                    ackState->store(1, std::memory_order_release);
                    return;
                  }
                  if (!compatibility) collectRoleDrains(requestId, windowIndex, drainedProviders);
                  if (!compatibility && drainedProviders.size() == expectedDrainCount()) {
                    workDrainedSeen->store(true, std::memory_order_release);
                    ackState->store(1, std::memory_order_release);
                    return;
                  }
                }
                ackState->store(2, std::memory_order_release);
              }
              catch (...) {
                ackState->store(3, std::memory_order_release);
              }
            });
        }
        catch (const std::exception&) {
          ackState->store(3, std::memory_order_release);
        }
      }
      else {
        ackState->store(4, std::memory_order_release);
      }

      auto finishCancellation = std::make_shared<std::function<void()>>();
      auto pollCancellation = std::make_shared<std::function<void()>>();
      const std::weak_ptr<std::function<void()>> weakPollCancellation =
        pollCancellation;
      *finishCancellation = [&, self = shared_from_this(), ackState, requestId, windowIndex] {
        if (cancellationAckThread.joinable()) cancellationAckThread.join();
        const int ackStatus = ackState->load(std::memory_order_acquire);
        remoteCancelConfirmed = revocationSeen->load(std::memory_order_acquire);
        remoteWorkDrained = workDrainedSeen->load(std::memory_order_acquire);
        if (remoteCancelConfirmed) {
          std::cerr << "SPEC210_CANCEL_ACK_CONFIRMED window=" << windowIndex
                    << " provider=" << activeComputeProvider->toUri() << std::endl;
        }
        else {
          const char* reason = ackStatus == 2 ? "timeout" :
            (ackStatus == 3 ? "wait-failed" : "publication-unavailable");
          std::cerr << "SPEC210_CANCEL_ACK_UNCONFIRMED window=" << windowIndex
                    << " reason=" << reason << std::endl;
        }
        const bool cancelled = user.CancelCollaboration(requestId);
        auto finishOutcome = std::make_shared<std::function<void(bool)>>();
        *finishOutcome = [&, self = shared_from_this(), requestId, windowIndex] (bool cancelAccepted) {
          if (finished) return;
          if (!cancelAccepted && deferredCancellationResponse &&
              (authorityLost || view.failureStage == "deadline" || view.failureStage == "drain")) {
            deferredCancellationResponse.reset();
            cancellationCompletionPending = false;
            exitCode = 1;
            finish();
            return;
          }
          if (!cancelAccepted && deferredCancellationResponse && cancellationResponseHandler &&
              *cancellationResponseHandler) {
            auto response = std::move(*deferredCancellationResponse);
            deferredCancellationResponse.reset();
            cancellationCompletionPending = false;
            cancellationInProgress = false;
            std::cerr << "SPEC210_CANCEL_RACE_TERMINAL_RESPONSE_WON window="
                      << windowIndex << std::endl;
            const auto handler = cancellationResponseHandler;
            afterResultDrain(windowIndex, response.first, response.second, handler);
            return;
          }
          deferredCancellationResponse.reset();
          cancellationCompletionPending = false;
          if (activeRequestId && *activeRequestId == requestId) {
            activeRequestId.reset();
          }
          publishPresentationFailure(windowIndex, "collaboration cancelled");
          std::cerr << "SPEC191_TRACKING_CANCELLED window=" << windowIndex
                    << " accepted=" << (cancelAccepted ? "true" : "false") << std::endl;
          exitCode = 1;
          finish();
        };
        if (!cancelled && !deferredCancellationResponse) {
          // CancelCollaboration(false) means ServiceUser already removed the
          // pending call. Its response callback may still be queued on the
          // ServiceUser worker pool, so allow that callback to reach the Face
          // event loop before declaring cancellation terminal.
          cancellationCompletionPending = true;
          std::cerr << "SPEC210_CANCEL_RACE_WAITING_FOR_RESPONSE window="
                    << windowIndex << std::endl;
          const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
          auto pollResponse = std::make_shared<std::function<void()>>();
          const std::weak_ptr<std::function<void()>> weakPollResponse = pollResponse;
          *pollResponse = [&, self = shared_from_this(), weakPollResponse, finishOutcome, deadline, windowIndex] {
            if (deferredCancellationResponse) {
              (*finishOutcome)(false);
              return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
              std::cerr << "SPEC210_CANCEL_RACE_RESPONSE_TIMEOUT window="
                        << windowIndex << std::endl;
              (*finishOutcome)(false);
              return;
            }
            if (const auto poll = weakPollResponse.lock()) {
              scheduler.schedule(ndn::time::milliseconds(25), [poll] { (*poll)(); });
            }
          };
          scheduler.schedule(ndn::time::milliseconds(25), [pollResponse] {
            (*pollResponse)();
          });
          return;
        }
        (*finishOutcome)(cancelled);
      };
      *pollCancellation = [&, self = shared_from_this(), ackState, weakPollCancellation, finishCancellation] {
        if (ackState->load(std::memory_order_acquire) == 0) {
          if (const auto poll = weakPollCancellation.lock()) {
            scheduler.schedule(ndn::time::milliseconds(25), [poll] { (*poll)(); });
          }
          return;
        }
        (*finishCancellation)();
      };
      scheduler.schedule(ndn::time::milliseconds(0), [pollCancellation] {
        (*pollCancellation)();
      });
    };
    bootstrapStarted = std::chrono::steady_clock::now();
    if (personExpectation) {
      uav::emitUavPersonTrace({{"runId", runId}, {"missionId", missionId}}, "replay-origin");
    }
    beginWindow = std::make_shared<std::function<void(size_t)>>();
    *beginWindow = [&, self = shared_from_this()] (size_t windowIndex) {
      if (finished) return;
      if (!checkLiveAuthority()) return;
      activeWindowIndex = windowIndex;
      const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - bootstrapStarted).count();
      const auto fail = [&, self = shared_from_this()] (const std::string& reason) {
        std::cerr << "SPEC191_TRACKING_PLAN_FAILED reason=" << reason << std::endl;
        publishPresentationFailure(windowIndex, reason);
        exitCode = 1;
        finish();
      };
      if (!user.getControllerVersion(uav::SERVICE_TRACKING_WINDOW)) {
        if (elapsedMs >= controllerReadyTimeoutMs) {
          fail("ControllerVersion readiness timeout");
          return;
        }
        std::cout << "SPEC191_TRACKING_WAIT_CONTROLLER_VERSION elapsed_ms="
                  << elapsedMs << " retry_ms=" << controllerReadyRetryMs << std::endl;
        scheduler.schedule(ndn::time::milliseconds(controllerReadyRetryMs), [beginWindow = this->beginWindow, windowIndex] {
          (*beginWindow)(windowIndex);
        });
        return;
      }
      const std::string windowId = windowPrefix + "-" + std::to_string(windowIndex);
      uav::UavSourceGeneration windowGeneration;
      if (!observationSchema.empty()) {
        try {
          windowGeneration = uav::uavSourceGenerationForWindow(*sourceGenerations, windowIndex);
        }
        catch (const std::exception& error) {
          fail(error.what());
          return;
        }
        if (windowGeneration.firstWindowIndex == windowIndex && windowIndex != 0) {
          std::cout << "SPEC210_SOURCE_GENERATION_ACTIVATED first_window=" << windowIndex
                    << " source_epoch=" << windowGeneration.sourceEpoch << std::endl;
        }
      }
      const auto motionJson = readMotionWindow(motionDir, windowId);
      const auto motionDigest = motionJson.empty() ? std::string() : sha256Text(motionJson);
      auto assignment = fieldsBuffer({
        {"schema", "spec191-assignment-v1"},
        {"run_id", runId},
        {"mission_id", missionId},
        {"window_id", windowId},
      });
      if (!calibrationProvider.empty()) {
        auto assignmentText = std::string(
          reinterpret_cast<const char*>(assignment.data()), assignment.size());
        assignmentText += ";roleProvider.Calibration-Provider=" +
                          calibrationProvider.toUri();
        assignment = ndn::Buffer(
          reinterpret_cast<const uint8_t*>(assignmentText.data()), assignmentText.size());
      }
      uav::Fields requestFields{
        {"schema", "spec191-window-request-v1"},
        {"tracking_request_timeout_ms", std::to_string(requestTimeoutMs)},
        {"run_id", runId},
        {"mission_id", missionId},
        {"window_id", windowId},
      };
      requestFields.insert(personRequestFields.begin(), personRequestFields.end());
      if (fault == "alter-committed-window" && alteredReplaySent) {
        requestFields.emplace("f4_altered_frame", "1");
      }
      if (!observationSchema.empty()) {
        requestFields.emplace("observation_schema", observationSchema);
        requestFields.emplace("source_epoch", std::to_string(windowGeneration.sourceEpoch));
        requestFields.emplace("source_uav1_id", windowGeneration.sourceIds[0]);
        requestFields.emplace("source_uav2_id", windowGeneration.sourceIds[1]);
        requestFields.emplace("source_uav3_id", windowGeneration.sourceIds[2]);
      }
      if (!motionJson.empty()) {
        requestFields.emplace("motion_schema", "spec191-motion-window-v1");
        requestFields.emplace("motion_window_digest", motionDigest);
        requestFields.emplace("motion_window_json", motionJson);
      }
      const auto request = fieldsBuffer(requestFields);
      auto makeCallbacks = [&, self = shared_from_this(), windowIndex, motionDigest, windowGeneration] {
        auto selectedComputeProvider = std::make_shared<ndn::Name>();
        auto responseHandler = std::make_shared<
          std::function<void(bool, const ndn::Buffer&)>>();
        cancellationResponseHandler = responseHandler;
        uav::UavTrackingCoordinatorCallbacks callbacks;
        callbacks.onPlanCommitted = [&, self = shared_from_this(), windowIndex, selectedComputeProvider] (
          const nsf::CollaborationAckClosure& closure,
          const std::vector<nsf::SelectedParticipant>& selected) {
          const auto requestId = closure.requestId;
          const auto selectedParticipants = selected;
          boost::asio::post(face.getIoContext(),
            [&, self = shared_from_this(), windowIndex, selectedComputeProvider, requestId, selectedParticipants] {
              if (finished) return;
              view.state = TrackingJobState::RUNNING;
              view.selectedProviders.clear();
              for (const auto& participant : selectedParticipants) {
                view.selectedProviders.push_back(participant.provider);
              }
              notify();
              activeRequestId = requestId;
              activeWindowIndex = windowIndex;
              std::cout << "SPEC191_TRACKING_PLAN_COMMITTED request="
                        << requestId.toUri()
                        << " participants=" << selectedParticipants.size() << std::endl;
              for (const auto& participant : selectedParticipants) {
                std::cout << "SPEC191_TRACKING_SELECTED role=" << participant.role
                          << " provider=" << participant.provider.toUri() << std::endl;
                if (participant.role == "Tracking-Compute") {
                  *selectedComputeProvider = participant.provider;
                  activeComputeProvider = participant.provider;
                }
              }
              if (fault == "cancel") {
                scheduler.schedule(ndn::time::milliseconds(0),
                  [&, self = shared_from_this(), requestId, windowIndex] {
                    cancelActiveCollaboration(requestId, windowIndex);
                  });
              }
            });
        };
        *responseHandler = [&, self = shared_from_this(), windowIndex, motionDigest, selectedComputeProvider,
                            windowGeneration] (
          bool responseStatus, const ndn::Buffer& responsePayload) {
          if (finished || !checkLiveAuthority()) return;
          const auto fields = parseFields(responsePayload);
          const auto accepted = uav::fieldOr(fields, "accepted", "");
          const auto failureReason = uav::fieldOr(fields, "failure_reason", "");
          const auto resultJson = uav::fieldOr(fields, "result_json", "");
          const auto resultDigest = uav::fieldOr(fields, "result_digest", "");
          const bool resultDigestVerified = responseStatus && accepted == "true" &&
            !resultJson.empty() && !resultDigest.empty() &&
            sha256Text(resultJson) == resultDigest;
          bool resultContextVerified = false;
          std::string resultContextError;
          if (resultDigestVerified) {
            try {
              uav::UavTrackingResultExpectation expected;
              expected.runId = runId;
              expected.missionId = missionId;
              expected.windowId = windowPrefix + "-" + std::to_string(windowIndex);
              expected.sequence = windowIndex + 1;
              expected.sourceEpoch = windowGeneration.sourceEpoch;
              expected.sourceIds = windowGeneration.sourceIds;
              expected.inputDigests = {
                uav::fieldOr(fields, "input_uav1_digest", ""),
                uav::fieldOr(fields, "input_uav2_digest", ""),
                uav::fieldOr(fields, "input_uav3_digest", ""),
              };
              expected.computeProvider = *selectedComputeProvider;
              std::copy_n(sourceProviders.begin(), expected.sourceProviders.size(),
                          expected.sourceProviders.begin());
              expected.personTracking = personExpectation;
              if (!personExpectation) {
                expected.vehicleTracking = uav::UavVehicleTrackingResultExpectation{
                  !observationSchema.empty(), {}};
              }
              uav::validateUavTrackingResultContext(
                uav::fieldOr(fields, "result_name", ""),
                uav::fieldOr(fields, "provider", ""), resultJson, expected);
              resultContextVerified = true;
            }
            catch (const std::exception& error) {
              resultContextError = error.what();
            }
          }
          const bool resultVerified = resultDigestVerified && resultContextVerified;
          const bool motionVerified = motionDigest.empty() ||
            uav::fieldOr(fields, "motion_window_digest", "") == motionDigest;
          if (responseStatus && !motionVerified) {
            std::cerr << "SPEC191_TRACKING_MOTION_VERIFY_FAILED window=" << windowIndex
                      << " expected_digest=" << motionDigest
                      << " received_digest="
                      << uav::fieldOr(fields, "motion_window_digest", "") << std::endl;
          }
          if (responseStatus && accepted == "true" && !resultDigestVerified) {
            std::cerr << "SPEC191_TRACKING_RESULT_VERIFY_FAILED"
                      << " expected_digest=" << resultDigest
                      << " result_bytes=" << resultJson.size() << std::endl;
          }
          if (responseStatus && accepted == "true" && resultDigestVerified &&
              !resultContextVerified) {
            std::cerr << "SPEC191_TRACKING_RESULT_CONTEXT_VERIFY_FAILED window=" << windowIndex
                      << " reason=" << resultContextError << std::endl;
          }
          const bool success = responseStatus && resultVerified && motionVerified;
          if (success) {
            view.completedWindows = windowIndex + 1;
            view.lastResultDigest = resultDigest;
          view.latestResult = std::make_shared<const std::string>(resultJson);
          if (personExpectation) {
            const auto result = person_json::parseBounded(
              resultJson, personExpectation->profile.limits.at("maxResultBytes"));
            view.actualDevice = result.at("actualDetectorDevice").get<std::string>();
            view.sourcePtsUs = result.at("frames").at(0).at("commonPtsUs").get<int64_t>();
          }
          else {
            view.actualDevice = "cpu";
            const auto result = person_json::parseBounded(resultJson, 8 * 1024 * 1024);
            view.sourcePtsUs = result.at("frames").at(0).at("ptsUs").get<int64_t>();
          }
            notify();
          }
          std::cout << "SPEC191_TRACKING_WINDOW_RESULT window=" << windowIndex
                    << " status=" << (success ? "success" : "failure")
                    << " response_status=" << (responseStatus ? "success" : "failure")
                    << " result_name=" << uav::fieldOr(fields, "result_name", "")
                    << " result_digest=" << resultDigest
                    << " result_bytes=" << resultJson.size()
                    << " result_verified=" << (resultVerified ? "true" : "false")
                    << " failure_reason=" << failureReason
                    << std::endl;
          if (!success) {
            std::cerr << "SPEC191_TRACKING_RESULT status=failure window=" << windowIndex;
            if (!failureReason.empty()) {
              std::cerr << " reason=" << failureReason;
            }
            std::cerr << std::endl;
            publishPresentationFailure(windowIndex, failureReason.empty()
              ? "native result validation failed" : failureReason);
            exitCode = 1;
            activeRequestId.reset();
            finish();
            return;
          }
          const auto advanceAfterPresentation = [&, self = shared_from_this(), windowIndex, beginWindow = this->beginWindow] {
            releaseWindowScope();
            if (stopRequested && windowIndex + 1 < windowCount) {
              cancellationInProgress = true;
              remoteWorkMayExist = false;
              exitCode = 1;
              finish();
              return;
            }
            if ((fault == "replay-committed-window" || fault == "alter-committed-window") &&
                windowIndex == 0 &&
                !committedReplaySent) {
              committedReplaySent = true;
              std::cout << "SPEC219_COMMITTED_WINDOW_REPLAY_REQUESTED window=window-0"
                        << std::endl;
              if (fault == "alter-committed-window") {
                replayWaiting = true;
                replayDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                std::cout << "SPEC219_COMMITTED_WINDOW_REPLAY_READY checkpoint=original"
                          << std::endl;
              }
              else {
                scheduler.schedule(ndn::time::milliseconds(0), [beginWindow = this->beginWindow] {
                  (*beginWindow)(0);
                });
              }
              return;
            }
            if (fault == "alter-committed-window" && windowIndex == 0 && !alteredReplaySent) {
              alteredReplaySent = true;
              replayWaiting = true;
              replayDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
              std::cout << "SPEC219_COMMITTED_WINDOW_REPLAY_READY checkpoint=cached" << std::endl;
              return;
            }
            if (windowIndex + 1 < windowCount) {
              scheduler.schedule(ndn::time::milliseconds(0), [beginWindow = this->beginWindow, windowIndex] {
                (*beginWindow)(windowIndex + 1);
              });
              return;
            }
            std::cout << "SPEC191_TRACKING_RESULT status=success window=" << windowIndex
                      << " windows=" << windowCount << std::endl;
            exitCode = 0;
            halted.store(true);
            finish();
          };
          if ((fault == "replay-committed-window" || fault == "alter-committed-window") &&
              windowIndex == 0) {
            if (alteredReplaySent) {
              publishPresentationFailure(windowIndex, "altered committed window unexpectedly succeeded");
              activeRequestId.reset();
              exitCode = 1;
              finish();
              return;
            }
            if (!committedReplayResult) {
              committedReplayResult = resultJson;
            }
            else if (committedReplaySent) {
              if (resultJson != *committedReplayResult) {
                publishPresentationFailure(windowIndex, "committed window replay changed result bytes");
                activeRequestId.reset();
                exitCode = 1;
                finish();
                return;
              }
              std::cout << "SPEC219_COMMITTED_WINDOW_REPLAY_VERIFIED window=window-0"
                        << " result_digest=" << resultDigest << std::endl;
              // Keep the first immutable presentation, including its timing.
              activeRequestId.reset();
              advanceAfterPresentation();
              return;
            }
          }
          if (!compatibility || !personPresentationDir.empty()) {
            if (!checkLiveAuthority()) return;
            activeRequestId.reset();
            auto batch = std::make_shared<PersonFrameReadBatch>();
            batch->resultJson = resultJson;
            batch->resultDigest = resultDigest;
            batch->resultName = uav::fieldOr(fields, "result_name", "");
            batch->responseProvider = uav::fieldOr(fields, "provider", "");
            const auto failPresentation = [&, self = shared_from_this(), batch, windowIndex] (const std::string& reason) {
              if (batch->terminal) return;
              batch->terminal = true;
              view.failureStage = "presentation";
              publishPresentationFailure(windowIndex, reason);
              std::cerr << "SPEC219_PRESENTATION_FAILED window=" << windowIndex
                        << " reason=" << reason << std::endl;
              exitCode = 1;
              halted.store(true);
              finish();
            };
            try {
              const auto result = uav::person_json::parseBounded(
                resultJson, personExpectation ? personExpectation->profile.limits.at("maxResultBytes")
                                              : 8 * 1024 * 1024);
              int64_t firstPts = std::numeric_limits<int64_t>::max();
              for (size_t i = 0; i < 3; ++i) {
                const auto& frame = result.at("frames").at(i);
                auto& binding = batch->bindings[i];
                binding.runId = runId;
                binding.missionId = missionId;
                binding.windowId = windowPrefix + "-" + std::to_string(windowIndex);
                binding.cameraId = frame.at("cameraId").get<std::string>();
                binding.sourceId = frame.at("sourceId").get<std::string>();
                binding.sourceEpoch = frame.at("sourceEpoch").get<uint64_t>();
                binding.sequence = frame.at("sequence").get<uint64_t>();
                const auto pts = frame.at(personExpectation ? "commonPtsUs" : "ptsUs").get<int64_t>();
                if (personExpectation) {
                  uav::emitUavPersonTrace({{"runId", runId}, {"missionId", missionId},
                    {"windowId", binding.windowId}, {"sequence", binding.sequence},
                    {"sourceEpoch", binding.sourceEpoch}, {"cameraId", binding.cameraId},
                    {"sourceId", binding.sourceId}, {"commonPtsUs", pts}}, "received");
                }
                const auto& reference = personExpectation ? frame.at("observation").at("imageRef")
                                                          : frame.at("imageRef");
                binding.frameName = reference.at("name").get<std::string>();
                binding.inputDigest = frame.at("inputDigest").get<std::string>();
                batch->dimensions[i] = {
                  frame.at(personExpectation ? "width" : "imageWidth").get<uint64_t>(),
                  frame.at(personExpectation ? "height" : "imageHeight").get<uint64_t>()};
                firstPts = std::min(firstPts, pts);
              }
              if (!presentationMediaOriginUs) presentationMediaOriginUs = firstPts;
              for (size_t i = 0; i < 3; ++i) {
                if (!checkLiveAuthority()) return;
                nsf::RequestMessage frameRequest;
                auto payload = fieldsBuffer(uav::makeUavPersonFrameReadFields(batch->bindings[i]));
                frameRequest.setPayload(payload, payload.size());
                auto settled = std::make_shared<bool>(false);
                ++pendingReads;
                ndn::Name requestId;
                try {
                  requestId = user.RequestServiceTargeted(
                  sourceProviders[i], uav::SERVICE_TRACKING_SOURCE, frameRequest, 5000,
                  [&, self = shared_from_this(), batch, settled, failPresentation, i] (const ndn::Name&) {
                    boost::asio::post(face.getIoContext(), [self = shared_from_this(), batch, settled, failPresentation, i] {
                      self->settleRead(settled);
                      if (!self->finished && !batch->terminal) {
                        failPresentation("source FRAME read timed out for UAV" + std::to_string(i + 1));
                      }
                    });
                  },
                  [&, self = shared_from_this(), batch, settled, failPresentation, advanceAfterPresentation, i, windowIndex]
                  (const nsf::ResponseMessage& response) {
                    const auto status = response.getStatus();
                    const auto bytes = response.getPayload();
                    boost::asio::post(face.getIoContext(),
                      [&, self = shared_from_this(), batch, settled, failPresentation, advanceAfterPresentation, i, windowIndex,
                       status, bytes] {
                        settleRead(settled);
                        if (finished || batch->terminal || batch->received[i]) return;
                        if (!checkLiveAuthority()) return;
                        try {
                          const auto maxBytes = personExpectation
                            ? personExpectation->profile.limits.at("maxFrameBytes")
                            : MAX_PERSON_PUBLISHED_FRAME_BYTES;
                          if (!status || bytes.empty() ||
                              bytes.size() > maxBytes ||
                              sha256Text(std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                         bytes.size())) != batch->bindings[i].inputDigest) {
                            throw std::invalid_argument("source FRAME response status/size/digest mismatch");
                          }
                          batch->images[i] = bytes;
                          batch->received[i] = true;
                          --batch->remaining;
                          std::cout << "SPEC219_SOURCE_FRAME_RECEIVED window=" << windowIndex
                                    << " camera=" << batch->bindings[i].cameraId
                                    << " producer=" << sourceProviders[i].toUri()
                                    << " frame_name=" << batch->bindings[i].frameName
                                    << " digest=" << batch->bindings[i].inputDigest
                                    << " bytes=" << bytes.size() << std::endl;
                          if (batch->remaining != 0) return;
                          std::shared_ptr<TrackingPresentation> presentation;
                          if (!compatibility) {
                            presentation = std::make_shared<TrackingPresentation>();
                            presentation->mode = definition.mode;
                            presentation->runId = runId;
                            presentation->generation = view.generation;
                            presentation->windowIndex = windowIndex;
                            presentation->resultDigest = batch->resultDigest;
                            presentation->resultJson = view.latestResult;
                            for (size_t j = 0; j < 3; ++j) {
                              const auto& binding = batch->bindings[j];
                              auto& image = presentation->frames[j];
                              image.cameraId = binding.cameraId;
                              image.sourceId = binding.sourceId;
                              image.sourceEpoch = binding.sourceEpoch;
                              image.sequence = binding.sequence;
                              image.producerIdentity = sourceProviders[j];
                              image.frameName = binding.frameName;
                              image.inputDigest = binding.inputDigest;
                              image.width = batch->dimensions[j].first;
                              image.height = batch->dimensions[j].second;
                              image.jpeg = std::make_shared<const ndn::Buffer>(std::move(batch->images[j]));
                            }
                            projectTrackingPresentation(*presentation, definition, view.generation,
                                                        view.lastResultDigest);
                            writePersonArtifact(definition.outputDirectory / "results" /
                              ("window-" + std::to_string(windowIndex) + ".json"), batch->resultJson);
                          }
                          PersonJson images = PersonJson::array();
                          const auto relativeWindow = "window-" + std::to_string(windowIndex);
                          for (size_t j = 0; j < 3; ++j) {
                            const auto relativePath = relativeWindow + "/" +
                              batch->bindings[j].cameraId + ".jpg";
                            const auto& bytes = presentation ? *presentation->frames[j].jpeg : batch->images[j];
                            writePersonArtifact(presentationRoot / relativePath,
                              std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                            images.push_back({{"cameraId", batch->bindings[j].cameraId},
                              {"path", relativePath}, {"inputDigest", batch->bindings[j].inputDigest}});
                          }
                          const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - bootstrapStarted).count();
                          PersonJson projection = {{"schema", personExpectation ? "uav-person-display-v1" :
                                                                                  "uav-tracking-display-v1"},
                            {"state", "ready"}, {"runId", runId}, {"missionId", missionId},
                            {"resultJson", batch->resultJson}, {"resultDigest", batch->resultDigest},
                            {"resultName", batch->resultName},
                            {"responseProvider", batch->responseProvider},
                            {"images", images}, {"gsElapsedUs", elapsedUs},
                            {"mediaOriginUs", *presentationMediaOriginUs},
                            {"timeBasis", "gs-bootstrap-elapsed-vs-first-common-media-pts"}};
                          if (personExpectation) {
                            projection["profileDigest"] = personExpectation->profile.profileDigest;
                            projection["computationDigest"] = personExpectation->computationDigest;
                          }
                          const auto serialized = projection.dump();
                          const auto relativeManifest = relativeWindow + "/presentation.json";
                          writePersonArtifact(presentationRoot / relativeManifest, serialized);
                          writePersonArtifact(presentationRoot / "latest.json", serialized, true);
                          lastGoodPresentation = relativeManifest;
                          std::cout << "SPEC219_PRESENTATION_READY window=" << windowIndex
                                    << " frames=3 manifest=" << relativeManifest << std::endl;
                          if (presentation) {
                            view.presentation = std::move(presentation);
                            notify();
                          }
                          batch->terminal = true;
                          advanceAfterPresentation();
                        }
                        catch (const std::exception& error) {
                          failPresentation(error.what());
                        }
                      });
                  });
                }
                catch (...) {
                  settleRead(settled);
                  throw;
                }
                if (requestId.empty()) {
                  settleRead(settled);
                  throw std::runtime_error("source FRAME targeted request did not start");
                }
              }
            }
            catch (const std::exception& error) {
              failPresentation(error.what());
            }
            return;
          }
          activeRequestId.reset();
          advanceAfterPresentation();
        };
        callbacks.onResponse = [&, self = shared_from_this(), windowIndex, responseHandler] (
          const nsf::ResponseMessage& response) {
          const bool responseStatus = response.getStatus();
          const auto responsePayload = response.getPayload();
          if (fault == "user-cancel-terminal-response-race" ||
              fault == "user-cancel-terminal-response-cancel-wins-race") {
            const char* runRoot = std::getenv("NDNSF_SPEC191_RUN_ROOT");
            if (runRoot != nullptr && *runRoot != '\0') {
              const auto releasePath = std::filesystem::path(runRoot) /
                "private/spec210-terminal-response-race/release-response-callback";
              std::cerr << "SPEC210_TERMINAL_RESPONSE_CALLBACK_HELD window="
                        << windowIndex << std::endl;
              const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(20);
              while (!std::filesystem::exists(releasePath) &&
                     std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
            }
          }
          boost::asio::post(face.getIoContext(),
            [&, self = shared_from_this(), windowIndex, responseHandler, responseStatus, responsePayload] {
              if (finished) return;
              if (cancellationInProgress) {
                // The Face event loop serializes this callback with cancellation
                // completion. Retain a response whose pending call was already
                // removed so CancelCollaboration(false) cannot discard it.
                deferredCancellationResponse.emplace(responseStatus, responsePayload);
                std::cerr << "SPEC210_TERMINAL_RESPONSE_DEFERRED window="
                          << windowIndex << std::endl;
                return;
              }
              afterResultDrain(windowIndex, responseStatus, responsePayload, responseHandler);
            });
        };
        callbacks.onTimeout = [&, self = shared_from_this(), windowIndex] (const ndn::Name& requestId) {
          boost::asio::post(face.getIoContext(), [&, self = shared_from_this(), windowIndex, requestId] {
            if (finished || cancellationInProgress) return;
            std::cerr << "SPEC191_TRACKING_TIMEOUT window=" << windowIndex
                      << " request=" << requestId.toUri() << std::endl;
            publishPresentationFailure(windowIndex, "collaboration timeout");
            exitCode = 1;
            finish();
          });
        };
        callbacks.onFailure = [&, self = shared_from_this(), windowIndex] (const std::string& reason) {
          boost::asio::post(face.getIoContext(), [&, self = shared_from_this(), windowIndex, reason] {
            if (finished || cancellationInProgress) return;
            std::cerr << "SPEC191_TRACKING_PLAN_FAILED window=" << windowIndex
                      << " reason=" << reason << std::endl;
            publishPresentationFailure(windowIndex, reason);
            exitCode = 1;
            finish();
          });
        };
        return callbacks;
      };
      try {
        UavTrackingAckRequirements requirements;
        if (!compatibility) {
          for (const auto& role : {"Camera-UAV1", "Camera-UAV2", "Camera-UAV3", "Tracking-Compute"}) {
            requirements[role] = {
              {"tracking_mode", personExpectation ? "person" : "vehicle"},
              {"tracking_work_drained_schema", TRACKING_WORK_DRAINED_SCHEMA},
              {"source_epoch", std::to_string(windowGeneration.sourceEpoch)}};
          }
          for (size_t i = 0; i < sourceProviders.size(); ++i) {
            requirements["Camera-UAV" + std::to_string(i + 1)]["source_id"] =
              windowGeneration.sourceIds[i];
          }
          if (!calibrationProvider.empty()) {
            requirements["Calibration-Provider"] = {
              {"tracking_mode", personExpectation ? "person" : "vehicle"},
              {"tracking_work_drained_schema", TRACKING_WORK_DRAINED_SCHEMA},
              {"source_epoch", std::to_string(windowGeneration.sourceEpoch)}};
          }
          if (personExpectation) {
            requirements["Tracking-Compute"]["tracking_profile_digest"] =
              personExpectation->profile.profileDigest;
            requirements["Tracking-Compute"]["tracking_computation_digest"] =
              personExpectation->computationDigest;
          }
        }
        auto plan = uav::UavTrackingCoordinator::makePlan(
          sourceProviders, computeProvider, assignment, 1000, requestTimeoutMs,
          calibrationProvider, std::move(requirements));
        if (personExpectation) {
          uav::emitUavPersonTrace({{"runId", runId}, {"missionId", missionId},
            {"windowId", windowId}, {"sequence", windowIndex + 1},
            {"sourceEpoch", windowGeneration.sourceEpoch}}, "window-request");
        }
        const auto requestId = uav::UavTrackingCoordinator{}.begin(
          user, uav::SERVICE_TRACKING_WINDOW, request, std::move(plan),
          makeCallbacks());
        if (requestId.empty()) {
          throw std::runtime_error("BeginCollaboration returned empty request id");
        }
        remoteWorkMayExist = true;
        remoteWorkDrained = false;
        activeRequestId = requestId;
        scopeRequestId = requestId;
        std::cout << "SPEC191_TRACKING_GS_REQUEST request=" << requestId.toUri()
                  << " mission=" << missionId << " window=" << windowId << std::endl;
      }
      catch (const std::exception& error) {
        const std::string reason = error.what();
        if (reason.find("ControllerVersion is not ready") != std::string::npos &&
            elapsedMs < controllerReadyTimeoutMs) {
          std::cout << "SPEC191_TRACKING_WAIT_CONTROLLER_VERSION elapsed_ms="
                    << elapsedMs << " retry_ms=" << controllerReadyRetryMs << std::endl;
          scheduler.schedule(ndn::time::milliseconds(controllerReadyRetryMs), [beginWindow = this->beginWindow, windowIndex] {
            (*beginWindow)(windowIndex);
          });
          return;
        }
        fail(reason);
      }
    };
    scheduler.schedule(ndn::time::milliseconds(startDelayMs), [beginWindow = this->beginWindow] { (*beginWindow)(0); });
    pollAuthority();
  }
};

UavTrackingJob::UavTrackingJob(nsf::ServiceUser& user, ndn::Face& face,
                             AuthorityCheck checkAuthority, SnapshotHandler onSnapshot)
  : m_user(user)
  , m_face(face)
  , m_checkAuthority(std::move(checkAuthority))
  , m_onSnapshot(std::move(onSnapshot))
{
}

UavTrackingJob::~UavTrackingJob()
{
  // The application must drain while its borrowed Face and User are still alive.
  if (m_impl && !m_impl->drained) std::terminate();
}

TrackingAdmission
UavTrackingJob::start(const TrackingJobDefinition& definition, std::string* reason)
{
  if (m_impl && (!m_impl->drained || m_impl->view.cleanupState != "CLEAN")) {
    if (reason) *reason = "previous tracking job is active or cleanup is incomplete";
    return TrackingAdmission::BUSY;
  }
  if (m_impl && (definition.runId == m_impl->definition.runId ||
                 definition.jobId == m_impl->definition.jobId)) {
    if (reason) *reason = "a new job requires a new job and run identity";
    return TrackingAdmission::INVALID;
  }
  try {
    validateTrackingJobDefinition(definition);
    std::string authorityReason;
    if (!m_checkAuthority || !m_checkAuthority(definition, &authorityReason)) {
      if (reason) *reason = authorityReason.empty() ? "operator authority unavailable" : authorityReason;
      return TrackingAdmission::UNAUTHORIZED;
    }
    const auto inputs = validateTrackingJobInputs(definition);
    auto next = std::make_shared<Impl>(m_user, m_face, m_checkAuthority, m_onSnapshot);
    next->definition = definition;
    next->sourceProviders.clear();
    for (const auto& source : definition.sources) {
      next->sourceProviders.push_back(source.producerIdentity);
    }
    next->computeProvider = definition.computeIdentity;
    std::vector<std::string> arguments{"tracking-job", "--run-id", definition.runId,
      "--mission-id", "tracking-" + definition.runId,
      "--window-count", std::to_string(definition.windowCount),
      "--observation-schema", "uav-observation-v2", "--source-epoch",
      std::to_string(definition.sources[0].sourceEpoch), "--source-uav1-id",
      definition.sources[0].sourceId, "--source-uav2-id", definition.sources[1].sourceId,
      "--source-uav3-id", definition.sources[2].sourceId, "--source-generations",
      definition.sources[0].sourceManifest.path.string(), "--start-delay-ms", "0",
      "--request-timeout-ms", std::to_string(definition.requestTimeoutMs)};
    if (definition.mode == TrackingMode::PERSON) {
      arguments.insert(arguments.end(), {"--tracking-profile", definition.profile.path.string(),
        "--person-computation-manifest", definition.computationManifest.path.string(),
        "--person-presentation-dir", (definition.outputDirectory / "presentation").string()});
    }
    if (!definition.calibrationIdentity.empty()) {
      arguments.insert(arguments.end(), {"--calibration-provider", definition.calibrationIdentity.toUri()});
    }
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    next->configure(static_cast<int>(argv.size()), argv.data(), &inputs);
    if (definition.mode == TrackingMode::VEHICLE) {
      next->presentationRoot = definition.outputDirectory / "presentation";
      const person_json::Json context = {{"schema", "uav-tracking-display-context-v1"},
        {"runId", definition.runId}, {"missionId", "tracking-" + definition.runId},
        {"trackingMode", "vehicle"}, {"windowCount", definition.windowCount}};
      writePersonArtifact(next->presentationRoot / "context.json", context.dump());
    }
    next->view.jobId = definition.jobId;
    next->view.runId = definition.runId;
    next->view.missionId = definition.missionId;
    next->view.wireMissionId = next->missionId;
    next->view.mode = definition.mode;
    next->view.generation = ++m_generation;
    next->view.state = TrackingJobState::PREPARING;
    next->view.totalWindows = definition.windowCount;
    next->view.cleanupState = "NOT_STARTED";
    m_impl = next;
    next->begin();
    next->notify();
    return TrackingAdmission::ACCEPTED;
  }
  catch (const std::exception& error) {
    if (reason) *reason = error.what();
    return TrackingAdmission::INVALID;
  }
}

TrackingCancelResult
UavTrackingJob::requestCancel(const std::string& jobId)
{
  if (!m_impl || m_impl->view.jobId != jobId) return TrackingCancelResult::WRONG_JOB;
  if (m_impl->finished) return TrackingCancelResult::ALREADY_TERMINAL;
  if (m_impl->cancellationInProgress) return TrackingCancelResult::ALREADY_REQUESTED;
  std::string reason;
  if (!m_impl->compatibility &&
      (!m_checkAuthority || !m_checkAuthority(m_impl->definition, &reason))) {
    return TrackingCancelResult::UNAUTHORIZED;
  }
  m_impl->stopRequested = true;
  if (m_impl->activeRequestId) {
    m_impl->cancelActiveCollaboration(*m_impl->activeRequestId, m_impl->activeWindowIndex);
  }
  else {
    m_impl->cancellationInProgress = true;
    m_impl->exitCode = 1;
    m_impl->finish();
  }
  return TrackingCancelResult::REQUESTED;
}

TrackingSnapshot
UavTrackingJob::snapshot() const
{
  return m_impl ? m_impl->view : TrackingSnapshot{};
}

bool
UavTrackingJob::isDrained() const
{
  return !m_impl || m_impl->drained;
}

void
UavTrackingJob::stop(std::function<void(const TrackingCleanupResult&)> onDrained)
{
  if (!m_impl || m_impl->drained || m_impl->drainReported) {
    const auto view = snapshot();
    if (onDrained) onDrained({view.jobId, view.cancelState, view.cleanupState, view.reason});
    return;
  }
  if (m_impl->onDrained) throw std::logic_error("tracking stop already pending");
  m_impl->onDrained = std::move(onDrained);
  if (m_impl->finished || m_impl->cancellationInProgress) return;
  m_impl->stopRequested = true;
  if (m_impl->activeRequestId) {
    m_impl->cancelActiveCollaboration(*m_impl->activeRequestId, m_impl->activeWindowIndex);
  }
  else {
    m_impl->cancellationInProgress = true;
    m_impl->exitCode = 1;
    m_impl->finish();
  }
}

int
runTrackingGroundStationCli(int argc, char** argv)
{
  const ndn::Name userIdentity(option(argc, argv, "--user-identity", "/example/uav/gs"));
  const ndn::Name controllerPrefix(option(argc, argv, "--controller-prefix", "/example/uav/controller"));
  const ndn::Name groupPrefix(option(argc, argv, "--group-prefix", "/example/uav"));
  const auto trustSchema = option(argc, argv, "--trust-schema", "examples/trust-any.conf");
  ndn::Face face;
  ndn::KeyChain keyChain;
  const auto userCert = getOrCreateIdentity(keyChain, userIdentity);
  const auto controllerCert = getOrCreateIdentity(keyChain, controllerPrefix);
  keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(userIdentity));
  nsf::CertificatePublisher certPublisher(face, keyChain, userCert.getName());
  nsf::ServiceUser user(face, groupPrefix, userCert, controllerCert, trustSchema);
  user.init();
  user.setUseTokens(true);
  nsf::ServiceUser::AdaptiveAdmissionOptions admission;
  admission.enabled = false;
  user.setAdaptiveAdmissionControl(admission);
  user.fetchPermissionsFromController(controllerPrefix);
  UavTrackingJob job(user, face, {}, {});
  auto impl = std::make_shared<UavTrackingJob::Impl>(user, face,
    UavTrackingJob::AuthorityCheck{}, UavTrackingJob::SnapshotHandler{});
  impl->compatibility = true;
  impl->configure(argc, argv);
  impl->sourceProviders = {uav::droneIdentity("UAV1"), uav::droneIdentity("UAV2"),
                           uav::droneIdentity("UAV3")};
  impl->computeProvider = ndn::Name("/example/uav/compute");
  impl->view.jobId = impl->runId;
  impl->view.runId = impl->runId;
  impl->view.missionId = impl->missionId;
  impl->view.wireMissionId = impl->missionId;
  impl->view.totalWindows = impl->windowCount;
  impl->view.state = TrackingJobState::PREPARING;
  job.m_impl = impl;
  impl->begin();
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::signal(SIGUSR1, onCancelSignal);
  if (impl->fault == "alter-committed-window") std::signal(SIGUSR2, onReplaySignal);
  std::cout << "SPEC191_TRACKING_GS_READY identity=" << userIdentity.toUri()
            << " service=" << uav::SERVICE_TRACKING_WINDOW.toUri() << std::endl;
  while (!impl->drained) {
    try {
      face.processEvents(ndn::time::milliseconds(100));
    }
    catch (const std::exception& error) {
      impl->view.reason = error.what();
      impl->exitCode = 1;
      impl->finish();
    }
    face.getIoContext().restart();
    if (impl->replayWaiting) {
      if (g_replay_requested.exchange(false)) {
        impl->replayWaiting = false;
        impl->scheduler.schedule(ndn::time::milliseconds(0), [impl] { (*impl->beginWindow)(0); });
      }
      else if (std::chrono::steady_clock::now() >= impl->replayDeadline) {
        impl->publishPresentationFailure(0, "committed window replay observer deadline expired");
        impl->exitCode = 1;
        impl->finish();
      }
    }
    if (g_cancel_requested.exchange(false)) job.requestCancel(impl->view.jobId);
  }
  std::cout << "SPEC191_TRACKING_GS_EXIT status=" << impl->exitCode << std::endl;
  return impl->exitCode;
}

} // namespace ndnsf::examples::uav
