#include "ServiceUser.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace ndn_service_framework
{

    NDN_LOG_INIT(ndn_service_framework.ServiceUser);

    namespace
    {
        class FileLock
        {
        public:
            explicit FileLock(const char* path)
            {
                m_fd = open(path, O_CREAT | O_RDWR, 0666);
                if (m_fd < 0 || flock(m_fd, LOCK_EX) != 0) {
                    throw std::runtime_error("Failed to acquire file lock");
                }
            }

            ~FileLock()
            {
                if (m_fd >= 0) {
                    flock(m_fd, LOCK_UN);
                    close(m_fd);
                }
            }

        private:
            int m_fd = -1;
        };

        bool
        decodeEncryptedPermissionResponseFromDataContent(
            const ndn::Data& data,
            EncryptedPermissionResponse& response)
        {
            const auto& content = data.getContent();
            if (content.type() == tlv::EncryptedPermissionResponseType) {
                return response.WireDecode(content);
            }

            auto [ok, block] = ndn::Block::fromBuffer(
                ndn::span<const uint8_t>(content.value(), content.value_size()));
            if (!ok) {
                return false;
            }
            return response.WireDecode(block);
        }

        bool
        decodePermissionResponseFromDataContent(const ndn::Data& data,
                                                PermissionResponse& response)
        {
            const auto& content = data.getContent();
            if (content.type() == tlv::PermissionResponseType) {
                return response.WireDecode(content);
            }

            auto [ok, block] = ndn::Block::fromBuffer(
                ndn::span<const uint8_t>(content.value(), content.value_size()));
            if (!ok) {
                return false;
            }
            return response.WireDecode(block);
        }

        bool
        payloadEquals(const RequestAckMessage& lhs, const RequestAckMessage& rhs)
        {
            const auto lhsPayload = lhs.getPayload();
            const auto rhsPayload = rhs.getPayload();
            return lhsPayload.size() == rhsPayload.size() &&
                   std::equal(lhsPayload.begin(),
                              lhsPayload.end(),
                              rhsPayload.begin());
        }

        bool
        ackEquals(const RequestAckMessage& lhs, const RequestAckMessage& rhs)
        {
            return lhs.getStatus() == rhs.getStatus() &&
                   lhs.getMessage() == rhs.getMessage() &&
                   payloadEquals(lhs, rhs);
        }

        uint64_t
        nowMilliseconds()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
    }

    ServiceUser::ServiceUser(ndn::Face &face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath) : 
        m_face(face),
        m_scheduler(m_face.getIoContext()),
        identity(identityCert.getIdentity()),
        validator(std::make_shared<MessageValidator>(trustSchemaPath)),
        identityCert(identityCert),
        // nac_validator(std::move(ndn::security::ValidatorNull())),
        nacConsumer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        nacProducer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        m_IMS(6000)
    {
        if (std::getenv("NDNSF_DISABLE_NDNSD") == nullptr) {
            m_ServiceDiscovery.enable(group_prefix,
                                      identity,
                                      face,
                                      m_keyChain,
                                      std::bind(&ServiceUser::processNDNSDServiceInfoCallback, this, _1));
        }

        nac_validator.load(trustSchemaPath);

        nacConsumer.obtainDecryptionKey();


        // Serve NDNSF and ck messages using IMS
        m_face.setInterestFilter(ndn::Name(identity.toUri()).append("NDNSF"),
            std::bind(&ServiceUser::onInterest, this, _1, _2),
            std::bind(&ServiceUser::onPrefixRegisterFailure, this, _1, _2));
        m_face.setInterestFilter(ndn::Name(identity.toUri()).append("CK"),
            std::bind(&ServiceUser::onInterest, this, _1, _2),
            std::bind(&ServiceUser::onPrefixRegisterFailure, this, _1, _2));

        m_signingInfo = ndn::security::signingByCertificate(identityCert);

        // Sign interest packets using a certificate
        ndn::svs::SecurityOptions secOpts(m_keyChain);
        // secOpts.interestSigner->signingInfo.setSigningCertName(cert);
        // secOpts.interestSigner->signingInfo = m_signingInfo;

        secOpts.interestSigner = std::make_shared<CommandInterestSigner>(m_keyChain);
        secOpts.interestSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        secOpts.interestSigner->signingInfo.setSigningKeyName(identityCert.getKeyName());
        // secOpts.interestSigner->signingInfo.setSigningHmacKey("dGhpcyBpcyBhIHNlY3JldCBtZXNzYWdl");

        // Sign data packets using a certificate
        // secOpts.dataSigner->signingInfo.setSha256Signing();
        // secOpts.dataSigner->signingInfo.setSigningCertName(cert);
        // secOpts.dataSigner->signingInfo = m_signingInfo;

        secOpts.dataSigner->signingInfo.setSigningCertName(identityCert.getName());
        secOpts.dataSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);

        // Sign publication packets using a certificate
        // secOpts.pubSigner->signingInfo.setSigningCertName(cert);
        // secOpts.pubSigner->signingInfo = m_signingInfo;

        secOpts.pubSigner->signingInfo.setSigningCertName(identityCert.getName());
        secOpts.pubSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);

        /** Validator to validate data and interests (unless using HMAC) */
        secOpts.validator = validator;

        /** Validator to validate encapsulated data */
        secOpts.encapsulatedDataValidator = validator;

        // Do not fetch publications older than 10 seconds
        ndn::svs::SVSPubSubOptions opts;
        #ifdef USE_TIMESTAMP
        opts.useTimestamp = true;
        // opts.maxPubAge = ndn::time::seconds(0);
        #else
        opts.useTimestamp = false;
        #endif

        ndn::Name node_id(identity);
        node_id.append("user");
        int session_id = m_configManager.loadAndIncrement(group_prefix.toUri(),node_id.toUri());
        node_id.append(std::to_string(session_id));

        {
            FileLock svsRegistrationLock("/tmp/ndnsf-svs-registration.lock");
            m_svsps = std::make_shared<ndn::svs::SVSPubSub>(
                ndn::Name(group_prefix),
                node_id,
                m_face,
                std::bind(&ServiceUser::onMissingData, this, _1),
                opts,
                secOpts);
        }

        while(!nacConsumer.readyForDecryption()){
            // log waiting for decryption key
            nacConsumer.obtainDecryptionKey();
            NDN_LOG_INFO("Waiting for decryption key");
            face.processEvents(ndn::time::milliseconds(1000));
        }



    }

    ServiceUser::ServiceUser(LocalMockTag,
                             ndn::Face& face,
                             ndn::Name group_prefix,
                             ndn::security::Certificate identityCert,
                             ndn::security::Certificate attrAuthorityCertificate,
                             std::string trustSchemaPath)
        : m_face(face),
        m_scheduler(m_face.getIoContext()),
        identity(identityCert.getIdentity()),
        m_keyChain(),
        m_svsps(nullptr),
        validator(std::make_shared<MessageValidator>(trustSchemaPath)),
        nac_validator(m_face),
        identityCert(identityCert),
        nacConsumer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        nacProducer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        m_IMS(6000),
        m_configManager("/tmp/ndnsf-service-user-local-mock.conf")
    {
        m_signingInfo = ndn::security::signingByCertificate(identityCert);
    }

    void ServiceUser::init()
    {
        registerNDNSFMessages();
    }

    void ServiceUser::setRequestPublisher(RequestPublisher publisher)
    {
        m_requestPublisher = std::move(publisher);
    }

    ndn::Name ServiceUser::getName()
    {
        return identity;
    }

    void ServiceUser::fetchPermissionsFromController(const ndn::Name& controllerPrefix)
    {
        ndn::Name interestName(controllerPrefix);
        interestName.append(ndn::Name("/NDNSF/PERMISSIONS/USER"));
        interestName.append(identity);

        ndn::Interest interest(interestName);
        interest.setCanBePrefix(true);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::seconds(4));
        ndn::security::InterestSigner signer(m_keyChain);
        signer.makeSignedInterest(
            interest,
            m_signingInfo,
            ndn::security::InterestSigner::SigningFlags::WantNonce |
            ndn::security::InterestSigner::SigningFlags::WantTime);

        NDN_LOG_INFO("Fetch user permissions: " << interestName);
        m_face.expressInterest(
            interest,
            std::bind(&ServiceUser::onPermissionResponseData, this, _1, _2),
            [this](const ndn::Interest& interest, const ndn::lp::Nack&) {
                onPermissionResponseTimeout(interest);
            },
            std::bind(&ServiceUser::onPermissionResponseTimeout, this, _1));
    }

    void ServiceUser::applyPermissionResponse(const PermissionResponse& response)
    {
        if (response.getPermissionKind() != tlv::UserPermission) {
            NDN_LOG_ERROR("Ignoring non-user PermissionResponse for "
                          << response.getTargetIdentity());
            return;
        }

        for (const auto& entry : response.getEntries()) {
            ndn::Name providerServiceName(entry.getProviderName());
            providerServiceName.append(ndn::Name(entry.getServiceName()));
            UPT.insertPermission(providerServiceName.toUri(),
                                 entry.getServiceName(),
                                 entry.getToken());
            NDN_LOG_INFO("Installed user permission provider="
                         << entry.getProviderName()
                         << " service=" << entry.getServiceName());
        }
    }

    bool ServiceUser::handlePermissionResponseData(const ndn::Data& data,
                                                   const ndn::Name& identity,
                                                   ndn::KeyChain& keyChain,
                                                   UserPermissionTable& permissionTable)
    {
        PermissionResponse response;
        EncryptedPermissionResponse encryptedResponse;
        if (decodeEncryptedPermissionResponseFromDataContent(data, encryptedResponse)) {
            try {
                response = decryptPermissionResponseWithKeyChain(encryptedResponse, keyChain);
            }
            catch (const std::exception& e) {
                NDN_LOG_ERROR("Failed to decrypt encrypted PermissionResponse from "
                              << data.getName() << ": " << e.what());
                return false;
            }

            NDN_LOG_INFO("Received encrypted PermissionResponse: "
                         << response.toString());
        }
        else {
            if (!decodePermissionResponseFromDataContent(data, response)) {
                NDN_LOG_ERROR("Failed to decode PermissionResponse from "
                              << data.getName());
                return false;
            }

            NDN_LOG_INFO("Received plaintext PermissionResponse fallback: "
                         << response.toString());
        }

        if (response.getTargetIdentity() != identity.toUri()) {
            NDN_LOG_ERROR("Ignoring PermissionResponse for unexpected targetIdentity="
                          << response.getTargetIdentity()
                          << " expected=" << identity.toUri());
            return false;
        }

        if (response.getPermissionKind() != tlv::UserPermission) {
            NDN_LOG_ERROR("Ignoring non-user PermissionResponse for "
                          << response.getTargetIdentity());
            return false;
        }

        for (const auto& entry : response.getEntries()) {
            ndn::Name providerServiceName(entry.getProviderName());
            providerServiceName.append(ndn::Name(entry.getServiceName()));
            permissionTable.insertPermission(providerServiceName.toUri(),
                                             entry.getServiceName(),
                                             entry.getToken());
            NDN_LOG_INFO("Installed user permission provider="
                         << entry.getProviderName()
                         << " service=" << entry.getServiceName());
        }
        return true;
    }

    void ServiceUser::PublishRequest(const std::vector<ndn::Name> &serviceProviderNames, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &payload, const size_t& strategy)
    {
        // log the request;
        NDN_LOG_INFO("PublishRequest: " <<  ServiceName << FunctionName << RequestID);
        
        ndn_service_framework::BloomFilter bloomFilter;
        std::vector<std::pair<std::string, std::string>> pairs; // serviceFullName->token mapping
        // find serviceFullName->token mapping from UPT;
        pairs = UPT.searchByFunctionName(ndn::Name(ServiceName.toUri()).append(FunctionName).toUri());

        if (serviceProviderNames.size() > 0){
            // add providers to bloom filter;
            for (auto providerName : serviceProviderNames){
                bloomFilter.insert(providerName.toUri());
            }
        }else{
            for (auto pair : pairs){
                bloomFilter.insert(ndn::Name(pair.first).getPrefix(-2).toUri());
            }
        }

        ndn_service_framework::RequestMessage requestMessage;
        // name->tokens to name->hash(token+requestID)
        std::map<std::string, std::string> tokens;
        for (auto pair : pairs){
            tokens[pair.first] = makeAuthorizationProof(pair.second, RequestID);
        }
        requestMessage.setTokens(tokens);
        requestMessage.setPayload(const_cast<ndn::Buffer&>(payload),payload.size());
        requestMessage.setStrategy(strategy);
        requestMessage.WireEncode().data();
        
        ndn::Name requestName = ndn_service_framework::makeRequestName(identity, ServiceName, FunctionName, ndn::Name(bloomFilter.toHexString()), RequestID);
        ndn::Name requestNameWithoutPrefix = ndn_service_framework::makeRequestNameWithoutPrefix(ServiceName, FunctionName, ndn::Name(bloomFilter.toHexString()),RequestID);

        auto pendingIt = m_pendingCalls.find(RequestID);
        if (pendingIt != m_pendingCalls.end()) {
            pendingIt->second.providers = serviceProviderNames;
            pendingIt->second.serviceName = makeUnifiedServiceName(ServiceName, FunctionName);
            pendingIt->second.requestName = requestName;
            pendingIt->second.requestNameWithoutPrefix = requestNameWithoutPrefix;
            pendingIt->second.requestMessage = requestMessage;
            pendingIt->second.strategy = strategy;
        }
    
        PublishMessage(requestName,requestNameWithoutPrefix,requestMessage);

        // register coordination strategy
        m_strategyMap.emplace(RequestID, strategy);

        // create a timer for LoadBalancing
        if (strategy == tlv::LoadBalancing){

            // insert RequestID->vector<AckInfo> into m_AckInfoMap
            m_AckInfoMap[RequestID] = std::vector<ndn_service_framework::AckInfo>();

            m_scheduler.schedule(100_ms,[this,RequestID](){
                // find vector<AckInfo> from m_AckInfoMap by RequestID 
                auto ackInfoVec = m_AckInfoMap.find(RequestID);
                if (ackInfoVec == m_AckInfoMap.end()){
                    NDN_LOG_ERROR("AckInfo vector not found for RequestID: " << RequestID.toUri());
                    return;
                }

                // if ackInfoVect.size() == 0, means no provider responded, change strategy to FirstResponding
                if (ackInfoVec->second.size() == 0){
                     NDN_LOG_ERROR("After waiting for 100 ms, No AckInfo found for RequestID: " << RequestID.toUri());
                    // log change strategy to FirstResponding
                    NDN_LOG_INFO("Change strategy of "<< RequestID<< " to FirstResponding");
                    m_strategyMap[RequestID] = tlv::FirstResponding;
                    m_AckInfoMap.erase(ackInfoVec);
                    return;
                }

                // random choose one AckInfo from ackInfoVec.second
                auto randomAckInfo = ackInfoVec->second[rand() % ackInfoVec->second.size()];
                // log AckInfo is choosen for LoadBalancing
                NDN_LOG_INFO("Choosen AckInfo for LoadBalancing: " << randomAckInfo.providerName.toUri() << " " << randomAckInfo.requestID.toUri());
                // publish service coordination message
                PublishServiceCoordinationMessage(randomAckInfo.providerName, randomAckInfo.serviceName, randomAckInfo.functionName, randomAckInfo.requestID);
                
            });
        }
    }

    void ServiceUser::PublishRequestV2(const std::vector<ndn::Name>& serviceProviderNames,
                                       const ndn::Name& serviceName,
                                       const ndn::Name& requestId,
                                       const ndn::Buffer& payload,
                                       const size_t& strategy)
    {
        NDN_LOG_INFO("PublishRequestV2: " << serviceName << requestId);

        ndn_service_framework::BloomFilter bloomFilter;
        std::vector<std::pair<std::string, std::string>> pairs =
            UPT.searchByFunctionName(serviceName.toUri());

        if (serviceProviderNames.size() > 0){
            for (auto providerName : serviceProviderNames){
                bloomFilter.insert(providerName.toUri());
            }
        }else{
            for (auto pair : pairs){
                ndn::Name serviceFullName(pair.first);
                bloomFilter.insert(serviceFullName.getPrefix(
                    -static_cast<int>(serviceName.size())).toUri());
            }
        }

        ndn_service_framework::RequestMessage requestMessage;
        std::map<std::string, std::string> tokens;
        for (auto pair : pairs){
            tokens[pair.first] = makeAuthorizationProof(pair.second, requestId);
        }
        requestMessage.setTokens(tokens);
        requestMessage.setPayload(const_cast<ndn::Buffer&>(payload), payload.size());
        requestMessage.setStrategy(strategy);
        requestMessage.WireEncode().data();

        ndn::Name requestName =
            ndn_service_framework::makeRequestNameV2(identity,
                                                     serviceName,
                                                     ndn::Name(bloomFilter.toHexString()),
                                                     requestId);
        ndn::Name requestNameWithoutPrefix =
            ndn_service_framework::makeRequestNameWithoutPrefixV2(
                serviceName,
                ndn::Name(bloomFilter.toHexString()),
                requestId);

        std::cout << "[ServiceUser] selected providerName(s)=";
        if (serviceProviderNames.empty()) {
            std::cout << "<discovery/bloom-filter>";
        }
        else {
            for (size_t i = 0; i < serviceProviderNames.size(); ++i) {
                if (i != 0) {
                    std::cout << ",";
                }
                std::cout << serviceProviderNames[i].toUri();
            }
        }
        std::cout << " selected serviceName=" << serviceName.toUri()
                  << " final request name=" << requestName.toUri()
                  << std::endl;
        NDN_LOG_INFO("PublishRequestV2 selected serviceName=" << serviceName.toUri()
                     << " final request name=" << requestName.toUri());

        auto pendingIt = m_pendingCalls.find(requestId);
        if (pendingIt != m_pendingCalls.end()) {
            pendingIt->second.providers = serviceProviderNames;
            pendingIt->second.serviceName = serviceName;
            pendingIt->second.requestName = requestName;
            pendingIt->second.requestNameWithoutPrefix = requestNameWithoutPrefix;
            pendingIt->second.requestMessage = requestMessage;
            pendingIt->second.strategy = strategy;
        }

        if (m_requestPublisher) {
            m_requestPublisher(requestId,
                               requestName,
                               serviceProviderNames,
                               serviceName,
                               requestMessage,
                               strategy);
        }
        else {
            PublishMessage(requestName, requestNameWithoutPrefix, requestMessage);
        }

        m_strategyMap.emplace(requestId, strategy);

        if (strategy == tlv::LoadBalancing){
            m_AckInfoMap[requestId] = std::vector<ndn_service_framework::AckInfo>();

            m_scheduler.schedule(100_ms,[this, requestId](){
                auto ackInfoVec = m_AckInfoMap.find(requestId);
                if (ackInfoVec == m_AckInfoMap.end()){
                    NDN_LOG_ERROR("AckInfo vector not found for RequestID: " << requestId.toUri());
                    return;
                }

                if (ackInfoVec->second.size() == 0){
                    NDN_LOG_ERROR("After waiting for 100 ms, No AckInfo found for RequestID: " << requestId.toUri());
                    NDN_LOG_INFO("Change strategy of "<< requestId<< " to FirstResponding");
                    m_strategyMap[requestId] = tlv::FirstResponding;
                    m_AckInfoMap.erase(ackInfoVec);
                    return;
                }

                auto randomAckInfo = ackInfoVec->second[rand() % ackInfoVec->second.size()];
                NDN_LOG_INFO("Choosen AckInfo for LoadBalancing: "
                             << randomAckInfo.providerName.toUri() << " "
                             << randomAckInfo.requestID.toUri());
                PublishServiceCoordinationMessage(randomAckInfo.providerName,
                                                  randomAckInfo.serviceName,
                                                  randomAckInfo.functionName,
                                                  randomAckInfo.requestID);
            });
        }
    }

    ndn::Name ServiceUser::async_call(const std::vector<ndn::Name>& providers,
                                      const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t strategy)
    {
        const ndn::Name requestId = makeRequestId();

        PendingCall pendingCall;
        pendingCall.providers = providers;
        pendingCall.serviceName = serviceName;
        pendingCall.requestMessage = requestMessage;
        pendingCall.strategy = strategy;
        pendingCall.timeoutMs = timeoutMs;
        pendingCall.timeoutHandler = std::move(onTimeout);
        pendingCall.responseHandler = std::move(onResponseHandler);
        m_pendingCalls[requestId] = std::move(pendingCall);

        const auto payload = requestMessage.getPayload();
        PublishRequestV2(providers, serviceName, requestId, payload, strategy);

        if (timeoutMs > 0) {
            m_scheduler.schedule(ndn::time::milliseconds(timeoutMs), [this, requestId]() {
                auto pendingCall = m_pendingCalls.find(requestId);
                if (pendingCall == m_pendingCalls.end()) {
                    return;
                }

                auto timeoutHandler = pendingCall->second.timeoutHandler;
                m_pendingCalls.erase(pendingCall);

                if (timeoutHandler) {
                    timeoutHandler(requestId);
                }
            });
        }

        return requestId;
    }

    ndn::Name ServiceUser::async_call(const std::vector<ndn::Name>& providers,
                                      const ndn::Name& serviceName,
                                      const ndn::Name& functionName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t strategy)
    {
        return async_call(providers,
                          makeUnifiedServiceName(serviceName, functionName),
                          std::move(requestMessage),
                          timeoutMs,
                          std::move(onTimeout),
                          std::move(onResponseHandler),
                          strategy);
    }

    ndn::Name ServiceUser::async_call(const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t strategy)
    {
        return async_call({},
                          serviceName,
                          std::move(requestMessage),
                          timeoutMs,
                          std::move(onTimeout),
                          std::move(onResponseHandler),
                          strategy);
    }

    ndn::Name ServiceUser::async_call(const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int ackTimeoutMs,
                                      AcksHandler onAcksHandler,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler)
    {
        const ndn::Name requestId = makeRequestId();

        PendingCall pendingCall;
        pendingCall.serviceName = serviceName;
        pendingCall.requestMessage = requestMessage;
        pendingCall.strategy = ndn_service_framework::tlv::FirstResponding;
        pendingCall.timeoutMs = timeoutMs;
        pendingCall.ackTimeoutMs = ackTimeoutMs;
        pendingCall.acksHandler = std::move(onAcksHandler);
        pendingCall.timeoutHandler = std::move(onTimeout);
        pendingCall.responseHandler = std::move(onResponseHandler);
        m_pendingCalls[requestId] = std::move(pendingCall);

        const auto payload = requestMessage.getPayload();
        PublishRequestV2({},
                         serviceName,
                         requestId,
                         payload,
                         ndn_service_framework::tlv::FirstResponding);

        if (ackTimeoutMs > 0) {
            m_scheduler.schedule(ndn::time::milliseconds(ackTimeoutMs), [this, requestId]() {
                evaluateAckSelection(requestId);
            });
        }

        if (timeoutMs > 0) {
            m_scheduler.schedule(ndn::time::milliseconds(timeoutMs), [this, requestId]() {
                auto pendingCall = m_pendingCalls.find(requestId);
                if (pendingCall == m_pendingCalls.end()) {
                    return;
                }

                auto timeoutHandler = pendingCall->second.timeoutHandler;
                m_pendingCalls.erase(pendingCall);

                if (timeoutHandler) {
                    timeoutHandler(requestId);
                }
            });
        }

        return requestId;
    }

    ndn::Name ServiceUser::async_call(const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int ackTimeoutMs,
                                      AckCandidatesHandler onAcksHandler,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler)
    {
        const ndn::Name requestId = makeRequestId();

        PendingCall pendingCall;
        pendingCall.serviceName = serviceName;
        pendingCall.requestMessage = requestMessage;
        pendingCall.strategy = ndn_service_framework::tlv::FirstResponding;
        pendingCall.timeoutMs = timeoutMs;
        pendingCall.ackTimeoutMs = ackTimeoutMs;
        pendingCall.ackCandidatesHandler = std::move(onAcksHandler);
        pendingCall.timeoutHandler = std::move(onTimeout);
        pendingCall.responseHandler = std::move(onResponseHandler);
        m_pendingCalls[requestId] = std::move(pendingCall);

        const auto payload = requestMessage.getPayload();
        PublishRequestV2({},
                         serviceName,
                         requestId,
                         payload,
                         ndn_service_framework::tlv::FirstResponding);

        if (ackTimeoutMs > 0) {
            m_scheduler.schedule(ndn::time::milliseconds(ackTimeoutMs), [this, requestId]() {
                evaluateAckSelection(requestId);
            });
        }
        else {
            m_scheduler.schedule(ndn::time::milliseconds(0), [this, requestId]() {
                evaluateAckSelection(requestId);
            });
        }

        if (timeoutMs > 0) {
            m_scheduler.schedule(ndn::time::milliseconds(timeoutMs), [this, requestId]() {
                auto pendingCall = m_pendingCalls.find(requestId);
                if (pendingCall == m_pendingCalls.end()) {
                    return;
                }

                auto timeoutHandler = pendingCall->second.timeoutHandler;
                m_pendingCalls.erase(pendingCall);

                if (timeoutHandler) {
                    timeoutHandler(requestId);
                }
            });
        }

        return requestId;
    }

    ndn::Name ServiceUser::async_call(const std::vector<ndn::Name>& providers,
                                      const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int ackTimeoutMs,
                                      AckCandidatesHandler onAcksHandler,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler)
    {
        const ndn::Name requestId = makeRequestId();

        PendingCall pendingCall;
        pendingCall.providers = providers;
        pendingCall.serviceName = serviceName;
        pendingCall.requestMessage = requestMessage;
        pendingCall.strategy = ndn_service_framework::tlv::FirstResponding;
        pendingCall.timeoutMs = timeoutMs;
        pendingCall.ackTimeoutMs = ackTimeoutMs;
        pendingCall.ackCandidatesHandler = std::move(onAcksHandler);
        pendingCall.timeoutHandler = std::move(onTimeout);
        pendingCall.responseHandler = std::move(onResponseHandler);
        m_pendingCalls[requestId] = std::move(pendingCall);

        const auto payload = requestMessage.getPayload();
        PublishRequestV2(providers,
                         serviceName,
                         requestId,
                         payload,
                         ndn_service_framework::tlv::FirstResponding);

        if (ackTimeoutMs > 0) {
            m_scheduler.schedule(ndn::time::milliseconds(ackTimeoutMs), [this, requestId]() {
                evaluateAckSelection(requestId);
            });
        }
        else {
            m_scheduler.schedule(ndn::time::milliseconds(0), [this, requestId]() {
                evaluateAckSelection(requestId);
            });
        }

        if (timeoutMs > 0) {
            m_scheduler.schedule(ndn::time::milliseconds(timeoutMs), [this, requestId]() {
                auto pendingCall = m_pendingCalls.find(requestId);
                if (pendingCall == m_pendingCalls.end()) {
                    return;
                }

                auto timeoutHandler = pendingCall->second.timeoutHandler;
                m_pendingCalls.erase(pendingCall);

                if (timeoutHandler) {
                    timeoutHandler(requestId);
                }
            });
        }

        return requestId;
    }

    void ServiceUser::handleResponse(const ndn::Name& requestId,
                                     const ndn_service_framework::ResponseMessage& responseMessage)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            return;
        }

        if (pendingCall->second.responseHandler) {
            pendingCall->second.responseHandler(responseMessage);
        }

        if (pendingCall->second.strategy != ndn_service_framework::tlv::NoCoordination) {
            m_pendingCalls.erase(pendingCall);
        }
    }

    bool ServiceUser::handleDecryptedResponse(
        const ndn::Name& requestId,
        const ndn_service_framework::ResponseMessage& responseMessage)
    {
        if (m_pendingCalls.find(requestId) == m_pendingCalls.end()) {
            return false;
        }

        handleResponse(requestId, responseMessage);
        return true;
    }

    bool ServiceUser::handleDecryptedResponse(const ndn::Name& requestId,
                                              const ndn::Block& responseBlock)
    {
        ndn_service_framework::ResponseMessage responseMessage;
        if (!responseMessage.WireDecode(responseBlock)) {
            return false;
        }

        return handleDecryptedResponse(requestId, responseMessage);
    }

    bool ServiceUser::handleDecryptedResponseByName(
        const ndn::Name& responseName,
        const ndn_service_framework::ResponseMessage& responseMessage)
    {
        auto parsedV2 = ndn_service_framework::parseResponseNameV2(responseName);
        if (parsedV2) {
            return handleDecryptedResponse(parsedV2->requestId, responseMessage);
        }

        auto parsed = ndn_service_framework::parseResponseName(responseName);
        if (!parsed) {
            NDN_LOG_ERROR("handleDecryptedResponseByName: parseResponseName failed: " << responseName);
            return false;
        }

        ndn::Name requesterIdentity;
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name functionName;
        ndn::Name requestId;
        std::tie(requesterIdentity, providerName, serviceName, functionName, requestId) =
            parsed.value();

        return handleDecryptedResponse(requestId, responseMessage);
    }

    bool ServiceUser::handleDecryptedResponseByName(const ndn::Name& responseName,
                                                    const ndn::Block& responseBlock)
    {
        ndn_service_framework::ResponseMessage responseMessage;
        if (!responseMessage.WireDecode(responseBlock)) {
            return false;
        }

        return handleDecryptedResponseByName(responseName, responseMessage);
    }

    bool ServiceUser::handleRequestAckByName(
        const ndn::Name& ackName,
        const ndn_service_framework::RequestAckMessage& ackMessage)
    {
        auto parsedV2 = ndn_service_framework::parseRequestAckNameV2(ackName);
        if (parsedV2) {
            auto pendingCall = m_pendingCalls.find(parsedV2->requestId);
            if (pendingCall == m_pendingCalls.end()) {
                return false;
            }

            pendingCall->second.requestAcks.push_back(
                StoredAck{parsedV2->providerName,
                          parsedV2->serviceName,
                          parsedV2->requestId,
                          ackMessage});
            if (pendingCall->second.acksHandler ||
                pendingCall->second.ackCandidatesHandler) {
                return true;
            }
            evaluateAckSelection(parsedV2->requestId);

            return true;
        }

        auto parsed = ndn_service_framework::parseRequestAckName(ackName);
        if (!parsed) {
            NDN_LOG_ERROR("handleRequestAckByName: parseRequestAckName failed: " << ackName);
            return false;
        }

        ndn::Name providerName;
        ndn::Name requesterName;
        ndn::Name serviceName;
        ndn::Name functionName;
        ndn::Name requestId;
        std::tie(providerName, requesterName, serviceName, functionName, requestId) =
            parsed.value();

        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            return false;
        }

        pendingCall->second.requestAcks.push_back(
            StoredAck{providerName,
                      makeUnifiedServiceName(serviceName, functionName),
                      requestId,
                      ackMessage});
        if (pendingCall->second.acksHandler ||
            pendingCall->second.ackCandidatesHandler) {
            return true;
        }
        evaluateAckSelection(requestId);

        return true;
    }

    bool ServiceUser::handleRequestAckByName(const ndn::Name& ackName,
                                             const ndn::Block& ackBlock)
    {
        ndn_service_framework::RequestAckMessage ackMessage;
        if (!ackMessage.WireDecode(ackBlock)) {
            return false;
        }

        return handleRequestAckByName(ackName, ackMessage);
    }

    ndn::Name ServiceUser::makeRequestId()
    {
        return ndn::Name(ndn::time::toIsoString(ndn::time::system_clock::now()));
    }

    ndn::Name ServiceUser::makeUnifiedServiceName(const ndn::Name& serviceName,
                                                  const ndn::Name& functionName)
    {
        if (functionName.empty()) {
            return serviceName;
        }

        ndn::Name unified(serviceName);
        for (const auto& component : functionName) {
            unified.append(component);
        }
        return unified;
    }

    bool ServiceUser::evaluateAckSelection(const ndn::Name& requestId)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            return false;
        }

        if (pendingCall->second.acksHandler ||
            pendingCall->second.ackCandidatesHandler) {
            return evaluateCustomAckSelection(pendingCall->second);
        }

        return evaluateBuiltInAckSelection(pendingCall->second);
    }

    bool ServiceUser::evaluateCustomAckSelection(PendingCall& pendingCall)
    {
        pendingCall.customSelectedAcks.clear();
        pendingCall.successfulAckProviders.clear();
        pendingCall.selectedProvider = ndn::Name();

        if (pendingCall.ackCandidatesHandler) {
            std::vector<ndn_service_framework::AckSelectionCandidate> candidates;
            for (const auto& storedAck : pendingCall.requestAcks) {
                candidates.push_back({storedAck.providerName,
                                      storedAck.serviceName,
                                      storedAck.requestId,
                                      storedAck.message});
            }

            const auto selectedCandidates = pendingCall.ackCandidatesHandler(candidates);
            for (const auto& selectedCandidate : selectedCandidates) {
                for (const auto& storedAck : pendingCall.requestAcks) {
                    if (!storedAck.providerName.equals(selectedCandidate.providerName) ||
                        !storedAck.serviceName.equals(selectedCandidate.serviceName) ||
                        !storedAck.requestId.equals(selectedCandidate.requestId)) {
                        continue;
                    }
                    if (!ackEquals(storedAck.message, selectedCandidate.ack)) {
                        continue;
                    }

                    if (!storedAck.message.getStatus()) {
                        break;
                    }

                    pendingCall.customSelectedAcks.push_back(storedAck);
                    addUniqueName(pendingCall.successfulAckProviders, storedAck.providerName);
                    if (pendingCall.selectedProvider.empty()) {
                        pendingCall.selectedProvider = storedAck.providerName;
                    }
                    break;
                }
            }
        }
        else {
            std::vector<ndn_service_framework::RequestAckMessage> ackMessages;
            for (const auto& storedAck : pendingCall.requestAcks) {
                ackMessages.push_back(storedAck.message);
            }

            const auto selectedMessages = pendingCall.acksHandler(ackMessages);
            for (const auto& selectedMessage : selectedMessages) {
                const auto* storedAck = findStoredAck(pendingCall, selectedMessage);
                if (storedAck == nullptr || !storedAck->message.getStatus()) {
                    continue;
                }

                pendingCall.customSelectedAcks.push_back(*storedAck);
                addUniqueName(pendingCall.successfulAckProviders, storedAck->providerName);
                if (pendingCall.selectedProvider.empty()) {
                    pendingCall.selectedProvider = storedAck->providerName;
                }
            }
        }

        for (const auto& selectedAck : pendingCall.customSelectedAcks) {
            PublishServiceCoordinationMessageV2(selectedAck.providerName,
                                                selectedAck.serviceName,
                                                selectedAck.requestId);
        }

        return true;
    }

    bool ServiceUser::evaluateBuiltInAckSelection(PendingCall& pendingCall)
    {
        pendingCall.successfulAckProviders.clear();
        for (const auto& storedAck : pendingCall.requestAcks) {
            if (storedAck.message.getStatus()) {
                addUniqueName(pendingCall.successfulAckProviders, storedAck.providerName);
            }
        }

        if (pendingCall.strategy == ndn_service_framework::tlv::FirstResponding) {
            if (pendingCall.selectedProvider.empty() && !pendingCall.successfulAckProviders.empty()) {
                pendingCall.selectedProvider = pendingCall.successfulAckProviders.front();
            }
            return true;
        }

        if (pendingCall.strategy == ndn_service_framework::tlv::LoadBalancing) {
            pendingCall.selectedProvider = selectLoadBalancingProvider(pendingCall.successfulAckProviders);
            return true;
        }

        if (pendingCall.strategy == ndn_service_framework::tlv::NoCoordination) {
            pendingCall.selectedProvider = ndn::Name();
            return true;
        }

        return false;
    }

    bool ServiceUser::containsName(const std::vector<ndn::Name>& names,
                                   const ndn::Name& name)
    {
        for (const auto& item : names) {
            if (item.equals(name)) {
                return true;
            }
        }
        return false;
    }

    void ServiceUser::addUniqueName(std::vector<ndn::Name>& names,
                                    const ndn::Name& name)
    {
        if (!name.empty() && !containsName(names, name)) {
            names.push_back(name);
        }
    }

    ndn::Name ServiceUser::selectLoadBalancingProvider(
        const std::vector<ndn::Name>& providers)
    {
        if (providers.empty()) {
            return ndn::Name();
        }

        ndn::Name selected = providers.front();
        for (const auto& provider : providers) {
            if (provider.toUri() < selected.toUri()) {
                selected = provider;
            }
        }
        return selected;
    }

    const ServiceUser::StoredAck* ServiceUser::findStoredAck(
        const PendingCall& pendingCall,
        const ndn_service_framework::RequestAckMessage& ackMessage)
    {
        for (const auto& storedAck : pendingCall.requestAcks) {
            if (ackEquals(storedAck.message, ackMessage)) {
                return &storedAck;
            }
        }
        return nullptr;
    }

    void ServiceUser::processNDNSDServiceInfoCallback(const ndnsd::discovery::Details &details)
    {
        NDN_LOG_INFO("processNDNSDServiceInfoCallback: Service publish callback received");
        // find key name = "tokenNames"
        if (details.serviceMetaInfo.find("tokenName") != details.serviceMetaInfo.end()) {
            std::string tokenName = details.serviceMetaInfo.at("tokenName");
            // split tokens uing ";"

            // consume tokenName using NAC-ABE
            ndn::Name providerName, ServiceName, FunctionName, seqNum;
            
            auto result = ndn_service_framework::parsePermissionTokenName(ndn::Name(tokenName));
            if(result){
                std::tie(providerName, ServiceName, FunctionName, seqNum) = result.value();
            nacConsumer.consume(
                ndn::Name(tokenName),
                std::bind(&ServiceUser::OnPermissionTokenDecryptionSuccessCallback, this, providerName, ServiceName, FunctionName, seqNum, _1),
                std::bind(&ServiceUser::OnPermissionTokenDecryptionErrorCallback, this, providerName, ServiceName, FunctionName, seqNum, _1));
            }else{
                NDN_LOG_ERROR("Invalid token name: " << tokenName); 
            }
            
        } else {
            NDN_LOG_ERROR("No tokenNames found in service publish callback");
        }


    }

    void ServiceUser::onPermissionResponseData(const ndn::Interest&,
                                               const ndn::Data& data)
    {
        handlePermissionResponseData(data, identity, m_keyChain, UPT);
    }

    void ServiceUser::onPermissionResponseTimeout(const ndn::Interest& interest)
    {
        NDN_LOG_ERROR("PermissionResponse timeout: " << interest.getName());
    }

    void ServiceUser::OnRequestAck(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        if(!isFresh(subscription)) {
            return;
        }
        // log message
        NDN_LOG_INFO("OnRequestAck: " << subscription.name);

        auto ackV2 = parseRequestAckNameV2(subscription.name);
        if (ackV2) {
            if(subscription.data.size() > 0){
                nacConsumer.consume(
                            ndn::Name(subscription.name),
                            ndn::Block(subscription.data),
                            std::bind(&ServiceUser::OnRequestAckDecryptionSuccessCallback,
                                      this,
                                      ackV2->providerName,
                                      ackV2->serviceName,
                                      ndn::Name(),
                                      ackV2->requestId,
                                      _1),
                            std::bind(&ServiceUser::OnRequestAckDecryptionErrorCallback,
                                      this,
                                      ackV2->providerName,
                                      ackV2->serviceName,
                                      ndn::Name(),
                                      ackV2->requestId,
                                      _1));
            }else{
                nacConsumer.consume(
                            ndn::Name(subscription.name),
                            std::bind(&ServiceUser::OnRequestAckDecryptionSuccessCallback,
                                      this,
                                      ackV2->providerName,
                                      ackV2->serviceName,
                                      ndn::Name(),
                                      ackV2->requestId,
                                      _1),
                            std::bind(&ServiceUser::OnRequestAckDecryptionErrorCallback,
                                      this,
                                      ackV2->providerName,
                                      ackV2->serviceName,
                                      ndn::Name(),
                                      ackV2->requestId,
                                      _1));
            }
            return;
        }

        // parse permission ack name
        ndn::Name providerName, userName, ServiceName, FunctionName, seqNum;
        auto results = parseRequestAckName(subscription.name);
        if (!results)
        {   
            NDN_LOG_ERROR("parseRequestAckName failed: " << subscription.name);
            return;
        }
        std::tie(providerName, userName, ServiceName, FunctionName, seqNum) = results.value();
        
        if(subscription.data.size() > 0){
            nacConsumer.consume(
                        ndn::Name(subscription.name),
                        ndn::Block(subscription.data),
                        std::bind(&ServiceUser::OnRequestAckDecryptionSuccessCallback, this, providerName, ServiceName, FunctionName, seqNum, _1),
                        std::bind(&ServiceUser::OnRequestAckDecryptionErrorCallback, this, providerName, ServiceName, FunctionName, seqNum, _1));
        }else{
            nacConsumer.consume(
                        ndn::Name(subscription.name),
                        std::bind(&ServiceUser::OnRequestAckDecryptionSuccessCallback, this, providerName, ServiceName, FunctionName, seqNum, _1),
                        std::bind(&ServiceUser::OnRequestAckDecryptionErrorCallback, this, providerName, ServiceName, FunctionName, seqNum, _1));
        }

    }

    void ServiceUser::OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        if(!isFresh(subscription)) {
            return;
        }

        NDN_LOG_INFO("OnResponse: " << subscription.name);

        ndn::Name requesterName, providerName, ServiceName, FunctionName, RequestId;
        auto resultsV2 = ndn_service_framework::parseResponseNameV2(subscription.name);
        if (resultsV2) {
            requesterName = resultsV2->requesterName;
            providerName = resultsV2->providerName;
            ServiceName = resultsV2->serviceName;
            FunctionName = ndn::Name();
            RequestId = resultsV2->requestId;
        }
        else {
            auto results = ndn_service_framework::parseResponseName(subscription.name);
            if (!results) {
            NDN_LOG_ERROR("parseResponseName failed: " << subscription.name);
            return;
            }
            std::tie(requesterName, providerName, ServiceName, FunctionName, RequestId) =
                results.value();
        }

        const ndn::Name responseName(subscription.name);
        auto onSuccess = [this, responseName](const ndn::Buffer& buffer) {
            ndn::Block responseBlock(buffer);
            if (!handleDecryptedResponseByName(responseName, responseBlock)) {
                NDN_LOG_INFO("OnResponse: no pending async callback for " << responseName);
            }
        };

        if(subscription.data.size() > 0){
            nacConsumer.consume(
                        ndn::Name(subscription.name),
                        ndn::Block(subscription.data),
                        onSuccess,
                        std::bind(&ServiceUser::OnResponseDecryptionErrorCallback, this, providerName, ServiceName, FunctionName, RequestId, _1));
        }else{
            nacConsumer.consume(
                        ndn::Name(subscription.name),
                        onSuccess,
                        std::bind(&ServiceUser::OnResponseDecryptionErrorCallback, this, providerName, ServiceName, FunctionName, RequestId, _1));
        }
    }

