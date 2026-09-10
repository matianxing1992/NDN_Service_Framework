#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"

#include <boost/test/unit_test.hpp>
#include <openssl/evp.h>

#include <memory>

namespace ndnsf::di::test {
namespace {

std::shared_ptr<EVP_PKEY>
key(std::uint8_t seed)
{
  std::string bytes(32, static_cast<char>(seed));
  auto* raw = EVP_PKEY_new_raw_private_key(
    EVP_PKEY_ED25519, nullptr,
    reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
  BOOST_REQUIRE(raw != nullptr);
  return {raw, EVP_PKEY_free};
}

NativeSignedGrantRequest
request()
{
  NativeSignedGrantRequest value;
  value.providerIdentity = "/provider/a";
  value.requestId = "/NDNSF/DI/REQUEST/r1";
  value.attempt = 1;
  value.planCoreDigest = "sha256:1111111111111111111111111111111111111111111111111111111111111111";
  value.grantViewDigest = "sha256:2222222222222222222222222222222222222222222222222222222222222222";
  value.modelManifestDigest = "sha256:3333333333333333333333333333333333333333333333333333333333333333";
  value.protectionEpoch = "epoch-1";
  value.requesterIdentity = "/requester/a";
  value.issuedAtMs = 1000;
  value.requesterSignature = std::string(128, 'a');
  return value;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec182NativeAuthority)

BOOST_AUTO_TEST_CASE(AuthorityRequestWireIsCanonicalAndRoundTrips)
{
  NativeGrantAuthorityRequest envelope;
  envelope.request = request();
  envelope.expiresAtMs = 60000;
  envelope.publishedManifestJson = "{\"schema\":\"fixture\"}";
  const auto wire = nativeGrantAuthorityRequestJson(envelope);
  const auto decoded = nativeGrantAuthorityRequestFromJson(wire);
  BOOST_CHECK_EQUAL(decoded.request.providerIdentity, envelope.request.providerIdentity);
  BOOST_CHECK_EQUAL(decoded.request.requesterSignature, envelope.request.requesterSignature);
  BOOST_CHECK_EQUAL(decoded.expiresAtMs, envelope.expiresAtMs);
  BOOST_CHECK_EQUAL(decoded.publishedManifestJson, envelope.publishedManifestJson);
  BOOST_CHECK_THROW(nativeGrantAuthorityRequestFromJson(wire + " "), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(KeyGrantWireIsCanonicalAndRoundTrips)
{
  NativeKeyGrant grant;
  grant.grantName = "/authority/grant/1";
  grant.grantDigest = "sha256:4444444444444444444444444444444444444444444444444444444444444444";
  grant.recipient = "/provider/a";
  grant.wireJson = "{\"wire\":true}";
  grant.expiresAtMs = 60000;
  const auto wire = nativeKeyGrantJson(grant);
  const auto decoded = nativeKeyGrantFromJson(wire);
  BOOST_CHECK_EQUAL(decoded.grantName, grant.grantName);
  BOOST_CHECK_EQUAL(decoded.grantDigest, grant.grantDigest);
  BOOST_CHECK_EQUAL(decoded.wireJson, grant.wireJson);
  BOOST_CHECK_THROW(nativeKeyGrantFromJson(wire + "\n"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RemoteAuthorityConstructorHasNoIssuerKeyMaterial)
{
  auto requesterKey = key(0x42);
  auto authorityKey = key(0x24);
  std::string authorityPublic(32, '\0');
  std::size_t size = authorityPublic.size();
  BOOST_REQUIRE_EQUAL(EVP_PKEY_get_raw_public_key(
    authorityKey.get(), reinterpret_cast<unsigned char*>(authorityPublic.data()), &size), 1);
  std::shared_ptr<const std::atomic<bool>> cancelled =
    std::make_shared<const std::atomic<bool>>(false);
  NativeAuthenticatedGrantClient client(
    "/requester/a", requesterKey, "/authority/a", authorityPublic, "epoch-1",
    [] (const NativeSignedGrantRequest&, const std::string&, std::uint64_t,
        const NativeGrantControl&) { return NativeKeyGrant{}; },
    [] (const std::string&, const std::string&, const NativeGrantControl&) {
      return std::string("/authority/grant/1");
    });
  BOOST_CHECK_EQUAL(client.protectionEpoch(), "epoch-1");
  BOOST_CHECK_EQUAL(client.requesterIdentity(), "/requester/a");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::test
