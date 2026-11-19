#include "SealedField.hpp"

#include <cstring>
#include <stdexcept>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace ndn_service_framework {
namespace {

std::string
toBase64(const std::vector<uint8_t>& bytes)
{
  std::string out(4 * ((bytes.size() + 2) / 3), '\0');
  const auto written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                       bytes.data(), static_cast<int>(bytes.size()));
  out.resize(written);
  return out;
}

bool
fromBase64(const std::string& text, std::vector<uint8_t>& bytes)
{
  if (text.empty() || text.size() % 4 != 0)
    return false;
  bytes.assign(text.size() / 4 * 3, 0);
  const auto written = EVP_DecodeBlock(bytes.data(),
                                       reinterpret_cast<const unsigned char*>(text.data()),
                                       static_cast<int>(text.size()));
  if (written < 0)
    return false;
  std::size_t padding = 0;
  if (!text.empty() && text.back() == '=')
    ++padding;
  if (text.size() > 1 && text[text.size() - 2] == '=')
    ++padding;
  bytes.resize(static_cast<std::size_t>(written) - padding);
  return true;
}

bool
crypt(bool encrypting, const std::vector<uint8_t>& key, std::vector<uint8_t>& nonce,
      const std::string& input, const std::string& associatedData, std::string& output)
{
  if (key.size() != 32 || nonce.size() != 12)
    return false;
  EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
  if (context == nullptr)
    return false;
  constexpr std::size_t TAG_BYTES = 16;
  bool ok = false;
  do {
    if (EVP_CipherInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr,
                          encrypting ? 1 : 0) != 1)
      break;
    if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) != 1)
      break;
    if (EVP_CipherInit_ex(context, nullptr, nullptr, key.data(), nonce.data(),
                          encrypting ? 1 : 0) != 1)
      break;
    int written = 0;
    if (!associatedData.empty() &&
        EVP_CipherUpdate(context, nullptr, &written,
                         reinterpret_cast<const unsigned char*>(associatedData.data()),
                         static_cast<int>(associatedData.size())) != 1)
      break;

    // On decrypt the tag travels at the end of the input and has to be installed before the final
    // call, which is what actually authenticates the ciphertext.
    std::size_t bodyBytes = input.size();
    std::vector<uint8_t> tag(TAG_BYTES);
    if (encrypting) {
      bodyBytes = input.size();
    }
    else {
      if (input.size() <= TAG_BYTES)
        break;
      bodyBytes = input.size() - TAG_BYTES;
      std::memcpy(tag.data(), input.data() + bodyBytes, TAG_BYTES);
      if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, TAG_BYTES, tag.data()) != 1)
        break;
    }

    std::string buffer(bodyBytes + 32, '\0');
    if (EVP_CipherUpdate(context, reinterpret_cast<unsigned char*>(buffer.data()), &written,
                         reinterpret_cast<const unsigned char*>(input.data()),
                         static_cast<int>(bodyBytes)) != 1)
      break;
    int total = written;
    if (EVP_CipherFinal_ex(context, reinterpret_cast<unsigned char*>(buffer.data()) + total,
                           &written) != 1)
      break;
    total += written;
    buffer.resize(static_cast<std::size_t>(total));
    if (encrypting) {
      if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, TAG_BYTES, tag.data()) != 1)
        break;
      buffer.append(reinterpret_cast<const char*>(tag.data()), tag.size());
    }
    output.assign(buffer.begin(), buffer.end());
    ok = true;
  } while (false);
  EVP_CIPHER_CTX_free(context);
  return ok;
}

} // namespace

bool
isSealedField(const std::string& field)
{
  return field.rfind(SEALED_FIELD_PREFIX, 0) == 0;
}

std::string
sealField(const std::vector<uint8_t>& key, const std::string& plaintext,
          const std::string& associatedData)
{
  std::vector<uint8_t> nonce(12);
  if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1)
    throw std::runtime_error("cannot generate a nonce for the sealed field");
  std::string sealed;
  if (!crypt(true, key, nonce, plaintext, associatedData, sealed))
    throw std::runtime_error("cannot seal the field");
  std::vector<uint8_t> packed;
  packed.reserve(nonce.size() + sealed.size());
  packed.insert(packed.end(), nonce.begin(), nonce.end());
  packed.insert(packed.end(), sealed.begin(), sealed.end());
  return std::string(SEALED_FIELD_PREFIX) + toBase64(packed);
}

bool
openField(const std::vector<uint8_t>& key, const std::string& field,
          const std::string& associatedData, std::string& plaintext)
{
  if (!isSealedField(field))
    return false;
  std::vector<uint8_t> packed;
  if (!fromBase64(field.substr(std::strlen(SEALED_FIELD_PREFIX)), packed) ||
      packed.size() <= 12 + 16)
    return false;
  std::vector<uint8_t> nonce(packed.begin(), packed.begin() + 12);
  const std::string sealed(reinterpret_cast<const char*>(packed.data()) + 12,
                           packed.size() - 12);
  std::string opened;
  if (!crypt(false, key, nonce, sealed, associatedData, opened))
    return false;
  plaintext = std::move(opened);
  return true;
}

} // namespace ndn_service_framework
