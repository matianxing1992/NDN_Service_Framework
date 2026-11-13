#ifndef NDNSF_DI_QWEN_TENSOR_SPLIT_HPP
#define NDNSF_DI_QWEN_TENSOR_SPLIT_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <onnx/onnx_pb.h>

#include <cstddef>
#include <functional>
#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf::di::qwen {

/** Block width of the frozen weight-only INT4 export (`MatMulNBits`). */
inline constexpr std::uint64_t QWEN_INT4_BITS = 4;
inline constexpr std::uint64_t QWEN_INT4_BLOCK_SIZE = 32;
/** Nibbles packed per byte, i.e. the innermost `MatMulNBits` weight stride. */
inline constexpr std::uint64_t QWEN_INT4_BLOB_BYTES =
  QWEN_INT4_BLOCK_SIZE * QWEN_INT4_BITS / 8;

/** Shape facts a tensor-parallel plan needs. These are read from the frozen
 * export; nothing here may be guessed from a model name. */
/** Layout digest of every activation this template moves: contiguous row-major
 * float32 over the trailing width. One constant is correct because every slot
 * shares that layout; a slot with a different physical layout would need its
 * own value.
 */
inline constexpr const char* QWEN_TP_ACTIVATION_LAYOUT_DIGEST =
  "sha256:6f2f1e2b9a0d4c3e8b7a5f6d1c2e3b4a5f60718293a4b5c6d7e8f90a1b2c3d4e";

/** Layout digest of the integer side inputs: token ids, position ids and the
 * attention mask. They are a different physical layout from the activations,
 * which is why they must not reuse the activation digest.
 */
inline constexpr const char* QWEN_TP_INDEX_LAYOUT_DIGEST =
  "sha256:2b8e4d1f7c3a5960a4d2e8b1c6f7093a5e7d2b4c8f1a3e6d9b0c2f5a7e1d3b6c";

struct QwenTpShape
{
  std::uint64_t hidden = 0;
  std::uint64_t layers = 0;
  std::uint64_t queryHeads = 0;
  std::uint64_t kvHeads = 0;
  std::uint64_t headDim = 0;
  std::uint64_t ffn = 0;
  /** Vocabulary width of the head output. The frozen export ties it to the
   * embedding table (151936 rows for Qwen3-0.6B), so it is a verified fact and
   * not a guess from the hidden width.
   */
  std::uint64_t vocab = 0;

  /** Reject a shape that the TP arithmetic cannot split exactly. */
  void validate() const;
  /** Query projection output width: `queryHeads * headDim`. */
  std::uint64_t queryWidth() const { return queryHeads * headDim; }
  /** Key/value projection output width: `kvHeads * headDim`. */
  std::uint64_t kvWidth() const { return kvHeads * headDim; }
};

/** Which tensor dimension a slice removes. */
enum class QwenSliceAxis
{
  /** Split the output channels (the `N` axis of `MatMulNBits`). */
  OutputChannels,
  /** Split the reduction channels (the `K` axis of `MatMulNBits`). */
  InputChannels,
};

/** One initializer range a rank must own. */
struct QwenInitializerSlice
{
  std::string name;
  QwenSliceAxis axis = QwenSliceAxis::OutputChannels;
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  /** `K` after slicing; only meaningful for InputChannels slices. */
  std::uint64_t slicedK = 0;
  /** `N` after slicing; only meaningful for OutputChannels slices. */
  std::uint64_t slicedN = 0;

  void validate() const;
};

/** Every slice one rank of one layer owns. */
struct QwenRankLayerSlices
{
  std::uint64_t layer = 0;
  std::vector<QwenInitializerSlice> initializers;
};

/**
 * Derive the exact per-rank slice list for one tensor-parallel degree.
 *
 * The partition follows the frozen export: Q/K/V/gate/up split their output
 * channels (whole heads for the projections, the same FFN interval for gate
 * and up), while O and down split the reduction channels and therefore emit a
 * full-width `D` partial that the runtime sums.
 *
 * @param shape  Verified shape facts.
 * @param degree Tensor-parallel degree (first acceptance: 2).
 * @param rank   Rank in `[0, degree)`.
 * @returns The initializer slices this rank owns, in layer order.
 * @throws std::invalid_argument for a shape/degree/rank that cannot split exactly.
 */
