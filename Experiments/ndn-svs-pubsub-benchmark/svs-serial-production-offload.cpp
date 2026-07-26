#include <ndn-svs/svspubsub.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace ndn;
using namespace ndn::svs;
using namespace std::chrono_literals;

namespace {

constexpr uint64_t NS = 1000000000ULL;
constexpr size_t PRODUCTION_QUEUE_CAPACITY = 4096;

uint64_t
nowRaw()
{
  ::timespec value{};
  if (::clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
    throw std::runtime_error("CLOCK_MONOTONIC_RAW failed");
  }
  return static_cast<uint64_t>(value.tv_sec) * NS +
         static_cast<uint64_t>(value.tv_nsec);
}

bool
pinCurrentThread(unsigned cpu)
{
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
}

uint64_t
hostToBe64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return __builtin_bswap64(value);
#else
  return value;
#endif
}

uint64_t
beToHost64(uint64_t value)
{
  return hostToBe64(value);
}

uint64_t
fnv1a(const std::string& value)
{
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::vector<uint8_t>
makePayload(uint64_t logicalId, uint64_t scheduledRawNs, uint8_t phase,
            const std::string& cellId, const std::string& peerId)
{
  std::vector<uint8_t> payload(256);
  const uint8_t magic[8] = {'S', 'V', 'S', '1', '3', '7', 0, 0};
  std::memcpy(payload.data(), magic, sizeof(magic));
  payload[8] = phase;
  const uint64_t idBe = hostToBe64(logicalId);
  const uint64_t timeBe = hostToBe64(scheduledRawNs);
  const uint64_t senderBe = hostToBe64(fnv1a(peerId));
  std::memcpy(payload.data() + 12, &idBe, sizeof(idBe));
  std::memcpy(payload.data() + 20, &timeBe, sizeof(timeBe));
  std::memcpy(payload.data() + 28, &senderBe, sizeof(senderBe));

  uint64_t state = fnv1a(cellId) ^ fnv1a(peerId) ^ logicalId ^ scheduledRawNs;
  for (size_t offset = 36; offset < payload.size(); ++offset) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    payload[offset] = static_cast<uint8_t>(state >> 56);
  }
  return payload;
}

void
highPrecisionWait(std::chrono::steady_clock::time_point deadline)
{
  constexpr auto spin = 50us;
  if (std::chrono::steady_clock::now() + spin < deadline) {
    std::this_thread::sleep_until(deadline - spin);
  }
  while (std::chrono::steady_clock::now() < deadline) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
  }
}

struct Options
{
  std::string productionMode;
  std::string syncPrefix;
  std::string nodePrefix;
  std::string campaignId;
  std::string cellId;
  std::string peerId;
  std::string remotePeerId;
  std::string events;
  std::string resources;
  bool diagnostics = true;
  bool publishEnabled = true;
  unsigned rate = 0;
  unsigned warmup = 0;
  unsigned measure = 0;
  unsigned drain = 0;
  unsigned mainCpu = 0;
  unsigned faceCpu = 0;
  unsigned workerCpu = 0;
};

std::string
runtimeConfig(const Options& options)
{
  const bool worker = options.productionMode == "worker-serial";
  std::ostringstream out;
  out << "{\"production_mode\":\"" << options.productionMode
      << "\",\"parallel_sync_processing\":false"
      << ",\"parallel_sync_production\":" << (worker ? "true" : "false")
      << ",\"face_threads\":1,\"receive_workers\":0"
      << ",\"production_workers\":" << (worker ? 1 : 0)
      << ",\"production_queue_capacity\":"
      << (worker ? PRODUCTION_QUEUE_CAPACITY : 0)
      << ",\"sign_in_worker\":" << (worker ? "true" : "false")
      << ",\"build_extra_in_worker\":" << (worker ? "true" : "false")
      << ",\"worker_cpu_active\":" << (worker ? "true" : "false")
      << ",\"sync_interest_batching\":false"
      << ",\"protocol\":\"v2\",\"sync_security\":\"hmac\""
      << ",\"publication_security\":\"sha256\""
      << ",\"diagnostics_enabled\":"
      << (options.diagnostics ? "true" : "false")
      << ",\"publish_enabled\":"
      << (options.publishEnabled ? "true" : "false")
      << ",\"payload_bytes\":256,\"main_cpu\":" << options.mainCpu
      << ",\"face_cpu\":" << options.faceCpu
      << ",\"worker_cpu\":" << options.workerCpu
      << ",\"rate_pps\":" << options.rate
      << ",\"warmup_s\":" << options.warmup
      << ",\"measure_s\":" << options.measure
      << ",\"drain_s\":" << options.drain << "}";
  return out.str();
}

