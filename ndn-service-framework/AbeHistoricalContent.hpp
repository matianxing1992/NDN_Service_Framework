#ifndef NDNSF_ABE_HISTORICAL_CONTENT_HPP
#define NDNSF_ABE_HISTORICAL_CONTENT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ndn_service_framework {

/** The parts of a protected object a receiver can see.
 *
 * Both envelopes are NDN TLV blobs written by the NAC-ABE producer: the CK packet carries the wrapped
 * symmetric key, and the payload carries the encrypted content plus its plaintext length and the CK name.
 */
struct HistoricalCkContent
{
  std::string wrappedKey;
};

struct HistoricalPayloadContent
{
  std::string encryptedContent;
  uint32_t plainTextSize = 0;
  /** The CK name the producer wrote into the payload (TLV 0x0249), when it carries one. This is what binds a payload
   *  to the key that protects it: without it a reader can only trust that some CK decrypted it. */
  std::string ckName;
};

/** Read the wrapped key out of a CK packet's content. */
bool
parseHistoricalCkContent(const std::string& content, HistoricalCkContent& parsed);

/** Read the encrypted content and its length out of a payload packet's content. */
bool
parseHistoricalPayloadContent(const std::string& content, HistoricalPayloadContent& parsed);

/** Decrypt a parsed payload with a decryption key issued for its generation.
 *
 * `publicParameters` is the generation's public parameter wire and `decryptionKey` the key issued for
 * it; both come from the Controller, which is why this takes them instead of fetching anything.
 */
std::optional<std::string>
decryptHistoricalPayload(const std::string& publicParameters,
                         const HistoricalCkContent& ckContent,
                         const HistoricalPayloadContent& payloadContent,
                         const std::vector<std::uint8_t>& decryptionKey);

} // namespace ndn_service_framework

#endif // NDNSF_ABE_HISTORICAL_CONTENT_HPP
