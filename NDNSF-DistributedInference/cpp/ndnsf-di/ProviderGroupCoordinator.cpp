#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderGroupCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"

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

std::string
explicitMemberDataName(const GroupCapabilityV1& capability,
                       const std::string& producerRank, const std::string& value)
{
  const auto member = std::find_if(capability.orderedMembers.begin(),
    capability.orderedMembers.end(), [&](const auto& item) {
      return std::to_string(item.rank) == producerRank;
    });
  if (member == capability.orderedMembers.end() || value.empty())
    throw std::invalid_argument("explicit member tensor Data name is required");
  const ndn::Name producer(member->provider);
  const ndn::Name name(value);
  if (producer.empty() || name.size() <= producer.size() ||
      !producer.isPrefixOf(name) || name.wireEncode().size() > 1024)
    throw std::invalid_argument("tensor Data name is outside its complete Producer");
  return name.toUri(ndn::name::UriFormat::CANONICAL);
}

const char* const COMPACT_SEGMENT_BUNDLE_MARKER =
  "NDNSF_DATA_V1_SEGMENT_BUNDLE_COMPACT";

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

  Bytes readFixedBytes(std::size_t size)
  {
    require(size);
    Bytes result(m_input.begin() + static_cast<std::ptrdiff_t>(m_offset),
                 m_input.begin() + static_cast<std::ptrdiff_t>(m_offset + size));
    m_offset += size;
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

bool
isHexDigest(const std::string& value)
{
  return value.size() == DIGEST_BYTES * 2 &&
         std::all_of(value.begin(), value.end(), [] (unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

Bytes
decodeHexDigest(const std::string& value)
{
  if (!isHexDigest(value)) {
    throw std::invalid_argument(
      "NDNSF_DATA_V1 compact digest is not canonical hexadecimal");
  }
  const auto nibble = [] (char ch) -> std::uint8_t {
    return static_cast<std::uint8_t>(ch <= '9' ? ch - '0' : ch - 'a' + 10);
  };
  Bytes result(DIGEST_BYTES);
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<std::uint8_t>(
      (nibble(value[index * 2]) << 4) | nibble(value[index * 2 + 1]));
  }
  return result;
}

std::string
encodeHexDigest(const Bytes& value)
{
  if (value.size() != DIGEST_BYTES) {
    throw std::invalid_argument("NDNSF_DATA_V1 compact digest size is invalid");
  }
  static constexpr char HEX[] = "0123456789abcdef";
  std::string result;
  result.reserve(DIGEST_BYTES * 2);
  for (const auto byte : value) {
    result.push_back(HEX[byte >> 4]);
    result.push_back(HEX[byte & 0x0f]);
  }
  return result;
}

std::string
manifestDigestForTransport(const CollectiveOperationManifestV1& manifest)
{
  return manifest.externalSegmentDigests && !manifest.transportManifestDigest.empty()
    ? manifest.transportManifestDigest
    : manifest.digest();
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

bool
sameOperationParameters(const GroupOperationV1& a, const GroupOperationV1& b)
{
  return a.kind == b.kind && a.producerRanks == b.producerRanks &&
    a.consumerRanks == b.consumerRanks && a.tensorLayoutDigest == b.tensorLayoutDigest &&
    a.maxBytes == b.maxBytes && a.maxSegments == b.maxSegments;
}

void
appendCapabilityOperation(Bytes& output, const GroupOperationV1& operation)
{
  appendU64(output, operation.operationIndex);
  appendString(output, operation.kind);
  appendStrings(output, operation.producerRanks);
  appendStrings(output, operation.consumerRanks);
  appendString(output, operation.tensorLayoutDigest);
  appendU64(output, operation.maxBytes);
  appendU64(output, operation.maxSegments);
}

std::uint64_t
operationEncodedSize(const GroupOperationV1& operation)
{
  constexpr std::uint64_t LIMIT = 16U << 20;
  std::uint64_t size = 7 * sizeof(std::uint64_t);
  const auto add = [&](std::uint64_t bytes) {
    if (bytes > LIMIT - size)
      throw std::invalid_argument("GroupCapability operation exceeds wire bound");
    size += bytes;
  };
  add(operation.kind.size());
  add(operation.tensorLayoutDigest.size());
  for (const auto& rank : operation.producerRanks) {
    add(sizeof(std::uint64_t));
    add(rank.size());
  }
  for (const auto& rank : operation.consumerRanks) {
    add(sizeof(std::uint64_t));
    add(rank.size());
  }
  return size;
}

std::size_t
canonicalCapabilitySize(const GroupCapabilityV1& capability, bool includeDigest)
{
  constexpr std::uint64_t LIMIT = 16U << 20;
  std::uint64_t size = 0;
  const auto add = [&](std::uint64_t bytes) {
    if (bytes > LIMIT - size)
      throw std::invalid_argument("GroupCapability expanded fields exceed wire bound");
    size += bytes;
  };
  const auto string = [&](const std::string& value) {
    if (value.size() > MAX_STRING_BYTES)
      throw std::invalid_argument("GroupCapability string exceeds bound");
    add(sizeof(std::uint64_t));
    add(value.size());
  };
  string("GroupCapabilityV1");
  string(capability.requestId);
  string(capability.attemptId);
  string(capability.planDigest);
  string(capability.groupId);
  add(2 * sizeof(std::uint64_t)); // Epoch and member count.
  for (const auto& member : capability.orderedMembers) {
    string(member.provider);
    add(sizeof(std::uint64_t));
    string(member.offerDigest);
    string(member.endpointPrefix);
  }
  capability.operationCount();
  add(sizeof(std::uint64_t));
  const auto operation = [&](const GroupOperationV1& value, std::uint64_t count) {
    const auto bytes = operationEncodedSize(value);
    if (bytes > (LIMIT - size) / count)
      throw std::invalid_argument("GroupCapability expanded operations exceed wire bound");
    add(bytes * count);
  };
  for (const auto& value : capability.permittedOperations) operation(value, 1);
  for (const auto& run : capability.operationRuns) operation(run.first, run.count);
  add(3 * sizeof(std::uint64_t));
  string(capability.epochKeyId);
  add(sizeof(std::uint64_t));
  for (const auto& commitment : capability.wrappedEpochKeyDigestByProvider) {
    string(commitment.first);
    string(commitment.second);
  }
  if (includeDigest) string(capability.capabilityDigest);
  return size;
}

template<typename Visitor>
void
visitCapabilityOperations(const GroupCapabilityV1& capability, Visitor&& visitor)
{
  if (capability.operationRuns.empty()) {
    for (const auto& operation : capability.permittedOperations) visitor(operation);
  }
  else {
    for (const auto& run : capability.operationRuns) {
      auto operation = run.first;
      for (std::uint64_t offset = 0; offset < run.count; ++offset) {
        operation.operationIndex = run.first.operationIndex + offset * run.stride;
        visitor(operation);
      }
    }
  }
}

void
appendCapabilityOperations(Bytes& output, const std::vector<GroupOperationV1>& operations,
                           bool compact)
{
  appendU64(output, operations.size());
  std::vector<std::pair<std::size_t, std::size_t>> runs;
  if (compact) {
    for (std::size_t first = 0; first < operations.size();) {
      std::size_t end = first + 1;
      if (end < operations.size() &&
          operations[end].operationIndex > operations[first].operationIndex &&
          sameOperationParameters(operations[first], operations[end])) {
        const auto stride = operations[end].operationIndex - operations[first].operationIndex;
        ++end;
        while (end < operations.size() &&
               operations[end].operationIndex > operations[end - 1].operationIndex &&
               operations[end].operationIndex - operations[end - 1].operationIndex == stride &&
               sameOperationParameters(operations[first], operations[end])) {
          ++end;
        }
      }
      runs.emplace_back(first, end - first);
      first = end;
    }
    appendU64(output, runs.size());
  }
  if (compact) {
    for (const auto& [first, count] : runs) {
      appendU64(output, count);
      appendU64(output, count > 1
        ? operations[first + 1].operationIndex - operations[first].operationIndex : 0);
      appendCapabilityOperation(output, operations[first]);
    }
  }
  else {
    for (const auto& operation : operations) appendCapabilityOperation(output, operation);
  }
}

} // namespace

std::size_t
GroupCapabilityV1::operationCount() const
{
  constexpr std::size_t LIMIT = 1U << 20;
  if (!permittedOperations.empty() && !operationRuns.empty())
    throw std::invalid_argument("GroupCapability has two operation representations");
  if (operationRuns.empty()) {
    if (permittedOperations.size() > LIMIT)
      throw std::invalid_argument("too many GroupCapabilityV1 operations");
    return permittedOperations.size();
  }
  std::size_t count = 0;
  for (const auto& run : operationRuns) {
    if (run.count == 0 || run.count > LIMIT - count ||
        (run.count == 1 ? run.stride != 0 : run.stride == 0) ||
        (run.count > 1 && run.stride >
          (std::numeric_limits<std::uint64_t>::max() - run.first.operationIndex) /
            (run.count - 1)))
      throw std::invalid_argument("invalid GroupCapability operation run");
    count += run.count;
  }
  return count;
}

GroupOperationV1
GroupCapabilityV1::operation(std::uint64_t index) const
{
  operationCount();
  for (const auto& operation : permittedOperations) {
    if (operation.operationIndex == index) return operation;
  }
  for (const auto& run : operationRuns) {
    if (index < run.first.operationIndex) continue;
    const auto offset = index - run.first.operationIndex;
    if (offset == 0 || (run.stride && offset % run.stride == 0 &&
                       offset / run.stride < run.count)) {
      auto result = run.first;
      result.operationIndex = index;
      return result;
    }
  }
  throw std::invalid_argument("operation is not permitted by capability");
}

void
GroupCapabilityV1::validate() const
{
  requireNonEmpty(requestId, "requestId");
  requireNonEmpty(attemptId, "attemptId");
  requireNonEmpty(planDigest, "planDigest");
  requireNonEmpty(groupId, "groupId");
  requireNonEmpty(epochKeyId, "epochKeyId");
  if (epoch == 0 || orderedMembers.empty() || operationCount() == 0 ||
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
  visitCapabilityOperations(*this, [&](const auto& operation) {
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
  });
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
  return encodeFields(includeDigest, false);
}

ProviderGroupBytes
GroupCapabilityV1::encodeFields(bool includeDigest, bool compactOperations) const
{
  const auto canonicalSize = canonicalCapabilitySize(*this, includeDigest);
  Bytes output;
  if (!compactOperations) output.reserve(canonicalSize);
  appendString(output, compactOperations ? "GroupCapabilityV2" : "GroupCapabilityV1");
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
  const auto count = operationCount();
  if (operationRuns.empty()) {
    appendCapabilityOperations(output, permittedOperations, compactOperations);
  }
  else {
    appendU64(output, count);
    if (compactOperations) {
      appendU64(output, operationRuns.size());
      for (const auto& run : operationRuns) {
        appendU64(output, run.count);
        appendU64(output, run.stride);
        appendCapabilityOperation(output, run.first);
      }
    }
    else {
      visitCapabilityOperations(*this, [&](const auto& operation) {
        appendCapabilityOperation(output, operation);
      });
    }
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
      ((!externalSegmentDigests && segmentCount != orderedSegmentDigests.size()) ||
       (externalSegmentDigests && !orderedSegmentDigests.empty() &&
        segmentCount != orderedSegmentDigests.size())) ||
      noProgressMs == 0 || hardDeadlineMs < noProgressMs ||
      (!externalSegmentDigests && producerSignature.empty()) ||
      (externalSegmentDigests && transportManifestDigest.empty())) {
    throw std::invalid_argument("invalid CollectiveOperationManifestV1 bounds");
  }
  if (segmentCount > (1U << 20)) {
    throw std::invalid_argument("manifest segment count exceeds bound");
  }
  if (!externalSegmentDigests) {
    for (const auto& digest : orderedSegmentDigests) {
      requireNonEmpty(digest, "manifest.segmentDigest");
    }
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

ProviderGroupCoordinator::RuntimeKeySnapshot::RuntimeKeySnapshot(
  const ProviderGroupBytes& bytes, std::uint64_t value)
  : key(bytes), generation(value)
{
}

ProviderGroupCoordinator::RuntimeKeySnapshot::~RuntimeKeySnapshot()
{
  cleanse(key);
}

ProviderGroupCoordinator::RuntimeKeySnapshot
ProviderGroupCoordinator::captureRuntimeKey() const
{
  std::lock_guard<std::mutex> lock(m_stateMutex);
  if (!m_hasCapability || terminal())
    throw std::runtime_error("NDNSF_DATA_V1 group is terminal");
  return RuntimeKeySnapshot(m_epochKey, m_keyGeneration);
}

bool
ProviderGroupCoordinator::runtimeKeyActiveLocked(const RuntimeKeySnapshot& snapshot) const
{
  return !terminal() && snapshot.generation == m_keyGeneration;
}

bool
ProviderGroupCoordinator::runtimeKeyActive(const RuntimeKeySnapshot& snapshot) const
{
  std::lock_guard<std::mutex> lock(m_stateMutex);
  return runtimeKeyActiveLocked(snapshot);
}

GroupCapabilityV1
ProviderGroupCoordinator::createCapability(
  std::string requestId, std::string attemptId, std::string planDigest,
  std::string groupId, std::uint64_t epoch, std::vector<GroupMemberV1> orderedMembers,
  std::vector<GroupOperationV1> permittedOperations, std::uint64_t maxInflightBytes,
  std::uint64_t noProgressMs, std::uint64_t hardDeadlineMs)
{
  return createCapabilityImpl(std::move(requestId), std::move(attemptId),
    std::move(planDigest), std::move(groupId), epoch, std::move(orderedMembers),
    std::move(permittedOperations), {}, maxInflightBytes, noProgressMs, hardDeadlineMs);
}

GroupCapabilityV1
ProviderGroupCoordinator::createCapabilityFromRuns(
  std::string requestId, std::string attemptId, std::string planDigest,
  std::string groupId, std::uint64_t epoch, std::vector<GroupMemberV1> orderedMembers,
  std::vector<GroupOperationRunV1> operationRuns, std::uint64_t maxInflightBytes,
  std::uint64_t noProgressMs, std::uint64_t hardDeadlineMs)
{
  return createCapabilityImpl(std::move(requestId), std::move(attemptId),
    std::move(planDigest), std::move(groupId), epoch, std::move(orderedMembers), {},
    std::move(operationRuns), maxInflightBytes, noProgressMs, hardDeadlineMs);
}

GroupCapabilityV1
ProviderGroupCoordinator::createCapabilityImpl(
  std::string requestId,
  std::string attemptId,
  std::string planDigest,
  std::string groupId,
  std::uint64_t epoch,
  std::vector<GroupMemberV1> orderedMembers,
  std::vector<GroupOperationV1> permittedOperations,
  std::vector<GroupOperationRunV1> operationRuns,
  std::uint64_t maxInflightBytes,
  std::uint64_t noProgressMs,
  std::uint64_t hardDeadlineMs)
{
  const auto statistics = m_options.workStatistics.lock();
  NativeWorkScope buildWork(statistics, "group-capability-build", NativeWorkKind::LOCAL);
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
  m_capability.operationRuns = std::move(operationRuns);
  m_capability.maxInflightBytes = maxInflightBytes;
  m_capability.noProgressMs = noProgressMs;
  m_capability.hardDeadlineMs = hardDeadlineMs;
  m_epochKey = [&] {
    NativeWorkScope callback(statistics, "group-key-random", NativeWorkKind::UNCLASSIFIED);
    return m_options.randomBytes(KEY_BYTES);
  }();
  if (m_epochKey.size() != KEY_BYTES) {
    throw std::runtime_error("epoch key generator returned the wrong size");
  }
  m_capability.epochKeyId = hex(m_epochKey);
  for (const auto& member : m_capability.orderedMembers) {
    auto wrapped = [&] {
      NativeWorkScope callback(statistics, "group-key-wrap", NativeWorkKind::UNCLASSIFIED);
      return m_options.wrapEpochKey(member.provider, m_epochKey);
    }();
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
    ? [&] {
        NativeWorkScope callback(statistics, "group-capability-sign", NativeWorkKind::UNCLASSIFIED);
        return m_options.signCapability(capabilityBytes);
      }()
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
  const auto statistics = m_options.workStatistics.lock();
  NativeWorkScope installWork(statistics, "group-capability-install", NativeWorkKind::LOCAL);
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
    epochKey = [&] {
      NativeWorkScope callback(statistics, "group-key-unwrap", NativeWorkKind::UNCLASSIFIED);
      return m_options.unwrapEpochKey(m_options.localProvider, it->second);
    }();
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
      ? [&] {
          NativeWorkScope callback(statistics, "group-capability-verify",
                                   NativeWorkKind::UNCLASSIFIED);
          return m_options.verifyCapability(capabilityBytes, capability.sealerSignature);
        }()
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
  const auto snapshot = captureRuntimeKey();
  if (!snapshot.key.empty()) {
    auto key = snapshot.key;
    if (!runtimeKeyActive(snapshot)) {
      cleanse(key);
      throw std::runtime_error("NDNSF_DATA_V1 group key changed during access");
    }
    return key;
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
  if (!runtimeKeyActive(snapshot)) {
    cleanse(key);
    throw std::runtime_error("NDNSF_DATA_V1 group key changed during unwrap");
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
  const auto expandedFieldsSize = wire.size();
  if (expandedFieldsSize > (16U << 20))
    throw std::invalid_argument("GroupCapabilityV1 expanded fields exceed bound");
  auto compact = capability.encodeFields(true, true);
  if (compact.size() < wire.size()) wire.swap(compact);
  const auto fieldsSize = wire.size();
  appendU64(wire, capability.wrappedEpochKeyByProvider.size());
  for (const auto& entry : capability.wrappedEpochKeyByProvider) {
    appendString(wire, entry.first);
    appendBytes(wire, entry.second);
  }
  appendBytes(wire, capability.sealerSignature);
  if (wire.size() > (16U << 20) || expandedFieldsSize + wire.size() - fieldsSize > (16U << 20)) {
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
  const auto marker = cursor.readString();
  const bool compact = marker == "GroupCapabilityV2";
  if (!compact && marker != "GroupCapabilityV1") {
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
  const auto runCount = compact ? cursor.readU64() : operationCount;
  if (runCount == 0 || runCount > operationCount)
    throw std::invalid_argument("GroupCapability operation run count exceeds bound");
  std::uint64_t operationBudget = 16U << 20;
  std::uint64_t decodedCount = 0;
  for (std::uint64_t index = 0; index < runCount; ++index) {
    const auto count = compact ? cursor.readU64() : 1;
    const auto stride = compact ? cursor.readU64() : 0;
    if (count == 0 || count > operationCount - decodedCount ||
        (count == 1 ? stride != 0 : stride == 0))
      throw std::invalid_argument("invalid GroupCapability operation run");
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
    if (count > 1 && stride >
        (std::numeric_limits<std::uint64_t>::max() - operation.operationIndex) / (count - 1))
      throw std::invalid_argument("GroupCapability operation run overflows");
    const auto bytesPerOperation = operationEncodedSize(operation);
    if (bytesPerOperation > operationBudget / count)
      throw std::invalid_argument("GroupCapability expanded operations exceed wire bound");
    operationBudget -= bytesPerOperation * count;
    decodedCount += count;
    if (compact) capability.operationRuns.push_back({std::move(operation), count, stride});
    else capability.permittedOperations.push_back(std::move(operation));
  }
  if (decodedCount != operationCount)
    throw std::invalid_argument("GroupCapability expanded operation count mismatch");

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
  const auto canonical = capability.canonicalBytes(false);
  if (canonical.size() > (16U << 20))
    throw std::invalid_argument("GroupCapability expanded fields exceed wire bound");
  if (capability.capabilityDigest != hex(canonical)) {
    throw std::invalid_argument("capability digest mismatch");
  }
  return capability;
}

GroupOperationV1
ProviderGroupCoordinator::findOperation(std::uint64_t operationIndex) const
{
  NativeWorkScope lookup(m_options.workStatistics.lock(), "group-operation-lookup",
                         NativeWorkKind::LOCAL);
  return m_capability.operation(operationIndex);
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
  manifest.producerSignature = signTensorObjectManifest(manifestBytes);
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
  const auto statistics = m_options.workStatistics.lock();
  NativeWorkScope operationWork(statistics, "tensor-operation-seal", NativeWorkKind::LOCAL);
  NativeWorkScope contextWork(statistics, "tensor-seal-context", NativeWorkKind::LOCAL);
  if (exactDataNames.empty() || exactDataNames.size() != plaintextSegments.size()) {
    throw std::invalid_argument(
      "exact NDNSF_DATA_V1 name count does not match segment count");
  }
  findOperation(operation.operationIndex);
  std::vector<std::string> names;
  names.reserve(exactDataNames.size());
  std::set<ndn::Name> uniqueNames;
  for (const auto& name : exactDataNames) {
    names.push_back(explicitMemberDataName(m_capability, producerRank, name));
    if (!uniqueNames.insert(ndn::Name(names.back())).second)
      throw std::invalid_argument("member tensor Data names repeat a segment");
  }
  const auto snapshot = captureRuntimeKey();
  contextWork.finish();
  NativeWorkScope manifestWork(statistics, "tensor-collective-manifest", NativeWorkKind::LOCAL);
  auto manifest = makeManifest(operation, producerRank, sourceLayoutDigest,
                               targetLayoutDigest, tensorDigest,
                               plaintextSegments, createdAtMs);
  manifestWork.finish();
  NativeWorkScope keyWork(statistics, "tensor-operation-key-derive", NativeWorkKind::LOCAL);
  const auto operationKey = deriveOperationKey(snapshot.key, m_capability, manifest);
  keyWork.finish();
  SealedCollectiveOperationV1 result;
  result.manifest = manifest;
  result.segments.reserve(plaintextSegments.size());
  for (std::size_t index = 0; index < plaintextSegments.size(); ++index) {
    NativeWorkScope descriptorWork(statistics, "tensor-segment-descriptor", NativeWorkKind::LOCAL);
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
    descriptorWork.finish();
    const auto& name = names[index];
    NativeWorkScope nonceWork(statistics, "tensor-nonce-derive", NativeWorkKind::LOCAL);
    const auto nonce = deriveNonce(m_capability, manifest, name, index);
    nonceWork.finish();
    NativeWorkScope sealWork(statistics, "tensor-segment-aead-seal", NativeWorkKind::LOCAL);
    result.segments.push_back(NdnsfCollectiveControl::sealWithNonce(
      descriptor, name, operationKey, nonce, plaintextSegments[index]));
  }
  if (!runtimeKeyActive(snapshot))
    throw std::runtime_error("NDNSF_DATA_V1 group key changed during sealing");
  return result;
}

ProviderGroupBytes
ProviderGroupCoordinator::signTensorObjectManifest(
  const ProviderGroupBytes& signingBytes) const
{
  NativeWorkScope signWork(m_options.workStatistics.lock(), "tensor-manifest-authenticate",
                           NativeWorkKind::LOCAL);
  if (!m_hasCapability || terminal() || signingBytes.empty()) {
    throw std::logic_error(
      "cannot sign a tensor manifest for an inactive group");
  }
  const auto snapshot = captureRuntimeKey();
  auto signature = m_options.signManifest
    ? m_options.signManifest(signingBytes)
    : hmac(snapshot.key, signingBytes);
  if (!runtimeKeyActive(snapshot))
    throw std::runtime_error("NDNSF_DATA_V1 group key changed during signing");
  return signature;
}

bool
ProviderGroupCoordinator::verifyTensorObjectManifest(
  const ProviderGroupBytes& signingBytes,
  const ProviderGroupBytes& signature) const
{
  NativeWorkScope verifyWork(m_options.workStatistics.lock(), "tensor-manifest-auth-verify",
                             NativeWorkKind::LOCAL);
  if (!m_hasCapability || terminal() || signingBytes.empty() ||
      signature.empty()) {
    return false;
  }
  const auto snapshot = captureRuntimeKey();
  const auto verified = m_options.verifyManifest
    ? m_options.verifyManifest(signingBytes, signature)
    : [&] {
        const auto expected = hmac(snapshot.key, signingBytes);
        return expected.size() == signature.size() &&
               CRYPTO_memcmp(expected.data(), signature.data(), expected.size()) == 0;
      }();
  return verified && runtimeKeyActive(snapshot);
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

ProviderGroupBytes
ProviderGroupCoordinator::encodeSegmentCompact(
  const CollectiveOperationManifestV1& manifest,
  const NdnsfDataV1Segment& segment)
{
  manifest.validate();
  if (manifest.externalSegmentDigests) {
    throw std::invalid_argument(
      "cannot compact-encode an external segment manifest");
  }
  segment.validate();
  if (segment.descriptor.manifestDigest != manifest.digest() ||
      segment.descriptor.segmentCount != manifest.segmentCount ||
      segment.descriptor.totalBytes != manifest.totalBytes ||
      segment.descriptor.segmentSize != manifest.segmentSize ||
      segment.descriptor.segmentNo >= manifest.segmentCount) {
    throw std::invalid_argument("NDNSF_DATA_V1 segment does not match manifest");
  }
  std::uint64_t producerRank = 0;
  try {
    producerRank = std::stoull(segment.descriptor.producerRank);
  }
  catch (const std::exception&) {
    throw std::invalid_argument("NDNSF_DATA_V1 compact producer rank is not numeric");
  }
  const auto manifestDigest = decodeHexDigest(manifest.digest());
  if (std::to_string(producerRank) != segment.descriptor.producerRank) {
    throw std::invalid_argument("NDNSF_DATA_V1 compact rank is not canonical");
  }
  Bytes wire;
  appendString(wire, COMPACT_SEGMENT_BUNDLE_MARKER);
  wire.insert(wire.end(), manifestDigest.begin(), manifestDigest.end());
  appendU64(wire, producerRank);
  appendU64(wire, segment.descriptor.segmentNo);
  appendBytes(wire, segment.nonce);
  appendBytes(wire, segment.ciphertext);
  appendBytes(wire, segment.authTag);
  appendBytes(wire, segment.hmac);
  if (wire.size() > (64U << 20)) {
    throw std::invalid_argument("NDNSF_DATA_V1 compact segment bundle exceeds bound");
  }
  return wire;
}

SealedCollectiveOperationV1
ProviderGroupCoordinator::decodeSegment(const ProviderGroupBytes& wire,
                                         const std::string& expectedDataName)
{
  if (wire.size() > (64U << 20)) {
    throw std::invalid_argument("NDNSF_DATA_V1 segment bundle exceeds bound");
  }
  WireCursor cursor(wire);
  const auto marker = cursor.readString();
  const bool compact = marker == COMPACT_SEGMENT_BUNDLE_MARKER;
  if (marker != "NDNSF_DATA_V1_SEGMENT_BUNDLE" && !compact) {
    throw std::invalid_argument("invalid NDNSF_DATA_V1 segment bundle marker");
  }
  SealedCollectiveOperationV1 result;
  auto& manifest = result.manifest;
  if (compact) {
    if (expectedDataName.empty()) {
      throw std::invalid_argument(
        "NDNSF_DATA_V1 compact segment requires its exact Data name");
    }
    manifest.transportManifestDigest = encodeHexDigest(
      cursor.readFixedBytes(DIGEST_BYTES));
    manifest.externalSegmentDigests = true;
    manifest.producerRank = std::to_string(cursor.readU64());
    const auto segmentNo = cursor.readU64();
    NdnsfDataV1Segment segment;
    segment.dataName = expectedDataName;
    segment.descriptor.manifestDigest = manifest.transportManifestDigest;
    segment.descriptor.operationIndex = 0;
    segment.descriptor.producerRank = manifest.producerRank;
    segment.descriptor.segmentNo = segmentNo;
    segment.nonce = cursor.readBytes(NONCE_BYTES);
    segment.ciphertext = cursor.readBytes(16U << 20);
    segment.authTag = cursor.readBytes(16);
    segment.hmac = cursor.readBytes(DIGEST_BYTES);
    if (!cursor.atEnd()) {
      throw std::invalid_argument("trailing compact NDNSF_DATA_V1 segment bytes");
    }
    result.segments.push_back(std::move(segment));
    return result;
  }
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
  // Compact exact segments deliberately omit the operation-wide plaintext
  // digest list and producer signature. The outer signed TensorObject
  // manifest authenticates every ciphertext segment and its final content
  // digest; the segment AEAD/HMAC still binds this operation manifest digest.
  if (!manifest.externalSegmentDigests) {
    const auto manifestBytes = manifest.canonicalBytes(false);
    const auto verified = verifyTensorObjectManifest(manifestBytes, manifest.producerSignature);
    if (!verified) {
      throw std::runtime_error("manifest authenticator verification failed");
    }
  }
}

ProviderGroupBytes
ProviderGroupCoordinator::openSegment(
  const CollectiveOperationManifestV1& manifest,
  const NdnsfDataV1Segment& segment,
  const std::string& expectedDataName)
{
  const auto statistics = m_options.workStatistics.lock();
  NativeWorkScope openWork(statistics, "tensor-segment-open", NativeWorkKind::LOCAL);
  NativeWorkScope bindingWork(statistics, "tensor-segment-binding", NativeWorkKind::LOCAL);
  const auto expectedName = explicitMemberDataName(
    m_capability, manifest.producerRank, expectedDataName);
  const auto snapshot = captureRuntimeKey();
  validateManifestAgainstCapability(manifest);
  if (segment.descriptor.manifestDigest != manifestDigestForTransport(manifest) ||
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
  bindingWork.finish();
  NativeWorkScope keyWork(statistics, "tensor-operation-key-derive", NativeWorkKind::LOCAL);
  const auto operationKey = deriveOperationKey(snapshot.key, m_capability, manifest);
  keyWork.finish();
  NativeWorkScope decryptWork(statistics, "tensor-segment-aead-open", NativeWorkKind::LOCAL);
  auto plaintext = NdnsfCollectiveControl::open(segment, operationKey, expectedName);
  decryptWork.finish();
  if (!manifest.externalSegmentDigests &&
      hex(plaintext) != manifest.orderedSegmentDigests[segment.descriptor.segmentNo]) {
    throw std::runtime_error("NDNSF_DATA_V1 plaintext digest mismatch");
  }
  if (!runtimeKeyActive(snapshot)) {
    cleanse(plaintext);
    throw std::runtime_error("NDNSF_DATA_V1 group key changed during decryption");
  }
  return plaintext;
}

DataSegmentReplayWindow&
ProviderGroupCoordinator::replayWindow(
  const CollectiveOperationManifestV1& manifest)
{
  // Called only while acceptSegment owns m_stateMutex; the reference stays local.
  const auto key = manifestDigestForTransport(manifest);
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
  return acceptAndOpenSegment(manifest, segment, expectedDataName).first;
}

std::pair<DataSegmentReplayWindow::Result, ProviderGroupBytes>
ProviderGroupCoordinator::acceptAndOpenSegment(
  const CollectiveOperationManifestV1& manifest,
  const NdnsfDataV1Segment& segment,
  const std::string& expectedDataName)
{
  // Reject absent/foreign references before changing operation/replay state.
  explicitMemberDataName(m_capability, manifest.producerRank, expectedDataName);
  const auto snapshot = captureRuntimeKey();
  {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!runtimeKeyActiveLocked(snapshot))
      throw std::runtime_error("NDNSF_DATA_V1 group is terminal");
    if (m_groupStartedAtMs == 0)
      m_groupStartedAtMs = manifest.createdAtMs;
  }
  auto plaintext = openSegment(manifest, segment, expectedDataName);
  std::lock_guard<std::mutex> lock(m_stateMutex);
  if (!runtimeKeyActiveLocked(snapshot)) {
    cleanse(plaintext);
    throw std::runtime_error("NDNSF_DATA_V1 group key changed before acceptance");
  }
  const auto result = replayWindow(manifest).acceptAuthenticated(segment, plaintext.size());
  if (result == DataSegmentReplayWindow::Result::Accepted) {
    m_lastProgressMs = std::max(m_lastProgressMs, manifest.createdAtMs);
  }
  return {result, std::move(plaintext)};
}

bool
ProviderGroupCoordinator::beginOperation(std::uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_stateMutex);
  if (!m_hasCapability || terminal()) {
    return false;
  }
  if (deadlineExpiredLocked(nowMs)) {
    terminateLocked("NDNSF_DATA_V1_HARD_DEADLINE", false);
    return false;
  }
  // A capability can span multiple dependency operations.  Reset only the
  // transport-progress baseline here; the authenticated group hard deadline
  // remains anchored to the first accepted segment.
  m_lastProgressMs = nowMs;
  return true;
}

bool
ProviderGroupCoordinator::recordProgress(std::uint64_t nowMs)
{
  std::lock_guard<std::mutex> lock(m_stateMutex);
  if (!m_hasCapability || terminal()) {
    return false;
  }
  if (deadlineExpiredLocked(nowMs)) {
    terminateLocked("NDNSF_DATA_V1_HARD_DEADLINE", false);
    return false;
  }
  if (m_lastProgressMs != 0 && nowMs >= m_lastProgressMs &&
      nowMs - m_lastProgressMs > m_capability.noProgressMs) {
    // The progress deadline is intentionally not an extension of the hard
    // deadline; it terminates the complete epoch.
    terminateLocked("NDNSF_DATA_V1_NO_PROGRESS", false);
    return false;
  }
  m_lastProgressMs = nowMs;
  return true;
}

bool
ProviderGroupCoordinator::deadlineExpired(std::uint64_t nowMs) const
{
  std::lock_guard<std::mutex> lock(m_stateMutex);
  return deadlineExpiredLocked(nowMs);
}

bool
ProviderGroupCoordinator::deadlineExpiredLocked(std::uint64_t nowMs) const
{
  return m_groupStartedAtMs != 0 && nowMs >= m_groupStartedAtMs &&
         nowMs - m_groupStartedAtMs >= m_capability.hardDeadlineMs;
}

void
ProviderGroupCoordinator::cancel(std::string reason)
{
  std::lock_guard<std::mutex> lock(m_stateMutex);
  terminateLocked(std::move(reason), true);
}

void
ProviderGroupCoordinator::fail(std::string reason)
{
  std::lock_guard<std::mutex> lock(m_stateMutex);
  terminateLocked(std::move(reason), false);
}

void
ProviderGroupCoordinator::terminateLocked(std::string reason, bool cancelled)
{
  if (terminal()) {
    return;
  }
  m_terminalReason = std::move(reason);
  clearEpochKeyLocked();
  if (cancelled)
    m_cancelled.store(true, std::memory_order_release);
  else
    m_failed.store(true, std::memory_order_release);
}

bool
ProviderGroupCoordinator::terminal() const noexcept
{
  return m_cancelled.load(std::memory_order_acquire) ||
         m_failed.load(std::memory_order_acquire);
}

bool
ProviderGroupCoordinator::cancelled() const noexcept
{
  return m_cancelled.load(std::memory_order_acquire);
}

bool
ProviderGroupCoordinator::failed() const noexcept
{
  return m_failed.load(std::memory_order_acquire);
}

const std::string&
ProviderGroupCoordinator::terminalReason() const noexcept
{
  // Terminal flags publish the immutable reason. Lifecycle reset is exclusive.
  static const std::string empty;
  return terminal() ? m_terminalReason : empty;
}

void
ProviderGroupCoordinator::clearEpochKey() noexcept
{
  const auto statistics = m_options.workStatistics.lock();
  NativeWorkScope lockWait(statistics, "group-key-lock", NativeWorkKind::WAIT);
  std::lock_guard<std::mutex> lock(m_stateMutex);
  lockWait.finish();
  NativeWorkScope clearWork(statistics, "group-key-clear", NativeWorkKind::LOCAL);
  clearEpochKeyLocked();
}

void
ProviderGroupCoordinator::clearEpochKeyLocked() noexcept
{
  ++m_keyGeneration;
  cleanse(m_epochKey);
  m_replayWindows.clear();
}

std::string
ProviderGroupCoordinator::makeDataName(const GroupCapabilityV1& capability,
                                       const CollectiveOperationManifestV1& manifest,
                                       std::uint64_t segmentNo)
{
  // A capability/embedded manifest has no sealed tensor endpoint or naming
  // stride. Keep the public symbol, but never fabricate a publication name.
  (void)capability;
  (void)manifest;
  (void)segmentNo;
  throw std::invalid_argument("C04 tensor endpoint exact Data name is required");
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
  appendString(info, manifestDigestForTransport(manifest));
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
  appendString(input, manifestDigestForTransport(manifest));
  appendString(input, exactDataName);
  appendU64(input, segmentNo);
  const auto digest = sha256(input);
  return Bytes(digest.begin(), digest.begin() + NONCE_BYTES);
}

} // namespace ndnsf::di
