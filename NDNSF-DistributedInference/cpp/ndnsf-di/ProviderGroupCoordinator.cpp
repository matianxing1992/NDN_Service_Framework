#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderGroupCoordinator.hpp"

#include "ndn-service-framework/HybridMessageCrypto.hpp"

#include <ndn-cxx/name.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

using Bytes = ProviderGroupBytes;
constexpr std::size_t KEY_BYTES = 32;
constexpr std::size_t NONCE_BYTES = 12;
constexpr std::size_t DIGEST_BYTES = 32;
constexpr std::size_t MAX_STRING_BYTES = 1U << 20;

void
appendU64(Bytes& out, std::uint64_t value)
{
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void
appendString(Bytes& out, const std::string& value)
{
  if (value.size() > std::numeric_limits<std::uint64_t>::max()) {
    throw std::invalid_argument("ProviderGroupCoordinator string is too large");
  }
  appendU64(out, static_cast<std::uint64_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

void
appendBytes(Bytes& out, const Bytes& value)
{
  if (value.size() > std::numeric_limits<std::uint64_t>::max()) {
    throw std::invalid_argument("ProviderGroupCoordinator bytes are too large");
  }
  appendU64(out, static_cast<std::uint64_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

class WireCursor
{
public:
  explicit WireCursor(const Bytes& input)
    : m_input(input)
  {
  }

  std::uint64_t readU64()
  {
    require(8);
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value = (value << 8) | m_input[m_offset + i];
    }
    m_offset += 8;
    return value;
  }

  std::string readString(std::size_t maxBytes = MAX_STRING_BYTES)
  {
    const auto size = readU64();
    if (size > maxBytes || size > m_input.size() - m_offset) {
      throw std::invalid_argument("ProviderGroupCoordinator string exceeds bound");
    }
    std::string result(reinterpret_cast<const char*>(m_input.data() + m_offset),
                       static_cast<std::size_t>(size));
    m_offset += static_cast<std::size_t>(size);
    return result;
  }

  Bytes readBytes(std::size_t maxBytes)
  {
    const auto size = readU64();
    if (size > maxBytes || size > m_input.size() - m_offset) {
      throw std::invalid_argument("ProviderGroupCoordinator byte field exceeds bound");
    }
    Bytes result(m_input.begin() + static_cast<std::ptrdiff_t>(m_offset),
                 m_input.begin() + static_cast<std::ptrdiff_t>(m_offset + size));
    m_offset += static_cast<std::size_t>(size);
    return result;
  }

  bool atEnd() const noexcept
  {
    return m_offset == m_input.size();
  }

private:
  void require(std::size_t bytes) const
  {
    if (bytes > m_input.size() - m_offset) {
      throw std::invalid_argument("truncated ProviderGroupCoordinator wire");
    }
  }

private:
  const Bytes& m_input;
  std::size_t m_offset = 0;
};

void
appendStrings(Bytes& out, const std::vector<std::string>& values)
{
  appendU64(out, values.size());
  for (const auto& value : values) {
    appendString(out, value);
  }
}

Bytes
sha256(const Bytes& input)
{
  Bytes output(DIGEST_BYTES);
  unsigned int length = 0;
  auto* context = EVP_MD_CTX_new();
  if (context == nullptr ||
      EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, input.data(), input.size()) != 1 ||
      EVP_DigestFinal_ex(context, output.data(), &length) != 1 ||
      length != DIGEST_BYTES) {
    EVP_MD_CTX_free(context);
    throw std::runtime_error("ProviderGroupCoordinator SHA-256 failed");
  }
  EVP_MD_CTX_free(context);
  return output;
}

std::string
hex(const Bytes& input)
{
  static constexpr char HEX[] = "0123456789abcdef";
  const auto digest = sha256(input);
  std::string result;
  result.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    result.push_back(HEX[byte >> 4]);
    result.push_back(HEX[byte & 0x0f]);
  }
  return result;
}

Bytes
hmac(const Bytes& key, const Bytes& input)
{
  Bytes output(EVP_MAX_MD_SIZE);
  unsigned int length = 0;
  if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
           input.data(), input.size(), output.data(), &length) == nullptr ||
      length != DIGEST_BYTES) {
    throw std::runtime_error("ProviderGroupCoordinator HMAC failed");
  }
  output.resize(length);
  return output;
}

Bytes
defaultRandom(std::size_t size)
{
  Bytes output(size);
  if (size > 0 && RAND_bytes(output.data(), static_cast<int>(size)) != 1) {
    throw std::runtime_error("ProviderGroupCoordinator CSPRNG failed");
  }
  return output;
}

Bytes
hkdf32(const Bytes& key, const Bytes& info)
{
  const Bytes salt{'N', 'D', 'N', 'S', 'F', '_', 'D', 'A', 'T', 'A',
                   '_', 'V', '1', '/', 'H', 'K', 'D', 'F'};
  const auto pseudorandomKey = hmac(salt, key);
  Bytes expandInput = info;
  expandInput.push_back(1); // one SHA-256 block is sufficient for 32 bytes
  return hmac(pseudorandomKey, expandInput);
}

void
cleanse(Bytes& bytes)
{
  if (!bytes.empty()) {
    OPENSSL_cleanse(bytes.data(), bytes.size());
    bytes.clear();
    bytes.shrink_to_fit();
  }
}

void
requireNonEmpty(const std::string& value, const char* field)
{
  if (value.empty() || value.size() > MAX_STRING_BYTES) {
    throw std::invalid_argument(std::string("missing or oversized ") + field);
  }
}

} // namespace

