#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"

namespace ndnsf::di::test {

BOOST_AUTO_TEST_SUITE(DiReservationLease)

static DiReservationRequest
request(std::string requester = "/user/A", std::string requestId = "request-1")
{
  DiReservationRequest value;
  value.providerName = "/provider/A";
  value.requesterName = std::move(requester);
  value.requestId = std::move(requestId);
  value.serviceName = "/Inference/Generic";
  value.planDigest = "sha256:plan";
  value.resourceBindingProof = ndn::Buffer{1, 2, 3};
  value.conflictKeys = {"gpu-slot:0"};
  value.authorized = true;
  return value;
}

BOOST_AUTO_TEST_CASE(AuthorizationPrecedesAllocationAndDuplicateDoesNotExtend)
{
  DiReservationAuthority authority("boot-A", {2, 1, 2, 100});
  auto denied = request();
  denied.authorized = false;
  BOOST_CHECK(!authority.reserve(denied, 1000).status);

  const auto first = authority.reserve(request(), 1000);
  const auto duplicate = authority.reserve(request(), 1050);
  BOOST_REQUIRE(first.status);
  BOOST_REQUIRE(duplicate.status);
  BOOST_CHECK(duplicate.idempotentReplay);
  BOOST_CHECK_EQUAL(first.lease.reservationId, duplicate.lease.reservationId);
  BOOST_CHECK_EQUAL(first.lease.expiresAtMs, duplicate.lease.expiresAtMs);
}

BOOST_AUTO_TEST_CASE(QuotaExpiryCommitAndShutdownAreBounded)
{
  DiReservationAuthority authority("boot-A", {2, 1, 2, 100});
  const auto first = authority.reserve(request(), 1000);
  BOOST_REQUIRE(first.status);
  BOOST_CHECK(!authority.reserve(request("/user/A", "request-2"), 1001).status);
  BOOST_CHECK_EQUAL(authority.cleanupExpired(1100), 1);
  BOOST_CHECK(!authority.commit(first.lease.reservationId, 1100).status);

  const auto second = authority.reserve(request("/user/A", "request-2"), 1101);
  BOOST_REQUIRE(second.status);
  BOOST_CHECK(authority.commit(second.lease.reservationId, 1102).status);
  authority.releaseAll(1103);
  BOOST_CHECK(authority.release(second.lease.reservationId, "DUPLICATE", 1104));
}

BOOST_AUTO_TEST_CASE(OrdinaryAckPathRequiresNoReservationAuthority)
{
  // DI reservation is opt-in: constructing and publishing an ordinary ACK has
  // no call path into DiReservationAuthority and therefore allocates nothing.
  ndn_service_framework::RequestAckMessage ack;
  ack.setStatus(true);
  BOOST_CHECK(ack.getStatus());
  BOOST_CHECK(!ack.hasReservationLease());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::test
