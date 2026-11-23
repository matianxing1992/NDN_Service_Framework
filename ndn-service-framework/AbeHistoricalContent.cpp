#include "AbeHistoricalContent.hpp"

#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/name.hpp>
#include <nac-abe/algo/abe-support.hpp>

namespace ndn_service_framework {
namespace {

ndn::Block
parseEnvelope(const std::string& content)
{
  const auto bytes = ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(content.data()),
                                             content.size());
  ndn::Block envelope(bytes);
  if (envelope.type() != ndn::tlv::Content || envelope.size() != bytes.size())
    throw ndn::tlv::Error("expected one complete encrypted Content block");
  envelope.parse();
  return envelope;
}

} // namespace

bool
parseHistoricalCkContent(const std::string& content, HistoricalCkContent& parsed)
{
  try {
    const auto envelope = parseEnvelope(content);
    if (envelope.elements_size() != 1)
      return false;
    const auto& wrapped = envelope.get(ndn::nacabe::TLV_EncryptedAesKey);
    if (wrapped.value_size() == 0)
      return false;
    parsed.wrappedKey.assign(wrapped.value_begin(), wrapped.value_end());
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
parseHistoricalPayloadContent(const std::string& content, HistoricalPayloadContent& parsed)
{
  try {
    const auto envelope = parseEnvelope(content);
    if (envelope.elements_size() != 3)
      return false;
    const auto& encrypted = envelope.get(ndn::nacabe::TLV_EncryptedContent);
    const auto plainTextSize = ndn::readNonNegativeIntegerAs<uint32_t>(
      envelope.get(ndn::nacabe::TLV_PlainTextSize));
    const auto& key = envelope.get(ndn::tlv::Name);
    if (encrypted.value_size() == 0 || ndn::Name(key).empty())
      return false;
    parsed = {std::string(encrypted.value_begin(), encrypted.value_end()), plainTextSize,
              std::string(key.begin(), key.end())};
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
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
    if (plain.size() != payloadContent.plainTextSize)
      return std::nullopt;
    return std::string(reinterpret_cast<const char*>(plain.data()), plain.size());
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}

} // namespace ndn_service_framework
