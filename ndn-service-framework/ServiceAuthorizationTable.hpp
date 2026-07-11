#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_AUTHORIZATION_TABLE_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_AUTHORIZATION_TABLE_HPP

#include "NDNSFMessages.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace ndn_service_framework {

struct ServiceAuthorizationRecord
{
  std::string providerServiceName;
  std::string serviceName;
  size_t permissionKind = 0;
  size_t policyEpoch = 0;

  bool
  isValid() const
  {
    return !providerServiceName.empty() && !serviceName.empty() &&
           policyEpoch > 0 &&
           (permissionKind == tlv::UserPermission ||
            permissionKind == tlv::ProviderPermission);
  }
};

class ServiceAuthorizationTable
{
public:
  bool
  replacePermissions(size_t permissionKind,
                     size_t policyEpoch,
                     const std::vector<ServiceAuthorizationRecord>& records)
  {
    if (policyEpoch == 0 ||
        (permissionKind != tlv::UserPermission &&
         permissionKind != tlv::ProviderPermission)) {
      return false;
    }
    for (const auto& record : records) {
      if (!record.isValid() || record.permissionKind != permissionKind ||
          record.policyEpoch != policyEpoch) {
        return false;
      }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_records) {
      if (item.second.permissionKind == permissionKind &&
          item.second.policyEpoch > policyEpoch) {
        return false;
      }
    }

    for (auto it = m_records.begin(); it != m_records.end();) {
      if (it->second.permissionKind == permissionKind) {
        it = m_records.erase(it);
      }
      else {
        ++it;
      }
    }
    for (const auto& record : records) {
      m_records[record.providerServiceName] = record;
    }
    return true;
  }

  bool
  upsert(const ServiceAuthorizationRecord& record)
  {
    if (!record.isValid()) {
      return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_records.find(record.providerServiceName);
    if (it != m_records.end() && it->second.policyEpoch > record.policyEpoch) {
      return false;
    }
    m_records[record.providerServiceName] = record;
    return true;
  }

  std::optional<ServiceAuthorizationRecord>
  find(const std::string& providerServiceName) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_records.find(providerServiceName);
    if (it == m_records.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  bool
  contains(const std::string& providerServiceName,
           const std::string& serviceName,
           size_t permissionKind) const
  {
    auto record = find(providerServiceName);
    return record && record->serviceName == serviceName &&
           record->permissionKind == permissionKind;
  }

  std::vector<ServiceAuthorizationRecord>
  snapshot() const
  {
    std::vector<ServiceAuthorizationRecord> result;
    std::lock_guard<std::mutex> lock(m_mutex);
    result.reserve(m_records.size());
    for (const auto& item : m_records) {
      result.push_back(item.second);
    }
    return result;
  }

  std::vector<std::tuple<std::string, std::string, size_t>>
  dumpAllowedServices() const
  {
    std::vector<std::tuple<std::string, std::string, size_t>> result;
    for (const auto& record : snapshot()) {
      result.emplace_back(record.providerServiceName,
                          record.serviceName,
                          record.policyEpoch);
    }
    return result;
  }

private:
  mutable std::mutex m_mutex;
  std::map<std::string, ServiceAuthorizationRecord> m_records;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_SERVICE_AUTHORIZATION_TABLE_HPP
