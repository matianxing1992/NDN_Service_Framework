#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/qwen/tokenizer-bridge/tokenizer-abi.h"

#include <dlfcn.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace ndnsf::di::qwen {
namespace {

using CreateFn = NdiTokenResult (*)(const uint8_t*, size_t, void**);
using EncodeFn = NdiTokenResult (*)(const void*, const uint8_t*, size_t, uint8_t);
using DecodeFn = NdiTokenResult (*)(const void*, const uint32_t*, size_t, uint8_t);
using StableFn = NdiTokenResult (*)(const void*, const uint32_t*, size_t, uint8_t, uint8_t);
using FreeFn = void (*)(uint8_t*, size_t);
using DestroyFn = void (*)(void*);

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
errorText(const NdiTokenResult& result, FreeFn freeFn)
{
  std::string message;
  if (result.data != nullptr && result.size != 0) {
    message.assign(reinterpret_cast<const char*>(result.data), result.size);
  }
  if (freeFn != nullptr) {
    freeFn(result.data, result.size);
  }
  return message.empty() ? "tokenizer bridge returned an error" : message;
}

struct Bridge
{
  void* library = nullptr;
  CreateFn create = nullptr;
  EncodeFn encode = nullptr;
  DecodeFn decode = nullptr;
  StableFn decodeStable = nullptr;
  FreeFn free = nullptr;
  DestroyFn destroy = nullptr;

  explicit Bridge(const std::string& requested)
  {
    std::vector<std::string> candidates;
    if (!requested.empty()) {
      candidates.push_back(requested);
    }
    if (const auto* value = std::getenv("NDNSF_TOKENIZER_BRIDGE_LIB")) {
      if (*value != '\0') candidates.emplace_back(value);
    }
    candidates.emplace_back("libndnsf_tokenizer_bridge.so");
    std::string executable(4096, '\0');
    const auto length = ::readlink("/proc/self/exe", executable.data(), executable.size() - 1);
    if (length > 0) {
      executable.resize(static_cast<std::size_t>(length));
      const auto slash = executable.find_last_of('/');
      if (slash != std::string::npos) {
        candidates.push_back(executable.substr(0, slash) + "/libndnsf_tokenizer_bridge.so");
      }
    }
    for (const auto& candidate : candidates) {
      library = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (library != nullptr) break;
    }
    if (library == nullptr) {
      throw std::runtime_error("native tokenizer bridge library is unavailable");
    }
    create = reinterpret_cast<CreateFn>(::dlsym(library, "ndi_token_create"));
    encode = reinterpret_cast<EncodeFn>(::dlsym(library, "ndi_token_encode"));
    decode = reinterpret_cast<DecodeFn>(::dlsym(library, "ndi_token_decode"));
    decodeStable = reinterpret_cast<StableFn>(::dlsym(library, "ndi_token_decode_stable"));
    free = reinterpret_cast<FreeFn>(::dlsym(library, "ndi_token_free"));
    destroy = reinterpret_cast<DestroyFn>(::dlsym(library, "ndi_token_destroy"));
    if (!create || !encode || !decode || !decodeStable || !free || !destroy) {
      ::dlclose(library);
      library = nullptr;
      throw std::runtime_error("native tokenizer bridge ABI is incomplete");
    }
  }

  ~Bridge()
  {
    if (library != nullptr) {
      ::dlclose(library);
    }
  }

  Bridge(const Bridge&) = delete;
  Bridge& operator=(const Bridge&) = delete;
};

} // namespace

struct NativeTokenizer::Impl
{
  std::string digest;
  std::unique_ptr<Bridge> bridge;
  void* handle = nullptr;
  mutable std::mutex mutex;

  Impl(const std::string& path, const std::string& expected,
       const std::string& library)
    : digest(sha256File(path)), bridge(std::make_unique<Bridge>(library))
  {
    if (expected.empty() || digest != expected) {
      throw std::invalid_argument("tokenizer digest mismatch");
    }
    const auto bytes = readBytes(path);
    const auto result = bridge->create(bytes.data(), bytes.size(), &handle);
    if (result.code != 0) {
      const auto detail = errorText(result, bridge->free);
      if (handle != nullptr) bridge->destroy(handle);
      handle = nullptr;
      throw std::runtime_error("native tokenizer create failed: " + detail);
    }
    if (handle == nullptr) {
      throw std::runtime_error("native tokenizer create returned no handle");
    }
    // The bridge only borrows `bytes`; no tokenizer bytes remain aliased here.
  }

  ~Impl()
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (handle != nullptr) {
      bridge->destroy(handle);
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
      ? bridge->decodeStable(handle, checked.data(), checked.size(), skip ? 1 : 0,
                             final ? 1 : 0)
      : bridge->decode(handle, checked.data(), checked.size(), skip ? 1 : 0);
    if (result.code != 0) {
      throw std::runtime_error("native tokenizer decode failed: " +
                               errorText(result, bridge->free));
    }
    std::string text(reinterpret_cast<const char*>(result.data), result.size);
    bridge->free(result.data, result.size);
    return text;
  }
};

NativeTokenizer::NativeTokenizer(const std::string& tokenizerPath,
                                 const std::string& expectedDigest,
                                 const std::string& bridgeLibrary)
  : m_impl(std::make_unique<Impl>(tokenizerPath, expectedDigest, bridgeLibrary))
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
  const auto result = m_impl->bridge->encode(
    m_impl->handle, reinterpret_cast<const uint8_t*>(text.data()), text.size(),
    addSpecialTokens ? 1 : 0);
  if (result.code != 0) {
    throw std::runtime_error("native tokenizer encode failed: " +
                             errorText(result, m_impl->bridge->free));
  }
  if (result.size % sizeof(std::uint32_t) != 0) {
    const auto detail = errorText(result, m_impl->bridge->free);
    throw std::runtime_error("native tokenizer returned malformed IDs: " + detail);
  }
  std::vector<std::int64_t> ids;
  ids.reserve(result.size / sizeof(std::uint32_t));
  for (std::size_t offset = 0; offset < result.size; offset += sizeof(std::uint32_t)) {
    std::uint32_t id = 0;
    std::memcpy(&id, result.data + offset, sizeof(id));
    ids.push_back(static_cast<std::int64_t>(id));
  }
  m_impl->bridge->free(result.data, result.size);
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
