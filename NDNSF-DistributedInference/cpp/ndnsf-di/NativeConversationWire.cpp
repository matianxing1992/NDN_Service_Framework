#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <limits>
#include <memory>

namespace ndnsf::di {
namespace {
constexpr std::size_t maxCheckpointBytes = 1024 * 1024;
constexpr std::size_t maxTranscriptBytes = 16 * 1024 * 1024;

void appendConversationCanonical(std::string& output, const NativeJson& value)
{
  if (value.is_array()) {
    output += '[';
    bool first = true;
    for (const auto& item : value) {
      if (!first) output += ',';
      first = false;
      appendConversationCanonical(output, item);
    }
    output += ']';
  }
  else if (value.is_object()) {
    output += '{';
    bool first = true;
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (!first) output += ',';
      first = false;
      output += NativeJson(it.key()).dump(-1, ' ', false);
      output += ':';
      appendConversationCanonical(output, it.value());
    }
    output += '}';
  }
  else {
    if (value.is_binary() || value.is_discarded())
      throw std::invalid_argument("conversation canonical JSON requires a JSON value");
    output += value.dump(-1, ' ', false);
  }
}

void require(bool condition, const char* message)
{
  if (!condition) throw std::invalid_argument(message);
}

std::string hex(const unsigned char* data, std::size_t size)
{
  std::string result;
  result.reserve(size * 2);
  for (std::size_t i = 0; i != size; ++i) {
    result += "0123456789abcdef"[data[i] >> 4];
    result += "0123456789abcdef"[data[i] & 15];
  }
  return result;
}

std::string digest(const NativeJson& value)
{
  const auto wire = nativeConversationCanonicalJson(value);
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(wire.data()), wire.size(), hash);
  return "sha256:" + hex(hash, sizeof(hash));
}

std::string signature(const std::string& value, const std::vector<std::uint8_t>& key)
{
  require(key.size() == 32, "conversation authentication key must be 32 bytes");
  unsigned char output[EVP_MAX_MD_SIZE];
  unsigned int size = 0;
  if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
            reinterpret_cast<const unsigned char*>(value.data()), value.size(), output, &size))
    throw std::runtime_error("conversation HMAC failed");
  const auto result = hex(output, size);
  OPENSSL_cleanse(output, sizeof(output));
  return result;
}

void fields(const NativeJson& value, std::initializer_list<const char*> names)
{
  require(value.is_object() && value.size() == names.size(), "conversation field set mismatch");
  for (const auto name : names) require(value.contains(name), "conversation field missing");
}

std::string string(const NativeJson& value, const char* key)
{
  require(value.at(key).is_string(), "conversation field must be string");
  return value.at(key).get<std::string>();
}

std::uint64_t integer(const NativeJson& value, const char* key)
{
  const auto& item = value.at(key);
  require(item.is_number_unsigned() ||
          (item.is_number_integer() && item.get<std::int64_t>() >= 0),
          "conversation field must be nonnegative integer");
  return item.get<std::uint64_t>();
}

void requireDigest(const std::string& value)
{
  require(value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
          value.find_first_not_of("0123456789abcdef", 7) == std::string::npos,
          "conversation digest is not canonical");
}

void identity(const NativeJson& value)
{
  const auto id = string(value, "conversationId");
  require(id.size() >= 16 && id.find_first_of("/\\") == std::string::npos,
          "conversation ID must be opaque");
  const auto service = string(value, "serviceName");
  require(!service.empty() && service.front() == '/' &&
          !string(value, "requesterIdentity").empty(), "conversation authorization identity missing");
  requireDigest(string(value, "securityDomainDigest"));
  requireDigest(string(value, "planRoleMapDigest"));
}

