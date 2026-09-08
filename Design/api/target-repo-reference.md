# Repo API 参考

声明从源码语法树提取。保留准确类型、参数、默认值、限定符和原始注释；注释不代替运行证据。中文语义契约见开发者指南。protected 扩展点、测试 helper、应用内部接口各自标注。

本文件来自冻结目标清单；路径/行号属于该快照，不指向当前工作树。还原方法见 [目标源码身份](../target-source-baseline.json) 与独立补丁。PLANNED 修改另见目标 PDF，冻结声明不冒充已完成目标。

## NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp

源码 SHA-256：`681fea55c4c843f5d427f59a6883e110e1b1711d7c02eb72aad1c2b38aeb57b7`。

### API-e0321bbb7aba · ndnsf_distributed_repo::artifact_manifest_error::* MalformedEncoding = "artifact-manifest-malformed-encoding"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 13 行。

```cpp
inline constexpr const char* MalformedEncoding = "artifact-manifest-malformed-encoding";
```

### API-d1f2724138e1 · ndnsf_distributed_repo::artifact_manifest_error::* InvalidSignature = "artifact-manifest-invalid-signature"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 14 行。

```cpp
inline constexpr const char* InvalidSignature = "artifact-manifest-invalid-signature";
```

### API-d292ee1a50a3 · ndnsf_distributed_repo::artifact_manifest_error::* TrustPolicyRejected = "artifact-manifest-trust-policy-rejected"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 15 行。

```cpp
inline constexpr const char* TrustPolicyRejected = "artifact-manifest-trust-policy-rejected";
```

### API-e3abcb95dff6 · ndnsf_distributed_repo::artifact_manifest_error::* RevokedPublisher = "artifact-manifest-revoked-publisher"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 16 行。

```cpp
inline constexpr const char* RevokedPublisher = "artifact-manifest-revoked-publisher";
```

### API-c23e02dee10a · ndnsf_distributed_repo::artifact_manifest_error::* ExpiredPolicy = "artifact-manifest-expired-policy"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 17 行。

```cpp
inline constexpr const char* ExpiredPolicy = "artifact-manifest-expired-policy";
```

### API-27280679519e · ndnsf_distributed_repo::artifact_manifest_error::* UnsupportedCriticalField =   "artifact-manifest-unsupported-critical-field"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 18 行。

```cpp
inline constexpr const char* UnsupportedCriticalField =
  "artifact-manifest-unsupported-critical-field";
```

### API-ea4ef34de70f · ndnsf_distributed_repo::artifact_manifest_error::* Substitution = "artifact-manifest-substitution"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 20 行。

```cpp
inline constexpr const char* Substitution = "artifact-manifest-substitution";
```

### API-119994dbbc7c · ndnsf_distributed_repo::artifact_manifest_error::* Downgrade = "artifact-manifest-downgrade"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 21 行。

```cpp
inline constexpr const char* Downgrade = "artifact-manifest-downgrade";
```

### API-1f70e212d0cc · ndnsf_distributed_repo::artifact_manifest_error::* DigestMismatch = "artifact-manifest-digest-mismatch"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 22 行。

```cpp
inline constexpr const char* DigestMismatch = "artifact-manifest-digest-mismatch";
```

### API-ff14d8378192 · ndnsf_distributed_repo::artifact_manifest_error::* InvalidGraph = "artifact-manifest-invalid-graph"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 23 行。

```cpp
inline constexpr const char* InvalidGraph = "artifact-manifest-invalid-graph";
```

### API-4e447495503f · ndnsf_distributed_repo::artifact_manifest_error::* Cycle = "artifact-manifest-cycle"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 24 行。

```cpp
inline constexpr const char* Cycle = "artifact-manifest-cycle";
```

### API-829d3d97f3ec · ndnsf_distributed_repo::artifact_manifest_error::* CryptoBudgetExceeded =   "artifact-manifest-crypto-budget-exceeded"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 25 行。

```cpp
inline constexpr const char* CryptoBudgetExceeded =
  "artifact-manifest-crypto-budget-exceeded";
```

### API-9e62050d8d7a · ndnsf_distributed_repo::artifact_manifest_error::* MixedResume = "artifact-manifest-mixed-resume"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 27 行。

```cpp
inline constexpr const char* MixedResume = "artifact-manifest-mixed-resume";
```

### API-83d49d2668a9 · ndnsf_distributed_repo::SignedArtifactRoot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 30 行。

```cpp
struct SignedArtifactRoot
```

### API-185021d84186 · ndnsf_distributed_repo::SignedArtifactRoot::root

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 32 行。

```cpp
ArtifactRootManifest root;
```

### API-4db77162fb55 · ndnsf_distributed_repo::SignedArtifactRoot::signatureValue

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 33 行。

```cpp
std::vector<uint8_t> signatureValue;
```

### API-d957313c3bd2 · ndnsf_distributed_repo::ArtifactManifestTrustPolicy

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 43 行。

```cpp
struct ArtifactManifestTrustPolicy
```

### API-313d966fdcdd · ndnsf_distributed_repo::ArtifactManifestTrustPolicy::trustedPublisherIdentity

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 45 行。

```cpp
std::string trustedPublisherIdentity;
```

### API-a56403969b09 · ndnsf_distributed_repo::ArtifactManifestTrustPolicy::trustedKeyLocator

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 46 行。

```cpp
std::string trustedKeyLocator;
```

### API-6910caf638d9 · ndnsf_distributed_repo::ArtifactManifestTrustPolicy::publicKeyPem

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 47 行。

```cpp
std::string publicKeyPem;
```

### API-b67451c6fd9a · ndnsf_distributed_repo::ArtifactManifestTrustPolicy::policyEpoch

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 48 行。

```cpp
std::string policyEpoch;
```

### API-3800b7dba6ca · ndnsf_distributed_repo::ArtifactManifestTrustPolicy::evaluationTimeMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 49 行。

```cpp
uint64_t evaluationTimeMs = 0;
```

### API-c2d3a49c34c5 · ndnsf_distributed_repo::ArtifactManifestTrustPolicy::allowedDigestAlgorithms

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 50 行。

```cpp
std::vector<std::string> allowedDigestAlgorithms;
```

### API-e627c57c806c · ndnsf_distributed_repo::ArtifactManifestTrustPolicy::allowedSignatureAlgorithms

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 51 行。

```cpp
std::vector<std::string> allowedSignatureAlgorithms;
```

### API-2464be995ccf · ndnsf_distributed_repo::ArtifactManifestTrustPolicy::supportedCriticalExtensions

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 52 行。

```cpp
std::vector<std::string> supportedCriticalExtensions;
```

### API-88f782ddf69b · ndnsf_distributed_repo::ArtifactManifestTrustPolicy::revokedKeyLocators

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 53 行。

```cpp
std::vector<std::string> revokedKeyLocators;
```

### API-24fa2afff8a1 · ndnsf_distributed_repo::ArtifactManifestTrustPolicy::validate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 55 行。

```cpp
void validate(const ArtifactLimits& limits = {}) const;
```

### API-3f070cfdcdd9 · ndnsf_distributed_repo::ArtifactManifestVerificationResult

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 58 行。

```cpp
struct ArtifactManifestVerificationResult
```

### API-f13dbed64d36 · ndnsf_distributed_repo::ArtifactManifestVerificationResult::artifact

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 60 行。

```cpp
ArtifactReference artifact;
```

### API-10e23ef476a8 · ndnsf_distributed_repo::ArtifactManifestVerificationResult::verifiedPageCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 61 行。

```cpp
uint64_t verifiedPageCount = 0;
```

### API-ed5fe7ff09af · ndnsf_distributed_repo::ArtifactManifestVerificationResult::verifiedChunkCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 62 行。

```cpp
uint64_t verifiedChunkCount = 0;
```

### API-a1a7d898ec92 · ndnsf_distributed_repo::ArtifactManifestVerificationResult::asymmetricVerificationCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 63 行。

```cpp
uint64_t asymmetricVerificationCount = 0;
```

### API-eec3fa9b069f · ndnsf_distributed_repo::ArtifactManifestVerificationResult::digestVerificationCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 64 行。

```cpp
uint64_t digestVerificationCount = 0;
```

### API-49e91fb64dac · ndnsf_distributed_repo::ArtifactManifestVerificationResult::derivedPageNames

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 65 行。

```cpp
std::vector<std::string> derivedPageNames;
```

### API-ea7e615f6925 · ndnsf_distributed_repo::artifactSha256Hex

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 68 行。

```cpp
std::string
artifactSha256Hex(const std::vector<uint8_t>& bytes);
```

### API-89e0d8639b0c · ndnsf_distributed_repo::canonicalRootManifestBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 71 行。

```cpp
std::vector<uint8_t>
canonicalRootManifestBytes(const ArtifactRootManifest& root,
                           const ArtifactLimits& limits = {});
```

### API-a997f3553f52 · ndnsf_distributed_repo::encodeSignedArtifactRoot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 75 行。

```cpp
std::vector<uint8_t>
encodeSignedArtifactRoot(const SignedArtifactRoot& signedRoot,
                         const ArtifactLimits& limits = {});
```

### API-f74d89e28514 · ndnsf_distributed_repo::decodeSignedArtifactRoot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 79 行。

```cpp
SignedArtifactRoot
decodeSignedArtifactRoot(const std::vector<uint8_t>& wire,
                         const ArtifactLimits& limits = {});
```

### API-29ae3eead3c3 · ndnsf_distributed_repo::canonicalManifestPageBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 83 行。

```cpp
std::vector<uint8_t>
canonicalManifestPageBytes(const ArtifactManifestPage& page,
                           const ArtifactLimits& limits = {});
```

### API-ef67eba0aaee · ndnsf_distributed_repo::encodeArtifactManifestPage

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 87 行。

```cpp
std::vector<uint8_t>
encodeArtifactManifestPage(const ArtifactManifestPage& page,
                           const ArtifactLimits& limits = {});
```

### API-8f039cbfbd90 · ndnsf_distributed_repo::decodeArtifactManifestPage

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 91 行。

```cpp
ArtifactManifestPage
decodeArtifactManifestPage(const std::vector<uint8_t>& wire,
                           const ArtifactLimits& limits = {});
```

### API-8bcb44dbe804 · ndnsf_distributed_repo::deriveManifestPageName

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 95 行。

```cpp
std::string
deriveManifestPageName(const ArtifactRootManifest& root,
                       const std::string& pageDigest);
```

### API-aec34ec35b1b · ndnsf_distributed_repo::deriveArtifactDataName

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 99 行。

```cpp
std::string
deriveArtifactDataName(const ArtifactRootManifest& root,
                       uint64_t chunkIndex, uint64_t segment);
```

### API-c844b788d714 · ndnsf_distributed_repo::verifySignedArtifactRoot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 103 行。

```cpp
void
verifySignedArtifactRoot(const SignedArtifactRoot& signedRoot,
                         const ArtifactReference& expectedArtifact,
                         const ArtifactCapability& capability,
                         const ArtifactManifestTrustPolicy& policy,
                         const ArtifactLimits& limits = {});
```

### API-2559643b0a3a · ndnsf_distributed_repo::verifyArtifactManifestGraph

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 110 行。

```cpp
ArtifactManifestVerificationResult
verifyArtifactManifestGraph(
  const SignedArtifactRoot& signedRoot,
  const ArtifactReference& expectedArtifact,
  const std::vector<ArtifactManifestPage>& pages,
  const std::vector<ArtifactChunk>& chunks,
  const ArtifactCapability& capability,
  const ArtifactManifestTrustPolicy& policy,
  const ArtifactLimits& limits = {});
```

### API-a967e80f32b6 · ndnsf_distributed_repo::verifyArtifactChunkPayload

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 120 行。

```cpp
void
verifyArtifactChunkPayload(const ArtifactChunk& chunk,
                           const std::vector<uint8_t>& payload);
```

### API-05ea9ca8b299 · ndnsf_distributed_repo::verifyArtifactPayload

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 124 行。

```cpp
void
verifyArtifactPayload(const ArtifactReference& artifact,
                      const std::vector<uint8_t>& payload);
```

### API-d095fc35b8ca · ndnsf_distributed_repo::validateArtifactResumeIdentity

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactManifest.hpp`，第 128 行。

```cpp
void
validateArtifactResumeIdentity(const ArtifactReference& expectedArtifact,
                               const ArtifactRootManifest& expectedRoot,
                               const ArtifactReference& resumedArtifact,
                               const ArtifactRootManifest& resumedRoot);
```

## NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp

源码 SHA-256：`76e16c82fbadd4bd43b31b1946967d4622b26afd9e35defa9ea20cba93e08869`。

### API-19da665dba8c · ndnsf_distributed_repo::AdaptiveTransferOptions

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 17 行。

```cpp
struct AdaptiveTransferOptions
```

### API-e62260803e2a · ndnsf_distributed_repo::AdaptiveTransferOptions::initialWindow

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 19 行。

```cpp
uint32_t initialWindow = 4;
```

### API-145001adbe89 · ndnsf_distributed_repo::AdaptiveTransferOptions::minimumWindow

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 20 行。

```cpp
uint32_t minimumWindow = 1;
```

### API-6ab3b39689fd · ndnsf_distributed_repo::AdaptiveTransferOptions::maximumWindow

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 21 行。

```cpp
uint32_t maximumWindow = 64;
```

### API-70e767d4ade2 · ndnsf_distributed_repo::AdaptiveTransferOptions::verificationBacklogLimit

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 22 行。

```cpp
uint32_t verificationBacklogLimit = 16;
```

### API-4e202af191db · ndnsf_distributed_repo::AdaptiveTransferOptions::maximumRetries

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 23 行。

```cpp
uint32_t maximumRetries = 5;
```

### API-bb21a7aa9464 · ndnsf_distributed_repo::AdaptiveTransferOptions::segmentTimeoutMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 24 行。

```cpp
uint64_t segmentTimeoutMs = 1000;
```

### API-7eb2d3a43a68 · ndnsf_distributed_repo::AdaptiveTransferOptions::validate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 26 行。

```cpp
void validate() const;
```

### API-dc443c6b1b7e · ndnsf_distributed_repo::ArtifactSegmentRequest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 29 行。

```cpp
struct ArtifactSegmentRequest
```

### API-fcf6faa66808 · ndnsf_distributed_repo::ArtifactSegmentRequest::segmentNo

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 31 行。

```cpp
uint64_t segmentNo = 0;
```

### API-95aeb668a4df · ndnsf_distributed_repo::ArtifactSegmentRequest::attempt

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 32 行。

```cpp
uint32_t attempt = 0;
```

### API-e0089c43222e · ndnsf_distributed_repo::ArtifactSegmentRequest::retransmission

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 33 行。

```cpp
bool retransmission = false;
```

### API-7fd7469873a4 · ndnsf_distributed_repo::ArtifactSegmentDisposition

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 36 行。

```cpp
enum class ArtifactSegmentDisposition
```

### API-addadb4588b7 · ndnsf_distributed_repo::ArtifactSegmentDisposition::Accepted

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 38 行。

```cpp
Accepted
```

### API-7f2ded7e8111 · ndnsf_distributed_repo::ArtifactSegmentDisposition::Duplicate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 39 行。

```cpp
Duplicate
```

### API-8adb4fbabc6f · ndnsf_distributed_repo::ArtifactSegmentDisposition::Unsolicited

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 40 行。

```cpp
Unsolicited
```

### API-16b261fad6d4 · ndnsf_distributed_repo::ArtifactTransferSnapshot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 43 行。

```cpp
struct ArtifactTransferSnapshot
```

### API-7566953f90b5 · ndnsf_distributed_repo::ArtifactTransferSnapshot::totalSegments

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 45 行。

```cpp
uint64_t totalSegments = 0;
```

### API-afb8d33eb3a3 · ndnsf_distributed_repo::ArtifactTransferSnapshot::verifiedSegments

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 46 行。

```cpp
uint64_t verifiedSegments = 0;
```

### API-be7937ee03c5 · ndnsf_distributed_repo::ArtifactTransferSnapshot::inFlightSegments

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 47 行。

```cpp
uint64_t inFlightSegments = 0;
```

### API-a85e12c7b45f · ndnsf_distributed_repo::ArtifactTransferSnapshot::verificationBacklog

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 48 行。

```cpp
uint64_t verificationBacklog = 0;
```

### API-cadf63e20e02 · ndnsf_distributed_repo::ArtifactTransferSnapshot::logicalBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 49 行。

```cpp
uint64_t logicalBytes = 0;
```

### API-b1339d2ff14a · ndnsf_distributed_repo::ArtifactTransferSnapshot::wireBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 50 行。

```cpp
uint64_t wireBytes = 0;
```

### API-1b3bf3099bcd · ndnsf_distributed_repo::ArtifactTransferSnapshot::retransmittedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 51 行。

```cpp
uint64_t retransmittedBytes = 0;
```

### API-f00ed91c60a3 · ndnsf_distributed_repo::ArtifactTransferSnapshot::interestCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 52 行。

```cpp
uint64_t interestCount = 0;
```

### API-f18508fdf185 · ndnsf_distributed_repo::ArtifactTransferSnapshot::retransmissionCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 53 行。

```cpp
uint64_t retransmissionCount = 0;
```

### API-8f9f0941b099 · ndnsf_distributed_repo::ArtifactTransferSnapshot::duplicateCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 54 行。

```cpp
uint64_t duplicateCount = 0;
```

### API-c3326e47a90a · ndnsf_distributed_repo::ArtifactTransferSnapshot::timeoutCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 55 行。

```cpp
uint64_t timeoutCount = 0;
```

### API-402e647ca78f · ndnsf_distributed_repo::ArtifactTransferSnapshot::rejectedCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 56 行。

```cpp
uint64_t rejectedCount = 0;
```

### API-8d577ffe3be0 · ndnsf_distributed_repo::ArtifactTransferSnapshot::congestionWindow

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 57 行。

```cpp
double congestionWindow = 0.0;
```

### API-f11423d48623 · ndnsf_distributed_repo::ArtifactTransferSnapshot::complete

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 58 行。

```cpp
bool complete = false;
```

### API-d740099fc958 · ndnsf_distributed_repo::ArtifactTransferSnapshot::failed

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 59 行。

```cpp
bool failed = false;
```

### API-4c130c469cda · ndnsf_distributed_repo::ArtifactTransferSnapshot::failureReason

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 60 行。

```cpp
std::string failureReason;
```

### API-48f6cb7d1748 · ndnsf_distributed_repo::AdaptiveArtifactTransfer

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 68 行。

```cpp
class AdaptiveArtifactTransfer
```

### API-a74936fb9a2b · ndnsf_distributed_repo::AdaptiveArtifactTransfer::AdaptiveArtifactTransfer

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 71 行。

```cpp
explicit AdaptiveArtifactTransfer(uint64_t totalSegments,
                                    AdaptiveTransferOptions options = {});
```

### API-121c21c1a5f9 · ndnsf_distributed_repo::AdaptiveArtifactTransfer::poll

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 74 行。

```cpp
std::vector<ArtifactSegmentRequest>
  poll(uint64_t nowMs,
       size_t maximumRequests = std::numeric_limits<size_t>::max());
```

### API-6f363704fd88 · ndnsf_distributed_repo::AdaptiveArtifactTransfer::receive

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 78 行。

```cpp
ArtifactSegmentDisposition
  receive(uint64_t segmentNo, uint64_t logicalBytes, uint64_t wireBytes,
          uint64_t nowMs);
```

### API-b8a723742c4e · ndnsf_distributed_repo::AdaptiveArtifactTransfer::markVerified

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 82 行。

```cpp
void markVerified(uint64_t segmentNo);
```

### API-39064b5f5c98 · ndnsf_distributed_repo::AdaptiveArtifactTransfer::reject

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 83 行。

```cpp
void reject(uint64_t segmentNo, const std::string& reason);
```

### API-35698c26c86e · ndnsf_distributed_repo::AdaptiveArtifactTransfer::expire

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 84 行。

```cpp
void expire(uint64_t nowMs);
```

### API-43e435c8289a · ndnsf_distributed_repo::AdaptiveArtifactTransfer::fail

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 85 行。

```cpp
void fail(const std::string& reason);
```

### API-032e1ae33f7d · ndnsf_distributed_repo::AdaptiveArtifactTransfer::snapshot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 87 行。

```cpp
ArtifactTransferSnapshot snapshot() const;
```

### API-9e057495248e · ndnsf_distributed_repo::AdaptiveArtifactTransfer::missingSegments

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 88 行。

```cpp
std::vector<uint64_t> missingSegments() const;
```

### API-dbdbe20a41cd · ndnsf_distributed_repo::ArtifactResumeState

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 114 行。

```cpp
enum class ArtifactResumeState
```

### API-21f2b7874252 · ndnsf_distributed_repo::ArtifactResumeState::Open

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 116 行。

```cpp
Open
```

### API-79e0ff1b2e89 · ndnsf_distributed_repo::ArtifactResumeState::Cancelled

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 117 行。

```cpp
Cancelled
```

### API-8397eb905f4a · ndnsf_distributed_repo::ArtifactResumeState::Expired

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 118 行。

```cpp
Expired
```

### API-7c134be5b9df · ndnsf_distributed_repo::ArtifactResumeState::Completed

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 119 行。

```cpp
Completed
```

### API-3cfce8496fbe · ndnsf_distributed_repo::ArtifactResumeState::Failed

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 120 行。

```cpp
Failed
```

### API-388dbf652f34 · ndnsf_distributed_repo::ArtifactResumeIdentity

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 123 行。

```cpp
struct ArtifactResumeIdentity
```

### API-20fb848b2326 · ndnsf_distributed_repo::ArtifactResumeIdentity::artifact

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 125 行。

```cpp
ArtifactReference artifact;
```

### API-6d032a46229d · ndnsf_distributed_repo::ArtifactResumeIdentity::manifestRootDigest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 126 行。

```cpp
std::string manifestRootDigest;
```

### API-badc5cdc0c6a · ndnsf_distributed_repo::ArtifactResumeIdentity::packetPayloadBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 127 行。

```cpp
uint64_t packetPayloadBytes = 0;
```

### API-9156ccd16b24 · ndnsf_distributed_repo::ArtifactResumeIdentity::chunkBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 128 行。

```cpp
uint64_t chunkBytes = 0;
```

### API-789e7e73c587 · ndnsf_distributed_repo::ArtifactResumeIdentity::validate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 130 行。

```cpp
void validate(const ArtifactLimits& limits = {}) const;
```

### API-4e99c87bc462 · ndnsf_distributed_repo::ArtifactResumeSnapshot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 133 行。

```cpp
struct ArtifactResumeSnapshot
```

### API-78db42787055 · ndnsf_distributed_repo::ArtifactResumeSnapshot::state

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 135 行。

```cpp
ArtifactResumeState state = ArtifactResumeState::Open;
```

### API-c0f21194342c · ndnsf_distributed_repo::ArtifactResumeSnapshot::operationId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 136 行。

```cpp
std::string operationId;
```

### API-9e3e47c4d0a7 · ndnsf_distributed_repo::ArtifactResumeSnapshot::leaseId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 137 行。

```cpp
std::string leaseId;
```

### API-964fa08ed1b3 · ndnsf_distributed_repo::ArtifactResumeSnapshot::expiresAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 138 行。

```cpp
uint64_t expiresAtMs = 0;
```

### API-e09ff7712c25 · ndnsf_distributed_repo::ArtifactResumeSnapshot::totalChunks

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 139 行。

```cpp
uint64_t totalChunks = 0;
```

### API-8f0542b1898a · ndnsf_distributed_repo::ArtifactResumeSnapshot::verifiedChunks

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 140 行。

```cpp
uint64_t verifiedChunks = 0;
```

### API-1530b85f4218 · ndnsf_distributed_repo::ArtifactResumeSnapshot::newlyVerifiedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 141 行。

```cpp
uint64_t newlyVerifiedBytes = 0;
```

### API-f99ed7439ba1 · ndnsf_distributed_repo::ArtifactResumeSnapshot::avoidedRetransmissionBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 142 行。

```cpp
uint64_t avoidedRetransmissionBytes = 0;
```

### API-5bfd93050c4d · ndnsf_distributed_repo::ArtifactResumeSnapshot::preservesProgress

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 143 行。

```cpp
bool preservesProgress = true;
```

### API-cd2b0713c350 · ndnsf_distributed_repo::ArtifactResumeSession

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 150 行。

```cpp
class ArtifactResumeSession
```

### API-57eb14ae687d · ndnsf_distributed_repo::ArtifactResumeSession::ArtifactResumeSession

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 153 行。

```cpp
ArtifactResumeSession(ArtifactResumeIdentity identity,
                        ArtifactUploadLease lease,
                        std::vector<ArtifactChunk> chunks,
                        uint64_t nowMs);
```

### API-d79635aaca7b · ndnsf_distributed_repo::ArtifactResumeSession::restoreVerified

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 158 行。

```cpp
void restoreVerified(const std::vector<uint64_t>& chunkIndices);
```

### API-15d1ff02c976 · ndnsf_distributed_repo::ArtifactResumeSession::markVerified

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 159 行。

```cpp
bool markVerified(uint64_t chunkIndex, uint64_t nowMs);
```

### API-d5708bfa2ac6 · ndnsf_distributed_repo::ArtifactResumeSession::missingChunks

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 160 行。

```cpp
std::vector<uint64_t> missingChunks(uint64_t nowMs);
```

### API-efe2d01c08f9 · ndnsf_distributed_repo::ArtifactResumeSession::renewLease

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 161 行。

```cpp
void renewLease(ArtifactUploadLease lease, uint64_t nowMs);
```

### API-32dd9b567e33 · ndnsf_distributed_repo::ArtifactResumeSession::resume

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 162 行。

```cpp
void resume(ArtifactResumeIdentity identity, ArtifactUploadLease lease,
              uint64_t nowMs);
```

### API-f5b853886992 · ndnsf_distributed_repo::ArtifactResumeSession::cancel

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 164 行。

```cpp
void cancel(bool preserveProgress);
```

### API-d5ff9f15019d · ndnsf_distributed_repo::ArtifactResumeSession::expire

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 165 行。

```cpp
bool expire(uint64_t nowMs);
```

### API-ce7551d2b4c6 · ndnsf_distributed_repo::ArtifactResumeSession::complete

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 166 行。

```cpp
void complete(uint64_t nowMs);
```

### API-0e9834b0df30 · ndnsf_distributed_repo::ArtifactResumeSession::fail

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 167 行。

```cpp
void fail(const std::string& reason);
```

### API-97e59f42ddac · ndnsf_distributed_repo::ArtifactResumeSession::snapshot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 169 行。

```cpp
ArtifactResumeSnapshot snapshot() const;
```

### API-b8d8421b1ee2 · ndnsf_distributed_repo::ArtifactResumeSession::identity

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 170 行。

```cpp
const ArtifactResumeIdentity& identity() const noexcept;
```

### API-69b37595d4b3 · ndnsf_distributed_repo::toString

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 187 行。

```cpp
std::string
toString(ArtifactResumeState state);
```

### API-c9b05a3e254e · ndnsf_distributed_repo::ReplicaLeaseControlState

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 190 行。

```cpp
enum class ReplicaLeaseControlState
```

### API-55292f3dfaa3 · ndnsf_distributed_repo::ReplicaLeaseControlState::Idle

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 192 行。

```cpp
Idle
```

### API-a181b89ed8fe · ndnsf_distributed_repo::ReplicaLeaseControlState::CollaborationOpen

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 193 行。

```cpp
CollaborationOpen
```

### API-49eed0f9a453 · ndnsf_distributed_repo::ReplicaLeaseControlState::AckClosed

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 194 行。

```cpp
AckClosed
```

### API-178a8b9a73e0 · ndnsf_distributed_repo::ReplicaLeaseControlState::PlanCommitted

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 195 行。

```cpp
PlanCommitted
```

### API-938f44d5b930 · ndnsf_distributed_repo::ReplicaLeaseControlState::Failed

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 196 行。

```cpp
Failed
```

### API-91d265a8667f · ndnsf_distributed_repo::ReplicaLeaseControlSnapshot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 199 行。

```cpp
struct ReplicaLeaseControlSnapshot
```

### API-39fc9e9d82a9 · ndnsf_distributed_repo::ReplicaLeaseControlSnapshot::state

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 201 行。

```cpp
ReplicaLeaseControlState state = ReplicaLeaseControlState::Idle;
```

### API-364b378bf5ca · ndnsf_distributed_repo::ReplicaLeaseControlSnapshot::requestId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 202 行。

```cpp
std::string requestId;
```

### API-12ac8520dd9b · ndnsf_distributed_repo::ReplicaLeaseControlSnapshot::candidateCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 203 行。

```cpp
uint64_t candidateCount = 0;
```

### API-b9a8da74ede8 · ndnsf_distributed_repo::ReplicaLeaseControlSnapshot::selectedReplicaCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 204 行。

```cpp
uint64_t selectedReplicaCount = 0;
```

### API-a16e6150e349 · ndnsf_distributed_repo::ReplicaLeaseControlSnapshot::controlOperationCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 205 行。

```cpp
uint64_t controlOperationCount = 0;
```

### API-c8ea0a88cb7f · ndnsf_distributed_repo::ReplicaLeaseControlSnapshot::leases

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 206 行。

```cpp
std::vector<ArtifactUploadLease> leases;
```

### API-84fd6319306e · ndnsf_distributed_repo::ReplicaLeaseControlFlow

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 214 行。

```cpp
class ReplicaLeaseControlFlow
```

### API-181670471543 · ndnsf_distributed_repo::ReplicaLeaseControlFlow::beginCollaboration

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 217 行。

```cpp
void beginCollaboration(std::string requestId);
```

### API-96b4da33db6e · ndnsf_distributed_repo::ReplicaLeaseControlFlow::closeAcks

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 218 行。

```cpp
void closeAcks(uint64_t candidateCount);
```

### API-8f58d13f8113 · ndnsf_distributed_repo::ReplicaLeaseControlFlow::commitPlan

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 219 行。

```cpp
void commitPlan(std::vector<ArtifactUploadLease> selectedLeases,
                  uint64_t nowMs);
```

### API-a6cc755f8aef · ndnsf_distributed_repo::ReplicaLeaseControlFlow::fail

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 221 行。

```cpp
void fail(const std::string& reason);
```

### API-0aa6ae1bc869 · ndnsf_distributed_repo::ReplicaLeaseControlFlow::snapshot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 223 行。

```cpp
ReplicaLeaseControlSnapshot snapshot() const;
```

### API-3fca436ddeb9 · ndnsf_distributed_repo::toString

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp`，第 230 行。

```cpp
std::string
toString(ReplicaLeaseControlState state);
```

## NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp

源码 SHA-256：`d0c5902ee17e9b78177bd6d4ca99ec0f8442e5e289da0d3aae4ad62381447042`。

### API-09ec33eb7030 · ndnsf_distributed_repo::artifact_error::* InvalidName = "artifact-invalid-name"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 16 行。

```cpp
inline constexpr const char* InvalidName = "artifact-invalid-name";
```

### API-ebf65f8e18bc · ndnsf_distributed_repo::artifact_error::* InvalidDigest = "artifact-invalid-digest"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 17 行。

```cpp
inline constexpr const char* InvalidDigest = "artifact-invalid-digest";
```

### API-dc919bb541b8 · ndnsf_distributed_repo::artifact_error::* UnsupportedFormat = "artifact-unsupported-format"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 18 行。

```cpp
inline constexpr const char* UnsupportedFormat = "artifact-unsupported-format";
```

### API-4e61242a0eda · ndnsf_distributed_repo::artifact_error::* UnsupportedAlgorithm = "artifact-unsupported-algorithm"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 19 行。

```cpp
inline constexpr const char* UnsupportedAlgorithm = "artifact-unsupported-algorithm";
```

### API-3efb3f3f3f3f · ndnsf_distributed_repo::artifact_error::* LimitExceeded = "artifact-limit-exceeded"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 20 行。

```cpp
inline constexpr const char* LimitExceeded = "artifact-limit-exceeded";
```

### API-8aab81dad9c6 · ndnsf_distributed_repo::artifact_error::* InvalidRange = "artifact-invalid-range"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 21 行。

```cpp
inline constexpr const char* InvalidRange = "artifact-invalid-range";
```

### API-9394c5ebb734 · ndnsf_distributed_repo::artifact_error::* InvalidManifest = "artifact-invalid-manifest"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 22 行。

```cpp
inline constexpr const char* InvalidManifest = "artifact-invalid-manifest";
```

### API-88af6aad80bd · ndnsf_distributed_repo::artifact_error::* InvalidCapability = "artifact-invalid-capability"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 23 行。

```cpp
inline constexpr const char* InvalidCapability = "artifact-invalid-capability";
```

### API-8fbf4773d56b · ndnsf_distributed_repo::artifact_error::* UnsupportedCapability = "artifact-unsupported-capability"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 24 行。

```cpp
inline constexpr const char* UnsupportedCapability = "artifact-unsupported-capability";
```

### API-a65940524e86 · ndnsf_distributed_repo::artifact_error::* InvalidLease = "artifact-invalid-lease"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 25 行。

```cpp
inline constexpr const char* InvalidLease = "artifact-invalid-lease";
```

### API-e88454b9fe38 · ndnsf_distributed_repo::artifact_error::* InvalidReceipt = "artifact-invalid-receipt"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 26 行。

