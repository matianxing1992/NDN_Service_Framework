#include "../../NDNSF-UAV-APP/shared/UavObservationSource.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <limits>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <utility>
#include <unistd.h>

using namespace ndnsf::examples::uav;

namespace {

SourceIdentity
identity()
{
  return {"source-1", "drone-1", "UAV1", 7, "synthetic-local",
          AcquisitionKind::Simulated, DeliveryMode::Replay};
}

ClockBinding
clock(uint64_t epoch = 2, uint64_t uncertaintyUs = 0)
{
  return {1, "sim-clock", epoch, 1.0, 0.0, 0, 10000000, uncertaintyUs};
}

CalibrationRecord
calibration()
{
  CalibrationRecord value;
  value.cameraId = "UAV1";
  value.revision = 3;
  value.width = 640;
  value.height = 480;
  value.fx = 500;
  value.fy = 500;
  value.cx = 320;
  value.cy = 240;
  value.validFromUs = 0;
  value.validUntilUs = 10000000;
  return value;
}

GroundRecord
ground()
{
  GroundRecord value;
  value.revision = 4;
  value.worldFrameId = "synthetic-local";
  value.roiVertices = {{{0, 0, 0}}, {{10, 0, 0}}, {{10, 10, 0}}, {{0, 10, 0}}};
  return value;
}

CapturedFrame
frame(uint64_t sequence = 1, uint64_t timeUs = 1000000)
{
  CapturedFrame value;
  value.key = {"source-1", "UAV1", 7, sequence};
  value.deviceTimeUs = timeUs;
  value.clockBinding = clock();
  value.width = 640;
  value.height = 480;
  value.calibrationRevision = 3;
  value.imageBytes = {0xff, 0xd8, 0xff, 0xd9};
  return value;
}

PoseSample
pose(uint64_t sequence, uint64_t timeUs, double x)
{
  PoseSample value;
  value.identity = identity();
  value.sampleSequence = sequence;
  value.deviceTimeUs = timeUs;
  value.clockBinding = clock();
  value.positionW = {x, 2.0, 3.0};
  value.qWC = {1.0, 0.0, 0.0, 0.0};
  value.calibrationRevision = 3;
  return value;
}

ObservationIngress
makeIngress(uint64_t& steady, ObservationPolicy policy = {})
{
  return ObservationIngress(identity(), policy, [&steady] { return steady; });
}

FrameGap
makeGap(uint64_t first, uint64_t last, SourceIdentity source = identity(),
        std::string reason = "CAPTURE_DROPPED")
{
  FrameGap value;
  value.identity = std::move(source);
  value.firstSequence = first;
  value.lastSequence = last;
  value.reason = std::move(reason);
  return value;
}

// A synchronous native producer can see Admission. JSONL writers cannot;
// they must announce an already chosen loss instead of submitting that frame.
class AdmissionPolicySource final : public IObservationSource
{
public:
  explicit AdmissionPolicySource(SourceIdentity source) : m_identity(std::move(source)) {}
  ~AdmissionPolicySource() override { stop(); }

  void start(IObservationSink& sink) override
  {
    if (m_started) throw std::logic_error("source already started");
    m_started = true;
    m_sink = &sink;
    calibrationAdmission = sink.submitCalibration(calibration());
    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
      auto captured = frame(sequence, 1000000 + sequence * 200000);
      const auto admitted = sink.submitFrame(captured);
      frameAdmissions.push_back(admitted);
      if (admitted.reason != "QUEUE_FULL") continue;
      if (m_identity.deliveryMode == DeliveryMode::LivePush) {
        gapAdmission = sink.submitGap(makeGap(captured.key.sequence, captured.key.sequence,
                                              m_identity, "QUEUE_FULL"));
      }
      else {
        m_retained = std::move(captured);
      }
      break;
    }
  }

  Admission retry()
  {
    if (!m_sink || !m_retained) return {ObservationAdmissionStatus::Closed, "CLOSED"};
    const auto result = m_sink->submitFrame(*m_retained);
    if (result.accepted()) m_retained.reset();
    return result;
  }

  void stop() noexcept override { m_retained.reset(); m_sink = nullptr; }

  Admission calibrationAdmission;
  std::vector<Admission> frameAdmissions;
  Admission gapAdmission;

private:
  SourceIdentity m_identity;
  IObservationSink* m_sink = nullptr;
  std::optional<CapturedFrame> m_retained;
  bool m_started = false;
};

class RecordingObservationSink final : public IObservationSink
{
public:
  Admission submitGap(FrameGap record) override
  {
    gap = std::move(record);
    ++gaps;
    return {ObservationAdmissionStatus::Accepted, "ACCEPTED"};
  }
  Admission submitCalibration(CalibrationRecord record) override
  {
    calibration = std::move(record);
    ++calibrations;
    return {ObservationAdmissionStatus::Accepted, "ACCEPTED"};
  }
  Admission submitGround(GroundRecord) override
  {
    ++grounds;
    return {ObservationAdmissionStatus::Accepted, "ACCEPTED"};
  }
  Admission submitPose(PoseSample) override
  {
    ++poses;
    return {ObservationAdmissionStatus::Accepted, "ACCEPTED"};
  }
  Admission submitFrame(CapturedFrame record) override
  {
    frame = std::move(record);
    ++frames;
    return {ObservationAdmissionStatus::Accepted, "ACCEPTED"};
  }

  size_t calibrations = 0;
  size_t grounds = 0;
  size_t poses = 0;
  size_t frames = 0;
  size_t gaps = 0;
  CalibrationRecord calibration;
  CapturedFrame frame;
  FrameGap gap;
};

class BlockingObservationSink final : public IObservationSink
{
public:
  Admission submitCalibration(CalibrationRecord) override
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_entered = true;
    m_condition.notify_all();
    m_condition.wait(lock, [this] { return m_released; });
    return accepted();
  }
  Admission submitGround(GroundRecord) override { return accepted(); }
  Admission submitPose(PoseSample) override { return accepted(); }
  Admission submitFrame(CapturedFrame) override { return accepted(); }
  Admission submitGap(FrameGap) override { return accepted(); }

  bool waitUntilEntered(std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_condition.wait_for(lock, timeout, [this] { return m_entered; });
  }

  bool waitUntilReleased(std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_condition.wait_for(lock, timeout, [this] { return m_released; });
  }

  void release()
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_released = true;
    }
    m_condition.notify_all();
  }

private:
  static Admission accepted()
  {
    return {ObservationAdmissionStatus::Accepted, "ACCEPTED"};
  }

  std::mutex m_mutex;
  std::condition_variable m_condition;
  bool m_entered = false;
  bool m_released = false;
};

const std::string CALIBRATION_EVENT =
  R"({"schema":"uav-source-event-v2","kind":"calibration","payload":{"schema":"uav-calibration-v2","cameraId":"UAV1","revision":"3","width":"640","height":"480","fx":"500","fy":"500","cx":"320","cy":"240","distortion":["0","0","0","0","0"],"imageTransform":"raw","validFromUs":"0","validUntilUs":"10000000"}})";

const std::string FRAME_EVENT =
  R"({"schema":"uav-source-event-v2","kind":"frame","payload":{"key":{"sourceId":"source-1","cameraId":"UAV1","epoch":"7","sequence":"1"},"deviceTimeUs":"1000","clockBinding":{"revision":"1","clockId":"sim-clock","clockEpoch":"2","a":"1","bUs":"0","validFromUs":"0","validUntilUs":"10000000","uncertaintyUs":"0"},"width":"640","height":"480","encoding":"image/jpeg","calibrationRevision":"3","imageBase64":"/9j/2Q=="}})";

const std::string GAP_EVENT =
  R"({"schema":"uav-source-event-v2","kind":"gap","payload":{"schema":"uav-frame-gap-v2","identity":{"sourceId":"source-1","droneId":"drone-1","cameraId":"UAV1","epoch":"7","worldFrameId":"synthetic-local","acquisitionKind":"simulated","deliveryMode":"replay"},"firstSequence":"2","lastSequence":"2","reason":"CAPTURE_DROPPED"}})";

void
writeAll(int fd, const std::string& value)
{
  size_t offset = 0;
  while (offset < value.size()) {
    const auto written = ::write(fd, value.data() + offset, value.size() - offset);
    BOOST_REQUIRE(written > 0);
    offset += static_cast<size_t>(written);
  }
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavObservationSourceTests)

