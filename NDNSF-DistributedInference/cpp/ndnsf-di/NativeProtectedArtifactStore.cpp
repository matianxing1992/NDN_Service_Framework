#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedArtifactStore.hpp"
#include <ndn-cxx/util/sha256.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf::di {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t GcmTagBytes = 16;
constexpr std::size_t StreamChunkBytes = 1U << 20;

std::runtime_error rejected(const std::string& reason)
{
  return std::runtime_error("DI_PROTECTED_GRANT_REJECTED: " + reason);
}

std::string hex(const unsigned char* bytes, std::size_t size)
{
  const char* digits = "0123456789abcdef";
  std::string value;
  value.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    value += digits[bytes[i] >> 4]; value += digits[bytes[i] & 15];
  }
  return value;
}

std::string digest(const unsigned char* bytes, std::size_t size)
{
  unsigned char out[SHA256_DIGEST_LENGTH];
  SHA256(bytes, size, out);
  return "sha256:" + hex(out, sizeof(out));
}

std::string contextBytes(const NativeAssembledEntryContext& value)
{
  for (const auto* text : {&value.modelManifestDigest, &value.roleAssemblySpecDigest,
                           &value.storageProfileDigest}) {
    if (text->size() != 71 || text->substr(0, 7) != "sha256:" ||
        !std::all_of(text->begin() + 7, text->end(), [] (unsigned char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) throw rejected("assembled context digest is invalid");
  }
  if (value.entryKind != "MODEL_PROTO" && value.entryKind != "EXTERNAL_DATA") {
    throw rejected("assembled entry kind is invalid");
  }
  return "NDNSF-DI/assembled/v1" + value.modelManifestDigest +
         value.roleAssemblySpecDigest + value.storageProfileDigest;
}

Bytes hkdf(const Bytes& key, const std::string& info)
{
  Bytes out(32);
  auto* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  if (!ctx) throw rejected("HKDF allocation failed");
  std::size_t size = out.size();
  const bool ok = EVP_PKEY_derive_init(ctx) > 0 &&
    EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) > 0 &&
    EVP_PKEY_CTX_set1_hkdf_key(ctx, key.data(), key.size()) > 0 &&
    EVP_PKEY_CTX_add1_hkdf_info(ctx,
      reinterpret_cast<const unsigned char*>(info.data()), info.size()) > 0 &&
    EVP_PKEY_derive(ctx, out.data(), &size) > 0 && size == out.size();
  EVP_PKEY_CTX_free(ctx);
  if (!ok) {
    OPENSSL_cleanse(out.data(), out.size());
    throw rejected("assembled key derivation failed");
  }
  return out;
}

Bytes crypt(bool encrypt, const Bytes& key, const Bytes& nonce,
            const Bytes& input, const std::string& aad)
{
  if (key.size() != 32 || nonce.size() != 12 || input.size() > INT_MAX ||
      (!encrypt && input.size() < 16)) throw rejected("assembled AEAD input is invalid");
  const std::size_t body = encrypt ? input.size() : input.size() - 16;
  Bytes out(body + 16);
  auto* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) throw rejected("AEAD allocation failed");
  int written = 0, final = 0;
  bool ok = EVP_CipherInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
                              key.data(), nonce.data(), encrypt ? 1 : 0) == 1 &&
            EVP_CipherUpdate(ctx, nullptr, &written,
              reinterpret_cast<const unsigned char*>(aad.data()), aad.size()) == 1 &&
            EVP_CipherUpdate(ctx, out.data(), &written, input.data(), body) == 1;
  if (!encrypt) ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                                              const_cast<unsigned char*>(input.data() + body)) == 1;
  ok = ok && EVP_CipherFinal_ex(ctx, out.data() + written, &final) == 1;
  if (encrypt) ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16,
                                             out.data() + body) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    OPENSSL_cleanse(out.data(), out.size());
    throw rejected("assembled entry failed AEAD authentication");
  }
  out.resize(body + (encrypt ? 16 : 0));
  return out;
}