```cpp
inline constexpr const char* InvalidReceipt = "artifact-invalid-receipt";
```

### API-1440a936d890 · ndnsf_distributed_repo::ArtifactValidationError

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 29 行。

```cpp
class ArtifactValidationError : public std::invalid_argument
```

### API-eac0f3707bd2 · ndnsf_distributed_repo::ArtifactValidationError::ArtifactValidationError

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 32 行。

```cpp
ArtifactValidationError(std::string code, std::string message)
```

### API-9ede379f6264 · ndnsf_distributed_repo::ArtifactValidationError::code

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 38 行。

```cpp
const std::string&
  code() const noexcept
```

### API-b92b6badf10f · ndnsf_distributed_repo::ArtifactLimits

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 48 行。

```cpp
struct ArtifactLimits
```

### API-9bc6eaf6f883 · ndnsf_distributed_repo::ArtifactLimits::maxArtifactBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 50 行。

```cpp
uint64_t maxArtifactBytes = 1ULL << 50;
```

### API-605ecb71bf6d · ndnsf_distributed_repo::ArtifactLimits::maxChunkBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 51 行。

```cpp
uint64_t maxChunkBytes = 64ULL * 1024 * 1024;
```

原始接口说明：

```text
// 1 PiB hard policy default.
```

### API-4c186ad0c5d6 · ndnsf_distributed_repo::ArtifactLimits::maxRootEncodedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 52 行。

```cpp
uint64_t maxRootEncodedBytes = 64ULL * 1024;
```

### API-f68c6ed9b9d0 · ndnsf_distributed_repo::ArtifactLimits::maxPageEncodedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 53 行。

```cpp
uint64_t maxPageEncodedBytes = 4ULL * 1024 * 1024;
```

### API-0479f78fa07d · ndnsf_distributed_repo::ArtifactLimits::maxPageEntries

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 54 行。

```cpp
uint32_t maxPageEntries = 65536;
```

### API-d35db032d4d4 · ndnsf_distributed_repo::ArtifactLimits::maxManifestDepth

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 55 行。

```cpp
uint32_t maxManifestDepth = 16;
```

### API-466212ce6cf3 · ndnsf_distributed_repo::ArtifactLimits::maxCriticalExtensions

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 56 行。

```cpp
uint32_t maxCriticalExtensions = 32;
```

### API-8e7928252c7a · ndnsf_distributed_repo::ArtifactLimits::maxNameBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 57 行。

```cpp
uint32_t maxNameBytes = 4096;
```

### API-943ad6d56773 · ndnsf_distributed_repo::ArtifactLimits::maxPacketPayloadBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 58 行。

```cpp
uint32_t maxPacketPayloadBytes = 8800;
```

### API-33414d2949c7 · ndnsf_distributed_repo::ArtifactLimits::maxSignatureBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 59 行。

```cpp
uint32_t maxSignatureBytes = 16384;
```

### API-03f6fc38c3cd · ndnsf_distributed_repo::ArtifactLimits::maxManifestPages

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 60 行。

```cpp
uint32_t maxManifestPages = 1U << 20;
```

### API-d301ff083d2f · ndnsf_distributed_repo::ArtifactLimits::maxManifestChunks

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 61 行。

```cpp
uint32_t maxManifestChunks = 1U << 24;
```

### API-f3bd280e6e12 · ndnsf_distributed_repo::ArtifactLimits::maxCryptographicOperations

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 62 行。

```cpp
uint64_t maxCryptographicOperations = 1ULL << 26;
```

### API-0f4dc71b9b1b · ndnsf_distributed_repo::isHex

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 65 行。

```cpp
bool
isHex(const std::string& value);
```

### API-402ab5d252f0 · ndnsf_distributed_repo::isKnownFormat

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 68 行。

```cpp
bool
isKnownFormat(const std::string& value);
```

### API-77e2731b35aa · ndnsf_distributed_repo::isKnownDigestAlgorithm

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 71 行。

```cpp
bool
isKnownDigestAlgorithm(const std::string& value);
```

### API-1112de61cc9c · ndnsf_distributed_repo::isPublicRootSignatureAlgorithm

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 74 行。

```cpp
bool
isPublicRootSignatureAlgorithm(const std::string& value);
```

### API-a7de238d1861 · ndnsf_distributed_repo::validateName

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 77 行。

```cpp
void
validateName(const std::string& value, const ArtifactLimits& limits,
             const std::string& field);
```

### API-f2018e494beb · ndnsf_distributed_repo::validateIdentifier

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 81 行。

```cpp
void
validateIdentifier(const std::string& value, size_t maxBytes, const std::string& field,
                   const char* errorCode);
```

### API-4eddda480860 · ndnsf_distributed_repo::validateDigest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 85 行。

```cpp
void
validateDigest(const std::string& algorithm, const std::string& digest,
               const std::string& field);
```

### API-ccfc8f46ed34 · ndnsf_distributed_repo::validateUniqueStrings

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 89 行。

```cpp
void
validateUniqueStrings(const std::vector<std::string>& values, size_t maximum,
                      const std::string& field, const char* errorCode);
```

### API-c4fb4909bb17 · ndnsf_distributed_repo::ArtifactReference

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 93 行。

```cpp
struct ArtifactReference
```

### API-d1c6caecd787 · ndnsf_distributed_repo::ArtifactReference::logicalName

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 95 行。

```cpp
std::string logicalName;
```

### API-37ccdc27bc8b · ndnsf_distributed_repo::ArtifactReference::digestAlgorithm

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 96 行。

```cpp
std::string digestAlgorithm = "sha256";
```

### API-ead33a5eb63a · ndnsf_distributed_repo::ArtifactReference::contentDigest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 97 行。

```cpp
std::string contentDigest;
```

### API-d9519cc4ed7f · ndnsf_distributed_repo::ArtifactReference::sizeBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 98 行。

```cpp
uint64_t sizeBytes = 0;
```

### API-7aa040c547a8 · ndnsf_distributed_repo::ArtifactReference::formatVersion

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 99 行。

```cpp
std::string formatVersion = "artifact-manifest-v2";
```

### API-012097864e97 · ndnsf_distributed_repo::ArtifactReference::rootManifestName

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 100 行。

```cpp
std::string rootManifestName;
```

### API-06097bcc0d40 · ndnsf_distributed_repo::ArtifactReference::publisherIdentity

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 101 行。

```cpp
std::string publisherIdentity;
```

### API-d8949df49e75 · ndnsf_distributed_repo::ArtifactReference::policyEpoch

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 102 行。

```cpp
std::string policyEpoch;
```

### API-22f3c77bfdce · ndnsf_distributed_repo::ArtifactReference::validate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 104 行。

```cpp
void
  validate(const ArtifactLimits& limits = {}) const
```

### API-06d7b42e7a21 · ndnsf_distributed_repo::ArtifactReference::sameBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 122 行。

```cpp
bool
  sameBytes(const ArtifactReference& other) const
```

### API-a0f0eb0b78ec · ndnsf_distributed_repo::ArtifactCapabilityRequirements

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 139 行。

```cpp
struct ArtifactCapabilityRequirements
```

### API-7885b8290182 · ndnsf_distributed_repo::ArtifactCapabilityRequirements::artifact

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 141 行。

```cpp
ArtifactReference artifact;
```

### API-f1203617d5c7 · ndnsf_distributed_repo::ArtifactCapabilityRequirements::rootSignatureAlgorithm

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 142 行。

```cpp
std::string rootSignatureAlgorithm;
```

### API-1207b12a0575 · ndnsf_distributed_repo::ArtifactCapabilityRequirements::chunkBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 143 行。

```cpp
uint64_t chunkBytes = 0;
```

### API-99aa5b7f2452 · ndnsf_distributed_repo::ArtifactCapabilityRequirements::rootEncodedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 144 行。

```cpp
uint64_t rootEncodedBytes = 0;
```

### API-306d9b468aff · ndnsf_distributed_repo::ArtifactCapabilityRequirements::pageEncodedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 145 行。

```cpp
uint64_t pageEncodedBytes = 0;
```

### API-f4b9cd2d7c0b · ndnsf_distributed_repo::ArtifactCapabilityRequirements::pageEntries

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 146 行。

```cpp
uint32_t pageEntries = 0;
```

### API-4c986b31c102 · ndnsf_distributed_repo::ArtifactCapabilityRequirements::manifestDepth

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 147 行。

```cpp
uint32_t manifestDepth = 0;
```

### API-68586ad71129 · ndnsf_distributed_repo::ArtifactCapabilityRequirements::requireResume

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 148 行。

```cpp
bool requireResume = false;
```

### API-29b01825adcc · ndnsf_distributed_repo::ArtifactCapabilityRequirements::requireReplicaReceipts

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 149 行。

```cpp
bool requireReplicaReceipts = false;
```

### API-d181f2d5c394 · ndnsf_distributed_repo::ArtifactCapabilityRequirements::validate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 151 行。

```cpp
void
  validate(const ArtifactLimits& hardLimits = {}) const;
```

### API-c17749d0b1b6 · ndnsf_distributed_repo::ArtifactCapability

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 155 行。

```cpp
struct ArtifactCapability
```

### API-b9c75c52c785 · ndnsf_distributed_repo::ArtifactCapability::repoNode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 157 行。

```cpp
std::string repoNode;
```

### API-84cbf01ca98d · ndnsf_distributed_repo::ArtifactCapability::formatVersions

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 158 行。

```cpp
std::vector<std::string> formatVersions;
```

### API-86cacc5c8242 · ndnsf_distributed_repo::ArtifactCapability::digestAlgorithms

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 159 行。

```cpp
std::vector<std::string> digestAlgorithms;
```

### API-51be8c2d6464 · ndnsf_distributed_repo::ArtifactCapability::signatureAlgorithms

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 160 行。

```cpp
std::vector<std::string> signatureAlgorithms;
```

### API-7a1a7a0d4e3f · ndnsf_distributed_repo::ArtifactCapability::maxArtifactBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 161 行。

```cpp
uint64_t maxArtifactBytes = 0;
```

### API-4867aebcd216 · ndnsf_distributed_repo::ArtifactCapability::maxChunkBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 162 行。

```cpp
uint64_t maxChunkBytes = 0;
```

### API-be8a23205c63 · ndnsf_distributed_repo::ArtifactCapability::maxRootEncodedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 163 行。

```cpp
uint64_t maxRootEncodedBytes = 0;
```

### API-272581e774a5 · ndnsf_distributed_repo::ArtifactCapability::maxPageEncodedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 164 行。

```cpp
uint64_t maxPageEncodedBytes = 0;
```

### API-d6c44bda2ddb · ndnsf_distributed_repo::ArtifactCapability::maxPageEntries

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 165 行。

```cpp
uint32_t maxPageEntries = 0;
```

### API-7001f25573e8 · ndnsf_distributed_repo::ArtifactCapability::maxManifestDepth

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 166 行。

```cpp
uint32_t maxManifestDepth = 0;
```

### API-4c38f71858e7 · ndnsf_distributed_repo::ArtifactCapability::supportsResume

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 167 行。

```cpp
bool supportsResume = false;
```

### API-b54c78f780e9 · ndnsf_distributed_repo::ArtifactCapability::supportsReplicaReceipts

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 168 行。

```cpp
bool supportsReplicaReceipts = false;
```

### API-f5f76b15d89d · ndnsf_distributed_repo::ArtifactCapability::policyEpoch

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 169 行。

```cpp
std::string policyEpoch;
```

### API-ec2c0f989f27 · ndnsf_distributed_repo::ArtifactCapability::validate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 171 行。

```cpp
void
  validate(const ArtifactLimits& hardLimits = {}) const
```

### API-227e61200ec6 · ndnsf_distributed_repo::ArtifactCapability::supports

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 217 行。

```cpp
bool
  supports(const ArtifactReference& reference, const std::string& rootSignature) const
```

### API-7e816757116c · ndnsf_distributed_repo::ArtifactCapability::incompatibilities

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 230 行。

```cpp
std::vector<std::string>
  incompatibilities(const ArtifactCapabilityRequirements& requirements,
                    const ArtifactLimits& hardLimits = {}) const;
```

### API-8a8973d9aa3c · ndnsf_distributed_repo::ArtifactCapability::requireSupport

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 234 行。

```cpp
void
  requireSupport(const ArtifactCapabilityRequirements& requirements,
                 const ArtifactLimits& hardLimits = {}) const;
```

### API-49659b67fde4 · ndnsf_distributed_repo::ArtifactManifestChild

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 239 行。

```cpp
struct ArtifactManifestChild
```

### API-229e00c2d47a · ndnsf_distributed_repo::ArtifactManifestChild::kind

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 241 行。

```cpp
std::string kind;
```

### API-0d951769ba35 · ndnsf_distributed_repo::ArtifactManifestChild::index

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 242 行。

```cpp
uint64_t index = 0;
```

### API-20928df43ef9 · ndnsf_distributed_repo::ArtifactManifestChild::offsetBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 243 行。

```cpp
uint64_t offsetBytes = 0;
```

### API-872a9d4dcc4a · ndnsf_distributed_repo::ArtifactManifestChild::lengthBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 244 行。

```cpp
uint64_t lengthBytes = 0;
```

### API-0e60abf69e31 · ndnsf_distributed_repo::ArtifactManifestChild::digestAlgorithm

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 245 行。

```cpp
std::string digestAlgorithm = "sha256";
```

### API-cc743f1af629 · ndnsf_distributed_repo::ArtifactManifestChild::digest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 246 行。

```cpp
std::string digest;
```

### API-639ab29241b3 · ndnsf_distributed_repo::ArtifactManifestChild::validate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 248 行。

```cpp
void
  validate(const ArtifactLimits& limits = {}) const
```

### API-07aa77293106 · ndnsf_distributed_repo::ArtifactRootManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 264 行。

```cpp
struct ArtifactRootManifest
```

### API-4523de1ea502 · ndnsf_distributed_repo::ArtifactRootManifest::artifact

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 266 行。

```cpp
ArtifactReference artifact;
```

### API-85fe640e3008 · ndnsf_distributed_repo::ArtifactRootManifest::packetPayloadBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 267 行。

```cpp
uint32_t packetPayloadBytes = 0;
```

### API-895c281103be · ndnsf_distributed_repo::ArtifactRootManifest::chunkBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 268 行。

```cpp
uint64_t chunkBytes = 0;
```

### API-c52f04f48a74 · ndnsf_distributed_repo::ArtifactRootManifest::namingTemplate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 269 行。

```cpp
std::string namingTemplate;
```

### API-1cf15d9fa5cd · ndnsf_distributed_repo::ArtifactRootManifest::manifestRootDigestAlgorithm

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 270 行。

```cpp
std::string manifestRootDigestAlgorithm = "sha256";
```

### API-779f2dbc48c6 · ndnsf_distributed_repo::ArtifactRootManifest::manifestRootDigest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 271 行。

```cpp
std::string manifestRootDigest;
```

### API-5fb3278ef415 · ndnsf_distributed_repo::ArtifactRootManifest::signatureAlgorithm

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 272 行。

```cpp
std::string signatureAlgorithm;
```

### API-d27908cd97fd · ndnsf_distributed_repo::ArtifactRootManifest::publisherKeyLocator

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 273 行。

```cpp
std::string publisherKeyLocator;
```

### API-39ed426d4b59 · ndnsf_distributed_repo::ArtifactRootManifest::createdAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 274 行。

```cpp
uint64_t createdAtMs = 0;
```

### API-9ea06c2da931 · ndnsf_distributed_repo::ArtifactRootManifest::expiresAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 275 行。

```cpp
uint64_t expiresAtMs = 0;
```

### API-f416e5fe379b · ndnsf_distributed_repo::ArtifactRootManifest::criticalExtensions

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 276 行。

```cpp
std::vector<std::string> criticalExtensions;
```

### API-af58b2b718b8 · ndnsf_distributed_repo::ArtifactRootManifest::validate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 278 行。

```cpp
void
  validate(uint64_t encodedBytes, const ArtifactLimits& limits = {}) const
```

### API-88f715b7ec0f · ndnsf_distributed_repo::ArtifactManifestPage

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 332 行。

```cpp
struct ArtifactManifestPage
```

### API-2e7402fe4d9e · ndnsf_distributed_repo::ArtifactManifestPage::pageVersion

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 334 行。

```cpp
std::string pageVersion = "artifact-manifest-page-v2";
```

### API-491bfd05cc8e · ndnsf_distributed_repo::ArtifactManifestPage::depth

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 335 行。

```cpp
uint32_t depth = 0;
```

### API-aa4bed758ca6 · ndnsf_distributed_repo::ArtifactManifestPage::offsetBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 336 行。

```cpp
uint64_t offsetBytes = 0;
```

### API-9b1941706cc4 · ndnsf_distributed_repo::ArtifactManifestPage::lengthBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 337 行。

```cpp
uint64_t lengthBytes = 0;
```

### API-588f84e9de12 · ndnsf_distributed_repo::ArtifactManifestPage::pageDigestAlgorithm

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 338 行。

```cpp
std::string pageDigestAlgorithm = "sha256";
```

### API-dccece687c89 · ndnsf_distributed_repo::ArtifactManifestPage::pageDigest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 339 行。

```cpp
std::string pageDigest;
```

### API-bdfedd10e84e · ndnsf_distributed_repo::ArtifactManifestPage::children

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 340 行。

```cpp
std::vector<ArtifactManifestChild> children;
```

### API-2cb7df835859 · ndnsf_distributed_repo::ArtifactManifestPage::validate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 342 行。

```cpp
void
  validate(uint64_t encodedBytes, const ArtifactLimits& limits = {}) const
```

### API-503ed5a9fc26 · ndnsf_distributed_repo::ArtifactChunk

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 385 行。

```cpp
struct ArtifactChunk
```

### API-520ad2822d8c · ndnsf_distributed_repo::ArtifactChunk::index

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 387 行。

```cpp
uint64_t index = 0;
```

### API-9e9a1e0bd4ff · ndnsf_distributed_repo::ArtifactChunk::offsetBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 388 行。

```cpp
uint64_t offsetBytes = 0;
```

### API-e4da8373d8f1 · ndnsf_distributed_repo::ArtifactChunk::lengthBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 389 行。

```cpp
uint64_t lengthBytes = 0;
```

### API-3254adb69d06 · ndnsf_distributed_repo::ArtifactChunk::digestAlgorithm

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 390 行。

```cpp
std::string digestAlgorithm = "sha256";
```

### API-4204fd185aaa · ndnsf_distributed_repo::ArtifactChunk::digest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 391 行。

```cpp
std::string digest;
```

### API-580f154338f6 · ndnsf_distributed_repo::ArtifactChunk::firstSegment

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 392 行。

```cpp
uint64_t firstSegment = 0;
```

### API-3c76f5e33591 · ndnsf_distributed_repo::ArtifactChunk::finalSegment

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 393 行。

```cpp
uint64_t finalSegment = 0;
```

### API-1c889fd6af1e · ndnsf_distributed_repo::ArtifactChunk::validate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 395 行。

```cpp
void
  validate(const ArtifactReference& artifact,
           const ArtifactLimits& limits = {}) const
```

### API-78a3f414449e · ndnsf_distributed_repo::ArtifactUploadLease

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 411 行。

```cpp
struct ArtifactUploadLease
```

### API-a648858472d4 · ndnsf_distributed_repo::ArtifactUploadLease::leaseId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 413 行。

```cpp
std::string leaseId;
```

### API-950864d1c214 · ndnsf_distributed_repo::ArtifactUploadLease::operationId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 414 行。

```cpp
std::string operationId;
```

### API-76f74b8d8132 · ndnsf_distributed_repo::ArtifactUploadLease::repoNode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 415 行。

```cpp
std::string repoNode;
```

### API-011f196a6d54 · ndnsf_distributed_repo::ArtifactUploadLease::artifact

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 416 行。

```cpp
ArtifactReference artifact;
```

### API-051a31033f73 · ndnsf_distributed_repo::ArtifactUploadLease::reservedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 417 行。

```cpp
uint64_t reservedBytes = 0;
```

### API-3c086b237497 · ndnsf_distributed_repo::ArtifactUploadLease::issuedAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 418 行。

```cpp
uint64_t issuedAtMs = 0;
```

### API-e9d58b6e51da · ndnsf_distributed_repo::ArtifactUploadLease::expiresAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 419 行。

```cpp
uint64_t expiresAtMs = 0;
```

### API-ec187e81f343 · ndnsf_distributed_repo::ArtifactUploadLease::replayId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 420 行。

```cpp
std::string replayId;
```

### API-2c8397194225 · ndnsf_distributed_repo::ArtifactUploadLease::validate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 422 行。

```cpp
void
  validate(uint64_t nowMs, const ArtifactLimits& limits = {}) const
```

### API-629da0c0df91 · ndnsf_distributed_repo::ArtifactReplicaReceipt

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 439 行。

```cpp
struct ArtifactReplicaReceipt
```

### API-bee56d0768b3 · ndnsf_distributed_repo::ArtifactReplicaReceipt::receiptId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 441 行。

```cpp
std::string receiptId;
```

### API-7830062e114b · ndnsf_distributed_repo::ArtifactReplicaReceipt::operationId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 442 行。

```cpp
std::string operationId;
```

### API-639683f37486 · ndnsf_distributed_repo::ArtifactReplicaReceipt::repoNode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 443 行。

```cpp
std::string repoNode;
```

### API-18f4d3161cc0 · ndnsf_distributed_repo::ArtifactReplicaReceipt::artifact

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 444 行。

```cpp
ArtifactReference artifact;
```

### API-32fcc452b548 · ndnsf_distributed_repo::ArtifactReplicaReceipt::committedAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 445 行。

```cpp
uint64_t committedAtMs = 0;
```

### API-f1686b1da8fc · ndnsf_distributed_repo::ArtifactReplicaReceipt::storageGeneration

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 446 行。

```cpp
uint64_t storageGeneration = 0;
```

### API-2b7052f30a5f · ndnsf_distributed_repo::ArtifactReplicaReceipt::policyEpoch

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 447 行。

```cpp
std::string policyEpoch;
```

### API-bd28ea512351 · ndnsf_distributed_repo::ArtifactReplicaReceipt::state

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 448 行。

```cpp
std::string state = "COMMITTED";
```

### API-d4a0b4215282 · ndnsf_distributed_repo::ArtifactReplicaReceipt::validate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp`，第 450 行。

```cpp
void
  validate(const ArtifactLimits& limits = {}) const
```

## NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp

源码 SHA-256：`d6705a458b8c783e24c35775422b74ac7dbe809e5722789130758f3ba9beb2f8`。

### API-76c09a2dae7a · ndnsf_distributed_repo::ArtifactBackendMigrationDiagnostics

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 12 行。

```cpp
struct ArtifactBackendMigrationDiagnostics
```

### API-8f80a61054ff · ndnsf_distributed_repo::ArtifactBackendMigrationDiagnostics::runtimeSchemaGeneration

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 14 行。

```cpp
uint64_t runtimeSchemaGeneration = 12;
```

### API-62a92645950d · ndnsf_distributed_repo::ArtifactBackendMigrationDiagnostics::databaseSchemaGeneration

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 15 行。

```cpp
uint64_t databaseSchemaGeneration = 0;
```

### API-d817b08f28c3 · ndnsf_distributed_repo::ArtifactBackendMigrationDiagnostics::previousSchemaGeneration

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 16 行。

```cpp
uint64_t previousSchemaGeneration = 0;
```

### API-73889562f5dc · ndnsf_distributed_repo::ArtifactBackendMigrationDiagnostics::maxWriteSchemaGeneration

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 17 行。

```cpp
uint64_t maxWriteSchemaGeneration = 12;
```

### API-63dc79af617e · ndnsf_distributed_repo::ArtifactBackendMigrationDiagnostics::writesEnabled

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 18 行。

```cpp
bool writesEnabled = false;
```

### API-7ba03c2439ec · ndnsf_distributed_repo::ArtifactBackendMigrationDiagnostics::destructiveChanges

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 19 行。

```cpp
bool destructiveChanges = false;
```

### API-bd0790f14ddb · ndnsf_distributed_repo::ArtifactBackendMigrationDiagnostics::action

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 20 行。

```cpp
std::string action;
```

### API-a8d009a2228b · ndnsf_distributed_repo::ArtifactBackendMigrationDiagnostics::reason

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 21 行。

```cpp
std::string reason;
```

### API-b2f98fc97114 · ndnsf_distributed_repo::FilesystemArtifactPayloadStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 32 行。

```cpp
class FilesystemArtifactPayloadStore final : public PayloadStore
```

### API-3a6a99d44db3 · ndnsf_distributed_repo::FilesystemArtifactPayloadStore::FilesystemArtifactPayloadStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 35 行。

```cpp
explicit FilesystemArtifactPayloadStore(
    std::string rootPath, uint64_t maxRangeBytes = 16 * 1024 * 1024);
```

### API-68898c75d2f5 · ndnsf_distributed_repo::FilesystemArtifactPayloadStore::begin

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 38 行。

```cpp
void begin(const ArtifactReference& artifact, uint64_t generation) override;
```

### API-54c669879b2e · ndnsf_distributed_repo::FilesystemArtifactPayloadStore::writeRange

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 39 行。

```cpp
void writeRange(const ArtifactReference& artifact, uint64_t generation,
                  ArtifactByteRange range,
                  const std::vector<uint8_t>& bytes) override;
```

### API-5906393fb015 · ndnsf_distributed_repo::FilesystemArtifactPayloadStore::readRange

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 42 行。

```cpp
std::vector<uint8_t> readRange(const ArtifactReference& artifact,
                                 uint64_t generation,
                                 ArtifactByteRange range) const override;
```

### API-e0939dcfa46e · ndnsf_distributed_repo::FilesystemArtifactPayloadStore::markVerified

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 45 行。

```cpp
void markVerified(const ArtifactReference& artifact, uint64_t generation,
                    ArtifactByteRange range) override;
```

### API-f3d4a0c3b517 · ndnsf_distributed_repo::FilesystemArtifactPayloadStore::verifiedRanges

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 47 行。

```cpp
std::vector<ArtifactByteRange>
  verifiedRanges(const ArtifactReference& artifact, uint64_t generation) const override;
```

### API-6beba4f0e361 · ndnsf_distributed_repo::FilesystemArtifactPayloadStore::flush

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 49 行。

```cpp
void flush(const ArtifactReference& artifact, uint64_t generation) override;
```

### API-8954ac967831 · ndnsf_distributed_repo::FilesystemArtifactPayloadStore::finalize

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 50 行。

```cpp
void finalize(const ArtifactReference& artifact, uint64_t generation) override;
```

### API-90ce4c49cca2 · ndnsf_distributed_repo::FilesystemArtifactPayloadStore::isCommitted

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 51 行。

```cpp
bool isCommitted(const ArtifactReference& artifact,
                   uint64_t generation) const override;
```

### API-2a1ce0784309 · ndnsf_distributed_repo::FilesystemArtifactPayloadStore::abort

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 53 行。

```cpp
void abort(const ArtifactReference& artifact, uint64_t generation) override;
```

### API-5c9c36389dd3 · ndnsf_distributed_repo::FilesystemArtifactPayloadStore::rootPath

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 55 行。

```cpp
const std::string& rootPath() const noexcept;
```

### API-2e1e6876266a · ndnsf_distributed_repo::FilesystemArtifactPayloadStore::committedPath

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 56 行。

```cpp
std::string committedPath(const ArtifactReference& artifact) const;
```

### API-324abc807957 · ndnsf_distributed_repo::FilesystemArtifactPayloadStore::stagingPath

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 57 行。

```cpp
std::string stagingPath(const ArtifactReference& artifact,
                          uint64_t generation) const;
```

### API-f585fa3b7a78 · ndnsf_distributed_repo::SqliteArtifactMetadataStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 68 行。

```cpp
class SqliteArtifactMetadataStore final : public MetadataStore
```

### API-6efbffcca664 · ndnsf_distributed_repo::SqliteArtifactMetadataStore::SqliteArtifactMetadataStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 71 行。

```cpp
explicit SqliteArtifactMetadataStore(
    std::string databasePath, bool artifactWritesEnabled = true,
    uint64_t maxWriteSchemaGeneration = 12);
```

### API-0828d41d51bd · ndnsf_distributed_repo::SqliteArtifactMetadataStore::~SqliteArtifactMetadataStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 74 行。

```cpp
~SqliteArtifactMetadataStore() override;
```

### API-d2d10d594cad · ndnsf_distributed_repo::SqliteArtifactMetadataStore::SqliteArtifactMetadataStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 76 行。

```cpp
SqliteArtifactMetadataStore(const SqliteArtifactMetadataStore&) = delete;
```

### API-f7bc2b5ec297 · ndnsf_distributed_repo::SqliteArtifactMetadataStore::operator=

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 77 行。

```cpp
SqliteArtifactMetadataStore&
  operator=(const SqliteArtifactMetadataStore&) = delete;
```

### API-581655125375 · ndnsf_distributed_repo::SqliteArtifactMetadataStore::schemaGeneration

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 80 行。

```cpp
uint64_t schemaGeneration() const override;
```

### API-0b33669c963b · ndnsf_distributed_repo::SqliteArtifactMetadataStore::artifactWritesEnabled

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 81 行。

```cpp
bool artifactWritesEnabled() const noexcept;
```

### API-eb16c33ee10a · ndnsf_distributed_repo::SqliteArtifactMetadataStore::migrationDiagnostics

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 82 行。

```cpp
ArtifactBackendMigrationDiagnostics migrationDiagnostics() const;
```

### API-3448287d3b21 · ndnsf_distributed_repo::SqliteArtifactMetadataStore::appendLifecycleEvent

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 83 行。

```cpp
void appendLifecycleEvent(const ArtifactLifecycleEvent& event) override;
```

### API-3e7507d2a6bb · ndnsf_distributed_repo::SqliteArtifactMetadataStore::lifecycleEvents

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 84 行。

```cpp
std::vector<ArtifactLifecycleEvent>
  lifecycleEvents(const std::string& operationId) const override;
```

### API-6231a66d6536 · ndnsf_distributed_repo::SqliteArtifactMetadataStore::currentLifecycleState

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 86 行。

```cpp
ArtifactLifecycleState
  currentLifecycleState(const std::string& operationId) const override;
```

### API-85c9b9a90034 · ndnsf_distributed_repo::SqliteArtifactMetadataStore::databasePath

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 89 行。

```cpp
const std::string& databasePath() const noexcept;
```

### API-173c27b2d92c · ndnsf_distributed_repo::makeFilesystemArtifactRepositoryStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/FilesystemArtifactStore.hpp`，第 96 行。

```cpp
std::unique_ptr<RepositoryStoreFacade>
makeFilesystemArtifactRepositoryStore(const std::string& rootPath,
                                      const std::string& ownerId);
```

## NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp

源码 SHA-256：`a89eb7708c82d5b8d46241b5094c2daf697db3fc3738790fa4c8034a594769fc`。

### API-485b7754e578 · ndn_service_framework::LocalServiceRegistry

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 13 行。

```cpp
class LocalServiceRegistry
```

### API-21bd25555b5c · ndnsf_distributed_repo::RepoNode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 18 行。

```cpp
class RepoNode
```

### API-741d440dfb70 · ndnsf_distributed_repo::StoreOptions

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 20 行。

```cpp
struct StoreOptions
```

### API-19c40c02e4f2 · ndnsf_distributed_repo::StoreOptions::objectType

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 22 行。

```cpp
std::string objectType = "object";
```

### API-05ac61ffd833 · ndnsf_distributed_repo::StoreOptions::replicationFactor

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 23 行。

```cpp
uint32_t replicationFactor = 1;
```

### API-bbe1d3e53e30 · ndnsf_distributed_repo::StoreOptions::replicaNodes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 24 行。

```cpp
std::vector<std::string> replicaNodes;
```

### API-379b32901ba1 · ndnsf_distributed_repo::StoreOptions::policyEpoch

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 25 行。

```cpp
std::string policyEpoch;
```

### API-25b338db1605 · ndnsf_distributed_repo::RepoClient

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 28 行。

```cpp
class RepoClient
```

### API-1b22073ecd7f · ndnsf_distributed_repo::RepoClient::* DEFAULT_SERVICE_NAME

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 31 行。

```cpp
static constexpr const char* DEFAULT_SERVICE_NAME = "/NDNSF/DistributedRepo";
```

### API-3690bad70c85 · ndnsf_distributed_repo::RepoClient::put

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 33 行。

```cpp
static RepoObjectManifest put(RepoNode& node,
                                const std::string& objectName,
                                const std::vector<uint8_t>& payload,
                                StoreOptions options = {});
```

### API-71bbc7c13277 · ndnsf_distributed_repo::RepoClient::get

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 38 行。

```cpp
static std::vector<uint8_t> get(const RepoNode& node,
                                  const std::string& objectName);
```

### API-a49e665e81ae · ndnsf_distributed_repo::RepoClient::getManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 41 行。

```cpp
static RepoObjectManifest getManifest(const RepoNode& node,
                                        const std::string& objectName);
```

### API-7fb155051f40 · ndnsf_distributed_repo::RepoClient::list

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 44 行。

```cpp
static std::vector<RepoObjectManifest> list(const RepoNode& node);
```

### API-3597142a1d7d · ndnsf_distributed_repo::RepoClient::remove

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 46 行。

