#ifndef NDN_SERVICE_FRAMEWORK_POLICY_PARSER_HPP
#define NDN_SERVICE_FRAMEWORK_POLICY_PARSER_HPP

#include <iostream>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <string>
#include <boost/property_tree/info_parser.hpp>

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

struct ProviderPolicy {
    std::string providerName; // 改为以 provider 为中心
    std::vector<std::string> allowedServices;
};

struct UserPolicy {
    std::string userName;
    std::vector<std::string> allowedServices;
};

class PolicyParser {
public:
    PolicyParser() = default;
    std::pair<std::vector<ProviderPolicy>, std::vector<UserPolicy>> parsePolicyFile(const std::string& filename);
};

}

#endif // NDN_SERVICE_FRAMEWORK_POLICY_PARSER_HPP
