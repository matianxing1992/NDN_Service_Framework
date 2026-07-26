#include <ndn-svs/svspubsub.hpp>

#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <pthread.h>
#include <sched.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ndn;
using namespace ndn::svs;

namespace {

constexpr uint64_t NS = 1000000000ULL;
constexpr size_t PAYLOAD_SIZE = 256;
constexpr char MAGIC[] = "SVS134IO";

uint64_t
nowRaw()
{
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) != 0) {
    throw std::runtime_error("CLOCK_MONOTONIC_RAW failed");
  }
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
  payload[9] = measured ? 1 : 0;
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
  unsigned warmup = 10;
  unsigned measure = 60;
  unsigned drain = 10;
  unsigned startDelayMs = 2000;
  unsigned ioCpu = 0;
};

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
    else if (key == "--io-cpu") options.ioCpu = std::stoul(value());
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
    if (!m_output) {
      throw std::runtime_error("cannot open event journal");
    }
  }

  void
  record(const std::string& event, const std::string& details = "{}")
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_output << "{\"schemaVersion\":\"spec134-io-lifecycle-v1\",\"cellId\":\""
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
    , m_releaseTimer(m_face.getIoContext())
    , m_ownSenderHash(fnv1a(m_options.peerId))
    , m_remoteSenderHash(fnv1a(m_options.remotePeerId))
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
    m_pubsub->subscribe(Name("/spec134/io/publication"),
      [this](const auto& data) { onDelivery(data); });
  }

  int
  run()
  {
    const bool pinned = pinCurrentThread(m_options.ioCpu);
    m_journal.record("process-start",
      std::string("{\"executionModel\":\"single-face-io\",\"ioCpu\":") +
      std::to_string(m_options.ioCpu) + ",\"pinned\":" +
      (pinned ? "true" : "false") + "}");
    std::cout << "SPEC134_READY peer=" << m_options.peerId
              << " executionModel=single-face-io" << std::endl;

    m_periodNs = NS / m_options.rate;
    m_scheduledMeasured = uint64_t(m_options.rate) * m_options.measure;
    m_start = std::chrono::steady_clock::now() +
              std::chrono::milliseconds(m_options.startDelayMs);
    m_measuredStart = m_start + std::chrono::seconds(m_options.warmup);
    m_end = m_measuredStart + std::chrono::seconds(m_options.measure);
    armPublication(0);
    m_face.processEvents(0_ms, true);
    return m_exitCode;
  }

