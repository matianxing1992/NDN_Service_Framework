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
        ServiceController(ndn::Face& face,ndn::security::Certificate aaCert, const std::string &configFolderPath) 
            :   m_face(face),
                m_aaCert(aaCert),
                m_aa(m_aaCert, m_face, m_validator, m_keyChain),
                m_configFolderPath(configFolderPath)
        {
            loadConfigFiles();
            AddAttributesForUsersAccordingToServicePolicy();
        }

    private:
        void loadConfigFiles()
        {
            // 遍历配置文件夹中的所有文件
            for (const auto &entry : fs::directory_iterator(m_configFolderPath))
            {
                if (entry.is_regular_file())
                {
                    std::string filePath = entry.path().string();
                    std::cout << "Loading config file: " << filePath << std::endl;

                    // 使用 PolicyParser 加载配置文件
                    PolicyParser parser;
                    auto policies = parser.parseServicePolicies(filePath);

                    // 将加载的策略存储到成员变量中
                    m_servicePolicies.insert(m_servicePolicies.end(), policies.begin(), policies.end());
                }
            }
        }

        void AddAttributesForUsersAccordingToServicePolicy()
        {
            for(auto policy : m_servicePolicies)
            {
                // 遍历策略中的用户}
                ndn::Name serviceFullName(policy.forValue);
                addAttribute(serviceFullName.getPrefix(-2).toUri(), "/ID"+serviceFullName.getPrefix(-2).toUri());
                addAttribute(serviceFullName.getPrefix(-2).toUri(), "/SERVICE"+serviceFullName.getSubName(-2,2).toUri());
                for(auto allowedUser: policy.allowedUsers)
                {
                    addAttribute(allowedUser, "/ID"+allowedUser);
                    addAttribute(allowedUser, "/PERMISSION"+serviceFullName.toUri());
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
                m_aa.addNewPolicy(ndn::Name(item.first), attributeList);
                // log item.first and attributeList
                std::cout << "Add attributes for identity: " << item.first << std::endl;
                for(auto attribute : attributeList)
                {
                    std::cout << attribute << std::endl;
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
            // add attribute to attributesMap
            attributesMap[identity].emplace(attributeName);
        }

    private:
        std::string m_configFolderPath;
        std::vector<ndn_service_framework::ServicePolicy> m_servicePolicies;
        ndn::Face& m_face;
        ndn::KeyChain m_keyChain;
        ndn::ValidatorConfig m_validator{m_face};
        ndn::security::Certificate m_aaCert;
        ndn::nacabe::CpAttributeAuthority m_aa;
        // identity -> attributes
        std::map<std::string, std::set<std::string>> attributesMap;
    };

}

#endif