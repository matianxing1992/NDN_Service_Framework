#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp"
#include "tests/boost-test.hpp"

#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <memory>

namespace ndnsf_distributed_repo::test {

namespace {

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

struct Fixture
{
  ArtifactLimits limits;
  PkeyPtr key{nullptr, &EVP_PKEY_free};
  ArtifactReference artifact;
  ArtifactManifestPage page;
  ArtifactChunk chunk;
  SignedArtifactRoot signedRoot;
  ArtifactCapability capability;
  ArtifactManifestTrustPolicy policy;
  std::vector<uint8_t> payload;
};

std::string
publicKeyPem(EVP_PKEY* key)
{
  BIO* raw = BIO_new(BIO_s_mem());
  BOOST_REQUIRE(raw != nullptr);
  BOOST_REQUIRE_EQUAL(PEM_write_bio_PUBKEY(raw, key), 1);
  char* data = nullptr;
  const auto size = BIO_get_mem_data(raw, &data);
  std::string pem(data, static_cast<size_t>(size));
  BIO_free(raw);
  return pem;
}

std::vector<uint8_t>
sign(EVP_PKEY* key, const std::vector<uint8_t>& bytes,
     const std::string& algorithm = "rsa-sha256")
{
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  BOOST_REQUIRE(context != nullptr);
  size_t size = 0;
  std::vector<uint8_t> signature;
  if (algorithm == "ed25519") {
    BOOST_REQUIRE_EQUAL(
      EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key), 1);
    BOOST_REQUIRE_EQUAL(
      EVP_DigestSign(context, nullptr, &size, bytes.data(), bytes.size()), 1);
    signature.resize(size);
    BOOST_REQUIRE_EQUAL(
      EVP_DigestSign(
        context, signature.data(), &size, bytes.data(), bytes.size()), 1);
  }
  else {
    BOOST_REQUIRE_EQUAL(
      EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key), 1);
    BOOST_REQUIRE_EQUAL(
      EVP_DigestSignUpdate(context, bytes.data(), bytes.size()), 1);
    BOOST_REQUIRE_EQUAL(EVP_DigestSignFinal(context, nullptr, &size), 1);
    signature.resize(size);
    BOOST_REQUIRE_EQUAL(
      EVP_DigestSignFinal(context, signature.data(), &size), 1);
  }
  signature.resize(size);
  EVP_MD_CTX_free(context);
  return signature;
}

PkeyPtr
generateKey(const std::string& algorithm)
{
  const int keyType =
    algorithm == "rsa-sha256" ? EVP_PKEY_RSA :
    algorithm == "ecdsa-sha256" ? EVP_PKEY_EC : EVP_PKEY_ED25519;
  EVP_PKEY_CTX* keyContext = EVP_PKEY_CTX_new_id(keyType, nullptr);
  BOOST_REQUIRE(keyContext != nullptr);
  BOOST_REQUIRE_EQUAL(EVP_PKEY_keygen_init(keyContext), 1);
  if (algorithm == "rsa-sha256") {
    BOOST_REQUIRE_EQUAL(
      EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext, 2048), 1);
  }
  else if (algorithm == "ecdsa-sha256") {
    BOOST_REQUIRE_EQUAL(
      EVP_PKEY_CTX_set_ec_paramgen_curve_nid(
        keyContext, NID_X9_62_prime256v1), 1);
  }
  EVP_PKEY* rawKey = nullptr;
  BOOST_REQUIRE_EQUAL(EVP_PKEY_keygen(keyContext, &rawKey), 1);
  EVP_PKEY_CTX_free(keyContext);
  return PkeyPtr(rawKey, &EVP_PKEY_free);
}

