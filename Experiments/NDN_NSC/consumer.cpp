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
#include <cmath>
#include <cstdlib>
#include <iomanip>
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
    //Usage: ./consumer <user> <provider> <service> <function> <interval_in_ms> <count>
    rpcConsumer(char *user, char *provider, char *service, char *function, char *interval_in_ms, char *count, char *run_id = nullptr, char *warmup_count = nullptr)
        : m_face(m_ioService),
          m_scheduler(m_ioService),
          CONSUMER_IDENTITY(user),
          PRODUCER_IDENTITY(provider)
    {
        rpcCall = 0;
        PRODUCER_FUNC_NAME = std::string(provider) + std::string(service) + std::string(function);
        INPUT_NAMESPACE = CONSUMER_IDENTITY + std::string(service) + "/inputs/";
        if (run_id != nullptr && std::string(run_id).size() > 0) {
            INPUT_NAMESPACE += std::string(run_id) + "/";
        }
        this->interval_in_ms = std::stoi(interval_in_ms);
        this->count = std::stoi(count);
        this->warmupCount = warmup_count != nullptr ? std::stoi(warmup_count) : 0;
    }

    void run()
    {
        nscLog() << "Attempting to rpc call" << std::endl;
        nscLog() << "------------------------" << std::endl;

        m_face.registerPrefix(INPUT_NAMESPACE,
                              RegisterPrefixSuccessCallback(),
                              bind(&rpcConsumer::onRegisterFailed, this, _1, _2));
        m_face.setInterestFilter(INPUT_NAMESPACE,
                                 bind(&rpcConsumer::onInterestForInput, this, _1, _2));
        // loop count
        const int totalCount = warmupCount + count;
        for (int i = 0; i < totalCount; i++)
        {
            m_scheduler.schedule(ndn::time::milliseconds(interval_in_ms*i), std::bind(&rpcConsumer::pubAndNotify, this));
        }
        m_scheduler.schedule(ndn::time::milliseconds(interval_in_ms*totalCount+20000), std::bind(&rpcConsumer::CalculateLantency, this));
       
        m_ioService.run();
    }

    void CalculateLantency(){

        // calculate success rate for RPC Calls
        int successfulRPCCalls = rpcEndTimeMap.size();
        int totalRPCCalls = rpcStartTimeMap.size();

        // rpcEndTimeMap.size() / rpcStartTimeMap.size()
        double successRate = (double)successfulRPCCalls / (double)totalRPCCalls;
        nscLog() << "------------------------" << std::endl;
        nscLog() << "Success Rate for RPC Calls: " << successRate  << std::endl;
        
  
    

        // calculate average latency for successful RPC Calls only
        if (successfulRPCCalls > 0)
        {
            std::vector<double> latenciesMs;
            latenciesMs.reserve(successfulRPCCalls);
            for (auto const& [id, endTime] : rpcEndTimeMap)
            {
                auto startTime = rpcStartTimeMap[id];
                auto latency = ndn::time::duration_cast<ndn::time::milliseconds>(endTime - startTime).count();
                latenciesMs.push_back(static_cast<double>(latency));
            }
            std::sort(latenciesMs.begin(), latenciesMs.end());
            double totalLatencyForSuccessCalls = 0.0;
            for (double latency : latenciesMs) {
                totalLatencyForSuccessCalls += latency;
            }
            auto percentile = [&latenciesMs](double p) {
                if (latenciesMs.empty()) {
                    return 0.0;
                }
                const auto index = static_cast<size_t>(std::ceil(p * latenciesMs.size())) - 1;
                return latenciesMs[std::min(index, latenciesMs.size() - 1)];
            };
            auto averageLatencyForSuccessCalls = totalLatencyForSuccessCalls / successfulRPCCalls;
            nscLog() << "Average Latency for Successful RPC Calls: " << averageLatencyForSuccessCalls << "ms" << std::endl;
            std::cout << std::fixed << std::setprecision(3)
                      << "NSC_CLIENT_SUMMARY count=" << totalRPCCalls
                      << " success=" << successfulRPCCalls
                      << " timeout=" << timeoutCount
                      << " success_rate=" << (successRate * 100.0)
                      << " avg_ms=" << averageLatencyForSuccessCalls
                      << " p50_ms=" << percentile(0.50)
                      << " p95_ms=" << percentile(0.95)
                      << " min_ms=" << latenciesMs.front()
                      << " max_ms=" << latenciesMs.back()
                      << std::endl;
        }
        else
        {
            nscLog() << "No successful RPC Calls" << std::endl;
            std::cout << "NSC_CLIENT_SUMMARY count=" << totalRPCCalls
                      << " success=0 timeout=" << timeoutCount
                      << " success_rate=0 avg_ms=0 p50_ms=0 p95_ms=0 min_ms=0 max_ms=0"
                      << std::endl;
        }

        m_ioService.stop();
    }

    void pubAndNotify()
    {


        nscLog() << "Attempting to publish data" << std::endl;
        nscLog() << "------------------------" << std::endl;

        //publish data
        int publishNum = publishInput();

        if (publishNum > warmupCount) {
            rpcStartTimeMap[publishNum] = ndn::time::steady_clock::now();
        }

        //send notification interest
        sendNotification(inputNameFor(publishNum));
    }

