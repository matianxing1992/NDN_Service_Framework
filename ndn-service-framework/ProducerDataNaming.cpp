#include "ProducerDataNaming.hpp"

#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace ndn_service_framework {
namespace {

bool label(const std::string& value)
{
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-';
  });
}

bool categoryAllowed(const std::string& nameSpace, const std::string& category)
{
  if (nameSpace == "NDNSF") return label(category) || category == "blob" || category == "event";
  if (nameSpace != "NDNSF-DI") return false;
  static const std::array<const char*, 12> categories{
    "model", "model-alias", "material", "manifest-page", "receipt", "assignment",
    "plan", "input", "invocation", "tensor", "collective", "result"};
  return category == "event" || category == "conversation" ||
    std::any_of(categories.begin(), categories.end(), [&](const char* c) { return category == c; });
}

void requireProducer(const ndn::Name& producer)
{
  if (producer.empty()) throw std::invalid_argument("Producer prefix must be non-root");
}

void appendInteger(std::vector<uint8_t>& bytes, uint64_t value, size_t width)
{
  for (size_t i = width; i > 0; --i)
    bytes.push_back(static_cast<uint8_t>(value >> (8 * (i - 1))));
}

bool validUtf8(const std::vector<uint8_t>& value)
{
  for (size_t i = 0; i < value.size();) {
    const auto first = value[i++];
    if (first < 0x80) continue;
    unsigned count;
    uint32_t code;
    uint32_t minimum;
    if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 0x1f; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 0x0f; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 0x07; minimum = 0x10000; }
    else return false;
    if (count > value.size() - i) return false;
    while (count-- > 0) {
      const auto next = value[i++];
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 0x3f);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
  }
  return true;
}

void validateField(const NamingDigestField& field)
{
  if (field.kind < 1 || field.kind > 5)
    throw std::invalid_argument("unknown naming digest field kind");
  if (field.kind == 3 && field.value.size() != 8)
    throw std::invalid_argument("naming UInt64 must contain eight bytes");
  if ((field.kind == 2 || field.kind == 5) && !validUtf8(field.value))
    throw std::invalid_argument("invalid naming UTF8 field");
  if (field.kind == 1) {
    try {
      const ndn::Block block(ndn::span<const uint8_t>(field.value.data(), field.value.size()));
      const ndn::Name name(block);
      if (block.size() != field.value.size() || name.wireEncode() != block)
        throw std::invalid_argument("incomplete naming Name wire");
    }
    catch (const std::exception&) {
      throw std::invalid_argument("invalid naming Name wire");
    }
  }
}

} // namespace

ndn::Name makeProducerDataPrefix(const ndn::Name& producer, const std::string& category)
{
  return makeProducerDataPrefix(producer, "NDNSF", category);
}

ndn::Name makeProducerDataPrefix(const ndn::Name& producer, const std::string& nameSpace,
                                const std::string& category)
{
  requireProducer(producer);
  if (!categoryAllowed(nameSpace, category))
    throw std::invalid_argument("invalid Data namespace/category");
  return ndn::Name(producer).append(ndn::name::Component(nameSpace))
    .append(ndn::name::Component(category));
}

ndn::name::Component makeNamingLabelComponent(const std::string& value)
{
  const std::vector<uint8_t> bytes(value.begin(), value.end());
  if (value.empty() || !validUtf8(bytes)) throw std::invalid_argument("invalid semantic label UTF8");
  // Exclude Unicode C0/C1 controls, including DEL. All other spelling is preserved.
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (bytes[i] < 0x20 || bytes[i] == 0x7f ||
        (bytes[i] == 0xc2 && i + 1 < bytes.size() && bytes[i + 1] >= 0x80 && bytes[i + 1] <= 0x9f))
      throw std::invalid_argument("semantic label contains control character");
  }
  size_t length = std::min<size_t>(64, bytes.size());
  while (length < bytes.size() && (bytes[length] & 0xc0) == 0x80) --length;
  return ndn::name::Component("name=" + value.substr(0, length));
}

bool matchesNamingLabelComponent(const ndn::name::Component& component,
                                 const std::string& fullLabel)
{
  try { return component == makeNamingLabelComponent(fullLabel); }
  catch (const std::invalid_argument&) { return false; }
}

ndn::name::Component makeNamingDigestComponent(const std::string& hex)
{
  if (hex.size() != 64 || !std::all_of(hex.begin(), hex.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      })) throw std::invalid_argument("naming digest requires full lowerhex SHA-256");
  std::array<uint8_t, SHA256_DIGEST_LENGTH> bytes{};
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
  return ndn::name::Component(ndn::span<const uint8_t>(bytes.data(), bytes.size()));
}