Fixture
makeFixture()
{
  Fixture fixture;
  fixture.limits.maxManifestPages = 8;
  fixture.limits.maxManifestChunks = 8;
  fixture.limits.maxCryptographicOperations = 32;
  fixture.payload = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};

  fixture.key = generateKey("rsa-sha256");

  fixture.artifact.logicalName = "/publisher/artifact";
  fixture.artifact.contentDigest = artifactSha256Hex(fixture.payload);
  fixture.artifact.sizeBytes = fixture.payload.size();
  fixture.artifact.rootManifestName = "/publisher/artifact/root/v=2";
  fixture.artifact.publisherIdentity = "/publisher";
  fixture.artifact.policyEpoch = "epoch-1";

  fixture.chunk.index = 0;
  fixture.chunk.offsetBytes = 0;
  fixture.chunk.lengthBytes = fixture.payload.size();
  fixture.chunk.digest = fixture.artifact.contentDigest;
  fixture.chunk.firstSegment = 0;
  fixture.chunk.finalSegment = 1;

  fixture.page.depth = 0;
  fixture.page.offsetBytes = 0;
  fixture.page.lengthBytes = fixture.payload.size();
  fixture.page.children = {{
    "chunk", fixture.chunk.index, fixture.chunk.offsetBytes,
    fixture.chunk.lengthBytes, "sha256", fixture.chunk.digest,
  }};
  fixture.page.pageDigest = artifactSha256Hex(
    canonicalManifestPageBytes(fixture.page, fixture.limits));

  fixture.signedRoot.root.artifact = fixture.artifact;
  fixture.signedRoot.root.packetPayloadBytes = 4;
  fixture.signedRoot.root.chunkBytes = 8;
  fixture.signedRoot.root.namingTemplate =
    "/publisher/artifact/chunk={chunk}/seg={segment}";
  fixture.signedRoot.root.manifestRootDigest = fixture.page.pageDigest;
  fixture.signedRoot.root.signatureAlgorithm = "rsa-sha256";
  fixture.signedRoot.root.publisherKeyLocator = "/publisher/KEY/1";
  fixture.signedRoot.root.createdAtMs = 1000;
  fixture.signedRoot.root.expiresAtMs = 3000;
  fixture.signedRoot.signatureValue = sign(
    fixture.key.get(),
    canonicalRootManifestBytes(fixture.signedRoot.root, fixture.limits));

  fixture.capability.repoNode = "/repo/1";
  fixture.capability.formatVersions = {"artifact-manifest-v2"};
  fixture.capability.digestAlgorithms = {"sha256"};
  fixture.capability.signatureAlgorithms = {"rsa-sha256"};
  fixture.capability.maxArtifactBytes = fixture.limits.maxArtifactBytes;
  fixture.capability.maxChunkBytes = fixture.limits.maxChunkBytes;
  fixture.capability.maxRootEncodedBytes = fixture.limits.maxRootEncodedBytes;
  fixture.capability.maxPageEncodedBytes = fixture.limits.maxPageEncodedBytes;
  fixture.capability.maxPageEntries = fixture.limits.maxPageEntries;
  fixture.capability.maxManifestDepth = fixture.limits.maxManifestDepth;
  fixture.capability.policyEpoch = "epoch-1";

  fixture.policy.trustedPublisherIdentity = "/publisher";
  fixture.policy.trustedKeyLocator = "/publisher/KEY/1";
  fixture.policy.publicKeyPem = publicKeyPem(fixture.key.get());
  fixture.policy.policyEpoch = "epoch-1";
  fixture.policy.evaluationTimeMs = 2000;
  fixture.policy.allowedDigestAlgorithms = {"sha256"};
  fixture.policy.allowedSignatureAlgorithms = {"rsa-sha256"};
  return fixture;
}

template<typename Callable>
void
checkCode(const char* code, Callable&& callable)
{
  try {
    callable();
    BOOST_FAIL("expected ArtifactValidationError");
  }
  catch (const ArtifactValidationError& error) {
    BOOST_CHECK_EQUAL(error.code(), code);
  }
}

} // namespace

BOOST_AUTO_TEST_SUITE(DistributedRepoArtifactManifest)

BOOST_AUTO_TEST_CASE(SignedRootAndDigestGraphVerifyWithOneAsymmetricOperation)
{
  auto fixture = makeFixture();
  const auto result = verifyArtifactManifestGraph(
    fixture.signedRoot, fixture.artifact, {fixture.page}, {fixture.chunk},
    fixture.capability, fixture.policy, fixture.limits);
  BOOST_CHECK_EQUAL(result.asymmetricVerificationCount, 1);
  BOOST_CHECK_EQUAL(result.verifiedPageCount, 1);
  BOOST_CHECK_EQUAL(result.verifiedChunkCount, 1);
  BOOST_CHECK_EQUAL(result.digestVerificationCount, 1);
  BOOST_REQUIRE_EQUAL(result.derivedPageNames.size(), 1);
  BOOST_CHECK(result.derivedPageNames.front().find(fixture.page.pageDigest) !=
              std::string::npos);
  BOOST_CHECK_NO_THROW(verifyArtifactChunkPayload(
    fixture.chunk, fixture.payload));
  BOOST_CHECK_NO_THROW(verifyArtifactPayload(fixture.artifact, fixture.payload));
}

