#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"

#include <stdexcept>
#include <string>

namespace ndnsf::di::test {
namespace {

ProtectedRuntimeBindingV1
binding()
{
  ProtectedRuntimeBindingV1 value;
  value.provider = "/provider/P0";
  value.role = "stage0";
  value.requestId = "/request/1";
  value.attempt = 1;
  value.planCoreDigest = "sha256:" + std::string(64, 'a');
  value.planDigest = "sha256:" + std::string(64, 'b');
  value.securityPolicySnapshotDigest = "sha256:" + std::string(64, 'c');
  value.protectionEpoch = "policy-epoch-7";
  value.grantName = "/authority/grants/request-1/provider-P0";
  value.grantDigest = "sha256:" + std::string(64, 'd');
  value.capabilityDigest = "sha256:" + std::string(64, 'e');
  value.groupId = "group-1";
  value.groupEpoch = 7;
  value.epochKeyId = "sha256:" + std::string(64, 'f');
  value.providerBootId = "boot-1";
  value.fencingToken = "fence-1";
  value.revocationSequence = 4;
  value.expiresAtMs = 5000;
  value.mayPublishEndpointDigests = {
    "sha256:" + std::string(64, '1')};
  value.mustFetchEndpointDigests = {
    "sha256:" + std::string(64, '2')};
  value.mayPublishConsumerByEndpoint = {
    {"sha256:" + std::string(64, '1'), "stage1"}};
  value.mustFetchProducerByEndpoint = {
    {"sha256:" + std::string(64, '2'), "stage-prev"}};
  return value;
}

} // namespace

BOOST_AUTO_TEST_CASE(ProtectedRuntimeRejectsWrongDataflowAndZeroizesOnRevocation)
{
  const auto expected = binding();
  ProtectedRuntime runtime(expected);
  runtime.verifyGrant(expected, 1000);

  BOOST_CHECK_NO_THROW(runtime.authorizeDataflow(
    ProtectedDataflowDirection::Publish,
    "sha256:" + std::string(64, '1'), "stage0", "stage1", 1001));
  BOOST_CHECK_THROW(runtime.authorizeDataflow(
    ProtectedDataflowDirection::Publish,
    "sha256:" + std::string(64, '9'), "stage0", "stage1", 1001),
    std::runtime_error);
  BOOST_CHECK_THROW(runtime.authorizeDataflow(
    ProtectedDataflowDirection::Publish,
    "sha256:" + std::string(64, '1'), "stage0", "wrong-consumer", 1001),
    std::runtime_error);
  BOOST_CHECK_THROW(runtime.authorizeDataflow(
    ProtectedDataflowDirection::Publish,
    "sha256:" + std::string(64, '1'), "wrong-role", "stage1", 1001),
    std::runtime_error);

  bool hostZeroized = false;
  bool deviceZeroized = false;
  runtime.registerHostPlaintextLease(
    "host-1", [&] { hostZeroized = true; });
  runtime.registerDevicePlaintextLease(
    "device-1", [&] { deviceZeroized = true; });
  runtime.revoke(5, "grant revoked");

  BOOST_CHECK(hostZeroized);
  BOOST_CHECK(deviceZeroized);
  BOOST_CHECK(runtime.revoked());
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::Zeroized);
  BOOST_CHECK_THROW(runtime.authorizeDataflow(
    ProtectedDataflowDirection::Publish,
    "sha256:" + std::string(64, '1'), "stage0", "stage1", 1002),
    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ProtectedRuntimeFailsClosedOnGrantSubstitution)
{
  const auto expected = binding();
  auto substituted = expected;
  substituted.provider = "/provider/P1";
  ProtectedRuntime runtime(expected);

  BOOST_CHECK_THROW(runtime.verifyGrant(substituted, 1000),
                    std::runtime_error);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::FailedClosed);
  BOOST_CHECK_THROW(runtime.authorizeDataflow(
    ProtectedDataflowDirection::Fetch,
    "sha256:" + std::string(64, '2'), "stage-prev", "stage0", 1001),
    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ProtectedRuntimeRemainsFailedClosedAfterZeroizerFailure)
{
  const auto expected = binding();
  ProtectedRuntime runtime(expected);
  runtime.verifyGrant(expected, 1000);
  runtime.registerHostPlaintextLease("host-bad", [] {
    throw std::runtime_error("device cleanup failed");
  });

  BOOST_CHECK_THROW(runtime.cancel("request cancelled"), std::runtime_error);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::FailedClosed);
  BOOST_CHECK_THROW(runtime.verifyGrant(expected, 1001), std::runtime_error);
}

} // namespace ndnsf::di::test
