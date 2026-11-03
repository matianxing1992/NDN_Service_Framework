#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-publication.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-save.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {

std::pair<NativePortableKvCheckpointLimitsV1, NativeProtectedKvCheckpointLimitsV1>
nativeKvPublicationLimits(const NativeProviderHandoffConfig& config,
  const std::function<void()>& current)
{
  nativeValidateKvSaveConfig(config, config.signingCertificate.getIdentity());
  const auto cancelled = [current] {
    if (!current) return false;
    try {
      current();
      return false;
    }
    catch (...) {
      return true;
    }
  };
  NativePortableKvCheckpointLimitsV1 portable;
  portable.portable = {config.maxStateBytes, config.maxStateBytes + 8 * 1024 * 1024, cancelled};
  portable.maxMetadataBytes = 1024 * 1024;
  portable.maxCheckpointBytes = portable.portable.maxEncodedBytes + 8 * 1024 * 1024;
  NativeProtectedKvCheckpointLimitsV1 protection{
    portable.maxCheckpointBytes, portable.maxCheckpointBytes + 1024 * 1024, cancelled};
  validateNativePortableKvCheckpointLimits(portable);
  return {std::move(portable), std::move(protection)};
}

NativeConversationHandoffOwnerContextV1
resolveNativeConversationHandoffOwnerContextV1(
  const NativeConversationHandoffOwnerContextResolverV1& resolver,
  const ndn::Name& authenticatedOwner, const ndn::Name& serviceName,
  const std::string& operationId)
{
  if (!resolver || authenticatedOwner.empty() || serviceName.empty() ||
      operationId.empty() || operationId.find('\0') != std::string::npos)
    throw std::invalid_argument("HANDOFF_AUTHENTICATED_OWNER_SCOPE_INVALID");
  auto context = resolver(authenticatedOwner, serviceName, operationId);
  if (context.ownerEncryptionCertificate.getName().empty() ||
      context.ownerEncryptionCertificate.getIdentity() != authenticatedOwner ||
      context.saveBatchLimits.maxEntries == 0 || context.saveBatchLimits.maxMetadataBytes == 0)
    throw std::invalid_argument("HANDOFF_OWNER_CONTEXT_BINDING_INVALID");
  const auto fileLimits = nativeKvPublicationLimits(context.publication);
  if (context.policy.scratchRoot != context.publication.workspace)
    throw std::invalid_argument("HANDOFF_OWNER_CONTEXT_WORKSPACE_MISMATCH");

  const auto& policy = context.policy;
  constexpr std::size_t MAX_CONCURRENT_EVENTS = 16;
  constexpr std::size_t MAX_CONCURRENT_ROLE_SLOTS = 64;
  constexpr std::uint64_t MAX_EVENT_BYTES = std::uint64_t{1} << 40;
  constexpr std::uint64_t MAX_SCRATCH_QUOTA_BYTES = std::uint64_t{1} << 44;
  constexpr std::uint64_t MAX_SAVE_DURATION_MS = 10 * 60 * 1000;
  if (!policy.enabled || policy.maxConcurrentEvents == 0 ||
      policy.maxConcurrentEvents > MAX_CONCURRENT_EVENTS ||
      policy.maxRolesPerEvent == 0 || policy.maxRolesPerEvent > 64 ||
      policy.maxConcurrentRoleSlots < policy.maxRolesPerEvent ||
      policy.maxConcurrentRoleSlots > MAX_CONCURRENT_ROLE_SLOTS ||
      policy.maxPacketCountPerRole == 0 || policy.maxPacketCountPerRole > 4096 ||
      policy.maxPacketSetWireBytesPerRole == 0 ||
      policy.maxPacketSetWireBytesPerRole > MAX_EVENT_BYTES ||
      policy.maxAggregateBytes == 0 || policy.maxAggregateBytes > MAX_EVENT_BYTES ||
      policy.maxScratchBytesPerEvent == 0 ||
      policy.maxScratchBytesPerEvent > MAX_EVENT_BYTES ||
      policy.scratchQuotaBytes < policy.maxScratchBytesPerEvent ||
      policy.scratchQuotaBytes > MAX_SCRATCH_QUOTA_BYTES ||
      policy.maxSaveDurationMs == 0 || policy.maxSaveDurationMs > MAX_SAVE_DURATION_MS ||
      !policy.scratchRoot.is_absolute() || policy.scratchRoot.empty() ||
      context.repoTargetByRole.empty() ||
      context.repoTargetByRole.size() > policy.maxRolesPerEvent) {
    throw std::invalid_argument("HANDOFF_EFFECTIVE_POLICY_INVALID");
  }

  NativeJson effectivePolicy = NativeJson::object();
  effectivePolicy["schema"] = "ndnsf-di-conversation-handoff-policy-v1";
  effectivePolicy["enabled"] = policy.enabled;
  effectivePolicy["maxConcurrentEvents"] = policy.maxConcurrentEvents;
  effectivePolicy["maxRolesPerEvent"] = policy.maxRolesPerEvent;
  effectivePolicy["maxConcurrentRoleSlots"] = policy.maxConcurrentRoleSlots;
  effectivePolicy["maxPacketCountPerRole"] = policy.maxPacketCountPerRole;
  effectivePolicy["maxPacketSetWireBytesPerRole"] = policy.maxPacketSetWireBytesPerRole;
  effectivePolicy["maxAggregateBytes"] = policy.maxAggregateBytes;
  effectivePolicy["maxScratchBytesPerEvent"] = policy.maxScratchBytesPerEvent;
  effectivePolicy["scratchQuotaBytes"] = policy.scratchQuotaBytes;
  effectivePolicy["maxSaveDurationMs"] = policy.maxSaveDurationMs;
  effectivePolicy["scratchRoot"] = policy.scratchRoot.generic_string();
  effectivePolicy["ownerIdentity"] = authenticatedOwner.toUri(ndn::name::UriFormat::ALTERNATE);
  effectivePolicy["serviceName"] = serviceName.toUri(ndn::name::UriFormat::ALTERNATE);
  effectivePolicy["ownerCertificate"] =
    context.ownerEncryptionCertificate.getName().toUri(ndn::name::UriFormat::ALTERNATE);
  const auto& ownerCertificateWire = context.ownerEncryptionCertificate.wireEncode();
  effectivePolicy["ownerCertificateDigest"] = nativePlanningDigest(std::string(
    reinterpret_cast<const char*>(ownerCertificateWire.data()), ownerCertificateWire.size()));
  effectivePolicy["limits"] = NativeJson{
    {"saveBatchEntries", context.saveBatchLimits.maxEntries},
    {"saveBatchMetadataBytes", context.saveBatchLimits.maxMetadataBytes},
    {"portablePayloadBytes", fileLimits.first.portable.maxPayloadBytes},
    {"portableEncodedBytes", fileLimits.first.portable.maxEncodedBytes},
    {"portableMetadataBytes", fileLimits.first.maxMetadataBytes},
    {"portableCheckpointBytes", fileLimits.first.maxCheckpointBytes},
    {"protectedPlaintextBytes", fileLimits.second.maxPlaintextBytes},
    {"protectedWireBytes", fileLimits.second.maxWireBytes}};
  const auto& sourceCertificateWire = context.publication.signingCertificate.wireEncode();
  effectivePolicy["publication"] = NativeJson{
    {"producerIdentity", context.publication.signingCertificate.getIdentity().toUri(
      ndn::name::UriFormat::ALTERNATE)},
    {"producerCertificateDigest", nativePlanningDigest(std::string(
      reinterpret_cast<const char*>(sourceCertificateWire.data()), sourceCertificateWire.size()))},
    {"workspace", context.publication.workspace.generic_string()},
    {"maxStateBytes", context.publication.maxStateBytes}};
  effectivePolicy["repoTargets"] = NativeJson::object();
  for (const auto& [role, target] : context.repoTargetByRole) {
    effectivePolicy["repoTargets"][role] = NativeJson{
      {"repoNode", target.repoNode},
      {"providerIdentity", target.providerIdentity},
      {"servicePrefix", target.servicePrefix}};
  }
  context.policy.effectiveDigest = nativePlanningDigest(nativeCanonicalJson(effectivePolicy));
  return context;
}

