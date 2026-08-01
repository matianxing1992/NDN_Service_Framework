#include "ndnsf-distributed-repo/ArtifactManifest.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace ndnsf_distributed_repo {

namespace {

constexpr std::array<uint8_t, 4> ROOT_CANONICAL_MAGIC{{'A', 'R', 'C', '2'}};
constexpr std::array<uint8_t, 4> ROOT_WIRE_MAGIC{{'A', 'R', 'S', '2'}};
constexpr std::array<uint8_t, 4> PAGE_CANONICAL_MAGIC{{'A', 'P', 'C', '2'}};
constexpr std::array<uint8_t, 4> PAGE_WIRE_MAGIC{{'A', 'P', 'G', '2'}};
constexpr size_t MAX_PUBLIC_KEY_PEM_BYTES = 64 * 1024;

[[noreturn]] void
fail(const char* code, const std::string& message)
{
  throw ArtifactValidationError(code, message);
}

class Writer
{
public:
  explicit Writer(size_t limit)
    : m_limit(limit)
  {
    m_bytes.reserve(std::min<size_t>(limit, 4096));
  }

  void
  raw(const uint8_t* data, size_t size)
  {
    if (size > m_limit - m_bytes.size()) {
      fail(artifact_error::LimitExceeded, "manifest encoding exceeds policy limit");
    }
    m_bytes.insert(m_bytes.end(), data, data + size);
  }

  template<size_t N>
  void
  magic(const std::array<uint8_t, N>& value)
  {
    raw(value.data(), value.size());
  }

  void
  u32(uint32_t value)
  {
    const std::array<uint8_t, 4> encoded{{
      static_cast<uint8_t>((value >> 24) & 0xff),
      static_cast<uint8_t>((value >> 16) & 0xff),
      static_cast<uint8_t>((value >> 8) & 0xff),
      static_cast<uint8_t>(value & 0xff),
    }};
    raw(encoded.data(), encoded.size());
  }

  void
  u64(uint64_t value)
  {
    std::array<uint8_t, 8> encoded{};
    for (size_t i = 0; i < encoded.size(); ++i) {
      encoded[encoded.size() - i - 1] =
        static_cast<uint8_t>((value >> (i * 8)) & 0xff);
    }
    raw(encoded.data(), encoded.size());
  }

  void
  string(const std::string& value, size_t fieldLimit)
  {
    if (value.size() > fieldLimit ||
        value.size() > std::numeric_limits<uint32_t>::max()) {
      fail(artifact_error::LimitExceeded, "manifest string exceeds field limit");
    }
    u32(static_cast<uint32_t>(value.size()));
    raw(reinterpret_cast<const uint8_t*>(value.data()), value.size());
  }

  void
  bytes(const std::vector<uint8_t>& value, size_t fieldLimit)
  {
    if (value.size() > fieldLimit ||
        value.size() > std::numeric_limits<uint32_t>::max()) {
      fail(artifact_error::LimitExceeded, "manifest bytes exceed field limit");
    }
    u32(static_cast<uint32_t>(value.size()));
    raw(value.data(), value.size());
  }

  std::vector<uint8_t>
  finish()
  {
    return std::move(m_bytes);
  }

private:
  size_t m_limit;
  std::vector<uint8_t> m_bytes;
};

class Reader
{
public:
  explicit Reader(const std::vector<uint8_t>& bytes)
    : m_bytes(bytes)
  {
  }

  template<size_t N>
  void
  magic(const std::array<uint8_t, N>& expected)
  {
    require(N);
    if (!std::equal(expected.begin(), expected.end(), m_bytes.begin() + m_offset)) {
      fail(artifact_manifest_error::MalformedEncoding,
           "manifest encoding has an unknown magic or version");
    }
    m_offset += N;
  }

  uint32_t
  u32()
  {
    require(4);
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
      value = (value << 8) | m_bytes[m_offset++];
    }
    return value;
  }

  uint64_t
  u64()
  {
    require(8);
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
      value = (value << 8) | m_bytes[m_offset++];
    }
    return value;
  }

  std::string
  string(size_t fieldLimit)
  {
    const auto size = u32();
    if (size > fieldLimit) {
      fail(artifact_error::LimitExceeded,
           "manifest string exceeds field limit before allocation");
    }
    require(size);
    std::string value(
      reinterpret_cast<const char*>(m_bytes.data() + m_offset), size);
    m_offset += size;
    return value;
  }

  std::vector<uint8_t>
  bytes(size_t fieldLimit)
  {
    const auto size = u32();
    if (size > fieldLimit) {
      fail(artifact_error::LimitExceeded,
           "manifest byte string exceeds field limit before allocation");
    }
    require(size);
    std::vector<uint8_t> value(
      m_bytes.begin() + m_offset, m_bytes.begin() + m_offset + size);
    m_offset += size;
    return value;
  }

  void
  done() const
  {
    if (m_offset != m_bytes.size()) {
      fail(artifact_manifest_error::MalformedEncoding,
           "manifest encoding contains trailing fields");
    }
  }

