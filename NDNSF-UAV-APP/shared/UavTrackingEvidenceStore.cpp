#include "UavTrackingEvidenceStore.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <stdexcept>

namespace ndnsf::examples::uav {

namespace {
std::string digest(const std::vector<uint8_t>& bytes)
{
  ndn::util::Sha256 sha;
  if (!bytes.empty()) {
    sha << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  return "sha256:" + sha.toString();
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
  if (frame.bytes.size() > m_maxRetainedBytes ||
      m_retainedBytes + frame.bytes.size() > m_maxRetainedBytes) {
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
  result.reference.exactDataName = makeUavEvidenceName(
    producer, missionId, windowId, frame.cameraId + "-" + std::to_string(frame.sequence), m_version++);
  result.reference.version = m_version - 1;
  result.reference.contentDigest = digest(frame.bytes);
  result.reference.contentType = frame.encoding;
  result.reference.retentionDeadlineMs = frame.ptsUs / 1000 + 120000;
  if (!frame.motionMetadata.empty()) {
    result.reference.motionMetadataDigest = digest(
      std::vector<uint8_t>(frame.motionMetadata.begin(), frame.motionMetadata.end()));
  }
  result.payload = ndn::Buffer(frame.bytes.begin(), frame.bytes.end());
  for (size_t offset = 0; offset < frame.bytes.size(); offset += m_maxSegmentBytes) {
    const auto end = std::min(frame.bytes.size(), offset + m_maxSegmentBytes);
    result.segments.emplace_back(frame.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                 frame.bytes.begin() + static_cast<std::ptrdiff_t>(end));
  }
  m_retainedBytes += frame.bytes.size();
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
  return context.publishLargeNamed(keyScope, frame.reference.exactDataName,
                                   frame.payload, m_maxSegmentBytes, freshnessMs);
}

void
UavTrackingEvidenceStore::clear()
{
  m_retainedBytes = 0;
}

} // namespace ndnsf::examples::uav
