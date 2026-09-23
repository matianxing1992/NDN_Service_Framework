#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantVerifier.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/bn.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace ndnsf::di {
namespace {

constexpr const char* GRANT_KDF_INFO = "NDNSF-DI/key-grant/v1";
constexpr const char* GRANT_POLICY = "CANCEL_IMMEDIATELY";

struct CleanseString
{
  std::string& value;
  ~CleanseString() { OPENSSL_cleanse(value.data(), value.size()); }
};

// ---------------------------------------------------------------------------
// Canonical JSON helpers (byte-compatible with the Python verifier's
// json.dumps(sort_keys=True, separators=(",", ":"), ensure_ascii=False))

std::string
jsonEscape(const std::string& value)
{
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (unsigned char ch : value) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
          out += buffer;
        }
        else {
          out.push_back(static_cast<char>(ch));
        }
    }
  }
  out.push_back('"');
  return out;
}

std::string
hexEncode(const std::vector<std::uint8_t>& bytes)
{
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (auto byte : bytes) {
    out.push_back(DIGITS[byte >> 4]);
    out.push_back(DIGITS[byte & 0x0f]);
  }
  return out;
}

bool
hexDecode(const std::string& text, std::vector<std::uint8_t>& out)
{
  if (text.size() % 2 != 0) {
    return false;
  }
  out.clear();
  out.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    auto nibble = [] (char ch) -> int {
      if (ch >= '0' && ch <= '9') return ch - '0';
      if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
      if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
      return -1;
    };
    int high = nibble(text[index]);
    int low = nibble(text[index + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    out.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return true;
}

std::string
sha256Hex(const std::string& bytes)
{
  std::vector<std::uint8_t> digest(SHA256_DIGEST_LENGTH);
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
         digest.data());
  return hexEncode(digest);
}

/** Canonical binding context: the AES-GCM AAD and HKDF info suffix. */
std::string
bindingContextJson(const std::string& providerIdentity,
                   const std::string& requestId,
                   std::uint64_t attempt,
                   const std::string& planCoreDigest,
                   const std::string& modelManifestDigest,
                   const std::string& protectionEpoch)
{
  return std::string("{\"attempt\":") + std::to_string(attempt) +
         ",\"modelManifestDigest\":" + jsonEscape(modelManifestDigest) +
         ",\"planCoreDigest\":" + jsonEscape(planCoreDigest) +
         ",\"protectionEpoch\":" + jsonEscape(protectionEpoch) +
         ",\"providerIdentity\":" + jsonEscape(providerIdentity) +
         ",\"requestId\":" + jsonEscape(requestId) + "}";
}

// ---------------------------------------------------------------------------
// Parsed grant wire record

struct ParsedGrant
{
  std::string policyAuthority;
  std::string providerIdentity;
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string planCoreDigest;
  std::string modelManifestDigest;
  std::string protectionEpoch;
  std::string keyId;
  std::string envelopeAlg;
  std::string envelopeKdf;
  std::string envelopeEphemeralPublicKey;
  std::string envelopeNonce;
  std::string envelopeCiphertext;
  std::vector<std::string> allowedResidencyTiers;
  std::uint64_t issuedAtMs = 0;
  std::uint64_t expiresAtMs = 0;
  std::uint64_t revocationSequence = 1;
  std::string activeRequestPolicy;
  std::string grantDigest;
  std::string authoritySignature;

  /** Canonical signed payload: every field except digest/signature. */
  std::string signingBytes() const
  {
    std::string tiers = "[";
    for (std::size_t index = 0; index < allowedResidencyTiers.size(); ++index) {
      if (index != 0) tiers.push_back(',');
      tiers += jsonEscape(allowedResidencyTiers[index]);
    }
    tiers.push_back(']');
    return std::string("{\"activeRequestPolicy\":") +
           jsonEscape(activeRequestPolicy) +
           ",\"allowedResidencyTiers\":" + tiers +
           ",\"attempt\":" + std::to_string(attempt) +
           ",\"expiresAtMs\":" + std::to_string(expiresAtMs) +
           ",\"issuedAtMs\":" + std::to_string(issuedAtMs) +
           ",\"keyId\":" + jsonEscape(keyId) +
           ",\"modelManifestDigest\":" + jsonEscape(modelManifestDigest) +
           ",\"planCoreDigest\":" + jsonEscape(planCoreDigest) +
           ",\"policyAuthority\":" + jsonEscape(policyAuthority) +
           ",\"protectionEpoch\":" + jsonEscape(protectionEpoch) +
           ",\"providerIdentity\":" + jsonEscape(providerIdentity) +
           ",\"requestId\":" + jsonEscape(requestId) +
           ",\"revocationSequence\":" + std::to_string(revocationSequence) +
           ",\"wrappedContentKey\":{\"alg\":" + jsonEscape(envelopeAlg) +
           ",\"ciphertext\":" + jsonEscape(envelopeCiphertext) +
           ",\"ephemeralPublicKey\":" + jsonEscape(envelopeEphemeralPublicKey) +
           ",\"kdf\":" + jsonEscape(envelopeKdf) +
           ",\"nonce\":" + jsonEscape(envelopeNonce) + "}}";
  }

  std::string computedGrantDigest() const
  {
    return "sha256:" + sha256Hex(signingBytes());
  }
};

std::string
requireString(const boost::property_tree::ptree& node, const std::string& key)
{
  return node.get<std::string>(key, "");
}

bool
parseGrant(const std::string& wireJson, ParsedGrant& out, std::string& error)
{
  try {
    std::istringstream input(wireJson);
    boost::property_tree::ptree root;
    boost::property_tree::read_json(input, root);
    out.policyAuthority = requireString(root, "policyAuthority");
    out.providerIdentity = requireString(root, "providerIdentity");
    out.requestId = requireString(root, "requestId");
    out.attempt = root.get<std::uint64_t>("attempt", 0);
    out.planCoreDigest = requireString(root, "planCoreDigest");
    out.modelManifestDigest = requireString(root, "modelManifestDigest");
    out.protectionEpoch = requireString(root, "protectionEpoch");
    out.keyId = requireString(root, "keyId");
    const auto& envelope = root.get_child("wrappedContentKey");
    out.envelopeAlg = requireString(envelope, "alg");
    out.envelopeKdf = requireString(envelope, "kdf");
    out.envelopeEphemeralPublicKey = requireString(envelope, "ephemeralPublicKey");
    out.envelopeNonce = requireString(envelope, "nonce");
    out.envelopeCiphertext = requireString(envelope, "ciphertext");
    for (const auto& item : root.get_child("allowedResidencyTiers")) {
      out.allowedResidencyTiers.push_back(item.second.get_value<std::string>());
    }
    out.issuedAtMs = root.get<std::uint64_t>("issuedAtMs", 0);
    out.expiresAtMs = root.get<std::uint64_t>("expiresAtMs", 0);
    out.revocationSequence = root.get<std::uint64_t>("revocationSequence", 1);
    out.activeRequestPolicy = requireString(root, "activeRequestPolicy");
    out.grantDigest = requireString(root, "grantDigest");
    out.authoritySignature = requireString(root, "authoritySignature");
    if (out.policyAuthority.empty() || out.providerIdentity.empty() ||
        out.requestId.empty() || out.attempt == 0 ||
        out.planCoreDigest.empty() || out.modelManifestDigest.empty() ||
        out.protectionEpoch.empty() || out.keyId.empty() ||
        out.envelopeAlg.empty() || out.envelopeCiphertext.empty() ||
        out.grantDigest.empty() || out.authoritySignature.empty() ||
        out.expiresAtMs == 0 || out.revocationSequence == 0) {
      error = "key grant payload is incomplete";
      return false;
    }
    if (out.activeRequestPolicy != GRANT_POLICY) {
      error = "key grant active request policy is not canonical";
      return false;
    }
    return true;
  }
  catch (const std::exception&) {
    error = "key grant wire bytes are malformed";
    return false;
  }
}

// ---------------------------------------------------------------------------
// OpenSSL primitives (Ed25519 verify, X25519/ECDH derive, HKDF, AES-GCM)

class EvpKey
{
public:
  explicit EvpKey(EVP_PKEY* key = nullptr) : m_key(key) {}
  ~EvpKey() { if (m_key) EVP_PKEY_free(m_key); }
  EvpKey(const EvpKey&) = delete;
  EvpKey& operator=(const EvpKey&) = delete;
  EVP_PKEY* get() const noexcept { return m_key; }

private:
  EVP_PKEY* m_key;
};

bool
verifyEd25519(const std::string& signatureHex,
              const std::string& data,
              const std::string& publicKeyRaw)
{
  std::vector<std::uint8_t> signature;
  if (publicKeyRaw.size() != 32 || !hexDecode(signatureHex, signature)) {
    return false;
  }
  EvpKey key(EVP_PKEY_new_raw_public_key(
    EVP_PKEY_ED25519, nullptr,
    reinterpret_cast<const unsigned char*>(publicKeyRaw.data()),
    publicKeyRaw.size()));
  if (!key.get()) {
    return false;
  }
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return false;
  bool ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key.get()) == 1 &&
            EVP_DigestVerify(ctx, signature.data(), signature.size(),
                             reinterpret_cast<const unsigned char*>(data.data()),
                             data.size()) == 1;
  EVP_MD_CTX_free(ctx);
  return ok;
}

