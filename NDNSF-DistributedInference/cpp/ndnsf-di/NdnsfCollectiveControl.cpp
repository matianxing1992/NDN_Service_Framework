#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollectiveControl.hpp"

#include "ndn-service-framework/HybridMessageCrypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

constexpr std::uint16_t WIRE_VERSION = 1;
constexpr std::size_t MAX_STRING_BYTES = 1U << 20;
constexpr std::size_t MAX_SEGMENT_BYTES = 16U << 20;
constexpr std::size_t SHA256_BYTES = 32;
constexpr std::size_t GCM_NONCE_BYTES = 12;
constexpr std::size_t GCM_TAG_BYTES = 16;

using Bytes = std::vector<std::uint8_t>;

void
appendU16(Bytes& out, std::uint16_t value)
{
  out.push_back(static_cast<std::uint8_t>(value >> 8));
  out.push_back(static_cast<std::uint8_t>(value));
}

void
appendU32(Bytes& out, std::uint32_t value)
{
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

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
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("NDNSF_DATA_V1 string is too large");
  }
  appendU32(out, static_cast<std::uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

void
appendBytes(Bytes& out, const Bytes& value)
{
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("NDNSF_DATA_V1 byte field is too large");
  }
  appendU32(out, static_cast<std::uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

class Cursor
{
public:
  explicit Cursor(const Bytes& input)
    : m_input(input)
  {
  }

  std::uint16_t
  readU16()
  {
    require(2);
    const auto value = static_cast<std::uint16_t>(m_input[m_offset] << 8) |
                       static_cast<std::uint16_t>(m_input[m_offset + 1]);
    m_offset += 2;
    return value;
  }

  std::uint32_t
  readU32()
  {
    require(4);
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value = (value << 8) | m_input[m_offset + i];
    }
    m_offset += 4;
    return value;
  }

  std::uint64_t
  readU64()
  {
    require(8);
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value = (value << 8) | m_input[m_offset + i];
    }
    m_offset += 8;
    return value;
  }

  std::string
  readString(std::size_t maxBytes = MAX_STRING_BYTES)
  {
    const auto size = readU32();
    if (size > maxBytes) {
      throw std::invalid_argument("NDNSF_DATA_V1 string exceeds bound");
    }
    require(size);
    std::string value(reinterpret_cast<const char*>(m_input.data() + m_offset), size);
    m_offset += size;
    return value;
  }

  Bytes
  readBytes(std::size_t maxBytes = MAX_SEGMENT_BYTES)
  {
    const auto size = readU32();
    if (size > maxBytes) {
      throw std::invalid_argument("NDNSF_DATA_V1 bytes exceed bound");
    }
    require(size);
    Bytes value(m_input.begin() + static_cast<std::ptrdiff_t>(m_offset),
                m_input.begin() + static_cast<std::ptrdiff_t>(m_offset + size));
    m_offset += size;
    return value;
  }

  std::string
  readMagic()
  {
    require(std::strlen(NDNSF_DATA_V1));
    std::string value(reinterpret_cast<const char*>(m_input.data() + m_offset),
                      std::strlen(NDNSF_DATA_V1));
    m_offset += std::strlen(NDNSF_DATA_V1);
    return value;
  }

  bool
  atEnd() const noexcept
  {
    return m_offset == m_input.size();
  }

private:
  void
  require(std::size_t bytes) const
  {
    if (bytes > m_input.size() - m_offset) {
      throw std::invalid_argument("truncated NDNSF_DATA_V1 wire");
    }
  }

private:
  const Bytes& m_input;
  std::size_t m_offset = 0;
};

void
appendDescriptor(Bytes& out, const CollectiveSegmentDescriptor& descriptor,
                 const std::string& dataName)
{
  appendString(out, descriptor.requestId);
  appendString(out, descriptor.attemptId);
  appendString(out, descriptor.planDigest);
  appendString(out, descriptor.groupId);
  appendU64(out, descriptor.epoch);
  appendU64(out, descriptor.operationIndex);
  appendString(out, descriptor.operationKind);
  appendString(out, descriptor.producerRank);
  appendString(out, descriptor.tensorDigest);
  appendString(out, descriptor.manifestDigest);
  appendU64(out, descriptor.segmentNo);
  appendU64(out, descriptor.segmentCount);
  appendU64(out, descriptor.totalBytes);
  appendU64(out, descriptor.segmentSize);
  appendU64(out, descriptor.noProgressMs);
  appendU64(out, descriptor.hardDeadlineMs);
  appendString(out, dataName);
}

