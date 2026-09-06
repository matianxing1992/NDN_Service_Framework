#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sys/stat.h>

namespace ndnsf::di::test {
namespace {
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

Key makeKey(int type, int curve = NID_X9_62_prime256v1)
{
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
    EVP_PKEY_CTX_new_id(type, nullptr), EVP_PKEY_CTX_free);
  if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
      (type == EVP_PKEY_EC &&
       EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), curve) != 1)) {
    throw std::runtime_error("fixture key setup failed");
  }
  EVP_PKEY* key = nullptr;
  if (EVP_PKEY_keygen(context.get(), &key) != 1) {
    throw std::runtime_error("fixture key generation failed");
  }
  return Key(key, EVP_PKEY_free);
}

std::string pem(EVP_PKEY* key, bool secret)
{
  std::unique_ptr<BIO, decltype(&BIO_free)> buffer(BIO_new(BIO_s_mem()), BIO_free);
  const auto ok = secret
    ? PEM_write_bio_PrivateKey(buffer.get(), key, nullptr, nullptr, 0, nullptr, nullptr)
    : PEM_write_bio_PUBKEY(buffer.get(), key);
  if (ok != 1) throw std::runtime_error("fixture PEM encoding failed");
  char* data = nullptr;
  const auto size = BIO_get_mem_data(buffer.get(), &data);
  return std::string(data, size);
}

struct ScopedEnv
{
  std::string name;
  std::optional<std::string> previous;
  ScopedEnv(std::string key, const std::string& value) : name(std::move(key))
  {
    if (const auto* old = std::getenv(name.c_str())) previous = old;
    if (::setenv(name.c_str(), value.c_str(), 1) != 0)
      throw std::runtime_error("fixture environment setup failed");
  }
  ~ScopedEnv()
  {
    if (previous) ::setenv(name.c_str(), previous->c_str(), 1);
    else ::unsetenv(name.c_str());
  }
};

struct NativeCredentialFixture
{
  std::filesystem::path root;
  std::unique_ptr<ScopedEnv> publicSetting;
  std::unique_ptr<ScopedEnv> recipientSetting;

  NativeCredentialFixture()
  {
    char pattern[] = "/tmp/spec181-native-credentials-XXXXXX";
    const auto* directory = ::mkdtemp(pattern);
    if (!directory) throw std::runtime_error("fixture directory setup failed");
    root = directory;
    std::filesystem::create_directory(root / "trust");
    const auto authority = makeKey(EVP_PKEY_ED25519);
    const auto publicPem = pem(authority.get(), false);
    std::ofstream(root / "authority.pub") << publicPem;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(publicPem.data()), publicPem.size(), hash);
    std::string digest = "sha256:";
    for (auto byte : hash) {
      digest += "0123456789abcdef"[byte >> 4];
      digest += "0123456789abcdef"[byte & 15];
    }
    boost::property_tree::ptree registry;
    registry.put("schemaVersion", 1);
    registry.put("status", "CONFIGURED");
    auto& policy = registry.put_child("artifactPolicyAuthority", {});
    policy.put("publicKeyAlgorithm", "ed25519");
    policy.put("signatureAlgorithm", "ed25519");
    policy.put("grantSchema", "ndnsf-di-key-grant-v1");
    policy.put("publicKeyPath", "authority.pub");
    policy.put("publicKeySha256", digest);
    policy.put("authorityId", "/test/authority");
    policy.put("keyId", "/test/authority/KEY/1");
    for (const auto& item : {std::make_pair("acceptedModelFamilies", "YOLO26n"),
                             std::make_pair("protectionEpochs", "protected-v1")}) {
      boost::property_tree::ptree array, value;
      value.put_value(item.second);
      array.push_back({"", value});
      policy.put_child(item.first, array);
    }
    boost::property_tree::write_json((root / "trust/trust-root-registry-v1.json").string(), registry);
    boost::property_tree::ptree mapping, keyPath;
    keyPath.put_value((root / "recipient.pem").string());
    mapping.push_back({"/test/provider", keyPath});
    boost::property_tree::write_json((root / "recipients.json").string(), mapping);
    publicSetting = std::make_unique<ScopedEnv>("SPEC181_GRANT_AUTHORITY_PUBLIC_KEY",
                                               (root / "trust/authority.pub").string());
    recipientSetting = std::make_unique<ScopedEnv>("SPEC181_PROVIDER_RECIPIENT_KEY_MAP",
                                                  (root / "recipients.json").string());
  }

  ~NativeCredentialFixture()
  {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  void writeRecipient(int type, int curve = NID_X9_62_prime256v1)
  {
    const auto key = makeKey(type, curve);
    std::ofstream(root / "recipient.pem") << pem(key.get(), true);
    if (::chmod((root / "recipient.pem").c_str(), 0600) != 0)
      throw std::runtime_error("fixture key permissions failed");
  }

  NativeProtectedGrantConfig load()
  {
    return loadNativeProtectedGrantConfig("/test/provider", "YOLO26n", "protected-v1");
  }
};
} // namespace

BOOST_FIXTURE_TEST_CASE(NativeProtectedCredentialsLoadEd25519, NativeCredentialFixture)
{
  writeRecipient(EVP_PKEY_ED25519);
  const auto config = load();
  BOOST_CHECK(config.recipientKey.kind == NativeRecipientKey::Kind::Ed25519Seed);
  BOOST_CHECK_EQUAL(config.recipientKey.material.size(), 32);
  BOOST_CHECK_EQUAL(config.authorityIdentity, "/test/authority");
}

BOOST_FIXTURE_TEST_CASE(NativeProtectedCredentialsLoadEcP256, NativeCredentialFixture)
{
  writeRecipient(EVP_PKEY_EC);
  const auto config = load();
  BOOST_CHECK(config.recipientKey.kind == NativeRecipientKey::Kind::EcP256Pem);
  BOOST_CHECK(config.recipientKey.material.find("BEGIN PRIVATE KEY") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(NativeProtectedCredentialsRejectOtherCurve, NativeCredentialFixture)
{
  writeRecipient(EVP_PKEY_EC, NID_secp384r1);
  BOOST_CHECK_THROW(load(), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(NativeProtectedCredentialsRejectPublicPermissions, NativeCredentialFixture)
{
  writeRecipient(EVP_PKEY_EC);
  BOOST_REQUIRE_EQUAL(::chmod((root / "recipient.pem").c_str(), 0644), 0);
  BOOST_CHECK_THROW(load(), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(NativeProtectedCredentialsRejectUnknownProvider, NativeCredentialFixture)
{
  writeRecipient(EVP_PKEY_ED25519);
  BOOST_CHECK_THROW(loadNativeProtectedGrantConfig("/other/provider", "YOLO26n", "protected-v1"),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(NativeProtectedRegistryFamilyMatchesOperatorModelNamespace)
{
  BOOST_CHECK_EQUAL(nativeProtectedModelFamily("/Model/YOLO26n"), "YOLO26n");
  BOOST_CHECK_EQUAL(nativeProtectedModelFamily("YOLO26n"), "YOLO26n");
  BOOST_CHECK_EQUAL(nativeProtectedModelFamily("/Other/YOLO26n"), "/Other/YOLO26n");
  BOOST_CHECK_EQUAL(nativeProtectedModelFamily("/Model/Other/YOLO26n"), "/Model/Other/YOLO26n");
}
} // namespace ndnsf::di::test
