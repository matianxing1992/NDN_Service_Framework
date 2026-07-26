#include <ndn-svs/svspubsub.hpp>

#include <arpa/inet.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef SPEC132_LATEST
#define SPEC132_LATEST 0
#endif

using namespace ndn;
using namespace ndn::svs;

namespace {

constexpr const char* SUBJECT = SPEC132_LATEST ?
  "async-publish-parallel-sync" : "sync-publish-no-internal-parallelism";
constexpr uint64_t NS = 1000000000ULL;

bool
pinCurrentThread(unsigned cpu)
{
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

bool
enableLowRealtimePriority()
{
  sched_param parameter{};
  parameter.sched_priority = 1;
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameter) == 0;
}

void
highPrecisionWait(std::chrono::steady_clock::time_point deadline)
{
  constexpr auto spin = std::chrono::microseconds(50);
  const auto sleepDeadline = deadline - spin;
  if (std::chrono::steady_clock::now() < sleepDeadline) {
    std::this_thread::sleep_until(sleepDeadline);
  }
  while (std::chrono::steady_clock::now() < deadline) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
  }
}

uint64_t
nowRaw()
{
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return uint64_t(ts.tv_sec) * NS + uint64_t(ts.tv_nsec);
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
makePayload(uint64_t logicalId, uint64_t scheduled, uint8_t phase,
            const std::string& cellId, const std::string& peerId)
{
  std::vector<uint8_t> payload(256);
  const uint8_t magic[8] = {'S', 'V', 'S', '1', '3', '2', 0, 0};
  std::memcpy(payload.data(), magic, sizeof(magic));
  payload[8] = phase;
  const uint64_t idBe = hostToBe64(logicalId);
  const uint64_t timeBe = hostToBe64(scheduled);
  const uint64_t senderBe = hostToBe64(fnv1a(peerId));
  std::memcpy(payload.data() + 12, &idBe, sizeof(idBe));
  std::memcpy(payload.data() + 20, &timeBe, sizeof(timeBe));
  std::memcpy(payload.data() + 28, &senderBe, sizeof(senderBe));

  uint64_t state = fnv1a(cellId) ^ fnv1a(peerId) ^ logicalId ^ scheduled;
  for (size_t offset = 36; offset < payload.size(); ++offset) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    payload[offset] = static_cast<uint8_t>(state >> 56);
  }
  return payload;
}

struct Options {
  std::string subject;
  std::string syncPrefix;
  std::string nodePrefix;
  std::string cellId;
  std::string peerId;
  std::string remotePeerId;
  std::string events;
  unsigned rate = 200;
  unsigned warmup = 10;
  unsigned measure = 60;
  unsigned drain = 10;
  unsigned startDelayMs = 1000;
  unsigned mainCpu = 0;
  unsigned faceCpu = 1;
};

Options
parse(int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string key = argv[i];
    auto value = [&]() -> std::string {
      if (++i >= argc) {
        throw std::runtime_error("missing value for " + key);
      }
      return argv[i];
    };
    if (key == "--subject") options.subject = value();
    else if (key == "--sync-prefix") options.syncPrefix = value();
    else if (key == "--node-prefix") options.nodePrefix = value();
    else if (key == "--cell-id") options.cellId = value();
    else if (key == "--peer-id") options.peerId = value();
    else if (key == "--remote-peer-id") options.remotePeerId = value();
    else if (key == "--events") options.events = value();
    else if (key == "--rate-pps") options.rate = std::stoul(value());
    else if (key == "--warmup-s") options.warmup = std::stoul(value());
    else if (key == "--measure-s") options.measure = std::stoul(value());
    else if (key == "--drain-s") options.drain = std::stoul(value());
    else if (key == "--start-delay-ms") options.startDelayMs = std::stoul(value());
    else if (key == "--main-cpu") options.mainCpu = std::stoul(value());
    else if (key == "--face-cpu") options.faceCpu = std::stoul(value());
    else throw std::runtime_error("unknown argument: " + key);
  }
  if (options.subject != SUBJECT || options.syncPrefix.empty() ||
      options.nodePrefix.empty() || options.cellId.empty() ||
      options.peerId.empty() || options.remotePeerId.empty() ||
      options.peerId == options.remotePeerId || options.events.empty() ||
      options.rate == 0 || options.measure == 0) {
    throw std::runtime_error("missing or mismatched required arguments");
  }
  return options;
}

