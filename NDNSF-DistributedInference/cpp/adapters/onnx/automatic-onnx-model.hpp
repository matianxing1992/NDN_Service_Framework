#ifndef NDNSF_DI_AUTOMATIC_ONNX_MODEL_HPP
#define NDNSF_DI_AUTOMATIC_ONNX_MODEL_HPP

#include "NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelTypes.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-automatic-shape-seed.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Original physical file identity, independent of normalized tensor identity. */
struct AutomaticOnnxSourceFile
{
  std::string location;
  std::uint64_t bytes = 0;
  std::string digest;
};

/** Hash a validated physical inventory; this performs no file reads or admission. */
std::string
automaticOnnxSourceDigest(const std::string& modelDigest,
                          const std::vector<AutomaticOnnxSourceFile>& files,
                          const NativeAssemblyControl& control);

/** Preparation-owned source. This does not imply semantic or execution readiness. */
struct OpenedAutomaticOnnxSource
{
  NativeCanonicalSource source;
  std::string modelDigest;
  std::string sourceDigest;
  std::vector<AutomaticOnnxSourceFile> externalFiles;
};

/** Open only regular files beneath the model directory, retaining verified fds.
 * No callback is retained. The caller must use the same preparation control for
 * subsequent inspection/publication; reads additionally reject file mutation.
 */
OpenedAutomaticOnnxSource
openAutomaticOnnxSource(const std::filesystem::path& modelFile,
                        const NativeAssemblyControl& control);

/** Source-scoped operator facts; names are graph value IDs, never Producer names. */
struct AutomaticOnnxOperatorFact
{
  std::string scope;
  std::uint64_t nodeIndex = 0;
  std::string domain;
  std::string opType;
  std::string overload;
  std::string functionScope;
  std::int64_t opset = 0;
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  std::map<std::string, std::int64_t> integerAttributes;
  std::map<std::string, std::vector<std::int64_t>> integerListAttributes;
  std::map<std::string, std::string> attributeReferences;
  bool atomicBody = false;
};

/** Constant input identity; packed storage remains distinct from scalar wire types. */
struct AutomaticOnnxConstantFact
{
  std::uint64_t inputIndex = 0;
  std::string valueName;
  std::int32_t onnxElementType = 0;
  std::vector<std::int64_t> shape;
  std::uint64_t storageBytes = 0;
  std::string normalizedContentDigest;
};

struct QuantizedOperatorFact
{
  std::uint64_t operatorIndex = 0;
  std::optional<std::int64_t> bits;
  std::optional<std::int64_t> axis;
  std::optional<std::int64_t> blockSize;
  std::optional<std::int64_t> implicitZeroPoint;
  std::vector<AutomaticOnnxConstantFact> constantInputs;
  // Only the statically available packing/parameter checks, never kernel readiness.
  bool staticParametersValidated = false;
};

/** A graph axis. Unknown dimensions never imply a unit or zero extent. */
struct DimensionConstraint
{
  enum class Kind { FIXED, SYMBOL, UNKNOWN };
  Kind kind = Kind::UNKNOWN;
  std::int64_t fixed = 0;
  std::string symbol;
  std::optional<std::uint64_t> upperBound;
};

/** Original tensor type with an optional transport representation and rank. */
struct AutomaticTensorContract
{
  std::string name;
  std::int32_t onnxElementType = 0;
  std::optional<TensorElementType> wireElementType;
  std::optional<std::vector<DimensionConstraint>> dimensions;
  std::vector<std::string> provenance;
};

struct TensorAxisReference
{
  std::string tensor;
  std::uint64_t axis = 0;
};

/** Finite, source-proven relations; PRODUCT compares the products on both sides. */
struct ShapeRelation
{
  enum class Kind { EQUAL, SUM, PRODUCT, BROADCAST, AT_MOST };
  Kind kind = Kind::EQUAL;
  std::vector<TensorAxisReference> outputs;
  std::vector<TensorAxisReference> inputs;
  std::uint64_t constant = 0;
  std::string provenance;
};

/** Formal attention evidence; capacity is max(past capacity, total length).
 * A complete generation proof must additionally establish total=P+S, an empty
 * initial cache, and the source's token/mask/position relationships.
 */
struct AutomaticAttentionState
{
  enum class Role { KEY, VALUE };
  Role role = Role::KEY;
  std::uint64_t operatorIndex = 0;
  std::string input;
  std::string output;
  std::string query;
  std::string key;
  std::string value;
  std::string sequenceLengths;
  std::string totalLength;
  TensorElementType elementType = TensorElementType::Float32;
  std::array<std::uint64_t, 4> inputAxes{};
  std::array<std::uint64_t, 4> outputAxes{};
  std::uint64_t heads = 0;
  std::uint64_t headSize = 0;
};

/** Source-proven feedback under empty-past, full replacement, all-ones mask
 * and contiguous position policy. Request token/EOS/sampling policy and runtime
 * readiness remain with their existing owners, independently of this proof.
 */
