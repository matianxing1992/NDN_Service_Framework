#ifndef NDNSF_DI_NATIVE_ONNX_RECIPE_ASSEMBLER_HPP
#define NDNSF_DI_NATIVE_ONNX_RECIPE_ASSEMBLER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <map>
#include <vector>

namespace ndnsf::di {

/** Shared ownership for a canonical byte object.
 *
 * The source is copied through several immutable preparation records.  Keeping
 * the vector behind shared ownership makes those copies cheap and, more
 * importantly, lets producer-side material payloads refer to bounded ranges
 * without duplicating a multi-gigabyte external initializer.
 */
class NativeCanonicalByteBuffer
{
public:
  using value_type = std::uint8_t;
  using vector_type = std::vector<value_type>;
  using iterator = vector_type::iterator;
  using const_iterator = vector_type::const_iterator;

  NativeCanonicalByteBuffer() = default;
  explicit NativeCanonicalByteBuffer(std::size_t size)
    : m_bytes(std::make_shared<vector_type>(size))
  {}
  NativeCanonicalByteBuffer(vector_type bytes)
    : m_bytes(std::make_shared<vector_type>(std::move(bytes)))
  {}

  NativeCanonicalByteBuffer& operator=(vector_type bytes)
  {
    m_bytes = std::make_shared<vector_type>(std::move(bytes));
    return *this;
  }

  bool empty() const noexcept { return !m_bytes || m_bytes->empty(); }
  std::size_t size() const noexcept { return m_bytes ? m_bytes->size() : 0; }
  const value_type* data() const noexcept { return m_bytes ? m_bytes->data() : nullptr; }
  value_type* data() noexcept { return m_bytes ? m_bytes->data() : nullptr; }
  const_iterator begin() const noexcept { return m_bytes ? m_bytes->begin() : const_iterator{}; }
  const_iterator end() const noexcept { return m_bytes ? m_bytes->end() : const_iterator{}; }
  iterator begin() noexcept { return m_bytes ? m_bytes->begin() : iterator{}; }
  iterator end() noexcept { return m_bytes ? m_bytes->end() : iterator{}; }
  const value_type& operator[](std::size_t index) const { return (*m_bytes)[index]; }
  value_type& operator[](std::size_t index) { return (*m_bytes)[index]; }

  /** Preserve legacy call sites that consume a const std::vector reference. */
  operator const vector_type&() const
  {
    static const vector_type empty;
    return m_bytes ? *m_bytes : empty;
  }

  const vector_type& asVector() const noexcept { return static_cast<const vector_type&>(*this); }
  vector_type& asVector() noexcept
  {
    if (!m_bytes)
      m_bytes = std::make_shared<vector_type>();
    return *m_bytes;
  }
  operator vector_type&() { return asVector(); }
  vector_type copy() const { return asVector(); }