std::string
deriveHkdfSha256(const std::string& ikm, const std::string& info)
{
  static const std::uint8_t zeroSalt[SHA256_DIGEST_LENGTH] = {};
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  if (!ctx) throw std::runtime_error("HKDF context allocation failed");
  std::string result(SHA256_DIGEST_LENGTH, '\0');
  std::size_t outLen = result.size();
  bool ok =
    EVP_PKEY_derive_init(ctx) == 1 &&
    EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) == 1 &&
    EVP_PKEY_CTX_set1_hkdf_salt(ctx, zeroSalt, sizeof(zeroSalt)) == 1 &&
    EVP_PKEY_CTX_set1_hkdf_key(ctx,
      reinterpret_cast<const unsigned char*>(ikm.data()), ikm.size()) == 1 &&
    EVP_PKEY_CTX_add1_hkdf_info(ctx,
      reinterpret_cast<const unsigned char*>(info.data()), info.size()) == 1 &&
    EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char*>(&result[0]),
                    &outLen) == 1;
  EVP_PKEY_CTX_free(ctx);
  if (!ok || outLen != result.size()) {
    throw std::runtime_error("HKDF derivation failed");
  }
  return result;
}

std::string
x25519SharedSecret(const std::string& privateRaw,
                   const std::string& peerPublicRaw)
{
  EvpKey privateKey(EVP_PKEY_new_raw_private_key(
    EVP_PKEY_X25519, nullptr,
    reinterpret_cast<const unsigned char*>(privateRaw.data()),
    privateRaw.size()));
  EvpKey peerKey(EVP_PKEY_new_raw_public_key(
    EVP_PKEY_X25519, nullptr,
    reinterpret_cast<const unsigned char*>(peerPublicRaw.data()),
    peerPublicRaw.size()));
  if (!privateKey.get() || !peerKey.get()) {
    throw std::runtime_error("X25519 key import failed");
  }
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(privateKey.get(), nullptr);
  if (!ctx) throw std::runtime_error("X25519 context allocation failed");
  std::string shared(32, '\0');
  std::size_t sharedLen = shared.size();
  bool ok = EVP_PKEY_derive_init(ctx) == 1 &&
            EVP_PKEY_derive_set_peer(ctx, peerKey.get()) == 1 &&
            EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char*>(&shared[0]),
                            &sharedLen) == 1;
  EVP_PKEY_CTX_free(ctx);
  if (!ok || sharedLen != shared.size()) {
    throw std::runtime_error("X25519 derive failed");
  }
  return shared;
}

