#include "../shared/UavNames.hpp"
#include "../shared/UavProtocol.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <gdkmm/pixbufloader.h>
#include <gtkmm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

NDN_LOG_INIT(ndn_service_framework.examples.UavGroundStationApp);

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

ndn_service_framework::RequestMessage
makeRequest(const std::string& payload, size_t strategy = ndn_service_framework::tlv::FirstResponding)
{
  auto requestPayload = bufferFromString(payload);
  ndn_service_framework::RequestMessage request;
  request.setPayload(requestPayload, requestPayload.size());
  request.setStrategy(strategy);
  return request;
}

std::string
responsePayload(const ndn_service_framework::ResponseMessage& response)
{
  const auto payload = response.getPayload();
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

class GroundStationRuntime
{
public:
  GroundStationRuntime(bool serveCertificates, int ackTimeoutMs, int timeoutMs,
                       std::string targetDroneId, uint64_t videoBitrateKbps)
    : m_serveCertificates(serveCertificates)
    , m_ackTimeoutMs(ackTimeoutMs)
    , m_timeoutMs(timeoutMs)
    , m_targetDroneId(std::move(targetDroneId))
    , m_videoBitrateKbps(videoBitrateKbps)
  {
    KeyChainInitLock lock(("/tmp/ndnsf-uav-keychain-" + std::to_string(getuid()) + ".lock").c_str());
    m_gsCert = getOrCreateIdentity(m_keyChain, GROUND_STATION_IDENTITY);
    m_controllerCert = getOrCreateIdentity(m_keyChain, CONTROLLER_PREFIX);
    m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(GROUND_STATION_IDENTITY));
  }

  ~GroundStationRuntime()
  {
    m_streaming = false;
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
        if (m_serveCertificates) {
          m_certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
            m_face, m_keyChain, m_gsCert.getName());
        }
        m_user = std::make_unique<ndn_service_framework::ServiceUser>(
          m_face, GROUP_PREFIX, m_gsCert, m_controllerCert, TRUST_SCHEMA);
        m_user->setHandlerThreads(2);
        m_user->init();
        m_user->fetchPermissionsFromController(CONTROLLER_PREFIX);
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

  void
  setFrameCallback(std::function<void(std::vector<uint8_t>, uint64_t, uint64_t)> callback)
  {
    m_frameCallback = std::move(callback);
  }

  void
  startVideo()
  {
    m_seenVideoStart = false;
    startVideoAttempt();
    std::thread([this] {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      if (!m_streaming.load() && !m_done.load()) {
        publishStatus("Retrying video start");
        startVideoAttempt();
      }
    }).detach();
  }

  void
  stopVideo()
  {
    m_streaming = false;
    postRequest(droneVideoControlService(m_targetDroneId),
                encodeFields({{"type", "video-control"}, {"action", "stop"}}),
                [this](const std::string& payload) {
                  const auto fields = decodeFields(payload);
                  publishStatus("Video stopped, frames=" + fieldOr(fields, "frames_published", "0"));
                });
  }

private:
  void
  startVideoAttempt()
  {
    postRequest(droneVideoControlService(m_targetDroneId),
                encodeFields({
                  {"type", "video-control"},
                  {"action", "start"},
                  {"fps", std::to_string(VIDEO_FPS)},
                  {"requested_bitrate_kbps", std::to_string(m_videoBitrateKbps)},
                }),
                [this](const std::string& payload) {
                  const auto fields = decodeFields(payload);
                  const auto prefix = fieldOr(fields, "stream_prefix", "");
                  const auto seqText = fieldOr(fields, "next_seq", "0");
                  if (prefix.empty()) {
                    publishStatus("Video control response missing stream prefix");
                    return;
                  }

                  m_streamPrefix = ndn::Name(prefix);
                  configurePrefetch(fields);
                  m_keyLane = PacketLane{};
                  m_deltaLane = PacketLane{"packet", 0, 0, 0, 0};
                  m_streaming = true;
                  m_seenVideoStart = true;
                  m_firstFrameMs = 0;
                  m_receivedChunks = 0;
                  m_frameNacks = 0;
                  m_frameTimeouts = 0;
                  m_frames.clear();
                  m_readyFrames.clear();
                  m_nextPlaybackFrame = 0;
                  publishStatus("Video packet stream from " + prefix);
                  requestVideoPackets();
                },
                [this] { return m_seenVideoStart.load(); });
  }

  void
  publishStatus(const std::string& value)
  {
    NDN_LOG_INFO("GS_STATUS " << value);
    std::cout << "GS_STATUS " << value << std::endl;
    if (m_statusCallback) {
      m_statusCallback(value);
    }
  }

  void
  postRequest(const ndn::Name& service, const std::string& payload,
              std::function<void(std::string)> onSuccess,
              std::function<bool()> ignoreTimeout = {})
  {
    m_face.getIoContext().post([this, service, payload,
                                onSuccess = std::move(onSuccess),
                                ignoreTimeout = std::move(ignoreTimeout)] {
      if (!m_runtimeReady.load() || !m_user) {
        publishStatus("NDNSF runtime not ready for " + service.toUri());
        return;
      }
      auto requestMessage = makeRequest(payload);
      m_user->RequestService(
        std::vector<ndn::Name>{droneIdentity(m_targetDroneId)},
        service,
        std::move(requestMessage),
        m_ackTimeoutMs,
        ndn_service_framework::ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
        m_timeoutMs,
        [this, service, ignoreTimeout = std::move(ignoreTimeout)](const ndn::Name&) {
          if (ignoreTimeout && ignoreTimeout()) {
            return;
          }
          publishStatus("Timeout waiting for " + service.toUri());
        },
        [onSuccess, service](const ndn_service_framework::ResponseMessage& response) {
          const auto payloadText = responsePayload(response);
          NDN_LOG_INFO("GS_RESPONSE service=" << service << " payload=" << payloadText);
          onSuccess(payloadText);
        });
    });
  }

  struct PacketLane
  {
    std::string kind;
    uint64_t second = 0;
    uint64_t nextSeq = 0;
    uint64_t inFlight = 0;
    uint64_t maxPacketsPerSecond = 0;
    uint64_t prefetchLimit = 0;
    uint64_t advertisedPackets = 0;
    uint64_t probeNotBeforeMs = 0;
  };

  struct FrameAssembly
  {
    uint32_t segmentCount = 0;
    uint32_t received = 0;
    uint64_t captureMs = 0;
    bool keyFrame = false;
    std::vector<std::vector<uint8_t>> segments;
  };

  struct ReadyFrame
  {
    std::vector<uint8_t> image;
    uint64_t elapsedMs = 0;
    bool keyFrame = false;
  };

  void
  requestVideoPackets()
  {
    if (!m_streaming.load()) {
      return;
    }
    requestVideoLane(m_deltaLane, m_deltaWindow);
  }

  static uint64_t
  fieldAsUint64(const Fields& fields, const std::string& key, uint64_t fallback)
  {
    try {
      return std::stoull(fieldOr(fields, key, std::to_string(fallback)));
    }
    catch (const std::exception&) {
      return fallback;
    }
  }

  void
  configurePrefetch(const Fields& fields)
  {
    const auto bitrateKbps = std::max<uint64_t>(
      128, fieldAsUint64(fields, "accepted_bitrate_kbps",
                         fieldAsUint64(fields, "target_bitrate_kbps", m_videoBitrateKbps)));
    const auto payloadBytes = std::max<uint64_t>(
      512, fieldAsUint64(fields, "max_payload_bytes", 3600));
    const auto fps = std::max<uint64_t>(1, fieldAsUint64(fields, "fps", VIDEO_FPS));
    const auto bytesPerSecond = (bitrateKbps * 1000 + 7) / 8;
    const auto estimatedPacketsPerSecond =
      std::max<uint64_t>(fps, (bytesPerSecond + payloadBytes - 1) / payloadBytes);

    m_keyPacketsPerSecond = std::clamp<uint64_t>(
      (estimatedPacketsPerSecond + 7) / 8, 4, 16);
    m_deltaPacketsPerSecond = std::clamp<uint64_t>(
      estimatedPacketsPerSecond + m_keyPacketsPerSecond + 8, 24, 180);
    m_keyWindow = std::clamp<uint64_t>(m_keyPacketsPerSecond, 4, 16);
    m_deltaWindow = std::clamp<uint64_t>(
      (m_deltaPacketsPerSecond + 2) / 3, 24, 48);

    NDN_LOG_INFO("GS_VIDEO_PREFETCH bitrateKbps=" << bitrateKbps
                 << " payloadBytes=" << payloadBytes
                 << " fps=" << fps
                 << " keyBudget=" << m_keyPacketsPerSecond
                 << " deltaBudget=" << m_deltaPacketsPerSecond
                 << " keyWindow=" << m_keyWindow
                 << " deltaWindow=" << m_deltaWindow);
    std::cout << "GS_VIDEO_PREFETCH bitrateKbps=" << bitrateKbps
              << " keyBudget=" << m_keyPacketsPerSecond
              << " deltaBudget=" << m_deltaPacketsPerSecond
              << " deltaWindow=" << m_deltaWindow << std::endl;
  }

  void
  requestVideoLane(PacketLane& lane, uint64_t window)
  {
    advanceLaneIfStale(lane);
    while (m_streaming.load() && lane.inFlight < window) {
      advanceLaneIfStale(lane);
      const auto highWaterLimit = lane.advertisedPackets == 0 ?
        INITIAL_PACKET_PROBE :
        lane.advertisedPackets + VIDEO_PACKET_LOOKAHEAD;
      if (lane.prefetchLimit == 0 &&
          lane.nextSeq >= highWaterLimit) {
        break;
      }
      if (lane.probeNotBeforeMs > 0 &&
          nowMilliseconds() < lane.probeNotBeforeMs &&
          lane.nextSeq >= lane.advertisedPackets) {
        break;
      }
      const auto packetSeq = lane.nextSeq++;
      ++lane.inFlight;
      ndn::Name packetName = m_streamPrefix;
      packetName.append(std::to_string(packetSeq));
      auto interest = ndn::Interest(packetName);
      interest.setMustBeFresh(false);
      interest.setInterestLifetime(300_ms);

      m_face.expressInterest(
        interest,
        [this, &lane, packetSeq](const ndn::Interest&, const ndn::Data& data) {
          if (lane.inFlight > 0) {
            --lane.inFlight;
          }
          lane.probeNotBeforeMs = 0;
          advanceLaneIfStale(lane);
        const auto receivedCount = ++m_receivedChunks;
        if (receivedCount <= 3 || receivedCount % 30 == 0) {
          NDN_LOG_INFO("GS_VIDEO_CHUNK count=" << receivedCount
                         << " packetSeq=" << packetSeq
                       << " name=" << data.getName()
                       << " bytes=" << data.getContent().value_size());
          std::cout << "GS_VIDEO_CHUNK count=" << receivedCount
                      << " packetSeq=" << packetSeq
                    << " bytes=" << data.getContent().value_size() << std::endl;
        }
        const auto receivedMs = nowMilliseconds();
        if (m_firstFrameMs == 0) {
          m_firstFrameMs = receivedMs;
        }
        const auto content = data.getContent();
        std::vector<uint8_t> bytes(content.value(), content.value() + content.value_size());
          try {
            const auto packet = decodeVideoPacket(bytes);
            updateLaneHighWatermark(lane, packet);
            handleVideoPacket(packet, receivedMs);
          }
          catch (const std::exception& e) {
            NDN_LOG_WARN("GS_VIDEO_PACKET_DECODE_FAILED " << e.what());
          }
          requestVideoPackets();
      },
        [this, &lane, packetSeq](const ndn::Interest&, const ndn::lp::Nack&) {
          if (lane.inFlight > 0) {
            --lane.inFlight;
          }
        const auto nackCount = ++m_frameNacks;
        if (nackCount <= 3 || nackCount % 30 == 0) {
            NDN_LOG_INFO("GS_VIDEO_NACK count=" << nackCount << " packetSeq=" << packetSeq);
          std::cout << "GS_VIDEO_NACK count=" << nackCount
                      << " packetSeq=" << packetSeq << std::endl;
        }
          advanceLaneIfStale(lane);
          requestVideoPackets();
      },
        [this, &lane, packetSeq](const ndn::Interest&) {
          if (lane.inFlight > 0) {
            --lane.inFlight;
          }
        const auto timeoutCount = ++m_frameTimeouts;
        if (timeoutCount <= 3 || timeoutCount % 30 == 0) {
            NDN_LOG_INFO("GS_VIDEO_TIMEOUT count=" << timeoutCount
                         << " packetSeq=" << packetSeq);
          std::cout << "GS_VIDEO_TIMEOUT count=" << timeoutCount
                      << " packetSeq=" << packetSeq << std::endl;
        }
          if (packetSeq >= lane.advertisedPackets &&
              lane.nextSeq > packetSeq) {
            lane.nextSeq = packetSeq;
            lane.probeNotBeforeMs = nowMilliseconds() + PROBE_RETRY_BACKOFF_MS;
          }
          advanceLaneIfStale(lane);
          requestVideoPackets();
      });
    }
  }

  void
  advanceLaneIfStale(PacketLane& lane)
  {
    const auto currentSecond = nowMilliseconds() / 1000;
    if (lane.second == 0) {
      return;
    }
    if (lane.prefetchLimit > 0 && currentSecond >= lane.second) {
      lane.prefetchLimit = 0;
    }
    if (currentSecond > lane.second + 1 ||
        (currentSecond > lane.second &&
         lane.maxPacketsPerSecond > 0 &&
         lane.nextSeq >= lane.maxPacketsPerSecond &&
         lane.inFlight == 0)) {
      lane.second = currentSecond;
      lane.nextSeq = 0;
      lane.inFlight = 0;
      lane.prefetchLimit = 0;
      lane.advertisedPackets = 0;
      lane.probeNotBeforeMs = 0;
    }
  }

  bool
  maybeAdvanceLaneForNextSecondPrefetch(PacketLane& lane)
  {
    if (lane.maxPacketsPerSecond == 0) {
      return false;
    }

    const auto nowMs = nowMilliseconds();
    const auto currentSecond = nowMs / 1000;
    const auto millisInSecond = nowMs % 1000;
    if (lane.second != currentSecond ||
        millisInSecond < NEXT_SECOND_PREFETCH_AFTER_MS) {
      return false;
    }

    lane.second = currentSecond + 1;
    lane.nextSeq = 0;
    lane.prefetchLimit = std::clamp<uint64_t>(
      lane.maxPacketsPerSecond / 8, 6, 16);
    lane.advertisedPackets = 0;
    lane.probeNotBeforeMs = 0;
    NDN_LOG_DEBUG("GS_VIDEO_PREFETCH_NEXT_SECOND kind=" << lane.kind
                  << " second=" << lane.second
                  << " limit=" << lane.prefetchLimit);
    return true;
  }

  void
  updateLaneHighWatermark(PacketLane& lane, const VideoPacket& packet)
  {
    lane.nextSeq = std::max(lane.nextSeq, packet.packetSeq + 1);
    lane.advertisedPackets = std::max(lane.advertisedPackets, packet.bucketPacketCount);
  }

  void
  handleVideoPacket(const VideoPacket& packet, uint64_t receivedMs)
  {
    if (packet.frameSeq + 90 < m_latestDecodedFrame.load()) {
      return;
    }

    auto& frame = m_frames[packet.frameSeq];
    if (frame.segmentCount == 0) {
      frame.segmentCount = packet.frameSegmentCount;
      frame.captureMs = packet.captureMs;
      frame.keyFrame = packet.keyFrame;
      frame.segments.resize(packet.frameSegmentCount);
    }
    if (packet.frameSegmentIndex >= frame.segments.size() ||
        !frame.segments[packet.frameSegmentIndex].empty()) {
      return;
    }
    frame.segments[packet.frameSegmentIndex] = packet.payload;
    ++frame.received;

    if (frame.received != frame.segmentCount) {
      return;
    }

    std::vector<uint8_t> image;
    size_t total = 0;
    for (const auto& segment : frame.segments) {
      total += segment.size();
    }
    image.reserve(total);
    for (const auto& segment : frame.segments) {
      image.insert(image.end(), segment.begin(), segment.end());
    }

    m_frames.erase(packet.frameSeq);
    m_readyFrames[packet.frameSeq] = ReadyFrame{
      std::move(image),
      receivedMs - m_firstFrameMs,
      packet.keyFrame,
    };
    flushPlaybackBuffer();
  }

  void
  flushPlaybackBuffer()
  {
    while (true) {
      const auto ready = m_readyFrames.find(m_nextPlaybackFrame);
      if (ready != m_readyFrames.end()) {
        emitReadyFrame(ready);
        ++m_nextPlaybackFrame;
        continue;
      }

      if (m_readyFrames.empty()) {
        break;
      }

      const auto newestReady = m_readyFrames.rbegin()->first;
      if (newestReady < m_nextPlaybackFrame + PLAYBACK_REORDER_WINDOW_FRAMES) {
        break;
      }

      const auto firstReady = m_readyFrames.begin()->first;
      m_nextPlaybackFrame = std::max(m_nextPlaybackFrame + 1, firstReady);
    }

    for (auto it = m_frames.begin(); it != m_frames.end();) {
      if (it->first + PLAYBACK_DROP_WINDOW_FRAMES < m_nextPlaybackFrame) {
        it = m_frames.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  void
  emitReadyFrame(std::map<uint64_t, ReadyFrame>::iterator it)
  {
    const auto frameSeq = it->first;
    auto frame = std::move(it->second);
    m_readyFrames.erase(it);
    if (m_frameCallback) {
      m_frameCallback(std::move(frame.image), frameSeq, frame.elapsedMs);
    }
    m_latestDecodedFrame = frameSeq;
  }

private:
  bool m_serveCertificates;
  int m_ackTimeoutMs;
  int m_timeoutMs;
  std::string m_targetDroneId;
  uint64_t m_videoBitrateKbps = 4000;
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::security::Certificate m_gsCert;
  ndn::security::Certificate m_controllerCert;
  std::unique_ptr<ndn_service_framework::CertificatePublisher> m_certPublisher;
  std::unique_ptr<ndn_service_framework::ServiceUser> m_user;
  std::thread m_faceThread;
  std::function<void(std::string)> m_statusCallback;
  std::function<void(std::vector<uint8_t>, uint64_t, uint64_t)> m_frameCallback;
  std::atomic<bool> m_runtimeReady{false};
  std::atomic<bool> m_streaming{false};
  std::atomic<bool> m_seenVideoStart{false};
  std::atomic<uint64_t> m_firstFrameMs{0};
  std::atomic<uint64_t> m_receivedChunks{0};
  std::atomic<uint64_t> m_frameNacks{0};
  std::atomic<uint64_t> m_frameTimeouts{0};
  std::atomic<uint64_t> m_latestDecodedFrame{0};
  ndn::Name m_streamPrefix;
  PacketLane m_keyLane;
  PacketLane m_deltaLane;
  std::map<uint64_t, FrameAssembly> m_frames;
  std::map<uint64_t, ReadyFrame> m_readyFrames;
  uint64_t m_nextPlaybackFrame = 0;
  uint64_t m_keyPacketsPerSecond = 16;
  uint64_t m_deltaPacketsPerSecond = 160;
  uint64_t m_keyWindow = 16;
  uint64_t m_deltaWindow = 108;
  static constexpr uint64_t VIDEO_FPS = 30;
  static constexpr uint64_t INITIAL_PACKET_PROBE = 8;
  static constexpr uint64_t VIDEO_PACKET_LOOKAHEAD = 12;
  static constexpr uint64_t PROBE_RETRY_BACKOFF_MS = 80;
  static constexpr uint64_t NEXT_SECOND_PREFETCH_AFTER_MS = 700;
  static constexpr uint64_t PLAYBACK_REORDER_WINDOW_FRAMES = 3;
  static constexpr uint64_t PLAYBACK_DROP_WINDOW_FRAMES = 90;
  std::atomic<bool> m_done{false};
};

int
serveObjectDetection(ndn::Face& face, ndn::KeyChain& keyChain,
                     const ndn::security::Certificate& gsCert,
                     const ndn::security::Certificate& controllerCert,
                     bool serveCertificates)
{
  std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
  if (serveCertificates) {
    certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
      face, keyChain, gsCert.getName());
  }

  ndn_service_framework::ServiceProvider provider(
    face, GROUP_PREFIX, gsCert, controllerCert, TRUST_SCHEMA);
  provider.setHandlerThreads(2);
  provider.setAckThreads(2);
  provider.addService(
    SERVICE_GS_OBJECT_DETECTION,
    ndn_service_framework::ServiceProvider::SimpleAckStrategyHandler(
      [](const ndn_service_framework::RequestMessage&) { return true; }),
    ndn_service_framework::ServiceProvider::SimpleRequestHandler(
      [](const ndn_service_framework::RequestMessage& request) {
        const auto payload = request.getPayload();
        const auto fields = decodeFields(std::string(
          reinterpret_cast<const char*>(payload.data()), payload.size()));
        const auto frameId = fieldOr(fields, "frame_id", "frame-unknown");
        return makeResponse(true, encodeFields({
          {"frame_id", frameId},
          {"model", "mock-yolo-gs"},
          {"objects", "road,vehicle,person"},
          {"summary", "mock detection generated at ground station"},
        }));
      }));
  provider.init();
  provider.fetchPermissionsFromController(CONTROLLER_PREFIX);
  NDN_LOG_INFO("UavGroundStationApp object detection service ready");
  face.processEvents();
  return 0;
}

class GroundStationWindow : public Gtk::Window
{
public:
  GroundStationWindow(GroundStationRuntime& runtime, bool autoStart, int autoStopSeconds)
    : m_runtime(runtime)
    , m_box(Gtk::ORIENTATION_VERTICAL, 8)
    , m_buttons(Gtk::ORIENTATION_HORIZONTAL, 8)
    , m_start("Start Video")
    , m_stop("Stop Video")
  {
    set_title("NDNSF UAV Ground Station");
    set_default_size(760, 560);
    set_border_width(12);

    m_status.set_text("Video stopped");
    m_stats.set_text("Frames: 0");
    m_stop.set_sensitive(false);

    m_buttons.pack_start(m_start, Gtk::PACK_SHRINK);
    m_buttons.pack_start(m_stop, Gtk::PACK_SHRINK);
    m_box.pack_start(m_buttons, Gtk::PACK_SHRINK);
    m_box.pack_start(m_status, Gtk::PACK_SHRINK);
    m_box.pack_start(m_image, Gtk::PACK_EXPAND_WIDGET);
    m_box.pack_start(m_stats, Gtk::PACK_SHRINK);
    add(m_box);
    show_all_children();

    m_start.signal_clicked().connect([this] {
      m_start.set_sensitive(false);
      m_stop.set_sensitive(true);
      beginLocalStreamView();
      m_runtime.startVideo();
    });
    m_stop.signal_clicked().connect([this] {
      m_stop.set_sensitive(false);
      m_start.set_sensitive(true);
      m_acceptFrames = false;
      m_runtime.stopVideo();
    });

    m_runtime.setStatusCallback([this](std::string status) {
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (status.rfind("Video packet stream", 0) == 0) {
          beginLocalStreamViewLocked();
        }
        else if (status.rfind("Video stopped", 0) == 0) {
          m_acceptFrames = false;
          status += ", decoded=" + std::to_string(m_decodedFrames.load());
        }
        m_pendingStatus = std::move(status);
      }
      m_statusDispatcher.emit();
    });
    m_runtime.setFrameCallback([this](std::vector<uint8_t> frame, uint64_t seq, uint64_t elapsedMs) {
      pushEncodedChunk(std::move(frame), seq, elapsedMs);
    });

    m_statusDispatcher.connect([this] {
      std::lock_guard<std::mutex> guard(m_mutex);
      m_status.set_text(m_pendingStatus);
    });
    m_frameDispatcher.connect([this] {
      Glib::RefPtr<Gdk::Pixbuf> pixbuf;
      uint64_t seq = 0;
      uint64_t elapsedMs = 0;
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        pixbuf = m_pendingPixbuf;
        seq = m_pendingSeq;
        elapsedMs = m_pendingElapsedMs;
      }
      if (pixbuf) {
        m_image.set(pixbuf);
        m_stats.set_text("Decoded frames: " + std::to_string(m_decodedFrames.load()) +
                         "  latest chunk: " + std::to_string(seq) +
                         "  stream elapsed: " + std::to_string(elapsedMs) + " ms");
      }
    });

    if (autoStart) {
      std::thread([this, autoStopSeconds] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        m_runtime.startVideo();
        std::this_thread::sleep_for(std::chrono::seconds(autoStopSeconds));
        m_runtime.stopVideo();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        Glib::signal_idle().connect_once([this] {
          hide();
        });
      }).detach();
    }
  }

