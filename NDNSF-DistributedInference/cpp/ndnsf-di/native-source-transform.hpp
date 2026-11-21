#ifndef NDNSF_DI_NATIVE_SOURCE_TRANSFORM_HPP
#define NDNSF_DI_NATIVE_SOURCE_TRANSFORM_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf::di {

inline constexpr const char* NATIVE_FFN_PAIR_CAPABILITY_V1 = "onnx-source-ffn-pair-v1";
inline constexpr const char* NATIVE_SOURCE_ENTRY_PROGRAM_V1 = "source_entry_program_v1";
inline constexpr const char* NATIVE_SOURCE_ENTRY_PROGRAM_CAPABILITY_V1 =
  "onnx-source-entry-program-v1";

struct NativeExecutionPlan;

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

  void validate() const;
  std::string canonicalJson() const;
  static NativeOnnxSourceTransformV1 parse(const std::string& wire);
};

/** One graph derivation under an ordered, role-local CALL entry. */
struct NativeOnnxSourceEntryRecipeV1
{
  std::string entryId;
  // Exact ordered node identities in the canonical graph, including intermediates.
  std::vector<std::string> sourceNodes;
  NativeOnnxSourceTransformV1 transform;
};

/** Bind every derived graph of a generic ONNX rank into its signed role recipe. */
struct NativeOnnxSourceEntryProgramV1
{
  std::vector<NativeOnnxSourceEntryRecipeV1> entries;

  void validate() const;
  void validateRank(const NativeExecutionPlan& plan, const std::string& role,
                    std::uint64_t rank) const;
  std::vector<std::vector<std::uint64_t>>
  bindSourceOrdinals(const std::vector<std::uint64_t>& roleOrdinals) const;
  std::string canonicalJson() const;
  static NativeOnnxSourceEntryProgramV1 parse(const std::string& wire);
};

/** Split an admitted Conv input-channel axis into contiguous rank ranges. */
std::vector<std::pair<std::uint64_t, std::uint64_t>>
nativeConvInputChannelRanges(std::uint64_t extent, std::uint64_t degree);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_SOURCE_TRANSFORM_HPP
