#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeStandaloneTokenizer.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.hpp"

#include <memory>
#include <stdexcept>

namespace ndnsf::di {

std::function<std::string(const std::vector<std::int64_t>&)>
makeNativeStandaloneTokenizerDecoder(
  NativeStandaloneTokenizerOptions options,
  std::string expectedDigest)
{
  return makeNativeStandaloneTokenizerDecoders(
           std::move(options), std::move(expectedDigest)).full;
}

NativeGenerationTextDecoders
makeNativeStandaloneTokenizerDecoders(
  NativeStandaloneTokenizerOptions options,
  std::string expectedDigest)
{
  if (options.tokenizerPath.empty() || expectedDigest.empty()) {
    throw std::invalid_argument("standalone tokenizer path/digest is required");
  }
  auto tokenizer = std::make_shared<qwen::NativeTokenizer>(
    options.tokenizerPath, expectedDigest);
  NativeGenerationTextDecoders result;
  result.full = [tokenizer](const std::vector<std::int64_t>& ids) {
    return tokenizer->decode(ids, true);
  };
  result.stable = [tokenizer](const std::vector<std::int64_t>& ids, bool final) {
    return tokenizer->decodeStable(ids, true, final);
  };
  return result;
}

} // namespace ndnsf::di
