// DI-private bounded native assembly worker protocol (T006-C).
//
// This header is deliberately NOT installed as an application header: it
// declares the anonymous-pipe framing, the parent transport (OA02), the
// worker child entry (OA03), and the in-process certified chain entry (OA04)
// used by the worker child and by the Spec182OnnxWorkerProtocol suite.  Only
// owned value types cross these declarations; no ONNX/protobuf class and no
// Core wire type is visible here.
//
// Protocol summary (native-onnx-assembly-design.md, Native Worker Framing
// and Lifetime): one local request frame and one response frame over
// anonymous pipes; stdout carries protocol bytes only; stderr is bounded.
//   request : 8B "NDI182A1" | u64le metadataLength | u64le modelLength
//             | u64le initializerLength | u8 flags | metadata
//             | model bytes | initializer bytes
//             flags bit 0 is hasInitializer; bit 1 is hasModelFile.  A file
//             request declares modelLength but carries no model bytes: the
//             parent passes the already-staged read-only model as fd 3.
//   response: 8B "NDI182R1" | u32le metadataLength | u64le modelLength
//             | u8 status (0 ok / 1 algorithm reject / 2 input error /
//             | 3 file-backed digest-only success)
//             | metadata | model bytes (status 0 only; status 3 reuses the
//             | already-staged file after the child exits)
// The parent closes its write end after one frame; the child drains the
// remaining input to EOF and rejects any trailing/second-frame byte.  The
// worker is not a network service and never receives credentials.

#ifndef NDNSF_DI_NATIVE_ONNX_ASSEMBLY_WORKER_HPP
#define NDNSF_DI_NATIVE_ONNX_ASSEMBLY_WORKER_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di {

inline constexpr char kNdnSf182RequestMagic[8] = {'N', 'D', 'I', '1', '8', '2', 'A', '1'};
inline constexpr char kNdnSf182ResponseMagic[8] = {'N', 'D', 'I', '1', '8', '2', 'R', '1'};

// Response metadata cap preserved from the old helper (MaxHelperMetadataBytes).
inline constexpr std::size_t kNativeOnnxWorkerMaxMetadataBytes = 65536;

// Result JSON schemas written by the worker child.
inline constexpr const char* kNativeOnnxAssemblyResultSchema =
  "ndnsf-di-native-assembly-result-v1";
inline constexpr const char* kNativeOnnxAssemblyErrorSchema =
  "ndnsf-di-native-assembly-error-v1";
inline constexpr const char* kNativeOnnxAssemblyRequestSchema =
  "ndnsf-di-native-assembly-request-v1";
inline constexpr std::uint8_t kNativeOnnxWorkerDigestOnlyStatus = 3;

// Certified assembly recipe JSON schema (CertifiedOnnxAssemblyRecipe.to_dict,
// camelCase keys; canonical digest definition in the S1 contract).
inline constexpr const char* kNativeOnnxCertifiedRecipeSchema =
  "ndnsf-di-certified-onnx-assembly-v1";

/** Parsed request frame header (all lengths little-endian on the wire). */
struct NativeOnnxRequestHeader
{
  std::uint64_t metadataLength = 0;
  std::uint64_t modelLength = 0;
  std::uint64_t initializerLength = 0;
  bool hasInitializer = false;
  bool hasModelFile = false;
};

/** Parsed response frame header. */
struct NativeOnnxResponseHeader
{
  std::uint32_t metadataLength = 0;
  std::uint64_t modelLength = 0;
  std::uint8_t status = 0;  // 0 ok / 1 algorithm reject / 2 input error /
                            // 3 file-backed digest-only success
};

/**
 * Incremental request-frame decoder shared by the parent write side and the
 * worker child read side.  Pure state; no allocation beyond the declared
 * payload lengths after the header has been validated.
 */
class NativeOnnxRequestDecoder
{
public:
  enum class Result { NeedMore, Complete, ProtocolError };
  enum class Phase
  {
    Header,  // 8B magic | u64le x3 | u8 flag, accumulated to 33 bytes
    Payload, // metadata, model, initializer segments in fixed order
    Trailing,
    Done,
  };

  // Feed bytes; returns the decoding outcome.  Any byte after the frame is
  // complete (a second/duplicate frame) yields ProtocolError.
  Result feed(const std::uint8_t* data, std::size_t size);

