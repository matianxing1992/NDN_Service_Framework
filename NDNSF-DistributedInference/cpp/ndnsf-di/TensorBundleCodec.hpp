#ifndef NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_TENSOR_BUNDLE_CODEC_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/dataflow-types.hpp"

#include <ndn-cxx/util/span.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di {

class NativeWorkStatistics;

enum class TensorElementType : std::uint32_t
{
  Float32 = 1,
  Float16 = 2,
  Int64 = 3,
  Bool = 4,
  UInt8 = 5,
  Int32 = 6,
};

struct NamedTensor
{
  std::string name;
  TensorElementType elementType = TensorElementType::Float32;
  std::vector<std::int64_t> shape;
  std::vector<std::uint8_t> payload;
};

/** Metadata owns its storage; payload borrows unchanged host-readable storage. */
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

/** Compatibility entry point: always return no alternative; new transport is uncompressed.
 *  Existing packed objects remain readable through unpackTensorBundleFromTransport().
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
encodeTensorBundle(const std::vector<NamedTensor>& tensors,
                   const std::shared_ptr<NativeWorkStatistics>& workStatistics = {});

/** Element-wise float32 reduction of one contribution into `accumulator[slot]`.
 *
 * SUM is a local admission operation in the sealed step template, not a
 * service, worker or session: the consumer reduces the contributions it already
 * admitted, then runs its next entry. Keeping the arithmetic here rather than in
 * an adapter is what makes every backend reduce identically, so one rank's
 * rounding cannot decide a shared result.
 *
 * The first contribution initialises the slot. Later ones must agree on tensor
 * name and payload size, and the payload must be a whole number of float32
 * elements; a mismatch is a contract violation, never something to truncate.
 */
void
accumulateTensorBundle(std::map<std::string, TensorBundle>& accumulator,
                       const std::string& slot, const TensorBundle& contribution);

/** Validate borrowed payloads and synchronously copy them into owned wire bytes. */
std::vector<std::uint8_t>
encodeTensorBundleViews(const std::vector<NamedTensorView>& tensors,
                        const std::shared_ptr<NativeWorkStatistics>& workStatistics = {});

void
validateNamedTensorView(const NamedTensorView& tensor);

std::vector<NamedTensor>
decodeTensorBundle(const std::vector<std::uint8_t>& payload,
                   const std::shared_ptr<NativeWorkStatistics>& workStatistics = {});

/**
 * Validate the complete bundle without copying tensor payloads.
 * The source buffer must outlive all views and remain unmoved and unmodified.
 * This is codec validation only, not signature or execution admission.
 */
std::vector<NamedTensorView>
decodeTensorBundleViews(const std::vector<std::uint8_t>& payload,
                        const std::shared_ptr<NativeWorkStatistics>& workStatistics = {});

std::vector<NamedTensorView>
decodeTensorBundleViews(std::vector<std::uint8_t>&&) = delete;

std::vector<NamedTensorView>
decodeTensorBundleViews(const std::vector<std::uint8_t>&&) = delete;

const NamedTensor&
findTensor(const std::vector<NamedTensor>& tensors, const std::string& name);

std::vector<NamedTensor>
selectTensors(const std::vector<NamedTensor>& tensors,
              const std::vector<std::string>& names);

std::vector<NamedTensorView>
selectTensorViews(const std::vector<NamedTensorView>& tensors,
                  const std::vector<std::string>& names);

TensorBundle
makeEncodedTensorBundle(std::string name, const std::vector<NamedTensor>& tensors);

/** Borrow only for this call; the returned bundle owns every payload byte. */
TensorBundle
makeEncodedTensorBundleViews(std::string name, const std::vector<NamedTensorView>& tensors);

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