std::string
ecP256SharedSecret(const std::string& privatePem,
                   const std::string& peerPublicRaw)
{
  BIO* bio = BIO_new_mem_buf(privatePem.data(),
                             static_cast<int>(privatePem.size()));
  if (!bio) throw std::runtime_error("EC key BIO allocation failed");
  EVP_PKEY* rawKey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  EvpKey privateKey(rawKey);
  if (!privateKey.get()) throw std::runtime_error("EC private key parse failed");
  std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)> ecKey(
    EVP_PKEY_get1_EC_KEY(privateKey.get()), EC_KEY_free);
  if (!ecKey) throw std::runtime_error("EC key extraction failed");
  const EC_GROUP* group = EC_KEY_get0_group(ecKey.get());
  if (!group || EC_GROUP_get_curve_name(group) != NID_X9_62_prime256v1) {
    throw std::runtime_error("EC recipient key must use P-256");
  }
  std::unique_ptr<EC_POINT, decltype(&EC_POINT_free)> point(
    EC_POINT_new(group), EC_POINT_free);
  bool pointOk = point != nullptr &&
    EC_POINT_oct2point(group, point.get(),
                       reinterpret_cast<const unsigned char*>(
                         peerPublicRaw.data()),
                       peerPublicRaw.size(), nullptr) == 1 &&
    EC_POINT_is_at_infinity(group, point.get()) == 0 &&
    EC_POINT_is_on_curve(group, point.get(), nullptr) == 1;
  if (!pointOk) {
    throw std::runtime_error("EC peer point parse failed");
  }
  std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)> peerEc(EC_KEY_new(), EC_KEY_free);
  EvpKey peerKey(EVP_PKEY_new());
  if (!peerEc || !peerKey.get() ||
      EC_KEY_set_group(peerEc.get(), group) != 1 ||
      EC_KEY_set_public_key(peerEc.get(), point.get()) != 1 ||
      EVP_PKEY_assign_EC_KEY(peerKey.get(), peerEc.get()) != 1) {
    throw std::runtime_error("EC peer key allocation failed");
  }
  // EVP_PKEY owns the peer only after a successful assignment.
  peerEc.release();

  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
    EVP_PKEY_CTX_new(privateKey.get(), nullptr), EVP_PKEY_CTX_free);
  if (!ctx) throw std::runtime_error("ECDH context allocation failed");
  std::string shared(32, '\0');
  std::size_t sharedLen = shared.size();
  bool ok = EVP_PKEY_derive_init(ctx.get()) == 1 &&
            EVP_PKEY_derive_set_peer(ctx.get(), peerKey.get()) == 1 &&
            EVP_PKEY_derive(ctx.get(), reinterpret_cast<unsigned char*>(&shared[0]),
                            &sharedLen) == 1;
  if (!ok || sharedLen != shared.size()) {
    OPENSSL_cleanse(shared.data(), shared.size());
    throw std::runtime_error("ECDH derive failed");
  }
  return shared;
}

