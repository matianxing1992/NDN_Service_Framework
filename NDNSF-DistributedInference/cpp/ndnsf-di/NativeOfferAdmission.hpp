#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeObservedOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeObservedOfferV4.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace ndn_service_framework { struct AckSelectionCandidate; }
namespace ndnsf::di {
class NativePreparedPlanningTemplate;
struct NativeOfferBindingContext {
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string serviceName, modelDigest, graphDigest;
  std::uint64_t deadlineMs = 0;
  // Local, authenticated conversation placement preference, never admission
  // authority or evidence that the Provider still retains usable KV state.
  std::map<std::string, std::string> preferredProvidersByRole;
  std::string candidateSetDigest;
  std::string candidateArtifactSetDigest;
  std::vector<NativeJson> candidateArtifacts;
  std::vector<std::string> candidateProviders;
  std::string probeScopeDigest;
  std::vector<NativeJson> allowedRttPairs;
  std::vector<NativeJson> allowedGoodputEdges;
  // Present only when every Provider in the authenticated request roster has
  // supplied a valid V4 offer. The planner freezes this exact ACK snapshot;
  // a placement strategy may consume it but cannot replace it.
  NativeJson placementObservationSnapshot;
  std::string placementObservationSnapshotDigest;
  /** Local planner reason when an otherwise optional V4 snapshot is unavailable. */
  std::string placementObservationFallbackReason;
  /** True after the one allowed targeted placement-probe round for this attempt. */
  bool placementProbeRoundCompleted = false;
  // Configured requester identity, checked against Core's authenticated ACK
  // request context before Provider probe authorization.
  std::string requesterIdentity;
  // Local immutable factory owner used only to reuse canonical snapshot
  // subtrees. Actual snapshot fields are checked before using its bytes.
  std::shared_ptr<const NativePreparedPlanningTemplate> preparedPlanningTemplate;
};
/** Only the admission owner constructs verified observations; no lease authority. */
class NativeAdmittedOfferV3 {
public:
  const NativeObservedProviderOfferV3& observation() const noexcept { return m_observation; }
private:
  friend class NativeOfferAdmission;
  explicit NativeAdmittedOfferV3(NativeObservedProviderOfferV3 value) : m_observation(std::move(value)) {}
  NativeObservedProviderOfferV3 m_observation;
};
class NativeAdmittedOfferV4 {
public:
  const NativeObservedProviderOfferV4& observation() const noexcept { return m_observation; }
  const std::string& signerIdentity() const noexcept { return m_signerIdentity; }
  const std::string& signerKeyLocator() const noexcept { return m_signerKeyLocator; }
  const std::string& ackWireDigest() const noexcept { return m_ackWireDigest; }
  /** Zero means absent/invalid. Bound to this admitted ACK and Provider boot. */
  std::uint64_t tensorSegmentBytes() const noexcept { return m_tensorSegmentBytes; }
private:
  friend class NativeOfferAdmission;
  NativeAdmittedOfferV4(NativeObservedProviderOfferV4 value,
                        std::string signerIdentity,
                        std::string signerKeyLocator,
                        std::string ackWireDigest,
                        std::uint64_t tensorSegmentBytes)
    : m_observation(std::move(value))
    , m_signerIdentity(std::move(signerIdentity))
    , m_signerKeyLocator(std::move(signerKeyLocator))
    , m_ackWireDigest(std::move(ackWireDigest))
    , m_tensorSegmentBytes(tensorSegmentBytes) {}
  NativeObservedProviderOfferV4 m_observation;
  std::string m_signerIdentity;
  std::string m_signerKeyLocator;
  std::string m_ackWireDigest;
  std::uint64_t m_tensorSegmentBytes = 0;
};
/** Data-only decoding owner for one ACK-closed planning continuation.
 * Exact wire bytes are owned; no admission or freshness verdict is retained.
 */
class NativeOfferDecodeCache {
public:
  std::size_t decodeReuseCount() const;
private:
  friend class NativeOfferAdmission;
  std::shared_ptr<const NativeObservedProviderOfferV4> decode(const std::string& wire);
  mutable std::mutex m_mutex;
  std::vector<std::pair<std::string,
    std::shared_ptr<const NativeObservedProviderOfferV4>>> m_entries;
  std::size_t m_reuses = 0;
};
/** Immutable candidate policy/public-key registry; Core owns ACK Trust Schema. */
class NativeOfferAdmission {
public:
  NativeOfferAdmission(const std::string& policyJson,
                       const std::map<std::string, std::string>& publicKeyPemById,
                       const std::string& candidateDigest);
  NativeAdmittedOfferV3 verify(const ndn_service_framework::AckSelectionCandidate& ack,
                              const NativeOfferBindingContext& context, std::uint64_t nowMs) const;
  NativeAdmittedOfferV4 verifyV4(
      const ndn_service_framework::AckSelectionCandidate& ack,
      const NativeOfferBindingContext& context, std::uint64_t nowMs) const;
  /** Batch admission with common context checks reused only in this call.
   * Each ACK retains independent authentication, signature and lifetime checks.
   * The snapshot caller checks the complete, unique ACK roster separately.
   * Failure returns nullopt and the same missing/invalid reason as the planner.
   */
  std::optional<std::vector<NativeAdmittedOfferV4>> tryVerifyV4Batch(
      const std::vector<ndn_service_framework::AckSelectionCandidate>& acks,
      const NativeOfferBindingContext& context, std::uint64_t nowMs,
      std::string* unavailableReason = nullptr) const;
  std::optional<std::vector<NativeAdmittedOfferV4>> tryVerifyV4Batch(
      const std::vector<ndn_service_framework::AckSelectionCandidate>& acks,
      const NativeOfferBindingContext& context, std::uint64_t nowMs,
      std::string* unavailableReason, NativeOfferDecodeCache* decodeCache) const;
private:
  struct V4ContextChecks;
  NativeAdmittedOfferV4 verifyV4Impl(
      const ndn_service_framework::AckSelectionCandidate& ack,
      const NativeOfferBindingContext& context, std::uint64_t nowMs,
      V4ContextChecks* checks, NativeOfferDecodeCache* decodeCache = nullptr) const;
  struct Entry { std::string keyLocatorPrefix, signerKeyId; };
  std::map<std::pair<std::string, std::string>, Entry> m_entries;
  std::map<std::string, std::array<unsigned char, 32>> m_keys;
};
} // namespace ndnsf::di