private:
  void
  beginLocalStreamView()
  {
    std::lock_guard<std::mutex> guard(m_mutex);
    beginLocalStreamViewLocked();
  }

  void
  beginLocalStreamViewLocked()
  {
    ++m_streamGeneration;
    m_acceptFrames = true;
    m_decodedFrames = 0;
    m_pendingSeq = 0;
    m_pendingElapsedMs = 0;
    m_pendingPixbuf.reset();
  }

  void
  pushEncodedChunk(std::vector<uint8_t> chunk, uint64_t seq, uint64_t elapsedMs)
  {
    uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      if (!m_acceptFrames) {
        return;
      }
      generation = m_streamGeneration;
    }
    Glib::signal_idle().connect_once([this, chunk = std::move(chunk), seq, elapsedMs, generation] {
      {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (!m_acceptFrames || generation != m_streamGeneration) {
          return;
        }
      }
      auto loader = Gdk::PixbufLoader::create();
      try {
        loader->write(chunk.data(), chunk.size());
        loader->close();
        auto pixbuf = loader->get_pixbuf();
        if (pixbuf) {
          {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_pendingPixbuf = pixbuf;
            m_pendingSeq = seq;
            m_pendingElapsedMs = elapsedMs;
          }
          ++m_decodedFrames;
          if (m_decodedFrames.load() % 30 == 0) {
            std::cout << "GS_DECODED_FRAMES count=" << m_decodedFrames.load() << std::endl;
          }
          m_frameDispatcher.emit();
        }
      }
      catch (const Glib::Error& e) {
        NDN_LOG_WARN("GS_DECODER_ERROR " << e.what());
        std::cout << "GS_DECODER_ERROR " << e.what() << std::endl;
      }
    });
  }

