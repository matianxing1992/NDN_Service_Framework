#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"

#include <ndn-cxx/util/sha256.hpp>
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <utility>
#include <chrono>
#include <openssl/crypto.h>
#include <boost/property_tree/json_parser.hpp>
#include <iostream>
#include <sstream>

namespace ndnsf::di {
namespace {

void
recordGrantVerification(const ProtectedRuntimeBindingV1& binding,
                        const char* status, const std::string& reason)
{
  boost::property_tree::ptree fields;
  fields.put("status", status);
  fields.put("boundary", "BEFORE_ASSEMBLY");
  fields.put("provider", binding.provider);
  fields.put("requestId", binding.requestId);
  fields.put("attemptId", "attempt-" + std::to_string(binding.attempt));
  fields.put("planCoreDigest", binding.planCoreDigest);
  fields.put("planDigest", binding.planDigest);
  fields.put("grantDigest", binding.grantDigest);
  fields.put("reason", reason);
  std::ostringstream record;
  record << "NDNSF_DI_GRANT_VERIFICATION ";
  boost::property_tree::write_json(record, fields, false);
  logRuntimeEvidence(record.str());
}

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
NativeProtectedKeyReferenceV1::validate() const
{
  requireText(authorityIdentity, "protected.keyReference.authorityIdentity");
  requireText(providerIdentity, "protected.keyReference.providerIdentity");
  requireText(protectionEpoch, "protected.keyReference.protectionEpoch");
  requireText(keyId, "protected.keyReference.keyId");
  if (!isDigest(modelManifestDigest)) {
    throw std::invalid_argument("protected.keyReference.modelManifestDigest is invalid");
  }
}

std::string
NativeProtectedKeyReferenceV1::canonical() const
{
  validate();
  const auto frame = [] (const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
  };
  return "NDNSF-DI/protected-key-reference/v1" + frame(authorityIdentity) +
         frame(providerIdentity) + frame(modelManifestDigest) +
         frame(protectionEpoch) + frame(keyId);
}

std::string
NativeProtectedKeyReferenceV1::digest() const
{
  ndn::util::Sha256 hash;
  const auto value = canonical();
  hash << value;
  auto hex = hash.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return "sha256:" + hex;
}

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
      recordGrantVerification(m_binding, "REJECTED", result.reason);
      throw std::runtime_error(result.reason.empty()
        ? "DI_PROTECTED_GRANT_REJECTED: invalid content key" : result.reason);
    }
    if (result.keyId.empty()) {
      throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: verified grant key identity is missing");
    }
    NativeProtectedKeyReferenceV1 keyReference{
      config.authorityIdentity, m_binding.provider,
      config.modelManifestDigest, m_binding.protectionEpoch, result.keyId};
    keyReference.validate();
    m_keyReference = std::move(keyReference);
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
    recordGrantVerification(m_binding, "VERIFIED", "");
    std::ostringstream verified;
    verified << "NDNSF_DI_GRANT_VERIFIED"
             << " requestId=" << m_binding.requestId
             << " attemptEpoch=" << m_binding.attempt
             << " provider=" << m_binding.provider
             << " planDigest=" << m_binding.planDigest;
    logRuntimeEvidence(verified.str());
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
    if (std::getenv("NDNSF_DI_PROTECTED_DATAFLOW_DIAGNOSTIC") != nullptr) {
      std::ostringstream record;
      record << "NDNSF_DI_PROTECTED_DATAFLOW_REJECT"
             << " direction=" << (direction == ProtectedDataflowDirection::Publish ? "publish" : "fetch")
             << " role=" << m_binding.role
             << " producer=" << producerRole
             << " consumer=" << consumerRole
             << " endpoint=" << endpointDigest
             << " state=" << static_cast<int>(m_state)
             << " authorized=" << (authorizedStateLocked() ? 1 : 0)
             << " expired=" << (nowMs >= m_grantExpiresAtMs ? 1 : 0)
             << " allowed=" << (allowed.count(endpointDigest) == 1 ? 1 : 0)
             << " owns_role=" << (ownsRole ? 1 : 0)
             << " peer_matches=" << (peerMatches ? 1 : 0)
             << " peer_present=" << (peer != peers.end() ? 1 : 0);
      logRuntimeEvidence(record.str());
    }
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
  m_keyReference.reset();
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

std::optional<NativeProtectedKeyReferenceV1>
ProtectedRuntime::keyReference() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!authorizedStateLocked() || !m_keyReference) {
    return std::nullopt;
  }
  return m_keyReference;
}

