#include "tests/boost-test.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericAdmissionLease)

static ndn::Buffer
textBuffer(const std::string& text)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

BOOST_AUTO_TEST_CASE(ProviderAdmissionLeaseConsumesOnce)
{
  ServiceProvider::ProviderAdmissionLeaseTable table;
  ServiceProvider::GenericAdmissionLease lease;
  lease.leaseId = "lease-1";
  lease.requesterName = ndn::Name("/test/user");
  lease.providerName = ndn::Name("/test/provider");
  lease.serviceName = ndn::Name("/HELLO");
  lease.expiresAtMs = 2000;
  lease.resourceBindingProof = textBuffer("role=/Backbone;");
  table.grant(lease);

  auto accepted = table.consume("lease-1",
                                ndn::Name("/test/user"),
                                ndn::Name("/test/provider"),
                                ndn::Name("/HELLO"),
                                textBuffer("role=/Backbone;"),
                                1000);
  BOOST_CHECK(accepted.status);
  BOOST_CHECK_EQUAL(accepted.reasonCode, "OK");

  auto replayed = table.consume("lease-1",
                                ndn::Name("/test/user"),
                                ndn::Name("/test/provider"),
                                ndn::Name("/HELLO"),
                                textBuffer("role=/Backbone;"),
                                1000);
  BOOST_CHECK(!replayed.status);
  BOOST_CHECK_EQUAL(replayed.reasonCode, "LEASE_ALREADY_CONSUMED");
}

BOOST_AUTO_TEST_CASE(ProviderAdmissionLeaseRejectsBindingAndScopeMismatch)
{
  ServiceProvider::ProviderAdmissionLeaseTable table;
  ServiceProvider::GenericAdmissionLease lease;
  lease.leaseId = "lease-2";
  lease.requesterName = ndn::Name("/test/user");
  lease.providerName = ndn::Name("/test/provider");
  lease.serviceName = ndn::Name("/Inference/NativeTracer");
  lease.resourceBindingProof = textBuffer("merge-fragment-a");
  table.grant(lease);

  auto wrongService = table.consume("lease-2",
                                    ndn::Name("/test/user"),
                                    ndn::Name("/test/provider"),
                                    ndn::Name("/HELLO"),
                                    textBuffer("merge-fragment-a"),
                                    1000);
  BOOST_CHECK(!wrongService.status);
  BOOST_CHECK_EQUAL(wrongService.reasonCode, "LEASE_SERVICE_MISMATCH");

  auto wrongBinding = table.consume("lease-2",
                                    ndn::Name("/test/user"),
                                    ndn::Name("/test/provider"),
                                    ndn::Name("/Inference/NativeTracer"),
                                    textBuffer("backbone-fragment-b"),
                                    1000);
  BOOST_CHECK(!wrongBinding.status);
  BOOST_CHECK_EQUAL(wrongBinding.reasonCode,
                    "LEASE_RESOURCE_BINDING_MISMATCH");
}

BOOST_AUTO_TEST_CASE(ProviderAdmissionLeaseRejectsExpiredAndMissingLease)
{
  ServiceProvider::ProviderAdmissionLeaseTable table;
  ServiceProvider::GenericAdmissionLease lease;
  lease.leaseId = "lease-expired";
  lease.serviceName = ndn::Name("/HELLO");
  lease.expiresAtMs = 1000;
  table.grant(lease);

  auto expired = table.consume("lease-expired",
                               ndn::Name("/test/user"),
                               ndn::Name("/test/provider"),
                               ndn::Name("/HELLO"),
                               ndn::Buffer(),
                               1001);
  BOOST_CHECK(!expired.status);
  BOOST_CHECK_EQUAL(expired.reasonCode, "LEASE_EXPIRED");

  auto missing = table.consume("lease-missing",
                               ndn::Name("/test/user"),
                               ndn::Name("/test/provider"),
                               ndn::Name("/HELLO"),
                               ndn::Buffer(),
                               1000);
  BOOST_CHECK(!missing.status);
  BOOST_CHECK_EQUAL(missing.reasonCode, "LEASE_NOT_FOUND");
}

BOOST_AUTO_TEST_CASE(ProviderRejectsInvalidLeaseBeforeExecution)
{
  ndn::security::KeyChain keyChain("pib-memory:generic-lease-execution",
                                   "tpm-memory:generic-lease-execution");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/lease");
  const ndn::Name serviceName("/Inference/NativeTracer");
  const ndn::Name requestId("/request-lease-invalid");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-lease-execution"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName));

  int executions = 0;
  provider.addService(
    serviceName,
    ServiceProvider::AckStrategyHandler{},
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++executions;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));
  provider.setGenericAdmissionLeaseRequired(serviceName, true);

  RequestMessage request = makeRequestMessageWithUserToken("hello");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         requestId,
                                         request,
                                         "provider-token");
  ServiceProvider::GenericAdmissionLease lease;
  lease.leaseId = "lease-exec";
  lease.requesterName = requesterName;
  lease.providerName = providerName;
  lease.serviceName = serviceName;
  lease.expiresAtMs = 4102444800000;
  lease.resourceBindingProof = textBuffer("merge-fragment-ok");
  provider.grantGenericAdmissionLease(lease);

  auto badSelectionPayload =
    textBuffer("leaseId=lease-exec;resourceBindingProof=backbone-fragment-b;");
  auto badSelection = makeSelectionBuffer(requestId,
                                          "provider-token",
                                          badSelectionPayload);
  ServiceSelectionMessage badSelectionMessage;
  auto [badBlockOk, badBlock] = ndn::Block::fromBuffer(
    ndn::span<const uint8_t>(badSelection.data(), badSelection.size()));
  BOOST_REQUIRE(badBlockOk);
  BOOST_REQUIRE(badSelectionMessage.WireDecode(badBlock));
  const auto badDigest = computeSelectionDigest(badSelectionMessage);
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(requesterName,
                                                                providerName,
                                                                serviceName,
                                                                requestId,
                                                                badSelection);
  BOOST_CHECK_EQUAL(executions, 0);
  auto rejected = provider.getSelectionExecutionStatus(badDigest);
  BOOST_REQUIRE(rejected);
  BOOST_CHECK(rejected->state == SelectionExecutionState::Rejected);
  BOOST_CHECK(rejected->message.find("LEASE_RESOURCE_BINDING_MISMATCH") !=
              std::string::npos);

  const ndn::Name retryRequestId("/request-lease-valid");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         retryRequestId,
                                         request,
                                         "provider-token-2");
  auto goodSelectionPayload = textBuffer(
    "leaseId=lease-exec;resourceBindingProof=merge-fragment-ok;");
  auto goodSelection = makeSelectionBuffer(retryRequestId,
                                           "provider-token-2",
                                           goodSelectionPayload);
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(requesterName,
                                                                providerName,
                                                                serviceName,
                                                                retryRequestId,
                                                                goodSelection);
  BOOST_CHECK_EQUAL(executions, 1);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
