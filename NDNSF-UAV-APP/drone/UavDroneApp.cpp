#include "../shared/UavNames.hpp"
#include "../shared/UavProtocol.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <gtkmm.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

NDN_LOG_INIT(ndn_service_framework.examples.UavDroneApp);

using namespace ndnsf::examples::uav;
using namespace std::chrono_literals;

class KeyChainInitLock
{
public:
  explicit KeyChainInitLock(const char* path)
  {
    m_fd = open(path, O_CREAT | O_RDWR, 0666);
    if (m_fd < 0) {
      throw std::runtime_error("failed to acquire keychain lock");
    }
    if (flock(m_fd, LOCK_EX | LOCK_NB) != 0) {
      close(m_fd);
      m_fd = -1;
    }
  }

  ~KeyChainInitLock()
  {
    if (m_fd >= 0) {
      flock(m_fd, LOCK_UN);
      close(m_fd);
    }
  }

private:
  int m_fd = -1;
};

std::string
getOption(int argc, char** argv, const std::string& option, const std::string& fallback)
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == option) {
      return argv[i + 1];
    }
  }
  return fallback;
}

bool
hasFlag(int argc, char** argv, const std::string& option)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == option) {
      return true;
    }
  }
  return false;
}

ndn::security::Certificate
getOrCreateIdentity(ndn::KeyChain& keyChain, const ndn::Name& identity)
{
  try {
    return keyChain.getPib().getIdentity(identity).getDefaultKey().getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey()
      .getDefaultCertificate();
  }
}

ndn::Buffer
bufferFromString(const std::string& value)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

