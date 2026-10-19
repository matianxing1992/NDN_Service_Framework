#include "UavTrackingEvidenceStore.hpp"
#include "UavObservationRefVerifier.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <set>
#include <stdexcept>

namespace ndnsf::examples::uav {

namespace {
std::string digest(const std::vector<uint8_t>& bytes)
{
  ndn::util::Sha256 sha;
  if (!bytes.empty()) {
    sha << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  return canonicalSha256Digest(sha.toString());
}
}

UavTrackingEvidenceStore::UavTrackingEvidenceStore(size_t maxSegmentBytes,
                                                   size_t maxRetainedBytes)
  : m_maxSegmentBytes(maxSegmentBytes)
  , m_maxRetainedBytes(maxRetainedBytes)
{
  if (m_maxSegmentBytes == 0 || m_maxRetainedBytes == 0 ||
      m_maxSegmentBytes > m_maxRetainedBytes) {
    throw std::invalid_argument("invalid evidence store bounds");
  }
}

std::optional<PublishedFrame>
UavTrackingEvidenceStore::stage(const ndn::Name& producer, const std::string& missionId,
                                const std::string& windowId, const FrameEnvelope& frame,
                                std::string* reason)
{
  auto reject = [reason](const std::string& message) {
    if (reason) *reason = message;
    return std::optional<PublishedFrame>{};
  };
  if (producer.empty() || missionId.empty() || windowId.empty()) {
    return reject("producer, mission and window are required");
  }
  if (frame.bytes.empty() || frame.sourceEpoch == 0 || frame.sequence == 0) {
    return reject("frame identity or bytes are invalid");
  }
  constexpr size_t MAX_OBSERVATION_METADATA_BYTES = 64 * 1024;
  if (frame.observationMetadata.size() > MAX_OBSERVATION_METADATA_BYTES) {
    return reject("observation metadata exceeds 64 KiB");
  }
  size_t dependencyBytes = 0;
  std::set<std::string> dependencyNames;
  for (const auto& object : frame.observationObjects) {
    if (object.category != "CALIBRATION" && object.category != "GROUND" &&
        object.category != "CLOCK" && object.category != "POSE") {
      return reject("observation dependency category is invalid");
    }
    const ndn::Name objectName(object.exactName);
    if (objectName.empty() || !producer.isPrefixOf(objectName) ||
        object.producerIdentity != producer.toUri() ||
        !dependencyNames.insert(objectName.toUri()).second ||
        object.payload.empty() || object.payload.size() > MAX_OBSERVATION_METADATA_BYTES ||
        digest(object.payload) != object.contentSha256) {
      return reject("observation dependency identity or digest is invalid");
    }
    if (object.payload.size() > m_maxRetainedBytes ||
        dependencyBytes > m_maxRetainedBytes - object.payload.size()) {
      return reject("observation dependency size overflow");
    }
    dependencyBytes += object.payload.size();
  }
  if (frame.bytes.size() > m_maxRetainedBytes ||
      frame.observationMetadata.size() > m_maxRetainedBytes - frame.bytes.size()) {
    return reject("evidence retention bound exceeded");
  }
  const size_t baseRetained = frame.bytes.size() + frame.observationMetadata.size();
  if (dependencyBytes > m_maxRetainedBytes - baseRetained) {
    return reject("evidence retention bound exceeded");
  }
  const size_t retained = baseRetained + dependencyBytes;
  if (retained > m_maxRetainedBytes ||
      m_retainedBytes > m_maxRetainedBytes - retained) {
    return reject("evidence retention bound exceeded");
  }
  PublishedFrame result;
  result.reference.producerIdentity = producer;
  result.reference.streamId = frame.cameraId;
  result.reference.streamSessionEpoch = frame.sourceEpoch;
  result.reference.firstSequence = frame.sequence;
  result.reference.lastSequence = frame.sequence;
  result.reference.windowStartMs = frame.ptsUs / 1000;
  result.reference.windowEndMs = frame.ptsUs / 1000;
  if (frame.observationMetadata.empty()) {
    result.reference.exactDataName = makeUavEvidenceName(
      producer, missionId, windowId, frame.cameraId + "-" + std::to_string(frame.sequence), m_version++);
    result.reference.version = m_version - 1;
  }
  else {
    result.reference.exactDataName = makeUavObservationDataName(
      producer, frame.cameraId, frame.sourceEpoch, frame.sequence, "FRAME", 1);
    result.reference.version = 1;
  }
  result.reference.contentDigest = digest(frame.bytes);
  result.reference.contentType = frame.encoding;
  result.reference.retentionDeadlineMs = frame.ptsUs / 1000 + 120000;
  if (!frame.motionMetadata.empty()) {
    result.reference.motionMetadataDigest = digest(
      std::vector<uint8_t>(frame.motionMetadata.begin(), frame.motionMetadata.end()));
  }
  result.payload = ndn::Buffer(frame.bytes.begin(), frame.bytes.end());
  if (!frame.observationMetadata.empty()) {
    result.observationName = makeUavObservationDataName(
      producer, frame.cameraId, frame.sourceEpoch, frame.sequence, "OBSERVATION", 1);
    result.observationPayload = ndn::Buffer(
      reinterpret_cast<const uint8_t*>(frame.observationMetadata.data()),
      frame.observationMetadata.size());
    result.observationDigest = digest(
      std::vector<uint8_t>(frame.observationMetadata.begin(), frame.observationMetadata.end()));
  }
  result.observationObjects = frame.observationObjects;
  for (size_t offset = 0; offset < frame.bytes.size(); offset += m_maxSegmentBytes) {
    const auto end = std::min(frame.bytes.size(), offset + m_maxSegmentBytes);
    result.segments.emplace_back(frame.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                 frame.bytes.begin() + static_cast<std::ptrdiff_t>(end));
  }
  m_retainedBytes += retained;
  return result;
}

ndn::Name
UavTrackingEvidenceStore::publish(
  ndn_service_framework::ServiceProvider::CollaborationContext& context,
  const std::string& keyScope, const PublishedFrame& frame, int freshnessMs) const
{
  if (frame.reference.exactDataName.empty() || frame.payload.empty() ||
      keyScope.empty() || freshnessMs <= 0) {
    throw std::invalid_argument("invalid staged evidence publication");
  }
  for (const auto& object : frame.observationObjects) {
    const ndn::Name objectName(object.exactName);
    if (objectName.empty() || object.payload.empty() ||
        digest(object.payload) != object.contentSha256) {
      throw std::invalid_argument("staged observation dependency is incomplete");
    }
    const auto reference = parseObservationReference(
      object.exactName, object.contentSha256, object.producerIdentity, object.category);
    if (reference.producerIdentity != frame.reference.producerIdentity) {
      throw std::invalid_argument("staged observation dependency has the wrong producer");
    }
    requireObservationReferenceNameShape(reference, object.category, frame.reference.streamId,
                                         frame.reference.streamSessionEpoch,
                                         frame.reference.firstSequence);
    if (object.category != "POSE") {
      // Static originals were committed before their sidecar was serialized.
      // Re-publishing here would re-encrypt one immutable name per request.
      continue;
    }
    const auto publishedObject = context.publishLargeNamed(
      "uav-tracking-observation", objectName,
      ndn::Buffer(object.payload.begin(), object.payload.end()), m_maxSegmentBytes, freshnessMs);
    if (publishedObject != objectName) {
      throw std::runtime_error("source observation dependency name changed");
    }
  }
  if (!frame.observationPayload.empty()) {
    if (frame.observationName.empty() || frame.observationDigest.empty()) {
      throw std::invalid_argument("staged observation sidecar is incomplete");
    }
    const auto publishedObservation = context.publishLargeNamed(
      "uav-tracking-observation", frame.observationName,
      frame.observationPayload, m_maxSegmentBytes, freshnessMs);
    if (publishedObservation != frame.observationName) {
      throw std::runtime_error("source observation sidecar name changed");
    }
  }
  return context.publishLargeNamed(keyScope, frame.reference.exactDataName,
                                   frame.payload, m_maxSegmentBytes, freshnessMs);
}

void
UavTrackingEvidenceStore::clear()
{
  m_retainedBytes = 0;
}

} // namespace ndnsf::examples::uav
