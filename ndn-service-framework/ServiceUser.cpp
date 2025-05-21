#include "ServiceUser.hpp"

namespace ndn_service_framework
{

    NDN_LOG_INIT(ndn_service_framework.ServiceUser);

    ServiceUser::ServiceUser(ndn::Face &face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath) : 
        m_face(face),
        m_scheduler(m_face.getIoContext()),
        identity(identityCert.getIdentity()),
        identityCert(identityCert),
        validator(std::make_shared<MessageValidator>(trustSchemaPath)),
        // nac_validator(std::move(ndn::security::ValidatorNull())),
        nacConsumer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        nacProducer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        m_IMS(6000),
        m_ServiceDiscovery(group_prefix, identity, face, m_keyChain, std::bind(&ServiceUser::processNDNSDServiceInfoCallback, this, _1))
    {
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
        // opts.useTimestamp = true;
        // opts.maxPubAge = ndn::time::seconds(10);

        // Create the Pub/Sub instance
        m_svsps = std::make_shared<ndn::svs::SVSPubSub>(
            ndn::Name(group_prefix),
            ndn::Name(identity),
            m_face,
            std::bind(&ServiceUser::onMissingData, this, _1),
            opts,
            secOpts);

        while(!nacConsumer.readyForDecryption()){
            // log waiting for decryption key
            NDN_LOG_INFO("Waiting for decryption key");
            face.processEvents(ndn::time::milliseconds(1000));
        }



    }

    void ServiceUser::init()
    {   
        registerNDNSFMessages();
    }

    ndn::Name ServiceUser::getName()
    {
        return identity;
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
            tokens[pair.first] = std::to_string(std::hash<std::string>()(pair.second + RequestID.toUri()));
        }
        requestMessage.setTokens(tokens);
        requestMessage.setPayload(const_cast<ndn::Buffer&>(payload),payload.size());
        requestMessage.setStrategy(strategy);
        requestMessage.WireEncode().data();
        
        ndn::Name requestName = ndn_service_framework::makeRequestName(identity, ServiceName, FunctionName, ndn::Name(bloomFilter.toHexString()), RequestID);
        ndn::Name requestNameWithoutPrefix = ndn_service_framework::makeRequestNameWithoutPrefix(ServiceName, FunctionName, ndn::Name(bloomFilter.toHexString()),RequestID);
    
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

    void ServiceUser::OnRequestAck(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        // log message
        NDN_LOG_INFO("OnRequestAck: " << subscription.name);
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

    void ServiceUser::OnRequestAckDecryptionSuccessCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID, const ndn::Buffer &buffer)
    {
        // log message
        NDN_LOG_INFO("OnRequestAckDecryptionSuccessCallback: " << providerName.toUri() << ServiceName.toUri() << FunctionName.toUri() << requestID.toUri());
        
        // parse Permission Ack Message from buffer
        RequestAckMessage AckMessage;
        AckMessage.WireDecode(ndn::Block(buffer));

        // check if permission is granted
        if(!AckMessage.getStatus()){
            NDN_LOG_ERROR("Permission Denied by Provider: " << providerName.toUri() << ServiceName.toUri() << FunctionName.toUri() << requestID.toUri() << " with message: " << AckMessage.getMessage());
            return;
        }


        // get strategy from m_strategyMap
        auto strategy = m_strategyMap.find(requestID);
        if(strategy == m_strategyMap.end()){
            NDN_LOG_ERROR("Strategy not found for requestID: " << requestID.toUri());
            return;
        }
        // log strategy
        NDN_LOG_INFO("Strategy: " << strategy->second);

        // make decision based on strategy
        if(strategy->second == tlv::FirstResponding){
            PublishServiceCoordinationMessage(providerName, ServiceName, FunctionName, requestID);
            m_strategyMap.erase(strategy);

        }else if(strategy->second == tlv::LoadBalancing){
            // check requestID exists in m_AckInfoMap
            if (m_AckInfoMap.find(requestID) == m_AckInfoMap.end()){
                NDN_LOG_ERROR("AckInfo vector not found for RequestID: " << requestID.toUri());
                return;
            }
            // insert AckInfo into m_AckInfoMap
            m_AckInfoMap[requestID].push_back({providerName, ServiceName, FunctionName, requestID});
            // log AckINfo added to m_AckInfoMap
            NDN_LOG_INFO("AckInfo added to m_AckInfoMap for RequestID: " << providerName << " " << requestID);

        }else if(strategy->second == tlv::NoCoordination){
            // publish service coordination message
            PublishServiceCoordinationMessage(providerName, ServiceName, FunctionName, requestID);
        }else{
            NDN_LOG_ERROR("Invalid strategy: " << strategy->second);
            return;
        }

    }

    void ServiceUser::OnRequestAckDecryptionErrorCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID, const std::string &error)
    {
        // log error
        NDN_LOG_ERROR("OnRequestAckDecryptionErrorCallback: " << providerName.toUri() << ServiceName.toUri() << FunctionName.toUri() << requestID.toUri() << " with error: " << error);
    }

    void ServiceUser::PublishServiceCoordinationMessage(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID)
    {
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

    void ServiceUser::onMissingData(const std::vector<ndn::svs::MissingDataInfo> &)
    {
        NDN_LOG_INFO("onMissingData");
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
        // log interest
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
        // serve data
        serveDataWithIMS(contentData, ckData);
        NDN_LOG_INFO("Merge Data Contents");
        auto buffer = mergeDataContents(contentData);
        
        ndn::Block contentBlock(buffer);
        // publish message name using ndn-svs
        NDN_LOG_INFO("publish message name using ndn-svs: " << messageName.toUri() << " size: " << contentBlock.size());
        m_svsps->publish(messageName, contentBlock);
        NDN_LOG_INFO("Message Published: " << messageName.toUri());
    }
    void ServiceUser::registerNDNSFMessages()
    {

        // log register
        NDN_LOG_INFO("Register NDNSF Messages in ndn-svs");

        // register Permission Ack Message
        std::string regex_str = "^(<>*)<NDNSF><ACK>" + ndn_service_framework::NameToRegexString(identity) ;
        NDN_LOG_INFO(regex_str);
        m_svsps->subscribeWithRegex(ndn::Regex(regex_str),
                                    std::bind(&ServiceUser::OnRequestAck, this, _1),
                                    false, false);
        // register Response Message
        std::string regex_str2 = "^(<>*)<NDNSF><RESPONSE>" + ndn_service_framework::NameToRegexString(identity);
        NDN_LOG_INFO(regex_str2);
        m_svsps->subscribeWithRegex(ndn::Regex(regex_str2),
                                    std::bind(&ServiceUser::OnResponse, this, _1),
                                    false, false);
        
    }
    void ServiceUser::requestForServiceInfo()
    {
        NDN_LOG_DEBUG("Requesting Service Info");
    }

    void ServiceUser::OnResponseDecryptionErrorCallback(const ndn::Name& serviceProviderName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const std::string & msg)
    {
        NDN_LOG_ERROR("OnResponseDecryptionErrorCallback: " << serviceProviderName << ServiceName << FunctionName << RequestID << " with error: " << msg);
    }
}