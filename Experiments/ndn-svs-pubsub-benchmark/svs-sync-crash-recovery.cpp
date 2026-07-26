#include <ndn-svs/svspubsub.hpp>

#include <boost/asio/io_context.hpp>

#include <pthread.h>
#include <sched.h>
#include <time.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace ndn;
using namespace ndn::svs;

namespace {

constexpr uint64_t NS = 1000000000ULL;
constexpr size_t PAYLOAD_SIZE = 256;
constexpr char MAGIC[] = "SVS134";

uint64_t
nowRaw()
{
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) != 0)
    throw std::runtime_error("CLOCK_MONOTONIC_RAW failed");
  return uint64_t(now.tv_sec) * NS + uint64_t(now.tv_nsec);
}

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
  if (std::chrono::steady_clock::now() < sleepDeadline)
    std::this_thread::sleep_until(sleepDeadline);
  while (std::chrono::steady_clock::now() < deadline) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
  }
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
makePayload(uint64_t logicalId, bool measured, const std::string& cellId,
            const std::string& peerId)
{
  std::vector<uint8_t> payload(PAYLOAD_SIZE);
  std::memcpy(payload.data(), MAGIC, sizeof(MAGIC));
  payload[8] = measured ? 1 : 0;
  const uint64_t id = hostToBe64(logicalId);
  const uint64_t sender = hostToBe64(fnv1a(peerId));
  std::memcpy(payload.data() + 16, &id, sizeof(id));
  std::memcpy(payload.data() + 24, &sender, sizeof(sender));
  uint64_t state = fnv1a(cellId) ^ fnv1a(peerId) ^ logicalId;
  for (size_t i = 32; i < payload.size(); ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    payload[i] = static_cast<uint8_t>(state >> 56);
  }
  return payload;
}

struct Options
{
  std::string syncPrefix;
  std::string nodePrefix;
  std::string cellId;
  std::string peerId;
  std::string remotePeerId;
  std::string events;
  unsigned rate = 1000;
  unsigned warmup = 1;
  unsigned measure = 5;
  unsigned drain = 2;
  unsigned startDelayMs = 2000;
  unsigned mainCpu = 0;
  unsigned faceCpu = 1;
};

Options
parse(int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    auto value = [&]() -> std::string {
      if (++i >= argc)
        throw std::runtime_error("missing value for " + key);
      return argv[i];
    };
    if (key == "--sync-prefix") options.syncPrefix = value();
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
  if (options.syncPrefix.empty() || options.nodePrefix.empty() ||
      options.cellId.empty() || options.peerId.empty() ||
      options.remotePeerId.empty() || options.events.empty() ||
      options.peerId == options.remotePeerId || options.rate == 0 ||
      options.measure == 0) {
    throw std::runtime_error("missing or invalid required argument");
  }
  return options;
}

class Journal
{
public:
  Journal(const std::string& path, const std::string& cellId,
          const std::string& peerId)
    : m_output(path, std::ios::out | std::ios::trunc)
    , m_cellId(cellId)
    , m_peerId(peerId)
  {
    if (!m_output)
      throw std::runtime_error("cannot open event journal");
  }

  void
  record(const std::string& event, const std::string& details = "{}")
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_output << "{\"schemaVersion\":\"spec134-lifecycle-v1\",\"cellId\":\""
             << m_cellId << "\",\"peerId\":\"" << m_peerId
             << "\",\"event\":\"" << event << "\",\"monotonicRawNs\":"
             << nowRaw() << ",\"details\":" << details << "}\n";
    m_output.flush();
  }

private:
  std::ofstream m_output;
  std::string m_cellId;
  std::string m_peerId;
  std::mutex m_mutex;
};

class Peer
{
public:
  explicit Peer(Options options)
    : m_options(std::move(options))
    , m_journal(m_options.events, m_options.cellId, m_options.peerId)
  {
    SecurityOptions security(m_keyChain);
    security.interestSigner->signingInfo.setSigningHmacKey(
      "dGhpcyBpcyBhIHNlY3JldCBtZXNzYWdl");
    security.dataSigner->signingInfo.setSha256Signing();
    SVSPubSubOptions pubsubOptions;
    pubsubOptions.useTimestamp = true;
    pubsubOptions.maxPubAge = time::seconds(
      m_options.warmup + m_options.measure + m_options.drain + 30);
    m_pubsub = std::make_unique<SVSPubSub>(
      Name(m_options.syncPrefix), Name(m_options.nodePrefix), m_face,
      [](const std::vector<MissingDataInfo>&) {}, pubsubOptions, security);
    m_pubsub->subscribe(Name("/spec134/publication"),
      [this](const auto& data) { onDelivery(data); });
  }