```cpp
static bool remove(RepoNode& node,
                     const std::string& objectName);
```

### API-325ed18957fe · ndnsf_distributed_repo::RepoClient::putDataPacket

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 50 行。

```cpp
static RepoObjectManifest putDataPacket(RepoNode& node,
                                          const std::vector<uint8_t>& wire);
```

原始接口说明：

```text
/** Store one signed Data packet under the exact name encoded in its wire. */
```

### API-c270a97a5de4 · ndnsf_distributed_repo::RepoClient::getDataPacket

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 54 行。

```cpp
static std::vector<uint8_t> getDataPacket(const RepoNode& node,
                                            const std::string& dataName);
```

原始接口说明：

```text
/** Retrieve the original packet wire by its complete NDN Data name. */
```

### API-208878700957 · ndnsf_distributed_repo::RepoClient::getDataPackets

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 62 行。

```cpp
static std::vector<std::vector<uint8_t>> getDataPackets(
    const RepoNode& node,
    const RepoObjectManifest& manifest);
```

原始接口说明：

```text
/**
   * Retrieve an application-produced packet set by the manifest's ordered exact
   * Data names. The operation validates the complete name encoded in every wire
   * and throws before returning if the packet index is incomplete or invalid.
   */
```

### API-e775c86b8dfa · ndnsf_distributed_repo::RepoClient::insert

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 66 行。

```cpp
static RepoOperationStatus insert(
    RepoNode& node,
    const RepoDataReference& reference);
```

### API-6c0504f5e18f · ndnsf_distributed_repo::RepoClient::insertPayload

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 76 行。

```cpp
static RepoOperationStatus insertPayload(
    RepoNode& node,
    const std::string& objectName,
    const std::vector<uint8_t>& payload,
    ndn::KeyChain& keyChain,
    const ndn::security::SigningInfo& signingInfo,
    StoreOptions options = {},
    size_t maxSegmentPayload = 6000);
```

原始接口说明：

```text
/**
   * Convenience INSERT adapter for callers that have payload bytes instead of
   * pre-published Data. The adapter segments and signs the payload under
   * objectName using ndn-cxx Segmenter, then stores the resulting Data wire
   * packets under their exact encoded Data names through insert().
   */
```

### API-fb1a3c182f71 · ndnsf_distributed_repo::RepoClient::status

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 85 行。

```cpp
static RepoOperationStatus status(const RepoNode& node,
                                    const std::string& operationId);
```

### API-3426faf86399 · ndnsf_distributed_repo::RepoClient::catalogStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 88 行。

```cpp
static RepoCatalogStatus catalogStatus(const RepoNode& node);
```

### API-92f825a0f88f · ndnsf_distributed_repo::RepoClient::cacheStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 90 行。

```cpp
static RepoCacheStatus cacheStatus(const RepoNode& node);
```

### API-c0c608b66946 · ndnsf_distributed_repo::RepoClient::catalogSnapshot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 92 行。

```cpp
static RepoCatalogDelta catalogSnapshot(const RepoNode& node);
```

### API-8a6c373170d8 · ndnsf_distributed_repo::RepoClient::catalogDelta

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 94 行。

```cpp
static RepoCatalogDelta catalogDelta(const RepoNode& node, uint64_t sinceEpoch);
```

### API-fcda4ff0344b · ndnsf_distributed_repo::RepoClient::catalogLookup

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 96 行。

```cpp
static RepoCatalogEntry catalogLookup(const RepoNode& node,
                                        const std::string& objectName);
```

### API-c3a7ce1ceba6 · ndnsf_distributed_repo::RepoClient::putSegmented

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 108 行。

```cpp
static RepoObjectManifest putSegmented(
    RepoNode& node,
    const std::string& objectName,
    const std::vector<uint8_t>& payload,
    StoreOptions options = {},
    size_t maxSegmentPayload = 6000);
```

原始接口说明：

```text
/**
   * Legacy opaque-byte helper: store a large object as object-level chunks named
   * <objectName>/seg/<index>, then store a manifest-only parent object.
   *
   * This is not the canonical NDN Data packet API. Repo storage is opaque here:
   * the repo does not perform APP trust, signature, or
   * hash verification while storing. Use getSegmented() on the APP side to
   * reassemble and verify size/hash against the returned manifest.
   */
```

### API-2751d910144a · ndnsf_distributed_repo::RepoClient::getSegmented

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 119 行。

```cpp
static std::vector<uint8_t> getSegmented(
    const RepoNode& node,
    const RepoObjectManifest& manifest);
```

原始接口说明：

```text
/**
   * Fetch chunks described by manifest, reassemble them, and verify APP-side
   * size/hash metadata. Throws std::runtime_error on mismatch.
   */
```

### API-06202a673376 · ndnsf_distributed_repo::RepoClient::getObject

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 128 行。

```cpp
static std::vector<uint8_t> getObject(
    const RepoNode& node,
    const RepoObjectManifest& manifest);
```

原始接口说明：

```text
/**
   * Fetch one logical repo object described by manifest. This is the preferred
   * object-level API for callers that do not care whether the object was stored
   * as one payload or as object-level chunks.
   */
```

### API-1f4fb56a9f20 · ndnsf_distributed_repo::RepoClient::localPut

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 132 行。

```cpp
static RepoObjectManifest localPut(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName,
    const std::vector<uint8_t>& payload,
    StoreOptions options = {});
```

### API-c1845fb1bb7b · ndnsf_distributed_repo::RepoClient::localGet

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 139 行。

```cpp
static std::vector<uint8_t> localGet(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName);
```

### API-40e3c3c734ba · ndnsf_distributed_repo::RepoClient::localGetManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 144 行。

```cpp
static RepoObjectManifest localGetManifest(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName);
```

### API-02eab20ad4cd · ndnsf_distributed_repo::RepoClient::localList

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 149 行。

```cpp
static std::vector<RepoObjectManifest> localList(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix);
```

### API-a4385e25551e · ndnsf_distributed_repo::RepoClient::localRemove

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 153 行。

```cpp
static bool localRemove(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName);
```

### API-3a7b3f893966 · ndnsf_distributed_repo::RepoClient::localInsert

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 158 行。

```cpp
static RepoOperationStatus localInsert(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const RepoDataReference& reference);
```

### API-872743cdf7b1 · ndnsf_distributed_repo::RepoClient::localStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 163 行。

```cpp
static RepoOperationStatus localStatus(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& operationId);
```

### API-f56247baadeb · ndnsf_distributed_repo::RepoClient::localCatalogStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 168 行。

```cpp
static RepoCatalogStatus localCatalogStatus(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix);
```

### API-397b1cffb073 · ndnsf_distributed_repo::RepoClient::localCacheStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 172 行。

```cpp
static RepoCacheStatus localCacheStatus(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix);
```

### API-e3118497bb4e · ndnsf_distributed_repo::RepoClient::localCatalogSnapshot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 176 行。

```cpp
static RepoCatalogDelta localCatalogSnapshot(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix);
```

### API-ffd749a9351a · ndnsf_distributed_repo::RepoClient::localCatalogDelta

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 180 行。

```cpp
static RepoCatalogDelta localCatalogDelta(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    uint64_t sinceEpoch);
```

### API-79b8e21f7245 · ndnsf_distributed_repo::RepoClient::localCatalogLookup

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 185 行。

```cpp
static RepoCatalogEntry localCatalogLookup(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName);
```

### API-4386fdeceb10 · ndnsf_distributed_repo::RepoClient::localPutSegmented

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 194 行。

```cpp
static RepoObjectManifest localPutSegmented(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName,
    const std::vector<uint8_t>& payload,
    StoreOptions options = {},
    size_t maxSegmentPayload = 6000);
```

原始接口说明：

```text
/**
   * Same as putSegmented(), but invokes a repo registered in the same trusted
   * LocalServiceRegistry instead of using NDNSF network messages.
   */
```

### API-302296fd05b8 · ndnsf_distributed_repo::RepoClient::localGetSegmented

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 205 行。

```cpp
static std::vector<uint8_t> localGetSegmented(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const RepoObjectManifest& manifest);
```

原始接口说明：

```text
/**
   * Same as getSegmented(), but fetches chunks from the local registry.
   */
```

### API-c79276954c35 · ndnsf_distributed_repo::RepoClient::localGetObject

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 213 行。

```cpp
static std::vector<uint8_t> localGetObject(
    ndn_service_framework::LocalServiceRegistry& registry,
    const ndn::Name& repoServicePrefix,
    const RepoObjectManifest& manifest);
```

原始接口说明：

```text
/**
   * Local trusted equivalent of getObject().
   */
```

### API-5eeb0c87367a · ndnsf_distributed_repo::RepoClient::makeManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 218 行。

```cpp
static RepoObjectManifest makeManifest(std::string objectName,
                                         std::string objectType,
                                         const std::vector<uint8_t>& payload,
                                         uint32_t replicationFactor,
                                         std::vector<std::string> replicaNodes,
                                         std::string policyEpoch);
```

### API-ecfbefc678b6 · ndnsf_distributed_repo::RepoClient::makeRequest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 225 行。

```cpp
static ndn_service_framework::RequestMessage makeRequest(
    const std::vector<uint8_t>& payload);
```

### API-25bae982ed92 · ndnsf_distributed_repo::RepoClient::requestCapability

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 228 行。

```cpp
static ndn::Name requestCapability(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
```

### API-8292ff8bb932 · ndnsf_distributed_repo::RepoClient::requestCacheStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 235 行。

```cpp
static ndn::Name requestCacheStatus(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
```

### API-82d12b602301 · ndnsf_distributed_repo::RepoClient::requestStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 242 行。

```cpp
static ndn::Name requestStore(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const RepoObjectManifest& manifest,
    const std::vector<uint8_t>& payload,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
```

### API-4afcb74c485b · ndnsf_distributed_repo::RepoClient::requestInsert

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 251 行。

```cpp
static ndn::Name requestInsert(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const RepoDataReference& reference,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
```

### API-14b5e7834a74 · ndnsf_distributed_repo::RepoClient::requestFetch

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 259 行。

```cpp
static ndn::Name requestFetch(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
```

### API-efc7df94513b · ndnsf_distributed_repo::RepoClient::requestManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 267 行。

```cpp
static ndn::Name requestManifest(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
```

### API-39f96e0592f1 · ndnsf_distributed_repo::RepoClient::requestInventory

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 275 行。

```cpp
static ndn::Name requestInventory(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
```

### API-57dede3e422d · ndnsf_distributed_repo::RepoClient::requestDelete

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 282 行。

```cpp
static ndn::Name requestDelete(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const std::string& objectName,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
```

### API-53049cab0c37 · ndnsf_distributed_repo::RepoClient::requestStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp`，第 290 行。

```cpp
static ndn::Name requestStatus(
    ndn_service_framework::ServiceUser& user,
    const ndn::Name& repoServicePrefix,
    const std::string& operationId,
    int timeoutMs,
    ndn_service_framework::ServiceUser::TimeoutHandler onTimeout,
    ndn_service_framework::ServiceUser::ResponseHandler onResponse);
```

## NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp

源码 SHA-256：`68172acaf94a97a6d34b17499d782f59f60f31638961e6f9508cf609d31dc902`。

### API-d31864878741 · ndnsf_distributed_repo::RepoCore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 13 行。

```cpp
class RepoCore
```

### API-60b8aeb6825b · ndnsf_distributed_repo::RepoCore::RepoCore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 16 行。

```cpp
RepoCore(StorageCapability capability, std::shared_ptr<RepoStoreBackend> store);
```

### API-ac3b30e34e33 · ndnsf_distributed_repo::RepoCore::put

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 18 行。

```cpp
RepoObjectManifest put(const std::string& objectName,
                         const std::vector<uint8_t>& payload,
                         const std::string& objectType = "object",
                         uint32_t replicationFactor = 1,
                         const std::string& policyEpoch = "",
                         std::vector<std::string> replicaNodes = {});
```

### API-2e22038586e7 · ndnsf_distributed_repo::RepoCore::get

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 25 行。

```cpp
std::vector<uint8_t> get(const std::string& objectName) const;
```

### API-d77475516039 · ndnsf_distributed_repo::RepoCore::getManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 27 行。

```cpp
RepoObjectManifest getManifest(const std::string& objectName) const;
```

### API-968f79944e62 · ndnsf_distributed_repo::RepoCore::list

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 29 行。

```cpp
std::vector<RepoObjectManifest> list() const;
```

### API-7f708024d1f1 · ndnsf_distributed_repo::RepoCore::remove

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 31 行。

```cpp
bool remove(const std::string& objectName);
```

### API-562871d8a20e · ndnsf_distributed_repo::RepoCore::putManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 33 行。

```cpp
RepoObjectManifest putManifest(const RepoObjectManifest& manifest);
```

### API-1f2e097e5ead · ndnsf_distributed_repo::RepoCore::putDataPacket

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 35 行。

```cpp
RepoObjectManifest putDataPacket(const std::string& dataName,
                                   const std::vector<uint8_t>& wire);
```

### API-2fed25b48953 · ndnsf_distributed_repo::RepoCore::getDataPacket

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 38 行。

```cpp
std::vector<uint8_t> getDataPacket(const std::string& dataName) const;
```

### API-49d0155d01b9 · ndnsf_distributed_repo::RepoCore::hasDataPacket

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 40 行。

```cpp
bool hasDataPacket(const std::string& dataName) const;
```

### API-3206c71a31be · ndnsf_distributed_repo::RepoCore::handleStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 42 行。

```cpp
std::vector<uint8_t> handleStore(const std::vector<uint8_t>& request);
```

### API-36549131eb91 · ndnsf_distributed_repo::RepoCore::handleStoreManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 44 行。

```cpp
std::vector<uint8_t> handleStoreManifest(const std::vector<uint8_t>& request);
```

### API-92e08a6818cc · ndnsf_distributed_repo::RepoCore::handleFetch

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 46 行。

```cpp
std::vector<uint8_t> handleFetch(const std::vector<uint8_t>& request) const;
```

### API-ed4141f41c77 · ndnsf_distributed_repo::RepoCore::handleManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 48 行。

```cpp
std::vector<uint8_t> handleManifest(const std::vector<uint8_t>& request) const;
```

### API-bd972c448f2c · ndnsf_distributed_repo::RepoCore::handleInventory

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 50 行。

```cpp
std::vector<uint8_t> handleInventory() const;
```

### API-037642dec848 · ndnsf_distributed_repo::RepoCore::handleCapability

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 52 行。

```cpp
std::vector<uint8_t> handleCapability() const;
```

### API-d3f541f06e3e · ndnsf_distributed_repo::RepoCore::cacheStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 54 行。

```cpp
RepoCacheStatus cacheStatus() const;
```

### API-53fcc13b3a4f · ndnsf_distributed_repo::RepoCore::handleCacheStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 56 行。

```cpp
std::vector<uint8_t> handleCacheStatus() const;
```

### API-08ca9144cf52 · ndnsf_distributed_repo::RepoCore::catalogStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 58 行。

```cpp
RepoCatalogStatus catalogStatus() const;
```

### API-b28b415568be · ndnsf_distributed_repo::RepoCore::catalogSnapshot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 60 行。

```cpp
RepoCatalogDelta catalogSnapshot() const;
```

### API-e296daab62b3 · ndnsf_distributed_repo::RepoCore::catalogDelta

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 62 行。

```cpp
RepoCatalogDelta catalogDelta(uint64_t sinceEpoch) const;
```

### API-2c18852cdee0 · ndnsf_distributed_repo::RepoCore::catalogLookup

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 64 行。

```cpp
RepoCatalogEntry catalogLookup(const std::string& objectName) const;
```

### API-b1c6f23c92d2 · ndnsf_distributed_repo::RepoCore::handleCatalogStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 66 行。

```cpp
std::vector<uint8_t> handleCatalogStatus() const;
```

### API-06099f302644 · ndnsf_distributed_repo::RepoCore::handleCatalogSnapshot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 68 行。

```cpp
std::vector<uint8_t> handleCatalogSnapshot() const;
```

### API-2df1dde58126 · ndnsf_distributed_repo::RepoCore::handleCatalogDelta

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 70 行。

```cpp
std::vector<uint8_t> handleCatalogDelta(const std::vector<uint8_t>& request) const;
```

### API-18441981c1be · ndnsf_distributed_repo::RepoCore::handleCatalogLookup

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 72 行。

```cpp
std::vector<uint8_t> handleCatalogLookup(const std::vector<uint8_t>& request) const;
```

### API-2c82ef717273 · ndnsf_distributed_repo::RepoCore::handleDelete

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoCore.hpp`，第 74 行。

```cpp
std::vector<uint8_t> handleDelete(const std::vector<uint8_t>& request);
```

## NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp

源码 SHA-256：`9d2e986221293fc1be16416e61ab2b2c11970c284c7096a98be123e41747cea1`。

### API-abe019f5e2fe · ndn_service_framework::LocalServiceRegistry

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 13 行。

```cpp
class LocalServiceRegistry
```

### API-25055523ab00 · ndn_service_framework::ResponseMessage

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 14 行。

```cpp
class ResponseMessage
```

### API-4b578a2e735d · ndn_service_framework::ServiceProvider

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 15 行。

```cpp
class ServiceProvider
```

### API-f3aed7bde9e4 · ndnsf_distributed_repo::RepoNode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 20 行。

```cpp
class RepoNode
```

### API-072e4ba38bd4 · ndnsf_distributed_repo::RepoNode::DataReferenceFetcher

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 23 行。

```cpp
using DataReferenceFetcher =
    std::function<std::vector<std::vector<uint8_t>>(const RepoDataReference&)>;
```

### API-e2ea3cdb1b60 · ndnsf_distributed_repo::RepoNode::RepoNode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 26 行。

```cpp
RepoNode(ndn::Name servicePrefix,
           StorageCapability capability,
           std::shared_ptr<RepoStoreBackend> store);
```

### API-8daf50e2eaf9 · ndnsf_distributed_repo::RepoNode::servicePrefix

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 30 行。

```cpp
const ndn::Name& servicePrefix() const;
```

### API-f158711b4122 · ndnsf_distributed_repo::RepoNode::core

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 32 行。

```cpp
RepoCore& core();
```

### API-17fd568b1055 · ndnsf_distributed_repo::RepoNode::core

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 34 行。

```cpp
const RepoCore& core() const;
```

### API-60a7805905f3 · ndnsf_distributed_repo::RepoNode::registerLocalServices

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 36 行。

```cpp
void registerLocalServices(ndn_service_framework::LocalServiceRegistry& registry);
```

### API-db9e3ee71199 · ndnsf_distributed_repo::RepoNode::put

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 38 行。

```cpp
RepoObjectManifest put(const std::string& objectName,
                         const std::vector<uint8_t>& payload,
                         const std::string& objectType = "object",
                         uint32_t replicationFactor = 1,
                         const std::string& policyEpoch = "",
                         std::vector<std::string> replicaNodes = {});
```

### API-2440b3f8a627 · ndnsf_distributed_repo::RepoNode::get

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 45 行。

```cpp
std::vector<uint8_t> get(const std::string& objectName) const;
```

### API-4039aaaba9f5 · ndnsf_distributed_repo::RepoNode::getManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 47 行。

```cpp
RepoObjectManifest getManifest(const std::string& objectName) const;
```

### API-e5c37cf7a489 · ndnsf_distributed_repo::RepoNode::list

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 49 行。

```cpp
std::vector<RepoObjectManifest> list() const;
```

### API-8b111fd4033e · ndnsf_distributed_repo::RepoNode::remove

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 51 行。

```cpp
bool remove(const std::string& objectName);
```

### API-f3c27255a016 · ndnsf_distributed_repo::RepoNode::putDataPacket

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 53 行。

```cpp
RepoObjectManifest putDataPacket(const std::string& dataName,
                                   const std::vector<uint8_t>& wire);
```

### API-ffce330c4a2c · ndnsf_distributed_repo::RepoNode::getDataPacket

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 56 行。

```cpp
std::vector<uint8_t> getDataPacket(const std::string& dataName) const;
```

### API-12f2f69e6d35 · ndnsf_distributed_repo::RepoNode::hasDataPacket

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 58 行。

```cpp
bool hasDataPacket(const std::string& dataName) const;
```

### API-773c650878f8 · ndnsf_distributed_repo::RepoNode::setDataReferenceFetcher

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 60 行。

```cpp
void setDataReferenceFetcher(DataReferenceFetcher fetcher);
```

### API-75a4c7b01100 · ndnsf_distributed_repo::RepoNode::insertWirePackets

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 62 行。

```cpp
RepoOperationStatus insertWirePackets(
    const RepoDataReference& reference,
    const std::vector<std::vector<uint8_t>>& wirePackets);
```

### API-8dc278e4ac0d · ndnsf_distributed_repo::RepoNode::handleStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 66 行。

```cpp
std::vector<uint8_t> handleStore(const std::vector<uint8_t>& request);
```

### API-bee51ae28047 · ndnsf_distributed_repo::RepoNode::handleInsert

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 68 行。

```cpp
std::vector<uint8_t> handleInsert(const std::vector<uint8_t>& request);
```

### API-f07e54481a58 · ndnsf_distributed_repo::RepoNode::handleStoreManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 70 行。

```cpp
std::vector<uint8_t> handleStoreManifest(const std::vector<uint8_t>& request);
```

### API-0797fa0783c8 · ndnsf_distributed_repo::RepoNode::handleFetch

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 72 行。

```cpp
std::vector<uint8_t> handleFetch(const std::vector<uint8_t>& request) const;
```

### API-40713d78c6ce · ndnsf_distributed_repo::RepoNode::handleManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 74 行。

```cpp
std::vector<uint8_t> handleManifest(const std::vector<uint8_t>& request) const;
```

### API-52cf1298ca87 · ndnsf_distributed_repo::RepoNode::handleInventory

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 76 行。

```cpp
std::vector<uint8_t> handleInventory() const;
```

### API-834ffa287f91 · ndnsf_distributed_repo::RepoNode::handleCapability

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 78 行。

```cpp
std::vector<uint8_t> handleCapability() const;
```

### API-4355c795e50f · ndnsf_distributed_repo::RepoNode::cacheStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 80 行。

```cpp
RepoCacheStatus cacheStatus() const;
```

### API-11119b284d36 · ndnsf_distributed_repo::RepoNode::handleCacheStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 82 行。

```cpp
std::vector<uint8_t> handleCacheStatus() const;
```

### API-7f0aab83de5e · ndnsf_distributed_repo::RepoNode::handleCatalogStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 84 行。

```cpp
std::vector<uint8_t> handleCatalogStatus() const;
```

### API-b8d61e42054d · ndnsf_distributed_repo::RepoNode::handleCatalogSnapshot

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 86 行。

```cpp
std::vector<uint8_t> handleCatalogSnapshot() const;
```

### API-d87560b98d15 · ndnsf_distributed_repo::RepoNode::handleCatalogDelta

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 88 行。

```cpp
std::vector<uint8_t> handleCatalogDelta(const std::vector<uint8_t>& request) const;
```

### API-7b03b2871dce · ndnsf_distributed_repo::RepoNode::handleCatalogLookup

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 90 行。

```cpp
std::vector<uint8_t> handleCatalogLookup(const std::vector<uint8_t>& request) const;
```

### API-28a4e0518655 · ndnsf_distributed_repo::RepoNode::handleDelete

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 92 行。

```cpp
std::vector<uint8_t> handleDelete(const std::vector<uint8_t>& request);
```

### API-4c7688bd5126 · ndnsf_distributed_repo::RepoNode::handleStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoNode.hpp`，第 94 行。

```cpp
std::vector<uint8_t> handleStatus(const std::vector<uint8_t>& request) const;
```

## NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp

源码 SHA-256：`74ca1596c71cf9db0fb6750a4dce787737a32152ed6a7c437d4dfabdc0f4f038`。

### API-91fee41583ea · ndnsf_distributed_repo::makeRepoServiceName

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 14 行。

```cpp
ndn::Name
makeRepoServiceName(const ndn::Name& prefix, const std::string& operation);
```

### API-7ffb7e5cff4f · ndnsf_distributed_repo::toBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 17 行。

```cpp
std::vector<uint8_t>
toBytes(const std::string& text);
```

### API-7ed960bc9073 · ndnsf_distributed_repo::toString

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 20 行。

```cpp
std::string
toString(const std::vector<uint8_t>& bytes);
```

### API-be092e328907 · ndnsf_distributed_repo::encodeStoreRequest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 23 行。

```cpp
std::vector<uint8_t>
encodeStoreRequest(const RepoObjectManifest& manifest,
                   const std::vector<uint8_t>& payload);
```

### API-d3df6e956b2c · ndnsf_distributed_repo::encodeManifestRequest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 27 行。

```cpp
std::vector<uint8_t>
encodeManifestRequest(const RepoObjectManifest& manifest);
```

### API-2606214bcd0b · ndnsf_distributed_repo::encodeDataReferenceRequest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 30 行。

```cpp
std::vector<uint8_t>
encodeDataReferenceRequest(const RepoDataReference& reference);
```

### API-16aec586027f · ndnsf_distributed_repo::encodeStatusRequest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 33 行。

```cpp
std::vector<uint8_t>
encodeStatusRequest(const std::string& operationId);
```

### API-d7b129048041 · ndnsf_distributed_repo::encodeCatalogDeltaRequest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 36 行。

```cpp
std::vector<uint8_t>
encodeCatalogDeltaRequest(uint64_t sinceEpoch);
```

### API-a1abea30fd5c · ndnsf_distributed_repo::encodeCatalogLookupRequest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 39 行。

```cpp
std::vector<uint8_t>
encodeCatalogLookupRequest(const std::string& objectName);
```

### API-c435862740fd · ndnsf_distributed_repo::decodeStoreRequest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 42 行。

```cpp
void
decodeStoreRequest(const std::vector<uint8_t>& request,
                   RepoObjectManifest& manifest,
                   std::vector<uint8_t>& payload);
```

### API-08dbe2447706 · ndnsf_distributed_repo::parseDataReferenceJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 47 行。

```cpp
RepoDataReference
parseDataReferenceJson(const std::string& referenceJson);
```

### API-4e2a5e970a98 · ndnsf_distributed_repo::parseOperationStatusJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 50 行。

```cpp
RepoOperationStatus
parseOperationStatusJson(const std::string& statusJson);
```

### API-71081b19dd5b · ndnsf_distributed_repo::parseManifestJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 53 行。

```cpp
RepoObjectManifest
parseManifestJson(const std::string& manifestJson);
```

### API-3ea910007b7a · ndnsf_distributed_repo::parseCatalogEntryJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 56 行。

```cpp
RepoCatalogEntry
parseCatalogEntryJson(const std::string& entryJson);
```

### API-c653af9f1037 · ndnsf_distributed_repo::parseCatalogStatusJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 59 行。

```cpp
RepoCatalogStatus
parseCatalogStatusJson(const std::string& statusJson);
```

### API-fbf47299de04 · ndnsf_distributed_repo::parseCacheStatusJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 62 行。

```cpp
RepoCacheStatus
parseCacheStatusJson(const std::string& statusJson);
```

### API-d06e24b1e720 · ndnsf_distributed_repo::parseCatalogDeltaJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 65 行。

```cpp
RepoCatalogDelta
parseCatalogDeltaJson(const std::string& deltaJson);
```

### API-50eb1519bff1 · ndnsf_distributed_repo::parseInventoryJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 68 行。

```cpp
std::vector<RepoObjectManifest>
parseInventoryJson(const std::string& inventoryJson);
```

### API-8709ef578559 · ndnsf_distributed_repo::encodeInventory

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoProtocol.hpp`，第 71 行。

```cpp
std::string
encodeInventory(const std::vector<RepoObjectManifest>& manifests);
```

## NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp

源码 SHA-256：`4d0f58e40516d8eb547bc075903369ed4adc7a8a42c6493ab5bb021d7b9a9e66`。

### API-2739534c71db · ndnsf_distributed_repo::ArtifactByteRange

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 13 行。

```cpp
struct ArtifactByteRange
```

### API-27c71290c220 · ndnsf_distributed_repo::ArtifactByteRange::offsetBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 15 行。

```cpp
uint64_t offsetBytes = 0;
```

### API-a9dd38159ee8 · ndnsf_distributed_repo::ArtifactByteRange::lengthBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 16 行。

```cpp
uint64_t lengthBytes = 0;
```

### API-3112a1039756 · ndnsf_distributed_repo::ArtifactLifecycleState

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 19 行。

```cpp
enum class ArtifactLifecycleState
```

### API-e4c904f28c7f · ndnsf_distributed_repo::ArtifactLifecycleState::Absent

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 21 行。

```cpp
Absent
```

### API-3b567d654798 · ndnsf_distributed_repo::ArtifactLifecycleState::Reserved

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 22 行。

```cpp
Reserved
```

### API-3784afc4cb40 · ndnsf_distributed_repo::ArtifactLifecycleState::Receiving

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 23 行。

```cpp
Receiving
```

### API-a9e2bd02fa7e · ndnsf_distributed_repo::ArtifactLifecycleState::Verified

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 24 行。

```cpp
Verified
```

### API-62fe971218f0 · ndnsf_distributed_repo::ArtifactLifecycleState::Committed

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 25 行。

```cpp
Committed
```

### API-b2ac71e741b1 · ndnsf_distributed_repo::ArtifactLifecycleState::Active

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 26 行。

```cpp
Active
```

### API-c08718b15e80 · ndnsf_distributed_repo::ArtifactLifecycleState::Failed

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 27 行。

```cpp
Failed
```

### API-e66f6212b14e · ndnsf_distributed_repo::ArtifactLifecycleState::Expired

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 28 行。

```cpp
Expired
```

### API-bc9aa80da80c · ndnsf_distributed_repo::toString

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 31 行。

```cpp
std::string
toString(ArtifactLifecycleState state);
```

### API-8d8b54a86ea7 · ndnsf_distributed_repo::parseArtifactLifecycleState

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 34 行。

```cpp
ArtifactLifecycleState
parseArtifactLifecycleState(const std::string& value);
```

### API-4c2e1d4f5ac3 · ndnsf_distributed_repo::isAllowedArtifactTransition

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 37 行。

```cpp
bool
isAllowedArtifactTransition(ArtifactLifecycleState from,
                            ArtifactLifecycleState to) noexcept;
```

### API-5e3897277fa0 · ndnsf_distributed_repo::ArtifactLifecycleEvent

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 41 行。

```cpp
struct ArtifactLifecycleEvent
```

### API-68e87c7083c3 · ndnsf_distributed_repo::ArtifactLifecycleEvent::eventId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 43 行。

```cpp
std::string eventId;
```

### API-be7ec0082198 · ndnsf_distributed_repo::ArtifactLifecycleEvent::operationId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 44 行。

```cpp
std::string operationId;
```

### API-23a20035eaa9 · ndnsf_distributed_repo::ArtifactLifecycleEvent::artifactDigest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 45 行。

```cpp
std::string artifactDigest;
```

### API-46934b933055 · ndnsf_distributed_repo::ArtifactLifecycleEvent::generation

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 46 行。

```cpp
uint64_t generation = 0;
```

### API-9f98caee98eb · ndnsf_distributed_repo::ArtifactLifecycleEvent::fromState

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 47 行。

```cpp
ArtifactLifecycleState fromState = ArtifactLifecycleState::Absent;
```

### API-8e2f2e926d2b · ndnsf_distributed_repo::ArtifactLifecycleEvent::toState

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 48 行。

```cpp
ArtifactLifecycleState toState = ArtifactLifecycleState::Absent;
```

### API-be3cac70d5dd · ndnsf_distributed_repo::ArtifactLifecycleEvent::eventTimeMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 49 行。

```cpp
uint64_t eventTimeMs = 0;
```

### API-0b7e38892658 · ndnsf_distributed_repo::ArtifactLifecycleEvent::accepted

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 50 行。

```cpp
bool accepted = true;
```

### API-50879b06a8e3 · ndnsf_distributed_repo::ArtifactLifecycleEvent::detail

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 51 行。

```cpp
std::string detail;
```

### API-4d10a39b73ef · ndnsf_distributed_repo::PayloadStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 54 行。

```cpp
class PayloadStore
```

### API-ada2a3760428 · ndnsf_distributed_repo::PayloadStore::~PayloadStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 57 行。

```cpp
virtual ~PayloadStore() = default;
```

### API-1d45cf4fb67c · ndnsf_distributed_repo::PayloadStore::begin

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 59 行。

```cpp
virtual void begin(const ArtifactReference& artifact, uint64_t generation) = 0;
```

### API-eb4fc63509dc · ndnsf_distributed_repo::PayloadStore::writeRange

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 60 行。

```cpp
virtual void writeRange(const ArtifactReference& artifact, uint64_t generation,
                          ArtifactByteRange range,
                          const std::vector<uint8_t>& bytes) = 0;
```

### API-865d12bc97a6 · ndnsf_distributed_repo::PayloadStore::readRange

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 63 行。

```cpp
virtual std::vector<uint8_t> readRange(const ArtifactReference& artifact,
                                         uint64_t generation,
                                         ArtifactByteRange range) const = 0;
```

### API-c83b5f3d5937 · ndnsf_distributed_repo::PayloadStore::markVerified

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 66 行。

```cpp
virtual void markVerified(const ArtifactReference& artifact, uint64_t generation,
                            ArtifactByteRange range) = 0;
