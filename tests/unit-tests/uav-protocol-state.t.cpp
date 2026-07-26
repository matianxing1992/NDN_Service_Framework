#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/shared/UavProtocol.hpp"
#include "NDNSF-UAV-APP/shared/UavVideoPipeline.hpp"
#include "NDNSF-UAV-APP/shared/UavSensorStreams.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <openssl/sha.h>
#include <set>

namespace ndn_service_framework::test {
namespace {

using ndnsf::examples::uav::FlightSafetyGateState;
using ndnsf::examples::uav::FlightActionControlState;
using ndnsf::examples::uav::FlightCommandState;
using ndnsf::examples::uav::AutoControlSequenceStep;
using ndnsf::examples::uav::DroneListRowState;
using ndnsf::examples::uav::Fields;
using ndnsf::examples::uav::MissionControlState;
using ndnsf::examples::uav::MissionPlanDocument;
using ndnsf::examples::uav::MissionStartGateState;
using ndnsf::examples::uav::MissionPart;
using ndnsf::examples::uav::MissionPlan;
using ndnsf::examples::uav::MissionProgressState;
using ndnsf::examples::uav::MissionState;
using ndnsf::examples::uav::MissionWaypoint;
using ndnsf::examples::uav::MavlinkMessageSummary;
using ndnsf::examples::uav::PreflightCheckItem;
using ndnsf::examples::uav::ReadinessState;
using ndnsf::examples::uav::RecordingDataProductState;
using ndnsf::examples::uav::OperatorAuthorityLease;
using ndnsf::examples::uav::OperatorAuthorityLeaseRequest;
using ndnsf::examples::uav::SafetyState;
using ndnsf::examples::uav::SelectedActionState;
using ndnsf::examples::uav::SelectedDroneSummaryState;
using ndnsf::examples::uav::TelemetryState;
using ndnsf::examples::uav::UavDataProductCatalogState;
using ndnsf::examples::uav::UavFunctionalityState;
using ndnsf::examples::uav::UavAnalyzeSnapshot;
using ndnsf::examples::uav::UavOperatorDashboardSnapshot;
using ndnsf::examples::uav::UavPracticalityState;
using ndnsf::examples::uav::UavStabilityState;
using ndnsf::examples::uav::UavVideoAad;
using ndnsf::examples::uav::UavVideoNonceUseGuard;
using ndnsf::examples::uav::CanonicalVideoRecordingManifest;
using ndnsf::examples::uav::RetainedVideoPacketReference;
using ndnsf::examples::uav::UavVideoContentKeyGrant;
using ndnsf::examples::uav::VehicleParameterEditRequest;
using ndnsf::examples::uav::VehicleParameterEditResult;
using ndnsf::examples::uav::VehicleParameterSnapshot;
using ndnsf::examples::uav::VideoAdaptiveState;
using ndnsf::examples::uav::VideoCoreFetchDecisionSnapshot;
using ndnsf::examples::uav::VideoAdaptivePolicyInput;
using ndnsf::examples::uav::VideoControlState;
using ndnsf::examples::uav::VideoPacket;
using ndnsf::examples::uav::UavH264ReadinessTracker;
using ndnsf::examples::uav::VideoState;
using ndnsf::examples::uav::BoundedLatestFrameQueue;
using ndnsf::examples::uav::LegacyPipeVideoPipeline;
using ndnsf::examples::uav::GStreamerVideoPipeline;
using ndnsf::examples::uav::UavVideoCaptureConfig;
using ndnsf::examples::uav::UavVideoFrame;
using ndnsf::examples::uav::UavVideoPipelineState;
using ndnsf::examples::uav::UavVideoSampleClassMode;
using ndnsf::examples::uav::UavVideoSampleClassSchedule;
using ndnsf::examples::uav::CompactTelemetrySample;
using ndnsf::examples::uav::LatestTelemetryAdmission;
using ndnsf::examples::uav::OpaqueAcousticSource;
using ndnsf::examples::uav::CompleteAcousticBlockAdmission;
using ndnsf::examples::uav::acousticSourceCountClass;
using ndnsf::examples::uav::buildPatrolMissionPlan;
using ndnsf::examples::uav::computeVideoAdaptivePolicy;
using ndnsf::examples::uav::computeLiveVideoRetentionItems;
using ndnsf::examples::uav::decodeVideoPacket;
using ndnsf::examples::uav::decodeVideoStreamDescriptorStrict;
using ndnsf::examples::uav::decodeUavVideoEnvelopeStrict;
using ndnsf::examples::uav::deriveUavVideoNonce;
using ndnsf::examples::uav::encodeFields;
using ndnsf::examples::uav::encodeVideoPacket;
using ndnsf::examples::uav::encodeVideoStreamDescriptor;
using ndnsf::examples::uav::loadMissionPlanDocument;
using ndnsf::examples::uav::makeVideoStartFields;
using ndnsf::examples::uav::makeUavVideoDataName;
using ndnsf::examples::uav::makeUavStreamNameMapCheckpoint;
using ndnsf::examples::uav::makeUavStreamNameMapResolverConfig;
using ndnsf::examples::uav::protectUavVideoPacket;
using ndnsf::examples::uav::parseVideoFecParityShards;
using ndnsf::examples::uav::saveMissionPlanDocument;
using ndnsf::examples::uav::sourceMediaSequenceForJoinCursor;
using ndnsf::examples::uav::streamChunkToVideoPacket;
using ndnsf::examples::uav::toCoreLiveStreamDescriptor;
using ndnsf::examples::uav::toServiceOperationStatus;
using ndnsf::examples::uav::unprotectUavVideoPacket;
using ndnsf::examples::uav::videoPacketToStreamChunk;
using ndnsf::examples::uav::applyCoreLiveStreamDescriptor;
using ndnsf::examples::uav::applyCoreLiveStreamStatus;

BOOST_AUTO_TEST_CASE(LiveVideoRetentionUsesDurationAndRemainsBounded)
{
  BOOST_CHECK_EQUAL(computeLiveVideoRetentionItems(30, 12, 1), 3900);
  BOOST_CHECK_EQUAL(computeLiveVideoRetentionItems(30, 12, 0, 1500), 540);
  BOOST_CHECK_EQUAL(computeLiveVideoRetentionItems(1000000, 1000000, 1),
                    ndnsf::examples::uav::UAV_VIDEO_MAX_RETAINED_ITEMS);
  BOOST_CHECK_THROW(computeLiveVideoRetentionItems(0, 12, 1),
                    std::invalid_argument);
  BOOST_CHECK_THROW(computeLiveVideoRetentionItems(30, 0, 1),
                    std::invalid_argument);
}

ReadinessState
makeReadyState(bool armed)
{
  ReadinessState readiness;
  readiness.droneId = "A";
  readiness.heartbeatSeen = "true";
  readiness.flightControllerReady = "true";
  readiness.gpsReady = "true";
  readiness.ekfReady = "true";
  readiness.batteryReady = "true";
  readiness.armed = armed ? "true" : "false";
  readiness.readiness = "ready";
  readiness.readinessReason = "ok";
  readiness.mode = armed ? "GUIDED" : "STANDBY";
  readiness.landedStateName = "on-ground";
  return readiness;
}

SafetyState
makeSafeState()
{
  SafetyState safety;
  safety.droneId = "A";
  safety.linkState = "connected";
  safety.manualControlState = "idle";
  safety.manualNeutralSent = "true";
  safety.detail = "ok";
  return safety;
}

BOOST_AUTO_TEST_CASE(UavVideoFecParityRequestContract)
{
  const auto disabled = makeVideoStartFields(30, 1200, 320, 0);
  BOOST_CHECK_EQUAL(disabled.at("type"), "video-control");
  BOOST_CHECK_EQUAL(disabled.at("action"), "start");
  BOOST_CHECK_EQUAL(disabled.at("fec_parity_shards"), "0");
  BOOST_CHECK_EQUAL(parseVideoFecParityShards(disabled), 0);

  const auto enabled = makeVideoStartFields(30, 1200, 320, 1);
  BOOST_CHECK_EQUAL(enabled.at("fec_parity_shards"), "1");
  BOOST_CHECK_EQUAL(parseVideoFecParityShards(enabled), 1);
  BOOST_CHECK_EQUAL(parseVideoFecParityShards(Fields{}), 1);

  BOOST_CHECK_THROW(makeVideoStartFields(30, 1200, 320, 2), std::invalid_argument);
  BOOST_CHECK_THROW(
    parseVideoFecParityShards({{"fec_parity_shards", "2"}}),
    std::invalid_argument);
  BOOST_CHECK_THROW(
    parseVideoFecParityShards({{"fec_parity_shards", "not-a-number"}}),
    std::invalid_argument);
}

MissionState
makeMissionState(const std::string& phase)
{
  MissionState mission;
  mission.droneId = "A";
  mission.missionId = "mission-test";
  mission.partId = "part-A";
  mission.phase = phase;
  mission.detail = "test";
  return mission;
}

BOOST_AUTO_TEST_SUITE(UavProtocolState)

BOOST_AUTO_TEST_CASE(UavVideoLegacyPipelineIsBoundedAndByteEquivalent)
{
  LegacyPipeVideoPipeline pipeline(true);
  std::vector<uint8_t> observed;
  pipeline.startDecode([&] (const UavVideoFrame& frame) {
    observed = frame.bytes;
  });
  UavVideoFrame encoded;
  encoded.sourceFrameId = 7;
  encoded.captureOriginNs = 1000;
  encoded.codecPts = 7;
  encoded.bytes = {0x00, 0x00, 0x01, 0x65, 0xff, 0x00};
  BOOST_CHECK(pipeline.submitAccessUnit(encoded));
  BOOST_CHECK(observed == encoded.bytes);
  BOOST_CHECK(pipeline.isHeadless());
  BOOST_CHECK(pipeline.state() == UavVideoPipelineState::Running);
  pipeline.stop();
  pipeline.stop();
  BOOST_CHECK(pipeline.state() == UavVideoPipelineState::Stopped);
  BOOST_CHECK(!pipeline.submitAccessUnit(encoded));

  BoundedLatestFrameQueue queue(2, 12);
  BOOST_CHECK(queue.push(encoded));
  encoded.sourceFrameId = 8;
  BOOST_CHECK(queue.push(encoded));
  encoded.sourceFrameId = 9;
  BOOST_CHECK(queue.push(encoded));
  const auto snapshot = queue.snapshot();
  BOOST_CHECK_EQUAL(snapshot.queuedFrames, 2);
  BOOST_CHECK_EQUAL(snapshot.queuedBytes, 12);
  BOOST_CHECK_EQUAL(snapshot.droppedFrames, 1);
  BOOST_CHECK_EQUAL(snapshot.lastDropReason, "superseded-by-newer-frame");
  const auto latest = queue.popLatest();
  BOOST_REQUIRE(latest.has_value());
  BOOST_CHECK_EQUAL(latest->sourceFrameId, 9);
  BOOST_CHECK_EQUAL(queue.snapshot().queuedFrames, 0);
}

BOOST_AUTO_TEST_CASE(UavVideoSampleClassScheduleIsBackendTruthfulAtSupportedFps)
{
  for (const uint32_t fps : {20u, 30u, 60u}) {
    const auto exact = UavVideoSampleClassSchedule::exactKeyDelta(
      fps, 12, 17);
    BOOST_CHECK(exact.mode() == UavVideoSampleClassMode::ExactKeyDelta);
    BOOST_CHECK_EQUAL(exact.fps(), fps);
    BOOST_CHECK_EQUAL(exact.hardMaxSources(), 12);
    BOOST_CHECK_EQUAL(exact.sessionGeneration(), 17);
    BOOST_CHECK_EQUAL(exact.classFor(0), "key");
    BOOST_CHECK_EQUAL(exact.classFor(1), "delta");
    BOOST_CHECK_EQUAL(exact.classFor(fps - 1), "delta");
    BOOST_CHECK_EQUAL(exact.classFor(fps), "key");
    BOOST_CHECK(exact.matchesActual(0, true));
    BOOST_CHECK(!exact.matchesActual(0, false));
    BOOST_CHECK(exact.matchesActual(1, false));

    const auto legacy = UavVideoSampleClassSchedule::boundedOpaque(
      fps, 12, 18);
    BOOST_CHECK(legacy.mode() == UavVideoSampleClassMode::BoundedOpaque);
    BOOST_CHECK_EQUAL(legacy.fps(), fps);
    BOOST_CHECK_EQUAL(legacy.hardMaxSources(), 12);
    BOOST_CHECK_EQUAL(legacy.sessionGeneration(), 18);
    BOOST_CHECK_EQUAL(legacy.classFor(0), "opaque");
    BOOST_CHECK_EQUAL(legacy.classFor(fps), "opaque");
    BOOST_CHECK(!legacy.hasExactFrameClass());
  }

  BOOST_CHECK_THROW(
    UavVideoSampleClassSchedule::exactKeyDelta(0, 12, 1),
    std::invalid_argument);
  BOOST_CHECK_THROW(
    UavVideoSampleClassSchedule::exactKeyDelta(61, 12, 1),
    std::invalid_argument);
  BOOST_CHECK_THROW(
    UavVideoSampleClassSchedule::boundedOpaque(20, 0, 1),
    std::invalid_argument);
  BOOST_CHECK_THROW(
    UavVideoSampleClassSchedule::boundedOpaque(20, 12, 0),
    std::invalid_argument);

  const auto beforeRestart =
    UavVideoSampleClassSchedule::exactKeyDelta(20, 12, 41);
  const auto afterRestart =
    UavVideoSampleClassSchedule::exactKeyDelta(20, 12, 42);
  BOOST_CHECK_NE(beforeRestart.sessionGeneration(),
                 afterRestart.sessionGeneration());
  BOOST_CHECK_EQUAL(afterRestart.classFor(0), "key");
}

BOOST_AUTO_TEST_CASE(UavVideoFrameBindingIsVersionedAndAuthenticated)
{
  VideoPacket packet;
  packet.streamId = "stream-00112233445566778899aabbccddeeff";
  packet.streamSessionEpoch = 17;
  packet.packetSeq = 4;
  packet.frameSeq = 2;
  packet.frameBindingVersion = 1;
  packet.sourceFrameId = 42;
  packet.captureOriginNs = 123456789;
  packet.captureClockId = "provider-monotonic";
  packet.codecPts = 9000;
  packet.codecTimeBaseNum = 1;
  packet.codecTimeBaseDen = 90000;
  packet.codecConfigEpoch = 3;
  packet.frameSegmentIndex = 0;
  packet.frameSegmentCount = 1;
  packet.payload = {0x00, 0x00, 0x01, 0x65};

  const auto decoded = decodeVideoPacket(encodeVideoPacket(packet));
  BOOST_CHECK_EQUAL(decoded.frameBindingVersion, 1);
  BOOST_CHECK_EQUAL(decoded.sourceFrameId, 42);
  BOOST_CHECK_EQUAL(decoded.captureOriginNs, 123456789);
  BOOST_CHECK_EQUAL(decoded.captureClockId, "provider-monotonic");
  BOOST_CHECK_EQUAL(decoded.codecPts, 9000);
  BOOST_CHECK_EQUAL(decoded.codecTimeBaseNum, 1);
  BOOST_CHECK_EQUAL(decoded.codecTimeBaseDen, 90000);
  BOOST_CHECK_EQUAL(decoded.codecConfigEpoch, 3);
  BOOST_CHECK_NO_THROW(validateVideoFrameBinding(decoded, 17));

  auto stale = decoded;
  stale.streamSessionEpoch = 16;
  BOOST_CHECK_THROW(validateVideoFrameBinding(stale, 17), std::invalid_argument);
  auto malformed = decoded;
  malformed.codecTimeBaseDen = 0;
  BOOST_CHECK_THROW(validateVideoFrameBinding(malformed, 17), std::invalid_argument);
  auto legacy = decoded;
  legacy.frameBindingVersion = 0;
  BOOST_CHECK(!hasExactVideoFrameBinding(legacy));
}

BOOST_AUTO_TEST_CASE(UavGStreamerPipelinePreservesSourceIdentityThroughDecode)
{
  GStreamerVideoPipeline capture;
  if (!capture.probeCapabilities().available) {
    BOOST_TEST_MESSAGE("GStreamer capability unavailable; covered by capability gate");
    return;
  }
  std::mutex mutex;
  std::condition_variable ready;
  std::vector<UavVideoFrame> encoded;
  UavVideoCaptureConfig config;
  config.source = "videotestsrc";
  config.width = 160;
  config.height = 120;
  config.fps = 30;
  capture.startCapture(config, [&] (const UavVideoFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    if (encoded.size() < 12) encoded.push_back(frame);
    ready.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(ready.wait_for(lock, std::chrono::seconds(3), [&] {
      return encoded.size() == 12;
    }));
  }
  capture.stop();
  BOOST_CHECK_EQUAL(encoded[1].sourceFrameId, encoded[0].sourceFrameId + 1);
  BOOST_CHECK(encoded[1].captureOriginNs > encoded[0].captureOriginNs);

  GStreamerVideoPipeline decoder;
  std::vector<UavVideoFrame> decoded;
  decoder.startDecode([&] (const UavVideoFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    decoded.push_back(frame);
    ready.notify_all();
  });
  for (auto& frame : encoded) {
    frame.sessionEpoch = 17;
    BOOST_REQUIRE(decoder.submitAccessUnit(frame));
  }
  auto conflictingPts = encoded.back();
  conflictingPts.sourceFrameId += 1000;
  BOOST_CHECK(!decoder.submitAccessUnit(conflictingPts));
  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(ready.wait_for(lock, std::chrono::seconds(3), [&] {
      return decoded.size() >= 8;
    }));
  }
  decoder.stop();
  for (const auto& output : decoded) {
    const auto input = std::find_if(encoded.begin(), encoded.end(), [&] (const auto& frame) {
      return frame.sourceFrameId == output.sourceFrameId;
    });
    BOOST_REQUIRE(input != encoded.end());
    BOOST_CHECK_EQUAL(output.captureOriginNs, input->captureOriginNs);
    BOOST_CHECK_EQUAL(output.codecPts, input->codecPts);
    BOOST_CHECK(!output.bytes.empty());
  }
}

BOOST_AUTO_TEST_CASE(UavGStreamerFileCaptureHonorsWallClockFrameRate)
{
  GStreamerVideoPipeline capabilityProbe;
  if (!capabilityProbe.probeCapabilities().available) {
    BOOST_TEST_MESSAGE("GStreamer capability unavailable; covered by capability gate");
    return;
  }

  const std::string source = "NDNSF-UAV-APP/videos/drone.mp4";
  for (const uint32_t fps : {10U, 60U}) {
    GStreamerVideoPipeline capture;
    std::atomic<uint64_t> callbacks{0};
    std::mutex lagMutex;
    std::vector<double> callbackLagMs;
    UavVideoCaptureConfig config;
    config.source = source;
    config.width = 160;
    config.height = 120;
    config.fps = fps;
    config.bitrateKbps = 600;
    config.keyFrameInterval = fps;

    const auto started = std::chrono::steady_clock::now();
    capture.startCapture(config, [&] (const UavVideoFrame& frame) {
      ++callbacks;
      const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
      std::lock_guard<std::mutex> lock(lagMutex);
      callbackLagMs.push_back(
        static_cast<double>(nowNs - frame.captureOriginNs) / 1'000'000.0);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    capture.stop();
    const auto seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - started).count();
    const auto achievedFps = callbacks.load() / seconds;
    std::sort(callbackLagMs.begin(), callbackLagMs.end());
    BOOST_REQUIRE(!callbackLagMs.empty());
    const auto medianCallbackLagMs =
      callbackLagMs[callbackLagMs.size() / 2];
    const auto maximumAcceptedLagMs =
      std::max(50.0, 1000.0 / static_cast<double>(fps));

    BOOST_TEST_CONTEXT("configured fps=" << fps
                       << " callbacks=" << callbacks.load()
                       << " seconds=" << seconds
                       << " achieved_fps=" << achievedFps
                       << " median_callback_lag_ms=" << medianCallbackLagMs) {
      BOOST_CHECK_GE(achievedFps, static_cast<double>(fps) * 0.75);
      BOOST_CHECK_LE(achievedFps, static_cast<double>(fps) * 1.25);
      BOOST_CHECK_LE(medianCallbackLagMs, maximumAcceptedLagMs);
    }
  }
}

BOOST_AUTO_TEST_CASE(UavGStreamerLowRateDecodeDoesNotBufferFourFrames)
{
  GStreamerVideoPipeline capture;
  if (!capture.probeCapabilities().available) {
    BOOST_TEST_MESSAGE("GStreamer capability unavailable; covered by capability gate");
    return;
  }

  GStreamerVideoPipeline decoder;
  std::mutex mutex;
  std::condition_variable ready;
  std::vector<double> decodeLagMs;
  decoder.startDecode([&] (const UavVideoFrame& frame) {
    const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(mutex);
    decodeLagMs.push_back(
      static_cast<double>(nowNs - frame.captureOriginNs) / 1'000'000.0);
    ready.notify_all();
  });

  UavVideoCaptureConfig config;
  config.source = "NDNSF-UAV-APP/videos/drone.mp4";
  config.width = 160;
  config.height = 120;
  config.fps = 10;
  config.bitrateKbps = 600;
  config.keyFrameInterval = 10;
  capture.startCapture(config, [&] (const UavVideoFrame& frame) {
    decoder.submitAccessUnit(frame);
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(ready.wait_for(lock, std::chrono::seconds(6), [&] {
      return decodeLagMs.size() >= 20;
    }));
  }
  capture.stop();
  decoder.stop();
  std::sort(decodeLagMs.begin(), decodeLagMs.end());
  const auto medianDecodeLagMs = decodeLagMs[decodeLagMs.size() / 2];
  BOOST_TEST_CONTEXT("decoded=" << decodeLagMs.size()
                     << " median_decode_lag_ms=" << medianDecodeLagMs) {
    BOOST_CHECK_LE(medianDecodeLagMs, 200.0);
  }
}

BOOST_AUTO_TEST_CASE(UavGStreamerCaptureCallbackExceptionsFailClosed)
{
  GStreamerVideoPipeline capabilityProbe;
  if (!capabilityProbe.probeCapabilities().available) {
    BOOST_TEST_MESSAGE("GStreamer capability unavailable; covered by capability gate");
    return;
  }

  UavVideoCaptureConfig config;
  config.source = "videotestsrc";
  config.width = 160;
  config.height = 120;
  config.fps = 20;
  config.keyFrameInterval = 20;

  for (const bool nonStandard : {false, true}) {
    GStreamerVideoPipeline pipeline;
    std::atomic<uint64_t> callbacks{0};
    pipeline.startCapture(config, [&] (const UavVideoFrame&) {
      ++callbacks;
      if (nonStandard) {
        throw 145;
      }
      throw std::runtime_error("injected-capture-failure");
    });
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (pipeline.state() != UavVideoPipelineState::Failed &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_REQUIRE(pipeline.state() == UavVideoPipelineState::Failed);
    const auto failure = pipeline.failure();
    BOOST_REQUIRE(failure.has_value());
    BOOST_CHECK_EQUAL(failure->direction, "capture");
    BOOST_CHECK_EQUAL(
      failure->code,
      nonStandard ? "capture-callback-nonstandard-exception" :
                    "capture-callback-exception");
    BOOST_CHECK(failure->reason.size() <= 256);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BOOST_CHECK_EQUAL(callbacks.load(), 1);
    pipeline.stop();
    pipeline.stop();
    BOOST_CHECK(pipeline.state() == UavVideoPipelineState::Stopped);
  }
}

BOOST_AUTO_TEST_CASE(UavGStreamerDecodeCallbackExceptionsFailClosed)
{
  GStreamerVideoPipeline capture;
  if (!capture.probeCapabilities().available) {
    BOOST_TEST_MESSAGE("GStreamer capability unavailable; covered by capability gate");
    return;
  }

  std::mutex mutex;
  std::condition_variable ready;
  std::vector<UavVideoFrame> encoded;
  UavVideoCaptureConfig config;
  config.source = "videotestsrc";
  config.width = 160;
  config.height = 120;
  config.fps = 20;
  config.keyFrameInterval = 20;
  capture.startCapture(config, [&] (const UavVideoFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    if (encoded.size() < 12) encoded.push_back(frame);
    ready.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    BOOST_REQUIRE(ready.wait_for(lock, std::chrono::seconds(3), [&] {
      return encoded.size() == 12;
    }));
  }
  capture.stop();
  BOOST_REQUIRE_EQUAL(encoded.size(), 12);
  BOOST_REQUIRE(encoded.front().keyFrame);

  for (const bool nonStandard : {false, true}) {
    GStreamerVideoPipeline decoder;
    std::atomic<uint64_t> callbacks{0};
    decoder.startDecode([&] (const UavVideoFrame&) {
      ++callbacks;
      if (nonStandard) {
        throw 145;
      }
      throw std::runtime_error("injected-decode-failure");
    });
    for (const auto& frame : encoded) {
      if (!decoder.submitAccessUnit(frame)) {
        break;
      }
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (decoder.state() != UavVideoPipelineState::Failed &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_REQUIRE(decoder.state() == UavVideoPipelineState::Failed);
    const auto failure = decoder.failure();
    BOOST_REQUIRE(failure.has_value());
    BOOST_CHECK_EQUAL(failure->direction, "decode");
    BOOST_CHECK_EQUAL(
      failure->code,
      nonStandard ? "decode-callback-nonstandard-exception" :
                    "decode-callback-exception");
    BOOST_CHECK(failure->reason.size() <= 256);
    BOOST_CHECK_EQUAL(callbacks.load(), 1);
    BOOST_CHECK(!decoder.submitAccessUnit(encoded.back()));
    decoder.stop();
    decoder.stop();
    BOOST_CHECK(decoder.state() == UavVideoPipelineState::Stopped);
  }
}

BOOST_AUTO_TEST_CASE(PublicationJoinCursorMapsToSourceOnlyMediaSequence)
{
  BOOST_CHECK_EQUAL(sourceMediaSequenceForJoinCursor(0, 4, 1), 0);
  BOOST_CHECK_EQUAL(sourceMediaSequenceForJoinCursor(3, 4, 1), 3);
  BOOST_CHECK_EQUAL(sourceMediaSequenceForJoinCursor(4, 4, 1), 4);
  BOOST_CHECK_EQUAL(sourceMediaSequenceForJoinCursor(5, 4, 1), 4);
  BOOST_CHECK_EQUAL(sourceMediaSequenceForJoinCursor(6, 4, 1), 5);
  BOOST_CHECK_EQUAL(sourceMediaSequenceForJoinCursor(10, 4, 1), 8);
  BOOST_CHECK_THROW(sourceMediaSequenceForJoinCursor(0, 0, 1),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(UavStreamExplicitNonceAesGcmGoldenVector)
{
  ndn::Buffer key(32, 0);
  ndn::Buffer nonce(12, 0);
  const ndn::Buffer plaintext(16, 0);

  const auto encrypted = hybridAesGcmEncryptWithNonce(
    key,
    ndn::span<const uint8_t>(nonce.data(), nonce.size()),
    ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
    {});

  BOOST_CHECK(encrypted.nonce == nonce);
  BOOST_CHECK_EQUAL(ndn::toHex(encrypted.ciphertext),
                    "CEA7403D4D606B6E074EC5D3BAF39D18");
  BOOST_CHECK_EQUAL(ndn::toHex(encrypted.tag),
                    "D0D1C8A799996BF0265B98B5D48AB919");

  ndn::Buffer shortNonce(11, 0);
  BOOST_CHECK_THROW(
    hybridAesGcmEncryptWithNonce(
      key,
      ndn::span<const uint8_t>(shortNonce.data(), shortNonce.size()),
      ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
      {}),
    std::invalid_argument);
}

Fields
makeUavStreamDescriptorFields()
{
  const std::string streamId = "stream-00112233445566778899aabbccddeeff";
  ndn::Name dataPrefix("/uav/7/video/front");
  dataPrefix.append(streamId).appendVersion(23);
  return {
    {"data_prefix", dataPrefix.toUri()},
    {"fec_data_shards", "4"},
    {"fec_parity_shards", "1"},
    {"key_epoch", "3"},
    {"latest_join_cursor", "4"},
    {"latest_join_media_sequence", "3"},
    {"latest_produced_cursor", "5"},
    {"mapping_anchor_block", "1"},
    {"mapping_anchor_content_digest", std::string(64, '0')},
    {"mapping_block_capacity", "4"},
    {"mapping_committed_through_cursor", "7"},
    {"mapping_root", "/uav/7/NDNSF/STREAM-MAP/" + streamId},
    {"mapping_version", "23"},
    {"max_name_reservations", "65536"},
    {"next_reserved_cursor", "8"},
    {"nonce_salt_hex", "a1b2c3d4"},
    {"oldest_retained_cursor", "0"},
    {"prefetch_eligibility", "ahead-mapped"},
    {"sample_period_ms", "33"},
    {"sample_class_key_seed", "4"},
    {"sample_class_delta_seed", "2"},
    {"sample_unit", "fec-group"},
    {"stream_cipher", "aes-256-gcm"},
    {"stream_contract_version", "2"},
    {"stream_id", streamId},
    {"stream_key_hex",
     "000102030405060708090a0b0c0d0e0f"
     "101112131415161718191a1b1c1d1e1f"},
    {"stream_session_epoch", "17"},
  };
}

ndnsf::examples::uav::VideoStreamDescriptor
makeUavStreamDescriptor()
{
  return decodeVideoStreamDescriptorStrict(
    encodeFields(makeUavStreamDescriptorFields()),
    ndn::Name("/uav/7"), ndn::Name("/uav/7"),
    ndn::Name("/UAV/Camera/Video"), ndn::Name("/UAV/Camera/Video"));
}

std::string
blockHex(const ndn::Block& block)
{
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string result;
  result.reserve(block.size() * 2);
  for (const auto byte : block) {
    result.push_back(DIGITS[byte >> 4]);
    result.push_back(DIGITS[byte & 0x0f]);
  }
  return result;
}

std::string
blockSha256Hex(const ndn::Block& block)
{
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  const auto* wire = block.size() == 0 ? nullptr : std::addressof(*block.begin());
  SHA256(wire, block.size(), digest.data());
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    result.push_back(DIGITS[byte >> 4]);
    result.push_back(DIGITS[byte & 0x0f]);
  }
  return result;
}

ndn::Buffer
blockBuffer(const ndn::Block& block)
{
  return ndn::Buffer(block.begin(), block.end());
}

BOOST_AUTO_TEST_CASE(UavStreamDescriptorStrictContract)
{
  const auto fields = makeUavStreamDescriptorFields();
  const auto wire = encodeFields(fields);
  BOOST_CHECK_EQUAL(wire,
    "data_prefix=/uav/7/video/front/stream-00112233445566778899aabbccddeeff/v%3D23;"
    "fec_data_shards=4;fec_parity_shards=1;key_epoch=3;latest_join_cursor=4;"
    "latest_join_media_sequence=3;latest_produced_cursor=5;mapping_anchor_block=1;"
    "mapping_anchor_content_digest=0000000000000000000000000000000000000000000000000000000000000000;"
    "mapping_block_capacity=4;mapping_committed_through_cursor=7;"
    "mapping_root=/uav/7/NDNSF/STREAM-MAP/stream-00112233445566778899aabbccddeeff;"
    "mapping_version=23;max_name_reservations=65536;next_reserved_cursor=8;"
    "nonce_salt_hex=a1b2c3d4;"
    "oldest_retained_cursor=0;prefetch_eligibility=ahead-mapped;"
    "sample_class_delta_seed=2;sample_class_key_seed=4;sample_period_ms=33;"
    "sample_unit=fec-group;stream_cipher=aes-256-gcm;stream_contract_version=2;"
    "stream_id=stream-00112233445566778899aabbccddeeff;"
    "stream_key_hex=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f;"
    "stream_session_epoch=17");
  const ndn::Name provider("/uav/7");
  const ndn::Name service("/UAV/Camera/Video");

  const auto descriptor = decodeVideoStreamDescriptorStrict(
    wire, provider, provider, service, service);
  BOOST_CHECK_EQUAL(descriptor.streamId,
                    "stream-00112233445566778899aabbccddeeff");
  BOOST_CHECK_EQUAL(descriptor.mappingVersion, 23);
  BOOST_CHECK_EQUAL(descriptor.mappingRoot,
                    ndn_service_framework::makeStreamNameMapRoot(
                      provider, descriptor.streamId));
  BOOST_CHECK(descriptor.dataPrefix[-1].isVersion());
  BOOST_CHECK_EQUAL(descriptor.dataPrefix[-1].toVersion(), 23);
  BOOST_CHECK_EQUAL(descriptor.streamKey.size(), 32);
  BOOST_CHECK_EQUAL(descriptor.nonceSalt.size(), 4);
  BOOST_CHECK_EQUAL(descriptor.frontiers.oldestRetained, 0);
  BOOST_CHECK_EQUAL(descriptor.frontiers.latestJoin, 4);
  BOOST_CHECK_EQUAL(descriptor.frontiers.latestProduced, 5);
  BOOST_CHECK_EQUAL(descriptor.frontiers.mappingCommittedThrough, 7);
  BOOST_CHECK_EQUAL(descriptor.frontiers.nextReserved, 8);
  BOOST_CHECK_EQUAL(encodeVideoStreamDescriptor(descriptor), wire);

  const auto resolverConfig = makeUavStreamNameMapResolverConfig(descriptor);
  BOOST_CHECK_EQUAL(resolverConfig.contractVersion,
                    ndn_service_framework::STREAM_NAME_MAP_CONTRACT_VERSION_V2);
  BOOST_CHECK_EQUAL(resolverConfig.expectedProvider, provider);
  BOOST_CHECK_EQUAL(resolverConfig.mappingRoot, descriptor.mappingRoot);
  BOOST_CHECK_EQUAL(resolverConfig.payloadPrefix, descriptor.dataPrefix);
  BOOST_CHECK_EQUAL(resolverConfig.mappingVersion, 23);
  BOOST_CHECK_EQUAL(
    resolverConfig.maxReverseEntries,
    ndnsf::examples::uav::UAV_VIDEO_MAX_NAME_RESERVATIONS);
  const auto checkpoint = makeUavStreamNameMapCheckpoint(descriptor);
  BOOST_CHECK_EQUAL(checkpoint.blockNumber, 1);
  BOOST_CHECK(checkpoint.frontiers.nextReserved == 8);
  BOOST_CHECK(checkpoint.contentDigest == descriptor.mappingAnchorContentDigest);
  BOOST_CHECK_EQUAL(
    ndn_service_framework::makeStreamNameMapBlockName(
      descriptor.mappingRoot, descriptor.mappingVersion,
      descriptor.mappingAnchorBlock).toUri(),
    "/uav/7/NDNSF/STREAM-MAP/stream-00112233445566778899aabbccddeeff/"
    "v=23/seq=1");

  BOOST_CHECK_THROW(
    decodeVideoStreamDescriptorStrict(
      wire + ";stream_id=stream-00112233445566778899aabbccddeeff",
      provider, provider, service, service),
    std::invalid_argument);

  auto uppercaseKey = fields;
  uppercaseKey["stream_key_hex"][10] = 'A';
  BOOST_CHECK_THROW(
    decodeVideoStreamDescriptorStrict(
      encodeFields(uppercaseKey), provider, provider, service, service),
    std::invalid_argument);

  auto shortKey = fields;
  shortKey["stream_key_hex"].pop_back();
  BOOST_CHECK_THROW(
    decodeVideoStreamDescriptorStrict(
      encodeFields(shortKey), provider, provider, service, service),
    std::invalid_argument);

  auto uppercaseSalt = fields;
  uppercaseSalt["nonce_salt_hex"][0] = 'A';
  BOOST_CHECK_THROW(
    decodeVideoStreamDescriptorStrict(
      encodeFields(uppercaseSalt), provider, provider, service, service),
    std::invalid_argument);

  auto nonCanonicalInteger = fields;
  nonCanonicalInteger["key_epoch"] = "03";
  BOOST_CHECK_THROW(
    decodeVideoStreamDescriptorStrict(
      encodeFields(nonCanonicalInteger), provider, provider, service, service),
    std::invalid_argument);

  auto zeroReservationLimit = fields;
  zeroReservationLimit["max_name_reservations"] = "0";
  BOOST_CHECK_THROW(
    decodeVideoStreamDescriptorStrict(
      encodeFields(zeroReservationLimit), provider, provider, service, service),
    std::invalid_argument);

  auto badFrontier = fields;
  badFrontier["latest_join_cursor"] = "6";
  BOOST_CHECK_THROW(
    decodeVideoStreamDescriptorStrict(
      encodeFields(badFrontier), provider, provider, service, service),
    std::invalid_argument);

  BOOST_CHECK_THROW(
    decodeVideoStreamDescriptorStrict(
      wire, provider, ndn::Name("/uav/8"), service, service),
    std::invalid_argument);
  BOOST_CHECK_THROW(
    decodeVideoStreamDescriptorStrict(
      wire, provider, provider, service, ndn::Name("/UAV/Camera/Status")),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(UavStreamDescriptorProjectsLegacyBoundedOpaqueClass)
{
  auto descriptor = makeUavStreamDescriptor();
  descriptor.extensions["sample_class_mode"] = "bounded-opaque";
  descriptor.extensions["sample_class_opaque_seed"] = "4";
  const auto core = toCoreLiveStreamDescriptor(descriptor);
  BOOST_REQUIRE_EQUAL(core.definition.sampleClasses.size(), 1);
  BOOST_CHECK_EQUAL(core.definition.sampleClasses.front().classId, "opaque");
  BOOST_CHECK_EQUAL(core.definition.sampleClasses.front().seedSourceItems, 4);
  BOOST_CHECK_EQUAL(core.definition.sampleClasses.front().hardMaxSourceItems, 4);

  descriptor.extensions["sample_class_mode"] = "unsupported";
  BOOST_CHECK_THROW(toCoreLiveStreamDescriptor(descriptor),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(UavStreamProjectsSecretsOutOfCoreDescriptor)
{
  auto uav = makeUavStreamDescriptor();
  const auto key = uav.streamKey;
  const auto salt = uav.nonceSalt;
  const auto core = toCoreLiveStreamDescriptor(uav);
  BOOST_CHECK_EQUAL(core.definition.streamId, uav.streamId);
  BOOST_CHECK_EQUAL(core.definition.provider, uav.providerIdentity);
  BOOST_CHECK_EQUAL(core.definition.semanticDataPrefix, uav.dataPrefix);
  BOOST_CHECK_EQUAL(core.safeJoinCursor, uav.frontiers.latestJoin);
  BOOST_REQUIRE(core.definition.fec.enabled());
  BOOST_CHECK_EQUAL(core.definition.fec.maxSourceItems, 4);

  auto archived = uav;
  archived.frontiers.oldestRetained = 0;
  archived.frontiers.latestJoin = 0;
  archived.frontiers.latestProduced = 15031;
  archived.frontiers.mappingCommittedThrough = 15031;
  archived.frontiers.nextReserved = 15032;
  archived.mappingAnchorBlock = 0;
  const auto archivedCore = toCoreLiveStreamDescriptor(archived);
  BOOST_CHECK_GE(archivedCore.definition.retainedItems, 15032);
  BOOST_CHECK(!archivedCore.validate());

  applyCoreLiveStreamDescriptor(uav, core);
  BOOST_CHECK(uav.streamKey == key);
  BOOST_CHECK(uav.nonceSalt == salt);

  auto substituted = core;
  substituted.definition.provider = "/uav/8";
  BOOST_CHECK_THROW(applyCoreLiveStreamDescriptor(uav, substituted),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(UavProvisionalDescriptorTracksAheadMappingFrontiers)
{
  auto descriptor = makeUavStreamDescriptor();
  ndn_service_framework::LiveStreamStatus status;
  status.frontiers = descriptor.frontiers;

  // Reproduces the frozen Spec 125 failure: the next atomic sample begins
  // beyond the provisional descriptor's original committed Mapping block.
  BOOST_CHECK_THROW(applyCoreLiveStreamStatus(descriptor, status, 8),
                    std::invalid_argument);

  status.frontiers.mappingCommittedThrough = 31;
  status.frontiers.nextReserved = 32;
  applyCoreLiveStreamStatus(descriptor, status, 8);
  BOOST_CHECK_EQUAL(descriptor.frontiers.latestProduced, 8);
  BOOST_CHECK_EQUAL(descriptor.frontiers.mappingCommittedThrough, 31);
  BOOST_CHECK_EQUAL(descriptor.frontiers.nextReserved, 32);
}

BOOST_AUTO_TEST_CASE(UavStreamReadinessRequiresMeasuredGroupsAndDecoderSafeJoin)
{
  UavH264ReadinessTracker tracker(3);
  const std::vector<uint8_t> parameterSets{
    0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f,
    0x00, 0x00, 0x01, 0x68, 0xee, 0x3c, 0x80,
  };
  const std::vector<uint8_t> delta{
    0x00, 0x00, 0x01, 0x41, 0x9a, 0x22,
  };
  const std::vector<uint8_t> idr{
    0x00, 0x00, 0x01, 0x65, 0x88, 0x84,
  };

  BOOST_CHECK(!tracker.ready());
  tracker.observePublicationGroup(0, 8, 100, parameterSets);
  tracker.observePublicationGroup(9, 17, 133, delta);
  BOOST_CHECK(!tracker.ready());
  tracker.observePublicationGroup(18, 26, 166, idr);
  BOOST_CHECK(tracker.ready());
  BOOST_CHECK_EQUAL(tracker.completedGroups(), 3);
  BOOST_CHECK_EQUAL(tracker.samplePeriodMs(), 33);
  BOOST_CHECK_EQUAL(tracker.latestJoinCursor(), 0);
  BOOST_CHECK_EQUAL(tracker.latestProducedCursor(), 26);

  tracker.reset();
  tracker.observePublicationGroup(0, 8, 100, parameterSets);
  tracker.observePublicationGroup(9, 17, 133, delta);
  tracker.observePublicationGroup(18, 26, 166, delta);
  BOOST_CHECK(!tracker.ready());
  BOOST_CHECK_EQUAL(tracker.reason(), "waiting-idr");
  BOOST_CHECK_THROW(
    tracker.observePublicationGroup(20, 28, 199, idr),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(UavStreamSemanticDataNameContract)
{
  const auto descriptor = decodeVideoStreamDescriptorStrict(
    encodeFields(makeUavStreamDescriptorFields()),
    ndn::Name("/uav/7"), ndn::Name("/uav/7"),
    ndn::Name("/UAV/Camera/Video"), ndn::Name("/UAV/Camera/Video"));

  VideoPacket data;
  data.streamId = descriptor.streamId;
  data.streamSessionEpoch = descriptor.sessionEpoch;
  data.packetSeq = 6;
  data.frameSeq = 9;
  data.frameSegmentIndex = 1;
  data.frameSegmentCount = 4;
  data.fecDataShards = 3;
  data.fecParityShards = 1;
  data.fecSymbolIndex = 1;
  data.fecSymbolCount = 4;

  const auto dataName = makeUavVideoDataName(descriptor, data);
  BOOST_CHECK_EQUAL(dataName.cursor, 6);
  BOOST_CHECK(!dataName.parity);
  BOOST_CHECK_EQUAL(dataName.name.toUri(),
    "/uav/7/video/front/stream-00112233445566778899aabbccddeeff/"
    "v=23/fec-group/seq=9/data/seg=1");
  BOOST_CHECK(dataName.finalBlockId.isSegment());
  BOOST_CHECK_EQUAL(dataName.finalBlockId.toSegment(), 2);

  auto parity = data;
  parity.packetSeq = 7;
  parity.frameSegmentIndex = 3;
  parity.fecSymbolIndex = 3;
  const auto parityName = makeUavVideoDataName(descriptor, parity);
  BOOST_CHECK(parityName.parity);
  BOOST_CHECK_EQUAL(parityName.name.toUri(),
    "/uav/7/video/front/stream-00112233445566778899aabbccddeeff/"
    "v=23/fec-group/seq=9/parity/seg=0");
  BOOST_CHECK_EQUAL(parityName.finalBlockId.toSegment(), 0);

  auto wrongStream = data;
  wrongStream.streamId = "stream-ffffffffffffffffffffffffffffffff";
  BOOST_CHECK_THROW(makeUavVideoDataName(descriptor, wrongStream),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(UavStreamThousandCursorNamesAndNoncesRemainUnique)
{
  const auto descriptor = makeUavStreamDescriptor();
  std::set<ndn::Name> names;
  std::set<ndn::Buffer> nonces;
  for (uint64_t cursor = 0; cursor < 1000; ++cursor) {
    VideoPacket packet;
    packet.streamId = descriptor.streamId;
    packet.streamSessionEpoch = descriptor.sessionEpoch;
    packet.packetSeq = cursor;
    packet.frameSeq = cursor / 4;
    packet.frameSegmentIndex = static_cast<uint32_t>(cursor % 4);
    packet.frameSegmentCount = 4;
    packet.fecDataShards = 3;
    packet.fecParityShards = 1;
    packet.fecSymbolIndex = static_cast<uint32_t>(cursor % 4);
    packet.fecSymbolCount = 4;

    const auto binding = makeUavVideoDataName(descriptor, packet);
    BOOST_CHECK_EQUAL(binding.cursor, cursor);
    BOOST_CHECK(names.insert(binding.name).second);
    BOOST_CHECK(nonces.insert(deriveUavVideoNonce(descriptor.nonceSalt, cursor)).second);
  }
  BOOST_CHECK_EQUAL(names.size(), 1000);
  BOOST_CHECK_EQUAL(nonces.size(), 1000);
}

BOOST_AUTO_TEST_CASE(UavStreamNonceAndCanonicalAadContract)
{
  const ndn::Buffer salt{0xa1, 0xb2, 0xc3, 0xd4};
  const auto nonce = deriveUavVideoNonce(salt, 0x0102030405060708ULL);
  BOOST_CHECK_EQUAL(ndn::toHex(nonce), "A1B2C3D40102030405060708");
  BOOST_CHECK_THROW(deriveUavVideoNonce(ndn::Buffer(3, 0), 1),
                    std::invalid_argument);

  UavVideoAad aad;
  aad.exactDataName = ndn::Name(
    "/uav/7/video/front/stream-00112233445566778899aabbccddeeff/"
    "v=23/fec-group/seq=9/data/seg=1");
  aad.providerIdentity = ndn::Name("/uav/7");
  aad.serviceName = ndn::Name("/UAV/Camera/Video");
  aad.streamId = "stream-00112233445566778899aabbccddeeff";
  aad.sessionEpoch = 17;
  aad.mappingVersion = 23;
  aad.keyEpoch = 3;
  aad.cursor = 0x0102030405060708ULL;

  const auto wire = aad.wireEncode();
  const auto decoded = UavVideoAad::wireDecodeStrict(wire);
  BOOST_CHECK_EQUAL(decoded.exactDataName, aad.exactDataName);
  BOOST_CHECK_EQUAL(decoded.providerIdentity, aad.providerIdentity);
  BOOST_CHECK_EQUAL(decoded.serviceName, aad.serviceName);
  BOOST_CHECK_EQUAL(decoded.streamId, aad.streamId);
  BOOST_CHECK_EQUAL(decoded.sessionEpoch, aad.sessionEpoch);
  BOOST_CHECK_EQUAL(decoded.mappingVersion, aad.mappingVersion);
  BOOST_CHECK_EQUAL(decoded.keyEpoch, aad.keyEpoch);
  BOOST_CHECK_EQUAL(decoded.cursor, aad.cursor);
  BOOST_CHECK_EQUAL(blockHex(decoded.wireEncode()), blockHex(wire));

  // Freeze a byte-exact vector after the private TLV assignments and the
  // standard nested Name encoding have both been applied.
  BOOST_CHECK_EQUAL(blockHex(wire),
    "fdf700d2fdf7010101fdf7025b075908037561760801370805766964656f080566726f6e74"
    "082773747265616d2d3030313132323333343435353636373738383939616162626363646465"
    "65666636011708096665632d67726f75703a0109080464617461320101fdf7030a0708080375"
    "6176080137fdf7041607140803554156080643616d6572610805566964656ffdf7052773747265"
    "616d2d3030313132323333343435353636373738383939616162626363646465656666fdf706"
    "0111fdf7070117fdf7080103fdf709080102030405060708");

  ndn::Block withUnknown(wire.type());
  wire.parse();
  for (const auto& element : wire.elements()) {
    withUnknown.push_back(element);
  }
  withUnknown.push_back(ndn::makeEmptyBlock(0xF70A));
  withUnknown.encode();
  BOOST_CHECK_THROW(UavVideoAad::wireDecodeStrict(withUnknown),
                    std::invalid_argument);

  ndn::Block duplicate(wire.type());
  for (const auto& element : wire.elements()) {
    duplicate.push_back(element);
  }
  duplicate.push_back(wire.elements().front());
  duplicate.encode();
  BOOST_CHECK_THROW(UavVideoAad::wireDecodeStrict(duplicate),
                    std::invalid_argument);

  const std::array<uint8_t, 2> nonMinimalOne{0x00, 0x01};
  ndn::Block nonMinimal(wire.type());
  nonMinimal.push_back(ndn::makeBinaryBlock(
    ndnsf::examples::uav::uav_stream_tlv::UavVideoAadVersionType,
    nonMinimalOne.begin(), nonMinimalOne.end()));
  for (size_t i = 1; i < wire.elements().size(); ++i) {
    nonMinimal.push_back(wire.elements()[i]);
  }
  nonMinimal.encode();
  BOOST_CHECK_THROW(UavVideoAad::wireDecodeStrict(nonMinimal),
                    std::invalid_argument);

  ndn::Block reordered(wire.type());
  reordered.push_back(wire.elements()[1]);
  reordered.push_back(wire.elements()[0]);
  for (size_t i = 2; i < wire.elements().size(); ++i) {
    reordered.push_back(wire.elements()[i]);
  }
  reordered.encode();
  BOOST_CHECK_THROW(UavVideoAad::wireDecodeStrict(reordered),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(UavStreamEncryptedVideoPacketStrictRoundTrip)
{
  const auto descriptor = makeUavStreamDescriptor();
  VideoPacket packet;
  packet.streamId = descriptor.streamId;
  packet.streamSessionEpoch = descriptor.sessionEpoch;
  packet.second = 4;
  packet.packetSeq = 6;
  packet.frameSeq = 9;
  packet.captureMs = 123456;
  packet.frameFirstPacketSeq = 5;
  packet.frameLastPacketSeq = 8;
  packet.bucketPacketCount = 7;
  packet.frameSegmentIndex = 1;
  packet.frameSegmentCount = 4;
  packet.keyFrame = true;
  packet.encoding = "h264";
  packet.fecDataShards = 3;
  packet.fecParityShards = 1;
  packet.fecSymbolIndex = 1;
  packet.fecSymbolCount = 4;
  packet.fecDataLengths = "5,4,3";
  packet.payload = {0x00, 0x01, 0xff, 0x7f, 0x00};
  const auto binding = makeUavVideoDataName(descriptor, packet);

  UavVideoNonceUseGuard guard(descriptor);
  const auto protectedWire = protectUavVideoPacket(
    descriptor, binding, packet, guard);
  const ndn::Block protectedBlock(ndn::span<const uint8_t>(
    protectedWire.data(), protectedWire.size()));
  BOOST_CHECK_EQUAL(blockSha256Hex(protectedBlock),
                    "767bd7c6357d7e3b591aeb6a03d5c795d7799e881f287eca054aadc02a586e27");
  const auto envelope = decodeUavVideoEnvelopeStrict(
    protectedWire, descriptor, binding);
  BOOST_CHECK_EQUAL(envelope.getAlgorithm(), "AES-256-GCM");
  BOOST_CHECK_EQUAL(envelope.getKeyId(), descriptor.streamId);
  BOOST_CHECK_EQUAL(envelope.getEpochId(), "3");
  BOOST_CHECK_EQUAL(envelope.getMessageType(), "uav-live-video-packet");
  BOOST_CHECK_EQUAL(ndn::toHex(envelope.getNonce()),
                    "A1B2C3D40000000000000006");
  BOOST_CHECK_EQUAL(envelope.getAuthTag().size(), 16);
  BOOST_CHECK(!envelope.hasWrappedMessageKey());

  const auto decoded = unprotectUavVideoPacket(
    descriptor, binding, ndn::Name("/uav/7"), protectedWire);
  BOOST_CHECK(encodeVideoPacket(decoded) == encodeVideoPacket(packet));

  // A consumer does not know the variable per-frame source extent before it
  // authenticates and decodes the packet. An omitted FinalBlock is therefore
  // resolved from authenticated VideoPacket coordinates, while an explicit
  // contradictory value remains a hard failure.
  auto unresolvedFinalBlock = binding;
  unresolvedFinalBlock.finalBlockId = ndn::name::Component();
  const auto decodedVariableExtent = unprotectUavVideoPacket(
    descriptor, unresolvedFinalBlock, ndn::Name("/uav/7"), protectedWire);
  BOOST_CHECK(encodeVideoPacket(decodedVariableExtent) == encodeVideoPacket(packet));
  auto contradictoryFinalBlock = binding;
  contradictoryFinalBlock.finalBlockId = ndn::name::Component::fromSegment(99);
  BOOST_CHECK_THROW(
    unprotectUavVideoPacket(
      descriptor, contradictoryFinalBlock, ndn::Name("/uav/7"), protectedWire),
    std::invalid_argument);

  // Payload publication is bound to immutable session identity, name and
  // cursor, not to a descriptor's historical join-checkpoint snapshot.
  auto historicalCheckpoint = descriptor;
  historicalCheckpoint.frontiers = {0, 0, 0, 3, 4};
  historicalCheckpoint.mappingAnchorBlock = 0;
  UavVideoNonceUseGuard historicalGuard(historicalCheckpoint);
  const auto historicalWire = protectUavVideoPacket(
    historicalCheckpoint, binding, packet, historicalGuard);
  const auto historicalDecoded = unprotectUavVideoPacket(
    historicalCheckpoint, unresolvedFinalBlock, ndn::Name("/uav/7"), historicalWire);
  BOOST_CHECK(encodeVideoPacket(historicalDecoded) == encodeVideoPacket(packet));

  BOOST_CHECK_THROW(
    unprotectUavVideoPacket(
      descriptor, binding, ndn::Name("/uav/8"), protectedWire),
    std::invalid_argument);

  auto wrongCursor = binding;
  wrongCursor.cursor = 7;
  BOOST_CHECK_THROW(
    unprotectUavVideoPacket(
      descriptor, wrongCursor, ndn::Name("/uav/7"), protectedWire),
    std::invalid_argument);

  auto wrongName = binding;
  wrongName.name.append("tampered");
  BOOST_CHECK_THROW(
    unprotectUavVideoPacket(
      descriptor, wrongName, ndn::Name("/uav/7"), protectedWire),
    std::invalid_argument);

  auto wrongService = descriptor;
  wrongService.serviceName = ndn::Name("/UAV/Camera/Status");
  BOOST_CHECK_THROW(
    unprotectUavVideoPacket(
      wrongService, binding, ndn::Name("/uav/7"), protectedWire),
    std::invalid_argument);

  auto wrongSession = descriptor;
  wrongSession.sessionEpoch += 1;
  BOOST_CHECK_THROW(
    unprotectUavVideoPacket(
      wrongSession, binding, ndn::Name("/uav/7"), protectedWire),
    std::invalid_argument);

  auto wrongKey = descriptor;
  wrongKey.streamKey[0] ^= 0x01;
  BOOST_CHECK_THROW(
    unprotectUavVideoPacket(
      wrongKey, binding, ndn::Name("/uav/7"), protectedWire),
    std::invalid_argument);

  auto wrongKeyEpoch = descriptor;
  wrongKeyEpoch.keyEpoch += 1;
  BOOST_CHECK_THROW(
    unprotectUavVideoPacket(
      wrongKeyEpoch, binding, ndn::Name("/uav/7"), protectedWire),
    std::invalid_argument);

  auto wrongMapping = descriptor;
  wrongMapping.mappingVersion += 1;
  wrongMapping.dataPrefix = ndn::Name("/uav/7/video/front");
  wrongMapping.dataPrefix.append(wrongMapping.streamId)
                         .appendVersion(wrongMapping.mappingVersion);
  const auto wrongMappingBinding = makeUavVideoDataName(wrongMapping, packet);
  BOOST_CHECK_THROW(
    unprotectUavVideoPacket(
      wrongMapping, wrongMappingBinding, ndn::Name("/uav/7"), protectedWire),
    std::invalid_argument);

  auto wrongStream = descriptor;
  wrongStream.streamId = "stream-ffffffffffffffffffffffffffffffff";
  wrongStream.dataPrefix = ndn::Name("/uav/7/video/front");
  wrongStream.dataPrefix.append(wrongStream.streamId)
                        .appendVersion(wrongStream.mappingVersion);
  wrongStream.mappingRoot = ndn_service_framework::makeStreamNameMapRoot(
    wrongStream.providerIdentity, wrongStream.streamId);
  auto wrongStreamPacket = packet;
  wrongStreamPacket.streamId = wrongStream.streamId;
  const auto wrongStreamBinding = makeUavVideoDataName(
    wrongStream, wrongStreamPacket);
  BOOST_CHECK_THROW(
    unprotectUavVideoPacket(
      wrongStream, wrongStreamBinding, ndn::Name("/uav/7"), protectedWire),
    std::invalid_argument);

  BOOST_CHECK_THROW(
    protectUavVideoPacket(descriptor, binding, packet, guard),
    std::invalid_argument);
  BOOST_CHECK(guard.isClosed());
}

BOOST_AUTO_TEST_CASE(UavStreamStrictEnvelopeRejectsMalformedWire)
{
  const auto descriptor = makeUavStreamDescriptor();
  VideoPacket packet;
  packet.streamId = descriptor.streamId;
  packet.streamSessionEpoch = descriptor.sessionEpoch;
  packet.packetSeq = 6;
  packet.frameSeq = 9;
  packet.frameSegmentIndex = 0;
  packet.frameSegmentCount = 1;
  packet.fecDataShards = 1;
  packet.fecSymbolIndex = 0;
  packet.fecSymbolCount = 1;
  packet.payload = {0x00, 0xff};
  const auto binding = makeUavVideoDataName(descriptor, packet);
  UavVideoNonceUseGuard guard(descriptor);
  const auto protectedWire = protectUavVideoPacket(
    descriptor, binding, packet, guard);

  ndn::Block valid(ndn::span<const uint8_t>(protectedWire.data(),
                                            protectedWire.size()));
  valid.parse();

  ndn::Block unknown(valid.type());
  for (const auto& element : valid.elements()) {
    unknown.push_back(element);
  }
  unknown.push_back(ndn::makeEmptyBlock(0xF70A));
  unknown.encode();
  BOOST_CHECK_THROW(
    decodeUavVideoEnvelopeStrict(blockBuffer(unknown), descriptor, binding),
    std::invalid_argument);

  ndn::Block duplicate(valid.type());
  for (const auto& element : valid.elements()) {
    duplicate.push_back(element);
  }
  duplicate.push_back(valid.elements()[5]);
  duplicate.encode();
  BOOST_CHECK_THROW(
    decodeUavVideoEnvelopeStrict(blockBuffer(duplicate), descriptor, binding),
    std::invalid_argument);

  HybridMessageEnvelope wrapped = decodeUavVideoEnvelopeStrict(
    protectedWire, descriptor, binding);
  wrapped.setWrappedMessageKey(ndn::Buffer{0x01});
  BOOST_CHECK_THROW(
    decodeUavVideoEnvelopeStrict(blockBuffer(wrapped.WireEncode()),
                                 descriptor, binding),
    std::invalid_argument);

  HybridMessageEnvelope shortNonce = decodeUavVideoEnvelopeStrict(
    protectedWire, descriptor, binding);
  shortNonce.setNonce(ndn::Buffer(11, 0));
  BOOST_CHECK_THROW(
    decodeUavVideoEnvelopeStrict(blockBuffer(shortNonce.WireEncode()),
                                 descriptor, binding),
    std::invalid_argument);

  HybridMessageEnvelope shortTag = decodeUavVideoEnvelopeStrict(
    protectedWire, descriptor, binding);
  shortTag.setAuthTag(ndn::Buffer(15, 0));
  BOOST_CHECK_THROW(
    decodeUavVideoEnvelopeStrict(blockBuffer(shortTag.WireEncode()),
                                 descriptor, binding),
    std::invalid_argument);

  HybridMessageEnvelope tamperedCipher = decodeUavVideoEnvelopeStrict(
    protectedWire, descriptor, binding);
  auto ciphertext = tamperedCipher.getCipherText();
  ciphertext[0] ^= 0x01;
  tamperedCipher.setCipherText(ciphertext);
  BOOST_CHECK_THROW(
    unprotectUavVideoPacket(
      descriptor, binding, ndn::Name("/uav/7"),
      blockBuffer(tamperedCipher.WireEncode())),
    std::invalid_argument);

  HybridMessageEnvelope tamperedTag = decodeUavVideoEnvelopeStrict(
    protectedWire, descriptor, binding);
  auto authTag = tamperedTag.getAuthTag();
  authTag[0] ^= 0x01;
  tamperedTag.setAuthTag(authTag);
  BOOST_CHECK_THROW(
    unprotectUavVideoPacket(
      descriptor, binding, ndn::Name("/uav/7"),
      blockBuffer(tamperedTag.WireEncode())),
    std::invalid_argument);

  auto trailing = protectedWire;
  trailing.push_back(0x00);
  BOOST_CHECK_THROW(
    decodeUavVideoEnvelopeStrict(trailing, descriptor, binding),
    std::invalid_argument);

  UavVideoNonceUseGuard remapGuard(descriptor);
  remapGuard.reserve(descriptor, binding);
  auto remapped = binding;
  remapped.name.append("different");
  BOOST_CHECK_THROW(remapGuard.reserve(descriptor, remapped),
                    std::invalid_argument);
  BOOST_CHECK(remapGuard.isClosed());

  UavVideoNonceUseGuard explicitNameGuard(descriptor);
  explicitNameGuard.reserve(descriptor, binding);
  auto sameNameAtNextCursor = binding;
  sameNameAtNextCursor.cursor += 1;
  BOOST_CHECK_THROW(
    explicitNameGuard.reserve(descriptor, sameNameAtNextCursor),
    std::invalid_argument);
  BOOST_CHECK(explicitNameGuard.isClosed());

  UavVideoNonceUseGuard rollbackGuard(descriptor);
  rollbackGuard.reserve(descriptor, binding);
  auto rollback = binding;
  rollback.cursor -= 1;
  BOOST_CHECK_THROW(rollbackGuard.reserve(descriptor, rollback),
                    std::invalid_argument);
  BOOST_CHECK(rollbackGuard.isClosed());

  // Spec 125 continuations can be appended after future sample reservations.
  // Unique cursor-derived nonces remain safe even when encryption order is
  // non-monotonic, while reusing a cursor under another name must still close
  // the guard.
  UavVideoNonceUseGuard continuationGuard(descriptor);
  auto laterBinding = binding;
  laterBinding.cursor += 2;
  laterBinding.name.append("later");
  continuationGuard.reserve(descriptor, laterBinding);
  auto earlierBinding = binding;
  earlierBinding.cursor += 1;
  earlierBinding.name.append("earlier");
  BOOST_CHECK_NO_THROW(continuationGuard.reserve(descriptor, earlierBinding));
  auto reusedCursor = binding;
  reusedCursor.cursor = laterBinding.cursor;
  reusedCursor.name.append("reused-cursor");
  BOOST_CHECK_THROW(continuationGuard.reserve(descriptor, reusedCursor),
                    std::invalid_argument);
  BOOST_CHECK(continuationGuard.isClosed());

  UavVideoNonceUseGuard overflowGuard(descriptor);
  auto overflow = binding;
  overflow.cursor = std::numeric_limits<uint64_t>::max();
  BOOST_CHECK_THROW(overflowGuard.reserve(descriptor, overflow),
                    std::invalid_argument);
  BOOST_CHECK(overflowGuard.isClosed());

  const auto expectContextChangeCloses = [&] (const auto& changedDescriptor) {
    UavVideoNonceUseGuard contextGuard(descriptor);
    contextGuard.reserve(descriptor, binding);
    auto nextBinding = binding;
    nextBinding.cursor += 1;
    nextBinding.name.append("next-context");
    BOOST_CHECK_THROW(contextGuard.reserve(changedDescriptor, nextBinding),
                      std::invalid_argument);
    BOOST_CHECK(contextGuard.isClosed());
  };
  auto changedKey = descriptor;
  changedKey.streamKey[0] ^= 0x01;
  expectContextChangeCloses(changedKey);
  auto changedSalt = descriptor;
  changedSalt.nonceSalt[0] ^= 0x01;
  expectContextChangeCloses(changedSalt);
  auto changedProvider = descriptor;
  changedProvider.providerIdentity = ndn::Name("/uav/8");
  expectContextChangeCloses(changedProvider);
  auto changedService = descriptor;
  changedService.serviceName = ndn::Name("/UAV/Camera/Other");
  expectContextChangeCloses(changedService);
  auto changedMappingVersion = descriptor;
  changedMappingVersion.mappingVersion += 1;
  expectContextChangeCloses(changedMappingVersion);
  auto changedBlockCapacity = descriptor;
  changedBlockCapacity.mappingBlockCapacity *= 2;
  expectContextChangeCloses(changedBlockCapacity);

  auto changedReservationLimit = descriptor;
  changedReservationLimit.maxNameReservations /= 2;
  expectContextChangeCloses(changedReservationLimit);

  auto oneEntryDescriptor = descriptor;
  oneEntryDescriptor.mappingBlockCapacity = 1;
  oneEntryDescriptor.maxNameReservations = 1;
  oneEntryDescriptor.mappingAnchorBlock = 0;
  oneEntryDescriptor.frontiers.oldestRetained = 0;
  oneEntryDescriptor.frontiers.latestJoin = 0;
  oneEntryDescriptor.frontiers.latestProduced = 0;
  oneEntryDescriptor.frontiers.mappingCommittedThrough = 0;
  oneEntryDescriptor.frontiers.nextReserved = 1;
  const auto oneEntryResolver =
    makeUavStreamNameMapResolverConfig(oneEntryDescriptor);
  BOOST_CHECK_EQUAL(oneEntryResolver.maxReverseEntries, 1);
  UavVideoNonceUseGuard oneEntryGuard(oneEntryDescriptor);
  oneEntryGuard.reserve(oneEntryDescriptor, binding);
  auto secondUniqueBinding = binding;
  secondUniqueBinding.cursor += 1;
  secondUniqueBinding.name.append("second-unique-name");
  BOOST_CHECK_THROW(
    oneEntryGuard.reserve(oneEntryDescriptor, secondUniqueBinding),
                    std::invalid_argument);
  BOOST_CHECK(oneEntryGuard.isClosed());
  auto invalidLimitDescriptor = oneEntryDescriptor;
  invalidLimitDescriptor.maxNameReservations = 0;
  BOOST_CHECK_THROW(UavVideoNonceUseGuard{invalidLimitDescriptor},
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(UavStreamOriginalPayloadFitsDirectSignedData)
{
  const auto descriptor = makeUavStreamDescriptor();
  VideoPacket packet;
  packet.streamId = descriptor.streamId;
  packet.streamSessionEpoch = descriptor.sessionEpoch;
  packet.packetSeq = 6;
  packet.frameSeq = 9;
  packet.frameFirstPacketSeq = 6;
  packet.frameLastPacketSeq = 18;
  packet.frameSegmentIndex = 0;
  packet.frameSegmentCount = 13;
  packet.fecDataShards = 12;
  packet.fecParityShards = 1;
  packet.fecSymbolIndex = 0;
  packet.fecSymbolCount = 13;
  packet.payload.assign(3600, 0x5a);
  const auto binding = makeUavVideoDataName(descriptor, packet);
  UavVideoNonceUseGuard guard(descriptor);
  const auto protectedWire = protectUavVideoPacket(
    descriptor, binding, packet, guard);

  ndn::KeyChain keyChain("pib-memory:uav-original-payload",
                         "tpm-memory:uav-original-payload");
  const auto identity = keyChain.createIdentity(descriptor.providerIdentity);
  ndn::Data data(binding.name);
  data.setContent(protectedWire);
  keyChain.sign(data, ndn::security::signingByIdentity(identity));
  BOOST_CHECK_LE(data.wireEncode().size(), ndn::MAX_NDN_PACKET_SIZE);
}

BOOST_AUTO_TEST_CASE(UavStreamLeavesGenericMessageWireUnchanged)
{
  HybridMessageEnvelope envelope;
  envelope.setVersion(1);
  envelope.setAlgorithm("AES-256-GCM");
  envelope.setKeyId("generic-key");
  envelope.setEpochId("9");
  envelope.setMessageType("RESPONSE");
  envelope.setNonce(ndn::Buffer(12, 0));
  envelope.setCipherText(ndn::Buffer{0x00, 0xff});
  envelope.setAuthTag(ndn::Buffer(16, 0x11));
  BOOST_CHECK_EQUAL(blockHex(envelope.WireEncode()),
                    "ac4ea30101a60b4145532d3235362d47434dad0b67656e657269632d6b6579ae"
                    "0139b208524553504f4e5345af0c000000000000000000000000a90200ffb010"
                    "11111111111111111111111111111111");

  ResponseMessage response;
  response.setStatus(true);
  response.setErrorInfo("ok");
  response.setTokens({{"a", "b"}});
  response.setUserToken("token");
  ndn::Buffer payload{0x00, 0xff};
  response.setPayload(payload, payload.size());
  response.setPolicyEpoch(7);
  BOOST_CHECK_EQUAL(blockHex(response.WireEncode()),
                    "811a98010199026f6b9603613d62aa05746f6b656e970200ffa30107");
}

BOOST_AUTO_TEST_CASE(FlightCommandEvidenceFactoriesPreserveTimeSemantics)
{
  const auto pending = FlightCommandState::makePending("A", "arm", 1000, 10500);
  BOOST_CHECK_EQUAL(pending.rttMs, 0);
  BOOST_CHECK_EQUAL(pending.updatedMs, 1000);
  BOOST_CHECK_EQUAL(pending.timeoutMs, 10500);
  BOOST_CHECK_EQUAL(pending.accepted, "unknown");

  const auto timeout = FlightCommandState::makeTimeout("A", "arm", 1000, 11500, 10500);
  BOOST_CHECK_EQUAL(timeout.rttMs, 10500);
  BOOST_CHECK_EQUAL(timeout.updatedMs, 11500);
  BOOST_CHECK_EQUAL(timeout.timeoutMs, 10500);
  BOOST_CHECK(timeout.isTimeout());

  const auto clockRegression = FlightCommandState::makeTimeout("A", "arm", 1000, 900, 10500);
  BOOST_CHECK_EQUAL(clockRegression.rttMs, 0);
  BOOST_CHECK_EQUAL(clockRegression.updatedMs, 1000);
}

BOOST_AUTO_TEST_CASE(AutoControlSequenceStepIsMonotonicAndDispatchesOnce)
{
  AutoControlSequenceStep step;
  BOOST_CHECK(step.beginWait("takeoff", "armed", 100));
  BOOST_CHECK_EQUAL(step.phase, "wait-begin");
  BOOST_CHECK(!step.markDispatched(110));
  BOOST_CHECK(step.satisfy("armed", 125));
  BOOST_CHECK_EQUAL(step.phase, "satisfied");
  BOOST_CHECK(step.markDispatched(130));
  BOOST_CHECK(!step.markDispatched(131));
  BOOST_CHECK_EQUAL(step.dispatchCount, 1);
  BOOST_CHECK(step.terminate("response", 170));
  BOOST_CHECK(step.isTerminal());
  BOOST_CHECK(!step.expire("late", 180));
}

BOOST_AUTO_TEST_CASE(AutoControlSequenceStepExpiresWithoutDispatch)
{
  AutoControlSequenceStep step;
  BOOST_CHECK(step.beginWait("takeoff", "armed", 200));
  BOOST_CHECK(step.expire("armed-state-not-converged", 2700));
  BOOST_CHECK(step.isTerminal());
  BOOST_CHECK(!step.markDispatched(2701));
  BOOST_CHECK_EQUAL(step.dispatchCount, 0);
  BOOST_CHECK_EQUAL(step.reason, "armed-state-not-converged");
  BOOST_CHECK_EQUAL(step.elapsedMs(2800), 2500);
}

BOOST_AUTO_TEST_CASE(FlightSafetyGateCombinesReadinessAndSafety)
{
  std::string reason;
  auto gate = FlightSafetyGateState::fromStates("A", makeReadyState(false), makeSafeState());
  BOOST_CHECK(gate.actionAllowed("arm", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(!gate.actionAllowed("takeoff", reason));
  BOOST_CHECK_EQUAL(reason, "not-armed");
  BOOST_CHECK(!gate.actionAllowed("manual_control", reason));
  BOOST_CHECK_EQUAL(reason, "not-armed");

  gate = FlightSafetyGateState::fromStates("A", makeReadyState(true), makeSafeState());
  BOOST_CHECK(gate.actionAllowed("takeoff", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(gate.actionAllowed("manual_control", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(gate.actionAllowed("control_panel", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(gate.actionAllowed("land", reason));
  BOOST_CHECK_EQUAL(reason, "ok");

  auto airborne = makeReadyState(true);
  airborne.landedStateName = "in-air";
  gate = FlightSafetyGateState::fromStates("A", airborne, makeSafeState());
  BOOST_CHECK(!gate.actionAllowed("takeoff", reason));
  BOOST_CHECK_EQUAL(reason, "not-on-ground");
  BOOST_CHECK(gate.actionAllowed("land", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(gate.actionAllowed("manual_control", reason));
  BOOST_CHECK_EQUAL(reason, "ok");

  TelemetryState airborneTelemetry;
  airborneTelemetry.heartbeatSeen = "true";
  airborneTelemetry.flightControllerReady = "true";
  airborneTelemetry.gpsReady = "true";
  airborneTelemetry.ekfReady = "true";
  airborneTelemetry.batteryReady = "true";
  airborneTelemetry.flightControllerBackend = "mock";
  airborneTelemetry.flightControllerAvailable = "true";
  airborneTelemetry.flightControllerState = "mock-ready";
  airborneTelemetry.flightControllerReason = "ok";
  airborneTelemetry.cameraAvailable = "true";
  airborneTelemetry.cameraSource = "/dev/video42";
  airborneTelemetry.cameraReason = "ok";
  airborneTelemetry.armed = "true";
  airborneTelemetry.readiness = "ready";
  airborneTelemetry.landedStateName = "in-air";
  BOOST_CHECK_EQUAL(airborneTelemetry.toFields().at("ready_for_takeoff"), "false");
  BOOST_CHECK_EQUAL(airborneTelemetry.toFields().at("flight_controller_backend"), "mock");
  BOOST_CHECK_EQUAL(airborneTelemetry.toFields().at("flight_controller_available"), "true");
  BOOST_CHECK_EQUAL(airborneTelemetry.toFields().at("camera_available"), "true");
  BOOST_CHECK_EQUAL(airborneTelemetry.toFields().at("camera_source"), "/dev/video42");
  const auto telemetryRoundTrip = TelemetryState::fromFields(airborneTelemetry.toFields());
  BOOST_CHECK_EQUAL(telemetryRoundTrip.flightControllerBackend, "mock");
  BOOST_CHECK_EQUAL(telemetryRoundTrip.flightControllerAvailable, "true");
  BOOST_CHECK_EQUAL(telemetryRoundTrip.cameraAvailable, "true");
  BOOST_CHECK_EQUAL(telemetryRoundTrip.cameraReason, "ok");
  BOOST_CHECK_NE(telemetryRoundTrip.statusLine().find("fc_backend=mock"), std::string::npos);
  BOOST_CHECK_NE(telemetryRoundTrip.statusLine().find("camera_available=true"), std::string::npos);
  airborneTelemetry.landedStateName = "on-ground";
  BOOST_CHECK_EQUAL(airborneTelemetry.toFields().at("ready_for_takeoff"), "true");

  auto safety = makeSafeState();
  safety.linkState = "lost";
  gate = FlightSafetyGateState::fromStates("A", makeReadyState(true), safety);
  BOOST_CHECK(!gate.actionAllowed("takeoff", reason));
  BOOST_CHECK_EQUAL(reason, "link-lost");
  BOOST_CHECK(!gate.actionAllowed("manual_control", reason));
  BOOST_CHECK_EQUAL(reason, "link-lost");
  BOOST_CHECK(!gate.actionAllowed("control_panel", reason));
  BOOST_CHECK_EQUAL(reason, "link-lost");
  BOOST_CHECK(gate.actionAllowed("land", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(gate.actionAllowed("emergency_stop", reason));
  BOOST_CHECK_EQUAL(reason, "ok");
}

BOOST_AUTO_TEST_CASE(FlightActionControlStateMirrorsSafetyGate)
{
  const auto readyGate = FlightSafetyGateState::fromStates("A", makeReadyState(true), makeSafeState());
  auto action = FlightActionControlState::fromGate(readyGate);
  BOOST_CHECK_EQUAL(action.selectedDrone, "A");
  BOOST_CHECK(action.hasReadiness);
  BOOST_CHECK(action.hasSafety);
  BOOST_CHECK(action.canTakeoff);
  BOOST_CHECK(action.canLand);
  BOOST_CHECK(action.canManualControl);
  BOOST_CHECK(action.canControlPanel);
  BOOST_CHECK(action.canEmergencyStop);
  BOOST_CHECK_EQUAL(action.takeoffReason, "ok");
  BOOST_CHECK_NE(action.statusLine().find("can_takeoff=true"), std::string::npos);
  BOOST_CHECK_NE(action.statusLine().find("emergency_stop=true"), std::string::npos);

  auto safety = makeSafeState();
  safety.linkState = "lost";
  action = FlightActionControlState::fromGate(
    FlightSafetyGateState::fromStates("A", makeReadyState(true), safety));
  BOOST_CHECK(!action.canTakeoff);
  BOOST_CHECK(!action.canManualControl);
  BOOST_CHECK(action.canEmergencyStop);
  BOOST_CHECK_EQUAL(action.takeoffReason, "link-lost");
  BOOST_CHECK_EQUAL(action.manualControlReason, "link-lost");
}

BOOST_AUTO_TEST_CASE(MissionStartGateCombinesMissionAndFlightReadiness)
{
  auto mission = makeMissionState("idle");
  auto flightGate = FlightSafetyGateState::fromStates("A", makeReadyState(false), makeSafeState());
  auto gate = MissionStartGateState::fromStates("A", mission, flightGate);
  BOOST_CHECK(!gate.canStart);
  BOOST_CHECK_EQUAL(gate.startReason, "mission-idle");
  BOOST_CHECK(!gate.canStop);
  BOOST_CHECK_EQUAL(gate.stopReason, "mission-idle");

  mission = makeMissionState("uploaded");
  gate = MissionStartGateState::fromStates("A", mission, std::nullopt);
  BOOST_CHECK(!gate.canStart);
  BOOST_CHECK_EQUAL(gate.startReason, "no-flight-gate");
  BOOST_CHECK(gate.canStop);
  BOOST_CHECK_EQUAL(gate.stopReason, "ok");

  flightGate = FlightSafetyGateState::fromStates("A", makeReadyState(false), makeSafeState());
  gate = MissionStartGateState::fromStates("A", mission, flightGate);
  BOOST_CHECK(gate.canStart);
  BOOST_CHECK_EQUAL(gate.startReason, "ok");
  BOOST_CHECK(gate.canStop);

  auto safety = makeSafeState();
  safety.linkState = "lost";
  flightGate = FlightSafetyGateState::fromStates("A", makeReadyState(true), safety);
  gate = MissionStartGateState::fromStates("A", mission, flightGate);
  BOOST_CHECK(!gate.canStart);
  BOOST_CHECK_EQUAL(gate.startReason, "link-lost");
  BOOST_CHECK(gate.canStop);
  BOOST_CHECK_EQUAL(gate.stopReason, "ok");
}

BOOST_AUTO_TEST_CASE(MissionControlStateCombinesGatesAndProgress)
{
  MissionStartGateState readyA;
  readyA.droneId = "A";
  readyA.hasMission = true;
  readyA.hasFlightGate = true;
  readyA.missionUploaded = true;
  readyA.missionPhase = "uploaded";
  readyA.canStart = true;
  readyA.startReason = "ok";
  readyA.canStop = true;
  readyA.stopReason = "ok";

  auto control = MissionControlState::fromStates({readyA}, std::nullopt, false, false, false);
  BOOST_CHECK(control.canUpload);
  BOOST_CHECK(control.canStart);
  BOOST_CHECK(control.canStop);
  BOOST_CHECK_EQUAL(control.startableCount, 1);
  BOOST_CHECK_EQUAL(control.startEligible, "A");
  BOOST_CHECK_EQUAL(control.startReason, "ok");
  BOOST_CHECK_NE(control.statusLine().find("can_start=true"), std::string::npos);

  auto blockedB = readyA;
  blockedB.droneId = "B";
  blockedB.canStart = false;
  blockedB.startReason = "link-lost";
  control = MissionControlState::fromStates({readyA, blockedB}, std::nullopt, false, false, false);
  BOOST_CHECK(!control.canStart);
  BOOST_CHECK_EQUAL(control.startableCount, 2);
  BOOST_CHECK_EQUAL(control.startEligibleCount, 1);
  BOOST_CHECK_EQUAL(control.startBlockedCount, 1);
  BOOST_CHECK_EQUAL(control.startBlocked, "B:link-lost");
  BOOST_CHECK_EQUAL(control.startReason, "blocked-B:link-lost");

  MissionProgressState progress;
  progress.phase = "executing";
  progress.totalParts = 2;
  progress.completedParts = 1;
  control = MissionControlState::fromStates({readyA}, progress, false, false, false);
  BOOST_CHECK(!control.canUpload);
  BOOST_CHECK(!control.canStart);
  BOOST_CHECK(control.canStop);
  BOOST_CHECK(control.progressActive);
  BOOST_CHECK_EQUAL(control.uploadReason, "progress-active");
  BOOST_CHECK_EQUAL(control.startReason, "progress-active");
}

BOOST_AUTO_TEST_CASE(SelectedActionStateCombinesFlightMissionAndManualMode)
{
  MissionStartGateState missionGate;
  missionGate.droneId = "A";
  missionGate.hasMission = true;
  missionGate.hasFlightGate = true;
  missionGate.missionUploaded = true;
  missionGate.missionPhase = "uploaded";
  missionGate.canStart = true;
  missionGate.startReason = "ok";
  missionGate.canStop = true;
  missionGate.stopReason = "ok";

  const auto mission = MissionControlState::fromStates({missionGate}, std::nullopt,
                                                       false, false, false);
  const auto flight = FlightActionControlState::fromGate(
    FlightSafetyGateState::fromStates("A", makeReadyState(true), makeSafeState()));
  const auto action = SelectedActionState::fromStates("A", flight, mission, true, true);

  BOOST_CHECK_EQUAL(action.selectedDrone, "A");
  BOOST_CHECK(action.flight.canTakeoff);
  BOOST_CHECK(action.flight.canManualControl);
  BOOST_CHECK(action.mission.canStart);
  BOOST_CHECK(action.mission.canStop);
  BOOST_CHECK(action.manualMode);
  BOOST_CHECK(action.manualInputActive);
  BOOST_CHECK(action.emergencyStopAvailable);
  BOOST_CHECK_NE(action.statusLine().find("mission_can_start=true"), std::string::npos);
  BOOST_CHECK_NE(action.statusLine().find("manual_mode=true"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(MissionProgressTracksCompensationAndCompletion)
{
  MissionProgressState progress;
  progress.taskId = "patrol-test";
  progress.phase = "waiting-compensation";
  progress.assignment = "clustered-waypoints-return-to-start";
  progress.drones = "A,B";
  progress.attempts = 1;
  progress.totalParts = 2;
  progress.completedParts = 1;
  progress.missingParts = 1;
  progress.compensatedParts = 0;
  progress.returnHomePlanned = true;
  progress.completedPartIds = "part1";
  progress.missingPartIds = "part0";
  progress.pendingPartIds = "none";

  BOOST_CHECK(progress.isActive());
  BOOST_CHECK(progress.needsCompensation());
  BOOST_CHECK(!progress.isComplete());
  BOOST_CHECK(!progress.isFailed());
  BOOST_CHECK_NE(progress.statusLine().find("return_home=true"), std::string::npos);
  BOOST_CHECK_NE(progress.statusLine().find("missing=part0"), std::string::npos);

  progress.phase = "completed";
  progress.attempts = 2;
  progress.completedParts = 2;
  progress.missingParts = 0;
  progress.compensatedParts = 1;
  progress.completedPartIds = "part0,part1";
  progress.missingPartIds = "none";
  progress.compensatedPartIds = "part0";

  BOOST_CHECK(!progress.isActive());
  BOOST_CHECK(!progress.needsCompensation());
  BOOST_CHECK(progress.isComplete());
  BOOST_CHECK(!progress.isFailed());
  BOOST_CHECK_NE(progress.statusLine().find("compensated_parts=1"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(MissionPlanClustersWaypointsAndReturnsHome)
{
  const std::vector<std::string> drones{"A", "B"};
  const std::vector<MissionWaypoint> route{
    {35.118600, -89.937500},
    {35.118700, -89.937400},
    {35.121000, -89.934000},
    {35.121100, -89.933900},
  };
  const std::map<std::string, MissionWaypoint> departures{
    {"A", {35.117000, -89.938000}},
    {"B", {35.122000, -89.933000}},
  };

  const auto plan = buildPatrolMissionPlan("patrol-test", 35.1186, -89.9375,
                                           140.0, drones, route, departures);
  BOOST_CHECK_EQUAL(plan.taskId, "patrol-test");
  BOOST_CHECK_EQUAL(plan.assignment, "clustered-waypoints-return-to-start");
  BOOST_CHECK_EQUAL(plan.completionObjective, "return-to-start");
  BOOST_CHECK_EQUAL(plan.parts.size(), 2);
  BOOST_CHECK(plan.returnHomePlanned);
  BOOST_CHECK_EQUAL(plan.droneList(), "A,B");
  BOOST_CHECK_NE(plan.statusLine().find("parts=2"), std::string::npos);
  BOOST_CHECK_NE(plan.statusLine().find("completion_objective=return-to-start"), std::string::npos);

  BOOST_CHECK_EQUAL(plan.parts[0].assignedDrone, "A");
  BOOST_CHECK_EQUAL(plan.parts[1].assignedDrone, "B");
  for (const auto& part : plan.parts) {
    BOOST_CHECK(part.returnHomePlanned);
    BOOST_CHECK_GE(part.waypoints.size(), 3);
    BOOST_CHECK_NE(part.waypointText().find(part.role + ":"), std::string::npos);
    const auto departure = departures.at(part.assignedDrone);
    BOOST_CHECK_CLOSE(part.waypoints.back().lat, departure.lat, 0.0001);
    BOOST_CHECK_CLOSE(part.waypoints.back().lon, departure.lon, 0.0001);
    BOOST_CHECK_NE(part.statusLine().find("return_home=true"), std::string::npos);
  }
}

BOOST_AUTO_TEST_CASE(MissionPlanBuildsDefaultSectorsWithoutRoute)
{
  const std::vector<std::string> drones{"A", "B", "C"};
  const auto plan = buildPatrolMissionPlan("patrol-auto", 35.1186, -89.9375,
                                           140.0, drones);
  BOOST_CHECK_EQUAL(plan.parts.size(), 3);
  BOOST_CHECK_EQUAL(plan.completionObjective, "return-to-start");
  BOOST_CHECK_EQUAL(plan.droneList(), "A,B,C");
  for (size_t i = 0; i < plan.parts.size(); ++i) {
    const auto& part = plan.parts[i];
    BOOST_CHECK_EQUAL(part.id, "part" + std::to_string(i));
    BOOST_CHECK_EQUAL(part.assignedDrone, drones[i]);
    BOOST_CHECK_EQUAL(part.waypoints.size(), 5);
    BOOST_CHECK_EQUAL(part.waypoints.back().str(), part.waypoints.front().str());
  }
}

BOOST_AUTO_TEST_CASE(MissionPlanDeterministicClusteringPrototype)
{
  const std::vector<std::string> drones{"A", "B", "C"};
  const std::vector<MissionWaypoint> route{
    {35.119100, -89.936100},
    {35.119200, -89.936300},
    {35.118900, -89.937900},
    {35.119900, -89.937100},
    {35.120100, -89.936500},
  };
  const std::map<std::string, MissionWaypoint> departures{
    {"A", {35.118000, -89.938000}},
    {"B", {35.119000, -89.935000}},
    {"C", {35.120000, -89.938000}},
  };

  const auto plan1 = buildPatrolMissionPlan("patrol-deterministic", 35.1186, -89.9375,
                                           140.0, drones, route, departures);
  const auto plan2 = buildPatrolMissionPlan("patrol-deterministic", 35.1186, -89.9375,
                                           140.0, drones, route, departures);

  BOOST_CHECK_EQUAL(plan1.parts.size(), plan2.parts.size());
  BOOST_CHECK_EQUAL(plan1.droneList(), plan2.droneList());
  BOOST_CHECK_EQUAL(plan1.assignment, plan2.assignment);
  BOOST_CHECK_EQUAL(plan1.returnHomePlanned, plan2.returnHomePlanned);
  for (size_t i = 0; i < plan1.parts.size(); ++i) {
    const auto& p1 = plan1.parts[i];
    const auto& p2 = plan2.parts[i];
    BOOST_CHECK_EQUAL(p1.id, p2.id);
    BOOST_CHECK_EQUAL(p1.role, p2.role);
    BOOST_CHECK_EQUAL(p1.assignedDrone, p2.assignedDrone);
    BOOST_CHECK_EQUAL(p1.returnHomePlanned, p2.returnHomePlanned);
    BOOST_CHECK_EQUAL(p1.waypoints.size(), p2.waypoints.size());
    for (size_t j = 0; j < p1.waypoints.size(); ++j) {
      BOOST_CHECK_CLOSE(p1.waypoints[j].lat, p2.waypoints[j].lat, 0.000001);
      BOOST_CHECK_CLOSE(p1.waypoints[j].lon, p2.waypoints[j].lon, 0.000001);
    }
  }
}

BOOST_AUTO_TEST_CASE(VideoAdaptiveStateRoundTripsAndReportsPressure)
{
  VideoAdaptiveState state;
  state.droneId = "A";
  state.state = "streaming";
  state.rttMs = 142;
  state.requestedBitrateKbps = 8000;
  state.acceptedBitrateKbps = 6000;
  state.suggestedBitrateKbps = 4000;
  state.bitrateAction = "decrease";
  state.bitrateReason = "pressure";
  state.coreFetchDecisionAvailable = true;
  state.coreFetchDecisionSource = "core-live-stream-status";
  state.coreFetchDecisionGeneration = 9;
  state.coreFetchDecisionObservedAtMs = 123450;
  state.coreFetchPhase = "FETCHING";
  state.coreFetchPolicyMode = "adaptive-sample-atomic";
  state.coreFetchCapacityReason = "sample-atomic-fit";
  state.coreFetchReason = "stable-live-edge";
  state.window = 64;
  state.lookahead = 18;
  state.futureProbeLimit = 3;
  state.futureProbeLimitSource = "uav-app-policy";
  state.interestLifetimeMs = 620;
  state.missingTimeoutMs = 240;
  state.timeoutPressure = 55;
  state.probePressure = 20;
  state.duplicatePressure = 10;
  state.lossPressure = 8;
  state.backlogPressure = 30;
  state.primaryPressure = "timeout";
  state.policyReason = "pressure-timeout";
  state.pendingChunks = 12;
  state.pendingBytes = 4096;
  state.receivedChunks = 100;
  state.fecRecoveredChunks = 4;
  state.timeouts = 2;
  state.nacks = 1;
  state.duplicates = 3;
  state.publishedFrames = 90;
  state.decodedFrames = 45;
  state.decodedFrameGap = 45;
  state.frameGapPressure = 35;
  state.updatedMs = 123456;

  const auto decoded = VideoAdaptiveState::fromFields(state.toFields());
  BOOST_CHECK_EQUAL(decoded.droneId, "A");
  BOOST_CHECK_EQUAL(decoded.state, "streaming");
  BOOST_CHECK_EQUAL(decoded.rttMs, 142);
  BOOST_CHECK_EQUAL(decoded.requestedBitrateKbps, 8000);
  BOOST_CHECK_EQUAL(decoded.acceptedBitrateKbps, 6000);
  BOOST_CHECK_EQUAL(decoded.suggestedBitrateKbps, 4000);
  BOOST_CHECK_EQUAL(decoded.bitrateAction, "decrease");
  BOOST_CHECK_EQUAL(decoded.bitrateReason, "pressure");
  BOOST_CHECK(decoded.coreFetchDecisionAvailable);
  BOOST_CHECK_EQUAL(decoded.coreFetchDecisionSource, "core-live-stream-status");
  BOOST_CHECK_EQUAL(decoded.coreFetchDecisionGeneration, 9);
  BOOST_CHECK_EQUAL(decoded.coreFetchDecisionObservedAtMs, 123450);
  BOOST_CHECK_EQUAL(decoded.coreFetchPhase, "FETCHING");
  BOOST_CHECK_EQUAL(decoded.coreFetchPolicyMode, "adaptive-sample-atomic");
  BOOST_CHECK_EQUAL(decoded.coreFetchCapacityReason, "sample-atomic-fit");
  BOOST_CHECK_EQUAL(decoded.coreFetchReason, "stable-live-edge");
  BOOST_CHECK_EQUAL(decoded.window, 64);
  BOOST_CHECK_EQUAL(decoded.futureProbeLimitSource, "uav-app-policy");
  BOOST_CHECK_EQUAL(decoded.missingTimeoutMs, 240);
  BOOST_CHECK_EQUAL(decoded.timeoutPressure, 55);
  BOOST_CHECK_EQUAL(decoded.primaryPressure, "timeout");
  BOOST_CHECK_EQUAL(decoded.policyReason, "pressure-timeout");
  BOOST_CHECK_EQUAL(decoded.pendingBytes, 4096);
  BOOST_CHECK_EQUAL(decoded.fecRecoveredChunks, 4);
  BOOST_CHECK_EQUAL(decoded.publishedFrames, 90);
  BOOST_CHECK_EQUAL(decoded.decodedFrameGap, 45);
  BOOST_CHECK_EQUAL(decoded.frameGapPressure, 35);
  BOOST_CHECK(decoded.underPressure());
  BOOST_CHECK_NE(decoded.statusLine().find("VideoAdaptive drone=A"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("suggested_bitrate_kbps=4000"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("bitrate_action=decrease"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("core_fetch_decision_available=true"),
                 std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("core_fetch_phase=FETCHING"),
                 std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("primary_pressure=timeout"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("policy_reason=pressure-timeout"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("window=64"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("pending_bytes=4096"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("fec_recovered_chunks=4"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("published_frames=90"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("decoded_frame_gap=45"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("frame_gap_pressure=35"), std::string::npos);
  BOOST_CHECK_NE(decoded.statusLine().find("decoded_frames=45"), std::string::npos);

  const auto health = decoded.toStreamHealth(
    7, ndn::Name("/uav/A/video"), 3000, 123999);
  BOOST_CHECK_EQUAL(health.streamId, "A-video");
  BOOST_CHECK_EQUAL(health.sessionEpoch, 7);
  BOOST_CHECK_EQUAL(health.nextSeq, decoded.receivedChunks + decoded.pendingChunks);
  BOOST_CHECK_EQUAL(ndn_service_framework::toString(health.state), "DEGRADED");
  BOOST_CHECK_EQUAL(health.metrics.timeouts, 2);
  BOOST_CHECK_EQUAL(health.metrics.nacks, 1);
  BOOST_CHECK_EQUAL(health.fetchDecision.window, 64);
  BOOST_CHECK_EQUAL(health.metadata.at("primary_pressure"), "timeout");
  const auto healthSummary = decoded.streamHealthSummary(
    7, ndn::Name("/uav/A/video"), 3000, 123999);
  BOOST_CHECK_NE(healthSummary.find("stream_health=DEGRADED"), std::string::npos);
  BOOST_CHECK_NE(healthSummary.find("reason=loss-or-gap"), std::string::npos);
  BOOST_CHECK_NE(healthSummary.find("window=64"), std::string::npos);
  BOOST_CHECK_NE(healthSummary.find("gaps=45"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(VideoCoreFetchDecisionSnapshotReportsOnlyCurrentCoreState)
{
  VideoCoreFetchDecisionSnapshot snapshot;
  snapshot.reset(7);

  VideoAdaptiveState unavailable;
  unavailable.window = 99;
  unavailable.lookahead = 88;
  unavailable.interestLifetimeMs = 777;
  unavailable.missingTimeoutMs = 666;
  snapshot.applyTo(unavailable);
  BOOST_CHECK(!unavailable.coreFetchDecisionAvailable);
  BOOST_CHECK_EQUAL(unavailable.coreFetchDecisionSource, "unavailable");
  BOOST_CHECK_EQUAL(unavailable.coreFetchDecisionGeneration, 7);
  BOOST_CHECK_EQUAL(unavailable.window, 0);
  BOOST_CHECK_EQUAL(unavailable.lookahead, 0);
  BOOST_CHECK_EQUAL(unavailable.interestLifetimeMs, 0);
  BOOST_CHECK_EQUAL(unavailable.missingTimeoutMs, 0);

  ndn_service_framework::LiveStreamStatus current;
  current.state = ndn_service_framework::LiveStreamLifecycleState::Active;
  current.fetchDecision =
    std::make_shared<ndn_service_framework::StreamFetchDecision>();
  current.fetchDecision->window = 41;
  current.fetchDecision->lookahead = 13;
  current.fetchDecision->interestLifetimeMs = 521;
  current.fetchDecision->missingTimeoutMs = 233;
  current.fetchDecision->phase =
    ndn_service_framework::StreamPrefetchPhase::Fetching;
  current.fetchDecision->policyMode = "adaptive-sample-atomic";
  current.fetchDecision->capacityReason = "sample-atomic-fit";
  current.fetchDecision->reason = "stable-live-edge";

  BOOST_CHECK(snapshot.observe(7, 7, current, 123456));
  VideoAdaptiveState available;
  snapshot.applyTo(available);
  BOOST_CHECK(available.coreFetchDecisionAvailable);
  BOOST_CHECK_EQUAL(available.coreFetchDecisionSource, "core-live-stream-status");
  BOOST_CHECK_EQUAL(available.coreFetchDecisionGeneration, 7);
  BOOST_CHECK_EQUAL(available.coreFetchDecisionObservedAtMs, 123456);
  BOOST_CHECK_EQUAL(available.coreFetchPhase, "FETCHING");
  BOOST_CHECK_EQUAL(available.coreFetchPolicyMode, "adaptive-sample-atomic");
  BOOST_CHECK_EQUAL(available.coreFetchCapacityReason, "sample-atomic-fit");
  BOOST_CHECK_EQUAL(available.coreFetchReason, "stable-live-edge");
  BOOST_CHECK_EQUAL(available.window, 41);
  BOOST_CHECK_EQUAL(available.lookahead, 13);
  BOOST_CHECK_EQUAL(available.interestLifetimeMs, 521);
  BOOST_CHECK_EQUAL(available.missingTimeoutMs, 233);

  auto stale = current;
  stale.fetchDecision = std::make_shared<ndn_service_framework::StreamFetchDecision>(
    *current.fetchDecision);
  stale.fetchDecision->window = 2;
  BOOST_CHECK(!snapshot.observe(7, 6, stale, 123999));
  VideoAdaptiveState afterStale;
  snapshot.applyTo(afterStale);
  BOOST_CHECK_EQUAL(afterStale.window, 41);
  BOOST_CHECK_EQUAL(afterStale.coreFetchDecisionObservedAtMs, 123456);

  ndn_service_framework::LiveStreamStatus noDecision;
  noDecision.state = ndn_service_framework::LiveStreamLifecycleState::Active;
  BOOST_CHECK(snapshot.observe(7, 7, noDecision, 124000));
  VideoAdaptiveState cleared;
  snapshot.applyTo(cleared);
  BOOST_CHECK(!cleared.coreFetchDecisionAvailable);
  BOOST_CHECK_EQUAL(cleared.coreFetchDecisionSource, "unavailable");
  BOOST_CHECK_EQUAL(cleared.window, 0);

  auto stopped = current;
  stopped.state = ndn_service_framework::LiveStreamLifecycleState::Stopped;
  BOOST_CHECK(snapshot.observe(7, 7, stopped, 124050));
  VideoAdaptiveState afterStop;
  snapshot.applyTo(afterStop);
  BOOST_CHECK(!afterStop.coreFetchDecisionAvailable);
  BOOST_CHECK_EQUAL(afterStop.window, 0);

  snapshot.reset(8);
  BOOST_CHECK(!snapshot.observe(8, 7, current, 124100));
  VideoAdaptiveState nextGeneration;
  snapshot.applyTo(nextGeneration);
  BOOST_CHECK_EQUAL(nextGeneration.coreFetchDecisionGeneration, 8);
  BOOST_CHECK(!nextGeneration.coreFetchDecisionAvailable);
}

BOOST_AUTO_TEST_CASE(VideoControlStateDerivesStartStopActions)
{
  const auto idle = VideoControlState::fromStates("A", std::nullopt, false);
  BOOST_CHECK_EQUAL(idle.selectedDrone, "A");
  BOOST_CHECK(!idle.remoteStreaming);
  BOOST_CHECK(!idle.displayActive);
  BOOST_CHECK(idle.canStart);
  BOOST_CHECK(!idle.canStop);
  BOOST_CHECK_NE(idle.statusLine().find("can_start=true"), std::string::npos);

  VideoState streaming;
  streaming.droneId = "A";
  streaming.status = "streaming";
  const auto remoteStreaming = VideoControlState::fromStates("A", streaming, false);
  BOOST_CHECK(remoteStreaming.remoteStreaming);
  BOOST_CHECK(!remoteStreaming.displayActive);
  BOOST_CHECK(!remoteStreaming.canStart);
  BOOST_CHECK(remoteStreaming.canStop);

  VideoState stopped;
  stopped.droneId = "A";
  stopped.status = "stopped";
  const auto localDisplay = VideoControlState::fromStates("A", stopped, true);
  BOOST_CHECK(!localDisplay.remoteStreaming);
  BOOST_CHECK(localDisplay.displayActive);
  BOOST_CHECK(!localDisplay.canStart);
  BOOST_CHECK(localDisplay.canStop);
}

BOOST_AUTO_TEST_CASE(UavStatesMapToCoreServiceOperationStatus)
{
  FlightCommandState command;
  command.droneId = "A";
  command.command = "takeoff";
  command.accepted = "true";
  command.ackResult = "accepted";
  command.updatedMs = 1000;

  auto status = toServiceOperationStatus(
    command, ndn::Name("/UAV/FlightCommand"), ndn::Name("/uav/drone/A"),
    ndn::Name("/request/flight/1"));
  BOOST_CHECK_EQUAL(status.operation, "UAV_FLIGHT_COMMAND");
  BOOST_CHECK_EQUAL(status.operationId, "A:takeoff");
  BOOST_CHECK_EQUAL(status.state, "DONE");
  BOOST_CHECK_EQUAL(status.reasonCode, "OK");
  BOOST_CHECK_CLOSE(status.progress, 1.0, 0.001);
  BOOST_CHECK(status.serviceName == ndn::Name("/UAV/FlightCommand"));
  BOOST_CHECK(status.providerName == ndn::Name("/uav/drone/A"));

  const auto commandPayload =
    ndn_service_framework::ServiceProvider::makeServiceOperationStatusPayload(status);
  const auto parsed =
    ndn_service_framework::ServiceProvider::parseServiceOperationStatusPayload(commandPayload);
  BOOST_REQUIRE(parsed);
  BOOST_CHECK_EQUAL(parsed->operation, "UAV_FLIGHT_COMMAND");
  BOOST_CHECK_EQUAL(parsed->state, "DONE");

  MissionState mission = makeMissionState("executing");
  mission.updatedMs = 2000;
  status = toServiceOperationStatus(
    mission, ndn::Name("/UAV/MissionAssign"), ndn::Name("/uav/drone/A"));
  BOOST_CHECK_EQUAL(status.operation, "UAV_MISSION_PART");
  BOOST_CHECK_EQUAL(status.state, "RUNNING");
  BOOST_CHECK_EQUAL(status.reasonCode, "executing");
  BOOST_CHECK_CLOSE(status.progress, 0.75, 0.001);

  mission = makeMissionState("uploaded");
  status = toServiceOperationStatus(mission);
  BOOST_CHECK_EQUAL(status.state, "WAITING_INPUT");
  BOOST_CHECK_CLOSE(status.progress, 0.35, 0.001);

  MissionProgressState progress;
  progress.taskId = "mission-test";
  progress.phase = "executing";
  progress.totalParts = 4;
  progress.completedParts = 3;
  status = toServiceOperationStatus(progress, ndn::Name("/UAV/MissionProgress"));
  BOOST_CHECK_EQUAL(status.operation, "UAV_MISSION");
  BOOST_CHECK_EQUAL(status.state, "RUNNING");
  BOOST_CHECK_CLOSE(status.progress, 0.75, 0.001);

  progress.phase = "waiting-compensation";
  status = toServiceOperationStatus(progress);
  BOOST_CHECK_EQUAL(status.state, "WAITING_INPUT");
}

BOOST_AUTO_TEST_CASE(UavRecordingStatusCarriesCoreDataProductReference)
{
  RecordingDataProductState recording;
  recording.droneId = "A";
  recording.sessionId = "session-7";
  recording.objectPrefix = "/uav/A/recording";
  recording.chunks = 12;
  recording.bytes = 4096;
  recording.updatedMs = 3000;

  const auto reference = recording.toDataProductReference(
    ndn::Name("/UAV/RecordingManifest"), ndn::Name("/uav/drone/A"));
  BOOST_CHECK(reference.name == ndn::Name("/uav/A/recording/session-7"));
  BOOST_CHECK_EQUAL(reference.objectClass, "camera-recording");
  BOOST_CHECK_EQUAL(reference.contentType, "video/h264");
  BOOST_CHECK_EQUAL(reference.segmentCount, 12);

  const auto status = toServiceOperationStatus(
    recording, ndn::Name("/UAV/RecordingManifest"), ndn::Name("/uav/drone/A"));
  BOOST_CHECK_EQUAL(status.operation, "UAV_RECORDING");
  BOOST_CHECK_EQUAL(status.state, "DONE");
  BOOST_REQUIRE(status.resultReference);
  BOOST_CHECK(status.resultReference->name == ndn::Name("/uav/A/recording/session-7"));

  const auto payload =
    ndn_service_framework::ServiceProvider::makeServiceOperationStatusPayload(status);
  const auto parsed =
    ndn_service_framework::ServiceProvider::parseServiceOperationStatusPayload(payload);
  BOOST_REQUIRE(parsed);
  BOOST_REQUIRE(parsed->resultReference);
  BOOST_CHECK_EQUAL(parsed->resultReference->objectClass, "camera-recording");
  BOOST_CHECK_EQUAL(parsed->resultReference->segmentCount, 12);
}

BOOST_AUTO_TEST_CASE(VideoAdaptivePolicyShrinksUnderPressure)
{
  VideoAdaptivePolicyInput base;
  base.rttMs = 120;
  base.fps = 30;
  base.deltaPacketsPerSecond = 180;
  base.timeoutBudgetMs = 2500;
  base.dynamicWindowMax = 180;
  base.dynamicLookaheadMax = 80;
  base.decoderBacklogLimit = 80;
  base.receivedChunks = 1000;
  base.acceptedBitrateKbps = 8000;
  base.requestedBitrateKbps = 8000;

  auto pressured = base;
  pressured.timeoutPressure = 95;
  pressured.probePressure = 80;
  pressured.decoderPendingChunks = 120;
  pressured.timeouts = 120;
  pressured.nacks = 20;
  pressured.receivedChunks = 200;

  const auto relaxed = computeVideoAdaptivePolicy(base);
  const auto stressed = computeVideoAdaptivePolicy(pressured);

  BOOST_CHECK_LT(stressed.window, relaxed.window);
  BOOST_CHECK_LT(stressed.lookahead, relaxed.lookahead);
  BOOST_CHECK_LE(stressed.missingTimeoutMs, relaxed.missingTimeoutMs);
  BOOST_CHECK_EQUAL(stressed.bitrateAction, "decrease");
  BOOST_CHECK_EQUAL(stressed.bitrateReason, "pressure");
  BOOST_CHECK_EQUAL(stressed.primaryPressure, "backlog");
  BOOST_CHECK_EQUAL(stressed.policyReason, "pressure-backlog");
  BOOST_CHECK_LT(stressed.suggestedBitrateKbps, pressured.acceptedBitrateKbps);
}

BOOST_AUTO_TEST_CASE(VideoAdaptivePolicyHandlesHighRttAndRecovery)
{
  VideoAdaptivePolicyInput highRtt;
  highRtt.rttMs = 950;
  highRtt.fps = 30;
  highRtt.deltaPacketsPerSecond = 180;
  highRtt.timeoutBudgetMs = 2500;
  highRtt.dynamicWindowMax = 180;
  highRtt.dynamicLookaheadMax = 80;
  highRtt.decoderBacklogLimit = 80;
  highRtt.receivedChunks = 1000;
  highRtt.acceptedBitrateKbps = 6000;
  highRtt.requestedBitrateKbps = 8000;

  const auto slowLink = computeVideoAdaptivePolicy(highRtt);
  BOOST_CHECK_EQUAL(slowLink.bitrateAction, "decrease");
  BOOST_CHECK_EQUAL(slowLink.bitrateReason, "high-rtt");
  BOOST_CHECK_EQUAL(slowLink.policyReason, "high-rtt");
  BOOST_CHECK_LT(slowLink.suggestedBitrateKbps, highRtt.acceptedBitrateKbps);

  auto recovering = highRtt;
  recovering.rttMs = 120;
  recovering.acceptedBitrateKbps = 2500;
  recovering.requestedBitrateKbps = 8000;

  const auto recovered = computeVideoAdaptivePolicy(recovering);
  BOOST_CHECK_EQUAL(recovered.bitrateAction, "increase");
  BOOST_CHECK_EQUAL(recovered.bitrateReason, "recovery");
  BOOST_CHECK_EQUAL(recovered.policyReason, "recovery");
  BOOST_CHECK_GT(recovered.suggestedBitrateKbps, recovering.acceptedBitrateKbps);
}

BOOST_AUTO_TEST_CASE(VideoAdaptivePolicyIdentifiesPressureProfiles)
{
  VideoAdaptivePolicyInput base;
  base.rttMs = 120;
  base.fps = 30;
  base.deltaPacketsPerSecond = 180;
  base.timeoutBudgetMs = 2500;
  base.dynamicWindowMax = 180;
  base.dynamicLookaheadMax = 80;
  base.decoderBacklogLimit = 80;
  base.receivedChunks = 1000;
  base.acceptedBitrateKbps = 8000;
  base.requestedBitrateKbps = 8000;

  auto timeout = base;
  timeout.timeoutPressure = 90;
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(timeout).primaryPressure, "timeout");
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(timeout).policyReason, "pressure-timeout");

  auto loss = base;
  loss.timeouts = 120;
  loss.nacks = 80;
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(loss).primaryPressure, "loss");
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(loss).policyReason, "pressure-loss");

  auto duplicate = base;
  duplicate.duplicatePressure = 180;
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(duplicate).primaryPressure, "duplicate");
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(duplicate).policyReason, "pressure-duplicate");

  auto backlog = base;
  backlog.decoderPendingChunks = 120;
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(backlog).primaryPressure, "backlog");
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(backlog).policyReason, "pressure-backlog");

  auto probe = base;
  probe.probePressure = 90;
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(probe).primaryPressure, "probe");
  BOOST_CHECK_EQUAL(computeVideoAdaptivePolicy(probe).policyReason, "pressure-probe");

  auto decodeGap = base;
  decodeGap.publishedFrames = 120;
  decodeGap.decodedFrames = 10;
  const auto decodeGapDecision = computeVideoAdaptivePolicy(decodeGap);
  BOOST_CHECK_EQUAL(decodeGapDecision.primaryPressure, "decode-gap");
  BOOST_CHECK_EQUAL(decodeGapDecision.policyReason, "pressure-decode-gap");
  BOOST_CHECK_EQUAL(decodeGapDecision.bitrateAction, "decrease");
  BOOST_CHECK_GT(decodeGapDecision.frameGapPressure, 0);
  BOOST_CHECK_LT(decodeGapDecision.suggestedBitrateKbps, decodeGap.acceptedBitrateKbps);
}

BOOST_AUTO_TEST_CASE(SelectedDroneSummaryStateUsesSharedModels)
{
  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.readiness = "ready";
  telemetry.video = "streaming";
  telemetry.linkState = "connected";

  auto readiness = makeReadyState(true);
  auto mission = makeMissionState("uploaded");

  MissionPlan plan;
  plan.taskId = "patrol-test";
  plan.assignment = "clustered-waypoints-return-to-start";

  MissionPart part;
  part.id = "part-A";
  part.assignedDrone = "A";
  part.waypoints = {{35.1186, -89.9375}, {35.1187, -89.9374}};
  plan.parts.push_back(part);

  MissionProgressState progress;
  progress.phase = "executing";
  progress.drones = "A,B";

  VideoState video;
  video.droneId = "A";
  video.status = "streaming";
  video.cameraAvailable = "true";
  video.cameraReason = "ok";

  VideoAdaptiveState adaptive;
  adaptive.droneId = "A";
  adaptive.rttMs = 105;
  adaptive.backlogPressure = 42;
  adaptive.primaryPressure = "backlog";

  const auto summary = SelectedDroneSummaryState::fromStates("A", telemetry, readiness,
                                                            mission, plan, part, progress,
                                                            video, adaptive, makeSafeState());
  BOOST_CHECK(summary.hasTelemetry);
  BOOST_CHECK_EQUAL(summary.selectedDrone, "A");
  BOOST_CHECK_EQUAL(summary.readiness, "ready");
  BOOST_CHECK_EQUAL(summary.missionPhase, "uploaded");
  BOOST_CHECK_EQUAL(summary.missionProgressPhase, "executing");
  BOOST_CHECK_EQUAL(summary.missionPlanTask, "patrol-test");
  BOOST_CHECK_EQUAL(summary.missionPartId, "part-A");
  BOOST_CHECK_EQUAL(summary.missionPartWaypoints, 2);
  BOOST_CHECK_EQUAL(summary.videoStatus, "streaming");
  BOOST_CHECK_EQUAL(summary.linkState, "connected");
  BOOST_CHECK(!summary.safetyAttention);
  BOOST_CHECK(!summary.canArm);
  BOOST_CHECK(summary.canTakeoff);
  BOOST_CHECK(summary.canManualControl);
  BOOST_CHECK_NE(summary.statusLine().find("mission_part=part-A"), std::string::npos);
  BOOST_CHECK_NE(summary.statusLine().find("video_adaptive=rtt=105ms"), std::string::npos);

  const auto empty = SelectedDroneSummaryState::fromStates("B", std::nullopt, std::nullopt,
                                                          std::nullopt, plan, std::nullopt,
                                                          std::nullopt, std::nullopt,
                                                          std::nullopt, std::nullopt);
  BOOST_CHECK(!empty.hasTelemetry);
  BOOST_CHECK_EQUAL(empty.readiness, "unknown");
  BOOST_CHECK_EQUAL(empty.missionPhase, "idle");
  BOOST_CHECK_EQUAL(empty.missionPlanTask, "patrol-test");
  BOOST_CHECK_EQUAL(empty.missionPartId, "none");
  BOOST_CHECK(!empty.canArm);
  BOOST_CHECK_EQUAL(empty.armReason, "no-telemetry");
}

BOOST_AUTO_TEST_CASE(UavFunctionalityStateTracksImplementedAndMissingCapabilities)
{
  MissionPlan plan;
  plan.taskId = "patrol-functionality";
  MissionPart part;
  part.id = "part-A";
  part.assignedDrone = "A";
  part.waypoints = {{35.1186, -89.9375}};
  plan.parts.push_back(part);

  RecordingDataProductState recording;
  recording.droneId = "A";
  recording.sessionId = "record-1";
  recording.objectPrefix = "/example/uav/drone/A/repo/camera/recording";
  recording.chunks = 3;
  recording.bytes = 1024;

  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.flightControllerBackend = "udp";
  telemetry.flightControllerState = "ready";
  telemetry.systemStatusName = "active";
  telemetry.batteryVoltageV = "12.1";

  const auto functionality = UavFunctionalityState::fromStates(plan, part, recording,
                                                               telemetry, true, 3);
  BOOST_CHECK_EQUAL(functionality.missionEditor, "prototype");
  BOOST_CHECK_EQUAL(functionality.perDroneMissionReview, "available");
  BOOST_CHECK_EQUAL(functionality.persistentMissionFiles, "available");
  BOOST_CHECK_EQUAL(functionality.recordingLogBrowsing, "available");
  BOOST_CHECK_EQUAL(functionality.parameterStatusInspection, "limited");
  BOOST_CHECK_EQUAL(functionality.objectDetectionDisplay, "metadata-only");
  BOOST_CHECK_EQUAL(functionality.multiDroneServiceSelection, "available");
  BOOST_CHECK_EQUAL(functionality.implementedCapabilityCount(), 7);
  BOOST_CHECK_EQUAL(functionality.missingOrLimitedCapabilities().find("persistent-mission-files"),
                    std::string::npos);
  BOOST_CHECK_NE(functionality.statusLine().find("mission_editor=prototype"), std::string::npos);

  const auto roundTrip = UavFunctionalityState::fromFields(functionality.toFields());
  BOOST_CHECK_EQUAL(roundTrip.recordingLogBrowsing, functionality.recordingLogBrowsing);
  BOOST_CHECK_EQUAL(roundTrip.multiDroneServiceSelection, functionality.multiDroneServiceSelection);

  const auto empty = UavFunctionalityState::fromStates(std::nullopt, std::nullopt,
                                                       std::nullopt, std::nullopt, false, 1);
  BOOST_CHECK_EQUAL(empty.implementedCapabilityCount(), 0);
  BOOST_CHECK_NE(empty.missingOrLimitedCapabilities().find("mission-editor=missing"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(UavPracticalityStateTracksDeploymentUsability)
{
  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.cameraAvailable = "true";
  telemetry.cameraSource = "/dev/video0";
  telemetry.cameraReason = "ok";
  telemetry.flightControllerBackend = "udp";
  telemetry.flightControllerAvailable = "true";
  telemetry.flightControllerReason = "ok";

  const auto practicality = UavPracticalityState::fromStates(telemetry, makeReadyState(true),
                                                             true, true, true);
  BOOST_CHECK_EQUAL(practicality.preflightSummary, "available");
  BOOST_CHECK_EQUAL(practicality.hardwareCompatibilityNotes, "documented");
  BOOST_CHECK_EQUAL(practicality.cameraDiagnostics, "available");
  BOOST_CHECK_EQUAL(practicality.flightControllerDiagnostics, "available");
  BOOST_CHECK_EQUAL(practicality.configValidation, "available");
  BOOST_CHECK_EQUAL(practicality.identityCertificateGuidance, "documented");
  BOOST_CHECK_EQUAL(practicality.operatorWorkflowGuidance, "documented");
  BOOST_CHECK_EQUAL(practicality.practicalCapabilityCount(), 7);
  BOOST_CHECK_NE(practicality.missingOrLimitedCapabilities().find("hardware-notes=documented"),
                 std::string::npos);
  BOOST_CHECK_NE(practicality.statusLine().find("camera_diagnostics=available"), std::string::npos);

  const auto roundTrip = UavPracticalityState::fromFields(practicality.toFields());
  BOOST_CHECK_EQUAL(roundTrip.preflightSummary, practicality.preflightSummary);
  BOOST_CHECK_EQUAL(roundTrip.operatorWorkflowGuidance, practicality.operatorWorkflowGuidance);

  TelemetryState unavailableCamera;
  unavailableCamera.cameraAvailable = "false";
  unavailableCamera.cameraReason = "device-not-opened";
  const auto weak = UavPracticalityState::fromStates(unavailableCamera, std::nullopt,
                                                     false, false, false);
  BOOST_CHECK_EQUAL(weak.preflightSummary, "missing");
  BOOST_CHECK_EQUAL(weak.cameraDiagnostics, "limited");
  BOOST_CHECK_EQUAL(weak.flightControllerDiagnostics, "missing");
  BOOST_CHECK_NE(weak.missingOrLimitedCapabilities().find("preflight-summary=missing"),
                 std::string::npos);
}

BOOST_AUTO_TEST_CASE(UavStabilityStateTracksTransportAndControlGuards)
{
  FlightCommandState command;
  command.droneId = "A";
  command.command = "land";
  command.accepted = "false";
  command.ackResult = "timeout";
  command.timeoutMs = 2500;

  VideoState video;
  video.droneId = "A";
  video.status = "streaming";
  video.streamId = "live|A|42";
  video.framesPublished = 12;

  VideoAdaptiveState adaptive;
  adaptive.droneId = "A";
  adaptive.timeoutPressure = 40;
  adaptive.backlogPressure = 20;
  adaptive.primaryPressure = "timeout";

  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.telemetryFreshness = "stale";

  SafetyState safety;
  safety.droneId = "A";
  safety.manualNeutralSent = "true";
  safety.manualControlState = "stale-neutral";

  const auto stability = UavStabilityState::fromStates(command, video, adaptive,
                                                       telemetry, safety, true, true);
  BOOST_CHECK_EQUAL(stability.commandTimeoutHandling, "operator-decision");
  BOOST_CHECK_EQUAL(stability.stopVideoIdempotence, "available");
  BOOST_CHECK_EQUAL(stability.streamSessionGuard, "available");
  BOOST_CHECK_EQUAL(stability.frameSequenceGuard, "available");
  BOOST_CHECK_EQUAL(stability.adaptiveVideoPressure, "active");
  BOOST_CHECK_EQUAL(stability.telemetryFreshness, "stale");
  BOOST_CHECK_EQUAL(stability.manualNeutralFallback, "available");
  BOOST_CHECK_EQUAL(stability.longDurationProfiles, "documented");
  BOOST_CHECK_EQUAL(stability.stableCapabilityCount(), 8);
  BOOST_CHECK_NE(stability.missingOrLimitedCapabilities().find("command-timeout=operator-decision"),
                 std::string::npos);
  BOOST_CHECK_NE(stability.statusLine().find("adaptive_video=active"), std::string::npos);

  const auto roundTrip = UavStabilityState::fromFields(stability.toFields());
  BOOST_CHECK_EQUAL(roundTrip.streamSessionGuard, stability.streamSessionGuard);
  BOOST_CHECK_EQUAL(roundTrip.telemetryFreshness, stability.telemetryFreshness);

  const auto empty = UavStabilityState::fromStates(std::nullopt, std::nullopt,
                                                   std::nullopt, std::nullopt,
                                                   std::nullopt, false, false);
  BOOST_CHECK_EQUAL(empty.stableCapabilityCount(), 0);
  BOOST_CHECK_NE(empty.missingOrLimitedCapabilities().find("stop-video=missing"),
                 std::string::npos);
}

BOOST_AUTO_TEST_CASE(UavMissionPlanDocumentSupportsPersistentOperationalPlan)
{
  auto plan = buildPatrolMissionPlan("patrol-v2", 35.1186, -89.9375, 120.0, {"A", "B"});
  auto document = MissionPlanDocument::fromPlan(plan, "plan-001", "Memphis patrol", "operator-1", 1000);
  document.geofence = {{35.1180, -89.9380}, {35.1190, -89.9380}, {35.1190, -89.9370}};
  document.rallyPoints = {{35.1185, -89.9375}};
  document.metadata["source"] = "unit-test";

  BOOST_CHECK(document.isSaveable());
  BOOST_CHECK(document.hasFenceOrRally());
  BOOST_CHECK_EQUAL(document.plan.parts.size(), 2);

  const auto fields = document.toFields();
  const auto roundTrip = MissionPlanDocument::fromFields(fields);
  BOOST_CHECK_EQUAL(roundTrip.schema, "ndnsf-uav-mission-plan-v2");
  BOOST_CHECK_EQUAL(roundTrip.planId, "plan-001");
  BOOST_CHECK_EQUAL(roundTrip.displayName, "Memphis patrol");
  BOOST_CHECK_EQUAL(roundTrip.operatorId, "operator-1");
  BOOST_CHECK_EQUAL(roundTrip.plan.taskId, "patrol-v2");
  BOOST_CHECK_EQUAL(roundTrip.plan.parts.size(), document.plan.parts.size());
  BOOST_CHECK_EQUAL(roundTrip.geofence.size(), 3);
  BOOST_CHECK_EQUAL(roundTrip.rallyPoints.size(), 1);
  BOOST_CHECK_EQUAL(roundTrip.metadata.at("source"), "unit-test");
  BOOST_CHECK_NE(roundTrip.statusLine().find("saveable=true"), std::string::npos);

  const auto path = std::string("/tmp/ndnsf-uav-mission-plan-document-test.conf");
  saveMissionPlanDocument(document, path);
  const auto loaded = loadMissionPlanDocument(path);
  BOOST_CHECK_EQUAL(loaded.planId, document.planId);
  BOOST_CHECK_EQUAL(loaded.plan.parts.size(), document.plan.parts.size());
  BOOST_CHECK_EQUAL(loaded.geofence.size(), document.geofence.size());
  BOOST_CHECK_EQUAL(loaded.rallyPoints.size(), document.rallyPoints.size());
  BOOST_CHECK_EQUAL(loaded.metadata.at("source"), "unit-test");
  std::remove(path.c_str());
}

BOOST_AUTO_TEST_CASE(UavDataProductCatalogSummarizesQueryableProducts)
{
  RecordingDataProductState recording;
  recording.droneId = "A";
  recording.productType = "camera-recording";
  recording.sessionId = "record-42";
  recording.objectPrefix = "/example/uav/drone/A/repo/camera/recording/42";
  recording.chunks = 4;
  recording.bytes = 4096;
  recording.updatedMs = 12345;

  auto catalog = UavDataProductCatalogState::fromRecording(recording);
  catalog.telemetryLogProducts = 2;
  catalog.detectionProducts = 1;
  catalog.repoObjects = 4;
  catalog.sourceRepo = "/example/uav/drone/A/local-repo";
  BOOST_CHECK(catalog.hasQueryableProducts());
  BOOST_CHECK_EQUAL(catalog.totalProducts(), 4);
  BOOST_CHECK_EQUAL(catalog.repoObjects, 4);
  BOOST_CHECK_EQUAL(catalog.totalBytes, 4096);
  BOOST_CHECK_EQUAL(catalog.latestObjectPrefix, recording.objectPrefix);

  const auto roundTrip = UavDataProductCatalogState::fromFields(catalog.toFields());
  BOOST_CHECK_EQUAL(roundTrip.totalProducts(), 4);
  BOOST_CHECK_EQUAL(roundTrip.repoObjects, 4);
  BOOST_CHECK_EQUAL(roundTrip.sourceRepo, "/example/uav/drone/A/local-repo");
  BOOST_CHECK_EQUAL(roundTrip.telemetryLogProducts, 2);
  BOOST_CHECK_NE(roundTrip.statusLine().find("detections=1"), std::string::npos);

  std::vector<Fields> repoEntries{
    {{"object_name", "/example/uav/drone/A/repo/camera/recording/record-1/chunk/0"},
     {"object_type", "video/h264-chunk"}, {"size", "1000"}, {"updated_ms", "10"}},
    {{"object_name", "/example/uav/drone/A/repo/camera/recording/record-1/chunk/1"},
     {"object_type", "video/h264-chunk"}, {"size", "1200"}, {"updated_ms", "11"}},
    {{"object_name", "/example/uav/drone/A/repo/telemetry/log-1"},
     {"object_type", "telemetry-log"}, {"size", "300"}},
    {{"object_name", "/example/uav/drone/A/repo/detection/yolo-1"},
     {"object_type", "detection-log"}, {"size", "400"}},
    {{"object_name", "/example/uav/drone/A/repo/mission/mission-1"},
     {"object_type", "mission-log"}, {"size", "500"}},
  };
  const auto repoCatalog = UavDataProductCatalogState::fromCatalogProductFields(
    repoEntries, "/example/uav/drone/A/local-repo", 99);
  BOOST_CHECK_EQUAL(repoCatalog.repoObjects, 5);
  BOOST_CHECK_EQUAL(repoCatalog.recordingProducts, 1);
  BOOST_CHECK_EQUAL(repoCatalog.telemetryLogProducts, 1);
  BOOST_CHECK_EQUAL(repoCatalog.detectionProducts, 1);
  BOOST_CHECK_EQUAL(repoCatalog.missionLogProducts, 1);
  BOOST_CHECK_EQUAL(repoCatalog.totalBytes, 3400);
  BOOST_CHECK_EQUAL(repoCatalog.sourceRepo, "/example/uav/drone/A/local-repo");
  BOOST_CHECK(repoCatalog.hasQueryableProducts());
}

BOOST_AUTO_TEST_CASE(VehicleParameterSnapshotCarriesCapabilityView)
{
  VehicleParameterSnapshot snapshot;
  snapshot.droneId = "A";
  snapshot.source = "mavlink-param-cache";
  snapshot.firmware = "PX4-1.14";
  snapshot.vehicleType = "quadrotor";
  snapshot.flightModes = "MANUAL,POSCTL,AUTO.MISSION";
  snapshot.completePercent = 80;
  snapshot.parameters["NAV_RCL_ACT"] = "2";
  snapshot.parameters["COM_RC_LOSS_T"] = "5";

  BOOST_CHECK(snapshot.isUsable());
  const auto fields = snapshot.toFields();
  const auto roundTrip = VehicleParameterSnapshot::fromFields(fields);
  BOOST_CHECK_EQUAL(roundTrip.parameterCount, 2);
  BOOST_CHECK_EQUAL(roundTrip.parameters.at("NAV_RCL_ACT"), "2");
  BOOST_CHECK_EQUAL(roundTrip.firmware, "PX4-1.14");
  BOOST_CHECK_NE(roundTrip.statusLine().find("usable=true"), std::string::npos);

  const auto compact = VehicleParameterSnapshot::fromFields(snapshot.toFields(false));
  BOOST_CHECK_EQUAL(compact.parameterCount, 2);
  BOOST_CHECK(compact.parameters.empty());
  BOOST_CHECK(compact.isUsable());
}

BOOST_AUTO_TEST_CASE(VehicleParameterEditContractsRoundTripAndValidate)
{
  VehicleParameterEditRequest request;
  request.requestId = "param-req-1";
  request.operatorId = "operator-1";
  request.droneId = "A";
  request.parameterName = "NAV_RCL_ACT";
  request.expectedValue = "2";
  request.requestedValue = "1";
  request.valueType = "MAV_PARAM_TYPE_INT32";
  request.targetSystem = 7;
  request.targetComponent = 1;
  request.requestedMs = 4567;

  std::string reason;
  BOOST_CHECK(request.isValid(reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  const auto requestRoundTrip = VehicleParameterEditRequest::fromFields(request.toFields());
  BOOST_CHECK_EQUAL(requestRoundTrip.requestId, "param-req-1");
  BOOST_CHECK_EQUAL(requestRoundTrip.parameterName, "NAV_RCL_ACT");
  BOOST_CHECK_EQUAL(requestRoundTrip.requestedValue, "1");
  BOOST_CHECK_EQUAL(requestRoundTrip.targetSystem, 7);
  BOOST_CHECK_NE(requestRoundTrip.statusLine().find("valid=true"), std::string::npos);

  auto invalid = request;
  invalid.parameterName = "THIS_PARAM_NAME_IS_TOO_LONG";
  BOOST_CHECK(!invalid.isValid(reason));
  BOOST_CHECK_EQUAL(reason, "parameter-name-too-long");

  VehicleParameterEditResult result;
  result.requestId = request.requestId;
  result.droneId = request.droneId;
  result.parameterName = request.parameterName;
  result.valueType = request.valueType;
  result.accepted = true;
  result.applied = true;
  result.verified = true;
  result.reason = "ok";
  result.previousValue = "2";
  result.requestedValue = "1";
  result.verifiedValue = "1";
  result.updatedMs = 5000;

  BOOST_CHECK(result.successful());
  const auto resultRoundTrip = VehicleParameterEditResult::fromFields(result.toFields());
  BOOST_CHECK(resultRoundTrip.successful());
  BOOST_CHECK_EQUAL(resultRoundTrip.verifiedValue, "1");
  BOOST_CHECK_NE(resultRoundTrip.statusLine().find("verified=true"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(PreflightAndAnalyzeContractsSupportQgcStylePanels)
{
  PreflightCheckItem gps;
  gps.checkId = "gps-fix";
  gps.droneId = "A";
  gps.label = "GPS Fix";
  gps.category = "Sensors";
  gps.status = "fail";
  gps.reason = "waiting-for-3d-fix";
  gps.blocking = true;
  gps.order = 10;
  gps.updatedMs = 1000;

  BOOST_CHECK(gps.isBlockingFailure());
  const auto gpsRoundTrip = PreflightCheckItem::fromFields(gps.toFields());
  BOOST_CHECK(gpsRoundTrip.isBlockingFailure());
  BOOST_CHECK_EQUAL(gpsRoundTrip.label, "GPS Fix");
  BOOST_CHECK_NE(gpsRoundTrip.statusLine().find("blocking_failure=true"), std::string::npos);

  MavlinkMessageSummary heartbeat;
  heartbeat.messageName = "HEARTBEAT";
  heartbeat.messageId = 0;
  heartbeat.systemId = 1;
  heartbeat.componentId = 1;
  heartbeat.count = 120;
  heartbeat.rateHz = "1.0";
  heartbeat.lastSeenMs = 9000;

  MavlinkMessageSummary position;
  position.messageName = "GLOBAL_POSITION_INT";
  position.messageId = 33;
  position.systemId = 1;
  position.componentId = 1;
  position.count = 360;
  position.rateHz = "3.0";
  position.lastSeenMs = 3000;

  UavAnalyzeSnapshot snapshot;
  snapshot.droneId = "A";
  snapshot.linkState = "connected";
  snapshot.flightMode = "GUIDED";
  snapshot.missionPhase = "executing";
  snapshot.videoState = "streaming";
  snapshot.parameterCacheStatus = "complete";
  snapshot.updatedMs = 10000;
  snapshot.messages = {heartbeat, position};

  const auto roundTrip = UavAnalyzeSnapshot::fromFields(snapshot.toFields());
  BOOST_CHECK_EQUAL(roundTrip.messages.size(), 2);
  BOOST_CHECK_EQUAL(roundTrip.messages[0].messageName, "HEARTBEAT");
  BOOST_CHECK_EQUAL(roundTrip.messages[1].messageId, 33);
  BOOST_CHECK_EQUAL(roundTrip.activeMessageCount(10000, 3000), 1);
  BOOST_CHECK_NE(roundTrip.statusLine().find("messages=2"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(OperatorDashboardSnapshotSummarizesQgcStyleState)
{
  UavOperatorDashboardSnapshot snapshot;
  snapshot.droneId = "A";
  snapshot.telemetryFreshness = "fresh";
  snapshot.readiness = "ready";
  snapshot.readinessReason = "ok";
  snapshot.linkState = "connected";
  snapshot.flightMode = "AUTO.MISSION";
  snapshot.missionPhase = "executing";
  snapshot.videoState = "streaming";
  snapshot.parameterCacheStatus = "complete";
  snapshot.parameterCount = 42;
  snapshot.preflightTotal = 6;
  snapshot.preflightBlockingFailures = 0;
  snapshot.mavlinkMessageCount = 4;
  snapshot.activeMavlinkMessageCount = 3;
  snapshot.canArm = true;
  snapshot.canTakeoff = true;
  snapshot.canLand = true;
  snapshot.canManualControl = true;
  snapshot.canEmergencyStop = true;
  snapshot.updatedMs = 12345;

  BOOST_CHECK(snapshot.operatorReady());
  const auto roundTrip = UavOperatorDashboardSnapshot::fromFields(snapshot.toFields());
  BOOST_CHECK(roundTrip.operatorReady());
  BOOST_CHECK_EQUAL(roundTrip.droneId, "A");
  BOOST_CHECK_EQUAL(roundTrip.parameterCount, 42);
  BOOST_CHECK_EQUAL(roundTrip.preflightTotal, 6);
  BOOST_CHECK_EQUAL(roundTrip.activeMavlinkMessageCount, 3);
  BOOST_CHECK(roundTrip.canEmergencyStop);
  BOOST_CHECK_NE(roundTrip.statusLine().find("operator_ready=true"), std::string::npos);

  auto blocked = roundTrip;
  blocked.preflightBlockingFailures = 1;
  BOOST_CHECK(!blocked.operatorReady());
}

BOOST_AUTO_TEST_CASE(OperatorAuthorityLeaseBlocksConflictingControl)
{
  OperatorAuthorityLease lease;
  lease.leaseId = "lease-A";
  lease.operatorId = "operator-1";
  lease.droneId = "A";
  lease.scope = "control";
  lease.issuedMs = 1000;
  lease.expiresMs = 5000;

  std::string reason;
  BOOST_CHECK(lease.allowsCommand("A", "takeoff", 2000, reason));
  BOOST_CHECK_EQUAL(reason, "ok");
  BOOST_CHECK(!lease.allowsCommand("B", "takeoff", 2000, reason));
  BOOST_CHECK_EQUAL(reason, "wrong-drone");
  BOOST_CHECK(!lease.allowsCommand("A", "takeoff", 6000, reason));
  BOOST_CHECK_EQUAL(reason, "lease-expired");

  lease.scope = "monitor";
  BOOST_CHECK(lease.allowsCommand("A", "telemetry", 2000, reason));
  BOOST_CHECK(!lease.allowsCommand("A", "land", 2000, reason));
  BOOST_CHECK_EQUAL(reason, "monitor-scope");

  const auto roundTrip = OperatorAuthorityLease::fromFields(lease.toFields());
  BOOST_CHECK_EQUAL(roundTrip.leaseId, "lease-A");
  BOOST_CHECK_EQUAL(roundTrip.scope, "monitor");
  BOOST_CHECK_NE(roundTrip.statusLine().find("operator=operator-1"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(OperatorAuthorityLeaseRequestRoundTripsAndValidates)
{
  OperatorAuthorityLeaseRequest request;
  request.requestId = "req-1";
  request.operatorId = "operator-1";
  request.droneId = "A";
  request.scope = "mission";
  request.ttlMs = 45000;
  request.requestedMs = 1234;

  std::string reason;
  BOOST_CHECK(request.isValid(reason));
  BOOST_CHECK_EQUAL(reason, "ok");

  const auto fields = request.toFields();
  BOOST_CHECK_EQUAL(fields.at("type"), "operator-authority-lease-request");
  BOOST_CHECK_EQUAL(fields.at("lease_request_id"), "req-1");
  BOOST_CHECK_EQUAL(fields.at("lease_operator"), "operator-1");
  BOOST_CHECK_EQUAL(fields.at("lease_drone"), "A");
  BOOST_CHECK_EQUAL(fields.at("lease_scope"), "mission");
  BOOST_CHECK_EQUAL(fields.at("lease_ttl_ms"), "45000");
  BOOST_CHECK_EQUAL(fields.at("lease_requested_ms"), "1234");

  const auto roundTrip = OperatorAuthorityLeaseRequest::fromFields(fields);
  BOOST_CHECK_EQUAL(roundTrip.requestId, "req-1");
  BOOST_CHECK_EQUAL(roundTrip.operatorId, "operator-1");
  BOOST_CHECK_EQUAL(roundTrip.droneId, "A");
  BOOST_CHECK_EQUAL(roundTrip.scope, "mission");
  BOOST_CHECK_EQUAL(roundTrip.ttlMs, 45000);
  BOOST_CHECK_EQUAL(roundTrip.requestedMs, 1234);
  BOOST_CHECK_NE(roundTrip.statusLine().find("scope=mission"), std::string::npos);

  auto invalid = request;
  invalid.scope = "fly-anywhere";
  BOOST_CHECK(!invalid.isValid(reason));
  BOOST_CHECK_EQUAL(reason, "unsupported-scope");
}

BOOST_AUTO_TEST_CASE(DroneListRowStateUsesSharedTelemetryMissionAndVideoModels)
{
  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.batteryPercent = "87";
  telemetry.video = "streaming";
  telemetry.readiness = "ready";
  telemetry.armed = "true";
  telemetry.gpsFixName = "3d-fix";
  telemetry.flightControllerAvailable = "true";
  telemetry.flightControllerReady = "true";
  telemetry.cameraAvailable = "true";

  auto readiness = makeReadyState(true);
  auto mission = makeMissionState("executing");

  VideoState video;
  video.droneId = "A";
  video.status = "streaming";
  video.cameraAvailable = "true";
  video.cameraReason = "ok";

  VideoAdaptiveState adaptive;
  adaptive.droneId = "A";
  adaptive.rttMs = 115;
  adaptive.window = 36;
  adaptive.timeoutPressure = 30;
  adaptive.probePressure = 10;
  adaptive.backlogPressure = 55;
  adaptive.primaryPressure = "backlog";
  adaptive.acceptedBitrateKbps = 6000;
  adaptive.suggestedBitrateKbps = 4000;
  adaptive.bitrateAction = "decrease";
  adaptive.policyReason = "pressure-backlog";

  FlightCommandState command;
  command.droneId = "A";
  command.command = "takeoff";
  command.ackResult = "accepted";

  auto safety = makeSafeState();

  MissionProgressState progress;
  progress.phase = "executing";
  progress.drones = "A,B";

  BOOST_CHECK(progress.appliesToDrone("A"));
  BOOST_CHECK(progress.appliesToDrone("B"));
  BOOST_CHECK(!progress.appliesToDrone("C"));
  BOOST_CHECK_EQUAL(adaptive.maxPressure(), 55);
  BOOST_CHECK_NE(adaptive.compactSummary().find("pressure=55/backlog"), std::string::npos);

  const auto row = DroneListRowState::fromStates("A", true, telemetry, readiness,
                                                 mission, video, adaptive, command,
                                                 safety, progress);
  BOOST_CHECK(row.selected);
  BOOST_CHECK(row.hasTelemetry);
  BOOST_CHECK(row.hasReadiness);
  BOOST_CHECK(row.hasMission);
  BOOST_CHECK(row.hasMissionProgress);
  BOOST_CHECK(row.hasVideo);
  BOOST_CHECK(row.hasVideoAdaptive);
  BOOST_CHECK(row.hasCommand);
  BOOST_CHECK(row.hasSafety);
  BOOST_CHECK_EQUAL(row.readiness, "ready");
  BOOST_CHECK_EQUAL(row.armed, "true");
  BOOST_CHECK_EQUAL(row.gps, "true");
  BOOST_CHECK_EQUAL(row.battery, "87%");
  BOOST_CHECK_EQUAL(row.mission, "executing");
  BOOST_CHECK_EQUAL(row.missionProgress, "executing");
  BOOST_CHECK_EQUAL(row.video, "streaming");
  BOOST_CHECK_NE(row.rowText.find("Drone A active"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("fc=true/true"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("cam=true"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("progress=executing"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("adaptive=rtt=115ms"), std::string::npos);

  const auto unrelatedRow = DroneListRowState::fromStates("C", false, std::nullopt,
                                                          std::nullopt, std::nullopt,
                                                          std::nullopt, std::nullopt,
                                                          std::nullopt, std::nullopt,
                                                          progress);
  BOOST_CHECK(!unrelatedRow.hasMissionProgress);
  BOOST_CHECK_EQUAL(unrelatedRow.missionProgress, "idle");
  BOOST_CHECK_NE(unrelatedRow.rowText.find("Drone C standby"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(DroneListAvailabilitySummaryShowsSubsystems)
{
  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.telemetryFreshness = "fresh";
  telemetry.readiness = "ready";
  telemetry.flightControllerAvailable = "true";
  telemetry.flightControllerReady = "true";
  telemetry.flightControllerBackend = "udp";
  telemetry.cameraAvailable = "true";
  telemetry.cameraSource = "/dev/video0";
  telemetry.cameraReason = "ok";
  telemetry.video = "streaming";
  telemetry.capture = "active";
  telemetry.recording = "recording";

  VideoState video;
  video.droneId = "A";
  video.status = "streaming";
  video.capture = "active";
  video.recording = "recording";
  video.cameraAvailable = "true";
  video.cameraReason = "ok";
  video.recordingChunks = 5;
  video.recordingBytes = 1234;

  const auto row = DroneListRowState::fromStates(
    "A", true, telemetry, makeReadyState(true), makeMissionState("executing"),
    video, std::nullopt, std::nullopt, makeSafeState(), std::nullopt,
    "available", "available", "available", "recording", "stored");

  BOOST_CHECK(row.hasTelemetry);
  BOOST_CHECK(row.hasReadiness);
  BOOST_CHECK(row.hasMission);
  BOOST_CHECK(row.hasVideo);
  BOOST_CHECK_EQUAL(row.serviceCamera, "available");
  BOOST_CHECK_EQUAL(row.serviceMavlink, "available");
  BOOST_CHECK_EQUAL(row.serviceMission, "available");
  BOOST_CHECK_EQUAL(row.serviceRecording, "recording");
  BOOST_CHECK_EQUAL(row.serviceRepo, "stored");
  BOOST_CHECK_NE(row.rowText.find("fc=true/true"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("cam=true"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("mission=executing"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("video=streaming"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("recording=recording"), std::string::npos);
  BOOST_CHECK_NE(row.rowText.find("repo=stored"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(TelemetryFreshnessAndManualNeutralRegression)
{
  TelemetryState telemetry;
  telemetry.droneId = "A";
  telemetry.telemetryFreshness = "fresh";
  BOOST_CHECK(telemetry.telemetryIsFresh());
  BOOST_CHECK(!telemetry.telemetryIsStale());
  BOOST_CHECK(!telemetry.telemetryIsMissing());

  telemetry.telemetryFreshness = "stale";
  BOOST_CHECK(telemetry.telemetryIsStale());
  BOOST_CHECK_NE(telemetry.statusLine().find("freshness=stale"), std::string::npos);

  telemetry.telemetryFreshness = "missing";
  BOOST_CHECK(telemetry.telemetryIsMissing());

  SafetyState safety;
  safety.droneId = "A";
  safety.linkState = "connected";
  safety.manualControlState = "fresh";
  safety.manualReplayActive = "true";
  safety.manualNeutralSent = "false";
  safety.manualFreshForMs = 120;
  BOOST_CHECK(safety.manualControlFresh());
  BOOST_CHECK(!safety.needsOperatorAttention());

  safety.manualControlState = "stale-waiting-neutral";
  safety.manualReplayActive = "false";
  safety.manualNeutralSent = "true";
  safety.manualFreshForMs = 0;
  BOOST_CHECK(!safety.manualControlFresh());
  BOOST_CHECK(safety.needsOperatorAttention());
  BOOST_CHECK_NE(safety.statusLine().find("neutral_sent=true"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(FlightCommandLifecycleTimeoutAndRttAreVisible)
{
  Fields successFields{
    {"drone_id", "A"},
    {"command", "takeoff"},
    {"accepted", "true"},
    {"ack_result", "accepted"},
    {"fc_state", "ready"},
    {"rtt_ms", "87"},
    {"timeout_ms", "0"},
    {"detail", "success"},
  };
  const auto success = FlightCommandState::fromFields(successFields);
  BOOST_CHECK(success.isAccepted());
  BOOST_CHECK(!success.isTimeout());
  BOOST_CHECK(success.isSafetyCritical());
  BOOST_CHECK_EQUAL(success.rttMs, 87);
  BOOST_CHECK_NE(success.statusLine().find("command=takeoff"), std::string::npos);
  BOOST_CHECK_NE(success.statusLine().find("rtt_ms=87"), std::string::npos);

  Fields timeoutFields{
    {"drone_id", "A"},
    {"command", "land"},
    {"accepted", "false"},
    {"ack_result", "timeout"},
    {"timeout_ms", "2500"},
    {"detail", "operator-retry-required"},
  };
  const auto timeout = FlightCommandState::fromFields(timeoutFields);
  BOOST_CHECK(!timeout.isAccepted());
  BOOST_CHECK(timeout.isTimeout());
  BOOST_CHECK(timeout.isSafetyCritical());
  BOOST_CHECK_EQUAL(timeout.timeoutMs, 2500);
  BOOST_CHECK_NE(timeout.statusLine().find("ack=timeout"), std::string::npos);
  BOOST_CHECK_NE(timeout.statusLine().find("detail=operator-retry-required"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(MissionPartialRecoveryAndCancelStatesAreExplicit)
{
  MissionProgressState partial;
  partial.taskId = "patrol-test";
  partial.phase = "waiting-compensation";
  partial.drones = "A,B";
  partial.totalParts = 2;
  partial.completedParts = 1;
  partial.missingParts = 1;
  partial.completedPartIds = "part0";
  partial.missingPartIds = "part1";
  partial.pendingPartIds = "part1";

  BOOST_CHECK(partial.isActive());
  BOOST_CHECK(partial.needsCompensation());
  BOOST_CHECK_EQUAL(partial.segmentStateForPart("part0", "executing"), "DONE");
  BOOST_CHECK_EQUAL(partial.segmentStateForPart("part1", "executing"), "RUNNING");
  BOOST_CHECK_NE(partial.statusLine().find("missing_parts=1"), std::string::npos);

  MissionState cancelled = makeMissionState("cancelled");
  BOOST_CHECK(cancelled.isCancelled());
  BOOST_CHECK(cancelled.isTerminal());
  BOOST_CHECK_EQUAL(partial.segmentStateForPart("part1", cancelled.phase), "FAILED");
}

BOOST_AUTO_TEST_CASE(VideoPacketSessionMetadataRoundTrips)
{
  VideoPacket packet;
  packet.streamId = "live|A|1";
  packet.streamSessionEpoch = 42;
  packet.packetSeq = 7;
  packet.mediaSequence = 5;
  packet.frameSeq = 3;
  packet.frameFirstPacketSeq = 6;
  packet.frameLastPacketSeq = 8;
  packet.frameSegmentIndex = 1;
  packet.frameSegmentCount = 3;
  packet.encoding = "h264";
  packet.keyFrame = false;
  packet.payload = {0x01, 0x02, 0x03};

  const auto decoded = decodeVideoPacket(encodeVideoPacket(packet));
  BOOST_CHECK_EQUAL(decoded.streamId, packet.streamId);
  BOOST_CHECK_EQUAL(decoded.streamSessionEpoch, 42);
  BOOST_CHECK_EQUAL(decoded.packetSeq, 7);
  BOOST_CHECK_EQUAL(decoded.mediaSequence, 5);
  BOOST_CHECK_EQUAL(decoded.frameSeq, 3);
  BOOST_CHECK_EQUAL(decoded.frameFirstPacketSeq, 6);
  BOOST_CHECK_EQUAL(decoded.frameLastPacketSeq, 8);
  BOOST_CHECK_EQUAL(decoded.payload.size(), 3);

  VideoPacket oldSession = decoded;
  oldSession.streamSessionEpoch = 41;
  BOOST_CHECK_NE(oldSession.streamSessionEpoch, decoded.streamSessionEpoch);
}

BOOST_AUTO_TEST_CASE(VideoPacketMapsToCoreStreamChunkWithoutChangingWire)
{
  VideoPacket packet;
  packet.streamId = "live|A|2";
  packet.streamSessionEpoch = 77;
  packet.second = 1234;
  packet.packetSeq = 9;
  packet.mediaSequence = 17;
  packet.frameSeq = 4;
  packet.captureMs = 5555;
  packet.frameFirstPacketSeq = 8;
  packet.frameLastPacketSeq = 11;
  packet.bucketPacketCount = 12;
  packet.frameSegmentIndex = 1;
  packet.frameSegmentCount = 4;
  packet.encoding = "video/h264";
  packet.keyFrame = true;
  packet.fecDataShards = 3;
  packet.fecParityShards = 1;
  packet.fecSymbolIndex = 3;
  packet.fecSymbolCount = 4;
  packet.fecDataLengths = "100,101,102";
  packet.payload = {0x01, 0x02, 0x03, 0x04};

  const auto streamChunk = videoPacketToStreamChunk(packet);
  BOOST_CHECK_EQUAL(streamChunk.streamId, packet.streamId);
  BOOST_CHECK_EQUAL(streamChunk.sessionEpoch, packet.streamSessionEpoch);
  BOOST_CHECK_EQUAL(streamChunk.seq, packet.packetSeq);
  BOOST_CHECK_EQUAL(streamChunk.contentType, packet.encoding);
  BOOST_CHECK_EQUAL(streamChunk.frameId, packet.frameSeq);
  BOOST_CHECK_EQUAL(streamChunk.metadata.at("uav.second"), "1234");
  BOOST_CHECK_EQUAL(streamChunk.metadata.at("uav.bucket_packet_count"), "12");
  BOOST_CHECK_EQUAL(streamChunk.metadata.at("uav.media_sequence"), "17");
  BOOST_REQUIRE(streamChunk.fec);
  BOOST_CHECK_EQUAL(streamChunk.fec->scheme, "xor-parity");
  BOOST_CHECK(streamChunk.fec->repairSymbol);
  BOOST_CHECK_EQUAL(streamChunk.fec->dataLengths.size(), 3);

  const auto restored = streamChunkToVideoPacket(streamChunk);
  BOOST_CHECK_EQUAL(restored.streamId, packet.streamId);
  BOOST_CHECK_EQUAL(restored.streamSessionEpoch, packet.streamSessionEpoch);
  BOOST_CHECK_EQUAL(restored.second, packet.second);
  BOOST_CHECK_EQUAL(restored.packetSeq, packet.packetSeq);
  BOOST_CHECK_EQUAL(restored.mediaSequence, packet.mediaSequence);
  BOOST_CHECK_EQUAL(restored.frameSeq, packet.frameSeq);
  BOOST_CHECK_EQUAL(restored.captureMs, packet.captureMs);
  BOOST_CHECK_EQUAL(restored.frameFirstPacketSeq, packet.frameFirstPacketSeq);
  BOOST_CHECK_EQUAL(restored.frameLastPacketSeq, packet.frameLastPacketSeq);
  BOOST_CHECK_EQUAL(restored.bucketPacketCount, packet.bucketPacketCount);
  BOOST_CHECK_EQUAL(restored.frameSegmentIndex, packet.frameSegmentIndex);
  BOOST_CHECK_EQUAL(restored.frameSegmentCount, packet.frameSegmentCount);
  BOOST_CHECK_EQUAL(restored.encoding, packet.encoding);
  BOOST_CHECK_EQUAL(restored.keyFrame, packet.keyFrame);
  BOOST_CHECK_EQUAL(restored.fecDataShards, packet.fecDataShards);
  BOOST_CHECK_EQUAL(restored.fecParityShards, packet.fecParityShards);
  BOOST_CHECK_EQUAL(restored.fecSymbolIndex, packet.fecSymbolIndex);
  BOOST_CHECK_EQUAL(restored.fecSymbolCount, packet.fecSymbolCount);
  BOOST_CHECK_EQUAL(restored.fecDataLengths, packet.fecDataLengths);
  BOOST_CHECK(restored.payload == packet.payload);

  BOOST_CHECK(encodeVideoPacket(restored) == encodeVideoPacket(packet));
}

BOOST_AUTO_TEST_CASE(StreamChunkHandoffPreservesFecRecoveryInputs)
{
  const std::vector<uint8_t> shard0{0x10, 0x20, 0x30, 0x40};
  const std::vector<uint8_t> shard1{0x01, 0x02, 0x03};
  std::vector<uint8_t> parity(shard0.size(), 0);
  for (size_t i = 0; i < shard0.size(); ++i) {
    parity[i] ^= shard0[i];
  }
  for (size_t i = 0; i < shard1.size(); ++i) {
    parity[i] ^= shard1[i];
  }

  auto makePacket = [] (uint64_t packetSeq,
                        uint32_t symbolIndex,
                        std::vector<uint8_t> payload) {
    VideoPacket packet;
    packet.streamId = "live|A|fec";
    packet.streamSessionEpoch = 88;
    packet.second = 123;
    packet.packetSeq = packetSeq;
    packet.frameSeq = 5;
    packet.captureMs = 6789;
    packet.frameFirstPacketSeq = 20;
    packet.frameLastPacketSeq = 22;
    packet.bucketPacketCount = 23;
    packet.frameSegmentIndex = symbolIndex;
    packet.frameSegmentCount = 3;
    packet.encoding = "video/h264";
    packet.fecDataShards = 2;
    packet.fecParityShards = 1;
    packet.fecSymbolIndex = symbolIndex;
    packet.fecSymbolCount = 3;
    packet.fecDataLengths = "4,3";
    packet.payload = std::move(payload);
    return packet;
  };

  const auto receivedData0 = videoPacketToStreamChunk(makePacket(20, 0, shard0));
  const auto receivedParity = videoPacketToStreamChunk(makePacket(22, 2, parity));

  BOOST_REQUIRE(receivedData0.fec);
  BOOST_REQUIRE(receivedParity.fec);
  BOOST_CHECK_EQUAL(receivedData0.fec->dataLengths.size(), 2);
  BOOST_CHECK_EQUAL(receivedParity.fec->dataLengths[1], shard1.size());
  BOOST_CHECK(receivedParity.fec->repairSymbol);

  const auto missingIdx = 1U;
  std::vector<uint8_t> recovered(receivedParity.fec->dataLengths[missingIdx], 0);
  for (size_t i = 0; i < recovered.size(); ++i) {
    recovered[i] ^= (i < receivedParity.payload.size() ? receivedParity.payload[i] : 0);
    recovered[i] ^= (i < receivedData0.payload.size() ? receivedData0.payload[i] : 0);
  }

  ndn_service_framework::StreamChunk recoveredChunk;
  recoveredChunk.streamId = receivedData0.streamId;
  recoveredChunk.sessionEpoch = receivedData0.sessionEpoch;
  recoveredChunk.seq = 21;
  recoveredChunk.payload = recovered;
  recoveredChunk.contentType = receivedData0.contentType;
  recoveredChunk.frameId = receivedData0.frameId;
  recoveredChunk.frameFirstSeq = receivedData0.frameFirstSeq;
  recoveredChunk.frameLastSeq = receivedData0.frameLastSeq;
  recoveredChunk.segmentIndex = missingIdx;
  recoveredChunk.segmentCount = receivedData0.segmentCount;

  BOOST_CHECK(recoveredChunk.payload == shard1);
  BOOST_CHECK_EQUAL(recoveredChunk.streamId, "live|A|fec");
  BOOST_CHECK_EQUAL(recoveredChunk.sessionEpoch, 88);
  BOOST_CHECK_EQUAL(recoveredChunk.seq, 21);
}

BOOST_AUTO_TEST_CASE(RecordingDataProductTracksCanonicalCatalogSummary)
{
  Fields fields{
    {"type", "camera-recording-manifest"},
    {"drone_id", "A"},
    {"recording_session_id", "record-123"},
    {"recording_object_prefix", "/example/uav/drone/A/repo/camera/recording"},
    {"recording_chunks", "42"},
    {"recording_bytes", "123456"},
  };

  const auto product = RecordingDataProductState::fromFields(fields);
  BOOST_CHECK_EQUAL(product.droneId, "A");
  BOOST_CHECK_EQUAL(product.productType, "camera-recording");
  BOOST_CHECK_EQUAL(product.sessionId, "record-123");
  BOOST_CHECK_EQUAL(product.chunks, 42);
  BOOST_CHECK_EQUAL(product.bytes, 123456);
  BOOST_CHECK(product.isAvailable());
  BOOST_CHECK(product.isPlayable());

  const auto roundTrip = RecordingDataProductState::fromFields(product.toFields());
  BOOST_CHECK_NE(roundTrip.statusLine().find("RecordingDataProduct drone=A"), std::string::npos);
  BOOST_CHECK_NE(roundTrip.statusLine().find("representation=canonical-signed-data"),
                 std::string::npos);
  BOOST_CHECK_NE(roundTrip.statusLine().find("playable=true"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(CanonicalRecordingManifestRejectsRollbackGapsAndPlaintextKeyFields)
{
  CanonicalVideoRecordingManifest manifest;
  manifest.manifestVersion = 3;
  manifest.recordingId = "record-stream";
  manifest.streamId = "stream-00112233445566778899aabbccddeeff";
  manifest.sessionEpoch = 7;
  manifest.mappingVersion = 8;
  manifest.keyEpoch = 1;
  manifest.providerIdentity = ndn::Name("/uav/drone/A");
  manifest.serviceName = ndn::Name("/UAV/VideoControl");
  manifest.firstCommittedCursor = 4;
  manifest.lastCommittedCursor = 4;
  manifest.safeJoinCursor = 4;
  manifest.startedMs = 100;
  manifest.endedMs = 200;
  manifest.complete = true;
  manifest.signerCertificateName = "/uav/drone/A/KEY/k/issuer/v=1";
  manifest.signerCertificateDigest.fill(0x11);
  manifest.trustPolicyVersion = "uav-stream-v1";
  manifest.redactedStreamDescriptor = {{"stream_id", manifest.streamId}};
  manifest.archivedCertificateObjects.emplace_back(
    "/uav/drone/A/KEY/k/issuer/v=1");
  manifest.keyAuthorizationObject = ndn::Name(
    "/uav/drone/A/repo/camera/recording/record-stream/KEY-AUTH/v=1");
  RetainedVideoPacketReference packet;
  packet.kind = "source";
  packet.cursor = 4;
  packet.dataName = ndn::Name("/uav/drone/A/video/front/stream/v=8/fec-group/0/data/0");
  packet.wireDigest.fill(0x22);
  manifest.packets.push_back(packet);

  const auto fields = manifest.toFields();
  BOOST_CHECK_EQUAL(fields.count("recording_encryption_content_key_hex"), 0);
  const auto decoded = CanonicalVideoRecordingManifest::fromFields(fields);
  BOOST_CHECK_EQUAL(decoded.packets.front().dataName, packet.dataName);
  BOOST_CHECK_EQUAL(decoded.keyAuthorizationObject, manifest.keyAuthorizationObject);

  manifest.gaps.push_back({4, 4, "storage-failure"});
  BOOST_REQUIRE(manifest.validate());
  BOOST_CHECK_EQUAL(*manifest.validate(), "gapped recording cannot be complete");
}

BOOST_AUTO_TEST_CASE(UavVideoContentKeyGrantBindsRecipientPermissionAndEpoch)
{
  UavVideoContentKeyGrant grant;
  grant.recipientIdentity = "/operator/alice";
  grant.providerIdentity = ndn::Name("/uav/drone/A");
  grant.serviceName = ndn::Name("/UAV/VideoControl");
  grant.permission = "/PERMISSION/UAV/VideoControl/history";
  grant.streamId = "stream-00112233445566778899aabbccddeeff";
  grant.sessionEpoch = 7;
  grant.keyEpoch = 2;
  grant.protectedKeyMaterial = ndn::Buffer(32, 0x33);
  grant.protectedNonceSalt = ndn::Buffer(4, 0x44);
  grant.issuedMs = 100;
  grant.expiresMs = 200;
  BOOST_CHECK(!grant.validate());
  const auto fields = grant.toProtectedFields();
  BOOST_CHECK_EQUAL(fields.at("grant_recipient"), "/operator/alice");
  grant.expiresMs = 99;
  BOOST_CHECK(grant.validate());
}

BOOST_AUTO_TEST_CASE(RecordingDataProductIgnoresRetiredEnvelopeFields)
{
  Fields fields{
    {"drone_id", "A"},
    {"recording_session_id", "record-123"},
    {"recording_object_prefix", "/example/uav/drone/A/repo/camera/recording"},
    {"recording_encryption", "hybrid-aes-256-gcm-at-rest"},
    {"recording_chunks", "2"},
    {"recording_bytes", "100"},
  };

  const auto product = RecordingDataProductState::fromFields(fields);
  BOOST_CHECK(product.isAvailable());
  BOOST_CHECK(product.isPlayable());
  const auto emitted = product.toFields();
  BOOST_CHECK(emitted.count("recording_encryption") == 0);
  BOOST_CHECK(emitted.count("recording_encryption_key_id") == 0);
  BOOST_CHECK(emitted.count("recording_encryption_content_key_hex") == 0);
}

BOOST_AUTO_TEST_CASE(CompactTelemetryHasFrozenSizesAndMonotonicAdmission)
{
  LatestTelemetryAdmission admission("A");
  for (uint64_t sampleId = 10; sampleId < 16; ++sampleId) {
    const auto sample = CompactTelemetrySample::deterministic(
      sampleId, 1'000'000'000 + sampleId * 50'000'000, "A");
    const auto wire = sample.encode();
    BOOST_CHECK_EQUAL(wire.size(), CompactTelemetrySample::encodedSizeFor(sampleId));
    const auto decoded = CompactTelemetrySample::decode(wire);
    BOOST_REQUIRE(decoded);
    BOOST_CHECK_EQUAL(decoded->sampleId, sampleId);
    BOOST_CHECK_EQUAL(decoded->droneId, "A");
    const auto result = admission.admit(wire, sample.sourceTimestampNs + 2'000'000);
    BOOST_CHECK(result.valid);
    BOOST_CHECK(result.stateAdvanced);
    BOOST_CHECK_EQUAL(result.ageNs, 2'000'000);
  }
  const auto duplicate = CompactTelemetrySample::deterministic(
    15, 1'750'000'000, "A").encode();
  BOOST_CHECK(admission.admit(duplicate, 1'752'000'000).duplicate);
  const auto old = CompactTelemetrySample::deterministic(
    9, 900'000'000, "A").encode();
  const auto outOfOrder = admission.admit(old, 1'753'000'000);
  BOOST_CHECK(outOfOrder.outOfOrder);
  BOOST_CHECK(outOfOrder.newSample);
  BOOST_REQUIRE(admission.latest());
  BOOST_CHECK_EQUAL(admission.latest()->sampleId, 15);
  BOOST_CHECK_EQUAL(admission.admittedCount(), 7);
  BOOST_CHECK_EQUAL(admission.duplicateCount(), 1);
  BOOST_CHECK_EQUAL(admission.outOfOrderCount(), 1);
}

BOOST_AUTO_TEST_CASE(CompactTelemetryFailsClosedOnWrongIdentityAndPadding)
{
  LatestTelemetryAdmission admission("A");
  auto wrong = CompactTelemetrySample::deterministic(0, 100, "B").encode();
  const auto wrongResult = admission.admit(wrong, 200);
  BOOST_CHECK(!wrongResult.valid);
  BOOST_CHECK(!wrongResult.stateAdvanced);
  BOOST_CHECK_EQUAL(wrongResult.reason, "wrong-drone");

  auto malformed = CompactTelemetrySample::deterministic(0, 100, "A").encode();
  malformed.back() ^= 0xff;
  const auto malformedResult = admission.admit(malformed, 200);
  BOOST_CHECK(!malformedResult.valid);
  BOOST_CHECK_EQUAL(malformedResult.reason, "invalid-payload");
}

BOOST_AUTO_TEST_CASE(OpaqueAcousticCyclesExtentAndCompletesOnce)
{
  CompleteAcousticBlockAdmission admission("uav-acoustic-test");
  for (uint64_t blockId = 0; blockId < 6; ++blockId) {
    const auto captured = 1'000'000'000 + blockId * 40'000'000;
    const auto count = OpaqueAcousticSource::sourceCountFor(blockId);
    BOOST_CHECK_EQUAL(count, 2 + blockId % 3);
    BOOST_CHECK_EQUAL(acousticSourceCountClass(count),
                      "opaque-block-" + std::to_string(count));
    for (size_t index = 0; index < count; ++index) {
      const auto wire =
        OpaqueAcousticSource::deterministic(blockId, captured, index).encode();
      BOOST_CHECK_LE(wire.size(), 512);
      const auto result = admission.admit(
        wire,
        index == 0 ? LiveStreamItemProvenance::FecRecovered :
                     LiveStreamItemProvenance::SignedData,
        captured + 3'000'000);
      BOOST_CHECK(result.valid);
      if (index + 1 == count) {
        BOOST_REQUIRE(result.completed);
        BOOST_CHECK_EQUAL(result.completed->blockId, blockId);
        BOOST_CHECK_EQUAL(result.completed->orderedSources.size(), count);
        BOOST_CHECK_EQUAL(result.completed->recoveredSources, 1);
      }
      else {
        BOOST_CHECK(!result.completed);
      }
    }
  }
  BOOST_CHECK_EQUAL(admission.completedCount(), 6);
}

BOOST_AUTO_TEST_CASE(OpaqueAcousticRejectsMalformedAndDuplicateSources)
{
  CompleteAcousticBlockAdmission admission("uav-acoustic-test");
  auto wire = OpaqueAcousticSource::deterministic(0, 100, 0).encode();
  BOOST_CHECK(admission.admit(
    wire, LiveStreamItemProvenance::SignedData, 200).valid);
  const auto duplicate = admission.admit(
    wire, LiveStreamItemProvenance::SignedData, 201);
  BOOST_CHECK(duplicate.duplicate);
  wire.back() ^= 0x01;
  const auto malformed = admission.admit(
    wire, LiveStreamItemProvenance::SignedData, 202);
  BOOST_CHECK(!malformed.valid);
  BOOST_CHECK_EQUAL(admission.duplicateCount(), 1);
  BOOST_CHECK_EQUAL(admission.invalidCount(), 1);
}

BOOST_AUTO_TEST_CASE(OpaqueAcousticRejectsSourceWireAboveFrozenLimit)
{
  auto source = OpaqueAcousticSource::deterministic(0, 100, 0);
  source.opaqueBytes.resize(486);
  BOOST_CHECK_THROW(source.encode(), std::invalid_argument);

  source.opaqueBytes.resize(485);
  const auto wire = source.encode();
  BOOST_CHECK_EQUAL(wire.size(), 512);
}

BOOST_AUTO_TEST_CASE(UavSensorDefinitionsStayOnGenericMappingV2)
{
  const auto telemetry = ndnsf::examples::uav::makeUavTelemetryStreamDefinition(
    ndn::Name("/example/uav/drone/A"), 144001, 144001);
  BOOST_CHECK(!telemetry.validate());
  BOOST_CHECK_EQUAL(telemetry.samplePeriodMs, 50);
  BOOST_CHECK_EQUAL(telemetry.mappingBlockCapacity, 1);
  BOOST_CHECK(!telemetry.fec.enabled());
  BOOST_REQUIRE_EQUAL(telemetry.sampleClasses.size(), 1);
  BOOST_CHECK_EQUAL(telemetry.sampleClasses.front().hardMaxSourceItems, 1);

  const auto acoustic = ndnsf::examples::uav::makeUavAcousticStreamDefinition(
    ndn::Name("/example/uav/drone/A"), 144002, 144002);
  BOOST_CHECK(!acoustic.validate());
  BOOST_CHECK_EQUAL(acoustic.samplePeriodMs, 40);
  BOOST_CHECK_EQUAL(acoustic.mappingBlockCapacity, 6);
  BOOST_CHECK_EQUAL(acoustic.maxNameReservations %
                      acoustic.mappingBlockCapacity, 0);
  BOOST_CHECK_EQUAL(acoustic.fec.recoveryCapacity(), 2);
  BOOST_CHECK_EQUAL(acoustic.fec.repairItemCount(), 2);
  BOOST_REQUIRE_EQUAL(acoustic.sampleClasses.size(), 3);
  for (size_t index = 0; index < acoustic.sampleClasses.size(); ++index) {
    BOOST_CHECK_EQUAL(acoustic.sampleClasses[index].seedSourceItems, index + 2);
    BOOST_CHECK_EQUAL(acoustic.sampleClasses[index].hardMaxSourceItems, index + 2);
    BOOST_CHECK_EQUAL(acoustic.sampleClasses[index].safetyMarginItems, 0);
  }
  BOOST_CHECK_THROW(acousticSourceCountClass(1), std::invalid_argument);
  BOOST_CHECK_THROW(acousticSourceCountClass(5), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndn_service_framework::test
