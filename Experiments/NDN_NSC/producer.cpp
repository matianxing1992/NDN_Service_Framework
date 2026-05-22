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

#define BOOST_THREAD_PROVIDES_FUTURE
#include <boost/thread.hpp>
#include <boost/thread/future.hpp>
#include <thread>
#include <future>
#include <chrono>
#include <random>

#include <functional>
#include <fstream>
#include <string>
#include <iostream>
#include <cstdlib>
#include <map>
#include <streambuf>
#include <algorithm>

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

class rpcProducer
{
public:
    rpcProducer(char *provider, char *service, char *function,
                int serviceDelayMs = 0,
                double failureProbability = 0.0,
                int epochMs = 10000,
                uint32_t seed = 100) : m_face(m_ioService),
                    m_scheduler(m_ioService),
                    PRODUCER_IDENTITY(provider),
                    m_serviceDelayMs(serviceDelayMs),
                    m_failureProbability(std::max(0.0, std::min(1.0, failureProbability))),
                    m_epochMs(std::max(1, epochMs)),
                    m_seed(seed),
                    m_startedAt(std::chrono::steady_clock::now())
    {
        BASE = PRODUCER_IDENTITY + std::string(service);
        FUNCTION = BASE + std::string(function);
        RESULTS = BASE + std::string("/results/");
    }

    void run()
    {
        nscLog() << "PRODUCER" << std::endl;
        nscLog() << "Attempting to schedule" << std::endl;
        nscLog() << "------------------------" << std::endl;

        // schedule rpcProducer::waitForNotification() to run after 1 nanosecond
        m_scheduler.schedule(1_ns, std::bind(&rpcProducer::waitForNotification, this));


        m_ioService.run();
    }

private:
    //object variables
    boost::asio::io_service m_ioService;
    Face m_face;
    Scheduler m_scheduler;
    KeyChain m_keyChain;
    int m_serviceDelayMs = 0;
    double m_failureProbability = 0.0;
    int m_epochMs = 10000;
    uint32_t m_seed = 100;
    std::chrono::steady_clock::time_point m_startedAt;
    const double WAIT_TIME_FACTOR = .75;

    //Naming Scheme
    int CONSUMER_NAME_FIELDS = 2;
    std::string PRODUCER_IDENTITY = "/muas/drone1";
    std::string CONSUMER_IDENTITY = "/muas";
    std::string BASE = "/muas/drone1/FlightControl";
    std::string FUNCTION = BASE + "/ManualControl/";
    std::string RESULTS = BASE + "/results/";
    const std::string DELAY_NAME = "delay/";
    const std::string DELIMITER = "/";

    //Results Messages
    const size_t CC_LENGTH = 16;
    const std::string APP_ACK = "APP_ACK";
    const std::string APP_NACK = "APP_NACK";
    const std::string SUCCESS = "GOOD";
    const std::string FAILURE = "BAD";
    std::map<std::string, ndn::time::steady_clock::time_point> m_resultReadyAt;
    std::map<std::string, bool> m_resultValue;
    std::map<std::string, ndn::Name> m_pendingResultInterest;
    std::map<std::string, bool> m_resultReplyScheduled;

    //Register prefix to listen for RPC Notifications
    void waitForNotification()
    {
        m_face.registerPrefix(BASE,
                              RegisterPrefixSuccessCallback(),
                              bind(&rpcProducer::onRegisterFailed, this, _1, _2));

        m_face.setInterestFilter(FUNCTION,
                                 bind(&rpcProducer::onNotification, this, _1, _2));
        m_face.setInterestFilter(RESULTS,
                                 bind(&rpcProducer::onResultInterest, this, _1, _2),
                                 [](const Name&, const std::string&){nscLog() << "Interest Filter Error" << std::endl;});
        std::cout << "REGISTER PREFIX " << BASE << std::endl;
        nscLog() << "LISTENING TO " << FUNCTION << std::endl;
        nscLog() << "LISTENING TO " << RESULTS << std::endl;
        nscLog() << "------------------------" << std::endl;
    }