```

### API-51b33ce43880 · ndnsf_distributed_repo::PayloadStore::verifiedRanges

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 68 行。

```cpp
virtual std::vector<ArtifactByteRange>
  verifiedRanges(const ArtifactReference& artifact, uint64_t generation) const = 0;
```

### API-eb8ca8d22940 · ndnsf_distributed_repo::PayloadStore::flush

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 70 行。

```cpp
virtual void flush(const ArtifactReference& artifact, uint64_t generation) = 0;
```

### API-1e3bc0f8eab2 · ndnsf_distributed_repo::PayloadStore::finalize

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 71 行。

```cpp
virtual void finalize(const ArtifactReference& artifact, uint64_t generation) = 0;
```

### API-7c142f81fcb4 · ndnsf_distributed_repo::PayloadStore::isCommitted

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 72 行。

```cpp
virtual bool isCommitted(const ArtifactReference& artifact,
                           uint64_t generation) const = 0;
```

### API-ec3c831abfec · ndnsf_distributed_repo::PayloadStore::abort

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 74 行。

```cpp
virtual void abort(const ArtifactReference& artifact, uint64_t generation) = 0;
```

### API-0d40cda3c37e · ndnsf_distributed_repo::MetadataStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 77 行。

```cpp
class MetadataStore
```

### API-23acdac927d6 · ndnsf_distributed_repo::MetadataStore::~MetadataStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 80 行。

```cpp
virtual ~MetadataStore() = default;
```

### API-40137b632f71 · ndnsf_distributed_repo::MetadataStore::schemaGeneration

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 82 行。

```cpp
virtual uint64_t schemaGeneration() const = 0;
```

### API-1a69d7d03fe4 · ndnsf_distributed_repo::MetadataStore::appendLifecycleEvent

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 83 行。

```cpp
virtual void appendLifecycleEvent(const ArtifactLifecycleEvent& event) = 0;
```

### API-32b37f531f68 · ndnsf_distributed_repo::MetadataStore::lifecycleEvents

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 84 行。

```cpp
virtual std::vector<ArtifactLifecycleEvent>
  lifecycleEvents(const std::string& operationId) const = 0;
```

### API-cd8e6fd80252 · ndnsf_distributed_repo::MetadataStore::currentLifecycleState

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 86 行。

```cpp
virtual ArtifactLifecycleState
  currentLifecycleState(const std::string& operationId) const = 0;
```

### API-f5c58babad10 · ndnsf_distributed_repo::BackendOwnershipLease

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 90 行。

```cpp
class BackendOwnershipLease
```

### API-9b252e0170dd · ndnsf_distributed_repo::BackendOwnershipLease::BackendOwnershipLease

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 93 行。

```cpp
BackendOwnershipLease(std::string backendPath, std::string ownerId);
```

### API-21728e24575e · ndnsf_distributed_repo::BackendOwnershipLease::~BackendOwnershipLease

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 94 行。

```cpp
~BackendOwnershipLease();
```

### API-5e8fbcbe86a1 · ndnsf_distributed_repo::BackendOwnershipLease::BackendOwnershipLease

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 96 行。

```cpp
BackendOwnershipLease(const BackendOwnershipLease&) = delete;
```

### API-e9fe6b454324 · ndnsf_distributed_repo::BackendOwnershipLease::operator=

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 97 行。

```cpp
BackendOwnershipLease& operator=(const BackendOwnershipLease&) = delete;
```

### API-137ab587df29 · ndnsf_distributed_repo::BackendOwnershipLease::BackendOwnershipLease

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 99 行。

```cpp
BackendOwnershipLease(BackendOwnershipLease&& other) noexcept;
```

### API-06f24589df50 · ndnsf_distributed_repo::BackendOwnershipLease::operator=

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 100 行。

```cpp
BackendOwnershipLease& operator=(BackendOwnershipLease&& other) noexcept;
```

### API-fee23f1071ce · ndnsf_distributed_repo::BackendOwnershipLease::ownerId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 102 行。

```cpp
const std::string& ownerId() const noexcept;
```

### API-a46eefacc93e · ndnsf_distributed_repo::BackendOwnershipLease::lockPath

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 103 行。

```cpp
const std::string& lockPath() const noexcept;
```

### API-a0446c4754a3 · ndnsf_distributed_repo::BackendOwnershipLease::ownsBackend

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 104 行。

```cpp
bool ownsBackend() const noexcept;
```

### API-7ac8bd82e00d · ndnsf_distributed_repo::RepositoryStoreFacade

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 115 行。

```cpp
class RepositoryStoreFacade
```

### API-b9b767048802 · ndnsf_distributed_repo::RepositoryStoreFacade::RepositoryStoreFacade

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 118 行。

```cpp
RepositoryStoreFacade(std::string backendPath, std::string ownerId,
                        std::shared_ptr<PayloadStore> payloadStore,
                        std::shared_ptr<MetadataStore> metadataStore);
```

### API-389941d71669 · ndnsf_distributed_repo::RepositoryStoreFacade::payload

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 122 行。

```cpp
PayloadStore& payload();
```

### API-93726ea6cb6a · ndnsf_distributed_repo::RepositoryStoreFacade::metadata

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 123 行。

```cpp
MetadataStore& metadata();
```

### API-955123cc53db · ndnsf_distributed_repo::RepositoryStoreFacade::ownership

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 124 行。

```cpp
const BackendOwnershipLease& ownership() const noexcept;
```

### API-859bb211f378 · ndnsf_distributed_repo::RepositoryStoreFacade::transition

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoStoreBackend.hpp`，第 126 行。

```cpp
ArtifactLifecycleEvent transition(ArtifactLifecycleEvent event);
```

## NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp

源码 SHA-256：`5c794bdc744abcc73b2f3c5ee5472d8726d4e6d24139a4e2cac949979a2b8abc`。

### API-321fb3efa48d · ndnsf_distributed_repo::reason::* OperationConflict = "repo-operation-conflict"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 14 行。

```cpp
inline constexpr const char* OperationConflict = "repo-operation-conflict";
```

### API-f7c948cc54af · ndnsf_distributed_repo::reason::* GenerationConflict = "repo-generation-conflict"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 15 行。

```cpp
inline constexpr const char* GenerationConflict = "repo-generation-conflict";
```

### API-d184b1dbcd14 · ndnsf_distributed_repo::reason::* WriteIncomplete = "repo-write-incomplete"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 16 行。

```cpp
inline constexpr const char* WriteIncomplete = "repo-write-incomplete";
```

### API-6d7be4690d00 · ndnsf_distributed_repo::reason::* Overloaded = "repo-overloaded"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 17 行。

```cpp
inline constexpr const char* Overloaded = "repo-overloaded";
```

### API-8a51a989ac71 · ndnsf_distributed_repo::reason::* CapacityReserved = "repo-capacity-reserved"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 18 行。

```cpp
inline constexpr const char* CapacityReserved = "repo-capacity-reserved";
```

### API-b4b6f107fc26 · ndnsf_distributed_repo::reason::* IntegrityFailure = "repo-integrity-failure"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 19 行。

```cpp
inline constexpr const char* IntegrityFailure = "repo-integrity-failure";
```

### API-abfd276555ad · ndnsf_distributed_repo::reason::* RepairUnavailable = "repo-repair-unavailable"

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 20 行。

```cpp
inline constexpr const char* RepairUnavailable = "repo-repair-unavailable";
```

### API-72e4ab253415 · ndnsf_distributed_repo::RepoDeploymentMode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 23 行。

```cpp
enum class RepoDeploymentMode
```

### API-69937e03c7b8 · ndnsf_distributed_repo::RepoDeploymentMode::Remote

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 25 行。

```cpp
Remote
```

### API-a52cfb31c144 · ndnsf_distributed_repo::RepoDeploymentMode::Embedded

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 26 行。

```cpp
Embedded
```

### API-75e48e337f13 · ndnsf_distributed_repo::RepoDeploymentMode::Both

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 27 行。

```cpp
Both
```

### API-fd3c891bc57c · ndnsf_distributed_repo::RepoWriteConsistency

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 30 行。

```cpp
enum class RepoWriteConsistency
```

### API-3d0a74e15163 · ndnsf_distributed_repo::RepoWriteConsistency::One

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 32 行。

```cpp
One
```

### API-d920e56ff274 · ndnsf_distributed_repo::RepoWriteConsistency::Quorum

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 33 行。

```cpp
Quorum
```

### API-d99c1a159970 · ndnsf_distributed_repo::RepoWriteConsistency::All

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 34 行。

```cpp
All
```

### API-676cd88d0b72 · ndnsf_distributed_repo::toString

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 37 行。

```cpp
std::string
toString(RepoWriteConsistency consistency);
```

### API-e75c65cf0c71 · ndnsf_distributed_repo::parseRepoWriteConsistency

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 40 行。

```cpp
RepoWriteConsistency
parseRepoWriteConsistency(const std::string& value);
```

### API-135d3d0552b0 · ndnsf_distributed_repo::requiredWriteAcks

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 43 行。

```cpp
uint32_t
requiredWriteAcks(uint32_t replicationFactor, RepoWriteConsistency consistency);
```

### API-e7e34142e1dd · ndnsf_distributed_repo::normalizeRepoOperationState

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 46 行。

```cpp
std::string
normalizeRepoOperationState(const std::string& value);
```

### API-dc946602256c · ndnsf_distributed_repo::RepoObjectManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 49 行。

```cpp
struct RepoObjectManifest
```

### API-f2dc25310539 · ndnsf_distributed_repo::RepoObjectManifest::objectName

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 51 行。

```cpp
std::string objectName;
```

### API-9831cb42de30 · ndnsf_distributed_repo::RepoObjectManifest::objectType

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 52 行。

```cpp
std::string objectType = "artifact";
```

### API-7a60b19a9144 · ndnsf_distributed_repo::RepoObjectManifest::sha256

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 53 行。

```cpp
std::string sha256;
```

### API-348a4535f766 · ndnsf_distributed_repo::RepoObjectManifest::size

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 54 行。

```cpp
uint64_t size = 0;
```

### API-4acfbda1d95e · ndnsf_distributed_repo::RepoObjectManifest::segmentCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 55 行。

```cpp
uint32_t segmentCount = 1;
```

### API-a41528de743f · ndnsf_distributed_repo::RepoObjectManifest::replicationFactor

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 56 行。

```cpp
uint32_t replicationFactor = 1;
```

### API-acdedbacc484 · ndnsf_distributed_repo::RepoObjectManifest::replicaNodes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 57 行。

```cpp
std::vector<std::string> replicaNodes;
```

### API-f049cb7e9086 · ndnsf_distributed_repo::RepoObjectManifest::packetNames

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 60 行。

```cpp
std::vector<std::string> packetNames;
```

原始接口说明：

```text
// Ordered original Data names for packet-backed objects. Packet wire bytes
// are stored under these exact names, never under Repo-generated aliases.
```

### API-1569f7b6ca6d · ndnsf_distributed_repo::RepoObjectManifest::policyEpoch

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 61 行。

```cpp
std::string policyEpoch;
```

### API-a833fed88a5f · ndnsf_distributed_repo::RepoObjectManifest::generation

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 62 行。

```cpp
uint64_t generation = 0;
```

### API-79d7926f4dab · ndnsf_distributed_repo::RepoObjectManifest::parentGeneration

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 63 行。

```cpp
int64_t parentGeneration = -1;
```

### API-15648caffe79 · ndnsf_distributed_repo::RepoObjectManifest::writeConsistency

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 64 行。

```cpp
std::string writeConsistency = "ALL";
```

### API-5fc8a29fc6c2 · ndnsf_distributed_repo::RepoObjectManifest::requiredWriteAcks

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 65 行。

```cpp
uint32_t requiredWriteAcks = 0;
```

### API-a6de00a37ba1 · ndnsf_distributed_repo::RepoObjectManifest::confirmedReplicaNodes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 66 行。

```cpp
std::vector<std::string> confirmedReplicaNodes;
```

### API-e596a7ed3f33 · ndnsf_distributed_repo::RepoObjectManifest::operationId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 67 行。

```cpp
std::string operationId;
```

### API-c753669abcdb · ndnsf_distributed_repo::RepoObjectManifest::lifecycleState

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 68 行。

```cpp
std::string lifecycleState = "COMMITTED";
```

### API-3d49ffac0813 · ndnsf_distributed_repo::RepoObjectManifest::toJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 70 行。

```cpp
std::string toJson() const;
```

### API-3f6b7d7f5d0e · ndnsf_distributed_repo::RepoWriteIntent

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 73 行。

```cpp
struct RepoWriteIntent
```

### API-358ae227cff4 · ndnsf_distributed_repo::RepoWriteIntent::operationId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 75 行。

```cpp
std::string operationId;
```

### API-4d08b3feddf6 · ndnsf_distributed_repo::RepoWriteIntent::objectName

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 76 行。

```cpp
std::string objectName;
```

### API-4470a2546488 · ndnsf_distributed_repo::RepoWriteIntent::generation

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 77 行。

```cpp
uint64_t generation = 0;
```

### API-3c50b7b3aa62 · ndnsf_distributed_repo::RepoWriteIntent::expectedGeneration

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 78 行。

```cpp
int64_t expectedGeneration = -1;
```

### API-00617132940c · ndnsf_distributed_repo::RepoWriteIntent::digest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 79 行。

```cpp
std::string digest;
```

### API-8906f170b380 · ndnsf_distributed_repo::RepoWriteIntent::replicationFactor

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 80 行。

```cpp
uint32_t replicationFactor = 1;
```

### API-d3060f207eca · ndnsf_distributed_repo::RepoWriteIntent::requiredAcks

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 81 行。

```cpp
uint32_t requiredAcks = 1;
```

### API-f911f3beec97 · ndnsf_distributed_repo::RepoWriteIntent::consistency

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 82 行。

```cpp
std::string consistency = "ALL";
```

### API-07f08c8879b2 · ndnsf_distributed_repo::RepoWriteIntent::selectedReplicas

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 83 行。

```cpp
std::vector<std::string> selectedReplicas;
```

### API-f92efbb5d6f0 · ndnsf_distributed_repo::RepoWriteIntent::state

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 84 行。

```cpp
std::string state = "RECEIVED";
```

### API-6c8c7402407a · ndnsf_distributed_repo::RepoWriteIntent::createdAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 85 行。

```cpp
uint64_t createdAtMs = 0;
```

### API-a857ad316755 · ndnsf_distributed_repo::RepoWriteIntent::updatedAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 86 行。

```cpp
uint64_t updatedAtMs = 0;
```

### API-b2b7f6345aa0 · ndnsf_distributed_repo::RepoWriteIntent::toJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 88 行。

```cpp
std::string toJson() const;
```

### API-16ef9fff8266 · ndnsf_distributed_repo::RepoWriteReceipt

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 91 行。

```cpp
struct RepoWriteReceipt
```

### API-24062be77a45 · ndnsf_distributed_repo::RepoWriteReceipt::operationId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 93 行。

```cpp
std::string operationId;
```

### API-a3ed11e5bbdf · ndnsf_distributed_repo::RepoWriteReceipt::repoNode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 94 行。

```cpp
std::string repoNode;
```

### API-5943d155ed6a · ndnsf_distributed_repo::RepoWriteReceipt::objectName

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 95 行。

```cpp
std::string objectName;
```

### API-8e01c80ebd8c · ndnsf_distributed_repo::RepoWriteReceipt::generation

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 96 行。

```cpp
uint64_t generation = 0;
```

### API-65ecbce94ccf · ndnsf_distributed_repo::RepoWriteReceipt::digest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 97 行。

```cpp
std::string digest;
```

### API-f299d6daae9a · ndnsf_distributed_repo::RepoWriteReceipt::persistedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 98 行。

```cpp
uint64_t persistedBytes = 0;
```

### API-239f19d81580 · ndnsf_distributed_repo::RepoWriteReceipt::state

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 99 行。

```cpp
std::string state = "COMMITTED";
```

### API-2a029f9fdc20 · ndnsf_distributed_repo::RepoWriteReceipt::completedAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 100 行。

```cpp
uint64_t completedAtMs = 0;
```

### API-794bba41abfc · ndnsf_distributed_repo::RepoWriteReceipt::toJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 102 行。

```cpp
std::string toJson() const;
```

### API-bde3cf3318ea · ndnsf_distributed_repo::RepoCapacityReservation

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 105 行。

```cpp
struct RepoCapacityReservation
```

### API-3a52432f3fde · ndnsf_distributed_repo::RepoCapacityReservation::reservationId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 107 行。

```cpp
std::string reservationId;
```

### API-d85426bd2db6 · ndnsf_distributed_repo::RepoCapacityReservation::operationId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 108 行。

```cpp
std::string operationId;
```

### API-cb6df33fd09b · ndnsf_distributed_repo::RepoCapacityReservation::reservedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 109 行。

```cpp
uint64_t reservedBytes = 0;
```

### API-f864a10d0991 · ndnsf_distributed_repo::RepoCapacityReservation::state

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 110 行。

```cpp
std::string state = "ACTIVE";
```

### API-590f4603c232 · ndnsf_distributed_repo::RepoCapacityReservation::expiresAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 111 行。

```cpp
uint64_t expiresAtMs = 0;
```

### API-3cd56731152f · ndnsf_distributed_repo::RepoCapacityReservation::toJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 113 行。

```cpp
std::string toJson() const;
```

### API-6e62625063ae · ndnsf_distributed_repo::RepoDataReference

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 116 行。

```cpp
struct RepoDataReference
```

### API-6cd2951edcea · ndnsf_distributed_repo::RepoDataReference::objectName

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 118 行。

```cpp
std::string objectName;
```

### API-6b526809101e · ndnsf_distributed_repo::RepoDataReference::dataPrefix

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 119 行。

```cpp
std::string dataPrefix;
```

### API-bf646dcf1f2f · ndnsf_distributed_repo::RepoDataReference::firstSegment

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 120 行。

```cpp
uint64_t firstSegment = 0;
```

### API-753855b43bb4 · ndnsf_distributed_repo::RepoDataReference::finalSegment

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 121 行。

```cpp
uint64_t finalSegment = 0;
```

### API-0170d8043b5f · ndnsf_distributed_repo::RepoDataReference::hasFinalSegment

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 122 行。

```cpp
bool hasFinalSegment = false;
```

### API-5c23994cc7d2 · ndnsf_distributed_repo::RepoDataReference::forwardingHint

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 123 行。

```cpp
std::string forwardingHint;
```

### API-4fcd70f7f39e · ndnsf_distributed_repo::RepoDataReference::expectedSha256

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 124 行。

```cpp
std::string expectedSha256;
```

### API-bd165f7e4314 · ndnsf_distributed_repo::RepoDataReference::expectedSize

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 125 行。

```cpp
uint64_t expectedSize = 0;
```

### API-368bb54bc14d · ndnsf_distributed_repo::RepoDataReference::storeWirePackets

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 126 行。

```cpp
bool storeWirePackets = true;
```

### API-dfd9e1a0ed3f · ndnsf_distributed_repo::RepoDataReference::objectType

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 127 行。

```cpp
std::string objectType = "ndn-segmented-data";
```

### API-17d7fcd4f482 · ndnsf_distributed_repo::RepoDataReference::toJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 129 行。

```cpp
std::string toJson() const;
```

### API-fac832596964 · ndnsf_distributed_repo::RepoOperationStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 132 行。

```cpp
struct RepoOperationStatus
```

### API-9e2fd5d41580 · ndnsf_distributed_repo::RepoOperationStatus::operationId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 134 行。

```cpp
std::string operationId;
```

### API-49f8417fa9aa · ndnsf_distributed_repo::RepoOperationStatus::operation

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 135 行。

```cpp
std::string operation;
```

### API-eb251c3546b7 · ndnsf_distributed_repo::RepoOperationStatus::state

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 136 行。

```cpp
std::string state = "RECEIVED";
```

### API-8c265d25bcc4 · ndnsf_distributed_repo::RepoOperationStatus::objectName

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 137 行。

```cpp
std::string objectName;
```

### API-db36c9a63025 · ndnsf_distributed_repo::RepoOperationStatus::message

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 138 行。

```cpp
std::string message;
```

### API-ee40f8da6ede · ndnsf_distributed_repo::RepoOperationStatus::completedSegments

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 139 行。

```cpp
uint64_t completedSegments = 0;
```

### API-83bdb948716d · ndnsf_distributed_repo::RepoOperationStatus::totalSegments

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 140 行。

```cpp
uint64_t totalSegments = 0;
```

### API-32f772105186 · ndnsf_distributed_repo::RepoOperationStatus::createdAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 141 行。

```cpp
uint64_t createdAtMs = 0;
```

### API-be902a2d1043 · ndnsf_distributed_repo::RepoOperationStatus::updatedAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 142 行。

```cpp
uint64_t updatedAtMs = 0;
```

### API-5066de68e2e3 · ndnsf_distributed_repo::RepoOperationStatus::expiresAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 143 行。

```cpp
uint64_t expiresAtMs = 0;
```

### API-dc294f2eb06a · ndnsf_distributed_repo::RepoOperationStatus::toJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 145 行。

```cpp
std::string toJson() const;
```

### API-598dd9b84911 · ndnsf_distributed_repo::RepoOperationMetrics

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 157 行。

```cpp
struct RepoOperationMetrics
```

### API-1a829938a2ad · ndnsf_distributed_repo::RepoOperationMetrics::MAX_OPERATION_ID_BYTES

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 159 行。

```cpp
static constexpr size_t MAX_OPERATION_ID_BYTES = 256;
```

### API-1d49f9cf1404 · ndnsf_distributed_repo::RepoOperationMetrics::operationId

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 161 行。

```cpp
std::string operationId;
```

### API-a1d27a0bb698 · ndnsf_distributed_repo::RepoOperationMetrics::startedAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 162 行。

```cpp
uint64_t startedAtMs = 0;
```

### API-a7c3e880bdbb · ndnsf_distributed_repo::RepoOperationMetrics::completedAtMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 163 行。

```cpp
uint64_t completedAtMs = 0;
```

### API-7f1d414f4667 · ndnsf_distributed_repo::RepoOperationMetrics::phaseTimingsMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 164 行。

```cpp
std::map<std::string, double> phaseTimingsMs;
```

### API-487e53892892 · ndnsf_distributed_repo::RepoOperationMetrics::logicalPayloadBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 166 行。

```cpp
uint64_t logicalPayloadBytes = 0;
```

### API-914dc4db03ae · ndnsf_distributed_repo::RepoOperationMetrics::dataWireBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 167 行。

```cpp
uint64_t dataWireBytes = 0;
```

### API-365b639f3adb · ndnsf_distributed_repo::RepoOperationMetrics::interestWireBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 168 行。

```cpp
uint64_t interestWireBytes = 0;
```

### API-59351d95af6b · ndnsf_distributed_repo::RepoOperationMetrics::wireBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 169 行。

```cpp
uint64_t wireBytes = 0;
```

### API-bf6e7f340273 · ndnsf_distributed_repo::RepoOperationMetrics::retransmittedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 170 行。

```cpp
uint64_t retransmittedBytes = 0;
```

### API-42a3e2674de0 · ndnsf_distributed_repo::RepoOperationMetrics::payloadStoreBytesRead

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 171 行。

```cpp
uint64_t payloadStoreBytesRead = 0;
```

### API-f3c25ada0b3f · ndnsf_distributed_repo::RepoOperationMetrics::payloadStoreBytesWritten

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 172 行。

```cpp
uint64_t payloadStoreBytesWritten = 0;
```

### API-67ebc96f8536 · ndnsf_distributed_repo::RepoOperationMetrics::metadataStoreBytesRead

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 173 行。

```cpp
uint64_t metadataStoreBytesRead = 0;
```

### API-6a6a747409d5 · ndnsf_distributed_repo::RepoOperationMetrics::metadataStoreBytesWritten

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 174 行。

```cpp
uint64_t metadataStoreBytesWritten = 0;
```

### API-078426d79d43 · ndnsf_distributed_repo::RepoOperationMetrics::storageBytesRead

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 175 行。

```cpp
uint64_t storageBytesRead = 0;
```

### API-867a2f07db46 · ndnsf_distributed_repo::RepoOperationMetrics::storageBytesWritten

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 176 行。

```cpp
uint64_t storageBytesWritten = 0;
```

### API-4132aa9a0665 · ndnsf_distributed_repo::RepoOperationMetrics::asymmetricVerifications

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 178 行。

```cpp
uint64_t asymmetricVerifications = 0;
```

### API-a59e11e501e3 · ndnsf_distributed_repo::RepoOperationMetrics::digestVerifications

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 179 行。

```cpp
uint64_t digestVerifications = 0;
```

### API-0ac791e3e009 · ndnsf_distributed_repo::RepoOperationMetrics::asymmetricVerificationMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 180 行。

```cpp
double asymmetricVerificationMs = 0.0;
```

### API-2c24c0cf2a1f · ndnsf_distributed_repo::RepoOperationMetrics::digestVerificationMs

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 181 行。

```cpp
double digestVerificationMs = 0.0;
```

### API-00dbc6ae9ad9 · ndnsf_distributed_repo::RepoOperationMetrics::controlOperations

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 183 行。

```cpp
uint64_t controlOperations = 0;
```

### API-51e5f02d8922 · ndnsf_distributed_repo::RepoOperationMetrics::metadataOperations

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 184 行。

```cpp
uint64_t metadataOperations = 0;
```

### API-a07a3453ce80 · ndnsf_distributed_repo::RepoOperationMetrics::metadataRecordCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 185 行。

```cpp
uint64_t metadataRecordCount = 0;
```

### API-c55085b3d3cb · ndnsf_distributed_repo::RepoOperationMetrics::requestedReplicaCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 187 行。

```cpp
uint32_t requestedReplicaCount = 0;
```

### API-2f7e1f06f53f · ndnsf_distributed_repo::RepoOperationMetrics::selectedReplicaCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 188 行。

```cpp
uint32_t selectedReplicaCount = 0;
```

### API-2ebb9f943c27 · ndnsf_distributed_repo::RepoOperationMetrics::committedReplicaCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 189 行。

```cpp
uint32_t committedReplicaCount = 0;
```

### API-a8e104235004 · ndnsf_distributed_repo::RepoOperationMetrics::rejectedReplicaReceiptCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 190 行。

```cpp
uint32_t rejectedReplicaReceiptCount = 0;
```

### API-d54412e66d9f · ndnsf_distributed_repo::RepoOperationMetrics::isCanonicalPhase

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 192 行。

```cpp
static bool isCanonicalPhase(const std::string& phase);
```

### API-c01b0c62ffb1 · ndnsf_distributed_repo::RepoOperationMetrics::validate

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 193 行。

```cpp
void validate() const;
```

### API-1ad6824908a8 · ndnsf_distributed_repo::RepoOperationMetrics::toJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 194 行。

```cpp
std::string toJson() const;
```

### API-e309b75b5c0a · ndnsf_distributed_repo::StorageCapability

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 197 行。

```cpp
struct StorageCapability
```

### API-ba50fad75c4c · ndnsf_distributed_repo::StorageCapability::repoNode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 199 行。

```cpp
std::string repoNode;
```

### API-9f4781ee3684 · ndnsf_distributed_repo::StorageCapability::freeBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 200 行。

```cpp
uint64_t freeBytes = 0;
```

### API-effdf3533d21 · ndnsf_distributed_repo::StorageCapability::usedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 201 行。

```cpp
uint64_t usedBytes = 0;
```

### API-849f3fd88159 · ndnsf_distributed_repo::StorageCapability::recentLoad

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 202 行。

```cpp
double recentLoad = 0.0;
```

### API-166eee4a150e · ndnsf_distributed_repo::StorageCapability::availabilityScore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 203 行。

```cpp
double availabilityScore = 1.0;
```

### API-94f38e2899af · ndnsf_distributed_repo::StorageCapability::failureDomain

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 204 行。

```cpp
std::string failureDomain;
```

### API-b4ae621c33c9 · ndnsf_distributed_repo::StorageCapability::storageClasses

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 205 行。

```cpp
std::vector<std::string> storageClasses;
```

### API-fd77d428c5a2 · ndnsf_distributed_repo::StorageCapability::repoMode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 206 行。

```cpp
std::string repoMode = "persistent";
```

### API-5e0c664cbd56 · ndnsf_distributed_repo::StorageCapability::acceptsBackupReplica

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 207 行。

```cpp
bool acceptsBackupReplica = true;
```

### API-0000061e134a · ndnsf_distributed_repo::StorageCapability::toJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 209 行。

```cpp
std::string toJson() const;
```

### API-b8a4b2377a97 · ndnsf_distributed_repo::RepoCatalogEntry

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 212 行。

```cpp
struct RepoCatalogEntry
```

### API-fce6821ee321 · ndnsf_distributed_repo::RepoCatalogEntry::manifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 214 行。

```cpp
RepoObjectManifest manifest;
```

### API-bc562951713b · ndnsf_distributed_repo::RepoCatalogEntry::sourceRepo

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 215 行。

```cpp
std::string sourceRepo;
```

### API-53700a791569 · ndnsf_distributed_repo::RepoCatalogEntry::repoMode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 216 行。

```cpp
std::string repoMode = "persistent";
```

### API-e7c479aa7cda · ndnsf_distributed_repo::RepoCatalogEntry::state

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 217 行。

```cpp
std::string state = "AVAILABLE";
```

### API-57364b362c96 · ndnsf_distributed_repo::RepoCatalogEntry::catalogEpoch

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 218 行。

```cpp
uint64_t catalogEpoch = 0;
```

### API-8af264400f2d · ndnsf_distributed_repo::RepoCatalogEntry::toJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 220 行。

```cpp
std::string toJson() const;
```

### API-6cc44f774c08 · ndnsf_distributed_repo::RepoCatalogStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 223 行。

```cpp
struct RepoCatalogStatus
```

### API-1432b04967ad · ndnsf_distributed_repo::RepoCatalogStatus::repoNode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 225 行。

```cpp
std::string repoNode;
```

### API-2b53ea74670d · ndnsf_distributed_repo::RepoCatalogStatus::repoMode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 226 行。

```cpp
std::string repoMode = "persistent";
```

### API-05c7ff350de6 · ndnsf_distributed_repo::RepoCatalogStatus::catalogEpoch

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 227 行。

```cpp
uint64_t catalogEpoch = 0;
```

### API-f9ce99585437 · ndnsf_distributed_repo::RepoCatalogStatus::objectCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 228 行。

```cpp
uint64_t objectCount = 0;
```

### API-0e86599af2f8 · ndnsf_distributed_repo::RepoCatalogStatus::acceptsBackupReplica

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 229 行。

```cpp
bool acceptsBackupReplica = true;
```

### API-597f97961e38 · ndnsf_distributed_repo::RepoCatalogStatus::toJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 231 行。

```cpp
std::string toJson() const;
```

### API-ce2a6aefcfec · ndnsf_distributed_repo::RepoCatalogDelta

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 234 行。

```cpp
struct RepoCatalogDelta
```

### API-315a0b237158 · ndnsf_distributed_repo::RepoCatalogDelta::repoNode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 236 行。

```cpp
std::string repoNode;
```

### API-d5ab27905ef6 · ndnsf_distributed_repo::RepoCatalogDelta::repoMode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 237 行。

```cpp
std::string repoMode = "persistent";
```

### API-bbb71bb51f65 · ndnsf_distributed_repo::RepoCatalogDelta::sinceEpoch

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 238 行。

```cpp
uint64_t sinceEpoch = 0;
```

### API-c6312b221e8c · ndnsf_distributed_repo::RepoCatalogDelta::catalogEpoch

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 239 行。

```cpp
uint64_t catalogEpoch = 0;
```

### API-9f2aafd4c1c4 · ndnsf_distributed_repo::RepoCatalogDelta::entries

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 240 行。

```cpp
std::vector<RepoCatalogEntry> entries;
```

### API-f7ca2babffcc · ndnsf_distributed_repo::RepoCatalogDelta::toJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 242 行。

```cpp
std::string toJson() const;
```

### API-3099c8329dd3 · ndnsf_distributed_repo::RepoCacheStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 245 行。

```cpp
struct RepoCacheStatus
```

### API-67ff1666fe74 · ndnsf_distributed_repo::RepoCacheStatus::storageBackend

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 247 行。

```cpp
std::string storageBackend = "unknown";
```

### API-7a7333a9df95 · ndnsf_distributed_repo::RepoCacheStatus::authoritativeBackend

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 248 行。

```cpp
std::string authoritativeBackend = "unknown";
```

### API-09aacc6ff8ee · ndnsf_distributed_repo::RepoCacheStatus::cachePolicy

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 249 行。

```cpp
std::string cachePolicy = "disabled";
```

### API-05fd56a4ffc1 · ndnsf_distributed_repo::RepoCacheStatus::budgetBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 250 行。

```cpp
uint64_t budgetBytes = 0;
```

### API-48d78cda1614 · ndnsf_distributed_repo::RepoCacheStatus::usedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 251 行。

```cpp
uint64_t usedBytes = 0;
```

### API-a44270bda3d1 · ndnsf_distributed_repo::RepoCacheStatus::entryCount

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 252 行。

```cpp
uint64_t entryCount = 0;
```

### API-f8bd5a017ad7 · ndnsf_distributed_repo::RepoCacheStatus::hits

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 253 行。

