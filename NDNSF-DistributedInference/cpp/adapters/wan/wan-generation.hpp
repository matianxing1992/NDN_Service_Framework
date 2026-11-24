#ifndef NDNSF_DI_WAN_GENERATION_HPP
#define NDNSF_DI_WAN_GENERATION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di::wan {

inline constexpr std::uint64_t WAN_FIXTURE_TRACE_MAX_BYTES = 1U << 20;
inline constexpr std::uint64_t WAN_TRACE_HARD_MAX_BYTES = 2ULL << 30;

struct WanPromptTokens
{
  std::vector<std::int64_t> ids;
  std::size_t validTokens = 0;
};

/** Encode two UTF-8 prompts using the same digest-bound UMT5 tokenizer.
 * This folds Unicode whitespace. Apply any upstream text repair and HTML
 * entity decoding before supplying the prompts.
 */
std::array<WanPromptTokens, 2>
tokenizeWanPrompts(const std::string& tokenizerFile, const std::string& tokenizerDigest,
                   std::size_t slots, const std::string& positive, const std::string& negative);

struct WanGenerationOptions
{
  std::array<std::int64_t, 4> latentShape{};
  std::int64_t paddedVideoTokens = 0;
  std::int64_t textWidth = 0;
  std::size_t steps = 0;
  double shift = 1;
  float guidance = 1;
  std::uint64_t seed = 0;
  std::size_t inlineLimit = 0;
  std::string generationId;
  std::chrono::steady_clock::time_point deadline;
  RequestOptions childRequest;
  std::int64_t vaeSpatialStride = 8;
  std::uint64_t traceMaxBytes = WAN_FIXTURE_TRACE_MAX_BYTES;
};

struct WanChildReceipt
{
  std::string label;
  std::string requestId;
  std::string planDigest;
  std::string inputDigest;
};

struct WanSamplingReceipt
{
  std::int64_t timestep;
  unsigned predictorOrder;
  unsigned correctorOrder;
};

/** Optional local observation of an admitted tensor. All references are borrowed. */
using WanTensorObserver = std::function<void(const std::string&, const NamedTensorView&,
                                             const WanChildReceipt*,
                                             const std::optional<WanSamplingReceipt>&)>;

struct WanGenerationResult
{
  std::array<std::int64_t, 3> framesHeightWidth{};
  std::uint64_t rgb24Bytes = 0;
  std::string initialLatentDigest;
  std::vector<WanChildReceipt> children;
};

struct WanClipOutput
{
  std::array<std::int64_t, 3> framesHeightWidth{};
  std::uint64_t rgb24Bytes = 0;
  std::uint64_t responseBytes = 0;
};

WanClipOutput
planWanClipOutput(const std::array<std::int64_t, 4>& latentShape);

WanClipOutput
planWanClipOutput(const std::array<std::int64_t, 4>& latentShape,
                  std::int64_t vaeSpatialStride);

/** Bound the encoded observation stream without allocating tensors or running a child. */
std::uint64_t
planWanGenerationTraceBytes(const WanGenerationOptions& options,
                            std::size_t positiveTokens, std::size_t negativeTokens);

/** The sink consumes each RGB24 chunk before returning. The view is borrowed. */
using WanRgb24Sink = std::function<void(ndn::span<const std::uint8_t>)>;

void
writeWanRgb24(ndn::span<const std::uint8_t> planarRgb, const WanRgb24Sink& sink,
             const std::function<void()>& requireActive);

/** Run one native T2V generation with a single admitted child in flight.
 * The caller supplies IDs from the pinned UMT5 tokenizer and a fixed placement
 * binding. requireActive checks cancellation at the generation owner boundary.
 */
WanGenerationResult
runWanGeneration(const User& user, const PreparedModel& textEncoder,
                 const PreparedModel& denoiser, const PreparedModel& decoder,
                 const WanPromptTokens& positive, const WanPromptTokens& negative,
                 const WanGenerationOptions& options,
                 const WanRgb24Sink& sink,
                 const std::function<void()>& requireActive,
                 const WanTensorObserver& observe = {});

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_GENERATION_HPP