std::string
payloadToString(const ndn_service_framework::RequestMessage& request)
{
  const auto payload = request.getPayload();
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

ndn_service_framework::ResponseMessage
makeResponse(bool status, const std::string& payload, const std::string& error = "No error")
{
  auto responsePayload = bufferFromString(payload);
  ndn_service_framework::ResponseMessage response;
  response.setStatus(status);
  response.setErrorInfo(error);
  response.setPayload(responsePayload, responsePayload.size());
  return response;
}

class MockFlightControllerBackend
{
public:
  explicit MockFlightControllerBackend(std::string droneId)
    : m_droneId(std::move(droneId))
  {
  }

  bool
  sendMavlink(const std::vector<uint8_t>& frame)
  {
    ++m_forwardedCount;
    NDN_LOG_INFO("MOCK_FC_FORWARD drone=" << m_droneId
                 << " bytes=" << frame.size()
                 << " count=" << m_forwardedCount.load());
    return true;
  }

private:
  std::string m_droneId;
  std::atomic<size_t> m_forwardedCount{0};
};

class VideoPublisher
{
public:
  VideoPublisher(ndn::Face& face, ndn::KeyChain& keyChain,
                 std::string droneId, std::string videoPath)
    : m_face(face)
    , m_keyChain(keyChain)
    , m_droneId(std::move(droneId))
    , m_videoPath(std::move(videoPath))
  {
    m_videoPrefix = droneIdentity(m_droneId).append("video");
    m_streamPrefix = ndn::Name(m_videoPrefix).append(m_streamId);
    m_captureThread = std::thread([this] { this->captureLoop(); });
  }

  ~VideoPublisher()
  {
    shutdown();
  }

  void
  shutdown()
  {
    m_done = true;
    m_streaming = false;
    if (m_captureThread.joinable()) {
      m_captureThread.join();
    }
  }

  Fields
  start(const Fields& requestFields)
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_targetFps = std::clamp<uint64_t>(
      std::stoull(fieldOr(requestFields, "fps", "30")), 1, 60);
    m_requestedBitrateKbps = std::max<uint64_t>(
      1, std::stoull(fieldOr(requestFields, "requested_bitrate_kbps",
                             fieldOr(requestFields, "target_bitrate_kbps", "8000"))));
    m_acceptedBitrateKbps = std::clamp<uint64_t>(
      m_requestedBitrateKbps.load(), MIN_VIDEO_BITRATE_KBPS, MAX_VIDEO_BITRATE_KBPS);
    auto requestedWidth = std::clamp<uint64_t>(
      std::stoull(fieldOr(requestFields, "requested_frame_width", "480")),
      MIN_VIDEO_FRAME_WIDTH, MAX_VIDEO_FRAME_WIDTH);
    if (requestedWidth % 2 != 0) {
      --requestedWidth;
    }
    m_requestedFrameWidth = requestedWidth;
    m_acceptedFrameWidth = requestedWidth;
    m_encoderQuality = qualityForBitrate(m_acceptedBitrateKbps);
    m_fecDataShards = defaultFecDataShardsForBitrate(m_acceptedBitrateKbps.load());
    m_fecParityShards = 1;
    m_restartEncoder = true;
    ensureFrameFilterRegistered();
    m_streamId = "stream-" + std::to_string(nowMilliseconds());
    m_streamPrefix = ndn::Name(m_videoPrefix).append(m_streamId);
    m_nextSeq = 0;
    m_nextPacketSeq = 0;
    m_nextFecFrameSeq = 0;
    m_packets.clear();
    m_order.clear();
    m_pending.clear();
    m_fecPendingChunks.clear();
    m_fecCurrentFrameStartMs = 0;
    m_jpegBuffer.clear();
    m_streaming = true;
    const auto startSecond = nowMilliseconds() / 1000;
    return {
      {"status", "streaming"},
      {"drone_id", m_droneId},
      {"stream_id", m_streamId},
      {"stream_prefix", m_streamPrefix.toUri()},
      {"fps", std::to_string(m_targetFps)},
      {"requested_bitrate_kbps", std::to_string(m_requestedBitrateKbps)},
      {"accepted_bitrate_kbps", std::to_string(m_acceptedBitrateKbps)},
      {"min_bitrate_kbps", std::to_string(MIN_VIDEO_BITRATE_KBPS)},
      {"max_bitrate_kbps", std::to_string(MAX_VIDEO_BITRATE_KBPS)},
      {"encoder_quality", std::to_string(m_encoderQuality)},
      {"requested_frame_width", std::to_string(m_requestedFrameWidth)},
      {"accepted_frame_width", std::to_string(m_acceptedFrameWidth)},
      {"start_second", std::to_string(startSecond)},
      {"next_packet", "0"},
      {"encoding", "video/h264"},
      {"stream_format", "stream-start-time/packetSeq with stream-chunk metadata"},
      {"fec_data_shards", std::to_string(m_fecDataShards)},
      {"fec_parity_shards", std::to_string(m_fecParityShards)},
      {"frame_width", std::to_string(m_acceptedFrameWidth)},
      {"max_payload_bytes", std::to_string(MAX_VIDEO_PACKET_PAYLOAD)},
      {"streaming_model", "mjpeg-low-latency-packet-stream"},
      {"prefetch_hint", "budget-from-bitrate"},
      {"source", m_videoPath},
      {"timestamp_ms", std::to_string(nowMilliseconds())},
    };
  }

  Fields
  stop()
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_streaming = false;
    m_pending.clear();
    m_packets.clear();
    m_order.clear();
    m_fecPendingChunks.clear();
    m_fecCurrentFrameStartMs = 0;
    m_nextFecFrameSeq = 0;
    m_jpegBuffer.clear();
    m_restartEncoder = true;
    return {
      {"status", "stopped"},
      {"drone_id", m_droneId},
      {"stream_id", m_streamId},
      {"frames_published", std::to_string(m_nextSeq)},
      {"timestamp_ms", std::to_string(nowMilliseconds())},
    };
  }

  bool
  isStreaming() const
  {
    return m_streaming.load();
  }

  uint64_t
  framesPublished() const
  {
    return m_nextSeq.load();
  }

  ndn::Name
  streamPrefix() const
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_streamPrefix;
  }

