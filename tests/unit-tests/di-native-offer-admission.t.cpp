// Spec182OfferAdmission: frozen admission gate of NativeOfferAdmission against
// the Core-authenticated ACK evidence shape.  The suite mirrors the Python
// oracle (ProviderOfferTrustVerifier::verify_ack in app_sdk/provider.py):
// Trust-Schema validation is a precondition, the signer identity/key locator/
// validated wire digest must be present and coherent, the ACK must be signed
// by the provider it claims, every authenticated identity must fall inside the
// immutable offer policy, and the wall-clock validity of ACK and policy is
// checked before an immutable planning view is produced.  Real Core admission
// (evidence produced by the validated ServiceUser subscription path) runs at
// T016; here every rejected fixture targets exactly one rule with all other
// dimensions valid.

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"

#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>

#include <functional>
#include <string>
#include <vector>

namespace ndnsf::di {
namespace {

std::string digest(const std::string& value)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);
  std::string result = "sha256:";
  for (const auto byte : hash) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}

NativeOfferPolicySnapshot policyFor()
{
  NativeOfferPolicySnapshot policy;
  policy.policyDigest = digest("policy");
  policy.acceptedSignerIdentities = {"/provider"};
  policy.acceptedProviders = {"/provider"};
  policy.acceptedServices = {"/service"};
  policy.acceptedRoles = {"role-a", "role-b"};
  policy.backends = {"onnxruntime"};
  policy.residencyDigests = {digest("resident")};
  policy.freeBytes = 4096;
  policy.resourceSequence = 1;
  policy.expiresAtMs = 2000;
  return policy;
}

NativeOfferBindingContext contextFor()
{
  return {"/request", 1, "/service", digest("model"), digest("graph")};
}

NativeAckEvidence ackFor(const NativeOfferBindingContext& context)
{
  NativeAckEvidence ack;
  ack.trustSchemaValidated = true;
  ack.requestId = context.requestId;
  ack.attempt = context.attempt;
  ack.provider = "/provider";
  ack.serviceName = context.serviceName;
  ack.signerIdentity = "/provider";
  ack.signerKeyLocator = "/provider/KEY/1";
  ack.wireDigest = digest("wire");
  ack.controllerVersion = "controller-v1";
  ack.offerDigest = digest("offer");
  ack.modelDigest = context.modelDigest;
  ack.graphDigest = context.graphDigest;
  ack.capturedAtMs = 500;
  ack.expiresAtMs = 1500;
  return ack;
}

void expectCode(const std::function<void()>& fn, const std::string& code)
{
  bool thrown = false;
  try {
    fn();
  } catch (const std::runtime_error& error) {
    thrown = true;
    BOOST_REQUIRE_EQUAL(std::string(error.what()), code);
  }
  if (!thrown) BOOST_ERROR("expected rejection " + code);
}

const std::string kUnauthenticated = "DI_NATIVE_OFFER_REJECTED_UNAUTHENTICATED";
const std::string kRejected = "DI_NATIVE_OFFER_REJECTED";

BOOST_AUTO_TEST_SUITE(Spec182OfferAdmission)

// Resident gate: an ACK that never passed the Core-validated subscription
// path (trustSchemaValidated false, as Core leaves direct/unit fixtures) can
// never produce a planning view, no matter what its other fields claim.
BOOST_AUTO_TEST_CASE(NativeOfferAdmissionRejectsUnauthenticatedAck)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  auto ack = ackFor(context);
  ack.trustSchemaValidated = false;
  expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kUnauthenticated);
}

// A caller-declared "verified" claim is not provenance: without the signer
// identity there is nothing to bind to policy.
BOOST_AUTO_TEST_CASE(RejectsAuthenticatedClaimWithoutSignerIdentity)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  auto ack = ackFor(context);
  ack.signerIdentity.clear();
  expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kUnauthenticated);
}