```cpp
uint64_t hits = 0;
```

### API-48e1eefa7fdb · ndnsf_distributed_repo::RepoCacheStatus::misses

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 254 行。

```cpp
uint64_t misses = 0;
```

### API-4a3fe7ba2bd7 · ndnsf_distributed_repo::RepoCacheStatus::admissions

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 255 行。

```cpp
uint64_t admissions = 0;
```

### API-be574aa9398f · ndnsf_distributed_repo::RepoCacheStatus::evictions

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 256 行。

```cpp
uint64_t evictions = 0;
```

### API-26ef87b5f27f · ndnsf_distributed_repo::RepoCacheStatus::invalidations

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 257 行。

```cpp
uint64_t invalidations = 0;
```

### API-def1c68c71a1 · ndnsf_distributed_repo::RepoCacheStatus::oversizedBypasses

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 258 行。

```cpp
uint64_t oversizedBypasses = 0;
```

### API-b4cb8d9152da · ndnsf_distributed_repo::RepoCacheStatus::backingReads

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 259 行。

```cpp
uint64_t backingReads = 0;
```

### API-1c9a5362ea99 · ndnsf_distributed_repo::RepoCacheStatus::backingWrites

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 260 行。

```cpp
uint64_t backingWrites = 0;
```

### API-ff108d2eec80 · ndnsf_distributed_repo::RepoCacheStatus::toJson

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 262 行。

```cpp
std::string toJson() const;
```

### API-ef5fd1b6a719 · ndnsf_distributed_repo::PlacementPolicy

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 265 行。

```cpp
struct PlacementPolicy
```

### API-dcfd42d581a3 · ndnsf_distributed_repo::PlacementPolicy::replicationFactor

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 267 行。

```cpp
uint32_t replicationFactor = 1;
```

### API-7a891349395f · ndnsf_distributed_repo::PlacementPolicy::avoidSameFailureDomain

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 268 行。

```cpp
bool avoidSameFailureDomain = true;
```

### API-ff4eb0f14d2c · ndnsf_distributed_repo::PlacementPolicy::preferLowLoad

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 269 行。

```cpp
bool preferLowLoad = true;
```

### API-0cd134605034 · ndnsf_distributed_repo::PlacementPolicy::preferHighAvailability

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 270 行。

```cpp
bool preferHighAvailability = true;
```

### API-a3b36935cc04 · ndnsf_distributed_repo::StoredObject

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 273 行。

```cpp
struct StoredObject
```

### API-aa997530ea5e · ndnsf_distributed_repo::StoredObject::manifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 275 行。

```cpp
RepoObjectManifest manifest;
```

### API-a2a66c4e4cee · ndnsf_distributed_repo::StoredObject::payload

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 276 行。

```cpp
std::vector<uint8_t> payload;
```

### API-2ab5138479d8 · ndnsf_distributed_repo::parseRepoDeploymentMode

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 279 行。

```cpp
RepoDeploymentMode
parseRepoDeploymentMode(const std::string& value);
```

### API-760cf651edca · ndnsf_distributed_repo::toString

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 282 行。

```cpp
std::string
toString(RepoDeploymentMode mode);
```

### API-1c5634a61cf6 · ndnsf_distributed_repo::enablesRemote

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 285 行。

```cpp
bool
enablesRemote(RepoDeploymentMode mode);
```

### API-7c5c0fd2aa4d · ndnsf_distributed_repo::enablesEmbedded

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 288 行。

```cpp
bool
enablesEmbedded(RepoDeploymentMode mode);
```

### API-45abb109267d · ndnsf_distributed_repo::isInAppRepo

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 291 行。

```cpp
bool
isInAppRepo(const StorageCapability& capability);
```

### API-f94170df34dd · ndnsf_distributed_repo::isPersistentRepo

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 294 行。

```cpp
bool
isPersistentRepo(const StorageCapability& capability);
```

### API-3e7f844b09d5 · ndnsf_distributed_repo::sha256Hex

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 297 行。

```cpp
std::string
sha256Hex(const std::vector<uint8_t>& payload);
```

### API-df8d469dc16d · ndnsf_distributed_repo::selectReplicas

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 300 行。

```cpp
std::vector<StorageCapability>
selectReplicas(const std::vector<StorageCapability>& candidates,
               const PlacementPolicy& policy,
               uint64_t objectSize);
```

### API-da92a743c24d · ndnsf_distributed_repo::RepoStoreBackend

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 305 行。

```cpp
class RepoStoreBackend
```

### API-0588a3f36558 · ndnsf_distributed_repo::RepoStoreBackend::~RepoStoreBackend

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 308 行。

```cpp
virtual ~RepoStoreBackend() = default;
```

### API-503757c5cd68 · ndnsf_distributed_repo::RepoStoreBackend::put

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 312 行。

```cpp
virtual void put(const RepoObjectManifest& manifest, std::vector<uint8_t> payload) = 0;
```

原始接口说明：

```text
// Store opaque APP bytes. Repo backends validate storage shape/capacity only;
// APP trust, signature, and hash verification happen after fetch.
```

### API-a5ef7a8f0000 · ndnsf_distributed_repo::RepoStoreBackend::putManifest

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 316 行。

```cpp
virtual void putManifest(const RepoObjectManifest& manifest) = 0;
```

原始接口说明：

```text
// Store metadata for a logical parent object whose payload is held in child
// objects such as <object>/seg/<index>.
```

### API-21e9b51ab262 · ndnsf_distributed_repo::RepoStoreBackend::get

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 318 行。

```cpp
virtual StoredObject get(const std::string& objectName) const = 0;
```

### API-c8902f7fa75d · ndnsf_distributed_repo::RepoStoreBackend::has

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 320 行。

```cpp
virtual bool has(const std::string& objectName) const = 0;
```

### API-301eaae0f6a9 · ndnsf_distributed_repo::RepoStoreBackend::erase

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 322 行。

```cpp
virtual bool erase(const std::string& objectName) = 0;
```

### API-f2625f162a57 · ndnsf_distributed_repo::RepoStoreBackend::size

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 324 行。

```cpp
virtual size_t size() const = 0;
```

### API-a6c7a869320b · ndnsf_distributed_repo::RepoStoreBackend::listManifests

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 326 行。

```cpp
virtual std::vector<RepoObjectManifest> listManifests() const = 0;
```

### API-84ba06e7584f · ndnsf_distributed_repo::RepoStoreBackend::usedBytes

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 328 行。

```cpp
virtual uint64_t usedBytes() const = 0;
```

### API-e7afebaae123 · ndnsf_distributed_repo::RepoStoreBackend::cacheStatus

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 330 行。

```cpp
virtual RepoCacheStatus cacheStatus() const;
```

### API-facb8e6b44ed · ndnsf_distributed_repo::makeSqliteRepoStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 333 行。

```cpp
std::shared_ptr<RepoStoreBackend>
makeSqliteRepoStore(const std::string& databasePath);
```

### API-9a97af85a418 · ndnsf_distributed_repo::makeTieredRepoStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 336 行。

```cpp
std::shared_ptr<RepoStoreBackend>
makeTieredRepoStore(const std::string& databasePath, uint64_t memoryCacheBytes);
```

### API-aca82839db26 · ndnsf_distributed_repo::makeTieredRepoStore

public / declared-interface；冻结源码：`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`，第 339 行。

```cpp
std::shared_ptr<RepoStoreBackend>
makeTieredRepoStore(std::shared_ptr<RepoStoreBackend> authoritativeStore,
                    uint64_t memoryCacheBytes,
                    std::string authoritativeBackend = "custom");
```

## NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py

源码 SHA-256：`6a0c6dabc3c19809f23aecb8929eaac215108179a0773375d7ebe6e078853d35`。

显式导出：`ARTIFACT_LIFECYCLE_STATES`, `ARTIFACT_LIFECYCLE_TRANSITIONS`, `AdaptiveArtifactTransfer`, `AdaptiveTransferOptions`, `AuthenticatedReplicaReceipt`, `ArtifactApiBackend`, `ArtifactApiError`, `ArtifactCancellationToken`, `ArtifactCapabilityNegotiation`, `ArtifactCapabilityRejection`, `ArtifactCapabilityRequirements`, `ArtifactControlMetrics`, `ArtifactControlMode`, `ArtifactControlOptions`, `ArtifactDescriptor`, `ArtifactErrorCode`, `ArtifactFetchDriver`, `ArtifactFetchResult`, `ArtifactFetchSession`, `ArtifactProgress`, `ArtifactPublishDriver`, `ArtifactPublishResult`, `ArtifactReplicaResult`, `ArtifactRepositoryApi`, `ArtifactReplicaSession`, `ArtifactSessionStatus`, `ArtifactStorageIdentity`, `ArtifactUploadSession`, `AtomicArtifactDestination`, `ArtifactCapability`, `ArtifactChunk`, `ArtifactLimits`, `ArtifactManifestChild`, `ArtifactManifestPage`, `ArtifactManifestTrustPolicy`, `ArtifactManifestVerificationResult`, `ArtifactReference`, `ArtifactReplicaReceipt`, `ArtifactRootManifest`, `ArtifactSegmentDisposition`, `ArtifactSegmentRequest`, `ArtifactTransferSnapshot`, `SignedArtifactRoot`, `ArtifactUploadLease`, `ArtifactValidationError`, `DEFAULT_REPO_SERVICE_ROOT`, `FilesystemCasPayloadStore`, `FilesystemArtifactApiBackend`, `CollaborationArtifactApiBackend`, `install_artifact_collaboration_service`, `HmacReceiptAuthenticator`, `LifecycleTransitionError`, `MetadataStore`, `PayloadStore`, `PersistenceOwnershipError`, `PendingReplicaLeaseCollaboration`, `PendingReplicaTaskCollaboration`, `canonical_repo_operation`, `PlacementPolicy`, `ReplicaLeaseControlFlow`, `ReplicaLeaseControlSnapshot`, `ReplicaLeaseControlState`, `ReplicaLeaseCollaborationClient`, `ReplicaTaskCollaborationClient`, `ReplicaTaskControlSnapshot`, `ArtifactStoreAssignment`, `ArtifactStoreOffer`, `RepoClient`, `RepoCacheStatus`, `RepoCatalogDelta`, `RepoCatalogEntry`, `RepoCatalogStatus`, `RepoDataReference`, `RepoObjectManifest`, `RepoOperationMetrics`, `RepoOperationStatus`, `RepoLifecycleEvent`, `SqliteRepositoryPersistence`, `StorageCapability`, `discovery_record_from_ack`, `capability_from_ack`, `ready_capability_from_ack`, `resolve_active_artifact`, `retrieve_to_atomic_destination`, `decode_store_request`, `artifact_sha256_hex`, `artifact_capability_from_ack`, `canonical_manifest_page_bytes`, `canonical_root_manifest_bytes`, `decode_artifact_manifest_page`, `decode_upload_lease_assignment`, `encode_upload_lease_ack`, `decode_store_assignment`, `decode_store_offer_ack`, `encode_store_offer_ack`, `decode_signed_artifact_root`, `encode_inventory`, `encode_artifact_manifest_page`, `encode_signed_artifact_root`, `encode_store_request`, `make_manifest`, `make_repo_service_name`, `negotiate_artifact_capabilities`, `derive_artifact_data_name`, `derive_manifest_page_name`, `manifest_to_dict`, `is_internal_repo_service`, `parse_manifest_json`, `parse_cache_status_json`, `parse_catalog_delta_json`, `parse_catalog_entry_json`, `parse_catalog_status_json`, `parse_data_reference_json`, `parse_inventory_json`, `parse_operation_status_json`, `select_replicas`, `validate_artifact_resume_identity`, `verify_artifact_chunk_payload`, `verify_artifact_manifest_graph`, `verify_artifact_payload`, `verify_signed_artifact_root`, `repo_service_for_operation`, `repo_versioned_services`, `sha256_hex`。

### API-569eb8d30ea6 · RepoDataPlaneProducer

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 172 行。

```python
class RepoDataPlaneProducer:
```

原始接口说明：

```text
Serve repository Data through one callback-backed native Face.
```

### API-0cf8d8500c85 · RepoDataPlaneProducer.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 175 行。

```python
def __init__(
        self,
        lookup: Callable[[str, bool], Optional[bytes]],
        *,
        signing_identity: str = "",
        forwarding_route_prefixes: Optional[list[str]] = None,
    ) -> None:
```

### API-abd27fa7f77c · RepoDataPlaneProducer.activate_prefix

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 188 行。

```python
def activate_prefix(self, prefix: str) -> None:
```

### API-67a7a3535c0e · RepoDataPlaneProducer.start

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 191 行。

```python
def start(self) -> "RepoDataPlaneProducer":
```

### API-f54c958962f1 · RepoDataPlaneProducer.stop

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 195 行。

```python
def stop(self) -> None:
```

### API-e675c15fb82c · RepoDataPlaneProducer.status

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 199 行。

```python
def status(self) -> dict[str, object]:
```

### API-27a38d61ed18 · manifest_to_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 210 行。

```python
def manifest_to_dict(manifest: RepoObjectManifest) -> dict:
```

### API-870b1a94218d · capability_from_ack

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 214 行。

```python
def capability_from_ack(candidate: AckCandidate) -> Optional[StorageCapability]:
```

### API-ee7de96e817c · artifact_capability_from_ack

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 249 行。

```python
def artifact_capability_from_ack(
    candidate: AckCandidate,
) -> Optional[ArtifactCapability]:
```

原始接口说明：

```text
Decode one strict artifact capability from a generic NDNSF ACK.
```

### API-47811dc5e62f · discovery_record_from_ack

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 268 行。

```python
def discovery_record_from_ack(candidate: AckCandidate) -> ServiceDiscoveryRecord:
```

原始接口说明：

```text
Parse a core service-discovery record from a Repo ACK.

Legacy-only ACKs require the explicit ``mixed`` compatibility mode.
Typed ``ProviderCapabilityHint`` ACKs can mark a provider unready or
draining, which capacity selection should respect before applying
storage-placement policy.
```

### API-7eca9d435263 · ready_capability_from_ack

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 285 行。

```python
def ready_capability_from_ack(candidate: AckCandidate) -> Optional[StorageCapability]:
```

### API-e51473e17876 · RepoClient

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 292 行。

```python
class RepoClient:
```

原始接口说明：

```text
Small synchronous repo client built on NDNSF Python ``ServiceUser``.

Public operations use versioned operation services. The payload operation
remains as a fail-closed consistency check, not as the authorization key.
```

### API-817e18c702e8 · RepoClient.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 299 行。

```python
def __init__(
        self,
        user: ServiceUser,
        repo_service_name: str = "/NDNSF/DistributedRepo",
        *,
        ack_timeout_ms: int = 1000,
        timeout_ms: int = 30000,
        artifact_backend: Optional[ArtifactApiBackend] = None,
    ) -> None:
```

### API-f5d757b6dc09 · RepoClient.artifact_api

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 319 行。

```python
def artifact_api(self) -> ArtifactRepositoryApi:
```

原始接口说明：

```text
Public advanced artifact facade; never exposes runtime private state.
```

### API-3aeb4d35fc55 · RepoClient.configure_artifact_backend

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 324 行。

```python
def configure_artifact_backend(
        self, backend: ArtifactApiBackend
    ) -> None:
```

原始接口说明：

```text
Install the application/runtime artifact-manifest-v2 backend.
```

### API-23f062e38d7e · RepoClient.publish_file

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 331 行。

```python
def publish_file(self, path: Union[str, Path], **kwargs) -> ArtifactPublishResult:
```

### API-f2721c41177a · RepoClient.publish_file_async

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 334 行。

```python
async def publish_file_async(
        self, path: Union[str, Path], **kwargs
    ) -> ArtifactPublishResult:
```

### API-49f10a018602 · RepoClient.fetch_file

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 339 行。

```python
def fetch_file(
        self,
        reference: ArtifactReference,
        destination: Union[str, Path],
        **kwargs,
    ) -> ArtifactFetchResult:
```

### API-54eb5308ac58 · RepoClient.fetch_file_async

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 349 行。

```python
async def fetch_file_async(
        self,
        reference: ArtifactReference,
        destination: Union[str, Path],
        **kwargs,
    ) -> ArtifactFetchResult:
```

### API-8bfc70e48880 · RepoClient.begin_upload

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 359 行。

```python
def begin_upload(
        self, descriptor: ArtifactDescriptor, **kwargs
    ) -> ArtifactUploadSession:
```

### API-3c59aae489c2 · RepoClient.begin_fetch

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 364 行。

```python
def begin_fetch(
        self,
        reference: ArtifactReference,
        destination: Union[str, Path],
        **kwargs,
    ) -> ArtifactFetchSession:
```

### API-a64a8588a0f0 · RepoClient.publisher_namespace

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 378 行。

```python
def publisher_namespace(self) -> str:
```

### API-a5f1057ab229 · RepoClient.publisher_object_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 384 行。

```python
def publisher_object_name(self, suffix: str) -> str:
```

### API-0735f49956d8 · RepoClient.make_manifest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 404 行。

```python
def make_manifest(
        *,
        object_name: str,
        object_type: str,
        payload: bytes,
        replication_factor: int = 1,
        replica_nodes: Iterable[str] = (),
        policy_epoch: str = "",
    ) -> RepoObjectManifest:
```

### API-14a688092ad3 · RepoClient.capability

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 422 行。

```python
def capability(self) -> list[StorageCapability]:
```

### API-01bf2985363b · RepoClient.artifact_capabilities

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 439 行。

```python
def artifact_capabilities(self) -> list[ArtifactCapability]:
```

原始接口说明：

```text
Fetch validated format capabilities without inferring defaults.
```

### API-dcbfaebc6639 · RepoClient.insert

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 465 行。

```python
def insert(
        self,
        *,
        object_name: str,
        payload: bytes,
        object_type: str = "artifact",
        replication_factor: int = 1,
        replica_nodes: Iterable[str] = (),
        policy_epoch: str = "",
        selector: Optional[Callable[[list[AckCandidate]], list[str]]] = None,
    ) -> RepoObjectManifest:
```

### API-b5098fef278f · RepoClient.store

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 510 行。

```python
def store(
        self,
        *,
        object_name: str,
        payload: bytes,
        object_type: str = "artifact",
        replication_factor: int = 1,
        replica_nodes: Iterable[str] = (),
        policy_epoch: str = "",
        selector: Optional[Callable[[list[AckCandidate]], list[str]]] = None,
    ) -> RepoObjectManifest:
```

### API-f99b7839d3ee · RepoClient.put

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 531 行。

```python
def put(
        self,
        object_name: str,
        payload: bytes,
        *,
        object_type: str = "object",
        replication_factor: int = 1,
        replica_nodes: Iterable[str] = (),
        policy_epoch: str = "",
        selector: Optional[Callable[[list[AckCandidate]], list[str]]] = None,
    ) -> RepoObjectManifest:
```

### API-14e720887863 · RepoClient.fetch

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 552 行。

```python
def fetch(self, object_name: str) -> bytes:
```

### API-e1a952026c43 · RepoClient.get

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 567 行。

```python
def get(self, object_name: str) -> bytes:
```

### API-dc55ea97af91 · RepoClient.fetch_object

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 570 行。

```python
def fetch_object(self, manifest: RepoObjectManifest) -> bytes:
```

原始接口说明：

```text
Fetch one logical object described by a repo manifest.

The current remote repo service returns object payloads by object name.
This helper gives callers the same object-level shape as the C++ API and
verifies manifest size/hash after the fetch. If a future remote service
exposes manifest-driven segmented fetch directly, this method remains
the stable high-level entry point.
```

### API-f19b67aad253 · RepoClient.get_object

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 583 行。

```python
def get_object(self, manifest: RepoObjectManifest) -> bytes:
```

### API-b74610ce77e3 · RepoClient.manifest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 586 行。

```python
def manifest(self, object_name: str) -> RepoObjectManifest:
```

### API-dcefa354ff45 · RepoClient.inventory

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 598 行。

```python
def inventory(self) -> dict[str, RepoObjectManifest]:
```

### API-b4c8c1c6dcbf · RepoClient.list

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 615 行。

```python
def list(self) -> dict[str, RepoObjectManifest]:
```

### API-fcbdc80262e0 · RepoClient.delete

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 618 行。

```python
def delete(self, object_name: str) -> None:
```

### API-5bbcaa4ec3fe · RepoClient.remove

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/__init__.py`，第 629 行。

```python
def remove(self, object_name: str) -> None:
```

## NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py

源码 SHA-256：`92420bbc20f1b1f153869c073fc3a4e8fa83285d4ec7452ae6385163d922fb81`。

显式导出：`ArtifactApiBackend`, `ArtifactApiError`, `ArtifactCancellationToken`, `ArtifactCapabilityNegotiation`, `ArtifactCapabilityRejection`, `ArtifactCapabilityRequirements`, `ArtifactControlMode`, `ArtifactControlOptions`, `ArtifactDescriptor`, `ArtifactErrorCode`, `ArtifactFetchDriver`, `ArtifactFetchResult`, `ArtifactFetchSession`, `ArtifactProgress`, `ArtifactPublishDriver`, `ArtifactPublishResult`, `ArtifactReplicaResult`, `ArtifactRepositoryApi`, `ArtifactSessionStatus`, `ArtifactUploadSession`, `negotiate_artifact_capabilities`。

### API-70f17fc06c55 · ArtifactErrorCode

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 27 行。

```python
class ArtifactErrorCode(str, Enum):
```

### API-83cfbf982439 · ArtifactControlMode

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 45 行。

```python
class ArtifactControlMode(str, Enum):
```

原始接口说明：

```text
Public selection of the generic NDNSF collaboration control path.
```

### API-8f3d94487aad · ArtifactControlOptions

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 53 行。

```python
class ArtifactControlOptions:
```

### API-a01fa99fc200 · ArtifactControlOptions.mode

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 54 行。

```python
mode: ArtifactControlMode = ArtifactControlMode.COLLABORATION
```

### API-80be4d6ed652 · ArtifactControlOptions.targeted_provider

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 55 行。

```python
targeted_provider: str = ""
```

### API-6deb85a1815c · ArtifactCapabilityRequirements

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 75 行。

```python
class ArtifactCapabilityRequirements:
```

原始接口说明：

```text
Exact v2 geometry and durability features required from every replica.
```

### API-607434819b1e · ArtifactCapabilityRequirements.root_signature_algorithm

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 78 行。

```python
root_signature_algorithm: str = "ed25519"
```

### API-ed86c1addc91 · ArtifactCapabilityRequirements.chunk_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 79 行。

```python
chunk_bytes: int = 1024 * 1024
```

### API-25ee3f2de594 · ArtifactCapabilityRequirements.root_encoded_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 80 行。

```python
root_encoded_bytes: int = 64 * 1024
```

### API-59c683962273 · ArtifactCapabilityRequirements.page_encoded_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 81 行。

```python
page_encoded_bytes: int = 1024 * 1024
```

### API-aa3724b57f9f · ArtifactCapabilityRequirements.page_entries

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 82 行。

```python
page_entries: int = 4096
```

### API-0a1d43ac64aa · ArtifactCapabilityRequirements.manifest_depth

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 83 行。

```python
manifest_depth: int = 8
```

### API-0c4accce799e · ArtifactCapabilityRequirements.require_resume

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 84 行。

```python
require_resume: bool = True
```

### API-70887092ffe7 · ArtifactCapabilityRequirements.require_replica_receipts

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 85 行。

```python
require_replica_receipts: bool = True
```

### API-4bb89c9e5801 · ArtifactCapabilityRejection

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 102 行。

```python
class ArtifactCapabilityRejection:
```

### API-c014dd6a4296 · ArtifactCapabilityRejection.repo_node

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 103 行。

```python
repo_node: str
```

### API-b8de10eb6c1d · ArtifactCapabilityRejection.reasons

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 104 行。

```python
reasons: tuple[str, ...]
```

### API-c2cc011da27e · ArtifactCapabilityNegotiation

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 108 行。

```python
class ArtifactCapabilityNegotiation:
```

### API-b48f55252630 · ArtifactCapabilityNegotiation.artifact

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 109 行。

```python
artifact: ArtifactReference
```

### API-957731c3539b · ArtifactCapabilityNegotiation.requested_replicas

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 110 行。

```python
requested_replicas: int
```

### API-4ef3c020689e · ArtifactCapabilityNegotiation.eligible

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 111 行。

```python
eligible: tuple[ArtifactCapability, ...]
```

### API-6c96649fe210 · ArtifactCapabilityNegotiation.rejected

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 112 行。

```python
rejected: tuple[ArtifactCapabilityRejection, ...]
```

### API-940fe9082749 · negotiate_artifact_capabilities

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 115 行。

```python
def negotiate_artifact_capabilities(
    capabilities: Iterable[ArtifactCapability],
    artifact: ArtifactReference,
    *,
    requested_replicas: int,
    requirements: ArtifactCapabilityRequirements = (
        ArtifactCapabilityRequirements()
    ),
) -> ArtifactCapabilityNegotiation:
```

原始接口说明：

```text
Fail-closed v2 capability filter; placement may rank the eligible set.
```

### API-76e04b15a6ec · ArtifactDescriptor

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 199 行。

```python
class ArtifactDescriptor:
```

### API-2933d217e0a3 · ArtifactDescriptor.reference

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 200 行。

```python
reference: ArtifactReference
```

### API-f3ecd7af92d6 · ArtifactDescriptor.requested_replicas

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 201 行。

```python
requested_replicas: int
```

### API-97dce424a6f5 · ArtifactDescriptor.idempotency_key

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 202 行。

```python
idempotency_key: str
```

### API-ba8988255165 · ArtifactDescriptor.verification

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 203 行。

```python
verification: str = "signed-manifest"
```

### API-16018bb57d43 · ArtifactDescriptor.resume

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 204 行。

```python
resume: bool = True
```

### API-1db99c04b386 · ArtifactDescriptor.timeout_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 205 行。

```python
timeout_ms: int = 60_000
```

### API-adc53621dd5c · ArtifactDescriptor.control

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 206 行。

```python
control: ArtifactControlOptions = field(
        default_factory=ArtifactControlOptions
    )
```

### API-25bf5ca6711a · ArtifactProgress

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 224 行。

```python
class ArtifactProgress:
```

### API-f3abb104eddd · ArtifactProgress.operation_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 225 行。

```python
operation_id: str
```

### API-44370b2e4887 · ArtifactProgress.artifact

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 226 行。

```python
artifact: ArtifactReference
```

### API-105cde412093 · ArtifactProgress.phase

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 227 行。

```python
phase: str
```

### API-228d38af8f9a · ArtifactProgress.received_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 228 行。

```python
received_bytes: int
```

### API-3424cfdc4d6b · ArtifactProgress.verified_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 229 行。

```python
verified_bytes: int
```

### API-23fb8b640b91 · ArtifactProgress.committed_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 230 行。

```python
committed_bytes: int
```

### API-29425c2fef7d · ArtifactProgress.total_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 231 行。

```python
total_bytes: int
```

### API-fdd74f51ff83 · ArtifactProgress.selected_replicas

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 232 行。

```python
selected_replicas: int
```

### API-ec050a2db7d2 · ArtifactProgress.committed_replicas

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 233 行。

```python
committed_replicas: int
```

### API-e87fd53a4cf8 · ArtifactProgress.retransmitted_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 234 行。

```python
retransmitted_bytes: int
```

### API-cc91d4fe03b7 · ArtifactProgress.sequence

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 235 行。

```python
sequence: int
```

### API-25b26664586f · ArtifactProgress.timestamp_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 236 行。

```python
timestamp_ms: int
```

### API-f15a8614369e · ArtifactProgress.last_segment

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 240 行。

```python
last_segment: int = -1
```

### API-9dbcfc9a9149 · ArtifactProgress.delivered_segments

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 241 行。

```python
delivered_segments: int = 0
```

### API-3df9f139452c · ArtifactProgress.total_segments

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 242 行。

```python
total_segments: int = 0
```

### API-3e9553f976e1 · ArtifactProgress.elapsed_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 243 行。

```python
elapsed_ms: float = 0.0
```

### API-1353fded8449 · ArtifactReplicaResult

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 275 行。

```python
class ArtifactReplicaResult:
```

### API-34eb4cf9b3b5 · ArtifactReplicaResult.repo_node

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 276 行。

```python
repo_node: str
```

### API-b7754a4cbb5c · ArtifactReplicaResult.state

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 277 行。

```python
state: str
```

### API-b8c11479c321 · ArtifactReplicaResult.receipt_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 278 行。

```python
receipt_id: str = ""
```

### API-1e6c4bf89c2a · ArtifactReplicaResult.error_code

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 279 行。

```python
error_code: str = ""
```

### API-66f31ce5b106 · ArtifactPublishResult

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 283 行。

```python
class ArtifactPublishResult:
```

### API-e2bf8338f8c2 · ArtifactPublishResult.reference

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 284 行。

```python
reference: ArtifactReference
```

### API-71da0fe29593 · ArtifactPublishResult.operation_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 285 行。

```python
operation_id: str
```

### API-08d318936a10 · ArtifactPublishResult.requested_replicas

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 286 行。

```python
requested_replicas: int
```

### API-0ff6d3cc81db · ArtifactPublishResult.achieved_replicas

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 287 行。

```python
achieved_replicas: int
```

### API-2089d8e5a396 · ArtifactPublishResult.replicas

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 288 行。

```python
replicas: tuple[ArtifactReplicaResult, ...]
```

### API-d074133e5789 · ArtifactPublishResult.deduplicated

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 289 行。

```python
deduplicated: bool = False
```

### API-508b256889a1 · ArtifactPublishResult.resumed

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 290 行。

```python
resumed: bool = False
```

### API-af6d4b437852 · ArtifactPublishResult.total_duration_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 291 行。

```python
total_duration_ms: float = 0.0
```

### API-ea04da3e541e · ArtifactPublishResult.phase_durations_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 292 行。

```python
phase_durations_ms: dict[str, float] = field(default_factory=dict)
```

### API-5d25ad5f1352 · ArtifactFetchResult

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 316 行。

```python
class ArtifactFetchResult:
```

### API-3201f85d62ce · ArtifactFetchResult.reference

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 317 行。

```python
reference: ArtifactReference
```

### API-027608ffdc8d · ArtifactFetchResult.operation_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 318 行。

```python
operation_id: str
```

### API-b539dcce836a · ArtifactFetchResult.destination

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 319 行。

```python
destination: Path
```

### API-547d5adcc1ec · ArtifactFetchResult.reused_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 320 行。

```python
reused_bytes: int
```

### API-f8b073217db7 · ArtifactFetchResult.transferred_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 321 行。

```python
transferred_bytes: int
```

### API-f10f5bd84b9b · ArtifactFetchResult.source_replicas

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 322 行。

```python
source_replicas: tuple[str, ...]
```

### API-75da3b72bdca · ArtifactFetchResult.total_duration_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 323 行。

```python
total_duration_ms: float = 0.0
```

### API-293beb1c3ab2 · ArtifactFetchResult.phase_durations_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 324 行。

```python
phase_durations_ms: dict[str, float] = field(default_factory=dict)
```

### API-6e95e4a01570 · ArtifactFetchResult.last_segment

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 325 行。

```python
last_segment: int = -1
```

### API-f8833f1f327f · ArtifactFetchResult.delivered_segments

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 326 行。

```python
delivered_segments: int = 0
```

### API-f967c8acaf38 · ArtifactFetchResult.total_segments

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 327 行。

```python
total_segments: int = 0
```

### API-b3ad2f45e994 · ArtifactFetchResult.retransmitted_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 328 行。

```python
retransmitted_bytes: int = 0
```

### API-a39fa1e69040 · ArtifactSessionStatus

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 349 行。

```python
class ArtifactSessionStatus:
```

### API-15b9503bd8a4 · ArtifactSessionStatus.operation_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 350 行。

```python
operation_id: str
```

### API-54328e63611f · ArtifactSessionStatus.direction

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 351 行。

```python
direction: str
```

### API-6305d64b52a5 · ArtifactSessionStatus.state

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 352 行。

```python
state: str
```

### API-baacb063a7a5 · ArtifactSessionStatus.artifact

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 353 行。

```python
artifact: ArtifactReference
```

### API-9c184d2d7ec9 · ArtifactSessionStatus.progress

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 354 行。

```python
progress: Optional[ArtifactProgress] = None
```

### API-d0164051f242 · ArtifactSessionStatus.error_code

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 355 行。

```python
error_code: str = ""
```

### API-183417f65f62 · ArtifactApiError

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 358 行。

```python
class ArtifactApiError(RuntimeError):
```

原始接口说明：

```text
Bounded, stable public error without peer-controlled raw diagnostics.
```

### API-67f73b689030 · ArtifactApiError.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 361 行。

```python
def __init__(
        self,
        code: Union[ArtifactErrorCode, str],
        message: str,
        *,
        operation_id: str = "",
        artifact: Optional[ArtifactReference] = None,
        achieved_replicas: int = 0,
    ) -> None:
```

### API-02b499b1c8ef · ArtifactCancellationToken

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 381 行。

```python
class ArtifactCancellationToken:
```

### API-969948be8c06 · ArtifactCancellationToken.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 382 行。

```python
def __init__(self) -> None:
```

### API-5e12a098fc2e · ArtifactCancellationToken.cancel

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 385 行。

```python
def cancel(self) -> None:
```

### API-82cc076b0e6d · ArtifactCancellationToken.cancelled

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 389 行。

```python
def cancelled(self) -> bool:
```

### API-dbaa6540c21c · ArtifactCancellationToken.raise_if_cancelled

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 392 行。

```python
def raise_if_cancelled(
        self,
        operation_id: str,
        artifact: Optional[ArtifactReference] = None,
    ) -> None:
```

### API-9bc1d2fb12f1 · ArtifactPublishDriver

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 409 行。

```python
class ArtifactPublishDriver(Protocol):
```

### API-05a3ab3b2ec5 · ArtifactPublishDriver.transfer

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 410 行。

```python
def transfer(
        self, path: Path, cancellation: ArtifactCancellationToken
    ) -> None:
```

### API-7c73f69603d5 · ArtifactPublishDriver.status

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 414 行。

```python
def status(self) -> ArtifactSessionStatus:
```

### API-7792c359b2f3 · ArtifactPublishDriver.commit

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 416 行。

```python
def commit(self) -> ArtifactPublishResult:
```

### API-e74d4a602715 · ArtifactPublishDriver.abort

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 418 行。

```python
def abort(self, preserve_progress: bool) -> ArtifactSessionStatus:
```

### API-35493a6ce28e · ArtifactFetchDriver

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 421 行。

```python
class ArtifactFetchDriver(Protocol):
```

### API-89876efbe323 · ArtifactFetchDriver.transfer

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 422 行。

```python
def transfer(self, cancellation: ArtifactCancellationToken) -> None:
```

### API-dccd1e67f9d9 · ArtifactFetchDriver.status

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 424 行。

```python
def status(self) -> ArtifactSessionStatus:
```

### API-a71fa7be6de1 · ArtifactFetchDriver.commit

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 426 行。

```python
def commit(self) -> ArtifactFetchResult:
```

### API-0b74f1cbf05b · ArtifactFetchDriver.abort

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 428 行。

```python
def abort(self, preserve_progress: bool) -> ArtifactSessionStatus:
```

### API-f0ec2c6f8999 · ArtifactApiBackend

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 431 行。

```python
class ArtifactApiBackend(Protocol):
```

### API-2e85c5ec1061 · ArtifactApiBackend.begin_publish

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 432 行。

```python
def begin_publish(
        self,
        descriptor: ArtifactDescriptor,
        operation_id: str,
        emit_progress: ProgressObserver,
    ) -> ArtifactPublishDriver:
```

### API-071f4ef39f9e · ArtifactApiBackend.begin_fetch

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 439 行。

```python
def begin_fetch(
        self,
        reference: ArtifactReference,
        destination: Path,
        operation_id: str,
        *,
        resume: bool,
        verify: bool,
        replace: bool,
        timeout_ms: int,
        control: ArtifactControlOptions,
        emit_progress: ProgressObserver,
    ) -> ArtifactFetchDriver:
```

### API-0ffe61d33e5b · ArtifactUploadSession

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 562 行。

```python
class ArtifactUploadSession:
```

### API-7f2ca31031ef · ArtifactUploadSession.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 563 行。

```python
def __init__(
        self,
        descriptor: ArtifactDescriptor,
        operation_id: str,
        driver: ArtifactPublishDriver,
        guard: _ProgressGuard,
        cancellation: ArtifactCancellationToken,
    ) -> None:
```

### API-0cce98235169 · ArtifactUploadSession.upload_file

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 582 行。

```python
def upload_file(self, path: Union[str, Path]) -> ArtifactSessionStatus:
```

### API-4c7b00fa79fd · ArtifactUploadSession.status

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 612 行。

```python
def status(self) -> ArtifactSessionStatus:
```

### API-49ab94470101 · ArtifactUploadSession.commit

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 620 行。

```python
def commit(self) -> ArtifactPublishResult:
```

### API-6a71304de9d1 · ArtifactUploadSession.abort

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 661 行。

```python
def abort(self, preserve_progress: bool = True) -> ArtifactSessionStatus:
```

### API-2ee6d94fb64a · ArtifactFetchSession

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 684 行。

```python
class ArtifactFetchSession:
```

### API-9e7f69dd1b29 · ArtifactFetchSession.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 685 行。

```python
def __init__(
        self,
        reference: ArtifactReference,
        destination: Path,
        operation_id: str,
        driver: ArtifactFetchDriver,
        guard: _ProgressGuard,
        cancellation: ArtifactCancellationToken,
    ) -> None:
```

### API-fd2993f679c4 · ArtifactFetchSession.transfer

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 706 行。

```python
def transfer(self) -> ArtifactSessionStatus:
```

### API-f804cacaa93e · ArtifactFetchSession.status

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 735 行。

```python
def status(self) -> ArtifactSessionStatus:
```

### API-75b5e5be8f27 · ArtifactFetchSession.commit

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 741 行。

```python
def commit(self) -> ArtifactFetchResult:
```

### API-ddfa95f9d936 · ArtifactFetchSession.abort

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 781 行。

```python
def abort(self, preserve_progress: bool = True) -> ArtifactSessionStatus:
```

### API-6f06f90f062b · ArtifactRepositoryApi

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 799 行。

```python
class ArtifactRepositoryApi:
```

### API-376225387b67 · ArtifactRepositoryApi.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 800 行。

```python
def __init__(
        self,
        backend: Optional[ArtifactApiBackend],
        *,
        publisher_identity: str,
        default_timeout_ms: int = 60_000,
    ) -> None:
```

### API-91dcd6af85dd · ArtifactRepositoryApi.begin_upload

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 819 行。

```python
def begin_upload(
        self,
        descriptor: ArtifactDescriptor,
        *,
        on_progress: Optional[ProgressObserver] = None,
        cancellation: Optional[ArtifactCancellationToken] = None,
    ) -> ArtifactUploadSession:
```

### API-c7c484fc1ed7 · ArtifactRepositoryApi.publish_file

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 844 行。

```python
def publish_file(
        self,
        path: Union[str, Path],
        *,
        name: str,
        expected_sha256: str,
        replicas: int = 1,
        verification: str = "signed-manifest",
        resume: bool = True,
        on_progress: Optional[ProgressObserver] = None,
        idempotency_key: str = "",
        policy_epoch: str = "default",
        timeout_ms: Optional[int] = None,
        control: ArtifactControlOptions = ArtifactControlOptions(),
        cancellation: Optional[ArtifactCancellationToken] = None,
    ) -> ArtifactPublishResult:
```

### API-3f88a1d573c8 · ArtifactRepositoryApi.publish_file_async

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 903 行。

```python
async def publish_file_async(self, *args, **kwargs) -> ArtifactPublishResult:
```

### API-39b8d985de2c · ArtifactRepositoryApi.begin_fetch

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 914 行。

```python
def begin_fetch(
        self,
        reference: ArtifactReference,
        destination: Union[str, Path],
        *,
        resume: bool = True,
        verify: bool = True,
        replace: bool = False,
        on_progress: Optional[ProgressObserver] = None,
        idempotency_key: str = "",
        timeout_ms: Optional[int] = None,
        control: ArtifactControlOptions = ArtifactControlOptions(),
        cancellation: Optional[ArtifactCancellationToken] = None,
    ) -> ArtifactFetchSession:
```

### API-1707faa2f3ea · ArtifactRepositoryApi.fetch_file

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 960 行。

```python
def fetch_file(
        self,
        reference: ArtifactReference,
        destination: Union[str, Path],
        **kwargs,
    ) -> ArtifactFetchResult:
```

### API-c2ee9ba061bd · ArtifactRepositoryApi.fetch_file_async

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_api.py`，第 979 行。

```python
async def fetch_file_async(self, *args, **kwargs) -> ArtifactFetchResult:
```

## NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py

源码 SHA-256：`52512671257f0309628d3b607666cdacdde5c0e05a47b7ff43bf64c13a4e29f7`。

显式导出：`AuthenticatedReplicaReceipt`, `ArtifactReplicaSession`, `AtomicArtifactDestination`, `HmacReceiptAuthenticator`, `resolve_active_artifact`, `retrieve_to_atomic_destination`。

### API-efff4199333d · AuthenticatedReplicaReceipt

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 106 行。

```python
class AuthenticatedReplicaReceipt:
```

### API-4e4f6b8b95eb · AuthenticatedReplicaReceipt.receipt

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 107 行。

```python
receipt: ArtifactReplicaReceipt
```

### API-b8f9bbb1012f · AuthenticatedReplicaReceipt.signer_key_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 108 行。

```python
signer_key_id: str
```

### API-4853071a9e1e · AuthenticatedReplicaReceipt.authentication_algorithm

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 109 行。

```python
authentication_algorithm: str
```

### API-fcbadb8afaa4 · AuthenticatedReplicaReceipt.signature

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 110 行。

```python
signature: bytes
```

### API-663745a7b462 · AuthenticatedReplicaReceipt.to_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 112 行。

```python
def to_dict(self) -> dict[str, Any]:
```

### API-6eff9e86030c · AuthenticatedReplicaReceipt.to_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 121 行。

```python
def to_bytes(self) -> bytes:
```

### API-7da69753bf16 · AuthenticatedReplicaReceipt.from_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 125 行。

```python
def from_dict(cls, value: dict[str, Any]) -> "AuthenticatedReplicaReceipt":
```

### API-b1e816cab2a2 · AuthenticatedReplicaReceipt.from_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 150 行。

```python
def from_bytes(cls, wire: bytes) -> "AuthenticatedReplicaReceipt":
```

### API-dc857c101026 · HmacReceiptAuthenticator

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 164 行。

```python
class HmacReceiptAuthenticator:
```

原始接口说明：

```text
Repository-identity-bound MAC for receipts inside protected NDNSF flows.

The key is provisioned by the same authorization domain that validates the
repository's NDNSF identity. It is not serialized into a receipt or catalog.
```

### API-40d9ad3e0311 · HmacReceiptAuthenticator.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 171 行。

```python
def __init__(self, repo_node: str, key_id: str, key: bytes) -> None:
```

### API-ba641920cfbd · HmacReceiptAuthenticator.sign

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 192 行。

```python
def sign(self, receipt: ArtifactReplicaReceipt) -> AuthenticatedReplicaReceipt:
```

### API-52147326855b · HmacReceiptAuthenticator.verify

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 207 行。

```python
def verify(
        self,
        envelope: AuthenticatedReplicaReceipt,
        *,
        expected_artifact: ArtifactReference | None = None,
        expected_operation_id: str = "",
    ) -> ArtifactReplicaReceipt:
```

### API-cbeec1b8a4fb · ArtifactReplicaSession

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 244 行。

```python
class ArtifactReplicaSession:
```

原始接口说明：

```text
Compose manifest trust, bounded chunk writes, durable commit, and activation.
```

### API-995caf53cc99 · ArtifactReplicaSession.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 247 行。

```python
def __init__(
        self,
        *,
        persistence: SqliteRepositoryPersistence,
        operation_id: str,
        repo_node: str,
        generation: int,
        upload_lease: ArtifactUploadLease,
        lease_validation_time_ms: int,
        artifact: ArtifactReference,
        signed_root: SignedArtifactRoot,
        pages: Sequence[ArtifactManifestPage],
        chunks: Sequence[ArtifactChunk],
        capability: ArtifactCapability,
        trust_policy: ArtifactManifestTrustPolicy,
        receipt_authenticator: HmacReceiptAuthenticator,
        limits: ArtifactLimits | None = None,
    ) -> None:
```

### API-8437182f2444 · ArtifactReplicaSession.payload_store

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 358 行。

```python
def payload_store(self) -> FilesystemCasPayloadStore:
```

### API-89356526d9aa · ArtifactReplicaSession.missing_chunks

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 398 行。

```python
def missing_chunks(self, now_ms: int) -> tuple[int, ...]:
```

### API-273539f65a7f · ArtifactReplicaSession.renew_lease

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 405 行。

```python
def renew_lease(self, lease: ArtifactUploadLease, now_ms: int) -> None:
```

### API-4a0c6cd02bc7 · ArtifactReplicaSession.resume

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 419 行。

```python
def resume(self, lease: ArtifactUploadLease, now_ms: int) -> None:
```

### API-50007a64c350 · ArtifactReplicaSession.expire

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 433 行。

```python
def expire(self, now_ms: int) -> bool:
```

### API-c455a99ec045 · ArtifactReplicaSession.cancel

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 439 行。

```python
def cancel(self, *, preserve_progress: bool, now_ms: int) -> None:
```

### API-c8de44f161f8 · ArtifactReplicaSession.state

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 466 行。

```python
def state(self) -> str:
```

### API-c6e90a23276c · ArtifactReplicaSession.reserve

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 485 行。

```python
def reserve(self, now_ms: int) -> None:
```

原始接口说明：

```text
Start a legacy capacity-reservation session.

New Selection-assigned tasks use :meth:`begin_assigned_task`.
```

### API-f99fb0867b9e · ArtifactReplicaSession.begin_assigned_task

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 513 行。

```python
def begin_assigned_task(self, now_ms: int) -> None:
```

原始接口说明：

```text
Accept one selected task without reserving bytes or taking a lock.
```

### API-5b9417bd234c · ArtifactReplicaSession.receive_chunk

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 525 行。

```python
def receive_chunk(
        self, chunk_index: int, payload: bytes, *, now_ms: int
    ) -> bool:
```

### API-361d9b20473e · ArtifactReplicaSession.verify_complete

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 556 行。

```python
def verify_complete(self, now_ms: int) -> None:
```

### API-1945a056a547 · ArtifactReplicaSession.commit_and_activate

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 577 行。

```python
def commit_and_activate(
        self,
        now_ms: int,
        *,
        crash_injector: Callable[[str], None] | None = None,
    ) -> AuthenticatedReplicaReceipt:
```

### API-16e74ea6b4d7 · ArtifactReplicaSession.fail

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 665 行。

```python
def fail(self, reason: str, now_ms: int) -> None:
```

### API-88526cf04c99 · resolve_active_artifact

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 686 行。

```python
def resolve_active_artifact(
    persistence: SqliteRepositoryPersistence,
    logical_name: str,
    policy_epoch: str,
) -> ArtifactReference:
```

### API-304d00646cc5 · AtomicArtifactDestination

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 699 行。

```python
class AtomicArtifactDestination:
```

原始接口说明：

```text
Out-of-order bounded range sink with no partial destination visibility.

Payload fsync and resume-sidecar replacement are batched so a large NDN
transfer does not perform one directory fsync per segment. ``finalize``
and ``abort(preserve_progress=True)`` always force the latest checkpoint.
```

### API-2b273df94ed0 · AtomicArtifactDestination.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 712 行。

```python
def __init__(
        self,
        destination: str | Path,
        artifact: ArtifactReference,
        operation_id: str,
        *,
        max_range_bytes: int = 16 * 1024 * 1024,
        resume: bool = True,
        replace: bool = False,
        checkpoint_bytes: int | None = None,
    ) -> None:
```

### API-0aba3574f194 · AtomicArtifactDestination.missing_ranges

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 888 行。

```python
def missing_ranges(
        self, *, maximum_range_bytes: int | None = None
    ) -> tuple[tuple[int, int], ...]:
```

### API-6c4ce66d99c1 · AtomicArtifactDestination.write_range

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 908 行。

```python
def write_range(self, offset: int, payload: bytes) -> None:
```

### API-16b8018e4931 · AtomicArtifactDestination.finalize

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 977 行。

```python
def finalize(self) -> Path:
```

### API-b2dbf3ebce0e · AtomicArtifactDestination.abort

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 1012 行。

```python
def abort(self, *, preserve_progress: bool = False) -> None:
```

### API-eb785b472cb5 · AtomicArtifactDestination.cancel

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 1021 行。

```python
def cancel(self, *, preserve_progress: bool = True) -> None:
```

### API-f3119dd0c156 · retrieve_to_atomic_destination

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_lifecycle.py`，第 1025 行。

```python
def retrieve_to_atomic_destination(
    artifact: ArtifactReference,
    destination: str | Path,
    operation_id: str,
    verified_ranges: Iterable[tuple[int, bytes]],
) -> Path:
```

## NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py

源码 SHA-256：`8711b7007987a6da3ead1aac21967eb5d5f3d8486e887dbad5a9e61967db377a`。

显式导出：`ArtifactStoreAssignment`, `ArtifactStoreOffer`, `ReplicaTaskControlSnapshot`, `PendingReplicaTaskCollaboration`, `ReplicaTaskCollaborationClient`, `decode_store_assignment`, `decode_store_offer_ack`, `encode_store_offer_ack`, `decode_upload_lease_assignment`, `encode_upload_lease_ack`, `PendingReplicaLeaseCollaboration`, `ReplicaLeaseCollaborationClient`。

### API-e06d18584310 · encode_upload_lease_ack

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 46 行。

```python
def encode_upload_lease_ack(lease: ArtifactUploadLease) -> bytes:
```

原始接口说明：

```text
Encode one provider-issued upload lease for an authenticated ACK.
```

### API-45dcd2ff698d · decode_upload_lease_assignment

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 52 行。

```python
def decode_upload_lease_assignment(
    payload: bytes, *, now_ms: int
) -> ArtifactUploadLease:
```

原始接口说明：

```text
Decode and validate the exact provider-side Selection assignment.
```

### API-b113710b4907 · ArtifactStoreOffer

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 68 行。

```python
class ArtifactStoreOffer:
```

原始接口说明：

```text
Advisory ACK metadata; it does not reserve or lock repository capacity.
```

### API-6c1b2403e2f0 · ArtifactStoreOffer.queue_depth

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 71 行。

```python
queue_depth: int
```

### API-cb14bd1abc3e · ArtifactStoreOffer.queue_capacity

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 72 行。

```python
queue_capacity: int
```

### API-145f51d97b51 · ArtifactStoreOffer.available_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 73 行。

```python
available_bytes: int
```

### API-e16abba77d1f · ArtifactStoreOffer.max_artifact_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 74 行。

```python
max_artifact_bytes: int
```

### API-3e88ad2a9bc1 · ArtifactStoreAssignment

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 78 行。

```python
class ArtifactStoreAssignment:
```

原始接口说明：

```text
Exact store task delivered only after ACK_CLOSED plan selection.
```

### API-72fb795b5b0b · ArtifactStoreAssignment.task_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 81 行。

```python
task_id: str
```

### API-016fec8e77a0 · ArtifactStoreAssignment.operation_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 82 行。

```python
operation_id: str
```

### API-7310b201903a · ArtifactStoreAssignment.repo_node

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 83 行。

```python
repo_node: str
```

### API-1727496ec6ba · ArtifactStoreAssignment.artifact

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 84 行。

```python
artifact: ArtifactReference
```

### API-2fd9196b172b · ArtifactStoreAssignment.source_root_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 85 行。

```python
source_root_name: str = ""
```

### API-d0ec430e66f0 · ArtifactStoreAssignment.source_page_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 86 行。

```python
source_page_name: str = ""
```

### API-9aa5cb1b93ae · ArtifactStoreAssignment.source_payload_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 87 行。

```python
source_payload_name: str = ""
```

### API-f8606b76c1a8 · ArtifactStoreAssignment.publisher_key_pem

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 88 行。

```python
publisher_key_pem: str = ""
```

### API-2b56fd0d8a4f · ArtifactStoreAssignment.publisher_key_locator

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 89 行。

```python
publisher_key_locator: str = ""
```

### API-d38a5585b023 · ArtifactStoreAssignment.packet_payload_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 90 行。

```python
packet_payload_bytes: int = 0
```

### API-6f89c11fa3af · ArtifactStoreAssignment.manifest_page_encoded_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 91 行。

```python
manifest_page_encoded_bytes: int = 0
```

### API-26d9c048c728 · ArtifactStoreAssignment.receipt_scope

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 92 行。

```python
receipt_scope: str = ""
```

### API-1d15bafb8111 · ArtifactStoreAssignment.receipt_topic

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 93 行。

```python
receipt_topic: str = ""
```

### API-24b0a79da82d · ArtifactStoreAssignment.coordinator_role

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 94 行。

```python
coordinator_role: str = ""
```

### API-bbb4742a9fd5 · ArtifactStoreAssignment.requested_replicas

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 95 行。

```python
requested_replicas: int = 1
```

### API-c254803dd3c2 · ReplicaTaskControlSnapshot

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 99 行。

```python
class ReplicaTaskControlSnapshot:
```

### API-42c50c0152f0 · ReplicaTaskControlSnapshot.state

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 100 行。

```python
state: str
```

### API-1d8a1c9fa87e · ReplicaTaskControlSnapshot.request_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 101 行。

```python
request_id: str
```

### API-c1757f5fd7b1 · ReplicaTaskControlSnapshot.candidate_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 102 行。

```python
candidate_count: int
```

### API-6ee7bf1ee30a · ReplicaTaskControlSnapshot.selected_repo_nodes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 103 行。

```python
selected_repo_nodes: tuple[str, ...]
```

### API-121dab4c835f · ReplicaTaskControlSnapshot.control_operation_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 104 行。

```python
control_operation_count: int
```

### API-f3734e85f79c · encode_store_offer_ack

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 107 行。

```python
def encode_store_offer_ack(offer: ArtifactStoreOffer) -> bytes:
```

### API-802c9c61bc2c · decode_store_offer_ack

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 126 行。

```python
def decode_store_offer_ack(payload: bytes) -> ArtifactStoreOffer:
```

### API-6b212f0eee45 · decode_store_assignment

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 225 行。

```python
def decode_store_assignment(payload: bytes) -> ArtifactStoreAssignment:
```

### API-5ab3747a8acc · PendingReplicaTaskCollaboration

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 293 行。

```python
class PendingReplicaTaskCollaboration:
```

原始接口说明：

```text
One store task selection; positive ACKs are offers, never reservations.
```

### API-89b0b2d67d1e · PendingReplicaTaskCollaboration.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 296 行。

```python
def __init__(
        self,
        invocation,
        service_name: str,
        artifact: ArtifactReference,
        requested_replicas: int,
        operation_id: str,
        service_user=None,
    ) -> None:
```

### API-9a4ee99a7dd3 · PendingReplicaTaskCollaboration.acks_closed

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 314 行。

```python
def acks_closed(self, timeout_ms: int | None = None):
```

### API-31a675eac267 · PendingReplicaTaskCollaboration.commit_ack_tasks

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 319 行。

```python
def commit_ack_tasks(
        self, transfer_descriptor: dict[str, Any] | None = None
    ) -> tuple[str, ...]:
```

### API-6db75ecc2e7d · PendingReplicaTaskCollaboration.snapshot

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 441 行。

```python
def snapshot(self) -> ReplicaTaskControlSnapshot:
```

### API-d3b313780bac · PendingReplicaTaskCollaboration.result

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 455 行。

```python
def result(self, timeout_ms: int | None = None):
```

### API-1e974907121c · ReplicaTaskCollaborationClient

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 459 行。

```python
class ReplicaTaskCollaborationClient:
```

原始接口说明：

```text
Start one delayed-planning store-task collaboration per artifact.
```

### API-7eb7a63155a1 · ReplicaTaskCollaborationClient.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 462 行。

```python
def __init__(self, service_user, service_name: str) -> None:
```

### API-321cb9a69705 · ReplicaTaskCollaborationClient.begin

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 468 行。

```python
def begin(
        self,
        artifact: ArtifactReference,
        *,
        requested_replicas: int,
        operation_id: str,
        ack_timeout_ms: int = 300,
        timeout_ms: int = 30000,
        request_id: str = "",
    ) -> PendingReplicaTaskCollaboration:
```

### API-24e108fb59e5 · PendingReplicaLeaseCollaboration

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 506 行。

```python
class PendingReplicaLeaseCollaboration:
```

原始接口说明：

```text
One durable NDNSF invocation from Request through lease Selection.
```

### API-1f4faee30e5d · PendingReplicaLeaseCollaboration.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 509 行。

```python
def __init__(
        self,
        invocation,
        service_name: str,
        artifact: ArtifactReference,
        requested_replicas: int,
        operation_id: str,
    ) -> None:
```

### API-d19c9759ca58 · PendingReplicaLeaseCollaboration.acks_closed

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 526 行。

```python
def acks_closed(self, timeout_ms: int | None = None):
```

### API-4f5f2e7b3d5a · PendingReplicaLeaseCollaboration.commit_leases

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 532 行。

```python
def commit_leases(
        self,
        leases: Iterable[ArtifactUploadLease],
        *,
        now_ms: int,
    ) -> bool:
```

### API-f0bf1038fae5 · PendingReplicaLeaseCollaboration.leases_from_acks

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 600 行。

```python
def leases_from_acks(self, *, now_ms: int) -> tuple[ArtifactUploadLease, ...]:
```

原始接口说明：

```text
Validate provider-issued ACK leases and return a deterministic subset.
```

### API-ceedc24d4727 · PendingReplicaLeaseCollaboration.commit_ack_leases

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 642 行。

```python
def commit_ack_leases(self, *, now_ms: int) -> tuple[ArtifactUploadLease, ...]:
```

### API-feed41bcb3b6 · PendingReplicaLeaseCollaboration.snapshot

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 648 行。

```python
def snapshot(self):
```

### API-7f10b39be04c · PendingReplicaLeaseCollaboration.result

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 651 行。

```python
def result(self, timeout_ms: int | None = None):
```

### API-47083a401efb · ReplicaLeaseCollaborationClient

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 655 行。

```python
class ReplicaLeaseCollaborationClient:
```

原始接口说明：

```text
Start one delayed-planning NDNSF collaboration per artifact operation.
```

### API-2f8cb0ce4517 · ReplicaLeaseCollaborationClient.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 658 行。

```python
def __init__(self, service_user, service_name: str) -> None:
```

### API-978db7f54c51 · ReplicaLeaseCollaborationClient.begin

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py`，第 664 行。

```python
def begin(
        self,
        artifact: ArtifactReference,
        *,
        requested_replicas: int,
        operation_id: str,
        ack_timeout_ms: int = 300,
        timeout_ms: int = 30000,
        request_id: str = "",
    ) -> PendingReplicaLeaseCollaboration:
```

## NDNSF-DistributedRepo/pythonWrapper/py_repoclient/local_artifact_backend.py

源码 SHA-256：`385037d57795eeb5d4dbde0cc340e72309c294b4031413e9600d292ee56f62e2`。

显式导出：`FilesystemArtifactApiBackend`。

### API-b9105586bfd9 · FilesystemArtifactApiBackend

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/local_artifact_backend.py`，第 383 行。

```python
class FilesystemArtifactApiBackend:
```

原始接口说明：

```text
Public single-replica backend for local trusted-process use.
```

### API-f1928303ad0e · FilesystemArtifactApiBackend.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/local_artifact_backend.py`，第 386 行。

```python
def __init__(
        self,
        root: Path,
        *,
        repo_node: str = "/local/filesystem-repo",
    ) -> None:
```

### API-909ccf6627e6 · FilesystemArtifactApiBackend.begin_publish

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/local_artifact_backend.py`，第 407 行。

```python
def begin_publish(
        self, descriptor: ArtifactDescriptor, operation_id: str, emit_progress
    ) -> _LocalPublishDriver:
```

### API-83e6c5f2e5a0 · FilesystemArtifactApiBackend.begin_fetch

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/local_artifact_backend.py`，第 428 行。

```python
def begin_fetch(
        self,
        reference,
        destination,
        operation_id,
        *,
        resume,
        verify,
        replace,
        timeout_ms,
        control,
        emit_progress,
    ) -> _LocalFetchDriver:
```

## NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py

源码 SHA-256：`140a9e4cfa0f2ebb32957b1175b8b8f075def481bccfc23d49c7ade450828e5b`。

显式导出：`ArtifactControlMetrics`, `CollaborationArtifactApiBackend`, `install_artifact_collaboration_service`。

### API-30f678443176 · ArtifactControlMetrics

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 139 行。

```python
class ArtifactControlMetrics:
```

### API-ddac3de07240 · ArtifactControlMetrics.request_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 140 行。

```python
request_count: int
```

### API-7f1307cc12b1 · ArtifactControlMetrics.ack_closed_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 141 行。

```python
ack_closed_count: int
```

### API-6756215d4ad7 · ArtifactControlMetrics.selection_commit_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 142 行。

```python
selection_commit_count: int
```

### API-849175983a87 · ArtifactControlMetrics.response_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 143 行。

```python
response_count: int
```

### API-7ccba1bca58d · ArtifactControlMetrics.selected_replicas

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 144 行。

```python
selected_replicas: int
```

### API-9f2b8fdf2d04 · ArtifactControlMetrics.elapsed_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 145 行。

```python
elapsed_ms: float
```

### API-6c59b6e60ef1 · ArtifactControlMetrics.control_operation_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 148 行。

```python
def control_operation_count(self) -> int:
```

### API-ba1c5b1182f4 · ArtifactControlMetrics.lifecycle_phase_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 152 行。

```python
def lifecycle_phase_count(self) -> int:
```

### API-b7aeaae5aa66 · CollaborationArtifactApiBackend

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 771 行。

```python
class CollaborationArtifactApiBackend:
```

原始接口说明：

```text
One collaboration and one segmented transfer per immutable artifact.

``delegate=None`` selects the production whole-artifact network path.
Supplying a delegate retains the pre-T034 compatibility adapter.
```

### API-c0494a8e4c53 · CollaborationArtifactApiBackend.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 778 行。

```python
def __init__(
        self,
        delegate: ArtifactApiBackend | None,
        service_user,
        service_name: str,
        *,
        ack_timeout_ms: int = 300,
        packet_payload_bytes: int = _DEFAULT_PACKET_PAYLOAD_BYTES,
        chunk_bytes: int = _DEFAULT_CHUNK_BYTES,
        committed_receipts: tuple[dict[str, Any], ...] = (),
        receipt_store_path: str | Path | None = None,
        clock_ms=None,
    ) -> None:
```

### API-7e575da1c70c · CollaborationArtifactApiBackend.close

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 852 行。

```python
def close(self) -> None:
```

原始接口说明：

```text
Stop and release the native control-plane client.

The backend owns the ``ServiceUser`` created by :meth:`from_config`.
Calling only ``ServiceUser.stop()`` leaves that native object retained
until Python interpreter teardown, where its worker pools and Face can
keep a short-lived publisher process alive after it has printed its
success marker.  Close is idempotent and drops the ownership edge so
the native destructor runs before the process returns.
```

### API-3f9708ce5099 · CollaborationArtifactApiBackend.from_config

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 874 行。

```python
def from_config(
        cls,
        config: str | Path,
        *,
        generated_policy_dir: str | Path,
        state_root: str | Path,
        user: str,
        service_name: str = "/NDNSF/DistributedRepo/Artifact/v2/STORE",
        bootstrap_token: str = "",
        ack_timeout_ms: int = 3000,
        packet_payload_bytes: int = _DEFAULT_PACKET_PAYLOAD_BYTES,
        chunk_bytes: int = _DEFAULT_CHUNK_BYTES,
        committed_receipts: tuple[dict[str, Any], ...] = (),
        test_only_allow_ephemeral_state_root: bool = False,
    ) -> "CollaborationArtifactApiBackend":
```

原始接口说明：

```text
Construct the public artifact transport from one deployment file.

A volatile state root is accepted only when an explicit test caller
opts in. Production callers retain the persistent-journal safety gate.
```

### API-055125b346e1 · CollaborationArtifactApiBackend.begin_publish

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 944 行。

```python
def begin_publish(
        self, descriptor: ArtifactDescriptor, operation_id: str, emit_progress
    ):
```

### API-14db2dd01688 · CollaborationArtifactApiBackend.begin_fetch

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 958 行。

```python
def begin_fetch(self, *args, **kwargs):
```