Bytes
sha256(const Bytes& input)
{
  Bytes output(SHA256_BYTES);
  unsigned int length = 0;
  auto* context = EVP_MD_CTX_new();
  if (context == nullptr ||
      EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, input.data(), input.size()) != 1 ||
      EVP_DigestFinal_ex(context, output.data(), &length) != 1 ||
      length != SHA256_BYTES) {
    EVP_MD_CTX_free(context);
    throw std::runtime_error("NDNSF_DATA_V1 SHA-256 failed");
  }
  EVP_MD_CTX_free(context);
  return output;
}

Bytes
deriveHmacKey(const Bytes& epochKey)
{
  Bytes input{'N', 'D', 'N', 'S', 'F', '_', 'D', 'A', 'T', 'A',
              '_', 'V', '1', '/', 'H', 'M', 'A', 'C'};
  input.insert(input.end(), epochKey.begin(), epochKey.end());
  return sha256(input);
}

Bytes
hmacSha256(const Bytes& key, const Bytes& input)
{
  Bytes output(EVP_MAX_MD_SIZE);
  unsigned int length = 0;
  if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
           input.data(), input.size(), output.data(), &length) == nullptr ||
      length != SHA256_BYTES) {
    throw std::runtime_error("NDNSF_DATA_V1 HMAC failed");
  }
  output.resize(length);
  return output;
}

Bytes
authInput(const NdnsfDataV1Segment& segment)
{
  Bytes input = segment.descriptor.associatedData(segment.dataName);
  appendBytes(input, segment.nonce);
  appendBytes(input, segment.ciphertext);
  appendBytes(input, segment.authTag);
  return input;
}

std::string
hexDigest(const Bytes& input)
{
  static constexpr char HEX[] = "0123456789abcdef";
  const auto digest = sha256(input);
  std::string value;
  value.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    value.push_back(HEX[byte >> 4]);
    value.push_back(HEX[byte & 0x0f]);
  }
  return value;
}

ndn::Buffer
toNdnBuffer(const Bytes& bytes)
{
  return ndn::Buffer(bytes.data(), bytes.size());
}

Bytes
fromNdnBuffer(const ndn::Buffer& buffer)
{
  return Bytes(buffer.begin(), buffer.end());
}

} // namespace

void
CollectiveSegmentDescriptor::validate() const
{
  if (requestId.empty() || attemptId.empty() || planDigest.empty() ||
      groupId.empty() || operationKind.empty() || producerRank.empty() ||
      tensorDigest.empty() || manifestDigest.empty()) {
    throw std::invalid_argument("NDNSF_DATA_V1 descriptor identity is incomplete");
  }
  bool minimumBytesInvalid = false;
  if (segmentCount > 1) {
    minimumBytesInvalid = segmentSize > totalBytes / (segmentCount - 1);
  }
  if (epoch == 0 || segmentCount == 0 || segmentNo >= segmentCount ||
      totalBytes == 0 || segmentSize == 0 || segmentSize > MAX_SEGMENT_BYTES ||
      minimumBytesInvalid || totalBytes < segmentSize * (segmentCount - 1) ||
      noProgressMs == 0 || hardDeadlineMs < noProgressMs) {
    throw std::invalid_argument("NDNSF_DATA_V1 descriptor bounds are invalid");
  }
  if (segmentCount > (1U << 20)) {
    throw std::invalid_argument("NDNSF_DATA_V1 segment count exceeds bound");
  }
}

std::vector<std::uint8_t>
CollectiveSegmentDescriptor::associatedData(const std::string& dataName) const
{
  if (dataName.empty()) {
    throw std::invalid_argument("NDNSF_DATA_V1 Data name is empty");
  }
  Bytes result;
  result.reserve(512 + dataName.size());
  appendString(result, NDNSF_DATA_V1);
  appendDescriptor(result, *this, dataName);
  return result;
}

