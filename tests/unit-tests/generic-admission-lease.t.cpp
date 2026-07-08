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

BOOST_AUTO_TEST_CASE(UserStoresGenericLeaseSelectionPayload)
{
  ndn::security::KeyChain keyChain("pib-memory:generic-lease-user-payload",
                                   "tpm-memory:generic-lease-user-payload");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/lease");
  const ndn::Name serviceName("/Inference/NativeTracer");
  const ndn::Name requestId("/request-lease-user-payload");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-lease-user-payload"));
  LocalServiceUser user(face,
                        ndn::Name("/test/group"),
                        userCert,
                        aaCert,
                        "examples/trust-any.conf");

  user.addPendingCallForTokenTest(requestId, serviceName, "user-token");
  const auto payload =
    ServiceUser::makeGenericAdmissionLeaseSelectionPayload(
      "lease-user",
      textBuffer("merge-fragment-ok"));
  BOOST_CHECK(user.setSelectionAssignmentPayloadForRequest(requestId,
                                                           providerName,
                                                           payload));
  const auto stored =
    user.getSelectionAssignmentPayloadForTest(requestId, providerName);
  BOOST_CHECK_EQUAL(std::string(reinterpret_cast<const char*>(stored.data()),
                                stored.size()),
                    "leaseId=lease-user;resourceBindingProof=merge-fragment-ok;");
  BOOST_CHECK(!user.setSelectionAssignmentPayloadForRequest(
    ndn::Name("/missing"), providerName, payload));
}

