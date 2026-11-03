#include "NDNSF-DistributedInference/cpp/ndnsf-di/protected-kv-checkpoint.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-private-checkpoint-file.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/private-buffer-wiper.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"

#include <ndn-cxx/security/transform/public-key.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace ndnsf::di {
namespace {
using Bytes = ndn::Buffer;
constexpr std::size_t CHUNK_BYTES = 64 * 1024;
constexpr std::uint64_t MAXIMUM_BYTES = 1000000000;
constexpr char MAGIC[] = "NDKVE001";

[[noreturn]] void invalid() { throw std::invalid_argument("protected KV checkpoint invalid"); }
void
require(bool value) { if (!value) invalid(); }
void
cancel(const NativeProtectedKvCheckpointLimitsV1& limits)
{
  if (limits.cancelled && limits.cancelled())
    throw std::runtime_error("protected KV checkpoint cancelled");
}
void
checkLimits(const NativeProtectedKvCheckpointLimitsV1& limits)
{
  require(limits.maxPlaintextBytes > 0 && limits.maxPlaintextBytes <= MAXIMUM_BYTES &&
          limits.maxWireBytes > 0 && limits.maxWireBytes <= MAXIMUM_BYTES);
  cancel(limits);
}
bool
lowerHex(const std::string& value)
{
  return std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
void
checkDigest(const std::string& value)
{
  require(value.size() == 71 && value.substr(0, 7) == "sha256:" && lowerHex(value.substr(7)));
}
std::string
hex(const std::uint8_t* data, std::size_t size)
{
  std::string result; result.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    result += "0123456789abcdef"[data[i] >> 4]; result += "0123456789abcdef"[data[i] & 15];
  }
  return result;
}
Bytes
unhex(const std::string& value)
{
  require(value.size() % 2 == 0 && lowerHex(value));
  auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  Bytes result(value.size() / 2);
  for (std::size_t i = 0; i < result.size(); ++i)
    result[i] = static_cast<std::uint8_t>((digit(value[2*i]) << 4) | digit(value[2*i+1]));
  return result;
}
struct Secret
{
  Bytes bytes;
  detail::PrivateBufferWiper wipe{bytes};
};
struct Hash
{
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
  Hash() { require(context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1); }
  void update(const std::uint8_t* data, std::size_t count)
  { require(EVP_DigestUpdate(context.get(), data, count) == 1); }
  std::string finish() {
    std::array<std::uint8_t, 32> bytes{}; unsigned size = 0;
    require(EVP_DigestFinal_ex(context.get(), bytes.data(), &size) == 1 && size == bytes.size());
    return "sha256:" + hex(bytes.data(), bytes.size());
  }
};
std::string
digest(const std::uint8_t* bytes, std::size_t size)
{ Hash hash; hash.update(bytes, size); return hash.finish(); }
std::string
nameText(const ndn::Name& name)
{
  const auto text = name.toUri(ndn::name::UriFormat::CANONICAL);
  require(!name.empty() && text.size() <= 4096 && name.wireEncode().size() <= 4096);
  return text;
}
ndn::Name checkpointName(const ndn::Name& producer, const std::string& wireDigest)
{
  auto name = ndn::Name(producer).append("kvcache").append("v1").append(wireDigest.substr(7));
  nameText(name); return name;
}
struct Input
{
  int fd = -1;
  struct stat info{};
  Input(const std::filesystem::path& path, const std::filesystem::path& output, std::uint64_t max) {
    fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) invalid();
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
        info.st_nlink != 1 || (info.st_mode & 077) != 0 || info.st_size <= 0 ||
        static_cast<std::uint64_t>(info.st_size) > max) {
      ::close(fd); fd = -1; invalid();
    }
    struct stat destination{};
    if (::lstat(output.c_str(), &destination) == 0 && destination.st_dev == info.st_dev &&
        destination.st_ino == info.st_ino) { ::close(fd); fd = -1; invalid(); }
  }
  ~Input() { if (fd >= 0) ::close(fd); }
  void rewind() { require(::lseek(fd, 0, SEEK_SET) == 0); }
  void eof() { std::uint8_t byte; require(::read(fd, &byte, 1) == 0); }
};
void
readAll(int fd, std::uint8_t* bytes, std::size_t size,
             const NativeProtectedKvCheckpointLimitsV1& limits)
{
  while (size) {
    cancel(limits); const auto n = ::read(fd, bytes, std::min(size, CHUNK_BYTES));
    if (n < 0 && errno == EINTR) continue;
    require(n > 0); bytes += n; size -= static_cast<std::size_t>(n);
  }
}
void
writeAll(int fd, const std::uint8_t* bytes, std::size_t size,
              const NativeProtectedKvCheckpointLimitsV1& limits)
{
  while (size) {
    cancel(limits); const auto n = ::write(fd, bytes, std::min(size, CHUNK_BYTES));
    if (n < 0 && errno == EINTR) continue;
    require(n > 0); bytes += n; size -= static_cast<std::size_t>(n);
  }
}
std::string
hashInput(Input& input, const NativeProtectedKvCheckpointLimitsV1& limits)
{
  input.rewind(); Hash hash; Secret buffer{Bytes(CHUNK_BYTES)};
  auto remaining = static_cast<std::uint64_t>(input.info.st_size);
  while (remaining) {
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, CHUNK_BYTES));
    readAll(input.fd, buffer.bytes.data(), count, limits); hash.update(buffer.bytes.data(), count);
    remaining -= count;
  }
  input.eof(); input.rewind(); return hash.finish();
}
void
checkPublicKey(ndn::span<const std::uint8_t> bytes)
{
  require(!bytes.empty() && bytes.size() <= 4096);
  ndn::security::transform::PublicKey key;
  try { key.loadPkcs8(bytes); }
  catch (...) { invalid(); }
  require(key.getKeyType() == ndn::KeyType::RSA && key.getKeySize() >= 2048 && key.getKeySize() <= 8192);
}
std::string
certificateDigest(const ndn::security::Certificate& certificate)
{
  nameText(certificate.getIdentity()); nameText(certificate.getName());
  const auto& wire = certificate.wireEncode(); require(wire.size() <= 16384);
  checkPublicKey(certificate.getPublicKey());
  return digest(wire.data(), wire.size());
}
void
checkReference(const NativeProtectedKvCheckpointReferenceV1& reference)
{
  require(!reference.keyEnvelope.empty() && reference.keyEnvelope.size() <= 16384);
  nameText(reference.ownerIdentity); nameText(reference.producerPrefix); nameText(reference.ownerCertificateName);
  checkDigest(reference.ownerCertificateDigest); checkDigest(reference.checkpointIdentityDigest);
  checkDigest(reference.wireDigest); require(reference.wireBytes > 0 && reference.wireBytes <= MAXIMUM_BYTES);
}
std::array<std::uint8_t, 16> prefix(std::uint64_t metadataSize)
{
  std::array<std::uint8_t, 16> result{}; std::copy_n(MAGIC, 8, result.begin());
  for (int i = 15; i >= 8; --i) { result[i] = metadataSize & 255; metadataSize >>= 8; }
  return result;
}
std::uint64_t
totalBytes(std::uint64_t plaintext, std::size_t metadata, std::uint64_t max)
{
  require(metadata > 0 && metadata <= 16384 && max >= 32 + metadata && plaintext <= max - 32 - metadata);
  return 32 + metadata + plaintext;
}
std::string
text(const NativeJson& object, const char* key, std::size_t max)
{
  const auto& value = object.at(key); require(value.is_string());
  const auto& result = value.get_ref<const std::string&>(); require(!result.empty() && result.size() <= max);
  return result;
}
std::uint64_t
number(const NativeJson& object, const char* key)
{ const auto& value = object.at(key); require(value.is_number_unsigned()); return value.get<std::uint64_t>(); }
using Cipher = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
Cipher
startCipher(bool encrypt, const Bytes& key, const Bytes& nonce, const std::string& metadata)
{
  require(key.size() == 32 && nonce.size() == 12);
  Cipher cipher(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free); int count = 0;
  require(cipher && EVP_CipherInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr,
      key.data(), nonce.data(), encrypt ? 1 : 0) == 1 &&
    EVP_CipherUpdate(cipher.get(), nullptr, &count,
      reinterpret_cast<const std::uint8_t*>(metadata.data()), static_cast<int>(metadata.size())) == 1);
  return cipher;
}
NativeJson
parseMetadata(const std::string& bytes)
{
  bool quoted = false, escaped = false; unsigned depth = 0;
  for (char c : bytes) {
    if (quoted) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; }
    else if (c == '"') quoted = true;
    else if (c == '{' || c == '[') { require(++depth <= 2); }
    else if (c == '}' || c == ']') { require(depth > 0); --depth; }
  }
  try {
    auto object = nativeParseJson(bytes);
    require(object.is_object() && object.size() == 12 && nativeCanonicalJson(object) == bytes);
    for (const auto* key : {"version", "algorithm", "owner", "producer", "checkpointIdentityDigest",
         "ownerCertificateName", "ownerCertificateDigest", "plaintextBytes", "plaintextDigest",
         "nonce", "wrappedKey", "wrappedKeyDigest"}) require(object.contains(key));
    return object;
  } catch (...) { invalid(); }
}

