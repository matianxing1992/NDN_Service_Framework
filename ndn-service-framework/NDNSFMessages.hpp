#ifndef NDN_SERVICE_FRAMEWORK_MESSAGES_HPP
#define NDN_SERVICE_FRAMEWORK_MESSAGES_HPP

#include <iostream>
#include <map>
#include <string>
#include <vector>
#include "common.hpp"

namespace ndn_service_framework {

namespace tlv {
    // Message types
    enum {
        RequestMessageType = 128,
        ResponseMessageType = 129,
        RequestAckMessageType = 130,
        ServiceCoordinationMessageType = 131,
        TokenType = 150,
        PayloadType = 151,
        StatusType = 152,
        ErrorInfoType = 153,
        RequestIDType = 154,
        StrategyType = 155,
    };

    // Coordination Strategies
    enum {
        FirstResponding = 0,
        LoadBalancing = 1,
        NoCoordination = 2,
    };
}

class AbstractMessage {
public:
    virtual ~AbstractMessage() {}

    virtual ndn::Block WireEncode() const = 0;
    virtual bool WireDecode(const ndn::Block& block) = 0;
    virtual void Clear() = 0;
};

class RequestMessage : public AbstractMessage {
public:
    RequestMessage();

    void setTokens(const std::map<std::string, std::string>& tokens);
    void setPayload(ndn::Buffer& payload, size_t size);
    // FirstResponding = 0, LoadBalancing = 1, All = 2,
    void setStrategy(size_t strategy);
    const std::map<std::string, std::string>& getTokens() const;
    ndn::Buffer getPayload() const;
    size_t getPayloadSize() const;
    size_t getStrategy() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    std::map<std::string, std::string> tokens_;
    ndn::Buffer payload_;
    size_t payloadSize_ = 0;
    size_t strategy_ = tlv::FirstResponding;
    mutable ndn::Block m_wire;
};

class ResponseMessage : public AbstractMessage {
public:
    ResponseMessage();

    void setStatus(bool status);
    void setErrorInfo(const std::string& errorInfo);
    void setPayload(ndn::Buffer& payload, size_t size);
    bool getStatus() const;
    const std::string& getErrorInfo() const;
    ndn::Buffer getPayload() const;
    size_t getPayloadSize() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    bool status_;
    std::string errorInfo_;
    ndn::Buffer payload_;
    size_t payloadSize_ = 0;
    mutable ndn::Block m_wire;
};

class RequestAckMessage : public AbstractMessage {
public:
    RequestAckMessage();

    void setStatus(bool status);
    void setMessage(const std::string& message);
    bool getStatus() const;
    const std::string& getMessage() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    bool status_;
    std::string message_;
    mutable ndn::Block m_wire;
};

class ServiceCoordinationMessage : public AbstractMessage {
public:
    ServiceCoordinationMessage();

    void setRequestIDs(const std::vector<std::string>& requestIDs);
    const std::vector<std::string>& getRequestIDs() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    std::vector<std::string> requestIDs_;
    mutable ndn::Block m_wire;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_MESSAGES_HPP
