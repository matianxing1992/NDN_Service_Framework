#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"

#include <stdexcept>
#include <string>

// Structural consistency grants no authority without configured credentials.
// Real grant, dataflow and zeroization checks use BoundGrantFixture in the
// companion protected-runtime-grant test file.

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
  value.capabilityDigest = std::string(64, 'e');
  value.groupId = "group-1";
  value.groupEpoch = 7;
  value.epochKeyId = std::string(64, 'f');
  value.providerBootId = "boot-1";
  value.fencingToken = "fence-1";
  // Passive wire field (fixed; no revocation ledger on this branch).
  value.revocationSequence = 1;
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

bool
hasToken(const std::exception& error, const std::string& token)
{
  return std::string(error.what()).find(token) != std::string::npos;
}

} // namespace

BOOST_AUTO_TEST_CASE(ProtectedRuntimeVerifyGrantFailsClosedWithoutConfiguration)
{
  const auto expected = binding();
  ProtectedRuntime runtime(expected);

  // Matching fields cannot substitute for fetching and verifying a grant.
  try {
    runtime.verifyGrant(expected, 1000);
    BOOST_FAIL("verifyGrant must fail closed without configured credentials");
  }
  catch (const std::runtime_error& error) {
    BOOST_CHECK(hasToken(error, "DI_PROTECTED_GRANT_UNAVAILABLE"));
  }
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::FailedClosed);
  BOOST_CHECK_THROW(runtime.authorizeDataflow(
    ProtectedDataflowDirection::Publish,
    "sha256:" + std::string(64, '1'), "stage0", "stage1", 1001),
    std::runtime_error);
  BOOST_CHECK_THROW(
    runtime.registerHostPlaintextLease("host-1", [] {}), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ProtectedRuntimeBindingConsistencyGrantsNoAuthority)
{
  const auto expected = binding();
  ProtectedRuntime runtime(expected);

  // Structural consistency is checked and passes...
  BOOST_CHECK_NO_THROW(runtime.verifyBindingConsistency(expected, 1000));
  // ...but it grants no execution authority: state stays NoGrant, so every
  // authorized operation still fails closed.
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::NoGrant);
  BOOST_CHECK_THROW(runtime.authorizeDataflow(
    ProtectedDataflowDirection::Publish,
    "sha256:" + std::string(64, '1'), "stage0", "stage1", 1001),
    std::runtime_error);
  BOOST_CHECK_THROW(
    runtime.registerDevicePlaintextLease("device-1", [] {}),
    std::runtime_error);
  try {
    runtime.verifyGrant(expected, 1001);
    BOOST_FAIL("verifyGrant must fail closed without configured credentials");
  }
  catch (const std::runtime_error& error) {
    BOOST_CHECK(hasToken(error, "DI_PROTECTED_GRANT_UNAVAILABLE"));
  }

  // Cleanup on an un-authorized runtime still drains cleanly.
  BOOST_CHECK_NO_THROW(runtime.cancel("no authority granted"));
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::Zeroized);
}

BOOST_AUTO_TEST_CASE(ProtectedRuntimeFailsClosedOnBindingSubstitution)
{
  const auto expected = binding();
  auto substituted = expected;
  substituted.provider = "/provider/P1";
  ProtectedRuntime runtime(expected);

  BOOST_CHECK_THROW(runtime.verifyBindingConsistency(substituted, 1000),
                    std::runtime_error);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::FailedClosed);
  BOOST_CHECK_THROW(runtime.authorizeDataflow(
    ProtectedDataflowDirection::Fetch,
    "sha256:" + std::string(64, '2'), "stage-prev", "stage0", 1001),
    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ProtectedRuntimeFailsClosedOnExpiry)
{
  const auto expected = binding();
  ProtectedRuntime runtime(expected);

  BOOST_CHECK_THROW(runtime.verifyBindingConsistency(expected, 5001),
                    std::runtime_error);
  BOOST_CHECK(runtime.state() == ProtectedRuntimeState::FailedClosed);
  BOOST_CHECK_THROW(runtime.authorizeDataflow(
    ProtectedDataflowDirection::Publish,
    "sha256:" + std::string(64, '1'), "stage0", "stage1", 1002),
    std::runtime_error);
}

} // namespace ndnsf::di::test