struct ProtectedHeader
{
  std::array<std::uint8_t, 16> prefix{};
  std::string metadata;
  std::uint64_t plaintextBytes = 0;
  std::string plaintextDigest;
  Bytes nonce, wrapped;
};
ProtectedHeader
parseHeaderMetadata(const std::string& metadata,
  const NativeProtectedKvCheckpointReferenceV1& expected,
  const NativeProtectedKvCheckpointLimitsV1& limits)
{
  ProtectedHeader result;
  require(!metadata.empty() && metadata.size() <= 16384 && expected.wireBytes >= 32 + metadata.size() &&
          expected.keyEnvelope == metadata);
  result.metadata = metadata;
  result.prefix = prefix(metadata.size());
  const auto object = parseMetadata(result.metadata);
  require(number(object, "version") == 1 && text(object, "algorithm", 16) == "AES-256-GCM" &&
    text(object, "owner", 4096) == nameText(expected.ownerIdentity) && text(object, "producer", 4096) == nameText(expected.producerPrefix) &&
    text(object, "ownerCertificateName", 4096) == nameText(expected.ownerCertificateName) &&
    text(object, "ownerCertificateDigest", 71) == expected.ownerCertificateDigest &&
    text(object, "checkpointIdentityDigest", 71) == expected.checkpointIdentityDigest);
  result.plaintextBytes = number(object, "plaintextBytes");
  require(result.plaintextBytes > 0 && result.plaintextBytes <= limits.maxPlaintextBytes &&
          totalBytes(result.plaintextBytes, result.metadata.size(), limits.maxWireBytes) == expected.wireBytes);
  result.plaintextDigest = text(object, "plaintextDigest", 71); checkDigest(result.plaintextDigest);
  const auto nonce = text(object, "nonce", 24); require(nonce.size() == 24); result.nonce = unhex(nonce);
  const auto wrapped = text(object, "wrappedKey", 2048); require(wrapped.size() >= 512);
  result.wrapped = unhex(wrapped); require(result.wrapped.size() >= 256 && result.wrapped.size() <= 1024);
  require(text(object, "wrappedKeyDigest", 71) == digest(result.wrapped.data(), result.wrapped.size()));
  return result;
}
ProtectedHeader
readHeader(Input& input, const NativeProtectedKvCheckpointReferenceV1& expected,
                           const NativeProtectedKvCheckpointLimitsV1& limits)
{
  std::array<std::uint8_t, 16> framing{};
  readAll(input.fd, framing.data(), framing.size(), limits);
  require(std::equal(framing.begin(), framing.begin() + 8, MAGIC));
  std::uint64_t size = 0; for (unsigned i = 8; i < 16; ++i) size = (size << 8) | framing[i];
  require(size > 0 && size <= 16384 && expected.wireBytes >= 32 + size);
  std::string metadata(static_cast<std::size_t>(size), '\0');
  readAll(input.fd, reinterpret_cast<std::uint8_t*>(metadata.data()), metadata.size(), limits);
  return parseHeaderMetadata(metadata, expected, limits);
}