std::vector<QwenRankLayerSlices>
planQwenTensorSlices(const QwenTpShape& shape, std::uint64_t degree,
                     std::uint64_t rank);

/**
 * Slice one packed INT4 initializer along the plan's axis.
 *
 * Packed weights are `[N, K / block, blob]` little-endian nibbles. An output
 * slice copies whole outer rows; an input slice copies whole block columns,
 * which is exactly why the cut must stay block aligned. Neither path re-packs a
 * nibble, so the original quantized values survive unchanged.
 *
 * @param packed     Source bytes of the packed tensor.
 * @param n          Source output width.
 * @param k          Source reduction width.
 * @param axis       Slice axis.
 * @param begin,end  Half-open range on that axis.
 * @returns The rank's packed bytes.
 */
std::vector<std::uint8_t>
slicePackedInt4(const std::vector<std::uint8_t>& packed, std::uint64_t n,
                std::uint64_t k, QwenSliceAxis axis, std::uint64_t begin,
                std::uint64_t end);

/**
 * Slice the per-block scales that accompany a packed INT4 initializer.
 *
 * Scales are `[N, K / block]` flattened: one float per (output channel, block)
 * pair, matching the packed weight's own (row, block-column) indexing.
 *
 * @param scales      Source scale bytes (`N * K / block * 4`).
 * @param n           Source output width.
 * @param k           Source reduction width.
 * @param axis        Slice axis; must match the weight slice.
 * @param begin,end   Half-open range on that axis.
 * @param scaleBytes  Width of one scale element (4 for float32).
 * @returns The rank's scale bytes.
 */
std::vector<std::uint8_t>
sliceInt4Scales(const std::vector<std::uint8_t>& scales, std::uint64_t n,
                std::uint64_t k, QwenSliceAxis axis, std::uint64_t begin,
                std::uint64_t end, std::size_t scaleBytes = 4);

/** Total packed-weight bytes of one `[N, K]` INT4 initializer. */
std::uint64_t packedInt4Bytes(std::uint64_t n, std::uint64_t k);

/** One `MatMulNBits` node found while reading the frozen export. */
struct QwenNBitsNode
{
  std::uint64_t nodeIndex = 0;
  std::string nodeName;
  std::string weightInitializer;
  std::string scalesInitializer;
  std::uint64_t k = 0;
  std::uint64_t n = 0;
  std::uint64_t bits = 0;
  std::uint64_t blockSize = 0;
  bool hasBias = false;
  bool hasZeroPoint = false;
  bool hasGroupIndex = false;
};

/**
 * Read the frozen export's `MatMulNBits` facts.
 *
 * This is the only place TP shape facts come from. It parses the graph on the
 * caller's behalf so `planQwenTensorSlices` never trusts a model name or a
 * hand-written constant: an unsupported export (extra operator, non-INT4
 * bits, block size mismatch, zero-point/g_idx/bias) is rejected by name.
 *
 * @param modelBytes Serialized ONNX bytes (external data may stay on disk).
 * @param layers     Expected transformer layer count; 0 disables the check.
 * @returns Per-node facts in graph order.
 * @throws std::invalid_argument for an export this TP partition cannot handle.
 */
std::vector<QwenNBitsNode>
inspectQwenNBitsGraph(const std::vector<std::uint8_t>& modelBytes,
                      std::uint64_t expectedLayers = 0);

/** Derive the TP shape facts from an already-inspected node list. */
QwenTpShape
qwenTpShapeFromNodes(const std::vector<QwenNBitsNode>& nodes,
                     std::uint64_t hidden, std::uint64_t layers,
                     std::uint64_t queryHeads, std::uint64_t kvHeads,
                     std::uint64_t headDim, std::uint64_t ffn,
                     std::uint64_t vocab);

/** One rank unit the TP plan exposes to placement. */
struct QwenTpUnit
{
  std::string roleId;
  std::uint64_t rank = 0;
  std::uint64_t layerBegin = 0;
  std::uint64_t layerEnd = 0;
};

/** Which entry a source-graph node belongs to.
 *
 * The taxonomy is fixed by C2: one INIT per unit, one ATTN and one MLP per
 * layer, and a single HEAD on the egress unit. Classification is by the frozen
 * export's own naming, not by ordinal, because the export interleaves
 * preprocessing nodes with layer nodes.
 */
