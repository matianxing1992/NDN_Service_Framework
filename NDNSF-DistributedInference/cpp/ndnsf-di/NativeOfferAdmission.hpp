#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeObservedOfferV3.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
namespace ndn_service_framework { struct AckSelectionCandidate; }
namespace ndnsf::di {
struct NativeOfferBindingContext {
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string serviceName, modelDigest, graphDigest;
  std::uint64_t deadlineMs = 0;
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
/** Immutable candidate policy/public-key registry; Core owns ACK Trust Schema. */
class NativeOfferAdmission {
public:
  NativeOfferAdmission(const std::string& policyJson,
                       const std::map<std::string, std::string>& publicKeyPemById,
                       const std::string& candidateDigest);
  NativeAdmittedOfferV3 verify(const ndn_service_framework::AckSelectionCandidate& ack,
                              const NativeOfferBindingContext& context, std::uint64_t nowMs) const;
private:
  struct Entry { std::string keyLocatorPrefix, signerKeyId; };
  std::map<std::pair<std::string, std::string>, Entry> m_entries;
  std::map<std::string, std::array<unsigned char, 32>> m_keys;
};
} // namespace ndnsf::di
