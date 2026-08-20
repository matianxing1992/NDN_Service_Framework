#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

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
      !isDigest(securityPolicySnapshotDigest) || !isDigest(grantDigest) ||
      (hasGroupBinding && (groupId.empty() || groupEpoch == 0 ||
                           !isDigest(capabilityDigest) ||
                           !isDigest(epochKeyId)))) {
    throw std::invalid_argument("protected runtime binding is incomplete");
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

ProtectedRuntime::ProtectedRuntime(ProtectedRuntimeBindingV1 expectedBinding)
  : m_binding(std::move(expectedBinding))
{
  m_binding.validate();
}

ProtectedRuntime::~ProtectedRuntime()
{
  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != ProtectedRuntimeState::Zeroized &&
        m_state != ProtectedRuntimeState::FailedClosed) {
      m_terminalReason = "protected runtime destroyed";
      drainLocked();
    }
  }
  catch (...) {
    // Destructors cannot propagate; drainLocked already records FailedClosed.
  }
}

void
ProtectedRuntime::verifyGrant(const ProtectedRuntimeBindingV1& observedBinding,
                              std::uint64_t nowMs)
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
  if (m_state != ProtectedRuntimeState::NoGrant ||
      !m_binding.exactlyMatches(observedBinding) || nowMs >= m_binding.expiresAtMs) {
    m_state = ProtectedRuntimeState::FailedClosed;
    m_terminalReason = "protected grant binding mismatch or expiry";
    throw std::runtime_error(m_terminalReason);
  }
  m_state = ProtectedRuntimeState::GrantVerified;
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
                                    std::uint64_t nowMs) const
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
  if (!authorizedStateLocked() || m_revoked || nowMs >= m_binding.expiresAtMs ||
      !isDigest(endpointDigest) || allowed.count(endpointDigest) != 1 ||
      !ownsRole || !peerMatches || producerRole.empty() || consumerRole.empty()) {
    throw std::runtime_error(
      "protected dataflow is not authorized for this role/endpoint");
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
  const auto drain = [&failed] (auto& leases) {
    for (auto it = leases.rbegin(); it != leases.rend(); ++it) {
      try {
        it->zeroize();
      }
      catch (...) {
        failed = true;
      }
    }
    leases.clear();
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
ProtectedRuntime::revoke(std::uint64_t revocationSequence, std::string reason)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (revocationSequence <= m_binding.revocationSequence) {
    throw std::invalid_argument("stale protected grant revocation sequence");
  }
  m_revoked = true;
  m_terminalReason = std::move(reason);
  drainLocked();
}

void
ProtectedRuntime::cancel(std::string reason)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == ProtectedRuntimeState::Zeroized ||
      m_state == ProtectedRuntimeState::FailedClosed) {
    return;
  }
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

bool
ProtectedRuntime::revoked() const noexcept
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_revoked;
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