private:
  static std::string
  shellQuote(const std::string& value)
  {
    std::string output = "'";
    for (const auto ch : value) {
      if (ch == '\'') {
        output += "'\\''";
      }
      else {
        output.push_back(ch);
      }
    }
    output.push_back('\'');
    return output;
  }

  static uint64_t
  qualityForBitrate(uint64_t bitrateKbps)
  {
    if (bitrateKbps >= 8000) {
      return 6;
    }
    if (bitrateKbps >= 6000) {
      return 8;
    }
    if (bitrateKbps >= 4000) {
      return 10;
    }
    if (bitrateKbps >= 2500) {
      return 14;
    }
    if (bitrateKbps >= 1500) {
      return 20;
    }
    if (bitrateKbps >= 800) {
      return 25;
    }
    return 31;
  }

  static uint64_t
  defaultFecDataShardsForBitrate(uint64_t bitrateKbps)
  {
    if (bitrateKbps >= 8000) {
      return 12;
    }
    if (bitrateKbps >= 4000) {
      return 8;
    }
    if (bitrateKbps >= 2000) {
      return 6;
    }
    if (bitrateKbps >= 1200) {
      return 4;
    }
    return 3;
  }

  static std::string
  joinFecLengths(const std::vector<size_t>& lengths)
  {
    std::string out;
    for (size_t i = 0; i < lengths.size(); ++i) {
      if (i > 0) {
        out += ",";
      }
      out += std::to_string(lengths[i]);
    }
    return out;
  }

  void
  onFrameInterest(const ndn::Interest& interest)
  {
    const auto interestCount = ++m_frameInterests;
    if (interestCount <= 3 || interestCount % 30 == 0) {
      NDN_LOG_INFO("VIDEO_FRAME_INTEREST count=" << interestCount
                   << " name=" << interest.getName());
    }
    const auto& name = interest.getName();
    if (name.size() < m_streamPrefix.size() + 1) {
      return;
    }

    std::vector<uint8_t> packet;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      if (!m_streaming.load() || !m_streamPrefix.isPrefixOf(name)) {
        return;
      }
      const auto uri = name.toUri();
      const auto it = m_packets.find(uri);
      if (it != m_packets.end()) {
        packet = it->second;
      }
      else {
        m_pending[uri] = name;
      }
    }

    if (!packet.empty()) {
      putPacket(name, packet);
    }
  }

  void
  ensureFrameFilterRegistered()
  {
    if (m_filterRegistered) {
      return;
    }
    m_face.setInterestFilter(
      m_videoPrefix,
      [this] (const auto&, const ndn::Interest& interest) {
        this->onFrameInterest(interest);
      },
      [] (const ndn::Name&) {},
      [] (const ndn::Name& prefix, const std::string& reason) {
        NDN_LOG_WARN("VIDEO_PREFIX_REGISTER_FAILED prefix=" << prefix << " reason=" << reason);
      });
    m_filterRegistered = true;
  }

  void
  putPacket(const ndn::Name& name, const std::vector<uint8_t>& packet)
  {
    const auto putCount = ++m_framePuts;
    if (putCount <= 3 || putCount % 30 == 0) {
      NDN_LOG_INFO("VIDEO_PACKET_PUT count=" << putCount
                   << " name=" << name
                   << " bytes=" << packet.size());
    }
    auto data = std::make_shared<ndn::Data>(name);
    data->setFreshnessPeriod(80_ms);
    data->setContent(ndn::span<const uint8_t>(packet.data(), packet.size()));
    {
      std::lock_guard<std::mutex> guard(m_signMutex);
      m_keyChain.sign(*data);
    }
    m_face.getIoContext().post([this, data] {
      m_face.put(*data);
    });
  }

  void
  rememberPacket(const ndn::Name& name, const std::vector<uint8_t>& packet)
  {
    ndn::Name pendingName;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      const auto uri = name.toUri();
      m_packets[uri] = packet;
      m_order.push_back(uri);
      while (m_order.size() > 600) {
        m_packets.erase(m_order.front());
        m_order.pop_front();
      }
      const auto pending = m_pending.find(uri);
      if (pending != m_pending.end()) {
        pendingName = pending->second;
        m_pending.erase(pending);
      }
    }

    if (!pendingName.empty()) {
      putPacket(pendingName, packet);
    }
  }

  void
  appendStreamChunk(std::vector<uint8_t> chunk, uint64_t nowMs)
  {
    if (!m_streaming.load()) {
      return;
    }
    if (m_fecPendingChunks.empty()) {
      m_fecCurrentFrameStartMs = nowMs;
    }
    m_fecPendingChunks.push_back(std::move(chunk));

    if (m_fecPendingChunks.size() >= m_fecDataShards ||
        (m_fecCurrentFrameStartMs != 0 &&
         nowMs >= m_fecCurrentFrameStartMs + m_fecFrameTimeoutMs)) {
      publishCurrentFrame(nowMs);
    }
  }

  void
  publishCurrentFrame(uint64_t captureMs)
  {
    const auto dataShardCount = m_fecPendingChunks.size();
    if (dataShardCount == 0 || !m_streaming.load()) {
      return;
    }

    const auto frameSeq = m_nextFecFrameSeq++;
    const auto second = captureMs / 1000;
    auto dataChunks = std::move(m_fecPendingChunks);
    m_fecPendingChunks.clear();
    m_fecCurrentFrameStartMs = 0;

    std::vector<size_t> dataLengths;
    dataLengths.reserve(dataChunks.size());
    size_t maxPayloadSize = 0;
    for (const auto& payload : dataChunks) {
      dataLengths.push_back(payload.size());
      maxPayloadSize = std::max(maxPayloadSize, payload.size());
    }

    std::vector<uint8_t> parityPayload(maxPayloadSize, 0);
    for (const auto& payload : dataChunks) {
      for (size_t i = 0; i < payload.size(); ++i) {
        parityPayload[i] ^= payload[i];
      }
    }

    const auto firstPacketSeq = allocatePacketRange(dataShardCount + m_fecParityShards);
    const auto frameLastPacketSeq = firstPacketSeq + dataShardCount + m_fecParityShards - 1;
    const auto dataLengthsCsv = joinFecLengths(dataLengths);
    m_nextSeq += static_cast<uint64_t>(dataShardCount + m_fecParityShards);

    for (uint64_t i = 0; i < dataShardCount; ++i) {
      VideoPacket packet;
      packet.second = second;
      packet.packetSeq = firstPacketSeq + i;
      packet.frameSeq = frameSeq;
      packet.captureMs = captureMs;
      packet.frameFirstPacketSeq = firstPacketSeq;
      packet.frameLastPacketSeq = frameLastPacketSeq;
      packet.bucketPacketCount = frameLastPacketSeq + 1;
      packet.frameSegmentIndex = static_cast<uint32_t>(i);
      packet.frameSegmentCount = static_cast<uint32_t>(dataShardCount + m_fecParityShards);
      packet.keyFrame = ((frameSeq % 30) == 0);
      packet.encoding = "video/h264";
      packet.fecDataShards = static_cast<uint32_t>(dataShardCount);
      packet.fecParityShards = static_cast<uint32_t>(m_fecParityShards);
      packet.fecSymbolIndex = static_cast<uint32_t>(i);
      packet.fecSymbolCount = static_cast<uint32_t>(dataShardCount + m_fecParityShards);
      packet.fecDataLengths = dataLengthsCsv;
      packet.payload = std::move(dataChunks[i]);

      ndn::Name packetName = m_streamPrefix;
      packetName.append(std::to_string(packet.packetSeq));
      rememberPacket(packetName, encodeVideoPacket(packet));
    }

    for (uint64_t i = 0; i < m_fecParityShards; ++i) {
      const auto symbolIndex = dataShardCount + i;
      VideoPacket packet;
      packet.second = second;
      packet.packetSeq = firstPacketSeq + symbolIndex;
      packet.frameSeq = frameSeq;
      packet.captureMs = captureMs;
      packet.frameFirstPacketSeq = firstPacketSeq;
      packet.frameLastPacketSeq = frameLastPacketSeq;
      packet.bucketPacketCount = frameLastPacketSeq + 1;
      packet.frameSegmentIndex = static_cast<uint32_t>(symbolIndex);
      packet.frameSegmentCount = static_cast<uint32_t>(dataShardCount + m_fecParityShards);
      packet.keyFrame = false;
      packet.encoding = "video/h264";
      packet.fecDataShards = static_cast<uint32_t>(dataShardCount);
      packet.fecParityShards = static_cast<uint32_t>(m_fecParityShards);
      packet.fecSymbolIndex = static_cast<uint32_t>(symbolIndex);
      packet.fecSymbolCount = static_cast<uint32_t>(dataShardCount + m_fecParityShards);
      packet.fecDataLengths = dataLengthsCsv;
      packet.payload = parityPayload;

      ndn::Name packetName = m_streamPrefix;
      packetName.append(std::to_string(packet.packetSeq));
      rememberPacket(packetName, encodeVideoPacket(packet));
    }
  }

  uint64_t
  allocatePacketRange(uint64_t count)
  {
    const auto first = m_nextPacketSeq.fetch_add(count);
    return first;
  }

  void
  captureLoop()
  {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(nullptr, pclose);
    std::vector<uint8_t> chunkBuffer;
    while (!m_done.load()) {
      if (!m_streaming.load()) {
        pipe.reset();
        std::lock_guard<std::mutex> guard(m_mutex);
        m_jpegBuffer.clear();
        std::this_thread::sleep_for(50ms);
        continue;
      }

      if (m_restartEncoder.exchange(false)) {
        pipe.reset();
      }

      if (!pipe) {
        const auto fps = m_targetFps.load();
        const auto width = m_acceptedFrameWidth.load();
        const auto command =
          "ffmpeg -loglevel error -re -stream_loop -1 -i " + shellQuote(m_videoPath) +
          " -vf fps=" + std::to_string(fps) +
          ",scale=" + std::to_string(width) + ":-2 -an "
          " -c:v libx264 -preset veryfast -tune zerolatency -x264-params keyint=60:min-keyint=60:scenecut=0 "
          "-f h264 pipe:1";
        pipe.reset(popen(command.c_str(), "r"));
        if (!pipe) {
          NDN_LOG_WARN("VIDEO_ENCODER_START_FAILED path=" << m_videoPath);
          std::this_thread::sleep_for(1s);
          continue;
        }
      }

      std::array<uint8_t, 8192> buffer{};
      const auto n = fread(buffer.data(), 1, buffer.size(), pipe.get());
      if (n == 0) {
        pipe.reset();
        continue;
      }

      chunkBuffer.insert(chunkBuffer.end(), buffer.begin(), buffer.begin() + n);
      while (chunkBuffer.size() >= MAX_VIDEO_PACKET_PAYLOAD) {
        const auto chunkSize = std::min(MAX_VIDEO_PACKET_PAYLOAD, chunkBuffer.size());
        std::vector<uint8_t> packetBytes(chunkBuffer.begin(), chunkBuffer.begin() + chunkSize);
        chunkBuffer.erase(chunkBuffer.begin(), chunkBuffer.begin() + chunkSize);
        appendStreamChunk(std::move(packetBytes), nowMilliseconds());
      }
      if (!m_fecPendingChunks.empty() &&
          m_fecCurrentFrameStartMs != 0 &&
          nowMilliseconds() >= m_fecCurrentFrameStartMs + m_fecFrameTimeoutMs) {
        publishCurrentFrame(nowMilliseconds());
      }
    }
  }