NativeDurableStorageGrantBarrierV1::NativeDurableStorageGrantBarrierV1(
  std::string ownerIdentity, ProducerPrefixesByRole producerPrefixesByRole,
  Clock::time_point deadline)
  : m_ownerIdentity(std::move(ownerIdentity)),
    m_producerPrefixesByRole(std::move(producerPrefixesByRole)), m_deadline(deadline)
{
  const ndn::Name owner(m_ownerIdentity);
  if (owner.empty() || owner.toUri(ndn::name::UriFormat::ALTERNATE) != m_ownerIdentity ||
      m_producerPrefixesByRole.empty() || m_producerPrefixesByRole.size() > 64 ||
      m_deadline <= Clock::now()) {
    throw std::invalid_argument("DURABLE_GRANT_BATCH_BINDING_INVALID");
  }
  for (const auto& [role, producerPrefix] : m_producerPrefixesByRole) {
    if (role.empty() || role.size() > 4096 || producerPrefix.empty() ||
        producerPrefix.wireEncode().size() > 4096) {
      throw std::invalid_argument("DURABLE_GRANT_BATCH_BINDING_INVALID");
    }
  }
}

NativeDurableStorageGrantResolver
NativeDurableStorageGrantBarrierV1::resolverFor(const std::string& roleName)
{
  if (m_producerPrefixesByRole.count(roleName) != 1)
    throw std::invalid_argument("DURABLE_GRANT_BATCH_UNKNOWN_ROLE");
  auto self = weak_from_this().lock();
  if (!self)
    throw std::logic_error("DURABLE_GRANT_BATCH_REQUIRES_SHARED_OWNERSHIP");
  return [self = std::move(self), roleName](
           const ndn::Name& manifestName, const std::string& writerIdentity,
           const std::function<void()>& current) {
    return self->resolve(roleName, manifestName, writerIdentity, current);
  };
}

