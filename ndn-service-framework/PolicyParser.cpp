#include "PolicyParser.hpp"

namespace ndn_service_framework {

ndn::Block ServicePolicy::WireEncode() const {
    ndn::Block block(tlv::ServicePolicyType);

    // Encode for
    block.push_back(ndn::makeStringBlock(tlv::ForType, forValue));

    // Encode allowedUsers
    for (const auto& user : allowedUsers) {
        block.push_back(ndn::makeStringBlock(tlv::AllowedUsersType, user));
    }

    // Encode deniedUsers
    for (const auto& user : deniedUsers) {
        block.push_back(ndn::makeStringBlock(tlv::DeniedUsersType, user));
    }

    block.encode();

    return block;
}

bool ServicePolicy::WireDecode(const ndn::Block& block) {
    // Verify message type
    if (block.type() != tlv::ServicePolicyType) {
        return false; // Message type mismatch
    }

    // Parse Block
    block.parse();
    for (const auto& element : block.elements()) {
        if (element.type() == tlv::ForType) {
            forValue = ndn::readString(element);
        } else if (element.type() == tlv::AllowedUsersType) {
            allowedUsers.push_back(ndn::readString(element));
        } else if (element.type() == tlv::DeniedUsersType) {
            deniedUsers.push_back(ndn::readString(element));
        }
    }

    return true;
}

std::list<ServicePolicy> PolicyParser::parseServicePolicies(const std::string& policyFilePath) {
    std::ifstream input(policyFilePath);
    pt::ptree tree;
    pt::read_info(input, tree);
    input.close();

    std::list<ServicePolicy> servicePolicies;
    for (const auto& servicePolicy : tree.get_child("service-policies")) {
        ServicePolicy policy;
        policy.forValue = servicePolicy.second.get<std::string>("for", "");
        for (const auto& allowUser : servicePolicy.second.get_child("allow")) {
            policy.allowedUsers.push_back(allowUser.second.data());
        }
        for (const auto& denyUser : servicePolicy.second.get_child("deny")) {
            policy.deniedUsers.push_back(denyUser.second.data());
        }
        servicePolicies.push_back(policy);
    }

    return servicePolicies;
}

void PolicyParser::writeServicePolicies(const std::string& policyFilePath, const std::list<ServicePolicy>& servicePolicies) {
    pt::ptree tree;

    pt::ptree servicePoliciesTree;
    for (const auto& policy : servicePolicies) {
        pt::ptree servicePolicyTree;
        servicePolicyTree.put("for", policy.forValue);

        pt::ptree allowTree;
        for (const auto& user : policy.allowedUsers) {
            allowTree.push_back(std::make_pair("", pt::ptree(user)));
        }
        servicePolicyTree.add_child("allow", allowTree);

        pt::ptree denyTree;
        for (const auto& user : policy.deniedUsers) {
            denyTree.push_back(std::make_pair("", pt::ptree(user)));
        }
        servicePolicyTree.add_child("deny", denyTree);

        servicePoliciesTree.push_back(std::make_pair("", servicePolicyTree));
    }

    tree.add_child("service-policies", servicePoliciesTree);

    std::ofstream output(policyFilePath);
    pt::write_info(output, tree);
    output.close();
}

}