struct QwenTpEntryNodes
{
  std::string kind;      // "INIT" | "ATTN" | "MLP" | "HEAD"
  std::uint64_t layer = 0;
  // Source-graph node indices, ascending so the entry keeps the source order.
  std::vector<std::uint64_t> nodeIndices;
};

/** Classify one source graph's nodes into the per-entry subgraphs.
 *
 * A rank shards every layer, so each unit gets an identical taxonomy over the
 * same node set; what differs between ranks is the initializer slice, not which
 * nodes an entry contains. That is why this function takes no degree.
 *
 * @param nodeNames Node names in source order.
 * @param layers    Number of transformer layers (28 for the frozen export).
 * @returns One entry per kind and layer, INIT first and HEAD last.
 * @throws std::invalid_argument when a node matches no entry, or a layer is
 *         missing an attention or MLP part: an unclassified node would silently
 *         be dropped from every entry and never executed.
 */
std::vector<QwenTpEntryNodes>
classifyQwenTpEntryNodes(const std::vector<std::string>& nodeNames,
                         std::uint64_t layers);

/** Classify using the graph, so constant-derived nodes land where they belong.
 *
 * Naming alone cannot place a node outside every layer whose only inputs are
 * initializers. The frozen export has exactly one: a transpose of the tied
 * embedding table, consumed by lm_head. Naming would put it in INIT, which is
 * wrong twice over -- INIT is defined as turning accepted token ids into the
 * embedding X0, and the transpose feeds the egress head. A constant-derived node
 * therefore follows its consumer.
 *
 * @param model Source graph.
 * @param layers Number of transformer layers.
 * @throws std::invalid_argument when such a node's consumers span entries, since
 *         no single entry could own it.
 */
std::vector<QwenTpEntryNodes>
classifyQwenTpEntryNodes(const onnx::ModelProto& model, std::uint64_t layers);

/** Extract one entry's subgraph out of a parsed source model.
 *
 * The entry keeps only the nodes its classification named, plus every
 * initializer and value_info those nodes reference, so ORT can load it as a
 * standalone graph. Inputs and outputs are the tensors that cross the entry
 * boundary: an input is produced outside the entry, an output is consumed
 * outside it. Nothing is inferred from names; the boundary is computed from the
 * node set.
 *
 * The source model is moved, not copied: the frozen export carries a
 * multi-gigabyte external initializer set, and copying it once per entry is the
 * per-entry memory blowup the allocation rules forbid.
 *
 * @param source       Parsed source model, consumed.
 * @param entry        Entry whose nodes to keep.
 * @param entryId      Identifier used for the entry's graph name and digest.
 * @returns A self-contained model for this entry.
 * @throws std::invalid_argument when the entry names no node of the source.
 */
onnx::ModelProto
extractQwenTpEntryModel(onnx::ModelProto& source, const QwenTpEntryNodes& entry,
                        const std::string& entryId);

/** One verified subgraph entry a tensor-parallel unit executes.
 *
 * The projection must not invent the entry naming rule or the entry interface:
 * the caller assembles these from the source graph and hands over what it
 * verified, and the projection only references them by id. That keeps the plan a
 * description of entries the assembler really produced rather than a guess at
 * names it hopes to find later.
 */
struct QwenTpEntryContract
{
  std::string entryId;
  // Unit that owns this entry, matching a QwenTpUnit role id.
  std::string unitId;
  // ONNX boundary names of the subgraph, in the order the assembler emits them.
  std::vector<std::string> inputNames;
  std::vector<std::string> outputNames;
  // Slice of the model this entry executes: which layer, and which projection
  // kind. Empty kind marks INIT and HEAD, which are per-unit rather than
  // per-layer.
  std::string kind;
  std::uint64_t layer = 0;
  // Path of this entry's graph file, relative to the assembly root. Absolute
  // paths would bind a contract to one machine's layout, and a relative path is
  // also what lets the parent verify the file stays inside the root.
  std::string relativeGraphPath;
  // Content identity of that file, so a cached entry can be checked rather than
  // trusted by name.
  std::string graphDigest;
  // Explicit binding between the entry's own graph tensors and the template's slot ids.
  // A graph name is a fact about the export; a slot id is a fact about the sealed plan,
  // and neither implies the other. Frozen at assembly, where the entry's kind and the
  // frozen export's node structure are both in hand, so the parent never has to guess
  // from a tensor name -- which is the inference C4 forbids.
  //
  // An output slot the entry does not compute itself is listed in
  // `passthroughSlotByInputGraphName` instead: the attention entry leaves the pre-MLP
  // residual in its hidden input rather than returning it, so that value is passed
  // through rather than asked for.
  std::map<std::string, std::string> inputSlotByGraphName;
  std::map<std::string, std::string> outputSlotByGraphName;
  std::map<std::string, std::string> passthroughSlotByInputGraphName;
  // Graph outputs that are decode state. They travel under their declared state names and
  // are never folded into a reduction contribution.
  std::vector<std::string> stateOutputGraphNames;
  // Graph inputs the entry reads that no template slot produces. The frozen export
  // derives its mask scalars from the accepted mask, so a caller must synthesize them
  // rather than look them up; naming them here keeps that gap visible instead of letting
  // an omission look like a complete binding.
  std::vector<std::string> synthesizedInputGraphNames;

