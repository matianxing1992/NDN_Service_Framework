#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_CONTAINER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_CONTAINER_HPP

#include "LocalServiceRegistry.hpp"
#include "ServiceController.hpp"
#include "ServiceProvider.hpp"
#include "ServiceUser.hpp"

#include <ndn-cxx/name.hpp>

#include <functional>
#include <algorithm>
#include <exception>
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
 * ServiceController, ServiceUser, or ServiceProvider. It owns or references the
 * controllers, users, providers, trusted local services, and helper lifecycle
 * hooks that live in the same process. Remote invocation still goes through
 * ServiceUser and ServiceProvider APIs; trusted same-process composition still
 * goes through LocalServiceRegistry.
 *
 * Applications should finish registering roles and lifecycle hooks before
 * start(). Runtime service invocation still uses ServiceUser/ServiceProvider;
 * ServiceContainer only coordinates process-local composition.
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
  addController(const std::string& role, std::shared_ptr<ServiceController> controller)
  {
    ensureNotStarted("register controller roles");
    if (role.empty()) {
      throw std::invalid_argument("ServiceContainer controller role must not be empty");
    }
    if (controller == nullptr) {
      throw std::invalid_argument("ServiceContainer controller must not be null");
    }
    m_controllers[role] = std::move(controller);
    if (m_defaultControllerRole.empty()) {
      m_defaultControllerRole = role;
    }
  }

  void
  addController(const std::string& role, std::unique_ptr<ServiceController> controller)
  {
    addController(role, std::shared_ptr<ServiceController>(std::move(controller)));
  }

  void
  useController(const std::string& role, ServiceController& controller)
  {
    addController(role, std::shared_ptr<ServiceController>(&controller, [] (ServiceController*) {}));
  }

  bool
  hasController(const std::string& role) const
  {
    return m_controllers.find(role) != m_controllers.end();
  }

  ServiceController&
  controller(const std::string& role)
  {
    const auto it = m_controllers.find(role);
    if (it == m_controllers.end()) {
      throw std::out_of_range("ServiceContainer controller role is not registered: " + role);
    }
    return *it->second;
  }

  const ServiceController&
  controller(const std::string& role) const
  {
    const auto it = m_controllers.find(role);
    if (it == m_controllers.end()) {
      throw std::out_of_range("ServiceContainer controller role is not registered: " + role);
    }
    return *it->second;
  }

  ServiceController&
  defaultController()
  {
    if (m_defaultControllerRole.empty()) {
      throw std::out_of_range("ServiceContainer has no default controller");
    }
    return controller(m_defaultControllerRole);
  }

  void
  addUser(const std::string& role, std::shared_ptr<ServiceUser> user)
  {
    ensureNotStarted("register user roles");
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
    ensureNotStarted("register provider roles");
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
  controllerRoles() const
  {
    std::vector<std::string> roles;
    for (const auto& entry : m_controllers) {
      roles.push_back(entry.first);
    }
    return roles;
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
    ensureNotStarted("register lifecycle hooks");
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
    size_t startedHooks = 0;
    try {
      for (auto& entry : m_lifecycleHooks) {
        if (entry.second.start) {
          entry.second.start();
        }
        ++startedHooks;
      }
    }
    catch (...) {
      while (startedHooks > 0) {
        --startedHooks;
        const auto& hook = m_lifecycleHooks[startedHooks].second;
        if (hook.stop) {
          try {
            hook.stop();
          }
          catch (...) {
            // Preserve the original startup failure. Applications can retry
            // start() after fixing the failed hook.
          }
        }
      }
      throw;
    }
    m_started = true;
  }

  void
  stop()
  {
    if (!m_started) {
      return;
    }
    std::exception_ptr firstError;
    for (auto it = m_lifecycleHooks.rbegin(); it != m_lifecycleHooks.rend(); ++it) {
      if (!it->second.stop) {
        continue;
      }
      try {
        it->second.stop();
      }
      catch (...) {
        if (!firstError) {
          firstError = std::current_exception();
        }
      }
    }
    m_started = false;
    if (firstError) {
      std::rethrow_exception(firstError);
    }
  }

private:
  void
  ensureNotStarted(const char* action) const
  {
    if (m_started) {
      throw std::logic_error(std::string("ServiceContainer cannot ") + action +
                             " after start()");
    }
  }

  RuntimeConfig m_config;
  LocalServiceRegistry m_localRegistry;
  std::map<std::string, std::shared_ptr<ServiceController>> m_controllers;
  std::map<std::string, std::shared_ptr<ServiceUser>> m_users;
  std::map<std::string, std::shared_ptr<ServiceProvider>> m_providers;
  std::vector<std::pair<std::string, LifecycleHook>> m_lifecycleHooks;
  std::string m_defaultControllerRole;
  std::string m_defaultUserRole;
  std::string m_defaultProviderRole;
  bool m_started = false;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_SERVICE_CONTAINER_HPP
