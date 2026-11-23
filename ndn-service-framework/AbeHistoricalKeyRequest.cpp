#include "AbeHistoricalKeyRequest.hpp"

#include "AbeHistoricalAuthorization.hpp"
#include "AbeHistoricalKey.hpp"
#include "PolicyStatus.hpp"
#include "ProducerDataNaming.hpp"
#include "ProducerSignerPolicy.hpp"
#include "utils.hpp"

#include <nac-abe/ndn-crypto/data-enc-dec.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <ndn-cxx/security/interest-signer.hpp>
#include <ndn-cxx/util/sha256.hpp>

namespace ndn_service_framework {
namespace {

constexpr char BODY_SCHEMA[] = "ndnsf-abe-history-dkey-request-v4";
constexpr char BODY_SCHEMA_V5[] = "ndnsf-abe-history-dkey-request-v5";
constexpr char BODY_SCHEMA_V6[] = "ndnsf-abe-history-dkey-request-v6";
constexpr char BODY_SCHEMA_V3[] = "ndnsf-abe-history-dkey-request-v3";
constexpr char REPLY_SCHEMA[] = "ndnsf-abe-history-dkey-reply-v2";
constexpr char REPLY_SCHEMA_V3[] = "ndnsf-abe-history-dkey-reply-v3";
constexpr char PUBLICATION_SCHEMA[] = "ndnsf-abe-history-publication-v1";
constexpr char PUBLICATION_RECEIPT_SCHEMA[] = "ndnsf-abe-history-publication-receipt-v2";

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
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(text.data(), end, value);
  return result.ec == std::errc{} && result.ptr == end;
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

std::string
encodePublication(const HistoricalPublication& p)
{
  const auto uri = [] (const ndn::Name& name) { return name.toUri(ndn::name::UriFormat::CANONICAL); };
  return std::string(PUBLICATION_SCHEMA) + "\n" + uri(p.controller) + "\n" + uri(p.producer) +
         "\n" + uri(p.service) + "\n" + uri(p.objectName) + "\n" + uri(p.ckFullName) + "\n" +
         uri(p.publicParametersName) + "\n" + std::to_string(p.generation) + "\n" +
         std::to_string(p.epoch) + "\n" + std::to_string(p.policyGeneration) + "\n" +
         std::to_string(p.policyEpoch) + "\n" + std::to_string(p.segments) + "\n" + p.policy +
         "\n" + p.objectDigest + "\n" + p.publicParametersDigest;
}

ndn::Name
publicationName(const HistoricalPublication& p, const std::string& body)
{
  auto name = makeProducerDataPrefix(p.producer, "HISTORY");
  name.append("PUBLICATION").append(makeNamingDigestComponent(sha256Hex(body)));
  return name;
}

bool
isCanonicalDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string
encodeEvidence(const std::vector<uint8_t>& evidence)
{
  static constexpr char HEX[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(evidence.size() * 2);
  for (const auto byte : evidence) {
    hex.push_back(HEX[byte >> 4]);
    hex.push_back(HEX[byte & 0x0F]);
  }
  return hex;
}

bool
decodeEvidence(const std::string& hex, std::vector<uint8_t>& evidence)
{
  if (hex.empty() || hex.size() % 2 != 0)
    return false;
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    unsigned int byte = 0;
    const auto* end = hex.data() + i + 2;
    const auto result = std::from_chars(end - 2, end, byte, 16);
    if (result.ec != std::errc{} || result.ptr != end)
      return false;
    evidence.push_back(static_cast<uint8_t>(byte));
  }
  return true;
}

} // namespace

ndn::Data
makeHistoricalPublicationData(const HistoricalPublication& publication, ndn::KeyChain& keyChain,
                              const ndn::security::SigningInfo& signing)
{
  const auto body = encodePublication(publication);
  ndn::Data data(publicationName(publication, body));
  data.setContent(ndn::makeStringBlock(ndn::tlv::Content, body));
  if (!decodeHistoricalPublication(data))
    throw std::invalid_argument("invalid historical publication binding");
  keyChain.sign(data, signing);
  return data;
}

std::optional<HistoricalPublication>
decodeHistoricalPublication(const ndn::Data& data)
{
  try {
    const auto body = ndn::readString(data.getContent());
    std::istringstream input(body);
    std::array<std::string, 15> fields;
    for (auto& field : fields) {
      if (!std::getline(input, field))
        return std::nullopt;
    }
    HistoricalPublication p;
    p.controller = ndn::Name(fields[1]);
    p.producer = ndn::Name(fields[2]);
    p.service = ndn::Name(fields[3]);
    p.objectName = ndn::Name(fields[4]);
    p.ckFullName = ndn::Name(fields[5]);
    p.publicParametersName = ndn::Name(fields[6]);
    if (!parseU64(fields[7], p.generation) || !parseU64(fields[8], p.epoch) ||
        !parseU64(fields[9], p.policyGeneration) || !parseU64(fields[10], p.policyEpoch) ||
        !parseU64(fields[11], p.segments))
      return std::nullopt;
    p.policy = fields[12];
    p.objectDigest = fields[13];
    p.publicParametersDigest = fields[14];
    if (p.controller.empty() || p.producer.empty() || p.service.empty() || p.generation == 0 ||
        p.policyGeneration == 0 || p.segments == 0 || p.objectName.size() <= p.producer.size() ||
        !p.producer.isPrefixOf(p.objectName) || !p.controller.isPrefixOf(p.publicParametersName) ||
        p.publicParametersName.size() <= p.controller.size() ||
        p.ckFullName.empty() || !p.ckFullName[-1].isImplicitSha256Digest() ||
        !isSafePolicy(p.policy) || !isCanonicalDigest(p.objectDigest) ||
        !isCanonicalDigest(p.publicParametersDigest) || body != encodePublication(p) ||
        !historicalCkBindsAttribute(p.ckFullName.getPrefix(-1), p.producer, p.service, p.policy) ||
        data.getName() != publicationName(p, body))
      return std::nullopt;
    return p;
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}

ndn::Data
makeHistoricalPublicationData(const PolicyStatusData& status, const std::string& attribute,
                              const ndn::Data& ck,
                              const std::vector<std::shared_ptr<ndn::Data>>& segments,
                              const ndn::security::Certificate& producer,
                              const ndn::security::Certificate& controller,
                              ndn::KeyChain& keyChain, uint64_t nowMs)
{
  const auto producerName = producer.getIdentity();
  const auto service = status.getServiceName();
  const auto& material = status.getAbeMaterialVersion();
  const auto& policy = status.getControllerVersion();
  const auto validOriginal = [&] (const ndn::Data& packet) {
    return packet.wireEncode().size() <= ndn::MAX_NDN_PACKET_SIZE &&
      ProducerSignerPolicy().allowsValidatedSigner(packet, producerName) &&
      ndn::security::verifySignature(packet, producer.getPublicKey());
  };
  if (!status.validate(nowMs) || !producer.isValid() || !controller.isValid() ||
      status.getControllerCertificate() != controller.getName() || !material.isValid() ||
      material.compare(policy) > 0 ||
      segments.empty() || !segments.front() || segments.front()->getName().empty() ||
      !historicalCkBindsAttribute(ck.getName(), producerName, service, attribute) ||
      !historicalCkLocator(ck, producerName) || !validOriginal(ck))
    throw std::invalid_argument("invalid historical publication inputs");

  const auto root = segments.front()->getName().getPrefix(-1);
  const auto final = ndn::name::Component::fromSegment(segments.size() - 1);
  ndn::util::Sha256 originalWire;
  for (size_t i = 0; i < segments.size(); ++i) {
    const auto& packet = segments[i];
    if (!packet || packet->getName() != ndn::Name(root).appendSegment(i) ||
        !packet->getFinalBlock() || *packet->getFinalBlock() != final || !validOriginal(*packet))
      throw std::invalid_argument("historical publication packets are incomplete or unordered");
    const auto& wire = packet->wireEncode();
    originalWire << ndn::span<const uint8_t>(wire.data(), wire.size());
  }
  auto parametersDigest = status.getAbePublicParametersDigest();
  std::transform(parametersDigest.begin(), parametersDigest.end(), parametersDigest.begin(),
    [] (unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  const HistoricalPublication publication{controller.getIdentity(), producerName, service, root,
    ck.getFullName(), status.getAbePublicParametersName(), material.controllerGenerationTimestamp,
    material.controllerEpoch, policy.controllerGenerationTimestamp, policy.controllerEpoch,
    segments.size(), attribute, "sha256:" + ndn::toHex(*originalWire.computeDigest(), false),
    parametersDigest};
  return makeHistoricalPublicationData(publication, keyChain,
                                       ndn::security::signingByCertificate(producer));
}

namespace {

ndn::Data
publicationReceiptPayload(const ndn::Name& controller, const ndn::Name& publicationFullName,
                          const ControllerVersion& version)
{
  const auto content = std::string(PUBLICATION_RECEIPT_SCHEMA) + "\n" +
    publicationFullName.toUri(ndn::name::UriFormat::CANONICAL) + "\n" +
    std::to_string(version.controllerGenerationTimestamp) + "\n" +
    std::to_string(version.controllerEpoch);
  auto name = ndn::Name(controller).append("NDNSF").append("HISTORY")
    .append("PUBLICATION-RECEIPT").append(sha256Hex(content));
  ndn::Data receipt(name);
  receipt.setContent(ndn::makeStringBlock(ndn::tlv::Content, content));
  receipt.setFreshnessPeriod(ndn::time::milliseconds(0));
  return receipt;
}

ndn::Data
publicationReceiptPayload(const HistoricalPublication& binding, const ndn::Data& publication)
{
  return publicationReceiptPayload(binding.controller, publication.getFullName(),
                                   {binding.policyGeneration, binding.policyEpoch});
}

} // namespace

namespace {

constexpr char GRANT_RECEIPT_SCHEMA[] = "ndnsf-abe-history-grant-receipt-v1";

ndn::Name
grantReceiptName(const ndn::Name& controller, const std::string& content)
{
  return ndn::Name(controller).append("NDNSF").append("HISTORY")
    .append("GRANT-RECEIPT").append(sha256Hex(content));
}

std::string
grantReceiptContent(const ndn::Name& publicationFullName, const std::string& grantDigest,
                    const ndn::Name& recipient, const std::string& service,
                    const ControllerVersion& version)
{
  return std::string(GRANT_RECEIPT_SCHEMA) + "\n" +
    publicationFullName.toUri(ndn::name::UriFormat::CANONICAL) + "\n" + grantDigest + "\n" +
    recipient.toUri(ndn::name::UriFormat::CANONICAL) + "\n" + service + "\n" +
    std::to_string(version.controllerGenerationTimestamp) + "\n" +
    std::to_string(version.controllerEpoch);
}

} // namespace

namespace {

constexpr char GRANT_REQUEST_SCHEMA[] = "ndnsf-abe-history-grant-request-v1";

// Length-prefixed fields: the publication wire is binary TLV and may contain newlines, so a
// line-oriented body cannot carry it.
void
appendLengthPrefixed(std::string& out, const std::string& value)
{
  out += std::to_string(value.size());
  out += '\n';
  out += value;
}

bool
readLengthPrefixed(std::istream& input, std::string& value)
{
  std::string length;
  if (!std::getline(input, length))
    return false;
  size_t count = 0;
  try {
    count = static_cast<size_t>(std::stoull(length));
  }
  catch (const std::exception&) {
    return false;
  }
  if (count > ndn::MAX_NDN_PACKET_SIZE)
    return false;
  value.assign(count, '\0');
  if (count != 0)
    input.read(value.data(), static_cast<std::streamsize>(count));
  return static_cast<size_t>(input.gcount()) == count;
}

std::string
grantRequestContent(const HistoricalGrantReceiptRequest& request)
{
  std::string out = std::string(GRANT_REQUEST_SCHEMA) + "\n";
  appendLengthPrefixed(out, request.controllerPrefix.toUri(ndn::name::UriFormat::CANONICAL));
  appendLengthPrefixed(out, request.grantDigest);
  appendLengthPrefixed(out, request.recipient.toUri(ndn::name::UriFormat::CANONICAL));
  appendLengthPrefixed(out, request.service);
  appendLengthPrefixed(out, std::to_string(request.currentControllerVersion.controllerGenerationTimestamp));
  appendLengthPrefixed(out, std::to_string(request.currentControllerVersion.controllerEpoch));
  appendLengthPrefixed(out, request.publicationWire);
  return out;
}

} // namespace

std::string
encodeHistoricalGrantReceiptRequest(const HistoricalGrantReceiptRequest& request)
{
  if (request.controllerPrefix.empty() || request.publicationWire.empty() ||
      request.grantDigest.empty() || request.recipient.empty() || request.service.empty() ||
      !request.currentControllerVersion.isValid())
    throw std::invalid_argument("grant receipt request needs a publication, grant, recipient and version");
  return grantRequestContent(request);
}

bool
decodeHistoricalGrantReceiptRequest(const std::string& body,
                                    HistoricalGrantReceiptRequest& request)
{
  std::istringstream input(body);
  std::string schema;
  if (!std::getline(input, schema) || schema != GRANT_REQUEST_SCHEMA)
    return false;
  std::string controller, grantDigest, recipient, service, generation, epoch, publication;
  if (!readLengthPrefixed(input, controller) || !readLengthPrefixed(input, grantDigest) ||
      !readLengthPrefixed(input, recipient) || !readLengthPrefixed(input, service) ||
      !readLengthPrefixed(input, generation) || !readLengthPrefixed(input, epoch) ||
      !readLengthPrefixed(input, publication) || publication.empty())
    return false;
  if (!parseU64(generation, request.currentControllerVersion.controllerGenerationTimestamp) ||
      !parseU64(epoch, request.currentControllerVersion.controllerEpoch) ||
      !request.currentControllerVersion.isValid())
    return false;
  request.controllerPrefix = ndn::Name(controller);
  request.recipient = ndn::Name(recipient);
  request.grantDigest = grantDigest;
  request.service = service;
  request.publicationWire = publication;
  return !request.controllerPrefix.empty() && !request.recipient.empty() && !grantDigest.empty() &&
         !service.empty();
}

ndn::Data
makeHistoricalGrantReceipt(const ndn::Name& controller, const ndn::Name& publicationFullName,
                           const std::string& grantDigest, const ndn::Name& recipient,
                           const std::string& service, const ControllerVersion& version,
                           const ndn::security::Certificate& controllerCertificate,
                           ndn::KeyChain& keyChain)
{
  if (controller.empty() || publicationFullName.empty() || grantDigest.empty() ||
      recipient.empty() || !version.isValid() || !controllerCertificate.isValid() ||
      controllerCertificate.getIdentity() != controller)
    throw std::invalid_argument("grant receipt needs a publication, grant digest, recipient and version");
  const auto content = grantReceiptContent(publicationFullName, grantDigest, recipient, service,
                                          version);
  ndn::Data receipt(grantReceiptName(controller, content));
  receipt.setContent(ndn::makeStringBlock(ndn::tlv::Content, content));
  receipt.setFreshnessPeriod(ndn::time::milliseconds(0));
  keyChain.sign(receipt, ndn::security::signingByCertificate(controllerCertificate));
  if (receipt.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("grant receipt exceeds NDN packet size");
  return receipt;
}

std::optional<HistoricalGrantReceipt>
decodeHistoricalGrantReceipt(const ndn::Data& receipt, const ndn::Name& controller)
{
  try {
    std::istringstream input(ndn::readString(receipt.getContent()));
    std::string schema, fullName, grantDigest, recipient, service, generation, epoch;
    HistoricalGrantReceipt binding;
    if (!std::getline(input, schema) || schema != GRANT_RECEIPT_SCHEMA ||
        !std::getline(input, fullName) || !std::getline(input, grantDigest) ||
        !std::getline(input, recipient) || !std::getline(input, service) ||
        !std::getline(input, generation) || !std::getline(input, epoch) ||
        !parseU64(generation, binding.version.controllerGenerationTimestamp) ||
        !parseU64(epoch, binding.version.controllerEpoch) || !binding.version.isValid())
      return std::nullopt;
    binding.publicationFullName = ndn::Name(fullName);
    binding.recipient = ndn::Name(recipient);
    binding.grantDigest = grantDigest;
    binding.service = service;
    if (binding.publicationFullName.empty() || binding.recipient.empty() || grantDigest.empty() ||
        service.empty() || !binding.publicationFullName[-1].isImplicitSha256Digest())
      return std::nullopt;
    const auto content = grantReceiptContent(binding.publicationFullName, grantDigest,
                                             binding.recipient, service, binding.version);
    if (grantReceiptName(controller, content) != receipt.getName())
      return std::nullopt;
    return binding;
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<HistoricalPublicationReceipt>
decodeHistoricalPublicationReceipt(const ndn::Data& receipt, const ndn::Name& controller)
{
  try {
    std::istringstream input(ndn::readString(receipt.getContent()));
    std::string schema, fullName, generation, epoch;
    HistoricalPublicationReceipt binding;
    if (!std::getline(input, schema) || schema != PUBLICATION_RECEIPT_SCHEMA ||
        !std::getline(input, fullName) || !std::getline(input, generation) ||
        !std::getline(input, epoch) ||
        !parseU64(generation, binding.version.controllerGenerationTimestamp) ||
        !parseU64(epoch, binding.version.controllerEpoch) || !binding.version.isValid())
      return std::nullopt;
    binding.publicationFullName = ndn::Name(fullName);
    if (binding.publicationFullName.empty() ||
        !binding.publicationFullName[-1].isImplicitSha256Digest())
      return std::nullopt;
    const auto expected = publicationReceiptPayload(controller, binding.publicationFullName,
                                                    binding.version);
    if (expected.getName() != receipt.getName() || expected.getContent() != receipt.getContent())
      return std::nullopt;
    return binding;
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}

ndn::Data
makeHistoricalPublicationReceipt(const ndn::Data& publication,
                                 const ndn::security::Certificate& producerCertificate,
                                 const PolicyStatusData& currentStatus,
                                 const ndn::security::Certificate& controllerCertificate,
                                 ndn::KeyChain& keyChain, uint64_t nowMs)
{
  const auto binding = decodeHistoricalPublication(publication);
  const auto& advertisedDigest = currentStatus.getAbePublicParametersDigest();
  if (!binding || !currentStatus.validate(nowMs) || !producerCertificate.isValid() ||
      !controllerCertificate.isValid() || controllerCertificate.getIdentity() != binding->controller ||
      currentStatus.getControllerCertificate() != controllerCertificate.getName() ||
      producerCertificate.getIdentity() != binding->producer ||
      !ProducerSignerPolicy().allowsValidatedSigner(publication, binding->producer) ||
      !ndn::security::verifySignature(publication, producerCertificate.getPublicKey()) ||
      currentStatus.getServiceName() != binding->service ||
      currentStatus.getControllerVersion() != ControllerVersion{binding->policyGeneration,
                                                                binding->policyEpoch} ||
      currentStatus.getAbeMaterialVersion() != ControllerVersion{binding->generation, binding->epoch} ||
      currentStatus.getAbePublicParametersName() != binding->publicParametersName ||
      !std::equal(advertisedDigest.begin(), advertisedDigest.end(),
                  binding->publicParametersDigest.begin(), binding->publicParametersDigest.end(),
                  [] (unsigned char actual, char canonical) {
                    return std::tolower(actual) == canonical;
                  }))
    throw std::invalid_argument("publication does not match the current authority binding");

  auto receipt = publicationReceiptPayload(*binding, publication);
  keyChain.sign(receipt, ndn::security::signingByCertificate(controllerCertificate));
  if (receipt.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("historical publication receipt exceeds NDN packet size");
  return receipt;
}

bool
verifyHistoricalPublicationReceipt(const ndn::Data& receipt, const ndn::Data& publication,
                                   const ndn::security::Certificate& receiptSigner)
{
  try {
    const auto binding = decodeHistoricalPublication(publication);
    if (!binding || receiptSigner.getIdentity() != binding->controller)
      return false;
    const auto expected = publicationReceiptPayload(*binding, publication);
    return receipt.getName() == expected.getName() &&
      receipt.getContent() == expected.getContent() &&
      ndn::security::verifySignature(receipt, receiptSigner.getPublicKey());
  }
  catch (const std::exception&) {
    return false;
  }
}

ndn::Interest
makeHistoricalPublicationRequest(const ndn::Data& publication, ndn::KeyChain& keyChain,
                                 const ndn::security::Certificate& producerCertificate)
{
  const auto binding = decodeHistoricalPublication(publication);
  if (!binding || binding->producer != producerCertificate.getIdentity())
    throw std::invalid_argument("invalid historical publication requester");
  const auto& wire = publication.wireEncode();
  const auto parameters = ndn::makeBinaryBlock(ndn::tlv::ApplicationParameters,
                                               wire.begin(), wire.end());
  ndn::Interest interest(makeControlRpcName(binding->controller, "PUBLISH-HISTORY", parameters,
                                            makeControlQueryNonce()));
  interest.setApplicationParameters(parameters);
  interest.setMustBeFresh(true);
  ndn::security::InterestSigner signer(keyChain);
  signer.makeSignedInterest(interest, ndn::security::signingByCertificate(producerCertificate));
  if (interest.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("historical publication request exceeds NDN packet size");
  return interest;
}

std::optional<ndn::Name>
verifyHistoricalPublicationAcknowledgement(const ndn::Interest& interest, const ndn::Data& reply,
                                          const ndn::Data& publication,
                                          const ndn::security::Certificate& controllerCertificate)
{
  try {
    const auto binding = decodeHistoricalPublication(publication);
    if (!binding || !controllerCertificate.isValid() ||
        binding->controller != controllerCertificate.getIdentity() ||
        reply.getName() != interest.getName() ||
        !ndn::security::verifySignature(reply, controllerCertificate.getPublicKey()))
      return std::nullopt;
    const auto parameters = interest.getApplicationParameters();
    const auto& wire = publication.wireEncode();
    if (parameters.value_size() != wire.size() ||
        !std::equal(wire.begin(), wire.end(), parameters.value()))
      return std::nullopt;
    const ndn::Name fullName(reply.getContent().blockFromValue());
    const auto expected = publicationReceiptPayload(*binding, publication);
    if (fullName.empty() || !fullName[-1].isImplicitSha256Digest() ||
        fullName.getPrefix(-1) != expected.getName())
      return std::nullopt;
    return fullName;
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}

ndn::PendingInterestHandle
requestHistoricalPublicationReceipt(ndn::Face& face, ndn::KeyChain& keyChain,
                                    const ndn::Data& publication,
                                    const ndn::security::Certificate& producer,
                                    const ndn::security::Certificate& controller,
                                    uint32_t lifetimeMs,
                                    std::function<void(std::optional<ndn::Name>)> onComplete)
{
  if (lifetimeMs == 0 || lifetimeMs > 120000 || !onComplete)
    throw std::invalid_argument("invalid historical publication receipt request lifetime");
  auto interest = makeHistoricalPublicationRequest(publication, keyChain, producer);
  const auto lifetime = ndn::time::milliseconds(lifetimeMs);
  interest.setInterestLifetime(lifetime);
  if (interest.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("historical publication receipt request exceeds NDN packet size");
  const auto deadline = ndn::time::steady_clock::now() + lifetime;
  return face.expressInterest(interest,
    [publication, controller, deadline, onComplete] (const ndn::Interest& sent,
                                                       const ndn::Data& reply) {
      auto receipt = verifyHistoricalPublicationAcknowledgement(sent, reply, publication, controller);
      if (ndn::time::steady_clock::now() >= deadline)
        receipt.reset();
      onComplete(std::move(receipt));
    },
    [onComplete] (const ndn::Interest&, const ndn::lp::Nack&) { onComplete(std::nullopt); },
    [onComplete] (const ndn::Interest&) { onComplete(std::nullopt); });
}

ndn::Interest
makeHistoricalGrantReceiptRequestInterest(const HistoricalGrantReceiptRequest& request,
                                          const ndn::security::Certificate& producer,
                                          ndn::KeyChain& keyChain)
{
  const auto body = encodeHistoricalGrantReceiptRequest(request);
  // The control-RPC framework reads its parameters from an ApplicationParameters element; handing it
  // bare bytes makes parseControlRpcName reject the interest before it reaches the Controller.
  const ndn::Block parameters(ndn::makeStringBlock(ndn::tlv::ApplicationParameters, body));
  ndn::Interest interest(makeControlRpcName(request.controllerPrefix, "PUBLISH-HISTORY", parameters,
                                            makeControlQueryNonce()));
  interest.setApplicationParameters(parameters);
  ndn::security::InterestSigner signer(keyChain);
  signer.makeSignedInterest(interest, ndn::security::signingByCertificate(producer));
  return interest;
}

ndn::PendingInterestHandle
requestHistoricalGrantReceipt(ndn::Face& face, ndn::KeyChain& keyChain,
                              const HistoricalGrantReceiptRequest& request,
                              const ndn::security::Certificate& producer,
                              const ndn::security::Certificate& controller,
                              uint32_t lifetimeMs,
                              std::function<void(std::optional<ndn::Name>)> onComplete)
{
  if (lifetimeMs == 0 || lifetimeMs > 120000 || !onComplete)
    throw std::invalid_argument("invalid historical grant receipt request lifetime");
  if (producer.getIdentity().empty() || controller.getIdentity().empty())
    throw std::invalid_argument("grant receipt request needs both identities");
  auto interest = makeHistoricalGrantReceiptRequestInterest(request, producer, keyChain);
  const auto lifetime = ndn::time::milliseconds(lifetimeMs);
  interest.setInterestLifetime(lifetime);
  if (interest.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("historical grant receipt request exceeds NDN packet size");
  const auto deadline = ndn::time::steady_clock::now() + lifetime;
  const auto requested = interest.getName();
  return face.expressInterest(interest,
    [controller, deadline, requested, onComplete] (const ndn::Interest& sent,
                                                   const ndn::Data& reply) {
      // The reply echoes the request name and carries the receipt's name as canonical text.
      std::optional<ndn::Name> receipt;
      if (sent.getName() == requested && reply.getName() == requested &&
          ndn::security::verifySignature(reply, controller.getPublicKey())) {
        try {
          receipt = ndn::Name(ndn::readString(reply.getContent()));
        }
        catch (const std::exception&) {
          receipt.reset();
        }
      }
      if (ndn::time::steady_clock::now() >= deadline)
        receipt.reset();
      onComplete(std::move(receipt));
    },
    [onComplete] (const ndn::Interest&, const ndn::lp::Nack&) { onComplete(std::nullopt); },
    [onComplete] (const ndn::Interest&) { onComplete(std::nullopt); });
}

bool
historicalPublicationMatches(const HistoricalPublication& p, const HistoricalKeyRequest& request,
                             const ndn::Data& ck)
{
  return p.controller == request.controllerPrefix && p.producer == request.producerName &&
    p.service == request.serviceName && p.objectName == request.objectName &&
    p.generation == request.generation && p.epoch == request.epoch && p.policy == request.policy &&
    ck.getName() == request.ckName && ck.getFullName() == p.ckFullName &&
    historicalCkLocator(ck, p.producer).has_value();
}

bool
HistoricalKeyRequest::operator==(const HistoricalKeyRequest& other) const
{
  return controllerPrefix == other.controllerPrefix && generation == other.generation &&
    epoch == other.epoch && objectName == other.objectName && producerName == other.producerName &&
    serviceName == other.serviceName && ckName == other.ckName && policy == other.policy &&
    ckEvidence == other.ckEvidence && publicationEvidence == other.publicationEvidence &&
    recipientCertificate == other.recipientCertificate && mintedAtMs == other.mintedAtMs &&
    expiresAtMs == other.expiresAtMs && currentControllerVersion == other.currentControllerVersion;
}

std::string
encodeHistoricalKeyRequest(const HistoricalKeyRequest& request)
{
  if (!isSafePolicy(request.policy))
    throw std::invalid_argument("historical key request policy is not a plain attribute expression");
  if (!request.publicationEvidence.empty() && request.ckEvidence.empty())
    throw std::invalid_argument("historical publication requires the original CK evidence");
  const bool versionBound = request.currentControllerVersion.isValid();
  std::string body(versionBound ? BODY_SCHEMA_V6 : request.ckEvidence.empty() ? BODY_SCHEMA_V3 :
                   request.publicationEvidence.empty() ? BODY_SCHEMA : BODY_SCHEMA_V5);
  body += "\ncontroller " + request.controllerPrefix.toUri(ndn::name::UriFormat::CANONICAL);
  body += "\ngeneration " + std::to_string(request.generation);
  body += "\nepoch " + std::to_string(request.epoch);
  body += "\nobject " + request.objectName.toUri(ndn::name::UriFormat::CANONICAL);
  body += "\nproducer " + request.producerName.toUri(ndn::name::UriFormat::CANONICAL);
  body += "\nservice " + request.serviceName.toUri(ndn::name::UriFormat::CANONICAL);
  body += "\nck " + request.ckName.toUri(ndn::name::UriFormat::CANONICAL);
  body += "\nrecipient " + request.recipientCertificate.toUri(ndn::name::UriFormat::CANONICAL);
  body += "\npolicy " + request.policy;
  body += "\nminted " + std::to_string(request.mintedAtMs);
  body += "\nexpires " + std::to_string(request.expiresAtMs);
  if (versionBound || !request.ckEvidence.empty())
    body += "\nck-evidence " + encodeEvidence(request.ckEvidence);
  if (versionBound || !request.publicationEvidence.empty())
    body += "\npublication-evidence " + encodeEvidence(request.publicationEvidence);
  if (versionBound) {
    body += "\ncurrent-generation " +
      std::to_string(request.currentControllerVersion.controllerGenerationTimestamp);
    body += "\ncurrent-epoch " + std::to_string(request.currentControllerVersion.controllerEpoch);
  }
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
    while (!lines.empty() && lines.back().empty())
      lines.pop_back();
    const bool v4 = lines.size() == 13 && lines[0] == BODY_SCHEMA;
    const bool v5 = lines.size() == 14 && lines[0] == BODY_SCHEMA_V5;
    const bool v6 = lines.size() == 16 && lines[0] == BODY_SCHEMA_V6;
    if (!v6 && !v5 && !v4 && (lines.size() != 12 || lines[0] != BODY_SCHEMA_V3))
      return false;
    const auto field = [] (const std::string& line, const char* prefix) -> std::string {
      const std::string head(prefix);
      if (line.compare(0, head.size(), head) != 0)
        throw std::runtime_error("missing field");
      return line.substr(head.size());
    };
    HistoricalKeyRequest parsed;
    if (v4 || v5 || v6) {
      const auto ckEvidence = field(lines[12], "ck-evidence ");
      if ((!v6 || !ckEvidence.empty()) && !decodeEvidence(ckEvidence, parsed.ckEvidence))
        return false;
    }
    if (v5 || v6) {
      const auto publicationEvidence = field(lines[13], "publication-evidence ");
      if ((!v6 || !publicationEvidence.empty()) &&
          !decodeEvidence(publicationEvidence, parsed.publicationEvidence))
        return false;
    }
    if (v6 &&
        (!parseU64(field(lines[14], "current-generation "),
                   parsed.currentControllerVersion.controllerGenerationTimestamp) ||
         !parseU64(field(lines[15], "current-epoch "),
                   parsed.currentControllerVersion.controllerEpoch) ||
         !parsed.currentControllerVersion.isValid()))
      return false;
    parsed.controllerPrefix = ndn::Name(field(lines[1], "controller "));
    if (!parseU64(field(lines[2], "generation "), parsed.generation) ||
        !parseU64(field(lines[3], "epoch "), parsed.epoch) ||
        !parseU64(field(lines[10], "minted "), parsed.mintedAtMs) ||
        !parseU64(field(lines[11], "expires "), parsed.expiresAtMs))
      return false;
    const auto objectText = field(lines[4], "object ");
    if (objectText.empty() || objectText.front() != '/')
      return false;
    parsed.objectName = ndn::Name(objectText);
    if (parsed.objectName.toUri(ndn::name::UriFormat::CANONICAL) != objectText)
      return false;
    const auto producerText = field(lines[5], "producer ");
    const auto serviceText = field(lines[6], "service ");
    const auto ckText = field(lines[7], "ck ");
    if (producerText.empty() || producerText.front() != '/' || serviceText.empty() ||
        serviceText.front() != '/' || ckText.empty() || ckText.front() != '/')
      return false;
    parsed.producerName = ndn::Name(producerText);
    parsed.serviceName = ndn::Name(serviceText);
    parsed.ckName = ndn::Name(ckText);
    if (parsed.producerName.toUri(ndn::name::UriFormat::CANONICAL) != producerText ||
        parsed.serviceName.toUri(ndn::name::UriFormat::CANONICAL) != serviceText ||
        parsed.ckName.toUri(ndn::name::UriFormat::CANONICAL) != ckText)
      return false;
    const auto recipientText = field(lines[8], "recipient ");
    // A certificate name is absolute and round-trips to the text it was written as; anything else
    // would let the body and the name disagree about who is asking.
    if (recipientText.empty() || recipientText.front() != '/' ||
        std::any_of(recipientText.begin(), recipientText.end(),
                    [] (unsigned char ch) { return ch < 0x20 || ch == 0x7f; }))
      return false;
    parsed.recipientCertificate = ndn::Name(recipientText);
    if (parsed.recipientCertificate.toUri(ndn::name::UriFormat::CANONICAL) != recipientText)
      return false;
    parsed.policy = field(lines[9], "policy ");
    if (parsed.controllerPrefix.empty() || parsed.generation == 0 ||
        parsed.recipientCertificate.empty() || parsed.objectName.empty() ||
        parsed.producerName.empty() || parsed.serviceName.empty() || parsed.ckName.empty() ||
        !isSafePolicy(parsed.policy) || (v6 && encodeHistoricalKeyRequest(parsed) != body))
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

namespace {

bool
matchesHistoricalRecipient(const ndn::Name& name, const ndn::security::Certificate& certificate)
{
  if (name.empty() || certificate.getName().empty())
    return false;
  return name[-1].isImplicitSha256Digest() ? name == certificate.getFullName() :
                                           name == certificate.getName();
}

} // namespace

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
  if (!matchesHistoricalRecipient(request.recipientCertificate, recipient))
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

  const bool versionBound = request.currentControllerVersion.isValid();
  std::string envelope(versionBound ? REPLY_SCHEMA_V3 : REPLY_SCHEMA);
  envelope += "\nrequest " + historicalKeyRequestDigest(request);
  envelope += "\nobject " + request.objectName.toUri(ndn::name::UriFormat::CANONICAL);
  envelope += "\nrecipient " + request.recipientCertificate.toUri(ndn::name::UriFormat::CANONICAL);
  envelope += "\ngeneration " + std::to_string(request.generation);
  envelope += "\nepoch " + std::to_string(request.epoch);
  envelope += "\npolicy " + request.policy;
  envelope += "\nexpires " + std::to_string(expires);
  if (versionBound) {
    envelope += "\ncurrent-generation " +
      std::to_string(request.currentControllerVersion.controllerGenerationTimestamp);
    envelope += "\ncurrent-epoch " + std::to_string(request.currentControllerVersion.controllerEpoch);
  }
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
    const bool versionBound = request.currentControllerVersion.isValid();
    const auto sealedField = versionBound ? 10u : 8u;
    std::vector<std::string> lines;
    std::size_t begin = 0;
    std::size_t envelopeEnd = std::string::npos;
    while (lines.size() <= sealedField) {
      const auto end = content.find('\n', begin);
      if (end == std::string::npos)
        return false;
      lines.push_back(content.substr(begin, end - begin));
      begin = end + 1;
      envelopeEnd = end;
    }
    if (lines[0] != (versionBound ? REPLY_SCHEMA_V3 : REPLY_SCHEMA))
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
        !parseU64(field(lines[sealedField], "sealed "), sealedBytes))
      return false;
    ControllerVersion responseVersion;
    if (versionBound &&
        (!parseU64(field(lines[8], "current-generation "),
                   responseVersion.controllerGenerationTimestamp) ||
         !parseU64(field(lines[9], "current-epoch "), responseVersion.controllerEpoch) ||
         responseVersion != request.currentControllerVersion))
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
    accepted.currentControllerVersion = responseVersion;
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
                                     const ndn::security::Certificate& signer,
                                     const ndn::security::Certificate* resolvedRecipient,
                                     Failure* failure)
{
  if (failure)
    *failure = Failure::REQUEST_REJECTED;
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
  const auto& recipient = resolvedRecipient ? *resolvedRecipient : signer;
  if (!matchesHistoricalRecipient(request.recipientCertificate, recipient) ||
      signer.getName().empty() || signer.getIdentity() != recipient.getIdentity() ||
      (!request.recipientCertificate[-1].isImplicitSha256Digest() &&
       signer.getFullName() != recipient.getFullName()))
    return std::nullopt;
  return finish(request, nowMs, interest.getName(), signer, recipient, failure);
}

std::optional<ndn::Data>
HistoricalKeyBroker::finish(const HistoricalKeyRequest& request, uint64_t nowMs,
                            const ndn::Name& replyName,
                            const ndn::security::Certificate& signer,
                            const ndn::security::Certificate& recipient, Failure* failure)
{
  for (auto it = m_seen.begin(); it != m_seen.end();) {
    if (it->second > nowMs) {
      ++it;
      continue;
    }
    it = m_seen.erase(it);
  }
  const auto digest = historicalKeyRequestDigest(request);
  if (m_seen.count(digest) || m_seen.size() >= 4096)
    return std::nullopt;
  std::optional<HistoricalPublication> publication;
  if (!request.publicationEvidence.empty()) {
    try {
      const ndn::Data evidence{ndn::Block(ndn::span<const uint8_t>(request.publicationEvidence))};
      publication = decodeHistoricalPublication(evidence);
      const ndn::Data ck{ndn::Block(ndn::span<const uint8_t>(request.ckEvidence))};
      if (!publication || !historicalPublicationMatches(*publication, request, ck))
        return std::nullopt;
    }
    catch (const std::exception&) {
      return std::nullopt;
    }
  }
  // Without a decision the broker must not mint: a Controller that cannot check the object's
  // authorization has no basis for handing out a key to a retired generation.
  // The certificate that signed the request, not the one the body claims, is what the decision covers.
  ndn::util::Sha256 signerDigest;
  const auto& signerWire = signer.wireEncode();
  signerDigest << ndn::span<const uint8_t>(signerWire.data(), signerWire.size());
  if (!m_authorizationCheck ||
      !m_authorizationCheck(signer.getIdentity(),
                            publication ? publication->policyGeneration : request.generation,
                            publication ? publication->policyEpoch : request.epoch, request.policy,
                            "sha256:" + signerDigest.toString())) {
    if (failure)
      *failure = Failure::NOT_AUTHORIZED;
    return std::nullopt;
  }
  // The key may only be one that belongs to the object's own scope, so a requester cannot borrow a key
  // minted for a different service, producer or attribute and point it at something else.
  if (!historicalCkBindsAttribute(request.ckName, request.producerName, request.serviceName,
                                  request.policy))
    return std::nullopt;
  // Only an authorized request may learn whether its archived material is usable.
  if (failure)
    *failure = Failure::KEY_UNAVAILABLE;
  Material material;
  if (!m_materialLookup || !m_materialLookup(request.generation, request.epoch, material))
    return std::nullopt;
  if (publication && (material.publicParametersName != publication->publicParametersName ||
                      "sha256:" + sha256Hex(material.publicParameters) !=
                        publication->publicParametersDigest))
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
  if (failure)
    *failure = Failure::NONE;
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
  // A request that claims a very long validity would keep its digest in the replay set for that
  // long, so the lifetime itself is bounded instead of trusted.
  constexpr uint64_t MAX_TTL_MS = 600000;
  if (request.expiresAtMs <= nowMs || request.mintedAtMs > nowMs + MAX_SKEW_MS ||
      request.mintedAtMs + MAX_AGE_MS < nowMs ||
      request.expiresAtMs - request.mintedAtMs > MAX_TTL_MS)
    return std::nullopt;

  ndn::security::Certificate signer;
  const auto& requested = request.recipientCertificate;
  const auto certName = requested[-1].isImplicitSha256Digest() ? requested.getPrefix(-1) : requested;
  if (!m_signerLookup || !m_signerLookup(certName, signer))
    return std::nullopt;
  if (!matchesHistoricalRecipient(requested, signer))
    return std::nullopt;
  if (!ndn::security::verifySignature(interest, signer.getPublicKey()))
    return std::nullopt;
  return finish(request, nowMs, interest.getName(), signer, signer);
}

} // namespace ndn_service_framework
