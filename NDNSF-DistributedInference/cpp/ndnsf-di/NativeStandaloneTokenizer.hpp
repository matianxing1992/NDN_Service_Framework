#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_STANDALONE_TOKENIZER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_STANDALONE_TOKENIZER_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeStandaloneTokenizerOptions
{
  std::string tokenizerPath;
};

struct NativeGenerationTextDecoders
{
  std::function<std::string(const std::vector<std::int64_t>&)> full;
  std::function<std::string(const std::vector<std::int64_t>&, bool)> stable;
};

/**
 * Create paired digest-bound callbacks for the native terminal role.  Both
 * callbacks share one read-only Rust tokenizer owner; no process, interpreter,
 * or per-token helper is created.
 */
std::function<std::string(const std::vector<std::int64_t>&)>
makeNativeStandaloneTokenizerDecoder(
  NativeStandaloneTokenizerOptions options,
  std::string expectedDigest);

NativeGenerationTextDecoders
makeNativeStandaloneTokenizerDecoders(
  NativeStandaloneTokenizerOptions options,
  std::string expectedDigest);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_STANDALONE_TOKENIZER_HPP
