#include "wan-generation.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.hpp"

#include <stdexcept>

namespace ndnsf::di::wan {
namespace {

std::size_t
whitespaceBytes(const std::string& text, std::size_t offset)
{
  const auto first = static_cast<std::uint8_t>(text[offset]);
  if (first == 0x20 || (first >= 0x09 && first <= 0x0d)) return 1;
  if (text.size() - offset >= 2 && first == 0xc2) {
    const auto second = static_cast<std::uint8_t>(text[offset + 1]);
    if (second == 0x85 || second == 0xa0) return 2;
  }
  if (text.size() - offset < 3) return 0;
  const auto second = static_cast<std::uint8_t>(text[offset + 1]);
  const auto third = static_cast<std::uint8_t>(text[offset + 2]);
  if ((first == 0xe1 && second == 0x9a && third == 0x80) ||
      (first == 0xe2 && second == 0x80 &&
        ((third >= 0x80 && third <= 0x8a) || third == 0xa8 || third == 0xa9 || third == 0xaf)) ||
      (first == 0xe2 && second == 0x81 && third == 0x9f) ||
      (first == 0xe3 && second == 0x80 && third == 0x80))
    return 3;
  return 0;
}

std::string
foldWhitespace(const std::string& text)
{
  if (text.size() > 1024 * 1024)
    throw std::invalid_argument("Wan prompt exceeds the tokenizer input limit");
  std::string result;
  result.reserve(text.size());
  bool addSpace = false;
  for (std::size_t offset = 0; offset < text.size();) {
    if (const auto length = whitespaceBytes(text, offset); length != 0) {
      offset += length;
      addSpace = !result.empty();
      continue;
    }
    if (addSpace) result.push_back(' ');
    addSpace = false;
    result.push_back(text[offset++]);
  }
  return result;
}

} // namespace

std::array<WanPromptTokens, 2>
tokenizeWanPrompts(const std::string& tokenizerFile, const std::string& tokenizerDigest,
                   std::size_t slots, const std::string& positive, const std::string& negative)
{
  if (slots == 0 || slots > 512)
    throw std::invalid_argument("Wan UMT5 token slots must be in [1,512]");
  constexpr std::int64_t eos = 1;
  constexpr std::int64_t pad = 0;
  qwen::NativeTokenizer tokenizer(tokenizerFile, tokenizerDigest);
  if (tokenizer.encode("", true) != std::vector<std::int64_t>{eos})
    throw std::invalid_argument("Wan UMT5 tokenizer must add EOS without built-in padding");
  const auto encode = [&] (const std::string& prompt) {
    WanPromptTokens result;
    result.ids = tokenizer.encode(foldWhitespace(prompt), true);
    if (result.ids.empty() || result.ids.back() != eos)
      throw std::invalid_argument("Wan UMT5 tokenizer output must end in EOS");
    if (result.ids.size() > slots) {
      result.ids.resize(slots);
      result.ids.back() = eos;
    }
    result.validTokens = result.ids.size();
    result.ids.resize(slots, pad);
    return result;
  };
  return {encode(positive), encode(negative)};
}

} // namespace ndnsf::di::wan
