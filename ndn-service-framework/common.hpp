#ifndef NDN_SERVICE_FRAMEWORK_COMMON_HPP
#define NDN_SERVICE_FRAMEWORK_COMMON_HPP

#define BOOST_BIND_NO_PLACEHOLDERS

#include <iostream>
#include <thread>
#include <stdexcept>
#include <ndn-cxx/face.hpp>
#include <ndn-svs/svspubsub.hpp>
#include <ndn-svs/security-options.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <ndn-cxx/security/validation-policy-signed-interest.hpp>
#include "ndn-cxx/security/certificate-fetcher-from-network.hpp"
#include "ndn-cxx/security/interest-signer.hpp"
#include <unordered_map>
#include <ndn-cxx/data.hpp>
#include <ndn-cxx/interest.hpp>
#include "ndn-cxx/util/logger.hpp"
#include <ndn-cxx/util/regex.hpp>
#include "ndn-cxx/util/time.hpp"
#include "ndn-cxx/util/random.hpp"
#include "ndn-cxx/util/scheduler.hpp"

#include <ndn-cxx/encoding/block-helpers.hpp>

#include <ndn-cxx/util/segment-fetcher.hpp>
#include <ndn-cxx/util/segmenter.hpp>
#include "ndn-cxx/ims/in-memory-storage-fifo.hpp"

#include <nac-abe/consumer.hpp>
#include <nac-abe/producer.hpp>
#include <nac-abe/cache-producer.hpp>

#include <ndnsd/discovery/service-discovery.hpp>

#include <mutex>
#include <boost/bimap/bimap.hpp>
#include <boost/bimap/unordered_set_of.hpp>
#include <boost/bimap.hpp>
#include <boost/bimap/set_of.hpp>
#include <boost/bimap/multiset_of.hpp>






namespace ndn_service_framework{



    class MessageValidator : public ndn::svs::BaseValidator
    {
    public:
        MessageValidator(std::string trustSchemaPath)
        {
            m_validator.load(trustSchemaPath);
        }

        /**
         * @brief Asynchronously validate @p data
         *
         * @note @p successCb and @p failureCb must not be nullptr
         */
        void
        validate(const ndn::Data &data,
                 const ndn::security::DataValidationSuccessCallback &successCb,
                 const ndn::security::DataValidationFailureCallback &failureCb) override
        {
            m_validator.validate(
                data, 
                [&](const ndn::Data &)
                { 
                    successCb(data); 
                },
                [&](const ndn::Data& data, const ndn::security::ValidationError& error) 
                {
                    //failureCb(data,error);
                });
        }

        /**
         * @brief Asynchronously validate @p interest
         *
         * @note @p successCb and @p failureCb must not be nullptr
         */
        void
        validate(const ndn::Interest &interest,
                 const ndn::security::InterestValidationSuccessCallback &successCb,
                 const ndn::security::InterestValidationFailureCallback &failureCb) override
        {
            // successCb(interest); 
            // std::cout<<interest.getSignatureInfo()->getTime()->time_since_epoch().count()<<std::endl;
            // std::cout<< interest.getSignatureInfo()->getKeyLocator().getName().toUri() <<std::endl;
            m_validator.validate(
                interest, 
                [&](const ndn::Interest& interest)
                { 
                    successCb(interest); 
                },
                [&](const ndn::Interest& interest, const ndn::security::ValidationError& error) 
                {
                    //failureCb(interest,error);
                });

        }

    private:
        ndn::Face m_face;
        ndn::ValidatorConfig m_validator{m_face};
    };

    /**
     * A signer using an ndn-cxx keychain instance
     */
    class CommandInterestSigner : public ndn::svs::BaseSigner
    {
    public:
        explicit CommandInterestSigner(ndn::security::KeyChain &keyChain)
            : m_keyChain(keyChain),
              _signer(m_keyChain)
        {
        }

        void
        sign(ndn::Interest &interest) const override
        {
            ndn::security::InterestSigner signer(_signer);
            signer.makeSignedInterest(interest, signingInfo,
                            ndn::security::InterestSigner::SigningFlags::WantNonce |
                            ndn::security::InterestSigner::SigningFlags::WantTime);
        }

        void
        sign(ndn::Data &data) const override
        {
            m_keyChain.sign(data, signingInfo);
        }

    private:
        ndn::security::KeyChain &m_keyChain;
        ndn::security::InterestSigner _signer;
    };
}

#endif