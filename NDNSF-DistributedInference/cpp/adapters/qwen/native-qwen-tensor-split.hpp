#ifndef NDNSF_DI_QWEN_TENSOR_SPLIT_HPP
#define NDNSF_DI_QWEN_TENSOR_SPLIT_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"

#include <onnx/onnx_pb.h>

#include <cstddef>
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
                 const std::string& graphName);

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
