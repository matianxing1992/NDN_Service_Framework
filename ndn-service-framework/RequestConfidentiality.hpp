#ifndef NDN_SERVICE_FRAMEWORK_REQUEST_CONFIDENTIALITY_HPP
#define NDN_SERVICE_FRAMEWORK_REQUEST_CONFIDENTIALITY_HPP

#include "ControllerVersion.hpp"
#include "HybridMessageCrypto.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ndn_service_framework {

/** Canonical, non-secret binding for one request attempt or stream event. */
struct RequestSecurityBinding
{
  static constexpr uint32_t TYPE = 0xF810;

  ndn::Name serviceName;
  ndn::Name requestId;
  uint64_t attempt = 0;
  ControllerVersion controllerVersion;
  ndn::Name userEncryptionCertName;
  std::string userEncryptionCertDigest;
  ndn::Name providerEncryptionCertName;
  std::string providerEncryptionCertDigest;
  std::string selectionDigest;
  ndn::Name inputDataName;
  std::string segmentOrEventId;

  bool isValid() const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
  ndn::Buffer canonicalAad() const;
  std::string digestHex() const;
};

/** Fresh request-scoped keys.  The Provider consumes the bundle once. */
struct RequestKeyBundle
{
  ndn::Buffer inputKey;
  ndn::Buffer responseKey;
  std::string keyId;
  uint64_t createdAtMs = 0;
  uint64_t expiresAtMs = 0;
  bool consumed = false;

  bool isValid(uint64_t nowMs) const;
  void zeroize() noexcept;
};

/** RSA-OAEP encrypted key bundle bound to one selected Provider. */
struct SelectionKeyEnvelope
{
  static constexpr uint32_t TYPE = 0xF830;

  RequestSecurityBinding binding;
  std::string envelopeAlgorithm = "RSA-OAEP-SHA256";
  ndn::Buffer wrappedKeys;
  uint64_t createdAtMs = 0;
  uint64_t expiresAtMs = 0;

  bool isValid(uint64_t nowMs) const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
};

/** AES-256-GCM ciphertext and its canonical binding digest. */
struct AeadEnvelope
{
  static constexpr uint32_t TYPE = 0xF820;

  std::string algorithm = "AES-256-GCM";
  std::string keyId;
  ndn::Buffer nonce;
  ndn::Buffer ciphertext;
  ndn::Buffer tag;
  ndn::Buffer aadDigest;
  std::string segmentOrEventId;

  bool isValid() const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
};

enum class RequestCryptoFailure
{
  NONE,
  INVALID_BINDING,
  INVALID_KEY_BUNDLE,
  EXPIRED_KEY,
  WRONG_KEY_ID,
  INVALID_ENVELOPE,
  AUTHENTICATION_FAILED,
  NONCE_REUSE,
  WRONG_RECIPIENT,
};

const char* requestCryptoFailureName(RequestCryptoFailure failure) noexcept;

RequestKeyBundle generateRequestKeyBundle(uint64_t nowMs, uint64_t expiresAtMs);

SelectionKeyEnvelope wrapSelectionKeyEnvelope(
  const RequestKeyBundle& keys,
  const RequestSecurityBinding& binding,
  ndn::span<const uint8_t> recipientPublicKey,
  uint64_t nowMs);

bool unwrapSelectionKeyEnvelope(
  const SelectionKeyEnvelope& envelope,
  const RequestSecurityBinding& expectedBinding,
  const ndn::Name& recipientCertificateName,
  const ndn::security::KeyChain& keyChain,
  uint64_t nowMs,
  RequestKeyBundle& keys,
  RequestCryptoFailure* failure = nullptr);

AeadEnvelope encryptRequestContent(
  const ndn::Buffer& key,
  const std::string& keyId,
  const RequestSecurityBinding& binding,
  ndn::span<const uint8_t> plaintext);

bool decryptRequestContent(
  const ndn::Buffer& key,
  const std::string& expectedKeyId,
  const RequestSecurityBinding& binding,
  const AeadEnvelope& envelope,
  ndn::Buffer& plaintext,
  RequestCryptoFailure* failure = nullptr);

/** Bounded per-invocation nonce replay registry. */
class NonceRegistry
{
public:
  static constexpr size_t NONCE_SIZE = HybridMessageCrypto::NONCE_SIZE;

  bool reserve(const std::string& keyId,
               ndn::span<const uint8_t> nonce,
               std::chrono::steady_clock::time_point expiresAt);
  bool reserve(const std::string& keyId,
               ndn::span<const uint8_t> nonce,
               std::chrono::milliseconds lifetime);
  /** Atomically reserve a group of authenticated nonces for one key. */
  bool reserveBatch(const std::string& keyId,
                    const std::vector<ndn::Buffer>& nonces,
                    std::chrono::milliseconds lifetime);
  void invalidate(const std::string& keyId);
  void clearExpired(std::chrono::steady_clock::time_point now =
                    std::chrono::steady_clock::now());
  size_t size() const;

private:
  struct Entry
  {
    std::set<std::string> nonces;
    std::chrono::steady_clock::time_point expiresAt;
  };

  static std::string nonceKey(ndn::span<const uint8_t> nonce);
  mutable std::mutex m_mutex;
  std::map<std::string, Entry> m_entries;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_REQUEST_CONFIDENTIALITY_HPP
