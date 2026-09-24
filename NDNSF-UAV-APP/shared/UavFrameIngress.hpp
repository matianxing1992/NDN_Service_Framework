#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

struct FrameEnvelope
{
  std::string cameraId;
  uint64_t sourceEpoch = 0;
  uint64_t sequence = 0;
  uint64_t ptsUs = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::string encoding = "image/jpeg";
  std::vector<uint8_t> bytes;
};

class IFrameSink
{
public:
  virtual ~IFrameSink() = default;
  virtual bool submit(FrameEnvelope frame, std::string* reason = nullptr) = 0;
  virtual void close() = 0;
};

struct FrameIngressConfig
{
  size_t maxQueuedFrames = 2;
  size_t maxQueuedBytes = 16 * 1024 * 1024;
  size_t maxFrameBytes = 8 * 1024 * 1024;
  std::set<std::string> cameras{"UAV1", "UAV2", "UAV3"};
};

class UavFrameIngress
{
public:
  explicit UavFrameIngress(FrameIngressConfig config = {});

  bool submit(FrameEnvelope frame, std::string* reason = nullptr);
  void close();
  bool closed() const noexcept { return m_closed; }
  size_t queuedFrames() const noexcept { return m_queue.size(); }
  size_t queuedBytes() const noexcept { return m_queuedBytes; }
  void setSink(IFrameSink* sink) noexcept { m_sink = sink; }

private:
  bool reject(const std::string& message, std::string* reason);

private:
  FrameIngressConfig m_config;
  IFrameSink* m_sink = nullptr;
  bool m_closed = false;
  size_t m_queuedBytes = 0;
  std::map<std::string, uint64_t> m_lastSequence;
  std::map<std::string, uint64_t> m_lastPts;
  std::map<std::string, uint64_t> m_epochs;
  std::vector<FrameEnvelope> m_queue;
};

} // namespace ndnsf::examples::uav
