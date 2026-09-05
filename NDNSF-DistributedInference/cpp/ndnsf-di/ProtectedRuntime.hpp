#ifndef NDNSF_DISTRIBUTED_INFERENCE_PROTECTED_RUNTIME_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_PROTECTED_RUNTIME_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantVerifier.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
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

/** Operator credentials and exact-name transport; payload verification is internal. */
struct NativeProtectedGrantConfig
{
  std::string authorityIdentity;
  std::string authorityPublicKeyRaw;
  NativeRecipientKey recipientKey;
  std::string modelManifestDigest;
  std::function<std::string(const std::string&)> fetchGrant;
  std::function<bool()> shouldCancel;
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
  // Passive wire field only (fixed value; no revocation ledger on this
  // branch — the revocation subsystem is owned by another branch, spec181
  // Out of Scope). Nothing on this machine's branches may act on it.
  std::uint64_t revocationSequence = 1;
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
 * becomes observable. Cancellation, completion, or destruction drains all
 * registered zeroizers; a cleanup failure leaves the runtime in FailedClosed
 * and never restores execution authority.
 *
 * Missing grant configuration fails closed with DI_PROTECTED_GRANT_UNAVAILABLE.
 * Only exact-name fetch, authority verification and recipient unwrap can
 * move the runtime to GrantVerified. The content key is owned by this runtime.
 * verifyBindingConsistency() performs only the structural consistency check;
 * it grants no execution authority (state stays NoGrant).
 */
class ProtectedRuntime
{
public:
  using Zeroizer = std::function<void()>;

  explicit ProtectedRuntime(ProtectedRuntimeBindingV1 expectedBinding,
                            std::optional<NativeProtectedGrantConfig> grantConfig = std::nullopt);
  ~ProtectedRuntime();

  // Verify the fetched payload against the sealed reference and configured issuer.
  void verifyGrant(const ProtectedRuntimeBindingV1& observedBinding,
                   std::uint64_t nowMs);
  void withContentKey(std::uint64_t nowMs,
                      const std::function<void(const std::vector<std::uint8_t>&)>& consume);
  // Binding structural consistency ONLY (no execution authority).
  void verifyBindingConsistency(const ProtectedRuntimeBindingV1& observedBinding,
                                std::uint64_t nowMs);
  void authorizeDataflow(ProtectedDataflowDirection direction,
                         const std::string& endpointDigest,
                         const std::string& producerRole,
                         const std::string& consumerRole,
                         std::uint64_t nowMs);
  void registerHostPlaintextLease(std::string leaseId, Zeroizer zeroizer);
  void registerDevicePlaintextLease(std::string leaseId, Zeroizer zeroizer);
  void cancel(std::string reason);
  void complete();

  ProtectedRuntimeState state() const noexcept;
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
  std::optional<NativeProtectedGrantConfig> m_grantConfig;
  std::vector<std::uint8_t> m_contentKey;
  std::uint64_t m_grantExpiresAtMs = 0;
  mutable std::mutex m_mutex;
  ProtectedRuntimeState m_state = ProtectedRuntimeState::NoGrant;
  std::string m_terminalReason;
  std::vector<Lease> m_hostLeases;
  std::vector<Lease> m_deviceLeases;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_PROTECTED_RUNTIME_HPP
