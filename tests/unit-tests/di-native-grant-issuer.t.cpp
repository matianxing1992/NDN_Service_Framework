#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantVerifier.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include <boost/test/unit_test.hpp>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <fstream>
#include <cstdlib>

namespace ndnsf::di {
namespace {
using Key = std::shared_ptr<EVP_PKEY>;
Key ed(char seed)
{
  const std::string raw(32, seed);
  return Key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
    reinterpret_cast<const unsigned char*>(raw.data()), raw.size()), EVP_PKEY_free);
}
std::string publicBytes(const Key& key)
{
  std::string raw(32, '\0'); std::size_t size = raw.size();
  if (!key || EVP_PKEY_get_raw_public_key(key.get(), reinterpret_cast<unsigned char*>(raw.data()), &size) != 1 || size != 32)
    throw std::runtime_error("test key extraction failed");
  return raw;
}
Key publicKey(const Key& key)
{
  const auto raw = publicBytes(key);
  return Key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
    reinterpret_cast<const unsigned char*>(raw.data()), raw.size()), EVP_PKEY_free);
}
NativeSignedGrantRequest request()
{
  NativeSignedGrantRequest r;
  r.providerIdentity = "/provider/a"; r.requesterIdentity = "/user/a";
  r.requestId = "/request/one"; r.attempt = 1; r.issuedAtMs = 1000;
  r.planCoreDigest = "sha256:" + std::string(64, '1');
  r.grantViewDigest = "sha256:" + std::string(64, '2');
  r.modelManifestDigest = "sha256:" + std::string(64, '3'); r.protectionEpoch = "epoch-1";
  return r;
}
NativeGrantIssuerConfig config()
{
  NativeGrantIssuerConfig c;
  c.authorityIdentity = "/authority"; c.requesterIdentity = "/user/a";
  c.protectionEpoch = "epoch-1"; c.keyId = "model-key";
  c.authorityPrivateKey = ed('a'); c.requesterPublicKey = publicKey(ed('b'));
  c.allowedModelManifests.insert(request().modelManifestDigest);
  c.recipientPublicKeys.emplace("/provider/a", publicKey(ed('c')));
  c.contentKey = [](const auto& model, const auto& epoch) {
    BOOST_CHECK_EQUAL(model, request().modelManifestDigest);
    BOOST_CHECK_EQUAL(epoch, "epoch-1");
    return std::vector<std::uint8_t>(32, 42);
  };
  return c;
}
NativeGrantVerificationResult unwrap(const NativeKeyGrant& grant, const NativeRecipientKey& recipient)
{
  const auto r = request();
  return verifyAndUnwrapNativeGrant(grant.wireJson, publicBytes(ed('a')), recipient,
    r.providerIdentity, r.requestId, r.attempt, r.planCoreDigest, r.modelManifestDigest,
    r.protectionEpoch, 1001, "/authority", grant.grantDigest);
}
void recordOracle(const char* kind, const NativeKeyGrant& grant, const NativeSignedGrantRequest& request)
{
  if (const char* path = std::getenv("NDNSF_GRANT_ISSUER_ORACLE_OUTPUT")) {
    std::ofstream output(path, std::ios::app);
    if (!output) throw std::runtime_error("cannot write grant oracle output");
    output << NativeJson{{"kind", kind}, {"wire", grant.wireJson},
      {"request", request.signingBytes()}, {"signature", request.requesterSignature}}.dump() << '\n';
  }
}
}

BOOST_AUTO_TEST_SUITE(Spec182GrantIssuer)

