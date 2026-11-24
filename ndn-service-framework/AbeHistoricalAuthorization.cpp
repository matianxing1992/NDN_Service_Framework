#include "AbeHistoricalAuthorization.hpp"

#include "ProducerDataNaming.hpp"
#include "detail/nac-consumer-transport.hpp"

#include <ndn-cxx/encoding/block-helpers.hpp>
#include <nac-abe/common.hpp>

#include <algorithm>
#include <charconv>

namespace ndn_service_framework {

bool
historicalAttributeIsAuthorized(const std::string& policy,
                                const std::set<std::string>& heldAtPublication,
                                const std::set<std::string>& heldNow)
{
  // Only a plain attribute name is decidable here. Anything carrying an operator is refused rather
  // than interpreted: an "A OR B" a requester wrote would widen what the Controller granted.
  if (policy.empty() || policy.size() > 512)
    return false;
  const bool compound = policy.find(" AND ") != std::string::npos ||
                        policy.find(" OR ") != std::string::npos ||
                        policy.find('|') != std::string::npos ||
                        policy.find('&') != std::string::npos ||
                        policy.find('(') != std::string::npos ||
                        policy.find(')') != std::string::npos;
  if (compound)
    return false;
  if (std::any_of(policy.begin(), policy.end(),
                  [] (unsigned char ch) { return ch < 0x20 || ch == 0x7f; }))
    return false;
  return heldAtPublication.count(policy) != 0 && heldNow.count(policy) != 0;
}

std::string
makeHistoricalGrantRecordHeader(const ndn::Name& identity, uint64_t generation, uint64_t epoch)
{
  return "NDNSF-GRANT-v1\nidentity " + identity.toUri(ndn::name::UriFormat::CANONICAL) +
         "\ngeneration " + std::to_string(generation) + "\nepoch " + std::to_string(epoch) + "\n";
}

std::optional<HistoricalGrantRecord>
decodeHistoricalGrantRecord(std::string_view content)
{
  const auto readLine = [&content] (std::string_view prefix) -> std::string_view {
    const auto end = content.find('\n');
    if (end == std::string_view::npos || content.substr(0, prefix.size()) != prefix ||
        end < prefix.size())
      throw std::invalid_argument("invalid historical grant line");
    const auto value = content.substr(prefix.size(), end - prefix.size());
    content.remove_prefix(end + 1);
    return value;
  };
  const auto readVersion = [] (std::string_view value, uint64_t& version) {
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), version);
    return parsed.ec == std::errc() && parsed.ptr == value.data() + value.size() &&
           !value.empty() && value.front() != '0';
  };
  try {
    if (!readLine("NDNSF-GRANT-v1").empty())
      return std::nullopt;
    const auto identity = readLine("identity ");
    HistoricalGrantRecord result;
    result.identity = ndn::Name(identity);
    const auto generation = readLine("generation ");
    const auto epoch = readLine("epoch ");
    if (result.identity.empty() ||
        result.identity.toUri(ndn::name::UriFormat::CANONICAL) != identity ||
        !readVersion(generation, result.version.controllerGenerationTimestamp) ||
        !readVersion(epoch, result.version.controllerEpoch))
      return std::nullopt;
    while (!content.empty()) {
      ndn::Name service(readLine("granted "));
      if (service.empty())
        return std::nullopt;
      result.services.emplace(std::move(service));
    }
    return result;
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}

bool
historicalGrantRecordCoversPolicy(const std::string& recordContent, const ndn::Name& identity,
                                  uint64_t generation, uint64_t epoch, const std::string& policy)
{
  constexpr std::string_view PERMISSION("/PERMISSION/");
  if (policy.size() <= PERMISSION.size() || policy.compare(0, PERMISSION.size(), PERMISSION) != 0)
    return false;
  ndn::Name service;
  try {
    service = ndn::Name(policy.substr(PERMISSION.size() - 1));
  }
  catch (const std::exception&) {
    return false;
  }
  if (service.empty())
    return false;
  const auto record = decodeHistoricalGrantRecord(recordContent);
  return record && record->identity == identity &&
         record->version == ControllerVersion{generation, epoch} &&
         record->services.count(service) != 0;
}

bool
historicalCkBindsAttribute(const ndn::Name& ckName, const ndn::Name& producer,
                           const ndn::Name& service, const std::string& attribute)
{
  if (ckName.empty() || producer.empty() || service.empty() || attribute.empty())
    return false;
  const auto root = ckName.getPrefix(-1);
  if (detail::isNacCkLocator(root, producer)) {
    if (!ckName[-1].isSegment() || ckName[-1].toSegment() != 0 ||
        attribute != ndn::Name("/PERMISSION").append(service)
                       .toUri(ndn::name::UriFormat::ALTERNATE))
      return false;
    const auto& attributes = root[-1];
    const auto [decoded, value] = ndn::Block::fromBuffer(
      ndn::span<const uint8_t>(attributes.value(), attributes.value_size()));
    return decoded && value.size() == attributes.value_size() &&
      value.type() == ndn::nacabe::TLV_Attribute && ndn::readString(value) == attribute;
  }
  const auto bytes = [] (const ndn::Name& value) {
    const auto& wire = value.wireEncode();
    return std::vector<uint8_t>(wire.data(), wire.data() + wire.size());
  };
  const auto expected = makeNamingDigestComponent(computeNamingDigest(
    "ndnsf-naming/hybrid-key-scope",
    {{1, 1, bytes(service)}, {2, 1, bytes(producer)},
     {3, 2, std::vector<uint8_t>(attribute.begin(), attribute.end())}}));
  auto prefix = makeProducerDataPrefix(producer, "KEY");
  prefix.append("kind=HYBRID-MESSAGE").append("SCOPE").append("SERVICE").append(expected)
        .append("EPOCH");
  if (!prefix.isPrefixOf(ckName))
    return false;
  const auto suffix = ckName.getSubName(prefix.size());
  const auto isDigest = [] (const ndn::name::Component& component) {
    return component.isGeneric() && component.value_size() == 32;
  };
  if (suffix.size() < 2 || !isDigest(suffix[0]) || suffix[1] != ndn::name::Component("OBJECT"))
    return false;
  // Historical single-packet CKs end at the object root. Wrapped CK names add
  // the payload digest and, on packets, the segment component.
  return suffix.size() == 2 ||
         (suffix.size() <= 4 && isDigest(suffix[2]) &&
          (suffix.size() == 3 || suffix[3].isSegment()));
}

std::optional<ndn::Name>
historicalCkLocator(const ndn::Data& packet, const ndn::Name& producer)
{
  const auto& name = packet.getName();
  if (name.empty() || !producer.isPrefixOf(name))
    return std::nullopt;
  const auto root = name.getPrefix(-1);
  if (!detail::isNacCkLocator(root, producer))
    return name;
  const auto segment = ndn::name::Component::fromSegment(0);
  if (name[-1] != segment || !packet.getFinalBlock() || *packet.getFinalBlock() != segment)
    return std::nullopt;
  return root;
}

} // namespace ndn_service_framework