std::string
aesGcmDecrypt(const std::string& key, const std::string& nonce,
              const std::string& ciphertext, const std::string& aad)
{
  constexpr std::size_t TAG_LENGTH = 16;
  if (ciphertext.size() < TAG_LENGTH) {
    throw std::runtime_error("envelope ciphertext is truncated");
  }
  const std::string body = ciphertext.substr(0, ciphertext.size() - TAG_LENGTH);
  const std::string tag = ciphertext.substr(ciphertext.size() - TAG_LENGTH);
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) throw std::runtime_error("AES-GCM context allocation failed");
  std::string plaintext(body.size(), '\0');
  int written = 0;
  bool ok =
    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                        static_cast<int>(nonce.size()), nullptr) == 1 &&
    EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                       reinterpret_cast<const unsigned char*>(key.data()),
                       reinterpret_cast<const unsigned char*>(nonce.data())) == 1 &&
    EVP_DecryptUpdate(ctx, nullptr, &written,
                      reinterpret_cast<const unsigned char*>(aad.data()),
                      static_cast<int>(aad.size())) == 1 &&
    (body.empty() ||
     EVP_DecryptUpdate(ctx,
                       reinterpret_cast<unsigned char*>(&plaintext[0]),
                       &written,
                       reinterpret_cast<const unsigned char*>(body.data()),
                       static_cast<int>(body.size())) == 1) &&
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LENGTH,
                        const_cast<char*>(tag.data())) == 1 &&
    EVP_DecryptFinal_ex(ctx, nullptr, &written) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    OPENSSL_cleanse(plaintext.data(), plaintext.size());
    throw std::runtime_error(
      "content-key envelope failed authentication");
  }
  return plaintext;
}

/** Ed25519 seed -> X25519 private key bytes (matches the Python converter). */
std::string
ed25519SeedToX25519(const std::string& seed)
{
  unsigned char digest[SHA512_DIGEST_LENGTH];
  SHA512(reinterpret_cast<const unsigned char*>(seed.data()), seed.size(),
         digest);
  std::string result(reinterpret_cast<char*>(digest), 32);
  OPENSSL_cleanse(digest, sizeof(digest));
  return result;
}

} // namespace

namespace detail {
std::string signNativeGrantBytes(EVP_PKEY& key, const std::string& bytes)
{
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  std::vector<std::uint8_t> signature(64);
  std::size_t size = signature.size();
  if (EVP_PKEY_id(&key) != EVP_PKEY_ED25519 || !ctx ||
      EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, &key) != 1 ||
      EVP_DigestSign(ctx.get(), signature.data(), &size,
        reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()) != 1 || size != 64)
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: Ed25519 signing failed");
  return hexEncode(signature);
}

bool verifyNativeGrantBytes(EVP_PKEY& key, const std::string& bytes,
                            const std::string& signature)
{
  if (signature.size() != 128) return false;
  std::string raw(32, '\0');
  std::size_t size = raw.size();
  return EVP_PKEY_id(&key) == EVP_PKEY_ED25519 &&
    EVP_PKEY_get_raw_public_key(&key, reinterpret_cast<unsigned char*>(raw.data()), &size) == 1 &&
    size == 32 && verifyEd25519(signature, bytes, raw);
}

