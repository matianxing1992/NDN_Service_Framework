#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"

#include <stdexcept>
#include <utility>
#include <algorithm>

namespace ndnsf::di {

void
KvStateBinding::validate() const
{
  if (sessionId.empty() || stage.empty() || modelDigest.empty() || planDigest.empty() ||
      providerName.empty() || providerBootId.empty()) {
    throw std::invalid_argument("KV state binding requires all identity fields");
  }
}

bool
KvStateBinding::operator==(const KvStateBinding& other) const
{
  return sessionId == other.sessionId && stage == other.stage &&
         contextEpoch == other.contextEpoch && modelDigest == other.modelDigest &&
         planDigest == other.planDigest && providerName == other.providerName &&
         providerBootId == other.providerBootId && securityEpoch == other.securityEpoch;
}

KvStateStore::KvStateStore(std::size_t maxBytes, std::size_t maxEntries)
  : m_maxBytes(maxBytes)
  , m_maxEntries(maxEntries)
{
  if (m_maxBytes == 0 || m_maxEntries == 0) {
    throw std::invalid_argument("KV state store limits must be positive");
  }
}

void
KvStateStore::setProviderBootId(std::string providerBootId)
{
  if (providerBootId.empty()) {
    throw std::invalid_argument("KV state store provider boot ID must not be empty");
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_providerBootId.empty() && m_providerBootId != providerBootId) {
    m_entries.clear();
    m_usedBytes = 0;
  }
  m_providerBootId = std::move(providerBootId);
}

std::string
KvStateStore::keyFor(const std::string& sessionId, const std::string& stage)
{
  return sessionId + "\n" + stage;
}

void
KvStateStore::evictUntilFits(std::size_t incomingBytes, const std::string& replacingKey)
{
  while ((!m_entries.empty() && m_usedBytes + incomingBytes > m_maxBytes) ||
         (m_entries.size() >= m_maxEntries && m_entries.count(replacingKey) == 0)) {
    auto victim = std::min_element(
      m_entries.begin(), m_entries.end(), [] (const auto& left, const auto& right) {
        return left.second.lastAccess < right.second.lastAccess;
      });
    if (victim == m_entries.end()) {
      break;
    }
    m_usedBytes -= victim->second.state.payload.size();
    m_entries.erase(victim);
  }
}

bool
KvStateStore::put(KvStateBinding binding, TensorBundle state)
{
  binding.validate();
  if (state.payload.empty() || state.payload.size() > m_maxBytes) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_providerBootId.empty() || binding.providerBootId != m_providerBootId) {
    return false;
  }
  const auto key = keyFor(binding.sessionId, binding.stage);
  const auto old = m_entries.find(key);
  if (old != m_entries.end()) {
    m_usedBytes -= old->second.state.payload.size();
    m_entries.erase(old);
  }
  evictUntilFits(state.payload.size(), key);
  if (m_usedBytes + state.payload.size() > m_maxBytes || m_entries.size() >= m_maxEntries) {
    return false;
  }
  m_usedBytes += state.payload.size();
  m_entries.emplace(key, Entry{std::move(binding), std::move(state), ++m_accessSequence});
  return true;
}

std::optional<TensorBundle>
KvStateStore::lookup(const KvStateBinding& binding)
{
  binding.validate();
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(keyFor(binding.sessionId, binding.stage));
  if (found == m_entries.end() || !(found->second.binding == binding)) {
    return std::nullopt;
  }
  found->second.lastAccess = ++m_accessSequence;
  return found->second.state;
}

bool
KvStateStore::erase(const std::string& sessionId, const std::string& stage)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(keyFor(sessionId, stage));
  if (found == m_entries.end()) {
    return false;
  }
  m_usedBytes -= found->second.state.payload.size();
  m_entries.erase(found);
  return true;
}

void
KvStateStore::clear()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_entries.clear();
  m_usedBytes = 0;
}

std::size_t
KvStateStore::size() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_entries.size();
}

std::size_t
KvStateStore::usedBytes() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_usedBytes;
}

NativeProviderSession::NativeProviderSession(NativeExecutionPlan plan,
                                             NativeProviderAssignment assignment,
                                             std::shared_ptr<DependencyIo> dependencyIo,
                                             std::shared_ptr<NativeModelRunnerFactory> runnerFactory,
                                             std::size_t workerCount)
  : m_plan(std::move(plan))
  , m_assignment(std::move(assignment))
  , m_dependencyIo(std::move(dependencyIo))
  , m_runnerFactory(std::move(runnerFactory))
  , m_runtime(workerCount)
{
  if (!m_dependencyIo) {
    throw std::invalid_argument("NativeProviderSession requires DependencyIo");
  }
  if (!m_runnerFactory) {
    throw std::invalid_argument("NativeProviderSession requires NativeModelRunnerFactory");
  }
}

void
NativeProviderSession::registerRunner(const NativeModelRunnerSpec& spec)
{
  ensureKnownRole(spec.role);
  m_runtime.registerRunner(spec.role, m_runnerFactory->create(spec));
}

bool
NativeProviderSession::hasRunner(const std::string& role) const
{
  return m_runtime.hasRunner(role);
}

RoleSpec
NativeProviderSession::roleSpec(const std::string& role, const std::string& sessionId) const
{
  return roleSpecFor(m_plan, role, sessionId, m_assignment);
}

std::future<ProviderRoleResult>
NativeProviderSession::executeRoleAsync(const std::string& sessionId,
                                        const std::string& role)
{
  return m_runtime.executeRoleAsync(sessionId, roleSpec(role, sessionId), m_dependencyIo);
}

std::future<ProviderRoleResult>
NativeProviderSession::executeRoleAsync(const std::string& sessionId,
                                        const std::string& role,
                                        std::map<std::string, TensorBundle> initialInputsByScope)
{
  return m_runtime.executeRoleAsync(sessionId,
                                    roleSpec(role, sessionId),
                                    m_dependencyIo,
                                    std::move(initialInputsByScope));
}

void
NativeProviderSession::ensureKnownRole(const std::string& role) const
{
  if (role.empty()) {
    throw std::invalid_argument("NativeProviderSession role must not be empty");
  }
  for (const auto& item : m_plan.roles) {
    if (item == role) {
      return;
    }
  }
  throw std::out_of_range("NativeProviderSession has no role in plan: " + role);
}

} // namespace ndnsf::di