void
GroupCapabilityV1::validate() const
{
  requireNonEmpty(requestId, "requestId");
  requireNonEmpty(attemptId, "attemptId");
  requireNonEmpty(planDigest, "planDigest");
  requireNonEmpty(groupId, "groupId");
  requireNonEmpty(epochKeyId, "epochKeyId");
  if (epoch == 0 || orderedMembers.empty() || permittedOperations.empty() ||
      maxInflightBytes == 0 || noProgressMs == 0 || hardDeadlineMs < noProgressMs ||
      sealerSignature.empty()) {
    throw std::invalid_argument("invalid GroupCapabilityV1 bounds or signature");
  }
  std::set<std::string> providers;
  std::set<std::uint64_t> ranks;
  for (const auto& member : orderedMembers) {
    requireNonEmpty(member.provider, "member.provider");
    requireNonEmpty(member.offerDigest, "member.offerDigest");
    requireNonEmpty(member.endpointPrefix, "member.endpointPrefix");
    if (!providers.insert(member.provider).second || !ranks.insert(member.rank).second) {
      throw std::invalid_argument("duplicate GroupCapabilityV1 member");
    }
  }
  std::set<std::uint64_t> operationIndexes;
  std::set<std::string> memberRanks;
  for (const auto rank : ranks) {
    memberRanks.insert(std::to_string(rank));
  }
  if (permittedOperations.size() > (1U << 20)) {
    throw std::invalid_argument("too many GroupCapabilityV1 operations");
  }
  for (const auto& operation : permittedOperations) {
    requireNonEmpty(operation.kind, "operation.kind");
    requireNonEmpty(operation.tensorLayoutDigest, "operation.tensorLayoutDigest");
    if (operation.maxBytes == 0 || operation.maxSegments == 0 ||
        operation.maxSegments > (1U << 20) ||
        !operationIndexes.insert(operation.operationIndex).second) {
      throw std::invalid_argument("invalid GroupCapabilityV1 operation bounds");
    }
    const std::set<std::string> producerRanks(
      operation.producerRanks.begin(), operation.producerRanks.end());
    const std::set<std::string> consumerRanks(
      operation.consumerRanks.begin(), operation.consumerRanks.end());
    if (producerRanks.empty() || consumerRanks.empty() ||
        producerRanks.size() != operation.producerRanks.size() ||
        consumerRanks.size() != operation.consumerRanks.size() ||
        !std::includes(memberRanks.begin(), memberRanks.end(),
                       producerRanks.begin(), producerRanks.end()) ||
        !std::includes(memberRanks.begin(), memberRanks.end(),
                       consumerRanks.begin(), consumerRanks.end())) {
      throw std::invalid_argument(
        "GroupCapabilityV1 operation rank is not a unique group member");
    }
  }
  if (capabilityDigest.empty() ||
      wrappedEpochKeyDigestByProvider.size() != providers.size() ||
      wrappedEpochKeyByProvider.empty() ||
      wrappedEpochKeyByProvider.size() > providers.size()) {
    throw std::invalid_argument("invalid GroupCapabilityV1 key projection");
  }
  for (const auto& provider : providers) {
    const auto commitment = wrappedEpochKeyDigestByProvider.find(provider);
    if (commitment == wrappedEpochKeyDigestByProvider.end() ||
        commitment->second.empty()) {
      throw std::invalid_argument("missing GroupCapabilityV1 key commitment");
    }
  }
  for (const auto& [provider, wrapped] : wrappedEpochKeyByProvider) {
    const auto commitment = wrappedEpochKeyDigestByProvider.find(provider);
    if (providers.count(provider) == 0 || wrapped.empty() ||
        commitment == wrappedEpochKeyDigestByProvider.end() ||
        commitment->second != hex(wrapped)) {
      throw std::invalid_argument("GroupCapabilityV1 key commitment mismatch");
    }
  }
}

ProviderGroupBytes
GroupCapabilityV1::canonicalBytes(bool includeDigest) const
{
  Bytes output;
  appendString(output, "GroupCapabilityV1");
  appendString(output, requestId);
  appendString(output, attemptId);
  appendString(output, planDigest);
  appendString(output, groupId);
  appendU64(output, epoch);
  appendU64(output, orderedMembers.size());
  for (const auto& member : orderedMembers) {
    appendString(output, member.provider);
    appendU64(output, member.rank);
    appendString(output, member.offerDigest);
    appendString(output, member.endpointPrefix);
  }
  appendU64(output, permittedOperations.size());
  for (const auto& operation : permittedOperations) {
    appendU64(output, operation.operationIndex);
    appendString(output, operation.kind);
    appendStrings(output, operation.producerRanks);
    appendStrings(output, operation.consumerRanks);
    appendString(output, operation.tensorLayoutDigest);
    appendU64(output, operation.maxBytes);
    appendU64(output, operation.maxSegments);
  }
  appendU64(output, maxInflightBytes);
  appendU64(output, noProgressMs);
  appendU64(output, hardDeadlineMs);
  appendString(output, epochKeyId);
  appendU64(output, wrappedEpochKeyDigestByProvider.size());
  for (const auto& entry : wrappedEpochKeyDigestByProvider) {
    appendString(output, entry.first);
    appendString(output, entry.second);
  }
  // The sealer signs these bytes; the signature itself is intentionally never
  // included in the signed representation.
  if (includeDigest) {
    appendString(output, capabilityDigest);
  }
  return output;
}

GroupCapabilityV1
GroupCapabilityV1::projectForProvider(const std::string& provider) const
{
  validate();
  if (wrappedEpochKeyDigestByProvider.count(provider) == 0) {
    throw std::invalid_argument("Provider is not a GroupCapabilityV1 member");
  }
  const auto wrapped = wrappedEpochKeyByProvider.find(provider);
  if (wrapped == wrappedEpochKeyByProvider.end()) {
    throw std::invalid_argument("Provider envelope is unavailable for projection");
  }
  auto projected = *this;
  projected.wrappedEpochKeyByProvider = {{provider, wrapped->second}};
  projected.validate();
  return projected;
}

void
CollectiveOperationManifestV1::validate() const
{
  requireNonEmpty(capabilityDigest, "manifest.capabilityDigest");
  requireNonEmpty(epochKeyId, "manifest.epochKeyId");
  requireNonEmpty(requestId, "manifest.requestId");
  requireNonEmpty(attemptId, "manifest.attemptId");
  requireNonEmpty(planDigest, "manifest.planDigest");
  requireNonEmpty(groupId, "manifest.groupId");
  requireNonEmpty(operationKind, "manifest.operationKind");
  requireNonEmpty(producerRank, "manifest.producerRank");
  requireNonEmpty(tensorDigest, "manifest.tensorDigest");
  if (epoch == 0 || totalBytes == 0 || segmentSize == 0 || segmentCount == 0 ||
      segmentCount != orderedSegmentDigests.size() || noProgressMs == 0 ||
      hardDeadlineMs < noProgressMs || producerSignature.empty()) {
    throw std::invalid_argument("invalid CollectiveOperationManifestV1 bounds");
  }
  if (segmentCount > (1U << 20)) {
    throw std::invalid_argument("manifest segment count exceeds bound");
  }
  for (const auto& digest : orderedSegmentDigests) {
    requireNonEmpty(digest, "manifest.segmentDigest");
  }
}

ProviderGroupBytes
CollectiveOperationManifestV1::canonicalBytes(bool includeSignature) const
{
  Bytes output;
  appendString(output, "CollectiveOperationManifestV1");
  appendString(output, capabilityDigest);
  appendString(output, epochKeyId);
  appendString(output, requestId);
  appendString(output, attemptId);
  appendString(output, planDigest);
  appendString(output, groupId);
  appendU64(output, epoch);
  appendU64(output, operationIndex);
  appendString(output, operationKind);
  appendString(output, producerRank);
  appendString(output, sourceLayoutDigest);
  appendString(output, targetLayoutDigest);
  appendString(output, tensorDigest);
  appendU64(output, totalBytes);
  appendU64(output, segmentSize);
  appendU64(output, segmentCount);
  appendStrings(output, orderedSegmentDigests);
  appendU64(output, createdAtMs);
  appendU64(output, noProgressMs);
  appendU64(output, hardDeadlineMs);
  if (includeSignature) {
    appendBytes(output, producerSignature);
  }
  return output;
}