// A signed ACK must carry its signer key locator; an empty locator is not
// authenticated provenance.
BOOST_AUTO_TEST_CASE(RejectsAuthenticatedClaimWithoutKeyLocator)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  auto ack = ackFor(context);
  ack.signerKeyLocator.clear();
  expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kUnauthenticated);
}

// The validated wire digest must be present and take the canonical sha256
// digest shape; anything else is a fabricated provenance claim.
BOOST_AUTO_TEST_CASE(RejectsMalformedOrMissingWireDigest)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  for (const auto& wire : {"", "not-a-digest", "sha256:zz", "sha256:"}) {
    auto ack = ackFor(context);
    ack.wireDigest = wire;
    expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kUnauthenticated);
  }
}

// The key locator must sit under the signer identity's /KEY/ namespace (the
// shape Core's identity extraction produces from key and certificate names);
// an out-of-namespace or non-NDN locator is incoherent provenance.
BOOST_AUTO_TEST_CASE(RejectsKeyLocatorOutsideSignerKeyNamespace)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  for (const auto& locator : {"/other/KEY/1", "/provider/keys/1",
                              "/provider", "provider/KEY/1", "/a//b/KEY/1"}) {
    auto ack = ackFor(context);
    ack.signerKeyLocator = locator;
    expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kUnauthenticated);
  }
}

// The ACK must have been signed by the provider it claims (frozen
// ProviderOfferTrustVerifier::verify_ack signer==provider rule): a coherent
// foreign signature is a binding failure, not valid provenance for planning.
BOOST_AUTO_TEST_CASE(RejectsAckSignedByOtherThanClaimedProvider)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  auto policy = policyFor();
  policy.acceptedSignerIdentities = {"/provider", "/other"};
  auto ack = ackFor(context);
  ack.signerIdentity = "/other";
  ack.signerKeyLocator = "/other/KEY/1";
  expectCode([&] { admission.verify(ack, policy, context, 1000); }, kRejected);
}

// Even a provider-signed ACK is rejected when its signer identity is outside
// the immutable policy's accepted signer identities.
BOOST_AUTO_TEST_CASE(RejectsSignerNotAcceptedByPolicy)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  auto policy = policyFor();
  policy.acceptedSignerIdentities = {"/other"};
  expectCode([&] { admission.verify(ackFor(context), policy, context, 1000); }, kRejected);
}

BOOST_AUTO_TEST_CASE(RejectsProviderNotAcceptedByPolicy)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  auto policy = policyFor();
  policy.acceptedProviders = {"/other"};
  expectCode([&] { admission.verify(ackFor(context), policy, context, 1000); }, kRejected);
}

BOOST_AUTO_TEST_CASE(RejectsServiceNotAcceptedByPolicy)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  auto policy = policyFor();
  policy.acceptedServices = {"/other-service"};
  expectCode([&] { admission.verify(ackFor(context), policy, context, 1000); }, kRejected);
}

// The ACK must be bound to this very request/attempt over the inspected
// model and graph; a foreign binding must not reach planning.
BOOST_AUTO_TEST_CASE(RejectsRequestOrModelBindingMismatch)
{
  NativeOfferAdmission admission;
  auto context = contextFor();
  auto ack = ackFor(context);

  context.requestId = "/foreign-request";
  expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kRejected);
  context = contextFor();
  context.attempt = 2;
  expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kRejected);
  context = contextFor();
  context.serviceName = "/other-service";
  expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kRejected);
  context = contextFor();
  context.modelDigest = digest("other-model");
  expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kRejected);
  context = contextFor();
  context.graphDigest = digest("other-graph");
  expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kRejected);
}

