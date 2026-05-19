#include "PolicyParser.hpp"

#include <ndn-cxx/util/logger.hpp>

namespace ndn_service_framework {

NDN_LOG_INIT(ndn_service_framework.PolicyParser);

std::pair<std::vector<ProviderPolicy>, std::vector<UserPolicy>> PolicyParser::parsePolicyFile(const std::string& filename) {
    std::ifstream file(filename);
    std::vector<ProviderPolicy> providerPolicies;
    std::vector<UserPolicy> userPolicies;

    if (!file.is_open()) {
        NDN_LOG_ERROR("Error opening file: " << filename);
        return { {}, {} };
    }

    pt::ptree pt;
    try {
        pt::read_info(file, pt);
    } catch (const pt::info_parser_error& e) {
        NDN_LOG_ERROR("Error parsing file: " << e.what());
        return { {}, {} };
    }

    // Parse provider policies (new structure)
    for (const auto& providerPolicyNode : pt.get_child("provider-policies")) {
        if (providerPolicyNode.first == "provider-policy") {
            ProviderPolicy providerPolicy;
            providerPolicy.providerName = providerPolicyNode.second.get<std::string>("for");

            for (const auto& serviceNode : providerPolicyNode.second.get_child("allow")) {
                providerPolicy.allowedServices.push_back(serviceNode.first.data());
            }

            providerPolicies.push_back(std::move(providerPolicy));
        }
    }

    // Parse user policies (unchanged)
    for (const auto& userPolicyNode : pt.get_child("user-policies")) {
        if (userPolicyNode.first == "user-policy") {
            UserPolicy userPolicy;
            userPolicy.userName = userPolicyNode.second.get<std::string>("for");

            for (const auto& serviceNode : userPolicyNode.second.get_child("allow")) {
                userPolicy.allowedServices.push_back(serviceNode.first.data());
            }

            userPolicies.push_back(std::move(userPolicy));
        }
    }

    return { providerPolicies, userPolicies };
}

}
