// T005-A InProcess Authority — Spec182GrantAuthority/* (case-manifest card
// T005-A; planned suite registered in this file).
//
// NativeArtifactPolicyAuthority is the validation-owning issuer boundary:
// request/policy construction follows the frozen Python
// ArtifactPolicyAuthority.issue semantics, crypto (requester signature,
// recipient-key envelope, authority signature) is injected through the issue
// port, and no network authority or self-written cipher enters the data
// path.  The envelope-level wrong-recipient/wrong-key checks live in the
// existing NativeGrantVerifier slice (CD-004 REUSE); real Provider
// consumption is T016.

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantClient.hpp"

#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>

#include <chrono>
#include <functional>
#include <string>

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

std::chrono::system_clock::time_point fixedClock(std::uint64_t nowMs)
{
  return std::chrono::system_clock::time_point(std::chrono::milliseconds(nowMs));
}

NativeKeyGrant completeGrant(std::uint64_t expiresAtMs)
{
  return NativeKeyGrant{"/grant/1", digest("grant-1"), "/provider/a",
                        "{\"grant\":1}", expiresAtMs};
}

NativeGrantRequest frozenRequest(std::uint64_t expiresAtMs)
{
  NativeGrantRequest request;
  request.requesterIdentity = "/user/a";
  request.providerIdentity = "/provider/a";
  request.requestId = "/request/1";
  request.attempt = 1;
  request.planCoreDigest = digest("plan-core");
  request.modelManifestDigest = digest("model-manifest");
  request.protectionEpoch = "epoch-1";
  request.artifactDigest = digest("artifact");
  request.expiresAtMs = expiresAtMs;
  return request;
}

// Asserts the registered reason family prefix of the rejection.  The full
// exception text is not a protocol oracle (code-design); the DI reason code
// family is.
void expectRejectedWithFamily(const std::function<void()>& operation)
{
  try {
    operation();
    BOOST_FAIL("expected a DI_PROTECTED_GRANT_REJECTED rejection");
  } catch (const std::runtime_error& error) {
    BOOST_CHECK_EQUAL(std::string(error.what()).substr(0, 28),
                      "DI_PROTECTED_GRANT_REJECTED:");
  }
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182GrantAuthority)