  /** Return the shared allocation for zero-copy producer-side range views. */
  std::shared_ptr<const vector_type> shared() const noexcept { return m_bytes; }

private:
  std::shared_ptr<vector_type> m_bytes;
};

/** Immutable bounded range reader for producer-side material publication.
 *
 * A preparation source may need the complete initializer while validating the
 * ONNX graph, but publication only needs one bounded material chunk at a time.
 * This interface lets a Repo-backed source release that complete byte vector
 * before the protected publication loop starts.
 */
class NativeCanonicalByteRangeSource
{
public:
  virtual ~NativeCanonicalByteRangeSource() = default;
  virtual std::uint64_t size() const noexcept = 0;
  virtual std::vector<std::uint8_t> read(std::uint64_t offset,
                                         std::uint64_t length) const = 0;
};

// Protected material bundles are deliberately bounded so a post-Selection
// consumer can reserve the complete fetched object before touching the Repo.
// A payload larger than this limit is rejected by the publisher rather than
// silently creating an unbounded bundle.
inline constexpr std::uint64_t NativeCanonicalMaterialBundleMaxBytes = 1U << 20;

// The material-object index is carried by an authenticated receipt rather
// than the inline authority root.  Prepare reserves this conservative bound
// before publishing any object; the generated receipt must fit it as well.
// Payload IDs and protected data names are producer-controlled strings, so a
// fixed per-record allowance is preferable to discovering an overrun after a
// publication prefix has become visible.
inline constexpr std::uint64_t NativeCanonicalMaterialReceiptEnvelopeMaxBytes = 4U << 10;
inline constexpr std::uint64_t NativeCanonicalMaterialReceiptRecordMaxBytes = 4U << 10;
inline constexpr std::uint64_t NativeCanonicalMaterialReceiptPayloadIdMaxBytes = 512;
inline constexpr std::uint64_t NativeCanonicalMaterialReceiptDataNameMaxBytes = 1024;
// Receipt payload IDs and NDN URI names are restricted to printable
// non-escaping tokens before serialization.  With two 71-byte digests, three
// uint64 fields (20 decimal digits each), and this fixed key/punctuation
// allowance, the per-record bound is proven below rather than guessed.
inline constexpr std::uint64_t NativeCanonicalMaterialReceiptFixedRecordOverheadMaxBytes = 512;
static_assert(NativeCanonicalMaterialReceiptPayloadIdMaxBytes +
              NativeCanonicalMaterialReceiptDataNameMaxBytes +
              2U * 71U + 3U * 20U +
              NativeCanonicalMaterialReceiptFixedRecordOverheadMaxBytes <=
              NativeCanonicalMaterialReceiptRecordMaxBytes);

/** Owned canonical ONNX bytes; source authentication belongs to the fetching owner. */
struct NativeCanonicalSource
{
  struct MaterialReference
  {
    std::string payloadId;
    // External initializers and large inline raw initializers use one small
    // TensorProto header plus ordered raw byte chunks. The legacy single-payload
    // form remains valid for small inline initializers and existing manifests.
    std::vector<std::string> chunkPayloadIds;
    std::string kind;
    std::string logicalName;
    std::uint64_t nodeIndex = 0;
    std::string digest;
    std::uint64_t bytes = 0;
    std::vector<std::string> dependencies;
    std::string sharedDigest;
  };

  struct MaterialPayload
  {
    std::string payloadId;
    std::string digest;
    std::vector<std::uint8_t> bytes;

    // A producer may authenticate and publish a bounded view of an immutable
    // source object instead of copying the range into bytes.  Consumer-fetched
    // payloads continue to use bytes, so this is wire/internal compatible.
    std::shared_ptr<const std::vector<std::uint8_t>> backing;
    std::size_t backingOffset = 0;
    std::size_t backingSize = 0;

    // Inline TensorProto raw_data is owned by a string in protobuf. Move that
    // allocation into shared backing before creating chunk views so manifest
    // derivation does not allocate a second full initializer-sized vector.
    std::shared_ptr<const std::string> stringBacking;
    std::size_t stringOffset = 0;
    std::size_t stringSize = 0;

    // A producer may retain only an authenticated bounded range reader after
    // source inspection.  Consumer-fetched payloads remain byte-owned.
    std::shared_ptr<const NativeCanonicalByteRangeSource> rangeSource;
    std::uint64_t rangeOffset = 0;
    std::uint64_t rangeSize = 0;

    bool empty() const noexcept { return byteSize() == 0; }
    std::size_t byteSize() const noexcept
    {
      return rangeSource ? static_cast<std::size_t>(rangeSize) :
        (backing ? backingSize : (stringBacking ? stringSize : bytes.size()));
    }
    const std::uint8_t* data() const noexcept
    {
      if (rangeSource)
        return nullptr;
      if (backing)
        return backing->data() + backingOffset;
      if (stringBacking)
        return reinterpret_cast<const std::uint8_t*>(stringBacking->data()) + stringOffset;
      return bytes.data();
    }
    std::vector<std::uint8_t> copyBytes() const
    {
      if (rangeSource) {
        if (rangeOffset > rangeSource->size() ||
            rangeSize > rangeSource->size() - rangeOffset)
          throw std::out_of_range("native canonical material range is out of bounds");
        auto result = rangeSource->read(rangeOffset, rangeSize);
        if (result.size() != rangeSize)
          throw std::runtime_error("native canonical material range has unexpected size");
        return result;
      }
      if (!backing)
        return stringBacking
          ? std::vector<std::uint8_t>(data(), data() + stringSize) : bytes;
      return std::vector<std::uint8_t>(data(), data() + backingSize);
    }