std::string
CollectiveOperationManifestV1::digest() const
{
  return hex(canonicalBytes(false));
}

ProviderGroupCoordinator::ProviderGroupCoordinator(
  ProviderGroupCoordinatorOptions options)
  : m_options(std::move(options))
{
  if (!m_options.randomBytes) {
    m_options.randomBytes = defaultRandom;
  }
}

ProviderGroupCoordinator::~ProviderGroupCoordinator()
{
  clearEpochKey();
}

GroupCapabilityV1
ProviderGroupCoordinator::createCapability(
  std::string requestId,
  std::string attemptId,
  std::string planDigest,
  std::string groupId,
  std::uint64_t epoch,
  std::vector<GroupMemberV1> orderedMembers,
  std::vector<GroupOperationV1> permittedOperations,
  std::uint64_t maxInflightBytes,
  std::uint64_t noProgressMs,
  std::uint64_t hardDeadlineMs)
{
  if (!m_options.wrapEpochKey) {
    throw std::invalid_argument(
      "ProviderGroupCoordinator requires epoch-key wrapping");
  }
  if (m_hasCapability && !m_cancelled && !m_failed) {
    throw std::logic_error("ProviderGroupCoordinator already has an active capability");
  }
  clearEpochKey();
  m_capability = GroupCapabilityV1{};
  m_capability.requestId = std::move(requestId);
  m_capability.attemptId = std::move(attemptId);
  m_capability.planDigest = std::move(planDigest);
  m_capability.groupId = std::move(groupId);
  m_capability.epoch = epoch;
  m_capability.orderedMembers = std::move(orderedMembers);
  m_capability.permittedOperations = std::move(permittedOperations);
  m_capability.maxInflightBytes = maxInflightBytes;
  m_capability.noProgressMs = noProgressMs;
  m_capability.hardDeadlineMs = hardDeadlineMs;
  m_epochKey = m_options.randomBytes(KEY_BYTES);
  if (m_epochKey.size() != KEY_BYTES) {
    throw std::runtime_error("epoch key generator returned the wrong size");
  }
  m_capability.epochKeyId = hex(m_epochKey);
  for (const auto& member : m_capability.orderedMembers) {
    auto wrapped = m_options.wrapEpochKey(member.provider, m_epochKey);
    if (wrapped.empty()) {
      throw std::runtime_error("epoch key wrapping returned an empty envelope");
    }
    m_capability.wrappedEpochKeyDigestByProvider.emplace(
      member.provider, hex(wrapped));
    m_capability.wrappedEpochKeyByProvider.emplace(member.provider, std::move(wrapped));
  }
  m_capability.capabilityDigest = hex(m_capability.canonicalBytes(false));
  const auto capabilityBytes = m_capability.canonicalBytes(true);
  m_capability.sealerSignature = m_options.signCapability
    ? m_options.signCapability(capabilityBytes)
    : hmac(m_epochKey, capabilityBytes);
  if (m_capability.sealerSignature.empty()) {
    throw std::runtime_error("capability signer returned an empty signature");
  }
  m_capability.validate();
  m_hasCapability = true;
  m_cancelled = false;
  m_failed = false;
  m_terminalReason.clear();
  m_lastProgressMs = 0;
  m_groupStartedAtMs = 0;
  m_replayWindows.clear();
  return m_capability;
}

void
ProviderGroupCoordinator::installCapability(GroupCapabilityV1 capability,
                                             ProviderGroupBytes epochKey,
                                             bool verifySignature)
{
  capability.validate();
  if (!m_options.localProvider.empty() &&
      (capability.wrappedEpochKeyByProvider.size() != 1 ||
       capability.wrappedEpochKeyByProvider.count(m_options.localProvider) != 1)) {
    throw std::invalid_argument(
      "Provider must receive exactly its own GroupCapabilityV1 key projection");
  }
  if (capability.capabilityDigest != hex(capability.canonicalBytes(false))) {
    throw std::runtime_error("capability digest mismatch");
  }
  if (epochKey.empty()) {
    if (m_options.localProvider.empty() || !m_options.unwrapEpochKey) {
      throw std::invalid_argument(
        "an empty epoch key requires localProvider and an unwrap callback");
    }
    const auto it = capability.wrappedEpochKeyByProvider.find(m_options.localProvider);
    if (it == capability.wrappedEpochKeyByProvider.end()) {
      throw std::invalid_argument("capability has no wrapped key for localProvider");
    }
    epochKey = m_options.unwrapEpochKey(m_options.localProvider, it->second);
  }
  if (epochKey.size() != KEY_BYTES) {
    cleanse(epochKey);
    throw std::invalid_argument("installed epoch key must be 256 bits");
  }
  if (hex(epochKey) != capability.epochKeyId) {
    cleanse(epochKey);
    throw std::runtime_error("installed epoch key identifier mismatch");
  }
  if (verifySignature) {
    const auto capabilityBytes = capability.canonicalBytes(true);
    const auto verified = m_options.verifyCapability
      ? m_options.verifyCapability(capabilityBytes, capability.sealerSignature)
      : [&] {
          const auto expected = hmac(epochKey, capabilityBytes);
          return expected.size() == capability.sealerSignature.size() &&
                 CRYPTO_memcmp(expected.data(), capability.sealerSignature.data(),
                               expected.size()) == 0;
        }();
    if (!verified) {
      cleanse(epochKey);
      throw std::runtime_error("capability authenticator verification failed");
    }
  }
  clearEpochKey();
  m_capability = std::move(capability);
  m_epochKey = std::move(epochKey);
  m_hasCapability = true;
  m_cancelled = false;
  m_failed = false;
  m_terminalReason.clear();
  m_lastProgressMs = 0;
  m_groupStartedAtMs = 0;
  m_replayWindows.clear();
}

const GroupCapabilityV1&
ProviderGroupCoordinator::capability() const
{
  if (!m_hasCapability) {
    throw std::logic_error("ProviderGroupCoordinator has no capability");
  }
  return m_capability;
}

bool
ProviderGroupCoordinator::hasCapability() const noexcept
{
  return m_hasCapability;
}