private:
  static constexpr size_t MAX_VIDEO_PACKET_PAYLOAD = 3600;
  static constexpr uint64_t MIN_VIDEO_BITRATE_KBPS = 256;
  static constexpr uint64_t MAX_VIDEO_BITRATE_KBPS = 16000;
  static constexpr uint64_t MIN_VIDEO_FRAME_WIDTH = 160;
  static constexpr uint64_t MAX_VIDEO_FRAME_WIDTH = 1280;
  ndn::Face& m_face;
  ndn::KeyChain& m_keyChain;
  std::string m_droneId;
  std::string m_videoPath;
  mutable std::mutex m_mutex;
  std::mutex m_signMutex;
  ndn::Name m_videoPrefix;
  ndn::Name m_streamPrefix;
  std::string m_streamId = "idle";
  bool m_filterRegistered = false;
  std::atomic<bool> m_streaming{false};
  std::atomic<bool> m_done{false};
  std::atomic<uint64_t> m_nextSeq{0};
  std::atomic<uint64_t> m_nextPacketSeq{0};
  std::atomic<uint64_t> m_nextFecFrameSeq{0};
  std::atomic<uint64_t> m_frameInterests{0};
  std::atomic<uint64_t> m_framePuts{0};
  std::atomic<uint64_t> m_targetFps{30};
  std::atomic<uint64_t> m_requestedBitrateKbps{8000};
  std::atomic<uint64_t> m_acceptedBitrateKbps{8000};
  std::atomic<uint64_t> m_requestedFrameWidth{480};
  std::atomic<uint64_t> m_acceptedFrameWidth{480};
  std::atomic<uint64_t> m_encoderQuality{6};
  std::atomic<bool> m_restartEncoder{false};
  std::map<std::string, std::vector<uint8_t>> m_packets;
  std::deque<std::string> m_order;
  std::map<std::string, ndn::Name> m_pending;
  std::vector<uint8_t> m_jpegBuffer;
  std::vector<std::vector<uint8_t>> m_fecPendingChunks;
  uint64_t m_fecCurrentFrameStartMs = 0;
  uint64_t m_fecDataShards = 8;
  uint64_t m_fecParityShards = 1;
  static constexpr uint64_t m_fecFrameTimeoutMs = 35;
  std::thread m_captureThread;
};