private:
  void
  require(size_t size) const
  {
    if (size > m_bytes.size() - m_offset) {
      fail(artifact_manifest_error::MalformedEncoding,
           "manifest encoding is truncated");
    }
  }

private:
  const std::vector<uint8_t>& m_bytes;
  size_t m_offset = 0;
};

void
writeArtifactReference(Writer& writer, const ArtifactReference& artifact,
                       const ArtifactLimits& limits)
{
  writer.string(artifact.logicalName, limits.maxNameBytes);
  writer.string(artifact.digestAlgorithm, 128);
  writer.string(artifact.contentDigest, 128);
  writer.u64(artifact.sizeBytes);
  writer.string(artifact.formatVersion, 128);
  writer.string(artifact.rootManifestName, limits.maxNameBytes);
  writer.string(artifact.publisherIdentity, limits.maxNameBytes);
  writer.string(artifact.policyEpoch, 256);
}

ArtifactReference
readArtifactReference(Reader& reader, const ArtifactLimits& limits)
{
  ArtifactReference artifact;
  artifact.logicalName = reader.string(limits.maxNameBytes);
  artifact.digestAlgorithm = reader.string(128);
  artifact.contentDigest = reader.string(128);
  artifact.sizeBytes = reader.u64();
  artifact.formatVersion = reader.string(128);
  artifact.rootManifestName = reader.string(limits.maxNameBytes);
  artifact.publisherIdentity = reader.string(limits.maxNameBytes);
  artifact.policyEpoch = reader.string(256);
  artifact.validate(limits);
  return artifact;
}

bool
sameArtifact(const ArtifactReference& left, const ArtifactReference& right)
{
  return left.logicalName == right.logicalName &&
         left.digestAlgorithm == right.digestAlgorithm &&
         left.contentDigest == right.contentDigest &&
         left.sizeBytes == right.sizeBytes &&
         left.formatVersion == right.formatVersion &&
         left.rootManifestName == right.rootManifestName &&
         left.publisherIdentity == right.publisherIdentity &&
         left.policyEpoch == right.policyEpoch;
}

bool
contains(const std::vector<std::string>& values, const std::string& expected)
{
  return std::find(values.begin(), values.end(), expected) != values.end();
}

std::vector<uint8_t>
encodeRootCanonicalUnchecked(const ArtifactRootManifest& root,
                             const ArtifactLimits& limits)
{
  Writer writer(limits.maxRootEncodedBytes);
  writer.magic(ROOT_CANONICAL_MAGIC);
  writer.string("artifact-root-v2", 128);
  writeArtifactReference(writer, root.artifact, limits);
  writer.u32(root.packetPayloadBytes);
  writer.u64(root.chunkBytes);
  writer.string(root.namingTemplate, limits.maxNameBytes);
  writer.string(root.manifestRootDigestAlgorithm, 128);
  writer.string(root.manifestRootDigest, 128);
  writer.string(root.signatureAlgorithm, 128);
  writer.string(root.publisherKeyLocator, limits.maxNameBytes);
  writer.u64(root.createdAtMs);
  writer.u64(root.expiresAtMs);
  if (root.criticalExtensions.size() > limits.maxCriticalExtensions) {
    fail(artifact_error::LimitExceeded, "too many critical root extensions");
  }
  writer.u32(static_cast<uint32_t>(root.criticalExtensions.size()));
  for (const auto& extension : root.criticalExtensions) {
    writer.string(extension, 128);
  }
  return writer.finish();
}

ArtifactRootManifest
decodeRootCanonical(const std::vector<uint8_t>& canonical,
                    const ArtifactLimits& limits)
{
  if (canonical.empty() || canonical.size() > limits.maxRootEncodedBytes) {
    fail(artifact_error::LimitExceeded, "root manifest encoded size is outside limits");
  }
  Reader reader(canonical);
  reader.magic(ROOT_CANONICAL_MAGIC);
  if (reader.string(128) != "artifact-root-v2") {
    fail(artifact_manifest_error::Downgrade, "unsupported root manifest version");
  }
  ArtifactRootManifest root;
  root.artifact = readArtifactReference(reader, limits);
  root.packetPayloadBytes = reader.u32();
  root.chunkBytes = reader.u64();
  root.namingTemplate = reader.string(limits.maxNameBytes);
  root.manifestRootDigestAlgorithm = reader.string(128);
  root.manifestRootDigest = reader.string(128);
  root.signatureAlgorithm = reader.string(128);
  root.publisherKeyLocator = reader.string(limits.maxNameBytes);
  root.createdAtMs = reader.u64();
  root.expiresAtMs = reader.u64();
  const auto extensionCount = reader.u32();
  if (extensionCount > limits.maxCriticalExtensions) {
    fail(artifact_error::LimitExceeded,
         "critical extension count exceeds limit before allocation");
  }
  root.criticalExtensions.reserve(extensionCount);
  for (uint32_t i = 0; i < extensionCount; ++i) {
    root.criticalExtensions.push_back(reader.string(128));
  }
  reader.done();
  root.validate(canonical.size(), limits);
  return root;
}