ProviderGroupBytes
ProviderGroupCoordinator::epochKeyForProvider(const std::string& provider) const
{
  if (!m_hasCapability) {
    throw std::logic_error("ProviderGroupCoordinator has no capability");
  }
  if (terminal()) {
    throw std::runtime_error("NDNSF_DATA_V1 group is terminal");
  }
  // A runtime-bound Provider may only access the wrapped epoch key addressed
  // to its own identity.  The producer coordinator leaves localProvider
  // empty and may use the in-memory epoch key for any group member when it
  // constructs outbound operations; an installed Provider must not turn its
  // local epoch key into a cross-Provider key oracle.
  if (!m_options.localProvider.empty() && provider != m_options.localProvider) {
    throw std::runtime_error("Provider epoch-key access targets another Provider");
  }
  if (std::none_of(m_capability.orderedMembers.begin(),
                   m_capability.orderedMembers.end(),
                   [&provider] (const auto& member) { return member.provider == provider; })) {
    throw std::invalid_argument("provider is not a group member");
  }
  if (!m_epochKey.empty()) {
    return m_epochKey;
  }
  if (!m_options.unwrapEpochKey) {
    throw std::runtime_error("no Provider epoch-key unwrap callback configured");
  }
  const auto it = m_capability.wrappedEpochKeyByProvider.find(provider);
  if (it == m_capability.wrappedEpochKeyByProvider.end()) {
    throw std::runtime_error("missing Provider wrapped epoch key");
  }
  auto key = m_options.unwrapEpochKey(provider, it->second);
  if (key.size() != KEY_BYTES) {
    cleanse(key);
    throw std::runtime_error("Provider epoch-key unwrap returned the wrong size");
  }
  return key;
}

ProviderGroupBytes
ProviderGroupCoordinator::encodeCapability(const GroupCapabilityV1& capability)
{
  capability.validate();
  if (capability.capabilityDigest != hex(capability.canonicalBytes(false))) {
    throw std::invalid_argument("capability digest mismatch");
  }
  auto wire = capability.canonicalBytes(true);
  appendU64(wire, capability.wrappedEpochKeyByProvider.size());
  for (const auto& entry : capability.wrappedEpochKeyByProvider) {
    appendString(wire, entry.first);
    appendBytes(wire, entry.second);
  }
  appendBytes(wire, capability.sealerSignature);
  if (wire.size() > (16U << 20)) {
    throw std::invalid_argument("GroupCapabilityV1 wire exceeds bound");
  }
  return wire;
}

GroupCapabilityV1
ProviderGroupCoordinator::decodeCapability(const ProviderGroupBytes& wire)
{
  if (wire.size() > (16U << 20)) {
    throw std::invalid_argument("GroupCapabilityV1 wire exceeds bound");
  }
  WireCursor cursor(wire);
  if (cursor.readString() != "GroupCapabilityV1") {
    throw std::invalid_argument("invalid GroupCapabilityV1 marker");
  }

  GroupCapabilityV1 capability;
  capability.requestId = cursor.readString();
  capability.attemptId = cursor.readString();
  capability.planDigest = cursor.readString();
  capability.groupId = cursor.readString();
  capability.epoch = cursor.readU64();

  const auto memberCount = cursor.readU64();
  if (memberCount == 0 || memberCount > (1U << 20)) {
    throw std::invalid_argument("GroupCapabilityV1 member count exceeds bound");
  }
  capability.orderedMembers.reserve(static_cast<std::size_t>(memberCount));
  for (std::uint64_t index = 0; index < memberCount; ++index) {
    GroupMemberV1 member;
    member.provider = cursor.readString();
    member.rank = cursor.readU64();
    member.offerDigest = cursor.readString();
    member.endpointPrefix = cursor.readString();
    capability.orderedMembers.push_back(std::move(member));
  }

  const auto operationCount = cursor.readU64();
  if (operationCount == 0 || operationCount > (1U << 20)) {
    throw std::invalid_argument("GroupCapabilityV1 operation count exceeds bound");
  }
  capability.permittedOperations.reserve(
    static_cast<std::size_t>(operationCount));
  for (std::uint64_t index = 0; index < operationCount; ++index) {
    GroupOperationV1 operation;
    operation.operationIndex = cursor.readU64();
    operation.kind = cursor.readString();
    const auto producerCount = cursor.readU64();
    if (producerCount > (1U << 20)) {
      throw std::invalid_argument(
        "GroupCapabilityV1 producer rank count exceeds bound");
    }
    operation.producerRanks.reserve(static_cast<std::size_t>(producerCount));
    for (std::uint64_t rank = 0; rank < producerCount; ++rank) {
      operation.producerRanks.push_back(cursor.readString());
    }
    const auto consumerCount = cursor.readU64();
    if (consumerCount > (1U << 20)) {
      throw std::invalid_argument(
        "GroupCapabilityV1 consumer rank count exceeds bound");
    }
    operation.consumerRanks.reserve(static_cast<std::size_t>(consumerCount));
    for (std::uint64_t rank = 0; rank < consumerCount; ++rank) {
      operation.consumerRanks.push_back(cursor.readString());
    }
    operation.tensorLayoutDigest = cursor.readString();
    operation.maxBytes = cursor.readU64();
    operation.maxSegments = cursor.readU64();
    capability.permittedOperations.push_back(std::move(operation));
  }

  capability.maxInflightBytes = cursor.readU64();
  capability.noProgressMs = cursor.readU64();
  capability.hardDeadlineMs = cursor.readU64();
  capability.epochKeyId = cursor.readString();
  const auto commitmentCount = cursor.readU64();
  if (commitmentCount == 0 || commitmentCount > (1U << 20)) {
    throw std::invalid_argument(
      "GroupCapabilityV1 key commitment count exceeds bound");
  }
  for (std::uint64_t index = 0; index < commitmentCount; ++index) {
    auto provider = cursor.readString();
    auto digest = cursor.readString();
    if (!capability.wrappedEpochKeyDigestByProvider.emplace(
          std::move(provider), std::move(digest)).second) {
      throw std::invalid_argument("duplicate GroupCapabilityV1 key commitment");
    }
  }
  capability.capabilityDigest = cursor.readString();
  const auto wrappedCount = cursor.readU64();
  if (wrappedCount == 0 || wrappedCount > (1U << 20)) {
    throw std::invalid_argument(
      "GroupCapabilityV1 wrapped key projection count exceeds bound");
  }
  for (std::uint64_t index = 0; index < wrappedCount; ++index) {
    auto provider = cursor.readString();
    auto wrapped = cursor.readBytes(1U << 20);
    if (!capability.wrappedEpochKeyByProvider.emplace(
          std::move(provider), std::move(wrapped)).second) {
      throw std::invalid_argument("duplicate GroupCapabilityV1 wrapped key");
    }
  }
  capability.sealerSignature = cursor.readBytes(1U << 20);
  if (!cursor.atEnd()) {
    throw std::invalid_argument("trailing GroupCapabilityV1 bytes");
  }
  capability.validate();
  if (capability.capabilityDigest != hex(capability.canonicalBytes(false))) {
    throw std::invalid_argument("capability digest mismatch");
  }
  return capability;
}