class DroneRuntime
{
public:
  DroneRuntime(std::string droneId, bool available, bool serveCertificates, std::string videoPath)
    : m_serveCertificates(serveCertificates)
    , m_droneId(std::move(droneId))
    , m_available(available)
    , m_identity(droneIdentity(m_droneId))
  {
    KeyChainInitLock lock(("/tmp/ndnsf-uav-keychain-" + std::to_string(getuid()) + ".lock").c_str());
    m_providerCert = getOrCreateIdentity(m_keyChain, m_identity);
    m_controllerCert = getOrCreateIdentity(m_keyChain, CONTROLLER_PREFIX);
    m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(m_identity));
    m_videoPath = std::move(videoPath);
  }

  ~DroneRuntime()
  {
    m_statusCallback = nullptr;
    m_done = true;
    m_face.getIoContext().stop();
    if (m_faceThread.joinable()) {
      m_faceThread.join();
    }
  }

  void
  start()
  {
    m_faceThread = std::thread([this] {
      try {
        auto provider = std::make_unique<ndn_service_framework::ServiceProvider>(
          m_face, GROUP_PREFIX, m_providerCert, m_controllerCert, TRUST_SCHEMA);
        if (m_serveCertificates) {
          m_certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
            m_face, m_keyChain, m_providerCert.getName());
        }
        auto videoPublisher = std::make_unique<VideoPublisher>(
          m_face, m_keyChain, m_droneId, m_videoPath);
        {
          std::lock_guard<std::mutex> guard(m_runtimeMutex);
          m_provider = std::move(provider);
          m_videoPublisher = std::move(videoPublisher);
        }
        installServices();
        m_provider->init();
        m_provider->fetchPermissionsFromController(CONTROLLER_PREFIX);
        m_runtimeReady = true;
        publishStatus("NDNSF runtime ready");

        while (!m_done.load()) {
          m_face.getIoContext().run_for(std::chrono::milliseconds(10));
          m_face.getIoContext().restart();
        }
      }
      catch (const std::exception& e) {
        publishStatus(std::string("NDNSF runtime error: ") + e.what());
        m_done = true;
      }
    });
  }

  bool
  waitUntilReady(std::chrono::seconds timeout)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (m_runtimeReady.load()) {
        return true;
      }
      if (m_done.load()) {
        return false;
      }
      std::this_thread::sleep_for(50ms);
    }
    return m_runtimeReady.load();
  }

  void
  setStatusCallback(std::function<void(std::string)> callback)
  {
    m_statusCallback = std::move(callback);
  }

  bool
  isStreaming() const
  {
    std::lock_guard<std::mutex> guard(m_runtimeMutex);
    return m_videoPublisher != nullptr && m_videoPublisher->isStreaming();
  }

  uint64_t
  framesPublished() const
  {
    std::lock_guard<std::mutex> guard(m_runtimeMutex);
    return m_videoPublisher != nullptr ? m_videoPublisher->framesPublished() : 0;
  }

  std::string
  identityUri() const
  {
    return m_identity.toUri();
  }

