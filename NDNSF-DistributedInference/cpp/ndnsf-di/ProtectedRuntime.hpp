#ifndef NDNSF_DISTRIBUTED_INFERENCE_PROTECTED_RUNTIME_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_PROTECTED_RUNTIME_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantVerifier.hpp"

#include <cstdint>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ndnsf::di {

class ProtectedRuntime;

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

/** Non-secret, stable identity of the protected key used for one artifact. */
struct NativeProtectedKeyReferenceV1
{
  std::string authorityIdentity;
  std::string providerIdentity;
  std::string modelManifestDigest;
  std::string protectionEpoch;
  std::string keyId;

  void validate() const;
  std::string canonical() const;
  std::string digest() const;
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
  // Grant wire identity may be a conversation lease while the surrounding
  // execution binding remains turn-scoped.
  std::string grantRequestId;
  std::uint64_t grantAttempt = 0;
  std::string grantPlanCoreDigest;
  std::uint64_t grantExpiresAtMs = 0;
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
 * Stable, non-secret identity for one provider-owned protected resident.
 *
 * Request id, attempt and expiry are deliberately absent.  The full
 * canonical form retains the current request/grant binding for authorization
 * checks, while cacheKey() excludes request-local plan/grant digests so a
 * valid next turn can reuse the same model session.  The resident never owns
 * a ProtectedRuntime or a content key.
 */
struct ProtectedResidentIdentityV1
{
  std::string provider;
  std::string providerBootId;
  std::string role;
  std::string modelManifestDigest;
  std::string graphDigest;
  std::string initializerDigest;
  std::string artifactDigest;
  std::string recipeDigest;
  std::string backend;
  std::string backendAbi;
  std::string protectionEpoch;
  std::string planCoreDigest;
  std::string planDigest;
  std::string securityPolicySnapshotDigest;
  std::string grantDigest;
  std::string fencingToken;
  std::uint64_t revocationSequence = 1;

  void validate() const;
  std::string canonical() const;
  std::string cacheKey() const;
  std::string cacheScopeKey() const;
};

/**
 * Provider-owned authorization gate for protected resident sessions.
 *
 * This authority stores only identity and active-use counts.  It never stores
 * model bytes, content keys, or a request-scoped ProtectedRuntime.  Retiring
 * an identity prevents new uses and invokes the optional cache-eviction
 * callback; existing uses must drain before the authority entry disappears.
 */
class ProtectedResidentAuthority
{
public:
  struct Counters
  {
    std::uint64_t activeUses = 0;
    std::uint64_t residentEntries = 0;
  };

  struct Shared;

  class Use
  {
  public:
    Use() noexcept = default;
    ~Use() noexcept;
    Use(const Use&) = delete;
    Use& operator=(const Use&) = delete;
    Use(Use&& other) noexcept;
    Use& operator=(Use&& other) noexcept;

    bool valid() const noexcept { return static_cast<bool>(m_release); }
    explicit operator bool() const noexcept { return valid(); }
    const std::string& identity() const noexcept { return m_identity; }

  private:
    struct Release;
    Use(std::shared_ptr<Release> release, std::string identity) noexcept;
    std::shared_ptr<Release> m_release;
    std::string m_identity;
    friend class ProtectedResidentAuthority;
  };

  ProtectedResidentAuthority();
  ~ProtectedResidentAuthority() noexcept;
  ProtectedResidentAuthority(const ProtectedResidentAuthority&) = delete;
  ProtectedResidentAuthority& operator=(const ProtectedResidentAuthority&) = delete;

  Use acquire(const ProtectedResidentIdentityV1& identity,
              const ProtectedRuntime& runtime,
              std::uint64_t nowMs);
  void retire(const ProtectedResidentIdentityV1& identity) noexcept;
  void retireAll() noexcept;
  void setRetireCallback(std::function<void(const std::string&)> callback) noexcept;
  bool drain(std::chrono::milliseconds timeout);
  Counters counters() const noexcept;

private:
  std::shared_ptr<Shared> m_shared;
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
  std::optional<NativeProtectedKeyReferenceV1> keyReference() const;

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
  std::optional<NativeProtectedKeyReferenceV1> m_keyReference;
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