BOOST_AUTO_TEST_CASE(PublishedManifestMustBindAnExplicitlyAuthorizedSource)
{
  auto policy = config();
  const auto original = request().modelManifestDigest;
  NativeGrantPublicationSource source{"model", nativePlanningDigest("content"), nativePlanningDigest("source"),
    "", nativePlanningDigest("profile")};
  policy.publicationSources.emplace(original, source);
  unsigned keyReads = 0;
  policy.contentKey = [&](const auto& manifest, const auto&) {
    ++keyReads; BOOST_CHECK_EQUAL(manifest, original);
    return std::vector<std::uint8_t>(32, 42);
  };
  NativeArtifactGrantIssuer issuer(policy);
  const NativeJson root{{"schema", "ndnsf-di-canonical-model-manifest-v1"}, {"state", "ACTIVE"},
    {"modelName", source.modelName}, {"modelIdentityDigest", source.modelContentDigest},
    {"artifactProfileDigest", source.artifactProfileDigest},
    {"metadata", {{"packageManifestDigest", original}, {"canonicalSourceDigest", source.canonicalSourceDigest}}}};
  const auto wire = root.dump();
  auto r = request(); r.modelManifestDigest = nativePlanningDigest(wire);
  const auto signedRequest = r.sign(*ed('b'));
  BOOST_CHECK_THROW(issuer.issue(signedRequest, 1000, 2000), std::runtime_error);
  const auto issued = issuer.issue(signedRequest, 1000, 2000, wire);
  const auto opened = verifyAndUnwrapNativeGrant(issued.wireJson, publicBytes(ed('a')),
    {NativeRecipientKey::Kind::Ed25519Seed, std::string(32, 'c')}, r.providerIdentity, r.requestId,
    r.attempt, r.planCoreDigest, r.modelManifestDigest, r.protectionEpoch, 1001, "/authority", issued.grantDigest);
  BOOST_REQUIRE_MESSAGE(opened.verified, opened.reason);
  BOOST_CHECK(opened.contentKey == std::vector<std::uint8_t>(32, 42));
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    auto changed = root;
    if (mutation == 0) changed["metadata"]["canonicalSourceDigest"] = nativePlanningDigest("foreign");
    if (mutation == 1) changed["metadata"]["packageManifestDigest"] = nativePlanningDigest("foreign");
    if (mutation == 2) changed["modelIdentityDigest"] = nativePlanningDigest("foreign");
    if (mutation == 3) changed["artifactProfileDigest"] = nativePlanningDigest("foreign");
    if (mutation == 4) changed["metadata"]["canonicalInitializerObjectDigest"] = nativePlanningDigest("foreign");
    auto rejected = request(); rejected.modelManifestDigest = nativePlanningDigest(changed.dump());
    BOOST_CHECK_THROW(issuer.issue(rejected.sign(*ed('b')), 1000, 2000, changed.dump()), std::runtime_error);
  }
  BOOST_CHECK_EQUAL(keyReads, 1U);
}

