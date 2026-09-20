#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedArtifactStore.hpp"
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
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf::di {
namespace {
using Bytes = std::vector<std::uint8_t>;

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
                     const std::string& aad, const Bytes& nonce, const Bytes& cipher)
{
  // Fields are fixed identifiers or validated lowercase digests, so no
  // untrusted text enters this canonical JSON serialization.
  return "{\"aead\":\"AES-256-GCM\",\"ciphertextDigest\":\"" +
    digest(cipher.data(), cipher.size()) + "\",\"ciphertextLength\":" +
    std::to_string(cipher.size()) + ",\"entryKind\":\"" + value.entryKind +
    "\",\"kdf\":\"HKDF-SHA256\",\"kdfContextDigest\":\"" +
    digest(reinterpret_cast<const unsigned char*>(aad.data()), aad.size()) +
    "\",\"modelManifestDigest\":\"" + value.modelManifestDigest +
    "\",\"nonce\":\"" + hex(nonce.data(), nonce.size()) +
    "\",\"roleAssemblySpecDigest\":\"" + value.roleAssemblySpecDigest +
    "\",\"schema\":\"ndnsf-di-assembled-ciphertext-v1\",\"storageProfileDigest\":\"" +
    value.storageProfileDigest + "\"}";
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