  void validate() const;

  /** Whether this contract carries everything a runner needs to load the entry.
   *
   * A contract built without assembly (the projection path) names boundary
   * tensors only; one built by assembly also names a verified file. Keeping the
   * distinction explicit stops a runner from treating the first kind as loadable.
   */
  bool hasGraphFile() const
  {
    return !relativeGraphPath.empty() && !graphDigest.empty();
  }
};

/** Generation contract a sharded plan needs so its egress unit can sample.
 *
 * The unit only reports its logits; the coordinator is what samples, and it reads the
 * contract to learn the token input, the decode-state pairs, the position policy and
 * the sampling parameters. Without one, the coordinator copies zero state names and
 * commits no KV, and every epoch would run without a state successor.
 *
 * Every field is a fact the template already knows except the sealing digests, which
 * the caller supplies because they are part of the sealed request rather than of the
 * model. The state pairs come from the template's own attention state slots, so the
 * contract cannot describe layers the plan does not execute.
 *
 * @param plan      Projected plan whose steps and slots supply the state pairs.
 * @param shape     Verified shape facts.
 * @param sealed    Digests and sampling values from the accepted request.
 * @throws std::invalid_argument when the template has no attention state, or the
 *         supplied contract is incomplete for sealing.
 */
struct QwenTpSealedGeneration
{
  std::size_t maxGeneratedTokens = 0;
  std::string samplingDigest;
  std::string tokenizerDigest;
  std::vector<std::int64_t> eosTokenIds;
  std::string samplingMode = "Greedy";
  double samplingTemperature = 0.0;
  std::uint64_t samplingTopK = 1;
  double samplingTopP = 1.0;
  double samplingRepetitionPenalty = 1.0;
  std::uint64_t samplingSeed = 1'750'001;
  std::string generationId;
};

NativeGenerationExecutionContractV1
buildQwenTpGenerationContract(const NativeExecutionPlan& plan, const QwenTpShape& shape,
                              const QwenTpSealedGeneration& sealed);

/** Bind a caller-supplied generation contract to a sharded plan's own KV slots.
 *
 * The application supplies generation options, and its state-name defaults describe one
 * role's KV. A sharded unit owns every layer's slices, so the names must come from the
 * template instead -- otherwise the coordinator commits state for tensors that do not
 * exist, or omits the layers that do.
 *
 * Only the state fields are rewritten. The token input, budget, sampling parameters and
 * digests stay the caller's, because those are request facts rather than model facts.
 *
 * @param plan     Projected plan whose attention state slots are authoritative.
 * @param contract Generation contract to rewrite in place.
 * @throws std::invalid_argument when the plan has no attention state, or the contract
 *         is not a streaming generation contract.
 */
void
bindQwenTpGenerationState(const NativeExecutionPlan& plan,
                          NativeGenerationExecutionContractV1& contract);

/** Build a placement candidate that carries a projected tensor-parallel plan.
 *
 * A projected plan is already in the form the template's expansion would produce: its
 * steps name units, and its plan degree is the tensor degree. So the candidate declares
 * those units as roles with a per-role degree of one. Nothing is sharded across
 * Providers, so no hybrid plan is required, while the plan's own degree stays two for its
 * step template.
 *
 * Every graph node is owned by one unit -- the ingress unit -- because source-graph
 * ownership stays single-valued. That is not a limitation here: both units execute the
 * same layers, and the steps say which unit runs which subgraph.
 *
 * @param graph          Planning graph the candidate must partition.
 * @param shape          Verified shape facts.
 * @param units          Units to project, at least two.
 * @param entryPrefix    Artifact entry prefix.
 * @param egressRole     Unit that owns the head; must be one of @p units.
 * @param model          Model descriptor the candidate binds.
 * @param artifactByUnit One recipe artifact digest per unit.
 * @param requirementByUnit Resource requirement per unit.
 * @throws std::invalid_argument on an inconsistent unit set or artifact cover.
 */
