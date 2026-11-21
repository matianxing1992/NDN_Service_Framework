#ifndef NDNSF_DI_NATIVE_SOURCE_TRANSFORM_HPP
#define NDNSF_DI_NATIVE_SOURCE_TRANSFORM_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

inline constexpr const char* NATIVE_FFN_PAIR_CAPABILITY_V1 = "onnx-source-ffn-pair-v1";
inline constexpr const char* NATIVE_SOURCE_ENTRY_PROGRAM_V1 = "source_entry_program_v1";
inline constexpr const char* NATIVE_SOURCE_ENTRY_PROGRAM_CAPABILITY_V1 =
  "onnx-source-entry-program-v1";
inline constexpr const char* NATIVE_SOURCE_ATTENTION_ENTRY_CAPABILITY_V1 =
  "onnx-source-attention-entry-v1";
inline constexpr const char* NATIVE_SOURCE_TENSOR_LAYOUT_CAPABILITY_V1 =
  "onnx-source-tensor-layout-v1";

struct NativeExecutionPlan;
struct NativeExecutionStepSpec;
struct NativeStepSlotSpec;
struct NativeTensorContract;

struct NativeOnnxTransferRange
{
  std::size_t axis;
  std::uint64_t begin;
  std::uint64_t end;

  bool operator==(const NativeOnnxTransferRange& other) const
  {
    return axis == other.axis && begin == other.begin && end == other.end;
  }
};

/** The original ONNX operand and optional slice behind a physical CALL slot. */
struct NativeOnnxEntryPort
{
  std::string sourceName;
  std::optional<NativeOnnxTransferRange> range;

  std::string slotId() const;
  std::string layoutDigest(const std::string& graphDigest) const;
  void validateSourceTensor(const NativeStepSlotSpec& slot,
                            const NativeTensorContract& source) const;
};

/** Versioned lowering instruction carried by a transformed ONNX role recipe.
 *
 * The source graph and initializer identities stay bound to the original
 * admitted model; this descriptor only states how one role derives its own
 * executable graph from that source.
 */
struct NativeOnnxSourceTransformV1
{
  std::string schema = "source_transform_v1";
  std::string kind;
  std::string node;
  std::string sourceConv;
  std::string axis;
  std::string input;
  std::string weight;
  std::string bias;
  std::string output;
  // The first projection is described by node/weight. These fields bind the
  // second projection of a complete FFN slice to the same channel interval.
  std::string downNode;
  std::string downWeight;
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  std::uint64_t extent = 0;
  std::vector<std::int64_t> pads;
  std::vector<std::int64_t> strides;
  std::vector<std::int64_t> dilations;
  // Exact original Q, K and V SSA ports. Head coordinates describe the
  // contiguous subset shared by Q/K/V output and O input slicing.
  std::vector<std::string> attentionPorts;
  std::uint64_t headWidth = 0;
  std::string statistic;
  std::string globalStatistic;

  void validate() const;
  std::string canonicalJson() const;
  static NativeOnnxSourceTransformV1 parse(const std::string& wire);
  bool isAttention() const;
  bool isRmsNorm() const;
  bool hasExplicitEntryPorts() const;
  std::vector<NativeStepSlotSpec>
  originalPorts(const std::vector<NativeStepSlotSpec>& slots, bool outputs) const;
};

/** One graph derivation under an ordered, role-local CALL entry. */
struct NativeOnnxSourceEntryRecipeV1
{
  std::string entryId;
  // Exact ordered node identities in the canonical graph, including intermediates.
  std::vector<std::string> sourceNodes;
  NativeOnnxSourceTransformV1 transform;
  std::vector<std::string> inputPorts;
  std::vector<std::string> outputPorts;
  std::string tensorBoundaryDigest{};

  static NativeOnnxSourceEntryRecipeV1 fromTensorStep(const NativeExecutionStepSpec& step);
};

/** Logical NCHW channel ranges and the original groups executed by one Conv rank. */
struct NativeOnnxConvShard
{
  std::uint64_t inputBegin;
  std::uint64_t inputEnd;
  std::uint64_t outputBegin;
  std::uint64_t outputEnd;
  std::uint64_t groups;
};

/** Two contiguous rank ranges, [0, split) and [split, shape[axis]). */
struct NativeOnnxTwoRankLayout
{
  std::size_t axis;
  std::uint64_t split;

  bool operator==(const NativeOnnxTwoRankLayout& other) const
  {
    return axis == other.axis && split == other.split;
  }
};

/** One source interval needed to reconstruct a rank's logical Concat output. */
struct NativeOnnxConcatPiece
{
  std::size_t input;
  std::size_t producer;
  std::size_t consumer;
  std::uint64_t begin;
  std::uint64_t end;
  std::uint64_t outputBegin;
};

/** Layout decisions in original node order. Unsharded computations belong to rank zero. */
struct NativeOnnxNodeLayout
{
  std::map<std::string, NativeOnnxTwoRankLayout> outputs;
  std::vector<std::string> reconstructInputs;
  std::vector<NativeOnnxConcatPiece> concatPieces;
  std::optional<std::array<NativeOnnxConvShard, 2>> convolution;
};

/** Bind every derived graph of a generic ONNX rank into its signed role recipe. */
struct NativeOnnxSourceEntryProgramV1
{
  std::vector<NativeOnnxSourceEntryRecipeV1> entries;
  std::map<std::string, NativeOnnxEntryPort> tensorPorts{};
  std::vector<NativeOnnxNodeLayout> tensorLayouts{};

  void validate() const;
  void validateRank(const NativeExecutionPlan& plan, const std::string& role,
                    std::uint64_t rank) const;
  std::vector<std::vector<std::uint64_t>>
  bindSourceOrdinals(const std::vector<std::uint64_t>& roleOrdinals) const;
  std::vector<std::string> sourceNodeCover() const;
  std::string canonicalJson() const;
  static NativeOnnxSourceEntryProgramV1 parse(const std::string& wire);
};

/** Split an admitted Conv input-channel axis into contiguous rank ranges. */
std::vector<std::pair<std::uint64_t, std::uint64_t>>
nativeConvInputChannelRanges(std::uint64_t extent, std::uint64_t degree);

/** Compute a proportional initializer shard after validating the packing alignment. */
std::uint64_t
nativeFfnSliceBytes(std::uint64_t sourceBytes, std::uint64_t extent, std::uint64_t width);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_SOURCE_TRANSFORM_HPP