    //Received notification of RPC call, acknolwedge and request consumer input
    void onNotification(const InterestFilter &interestFil, const Interest &interest)
    {
        nscLog() << "Received Notification at: " << interestFil.getPrefix() << std::endl;

        if (isUnavailable()) {
            std::cout << "NSC_PRODUCER_SUPPRESS provider=" << PRODUCER_IDENTITY
                      << " epoch=" << currentEpoch() << std::endl;
            return;
        }

        if (verifyInterestSignature(interest, CONSUMER_IDENTITY))
        {
            std::string consumerInputParam = extractInterestParam(interest);
            std::string token = extractRPCCaller(consumerInputParam);
            std::string resultName = RESULTS + token;
            auto data = createData(interest.getName(), resultName, PRODUCER_IDENTITY);
            m_face.put(*data);
            requestConsumerInput(consumerInputParam, token, resultName);

            nscLog() << "Sending response to notification" << std::endl;
            nscLog() << "------------------------" << std::endl;
        }
    }

    uint64_t currentEpoch() const
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_startedAt).count();
        return static_cast<uint64_t>(elapsed / m_epochMs);
    }

    bool isUnavailable() const
    {
        if (m_failureProbability <= 0.0) {
            return false;
        }
        const auto epoch = currentEpoch();
        std::seed_seq seq{
            m_seed,
            static_cast<uint32_t>(epoch),
            static_cast<uint32_t>(epoch >> 32),
            0x9e3779b9u
        };
        std::mt19937 rng(seq);
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return dist(rng) < m_failureProbability;
    }

    //Request Consumer Input based on Parameter in Notification Interest
    void requestConsumerInput(std::string consumerInputParam, std::string token, std::string resultName)
    {
        Interest interest = createInterest(consumerInputParam, true, true);
        addInterestParameterString(resultName, interest);
        m_keyChain.sign(interest, security::signingByIdentity(Name(PRODUCER_IDENTITY)));
        m_face.expressInterest(interest,
                               bind(&rpcProducer::onConsumerData, this, _1, _2, token),
                               bind(&rpcProducer::onNack, this, _1, _2),
                               bind(&rpcProducer::onTimeout, this, _1));

        nscLog() << "Sending Interest for RPC input " << consumerInputParam << std::endl;
        nscLog() << "------------------------" << std::endl;
    }

    //Receive Input Data from consumer, listen for consumer request for data
    void onConsumerData(const Interest &, const Data &data, std::string token)
    {
        if (verifyDataSignature(data, CONSUMER_IDENTITY))
        {
            std::string dataValue = extractDataValue(data);
            m_resultValue[token] = ccCheck(dataValue);
            m_resultReadyAt[token] = ndn::time::steady_clock::now() +
                                     ndn::time::milliseconds(m_serviceDelayMs);
            scheduleResultReply(token);

            nscLog() << "Successfully fetched input paramaters from consumer" << std::endl;
            nscLog() << dataValue << std::endl;
            nscLog() << "Now listening for " << RESULTS + token << std::endl;
            nscLog() << "------------------------" << std::endl;
        }
    }

    //Consumer requests data, generate and respond with it
    void onResultInterest(const InterestFilter &, const Interest &interest)
    {
        std::string token = extractResultToken(interest.getName());
        nscLog() << "Received interest for final results at " << interest.getName() << std::endl;
        m_pendingResultInterest[token] = interest.getName();
        scheduleResultReply(token);
        nscLog() << "------------------------" << std::endl;
    }

    void scheduleResultReply(const std::string& token)
    {
        if (m_pendingResultInterest.count(token) == 0 || m_resultReadyAt.count(token) == 0 ||
            m_resultReplyScheduled[token]) {
            return;
        }
        auto now = ndn::time::steady_clock::now();
        auto readyAt = m_resultReadyAt[token];
        auto delay = readyAt <= now ? ndn::time::milliseconds(0) :
                                      ndn::time::duration_cast<ndn::time::milliseconds>(readyAt - now);
        m_resultReplyScheduled[token] = true;
        m_scheduler.schedule(delay, [this, token] { sendStoredResult(token); });
    }

    void sendStoredResult(const std::string& token)
    {
        m_resultReplyScheduled[token] = false;
        if (m_pendingResultInterest.count(token) == 0 || m_resultReadyAt.count(token) == 0) {
            return;
        }
        if (ndn::time::steady_clock::now() < m_resultReadyAt[token]) {
            scheduleResultReply(token);
            return;
        }
        auto interestName = m_pendingResultInterest[token];
        auto data = createData(interestName, m_resultValue[token] ? SUCCESS : FAILURE, PRODUCER_IDENTITY);
        m_face.put(*data);
        m_pendingResultInterest.erase(token);
        m_resultReadyAt.erase(token);
        m_resultValue.erase(token);
        nscLog() << "Generated Result for " << token << ", sending" << std::endl;
    }

    //Requested data finished generating, respond with result
    void sendGeneratedResult(const Interest &interest, boost::shared_future<bool> fut)
    {
        //
        nscLog() << "Received Interest: " << interest.getName().toUri() << std::endl;
        std::string dataValue;
        if (fut.get())
            dataValue = SUCCESS;
        else
            dataValue = FAILURE;
        auto data = createData(interest.getName(), dataValue, PRODUCER_IDENTITY);
        m_face.put(*data);

        nscLog() << "Generated Result, " << dataValue << " , sending" << std::endl;
    }

    //Requested data was still in process, send delay message
    void sendDelayResult(const Interest &interest, std::string baseName, std::string tokenName, boost::shared_future<bool> fut)
    {
        std::string delayedDataName = baseName + DELAY_NAME + tokenName;
        auto data = createData(interest.getName(), APP_NACK + delayedDataName, PRODUCER_IDENTITY);
        m_face.put(*data);

        nscLog() << "Timed out on generating result, sending delay message" << std::endl;
        nscLog() << "Now listening to " << delayedDataName << std::endl;
    }

    //Basic check to verify credit card
    bool ccCheck(std::string inputValue)
    {
        // skip input check
        return true;

        //checks that Credit Card is 16 Digits Long and is all Digits
        if (inputValue.length() != CC_LENGTH || !allStringIsDigit(inputValue))
            return false;

        //Check with Luhn's Algorithms
        if (!luhnAlgo(inputValue))
            return false;

        return true;
    }

    bool allStringIsDigit(std::string inputValue)
    {
        for (size_t i = 0; i < inputValue.length(); i++)
        {
            if (!std::isdigit(inputValue.at(i)))
                return false;
        }
        return true;
    }

    bool luhnAlgo(std::string inputValue)
    {
        //find sum of first 15 numbers according to luhns
        int sum = 0;
        for (size_t i = 0; i < inputValue.length() - 1; i++)
        {
            int x = inputValue.at(i) - '0';
            if (i % 2 == 0)
            {
                x *= 2;
                if (x >= 10)
                    x = (x / 10) + (x % 10);
            }
            sum += x;
        }
        //check first 15 sum + check number is divisble by 10 with no remainder
        if (((sum + (inputValue.at(inputValue.length() - 1) - '0')) % 10) == 0)
            return true;
        else
            return false;
    }

    //extract Interest Parameter as String
    std::string extractInterestParam(const Interest &interest)
    {
        //std::string interestParam(reinterpret_cast<const char *>(interest.getApplicationParameters().value()));
        return ndn::readString(interest.getApplicationParameters());
    }

    //extract Interest Parameter as String
    std::string extractDataValue(const Data &data)
    {
        std::string dataValue(reinterpret_cast<const char *>(data.getContent().value()));
        return dataValue;
    }

    //extract the client token + RPC number to be used in generating result name
    std::string extractRPCCaller(std::string consumerRPCInput)
    {
        Name inputName(consumerRPCInput);
        if (inputName.size() < 3) {
            return consumerRPCInput;
        }
        std::string token = inputName.at(0).toUri() + DELIMITER + inputName.at(1).toUri();
        for (size_t i = 0; i < inputName.size(); ++i) {
            if (inputName.at(i).toUri() != "inputs") {
                continue;
            }
            for (size_t j = i + 1; j < inputName.size(); ++j) {
                token += DELIMITER + inputName.at(j).toUri();
            }
            return token;
        }
        token += DELIMITER + inputName.at(inputName.size() - 1).toUri();
        return token;
    }

    std::string extractResultToken(const ndn::Name& resultName)
    {
        Name resultPrefix(RESULTS);
        if (resultName.size() <= resultPrefix.size()) {
            return "";
        }
        std::string token = resultName.getSubName(resultPrefix.size()).toUri();
        if (!token.empty() && token.front() == '/') {
            token.erase(token.begin());
        }
        return token;
    }

    //create an Interest packet with specified values
    Interest createInterest(std::string name, bool canBePrefix, bool mustBeFresh)
    {
        Name interestName(name);
        Interest interest(interestName);
        interest.setCanBePrefix(canBePrefix);
        interest.setMustBeFresh(mustBeFresh);

        return interest;
    }

    //Add a string as an Interest Parameter
    void addInterestParameterString(std::string params, Interest &interest)
    {
        //const uint8_t *params_uint = reinterpret_cast<const uint8_t *>(&params[0]);
        //interest.setApplicationParameters(params_uint, params.length() + 1);

        // set application parameters using params
        interest.setApplicationParameters(ndn::makeStringBlock(ndn::tlv::ApplicationParameters,params));
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
        //skip verification because NDN_NSC does have a good API for mulitiple identities
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
        //skip verification because NDN_NSC does have a good API for mulitiple identities
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

    //create a Data packet with specified values
    std::shared_ptr<ndn::Data> createData(const ndn::Name dataName, std::string content, std::string identity)
    {
        auto data = make_shared<Data>(dataName);
        data->setFreshnessPeriod(4000_ms);
        //data->setContent(reinterpret_cast<const uint8_t *>(content.c_str()), content.length() + 1);
        // set content of data using content
        data->setContent(ndn::makeStringBlock(ndn::tlv::Content,content));
        m_keyChain.sign(*data, signingByIdentity(identity));

        return data;
    }

    //Boilerplate NACK, Timeout, Failure to Register
    void onNack(const Interest &, const lp::Nack &nack) const
    {
        nscLog() << "Received Nack with reason " << nack.getReason() << std::endl;
        nscLog() << "------------------------" << std::endl;
    }

    void onTimeout(const Interest &interest) const
    {
        nscLog() << "Timeout for " << interest << std::endl;
        nscLog() << "------------------------" << std::endl;
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
        if (argc < 4 || argc > 8)
        {
            nscLog() << "Usage: ./producer <provider> <service> <function> "
                     << "[service_delay_ms] [failure_probability] [epoch_ms] [seed]"
                     << std::endl;
            exit(1);
        }
        int serviceDelayMs = argc >= 5 ? std::stoi(argv[4]) : 0;
        double failureProbability = argc >= 6 ? std::stod(argv[5]) : 0.0;
        int epochMs = argc >= 7 ? std::stoi(argv[6]) : 10000;
        uint32_t seed = argc >= 8 ? static_cast<uint32_t>(std::stoul(argv[7])) : 100;
        rpcProducer producer(argv[1], argv[2], argv[3],
                             serviceDelayMs, failureProbability, epochMs, seed);
        producer.run();
        return 0;
    }
    catch (const std::exception &e)
    {
        nscLog() << "ERROR: " << e.what() << std::endl;
        nscLog() << "------------------------" << std::endl;
        return 1;
    }
}