  Result result() const { return m_result; }
  bool complete() const { return m_result == Result::Complete; }
  const NativeOnnxRequestHeader& header() const { return m_header; }
  const std::vector<std::uint8_t>& metadata() const { return m_metadata; }
  const std::vector<std::uint8_t>& model() const { return m_model; }
  const std::vector<std::uint8_t>& initializer() const { return m_initializer; }

private:
  Result m_result = Result::NeedMore;
  Phase m_phase = Phase::Header;
  std::size_t m_headerBytes = 0;
  std::array<std::uint8_t, 33> m_headerData{};
  NativeOnnxRequestHeader m_header;
  std::uint64_t m_metadataRemaining = 0;
  std::uint64_t m_modelRemaining = 0;
  std::uint64_t m_initializerRemaining = 0;
  bool m_metadataDone = false;
  bool m_modelDone = false;
  bool m_initializerDone = false;
  std::vector<std::uint8_t> m_metadata;
  std::vector<std::uint8_t> m_model;
  std::vector<std::uint8_t> m_initializer;
};

/**
 * Incremental response-frame decoder used by the parent.  Bounds the metadata
 * at kNativeOnnxWorkerMaxMetadataBytes and rejects bytes beyond the frame.
 */
class NativeOnnxResponseDecoder
{
public:
  enum class Result { NeedMore, Complete, ProtocolError };
  enum class Phase
  {
    Header,  // 8B magic | u32le metadata | u64le model | u8 status
    Payload,
    Trailing,
    Done,
  };

  Result feed(const std::uint8_t* data, std::size_t size);

  Result result() const { return m_result; }
  bool complete() const { return m_result == Result::Complete; }
  const NativeOnnxResponseHeader& header() const { return m_header; }
  const std::vector<std::uint8_t>& metadata() const { return m_metadata; }
  const std::vector<std::uint8_t>& model() const { return m_model; }
  std::vector<std::uint8_t> takeModel() { return std::move(m_model); }

private:
  Result m_result = Result::NeedMore;
  Phase m_phase = Phase::Header;
  std::size_t m_headerBytes = 0;
  std::array<std::uint8_t, 21> m_headerData{};
  NativeOnnxResponseHeader m_header;
  std::uint64_t m_metadataRemaining = 0;
  std::uint64_t m_modelRemaining = 0;
  bool m_metadataDone = false;
  bool m_modelDone = false;
  std::vector<std::uint8_t> m_metadata;
  std::vector<std::uint8_t> m_model;
};

/**
 * Canonical certified recipe JSON (camelCase keys, sorted keys, compact UTF-8)
 * over exactly the CertifiedOnnxAssemblyRecipe.to_dict key set.  The
 * certified recipeDigest is sha256 of these bytes with the "sha256:" prefix.
 */
std::string
canonicalNativeOnnxRecipeJson(const NativeCertifiedRecipe& recipe);

/** Typed metadata of one worker request after child-side S1 revalidation. */
struct NativeOnnxWorkerMetadata
{
  NativeCertifiedRecipe recipe;  // certified slice incl. adapter/backend identity
  std::string recipeDigest;      // certified digest over the canonical recipe JSON
  std::string schema;            // kNativeOnnxAssemblyRequestSchema
  std::optional<std::uint64_t> sourceBytes;
  std::string sourceDigest;
};

/** Outcome of child-side S1 metadata validation. */
struct NativeOnnxMetadataCheck
{
  bool ok = false;
  std::string failureCode;   // "DI_NATIVE_ONNX_*" family when !ok
  std::string failureMessage;
  NativeOnnxWorkerMetadata value;
};

/**
 * Child-side S1: parse the request metadata JSON, enforce the certified
 * recipe format rules (schema, sha256 digests, non-empty identity, layer
 * interval, strictly increasing unique node cover, exact io-name cover of
 * expected contracts, positive resource bounds) and revalidate
 * recipeDigest == sha256(canonical recipe JSON).  Pure; no graph knowledge.
 */
NativeOnnxMetadataCheck
validateNativeOnnxWorkerMetadata(const std::string& json);

/**
 * Serialize the worker-request metadata envelope: the certified recipe JSON
 * (canonical bytes) embedded as "recipe", its sha256 in "recipeDigest", the
 * non-certified identity binding "backend"/"adapterId", and the envelope
 * schema.  Callers must already have checked that a non-empty provided
 * recipeDigest equals the derived digest (the signed recipeDigest and the
 * canonical payload are one certificate).
 */
std::string
buildNativeOnnxWorkerRequestMetadata(
  const NativeCertifiedRecipe& recipe,
  std::optional<std::uint64_t> sourceBytes = std::nullopt,
  const std::string& sourceDigest = {});

/**
 * Compose one request frame (magic, little-endian lengths, hasInitializer
 * byte, metadata, model, initializer).  Frame coherence (hasInitializer == 0
 * requires an empty initializer) is enforced.
 */
std::vector<std::uint8_t>
composeNativeOnnxWorkerRequest(const std::string& metadata,
                               const std::vector<std::uint8_t>& model,
                               const std::vector<std::uint8_t>& initializer,
                               bool hasInitializer);

