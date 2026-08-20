#ifndef NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::di {

enum class TensorElementType : std::uint32_t
{
  Float32 = 1,
  Float16 = 2,
  Int64 = 3,
  Bool = 4,
};

struct NamedTensor
{
  std::string name;
  TensorElementType elementType = TensorElementType::Float32;
  std::vector<std::int64_t> shape;
  std::vector<std::uint8_t> payload;
};

/**
 * Signed root manifest for one immutable request-scoped tensor object.
 *
 * The producer signature covers signingBytes(). The encoded manifest is then
 * carried in an identity-signed NDN Data packet. `manifestContractDigest` is
 * known at plan time; `objectManifestDigest` is computed after execution and
 * binds the concrete content plus every ciphertext segment digest.
 */
struct TensorObjectManifestV1
{
  std::string capabilityDigest;
  std::string epochKeyId;
  std::string requester;
  std::string requestId;
  std::string attemptId;
  std::string planDigest;
  std::string groupId;
  std::string epoch;
  std::uint64_t operationIndex = 0;
  std::uint64_t round = 0;
  std::string operationKind;
  std::string producerRole;
  std::uint64_t producerRank = 0;
  std::vector<std::string> consumerRoles;
  std::uint64_t microbatch = 0;
  std::string sourceLayoutDigest;
  std::string targetLayoutDigest;
  std::string tensorId;
  std::string tensorDigest;
  std::string contentDigest;
  std::uint64_t totalBytes = 0;
  std::uint64_t segmentSize = 0;
  std::uint64_t segmentCount = 0;
  std::vector<std::string> orderedSegmentDigests;
  std::uint64_t createdAtMs = 0;
  std::uint64_t noProgressMs = 0;
  std::uint64_t hardDeadlineMs = 0;
  std::string endpointDigest;
  std::string manifestContractDigest;
  std::vector<std::uint8_t> producerSignature;
  std::string objectManifestDigest;

  void validate() const;
  std::vector<std::uint8_t> signingBytes() const;
  std::string digest() const;
};

std::vector<std::uint8_t>
encodeTensorObjectManifest(const TensorObjectManifestV1& manifest);

TensorObjectManifestV1
decodeTensorObjectManifest(const std::vector<std::uint8_t>& wire);

std::string
sha256TensorBytes(const std::vector<std::uint8_t>& bytes);

std::size_t
tensorElementByteSize(TensorElementType elementType);

void
validateNamedTensor(const NamedTensor& tensor);

bool
isEncodedTensorBundle(const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t>
float32Payload(std::initializer_list<float> values);

NamedTensor
makeFloat32Tensor(std::string name,
                  std::vector<std::int64_t> shape,
                  const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t>
encodeTensorBundle(const std::vector<NamedTensor>& tensors);

std::vector<NamedTensor>
decodeTensorBundle(const std::vector<std::uint8_t>& payload);

const NamedTensor&
findTensor(const std::vector<NamedTensor>& tensors, const std::string& name);

std::vector<NamedTensor>
selectTensors(const std::vector<NamedTensor>& tensors,
              const std::vector<std::string>& names);

TensorBundle
makeEncodedTensorBundle(std::string name, const std::vector<NamedTensor>& tensors);

TensorBundle
selectTensorBundle(std::string name,
                   const TensorBundle& bundle,
                   const std::vector<std::string>& tensorNames);

/** Apply adapter-certified GATHER/SCATTER/RESHARD transitions to role inputs. */
std::map<std::string, TensorBundle>
applyCertifiedTensorRedistributions(const RoleExecutionContext& context);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP
