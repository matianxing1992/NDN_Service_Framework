#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>
#include <chrono>
#include <openssl/crypto.h>

namespace ndnsf::di {
namespace {

bool
isGroupDigest(const std::string& value)
{
  // GroupCapabilityV1's existing wire contract uses unprefixed SHA-256 hex.
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [] (unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool
isDigest(const std::string& value)
{
  if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) {
    return false;
  }
  return std::all_of(value.begin() + 7, value.end(), [] (unsigned char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
  });
}

void
requireText(const std::string& value, const char* field)
{
  if (value.empty() || value.size() > 4096) {
    throw std::invalid_argument(std::string("missing or oversized ") + field);
  }
}

} // namespace

void
ProtectedRuntimeBindingV1::validate() const
{
  requireText(provider, "protected.provider");
  requireText(role, "protected.role");
  requireText(requestId, "protected.requestId");
  requireText(protectionEpoch, "protected.protectionEpoch");
  requireText(grantName, "protected.grantName");
  requireText(providerBootId, "protected.providerBootId");
  requireText(fencingToken, "protected.fencingToken");
  const bool hasGroupBinding = !capabilityDigest.empty() || !groupId.empty() ||
                               groupEpoch != 0 || !epochKeyId.empty();
  if (grantName.front() != '/' || protectionEpoch == "plaintext-v1" ||
      attempt == 0 || expiresAtMs == 0 ||
      !isDigest(planCoreDigest) || !isDigest(planDigest) ||
      !isDigest(securityPolicySnapshotDigest) || !isDigest(grantDigest)) {
    throw std::invalid_argument("protected runtime binding is incomplete");
  }
  if (hasGroupBinding && (groupId.empty() || groupEpoch == 0 ||
                         !isGroupDigest(capabilityDigest) ||
                         !isGroupDigest(epochKeyId))) {
    throw std::invalid_argument("protected runtime group binding is invalid");
  }
  const auto validateEndpoints = [] (const auto& endpoints) {
    return std::all_of(endpoints.begin(), endpoints.end(), [] (const auto& item) {
      return isDigest(item);
    });
  };
  if (!validateEndpoints(mayPublishEndpointDigests) ||
      !validateEndpoints(mustFetchEndpointDigests) ||
      mayPublishConsumerByEndpoint.size() != mayPublishEndpointDigests.size() ||
      mustFetchProducerByEndpoint.size() != mustFetchEndpointDigests.size() ||
      !std::all_of(mayPublishConsumerByEndpoint.begin(),
                   mayPublishConsumerByEndpoint.end(), [&] (const auto& item) {
                     return mayPublishEndpointDigests.count(item.first) == 1 &&
                            !item.second.empty();
                   }) ||
      !std::all_of(mustFetchProducerByEndpoint.begin(),
                   mustFetchProducerByEndpoint.end(), [&] (const auto& item) {
                     return mustFetchEndpointDigests.count(item.first) == 1 &&
                            !item.second.empty();
                   })) {
    throw std::invalid_argument("protected runtime endpoint binding is invalid");
  }
}

bool
ProtectedRuntimeBindingV1::exactlyMatches(
  const ProtectedRuntimeBindingV1& other) const noexcept
{
  return provider == other.provider && role == other.role &&
         requestId == other.requestId && attempt == other.attempt &&
         planCoreDigest == other.planCoreDigest &&
         planDigest == other.planDigest &&
         securityPolicySnapshotDigest == other.securityPolicySnapshotDigest &&
         protectionEpoch == other.protectionEpoch &&
         grantName == other.grantName && grantDigest == other.grantDigest &&
         capabilityDigest == other.capabilityDigest && groupId == other.groupId &&
         groupEpoch == other.groupEpoch && epochKeyId == other.epochKeyId &&
         providerBootId == other.providerBootId &&
         fencingToken == other.fencingToken &&
         revocationSequence == other.revocationSequence &&
         expiresAtMs == other.expiresAtMs &&
         mayPublishEndpointDigests == other.mayPublishEndpointDigests &&
         mustFetchEndpointDigests == other.mustFetchEndpointDigests &&
         mayPublishConsumerByEndpoint == other.mayPublishConsumerByEndpoint &&
         mustFetchProducerByEndpoint == other.mustFetchProducerByEndpoint;
}

ProtectedRuntime::ProtectedRuntime(ProtectedRuntimeBindingV1 expectedBinding,
                                   std::optional<NativeProtectedGrantConfig> grantConfig)
  : m_binding(std::move(expectedBinding))
  , m_grantConfig(std::move(grantConfig))
{
  m_binding.validate();
}

ProtectedRuntime::~ProtectedRuntime()
{
  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != ProtectedRuntimeState::Zeroized) {
      m_terminalReason = "protected runtime destroyed";
      drainLocked();
    }
  }
  catch (...) {
    // Destructors cannot propagate; drainLocked already records FailedClosed.
  }
  OPENSSL_cleanse(m_contentKey.data(), m_contentKey.size());
  if (m_grantConfig) {
    auto& material = m_grantConfig->recipientKey.material;
    OPENSSL_cleanse(material.data(), material.size());
  }
}

void
ProtectedRuntime::verifyGrant(const ProtectedRuntimeBindingV1& observedBinding,
                              std::uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  try {
    const auto started = std::chrono::steady_clock::now();
    if (!m_grantConfig || !m_grantConfig->fetchGrant ||
        m_grantConfig->authorityIdentity.empty() ||
        m_grantConfig->authorityPublicKeyRaw.size() != 32) {
      throw std::runtime_error("DI_PROTECTED_GRANT_UNAVAILABLE: native grant configuration is missing");
    }
    observedBinding.validate();
    if (m_state != ProtectedRuntimeState::NoGrant ||
        !m_binding.exactlyMatches(observedBinding) || nowMs >= m_binding.expiresAtMs) {
      throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: sealed binding mismatch or expiry");
    }
    const auto& config = *m_grantConfig;
    if (config.shouldCancel && config.shouldCancel()) {
      throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: grant acquisition cancelled");
    }
    const auto separator = m_binding.grantName.find("/NDNSF-DI/KEY-GRANT/v1/");
    if (separator == std::string::npos || m_binding.grantName != canonicalNativeGrantName(
          m_binding.grantName.substr(0, separator), m_binding.provider,
          m_binding.requestId, m_binding.attempt, m_binding.planCoreDigest,
          config.modelManifestDigest, m_binding.protectionEpoch, m_binding.grantDigest)) {
      throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: canonical grant name mismatch");
    }
    auto result = verifyAndUnwrapNativeGrant(
      config.fetchGrant(m_binding.grantName), config.authorityPublicKeyRaw,
      config.recipientKey, m_binding.provider, m_binding.requestId, m_binding.attempt,
      m_binding.planCoreDigest, config.modelManifestDigest, m_binding.protectionEpoch,
      nowMs, config.authorityIdentity, m_binding.grantDigest);
    // Transfer the allocation before any later exception can release it unwiped.
    m_contentKey = std::move(result.contentKey);
    if (!result.verified || m_contentKey.size() != 32) {
      throw std::runtime_error(result.reason.empty()
        ? "DI_PROTECTED_GRANT_REJECTED: invalid content key" : result.reason);
    }
    if (std::find(result.allowedResidencyTiers.begin(), result.allowedResidencyTiers.end(),
                  "DISK_CIPHERTEXT_ASSEMBLED") == result.allowedResidencyTiers.end()) {
      throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: assembled residency is forbidden");
    }
    m_grantExpiresAtMs = std::min(m_binding.expiresAtMs, result.expiresAtMs);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started).count();
    if (nowMs >= m_grantExpiresAtMs ||
        static_cast<std::uint64_t>(elapsed) >= m_grantExpiresAtMs - nowMs ||
        (config.shouldCancel && config.shouldCancel())) {
      throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: grant acquisition expired or cancelled");
    }
    m_state = ProtectedRuntimeState::GrantVerified;
  }
  catch (const std::exception& error) {
    m_terminalReason = error.what();
    try { drainLocked(); } catch (...) {}
    m_state = ProtectedRuntimeState::FailedClosed;
    throw;
  }
}

void
ProtectedRuntime::withContentKey(
  std::uint64_t nowMs,
  const std::function<void(const std::vector<std::uint8_t>&)>& consume)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!authorizedStateLocked() || nowMs >= m_grantExpiresAtMs || !consume ||
      (m_grantConfig && m_grantConfig->shouldCancel && m_grantConfig->shouldCancel())) {
    m_terminalReason = "DI_PROTECTED_GRANT_REJECTED: content key lease is unavailable or expired";
    drainLocked();
    throw std::runtime_error(m_terminalReason);
  }
  try {
    consume(m_contentKey);
  }
  catch (...) {
    m_terminalReason = "DI_PROTECTED_GRANT_REJECTED: content key consumer failed";
    try { drainLocked(); } catch (...) {}
    m_state = ProtectedRuntimeState::FailedClosed;
    throw;
  }
}

void
ProtectedRuntime::verifyBindingConsistency(
  const ProtectedRuntimeBindingV1& observedBinding, std::uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  try {
    observedBinding.validate();
  }
  catch (...) {
    m_state = ProtectedRuntimeState::FailedClosed;
    m_terminalReason = "protected grant binding is invalid";
    throw;
  }
  // Structural consistency and expiry only. Execution authority is NOT
  // granted: state stays NoGrant unless a real grant verification (T002)
  // moves it to GrantVerified.
  if (m_state != ProtectedRuntimeState::NoGrant ||
      !m_binding.exactlyMatches(observedBinding) || nowMs >= m_binding.expiresAtMs) {
    m_state = ProtectedRuntimeState::FailedClosed;
    m_terminalReason = "protected grant binding mismatch or expiry";
    throw std::runtime_error(m_terminalReason);
  }
}