void validateUnsigned(const NativeJson& value)
{
  fields(value, {"schema", "version", "conversationId", "parentContextEpoch", "contextEpoch",
    "serviceName", "requesterIdentity", "securityDomainDigest", "modelContractDigest",
    "planRoleMapDigest", "logicalPrefixDigest", "prefixTokenCount", "roleReceiptDigests",
    "issuedAtMs", "expiresAtMs"});
  require(string(value, "schema") == "ndnsf-di-conversation-checkpoint-v1" &&
          integer(value, "version") == 1, "conversation checkpoint version mismatch");
  identity(value);
  const auto parent = integer(value, "parentContextEpoch");
  require(parent != std::numeric_limits<std::uint64_t>::max() &&
          integer(value, "contextEpoch") == parent + 1, "conversation checkpoint epoch mismatch");
  require(integer(value, "issuedAtMs") > 0 &&
          integer(value, "expiresAtMs") > integer(value, "issuedAtMs"),
          "conversation checkpoint lifetime invalid");
  integer(value, "prefixTokenCount");
  requireDigest(string(value, "modelContractDigest"));
  requireDigest(string(value, "logicalPrefixDigest"));
  const auto& receipts = value.at("roleReceiptDigests");
  require(receipts.is_object() && !receipts.empty(), "conversation requires role receipt set");
  for (auto it = receipts.begin(); it != receipts.end(); ++it) {
    require(!it.key().empty() && it.key().front() == '/' && it.value().is_string(),
            "conversation role receipt invalid");
    requireDigest(it.value().get<std::string>());
  }
  require(nativeConversationCanonicalJson(value).size() <= maxCheckpointBytes,
          "conversation checkpoint exceeds bound");
}

std::string base64(const NativeJson& value)
{
  require(value.is_string(), "conversation base64 must be string");
  const auto encoded = value.get<std::string>();
  require(encoded.size() % 4 == 0, "conversation malformed base64");
  const auto padding = encoded.empty() ? 0U : encoded.back() != '=' ? 0U :
    encoded.size() >= 2 && encoded[encoded.size() - 2] == '=' ? 2U : 1U;
  require(encoded.substr(0, encoded.size() - padding).find_first_not_of(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") == std::string::npos,
    "conversation malformed base64");
  if (encoded.empty()) return {};
  std::string result(encoded.size() / 4 * 3, '\0');
  const auto size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(result.data()),
    reinterpret_cast<const unsigned char*>(encoded.data()), static_cast<int>(encoded.size()));
  require(size >= static_cast<int>(padding), "conversation malformed base64");
  result.resize(static_cast<std::size_t>(size) - padding);
  return result;
}
} // namespace

std::string nativeConversationBase64Encode(const std::string& bytes)
{
  require(bytes.size() <= 64 * 1024 * 1024, "conversation base64 exceeds bound");
  if (bytes.empty()) return {};
  std::string encoded(4 * ((bytes.size() + 2) / 3) + 1, '\0');
  const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
    reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
  require(size >= 0, "conversation base64 encoding failed");
  encoded.resize(static_cast<std::size_t>(size));
  return encoded;
}

std::string nativeConversationBase64Decode(const std::string& encoded)
{
  require(encoded.size() <= 64 * 1024 * 1024, "conversation base64 exceeds bound");
  return base64(encoded);
}

std::string nativeConversationCanonicalJson(const NativeJson& value)
{
  std::string result;
  appendConversationCanonical(result, value);
  return result;
}

std::string nativeSealConversationEnvelope(
  const std::string& plaintext, const std::string& envelopeId,
  std::uint64_t expiresAtMs, const NativeConversationJournalKey& key)
{
  require(plaintext.size() <= maxTranscriptBytes && !envelopeId.empty() &&
          envelopeId.find_first_of("/\\") == std::string::npos &&
          envelopeId != "." && envelopeId != ".." && expiresAtMs > 0 &&
          key.bytes.size() == 32 && !key.id.empty() &&
          key.id.find_first_of("/\\") == std::string::npos, "conversation envelope input invalid");
  NativeJson metadata{{"schema", "ndnsf-di-protected-request-v3"}, {"requestId", envelopeId},
                      {"expiresAtMs", expiresAtMs}, {"keyId", key.id}};
  const auto aad = metadata.dump(-1, ' ', true);
  std::string nonce(12, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), nonce.size()) != 1)
    throw std::runtime_error("conversation nonce generation failed");
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(
    EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!cipher) throw std::runtime_error("conversation cipher allocation failed");
  std::string ciphertext(plaintext.size() + 16, '\0');
  int count = 0, written = 0;
  const bool ok =
    EVP_EncryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
    EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
    EVP_EncryptInit_ex(cipher.get(), nullptr, nullptr, key.bytes.data(),
      reinterpret_cast<const unsigned char*>(nonce.data())) == 1 &&
    EVP_EncryptUpdate(cipher.get(), nullptr, &count,
      reinterpret_cast<const unsigned char*>(aad.data()), static_cast<int>(aad.size())) == 1 &&
    EVP_EncryptUpdate(cipher.get(), reinterpret_cast<unsigned char*>(ciphertext.data()), &written,
      reinterpret_cast<const unsigned char*>(plaintext.data()), static_cast<int>(plaintext.size())) == 1 &&
    EVP_EncryptFinal_ex(cipher.get(), reinterpret_cast<unsigned char*>(ciphertext.data()) + written, &count) == 1;
  if (!ok) throw std::runtime_error("conversation envelope encryption failed");
  ciphertext.resize(static_cast<std::size_t>(written + count) + 16);
  if (EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG, 16,
      ciphertext.data() + written + count) != 1)
    throw std::runtime_error("conversation envelope tag failed");
  metadata["nonce"] = nativeConversationBase64Encode(nonce);
  metadata["ciphertext"] = nativeConversationBase64Encode(ciphertext);
  return metadata.dump(-1, ' ', true);
}