void verifyNativeIssuedGrant(const NativeKeyGrant& value, const NativeSignedGrantRequest& request,
  const std::string& authority, const std::string& authorityPublicKeyRaw,
  std::uint64_t nowMs, std::uint64_t expiresAtMs)
{
  ParsedGrant grant;
  std::string error;
  if (value.wireJson.size() > 65536 || !parseGrant(value.wireJson, grant, error) ||
      grant.policyAuthority != authority || grant.providerIdentity != request.providerIdentity ||
      grant.requestId != request.requestId || grant.attempt != request.attempt ||
      grant.planCoreDigest != request.planCoreDigest || grant.modelManifestDigest != request.modelManifestDigest ||
      grant.protectionEpoch != request.protectionEpoch || grant.issuedAtMs > nowMs ||
      grant.issuedAtMs < request.issuedAtMs || grant.expiresAtMs != expiresAtMs || nowMs >= expiresAtMs ||
      grant.allowedResidencyTiers != request.allowedResidencyTiers ||
      grant.grantDigest != grant.computedGrantDigest() || value.grantDigest != grant.grantDigest ||
      value.recipient != request.providerIdentity || value.expiresAtMs != grant.expiresAtMs ||
      !verifyEd25519(grant.authoritySignature, grant.signingBytes(), authorityPublicKeyRaw))
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: issuer answer authentication failed");
  auto canonical = NativeJson::parse(grant.signingBytes());
  canonical["grantDigest"] = grant.grantDigest;
  canonical["authoritySignature"] = grant.authoritySignature;
  if (canonical.dump() != value.wireJson)
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: issuer answer is not canonical");
  if (value.grantName != canonicalNativeGrantName(request.requesterIdentity, request.providerIdentity,
        request.requestId, request.attempt, request.planCoreDigest, request.modelManifestDigest,
        request.protectionEpoch, grant.grantDigest))
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: issuer answer name mismatch");
}