NativeSplitCandidate
buildQwenTpCandidate(const NativeGraphSnapshot& graph, const QwenTpShape& shape,
                     const std::vector<QwenTpUnit>& units, const std::string& entryPrefix,
                     const std::string& egressRole, const NativeModelDescriptor& model,
                     const std::map<std::string, std::string>& artifactByUnit,
                     const std::map<std::string, NativeRoleResourceRequirement>& requirementByUnit);

/** Geometry a rank's entry graph must be rewritten to describe.
 *
 * Slicing the weights is only half of a shard: the operators still declare the
 * source widths, so a graph with half the weight rows would fail to load or, worse,
 * load and compute a different function. Every field here is a source fact; the
 * rank's own values are derived from them.
 */
struct QwenTpRankGeometry
{
  std::uint64_t degree = 1;
  std::uint64_t rank = 0;
  std::uint64_t headDim = 0;
  // Source widths of the sharded projections.
  std::uint64_t queryWidth = 0;
  std::uint64_t kvWidth = 0;
  std::uint64_t hidden = 0;
  std::uint64_t ffn = 0;

  void validate() const;
};

/** Options that make assembly produce a rank's own shards.
 *
 * Without these, every entry keeps the source graph's full-width projections and
 * each rank loads complete weights. They are optional only because a caller that
 * has no verified shape cannot slice; a tensor-parallel Provider always has one.
 */
struct QwenTpAssemblyShards
{
  // Per layer, the slices this rank owns. Empty means "do not slice".
  std::vector<QwenRankLayerSlices> slicesByLayer;
  // Bounded read of the shared sidecar for non-contiguous slices.
  std::function<std::vector<std::uint8_t>(std::uint64_t offset, std::uint64_t length)>
    readExternal;

  // Geometry for the operator rewrite. Empty degree means "weights only", which
  // is only correct when every sharded width already equals the source value.
  QwenTpRankGeometry geometry;

  bool enabled() const { return !slicesByLayer.empty(); }
};

/** Write one extracted entry and return its verified contract.
 *
 * The file lands inside `root` at a path derived from the entry id, and the
 * digest is computed from the bytes actually written, so the contract describes
 * the file on disk rather than the intent to write one.
 *
 * @param root       Assembly root; it must already exist.
 * @param unitId     Unit that owns the entry.
 * @param entry      Classified node set.
 * @param source     Parsed source model, consumed.
 * @param graphName  File name for this entry, without directory components.
 * @returns The entry's contract, including the relative path and digest.
 * @throws std::invalid_argument when the name is not a plain file name, which is
 *         what keeps an entry from writing outside the root.
 */
QwenTpEntryContract
writeQwenTpEntry(const std::filesystem::path& root, const std::string& unitId,
                 const QwenTpEntryNodes& entry, onnx::ModelProto& source,
                 const std::string& graphName,
                 const QwenTpAssemblyShards* shards = nullptr);

/** Assemble one unit's whole entry table from a rank artifact.
 *
 * This is the single path from a source graph to a loadable unit: classify the
 * source, extract each entry, write it, and return the verified table. Keeping it
 * one function is deliberate. Splitting it would let a caller assemble part of a
 * table and hand the runner a unit whose entries disagree about which graph they
 * came from.
 *
 * All entries are written under `root`, named from the unit id and entry kind, so
 * a unit's files are identifiable without opening them.
 *
 * @param root       Assembly root, created if absent.
 * @param unitId     Unit that owns every entry in the result.
 * @param graphBytes Serialized source graph for this rank.
 * @param layers     Number of transformer layers the graph carries.
 * @returns The verified entry table, in classification order.
 * @throws std::runtime_error when the source cannot be parsed, and
 *         std::invalid_argument when classification or writing fails; a partially
 *         written table is never returned.
 */
std::vector<QwenTpEntryContract>
assembleQwenTpEntryTable(const std::filesystem::path& root, const std::string& unitId,
                         const std::vector<std::uint8_t>& graphBytes,
                         std::uint64_t layers);