std::vector<uint8_t>
encodePageCanonicalUnchecked(const ArtifactManifestPage& page,
                             const ArtifactLimits& limits)
{
  Writer writer(limits.maxPageEncodedBytes);
  writer.magic(PAGE_CANONICAL_MAGIC);
  writer.string(page.pageVersion, 128);
  writer.u32(page.depth);
  writer.u64(page.offsetBytes);
  writer.u64(page.lengthBytes);
  writer.string(page.pageDigestAlgorithm, 128);
  if (page.children.size() > limits.maxPageEntries) {
    fail(artifact_error::LimitExceeded, "manifest page entry count exceeds limit");
  }
  writer.u32(static_cast<uint32_t>(page.children.size()));
  for (const auto& child : page.children) {
    writer.string(child.kind, 16);
    writer.u64(child.index);
    writer.u64(child.offsetBytes);
    writer.u64(child.lengthBytes);
    writer.string(child.digestAlgorithm, 128);
    writer.string(child.digest, 128);
  }
  return writer.finish();
}

ArtifactManifestPage
decodePageCanonical(const std::vector<uint8_t>& canonical,
                    const ArtifactLimits& limits)
{
  if (canonical.empty() || canonical.size() > limits.maxPageEncodedBytes) {
    fail(artifact_error::LimitExceeded, "manifest page encoded size is outside limits");
  }
  Reader reader(canonical);
  reader.magic(PAGE_CANONICAL_MAGIC);
  ArtifactManifestPage page;
  page.pageVersion = reader.string(128);
  page.depth = reader.u32();
  page.offsetBytes = reader.u64();
  page.lengthBytes = reader.u64();
  page.pageDigestAlgorithm = reader.string(128);
  const auto childCount = reader.u32();
  if (childCount == 0 || childCount > limits.maxPageEntries) {
    fail(artifact_error::LimitExceeded,
         "manifest child count exceeds limit before allocation");
  }
  page.children.reserve(childCount);
  for (uint32_t i = 0; i < childCount; ++i) {
    ArtifactManifestChild child;
    child.kind = reader.string(16);
    child.index = reader.u64();
    child.offsetBytes = reader.u64();
    child.lengthBytes = reader.u64();
    child.digestAlgorithm = reader.string(128);
    child.digest = reader.string(128);
    page.children.push_back(std::move(child));
  }
  reader.done();
  return page;
}

void
verifyPublicKeySignature(const SignedArtifactRoot& signedRoot,
                         const ArtifactManifestTrustPolicy& policy,
                         const std::vector<uint8_t>& canonical)
{
  BIO* rawBio = BIO_new_mem_buf(
    policy.publicKeyPem.data(), static_cast<int>(policy.publicKeyPem.size()));
  if (rawBio == nullptr) {
    fail(artifact_manifest_error::TrustPolicyRejected,
         "cannot allocate publisher public-key parser");
  }
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(rawBio, &BIO_free);
  EVP_PKEY* rawKey = PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
  if (rawKey == nullptr) {
    fail(artifact_manifest_error::TrustPolicyRejected,
         "configured publisher public key is invalid");
  }
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, &EVP_PKEY_free);

  const auto algorithm = signedRoot.root.signatureAlgorithm;
  const auto keyType = EVP_PKEY_base_id(key.get());
  if ((algorithm == "rsa-sha256" && keyType != EVP_PKEY_RSA) ||
      (algorithm == "ecdsa-sha256" && keyType != EVP_PKEY_EC) ||
      (algorithm == "ed25519" && keyType != EVP_PKEY_ED25519)) {
    fail(artifact_manifest_error::TrustPolicyRejected,
         "publisher key type does not match negotiated signature algorithm");
  }

  EVP_MD_CTX* rawContext = EVP_MD_CTX_new();
  if (rawContext == nullptr) {
    fail(artifact_manifest_error::TrustPolicyRejected,
         "cannot allocate signature verification context");
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
    rawContext, &EVP_MD_CTX_free);
  int result = 0;
  if (algorithm == "ed25519") {
    if (EVP_DigestVerifyInit(
          context.get(), nullptr, nullptr, nullptr, key.get()) == 1) {
      result = EVP_DigestVerify(
        context.get(), signedRoot.signatureValue.data(),
        signedRoot.signatureValue.size(), canonical.data(), canonical.size());
    }
  }
  else if (EVP_DigestVerifyInit(
             context.get(), nullptr, EVP_sha256(), nullptr, key.get()) == 1 &&
           EVP_DigestVerifyUpdate(
             context.get(), canonical.data(), canonical.size()) == 1) {
    result = EVP_DigestVerifyFinal(
      context.get(), signedRoot.signatureValue.data(),
      signedRoot.signatureValue.size());
  }
  if (result != 1) {
    fail(artifact_manifest_error::InvalidSignature,
         "publisher root signature verification failed");
  }
}