private:
  void
  publishStatus(const std::string& value)
  {
    NDN_LOG_INFO("DRONE_STATUS drone=" << m_droneId << " " << value);
    std::cout << "DRONE_STATUS drone=" << m_droneId << " " << value << std::endl;
    if (m_statusCallback) {
      m_statusCallback(value);
    }
  }

  void
  installServices()
  {
    m_provider->setHandlerThreads(2);
    m_provider->setAckThreads(2);
    m_provider->setPerformanceMode(false);

    auto backend = std::make_shared<MockFlightControllerBackend>(m_droneId);
    auto lastMission = std::make_shared<std::string>("idle");
    auto missionMutex = std::make_shared<std::mutex>();

    auto ackHandler = [this](const ndn_service_framework::RequestMessage&) {
      ndn_service_framework::ServiceProvider::AckDecision decision;
      decision.status = m_available;
      decision.message = m_available ? "drone ready" : "drone unavailable";
      decision.payload = bufferFromString(encodeFields({
        {"drone_id", m_droneId},
        {"backend", "mock-flight-controller"},
        {"queue", "0"},
        {"streaming", isStreaming() ? "true" : "false"},
      }));
      return decision;
    };

    m_provider->addService(
      droneVideoControlService(m_droneId),
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [this](const ndn_service_framework::RequestMessage& request) {
          const auto fields = decodeFields(payloadToString(request));
          const auto action = fieldOr(fields, "action", "start");
          if (action == "start") {
            std::lock_guard<std::mutex> guard(m_runtimeMutex);
            const auto responseFields = m_videoPublisher->start(fields);
            publishStatus("video streaming");
            return makeResponse(true, encodeFields(responseFields));
          }
          if (action == "stop") {
            std::lock_guard<std::mutex> guard(m_runtimeMutex);
            const auto responseFields = m_videoPublisher->stop();
            publishStatus("video stopped");
            return makeResponse(true, encodeFields(responseFields));
          }
          return makeResponse(false, encodeFields({
            {"status", "rejected"},
            {"reason", "unknown video control action"},
            {"action", action},
          }), "unknown video control action");
        }));

    m_provider->addService(
      SERVICE_MAVLINK_EXECUTE,
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::RequestHandler(
        [backend, this](const ndn::Name&, const ndn::Name&, const ndn::Name&,
                        const ndn::Name&,
                        const ndn_service_framework::RequestMessage& request) {
          const auto fields = decodeFields(payloadToString(request));
          const auto frame = hexDecode(fieldOr(fields, "mavlink_hex", ""));
          const bool ok = backend->sendMavlink(frame);
          return makeResponse(ok, encodeFields({
            {"accepted", ok ? "true" : "false"},
            {"drone_id", m_droneId},
            {"command", fieldOr(fields, "command", "unknown")},
            {"forwarded_bytes", std::to_string(frame.size())},
          }), ok ? "No error" : "mock flight-controller rejected frame");
        }));

    m_provider->addService(
      SERVICE_TELEMETRY_STATUS,
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [this, lastMission, missionMutex](const ndn_service_framework::RequestMessage&) {
          std::string mission;
          {
            std::lock_guard<std::mutex> guard(*missionMutex);
            mission = *lastMission;
          }
          return makeResponse(true, encodeFields({
            {"drone_id", m_droneId},
            {"lat", "35.3000"},
            {"lon", "-120.6600"},
            {"altitude_m", "42.0"},
            {"battery_percent", "87.5"},
            {"mission_status", mission},
            {"video", isStreaming() ? "streaming" : "stopped"},
            {"timestamp_ms", std::to_string(nowMilliseconds())},
          }));
        }));

    m_provider->addService(
      SERVICE_CAMERA_FRAME,
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [this](const ndn_service_framework::RequestMessage&) {
          const auto frameId = "frame-" + std::to_string(nowMilliseconds());
          const auto image = buildMockJpeg(m_droneId, frameId);
          return makeResponse(true, encodeFields({
            {"drone_id", m_droneId},
            {"frame_id", frameId},
            {"mime", "image/jpeg"},
            {"image_hex", hexEncode(image)},
            {"timestamp_ms", std::to_string(nowMilliseconds())},
          }));
        }));

    m_provider->addService(
      SERVICE_MISSION_ASSIGN,
      ndn_service_framework::ServiceProvider::AckStrategyHandler(ackHandler),
      ndn_service_framework::ServiceProvider::SimpleRequestHandler(
        [backend, this, lastMission, missionMutex](
          const ndn_service_framework::RequestMessage& request) {
          const auto fields = decodeFields(payloadToString(request));
          const auto missionId = fieldOr(fields, "mission_id", "mission-unknown");
          const auto role = fieldOr(fields, "role", "survey");
          const auto waypoints = fieldOr(fields, "waypoints", "");

          {
            std::lock_guard<std::mutex> guard(*missionMutex);
            *lastMission = "executing:" + missionId + ":" + role;
          }

          backend->sendMavlink(buildMockMavlinkFrame("mission-waypoints", {
            {"mission_id", missionId},
            {"role", role},
            {"waypoints", waypoints},
          }));
          const auto frameId = "mission-" + missionId + "-capture";
          const auto image = buildMockJpeg(m_droneId, frameId);

          return makeResponse(true, encodeFields({
            {"accepted", "true"},
            {"mission_id", missionId},
            {"drone_id", m_droneId},
            {"role", role},
            {"status", "mission-executed-with-mock-fc"},
            {"captured_frame_id", frameId},
            {"captured_image_bytes", std::to_string(image.size())},
            {"object_detection_service", SERVICE_GS_OBJECT_DETECTION.toUri()},
            {"detection_summary", "mock-detected=road,vehicle;confidence=0.91"},
          }));
        }));
  }

