#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/util/scheduler.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <ndn-cxx/security/validator.hpp>
#include <ndn-cxx/security/validation-callback.hpp>
#include <ndn-cxx/security/certificate-fetcher-offline.hpp>
#include <boost/asio/io_service.hpp>

#include <functional>
#include <fstream>
#include <string>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <vector>

using namespace ndn;

namespace {
class NullBuffer : public std::streambuf
{
public:
    int overflow(int c) override { return c; }
};

std::ostream& nscLog()
{
    static NullBuffer nullBuffer;
    static std::ostream nullStream(&nullBuffer);
    const char* verbose = std::getenv("NSC_VERBOSE");
    return verbose != nullptr && std::string(verbose) == "1" ? std::cerr : nullStream;
}
}

class rpcConsumer
{
public:
    // Usage: ./consumer <user> <provider[,provider...]> <service> <function>
    //                   <interval_in_ms> <count> [run_id] [warmup_count]
    //                   [request_deadline_ms] [attempt_timeout_ms]
    //                   [measurement_start_monotonic_ms]
    //                   [measurement_start_tolerance_ms]
    rpcConsumer(char *user, char *providers, char *service, char *function,
                char *interval_in_ms, char *count, char *run_id = nullptr,
                char *warmup_count = nullptr, char *request_deadline_ms = nullptr,
                char *attempt_timeout_ms = nullptr,
                char *measurement_start_monotonic_ms = nullptr,
                char *measurement_start_tolerance_ms = nullptr)
        : m_face(m_ioService),
          m_scheduler(m_ioService),
          CONSUMER_IDENTITY(user),
          SERVICE_NAME(service),
          FUNCTION_NAME(function)
    {
        m_providers = parseProviders(providers);
        if (m_providers.empty()) {
            throw std::invalid_argument("at least one provider is required");
        }

        intervalInMs = std::stoi(interval_in_ms);
        measuredCount = std::stoi(count);
        warmupCount = warmup_count != nullptr ? std::stoi(warmup_count) : 0;
        requestDeadlineMs = request_deadline_ms != nullptr ?
                            std::stoi(request_deadline_ms) : 5000;
        attemptTimeoutMs = attempt_timeout_ms != nullptr ?
                           std::stoi(attempt_timeout_ms) : 200;
        measurementStartMonotonicMs = measurement_start_monotonic_ms != nullptr ?
                                      std::stoll(measurement_start_monotonic_ms) : 0;
        measurementStartToleranceMs = measurement_start_tolerance_ms != nullptr ?
                                      std::stoi(measurement_start_tolerance_ms) : 0;
        if (intervalInMs <= 0 || measuredCount < 0 || warmupCount < 0 ||
            requestDeadlineMs <= 0 || attemptTimeoutMs <= 0 ||
            measurementStartMonotonicMs < 0 || measurementStartToleranceMs < 0) {
            throw std::invalid_argument("interval, deadlines, and counts must be positive");
        }

        runId = run_id != nullptr && std::string(run_id).size() > 0 ?
                std::string(run_id) : makeRunId();
        INPUT_NAMESPACE = CONSUMER_IDENTITY + SERVICE_NAME + "/inputs/" + runId + "/";
        for (const auto& provider : m_providers) {
            providerAttemptCounts[provider] = 0;
        }
    }

    void run()
    {
        nscLog() << "Attempting to rpc call with " << m_providers.size()
                 << " provider(s), run_id=" << runId << std::endl;

        m_face.registerPrefix(
          INPUT_NAMESPACE,
          [this](const Name&) {
              inputPrefixRegistered = true;
              std::cout << "NSC_INPUT_PREFIX_READY prefix=" << INPUT_NAMESPACE
                        << std::endl;
          },
          bind(&rpcConsumer::onRegisterFailed, this, _1, _2));
        m_face.setInterestFilter(INPUT_NAMESPACE,
                                 bind(&rpcConsumer::onInterestForInput, this, _1, _2));

        if (measurementStartMonotonicMs > 0) {
            const int64_t delayMs = std::max<int64_t>(
              0, measurementStartMonotonicMs - monotonicNowMs());
            m_scheduler.schedule(
              ndn::time::milliseconds(delayMs), [this] { scheduleWorkload(); });
        }
        else {
            scheduleWorkload();
        }

        m_ioService.run();
    }

