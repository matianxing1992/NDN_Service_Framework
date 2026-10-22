#ifndef NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"

#include <ndn-cxx/util/span.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di {

enum class TensorElementType : std::uint32_t
{
  Float32 = 1,
  Float16 = 2,
  Int64 = 3,
  Bool = 4,
  UInt8 = 5,
};

struct NamedTensor
{
  std::string name;
  TensorElementType elementType = TensorElementType::Float32;
  std::vector<std::int64_t> shape;
  std::vector<std::uint8_t> payload;
};

/** Metadata owns its storage; payload borrows an unchanged encoded buffer. */
struct NamedTensorView
{
  std::string name;
  TensorElementType elementType = TensorElementType::Float32;
  std::vector<std::int64_t> shape;
  ndn::span<const std::uint8_t> payload;
};

/** Layout bytes known without allocating tensor data; not content authority. */
struct TensorBundleEncodingMember
{
  std::size_t nameBytes = 0;
  std::size_t rank = 0;
  std::uint64_t payloadBytes = 0;
};

/** Exact NDITB001 encoded size, including all framing, with codec limits. */
std::uint64_t
tensorBundleEncodedSize(const std::vector<TensorBundleEncodingMember>& members);

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
  /** Zero retains raw payload semantics; nonzero authenticates packed tensor
   *  encoding and the exact expansion size in the manifest signature/digest. */
  std::uint64_t unpackedBytes = 0;
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

  // The context-bound transport form omits fields that the authenticated
  // Selection edge and group capability reconstruct at the receiver.
  bool compactContextRequired = false;
  std::string compactSegmentDigestCommitment;

  void validate() const;
  std::vector<std::uint8_t> signingBytes() const;
  std::string digest() const;
};

std::vector<std::uint8_t>
encodeTensorObjectManifest(const TensorObjectManifestV1& manifest);

std::vector<std::uint8_t>
encodeTensorObjectManifestContextCompact(const TensorObjectManifestV1& manifest);

TensorObjectManifestV1
decodeTensorObjectManifest(const std::vector<std::uint8_t>& wire);

// Framing only: callers still verify the restored manifest and inner segment.
std::vector<std::uint8_t>
encodeTensorManifestWithFirstSegment(const std::vector<std::uint8_t>& manifest,
                                    const std::vector<std::uint8_t>& segment,
                                    std::size_t maxWireBytes);

// A legacy plain manifest returns an empty second member.
std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>
decodeTensorManifestWithFirstSegment(const std::vector<std::uint8_t>& wire,
                                    std::size_t maxWireBytes);

std::string
sha256TensorBytes(const std::vector<std::uint8_t>& bytes);

std::size_t
tensorElementByteSize(TensorElementType elementType);

/** Sum the decoded tensor payload bytes without counting bundle framing. */
std::size_t
tensorPayloadBytes(const std::vector<NamedTensor>& tensors);

/** Decode one bundle and sum its tensor payload bytes. */
std::size_t
tensorPayloadBytes(const TensorBundle& bundle);

void
validateNamedTensor(const NamedTensor& tensor);

bool
isEncodedTensorBundle(const std::vector<std::uint8_t>& payload);

/** Return a packed NDITB001 alternative only when it saves at least 64 bytes.
 *  Small, opaque, already packed and >64 MiB inputs return no alternative.
 *  Local tensor encoders and encoded-size budgets continue to use NDITB001.
 */
std::optional<std::vector<std::uint8_t>>
tryPackTensorBundleForTransport(const std::vector<std::uint8_t>& payload);

/** Restore packed tensor wire, bounded to 64 MiB; other representations pass through.
 *  Reject corrupt, truncated, trailing, nested and oversized packed representations.
 *  Network callers must authenticate the complete object and its explicit
 *  manifest packing declaration first, then supply the signed expansion bound.
 */
std::vector<std::uint8_t>
unpackTensorBundleFromTransport(const std::vector<std::uint8_t>& payload,
                               std::size_t maxExpansionBytes = 64U * 1024U * 1024U);

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

/**
 * Validate the complete bundle without copying tensor payloads.
 * The source buffer must outlive all views and remain unmoved and unmodified.
 * This is codec validation only, not signature or execution admission.
 */
std::vector<NamedTensorView>
decodeTensorBundleViews(const std::vector<std::uint8_t>& payload);

std::vector<NamedTensorView>
decodeTensorBundleViews(std::vector<std::uint8_t>&&) = delete;

std::vector<NamedTensorView>
decodeTensorBundleViews(const std::vector<std::uint8_t>&&) = delete;

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

/** Reserved tensor name used for authenticated generation-lineage metadata. */
const std::string&
generationEpochLineageTensorName();

TensorBundle
attachGenerationEpochLineage(const TensorBundle& bundle,
                             const GenerationEpochLineageV1& lineage);

std::optional<GenerationEpochLineageV1>
extractGenerationEpochLineage(const TensorBundle& bundle);

TensorBundle
stripGenerationEpochLineage(const TensorBundle& bundle);

/** Apply adapter-certified GATHER/SCATTER/RESHARD transitions to role inputs. */
std::map<std::string, TensorBundle>
applyCertifiedTensorRedistributions(const RoleExecutionContext& context);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP
