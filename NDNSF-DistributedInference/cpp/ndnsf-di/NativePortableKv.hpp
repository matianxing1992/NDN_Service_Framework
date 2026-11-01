#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PORTABLE_KV_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PORTABLE_KV_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/DecodeStateIdentity.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Mandatory allocation budgets for one portable state operation. */
struct NativePortableKvLimitsV1
{
  std::uint64_t maxPayloadBytes = 0;
  std::uint64_t maxEncodedBytes = 0;
  // Nonblocking, thread-safe probe; must not re-enter runner/store operations.
  std::function<bool()> cancelled;
};

/** One successor-input tensor. Plaintext process-local data, never loggable. */
struct NativePortableKvComponentV1
{
  std::string inputName;
  TensorElementType elementType = TensorElementType::Float32;
  std::vector<std::int64_t> shape;
  std::uint64_t payloadBytes = 0;
  std::string contentDigest;
  // Exactly one NamedTensor in the maintained NDITB001 codec.
  std::vector<std::uint8_t> tensorWire;
};

/** Complete immutable origin state; destination incarnation is bound separately. */
struct NativePortableKvSnapshotV1
{
  std::uint32_t version = 1;
  DecodeStateIdentityV1 originIdentity;
  std::string stateIoDigest;
  std::vector<NativePortableKvComponentV1> components;
  std::uint64_t logicalBytes = 0;
  std::uint64_t encodedBytes = 0;
};

/** Check framing, actual byte budgets and hashes without allocating tensor payload.
 * Throws invalid_argument for malformed data/budgets and runtime_error on cancel.
 * This validates representation, not authorization or adapter-specific state IO.
 */
void validateNativePortableKvSnapshot(const NativePortableKvSnapshotV1& snapshot,
                                      const NativePortableKvLimitsV1& limits);

/** Compare complete semantics while allowing a fresh destination incarnation.
 * Both identities must validate. This predicate does not grant user authority.
 */
bool nativePortableKvIdentityCompatible(const DecodeStateIdentityV1& origin,
                                        const DecodeStateIdentityV1& destination);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PORTABLE_KV_HPP
