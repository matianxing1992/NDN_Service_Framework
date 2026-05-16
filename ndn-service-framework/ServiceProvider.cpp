#include <ServiceProvider.hpp>
namespace ndn_service_framework
{
    NDN_LOG_INIT(ndn_service_framework.ServiceProvider);

    namespace
    {
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
    }

    ServiceProvider::ServiceProvider(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath) 
        : m_face(face),
        m_scheduler(m_face.getIoContext()),
        identity(identityCert.getIdentity()),
        validator(std::make_shared<MessageValidator>(trustSchemaPath)),
        identityCert(identityCert),
        // nac_validator(std::move(ndn::security::ValidatorNull())),
        nacConsumer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        nacProducer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        random(ndn::random::getRandomNumberEngine()),
        m_IMS(6000)
    {
        m_ServiceDiscovery.enable(group_prefix,
                                  identity,
                                  m_face,
                                  m_keyChain,
                                  std::bind(&ServiceProvider::processNDNSDServiceInfoCallback, this, _1));

        nac_validator.load(trustSchemaPath);

        nacConsumer.obtainDecryptionKey();

        // Serve NDNSF and ck messages using IMS
        m_face.setInterestFilter(ndn::Name(identity.toUri()).append("NDNSF"),
            std::bind(&ServiceProvider::onInterest, this, _1, _2),
            std::bind(&ServiceProvider::onPrefixRegisterFailure, this, _1, _2));
        m_face.setInterestFilter(ndn::Name(identity.toUri()).append("CK"),
            std::bind(&ServiceProvider::onInterest, this, _1, _2),
            std::bind(&ServiceProvider::onPrefixRegisterFailure, this, _1, _2));

        m_signingInfo = ndn::security::signingByCertificate(identityCert);
        
        // Sign interest packets using a certificate
        ndn::svs::SecurityOptions secOpts(m_keyChain);
        // secOpts.interestSigner->signingInfo.setSigningCertName(cert);
        //secOpts.interestSigner->signingInfo = m_signingInfo;

        secOpts.interestSigner = std::make_shared<CommandInterestSigner>(m_keyChain);
        secOpts.interestSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        secOpts.interestSigner->signingInfo.setSigningKeyName(identityCert.getKeyName());

        //secOpts.interestSigner->signingInfo.setSigningHmacKey("dGhpcyBpcyBhIHNlY3JldCBtZXNzYWdl");

        // Sign data packets using a certificate
        // secOpts.dataSigner->signingInfo.setSha256Signing();
        // secOpts.dataSigner->signingInfo.setSigningCertName(cert);
        // secOpts.dataSigner->signingInfo = m_signingInfo;

        secOpts.dataSigner->signingInfo.setSigningCertName(identityCert.getName());
        secOpts.dataSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);

        // Sign publication packets using a certificate
        // secOpts.pubSigner->signingInfo.setSigningCertName(cert);
        //secOpts.pubSigner->signingInfo = m_signingInfo;

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
        node_id.append("provider");
        int session_id = m_configManager.loadAndIncrement(group_prefix.toUri(),node_id.toUri());
        node_id.append(std::to_string(session_id));

        // Create the Pub/Sub instance
        m_svsps = std::make_shared<ndn::svs::SVSPubSub>(
            ndn::Name(group_prefix),
            ndn::Name(node_id),
            m_face,
            std::bind(&ServiceProvider::onMissingData, this, _1),
            opts,
            secOpts);

