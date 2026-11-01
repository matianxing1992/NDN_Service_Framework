#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePortableKv.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

namespace ndnsf::di {
namespace {

[[noreturn]] void invalid()
{
  throw std::invalid_argument("native-portable-kv-invalid");
}

void checkCancelled(const NativePortableKvLimitsV1& limits)
{
  if (limits.cancelled && limits.cancelled())
    throw std::runtime_error("native-portable-kv-cancelled");
}

void addBounded(std::uint64_t& sum, std::uint64_t value, std::uint64_t limit)
{
  if (sum > limit || value > limit - sum) invalid();
  sum += value;
}

// Borrow the maintained NDITB001 little-endian framing without decoding payload.
template<typename T>
T readUnsigned(const std::vector<std::uint8_t>& wire, std::size_t& offset)
{
  if (offset > wire.size() || sizeof(T) > wire.size() - offset) invalid();
  T value = 0;
  for (std::size_t i = 0; i != sizeof(T); ++i)
    value |= static_cast<T>(wire[offset + i]) << (i * 8);
  offset += sizeof(T);
  return value;
}

std::uint64_t elementWidth(TensorElementType type)
{
  switch (type) {
    case TensorElementType::Float32: return 4;
    case TensorElementType::Float16: return 2;
    case TensorElementType::Int64: return 8;
    case TensorElementType::Bool:
    case TensorElementType::UInt8: return 1;
  }
  invalid();
}

void preflightComponent(const NativePortableKvComponentV1& component)
{
  const auto& wire = component.tensorWire;
  if (!isEncodedTensorBundle(wire)) invalid();
  std::size_t offset = 8; // NDITB001
  if (readUnsigned<std::uint32_t>(wire, offset) != 1) invalid();
  const auto nameBytes = readUnsigned<std::uint32_t>(wire, offset);
  if (nameBytes == 0 || nameBytes > 1024 || nameBytes != component.inputName.size() ||
      nameBytes > wire.size() - offset ||
      std::memcmp(component.inputName.data(), wire.data() + offset, nameBytes) != 0)
    invalid();
  offset += nameBytes;
  const auto type = readUnsigned<std::uint32_t>(wire, offset);
  if (type != static_cast<std::uint32_t>(component.elementType)) invalid();
  const auto width = elementWidth(component.elementType);
  const auto rank = readUnsigned<std::uint32_t>(wire, offset);
  if (rank > 16 || rank != component.shape.size()) invalid();
  std::uint64_t elements = 1;
  for (const auto expected : component.shape) {
    const auto dim = readUnsigned<std::uint64_t>(wire, offset);
    if (expected < 0 || dim > (1ULL << 31) || dim != static_cast<std::uint64_t>(expected) ||
        (dim != 0 && elements > std::numeric_limits<std::uint64_t>::max() / dim))
      invalid();
    elements *= dim;
  }
  constexpr std::uint64_t payloadLimit = 512ULL * 1024 * 1024;
  if (elements > payloadLimit / width) invalid();
  const auto payloadBytes = readUnsigned<std::uint64_t>(wire, offset);
  if (payloadBytes != elements * width || payloadBytes != component.payloadBytes ||
      payloadBytes != wire.size() - offset)
    invalid();
}

std::string contentDigest(const std::vector<std::uint8_t>& wire)
{
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
    EVP_MD_CTX_new(), EVP_MD_CTX_free);
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned length = 0;
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), wire.data(), wire.size()) != 1 ||
      EVP_DigestFinal_ex(context.get(), digest, &length) != 1 || length != 32)
    throw std::runtime_error("native-portable-kv-hash-failed");
  constexpr char hex[] = "0123456789abcdef";
  std::string result = "sha256:";
  result.reserve(71);
  for (unsigned i = 0; i != length; ++i) {
    result.push_back(hex[digest[i] >> 4]);
    result.push_back(hex[digest[i] & 15]);
  }
  return result;
}

} // namespace

void validateNativePortableKvSnapshot(const NativePortableKvSnapshotV1& snapshot,
                                      const NativePortableKvLimitsV1& limits)
{
  if (limits.maxPayloadBytes == 0 || limits.maxEncodedBytes < limits.maxPayloadBytes ||
      snapshot.version != 1 || snapshot.logicalBytes == 0 ||
      snapshot.components.empty() || snapshot.components.size() > 256)
    invalid();
  checkCancelled(limits);
  snapshot.originIdentity.validate();
  decode_state_identity_detail::requireDigest(snapshot.stateIoDigest, "state IO");
  std::uint64_t payloadBytes = 0;
  std::uint64_t encodedBytes = 0;
  std::set<std::string> names;
  // Preflight all components before hashing or any caller's tensor decode.
  for (const auto& component : snapshot.components) {
    checkCancelled(limits);
    if (component.inputName.empty() || component.inputName.size() > 1024 ||
        !names.insert(component.inputName).second)
      invalid();
    addBounded(encodedBytes, component.tensorWire.size(), limits.maxEncodedBytes);
    preflightComponent(component);
    addBounded(payloadBytes, component.payloadBytes, limits.maxPayloadBytes);
    decode_state_identity_detail::requireDigest(component.contentDigest, "content");
  }
  if (payloadBytes != snapshot.logicalBytes || encodedBytes != snapshot.encodedBytes) invalid();
  for (const auto& component : snapshot.components) {
    checkCancelled(limits);
    if (contentDigest(component.tensorWire) != component.contentDigest) invalid();
  }
  checkCancelled(limits);
}

bool nativePortableKvIdentityCompatible(const DecodeStateIdentityV1& origin,
                                        const DecodeStateIdentityV1& destination)
{
  origin.validate();
  destination.validate();
  auto rebound = origin;
  rebound.providerIdentity = destination.providerIdentity;
  rebound.providerBootId = destination.providerBootId;
  rebound.cacheEpoch = destination.cacheEpoch;
  rebound.requestId = destination.requestId;
  rebound.attemptEpoch = destination.attemptEpoch;
  rebound.generationId = destination.generationId;
  return rebound == destination;
}

} // namespace ndnsf::di
