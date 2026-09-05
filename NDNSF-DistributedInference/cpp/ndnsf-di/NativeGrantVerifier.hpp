#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_GRANT_VERIFIER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_GRANT_VERIFIER_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf::di {

/**
 * Recipient key material for one Provider identity.
 *
 * The envelope algorithm is chosen by the grant wire encoding and must match
 * the recipient key kind, exactly like the Python verifier:
 *  - Ed25519 seed  -> X25519-AESGCM-SHA256 (Edwards->Montgomery conversion);
 *  - EC P-256 PEM  -> ECDH-P256-AESGCM-SHA256.
 */
struct NativeRecipientKey
{
  enum class Kind
  {
    Ed25519Seed, ///< 32-byte raw Ed25519 private seed
    EcP256Pem,   ///< PKCS#8 PEM EC P-256 private key
  };

  Kind kind = Kind::Ed25519Seed;
  std::string material;
};

/** Outcome of one grant verification + content-key unwrap. */
struct NativeGrantVerificationResult
{
  bool verified = false;
  /** Empty on success; otherwise the registered rejection reason. */
  std::string reason;
  std::vector<std::uint8_t> contentKey;
};

/**
 * Verify and unwrap a KeyGrantV1 (canonical JSON wire bytes) inside the
 * Provider boundary, byte-compatible with the Python verifier.
 *
 * Every binding mismatch, a bad authority signature, an inconsistent grant
 * digest, expiry, or an envelope authentication failure fails closed before
 * any content key is exposed.  The rejection reasons belong to the
 * DI_PROTECTED_GRANT_REJECTED family (T002) or preserve the existing
 * DI_PROTECTED_RUNTIME_BINDING_MISMATCH semantics for binding mismatches.
 */
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
                           std::uint64_t nowMs);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_GRANT_VERIFIER_HPP
