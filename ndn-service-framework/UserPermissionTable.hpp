#ifndef NDN_SERVICE_FRAMEWORK_USER_PERMISSION_TABLE_HPP
#define NDN_SERVICE_FRAMEWORK_USER_PERMISSION_TABLE_HPP

#include <boost/bimap.hpp>
#include <boost/bimap/set_of.hpp>
#include <boost/bimap/multiset_of.hpp>
#include <iostream>
#include <string>
#include <optional>
#include <vector>

namespace ndn_service_framework {
    class UserPermissionTable {
public:
    using UserPermissionMap = boost::bimap<
        boost::bimaps::set_of<std::string>,             // Key: Service Full Name; "/<provdier>/<service>/<function>""
        boost::bimaps::multiset_of<std::pair<std::string, std::string>>  // Value: (FunctionName, Service Token):("/<service>/<function>","token")
    >;

    // Insert a user permission
    void insertPermission(const std::string& serviceName, const std::string& FunctionName, const std::string& serviceToken) {
        userPermissions.insert({serviceName, {FunctionName, serviceToken}});
    }

    // Remove a user permission by service name
    void removePermissionByService(const std::string& serviceName) {
        userPermissions.left.erase(serviceName);
    }

    // Search by FunctionName and return all corresponding service full names and tokens
	std::vector<std::pair<std::string, std::string>> searchByFunctionName(const std::string& functionName) const {
	    std::vector<std::pair<std::string, std::string>> result;

	    // Iterate over the permissions to find entries with the specified FunctionName
	    for (auto it = userPermissions.right.begin(); it != userPermissions.right.end(); ++it) {
		if (it->first.first == functionName) {
		    result.emplace_back(it->second, it->first.second);
		}
	    }

	    return result;
	}

    // Query user permission and return optional string
    std::optional<std::string> queryPermission(const std::string& serviceName, const std::string& FunctionName) const {
        auto it = userPermissions.left.find(serviceName);
        if (it != userPermissions.left.end() && it->second.first == FunctionName) {
            return it->second.second;
        }
        return std::nullopt;  // Permission not found
    }

public:
    UserPermissionMap userPermissions;
};
} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_USER_PERMISSION_TABLE_HPP