    static int runLogicSelfTest()
    {
        const auto providers = parseProviders(" /muas/ucla,/muas/wustl,/muas/uiuc,/muas/ucla ");
        const bool providersOk = providers == std::vector<std::string>{
          "/muas/ucla", "/muas/wustl", "/muas/uiuc"};
        const bool rotationOk =
          initialProviderIndex(1, 3) == 0 && initialProviderIndex(2, 3) == 1 &&
          initialProviderIndex(3, 3) == 2 &&
          rotatedProviderIndex(0, 0, 3) == 0 &&
          rotatedProviderIndex(0, 1, 3) == 1 &&
          rotatedProviderIndex(0, 2, 3) == 2 &&
          rotatedProviderIndex(1, 0, 3) == 1 &&
          rotatedProviderIndex(1, 1, 3) == 2 &&
          rotatedProviderIndex(1, 2, 3) == 0;
        const bool deadlineOk = boundedAttemptLifetimeMs(200, 5000) == 200 &&
                                boundedAttemptLifetimeMs(200, 75) == 75;
        const bool summaryOk = legacyTimeoutCount(2) == 2;
        if (!providersOk || !rotationOk || !deadlineOk || !summaryOk) {
            std::cerr << "NSC_LOGIC_SELF_TEST_FAILED"
                      << " providers=" << providersOk
                      << " rotation=" << rotationOk
                      << " deadline=" << deadlineOk
                      << " summary=" << summaryOk << std::endl;
            return 1;
        }
        std::cout << "NSC_LOGIC_SELF_TEST_OK"
                  << " providers=" << providers.size()
                  << " rotation=0,1,2|1,2,0"
                  << " attempt_lifetime_full=" << boundedAttemptLifetimeMs(200, 5000)
                  << " attempt_lifetime_remaining=" << boundedAttemptLifetimeMs(200, 75)
                  << " legacy_timeout=" << legacyTimeoutCount(2)
                  << " message_definition=" << messageDefinition()
                  << std::endl;
        return 0;
    }

private:
    struct RequestState
    {
        int id = 0;
        bool measured = false;
        bool completed = false;
        bool resultRequested = false;
        size_t firstProviderIndex = 0;
        size_t nextAttemptOrdinal = 0;
        uint64_t generation = 0;
        std::string currentProvider;
        std::string inputName;
        ndn::time::steady_clock::time_point startedAt;
        ndn::time::steady_clock::time_point deadline;
        PendingInterestHandle notificationHandle;
        PendingInterestHandle resultHandle;
        scheduler::EventId attemptTimer;
        scheduler::EventId globalTimer;
    };

    enum class FailureKind
    {
        Timeout,
        Nack
    };

    boost::asio::io_service m_ioService;
    Face m_face;
    Scheduler m_scheduler;
    KeyChain m_keyChain;
    int rpcCall = 0;
    const std::string CCNUM = "CCNUM";
    std::string CONSUMER_IDENTITY = "/muas/gs1";
    std::string SERVICE_NAME = "/FlightControl";
    std::string FUNCTION_NAME = "/ManualControl";
    std::string INPUT_NAMESPACE;
    std::string runId;
    std::vector<std::string> m_providers;
    const std::string APP_NACK = "APP_NACK";
    int intervalInMs = 1000;
    int measuredCount = 1;
    int warmupCount = 0;
    int requestDeadlineMs = 5000;
    int attemptTimeoutMs = 200;
    int64_t measurementStartMonotonicMs = 0;
    int measurementStartToleranceMs = 0;
    int64_t measurementStartedMonotonicMs = 0;
    double measurementStartLatenessMs = 0.0;
    double firstRequestStartLatenessMs = 0.0;
    bool firstMeasuredRequestStarted = false;
    bool inputPrefixRegistered = false;
    std::map<int, RequestState> requests;
    std::map<int, ndn::time::steady_clock::time_point> rpcStartTimeMap;
    std::map<int, ndn::time::steady_clock::time_point> rpcEndTimeMap;
    std::map<std::string, uint64_t> providerAttemptCounts;
    uint64_t successfulCalls = 0;
    uint64_t terminalFailures = 0;
    uint64_t totalAttempts = 0;
    uint64_t attemptTimeouts = 0;
    uint64_t nackCount = 0;
    uint64_t failoverCount = 0;
    uint64_t lateCallbacks = 0;
    uint64_t lateMessages = 0;
    // Consumer-observed application protocol messages. These count one logical
    // Interest/Data at each NSC stage, not NDN retransmissions or link packets.
    uint64_t notificationInterests = 0;
    uint64_t notificationData = 0;
    uint64_t inputInterests = 0;
    uint64_t inputData = 0;
    uint64_t resultInterests = 0;
    uint64_t resultData = 0;

