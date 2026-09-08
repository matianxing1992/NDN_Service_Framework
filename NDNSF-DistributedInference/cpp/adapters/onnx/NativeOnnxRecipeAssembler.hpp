#ifndef NDNSF_DI_NATIVE_ONNX_RECIPE_ASSEMBLER_HPP
#define NDNSF_DI_NATIVE_ONNX_RECIPE_ASSEMBLER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Owned canonical ONNX bytes; source authentication belongs to the fetching owner. */
struct NativeCanonicalSource
{
  std::vector<std::uint8_t> modelBytes;
  std::optional<std::vector<std::uint8_t>> initializerBytes;
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
 * adapter-bound planning schema. Reject a different expected graph identity;
 * source fetching/authentication belongs to the caller's native catalog owner.
 */
NativeOnnxGraphInspection
inspectNativeOnnxPlanningGraph(const NativeCanonicalSource& source,
  const NativeModelDescriptor& expectedModel, const NativeAssemblyControl& control);

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
 * parse the given bytes, validate and inline external tensors strictly from
 * the passed memory (never from paths declared in the model), then compute
 * the graph digest and the ordered normalized-initializer digest on the
 * original, not shape-inferred, graph.  The model is not full-checked here;
 * checker and extractor runs arrive with the certified-extraction cards.
 */
NativeOnnxIdentity
canonicalOnnxSourceIdentity(const NativeCanonicalSource& source,
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
