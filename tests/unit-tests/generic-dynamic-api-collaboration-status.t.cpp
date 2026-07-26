#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)
BOOST_AUTO_TEST_SUITE(CollaborationStatus)

BOOST_AUTO_TEST_CASE(OperationStatusCodecRetainsMonotonicAndUnknownProgressFields)
{
  ServiceProvider::ServiceOperationStatus status;
  status.operationId = "prepare:prefill";
  status.operation = "MODEL_PREPARE";
  status.serviceName = ndn::Name("/LLM/Qwen");
  status.providerName = ndn::Name("/provider/a");
  status.requestId = ndn::Name("/request/1");
  status.role = "prefill";
  status.attempt = 2;
  status.epoch = 3;
  status.sequence = 4;
  status.state = "LOADING";
  status.progressKnown = false;
  status.progress = 0.0;
  status.detailsSchema = "ndnsf-di-progress-v1";
  status.detailsPayload = ndn::Buffer{0x00, 0x7f, 0xff};

  const auto wire = ServiceProvider::makeServiceOperationStatusPayload(status);
  const auto decoded = ServiceProvider::parseServiceOperationStatusPayload(wire);
  BOOST_REQUIRE(decoded);
  BOOST_CHECK_EQUAL(decoded->role, "prefill");
  BOOST_CHECK_EQUAL(decoded->attempt, 2);
  BOOST_CHECK_EQUAL(decoded->epoch, 3);
  BOOST_CHECK_EQUAL(decoded->sequence, 4);
  BOOST_CHECK(!decoded->progressKnown);
  BOOST_CHECK_EQUAL_COLLECTIONS(decoded->detailsPayload.begin(),
                                decoded->detailsPayload.end(),
                                status.detailsPayload.begin(),
                                status.detailsPayload.end());
}

BOOST_AUTO_TEST_CASE(SelectionSnapshotRejectsStaleMemberAndKeepsLatest)
{
  ndn::security::KeyChain keyChain("pib-memory:collab-status",
                                   "tpm-memory:collab-status");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/provider/a"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceProvider provider(face, ndn::Name("/test/group"),
                                providerCert, aaCert,
                                "examples/trust-any.conf");
  provider.seedSelectionStatusForTest("sha256:selection",
                                      ndn::Name("/LLM/Qwen"),
                                      ndn::Name("/request/1"));

  ServiceProvider::ServiceOperationStatus status;
  status.operationId = "prepare:decode";
  status.operation = "MODEL_PREPARE";
  status.role = "decode";
  status.attempt = 1;
  status.epoch = 1;
  status.sequence = 1;
  status.state = "FETCHING";
  status.progressKnown = true;
  status.progress = 0.25;
  provider.reportSelectionOperationStatus("sha256:selection", status);

  auto snapshot = provider.getSelectionExecutionStatus("sha256:selection");
  BOOST_REQUIRE(snapshot);
  BOOST_REQUIRE_EQUAL(snapshot->memberStatuses.size(), 1);
  BOOST_CHECK_EQUAL(snapshot->memberStatuses.front().role, "decode");
  BOOST_CHECK_CLOSE(snapshot->memberStatuses.front().progress, 0.25, 0.001);

  BOOST_CHECK_THROW(
    provider.reportSelectionOperationStatus("sha256:selection", status),
    std::invalid_argument);
  status.sequence = 2;
  status.state = "VERIFYING";
  status.progress = 0.5;
  provider.reportSelectionOperationStatus("sha256:selection", status);
  snapshot = provider.getSelectionExecutionStatus("sha256:selection");
  BOOST_REQUIRE(snapshot);
  BOOST_REQUIRE_EQUAL(snapshot->memberStatuses.size(), 1);
  BOOST_CHECK_EQUAL(snapshot->memberStatuses.front().sequence, 2);
}

BOOST_AUTO_TEST_CASE(R1DecisionReceiptSurvivesSignedStatusPayloadCodec)
{
  SelectionDecisionReceipt receipt;
  receipt.setField("decisionDigest", "sha256:decision");
  receipt.setField("reservationId", "reservation-1");
  receipt.setField("provider", "/provider/a");
  receipt.setField("acceptedState", "RELEASE_ACCEPTED");
  const auto block = receipt.WireEncode();
  SelectionExecutionStatus status;
  status.providerName = ndn::Name("/provider/a");
  status.serviceName = ndn::Name("/Inference/Generic");
  status.requestId = ndn::Name("request-1");
  status.selectionDigest = "sha256:selection";
  status.state = SelectionExecutionState::Completed;
  status.decisionReceipt = ndn::Buffer(block.data(), block.size());
  const auto payload = LocalServiceProvider::encodeSelectionStatusForTest(status);
  ndn::Data data(ndn::Name("/provider/a/status"));
  data.setContent(payload);
  const auto decoded = LocalServiceUser::parseSelectionStatusForTest(
    data, status.providerName, status.selectionDigest);
  BOOST_CHECK_EQUAL_COLLECTIONS(decoded.decisionReceipt.begin(),
                                decoded.decisionReceipt.end(),
                                status.decisionReceipt.begin(),
                                status.decisionReceipt.end());
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