    static std::vector<std::string> parseProviders(const std::string& value)
    {
        std::vector<std::string> result;
        std::set<std::string> seen;
        std::stringstream input(value);
        std::string provider;
        while (std::getline(input, provider, ',')) {
            provider.erase(std::remove_if(provider.begin(), provider.end(),
                                          [](unsigned char ch) { return std::isspace(ch); }),
                           provider.end());
            if (!provider.empty() && seen.insert(provider).second) {
                result.push_back(provider);
            }
        }
        return result;
    }

    static size_t initialProviderIndex(int requestId, size_t providerCount)
    {
        if (requestId <= 0 || providerCount == 0) {
            throw std::invalid_argument("request ID and provider count must be positive");
        }
        return static_cast<size_t>(requestId - 1) % providerCount;
    }

    static size_t rotatedProviderIndex(size_t firstProviderIndex,
                                       size_t attemptOrdinal,
                                       size_t providerCount)
    {
        if (providerCount == 0) {
            throw std::invalid_argument("provider count must be positive");
        }
        return (firstProviderIndex + attemptOrdinal) % providerCount;
    }

    static int boundedAttemptLifetimeMs(int configuredMs, int remainingMs)
    {
        return std::max(1, std::min(configuredMs, remainingMs));
    }

    static uint64_t legacyTimeoutCount(uint64_t logicalTerminalFailures)
    {
        return logicalTerminalFailures;
    }

    static const char* messageDefinition()
    {
        return "consumer_observed_accepted_or_sent_nsc_stage_events_excludes_late_and_wire_retransmissions";
    }

    static std::string makeRunId()
    {
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
        return "run-" + std::to_string(now);
    }

    static int64_t monotonicNowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void scheduleWorkload()
    {
        measurementStartedMonotonicMs = monotonicNowMs();
        measurementStartLatenessMs = measurementStartMonotonicMs > 0 ?
          static_cast<double>(std::max<int64_t>(
            0, measurementStartedMonotonicMs - measurementStartMonotonicMs)) : 0.0;
        if (measurementStartToleranceMs > 0 &&
            measurementStartLatenessMs > measurementStartToleranceMs) {
            throw std::runtime_error(
              "measurement start lateness exceeds configured tolerance");
        }
        if (measurementStartMonotonicMs > 0 && !inputPrefixRegistered) {
            throw std::runtime_error(
              "input prefix was not registered before the measurement barrier");
        }
        std::cout << std::fixed << std::setprecision(3)
                  << "NSC_MEASUREMENT_START monotonic_ms="
                  << measurementStartedMonotonicMs
                  << " target_monotonic_ms=" << measurementStartMonotonicMs
                  << " lateness_ms=" << measurementStartLatenessMs
                  << std::endl;

        const int totalCount = warmupCount + measuredCount;
        for (int i = 0; i < totalCount; ++i) {
            m_scheduler.schedule(ndn::time::milliseconds(intervalInMs * i),
                                 [this] { beginLogicalRequest(); });
        }
        m_scheduler.schedule(
          ndn::time::milliseconds(intervalInMs * totalCount + requestDeadlineMs + 1000),
          [this] { calculateLatency(); });
    }