bool
ProtectedRuntime::authorizedStateLocked() const noexcept
{
  return m_state == ProtectedRuntimeState::GrantVerified ||
         m_state == ProtectedRuntimeState::HostPlaintextLeased ||
         m_state == ProtectedRuntimeState::DevicePlaintextLeased;
}

void
ProtectedRuntime::authorizeDataflow(ProtectedDataflowDirection direction,
                                    const std::string& endpointDigest,
                                    const std::string& producerRole,
                                    const std::string& consumerRole,
                                    std::uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto& allowed = direction == ProtectedDataflowDirection::Publish
    ? m_binding.mayPublishEndpointDigests
    : m_binding.mustFetchEndpointDigests;
  const bool ownsRole = direction == ProtectedDataflowDirection::Publish
    ? producerRole == m_binding.role
    : consumerRole == m_binding.role;
  const auto& peers = direction == ProtectedDataflowDirection::Publish
    ? m_binding.mayPublishConsumerByEndpoint
    : m_binding.mustFetchProducerByEndpoint;
  const auto peer = peers.find(endpointDigest);
  const bool peerMatches = peer != peers.end() &&
    (direction == ProtectedDataflowDirection::Publish
       ? peer->second == consumerRole
       : peer->second == producerRole);
  if (!authorizedStateLocked() || nowMs >= m_grantExpiresAtMs ||
      (m_grantConfig && m_grantConfig->shouldCancel && m_grantConfig->shouldCancel()) ||
      !isDigest(endpointDigest) || allowed.count(endpointDigest) != 1 ||
      !ownsRole || !peerMatches || producerRole.empty() || consumerRole.empty()) {
    m_terminalReason = "protected dataflow is not authorized for this role/endpoint";
    try { drainLocked(); } catch (...) {}
    m_state = ProtectedRuntimeState::FailedClosed;
    throw std::runtime_error(m_terminalReason);
  }
}

void
ProtectedRuntime::registerLease(std::vector<Lease>& leases,
                                ProtectedRuntimeState nextState,
                                std::string leaseId,
                                Zeroizer zeroizer)
{
  if (!authorizedStateLocked() || leaseId.empty() || !zeroizer ||
      std::any_of(m_hostLeases.begin(), m_hostLeases.end(),
                  [&] (const auto& item) { return item.id == leaseId; }) ||
      std::any_of(m_deviceLeases.begin(), m_deviceLeases.end(),
                  [&] (const auto& item) { return item.id == leaseId; })) {
    m_state = ProtectedRuntimeState::FailedClosed;
    m_terminalReason = "protected plaintext lease registration failed";
    throw std::runtime_error(m_terminalReason);
  }
  leases.push_back({std::move(leaseId), std::move(zeroizer)});
  m_state = nextState;
}

void
ProtectedRuntime::registerHostPlaintextLease(std::string leaseId,
                                             Zeroizer zeroizer)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  registerLease(m_hostLeases, ProtectedRuntimeState::HostPlaintextLeased,
                std::move(leaseId), std::move(zeroizer));
}

void
ProtectedRuntime::registerDevicePlaintextLease(std::string leaseId,
                                               Zeroizer zeroizer)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  registerLease(m_deviceLeases, ProtectedRuntimeState::DevicePlaintextLeased,
                std::move(leaseId), std::move(zeroizer));
}

void
ProtectedRuntime::drainLocked()
{
  m_state = ProtectedRuntimeState::Draining;
  bool failed = false;
  OPENSSL_cleanse(m_contentKey.data(), m_contentKey.size());
  m_contentKey.clear();
  const auto drain = [&failed] (auto& leases) {
    for (auto it = leases.rbegin(); it != leases.rend(); ++it) {
      try {
        it->zeroize();
        it->zeroize = {};
      }
      catch (...) {
        failed = true;
      }
    }
    leases.erase(std::remove_if(leases.begin(), leases.end(),
      [] (const auto& item) { return !item.zeroize; }), leases.end());
  };
  drain(m_deviceLeases);
  drain(m_hostLeases);
  m_state = failed ? ProtectedRuntimeState::FailedClosed
                   : ProtectedRuntimeState::Zeroized;
  if (failed) {
    throw std::runtime_error("protected plaintext zeroization failed");
  }
}

void
ProtectedRuntime::cancel(std::string reason)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == ProtectedRuntimeState::Zeroized) {
    return;
  }
  // FailedClosed still drains: cancel is the explicit cleanup path and must
  // best-effort zeroize any remaining plaintext (spec181 R002 unit contract).
  m_terminalReason = std::move(reason);
  drainLocked();
}

void
ProtectedRuntime::complete()
{
  cancel("protected role completed");
}

ProtectedRuntimeState
ProtectedRuntime::state() const noexcept
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_state;
}

const std::string&
ProtectedRuntime::terminalReason() const noexcept
{
  return m_terminalReason;
}

const ProtectedRuntimeBindingV1&
ProtectedRuntime::binding() const noexcept
{
  return m_binding;
}

} // namespace ndnsf::di
