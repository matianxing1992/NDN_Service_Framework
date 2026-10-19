#include "UavObservationSource.hpp"
#include "UavDigest.hpp"

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ndnsf::examples::uav {
namespace {

constexpr uint64_t MAX_TIME_US = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
constexpr size_t MAX_IMAGE_BYTES = 8 * 1024 * 1024;
constexpr size_t MAX_SEEN_FRAMES = 128;
constexpr size_t MAX_FRAME_GAPS = 128;
constexpr size_t MAX_CALIBRATIONS = 4;

bool
validUtf8Identifier(const std::string& value)
{
  if (value.empty() || value.size() > 128) return false;
  size_t i = 0;
  while (i < value.size()) {
    const auto lead = static_cast<unsigned char>(value[i]);
    if (lead <= 0x7f) {
      if (lead == 0) return false;
      ++i;
      continue;
    }
    size_t count = 0;
    uint32_t codepoint = 0;
    if ((lead & 0xe0) == 0xc0) { count = 2; codepoint = lead & 0x1f; }
    else if ((lead & 0xf0) == 0xe0) { count = 3; codepoint = lead & 0x0f; }
    else if ((lead & 0xf8) == 0xf0) { count = 4; codepoint = lead & 0x07; }
    else return false;
    if (i + count > value.size()) return false;
    for (size_t j = 1; j < count; ++j) {
      const auto next = static_cast<unsigned char>(value[i + j]);
      if ((next & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if ((count == 2 && codepoint < 0x80) ||
        (count == 3 && codepoint < 0x800) ||
        (count == 4 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
      return false;
    }
    i += count;
  }
  return true;
}

bool
validIdentity(const SourceIdentity& identity)
{
  return validUtf8Identifier(identity.sourceId) && validUtf8Identifier(identity.droneId) &&
         validUtf8Identifier(identity.cameraId) && validUtf8Identifier(identity.worldFrameId) &&
         identity.epoch > 0 && identity.epoch <= MAX_TIME_US &&
         (identity.acquisitionKind == AcquisitionKind::Simulated ||
          identity.acquisitionKind == AcquisitionKind::Measured) &&
         (identity.deliveryMode == DeliveryMode::Generated ||
          identity.deliveryMode == DeliveryMode::Replay ||
          identity.deliveryMode == DeliveryMode::LivePush);
}

bool
sameIdentity(const SourceIdentity& a, const SourceIdentity& b)
{
  return a.sourceId == b.sourceId && a.droneId == b.droneId &&
         a.cameraId == b.cameraId && a.epoch == b.epoch &&
         a.worldFrameId == b.worldFrameId && a.acquisitionKind == b.acquisitionKind &&
         a.deliveryMode == b.deliveryMode;
}

bool
finite(double value)
{
  return std::isfinite(value);
}

template<size_t N>
bool
finiteArray(const std::array<double, N>& values)
{
  return std::all_of(values.begin(), values.end(), [] (double x) { return finite(x); });
}

template<size_t N>
bool
validCovariance(const std::array<double, N * N>& matrix)
{
  std::array<double, N * N> lower{};
  constexpr double tolerance = 1e-9;
  for (size_t row = 0; row < N; ++row) {
    for (size_t column = 0; column < N; ++column) {
      const double value = matrix[row * N + column];
      if (!finite(value) || std::abs(value - matrix[column * N + row]) > tolerance) return false;
    }
    if (matrix[row * N + row] < -tolerance) return false;
  }
  // Semidefinite Cholesky. Zero pivots are allowed only when the corresponding
  // residual column is also zero, which rejects indefinite singular matrices.
  for (size_t row = 0; row < N; ++row) {
    for (size_t column = 0; column <= row; ++column) {
      double residual = matrix[row * N + column];
      for (size_t k = 0; k < column; ++k) {
        residual -= lower[row * N + k] * lower[column * N + k];
      }
      if (row == column) {
        if (residual < -tolerance) return false;
        lower[row * N + column] = std::sqrt(std::max(0.0, residual));
      }
      else if (lower[column * N + column] > std::sqrt(tolerance)) {
        lower[row * N + column] = residual / lower[column * N + column];
      }
      else if (std::abs(residual) > tolerance) {
        return false;
      }
    }
  }
  return true;
}

bool
validClock(const ClockBinding& clock)
{
  return clock.revision > 0 && validUtf8Identifier(clock.clockId) && clock.clockEpoch > 0 &&
         finite(clock.a) && clock.a > 0.0 && finite(clock.bUs) &&
         clock.validFromUs <= clock.validUntilUs && clock.validUntilUs <= MAX_TIME_US &&
         clock.uncertaintyUs <= MAX_TIME_US;
}

bool
validExternalDataRef(const DataRef& reference)
{
  if (reference.contentSha256.size() != 71 ||
      reference.contentSha256.compare(0, 7, "sha256:") != 0 ||
      !std::all_of(reference.contentSha256.begin() + 7, reference.contentSha256.end(),
                   [] (unsigned char c) {
                     return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                   })) {
    return false;
  }
  try {
    const ndn::Name name(reference.exactName);
    const ndn::Name producer(reference.producerIdentity);
    return !name.empty() && !producer.empty() && name.toUri() == reference.exactName &&
           producer.toUri() == reference.producerIdentity && producer.isPrefixOf(name);
  }
  catch (const std::exception&) {
    return false;
  }
}

std::optional<uint64_t>
toRunTimeUs(const ClockBinding& clock, uint64_t deviceTimeUs)
{
  if (!validClock(clock) || deviceTimeUs < clock.validFromUs ||
      deviceTimeUs > clock.validUntilUs || deviceTimeUs > MAX_TIME_US) {
    return std::nullopt;
  }
  const long double value = static_cast<long double>(clock.a) * deviceTimeUs + clock.bUs;
  if (!std::isfinite(value) || value < 0.0L || value > static_cast<long double>(MAX_TIME_US)) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(std::llround(value));
}

std::string
sha256(const std::string& metadata, const std::vector<uint8_t>& bytes = {})
{
  ndn::util::Sha256 digest;
  digest << metadata;
  if (!bytes.empty()) {
    digest << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  return canonicalSha256Hex(digest.toString());
}

template<size_t N>
void
writeArray(std::ostringstream& out, const std::array<double, N>& values)
{
  out << std::setprecision(17);
  for (double value : values) out << value << ',';
}

void
writeClock(std::ostringstream& out, const ClockBinding& clock)
{
  out << std::setprecision(17) << clock.revision << '|' << clock.clockId << '|'
      << clock.clockEpoch << '|' << clock.a << '|' << clock.bUs << '|'
      << clock.validFromUs << '|' << clock.validUntilUs << '|'
      << clock.uncertaintyUs << '|';
}

std::string
digestCalibration(const CalibrationRecord& record)
{
  std::ostringstream out;
  out << record.schema << '|' << record.cameraId << '|' << record.revision << '|'
      << record.width << '|' << record.height << '|' << std::setprecision(17)
      << record.fx << '|' << record.fy << '|' << record.cx << '|' << record.cy << '|'
      << record.imageTransform << '|' << record.validFromUs << '|' << record.validUntilUs << '|';
  writeArray(out, record.distortion);
  if (record.parameterCovariance) writeArray(out, *record.parameterCovariance);
  if (record.anchorSigmaPixels) out << *record.anchorSigmaPixels;
  if (record.externalDataRef) {
    out << '|' << record.externalDataRef->exactName << '|'
        << record.externalDataRef->contentSha256 << '|'
        << record.externalDataRef->producerIdentity;
  }
  return sha256(out.str());
}

std::string
digestGround(const GroundRecord& record)
{
  std::ostringstream out;
  out << record.schema << '|' << record.revision << '|' << record.worldFrameId << '|'
      << std::setprecision(17) << record.offsetM << '|' << record.geometryValidity << '|';
  writeArray(out, record.normal);
  for (const auto& vertex : record.roiVertices) writeArray(out, vertex);
  if (record.parameterCovariance) writeArray(out, *record.parameterCovariance);
  return sha256(out.str());
}

std::string
digestPose(const PoseSample& sample)
{
  std::ostringstream out;
  out << sample.schema << '|' << sample.identity.sourceId << '|' << sample.identity.cameraId
      << '|' << sample.identity.epoch << '|' << sample.sampleSequence << '|'
      << sample.deviceTimeUs << '|';
  writeClock(out, sample.clockBinding);
  out << sample.calibrationRevision << '|';
  writeArray(out, sample.positionW);
  writeArray(out, sample.qWC);
  if (sample.poseCovariance) writeArray(out, *sample.poseCovariance);
  return sha256(out.str());
}

std::string
digestFrame(const CapturedFrame& frame)
{
  std::ostringstream out;
  out << frame.key.sourceId << '|' << frame.key.cameraId << '|' << frame.key.epoch << '|'
      << frame.key.sequence << '|' << frame.deviceTimeUs << '|';
  writeClock(out, frame.clockBinding);
  out << frame.width << '|' << frame.height << '|' << frame.encoding << '|'
      << frame.calibrationRevision << '|';
  return sha256(out.str(), frame.imageBytes);
}

bool
validCalibration(const CalibrationRecord& record, const std::string& cameraId)
{
  if (record.schema != "uav-calibration-v2" || record.cameraId != cameraId ||
      record.revision == 0 || record.width == 0 || record.height == 0 ||
      record.width > 8192 || record.height > 8192 || !finite(record.fx) ||
      !finite(record.fy) || !finite(record.cx) || !finite(record.cy) ||
      record.fx <= 0.0 || record.fy <= 0.0 || record.validFromUs > record.validUntilUs ||
      record.validUntilUs > MAX_TIME_US ||
      (record.imageTransform != "raw" && record.imageTransform != "rectified") ||
      !finiteArray(record.distortion) ||
      (record.anchorSigmaPixels &&
       (!finite(*record.anchorSigmaPixels) || *record.anchorSigmaPixels < 0.0)) ||
      (record.externalDataRef && !validExternalDataRef(*record.externalDataRef))) {
    return false;
  }
  if (record.imageTransform == "rectified" &&
      std::any_of(record.distortion.begin(), record.distortion.end(),
                  [] (double x) { return x != 0.0; })) {
    return false;
  }
  return !record.parameterCovariance || validCovariance<9>(*record.parameterCovariance);
}

bool
validGround(const GroundRecord& record, const std::string& worldFrameId)
{
  if (record.schema != "uav-ground-v2" || record.revision == 0 ||
      record.worldFrameId != worldFrameId || !finiteArray(record.normal) ||
      !finite(record.offsetM) || record.normal[2] <= 0.0 ||
      record.roiVertices.size() < 3 || record.roiVertices.size() > 32 ||
      (record.geometryValidity != "illustrative" &&
       record.geometryValidity != "calibrated-plane" &&
       record.geometryValidity != "analytic-fixture")) {
    return false;
  }
  const double norm = std::sqrt(record.normal[0] * record.normal[0] +
                                record.normal[1] * record.normal[1] +
                                record.normal[2] * record.normal[2]);
  if (!finite(norm) || std::abs(norm - 1.0) > 1e-3) return false;
  for (const auto& vertex : record.roiVertices) {
    if (!finiteArray(vertex) || std::abs(record.normal[0] * vertex[0] +
        record.normal[1] * vertex[1] + record.normal[2] * vertex[2] +
        record.offsetM) > 1e-4) return false;
  }
  double orientation = 0.0;
  const size_t count = record.roiVertices.size();
  for (size_t i = 0; i < count; ++i) {
    const auto& a = record.roiVertices[i];
    const auto& b = record.roiVertices[(i + 1) % count];
    const auto& c = record.roiVertices[(i + 2) % count];
    std::array<double, 3> ab{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    std::array<double, 3> bc{c[0] - b[0], c[1] - b[1], c[2] - b[2]};
    const double crossAlongNormal = record.normal[0] * (ab[1] * bc[2] - ab[2] * bc[1]) +
                                    record.normal[1] * (ab[2] * bc[0] - ab[0] * bc[2]) +
                                    record.normal[2] * (ab[0] * bc[1] - ab[1] * bc[0]);
    if (std::abs(crossAlongNormal) < 1e-10) return false;
    if (orientation == 0.0) orientation = crossAlongNormal;
    else if ((orientation > 0.0) != (crossAlongNormal > 0.0)) return false;
  }
  return !record.parameterCovariance || validCovariance<4>(*record.parameterCovariance);
}

bool
validPose(PoseSample& sample, const SourceIdentity& expected,
          const ObservationPolicy& policy, uint64_t& runTimeUs)
{
  if (sample.schema != "uav-pose-v2" || !sameIdentity(sample.identity, expected) ||
      sample.sampleSequence == 0 || sample.sampleSequence > MAX_TIME_US ||
      sample.deviceTimeUs > MAX_TIME_US || !validClock(sample.clockBinding) ||
      sample.clockBinding.uncertaintyUs > policy.maxClockUncertaintyUs ||
      sample.calibrationRevision == 0 || !finiteArray(sample.positionW) ||
      std::any_of(sample.positionW.begin(), sample.positionW.end(),
                  [] (double x) { return std::abs(x) > 1e7; }) || !finiteArray(sample.qWC)) {
    return false;
  }
  const auto time = toRunTimeUs(sample.clockBinding, sample.deviceTimeUs);
  if (!time) return false;
  const double norm = std::sqrt(sample.qWC[0] * sample.qWC[0] +
                                sample.qWC[1] * sample.qWC[1] +
                                sample.qWC[2] * sample.qWC[2] +
                                sample.qWC[3] * sample.qWC[3]);
  if (!finite(norm) || std::abs(norm - 1.0) > 1e-3) return false;
  for (double& component : sample.qWC) component /= norm;
  if (sample.poseCovariance && !validCovariance<6>(*sample.poseCovariance)) return false;
  runTimeUs = *time;
  return true;
}

std::array<double, 4>
slerp(std::array<double, 4> a, std::array<double, 4> b, double t)
{
  double cosine = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (cosine < 0.0) {
    cosine = -cosine;
    for (double& value : b) value = -value;
  }
  cosine = std::clamp(cosine, -1.0, 1.0);
  if (cosine > 0.9995) {
    std::array<double, 4> result{};
    double normSquared = 0.0;
    for (size_t i = 0; i < 4; ++i) {
      result[i] = a[i] + t * (b[i] - a[i]);
      normSquared += result[i] * result[i];
    }
    const double norm = std::sqrt(normSquared);
    for (double& value : result) value /= norm;
    return result;
  }
  const double angle = std::acos(cosine);
  const double sine = std::sin(angle);
  const double left = std::sin((1.0 - t) * angle) / sine;
  const double right = std::sin(t * angle) / sine;
  std::array<double, 4> result{};
  for (size_t i = 0; i < 4; ++i) result[i] = left * a[i] + right * b[i];
  return result;
}

std::array<double, 4>
quaternionMultiply(const std::array<double, 4>& a, const std::array<double, 4>& b)
{
  return {
    a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
    a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
    a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
    a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0],
  };
}

std::array<double, 6>
poseRate(const PoseSample& first, const PoseSample& second, uint64_t gapUs)
{
  const double gapSeconds = static_cast<double>(gapUs) / 1'000'000.0;
  std::array<double, 6> rate{};
  for (size_t axis = 0; axis < 3; ++axis) {
    rate[axis] = (second.positionW[axis] - first.positionW[axis]) / gapSeconds;
  }

  auto q0 = first.qWC;
  auto q1 = second.qWC;
  double cosine = 0.0;
  for (size_t i = 0; i < 4; ++i) cosine += q0[i] * q1[i];
  if (cosine < 0.0) {
    for (double& component : q1) component = -component;
  }
  const std::array<double, 4> conjugate{q0[0], -q0[1], -q0[2], -q0[3]};
  auto delta = quaternionMultiply(q1, conjugate);
  if (delta[0] < 0.0) {
    for (double& component : delta) component = -component;
  }
  const double vectorNorm = std::sqrt(delta[1] * delta[1] + delta[2] * delta[2] +
                                      delta[3] * delta[3]);
  if (vectorNorm > 1e-15) {
    const double angle = 2.0 * std::atan2(vectorNorm, std::clamp(delta[0], -1.0, 1.0));
    const double scale = angle / (vectorNorm * gapSeconds);
    for (size_t axis = 0; axis < 3; ++axis) rate[axis + 3] = delta[axis + 1] * scale;
  }
  return rate;
}

bool
addTimeUncertainty(std::array<double, 36>& covariance, const PoseSample& first,
                   const PoseSample& second, uint64_t gapUs, uint64_t uncertaintyUs)
{
  if (uncertaintyUs == 0) return true;
  const auto rate = poseRate(first, second, gapUs);
  const double sigmaSeconds = static_cast<double>(uncertaintyUs) / 1'000'000.0;
  const double variance = sigmaSeconds * sigmaSeconds;
  for (size_t row = 0; row < 6; ++row) {
    for (size_t column = 0; column < 6; ++column) {
      covariance[row * 6 + column] += rate[row] * rate[column] * variance;
      if (!finite(covariance[row * 6 + column])) return false;
    }
  }
  return true;
}

} // namespace

bool
FrameKey::operator<(const FrameKey& other) const noexcept
{
  return std::tie(sourceId, cameraId, epoch, sequence) <
         std::tie(other.sourceId, other.cameraId, other.epoch, other.sequence);
}

bool
FrameKey::operator==(const FrameKey& other) const noexcept
{
  return sourceId == other.sourceId && cameraId == other.cameraId &&
         epoch == other.epoch && sequence == other.sequence;
}

ObservationIngress::ObservationIngress(SourceIdentity identity, ObservationPolicy policy,
                                       std::function<uint64_t()> steadyNowUs)
  : m_identity(std::move(identity))
  , m_policy(policy)
  , m_steadyNowUs(std::move(steadyNowUs))
{
  if (!validIdentity(m_identity) || !m_steadyNowUs ||
      m_policy.maxPoseGapUs == 0 || m_policy.maxClockUncertaintyUs == 0 ||
      m_policy.maxAlignWaitMs == 0 || m_policy.poseRetentionUs == 0 ||
      m_policy.maxPoseSamples == 0 || m_policy.maxQueuedFrames == 0 ||
      m_policy.maxQueuedBytes == 0 || m_policy.maxSourceAgeMs == 0 ||
      m_policy.maxAlignWaitMs > MAX_TIME_US / 1000) {
    throw std::invalid_argument("invalid observation identity, policy, or steady clock");
  }
}

Admission
ObservationIngress::reject(const std::string& reason) const
{
  return {ObservationAdmissionStatus::Rejected, reason};
}

bool
ObservationIngress::calibrationReferenced(uint64_t revision) const
{
  return std::any_of(m_pending.begin(), m_pending.end(), [revision] (const auto& entry) {
    return entry.second.aligned.frame.calibrationRevision == revision;
  });
}

Admission
ObservationIngress::submitCalibration(CalibrationRecord record)
{
  if (m_closed) return {ObservationAdmissionStatus::Closed, "CLOSED"};
  if (!validCalibration(record, m_identity.cameraId)) return reject("INVALID_CALIBRATION");
  const auto digest = digestCalibration(record);
  const auto existing = m_calibrationDigests.find(record.revision);
  if (existing != m_calibrationDigests.end()) {
    return existing->second == digest
      ? Admission{ObservationAdmissionStatus::Duplicate, "DUPLICATE"}
      : reject("IDENTITY_CONFLICT");
  }
  if (m_calibrations.size() >= MAX_CALIBRATIONS) {
    const auto evict = std::find_if(m_calibrations.begin(), m_calibrations.end(),
      [this] (const auto& entry) { return !calibrationReferenced(entry.first); });
    if (evict == m_calibrations.end()) return reject("QUEUE_FULL");
    m_calibrationDigests.erase(evict->first);
    m_calibrations.erase(evict);
  }
  m_calibrationDigests.emplace(record.revision, digest);
  m_calibrations.emplace(record.revision, std::move(record));
  return {ObservationAdmissionStatus::Accepted, "ACCEPTED"};
}

Admission
ObservationIngress::submitGround(GroundRecord record)
{
  if (m_closed) return {ObservationAdmissionStatus::Closed, "CLOSED"};
  if (!validGround(record, m_identity.worldFrameId)) return reject("INVALID_GROUND");
  const auto digest = digestGround(record);
  if (m_ground) {
    if (m_ground->revision == record.revision) {
      return m_groundDigest == digest
        ? Admission{ObservationAdmissionStatus::Duplicate, "DUPLICATE"}
        : reject("IDENTITY_CONFLICT");
    }
    if (record.revision < m_ground->revision) return reject("OLD_REVISION");
  }
  m_groundDigest = digest;
  m_ground = std::move(record);
  return {ObservationAdmissionStatus::Accepted, "ACCEPTED"};
}

Admission
ObservationIngress::submitPose(PoseSample sample)
{
  if (m_closed) return {ObservationAdmissionStatus::Closed, "CLOSED"};
  uint64_t runTimeUs = 0;
  if (!validPose(sample, m_identity, m_policy, runTimeUs)) return reject("INVALID_POSE");
  if (m_calibrations.find(sample.calibrationRevision) == m_calibrations.end()) {
    return reject("UNKNOWN_CALIBRATION");
  }
  const auto digest = digestPose(sample);
  const auto previous = m_poseDigests.find(sample.sampleSequence);
  if (previous != m_poseDigests.end()) {
    return previous->second == digest
      ? Admission{ObservationAdmissionStatus::Duplicate, "DUPLICATE"}
      : reject("IDENTITY_CONFLICT");
  }
  if (sample.sampleSequence <= m_highestPoseSequence) return reject("OLD_SEQUENCE");
  if (!m_poses.empty() && m_poses.back().runTimeUs > runTimeUs &&
      m_poses.back().runTimeUs - runTimeUs > m_policy.poseRetentionUs) {
    return reject("STALE_POSE");
  }
  TimedPose value{runTimeUs, std::move(sample), digest};
  const auto position = std::upper_bound(m_poses.begin(), m_poses.end(), runTimeUs,
    [] (uint64_t time, const TimedPose& pose) { return time < pose.runTimeUs; });
  const auto inserted = m_poses.insert(position, std::move(value));
  m_poseDigests.emplace(inserted->sample.sampleSequence, digest);
  m_highestPoseSequence = std::max(m_highestPoseSequence, inserted->sample.sampleSequence);
  const uint64_t newest = m_poses.back().runTimeUs;
  while (!m_poses.empty() && newest > m_poses.front().runTimeUs &&
         newest - m_poses.front().runTimeUs > m_policy.poseRetentionUs) {
    m_poseDigests.erase(m_poses.front().sample.sampleSequence);
    m_poses.pop_front();
  }
  while (m_poses.size() > m_policy.maxPoseSamples) {
    m_poseDigests.erase(m_poses.front().sample.sampleSequence);
    m_poses.pop_front();
  }
  return {ObservationAdmissionStatus::Accepted, "ACCEPTED"};
}

void
ObservationIngress::remember(const FrameKey& key, const std::string& digest)
{
  m_seen.emplace(key, digest);
  m_seenOrder.push_back(key);
  while (m_seenOrder.size() > MAX_SEEN_FRAMES) {
    m_seen.erase(m_seenOrder.front());
    m_seenOrder.pop_front();
  }
}

Admission
ObservationIngress::submitGap(FrameGap gap)
{
  if (m_closed) return {ObservationAdmissionStatus::Closed, "CLOSED"};
  if (!sameIdentity(gap.identity, m_identity)) return reject("IDENTITY_MISMATCH");
  if (gap.schema != "uav-frame-gap-v2" || gap.firstSequence == 0 ||
      gap.firstSequence > gap.lastSequence || gap.lastSequence > MAX_TIME_US ||
      (gap.reason != "CAPTURE_DROPPED" && gap.reason != "QUEUE_FULL")) {
    return reject("INVALID_GAP");
  }
  for (const auto& previous : m_frameGaps) {
    if (gap.firstSequence == previous.firstSequence &&
        gap.lastSequence == previous.lastSequence && gap.reason == previous.reason) {
      return {ObservationAdmissionStatus::Duplicate, "DUPLICATE"};
    }
    if (gap.firstSequence <= previous.lastSequence &&
        gap.lastSequence >= previous.firstSequence) return reject("IDENTITY_CONFLICT");
  }
  if (gap.firstSequence <= m_lastFrameSequence) return reject("OLD_SEQUENCE");
  if (m_frameGaps.size() >= MAX_FRAME_GAPS) return reject("GAP_LIMIT");
  // A queue rejection is not a drop. Only this explicit producer decision
  // commits missing captures, without allocating image storage or timestamps.
  const auto lastSequence = gap.lastSequence;
  m_frameGaps.push_back(std::move(gap));
  m_lastFrameSequence = lastSequence;
  return {ObservationAdmissionStatus::Accepted, "ACCEPTED"};
}

bool
ObservationIngress::hasFrameGap(const FrameKey& key) const noexcept
{
  if (key.sourceId != m_identity.sourceId || key.cameraId != m_identity.cameraId ||
      key.epoch != m_identity.epoch) return false;
  return std::any_of(m_frameGaps.begin(), m_frameGaps.end(), [&](const auto& gap) {
    return key.sequence >= gap.firstSequence && key.sequence <= gap.lastSequence;
  });
}

Admission
ObservationIngress::submitFrame(CapturedFrame frame)
{
  if (m_closed) return {ObservationAdmissionStatus::Closed, "CLOSED"};
  if (frame.key.sourceId != m_identity.sourceId || frame.key.cameraId != m_identity.cameraId ||
      frame.key.epoch != m_identity.epoch || frame.key.sequence == 0 ||
      frame.key.sequence > MAX_TIME_US) return reject("IDENTITY_MISMATCH");
  if (hasFrameGap(frame.key)) return reject("FRAME_GAP");
  if (frame.imageBytes.empty() || frame.imageBytes.size() > MAX_IMAGE_BYTES) {
    return reject("INVALID_FRAME");
  }
  const auto existing = m_seen.find(frame.key);
  const std::string digest = digestFrame(frame);
  if (existing != m_seen.end()) {
    return existing->second == digest
      ? Admission{ObservationAdmissionStatus::Duplicate, "DUPLICATE"}
      : reject("IDENTITY_CONFLICT");
  }
  const auto pending = m_pending.find(frame.key);
  if (pending != m_pending.end()) {
    return pending->second.digest == digest
      ? Admission{ObservationAdmissionStatus::Duplicate, "DUPLICATE"}
      : reject("IDENTITY_CONFLICT");
  }
  if (frame.key.sequence <= m_lastFrameSequence) return reject("OLD_SEQUENCE");
  if (frame.imageBytes.empty() || frame.imageBytes.size() > MAX_IMAGE_BYTES ||
      frame.width == 0 || frame.height == 0 || frame.width > 8192 || frame.height > 8192 ||
      frame.encoding != "image/jpeg" || frame.deviceTimeUs > MAX_TIME_US ||
      !validClock(frame.clockBinding) ||
      frame.clockBinding.uncertaintyUs > m_policy.maxClockUncertaintyUs ||
      frame.clockBinding.uncertaintyUs > MAX_TIME_US ||
      (frame.calibrationRevision != 0 &&
       m_calibrations.find(frame.calibrationRevision) == m_calibrations.end())) {
    return reject("INVALID_FRAME");
  }
  const auto captureTime = toRunTimeUs(frame.clockBinding, frame.deviceTimeUs);
  if (!captureTime) return reject("INVALID_CLOCK_BINDING");
  if (frame.calibrationRevision != 0) {
    const auto& calibration = m_calibrations.at(frame.calibrationRevision);
    if (*captureTime < calibration.validFromUs || *captureTime > calibration.validUntilUs ||
        calibration.width != frame.width || calibration.height != frame.height) {
      return reject("CALIBRATION_MISMATCH");
    }
  }
  if (m_pending.size() >= m_policy.maxQueuedFrames ||
      frame.imageBytes.size() > m_policy.maxQueuedBytes -
        std::min(m_queuedBytes, m_policy.maxQueuedBytes)) {
    return reject("QUEUE_FULL");
  }
  const uint64_t now = m_steadyNowUs();
  const uint64_t waitUs = m_policy.maxAlignWaitMs * 1000;
  const uint64_t deadline = now > MAX_TIME_US - waitUs ? MAX_TIME_US : now + waitUs;
  AlignedFrame aligned;
  aligned.identity = m_identity;
  aligned.captureTimeUs = *captureTime;
  aligned.timeUncertaintyUs = frame.clockBinding.uncertaintyUs;
  if (frame.calibrationRevision != 0) aligned.calibration = m_calibrations.at(frame.calibrationRevision);
  if (m_ground) aligned.ground = *m_ground;
  if (!aligned.calibration) aligned.reason = "CALIBRATION_MISSING";
  if (!aligned.ground) aligned.reason = aligned.reason.empty() ? "GROUND_MISSING" : aligned.reason;
  aligned.frame = std::move(frame);
  const size_t bytes = aligned.frame.imageBytes.size();
  m_queuedBytes += bytes;
  m_lastFrameSequence = aligned.frame.key.sequence;
  const FrameKey key = aligned.frame.key;
  m_pending.emplace(key, PendingFrame{std::move(aligned), deadline, digest, bytes});
  remember(key, digest);
  return {ObservationAdmissionStatus::Accepted, "ACCEPTED"};
}

AlignedObservation
ObservationIngress::align(PendingFrame& pending, uint64_t nowSteadyUs) const
{
  auto& aligned = pending.aligned;
  const uint64_t target = aligned.captureTimeUs;
  auto upper = std::lower_bound(m_poses.begin(), m_poses.end(), target,
    [] (const TimedPose& pose, uint64_t time) { return pose.runTimeUs < time; });
  const bool hasExactPose = upper != m_poses.end() && upper->runTimeUs == target;
  const TimedPose* first = nullptr;
  const TimedPose* second = nullptr;
  if (hasExactPose) {
    first = &*upper;
  }
  else if (upper != m_poses.begin() && upper != m_poses.end()) {
    first = &*std::prev(upper);
    second = &*upper;
  }
  const auto sameClockBinding = [&aligned] (const TimedPose& pose) {
    const auto& left = pose.sample.clockBinding;
    const auto& right = aligned.frame.clockBinding;
    return left.revision == right.revision && left.clockId == right.clockId &&
           left.clockEpoch == right.clockEpoch && left.a == right.a && left.bUs == right.bUs &&
           left.validFromUs == right.validFromUs && left.validUntilUs == right.validUntilUs &&
           left.uncertaintyUs == right.uncertaintyUs;
  };
  if (first && (first->sample.calibrationRevision != aligned.frame.calibrationRevision ||
                !sameClockBinding(*first))) {
    first = nullptr;
  }
  if (second && (!first || second->sample.calibrationRevision != aligned.frame.calibrationRevision ||
                 !sameClockBinding(*second) ||
                 second->runTimeUs - first->runTimeUs > m_policy.maxPoseGapUs)) {
    second = nullptr;
  }
  if (hasExactPose && first) {
    const uint64_t poseUncertaintyUs = first->sample.clockBinding.uncertaintyUs;
    aligned.timeUncertaintyUs = poseUncertaintyUs > MAX_TIME_US - aligned.timeUncertaintyUs
      ? MAX_TIME_US : aligned.timeUncertaintyUs + poseUncertaintyUs;
    if (aligned.timeUncertaintyUs > m_policy.maxClockUncertaintyUs) {
      aligned.reason = "CLOCK_UNCERTAINTY";
    }
    else if (aligned.timeUncertaintyUs != 0) {
      // A single exact sample carries no local motion bound for its clock error.
      // Do not let a geometrically exact timestamp imply a time-exact pose.
      aligned.reason = "TIME_UNCERTAINTY_UNBOUNDED";
    }
    else {
      aligned.poseStatus = PoseStatus::Aligned;
      aligned.poseEndpoints = {first->sample};
      aligned.positionW = first->sample.positionW;
      aligned.qWC = first->sample.qWC;
      aligned.poseCovariance = first->sample.poseCovariance;
    }
  }
  else if (first && second) {
    const uint64_t gapUs = second->runTimeUs - first->runTimeUs;
    const double ratio = static_cast<double>(target - first->runTimeUs) /
                         static_cast<double>(gapUs);
    aligned.poseStatus = PoseStatus::Aligned;
    aligned.poseEndpoints = {first->sample, second->sample};
    std::array<double, 3> position{};
    for (size_t i = 0; i < 3; ++i) {
      position[i] = first->sample.positionW[i] + ratio *
                    (second->sample.positionW[i] - first->sample.positionW[i]);
    }
    aligned.positionW = position;
    aligned.qWC = slerp(first->sample.qWC, second->sample.qWC, ratio);
    if (first->sample.poseCovariance && second->sample.poseCovariance) {
      std::array<double, 36> covariance{};
      for (size_t i = 0; i < covariance.size(); ++i) {
        covariance[i] = (1.0 - ratio) * (*first->sample.poseCovariance)[i] +
                         ratio * (*second->sample.poseCovariance)[i];
      }
      aligned.poseCovariance = covariance;
    }
    const uint64_t endpointUncertaintyUs = std::max(
      first->sample.clockBinding.uncertaintyUs,
      second->sample.clockBinding.uncertaintyUs);
    aligned.timeUncertaintyUs = endpointUncertaintyUs > MAX_TIME_US - aligned.timeUncertaintyUs
      ? MAX_TIME_US : aligned.timeUncertaintyUs + endpointUncertaintyUs;
    if (aligned.timeUncertaintyUs <= m_policy.maxClockUncertaintyUs &&
        aligned.timeUncertaintyUs != 0) {
      if (!aligned.poseCovariance) {
        aligned.poseStatus = PoseStatus::Unknown;
        aligned.poseEndpoints.clear();
        aligned.positionW.reset();
        aligned.qWC.reset();
        aligned.reason = "TIME_UNCERTAINTY_UNBOUNDED";
      }
      else if (!addTimeUncertainty(*aligned.poseCovariance, first->sample,
                                   second->sample, gapUs,
                                   aligned.timeUncertaintyUs)) {
        aligned.poseStatus = PoseStatus::Unknown;
        aligned.poseEndpoints.clear();
        aligned.positionW.reset();
        aligned.qWC.reset();
        aligned.poseCovariance.reset();
        aligned.reason = "TIME_UNCERTAINTY_UNBOUNDED";
      }
    }
  }
  if (aligned.poseStatus == PoseStatus::Aligned &&
      aligned.timeUncertaintyUs > m_policy.maxClockUncertaintyUs) {
    aligned.poseStatus = PoseStatus::Unknown;
    aligned.poseEndpoints.clear();
    aligned.positionW.reset();
    aligned.qWC.reset();
    aligned.poseCovariance.reset();
    aligned.reason = "CLOCK_UNCERTAINTY";
  }
  if (aligned.poseStatus == PoseStatus::Unknown && aligned.reason != "CLOCK_UNCERTAINTY" &&
      aligned.reason != "TIME_UNCERTAINTY_UNBOUNDED") {
    if (nowSteadyUs < pending.deadlineSteadyUs) {
      return {AlignedObservationStatus::Pending, std::nullopt, "POSE_PENDING"};
    }
    aligned.reason = "POSE_UNAVAILABLE";
  }
  if (aligned.reason.empty() && !aligned.calibration) aligned.reason = "CALIBRATION_MISSING";
  if (aligned.reason.empty() && !aligned.ground) aligned.reason = "GROUND_MISSING";
  return {AlignedObservationStatus::Ready, aligned, aligned.reason};
}

AlignedObservation
ObservationIngress::take(const FrameKey& key, uint64_t nowSteadyUs)
{
  if (m_closed) return {AlignedObservationStatus::Closed, std::nullopt, "CLOSED"};
  if (hasFrameGap(key)) return {AlignedObservationStatus::Rejected, std::nullopt, "FRAME_GAP"};
  const auto it = m_pending.find(key);
  if (it == m_pending.end()) return {AlignedObservationStatus::Rejected, std::nullopt, "NOT_FOUND"};
  auto result = align(it->second, nowSteadyUs);
  if (result.status == AlignedObservationStatus::Ready ||
      result.status == AlignedObservationStatus::Rejected) {
    m_queuedBytes -= it->second.bytes;
    m_pending.erase(it);
  }
  return result;
}

void
ObservationIngress::close() noexcept
{
  if (m_closed) return;
  m_closed = true;
  m_pending.clear();
  m_frameGaps.clear();
  m_queuedBytes = 0;
  m_poses.clear();
  m_poseDigests.clear();
}

} // namespace ndnsf::examples::uav