void
ProtectedResidentIdentityV1::validate() const
{
  const auto require = [] (const std::string& value, const char* field) {
    requireText(value, field);
  };
  require(provider, "protected.resident.provider");
  require(providerBootId, "protected.resident.providerBootId");
  require(role, "protected.resident.role");
  require(modelManifestDigest, "protected.resident.modelManifestDigest");
  require(graphDigest, "protected.resident.graphDigest");
  require(initializerDigest, "protected.resident.initializerDigest");
  require(artifactDigest, "protected.resident.artifactDigest");
  require(recipeDigest, "protected.resident.recipeDigest");
  require(backend, "protected.resident.backend");
  require(backendAbi, "protected.resident.backendAbi");
  require(protectionEpoch, "protected.resident.protectionEpoch");
  require(planCoreDigest, "protected.resident.planCoreDigest");
  require(planDigest, "protected.resident.planDigest");
  require(securityPolicySnapshotDigest,
          "protected.resident.securityPolicySnapshotDigest");
  require(grantDigest, "protected.resident.grantDigest");
  require(fencingToken, "protected.resident.fencingToken");
  if (protectionEpoch == "plaintext-v1" || revocationSequence == 0) {
    throw std::invalid_argument("protected resident identity is not protected");
  }
}

std::string
ProtectedResidentIdentityV1::canonical() const
{
  validate();
  std::ostringstream result;
  const auto frame = [&result] (const std::string& value) {
    result << value.size() << ':' << value;
  };
  frame("ndnsf-di-protected-resident-v1");
  frame(provider);
  frame(providerBootId);
  frame(role);
  frame(modelManifestDigest);
  frame(graphDigest);
  frame(initializerDigest);
  frame(artifactDigest);
  frame(recipeDigest);
  frame(backend);
  frame(backendAbi);
  frame(protectionEpoch);
  frame(planCoreDigest);
  frame(planDigest);
  frame(securityPolicySnapshotDigest);
  frame(grantDigest);
  frame(fencingToken);
  frame(std::to_string(revocationSequence));
  return result.str();
}

std::string
ProtectedResidentIdentityV1::cacheScopeKey() const
{
  validate();
  std::ostringstream result;
  const auto frame = [&result] (const std::string& value) {
    result << value.size() << ':' << value;
  };
  frame("ndnsf-di-protected-resident-scope-v1");
  frame(provider);
  frame(providerBootId);
  frame(role);
  frame(modelManifestDigest);
  frame(graphDigest);
  frame(initializerDigest);
  frame(artifactDigest);
  frame(recipeDigest);
  frame(backend);
  frame(backendAbi);
  frame(protectionEpoch);
  return result.str();
}

std::string
ProtectedResidentIdentityV1::cacheKey() const
{
  validate();
  std::ostringstream result;
  const auto frame = [&result] (const std::string& value) {
    result << value.size() << ':' << value;
  };
  frame("ndnsf-di-protected-resident-cache-v1");
  frame(cacheScopeKey());
  frame(securityPolicySnapshotDigest);
  frame(std::to_string(revocationSequence));
  return result.str();
}

struct ProtectedResidentAuthority::Use::Release
{
  std::shared_ptr<ProtectedResidentAuthority::Shared> shared;
  std::string identity;

  ~Release() noexcept;
};

struct ProtectedResidentAuthority::Shared
{
  struct Entry
  {
    std::size_t activeUses = 0;
    bool retired = false;
    std::string scopeKey;
  };