void
NdnsfDataV1Segment::validate() const
{
  descriptor.validate();
  if (magic != NDNSF_DATA_V1 || dataName.empty() ||
      nonce.size() != GCM_NONCE_BYTES || authTag.size() != GCM_TAG_BYTES ||
      hmac.size() != SHA256_BYTES || ciphertext.empty() ||
      ciphertext.size() > descriptor.segmentSize) {
    throw std::invalid_argument("NDNSF_DATA_V1 segment structure is invalid");
  }
}

std::vector<std::uint8_t>
NdnsfDataV1Segment::wireEncode() const
{
  validate();
  Bytes wire;
  wire.reserve(1024 + ciphertext.size());
  wire.insert(wire.end(), magic.begin(), magic.end());
  appendU16(wire, WIRE_VERSION);
  appendString(wire, descriptor.requestId);
  appendString(wire, descriptor.attemptId);
  appendString(wire, descriptor.planDigest);
  appendString(wire, descriptor.groupId);
  appendU64(wire, descriptor.epoch);
  appendU64(wire, descriptor.operationIndex);
  appendString(wire, descriptor.operationKind);
  appendString(wire, descriptor.producerRank);
  appendString(wire, descriptor.tensorDigest);
  appendString(wire, descriptor.manifestDigest);
  appendU64(wire, descriptor.segmentNo);
  appendU64(wire, descriptor.segmentCount);
  appendU64(wire, descriptor.totalBytes);
  appendU64(wire, descriptor.segmentSize);
  appendU64(wire, descriptor.noProgressMs);
  appendU64(wire, descriptor.hardDeadlineMs);
  appendString(wire, dataName);
  appendBytes(wire, nonce);
  appendBytes(wire, ciphertext);
  appendBytes(wire, authTag);
  appendBytes(wire, hmac);
  return wire;
}

NdnsfDataV1Segment
NdnsfDataV1Segment::wireDecode(const std::vector<std::uint8_t>& wire)
{
  if (wire.size() > MAX_SEGMENT_BYTES + (4U << 20)) {
    throw std::invalid_argument("NDNSF_DATA_V1 wire exceeds bound");
  }
  Cursor cursor(wire);
  NdnsfDataV1Segment segment;
  segment.magic = cursor.readMagic();
  if (cursor.readU16() != WIRE_VERSION) {
    throw std::invalid_argument("unsupported NDNSF_DATA_V1 wire version");
  }
  segment.descriptor.requestId = cursor.readString();
  segment.descriptor.attemptId = cursor.readString();
  segment.descriptor.planDigest = cursor.readString();
  segment.descriptor.groupId = cursor.readString();
  segment.descriptor.epoch = cursor.readU64();
  segment.descriptor.operationIndex = cursor.readU64();
  segment.descriptor.operationKind = cursor.readString();
  segment.descriptor.producerRank = cursor.readString();
  segment.descriptor.tensorDigest = cursor.readString();
  segment.descriptor.manifestDigest = cursor.readString();
  segment.descriptor.segmentNo = cursor.readU64();
  segment.descriptor.segmentCount = cursor.readU64();
  segment.descriptor.totalBytes = cursor.readU64();
  segment.descriptor.segmentSize = cursor.readU64();
  segment.descriptor.noProgressMs = cursor.readU64();
  segment.descriptor.hardDeadlineMs = cursor.readU64();
  segment.dataName = cursor.readString();
  segment.nonce = cursor.readBytes(GCM_NONCE_BYTES);
  segment.ciphertext = cursor.readBytes(MAX_SEGMENT_BYTES);
  segment.authTag = cursor.readBytes(GCM_TAG_BYTES);
  segment.hmac = cursor.readBytes(SHA256_BYTES);
  if (!cursor.atEnd()) {
    throw std::invalid_argument("trailing bytes in NDNSF_DATA_V1 wire");
  }
  segment.validate();
  return segment;
}

