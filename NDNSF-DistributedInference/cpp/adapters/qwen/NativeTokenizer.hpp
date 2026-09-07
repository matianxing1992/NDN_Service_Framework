#ifndef NDNSF_DI_NATIVE_TOKENIZER_HPP
#define NDNSF_DI_NATIVE_TOKENIZER_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::di::qwen {

/**
 * Digest-bound, read-only tokenizer backed by the pinned Rust tokenizers
 * bridge.  The object owns the bridge handle and serializes calls into it;
 * callers may share it between decoder callbacks but must keep it alive while
 * callbacks are in flight.
 */
class NativeTokenizer
{
public:
  NativeTokenizer(const std::string& tokenizerPath,
                  const std::string& expectedDigest,
                  const std::string& bridgeLibrary = {});
  ~NativeTokenizer();

  NativeTokenizer(const NativeTokenizer&) = delete;
  NativeTokenizer& operator=(const NativeTokenizer&) = delete;

  /** Encode UTF-8 application text using the tokenizer's configured model. */
  std::vector<std::int64_t> encode(const std::string& text,
                                   bool addSpecialTokens = true) const;

  /** Decode complete token IDs to UTF-8 text. */
  std::string decode(const std::vector<std::int64_t>& ids,
                     bool skipSpecialTokens = true) const;

  /**
   * Decode a candidate prefix.  With final=false an unstable replacement
   * suffix is withheld; final=true is byte-identical to decode().
   */
  std::string decodeStable(const std::vector<std::int64_t>& ids,
                           bool skipSpecialTokens = true,
                           bool final = false) const;

  const std::string& digest() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace ndnsf::di::qwen

#endif // NDNSF_DI_NATIVE_TOKENIZER_HPP
