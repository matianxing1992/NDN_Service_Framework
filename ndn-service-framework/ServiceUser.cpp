#include "ServiceUser.hpp"

namespace ndn_service_framework
{

    NDN_LOG_INIT(ndn_service_framework.ServiceUser);

    ServiceUser::ServiceUser(ndn::Face &face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath) : 
        m_face(face),
        identity(identityCert.getIdentity()),
        identityCert(identityCert),
        validator(std::make_shared<MessageValidator>(trustSchemaPath)),
        // nac_validator(std::move(ndn::security::ValidatorNull())),
        requestConsumer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        responseProduer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        m_IMS(6000)
    {
        nac_validator.load(trustSchemaPath);

        requestConsumer.obtainDecryptionKey();

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

        std::string regex_str = "^(<>*)<NDNSF><PERMISSION><CHALLENGE>" + ndn_service_framework::NameToRegexString(identity) + "(<>)(<>)(<>)";
        NDN_LOG_INFO(regex_str);
        m_svsps->subscribeWithRegex(ndn::Regex(regex_str),
                                    std::bind(&ServiceUser::OnPermissionChallenge, this, _1),
                                    false);

        // self certificate filter
        // m_face.setInterestFilter(identityCert.getKeyName(),
        //                          [this](auto &...)
        //                          {
        //                              m_face.put(this->identityCert);
        //                          });
        // self filter for IMS

    }

    void ServiceUser::OnPermissionChallenge(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        ndn::Name ServiceProviderName, RequesterName, ServiceName, FunctionName, ChallengeID;
        std::tie(ServiceProviderName, RequesterName, ServiceName, FunctionName, ChallengeID) =
            ndn_service_framework::parsePermissionChallengeName(subscription.name);
        requestConsumer.consume(
            subscription.name,
            [=](const ndn::Buffer &buffer)
            {
                //buffer.data();
                //std::string token = (char*)buffer.data();
                //NDN_LOG_INFO("Token: "<<token);
                ndn::Name permissionResponseName =
                    ndn_service_framework::makePermissionResponseName(RequesterName, ServiceProviderName, ServiceName, FunctionName, ChallengeID);
                m_svsps->publish(permissionResponseName, buffer);
            },
            [](const std::string &error)
            {
                NDN_LOG_ERROR(error);
            });
    }

    void ServiceUser::onMissingData(const std::vector<ndn::svs::MissingDataInfo> &)
    {
        NDN_LOG_INFO("onMissingData");
    }

    bool ServiceUser::replyFromIMS(const ndn::Interest &interest)
    {
        std::lock_guard<std::mutex> lock(_cache_mutex);
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
}