std::string
recipientAssociatedData(const NativeProtectedKvCheckpointReferenceV1& reference,
                                  const NativeProtectedKvRecipientBindingV1& binding)
{
  nativeValidateProtectedKvKeyEnvelope(reference);
  nameText(binding.requesterIdentity); nameText(binding.recipientIdentity);
  nameText(binding.serviceName); nameText(binding.requestId);
  const auto certificateName = nameText(binding.recipientCertificateName);
  checkDigest(binding.planDigest); checkDigest(binding.recipientPublicKeyDigest);
  require(binding.requesterIdentity == reference.ownerIdentity &&
    ndn::Name(binding.recipientIdentity).append("KEY").isPrefixOf(binding.recipientCertificateName) &&
    binding.recipientCertificateName.size() > binding.recipientIdentity.size() + 1 &&
    !binding.authorizationBinding.empty() && binding.authorizationBinding.size() <= 4096 &&
    !binding.roleName.empty() && binding.roleName.size() <= 4096 &&
    binding.attemptEpoch >= 1 && binding.attemptEpoch <= 2 && binding.expiresAtMs > 0);
  const auto core = ndn_service_framework::recipientAssignmentAssociatedData(
    binding.requesterIdentity, binding.recipientIdentity, binding.serviceName, binding.requestId,
    binding.authorizationBinding, binding.planDigest);
  return nativeCanonicalJson(NativeJson{
    {"version", 1U}, {"purpose", "NDNSF-DI/KV-RECIPIENT-KEY/V1"},
    {"coreAssignmentDigest", digest(core.data(), core.size())},
    {"recipientCertificateName", certificateName}, {"recipientPublicKeyDigest", binding.recipientPublicKeyDigest},
    {"roleName", binding.roleName}, {"attemptEpoch", binding.attemptEpoch}, {"expiresAtMs", binding.expiresAtMs},
    {"wireDigest", reference.wireDigest}, {"wireBytes", reference.wireBytes},
    {"checkpointIdentityDigest", reference.checkpointIdentityDigest},
    {"ownerCertificateDigest", reference.ownerCertificateDigest},
    {"keyEnvelopeDigest", digest(reinterpret_cast<const std::uint8_t*>(reference.keyEnvelope.data()), reference.keyEnvelope.size())}});
}