std::string manifest(const NativeAssembledEntryContext& value,
                     const std::string& aad, const Bytes& nonce,
                     const std::string& cipherDigest, std::uint64_t cipherLength)
{
  // Fields are fixed identifiers or validated lowercase digests, so no
  // untrusted text enters this canonical JSON serialization.
  return "{\"aead\":\"AES-256-GCM\",\"ciphertextDigest\":\"" +
    cipherDigest + "\",\"ciphertextLength\":" +
    std::to_string(cipherLength) + ",\"entryKind\":\"" + value.entryKind +
    "\",\"kdf\":\"HKDF-SHA256\",\"kdfContextDigest\":\"" +
    digest(reinterpret_cast<const unsigned char*>(aad.data()), aad.size()) +
    "\",\"modelManifestDigest\":\"" + value.modelManifestDigest +
    "\",\"nonce\":\"" + hex(nonce.data(), nonce.size()) +
    "\",\"roleAssemblySpecDigest\":\"" + value.roleAssemblySpecDigest +
    "\",\"schema\":\"ndnsf-di-assembled-ciphertext-v1\",\"storageProfileDigest\":\"" +
    value.storageProfileDigest + "\"}";
}

std::string manifest(const NativeAssembledEntryContext& value,
                     const std::string& aad, const Bytes& nonce,
                     const Bytes& cipher)
{
  return manifest(value, aad, nonce, digest(cipher.data(), cipher.size()), cipher.size());
}

std::string finalDigest(ndn::util::Sha256& hash)
{
  auto value = hash.toString();
  std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return "sha256:" + value;
}

std::filesystem::path temporaryPath(const std::filesystem::path& path,
                                     const char* suffix)
{
  return path.parent_path() /
    (path.filename().string() + suffix + std::to_string(::getpid()));
}