NativeKeyGrant issueNativeGrantWire(const NativeSignedGrantRequest& request,
  const std::string& authority, const std::string& keyId, EVP_PKEY& authorityKey,
  EVP_PKEY& recipientKey, const std::vector<std::uint8_t>& contentKey,
  std::uint64_t nowMs, std::uint64_t expiresAtMs)
{
  const auto require = [](bool ok) {
    if (!ok) throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: recipient envelope creation failed");
  };
  // Only public points cross this conversion. Match the frozen Ed25519 ->
  // Montgomery representation; OpenSSL owns all key exchange and AEAD work.
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> converted(nullptr, EVP_PKEY_free);
  EVP_PKEY* peer = &recipientKey;
  const int kind = EVP_PKEY_id(peer);
  if (kind == EVP_PKEY_ED25519) {
    unsigned char raw[32]; std::size_t size = sizeof(raw);
    require(EVP_PKEY_get_raw_public_key(peer, raw, &size) == 1 && size == sizeof(raw));
    raw[31] &= 127;
    std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> bn(BN_CTX_new(), BN_CTX_free);
    require(bool(bn));
    BN_CTX_start(bn.get());
    auto* y = BN_CTX_get(bn.get()); auto* p = BN_CTX_get(bn.get());
    auto* num = BN_CTX_get(bn.get()); auto* den = BN_CTX_get(bn.get());
    auto* inv = BN_CTX_get(bn.get()); auto* u = BN_CTX_get(bn.get());
    require(u && BN_lebin2bn(raw, sizeof(raw), y) && BN_one(p) == 1 &&
      BN_lshift(p, p, 255) == 1 && BN_sub_word(p, 19) == 1 && BN_cmp(y, p) < 0 &&
      BN_mod_add(num, y, BN_value_one(), p, bn.get()) == 1 &&
      BN_mod_sub(den, BN_value_one(), y, p, bn.get()) == 1 &&
      BN_mod_inverse(inv, den, p, bn.get()) &&
      BN_mod_mul(u, num, inv, p, bn.get()) == 1 && BN_bn2lebinpad(u, raw, sizeof(raw)) == 32);
    converted.reset(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, raw, sizeof(raw)));
    BN_CTX_end(bn.get());
    require(bool(converted)); peer = converted.get();
  }
  require(EVP_PKEY_id(peer) == EVP_PKEY_X25519 || EVP_PKEY_id(peer) == EVP_PKEY_EC);
  const bool ec = EVP_PKEY_id(peer) == EVP_PKEY_EC;
  if (ec) {
    std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)> key(EVP_PKEY_get1_EC_KEY(peer), EC_KEY_free);
    require(key && EC_KEY_get0_group(key.get()) &&
      EC_GROUP_get_curve_name(EC_KEY_get0_group(key.get())) == NID_X9_62_prime256v1 &&
      EC_KEY_check_key(key.get()) == 1);
  }
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> gen(
    EVP_PKEY_CTX_new_id(ec ? EVP_PKEY_EC : EVP_PKEY_X25519, nullptr), EVP_PKEY_CTX_free);
  require(gen && EVP_PKEY_keygen_init(gen.get()) == 1);
  if (ec) require(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(gen.get(), NID_X9_62_prime256v1) == 1);
  EVP_PKEY* generated = nullptr;
  const int generatedOk = EVP_PKEY_keygen(gen.get(), &generated);
  EvpKey ephemeral(generated);
  require(generatedOk == 1 && ephemeral.get());
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> exchange(
    EVP_PKEY_CTX_new(ephemeral.get(), nullptr), EVP_PKEY_CTX_free);
  std::string shared(32, '\0'); CleanseString clearShared{shared};
  std::size_t sharedSize = shared.size();
  require(exchange && EVP_PKEY_derive_init(exchange.get()) == 1 &&
    EVP_PKEY_derive_set_peer(exchange.get(), peer) == 1 &&
    EVP_PKEY_derive(exchange.get(), reinterpret_cast<unsigned char*>(shared.data()), &sharedSize) == 1 &&
    sharedSize == shared.size());
  std::vector<std::uint8_t> ephemeralPublic(ec ? 65 : 32);
  if (ec) {
    std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)> key(
      EVP_PKEY_get1_EC_KEY(ephemeral.get()), EC_KEY_free);
    require(key && EC_POINT_point2oct(EC_KEY_get0_group(key.get()), EC_KEY_get0_public_key(key.get()),
      POINT_CONVERSION_UNCOMPRESSED, ephemeralPublic.data(), ephemeralPublic.size(), nullptr) == 65);
  }
  else {
    std::size_t size = ephemeralPublic.size();
    require(EVP_PKEY_get_raw_public_key(ephemeral.get(), ephemeralPublic.data(), &size) == 1 && size == 32);
  }
  const auto context = bindingContextJson(request.providerIdentity, request.requestId, request.attempt,
    request.planCoreDigest, request.modelManifestDigest, request.protectionEpoch);
  auto derived = deriveHkdfSha256(shared, std::string(GRANT_KDF_INFO) + context);
  CleanseString clearDerived{derived};
  std::vector<std::uint8_t> nonce(12), ciphertext(contentKey.size() + 16);
  require(RAND_bytes(nonce.data(), nonce.size()) == 1);
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(
    EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  int aadSize = 0, bodySize = 0, finalSize = 0;
  require(cipher && EVP_EncryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr,
      reinterpret_cast<const unsigned char*>(derived.data()), nonce.data()) == 1 &&
    EVP_EncryptUpdate(cipher.get(), nullptr, &aadSize,
      reinterpret_cast<const unsigned char*>(context.data()), context.size()) == 1 &&
    EVP_EncryptUpdate(cipher.get(), ciphertext.data(), &bodySize, contentKey.data(), contentKey.size()) == 1 &&
    EVP_EncryptFinal_ex(cipher.get(), ciphertext.data() + bodySize, &finalSize) == 1 &&
    bodySize + finalSize == static_cast<int>(contentKey.size()) &&
    EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG, 16, ciphertext.data() + contentKey.size()) == 1);
  ParsedGrant grant;
  grant.policyAuthority = authority; grant.providerIdentity = request.providerIdentity;
  grant.requestId = request.requestId; grant.attempt = request.attempt;
  grant.planCoreDigest = request.planCoreDigest; grant.modelManifestDigest = request.modelManifestDigest;
  grant.protectionEpoch = request.protectionEpoch; grant.keyId = keyId;
  grant.envelopeAlg = ec ? "ECDH-P256-AESGCM-SHA256" : "X25519-AESGCM-SHA256";
  grant.envelopeKdf = "HKDF-SHA256"; grant.envelopeEphemeralPublicKey = hexEncode(ephemeralPublic);
  grant.envelopeNonce = hexEncode(nonce); grant.envelopeCiphertext = hexEncode(ciphertext);
  grant.allowedResidencyTiers = request.allowedResidencyTiers;
  grant.issuedAtMs = nowMs; grant.expiresAtMs = expiresAtMs; grant.activeRequestPolicy = GRANT_POLICY;
  grant.grantDigest = grant.computedGrantDigest();
  grant.authoritySignature = signNativeGrantBytes(authorityKey, grant.signingBytes());
  // Provider parser owns the signed payload. Wire appends only its two excluded fields.
  auto wire = grant.signingBytes(); wire.pop_back();
  wire += ",\"grantDigest\":" + jsonEscape(grant.grantDigest) +
          ",\"authoritySignature\":" + jsonEscape(grant.authoritySignature) + "}";
  require(wire.size() <= 65536);
  return {canonicalNativeGrantName(request.requesterIdentity, request.providerIdentity,
    request.requestId, request.attempt, request.planCoreDigest, request.modelManifestDigest,
    request.protectionEpoch, grant.grantDigest), grant.grantDigest, request.providerIdentity,
    NativeJson::parse(wire).dump(), expiresAtMs};
}
} // namespace detail