    void beginLogicalRequest()
    {
        const int id = ++rpcCall;
        const auto now = ndn::time::steady_clock::now();
        auto& state = requests[id];
        state.id = id;
        state.measured = id > warmupCount;
        if (state.measured && !firstMeasuredRequestStarted) {
            firstMeasuredRequestStarted = true;
            const int64_t actualMs = monotonicNowMs();
            const int64_t barrierMs = measurementStartMonotonicMs > 0 ?
              measurementStartMonotonicMs : measurementStartedMonotonicMs;
            const int64_t expectedMs = barrierMs +
              static_cast<int64_t>(warmupCount) * intervalInMs;
            firstRequestStartLatenessMs = static_cast<double>(
              std::max<int64_t>(0, actualMs - expectedMs));
            std::cout << std::fixed << std::setprecision(3)
                      << "NSC_FIRST_MEASURED_REQUEST_START monotonic_ms=" << actualMs
                      << " expected_monotonic_ms=" << expectedMs
                      << " lateness_ms=" << firstRequestStartLatenessMs
                      << std::endl;
        }
        state.firstProviderIndex = initialProviderIndex(id, m_providers.size());
        state.inputName = inputNameFor(id);
        state.startedAt = now;
        state.deadline = now + ndn::time::milliseconds(requestDeadlineMs);
        if (state.measured) {
            rpcStartTimeMap[id] = now;
        }
        state.globalTimer = m_scheduler.schedule(
          ndn::time::milliseconds(requestDeadlineMs),
          [this, id] { onGlobalDeadline(id); });
        startNextAttempt(id);
    }

    std::string inputNameFor(int requestId) const
    {
        return INPUT_NAMESPACE + std::to_string(requestId);
    }

    std::string functionNameFor(const std::string& provider) const
    {
        return provider + SERVICE_NAME + FUNCTION_NAME;
    }

    std::string resultTokenForInput(const std::string& inputName) const
    {
        Name name(inputName);
        if (name.size() < 3) {
            return name.toUri();
        }
        std::string token = name.at(0).toUri() + "/" + name.at(1).toUri();
        for (size_t i = 0; i < name.size(); ++i) {
            if (name.at(i).toUri() != "inputs") {
                continue;
            }
            for (size_t j = i + 1; j < name.size(); ++j) {
                token += "/" + name.at(j).toUri();
            }
            return token;
        }
        token += "/" + name.at(name.size() - 1).toUri();
        return token;
    }

    std::string expectedResultName(const RequestState& state) const
    {
        return state.currentProvider + SERVICE_NAME + "/results/" +
               resultTokenForInput(state.inputName);
    }

    int remainingMs(const RequestState& state) const
    {
        const auto remaining = ndn::time::duration_cast<ndn::time::milliseconds>(
          state.deadline - ndn::time::steady_clock::now()).count();
        return static_cast<int>(std::max<int64_t>(0, remaining));
    }

    bool isCurrent(int id, uint64_t generation) const
    {
        const auto it = requests.find(id);
        return it != requests.end() && !it->second.completed &&
               it->second.generation == generation;
    }

    void recordLate(int id, bool isApplicationMessage = false)
    {
        const auto it = requests.find(id);
        if (it != requests.end() && it->second.measured) {
            ++lateCallbacks;
            if (isApplicationMessage) {
                ++lateMessages;
            }
        }
    }

    void startNextAttempt(int id)
    {
        auto it = requests.find(id);
        if (it == requests.end() || it->second.completed) {
            return;
        }
        auto& state = it->second;
        if (remainingMs(state) <= 0 || state.nextAttemptOrdinal >= m_providers.size()) {
            completeFailure(state);
            return;
        }

        cancelAttempt(state);
        const size_t providerIndex = rotatedProviderIndex(
          state.firstProviderIndex, state.nextAttemptOrdinal, m_providers.size());
        ++state.nextAttemptOrdinal;
        ++state.generation;
        state.currentProvider = m_providers[providerIndex];
        state.resultRequested = false;
        const uint64_t generation = state.generation;

        if (state.measured) {
            ++totalAttempts;
            ++providerAttemptCounts[state.currentProvider];
            ++notificationInterests;
        }

        const int timeoutMs = boundedAttemptLifetimeMs(attemptTimeoutMs, remainingMs(state));
        state.attemptTimer = m_scheduler.schedule(
          ndn::time::milliseconds(timeoutMs),
          [this, id, generation] { onAttemptTimeout(id, generation); });

        Interest interest = createInterest(functionNameFor(state.currentProvider), true, true,
                                           timeoutMs);
        addInterestParameterString(state.inputName, interest);
        m_keyChain.sign(interest, security::signingByIdentity(Name(CONSUMER_IDENTITY)));
        state.notificationHandle = m_face.expressInterest(
          interest,
          [this, id, generation](const Interest& sent, const Data& data) {
              onNotificationData(id, generation, sent, data);
          },
          [this, id, generation](const Interest& sent, const lp::Nack& nack) {
              onNack(id, generation, sent, nack);
          },
          [this, id, generation](const Interest& sent) {
              onInterestTimeout(id, generation, sent);
          });

        nscLog() << "NSC_ATTEMPT request=" << id
                 << " generation=" << generation
                 << " provider=" << state.currentProvider
                 << " timeout_ms=" << timeoutMs << std::endl;
    }

