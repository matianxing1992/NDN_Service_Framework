#pragma once
#include <ndn-cxx/name.hpp>
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderGroupCoordinator.hpp"

namespace ndnsf::di {
/** Immutable key offers extracted from the same Core-authenticated ACK as V3.
 * Group membership/operation authorization remains the sealed-plan owner's job.
 */
class NativeGroupKeyAdmission
{
public:
  struct RecipientKey {
    ndn::Name certificateName;
    ProviderGroupBytes publicKey;
    std::string publicKeyDigest;
  };
  NativeGroupKeyAdmission(const NativeOfferAdmission& admission,
    const std::vector<ndn_service_framework::AckSelectionCandidate>& acks,
    const NativeOfferBindingContext& context, std::uint64_t nowMs);
  const NativeAdmittedOfferV3& offer(const std::string& provider) const;
  const std::string& endpoint(const std::string& provider) const;
  // Immutable same-ACK key material; the admission object owns this view.
  const RecipientKey& recipient(const std::string& provider) const;
  ProviderGroupCoordinatorOptions options() const;
private:
  struct Entry {
    NativeAdmittedOfferV3 offer;
    std::string endpoint;
    RecipientKey recipient;
  };
  std::shared_ptr<const std::map<std::string, Entry>> m_entries;
};
} // namespace ndnsf::di
