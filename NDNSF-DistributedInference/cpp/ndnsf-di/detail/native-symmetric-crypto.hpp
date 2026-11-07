#ifndef NDNSF_DI_DETAIL_NATIVE_SYMMETRIC_CRYPTO_HPP
#define NDNSF_DI_DETAIL_NATIVE_SYMMETRIC_CRYPTO_HPP

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>

#include <climits>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::di::detail {

/** Existing native HKDF-SHA256 primitive, shared by cache and stored-model formats. */
inline std::vector<std::uint8_t>
deriveSymmetricKey(const std::vector<std::uint8_t>& key, const std::string& info)
{
  if (key.empty() || key.size() > INT_MAX || info.size() > INT_MAX)
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: invalid HKDF input");
  std::vector<std::uint8_t> out(32);
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
    EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
  if (!ctx)
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: HKDF allocation failed");
  std::size_t size = out.size();
  const bool ok = EVP_PKEY_derive_init(ctx.get()) > 0 &&
    EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()) > 0 &&
    EVP_PKEY_CTX_set1_hkdf_key(ctx.get(), key.data(), key.size()) > 0 &&
    EVP_PKEY_CTX_add1_hkdf_info(ctx.get(),
      reinterpret_cast<const unsigned char*>(info.data()), info.size()) > 0 &&
    EVP_PKEY_derive(ctx.get(), out.data(), &size) > 0 && size == out.size();
  if (!ok) {
    OPENSSL_cleanse(out.data(), out.size());
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: assembled key derivation failed");
  }
  return out;
}

/** AES-256-GCM ciphertext ends with its 16-byte authentication tag. */
inline std::vector<std::uint8_t>
cryptSymmetric(bool encrypt, const std::vector<std::uint8_t>& key,
               const std::vector<std::uint8_t>& nonce,
               const std::vector<std::uint8_t>& input, const std::string& aad)
{
  if (key.size() != 32 || nonce.size() != 12 || input.size() > INT_MAX ||
      aad.size() > INT_MAX || (!encrypt && input.size() < 16))
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: assembled AEAD input is invalid");
  const std::size_t body = encrypt ? input.size() : input.size() - 16;
  std::vector<std::uint8_t> out(body + 16);
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(
    EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!ctx)
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: AEAD allocation failed");
  int written = 0;
  int final = 0;
  bool ok = EVP_CipherInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
                             key.data(), nonce.data(), encrypt ? 1 : 0) == 1 &&
    EVP_CipherUpdate(ctx.get(), nullptr, &written,
      reinterpret_cast<const unsigned char*>(aad.data()), aad.size()) == 1 &&
    EVP_CipherUpdate(ctx.get(), out.data(), &written, input.data(), body) == 1;
  if (!encrypt)
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16,
      const_cast<unsigned char*>(input.data() + body)) == 1;
  ok = ok && EVP_CipherFinal_ex(ctx.get(), out.data() + written, &final) == 1;
  if (encrypt)
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16,
                                  out.data() + body) == 1;
  if (!ok) {
    OPENSSL_cleanse(out.data(), out.size());
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: assembled entry failed AEAD authentication");
  }
  out.resize(body + (encrypt ? 16 : 0));
  return out;
}

} // namespace ndnsf::di::detail

#endif // NDNSF_DI_DETAIL_NATIVE_SYMMETRIC_CRYPTO_HPP