/**
 * Compose one response frame.  status 0 may carry a model; status 1/2 must
 * not (modelLength is forced to zero by the caller contract).
 */
std::vector<std::uint8_t>
composeNativeOnnxWorkerResponse(std::uint8_t status,
                                const std::string& metadata,
                                const std::vector<std::uint8_t>& model);

/**
 * Parent-side result revalidation (a worker PASS claim never bypasses it):
 * the child must have exited zero, the frame status semantics must hold, the
 * result metadata must parse under the result schema, sha256 of the model
 * bytes must equal the declared modelDigest, the model must respect both the
 * recipe and the request budget, and node/io counts must match the certified
 * recipe.  activeAfterResponse is the outcome of the mandatory final
 * requireActive call: a late success arriving after cancellation must not be
 * published.  Pure; no worker state involved.
 */
struct NativeOnnxWorkerOutcome
{
  bool ok = false;
  std::string failureCode;    // full "DI_NATIVE_ONNX_*" text when !ok
  std::string failureMessage;
  NativeCertifiedAssembly value;
};

NativeOnnxWorkerOutcome
finalizeNativeOnnxWorkerResponse(bool childExitZero,
                                 std::uint8_t status,
                                 const std::string& metadataJson,
                                 std::vector<std::uint8_t> modelBytes,
                                 const NativeCertifiedRecipe& recipe,
                                 std::uint64_t maxAssembledBytes,
                                 bool activeAfterResponse);

/**
 * OA04 (implemented in NativeOnnxRecipeAssembler.cpp): run the shared
 * S1-S7 certified chain in-process with the certified recipe budget and no
 * cancellation callback.  The bounded worker child calls this; it starts no
 * second worker, touches no Core/Repo state, and writes no cache or manifest.
 * When sourceToReleaseAfterParse is non-null, OA04 scrubs and releases the
 * supplied child-owned request source after parsing/inlining completes; the
 * default remains non-destructive for focused in-process callers.
 */
NativeCertifiedAssembly
assembleInProcess(const NativeCanonicalSource& source,
                  const NativeCertifiedRecipe& recipe,
                  NativeCanonicalSource* sourceToReleaseAfterParse = nullptr,
                  const NativeOnnxModelFileInput* modelFile = nullptr);

/** Fixed location of the installed native worker binary. */
struct NativeOnnxWorkerLocation
{
  std::string path;    // absolute path; no PATH search or fallback allowed
  std::string sha256;  // "sha256:" + 64 hex of the binary, preflight-checked
};

/**
 * Register the process-global fixed worker location (test identities must be
 * registered before any spawn; production provider registers its installed
 * binary once at startup).  Preflight failure (unreadable/empty path or an
 * identity mismatch) throws DI_NATIVE_ONNX_WORKER_PREFLIGHT.
 */
void
registerNativeOnnxWorkerLocation(const NativeOnnxWorkerLocation& location);

/**
 * OA02 (default location): spawn the fixed installed worker with a fixed
 * argv and a minimal sanitized environment, own process group, only
 * stdin/stdout/stderr mapped, nonblocking pipes polled in both directions,
 * steady deadline and requireActive on every round; cancel first TERM then
 * KILL after 1s then waitpid.  Any late success must pass requireActive
 * again before the result is published; a worker's own PASS claim never
 * bypasses the parent length/hash/authorization revalidation.
 */
NativeCertifiedAssembly
runNativeOnnxAssemblyWorker(const NativeCanonicalSource& source,
                            const NativeCertifiedRecipe& recipe,
                            const NativeAssemblyControl& control);

/** OA02 over an explicitly located worker (same transport semantics). */
NativeCertifiedAssembly
runNativeOnnxAssemblyWorkerAt(const NativeOnnxWorkerLocation& location,
                              const NativeCanonicalSource& source,
                              const NativeCertifiedRecipe& recipe,
                              const NativeAssemblyControl& control,
                              NativeCanonicalSource* sourceToReleaseAfterWrite = nullptr,
                              const std::filesystem::path& modelFile = {});

/**
 * OA03 worker child entry: accepts only the fixed "--stdio-v1
 * --metadata-bytes <decimal>" mode, closes every non-stdio descriptor first,
 * parses exactly one request frame whose metadataLength equals the argv
 * length, drains the remaining input to EOF (trailing bytes rejected), runs
 * OA04, and writes exactly one response frame.  Exit: 0 ok / 1 algorithm
 * reject / 2 protocol input error.  stdout carries protocol bytes only;
 * stderr carries bounded diagnostics without source or secret content.
 */
int
runNativeOnnxAssemblyWorkerMain(int argc, char** argv);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_ONNX_ASSEMBLY_WORKER_HPP
