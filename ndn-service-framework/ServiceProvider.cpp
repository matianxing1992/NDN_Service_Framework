#include <ServiceProvider.hpp>
namespace ndn_service_framework
{
    NDN_LOG_INIT(ndn_service_framework.ServiceProvider);

    ServiceProvider::ServiceProvider(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath) 
        : m_face(face),
        identity(identityCert.getIdentity()),
        identityCert(identityCert),
        validator(std::make_shared<MessageValidator>(trustSchemaPath)),
        // nac_validator(std::move(ndn::security::ValidatorNull())),
        nacConsumer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        nacProducer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        random(ndn::random::getRandomNumberEngine()),
        m_IMS(6000)
    {
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
        opts.useTimestamp = true;
        opts.maxPubAge = ndn::time::seconds(10);

        // Create the Pub/Sub instance
        m_svsps = std::make_shared<ndn::svs::SVSPubSub>(
            ndn::Name(group_prefix),
            ndn::Name(identity),
            m_face,
            std::bind(&ServiceProvider::onMissingData, this, _1),
            opts,
            secOpts);

        while(!nacConsumer.readyForDecryption()){
            // log waiting for decryption key
            NDN_LOG_INFO("Waiting for decryption key");
            face.processEvents(ndn::time::milliseconds(1000));
        }


    }

    void ServiceProvider::init()
    {   
        registerServiceInfo();
        registerNDNSFMessages();
    }

    void ServiceProvider::onMissingData(const std::vector<ndn::svs::MissingDataInfo> &)
    {
        NDN_LOG_INFO("onMissingData");

    }