void
NativeDurableStorageGrantBarrierV1::waitUntil(
  std::unique_lock<std::mutex>& lock, const ReadyPredicate& ready,
  const std::function<void()>& current, const char* timeoutError)
{
  while (!ready() && !m_error) {
    const auto now = Clock::now();
    if (now >= m_deadline) {
      failLocked(std::make_exception_ptr(std::runtime_error(timeoutError)));
      m_condition.notify_all();
      break;
    }
    auto wakeAt = now + std::chrono::milliseconds(50);
    if (wakeAt > m_deadline) wakeAt = m_deadline;
    m_condition.wait_until(lock, wakeAt);
    if (ready() || m_error || !current)
      continue;

    lock.unlock();
    try {
      current();
    }
    catch (...) {
      const auto error = std::current_exception();
      abort(error);
      lock.lock();
      break;
    }
    lock.lock();
  }
  rethrowIfFailedLocked();
}

std::vector<NativeDurableStorageRoleGrantChallengeV1>
NativeDurableStorageGrantBarrierV1::waitForAllChallenges(
  const std::function<void()>& current)
{
  if (current) {
    try {
      current();
    }
    catch (...) {
      abortAndRethrow(std::current_exception());
    }
  }
  std::unique_lock<std::mutex> lock(m_mutex);
  waitUntil(lock, [this] {
    return m_challenges.size() == m_producerPrefixesByRole.size();
  }, current, "DURABLE_GRANT_BATCH_CHALLENGE_TIMEOUT");

  std::vector<NativeDurableStorageRoleGrantChallengeV1> result;
  result.reserve(m_challenges.size());
  for (const auto& [role, unused] : m_producerPrefixesByRole) {
    (void)unused;
    result.push_back(m_challenges.at(role));
  }
  return result;
}

void
NativeDurableStorageGrantBarrierV1::releaseAll(GrantSet grants)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  rethrowIfFailedLocked();
  const auto reject = [this, &lock](const char* reason) {
    const auto error = std::make_exception_ptr(std::runtime_error(reason));
    failLocked(error);
    m_condition.notify_all();
    lock.unlock();
    std::rethrow_exception(error);
  };
  if (m_released)
    reject("DURABLE_GRANT_BATCH_ALREADY_RELEASED");
  if (Clock::now() >= m_deadline)
    reject("DURABLE_GRANT_BATCH_AUTHORIZATION_TIMEOUT");
  if (m_challenges.size() != m_producerPrefixesByRole.size() ||
      grants.size() != m_producerPrefixesByRole.size()) {
    reject("DURABLE_GRANT_BATCH_INCOMPLETE");
  }
  SelectorSet selectors;
  for (const auto& [role, unusedPrefix] : m_producerPrefixesByRole) {
    (void)unusedPrefix;
    const auto challenge = m_challenges.find(role);
    const auto grant = grants.find(role);
    if (challenge == m_challenges.end() || grant == grants.end() ||
        grant->second.writerIdentity != challenge->second.writerIdentity ||
        grant->second.ownerIdentity != m_ownerIdentity ||
        grant->second.grantId.empty() || grant->second.grantId.size() > 4096) {
      reject("DURABLE_GRANT_BATCH_MISMATCH");
    }
    bool validObjectName = false;
    bool validRepoNode = false;
    try {
      const ndn::Name objectName(grant->second.objectName);
      validObjectName = !objectName.empty() && objectName.wireEncode().size() <= 4096 &&
        objectName == challenge->second.manifestName;
      const ndn::Name repoNode(grant->second.repoNode);
      validRepoNode = !repoNode.empty() &&
        repoNode.wireEncode().size() <= 4096 &&
        repoNode.toUri(ndn::name::UriFormat::ALTERNATE) == grant->second.repoNode;
    }
    catch (const std::exception&) {
      validObjectName = false;
      validRepoNode = false;
    }
    if (!validObjectName || !validRepoNode) {
      reject("DURABLE_GRANT_BATCH_MISMATCH");
    }
    selectors.emplace(role, grant->second);
  }
  if (Clock::now() >= m_deadline)
    reject("DURABLE_GRANT_BATCH_AUTHORIZATION_TIMEOUT");
  m_grants = std::move(selectors);
  m_released = true;
  m_condition.notify_all();
}