Options
parse(int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    auto value = [&]() -> std::string {
      if (++i >= argc) {
        throw std::runtime_error("missing value for " + key);
      }
      return argv[i];
    };
    if (key == "--production-mode") options.productionMode = value();
    else if (key == "--sync-prefix") options.syncPrefix = value();
    else if (key == "--node-prefix") options.nodePrefix = value();
    else if (key == "--campaign-id") options.campaignId = value();
    else if (key == "--cell-id") options.cellId = value();
    else if (key == "--peer-id") options.peerId = value();
    else if (key == "--remote-peer-id") options.remotePeerId = value();
    else if (key == "--events") options.events = value();
    else if (key == "--resources") options.resources = value();
    else if (key == "--diagnostics") {
      const auto mode = value();
      if (mode == "enabled") options.diagnostics = true;
      else if (mode == "disabled") options.diagnostics = false;
      else throw std::runtime_error("invalid diagnostics mode: " + mode);
    }
    else if (key == "--publish-enabled") {
      const auto mode = value();
      if (mode == "true") options.publishEnabled = true;
      else if (mode == "false") options.publishEnabled = false;
      else throw std::runtime_error("invalid publish-enabled value: " + mode);
    }
    else if (key == "--rate") options.rate = std::stoul(value());
    else if (key == "--warmup") options.warmup = std::stoul(value());
    else if (key == "--measure") options.measure = std::stoul(value());
    else if (key == "--drain") options.drain = std::stoul(value());
    else if (key == "--main-cpu") options.mainCpu = std::stoul(value());
    else if (key == "--face-cpu") options.faceCpu = std::stoul(value());
    else if (key == "--worker-cpu") options.workerCpu = std::stoul(value());
    else throw std::runtime_error("unknown argument: " + key);
  }

  if ((options.productionMode != "face-serial" &&
       options.productionMode != "worker-serial") ||
      options.syncPrefix.empty() || options.nodePrefix.empty() ||
      options.campaignId.empty() || options.cellId.empty() ||
      options.peerId.empty() || options.remotePeerId.empty() ||
      options.peerId == options.remotePeerId || options.events.empty() ||
      options.resources.empty() || options.rate == 0 || options.measure == 0) {
    throw std::runtime_error("missing or invalid required argument");
  }
  const long cpuCount = ::sysconf(_SC_NPROCESSORS_CONF);
  if (cpuCount <= 0 || options.mainCpu >= static_cast<unsigned>(cpuCount) ||
      options.faceCpu >= static_cast<unsigned>(cpuCount) ||
      options.workerCpu >= static_cast<unsigned>(cpuCount) ||
      (options.productionMode == "worker-serial" &&
       options.workerCpu == options.faceCpu)) {
    throw std::runtime_error("invalid or overlapping CPU assignment");
  }
  return options;
}

class EventSink
{
public:
  explicit
  EventSink(const Options& options)
    : m_options(options)
  {
  }

  void
  add(const std::string& event, const std::string& phase,
      const std::string& role, uint64_t logicalId = 0,
      uint64_t productionId = 0, const std::string& details = "{}")
  {
    std::ostringstream out;
    out << "{\"schema\":\"spec137.event.v1\",\"campaignId\":\""
        << m_options.campaignId << "\",\"cellId\":\"" << m_options.cellId
        << "\",\"peerId\":\"" << m_options.peerId << "\",\"phase\":\""
        << phase << "\",\"event\":\"" << event << "\",\"monotonicNs\":"
        << nowRaw() << ",\"threadRole\":\"" << role << "\",\"logicalId\":"
        << logicalId << ",\"productionId\":" << productionId
        << ",\"details\":" << details << "}";
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lines.push_back(out.str());
  }