  mutable std::mutex mutex;
  std::condition_variable condition;
  std::map<std::string, Entry> entries;
  std::uint64_t activeUses = 0;
  std::function<void(const std::string&)> retireCallback;
};

ProtectedResidentAuthority::Use::Release::~Release() noexcept
{
  if (!shared) {
    return;
  }
  std::lock_guard<std::mutex> lock(shared->mutex);
  const auto found = shared->entries.find(identity);
  if (found != shared->entries.end()) {
    if (found->second.activeUses != 0) {
      --found->second.activeUses;
    }
    if (shared->activeUses != 0) {
      --shared->activeUses;
    }
    if (found->second.retired && found->second.activeUses == 0) {
      shared->entries.erase(found);
    }
  }
  shared->condition.notify_all();
}

namespace {

bool
protectedResidentStateAllowsUse(ProtectedRuntimeState state) noexcept
{
  return state == ProtectedRuntimeState::GrantVerified ||
         state == ProtectedRuntimeState::HostPlaintextLeased ||
         state == ProtectedRuntimeState::DevicePlaintextLeased;
}

void
requireResidentBinding(const ProtectedResidentIdentityV1& identity,
                       const ProtectedRuntime& runtime,
                       std::uint64_t nowMs)
{
  if (!protectedResidentStateAllowsUse(runtime.state())) {
    throw std::runtime_error(
      "DI_PROTECTED_RESIDENT_AUTHORITY_REJECTED: runtime is not authorized");
  }
  const auto& binding = runtime.binding();
  if (binding.expiresAtMs <= nowMs ||
      binding.provider != identity.provider ||
      binding.planCoreDigest != identity.planCoreDigest ||
      binding.planDigest != identity.planDigest ||
      binding.securityPolicySnapshotDigest != identity.securityPolicySnapshotDigest ||
      binding.protectionEpoch != identity.protectionEpoch ||
      binding.grantDigest != identity.grantDigest ||
      binding.providerBootId != identity.providerBootId ||
      binding.fencingToken != identity.fencingToken ||
      binding.revocationSequence != identity.revocationSequence) {
    throw std::runtime_error(
      "DI_PROTECTED_RESIDENT_AUTHORITY_REJECTED: binding identity mismatch");
  }
  const auto keyReference = runtime.keyReference();
  if (!keyReference || keyReference->providerIdentity != identity.provider ||
      keyReference->modelManifestDigest != identity.modelManifestDigest ||
      keyReference->protectionEpoch != identity.protectionEpoch) {
    throw std::runtime_error(
      "DI_PROTECTED_RESIDENT_AUTHORITY_REJECTED: key identity mismatch");
  }
}

} // namespace

ProtectedResidentAuthority::Use::Use(
  std::shared_ptr<Release> release, std::string identity) noexcept
  : m_release(std::move(release))
  , m_identity(std::move(identity))
{
}

ProtectedResidentAuthority::Use::~Use() noexcept = default;

ProtectedResidentAuthority::Use::Use(Use&& other) noexcept
  : m_release(std::move(other.m_release))
  , m_identity(std::move(other.m_identity))
{
}

ProtectedResidentAuthority::Use&
ProtectedResidentAuthority::Use::operator=(Use&& other) noexcept
{
  if (this == &other) {
    return *this;
  }
  m_release = std::move(other.m_release);
  m_identity = std::move(other.m_identity);
  return *this;
}

ProtectedResidentAuthority::ProtectedResidentAuthority()
  : m_shared(std::make_shared<Shared>())
{
}

ProtectedResidentAuthority::~ProtectedResidentAuthority() noexcept
{
  retireAll();
}