void
checkCapsule(const ndn_service_framework::RecipientEncryptedAssignment& capsule,
                  const NativeProtectedKvRecipientBindingV1& binding)
{
  const std::array<const char*, 10> fields{{"schemaVersion", "algorithm", "recipient", "recipientCertName",
    "wrappedAssignmentKey", "nonce", "ciphertext", "authTag", "aadDigest", "assignmentDigest"}};
  require(capsule.getVersion() == 1 && capsule.getFields().size() == fields.size());
  for (const auto field : fields) require(capsule.hasField(field));
  require(capsule.getField("schemaVersion") == "1" &&
    capsule.getField("algorithm") == "RSA-WRAPPED-AES-256-GCM" &&
    capsule.getField("recipient") == nameText(binding.recipientIdentity) &&
    capsule.getField("recipientCertName") == nameText(binding.recipientCertificateName));
  const auto hexField = [&](const char* field, std::size_t size) {
    const auto& value = capsule.getField(field); require(value.size() == size && lowerHex(value));
  };
  hexField("nonce", 24); hexField("ciphertext", 64); hexField("authTag", 32);
  hexField("aadDigest", 64); hexField("assignmentDigest", 64);
  const auto& wrapped = capsule.getField("wrappedAssignmentKey");
  require(wrapped.size() >= 512 && wrapped.size() <= 2048 && wrapped.size() % 2 == 0 && lowerHex(wrapped));
}