private:
  bool m_serveCertificates;
  std::string m_droneId;
  bool m_available;
  ndn::Name m_identity;
  std::string m_videoPath;
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::security::Certificate m_providerCert;
  ndn::security::Certificate m_controllerCert;
  std::unique_ptr<ndn_service_framework::CertificatePublisher> m_certPublisher;
  std::unique_ptr<ndn_service_framework::ServiceProvider> m_provider;
  std::unique_ptr<VideoPublisher> m_videoPublisher;
  mutable std::mutex m_runtimeMutex;
  std::thread m_faceThread;
  std::function<void(std::string)> m_statusCallback;
  std::atomic<bool> m_runtimeReady{false};
  std::atomic<bool> m_done{false};
};

class DroneWindow : public Gtk::Window
{
public:
  explicit DroneWindow(DroneRuntime& runtime)
    : m_runtime(runtime)
    , m_box(Gtk::ORIENTATION_VERTICAL, 8)
  {
    set_title("NDNSF UAV Drone");
    set_default_size(420, 180);
    set_border_width(12);

    m_title.set_markup("<b>Drone " + m_runtime.identityUri() + "</b>");
    m_status.set_text("Video stopped");
    m_frames.set_text("Frames published: 0");

    m_box.pack_start(m_title, Gtk::PACK_SHRINK);
    m_box.pack_start(m_status, Gtk::PACK_SHRINK);
    m_box.pack_start(m_frames, Gtk::PACK_SHRINK);
    add(m_box);
    show_all_children();

    m_runtime.setStatusCallback([this](std::string status) {
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_pendingStatus = std::move(status);
      }
      m_dispatcher.emit();
    });
    m_dispatcher.connect([this] {
      std::lock_guard<std::mutex> guard(m_mutex);
      m_status.set_text(m_pendingStatus);
    });
    Glib::signal_timeout().connect([this] {
      m_frames.set_text("Frames published: " + std::to_string(m_runtime.framesPublished()));
      if (!m_runtime.isStreaming()) {
        m_status.set_text("Video stopped");
      }
      return true;
    }, 500);
  }

