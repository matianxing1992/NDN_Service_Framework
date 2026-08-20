#ifndef NDNSF_DISTRIBUTED_INFERENCE_PROTECTED_RUNTIME_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_PROTECTED_RUNTIME_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ndnsf::di {

enum class ProtectedRuntimeState
{
  NoGrant,
  GrantVerified,
  HostPlaintextLeased,
  DevicePlaintextLeased,
  Draining,
  Zeroized,
  FailedClosed,
};

enum class ProtectedDataflowDirection
{
  Publish,
  Fetch,
};

/** Exact, non-secret authorization binding for one protected execution role. */
struct ProtectedRuntimeBindingV1
{
  std::string provider;
  std::string role;
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string planCoreDigest;
  std::string planDigest;
  std::string securityPolicySnapshotDigest;
  std::string protectionEpoch;
  std::string grantName;
  std::string grantDigest;
  std::string capabilityDigest;
  std::string groupId;
  std::uint64_t groupEpoch = 0;
  std::string epochKeyId;
  std::string providerBootId;
  std::string fencingToken;
  std::uint64_t revocationSequence = 0;
  std::uint64_t expiresAtMs = 0;
  std::set<std::string> mayPublishEndpointDigests;
  std::set<std::string> mustFetchEndpointDigests;
  std::map<std::string, std::string> mayPublishConsumerByEndpoint;
  std::map<std::string, std::string> mustFetchProducerByEndpoint;

  void validate() const;
  bool exactlyMatches(const ProtectedRuntimeBindingV1& other) const noexcept;
};

/**
 * Fail-closed plaintext lease owner for one Provider/role/attempt.
 *
 * Callers must register every host or device plaintext allocation before it
 * becomes observable. Cancellation, revocation, completion, or destruction
 * drains all registered zeroizers; a cleanup failure leaves the runtime in
 * FailedClosed and never restores execution authority.
 */
class ProtectedRuntime
{
public:
  using Zeroizer = std::function<void()>;

  explicit ProtectedRuntime(ProtectedRuntimeBindingV1 expectedBinding);
  ~ProtectedRuntime();

  void verifyGrant(const ProtectedRuntimeBindingV1& observedBinding,
                   std::uint64_t nowMs);
  void authorizeDataflow(ProtectedDataflowDirection direction,
                         const std::string& endpointDigest,
                         const std::string& producerRole,
                         const std::string& consumerRole,
                         std::uint64_t nowMs) const;
  void registerHostPlaintextLease(std::string leaseId, Zeroizer zeroizer);
  void registerDevicePlaintextLease(std::string leaseId, Zeroizer zeroizer);
  void revoke(std::uint64_t revocationSequence, std::string reason);
  void cancel(std::string reason);
  void complete();

  ProtectedRuntimeState state() const noexcept;
  bool revoked() const noexcept;
  const std::string& terminalReason() const noexcept;
  const ProtectedRuntimeBindingV1& binding() const noexcept;

private:
  struct Lease
  {
    std::string id;
    Zeroizer zeroize;
  };

  void registerLease(std::vector<Lease>& leases,
                     ProtectedRuntimeState nextState,
                     std::string leaseId,
                     Zeroizer zeroizer);
  void drainLocked();
  bool authorizedStateLocked() const noexcept;

private:
  ProtectedRuntimeBindingV1 m_binding;
  mutable std::mutex m_mutex;
  ProtectedRuntimeState m_state = ProtectedRuntimeState::NoGrant;
  bool m_revoked = false;
  std::string m_terminalReason;
  std::vector<Lease> m_hostLeases;
  std::vector<Lease> m_deviceLeases;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_PROTECTED_RUNTIME_HPP
