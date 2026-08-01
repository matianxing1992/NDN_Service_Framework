#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp"
#include "tests/boost-test.hpp"

namespace ndnsf_distributed_repo::test {

namespace {

const std::string DIGEST(64, 'a');

ArtifactReference
makeReference()
{
  ArtifactReference reference;
  reference.logicalName = "/publisher/models/qwen";
  reference.contentDigest = DIGEST;
  reference.sizeBytes = 8192;
  reference.rootManifestName = "/publisher/models/qwen/manifest/v=2";
  reference.publisherIdentity = "/publisher";
  reference.policyEpoch = "epoch-1";
  return reference;
}

ArtifactCapability
makeCapability()
{
  ArtifactCapability capability;
  capability.repoNode = "/repo/node-1";
  capability.formatVersions = {"artifact-manifest-v2", "exact-packet-v1"};
  capability.digestAlgorithms = {"sha256"};
  capability.signatureAlgorithms = {"ed25519"};
  capability.maxArtifactBytes = 1ULL << 30;
  capability.maxChunkBytes = 1ULL << 20;
  capability.maxRootEncodedBytes = 1ULL << 16;
  capability.maxPageEncodedBytes = 1ULL << 20;
  capability.maxPageEntries = 4096;
  capability.maxManifestDepth = 8;
  capability.supportsResume = true;
  capability.supportsReplicaReceipts = true;
  capability.policyEpoch = "epoch-1";
  return capability;
}

ArtifactCapabilityRequirements
makeRequirements()
{
  ArtifactCapabilityRequirements requirements;
  requirements.artifact = makeReference();
  requirements.rootSignatureAlgorithm = "ed25519";
  requirements.chunkBytes = 1ULL << 20;
  requirements.rootEncodedBytes = 1ULL << 16;
  requirements.pageEncodedBytes = 1ULL << 20;
  requirements.pageEntries = 4096;
  requirements.manifestDepth = 8;
  requirements.requireResume = true;
  requirements.requireReplicaReceipts = true;
  return requirements;
}

template<typename Callable>
void
checkError(const std::string& expectedCode, Callable&& callable)
{
  try {
    callable();
    BOOST_FAIL("expected ArtifactValidationError");
  }
  catch (const ArtifactValidationError& error) {
    BOOST_CHECK_EQUAL(error.code(), expectedCode);
  }
}

} // namespace

BOOST_AUTO_TEST_SUITE(DistributedRepoArtifactTypes)

BOOST_AUTO_TEST_CASE(ReferenceAndCapabilityNegotiateExactIdentity)
{
  const auto reference = makeReference();
  BOOST_CHECK_NO_THROW(reference.validate());

  auto alias = reference;
  alias.logicalName = "/catalog/qwen-alias";
  BOOST_CHECK(reference.sameBytes(alias));

  auto capability = makeCapability();

  BOOST_CHECK_NO_THROW(capability.validate());
  BOOST_CHECK(capability.supports(reference, "ed25519"));
  capability.signatureAlgorithms = {"hmac-sha256"};
  checkError(artifact_error::UnsupportedAlgorithm, [&] { capability.validate(); });
}

BOOST_AUTO_TEST_CASE(CapabilityRequirementsRejectDowngradeLimitsAndMissingDurability)
{
  auto capability = makeCapability();
  const auto requirements = makeRequirements();
  BOOST_CHECK_NO_THROW(requirements.validate());
  BOOST_CHECK(capability.incompatibilities(requirements).empty());
  BOOST_CHECK_NO_THROW(capability.requireSupport(requirements));

  capability.formatVersions = {"exact-packet-v1"};
  auto reasons = capability.incompatibilities(requirements);
  BOOST_CHECK(std::find(reasons.begin(), reasons.end(), "format-version") !=
              reasons.end());
  checkError(artifact_error::UnsupportedCapability,
             [&] { capability.requireSupport(requirements); });

  capability = makeCapability();
  capability.signatureAlgorithms = {"rsa-sha256"};
  reasons = capability.incompatibilities(requirements);
  BOOST_CHECK(std::find(reasons.begin(), reasons.end(),
                        "root-signature-algorithm") != reasons.end());

  capability = makeCapability();
  capability.maxChunkBytes = (1ULL << 20) - 1;
  reasons = capability.incompatibilities(requirements);
  BOOST_CHECK(std::find(reasons.begin(), reasons.end(), "chunk-size-limit") !=
              reasons.end());

  capability = makeCapability();
  capability.supportsResume = false;
  capability.supportsReplicaReceipts = false;
  reasons = capability.incompatibilities(requirements);
  BOOST_CHECK(std::find(reasons.begin(), reasons.end(), "resume") != reasons.end());
  BOOST_CHECK(std::find(reasons.begin(), reasons.end(), "replica-receipts") !=
              reasons.end());
}

BOOST_AUTO_TEST_CASE(ExactPacketRequirementsRemainSeparateFromV2Trust)
{
  auto requirements = ArtifactCapabilityRequirements{};
  requirements.artifact = makeReference();
  requirements.artifact.formatVersion = "exact-packet-v1";
  BOOST_CHECK_NO_THROW(requirements.validate());

  auto capability = makeCapability();
  capability.signatureAlgorithms = {"rsa-sha256"};
  capability.supportsResume = false;
  capability.supportsReplicaReceipts = false;
  BOOST_CHECK(capability.incompatibilities(requirements).empty());

  requirements.rootSignatureAlgorithm = "ed25519";
  checkError(artifact_error::InvalidCapability,
             [&] { requirements.validate(); });
}

BOOST_AUTO_TEST_CASE(ManifestPageRejectsGapsAndEntryBombs)
{
  ArtifactManifestPage page;
  page.depth = 1;
  page.offsetBytes = 0;
  page.lengthBytes = 8192;
  page.pageDigest = DIGEST;
  page.children = {
    {"chunk", 0, 0, 4096, "sha256", DIGEST},
    {"chunk", 1, 4096, 4096, "sha256", DIGEST},
  };
  BOOST_CHECK_NO_THROW(page.validate(512));

  page.children[1].offsetBytes = 4097;
  checkError(artifact_error::InvalidRange, [&] { page.validate(512); });

  page.children[1].offsetBytes = 4096;
  ArtifactLimits limits;
  limits.maxPageEntries = 1;
  checkError(artifact_error::LimitExceeded, [&] { page.validate(512, limits); });
}

BOOST_AUTO_TEST_CASE(ChunkLeaseAndReceiptFailClosed)
{
  const auto reference = makeReference();

  ArtifactChunk chunk{0, 0, 4096, "sha256", DIGEST, 0, 1};
  BOOST_CHECK_NO_THROW(chunk.validate(reference));
  chunk.finalSegment = 0;
  chunk.firstSegment = 1;
  checkError(artifact_error::InvalidRange, [&] { chunk.validate(reference); });

  ArtifactUploadLease lease;
  lease.leaseId = "lease-1";
  lease.operationId = "operation-1";
  lease.repoNode = "/repo/node-1";
  lease.artifact = reference;
  lease.reservedBytes = reference.sizeBytes;
  lease.issuedAtMs = 1000;
  lease.expiresAtMs = 2000;
  lease.replayId = "nonce-1";
  BOOST_CHECK_NO_THROW(lease.validate(1500));
  checkError(artifact_error::InvalidLease, [&] { lease.validate(2000); });

  ArtifactReplicaReceipt receipt;
  receipt.receiptId = "receipt-1";
  receipt.operationId = "operation-1";
  receipt.repoNode = "/repo/node-1";
  receipt.artifact = reference;
  receipt.committedAtMs = 2500;
  receipt.policyEpoch = "epoch-1";
  BOOST_CHECK_NO_THROW(receipt.validate());
  receipt.policyEpoch = "epoch-2";
  checkError(artifact_error::InvalidReceipt, [&] { receipt.validate(); });
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::test