BOOST_AUTO_TEST_CASE(DeclaredGapPreservesSurvivingKeysAndCaptureIntervals)
{
  uint64_t steady = 0;
  auto ingress = makeIngress(steady);
  BOOST_REQUIRE(ingress.submitCalibration(calibration()).accepted());
  BOOST_REQUIRE(ingress.submitGround(ground()).accepted());
  BOOST_REQUIRE(ingress.submitPose(pose(1, 1000000, 0.0)).accepted());
  BOOST_REQUIRE(ingress.submitPose(pose(3, 1400000, 2.0)).accepted());
  BOOST_REQUIRE(ingress.submitFrame(frame(1, 1000000)).accepted());
  const auto bytes = ingress.queuedBytes();
  BOOST_REQUIRE(ingress.submitGap(makeGap(2, 2)).accepted());
  BOOST_CHECK_EQUAL(ingress.queuedBytes(), bytes);
  BOOST_CHECK_EQUAL(ingress.queuedFrames(), 1U);
  BOOST_REQUIRE(ingress.submitFrame(frame(3, 1400000)).accepted());
  const auto first = ingress.take(frame(1).key, steady);
  const auto missing = ingress.take(frame(2).key, steady);
  const auto next = ingress.take(frame(3).key, steady);
  BOOST_REQUIRE(first.aligned);
  BOOST_REQUIRE(next.aligned);
  BOOST_CHECK(first.status == AlignedObservationStatus::Ready);
  BOOST_CHECK(next.status == AlignedObservationStatus::Ready);
  BOOST_CHECK(missing.status == AlignedObservationStatus::Rejected);
  BOOST_CHECK_EQUAL(missing.reason, "FRAME_GAP");
  BOOST_CHECK(!missing.aligned);
  BOOST_CHECK_EQUAL(first.aligned->frame.key.sequence, 1U);
  BOOST_CHECK_EQUAL(next.aligned->frame.key.sequence, 3U);
  BOOST_CHECK_EQUAL(next.aligned->captureTimeUs - first.aligned->captureTimeUs, 400000U);
  BOOST_CHECK_EQUAL(ingress.submitFrame(frame(2)).reason, "FRAME_GAP");
  BOOST_CHECK_EQUAL(ingress.take(frame(4).key, steady).reason, "NOT_FOUND");
  auto foreign = frame(2).key;
  foreign.epoch = 8;
  BOOST_CHECK_EQUAL(ingress.take(foreign, steady).reason, "NOT_FOUND");
  const auto records = ingress.frameGaps();
  BOOST_REQUIRE_EQUAL(records.size(), 1U);
  BOOST_CHECK_EQUAL(records[0].firstSequence, 2U);
  BOOST_CHECK_EQUAL(records[0].lastSequence, 2U);
  BOOST_CHECK_EQUAL(records[0].identity.sourceId, "source-1");
}

BOOST_AUTO_TEST_CASE(SourcePolicyDistinguishesLiveDropFromReplayBackpressure)
{
  for (const auto mode : {DeliveryMode::LivePush, DeliveryMode::Replay}) {
    uint64_t steady = 0;
    auto sourceIdentity = identity();
    sourceIdentity.deliveryMode = mode;
    ObservationIngress ingress(sourceIdentity, {}, [&] { return steady; });
    AdmissionPolicySource source(sourceIdentity); // destroyed before its sink
    source.start(ingress);
    BOOST_REQUIRE(source.calibrationAdmission.accepted());
    BOOST_REQUIRE_EQUAL(source.frameAdmissions.size(), 3U);
    BOOST_CHECK(source.frameAdmissions[0].accepted());
    BOOST_CHECK(source.frameAdmissions[1].accepted());
    BOOST_CHECK_EQUAL(source.frameAdmissions[2].reason, "QUEUE_FULL");
    BOOST_CHECK_EQUAL(ingress.queuedFrames(), 2U);
    if (mode == DeliveryMode::LivePush) {
      BOOST_REQUIRE(source.gapAdmission.accepted());
      const auto records = ingress.frameGaps();
      BOOST_REQUIRE_EQUAL(records.size(), 1U);
      BOOST_CHECK_EQUAL(records[0].firstSequence, 3U);
      BOOST_CHECK_EQUAL(records[0].lastSequence, 3U);
      BOOST_CHECK_EQUAL(records[0].reason, "QUEUE_FULL");
      BOOST_CHECK(records[0].identity.deliveryMode == DeliveryMode::LivePush);
      BOOST_CHECK_EQUAL(ingress.take(frame(3).key, steady).reason, "FRAME_GAP");
      BOOST_CHECK_EQUAL(ingress.submitFrame(frame(3, 1600000)).reason, "FRAME_GAP");
    }
    else {
      BOOST_CHECK(ingress.frameGaps().empty());
      const auto taken = ingress.take(frame(1).key, 10000000);
      BOOST_REQUIRE(taken.aligned);
      BOOST_REQUIRE(source.retry().accepted());
      BOOST_CHECK(ingress.frameGaps().empty());
      const auto retried = ingress.take(frame(3).key, 10000000);
      BOOST_REQUIRE(retried.aligned);
      BOOST_CHECK_EQUAL(retried.aligned->frame.key.sequence, 3U);
      BOOST_CHECK_EQUAL(retried.aligned->captureTimeUs, 1600000U);
    }
    source.stop();
    BOOST_CHECK(source.retry().status == ObservationAdmissionStatus::Closed);
  }
}

BOOST_AUTO_TEST_CASE(GapIdentityConflictsAndRejectedRecordsDoNotAdvanceState)
{
  uint64_t steady = 0;
  auto ingress = makeIngress(steady);
  BOOST_REQUIRE(ingress.submitGap(makeGap(2, 4)).accepted());
  BOOST_CHECK(ingress.submitGap(makeGap(2, 4)).status == ObservationAdmissionStatus::Duplicate);
  BOOST_CHECK_EQUAL(ingress.submitGap(makeGap(2, 4, identity(), "QUEUE_FULL")).reason,
                    "IDENTITY_CONFLICT");
  BOOST_CHECK_EQUAL(ingress.submitGap(makeGap(4, 6)).reason, "IDENTITY_CONFLICT");
  BOOST_CHECK_EQUAL(ingress.submitGap(makeGap(1, 1)).reason, "OLD_SEQUENCE");
  BOOST_CHECK_EQUAL(ingress.frameGaps().size(), 1U);
  BOOST_REQUIRE(ingress.submitCalibration(calibration()).accepted());
  BOOST_CHECK(ingress.submitFrame(frame(5)).accepted()); // overlap did not commit6

  for (size_t field = 0; field < 7; ++field) {
    auto foreign = makeGap(10, 10);
    if (field == 0) foreign.identity.sourceId = "other";
    if (field == 1) foreign.identity.droneId = "other";
    if (field == 2) foreign.identity.cameraId = "UAV2";
    if (field == 3) foreign.identity.epoch = 8;
    if (field == 4) foreign.identity.worldFrameId = "other";
    if (field == 5) foreign.identity.acquisitionKind = AcquisitionKind::Measured;
    if (field == 6) foreign.identity.deliveryMode = DeliveryMode::LivePush;
    BOOST_CHECK_EQUAL(ingress.submitGap(foreign).reason, "IDENTITY_MISMATCH");
  }
  auto invalid = makeGap(10, 9);
  BOOST_CHECK_EQUAL(ingress.submitGap(invalid).reason, "INVALID_GAP");
  invalid = makeGap(0, 1);
  BOOST_CHECK_EQUAL(ingress.submitGap(invalid).reason, "INVALID_GAP");
  invalid = makeGap(6, 6);
  invalid.schema = "uav-frame-gap-v99";
  BOOST_CHECK_EQUAL(ingress.submitGap(invalid).reason, "INVALID_GAP");
  invalid.schema = "uav-frame-gap-v2";
  invalid.reason = "invented";
  BOOST_CHECK_EQUAL(ingress.submitGap(invalid).reason, "INVALID_GAP");
  BOOST_CHECK(ingress.submitGap(makeGap(6, 6)).accepted());
}