ndn::name::Component makeNamingDecimalComponent(uint64_t value)
{
  return ndn::name::Component(std::to_string(value));
}

std::string computeNamingDigest(const std::string& domain,
                               const std::vector<NamingDigestField>& fields)
{
  if (domain.empty() || domain.size() > std::numeric_limits<uint16_t>::max() ||
      fields.size() > std::numeric_limits<uint16_t>::max() ||
      !std::all_of(domain.begin(), domain.end(), [](unsigned char c) { return c > 0 && c < 128; }))
    throw std::invalid_argument("invalid naming digest domain/count");
  std::vector<uint8_t> transcript;
  appendInteger(transcript, domain.size(), 2);
  transcript.insert(transcript.end(), domain.begin(), domain.end());
  appendInteger(transcript, 2, 2); // Internal transcript schema, never a Name component.
  appendInteger(transcript, fields.size(), 2);
  uint16_t previous = 0;
  bool first = true;
  for (const auto& field : fields) {
    if (!first && field.tag <= previous)
      throw std::invalid_argument("naming digest tags must be strictly increasing");
    validateField(field);
    previous = field.tag;
    first = false;
    if (transcript.size() > transcript.max_size() - 11 ||
        field.value.size() > transcript.max_size() - transcript.size() - 11)
      throw std::invalid_argument("naming digest field length overflow");
    appendInteger(transcript, field.tag, 2);
    transcript.push_back(field.kind);
    appendInteger(transcript, field.value.size(), 8);
    transcript.insert(transcript.end(), field.value.begin(), field.value.end());
  }
  std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{};
  SHA256(transcript.data(), transcript.size(), digest.data());
  constexpr char HEX[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (const auto byte : digest) { result.push_back(HEX[byte >> 4]); result.push_back(HEX[byte & 15]); }
  return result;
}

ndn::name::Component makeReversibleNameComponent(const ndn::Name& name)
{
  return ndn::name::Component(name.toUri(ndn::name::UriFormat::CANONICAL));
}

std::optional<ndn::Name> parseReversibleNameComponent(const ndn::name::Component& component)
{
  if (component.type() != ndn::tlv::GenericNameComponent) return std::nullopt;
  try {
    const std::string uri(reinterpret_cast<const char*>(component.value()), component.value_size());
    if (uri.empty() || uri.front() != '/') return std::nullopt;
    ndn::Name name(uri);
    if (name.toUri(ndn::name::UriFormat::CANONICAL) != uri) return std::nullopt;
    return name;
  }
  catch (const std::exception&) { return std::nullopt; }
}

std::optional<ParsedProducerDataName> parseProducerDataName(
  const ndn::Name& name, const ndn::Name& producer)
{
  const auto boundary = producer.size();
  if (producer.empty() || !producer.isPrefixOf(name) || name.size() <= boundary + 2 ||
      name[boundary].type() != ndn::tlv::GenericNameComponent ||
      name[boundary + 1].type() != ndn::tlv::GenericNameComponent) return std::nullopt;
  const auto& component = name[boundary + 1];
  const std::string category(reinterpret_cast<const char*>(component.value()), component.value_size());
  const auto& marker = name[boundary];
  const std::string nameSpace(reinterpret_cast<const char*>(marker.value()), marker.value_size());
  if (!categoryAllowed(nameSpace, category)) return std::nullopt;
  return ParsedProducerDataName{producer, category, name.getSubName(boundary + 2), nameSpace};
}

void checkNamingNameSize(const ndn::Name& name, size_t maximum)
{
  if (maximum == 0) throw std::invalid_argument("zero naming wire budget");
  const auto size = name.wireEncode().size();
  if (size > maximum)
    throw std::length_error("NAME_TOO_LONG: wire=" + std::to_string(size) +
                            " limit=" + std::to_string(maximum));
}

ndn::Name makeProducerRpcName(const ndn::Name& producer, const std::string& operation,
                              const std::string& callIdentityDigest, const std::string& nonce)
{
  if (!label(operation) || nonce.empty()) throw std::invalid_argument("invalid RPC operation/nonce");
  return makeProducerDataPrefix(producer, "RPC").append(ndn::name::Component(operation))
    .append(makeNamingDigestComponent(callIdentityDigest)).append(ndn::name::Component(nonce));
}

} // namespace ndn_service_framework
