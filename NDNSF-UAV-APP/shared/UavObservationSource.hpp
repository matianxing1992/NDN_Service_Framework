#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ndnsf::examples::uav {

enum class AcquisitionKind { Simulated, Measured };
enum class DeliveryMode { Generated, Replay, LivePush };
enum class ObservationAdmissionStatus { Accepted, Duplicate, Rejected, Closed };
enum class AlignedObservationStatus { Pending, Ready, Rejected, Closed };
enum class PoseStatus { Aligned, Unknown };

struct SourceIdentity
{
  std::string sourceId;
  std::string droneId;
  std::string cameraId;
  uint64_t epoch = 0;
  std::string worldFrameId;
  AcquisitionKind acquisitionKind = AcquisitionKind::Simulated;
  DeliveryMode deliveryMode = DeliveryMode::Generated;
};

struct FrameKey
{
  std::string sourceId;
  std::string cameraId;
  uint64_t epoch = 0;
  uint64_t sequence = 0;

  bool operator<(const FrameKey& other) const noexcept;
  bool operator==(const FrameKey& other) const noexcept;
};

/** A producer-declared lost capture range; it carries no invented frame/time. */
struct FrameGap
{
  std::string schema = "uav-frame-gap-v2";
  SourceIdentity identity;
  uint64_t firstSequence = 0;
  uint64_t lastSequence = 0;
  std::string reason = "CAPTURE_DROPPED";
};

struct ClockBinding
{
  uint64_t revision = 0;
  std::string clockId;
  uint64_t clockEpoch = 0;
  double a = 1.0;
  double bUs = 0.0;
  uint64_t validFromUs = 0;
  uint64_t validUntilUs = 0;
  uint64_t uncertaintyUs = 0;
};

struct DataRef
{
  std::string exactName;
  std::string contentSha256;
  std::string producerIdentity;
};

struct CalibrationRecord
{
  std::string schema = "uav-calibration-v2";
  std::string cameraId;
  uint64_t revision = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  double fx = 0.0;
  double fy = 0.0;
  double cx = 0.0;
  double cy = 0.0;
  std::array<double, 5> distortion{};
  std::string imageTransform = "raw";
  uint64_t validFromUs = 0;
  uint64_t validUntilUs = 0;
  std::optional<std::array<double, 81>> parameterCovariance;
  std::optional<double> anchorSigmaPixels;
  /** Existing signed calibration object; omitted means the source publishes it. */
  std::optional<DataRef> externalDataRef;
};

struct GroundRecord
{
  std::string schema = "uav-ground-v2";
  uint64_t revision = 0;
  std::string worldFrameId;
  std::array<double, 3> normal{0.0, 0.0, 1.0};
  double offsetM = 0.0;
  std::vector<std::array<double, 3>> roiVertices;
  std::string geometryValidity = "illustrative";
  std::optional<std::array<double, 16>> parameterCovariance;
};

struct PoseSample
{
  std::string schema = "uav-pose-v2";
  SourceIdentity identity;
  uint64_t sampleSequence = 0;
  uint64_t deviceTimeUs = 0;
  ClockBinding clockBinding;
  std::array<double, 3> positionW{};
  std::array<double, 4> qWC{1.0, 0.0, 0.0, 0.0};
  uint64_t calibrationRevision = 0;
  std::optional<std::array<double, 36>> poseCovariance;
};

struct CapturedFrame
{
  FrameKey key;
  uint64_t deviceTimeUs = 0;
  ClockBinding clockBinding;
  uint32_t width = 0;
  uint32_t height = 0;
  std::string encoding = "image/jpeg";
  uint64_t calibrationRevision = 0;
  std::vector<uint8_t> imageBytes;
};

struct FrameObservation
{
  std::string schema = "uav-observation-v2";
  SourceIdentity identity;
  FrameKey key;
  uint64_t captureTimeUs = 0;
  uint64_t timeUncertaintyUs = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  DataRef imageRef;
  std::optional<DataRef> calibrationRef;
  std::optional<DataRef> groundRef;
  DataRef clockRef;
  PoseStatus poseStatus = PoseStatus::Unknown;
  std::vector<DataRef> poseSampleRefs;
  std::string geometryValidity = "illustrative";
  std::string reason;
  std::optional<std::array<double, 3>> positionW;
  std::optional<std::array<double, 4>> qWC;
  std::optional<std::array<double, 36>> poseCovariance;
};

struct ObservationPolicy
{
  uint64_t maxPoseGapUs = 100000;
  uint64_t maxClockUncertaintyUs = 20000;
  uint64_t maxAlignWaitMs = 100;
  uint64_t poseRetentionUs = 5000000;
  size_t maxPoseSamples = 512;
  size_t maxQueuedFrames = 2;
  size_t maxQueuedBytes = 16 * 1024 * 1024;
  uint64_t maxSourceAgeMs = 1000;
};

struct Admission
{
  ObservationAdmissionStatus status = ObservationAdmissionStatus::Rejected;
  std::string reason;

  bool accepted() const noexcept { return status == ObservationAdmissionStatus::Accepted; }
};

struct AlignedFrame
{
  CapturedFrame frame;
  SourceIdentity identity;
  uint64_t captureTimeUs = 0;
  uint64_t timeUncertaintyUs = 0;
  std::optional<CalibrationRecord> calibration;
  std::optional<GroundRecord> ground;
  std::vector<PoseSample> poseEndpoints;
  PoseStatus poseStatus = PoseStatus::Unknown;
  std::string reason;
  std::optional<std::array<double, 3>> positionW;
  std::optional<std::array<double, 4>> qWC;
  std::optional<std::array<double, 36>> poseCovariance;
};