private:
  GroundStationRuntime& m_runtime;
  Gtk::Box m_box;
  Gtk::Box m_buttons;
  Gtk::Button m_start;
  Gtk::Button m_stop;
  Gtk::Label m_status;
  Gtk::Image m_image;
  Gtk::Label m_stats;
  Glib::Dispatcher m_statusDispatcher;
  Glib::Dispatcher m_frameDispatcher;
  std::mutex m_mutex;
  std::string m_pendingStatus = "Video stopped";
  Glib::RefPtr<Gdk::Pixbuf> m_pendingPixbuf;
  uint64_t m_pendingSeq = 0;
  uint64_t m_pendingElapsedMs = 0;
  uint64_t m_streamGeneration = 0;
  bool m_acceptFrames = false;
  std::atomic<uint64_t> m_decodedFrames{0};
};

} // namespace

int
main(int argc, char** argv)
{
  try {
    const bool serveCertificates = !hasFlag(argc, argv, "--no-serve-certificates");
    const bool objectDetectionMode = hasFlag(argc, argv, "--serve-object-detection");
    const bool autoStart = hasFlag(argc, argv, "--auto-video-test");
    const int autoStopSeconds = std::stoi(getOption(argc, argv, "--auto-stop-seconds", "10"));
    const std::string targetDroneId = getOption(argc, argv, "--target-drone", "A");
    const int ackTimeoutMs = std::stoi(getOption(argc, argv, "--ack-timeout-ms", "500"));
    const int timeoutMs = std::stoi(getOption(argc, argv, "--timeout-ms", "10000"));
    const auto videoBitrateKbps = static_cast<uint64_t>(
      std::stoull(getOption(argc, argv, "--video-bitrate-kbps", "4000")));

    if (objectDetectionMode) {
      ndn::Face faceForObjectDetection;
      KeyChainInitLock lock(("/tmp/ndnsf-uav-keychain-" + std::to_string(getuid()) + ".lock").c_str());
      ndn::KeyChain keyChain;
      auto gsCert = getOrCreateIdentity(keyChain, GROUND_STATION_IDENTITY);
      auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);
      keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(GROUND_STATION_IDENTITY));
      return serveObjectDetection(faceForObjectDetection, keyChain, gsCert, controllerCert,
                                  serveCertificates);
    }

    auto runtime = std::make_unique<GroundStationRuntime>(
      serveCertificates, ackTimeoutMs, timeoutMs, targetDroneId, videoBitrateKbps);
    runtime->start();
    if (!runtime->waitUntilReady(std::chrono::seconds(30))) {
      throw std::runtime_error("ground-station NDNSF runtime did not become ready");
    }
    auto app = Gtk::Application::create("org.ndnsf.uav.gs", Gio::APPLICATION_NON_UNIQUE);
    GroundStationWindow window(*runtime, autoStart, autoStopSeconds);
    NDN_LOG_INFO("UavGroundStationApp GUI ready");
    std::cout << "GS_GUI_READY target_drone=" << targetDroneId
              << " auto_video_test=" << (autoStart ? "true" : "false") << std::endl;
    const int rc = app->run(window);
    std::cout << "GS_GUI_EXIT rc=" << rc << std::endl;
    return rc;
  }
  catch (const std::exception& e) {
    std::cerr << "UavGroundStationApp error: " << e.what() << std::endl;
    return 1;
  }
}