ProtectedResidentAuthority::Use
ProtectedResidentAuthority::acquire(
  const ProtectedResidentIdentityV1& identity,
  const ProtectedRuntime& runtime,
  std::uint64_t nowMs)
{
  identity.validate();
  requireResidentBinding(identity, runtime, nowMs);
  const auto key = identity.cacheKey();
  const auto scopeKey = identity.cacheScopeKey();
  const auto shared = m_shared;
  std::vector<std::string> evictedKeys;
  std::function<void(const std::string&)> callback;
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    callback = shared->retireCallback;
    for (auto it = shared->entries.begin(); it != shared->entries.end();) {
      if (it->first != key && it->second.scopeKey == scopeKey) {
        it->second.retired = true;
        evictedKeys.push_back(it->first);
        if (it->second.activeUses == 0) {
          it = shared->entries.erase(it);
          continue;
        }
      }
      ++it;
    }
    auto& entry = shared->entries[key];
    if (entry.retired) {
      throw std::runtime_error(
        "DI_PROTECTED_RESIDENT_AUTHORITY_REJECTED: identity is retired");
    }
    entry.scopeKey = scopeKey;
    ++entry.activeUses;
    ++shared->activeUses;
  }
  if (callback) {
    for (const auto& evictedKey : evictedKeys) {
      try {
        callback(evictedKey);
      }
      catch (...) {
      }
    }
  }
  auto release = std::make_shared<Use::Release>();
  release->shared = shared;
  release->identity = key;
  return Use(std::move(release), key);
}

void
ProtectedResidentAuthority::retire(
  const ProtectedResidentIdentityV1& identity) noexcept
{
  std::string key;
  std::string scopeKey;
  try {
    key = identity.cacheKey();
    scopeKey = identity.cacheScopeKey();
  }
  catch (...) {
    return;
  }
  std::vector<std::string> keys;
  std::function<void(const std::string&)> callback;
  bool notify = false;
  const auto shared = m_shared;
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    for (auto it = shared->entries.begin(); it != shared->entries.end();) {
      if (it->first == key || it->second.scopeKey == scopeKey) {
        it->second.retired = true;
        keys.push_back(it->first);
        if (it->second.activeUses == 0) {
          it = shared->entries.erase(it);
          continue;
        }
      }
      ++it;
    }
    callback = shared->retireCallback;
    notify = !keys.empty();
    shared->condition.notify_all();
  }
  if (notify && callback) {
    for (const auto& evictedKey : keys) {
      try {
        callback(evictedKey);
      }
      catch (...) {
      }
    }
  }
}

void
ProtectedResidentAuthority::retireAll() noexcept
{
  std::vector<std::string> keys;
  std::function<void(const std::string&)> callback;
  const auto shared = m_shared;
  {
    std::lock_guard<std::mutex> lock(shared->mutex);
    callback = shared->retireCallback;
    keys.reserve(shared->entries.size());
    for (auto it = shared->entries.begin(); it != shared->entries.end();) {
      it->second.retired = true;
      keys.push_back(it->first);
      if (it->second.activeUses == 0) {
        it = shared->entries.erase(it);
      }
      else {
        ++it;
      }
    }
    shared->condition.notify_all();
  }
  if (callback) {
    for (const auto& key : keys) {
      try {
        callback(key);
      }
      catch (...) {
      }
    }
  }
}

void
ProtectedResidentAuthority::setRetireCallback(
  std::function<void(const std::string&)> callback) noexcept
{
  const auto shared = m_shared;
  std::lock_guard<std::mutex> lock(shared->mutex);
  shared->retireCallback = std::move(callback);
}

bool
ProtectedResidentAuthority::drain(std::chrono::milliseconds timeout)
{
  if (timeout.count() < 0) {
    throw std::invalid_argument(
      "protected resident authority drain timeout is negative");
  }
  const auto shared = m_shared;
  std::unique_lock<std::mutex> lock(shared->mutex);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (shared->activeUses != 0) {
    if (shared->condition.wait_until(lock, deadline) == std::cv_status::timeout &&
        shared->activeUses != 0) {
      return false;
    }
  }
  return true;
}

ProtectedResidentAuthority::Counters
ProtectedResidentAuthority::counters() const noexcept
{
  const auto shared = m_shared;
  std::lock_guard<std::mutex> lock(shared->mutex);
  return Counters{shared->activeUses,
                  static_cast<std::uint64_t>(shared->entries.size())};
}

} // namespace ndnsf::di