std::string
replaceToken(std::string value, const std::string& token,
             const std::string& replacement)
{
  const auto position = value.find(token);
  if (position == std::string::npos ||
      value.find(token, position + token.size()) != std::string::npos) {
    fail(artifact_error::InvalidManifest,
         "naming template token must occur exactly once");
  }
  value.replace(position, token.size(), replacement);
  return value;
}

} // namespace

void
ArtifactManifestTrustPolicy::validate(const ArtifactLimits& limits) const
{
  validateName(trustedPublisherIdentity, limits, "trustedPublisherIdentity");
  validateName(trustedKeyLocator, limits, "trustedKeyLocator");
  validateIdentifier(
    policyEpoch, 256, "policyEpoch", artifact_manifest_error::TrustPolicyRejected);
  if (evaluationTimeMs == 0 || publicKeyPem.empty() ||
      publicKeyPem.size() > MAX_PUBLIC_KEY_PEM_BYTES ||
      publicKeyPem.find('\0') != std::string::npos) {
    fail(artifact_manifest_error::TrustPolicyRejected,
         "trust policy time or public key is invalid");
  }
  validateUniqueStrings(
    allowedDigestAlgorithms, 16, "allowedDigestAlgorithms",
    artifact_manifest_error::TrustPolicyRejected);
  validateUniqueStrings(
    allowedSignatureAlgorithms, 16, "allowedSignatureAlgorithms",
    artifact_manifest_error::TrustPolicyRejected);
  for (const auto& algorithm : allowedDigestAlgorithms) {
    if (!isKnownDigestAlgorithm(algorithm)) {
      fail(artifact_error::UnsupportedAlgorithm,
           "trust policy contains unsupported digest algorithm");
    }
  }
  for (const auto& algorithm : allowedSignatureAlgorithms) {
    if (!isPublicRootSignatureAlgorithm(algorithm)) {
      fail(artifact_error::UnsupportedAlgorithm,
           "trust policy contains unsupported signature algorithm");
    }
  }
  if (supportedCriticalExtensions.size() > limits.maxCriticalExtensions ||
      revokedKeyLocators.size() > 1024) {
    fail(artifact_error::LimitExceeded,
         "trust policy extension or revocation list exceeds limits");
  }
  std::set<std::string> extensions;
  for (const auto& extension : supportedCriticalExtensions) {
    validateIdentifier(
      extension, 128, "supportedCriticalExtension",
      artifact_manifest_error::TrustPolicyRejected);
    if (!extensions.insert(extension).second) {
      fail(artifact_manifest_error::TrustPolicyRejected,
           "duplicate supported critical extension");
    }
  }
  std::set<std::string> revoked;
  for (const auto& keyLocator : revokedKeyLocators) {
    validateName(keyLocator, limits, "revokedKeyLocator");
    if (!revoked.insert(keyLocator).second) {
      fail(artifact_manifest_error::TrustPolicyRejected,
           "duplicate revoked key locator");
    }
  }
}

