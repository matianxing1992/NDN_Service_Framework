#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/tokenizer-bridge/tokenizer-abi.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>

namespace ndnsf::di::qwen {
namespace {

std::string
sha256File(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::invalid_argument("tokenizer file is unavailable");
  }
  SHA256_CTX context;
  if (SHA256_Init(&context) != 1) {
    throw std::runtime_error("tokenizer digest initialization failed");
  }
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0 && SHA256_Update(&context, buffer.data(),
                                   static_cast<std::size_t>(count)) != 1) {
      throw std::runtime_error("tokenizer digest update failed");
    }
  }
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  if (SHA256_Final(digest.data(), &context) != 1) {
    throw std::runtime_error("tokenizer digest finalization failed");
  }
  std::ostringstream output;
  output << "sha256:" << std::hex << std::setfill('0');
  for (const auto value : digest) {
    output << std::setw(2) << static_cast<unsigned>(value);
  }
  return output.str();
}

std::vector<uint8_t>
readBytes(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::invalid_argument("tokenizer file is unavailable");
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size <= 0 || static_cast<std::uint64_t>(size) > 32ULL * 1024ULL * 1024ULL) {
    throw std::invalid_argument("tokenizer file size is out of range");
  }
  input.seekg(0, std::ios::beg);
  std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw std::runtime_error("tokenizer file read was truncated");
  }
  return bytes;
}

std::string
errorText(const NdiTokenResult& result)
{
  std::string message;
  if (result.data != nullptr && result.size != 0) {
    message.assign(reinterpret_cast<const char*>(result.data), result.size);
  }
  ndi_token_free(result.data, result.size);
  return message.empty() ? "tokenizer engine returned an error" : message;
}

} // namespace

struct NativeTokenizer::Impl
{
  std::string digest;
  void* handle = nullptr;
  mutable std::mutex mutex;

  Impl(const std::string& path, const std::string& expected)
    : digest(sha256File(path))
  {
    if (expected.empty() || digest != expected) {
      throw std::invalid_argument("tokenizer digest mismatch");
    }
    // Verify the authenticated tokenizer bytes before constructing any Rust
    // engine.  This keeps an identity failure deterministic and prevents a
    // wrong artifact from ever reaching the engine.
    const auto bytes = readBytes(path);
    const auto result = ndi_token_create(bytes.data(), bytes.size(), &handle);
    if (result.code != 0) {
      const auto detail = errorText(result);
      if (handle != nullptr) ndi_token_destroy(handle);
      handle = nullptr;
      throw std::runtime_error("native tokenizer create failed: " + detail);
    }
    if (handle == nullptr) {
      throw std::runtime_error("native tokenizer create returned no handle");
    }
    // ndi_token_create only borrows `bytes`; no tokenizer bytes remain
    // aliased here.
  }

  ~Impl()
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (handle != nullptr) {
      ndi_token_destroy(handle);
      handle = nullptr;
    }
  }

  std::vector<std::uint32_t>
  checkedIds(const std::vector<std::int64_t>& ids) const
  {
    std::vector<std::uint32_t> result;
    result.reserve(ids.size());
    for (const auto id : ids) {
      if (id < 0 || static_cast<std::uint64_t>(id) >
          std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("token ID is out of range");
      }
      result.push_back(static_cast<std::uint32_t>(id));
    }
    return result;
  }

  std::string
  decodeImpl(const std::vector<std::int64_t>& ids, bool skip, bool final,
             bool stable) const
  {
    const auto checked = checkedIds(ids);
    std::lock_guard<std::mutex> lock(mutex);
    const auto result = stable
      ? ndi_token_decode_stable(handle, checked.data(), checked.size(),
                                skip ? 1 : 0, final ? 1 : 0)
      : ndi_token_decode(handle, checked.data(), checked.size(), skip ? 1 : 0);
    if (result.code != 0) {
      throw std::runtime_error("native tokenizer decode failed: " +
                               errorText(result));
    }
    if (result.size != 0 && result.data == nullptr) {
      throw std::runtime_error("native tokenizer returned a null output buffer");
    }
    std::string text;
    if (result.size != 0) {
      text.assign(reinterpret_cast<const char*>(result.data), result.size);
    }
    ndi_token_free(result.data, result.size);
    return text;
  }
};

NativeTokenizer::NativeTokenizer(const std::string& tokenizerPath,
                                 const std::string& expectedDigest)
  : m_impl(std::make_unique<Impl>(tokenizerPath, expectedDigest))
{
}

NativeTokenizer::~NativeTokenizer() = default;

std::vector<std::int64_t>
NativeTokenizer::encode(const std::string& text, bool addSpecialTokens) const
{
  if (text.size() > 1024 * 1024) {
    throw std::invalid_argument("tokenizer input is too large");
  }
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  const auto result = ndi_token_encode(
    m_impl->handle, reinterpret_cast<const uint8_t*>(text.data()), text.size(),
    addSpecialTokens ? 1 : 0);
  if (result.code != 0) {
    throw std::runtime_error("native tokenizer encode failed: " +
                             errorText(result));
  }
  if (result.size % sizeof(std::uint32_t) != 0) {
    const auto detail = errorText(result);
    throw std::runtime_error("native tokenizer returned malformed IDs: " + detail);
  }
  std::vector<std::int64_t> ids;
  ids.reserve(result.size / sizeof(std::uint32_t));
  for (std::size_t offset = 0; offset < result.size; offset += sizeof(std::uint32_t)) {
    std::uint32_t id = 0;
    std::memcpy(&id, result.data + offset, sizeof(id));
    ids.push_back(static_cast<std::int64_t>(id));
  }
  ndi_token_free(result.data, result.size);
  return ids;
}

std::string
NativeTokenizer::decode(const std::vector<std::int64_t>& ids,
                        bool skipSpecialTokens) const
{
  return m_impl->decodeImpl(ids, skipSpecialTokens, true, false);
}

std::string
NativeTokenizer::decodeStable(const std::vector<std::int64_t>& ids,
                              bool skipSpecialTokens, bool final) const
{
  return m_impl->decodeImpl(ids, skipSpecialTokens, final, true);
}

const std::string&
NativeTokenizer::digest() const noexcept
{
  return m_impl->digest;
}

} // namespace ndnsf::di::qwen