    /** Scrub only payload-owned plaintext; source owners retain shared views. */
    void scrub() noexcept
    {
      if (rangeSource || backing || stringBacking)
        return;
      std::fill(bytes.begin(), bytes.end(), 0);
    }

    /** Release this payload after its final authenticated consumer. */
    void release() noexcept
    {
      scrub();
      std::vector<std::uint8_t>{}.swap(bytes);
      backing.reset();
      stringBacking.reset();
      rangeSource.reset();
      backingOffset = 0;
      backingSize = 0;
      stringOffset = 0;
      stringSize = 0;
      rangeOffset = 0;
      rangeSize = 0;
    }
  };

  /** Versioned, topology-independent preparation materials.  The graph
   * template contains model metadata and I/O declarations but no nodes or
   * initializers; node and initializer payloads are immutable, addressable
   * objects.  Provider placement is intentionally absent from this schema. */
  struct MaterialManifest
  {
    std::string schema = "ndnsf-di-canonical-material-manifest-v1";
    std::string sourceDigest;
    std::string graphDigest;
    std::string initializerDigest;
    std::string manifestDigest;
    std::string templatePayloadId;
    std::vector<MaterialReference> references;
    std::vector<MaterialPayload> payloads;
    // Producers set this true.  A post-Selection consumer carries the
    // authenticated reference index with only the selected payload bytes;
    // the canonical JSON/digest remains the producer's full index.
    bool payloadsComplete = true;

    std::string canonicalJson() const;
    void validate() const;
  };

  std::vector<std::uint8_t> modelBytes;
  std::optional<NativeCanonicalByteBuffer> initializerBytes;
  std::shared_ptr<const NativeCanonicalByteRangeSource> initializerRangeSource;
  std::shared_ptr<const MaterialManifest> materialManifest;
  // Bytes fetched after Selection.  They are deliberately separate from the
  // producer-owned manifest payload list so a Provider cannot imply that the
  // complete model was downloaded.
  std::vector<MaterialPayload> materialPayloads;

  // A post-Selection consumer may carry a role model rebuilt from the
  // authenticated material index.  The indices remain the canonical source
  // indices; the serialized model contains only the selected nodes.  These
  // fields are internal assembly provenance and are never accepted from an
  // application-facing request.
  bool materializedRole = false;
  std::vector<std::uint64_t> materializedNodeIndices;
  std::string materializedSourceDigest;
  std::string materializedGraphDigest;
  std::string materializedInitializerDigest;

