#include "ndn-service-framework/CertificateBootstrap.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/NegativeAckReason.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

NDN_LOG_INIT(ndn_service_framework.AppProvider);

const ndn::Name GROUP_PREFIX("/example/hello/group");
const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
const ndn::Name PROVIDER_IDENTITY("/example/hello/provider");

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

bool
parseAckStatus(const std::string& value)
{
  return !(value == "false" || value == "0" || value == "reject" || value == "no");
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

uint64_t
nowMilliseconds()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
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

    const std::string providerId = getOption(argc, argv, "--provider-id", "");
    const bool benchmark = hasFlag(argc, argv, "--benchmark");
    const bool performanceMode = hasFlag(argc, argv, "--performance-mode");
    auto perfLogGate = std::make_shared<SampledLogGate>(10);
    const bool useTokens = !hasFlag(argc, argv, "--disable-tokens");
    const bool timelineTrace = hasFlag(argc, argv, "--timeline-trace");
    const bool adaptiveProviderAck = hasFlag(argc, argv, "--adaptive-provider-ack");
    const bool dkBootstrapOnly = hasFlag(argc, argv, "--dk-bootstrap-only");
    const bool serveCertificates = !hasFlag(argc, argv, "--no-serve-certificates");
    const bool largeDataFetchTest = hasFlag(argc, argv, "--large-data-fetch-test");
    const bool expectLargeDataFailure = hasFlag(argc, argv, "--expect-large-data-failure");
    const std::string providerLabel = providerId.empty() ? "default" : providerId;
    const ndn::Name providerIdentity = providerId.empty()
      ? PROVIDER_IDENTITY
      : ndn::Name(PROVIDER_IDENTITY).append(providerId);
    const std::string largeDataNameText =
      getOption(argc, argv, "--large-data-name", "");
    const std::string expectedLargeDataPlaintext =
      getOption(argc, argv, "--expect-large-data-plaintext", "");
    const std::string ackPayloadText = getOption(
      argc, argv, "--ack-payload", "queue=0;gpu=idle;model=hello-v1");
    const bool ackStatus = parseAckStatus(
      getOption(argc, argv, "--ack-status", "true"));
    const std::string ackMessage = getOption(
      argc, argv, "--ack-message",
      ackStatus ? "HELLO provider ready" :
                  ndn_service_framework::negative_ack_reason::ProviderBusy);
    const std::string responseText = getOption(
      argc, argv, "--response-payload", "HELLO");
    const int providerAckMaxPending = parseIntOption(
      argc, argv, "--provider-ack-max-pending", 1000);
    const int providerAckMaxEventLoopLagMs = parseIntOption(
      argc, argv, "--provider-ack-max-event-loop-lag-ms", 0);
    const int providerAckMaxSelectionLagMs = parseIntOption(
      argc, argv, "--provider-ack-max-selection-lag-ms", 0);
    const int handlerThreads = parseIntOption(argc, argv, "--handler-threads", -1);
    const int ackThreads = parseIntOption(argc, argv, "--ack-threads", -1);
    const int providerRequestDelayMs = parseIntOption(
      argc, argv, "--provider-request-delay-ms", 0);
    const std::string providerLifecycleCsv =
      getOption(argc, argv, "--provider-lifecycle-csv", "");
    const std::string bootstrapToken =
      getOption(argc, argv, "--bootstrap-token", "");
    const ndn::Name bootstrapName(
      getOption(argc, argv, "--bootstrap-name", providerIdentity.toUri()));

    auto providerCert = getOrCreateIdentity(keyChain, providerIdentity);
    auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);
    if (!bootstrapToken.empty()) {
      providerCert = ndn_service_framework::ensureControllerSignedCertificate(
        face, keyChain, CONTROLLER_PREFIX, providerIdentity, bootstrapName, bootstrapToken);
    }
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(providerIdentity));
    keyChainInitLock.unlock();

    NDN_LOG_INFO( "[App_Provider] provider identity="
              << providerIdentity.toUri()
              << " providerId=" << providerLabel
              << " benchmark=" << benchmark
              << " performanceMode=" << performanceMode
              << " tokenMode=" << (useTokens ? "enabled" : "disabled")
              << " hybridMessageCrypto=enabled"
              << " timelineTrace=" << timelineTrace
              << " adaptiveProviderAck=" << adaptiveProviderAck
              << " providerAckMaxPending=" << providerAckMaxPending
              << " providerAckMaxEventLoopLagMs=" << providerAckMaxEventLoopLagMs
              << " providerAckMaxSelectionLagMs=" << providerAckMaxSelectionLagMs
              << " handlerThreads=" << handlerThreads
              << " providerRequestDelayMs=" << providerRequestDelayMs
              << " dkBootstrapOnly=" << dkBootstrapOnly
              << " serveCertificates=" << serveCertificates
              << " ackStatus=" << ackStatus
              << " ackPayload=" << ackPayloadText
              << " responsePayload=" << responseText
             );

    const auto routeRegistrationLockPath =
      userScopedLockPath("/tmp/ndnsf-provider-route-registration");
    KeyChainInitLock routeRegistrationLock(routeRegistrationLockPath.c_str());
    std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
    if (serveCertificates) {
      certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
        face, keyChain, providerCert.getName());
    }
    ndn_service_framework::ServiceProvider provider(
      face,
      GROUP_PREFIX,
      providerCert,
      controllerCert,
      "examples/trust-schema.conf");
    provider.setPerformanceMode(performanceMode);
    provider.setUseTokens(useTokens);
    provider.setTimelineTrace(timelineTrace);
    provider.setAdaptiveAckAdmission(adaptiveProviderAck);
    provider.setProviderAckMaxPending(
      providerAckMaxPending > 0 ? static_cast<size_t>(providerAckMaxPending) : 0);
    provider.setProviderAckMaxEventLoopLag(
      ndn::time::milliseconds(std::max(0, providerAckMaxEventLoopLagMs)));
    provider.setProviderAckMaxSelectionLag(
      ndn::time::milliseconds(std::max(0, providerAckMaxSelectionLagMs)));
    if (handlerThreads >= 0) {
      provider.setHandlerThreads(static_cast<size_t>(handlerThreads));
    }
    if (ackThreads >= 0) {
      provider.setAckThreads(static_cast<size_t>(ackThreads));
    }
    std::shared_ptr<std::ofstream> providerLifecycleStream;
    if (benchmark && !providerLifecycleCsv.empty()) {
      providerLifecycleStream =
        std::make_shared<std::ofstream>(providerLifecycleCsv);
      if (*providerLifecycleStream) {
        const auto providerLifecycleSampleRate =
          envSizeOption("NDNSF_TIMELINE_TRACE_SAMPLE_RATE", 100);
        *providerLifecycleStream
          << "request_id,service_name,provider_name,state,"
          << "request_observed_timestamp_us,ack_admission_decision_timestamp_us,"
          << "ack_published_or_suppressed_timestamp_us,suppression_reason,"
          << "provider_pending_count_at_decision,event_loop_lag_us,"
          << "selection_lag_us,selection_received_timestamp_us,"
          << "execution_start_timestamp_us,execution_done_timestamp_us,"
          << "response_published_timestamp_us,final_status\n";
        provider.setProviderRequestLifecycleCallback(
          [providerLifecycleStream, providerLifecycleSampleRate](
            const ndn_service_framework::ServiceProvider::ProviderRequestLifecycleStatus& status) {
            if (!sampleByRequestId(status.requestId, providerLifecycleSampleRate)) {
              return;
            }
            *providerLifecycleStream
              << status.requestId.toUri() << ","
              << status.serviceName.toUri() << ","
              << status.providerName.toUri() << ","
              << ndn_service_framework::ServiceProvider::providerRequestLifecycleStateToString(status.state) << ","
              << status.requestObservedTimestampUs << ","
              << status.ackAdmissionDecisionTimestampUs << ","
              << status.ackPublishedOrSuppressedTimestampUs << ","
              << status.suppressionReason << ","
              << status.providerPendingCountAtDecision << ","
              << status.eventLoopLagUs << ","
              << status.selectionLagUs << ","
              << status.selectionReceivedTimestampUs << ","
              << status.executionStartTimestampUs << ","
              << status.executionDoneTimestampUs << ","
              << status.responsePublishedTimestampUs << ","
              << status.finalStatus << "\n";
          });
      }
    }
    routeRegistrationLock.unlock();

    if (dkBootstrapOnly) {
      NDN_LOG_INFO( "DK_BOOTSTRAP_ONLY success=1 provider="
                << providerIdentity.toUri()
                << " authority=" << controllerCert.getIdentity().toUri()
               );
      return 0;
    }

    auto executingRequests = std::make_shared<std::atomic<size_t>>(0);
    auto completionTimes =
      std::make_shared<std::deque<std::chrono::steady_clock::time_point>>();
    auto completionTimesMutex = std::make_shared<std::mutex>();

    provider.addService(
      ndn::Name("/HELLO"),
      ndn_service_framework::ServiceProvider::AckStrategyHandler(
        [providerLabel, ackStatus, ackMessage, ackPayloadText, performanceMode, perfLogGate,
         &provider, executingRequests, completionTimes, completionTimesMutex](
          const ndn_service_framework::RequestMessage&) {
          const bool logThisRequest = !performanceMode && perfLogGate->allow();
          if (logThisRequest) {
            NDN_LOG_INFO("Provider " << providerLabel
                         << " selective ACK handler received request");
            NDN_LOG_INFO("Provider " << providerLabel
                         << " request received timestampMs=" << nowMilliseconds());
          }
          if (!ackStatus && logThisRequest) {
            NDN_LOG_INFO("Provider " << providerLabel
                         << " selective ACK handler rejected request");
          }

          const auto now = std::chrono::steady_clock::now();
          size_t processed10s = 0;
          {
            std::lock_guard<std::mutex> lock(*completionTimesMutex);
            while (!completionTimes->empty() &&
                   now - completionTimes->front() > std::chrono::seconds(10)) {
              completionTimes->pop_front();
            }
            processed10s = completionTimes->size();
          }

          const size_t backlog =
            provider.getSelectedOutstandingRequestCountForTesting();
          const int rank = parseMetadataInt(ackPayloadText, "rank", 1);
          std::ostringstream metadataStream;
          metadataStream << "backlog=" << backlog
                         << ";processed10s=" << processed10s
                         << ";queue=" << backlog
                         << ";rank=" << rank;
          const std::string metadata = metadataStream.str();
          ndn::Buffer ackPayload(
            reinterpret_cast<const uint8_t*>(metadata.data()),
            metadata.size());

          ndn_service_framework::ServiceProvider::AckDecision decision;
          decision.status = ackStatus;
          decision.message = ackMessage;
          decision.payload = ackPayload;
          if (logThisRequest) {
            NDN_LOG_INFO("Provider " << providerLabel
                         << " publishing HELLO ACK status=" << decision.status
                         << " message=" << decision.message
                         << " payload=" << metadata);
            NDN_LOG_INFO("Publishing HELLO ACK payload: " << metadata);
          }
          return decision;
        }),
      std::function<ndn_service_framework::ResponseMessage(
        const ndn::Name&,
        const ndn::Name&,
        const ndn::Name&,
        const ndn::Name&,
        const ndn_service_framework::RequestMessage&)>(
        [providerLabel, responseText, performanceMode, perfLogGate, providerRequestDelayMs,
         executingRequests, completionTimes, completionTimesMutex](
          const ndn::Name&,
          const ndn::Name&,
          const ndn::Name& serviceName,
          const ndn::Name&,
          const ndn_service_framework::RequestMessage& request) {
          const auto requestPayload = request.getPayload();
          const std::string requestText(
            reinterpret_cast<const char*>(requestPayload.data()),
            requestPayload.size());

          if (requestText != "HELLO") {
            ndn_service_framework::ResponseMessage response;
            response.setStatus(false);
            response.setErrorInfo("Unexpected payload for " + serviceName.toUri());
            return response;
          }

          executingRequests->fetch_add(1, std::memory_order_relaxed);
          if (providerRequestDelayMs > 0) {
            std::this_thread::sleep_for(
              std::chrono::milliseconds(providerRequestDelayMs));
          }
          executingRequests->fetch_sub(1, std::memory_order_relaxed);
          {
            std::lock_guard<std::mutex> lock(*completionTimesMutex);
            completionTimes->push_back(std::chrono::steady_clock::now());
          }

          if (!performanceMode && perfLogGate->allow()) {
            NDN_LOG_INFO("Received HELLO request");
            NDN_LOG_INFO("Provider " << providerLabel
                         << " executing selected request");
            NDN_LOG_INFO("Provider " << providerLabel
                         << " publishing final response: " << responseText);
          }

          ndn::Buffer responsePayload(
            reinterpret_cast<const uint8_t*>(responseText.data()),
            responseText.size());

          ndn_service_framework::ResponseMessage response;
          response.setStatus(true);
          response.setErrorInfo("No error");
          response.setPayload(responsePayload, responsePayload.size());
          return response;
        }),
      ndn_service_framework::ServiceProvider::ServiceInvocationMode::NormalAndTargeted);
    provider.init();
    provider.fetchPermissionsFromController(CONTROLLER_PREFIX);

    if (largeDataFetchTest) {
      if (largeDataNameText.empty()) {
        std::cerr << "LARGE_DATA_FETCH_FAILURE error=--large-data-name is required"
                  << std::endl;
        return 2;
      }

      const auto result =
        provider.fetchAndDecryptLargeData(ndn::Name(largeDataNameText), "/HELLO");
      if (expectLargeDataFailure) {
        if (!result.success && !result.errorMessage.empty()) {
          NDN_LOG_INFO( "LARGE_DATA_UNAUTHORIZED_FAILURE_CLEAN error="
                    << result.errorMessage);
          return 0;
        }
        std::cerr << "LARGE_DATA_UNAUTHORIZED_FAILURE_EXPECTED success="
                  << result.success << " error=" << result.errorMessage
                  << std::endl;
        return 1;
      }

      if (!result.success) {
        std::cerr << "LARGE_DATA_FETCH_FAILURE error="
                  << result.errorMessage << std::endl;
        return 1;
      }

      const std::string plaintext(result.plaintext.begin(), result.plaintext.end());
      if (!expectedLargeDataPlaintext.empty() &&
          plaintext != expectedLargeDataPlaintext) {
        std::cerr << "LARGE_DATA_FETCH_FAILURE error=plaintext mismatch expected="
                  << expectedLargeDataPlaintext
                  << " actual=" << plaintext << std::endl;
        return 1;
      }

      NDN_LOG_INFO( "LARGE_DATA_FETCH_SUCCESS plaintext="
                << plaintext);
      return 0;
    }

    NDN_LOG_INFO( "Provider " << providerLabel
              << " registered service /HELLO");
    face.processEvents();
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "App_Provider error: " << e.what() << std::endl;
    return 1;
  }
}