void
recoverProtectedFile(const std::filesystem::path& protectedInput, const std::filesystem::path& privateOutput,
  const NativeProtectedKvCheckpointReferenceV1& expected, const NativeProtectedKvCheckpointLimitsV1& limits,
  const NativeCheckpointExportOptions& options, const std::function<Bytes(const ProtectedHeader&)>& unwrap)
{
  Input input(protectedInput, privateOutput, limits.maxWireBytes);
  require(static_cast<std::uint64_t>(input.info.st_size) == expected.wireBytes && hashInput(input, limits) == expected.wireDigest);
  const auto parsed = readHeader(input, expected, limits);
  const auto& header = parsed.prefix; const auto& metadata = parsed.metadata;
  const auto plaintextBytes = parsed.plaintextBytes; const auto& plaintextDigest = parsed.plaintextDigest;
  const auto& nonce = parsed.nonce;
  cancel(limits); Secret key{unwrap(parsed)}; require(key.bytes.size() == 32); cancel(limits);
  detail::writePrivateCheckpointFile(privateOutput, options, [&](int output) {
    auto cipher = startCipher(false, key.bytes, nonce, metadata); Hash wireHash, plainHash;
    wireHash.update(header.data(), header.size()); wireHash.update(reinterpret_cast<const std::uint8_t*>(metadata.data()), metadata.size());
    Secret buffer{Bytes(CHUNK_BYTES)}, plaintext{Bytes(CHUNK_BYTES + 16)};
    auto remaining = plaintextBytes; std::uint64_t emitted = 0;
    while (remaining) {
      const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, CHUNK_BYTES));
      readAll(input.fd, buffer.bytes.data(), count, limits); wireHash.update(buffer.bytes.data(), count);
      int written = 0;
      require(EVP_CipherUpdate(cipher.get(), plaintext.bytes.data(), &written,
        buffer.bytes.data(), static_cast<int>(count)) == 1 && written >= 0 && written <= static_cast<int>(CHUNK_BYTES + 16));
      writeAll(output, plaintext.bytes.data(), written, limits); plainHash.update(plaintext.bytes.data(), written);
      emitted += written; remaining -= count;
    }
    std::array<std::uint8_t, 16> tag{}; readAll(input.fd, tag.data(), tag.size(), limits); input.eof(); wireHash.update(tag.data(), tag.size());
    require(wireHash.finish() == expected.wireDigest && EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_TAG, tag.size(), tag.data()) == 1);
    int final = 0; require(EVP_CipherFinal_ex(cipher.get(), plaintext.bytes.data(), &final) == 1 && final >= 0 && final <= 16);
    writeAll(output, plaintext.bytes.data(), final, limits); plainHash.update(plaintext.bytes.data(), final); emitted += final;
    require(emitted == plaintextBytes && plainHash.finish() == plaintextDigest); cancel(limits);
  }, limits.cancelled);
}
} // namespace

void
nativeValidateProtectedKvKeyEnvelope(const NativeProtectedKvCheckpointReferenceV1& reference)
{
  checkReference(reference);
  require(!reference.keyEnvelope.empty() && reference.ownerIdentity.isPrefixOf(reference.ownerCertificateName) &&
          reference.ownerCertificateName.size() > reference.ownerIdentity.size());
  (void)parseHeaderMetadata(reference.keyEnvelope, reference, {MAXIMUM_BYTES, MAXIMUM_BYTES, {}});
}

void
nativeValidateProtectedKvRecipientCapsule(
  const ndn_service_framework::RecipientEncryptedAssignment& capsule,
  const NativeProtectedKvRecipientBindingV1& binding)
{
  checkCapsule(capsule, binding);
}

void
nativeValidateProtectedKvRecipientCapsule(
  const ndn_service_framework::RecipientEncryptedAssignment& capsule,
  const NativeProtectedKvCheckpointReferenceV1& reference,
  const NativeProtectedKvRecipientBindingV1& binding)
{
  checkCapsule(capsule, binding);
  const auto ad = recipientAssociatedData(reference, binding);
  require(capsule.getField("aadDigest") == digest(
    reinterpret_cast<const std::uint8_t*>(ad.data()), ad.size()).substr(7));
}

ndn::Name
NativeProtectedKvCheckpointReferenceV1::objectName() const
{
  checkReference(*this); return checkpointName(producerPrefix, wireDigest);
}