  int
  run()
  {
    m_journal.record("process-start");
    std::thread faceThread([this] {
      const bool pinned = pinCurrentThread(m_options.faceCpu);
      m_journal.record("face-thread-start",
        std::string("{\"pinned\":") + (pinned ? "true" : "false") + "}");
      m_face.processEvents(0_ms, true);
      m_journal.record("face-thread-stop");
    });
    std::cout << "SPEC134_READY peer=" << m_options.peerId << std::endl;
    const bool pinned = pinCurrentThread(m_options.mainCpu);
    const bool realtime = enableLowRealtimePriority();
    m_journal.record("main-thread-config",
      std::string("{\"pinned\":") + (pinned ? "true" : "false") +
      ",\"schedFifoPriority1\":" + (realtime ? "true" : "false") + "}");

    const auto start = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(m_options.startDelayMs);
    const auto measuredStart = start + std::chrono::seconds(m_options.warmup);
    const auto end = measuredStart + std::chrono::seconds(m_options.measure);
    const uint64_t periodNs = NS / m_options.rate;
    uint64_t logicalId = 0;
    uint64_t lastHeartbeatSecond = 0;
    while (true) {
      const auto deadline = start + std::chrono::nanoseconds(logicalId * periodNs);
      if (deadline >= end)
        break;
      highPrecisionWait(deadline);
      const auto actual = std::chrono::steady_clock::now();
      if (actual >= end)
        break;
      ++logicalId;
      const bool measured = actual >= measuredStart;
      if (measured)
        ++m_attemptedMeasured;
      auto payload = makePayload(logicalId, measured, m_options.cellId, m_options.peerId);
      Name name("/spec134/publication");
      name.append(m_options.peerId).appendNumber(logicalId);
      try {
        m_pubsub->publish(name, make_span(payload));
      }
      catch (const std::exception& error) {
        ++m_publishErrors;
        std::cerr << "publish error logicalId=" << logicalId
                  << " error=" << error.what() << '\n';
      }
      const uint64_t elapsedSecond = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(actual - start).count());
      if (elapsedSecond > lastHeartbeatSecond) {
        lastHeartbeatSecond = elapsedSecond;
        m_journal.record("heartbeat", countersJson());
      }
    }

    m_journal.record("publish-loop-stop", countersJson());
    std::this_thread::sleep_for(std::chrono::seconds(m_options.drain + 2));
    m_face.shutdown();
    m_face.getIoContext().stop();
    faceThread.join();
    m_journal.record("process-stop", countersJson());
    return 0;
  }

private:
  void
  onDelivery(const SVSPubSub::SubscriptionData& data)
  {
    if (data.data.size() != PAYLOAD_SIZE ||
        std::memcmp(data.data.data(), MAGIC, sizeof(MAGIC)) != 0) {
      ++m_invalidMeasured;
      return;
    }
    uint64_t senderBe = 0;
    std::memcpy(&senderBe, data.data.data() + 24, sizeof(senderBe));
    const bool validSender = hostToBe64(senderBe) == fnv1a(m_options.remotePeerId);
    if (!validSender) {
      ++m_invalidMeasured;
      return;
    }
    if (data.data[8] == 1)
      ++m_deliveredMeasured;
  }

  std::string
  countersJson() const
  {
    std::ostringstream output;
    output << "{\"attemptedMeasured\":" << m_attemptedMeasured.load()
           << ",\"deliveredMeasured\":" << m_deliveredMeasured.load()
           << ",\"invalidMeasured\":" << m_invalidMeasured.load()
           << ",\"publishErrors\":" << m_publishErrors.load() << "}";
    return output.str();
  }

private:
  Options m_options;
  Journal m_journal;
  Face m_face;
  KeyChain m_keyChain;
  std::unique_ptr<SVSPubSub> m_pubsub;
  std::atomic<uint64_t> m_attemptedMeasured{0};
  std::atomic<uint64_t> m_deliveredMeasured{0};
  std::atomic<uint64_t> m_invalidMeasured{0};
  std::atomic<uint64_t> m_publishErrors{0};
};

} // namespace

int
main(int argc, char** argv)
{
  try {
    return Peer(parse(argc, argv)).run();
  }
  catch (const std::exception& error) {
    std::cerr << "SPEC134_ERROR " << error.what() << std::endl;
    return 2;
  }
}