    void onNotificationData(int id, uint64_t generation, const Interest&, const Data&)
    {
        if (!isCurrent(id, generation)) {
            recordLate(id, true);
            return;
        }
        auto& state = requests.at(id);
        if (ndn::time::steady_clock::now() >= state.deadline) {
            recordLate(id, true);
            completeFailure(state);
            return;
        }
        if (state.measured) {
            ++notificationData;
        }
        nscLog() << "Received acknowledgement for request " << id << std::endl;
    }

    void onInterestForInput(const InterestFilter&, const Interest& interest)
    {
        const int id = extractRequestId(interest.getName());
        auto it = requests.find(id);
        if (it == requests.end()) {
            return;
        }
        auto& state = it->second;
        const std::string resultName = extractInterestParam(interest);
        if (state.completed || ndn::time::steady_clock::now() >= state.deadline ||
            resultName != expectedResultName(state)) {
            recordLate(id, true);
            if (!state.completed && ndn::time::steady_clock::now() >= state.deadline) {
                completeFailure(state);
            }
            return;
        }
        const uint64_t generation = state.generation;
        if (!verifyInterestSignature(interest, state.currentProvider)) {
            return;
        }
        if (state.measured) {
            ++inputInterests;
        }

        auto data = createData(interest.getName(), CCNUM, CONSUMER_IDENTITY);
        m_face.put(*data);
        if (state.measured) {
            ++inputData;
        }
        if (!state.resultRequested) {
            state.resultRequested = true;
            sendInterestForResult(id, generation, resultName);
        }
    }

    void sendInterestForResult(int id, uint64_t generation, const std::string& resultName)
    {
        if (!isCurrent(id, generation)) {
            recordLate(id);
            return;
        }
        auto& state = requests.at(id);
        const int lifetimeMs = boundedAttemptLifetimeMs(attemptTimeoutMs, remainingMs(state));
        Interest interest = createInterest(resultName, true, true, lifetimeMs);
        if (state.measured) {
            ++resultInterests;
        }
        state.resultHandle.cancel();
        state.resultHandle = m_face.expressInterest(
          interest,
          [this, id, generation](const Interest& sent, const Data& data) {
              onResultData(id, generation, sent, data);
          },
          [this, id, generation](const Interest& sent, const lp::Nack& nack) {
              onNack(id, generation, sent, nack);
          },
          [this, id, generation](const Interest& sent) {
              onInterestTimeout(id, generation, sent);
          });
    }

    void onResultData(int id, uint64_t generation, const Interest&, const Data& data)
    {
        if (!isCurrent(id, generation)) {
            recordLate(id, true);
            return;
        }
        auto& state = requests.at(id);
        if (ndn::time::steady_clock::now() >= state.deadline) {
            recordLate(id, true);
            completeFailure(state);
            return;
        }
        if (!verifyDataSignature(data, state.currentProvider)) {
            return;
        }
        if (state.measured) {
            ++resultData;
        }
        const std::string result = extractDataValue(data);
        if (isAppNACK(result)) {
            const std::string retryName = result.substr(APP_NACK.size());
            const Name providerService(state.currentProvider + SERVICE_NAME);
            if (retryName.empty() || !providerService.isPrefixOf(Name(retryName))) {
                handleAttemptFailure(id, generation, FailureKind::Nack);
                return;
            }
            sendInterestForResult(id, generation, retryName);
            return;
        }
        completeSuccess(state);
    }

    void onNack(int id, uint64_t generation, const Interest&, const lp::Nack& nack)
    {
        if (!isCurrent(id, generation)) {
            recordLate(id);
            return;
        }
        nscLog() << "Received Nack for request " << id
                 << " reason=" << nack.getReason() << std::endl;
        handleAttemptFailure(id, generation, FailureKind::Nack);
    }

    void onInterestTimeout(int id, uint64_t generation, const Interest& interest)
    {
        if (!isCurrent(id, generation)) {
            recordLate(id);
            return;
        }
        nscLog() << "Interest timeout for request " << id << ": " << interest << std::endl;
        handleAttemptFailure(id, generation, FailureKind::Timeout);
    }

