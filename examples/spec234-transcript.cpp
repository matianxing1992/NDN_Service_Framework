/** Host-side transcript tokenizer for the frozen Spec 234 load.
 *
 * Encodes one UTF-8 text file with the pinned Qwen tokenizer and writes the
 * canonical token list used by the workload fixture. The tool exists so the
 * four-round transcript is tokenized by the same engine the runtime uses,
 * not by a second implementation.
 */

#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <openssl/sha.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string
sha256File(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read " + path);
  SHA256_CTX context;
  if (SHA256_Init(&context) != 1)
    throw std::runtime_error("sha256 init failed");
  std::vector<char> buffer(1 << 20);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto got = input.gcount();
    if (got > 0 && SHA256_Update(&context, buffer.data(), static_cast<std::size_t>(got)) != 1)
      throw std::runtime_error("sha256 update failed");
  }
  unsigned char digest[SHA256_DIGEST_LENGTH];
  if (SHA256_Final(digest, &context) != 1)
    throw std::runtime_error("sha256 final failed");
  static const char* hex = "0123456789abcdef";
  std::string result;
  result.reserve(SHA256_DIGEST_LENGTH * 2);
  for (const auto value : digest) {
    result.push_back(hex[value >> 4]);
    result.push_back(hex[value & 0x0f]);
  }
  return result;
}

} // namespace

int
main(int argc, char** argv)
{
  std::string tokenizer;
  std::string text;
  std::string output;
  bool addSpecial = true;
  for (int index = 1; index < argc; ++index) {
    const std::string option(argv[index]);
    if (option == "--tokenizer" && index + 1 < argc)
      tokenizer = argv[++index];
    else if (option == "--text" && index + 1 < argc)
      text = argv[++index];
    else if (option == "--output" && index + 1 < argc)
      output = argv[++index];
    else if (option == "--no-special")
      addSpecial = false;
    else {
      std::cerr << "usage: " << argv[0] << " --tokenizer FILE --text FILE --output FILE"
                << " [--no-special]\n";
      return 2;
    }
  }
  if (tokenizer.empty() || text.empty() || output.empty()) {
    std::cerr << "usage: " << argv[0] << " --tokenizer FILE --text FILE --output FILE\n";
    return 2;
  }
  try {
    std::ifstream textInput(text, std::ios::binary);
    if (!textInput)
      throw std::runtime_error("cannot read " + text);
    const std::string content((std::istreambuf_iterator<char>(textInput)),
                              std::istreambuf_iterator<char>());
    const std::string digest = "sha256:" + sha256File(tokenizer);
    ndnsf::di::qwen::NativeTokenizer engine(tokenizer, digest);
    const auto ids = engine.encode(content, addSpecial);
    const auto decoded = engine.decode(ids, true);
    auto json = ndnsf::di::NativeJson::object();
    json["schema"] = "ndnsf-spec234-transcript-v1";
    json["tokenizerDigest"] = digest;
    json["textSha256"] = "sha256:" + sha256File(text);
    json["addSpecialTokens"] = addSpecial;
    json["tokenCount"] = ids.size();
    json["ids"] = ids;
    json["decoded"] = decoded;
    std::ofstream result(output, std::ios::binary);
    if (!result)
      throw std::runtime_error("cannot write " + output);
    result << json.dump(2) << '\n';
    std::cout << "tokens=" << ids.size() << " text=" << text << '\n';
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "transcript failed: " << error.what() << '\n';
    return 1;
  }
}