        while(!nacConsumer.readyForDecryption()){
            // log waiting for decryption key
            nacConsumer.obtainDecryptionKey();
            NDN_LOG_INFO("Waiting for decryption key");
            face.processEvents(ndn::time::milliseconds(1000));
        }


    }

    ServiceProvider::ServiceProvider(LocalMockTag,
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
        attrAuthorityCertificate(attrAuthorityCertificate),
        nacConsumer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        nacProducer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        random(ndn::random::getRandomNumberEngine()),
        m_IMS(6000),
        m_configManager("/tmp/ndnsf-service-provider-local-mock.conf")
    {
        m_signingInfo = ndn::security::signingByCertificate(identityCert);
    }

    void ServiceProvider::init()
    {
        registerServiceInfo();
        registerNDNSFMessages();
    }

    void ServiceProvider::ConsumeRequest(const ndn::Name& RequesterName,
                                         const ndn::Name& providerName,
                                         const ndn::Name& ServiceName,
                                         const ndn::Name& FunctionName,
                                         const ndn::Name& RequestID,
                                         RequestMessage&)
    {
        const auto unifiedServiceName = makeUnifiedServiceName(ServiceName, FunctionName);
        NDN_LOG_ERROR("No legacy ConsumeRequest handler registered for requester="
                      << RequesterName.toUri()
                      << " provider=" << providerName.toUri()
                      << " service=" << unifiedServiceName.toUri()
                      << " requestId=" << RequestID.toUri());
    }

    void ServiceProvider::registerServiceInfo()
    {
        NDN_LOG_INFO("No provider service info registration configured for "
                     << identity.toUri());
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     AckStrategyHandler ackHandler,
                                     RequestHandler requestHandler)
    {
        m_services[serviceName] = {std::move(ackHandler), std::move(requestHandler)};
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     RequestHandler requestHandler)
    {
        addService(serviceName, AckStrategyHandler{}, std::move(requestHandler));
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     AckStrategyHandler ackHandler,
                                     SimpleRequestHandler requestHandler)
    {
        addService(serviceName,
                   std::move(ackHandler),
                   [handler = std::move(requestHandler)](
                       const ndn::Name&,
                       const ndn::Name&,
                       const ndn::Name&,
                       const ndn::Name&,
                       const RequestMessage& requestMessage) {
                       return handler(requestMessage);
                   });
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     const ndn::Name& functionName,
                                     AckStrategyHandler ackHandler,
                                     RequestHandler requestHandler)
    {
        addService(makeUnifiedServiceName(serviceName, functionName),
                   std::move(ackHandler),
                   std::move(requestHandler));
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     const ndn::Name& functionName,
                                     RequestHandler requestHandler)
    {
        addService(makeUnifiedServiceName(serviceName, functionName),
                   std::move(requestHandler));
    }

    bool ServiceProvider::hasService(const ndn::Name& serviceName) const
    {
        return m_services.find(serviceName) != m_services.end();
    }

    bool ServiceProvider::hasService(const ndn::Name& serviceName,
                                     const ndn::Name& functionName) const
    {
        return hasService(makeUnifiedServiceName(serviceName, functionName));
    }

    ResponseMessage ServiceProvider::dispatchRequest(
        const ndn::Name& requesterIdentity,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const RequestMessage& requestMessage) const
    {
        auto service = m_services.find(serviceName);
        if (service == m_services.end()) {
            return makeErrorResponse("No handler registered for " + serviceName.toUri());
        }

        if (!service->second.requestHandler) {
            return makeErrorResponse("Registered service has no request handler for " +
                                     serviceName.toUri());
        }

        return service->second.requestHandler(requesterIdentity,
                                              providerName,
                                              serviceName,
                                              requestId,
                                              requestMessage);
    }

    ResponseMessage ServiceProvider::dispatchRequest(
        const ndn::Name& requesterIdentity,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& functionName,
        const ndn::Name& requestId,
        const RequestMessage& requestMessage) const
    {
        return dispatchRequest(requesterIdentity,
                               providerName,
                               makeUnifiedServiceName(serviceName, functionName),
                               requestId,
                               requestMessage);
    }

    ResponseMessage ServiceProvider::handleDecryptedRequestByName(
        const ndn::Name& requestName,
        const RequestMessage& requestMessage) const
    {
        auto parsedV2 = ndn_service_framework::parseRequestNameV2(requestName);
        if (parsedV2) {
            return dispatchRequest(parsedV2->requesterName,
                                   identity,
                                   parsedV2->serviceName,
                                   parsedV2->requestId,
                                   requestMessage);
        }

        auto parsed = parseRequestNameForUnifiedService(requestName);
        if (!parsed) {
            return makeErrorResponse("Failed to parse request name " + requestName.toUri());
        }

        return dispatchRequest(parsed->requesterIdentity,
                               identity,
                               parsed->serviceName,
                               parsed->requestId,
                               requestMessage);
    }

    ResponseMessage ServiceProvider::handleDecryptedRequestByName(
        const ndn::Name& requestName,
        const ndn::Block& requestBlock) const
    {
        RequestMessage requestMessage;
        if (!requestMessage.WireDecode(requestBlock)) {
            return makeErrorResponse("Failed to decode RequestMessage for " +
                                     requestName.toUri());
        }

        return handleDecryptedRequestByName(requestName, requestMessage);
    }

    ResponseMessage ServiceProvider::makeErrorResponse(const std::string& errorInfo)
    {
        ResponseMessage response;
        response.setStatus(false);
        response.setErrorInfo(errorInfo);
        return response;
    }

    ndn::Name ServiceProvider::makeUnifiedServiceName(const ndn::Name& serviceName,
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

    std::optional<ServiceProvider::ParsedRequestName>
    ServiceProvider::parseRequestNameForUnifiedService(const ndn::Name& requestName)
    {
        auto parsed = ndn_service_framework::parseRequestName(requestName);
        if (!parsed) {
            return std::nullopt;
        }

        ndn::Name requesterIdentity;
        ndn::Name serviceName;
        ndn::Name functionName;
        ndn::Name bloomFilterName;
        ndn::Name requestId;
        std::tie(requesterIdentity, serviceName, functionName, bloomFilterName, requestId) =
            parsed.value();

        return ParsedRequestName{
            requesterIdentity,
            makeUnifiedServiceName(serviceName, functionName),
            requestId};
    }

    void ServiceProvider::PublishMessage(const ndn::Name &messageName, const ndn::Name &messageNameWithoutPrefix,AbstractMessage &message)
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
        NDN_LOG_INFO("Message Published: " << messageName.toUri() << " " << contentBlock.value_size());
        // });
        
    }

    void ServiceProvider::onMissingData(const std::vector<ndn::svs::MissingDataInfo>& infoVector)
    {
        // for (const auto& info : infoVector) {
        //     NDN_LOG_INFO("onMissingData from node " << info.nodeId
        //                 << " seq range [" << info.low << ", " << info.high << "]");
        // }
    }

    void ServiceProvider::UpdateUPTWithServiceMetaInfo(ndnsd::discovery::Details serviceDetails)
    {
        if (serviceDetails.serviceMetaInfo.find("tokenName") != serviceDetails.serviceMetaInfo.end()) {
            std::string tokenName = serviceDetails.serviceMetaInfo.at("tokenName");

            // parse token and get return values
            ndn::Name providerName, ServiceName, FunctionName, seqNum;
                
            auto result = ndn_service_framework::parsePermissionTokenName(ndn::Name(tokenName));
            if (!result){
                NDN_LOG_ERROR("Invalid Permission Token Name: " << tokenName);    
                return;
            }
            std::tie(providerName, ServiceName, FunctionName, seqNum) = result.value();
            // update UPT
            std::string token = ndn_service_framework::RandomString(16);
            // log tokenName and token
            NDN_LOG_INFO("TokenName: " << tokenName << " Token: " << token);

            UPT.insertPermission(ndn::Name(identity.toUri()).append(ServiceName).append(FunctionName).toUri(), 
                                ndn::Name(ServiceName.toUri()).append(FunctionName).toUri(),
                                token);
            // encrypt the token with nac-abe; and then serve it using IMS
            ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
            const std::vector<std::string> attributes = {"/PERMISSION"+ndn::Name(identity.toUri()).append(ServiceName).append(FunctionName).toUri()};
            std::tie(contentData, ckData) =
                nacProducer.produce(ndn_service_framework::makePermissionTokenNameWithoutPrefix(ServiceName, FunctionName, seqNum),
                    attributes, ndn::make_span(reinterpret_cast<const uint8_t *>(token.data()), token.size()), m_signingInfo);
            // std::tie(contentData, ckData) =
            //     nacProducer.produce(ndn_service_framework::makePermissionTokenNameWithoutPrefix(ServiceName, FunctionName, seqNum),
            //         ndn_service_framework::ConcatenateString(attributes), ndn::make_span(reinterpret_cast<const uint8_t *>(token.data()), token.size()), m_signingInfo);
            
            
            // serve data
            for (auto data : contentData)
                m_IMS.insert(*data);
            for (auto data : ckData)
                m_IMS.insert(*data);

        }
    }


    void ServiceProvider::OnRequest(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        if(!isFresh(subscription)) return;
        // log the request
        NDN_LOG_INFO("OnRequest: " << subscription.name << " " << subscription.data.size());

        auto requestV2 = ndn_service_framework::parseRequestNameV2(subscription.name);
        if (requestV2) {
            std::string bfStr = requestV2->bloomFilter.toUri().substr(1);

            ndn_service_framework::BloomFilter bloomFilter;
            if(!bloomFilter.fromHexString(bfStr))
            {
                NDN_LOG_ERROR("OnRequest: BloomFilter parse failed: " << bfStr);
                return;
            }
            bool isTarget = bloomFilter.contains(this->identity.toUri());
            NDN_LOG_INFO("BloomFilter: " << bfStr << isTarget);

            if(isTarget)
            {
                auto token = UPT.queryPermission(
                    ndn::Name(identity.toUri()).append(requestV2->serviceName).toUri(),
                    requestV2->serviceName.toUri());
                if(!token)
                {
                    NDN_LOG_ERROR("Not serving: " << requestV2->serviceName);
                    return;
                }

                if(subscription.data.size() > 0){
                    nacConsumer.consume(subscription.name,
                                        ndn::Block(subscription.data),
                                        std::bind(&ServiceProvider::OnRequestDecryptionSuccessCallbackV2,
                                                  this,
                                                  requestV2->requesterName,
                                                  requestV2->serviceName,
                                                  requestV2->bloomFilter,
                                                  requestV2->requestId,
                                                  _1),
                                        std::bind(&ServiceProvider::OnRequestDecryptionErrorCallback,
                                                  this,
                                                  requestV2->requesterName,
                                                  requestV2->serviceName,
                                                  ndn::Name(),
                                                  requestV2->requestId,
                                                  _1));

                }else{
                    nacConsumer.consume(subscription.name,
                                        std::bind(&ServiceProvider::OnRequestDecryptionSuccessCallbackV2,
                                                  this,
                                                  requestV2->requesterName,
                                                  requestV2->serviceName,
                                                  requestV2->bloomFilter,
                                                  requestV2->requestId,
                                                  _1),
                                        std::bind(&ServiceProvider::OnRequestDecryptionErrorCallback,
                                                  this,
                                                  requestV2->requesterName,
                                                  requestV2->serviceName,
                                                  ndn::Name(),
                                                  requestV2->requestId,
                                                  _1));

                }
            }
            else
            {
                NDN_LOG_ERROR("OnRequest: Requester is not in the bloom filter: "
                              << requestV2->requesterName);
            }
            return;
        }
        
        ndn::Name RequesterName, ServiceName, FunctionName, bloomFilterName, RequestId;
        
        auto resutls = ndn_service_framework::parseRequestName(subscription.name);
        if(!resutls)
        {
            NDN_LOG_ERROR("OnRequest: parseRequestName failed: " << subscription.name);
            return; // parseRequestName failed
        }
        std::tie(RequesterName, ServiceName, FunctionName, bloomFilterName, RequestId) = resutls.value();

        // check whether its identity in the bloom filter
        std::string bfStr = bloomFilterName.toUri().substr(1);
        
        ndn_service_framework::BloomFilter bloomFilter;
        if(!bloomFilter.fromHexString(bfStr))
        {
            NDN_LOG_ERROR("OnRequest: BloomFilter parse failed: " << bfStr);
            return;
        }
        bool isTarget = bloomFilter.contains(this->identity.toUri());
        // log bfstr and isTarget
        NDN_LOG_INFO("BloomFilter: " << bfStr << isTarget);

        if(isTarget)    // if the identity is in the bloom filter, consume the request
        {
            auto token = UPT.queryPermission(ndn::Name(identity.toUri()).append(ServiceName).append(FunctionName).toUri(), ndn::Name(ServiceName).append(FunctionName).toUri());
            // for (auto item : UPT.userPermissions){
            //     NDN_LOG_INFO("UPT: " << item.left << " " << item.right.first << " " << item.right.second);
            // }
            if(!token)
            {
                NDN_LOG_ERROR("Not serving: " << ServiceName << " function " << FunctionName);
                return;
            }
            // fetch and decrypt the request, and then PreProcess it to check permisison and publish ACK;
            if(subscription.data.size() > 0){
                nacConsumer.consume(subscription.name,
                                    ndn::Block(subscription.data),
                                    std::bind(&ServiceProvider::OnRequestDecryptionSuccessCallback, this, RequesterName, ServiceName, FunctionName, bloomFilterName, RequestId,  _1),
                                    std::bind(&ServiceProvider::OnRequestDecryptionErrorCallback, this, RequesterName, ServiceName, FunctionName, RequestId, _1));
       
            }else{
                nacConsumer.consume(subscription.name,
                                    std::bind(&ServiceProvider::OnRequestDecryptionSuccessCallback, this, RequesterName, ServiceName, FunctionName, bloomFilterName, RequestId,  _1),
                                    std::bind(&ServiceProvider::OnRequestDecryptionErrorCallback, this, RequesterName, ServiceName, FunctionName, RequestId, _1));
       
            }
           
            //preprocessRequest(RequesterName, ServiceName, FunctionName, bloomFilterName, RequestId);
        }
        else    // if the identity is not in the bloom filter, return error message
        {
            NDN_LOG_ERROR("OnRequest: Requester is not in the bloom filter: " << RequesterName);
        }

    }

void ServiceProvider::OnRequestDecryptionSuccessCallbackV2(
    const ndn::Name& requesterIdentity,
    const ndn::Name& serviceName,
    const ndn::Name& bloomFilterName,
    const ndn::Name& requestId,
    const ndn::Buffer& buffer)
{
    auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());

    auto spanBuf = ndn::span<const uint8_t>(raw->data(), raw->size());
    auto [ok, block] = ndn::Block::fromBuffer(spanBuf);

    NDN_LOG_INFO("OnRequestDecryptionSuccessCallbackV2: "
        << requesterIdentity.toUri()
        << serviceName.toUri()
        << bloomFilterName.toUri()
        << requestId.toUri());

    auto token = UPT.queryPermission(
        ndn::Name(identity.toUri()).append(serviceName).toUri(),
        serviceName.toUri());

    if (!token) {
        NDN_LOG_ERROR("Not Serving: " << serviceName);
        return;
    }

    ndn_service_framework::RequestMessage requestMessage;
    requestMessage.WireDecode(block);

    bool isAuthorized = false;
    for (const auto& pair : requestMessage.getTokens()) {
        if (pair.second ==
            std::to_string(std::hash<std::string>()(
                token.value() + requestId.toUri())))
        {
            isAuthorized = true;
            break;
        }
    }

    // For debugging only
    isAuthorized = true;

    if (!isAuthorized) {
        NDN_LOG_ERROR("OnRequestDecryptionSuccessCallbackV2: Permission Denied");
        return;
    }

    if (hasService(serviceName)) {
        NDN_LOG_INFO("Dispatch request using V2 dynamic handler for "
                     << serviceName.toUri());

        auto response = dispatchRequest(requesterIdentity,
                                        identity,
                                        serviceName,
                                        requestId,
                                        requestMessage);
        ndn::Name responseName = makeResponseNameV2(identity,
                                                    requesterIdentity,
                                                    serviceName,
                                                    requestId);
        ndn::Name responseNameWithoutPrefix =
            makeResponseNameWithoutPrefixV2(requesterIdentity,
                                            serviceName,
                                            requestId);
        PublishMessage(responseName, responseNameWithoutPrefix, response);
        return;
    }

    NDN_LOG_INFO("No V2 dynamic handler for " << serviceName.toUri());

    ndn::Name pendingKey = ndn::Name(requesterIdentity.toUri())
                               .append(serviceName)
                               .append(requestId);
    pendingRequests[pendingKey] =
        std::make_shared<RequestMessage>(requestMessage);

    std::string msg = "Permission Granted";
    PublishRequestAckMessageV2(requesterIdentity,
                               serviceName,
                               requestId,
                               true,
                               msg);
}