private:
  void
  armPublication(uint64_t index)
  {
    const auto deadline = m_start + std::chrono::nanoseconds(index * m_periodNs);
    if (deadline >= m_end) {
      armStop();
      return;
    }
    m_releaseTimer.expires_at(deadline);
    m_releaseTimer.async_wait([this, index, deadline](const boost::system::error_code& error) {
      if (error) {
        if (error != boost::asio::error::operation_aborted) {
          ++m_publishErrors;
          m_exitCode = 2;
          armStop();
        }
        return;
      }
      onRelease(index, deadline);
    });
  }

  void
  onRelease(uint64_t index, std::chrono::steady_clock::time_point deadline)
  {
    const auto actual = std::chrono::steady_clock::now();
    if (actual >= m_end) {
      armStop();
      return;
    }

    const bool measured = deadline >= m_measuredStart;
    const uint64_t logicalId = index + 1;
    if (measured) {
      ++m_attemptedMeasured;
      const auto late = std::chrono::duration_cast<std::chrono::nanoseconds>(
        actual - deadline).count();
      m_latenessNs.push_back(late > 0 ? static_cast<uint64_t>(late) : 0);
    }

    auto payload = makePayload(logicalId, measured, m_options.cellId,
                               m_options.peerId);
    Name name("/spec134/io/publication");
    name.append(m_options.peerId).appendNumber(logicalId);
    try {
      m_pubsub->publish(name, make_span(payload));
    }
    catch (const std::exception& error) {
      ++m_publishErrors;
      std::cerr << "publish error logicalId=" << logicalId
                << " error=" << error.what() << '\n';
    }

    const auto completed = std::chrono::steady_clock::now();
    uint64_t next = index + 1;
    if (completed > m_start) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        completed - m_start).count();
      const uint64_t firstFuture = static_cast<uint64_t>(elapsed) / m_periodNs + 1;
      next = std::max(next, firstFuture);
    }
    const uint64_t heartbeatSecond = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(completed - m_start).count());
    if (heartbeatSecond > m_lastHeartbeatSecond) {
      m_lastHeartbeatSecond = heartbeatSecond;
      m_journal.record("heartbeat", countersJson());
    }
    armPublication(next);
  }

  void
  armStop()
  {
    if (m_stopArmed) {
      return;
    }
    m_stopArmed = true;
    m_releaseTimer.expires_at(m_end + std::chrono::seconds(m_options.drain + 2));
    m_releaseTimer.async_wait([this](const boost::system::error_code& error) {
      if (!error) {
        stop();
      }
    });
  }

  void
  stop()
  {
    m_journal.record("process-stop", countersJson());
    m_face.shutdown();
    m_face.getIoContext().stop();
  }

  void
  onDelivery(const SVSPubSub::SubscriptionData& data)
  {
    if (data.data.size() != PAYLOAD_SIZE ||
        std::memcmp(data.data.data(), MAGIC, sizeof(MAGIC)) != 0) {
      ++m_invalidRemoteMeasured;
      return;
    }
    uint64_t senderBe = 0;
    std::memcpy(&senderBe, data.data.data() + 24, sizeof(senderBe));
    const uint64_t sender = hostToBe64(senderBe);
    if (sender == m_ownSenderHash) {
      ++m_localDeliveryIgnored;
      return;
    }
    if (sender != m_remoteSenderHash) {
      ++m_invalidRemoteMeasured;
      return;
    }
    uint64_t logicalBe = 0;
    std::memcpy(&logicalBe, data.data.data() + 16, sizeof(logicalBe));
    const uint64_t logicalId = hostToBe64(logicalBe);
    const bool measured = data.data[9] == 1;
    const auto expected = makePayload(logicalId, measured, m_options.cellId,
                                      m_options.remotePeerId);
    if (!std::equal(expected.begin(), expected.end(), data.data.begin())) {
      ++m_invalidRemoteMeasured;
      return;
    }
    if (measured) {
      ++m_deliveredMeasured;
    }
  }

  uint64_t
  percentile(double fraction) const
  {
    if (m_latenessNs.empty()) {
      return 0;
    }
    auto values = m_latenessNs;
    std::sort(values.begin(), values.end());
    const size_t rank = std::max<size_t>(
      1, static_cast<size_t>(fraction * values.size() + 0.999999));
    return values[std::min(rank, values.size()) - 1];
  }

  std::string
  countersJson() const
  {
    const uint64_t missedReleaseMeasured =
      m_scheduledMeasured > m_attemptedMeasured
        ? m_scheduledMeasured - m_attemptedMeasured : 0;
    std::ostringstream output;
    output << "{\"scheduledMeasured\":" << m_scheduledMeasured
           << ",\"attemptedMeasured\":" << m_attemptedMeasured
           << ",\"missedReleaseMeasured\":" << missedReleaseMeasured
           << ",\"deliveredMeasured\":" << m_deliveredMeasured
           << ",\"localDeliveryIgnored\":" << m_localDeliveryIgnored
           << ",\"invalidRemoteMeasured\":" << m_invalidRemoteMeasured
           << ",\"publishErrors\":" << m_publishErrors
           << ",\"latenessP50Ns\":" << percentile(0.50)
           << ",\"latenessP95Ns\":" << percentile(0.95)
           << ",\"latenessMaxNs\":" << percentile(1.0) << "}";
    return output.str();
  }

private:
  Options m_options;
  Journal m_journal;
  Face m_face;
  KeyChain m_keyChain;
  boost::asio::steady_timer m_releaseTimer;
  std::unique_ptr<SVSPubSub> m_pubsub;
  const uint64_t m_ownSenderHash;
  const uint64_t m_remoteSenderHash;
  std::chrono::steady_clock::time_point m_start;
  std::chrono::steady_clock::time_point m_measuredStart;
  std::chrono::steady_clock::time_point m_end;
  uint64_t m_periodNs = 0;
  uint64_t m_scheduledMeasured = 0;
  uint64_t m_attemptedMeasured = 0;
  uint64_t m_deliveredMeasured = 0;
  uint64_t m_localDeliveryIgnored = 0;
  uint64_t m_invalidRemoteMeasured = 0;
  uint64_t m_publishErrors = 0;
  uint64_t m_lastHeartbeatSecond = 0;
  std::vector<uint64_t> m_latenessNs;
  bool m_stopArmed = false;
  int m_exitCode = 0;
};

} // namespace

int
main(int argc, char** argv)
{
  try {
    return Peer(parse(argc, argv)).run();
  }
  catch (const std::exception& error) {
    std::cerr << "SPEC134_IO_ERROR " << error.what() << std::endl;
    return 2;
  }
}