BOOST_AUTO_TEST_CASE(MultiChunkNamesUseChunkLocalSegmentCoordinates)
{
  auto fixture = makeFixture();
  fixture.payload = {
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
    'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p',
  };
  fixture.artifact.sizeBytes = fixture.payload.size();
  fixture.artifact.contentDigest = artifactSha256Hex(fixture.payload);

  const std::vector<uint8_t> firstPayload(
    fixture.payload.begin(), fixture.payload.begin() + 8);
  const std::vector<uint8_t> secondPayload(
    fixture.payload.begin() + 8, fixture.payload.end());
  fixture.chunk.digest = artifactSha256Hex(firstPayload);

  auto second = fixture.chunk;
  second.index = 1;
  second.offsetBytes = 8;
  second.digest = artifactSha256Hex(secondPayload);
  second.firstSegment = 0;
  second.finalSegment = 1;

  fixture.page.lengthBytes = fixture.payload.size();
  fixture.page.children = {
    {
      "chunk", fixture.chunk.index, fixture.chunk.offsetBytes,
      fixture.chunk.lengthBytes, "sha256", fixture.chunk.digest,
    },
    {
      "chunk", second.index, second.offsetBytes,
      second.lengthBytes, "sha256", second.digest,
    },
  };
  fixture.page.pageDigest = artifactSha256Hex(
    canonicalManifestPageBytes(fixture.page, fixture.limits));
  fixture.signedRoot.root.artifact = fixture.artifact;
  fixture.signedRoot.root.manifestRootDigest = fixture.page.pageDigest;
  fixture.signedRoot.signatureValue = sign(
    fixture.key.get(),
    canonicalRootManifestBytes(fixture.signedRoot.root, fixture.limits));

  const auto result = verifyArtifactManifestGraph(
    fixture.signedRoot, fixture.artifact, {fixture.page},
    {fixture.chunk, second}, fixture.capability, fixture.policy,
    fixture.limits);
  BOOST_CHECK_EQUAL(result.verifiedChunkCount, 2);
  BOOST_CHECK_EQUAL(
    deriveArtifactDataName(fixture.signedRoot.root, second.index, 0),
    "/publisher/artifact/chunk=1/seg=0");

  auto globalSegmentCoordinates = second;
  globalSegmentCoordinates.firstSegment = 2;
  globalSegmentCoordinates.finalSegment = 3;
  checkCode(artifact_manifest_error::InvalidGraph, [&] {
    verifyArtifactManifestGraph(
      fixture.signedRoot, fixture.artifact, {fixture.page},
      {fixture.chunk, globalSegmentCoordinates}, fixture.capability,
      fixture.policy, fixture.limits);
  });
}

BOOST_AUTO_TEST_CASE(NegotiatedPublicSignatureAlgorithmsAreInteroperable)
{
  for (const auto& algorithm :
       {"rsa-sha256", "ecdsa-sha256", "ed25519"}) {
    auto fixture = makeFixture();
    fixture.key = generateKey(algorithm);
    fixture.signedRoot.root.signatureAlgorithm = algorithm;
    fixture.signedRoot.signatureValue = sign(
      fixture.key.get(),
      canonicalRootManifestBytes(fixture.signedRoot.root, fixture.limits),
      algorithm);
    fixture.capability.signatureAlgorithms = {algorithm};
    fixture.policy.allowedSignatureAlgorithms = {algorithm};
    fixture.policy.publicKeyPem = publicKeyPem(fixture.key.get());
    BOOST_CHECK_NO_THROW(verifySignedArtifactRoot(
      fixture.signedRoot, fixture.artifact, fixture.capability,
      fixture.policy, fixture.limits));
  }
}

BOOST_AUTO_TEST_CASE(EmptyArtifactUsesCanonicalEmptyHierarchy)
{
  auto fixture = makeFixture();
  fixture.artifact.sizeBytes = 0;
  fixture.artifact.contentDigest = artifactSha256Hex({});
  fixture.signedRoot.root.artifact = fixture.artifact;
  fixture.signedRoot.root.manifestRootDigest = artifactSha256Hex({});
  fixture.signedRoot.signatureValue = sign(
    fixture.key.get(),
    canonicalRootManifestBytes(fixture.signedRoot.root, fixture.limits));
  const auto result = verifyArtifactManifestGraph(
    fixture.signedRoot, fixture.artifact, {}, {},
    fixture.capability, fixture.policy, fixture.limits);
  BOOST_CHECK_EQUAL(result.verifiedPageCount, 0);
  BOOST_CHECK_EQUAL(result.verifiedChunkCount, 0);
  BOOST_CHECK_NO_THROW(verifyArtifactPayload(fixture.artifact, {}));
}