  void
  flush(const std::string& path)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("cannot open event output");
    }
    for (const auto& line : m_lines) {
      output << line << '\n';
    }
  }

private:
  const Options& m_options;
  std::mutex m_mutex;
  std::vector<std::string> m_lines;
};

uint64_t
percentile(std::vector<uint64_t> values, unsigned pct)
{
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  const size_t rank = (values.size() * pct + 99) / 100;
  return values[std::max<size_t>(1, rank) - 1];
}

class Peer
{
public:
  explicit
  Peer(Options options)
    : m_options(std::move(options))
    , m_events(m_options)
    , m_heartbeat(m_face.getIoContext())
    , m_remoteSenderHash(fnv1a(m_options.remotePeerId))
  {
    SecurityOptions security(m_keyChain);
    security.interestSigner->signingInfo.setSigningHmacKey(
      "dGhpcyBpcyBhIHNlY3JldCBtZXNzYWdl");
    security.dataSigner->signingInfo.setSha256Signing();

    SVSPubSubOptions pubsubOptions;
    pubsubOptions.useTimestamp = true;
    pubsubOptions.maxPubAge =
      time::seconds(m_options.warmup + m_options.measure + m_options.drain + 30);
    pubsubOptions.syncProtocol.version = SvsProtocolVersion::V2;
    pubsubOptions.syncProtocol.syncInterestLifetime = 1_ms;
    pubsubOptions.syncProtocol.suppressionPeriod = 500_ms;
    pubsubOptions.syncProtocol.periodicTimeout = 30_s;
    pubsubOptions.syncProtocol.periodicJitter = 0.1;

    m_pubsub = std::make_unique<SVSPubSub>(
      Name(m_options.syncPrefix), Name(m_options.nodePrefix), m_face,
      [](const std::vector<MissingDataInfo>&) {}, pubsubOptions, security);
    m_pubsub->subscribe(Name("/spec137/publication"),
                        [this](const auto& data) { onDelivery(data); });
  }

  int
  run()
  {
    const bool mainPinned = pinCurrentThread(m_options.mainCpu);
    if (!mainPinned) {
      throw std::runtime_error("failed to pin main pacer thread");
    }

    std::thread faceThread([this] { runFace(); });
    std::string startupError;
    {
      std::unique_lock<std::mutex> lock(m_readyMutex);
      if (!m_readyCv.wait_for(lock, 5s, [this] { return m_ready; })) {
        startupError = "Face thread did not reach ready barrier";
      }
      else {
        startupError = m_readyError;
      }
    }
    if (!startupError.empty()) {
      m_face.shutdown();
      m_face.getIoContext().stop();
      faceThread.join();
      throw std::runtime_error(startupError);
    }

    std::cout << "SPEC137_READY peer=" << m_options.peerId
              << " mode=" << m_options.productionMode << std::endl;
    m_start = std::chrono::steady_clock::now() + 2s;
    const auto rawOffset = std::chrono::duration_cast<std::chrono::nanoseconds>(
      m_start - std::chrono::steady_clock::now()).count();
    m_rawStart = nowRaw() + std::max<int64_t>(0, rawOffset);
    m_measuredStart = m_start + std::chrono::seconds(m_options.warmup);
    m_measuredEnd = m_measuredStart + std::chrono::seconds(m_options.measure);
    m_stopAt = m_measuredEnd + std::chrono::seconds(m_options.drain);

    boost::asio::post(m_face.getIoContext(), [this] {
      m_events.add("phase-boundary", "warmup", "face");
      m_nextHeartbeat = std::chrono::steady_clock::now() + 1ms;
      armHeartbeat();
    });

    if (m_options.publishEnabled) {
      publishLoop();
    }
    else {
      std::this_thread::sleep_until(m_measuredEnd);
      m_events.add("phase-boundary", "drain", "main");
    }
    std::this_thread::sleep_until(m_stopAt + 250ms);
    boost::asio::post(m_face.getIoContext(), [this] { stopOnFace(); });
    faceThread.join();

    m_events.flush(m_options.events);
    writeResources();
    return m_exitCode;
  }

private:
  std::string
  phaseAt(std::chrono::steady_clock::time_point now) const
  {
    if (now < m_measuredStart) return "warmup";
    if (now < m_measuredEnd) return "measured";
    if (now < m_stopAt) return "drain";
    return "shutdown";
  }

