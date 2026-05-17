#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
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
  if (strategy == "no-coordination" || strategy == "NoCoordination") {
    return ndn_service_framework::tlv::NoCoordination;
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
    return "custom-selection";
  }
  if (strategy == ndn_service_framework::tlv::NoCoordination) {
    return "no-coordination";
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

} // namespace

int
main(int argc, char** argv)
{
  try {
    ndn::Face face;
    KeyChainInitLock keyChainInitLock("/tmp/ndnsf-keychain-init.lock");
    ndn::KeyChain keyChain;
    ndn::Scheduler scheduler(face.getIoContext());

    const bool useCustomSelection = hasFlag(argc, argv, "--custom-selection");
    const bool benchmark = hasFlag(argc, argv, "--benchmark");
    const int benchmarkCount = parseIntOption(argc, argv, "--count", 100);
    const int benchmarkWarmup = parseIntOption(argc, argv, "--warmup", 5);
    const int benchmarkIntervalMs = parseIntOption(argc, argv, "--interval-ms", 1000);
    const int ackTimeoutMs = parseIntOption(argc, argv, "--ack-timeout-ms", 3000);
    const int timeoutMs = parseIntOption(argc, argv, "--timeout-ms", 5000);
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

    ndn_service_framework::ServiceUser user(
      face,
      GROUP_PREFIX,
      userCert,
      controllerCert,
      "examples/trust-any.conf");

    user.init();
    user.fetchPermissionsFromController(CONTROLLER_PREFIX);

    int exitCode = 0;

    if (benchmark) {
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
            benchmarkServiceName,
            request,
            ackTimeoutMs,
            ndn_service_framework::ServiceUser::AckCandidatesHandler(
              [](const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
                std::vector<ndn_service_framework::AckSelectionCandidate> selected;
                for (const auto& candidate : candidates) {
                  if (candidate.ack.getStatus()) {
                    selected.push_back(candidate);
                    break;
                  }
                }
                return selected;
              }),
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
          ndn::Name("/HELLO"),
          request,
          ackTimeoutMs,
          ndn_service_framework::ServiceUser::AckCandidatesHandler(
            [](const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
            std::vector<ndn_service_framework::AckSelectionCandidate> selected;
            for (const auto& candidate : candidates) {
              const auto payload = candidate.ack.getPayload();
              const std::string payloadText(
                reinterpret_cast<const char*>(payload.data()),
                payload.size());
              std::cout << "[App_User] collected ACK provider="
                        << candidate.providerName.toUri()
                        << " status=" << candidate.ack.getStatus()
                        << " message=" << candidate.ack.getMessage()
                        << " payload=" << payloadText << std::endl;

              if (selected.empty() &&
                  candidate.ack.getStatus() &&
                  payloadText.find("model=hello-v1") != std::string::npos) {
                selected.push_back(candidate);
              }
            }
            if (selected.empty() && !candidates.empty()) {
              selected.push_back(candidates.front());
            }
            return selected;
            }),
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
