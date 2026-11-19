#include "AbeHistoricalKeyRequest.hpp"

#include "AbeHistoricalKey.hpp"

#include <nac-abe/ndn-crypto/data-enc-dec.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <regex>
#include <stdexcept>
#include <vector>

#include <ndn-cxx/security/interest-signer.hpp>
#include <ndn-cxx/util/sha256.hpp>

namespace ndn_service_framework {
namespace {

constexpr char BODY_SCHEMA[] = "ndnsf-abe-history-dkey-request-v2";
constexpr char REPLY_SCHEMA[] = "ndnsf-abe-history-dkey-reply-v2";

std::string
sha256Hex(const std::string& bytes)
{
  ndn::util::Sha256 hash;
  if (!bytes.empty())
    hash << bytes;
  auto hex = hash.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return hex;
}

bool
parseU64(const std::string& text, uint64_t& value)
{
  if (text.empty() || text.size() > 20 ||
      !std::all_of(text.begin(), text.end(), [] (unsigned char ch) { return std::isdigit(ch) != 0; }))
    return false;
  value = std::strtoull(text.c_str(), nullptr, 10);
  return true;
}

} // namespace

namespace {

/** A policy is an attribute expression, never a place to smuggle envelope fields.
 *
 * The canonical body is line oriented, so a control character inside a field would move the parse to
 * another field. Rejecting them here keeps the encoding unambiguous for every reader.
 */
bool
isSafePolicy(const std::string& policy)
{
  if (policy.empty() || policy.size() > 512)
    return false;
  return std::all_of(policy.begin(), policy.end(), [] (unsigned char ch) {
    return ch >= 0x20 && ch != 0x7f;
  });
}

} // namespace

bool
HistoricalKeyRequest::operator==(const HistoricalKeyRequest& other) const
{
  return controllerPrefix == other.controllerPrefix && generation == other.generation &&
    epoch == other.epoch && policy == other.policy &&
    recipientCertificate == other.recipientCertificate && mintedAtMs == other.mintedAtMs &&
    expiresAtMs == other.expiresAtMs;
}

std::string
encodeHistoricalKeyRequest(const HistoricalKeyRequest& request)
{
  if (!isSafePolicy(request.policy))
    throw std::invalid_argument("historical key request policy is not a plain attribute expression");
  std::string body(BODY_SCHEMA);
  body += "\ncontroller " + request.controllerPrefix.toUri(ndn::name::UriFormat::CANONICAL);
  body += "\ngeneration " + std::to_string(request.generation);
  body += "\nepoch " + std::to_string(request.epoch);
  body += "\nobject " + request.objectName.toUri(ndn::name::UriFormat::CANONICAL);
  body += "\nrecipient " + request.recipientCertificate.toUri(ndn::name::UriFormat::CANONICAL);
  body += "\npolicy " + request.policy;
  body += "\nminted " + std::to_string(request.mintedAtMs);
  body += "\nexpires " + std::to_string(request.expiresAtMs);
  return body;
}

bool
decodeHistoricalKeyRequest(const std::string& body, HistoricalKeyRequest& request)
{
  try {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= body.size()) {
      const auto end = body.find('\n', begin);
      lines.push_back(body.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
      if (end == std::string::npos)
        break;
      begin = end + 1;
    }
    if (lines.size() != 9 || lines[0] != BODY_SCHEMA)
      return false;
    const auto field = [] (const std::string& line, const char* prefix) -> std::string {
      const std::string head(prefix);
      if (line.compare(0, head.size(), head) != 0)
        throw std::runtime_error("missing field");
      return line.substr(head.size());
    };
    HistoricalKeyRequest parsed;
    parsed.controllerPrefix = ndn::Name(field(lines[1], "controller "));
    if (!parseU64(field(lines[2], "generation "), parsed.generation) ||
        !parseU64(field(lines[3], "epoch "), parsed.epoch) ||
        !parseU64(field(lines[7], "minted "), parsed.mintedAtMs) ||
        !parseU64(field(lines[8], "expires "), parsed.expiresAtMs))
      return false;
    const auto objectText = field(lines[4], "object ");
    if (objectText.empty() || objectText.front() != '/')
      return false;
    parsed.objectName = ndn::Name(objectText);
    if (parsed.objectName.toUri(ndn::name::UriFormat::CANONICAL) != objectText)
      return false;
    const auto recipientText = field(lines[5], "recipient ");
    // A certificate name is absolute and round-trips to the text it was written as; anything else
    // would let the body and the name disagree about who is asking.
    if (recipientText.empty() || recipientText.front() != '/' ||
        std::any_of(recipientText.begin(), recipientText.end(),
                    [] (unsigned char ch) { return ch < 0x20 || ch == 0x7f; }))
      return false;
    parsed.recipientCertificate = ndn::Name(recipientText);
    if (parsed.recipientCertificate.toUri(ndn::name::UriFormat::CANONICAL) != recipientText)
      return false;
    parsed.policy = field(lines[6], "policy ");
    if (parsed.controllerPrefix.empty() || parsed.generation == 0 ||
        parsed.recipientCertificate.empty() || parsed.objectName.empty() ||
        !isSafePolicy(parsed.policy))
      return false;
    request = std::move(parsed);
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

std::string
historicalKeyRequestDigest(const HistoricalKeyRequest& request)
{
  return "sha256:" + sha256Hex(encodeHistoricalKeyRequest(request));
}

ndn::Name
makeHistoricalKeyRequestName(const ndn::Name& controllerPrefix,
                             const HistoricalKeyRequest& request)
{
  if (controllerPrefix.empty() || request.generation == 0)
    throw std::invalid_argument("historical key request needs a controller prefix and a generation");
  const auto digest = historicalKeyRequestDigest(request);
  ndn::Name name(controllerPrefix);
  name.append("NDNSF").append("HISTORY").append("DKEY");
  name.append(std::to_string(request.generation));
  name.append(std::to_string(request.epoch));
  name.append(digest.substr(7));
  return name;
}

void
signHistoricalKeyRequest(ndn::Interest& interest, const HistoricalKeyRequest& request,
                         ndn::KeyChain& keyChain, const ndn::security::SigningInfo& signing)
{
  const auto body = encodeHistoricalKeyRequest(request);
  interest.setName(makeHistoricalKeyRequestName(request.controllerPrefix, request));
  interest.setApplicationParameters(ndn::Buffer(reinterpret_cast<const uint8_t*>(body.data()),
                                                body.size()));
  ndn::security::InterestSigner signer(keyChain);
  signer.makeSignedInterest(interest, signing);
}

bool
matchHistoricalKeyRequestName(const ndn::Name& name, const ndn::Name& controllerPrefix,
                              const HistoricalKeyRequest& request)
{
  if (controllerPrefix.empty())
    return false;
  // A Signed Interest appends its signature components to the name, so the request name is the
  // prefix and the trailing signature must not be read as part of it.
  const auto expected = makeHistoricalKeyRequestName(controllerPrefix, request);
  if (name.size() < expected.size() || name.getSubName(0, expected.size()) != expected)
    return false;
  return expected.get(-1).toUri() == historicalKeyRequestDigest(request).substr(7);
}

bool
buildHistoricalKeyReply(const HistoricalKeyRequest& request, const std::string& keyBytes,
                        const ndn::security::Certificate& recipient,
                        uint64_t replyExpiresAtMs,
                        const std::function<void(ndn::Data&)>& sign, ndn::Data& result,
                        const ndn::Name& replyName)
{
  if (request.generation == 0 || keyBytes.empty() || request.expiresAtMs == 0 ||
      !isSafePolicy(request.policy) || request.objectName.empty())
    return false;
  // The reply must be readable only by the certificate the request named. Refusing here when the
  // caller hands over a different certificate keeps the seal and the claim from disagreeing.
  if (recipient.getName() != request.recipientCertificate)
    return false;
  // A reply may not outlive the request that asked for it.
  const auto expires = std::min(replyExpiresAtMs, request.expiresAtMs);
  if (expires == 0)
    return false;

  ndn::Block sealed;
  try {
    const auto& publicKey = recipient.getPublicKey();
    sealed = ndn::nacabe::encryptDataContentWithCK(
      ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(keyBytes.data()), keyBytes.size()),
      ndn::span<const uint8_t>(publicKey.data(), publicKey.size()));
  }
  catch (const std::exception&) {
    return false;
  }
  if (sealed.size() == 0)
    return false;

  std::string envelope(REPLY_SCHEMA);
  envelope += "\nrequest " + historicalKeyRequestDigest(request);
  envelope += "\nobject " + request.objectName.toUri(ndn::name::UriFormat::CANONICAL);
  envelope += "\nrecipient " + request.recipientCertificate.toUri(ndn::name::UriFormat::CANONICAL);
  envelope += "\ngeneration " + std::to_string(request.generation);
  envelope += "\nepoch " + std::to_string(request.epoch);
  envelope += "\npolicy " + request.policy;
  envelope += "\nexpires " + std::to_string(expires);
  envelope += "\nsealed " + std::to_string(sealed.size());
  envelope += "\n";

  const auto requestName = makeHistoricalKeyRequestName(request.controllerPrefix, request);
  // The reply answers the Interest that was expressed, whose name carries the requester's signature
  // components after the request name. The request name must stay its prefix, and exact naming is
  // what the forwarder accepts.
  if (replyName.empty())
    result.setName(requestName);
  else if (replyName.size() >= requestName.size() &&
           replyName.getSubName(0, requestName.size()) == requestName)
    result.setName(replyName);
  else
    return false;
  ndn::Buffer content(reinterpret_cast<const uint8_t*>(envelope.data()), envelope.size());
  content.insert(content.end(), sealed.begin(), sealed.end());
  result.setContent(content);
  result.setFreshnessPeriod(ndn::time::milliseconds(0));
  if (sign)
    sign(result);
  return true;
}

bool
verifyHistoricalKeyReply(const ndn::Data& reply, const HistoricalKeyRequest& request,
                         uint64_t nowMs, HistoricalKeyReply& accepted)
{
  try {
    const std::string content(reinterpret_cast<const char*>(reply.getContent().value()),
                              reply.getContent().value_size());
    // The envelope is nine lines and the sealed key follows them as bytes.
    std::vector<std::string> lines;
    std::size_t begin = 0;
    std::size_t envelopeEnd = std::string::npos;
    while (lines.size() < 9) {
      const auto end = content.find('\n', begin);
      if (end == std::string::npos)
        return false;
      lines.push_back(content.substr(begin, end - begin));
      begin = end + 1;
      envelopeEnd = end;
    }
    if (lines[0] != REPLY_SCHEMA)
      return false;
    const auto field = [] (const std::string& line, const char* prefix) -> std::string {
      const std::string head(prefix);
      if (line.compare(0, head.size(), head) != 0)
        throw std::runtime_error("missing field");
      return line.substr(head.size());
    };
    if (field(lines[1], "request ") != historicalKeyRequestDigest(request))
      return false;
    if (field(lines[2], "object ") !=
        request.objectName.toUri(ndn::name::UriFormat::CANONICAL))
      return false;
    const auto requestName = makeHistoricalKeyRequestName(request.controllerPrefix, request);
    if (reply.getName().size() < requestName.size() ||
        reply.getName().getSubName(0, requestName.size()) != requestName)
      return false;
    if (field(lines[3], "recipient ") !=
        request.recipientCertificate.toUri(ndn::name::UriFormat::CANONICAL))
      return false;
    uint64_t generation = 0;
    uint64_t epoch = 0;
    uint64_t expires = 0;
    uint64_t sealedBytes = 0;
    if (!parseU64(field(lines[4], "generation "), generation) ||
        !parseU64(field(lines[5], "epoch "), epoch) ||
        !parseU64(field(lines[7], "expires "), expires) ||
        !parseU64(field(lines[8], "sealed "), sealedBytes))
      return false;
    if (generation != request.generation || epoch != request.epoch ||
        field(lines[6], "policy ") != request.policy)
      return false;
    if (expires <= nowMs || expires > request.expiresAtMs)
      return false;
    // A sealed key is small and its claimed length is attacker-controlled, so bound it and require the
    // bytes to match exactly what the envelope promises.
    constexpr uint64_t MAX_SEALED_BYTES = 65536;
    if (sealedBytes == 0 || sealedBytes > MAX_SEALED_BYTES ||
        content.size() != static_cast<std::size_t>(envelopeEnd) + 1 + sealedBytes)
      return false;
    accepted.requestName = reply.getName();
    accepted.objectName = request.objectName;
    accepted.sealedKey = content.substr(envelopeEnd + 1, sealedBytes);
    accepted.generation = generation;
    accepted.epoch = epoch;
    accepted.expiresAtMs = expires;
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
unsealHistoricalKeyReply(const HistoricalKeyReply& reply, const ndn::security::Tpm& tpm,
                         const ndn::Name& certificateName, std::string& keyBytes)
{
  if (reply.sealedKey.empty())
    return false;
  try {
    const ndn::Block block(ndn::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(reply.sealedKey.data()), reply.sealedKey.size()));
    const auto plain = ndn::nacabe::decryptDataContent(block, tpm, certificateName);
    keyBytes.assign(reinterpret_cast<const char*>(plain.data()), plain.size());
    return !keyBytes.empty();
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
verifyHistoricalKeyReplySignature(const ndn::Data& reply,
                                  const ndn::security::Certificate& controllerCertificate)
{
  try {
    return ndn::security::verifySignature(reply, controllerCertificate.getPublicKey());
  }
  catch (const std::exception&) {
    return false;
  }
}

std::optional<ndn::Data>
HistoricalKeyBroker::answerValidated(const ndn::Interest& interest,
                                     const ndn::Name& controllerPrefix, uint64_t nowMs,
                                     const ndn::security::Certificate& signer)
{
  const auto parameters = interest.getApplicationParameters();
  HistoricalKeyRequest request;
  if (!decodeHistoricalKeyRequest(std::string(reinterpret_cast<const char*>(parameters.value()),
                                              parameters.value_size()), request))
    return std::nullopt;
  if (!matchHistoricalKeyRequestName(interest.getName(), controllerPrefix, request))
    return std::nullopt;
  constexpr uint64_t MAX_SKEW_MS = 60000;
  constexpr uint64_t MAX_AGE_MS = 300000;
  // A request that claims a very long validity would keep its digest in the replay set for that long,
  // so the lifetime itself is bounded instead of trusted.
  constexpr uint64_t MAX_TTL_MS = 600000;
  if (request.expiresAtMs <= nowMs || request.mintedAtMs > nowMs + MAX_SKEW_MS ||
      request.mintedAtMs + MAX_AGE_MS < nowMs ||
      request.expiresAtMs - request.mintedAtMs > MAX_TTL_MS)
    return std::nullopt;
  // Mint only for the certificate the request named.
  if (signer.getName() != request.recipientCertificate)
    return std::nullopt;
  return finish(request, nowMs, interest.getName(), signer);
}

std::optional<ndn::Data>
HistoricalKeyBroker::finish(const HistoricalKeyRequest& request, uint64_t nowMs,
                            const ndn::Name& replyName,
                            const ndn::security::Certificate& recipient)
{
  for (auto it = m_seen.begin(); it != m_seen.end();) {
    if (it->second > nowMs) {
      ++it;
      continue;
    }
    it = m_seen.erase(it);
  }
  const auto digest = historicalKeyRequestDigest(request);
  if (m_seen.count(digest))
    return std::nullopt;
  Material material;
  if (!m_materialLookup || !m_materialLookup(request.generation, request.epoch, material))
    return std::nullopt;
  HistoricalAbeMaterial keyMaterial{material.publicParameters, material.masterSecret};
  std::vector<std::uint8_t> key;
  try {
    key = issueHistoricalDecryptionKey(keyMaterial, request.policy);
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
  if (key.empty())
    return std::nullopt;
  const std::string keyBytes(reinterpret_cast<const char*>(key.data()), key.size());
  ndn::Data reply;
  if (!buildHistoricalKeyReply(request, keyBytes, recipient, request.expiresAtMs, m_replySigner,
                               reply, replyName))
    return std::nullopt;
  m_seen[digest] = request.expiresAtMs;
  return reply;
}

std::optional<ndn::Data>
HistoricalKeyBroker::answer(const ndn::Interest& interest, const ndn::Name& controllerPrefix,
                            uint64_t nowMs)
{
  const auto parameters = interest.getApplicationParameters();
  HistoricalKeyRequest request;
  if (!decodeHistoricalKeyRequest(std::string(reinterpret_cast<const char*>(parameters.value()),
                                              parameters.value_size()), request))
    return std::nullopt;
  if (!matchHistoricalKeyRequestName(interest.getName(), controllerPrefix, request))
    return std::nullopt;

  constexpr uint64_t MAX_SKEW_MS = 60000;
  constexpr uint64_t MAX_AGE_MS = 300000;
  // A request that claims a very long validity would keep its digest in the replay set for that long,
  // so the lifetime itself is bounded instead of trusted.
  constexpr uint64_t MAX_TTL_MS = 600000;
  if (request.expiresAtMs <= nowMs || request.mintedAtMs > nowMs + MAX_SKEW_MS ||
      request.mintedAtMs + MAX_AGE_MS < nowMs ||
      request.expiresAtMs - request.mintedAtMs > MAX_TTL_MS)
    return std::nullopt;

  ndn::security::Certificate signer;
  if (!m_signerLookup || !m_signerLookup(request.recipientCertificate, signer))
    return std::nullopt;
  if (signer.getName() != request.recipientCertificate)
    return std::nullopt;
  if (!ndn::security::verifySignature(interest, signer.getPublicKey()))
    return std::nullopt;
  return finish(request, nowMs, interest.getName(), signer);
}

} // namespace ndn_service_framework