void ServiceUser::OnRequestAckDecryptionSuccessCallback(
    const ndn::Name& providerName,
    const ndn::Name& ServiceName,
    const ndn::Name& FunctionName,
    const ndn::Name& requestID,
    const ndn::Buffer& buffer)
{
    // Copy raw bytes across thread boundary (DO NOT pass Block/Buffer)
    auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());

    // ndnsf::post([this,
    //              providerName,
    //              ServiceName,
    //              FunctionName,
    //              requestID,
    //              raw]() mutable
    // {
        // Reconstruct TLV block safely
        ndn::Block block(ndn::span<const uint8_t>(raw->data(), raw->size()));

        try {
            block.parse();
        }
        catch (const std::exception& e) {
            NDN_LOG_ERROR("OnRequestAckDecryptionSuccessCallback: Failed to parse Block: " << e.what());
            return;
        }

        // Logging
        NDN_LOG_INFO("OnRequestAckDecryptionSuccessCallback: "
                     << providerName.toUri() << " "
                     << ServiceName.toUri() << " "
                     << FunctionName.toUri() << " "
                     << requestID.toUri());

        // Decode Permission Ack message
        RequestAckMessage AckMessage;
        try {
            AckMessage.WireDecode(block);
        }
        catch (const std::exception& e) {
            NDN_LOG_ERROR("RequestAckMessage decode failed: " << e.what());
            return;
        }

        const auto ackPayload = AckMessage.getPayload();
        const std::string ackPayloadText(
            reinterpret_cast<const char*>(ackPayload.data()),
            ackPayload.size());
        std::cout << "[ServiceUser] ACK received timestampMs="
                  << nowMilliseconds()
                  << " providerName=" << providerName.toUri()
                  << " status=" << AckMessage.getStatus()
                  << " message=" << AckMessage.getMessage()
                  << " payload=" << ackPayloadText
                  << std::endl;

        const ndn::Name ackName = FunctionName.empty() ?
            ndn_service_framework::makeRequestAckNameV2(providerName,
                                                        identity,
                                                        ServiceName,
                                                        requestID) :
            ndn_service_framework::makeRequestAckName(providerName,
                                                      identity,
                                                      ServiceName,
                                                      FunctionName,
                                                      requestID);
        const bool handledByPendingCall = handleRequestAckByName(ackName, AckMessage);
        auto pendingCall = m_pendingCalls.find(requestID);
        if (handledByPendingCall &&
            pendingCall != m_pendingCalls.end() &&
            (pendingCall->second.acksHandler ||
             pendingCall->second.ackCandidatesHandler)) {
            return;
        }

        // Check permission
        if (!AckMessage.getStatus()) {
            NDN_LOG_ERROR("Permission Denied by Provider: "
                          << providerName.toUri() << " "
                          << ServiceName.toUri() << " "
                          << FunctionName.toUri() << " "
                          << requestID.toUri() << " "
                          << "message: " << AckMessage.getMessage());
            return;
        }

        // Lookup strategy
        auto strategyIt = m_strategyMap.find(requestID);
        if (strategyIt == m_strategyMap.end()) {
            NDN_LOG_ERROR("Strategy not found for requestID: " << requestID.toUri());
            return;
        }

        auto strategy = strategyIt->second;
        NDN_LOG_INFO("Strategy: " << strategy);

        // Decision based on strategy
        if (strategy == tlv::NoCoordination) {
            PublishServiceCoordinationMessage(providerName, ServiceName, FunctionName, requestID);
        }
        else if (strategy == tlv::FirstResponding) {
            PublishServiceCoordinationMessage(providerName, ServiceName, FunctionName, requestID);
            m_strategyMap.erase(strategyIt);
        }
        else if (strategy == tlv::LoadBalancing) {
            // Ensure vector exists
            auto itAck = m_AckInfoMap.find(requestID);
            if (itAck == m_AckInfoMap.end()) {
                NDN_LOG_ERROR("AckInfo vector missing for RequestID: " << requestID.toUri());
                return;
            }

            // Add provider ACK info
            itAck->second.push_back({providerName, ServiceName, FunctionName, requestID});

            NDN_LOG_INFO("AckInfo added for RequestID: "
                         << providerName.toUri() << " "
                         << requestID.toUri());
        }
        else if (strategy == tlv::NoCoordination) {
            // Directly publish coordination message
            PublishServiceCoordinationMessage(providerName, ServiceName, FunctionName, requestID);
        }
        else {
            NDN_LOG_ERROR("Invalid strategy: " << strategy);
            return;
        }
    // });
}



    void ServiceUser::OnRequestAckDecryptionErrorCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID, const std::string &error)
    {
        // log error
        NDN_LOG_ERROR("OnRequestAckDecryptionErrorCallback: " << providerName.toUri() << ServiceName.toUri() << FunctionName.toUri() << requestID.toUri() << " with error: " << error);
    }

    void ServiceUser::PublishServiceCoordinationMessage(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID)
    {
        if (FunctionName.empty()) {
            PublishServiceCoordinationMessageV2(providerName, ServiceName, requestID);
            return;
        }

        // log message
        NDN_LOG_INFO("PublishServiceCoordinationMessage: " << providerName.toUri() << ServiceName.toUri() << FunctionName.toUri() << requestID.toUri());
        // create service coordination message
        ServiceCoordinationMessage coordinationMessage;
        coordinationMessage.setRequestIDs({requestID.toUri()});

        // make service coordination message name
        ndn::Name serviceCoordinationName = makeServiceCoordinationName(identity, providerName, ServiceName, FunctionName, requestID);
        ndn::Name serviceCoordinationNameWithoutPrefix = makeServiceCoordinationNameWithoutPrefix(providerName, ServiceName, FunctionName, requestID);

        // publish service coordination message
        PublishMessage(serviceCoordinationName, serviceCoordinationNameWithoutPrefix, coordinationMessage);
    }

    void ServiceUser::PublishServiceCoordinationMessageV2(const ndn::Name& providerName,
                                                          const ndn::Name& serviceName,
                                                          const ndn::Name& requestId)
    {
        NDN_LOG_INFO("PublishServiceCoordinationMessageV2: "
                     << providerName.toUri()
                     << serviceName.toUri()
                     << requestId.toUri());

        ServiceCoordinationMessage coordinationMessage;
        coordinationMessage.setRequestIDs({requestId.toUri()});

        ndn::Name serviceCoordinationName =
            makeServiceCoordinationNameV2(identity, providerName, serviceName, requestId);
        ndn::Name serviceCoordinationNameWithoutPrefix =
            makeServiceCoordinationNameWithoutPrefixV2(providerName, serviceName, requestId);

        PublishMessage(serviceCoordinationName,
                       serviceCoordinationNameWithoutPrefix,
                       coordinationMessage);
    }

    void ServiceUser::onMissingData(const std::vector<ndn::svs::MissingDataInfo>& infoVector)
    {
        // for (const auto& info : infoVector) {
        //     NDN_LOG_INFO("onMissingData from node " << info.nodeId
        //                 << " seq range [" << info.low << ", " << info.high << "]");
        // }
    }


    void ServiceUser::OnPermissionTokenDecryptionSuccessCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &seqNum, const ndn::Buffer &buffer)
    {
        // Buffer to const char*
        const char* dataPtr = reinterpret_cast<const char*>(buffer.data());
        // log names and tokens
        NDN_LOG_INFO("OnPermissionTokenDecryptionSuccessCallback: " << providerName.toUri() << ServiceName.toUri() << FunctionName.toUri() << seqNum.toUri() << "token:" << std::string(dataPtr, buffer.size()));
        // insert token into UPT
        UPT.insertPermission(ndn::Name(providerName.toUri()).append(ServiceName).append(FunctionName).toUri(), 
            ndn::Name(ServiceName.toUri()).append(FunctionName).toUri(), 
            std::string(dataPtr, buffer.size()));
    }

    void ServiceUser::OnPermissionTokenDecryptionErrorCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &seqNum, const std::string &error)
    {
        NDN_LOG_ERROR("OnPermissionTokenDecryptionErrorCallback: No access to service " << providerName << ServiceName << FunctionName << " with error: " << error);
    }

    bool ServiceUser::replyFromIMS(const ndn::Interest &interest)
    {
        auto data = m_IMS.find(interest.getName());
        if (data != nullptr)
        {
            NDN_LOG_TRACE("Reply from IMS: " << interest.getName().toUri());
            m_face.put(*data);
        }else{
            m_IMS.size();
            NDN_LOG_TRACE("Not Found In IMS: " << interest.getName().toUri()<<" SIZE: "<< m_IMS.size());
        }
        return false;
    }

    void ServiceUser::onPrefixRegisterFailure(const ndn::Name &prefix, const std::string &reason)
    {
        // log error
        NDN_LOG_ERROR("Prefix registration failed for prefix " << prefix.toUri() << " reason: " << reason);
    }
    void ServiceUser::onInterest(const ndn::InterestFilter &, const ndn::Interest &interest)
    {
        NDN_LOG_INFO("Received Interest: " << interest.getName().toUri());
        replyFromIMS(interest);
        
    }
    void ServiceUser::serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data> &contentData, ndn::nacabe::SPtrVector<ndn::Data> &ckData)
    {
        //log data
        NDN_LOG_INFO("serveDataWithIMS: " << contentData.size() << " " << ckData.size());
        std::lock_guard<std::mutex> lock(_cache_mutex);
        // contentData is now served by svsps
        for (auto data : contentData)
        {
            m_IMS.insert(*data);
        }
        for (auto data : ckData)
        {
            m_IMS.insert(*data); 
        }
    }
    void ServiceUser::PublishMessage(const ndn::Name &messageName, const ndn::Name &messageNameWithoutPrefix,AbstractMessage &message)
    {
        // log message
        NDN_LOG_INFO("PublishMessage: " << messageName.toUri());

        ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
        auto results = ndn_service_framework::GetAttributesByName(messageName);
        if (!results)
        {
            NDN_LOG_ERROR("GetAttributesByName failed: " << messageName);
            return;
        }
        std::tie(contentData, ckData) =
            nacProducer.produce(messageNameWithoutPrefix, 
                ndn_service_framework::GetAttributesByName(messageName).value(), 
                ndn::span<const uint8_t>(message.WireEncode().data(), message.WireEncode().size()),
                m_signingInfo);
        // m_face.getIoContext().post([this,
        //  messageName,
        //  contentData = std::move(contentData),
        //  ckData      = std::move(ckData)]() mutable
        // {
        serveDataWithIMS(contentData, ckData);
        auto buffer = mergeDataContents(contentData);
        ndn::Block contentBlock(buffer);
        m_svsps->publish(messageName, contentBlock);
        //});
        NDN_LOG_INFO("Message Published: " << messageName.toUri() << " " << contentBlock.value_size());
    }
    void ServiceUser::registerNDNSFMessages()
    {

        // log register
        NDN_LOG_INFO("Register NDNSF Messages in ndn-svs");

        // V2 ACK/RESPONSE subscriptions match every provider and parse counted
        // requester/service names from the message body.
        std::string regex_str = "^(<>*)<NDNSF><ACK>(<>*)$";
        NDN_LOG_INFO(regex_str);
        m_svsps->subscribeWithRegex(ndn::Regex(regex_str),
                                    std::bind(&ServiceUser::OnRequestAck, this, _1),
                                    true, false);
        std::string regex_str2 = "^(<>*)<NDNSF><RESPONSE>(<>*)$";
        NDN_LOG_INFO(regex_str2);
        m_svsps->subscribeWithRegex(ndn::Regex(regex_str2),
                                    std::bind(&ServiceUser::OnResponse, this, _1),
                                    true, false);
        
    }
    void ServiceUser::requestForServiceInfo()
    {
        NDN_LOG_DEBUG("Requesting Service Info");
    }

    bool ServiceUser::isFresh(const ndn::svs::SVSPubSub::SubscriptionData& subscription)
    {
        const ndn::Name& producerPrefix = subscription.producerPrefix;

        if (producerPrefix.size() < 1)
            return false;

        std::string lastComponentStr = producerPrefix[-1].toUri();
        int sessionID = 0;

        try {
            sessionID = std::stoi(lastComponentStr);
        }
        catch (const std::invalid_argument& e) {
            NDN_LOG_WARN("Wrong sessionID" << lastComponentStr);
            return false;
        }
        catch (const std::out_of_range& e) {
            NDN_LOG_WARN("Wrong sessionID: " << lastComponentStr);
            return false;
        }

        ndn::Name basePrefix = producerPrefix.getPrefix(-1); // 去掉最后一个component作为key

        auto it = m_sessionIDMap.find(basePrefix);
        if (it != m_sessionIDMap.end()) {
            if (it->second > sessionID) {
                return false;
            }
        }

        // Update
        m_sessionIDMap[basePrefix] = sessionID;
        return true;
    }


    void ServiceUser::OnResponseDecryptionErrorCallback(const ndn::Name& serviceProviderName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const std::string & msg)
    {
        std::cout << "[ServiceUser] OnResponseDecryptionErrorCallback provider="
                  << serviceProviderName.toUri()
                  << " service=" << ServiceName.toUri()
                  << " function=" << FunctionName.toUri()
                  << " requestID=" << RequestID.toUri()
                  << " error=" << msg << std::endl;
        NDN_LOG_ERROR("OnResponseDecryptionErrorCallback: " << serviceProviderName << ServiceName << FunctionName << RequestID << " with error: " << msg);
    }
}
