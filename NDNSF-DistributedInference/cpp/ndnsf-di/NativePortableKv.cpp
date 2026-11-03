#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePortableKv.hpp"

#include <openssl/evp.h>

#include <memory>
#include <set>
#include <stdexcept>

namespace ndnsf::di {
namespace {

[[noreturn]] void
invalid()
{
  throw std::invalid_argument("native-portable-kv-invalid");
}

void
checkCancelled(const NativePortableKvLimitsV1& limits)
{
  if (limits.cancelled && limits.cancelled()) {
    throw std::runtime_error("native-portable-kv-cancelled");
  }
}

void
addBounded(std::uint64_t& sum, std::uint64_t value, std::uint64_t limit)
{
  if (sum > limit || value > limit - sum) {
    invalid();
  }
  sum += value;
}

std::string
contentDigest(const std::vector<std::uint8_t>& wire)
{
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
    EVP_MD_CTX_new(), EVP_MD_CTX_free);
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned length = 0;
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), wire.data(), wire.size()) != 1 ||
      EVP_DigestFinal_ex(context.get(), digest, &length) != 1 || length != 32) {
    throw std::runtime_error("native-portable-kv-hash-failed");
  }
  constexpr char HEX[] = "0123456789abcdef";
  std::string result = "sha256:";
  result.reserve(71);
  for (unsigned i = 0; i != length; ++i) {
    result.push_back(HEX[digest[i] >> 4]);
    result.push_back(HEX[digest[i] & 15]);
  }
  return result;
}

} // namespace

void
validateNativePortableKvSnapshot(const NativePortableKvSnapshotV1& snapshot,
                                 const NativePortableKvLimitsV1& limits)
{
  if (limits.maxPayloadBytes == 0 || limits.maxEncodedBytes < limits.maxPayloadBytes ||
      snapshot.version != 1 || snapshot.logicalBytes == 0 ||
      snapshot.components.empty() || snapshot.components.size() > 256) {
    invalid();
  }
  checkCancelled(limits);
  snapshot.originIdentity.validate();
  decode_state_identity_detail::requireDigest(snapshot.stateIoDigest, "state IO");
  std::uint64_t payloadBytes = 0;
  std::uint64_t encodedBytes = 0;
  std::set<std::string> names;
  for (const auto& component : snapshot.components) {
    checkCancelled(limits);
    if (component.inputName.empty() || !names.insert(component.inputName).second) {
      invalid();
    }
    addBounded(encodedBytes, component.tensorWire.size(), limits.maxEncodedBytes);
    // The maintained codec validates framing, dtype, dimensions and byte sizes
    // without copying payloads. Keep one authoritative NDITB001 implementation.
    const auto tensors = decodeTensorBundleViews(component.tensorWire);
    if (tensors.size() != 1 || tensors.front().name != component.inputName ||
        tensors.front().elementType != component.elementType ||
        tensors.front().shape != component.shape ||
        tensors.front().payload.size() != component.payloadBytes) {
      invalid();
    }
    addBounded(payloadBytes, component.payloadBytes, limits.maxPayloadBytes);
    decode_state_identity_detail::requireDigest(component.contentDigest, "content");
  }
  if (payloadBytes != snapshot.logicalBytes || encodedBytes != snapshot.encodedBytes) {
    invalid();
  }
  for (const auto& component : snapshot.components) {
    checkCancelled(limits);
    if (contentDigest(component.tensorWire) != component.contentDigest) {
      invalid();
    }
  }
  checkCancelled(limits);
}

bool
nativePortableKvIdentityCompatible(const DecodeStateIdentityV1& origin,
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