std::vector<std::vector<std::uint8_t>> nativeConversationAuthenticationKeys(
  const std::string& identity, const std::vector<NativeConversationJournalKey>& keys)
{
  require(!identity.empty() && identity.find_first_of("/\\") == std::string::npos &&
          !keys.empty() && keys.size() <= 16, "conversation journal identity/key ring invalid");
  std::string context = "ndnsf-di-runtime-authentication-subkey-v1";
  context.push_back('\0');
  context += "conversation-checkpoint-v1";
  context.push_back('\0');
  context += identity;
  std::set<std::string> ids;
  std::vector<std::vector<std::uint8_t>> result;
  for (const auto& key : keys) {
    require(!key.id.empty() && key.id.find_first_of("/\\") == std::string::npos &&
            key.bytes.size() == 32 && ids.insert(key.id).second,
            "conversation journal key invalid");
    unsigned int size = 32;
    std::vector<std::uint8_t> derived(size);
    if (!HMAC(EVP_sha256(), key.bytes.data(), static_cast<int>(key.bytes.size()),
              reinterpret_cast<const unsigned char*>(context.data()), context.size(),
              derived.data(), &size))
      throw std::runtime_error("conversation subkey derivation failed");
    result.push_back(std::move(derived));
  }
  return result;
}

std::string nativeReadConversationEnvelope(
  const std::string& encoded, const std::string& envelopeId,
  const std::vector<NativeConversationJournalKey>& keys, std::uint64_t nowMs)
{
  require(!keys.empty() && keys.size() <= 16 && nowMs > 0 && !envelopeId.empty() &&
          encoded.size() <= 64 * 1024 * 1024, "conversation envelope input invalid");
  std::set<std::string> ids;
  for (const auto& key : keys)
    require(key.bytes.size() == 32 && !key.id.empty() && ids.insert(key.id).second,
            "conversation journal key invalid");
  auto body = nativeParseJson(encoded);
  require(string(body, "requestId") == envelopeId && integer(body, "expiresAtMs") > nowMs,
          "conversation envelope identity or expiry mismatch");
  const auto schema = string(body, "schema");
  NativeJson metadata{{"schema", schema}, {"requestId", envelopeId},
                      {"expiresAtMs", integer(body, "expiresAtMs")}};
  if (schema == "ndnsf-di-protected-request-v1") {
    fields(body, {"schema", "requestId", "expiresAtMs", "payload", "mac"});
    const auto mac = string(body, "mac");
    body.erase("mac");
    // RuntimeJournal uses Python's ASCII JSON for envelope authentication.
    const auto unsignedWire = body.dump(-1, ' ', true);
    bool verified = false;
    for (const auto& key : keys) {
      const auto expected = signature(unsignedWire, key.bytes);
      verified |= mac.size() == expected.size() &&
                  CRYPTO_memcmp(mac.data(), expected.data(), expected.size()) == 0;
    }
    require(verified, "conversation envelope authentication failed");
    return base64(body.at("payload"));
  }
  require(schema == "ndnsf-di-protected-request-v2" || schema == "ndnsf-di-protected-request-v3",
          "conversation envelope schema mismatch");
  const bool keyed = schema == "ndnsf-di-protected-request-v3";
  if (keyed) {
    fields(body, {"schema", "requestId", "expiresAtMs", "keyId", "nonce", "ciphertext"});
    metadata["keyId"] = string(body, "keyId");
  }
  else fields(body, {"schema", "requestId", "expiresAtMs", "nonce", "ciphertext"});
  const auto aad = metadata.dump(-1, ' ', true);
  const auto nonce = base64(body.at("nonce"));
  const auto ciphertext = base64(body.at("ciphertext"));
  require(nonce.size() == 12 && ciphertext.size() >= 16,
          "conversation envelope nonce or tag invalid");
  for (const auto& key : keys) {
    if (keyed && key.id != string(body, "keyId")) continue;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(
      EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!cipher) throw std::runtime_error("conversation cipher allocation failed");
    std::string plaintext(ciphertext.size(), '\0');
    int count = 0, written = 0;
    const auto payloadSize = ciphertext.size() - 16;
    const bool initialized =
      EVP_DecryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
      EVP_DecryptInit_ex(cipher.get(), nullptr, nullptr, key.bytes.data(),
        reinterpret_cast<const unsigned char*>(nonce.data())) == 1 &&
      EVP_DecryptUpdate(cipher.get(), nullptr, &count,
        reinterpret_cast<const unsigned char*>(aad.data()), static_cast<int>(aad.size())) == 1 &&
      EVP_DecryptUpdate(cipher.get(), reinterpret_cast<unsigned char*>(plaintext.data()), &written,
        reinterpret_cast<const unsigned char*>(ciphertext.data()), static_cast<int>(payloadSize)) == 1 &&
      EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_TAG, 16,
        const_cast<char*>(ciphertext.data() + payloadSize)) == 1;
    if (initialized && EVP_DecryptFinal_ex(cipher.get(),
          reinterpret_cast<unsigned char*>(plaintext.data()) + written, &count) == 1) {
      plaintext.resize(static_cast<std::size_t>(written + count));
      return plaintext;
    }
    OPENSSL_cleanse(plaintext.data(), plaintext.size());
  }
  throw std::invalid_argument("conversation envelope authentication failed");
}