BOOST_AUTO_TEST_CASE(GapLedgerIsBoundedAndClearedOnCloseAndEpochChange)
{
  uint64_t steady = 0;
  auto ingress = makeIngress(steady);
  for (uint64_t sequence = 1; sequence <= 128; ++sequence) {
    BOOST_REQUIRE(ingress.submitGap(makeGap(sequence, sequence)).accepted());
  }
  BOOST_CHECK_EQUAL(ingress.frameGaps().size(), 128U);
  BOOST_CHECK(ingress.submitGap(makeGap(1, 1)).status == ObservationAdmissionStatus::Duplicate);
  BOOST_CHECK_EQUAL(ingress.submitGap(makeGap(129, 129)).reason, "GAP_LIMIT");
  BOOST_REQUIRE(ingress.submitCalibration(calibration()).accepted());
  BOOST_CHECK(ingress.submitFrame(frame(129)).accepted());
  auto retained = ingress.frameGaps();
  ingress.close();
  BOOST_CHECK(ingress.frameGaps().empty());
  BOOST_CHECK_EQUAL(retained.size(), 128U);
  BOOST_CHECK_EQUAL(ingress.queuedBytes(), 0U);
  BOOST_CHECK(ingress.submitGap(makeGap(130, 130)).status == ObservationAdmissionStatus::Closed);
  BOOST_CHECK(ingress.take(frame(2).key, steady).status == AlignedObservationStatus::Closed);
  auto nextIdentity = identity();
  nextIdentity.epoch = 8;
  ObservationIngress next(nextIdentity, {}, [&] { return steady; });
  BOOST_CHECK_EQUAL(next.submitGap(makeGap(1, 1)).reason, "IDENTITY_MISMATCH");
  BOOST_CHECK(next.submitGap(makeGap(1, 1, nextIdentity)).accepted());
  BOOST_CHECK_EQUAL(next.frameGaps().size(), 1U);

  auto huge = makeIngress(steady);
  const uint64_t last = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  BOOST_CHECK_EQUAL(huge.submitGap(makeGap(1, last + 1)).reason, "INVALID_GAP");
  BOOST_REQUIRE(huge.submitGap(makeGap(1, last)).accepted());
  BOOST_CHECK_EQUAL(huge.frameGaps().size(), 1U);
  BOOST_CHECK_EQUAL(huge.queuedFrames(), 0U);
  BOOST_CHECK_EQUAL(huge.queuedBytes(), 0U);
  auto lastKey = frame().key;
  lastKey.sequence = last;
  BOOST_CHECK_EQUAL(huge.take(lastKey, steady).reason, "FRAME_GAP");
}

BOOST_AUTO_TEST_CASE(StreamAdmitsTypedGapAndRejectsMalformedOrConflictingRecords)
{
  const auto run = [](const std::string& input) {
    int descriptors[2];
    BOOST_REQUIRE_EQUAL(::pipe(descriptors), 0);
    BOOST_REQUIRE(input.size() < 4096); // do not block a pipe before its reader starts
    writeAll(descriptors[1], input);
    BOOST_REQUIRE_EQUAL(::close(descriptors[1]), 0);
    uint64_t steady = 0;
    auto ingress = makeIngress(steady);
    StreamObservationSource source(descriptors[0], identity(), {});
    source.start(ingress);
    const auto result = source.pump();
    return std::make_pair(result, ingress.frameGaps());
  };
  const auto valid = run(GAP_EVENT + "\n" + GAP_EVENT + "\n");
  BOOST_CHECK_EQUAL(valid.first.acceptedEvents, 1U);
  BOOST_CHECK_EQUAL(valid.first.duplicateEvents, 1U);
  BOOST_CHECK_EQUAL(valid.first.rejectedEvents, 0U);
  BOOST_REQUIRE_EQUAL(valid.second.size(), 1U);
  BOOST_CHECK_EQUAL(valid.second[0].firstSequence, 2U);
  BOOST_CHECK_EQUAL(valid.second[0].identity.epoch, 7U);
  BOOST_CHECK_EQUAL(valid.second[0].reason, "CAPTURE_DROPPED");
  auto range = GAP_EVENT;
  range.replace(range.find("\"lastSequence\":\"2\""), std::string("\"lastSequence\":\"2\"").size(), "\"lastSequence\":\"4\"");
  auto overlap = range;
  overlap.replace(overlap.find("\"firstSequence\":\"2\""), std::string("\"firstSequence\":\"2\"").size(), "\"firstSequence\":\"4\"");
  overlap.replace(overlap.find("\"lastSequence\":\"4\""), std::string("\"lastSequence\":\"4\"").size(), "\"lastSequence\":\"6\"");
  const auto conflicting = run(range + "\n" + overlap + "\n");
  BOOST_CHECK_EQUAL(conflicting.first.acceptedEvents, 1U);
  BOOST_CHECK_EQUAL(conflicting.first.rejectedEvents, 1U);
  BOOST_CHECK_EQUAL(conflicting.first.lastError, "IDENTITY_CONFLICT");
  BOOST_REQUIRE_EQUAL(conflicting.second.size(), 1U);
  BOOST_CHECK_EQUAL(conflicting.second[0].lastSequence, 4U);
  const auto laterInvalid = run(GAP_EVENT + "\n{}\n");
  BOOST_CHECK_EQUAL(laterInvalid.first.acceptedEvents, 1U);
  BOOST_CHECK_EQUAL(laterInvalid.first.rejectedEvents, 1U);
  BOOST_CHECK_EQUAL(laterInvalid.first.lastError, "INVALID_EVENT");
  BOOST_REQUIRE_EQUAL(laterInvalid.second.size(), 1U);
  BOOST_CHECK_EQUAL(laterInvalid.second[0].firstSequence, 2U);
  const std::vector<std::pair<std::string, std::string>> changes = {
    {"\"firstSequence\":\"2\"", "\"firstSequence\":2"},
    {"\"firstSequence\":\"2\"", "\"firstSequence\":\"0\""},
    {"\"lastSequence\":\"2\"", "\"lastSequence\":\"1\""},
    {"\"lastSequence\":\"2\"", "\"lastSequence\":\"9223372036854775808\""},
    {"\"reason\":\"CAPTURE_DROPPED\"", "\"reason\":true"},
    {"CAPTURE_DROPPED", "invented"}, {"uav-frame-gap-v2", "uav-frame-gap-v99"},
    {"source-1", "foreign"}, {"drone-1", "foreign"}, {"UAV1", "UAV2"},
    {"\"epoch\":\"7\"", "\"epoch\":\"8\""},
    {"synthetic-local", "foreign"}, {"simulated", "measured"}, {"replay", "live-push"},
    {"\"reason\":", "\"captureTimeUs\":\"0\",\"reason\":"},
    {"\"firstSequence\":\"2\"", "\"firstSequence\":\"2\",\"firstSequence\":\"2\""},
  };
  for (const auto& change : changes) {
    auto line = GAP_EVENT;
    const auto position = line.find(change.first);
    BOOST_REQUIRE(position != std::string::npos);
    line.replace(position, change.first.size(), change.second);
    const auto invalid = run(line + "\n");
    BOOST_CHECK_EQUAL(invalid.first.rejectedEvents, 1U);
    BOOST_CHECK(invalid.second.empty());
  }
}

BOOST_AUTO_TEST_CASE(InterpolatesOnlyBracketedPoseAndPreservesFrameSnapshot)
{
  uint64_t steady = 1000;
  auto ingress = makeIngress(steady);
  BOOST_REQUIRE(ingress.submitCalibration(calibration()).status ==
                ObservationAdmissionStatus::Accepted);
  BOOST_REQUIRE(ingress.submitGround(ground()).status ==
                ObservationAdmissionStatus::Accepted);
  auto captured = frame();
  const auto key = captured.key;
  BOOST_REQUIRE(ingress.submitFrame(std::move(captured)).status ==
                ObservationAdmissionStatus::Accepted);
  auto pending = ingress.take(key, steady + 50000);
  BOOST_CHECK(pending.status == AlignedObservationStatus::Pending);
  BOOST_CHECK(ingress.submitPose(pose(1, 950000, 0.0)).status ==
              ObservationAdmissionStatus::Accepted);
  BOOST_CHECK(ingress.submitPose(pose(2, 1050000, 1.0)).status ==
              ObservationAdmissionStatus::Accepted);

  auto ready = ingress.take(key, steady + 50001);
  BOOST_REQUIRE(ready.status == AlignedObservationStatus::Ready);
  BOOST_REQUIRE(ready.aligned);
  BOOST_CHECK(ready.aligned->poseStatus == PoseStatus::Aligned);
  BOOST_CHECK_EQUAL(ready.aligned->poseEndpoints.size(), 2U);
  BOOST_CHECK_CLOSE((*ready.aligned->positionW)[0], 0.5, 1e-9);
  BOOST_CHECK_EQUAL(ready.aligned->captureTimeUs, 1000000U);
  BOOST_CHECK_EQUAL(ready.aligned->calibration->revision, 3U);
  BOOST_CHECK_EQUAL(ready.aligned->ground->revision, 4U);
  BOOST_CHECK_EQUAL(ingress.queuedBytes(), 0U);
}