NativeProtectedKvCheckpointReferenceV1
nativeProtectPortableKvCheckpoint(
  const std::filesystem::path& privateInput, const std::filesystem::path& protectedOutput,
  const ndn::security::Certificate& certificate, const ndn::Name& producer,
  const std::string& identityDigest, const NativeProtectedKvCheckpointLimitsV1& limits,
  const NativeCheckpointExportOptions& options)
{
  checkLimits(limits); nameText(producer); checkDigest(identityDigest);
  // Every digest component has the same fixed width; reject oversized final names before writing.
  checkpointName(producer, identityDigest);
  const auto certDigest = certificateDigest(certificate);
  Input input(privateInput, protectedOutput, limits.maxPlaintextBytes);
  const auto plaintextBytes = static_cast<std::uint64_t>(input.info.st_size);
  const auto plaintextDigest = hashInput(input, limits);
  Secret key{Bytes(32)}; Bytes nonce(12);
  require(RAND_bytes(key.bytes.data(), key.bytes.size()) == 1 && RAND_bytes(nonce.data(), nonce.size()) == 1);
  Bytes wrapped;
  try { wrapped = ndn_service_framework::wrapSelectionGatedInputKey(key.bytes, certificate.getPublicKey()); }
  catch (...) { invalid(); }
  require(wrapped.size() >= 256 && wrapped.size() <= 1024);
  const auto metadata = nativeCanonicalJson(NativeJson{
    {"version", 1U}, {"algorithm", "AES-256-GCM"}, {"owner", nameText(certificate.getIdentity())},
    {"producer", nameText(producer)}, {"checkpointIdentityDigest", identityDigest},
    {"ownerCertificateName", nameText(certificate.getName())}, {"ownerCertificateDigest", certDigest},
    {"plaintextBytes", plaintextBytes}, {"plaintextDigest", plaintextDigest},
    {"nonce", hex(nonce.data(), nonce.size())}, {"wrappedKey", hex(wrapped.data(), wrapped.size())},
    {"wrappedKeyDigest", digest(wrapped.data(), wrapped.size())}});
  const auto wireBytes = totalBytes(plaintextBytes, metadata.size(), limits.maxWireBytes);
  const auto header = prefix(metadata.size()); std::string wireDigest;
  cancel(limits);
  detail::writePrivateCheckpointFile(protectedOutput, options, [&](int output) {
    auto cipher = startCipher(true, key.bytes, nonce, metadata); Hash wireHash, plainHash;
    auto emit = [&](const std::uint8_t* bytes, std::size_t size) {
      writeAll(output, bytes, size, limits); wireHash.update(bytes, size);
    };
    emit(header.data(), header.size()); emit(reinterpret_cast<const std::uint8_t*>(metadata.data()), metadata.size());
    Secret buffer{Bytes(CHUNK_BYTES)}, encrypted{Bytes(CHUNK_BYTES + 16)};
    auto remaining = plaintextBytes; std::uint64_t emitted = 0;
    while (remaining) {
      const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, CHUNK_BYTES));
      readAll(input.fd, buffer.bytes.data(), count, limits); plainHash.update(buffer.bytes.data(), count);
      int written = 0;
      require(EVP_CipherUpdate(cipher.get(), encrypted.bytes.data(), &written,
        buffer.bytes.data(), static_cast<int>(count)) == 1 && written >= 0 && written <= static_cast<int>(CHUNK_BYTES + 16));
      emit(encrypted.bytes.data(), written); emitted += written; remaining -= count;
    }
    input.eof(); require(plainHash.finish() == plaintextDigest);
    int final = 0; require(EVP_CipherFinal_ex(cipher.get(), encrypted.bytes.data(), &final) == 1 && final >= 0 && final <= 16);
    emit(encrypted.bytes.data(), final); emitted += final; require(emitted == plaintextBytes);
    std::array<std::uint8_t, 16> tag{};
    require(EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) == 1);
    emit(tag.data(), tag.size()); wireDigest = wireHash.finish(); cancel(limits);
  }, limits.cancelled);
  return {certificate.getIdentity(), producer, certificate.getName(), certDigest, identityDigest, wireDigest, wireBytes, metadata};
}

ndn::Buffer
nativeLoadProtectedPortableKvCheckpoint(
  const std::filesystem::path& protectedInput, const NativeProtectedKvCheckpointReferenceV1& expected,
  const NativeProtectedKvCheckpointLimitsV1& limits)
{
  checkLimits(limits); checkReference(expected);
  Input input(protectedInput, {}, limits.maxWireBytes);
  require(static_cast<std::uint64_t>(input.info.st_size) == expected.wireBytes && expected.wireBytes <= Bytes{}.max_size());
  (void)readHeader(input, expected, limits); input.rewind(); cancel(limits);
  Bytes result(static_cast<std::size_t>(expected.wireBytes));
  readAll(input.fd, result.data(), result.size(), limits); input.eof();
  require(digest(result.data(), result.size()) == expected.wireDigest); cancel(limits);
  return result;
}