  void
  runFace()
  {
    try {
      if (!pinCurrentThread(m_options.faceCpu)) {
        throw std::runtime_error("failed to pin Face thread");
      }
      auto& core = m_pubsub->getSVSync().getCore();
      core.setParallelSyncProcessing(false);
      core.setSyncInterestBatching(false);
      core.setSyncProductionDiagnostics(m_options.diagnostics, 100);
      if (m_options.productionMode == "worker-serial") {
        core.setParallelSyncProduction(
          true, 1, PRODUCTION_QUEUE_CAPACITY, true, true,
          [this] {
            const bool pinned = pinCurrentThread(m_options.workerCpu);
            m_events.add("worker-thread-config", "startup",
                         "production-worker", 0, 0,
                         std::string("{\"cpu\":") +
                         std::to_string(m_options.workerCpu) +
                         ",\"pinned\":" + (pinned ? "true" : "false") + "}");
            if (!pinned) {
              m_workerPinFailed.store(true, std::memory_order_relaxed);
            }
            {
              std::lock_guard<std::mutex> lock(m_workerReadyMutex);
              m_workerReady = true;
            }
            m_workerReadyCv.notify_all();
          });
        std::unique_lock<std::mutex> lock(m_workerReadyMutex);
        if (!m_workerReadyCv.wait_for(lock, 2s, [this] { return m_workerReady; })) {
          throw std::runtime_error("production worker did not reach ready barrier");
        }
        if (m_workerPinFailed.load(std::memory_order_relaxed)) {
          throw std::runtime_error("failed to pin production worker");
        }
      }
      else {
        core.setParallelSyncProduction(false);
      }
      m_events.add("runtime-config", "startup", "face", 0, 0,
                   runtimeConfig(m_options));
      m_events.add("ready", "startup", "face");
    }
    catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(m_readyMutex);
      m_readyError = error.what();
      m_ready = true;
      m_readyCv.notify_all();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(m_readyMutex);
      m_ready = true;
    }
    m_readyCv.notify_all();
    m_face.processEvents(0_ms, true);
  }

  void
  publishLoop()
  {
    const uint64_t measuredStartIndex =
      static_cast<uint64_t>(m_options.rate) * m_options.warmup;
    const uint64_t terminalIndex =
      static_cast<uint64_t>(m_options.rate) *
      (m_options.warmup + m_options.measure);
    auto countSkipped = [this, measuredStartIndex, terminalIndex]
                        (uint64_t begin, uint64_t end) {
      if (end <= begin) {
        return;
      }
      m_skippedReleases += end - begin;
      const uint64_t measuredBegin = std::max(begin, measuredStartIndex);
      const uint64_t measuredEnd = std::min(end, terminalIndex);
      if (measuredEnd > measuredBegin) {
        m_skippedMeasuredReleases += measuredEnd - measuredBegin;
      }
    };
    uint64_t index = 0;
    while (true) {
      const auto deadline = m_start + std::chrono::nanoseconds(
        index * NS / m_options.rate);
      if (deadline >= m_measuredEnd) {
        break;
      }
      highPrecisionWait(deadline);
      const auto actual = std::chrono::steady_clock::now();
      if (actual >= m_measuredEnd) {
        countSkipped(index, terminalIndex);
        break;
      }
      const uint8_t phaseCode = deadline >= m_measuredStart ? 1 : 0;
      const std::string phase = phaseCode ? "measured" : "warmup";
      const uint64_t logicalId = index + 1;
      const uint64_t scheduledRaw =
        m_rawStart + index * NS / m_options.rate;
      if (phaseCode) {
        ++m_attemptedMeasured;
      }
      auto payload = makePayload(logicalId, scheduledRaw, phaseCode,
                                 m_options.cellId, m_options.peerId);
      Name name("/spec137/publication");
      name.append(m_options.peerId).appendNumber(logicalId);
      try {
        const SeqNo seq = m_pubsub->publishAsync(name, make_span(payload));
        if (phaseCode) {
          ++m_apiReturnedMeasured;
        }
        else {
          ++m_apiReturnedWarmup;
        }
        if (logicalId % 100 == 0) {
          m_events.add("publication-api-return", phase, "main",
                       logicalId, 0,
                       std::string("{\"sequence\":") +
                       std::to_string(seq) +
                       ",\"scheduledNs\":" +
                       std::to_string(scheduledRaw) + "}");
        }
      }
      catch (const std::exception&) {
        ++m_publishErrors;
        if (phaseCode) {
          ++m_publishErrorsMeasured;
        }
        m_events.add("publication-error", phase, "main", logicalId);
      }

      const auto completed = std::chrono::steady_clock::now();
      uint64_t next = index + 1;
      if (completed > m_start) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
          completed - m_start).count();
        const uint64_t currentSlot =
          static_cast<uint64_t>(elapsed) * m_options.rate / NS;
        if (currentSlot + 1 > next) {
          countSkipped(next, std::min(currentSlot + 1, terminalIndex));
          next = currentSlot + 1;
        }
      }
      index = next;
    }
    m_events.add("phase-boundary", "drain", "main");
  }

  void
  armHeartbeat()
  {
    m_heartbeat.expires_at(m_nextHeartbeat);
    m_heartbeat.async_wait([this](const boost::system::error_code& error) {
      if (error || m_stopping) {
        return;
      }
      const auto observed = std::chrono::steady_clock::now();
      const auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        observed - m_nextHeartbeat).count();
      if (phaseAt(observed) == "measured") {
        m_heartbeatDelays.push_back(
          static_cast<uint64_t>(std::max<int64_t>(0, delay)));
      }
      m_nextHeartbeat += 1ms;
      if (observed >= m_nextHeartbeat) {
        const auto late = std::chrono::duration_cast<std::chrono::milliseconds>(
          observed - m_nextHeartbeat).count();
        const uint64_t skipped = static_cast<uint64_t>(late) + 1;
        m_heartbeatSkipped += skipped;
        m_nextHeartbeat += std::chrono::milliseconds(skipped);
      }
      armHeartbeat();
    });
  }

  void
  onDelivery(const SVSPubSub::SubscriptionData& data)
  {
    if (data.data.size() != 256 ||
        std::memcmp(data.data.data(), "SVS137\0\0", 8) != 0) {
      ++m_invalid;
      return;
    }
    uint64_t idBe = 0;
    uint64_t scheduledBe = 0;
    uint64_t senderBe = 0;
    std::memcpy(&idBe, data.data.data() + 12, sizeof(idBe));
    std::memcpy(&scheduledBe, data.data.data() + 20, sizeof(scheduledBe));
    std::memcpy(&senderBe, data.data.data() + 28, sizeof(senderBe));
    const uint64_t logicalId = beToHost64(idBe);
    const uint64_t scheduledRawNs = beToHost64(scheduledBe);
    const uint64_t sender = beToHost64(senderBe);
    if (sender != m_remoteSenderHash) {
      return;
    }
    const bool duplicate = !m_seen.emplace(logicalId, true).second;
    if (duplicate) {
      ++m_duplicates;
      return;
    }
    if (m_lastRemoteLogicalId != 0 && logicalId < m_lastRemoteLogicalId) {
      ++m_outOfOrder;
    }
    m_lastRemoteLogicalId = std::max(m_lastRemoteLogicalId, logicalId);
    if (data.data[8] == 1) {
      ++m_deliveredMeasured;
      const uint64_t observedRawNs = nowRaw();
      const uint64_t deliveryNs =
        observedRawNs >= scheduledRawNs ? observedRawNs - scheduledRawNs : 0;
      m_deliveryDelays.push_back(deliveryNs);
      if (logicalId % 100 == 0) {
        m_events.add("remote-delivery", "measured", "face", logicalId, 0,
          std::string("{\"scheduledNs\":") + std::to_string(scheduledRawNs) +
          ",\"observedNs\":" + std::to_string(observedRawNs) +
          ",\"deliveryNs\":" + std::to_string(deliveryNs) + "}");
      }
    }
  }

  void
  stopOnFace()
  {
    m_stopping = true;
    m_heartbeat.cancel();
    m_events.add("shutdown-start", "shutdown", "face");
    auto& core = m_pubsub->getSVSync().getCore();
    core.setParallelSyncProduction(false);
    m_stats = core.getSyncProcessingStats();
    const uint64_t localCommitted = core.getSeqNo(Name(m_options.nodePrefix));
    const uint64_t committedMeasured =
      localCommitted > m_apiReturnedWarmup ?
        std::min(m_apiReturnedMeasured, localCommitted - m_apiReturnedWarmup) : 0;

    std::ostringstream details;
    details << "{\"triggers\":" << m_stats.syncProductionTriggers
            << ",\"submitted\":" << m_stats.syncProductionJobsSubmitted
            << ",\"serialCompleted\":" << m_stats.syncProductionSerialCompleted
            << ",\"serialFailures\":" << m_stats.syncProductionSerialFailures
            << ",\"completed\":" << m_stats.syncProductionJobsCompleted
            << ",\"staleSent\":" << m_stats.syncProductionStaleSent
            << ",\"staleDropped\":" << m_stats.syncProductionStaleDropped
            << ",\"workerFailures\":" << m_stats.syncProductionWorkerFailures
            << ",\"faceFailures\":" << m_stats.syncProductionFaceFailures
            << ",\"cancelled\":" << m_stats.syncProductionCancelled
            << ",\"pending\":" << m_stats.syncProductionPending
            << ",\"fallbacks\":" << m_stats.syncProductionFallbacks
            << ",\"activeSigners\":" << m_stats.syncProductionActiveSigners
            << ",\"maxActiveSigners\":" << m_stats.syncProductionMaxActiveSigners
            << ",\"ownerViolations\":"
            << m_stats.syncProductionThreadOwnerViolations
            << ",\"workerThreadChanges\":"
            << m_stats.syncProductionWorkerThreadChanges
            << ",\"maxWorkerQueueDepth\":"
            << m_stats.syncProductionMaxWorkerQueueDepth
            << ",\"snapshotNs\":" << m_stats.syncProductionSnapshotNs
            << ",\"queueWaitNs\":" << m_stats.syncProductionQueueWaitNs
            << ",\"extraBuildNs\":" << m_stats.syncProductionExtraBuildNs
            << ",\"encodeNs\":" << m_stats.syncProductionEncodeNs
            << ",\"signNs\":" << m_stats.syncProductionSignNs
            << ",\"workerServiceNs\":" << m_stats.syncProductionWorkerServiceNs
            << ",\"faceQueueWaitNs\":" << m_stats.syncProductionFaceQueueWaitNs
            << ",\"faceFinalizeNs\":" << m_stats.syncProductionFaceFinalizeNs
            << ",\"faceCpuNs\":" << m_stats.syncProductionFaceCpuNs
            << ",\"workerCpuNs\":" << m_stats.syncProductionWorkerCpuNs
            << ",\"serialCpuNs\":" << m_stats.syncProductionSerialCpuNs << "}";
    m_events.add("worker-stats", "shutdown", "face", 0, 0, details.str());
    m_events.add("signer-concurrency-max", "shutdown", "face", 0, 0,
      std::string("{\"value\":") +
      std::to_string(m_stats.syncProductionMaxActiveSigners) + "}");
    if (m_stats.syncProductionFallbacks != 0) {
      m_events.add("production-fallback", "shutdown", "face", 0, 0,
        std::string("{\"count\":") +
        std::to_string(m_stats.syncProductionFallbacks) + "}");
    }

    const uint64_t scheduled = m_options.publishEnabled ?
      static_cast<uint64_t>(m_options.rate) * m_options.measure : 0;
    std::ostringstream summary;
    summary << "{\"scheduledMeasured\":" << scheduled
            << ",\"attemptedMeasured\":" << m_attemptedMeasured
            << ",\"apiReturnedMeasured\":" << m_apiReturnedMeasured
            << ",\"committedMeasured\":" << committedMeasured
            << ",\"advertisedMeasured\":" << committedMeasured
            << ",\"skippedReleases\":" << m_skippedReleases
            << ",\"skippedMeasuredReleases\":"
            << m_skippedMeasuredReleases
            << ",\"deliveredMeasured\":" << m_deliveredMeasured
            << ",\"publishErrors\":" << m_publishErrors
            << ",\"publishErrorsMeasured\":" << m_publishErrorsMeasured
            << ",\"invalid\":" << m_invalid
            << ",\"duplicates\":" << m_duplicates
            << ",\"outOfOrder\":" << m_outOfOrder
            << ",\"deliveryCount\":" << m_deliveryDelays.size()
            << ",\"deliveryP50Ns\":" << percentile(m_deliveryDelays, 50)
            << ",\"deliveryP95Ns\":" << percentile(m_deliveryDelays, 95)
            << ",\"deliveryP99Ns\":" << percentile(m_deliveryDelays, 99)
            << ",\"deliveryMaxNs\":"
            << (m_deliveryDelays.empty() ? 0 :
                *std::max_element(m_deliveryDelays.begin(), m_deliveryDelays.end()))
            << ",\"heartbeatCount\":" << m_heartbeatDelays.size()
            << ",\"heartbeatP50Ns\":" << percentile(m_heartbeatDelays, 50)
            << ",\"heartbeatP95Ns\":" << percentile(m_heartbeatDelays, 95)
            << ",\"heartbeatP99Ns\":" << percentile(m_heartbeatDelays, 99)
            << ",\"heartbeatMaxNs\":"
            << (m_heartbeatDelays.empty() ? 0 :
                *std::max_element(m_heartbeatDelays.begin(), m_heartbeatDelays.end()))
            << ",\"heartbeatSkipped\":" << m_heartbeatSkipped
            << ",\"workerPinFailed\":"
            << (m_workerPinFailed.load(std::memory_order_relaxed) ? "true" : "false")
            << "}";
    m_events.add("process-summary", "shutdown", "face", 0, 0, summary.str());

    const uint64_t workerTerminals =
      m_stats.syncProductionJobsCompleted +
      m_stats.syncProductionStaleDropped +
      m_stats.syncProductionWorkerFailures +
      m_stats.syncProductionFaceFailures +
      m_stats.syncProductionCancelled;
    const bool faceAccountingBad =
      m_options.productionMode == "face-serial" &&
      m_stats.syncProductionTriggers !=
        m_stats.syncProductionSerialCompleted +
        m_stats.syncProductionSerialFailures;
    const bool workerAccountingBad =
      m_options.productionMode == "worker-serial" &&
      m_stats.syncProductionJobsSubmitted != workerTerminals;
    const bool bad = m_publishErrors != 0 ||
      m_stats.syncProductionActiveSigners != 0 ||
      m_stats.syncProductionMaxActiveSigners > 1 ||
      m_stats.syncProductionThreadOwnerViolations != 0 ||
      m_stats.syncProductionPending != 0 ||
      m_stats.syncProductionSerialFailures != 0 ||
      m_stats.syncProductionWorkerFailures != 0 ||
      m_stats.syncProductionFaceFailures != 0 ||
      m_stats.syncProductionCancelled != 0 ||
      faceAccountingBad || workerAccountingBad ||
      (m_options.productionMode == "worker-serial" &&
       (m_stats.syncProductionFallbacks != 0 ||
        m_workerPinFailed.load(std::memory_order_relaxed)));
    if (bad) {
      m_events.add("production-terminal-anomaly", "shutdown", "face");
      m_exitCode = 3;
    }
    m_events.add("shutdown-complete", "shutdown", "face");
    m_face.shutdown();
    m_face.getIoContext().stop();
  }

  void
  writeResources()
  {
    ::rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
      throw std::runtime_error("getrusage failed");
    }
    std::ofstream output(m_options.resources, std::ios::out | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("cannot open resource output");
    }
    output << "{\"schema\":\"spec137.resources.v1\",\"campaignId\":\""
           << m_options.campaignId << "\",\"cellId\":\"" << m_options.cellId
           << "\",\"peerId\":\"" << m_options.peerId << "\",\"maxRssKiB\":"
           << usage.ru_maxrss << ",\"voluntaryContextSwitches\":" << usage.ru_nvcsw
           << ",\"involuntaryContextSwitches\":" << usage.ru_nivcsw << "}\n";
  }

