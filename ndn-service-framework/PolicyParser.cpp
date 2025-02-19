#include "PolicyParser.hpp"

namespace ndn_service_framework {

std::pair<std::vector<ProviderPolicy>, std::vector<UserPolicy>> PolicyParser::parsePolicyFile(const std::string& filename) {
    std::ifstream file(filename);
    std::vector<ProviderPolicy> providerPolicies;
    std::vector<UserPolicy> userPolicies;

    if (!file.is_open()) {
        std::cerr << "Error opening file!" << std::endl;
        return std::make_pair(std::vector<ProviderPolicy>(), std::vector<UserPolicy>());
    }

    boost::property_tree::ptree pt;

    // 将文件内容读取到ptree对象
    try {
        boost::property_tree::read_info(file, pt);
    } catch (const boost::property_tree::info_parser_error& e) {
        std::cerr << "Error parsing file: " << e.what() << std::endl;
    }

    // 解析Provider Policies
    for (const auto& providerPolicyNode : pt.get_child("provider-policies")) {
        if (providerPolicyNode.first == "provider-policy") {
            ProviderPolicy providerPolicy;
            providerPolicy.serviceName = providerPolicyNode.second.get<std::string>("for");

            // 获取允许的提供者
            for (const auto& providerNode : providerPolicyNode.second.get_child("allow")) {
                providerPolicy.allowedProviders.push_back(providerNode.first.data());
            }

            providerPolicies.push_back(providerPolicy);
        }
    }

    // 解析User Policies
    for (const auto& userPolicyNode : pt.get_child("user-policies")) {
        if (userPolicyNode.first == "user-policy") {
            UserPolicy userPolicy;
            userPolicy.userName = userPolicyNode.second.get<std::string>("for");

            // 获取允许的服务
            for (const auto& serviceNode : userPolicyNode.second.get_child("allow")) {
               userPolicy.allowedServices.push_back(serviceNode.first.data());
            }

            userPolicies.push_back(userPolicy);
        }
    }

    // // 打印解析结果（可选）
    // std::cout << "Provider Policies:" << std::endl;
    // for (const auto& policy : providerPolicies) {
    //     std::cout << "  Service Name: " << policy.serviceName << std::endl;
    //     for (const auto& provider : policy.allowedProviders) {
    //         std::cout << "    Allowed Provider: " << provider << std::endl;
    //     }
    // }

    // std::cout << "User Policies:" << std::endl;
    // for (const auto& policy : userPolicies) {
    //     std::cout << "  User Name: " << policy.userName << std::endl;
    //     for (const auto& service : policy.allowedServices) {
    //         std::cout << "    Allowed Service: " << service << std::endl;
    //     }
    // }
    return std::make_pair(providerPolicies, userPolicies);
}

}