struct EventSink {
  std::vector<std::string> lines;
  std::string cellId;
  std::string peerId;

  void
  reserve(size_t count)
  {
    lines.reserve(count);
  }

  void
  add(const std::string& event, uint64_t logicalId, uint64_t seqNo,
      const std::string& phase, uint64_t timestamp,
      const std::string& details = "{}")
  {
    std::ostringstream output;
    output << "{\"schemaVersion\":\"spec132-event-v1\",\"cellId\":\"" << cellId
           << "\",\"peerId\":\"" << peerId << "\",\"event\":\"" << event
           << "\",\"logicalId\":" << logicalId << ",\"svsSeqNo\":" << seqNo
           << ",\"phase\":\"" << phase << "\",\"monotonicRawNs\":" << timestamp
           << ",\"details\":" << details << "}";
    lines.push_back(output.str());
  }
};

class Peer {
public:
  explicit Peer(Options options)
    : m_options(std::move(options))
    , m_publishEvents{{}, m_options.cellId, m_options.peerId}
    , m_faceEvents{{}, m_options.cellId, m_options.peerId}
  {
    const size_t planned = size_t(m_options.rate) *
                           (m_options.warmup + m_options.measure);
    m_publishEvents.reserve(planned * 3 + 4);
    m_faceEvents.reserve(planned * 2 + 16);

    SecurityOptions security(m_keyChain);
    security.interestSigner->signingInfo.setSigningHmacKey(
      "dGhpcyBpcyBhIHNlY3JldCBtZXNzYWdl");
    security.dataSigner->signingInfo.setSha256Signing();

    SVSPubSubOptions pubsubOptions;
    pubsubOptions.useTimestamp = true;
    pubsubOptions.maxPubAge = time::seconds(
      m_options.warmup + m_options.measure + m_options.drain + 30);
#if SPEC132_LATEST
    pubsubOptions.syncProtocol.version = SvsProtocolVersion::V2;
    pubsubOptions.syncProtocol.syncInterestLifetime = 1_ms;
    pubsubOptions.syncProtocol.suppressionPeriod = 500_ms;
    pubsubOptions.syncProtocol.periodicTimeout = 30_s;
    pubsubOptions.syncProtocol.periodicJitter = 0.1;
#endif

    m_pubsub = std::make_unique<SVSPubSub>(
      Name(m_options.syncPrefix), Name(m_options.nodePrefix), m_face,
      [this](const std::vector<MissingDataInfo>& info) { onUpdate(info); },
      pubsubOptions, security);

#if SPEC132_LATEST
    auto& core = m_pubsub->getSVSync().getCore();
    core.setParallelSyncProcessing(true, 4, 4096);
    core.setParallelSyncProduction(true, 4, 4096, true, true);
    core.setSyncInterestBatching(false);
#endif

    m_pubsub->subscribe(Name("/spec132/publication"),
                        [this](const auto& data) { onDelivery(data); });
  }