/** Derive the TP shape facts from a source graph.
 *
 * The assembler holds only the graph bytes and the rank identity from the sealed
 * contract, so the geometry has to come from the export itself. Every fact needed
 * for slicing is present in the frozen graph and cross-checked: the attention
 * operator declares its head counts, the sharded projections declare their
 * widths, and a normalization weight declares the hidden size. Declaring these by
 * hand would let a Provider slice weights that no longer match the graph.
 *
 * @param modelBytes Serialized source graph.
 * @param layers     Layer count the caller already established.
 * @throws std::invalid_argument when a required node or attribute is missing, or
 *         the widths contradict each other.
 */
QwenTpShape
deriveQwenTpShape(const std::vector<std::uint8_t>& modelBytes, std::uint64_t layers);

/** Assemble a rank's entry table with its own weight shards. */
std::vector<QwenTpEntryContract>
assembleQwenTpEntryTable(const std::filesystem::path& root, const std::string& unitId,
                         const std::vector<std::uint8_t>& graphBytes,
                         std::uint64_t layers, const QwenTpAssemblyShards& shards);

/** Assemble a unit's table with a known head owner.
 *
 * Classification is per rank but the head belongs to one unit only. Without knowing which
 * unit owns it, every rank assembles a HEAD entry -- so a non-egress rank's table holds one
 * entry no step of its own references, and the strict verifier rejects the artifact for
 * exactly that reason. This overload drops the head from a non-egress unit's table so the
 * table matches the steps that will run.
 *
 * @param egressRole The unit that owns the head; other units omit it.
 * @throws std::invalid_argument when the egress role owns no head entry, which would mean
 *         the taxonomy and the caller disagree about the model.
 */
std::vector<QwenTpEntryContract>
assembleQwenTpEntryTable(const std::filesystem::path& root, const std::string& unitId,
                         const std::vector<std::uint8_t>& graphBytes,
                         std::uint64_t layers, const QwenTpAssemblyShards& shards,
                         const std::string& egressRole);

/** Rewrite an extracted entry's sharded weights to this rank's slice.
 *
 * An extracted entry still carries the source graph's full-width projections.
 * A rank owns only its share, so without this every rank would load complete
 * weights: the memory the partition exists to save, spent on both ranks.
 *
 * Two slice shapes are handled differently, and the difference is not an
 * optimization detail:
 *  - An output-channel slice over packed INT4 is a contiguous byte range, so the
 *    initializer keeps pointing at the shared sidecar with a narrowed offset,
 *    length and dims. No weight byte is copied.
 *  - An input-channel slice keeps every output row but drops block columns, so
 *    the bytes are not contiguous. Those are materialized inline, and only for
 *    the entry's own weight, not the whole model.
 *
 * Scales follow the same axis as their weight, because a scale is indexed by the
 * same (output row, block column) pair. A slice whose name matches no initializer
 * in the entry is ignored: an entry legitimately lacks other layers' weights.
 *
 * @param entry  Extracted entry model, rewritten in place.
 * @param layerSlices The rank's slices for this entry's layer, or nullptr for an
 *        entry that owns no sharded weight (INIT, HEAD).
 * @param readExternal Bounded read of the shared sidecar, used only for a slice
 *        whose bytes are not contiguous and must be materialized.
 * @returns Bytes of initializer data this entry now references, for accounting.
 */
std::uint64_t
applyQwenTpRankSlices(onnx::ModelProto& entry,
                      const QwenRankLayerSlices* layerSlices,
                      const std::function<std::vector<std::uint8_t>(
                        std::uint64_t offset, std::uint64_t length)>& readExternal);

/** Rewrite an entry's operator parameters to this rank's shard.
 *
 * The weights are already narrowed by `applyQwenTpRankSlices`; this makes the
 * graph agree with them. Three classes of declaration move together, and a shard
 * that updated only some of them would produce a shape error at load:
 *
 *  - `MatMulNBits` `K`/`N`: an output-channel shard halves N, an input-channel
 *    shard halves K. Leaving them at source width makes ORT reject the weight.
 *  - `GroupQueryAttention` head counts: whole heads per rank, which is what makes
 *    the reduced Q/K/V widths and the attention op consistent.
 *  - Value constraints that name a sharded width: the q/k normalization reshapes
 *    and the KV state dimensions. These are matched by value, not by name, because
 *    the export names them by literal content.
 *
 * A constant that equals none of the source widths is left alone: it describes
 * something this partition does not divide, such as the head dimension.
 *
 * @param entry    Extracted entry model, rewritten in place.
 * @param geometry Source geometry and this rank's index within it.
 * @returns How many operator attributes and value constraints changed, so a caller
 *          can tell "this entry had nothing to adapt" from "adaptation ran".
 */
