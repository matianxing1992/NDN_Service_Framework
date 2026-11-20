#include "ProtectedBlobManifest.hpp"
#include "HybridMessageCrypto.hpp"
#include "ProducerDataNaming.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <array>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ndn_service_framework {
namespace {
constexpr std::uint32_t ManifestType = 242;
constexpr std::uint32_t LegacyManifestType = 240;
constexpr std::array<std::uint32_t, 14> Fields{
  129, 131, 133, 135, 137, 139, 141, 143, 145, 147, 149, 151, 153, 155};
ndn::span<const std::uint8_t> bytes(const std::vector<std::uint8_t>& value)
{
  return {value.data(), value.size()};
}
std::string text(const ndn::Block& value)
{
  return {reinterpret_cast<const char*>(value.value()), value.value_size()};
}

std::string hex(const std::vector<std::uint8_t>& value)
{
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (const auto byte : value) {
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result;
}
ndn::Name readName(const ndn::Block& value)
{
  ndn::Block block(ndn::span<const std::uint8_t>(value.value(), value.value_size()));
  if (block.size() != value.value_size())
    throw std::invalid_argument("BLOB_MANIFEST_NAME_TRAILING_BYTES");
  return ndn::Name(block);
}

bool digestComponent(const ndn::name::Component& value)
{
  return value.type() == 8 && value.value_size() == 32;
}

bool atomComponent(const ndn::name::Component& value)
{
  return value.type() == 8 && value.value_size() > 0 &&
    std::all_of(value.value(), value.value() + value.value_size(), [](unsigned char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '=' || c == '.';
    });
}

bool semanticPrefix(const ndn::Name& suffix)
{
  const auto eq = [&](size_t i, const char* label) { return suffix[i] == ndn::name::Component(label); };
  if (suffix.size() == 5 &&
      ((eq(0, "NDNSF") && eq(1, "blob")) || (eq(0, "NDNSF-DI") && eq(1, "material"))))
    return isNamingLabelComponent(suffix[2]) && digestComponent(suffix[3]) && digestComponent(suffix[4]);
  if (suffix.size() == 6 && eq(0, "NDNSF-DI") && eq(1, "input"))
    return atomComponent(suffix[2]) && isNamingLabelComponent(suffix[3]) &&
      digestComponent(suffix[4]) && digestComponent(suffix[5]);
  if (suffix.size() == 7 && eq(0, "NDNSF-DI") && eq(1, "assignment"))
    return eq(2, "model") && isNamingLabelComponent(suffix[3]) && eq(4, "ROLE") &&
      isNamingLabelComponent(suffix[5]) && digestComponent(suffix[6]);
  if ((suffix.size() != 8 && suffix.size() != 12) || !eq(0, "NDNSF-DI") || !eq(1, "model") ||
      !isNamingLabelComponent(suffix[2]) || !digestComponent(suffix[3])) return false;
  if (suffix.size() == 8)
    return (eq(4, "file") || eq(4, "weights") || eq(4, "initializer") || eq(4, "tokenizer")) &&
      isNamingLabelComponent(suffix[5]) && digestComponent(suffix[6]) && digestComponent(suffix[7]);
  return eq(4, "artifact") && atomComponent(suffix[5]) && atomComponent(suffix[6]) &&
    digestComponent(suffix[7]) && eq(8, "file") && isNamingLabelComponent(suffix[9]) &&
    digestComponent(suffix[10]) && digestComponent(suffix[11]);
}

std::optional<ndn::Name> semanticProducer(const ndn::Name& name, bool manifest)
{
  const size_t tail = manifest ? 4 : 3;
  if (name.size() < tail || name[-static_cast<ssize_t>(tail)] != ndn::name::Component("PROTECTED") ||
      !digestComponent(name[-static_cast<ssize_t>(tail) + 1]) ||
      name[-static_cast<ssize_t>(tail) + 2] != ndn::name::Component(manifest ? "MANIFEST" : "DATA") ||
      (manifest && !digestComponent(name[-1]))) return std::nullopt;
  std::optional<ndn::Name> producer;
  for (const size_t length : {size_t(5), size_t(6), size_t(7), size_t(8), size_t(12)}) {
    if (name.size() <= length + tail) continue;
    const auto boundary = name.size() - length - tail;
    if (!semanticPrefix(name.getSubName(boundary, length))) continue;
    const auto candidate = name.getPrefix(boundary);
    if (producer && *producer != candidate) return std::nullopt;
    producer = candidate;
  }
  return producer;
}
/** Producer of a semantic object named `<stem>/rep/<RepresentationId>/{DATA,MANIFEST}`.
 *
 * The stem begins with the Producer and then the namespace component (`NDNSF-DI` or `NDNSF`); everything before
 * that component is the Producer. */
std::optional<ndn::Name> representationProducer(const ndn::Name& name, const bool manifest)
{
  const size_t tail = manifest ? 3 : 3;
  if (name.size() < tail + 2)
    return std::nullopt;
  if (name[-static_cast<ssize_t>(tail)] != ndn::name::Component("rep") ||
      !digestComponent(name[-static_cast<ssize_t>(tail) + 1]) ||
      name[-static_cast<ssize_t>(tail) + 2] != ndn::name::Component(manifest ? "MANIFEST" : "DATA"))
    return std::nullopt;
  const auto stemEnd = name.size() - tail;
  for (std::size_t index = stemEnd; index-- > 0;) {
    if (name[index] == ndn::name::Component("NDNSF-DI") || name[index] == ndn::name::Component("NDNSF"))
      return index == 0 ? std::nullopt : std::optional<ndn::Name>(name.getPrefix(index));
  }
  return std::nullopt;
}

} // namespace

ndn::Name
ProtectedBlobManifest::semanticDataPrefix() const
{
  if (!contentName || !contentName->hasSemanticCoordinates() || plaintextDigest.size() != 32 ||
      publicationNonce.size() != HybridMessageCrypto::NONCE_SIZE ||
      !ndn::security::Certificate::isValidName(signerCertificate))
    throw std::invalid_argument("BLOB_REPRESENTATION_DESCRIPTOR_INVALID");

  const auto role = std::to_string(static_cast<std::uint8_t>(contentName->semanticRole));
  const auto semantic = "{\"contentDigest\":\"sha256:" + hex(plaintextDigest) +
    "\",\"objectIdentity\":\"" + contentName->objectIdentity() + "\"}";
  const auto signer = "{\"certificate\":\"" +
    signerCertificate.toUri(ndn::name::UriFormat::CANONICAL) + "\"}";
  const auto& producerWire = producer.wireEncode();
  const auto representation = computeNamingDigest("ndnsf-naming/representation", {
    {1, 1, {producerWire.begin(), producerWire.end()}},
    {2, 2, {role.begin(), role.end()}},
    {3, 5, {semantic.begin(), semantic.end()}},
    {4, 4, publicationNonce},
    {5, 5, {representationSpec.begin(), representationSpec.end()}},
    {6, 5, {protectionSpec.begin(), protectionSpec.end()}},
    {7, 5, {signer.begin(), signer.end()}}});
  return contentName->representationPrefix(producer, representation).append("DATA");
}

void
ProtectedBlobManifest::validateSigner(const ndn::Data& data) const
{
  if (!contentName || !contentName->hasSemanticCoordinates())
    return;
  const auto& signature = data.getSignatureInfo();
  if (!signature.hasKeyLocator() || signature.getKeyLocator().getType() != ndn::tlv::Name)
    throw std::invalid_argument("BLOB_REPRESENTATION_SIGNER_MISSING");
  const auto& locator = signature.getKeyLocator().getName();
  if (locator != signerCertificate && locator != ndn::security::extractKeyNameFromCertName(signerCertificate))
    throw std::invalid_argument("BLOB_REPRESENTATION_SIGNER_MISMATCH");
}

void ProtectedBlobManifest::validate(DataNameReadMode mode) const
{
  if (mode != DataNameReadMode::StrictCanonical && mode != DataNameReadMode::AllowLegacyRead)
    throw std::invalid_argument("INVALID_MANIFEST_READ_MODE");
  if (!contentName && mode != DataNameReadMode::AllowLegacyRead)
    throw std::invalid_argument("MISSING_CONTENT_SEMANTIC_IDENTITY");
  const bool legacySemanticReceipt = mode == DataNameReadMode::AllowLegacyRead &&
    manifestSuffix.size() == 3 &&
    manifestSuffix[0] == ndn::name::Component("NDNSF-DI") &&
    manifestSuffix[1] == ndn::name::Component("receipt");
  validateSuffix(manifestSuffix,
    contentName && !legacySemanticReceipt ? DataNameReadMode::StrictCanonical : mode);
  if (contentName && !manifestSuffix.empty()) {
    if (manifestSuffix[1] == ndn::name::Component("model") &&
        (contentName->modelIdentity.empty() ||
         !matchesNamingLabelComponent(manifestSuffix[2], contentName->modelName) ||
         manifestSuffix[3] != makeNamingDigestComponent(contentName->modelIdentity)))
      throw std::invalid_argument("MODEL_MANIFEST_SEMANTIC_IDENTITY_MISMATCH");
    if (manifestSuffix[1] == ndn::name::Component("material") &&
        (contentName->kind != ContentDataKind::Material ||
         !matchesNamingLabelComponent(manifestSuffix[2], contentName->label)))
      throw std::invalid_argument("MATERIAL_MANIFEST_SEMANTIC_IDENTITY_MISMATCH");
    if (manifestSuffix[1] == ndn::name::Component("receipt") &&
        (contentName->kind != ContentDataKind::Material ||
         (!legacySemanticReceipt &&
          !matchesNamingLabelComponent(manifestSuffix[3], contentName->label))))
      throw std::invalid_argument("RECEIPT_MANIFEST_SEMANTIC_IDENTITY_MISMATCH");
  }
  if (plaintextDigest.size() != 32 || envelopeBytes == 0 || segmentBytes == 0 ||
      segmentBytes > ndn::MAX_NDN_PACKET_SIZE ||
      segmentCount != 1 + (envelopeBytes - 1) / segmentBytes ||
      representationSpec.empty() || protectionSpec.empty() ||
      authorizationScope.empty() || protectionEpoch.empty() || keyReferenceId.empty() || keyIdentifier.empty())
    throw std::invalid_argument("BLOB_MANIFEST_FIELDS_INVALID");
  const auto digest = computeNamingDigest("ndnsf-naming/protection",
    {{1, 5, std::vector<std::uint8_t>(representationSpec.begin(), representationSpec.end())},
     {2, 5, std::vector<std::uint8_t>(protectionSpec.begin(), protectionSpec.end())}});
  const auto plaintextHex = hex(plaintextDigest);
  auto expected = contentName ? contentName->contentPrefix(producer, plaintextHex) :
    makeProducerDataPrefix(producer, "BLOB").append(ndn::name::Component(bytes(plaintextDigest)));
  if (contentName && contentName->hasSemanticCoordinates()) {
    if (!manifestSuffix.empty() || dataPrefix != semanticDataPrefix())
      throw std::invalid_argument("BLOB_MANIFEST_BINDING_MISMATCH");
    checkNamingNameSize(ndn::Name(dataPrefix).appendSegment(
      std::numeric_limits<std::uint64_t>::max()));
    return;
  }
  if (!publicationNonce.empty() || !signerCertificate.empty())
    throw std::invalid_argument("BLOB_LEGACY_REPRESENTATION_DESCRIPTOR_UNEXPECTED");
  expected.append("PROTECTED")
    .append(makeNamingDigestComponent(digest)).append("DATA");
  if (dataPrefix != expected)
    throw std::invalid_argument("BLOB_MANIFEST_BINDING_MISMATCH");
  checkNamingNameSize(ndn::Name(dataPrefix).appendSegment(
    std::numeric_limits<std::uint64_t>::max()));
}

std::vector<std::uint8_t> ProtectedBlobManifest::encode(DataNameReadMode mode) const
{
  validate(mode);
  ndn::Block body(contentName ? ManifestType : LegacyManifestType);
  body.push_back(ndn::Block(Fields[0], producer.wireEncode()));
  body.push_back(ndn::Block(Fields[1], dataPrefix.wireEncode()));
  body.push_back(ndn::makeBinaryBlock(Fields[2], bytes(plaintextDigest)));
  body.push_back(ndn::makeNonNegativeIntegerBlock(Fields[3], plaintextBytes));
  body.push_back(ndn::makeNonNegativeIntegerBlock(Fields[4], envelopeBytes));
  body.push_back(ndn::makeNonNegativeIntegerBlock(Fields[5], segmentBytes));
  body.push_back(ndn::makeNonNegativeIntegerBlock(Fields[6], segmentCount));
  body.push_back(ndn::makeStringBlock(Fields[7], representationSpec));
  body.push_back(ndn::makeStringBlock(Fields[8], protectionSpec));
  body.push_back(ndn::makeStringBlock(Fields[9], authorizationScope));
  body.push_back(ndn::makeStringBlock(Fields[10], protectionEpoch));
  body.push_back(ndn::makeStringBlock(Fields[11], keyReferenceId));
  body.push_back(ndn::makeStringBlock(Fields[12], keyIdentifier));
  body.push_back(ndn::Block(Fields[13], manifestSuffix.wireEncode()));
  if (contentName) {
    const auto metadata = contentName->encode();
    body.push_back(ndn::makeBinaryBlock(157, bytes(metadata)));
    if (contentName->hasSemanticCoordinates()) {
      body.push_back(ndn::makeBinaryBlock(159, bytes(publicationNonce)));
      body.push_back(ndn::Block(161, signerCertificate.wireEncode()));
    }
  }
  body.encode();
  if (body.size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::length_error("BLOB_MANIFEST_PAYLOAD_TOO_LARGE");
  return {body.begin(), body.end()};
}

ndn::Name ProtectedBlobManifest::getName(DataNameReadMode mode) const
{
  const auto payload = encode(mode);
  ndn::util::Sha256 digest;
  digest.update(bytes(payload));
  const auto hash = digest.computeDigest();
  auto name = manifestSuffix.empty() ? ndn::Name(dataPrefix.getPrefix(-1)).append("MANIFEST") :
    (contentName ? ndn::Name(producer).append(manifestSuffix) :
                   ndn::Name(producer).append("NDNSF").append(manifestSuffix));
  // C2: a semantical object's manifest is `<ObjectPrefix>/MANIFEST`; the payload digest is bound inside the
  // manifest, not appended to its name. Legacy records keep the trailing payload hash.
  if (!(contentName && contentName->hasSemanticCoordinates() && manifestSuffix.empty()))
    name.append(ndn::name::Component(bytes(*hash)));
  checkNamingNameSize(name);
  return name;
}

ProtectedBlobManifest ProtectedBlobManifest::decode(
  const std::vector<std::uint8_t>& payload, const ndn::Name& expectedProducer,
  const ndn::Name& expectedManifestName, DataNameReadMode mode)
{
  if (payload.empty() || payload.size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("BLOB_MANIFEST_PAYLOAD_SIZE_INVALID");
  ndn::Block body(bytes(payload));
  const bool legacy = body.type() == LegacyManifestType;
  if ((body.type() != ManifestType && !legacy) || body.size() != payload.size() ||
      (legacy && mode != DataNameReadMode::AllowLegacyRead))
    throw std::invalid_argument("BLOB_MANIFEST_ENVELOPE_INVALID");
  body.parse();
  const auto& fields = body.elements();
  if (legacy ? fields.size() != Fields.size()
             : (fields.size() != Fields.size() + 1 && fields.size() != Fields.size() + 3))
    throw std::invalid_argument("BLOB_MANIFEST_FIELD_COUNT_INVALID");
  for (std::size_t i = 0; i < Fields.size(); ++i)
    if (fields[i].type() != Fields[i])
      throw std::invalid_argument("BLOB_MANIFEST_FIELD_ORDER_INVALID");
  ProtectedBlobManifest result;
  result.producer = readName(fields[0]);
  if (result.producer != expectedProducer)
    throw std::invalid_argument("BLOB_MANIFEST_PRODUCER_MISMATCH");
  result.dataPrefix = readName(fields[1]);
  result.plaintextDigest.assign(fields[2].value(), fields[2].value() + fields[2].value_size());
  result.plaintextBytes = ndn::readNonNegativeInteger(fields[3]);
  result.envelopeBytes = ndn::readNonNegativeInteger(fields[4]);
  result.segmentBytes = ndn::readNonNegativeInteger(fields[5]);
  result.segmentCount = ndn::readNonNegativeInteger(fields[6]);
  result.representationSpec = text(fields[7]);
  result.protectionSpec = text(fields[8]);
  result.authorizationScope = text(fields[9]);
  result.protectionEpoch = text(fields[10]);
  result.keyReferenceId = text(fields[11]);
  result.keyIdentifier = text(fields[12]);
  result.manifestSuffix = readName(fields[13]);
  if (!legacy) {
    if (fields[14].type() != 157) throw std::invalid_argument("SEMANTIC_MANIFEST_FIELD_ORDER_INVALID");
    result.contentName = PublishedContentName::decode(
      {fields[14].value(), fields[14].value() + fields[14].value_size()});
    if (result.contentName->hasSemanticCoordinates()) {
      if (fields.size() != Fields.size() + 3 || fields[15].type() != 159 || fields[16].type() != 161)
        throw std::invalid_argument("BLOB_REPRESENTATION_DESCRIPTOR_MISSING");
      result.publicationNonce.assign(fields[15].value(), fields[15].value() + fields[15].value_size());
      result.signerCertificate = readName(fields[16]);
    }
  }
  if (result.encode(mode) != payload)
    throw std::invalid_argument("BLOB_MANIFEST_NONCANONICAL_WIRE");
  if (result.getName(mode) != expectedManifestName)
    throw std::invalid_argument("BLOB_MANIFEST_PAYLOAD_HASH_MISMATCH");
  return result;
}

namespace {
bool supportedManifestSuffix(const ndn::Name& suffix, DataNameReadMode mode)
{
  if (suffix.empty()) return true;
  if (suffix.size() == 5 && suffix[0] == ndn::name::Component("NDNSF-DI") &&
      suffix[1] == ndn::name::Component("model") && isNamingLabelComponent(suffix[2]) &&
      suffix[3].type() == 8 && suffix[3].value_size() == 32 &&
      suffix[4] == ndn::name::Component("MANIFEST")) return true;
  if (suffix.size() == 4 && suffix[0] == ndn::name::Component("NDNSF-DI") &&
      suffix[1] == ndn::name::Component("material") && isNamingLabelComponent(suffix[2]) &&
      suffix[3] == ndn::name::Component("MANIFEST")) return true;
  if ((suffix.size() == 4 ||
       (suffix.size() == 3 && mode == DataNameReadMode::AllowLegacyRead)) &&
      suffix[0] == ndn::name::Component("NDNSF-DI") &&
      suffix[1] == ndn::name::Component("receipt") && suffix[2].type() == 8 &&
      (suffix.size() == 3 || isNamingLabelComponent(suffix[3]))) {
    const std::string operation(reinterpret_cast<const char*>(suffix[2].value()), suffix[2].value_size());
    if (!operation.empty() && std::all_of(operation.begin(), operation.end(), [](unsigned char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-';
    })) return true;
  }
  if (mode != DataNameReadMode::AllowLegacyRead)
    return false;
  if (suffix.size() == 3 && suffix[0] == ndn::name::Component("MODEL") &&
      suffix[1].type() == 8 && suffix[1].value_size() == 32 && suffix[2] == ndn::name::Component("MANIFEST")) return true;
  return suffix == ndn::Name("/MATERIAL/MANIFEST") || suffix == ndn::Name("/RECEIPT");
}
} // namespace

void ProtectedBlobManifest::validateSuffix(const ndn::Name& suffix, DataNameReadMode mode)
{
  if (mode != DataNameReadMode::StrictCanonical && mode != DataNameReadMode::AllowLegacyRead)
    throw std::invalid_argument("INVALID_MANIFEST_READ_MODE");
  if (!supportedManifestSuffix(suffix, mode))
    throw std::invalid_argument("UNSUPPORTED_PROTECTED_MANIFEST_SUFFIX");
}

std::optional<ndn::Name> ProtectedBlobManifest::inferProducer(const ndn::Name& name,
                                                           DataNameReadMode mode)
{
  if (mode != DataNameReadMode::StrictCanonical && mode != DataNameReadMode::AllowLegacyRead)
    return std::nullopt;
  const auto digest = [](const ndn::name::Component& c) { return c.type() == 8 && c.value_size() == 32; };
  if (name.empty() || (!digest(name.get(-1)) &&
      name[-1] != ndn::name::Component("MANIFEST"))) return std::nullopt;
  if (const auto represented = representationProducer(name, true))
    return represented;
  const auto bodyProducer = semanticProducer(name, true);
  if (bodyProducer) return bodyProducer;
  for (const size_t length : {size_t(6), size_t(5), size_t(4)}) {
    if (name.size() <= length) continue;
    const auto boundary = name.size() - length;
    if (supportedManifestSuffix(name.getSubName(boundary, length - 1),
                                DataNameReadMode::StrictCanonical))
      return name.getPrefix(boundary);
  }
  if (mode != DataNameReadMode::AllowLegacyRead) return std::nullopt;
  if (name.size() > 4) {
    const auto boundary = name.size() - 4;
    const auto suffix = name.getSubName(boundary, 3);
    if (suffix[0] == ndn::name::Component("NDNSF-DI") &&
        suffix[1] == ndn::name::Component("receipt")) {
      if (supportedManifestSuffix(suffix, mode))
        return name.getPrefix(boundary);
    }
  }
  if (name.size() > 7) {
    const auto i = name.size() - 7;
    if (name[i] == ndn::name::Component("NDNSF") && name[i + 1] == ndn::name::Component("BLOB") &&
        digest(name[i + 2]) && name[i + 3] == ndn::name::Component("PROTECTED") &&
        digest(name[i + 4]) && name[i + 5] == ndn::name::Component("MANIFEST")) return name.getPrefix(i);
  }
  if (name.size() > 5) {
    const auto i = name.size() - 5;
    if (name[i] == ndn::name::Component("NDNSF") && name[i + 1] == ndn::name::Component("MODEL") &&
        digest(name[i + 2]) && name[i + 3] == ndn::name::Component("MANIFEST")) return name.getPrefix(i);
  }
  if (name.size() > 4) {
    const auto i = name.size() - 4;
    if (name[i] == ndn::name::Component("NDNSF") && name[i + 1] == ndn::name::Component("MATERIAL") &&
        name[i + 2] == ndn::name::Component("MANIFEST")) return name.getPrefix(i);
  }
  if (name.size() > 3) {
    const auto i = name.size() - 3;
    if (name[i] == ndn::name::Component("NDNSF") && name[i + 1] == ndn::name::Component("RECEIPT")) return name.getPrefix(i);
  }
  return std::nullopt;
}

std::optional<ndn::Name> ProtectedBlobManifest::inferDataProducer(const ndn::Name& name,
                                                               DataNameReadMode mode)
{
  if (mode != DataNameReadMode::StrictCanonical && mode != DataNameReadMode::AllowLegacyRead)
    return std::nullopt;
  if (const auto represented = representationProducer(name, false))
    return represented;
  const auto producer = semanticProducer(name, false);
  if (producer || mode != DataNameReadMode::AllowLegacyRead || name.size() <= 6) return producer;
  const auto boundary = name.size() - 6;
  const auto parsed = parseProducerDataName(name, name.getPrefix(boundary));
  if (parsed && parsed->nameSpace == "NDNSF" && parsed->category == "BLOB" &&
      digestComponent(parsed->suffix[0]) && parsed->suffix[1] == ndn::name::Component("PROTECTED") &&
      digestComponent(parsed->suffix[2]) && parsed->suffix[3] == ndn::name::Component("DATA"))
    return parsed->producer;
  return std::nullopt;
}
} // namespace ndn_service_framework