BOOST_AUTO_TEST_CASE(IssueAcceptsFrozenRequestAndFixedClockVector)
{
  // One frozen request/clock vector: expiry in the future and distinct
  // requester/Provider identities.  The policy port receives the request
  // unchanged, and the returned grant must carry the request expiry back.
  NativeGrantRequest received;
  NativeArtifactPolicyAuthority authority(
    [&received] (const NativeGrantRequest& request) {
      received = request;
      return NativeKeyGrant{"/grant/1", digest("grant-1"), "/provider/a",
                            "{\"grant\":1}", request.expiresAtMs};
    });
  const auto request = frozenRequest(4'000);
  const auto grant = authority.issue(request, fixedClock(1'000));
  BOOST_CHECK_EQUAL(received.requesterIdentity, request.requesterIdentity);
  BOOST_CHECK_EQUAL(received.providerIdentity, request.providerIdentity);
  BOOST_CHECK_EQUAL(received.requestId, request.requestId);
  BOOST_CHECK_EQUAL(received.attempt, request.attempt);
  BOOST_CHECK_EQUAL(received.planCoreDigest, request.planCoreDigest);
  BOOST_CHECK_EQUAL(received.modelManifestDigest, request.modelManifestDigest);
  BOOST_CHECK_EQUAL(received.protectionEpoch, request.protectionEpoch);
  BOOST_CHECK_EQUAL(received.artifactDigest, request.artifactDigest);
  BOOST_CHECK_EQUAL(received.expiresAtMs, request.expiresAtMs);
  BOOST_CHECK_EQUAL(grant.expiresAtMs, request.expiresAtMs);
  BOOST_CHECK_EQUAL(grant.grantName, "/grant/1");
  BOOST_CHECK_EQUAL(grant.grantDigest, digest("grant-1"));
  BOOST_CHECK_EQUAL(grant.recipient, "/provider/a");
  // Deterministic vector: the same fixed clock/request/issues produce the
  // same grant through a pure policy port.
  const auto again = authority.issue(request, fixedClock(1'000));
  BOOST_CHECK_EQUAL(again.grantDigest, grant.grantDigest);
  BOOST_CHECK_EQUAL(again.wireJson, grant.wireJson);
}

BOOST_AUTO_TEST_CASE(IssueRejectsSelfAuthorizedRequesterWithReasonFamily)
{
  // A requester cannot authorize a grant to its own identity (frozen Python
  // "grant requester cannot be the selected Provider").
  NativeArtifactPolicyAuthority authority(
    [] (const NativeGrantRequest& request) {
      return completeGrant(request.expiresAtMs);
    });
  auto request = frozenRequest(4'000);
  request.providerIdentity = request.requesterIdentity;
  expectRejectedWithFamily([&] {
    authority.issue(request, fixedClock(1'000));
  });
  // The same identity strings in a legitimate cross-identity request still
  // pass the boundary once the identity pair differs.
  request.providerIdentity = "/provider/a";
  BOOST_CHECK_NO_THROW(authority.issue(request, fixedClock(1'000)));
}

BOOST_AUTO_TEST_CASE(IssueRejectsIncompleteRequestIdentity)
{
  // Incomplete identities are refused before any policy port runs: an empty
  // requester/Provider/requestId, a zero attempt, non-canonical digests, an
  // empty protection epoch or a zero expiry cannot become a grant request.
  NativeArtifactPolicyAuthority authority(
    [] (const NativeGrantRequest& request) {
      return completeGrant(request.expiresAtMs);
    });
  BOOST_CHECK_THROW(authority.issue(NativeGrantRequest{}, fixedClock(1'000)),
                    std::invalid_argument);
  auto request = frozenRequest(4'000);
  request.requesterIdentity.clear();
  BOOST_CHECK_THROW(authority.issue(request, fixedClock(1'000)),
                    std::invalid_argument);
  request.requesterIdentity = "/user/a";
  request.requestId.clear();
  BOOST_CHECK_THROW(authority.issue(request, fixedClock(1'000)),
                    std::invalid_argument);
  request.requestId = "/request/1";
  request.attempt = 0;
  BOOST_CHECK_THROW(authority.issue(request, fixedClock(1'000)),
                    std::invalid_argument);
  request.attempt = 1;
  request.planCoreDigest = "not-a-digest";
  BOOST_CHECK_THROW(authority.issue(request, fixedClock(1'000)),
                    std::invalid_argument);
  request.planCoreDigest = digest("plan-core");
  request.artifactDigest.clear();
  BOOST_CHECK_THROW(authority.issue(request, fixedClock(1'000)),
                    std::invalid_argument);
  request.artifactDigest = digest("artifact");
  request.protectionEpoch.clear();
  BOOST_CHECK_THROW(authority.issue(request, fixedClock(1'000)),
                    std::invalid_argument);
  request.protectionEpoch = "epoch-1";
  request.expiresAtMs = 0;
  BOOST_CHECK_THROW(authority.issue(request, fixedClock(1'000)),
                    std::invalid_argument);
  // After every rejection the frozen vector still issues.
  request.expiresAtMs = 4'000;
  BOOST_CHECK_NO_THROW(authority.issue(request, fixedClock(1'000)));
}

BOOST_AUTO_TEST_CASE(IssueRejectsExpiredGrantAtFixedClockBoundary)
{
  // Fixed clock vectors, no wall clock: expiry at or before now is refused,
  // one millisecond past now is issued.
  NativeArtifactPolicyAuthority authority(
    [] (const NativeGrantRequest&) {
      return NativeKeyGrant{"/grant/1", digest("grant-1"), "/provider/a",
                            "{\"grant\":1}", 4'000};
    });
  const auto request = frozenRequest(4'000);
  expectRejectedWithFamily([&] {
    authority.issue(request, fixedClock(4'000));
  });
  expectRejectedWithFamily([&] {
    authority.issue(request, fixedClock(8'000));
  });
  const auto grant = authority.issue(request, fixedClock(3'999));
  BOOST_CHECK_EQUAL(grant.expiresAtMs, request.expiresAtMs);
}

BOOST_AUTO_TEST_CASE(IssueRejectsIncompleteIssuerResponseWithReasonFamily)
{
  // Wrong-key/recipient/expiry surface at the issuer boundary: a policy port
  // that returns an incomplete or substituted grant (empty name or
  // recipient, non-canonical grant digest, empty wire, or a different expiry
  // than the request) is rejected with the registered reason family.  The
  // cryptographic wrong-recipient-key rejection belongs to the verifier
  // envelope (NativeGrantVerifier) and Provider consumption (T016).
  const auto issueWith = [] (const NativeKeyGrant& returned) {
    NativeArtifactPolicyAuthority authority(
      [&returned] (const NativeGrantRequest&) { return returned; });
    return authority.issue(frozenRequest(4'000), fixedClock(1'000));
  };
  expectRejectedWithFamily([&] {
    issueWith(NativeKeyGrant{});
  });
  const auto requestExpiry = frozenRequest(4'000).expiresAtMs;
  expectRejectedWithFamily([&] {
    issueWith(NativeKeyGrant{"/grant/1", digest("grant-1"), "", "{}",
                             requestExpiry});
  });
  expectRejectedWithFamily([&] {
    issueWith(NativeKeyGrant{"/grant/1", "not-a-digest", "/provider/a", "{}",
                             requestExpiry});
  });
  expectRejectedWithFamily([&] {
    issueWith(NativeKeyGrant{"", digest("grant-1"), "/provider/a", "{}",
                             requestExpiry});
  });
  expectRejectedWithFamily([&] {
    issueWith(NativeKeyGrant{"/grant/1", digest("grant-1"), "/provider/a", "",
                             requestExpiry});
  });
  expectRejectedWithFamily([&] {
    issueWith(NativeKeyGrant{"/grant/1", digest("grant-1"), "/provider/a",
                             "{}", requestExpiry + 1});
  });
  // Issuer-side policy rejections (epoch/model-manifest/residency policy in
  // the injected port) propagate verbatim with the registered family; the
  // authority never swallows or rewrites them.
  NativeArtifactPolicyAuthority policyAuthority(
    [] (const NativeGrantRequest&) -> NativeKeyGrant {
      throw std::runtime_error(
        "DI_PROTECTED_GRANT_REJECTED: model manifest is not authorized");
    });
  expectRejectedWithFamily([&] {
    policyAuthority.issue(frozenRequest(4'000), fixedClock(1'000));
  });
}

BOOST_AUTO_TEST_CASE(AuthorityConstructionRejectsEmptyIssuePort)
{
  // The issue port is mandatory at construction: no default issuer and no
  // authority without a policy sink.
  BOOST_CHECK_THROW(NativeArtifactPolicyAuthority(nullptr),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di