BOOST_AUTO_TEST_CASE(CanonicalCodecRejectsTruncationExtensionAndOversizedCounts)
{
  auto fixture = makeFixture();
  const auto rootWire = encodeSignedArtifactRoot(
    fixture.signedRoot, fixture.limits);
  const auto decoded = decodeSignedArtifactRoot(rootWire, fixture.limits);
  BOOST_CHECK_EQUAL(decoded.root.artifact.contentDigest,
                    fixture.artifact.contentDigest);
  BOOST_CHECK(decoded.signatureValue == fixture.signedRoot.signatureValue);

  auto truncated = rootWire;
  truncated.pop_back();
  checkCode(artifact_manifest_error::MalformedEncoding, [&] {
    decodeSignedArtifactRoot(truncated, fixture.limits);
  });
  auto extended = rootWire;
  extended.push_back(0);
  checkCode(artifact_manifest_error::MalformedEncoding, [&] {
    decodeSignedArtifactRoot(extended, fixture.limits);
  });
  auto downgradedVersion = rootWire;
  const std::string version = "artifact-root-v2";
  const auto versionAt = std::search(
    downgradedVersion.begin(), downgradedVersion.end(),
    version.begin(), version.end());
  BOOST_REQUIRE(versionAt != downgradedVersion.end());
  *(versionAt + version.size() - 1) = '1';
  checkCode(artifact_manifest_error::Downgrade, [&] {
    decodeSignedArtifactRoot(downgradedVersion, fixture.limits);
  });

  const auto pageWire = encodeArtifactManifestPage(
    fixture.page, fixture.limits);
  auto pageExtended = pageWire;
  pageExtended.push_back(0);
  checkCode(artifact_manifest_error::MalformedEncoding, [&] {
    decodeArtifactManifestPage(pageExtended, fixture.limits);
  });

  auto entryBomb = pageWire;
  size_t cursor = 8 + 4; // wire header/length plus canonical page magic.
  auto skipString = [&] {
    BOOST_REQUIRE_LE(cursor + 4, entryBomb.size());
    const uint32_t size =
      (static_cast<uint32_t>(entryBomb[cursor]) << 24) |
      (static_cast<uint32_t>(entryBomb[cursor + 1]) << 16) |
      (static_cast<uint32_t>(entryBomb[cursor + 2]) << 8) |
      static_cast<uint32_t>(entryBomb[cursor + 3]);
    cursor += 4 + size;
    BOOST_REQUIRE_LE(cursor, entryBomb.size());
  };
  skipString(); // pageVersion
  cursor += 4 + 8 + 8;
  skipString(); // pageDigestAlgorithm
  BOOST_REQUIRE_LE(cursor + 4, entryBomb.size());
  entryBomb[cursor] = 0xff;
  entryBomb[cursor + 1] = 0xff;
  entryBomb[cursor + 2] = 0xff;
  entryBomb[cursor + 3] = 0xff;
  checkCode(artifact_error::LimitExceeded, [&] {
    decodeArtifactManifestPage(entryBomb, fixture.limits);
  });
}

BOOST_AUTO_TEST_CASE(SubstitutionDowngradeRevocationAndUnknownCriticalFailClosed)
{
  auto fixture = makeFixture();

  auto substituted = fixture.artifact;
  substituted.logicalName = "/publisher/other";
  checkCode(artifact_manifest_error::Substitution, [&] {
    verifySignedArtifactRoot(
      fixture.signedRoot, substituted, fixture.capability,
      fixture.policy, fixture.limits);
  });

  auto downgraded = fixture.capability;
  downgraded.signatureAlgorithms = {"ecdsa-sha256"};
  checkCode(artifact_manifest_error::Downgrade, [&] {
    verifySignedArtifactRoot(
      fixture.signedRoot, fixture.artifact, downgraded,
      fixture.policy, fixture.limits);
  });

  auto revoked = fixture.policy;
  revoked.revokedKeyLocators = {revoked.trustedKeyLocator};
  checkCode(artifact_manifest_error::RevokedPublisher, [&] {
    verifySignedArtifactRoot(
      fixture.signedRoot, fixture.artifact, fixture.capability,
      revoked, fixture.limits);
  });

  fixture.signedRoot.root.criticalExtensions = {"future-required-field"};
  fixture.signedRoot.signatureValue = sign(
    fixture.key.get(),
    canonicalRootManifestBytes(fixture.signedRoot.root, fixture.limits));
  checkCode(artifact_manifest_error::UnsupportedCriticalField, [&] {
    verifySignedArtifactRoot(
      fixture.signedRoot, fixture.artifact, fixture.capability,
      fixture.policy, fixture.limits);
  });
}