std::string
canonicalNativeGrantName(
  const std::string& publicationIdentity, const std::string& providerIdentity,
  const std::string& requestId, std::uint64_t attempt,
  const std::string& planCoreDigest, const std::string& modelManifestDigest,
  const std::string& protectionEpoch, const std::string& grantDigest)
{
  const auto safe = [] (unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
  };
  if (publicationIdentity.empty() || publicationIdentity.front() != '/' ||
      publicationIdentity.back() == '/' || publicationIdentity.find("//") != std::string::npos ||
      !std::all_of(publicationIdentity.begin(), publicationIdentity.end(),
        [&] (unsigned char c) { return safe(c) || c == '/' || c == ':'; }) ||
      providerIdentity.empty() || requestId.empty() || protectionEpoch.empty() || attempt == 0) {
    throw std::invalid_argument("grant publication or request identity is invalid");
  }
  const auto bare = [] (const std::string& digest) {
    if (digest.size() != 71 || digest.substr(0, 7) != "sha256:" ||
        !std::all_of(digest.begin() + 7, digest.end(), [] (unsigned char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) throw std::invalid_argument("grant name digest is not canonical");
    return digest.substr(7);
  };
  const auto component = [&] (const std::string& value) {
    const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
      if (safe(c)) out += static_cast<char>(c);
      else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
  };
  return publicationIdentity + "/NDNSF-DI/KEY-GRANT/v1/PROVIDER/" + sha256Hex(providerIdentity) +
    "/REQ/" + component(requestId) + "/ATTEMPT/" + std::to_string(attempt) +
    "/PLAN-CORE/" + bare(planCoreDigest) + "/MODEL/" + bare(modelManifestDigest) +
    "/EPOCH/" + component(protectionEpoch) + "/GRANT/" + bare(grantDigest);
}

NativeGrantVerificationResult
verifyAndUnwrapNativeGrant(const std::string& wireJson,
                           const std::string& authorityPublicKeyRaw,
                           const NativeRecipientKey& recipientKey,
                           const std::string& providerIdentity,
                           const std::string& requestId,
                           std::uint64_t attempt,
                           const std::string& planCoreDigest,
                           const std::string& modelManifestDigest,
                           const std::string& protectionEpoch,
                           std::uint64_t nowMs,
                           const std::string& expectedAuthority,
                           const std::string& expectedGrantDigest)
{
  NativeGrantVerificationResult result;
  if (wireJson.size() > 65536) {
    result.reason = "DI_PROTECTED_GRANT_REJECTED: grant wire exceeds limit";
    return result;
  }
  ParsedGrant grant;
  std::string error;
  if (!parseGrant(wireJson, grant, error)) {
    result.reason = "DI_PROTECTED_GRANT_REJECTED: " + error;
    return result;
  }
  // Binding comparisons first: preserve the existing binding-mismatch
  // semantics; every other verifier decision registers the rejected family.
  if (grant.providerIdentity != providerIdentity ||
      grant.requestId != requestId ||
      grant.attempt != attempt ||
      grant.planCoreDigest != planCoreDigest ||
      grant.modelManifestDigest != modelManifestDigest ||
      grant.protectionEpoch != protectionEpoch) {
    result.reason = "DI_PROTECTED_RUNTIME_BINDING_MISMATCH: key grant "
                    "binding does not match the assignment";
    return result;
  }
  if ((!expectedAuthority.empty() && grant.policyAuthority != expectedAuthority) ||
      (!expectedGrantDigest.empty() && grant.grantDigest != expectedGrantDigest)) {
    result.reason = "DI_PROTECTED_GRANT_REJECTED: sealed grant reference or authority mismatch";
    return result;
  }
  if (grant.grantDigest != grant.computedGrantDigest()) {
    result.reason = "DI_PROTECTED_GRANT_REJECTED: key grant digest is "
                    "inconsistent";
    return result;
  }
  if (nowMs >= grant.expiresAtMs) {
    result.reason = "DI_PROTECTED_GRANT_REJECTED: key grant is expired";
    return result;
  }
  if (!verifyEd25519(grant.authoritySignature, grant.signingBytes(),
                     authorityPublicKeyRaw)) {
    result.reason = "DI_PROTECTED_GRANT_REJECTED: key grant authority "
                    "signature is invalid";
    return result;
  }

  const std::string context = bindingContextJson(
    providerIdentity, requestId, attempt, planCoreDigest,
    modelManifestDigest, protectionEpoch);
  try {
    std::string shared;
    CleanseString cleanShared{shared};
    if (grant.envelopeAlg == "X25519-AESGCM-SHA256") {
      if (recipientKey.kind != NativeRecipientKey::Kind::Ed25519Seed ||
          recipientKey.material.size() != 32) {
        result.reason = "DI_PROTECTED_GRANT_REJECTED: X25519 envelope "
                        "requires an Ed25519 recipient seed";
        return result;
      }
      std::vector<std::uint8_t> peerPublic;
      if (!hexDecode(grant.envelopeEphemeralPublicKey, peerPublic)) {
        result.reason = "DI_PROTECTED_GRANT_REJECTED: envelope ephemeral "
                        "public key is malformed";
        return result;
      }
      auto curveSeed = ed25519SeedToX25519(recipientKey.material);
      CleanseString cleanSeed{curveSeed};
      shared = x25519SharedSecret(
        curveSeed,
        std::string(reinterpret_cast<const char*>(peerPublic.data()),
                    peerPublic.size()));
    }
    else if (grant.envelopeAlg == "ECDH-P256-AESGCM-SHA256") {
      if (recipientKey.kind != NativeRecipientKey::Kind::EcP256Pem) {
        result.reason = "DI_PROTECTED_GRANT_REJECTED: P-256 envelope "
                        "requires an EC recipient key";
        return result;
      }
      std::vector<std::uint8_t> peerPublic;
      if (!hexDecode(grant.envelopeEphemeralPublicKey, peerPublic)) {
        result.reason = "DI_PROTECTED_GRANT_REJECTED: envelope ephemeral "
                        "public key is malformed";
        return result;
      }
      shared = ecP256SharedSecret(
        recipientKey.material,
        std::string(reinterpret_cast<const char*>(peerPublic.data()),
                    peerPublic.size()));
    }
    else {
      result.reason = "DI_PROTECTED_GRANT_REJECTED: unsupported recipient "
                      "envelope algorithm";
      return result;
    }
    std::string derived = deriveHkdfSha256(
      shared, std::string(GRANT_KDF_INFO) + context);
    CleanseString cleanDerived{derived};
    std::vector<std::uint8_t> nonce;
    std::vector<std::uint8_t> ciphertext;
    if (!hexDecode(grant.envelopeNonce, nonce) ||
        !hexDecode(grant.envelopeCiphertext, ciphertext)) {
      result.reason = "DI_PROTECTED_GRANT_REJECTED: envelope encoding is "
                      "malformed";
      return result;
    }
    std::string plaintext = aesGcmDecrypt(
      derived,
      std::string(reinterpret_cast<const char*>(nonce.data()), nonce.size()),
      std::string(reinterpret_cast<const char*>(ciphertext.data()),
                  ciphertext.size()),
      context);
    CleanseString cleanPlaintext{plaintext};
    if (plaintext.size() != 32) {
      OPENSSL_cleanse(plaintext.data(), plaintext.size());
      throw std::runtime_error("content key is not 256 bits");
    }
    result.contentKey.assign(
      reinterpret_cast<const std::uint8_t*>(plaintext.data()),
      reinterpret_cast<const std::uint8_t*>(plaintext.data()) +
        plaintext.size());
    result.verified = true;
    result.keyId = grant.keyId;
    result.expiresAtMs = grant.expiresAtMs;
    result.allowedResidencyTiers = grant.allowedResidencyTiers;
    OPENSSL_cleanse(plaintext.data(), plaintext.size());
    return result;
  }
  catch (const std::exception& exc) {
    OPENSSL_cleanse(result.contentKey.data(), result.contentKey.size());
    result.contentKey.clear();
    result.verified = false;
    result.reason = std::string("DI_PROTECTED_GRANT_REJECTED: ") + exc.what();
    return result;
  }
}

} // namespace ndnsf::di