void removeFileSecurely(const std::filesystem::path& path) noexcept
{
  const int file = ::open(path.c_str(), O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (file >= 0) {
    struct stat opened{};
    if (::fstat(file, &opened) == 0 && S_ISREG(opened.st_mode)) {
      std::array<unsigned char, 65536> zero{};
      off_t offset = 0;
      while (offset < opened.st_size) {
        const auto count = ::pwrite(file, zero.data(),
          std::min<off_t>(zero.size(), opened.st_size - offset), offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        offset += count;
      }
      (void)::fsync(file);
    }
    ::close(file);
  }
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

struct TemporaryFiles
{
  std::vector<std::filesystem::path> paths;
  bool active = true;

  ~TemporaryFiles()
  {
    if (!active) return;
    for (const auto& path : paths)
      removeFileSecurely(path);
  }
};

void writeBytes(std::ofstream& output, const std::uint8_t* bytes, std::size_t size)
{
  if (size == 0) return;
  output.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
  if (!output.good()) throw rejected("protected artifact file write failed");
}

void readBytes(std::ifstream& input, std::uint8_t* bytes, std::size_t size)
{
  if (size == 0) return;
  input.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(size));
  if (input.gcount() != static_cast<std::streamsize>(size))
    throw rejected("protected artifact file read failed");
}

std::uint64_t decodeLength(const std::array<std::uint8_t, 8>& bytes)
{
  std::uint64_t value = 0;
  for (const auto byte : bytes)
    value = (value << 8) | byte;
  return value;
}

std::array<std::uint8_t, 8> encodeLength(std::uint64_t value)
{
  std::array<std::uint8_t, 8> bytes{};
  for (int index = 7; index >= 0; --index) {
    bytes[index] = static_cast<std::uint8_t>(value & 255);
    value >>= 8;
  }
  return bytes;
}

std::string nonceText(const boost::property_tree::ptree& fields)
{
  const auto value = fields.get<std::string>("nonce", "");
  if (value.size() != 24 || !std::all_of(value.begin(), value.end(), [] (char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      })) {
    throw rejected("assembled nonce is malformed");
  }
  return value;
}

Bytes decodeNonce(const std::string& text)
{
  Bytes nonce;
  for (std::size_t i = 0; i < text.size(); i += 2)
    nonce.push_back(std::stoul(text.substr(i, 2), nullptr, 16));
  return nonce;
}

void eraseDirectoryFd(int fd);

void eraseEntryFd(int fd, const std::string& name, bool ignoreMissing)
{
  struct stat st{};
  if (::fstatat(fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
    if (ignoreMissing && errno == ENOENT) return;
    throw rejected("plaintext entry stat failed");
  }
  if (S_ISDIR(st.st_mode)) {
    const int child = ::openat(fd, name.c_str(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) throw rejected("plaintext child directory open failed");
    try { eraseDirectoryFd(child); }
    catch (...) { ::close(child); throw; }
    ::close(child);
    if (::unlinkat(fd, name.c_str(), AT_REMOVEDIR) != 0)
      throw rejected("plaintext directory removal failed");
  }
  else {
    if (S_ISREG(st.st_mode)) {
      const int file = ::openat(fd, name.c_str(),
                                O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
      if (file < 0) throw rejected("plaintext file open failed");
      struct stat opened{};
      if (::fstat(file, &opened) != 0 || opened.st_ino != st.st_ino ||
          opened.st_dev != st.st_dev || !S_ISREG(opened.st_mode)) {
        ::close(file); throw rejected("plaintext allocation changed");
      }
      std::array<unsigned char, 65536> zero{};
      off_t offset = 0;
      while (offset < opened.st_size) {
        const auto count = ::pwrite(file, zero.data(),
          std::min<off_t>(zero.size(), opened.st_size - offset), offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ::close(file); throw rejected("plaintext overwrite failed"); }
        offset += count;
      }
      const bool flushed = ::fsync(file) == 0;
      ::close(file);
      if (!flushed) throw rejected("plaintext flush failed");
    }
    // Symlinks are removed as directory entries; their targets are never opened.
    if (::unlinkat(fd, name.c_str(), 0) != 0)
      throw rejected("plaintext unlink failed");
  }
}

void eraseDirectoryFd(int fd)
{
  const int copy = ::dup(fd);
  if (copy < 0) throw rejected("plaintext directory duplication failed");
  DIR* directory = ::fdopendir(copy);
  if (!directory) { ::close(copy); throw rejected("plaintext directory scan failed"); }
  std::string failure;
  while (auto* entry = ::readdir(directory)) {
    const std::string name(entry->d_name);
    if (name == "." || name == "..") continue;
    try {
      eraseEntryFd(fd, name, false);
    }
    catch (const std::exception& error) { failure = error.what(); }
  }
  ::closedir(directory);
  if (!failure.empty()) throw std::runtime_error(failure);
}

struct DirectoryLease
{
  int fd = -1;
  std::filesystem::path path;
  struct stat identity{};
  std::mutex mutex;
  ~DirectoryLease() { if (fd >= 0) ::close(fd); }
  void erase()
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (fd < 0) return;
    ::lseek(fd, 0, SEEK_SET);
    eraseDirectoryFd(fd);
    struct stat current{};
    if (::lstat(path.c_str(), &current) == 0) {
      if (current.st_ino != identity.st_ino || current.st_dev != identity.st_dev) {
        throw rejected("plaintext directory path was replaced");
      }
      if (::rmdir(path.c_str()) != 0) throw rejected("plaintext directory removal failed");
    }
    else if (errno != ENOENT) throw rejected("plaintext directory stat failed");
    ::close(fd); fd = -1;
  }

  void eraseFile(const std::filesystem::path& file)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (fd < 0) return;
    if (file.parent_path() != path || file.filename().empty() ||
        file.filename() == "." || file.filename() == ".." ||
        file.filename().string().find('/') != std::string::npos) {
      throw rejected("plaintext file is not a direct child of its lease");
    }
    eraseEntryFd(fd, file.filename().string(), true);
  }
};
} // namespace

NativePlaintextBufferGuard::~NativePlaintextBufferGuard()
{
  OPENSSL_cleanse(bytes.data(), bytes.size());
}

std::string nativeAssemblyDigestFromCanonicalProjection(const std::string& wire)
{
  std::size_t depth = 0;
  bool quoted = false, escaped = false;
  for (std::size_t i = 0; i < wire.size(); ++i) {
    const char c = wire[i];
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
      continue;
    }
    if (depth == 1 && wire.compare(i, 12, "\"assembly\":{") == 0) {
      const std::size_t start = i + 11;
      std::size_t nesting = 0;
      bool inString = false, slash = false;
      for (std::size_t j = start; j < wire.size(); ++j) {
        const char value = wire[j];
        if (inString) {
          if (slash) slash = false;
          else if (value == '\\') slash = true;
          else if (value == '"') inString = false;
        }
        else if (value == '"') inString = true;
        else if (std::isspace(static_cast<unsigned char>(value))) {
          throw rejected("protected Selection assembly must use canonical JSON");
        }
        else if (value == '{' || value == '[') ++nesting;
        else if (value == '}' || value == ']') {
          if (--nesting == 0) return digest(
            reinterpret_cast<const unsigned char*>(wire.data() + start), j - start + 1);
        }
      }
      break;
    }
    if (c == '"') quoted = true;
    else if (c == '{' || c == '[') ++depth;
    else if (c == '}' || c == ']') { if (depth) --depth; }
  }
  throw rejected("protected Selection lacks canonical assembly bytes");
}

std::vector<std::uint8_t> sealNativeAssembledEntry(
  const Bytes& contentKey, const Bytes& plaintext, const NativeAssembledEntryContext& context)
{
  if (contentKey.size() != 32 || plaintext.empty()) throw rejected("assembled key or plaintext is invalid");
  const auto aad = contextBytes(context);
  auto bundle = hkdf(contentKey, aad);
  NativePlaintextBufferGuard bundleGuard{bundle};
  auto key = hkdf(bundle, context.entryKind);
  NativePlaintextBufferGuard keyGuard{key};
  Bytes nonce(12);
  if (RAND_bytes(nonce.data(), nonce.size()) != 1) throw rejected("assembled nonce generation failed");
  auto cipher = crypt(true, key, nonce, plaintext, aad);
  const auto json = manifest(context, aad, nonce, cipher);
  Bytes wire(8);
  std::uint64_t length = json.size();
  for (int i = 7; i >= 0; --i) { wire[i] = length & 255; length >>= 8; }
  wire.insert(wire.end(), json.begin(), json.end());
  wire.insert(wire.end(), cipher.begin(), cipher.end());
  return wire;
}

std::vector<std::uint8_t> openNativeAssembledEntry(
  const Bytes& contentKey, const Bytes& wire, const NativeAssembledEntryContext& expected,
  std::uint64_t maxPlaintextBytes)
{
  if (contentKey.size() != 32 || wire.size() < 25 || maxPlaintextBytes == 0) {
    throw rejected("assembled ciphertext framing is invalid");
  }
  std::uint64_t length = 0;
  for (int i = 0; i < 8; ++i) length = (length << 8) | wire[i];
  if (length == 0 || length > 65536 || length > wire.size() - 24 ||
      wire.size() - 8 - length - 16 > maxPlaintextBytes) {
    throw rejected("assembled ciphertext exceeds framing or resource limit");
  }
  const auto aad = contextBytes(expected);
  const std::string json(wire.begin() + 8, wire.begin() + 8 + length);
  boost::property_tree::ptree fields;
  std::istringstream input(json);
  try { boost::property_tree::read_json(input, fields); }
  catch (...) { throw rejected("assembled manifest is malformed"); }
  const auto nonceText = fields.get<std::string>("nonce", "");
  if (nonceText.size() != 24 || !std::all_of(nonceText.begin(), nonceText.end(),
      [] (char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
    throw rejected("assembled nonce is malformed");
  }
  Bytes nonce;
  for (std::size_t i = 0; i < nonceText.size(); i += 2) {
    nonce.push_back(std::stoul(nonceText.substr(i, 2), nullptr, 16));
  }
  Bytes cipher(wire.begin() + 8 + length, wire.end());
  if (json != manifest(expected, aad, nonce, cipher)) {
    throw rejected("assembled authentication context or manifest mismatch");
  }
  auto bundle = hkdf(contentKey, aad);
  NativePlaintextBufferGuard bundleGuard{bundle};
  auto key = hkdf(bundle, expected.entryKind);
  NativePlaintextBufferGuard keyGuard{key};
  return crypt(false, key, nonce, cipher, aad);
}

std::string sealNativeAssembledEntryToFile(
  const Bytes& contentKey, const Bytes& plaintext,
  const std::filesystem::path& wirePath, const NativeAssembledEntryContext& context)
{
  if (contentKey.size() != 32 || plaintext.empty())
    throw rejected("assembled key or plaintext is invalid");
  const auto aad = contextBytes(context);
  auto bundle = hkdf(contentKey, aad);
  NativePlaintextBufferGuard bundleGuard{bundle};
  auto key = hkdf(bundle, context.entryKind);
  NativePlaintextBufferGuard keyGuard{key};
  Bytes nonce(12);
  if (RAND_bytes(nonce.data(), nonce.size()) != 1)
    throw rejected("assembled nonce generation failed");

  std::filesystem::create_directories(wirePath.parent_path());
  const auto bodyPath = temporaryPath(wirePath, ".body-tmp-");
  const auto wireTempPath = temporaryPath(wirePath, ".wire-tmp-");
  TemporaryFiles cleanup{{bodyPath, wireTempPath}};
  ndn::util::Sha256 cipherHash;
  std::uint64_t cipherLength = 0;
  {
    std::ofstream body(bodyPath, std::ios::binary | std::ios::trunc);
    if (!body.good()) throw rejected("protected artifact body open failed");
    auto cipher = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>(
      EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!cipher) throw rejected("AEAD allocation failed");
    int written = 0;
    if (EVP_CipherInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr,
                          key.data(), nonce.data(), 1) != 1 ||
        EVP_CipherUpdate(cipher.get(), nullptr, &written,
                         reinterpret_cast<const unsigned char*>(aad.data()),
                         static_cast<int>(aad.size())) != 1) {
      throw rejected("assembled encryption initialization failed");
    }
    Bytes output(StreamChunkBytes + GcmTagBytes);
    for (std::size_t offset = 0; offset < plaintext.size();) {
      const auto count = std::min(StreamChunkBytes, plaintext.size() - offset);
      int outputBytes = 0;
      if (EVP_CipherUpdate(cipher.get(), output.data(), &outputBytes,
                           plaintext.data() + offset, static_cast<int>(count)) != 1)
        throw rejected("assembled encryption failed");
      writeBytes(body, output.data(), static_cast<std::size_t>(outputBytes));
      if (outputBytes != 0) {
        cipherHash.update(ndn::span<const std::uint8_t>(output.data(), outputBytes));
        cipherLength += static_cast<std::uint64_t>(outputBytes);
      }
      offset += count;
    }
    int finalBytes = 0;
    if (EVP_CipherFinal_ex(cipher.get(), output.data(), &finalBytes) != 1)
      throw rejected("assembled encryption finalization failed");
    writeBytes(body, output.data(), static_cast<std::size_t>(finalBytes));
    if (finalBytes != 0) {
      cipherHash.update(ndn::span<const std::uint8_t>(output.data(), finalBytes));
      cipherLength += static_cast<std::uint64_t>(finalBytes);
    }
    std::array<std::uint8_t, GcmTagBytes> tag{};
    if (EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(tag.size()), tag.data()) != 1)
      throw rejected("assembled encryption tag failed");
    writeBytes(body, tag.data(), tag.size());
    cipherHash.update(ndn::span<const std::uint8_t>(tag.data(), tag.size()));
    cipherLength += tag.size();
    if (cipherLength != plaintext.size() + GcmTagBytes)
      throw rejected("assembled encryption size mismatch");
    body.flush();
    if (!body.good()) throw rejected("protected artifact body flush failed");
  }

  const auto cipherDigest = finalDigest(cipherHash);
  const auto manifestText = manifest(context, aad, nonce, cipherDigest, cipherLength);
  const auto header = encodeLength(manifestText.size());
  ndn::util::Sha256 wireHash;
  {
    std::ofstream output(wireTempPath, std::ios::binary | std::ios::trunc);
    if (!output.good()) throw rejected("protected artifact wire open failed");
    writeBytes(output, header.data(), header.size());
    wireHash.update(ndn::span<const std::uint8_t>(header.data(), header.size()));
    const auto* manifestBytes = reinterpret_cast<const std::uint8_t*>(manifestText.data());
    writeBytes(output, manifestBytes, manifestText.size());
    wireHash.update(ndn::span<const std::uint8_t>(manifestBytes, manifestText.size()));

    std::ifstream body(bodyPath, std::ios::binary);
    if (!body.good()) throw rejected("protected artifact body reopen failed");
    Bytes buffer(StreamChunkBytes);
    while (body) {
      body.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
      const auto count = static_cast<std::size_t>(body.gcount());
      if (count == 0) break;
      writeBytes(output, buffer.data(), count);
      wireHash.update(ndn::span<const std::uint8_t>(buffer.data(), count));
    }
    if (!body.eof()) throw rejected("protected artifact body copy failed");
    output.flush();
    if (!output.good()) throw rejected("protected artifact wire flush failed");
  }
  const auto wireDigest = finalDigest(wireHash);
  std::filesystem::rename(wireTempPath, wirePath);
  removeFileSecurely(bodyPath);
  cleanup.active = false;
  return wireDigest;
}

std::string openNativeAssembledEntryToFile(
  const Bytes& contentKey, const std::filesystem::path& wirePath,
  const std::filesystem::path& plaintextPath,
  const NativeAssembledEntryContext& expected, std::uint64_t maxPlaintextBytes,
  const std::string& expectedWireDigest)
{
  if (contentKey.size() != 32 || maxPlaintextBytes == 0)
    throw rejected("assembled key or plaintext limit is invalid");
  std::error_code fileError;
  const auto wireBytes = std::filesystem::file_size(wirePath, fileError);
  constexpr std::uint64_t framingOverhead = 8 + 65536 + GcmTagBytes;
  if (fileError || wireBytes < 8 + 1 + GcmTagBytes ||
      (maxPlaintextBytes > std::numeric_limits<std::uint64_t>::max() - framingOverhead) ||
      wireBytes > maxPlaintextBytes + framingOverhead)
    throw rejected("assembled ciphertext exceeds framing or resource limit");

  std::ifstream input(wirePath, std::ios::binary);
  if (!input.good()) throw rejected("assembled ciphertext file open failed");
  std::array<std::uint8_t, 8> header{};
  readBytes(input, header.data(), header.size());
  const auto manifestLength = decodeLength(header);
  if (manifestLength == 0 || manifestLength > 65536 ||
      wireBytes < header.size() + manifestLength + GcmTagBytes)
    throw rejected("assembled ciphertext framing is invalid");
  const auto cipherLength = wireBytes - header.size() - manifestLength;
  if (cipherLength < GcmTagBytes || cipherLength - GcmTagBytes > maxPlaintextBytes)
    throw rejected("assembled ciphertext exceeds framing or resource limit");
  std::string json(manifestLength, '\0');
  readBytes(input, reinterpret_cast<std::uint8_t*>(json.data()), json.size());
  boost::property_tree::ptree fields;
  std::istringstream manifestInput(json);
  try {
    boost::property_tree::read_json(manifestInput, fields);
  }
  catch (...) {
    throw rejected("assembled manifest is malformed");
  }
  const auto nonce = decodeNonce(nonceText(fields));
  const auto aad = contextBytes(expected);
  auto bundle = hkdf(contentKey, aad);
  NativePlaintextBufferGuard bundleGuard{bundle};
  auto key = hkdf(bundle, expected.entryKind);
  NativePlaintextBufferGuard keyGuard{key};

  std::filesystem::create_directories(plaintextPath.parent_path());
  const auto plaintextTempPath = temporaryPath(plaintextPath, ".tmp-");
  TemporaryFiles cleanup{{plaintextTempPath}};
  std::ofstream output(plaintextTempPath, std::ios::binary | std::ios::trunc);
  if (!output.good()) throw rejected("protected plaintext file open failed");

  auto cipher = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>(
    EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!cipher) throw rejected("AEAD allocation failed");
  int written = 0;
  if (EVP_CipherInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr,
                        key.data(), nonce.data(), 0) != 1 ||
      EVP_CipherUpdate(cipher.get(), nullptr, &written,
                       reinterpret_cast<const unsigned char*>(aad.data()),
                       static_cast<int>(aad.size())) != 1) {
    throw rejected("assembled decryption initialization failed");
  }

  ndn::util::Sha256 cipherHash;
  ndn::util::Sha256 wireHash;
  wireHash.update(ndn::span<const std::uint8_t>(header.data(), header.size()));
  wireHash.update(ndn::span<const std::uint8_t>(
    reinterpret_cast<const std::uint8_t*>(json.data()), json.size()));
  Bytes inputBuffer(StreamChunkBytes);
  Bytes outputBuffer(StreamChunkBytes + GcmTagBytes);
  std::uint64_t remaining = cipherLength - GcmTagBytes;
  std::uint64_t plaintextWritten = 0;
  while (remaining != 0) {
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
      remaining, inputBuffer.size()));
    readBytes(input, inputBuffer.data(), count);
    wireHash.update(ndn::span<const std::uint8_t>(inputBuffer.data(), count));
    cipherHash.update(ndn::span<const std::uint8_t>(inputBuffer.data(), count));
    int outputBytes = 0;
    if (EVP_CipherUpdate(cipher.get(), outputBuffer.data(), &outputBytes,
                         inputBuffer.data(), static_cast<int>(count)) != 1)
      throw rejected("assembled decryption failed");
    writeBytes(output, outputBuffer.data(), static_cast<std::size_t>(outputBytes));
    plaintextWritten += static_cast<std::uint64_t>(outputBytes);
    remaining -= count;
  }
  std::array<std::uint8_t, GcmTagBytes> tag{};
  readBytes(input, tag.data(), tag.size());
  wireHash.update(ndn::span<const std::uint8_t>(tag.data(), tag.size()));
  cipherHash.update(ndn::span<const std::uint8_t>(tag.data(), tag.size()));
  const auto cipherDigest = finalDigest(cipherHash);
  const auto expectedManifest = manifest(expected, aad, nonce, cipherDigest, cipherLength);
  if (json != expectedManifest)
    throw rejected("assembled authentication context or manifest mismatch");
  const auto wireDigest = finalDigest(wireHash);
  if (!expectedWireDigest.empty() && wireDigest != expectedWireDigest)
    throw rejected("assembled ciphertext digest mismatch");
  if (EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_TAG,
                          static_cast<int>(tag.size()), tag.data()) != 1)
    throw rejected("assembled decryption tag setup failed");
  int finalBytes = 0;
  if (EVP_CipherFinal_ex(cipher.get(), outputBuffer.data(), &finalBytes) != 1)
    throw rejected("assembled entry failed AEAD authentication");
  writeBytes(output, outputBuffer.data(), static_cast<std::size_t>(finalBytes));
  plaintextWritten += static_cast<std::uint64_t>(finalBytes);
  if (plaintextWritten != cipherLength - GcmTagBytes)
    throw rejected("assembled decryption size mismatch");
  output.flush();
  if (!output.good()) throw rejected("protected plaintext file flush failed");
  output.close();
  std::filesystem::rename(plaintextTempPath, plaintextPath);
  cleanup.active = false;
  return wireDigest;
}

NativePlaintextFileEraser registerNativePlaintextDirectoryWithFileEraser(
  ProtectedRuntime& runtime, const std::filesystem::path& directory, const std::string& leaseId)
{
  auto lease = std::make_shared<DirectoryLease>();
  lease->path = directory;
  lease->fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (lease->fd < 0 || ::fstat(lease->fd, &lease->identity) != 0 ||
      (lease->identity.st_mode & 077) != 0) {
    throw rejected("plaintext staging directory is not private");
  }
  runtime.registerHostPlaintextLease(leaseId, [lease] { lease->erase(); });
  return [lease] (const std::filesystem::path& file) { lease->eraseFile(file); };
}

void registerNativePlaintextDirectory(
  ProtectedRuntime& runtime, const std::filesystem::path& directory, const std::string& leaseId)
{
  (void)registerNativePlaintextDirectoryWithFileEraser(runtime, directory, leaseId);
}
} // namespace ndnsf::di
