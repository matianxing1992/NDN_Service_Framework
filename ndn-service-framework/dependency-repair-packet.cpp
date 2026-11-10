#include "detail/dependency-repair-packet.hpp"
#include "detail/canonical-tlv.hpp"
#include "detail/finite-packet-layout.hpp"
#include "detail/finite-packet-recovery.hpp"
#include "xor-repair.hpp"
#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace ndn_service_framework::detail {
namespace {
enum : uint32_t {
  Version = 0xF422, RootName = 0xF428, RootDigest = 0xF42A, Group = 0xF42E,
  Index = 0xF430, Member = 0xF432, WireLength = 0xF434, WireDigest = 0xF436,
  RepairRoot = 0xF438, RepairLength = 0xF43A, RepairDigest = 0xF43C,
  RepairPacket = 0xF440, CodedBytes = 0xF442,
  SourceCount = 0xF444, TotalCount = 0xF446, FragmentsPerGroup = 0xF448,
  FramingField = 0xF44A, Layout = 0xF44C,
};

void require(bool value)
{
  if (!value) throw std::invalid_argument("DEPENDENCY_REPAIR_INVALID");
}

void validateLimits(const DependencyRepairLimits& limits)
{
  require(limits.maxRepairBytes > 0 && limits.maxRepairBytes <= 64 * 1024 &&
          limits.maxSourcePackets > 0 && limits.maxSourcePackets <= 65536 &&
          limits.maxPacketWireBytes > 0 && limits.maxPacketWireBytes <= ndn::MAX_NDN_PACKET_SIZE);
}

bool isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [](unsigned char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void validateName(const ndn::Name& name, const ndn::Name& producer)
{
  require(!producer.empty() && producer.isPrefixOf(name) && name.size() > producer.size() &&
          name.wireEncode().size() <= 4096 && name.size() <= 128);
  canonicalTlvChildren(name.wireEncode(), 128);
}

// Block::parse() eagerly builds every child. Bound the count while scanning,
// before materializing children of an untrusted but authenticated manifest.
std::vector<ndn::Block> children(const ndn::Block& block, size_t maximum)
{
  return canonicalTlvChildren(block, maximum);
}

ndn::Block nameField(uint32_t type, const ndn::Name& name)
{
  ndn::Block field(type);
  field.push_back(name.wireEncode());
  field.encode();
  return field;
}

ndn::Name readName(const ndn::Block& field, uint32_t type)
{
  require(field.type() == type);
  const auto values = children(field, 1);
  require(values.size() == 1 && values.front().type() == ndn::tlv::Name && values.front().size() <= 4096);
  children(values.front(), 128);
  return ndn::Name(values.front());
}

uint64_t readInteger(const ndn::Block& field, uint32_t type)
{
  require(field.type() == type);
  return ndn::readNonNegativeInteger(field);
}

std::string readText(const ndn::Block& field, uint32_t type)
{
  require(field.type() == type && field.value_size() <= 71);
  return ndn::readString(field);
}

size_t groupWidth(const DependencyRepairGroup& group, const DependencyRepairLimits& limits)
{
  require(!group.members.empty() && group.members.size() <= DEPENDENCY_XOR_GROUP_SIZE &&
          group.members.size() <= limits.maxSourcePackets &&
          isDigest(group.plaintextSha256));
  size_t width = 0;
  std::set<ndn::Name> unique;
  for (const auto& member : group.members) {
    require(member.wireLength > 0 && member.wireLength <= limits.maxPacketWireBytes &&
            isDigest(member.wireSha256) && unique.insert(member.name).second);
    width = std::max<uint64_t>(width, member.wireLength);
  }
  require(group.plaintextLength == width);
  return width;
}

ndn::Block
encodeGroup(const DependencyRepairGroup& group, const ndn::Name& producer,
            const DependencyRepairLimits& limits)
{
  groupWidth(group, limits);
  validateName(group.repairRoot, producer);
  ndn::Block encoded(Group);
  encoded.push_back(ndn::makeNonNegativeIntegerBlock(Index, group.index));
  encoded.push_back(nameField(RepairRoot, group.repairRoot));
  encoded.push_back(ndn::makeNonNegativeIntegerBlock(RepairLength, group.plaintextLength));
  encoded.push_back(ndn::makeStringBlock(RepairDigest, group.plaintextSha256));
  for (const auto& member : group.members) {
    validateName(member.name, producer);
    require(member.name != group.repairRoot);
    ndn::Block item(Member);
    item.push_back(member.name.wireEncode());
    item.push_back(ndn::makeNonNegativeIntegerBlock(WireLength, member.wireLength));
    item.push_back(ndn::makeStringBlock(WireDigest, member.wireSha256));
    item.encode();
    encoded.push_back(item);
  }
  encoded.encode();
  return encoded;
}

DependencyRepairGroup
decodeGroup(const ndn::Block& wire, const ndn::Name& producer,
            const DependencyRepairLimits& limits)
{
  require(wire.type() == Group);
  const auto values = children(wire, 12);
  require(values.size() >= 5 && values.size() <= 12);
  DependencyRepairGroup group;
  group.index = readInteger(values[0], Index);
  group.repairRoot = readName(values[1], RepairRoot);
  group.plaintextLength = readInteger(values[2], RepairLength);
  group.plaintextSha256 = readText(values[3], RepairDigest);
  for (size_t j = 4; j < values.size(); ++j) {
    require(values[j].type() == Member);
    const auto item = children(values[j], 3);
    require(item.size() == 3 && item[0].type() == ndn::tlv::Name && item[0].size() <= 4096);
    children(item[0], 128);
    DependencyRepairMember member;
    member.wireLength = readInteger(item[1], WireLength);
    member.wireSha256 = readText(item[2], WireDigest);
    member.name = ndn::Name(item[0]);
    group.members.push_back(std::move(member));
  }
  require(encodeGroup(group, producer, limits) == wire);
  return group;
}
} // namespace

std::string dependencyWireDigest(ndn::span<const uint8_t> bytes)
{
  ndn::util::Sha256 digest;
  if (!bytes.empty()) digest << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  auto text = digest.toString();
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
  return "sha256:" + text;
}


ndn::Block
DependencyRepairPacketV1::wireEncode(const ndn::Name& producer,
                                      const DependencyRepairLimits& limits) const
{
  validateLimits(limits);
  validateName(originalRootName, producer);
  require(isDigest(rootWireSha256) && group.members.size() <= limits.maxSourcePackets &&
          group.index < (limits.maxSourcePackets + DEPENDENCY_XOR_GROUP_SIZE - 1) /
                        DEPENDENCY_XOR_GROUP_SIZE &&
          group.repairRoot != originalRootName && codedBytes.size() == groupWidth(group, limits) &&
          dependencyWireDigest(codedBytes) == group.plaintextSha256);
  for (const auto& member : group.members) {
    if (member.name == originalRootName) require(member.wireSha256 == rootWireSha256);
  }
  ndn::Block wire(RepairPacket);
  wire.push_back(ndn::makeNonNegativeIntegerBlock(Version, 1));
  wire.push_back(nameField(RootName, originalRootName));
  wire.push_back(ndn::makeStringBlock(RootDigest, rootWireSha256));
  wire.push_back(encodeGroup(group, producer, limits));
  wire.push_back(ndn::makeBinaryBlock(CodedBytes,
    ndn::span<const uint8_t>(codedBytes.data(), codedBytes.size())));
  wire.encode();
  require(wire.size() <= limits.maxRepairBytes);
  return wire;
}

DependencyRepairPacketV1
DependencyRepairPacketV1::wireDecode(const ndn::Block& wire, const ndn::Name& producer,
                                     const DependencyRepairLimits& limits)
{
  try {
    validateLimits(limits);
    require(wire.type() == RepairPacket &&
            wire.size() <= limits.maxRepairBytes);
    const auto fields = children(wire, 5);
    require(fields.size() == 5 && fields[0] == ndn::makeNonNegativeIntegerBlock(Version, 1) &&
            fields[4].type() == CodedBytes && fields[4].value_size() <= limits.maxPacketWireBytes);
    DependencyRepairPacketV1 packet;
    packet.originalRootName = readName(fields[1], RootName);
    packet.rootWireSha256 = readText(fields[2], RootDigest);
    packet.group = decodeGroup(fields[3], producer, limits);
    packet.codedBytes.assign(fields[4].value_begin(), fields[4].value_end());
    require(packet.wireEncode(producer, limits) == wire);
    return packet;
  }
  catch (const std::exception&) {
    throw std::invalid_argument("DEPENDENCY_REPAIR_INVALID");
  }
}

namespace {
bool
isLayoutMeta(const ndn::Block& block)
{
  if (block.type() != FinitePacketLayout::APP_META_TYPE) return false;
  auto first = block.value_begin();
  uint32_t type = 0;
  return ndn::tlv::readType(first, block.value_end(), type) && type == Layout;
}

void
validateLayoutRoot(const FinitePacketLayout& layout, const ndn::Data& root)
{
  const auto expected = layout.framing == FinitePacketLayout::Framing::SEGMENTED ?
    ndn::Name(layout.objectName).appendSegment(0) : layout.objectName;
  require(root.getName() == expected && root.getContentType() == ndn::tlv::ContentType_Blob);
}
} // namespace

uint64_t
FinitePacketLayout::groupCount() const
{
  require(sourceCount > 0 && sourceCount <= 65536);
  return (sourceCount + DEPENDENCY_XOR_GROUP_SIZE - 1) / DEPENDENCY_XOR_GROUP_SIZE;
}

uint64_t
FinitePacketLayout::totalPacketCount() const
{
  const auto groups = groupCount();
  require(repairFragmentsPerGroup > 0 &&
          repairFragmentsPerGroup <= (65536 - sourceCount) / groups);
  return sourceCount + groups * repairFragmentsPerGroup;
}

ndn::Name
FinitePacketLayout::repairName(uint64_t group, uint64_t fragment) const
{
  (void)totalPacketCount();
  require(!objectName.empty() && group < groupCount() && fragment < repairFragmentsPerGroup);
  if (framing == Framing::SEGMENTED)
    return ndn::Name(objectName).appendSegment(sourceCount + group * repairFragmentsPerGroup + fragment);
  require(framing == Framing::NAMED_PACKETS);
  return ndn::Name(objectName).append("FEC").appendNumber(group).appendSegment(fragment);
}

ndn::Block
FinitePacketLayout::wireEncode(const ndn::Name& producer) const
{
  validateName(objectName, producer);
  require(framing == Framing::SEGMENTED || framing == Framing::NAMED_PACKETS);
  ndn::Block layout(Layout);
  layout.push_back(ndn::makeNonNegativeIntegerBlock(Version, 1));
  layout.push_back(nameField(RootName, objectName));
  layout.push_back(ndn::makeNonNegativeIntegerBlock(SourceCount, sourceCount));
  layout.push_back(ndn::makeNonNegativeIntegerBlock(TotalCount, totalPacketCount()));
  layout.push_back(ndn::makeNonNegativeIntegerBlock(FragmentsPerGroup, repairFragmentsPerGroup));
  layout.push_back(ndn::makeNonNegativeIntegerBlock(FramingField, static_cast<uint64_t>(framing)));
  layout.encode();
  ndn::Block app(APP_META_TYPE);
  app.push_back(layout);
  app.encode();
  require(app.size() <= ndn::MAX_NDN_PACKET_SIZE);
  return app;
}

FinitePacketLayout
FinitePacketLayout::wireDecode(const ndn::Block& wire, const ndn::Name& producer)
{
  try {
    require(wire.type() == APP_META_TYPE && wire.size() <= ndn::MAX_NDN_PACKET_SIZE);
    const auto container = children(wire, 1);
    require(container.size() == 1 && container.front().type() == Layout);
    const auto fields = children(container.front(), 6);
    require(fields.size() == 6 && fields[0] == ndn::makeNonNegativeIntegerBlock(Version, 1));
    FinitePacketLayout value;
    value.objectName = readName(fields[1], RootName);
    value.sourceCount = readInteger(fields[2], SourceCount);
    const auto total = readInteger(fields[3], TotalCount);
    value.repairFragmentsPerGroup = readInteger(fields[4], FragmentsPerGroup);
    const auto kind = readInteger(fields[5], FramingField);
    require(kind <= static_cast<uint64_t>(Framing::NAMED_PACKETS));
    value.framing = static_cast<Framing>(kind);
    require(value.totalPacketCount() == total && value.wireEncode(producer) == wire);
    return value;
  }
  catch (const std::exception&) {
    throw std::invalid_argument("DEPENDENCY_PACKET_LAYOUT_INVALID");
  }
}

void
FinitePacketLayout::applyTo(ndn::Data& root, const ndn::Name& producer) const
{
  validateLayoutRoot(*this, root);
  const auto encoded = wireEncode(producer);
  auto meta = root.getMetaInfo();
  auto items = meta.getAppMetaInfo();
  items.remove_if(isLayoutMeta);
  items.push_back(encoded);
  meta.setAppMetaInfo(items);
  if (framing == Framing::SEGMENTED)
    meta.setFinalBlock(ndn::name::Component::fromSegment(totalPacketCount() - 1));
  root.setMetaInfo(meta);
}

std::optional<FinitePacketLayout>
FinitePacketLayout::readFrom(const ndn::Data& root, const ndn::Name& expectedObject,
                           const ndn::Name& producer)
{
  std::optional<FinitePacketLayout> layout;
  for (const auto& block : root.getMetaInfo().getAppMetaInfo()) {
    if (!isLayoutMeta(block)) continue;
    require(!layout);
    layout = wireDecode(block, producer);
  }
  if (!layout) return std::nullopt;
  require(layout->objectName == expectedObject);
  validateLayoutRoot(*layout, root);
  if (layout->framing == Framing::SEGMENTED)
    require(root.getFinalBlock() == ndn::name::Component::fromSegment(layout->totalPacketCount() - 1));
  return layout;
}

std::vector<std::shared_ptr<const ndn::Data>>
prepareFinitePacketSet(std::vector<ndn::Data> sources, FinitePacketLayout layout,
                       const ndn::Name& producer,
                       const std::function<void(ndn::Data&)>& sign,
                       size_t maxWireBytes, size_t repairPayloadBytes)
{
  require(sign && !sources.empty() && sources.size() <= 65536 &&
          layout.sourceCount == sources.size() && maxWireBytes > 0 &&
          repairPayloadBytes > 0 && repairPayloadBytes <= ndn::MAX_NDN_PACKET_SIZE);
  validateName(layout.objectName, producer);
  std::set<ndn::Name> names;
  for (size_t i = 0; i < sources.size(); ++i) {
    const auto& source = sources[i];
    validateName(source.getName(), producer);
    require(names.insert(source.getName()).second &&
            source.getContentType() == ndn::tlv::ContentType_Blob &&
            source.getContent().value_size() <= ndn::MAX_NDN_PACKET_SIZE);
    if (layout.framing == FinitePacketLayout::Framing::SEGMENTED)
      require(source.getName() == ndn::Name(layout.objectName).appendSegment(i));
  }
  validateLayoutRoot(layout, sources.front());
  // Measure the real signer envelope locally; this packet is never published.
  const auto probeName = layout.framing == FinitePacketLayout::Framing::SEGMENTED ?
    ndn::Name(layout.objectName).appendSegment(65535) :
    ndn::Name(layout.objectName).append("FEC").appendNumber(layout.groupCount() - 1)
      .appendSegment(65535);
  ndn::Data probe(probeName);
  probe.setFreshnessPeriod(sources.front().getFreshnessPeriod());
  probe.setFinalBlock(ndn::name::Component::fromSegment(65535));
  probe.setContent(ndn::Buffer{});
  sign(probe);
  const auto probeBytes = probe.wireEncode().size();
  // Content and outer TLV lengths can each grow by two bytes below the MTU.
  require(probe.getName() == probeName && probeBytes + 4 < ndn::MAX_NDN_PACKET_SIZE);
  repairPayloadBytes = std::min(repairPayloadBytes, ndn::MAX_NDN_PACKET_SIZE - probeBytes - 4);
  layout.repairFragmentsPerGroup = 1;
  // Only unsigned, unpublished staging changes between iterations. Increasing
  // the uniform fragment count can change signed root/final-block widths.
  for (size_t iteration = 0; iteration < 16; ++iteration) {
    const auto totalCount = layout.totalPacketCount();
    layout.applyTo(sources.front(), producer);
    std::vector<std::shared_ptr<const ndn::Data>> packets;
    packets.reserve(totalCount);
    size_t totalBytes = 0;
    const auto append = [&](std::shared_ptr<const ndn::Data> packet) {
      const auto length = packet->wireEncode().size();
      require(length <= ndn::MAX_NDN_PACKET_SIZE &&
              length <= maxWireBytes - totalBytes);
      totalBytes += length;
      packets.push_back(std::move(packet));
    };
    for (auto& source : sources) {
      if (layout.framing == FinitePacketLayout::Framing::SEGMENTED)
        source.setFinalBlock(ndn::name::Component::fromSegment(totalCount - 1));
      const auto originalName = source.getName();
      sign(source);
      require(source.getName() == originalName);
      append(std::make_shared<const ndn::Data>(source));
    }
    const auto& rootWire = packets.front()->wireEncode();
    const auto rootDigest = dependencyWireDigest(
      ndn::span<const uint8_t>(rootWire.data(), rootWire.size()));
    std::vector<ndn::Buffer> repairs;
    repairs.reserve(layout.groupCount());
    uint64_t requiredFragments = 1;
    size_t stagedRepairBytes = 0;
    for (size_t offset = 0, groupIndex = 0; offset < sources.size();
         offset += DEPENDENCY_XOR_GROUP_SIZE, ++groupIndex) {
      DependencyRepairPacketV1 repair;
      repair.originalRootName = packets.front()->getName();
      repair.rootWireSha256 = rootDigest;
      repair.group.index = groupIndex;
      repair.group.repairRoot = layout.repairName(groupIndex, 0);
      const auto end = std::min(sources.size(), offset + DEPENDENCY_XOR_GROUP_SIZE);
      size_t width = 0;
      for (size_t i = offset; i < end; ++i) {
        const auto& wire = packets[i]->wireEncode();
        width = std::max(width, wire.size());
        repair.group.members.push_back({packets[i]->getName(), wire.size(),
          dependencyWireDigest(ndn::span<const uint8_t>(wire.data(), wire.size()))});
      }
      repair.codedBytes.resize(width);
      std::fill(repair.codedBytes.begin(), repair.codedBytes.end(), 0);
      for (size_t i = offset; i < end; ++i) {
        const auto& wire = packets[i]->wireEncode();
        xorInto(ndn::span<uint8_t>(repair.codedBytes.data(), repair.codedBytes.size()),
                ndn::span<const uint8_t>(wire.data(), wire.size()));
      }
      repair.group.plaintextLength = width;
      repair.group.plaintextSha256 = dependencyWireDigest(repair.codedBytes);
      const auto encoded = repair.wireEncode(producer);
      require(encoded.size() <= maxWireBytes - totalBytes - stagedRepairBytes);
      stagedRepairBytes += encoded.size();
      requiredFragments = std::max<uint64_t>(requiredFragments,
        (encoded.size() + repairPayloadBytes - 1) / repairPayloadBytes);
      repairs.emplace_back(encoded.begin(), encoded.end());
    }
    if (requiredFragments > layout.repairFragmentsPerGroup) {
      layout.repairFragmentsPerGroup = requiredFragments;
      continue;
    }
    bool retryPacking = false;
    for (size_t group = 0; group < repairs.size(); ++group) {
      const auto& body = repairs[group];
      for (uint64_t fragment = 0; fragment < layout.repairFragmentsPerGroup; ++fragment) {
        const auto name = layout.repairName(group, fragment);
        require(names.count(name) == 0);
        auto packet = std::make_shared<ndn::Data>(name);
        packet->setFreshnessPeriod(sources.front().getFreshnessPeriod());
        packet->setFinalBlock(ndn::name::Component::fromSegment(
          layout.framing == FinitePacketLayout::Framing::SEGMENTED ?
          totalCount - 1 : layout.repairFragmentsPerGroup - 1));
        const size_t begin = std::min<uint64_t>(fragment * repairPayloadBytes, body.size());
        const auto length = std::min(repairPayloadBytes, body.size() - begin);
        packet->setContent(ndn::span<const uint8_t>(body.data() + begin, length));
        sign(*packet);
        require(packet->getName() == name);
        const auto lengthOnWire = packet->wireEncode().size();
        if (lengthOnWire > ndn::MAX_NDN_PACKET_SIZE) {
          // Variable-length signatures may exceed the sizing sample. Restage
          // the complete unpublished set with the measured smaller payload.
          const auto excess = lengthOnWire - ndn::MAX_NDN_PACKET_SIZE;
          require(repairPayloadBytes > excess);
          repairPayloadBytes -= excess;
          retryPacking = true;
          break;
        }
        append(std::move(packet));
      }
      if (retryPacking) break;
    }
    if (retryPacking) continue;
    require(packets.size() == totalCount);
    return packets;
  }
  throw std::length_error("DEPENDENCY_PACKET_LAYOUT_DID_NOT_CONVERGE");
}

FinitePacketRecovery::FinitePacketRecovery(FinitePacketLayout layout,
    std::vector<ndn::Name> sources,
    std::map<ndn::Name, std::shared_ptr<const ndn::Data>> bootstrap,
    const ndn::Name& producer)
  : m_layout(std::move(layout))
  , m_producer(producer)
  , m_sources(std::move(sources))
  , m_bootstrap(std::move(bootstrap))
{
  (void)m_layout.wireEncode(producer);
  require(m_sources.size() == m_layout.sourceCount && m_bootstrap.size() <= m_sources.size());
  const auto rootName = m_layout.framing == FinitePacketLayout::Framing::SEGMENTED ?
    ndn::Name(m_layout.objectName).appendSegment(0) : m_layout.objectName;
  require(!m_sources.empty() && m_sources.front() == rootName);
  std::set<ndn::Name> unique;
  m_sourceSlots.resize(m_sources.size());
  m_requested.reserve(m_layout.totalPacketCount() - m_bootstrap.size());
  for (size_t i = 0; i < m_sources.size(); ++i) {
    const auto& name = m_sources[i];
    validateName(name, producer);
    require(unique.insert(name).second);
    if (m_layout.framing == FinitePacketLayout::Framing::SEGMENTED)
      require(name == ndn::Name(m_layout.objectName).appendSegment(i));
    const auto known = m_bootstrap.find(name);
    if (known != m_bootstrap.end()) {
      require(known->second && known->second->getName() == name);
    }
  }
  for (const auto& [name, packet] : m_bootstrap) {
    (void)packet;
    require(unique.count(name) != 0);
  }
  const auto root = m_bootstrap.find(rootName);
  require(root != m_bootstrap.end());
  const auto actual = FinitePacketLayout::readFrom(*root->second, m_layout.objectName, producer);
  require(actual && actual->wireEncode(producer) == m_layout.wireEncode(producer));
  const auto& wire = root->second->wireEncode();
  m_rootDigest = dependencyWireDigest(ndn::span<const uint8_t>(wire.data(), wire.size()));
  for (uint64_t group = 0; group < m_layout.groupCount(); ++group) {
    bool needsRepair = false;
    const auto end = std::min(m_sources.size(),
                             static_cast<size_t>((group + 1) * DEPENDENCY_XOR_GROUP_SIZE));
    for (size_t i = group * DEPENDENCY_XOR_GROUP_SIZE; i < end; ++i) {
      if (m_bootstrap.count(m_sources[i])) continue;
      needsRepair = true;
      m_sourceSlots[i] = m_requested.size();
      m_requested.push_back(m_sources[i]);
      m_slotGroups.push_back(group);
    }
    m_firstRepair.push_back(m_requested.size());
    for (uint64_t fragment = 0; fragment < m_layout.repairFragmentsPerGroup; ++fragment) {
      const auto name = m_layout.repairName(group, fragment);
      require(unique.insert(name).second);
      if (!needsRepair) continue;
      m_optional.push_back(m_requested.size());
      m_requested.push_back(name);
      m_slotGroups.push_back(group);
    }
  }
}

std::optional<std::pair<size_t, size_t>>
FinitePacketRecovery::ready(size_t completedSlot, const std::vector<ndn::Buffer>& wires,
                            std::vector<size_t>& snapshotSlots) const
{
  snapshotSlots.clear();
  if (wires.size() != m_requested.size() || completedSlot >= wires.size()) return std::nullopt;
  const auto group = m_slotGroups[completedSlot];
  const auto firstRepair = m_firstRepair[group];
  for (size_t i = 0; i < m_layout.repairFragmentsPerGroup; ++i) {
    if (wires[firstRepair + i].empty()) return std::nullopt;
  }
  std::optional<size_t> missing;
  const auto end = std::min(m_sources.size(), (group + 1) * DEPENDENCY_XOR_GROUP_SIZE);
  for (size_t i = group * DEPENDENCY_XOR_GROUP_SIZE; i < end; ++i) {
    if (!m_sourceSlots[i]) continue;
    const auto slot = *m_sourceSlots[i];
    if (wires[slot].empty()) {
      if (missing) return std::nullopt;
      missing = slot;
    }
  }
  if (!missing) return std::nullopt;
  for (size_t i = group * DEPENDENCY_XOR_GROUP_SIZE; i < end; ++i)
    if (m_sourceSlots[i]) snapshotSlots.push_back(*m_sourceSlots[i]);
  for (size_t i = 0; i < m_layout.repairFragmentsPerGroup; ++i)
    snapshotSlots.push_back(firstRepair + i);
  return std::make_pair(*missing, firstRepair);
}

std::optional<ndn::Buffer>
FinitePacketRecovery::recover(const ndn::Name& missing, const std::vector<ndn::Name>& names,
                              const std::vector<ndn::Buffer>& wires) const
{
  try {
    require(names == m_requested && wires.size() == names.size());
    const auto source = std::find(m_sources.begin(), m_sources.end(), missing);
    require(source != m_sources.end());
    const size_t sourceIndex = std::distance(m_sources.begin(), source);
    require(m_sourceSlots[sourceIndex] && wires[*m_sourceSlots[sourceIndex]].empty());
    const auto groupIndex = sourceIndex / DEPENDENCY_XOR_GROUP_SIZE;
    const auto firstRepair = m_firstRepair[groupIndex];
    ndn::Buffer encoded;
    for (size_t i = 0; i < m_layout.repairFragmentsPerGroup; ++i) {
      const auto& wire = wires[firstRepair + i];
      require(!wire.empty() && wire.size() <= ndn::MAX_NDN_PACKET_SIZE);
      const ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
      require(packet.getName() == names[firstRepair + i] &&
              packet.getContentType() == ndn::tlv::ContentType_Blob &&
              packet.getFinalBlock() == ndn::name::Component::fromSegment(
                m_layout.framing == FinitePacketLayout::Framing::SEGMENTED ?
                m_layout.totalPacketCount() - 1 : m_layout.repairFragmentsPerGroup - 1));
      const auto& content = packet.getContent();
      require(content.value_size() <= 64 * 1024 - encoded.size());
      encoded.insert(encoded.end(), content.value_begin(), content.value_end());
    }
    const auto repair = DependencyRepairPacketV1::wireDecode(
      ndn::Block(ndn::span<const uint8_t>(encoded.data(), encoded.size())), m_producer);
    require(repair.originalRootName == m_sources.front() && repair.rootWireSha256 == m_rootDigest &&
            repair.group.index == groupIndex &&
            repair.group.repairRoot == m_layout.repairName(groupIndex, 0));
    const auto offset = groupIndex * DEPENDENCY_XOR_GROUP_SIZE;
    const auto end = std::min(m_sources.size(), offset + DEPENDENCY_XOR_GROUP_SIZE);
    require(repair.group.members.size() == end - offset);
    std::vector<std::optional<ndn::Buffer>> originals;
    originals.reserve(end - offset);
    for (size_t i = offset; i < end; ++i) {
      require(repair.group.members[i - offset].name == m_sources[i]);
      if (m_sourceSlots[i]) {
        const auto& wire = wires[*m_sourceSlots[i]];
        originals.push_back(wire.empty() ? std::nullopt : std::optional<ndn::Buffer>(wire));
      }
      else {
        const auto& wire = m_bootstrap.at(m_sources[i])->wireEncode();
        originals.emplace_back(ndn::Buffer(wire.begin(), wire.end()));
      }
    }
    return recoverDependencyWire(repair.group, originals, repair.codedBytes);
  }
  catch (const std::exception&) { return std::nullopt; }
}

std::optional<ndn::Buffer> recoverDependencyWire(const DependencyRepairGroup& group,
    const std::vector<std::optional<ndn::Buffer>>& originals,
    const ndn::Buffer& parity, const DependencyRepairLimits& limits)
{
  try {
    validateLimits(limits);
    const auto width = groupWidth(group, limits);
    if (originals.size() != group.members.size() || parity.size() != width ||
        dependencyWireDigest(parity) != group.plaintextSha256) return std::nullopt;
    size_t missing = originals.size();
    for (size_t i = 0; i < originals.size(); ++i) {
      if (!originals[i]) {
        if (missing != originals.size()) return std::nullopt;
        missing = i;
      }
      else if (originals[i]->size() != group.members[i].wireLength ||
               dependencyWireDigest(*originals[i]) != group.members[i].wireSha256) return std::nullopt;
    }
    if (missing == originals.size()) return std::nullopt;
    ndn::Buffer recovered(parity.begin(), parity.end());
    for (const auto& original : originals)
      if (original) xorInto(ndn::span<uint8_t>(recovered.data(), recovered.size()),
        ndn::span<const uint8_t>(original->data(), original->size()));
    recovered.resize(group.members[missing].wireLength);
    if (dependencyWireDigest(recovered) != group.members[missing].wireSha256) return std::nullopt;
    return recovered;
  }
  catch (const std::exception&) { return std::nullopt; }
}
} // namespace ndn_service_framework::detail
