#include <ndn-svs/svspubsub.hpp>

#ifndef SPEC133_PROFILED
#define SPEC133_PROFILED 0
#endif

#if SPEC133_PROFILED
#include <ndn-svs/profile.hpp>
#endif

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>

#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ndn;
using namespace ndn::svs;

namespace {

constexpr const char* SUBJECT = "sync-publish-no-internal-parallelism";
constexpr uint64_t NS = 1000000000ULL;

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
  const uint8_t magic[8] = {'S', 'V', 'S', '1', '3', '3', 0, 0};
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

std::string
readEnv(const char* key)
{
  const char* value = std::getenv(key);
  return value == nullptr ? "" : value;
}

struct Options
{
  std::string subject;
  std::string profileMode;
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
  unsigned ioCpu = 0;
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
    if (key == "--subject") options.subject = value();
    else if (key == "--profile-mode") options.profileMode = value();
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
    else if (key == "--io-cpu") options.ioCpu = std::stoul(value());
    else throw std::runtime_error("unknown argument: " + key);
  }

  if (options.subject != SUBJECT || options.syncPrefix.empty() ||
      options.nodePrefix.empty() || options.cellId.empty() || options.peerId.empty() ||
      options.remotePeerId.empty() || options.peerId == options.remotePeerId ||
      options.events.empty() || options.rate == 0 || options.measure == 0) {
    throw std::runtime_error("missing or mismatched required arguments");
  }
  if (options.profileMode != "clean" && options.profileMode != "disabled" &&
      options.profileMode != "enabled") {
    throw std::runtime_error("profile mode must be clean, disabled, or enabled");
  }
#if SPEC133_PROFILED
  if (options.profileMode == "clean")
    throw std::runtime_error("profiled binary cannot claim clean mode");
#else
  if (options.profileMode != "clean")
    throw std::runtime_error("clean binary cannot claim profiled mode");
#endif

  const std::string envCell = readEnv("NDN_SVS_PROFILE_CELL_ID");
  const std::string envPeer = readEnv("NDN_SVS_PROFILE_PEER_ID");
  if ((!envCell.empty() && envCell != options.cellId) ||
      (!envPeer.empty() && envPeer != options.peerId)) {
    throw std::runtime_error("profile environment identity mismatch");
  }
  return options;
}

struct EventSink
{
  std::vector<std::string> lines;
  std::string cellId;
  std::string peerId;

  void
  add(const std::string& event, uint64_t logicalId, uint64_t seqNo,
      const std::string& phase, uint64_t timestamp,
      const std::string& details = "{}")
  {
    std::ostringstream output;
    output << "{\"schemaVersion\":\"spec133-app-event-v1\",\"cellId\":\""
           << cellId << "\",\"peerId\":\"" << peerId << "\",\"event\":\""
           << event << "\",\"logicalId\":" << logicalId << ",\"svsSeqNo\":"
           << seqNo << ",\"phase\":\"" << phase << "\",\"monotonicRawNs\":"
           << timestamp << ",\"details\":" << details << "}";
    lines.push_back(output.str());
  }
};