std::string
artifactSha256Hex(const std::vector<uint8_t>& bytes)
{
  std::array<uint8_t, EVP_MAX_MD_SIZE> digest{};
  unsigned int digestSize = 0;
  EVP_MD_CTX* rawContext = EVP_MD_CTX_new();
  if (rawContext == nullptr ||
      EVP_DigestInit_ex(rawContext, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(rawContext, bytes.data(), bytes.size()) != 1 ||
      EVP_DigestFinal_ex(rawContext, digest.data(), &digestSize) != 1) {
    if (rawContext != nullptr) {
      EVP_MD_CTX_free(rawContext);
    }
    fail(artifact_manifest_error::DigestMismatch,
         "SHA-256 computation failed");
  }
  EVP_MD_CTX_free(rawContext);
  static constexpr char HEX[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(digestSize * 2);
  for (unsigned int i = 0; i < digestSize; ++i) {
    encoded.push_back(HEX[(digest[i] >> 4) & 0xf]);
    encoded.push_back(HEX[digest[i] & 0xf]);
  }
  return encoded;
}

std::vector<uint8_t>
canonicalRootManifestBytes(const ArtifactRootManifest& root,
                           const ArtifactLimits& limits)
{
  root.validate(1, limits);
  const auto encoded = encodeRootCanonicalUnchecked(root, limits);
  root.validate(encoded.size(), limits);
  return encoded;
}

std::vector<uint8_t>
encodeSignedArtifactRoot(const SignedArtifactRoot& signedRoot,
                         const ArtifactLimits& limits)
{
  const auto canonical = canonicalRootManifestBytes(signedRoot.root, limits);
  if (signedRoot.signatureValue.empty() ||
      signedRoot.signatureValue.size() > limits.maxSignatureBytes) {
    fail(artifact_error::LimitExceeded, "root signature size is outside limits");
  }
  Writer writer(
    limits.maxRootEncodedBytes + limits.maxSignatureBytes + 16);
  writer.magic(ROOT_WIRE_MAGIC);
  writer.bytes(canonical, limits.maxRootEncodedBytes);
  writer.bytes(signedRoot.signatureValue, limits.maxSignatureBytes);
  return writer.finish();
}

SignedArtifactRoot
decodeSignedArtifactRoot(const std::vector<uint8_t>& wire,
                         const ArtifactLimits& limits)
{
  if (wire.empty() ||
      wire.size() > limits.maxRootEncodedBytes + limits.maxSignatureBytes + 16) {
    fail(artifact_error::LimitExceeded, "signed root encoded size is outside limits");
  }
  Reader reader(wire);
  reader.magic(ROOT_WIRE_MAGIC);
  const auto canonical = reader.bytes(limits.maxRootEncodedBytes);
  SignedArtifactRoot signedRoot;
  signedRoot.root = decodeRootCanonical(canonical, limits);
  signedRoot.signatureValue = reader.bytes(limits.maxSignatureBytes);
  reader.done();
  if (signedRoot.signatureValue.empty()) {
    fail(artifact_manifest_error::MalformedEncoding,
         "signed root has an empty signature");
  }
  return signedRoot;
}

std::vector<uint8_t>
canonicalManifestPageBytes(const ArtifactManifestPage& page,
                           const ArtifactLimits& limits)
{
  auto validationPage = page;
  if (validationPage.pageDigest.empty()) {
    validationPage.pageDigest.assign(64, '0');
  }
  validationPage.validate(1, limits);
  const auto encoded = encodePageCanonicalUnchecked(page, limits);
  validationPage.validate(encoded.size(), limits);
  return encoded;
}

std::vector<uint8_t>
encodeArtifactManifestPage(const ArtifactManifestPage& page,
                           const ArtifactLimits& limits)
{
  const auto canonical = canonicalManifestPageBytes(page, limits);
  page.validate(canonical.size() + page.pageDigest.size() + 12, limits);
  Writer writer(limits.maxPageEncodedBytes);
  writer.magic(PAGE_WIRE_MAGIC);
  writer.bytes(canonical, limits.maxPageEncodedBytes);
  writer.string(page.pageDigest, 128);
  return writer.finish();
}

ArtifactManifestPage
decodeArtifactManifestPage(const std::vector<uint8_t>& wire,
                           const ArtifactLimits& limits)
{
  if (wire.empty() || wire.size() > limits.maxPageEncodedBytes) {
    fail(artifact_error::LimitExceeded, "manifest page wire size is outside limits");
  }
  Reader reader(wire);
  reader.magic(PAGE_WIRE_MAGIC);
  const auto canonical = reader.bytes(limits.maxPageEncodedBytes);
  auto page = decodePageCanonical(canonical, limits);
  page.pageDigest = reader.string(128);
  reader.done();
  page.validate(wire.size(), limits);
  return page;
}

std::string
deriveManifestPageName(const ArtifactRootManifest& root,
                       const std::string& pageDigest)
{
  validateDigest(root.manifestRootDigestAlgorithm, pageDigest, "pageDigest");
  return root.artifact.rootManifestName + "/page/" +
         root.manifestRootDigestAlgorithm + "=" + pageDigest;
}

std::string
deriveArtifactDataName(const ArtifactRootManifest& root,
                       uint64_t chunkIndex, uint64_t segment)
{
  auto name = replaceToken(
    root.namingTemplate, "{chunk}", std::to_string(chunkIndex));
  name = replaceToken(name, "{segment}", std::to_string(segment));
  ArtifactLimits limits;
  validateName(name, limits, "derivedDataName");
  if (name.compare(0, root.artifact.logicalName.size(),
                   root.artifact.logicalName) != 0 ||
      (name.size() > root.artifact.logicalName.size() &&
       name[root.artifact.logicalName.size()] != '/')) {
    fail(artifact_manifest_error::Substitution,
         "derived Data name escapes authenticated logical naming scope");
  }
  return name;
}

void
verifySignedArtifactRoot(const SignedArtifactRoot& signedRoot,
                         const ArtifactReference& expectedArtifact,
                         const ArtifactCapability& capability,
                         const ArtifactManifestTrustPolicy& policy,
                         const ArtifactLimits& limits)
{
  const auto canonical = canonicalRootManifestBytes(signedRoot.root, limits);
  if (signedRoot.signatureValue.empty() ||
      signedRoot.signatureValue.size() > limits.maxSignatureBytes) {
    fail(artifact_error::LimitExceeded, "root signature size is outside limits");
  }
  expectedArtifact.validate(limits);
  capability.validate(limits);
  policy.validate(limits);

  const auto& root = signedRoot.root;
  if (!sameArtifact(root.artifact, expectedArtifact)) {
    fail(artifact_manifest_error::Substitution,
         "signed root does not match the requested artifact identity");
  }
  if (root.artifact.publisherIdentity != policy.trustedPublisherIdentity ||
      root.publisherKeyLocator != policy.trustedKeyLocator) {
    fail(artifact_manifest_error::TrustPolicyRejected,
         "publisher identity or key locator is outside configured trust policy");
  }
  if (root.artifact.policyEpoch != policy.policyEpoch ||
      capability.policyEpoch != policy.policyEpoch) {
    fail(artifact_manifest_error::ExpiredPolicy,
         "manifest, capability, and trust policy epochs do not match");
  }
  if (policy.evaluationTimeMs < root.createdAtMs ||
      (root.expiresAtMs != 0 && policy.evaluationTimeMs >= root.expiresAtMs)) {
    fail(artifact_manifest_error::ExpiredPolicy,
         "root manifest is not valid at the trust-policy evaluation time");
  }
  if (contains(policy.revokedKeyLocators, root.publisherKeyLocator)) {
    fail(artifact_manifest_error::RevokedPublisher,
         "publisher key is revoked in the selected policy epoch");
  }
  if (!contains(policy.allowedDigestAlgorithms, root.artifact.digestAlgorithm) ||
      !contains(policy.allowedDigestAlgorithms,
                root.manifestRootDigestAlgorithm) ||
      !contains(policy.allowedSignatureAlgorithms, root.signatureAlgorithm) ||
      !capability.supports(root.artifact, root.signatureAlgorithm)) {
    fail(artifact_manifest_error::Downgrade,
         "manifest algorithms or format were not jointly negotiated");
  }
  if (root.manifestRootDigestAlgorithm != root.artifact.digestAlgorithm) {
    fail(artifact_manifest_error::Downgrade,
         "one negotiated digest algorithm must bind pages, chunks, and artifact");
  }
  for (const auto& extension : root.criticalExtensions) {
    if (!contains(policy.supportedCriticalExtensions, extension)) {
      fail(artifact_manifest_error::UnsupportedCriticalField,
           "unsupported critical root extension " + extension);
    }
  }
  if (root.namingTemplate.compare(
        0, root.artifact.logicalName.size(), root.artifact.logicalName) != 0 ||
      (root.namingTemplate.size() > root.artifact.logicalName.size() &&
       root.namingTemplate[root.artifact.logicalName.size()] != '/')) {
    fail(artifact_manifest_error::Substitution,
         "authenticated naming template escapes artifact naming scope");
  }
  if (limits.maxCryptographicOperations < 1) {
    fail(artifact_manifest_error::CryptoBudgetExceeded,
         "trust policy does not permit root signature verification");
  }
  verifyPublicKeySignature(signedRoot, policy, canonical);
}

ArtifactManifestVerificationResult
verifyArtifactManifestGraph(
  const SignedArtifactRoot& signedRoot,
  const ArtifactReference& expectedArtifact,
  const std::vector<ArtifactManifestPage>& pages,
  const std::vector<ArtifactChunk>& chunks,
  const ArtifactCapability& capability,
  const ArtifactManifestTrustPolicy& policy,
  const ArtifactLimits& limits)
{
  if (pages.size() > limits.maxManifestPages ||
      chunks.size() > limits.maxManifestChunks) {
    fail(artifact_error::LimitExceeded,
         "manifest graph exceeds page or chunk count limits");
  }
  const uint64_t cryptographicWork =
    1 + static_cast<uint64_t>(pages.size());
  if (cryptographicWork > limits.maxCryptographicOperations) {
    fail(artifact_manifest_error::CryptoBudgetExceeded,
         "manifest graph exceeds cryptographic work budget");
  }
  verifySignedArtifactRoot(
    signedRoot, expectedArtifact, capability, policy, limits);
  const auto& root = signedRoot.root;

  ArtifactManifestVerificationResult result;
  result.artifact = root.artifact;
  result.asymmetricVerificationCount = 1;

  if (root.artifact.sizeBytes == 0) {
    if (!pages.empty() || !chunks.empty() ||
        root.manifestRootDigest !=
          artifactSha256Hex(std::vector<uint8_t>{})) {
      fail(artifact_manifest_error::InvalidGraph,
           "empty artifact must use the canonical empty hierarchy");
    }
    return result;
  }
  if (pages.empty() || chunks.empty()) {
    fail(artifact_manifest_error::InvalidGraph,
         "non-empty artifact requires pages and chunks");
  }
  if (root.chunkBytes % root.packetPayloadBytes != 0) {
    fail(artifact_manifest_error::InvalidGraph,
         "chunk geometry must align to signed packet geometry");
  }

  std::unordered_map<std::string, const ArtifactManifestPage*> pageByDigest;
  for (const auto& page : pages) {
    page.validate(encodeArtifactManifestPage(page, limits).size(), limits);
    if (page.pageDigestAlgorithm != root.manifestRootDigestAlgorithm) {
      fail(artifact_manifest_error::Downgrade,
           "manifest page digest algorithm differs from signed root");
    }
    const auto actualDigest = artifactSha256Hex(
      canonicalManifestPageBytes(page, limits));
    ++result.digestVerificationCount;
    if (actualDigest != page.pageDigest) {
      fail(artifact_manifest_error::DigestMismatch,
           "manifest page bytes do not match declared content digest");
    }
    if (!pageByDigest.emplace(page.pageDigest, &page).second) {
      fail(artifact_manifest_error::InvalidGraph,
           "manifest graph contains duplicate page digest");
    }
  }

  std::unordered_map<uint64_t, const ArtifactChunk*> chunkByIndex;
  for (const auto& chunk : chunks) {
    chunk.validate(root.artifact, limits);
    if (chunk.digestAlgorithm != root.artifact.digestAlgorithm ||
        !chunkByIndex.emplace(chunk.index, &chunk).second) {
      fail(artifact_manifest_error::InvalidGraph,
           "chunk algorithm or index conflicts with signed root");
    }
    if (chunk.index > std::numeric_limits<uint64_t>::max() / root.chunkBytes) {
      fail(artifact_manifest_error::InvalidGraph,
           "chunk index overflows signed geometry");
    }
    const auto geometryOffset = chunk.index * root.chunkBytes;
    if (geometryOffset >= root.artifact.sizeBytes ||
        chunk.offsetBytes != geometryOffset ||
        chunk.lengthBytes != std::min<uint64_t>(
          root.chunkBytes, root.artifact.sizeBytes - geometryOffset)) {
      fail(artifact_manifest_error::Substitution,
           "chunk range differs from signed chunk geometry");
    }
    // Segment numbers are local to the authenticated {chunk} name component.
    const auto expectedFirst = uint64_t{0};
    const auto expectedFinal =
      (chunk.lengthBytes - 1) / root.packetPayloadBytes;
    if (chunk.firstSegment != expectedFirst ||
        chunk.finalSegment != expectedFinal) {
      fail(artifact_manifest_error::InvalidGraph,
           "chunk segment coordinates do not match signed packet geometry");
    }
  }

  std::unordered_set<std::string> visitingPages;
  std::unordered_set<std::string> visitedPages;
  std::unordered_set<uint64_t> visitedChunks;
  std::function<void(const std::string&, uint32_t, uint64_t, uint64_t)> visit;
  visit = [&] (const std::string& digest, uint32_t expectedDepth,
               uint64_t expectedOffset, uint64_t expectedLength) {
    if (visitingPages.count(digest) != 0) {
      fail(artifact_manifest_error::Cycle,
           "manifest hierarchy contains a page cycle");
    }
    if (visitedPages.count(digest) != 0) {
      fail(artifact_manifest_error::InvalidGraph,
           "manifest page is referenced more than once");
    }
    const auto found = pageByDigest.find(digest);
    if (found == pageByDigest.end()) {
      fail(artifact_manifest_error::InvalidGraph,
           "manifest hierarchy references a missing page");
    }
    const auto& page = *found->second;
    if (page.depth != expectedDepth || page.offsetBytes != expectedOffset ||
        page.lengthBytes != expectedLength) {
      fail(artifact_manifest_error::Substitution,
           "manifest page coordinates differ from authenticated parent");
    }
    visitingPages.insert(digest);
    for (const auto& child : page.children) {
      if (child.digestAlgorithm != root.manifestRootDigestAlgorithm) {
        fail(artifact_manifest_error::Downgrade,
             "manifest child digest algorithm differs from signed root");
      }
      if (child.kind == "page") {
        visit(child.digest, expectedDepth + 1,
              child.offsetBytes, child.lengthBytes);
      }
      else {
        const auto chunkFound = chunkByIndex.find(child.index);
        if (chunkFound == chunkByIndex.end()) {
          fail(artifact_manifest_error::InvalidGraph,
               "manifest hierarchy references a missing chunk");
        }
        const auto& chunk = *chunkFound->second;
        if (chunk.offsetBytes != child.offsetBytes ||
            chunk.lengthBytes != child.lengthBytes ||
            chunk.digest != child.digest ||
            !visitedChunks.insert(chunk.index).second) {
          fail(artifact_manifest_error::Substitution,
               "chunk identity differs from authenticated manifest entry");
        }
      }
    }
    visitingPages.erase(digest);
    visitedPages.insert(digest);
    result.derivedPageNames.push_back(deriveManifestPageName(root, digest));
  };

  visit(root.manifestRootDigest, 0, 0, root.artifact.sizeBytes);
  if (visitedPages.size() != pages.size() ||
      visitedChunks.size() != chunks.size()) {
    fail(artifact_manifest_error::InvalidGraph,
         "manifest graph contains unreachable pages or chunks");
  }

  std::vector<const ArtifactChunk*> orderedChunks;
  orderedChunks.reserve(chunks.size());
  for (const auto& chunk : chunks) {
    orderedChunks.push_back(&chunk);
  }
  std::sort(orderedChunks.begin(), orderedChunks.end(),
            [] (const auto* left, const auto* right) {
              return left->offsetBytes < right->offsetBytes;
            });
  uint64_t expectedOffset = 0;
  for (const auto* chunk : orderedChunks) {
    if (chunk->offsetBytes != expectedOffset) {
      fail(artifact_manifest_error::InvalidGraph,
           "chunks do not cover the artifact exactly once");
    }
    expectedOffset += chunk->lengthBytes;
  }
  if (expectedOffset != root.artifact.sizeBytes) {
    fail(artifact_manifest_error::InvalidGraph,
         "chunks do not cover the complete artifact");
  }

  result.verifiedPageCount = visitedPages.size();
  result.verifiedChunkCount = visitedChunks.size();
  return result;
}

void
verifyArtifactChunkPayload(const ArtifactChunk& chunk,
                           const std::vector<uint8_t>& payload)
{
  validateDigest(chunk.digestAlgorithm, chunk.digest, "chunk digest");
  if (payload.size() != chunk.lengthBytes ||
      artifactSha256Hex(payload) != chunk.digest) {
    fail(artifact_manifest_error::DigestMismatch,
         "chunk payload length or digest does not match manifest");
  }
}

void
verifyArtifactPayload(const ArtifactReference& artifact,
                      const std::vector<uint8_t>& payload)
{
  artifact.validate();
  if (payload.size() != artifact.sizeBytes ||
      artifactSha256Hex(payload) != artifact.contentDigest) {
    fail(artifact_manifest_error::DigestMismatch,
         "artifact payload length or digest does not match signed root");
  }
}

void
validateArtifactResumeIdentity(const ArtifactReference& expectedArtifact,
                               const ArtifactRootManifest& expectedRoot,
                               const ArtifactReference& resumedArtifact,
                               const ArtifactRootManifest& resumedRoot)
{
  if (!sameArtifact(expectedArtifact, resumedArtifact) ||
      !sameArtifact(expectedRoot.artifact, resumedRoot.artifact) ||
      expectedRoot.packetPayloadBytes != resumedRoot.packetPayloadBytes ||
      expectedRoot.chunkBytes != resumedRoot.chunkBytes ||
      expectedRoot.namingTemplate != resumedRoot.namingTemplate ||
      expectedRoot.manifestRootDigestAlgorithm !=
        resumedRoot.manifestRootDigestAlgorithm ||
      expectedRoot.manifestRootDigest != resumedRoot.manifestRootDigest ||
      expectedRoot.signatureAlgorithm != resumedRoot.signatureAlgorithm ||
      expectedRoot.publisherKeyLocator != resumedRoot.publisherKeyLocator) {
    fail(artifact_manifest_error::MixedResume,
         "resume state belongs to a different manifest identity or geometry");
  }
}

} // namespace ndnsf_distributed_repo
