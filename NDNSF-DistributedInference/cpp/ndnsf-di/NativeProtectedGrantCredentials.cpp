#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include <boost/property_tree/json_parser.hpp>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf::di {
namespace {
std::runtime_error reject(const std::string& message)
{
  return std::runtime_error("DI_PROTECTED_GRANT_REJECTED: " + message);
}
std::string env(const char* name)
{
  const char* value = std::getenv(name);
  if (!value || !*value) throw reject(std::string("missing operator setting ") + name);
  return value;
}
std::string digest(const std::string& bytes)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), hash);
  std::string out = "sha256:";
  for (const auto c : hash) {
    out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15];
  }
  return out;
}
std::string readBounded(const std::filesystem::path& path, bool privateKey = false)
{
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) throw reject("operator file cannot be opened");
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      st.st_size > 1024 * 1024 || (privateKey && (st.st_mode & 0777) != 0600)) {
    ::close(fd); throw reject("operator file type, size or permissions are invalid");
  }
  std::string value(st.st_size, '\0');
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto count = ::read(fd, value.data() + offset, value.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      ::close(fd); OPENSSL_cleanse(value.data(), value.size());
      throw reject("operator file is truncated");
    }
    offset += count;
  }
  ::close(fd);
  return value;
}
boost::property_tree::ptree json(const std::string& text)
{
  boost::property_tree::ptree value;
  std::istringstream input(text);
  boost::property_tree::read_json(input, value);
  return value;
}
bool contains(const boost::property_tree::ptree& policy,
              const std::string& field, const std::string& value)
{
  const auto array = policy.get_child_optional(field);
  return array && std::any_of(array->begin(), array->end(), [&] (const auto& entry) {
    return entry.first.empty() && entry.second.template get_value<std::string>() == value;
  });
}
std::string rawPublicKey(const std::string& pem)
{
  BIO* bio = BIO_new_mem_buf(pem.data(), pem.size());
  EVP_PKEY* key = bio ? PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr) : nullptr;
  if (bio) BIO_free(bio);
  std::string raw(32, '\0');
  std::size_t size = raw.size();
  const bool ok = key && EVP_PKEY_id(key) == EVP_PKEY_ED25519 &&
    EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char*>(raw.data()), &size) == 1 &&
    size == raw.size();
  if (key) EVP_PKEY_free(key);
  if (!ok) throw reject("registry authority key is not Ed25519");
  return raw;
}
NativeProtectedGrantConfig credentials(const std::string& provider,
                                      const std::string& modelFamily,
                                      const std::string& epoch)
{
  const auto publicHint = std::filesystem::path(env("SPEC181_GRANT_AUTHORITY_PUBLIC_KEY"));
  const auto registry = publicHint.parent_path() / "trust-root-registry-v1.json";
  const auto document = json(readBounded(registry));
  if (document.get<int>("schemaVersion", 0) != 1 ||
      document.get<std::string>("status", "") != "CONFIGURED") {
    throw reject("authority registry is not configured");
  }
  const auto& policy = document.get_child("artifactPolicyAuthority");
  if (policy.get<std::string>("publicKeyAlgorithm", "") != "ed25519" ||
      policy.get<std::string>("signatureAlgorithm", "") != "ed25519" ||
      policy.get<std::string>("grantSchema", "") != "ndnsf-di-key-grant-v1" ||
      !contains(policy, "acceptedModelFamilies", modelFamily) ||
      !contains(policy, "protectionEpochs", epoch)) {
    throw reject("registry algorithm or model/epoch policy rejected");
  }
  const std::filesystem::path relative(policy.get<std::string>("publicKeyPath", ""));
  if (relative.empty() || relative.is_absolute() ||
      std::find(relative.begin(), relative.end(), "..") != relative.end()) {
    throw reject("registry public key path is unsafe");
  }
  const auto pem = readBounded(registry.parent_path().parent_path() / relative);
  if (digest(pem) != policy.get<std::string>("publicKeySha256", "")) {
    throw reject("registry authority public key digest mismatch");
  }
  NativeProtectedGrantConfig out;
  out.authorityIdentity = policy.get<std::string>("authorityId", "");
  if (out.authorityIdentity.empty() || policy.get<std::string>("keyId", "").empty()) {
    throw reject("registry issuer identity is missing");
  }
  out.authorityPublicKeyRaw = rawPublicKey(pem);
  const auto mapping = json(readBounded(env("SPEC181_PROVIDER_RECIPIENT_KEY_MAP")));
  std::string recipientPath;
  for (const auto& entry : mapping) {
    if (entry.first == provider) recipientPath = entry.second.get_value<std::string>();
  }
  if (recipientPath.empty()) throw reject("provider recipient identity has no configured key");
  auto secret = readBounded(recipientPath, true);
  struct CleansePem {
    std::string& value;
    ~CleansePem() { OPENSSL_cleanse(value.data(), value.size()); }
  } cleanse{secret};
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
    BIO_new_mem_buf(secret.data(), secret.size()), BIO_free);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
    bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) : nullptr,
    EVP_PKEY_free);
  if (!key) throw reject("provider recipient private key cannot be parsed");
  if (EVP_PKEY_id(key.get()) == EVP_PKEY_ED25519) {
    out.recipientKey.kind = NativeRecipientKey::Kind::Ed25519Seed;
    out.recipientKey.material.resize(32);
    std::size_t size = 32;
    if (EVP_PKEY_get_raw_private_key(key.get(),
          reinterpret_cast<unsigned char*>(out.recipientKey.material.data()), &size) != 1 ||
        size != 32) {
      OPENSSL_cleanse(out.recipientKey.material.data(), out.recipientKey.material.size());
      throw reject("provider Ed25519 private seed cannot be extracted");
    }
  }
  else if (EVP_PKEY_id(key.get()) == EVP_PKEY_EC) {
    std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)> ec(
      EVP_PKEY_get1_EC_KEY(key.get()), EC_KEY_free);
    const auto* group = ec ? EC_KEY_get0_group(ec.get()) : nullptr;
    if (!group || EC_GROUP_get_curve_name(group) != NID_X9_62_prime256v1 ||
        EC_KEY_check_key(ec.get()) != 1) {
      throw reject("provider EC recipient private key must use P-256");
    }
    out.recipientKey.kind = NativeRecipientKey::Kind::EcP256Pem;
    out.recipientKey.material = std::move(secret);
  }
  else {
    throw reject("provider recipient private key must be Ed25519 or EC P-256");
  }
  return out;
}

} // namespace

NativeProtectedGrantConfig loadNativeProtectedGrantConfig(
  const std::string& provider, const std::string& modelFamily,
  const std::string& protectionEpoch)
{
  return credentials(provider, modelFamily, protectionEpoch);
}

} // namespace ndnsf::di