BOOST_AUTO_TEST_CASE(ProviderAckPayloadCanCarryGenericLease)
{
  ServiceProvider::GenericAdmissionLease lease;
  lease.leaseId = "lease-ack";
  lease.requesterName = ndn::Name("/test/user");
  lease.providerName = ndn::Name("/test/provider");
  lease.serviceName = ndn::Name("/Inference/NativeTracer");
  lease.expiresAtMs = 2000;
  const auto payload = ServiceProvider::makeGenericAdmissionLeaseAckPayload(
    lease,
    textBuffer("queueLength=1;"));
  const std::string text(reinterpret_cast<const char*>(payload.data()),
                         payload.size());
  BOOST_CHECK(text.find("leaseId=lease-ack;") != std::string::npos);
  BOOST_CHECK(text.find("leaseProvider=/test/provider;") != std::string::npos);
  BOOST_CHECK(text.find("leaseService=/Inference/NativeTracer;") != std::string::npos);
  BOOST_CHECK(text.find("leaseExpiresAtMs=2000;") != std::string::npos);
  BOOST_CHECK(text.find("queueLength=1;") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(GenericAckMetadataCarriesRuntimeHintLeaseAndPeerMetric)
{
  ServiceProvider::PeerNetworkMetric metric;
  metric.srcPeer = ndn::Name("/provider/A");
  metric.dstPeer = ndn::Name("/provider/B");
  metric.rttMs = 12.5;
  metric.bandwidthMbps = 500.0;
  metric.lossRate = 0.01;
  metric.jitterMs = 1.5;
  metric.observedAtMs = 123456;
  metric.confidence = 0.75;

  ServiceProvider::GenericProviderRuntimeHint hint;
  hint.providerName = ndn::Name("/provider/A");
  hint.queueLength = 2;
  hint.estimatedQueueWaitMs = 15;
  hint.cpuUtilization = 0.4;
  hint.gpuUtilization = 0.6;
  hint.freeMemoryMb = 4096;
  hint.freeGpuMemoryMb = 2048;
  hint.peerMetrics.push_back(metric);

  ServiceProvider::GenericAdmissionLease lease;
  lease.leaseId = "lease-meta";
  lease.requesterName = ndn::Name("/user/A");
  lease.providerName = ndn::Name("/provider/A");
  lease.serviceName = ndn::Name("/Inference/NativeTracer");
  lease.expiresAtMs = 4102444800000;

  ServiceProvider::GenericAckMetadata metadata;
  metadata.runtimeHint = hint;
  metadata.leaseOffers.push_back(lease);
  metadata.servicePayloadSchema = "di-runtime-v1";
  metadata.servicePayload = textBuffer("fragmentStates=sample");

  const auto payload = ServiceProvider::makeGenericAckMetadataPayload(metadata);
  const auto parsed = ServiceProvider::parseGenericAckMetadataPayload(payload);

  BOOST_REQUIRE(parsed.runtimeHint);
  BOOST_CHECK(parsed.runtimeHint->providerName == ndn::Name("/provider/A"));
  BOOST_CHECK_EQUAL(parsed.runtimeHint->queueLength, 2);
  BOOST_CHECK_EQUAL(parsed.runtimeHint->estimatedQueueWaitMs, 15);
  BOOST_CHECK_EQUAL(parsed.runtimeHint->freeMemoryMb, 4096);
  BOOST_REQUIRE_EQUAL(parsed.runtimeHint->peerMetrics.size(), 1);
  BOOST_CHECK(parsed.runtimeHint->peerMetrics.front().srcPeer == ndn::Name("/provider/A"));
  BOOST_CHECK(parsed.runtimeHint->peerMetrics.front().dstPeer == ndn::Name("/provider/B"));
  BOOST_CHECK_CLOSE(parsed.runtimeHint->peerMetrics.front().lossRate, 0.01, 0.001);
  BOOST_REQUIRE_EQUAL(parsed.leaseOffers.size(), 1);
  BOOST_CHECK_EQUAL(parsed.leaseOffers.front().leaseId, "lease-meta");
  BOOST_CHECK(parsed.leaseOffers.front().serviceName == ndn::Name("/Inference/NativeTracer"));
  BOOST_CHECK_EQUAL(parsed.servicePayloadSchema, "di-runtime-v1");
}

BOOST_AUTO_TEST_CASE(PeerNetworkMetricEnvelopeRoundTripsAsDirected)
{
  ServiceProvider::PeerNetworkMetric metric;
  metric.srcPeer = ndn::Name("/provider/left");
  metric.dstPeer = ndn::Name("/provider/right");
  metric.rttMs = 4.0;
  metric.bandwidthMbps = 900.0;
  metric.lossRate = 0.02;

  const auto payload = ServiceProvider::makePeerNetworkMetricPayload(metric);
  const auto parsed = ServiceProvider::parsePeerNetworkMetricPayload(payload);
  BOOST_REQUIRE(parsed);
  BOOST_CHECK(parsed->srcPeer == ndn::Name("/provider/left"));
  BOOST_CHECK(parsed->dstPeer == ndn::Name("/provider/right"));
  BOOST_CHECK_CLOSE(parsed->bandwidthMbps, 900.0, 0.001);
}

BOOST_AUTO_TEST_CASE(CoreOperationStatusCarriesDataProductReference)
{
  ServiceProvider::DataProductReference reference;
  reference.name = ndn::Name("/repo/model/stage0/v=1");
  reference.producerName = ndn::Name("/repo/A");
  reference.serviceName = ndn::Name("/NDNSF/DistributedRepo");
  reference.objectClass = "model-artifact";
  reference.contentType = "application/octet-stream";
  reference.digest = "sha256:abc";
  reference.sizeBytes = 4096;
  reference.segmentCount = 3;
  reference.freshnessMs = 60000;

  ServiceProvider::ServiceOperationStatus status;
  status.operationId = "store-1";
  status.operation = "STORE";
  status.serviceName = ndn::Name("/NDNSF/DistributedRepo");
  status.providerName = ndn::Name("/repo/A");
  status.requestId = ndn::Name("/request/1");
  status.state = "DONE";
  status.reasonCode = "OK";
  status.message = "stored";
  status.progress = 1.0;
  status.resultReference = reference;
  status.updatedAtMs = 2000;

  const auto payload = ServiceProvider::makeServiceOperationStatusPayload(status);
  const auto parsed = ServiceProvider::parseServiceOperationStatusPayload(payload);

  BOOST_REQUIRE(parsed);
  BOOST_CHECK_EQUAL(parsed->operationId, "store-1");
  BOOST_CHECK_EQUAL(parsed->operation, "STORE");
  BOOST_CHECK_EQUAL(parsed->state, "DONE");
  BOOST_CHECK_CLOSE(parsed->progress, 1.0, 0.001);
  BOOST_REQUIRE(parsed->resultReference);
  BOOST_CHECK(parsed->resultReference->name == ndn::Name("/repo/model/stage0/v=1"));
  BOOST_CHECK_EQUAL(parsed->resultReference->objectClass, "model-artifact");
  BOOST_CHECK_EQUAL(parsed->resultReference->segmentCount, 3);
}

BOOST_AUTO_TEST_CASE(CoreProviderCapabilityHintCarriesRuntimeDrainAndOperation)
{
  ServiceProvider::GenericProviderRuntimeHint runtime;
  runtime.providerName = ndn::Name("/provider/A");
  runtime.queueLength = 4;
  runtime.estimatedQueueWaitMs = 25;
  runtime.freeGpuMemoryMb = 8192;

  ServiceProvider::ServiceOperationStatus status;
  status.operationId = "provision-1";
  status.operation = "ARTIFACT_PROVISION";
  status.serviceName = ndn::Name("/Inference/NativeTracer");
  status.providerName = ndn::Name("/provider/A");
  status.state = "RUNNING";
  status.progress = 0.5;

  ServiceProvider::ProviderCapabilityHint hint;
  hint.providerName = ndn::Name("/provider/A");
  hint.serviceName = ndn::Name("/Inference/NativeTracer");
  hint.ready = false;
  hint.drainState = "DRAINING";
  hint.reasonCode = "PROVIDER_BUSY";
  hint.message = "finishing active work";
  hint.runtimeHint = runtime;
  hint.operationStatus = status;
  hint.servicePayloadSchema = "ndnsf-di-runtime-ack-v1";
  hint.servicePayload = textBuffer("fragmentStates=stage0:GPU_LOADED");

  const auto payload = ServiceProvider::makeProviderCapabilityHintPayload(hint);
  const auto parsed = ServiceProvider::parseProviderCapabilityHintPayload(payload);

  BOOST_REQUIRE(parsed);
  BOOST_CHECK(parsed->providerName == ndn::Name("/provider/A"));
  BOOST_CHECK(parsed->serviceName == ndn::Name("/Inference/NativeTracer"));
  BOOST_CHECK(!parsed->ready);
  BOOST_CHECK_EQUAL(parsed->drainState, "DRAINING");
  BOOST_CHECK(!parsed->readyForNewRequest());
  BOOST_REQUIRE(parsed->runtimeHint);
  BOOST_CHECK_EQUAL(parsed->runtimeHint->queueLength, 4);
  BOOST_CHECK_EQUAL(parsed->runtimeHint->freeGpuMemoryMb, 8192);
  BOOST_REQUIRE(parsed->operationStatus);
  BOOST_CHECK_EQUAL(parsed->operationStatus->operation, "ARTIFACT_PROVISION");
  BOOST_CHECK_CLOSE(parsed->operationStatus->progress, 0.5, 0.001);
  BOOST_CHECK_EQUAL(parsed->servicePayloadSchema, "ndnsf-di-runtime-ack-v1");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