const GroupOperationV1&
ProviderGroupCoordinator::findOperation(std::uint64_t operationIndex) const
{
  const auto it = std::find_if(m_capability.permittedOperations.begin(),
                               m_capability.permittedOperations.end(),
                               [operationIndex] (const auto& operation) {
                                 return operation.operationIndex == operationIndex;
                               });
  if (it == m_capability.permittedOperations.end()) {
    throw std::invalid_argument("operation is not permitted by capability");
  }
  return *it;
}

CollectiveOperationManifestV1
ProviderGroupCoordinator::makeManifest(
  const GroupOperationV1& operation,
  const std::string& producerRank,
  const std::string& sourceLayoutDigest,
  const std::string& targetLayoutDigest,
  const std::string& tensorDigest,
  const std::vector<ProviderGroupBytes>& plaintextSegments,
  std::uint64_t createdAtMs) const
{
  if (!m_hasCapability || m_cancelled || m_failed) {
    throw std::logic_error("cannot make a manifest for a terminal group");
  }
  const auto& permitted = findOperation(operation.operationIndex);
  if (operation.kind != permitted.kind ||
      std::find(permitted.producerRanks.begin(), permitted.producerRanks.end(),
                producerRank) == permitted.producerRanks.end()) {
    throw std::invalid_argument(
      "manifest producer is not permitted by GroupCapabilityV1");
  }
  if (plaintextSegments.empty() ||
      plaintextSegments.size() > permitted.maxSegments ||
      plaintextSegments.size() > m_options.maxSegments) {
    throw std::invalid_argument("manifest segment count exceeds operation bound");
  }
  CollectiveOperationManifestV1 manifest;
  manifest.capabilityDigest = m_capability.capabilityDigest;
  manifest.epochKeyId = m_capability.epochKeyId;
  manifest.requestId = m_capability.requestId;
  manifest.attemptId = m_capability.attemptId;
  manifest.planDigest = m_capability.planDigest;
  manifest.groupId = m_capability.groupId;
  manifest.epoch = m_capability.epoch;
  manifest.operationIndex = operation.operationIndex;
  manifest.operationKind = operation.kind;
  manifest.producerRank = producerRank;
  manifest.sourceLayoutDigest = sourceLayoutDigest;
  manifest.targetLayoutDigest = targetLayoutDigest;
  manifest.tensorDigest = tensorDigest;
  manifest.segmentCount = plaintextSegments.size();
  manifest.segmentSize = 0;
  manifest.createdAtMs = createdAtMs;
  manifest.noProgressMs = m_capability.noProgressMs;
  manifest.hardDeadlineMs = m_capability.hardDeadlineMs;
  for (const auto& plaintext : plaintextSegments) {
    if (plaintext.empty()) {
      throw std::invalid_argument("empty NDNSF_DATA_V1 plaintext segment");
    }
    const auto bytes = static_cast<std::uint64_t>(plaintext.size());
    if (manifest.totalBytes > permitted.maxBytes ||
        bytes > permitted.maxBytes - manifest.totalBytes ||
        manifest.totalBytes > m_capability.maxInflightBytes ||
        bytes > m_capability.maxInflightBytes - manifest.totalBytes ||
        bytes > std::numeric_limits<std::uint64_t>::max() - manifest.totalBytes) {
      throw std::invalid_argument("manifest byte bound exceeded");
    }
    manifest.totalBytes += bytes;
    manifest.segmentSize = std::max<std::uint64_t>(manifest.segmentSize, plaintext.size());
    manifest.orderedSegmentDigests.push_back(hex(plaintext));
  }
  if (manifest.totalBytes == 0 || manifest.totalBytes > permitted.maxBytes ||
      manifest.totalBytes > m_capability.maxInflightBytes) {
    throw std::invalid_argument("manifest byte bound exceeded");
  }
  const auto manifestBytes = manifest.canonicalBytes(false);
  manifest.producerSignature = m_options.signManifest
    ? m_options.signManifest(manifestBytes)
    : hmac(m_epochKey, manifestBytes);
  if (manifest.producerSignature.empty()) {
    throw std::runtime_error("manifest signer returned an empty signature");
  }
  manifest.validate();
  return manifest;
}

SealedCollectiveOperationV1
ProviderGroupCoordinator::sealOperation(
  const GroupOperationV1& operation,
  const std::string& producerRank,
  const std::string& sourceLayoutDigest,
  const std::string& targetLayoutDigest,
  const std::string& tensorDigest,
  const std::vector<ProviderGroupBytes>& plaintextSegments,
  std::uint64_t createdAtMs,
  const std::vector<std::string>& exactDataNames)
{
  if (!exactDataNames.empty() &&
      exactDataNames.size() != plaintextSegments.size()) {
    throw std::invalid_argument(
      "exact NDNSF_DATA_V1 name count does not match segment count");
  }
  findOperation(operation.operationIndex);
  auto manifest = makeManifest(operation, producerRank, sourceLayoutDigest,
                               targetLayoutDigest, tensorDigest,
                               plaintextSegments, createdAtMs);
  const auto operationKey = deriveOperationKey(m_epochKey, m_capability, manifest);
  SealedCollectiveOperationV1 result;
  result.manifest = manifest;
  result.segments.reserve(plaintextSegments.size());
  for (std::size_t index = 0; index < plaintextSegments.size(); ++index) {
    CollectiveSegmentDescriptor descriptor;
    descriptor.requestId = manifest.requestId;
    descriptor.attemptId = manifest.attemptId;
    descriptor.planDigest = manifest.planDigest;
    descriptor.groupId = manifest.groupId;
    descriptor.epoch = manifest.epoch;
    descriptor.operationIndex = manifest.operationIndex;
    descriptor.operationKind = manifest.operationKind;
    descriptor.producerRank = manifest.producerRank;
    descriptor.tensorDigest = manifest.tensorDigest;
    descriptor.manifestDigest = manifest.digest();
    descriptor.segmentNo = index;
    descriptor.segmentCount = manifest.segmentCount;
    descriptor.totalBytes = manifest.totalBytes;
    descriptor.segmentSize = manifest.segmentSize;
    descriptor.noProgressMs = manifest.noProgressMs;
    descriptor.hardDeadlineMs = manifest.hardDeadlineMs;
    const auto name = exactDataNames.empty()
      ? makeDataName(m_capability, manifest, index)
      : exactDataNames[index];
    if (name.empty()) {
      throw std::invalid_argument("exact NDNSF_DATA_V1 name is empty");
    }
    const auto nonce = deriveNonce(m_capability, manifest, name, index);
    result.segments.push_back(NdnsfCollectiveControl::sealWithNonce(
      descriptor, name, operationKey, nonce, plaintextSegments[index]));
  }
  return result;
}

