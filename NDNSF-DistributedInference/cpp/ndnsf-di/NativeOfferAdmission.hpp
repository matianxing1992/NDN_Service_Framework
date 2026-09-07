#ifndef NDNSF_DI_NATIVE_OFFER_ADMISSION_HPP
#define NDNSF_DI_NATIVE_OFFER_ADMISSION_HPP

// T008-B: authenticated offer admission, split out of NativeRequestPreparation.
// verify() is the single gate through which a Core-authenticated ACK may
// become an immutable provider planning view.  The class carries no Trust
// Schema of its own, accepts no caller trust flag and no verifier callback:
// provenance must already be established by the Core subscription path and is
// recorded in the evidence before any DI offer policy is consulted.

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf::di {

// Evidence of an ACK that already passed Core packet authentication.  The
// signer identity/key locator/wire digest mirror the Core
// AckAuthenticationEvidence shape (ServiceUser.hpp); trustSchemaValidated is
// true only for an ACK delivered through the validated ServiceUser
// subscription path.  Direct/unit fixtures and caller-declared "verified"
// claims leave it false.  The remaining fields are the authenticated DI
// binding extracted from the same ACK (request/attempt/provider/service and
// the certified offer identity).
struct NativeAckEvidence
{
  bool trustSchemaValidated = false;
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string provider;
  std::string serviceName;
  std::string signerIdentity;
  std::string signerKeyLocator;
  std::string wireDigest;
  std::string controllerVersion;
  std::string offerDigest;
  std::string modelDigest;
  std::string graphDigest;
  std::uint64_t capturedAtMs = 0;
  std::uint64_t expiresAtMs = 0;
};

// Immutable snapshot of the DI offer policy for one candidate, owned by the
// policy holder.  It never carries crypto keys and is never a second Trust
// Schema; it only constrains which Core-authenticated offers may plan.
struct NativeOfferPolicySnapshot
{
  std::string policyDigest;
  std::vector<std::string> acceptedSignerIdentities;
  std::vector<std::string> acceptedProviders;
  std::vector<std::string> acceptedServices;
  std::vector<std::string> acceptedRoles;
  std::vector<std::string> backends;
  std::vector<std::string> residencyDigests;
  std::uint64_t freeBytes = 0;
  std::uint64_t resourceSequence = 0;
  std::uint64_t expiresAtMs = 0;
};

struct NativeOfferBindingContext
{
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string serviceName;
  std::string modelDigest;
  std::string graphDigest;
};

class NativeOfferAdmission
{
public:
  NativeProviderPlanningView verify(const NativeAckEvidence& ack,
                                    const NativeOfferPolicySnapshot& policy,
                                    const NativeOfferBindingContext& context,
                                    std::uint64_t nowMs) const;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_OFFER_ADMISSION_HPP