std::string nativeConversationPrefixDigest(const std::vector<std::int64_t>& tokens)
{
  for (const auto token : tokens) require(token >= 0, "conversation token must be nonnegative");
  return digest({{"schema", "ndnsf-di-prefix-v1"}, {"tokenIds", tokens}});
}

std::string nativeSignConversationCheckpoint(
  NativeJson value, const std::vector<std::uint8_t>& key)
{
  validateUnsigned(value);
  const auto hash = digest(value);
  value["checkpointDigest"] = hash;
  value["signature"] = signature(hash, key);
  const auto wire = nativeConversationCanonicalJson(value);
  require(wire.size() <= maxCheckpointBytes, "conversation checkpoint exceeds bound");
  return wire;
}

NativeJson nativeReadConversationCheckpoint(
  const std::string& wire, const std::vector<std::vector<std::uint8_t>>& keys,
  std::uint64_t nowMs)
{
  require(!wire.empty() && wire.size() <= maxCheckpointBytes, "conversation checkpoint exceeds bound");
  require(!keys.empty() && keys.size() <= 16 && nowMs > 0,
          "conversation verification configuration invalid");
  auto value = nativeParseJson(wire);
  require(nativeConversationCanonicalJson(value) == wire, "conversation checkpoint is not canonical");
  const auto claimed = string(value, "checkpointDigest");
  const auto signedHash = string(value, "signature");
  auto unsignedValue = value;
  unsignedValue.erase("checkpointDigest");
  unsignedValue.erase("signature");
  validateUnsigned(unsignedValue);
  requireDigest(claimed);
  require(claimed == digest(unsignedValue), "conversation checkpoint digest mismatch");
  require(signedHash.size() == 64 && signedHash.find_first_not_of("0123456789abcdef") ==
          std::string::npos, "conversation checkpoint signature malformed");
  bool verified = false;
  for (const auto& key : keys) {
    const auto expected = signature(claimed, key);
    verified |= CRYPTO_memcmp(expected.data(), signedHash.data(), expected.size()) == 0;
  }
  require(verified, "conversation checkpoint authentication failed");
  require(integer(value, "expiresAtMs") > nowMs, "conversation checkpoint expired");
  return value;
}