void ServiceProvider::OnRequestDecryptionSuccessCallback(
    const ndn::Name& requesterIdentity,
    const ndn::Name& ServiceName,
    const ndn::Name& FunctionName,
    const ndn::Name& bloomFilterName,
    const ndn::Name& RequestID,
    const ndn::Buffer& buffer)
{
    // Deep copy buffer so it is safe to pass across threads
    auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());

    // ndnsf::post([this,
    //              requesterIdentity,
    //              ServiceName,
    //              FunctionName,
    //              bloomFilterName,
    //              RequestID,
    //              raw]() mutable
    // {
        // Reconstruct a safe TLV block
        auto spanBuf = ndn::span<const uint8_t>(raw->data(), raw->size());
        auto [ok, block] = ndn::Block::fromBuffer(spanBuf);

        // Logging
        NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: "
            << requesterIdentity.toUri()
            << ServiceName.toUri()
            << FunctionName.toUri()
            << bloomFilterName.toUri()
            << RequestID.toUri());

        // Token permission check
        auto token = UPT.queryPermission(
            ndn::Name(identity.toUri())
                .append(ServiceName)
                .append(FunctionName)
                .toUri(),
            ndn::Name(ServiceName).append(FunctionName).toUri());

        if (!token) {
            NDN_LOG_ERROR("Not Serving: " << ServiceName << " function " << FunctionName);
            return;
        }

        // Decode RequestMessage safely
        ndn_service_framework::RequestMessage requestMessage;
        requestMessage.WireDecode(block);

        // Validate tokens
        bool isAuthorized = false;
        for (const auto& pair : requestMessage.getTokens()) {
            if (pair.second ==
                std::to_string(std::hash<std::string>()(
                    token.value() + RequestID.toUri())))
            {
                isAuthorized = true;
                break;
            }
        }

        // For debugging only
        isAuthorized = true;

        if (!isAuthorized) {
            NDN_LOG_ERROR("OnRequestDecryptionSuccessCallback: Permission Denied");
            return;
        }
        else {
            NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: Permission Granted to "
                << requesterIdentity.toUri()
                << " for " << ServiceName.toUri()
                << " function " << FunctionName.toUri());
        }

        const ndn::Name unifiedServiceName = makeUnifiedServiceName(ServiceName, FunctionName);
        if (hasService(unifiedServiceName)) {
            NDN_LOG_INFO("Dispatch request using dynamic handler for "
                         << unifiedServiceName.toUri());

            auto response = dispatchRequest(requesterIdentity,
                                            identity,
                                            unifiedServiceName,
                                            RequestID,
                                            requestMessage);
            ndn::Name responseName = makeResponseName(identity,
                                                      requesterIdentity,
                                                      ServiceName,
                                                      FunctionName,
                                                      RequestID);
            ndn::Name responseNameWithoutPrefix =
                makeResponseNameWithoutPrefix(requesterIdentity,
                                              ServiceName,
                                              FunctionName,
                                              RequestID);
            PublishMessage(responseName, responseNameWithoutPrefix, response);
            return;
        }

        if (requestMessage.getStrategy() == tlv::NoCoordination) {
            NDN_LOG_INFO("No dynamic handler for "
                         << unifiedServiceName.toUri()
                         << "; falling back to old ConsumeRequest path");
            ConsumeRequest(requesterIdentity,
                           identity,
                           ServiceName,
                           FunctionName,
                           RequestID,
                           requestMessage);
            return;
        }

        NDN_LOG_INFO("No dynamic handler for "
                     << unifiedServiceName.toUri()
                     << "; preserving old ACK/coordination path");

        // Save request into pendingRequests
        ndn::Name pendingKey = ndn::Name(requesterIdentity.toUri())
                                   .append(ServiceName)
                                   .append(FunctionName)
                                   .append(RequestID);

        pendingRequests[pendingKey] =
            std::make_shared<RequestMessage>(requestMessage);

        // Send Permission ACK
        std::string msg = "Permission Granted";
        PublishRequestAckMessage(requesterIdentity,
                                 ServiceName,
                                 FunctionName,
                                 RequestID,
                                 true,
                                 msg);
    //});
}




    void ServiceProvider::OnRequestDecryptionErrorCallback(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const std::string &)
    {
        // log error
        NDN_LOG_ERROR("OnRequestDecryptionErrorCallback: " << requesterIdentity.toUri() << ServiceName.toUri() << FunctionName.toUri() << RequestID.toUri());
    }