void
nativeValidateProtectedKvCheckpointWire(ndn::span<const std::uint8_t> wire,
  const NativeProtectedKvCheckpointReferenceV1& expected,
  const NativeProtectedKvCheckpointLimitsV1& limits)
{
  checkLimits(limits); checkReference(expected); cancel(limits);
  require(wire.size() == expected.wireBytes && wire.size() <= limits.maxWireBytes);
  const auto header = parseHeaderMetadata(expected.keyEnvelope, expected, limits);
  require(std::equal(header.prefix.begin(), header.prefix.end(), wire.begin()) &&
    std::equal(header.metadata.begin(), header.metadata.end(), wire.begin() + 16));
  require(digest(wire.data(), wire.size()) == expected.wireDigest);
  cancel(limits);
}

void
nativeRecoverProtectedPortableKvCheckpoint(
  const std::filesystem::path& protectedInput, const std::filesystem::path& privateOutput,
  const NativeProtectedKvCheckpointReferenceV1& expected, const ndn::security::Certificate& certificate,
  const ndn::security::KeyChain& keyChain, const NativeProtectedKvCheckpointLimitsV1& limits,
  const NativeCheckpointExportOptions& options)
{
  checkLimits(limits); checkReference(expected);
  require(expected.ownerIdentity == certificate.getIdentity() &&
    expected.ownerCertificateName == certificate.getName() &&
    expected.ownerCertificateDigest == certificateDigest(certificate));
  nativeRecoverProtectedPortableKvCheckpoint(protectedInput, privateOutput, expected,
    [&](const ndn::Buffer& wrapped) {
      return ndn_service_framework::unwrapSelectionGatedInputKey(wrapped, certificate.getName(),
        keyChain);
    }, limits, options);
}

void
nativeRecoverProtectedPortableKvCheckpoint(
  const std::filesystem::path& protectedInput, const std::filesystem::path& privateOutput,
  const NativeProtectedKvCheckpointReferenceV1& expected,
  const std::function<ndn::Buffer(const ndn::Buffer&)>& unwrap,
  const NativeProtectedKvCheckpointLimitsV1& limits,
  const NativeCheckpointExportOptions& options)
{
  checkLimits(limits); checkReference(expected);
  require(static_cast<bool>(unwrap));
  recoverProtectedFile(protectedInput, privateOutput, expected, limits, options,
    [&](const ProtectedHeader& header) -> Bytes {
      try { return unwrap(header.wrapped); }
      catch (...) { invalid(); }
  });
}

ndn_service_framework::RecipientEncryptedAssignment
nativeWrapProtectedKvKeyForRecipient(
  const NativeProtectedKvCheckpointReferenceV1& reference,
  const ndn::security::Certificate& ownerCertificate, const ndn::security::KeyChain& ownerKeyChain,
  ndn::span<const std::uint8_t> recipientPublicKey, const NativeProtectedKvRecipientBindingV1& binding,
  const std::function<void()>& requireCurrent)
{
  require(static_cast<bool>(requireCurrent)); requireCurrent();
  const auto ad = recipientAssociatedData(reference, binding);
  require(reference.ownerIdentity == ownerCertificate.getIdentity() &&
    reference.ownerCertificateName == ownerCertificate.getName() &&
    reference.ownerCertificateDigest == certificateDigest(ownerCertificate));
  checkPublicKey(recipientPublicKey);
  require(digest(recipientPublicKey.data(), recipientPublicKey.size()) == binding.recipientPublicKeyDigest);
  const auto header = parseHeaderMetadata(reference.keyEnvelope, reference, {MAXIMUM_BYTES, MAXIMUM_BYTES, {}});
  Secret key;
  try { key.bytes = ndn_service_framework::unwrapSelectionGatedInputKey(header.wrapped, ownerCertificate.getName(), ownerKeyChain); }
  catch (...) { invalid(); }
  require(key.bytes.size() == 32); requireCurrent();
  auto result = ndn_service_framework::encryptRecipientAssignment(key.bytes, recipientPublicKey,
    binding.recipientIdentity, binding.recipientCertificateName,
    ndn::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(ad.data()), ad.size()));
  requireCurrent(); return result;
}

