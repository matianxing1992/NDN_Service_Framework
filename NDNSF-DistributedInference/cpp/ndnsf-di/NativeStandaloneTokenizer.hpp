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
  std::string pythonExecutable = "python3";
  std::string pythonModule =
    "ndnsf_distributed_inference.native_token_decode_helper";
};

/**
 * Create a digest-bound standalone tokenizer callback for the native terminal
 * role.  The callback invokes the deployment-safe Rust-backed `tokenizers`
 * helper without importing PyTorch or Transformers into the C++ process.
 */
std::function<std::string(const std::vector<std::int64_t>&)>
makeNativeStandaloneTokenizerDecoder(
  NativeStandaloneTokenizerOptions options,
  std::string expectedDigest);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_STANDALONE_TOKENIZER_HPP
