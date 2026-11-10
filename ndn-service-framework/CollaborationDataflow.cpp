#include "CollaborationDataflow.hpp"
#include "detail/canonical-tlv.hpp"
#include <ndn-cxx/encoding/block-helpers.hpp>
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace ndn_service_framework {
namespace {
constexpr uint32_t Envelope = 0xF400;
constexpr uint32_t Version = 0xF402;
constexpr uint32_t Profile = 0xF404;
constexpr uint32_t Binding = 0xF406;
constexpr uint32_t Role = 0xF408;
constexpr uint32_t Producer = 0xF40A;
constexpr uint32_t Recovery = 0xF40C;
constexpr size_t MaxWireBytes = 64 * 1024;
constexpr size_t MaxBindings = 128;

void require(bool condition)
{
  if (!condition) throw std::invalid_argument("invalid collaboration dataflow metadata");
}

bool isRoleLabel(const std::string& role)
{
  if (role.empty() || role.size() > 128) return false;
  for (size_t i = 0; i < role.size();) {
    const auto lead = static_cast<unsigned char>(role[i++]);
    uint32_t code = lead;
    uint32_t minimum = 0;
    size_t remaining = 0;
    if (lead >= 0xC2 && lead <= 0xDF) { code &= 0x1F; remaining = 1; minimum = 0x80; }
    else if (lead >= 0xE0 && lead <= 0xEF) { code &= 0x0F; remaining = 2; minimum = 0x800; }
    else if (lead >= 0xF0 && lead <= 0xF4) { code &= 0x07; remaining = 3; minimum = 0x10000; }
    else if (lead >= 0x80) return false;
    if (remaining > role.size() - i) return false;
    while (remaining-- > 0) {
      const auto next = static_cast<unsigned char>(role[i++]);
      if ((next & 0xC0) != 0x80) return false;
      code = (code << 6) | (next & 0x3F);
    }
    if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF) ||
        code < 0x20 || (code >= 0x7F && code <= 0x9F)) return false;
  }
  return true;
}
}

void CollaborationAdmissionLimits::validate() const
{
  if (!maxRetainedObjects || maxRetainedObjects > 4096 ||
      !maxRetainedBytes || maxRetainedBytes > 256 * 1024 * 1024 ||
      !maxQueuedCallbacks || maxQueuedCallbacks > 1024 ||
      !maxInFlightReads || maxInFlightReads > 256 ||
      !maxInputAcquisitionMs || maxInputAcquisitionMs > 300000)
    throw std::invalid_argument("invalid collaboration admission limits");
}

ndn::Block
CollaborationDataflowMetadataV1::wireEncode() const
{
  require(profile == "data-admission-v1");
  require(dependencyRecoveryProfile == "arq-v1" ||
          dependencyRecoveryProfile == "xor-packet-set-v1");
  require(!roleBindings.empty());
  ndn::Block wire(Envelope);
  const auto version = ndn::makeNonNegativeIntegerBlock(Version, 1);
  const auto profileBlock = ndn::makeStringBlock(Profile, profile);
  const auto recovery = ndn::makeStringBlock(Recovery, dependencyRecoveryProfile);
  wire.push_back(version);
  wire.push_back(profileBlock);
  wire.push_back(recovery);
  size_t count = 0;
  // At the 64 KiB limit the outer type and length each occupy three bytes.
  size_t encodedBytes = 6 + version.size() + profileBlock.size() + recovery.size();
  for (const auto& [role, values] : roleBindings) {
    require(isRoleLabel(role));
    require(!values.empty() && values.size() <= MaxBindings - count);
    const std::set<ndn::Name> producers(values.begin(), values.end());
    require(producers.size() == values.size());
    for (const auto& producer : producers) {
      require(!producer.empty());
      require(producer.wireEncode().size() <= MaxWireBytes);
      detail::canonicalTlvChildren(producer.wireEncode(), MaxWireBytes / 2);
      ndn::Block binding(Binding);
      binding.push_back(ndn::makeStringBlock(Role, role));
      ndn::Block name(Producer);
      name.push_back(producer.wireEncode());
      name.encode();
      binding.push_back(name);
      binding.encode();
      require(binding.size() <= MaxWireBytes - encodedBytes);
      encodedBytes += binding.size();
      wire.push_back(binding);
      ++count;
    }
  }
  wire.encode();
  require(wire.size() <= MaxWireBytes);
  return wire;
}

CollaborationDataflowMetadataV1
CollaborationDataflowMetadataV1::wireDecode(const ndn::Block& wire)
{
  try {
    require(wire.type() == Envelope && wire.size() <= MaxWireBytes);
    const auto fields = detail::canonicalTlvChildren(wire, 3 + MaxBindings);
    require(fields.size() >= 4 && fields.size() <= 3 + MaxBindings);
    require(fields[0] == ndn::makeNonNegativeIntegerBlock(Version, 1));
    require(fields[1].type() == Profile && fields[2].type() == Recovery);
    CollaborationDataflowMetadataV1 metadata;
    metadata.profile = ndn::readString(fields[1]);
    metadata.dependencyRecoveryProfile = ndn::readString(fields[2]);
    for (size_t i = 3; i < fields.size(); ++i) {
      const auto& binding = fields[i];
      require(binding.type() == Binding);
      const auto pair = detail::canonicalTlvChildren(binding, 2);
      require(pair.size() == 2 && pair[0].type() == Role && pair[1].type() == Producer);
      const auto name = detail::canonicalTlvChildren(pair[1], 1);
      require(name.size() == 1 && name[0].type() == ndn::tlv::Name);
      detail::canonicalTlvChildren(name[0], MaxWireBytes / 2);
      metadata.roleBindings[ndn::readString(pair[0])].emplace_back(name[0]);
    }
    // This also checks duplicate bindings, bounded strings and canonical order.
    require(metadata.wireEncode() == wire);
    return metadata;
  }
  catch (const std::exception&) {
    throw std::invalid_argument("invalid collaboration dataflow metadata");
  }
}
} // namespace ndn_service_framework
