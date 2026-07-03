#include "ndn-service-framework/CertificateBootstrap.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

NDN_LOG_INIT(ndn_service_framework.AppUser);

const ndn::Name GROUP_PREFIX("/example/hello/group");
const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
const ndn::Name PROVIDER_IDENTITY("/example/hello/provider");
const ndn::Name USER_IDENTITY("/example/hello/user");

class KeyChainInitLock
{
public:
  explicit KeyChainInitLock(const char* path)
  {
    m_fd = open(path, O_CREAT | O_RDWR, 0666);
    if (m_fd < 0 || flock(m_fd, LOCK_EX) != 0) {
      throw std::runtime_error("Failed to acquire KeyChain initialization lock");
    }
  }

  ~KeyChainInitLock()
  {
    unlock();
  }

  void
  unlock()
  {
    if (m_fd >= 0) {
      flock(m_fd, LOCK_UN);
      close(m_fd);
      m_fd = -1;
    }
  }

private:
  int m_fd = -1;
};

class SampledLogGate
{
public:
  explicit SampledLogGate(size_t maxPerSecond)
    : m_maxPerSecond(maxPerSecond)
  {
  }

  bool
  allow()
  {
    using clock = std::chrono::steady_clock;
    const auto second = std::chrono::duration_cast<std::chrono::seconds>(
      clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(m_mutex);
    if (second != m_second) {
      m_second = second;
      m_count = 0;
    }
    if (m_count >= m_maxPerSecond) {
      return false;
    }
    ++m_count;
    return true;
  }

private:
  const size_t m_maxPerSecond;
  int64_t m_second = -1;
  size_t m_count = 0;
  std::mutex m_mutex;
};

size_t
envSizeOption(const char* name, size_t defaultValue)
{
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return defaultValue;
  }
  try {
    return std::max<size_t>(1, static_cast<size_t>(std::stoull(value)));
  }
  catch (...) {
    return defaultValue;
  }
}

bool
sampleByRequestId(const ndn::Name& requestId, size_t sampleRate)
{
  if (sampleRate <= 1 || requestId.empty()) {
    return true;
  }
  return (std::hash<std::string>{}(requestId.toUri()) % sampleRate) == 0;
}

ndn::security::Certificate
getOrCreateIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
{
  try {
    return keyChain.getPib()
      .getIdentity(identity)
      .getDefaultKey()
      .getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey()
      .getDefaultCertificate();
  }
}

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

int
parseIntOption(int argc, char** argv, const std::string& option, int fallback)
{
  const auto value = getOption(argc, argv, option, "");
  if (value.empty()) {
    return fallback;
  }
  try {
    return std::stoi(value);
  }
  catch (const std::exception&) {
    return fallback;
  }
}

double
parseDoubleOption(int argc, char** argv, const std::string& option, double fallback)
{
  const auto value = getOption(argc, argv, option, "");
  if (value.empty()) {
    return fallback;
  }
  try {
    return std::stod(value);
  }
  catch (const std::exception&) {
    return fallback;
  }
}

int
parseMetadataInt(const std::string& payload, const std::string& key, int fallback)
{
  const std::string marker = key + "=";
  const auto start = payload.find(marker);
  if (start == std::string::npos) {
    return fallback;
  }

  const auto valueStart = start + marker.size();
  const auto valueEnd = payload.find(';', valueStart);
  const auto value = payload.substr(valueStart, valueEnd - valueStart);
  try {
    return std::stoi(value);
  }
  catch (const std::exception&) {
    return fallback;
  }
}

std::string
providerLabel(const ndn::Name& providerName);

size_t
parseBenchmarkStrategy(const std::string& strategy)
{
  if (strategy == "all-selected" || strategy == "AllSelected") {
    return ndn_service_framework::tlv::AllSelected;
  }
  if (strategy == "first-responding" || strategy == "FirstResponding") {
    return ndn_service_framework::tlv::FirstResponding;
  }
  if (strategy == "random-selection" || strategy == "RandomSelection") {
    return ndn_service_framework::tlv::RandomSelection;
  }
  return ndn_service_framework::tlv::FirstResponding;
}

std::string
benchmarkStrategyLabel(size_t strategy, bool customSelection, bool randomSelection)
{
  if (customSelection) {
    return "custom-selection";
  }
  if (randomSelection) {
    return "random-selection";
  }
  if (strategy == ndn_service_framework::tlv::AllSelected) {
    return "all-selected";
  }
  if (strategy == ndn_service_framework::tlv::RandomSelection) {
    return "random-selection";
  }
  return "first-responding";
}

class RankQueueSelectionPolicy final : public ndnsf::AckSelectionPolicy
{
public:
  RankQueueSelectionPolicy(int ackTimeoutMs, bool performanceMode)
    : m_ackTimeoutMs(ackTimeoutMs)
    , m_performanceMode(performanceMode)
  {
  }

  std::vector<ndnsf::ProviderId>
  select(const std::vector<ndnsf::AckCandidate>& candidates) const override
  {
    if (!m_performanceMode) {
      NDN_LOG_INFO( "customSelectionStrategy ran after ackTimeoutMs="
                << m_ackTimeoutMs
                << " candidateCount=" << candidates.size());
    }

    std::vector<ndnsf::AckCandidate> selected;
    int bestQueue = std::numeric_limits<int>::max();
    int bestBacklog = std::numeric_limits<int>::max();
    for (const auto& candidate : candidates) {
      const auto payload = candidate.ack.getPayload();
      const std::string payloadText(
        reinterpret_cast<const char*>(payload.data()),
        payload.size());

      if (!candidate.ack.getStatus()) {
        if (!m_performanceMode) {
          NDN_LOG_INFO( "customSelectionStrategy rejected provider="
                    << providerLabel(candidate.providerName)
                    << " status=0");
        }
        continue;
      }

      const int rank = parseMetadataInt(payloadText, "rank",
                                        std::numeric_limits<int>::max());
      const int queue = parseMetadataInt(payloadText, "queue",
                                         std::numeric_limits<int>::max());
      const int backlog = parseMetadataInt(payloadText, "backlog", queue);
      const int processed10s = parseMetadataInt(payloadText, "processed10s", 0);
      if (!m_performanceMode) {
        NDN_LOG_INFO( "collected ACK payload provider="
                  << providerLabel(candidate.providerName)
                  << " backlog=" << backlog
                  << " processed10s=" << processed10s
                  << " queue=" << queue
                  << " rank=" << rank);
      }
      if (selected.empty() ||
          queue < bestQueue ||
          (queue == bestQueue && backlog < bestBacklog)) {
        selected.clear();
        selected.push_back(candidate);
        bestQueue = queue;
        bestBacklog = backlog;
      }
      else if (queue == bestQueue &&
               backlog == bestBacklog) {
        selected.push_back(candidate);
      }
    }

    if (selected.size() > 1) {
      auto chosen = selected.front();
      size_t bestSelectionCount = std::numeric_limits<size_t>::max();
      for (const auto& candidate : selected) {
        const auto provider = candidate.providerName.toUri();
        const auto countIt = m_selectionCounts.find(provider);
        const size_t count =
          countIt == m_selectionCounts.end() ? 0 : countIt->second;
        if (count < bestSelectionCount) {
          bestSelectionCount = count;
          chosen = candidate;
        }
      }
      selected.clear();
      selected.push_back(chosen);
    }
    if (!selected.empty()) {
      const auto provider = selected.front().providerName.toUri();
      ++m_selectionCounts[provider];
    }
    if (!selected.empty() && !m_performanceMode) {
      NDN_LOG_INFO( "customSelectionStrategy selected providerName="
                << selected.front().providerName.toUri());
    }
    if (selected.empty()) {
      return {};
    }
    return {selected.front().providerName};
  }

private:
  int m_ackTimeoutMs = 0;
  bool m_performanceMode = false;
  mutable std::map<std::string, size_t> m_selectionCounts;
};

std::shared_ptr<const ndnsf::AckSelectionPolicy>
makeRankQueueSelectionPolicy(int ackTimeoutMs, bool performanceMode)
{
  return std::make_shared<RankQueueSelectionPolicy>(ackTimeoutMs,
                                                    performanceMode);
}

class FunctionalAckSelectionPolicy final : public ndnsf::AckSelectionPolicy
{
public:
  using Selector =
    std::function<std::vector<ndnsf::ProviderId>(
      const std::vector<ndnsf::AckCandidate>&)>;

  explicit FunctionalAckSelectionPolicy(Selector selector)
    : m_selector(std::move(selector))
  {
  }

  std::vector<ndnsf::ProviderId>
  select(const std::vector<ndnsf::AckCandidate>& candidates) const override
  {
    return m_selector ? m_selector(candidates) : std::vector<ndnsf::ProviderId>{};
  }

private:
  Selector m_selector;
};

uint64_t
nowMilliseconds()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string
providerLabel(const ndn::Name& providerName)
{
  if (providerName.empty()) {
    return "";
  }
  return providerName[-1].toUri();
}

std::vector<ndn::Name>
parseKnownProviderIds(const std::string& csv)
{
  std::vector<ndn::Name> providers;
  std::stringstream stream(csv);
  std::string item;
  while (std::getline(stream, item, ',')) {
    item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) {
      return !std::isspace(ch);
    }));
    item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) {
      return !std::isspace(ch);
    }).base(), item.end());
    if (item.empty()) {
      continue;
    }
    if (item == "default") {
      providers.push_back(PROVIDER_IDENTITY);
    }
    else if (item.front() == '/') {
      providers.emplace_back(item);
    }
    else {
      providers.push_back(ndn::Name(PROVIDER_IDENTITY).append(item));
    }
  }
  return providers;
}

std::string
csvEscape(const std::string& value)
{
  if (value.find_first_of(",\"\n\r") == std::string::npos) {
    return value;
  }

  std::string escaped = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      escaped += "\"\"";
    }
    else {
      escaped += ch;
    }
  }
  escaped += '"';
  return escaped;
}

double
percentile(std::vector<double> sortedValues, double percentileRank)
{
  if (sortedValues.empty()) {
    return 0.0;
  }

  std::sort(sortedValues.begin(), sortedValues.end());
  const auto index = static_cast<size_t>(
    std::ceil((percentileRank / 100.0) * sortedValues.size()));
  return sortedValues[std::min(sortedValues.size() - 1, index == 0 ? 0 : index - 1)];
}

struct BenchmarkResult
{
  std::string requestId;
  bool success = false;
  double latencyMs = 0.0;
  std::string responsePayload;
};

struct OpenLoopRequestState
{
  std::string requestId;
  std::chrono::steady_clock::time_point start;
  bool measured = true;
  bool completed = false;
  std::string selectedProvider = "-";
};

struct AdaptiveAdmissionConfig
{
  bool enabled = true;
  size_t minWindow = 1;
  size_t maxWindow = 512;
  size_t initialWindow = 16;
  size_t hardInflightLimit = 512;
  size_t aiStep = 4;
  double mdFactor = 0.85;
  double severeMdFactor = 0.5;
  int controlIntervalMs = 500;
  int targetLatencyMs = 350;
  int hardTargetLatencyMs = 500;
  size_t softQueueLimit = 0;
  size_t hardQueueLimit = 0;
  int warningBackoffMs = 0;
  int rejectBackoffMs = 0;
  bool queueAwarePause = false;
  bool useRecommendedRate = true;
  size_t warningResumeQueueDepth = 0;
  size_t rejectResumeQueueDepth = 0;
  int queuePausePollMs = 10;
};

std::pair<size_t, size_t>
adaptiveQueueLimitsForWindow(size_t window, size_t softLimitCap, size_t hardLimitCap)
{
  const size_t active = std::max<size_t>(1, window);
  const size_t scaledHard = active > (std::numeric_limits<size_t>::max() - 16) / 2 ?
    std::numeric_limits<size_t>::max() : 16 + active * 2;
  size_t hardLimit = std::min<size_t>(256, std::max<size_t>(32, scaledHard));
  if (hardLimitCap > 0) {
    hardLimit = std::min(hardLimit, hardLimitCap);
  }
  hardLimit = std::max<size_t>(1, hardLimit);

  size_t softLimit = std::max<size_t>(
    1, static_cast<size_t>(std::ceil(static_cast<double>(hardLimit) * 0.5)));
  if (softLimitCap > 0) {
    softLimit = std::min(softLimit, softLimitCap);
  }
  return {std::min(softLimit, hardLimit), hardLimit};
}

