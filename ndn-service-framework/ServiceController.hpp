#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_CONTROLLER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_CONTROLLER_HPP

#include "common.hpp"
#include "utils.hpp"
#include <iostream>
#include <filesystem>
#include <vector>
#include "PolicyParser.hpp"
#include <nac-abe/attribute-authority.hpp>

namespace ndn_service_framework
{

namespace fs = std::filesystem;

/*
 * For the time being, only allow sections in the configuration files work;
 * In the allow section, the items should be names instead of regexes;
 */
class ServiceController
{
public:
    ServiceController(ndn::Face& face, ndn::security::Certificate aaCert, ndn::ValidatorConfig& m_validator, const std::string& configFilePath)
        : m_face(face)
        , m_aaCert(aaCert)
        , m_aa(m_aaCert, m_face, m_validator, m_keyChain)
        , m_configFilePath(configFilePath)
    {
        loadConfigFiles();
        addAttributesForUsersAccordingToServicePolicy();
    }

private:
    void loadConfigFiles()
    {
        if (fs::is_regular_file(m_configFilePath)) {
            std::cout << "Loading config file: " << m_configFilePath << std::endl;

            PolicyParser parser;
            auto policies = parser.parsePolicyFile(m_configFilePath);

            m_providerPolicies.insert(m_providerPolicies.end(), policies.first.begin(), policies.first.end());
            m_userPolicies.insert(m_userPolicies.end(), policies.second.begin(), policies.second.end());
        } else {
            std::cerr << "Error: " << m_configFilePath << " is not a valid file." << std::endl;
        }
    }

    void addAttributesForUsersAccordingToServicePolicy()
    {
        for (const auto& policy : m_providerPolicies) {
            for (const auto& service : policy.allowedServices) {
                addAttribute(policy.providerName, "/SERVICE" + service);
            }
            addAttribute(policy.providerName, "/ID" + policy.providerName);
        }

        for (const auto& policy : m_userPolicies) {
            ndn::Name userName(policy.userName);
            addAttribute(userName.toUri(), "/ID" + userName.toUri());

            for (const auto& service : policy.allowedServices) {
                addAttribute(userName.toUri(), "/PERMISSION" + service);
            }
        }

        for (const auto& item : m_attributesMap) {
            std::list<std::string> attributeList(item.second.begin(), item.second.end());

            std::string policy = boost::algorithm::join(attributeList, " OR ");
            try {
                auto cert = m_keyChain.getPib().getIdentity(item.first).getDefaultKey().getDefaultCertificate();
                m_aa.addNewPolicy(cert, policy);
                std::cout << "Add policy: " << policy << " for identity: " << item.first << std::endl;
            } catch (const std::exception& e) {
                m_aa.addNewPolicy(item.first, policy);
                std::cout << "Add policy (fallback): " << policy << " for identity: " << item.first << std::endl;
            }
        }
    }

    void addAttribute(const std::string& identity, const std::string& attributeName)
    {
        m_attributesMap[identity].emplace(attributeName);
    }

    void run()
    {
        m_face.processEvents();
    }

private:
    std::string m_configFilePath;
    std::vector<ndn_service_framework::ProviderPolicy> m_providerPolicies;
    std::vector<ndn_service_framework::UserPolicy> m_userPolicies;

    ndn::Face& m_face;
    ndn::KeyChain m_keyChain;
    ndn::security::Certificate m_aaCert;
    ndn::nacabe::KpAttributeAuthority m_aa;

    std::map<std::string, std::set<std::string>> m_attributesMap;
};

} // namespace ndn_service_framework

#endif