void ServiceProvider::processNDNSDServiceInfoCallback(const ndnsd::discovery::Details & callback)
{
        NDN_LOG_INFO("Service publish callback received");
}

    void ServiceProvider::onPermissionResponseData(const ndn::Interest&,
                                                   const ndn::Data& data)
    {
        handlePermissionResponseData(data, identity, m_keyChain, UPT);
    }

    void ServiceProvider::onPermissionResponseTimeout(const ndn::Interest& interest)
    {
        NDN_LOG_ERROR("PermissionResponse timeout: " << interest.getName());
    }

// void ServiceProvider::PublishResponse(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &buffer)
//     {
//         //  /<identity>/NDNSF/RESPONSE/<requesterIdentity>/<ServiceName>/<FunctionName>/<request-id>
//         // identity will be appended by NAC-ABE
        
//         ndn::Name responseName = ndn_service_framework::makeResponseName(identity, requesterIdentity, ServiceName, FunctionName, RequestID);
//         ndn::Name responseNameWithoutPrefix = ndn_service_framework::makeResponseNameWithoutPrefix(requesterIdentity, ServiceName, FunctionName, RequestID);
//         NDN_LOG_INFO("PublishResponse:"<<responseName);
//         // Encrypt the response with nac-abe
//         // publish the encrypted response with ndn-svs, and insert ck into the repo
//         //  contentData segments, and ckData segments
//         ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
//         const std::vector<std::string> attributes = {"/ID"+requesterIdentity.toUri()};
//         std::tie(contentData, ckData) =
//             nacProducer.produce(responseNameWithoutPrefix, attributes, ndn::make_span(reinterpret_cast<const uint8_t *>(buffer.data()), buffer.size()), m_signingInfo);
//         // serve data
//         for (auto data : contentData){
//             //NDN_LOG_INFO(data->getName());
//             m_IMS.insert(*data);
//         }
//         for (auto data : ckData)
//             m_IMS.insert(*data);
//         // content
//         // ndn::BufferPtr contentBuffer = ndn_service_framework::CombineSegmentsIntoBuffer(contentData);
//         // ndn::BufferPtr ckBuffer = ndn_service_framework::CombineSegmentsIntoBuffer(ckData);

