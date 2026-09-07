// T005-B Requester Grant Publication — Spec182GrantClient/* (case-manifest
// card T005-B; suite registered in this file with the two pre-existing
// module-level cases migrated into it).
//
// NativeGrantClient is the requester-side acquisition boundary: it turns a
// frozen provider grant view into a signed-grant request, runs the policy
// authority in process, publishes the issued grant under the canonical
// KEY-GRANT/v1 exact name through the injected Face publication port, and
// returns the grant binding.  The client is state-free and synchronous:
// every acquire performs exactly one issue plus one publication, a deadline
// that already passed fences the attempt before any side effect, and a
// failed publication leaves no pending success that a late callback could
// revive (runtime-boundaries Cancellation and Observer Contract:
// "只消费/忽略，不复活").  Crypto (requester signature, recipient-key
// envelope, authority signature) stays injected through the authority issue
// port; the verifier envelope and real Provider consumption are T016.

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantClient.hpp"

#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
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

std::uint64_t nowMs()
{
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

std::chrono::system_clock::time_point deadlineIn(std::int64_t seconds)
{
  return std::chrono::system_clock::now() + std::chrono::seconds(seconds);
}

// Port-call counters shared by the authority issue port and the publication
// port of one client under test.
struct Counters
{
  unsigned issue = 0;
  unsigned publish = 0;
};

// One frozen, complete grant view.  Digests are distinct so that digest
// routing (which digest lands in the canonical name) stays observable.
NativeProviderGrantView completeView()
{
  NativeProviderGrantView view;
  view.provider = "/provider/a";
  view.role = "/role/0";
  view.planCoreDigest = digest("plan-core");
  view.policyDigest = digest("policy");
  view.modelDigest = digest("model");
  view.graphDigest = digest("graph");
  view.artifactDigest = digest("artifact");
  view.requesterIdentity = "/user/a";
  view.requestId = "/request/1";
  view.attempt = 1;
  view.modelManifestDigest = digest("model-manifest");
  view.protectionEpoch = "epoch-1";
  view.expiresAtMs = nowMs() + 60'000;
  return view;
}

// Policy port that echoes the request expiry back and yields a fixed grant
// whose digest/recipient/wire the acquire path must preserve.
NativeKeyGrant frozenGrant(const NativeGrantRequest& request)
{
  return NativeKeyGrant{"unused", digest("grant"), "/recipient/a",
                        "{\"grant\":1}", request.expiresAtMs};
}

// Asserts the registered reason family prefix of a rejected acquisition.
// The full exception text is not a protocol oracle (code-design); the DI
// reason code family is.
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

std::string bare(const std::string& canonicalDigest)
{
  return canonicalDigest.substr(7);
}

// Asserts that a published name is exactly the canonical KEY-GRANT/v1 layout
// with the fixed requestId/epoch vector of completeView(): the fixed vector
// makes the percent-encoded literals ("/request/1" -> %2Frequest%2F1) plain
// constants instead of a re-implementation of the encoding.
void expectCanonicalGrantName(
  const std::string& name, const std::string& modelManifestDigest,
  const std::string& grantDigest)
{
  const std::string prefix = "/user/a/NDNSF-DI/KEY-GRANT/v1/PROVIDER/";
  const std::string suffix =
    "/REQ/%2Frequest%2F1/ATTEMPT/1/PLAN-CORE/" + bare(digest("plan-core")) +
    "/MODEL/" + bare(modelManifestDigest) +
    "/EPOCH/epoch-1/GRANT/" + bare(grantDigest);
  BOOST_REQUIRE(name.size() == prefix.size() + 64 + suffix.size());
  BOOST_CHECK_EQUAL(name.substr(0, prefix.size()), prefix);
  // The PROVIDER component is the 64-hex digest of the Provider identity
  // (verifier-slice contract, covered by its own suite); the surrounding
  // layout and every other component belong to this acquisition contract.
  BOOST_CHECK_EQUAL(name.substr(prefix.size() + 64), suffix);
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182GrantClient)

BOOST_AUTO_TEST_CASE(NativeGrantClientBindsAndPublishesExactName)
{
  const auto d = digest("grant");
  auto authority = std::make_shared<NativeArtifactPolicyAuthority>(
    [&] (const NativeGrantRequest& request) {
      BOOST_CHECK_EQUAL(request.providerIdentity, "/provider/a");
      return NativeKeyGrant{"unused", d, "/recipient/a", "{\"grant\":1}", request.expiresAtMs};
    });
  std::string publishedName;
  std::string publishedWire;
  NativeGrantClient client(
    "/user/a", authority,
    [&] (const std::string& name, const std::string& wire) {
      publishedName = name;
      publishedWire = wire;
      return name;
    });
  NativeProviderGrantView view;
  view.provider = "/provider/a";
  view.role = "/role/0";
  view.planCoreDigest = d;
  view.policyDigest = d;
  view.modelDigest = d;
  view.graphDigest = d;
  view.artifactDigest = d;
  view.requesterIdentity = "/user/a";
  view.requestId = "/request/1";
  view.attempt = 1;
  view.modelManifestDigest = d;
  view.protectionEpoch = "epoch-1";
  view.expiresAtMs = nowMs() + 60'000;
  const auto binding = client.acquire(view, deadlineIn(2));
  BOOST_CHECK_EQUAL(binding.provider, view.provider);
  BOOST_CHECK_EQUAL(binding.grantDigest, d);
  BOOST_CHECK_EQUAL(binding.grantName, publishedName);
  BOOST_CHECK_EQUAL(binding.wireJson, publishedWire);
}

BOOST_AUTO_TEST_CASE(NativeGrantClientRejectsWrongRequester)
{
  auto authority = std::make_shared<NativeArtifactPolicyAuthority>(
    [] (const NativeGrantRequest&) { return NativeKeyGrant{}; });
  NativeGrantClient client("/user/a", authority,
    [] (const std::string& name, const std::string&) { return name; });
  NativeProviderGrantView view;
  view.requesterIdentity = "/user/b";
  BOOST_CHECK_THROW(client.acquire(view, deadlineIn(1)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(GrantClientConstructionRequiresIdentityAuthorityAndPublishPort)
{
  auto authority = std::make_shared<NativeArtifactPolicyAuthority>(frozenGrant);
  // No default requester identity, no default issuer, no default Face
  // publication path: all three ports are mandatory at construction.
  BOOST_CHECK_THROW(NativeGrantClient("", authority,
    [] (const std::string& n, const std::string&) { return n; }),
    std::invalid_argument);
  BOOST_CHECK_THROW(NativeGrantClient("/user/a", nullptr,
    [] (const std::string& n, const std::string&) { return n; }),
    std::invalid_argument);
  BOOST_CHECK_THROW(NativeGrantClient("/user/a", authority, nullptr),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(GrantClientRejectsIncompleteViewBeforeAnyPortSideEffect)
{
  // The view gate runs before the issue port and the publication port:
  // an incomplete or inconsistent view is refused without any side effect.
  Counters counters;
  auto authority = std::make_shared<NativeArtifactPolicyAuthority>(
    [&] (const NativeGrantRequest& request) {
      ++counters.issue;
      return frozenGrant(request);
    });
  NativeGrantClient client("/user/a", authority,
    [&] (const std::string& name, const std::string&) {
      ++counters.publish;
      return name;
    });
  const auto expectRefused = [&] (NativeProviderGrantView view) {
    BOOST_CHECK_THROW(client.acquire(view, deadlineIn(1)), std::invalid_argument);
  };
  auto view = completeView();
  view.requestId.clear();
  expectRefused(view);
  view.requestId = "/request/1";
  view.attempt = 0;
  expectRefused(view);
  view.attempt = 1;
  view.protectionEpoch.clear();
  expectRefused(view);
  view.protectionEpoch = "epoch-1";
  view.expiresAtMs = 0;
  expectRefused(view);
  view.expiresAtMs = nowMs() + 60'000;
  view.planCoreDigest = "not-a-digest";
  expectRefused(view);
  view.planCoreDigest = digest("plan-core");
  view.modelDigest.clear();
  expectRefused(view);
  view.modelDigest = digest("model");
  view.graphDigest = "sha256:0123456789abcdef";  // wrong length
  expectRefused(view);
  view.graphDigest = digest("graph");
  view.artifactDigest.clear();
  expectRefused(view);
  view.artifactDigest = digest("artifact");
  // After every refusal the complete vector still acquires once.
  BOOST_CHECK_NO_THROW(client.acquire(completeView(), deadlineIn(1)));
  BOOST_CHECK_EQUAL(counters.issue, 1u);
  BOOST_CHECK_EQUAL(counters.publish, 1u);
}

BOOST_AUTO_TEST_CASE(GrantClientRejectsExpiredDeadlineBeforeAnySideEffect)
{
  // A deadline that already passed fences the attempt before the issue or
  // publication ports run: an expired wait never resurrects into work or a
  // late success (runtime-boundaries cancel/deadline gate).
  Counters counters;
  auto authority = std::make_shared<NativeArtifactPolicyAuthority>(
    [&] (const NativeGrantRequest& request) {
      ++counters.issue;
      return frozenGrant(request);
    });
  NativeGrantClient client("/user/a", authority,
    [&] (const std::string& name, const std::string&) {
      ++counters.publish;
      return name;
    });
  expectRejectedWithFamily([&] {
    client.acquire(completeView(), std::chrono::system_clock::time_point());
  });
  BOOST_CHECK_EQUAL(counters.issue, 0u);
  BOOST_CHECK_EQUAL(counters.publish, 0u);
}

BOOST_AUTO_TEST_CASE(GrantClientRejectsExpiredGrantThroughAuthorityBeforePublication)
{
  // An already-expired grant cannot become published just because the
  // acquisition deadline is still far away: the authority boundary rejects
  // it with the registered reason family before the publication port runs.
  Counters counters;
  auto authority = std::make_shared<NativeArtifactPolicyAuthority>(
    [&] (const NativeGrantRequest& request) {
      ++counters.issue;
      return frozenGrant(request);
    });
  NativeGrantClient client("/user/a", authority,
    [&] (const std::string& name, const std::string&) {
      ++counters.publish;
      return name;
    });
  auto view = completeView();
  view.expiresAtMs = nowMs() - 10'000;  // grant already dead at issue time
  expectRejectedWithFamily([&] {
    client.acquire(view, deadlineIn(5));
  });
  // The deadline is still in the future, so the family rejection can only
  // come from the authority expiry boundary, which runs before the policy
  // port (T005-A ordering): no issue-port call and no publication.
  BOOST_CHECK_EQUAL(counters.issue, 0u);
  BOOST_CHECK_EQUAL(counters.publish, 0u);
}

BOOST_AUTO_TEST_CASE(GrantClientPublishesCanonicalGrantNameFromViewAndIssuedGrant)
{
  // The exact published name is derived from the view plus the grant the
  // authority actually returned: requester publication identity, Provider,
  // requestId, attempt, plan-core digest, model-manifest digest (falling
  // back to the model digest when the view carries none), epoch and the
  // issued grant digest.  Each acquire performs exactly one issue and one
  // publication, and the binding mirrors the published grant.
  Counters counters;
  auto authority = std::make_shared<NativeArtifactPolicyAuthority>(
    [&] (const NativeGrantRequest& request) {
      ++counters.issue;
      return frozenGrant(request);
    });
  std::string publishedName;
  std::string publishedWire;
  NativeGrantClient client("/user/a", authority,
    [&] (const std::string& name, const std::string& wire) {
      ++counters.publish;
      publishedName = name;
      publishedWire = wire;
      return name;
    });

  const auto view = completeView();
  const auto binding = client.acquire(view, deadlineIn(2));
  expectCanonicalGrantName(publishedName, view.modelManifestDigest,
                           digest("grant"));
  BOOST_CHECK_EQUAL(binding.grantName, publishedName);
  BOOST_CHECK_EQUAL(binding.provider, view.provider);
  BOOST_CHECK_EQUAL(binding.role, view.role);
  BOOST_CHECK_EQUAL(binding.grantDigest, digest("grant"));
  BOOST_CHECK_EQUAL(binding.recipient, "/recipient/a");
  BOOST_CHECK_EQUAL(binding.wireJson, publishedWire);
  BOOST_CHECK_EQUAL(binding.wireJson, "{\"grant\":1}");
  BOOST_CHECK_EQUAL(binding.expiresAtMs, view.expiresAtMs);

  // Deterministic name: the same frozen vector published again yields the
  // identical exact name through a second issue + publication.
  const auto again = client.acquire(view, deadlineIn(2));
  expectCanonicalGrantName(publishedName, view.modelManifestDigest,
                           digest("grant"));
  BOOST_CHECK_EQUAL(again.grantName, publishedName);
  BOOST_CHECK_EQUAL(counters.issue, 2u);
  BOOST_CHECK_EQUAL(counters.publish, 2u);

  // Model-manifest fallback: a view without a model-manifest digest routes
  // the model digest into the MODEL name component (matching the request
  // construction fallback), never the plan-core digest.
  auto bareView = completeView();
  bareView.modelManifestDigest.clear();
  bareView.requestId = "/request/2";
  const auto fallback = client.acquire(bareView, deadlineIn(2));
  const std::string fallbackPrefix = "/user/a/NDNSF-DI/KEY-GRANT/v1/PROVIDER/";
  const std::string fallbackSuffix =
    "/REQ/%2Frequest%2F2/ATTEMPT/1/PLAN-CORE/" + bare(digest("plan-core")) +
    "/MODEL/" + bare(digest("model")) +
    "/EPOCH/epoch-1/GRANT/" + bare(digest("grant"));
  BOOST_REQUIRE(fallback.grantName.size() ==
                fallbackPrefix.size() + 64 + fallbackSuffix.size());
  BOOST_CHECK_EQUAL(fallback.grantName.substr(fallbackPrefix.size() + 64),
                    fallbackSuffix);
  BOOST_CHECK_EQUAL(counters.issue, 3u);
  BOOST_CHECK_EQUAL(counters.publish, 3u);
}

BOOST_AUTO_TEST_CASE(GrantClientMismatchedPublicationIsConsumedOnceAndNeverRevives)
{
  // A publication that reports a different name than the canonical exact
  // name is rejected with the registered reason family and consumed exactly
  // once.  The client keeps no pending state: a later attempt performs a
  // fresh issue + publication, and the stale attempt's outcome never
  // surfaces as success.
  Counters counters;
  unsigned publicationCall = 0;
  auto authority = std::make_shared<NativeArtifactPolicyAuthority>(
    [&] (const NativeGrantRequest& request) {
      ++counters.issue;
      return frozenGrant(request);
    });
  std::string secondPublishedName;
  NativeGrantClient client("/user/a", authority,
    [&] (const std::string& name, const std::string&) {
      ++counters.publish;
      // First publication reports a mismatched (stale/wrong) name.
      if (publicationCall++ == 0) {
        return name + "/stale";
      }
      secondPublishedName = name;
      return name;
    });

  const auto view = completeView();
  expectRejectedWithFamily([&] {
    client.acquire(view, deadlineIn(2));
  });
  BOOST_CHECK_EQUAL(counters.issue, 1u);
  BOOST_CHECK_EQUAL(counters.publish, 1u);

  // No state survived the rejection: the retry is a complete fresh attempt
  // whose binding names only its own publication.
  const auto binding = client.acquire(view, deadlineIn(2));
  expectCanonicalGrantName(binding.grantName, view.modelManifestDigest,
                           digest("grant"));
  BOOST_CHECK_EQUAL(binding.grantName, secondPublishedName);
  BOOST_CHECK_EQUAL(counters.issue, 2u);
  BOOST_CHECK_EQUAL(counters.publish, 2u);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di
