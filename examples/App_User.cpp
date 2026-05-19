#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
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

size_t
parseBenchmarkStrategy(const std::string& strategy)
{
  if (strategy == "all-responders" || strategy == "AllResponders") {
    return ndn_service_framework::tlv::AllResponders;
  }
  if (strategy == "first-responding" || strategy == "FirstResponding") {
    return ndn_service_framework::tlv::FirstResponding;
  }
  if (strategy == "load-balancing" || strategy == "LoadBalancing") {
    return ndn_service_framework::tlv::LoadBalancing;
  }
  return ndn_service_framework::tlv::FirstResponding;
}

std::string
benchmarkStrategyLabel(size_t strategy, bool customSelection)
{
  if (customSelection) {
    return "random-selection";
  }
  if (strategy == ndn_service_framework::tlv::AllResponders) {
    return "all-responders";
  }
  if (strategy == ndn_service_framework::tlv::LoadBalancing) {
    return "load-balancing";
  }
  return "first-responding";
}

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
  bool completed = false;
  std::string selectedProvider = "-";
};

struct AdaptiveAdmissionConfig
{
  bool enabled = false;
  size_t minWindow = 1;
  size_t maxWindow = 512;
  size_t initialWindow = 16;
  size_t hardInflightLimit = 512;
  size_t aiStep = 4;
  double mdFactor = 0.85;
  double severeMdFactor = 0.75;
  int controlIntervalMs = 500;
  int targetLatencyMs = 350;
};

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
    const bool useTokens = !hasFlag(argc, argv, "--disable-tokens");
    const bool hybridMessageCrypto = hasFlag(argc, argv, "--hybrid-message-crypto");
    const bool timelineTrace = hasFlag(argc, argv, "--timeline-trace");
    const bool serveCertificates = !hasFlag(argc, argv, "--no-serve-certificates");
    const std::string workloadMode = getOption(argc, argv, "--workload-mode", "closed-loop");
    const int benchmarkCount = parseIntOption(argc, argv, "--count", 100);
    const int benchmarkWarmup = parseIntOption(argc, argv, "--warmup", 5);
    const int benchmarkIntervalMs = parseIntOption(argc, argv, "--interval-ms", 1000);
    const int ackTimeoutMs = parseIntOption(argc, argv, "--ack-timeout-ms", 3000);
    const int timeoutMs = parseIntOption(argc, argv, "--timeout-ms", 5000);
    const int requestTimeoutMs = parseIntOption(argc, argv, "--request-timeout-ms", timeoutMs);
    const int handlerThreads = parseIntOption(argc, argv, "--handler-threads", 0);
    const double rateRps = parseDoubleOption(argc, argv, "--rate-rps", 1.0);
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
    adaptiveAdmission.enabled = hasFlag(argc, argv, "--adaptive-admission-control");
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
    adaptiveAdmission.maxWindow = std::max(adaptiveAdmission.minWindow,
                                           adaptiveAdmission.maxWindow);
    adaptiveAdmission.hardInflightLimit =
      std::max(adaptiveAdmission.minWindow, adaptiveAdmission.hardInflightLimit);
    adaptiveAdmission.maxWindow = std::min(adaptiveAdmission.maxWindow,
                                           adaptiveAdmission.hardInflightLimit);
    adaptiveAdmission.initialWindow = std::max(
      adaptiveAdmission.minWindow,
      std::min(adaptiveAdmission.initialWindow, adaptiveAdmission.maxWindow));
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

    auto userCert = getOrCreateIdentity(keyChain, USER_IDENTITY);
    auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);
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
      "examples/trust-any.conf");

    user.init();
    user.setPerformanceMode(performanceMode);
    user.setHandlerThreads(
      handlerThreads > 0 ? static_cast<size_t>(handlerThreads) : 0);
    user.setUseTokens(useTokens);
    user.setUseHybridMessageCrypto(hybridMessageCrypto);
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
    user.setAdaptiveAdmissionControl(runtimeAdmission);
    std::cout << "[App_User] token_mode="
              << (user.getUseTokens() ? "enabled" : "disabled")
              << " hybridMessageCrypto=" << user.getUseHybridMessageCrypto()
              << " timelineTrace=" << timelineTrace
              << " handlerThreads=" << user.getHandlerThreads()
              << " adaptiveAdmission=" << adaptiveAdmission.enabled
              << " adaptiveWindow=" << user.getAdaptiveAdmissionWindow()
              << std::endl;
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

        std::cout << "LARGE_DATA_PUBLISH_SUCCESS name="
                  << result.encryptedDataName.toUri()
                  << " requestId=" << ctx.requestId.toUri()
                  << " objectId=" << result.objectId
                  << std::endl;

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

        std::cout << "LARGE_DATA_PUBLISH_SERVING" << std::endl;
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
        const auto lifecycleCsvPath =
          std::filesystem::path(outputCsv).parent_path() / "request_lifecycle.csv";
        auto lifecycleCsv = std::make_shared<std::ofstream>(lifecycleCsvPath);
        if (*lifecycleCsv) {
          *lifecycleCsv
            << "application_task_id,request_id,service_name,state,selected_provider,"
            << "enqueue_timestamp_us,admission_timestamp_us,publish_timestamp_us,"
            << "ack_matched_timestamp_us,provider_selection_timestamp_us,"
            << "coordination_publish_timestamp_us,response_observed_timestamp_us,"
            << "response_decrypted_timestamp_us,callback_timestamp_us,"
            << "completion_timestamp_us,timeout_timestamp_us,queued_duration_ms,"
            << "inflight_duration_ms,end_to_end_latency_ms,"
            << "delayed_by_admission_control,final_cleanup_reason\n";
          user.setRequestLifecycleCallback(
            [lifecycleCsv](const ndn_service_framework::ServiceUser::RequestLifecycleStatus& status) {
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
                << status.coordinationPublishTimestampUs << ","
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
          benchmarkStrategyText == "custom-selection" || benchmarkStrategyText == "custom" ||
          benchmarkStrategyText == "random-selection" || benchmarkStrategyText == "random";
        const size_t benchmarkStrategy = parseBenchmarkStrategy(benchmarkStrategyText);
        const auto intervalNs = static_cast<int64_t>(
          std::max(1.0, 1000000000.0 / rateRps));
        const auto startTime = std::make_shared<std::chrono::steady_clock::time_point>();
        const auto stopSendingAt = std::make_shared<std::chrono::steady_clock::time_point>();
        const auto drainDeadline = std::make_shared<std::chrono::steady_clock::time_point>();
        auto nextSequence = std::make_shared<uint64_t>(0);
        auto sendStopped = std::make_shared<bool>(false);
        auto states = std::make_shared<std::map<std::string, std::shared_ptr<OpenLoopRequestState>>>();
        auto completedRequestIds = std::make_shared<std::set<std::string>>();
        auto sentCount = std::make_shared<uint64_t>(0);
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
        auto intervalTimeouts = std::make_shared<uint64_t>(0);
        auto intervalLateResponses = std::make_shared<uint64_t>(0);
        auto intervalSuccesses = std::make_shared<uint64_t>(0);
        auto intervalLatencies = std::make_shared<std::vector<double>>();
        auto ackLatencySamples = std::make_shared<std::vector<double>>();
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
          std::cout << "PERF_ADMISSION_PAUSED reason=" << reason
                    << " inflight=" << states->size()
                    << " queued=" << *queuedTasks
                    << " adaptive_window=" << *adaptiveWindow
                    << " hard_inflight_limit=" << adaptiveAdmission.hardInflightLimit
                    << " pause_threshold=" << pauseThreshold()
                    << " ts=" << nowMilliseconds() << std::endl;
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
          std::cout << "PERF_ADMISSION_RESUMED"
                    << " inflight=" << states->size()
                    << " queued=" << *queuedTasks
                    << " adaptive_window=" << *adaptiveWindow
                    << " resume_threshold=" << resumeThreshold()
                    << " paused_ms=" << pausedForMs
                    << " ts=" << nowMilliseconds() << std::endl;
        };

        *maybeFinish = [&, csv, states, sendStopped, drainDeadline, sentCount, successCount,
                        timeoutCount, lateResponseCount, outstandingLimitSkips, latencies,
                        outstandingSamples, maxOutstandingObserved, startTime,
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
            std::chrono::duration<double>(*stopSendingAt - *startTime).count();
          const double achievedRps = configuredDurationSeconds > 0.0 ?
            static_cast<double>(*sentCount) / configuredDurationSeconds : 0.0;
          uint64_t reportedPausedMs = *totalPausedMs;
          if (*paused) {
            const auto pausedForMs = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - *pauseStartedAt).count();
            if (pausedForMs > 0) {
              reportedPausedMs += static_cast<uint64_t>(pausedForMs);
            }
          }

          std::cout << std::fixed << std::setprecision(3)
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
                    << std::endl;
          csv->flush();
          if (lifecycleCsv && *lifecycleCsv) {
            lifecycleCsv->flush();
          }
          exitCode = 0;
          face.getIoContext().stop();
        };

        *sendNext = [&, csv, benchmarkServiceName, useBenchmarkCustomSelection, benchmarkStrategy,
                     intervalNs, startTime, stopSendingAt, drainDeadline, nextSequence,
                     sendStopped, states, completedRequestIds, sentCount, successCount,
                     timeoutCount, lateResponseCount, outstandingLimitSkips, latencies,
                     sampleOutstanding, sendNext, maybeFinish]() {
          const auto now = std::chrono::steady_clock::now();
          const bool generating = now < *stopSendingAt;
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
          auto skipCurrentOpenLoopTick = [&]() {
            if (!generating) {
              return;
            }
            uint64_t nextTick = *nextSequence + 1;
            if (intervalNs > 0) {
              const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - *startTime).count();
              if (elapsedNs > 0) {
                nextTick = std::max<uint64_t>(
                  nextTick,
                  static_cast<uint64_t>(elapsedNs / intervalNs) + 1);
              }
            }
            *nextSequence = nextTick;
          };
          auto delayUntilNextOpenLoopTick = [&]() {
            if (!generating || intervalNs <= 0) {
              return retryDelay;
            }
            const auto nextDue = *startTime + std::chrono::nanoseconds(
              intervalNs * static_cast<int64_t>(*nextSequence));
            return std::max(std::chrono::nanoseconds(0),
                            nextDue - std::chrono::steady_clock::now());
          };

          (*maybeResume)();
          const auto runtimeWindow = user.getAdaptiveAdmissionWindow();
          const auto runtimeAdmissionInflight = user.getAdaptiveAdmissionInflight();
          const size_t currentPauseThreshold = adaptiveAdmission.enabled ?
            std::max<size_t>(
              1,
              std::min(runtimeWindow, adaptiveAdmission.hardInflightLimit)) :
            pauseThreshold();
          const size_t activeLimit = adaptiveAdmission.enabled ?
            currentPauseThreshold :
            static_cast<size_t>(maxOutstanding);
          const bool admissionAtLimit =
            adaptiveAdmission.enabled && runtimeAdmissionInflight >= activeLimit;
          const bool pendingResponseAtLimit =
            adaptiveAdmission.enabled &&
            states->size() >= static_cast<size_t>(maxOutstanding);

          if (adaptiveAdmission.enabled &&
              (admissionAtLimit || pendingResponseAtLimit || *paused)) {
            ++(*outstandingLimitSkips);
            ++(*delayedPublications);
            (*sampleOutstanding)();
            (*sampleAdaptive)();
            if (!performanceMode && (admissionAtLimit || pendingResponseAtLimit)) {
              std::cout << "PERF_OUTSTANDING_LIMIT_REACHED outstanding="
                        << states->size()
                        << " admission_inflight=" << runtimeAdmissionInflight
                        << " queued=" << *queuedTasks
                        << " adaptive_window=" << runtimeWindow
                        << " pending_response_limit=" << maxOutstanding
                        << " ts=" << nowMilliseconds() << std::endl;
            }
            skipCurrentOpenLoopTick();
            scheduleNext(delayUntilNextOpenLoopTick());
            return;
          }

          if (generating) {
            ++(*queuedTasks);
            (*sampleAdaptive)();
          }

          const size_t currentLoad = adaptiveAdmission.enabled ?
            runtimeAdmissionInflight : states->size();
          if (currentLoad >= activeLimit ||
              (adaptiveAdmission.enabled &&
               states->size() >= static_cast<size_t>(maxOutstanding))) {
            ++(*outstandingLimitSkips);
            ++(*delayedPublications);
            (*sampleOutstanding)();
            (*sampleAdaptive)();
            if (!performanceMode) {
              std::cout << "PERF_OUTSTANDING_LIMIT_REACHED outstanding="
                        << states->size()
                        << " admission_inflight=" << runtimeAdmissionInflight
                        << " queued=" << *queuedTasks
                        << " adaptive_window=" << *adaptiveWindow
                        << " pending_response_limit=" << maxOutstanding
                        << " ts=" << nowMilliseconds() << std::endl;
            }
          }
          else if (*queuedTasks > 0) {
            const std::string requestText = "HELLO";
            ndn::Buffer requestPayload(
              reinterpret_cast<const uint8_t*>(requestText.data()),
              requestText.size());

            ndn_service_framework::RequestMessage request;
            request.setPayload(requestPayload, requestPayload.size());
            request.setStrategy(useBenchmarkCustomSelection ?
                                static_cast<size_t>(ndn_service_framework::tlv::FirstResponding) :
                                benchmarkStrategy);

            auto state = std::make_shared<OpenLoopRequestState>();
            state->start = now;

            auto onTimeout = std::function<void(const ndn::Name&)>(
              [&, csv, states, completedRequestIds, timeoutCount,
               intervalTimeouts, sampleOutstanding, sampleAdaptive](
                const ndn::Name& timedOutRequestId) {
                face.getIoContext().post(
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
                    if (stateIt != states->end()) {
                      latencyMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - stateIt->second->start).count();
                      states->erase(stateIt);
                    }
                    ++(*timeoutCount);
                    ++(*intervalTimeouts);
                    if (!adaptiveAdmission.enabled) {
                      (*maybePause)("timeout");
                    }
                    *csv << csvEscape(requestIdText) << ",0,"
                         << std::fixed << std::setprecision(3) << latencyMs << ",\n";
                    if (!performanceMode) {
                      std::cout << "PERF_REQUEST_TIMEOUT id=" << requestIdText
                                << " ts=" << nowMilliseconds() << std::endl;
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
                face.getIoContext().post(
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
                      if (!adaptiveAdmission.enabled) {
                        (*maybePause)("late_response");
                      }
                      if (!performanceMode) {
                        std::cout << "PERF_LATE_RESPONSE id=" << requestIdText
                                  << " ts=" << nowMilliseconds() << std::endl;
                      }
                      return;
                    }
                    completedRequestIds->insert(requestIdText);

                    const double latencyMs = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - state->start).count();
                    ++(*successCount);
                    ++(*intervalSuccesses);
                    latencies->push_back(latencyMs);
                    intervalLatencies->push_back(latencyMs);
                    *csv << csvEscape(requestIdText) << ",1,"
                         << std::fixed << std::setprecision(3) << latencyMs << ","
                         << csvEscape(responseText) << "\n";
                    if (!performanceMode) {
                      std::cout << "PERF_RESPONSE_RECEIVED id=" << requestIdText
                                << " provider=" << state->selectedProvider
                                << " latency_ms=" << std::fixed << std::setprecision(3)
                                << latencyMs
                                << " ts=" << nowMilliseconds() << std::endl;
                    }
                    states->erase(requestIdText);
                    (*sampleOutstanding)();
                    (*sampleAdaptive)();
                    (*maybeResume)();
                  });
              });

            ndn::Name requestId;
            if (useBenchmarkCustomSelection) {
              requestId = user.async_call(
                std::vector<ndn::Name>{},
                benchmarkServiceName,
                request,
                ackTimeoutMs,
                ndn_service_framework::ServiceUser::AckSelectionStrategy::RandomSelection,
                requestTimeoutMs,
                onTimeout,
                onResponse);
            }
            else if (benchmarkStrategy == ndn_service_framework::tlv::FirstResponding) {
              requestId = user.async_call(
                std::vector<ndn::Name>{},
                benchmarkServiceName,
                request,
                ackTimeoutMs,
                ndn_service_framework::ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
                requestTimeoutMs,
                onTimeout,
                onResponse);
            }
            else if (benchmarkStrategy == ndn_service_framework::tlv::AllResponders) {
              requestId = user.async_call(
                std::vector<ndn::Name>{},
                benchmarkServiceName,
                request,
                ackTimeoutMs,
                ndn_service_framework::ServiceUser::AckSelectionStrategy::AllResponders,
                requestTimeoutMs,
                onTimeout,
                onResponse);
            }
            else {
              requestId = user.async_call(
                benchmarkServiceName,
                request,
                requestTimeoutMs,
                onTimeout,
                onResponse,
                benchmarkStrategy);
            }

            state->requestId = requestId.toUri();
            (*states)[state->requestId] = state;
            if (*queuedTasks > 0) {
              --(*queuedTasks);
            }
            ++(*sentCount);
            (*sampleOutstanding)();
            (*sampleAdaptive)();
            if (!performanceMode) {
              std::cout << "PERF_REQUEST_SENT id=" << state->requestId
                        << " ts=" << nowMilliseconds() << std::endl;
            }
          }

          ++(*nextSequence);
          const auto nextDue = *startTime + std::chrono::nanoseconds(
            intervalNs * static_cast<int64_t>(*nextSequence));
          auto delay = std::max(std::chrono::nanoseconds(0),
                                nextDue - std::chrono::steady_clock::now());
          if (!generating && *queuedTasks > 0) {
            delay = std::max(delay, std::chrono::nanoseconds(std::chrono::milliseconds(10)));
          }
          scheduleNext(delay);
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

          std::cout << "PERF_ADAPTIVE_WINDOW window=" << *adaptiveWindow
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
                    << " ts=" << nowMilliseconds() << std::endl;

          *intervalTimeouts = 0;
          *intervalLateResponses = 0;
          *intervalSuccesses = 0;
          intervalLatencies->clear();
          (*sampleAdaptive)();
          scheduler.schedule(ndn::time::milliseconds(adaptiveAdmission.controlIntervalMs),
                             [controlAdaptiveWindow] { (*controlAdaptiveWindow)(); });
        };

        scheduler.schedule(ndn::time::seconds(2), [&, startTime, stopSendingAt,
                                                   drainDeadline, sendNext,
                                                   benchmarkStrategy,
                                                   useBenchmarkCustomSelection] {
          *startTime = std::chrono::steady_clock::now();
          *stopSendingAt = *startTime + std::chrono::seconds(openLoopDurationSeconds);
          *drainDeadline = *stopSendingAt + std::chrono::seconds(drainSeconds);
          std::cout << "Starting open-loop benchmark strategy="
                    << benchmarkStrategyLabel(benchmarkStrategy, useBenchmarkCustomSelection)
                    << " rate_rps=" << std::fixed << std::setprecision(3) << rateRps
                    << " duration_s=" << openLoopDurationSeconds
                    << " max_inflight=" << maxOutstanding
                    << " adaptive_admission=" << adaptiveAdmission.enabled
                    << " adaptive_initial_window=" << *adaptiveWindow
                    << " adaptive_min_window=" << adaptiveAdmission.minWindow
                    << " adaptive_max_window=" << adaptiveAdmission.maxWindow
                    << " hard_inflight_limit=" << adaptiveAdmission.hardInflightLimit
                    << " adaptive_target_latency_ms=" << adaptiveAdmission.targetLatencyMs
                    << " request_timeout_ms=" << requestTimeoutMs
                    << " drain_seconds=" << drainSeconds
                    << std::endl;
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
        benchmarkStrategyText == "custom-selection" || benchmarkStrategyText == "custom" ||
        benchmarkStrategyText == "random-selection" || benchmarkStrategyText == "random";
      const size_t benchmarkStrategy = parseBenchmarkStrategy(benchmarkStrategyText);

      *runNext = [&, csv, results, issued, completed, timedOut, completedRequestIds, currentStart,
                  currentMeasured, currentRequestId, runNext, totalCalls,
                  benchmarkServiceName, useBenchmarkCustomSelection, benchmarkStrategy]() {
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

          std::cout << std::fixed << std::setprecision(3)
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
                                                              useBenchmarkCustomSelection) << "\n"
                    << "pendingCalls_remaining=" << user.getPendingCallCount() << "\n"
                    << "csv=" << outputCsv << std::endl;

          csv->flush();
          if (success == benchmarkCount && timeout == 0) {
            std::cout << "LOCAL_NFD_SERVICE_LATENCY_BENCHMARK=PASS" << std::endl;
            exitCode = 0;
          }
          else {
            std::cout << "LOCAL_NFD_SERVICE_LATENCY_BENCHMARK=FAIL" << std::endl;
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
        request.setStrategy(useBenchmarkCustomSelection ?
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
              std::cout << "PERF_REQUEST_TIMEOUT id="
                        << timedOutRequestId.toUri()
                        << " ts=" << nowMilliseconds() << std::endl;
              std::cout << "[App_User] benchmark timeout requestId="
                        << timedOutRequestId.toUri()
                        << " completed=" << *completed
                        << "/" << totalCalls << std::endl;
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
              std::cout << "PERF_RESPONSE_RECEIVED id=" << *currentRequestId
                        << " provider=-"
                        << " latency_ms=" << std::fixed << std::setprecision(3)
                        << latencyMs
                        << " ts=" << nowMilliseconds() << std::endl;
              std::cout << "[App_User] benchmark response requestId="
                        << *currentRequestId
                        << " latency_ms=" << std::fixed << std::setprecision(3)
                        << latencyMs
                        << " payload=" << responseText
                        << " completed=" << *completed
                        << "/" << totalCalls << std::endl;
              scheduler.schedule(ndn::time::milliseconds(benchmarkIntervalMs),
                                 [runNext] { (*runNext)(); });
            });

        ndn::Name requestId;
        if (useBenchmarkCustomSelection) {
          requestId = user.async_call(
            std::vector<ndn::Name>{},
            benchmarkServiceName,
            request,
            ackTimeoutMs,
            ndn_service_framework::ServiceUser::AckSelectionStrategy::RandomSelection,
            timeoutMs,
            onTimeout,
            onResponse);
        }
        else if (benchmarkStrategy == ndn_service_framework::tlv::FirstResponding) {
          requestId = user.async_call(
            std::vector<ndn::Name>{},
            benchmarkServiceName,
            request,
            ackTimeoutMs,
            ndn_service_framework::ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
            timeoutMs,
            onTimeout,
            onResponse);
        }
        else if (benchmarkStrategy == ndn_service_framework::tlv::AllResponders) {
          requestId = user.async_call(
            std::vector<ndn::Name>{},
            benchmarkServiceName,
            request,
            ackTimeoutMs,
            ndn_service_framework::ServiceUser::AckSelectionStrategy::AllResponders,
            timeoutMs,
            onTimeout,
            onResponse);
        }
        else {
          requestId = user.async_call(
            benchmarkServiceName,
            request,
            timeoutMs,
            onTimeout,
            onResponse,
            benchmarkStrategy);
        }

        *currentRequestId = requestId.toUri();
        std::cout << "PERF_REQUEST_SENT id=" << *currentRequestId
                  << " ts=" << nowMilliseconds() << std::endl;
        ++(*issued);
      };

      scheduler.schedule(ndn::time::seconds(2), [runNext, benchmarkStrategy,
                                                 useBenchmarkCustomSelection] {
        std::cout << "Starting local NFD service latency benchmark strategy="
                  << benchmarkStrategyLabel(benchmarkStrategy, useBenchmarkCustomSelection)
                  << std::endl;
        (*runNext)();
      });

      face.processEvents();
      return exitCode;
    }

    scheduler.schedule(ndn::time::seconds(2), [&] {
      std::cout << "Sending HELLO request..." << std::endl;
      std::cout << "[App_User] selected providerName="
                << PROVIDER_IDENTITY.toUri() << std::endl;
      std::cout << "[App_User] selected serviceName=/HELLO" << std::endl;
      std::cout << "[App_User] final request name="
                   "/example/hello/user/NDNSF/REQUEST/1/HELLO/<bloomFilter>/<requestId>"
                << std::endl;

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
          std::cout << "Received response: " << responseText << std::endl;
          if (!expectedResponse.empty() && responseText == expectedResponse) {
            std::cout << "SELECTIVE_ACK_CUSTOM_SELECTION_REGRESSION=PASS" << std::endl;
          }
          face.getIoContext().stop();
        });

      if (useCustomSelection) {
        const std::vector<ndn::Name> providers = {
          ndn::Name(PROVIDER_IDENTITY).append("A"),
          ndn::Name(PROVIDER_IDENTITY).append("B"),
          ndn::Name(PROVIDER_IDENTITY).append("C")
        };
        user.async_call(
          providers,
          ndn::Name("/HELLO"),
          request,
          ackTimeoutMs,
          ndn_service_framework::ServiceUser::AckCandidatesHandler(
            [ackTimeoutMs](const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
              std::cout << "customSelectionStrategy ran after ackTimeoutMs="
                        << ackTimeoutMs
                        << " candidateCount=" << candidates.size() << std::endl;

              std::vector<ndn_service_framework::AckSelectionCandidate> selected;
              int bestRank = std::numeric_limits<int>::max();
              int bestQueue = std::numeric_limits<int>::max();
              for (const auto& candidate : candidates) {
                const auto payload = candidate.ack.getPayload();
                const std::string payloadText(
                  reinterpret_cast<const char*>(payload.data()),
                  payload.size());
                std::cout << "customSelectionStrategy candidate providerName="
                          << candidate.providerName.toUri()
                          << " status=" << candidate.ack.getStatus()
                          << " message=" << candidate.ack.getMessage()
                          << " payload=" << payloadText << std::endl;
                std::cout << "customSelectionStrategy ACK received timestampMs="
                          << nowMilliseconds()
                          << " providerName=" << candidate.providerName.toUri()
                          << std::endl;

                if (!candidate.ack.getStatus()) {
                  std::cout << "customSelectionStrategy rejected provider="
                            << providerLabel(candidate.providerName)
                            << " status=0" << std::endl;
                  continue;
                }

                const int rank = parseMetadataInt(payloadText, "rank",
                                                  std::numeric_limits<int>::max());
                const int queue = parseMetadataInt(payloadText, "queue",
                                                   std::numeric_limits<int>::max());
                std::cout << "collected ACK payload provider="
                          << providerLabel(candidate.providerName)
                          << " queue=" << queue
                          << " rank=" << rank << std::endl;
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
                std::cout << "customSelectionStrategy selected providerName="
                          << selected.front().providerName.toUri() << std::endl;
              }
              return selected;
            }),
          20000,
          onTimeout,
          onResponse);
      }
      else {
        user.async_call(
          std::vector<ndn::Name>{},
          ndn::Name("/HELLO"),
          request,
          ackTimeoutMs,
          ndn_service_framework::ServiceUser::AckSelectionStrategy::RandomSelection,
          20000,
          onTimeout,
          onResponse);
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