// A policy snapshot that cannot produce an executable planning view (no bound
// digest, no roles, no backend, zero resource sequence, dirty residency
// digests) fails closed before any view is formed.
BOOST_AUTO_TEST_CASE(RejectsUnusablePolicySnapshot)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  auto policy = policyFor();
  policy.policyDigest = "not-a-digest";
  expectCode([&] { admission.verify(ackFor(context), policy, context, 1000); }, kRejected);
  policy = policyFor();
  policy.acceptedRoles.clear();
  expectCode([&] { admission.verify(ackFor(context), policy, context, 1000); }, kRejected);
  policy = policyFor();
  policy.backends.clear();
  expectCode([&] { admission.verify(ackFor(context), policy, context, 1000); }, kRejected);
  policy = policyFor();
  policy.resourceSequence = 0;
  expectCode([&] { admission.verify(ackFor(context), policy, context, 1000); }, kRejected);
  policy = policyFor();
  policy.residencyDigests = {digest("resident"), "dirty"};
  expectCode([&] { admission.verify(ackFor(context), policy, context, 1000); }, kRejected);
}

// Wall-clock validity: an ACK captured in the future is a forged clock claim
// and an already-expired ACK can never be admitted.
BOOST_AUTO_TEST_CASE(RejectsFutureDatedOrExpiredAck)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  auto ack = ackFor(context);
  ack.capturedAtMs = 1500;
  expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kRejected);
  ack = ackFor(context);
  ack.expiresAtMs = 1000;
  expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kRejected);
}

BOOST_AUTO_TEST_CASE(RejectsPolicyAlreadyExpired)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  auto policy = policyFor();
  policy.expiresAtMs = 1000;
  expectCode([&] { admission.verify(ackFor(context), policy, context, 1000); }, kRejected);
}

// The certified offer identity (controller version and offer digest) is part
// of the evidence; a claim without them is rejected with the policy family.
BOOST_AUTO_TEST_CASE(RejectsMissingControllerVersionOrOfferDigest)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  auto ack = ackFor(context);
  ack.controllerVersion.clear();
  expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kRejected);
  ack = ackFor(context);
  ack.offerDigest.clear();
  expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kRejected);
  ack = ackFor(context);
  ack.offerDigest = "bogus";
  expectCode([&] { admission.verify(ack, policyFor(), context, 1000); }, kRejected);
}

// The one valid path: a fully coherent Core-validated ACK inside the policy
// produces the same immutable planning view on every call, and the view is
// derived only from the authenticated ACK and the policy snapshot.
BOOST_AUTO_TEST_CASE(AcceptsCoreValidatedAckIntoImmutablePlanningView)
{
  NativeOfferAdmission admission;
  const auto context = contextFor();
  const auto policy = policyFor();
  const auto ack = ackFor(context);
  const auto view = admission.verify(ack, policy, context, 1000);
  view.validate();
  BOOST_CHECK_EQUAL(view.provider, "/provider");
  BOOST_CHECK_EQUAL(view.offerDigest, digest("offer"));
  BOOST_CHECK_EQUAL(view.acceptedRoles.size(), 2u);
  BOOST_CHECK_EQUAL(view.acceptedRoles[0], "role-a");
  BOOST_CHECK_EQUAL(view.acceptedRoles[1], "role-b");
  BOOST_CHECK_EQUAL(view.backends.size(), 1u);
  BOOST_CHECK_EQUAL(view.backends[0], "onnxruntime");
  BOOST_CHECK_EQUAL(view.residencyDigests.size(), 1u);
  BOOST_CHECK_EQUAL(view.residencyDigests[0], digest("resident"));
  BOOST_CHECK_EQUAL(view.freeBytes, 4096u);
  BOOST_CHECK_EQUAL(view.resourceSequence, 1u);
  BOOST_CHECK(view.preparationAccepted);
  BOOST_CHECK(view.executionAllowed);
  const auto again = admission.verify(ack, policy, context, 1000);
  BOOST_CHECK_EQUAL(again.provider, view.provider);
  BOOST_CHECK_EQUAL(again.offerDigest, view.offerDigest);
  BOOST_CHECK(again.acceptedRoles == view.acceptedRoles);
  BOOST_CHECK(again.backends == view.backends);
  BOOST_CHECK(again.residencyDigests == view.residencyDigests);
  BOOST_CHECK_EQUAL(again.freeBytes, view.freeBytes);
  BOOST_CHECK_EQUAL(again.resourceSequence, view.resourceSequence);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndnsf::di