class Peer
{
public:
  explicit Peer(Options options)
    : m_options(std::move(options))
    , m_publishEvents{{}, m_options.cellId, m_options.peerId}
    , m_faceEvents{{}, m_options.cellId, m_options.peerId}
    , m_releaseTimer(m_face.getIoContext())
    , m_ownSenderHash(fnv1a(m_options.peerId))
    , m_remoteSenderHash(fnv1a(m_options.remotePeerId))
  {
    const size_t planned = size_t(m_options.rate) *
                           (m_options.warmup + m_options.measure);
    m_publishEvents.lines.reserve(planned * 3 + 8);
    m_faceEvents.lines.reserve(planned * 2 + 16);

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
      [this](const std::vector<MissingDataInfo>& info) { onUpdate(info); },
      pubsubOptions, security);
    m_pubsub->subscribe(Name("/spec133/publication"),
                        [this](const auto& data) { onDelivery(data); });
  }

  int
  run()
  {
    const bool pinned = pinCurrentThread(m_options.ioCpu);
    m_publishEvents.add("process-start", 0, 0, "all", nowRaw(),
      std::string("{\"profileMode\":\"") + m_options.profileMode +
      "\",\"profileEnabledEnv\":\"" + readEnv("NDN_SVS_PROFILE_ENABLED") +
      "\",\"loggerEnv\":\"" + readEnv("NDN_LOG") +
      "\",\"executionModel\":\"single-face-io-thread\",\"ioCpu\":" +
      std::to_string(m_options.ioCpu) + ",\"pinned\":" +
      (pinned ? "true" : "false") + "}");

    std::cout << "SPEC133_READY peer=" << m_options.peerId
              << " subject=" << SUBJECT
              << " profileMode=" << m_options.profileMode
              << " executionModel=single-face-io-thread" << std::endl;

    m_periodNs = NS / m_options.rate;
    m_scheduledMeasured = uint64_t(m_options.rate) * m_options.measure;
    m_start = std::chrono::steady_clock::now() +
              std::chrono::milliseconds(m_options.startDelayMs);
    const auto untilStart = std::chrono::duration_cast<std::chrono::nanoseconds>(
      m_start - std::chrono::steady_clock::now()).count();
    m_rawStart = nowRaw() + std::max<int64_t>(0, untilStart);
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
    m_releaseTimer.async_wait(
      [this, index, deadline](const boost::system::error_code& error) {
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
    const uint64_t wakeRaw = nowRaw();
    const uint64_t scheduled = m_rawStart + index * m_periodNs;
    const uint8_t phaseCode = deadline >= m_measuredStart ? 1 : 0;
    const std::string phase = phaseCode ? "measured" : "warmup";
    const uint64_t logicalId = index + 1;
    if (phaseCode)
      ++m_attemptedMeasured;
    m_publishEvents.add("deadline", logicalId, 0, phase, scheduled,
      std::string("{\"actualWakeNs\":") + std::to_string(wakeRaw) + "}");

    auto payload = makePayload(logicalId, scheduled, phaseCode,
                               m_options.cellId, m_options.peerId);
    Name name("/spec133/publication");
    name.append(m_options.peerId).appendNumber(logicalId);
    m_publishEvents.add("api-enter", logicalId, 0, phase, nowRaw());
    try {
      const SeqNo seqNo = m_pubsub->publish(name, make_span(payload));
      m_publishEvents.add("api-return", logicalId, seqNo, phase, nowRaw());
    }
    catch (const std::exception& error) {
      ++m_publishErrors;
      m_publishEvents.add("api-error", logicalId, 0, phase, nowRaw(),
                          "{\"reason\":\"publication-exception\"}");
      std::cerr << "publication exception peer=" << m_options.peerId
                << " logicalId=" << logicalId << " error=" << error.what() << '\n';
    }

    const auto completed = std::chrono::steady_clock::now();
    uint64_t next = index + 1;
    if (completed > m_start) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        completed - m_start).count();
      next = std::max(next, static_cast<uint64_t>(elapsed) / m_periodNs + 1);
    }
    armPublication(next);
  }

  void
  armStop()
  {
    if (m_stopArmed)
      return;
    m_stopArmed = true;
    m_releaseTimer.expires_at(m_end + std::chrono::seconds(m_options.drain + 2));
    m_releaseTimer.async_wait([this](const boost::system::error_code& error) {
      if (!error)
        stop();
    });
  }

  void
  stop()
  {
#if SPEC133_PROFILED
    profile::Profiler::get().flush();
#endif
    const uint64_t missed = m_scheduledMeasured > m_attemptedMeasured ?
                            m_scheduledMeasured - m_attemptedMeasured : 0;
    m_publishEvents.add("process-stop", 0, 0, "all", nowRaw(),
      std::string("{\"scheduledMeasured\":") + std::to_string(m_scheduledMeasured) +
      ",\"attemptedMeasured\":" + std::to_string(m_attemptedMeasured) +
      ",\"missedReleaseMeasured\":" + std::to_string(missed) +
      ",\"localDeliveryIgnored\":" + std::to_string(m_localDeliveryIgnored) +
      ",\"publishErrors\":" + std::to_string(m_publishErrors) + "}");
    flushEvents();
    m_face.shutdown();
    m_face.getIoContext().stop();
  }

  void
  onUpdate(const std::vector<MissingDataInfo>& info)
  {
    const uint64_t timestamp = nowRaw();
#if SPEC133_PROFILED
    const uint64_t traceId = profile::Profiler::get().currentTraceId();
    profile::Profiler::get().recordInterval(profile::StageId::APP_STATE_UPDATE,
                                            traceId, timestamp, 0);
#endif
    for (const auto& item : info) {
      if (item.nodeId.toUri().find(m_options.remotePeerId) == std::string::npos)
        continue;
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
#if SPEC133_PROFILED
    const uint64_t traceId = profile::Profiler::get().currentTraceId();
    profile::Span checkSpan(profile::StageId::APP_PAYLOAD_CHECK, traceId);
#endif
    if (data.data.size() != 256 || std::memcmp(data.data.data(), "SVS133\0\0", 8) != 0) {
#if SPEC133_PROFILED
      checkSpan.setOutcome(profile::Outcome::Invalid);
#endif
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
    if (senderHash == m_ownSenderHash) {
#if SPEC133_PROFILED
      checkSpan.setOutcome(profile::Outcome::Success);
      checkSpan.setCounts(data.data.size(), 1);
      checkSpan.stop();
#endif
      ++m_localDeliveryIgnored;
      m_faceEvents.add("local-delivery", logicalId, data.seqNo, phase, timestamp,
                       "{\"excludedFromRemoteMetrics\":true}");
      return;
    }
    const auto expected = makePayload(logicalId, scheduled, phaseCode,
                                      m_options.cellId, m_options.remotePeerId);
    const bool payloadValid = senderHash == m_remoteSenderHash &&
      std::equal(expected.begin(), expected.end(), data.data.begin());
    const bool duplicate = !m_seen.emplace(logicalId, true).second;
#if SPEC133_PROFILED
    checkSpan.setOutcome(payloadValid ? profile::Outcome::Success : profile::Outcome::Invalid);
    checkSpan.setCounts(data.data.size(), 1);
    checkSpan.stop();
    if (payloadValid && !duplicate) {
      profile::Profiler::get().recordInterval(profile::StageId::APP_DELIVERY,
                                              traceId, timestamp, 0);
    }
#endif

    std::ostringstream details;
    details << "{\"senderPeer\":\"" << m_options.remotePeerId
            << "\",\"scheduledNs\":" << scheduled
            << ",\"payloadValid\":" << (payloadValid ? "true" : "false")
            << ",\"hasPacket\":" << (data.packet ? "true" : "false") << "}";
    m_faceEvents.add(!payloadValid ? "invalid" : (duplicate ? "duplicate" : "delivery"),
                     logicalId, data.seqNo, phase, timestamp, details.str());
  }

  void
  flushEvents()
  {
    std::ofstream output(m_options.events);
    if (!output)
      throw std::runtime_error("cannot open event output");
    for (const auto& line : m_publishEvents.lines)
      output << line << '\n';
    for (const auto& line : m_faceEvents.lines)
      output << line << '\n';
  }

private:
  Options m_options;
  EventSink m_publishEvents;
  EventSink m_faceEvents;
  Face m_face;
  KeyChain m_keyChain;
  std::unique_ptr<SVSPubSub> m_pubsub;
  boost::asio::steady_timer m_releaseTimer;
  std::map<uint64_t, bool> m_seen;
  uint64_t m_ownSenderHash = 0;
  uint64_t m_remoteSenderHash = 0;
  uint64_t m_periodNs = 0;
  uint64_t m_rawStart = 0;
  uint64_t m_scheduledMeasured = 0;
  uint64_t m_attemptedMeasured = 0;
  uint64_t m_localDeliveryIgnored = 0;
  uint64_t m_publishErrors = 0;
  std::chrono::steady_clock::time_point m_start;
  std::chrono::steady_clock::time_point m_measuredStart;
  std::chrono::steady_clock::time_point m_end;
  bool m_stopArmed = false;
  int m_exitCode = 0;
};

bool
runSelfTestScenario(bool forceFallback)
{
  boost::asio::io_context io;
  DummyClientFace producerFace(io, {true, true});
  DummyClientFace receiverFace(io, {true, true});
  producerFace.linkTo(receiverFace);

  KeyChain producerKeyChain("pib-memory:spec133-producer", "tpm-memory:spec133-producer");
  KeyChain receiverKeyChain("pib-memory:spec133-receiver", "tpm-memory:spec133-receiver");
  SecurityOptions producerSecurity(producerKeyChain);
  SecurityOptions receiverSecurity(receiverKeyChain);
  producerSecurity.interestSigner->signingInfo.setSigningHmacKey(
    "dGhpcyBpcyBhIHNlY3JldCBtZXNzYWdl");
  receiverSecurity.interestSigner->signingInfo.setSigningHmacKey(
    "dGhpcyBpcyBhIHNlY3JldCBtZXNzYWdl");
  producerSecurity.dataSigner->signingInfo.setSha256Signing();
  receiverSecurity.dataSigner->signingInfo.setSha256Signing();

  SVSPubSubOptions producerOptions;
  producerOptions.useTimestamp = false;
  if (forceFallback) {
    producerOptions.maxApplicationParametersSize = 1;
    producerOptions.maxPiggyDataSize = 1;
  }
  SVSPubSubOptions receiverOptions;
  receiverOptions.useTimestamp = false;
  const std::string suffix = forceFallback ? "fallback" : "piggy";
  const Name syncPrefix("/spec133/selftest/" + suffix);
  const Name producerPrefix("/spec133/selftest/" + suffix + "/producer");
  const Name receiverPrefix("/spec133/selftest/" + suffix + "/receiver");
  SVSPubSub producer(syncPrefix, producerPrefix, producerFace,
                     [](const std::vector<MissingDataInfo>&) {},
                     producerOptions, producerSecurity);
  size_t deliveries = 0;
  std::string received;
  SVSPubSub receiver(syncPrefix, receiverPrefix, receiverFace,
                     [](const std::vector<MissingDataInfo>&) {
#if SPEC133_PROFILED
                       auto& profiler = profile::Profiler::get();
                       profiler.recordInterval(profile::StageId::APP_STATE_UPDATE,
                                               profiler.currentTraceId(), nowRaw(), 0);
#endif
                     },
                     receiverOptions, receiverSecurity);
  receiver.subscribe(Name("/spec133/selftest/publication"),
                     [&](const SVSPubSub::SubscriptionData& data) {
#if SPEC133_PROFILED
                       auto& profiler = profile::Profiler::get();
                       const uint64_t traceId = profiler.currentTraceId();
                       profile::Span checkSpan(profile::StageId::APP_PAYLOAD_CHECK, traceId);
                       checkSpan.setCounts(data.data.size(), 1);
#endif
                       ++deliveries;
                       received.assign(reinterpret_cast<const char*>(data.data.data()),
                                       data.data.size());
#if SPEC133_PROFILED
                       checkSpan.stop();
                       profiler.recordInterval(profile::StageId::APP_DELIVERY,
                                               traceId, nowRaw(), 0);
#endif
                     });

  auto pumpUntil = [&](const std::function<bool()>& done,
                       std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      io.restart();
      io.run_for(std::chrono::milliseconds(10));
    }
    return done();
  };
  pumpUntil([] { return false; }, std::chrono::milliseconds(30));
  const std::string body = forceFallback ? "fallback" : "piggyback";
  producer.publish(Name("/spec133/selftest/publication/" + suffix),
                   make_span(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
  return pumpUntil([&] { return deliveries == 1; }, std::chrono::seconds(3)) &&
         received == body;
}

int
selfTest()
{
  const auto payload = makePayload(7, 11, 1, "self-test", "peer-a");
  if (payload.size() != 256 || std::memcmp(payload.data(), "SVS133\0\0", 8) != 0)
    return 2;
  const bool piggyback = runSelfTestScenario(false);
  const bool fallback = runSelfTestScenario(true);
#if SPEC133_PROFILED
  profile::Profiler::get().flush();
#endif
  if (!piggyback || !fallback)
    return 3;
  std::cout << "SPEC133_SELF_TEST_OK subject=" << SUBJECT
            << " piggyback=1 fallback=1 profiled=" << SPEC133_PROFILED << std::endl;
  return 0;
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    if (argc == 2 && std::string(argv[1]) == "--self-test")
      return selfTest();
    if (argc == 2 && std::string(argv[1]) == "--clock-probe") {
      std::cout << nowRaw() << std::endl;
      return 0;
    }
    return Peer(parse(argc, argv)).run();
  }
  catch (const std::exception& error) {
    std::cerr << "SPEC133_ERROR " << error.what() << std::endl;
    return 2;
  }
}