  int
  run()
  {
    std::thread faceThread([this] {
      const bool pinned = pinCurrentThread(m_options.faceCpu);
      m_faceEvents.add("face-thread-config", 0, 0, "all", nowRaw(),
        std::string("{\"cpu\":") + std::to_string(m_options.faceCpu) +
        ",\"pinned\":" + (pinned ? "true" : "false") + "}");
      m_face.processEvents(0_ms, true);
    });

    std::cout << "SPEC132_READY peer=" << m_options.peerId
              << " subject=" << SUBJECT << std::endl;

    const auto start = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(m_options.startDelayMs);
    const auto untilStart = std::chrono::duration_cast<std::chrono::nanoseconds>(
      start - std::chrono::steady_clock::now()).count();
    const uint64_t rawStart = nowRaw() + std::max<int64_t>(0, untilStart);

    // This call remains on the application main thread. It directly invokes
    // the public API selected at compile time; there is no harness adapter.
    publishLoop(start, rawStart);
    std::this_thread::sleep_for(std::chrono::seconds(m_options.drain + 2));

    m_face.shutdown();
    m_face.getIoContext().stop();
    faceThread.join();
    emitStats();
    flushEvents();
    return 0;
  }

private:
  void
  publishLoop(std::chrono::steady_clock::time_point start, uint64_t rawStart)
  {
    const bool pinned = pinCurrentThread(m_options.mainCpu);
    const bool realtime = enableLowRealtimePriority();
    m_publishEvents.add("main-thread-config", 0, 0, "all", nowRaw(),
      std::string("{\"cpu\":") + std::to_string(m_options.mainCpu) +
      ",\"pinned\":" + (pinned ? "true" : "false") +
      ",\"schedFifoPriority1\":" + (realtime ? "true" : "false") + "}");

    const uint64_t periodNs = NS / m_options.rate;
    const auto measuredStart = start + std::chrono::seconds(m_options.warmup);
    const auto end = measuredStart + std::chrono::seconds(m_options.measure);
    uint64_t index = 0;

    while (true) {
      const auto deadline = start + std::chrono::nanoseconds(index * periodNs);
      if (deadline >= end) {
        break;
      }
      highPrecisionWait(deadline);
      const auto actual = std::chrono::steady_clock::now();
      if (actual >= end) {
        break;
      }

      const uint64_t wakeRaw = nowRaw();
      const uint64_t scheduled = rawStart + index * periodNs;
      const uint8_t phaseCode = actual >= measuredStart ? 1 : 0;
      const std::string phase = phaseCode ? "measured" : "warmup";
      const uint64_t logicalId = index + 1;
      m_publishEvents.add("deadline", logicalId, 0, phase, scheduled,
        std::string("{\"actualWakeNs\":") + std::to_string(wakeRaw) + "}");

      auto payload = makePayload(logicalId, scheduled, phaseCode,
                                 m_options.cellId, m_options.peerId);
      Name name("/spec132/publication");
      name.append(m_options.peerId).appendNumber(logicalId);
      m_publishEvents.add("api-enter", logicalId, 0, phase, nowRaw());
      try {
        SeqNo seqNo;
#if SPEC132_LATEST
        seqNo = m_pubsub->publishAsync(name, make_span(payload));
#else
        seqNo = m_pubsub->publish(name, make_span(payload));
#endif
        m_publishEvents.add("api-return", logicalId, seqNo, phase, nowRaw());
      }
      catch (const std::exception& error) {
        m_publishEvents.add("api-error", logicalId, 0, phase, nowRaw(),
                            "{\"reason\":\"publication-exception\"}");
        std::cerr << "publication exception peer=" << m_options.peerId
                  << " logicalId=" << logicalId << " error=" << error.what() << '\n';
      }
      ++index;
    }
  }

  void
  onUpdate(const std::vector<MissingDataInfo>& info)
  {
    const uint64_t timestamp = nowRaw();
    for (const auto& item : info) {
      if (item.nodeId.toUri().find(m_options.remotePeerId) == std::string::npos) {
        continue;
      }
      for (uint64_t seqNo = item.low; seqNo <= item.high; ++seqNo) {
        m_faceEvents.add("state-update", 0, seqNo, "unknown", timestamp,
          std::string("{\"senderPeer\":\"") + m_options.remotePeerId + "\"}");
      }
    }
  }