void
nativeRecoverProtectedPortableKvCheckpointForRecipient(
  const std::filesystem::path& protectedInput, const std::filesystem::path& privateOutput,
  const NativeProtectedKvCheckpointReferenceV1& reference,
  const ndn_service_framework::RecipientEncryptedAssignment& capsule,
  const NativeProtectedKvRecipientBindingV1& binding, const NativeProtectedKvCheckpointLimitsV1& limits,
  const NativeProtectedKvRecipientConsumerV1& recipientConsumer,
  const std::function<void()>& requireCurrent, const NativeCheckpointExportOptions& options)
{
  require(static_cast<bool>(requireCurrent) && static_cast<bool>(recipientConsumer));
  requireCurrent(); checkLimits(limits);
  const auto ad = recipientAssociatedData(reference, binding);
  nativeValidateProtectedKvRecipientCapsule(capsule, reference, binding);
  const ndn::Buffer associatedData(reinterpret_cast<const std::uint8_t*>(ad.data()), ad.size());
  auto bounded = limits;
  bounded.cancelled = [requireCurrent, cancelled = limits.cancelled] {
    requireCurrent(); return cancelled && cancelled();
  };
  auto guarded = options;
  guarded.beforeStage = [requireCurrent, observer = options.beforeStage](NativeCheckpointExportStage stage) {
    requireCurrent(); if (observer) observer(stage); requireCurrent();
  };
  recoverProtectedFile(protectedInput, privateOutput, reference, bounded, guarded,
    [&](const ProtectedHeader&) -> Bytes {
      requireCurrent(); Secret key;
      const auto worker = std::this_thread::get_id();
      std::atomic<std::size_t> calls{0};
      bool complete = false;
      const bool accepted = recipientConsumer(capsule, binding.recipientCertificateName,
        binding.recipientPublicKeyDigest, associatedData, [&](const ndn::Buffer& plaintext) {
          const auto call = calls.fetch_add(1);
          // Even a caught repeated/wrong-thread exception permanently rejects
          // this operation before the private writer can start.
          require(call == 0 && std::this_thread::get_id() == worker);
          requireCurrent(); require(plaintext.size() == 32);
          key.bytes = plaintext;
          requireCurrent(); complete = true;
        });
      require(accepted && calls.load() == 1 && complete && key.bytes.size() == 32);
      requireCurrent(); return std::move(key.bytes);
    });
}

void
nativeRecoverProtectedPortableKvCheckpointForRecipient(
  const std::filesystem::path& protectedInput, const std::filesystem::path& privateOutput,
  const NativeProtectedKvCheckpointReferenceV1& reference,
  const ndn_service_framework::RecipientEncryptedAssignment& capsule,
  const ndn::security::Certificate& recipientCertificate, const ndn::security::KeyChain& recipientKeyChain,
  const NativeProtectedKvRecipientBindingV1& binding, const NativeProtectedKvCheckpointLimitsV1& limits,
  const std::function<void()>& requireCurrent, const NativeCheckpointExportOptions& options)
{
  require(static_cast<bool>(requireCurrent)); requireCurrent(); checkLimits(limits);
  (void)certificateDigest(recipientCertificate);
  const auto publicKey = recipientCertificate.getPublicKey();
  require(recipientCertificate.getIdentity() == binding.recipientIdentity &&
    recipientCertificate.getName() == binding.recipientCertificateName &&
    digest(publicKey.data(), publicKey.size()) == binding.recipientPublicKeyDigest);
  NativeProtectedKvRecipientConsumerV1 consumer = [&](const auto& encrypted,
    const ndn::Name& certificateName, const std::string& publicKeyDigest,
    const ndn::Buffer& associatedData, const auto& consume) {
      require(certificateName == recipientCertificate.getName() &&
        publicKeyDigest == binding.recipientPublicKeyDigest);
      Secret plaintext;
      try {
        require(ndn_service_framework::decryptRecipientAssignment(encrypted,
          binding.recipientIdentity, certificateName, recipientKeyChain,
          associatedData, plaintext.bytes));
      }
      catch (...) { invalid(); }
      consume(plaintext.bytes);
      return true;
    };
  nativeRecoverProtectedPortableKvCheckpointForRecipient(protectedInput, privateOutput,
    reference, capsule, binding, limits, consumer, requireCurrent, options);
}
} // namespace ndnsf::di