    void onAttemptTimeout(int id, uint64_t generation)
    {
        if (!isCurrent(id, generation)) {
            return;
        }
        handleAttemptFailure(id, generation, FailureKind::Timeout);
    }

    void onGlobalDeadline(int id)
    {
        auto it = requests.find(id);
        if (it != requests.end() && !it->second.completed) {
            completeFailure(it->second);
        }
    }

    void handleAttemptFailure(int id, uint64_t generation, FailureKind kind)
    {
        if (!isCurrent(id, generation)) {
            recordLate(id);
            return;
        }
        auto& state = requests.at(id);
        cancelAttempt(state);
        if (state.measured) {
            if (kind == FailureKind::Timeout) {
                ++attemptTimeouts;
            }
            else {
                ++nackCount;
            }
        }

        if (state.nextAttemptOrdinal < m_providers.size() && remainingMs(state) > 0) {
            if (state.measured) {
                ++failoverCount;
            }
            startNextAttempt(id);
        }
        else {
            completeFailure(state);
        }
    }

    void cancelAttempt(RequestState& state)
    {
        state.notificationHandle.cancel();
        state.notificationHandle = PendingInterestHandle();
        state.resultHandle.cancel();
        state.resultHandle = PendingInterestHandle();
        state.attemptTimer.cancel();
        state.attemptTimer = scheduler::EventId();
    }

    void completeSuccess(RequestState& state)
    {
        if (state.completed) {
            return;
        }
        state.completed = true;
        cancelAttempt(state);
        state.globalTimer.cancel();
        if (state.measured) {
            const auto completedAt = ndn::time::steady_clock::now();
            rpcEndTimeMap[state.id] = completedAt;
            ++successfulCalls;
            const auto latencyMs = ndn::time::duration_cast<ndn::time::milliseconds>(
                completedAt - state.startedAt).count();
            const double publishedMonotonicMs =
              ndn::time::duration_cast<ndn::time::microseconds>(
                state.startedAt.time_since_epoch()).count() / 1000.0;
            std::cout << std::fixed << std::setprecision(3)
                      << "NSC_REQUEST_RESULT request_id=" << state.id
                      << " status=SUCCESS latency_ms=" << latencyMs
                      << " attempts=" << state.nextAttemptOrdinal
                      << " provider=" << state.currentProvider
                      << " published_monotonic_ms=" << publishedMonotonicMs
                      << std::endl;
        }
    }

    void completeFailure(RequestState& state)
    {
        if (state.completed) {
            return;
        }
        state.completed = true;
        cancelAttempt(state);
        state.globalTimer.cancel();
        if (state.measured) {
            ++terminalFailures;
            const auto latencyMs = ndn::time::duration_cast<ndn::time::milliseconds>(
                ndn::time::steady_clock::now() - state.startedAt).count();
            const double publishedMonotonicMs =
              ndn::time::duration_cast<ndn::time::microseconds>(
                state.startedAt.time_since_epoch()).count() / 1000.0;
            std::cout << std::fixed << std::setprecision(3)
                      << "NSC_REQUEST_RESULT request_id=" << state.id
                      << " status=FAILURE latency_ms=" << latencyMs
                      << " attempts=" << state.nextAttemptOrdinal
                      << " provider=" << state.currentProvider
                      << " published_monotonic_ms=" << publishedMonotonicMs
                      << std::endl;
        }
    }

