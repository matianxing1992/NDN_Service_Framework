#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"

#include <stdexcept>
#include <utility>
#include <algorithm>

namespace ndnsf::di {

namespace {

void
wipeTensorBundle(TensorBundle& bundle) noexcept
{
  volatile std::uint8_t* bytes = bundle.payload.data();
  for (std::size_t index = 0; index < bundle.payload.size(); ++index) {
    bytes[index] = 0;
  }
  bundle.payload.clear();
  bundle.expectedBytes = 0;
  bundle.expectedSegments = 0;
}

} // namespace

void
KvStateBinding::validate() const
{
  if (sessionId.empty() || stage.empty() || modelDigest.empty() || planDigest.empty() ||
      providerName.empty() || providerBootId.empty() || requestId.empty() ||
      generationId.empty() || stateSchemaDigest.empty()) {
    throw std::invalid_argument("KV state binding requires all identity fields");
  }
  if (exactIdentity) {
    exactIdentity->validate();
    if (exactIdentity->modelDigest != modelDigest ||
        exactIdentity->roleName != stage ||
        exactIdentity->providerIdentity != providerName ||
        exactIdentity->providerBootId != providerBootId ||
        exactIdentity->requestId != requestId ||
        exactIdentity->attemptEpoch != attemptEpoch ||
        exactIdentity->generationId != generationId ||
        exactIdentity->stateSchemaDigest != stateSchemaDigest) {
      throw std::invalid_argument(
        "KV state compact binding disagrees with complete identity");
    }
  }
}

bool
KvStateBinding::operator==(const KvStateBinding& other) const
{
  return sessionId == other.sessionId && stage == other.stage &&
         contextEpoch == other.contextEpoch && modelDigest == other.modelDigest &&
         planDigest == other.planDigest && providerName == other.providerName &&
         providerBootId == other.providerBootId && securityEpoch == other.securityEpoch &&
         requestId == other.requestId && attemptEpoch == other.attemptEpoch &&
         generationId == other.generationId &&
         stateSchemaDigest == other.stateSchemaDigest &&
         exactIdentity == other.exactIdentity;
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
KvStateStore::wipeEntry(Entry& entry) noexcept
{
  wipeTensorBundle(entry.state);
  if (entry.candidate) {
    wipeTensorBundle(entry.candidate->second);
  }
  entry.candidate.reset();
}

void
KvStateStore::setProviderBootId(std::string providerBootId)
{
  if (providerBootId.empty()) {
    throw std::invalid_argument("KV state store provider boot ID must not be empty");
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_providerBootId.empty() && m_providerBootId != providerBootId) {
    m_cleanups += m_entries.size();
    for (auto& item : m_entries) {
      wipeEntry(item.second);
    }
    m_entries.clear();
    m_usedBytes = 0;
  }
  m_providerBootId = std::move(providerBootId);
}

bool
KvStateStore::sameLineage(const KvStateBinding& left,
                          const KvStateBinding& right)
{
  auto normalizedLeft = left;
  auto normalizedRight = right;
  normalizedLeft.contextEpoch = 0;
  normalizedRight.contextEpoch = 0;
  if (normalizedLeft.exactIdentity && normalizedRight.exactIdentity) {
    normalizedLeft.exactIdentity->prefixDigest =
      normalizedRight.exactIdentity->prefixDigest;
    normalizedLeft.exactIdentity->prefixTokenCount =
      normalizedRight.exactIdentity->prefixTokenCount;
    normalizedLeft.exactIdentity->positionDigest =
      normalizedRight.exactIdentity->positionDigest;
    normalizedLeft.exactIdentity->stateInferenceEpoch =
      normalizedRight.exactIdentity->stateInferenceEpoch;
    normalizedLeft.exactIdentity->predecessorInferenceEpoch =
      normalizedRight.exactIdentity->predecessorInferenceEpoch;
  }
  return normalizedLeft == normalizedRight;
}

std::size_t
KvStateStore::entryBytes(const Entry& entry)
{
  const auto committed = entry.hasCommitted ? entry.state.payload.size() : 0;
  const auto candidate = entry.candidate
    ? entry.candidate->second.payload.size()
    : 0;
  return committed + candidate;
}

std::string
KvStateStore::keyFor(const KvStateBinding& binding)
{
  return binding.sessionId + "\n" + binding.stage + "\n" + binding.requestId +
         "\n" + std::to_string(binding.attemptEpoch) + "\n" +
         binding.generationId;
}

bool
KvStateStore::evictUntilFits(std::size_t incomingBytes,
                             const std::string& replacingKey)
{
  while ((!m_entries.empty() && m_usedBytes + incomingBytes > m_maxBytes) ||
         (m_entries.size() >= m_maxEntries && m_entries.count(replacingKey) == 0)) {
    auto victim = m_entries.end();
    for (auto current = m_entries.begin(); current != m_entries.end(); ++current) {
      if (current->first == replacingKey || current->second.pinned) {
        continue;
      }
      if (victim == m_entries.end() ||
          current->second.lastAccess < victim->second.lastAccess) {
        victim = current;
      }
    }
    if (victim == m_entries.end()) {
      return false;
    }
    m_usedBytes -= entryBytes(victim->second);
    wipeEntry(victim->second);
    m_entries.erase(victim);
    ++m_evictions;
  }
  return m_usedBytes + incomingBytes <= m_maxBytes &&
         (m_entries.size() < m_maxEntries || m_entries.count(replacingKey) != 0);
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
  const auto key = keyFor(binding);
  const auto old = m_entries.find(key);
  if (old != m_entries.end()) {
    if (old->second.pinned || old->second.candidate) {
      return false;
    }
    const auto previousBytes = entryBytes(old->second);
    const auto additionalBytes = state.payload.size() > previousBytes
      ? state.payload.size() - previousBytes
      : 0;
    if (!evictUntilFits(additionalBytes, key)) {
      return false;
    }
    m_usedBytes -= previousBytes;
    wipeEntry(old->second);
    old->second = Entry{
      std::move(binding), std::move(state), true, false, std::nullopt,
      ++m_accessSequence};
    m_usedBytes += old->second.state.payload.size();
    return true;
  }
  if (!evictUntilFits(state.payload.size(), key)) {
    return false;
  }
  m_usedBytes += state.payload.size();
  m_entries.emplace(key, Entry{
    std::move(binding), std::move(state), true, false, std::nullopt,
    ++m_accessSequence});
  return true;
}

std::optional<TensorBundle>
KvStateStore::lookup(const KvStateBinding& binding)
{
  binding.validate();
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(keyFor(binding));
  if (found == m_entries.end() || !found->second.hasCommitted ||
      !(found->second.binding == binding)) {
    return std::nullopt;
  }
  found->second.lastAccess = ++m_accessSequence;
  return found->second.state;
}

std::optional<TensorBundle>
KvStateStore::beginTransition(const KvStateBinding& predecessor)
{
  predecessor.validate();
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(keyFor(predecessor));
  if (found == m_entries.end() || !found->second.hasCommitted ||
      found->second.pinned || found->second.candidate ||
      !(found->second.binding == predecessor)) {
    return std::nullopt;
  }
  found->second.pinned = true;
  found->second.lastAccess = ++m_accessSequence;
  return found->second.state;
}

bool
KvStateStore::stageCandidate(std::optional<KvStateBinding> predecessor,
                             KvStateBinding candidate,
                             TensorBundle state)
{
  candidate.validate();
  if (state.payload.empty() || state.payload.size() > m_maxBytes) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_providerBootId.empty() || candidate.providerBootId != m_providerBootId) {
    return false;
  }
  const auto key = keyFor(candidate);
  auto found = m_entries.find(key);
  if (predecessor) {
    predecessor->validate();
    if (predecessor->exactIdentity.has_value() !=
        candidate.exactIdentity.has_value()) {
      return false;
    }
    if (predecessor->exactIdentity &&
        (candidate.exactIdentity->prefixTokenCount !=
           predecessor->exactIdentity->prefixTokenCount + 1 ||
         candidate.exactIdentity->positionDigest ==
           predecessor->exactIdentity->positionDigest ||
         candidate.exactIdentity->stateInferenceEpoch !=
           predecessor->exactIdentity->stateInferenceEpoch + 1 ||
         candidate.exactIdentity->predecessorInferenceEpoch !=
           std::optional<std::uint64_t>(
             predecessor->exactIdentity->stateInferenceEpoch) ||
         candidate.exactIdentity->cacheEpoch !=
           predecessor->exactIdentity->cacheEpoch)) {
      return false;
    }
    if (!sameLineage(*predecessor, candidate) ||
        candidate.contextEpoch != predecessor->contextEpoch + 1 ||
        found == m_entries.end() || !found->second.hasCommitted ||
        !found->second.pinned || found->second.candidate ||
        !(found->second.binding == *predecessor)) {
      return false;
    }
    if (!evictUntilFits(state.payload.size(), key)) {
      return false;
    }
    m_usedBytes += state.payload.size();
    found->second.candidate = std::make_pair(
      std::move(candidate), std::move(state));
    return true;
  }

  if (candidate.contextEpoch != 0 ||
      (candidate.exactIdentity &&
       (candidate.exactIdentity->stateInferenceEpoch != 0 ||
        candidate.exactIdentity->predecessorInferenceEpoch)) ||
      found != m_entries.end() ||
      !evictUntilFits(state.payload.size(), key)) {
    return false;
  }
  Entry entry;
  entry.hasCommitted = false;
  entry.pinned = true;
  entry.candidate = std::make_pair(std::move(candidate), std::move(state));
  entry.lastAccess = ++m_accessSequence;
  m_usedBytes += entry.candidate->second.payload.size();
  m_entries.emplace(key, std::move(entry));
  return true;
}

bool
KvStateStore::commitCandidate(const KvStateBinding& candidate)
{
  candidate.validate();
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(keyFor(candidate));
  if (found == m_entries.end() || !found->second.pinned ||
      !found->second.candidate ||
      !(found->second.candidate->first == candidate)) {
    return false;
  }
  if (found->second.hasCommitted) {
    m_usedBytes -= found->second.state.payload.size();
  }
  found->second.binding = std::move(found->second.candidate->first);
  found->second.state = std::move(found->second.candidate->second);
  found->second.candidate.reset();
  found->second.hasCommitted = true;
  found->second.pinned = false;
  found->second.lastAccess = ++m_accessSequence;
  return true;
}

bool
KvStateStore::rollbackTransition(const KvStateBinding& binding)
{
  binding.validate();
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_entries.find(keyFor(binding));
  if (found == m_entries.end() || !found->second.pinned) {
    return false;
  }
  if (found->second.candidate) {
    if (!(found->second.candidate->first == binding)) {
      return false;
    }
      m_usedBytes -= found->second.candidate->second.payload.size();
      wipeTensorBundle(found->second.candidate->second);
      found->second.candidate.reset();
      if (!found->second.hasCommitted) {
        wipeTensorBundle(found->second.state);
        m_entries.erase(found);
      return true;
    }
  }
  else if (!found->second.hasCommitted || !(found->second.binding == binding)) {
    return false;
  }
  found->second.pinned = false;
  found->second.lastAccess = ++m_accessSequence;
  return true;
}

bool
KvStateStore::erase(const std::string& sessionId, const std::string& stage)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  bool erased = false;
  for (auto found = m_entries.begin(); found != m_entries.end();) {
    if (found->second.binding.sessionId != sessionId ||
        found->second.binding.stage != stage) {
      ++found;
      continue;
    }
    m_usedBytes -= entryBytes(found->second);
    found = m_entries.erase(found);
    ++m_cleanups;
    erased = true;
  }
  return erased;
}

