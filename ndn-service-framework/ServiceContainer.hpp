#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_CONTAINER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_CONTAINER_HPP

#include "LocalServiceRegistry.hpp"
#include "ServiceProvider.hpp"
#include "ServiceUser.hpp"

#include <ndn-cxx/name.hpp>

#include <functional>
#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ndn_service_framework {

/**
 * In-process runtime boundary for composing NDNSF roles and helper modules.
 *
 * ServiceContainer is not a wire protocol role and does not replace
 * ServiceUser or ServiceProvider. It owns or references the users, providers,
 * trusted local services, and helper lifecycle hooks that live in the same
 * process. Remote invocation still goes through ServiceUser and ServiceProvider
 * APIs; trusted same-process composition still goes through LocalServiceRegistry.
 */
class ServiceContainer
{
public:
  struct RuntimeConfig
  {
    ndn::Name containerIdentity;
    ndn::Name groupPrefix;
    ndn::Name controllerPrefix;
    std::string trustSchemaPath;
  };

  struct LifecycleHook
  {
    std::function<void()> start;
    std::function<void()> stop;
  };

  explicit
  ServiceContainer(RuntimeConfig config = RuntimeConfig{})
    : m_config(std::move(config))
  {
  }

  const RuntimeConfig&
  config() const
  {
    return m_config;
  }

  LocalServiceRegistry&
  localRegistry()
  {
    return m_localRegistry;
  }

  const LocalServiceRegistry&
  localRegistry() const
  {
    return m_localRegistry;
  }

  void
  addUser(const std::string& role, std::shared_ptr<ServiceUser> user)
  {
    if (role.empty()) {
      throw std::invalid_argument("ServiceContainer user role must not be empty");
    }
    if (user == nullptr) {
      throw std::invalid_argument("ServiceContainer user must not be null");
    }
    m_users[role] = std::move(user);
    if (m_defaultUserRole.empty()) {
      m_defaultUserRole = role;
    }
  }

  void
  addUser(const std::string& role, std::unique_ptr<ServiceUser> user)
  {
    addUser(role, std::shared_ptr<ServiceUser>(std::move(user)));
  }

  void
  useUser(const std::string& role, ServiceUser& user)
  {
    addUser(role, std::shared_ptr<ServiceUser>(&user, [] (ServiceUser*) {}));
  }

  bool
  hasUser(const std::string& role) const
  {
    return m_users.find(role) != m_users.end();
  }

  ServiceUser&
  user(const std::string& role)
  {
    const auto it = m_users.find(role);
    if (it == m_users.end()) {
      throw std::out_of_range("ServiceContainer user role is not registered: " + role);
    }
    return *it->second;
  }

  const ServiceUser&
  user(const std::string& role) const
  {
    const auto it = m_users.find(role);
    if (it == m_users.end()) {
      throw std::out_of_range("ServiceContainer user role is not registered: " + role);
    }
    return *it->second;
  }

  ServiceUser&
  defaultUser()
  {
    if (m_defaultUserRole.empty()) {
      throw std::out_of_range("ServiceContainer has no default user");
    }
    return user(m_defaultUserRole);
  }

  void
  addProvider(const std::string& role, std::shared_ptr<ServiceProvider> provider)
  {
    if (role.empty()) {
      throw std::invalid_argument("ServiceContainer provider role must not be empty");
    }
    if (provider == nullptr) {
      throw std::invalid_argument("ServiceContainer provider must not be null");
    }
    m_providers[role] = std::move(provider);
    if (m_defaultProviderRole.empty()) {
      m_defaultProviderRole = role;
    }
  }

  void
  addProvider(const std::string& role, std::unique_ptr<ServiceProvider> provider)
  {
    addProvider(role, std::shared_ptr<ServiceProvider>(std::move(provider)));
  }

  void
  useProvider(const std::string& role, ServiceProvider& provider)
  {
    addProvider(role, std::shared_ptr<ServiceProvider>(&provider, [] (ServiceProvider*) {}));
  }

  bool
  hasProvider(const std::string& role) const
  {
    return m_providers.find(role) != m_providers.end();
  }

  ServiceProvider&
  provider(const std::string& role)
  {
    const auto it = m_providers.find(role);
    if (it == m_providers.end()) {
      throw std::out_of_range("ServiceContainer provider role is not registered: " + role);
    }
    return *it->second;
  }

  const ServiceProvider&
  provider(const std::string& role) const
  {
    const auto it = m_providers.find(role);
    if (it == m_providers.end()) {
      throw std::out_of_range("ServiceContainer provider role is not registered: " + role);
    }
    return *it->second;
  }

  ServiceProvider&
  defaultProvider()
  {
    if (m_defaultProviderRole.empty()) {
      throw std::out_of_range("ServiceContainer has no default provider");
    }
    return provider(m_defaultProviderRole);
  }

  std::vector<std::string>
  userRoles() const
  {
    std::vector<std::string> roles;
    for (const auto& entry : m_users) {
      roles.push_back(entry.first);
    }
    return roles;
  }

  std::vector<std::string>
  providerRoles() const
  {
    std::vector<std::string> roles;
    for (const auto& entry : m_providers) {
      roles.push_back(entry.first);
    }
    return roles;
  }

  void
  addLifecycleHook(const std::string& name, LifecycleHook hook)
  {
    if (name.empty()) {
      throw std::invalid_argument("ServiceContainer lifecycle hook name must not be empty");
    }
    auto existing = std::find_if(m_lifecycleHooks.begin(),
                                 m_lifecycleHooks.end(),
                                 [&name] (const auto& entry) {
                                   return entry.first == name;
                                 });
    if (existing != m_lifecycleHooks.end()) {
      existing->second = std::move(hook);
      return;
    }
    m_lifecycleHooks.emplace_back(name, std::move(hook));
  }

  bool
  isStarted() const
  {
    return m_started;
  }

  void
  start()
  {
    if (m_started) {
      return;
    }
    for (auto& entry : m_lifecycleHooks) {
      if (entry.second.start) {
        entry.second.start();
      }
    }
    m_started = true;
  }

  void
  stop()
  {
    if (!m_started) {
      return;
    }
    for (auto it = m_lifecycleHooks.rbegin(); it != m_lifecycleHooks.rend(); ++it) {
      if (it->second.stop) {
        it->second.stop();
      }
    }
    m_started = false;
  }

private:
  RuntimeConfig m_config;
  LocalServiceRegistry m_localRegistry;
  std::map<std::string, std::shared_ptr<ServiceUser>> m_users;
  std::map<std::string, std::shared_ptr<ServiceProvider>> m_providers;
  std::vector<std::pair<std::string, LifecycleHook>> m_lifecycleHooks;
  std::string m_defaultUserRole;
  std::string m_defaultProviderRole;
  bool m_started = false;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_SERVICE_CONTAINER_HPP
