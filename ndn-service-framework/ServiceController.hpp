#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_Controller_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_Controller_HPP

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
        ServiceController(ndn::Face& face,ndn::security::Certificate aaCert, ndn::ValidatorConfig& m_validator, const std::string &configFilePath) 
            :   m_face(face),
                m_aaCert(aaCert),
                m_aa(m_aaCert, m_face, m_validator, m_keyChain),
                m_configFilePath(configFilePath)
        {
            loadConfigFiles();
            AddAttributesForUsersAccordingToServicePolicy();
        }

    private:
        void loadConfigFiles()
        {
            // Check if the specified path is a regular file
            if (fs::is_regular_file(m_configFilePath))
            {
                std::cout << "Loading config file: " << m_configFilePath << std::endl;
        
                // Use PolicyParser to load the configuration file
                PolicyParser parser;
                auto policies = parser.parsePolicyFile(m_configFilePath);
        
                // Store the loaded policies into member variables
                m_ProviderPolicies.insert(m_ProviderPolicies.end(), policies.first.begin(), policies.first.end());
                m_UserPolicies.insert(m_UserPolicies.end(), policies.second.begin(), policies.second.end());
            }
            else
            {
                std::cerr << "Error: " << m_configFilePath << " is not a valid file." << std::endl;
            }
        }

        void AddAttributesForUsersAccordingToServicePolicy()
        {
            for(auto policy : m_ProviderPolicies){
                // 遍历策略中的服务
                for (auto provider : policy.allowedProviders){
                    addAttribute(provider, "/ID"+provider);
                    addAttribute(provider, "/SERVICE"+policy.serviceName);
                }
            }
            for(auto policy : m_UserPolicies)
            {
                // 遍历策略中的用户
                ndn::Name userName(policy.userName);
                addAttribute(userName.toUri(), "/ID"+userName.toUri());
                for(auto allowedService: policy.allowedServices)
                {
                    addAttribute(userName.toUri(), "/PERMISSION"+allowedService);
                }
            }
            for(auto item : attributesMap)
            {
                // item.second to List<std::string>
                std::set<std::string> attributeSet = item.second;
                std::list<std::string> attributeList;

                // 将 set<string> 中的元素复制到 vector<string> 中
                std::copy(item.second.begin(), item.second.end(), std::back_inserter(attributeList));
                // add attributes to attribute authority
                try{
                    auto cert = m_keyChain.getPib().getIdentity(item.first).getDefaultKey().getDefaultCertificate();
                    //m_aa.addNewPolicy(cert, attributeList);
                    // merge attributesList using " or "
                    std::string policy = boost::algorithm::join(attributeList, " OR ");
                    m_aa.addNewPolicy(cert, policy);
                    std::cout << "Add policy: " << policy << " For " << item.first << std::endl;
                }catch(const std::exception& e){
                    std::string policy = boost::algorithm::join(attributeList, " OR ");
                    m_aa.addNewPolicy(item.first, policy);
                    std::cout << "Add policy: " << policy << " For " << item.first << std::endl;
                }

            }
        
        }

        void run()
        {
            m_face.processEvents();
        }

        void addAttribute(std::string identity, std::string attributeName)
        {
            // check if identity exists in attributesMap
            if(attributesMap.find(identity) == attributesMap.end())
            {
                attributesMap[identity] = std::set<std::string>();
            }
            //check if attributeName exists in attributesMap[identity];
            if(attributesMap[identity].find(attributeName) != attributesMap[identity].end())
            {
                return;
            }
            // add attribute to attributesMap
            attributesMap[identity].emplace(attributeName);
        }

    private:
        std::string m_configFilePath;
        std::vector<ndn_service_framework::ProviderPolicy> m_ProviderPolicies;
        std::vector<ndn_service_framework::UserPolicy> m_UserPolicies;
        ndn::Face& m_face;
        ndn::KeyChain m_keyChain;
        ndn::security::Certificate m_aaCert;
        ndn::nacabe::KpAttributeAuthority m_aa;
        // identity -> attributes
        std::map<std::string, std::set<std::string>> attributesMap;
    };

}

#endif