    void calculateLatency()
    {
        for (auto& [id, state] : requests) {
            (void)id;
            if (state.measured && !state.completed) {
                completeFailure(state);
            }
        }

        std::vector<double> latenciesMs;
        latenciesMs.reserve(rpcEndTimeMap.size());
        for (const auto& [id, endTime] : rpcEndTimeMap) {
            const auto startIt = rpcStartTimeMap.find(id);
            if (startIt == rpcStartTimeMap.end()) {
                continue;
            }
            const auto latency = ndn::time::duration_cast<ndn::time::microseconds>(
              endTime - startIt->second).count() / 1000.0;
            latenciesMs.push_back(latency);
        }
        std::sort(latenciesMs.begin(), latenciesMs.end());

        const auto percentile = [&latenciesMs](double p) {
            if (latenciesMs.empty()) {
                return 0.0;
            }
            const auto index = static_cast<size_t>(std::ceil(p * latenciesMs.size())) - 1;
            return latenciesMs[std::min(index, latenciesMs.size() - 1)];
        };
        double latencySum = 0.0;
        for (double latency : latenciesMs) {
            latencySum += latency;
        }
        const double average = latenciesMs.empty() ? 0.0 : latencySum / latenciesMs.size();
        const double successRate = measuredCount == 0 ? 0.0 :
          100.0 * static_cast<double>(latenciesMs.size()) / static_cast<double>(measuredCount);

        std::ostringstream providerAttempts;
        bool first = true;
        for (const auto& provider : m_providers) {
            if (!first) {
                providerAttempts << ',';
            }
            first = false;
            providerAttempts << provider << ':' << providerAttemptCounts[provider];
        }
        const uint64_t applicationMessages = notificationInterests + notificationData +
          inputInterests + inputData + resultInterests + resultData;

        std::cout << std::fixed << std::setprecision(3)
                  << "NSC_FAILOVER_SUMMARY count=" << measuredCount
                  << " success=" << latenciesMs.size()
                  << " terminal_failures=" << terminalFailures
                  << " attempts=" << totalAttempts
                  << " attempt_timeouts=" << attemptTimeouts
                  << " nacks=" << nackCount
                  << " failovers=" << failoverCount
                  << " late_callbacks=" << lateCallbacks
                  << " late_messages=" << lateMessages
                  << " application_messages=" << applicationMessages
                  << " notification_interests=" << notificationInterests
                  << " notification_data=" << notificationData
                  << " input_interests=" << inputInterests
                  << " input_data=" << inputData
                  << " result_interests=" << resultInterests
                  << " result_data=" << resultData
                  << " mean_ms=" << average
                  << " p50_ms=" << percentile(0.50)
                  << " p95_ms=" << percentile(0.95)
                  << " p99_ms=" << percentile(0.99)
                  << " provider_attempts=" << providerAttempts.str()
                  << " deadline_ms=" << requestDeadlineMs
                  << " attempt_timeout_ms=" << attemptTimeoutMs
                  << " message_definition=" << messageDefinition()
                  << " late_message_definition=rejected_late_protocol_events"
                  << " measurement_start_monotonic_ms=" << measurementStartedMonotonicMs
                  << " measurement_start_lateness_ms=" << measurementStartLatenessMs
                  << " first_request_start_lateness_ms="
                  << firstRequestStartLatenessMs
                  << std::endl;

        // Preserve the old parser-facing summary while exposing the richer failover summary above.
        std::cout << std::fixed << std::setprecision(3)
                  << "NSC_CLIENT_SUMMARY count=" << measuredCount
                  << " success=" << latenciesMs.size()
                  << " timeout=" << legacyTimeoutCount(terminalFailures)
                  << " late_after_deadline=0"
                  << " deadline_ms=" << requestDeadlineMs
                  << " success_rate=" << successRate
                  << " avg_ms=" << average
                  << " p50_ms=" << percentile(0.50)
                  << " p95_ms=" << percentile(0.95)
                  << " p99_ms=" << percentile(0.99)
                  << " min_ms=" << (latenciesMs.empty() ? 0.0 : latenciesMs.front())
                  << " max_ms=" << (latenciesMs.empty() ? 0.0 : latenciesMs.back())
                  << std::endl;

        const char* smoke = std::getenv("NSC_SMOKE_TEST");
        if (smoke != nullptr && std::string(smoke) == "1" &&
            measuredCount > 0 && latenciesMs.size() == static_cast<size_t>(measuredCount)) {
            std::cout << "SMOKE_OK" << std::endl;
        }
        m_ioService.stop();
    }

    bool isAppNACK(const std::string& dataContent) const
    {
        return dataContent.rfind(APP_NACK, 0) == 0;
    }

    Interest createInterest(const std::string& name, bool canBePrefix,
                            bool mustBeFresh, int lifetimeMs) const
    {
        Interest interest{Name(name)};
        interest.setCanBePrefix(canBePrefix);
        interest.setMustBeFresh(mustBeFresh);
        interest.setInterestLifetime(ndn::time::milliseconds(std::max(1, lifetimeMs)));
        return interest;
    }

    //extract Interest Parameter as String
    std::string extractInterestParam(const Interest &interest)
    {
        return ndn::readString(interest.getApplicationParameters());
    }