ProviderGroupBytes
ProviderGroupCoordinator::signTensorObjectManifest(
  const ProviderGroupBytes& signingBytes) const
{
  if (!m_hasCapability || terminal() || signingBytes.empty()) {
    throw std::logic_error(
      "cannot sign a tensor manifest for an inactive group");
  }
  return m_options.signManifest
    ? m_options.signManifest(signingBytes)
    : hmac(m_epochKey, signingBytes);
}

bool
ProviderGroupCoordinator::verifyTensorObjectManifest(
  const ProviderGroupBytes& signingBytes,
  const ProviderGroupBytes& signature) const
{
  if (!m_hasCapability || terminal() || signingBytes.empty() ||
      signature.empty()) {
    return false;
  }
  if (m_options.verifyManifest) {
    return m_options.verifyManifest(signingBytes, signature);
  }
  const auto expected = hmac(m_epochKey, signingBytes);
  return expected.size() == signature.size() &&
         CRYPTO_memcmp(expected.data(), signature.data(), expected.size()) == 0;
}

ProviderGroupBytes
ProviderGroupCoordinator::encodeOperation(
  const SealedCollectiveOperationV1& operation)
{
  operation.manifest.validate();
  if (operation.segments.size() != operation.manifest.segmentCount ||
      operation.segments.size() > (1U << 20)) {
    throw std::invalid_argument("invalid NDNSF_DATA_V1 operation segment count");
  }
  Bytes wire;
  appendString(wire, "NDNSF_DATA_V1_BUNDLE");
  appendString(wire, operation.manifest.capabilityDigest);
  appendString(wire, operation.manifest.epochKeyId);
  appendString(wire, operation.manifest.requestId);
  appendString(wire, operation.manifest.attemptId);
  appendString(wire, operation.manifest.planDigest);
  appendString(wire, operation.manifest.groupId);
  appendU64(wire, operation.manifest.epoch);
  appendU64(wire, operation.manifest.operationIndex);
  appendString(wire, operation.manifest.operationKind);
  appendString(wire, operation.manifest.producerRank);
  appendString(wire, operation.manifest.sourceLayoutDigest);
  appendString(wire, operation.manifest.targetLayoutDigest);
  appendString(wire, operation.manifest.tensorDigest);
  appendU64(wire, operation.manifest.totalBytes);
  appendU64(wire, operation.manifest.segmentSize);
  appendU64(wire, operation.manifest.segmentCount);
  appendStrings(wire, operation.manifest.orderedSegmentDigests);
  appendU64(wire, operation.manifest.createdAtMs);
  appendU64(wire, operation.manifest.noProgressMs);
  appendU64(wire, operation.manifest.hardDeadlineMs);
  appendBytes(wire, operation.manifest.producerSignature);
  appendU64(wire, operation.segments.size());
  for (const auto& segment : operation.segments) {
    const auto encoded = segment.wireEncode();
    appendBytes(wire, encoded);
  }
  return wire;
}

SealedCollectiveOperationV1
ProviderGroupCoordinator::decodeOperation(const ProviderGroupBytes& wire)
{
  if (wire.size() > (64U << 20)) {
    throw std::invalid_argument("NDNSF_DATA_V1 bundle exceeds bound");
  }
  WireCursor cursor(wire);
  if (cursor.readString() != "NDNSF_DATA_V1_BUNDLE") {
    throw std::invalid_argument("invalid NDNSF_DATA_V1 bundle marker");
  }
  SealedCollectiveOperationV1 result;
  auto& manifest = result.manifest;
  manifest.capabilityDigest = cursor.readString();
  manifest.epochKeyId = cursor.readString();
  manifest.requestId = cursor.readString();
  manifest.attemptId = cursor.readString();
  manifest.planDigest = cursor.readString();
  manifest.groupId = cursor.readString();
  manifest.epoch = cursor.readU64();
  manifest.operationIndex = cursor.readU64();
  manifest.operationKind = cursor.readString();
  manifest.producerRank = cursor.readString();
  manifest.sourceLayoutDigest = cursor.readString();
  manifest.targetLayoutDigest = cursor.readString();
  manifest.tensorDigest = cursor.readString();
  manifest.totalBytes = cursor.readU64();
  manifest.segmentSize = cursor.readU64();
  manifest.segmentCount = cursor.readU64();
  const auto digestCount = cursor.readU64();
  if (digestCount > (1U << 20)) {
    throw std::invalid_argument("NDNSF_DATA_V1 manifest digest count exceeds bound");
  }
  manifest.orderedSegmentDigests.reserve(static_cast<std::size_t>(digestCount));
  for (std::uint64_t index = 0; index < digestCount; ++index) {
    manifest.orderedSegmentDigests.push_back(cursor.readString());
  }
  manifest.createdAtMs = cursor.readU64();
  manifest.noProgressMs = cursor.readU64();
  manifest.hardDeadlineMs = cursor.readU64();
  manifest.producerSignature = cursor.readBytes(1U << 20);
  const auto segmentCount = cursor.readU64();
  if (segmentCount > (1U << 20) || segmentCount != manifest.segmentCount) {
    throw std::invalid_argument("NDNSF_DATA_V1 bundle segment count mismatch");
  }
  result.segments.reserve(static_cast<std::size_t>(segmentCount));
  for (std::uint64_t index = 0; index < segmentCount; ++index) {
    result.segments.push_back(NdnsfDataV1Segment::wireDecode(
      cursor.readBytes(20U << 20)));
  }
  if (!cursor.atEnd()) {
    throw std::invalid_argument("trailing NDNSF_DATA_V1 bundle bytes");
  }
  manifest.validate();
  return result;
}

ProviderGroupBytes
ProviderGroupCoordinator::encodeSegment(
  const CollectiveOperationManifestV1& manifest,
  const NdnsfDataV1Segment& segment)
{
  manifest.validate();
  segment.validate();
  if (segment.descriptor.manifestDigest != manifest.digest() ||
      segment.descriptor.segmentCount != manifest.segmentCount ||
      segment.descriptor.totalBytes != manifest.totalBytes ||
      segment.descriptor.segmentNo >= manifest.segmentCount) {
    throw std::invalid_argument("NDNSF_DATA_V1 segment does not match manifest");
  }
  Bytes wire;
  appendString(wire, "NDNSF_DATA_V1_SEGMENT_BUNDLE");
  appendString(wire, manifest.capabilityDigest);
  appendString(wire, manifest.epochKeyId);
  appendString(wire, manifest.requestId);
  appendString(wire, manifest.attemptId);
  appendString(wire, manifest.planDigest);
  appendString(wire, manifest.groupId);
  appendU64(wire, manifest.epoch);
  appendU64(wire, manifest.operationIndex);
  appendString(wire, manifest.operationKind);
  appendString(wire, manifest.producerRank);
  appendString(wire, manifest.sourceLayoutDigest);
  appendString(wire, manifest.targetLayoutDigest);
  appendString(wire, manifest.tensorDigest);
  appendU64(wire, manifest.totalBytes);
  appendU64(wire, manifest.segmentSize);
  appendU64(wire, manifest.segmentCount);
  appendStrings(wire, manifest.orderedSegmentDigests);
  appendU64(wire, manifest.createdAtMs);
  appendU64(wire, manifest.noProgressMs);
  appendU64(wire, manifest.hardDeadlineMs);
  appendBytes(wire, manifest.producerSignature);
  appendBytes(wire, segment.wireEncode());
  if (wire.size() > (64U << 20)) {
    throw std::invalid_argument("NDNSF_DATA_V1 segment bundle exceeds bound");
  }
  return wire;
}