NdnsfDataV1Segment
NdnsfCollectiveControl::seal(const CollectiveSegmentDescriptor& descriptor,
                             const std::string& dataName,
                             const std::vector<std::uint8_t>& epochKey,
                             const std::vector<std::uint8_t>& plaintext)
{
  descriptor.validate();
  if (epochKey.size() != ndn_service_framework::HybridMessageCrypto::MESSAGE_KEY_SIZE) {
    throw std::invalid_argument("NDNSF_DATA_V1 epoch key must be 256 bits");
  }
  if (dataName.empty() || plaintext.empty() || plaintext.size() > descriptor.segmentSize) {
    throw std::invalid_argument("NDNSF_DATA_V1 plaintext/name violates bounds");
  }

  const auto associatedData = descriptor.associatedData(dataName);
  const auto encrypted = ndn_service_framework::hybridAesGcmEncrypt(
    toNdnBuffer(epochKey),
    ndn::span<const std::uint8_t>(plaintext.data(), plaintext.size()),
    ndn::span<const std::uint8_t>(associatedData.data(), associatedData.size()));

  NdnsfDataV1Segment segment;
  segment.descriptor = descriptor;
  segment.dataName = dataName;
  segment.nonce = fromNdnBuffer(encrypted.nonce);
  segment.ciphertext = fromNdnBuffer(encrypted.ciphertext);
  segment.authTag = fromNdnBuffer(encrypted.tag);
  segment.hmac = hmacSha256(deriveHmacKey(epochKey), authInput(segment));
  segment.validate();
  return segment;
}

NdnsfDataV1Segment
NdnsfCollectiveControl::sealWithNonce(
  const CollectiveSegmentDescriptor& descriptor,
  const std::string& dataName,
  const std::vector<std::uint8_t>& operationKey,
  const std::vector<std::uint8_t>& nonce,
  const std::vector<std::uint8_t>& plaintext)
{
  descriptor.validate();
  if (operationKey.size() != ndn_service_framework::HybridMessageCrypto::MESSAGE_KEY_SIZE) {
    throw std::invalid_argument("NDNSF_DATA_V1 operation key must be 256 bits");
  }
  if (nonce.size() != GCM_NONCE_BYTES || dataName.empty() || plaintext.empty() ||
      plaintext.size() > descriptor.segmentSize) {
    throw std::invalid_argument("NDNSF_DATA_V1 deterministic nonce/plaintext violates bounds");
  }

  const auto associatedData = descriptor.associatedData(dataName);
  const auto encrypted = ndn_service_framework::hybridAesGcmEncryptWithNonce(
    toNdnBuffer(operationKey), toNdnBuffer(nonce),
    ndn::span<const std::uint8_t>(plaintext.data(), plaintext.size()),
    ndn::span<const std::uint8_t>(associatedData.data(), associatedData.size()));

  NdnsfDataV1Segment segment;
  segment.descriptor = descriptor;
  segment.dataName = dataName;
  segment.nonce = fromNdnBuffer(encrypted.nonce);
  segment.ciphertext = fromNdnBuffer(encrypted.ciphertext);
  segment.authTag = fromNdnBuffer(encrypted.tag);
  segment.hmac = hmacSha256(deriveHmacKey(operationKey), authInput(segment));
  segment.validate();
  return segment;
}