  /**
   * Immutable placement packages produced by the preparation boundary.  The
   * bytes are owned only until the publication receipt is committed; request
   * execution must use the Repo names recorded in that receipt.
   */
  struct LayerPayload
  {
    std::uint64_t stageIndex = 0;
    std::uint64_t layerBegin = 0;
    std::uint64_t layerEnd = 0;
    std::string digest;
    std::vector<std::uint8_t> bytes;
  };
  std::vector<LayerPayload> layerPayloads;
  // Versioned reference-only preparation metadata. It contains bounded graph
  // inspection facts, never canonical model or initializer bytes.
  std::string preparedMetadataJson;
};

/**
 * Borrowed fd for a worker-side materialized role source.  The owner keeps the
 * descriptor open for the duration of assembleInProcess; the parser validates
 * the regular-file size and SHA-256 before consuming it and never closes it.
 */
struct NativeOnnxModelFileInput
{
  int fd = -1;
  std::uint64_t bytes = 0;
  std::string digest;
};

using NativeCertifiedRecipe = NativeSelectionRoleV3;

/** Request-owned limits and cancellation callback for native assembly. */
struct NativeAssemblyControl
{
  std::chrono::steady_clock::time_point deadline;
  /** Required cancellation/owner fence, invoked before and during native work. */
  std::function<void()> requireActive;
  std::uint64_t maxSourceBytes = 0;
  std::uint64_t maxAssembledBytes = 0;
};

struct NativeCertifiedAssembly
{
  std::vector<std::uint8_t> modelBytes;
  std::vector<std::string> inputNames;
  std::vector<std::string> outputNames;
  std::uint64_t nodeCount = 0;
  std::string modelDigest;
};

/**
 * OA01 (legacy, suite-only entry): assemble one authenticated ONNX role
 * in-process.  Retained for the frozen native-assembly parity suites;
 * production post-Selection activation runs the same chain through the
 * OA02 worker subprocess (runNativeOnnxAssemblyWorkerAt), never through
 * this in-process helper.
 */
NativeCertifiedAssembly
assembleNativeCertifiedOnnxModel(const NativeCanonicalSource& source,
                                 const NativeCertifiedRecipe& recipe,
                                 const NativeAssemblyControl& control);

/** Canonical content of one ONNX initializer per the normalization rules. */
struct NormalizedInitializerPayload
{
  std::string dtype;                   // numpy-1.24 label (structured for
                                       // BFLOAT16/FLOAT8/INT4/UINT4)
  std::vector<std::int64_t> shape;     // TensorProto.dims 原序
  std::string byteOrder;               // "little" when itemsize > 1, else "na"
  std::vector<std::uint8_t> content;   // canonical bytes (request-scoped owner)
};

/** Graph and normalized-initializer identity of an un-shape-inferred model. */
struct NativeOnnxIdentity
{
  std::string graphDigest;
  std::string initializerDigest;
};

/** Source-derived planning facts, distinct from the original assembly identity. */
struct NativeOnnxGraphInspection
{
  NativeGraphSnapshot graph;
  std::vector<std::string> nodeNames;
  // ONNX planning nodes have a one-to-one original source index. Semantic
  // layer adapters must provide their own many-node mapping rather than reuse it.
  std::map<std::string, std::uint64_t> canonicalNodeIndices;
  std::string graphMetadataJson;
  NativeOnnxIdentity canonicalIdentity;
};

/** Inspect owned ONNX bytes using official shape inference and the maintained
 * adapter-bound planning schema. Deep attribute tensors are materialized into
 * the graph copy; large top-level external initializers remain range-bound and
 * only shape-value inputs needed by inference are materialized under the
 * assembly budget. Reject a different expected graph identity; source
 * fetching/authentication belongs to the caller's native catalog owner.
 */
NativeOnnxGraphInspection
inspectNativeOnnxPlanningGraph(const NativeCanonicalSource& source,
  const NativeModelDescriptor& expectedModel, const NativeAssemblyControl& control);

/** Derive the actual ONNX graph independently of a semantic adapter graph.
 * The returned graph has its own computed identity. Callers must bind the
 * source bytes/canonical identity and explicitly map semantic nodes to it. */
NativeOnnxGraphInspection
inspectNativeOnnxSourceGraph(const NativeCanonicalSource& source,
  const NativeModelDescriptor& model, const NativeAssemblyControl& control);

/**
 * Normalize one serialized ONNX TensorProto into its canonical payload, or
 * throw DI_ONNX_INITIALIZER_ENCODING_INVALID when the shape, field, or
 * encoding violates the normalization rules.  The seam takes serialized
 * bytes so no ONNX/protobuf type is installed in this public header; the
 * implementation stays private in the module TU.
 */
NormalizedInitializerPayload
normalizedOnnxInitializerPayload(const std::vector<std::uint8_t>& serializedTensorProto);

/**
 * Owned-source canonical identity of one source model (OA05 + OA06 seam):
 * parse the given bytes, validate external metadata strictly from the passed
 * memory (never from paths declared in the model), materialize deep attribute
 * tensors and hash top-level ranges one at a time, then compute the graph
 * digest and ordered normalized-initializer digest on the original, not
 * shape-inferred, graph.  The model is not full-checked here;
 * checker and extractor runs arrive with the certified-extraction cards.
 */
NativeOnnxIdentity
canonicalOnnxSourceIdentity(const NativeCanonicalSource& source,
                            const NativeAssemblyControl& control);

/**
 * Derive the prepare-time material manifest from the authenticated canonical
 * ONNX source.  This is a producer operation: it does not choose Provider
 * ranges and it emits no final partition model.  The caller owns the returned
 * bytes until the protected publisher commits every referenced object.
 */
std::shared_ptr<const NativeCanonicalSource::MaterialManifest>
deriveNativeCanonicalMaterialManifest(const NativeCanonicalSource& source,
                                      const NativeAssemblyControl& control);

/** Parse an authenticated producer manifest without materializing payloads. */
std::shared_ptr<NativeCanonicalSource::MaterialManifest>
parseNativeCanonicalMaterialManifest(const std::vector<std::uint8_t>& bytes);

/** Rebuild one selected role model from the template, selected nodes and
 * explicit shared initializer payloads.  No complete source/initializer is
 * required by this operation.  When boundary contracts are supplied, the
 * materialized graph input/output declarations are rebuilt in that exact
 * order.  This is the native equivalent of the old stage exporter: an
 * internal handoff tensor such as `hidden_states_out` is a stage output even
 * though it is not a canonical source graph output. */
std::vector<std::uint8_t>
materializeNativeCanonicalModel(NativeCanonicalSource& source,
                                const std::vector<std::uint64_t>& nodeIndices,
                                const std::vector<NativeAssemblyTensorContractV3>& expectedInputs,
                                const std::vector<NativeAssemblyTensorContractV3>& expectedOutputs,
                                const NativeAssemblyControl& control);

/** Compatibility entry for producer/fixture callers that retain the source
 * graph boundary.  Provider post-Selection assembly must use the overload
 * above with the authenticated role contracts. */
std::vector<std::uint8_t>
materializeNativeCanonicalModel(NativeCanonicalSource& source,
                                const std::vector<std::uint64_t>& nodeIndices,
                                const NativeAssemblyControl& control);

/** Validate a prepared material manifest against the authenticated ONNX source. */
void validateNativeCanonicalMaterialManifest(
  const NativeCanonicalSource& source,
  const NativeCanonicalSource::MaterialManifest& manifest,
  const NativeAssemblyControl& control);

/** Validate only a durable reference index restored before payload fetch. */
void validateNativeCanonicalMaterialReferenceIndex(
  const NativeCanonicalSource& source,
  const NativeCanonicalSource::MaterialManifest& manifest,
  const NativeAssemblyControl& control);

/**
 * Initializer-normalization revision (1 or 2) of a source model after
 * external inlining, classified from top-level graph initializers only:
 * STRING, BFLOAT16 raw (incl. external, which inlines to raw), and COMPLEX
 * typed select revision 2; everything else stays 1.
 */
std::uint32_t
onnxInitializerNormalizationRevision(const NativeCanonicalSource& source,
                                     const NativeAssemblyControl& control);

/**
 * Descriptor-revision binding gate: a model whose inlined initializers need
 * normalization revision 2 must be bound to a recipe that declares the exact
 * v2 assembler descriptor digest; otherwise DI_ONNX_NORMALIZATION_REVISION_
 * REQUIRED.  Revision-1 sources keep the legacy descriptor rules (no extra
 * rejection of existing legal adapter descriptors).
 */
void
checkOnnxAssemblerDescriptorBinding(const std::string& assemblerDescriptorDigest,
                                    const NativeCanonicalSource& source,
                                    const NativeAssemblyControl& control);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_ONNX_RECIPE_ASSEMBLER_HPP
