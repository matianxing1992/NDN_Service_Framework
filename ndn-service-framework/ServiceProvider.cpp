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
        requestConsumer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        responseProduer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        random(ndn::random::getRandomNumberEngine()),
        m_IMS(6000)
    {
        nac_validator.load(trustSchemaPath);

        requestConsumer.obtainDecryptionKey();

        // self filter for IMS
        m_face.setInterestFilter(identity,
            [&](const ndn::InterestFilter &, const ndn::Interest &interest)
            {
                NDN_LOG_INFO(interest.getName());
                replyFromIMS(interest);
            },
            [&](const ndn::Name&, const std::string&)
            {

            });
        //std::thread ims ([&](){m_IMSFace.processEvents();});
        //m_face.processEvents(ndn::time::milliseconds(1000));

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

        // m_svsps->subscribeWithRegex()
        std::string regex_str = "^(<>*)<NDNSF><PERMISSION><RESPONSE>" + ndn_service_framework::NameToRegexString(identity) + "(<>)(<>)(<>)";
        NDN_LOG_INFO(regex_str);

        m_svsps->subscribeWithRegex(ndn::Regex(regex_str),
                                    std::bind(&ServiceProvider::OnPermissionChallengeResponse, this, _1),
                                    true);

        // self certificate filter
        // m_face.setInterestFilter(identityCert.getKeyName(),
        //                          [&](auto &...)
        //                          {
        //                              m_face.put(this->identityCert);
        //                          });


    }

    void ServiceProvider::onMissingData(const std::vector<ndn::svs::MissingDataInfo> &)
    {
        NDN_LOG_INFO("onMissingData");

    }

    void ServiceProvider::PermissionCheck(const ndn::Name &requesterIdentity, const ndn::Name &ServiceProviderName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, PermissionCheckCallback AfterPermissionCheck)
    {

        ndn::Name requestPrefix = ndn_service_framework::makeRequestPrefixName(requesterIdentity, ServiceProviderName, ServiceName, FunctionName);
        
        if (authorizedRequestPrefixSet.find(requestPrefix) != authorizedRequestPrefixSet.end())
        {
            NDN_LOG_INFO("Permission Check Passed: " << requestPrefix);
            AfterPermissionCheck(true);
            return;
        }

        // add request to unAuthorizedRequestMap, and later it will be processed on Permission Response
        ndn::Name requestName = ndn_service_framework::makeRequestName(requesterIdentity, ServiceProviderName, ServiceName, FunctionName, RequestID);
        ndn::Name requestNameWithoutPrefix = ndn_service_framework::makeRequestNameWithoutPrefix(ServiceProviderName, ServiceName, FunctionName, RequestID);

        // Permission Challenge : ^(<>+)<NDNSF><PERMISSION><CHALLENGE>(<>+)(<>)(<>)(<>)$
        // For nac-abe api, it doesn't include producer's name
        ndn::Name chanllengeID (ndn::time::toIsoString(ndn::time::system_clock::now()));
        ndn::Name permissionChallengeName=
            ndn_service_framework::makePermissionChallengeName(identity,requesterIdentity,ServiceName,FunctionName,chanllengeID);
        ndn::Name permissionChallengeNameWithoutPrefix = ndn_service_framework::makePermissionChallengeNameWithoutPrefix(requesterIdentity,ServiceName,FunctionName,chanllengeID);
        
        // uint8_t tokenBuf[32];
        // ndn::random::generateSecureBytes(tokenBuf);

        // std::string token = (char*)tokenBuf;

        std::string token = ndn_service_framework::RandomString(32);
        std::vector<uint8_t> tokenBuffer(token.begin(), token.end());
        
        
        // record challenge and unauthorized requests
        chanllengeRecords.insert({chanllengeID,{token,requestPrefix}});
        NDN_LOG_TRACE("PermissionCheck: "<<chanllengeID<<" "<<token<<" "<<requestPrefix);
        unauthorizedRequestMap.insert({requestName,requestPrefix});

        ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
        ndn::Name servicePermissionName("/PERMISSION");
        //servicePermissionName.append("/PERMISSION").append(ServiceName).append(FunctionName);
        servicePermissionName.append(ServiceProviderName).append(ServiceName).append(FunctionName);
        const std::vector<std::string> attributes = {servicePermissionName.toUri()};
        //const std::vector<std::string> attributes ={"attribute"};
        
        std::tie(contentData, ckData) =
            responseProduer.produce(permissionChallengeNameWithoutPrefix, attributes, ndn::span<uint8_t>{tokenBuffer}, m_signingInfo);
        // serve data
        for (auto data : contentData) m_IMS.insert(*data);
        for(auto data:ckData) m_IMS.insert(*data);

        m_svsps->publish(permissionChallengeName);
        NDN_LOG_INFO("Publish Permission Challenge" << contentData.at(0)->getName());
    }

    void ServiceProvider::OnPermissionChallengeResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        // Permission Challenge : ^(<>+)<NDNSF><PERMISSION><RESPONSE>(<>+)(<>)(<>)(<>)$
        ndn::Name RequesterName, ServiceProviderName, ServiceName, FunctionName, ChallengeId;
        std::tie(RequesterName, ServiceProviderName, ServiceName, FunctionName, ChallengeId) =
            ndn_service_framework::parsePermissionResponseName(subscription.name);
        NDN_LOG_INFO("OnPermissionChallengeResponse: " << ServiceProviderName << RequesterName << ServiceName << FunctionName << ChallengeId);
        ndn::Name requestPrefix = ndn_service_framework::makeRequestPrefixName(RequesterName, ServiceProviderName, ServiceName, FunctionName);
        
        
        std::string token(subscription.data.data(),subscription.data.data()+subscription.data.size());

        NDN_LOG_INFO("OnPermissionChallengeResponse: Search in ChanllengeRecords token: "<<token);
        auto it = chanllengeRecords.find(ChallengeId);
        if (it != chanllengeRecords.end())
        {
            
            if (it->second.first == token)
            {
                NDN_LOG_INFO("OnPermissionChallengeResponse: Token matches "<<unauthorizedRequestMap.size());
                authorizedRequestPrefixSet.emplace(requestPrefix);
                // search unauthorizedRequestMap;
                for (auto pair = unauthorizedRequestMap.cbegin(); pair != unauthorizedRequestMap.cend();)
                {
                    // if permission satisfied in the unauthorizedRequestMap
                    NDN_LOG_INFO(requestPrefix<<"->"<<pair->first<<" -"<<pair->second);
                    if (requestPrefix.isPrefixOf(pair->first))
                    {
                        NDN_LOG_INFO("OnPermissionChallengeResponse: Found pending request ");
                        // ConsumeRequest()
                        ndn::Name RequesterName, ServiceProviderIdentity, ServiceName, FunctionName, RequestId;
                        std::tie(RequesterName, ServiceProviderIdentity, ServiceName, FunctionName, RequestId) =
                            ndn_service_framework::parseRequestName(pair->first);
                        ConsumeRequest(RequesterName, ServiceProviderIdentity,  ServiceName, FunctionName, RequestId);
                        unauthorizedRequestMap.erase(pair++);
                    }
                    else
                    {
                        ++pair;
                    }
                }
            }
            else
            {
                it++;
            }
        }

        // permissionTokenMap
    }

    void ServiceProvider::PublishResponse(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &buffer)
    {
        //  /<identity>/NDNSF/RESPONSE/<requesterIdentity>/<ServiceName>/<FunctionName>/<request-id>
        // identity will be appended by NAC-ABE
        
        ndn::Name responseName = ndn_service_framework::makeResponseName(identity, requesterIdentity, ServiceName, FunctionName, RequestID);
        ndn::Name responseNameWithoutPrefix = ndn_service_framework::makeResponseNameWithoutPrefix(requesterIdentity, ServiceName, FunctionName, RequestID);
        NDN_LOG_INFO("PublishResponse:"<<responseName);
        // Encrypt the response with nac-abe
        // publish the encrypted response with ndn-svs, and insert ck into the repo
        //  contentData segments, and ckData segments
        ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
        const std::vector<std::string> attributes = {"/ID"+requesterIdentity.toUri()};
        std::tie(contentData, ckData) =
            responseProduer.produce(responseNameWithoutPrefix, attributes, ndn::make_span(reinterpret_cast<const uint8_t *>(buffer.data()), buffer.size()), m_signingInfo);
        // serve data
        for (auto data : contentData){
            //NDN_LOG_INFO(data->getName());
            m_IMS.insert(*data);
        }
        for (auto data : ckData)
            m_IMS.insert(*data);
        // content
        // ndn::BufferPtr contentBuffer = ndn_service_framework::CombineSegmentsIntoBuffer(contentData);
        // ndn::BufferPtr ckBuffer = ndn_service_framework::CombineSegmentsIntoBuffer(ckData);

        m_svsps->publish(responseName);
        //m_svsps->publish(responseName, ndn::make_span(reinterpret_cast<const uint8_t *>(contentBuffer->data()), contentBuffer->size()));
        NDN_LOG_INFO("Publish Encrypted response" << contentData.at(0)->getName().getPrefix(-1));
        //m_svsps->publish(ckData.at(0)->getName().getPrefix(-1));
        //m_svsps->publish(ckData.at(0)->getName().getPrefix(-1), ndn::make_span(reinterpret_cast<const uint8_t *>(ckBuffer->data()), ckBuffer->size()));
        //NDN_LOG_INFO("ServiceProvider_Drone::PublishResponse CK" << ckData.at(0)->getName().getPrefix(-1));
    }

    bool ServiceProvider::replyFromIMS(const ndn::Interest &interest)
    {
        std::lock_guard<std::mutex> lock(_cache_mutex);
        auto data = m_IMS.find(interest.getName());
        if (data != nullptr)
        {
            NDN_LOG_TRACE("Reply from IMS: " << interest.getName().toUri());
            m_face.put(*data);
        }else{
            NDN_LOG_TRACE("Not Found In IMS: " << interest.getName().toUri());
            for(auto d:m_IMS)
            {
                NDN_LOG_TRACE("In IMS: " << d.getName().toUri());
            }
        }
        return false;
    }

}