private:
  DroneRuntime& m_runtime;
  Gtk::Box m_box;
  Gtk::Label m_title;
  Gtk::Label m_status;
  Gtk::Label m_frames;
  Glib::Dispatcher m_dispatcher;
  std::mutex m_mutex;
  std::string m_pendingStatus = "Video stopped";
};

} // namespace

int
main(int argc, char** argv)
{
  try {
    const std::string droneId = getOption(argc, argv, "--drone-id", "A");
    const bool available = !hasFlag(argc, argv, "--unavailable");
    const bool serveCertificates = !hasFlag(argc, argv, "--no-serve-certificates");
    const std::string videoPath = getOption(argc, argv, "--video-source",
                                            "NDNSF-UAV-APP/videos/drone.mp4");

    auto runtime = std::make_unique<DroneRuntime>(droneId, available, serveCertificates, videoPath);
    runtime->start();
    if (!runtime->waitUntilReady(std::chrono::seconds(30))) {
      throw std::runtime_error("drone NDNSF runtime did not become ready");
    }
    auto app = Gtk::Application::create("org.ndnsf.uav.drone", Gio::APPLICATION_NON_UNIQUE);
    DroneWindow window(*runtime);
    NDN_LOG_INFO("UavDroneApp ready identity=" << runtime->identityUri()
                 << " available=" << available
                 << " video_source=" << videoPath);
    std::cout << "DRONE_GUI_READY identity=" << runtime->identityUri()
              << " video_source=" << videoPath << std::endl;
    const int rc = app->run(window);
    std::cout << "DRONE_GUI_EXIT rc=" << rc << std::endl;
    return rc;
  }
  catch (const std::exception& e) {
    std::cerr << "UavDroneApp error: " << e.what() << std::endl;
    return 1;
  }
}