    void ServiceProvider::UpdateUPTWithServiceMetaInfo(std::shared_ptr<ndnsd::discovery::ServiceDiscovery> serviceDiscovery)
    {
        std::string tokenNames = serviceDiscovery->m_producerState.serviceMetaInfo["tokenNames"];
        // split tokens uing ";"
        std::vector<std::string> tokenNamesVec;
        boost::split(tokenNamesVec, tokenNames, boost::is_any_of(";"));
        for (auto tokenName : tokenNamesVec)
        {
            // parse token and get return values
            ndn::Name providerName, ServiceName, FunctionName, seqNum;
            
            auto result = ndn_service_framework::parsePermissionTokenName(ndn::Name(tokenName));
            if (!result){
                NDN_LOG_ERROR("Invalid Permission Token Name: " << tokenName);    
                continue;
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
        // log the request
        NDN_LOG_INFO("OnRequest: " << subscription.name << " " << subscription.data.size());
        
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

    void ServiceProvider::OnRequestDecryptionSuccessCallback(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &bloomFilterName,  const ndn::Name &RequestID, const ndn::Buffer & buffer)
    {
        // log request
        NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: " << requesterIdentity.toUri() << ServiceName.toUri() << FunctionName.toUri() << bloomFilterName.toUri() << RequestID.toUri());

        auto token = UPT.queryPermission(ndn::Name(identity.toUri()).append(ServiceName).append(FunctionName).toUri(), ndn::Name(ServiceName).append(FunctionName).toUri());
            
        if(!token)
        {
            NDN_LOG_ERROR("Not Serving:" << ServiceName << " function " << FunctionName);
            return;
        }

        ndn_service_framework::RequestMessage requestMessage;
        requestMessage.WireDecode(ndn::Block(buffer));

        // check whether the tokens in request messaget match the token passed in
        bool isAuthorized = false;
        for (auto pair :requestMessage.getTokens()){
            if (pair.second == std::to_string(std::hash<std::string>()(token.value() + RequestID.toUri())))
            {
                isAuthorized = true;
                break;
            } 
        }
        // for test only
        isAuthorized = true;

        if(!isAuthorized) 
        {
            NDN_LOG_ERROR("OnRequestDecryptionSuccessCallback: Permission Denied");
            return;
        }else{
            // log request is authorized
            NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: Permission Granted to" << requesterIdentity.toUri() << " for " << ServiceName.toUri() << " function " << FunctionName.toUri());
        }

        // In case of strategy is NoCoordination, we don't need to wait for Service Coordination Message
        if(requestMessage.getStrategy() == tlv::NoCoordination)
        {
            // log request is not waiting for Service Coordination Message
            NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: NoCoordination Strategy");
            ConsumeRequest(requesterIdentity, identity,ServiceName, FunctionName, RequestID, requestMessage);
            return;
        }

        // add requestMessage to pendingRequests
        pendingRequests[ndn::Name(requesterIdentity.toUri()).append(ServiceName).append(FunctionName).append(RequestID)] = std::make_shared<RequestMessage>(requestMessage);

        // publish Permission Ack Message and wait for Service Coordination Message
        std::string msg = "Permission Granted";
        PublishRequestAckMessage(requesterIdentity, ServiceName, FunctionName, RequestID, true, msg);
       
    }

    void ServiceProvider::OnRequestDecryptionErrorCallback(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const std::string &)
    {
        // log error
        NDN_LOG_ERROR("OnRequestDecryptionErrorCallback: " << requesterIdentity.toUri() << ServiceName.toUri() << FunctionName.toUri() << RequestID.toUri());
    }

void ServiceProvider::processNDNSDServiceInfoCallback(const ndnsd::discovery::Reply & callback)
{
        NDN_LOG_INFO("Service publish callback received");
        auto status = (callback.status == ndnsd::discovery::ACTIVE)? "ACTIVE": "EXPIRED";
        NDN_LOG_INFO("Status: " << status);
        for (auto& item : callback.serviceDetails)
        {
            NDN_LOG_INFO("Callback: " << item.first << ":" << item.second);
        }
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

    void ServiceProvider::onServiceCoordinationMessage(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        // log message
        NDN_LOG_INFO("Received Service Coordination Message: " << subscription.name.toUri());
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


    void ServiceProvider::PublishMessage(const ndn::Name &messageName, const ndn::Name &messageNameWithoutPrefix, AbstractMessage &message)
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
        // serve data
        serveDataWithIMS(contentData, ckData);
        auto buffer = mergeDataContents(contentData);
        ndn::Block contentBlock(buffer);
        // publish message name using ndn-svs
        m_svsps->publish(messageName, contentBlock);
    }
    void ServiceProvider::OnServiceCoordinationMessageDecryptionSuccessCallback(const ndn::Name &requesterName, const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID, const ndn::Buffer &buffer)
    {
        // log message
        NDN_LOG_INFO("OnServiceCoordinationMessageDecryptionSuccessCallback: " << requesterName.toUri() << providerName.toUri() << ServiceName.toUri() << FunctionName.toUri() << msgID.toUri());
        // parse ServiceCoordinationMessage

        ServiceCoordinationMessage message;
        message.WireDecode(ndn::Block(buffer));
        for(auto requestID:message.getRequestIDs()){
            // find the corresponding request message in the pendingRequests
            auto it = pendingRequests.find(ndn::Name(requesterName.toUri()).append(ServiceName).append(FunctionName).append(msgID));
            if (it != pendingRequests.end())
            {
                // consume the request
                ConsumeRequest(requesterName, providerName, ServiceName, FunctionName, ndn::Name(requestID), *(it->second));
            }
            pendingRequests.erase(ndn::Name(requestID));
        }

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
                                        false);
            // register Service Coordination Message
            std::string regex_str2 = "^(<>*)<NDNSF><COORDINATION>" + ndn_service_framework::NameToRegexString(identity);
            NDN_LOG_INFO(regex_str2);
            m_svsps->subscribeWithRegex(ndn::Regex(regex_str2),
                                        std::bind(&ServiceProvider::onServiceCoordinationMessage, this, _1),
                                        false);
        }
    }
    void ServiceProvider::registerServiceInfo()
    {
        // Serve ServiceInfo using NDNSD;
        std::map<char, uint8_t> flagsMap = {
            {'p', ndnsd::SYNC_PROTOCOL_CHRONOSYNC}, // Protocol choice
            {'t', ndnsd::discovery::PRODUCER}  // Type consumer: 0
        };
        for(auto serviceName:m_serviceNames){ 
            auto serviceDiscovery = std::make_shared<ndnsd::discovery::ServiceDiscovery>((serviceName+".info"), flagsMap, std::bind(&ServiceProvider::processNDNSDServiceInfoCallback, this, std::placeholders::_1));
            UpdateUPTWithServiceMetaInfo(serviceDiscovery);
            multiServiceDiscovery.addServiceDiscovery(serviceDiscovery);
        }
        multiServiceDiscovery.startAll();
    }
}