### API-7e9625e47b19 · install_artifact_collaboration_service

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py`，第 1270 行。

```python
def install_artifact_collaboration_service(
    repo_app,
    service_name: str | None = None,
    *,
    queue_capacity: int = 4,
) -> str:
```

原始接口说明：

```text
Register the queued whole-artifact service on one RepoNodeApp.
```

## NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py

源码 SHA-256：`2cc685017ee7f33a2f7ef819db2088c805121c87f22a5c496f84a35d23fe6095`。

### API-23b6e35c5b6b · WriteConsistency

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 100 行。

```python
class WriteConsistency(str, Enum):
```

### API-b5d4ffdb5427 · RepoOperationMetrics

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 166 行。

```python
class RepoOperationMetrics:
```

原始接口说明：

```text
Canonical per-operation evidence shared by every Spec 164 gate.
```

### API-31149e7f5188 · RepoOperationMetrics.operation_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 169 行。

```python
operation_id: str
```

### API-ff839afbbc39 · RepoOperationMetrics.started_at_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 170 行。

```python
started_at_ms: int = 0
```

### API-ce3c9f79e4b1 · RepoOperationMetrics.completed_at_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 171 行。

```python
completed_at_ms: int = 0
```

### API-4b6091e929c7 · RepoOperationMetrics.phase_timings_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 172 行。

```python
phase_timings_ms: dict[str, float] = field(default_factory=dict)
```

### API-38cad820bcde · RepoOperationMetrics.logical_payload_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 173 行。

```python
logical_payload_bytes: int = 0
```

### API-8aaa2635318a · RepoOperationMetrics.data_wire_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 174 行。

```python
data_wire_bytes: int = 0
```

### API-51ff6d56ff8c · RepoOperationMetrics.interest_wire_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 175 行。

```python
interest_wire_bytes: int = 0
```

### API-b319ba46f77f · RepoOperationMetrics.wire_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 176 行。

```python
wire_bytes: int = 0
```

### API-3494aeefa8ef · RepoOperationMetrics.retransmitted_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 177 行。

```python
retransmitted_bytes: int = 0
```

### API-4875b5c84295 · RepoOperationMetrics.payload_store_bytes_read

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 178 行。

```python
payload_store_bytes_read: int = 0
```

### API-3db127f72653 · RepoOperationMetrics.payload_store_bytes_written

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 179 行。

```python
payload_store_bytes_written: int = 0
```

### API-5a90bfc8b6c7 · RepoOperationMetrics.metadata_store_bytes_read

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 180 行。

```python
metadata_store_bytes_read: int = 0
```

### API-76401e695881 · RepoOperationMetrics.metadata_store_bytes_written

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 181 行。

```python
metadata_store_bytes_written: int = 0
```

### API-b81246df84b2 · RepoOperationMetrics.storage_bytes_read

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 182 行。

```python
storage_bytes_read: int = 0
```

### API-4610fcfeeaff · RepoOperationMetrics.storage_bytes_written

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 183 行。

```python
storage_bytes_written: int = 0
```

### API-df4f51650274 · RepoOperationMetrics.asymmetric_verifications

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 184 行。

```python
asymmetric_verifications: int = 0
```

### API-163c67801223 · RepoOperationMetrics.digest_verifications

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 185 行。

```python
digest_verifications: int = 0
```

### API-4730de05798f · RepoOperationMetrics.asymmetric_verification_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 186 行。

```python
asymmetric_verification_ms: float = 0.0
```

### API-07b940c5cc5a · RepoOperationMetrics.digest_verification_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 187 行。

```python
digest_verification_ms: float = 0.0
```

### API-351a597e6041 · RepoOperationMetrics.control_operations

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 188 行。

```python
control_operations: int = 0
```

### API-9db2e0f99855 · RepoOperationMetrics.metadata_operations

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 189 行。

```python
metadata_operations: int = 0
```

### API-7d09428a7b8c · RepoOperationMetrics.metadata_record_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 190 行。

```python
metadata_record_count: int = 0
```

### API-00e1fd0b5a7e · RepoOperationMetrics.requested_replica_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 191 行。

```python
requested_replica_count: int = 0
```

### API-88bb16f1bca3 · RepoOperationMetrics.selected_replica_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 192 行。

```python
selected_replica_count: int = 0
```

### API-a93efffe6cd3 · RepoOperationMetrics.committed_replica_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 193 行。

```python
committed_replica_count: int = 0
```

### API-0037e2491a27 · RepoOperationMetrics.rejected_replica_receipt_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 194 行。

```python
rejected_replica_receipt_count: int = 0
```

### API-420a738fe986 · RepoOperationMetrics.record_phase

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 262 行。

```python
def record_phase(self, phase: str, elapsed_ms: float) -> None:
```

### API-3155a95749f8 · RepoOperationMetrics.increment

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 269 行。

```python
def increment(self, field_name: str, amount: int = 1) -> None:
```

### API-854840f69dac · RepoOperationMetrics.to_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 289 行。

```python
def to_dict(self) -> dict[str, object]:
```

### API-0209ed6ff52e · RepoOperationMetrics.from_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 321 行。

```python
def from_dict(obj: dict) -> "RepoOperationMetrics":
```

### API-4d3bf5c491d8 · normalize_write_consistency

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 363 行。

```python
def normalize_write_consistency(value: str | WriteConsistency) -> str:
```

### API-85533b63446f · required_write_acks

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 370 行。

```python
def required_write_acks(replication_factor: int,
                        consistency: str | WriteConsistency) -> int:
```

### API-236dea3f40f0 · normalize_repo_operation_state

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 383 行。

```python
def normalize_repo_operation_state(value: str) -> str:
```

### API-80d23e1abb38 · RepoWriteIntent

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 391 行。

```python
class RepoWriteIntent:
```

### API-da994911c74e · RepoWriteIntent.operation_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 392 行。

```python
operation_id: str
```

### API-620feeda8a6e · RepoWriteIntent.object_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 393 行。

```python
object_name: str
```

### API-c061f51c450c · RepoWriteIntent.generation

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 394 行。

```python
generation: int
```

### API-7c5c16daca32 · RepoWriteIntent.digest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 395 行。

```python
digest: str
```

### API-60433739056c · RepoWriteIntent.replication_factor

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 396 行。

```python
replication_factor: int
```

### API-2341621ccba8 · RepoWriteIntent.required_acks

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 397 行。

```python
required_acks: int = 0
```

### API-25f0556d0c59 · RepoWriteIntent.consistency

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 398 行。

```python
consistency: str = WriteConsistency.ALL.value
```

### API-92e5c5b68423 · RepoWriteIntent.expected_generation

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 399 行。

```python
expected_generation: int = -1
```

### API-8a0194de8864 · RepoWriteIntent.selected_replicas

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 400 行。

```python
selected_replicas: tuple[str, ...] = ()
```

### API-4bcff16eedac · RepoWriteIntent.state

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 401 行。

```python
state: str = "RECEIVED"
```

### API-2f38329ef0f0 · RepoWriteIntent.created_at_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 402 行。

```python
created_at_ms: int = 0
```

### API-61d8343fd2b6 · RepoWriteIntent.updated_at_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 403 行。

```python
updated_at_ms: int = 0
```

### API-def3a2ad654e · RepoWriteIntent.to_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 419 行。

```python
def to_dict(self) -> dict[str, object]:
```

### API-c0e84bfd0434 · RepoWriteIntent.from_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 436 行。

```python
def from_dict(obj: dict) -> "RepoWriteIntent":
```

### API-e032718a8611 · RepoWriteReceipt

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 454 行。

```python
class RepoWriteReceipt:
```

### API-9c4d6ef4828e · RepoWriteReceipt.operation_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 455 行。

```python
operation_id: str
```

### API-dfdf318a840a · RepoWriteReceipt.repo_node

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 456 行。

```python
repo_node: str
```

### API-2070bb510ca1 · RepoWriteReceipt.object_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 457 行。

```python
object_name: str
```

### API-3842ef953dc0 · RepoWriteReceipt.generation

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 458 行。

```python
generation: int
```

### API-c49be4ed1c44 · RepoWriteReceipt.digest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 459 行。

```python
digest: str
```

### API-e2d9cac38bfc · RepoWriteReceipt.persisted_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 460 行。

```python
persisted_bytes: int
```

### API-52648da8d835 · RepoWriteReceipt.state

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 461 行。

```python
state: str = "COMMITTED"
```

### API-9036c62c0350 · RepoWriteReceipt.completed_at_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 462 行。

```python
completed_at_ms: int = 0
```

### API-7b56427a4afa · RepoWriteReceipt.to_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 471 行。

```python
def to_dict(self) -> dict[str, object]:
```

### API-92a3c37510d5 · RepoWriteReceipt.from_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 484 行。

```python
def from_dict(obj: dict) -> "RepoWriteReceipt":
```

### API-4f5ee55479e6 · RepoIncompleteWriteError

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 497 行。

```python
class RepoIncompleteWriteError(RuntimeError):
```

### API-da45b5fc08e7 · RepoIncompleteWriteError.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 498 行。

```python
def __init__(self, intent: RepoWriteIntent,
                 receipts: Iterable[RepoWriteReceipt],
                 failures: Optional[dict[str, str]] = None) -> None:
```

### API-000c40eae624 · RepoIncompleteWriteError.to_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 511 行。

```python
def to_dict(self) -> dict[str, object]:
```

### API-f42c8a549329 · validate_write_receipts

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 521 行。

```python
def validate_write_receipts(
    intent: RepoWriteIntent,
    receipts: Iterable[RepoWriteReceipt],
    *,
    failures: Optional[dict[str, str]] = None,
) -> tuple[RepoWriteReceipt, ...]:
```

### API-027b68c3a53e · REPO_OBJECT_CLASS_DEFAULTS

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 616 行。

```python
REPO_OBJECT_CLASS_DEFAULTS: dict[str, dict[str, object]] = {
    "temporary-activation": {
        "minReplicationFactor": 1,
        "maxReplicationFactor": 1,
        "ttlMs": 10 * 60 * 1000,
        "repairAllowed": False,
    },
    "model-artifact": {
        "minReplicationFactor": 2,
        "maxReplicationFactor": 3,
        "ttlMs": 0,
        "repairAllowed": True,
    },
    "uav-recording": {
        "minReplicationFactor": 2,
        "maxReplicationFactor": 3,
        "ttlMs": 7 * 24 * 60 * 60 * 1000,
        "repairAllowed": True,
    },
    "telemetry-log": {
        "minReplicationFactor": 1,
        "maxReplicationFactor": 2,
        "ttlMs": 7 * 24 * 60 * 60 * 1000,
        "repairAllowed": True,
    },
    "mission-log": {
        "minReplicationFactor": 2,
        "maxReplicationFactor": 3,
        "ttlMs": 30 * 24 * 60 * 60 * 1000,
        "repairAllowed": True,
    },
}
```

### API-a867a6495e2f · REPO_OBJECT_CLASS_POLICIES

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 648 行。

```python
REPO_OBJECT_CLASS_POLICIES: dict[str, dict[str, object]] = {
    name: dict(policy)
    for name, policy in REPO_OBJECT_CLASS_DEFAULTS.items()
}
```

### API-1713ceda49e0 · configure_repo_object_class_policies

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 684 行。

```python
def configure_repo_object_class_policies(config: dict) -> None:
```

原始接口说明：

```text
Install deployment-specific object class policies for this process.
```

### API-49b6a1e7adc9 · repo_object_class_policy

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 720 行。

```python
def repo_object_class_policy(object_type: str, object_class: str = "") -> dict[str, object]:
```

原始接口说明：

```text
Return default lifecycle/replication metadata for a repo object class.
```

### API-61d0a9a3a0e0 · RepoObjectManifest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 751 行。

```python
class RepoObjectManifest:
```

### API-49a8e769df96 · RepoObjectManifest.object_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 752 行。

```python
object_name: str
```

### API-7c8738a7c44a · RepoObjectManifest.object_type

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 753 行。

```python
object_type: str
```

### API-168a5ea171ef · RepoObjectManifest.sha256

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 754 行。

```python
sha256: str
```

### API-78fe4dfad372 · RepoObjectManifest.size

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 755 行。

```python
size: int
```

### API-dd0c24e5e3e1 · RepoObjectManifest.segment_count

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 756 行。

```python
segment_count: int = 1
```

### API-2e65f32549cd · RepoObjectManifest.replication_factor

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 757 行。

```python
replication_factor: int = 1
```

### API-904bee35e418 · RepoObjectManifest.min_replication_factor

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 760 行。

```python
min_replication_factor: int = 0
```

### API-8817230aa1bf · RepoObjectManifest.max_replication_factor

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 761 行。

```python
max_replication_factor: int = 0
```

### API-18a834538423 · RepoObjectManifest.replica_nodes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 762 行。

```python
replica_nodes: tuple[str, ...] = ()
```

### API-932df31fbada · RepoObjectManifest.replica_data_names

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 763 行。

```python
replica_data_names: tuple[str, ...] = ()
```

### API-19e8850758a7 · RepoObjectManifest.packet_names

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 764 行。

```python
packet_names: tuple[str, ...] = ()
```

### API-fbedd8d15344 · RepoObjectManifest.segment_locations

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 765 行。

```python
segment_locations: tuple[dict, ...] = ()
```

### API-5a1c4629ad2d · RepoObjectManifest.policy_epoch

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 766 行。

```python
policy_epoch: str = ""
```

### API-fe27e6416a9f · RepoObjectManifest.object_class

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 767 行。

```python
object_class: str = ""
```

### API-ecdb17dec7e5 · RepoObjectManifest.ttl_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 768 行。

```python
ttl_ms: int = 0
```

### API-82258c948749 · RepoObjectManifest.repair_allowed

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 769 行。

```python
repair_allowed: bool = True
```

### API-3d7fd89ed322 · RepoObjectManifest.auto_delete

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 770 行。

```python
auto_delete: bool = False
```

### API-d8dc5332276e · RepoObjectManifest.delete_policy

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 771 行。

```python
delete_policy: str = ""
```

### API-133e63b7f6f4 · RepoObjectManifest.priority

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 772 行。

```python
priority: int = 0
```

### API-8e9c23ab2b41 · RepoObjectManifest.metadata

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 773 行。

```python
metadata: dict = field(default_factory=dict)
```

### API-97aca0878f4b · RepoObjectManifest.generation

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 774 行。

```python
generation: int = 0
```

### API-832f0aaf7f3b · RepoObjectManifest.parent_generation

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 775 行。

```python
parent_generation: int = -1
```

### API-eb6bab801f48 · RepoObjectManifest.write_consistency

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 776 行。

```python
write_consistency: str = WriteConsistency.ALL.value
```

### API-de3443c5c83d · RepoObjectManifest.required_write_acks

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 777 行。

```python
required_write_acks: int = 0
```

### API-e380dc4e9b69 · RepoObjectManifest.confirmed_replica_nodes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 778 行。

```python
confirmed_replica_nodes: tuple[str, ...] = ()
```

### API-1cf4643c081d · RepoObjectManifest.operation_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 779 行。

```python
operation_id: str = ""
```

### API-38b953170a70 · RepoObjectManifest.lifecycle_state

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 780 行。

```python
lifecycle_state: str = "COMMITTED"
```

### API-632701159886 · RepoObjectManifest.to_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 800 行。

```python
def to_dict(self) -> dict:
```

### API-ea24d4e65799 · RepoObjectManifest.to_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 864 行。

```python
def to_bytes(self) -> bytes:
```

### API-6dd3c909315e · RepoObjectManifest.from_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 868 行。

```python
def from_dict(obj: dict) -> "RepoObjectManifest":
```

### API-dfdf7abd46d3 · RepoRepairAction

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 912 行。

```python
class RepoRepairAction:
```

原始接口说明：

```text
Validated catalog repair action.

The wire/catalog shape remains a JSON object, but this class gives the
control plane a typed schema boundary before a sidecar executes repair.
```

### API-9529a700438a · RepoRepairAction.object_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 919 行。

```python
object_name: str
```

### API-0ae19b6731b8 · RepoRepairAction.object_sha256

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 920 行。

```python
object_sha256: str
```

### API-bdbee52f4b70 · RepoRepairAction.manifest_sha256

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 921 行。

```python
manifest_sha256: str
```

### API-7084bc32100d · RepoRepairAction.source_repo

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 922 行。

```python
source_repo: str
```

### API-6910bee25284 · RepoRepairAction.target_repo

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 923 行。

```python
target_repo: str
```

### API-13d180839e8a · RepoRepairAction.min_replication_factor

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 924 行。

```python
min_replication_factor: int
```

### API-8b2e76317a7b · RepoRepairAction.max_replication_factor

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 925 行。

```python
max_replication_factor: int
```

### API-274f4b0a28c4 · RepoRepairAction.reason

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 926 行。

```python
reason: str = "under-replicated"
```

### API-5d994a9d51eb · RepoRepairAction.action_type

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 927 行。

```python
action_type: str = "copy-replica"
```

### API-18d412f74b05 · RepoRepairAction.schema_version

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 928 行。

```python
schema_version: int = 1
```

### API-92c62a110e20 · RepoRepairAction.to_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 930 行。

```python
def to_dict(self) -> dict:
```

### API-a0c9d2c9fc6a · RepoRepairAction.from_dict

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 945 行。

```python
def from_dict(obj: dict, *, target_repo_node: str = "") -> "RepoRepairAction":
```

### API-0f35c0071229 · large_data_reference_from_repo_manifest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1001 行。

```python
def large_data_reference_from_repo_manifest(
    manifest: RepoObjectManifest | dict,
    *,
    object_type: str = "",
    object_id: str = "",
) -> dict:
```

原始接口说明：

```text
Return the generic large-object reference metadata for a repo manifest.
```

### API-56b9e620a733 · repo_artifact_reference

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1021 行。

```python
def repo_artifact_reference(
    manifest: RepoObjectManifest | dict,
    *,
    object_type: str = "",
    object_id: str = "",
) -> dict:
```

原始接口说明：

```text
Wrap a repo manifest with explicit large-data reference metadata.
```

### API-46de92b60f1d · repo_manifest_from_artifact_reference

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1040 行。

```python
def repo_manifest_from_artifact_reference(entry: dict) -> dict:
```

原始接口说明：

```text
Extract the repo manifest from a new or legacy artifact manifest entry.
```

### API-bc0d898264af · repo_manifest_from_large_data_reference

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1077 行。

```python
def repo_manifest_from_large_data_reference(entry: dict) -> dict:
```

原始接口说明：

```text
Resolve a repo-backed artifact through the large-data reference layer.

New planner/executor code should call this helper instead of directly
reading ``repoManifest``. The implementation still accepts legacy manifest
shapes so older generated policies keep working during migration.
```

### API-e69b5ee22d6b · StorageCapability

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1089 行。

```python
class StorageCapability:
```

### API-1cd4e8a4d320 · StorageCapability.repo_node

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1090 行。

```python
repo_node: str
```

### API-66b4ace8c86e · StorageCapability.free_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1091 行。

```python
free_bytes: int
```

### API-cb625a5205e8 · StorageCapability.used_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1092 行。

```python
used_bytes: int = 0
```

### API-f605476464fa · StorageCapability.recent_load

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1093 行。

```python
recent_load: float = 0.0
```

### API-1e2cd29219c3 · StorageCapability.availability_score

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1094 行。

```python
availability_score: float = 1.0
```

### API-f2db76bf439b · StorageCapability.failure_domain

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1095 行。

```python
failure_domain: str = ""
```

### API-d0170ed3133d · StorageCapability.storage_classes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1096 行。

```python
storage_classes: tuple[str, ...] = ("model", "intermediate")
```

### API-415bcd5baf35 · StorageCapability.repo_mode

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1097 行。

```python
repo_mode: str = "persistent"
```

### API-5e02f8e00ab8 · StorageCapability.accepts_backup_replica

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1098 行。

```python
accepts_backup_replica: bool = True
```

### API-cb7e7bac3fec · StorageCapability.queue_depth

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1099 行。

```python
queue_depth: int = 0
```

### API-40bd992658b8 · StorageCapability.inflight_operations

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1100 行。

```python
inflight_operations: int = 0
```

### API-71e4ce77192d · StorageCapability.storage_latency_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1101 行。

```python
storage_latency_ms: float = 0.0
```

### API-63935fd72786 · StorageCapability.network_rtt_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1102 行。

```python
network_rtt_ms: float = 0.0
```

### API-5810727bf8e7 · StorageCapability.network_bandwidth_mbps

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1103 行。

```python
network_bandwidth_mbps: float = 0.0
```

### API-202654982d72 · StorageCapability.artifact_format_versions

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1104 行。

```python
artifact_format_versions: tuple[str, ...] = ("exact-packet-v1",)
```

### API-75a09332f5e0 · StorageCapability.artifact_digest_algorithms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1105 行。

```python
artifact_digest_algorithms: tuple[str, ...] = ("sha256",)
```

### API-949226171eb4 · StorageCapability.artifact_signature_algorithms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1106 行。

```python
artifact_signature_algorithms: tuple[str, ...] = (
        "rsa-sha256", "ecdsa-sha256", "ed25519",
    )
```

### API-9c898dc93e13 · StorageCapability.artifact_max_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1109 行。

```python
artifact_max_bytes: int = 1 << 50
```

### API-0fcc6386f27f · StorageCapability.artifact_max_chunk_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1110 行。

```python
artifact_max_chunk_bytes: int = 64 * 1024 * 1024
```

### API-3a3614fb5a36 · StorageCapability.artifact_max_root_encoded_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1111 行。

```python
artifact_max_root_encoded_bytes: int = 64 * 1024
```

### API-e7f5eea1bdad · StorageCapability.artifact_max_page_encoded_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1112 行。

```python
artifact_max_page_encoded_bytes: int = 4 * 1024 * 1024
```

### API-690005290387 · StorageCapability.artifact_max_page_entries

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1113 行。

```python
artifact_max_page_entries: int = 65536
```

### API-4c4446fd5314 · StorageCapability.artifact_max_manifest_depth

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1114 行。

```python
artifact_max_manifest_depth: int = 16
```

### API-6adb51754924 · StorageCapability.artifact_supports_resume

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1115 行。

```python
artifact_supports_resume: bool = False
```

### API-617d09cda49a · StorageCapability.artifact_supports_replica_receipts

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1116 行。

```python
artifact_supports_replica_receipts: bool = False
```

### API-74b6010d2664 · StorageCapability.artifact_policy_epoch

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1117 行。

```python
artifact_policy_epoch: str = "default"
```

### API-0c98a9d739cd · PlacementPolicy

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1149 行。

```python
class PlacementPolicy:
```

### API-287a7347b23a · PlacementPolicy.replication_factor

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1150 行。

```python
replication_factor: int = 1
```

### API-01841a960ce3 · PlacementPolicy.avoid_same_failure_domain

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1151 行。

```python
avoid_same_failure_domain: bool = True
```

### API-1aa700b774d4 · PlacementPolicy.prefer_low_load

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1152 行。

```python
prefer_low_load: bool = True
```

### API-ba35dd6aecdf · PlacementPolicy.prefer_high_availability

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1153 行。

```python
prefer_high_availability: bool = True
```

### API-7e5d63544fd8 · RepoPlacement

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1157 行。

```python
class RepoPlacement:
```

### API-b540642f0116 · RepoPlacement.object_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1158 行。

```python
object_name: str
```

### API-27e608d44e3d · RepoPlacement.replicas

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1159 行。

```python
replicas: tuple[StorageCapability, ...]
```

### API-782c1ee1d664 · RepoPlacement.replica_names

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1162 行。

```python
def replica_names(self) -> tuple[str, ...]:
```

### API-f8a014e547df · LocalDistributedRepo

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1199 行。

```python
class LocalDistributedRepo:
```

原始接口说明：

```text
Deterministic local repo-cluster planner used by examples and smoke tests.

The C++ DistributedRepo subproject owns the long-term repo-node service
implementation. This Python class mirrors its manifest and placement rules
so NDNSF-DI examples can already carry repo object references in plans and
validate store/fetch behavior before running a full NDNSF repo cluster.
```

### API-1c6063ca74b1 · LocalDistributedRepo.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1208 行。

```python
def __init__(self, capabilities: Iterable[StorageCapability]):
```

### API-d34f9b67db41 · LocalDistributedRepo.capabilities

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1215 行。

```python
def capabilities(self) -> tuple[StorageCapability, ...]:
```

### API-b7dffe07de22 · LocalDistributedRepo.objects

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1219 行。

```python
def objects(self) -> dict[str, tuple[RepoObjectManifest, bytes]]:
```

### API-1ecdd74d8057 · LocalDistributedRepo.put

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1226 行。

```python
def put(
        self,
        *,
        object_name: str,
        payload: bytes,
        object_type: str = "artifact",
        policy: PlacementPolicy = PlacementPolicy(),
        policy_epoch: str = "",
    ) -> RepoObjectManifest:
```

### API-7c13ac640d88 · LocalDistributedRepo.fetch

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1256 行。

```python
def fetch(self, object_name: str) -> bytes:
```

### API-b1d0dcd07014 · LocalDistributedRepo.get

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1273 行。

```python
def get(self, object_name: str) -> bytes:
```

### API-adb15b884b06 · LocalDistributedRepo.fetch_object

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1276 行。

```python
def fetch_object(
        self,
        object_name: str,
        manifest: RepoObjectManifest | None = None,
    ) -> bytes:
```

原始接口说明：

```text
Fetch one logical object and verify it against its manifest.
```

### API-b65ccd9d09a8 · LocalDistributedRepo.get_object

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1291 行。

```python
def get_object(
        self,
        object_name: str,
        manifest: RepoObjectManifest | None = None,
    ) -> bytes:
```

### API-ed1b2d7b9531 · LocalDistributedRepo.put_manifest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1298 行。

```python
def put_manifest(self, manifest: RepoObjectManifest) -> None:
```

### API-0336477cb4c2 · LocalDistributedRepo.erase

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1304 行。

```python
def erase(self, object_name: str) -> bool:
```

### API-f572a9161809 · LocalDistributedRepo.manifest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1315 行。

```python
def manifest(self, object_name: str) -> RepoObjectManifest:
```

### API-ce0b4c89c7ff · LocalDistributedRepo.inventory

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1321 行。

```python
def inventory(self, repo_node: str | None = None) -> dict[str, RepoObjectManifest]:
```

### API-681d61139a2c · LocalDistributedRepo.set_available

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1329 行。

```python
def set_available(self, repo_node: str, available: bool) -> None:
```

### API-75bf577c6e93 · encode_repo_request

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1333 行。

```python
def encode_repo_request(operation: str, **fields) -> bytes:
```

### API-9bf20fde7558 · decode_repo_request

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1340 行。

```python
def decode_repo_request(payload: bytes) -> dict:
```

### API-22e8ad1d47e7 · RepoNodeApp

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1541 行。

```python
class RepoNodeApp:
```

原始接口说明：

```text
Real NDNSF repo node using versioned public and peer-only services.
```

### API-e31c63815365 · RepoNodeApp.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 1544 行。

```python
def __init__(
        self,
        *,
        repo_node: str,
        service_name: str = "/NDNSF/DistributedRepo",
        provider_id: str = "",
        group: str = "/NDNSF-DistributeInference/example/group",
        controller: str = "/NDNSF-DistributeInference/example/controller",
        provider_prefix: str = "/NDNSF-DistributeInference/example/provider",
        trust_schema: str = "examples/trust-schema.conf",
        free_bytes: int = 4_000_000_000,
        failure_domain: str = "",
        storage_classes: tuple[str, ...] = ("model", "intermediate"),
        storage_dir: str | Path | None = None,
        memory_cache_bytes: int = 64 * 1024 * 1024,
        preallocate_bytes: int = 0,
        advertise_stored_prefixes: bool = False,
        advertise_command: str = "nlsrc",
        repo_mode: str = "persistent",
        accepts_backup_replica: bool = True,
        peer_repo_nodes: tuple[str, ...] = (),
        peer_provider_identities: tuple[str, ...] = (),
        catalog_sync_interval_s: float = 10.0,
        handler_threads: int = 4,
        ack_threads: int = 2,
        serve_certificates: bool = True,
        bootstrap_token: str = "",
        exact_data_validation_policy: str = "wire-name-and-request-digest",
        artifact_format_versions: tuple[str, ...] = ("exact-packet-v1",),
        artifact_policy_epoch: str = "default",
        artifact_supports_resume: bool = False,
        artifact_supports_replica_receipts: bool = False,
        artifact_writes_enabled: bool = True,
        artifact_max_write_schema_generation: int | None = None,
    ) -> None:
```

### API-b5d2746251b8 · RepoNodeApp.data_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 4277 行。

```python
def data_name(repo_node: str, object_name: str) -> str:
```

### API-77f7f0861624 · RepoNodeApp.object_data_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 4284 行。

```python
def object_data_name(object_name: str) -> str:
```

### API-58881e556bb0 · RepoNodeApp.run

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5437 行。

```python
def run(self) -> int:
```

### API-68d619ba427c · RepoNodeApp.seed_object

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5474 行。

```python
def seed_object(
        self,
        object_name: str,
        payload: bytes | bytearray | memoryview | str,
        *,
        object_type: str = "bootstrap-config",
        policy_epoch: str = "",
    ) -> RepoObjectManifest:
```

原始接口说明：

```text
Preload an object into this repo node before serving requests.
```

### API-da29c29af215 · NetworkDistributedRepoClient

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5506 行。

```python
class NetworkDistributedRepoClient:
```

原始接口说明：

```text
NDNSF client for a versioned-operation DistributedRepo cluster.
```

### API-c919ccece276 · NetworkDistributedRepoClient.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5509 行。

```python
def __init__(
        self,
        *,
        user: ServiceUser,
        service_name: str = "/NDNSF/DistributedRepo",
        upload_prefix: str = "/NDNSF-DistributeInference/example/user/NDNSF-DISTRIBUTED-REPO/UPLOAD",
        ack_timeout_ms: int = 500,
        timeout_ms: int = 10000,
        max_segment_payload: int = 4800,
        verbose: bool = False,
        max_store_batch_wire_bytes: int = 2500,
        pull_store_threshold_bytes: int = 65536,
        placement_cache_ttl_ms: int = 5000,
        replica_cooldown_ms: int = 3000,
        hedged_read_delay_ms: int = 0,
        control_mode: str = "targeted",
        enable_targeted_fallback: bool = True,
    ) -> None:
```

### API-41a70990c3ae · NetworkDistributedRepoClient.begin_operation_metrics

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5672 行。

```python
def begin_operation_metrics(self, operation_id: Optional[str] = None) -> str:
```

### API-a96d34d56e04 · NetworkDistributedRepoClient.operation_metrics

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5682 行。

```python
def operation_metrics(self) -> Optional[RepoOperationMetrics]:
```

### API-663eda228e8f · NetworkDistributedRepoClient.end_operation_metrics

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5687 行。

```python
def end_operation_metrics(self) -> dict[str, object]:
```

### API-cdcc96ed012a · NetworkDistributedRepoClient.control_metrics

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5696 行。

```python
def control_metrics(self) -> dict[str, int | float | str]:
```

### API-4bc6588f45b3 · NetworkDistributedRepoClient.reset_control_metrics

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5704 行。

```python
def reset_control_metrics(self) -> None:
```

原始接口说明：

```text
Reset measured counters after bootstrap or experiment warmup.
```

### API-1b81c2cf9a17 · NetworkDistributedRepoClient.close

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5712 行。

```python
def close(self) -> None:
```

### API-583204ddc6d8 · NetworkDistributedRepoClient.publisher_namespace

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5838 行。

```python
def publisher_namespace(self) -> str:
```

### API-fff26808974c · NetworkDistributedRepoClient.publisher_object_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5844 行。

```python
def publisher_object_name(self, suffix: str) -> str:
```

### API-49557f9f52b6 · NetworkDistributedRepoClient.capability

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5889 行。

```python
def capability(self, *, timeout_ms: int | None = None) -> dict:
```

### API-b6b7bc83d309 · NetworkDistributedRepoClient.cache_status

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5901 行。

```python
def cache_status(self, repo_node: str) -> dict:
```

### API-05ade06b987b · NetworkDistributedRepoClient.catalog_bucket_digest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5912 行。

```python
def catalog_bucket_digest(self, repo_node: str,
                              bucket_count: int = 64) -> dict:
```

### API-c848c1fab3d3 · NetworkDistributedRepoClient.catalog_bucket_entries

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5921 行。

```python
def catalog_bucket_entries(self, repo_node: str, bucket: int,
                               bucket_count: int = 64) -> dict:
```

### API-ac0af973e650 · NetworkDistributedRepoClient.repair_scan

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5931 行。

```python
def repair_scan(self, repo_node: str) -> dict:
```

### API-2e104e6b82bf · NetworkDistributedRepoClient.repair_claim

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5936 行。

```python
def repair_claim(
        self,
        repo_node: str,
        *,
        lease_owner: str,
        lease_ms: int = 60_000,
    ) -> dict:
```

### API-d2b32dd1d6a8 · NetworkDistributedRepoClient.repair_complete

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5950 行。

```python
def repair_complete(
        self,
        repo_node: str,
        *,
        repair_id: str,
        result: dict,
    ) -> dict:
```

### API-d1ef6ae97fab · NetworkDistributedRepoClient.repair_fail

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5964 行。

```python
def repair_fail(
        self,
        repo_node: str,
        *,
        repair_id: str,
        error: str,
    ) -> dict:
```

### API-44ba42e69fc3 · NetworkDistributedRepoClient.scrub

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5978 行。

```python
def scrub(self, repo_node: str, limit: int = 100) -> dict:
```

### API-dc67dfe9ab2c · NetworkDistributedRepoClient.store_versioned

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 5986 行。

```python
def store_versioned(
        self, *, object_name: str, payload: bytes, object_type: str,
        generation: int, expected_generation: int,
        write_consistency: str = WriteConsistency.ALL.value,
        replication_factor: int = 1, replica_nodes: tuple[str, ...] = (),
        policy_epoch: str = "", metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
```

### API-c14650a81306 · NetworkDistributedRepoClient.fetch_packet

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 6069 行。

```python
def fetch_packet(self, repo_node: str, data_name: str) -> DataPacket:
```

原始接口说明：

```text
Fetch one immutable packet by its complete original NDN Data name.
```

### API-cf24b3da9f69 · NetworkDistributedRepoClient.fetch_signed_packets

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 6108 行。

```python
def fetch_signed_packets(
        self,
        manifest: RepoObjectManifest,
        *,
        repo_node: str = "",
    ) -> list[DataPacket]:
```

原始接口说明：

```text
Fetch one complete app-produced packet set in manifest order.

Each replica attempt starts a fresh local result. A missing or invalid
packet therefore fails that replica atomically instead of exposing a
partial packet set to the caller.
```

### API-b45011caaca3 · NetworkDistributedRepoClient.data_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 6193 行。

```python
def data_name(repo_node: str, object_name: str) -> str:
```

### API-00b9b4ea92c3 · NetworkDistributedRepoClient.store

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 6710 行。

```python
def store(
        self,
        *,
        object_name: str,
        payload: bytes,
        object_type: str,
        replication_factor: int = 1,
        replica_nodes: tuple[str, ...] = (),
        policy_epoch: str,
        metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
```

### API-c46e5138a41d · NetworkDistributedRepoClient.store_object

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 6775 行。

```python
def store_object(
        self,
        *,
        object_name: str,
        payload: bytes,
        object_type: str,
        replication_factor: int = 1,
        replica_nodes: tuple[str, ...] = (),
        policy_epoch: str,
        metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
```

### API-d4284fbde82c · NetworkDistributedRepoClient.store_signed_packets

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7055 行。

```python
def store_signed_packets(
        self,
        *,
        object_name: str,
        packets: list[DataPacket],
        object_type: str,
        object_size: int,
        object_sha256: str,
        replication_factor: int = 1,
        replica_nodes: tuple[str, ...] = (),
        policy_epoch: str,
        data_name: str = "",
        metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
```

原始接口说明：

```text
Store app-produced signed NDN Data packets without re-signing them.

The application remains responsible for segmentation, signatures,
payload encryption, and the object-level hash. The repo verifies only
that each submitted packet name and wire hash matches the request
metadata, then stores the signed Data wire bytes as-is.
```

### API-70a548fca4d4 · NetworkDistributedRepoClient.manifest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7223 行。

```python
def manifest(self, object_name: str) -> RepoObjectManifest:
```

### API-4790c2009b82 · NetworkDistributedRepoClient.inventory

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7243 行。

```python
def inventory(self) -> dict[str, RepoObjectManifest]:
```

### API-e0f06c5d9b3d · NetworkDistributedRepoClient.catalog_lookup

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7269 行。

```python
def catalog_lookup(self, object_name: str, repo_node: str) -> dict:
```

### API-2d03775418bf · NetworkDistributedRepoClient.catalog_query

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7280 行。

```python
def catalog_query(self, repo_node: str, query: dict) -> dict:
```

### API-d4b7e0b4d9f2 · NetworkDistributedRepoClient.catalog_status

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7291 行。

```python
def catalog_status(self, repo_node: str) -> dict:
```

### API-a08a26191cd0 · NetworkDistributedRepoClient.catalog_merge

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7302 行。

```python
def catalog_merge(
        self,
        repo_node: str,
        entries: Iterable[dict],
        source_status: Optional[dict] = None,
    ) -> dict:
```

### API-6423201073d4 · NetworkDistributedRepoClient.catalog_repair

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7322 行。

```python
def catalog_repair(self, target_repo_node: str, action: dict) -> dict:
```

### API-e2043ab91c79 · NetworkDistributedRepoClient.catalog_snapshot

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7482 行。

```python
def catalog_snapshot(self, repo_node: str) -> dict:
```

### API-6120420e16bc · NetworkDistributedRepoClient.catalog_snapshot_with_payload

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7486 行。

```python
def catalog_snapshot_with_payload(self, repo_node: str) -> tuple[dict, bytes]:
```

### API-9cd62aaedb77 · NetworkDistributedRepoClient.delete

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7497 行。

```python
def delete(
        self,
        object_name: str,
        replica_nodes: Iterable[str] = (),
    ) -> bool:
```

### API-d17a19e669df · NetworkDistributedRepoClient.fetch

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7544 行。

```python
def fetch(self, object_name: str, manifest: RepoObjectManifest | None = None) -> bytes:
```

### API-7deae2197837 · NetworkDistributedRepoClient.fetch_object

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7548 行。

```python
def fetch_object(
        self,
        object_name: str,
        manifest: RepoObjectManifest | None = None,
    ) -> bytes:
```

### API-78e065aef54b · NetworkDistributedRepoClient.wait_until_ready

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7781 行。

```python
def wait_until_ready(self, timeout_s: float = 10.0, *, probe_timeout_ms: int = 3000) -> dict:
```

### API-b1f70ea75a8c · DistributedRepo

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7794 行。

```python
class DistributedRepo:
```

原始接口说明：

```text
User-facing generic object-store facade.

This wrapper hides NDNSF-specific setup details such as ``ServiceUser``,
the shared repo service name, and the upload prefix. Applications can treat
the repo as a named object store: ``put`` bytes, ``get`` bytes, and inspect
returned manifests when placement metadata matters.
```

### API-25d325cb7086 · DistributedRepo.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7808 行。

```python
def __init__(self, client: NetworkDistributedRepoClient):
```

### API-adf7924aaff0 · DistributedRepo.publisher_namespace

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7813 行。

```python
def publisher_namespace(self) -> str:
```

### API-7e230227ed9e · DistributedRepo.exact_packets

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7817 行。

```python
def exact_packets(self) -> "ExactPacketRepositoryApi":
```

原始接口说明：

```text
Explicit exact-packet-v1 compatibility surface.

The legacy direct methods remain source-compatible, but new code can
name the preserved wire/trust format without confusing it with the
artifact-manifest-v2 file API.
```

### API-d4d85a1d4efe · DistributedRepo.object_name

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7827 行。

```python
def object_name(self, suffix: str) -> str:
```

### API-eda5def5bffb · DistributedRepo.from_config

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7837 行。

```python
def from_config(
        cls,
        config: str | Path,
        *,
        generated_policy_dir: str | Path = "/tmp/ndnsf-distributed-repo-policy",
        user: str | None = None,
        service_name: str = DEFAULT_SERVICE,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 10000,
        verbose: bool = False,
        bootstrap_token: str = "",
    ) -> "DistributedRepo":
```

### API-8983262f35af · DistributedRepo.from_ndn_config

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7878 行。

```python
def from_ndn_config(
        cls,
        *,
        controller: str,
        user: str,
        group: str,
        trust_schema: str,
        config_object_name: str = DEFAULT_CONFIG_OBJECT,
        generated_policy_dir: str | Path = "/tmp/ndnsf-distributed-repo-policy",
        service_name: str = DEFAULT_SERVICE,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 10000,
        verbose: bool = False,
    ) -> "DistributedRepo":
```

### API-9c13b721c428 · DistributedRepo.wait_until_ready

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7925 行。

```python
def wait_until_ready(self, timeout_s: float = 10.0) -> dict:
```

### API-608ca411e9f1 · DistributedRepo.put

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7928 行。

```python
def put(
        self,
        object_name: str,
        payload: bytes | bytearray | memoryview | str,
        *,
        object_type: str = "object",
        replication_factor: int = 1,
        replica_nodes: Iterable[str] = (),
        policy_epoch: str = "",
        metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
```

### API-2b352cce3976 · DistributedRepo.put_file

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 7956 行。

```python
def put_file(
        self,
        object_name: str,
        path: str | Path,
        *,
        chunk_size: int = 16 * 1024 * 1024,
        expected_sha256: str = "",
        expected_size: int | None = None,
        object_type: str = "file",
        replication_factor: int = 1,
        replica_nodes: Iterable[str] = (),
        policy_epoch: str = "",
        metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
```

原始接口说明：

```text
Publish one file through bounded Repo objects plus a root manifest.

``put`` remains suitable for small objects. This method bounds publisher
memory independently of file size by passing at most one chunk to
``put`` at a time. The returned manifest describes the small root
manifest; its metadata binds the complete file digest and size.
```

### API-f3d673b97146 · DistributedRepo.get_file

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8080 行。

```python
def get_file(
        self,
        object_name: str,
        destination: str | Path,
        *,
        manifest: RepoObjectManifest | None = None,
        expected_sha256: str = "",
        expected_size: int | None = None,
    ) -> Path:
```

原始接口说明：

```text
Fetch and verify a bounded file bundle into a new local file.
```

### API-35cdd508c1ab · DistributedRepo.get

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8134 行。

```python
def get(self, object_name: str, manifest: RepoObjectManifest | None = None) -> bytes:
```

### API-ef163f9e0f68 · DistributedRepo.put_signed_packets

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8144 行。

```python
def put_signed_packets(
        self,
        object_name: str,
        packets: list[DataPacket],
        *,
        object_type: str,
        object_size: int,
        object_sha256: str,
        replication_factor: int = 1,
        replica_nodes: Iterable[str] = (),
        policy_epoch: str = "",
        data_name: str = "",
        metadata: Optional[dict] = None,
    ) -> RepoObjectManifest:
```

### API-9f05c5b1edd8 · DistributedRepo.fetch_packet

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8174 行。

```python
def fetch_packet(self, repo_node: str, data_name: str) -> DataPacket:
```

### API-260bae863380 · DistributedRepo.get_signed_packets

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8177 行。

```python
def get_signed_packets(
        self,
        object_name: str,
        manifest: RepoObjectManifest | None = None,
        *,
        repo_node: str = "",
    ) -> list[DataPacket]:
```

### API-63193d060f5c · DistributedRepo.manifest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8196 行。

```python
def manifest(self, object_name: str) -> RepoObjectManifest:
```

### API-afc46fc34cf3 · DistributedRepo.list

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8202 行。

```python
def list(self) -> dict[str, RepoObjectManifest]:
```

### API-60ed7afa5783 · DistributedRepo.remote_inventory

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8205 行。

```python
def remote_inventory(self) -> dict[str, RepoObjectManifest]:
```

### API-7c3228cd70e4 · DistributedRepo.catalog_lookup

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8210 行。

```python
def catalog_lookup(self, object_name: str, repo_node: str) -> dict:
```

### API-af247ae22bc0 · DistributedRepo.catalog_query

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8213 行。

```python
def catalog_query(self, repo_node: str, query: dict) -> dict:
```

### API-77a1fb1f22fb · DistributedRepo.catalog_status

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8216 行。

```python
def catalog_status(self, repo_node: str) -> dict:
```

### API-3a9d366a637b · DistributedRepo.cache_status

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8219 行。

```python
def cache_status(self, repo_node: str) -> dict:
```

### API-c2cbfa8a16f9 · DistributedRepo.catalog_merge

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8222 行。

```python
def catalog_merge(
        self,
        repo_node: str,
        entries: Iterable[dict],
        source_status: Optional[dict] = None,
    ) -> dict:
```

### API-d53110ed1913 · DistributedRepo.catalog_repair

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8230 行。

```python
def catalog_repair(self, target_repo_node: str, action: dict) -> dict:
```

### API-fb98eb9d8b01 · DistributedRepo.repair_scan

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8233 行。

```python
def repair_scan(self, repo_node: str) -> dict:
```

### API-f3f3e3948ec4 · DistributedRepo.repair_claim

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8236 行。

```python
def repair_claim(
        self,
        repo_node: str,
        *,
        lease_owner: str,
        lease_ms: int = 60_000,
    ) -> dict:
```

### API-2da6637b7310 · DistributedRepo.repair_complete

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8246 行。

```python
def repair_complete(
        self,
        repo_node: str,
        *,
        repair_id: str,
        result: dict,
    ) -> dict:
```

### API-5acb0b661fc0 · DistributedRepo.repair_fail

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8256 行。

```python
def repair_fail(
        self,
        repo_node: str,
        *,
        repair_id: str,
        error: str,
    ) -> dict:
```

### API-88f37b63ae6c · DistributedRepo.catalog_snapshot

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8266 行。

```python
def catalog_snapshot(self, repo_node: str) -> dict:
```

### API-533967651965 · DistributedRepo.catalog_snapshot_with_payload

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8269 行。

```python
def catalog_snapshot_with_payload(self, repo_node: str) -> tuple[dict, bytes]:
```

### API-75f3274604d7 · DistributedRepo.remove

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8272 行。

```python
def remove(self, object_name: str) -> bool:
```

### API-b79f17e621a0 · ExactPacketRepositoryApi

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8289 行。

```python
class ExactPacketRepositoryApi:
```

原始接口说明：

```text
Compatibility backend that preserves application-signed Data wires.
```

### API-bcab84949e21 · ExactPacketRepositoryApi.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8294 行。

```python
def __init__(self, repo: DistributedRepo):
```

### API-fb92e00ea658 · ExactPacketRepositoryApi.put_signed_packets

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8297 行。

```python
def put_signed_packets(self, object_name: str, packets: list[DataPacket],
                           **kwargs) -> RepoObjectManifest:
```

### API-dcf935e41a5b · ExactPacketRepositoryApi.get_signed_packets

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8301 行。

```python
def get_signed_packets(
        self,
        object_name: str,
        manifest: RepoObjectManifest | None = None,
        *,
        repo_node: str = "",
    ) -> list[DataPacket]:
```

### API-dc690a950370 · select_replicas

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`，第 8328 行。

```python
def select_replicas(
    candidates: Iterable[StorageCapability],
    policy: PlacementPolicy,
    object_size: int,
) -> tuple[StorageCapability, ...]:
```

## NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py

源码 SHA-256：`6c16c3d5b32d26e5c65b1f497d58509897fd3b0bf2cbe946a85fca6d2ff98eea`。

显式导出：`ARTIFACT_LIFECYCLE_STATES`, `ARTIFACT_LIFECYCLE_TRANSITIONS`, `ArtifactCapacityStatus`, `ArtifactFinalizationRecord`, `ArtifactStorageIdentity`, `ArtifactTransferSessionRecord`, `FilesystemCasPayloadStore`, `LifecycleTransitionError`, `MetadataStore`, `PayloadStore`, `PersistenceOwnershipError`, `RepoLifecycleEvent`, `SqliteRepositoryPersistence`。

### API-cad037fcc2ff · PersistenceOwnershipError

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 57 行。

```python
class PersistenceOwnershipError(RuntimeError):
```

原始接口说明：

```text
Raised when a second authority tries to own one backend.
```

### API-c8eb48c3464a · LifecycleTransitionError

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 61 行。

```python
class LifecycleTransitionError(ValueError):
```

原始接口说明：

```text
Raised after a rejected lifecycle event is durably journaled.
```

### API-ee14cccc614b · RepoLifecycleEvent

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 66 行。

```python
class RepoLifecycleEvent:
```

### API-6122057cde62 · RepoLifecycleEvent.event_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 67 行。

```python
event_id: str
```

### API-f4c3da9aee61 · RepoLifecycleEvent.operation_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 68 行。

```python
operation_id: str
```

### API-05e3c3e62021 · RepoLifecycleEvent.artifact_digest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 69 行。

```python
artifact_digest: str
```

### API-206627ca29a2 · RepoLifecycleEvent.generation

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 70 行。

```python
generation: int
```

### API-a15dbfc597bc · RepoLifecycleEvent.from_state

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 71 行。

```python
from_state: str
```

### API-bcb4b20acfac · RepoLifecycleEvent.to_state

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 72 行。

```python
to_state: str
```

### API-f7944de51e64 · RepoLifecycleEvent.event_time_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 73 行。

```python
event_time_ms: int
```

### API-e913d2d80ac7 · RepoLifecycleEvent.accepted

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 74 行。

```python
accepted: bool
```

### API-dc0c2abc0747 · RepoLifecycleEvent.detail

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 75 行。

```python
detail: dict
```

### API-c88c64ca393f · RepoLifecycleEvent.error

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 76 行。

```python
error: str = ""
```

### API-4c8d5e09a2aa · RepoLifecycleEvent.sequence

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 77 行。

```python
sequence: int = 0
```

### API-f74a87df3466 · ArtifactTransferSessionRecord

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 81 行。

```python
class ArtifactTransferSessionRecord:
```

### API-9d083e55f91b · ArtifactTransferSessionRecord.operation_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 82 行。

```python
operation_id: str
```

### API-a289a57e5baf · ArtifactTransferSessionRecord.artifact_digest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 83 行。

```python
artifact_digest: str
```

### API-5301c98e9569 · ArtifactTransferSessionRecord.generation

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 84 行。

```python
generation: int
```

### API-01aad539987d · ArtifactTransferSessionRecord.identity

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 85 行。

```python
identity: dict
```

### API-fdf30b1be083 · ArtifactTransferSessionRecord.lease

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 86 行。

```python
lease: dict
```

### API-90874d297c56 · ArtifactTransferSessionRecord.state

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 87 行。

```python
state: str
```

### API-bddde6f5e69c · ArtifactTransferSessionRecord.preserves_progress

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 88 行。

```python
preserves_progress: bool
```

### API-933f3ebcffd3 · ArtifactTransferSessionRecord.verified_chunks

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 89 行。

```python
verified_chunks: int
```

### API-379ccf12d9fd · ArtifactTransferSessionRecord.newly_verified_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 90 行。

```python
newly_verified_bytes: int
```

### API-ccaef7a37ad8 · ArtifactTransferSessionRecord.avoided_retransmission_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 91 行。

```python
avoided_retransmission_bytes: int
```

### API-df8c46b88e2e · ArtifactTransferSessionRecord.updated_at_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 92 行。

```python
updated_at_ms: int
```

### API-f8efca760cca · ArtifactFinalizationRecord

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 96 行。

```python
class ArtifactFinalizationRecord:
```

### API-6291d38c6307 · ArtifactFinalizationRecord.operation_id

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 97 行。

```python
operation_id: str
```

### API-c9716128a170 · ArtifactFinalizationRecord.artifact_digest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 98 行。

```python
artifact_digest: str
```

### API-b98dd751c150 · ArtifactFinalizationRecord.generation

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 99 行。

```python
generation: int
```

### API-a78a7d7c9e2e · ArtifactFinalizationRecord.phase

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 100 行。

```python
phase: str
```

### API-26dedf6221b7 · ArtifactFinalizationRecord.detail

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 101 行。

```python
detail: dict
```

### API-2bdea2d0d27b · ArtifactFinalizationRecord.updated_at_ms

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 102 行。

```python
updated_at_ms: int
```

### API-7ce6c4787ab4 · ArtifactFinalizationRecord.error

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 103 行。

```python
error: str = ""
```

### API-5b9a9a594c44 · ArtifactCapacityStatus

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 107 行。

```python
class ArtifactCapacityStatus:
```

### API-92df01f06bb9 · ArtifactCapacityStatus.capacity_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 108 行。

```python
capacity_bytes: int
```

### API-f28e298a2a90 · ArtifactCapacityStatus.committed_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 109 行。

```python
committed_bytes: int
```

### API-a47ed85f9e79 · ArtifactCapacityStatus.reserved_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 110 行。

```python
reserved_bytes: int
```

### API-2e07aa7a019e · ArtifactCapacityStatus.available_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 111 行。

```python
available_bytes: int
```

### API-20be8f3f96bf · PayloadStore

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 114 行。

```python
class PayloadStore(Protocol):
```

原始接口说明：

```text
Persistence role owning artifact payload bytes.
```

### API-0510402cd2c5 · PayloadStore.backend_kind

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 118 行。

```python
def backend_kind(self) -> str:
```

### API-7104edb79b87 · ArtifactStorageIdentity

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 123 行。

```python
class ArtifactStorageIdentity:
```

原始接口说明：

```text
Exact byte identity used by a generation-scoped payload session.
```

### API-2ca924c2e58d · ArtifactStorageIdentity.content_digest

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 126 行。

```python
content_digest: str
```

### API-1401d3eab69c · ArtifactStorageIdentity.size_bytes

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 127 行。

```python
size_bytes: int
```

### API-aae919566e6e · ArtifactStorageIdentity.generation

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 128 行。

```python
generation: int
```

### API-0995b105386e · ArtifactStorageIdentity.digest_algorithm

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 129 行。

```python
digest_algorithm: str = "sha256"
```

### API-5c703d973144 · ArtifactStorageIdentity.format_version

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 130 行。

```python
format_version: str = "artifact-manifest-v2"
```

### API-5d808041dbc3 · FilesystemCasPayloadStore

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 147 行。

```python
class FilesystemCasPayloadStore:
```

原始接口说明：

```text
Bounded-memory artifact-v2 CAS with compact verified-range sidecars.
```

### API-70875826a29a · FilesystemCasPayloadStore.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 154 行。

```python
def __init__(
        self, root_path: str | Path, max_range_bytes: int = 16 * 1024 * 1024
    ) -> None:
```

### API-6c3bcf04f759 · FilesystemCasPayloadStore.backend_kind

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 171 行。

```python
def backend_kind(self) -> str:
```

### API-b503e2c91ba8 · FilesystemCasPayloadStore.committed_path

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 174 行。

```python
def committed_path(self, identity: ArtifactStorageIdentity) -> Path:
```

### API-bb522ed161cb · FilesystemCasPayloadStore.staging_path

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 183 行。

```python
def staging_path(self, identity: ArtifactStorageIdentity) -> Path:
```

### API-a42fef8bff62 · FilesystemCasPayloadStore.begin

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 303 行。

```python
def begin(self, identity: ArtifactStorageIdentity) -> None:
```

### API-448fd287d835 · FilesystemCasPayloadStore.write_range

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 321 行。

```python
def write_range(
        self, identity: ArtifactStorageIdentity, offset: int, payload: bytes
    ) -> None:
```

### API-91716c5082e0 · FilesystemCasPayloadStore.read_range

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 338 行。

```python
def read_range(
        self, identity: ArtifactStorageIdentity, offset: int, length: int
    ) -> bytes:
```

### API-035b2ab575c7 · FilesystemCasPayloadStore.mark_verified

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 361 行。

```python
def mark_verified(
        self, identity: ArtifactStorageIdentity, offset: int, length: int
    ) -> None:
```

### API-f3bc531a579d · FilesystemCasPayloadStore.verified_ranges

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 372 行。

```python
def verified_ranges(
        self, identity: ArtifactStorageIdentity
    ) -> tuple[tuple[int, int], ...]:
```

### API-deec841635c4 · FilesystemCasPayloadStore.flush

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 382 行。

```python
def flush(self, identity: ArtifactStorageIdentity) -> None:
```

### API-8afc3e2bfd41 · FilesystemCasPayloadStore.finalize

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 390 行。

```python
def finalize(self, identity: ArtifactStorageIdentity) -> Path:
```

### API-80888967a407 · FilesystemCasPayloadStore.is_committed

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 441 行。

```python
def is_committed(self, identity: ArtifactStorageIdentity) -> bool:
```

### API-d0b69f99fc24 · FilesystemCasPayloadStore.verify_committed

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 445 行。

```python
def verify_committed(self, identity: ArtifactStorageIdentity) -> bool:
```

### API-2b3697368529 · FilesystemCasPayloadStore.abort

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 454 行。

```python
def abort(self, identity: ArtifactStorageIdentity) -> None:
```

### API-75082b77460c · FilesystemCasPayloadStore.reclaim_unreferenced_finalized

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 461 行。

```python
def reclaim_unreferenced_finalized(
        self, identity: ArtifactStorageIdentity
    ) -> None:
```

原始接口说明：

```text
Remove finalized bytes only after metadata authority proves orphaning.
```

### API-0eaf4c0a2d93 · MetadataStore

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 473 行。

```python
class MetadataStore(Protocol):
```

原始接口说明：

```text
Persistence role owning lifecycle and catalog metadata.
```

### API-64860175aada · MetadataStore.transition

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 476 行。

```python
def transition(
        self,
        *,
        event_id: str,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        from_state: str,
        to_state: str,
        detail: dict | None = None,
        event_time_ms: int | None = None,
    ) -> RepoLifecycleEvent:
```

### API-feb6a3a3eb5d · MetadataStore.lifecycle_events

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 490 行。

```python
def lifecycle_events(self, operation_id: str) -> tuple[RepoLifecycleEvent, ...]:
```

### API-e539bb292011 · SqliteRepositoryPersistence

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 537 行。

```python
class SqliteRepositoryPersistence:
```

原始接口说明：

```text
One authoritative facade for one deployed repository database.
```

### API-279896f2452d · SqliteRepositoryPersistence.__init__

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 542 行。

```python
def __init__(
        self,
        database_path: str | Path,
        owner_id: str,
        *,
        capacity_bytes: int | None = None,
        reservation_overhead_bytes: int = 64 * 1024,
        reconcile_on_startup: bool = True,
        artifact_writes_enabled: bool = True,
        max_write_schema_generation: int | None = None,
    ) -> None:
```

### API-189e6b997ee9 · SqliteRepositoryPersistence.artifact_writes_enabled

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 625 行。

```python
def artifact_writes_enabled(self) -> bool:
```

### API-c88c492f5fa9 · SqliteRepositoryPersistence.migration_diagnostics

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 628 行。

```python
def migration_diagnostics(self) -> dict[str, Any]:
```

### API-ca0b4363ffcf · SqliteRepositoryPersistence.commit

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 929 行。

```python
def commit(self) -> None:
```

### API-9688576d432b · SqliteRepositoryPersistence.rollback

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 933 行。

```python
def rollback(self) -> None:
```

### API-818d5f32f066 · SqliteRepositoryPersistence.transition

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 1003 行。

```python
def transition(
        self,
        *,
        event_id: str,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        from_state: str,
        to_state: str,
        detail: dict | None = None,
        event_time_ms: int | None = None,
    ) -> RepoLifecycleEvent:
```

### API-5259de992950 · SqliteRepositoryPersistence.lifecycle_events

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 1113 行。

```python
def lifecycle_events(self, operation_id: str) -> tuple[RepoLifecycleEvent, ...]:
```

### API-64d560716fe4 · SqliteRepositoryPersistence.transfer_session

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 1158 行。

```python
def transfer_session(
        self, operation_id: str
    ) -> ArtifactTransferSessionRecord | None:
```

### API-c7b6bd4c4bcd · SqliteRepositoryPersistence.save_transfer_session

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 1171 行。

```python
def save_transfer_session(
        self,
        *,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        identity: dict[str, Any],
        lease: dict[str, Any],
        state: str,
        preserves_progress: bool,
        verified_chunks: int,
        newly_verified_bytes: int,
        avoided_retransmission_bytes: int,
        updated_at_ms: int,
    ) -> ArtifactTransferSessionRecord:
```

原始接口说明：

```text
Durably save an exact-identity, monotonic resume checkpoint.
```

### API-aa676361be46 · SqliteRepositoryPersistence.finalization_record

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 1371 行。

```python
def finalization_record(
        self, operation_id: str
    ) -> ArtifactFinalizationRecord | None:
```

### API-bb619ab7ba88 · SqliteRepositoryPersistence.begin_finalization

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 1382 行。

```python
def begin_finalization(
        self,
        *,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        logical_name: str,
        policy_epoch: str,
        artifact: dict[str, Any],
        receipt_id: str,
        receipt: dict[str, Any],
        repo_node: str,
        signer_key_id: str,
        authentication_algorithm: str,
        signature_hex: str,
        committed_at_ms: int,
    ) -> ArtifactFinalizationRecord:
```

原始接口说明：

```text
Record all replay material before crossing the payload DB boundary.
```

### API-cd5b163c368b · SqliteRepositoryPersistence.mark_payload_finalized

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 1533 行。

```python
def mark_payload_finalized(
        self, operation_id: str, updated_at_ms: int
    ) -> ArtifactFinalizationRecord:
```

### API-cf882eafdcf1 · SqliteRepositoryPersistence.commit_finalized_artifact

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 1579 行。

```python
def commit_finalized_artifact(
        self, operation_id: str
    ) -> ArtifactFinalizationRecord:
```

### API-8af0c3165950 · SqliteRepositoryPersistence.activate_finalized_artifact

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 1681 行。

```python
def activate_finalized_artifact(
        self, operation_id: str
    ) -> ArtifactFinalizationRecord:
```

### API-6fd93e246d04 · SqliteRepositoryPersistence.reconcile_finalizations

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 1795 行。

```python
def reconcile_finalizations(self) -> tuple[ArtifactFinalizationRecord, ...]:
```

原始接口说明：

```text
Replay every durable nonterminal intent without filename guessing.
```

### API-8e261bae9d71 · SqliteRepositoryPersistence.rollback_finalization

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 1852 行。

```python
def rollback_finalization(
        self, operation_id: str, *, reason: str, now_ms: int
    ) -> ArtifactFinalizationRecord:
```

原始接口说明：

```text
Fail closed and release an unrecoverable pre-commit intent.
```

### API-4e45e4c15b8e · SqliteRepositoryPersistence.commit_and_activate_artifact

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 1947 行。

```python
def commit_and_activate_artifact(self, **values) -> dict[str, Any]:
```

原始接口说明：

```text
Compatibility wrapper routed through the recovery journal.
```

### API-27f1183a4da7 · SqliteRepositoryPersistence.capacity_status

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 2177 行。

```python
def capacity_status(self) -> ArtifactCapacityStatus:
```

### API-e351ed92dec9 · SqliteRepositoryPersistence.reserve_artifact_capacity

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 2214 行。

```python
def reserve_artifact_capacity(
        self,
        *,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        lease_id: str,
        reserved_bytes: int,
        expires_at_ms: int,
        now_ms: int,
    ) -> ArtifactCapacityStatus:
```

### API-9e5b754b2f1c · SqliteRepositoryPersistence.release_artifact_capacity

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 2325 行。

```python
def release_artifact_capacity(
        self, operation_id: str, now_ms: int
    ) -> None:
```

### API-2ff6af3c0bd9 · SqliteRepositoryPersistence.renew_artifact_capacity

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 2338 行。

```python
def renew_artifact_capacity(
        self,
        *,
        operation_id: str,
        lease_id: str,
        expires_at_ms: int,
        now_ms: int,
    ) -> None:
```

### API-0a32285b0f24 · SqliteRepositoryPersistence.claim_garbage_collection

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 2433 行。

```python
def claim_garbage_collection(
        self,
        *,
        operation_id: str,
        artifact_digest: str,
        generation: int,
        gc_owner: str,
        now_ms: int,
        deadline_ms: int,
        format_version: str = "artifact-manifest-v2",
        digest_algorithm: str = "sha256",
    ) -> None:
```

### API-f0784f4c73f2 · SqliteRepositoryPersistence.reclaim_temporary_generation

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 2532 行。

```python
def reclaim_temporary_generation(
        self,
        identity: ArtifactStorageIdentity,
        *,
        operation_id: str,
        gc_owner: str,
        now_ms: int,
        crash_injector: Any | None = None,
    ) -> None:
```

### API-9526eeb006d7 · SqliteRepositoryPersistence.reconcile_gc_claims

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 2679 行。

```python
def reconcile_gc_claims(self) -> tuple[str, ...]:
```

### API-c130d1aa67e0 · SqliteRepositoryPersistence.collect_expired_temporaries

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 2720 行。

```python
def collect_expired_temporaries(
        self, *, gc_owner: str, now_ms: int, claim_ttl_ms: int = 30000
    ) -> tuple[str, ...]:
```

### API-4b039c86dc86 · SqliteRepositoryPersistence.active_artifact

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 2778 行。

```python
def active_artifact(
        self,
        logical_name: str,
        policy_epoch: str,
        *,
        format_version: str = "artifact-manifest-v2",
        digest_algorithm: str = "sha256",
    ) -> dict[str, Any] | None:
```

### API-ce92714e311b · SqliteRepositoryPersistence.active_artifacts

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 2819 行。

```python
def active_artifacts(
        self,
        *,
        format_version: str = "artifact-manifest-v2",
        digest_algorithm: str = "sha256",
    ) -> tuple[dict[str, Any], ...]:
```

原始接口说明：

```text
Return durable active-catalog rows for restart-time rehydration.
```

### API-2165e1cf3c14 · SqliteRepositoryPersistence.authenticated_receipt

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 2850 行。

```python
def authenticated_receipt(
        self, operation_id: str
    ) -> dict[str, Any] | None:
```

### API-35df5aa47cfa · SqliteRepositoryPersistence.close

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/persistence.py`，第 2873 行。

```python
def close(self) -> None:
```

## NDNSF-DistributedRepo/pythonWrapper/py_repoclient/service_names.py

源码 SHA-256：`4f2dfdba1885ad3fcb2e3b5c1e132e2f5c02cca5119a0d2c43f5f604678207cc`。

显式导出：`DEFAULT_REPO_SERVICE_ROOT`, `canonical_repo_operation`, `is_internal_repo_service`, `repo_service_for_operation`, `repo_versioned_services`。

### API-582ed1df2028 · repo_service_for_operation

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/service_names.py`，第 54 行。

```python
def repo_service_for_operation(
    operation: str,
    root: str = DEFAULT_REPO_SERVICE_ROOT,
) -> str:
```

原始接口说明：

```text
Return the public or peer-only versioned service for an operation.
```

### API-cb5f37c8975d · canonical_repo_operation

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/service_names.py`，第 71 行。

```python
def canonical_repo_operation(operation: str) -> str:
```

### API-e169fa73e988 · repo_versioned_services

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/service_names.py`，第 76 行。

```python
def repo_versioned_services(
    root: str = DEFAULT_REPO_SERVICE_ROOT,
) -> tuple[str, ...]:
```

原始接口说明：

```text
Return every service a Repo node must register.
```

### API-6e4b2942a42c · is_internal_repo_service

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/service_names.py`，第 87 行。

```python
def is_internal_repo_service(service_name: str) -> bool:
```

## NDNSF-DistributedRepo/pythonWrapper/setup.py

源码 SHA-256：`068f7b9d33ccd5beee231edd4f5602c9ffd2972466b13957c4d9b55c0e386151`。

### API-282eea0e442f · pkg_config

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/setup.py`，第 15 行。

```python
def pkg_config(*packages: str) -> tuple[list[str], list[str], list[str], list[str]]:
```

### API-d77bf8412d49 · build_extension

public-by-name / declared-interface；冻结源码：`NDNSF-DistributedRepo/pythonWrapper/setup.py`，第 40 行。

```python
def build_extension() -> Extension:
```