struct AlignedObservation
{
  AlignedObservationStatus status = AlignedObservationStatus::Rejected;
  std::optional<AlignedFrame> aligned;
  std::string reason;
};

/**
 * Source-neutral admission and camera-time alignment boundary.
 *
 * The source executor serializes every submit/take/close call. Inputs are
 * moved into bounded owned storage. Capture/clock timestamps describe the
 * camera timeline; deadlines use only the injected steady clock. close()
 * discards pending frames and makes subsequent calls return CLOSED. A source
 * must stop and join any reader before destroying this sink.
 */
class IObservationSink
{
public:
  virtual ~IObservationSink() = default;
  virtual Admission submitCalibration(CalibrationRecord record) = 0;
  virtual Admission submitGround(GroundRecord record) = 0;
  virtual Admission submitPose(PoseSample sample) = 0;
  virtual Admission submitFrame(CapturedFrame frame) = 0;
  virtual Admission submitGap(FrameGap gap) = 0;
};

class IObservationSource
{
public:
  virtual ~IObservationSource() = default;
  virtual void start(IObservationSink& sink) = 0;
  virtual void stop() noexcept = 0;
};

struct StreamPumpResult
{
  size_t acceptedEvents = 0;
  size_t duplicateEvents = 0;
  size_t rejectedEvents = 0;
  bool hasBufferedEvents = false;
  bool eof = false;
  std::string lastError;
};

/**
 * Bounded JSONL adapter that owns its file descriptor. pump() serializes sink
 * callbacks on its caller; it never starts a callback thread. stop() may be
 * called from another thread and waits for an active pump before closing the
 * descriptor or clearing its buffers. This keeps real pipe writers, file
 * replay, and simulation on the identical validation/alignment path without
 * borrowing caller buffers.
 */
class StreamObservationSource final : public IObservationSource
{
public:
  StreamObservationSource(int ownedFd, SourceIdentity identity, ObservationPolicy policy);
  ~StreamObservationSource() override;
  StreamObservationSource(const StreamObservationSource&) = delete;
  StreamObservationSource& operator=(const StreamObservationSource&) = delete;

  void start(IObservationSink& sink) override;
  StreamPumpResult pump(size_t maxEvents = 64);
  void stop() noexcept override;

private:
  Admission processLine(std::string_view line);
  void stopLocked() noexcept;

private:
  std::mutex m_mutex;
  int m_fd;
  const SourceIdentity m_identity;
  const ObservationPolicy m_policy;
  IObservationSink* m_sink = nullptr;
  bool m_started = false;
  bool m_stopped = false;
  bool m_eof = false;
  bool m_discardingLine = false;
  std::string m_currentLine;
  std::string m_pendingInput;
};

class ObservationIngress final : public IObservationSink
{
public:
  ObservationIngress(SourceIdentity identity, ObservationPolicy policy,
                    std::function<uint64_t()> steadyNowUs);

  Admission submitCalibration(CalibrationRecord record) override;
  Admission submitGround(GroundRecord record) override;
  Admission submitPose(PoseSample sample) override;
  Admission submitFrame(CapturedFrame frame) override;
  Admission submitGap(FrameGap gap) override;
  AlignedObservation take(const FrameKey& key, uint64_t nowSteadyUs);
  void close() noexcept;

  /** Call on the source executor; the bounded returned snapshot owns its records. */
  std::vector<FrameGap> frameGaps() const { return m_frameGaps; }

  const SourceIdentity& identity() const noexcept { return m_identity; }
  size_t queuedFrames() const noexcept { return m_pending.size(); }
  size_t queuedBytes() const noexcept { return m_queuedBytes; }
  size_t poseSamples() const noexcept { return m_poses.size(); }

private:
  struct TimedPose
  {
    uint64_t runTimeUs = 0;
    PoseSample sample;
    std::string digest;
  };

  struct PendingFrame
  {
    AlignedFrame aligned;
    uint64_t deadlineSteadyUs = 0;
    std::string digest;
    size_t bytes = 0;
  };

  Admission reject(const std::string& reason) const;
  AlignedObservation align(PendingFrame& pending, uint64_t nowSteadyUs) const;
  void remember(const FrameKey& key, const std::string& digest);
  bool calibrationReferenced(uint64_t revision) const;
  bool hasFrameGap(const FrameKey& key) const noexcept;

private:
  const SourceIdentity m_identity;
  const ObservationPolicy m_policy;
  const std::function<uint64_t()> m_steadyNowUs;
  bool m_closed = false;
  uint64_t m_lastFrameSequence = 0;
  std::vector<FrameGap> m_frameGaps;
  size_t m_queuedBytes = 0;
  std::map<uint64_t, CalibrationRecord> m_calibrations;
  std::map<uint64_t, std::string> m_calibrationDigests;
  std::optional<GroundRecord> m_ground;
  std::string m_groundDigest;
  std::deque<TimedPose> m_poses;
  std::map<uint64_t, std::string> m_poseDigests;
  uint64_t m_highestPoseSequence = 0;
  std::map<FrameKey, PendingFrame> m_pending;
  std::map<FrameKey, std::string> m_seen;
  std::deque<FrameKey> m_seenOrder;
};

} // namespace ndnsf::examples::uav