BOOST_AUTO_TEST_CASE(NativeIngressIsInvariantAcrossDeliveryRatesAndModes)
{
  constexpr size_t FRAME_COUNT = 100;
  const std::array<DeliveryMode, 3> modes{
    DeliveryMode::Generated, DeliveryMode::Replay, DeliveryMode::LivePush};
  const std::array<std::uint64_t, 3> arrivalIntervalsUs{400000, 200000, 100000};
  std::vector<double> referencePositions;

  for (const auto mode : modes) {
    for (const auto arrivalIntervalUs : arrivalIntervalsUs) {
      auto sourceIdentity = identity();
      sourceIdentity.deliveryMode = mode;
      uint64_t steady = 0;
      ObservationPolicy policy;
      ObservationIngress ingress(sourceIdentity, policy, [&steady] { return steady; });

      auto camera = calibration();
      camera.validUntilUs = 30000000;
      BOOST_REQUIRE(ingress.submitCalibration(camera).accepted());
      BOOST_REQUIRE(ingress.submitGround(ground()).accepted());

      std::vector<double> positions;
      positions.reserve(FRAME_COUNT);
      for (size_t index = 0; index < FRAME_COUNT; ++index) {
        const auto sequence = static_cast<std::uint64_t>(index + 1);
        const auto captureTimeUs = 1000000 + index * 200000;

        auto before = pose(sequence * 2 - 1, captureTimeUs - 50000,
                           static_cast<double>(captureTimeUs - 50000) / 1000000.0);
        auto after = pose(sequence * 2, captureTimeUs + 50000,
                          static_cast<double>(captureTimeUs + 50000) / 1000000.0);
        before.identity = sourceIdentity;
        after.identity = sourceIdentity;
        before.clockBinding.validUntilUs = 30000000;
        after.clockBinding.validUntilUs = 30000000;
        BOOST_REQUIRE(ingress.submitPose(std::move(before)).accepted());
        BOOST_REQUIRE(ingress.submitPose(std::move(after)).accepted());

        auto captured = frame(sequence, captureTimeUs);
        captured.key.sourceId = sourceIdentity.sourceId;
        captured.key.cameraId = sourceIdentity.cameraId;
        captured.key.epoch = sourceIdentity.epoch;
        captured.clockBinding.validUntilUs = 30000000;
        BOOST_REQUIRE(ingress.submitFrame(captured).accepted());

        // The capture timeline stays at 5 Hz while the arrival cadence varies
        // from half-rate to twice-rate. The injected monotonic clock makes the
        // boundary deterministic without sleeping in the native test suite.
        steady += arrivalIntervalUs;
        const auto aligned = ingress.take(captured.key, steady);
        BOOST_REQUIRE(aligned.status == AlignedObservationStatus::Ready);
        BOOST_REQUIRE(aligned.aligned);
        BOOST_REQUIRE(aligned.aligned->positionW);
        BOOST_CHECK(aligned.aligned->poseStatus == PoseStatus::Aligned);
        BOOST_CHECK_EQUAL(aligned.aligned->captureTimeUs, captureTimeUs);
        const double x = (*aligned.aligned->positionW)[0];
        BOOST_CHECK_CLOSE(x, static_cast<double>(captureTimeUs) / 1000000.0, 1e-9);
        positions.push_back(x);
      }

      if (referencePositions.empty()) {
        referencePositions = std::move(positions);
      }
      else {
        BOOST_REQUIRE_EQUAL(positions.size(), referencePositions.size());
        for (size_t index = 0; index < positions.size(); ++index) {
          BOOST_CHECK_CLOSE(positions[index], referencePositions[index], 1e-9);
        }
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(ExactPoseUsesOneEndpointAndAntipodalQuaternionUsesShortestArc)
{
  uint64_t steady = 0;
  auto exactIngress = makeIngress(steady);
  BOOST_REQUIRE(exactIngress.submitCalibration(calibration()).accepted());
  BOOST_REQUIRE(exactIngress.submitGround(ground()).accepted());
  auto exactPose = pose(1, 1000000, 4.0);
  exactPose.qWC = {0.7071067811865476, 0.0, 0.0, 0.7071067811865476};
  BOOST_REQUIRE(exactIngress.submitPose(exactPose).accepted());
  const auto exactFrame = frame();
  BOOST_REQUIRE(exactIngress.submitFrame(exactFrame).accepted());
  auto exact = exactIngress.take(exactFrame.key, steady);
  BOOST_REQUIRE(exact.status == AlignedObservationStatus::Ready);
  BOOST_REQUIRE(exact.aligned);
  BOOST_CHECK_EQUAL(exact.aligned->poseEndpoints.size(), 1U);
  BOOST_CHECK_CLOSE((*exact.aligned->positionW)[0], 4.0, 1e-9);

  uint64_t interpolatedSteady = 0;
  auto interpolatedIngress = makeIngress(interpolatedSteady);
  BOOST_REQUIRE(interpolatedIngress.submitCalibration(calibration()).accepted());
  BOOST_REQUIRE(interpolatedIngress.submitGround(ground()).accepted());
  auto first = pose(1, 950000, 0.0);
  first.qWC = {0.7071067811865476, 0.0, 0.0, 0.7071067811865476};
  auto antipodal = pose(2, 1050000, 1.0);
  antipodal.qWC = {-0.7071067811865476, 0.0, 0.0, -0.7071067811865476};
  BOOST_REQUIRE(interpolatedIngress.submitPose(first).accepted());
  BOOST_REQUIRE(interpolatedIngress.submitPose(antipodal).accepted());
  auto captured = frame();
  const auto key = captured.key;
  BOOST_REQUIRE(interpolatedIngress.submitFrame(std::move(captured)).accepted());
  auto midpoint = interpolatedIngress.take(key, interpolatedSteady);
  BOOST_REQUIRE(midpoint.status == AlignedObservationStatus::Ready);
  BOOST_REQUIRE(midpoint.aligned && midpoint.aligned->qWC);
  BOOST_CHECK_CLOSE(std::abs((*midpoint.aligned->qWC)[0]), 0.7071067811865476, 1e-9);
  BOOST_CHECK_CLOSE(std::abs((*midpoint.aligned->qWC)[3]), 0.7071067811865476, 1e-9);
}

BOOST_AUTO_TEST_CASE(InterpolationAddsClockErrorCovarianceAtMaximumPoseGap)
{
  uint64_t steady = 0;
  auto ingress = makeIngress(steady);
  BOOST_REQUIRE(ingress.submitCalibration(calibration()).accepted());
  BOOST_REQUIRE(ingress.submitGround(ground()).accepted());

  auto first = pose(1, 950000, 0.0);
  auto second = pose(2, 1050000, 1.0);
  first.clockBinding = clock(2, 5000);
  second.clockBinding = clock(2, 5000);
  std::array<double, 36> zeroCovariance{};
  first.poseCovariance = zeroCovariance;
  second.poseCovariance = zeroCovariance;
  first.qWC = {0.0, 1.0, 0.0, 0.0};
  second.qWC = {0.0, std::cos(0.1), std::sin(0.1), 0.0};
  BOOST_REQUIRE(ingress.submitPose(first).accepted());
  BOOST_REQUIRE(ingress.submitPose(second).accepted());

  auto captured = frame();
  captured.clockBinding = clock(2, 5000);
  const auto key = captured.key;
  BOOST_REQUIRE(ingress.submitFrame(captured).accepted());
  const auto aligned = ingress.take(key, steady);
  BOOST_REQUIRE(aligned.status == AlignedObservationStatus::Ready);
  BOOST_REQUIRE(aligned.aligned);
  BOOST_CHECK(aligned.aligned->poseStatus == PoseStatus::Aligned);
  BOOST_CHECK_EQUAL(aligned.aligned->timeUncertaintyUs, 10000U);
  BOOST_REQUIRE(aligned.aligned->poseCovariance);
  BOOST_CHECK_CLOSE((*aligned.aligned->poseCovariance)[0], 0.01, 1e-8);
  BOOST_CHECK_CLOSE((*aligned.aligned->poseCovariance)[5], 0.002, 1e-8);
  BOOST_CHECK_CLOSE((*aligned.aligned->poseCovariance)[30], 0.002, 1e-8);
  BOOST_CHECK_CLOSE((*aligned.aligned->poseCovariance)[35], 0.0004, 1e-8);
}

BOOST_AUTO_TEST_CASE(PoseBracketOverMaximumGapNeverExtrapolates)
{
  uint64_t steady = 5000;
  auto ingress = makeIngress(steady);
  BOOST_REQUIRE(ingress.submitCalibration(calibration()).accepted());
  BOOST_REQUIRE(ingress.submitGround(ground()).accepted());
  BOOST_REQUIRE(ingress.submitPose(pose(1, 949999, 0.0)).accepted());
  BOOST_REQUIRE(ingress.submitPose(pose(2, 1050001, 1.0)).accepted());
  const auto captured = frame();
  BOOST_REQUIRE(ingress.submitFrame(captured).accepted());
  BOOST_CHECK(ingress.take(captured.key, steady + 99999).status ==
              AlignedObservationStatus::Pending);
  const auto aligned = ingress.take(captured.key, steady + 100000);
  BOOST_REQUIRE(aligned.status == AlignedObservationStatus::Ready);
  BOOST_REQUIRE(aligned.aligned);
  BOOST_CHECK(aligned.aligned->poseStatus == PoseStatus::Unknown);
  BOOST_CHECK_EQUAL(aligned.aligned->reason, "POSE_UNAVAILABLE");
  BOOST_CHECK(aligned.aligned->poseEndpoints.empty());
  BOOST_CHECK(!aligned.aligned->positionW);
}

BOOST_AUTO_TEST_CASE(ClockUncertaintyBoundaryIsAcceptedButNotMisrepresentedAsExact)
{
  uint64_t steady = 0;
  auto ingress = makeIngress(steady);
  BOOST_REQUIRE(ingress.submitCalibration(calibration()).accepted());
  BOOST_REQUIRE(ingress.submitGround(ground()).accepted());
  auto sample = pose(1, 1000000, 0.0);
  sample.clockBinding = clock(2, 10000);
  BOOST_REQUIRE(ingress.submitPose(sample).accepted());
  auto captured = frame();
  captured.clockBinding = clock(2, 10000);
  BOOST_REQUIRE(ingress.submitFrame(captured).accepted());
  const auto aligned = ingress.take(captured.key, steady);
  BOOST_REQUIRE(aligned.status == AlignedObservationStatus::Ready);
  BOOST_REQUIRE(aligned.aligned);
  BOOST_CHECK_EQUAL(aligned.aligned->timeUncertaintyUs, 20000U);
  BOOST_CHECK(aligned.aligned->poseStatus == PoseStatus::Unknown);
  BOOST_CHECK_EQUAL(aligned.aligned->reason, "TIME_UNCERTAINTY_UNBOUNDED");

  uint64_t overSteady = 0;
  auto overIngress = makeIngress(overSteady);
  BOOST_REQUIRE(overIngress.submitCalibration(calibration()).accepted());
  auto over = frame();
  over.clockBinding = clock(2, 20001);
  BOOST_CHECK_EQUAL(overIngress.submitFrame(over).reason, "INVALID_FRAME");
  auto overPose = pose(1, 900000, 0.0);
  overPose.clockBinding = clock(2, 20001);
  BOOST_CHECK_EQUAL(overIngress.submitPose(overPose).reason, "INVALID_POSE");
}

BOOST_AUTO_TEST_CASE(ClockBindingChangesAreIdentityConflicts)
{
  uint64_t steady = 0;
  auto ingress = makeIngress(steady);
  BOOST_REQUIRE(ingress.submitCalibration(calibration()).accepted());
  auto sample = pose(1, 900000, 0.0);
  BOOST_REQUIRE(ingress.submitPose(sample).accepted());
  auto changedPose = sample;
  changedPose.clockBinding.bUs = 1.0;
  BOOST_CHECK_EQUAL(ingress.submitPose(changedPose).reason, "IDENTITY_CONFLICT");

  auto captured = frame();
  BOOST_REQUIRE(ingress.submitFrame(captured).accepted());
  auto changedFrame = captured;
  changedFrame.clockBinding.bUs = 1.0;
  BOOST_CHECK_EQUAL(ingress.submitFrame(changedFrame).reason, "IDENTITY_CONFLICT");
}

BOOST_AUTO_TEST_CASE(PoseAlignmentRequiresTheCompleteFrameClockBinding)
{
  const auto baseClock = frame().clockBinding;
  std::array<ClockBinding, 4> mismatches{baseClock, baseClock, baseClock, baseClock};
  ++mismatches[0].revision;
  mismatches[1].a = 1.0001;
  mismatches[2].bUs = 1.0;
  mismatches[3].validFromUs = 1;

  for (const auto& poseClock : mismatches) {
    uint64_t steady = 0;
    auto ingress = makeIngress(steady);
    BOOST_REQUIRE(ingress.submitCalibration(calibration()).accepted());
    BOOST_REQUIRE(ingress.submitGround(ground()).accepted());
    auto sample = pose(1, 1000000, 3.0);
    sample.clockBinding = poseClock;
    BOOST_REQUIRE(ingress.submitPose(sample).accepted());
    const auto captured = frame();
    BOOST_REQUIRE(ingress.submitFrame(captured).accepted());

    const auto result = ingress.take(captured.key, steady + 100000);
    BOOST_REQUIRE(result.status == AlignedObservationStatus::Ready);
    BOOST_REQUIRE(result.aligned);
    BOOST_CHECK(result.aligned->poseStatus == PoseStatus::Unknown);
    BOOST_CHECK_EQUAL(result.aligned->reason, "POSE_UNAVAILABLE");
    BOOST_CHECK(result.aligned->poseEndpoints.empty());
  }
}

BOOST_AUTO_TEST_CASE(DeadlineReturnsReadyUnknownWithoutExtrapolation)
{
  uint64_t steady = 5000;
  auto ingress = makeIngress(steady);
  BOOST_REQUIRE(ingress.submitCalibration(calibration()).status ==
                ObservationAdmissionStatus::Accepted);
  auto captured = frame();
  const auto key = captured.key;
  BOOST_REQUIRE(ingress.submitFrame(std::move(captured)).accepted());
  BOOST_CHECK(ingress.take(key, steady + 99999).status ==
              AlignedObservationStatus::Pending);
  auto ready = ingress.take(key, steady + 100000);
  BOOST_REQUIRE(ready.status == AlignedObservationStatus::Ready);
  BOOST_REQUIRE(ready.aligned);
  BOOST_CHECK(ready.aligned->poseStatus == PoseStatus::Unknown);
  BOOST_CHECK_EQUAL(ready.aligned->reason, "POSE_UNAVAILABLE");
  BOOST_CHECK(!ready.aligned->positionW);
}

BOOST_AUTO_TEST_CASE(RejectsMalformedRecordsWithoutAdvancingFrameSequence)
{
  uint64_t steady = 0;
  auto ingress = makeIngress(steady);
  BOOST_REQUIRE(ingress.submitCalibration(calibration()).accepted());

  auto badPose = pose(1, 900000, 1.0);
  badPose.qWC = {2.0, 0.0, 0.0, 0.0};
  BOOST_CHECK_EQUAL(ingress.submitPose(std::move(badPose)).reason, "INVALID_POSE");
  auto goodPose = pose(1, 900000, 1.0);
  BOOST_CHECK(ingress.submitPose(std::move(goodPose)).accepted());

  auto bad = frame(1);
  bad.clockBinding.a = 0.0;
  BOOST_CHECK_EQUAL(ingress.submitFrame(std::move(bad)).reason, "INVALID_FRAME");
  auto good = frame(1);
  const auto key = good.key;
  BOOST_CHECK(ingress.submitFrame(good).accepted());
  BOOST_CHECK(ingress.submitFrame(good).status == ObservationAdmissionStatus::Duplicate);
  good.imageBytes[1] = 0x00;
  BOOST_CHECK_EQUAL(ingress.submitFrame(std::move(good)).reason, "IDENTITY_CONFLICT");
  BOOST_CHECK(ingress.take(key, 100000).status == AlignedObservationStatus::Ready);
}

BOOST_AUTO_TEST_CASE(ExternalCalibrationReferenceIsValidatedAndRevisionBound)
{
  uint64_t steady = 0;
  auto ingress = makeIngress(steady);
  auto record = calibration();
  record.externalDataRef = DataRef{
    "/calibration/UAV/CALIBRATION/UAV1/3/v=1", "sha256:" + std::string(64, 'a'),
    "/calibration"};
  BOOST_CHECK(ingress.submitCalibration(record).accepted());
  BOOST_CHECK(ingress.submitCalibration(record).status == ObservationAdmissionStatus::Duplicate);

  auto conflicting = record;
  conflicting.externalDataRef = DataRef{
    "/another-calibration/UAV/CALIBRATION/UAV1/3/v=1",
    "sha256:" + std::string(64, 'a'), "/another-calibration"};
  BOOST_CHECK_EQUAL(ingress.submitCalibration(conflicting).reason, "IDENTITY_CONFLICT");

  uint64_t invalidSteady = 0;
  auto invalidIngress = makeIngress(invalidSteady);
  auto invalid = calibration();
  invalid.externalDataRef = DataRef{"not-a-name", "sha256:bad", "/calibration"};
  BOOST_CHECK_EQUAL(invalidIngress.submitCalibration(invalid).reason, "INVALID_CALIBRATION");
}

BOOST_AUTO_TEST_CASE(BoundsEpochAndCloseAreEnforced)
{
  uint64_t steady = 0;
  ObservationPolicy policy;
  policy.maxQueuedFrames = 1;
  policy.maxQueuedBytes = 4;
  auto ingress = makeIngress(steady, policy);
  BOOST_REQUIRE(ingress.submitCalibration(calibration()).accepted());
  auto first = frame(1);
  BOOST_REQUIRE(ingress.submitFrame(first).accepted());
  BOOST_CHECK_EQUAL(ingress.submitFrame(frame(2)).reason, "QUEUE_FULL");

  auto wrongEpoch = frame(2);
  wrongEpoch.key.epoch = 8;
  BOOST_CHECK_EQUAL(ingress.submitFrame(std::move(wrongEpoch)).reason, "IDENTITY_MISMATCH");
  ingress.close();
  ingress.close();
  BOOST_CHECK(ingress.take(first.key, 0).status == AlignedObservationStatus::Closed);
  BOOST_CHECK(ingress.submitFrame(frame(2)).status == ObservationAdmissionStatus::Closed);
  BOOST_CHECK_EQUAL(ingress.queuedBytes(), 0U);
}

BOOST_AUTO_TEST_CASE(SourceSwitchClosesOldEpochAndRejectsLateRecords)
{
  uint64_t steady = 0;
  const auto oldIdentity = identity();
  ObservationIngress oldSource(oldIdentity, {}, [&steady] { return steady; });
  BOOST_REQUIRE(oldSource.submitCalibration(calibration()).accepted());
  BOOST_REQUIRE(oldSource.submitGround(ground()).accepted());
  auto oldPose = pose(1, 1000000, 1.0);
  auto oldFrame = frame(1, 1000000);
  BOOST_REQUIRE(oldSource.submitPose(oldPose).accepted());
  BOOST_REQUIRE(oldSource.submitFrame(oldFrame).accepted());

  oldSource.close();
  BOOST_CHECK(oldSource.submitPose(std::move(oldPose)).status ==
              ObservationAdmissionStatus::Closed);
  BOOST_CHECK(oldSource.take(oldFrame.key, steady).status == AlignedObservationStatus::Closed);

  auto nextIdentity = oldIdentity;
  nextIdentity.sourceId = "source-2";
  nextIdentity.epoch = oldIdentity.epoch + 1;
  ObservationIngress newSource(nextIdentity, {}, [&steady] { return steady; });
  BOOST_REQUIRE(newSource.submitCalibration(calibration()).accepted());
  BOOST_REQUIRE(newSource.submitGround(ground()).accepted());
  BOOST_CHECK_EQUAL(newSource.submitPose(pose(2, 1000000, 2.0)).reason, "INVALID_POSE");
  BOOST_CHECK_EQUAL(newSource.submitFrame(oldFrame).reason, "IDENTITY_MISMATCH");

  auto currentPose = pose(1, 1000000, 3.0);
  currentPose.identity = nextIdentity;
  auto currentFrame = frame(1, 1000000);
  currentFrame.key.sourceId = nextIdentity.sourceId;
  currentFrame.key.epoch = nextIdentity.epoch;
  BOOST_REQUIRE(newSource.submitPose(std::move(currentPose)).accepted());
  BOOST_REQUIRE(newSource.submitFrame(currentFrame).accepted());
  const auto ready = newSource.take(currentFrame.key, steady);
  BOOST_REQUIRE(ready.status == AlignedObservationStatus::Ready);
  BOOST_REQUIRE(ready.aligned);
  BOOST_CHECK(ready.aligned->poseStatus == PoseStatus::Aligned);
  BOOST_CHECK_EQUAL((*ready.aligned->positionW)[0], 3.0);
  BOOST_CHECK_EQUAL(ready.aligned->frame.key.sourceId, nextIdentity.sourceId);
  BOOST_CHECK_EQUAL(ready.aligned->frame.key.epoch, nextIdentity.epoch);
}

BOOST_AUTO_TEST_CASE(PipeSourceSwitchRotatesIdentityAndRejectsLateFrame)
{
  uint64_t steady = 0;
  auto oldIdentity = identity();
  oldIdentity.deliveryMode = DeliveryMode::Replay;
  ObservationIngress oldIngress(oldIdentity, {}, [&steady] { return steady; });

  int oldPipe[2] = {-1, -1};
  BOOST_REQUIRE_EQUAL(::pipe(oldPipe), 0);
  writeAll(oldPipe[1], CALIBRATION_EVENT + "\n" + FRAME_EVENT + "\n");
  BOOST_REQUIRE_EQUAL(::close(oldPipe[1]), 0);
  StreamObservationSource oldStream(oldPipe[0], oldIdentity, {});
  oldStream.start(oldIngress);
  const auto oldResult = oldStream.pump();
  oldStream.stop();
  BOOST_REQUIRE_EQUAL(oldResult.acceptedEvents, 2U);
  BOOST_CHECK(oldResult.eof);
  BOOST_CHECK_EQUAL(oldIngress.queuedFrames(), 1U);
  oldIngress.close();

  auto nextIdentity = oldIdentity;
  nextIdentity.sourceId = "source-2";
  ++nextIdentity.epoch;
  nextIdentity.deliveryMode = DeliveryMode::LivePush;
  ObservationIngress nextIngress(nextIdentity, {}, [&steady] { return steady; });

  auto currentFrameEvent = FRAME_EVENT;
  auto replaceField = [&currentFrameEvent] (const std::string& before,
                                             const std::string& after) {
    const auto position = currentFrameEvent.find(before);
    BOOST_REQUIRE(position != std::string::npos);
    currentFrameEvent.replace(position, before.size(), after);
  };
  replaceField("\"sourceId\":\"source-1\"", "\"sourceId\":\"source-2\"");
  replaceField("\"epoch\":\"7\"", "\"epoch\":\"8\"");

  int nextPipe[2] = {-1, -1};
  BOOST_REQUIRE_EQUAL(::pipe(nextPipe), 0);
  writeAll(nextPipe[1], CALIBRATION_EVENT + "\n" + FRAME_EVENT + "\n" +
                       currentFrameEvent + "\n");
  BOOST_REQUIRE_EQUAL(::close(nextPipe[1]), 0);
  StreamObservationSource nextStream(nextPipe[0], nextIdentity, {});
  nextStream.start(nextIngress);
  const auto nextResult = nextStream.pump();
  nextStream.stop();

  BOOST_CHECK(nextResult.eof);
  BOOST_CHECK_EQUAL(nextResult.acceptedEvents, 2U);
  BOOST_CHECK_EQUAL(nextResult.rejectedEvents, 1U);
  BOOST_CHECK_EQUAL(nextResult.lastError, "IDENTITY_MISMATCH");
  // The late old-epoch sequence did not advance the new ingress frontier.
  BOOST_CHECK_EQUAL(nextIngress.queuedFrames(), 1U);
  BOOST_CHECK(nextIngress.identity().sourceId == "source-2");
  BOOST_CHECK_EQUAL(nextIngress.identity().epoch, 8U);

  bool stoppedPumpRejected = false;
  try {
    nextStream.pump();
  }
  catch (const std::logic_error&) {
    stoppedPumpRejected = true;
  }
  BOOST_CHECK(stoppedPumpRejected);
  nextIngress.close();
}

BOOST_AUTO_TEST_CASE(RejectsNonConvexGroundAndNonPositiveSemidefiniteCovariance)
{
  uint64_t steady = 0;
  auto ingress = makeIngress(steady);
  auto invalidGround = ground();
  invalidGround.roiVertices = {{{0, 0, 0}}, {{10, 0, 0}}, {{2, 2, 0}},
                               {{10, 10, 0}}, {{0, 10, 0}}};
  BOOST_CHECK_EQUAL(ingress.submitGround(std::move(invalidGround)).reason, "INVALID_GROUND");

  BOOST_REQUIRE(ingress.submitCalibration(calibration()).accepted());
  auto badPose = pose(1, 1000, 0.0);
  std::array<double, 36> covariance{};
  covariance[0] = -1.0;
  badPose.poseCovariance = covariance;
  BOOST_CHECK_EQUAL(ingress.submitPose(std::move(badPose)).reason, "INVALID_POSE");
}

BOOST_AUTO_TEST_CASE(StreamReadsTypedEventsOnCallerExecutor)
{
  int descriptors[2] = {-1, -1};
  BOOST_REQUIRE_EQUAL(::pipe(descriptors), 0);
  const std::string input = CALIBRATION_EVENT + "\n" + FRAME_EVENT + "\n";
  writeAll(descriptors[1], input);
  BOOST_REQUIRE_EQUAL(::close(descriptors[1]), 0);

  RecordingObservationSink sink;
  StreamObservationSource source(descriptors[0], identity(), {});
  source.start(sink);
  const auto first = source.pump(1);
  BOOST_CHECK_EQUAL(first.acceptedEvents, 1U);
  BOOST_CHECK(first.hasBufferedEvents);
  BOOST_CHECK_EQUAL(sink.calibrations, 1U);
  const auto second = source.pump(1);
  BOOST_CHECK_EQUAL(second.acceptedEvents, 1U);
  BOOST_CHECK(!second.hasBufferedEvents);
  const auto drained = source.pump();
  BOOST_CHECK(drained.eof);
  BOOST_CHECK_EQUAL(sink.frames, 1U);
  BOOST_CHECK_EQUAL(sink.calibration.width, 640U);
  BOOST_CHECK_EQUAL(sink.frame.imageBytes.size(), 4U);
  BOOST_CHECK_EQUAL(sink.frame.imageBytes[0], 0xffU);
  source.stop();
  source.stop();
}

BOOST_AUTO_TEST_CASE(FilePipeAndNativeSinkPreserveTheSameCapturedFrame)
{
  const std::string input = CALIBRATION_EVENT + "\n" + FRAME_EVENT + "\n";
  auto readFrom = [&] (int fd) {
    RecordingObservationSink sink;
    StreamObservationSource source(fd, identity(), {});
    source.start(sink);
    const auto result = source.pump();
    BOOST_CHECK_EQUAL(result.acceptedEvents, 2U);
    BOOST_CHECK(result.eof);
    source.stop();
    return sink;
  };

  int descriptors[2] = {-1, -1};
  BOOST_REQUIRE_EQUAL(::pipe(descriptors), 0);
  writeAll(descriptors[1], input);
  BOOST_REQUIRE_EQUAL(::close(descriptors[1]), 0);
  auto pipeResult = readFrom(descriptors[0]);

  char path[] = "/tmp/uav-observation-file-XXXXXX";
  const int fileFd = ::mkstemp(path);
  BOOST_REQUIRE(fileFd >= 0);
  ::unlink(path);
  writeAll(fileFd, input);
  BOOST_REQUIRE_EQUAL(::lseek(fileFd, 0, SEEK_SET), 0);
  auto fileResult = readFrom(fileFd);

  RecordingObservationSink nativeResult;
  BOOST_CHECK(nativeResult.submitCalibration(calibration()).accepted());
  BOOST_CHECK(nativeResult.submitFrame(frame(1, 1000)).accepted());

  for (const auto* result : {&pipeResult, &fileResult, &nativeResult}) {
    BOOST_CHECK_EQUAL(result->calibrations, 1U);
    BOOST_CHECK_EQUAL(result->frames, 1U);
    BOOST_CHECK_EQUAL(result->frame.key.sourceId, "source-1");
    BOOST_CHECK_EQUAL(result->frame.key.cameraId, "UAV1");
    BOOST_CHECK_EQUAL(result->frame.key.epoch, 7U);
    BOOST_CHECK_EQUAL(result->frame.key.sequence, 1U);
    BOOST_CHECK_EQUAL(result->frame.deviceTimeUs, 1000U);
    BOOST_CHECK_EQUAL(result->frame.clockBinding.clockId, "sim-clock");
    BOOST_CHECK_EQUAL(result->frame.calibrationRevision, 3U);
    BOOST_CHECK_EQUAL(result->frame.imageBytes.size(), 4U);
    BOOST_CHECK_EQUAL_COLLECTIONS(result->frame.imageBytes.begin(), result->frame.imageBytes.end(),
                                  nativeResult.frame.imageBytes.begin(),
                                  nativeResult.frame.imageBytes.end());
  }
}

BOOST_AUTO_TEST_CASE(WriterThreadFeedsSingleOwnerAndStopPreventsFurtherCallbacks)
{
  int descriptors[2] = {-1, -1};
  BOOST_REQUIRE_EQUAL(::pipe(descriptors), 0);
  const std::string input = CALIBRATION_EVENT + "\n" + FRAME_EVENT + "\n";
  const int writerFd = descriptors[1];
  bool writerOk = true;
  std::thread writer([&] {
    size_t offset = 0;
    while (offset < input.size()) {
      const size_t chunk = std::min<size_t>(17, input.size() - offset);
      const ssize_t written = ::write(writerFd, input.data() + offset, chunk);
      if (written <= 0) {
        writerOk = false;
        break;
      }
      offset += static_cast<size_t>(written);
    }
    ::close(writerFd);
  });

  RecordingObservationSink sink;
  StreamObservationSource source(descriptors[0], identity(), {});
  source.start(sink);
  size_t accepted = 0;
  bool eof = false;
  for (size_t attempt = 0; attempt < 200 && !eof; ++attempt) {
    pollfd descriptor{descriptors[0], POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, 10);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) continue;
    const auto result = source.pump();
    accepted += result.acceptedEvents;
    eof = result.eof;
  }
  writer.join();
  const size_t callbacksBeforeStop = sink.calibrations + sink.frames;
  source.stop();
  bool rejectedAfterStop = false;
  try {
    source.pump();
  }
  catch (const std::logic_error&) {
    rejectedAfterStop = true;
  }
  BOOST_CHECK(writerOk);
  BOOST_CHECK(eof);
  BOOST_CHECK_EQUAL(accepted, 2U);
  BOOST_CHECK_EQUAL(sink.calibrations, 1U);
  BOOST_CHECK_EQUAL(sink.frames, 1U);
  BOOST_CHECK_EQUAL(sink.calibrations + sink.frames, callbacksBeforeStop);
  BOOST_CHECK(rejectedAfterStop);
}

BOOST_AUTO_TEST_CASE(CrossThreadStopWaitsForActivePumpBeforeClosingOwnedDescriptor)
{
  int descriptors[2] = {-1, -1};
  BOOST_REQUIRE_EQUAL(::pipe(descriptors), 0);
  writeAll(descriptors[1], CALIBRATION_EVENT + "\n");
  BOOST_REQUIRE_EQUAL(::close(descriptors[1]), 0);

  BlockingObservationSink sink;
  StreamObservationSource source(descriptors[0], identity(), {});
  source.start(sink);
  StreamPumpResult result;
  std::thread pump([&] { result = source.pump(1); });
  const bool callbackEntered = sink.waitUntilEntered(std::chrono::seconds(2));

  std::mutex stopMutex;
  std::condition_variable stopCondition;
  bool stopStarted = false;
  bool stopCompleted = false;
  std::thread stopper([&] {
    {
      std::lock_guard<std::mutex> lock(stopMutex);
      stopStarted = true;
    }
    stopCondition.notify_all();
    source.stop();
    {
      std::lock_guard<std::mutex> lock(stopMutex);
      stopCompleted = true;
    }
    stopCondition.notify_all();
  });

  bool stopperStarted = false;
  bool stopCompletedBeforeRelease = false;
  {
    std::unique_lock<std::mutex> lock(stopMutex);
    stopperStarted = stopCondition.wait_for(lock, std::chrono::seconds(2),
                                            [&] { return stopStarted; });
    if (callbackEntered && stopperStarted) {
      stopCompletedBeforeRelease = stopCondition.wait_for(
        lock, std::chrono::milliseconds(100), [&] { return stopCompleted; });
    }
  }

  sink.release();
  const bool callbackReleased = sink.waitUntilReleased(std::chrono::seconds(2));
  pump.join();
  stopper.join();

  BOOST_CHECK(callbackEntered);
  BOOST_CHECK(stopperStarted);
  BOOST_CHECK(callbackReleased);
  BOOST_CHECK(!stopCompletedBeforeRelease);
  BOOST_CHECK_EQUAL(result.acceptedEvents, 1U);
  BOOST_CHECK_EQUAL(result.rejectedEvents, 0U);
  BOOST_CHECK(stopCompleted);
  bool stoppedPumpRejected = false;
  try {
    source.pump();
  }
  catch (const std::logic_error&) {
    stoppedPumpRejected = true;
  }
  BOOST_CHECK(stoppedPumpRejected);

  auto nextIdentity = identity();
  nextIdentity.sourceId = "source-2";
  ++nextIdentity.epoch;
  ObservationIngress nextIngress(nextIdentity, {}, [] { return uint64_t{0}; });
  auto nextFrameEvent = FRAME_EVENT;
  const auto sourceField = nextFrameEvent.find("\"sourceId\":\"source-1\"");
  BOOST_REQUIRE(sourceField != std::string::npos);
  nextFrameEvent.replace(sourceField, std::string("\"sourceId\":\"source-1\"").size(),
                         "\"sourceId\":\"source-2\"");
  const auto epochField = nextFrameEvent.find("\"epoch\":\"7\"");
  BOOST_REQUIRE(epochField != std::string::npos);
  nextFrameEvent.replace(epochField, std::string("\"epoch\":\"7\"").size(),
                         "\"epoch\":\"8\"");
  int nextPipe[2] = {-1, -1};
  BOOST_REQUIRE_EQUAL(::pipe(nextPipe), 0);
  writeAll(nextPipe[1], CALIBRATION_EVENT + "\n" + FRAME_EVENT + "\n" +
                       nextFrameEvent + "\n");
  BOOST_REQUIRE_EQUAL(::close(nextPipe[1]), 0);
  StreamObservationSource nextSource(nextPipe[0], nextIdentity, {});
  nextSource.start(nextIngress);
  const auto nextResult = nextSource.pump();
  nextSource.stop();
  BOOST_CHECK(nextResult.eof);
  BOOST_CHECK_EQUAL(nextResult.acceptedEvents, 2U);
  BOOST_CHECK_EQUAL(nextResult.rejectedEvents, 1U);
  BOOST_CHECK_EQUAL(nextResult.lastError, "IDENTITY_MISMATCH");
  BOOST_CHECK_EQUAL(nextIngress.queuedFrames(), 1U);
  BOOST_CHECK_EQUAL(nextIngress.identity().sourceId, "source-2");
  BOOST_CHECK_EQUAL(nextIngress.identity().epoch, 8U);
  nextIngress.close();
}

BOOST_AUTO_TEST_CASE(StreamPreservesExternalCalibrationDataReference)
{
  int descriptors[2] = {-1, -1};
  BOOST_REQUIRE_EQUAL(::pipe(descriptors), 0);
  std::string input = CALIBRATION_EVENT;
  const std::string reference =
    ",\"externalDataRef\":{\"exactName\":\"/calibration/UAV/CALIBRATION/UAV1/3/v=1\","
    "\"contentSha256\":\"sha256:" + std::string(64, 'a') +
    "\",\"producerIdentity\":\"/calibration\"}";
  input.insert(input.size() - 2, reference);
  writeAll(descriptors[1], input + "\n");
  BOOST_REQUIRE_EQUAL(::close(descriptors[1]), 0);

  RecordingObservationSink sink;
  StreamObservationSource source(descriptors[0], identity(), {});
  source.start(sink);
  const auto result = source.pump();
  BOOST_CHECK_EQUAL(result.acceptedEvents, 1U);
  BOOST_REQUIRE(sink.calibration.externalDataRef);
  BOOST_CHECK_EQUAL(sink.calibration.externalDataRef->exactName,
                    "/calibration/UAV/CALIBRATION/UAV1/3/v=1");
  BOOST_CHECK_EQUAL(sink.calibration.externalDataRef->producerIdentity, "/calibration");
}

BOOST_AUTO_TEST_CASE(StreamRejectsWrongScalarTypesAndDuplicateKeys)
{
  int descriptors[2] = {-1, -1};
  BOOST_REQUIRE_EQUAL(::pipe(descriptors), 0);
  auto numeric = CALIBRATION_EVENT;
  const auto revision = numeric.find("\"revision\":\"3\"");
  BOOST_REQUIRE(revision != std::string::npos);
  numeric.replace(revision, std::string("\"revision\":\"3\"").size(), "\"revision\":3");
  const std::string duplicate =
    R"({"schema":"uav-source-event-v2","kind":"calibration","kind":"frame","payload":{}})";
  writeAll(descriptors[1], numeric + "\n" + duplicate + "\n" + CALIBRATION_EVENT + "\n");
  BOOST_REQUIRE_EQUAL(::close(descriptors[1]), 0);

  RecordingObservationSink sink;
  StreamObservationSource source(descriptors[0], identity(), {});
  source.start(sink);
  const auto result = source.pump();
  BOOST_CHECK_EQUAL(result.rejectedEvents, 2U);
  BOOST_CHECK_EQUAL(result.acceptedEvents, 1U);
  BOOST_CHECK_EQUAL(result.lastError, "INVALID_EVENT");
  BOOST_CHECK(result.eof);
  BOOST_CHECK_EQUAL(sink.calibrations, 1U);
}

BOOST_AUTO_TEST_CASE(StreamRejectsExcessiveJsonNestingBeforeRecursiveParse)
{
  int descriptors[2] = {-1, -1};
  BOOST_REQUIRE_EQUAL(::pipe(descriptors), 0);
  std::string deeplyNested =
    R"({"schema":"uav-source-event-v2","kind":"calibration","payload":)";
  deeplyNested.append(65, '[');
  deeplyNested.append(65, ']');
  deeplyNested += "}\n";
  writeAll(descriptors[1], deeplyNested + CALIBRATION_EVENT + "\n");
  BOOST_REQUIRE_EQUAL(::close(descriptors[1]), 0);

  RecordingObservationSink sink;
  StreamObservationSource source(descriptors[0], identity(), {});
  source.start(sink);
  const auto result = source.pump();
  BOOST_CHECK_EQUAL(result.rejectedEvents, 1U);
  BOOST_CHECK_EQUAL(result.acceptedEvents, 1U);
  BOOST_CHECK_EQUAL(result.lastError, "INVALID_EVENT");
  BOOST_CHECK(result.eof);
  BOOST_CHECK_EQUAL(sink.calibrations, 1U);
}

BOOST_AUTO_TEST_CASE(StreamRejectsOversizedAndTruncatedFinalLines)
{
  char path[] = "/tmp/uav-observation-stream-XXXXXX";
  const int fd = ::mkstemp(path);
  BOOST_REQUIRE(fd >= 0);
  ::unlink(path);
  std::string large(12 * 1024 * 1024 + 1, 'x');
  large += "\n" + CALIBRATION_EVENT;
  writeAll(fd, large);
  BOOST_REQUIRE_EQUAL(::lseek(fd, 0, SEEK_SET), 0);

  RecordingObservationSink sink;
  StreamObservationSource source(fd, identity(), {});
  source.start(sink);
  const auto first = source.pump(1);
  BOOST_CHECK_EQUAL(first.rejectedEvents, 1U);
  BOOST_CHECK_EQUAL(first.lastError, "LINE_TOO_LARGE");
  const auto second = source.pump();
  BOOST_CHECK_EQUAL(second.acceptedEvents, 0U);
  BOOST_CHECK_EQUAL(second.rejectedEvents, 1U);
  BOOST_CHECK_EQUAL(second.lastError, "TRUNCATED_LINE");
  BOOST_CHECK(second.eof);
}

BOOST_AUTO_TEST_SUITE_END()