void
KvStateStore::clear()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_cleanups += m_entries.size();
  for (auto& item : m_entries) {
    wipeEntry(item.second);
  }
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

std::size_t
KvStateStore::pinnedCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<std::size_t>(std::count_if(
    m_entries.begin(), m_entries.end(), [] (const auto& item) {
      return item.second.pinned;
    }));
}

std::size_t
KvStateStore::candidateCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<std::size_t>(std::count_if(
    m_entries.begin(), m_entries.end(), [] (const auto& item) {
      return item.second.candidate.has_value();
    }));
}

std::uint64_t
KvStateStore::evictionCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_evictions;
}

std::uint64_t
KvStateStore::cleanupCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_cleanups;
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
  m_runtime.registerRunner(spec, m_runnerFactory->create(spec));
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

ConversationStateSnapshot
NativeProviderSession::conversationStateSnapshot() const
{
  return m_runtime.conversationStateSnapshot();
}

bool
NativeProviderSession::promoteDecodeStateToConversation(
  const std::string& sessionId,
  const std::string& role,
  ConversationStateBinding binding,
  std::uint64_t nowMs)
{
  ensureKnownRole(role);
  auto spec = roleSpec(role, sessionId);
  return m_runtime.promoteDecodeStateToConversation(
    sessionId, spec, std::move(binding), nowMs);
}