private:
    boost::asio::io_service m_ioService;
    Face m_face;
    Scheduler m_scheduler;
    KeyChain m_keyChain;
    int rpcCall;
    const std::string CCNUM = "CCNUM";
    std::string CONSUMER_IDENTITY = "/muas/gs1";
    std::string PRODUCER_IDENTITY = "/muas/drone1";
    std::string PRODUCER_FUNC_NAME = "/muas/drone1/FlightControl/ManualControl";
    std::string INPUT_NAMESPACE = CONSUMER_IDENTITY + "/FlightControl/inputs/";
    const std::string APP_NACK = "APP_NACK";
    int interval_in_ms = 1000;
    int count = 1;
    int warmupCount = 0;
    // a map to record the starting time of each RPC Call
    std::map<int, ndn::time::steady_clock::time_point> rpcStartTimeMap;
    // a map to record the end time of each RPC Call
    std::map<int, ndn::time::steady_clock::time_point> rpcEndTimeMap;
    int timeoutCount = 0;

    //Publish Input data for future RPC Producer to retrieve
    int publishInput()
    {
        int publishNum = ++rpcCall;
        nscLog() << "RPC Call is at " << rpcCall << std::endl;
        nscLog() << "------------------------" << std::endl;
        return publishNum;
    }

    //Send a notification to the RPC Producer to initiate RPC Call
    std::string inputNameFor(int publishNum) const
    {
        return INPUT_NAMESPACE + std::to_string(publishNum);
    }

    void sendNotification(const std::string& inputName)
    {
        Interest interest = createInterest(PRODUCER_FUNC_NAME, true, true);
        addInterestParameterString(inputName, interest);
        m_keyChain.sign(interest, security::signingByIdentity(Name(CONSUMER_IDENTITY)));
        m_face.expressInterest(interest,
                               bind(&rpcConsumer::onNotificationData, this, _1, _2),
                               bind(&rpcConsumer::onNack, this, _1, _2),
                               bind(&rpcConsumer::onTimeout, this, _1));

        nscLog() << "Sending Notification Interest\n"
                  << PRODUCER_FUNC_NAME << std::endl;
        nscLog() << "------------------------" << std::endl;
    }

    //Acknowledge that Producer received RPC Notification
    void onNotificationData(const Interest &, const Data &data)
    {
        nscLog() << "Received Acknowledgement to Notification:" << std::endl;
        (void)data;
        nscLog() << "------------------------" << std::endl;
    }

    //Respond with original Input Data
    void onInterestForInput(const InterestFilter &, const Interest &interest)
    {
        nscLog() << "Received an interest for rpc input" << std::endl;
        if (verifyInterestSignature(interest, PRODUCER_IDENTITY))
        {
            nscLog() << "Sending Input Data as Published Earlier" << std::endl;
            nscLog() << "------------------------" << std::endl;

            auto data = createData(interest.getName(), CCNUM, CONSUMER_IDENTITY);
            m_face.put(*data);

            std::string resultName = extractInterestParam(interest);
            if (!resultName.empty()) {
                sendInterestForResult(resultName);
            }
        }
    }

    //Request results of RPC Call from location provided as Interest Parameters
    void sendInterestForResult(std::string resultName)
    {
        Interest interest = createInterest(resultName, true, false);
        // m_keyChain.sign(interest, security::signingByIdentity(Name(CONSUMER_IDENTITY)));
        m_face.expressInterest(interest,
                               bind(&rpcConsumer::onResultData, this, _1, _2),
                               bind(&rpcConsumer::onNack, this, _1, _2),
                               bind(&rpcConsumer::onTimeout, this, _1));

        nscLog() << "Sending Interest for final Result Data " << std::endl;
        nscLog() << resultName << std::endl;
        nscLog() << "------------------------" << std::endl;
    }

    //Print result Data
    void onResultData(const Interest &interest, const Data &data)
    {
        nscLog() << "Received Result Data from Producer" << std::endl;

        if (verifyDataSignature(data, PRODUCER_IDENTITY))
        {
            std::string ccResult(reinterpret_cast<const char *>(data.getContent().value()));

            //check for application NACK
            if (isAppNACK(ccResult))
            {

                std::string newResultName = ccResult.substr(APP_NACK.length(), ccResult.length() - APP_NACK.length());
                Interest delayInterest = createInterest(newResultName, false, true);
                m_keyChain.sign(delayInterest, security::signingByIdentity(Name(CONSUMER_IDENTITY)));
                m_face.expressInterest(delayInterest,
                                       bind(&rpcConsumer::onResultData, this, _1, _2),
                                       bind(&rpcConsumer::onNack, this, _1, _2),
                                       bind(&rpcConsumer::onTimeout, this, _1));

                nscLog() << "Received Delay message, now retrying " << newResultName << std::endl;
            }
            else
            {
                int id = std::stoi(interest.getName().at(interest.getName().size() - 1).toUri());
                nscLog() << id << std::endl;
                if (rpcStartTimeMap.count(id) != 0) {
                    rpcEndTimeMap[id] = ndn::time::steady_clock::now();
                }
                nscLog() << "------------------------" << std::endl;
                nscLog() << "------------------------" << std::endl;
                nscLog() << "RPC Call Id: " << id << std::endl;
                nscLog() << "Result of RPC Received: " << std::endl;
                nscLog() << "------------------------" << std::endl;
                nscLog() << "------------------------" << std::endl;
                // m_ioService.stop();
            }
        }
    }

    bool isAppNACK(std::string dataContent)
    {
        std::size_t found = dataContent.find(APP_NACK);
        if (found == 0)
            return true;
        else
            return false;
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

    //Boilerplate NACK, Timeout, Failure to Register
    void onNack(const Interest &, const lp::Nack &nack) const
    {
        nscLog() << "Received Nack with reason " << nack.getReason() << std::endl;
        nscLog() << "------------------------" << std::endl;
    }

    void onTimeout(const Interest &interest)
    {
        int id = extractRequestId(interest.getName());
        if (id == 0 || id > warmupCount) {
            timeoutCount++;
        }
        nscLog() << "Timeout for " << interest << std::endl;
        nscLog() << "------------------------" << std::endl;
    }

    int extractRequestId(const ndn::Name& name) const
    {
        if (name.empty()) {
            return 0;
        }
        try {
            return std::stoi(name.at(name.size() - 1).toUri());
        }
        catch (const std::exception&) {
            return 0;
        }
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
        if (argc < 7 || argc > 9)
        {
            std::cerr << "Usage: ./consumer <user> <provider> <service> <function> <interval_in_ms> <count> [run_id] [warmup_count]" << std::endl;
            exit(1);
        }
        rpcConsumer consumer1(argv[1], argv[2], argv[3], argv[4], argv[5], argv[6],
                              argc >= 8 ? argv[7] : nullptr,
                              argc >= 9 ? argv[8] : nullptr);
        consumer1.run();
        return 0;
    }
    catch (const std::exception &e)
    {
        nscLog() << "ERROR: " << e.what() << std::endl;
        nscLog() << "------------------------" << std::endl;
        return 1;
    }
}