  void
  onDelivery(const SVSPubSub::SubscriptionData& data)
  {
    const uint64_t timestamp = nowRaw();
    if (data.data.size() != 256 ||
        std::memcmp(data.data.data(), "SVS132\0\0", 8) != 0) {
      m_faceEvents.add("invalid", 0, data.seqNo, "unknown", timestamp,
                       "{\"reason\":\"payload-shape\"}");
      return;
    }

    uint64_t idBe = 0;
    uint64_t scheduledBe = 0;
    uint64_t senderBe = 0;
    std::memcpy(&idBe, data.data.data() + 12, sizeof(idBe));
    std::memcpy(&scheduledBe, data.data.data() + 20, sizeof(scheduledBe));
    std::memcpy(&senderBe, data.data.data() + 28, sizeof(senderBe));
    const uint64_t logicalId = beToHost64(idBe);
    const uint64_t scheduled = beToHost64(scheduledBe);
    const uint64_t senderHash = beToHost64(senderBe);
    const uint8_t phaseCode = data.data[8];
    const std::string phase = phaseCode ? "measured" : "warmup";
    const bool senderValid = senderHash == fnv1a(m_options.remotePeerId);
    const auto expected = makePayload(logicalId, scheduled, phaseCode,
                                      m_options.cellId, m_options.remotePeerId);
    const bool payloadValid = senderValid &&
      std::equal(expected.begin(), expected.end(), data.data.begin());
    const bool duplicate = !m_seen.emplace(logicalId, true).second;

    std::ostringstream details;
    details << "{\"senderPeer\":\"" << m_options.remotePeerId
            << "\",\"scheduledNs\":" << scheduled
            << ",\"payloadValid\":" << (payloadValid ? "true" : "false") << "}";
    m_faceEvents.add(duplicate ? "duplicate" : "delivery", logicalId,
                     data.seqNo, phase, timestamp, details.str());
  }

  void
  emitStats()
  {
#if SPEC132_LATEST
    const auto stats = m_pubsub->getSVSync().getCore().getSyncProcessingStats();
    std::ostringstream details;
    details << "{\"syncJobsSubmitted\":" << stats.syncJobsSubmitted
            << ",\"syncJobsCompleted\":" << stats.syncJobsCompleted
            << ",\"syncJobsDropped\":" << stats.syncJobsDropped
            << ",\"syncJobsStale\":" << stats.syncJobsStale
            << ",\"syncWorkerQueueDepth\":" << stats.syncWorkerQueueDepth
            << ",\"syncWorkerProcessingMs\":" << stats.syncWorkerProcessingMs
            << ",\"syncMainThreadPublishMs\":" << stats.syncMainThreadPublishMs
            << ",\"syncProductionJobsSubmitted\":" << stats.syncProductionJobsSubmitted
            << ",\"syncProductionJobsCompleted\":" << stats.syncProductionJobsCompleted
            << ",\"syncProductionJobsDropped\":" << stats.syncProductionJobsDropped
            << ",\"syncProductionJobsStale\":" << stats.syncProductionJobsStale
            << ",\"syncProductionWorkerQueueDepth\":"
            << stats.syncProductionWorkerQueueDepth << "}";
    m_faceEvents.add("worker-stats", 0, 0, "all", nowRaw(), details.str());
#else
    m_faceEvents.add("worker-stats", 0, 0, "all", nowRaw(), "null");
#endif
  }

  void
  flushEvents()
  {
    std::ofstream output(m_options.events);
    for (const auto& line : m_publishEvents.lines) {
      output << line << '\n';
    }
    for (const auto& line : m_faceEvents.lines) {
      output << line << '\n';
    }
  }

private:
  Options m_options;
  EventSink m_publishEvents;
  EventSink m_faceEvents;
  Face m_face;
  KeyChain m_keyChain;
  std::unique_ptr<SVSPubSub> m_pubsub;
  std::map<uint64_t, bool> m_seen;
};

} // namespace

int
main(int argc, char** argv)
{
  try {
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
      const auto payload = makePayload(7, 11, 1, "self-test", "peer-a");
      if (payload.size() != 256 ||
          std::memcmp(payload.data(), "SVS132\0\0", 8) != 0) {
        return 2;
      }
      std::cout << "SPEC132_SELF_TEST_OK subject=" << SUBJECT << std::endl;
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--clock-probe") {
      std::cout << nowRaw() << std::endl;
      return 0;
    }
    return Peer(parse(argc, argv)).run();
  }
  catch (const std::exception& error) {
    std::cerr << "SPEC132_ERROR " << error.what() << std::endl;
    return 2;
  }
}