std::optional<TensorBundle>
NativeProviderSession::lookupConversationState(
  const ConversationStateBinding& binding,
  std::uint64_t nowMs)
{
  return m_runtime.lookupConversationState(binding, nowMs);
}

bool
NativeProviderSession::pauseConversationStateToHost(
  const ConversationStateBinding& binding,
  std::uint64_t nowMs)
{
  return m_runtime.pauseConversationStateToHost(binding, nowMs);
}

std::future<bool>
NativeProviderSession::prefetchConversationStateToGpu(
  const ConversationStateBinding& binding,
  std::uint64_t nowMs)
{
  return m_runtime.prefetchConversationStateToGpu(binding, nowMs);
}

bool
NativeProviderSession::cancelConversationStatePrefetch(
  const ConversationStateBinding& binding)
{
  return m_runtime.cancelConversationStatePrefetch(binding);
}

bool
NativeProviderSession::pinConversationState(
  const ConversationStateBinding& binding,
  std::uint64_t nowMs)
{
  return m_runtime.pinConversationState(binding, nowMs);
}

bool
NativeProviderSession::unpinConversationState(
  const ConversationStateBinding& binding)
{
  return m_runtime.unpinConversationState(binding);
}

bool
NativeProviderSession::releaseConversationState(
  const ConversationStateBinding& binding)
{
  return m_runtime.releaseConversationState(binding);
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