std::string
formatCounters(const std::map<std::string, uint64_t>& counters)
{
  std::ostringstream os;
  bool first = true;
  for (const auto& item : counters) {
    if (!first) {
      os << ";";
    }
    first = false;
    os << item.first << ":" << item.second;
  }
  return os.str();
}

double
percentileSize(std::vector<size_t> values, double percentileRank)
{
  if (values.empty()) {
    return 0.0;
  }

  std::sort(values.begin(), values.end());
  const auto index = static_cast<size_t>(
    std::ceil((percentileRank / 100.0) * values.size()));
  return static_cast<double>(values[std::min(values.size() - 1, index == 0 ? 0 : index - 1)]);
}

double
medianSize(std::vector<size_t> values)
{
  if (values.empty()) {
    return 0.0;
  }

  std::sort(values.begin(), values.end());
  const auto mid = values.size() / 2;
  if (values.size() % 2 == 1) {
    return static_cast<double>(values[mid]);
  }
  return (static_cast<double>(values[mid - 1]) + static_cast<double>(values[mid])) / 2.0;
}

std::string
userScopedLockPath(const std::string& base)
{
  return base + "-" + std::to_string(getuid()) + ".lock";
}

std::chrono::nanoseconds
openLoopPacingJitter(uint64_t sequence, int jitterUs)
{
  if (jitterUs <= 0) {
    return std::chrono::nanoseconds(0);
  }
  static constexpr int pattern[] = {-100, -50, 75, 25, 100, -75, 50, -25};
  const int scaledUs = (jitterUs * pattern[sequence % std::size(pattern)]) / 100;
  return std::chrono::microseconds(scaledUs);
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    ndn::Face face;
    const auto keyChainLockPath = userScopedLockPath("/tmp/ndnsf-keychain-init");
    KeyChainInitLock keyChainInitLock(keyChainLockPath.c_str());
    ndn::KeyChain keyChain;
    ndn::Scheduler scheduler(face.getIoContext());

    const bool useCustomSelection = hasFlag(argc, argv, "--custom-selection");
    const bool benchmark = hasFlag(argc, argv, "--benchmark");
    const bool performanceMode = hasFlag(argc, argv, "--performance-mode");
    auto perfLogGate = std::make_shared<SampledLogGate>(10);
    const bool useTokens = !hasFlag(argc, argv, "--disable-tokens");
    const bool timelineTrace = hasFlag(argc, argv, "--timeline-trace");
    const bool serveCertificates = !hasFlag(argc, argv, "--no-serve-certificates");
    const std::string workloadMode = getOption(argc, argv, "--workload-mode", "closed-loop");
    const int benchmarkCount = parseIntOption(argc, argv, "--count", 100);
    const int benchmarkWarmup = parseIntOption(argc, argv, "--warmup", 5);
    const int benchmarkIntervalMs = parseIntOption(argc, argv, "--interval-ms", 1000);
    const int ackTimeoutMs = parseIntOption(argc, argv, "--ack-timeout-ms", 3000);
    const int timeoutMs = parseIntOption(argc, argv, "--timeout-ms", 5000);
    const int requestTimeoutMs = parseIntOption(argc, argv, "--request-timeout-ms", timeoutMs);
    const int handlerThreads = parseIntOption(argc, argv, "--handler-threads", -1);
    const double rateRps = parseDoubleOption(argc, argv, "--rate-rps", 1.0);
    const int openLoopPacingJitterUs = std::max(
      0, parseIntOption(argc, argv, "--pacing-jitter-us", 0));
    const int openLoopDurationSeconds = parseIntOption(argc, argv, "--duration", 10);
    const int maxOutstanding = parseIntOption(
      argc, argv, "--max-inflight",
      parseIntOption(argc, argv, "--max-outstanding", 512));
    const bool largeDataPublishTest = hasFlag(argc, argv, "--large-data-publish-test");
    const std::string largeDataPlaintext =
      getOption(argc, argv, "--large-data-plaintext", "large-data-image");
    const std::string largeDataNameFile =
      getOption(argc, argv, "--large-data-name-file", "");
    AdaptiveAdmissionConfig adaptiveAdmission;
    adaptiveAdmission.enabled =
      !hasFlag(argc, argv, "--disable-adaptive-admission-control");
    adaptiveAdmission.minWindow = static_cast<size_t>(std::max(
      1, parseIntOption(argc, argv, "--adaptive-min-window", 1)));
    adaptiveAdmission.maxWindow = static_cast<size_t>(std::max(
      1, parseIntOption(argc, argv, "--adaptive-max-window", maxOutstanding)));
    adaptiveAdmission.initialWindow = static_cast<size_t>(std::max(
      1, parseIntOption(argc, argv, "--adaptive-initial-window",
                        std::min(16, maxOutstanding))));
    adaptiveAdmission.hardInflightLimit = static_cast<size_t>(std::max(
      1, parseIntOption(argc, argv, "--adaptive-hard-inflight-limit",
                        static_cast<int>(adaptiveAdmission.maxWindow))));
    adaptiveAdmission.aiStep = static_cast<size_t>(std::max(
      1, parseIntOption(argc, argv, "--adaptive-ai-step", 4)));
    adaptiveAdmission.mdFactor = parseDoubleOption(argc, argv, "--adaptive-md-factor", 0.85);
    adaptiveAdmission.severeMdFactor =
      parseDoubleOption(argc, argv, "--adaptive-severe-md-factor", 0.5);
    adaptiveAdmission.controlIntervalMs = std::max(
      1, parseIntOption(argc, argv, "--adaptive-control-interval-ms", 500));
    adaptiveAdmission.targetLatencyMs = std::max(
      1, parseIntOption(argc, argv, "--adaptive-target-latency-ms", 350));
    adaptiveAdmission.hardTargetLatencyMs = std::max(
      adaptiveAdmission.targetLatencyMs,
      parseIntOption(argc, argv, "--adaptive-hard-target-latency-ms", 500));
    adaptiveAdmission.hardQueueLimit = static_cast<size_t>(std::max(
      0, parseIntOption(argc, argv, "--adaptive-hard-queue-limit",
                        parseIntOption(argc, argv, "--adaptive-max-queue-depth",
                                       0))));
    adaptiveAdmission.softQueueLimit = static_cast<size_t>(std::max(
      0, parseIntOption(argc, argv, "--adaptive-soft-queue-limit", 0)));
    adaptiveAdmission.warningBackoffMs = std::max(
      0, parseIntOption(argc, argv, "--adaptive-warning-backoff-ms", 0));
    adaptiveAdmission.rejectBackoffMs = std::max(
      0, parseIntOption(argc, argv, "--adaptive-reject-backoff-ms", 0));
    adaptiveAdmission.queueAwarePause =
      hasFlag(argc, argv, "--enable-adaptive-queue-aware-pause") &&
      !hasFlag(argc, argv, "--disable-adaptive-queue-aware-pause");
    adaptiveAdmission.useRecommendedRate =
      hasFlag(argc, argv, "--enable-adaptive-recommended-rate") &&
      !hasFlag(argc, argv, "--disable-adaptive-recommended-rate");
    adaptiveAdmission.queuePausePollMs = std::max(
      1, parseIntOption(argc, argv, "--adaptive-queue-pause-poll-ms", 10));
    adaptiveAdmission.maxWindow = std::max(adaptiveAdmission.minWindow,
                                           adaptiveAdmission.maxWindow);
    adaptiveAdmission.hardInflightLimit =
      std::max(adaptiveAdmission.minWindow, adaptiveAdmission.hardInflightLimit);
    adaptiveAdmission.maxWindow = std::min(adaptiveAdmission.maxWindow,
                                           adaptiveAdmission.hardInflightLimit);
    adaptiveAdmission.initialWindow = std::max(
      adaptiveAdmission.minWindow,
      std::min(adaptiveAdmission.initialWindow, adaptiveAdmission.maxWindow));
    if (adaptiveAdmission.softQueueLimit > 0 &&
        adaptiveAdmission.hardQueueLimit > 0) {
      adaptiveAdmission.softQueueLimit =
        std::min(adaptiveAdmission.softQueueLimit,
                 adaptiveAdmission.hardQueueLimit);
    }
    const auto initialQueueLimits = adaptiveQueueLimitsForWindow(
      adaptiveAdmission.initialWindow,
      adaptiveAdmission.softQueueLimit,
      adaptiveAdmission.hardQueueLimit);
    adaptiveAdmission.warningResumeQueueDepth = static_cast<size_t>(std::max(
      0, parseIntOption(argc, argv, "--adaptive-warning-resume-queue-depth",
                        static_cast<int>(initialQueueLimits.first / 2))));
    adaptiveAdmission.rejectResumeQueueDepth = static_cast<size_t>(std::max(
      0, parseIntOption(argc, argv, "--adaptive-reject-resume-queue-depth",
                        static_cast<int>(initialQueueLimits.first / 4))));
    adaptiveAdmission.warningResumeQueueDepth =
      std::min(adaptiveAdmission.warningResumeQueueDepth,
               initialQueueLimits.first);
    adaptiveAdmission.rejectResumeQueueDepth =
      std::min(adaptiveAdmission.rejectResumeQueueDepth,
               adaptiveAdmission.warningResumeQueueDepth);
    if (adaptiveAdmission.mdFactor <= 0.0 || adaptiveAdmission.mdFactor >= 1.0) {
      adaptiveAdmission.mdFactor = 0.85;
    }
    if (adaptiveAdmission.severeMdFactor <= 0.0 ||
        adaptiveAdmission.severeMdFactor >= adaptiveAdmission.mdFactor) {
      adaptiveAdmission.severeMdFactor = std::min(0.5, adaptiveAdmission.mdFactor * 0.7);
    }
    const int defaultDrainSeconds = static_cast<int>(
      std::ceil((requestTimeoutMs + ackTimeoutMs + 2000) / 1000.0));
    const int drainSeconds = parseIntOption(argc, argv, "--drain-seconds", defaultDrainSeconds);
    const std::string serviceNameText = getOption(argc, argv, "--service", "/HELLO");
    const std::string outputCsv = getOption(argc, argv, "--output-csv", "");
    const std::string benchmarkStrategyText = getOption(argc, argv, "--strategy", "custom-selection");
    const std::string expectedResponse = getOption(argc, argv, "--expect-response", "");
    const std::string bootstrapToken = getOption(argc, argv, "--bootstrap-token", "");
    const auto knownProviders =
      parseKnownProviderIds(getOption(argc, argv, "--known-provider-ids", ""));

    auto userCert = getOrCreateIdentity(keyChain, USER_IDENTITY);
    auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);
    if (!bootstrapToken.empty()) {
      userCert = ndn_service_framework::ensureControllerSignedCertificate(
        face, keyChain, CONTROLLER_PREFIX, USER_IDENTITY, bootstrapToken);
    }
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(USER_IDENTITY));
    getOrCreateIdentity(keyChain, PROVIDER_IDENTITY);
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("A"));
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("B"));
    getOrCreateIdentity(keyChain, ndn::Name(PROVIDER_IDENTITY).append("C"));
    keyChainInitLock.unlock();

    std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
    if (serveCertificates) {
      certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
        face, keyChain, userCert.getName());
    }

    ndn_service_framework::ServiceUser user(
      face,
      GROUP_PREFIX,
      userCert,
      controllerCert,
      "examples/trust-schema.conf");

    user.init();
    user.setPerformanceMode(performanceMode);
    if (handlerThreads >= 0) {
      user.setHandlerThreads(static_cast<size_t>(handlerThreads));
    }
    user.setUseTokens(useTokens);
    user.setTimelineTrace(timelineTrace);
    ndn_service_framework::ServiceUser::AdaptiveAdmissionOptions runtimeAdmission;
    runtimeAdmission.enabled = adaptiveAdmission.enabled;
    runtimeAdmission.minWindow = adaptiveAdmission.minWindow;
    runtimeAdmission.maxWindow = adaptiveAdmission.maxWindow;
    runtimeAdmission.initialWindow = adaptiveAdmission.initialWindow;
    runtimeAdmission.hardInflightLimit = adaptiveAdmission.hardInflightLimit;
    runtimeAdmission.aiStep = adaptiveAdmission.aiStep;
    runtimeAdmission.mdFactor = adaptiveAdmission.mdFactor;
    runtimeAdmission.severeMdFactor = adaptiveAdmission.severeMdFactor;
    runtimeAdmission.controlIntervalMs = adaptiveAdmission.controlIntervalMs;
    runtimeAdmission.targetLatencyMs = adaptiveAdmission.targetLatencyMs;
    runtimeAdmission.hardTargetLatencyMs = adaptiveAdmission.hardTargetLatencyMs;
    runtimeAdmission.softQueueLimit = adaptiveAdmission.softQueueLimit;
    runtimeAdmission.hardQueueLimit = adaptiveAdmission.hardQueueLimit;
    user.setAdaptiveAdmissionControl(runtimeAdmission);
    NDN_LOG_INFO( "[App_User] token_mode="
              << (user.getUseTokens() ? "enabled" : "disabled")
              << " hybridMessageCrypto=enabled"
              << " timelineTrace=" << timelineTrace
              << " handlerThreads=" << user.getHandlerThreads()
              << " adaptiveAdmission=" << adaptiveAdmission.enabled
              << " adaptiveWindow=" << user.getAdaptiveAdmissionWindow()
              << " adaptiveSoftQueueLimit="
              << adaptiveQueueLimitsForWindow(user.getAdaptiveAdmissionWindow(),
                                              adaptiveAdmission.softQueueLimit,
                                              adaptiveAdmission.hardQueueLimit).first
              << " adaptiveHardQueueLimit="
              << adaptiveQueueLimitsForWindow(user.getAdaptiveAdmissionWindow(),
                                              adaptiveAdmission.softQueueLimit,
                                              adaptiveAdmission.hardQueueLimit).second
             );
    user.fetchPermissionsFromController(CONTROLLER_PREFIX);

    int exitCode = 0;

    if (largeDataPublishTest) {
      scheduler.schedule(ndn::time::seconds(2), [&] {
        const ndn::Name serviceName("/HELLO");
        const auto ctx = user.prepareServiceRequest(serviceName.toUri());
        const std::vector<uint8_t> plaintext(largeDataPlaintext.begin(),
                                             largeDataPlaintext.end());
        const auto result = user.publishEncryptedLargeData(ctx, plaintext, "image");
        if (!result.success) {
          std::cerr << "LARGE_DATA_PUBLISH_FAILURE error="
                    << result.errorMessage << std::endl;
          exitCode = 1;
          face.getIoContext().stop();
          return;
        }

        NDN_LOG_INFO( "LARGE_DATA_PUBLISH_SUCCESS name="
                  << result.encryptedDataName.toUri()
                  << " requestId=" << ctx.requestId.toUri()
                  << " objectId=" << result.objectId
                 );

        if (!largeDataNameFile.empty()) {
          std::ofstream output(largeDataNameFile);
          if (!output) {
            std::cerr << "LARGE_DATA_PUBLISH_FAILURE error=failed to open name file "
                      << largeDataNameFile << std::endl;
            exitCode = 2;
            face.getIoContext().stop();
            return;
          }
          output << result.encryptedDataName.toUri() << std::endl;
        }

        NDN_LOG_INFO( "LARGE_DATA_PUBLISH_SERVING");
      });

      face.processEvents();
      return exitCode;
    }

    if (benchmark) {
      if (workloadMode == "open-loop") {
        if (rateRps <= 0.0 || openLoopDurationSeconds <= 0 ||
            maxOutstanding <= 0 || requestTimeoutMs <= 0 || drainSeconds < 0) {
          std::cerr << "Open-loop rate, duration, max outstanding, and request timeout must be positive; drain seconds must be non-negative"
                    << std::endl;
          return 2;
        }
        if (outputCsv.empty()) {
          std::cerr << "--output-csv is required in --benchmark mode" << std::endl;
          return 2;
        }

        auto csv = std::make_shared<std::ofstream>(outputCsv);
        if (!*csv) {
          std::cerr << "Failed to open benchmark CSV: " << outputCsv << std::endl;
          return 2;
        }
        *csv << "request_id,success,latency_ms,response_payload\n";
        std::shared_ptr<std::ofstream> lifecycleCsv;
        if (timelineTrace) {
          const auto lifecycleCsvPath =
            std::filesystem::path(outputCsv).parent_path() / "request_lifecycle.csv";
          lifecycleCsv = std::make_shared<std::ofstream>(lifecycleCsvPath);
        }
        if (lifecycleCsv && *lifecycleCsv) {
          const auto lifecycleSampleRate =
            envSizeOption("NDNSF_TIMELINE_TRACE_SAMPLE_RATE", 100);
          *lifecycleCsv
            << "application_task_id,request_id,service_name,state,selected_provider,"
            << "enqueue_timestamp_us,admission_timestamp_us,publish_timestamp_us,"
            << "ack_matched_timestamp_us,provider_selection_timestamp_us,"
            << "selection_publish_timestamp_us,response_observed_timestamp_us,"
            << "response_decrypted_timestamp_us,callback_timestamp_us,"
            << "completion_timestamp_us,timeout_timestamp_us,queued_duration_ms,"
            << "inflight_duration_ms,end_to_end_latency_ms,"
            << "delayed_by_admission_control,final_cleanup_reason\n";
          user.setRequestLifecycleCallback(
            [lifecycleCsv, lifecycleSampleRate](
              const ndn_service_framework::ServiceUser::RequestLifecycleStatus& status) {
              if (!sampleByRequestId(status.requestId, lifecycleSampleRate)) {
                return;
              }
              *lifecycleCsv
                << csvEscape(status.applicationTaskId) << ","
                << csvEscape(status.requestId.toUri()) << ","
                << csvEscape(status.serviceName.toUri()) << ","
                << ndn_service_framework::ServiceUser::requestLifecycleStateToString(status.state) << ","
                << csvEscape(status.selectedProviderName.empty() ? "-" : status.selectedProviderName.toUri()) << ","
                << status.enqueueTimestampUs << ","
                << status.admissionTimestampUs << ","
                << status.publishTimestampUs << ","
                << status.ackMatchedTimestampUs << ","
                << status.providerSelectionTimestampUs << ","
                << status.selectionPublishTimestampUs << ","
                << status.responseObservedTimestampUs << ","
                << status.responseDecryptedTimestampUs << ","
                << status.callbackTimestampUs << ","
                << status.completionTimestampUs << ","
                << status.timeoutTimestampUs << ","
                << std::fixed << std::setprecision(3)
                << status.queuedDurationMs << ","
                << status.inflightDurationMs << ","
                << status.endToEndLatencyMs << ","
                << status.delayedByAdmissionControl << ","
                << csvEscape(status.finalCleanupReason) << "\n";
            });
        }

        const ndn::Name benchmarkServiceName(serviceNameText);
        const bool useBenchmarkCustomSelection =
          benchmarkStrategyText == "custom-selection" || benchmarkStrategyText == "custom";
        const bool useBenchmarkRandomSelection =
          benchmarkStrategyText == "random-selection" || benchmarkStrategyText == "random";
        const size_t benchmarkStrategy = parseBenchmarkStrategy(benchmarkStrategyText);
        const auto intervalNs = static_cast<int64_t>(
          std::max(1.0, 1000000000.0 / rateRps));
        const auto startTime = std::make_shared<std::chrono::steady_clock::time_point>();
        const auto measurementStartAt =
          std::make_shared<std::chrono::steady_clock::time_point>();
        const auto stopSendingAt = std::make_shared<std::chrono::steady_clock::time_point>();
        const auto drainDeadline = std::make_shared<std::chrono::steady_clock::time_point>();
        const auto nextDueAt = std::make_shared<std::chrono::steady_clock::time_point>();
        const auto noAdmissionWarmupRequests = static_cast<uint64_t>(
          std::llround(std::max(0.0, rateRps * static_cast<double>(benchmarkWarmup))));
        const auto noAdmissionMeasuredRequests = static_cast<uint64_t>(
          std::llround(std::max(1.0, rateRps * static_cast<double>(openLoopDurationSeconds))));
        const auto noAdmissionTargetRequests =
          noAdmissionWarmupRequests + noAdmissionMeasuredRequests;
        auto nextSequence = std::make_shared<uint64_t>(0);
        auto sendStopped = std::make_shared<bool>(false);
        auto states = std::make_shared<std::map<std::string, std::shared_ptr<OpenLoopRequestState>>>();
        auto completedRequestIds = std::make_shared<std::set<std::string>>();
        auto admissionRejectedRequestIds = std::make_shared<std::set<std::string>>();
        auto sentCount = std::make_shared<uint64_t>(0);
        auto firstRequestSentAt =
          std::make_shared<std::chrono::steady_clock::time_point>();
        auto lastRequestSentAt =
          std::make_shared<std::chrono::steady_clock::time_point>();
        auto successCount = std::make_shared<uint64_t>(0);
        auto timeoutCount = std::make_shared<uint64_t>(0);
        auto lateResponseCount = std::make_shared<uint64_t>(0);
        auto outstandingLimitSkips = std::make_shared<uint64_t>(0);
        auto latencies = std::make_shared<std::vector<double>>();
        auto outstandingSamples = std::make_shared<std::vector<size_t>>();
        auto queuedSamples = std::make_shared<std::vector<size_t>>();
        auto windowSamples = std::make_shared<std::vector<size_t>>();
        auto maxOutstandingObserved = std::make_shared<size_t>(0);
        auto maxQueuedObserved = std::make_shared<size_t>(0);
        auto queuedTasks = std::make_shared<size_t>(0);
        auto adaptiveWindow = std::make_shared<size_t>(
          adaptiveAdmission.enabled ? adaptiveAdmission.initialWindow :
          static_cast<size_t>(maxOutstanding));
        auto minAdaptiveWindowObserved = std::make_shared<size_t>(*adaptiveWindow);
        auto maxAdaptiveWindowObserved = std::make_shared<size_t>(*adaptiveWindow);
        auto delayedPublications = std::make_shared<uint64_t>(0);
        auto admissionControlWarnings = std::make_shared<uint64_t>(0);
        auto admissionControlRejects = std::make_shared<uint64_t>(0);
        auto minAdmissionHardQueueRemaining = std::make_shared<size_t>(
          adaptiveQueueLimitsForWindow(*adaptiveWindow,
                                       adaptiveAdmission.softQueueLimit,
                                       adaptiveAdmission.hardQueueLimit).second);
        auto admissionBackoffUntil = std::make_shared<std::chrono::steady_clock::time_point>(
          std::chrono::steady_clock::time_point::min());
        auto admissionBackoffEvents = std::make_shared<uint64_t>(0);
        auto admissionWarningBackoffEvents = std::make_shared<uint64_t>(0);
        auto admissionRejectBackoffEvents = std::make_shared<uint64_t>(0);
        auto totalAdmissionBackoffMs = std::make_shared<uint64_t>(0);
        auto admissionQueuePauseActive = std::make_shared<bool>(false);
        auto admissionQueuePauseReason = std::make_shared<std::string>();
        auto admissionQueuePauseResumeDepth = std::make_shared<size_t>(0);
        auto admissionQueuePauseStartedAt =
          std::make_shared<std::chrono::steady_clock::time_point>();
        auto admissionQueuePauseEvents = std::make_shared<uint64_t>(0);
        auto admissionQueuePauseResumes = std::make_shared<uint64_t>(0);
        auto admissionQueuePauseSkips = std::make_shared<uint64_t>(0);
        auto admissionWarningQueuePauses = std::make_shared<uint64_t>(0);
        auto admissionRejectQueuePauses = std::make_shared<uint64_t>(0);
        auto totalAdmissionQueuePausedMs = std::make_shared<uint64_t>(0);
        auto nextAdmissionRecommendedSendAt =
          std::make_shared<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::time_point::min());
        auto admissionRecommendedRateSkips = std::make_shared<uint64_t>(0);
        auto admissionRecommendedRateMin =
          std::make_shared<double>(std::numeric_limits<double>::max());
        auto admissionRecommendedRateMax = std::make_shared<double>(0.0);
        auto intervalTimeouts = std::make_shared<uint64_t>(0);
        auto intervalLateResponses = std::make_shared<uint64_t>(0);
        auto intervalSuccesses = std::make_shared<uint64_t>(0);
        auto intervalLatencies = std::make_shared<std::vector<double>>();
        auto ackLatencySamples = std::make_shared<std::vector<double>>();
        std::shared_ptr<const ndnsf::AckSelectionPolicy> benchmarkSelectionPolicy;
        if (useBenchmarkCustomSelection) {
          benchmarkSelectionPolicy =
            makeRankQueueSelectionPolicy(ackTimeoutMs, performanceMode);
        }
        else if (useBenchmarkRandomSelection ||
                 benchmarkStrategy == ndn_service_framework::tlv::RandomSelection) {
          benchmarkSelectionPolicy = ndnsf::strategy::RandomSelection;
        }
        else if (benchmarkStrategy == ndn_service_framework::tlv::AllSelected) {
          benchmarkSelectionPolicy = ndnsf::strategy::AllSelected;
        }
        else {
          benchmarkSelectionPolicy = ndnsf::strategy::FirstResponding;
        }
        auto paused = std::make_shared<bool>(false);
        auto pauseCount = std::make_shared<uint64_t>(0);
        auto pauseTransitions = std::make_shared<uint64_t>(0);
        auto totalPausedMs = std::make_shared<uint64_t>(0);
        auto pauseStartedAt = std::make_shared<std::chrono::steady_clock::time_point>();
        auto highInflightNoSuccessIntervals = std::make_shared<size_t>(0);
        auto pauseReasonCounters = std::make_shared<std::map<std::string, uint64_t>>();
        auto sampleOutstanding = std::make_shared<std::function<void()>>();
        auto sampleAdaptive = std::make_shared<std::function<void()>>();
        auto maybePause = std::make_shared<std::function<void(const std::string&)>>();
        auto maybeResume = std::make_shared<std::function<void()>>();
        auto controlAdaptiveWindow = std::make_shared<std::function<void()>>();
        auto sendNext = std::make_shared<std::function<void()>>();
        auto maybeFinish = std::make_shared<std::function<void()>>();

        user.setAdmissionControlWarningHandler(
          [admissionControlWarnings,
           minAdmissionHardQueueRemaining,
           admissionBackoffUntil,
           admissionBackoffEvents,
           admissionWarningBackoffEvents,
           totalAdmissionBackoffMs,
           admissionQueuePauseActive,
           admissionQueuePauseReason,
           admissionQueuePauseResumeDepth,
           admissionQueuePauseStartedAt,
           admissionQueuePauseEvents,
           admissionWarningQueuePauses,
           adaptiveAdmission,
           performanceMode,
           perfLogGate](
            const ndn_service_framework::ServiceUser::AdmissionControlStatus& status) {
            ++(*admissionControlWarnings);
            *minAdmissionHardQueueRemaining =
              std::min(*minAdmissionHardQueueRemaining, status.remainingHardSlots);
            if (adaptiveAdmission.warningBackoffMs > 0) {
              const auto now = std::chrono::steady_clock::now();
              const auto until = now + std::chrono::milliseconds(
                adaptiveAdmission.warningBackoffMs);
              if (until > *admissionBackoffUntil) {
                *admissionBackoffUntil = until;
              }
              ++(*admissionBackoffEvents);
              ++(*admissionWarningBackoffEvents);
              *totalAdmissionBackoffMs +=
                static_cast<uint64_t>(adaptiveAdmission.warningBackoffMs);
            }
            // Soft-limit warnings are advisory: record them and apply the
            // optional warning backoff above, but keep admission flowing.
            if (!performanceMode && perfLogGate->allow()) {
              NDN_LOG_TRACE( "PERF_ADMISSION_WARNING"
                        << " depth=" << status.queueDepth
                        << " soft_limit=" << status.softQueueLimit
                        << " hard_limit=" << status.hardQueueLimit
                        << " remaining_hard=" << status.remainingHardSlots
                        << " queue_pause=0"
                        << " resume_depth="
                        << adaptiveAdmission.warningResumeQueueDepth
                        << " reason=" << status.reason
                        << " request_id=" << status.requestId.toUri()
                        << " ts=" << nowMilliseconds());
            }
          });
        user.setAdmissionControlRejectHandler(
          [admissionControlRejects,
           minAdmissionHardQueueRemaining,
           admissionRejectedRequestIds,
           admissionBackoffUntil,
           admissionBackoffEvents,
           admissionRejectBackoffEvents,
           totalAdmissionBackoffMs,
           admissionQueuePauseActive,
           admissionQueuePauseReason,
           admissionQueuePauseResumeDepth,
           admissionQueuePauseStartedAt,
           admissionQueuePauseEvents,
           admissionRejectQueuePauses,
           states,
           completedRequestIds,
           adaptiveAdmission,
           performanceMode,
           perfLogGate](
            const ndn_service_framework::ServiceUser::AdmissionControlStatus& status) {
            ++(*admissionControlRejects);
            *minAdmissionHardQueueRemaining =
              std::min(*minAdmissionHardQueueRemaining, status.remainingHardSlots);
            const std::string requestIdText = status.requestId.toUri();
            admissionRejectedRequestIds->insert(requestIdText);
            states->erase(requestIdText);
            completedRequestIds->insert(requestIdText);
            const bool hardReject = status.reason != "admission_window_full";
            if (hardReject && adaptiveAdmission.rejectBackoffMs > 0) {
              const auto now = std::chrono::steady_clock::now();
              const auto until = now + std::chrono::milliseconds(
                adaptiveAdmission.rejectBackoffMs);
              if (until > *admissionBackoffUntil) {
                *admissionBackoffUntil = until;
              }
              ++(*admissionBackoffEvents);
              ++(*admissionRejectBackoffEvents);
              *totalAdmissionBackoffMs +=
                static_cast<uint64_t>(adaptiveAdmission.rejectBackoffMs);
            }
            if (hardReject && adaptiveAdmission.queueAwarePause) {
              if (!*admissionQueuePauseActive) {
                ++(*admissionQueuePauseEvents);
                *admissionQueuePauseStartedAt = std::chrono::steady_clock::now();
              }
              *admissionQueuePauseActive = true;
              *admissionQueuePauseReason = "reject";
              *admissionQueuePauseResumeDepth =
                adaptiveAdmission.rejectResumeQueueDepth;
              ++(*admissionRejectQueuePauses);
            }
            if (!performanceMode && perfLogGate->allow()) {
              NDN_LOG_TRACE( "PERF_ADMISSION_REJECT"
                        << " depth=" << status.queueDepth
                        << " soft_limit=" << status.softQueueLimit
                        << " hard_limit=" << status.hardQueueLimit
                        << " remaining_hard=" << status.remainingHardSlots
                        << " queue_pause=" << (adaptiveAdmission.queueAwarePause ? 1 : 0)
                        << " resume_depth="
                        << adaptiveAdmission.rejectResumeQueueDepth
                        << " reason=" << status.reason
                        << " request_id=" << status.requestId.toUri()
                        << " ts=" << nowMilliseconds());
            }
          });

        *sampleOutstanding = [states, outstandingSamples, maxOutstandingObserved]() {
          const auto current = states->size();
          outstandingSamples->push_back(current);
          *maxOutstandingObserved = std::max(*maxOutstandingObserved, current);
        };

        *sampleAdaptive = [&, states, queuedTasks, adaptiveWindow, queuedSamples,
                           windowSamples, maxQueuedObserved,
                           minAdaptiveWindowObserved, maxAdaptiveWindowObserved]() {
          const auto currentQueued = adaptiveAdmission.enabled ?
            user.getAdaptiveAdmissionQueueDepth() : *queuedTasks;
          const auto currentWindow = adaptiveAdmission.enabled ?
            user.getAdaptiveAdmissionWindow() : *adaptiveWindow;
          queuedSamples->push_back(currentQueued);
          windowSamples->push_back(currentWindow);
          *maxQueuedObserved = std::max(*maxQueuedObserved, currentQueued);
          *minAdaptiveWindowObserved = std::min(*minAdaptiveWindowObserved, currentWindow);
          *maxAdaptiveWindowObserved = std::max(*maxAdaptiveWindowObserved, currentWindow);
          (void)states;
        };

        auto pauseThreshold = [&adaptiveAdmission, adaptiveWindow]() {
          const auto hardWatermark = static_cast<size_t>(std::ceil(
            static_cast<double>(adaptiveAdmission.hardInflightLimit) * 0.9));
          return std::max<size_t>(1, std::min(*adaptiveWindow, hardWatermark));
        };
        auto resumeThreshold = [&pauseThreshold]() {
          const auto threshold = pauseThreshold();
          return threshold <= 1 ? size_t(0) :
            std::max<size_t>(1, static_cast<size_t>(std::floor(
              static_cast<double>(threshold) * 0.6)));
        };

        *maybePause = [&, paused, pauseCount, pauseTransitions, pauseStartedAt,
                       pauseReasonCounters, states, queuedTasks, adaptiveWindow,
                       pauseThreshold](const std::string& reason) {
          ++((*pauseReasonCounters)[reason]);
          if (*paused) {
            return;
          }
          *paused = true;
          ++(*pauseCount);
          ++(*pauseTransitions);
          *pauseStartedAt = std::chrono::steady_clock::now();
          NDN_LOG_TRACE( "PERF_ADMISSION_PAUSED reason=" << reason
                    << " inflight=" << states->size()
                    << " queued=" << *queuedTasks
                    << " adaptive_window=" << *adaptiveWindow
                    << " hard_inflight_limit=" << adaptiveAdmission.hardInflightLimit
                    << " pause_threshold=" << pauseThreshold()
                    << " ts=" << nowMilliseconds());
        };

        *maybeResume = [&, paused, pauseTransitions, totalPausedMs, pauseStartedAt,
                        states, queuedTasks, adaptiveWindow, resumeThreshold]() {
          if (!*paused || states->size() > resumeThreshold()) {
            return;
          }
          *paused = false;
          ++(*pauseTransitions);
          const auto pausedForMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - *pauseStartedAt).count();
          if (pausedForMs > 0) {
            *totalPausedMs += static_cast<uint64_t>(pausedForMs);
          }
          NDN_LOG_TRACE( "PERF_ADMISSION_RESUMED"
                    << " inflight=" << states->size()
                    << " queued=" << *queuedTasks
                    << " adaptive_window=" << *adaptiveWindow
                    << " resume_threshold=" << resumeThreshold()
                    << " paused_ms=" << pausedForMs
                    << " ts=" << nowMilliseconds());
        };

        *maybeFinish = [&, csv, states, sendStopped, drainDeadline, sentCount, successCount,
                        timeoutCount, lateResponseCount, outstandingLimitSkips, latencies,
                        outstandingSamples, maxOutstandingObserved, measurementStartAt,
                        stopSendingAt, rateRps, maybeFinish]() {
          if (!*sendStopped) {
            return;
          }
          if (!states->empty() && std::chrono::steady_clock::now() < *drainDeadline) {
            scheduler.schedule(ndn::time::milliseconds(100), [maybeFinish] { (*maybeFinish)(); });
            return;
          }

          double avg = 0.0;
          if (!latencies->empty()) {
            avg = std::accumulate(latencies->begin(), latencies->end(), 0.0) /
                  static_cast<double>(latencies->size());
          }
          const double configuredDurationSeconds =
            std::chrono::duration<double>(*stopSendingAt - *measurementStartAt).count();
          double sendWindowSeconds = 0.0;
          if (*sentCount >= 2) {
            sendWindowSeconds =
              std::chrono::duration<double>(*lastRequestSentAt - *firstRequestSentAt).count();
          }
          const double achievedRps = sendWindowSeconds > 0.0 ?
            static_cast<double>(*sentCount - 1) / sendWindowSeconds :
            (configuredDurationSeconds > 0.0 ?
             static_cast<double>(*sentCount) / configuredDurationSeconds : 0.0);
          uint64_t reportedPausedMs = *totalPausedMs;
          if (*paused) {
            const auto pausedForMs = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - *pauseStartedAt).count();
            if (pausedForMs > 0) {
              reportedPausedMs += static_cast<uint64_t>(pausedForMs);
            }
          }
          uint64_t reportedAdmissionQueuePausedMs = *totalAdmissionQueuePausedMs;
          if (*admissionQueuePauseActive) {
            const auto pausedForMs = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() -
              *admissionQueuePauseStartedAt).count();
            if (pausedForMs > 0) {
              reportedAdmissionQueuePausedMs +=
                static_cast<uint64_t>(pausedForMs);
            }
          }
          const auto summaryQueueLimits = adaptiveQueueLimitsForWindow(
            user.getAdaptiveAdmissionWindow(),
            adaptiveAdmission.softQueueLimit,
            adaptiveAdmission.hardQueueLimit);

          NDN_LOG_WARN( std::fixed << std::setprecision(3)
                    << "OPEN_LOOP_SUMMARY sent=" << *sentCount
                    << " success=" << *successCount
                    << " timeout=" << *timeoutCount
                    << " late_response=" << *lateResponseCount
                    << " outstanding_limit_skips=" << *outstandingLimitSkips
                    << " user_delayed_publications=" << *delayedPublications
                    << " queued_remaining=" << *queuedTasks
                    << " queued_p50=" << percentileSize(*queuedSamples, 50.0)
                    << " queued_p95=" << percentileSize(*queuedSamples, 95.0)
                    << " queued_max=" << *maxQueuedObserved
                    << " adaptive_enabled=" << adaptiveAdmission.enabled
                    << " adaptive_window_p50=" << percentileSize(*windowSamples, 50.0)
                    << " adaptive_window_p95=" << percentileSize(*windowSamples, 95.0)
                    << " adaptive_window_min=" << *minAdaptiveWindowObserved
                    << " adaptive_window_max=" << *maxAdaptiveWindowObserved
                    << " hard_inflight_limit=" << adaptiveAdmission.hardInflightLimit
                    << " admission_soft_queue_limit=" << summaryQueueLimits.first
                    << " admission_hard_queue_limit=" << summaryQueueLimits.second
                    << " admission_control_warnings=" << *admissionControlWarnings
                    << " admission_control_rejects=" << *admissionControlRejects
                    << " admission_hard_queue_min_remaining="
                    << *minAdmissionHardQueueRemaining
                    << " admission_backoff_events=" << *admissionBackoffEvents
                    << " admission_warning_backoff_events="
                    << *admissionWarningBackoffEvents
                    << " admission_reject_backoff_events="
                    << *admissionRejectBackoffEvents
                    << " total_admission_backoff_ms="
                    << *totalAdmissionBackoffMs
                    << " admission_queue_pause_enabled="
                    << adaptiveAdmission.queueAwarePause
                    << " admission_warning_resume_queue_depth="
                    << adaptiveAdmission.warningResumeQueueDepth
                    << " admission_reject_resume_queue_depth="
                    << adaptiveAdmission.rejectResumeQueueDepth
                    << " admission_queue_pause_events="
                    << *admissionQueuePauseEvents
                    << " admission_queue_pause_resumes="
                    << *admissionQueuePauseResumes
                    << " admission_queue_pause_skips="
                    << *admissionQueuePauseSkips
                    << " admission_warning_queue_pauses="
                    << *admissionWarningQueuePauses
                    << " admission_reject_queue_pauses="
                    << *admissionRejectQueuePauses
                    << " total_admission_queue_paused_ms="
                    << reportedAdmissionQueuePausedMs
                    << " admission_queue_pause_active="
                    << *admissionQueuePauseActive
                    << " admission_recommended_rate_skips="
                    << *admissionRecommendedRateSkips
                    << " admission_recommended_rate_min="
                    << (*admissionRecommendedRateMin == std::numeric_limits<double>::max() ?
                        0.0 : *admissionRecommendedRateMin)
                    << " admission_recommended_rate_max="
                    << *admissionRecommendedRateMax
                    << " admission_recommended_rate_final="
                    << user.getAdaptiveAdmissionRecommendedRateRps()
                    << " pause_count=" << *pauseCount
                    << " total_paused_ms=" << reportedPausedMs
                    << " paused_state=" << *paused
                    << " paused_state_transitions=" << *pauseTransitions
                    << " pause_reason_counters=" << formatCounters(*pauseReasonCounters)
                    << " in_flight_p50=" << medianSize(*outstandingSamples)
                    << " in_flight_p95=" << percentileSize(*outstandingSamples, 95.0)
                    << " in_flight_max=" << *maxOutstandingObserved
                    << " target_rps=" << rateRps
                    << " achieved_rps=" << achievedRps
                    << " warmup_s=" << benchmarkWarmup
                    << " send_window_s=" << sendWindowSeconds
                    << " remaining_outstanding=" << states->size()
                    << " median_outstanding=" << medianSize(*outstandingSamples)
                    << " p95_outstanding=" << percentileSize(*outstandingSamples, 95.0)
                    << " max_outstanding_observed=" << *maxOutstandingObserved
                    << " avg_ms=" << avg
                    << " p50_ms=" << percentile(*latencies, 50.0)
                    << " p95_ms=" << percentile(*latencies, 95.0)
                    << " p99_ms=" << percentile(*latencies, 99.0)
                    << " ack_p95_ms=" << percentile(*ackLatencySamples, 95.0)
                    << " csv=" << outputCsv
                   );
          csv->flush();
          if (lifecycleCsv && *lifecycleCsv) {
            lifecycleCsv->flush();
          }
          exitCode = 0;
          face.getIoContext().stop();
        };

        *sendNext = [&, csv, benchmarkServiceName, useBenchmarkCustomSelection,
                     useBenchmarkRandomSelection, benchmarkStrategy,
                     intervalNs, startTime, measurementStartAt, stopSendingAt,
                     drainDeadline, nextDueAt, nextSequence,
                     sendStopped, states, completedRequestIds, sentCount, successCount,
                     timeoutCount, lateResponseCount, outstandingLimitSkips, latencies,
                     sampleOutstanding, sendNext, maybeFinish]() {
          const auto now = std::chrono::steady_clock::now();
          const bool generating = adaptiveAdmission.enabled ?
            now < *stopSendingAt : *nextSequence < noAdmissionTargetRequests;
          if (!generating &&
              (!adaptiveAdmission.enabled || *queuedTasks == 0 ||
               now >= *drainDeadline)) {
            *sendStopped = true;
            (*maybeFinish)();
            return;
          }

          auto scheduleNext = [&](std::chrono::nanoseconds delay) {
            scheduler.schedule(ndn::time::nanoseconds(delay.count()),
                               [sendNext] { (*sendNext)(); });
          };
          const auto retryDelay = std::chrono::nanoseconds(std::chrono::milliseconds(10));
          auto accrueOpenLoopTicks = [&]() {
            if (!generating) {
              return;
            }
            if (!adaptiveAdmission.enabled) {
              if (*queuedTasks == 0) {
                *queuedTasks = 1;
                (*sampleAdaptive)();
              }
              return;
            }
            uint64_t dueExclusive = *nextSequence;
            if (intervalNs > 0) {
              const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - *startTime).count();
              if (elapsedNs > 0) {
                dueExclusive = std::max<uint64_t>(
                  dueExclusive,
                  static_cast<uint64_t>(elapsedNs / intervalNs) + 1);
              }
            }
            if (dueExclusive <= *nextSequence) {
              return;
            }
            const uint64_t dueTicks = dueExclusive - *nextSequence;
            const auto currentQueueLimits = adaptiveQueueLimitsForWindow(
              user.getAdaptiveAdmissionWindow(),
              adaptiveAdmission.softQueueLimit,
              adaptiveAdmission.hardQueueLimit);
            const size_t creditLimit = adaptiveAdmission.enabled ?
              std::max<size_t>(
                4,
                std::min<size_t>(
                  currentQueueLimits.second,
                  static_cast<size_t>(std::ceil(std::max(1.0, rateRps) * 0.25)))) :
              static_cast<size_t>(maxOutstanding);
            const size_t room = *queuedTasks >= creditLimit ?
              0 : creditLimit - *queuedTasks;
            const uint64_t perDispatchLimit = 1;
            const size_t acceptedTicks = static_cast<size_t>(
              std::min<uint64_t>(
                dueTicks,
                std::min<uint64_t>(static_cast<uint64_t>(room),
                                   perDispatchLimit)));
            if (acceptedTicks > 0) {
              *queuedTasks += acceptedTicks;
              (*sampleAdaptive)();
            }
            if (dueTicks > static_cast<uint64_t>(acceptedTicks)) {
              *delayedPublications +=
                static_cast<uint64_t>(dueTicks - acceptedTicks);
            }
            *nextSequence = dueExclusive;
          };
        auto delayUntilNextOpenLoopTick = [&]() {
          if (!generating || intervalNs <= 0) {
            return retryDelay;
          }
          if (!adaptiveAdmission.enabled) {
            return std::max(std::chrono::nanoseconds(0),
                            *nextDueAt - std::chrono::steady_clock::now());
          }
            const auto nextDue = *startTime +
              std::chrono::nanoseconds(intervalNs * static_cast<int64_t>(*nextSequence)) +
              openLoopPacingJitter(*nextSequence, openLoopPacingJitterUs);
            return std::max(std::chrono::nanoseconds(0),
                            nextDue - std::chrono::steady_clock::now());
          };

          (*maybeResume)();
          accrueOpenLoopTicks();
          if (adaptiveAdmission.enabled && adaptiveAdmission.useRecommendedRate) {
            const double recommendedRate = user.getAdaptiveAdmissionRecommendedRateRps();
            if (recommendedRate > 0.0) {
              *admissionRecommendedRateMin =
                std::min(*admissionRecommendedRateMin, recommendedRate);
              *admissionRecommendedRateMax =
                std::max(*admissionRecommendedRateMax, recommendedRate);
            }
            if (recommendedRate > 0.0 && recommendedRate < rateRps &&
                now < *nextAdmissionRecommendedSendAt) {
              const size_t runtimeQueueDepth =
                user.getAdaptiveAdmissionQueueDepth();
              const size_t runtimeWindow =
                std::max<size_t>(1, user.getAdaptiveAdmissionWindow());
              const size_t runtimeInflight =
                user.getAdaptiveAdmissionInflight();
              const auto runtimeQueueLimits = adaptiveQueueLimitsForWindow(
                runtimeWindow,
                adaptiveAdmission.softQueueLimit,
                adaptiveAdmission.hardQueueLimit);
              const bool queuePressure =
                runtimeQueueDepth >= runtimeQueueLimits.first;
              const bool inflightQueuePressure =
                runtimeInflight >= runtimeWindow &&
                runtimeQueueDepth >= adaptiveAdmission.warningResumeQueueDepth;
              const bool enforceSoftPacing =
                *admissionQueuePauseActive || queuePressure || inflightQueuePressure;
              if (!enforceSoftPacing) {
                *nextAdmissionRecommendedSendAt =
                  std::chrono::steady_clock::time_point::min();
              }
              else {
              ++(*delayedPublications);
              ++(*admissionRecommendedRateSkips);
              user.recordAdaptiveAdmissionBackpressure();
              (*sampleOutstanding)();
              (*sampleAdaptive)();
              if (!performanceMode && perfLogGate->allow()) {
                const auto remainingMs =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                    *nextAdmissionRecommendedSendAt - now).count();
                NDN_LOG_TRACE( "PERF_ADMISSION_RECOMMENDED_RATE_PAUSED"
                          << " recommended_rps=" << recommendedRate
                          << " offered_rps=" << rateRps
                          << " remaining_ms=" << remainingMs
                          << " queue_depth=" << runtimeQueueDepth
                          << " inflight=" << runtimeInflight
                          << " adaptive_window=" << runtimeWindow
                          << " queued_credit=" << *queuedTasks
                          << " ts=" << nowMilliseconds());
              }
              scheduleNext(*nextAdmissionRecommendedSendAt - now);
              return;
              }
            }
          }
          if (adaptiveAdmission.enabled && adaptiveAdmission.queueAwarePause &&
              *admissionQueuePauseActive) {
            const size_t runtimeQueueDepth = user.getAdaptiveAdmissionQueueDepth();
            const bool queueRecovered =
              runtimeQueueDepth <= *admissionQueuePauseResumeDepth;
            const bool minBackoffElapsed = now >= *admissionBackoffUntil;
            if (queueRecovered && minBackoffElapsed) {
              *admissionQueuePauseActive = false;
              ++(*admissionQueuePauseResumes);
              const auto pausedForMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() -
                  *admissionQueuePauseStartedAt).count();
              if (pausedForMs > 0) {
                *totalAdmissionQueuePausedMs +=
                  static_cast<uint64_t>(pausedForMs);
              }
              if (!performanceMode && perfLogGate->allow()) {
                NDN_LOG_TRACE( "PERF_ADMISSION_QUEUE_RESUMED"
                          << " reason=" << *admissionQueuePauseReason
                          << " queue_depth=" << runtimeQueueDepth
                          << " resume_depth=" << *admissionQueuePauseResumeDepth
                          << " paused_ms=" << pausedForMs
                          << " ts=" << nowMilliseconds());
              }
            }
            else {
              ++(*delayedPublications);
              ++(*admissionQueuePauseSkips);
              (*sampleOutstanding)();
              (*sampleAdaptive)();
              if (!performanceMode && perfLogGate->allow()) {
                const auto remainingBackoffMs = minBackoffElapsed ? 0 :
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                    *admissionBackoffUntil - now).count();
                NDN_LOG_TRACE( "PERF_ADMISSION_QUEUE_PAUSED"
                          << " reason=" << *admissionQueuePauseReason
                          << " queue_depth=" << runtimeQueueDepth
                          << " resume_depth=" << *admissionQueuePauseResumeDepth
                          << " remaining_backoff_ms=" << remainingBackoffMs
                          << " queued_credit=" << *queuedTasks
                          << " ts=" << nowMilliseconds());
              }
              scheduleNext(std::chrono::milliseconds(
                adaptiveAdmission.queuePausePollMs));
              return;
            }
          }
          if (adaptiveAdmission.enabled && now < *admissionBackoffUntil) {
            ++(*delayedPublications);
            (*sampleOutstanding)();
            (*sampleAdaptive)();
            if (!performanceMode && perfLogGate->allow()) {
              const auto backoffMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                *admissionBackoffUntil - now).count();
              NDN_LOG_TRACE( "PERF_ADMISSION_BACKOFF"
                        << " remaining_ms=" << backoffMs
                        << " queued_credit=" << *queuedTasks
                        << " ts=" << nowMilliseconds());
            }
            scheduleNext(*admissionBackoffUntil - now);
            return;
          }
          const auto runtimeWindow = user.getAdaptiveAdmissionWindow();
          const auto runtimeAdmissionInflight = user.getAdaptiveAdmissionInflight();
          const bool pendingResponseAtLimit =
            states->size() >= static_cast<size_t>(maxOutstanding);

          if (pendingResponseAtLimit) {
            ++(*outstandingLimitSkips);
            ++(*delayedPublications);
            (*sampleOutstanding)();
            (*sampleAdaptive)();
            if (!performanceMode && pendingResponseAtLimit) {
              NDN_LOG_TRACE( "PERF_OUTSTANDING_LIMIT_REACHED outstanding="
                        << states->size()
                        << " admission_inflight=" << runtimeAdmissionInflight
                        << " queued=" << *queuedTasks
                        << " adaptive_window=" << runtimeWindow
                        << " pending_response_limit=" << maxOutstanding
                        << " ts=" << nowMilliseconds());
            }
            scheduleNext(delayUntilNextOpenLoopTick());
            return;
          }

          bool scheduledNextTickBeforeSend = false;
          uint64_t currentSequence = *nextSequence;
          if (*queuedTasks > 0) {
            if (generating) {
              if (!adaptiveAdmission.enabled) {
                ++(*nextSequence);
                if (*nextSequence < noAdmissionTargetRequests) {
                  auto idealNextDue =
                    *startTime +
                    std::chrono::nanoseconds(intervalNs * static_cast<int64_t>(*nextSequence)) +
                    openLoopPacingJitter(*nextSequence, openLoopPacingJitterUs);
                  const auto boundedCatchUpSpacing = std::min(
                    std::chrono::nanoseconds(intervalNs),
                    std::chrono::nanoseconds(std::chrono::milliseconds(5)));
                  const auto minimumNextDue =
                    std::chrono::steady_clock::now() + boundedCatchUpSpacing;
                  if (idealNextDue < minimumNextDue) {
                    ++(*delayedPublications);
                    idealNextDue = minimumNextDue;
                  }
                  *nextDueAt = idealNextDue;
                  scheduleNext(std::max(std::chrono::nanoseconds(0),
                                        *nextDueAt - std::chrono::steady_clock::now()));
                  scheduledNextTickBeforeSend = true;
                }
              }
              else {
                scheduleNext(delayUntilNextOpenLoopTick());
                scheduledNextTickBeforeSend = true;
              }
            }
            const std::string requestText = "HELLO";
            ndn::Buffer requestPayload(
              reinterpret_cast<const uint8_t*>(requestText.data()),
              requestText.size());

            ndn_service_framework::RequestMessage request;
            request.setPayload(requestPayload, requestPayload.size());
            request.setStrategy((useBenchmarkCustomSelection || useBenchmarkRandomSelection) ?
                                static_cast<size_t>(ndn_service_framework::tlv::FirstResponding) :
                                benchmarkStrategy);

            auto state = std::make_shared<OpenLoopRequestState>();
            state->start = now;
            state->measured = adaptiveAdmission.enabled ?
              now >= *measurementStartAt : currentSequence >= noAdmissionWarmupRequests;

            auto onTimeout = std::function<void(const ndn::Name&)>(
              [&, csv, states, completedRequestIds, timeoutCount,
               intervalTimeouts, sampleOutstanding, sampleAdaptive](
                const ndn::Name& timedOutRequestId) {
                boost::asio::post(face.getIoContext(),
                  [&, csv, states, completedRequestIds, timeoutCount,
                   intervalTimeouts, sampleOutstanding, sampleAdaptive,
                   timedOutRequestId] {
                    const std::string requestIdText = timedOutRequestId.toUri();
                    if (completedRequestIds->find(requestIdText) != completedRequestIds->end()) {
                      return;
                    }
                    completedRequestIds->insert(requestIdText);

                    auto stateIt = states->find(requestIdText);
                    double latencyMs = 0.0;
                    bool measured = true;
                    if (stateIt != states->end()) {
                      latencyMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - stateIt->second->start).count();
                      measured = stateIt->second->measured;
                      states->erase(stateIt);
                    }
                    if (measured) {
                      ++(*timeoutCount);
                      ++(*intervalTimeouts);
                    }
                    // In no-admission benchmark mode, timeouts are recorded
                    // but must not trigger an additional app-side congestion
                    // pause; otherwise the "no admission" baseline is still
                    // self-throttled by the traffic generator.
                    if (measured) {
                      *csv << csvEscape(requestIdText) << ",0,"
                           << std::fixed << std::setprecision(3) << latencyMs << ",\n";
                    }
                    if (!performanceMode && perfLogGate->allow()) {
                      NDN_LOG_TRACE( "PERF_REQUEST_TIMEOUT id=" << requestIdText
                                << " ts=" << nowMilliseconds());
                    }
                    (*sampleOutstanding)();
                    (*sampleAdaptive)();
                    (*maybeResume)();
                  });
              });
            auto onResponse = std::function<void(const ndn_service_framework::ResponseMessage&)>(
              [&, csv, states, completedRequestIds, state, successCount, lateResponseCount,
               intervalLateResponses, intervalSuccesses, latencies, intervalLatencies,
               sampleOutstanding, sampleAdaptive](
                const ndn_service_framework::ResponseMessage& response) {
                const auto payload = response.getPayload();
                const std::string responseText(
                  reinterpret_cast<const char*>(payload.data()),
                  payload.size());
                boost::asio::post(face.getIoContext(),
                  [&, csv, states, completedRequestIds, state, successCount,
                   lateResponseCount, intervalLateResponses, intervalSuccesses,
                   latencies, intervalLatencies, sampleOutstanding, sampleAdaptive,
                   responseText] {
                    const std::string requestIdText = state->requestId;
                    if (requestIdText.empty()) {
                      return;
                    }
                    if (completedRequestIds->find(requestIdText) != completedRequestIds->end()) {
                      ++(*lateResponseCount);
                      ++(*intervalLateResponses);
                      // Same as timeout handling above: late responses are
                      // measured, not converted into admission-like pacing.
                      if (!performanceMode && perfLogGate->allow()) {
                        NDN_LOG_TRACE( "PERF_LATE_RESPONSE id=" << requestIdText
                                  << " ts=" << nowMilliseconds());
                      }
                      return;
                    }
                    completedRequestIds->insert(requestIdText);

                    const double latencyMs = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - state->start).count();
                    if (state->measured) {
                      ++(*successCount);
                      ++(*intervalSuccesses);
                      latencies->push_back(latencyMs);
                      intervalLatencies->push_back(latencyMs);
                      *csv << csvEscape(requestIdText) << ",1,"
                           << std::fixed << std::setprecision(3) << latencyMs << ","
                           << csvEscape(responseText) << "\n";
                    }
                    if (!performanceMode && perfLogGate->allow()) {
                      NDN_LOG_TRACE( "PERF_RESPONSE_RECEIVED id=" << requestIdText
                                << " provider=" << state->selectedProvider
                                << " latency_ms=" << std::fixed << std::setprecision(3)
                                << latencyMs
                                << " ts=" << nowMilliseconds());
                    }
                    states->erase(requestIdText);
                    (*sampleOutstanding)();
                    (*sampleAdaptive)();
                    (*maybeResume)();
                  });
              });

            ndn::Name requestId = user.RequestService(
              benchmarkServiceName,
              request.getPayload(),
              ackTimeoutMs,
              benchmarkSelectionPolicy,
              requestTimeoutMs,
              onResponse,
              onTimeout);

            state->requestId = requestId.toUri();
            const bool admissionRejected =
              admissionRejectedRequestIds->erase(state->requestId) > 0;
            if (admissionRejected || state->requestId.empty()) {
              if (*queuedTasks > 0) {
                --(*queuedTasks);
              }
              (*sampleOutstanding)();
              (*sampleAdaptive)();
              if (!performanceMode && !state->requestId.empty()) {
                NDN_LOG_TRACE( "PERF_REQUEST_ADMISSION_REJECTED id=" << state->requestId
                          << " ts=" << nowMilliseconds());
              }
              if (!scheduledNextTickBeforeSend) {
                scheduleNext(delayUntilNextOpenLoopTick());
              }
              return;
            }
            (*states)[state->requestId] = state;
            if (*queuedTasks > 0) {
              --(*queuedTasks);
            }
            if (*sentCount == 0) {
              *firstRequestSentAt = now;
            }
            *lastRequestSentAt = now;
            ++(*sentCount);
            (*sampleOutstanding)();
            (*sampleAdaptive)();
            if (!performanceMode && perfLogGate->allow()) {
              NDN_LOG_TRACE( "PERF_REQUEST_SENT id=" << state->requestId
                        << " ts=" << nowMilliseconds());
            }
            if (adaptiveAdmission.enabled && adaptiveAdmission.useRecommendedRate) {
              const double recommendedRate = user.getAdaptiveAdmissionRecommendedRateRps();
              if (recommendedRate > 0.0 && recommendedRate < rateRps) {
                *nextAdmissionRecommendedSendAt =
                  std::chrono::steady_clock::now() +
                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1.0 / recommendedRate));
              }
              else {
                *nextAdmissionRecommendedSendAt =
                  std::chrono::steady_clock::time_point::min();
              }
            }
          }

          if (!scheduledNextTickBeforeSend) {
            const auto nextDue = adaptiveAdmission.enabled ?
              *startTime + std::chrono::nanoseconds(
                intervalNs * static_cast<int64_t>(*nextSequence)) +
              openLoopPacingJitter(*nextSequence, openLoopPacingJitterUs) :
              *nextDueAt;
            auto delay = std::max(std::chrono::nanoseconds(0),
                                  nextDue - std::chrono::steady_clock::now());
            if (*queuedTasks > 0) {
              delay = std::chrono::nanoseconds(0);
            }
            else if (!generating) {
              delay = std::chrono::nanoseconds(std::chrono::milliseconds(10));
            }
            scheduleNext(delay);
          }
        };

        *controlAdaptiveWindow = [&, states, adaptiveWindow, queuedTasks,
                                  intervalTimeouts, intervalLateResponses,
                                  intervalSuccesses, intervalLatencies,
                                  highInflightNoSuccessIntervals, sampleAdaptive,
                                  controlAdaptiveWindow]() {
          if (!adaptiveAdmission.enabled || *sendStopped) {
            return;
          }

          const double p95Latency = percentile(*intervalLatencies, 95.0);
          auto diagnostics = user.consumeRuntimeDiagnostics();
          ackLatencySamples->insert(ackLatencySamples->end(),
                                    diagnostics.ackLatenciesMs.begin(),
                                    diagnostics.ackLatenciesMs.end());
          const double p95AckLatency = percentile(diagnostics.ackLatenciesMs, 95.0);
          const size_t runtimeWindow = user.getAdaptiveAdmissionWindow();
          const size_t runtimeQueueDepth = user.getAdaptiveAdmissionQueueDepth();
          const size_t runtimeInflight = user.getAdaptiveAdmissionInflight();
          const bool highInflight = runtimeInflight >= runtimeWindow;
          if (highInflight && *intervalSuccesses == 0) {
            ++(*highInflightNoSuccessIntervals);
          }
          else {
            *highInflightNoSuccessIntervals = 0;
          }
          const bool blocked =
            runtimeInflight >= adaptiveAdmission.hardInflightLimit ||
            p95Latency > 0.9 * static_cast<double>(requestTimeoutMs) ||
            p95AckLatency > 0.9 * static_cast<double>(ackTimeoutMs) ||
            *intervalTimeouts > 0 ||
            *intervalLateResponses > 0 ||
            diagnostics.callbackSkippedNoPending > 0 ||
            diagnostics.callbackSkippedTimeout > 0 ||
            diagnostics.responseAfterPendingTimeout > 0 ||
            *highInflightNoSuccessIntervals >= 3;
          const bool severe =
            blocked ||
            p95Latency > 0.9 * static_cast<double>(requestTimeoutMs);
          const bool congested =
            severe ||
            *intervalTimeouts > 0 ||
            *intervalLateResponses > 0 ||
            p95Latency > 0.75 * static_cast<double>(requestTimeoutMs) ||
            (highInflight && runtimeQueueDepth > 0);
          const bool healthy =
            *intervalTimeouts == 0 &&
            *intervalLateResponses == 0 &&
            diagnostics.callbackSkippedNoPending == 0 &&
            diagnostics.callbackSkippedTimeout == 0 &&
            diagnostics.responseAfterPendingTimeout == 0 &&
            *highInflightNoSuccessIntervals == 0 &&
            p95Latency < 0.5 * static_cast<double>(requestTimeoutMs) &&
            p95AckLatency < 0.5 * static_cast<double>(ackTimeoutMs) &&
            runtimeInflight < runtimeWindow;

          const size_t oldWindow = *adaptiveWindow;
          *adaptiveWindow = runtimeWindow;

          NDN_LOG_TRACE( "PERF_ADAPTIVE_WINDOW window=" << *adaptiveWindow
                    << " old_window=" << oldWindow
                    << " queued=" << *queuedTasks
                    << " inflight=" << states->size()
                    << " runtime_inflight=" << runtimeInflight
                    << " runtime_queue=" << runtimeQueueDepth
                    << " hard_inflight_limit=" << adaptiveAdmission.hardInflightLimit
                    << " paused=" << *paused
                    << " pause_threshold=" << runtimeWindow
                    << " resume_threshold=" << runtimeWindow
                    << " p95_ms=" << std::fixed << std::setprecision(3) << p95Latency
                    << " ack_p95_ms=" << p95AckLatency
                    << " timeouts=" << *intervalTimeouts
                    << " late_responses=" << *intervalLateResponses
                    << " callback_skipped_no_pending="
                    << diagnostics.callbackSkippedNoPending
                    << " callback_skipped_timeout="
                    << diagnostics.callbackSkippedTimeout
                    << " response_after_pending_timeout="
                    << diagnostics.responseAfterPendingTimeout
                    << " interval_successes=" << *intervalSuccesses
                    << " no_success_high_inflight_intervals="
                    << *highInflightNoSuccessIntervals
                    << " blocked=" << blocked
                    << " congested=" << congested
                    << " healthy=" << healthy
                    << " ts=" << nowMilliseconds());

          *intervalTimeouts = 0;
          *intervalLateResponses = 0;
          *intervalSuccesses = 0;
          intervalLatencies->clear();
          (*sampleAdaptive)();
          scheduler.schedule(ndn::time::milliseconds(adaptiveAdmission.controlIntervalMs),
                             [controlAdaptiveWindow] { (*controlAdaptiveWindow)(); });
        };

        scheduler.schedule(ndn::time::seconds(2), [&, startTime, measurementStartAt, stopSendingAt,
                                                   drainDeadline, sendNext,
                                                   benchmarkStrategy,
                                                   useBenchmarkCustomSelection,
                                                   useBenchmarkRandomSelection] {
          *startTime = std::chrono::steady_clock::now();
          *nextDueAt = *startTime;
          *measurementStartAt = *startTime + std::chrono::seconds(benchmarkWarmup);
          *stopSendingAt =
            *measurementStartAt + std::chrono::seconds(openLoopDurationSeconds);
          *drainDeadline = *stopSendingAt + std::chrono::seconds(drainSeconds);
          const auto startQueueLimits = adaptiveQueueLimitsForWindow(
            *adaptiveWindow,
            adaptiveAdmission.softQueueLimit,
            adaptiveAdmission.hardQueueLimit);
          NDN_LOG_WARN( "Starting open-loop benchmark strategy="
                    << benchmarkStrategyLabel(benchmarkStrategy,
                                              useBenchmarkCustomSelection,
                                              useBenchmarkRandomSelection)
                    << " rate_rps=" << std::fixed << std::setprecision(3) << rateRps
                    << " duration_s=" << openLoopDurationSeconds
                    << " warmup_s=" << benchmarkWarmup
                    << " max_inflight=" << maxOutstanding
                    << " adaptive_admission=" << adaptiveAdmission.enabled
                    << " adaptive_initial_window=" << *adaptiveWindow
                    << " adaptive_min_window=" << adaptiveAdmission.minWindow
                    << " adaptive_max_window=" << adaptiveAdmission.maxWindow
                    << " hard_inflight_limit=" << adaptiveAdmission.hardInflightLimit
                    << " admission_soft_queue_limit=" << startQueueLimits.first
                    << " admission_hard_queue_limit=" << startQueueLimits.second
                    << " admission_warning_backoff_ms="
                    << adaptiveAdmission.warningBackoffMs
                    << " admission_reject_backoff_ms="
                    << adaptiveAdmission.rejectBackoffMs
                    << " admission_queue_aware_pause="
                    << adaptiveAdmission.queueAwarePause
                    << " admission_use_recommended_rate="
                    << adaptiveAdmission.useRecommendedRate
                    << " adaptive_hard_target_latency_ms="
                    << adaptiveAdmission.hardTargetLatencyMs
                    << " admission_warning_resume_queue_depth="
                    << adaptiveAdmission.warningResumeQueueDepth
                    << " admission_reject_resume_queue_depth="
                    << adaptiveAdmission.rejectResumeQueueDepth
                    << " adaptive_target_latency_ms=" << adaptiveAdmission.targetLatencyMs
                    << " request_timeout_ms=" << requestTimeoutMs
                    << " pacing_jitter_us=" << openLoopPacingJitterUs
                    << " drain_seconds=" << drainSeconds
                   );
          if (adaptiveAdmission.enabled) {
            scheduler.schedule(ndn::time::milliseconds(adaptiveAdmission.controlIntervalMs),
                               [controlAdaptiveWindow] { (*controlAdaptiveWindow)(); });
          }
          (*sendNext)();
        });

        face.processEvents();
        return exitCode;
      }

      if (benchmarkCount < 0 || benchmarkWarmup < 0 || benchmarkIntervalMs < 0) {
        std::cerr << "Benchmark count, warmup, and interval must be non-negative" << std::endl;
        return 2;
      }
      if (outputCsv.empty()) {
        std::cerr << "--output-csv is required in --benchmark mode" << std::endl;
        return 2;
      }

      auto csv = std::make_shared<std::ofstream>(outputCsv);
      if (!*csv) {
        std::cerr << "Failed to open benchmark CSV: " << outputCsv << std::endl;
        return 2;
      }
      *csv << "request_id,success,latency_ms,response_payload\n";

      auto results = std::make_shared<std::vector<BenchmarkResult>>();
      auto issued = std::make_shared<int>(0);
      auto completed = std::make_shared<int>(0);
      auto timedOut = std::make_shared<int>(0);
      auto completedRequestIds = std::make_shared<std::vector<std::string>>();
      auto currentStart = std::make_shared<std::chrono::steady_clock::time_point>();
      auto currentMeasured = std::make_shared<bool>(false);
      auto currentRequestId = std::make_shared<std::string>();
      auto runNext = std::make_shared<std::function<void()>>();
      const int totalCalls = benchmarkWarmup + benchmarkCount;
      const ndn::Name benchmarkServiceName(serviceNameText);
      const bool useBenchmarkCustomSelection =
        benchmarkStrategyText == "custom-selection" || benchmarkStrategyText == "custom";
      const bool useBenchmarkRandomSelection =
        benchmarkStrategyText == "random-selection" || benchmarkStrategyText == "random";
      const size_t benchmarkStrategy = parseBenchmarkStrategy(benchmarkStrategyText);

      *runNext = [&, csv, results, issued, completed, timedOut, completedRequestIds, currentStart,
                  currentMeasured, currentRequestId, runNext, totalCalls,
                  benchmarkServiceName, useBenchmarkCustomSelection,
                  useBenchmarkRandomSelection, benchmarkStrategy]() {
        if (*issued >= totalCalls) {
          const int success = static_cast<int>(results->size());
          const int timeout = *timedOut;
          std::vector<double> latencies;
          latencies.reserve(results->size());
          for (const auto& result : *results) {
            if (result.success) {
              latencies.push_back(result.latencyMs);
            }
          }

          double avg = 0.0;
          double min = 0.0;
          double max = 0.0;
          if (!latencies.empty()) {
            avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) /
                  static_cast<double>(latencies.size());
            min = *std::min_element(latencies.begin(), latencies.end());
            max = *std::max_element(latencies.begin(), latencies.end());
          }

          NDN_LOG_INFO( std::fixed << std::setprecision(3)
                    << "count=" << benchmarkCount << "\n"
                    << "success=" << success << "\n"
                    << "timeout=" << timeout << "\n"
                    << "avg_ms=" << avg << "\n"
                    << "min_ms=" << min << "\n"
                    << "max_ms=" << max << "\n"
                    << "p50_ms=" << percentile(latencies, 50.0) << "\n"
                    << "p95_ms=" << percentile(latencies, 95.0) << "\n"
                    << "p99_ms=" << percentile(latencies, 99.0) << "\n"
                    << "strategy=" << benchmarkStrategyLabel(benchmarkStrategy,
                                                              useBenchmarkCustomSelection,
                                                              useBenchmarkRandomSelection) << "\n"
                    << "pendingCalls_remaining=" << user.getPendingCallCount() << "\n"
                    << "csv=" << outputCsv);

          csv->flush();
          if (success == benchmarkCount && timeout == 0) {
            NDN_LOG_INFO( "LOCAL_NFD_SERVICE_LATENCY_BENCHMARK=PASS");
            exitCode = 0;
          }
          else {
            NDN_LOG_INFO( "LOCAL_NFD_SERVICE_LATENCY_BENCHMARK=FAIL");
            exitCode = 1;
          }
          face.getIoContext().stop();
          return;
        }

        const bool measured = *issued >= benchmarkWarmup;
        *currentMeasured = measured;

        const std::string requestText = "HELLO";
        ndn::Buffer requestPayload(
          reinterpret_cast<const uint8_t*>(requestText.data()),
          requestText.size());

        ndn_service_framework::RequestMessage request;
        request.setPayload(requestPayload, requestPayload.size());
        request.setStrategy((useBenchmarkCustomSelection || useBenchmarkRandomSelection) ?
                            static_cast<size_t>(ndn_service_framework::tlv::FirstResponding) :
                            benchmarkStrategy);

        *currentStart = std::chrono::steady_clock::now();
        auto onTimeout = std::function<void(const ndn::Name&)>(
            [&, csv, results, issued, completed, timedOut, completedRequestIds, currentStart,
             currentMeasured, currentRequestId, runNext, totalCalls](const ndn::Name& timedOutRequestId) {
              const std::string requestIdText = timedOutRequestId.toUri();
              if (std::find(completedRequestIds->begin(),
                            completedRequestIds->end(),
                            requestIdText) != completedRequestIds->end()) {
                return;
              }

              const auto end = std::chrono::steady_clock::now();
              const double latencyMs = std::chrono::duration<double, std::milli>(
                end - *currentStart).count();

              if (*currentMeasured) {
                ++(*timedOut);
                *csv << timedOutRequestId.toUri() << ",0,"
                     << std::fixed << std::setprecision(3) << latencyMs << ",\n";
              }

              ++(*completed);
              NDN_LOG_TRACE( "PERF_REQUEST_TIMEOUT id="
                        << timedOutRequestId.toUri()
                        << " ts=" << nowMilliseconds());
              NDN_LOG_TRACE( "[App_User] benchmark timeout requestId="
                        << timedOutRequestId.toUri()
                        << " completed=" << *completed
                        << "/" << totalCalls);
              scheduler.schedule(ndn::time::milliseconds(benchmarkIntervalMs),
                                 [runNext] { (*runNext)(); });
            });
        auto onResponse = std::function<void(const ndn_service_framework::ResponseMessage&)>(
            [&, csv, results, issued, completed, completedRequestIds, currentStart, currentMeasured,
             currentRequestId, runNext, totalCalls](const ndn_service_framework::ResponseMessage& response) {
              if (std::find(completedRequestIds->begin(),
                            completedRequestIds->end(),
                            *currentRequestId) != completedRequestIds->end()) {
                return;
              }
              completedRequestIds->push_back(*currentRequestId);

              const auto end = std::chrono::steady_clock::now();
              const double latencyMs = std::chrono::duration<double, std::milli>(
                end - *currentStart).count();
              const auto payload = response.getPayload();
              const std::string responseText(
                reinterpret_cast<const char*>(payload.data()),
                payload.size());

              if (*currentMeasured) {
                results->push_back({*currentRequestId, true, latencyMs, responseText});
                *csv << csvEscape(*currentRequestId) << ",1,"
                     << std::fixed << std::setprecision(3) << latencyMs << ","
                     << csvEscape(responseText) << "\n";
              }

              ++(*completed);
              NDN_LOG_TRACE( "PERF_RESPONSE_RECEIVED id=" << *currentRequestId
                        << " provider=-"
                        << " latency_ms=" << std::fixed << std::setprecision(3)
                        << latencyMs
                        << " ts=" << nowMilliseconds());
              NDN_LOG_TRACE( "[App_User] benchmark response requestId="
                        << *currentRequestId
                        << " latency_ms=" << std::fixed << std::setprecision(3)
                        << latencyMs
                        << " payload=" << responseText
                        << " completed=" << *completed
                        << "/" << totalCalls);
              scheduler.schedule(ndn::time::milliseconds(benchmarkIntervalMs),
                                 [runNext] { (*runNext)(); });
            });

        std::shared_ptr<const ndnsf::AckSelectionPolicy> selectionPolicy;
        if (useBenchmarkCustomSelection) {
          selectionPolicy = makeRankQueueSelectionPolicy(ackTimeoutMs, performanceMode);
        }
        else if (useBenchmarkRandomSelection ||
                 benchmarkStrategy == ndn_service_framework::tlv::RandomSelection) {
          selectionPolicy = ndnsf::strategy::RandomSelection;
        }
        else if (benchmarkStrategy == ndn_service_framework::tlv::AllSelected) {
          selectionPolicy = ndnsf::strategy::AllSelected;
        }
        else {
          selectionPolicy = ndnsf::strategy::FirstResponding;
        }

        ndn::Name requestId = user.RequestService(
          benchmarkServiceName,
          request.getPayload(),
          ackTimeoutMs,
          selectionPolicy,
          timeoutMs,
          onResponse,
          onTimeout);

        *currentRequestId = requestId.toUri();
        NDN_LOG_TRACE( "PERF_REQUEST_SENT id=" << *currentRequestId
                  << " ts=" << nowMilliseconds());
        ++(*issued);
      };

      scheduler.schedule(ndn::time::seconds(2), [runNext, benchmarkStrategy,
                                                 useBenchmarkCustomSelection,
                                                 useBenchmarkRandomSelection] {
        NDN_LOG_INFO( "Starting local NFD service latency benchmark strategy="
                  << benchmarkStrategyLabel(benchmarkStrategy,
                                            useBenchmarkCustomSelection,
                                            useBenchmarkRandomSelection)
                 );
        (*runNext)();
      });

      face.processEvents();
      return exitCode;
    }

    scheduler.schedule(ndn::time::seconds(2), [&] {
      NDN_LOG_INFO( "Sending HELLO request...");
      NDN_LOG_INFO( "[App_User] selected providerName="
                << PROVIDER_IDENTITY.toUri());
      if (!knownProviders.empty()) {
        std::ostringstream knownProvidersText;
        for (const auto& provider : knownProviders) {
          if (knownProvidersText.tellp() > 0) {
            knownProvidersText << ",";
          }
          knownProvidersText << provider.toUri();
        }
        NDN_LOG_INFO( "[App_User] known providers=" << knownProvidersText.str());
      }
      NDN_LOG_INFO( "[App_User] selected serviceName=/HELLO");
      NDN_LOG_INFO( "[App_User] final request name="
                   "/example/hello/user/NDNSF/REQUEST/HELLO/<requestId>"
               );

      const std::string requestText = "HELLO";
      ndn::Buffer requestPayload(
        reinterpret_cast<const uint8_t*>(requestText.data()),
        requestText.size());

      ndn_service_framework::RequestMessage request;
      request.setPayload(requestPayload, requestPayload.size());
      request.setStrategy(ndn_service_framework::tlv::FirstResponding);

      auto onTimeout = std::function<void(const ndn::Name&)>([&](const ndn::Name&) {
        std::cerr << "HELLO request timed out" << std::endl;
        face.getIoContext().stop();
      });
      auto onResponse = std::function<void(const ndn_service_framework::ResponseMessage&)>(
        [&](const ndn_service_framework::ResponseMessage& response) {
          const auto payload = response.getPayload();
          const std::string responseText(
            reinterpret_cast<const char*>(payload.data()),
            payload.size());
          NDN_LOG_INFO( "Received response: " << responseText);
          if (!expectedResponse.empty() && responseText == expectedResponse) {
            NDN_LOG_INFO( "SELECTIVE_ACK_CUSTOM_SELECTION_REGRESSION=PASS");
          }
          face.getIoContext().stop();
        });

      if (useCustomSelection) {
        auto selectCandidates =
          [ackTimeoutMs](const std::vector<ndnsf::AckCandidate>& candidates) {
            NDN_LOG_INFO( "customSelectionStrategy ran after ackTimeoutMs="
                      << ackTimeoutMs
                      << " candidateCount=" << candidates.size());

            std::vector<ndnsf::AckCandidate> selected;
            int bestRank = std::numeric_limits<int>::max();
            int bestQueue = std::numeric_limits<int>::max();
            for (const auto& candidate : candidates) {
              const auto payload = candidate.ack.getPayload();
              const std::string payloadText(
                reinterpret_cast<const char*>(payload.data()),
                payload.size());
              NDN_LOG_INFO( "customSelectionStrategy candidate providerName="
                        << candidate.providerName.toUri()
                        << " status=" << candidate.ack.getStatus()
                        << " message=" << candidate.ack.getMessage()
                        << " payload=" << payloadText);
              NDN_LOG_INFO( "customSelectionStrategy ACK received timestampMs="
                        << nowMilliseconds()
                        << " providerName=" << candidate.providerName.toUri()
                       );

              if (!candidate.ack.getStatus()) {
                NDN_LOG_INFO( "customSelectionStrategy rejected provider="
                          << providerLabel(candidate.providerName)
                          << " status=0");
                continue;
              }

              const int rank = parseMetadataInt(payloadText, "rank",
                                                std::numeric_limits<int>::max());
              const int queue = parseMetadataInt(payloadText, "queue",
                                                 std::numeric_limits<int>::max());
              NDN_LOG_INFO( "collected ACK payload provider="
                        << providerLabel(candidate.providerName)
                        << " queue=" << queue
                        << " rank=" << rank);
              if (selected.empty() ||
                  rank < bestRank ||
                  (rank == bestRank && queue < bestQueue)) {
                selected.clear();
                selected.push_back(candidate);
                bestRank = rank;
                bestQueue = queue;
              }
            }

            if (!selected.empty()) {
              NDN_LOG_INFO( "customSelectionStrategy selected providerName="
                        << selected.front().providerName.toUri());
              return std::vector<ndnsf::ProviderId>{selected.front().providerName};
            }
            return std::vector<ndnsf::ProviderId>{};
          };
        if (!knownProviders.empty()) {
          ndn_service_framework::ServiceUser::AckCandidatesHandler handler =
            [selectCandidates](const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
              const auto selectedProviders = selectCandidates(candidates);
              std::vector<ndn_service_framework::AckSelectionCandidate> selected;
              for (const auto& provider : selectedProviders) {
                for (const auto& candidate : candidates) {
                  if (candidate.providerName.equals(provider) &&
                      candidate.ack.getStatus()) {
                    selected.push_back(candidate);
                    break;
                  }
                }
              }
              return selected;
            };
          user.RequestService(
            knownProviders,
            ndn::Name("/HELLO"),
            request,
            ackTimeoutMs,
            std::move(handler),
            20000,
            onTimeout,
            onResponse,
            ndn_service_framework::tlv::FirstResponding);
        }
        else {
          auto selectionPolicy = std::make_shared<FunctionalAckSelectionPolicy>(
            std::move(selectCandidates));
          user.RequestService(
            ndn::Name("/HELLO"),
            request.getPayload(),
            ackTimeoutMs,
            selectionPolicy,
            20000,
            onResponse,
            onTimeout);
        }
      }
      else {
        if (!knownProviders.empty()) {
          user.RequestService(
            knownProviders,
            ndn::Name("/HELLO"),
            request,
            ackTimeoutMs,
            ndn_service_framework::ServiceUser::AckSelectionStrategy::RandomSelection,
            20000,
            onTimeout,
            onResponse);
        }
        else {
          user.RequestService(
            ndn::Name("/HELLO"),
            request.getPayload(),
            ackTimeoutMs,
            ndnsf::strategy::RandomSelection,
            20000,
            onResponse,
            onTimeout);
        }
      }
    });

    face.processEvents();
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "App_User error: " << e.what() << std::endl;
    return 1;
  }
}