SealedCollectiveOperationV1
ProviderGroupCoordinator::decodeSegment(const ProviderGroupBytes& wire)
{
  if (wire.size() > (64U << 20)) {
    throw std::invalid_argument("NDNSF_DATA_V1 segment bundle exceeds bound");
  }
  WireCursor cursor(wire);
  if (cursor.readString() != "NDNSF_DATA_V1_SEGMENT_BUNDLE") {
    throw std::invalid_argument("invalid NDNSF_DATA_V1 segment bundle marker");
  }
  SealedCollectiveOperationV1 result;
  auto& manifest = result.manifest;
  manifest.capabilityDigest = cursor.readString();
  manifest.epochKeyId = cursor.readString();
  manifest.requestId = cursor.readString();
  manifest.attemptId = cursor.readString();
  manifest.planDigest = cursor.readString();
  manifest.groupId = cursor.readString();
  manifest.epoch = cursor.readU64();
  manifest.operationIndex = cursor.readU64();
  manifest.operationKind = cursor.readString();
  manifest.producerRank = cursor.readString();
  manifest.sourceLayoutDigest = cursor.readString();
  manifest.targetLayoutDigest = cursor.readString();
  manifest.tensorDigest = cursor.readString();
  manifest.totalBytes = cursor.readU64();
  manifest.segmentSize = cursor.readU64();
  manifest.segmentCount = cursor.readU64();
  const auto digestCount = cursor.readU64();
  if (digestCount > (1U << 20)) {
    throw std::invalid_argument("NDNSF_DATA_V1 manifest digest count exceeds bound");
  }
  manifest.orderedSegmentDigests.reserve(static_cast<std::size_t>(digestCount));
  for (std::uint64_t index = 0; index < digestCount; ++index) {
    manifest.orderedSegmentDigests.push_back(cursor.readString());
  }
  manifest.createdAtMs = cursor.readU64();
  manifest.noProgressMs = cursor.readU64();
  manifest.hardDeadlineMs = cursor.readU64();
  manifest.producerSignature = cursor.readBytes(1U << 20);
  result.segments.push_back(NdnsfDataV1Segment::wireDecode(
    cursor.readBytes(20U << 20)));
  if (!cursor.atEnd()) {
    throw std::invalid_argument("trailing NDNSF_DATA_V1 segment bundle bytes");
  }
  manifest.validate();
  return result;
}

void
ProviderGroupCoordinator::validateManifestAgainstCapability(
  const CollectiveOperationManifestV1& manifest) const
{
  if (!m_hasCapability || m_cancelled || m_failed) {
    throw std::runtime_error("NDNSF_DATA_V1 group is terminal");
  }
  manifest.validate();
  if (manifest.capabilityDigest != m_capability.capabilityDigest ||
      manifest.epochKeyId != m_capability.epochKeyId ||
      manifest.requestId != m_capability.requestId ||
      manifest.attemptId != m_capability.attemptId ||
      manifest.planDigest != m_capability.planDigest ||
      manifest.groupId != m_capability.groupId ||
      manifest.epoch != m_capability.epoch) {
    throw std::runtime_error("manifest capability binding mismatch");
  }
  const auto& operation = findOperation(manifest.operationIndex);
  if (operation.kind != manifest.operationKind ||
      std::find(operation.producerRanks.begin(), operation.producerRanks.end(),
                manifest.producerRank) == operation.producerRanks.end() ||
      manifest.segmentCount > operation.maxSegments ||
      manifest.totalBytes > operation.maxBytes ||
      manifest.totalBytes > m_capability.maxInflightBytes) {
    throw std::runtime_error("manifest operation bounds mismatch");
  }
  const auto manifestBytes = manifest.canonicalBytes(false);
  const auto verified = m_options.verifyManifest
    ? m_options.verifyManifest(manifestBytes, manifest.producerSignature)
    : [&] {
        const auto expected = hmac(m_epochKey, manifestBytes);
        return expected.size() == manifest.producerSignature.size() &&
               CRYPTO_memcmp(expected.data(), manifest.producerSignature.data(),
                             expected.size()) == 0;
      }();
  if (!verified) {
    throw std::runtime_error("manifest authenticator verification failed");
  }
}

ProviderGroupBytes
ProviderGroupCoordinator::openSegment(
  const CollectiveOperationManifestV1& manifest,
  const NdnsfDataV1Segment& segment,
  const std::string& expectedDataName)
{
  validateManifestAgainstCapability(manifest);
  const auto expectedName = expectedDataName.empty()
    ? makeDataName(m_capability, manifest, segment.descriptor.segmentNo)
    : expectedDataName;
  if (segment.descriptor.manifestDigest != manifest.digest() ||
      segment.descriptor.segmentCount != manifest.segmentCount ||
      segment.descriptor.totalBytes != manifest.totalBytes ||
      segment.descriptor.segmentSize != manifest.segmentSize ||
      segment.descriptor.operationIndex != manifest.operationIndex ||
      segment.descriptor.producerRank != manifest.producerRank ||
      segment.dataName != expectedName) {
    throw std::runtime_error("NDNSF_DATA_V1 segment manifest binding mismatch");
  }
  if (segment.descriptor.segmentNo >= manifest.segmentCount) {
    throw std::runtime_error("NDNSF_DATA_V1 segment index out of range");
  }
  const auto expectedNonce = deriveNonce(
    m_capability, manifest, expectedName, segment.descriptor.segmentNo);
  if (segment.nonce != expectedNonce) {
    throw std::runtime_error("NDNSF_DATA_V1 nonce derivation mismatch");
  }
  const auto operationKey = deriveOperationKey(m_epochKey, m_capability, manifest);
  auto plaintext = NdnsfCollectiveControl::open(segment, operationKey, expectedName);
  if (hex(plaintext) != manifest.orderedSegmentDigests[segment.descriptor.segmentNo]) {
    throw std::runtime_error("NDNSF_DATA_V1 plaintext digest mismatch");
  }
  return plaintext;
}