std::uint64_t
adaptQwenTpEntryParameters(onnx::ModelProto& entry, const QwenTpRankGeometry& geometry);

/** Verify a rank's entry set before its READY record may be published.
 *
 * D6 requires the parent to check every returned entry and the set as a whole.
 * The checks are ordered from cheapest to most expensive, because a rejected
 * rank must fail before any file is read in full:
 *
 *  - a step references an entry the table lacks, or the table holds an entry no
 *    step references;
 *  - an entry belongs to another unit, or two entries share an id;
 *  - a path is absolute, escapes the root, or is not a plain name;
 *  - a digest is not canonical, or a file's bytes do not match its digest;
 *  - the table's files exceed the rank's total byte budget.
 *
 * The budget is per rank, not per entry: D6 forbids resetting it for each entry,
 * which would let a rank quietly assemble N times its declared size.
 *
 * @param root            Assembly root the entries must stay inside.
 * @param unitId          Unit whose READY is being considered.
 * @param table           Entry table the rank returned.
 * @param steps           Sealed steps the unit must be able to execute.
 * @param rankedByteBudget Total bytes this rank may occupy.
 * @returns Empty when the rank may be published READY, otherwise the first
 *          problem, phrased for the operator who has to fix the assembly.
 */
std::string
verifyQwenTpRankEntries(const std::filesystem::path& root, const std::string& unitId,
                        const std::vector<QwenTpEntryContract>& table,
                        const std::vector<NativeExecutionStepSpec>& steps,
                        std::uint64_t rankedByteBudget);

/** Turn an assembled rank spec into a tensor-parallel parent spec.
 *
 * Assembly produces one file per role: the rank's graph beside its shared
 * initializer sidecar. Tensor parallelism needs one file per entry instead, so
 * this splits that rank artifact into its entry table and rewrites the spec to
 * select the parent runner.
 *
 * Entries are written into the rank artifact's own directory, not a subdirectory.
 * The initializers stay external and shared (C3), so each entry's external data
 * path is "model.onnx.data"; ORT refuses a path that resolves outside the model's
 * directory, which a nested layout would produce.
 *
 * @param spec   Rank spec produced by assembly; rewritten in place.
 * @param layers Number of transformer layers the rank graph carries.
 * @param steps  Sealed steps this unit must be able to execute.
 * @param rankedByteBudget Total bytes this rank may occupy. It is one budget for
 *        the whole rank, per D6, not a limit applied afresh to each entry.
 * @param tensorDegree Degree from the sealed contract. Above one, entries are
 *        narrowed to this rank's slices; one keeps every entry at full width so a
 *        pipeline role is unaffected.
 * @param tensorRank Rank of this unit within `tensorDegree`.
 * @throws std::runtime_error when the rank artifact cannot be read or split.
 */
void
configureQwenTpRankSpec(NativeModelRunnerSpec& spec, std::uint64_t layers,
                        const std::vector<NativeExecutionStepSpec>& steps,
                        std::uint64_t rankedByteBudget,
                        std::uint64_t tensorDegree = 1,
                        std::uint64_t tensorRank = 0);

/** Serialize the step descriptors a spec carries for consistency checking.
 *
 * Only identity fields travel: the worker still owns the authoritative template,
 * and duplicating slot detail here would create a second source of truth.
 */
std::string
qwenTpStepDeclarationJson(const std::vector<NativeExecutionStepSpec>& steps);

/** Serialize a verified entry table for the runner spec.
 *
 * The spec is a flat string map, so the table travels as one JSON value. It is
 * written from verified contracts only: serializing a hand-built table would let
 * a runner load a file the assembler never produced.
 *
 * @throws std::invalid_argument when a contract is invalid or lacks its file,
 *         so an unverified table cannot be written at all.
 */
std::string
qwenTpEntryTableJson(const std::vector<QwenTpEntryContract>& entries);