private:
  Options m_options;
  EventSink m_events;
  Face m_face;
  KeyChain m_keyChain;
  std::unique_ptr<SVSPubSub> m_pubsub;
  boost::asio::steady_timer m_heartbeat;
  std::mutex m_readyMutex;
  std::condition_variable m_readyCv;
  bool m_ready = false;
  std::string m_readyError;
  std::mutex m_workerReadyMutex;
  std::condition_variable m_workerReadyCv;
  bool m_workerReady = false;
  std::atomic<bool> m_workerPinFailed{false};
  std::map<uint64_t, bool> m_seen;
  uint64_t m_remoteSenderHash = 0;
  uint64_t m_lastRemoteLogicalId = 0;
  uint64_t m_rawStart = 0;
  uint64_t m_attemptedMeasured = 0;
  uint64_t m_apiReturnedWarmup = 0;
  uint64_t m_apiReturnedMeasured = 0;
  uint64_t m_skippedReleases = 0;
  uint64_t m_skippedMeasuredReleases = 0;
  uint64_t m_deliveredMeasured = 0;
  uint64_t m_publishErrors = 0;
  uint64_t m_publishErrorsMeasured = 0;
  uint64_t m_invalid = 0;
  uint64_t m_duplicates = 0;
  uint64_t m_outOfOrder = 0;
  uint64_t m_heartbeatSkipped = 0;
  std::vector<uint64_t> m_heartbeatDelays;
  std::vector<uint64_t> m_deliveryDelays;
  std::chrono::steady_clock::time_point m_start;
  std::chrono::steady_clock::time_point m_measuredStart;
  std::chrono::steady_clock::time_point m_measuredEnd;
  std::chrono::steady_clock::time_point m_stopAt;
  std::chrono::steady_clock::time_point m_nextHeartbeat;
  SVSyncCore::SyncProcessingStats m_stats;
  bool m_stopping = false;
  int m_exitCode = 0;
};

} // namespace

int
main(int argc, char** argv)
{
  try {
    if (argc == 3 && std::string(argv[1]) == "--self-test") {
      Options options;
      options.productionMode = argv[2];
      options.rate = 200;
      options.warmup = 10;
      options.measure = 60;
      options.drain = 10;
      options.mainCpu = 0;
      options.faceCpu = 1;
      options.workerCpu = 2;
      if (options.productionMode != "face-serial" &&
          options.productionMode != "worker-serial") {
        throw std::runtime_error("invalid self-test mode");
      }
      std::cout << runtimeConfig(options) << std::endl;
      return 0;
    }
    return Peer(parse(argc, argv)).run();
  }
  catch (const std::exception& error) {
    std::cerr << "SPEC137_ERROR " << error.what() << std::endl;
    return 2;
  }
}
