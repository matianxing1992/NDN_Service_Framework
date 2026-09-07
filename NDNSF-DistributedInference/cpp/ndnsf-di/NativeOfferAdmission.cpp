#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace ndnsf::di {
namespace {
bool digest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool contains(const std::vector<std::string>& values, const std::string& value)
{
  return std::find(values.begin(), values.end(), value) != values.end();
}

// NDN names that signer identity and key locator must take: absolute, with
// non-empty components and no control characters.
bool ndnName(const std::string& value)
{
  if (value.size() < 2 || value.front() != '/') return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto c = static_cast<unsigned char>(value[i]);
    if (c < 0x21 || c == 0x7f) return false;
    if (c == '/' && (i + 1 == value.size() || value[i + 1] == '/')) return false;
  }
  return true;
}

bool underKeyNamespace(const std::string& locator, const std::string& identity)
{
  // A key locator is <identity>/KEY/... for both key names and certificate
  // names, matching the identity extraction Core performs.
  return locator.size() > identity.size() + 5 &&
    locator.compare(0, identity.size(), identity) == 0 &&
    locator.compare(identity.size(), 5, "/KEY/") == 0;
}
} // namespace

NativeProviderPlanningView NativeOfferAdmission::verify(
  const NativeAckEvidence& ack, const NativeOfferPolicySnapshot& policy,
  const NativeOfferBindingContext& context, std::uint64_t nowMs) const
{
  // Provenance is a precondition, not a field equality (CD-013): an ACK that
  // did not traverse the Core-validated subscription path must never reach
  // DI offer policy, and the evidence must carry the authenticated signer
  // identity, its key locator and the validated wire digest.
  if (!ack.trustSchemaValidated) {
    throw std::runtime_error("DI_NATIVE_OFFER_REJECTED_UNAUTHENTICATED");
  }
  if (ack.signerIdentity.empty() || ack.signerKeyLocator.empty() ||
      !digest(ack.wireDigest) || !ndnName(ack.signerIdentity) ||
      !ndnName(ack.signerKeyLocator) ||
      !underKeyNamespace(ack.signerKeyLocator, ack.signerIdentity)) {
    throw std::runtime_error("DI_NATIVE_OFFER_REJECTED_UNAUTHENTICATED");
  }
  // The policy must be a usable snapshot: a bound digest, non-empty role and
  // backend sets for the planning view, and clean residency digests.
  if (!digest(policy.policyDigest) || policy.acceptedRoles.empty() ||
      policy.backends.empty() || policy.resourceSequence == 0 ||
      std::any_of(policy.residencyDigests.begin(), policy.residencyDigests.end(),
                  [] (const auto& value) { return !digest(value); })) {
    throw std::runtime_error("DI_NATIVE_OFFER_REJECTED");
  }
  // The ACK must be bound to this very request/attempt over the inspected
  // model and graph; a stale or foreign offer must not influence planning.
  if (ack.requestId != context.requestId || ack.attempt != context.attempt ||
      ack.serviceName != context.serviceName ||
      ack.modelDigest != context.modelDigest ||
      ack.graphDigest != context.graphDigest) {
    throw std::runtime_error("DI_NATIVE_OFFER_REJECTED");
  }
  // The authenticated ACK must have been signed by the provider it claims
  // (frozen ProviderOfferTrustVerifier::verify_ack signer==provider rule); an
  // offer signed by any other identity is rejected before policy planning.
  if (ack.signerIdentity != ack.provider) {
    throw std::runtime_error("DI_NATIVE_OFFER_REJECTED");
  }
  // The authenticated identities must all fall inside the immutable policy.
  if (ack.provider.empty() || ack.signerIdentity.empty() ||
      ack.controllerVersion.empty() || !digest(ack.offerDigest) ||
      !contains(policy.acceptedProviders, ack.provider) ||
      !contains(policy.acceptedServices, ack.serviceName) ||
      !contains(policy.acceptedSignerIdentities, ack.signerIdentity)) {
    throw std::runtime_error("DI_NATIVE_OFFER_REJECTED");
  }
  // Wall-clock validity of both the ACK and the policy; a capture stamped in
  // the future or an already-expired offer or policy is rejected.
  if (ack.capturedAtMs > nowMs || ack.expiresAtMs <= nowMs ||
      policy.expiresAtMs <= nowMs) {
    throw std::runtime_error("DI_NATIVE_OFFER_REJECTED");
  }
  NativeProviderPlanningView result;
  result.provider = ack.provider;
  result.offerDigest = ack.offerDigest;
  result.acceptedRoles = policy.acceptedRoles;
  result.backends = policy.backends;
  result.residencyDigests = policy.residencyDigests;
  result.freeBytes = policy.freeBytes;
  result.resourceSequence = policy.resourceSequence;
  result.preparationAccepted = true;
  result.executionAllowed = true;
  result.validate();
  return result;
}

} // namespace ndnsf::di