/** Parse an entry table written by qwenTpEntryTableJson.
 *
 * Symmetric with the writer: every field the writer emits is required, and each
 * contract is validated, so a truncated or hand-edited table fails here instead
 * of producing a runner that loads the wrong file.
 */
std::vector<QwenTpEntryContract>
parseQwenTpEntryTable(const std::string& json);

/** The complete sealed TP template for one model and degree. */
struct QwenTpTemplate
{
  std::uint64_t degree = 1;
  std::vector<QwenTpUnit> units;
  // Per rank, per layer: the initializer slices that rank owns.
  std::vector<std::vector<QwenRankLayerSlices>> slicesByRank;
  // Verified entries referenced by the projected steps, empty when the caller
  // let the projection name them.
  std::vector<QwenTpEntryContract> entries;
};

/**
 * Build the unit/step template for one verified shape and degree.
 *
 * Each unit covers every layer; inside a token a unit runs one attention step,
 * one SUM, one MLP step and one SUM per layer, then the head on rank 0 only.
 * The steps reference recipe entries by name, so the caller supplies the entry
 * naming rule it will actually assemble (`entryPrefix`).
 *
 * @param shape       Verified shape facts.
 * @param degree      Tensor-parallel degree.
 * @param entryPrefix Prefix for generated recipe entry ids.
 * @returns Units and per-rank slice plans.
 * @throws std::invalid_argument for an unsplittable shape or degree.
 */
QwenTpTemplate
buildQwenTpTemplate(const QwenTpShape& shape, std::uint64_t degree,
                    const std::string& entryPrefix);

/** Build the template for caller-supplied units.
 *
 * The units carry their own role ids, so a caller that derived them from a
 * Catalog stage never has a naming rule imposed on it. The list must be
 * internally consistent: one unit per rank in [0, degree), distinct non-empty
 * role ids, and every unit covering all layers, because a tensor-parallel rank
 * holds a shard of every layer rather than a layer range.
 *
 * @param units Explicit units; validated as described above.
 * @throws std::invalid_argument when the units do not describe this degree.
 */
QwenTpTemplate
buildQwenTpTemplate(const QwenTpShape& shape, std::uint64_t degree,
                    const std::string& entryPrefix,
                    const std::vector<QwenTpUnit>& units);

/**
 * Project one template into sealed execution steps and dependencies.
 *
 * The result is deliberately data, not a second scheduler: `roleSpecFor` and
 * the existing worker consume it exactly like any other plan. Step ids encode
 * their token-local position so the runtime can bind a contribution to the
 * exact step that produced it.
 *
 * @param plan       Plan receiving the roles, steps and dependencies.
 * @param shape      Verified shape facts.
 * @param degree     Tensor-parallel degree.
 * @param entryPrefix Prefix for generated recipe entry ids.
 * @param egressRole  Role that owns the final response.
 */
void
projectQwenTpPlan(ndnsf::di::NativeExecutionPlan& plan, const QwenTpShape& shape,
                  std::uint64_t degree, const std::string& entryPrefix,
                  const std::string& egressRole);

/**
 * Project with caller-supplied units.
 *
 * @param units Explicit units; the roles in the plan are exactly these ids, so
 *              nothing downstream has to know a naming convention.
 * @param egressRole Must be one of the units; it owns the head.
 */
void
projectQwenTpPlan(ndnsf::di::NativeExecutionPlan& plan, const QwenTpShape& shape,
                  std::uint64_t degree, const std::string& entryPrefix,
                  const std::string& egressRole,
                  const std::vector<QwenTpUnit>& units);

/**
 * Project against caller-verified entries.
 *
 * Every CALL_SUBGRAPH step must resolve to an entry owned by the same unit, with
 * the kind and layer the step needs; the projection refuses to name an entry
 * that was not verified rather than emitting a plausible-looking id. `entryPrefix`
 * is unused in this form because the ids come from the contracts.
 *
 * @param entries Verified entry contracts for every unit.
 * @throws std::invalid_argument when an entry is missing, duplicated or belongs
 *         to another unit.
 */
void
projectQwenTpPlan(ndnsf::di::NativeExecutionPlan& plan, const QwenTpShape& shape,
                  std::uint64_t degree, const std::string& egressRole,
                  const std::vector<QwenTpUnit>& units,
                  const std::vector<QwenTpEntryContract>& entries);

} // namespace ndnsf::di::qwen

#endif // NDNSF_DI_QWEN_TENSOR_SPLIT_HPP
