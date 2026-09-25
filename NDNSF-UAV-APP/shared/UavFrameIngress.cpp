#include "UavFrameIngress.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace ndnsf::examples::uav {

UavFrameIngress::UavFrameIngress(FrameIngressConfig config)
  : m_config(std::move(config))
{
  if (m_config.maxQueuedFrames == 0 || m_config.maxQueuedBytes == 0 ||
      m_config.maxFrameBytes == 0 || m_config.cameras.empty()) {
    throw std::invalid_argument("invalid frame ingress bounds");
  }
}

bool
UavFrameIngress::reject(const std::string& message, std::string* reason)
{
  if (reason) {
    *reason = message;
  }
  return false;
}

bool
UavFrameIngress::submit(FrameEnvelope frame, std::string* reason)
{
  if (m_closed) {
    return reject("ingress is closed", reason);
  }
  if (!m_config.cameras.count(frame.cameraId)) {
    return reject("unknown camera", reason);
  }
  if (frame.sourceEpoch == 0 || frame.bytes.empty() ||
      frame.bytes.size() > m_config.maxFrameBytes || frame.width == 0 || frame.height == 0) {
    return reject("invalid frame bounds", reason);
  }
  if (frame.motionMetadata.size() > 256 * 1024) {
    return reject("motion metadata exceeds 256 KiB", reason);
  }
  const auto epoch = m_epochs.find(frame.cameraId);
  if (epoch != m_epochs.end() && epoch->second != frame.sourceEpoch) {
    return reject("source epoch changed without closing ingress", reason);
  }
  m_epochs[frame.cameraId] = frame.sourceEpoch;
  const auto sequence = m_lastSequence.find(frame.cameraId);
  if (sequence != m_lastSequence.end() && frame.sequence <= sequence->second) {
    return reject("sequence is not strictly increasing", reason);
  }
  const auto pts = m_lastPts.find(frame.cameraId);
  if (pts != m_lastPts.end() && frame.ptsUs < pts->second) {
    return reject("source PTS moved backwards", reason);
  }
  if (m_queue.size() >= m_config.maxQueuedFrames ||
      m_queuedBytes + frame.bytes.size() > m_config.maxQueuedBytes) {
    return reject("bounded ingress queue is full", reason);
  }
  m_lastSequence[frame.cameraId] = frame.sequence;
  m_lastPts[frame.cameraId] = frame.ptsUs;
  m_queuedBytes += frame.bytes.size();
  m_queue.push_back(frame);
  if (m_sink && !m_sink->submit(std::move(frame), reason)) {
    m_queuedBytes -= m_queue.back().bytes.size();
    m_queue.pop_back();
    return false;
  }
  return true;
}

void
UavFrameIngress::close()
{
  if (!m_closed) {
    m_closed = true;
    if (m_sink) {
      m_sink->close();
    }
  }
}

} // namespace ndnsf::examples::uav