//         m_svsps->publish(responseName);
//         //m_svsps->publish(responseName, ndn::make_span(reinterpret_cast<const uint8_t *>(contentBuffer->data()), contentBuffer->size()));
//         NDN_LOG_INFO("Publish Encrypted response" << contentData.at(0)->getName().getPrefix(-1));
//         //m_svsps->publish(ckData.at(0)->getName().getPrefix(-1));
//         //m_svsps->publish(ckData.at(0)->getName().getPrefix(-1), ndn::make_span(reinterpret_cast<const uint8_t *>(ckBuffer->data()), ckBuffer->size()));
//         //NDN_LOG_INFO("ServiceProvider_Drone::PublishResponse CK" << ckData.at(0)->getName().getPrefix(-1));
//     }

    bool ServiceProvider::replyFromIMS(const ndn::Interest &interest)
    {
        auto data = m_IMS.find(interest.getName());
        if (data != nullptr)
        {
            NDN_LOG_TRACE("Reply from IMS: " << interest.getName().toUri());
            m_face.put(*data);
        }else{
            NDN_LOG_TRACE("Not Found In IMS: " << interest.getName().toUri());
            // for(auto d:m_IMS)
            // {
            //     NDN_LOG_TRACE("In IMS: " << d.getName().toUri());
            // }
        }
        return false;
    }

    void ServiceProvider::onPrefixRegisterFailure(const ndn::Name &prefix, const std::string &reason)
    {
        // log error
        NDN_LOG_ERROR("Prefix registration failed for prefix " << prefix.toUri() << " reason: " << reason);
    }
    void ServiceProvider::onInterest(const ndn::InterestFilter &, const ndn::Interest &interest)
    {
        // log interest
        NDN_LOG_INFO("Received Interest: " << interest.getName().toUri());
        replyFromIMS(interest);
        
    }

    void ServiceProvider::serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data> &contentData, ndn::nacabe::SPtrVector<ndn::Data> &ckData)
    {
        //log data
        NDN_LOG_INFO("serveDataWithIMS: " << contentData.size() << " " << ckData.size());
        std::lock_guard<std::mutex> lock(_cache_mutex);
        for (auto data : contentData)
        {
            m_IMS.insert(*data);
        }
        for (auto data : ckData)
        {
            m_IMS.insert(*data); 
        }
    }



    void ServiceProvider::PublishRequestAckMessage(const ndn::Name & requesterIdentity, const ndn::Name & ServiceName, const ndn::Name & FunctionName, const ndn::Name & RequestID, bool status, std::string& msg)
    {
        // log message
        NDN_LOG_INFO("PublishRequestAckMessage: " << requesterIdentity.toUri() << ServiceName.toUri() << FunctionName.toUri() << RequestID.toUri());
        // create Permission Ack Message
        RequestAckMessage RequestAckMessage;
        RequestAckMessage.setStatus(status);
        RequestAckMessage.setMessage(msg);

        ndn::Name name = makeRequestAckName(identity, requesterIdentity, ServiceName, FunctionName, RequestID);
        ndn::Name nameWithouPrefix = makeRequestAckNameWithoutPrefix(requesterIdentity, ServiceName, FunctionName, RequestID);
        PublishMessage(name,nameWithouPrefix,RequestAckMessage);
    }

    void ServiceProvider::PublishRequestAckMessageV2(const ndn::Name& requesterIdentity,
                                                     const ndn::Name& serviceName,
                                                     const ndn::Name& requestId,
                                                     bool status,
                                                     const std::string& msg)
    {
        NDN_LOG_INFO("PublishRequestAckMessageV2: " << requesterIdentity.toUri()
                     << serviceName.toUri() << requestId.toUri());

        RequestAckMessage requestAckMessage;
        requestAckMessage.setStatus(status);
        requestAckMessage.setMessage(msg);

        ndn::Name name = makeRequestAckNameV2(identity,
                                              requesterIdentity,
                                              serviceName,
                                              requestId);
        ndn::Name nameWithoutPrefix =
            makeRequestAckNameWithoutPrefixV2(requesterIdentity,
                                              serviceName,
                                              requestId);
        PublishMessage(name, nameWithoutPrefix, requestAckMessage);
    }

    void ServiceProvider::onServiceCoordinationMessage(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        if(!isFresh(subscription)) return;
        // log message
        NDN_LOG_INFO("Received Service Coordination Message: " << subscription.name.toUri());

        auto coordinationV2 =
            ndn_service_framework::parseServiceCoordinationNameV2(subscription.name);
        if (coordinationV2) {
            if(subscription.data.size() > 0){
                nacConsumer.consume(subscription.name,
                                    ndn::Block(subscription.data),
                                    std::bind(&ServiceProvider::OnServiceCoordinationMessageDecryptionSuccessCallbackV2,
                                              this,
                                              coordinationV2->requesterName,
                                              coordinationV2->providerName,
                                              coordinationV2->serviceName,
                                              coordinationV2->requestId,
                                              _1),
                                    std::bind(&ServiceProvider::OnServiceCoordinationMessageDecryptionErrorCallback,
                                              this,
                                              coordinationV2->requesterName,
                                              coordinationV2->providerName,
                                              coordinationV2->serviceName,
                                              ndn::Name(),
                                              coordinationV2->requestId,
                                              _1));

            }else{
                nacConsumer.consume(subscription.name,
                                    std::bind(&ServiceProvider::OnServiceCoordinationMessageDecryptionSuccessCallbackV2,
                                              this,
                                              coordinationV2->requesterName,
                                              coordinationV2->providerName,
                                              coordinationV2->serviceName,
                                              coordinationV2->requestId,
                                              _1),
                                    std::bind(&ServiceProvider::OnServiceCoordinationMessageDecryptionErrorCallback,
                                              this,
                                              coordinationV2->requesterName,
                                              coordinationV2->providerName,
                                              coordinationV2->serviceName,
                                              ndn::Name(),
                                              coordinationV2->requestId,
                                              _1));
            }
            return;
        }

        // parse ServiceCoordinationMessage
        ndn::Name requesterName, providerName, ServiceName, FunctionName, msgId;
        auto results = ndn_service_framework::parseServiceCoordinationName(subscription.name);
        if (!results)
        {
            NDN_LOG_ERROR("parseServiceCoordinationMessageName failed: " << subscription.name.toUri());
            return;
        }
        std::tie(requesterName, providerName, ServiceName, FunctionName, msgId) = results.value();
        // fetch and decrypt the request, and then PreProcess it to check permisison and publish ACK;
        if(subscription.data.size() > 0){
            nacConsumer.consume(subscription.name,
                                ndn::Block(subscription.data),
                                std::bind(&ServiceProvider::OnServiceCoordinationMessageDecryptionSuccessCallback, this, requesterName, providerName, ServiceName, FunctionName, msgId, _1),
                                std::bind(&ServiceProvider::OnServiceCoordinationMessageDecryptionErrorCallback, this, requesterName, providerName, ServiceName, FunctionName, msgId, _1));
    
        }else{
            nacConsumer.consume(subscription.name,
                                std::bind(&ServiceProvider::OnServiceCoordinationMessageDecryptionSuccessCallback, this, requesterName, providerName, ServiceName, FunctionName, msgId, _1),
                                std::bind(&ServiceProvider::OnServiceCoordinationMessageDecryptionErrorCallback, this, requesterName, providerName, ServiceName, FunctionName, msgId, _1));
        }
        
 

    }

    ndn::Name ServiceProvider::getName()
    {
        return identity;
    }

    void ServiceProvider::fetchPermissionsFromController(const ndn::Name& controllerPrefix)
    {
        ndn::Name interestName(controllerPrefix);
        interestName.append(ndn::Name("/NDNSF/PERMISSIONS/PROVIDER"));
        interestName.append(identity);

        ndn::Interest interest(interestName);
        interest.setCanBePrefix(true);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::seconds(4));

        NDN_LOG_INFO("Fetch provider permissions: " << interestName);
        m_face.expressInterest(
            interest,
            std::bind(&ServiceProvider::onPermissionResponseData, this, _1, _2),
            [this](const ndn::Interest& interest, const ndn::lp::Nack&) {
                onPermissionResponseTimeout(interest);
            },
            std::bind(&ServiceProvider::onPermissionResponseTimeout, this, _1));
    }

    void ServiceProvider::applyPermissionResponse(const PermissionResponse& response)
    {
        if (response.getPermissionKind() != tlv::ProviderPermission) {
            NDN_LOG_ERROR("Ignoring non-provider PermissionResponse for "
                          << response.getTargetIdentity());
            return;
        }

        for (const auto& entry : response.getEntries()) {
            ndn::Name providerServiceName(entry.getProviderName());
            providerServiceName.append(ndn::Name(entry.getServiceName()));
            UPT.insertPermission(providerServiceName.toUri(),
                                 entry.getServiceName(),
                                 entry.getToken());
            NDN_LOG_INFO("Installed provider permission provider="
                         << entry.getProviderName()
                         << " service=" << entry.getServiceName());
        }
    }

    bool ServiceProvider::handlePermissionResponseData(const ndn::Data& data,
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

        if (response.getPermissionKind() != tlv::ProviderPermission) {
            NDN_LOG_ERROR("Ignoring non-provider PermissionResponse for "
                          << response.getTargetIdentity());
            return false;
        }

        for (const auto& entry : response.getEntries()) {
            ndn::Name providerServiceName(entry.getProviderName());
            providerServiceName.append(ndn::Name(entry.getServiceName()));
            permissionTable.insertPermission(providerServiceName.toUri(),
                                             entry.getServiceName(),
                                             entry.getToken());
            NDN_LOG_INFO("Installed provider permission provider="
                         << entry.getProviderName()
                         << " service=" << entry.getServiceName());
        }
        return true;
    }

    void ServiceProvider::OnServiceCoordinationMessageDecryptionSuccessCallbackV2(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& msgId,
        const ndn::Buffer& buffer)
    {
        auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());

        auto spanBuf = ndn::span<const uint8_t>(raw->data(), raw->size());
        auto [ok, block] = ndn::Block::fromBuffer(spanBuf);

        NDN_LOG_INFO("OnServiceCoordinationMessageDecryptionSuccessCallbackV2: "
            << requesterName.toUri()
            << providerName.toUri()
            << serviceName.toUri()
            << msgId.toUri());

        ServiceCoordinationMessage message;
        message.WireDecode(block);

        auto key = ndn::Name(requesterName.toUri())
                    .append(serviceName)
                    .append(msgId);

        auto it = pendingRequests.find(key);
        if (it == pendingRequests.end()) {
            NDN_LOG_INFO("No pending V2 request for " << key.toUri());
            return;
        }

        for (const auto& requestID : message.getRequestIDs()) {
            const ndn::Name requestId(requestID);
            if (!hasService(serviceName)) {
                NDN_LOG_INFO("No V2 dynamic handler for " << serviceName.toUri());
                continue;
            }

            auto response = dispatchRequest(requesterName,
                                            providerName,
                                            serviceName,
                                            requestId,
                                            *(it->second));
            ndn::Name responseName = makeResponseNameV2(providerName,
                                                        requesterName,
                                                        serviceName,
                                                        requestId);
            ndn::Name responseNameWithoutPrefix =
                makeResponseNameWithoutPrefixV2(requesterName,
                                                serviceName,
                                                requestId);
            PublishMessage(responseName, responseNameWithoutPrefix, response);
        }

        pendingRequests.erase(it);
    }


    void ServiceProvider::OnServiceCoordinationMessageDecryptionSuccessCallback(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& ServiceName,
        const ndn::Name& FunctionName,
        const ndn::Name& msgID,
        const ndn::Buffer& buffer)
    {
        // Deep copy the buffer (never pass ndn::Buffer or ndn::Block across threads)
        auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());

        // ndnsf::post([this,
        //             requesterName,
        //             providerName,
        //             ServiceName,
        //             FunctionName,
        //             msgID,
        //             raw]() mutable
        // {
            // Reconstruct Block on NDNSF thread (safe)
            auto spanBuf = ndn::span<const uint8_t>(raw->data(), raw->size());
            auto [ok, block] = ndn::Block::fromBuffer(spanBuf);

            NDN_LOG_INFO("OnServiceCoordinationMessageDecryptionSuccessCallback: "
                << requesterName.toUri()
                << providerName.toUri()
                << ServiceName.toUri()
                << FunctionName.toUri()
                << msgID.toUri());

            // Decode ServiceCoordinationMessage
            ServiceCoordinationMessage message;
            message.WireDecode(block);

            // Build lookup key
            auto key = ndn::Name(requesterName.toUri())
                        .append(ServiceName)
                        .append(FunctionName)
                        .append(msgID);

            auto it = pendingRequests.find(key);
            if (it != pendingRequests.end()) {

                for (const auto& requestID : message.getRequestIDs()) {

                    // Consume corresponding request
                    ConsumeRequest(
                        requesterName,
                        providerName,
                        ServiceName,
                        FunctionName,
                        ndn::Name(requestID),
                        *(it->second));
                }

                // Remove pending record
                pendingRequests.erase(it);
            }
        // });
    }



    void ServiceProvider::OnServiceCoordinationMessageDecryptionErrorCallback(const ndn::Name &requesterName, const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID, const std::string &reason)
    {
        // log error
        NDN_LOG_ERROR("OnServiceCoordinationMessageDecryptionErrorCallback: " << requesterName.toUri() << providerName.toUri() << ServiceName.toUri() << FunctionName.toUri() << msgID.toUri() << " reason: " << reason);

    }

    void ServiceProvider::registerNDNSFMessages()
    {
        // log register
        NDN_LOG_INFO("Register NDNSF Messages in ndn-svs");
        for(auto serviceName:m_serviceNames){ 
            // register Request Message
            ndn::Name sname(serviceName);
            std::string regex_str = "^(<>*)<NDNSF><REQUEST>" + ndn_service_framework::NameToRegexString(sname) ;
            NDN_LOG_INFO(regex_str);
            m_svsps->subscribeWithRegex(ndn::Regex(regex_str),
                                        std::bind(&ServiceProvider::OnRequest, this, _1),
                                        true, false);
            // register Service Coordination Message
            std::string regex_str2 = "^(<>*)<NDNSF><COORDINATION>" + ndn_service_framework::NameToRegexString(identity);
            NDN_LOG_INFO(regex_str2);
            m_svsps->subscribeWithRegex(ndn::Regex(regex_str2),
                                        std::bind(&ServiceProvider::onServiceCoordinationMessage, this, _1),
                                        true, false);
        }
    }

    bool ServiceProvider::isFresh(const ndn::svs::SVSPubSub::SubscriptionData& subscription)
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

}
