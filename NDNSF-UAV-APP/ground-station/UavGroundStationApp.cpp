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

#include <boost/asio/steady_timer.hpp>

#include <gdkmm/pixbufloader.h>
#include <gtkmm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <signal.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sstream>
#include <set>
#include <sys/file.h>
#include <sys/wait.h>
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

std::vector<std::string>
splitCsv(const std::string& value)
{
  std::vector<std::string> out;
  std::stringstream ss(value);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) {
      out.push_back(item);
    }
  }
  return out;
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
                       std::string targetDroneId, uint64_t videoBitrateKbps,
                       uint64_t videoFrameWidth,
                       std::vector<std::string> patrolDroneIds = {})
    : m_serveCertificates(serveCertificates)
    , m_ackTimeoutMs(ackTimeoutMs)
    , m_timeoutMs(timeoutMs)
    , m_targetDroneId(std::move(targetDroneId))
    , m_videoBitrateKbps(videoBitrateKbps)
    , m_videoFrameWidth(videoFrameWidth)
    , m_patrolDroneIds(std::move(patrolDroneIds))
    , m_videoPumpTimer(m_face.getIoContext())
  {
    if (m_patrolDroneIds.empty()) {
      m_patrolDroneIds.push_back(m_targetDroneId);
    }
    KeyChainInitLock lock(("/tmp/ndnsf-uav-keychain-" + std::to_string(getuid()) + ".lock").c_str());
    m_gsCert = getOrCreateIdentity(m_keyChain, GROUND_STATION_IDENTITY);
    m_controllerCert = getOrCreateIdentity(m_keyChain, CONTROLLER_PREFIX);
    m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(GROUND_STATION_IDENTITY));
  }

  ~GroundStationRuntime()
  {
    m_streaming = false;
    m_done = true;
    stopDecoder();
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
    if (m_streaming.load()) {
      publishStatus("Video already streaming");
      return;
    }
    if (m_videoStartInFlight.exchange(true)) {
      publishStatus("Video start already pending");
      return;
    }
    m_seenVideoStart = false;
    startVideoAttempt();
  }

  void
  stopVideo()
  {
    if (m_videoStopInFlight.exchange(true)) {
      publishStatus("Video stop already pending");
      return;
    }
    m_videoStartInFlight = false;
    m_streaming = false;
    m_videoPumpScheduled = false;
    boost::system::error_code ec;
    m_videoPumpTimer.cancel(ec);
    stopDecoder();
    postRequest(droneVideoControlService(m_targetDroneId),
                encodeFields({{"type", "video-control"}, {"action", "stop"}}),
                [this](const std::string& payload) {
                  m_videoStopInFlight = false;
                  const auto fields = decodeFields(payload);
                  publishStatus("Video stopped, frames=" + fieldOr(fields, "frames_published", "0"));
                },
                {},
                [this] {
                  m_videoStopInFlight = false;
                });
  }

  bool
  isStreaming() const
  {
    return m_streaming.load();
  }

  bool
  runAutoPatrolCompensationDemo(std::chrono::seconds timeout)
  {
    struct PatrolPart
    {
      std::string id;
      std::string role;
      std::string waypoints;
      std::string assignedDrone;
      std::string completedBy;
      int attempt = 0;
      bool done = false;
    };

    struct PatrolDemoState
    {
      std::mutex mutex;
      std::condition_variable cv;
      std::map<std::string, PatrolPart> parts;
      std::set<std::string> timedOut;
    };

    if (m_patrolDroneIds.size() < 2) {
      publishStatus("Patrol demo needs at least two drones");
      return false;
    }

    const std::string taskId = "patrol-" + std::to_string(nowMilliseconds());
    auto state = std::make_shared<PatrolDemoState>();
    state->parts = {
      {"part0", PatrolPart{"part0", "north-sector", "N0>N1>N2", m_patrolDroneIds[0], "", 0, false}},
      {"part1", PatrolPart{"part1", "south-sector", "S0>S1>S2", m_patrolDroneIds[1], "", 0, false}},
    };

    auto logLedger = [&] (const std::string& line) {
      NDN_LOG_INFO(line);
      std::cout << line << std::endl;
    };

    auto allDone = [state] {
      for (const auto& item : state->parts) {
        if (!item.second.done) {
          return false;
        }
      }
      return true;
    };

    auto joinDroneIds = [] (const std::vector<std::string>& droneIds) {
      std::string out;
      for (size_t i = 0; i < droneIds.size(); ++i) {
        if (i > 0) {
          out += ",";
        }
        out += droneIds[i];
      }
      return out;
    };

    auto dispatchPart = [&] (const std::string& partId, std::vector<std::string> droneIds,
                             int attempt, bool simulateNoResponse) {
      const std::string candidateText = joinDroneIds(droneIds);
      PatrolPart part;
      {
        std::lock_guard<std::mutex> guard(state->mutex);
        auto& storedPart = state->parts[partId];
        storedPart.assignedDrone = candidateText;
        storedPart.attempt = attempt;
        part = storedPart;
      }
      const std::string payload = encodeFields({
        {"type", "patrol-task"},
        {"patrol_task_id", taskId},
        {"mission_id", taskId},
        {"attempt_id", std::to_string(attempt)},
        {"part_id", part.id},
        {"role", part.role},
        {"area", "demo-area"},
        {"waypoints", part.waypoints},
        {"capture_required", "true"},
        {"simulate_no_response", simulateNoResponse ? "true" : "false"},
        {"simulate_delay_ms", "6500"},
      });
      logLedger("PATROL_ASSIGN task=" + taskId +
                " attempt=" + std::to_string(attempt) +
                " part=" + part.id +
                " candidates=" + candidateText +
                " simulate_no_response=" + (simulateNoResponse ? "true" : "false"));

      auto requestMessage = makeRequest(payload);
      std::vector<ndn::Name> providerNames;
      providerNames.reserve(droneIds.size());
      for (const auto& droneId : droneIds) {
        providerNames.push_back(droneIdentity(droneId));
      }
      m_face.getIoContext().post([this, requestMessage = std::move(requestMessage),
                                  providerNames = std::move(providerNames),
                                  taskId, partId, candidateText,
                                  attempt, state,
                                  logLedger] () mutable {
        if (!m_runtimeReady.load() || !m_user) {
          logLedger("PATROL_RUNTIME_NOT_READY task=" + taskId +
                    " part=" + partId);
          std::lock_guard<std::mutex> guard(state->mutex);
          state->timedOut.insert(partId);
          state->cv.notify_all();
          return;
        }
        auto selectIdleCandidate =
          [providerNames, taskId, partId, attempt, logLedger](
            const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
            std::vector<ndn_service_framework::AckSelectionCandidate> selected;
            for (const auto& candidate : candidates) {
              bool inCandidateSet = false;
              for (const auto& providerName : providerNames) {
                if (candidate.providerName.equals(providerName)) {
                  inCandidateSet = true;
                  break;
                }
              }
              if (!inCandidateSet || !candidate.ack.getStatus()) {
                if (!inCandidateSet) {
                  continue;
                }
              }

              const auto payload = candidate.ack.getPayload();
              const auto fields = decodeFields(
                std::string(reinterpret_cast<const char*>(payload.data()),
                            payload.size()));
              if (fieldOr(fields, "mission_busy", "false") == "true") {
                logLedger("PATROL_ACK_BUSY task=" + taskId +
                          " attempt=" + std::to_string(attempt) +
                          " part=" + partId +
                          " provider=" + candidate.providerName.toUri());
                continue;
              }
              if (!candidate.ack.getStatus()) {
                continue;
              }

              logLedger("PATROL_ACK_SELECTED task=" + taskId +
                        " attempt=" + std::to_string(attempt) +
                        " part=" + partId +
                        " provider=" + candidate.providerName.toUri());
              selected.push_back(candidate);
              break;
            }
            return selected;
          };
        m_user->RequestService(
          providerNames,
          SERVICE_MISSION_ASSIGN,
          std::move(requestMessage),
          m_ackTimeoutMs,
          std::move(selectIdleCandidate),
          m_timeoutMs,
          [taskId, partId, candidateText, attempt, state, logLedger](const ndn::Name&) {
            logLedger("PATROL_PART_MISSING task=" + taskId +
                      " attempt=" + std::to_string(attempt) +
                      " part=" + partId +
                      " candidates=" + candidateText);
            {
              std::lock_guard<std::mutex> guard(state->mutex);
              if (!state->parts[partId].done) {
                state->timedOut.insert(partId);
              }
            }
            state->cv.notify_all();
          },
          [taskId, partId, candidateText, attempt, state, logLedger](
            const ndn_service_framework::ResponseMessage& response) {
            const auto fields = decodeFields(responsePayload(response));
            const auto responder = fieldOr(fields, "drone_id", candidateText);
            bool accepted = false;
            {
              std::lock_guard<std::mutex> guard(state->mutex);
              auto& part = state->parts[partId];
              if (!part.done && response.getStatus()) {
                part.done = true;
                part.completedBy = responder;
                accepted = true;
              }
            }
            if (accepted) {
              logLedger("PATROL_PART_DONE task=" + taskId +
                        " attempt=" + std::to_string(attempt) +
                        " part=" + partId +
                        " provider=" + responder +
                        " status=true");
            }
            else {
              logLedger("PATROL_LATE_RESPONSE_IGNORED task=" + taskId +
                        " attempt=" + std::to_string(attempt) +
                        " part=" + partId +
                        " provider=" + responder +
                        " status=" + (response.getStatus() ? "true" : "false"));
            }
            state->cv.notify_all();
          });
      });
    };

    logLedger("PATROL_TASK_START task=" + taskId + " parts=part0,part1");
    logLedger("PATROL_ATTEMPT task=" + taskId + " attempt=1 parts=part0,part1");
    dispatchPart("part0", {m_patrolDroneIds[0]}, 1, true);
    dispatchPart("part1", {m_patrolDroneIds[1]}, 1, false);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait_until(lock, deadline, [&] {
        return state->parts["part1"].done &&
               state->timedOut.find("part0") != state->timedOut.end();
      });
    }

    {
      std::lock_guard<std::mutex> guard(state->mutex);
      if (!state->parts["part0"].done) {
        logLedger("PATROL_COMPENSATION task=" + taskId +
                  " attempt=2 parts=part0 candidates=" + joinDroneIds(m_patrolDroneIds));
      }
      else {
        logLedger("PATROL_TASK_DONE task=" + taskId + " attempts=1");
        return true;
      }
    }
    dispatchPart("part0", m_patrolDroneIds, 2, false);

    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait_until(lock, deadline, allDone);
      if (!allDone()) {
        logLedger("PATROL_TASK_FAILED task=" + taskId);
        return false;
      }
    }
    logLedger("PATROL_TASK_DONE task=" + taskId + " attempts=2");
    return true;
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
                  {"requested_frame_width", std::to_string(m_videoFrameWidth)},
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
	                  m_videoPumpScheduled = false;
	                  m_streaming = true;
	                  m_seenVideoStart = true;
	                  m_videoStartInFlight = false;
	                  m_firstFrameMs = 0;
                  m_receivedChunks = 0;
                  m_frameNacks = 0;
                  m_frameTimeouts = 0;
                  m_nextChunkSeqToDecode = 0;
                  {
                    std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
                    m_chunkQueue.clear();
                    m_pendingChunks.clear();
                    m_decoderOutBuffer.clear();
                  }
                  stopDecoder();
                  startDecoder();
                  publishStatus("Video packet stream from " + prefix);
	                  requestVideoPackets();
	                },
	                [this] { return m_seenVideoStart.load(); },
	                [this] {
	                  if (!m_seenVideoStart.load()) {
	                    m_videoStartInFlight = false;
	                  }
	                });
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
              std::function<bool()> ignoreTimeout = {},
              std::function<void()> onTimeout = {})
  {
    m_face.getIoContext().post([this, service, payload,
                                onSuccess = std::move(onSuccess),
                                ignoreTimeout = std::move(ignoreTimeout),
                                onTimeout = std::move(onTimeout)] {
      if (!m_runtimeReady.load() || !m_user) {
        publishStatus("NDNSF runtime not ready for " + service.toUri());
        if (onTimeout) {
          onTimeout();
        }
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
        [this, service,
         ignoreTimeout = std::move(ignoreTimeout),
         onTimeout = std::move(onTimeout)](const ndn::Name&) {
          if (ignoreTimeout && ignoreTimeout()) {
            return;
          }
          if (onTimeout) {
            onTimeout();
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

  struct StreamChunk
  {
    uint64_t packetSeq = 0;
    uint64_t arrivalMs = 0;
    uint64_t elapsedMs = 0;
    std::vector<uint8_t> payload;
  };

  struct FecFrameState
  {
    bool initialized = false;
    uint64_t frameSeq = 0;
    uint64_t frameFirstPacketSeq = 0;
    uint64_t frameLastPacketSeq = 0;
    uint32_t dataShards = 0;
    uint32_t parityShards = 0;
    uint32_t symbolCount = 0;
    uint64_t firstArrivalMs = 0;
    std::vector<size_t> fecDataLengths;
    std::map<uint32_t, std::vector<uint8_t>> shards;
    bool complete = false;
  };

  void
  requestVideoPackets()
  {
    if (!m_streaming.load()) {
      return;
    }
    requestVideoLane(m_deltaLane, m_deltaWindow);
  }

  void
  scheduleVideoPump(uint64_t delayMs)
  {
    if (!m_streaming.load() || m_videoPumpScheduled.exchange(true)) {
      return;
    }
    m_videoPumpTimer.expires_after(std::chrono::milliseconds(delayMs));
    m_videoPumpTimer.async_wait([this] (const boost::system::error_code& ec) {
      m_videoPumpScheduled = false;
      if (!ec && m_streaming.load()) {
        requestVideoPackets();
      }
    });
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
    const auto frameWidth = std::max<uint64_t>(
      1, fieldAsUint64(fields, "accepted_frame_width",
                       fieldAsUint64(fields, "frame_width", m_videoFrameWidth)));
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
                 << " frameWidth=" << frameWidth
                 << " payloadBytes=" << payloadBytes
                 << " fps=" << fps
                 << " keyBudget=" << m_keyPacketsPerSecond
                 << " deltaBudget=" << m_deltaPacketsPerSecond
                 << " keyWindow=" << m_keyWindow
                 << " deltaWindow=" << m_deltaWindow);
    std::cout << "GS_VIDEO_PREFETCH bitrateKbps=" << bitrateKbps
              << " frameWidth=" << frameWidth
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
        if (lane.inFlight == 0 && lane.advertisedPackets > 0) {
          lane.nextSeq = lane.advertisedPackets;
        }
        scheduleVideoPump(STREAM_PUMP_INTERVAL_MS);
        break;
      }
      if (lane.probeNotBeforeMs > 0 &&
          nowMilliseconds() < lane.probeNotBeforeMs &&
          lane.nextSeq >= lane.advertisedPackets) {
        scheduleVideoPump(PROBE_RETRY_BACKOFF_MS);
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
            queueStreamChunk(packet, receivedMs);
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
          scheduleVideoPump(PROBE_RETRY_BACKOFF_MS);
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

  void
  updateLaneHighWatermark(PacketLane& lane, const VideoPacket& packet)
  {
    lane.nextSeq = std::max(lane.nextSeq, packet.packetSeq + 1);
    lane.advertisedPackets = std::max(lane.advertisedPackets, packet.bucketPacketCount);
  }

  void
  queueStreamChunk(const VideoPacket& packet, uint64_t receivedMs)
  {
    if (!m_decoderRunning.load() || packet.payload.empty()) {
      return;
    }

    if (packet.fecDataShards > 0 || packet.fecParityShards > 0 || packet.fecSymbolCount > 0) {
      processFecChunk(packet, receivedMs);
      return;
    }

    if (packet.packetSeq == UINT64_MAX) {
      return;
    }

    const auto elapsedMs = (m_firstFrameMs == 0 ? 0 : receivedMs - m_firstFrameMs);
    insertChunkForDecode(packet.packetSeq, packet.payload, elapsedMs);
  }

  void
  processFecChunk(const VideoPacket& packet, uint64_t receivedMs)
  {
    if (!m_decoderRunning.load() || packet.payload.empty() ||
        packet.fecSymbolCount == 0 || packet.fecDataShards == 0) {
      return;
    }

    const auto frameSeq = packet.frameSeq;
    auto& state = m_fecFrames[frameSeq];

    if (!state.initialized) {
      state.frameSeq = frameSeq;
      state.frameFirstPacketSeq = packet.frameFirstPacketSeq;
      state.frameLastPacketSeq = packet.frameLastPacketSeq;
      state.dataShards = packet.fecDataShards;
      state.parityShards = packet.fecParityShards;
      state.symbolCount = packet.fecSymbolCount;
      state.fecDataLengths = parseFecDataLengths(packet.fecDataLengths);
      state.firstArrivalMs = receivedMs;
      state.initialized = true;
    }

    state.dataShards = std::max<uint32_t>(state.dataShards, packet.fecDataShards);
    state.parityShards = std::max<uint32_t>(state.parityShards, packet.fecParityShards);
    state.symbolCount = std::max<uint32_t>(state.symbolCount, packet.fecSymbolCount);
    state.frameLastPacketSeq =
      packet.frameLastPacketSeq != 0 ?
      packet.frameLastPacketSeq :
      (packet.frameFirstPacketSeq + state.symbolCount - 1);
    if (state.fecDataLengths.empty() && !packet.fecDataLengths.empty()) {
      state.fecDataLengths = parseFecDataLengths(packet.fecDataLengths);
    }

    if (packet.fecSymbolIndex < packet.fecSymbolCount) {
      state.shards.try_emplace(packet.fecSymbolIndex, packet.payload);
    }

    const auto elapsedMs = (m_firstFrameMs == 0 ? 0 : receivedMs - m_firstFrameMs);
    attemptAndRecoverFrame(state);
    if (packet.fecSymbolIndex < state.dataShards) {
      insertChunkForDecode(packet.packetSeq, packet.payload, elapsedMs);
    }
    if (state.complete) {
      cleanupFecFrames();
    }
  }

  void
  attemptAndRecoverFrame(FecFrameState& state)
  {
    if (state.complete || state.dataShards == 0 || state.symbolCount == 0) {
      return;
    }

    uint32_t receivedDataShards = 0;
    for (uint32_t i = 0; i < state.dataShards; ++i) {
      if (state.shards.find(i) != state.shards.end()) {
        ++receivedDataShards;
      }
    }

    if (receivedDataShards == state.dataShards) {
      state.complete = true;
      return;
    }

    if (receivedDataShards + state.parityShards < state.dataShards) {
      return;
    }

    if (state.dataShards - receivedDataShards != 1) {
      return;
    }

    for (uint32_t missingIdx = 0; missingIdx < state.dataShards; ++missingIdx) {
      if (state.shards.find(missingIdx) != state.shards.end()) {
        continue;
      }
      const auto recovered = recoverFecDataSymbol(state, missingIdx);
      if (recovered.empty()) {
        return;
      }
      const auto recoveredSeq = state.frameFirstPacketSeq + missingIdx;
      const auto recoveredElapsed = (m_firstFrameMs == 0 ? 0 : state.firstArrivalMs - m_firstFrameMs);
      insertChunkForDecode(recoveredSeq, recovered, recoveredElapsed);
      state.shards[missingIdx] = recovered;
      state.complete = true;
      break;
    }
  }

  std::vector<size_t>
  parseFecDataLengths(const std::string& value)
  {
    std::vector<size_t> lengths;
    if (value.empty()) {
      return lengths;
    }

    std::stringstream parser(value);
    std::string token;
    while (std::getline(parser, token, ',')) {
      if (token.empty()) {
        continue;
      }
      try {
        lengths.push_back(std::stoull(token));
      }
      catch (const std::exception&) {
      }
    }
    return lengths;
  }

  std::vector<uint8_t>
  recoverFecDataSymbol(const FecFrameState& state, uint32_t missingIdx)
  {
    if (missingIdx >= state.dataShards || state.fecDataLengths.empty()) {
      return {};
    }
    if (missingIdx >= state.fecDataLengths.size()) {
      return {};
    }

    const auto targetLen = state.fecDataLengths[missingIdx];
    if (targetLen == 0) {
      return {};
    }

    std::vector<uint8_t> recovered(targetLen, 0);
    bool usedParity = false;
    for (uint32_t i = 0; i < state.symbolCount; ++i) {
      if (i == missingIdx) {
        continue;
      }
      const auto it = state.shards.find(i);
      if (it == state.shards.end()) {
        continue;
      }
      const auto& payload = it->second;
      for (size_t j = 0; j < targetLen; ++j) {
        const auto byte = (j < payload.size()) ? payload[j] : 0;
        recovered[j] ^= byte;
      }
      if (i >= state.dataShards) {
        usedParity = true;
      }
    }

    if (!usedParity) {
      return {};
    }
    return recovered;
  }

  void
  cleanupFecFrames()
  {
    for (auto it = m_fecFrames.begin(); it != m_fecFrames.end();) {
      if (it->second.complete ||
          (it->second.frameLastPacketSeq != 0 &&
           it->second.frameLastPacketSeq < m_nextChunkSeqToDecode)) {
        it = m_fecFrames.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  void
  insertChunkForDecode(uint64_t packetSeq, const std::vector<uint8_t>& payload,
                      uint64_t elapsedMs)
  {
    if (packetSeq == UINT64_MAX) {
      return;
    }
    if (packetSeq < m_nextChunkSeqToDecode) {
      ++m_decoderDroppedChunks;
      return;
    }

    bool notifyWriter = false;
    {
      std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
      const auto inserted = m_pendingChunks.emplace(packetSeq, StreamChunk{});
      if (inserted.second) {
        StreamChunk chunk;
        chunk.packetSeq = packetSeq;
        chunk.arrivalMs = (m_firstFrameMs == 0 ? 0 : m_firstFrameMs + elapsedMs);
        chunk.elapsedMs = elapsedMs;
        chunk.payload = payload;
        inserted.first->second = std::move(chunk);
      }

      while (!m_pendingChunks.empty()) {
        auto it = m_pendingChunks.find(m_nextChunkSeqToDecode);
        if (it == m_pendingChunks.end()) {
          break;
        }
        m_chunkQueue.push_back(std::move(it->second));
        m_pendingChunks.erase(it);
        ++m_nextChunkSeqToDecode;
        notifyWriter = true;
      }

      if (m_pendingChunks.size() > m_decoderReorderWindow * 4 &&
          !m_pendingChunks.empty()) {
        auto first = m_pendingChunks.begin();
        if (first->first > m_nextChunkSeqToDecode) {
          NDN_LOG_WARN("GS_VIDEO_SKIP_MISSING_CHUNKS start="
                       << m_nextChunkSeqToDecode << " to=" << first->first - 1);
          m_decoderDroppedChunks += (first->first - m_nextChunkSeqToDecode);
          m_nextChunkSeqToDecode = first->first;
        }
      }

      if (m_pendingChunks.empty() || m_pendingChunks.begin()->first == m_nextChunkSeqToDecode) {
        m_decoderMissingChunkSeq = UINT64_MAX;
        m_decoderMissingChunkStartMs = 0;
      }
      else if (m_decoderMissingChunkSeq != m_nextChunkSeqToDecode) {
        m_decoderMissingChunkSeq = m_nextChunkSeqToDecode;
        m_decoderMissingChunkStartMs = nowMilliseconds();
      }
    }

    if (notifyWriter) {
      m_decoderQueueCv.notify_one();
    }
  }

  void
  startDecoder()
  {
    if (m_decoderRunning.load()) {
      return;
    }
    std::string command =
      "ffmpeg -hide_banner -loglevel error -fflags nobuffer -flags low_delay "
      "-analyzeduration 0 -probesize 32 -f h264 -i pipe:0 -f image2pipe -vcodec mjpeg -";

    if (!startDecoderProcess(command)) {
      publishStatus("Failed to start video decoder");
      return;
    }

    m_decoderRunning = true;
    m_lastOutputChunkSeq = 0;
    m_lastOutputChunkElapsedMs = 0;
    m_decoderDroppedChunks = 0;
    m_decoderMissingChunkSeq = UINT64_MAX;
    m_decoderMissingChunkStartMs = 0;
    m_decoderOutBuffer.clear();
    {
      std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
      m_chunkQueue.clear();
      m_pendingChunks.clear();
      m_nextChunkSeqToDecode = 0;
    }

    m_decoderWriterThread = std::thread([this] { decoderWriterLoop(); });
    m_decoderReaderThread = std::thread([this] { decoderReaderLoop(); });
    publishStatus("Video decoder started");
  }

  void
  stopDecoder()
  {
    m_decoderRunning = false;
    m_decoderQueueCv.notify_all();

    if (m_decoderInFd >= 0) {
      shutdown(m_decoderInFd, SHUT_WR);
      close(m_decoderInFd);
      m_decoderInFd = -1;
    }
    if (m_decoderOutFd >= 0) {
      close(m_decoderOutFd);
      m_decoderOutFd = -1;
    }
    if (m_decoderWriterThread.joinable()) {
      m_decoderWriterThread.join();
    }
    if (m_decoderReaderThread.joinable()) {
      m_decoderReaderThread.join();
    }

    if (m_decoderPid > 0) {
      kill(m_decoderPid, SIGTERM);
      waitpid(m_decoderPid, nullptr, 0);
      m_decoderPid = -1;
    }

    {
      std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
      m_chunkQueue.clear();
      m_pendingChunks.clear();
      m_decoderOutBuffer.clear();
      m_decoderMissingChunkSeq = UINT64_MAX;
      m_decoderMissingChunkStartMs = 0;
    }
    m_decoderDroppedChunks = 0;
  }

  void
  decoderWriterLoop()
  {
    while (m_decoderRunning.load()) {
      StreamChunk chunk;
      {
        std::unique_lock<std::mutex> guard(m_decoderQueueMutex);
        m_decoderQueueCv.wait_for(guard, std::chrono::milliseconds(10), [this] {
          return !m_decoderRunning.load() ||
                 !m_chunkQueue.empty() ||
                 shouldAdvanceMissingChunk();
        });

        if (!m_decoderRunning.load()) {
          return;
        }

        const auto nowMs = nowMilliseconds();
        advanceMissingChunkUnderTimeout(nowMs);
        if (m_chunkQueue.empty()) {
          continue;
        }
        chunk = std::move(m_chunkQueue.front());
        m_chunkQueue.pop_front();
      }

      if (m_decoderInFd < 0) {
        continue;
      }

      if (chunk.payload.empty()) {
        continue;
      }
      m_lastOutputChunkSeq = chunk.packetSeq;
      m_lastOutputChunkElapsedMs = chunk.elapsedMs;

      const auto* data = chunk.payload.data();
      auto remaining = chunk.payload.size();
      while (remaining > 0 && m_decoderRunning.load()) {
        const auto n = write(m_decoderInFd, data, remaining);
        if (n > 0) {
          remaining -= static_cast<size_t>(n);
          data += n;
          continue;
        }
        if (errno == EINTR) {
          continue;
        }
        m_decoderRunning = false;
        return;
      }
    }
  }

  void
  decoderReaderLoop()
  {
    std::vector<uint8_t> buffer(8192);
    while (m_decoderRunning.load()) {
      const auto n = read(m_decoderOutFd, buffer.data(), buffer.size());
      if (n <= 0) {
        if (errno == EINTR) {
          continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      {
        std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
        m_decoderOutBuffer.insert(m_decoderOutBuffer.end(), buffer.data(), buffer.data() + n);
      }
      emitDecodedFramesFromBuffer();
    }
  }

  void
  emitDecodedFramesFromBuffer()
  {
    std::vector<std::vector<uint8_t>> frameCandidates;
    {
      std::lock_guard<std::mutex> guard(m_decoderQueueMutex);
      static constexpr uint8_t kJpegStart[2] = {0xff, 0xd8};
      static constexpr uint8_t kJpegEnd[2] = {0xff, 0xd9};

      while (m_decoderOutBuffer.size() >= 4) {
        const auto start = std::search(
          m_decoderOutBuffer.begin(), m_decoderOutBuffer.end(), std::begin(kJpegStart), std::end(kJpegStart));
        if (start == m_decoderOutBuffer.end()) {
          m_decoderOutBuffer.clear();
          break;
        }
        if (start != m_decoderOutBuffer.begin()) {
          m_decoderOutBuffer.erase(m_decoderOutBuffer.begin(), start);
          if (m_decoderOutBuffer.size() < 4) {
            break;
          }
        }

        const auto end = std::search(
          m_decoderOutBuffer.begin() + 2, m_decoderOutBuffer.end(),
          std::begin(kJpegEnd), std::end(kJpegEnd));
        if (end == m_decoderOutBuffer.end()) {
          break;
        }
        const auto endIt = end + 2;
        if (endIt > m_decoderOutBuffer.end()) {
          break;
        }
        frameCandidates.emplace_back(m_decoderOutBuffer.begin(), endIt);
        m_decoderOutBuffer.erase(m_decoderOutBuffer.begin(), endIt);
      }
    }

    for (auto& frame : frameCandidates) {
      if (m_frameCallback) {
        m_frameCallback(std::move(frame), m_lastOutputChunkSeq, m_lastOutputChunkElapsedMs);
      }
    }
  }

  bool
  shouldAdvanceMissingChunk()
  {
    if (m_decoderMissingChunkSeq == UINT64_MAX || !m_decoderRunning.load()) {
      return false;
    }
    const auto nowMs = nowMilliseconds();
    return nowMs >= m_decoderMissingChunkStartMs + m_decoderMissingTimeoutMs;
  }

  void
  advanceMissingChunkUnderTimeout(uint64_t nowMs)
  {
    if (m_decoderMissingChunkSeq == UINT64_MAX || m_pendingChunks.empty() ||
        m_decoderRunning.load() == false) {
      return;
    }

    const auto now = nowMs;
    const auto first = m_pendingChunks.begin();
    if (first->first <= m_nextChunkSeqToDecode) {
      m_decoderMissingChunkSeq = UINT64_MAX;
      m_decoderMissingChunkStartMs = 0;
      return;
    }

    if (first->first > m_nextChunkSeqToDecode &&
        now >= m_decoderMissingChunkStartMs + m_decoderMissingTimeoutMs) {
      NDN_LOG_WARN("GS_VIDEO_SKIP_MISSING_CHUNKS_TIMEOUT start=" << m_decoderMissingChunkSeq
                     << " to=" << first->first - 1
                     << " timeoutMs=" << m_decoderMissingTimeoutMs
                     << " nowMs=" << now);
      m_decoderDroppedChunks += (first->first - m_nextChunkSeqToDecode);
      m_nextChunkSeqToDecode = first->first;
      m_decoderMissingChunkSeq = UINT64_MAX;
      m_decoderMissingChunkStartMs = 0;

      while (!m_pendingChunks.empty()) {
        auto it = m_pendingChunks.find(m_nextChunkSeqToDecode);
        if (it == m_pendingChunks.end()) {
          m_decoderMissingChunkSeq = m_nextChunkSeqToDecode;
          m_decoderMissingChunkStartMs = nowMs;
          break;
        }
        m_chunkQueue.push_back(std::move(it->second));
        m_pendingChunks.erase(it);
        ++m_nextChunkSeqToDecode;
      }
      if (m_pendingChunks.empty() ||
          (!m_pendingChunks.empty() && m_pendingChunks.begin()->first == m_nextChunkSeqToDecode)) {
        m_decoderMissingChunkSeq = UINT64_MAX;
        m_decoderMissingChunkStartMs = 0;
      }
      m_decoderQueueCv.notify_one();
    }
  }

  bool
  startDecoderProcess(const std::string& command)
  {
    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0) {
      NDN_LOG_WARN("GS_VIDEO_PIPE_ERROR errno=" << errno);
      return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
      NDN_LOG_WARN("GS_VIDEO_DECODER_FORK_FAILED errno=" << errno);
      return false;
    }

    if (pid == 0) {
      dup2(inPipe[0], STDIN_FILENO);
      dup2(outPipe[1], STDOUT_FILENO);
      dup2(outPipe[1], STDERR_FILENO);

      close(inPipe[0]);
      close(inPipe[1]);
      close(outPipe[0]);
      close(outPipe[1]);
      execl("/bin/sh", "/bin/sh", "-c", command.c_str(), (char*)nullptr);
      _exit(1);
    }

    close(inPipe[0]);
    close(outPipe[1]);
    m_decoderPid = pid;
    m_decoderInFd = inPipe[1];
    m_decoderOutFd = outPipe[0];
    return true;
  }

private:
  bool m_serveCertificates;
  int m_ackTimeoutMs;
  int m_timeoutMs;
  std::string m_targetDroneId;
  uint64_t m_videoBitrateKbps = 8000;
  uint64_t m_videoFrameWidth = 480;
  std::vector<std::string> m_patrolDroneIds;
  ndn::Face m_face;
  boost::asio::steady_timer m_videoPumpTimer;
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
  std::atomic<bool> m_videoStartInFlight{false};
  std::atomic<bool> m_videoStopInFlight{false};
  std::atomic<uint64_t> m_firstFrameMs{0};
  std::atomic<uint64_t> m_receivedChunks{0};
  std::atomic<uint64_t> m_frameNacks{0};
  std::atomic<uint64_t> m_frameTimeouts{0};
  ndn::Name m_streamPrefix;
  PacketLane m_keyLane;
  PacketLane m_deltaLane;
  uint64_t m_keyPacketsPerSecond = 16;
  uint64_t m_deltaPacketsPerSecond = 160;
  uint64_t m_keyWindow = 16;
  uint64_t m_deltaWindow = 108;
  uint64_t m_nextChunkSeqToDecode = 0;
  uint64_t m_decoderDroppedChunks = 0;
  uint64_t m_decoderMissingChunkSeq = UINT64_MAX;
  uint64_t m_decoderMissingChunkStartMs = 0;
  uint64_t m_lastOutputChunkSeq = 0;
  uint64_t m_lastOutputChunkElapsedMs = 0;
  static constexpr uint64_t VIDEO_FPS = 30;
  static constexpr uint64_t INITIAL_PACKET_PROBE = 8;
  static constexpr uint64_t VIDEO_PACKET_LOOKAHEAD = 12;
  static constexpr uint64_t PROBE_RETRY_BACKOFF_MS = 80;
  static constexpr uint64_t STREAM_PUMP_INTERVAL_MS = 25;
  static constexpr uint64_t m_decoderReorderWindow = 12;
  static constexpr uint64_t m_decoderMissingTimeoutMs = 80;
  std::atomic<bool> m_done{false};
  std::atomic<bool> m_videoPumpScheduled{false};
  std::mutex m_decoderQueueMutex;
  std::condition_variable m_decoderQueueCv;
  std::deque<StreamChunk> m_chunkQueue;
  std::map<uint64_t, StreamChunk> m_pendingChunks;
  std::map<uint64_t, FecFrameState> m_fecFrames;
  std::vector<uint8_t> m_decoderOutBuffer;
  std::thread m_decoderWriterThread;
  std::thread m_decoderReaderThread;
  std::atomic<bool> m_decoderRunning{false};
  int m_decoderInFd = -1;
  int m_decoderOutFd = -1;
  pid_t m_decoderPid = -1;
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
    set_default_size(920, 700);
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
        for (int i = 0; i < 100 && !m_runtime.isStreaming(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
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
    const bool autoPatrolTest = hasFlag(argc, argv, "--auto-patrol-test");
    const int autoStopSeconds = std::stoi(getOption(argc, argv, "--auto-stop-seconds", "10"));
    const std::string targetDroneId = getOption(argc, argv, "--target-drone", "A");
    auto patrolDroneIds = splitCsv(getOption(argc, argv, "--patrol-drones", targetDroneId));
    const int ackTimeoutMs = std::stoi(getOption(argc, argv, "--ack-timeout-ms", "500"));
    const int timeoutMs = std::stoi(getOption(argc, argv, "--timeout-ms", "10000"));
    const auto videoBitrateKbps = static_cast<uint64_t>(
      std::stoull(getOption(argc, argv, "--video-bitrate-kbps", "8000")));
    const auto videoFrameWidth = static_cast<uint64_t>(
      std::stoull(getOption(argc, argv, "--video-width", "480")));

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
      serveCertificates, ackTimeoutMs, timeoutMs, targetDroneId,
      videoBitrateKbps, videoFrameWidth, patrolDroneIds);
    runtime->start();
    if (!runtime->waitUntilReady(std::chrono::seconds(30))) {
      throw std::runtime_error("ground-station NDNSF runtime did not become ready");
    }
    if (autoPatrolTest) {
      const bool ok = runtime->runAutoPatrolCompensationDemo(std::chrono::seconds(30));
      std::cout << "GS_PATROL_EXIT ok=" << (ok ? "true" : "false") << std::endl;
      return ok ? 0 : 2;
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
