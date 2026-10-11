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
    std::map<std::string, ServiceAuthorizationRecord> normalized;
    for (const auto& record : records) {
      const auto canonical = normalize(record);
      if (!canonical || canonical->permissionKind != permissionKind ||
          canonical->policyEpoch != policyEpoch) return false;
      const auto inserted = normalized.emplace(canonical->providerServiceName, *canonical);
      if (!inserted.second && (inserted.first->second.serviceName != canonical->serviceName ||
          inserted.first->second.permissionKind != canonical->permissionKind ||
          inserted.first->second.policyEpoch != canonical->policyEpoch)) return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_records) {
      if (item.second.permissionKind == permissionKind &&
          item.second.policyEpoch > policyEpoch) return false;
      const auto replacement = normalized.find(item.first);
      if (replacement != normalized.end() &&
          (item.second.permissionKind != replacement->second.permissionKind ||
           item.second.serviceName != replacement->second.serviceName)) return false;
    }
    // Commit the complete replacement only after normalization and allocation.
    auto next = m_records;
    for (auto it = next.begin(); it != next.end();) {
      if (it->second.permissionKind == permissionKind) it = next.erase(it);
      else ++it;
    }
    next.insert(normalized.begin(), normalized.end());
    m_records.swap(next);
    return true;
  }

  bool
  upsert(const ServiceAuthorizationRecord& record)
  {
    const auto canonical = normalize(record);
    if (!canonical) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_records.find(canonical->providerServiceName);
    if (it != m_records.end() &&
        (it->second.policyEpoch > canonical->policyEpoch ||
         it->second.serviceName != canonical->serviceName ||
         it->second.permissionKind != canonical->permissionKind)) return false;
    m_records[canonical->providerServiceName] = *canonical;
    return true;
  }

  std::optional<ServiceAuthorizationRecord>
  find(const std::string& providerServiceName) const
  {
    const auto canonical = canonicalName(providerServiceName);
    if (!canonical) return std::nullopt;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_records.find(*canonical);
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
    const auto canonical = canonicalName(serviceName);
    return record && canonical && record->serviceName == *canonical &&
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
  static std::optional<std::string> canonicalName(const std::string& value)
  {
    try {
      if (value.empty() || value.front() != '/') return std::nullopt;
      const ndn::Name name(value);
      if (name.empty()) return std::nullopt;
      return name.toUri(ndn::name::UriFormat::CANONICAL);
    }
    catch (const std::exception&) { return std::nullopt; }
  }

  static std::optional<ServiceAuthorizationRecord> normalize(const ServiceAuthorizationRecord& record)
  {
    if (!record.isValid()) return std::nullopt;
    const auto provider = canonicalName(record.providerServiceName);
    const auto service = canonicalName(record.serviceName);
    if (!provider || !service) return std::nullopt;
    auto result = record;
    result.providerServiceName = *provider;
    result.serviceName = *service;
    return result;
  }

  mutable std::mutex m_mutex;
  std::map<std::string, ServiceAuthorizationRecord> m_records;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_SERVICE_AUTHORIZATION_TABLE_HPP
