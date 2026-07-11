#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionLeaseService.hpp"

namespace ndnsf::di::test {

BOOST_AUTO_TEST_SUITE(DiExecutionLeaseService)

BOOST_AUTO_TEST_CASE(PythonFixtureRoundTripsByteForByte)
{
  const std::string pythonRequest =
    "{\"expiresAtMs\":5000,\"idempotencyKey\":\"prepare-1\",\"leaseId\":\"\","
    "\"operation\":\"PREPARE\",\"planDigest\":\"plan-1\",\"providerEpoch\":\"\","
    "\"requestId\":\"request-1\",\"resourceBindingProof\":\"AGJpbmRpbmf/\","
    "\"resourceBindingSchema\":\"ndnsf-di-binding-v1\",\"roles\":[\"/Backbone\"],"
    "\"schema\":\"ndnsf-di-execution-lease-operation-v1\","
    "\"targetServiceName\":\"/Inference/NativeTracer\"}";
  const auto decodedRequest = decodeLeaseOperationRequest(pythonRequest);
  BOOST_CHECK_EQUAL(encodeLeaseOperationRequest(decodedRequest), pythonRequest);

  const std::string pythonResponse =
    "{\"conflictKeys\":[],\"executionDeadlineMs\":0,\"expiresAtMs\":0,"
    "\"leaseId\":\"lease-1\",\"operation\":\"COMMIT\",\"providerEpoch\":\"epoch-old\","
    "\"reasonCode\":\"LEASE_STALE_EPOCH\",\"retryAfterMs\":0,"
    "\"schema\":\"ndnsf-di-execution-lease-operation-v1\",\"state\":\"PREPARED\","
    "\"status\":false}";
  const auto decodedResponse = decodeLeaseOperationResponse(pythonResponse);
  BOOST_CHECK_EQUAL(encodeLeaseOperationResponse(decodedResponse), pythonResponse);
}

BOOST_AUTO_TEST_CASE(AuthenticatedContextAndTrustedConflictKeysDriveCoreTable)
{
  ExecutionLeaseService service(
    "/provider/A",
    "/Inference/NativeTracer",
    [] (const LeaseOperationRequest&, const ExecutionLeaseRequestContext&) {
      return std::vector<std::string>{"compute-slot:0"};
    },
    "epoch-A");
  ExecutionLeaseRequestContext context{
    "/user/A", "/provider/A", EXECUTION_LEASE_SERVICE_NAME, "request-1"};
  LeaseOperationRequest prepare;
  prepare.operation = LeaseOperation::Prepare;
  prepare.requestId = "request-1";
  prepare.planDigest = "plan-1";
  prepare.idempotencyKey = "prepare-1";
  prepare.targetServiceName = "/Inference/NativeTracer";
  prepare.resourceBindingProof = ndn::Buffer{1, 2, 3};
  prepare.roles = {"/Backbone"};
  prepare.expiresAtMs = 5000;

  const auto prepared = decodeLeaseOperationResponse(
    service.handle(context, encodeLeaseOperationRequest(prepare), 1000));
  BOOST_REQUIRE(prepared.status);
  BOOST_CHECK_EQUAL(prepared.providerEpoch, "epoch-A");
  BOOST_REQUIRE_EQUAL(prepared.conflictKeys.size(), 1);
  BOOST_CHECK_EQUAL(prepared.conflictKeys.front(), "compute-slot:0");

  auto forgedContext = context;
  forgedContext.requesterIdentity.clear();
  const auto forged = decodeLeaseOperationResponse(
    service.handle(forgedContext, encodeLeaseOperationRequest(prepare), 1100));
  BOOST_CHECK(!forged.status);
  BOOST_CHECK_EQUAL(forged.reasonCode, "LEASE_BINDING_MISMATCH");
}

BOOST_AUTO_TEST_CASE(MalformedAndUnknownVersionFailClosed)
{
  BOOST_CHECK_THROW(decodeLeaseOperationRequest("not-json"), std::invalid_argument);
  BOOST_CHECK_THROW(
    decodeLeaseOperationRequest("{\"schema\":\"future-v2\"}"),
    std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::test