BOOST_AUTO_TEST_CASE(CorruptionGeometryAndMixedResumeFailClosed)
{
  auto fixture = makeFixture();
  auto corruptPage = fixture.page;
  corruptPage.children.front().digest[0] =
    corruptPage.children.front().digest[0] == '0' ? '1' : '0';
  checkCode(artifact_manifest_error::DigestMismatch, [&] {
    verifyArtifactManifestGraph(
      fixture.signedRoot, fixture.artifact, {corruptPage}, {fixture.chunk},
      fixture.capability, fixture.policy, fixture.limits);
  });

  auto truncatedPayload = fixture.payload;
  truncatedPayload.pop_back();
  checkCode(artifact_manifest_error::DigestMismatch, [&] {
    verifyArtifactPayload(fixture.artifact, truncatedPayload);
  });
  checkCode(artifact_manifest_error::DigestMismatch, [&] {
    verifyArtifactChunkPayload(fixture.chunk, truncatedPayload);
  });

  auto resumed = fixture.artifact;
  resumed.policyEpoch = "epoch-2";
  checkCode(artifact_manifest_error::MixedResume, [&] {
    validateArtifactResumeIdentity(
      fixture.artifact, fixture.signedRoot.root,
      resumed, fixture.signedRoot.root);
  });
}

BOOST_AUTO_TEST_CASE(SignatureValidityTrustAndResourceBudgetsFailClosed)
{
  auto fixture = makeFixture();
  fixture.signedRoot.signatureValue.front() ^= 0x01;
  checkCode(artifact_manifest_error::InvalidSignature, [&] {
    verifySignedArtifactRoot(
      fixture.signedRoot, fixture.artifact, fixture.capability,
      fixture.policy, fixture.limits);
  });

  fixture = makeFixture();
  fixture.policy.evaluationTimeMs = fixture.signedRoot.root.expiresAtMs;
  checkCode(artifact_manifest_error::ExpiredPolicy, [&] {
    verifySignedArtifactRoot(
      fixture.signedRoot, fixture.artifact, fixture.capability,
      fixture.policy, fixture.limits);
  });

  fixture = makeFixture();
  fixture.policy.publicKeyPem = "not a public key";
  checkCode(artifact_manifest_error::TrustPolicyRejected, [&] {
    verifySignedArtifactRoot(
      fixture.signedRoot, fixture.artifact, fixture.capability,
      fixture.policy, fixture.limits);
  });

  fixture = makeFixture();
  fixture.policy.trustedPublisherIdentity = "/other-publisher";
  checkCode(artifact_manifest_error::TrustPolicyRejected, [&] {
    verifySignedArtifactRoot(
      fixture.signedRoot, fixture.artifact, fixture.capability,
      fixture.policy, fixture.limits);
  });

  fixture = makeFixture();
  fixture.limits.maxCryptographicOperations = 1;
  checkCode(artifact_manifest_error::CryptoBudgetExceeded, [&] {
    verifyArtifactManifestGraph(
      fixture.signedRoot, fixture.artifact, {fixture.page}, {fixture.chunk},
      fixture.capability, fixture.policy, fixture.limits);
  });
}

BOOST_AUTO_TEST_CASE(MissingDuplicateDepthAndNameScopeCasesFailClosed)
{
  auto fixture = makeFixture();
  checkCode(artifact_manifest_error::InvalidGraph, [&] {
    verifyArtifactManifestGraph(
      fixture.signedRoot, fixture.artifact, {}, {fixture.chunk},
      fixture.capability, fixture.policy, fixture.limits);
  });
  checkCode(artifact_manifest_error::InvalidGraph, [&] {
    verifyArtifactManifestGraph(
      fixture.signedRoot, fixture.artifact,
      {fixture.page, fixture.page}, {fixture.chunk},
      fixture.capability, fixture.policy, fixture.limits);
  });

  auto deepPage = fixture.page;
  deepPage.depth = fixture.limits.maxManifestDepth + 1;
  checkCode(artifact_error::LimitExceeded, [&] {
    verifyArtifactManifestGraph(
      fixture.signedRoot, fixture.artifact, {deepPage}, {fixture.chunk},
      fixture.capability, fixture.policy, fixture.limits);
  });

  fixture.signedRoot.root.namingTemplate =
    "/publisher/artifact-other/chunk={chunk}/seg={segment}";
  fixture.signedRoot.signatureValue = sign(
    fixture.key.get(),
    canonicalRootManifestBytes(fixture.signedRoot.root, fixture.limits));
  checkCode(artifact_manifest_error::Substitution, [&] {
    verifySignedArtifactRoot(
      fixture.signedRoot, fixture.artifact, fixture.capability,
      fixture.policy, fixture.limits);
  });
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::test
