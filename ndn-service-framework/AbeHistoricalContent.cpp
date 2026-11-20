#include "AbeHistoricalContent.hpp"

#include <cstring>

#include <nac-abe/algo/abe-support.hpp>

namespace ndn_service_framework {
namespace {

bool
readVarNumber(const std::string& bytes, std::size_t& offset, uint32_t& value)
{
  if (offset >= bytes.size())
    return false;
  const auto first = static_cast<uint8_t>(bytes[offset++]);
  if (first < 0xfd) {
    value = first;
    return true;
  }
  const std::size_t extra = first == 0xfd ? 2 : first == 0xfe ? 4 : 8;
  if (bytes.size() - offset < extra)
    return false;
  value = 0;
  for (std::size_t i = 0; i < extra; ++i)
    value = (value << 8) | static_cast<uint8_t>(bytes[offset++]);
  return true;
}

/** Walk the top-level TLV of `bytes` and return the value of the first field of `wanted`. */
bool
findField(const std::string& bytes, uint32_t wanted, std::string& value)
{
  std::size_t offset = 0;
  std::uint32_t type = 0;
  std::uint32_t length = 0;
  if (!readVarNumber(bytes, offset, type) || !readVarNumber(bytes, offset, length) ||
      bytes.size() - offset < length)
    return false;
  const std::string body = bytes.substr(offset, length);
  std::size_t inner = 0;
  while (inner < body.size()) {
    std::uint32_t fieldType = 0;
    std::uint32_t fieldLength = 0;
    if (!readVarNumber(body, inner, fieldType) || !readVarNumber(body, inner, fieldLength) ||
        body.size() - inner < fieldLength)
      return false;
    if (fieldType == wanted) {
      value = body.substr(inner, fieldLength);
      return true;
    }
    inner += fieldLength;
  }
  return false;
}

} // namespace

bool
parseHistoricalCkContent(const std::string& content, HistoricalCkContent& parsed)
{
  std::string wrapped;
  if (!findField(content, 0x0259, wrapped) || wrapped.empty())
    return false;
  parsed.wrappedKey = std::move(wrapped);
  return true;
}

bool
parseHistoricalPayloadContent(const std::string& content, HistoricalPayloadContent& parsed)
{
  std::string encrypted;
  std::string size;
  if (!findField(content, 0x025A, encrypted) || encrypted.empty() ||
      !findField(content, 0x025B, size) || size.size() != 1)
    return false;
  parsed.encryptedContent = std::move(encrypted);
  parsed.plainTextSize = static_cast<uint8_t>(size[0]);
  // Optional on the fixture's hand-built envelopes, present on real Producer output.
  std::string ckName;
  if (findField(content, 0x0249, ckName) && !ckName.empty())
    parsed.ckName = std::move(ckName);
  return true;
}

std::optional<std::string>
decryptHistoricalPayload(const std::string& publicParameters,
                         const HistoricalCkContent& ckContent,
                         const HistoricalPayloadContent& payloadContent,
                         const std::vector<std::uint8_t>& decryptionKey)
{
  if (publicParameters.empty() || ckContent.wrappedKey.empty() ||
      payloadContent.encryptedContent.empty() || decryptionKey.empty())
    return std::nullopt;
  try {
    ndn::nacabe::algo::PublicParams params;
    params.fromBuffer(ndn::Buffer(reinterpret_cast<const uint8_t*>(publicParameters.data()),
                                  publicParameters.size()));
    ndn::nacabe::algo::PrivateKey key;
    key.fromBuffer(ndn::Buffer(decryptionKey.data(), decryptionKey.size()));

    ndn::nacabe::algo::CipherText cipherText;
    cipherText.m_contentKey = std::make_shared<ndn::nacabe::algo::ContentKey>(
      std::string(), ndn::Buffer(reinterpret_cast<const uint8_t*>(ckContent.wrappedKey.data()),
                                 ckContent.wrappedKey.size()));
    cipherText.m_content = ndn::Buffer(
      reinterpret_cast<const uint8_t*>(payloadContent.encryptedContent.data()),
      payloadContent.encryptedContent.size());
    cipherText.m_plainTextSize = payloadContent.plainTextSize;

    const auto plain = ndn::nacabe::algo::ABESupport::getInstance().kpDecrypt(params, key,
                                                                             cipherText);
    return std::string(reinterpret_cast<const char*>(plain.data()), plain.size());
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}

} // namespace ndn_service_framework
