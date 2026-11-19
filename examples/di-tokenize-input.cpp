#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <ndn-cxx/util/string-helper.hpp>

#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

/** Encode an offline prompt through the same digest-bound native tokenizer as inference. */
int
main(int argc, char** argv)
{
  try {
    std::string tokenizerFile;
    std::string tokenizerDigest;
    std::string textFile;
    bool addSpecialTokens = true;
    for (int index = 1; index < argc; ++index) {
      const std::string option(argv[index]);
      if (option == "--no-special-tokens") {
        addSpecialTokens = false;
        continue;
      }
      if (index + 1 >= argc)
        throw std::invalid_argument("missing value for " + option);
      const std::string value(argv[++index]);
      if (option == "--tokenizer" && tokenizerFile.empty()) tokenizerFile = value;
      else if (option == "--tokenizer-digest" && tokenizerDigest.empty()) tokenizerDigest = value;
      else if (option == "--text-file" && textFile.empty()) textFile = value;
      else throw std::invalid_argument("unknown or repeated argument " + option);
    }
    if (tokenizerFile.empty() || tokenizerDigest.empty() || textFile.empty()) {
      throw std::invalid_argument("usage: ndnsf-di-tokenize-input --tokenizer FILE "
        "--tokenizer-digest sha256:HASH --text-file FILE [--no-special-tokens]");
    }
    constexpr std::size_t MAX_TEXT_BYTES = 64 * 1024;
    constexpr std::size_t MAX_TOKEN_COUNT = 16 * 1024;
    std::ifstream input(textFile, std::ios::binary);
    if (!input)
      throw std::invalid_argument("prompt file is unavailable");
    std::string text(MAX_TEXT_BYTES + 1, '\0');
    input.read(text.data(), text.size());
    const auto count = static_cast<std::size_t>(input.gcount());
    if (input.bad() || count > MAX_TEXT_BYTES)
      throw std::invalid_argument("prompt exceeds its read bound");
    text.resize(count);

    ndnsf::di::qwen::NativeTokenizer tokenizer(tokenizerFile, tokenizerDigest);
    const auto ids = tokenizer.encode(text, addSpecialTokens);
    if (ids.size() > MAX_TOKEN_COUNT)
      throw std::invalid_argument("prompt exceeds its token bound");
    ndnsf::di::NamedTensor tensor;
    tensor.name = "input_ids";
    tensor.elementType = ndnsf::di::TensorElementType::Int64;
    tensor.shape = {1, static_cast<std::int64_t>(ids.size())};
    tensor.payload.resize(ids.size() * sizeof(std::int64_t));
    if (!ids.empty())
      std::memcpy(tensor.payload.data(), ids.data(), tensor.payload.size());
    const auto bundle = ndnsf::di::encodeTensorBundle({tensor});
    const ndnsf::di::NativeJson receipt{
      {"schema", "ndnsf-di-tokenized-input-v1"}, {"tokenizerDigest", tokenizer.digest()},
      {"addSpecialTokens", addSpecialTokens}, {"tokenIds", ids},
      {"decodedText", tokenizer.decode(ids, false)}, {"tensorBundleHex", ndn::toHex(bundle)}};
    std::cout << ndnsf::di::nativeCanonicalJson(receipt) << '\n';
    return std::cout ? 0 : 2;
  }
  catch (const std::exception& error) {
    std::cerr << "INPUT_PREPARATION_FAILED: " << error.what() << '\n';
    return 2;
  }
}