void nativeValidateConversationTranscript(const NativeJson& value, const NativeJson& checkpoint,
  std::optional<std::size_t> nativeInitialPromptTokenCount)
{
  require(nativeConversationCanonicalJson(value).size() <= maxTranscriptBytes, "conversation transcript exceeds bound");
  fields(value, {"schema", "conversationId", "contextEpoch", "requesterIdentity", "serviceName",
    "securityDomainDigest", "applicationMessages", "tokenizerDigest", "chatTemplateDigest",
    "canonicalTokenIds", "prefixDigest", "prefixTokenCount", "providerRoleReceipts",
    "checkpointDigest", "planRoleMapDigest", "createdAtMs", "expiresAtMs"});
  require(string(value, "schema") == "ndnsf-di-conversation-transcript-v1", "conversation transcript schema mismatch");
  identity(value);
  requireDigest(string(value, "tokenizerDigest"));
  requireDigest(string(value, "chatTemplateDigest"));
  requireDigest(string(value, "prefixDigest"));
  requireDigest(string(value, "checkpointDigest"));
  const auto& tokens = value.at("canonicalTokenIds");
  require(tokens.is_array() && !tokens.empty(), "conversation transcript token list missing");
  std::vector<std::int64_t> ids;
  for (const auto& token : tokens) {
    require(token.is_number_integer() &&
      (!token.is_number_unsigned() || token.get<std::uint64_t>() <=
       static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())),
      "conversation token type invalid");
    ids.push_back(token.get<std::int64_t>());
  }
  require(integer(value, "prefixTokenCount") == ids.size() &&
          string(value, "prefixDigest") == nativeConversationPrefixDigest(ids),
          "conversation transcript prefix mismatch");
  require(integer(value, "contextEpoch") > 0 && integer(value, "createdAtMs") > 0 &&
          integer(value, "expiresAtMs") > integer(value, "createdAtMs"),
          "conversation transcript lifetime invalid");
  base64(value.at("applicationMessages"));
  require(value.at("providerRoleReceipts").is_array(), "conversation transcript receipts invalid");
  std::set<std::string> roles;
  const auto runtimePrefix = nativeInitialPromptTokenCount ? nativeGenerationStatePrefixDigest(
    ids, *nativeInitialPromptTokenCount, string(value, "tokenizerDigest")) : std::string{};
  std::string receiptPrefix;
  for (const auto& encoded : value.at("providerRoleReceipts")) {
    auto receipt = nativeParseJson(base64(encoded));
    const auto role = string(receipt, "roleName");
    const auto receiptDigest = string(receipt, "receiptDigest");
    require(roles.insert(role).second && checkpoint.at("roleReceiptDigests").contains(role) &&
            checkpoint.at("roleReceiptDigests").at(role) == receiptDigest,
            "conversation transcript role receipt mismatch");
    for (const auto field : {"conversationId", "requesterIdentity", "serviceName",
          "securityDomainDigest", "planRoleMapDigest", "prefixTokenCount"}) {
      if (receipt.at(field) != checkpoint.at(field))
        throw std::invalid_argument(std::string("conversation receipt scope mismatch: ") + field);
    }
    const auto currentPrefix = string(receipt, "prefixDigest");
    if (receiptPrefix.empty()) receiptPrefix = currentPrefix;
    require(currentPrefix == receiptPrefix &&
            (currentPrefix == string(checkpoint, "logicalPrefixDigest") ||
             (!runtimePrefix.empty() && currentPrefix == runtimePrefix)) &&
            receipt.at("successorContextEpoch") == checkpoint.at("contextEpoch") &&
            receipt.at("parentContextEpoch") == checkpoint.at("parentContextEpoch") &&
            integer(receipt, "expiresAtMs") >= integer(checkpoint, "expiresAtMs"),
            "conversation receipt lineage mismatch");
    receipt.erase("receiptDigest");
    receipt.erase("signature");
    require(receiptDigest == digest(receipt), "conversation transcript receipt digest mismatch");
  }
  require(roles.size() == checkpoint.at("roleReceiptDigests").size(),
          "conversation transcript role receipts incomplete");
  for (const auto field : {"conversationId", "contextEpoch", "requesterIdentity", "serviceName",
        "securityDomainDigest", "planRoleMapDigest", "checkpointDigest", "prefixTokenCount", "expiresAtMs"})
    require(value.at(field) == checkpoint.at(field), "conversation transcript checkpoint binding mismatch");
  require(value.at("prefixDigest") == checkpoint.at("logicalPrefixDigest"),
          "conversation transcript logical prefix mismatch");
}

} // namespace ndnsf::di
