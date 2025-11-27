#ifndef NDN_SERVICE_FRAMEWORK_USER_PERMISSION_TABLE_HPP
#define NDN_SERVICE_FRAMEWORK_USER_PERMISSION_TABLE_HPP

#include <boost/bimap.hpp>
#include <boost/bimap/set_of.hpp>
#include <boost/bimap/multiset_of.hpp>

#include <iostream>
#include <string>
#include <optional>
#include <vector>
#include <mutex>

namespace ndn_service_framework {

class UserPermissionTable
{
public:
  // Key: service full name, e.g., "/<provider>/<service>/<function>"
  // Value: (FunctionName, ServiceToken), e.g., ("/<service>/<function>", "token")
  using UserPermissionMap = boost::bimap<
      boost::bimaps::set_of<std::string>,
      boost::bimaps::multiset_of<std::pair<std::string, std::string>>>;

public:
  UserPermissionTable() = default;
  ~UserPermissionTable() = default;

  UserPermissionTable(const UserPermissionTable&)            = delete;
  UserPermissionTable& operator=(const UserPermissionTable&) = delete;

  // Insert a user permission
  void insertPermission(const std::string& serviceName,
                        const std::string& functionName,
                        const std::string& serviceToken)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    userPermissions.insert(UserPermissionMap::value_type(
        serviceName, std::make_pair(functionName, serviceToken)));
  }

  // Remove a user permission by service name (full name)
  void removePermissionByService(const std::string& serviceName)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    userPermissions.left.erase(serviceName);
  }

  // Search by FunctionName and return all corresponding service full names and tokens
  // Result: vector of (serviceFullName, token)
  std::vector<std::pair<std::string, std::string>>
  searchByFunctionName(const std::string& functionName) const
  {
    std::vector<std::pair<std::string, std::string>> result;

    // Right view: key = (FunctionName, Token), value = serviceFullName
    for (auto it = userPermissions.right.begin();
         it != userPermissions.right.end(); ++it) {
      if (it->first.first == functionName) {
        const std::string& serviceFullName = it->second;
        const std::string& token           = it->first.second;
        result.emplace_back(serviceFullName, token);
      }
    }

    return result;
  }

  // Query user permission and return optional token
  // serviceName: full name "/<provider>/<service>/<function>"
  // functionName: "/<service>/<function>"
  std::optional<std::string>
  queryPermission(const std::string& serviceName,
                  const std::string& functionName) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = userPermissions.left.find(serviceName);
    if (it != userPermissions.left.end()) {
      const auto& fnAndToken = it->second; // (FunctionName, Token)
      if (fnAndToken.first == functionName) {
        return fnAndToken.second;
      }
    }
    return std::nullopt; // Permission not found
  }

  // (Optional) Debug helper: get a snapshot of all entries
  std::vector<std::tuple<std::string, std::string, std::string>>
  dumpAll() const
  {
    std::vector<std::tuple<std::string, std::string, std::string>> out;
    std::lock_guard<std::mutex> lock(m_mutex);

    for (const auto& entry : userPermissions.left) {
      const std::string& serviceName = entry.first;
      const std::string& fn          = entry.second.first;
      const std::string& token       = entry.second.second;
      out.emplace_back(serviceName, fn, token);
    }
    return out;
  }

private:
  mutable std::mutex m_mutex;
  UserPermissionMap  userPermissions;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_USER_PERMISSION_TABLE_HPP
