#ifndef NDN_SERVICE_FRAMEWORK_POLICY_PARSER_HPP
#define NDN_SERVICE_FRAMEWORK_POLICY_PARSER_HPP

#include <iostream>
#include <fstream>
#include <stdexcept>
#include <list>
#include "common.hpp"

namespace ndn_service_framework {

namespace tlv {
    enum {
        ServicePolicyType = 170,
        ForType = 171,
        AllowedUsersType = 173,
        DeniedUsersType = 174,
    };
}

namespace pt = boost::property_tree;

struct ServicePolicy {
    std::string forValue;
    std::list<std::string> allowedUsers;
    std::list<std::string> deniedUsers;

    // Serialize to ndn::Block
    ndn::Block WireEncode() const;

    // Deserialize from ndn::Block
    bool WireDecode(const ndn::Block& block);
};

class PolicyParser {
public:
    PolicyParser() {}

    std::list<ServicePolicy> parseServicePolicies(const std::string& policyFilePath);

    void writeServicePolicies(const std::string& policyFilePath, const std::list<ServicePolicy>& servicePolicies);

};

}

#endif // NDN_SERVICE_FRAMEWORK_POLICY_PARSER_HPP