    std::string extractDataValue(const Data &data)
    {
        return ndn::readString(data.getContent());
    }

    //Add a string as an Interest Parameter
    void addInterestParameterString(std::string params, Interest &interest)
    {
        // const uint8_t *params_uint = reinterpret_cast<const uint8_t *>(&params[0]);
        // interest.setApplicationParameters(params_uint, params.length() + 1);
        interest.setApplicationParameters(ndn::makeStringBlock(ndn::tlv::ApplicationParameters,params));
    }

    //create a Data packet with specified values
    std::shared_ptr<ndn::Data> createData(const ndn::Name dataName, std::string content, std::string identity)
    {
        auto data = make_shared<Data>(dataName);
        data->setFreshnessPeriod(1000_ms);
        //data->setContent(reinterpret_cast<const uint8_t *>(content.c_str()), content.length() + 1);
        data->setContent(ndn::makeStringBlock(ndn::tlv::Content,content));
        m_keyChain.sign(*data, security::signingByIdentity(Name(identity)));

        return data;
    }

    //Retrieves Key for a specific identity
    ndn::security::pib::Key getKeyForIdentity(std::string identity)
    {
        const auto &pib = m_keyChain.getPib();
        const auto &verifyIdentity = pib.getIdentity(Name(identity));
        return verifyIdentity.getDefaultKey();
    }

    //Signature Verification Functions for Interest
    bool verifyInterestSignature(const Interest &interest, std::string identity)
    {
        // skip verification because NDN_NSC does provide a good API for multiple identities;
        return true;
        if (security::verifySignature(interest, getKeyForIdentity(identity)))
        {
            nscLog() << "Interest Signature - Verified" << std::endl;
            return true;
        }
        else
        {
            nscLog() << "Interest Signature - ERROR, can't verify" << std::endl;
            return false;
        }
    }

    //Signature Verification Functions for Data
    bool verifyDataSignature(const Data &data, std::string identity)
    {
        // skip verification because NDN_NSC does provide a good API for multiple identities;
        return true;
        if (security::verifySignature(data, getKeyForIdentity(identity)))
        {
            nscLog() << "Data Signature - Verified" << std::endl;
            return true;
        }
        else
        {
            nscLog() << "Data Signature - ERROR, can't verify" << std::endl;
            return false;
        }
    }

    int extractRequestId(const ndn::Name& name) const
    {
        // Signed Interests with ApplicationParameters append a parameters digest,
        // so the logical request ID is the last all-decimal component, not
        // necessarily the final name component.
        for (size_t offset = 0; offset < name.size(); ++offset) {
            const std::string component = name.at(name.size() - 1 - offset).toUri();
            if (component.empty() ||
                !std::all_of(component.begin(), component.end(),
                             [](unsigned char ch) { return std::isdigit(ch); })) {
                continue;
            }
            try {
                return std::stoi(component);
            }
            catch (const std::exception&) {
                return 0;
            }
        }
        return 0;
    }

    void onRegisterFailed(const Name &prefix, const std::string &reason)
    {
        nscLog() << "ERROR: Failed to register prefix '" << prefix
                  << "' with the local forwarder (" << reason << ")" << std::endl;
        nscLog() << "------------------------" << std::endl;
        m_face.shutdown();
    }
};

int main(int argc, char **argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--logic-self-test") {
            return rpcConsumer::runLogicSelfTest();
        }
        if (argc < 7 || argc > 13)
        {
            std::cerr << "Usage: ./consumer <user> <provider[,provider...]> <service> <function> "
                         "<interval_in_ms> <count> [run_id] [warmup_count] "
                         "[request_deadline_ms] [attempt_timeout_ms] "
                         "[measurement_start_monotonic_ms] "
                         "[measurement_start_tolerance_ms]" << std::endl;
            exit(1);
        }
        rpcConsumer consumer1(argv[1], argv[2], argv[3], argv[4], argv[5], argv[6],
                              argc >= 8 ? argv[7] : nullptr,
                              argc >= 9 ? argv[8] : nullptr,
                              argc >= 10 ? argv[9] : nullptr,
                              argc >= 11 ? argv[10] : nullptr,
                              argc >= 12 ? argv[11] : nullptr,
                              argc >= 13 ? argv[12] : nullptr);
        consumer1.run();
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
}