// Migration boundary: legacy inline materialObjects roots above the ordinary
// RequestMessage authority cap are intentionally rejected.  New preparation
// receipts must use the authenticated material-receipt indirection instead.
BOOST_AUTO_TEST_CASE(LegacyInlineRootAboveAuthorityCapIsRejectedBeforeRequestTransport)
{
  NativeGrantAuthorityRequest envelope;
  envelope.request = request();
  envelope.expiresAtMs = 60000;
  envelope.publishedManifestJson.assign(NativeGrantInlineManifestMaxBytes + 1, 'x');
  BOOST_CHECK_THROW(nativeGrantAuthorityRequestJson(envelope), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SignedIssuerReachesIndependentProviderUnwrap)
{
  const auto unsignedRequest = request();
  const auto signedRequest = unsignedRequest.sign(*ed('b'));
  BOOST_CHECK(unsignedRequest.requesterSignature.empty());
  BOOST_CHECK_EQUAL(signedRequest.requesterSignature.size(), 128);
  const auto issued = NativeArtifactGrantIssuer(config()).issue(signedRequest, 1000, 2000);
  recordOracle("ed25519", issued, signedRequest);
  BOOST_CHECK_EQUAL(issued.recipient, signedRequest.providerIdentity);
  BOOST_CHECK_EQUAL(issued.expiresAtMs, 2000);
  const auto wire = NativeJson::parse(issued.wireJson);
  BOOST_CHECK_EQUAL(issued.wireJson, wire.dump());
  BOOST_CHECK_NO_THROW(detail::verifyNativeIssuedGrant(issued, signedRequest, "/authority", publicBytes(ed('a')), 1001, 2000));
  auto wrongAnswer = issued; wrongAnswer.recipient = "/wrong";
  BOOST_CHECK_THROW(detail::verifyNativeIssuedGrant(wrongAnswer, signedRequest, "/authority", publicBytes(ed('a')), 1001, 2000), std::runtime_error);
  BOOST_CHECK_THROW(detail::verifyNativeIssuedGrant(issued, signedRequest, "/wrong", publicBytes(ed('a')), 1001, 2000), std::runtime_error);
  BOOST_CHECK_THROW(detail::verifyNativeIssuedGrant(issued, signedRequest, "/authority", publicBytes(ed('d')), 1001, 2000), std::runtime_error);
  auto unicode = request(); unicode.requestId = "/request/中文";
  const auto unicodeSigned = unicode.sign(*ed('b'));
  const auto unicodeGrant = NativeArtifactGrantIssuer(config()).issue(unicodeSigned, 1000, 2000);
  recordOracle("unicode", unicodeGrant, unicodeSigned);
  BOOST_CHECK(unicodeSigned.signingBytes().find("中文") != std::string::npos);
  BOOST_CHECK_EQUAL(wire.at("wrappedContentKey").at("alg").get<std::string>(), "X25519-AESGCM-SHA256");
  const auto opened = unwrap(issued, {NativeRecipientKey::Kind::Ed25519Seed, std::string(32, 'c')});
  BOOST_REQUIRE_MESSAGE(opened.verified, opened.reason);
  BOOST_CHECK(opened.contentKey == std::vector<std::uint8_t>(32, 42));
  BOOST_CHECK(!unwrap(issued, {NativeRecipientKey::Kind::Ed25519Seed, std::string(32, 'd')}).verified);
  auto changed = issued;
  auto tampered = wire;
  tampered["providerIdentity"] = "/provider/other";
  changed.wireJson = tampered.dump();
  BOOST_CHECK(!unwrap(changed, {NativeRecipientKey::Kind::Ed25519Seed, std::string(32, 'c')}).verified);
  tampered = wire; tampered["authoritySignature"] = std::string(128, '0');
  changed.wireJson = tampered.dump();
  BOOST_CHECK(!unwrap(changed, {NativeRecipientKey::Kind::Ed25519Seed, std::string(32, 'c')}).verified);
}

BOOST_AUTO_TEST_CASE(PolicyAndRequestTamperingFailBeforeKeyConsumption)
{
  auto c = config(); unsigned reads = 0;
  c.contentKey = [&](const auto&, const auto&) { ++reads; return std::vector<std::uint8_t>(32, 42); };
  NativeArtifactGrantIssuer issuer(c);
  auto r = request().sign(*ed('b'));
  BOOST_CHECK_THROW(issuer.issue(request(), 1000, 2000), std::runtime_error);
  auto changed = r; changed.grantViewDigest.back() = '4';
  BOOST_CHECK_THROW(issuer.issue(changed, 1000, 2000), std::runtime_error);
  for (unsigned field = 0; field < 5; ++field) {
    changed = request();
    if (field == 0) changed.modelManifestDigest.back() = '4';
    if (field == 1) changed.protectionEpoch = "other";
    if (field == 2) changed.allowedResidencyTiers = {"UNAUTHORIZED"};
    if (field == 3) changed.requesterIdentity = "/other";
    if (field == 4) changed.providerIdentity = "/unknown";
    changed = changed.sign(*ed('b'));
    BOOST_CHECK_THROW(issuer.issue(changed, 1000, 2000), std::runtime_error);
  }
  BOOST_CHECK_THROW(issuer.issue(request().sign(*ed('d')), 1000, 2000), std::runtime_error);
  BOOST_CHECK_THROW(issuer.issue(r, 1000, 1000), std::runtime_error);
  BOOST_CHECK_THROW(issuer.issue(r, 999, 2000), std::runtime_error);
  BOOST_CHECK_EQUAL(reads, 0);
  auto missing = c; missing.contentKey = [](const auto&, const auto&) { return std::vector<std::uint8_t>{}; };
  BOOST_CHECK_THROW(NativeArtifactGrantIssuer(missing).issue(r, 1000, 2000), std::runtime_error);
  auto same = c; same.authorityPrivateKey = ed('b');
  BOOST_CHECK_THROW(NativeArtifactGrantIssuer{same}, std::runtime_error);
  same = c; same.authorityIdentity = same.requesterIdentity;
  BOOST_CHECK_THROW(NativeArtifactGrantIssuer{same}, std::runtime_error);
  BOOST_CHECK_NO_THROW(issuer.issue(r, 1000, 2000));
  BOOST_CHECK_EQUAL(reads, 1);
}

BOOST_AUTO_TEST_CASE(P256AndRawX25519RecipientsUseSameWireContract)
{
  auto c = config();
  // Fixed public test scalar 7, also used by the independent Python oracle.
  std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)> rawEc(EC_KEY_new_by_curve_name(NID_X9_62_prime256v1), EC_KEY_free);
  std::unique_ptr<BIGNUM, decltype(&BN_free)> scalar(BN_new(), BN_free);
  BOOST_REQUIRE(rawEc && scalar && BN_set_word(scalar.get(), 7) == 1);
  const auto* group = EC_KEY_get0_group(rawEc.get());
  std::unique_ptr<EC_POINT, decltype(&EC_POINT_free)> point(EC_POINT_new(group), EC_POINT_free);
  BOOST_REQUIRE(point && EC_POINT_mul(group, point.get(), scalar.get(), nullptr, nullptr, nullptr) == 1 &&
    EC_KEY_set_private_key(rawEc.get(), scalar.get()) == 1 && EC_KEY_set_public_key(rawEc.get(), point.get()) == 1);
  Key ec(EVP_PKEY_new(), EVP_PKEY_free);
  BOOST_REQUIRE(ec && EVP_PKEY_assign_EC_KEY(ec.get(), rawEc.get()) == 1);
  rawEc.release();
  c.recipientPublicKeys["/provider/a"] = ec;
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
  BOOST_REQUIRE(bio && PEM_write_bio_PrivateKey(bio.get(), ec.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
  char* data = nullptr; const auto size = BIO_get_mem_data(bio.get(), &data);
  const auto issued = NativeArtifactGrantIssuer(c).issue(request().sign(*ed('b')), 1000, 2000);
  recordOracle("p256", issued, request().sign(*ed('b')));
  auto opened = unwrap(issued, {NativeRecipientKey::Kind::EcP256Pem, std::string(data, size)});
  BOOST_REQUIRE_MESSAGE(opened.verified, opened.reason);
  BOOST_CHECK(opened.contentKey == std::vector<std::uint8_t>(32, 42));
  // Raw X25519 public key derived from the same deterministic Ed25519 fixture.
  const std::string seed(32, 'c'); unsigned char derived[64];
  SHA512(reinterpret_cast<const unsigned char*>(seed.data()), seed.size(), derived);
  Key x(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, derived, 32), EVP_PKEY_free);
  c.recipientPublicKeys["/provider/a"] = x;
  const auto rawGrant = NativeArtifactGrantIssuer(c).issue(request().sign(*ed('b')), 1000, 2000);
  recordOracle("x25519", rawGrant, request().sign(*ed('b')));
  opened = unwrap(rawGrant, {NativeRecipientKey::Kind::Ed25519Seed, seed});
  BOOST_REQUIRE_MESSAGE(opened.verified, opened.reason);
  BOOST_CHECK(opened.contentKey == std::vector<std::uint8_t>(32, 42));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndnsf::di