std::vector<std::uint8_t>
NdnsfCollectiveControl::open(const NdnsfDataV1Segment& segment,
                             const std::vector<std::uint8_t>& epochKey,
                             const std::string& expectedDataName)
{
  segment.validate();
  if (epochKey.size() != ndn_service_framework::HybridMessageCrypto::MESSAGE_KEY_SIZE) {
    throw std::invalid_argument("NDNSF_DATA_V1 epoch key must be 256 bits");
  }
  if (!expectedDataName.empty() && segment.dataName != expectedDataName) {
    throw std::runtime_error("NDNSF_DATA_V1 Data name mismatch");
  }
  const auto expectedHmac = hmacSha256(deriveHmacKey(epochKey), authInput(segment));
  if (expectedHmac.size() != segment.hmac.size() ||
      CRYPTO_memcmp(expectedHmac.data(), segment.hmac.data(), expectedHmac.size()) != 0) {
    throw std::runtime_error("NDNSF_DATA_V1 HMAC verification failed");
  }

  ndn_service_framework::HybridMessageEnvelope envelope;
  envelope.setVersion(2);
  envelope.setAlgorithm("AES-256-GCM");
  envelope.setKeyId(segment.descriptor.manifestDigest);
  envelope.setEpochId(std::to_string(segment.descriptor.epoch));
  envelope.setMessageType(NDNSF_DATA_V1);
  envelope.setNonce(toNdnBuffer(segment.nonce));
  envelope.setCipherText(toNdnBuffer(segment.ciphertext));
  envelope.setAuthTag(toNdnBuffer(segment.authTag));

  ndn::Buffer plaintext;
  const auto associatedData = segment.descriptor.associatedData(segment.dataName);
  if (!ndn_service_framework::hybridAesGcmDecrypt(
        toNdnBuffer(epochKey), envelope,
        ndn::span<const std::uint8_t>(associatedData.data(), associatedData.size()),
        plaintext)) {
    throw std::runtime_error("NDNSF_DATA_V1 AEAD verification failed");
  }
  return Bytes(plaintext.begin(), plaintext.end());
}

DataSegmentReplayWindow::DataSegmentReplayWindow(std::size_t maxSegments,
                                                 std::size_t maxBytes)
  : m_maxSegments(maxSegments)
  , m_maxBytes(maxBytes)
{
  if (m_maxSegments == 0 || m_maxBytes == 0) {
    throw std::invalid_argument("NDNSF_DATA_V1 replay bounds must be positive");
  }
}

DataSegmentReplayWindow::Result
DataSegmentReplayWindow::accept(const NdnsfDataV1Segment& segment,
                                const std::vector<std::uint8_t>& epochKey,
                                const std::string& expectedDataName)
{
  const auto plaintext = NdnsfCollectiveControl::open(segment, epochKey, expectedDataName);
  if (!m_bound) {
    m_bound = true;
    m_epoch = segment.descriptor.epoch;
    m_operationIndex = segment.descriptor.operationIndex;
    m_segmentCount = segment.descriptor.segmentCount;
    m_totalBytes = segment.descriptor.totalBytes;
    if (m_segmentCount > m_maxSegments || m_totalBytes > m_maxBytes) {
      throw std::invalid_argument("NDNSF_DATA_V1 replay window bounds exceeded");
    }
  }
  if (segment.descriptor.epoch != m_epoch ||
      segment.descriptor.operationIndex != m_operationIndex ||
      segment.descriptor.segmentCount != m_segmentCount ||
      segment.descriptor.totalBytes != m_totalBytes) {
    throw std::runtime_error("NDNSF_DATA_V1 replay epoch/operation mismatch");
  }

  Bytes digestInput = segment.ciphertext;
  digestInput.insert(digestInput.end(), segment.authTag.begin(), segment.authTag.end());
  const auto digest = hexDigest(digestInput);
  const auto found = m_seenDigests.find(segment.descriptor.segmentNo);
  if (found != m_seenDigests.end()) {
    if (found->second == digest) {
      return Result::Duplicate;
    }
    throw std::runtime_error("NDNSF_DATA_V1 conflicting duplicate segment");
  }
  if (m_seenDigests.size() >= m_maxSegments ||
      plaintext.size() > m_maxBytes - m_acceptedBytes) {
    throw std::invalid_argument("NDNSF_DATA_V1 replay window byte bound exceeded");
  }
  m_seenDigests.emplace(segment.descriptor.segmentNo, digest);
  m_acceptedBytes += plaintext.size();
  return Result::Accepted;
}

bool
DataSegmentReplayWindow::complete() const
{
  return m_bound && m_seenDigests.size() == m_segmentCount &&
         m_acceptedBytes == m_totalBytes;
}

std::size_t
DataSegmentReplayWindow::acceptedSegments() const noexcept
{
  return m_seenDigests.size();
}

std::size_t
DataSegmentReplayWindow::acceptedBytes() const noexcept
{
  return m_acceptedBytes;
}

} // namespace ndnsf::di