struct AutomaticGenerationContract
{
  std::string tokenInput;
  std::uint64_t tokenBatchAxis = 0;
  std::uint64_t tokenSequenceAxis = 1;
  std::string logitsOutput;
  std::uint64_t logitsBatchAxis = 0;
  std::uint64_t logitsSequenceAxis = 1;
  std::uint64_t logitsVocabularyAxis = 2;
  std::uint64_t vocabularySize = 0;
  std::vector<AutomaticAttentionState> states;
  std::string maskInput;
  std::uint64_t maskBatchAxis = 0;
  std::uint64_t maskContextAxis = 1;
  std::string positionInput;
  std::uint64_t positionBatchAxis = 0;
  std::uint64_t positionSequenceAxis = 1;
  std::uint64_t contextLimit = 0;
  std::string ruleId;
  std::uint64_t ruleVersion = 1;
};

/** Portable immutable facts. No file handles, request state or readiness proof. */
struct AutomaticOnnxContract
{
  std::string sourceDigest;
  std::vector<AutomaticOnnxOperatorFact> operators;
  std::vector<QuantizedOperatorFact> quantization;
  std::string factsDigest;
  std::vector<AutomaticTensorContract> tensors;
  std::vector<std::string> modelInputs;
  std::vector<std::string> modelOutputs;
  std::vector<ShapeRelation> relations;
  /** Source-owned small INT32/INT64 constants; absent values are not zero. */
  std::map<std::string, std::vector<std::int64_t>> integerLiterals;
  std::optional<AutomaticGenerationContract> generation;

  /** Preserve the existing ndnsf-di-automatic-facts-v1 canonical identity. */
  std::string factsJson() const;
  /** Validate only immutable source facts, without trusting a generation proof. */
  void validateSourceFacts(const NativeAssemblyControl& control) const;
  void validate(const NativeAssemblyControl& control) const;
  std::string canonicalJson(const NativeAssemblyControl& control) const;
  std::string digest(const NativeAssemblyControl& control) const;
  static AutomaticOnnxContract fromCanonicalJson(const std::string& encoded,
                                                const NativeAssemblyControl& control);
};

/** Source-only analysis; publication and stage readiness belong to later owners. */
struct AutomaticOnnxModel
{
  OpenedAutomaticOnnxSource opened;
  NativeModelDescriptor descriptor;
  NativeOnnxGraphInspection inspection;
  AutomaticOnnxContract contract;
  std::string generationDiagnostic;
};

/** Derive descriptor and source facts without a catalog, exporter name or model config. */
AutomaticOnnxModel
inspectAutomaticOnnxModel(const OnnxModelInput& input, const NativeAssemblyControl& control);

/** Derive only finite supported graph relations, retaining opaque regions as atomic. */
std::vector<ShapeRelation>
deriveAutomaticOnnxShapeRelations(const AutomaticOnnxContract& contract,
                                  const NativeAssemblyControl& control);

struct AutomaticTensorBounds
{
  std::optional<std::vector<std::uint64_t>> minimumDimensions;
  std::optional<std::vector<std::optional<std::uint64_t>>> maximumDimensions;
  std::optional<std::uint64_t> maxBytes;
  std::optional<std::uint64_t> maxEncodedBytes;
};

/** Request-specific bounds; no model weights or input payloads are retained. */
struct ExecutionShapeEnvelope
{
  std::string contractDigest;
  std::string bindingDigest;
  std::string budgetDigest;
  std::string envelopeDigest;
  std::uint64_t maxContext = 0;
  std::uint64_t maxNewTokens = 0;
  std::map<std::string, std::vector<std::int64_t>> inputShapes;
  std::map<std::string, AutomaticTensorBounds> tensors;
};

/** Copy the portable request metadata; this is not a validation or trust proof. */
NativeAutomaticShapeSeed
automaticOnnxShapeSeed(const ExecutionShapeEnvelope& envelope);

/** Reconstruct and compare all identities without reading or allocating payloads. */
ExecutionShapeEnvelope
rebindAutomaticOnnxShapeSeed(const AutomaticOnnxContract& contract,
                             const NativeAutomaticShapeSeed& seed,
                             const NativeAssemblyControl& control);

ExecutionShapeEnvelope
bindAutomaticOnnxShapes(const AutomaticOnnxContract& contract,
                        const std::vector<NamedTensor>& inputs,
                        std::uint64_t maxNewTokens, std::uint64_t maxContext,
                        const NativeAssemblyControl& control);

/** Reconstruct root input shapes for an owned round; retains no execution state. */
std::map<std::string, std::vector<std::int64_t>>
automaticOnnxRoundInputShapes(const AutomaticOnnxContract& contract,
                              const ExecutionShapeEnvelope& envelope,
                              std::uint64_t roundIndex,
                              const NativeAssemblyControl& control);

/** Validate the exact expected stage ports against the sealed request bounds. */
void
validateAutomaticOnnxTensors(const AutomaticOnnxContract& contract,
                             const ExecutionShapeEnvelope& envelope,
                             const std::vector<std::string>& expectedScopedNames,
                             const std::vector<NamedTensor>& tensors,
                             const NativeAssemblyControl& control);

/** The admitted request owner supplies its authenticated round, not payload metadata. */
void
validateAutomaticOnnxTensors(const AutomaticOnnxContract& contract,
                             const ExecutionShapeEnvelope& envelope,
                             const std::vector<std::string>& expectedScopedNames,
                             const std::vector<NamedTensor>& tensors,
                             std::uint64_t roundIndex,
                             const NativeAssemblyControl& control);

} // namespace ndnsf::di

#endif // NDNSF_DI_AUTOMATIC_ONNX_MODEL_HPP
