#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeObservedOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeObservedOfferV4.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
namespace ndn_service_framework { struct AckSelectionCandidate; }
namespace ndnsf::di {
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
private:
  friend class NativeOfferAdmission;
  NativeAdmittedOfferV4(NativeObservedProviderOfferV4 value,
                        std::string signerIdentity,
                        std::string signerKeyLocator,
                        std::string ackWireDigest)
    : m_observation(std::move(value))
    , m_signerIdentity(std::move(signerIdentity))
    , m_signerKeyLocator(std::move(signerKeyLocator))
    , m_ackWireDigest(std::move(ackWireDigest)) {}
  NativeObservedProviderOfferV4 m_observation;
  std::string m_signerIdentity;
  std::string m_signerKeyLocator;
  std::string m_ackWireDigest;
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
private:
  struct Entry { std::string keyLocatorPrefix, signerKeyId; };
  std::map<std::pair<std::string, std::string>, Entry> m_entries;
  std::map<std::string, std::array<unsigned char, 32>> m_keys;
};
} // namespace ndnsf::di