void
NativeDurableStorageGrantBarrierV1::abort(std::exception_ptr error)
{
  if (!error)
    error = std::make_exception_ptr(std::runtime_error("DURABLE_GRANT_BATCH_ABORTED"));
  std::lock_guard<std::mutex> lock(m_mutex);
  failLocked(std::move(error));
  m_condition.notify_all();
}

void
NativeDurableStorageGrantBarrierV1::failLocked(std::exception_ptr error)
{
  if (!m_error)
    m_error = std::move(error);
}

void
NativeDurableStorageGrantBarrierV1::rethrowIfFailedLocked() const
{
  if (m_error)
    std::rethrow_exception(m_error);
}

[[noreturn]] void
NativeDurableStorageGrantBarrierV1::abortAndRethrow(std::exception_ptr error)
{
  abort(error);
  std::rethrow_exception(error);
}

NativeDurableStorageRoleGrantV1
NativeDurableStorageGrantBarrierV1::resolve(
  const std::string& roleName, const ndn::Name& manifestName,
  const std::string& writerIdentity, const std::function<void()>& current)
{
  if (current) {
    try {
      current();
    }
    catch (...) {
      abortAndRethrow(std::current_exception());
    }
  }
  const auto expected = m_producerPrefixesByRole.find(roleName);
  bool validWriter = false;
  try {
    const ndn::Name writer(writerIdentity);
    validWriter = !writer.empty() &&
      writer.toUri(ndn::name::UriFormat::ALTERNATE) == writerIdentity &&
      expected != m_producerPrefixesByRole.end() && writer == expected->second;
  }
  catch (const std::exception&) {
    validWriter = false;
  }
  if (!validWriter || manifestName.empty() ||
      manifestName.wireEncode().size() > 4096 ||
      !expected->second.isPrefixOf(manifestName) ||
      manifestName.size() <= expected->second.size()) {
    abortAndRethrow(std::make_exception_ptr(
      std::runtime_error("DURABLE_GRANT_BATCH_CHALLENGE_MISMATCH")));
  }

  std::unique_lock<std::mutex> lock(m_mutex);
  rethrowIfFailedLocked();
  if (m_released || m_challenges.count(roleName) != 0) {
    const auto error = std::make_exception_ptr(
      std::runtime_error("DURABLE_GRANT_BATCH_DUPLICATE_ROLE"));
    failLocked(error);
    m_condition.notify_all();
    lock.unlock();
    std::rethrow_exception(error);
  }
  const auto reusedObject = std::find_if(m_challenges.begin(), m_challenges.end(),
    [&manifestName](const auto& item) {
      return item.second.manifestName == manifestName;
    });
  if (reusedObject != m_challenges.end()) {
    const auto error = std::make_exception_ptr(
      std::runtime_error("DURABLE_GRANT_BATCH_DUPLICATE_OBJECT"));
    failLocked(error);
    m_condition.notify_all();
    lock.unlock();
    std::rethrow_exception(error);
  }
  m_challenges.emplace(roleName, NativeDurableStorageRoleGrantChallengeV1{
    roleName, manifestName, writerIdentity});
  m_condition.notify_all();
  waitUntil(lock, [this] { return m_released; }, current,
    "DURABLE_GRANT_BATCH_AUTHORIZATION_TIMEOUT");
  return m_grants.at(roleName);
}

void
NativeProtectedKvPublicationV1::requireCurrent() const
{
  if (!m_current)
    throw std::runtime_error("HANDOFF_PUBLICATION_CURRENT_GUARD_MISSING");
  m_current();
}

NativeJson
NativeProtectedKvPublicationV1::reference() const
{
  requireCurrent();
  return nativeSavedKvRoleToJson(m_saved);
}

NativeJson
nativeProtectedKvPublicationReference(const NativeProtectedKvPublicationV1& publication)
{
  return publication.reference();
}

} // namespace ndnsf::di