DataSegmentReplayWindow&
ProviderGroupCoordinator::replayWindow(
  const CollectiveOperationManifestV1& manifest)
{
  const auto key = manifest.digest();
  auto found = m_replayWindows.find(key);
  if (found != m_replayWindows.end()) {
    return *found->second;
  }
  const auto& operation = findOperation(manifest.operationIndex);
  auto window = std::make_unique<DataSegmentReplayWindow>(
    operation.maxSegments,
    static_cast<std::size_t>(std::min<std::uint64_t>(
      operation.maxBytes, std::numeric_limits<std::size_t>::max())));
  auto* result = window.get();
  m_replayWindows.emplace(key, std::move(window));
  return *result;
}

DataSegmentReplayWindow::Result
ProviderGroupCoordinator::acceptSegment(
  const CollectiveOperationManifestV1& manifest,
  const NdnsfDataV1Segment& segment,
  const std::string& expectedDataName)
{
  if (m_groupStartedAtMs == 0) {
    m_groupStartedAtMs = manifest.createdAtMs;
  }
  const auto plaintext = openSegment(manifest, segment, expectedDataName);
  (void)plaintext;
  const auto operationKey = deriveOperationKey(m_epochKey, m_capability, manifest);
  const auto result = replayWindow(manifest).accept(
    segment, operationKey, segment.dataName);
  if (result == DataSegmentReplayWindow::Result::Accepted) {
    m_lastProgressMs = std::max(m_lastProgressMs, manifest.createdAtMs);
  }
  return result;
}

bool
ProviderGroupCoordinator::recordProgress(std::uint64_t nowMs)
{
  if (!m_hasCapability || terminal()) {
    return false;
  }
  if (deadlineExpired(nowMs)) {
    fail("NDNSF_DATA_V1_HARD_DEADLINE");
    return false;
  }
  if (m_lastProgressMs != 0 && nowMs >= m_lastProgressMs &&
      nowMs - m_lastProgressMs > m_capability.noProgressMs) {
    // The progress deadline is intentionally not an extension of the hard
    // deadline; it terminates the complete epoch.
    fail("NDNSF_DATA_V1_NO_PROGRESS");
    return false;
  }
  m_lastProgressMs = nowMs;
  return true;
}

bool
ProviderGroupCoordinator::deadlineExpired(std::uint64_t nowMs) const
{
  return m_groupStartedAtMs != 0 && nowMs >= m_groupStartedAtMs &&
         nowMs - m_groupStartedAtMs >= m_capability.hardDeadlineMs;
}

void
ProviderGroupCoordinator::cancel(std::string reason)
{
  if (terminal()) {
    return;
  }
  m_cancelled = true;
  m_terminalReason = std::move(reason);
  clearEpochKey();
}

void
ProviderGroupCoordinator::fail(std::string reason)
{
  if (terminal()) {
    return;
  }
  m_failed = true;
  m_terminalReason = std::move(reason);
  clearEpochKey();
}

bool
ProviderGroupCoordinator::terminal() const noexcept
{
  return m_cancelled || m_failed;
}

bool
ProviderGroupCoordinator::cancelled() const noexcept
{
  return m_cancelled;
}

bool
ProviderGroupCoordinator::failed() const noexcept
{
  return m_failed;
}

const std::string&
ProviderGroupCoordinator::terminalReason() const noexcept
{
  return m_terminalReason;
}

void
ProviderGroupCoordinator::clearEpochKey() noexcept
{
  cleanse(m_epochKey);
  m_replayWindows.clear();
}

std::string
ProviderGroupCoordinator::makeDataName(const GroupCapabilityV1& capability,
                                       const CollectiveOperationManifestV1& manifest,
                                       std::uint64_t segmentNo)
{
  std::string producer;
  for (const auto& member : capability.orderedMembers) {
    if (std::to_string(member.rank) == manifest.producerRank ||
        member.provider == manifest.producerRank) {
      // Data names and SVSPubSub producer subscriptions are routed by the
      // advertised endpoint prefix.  The Provider identity remains the
      // authorization/key-wrap subject and is intentionally not assumed to be
      // the transport prefix.
      producer = member.endpointPrefix;
      break;
    }
  }
  if (producer.empty()) {
    producer = "/NDNSF-DI/producer/" + manifest.producerRank;
  }
  ndn::Name name(producer.empty() ? "/NDNSF-DI/producer" : producer);
  name.append("NDNSF-DI").append("COLLECTIVE").append("v1").append("REQ");
  name.append(ndn::Name(manifest.requestId));
  name.append("ATTEMPT").append(ndn::Name(manifest.attemptId));
  name.append("PLAN").append(ndn::Name(manifest.planDigest));
  name.append("GROUP").append(ndn::Name(manifest.groupId));
  name.append("EPOCH").append(std::to_string(manifest.epoch));
  name.append("OP").append(std::to_string(manifest.operationIndex));
  name.append("RANK").append(ndn::Name(manifest.producerRank));
  name.append("TENSOR").append(ndn::Name(manifest.tensorDigest));
  name.append("SEG").append(std::to_string(segmentNo));
  return name.toUri();
}

ProviderGroupBytes
ProviderGroupCoordinator::deriveOperationKey(
  const ProviderGroupBytes& epochKey,
  const GroupCapabilityV1& capability,
  const CollectiveOperationManifestV1& manifest)
{
  if (epochKey.size() != KEY_BYTES) {
    throw std::invalid_argument("NDNSF_DATA_V1 epoch key must be 256 bits");
  }
  Bytes info;
  appendString(info, "NDNSF_DATA_V1/OPERATION");
  appendString(info, capability.capabilityDigest);
  appendU64(info, capability.epoch);
  appendU64(info, manifest.operationIndex);
  appendString(info, manifest.operationKind);
  appendString(info, manifest.producerRank);
  appendString(info, manifest.sourceLayoutDigest);
  appendString(info, manifest.targetLayoutDigest);
  appendString(info, manifest.tensorDigest);
  appendString(info, manifest.digest());
  return hkdf32(epochKey, info);
}

ProviderGroupBytes
ProviderGroupCoordinator::deriveNonce(
  const GroupCapabilityV1& capability,
  const CollectiveOperationManifestV1& manifest,
  const std::string& exactDataName,
  std::uint64_t segmentNo)
{
  requireNonEmpty(exactDataName, "exactDataName");
  Bytes input;
  appendString(input, "NDNSF_DATA_V1/NONCE");
  appendString(input, capability.capabilityDigest);
  appendU64(input, capability.epoch);
  appendU64(input, manifest.operationIndex);
  appendString(input, manifest.producerRank);
  appendString(input, manifest.tensorDigest);
  appendString(input, manifest.digest());
  appendString(input, exactDataName);
  appendU64(input, segmentNo);
  const auto digest = sha256(input);
  return Bytes(digest.begin(), digest.begin() + NONCE_BYTES);
}

} // namespace ndnsf::di
