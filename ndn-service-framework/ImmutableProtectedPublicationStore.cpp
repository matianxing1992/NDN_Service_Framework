#include "ImmutableProtectedPublicationStore.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ndn_service_framework {

ImmutableProtectedPublicationStore::ImmutableProtectedPublicationStore(
  ndn::Name producer, uint64_t firstVersion)
  : ImmutableProtectedPublicationStore(std::move(producer), firstVersion, Limits{})
{
}

ImmutableProtectedPublicationStore::ImmutableProtectedPublicationStore(
  ndn::Name producer, uint64_t firstVersion, Limits limits)
  : m_producer(std::move(producer))
  , m_limits(limits)
  , m_nextVersion(firstVersion)
{
  if (m_producer.empty() || firstVersion == 0 || limits.entries == 0 ||
      limits.plaintextBytes == 0 || limits.reservationBytes == 0 ||
      limits.reservationBytes > limits.wireBytes) {
    throw std::invalid_argument("invalid immutable protected publication owner bounds");
  }
}

ImmutableProtectedPublicationStore::BeginResult
ImmutableProtectedPublicationStore::begin(
  const ndn::Name& service, const ControllerVersion& generation,
  const ndn::Name& semanticPrefix, const std::string& contentSha256, size_t plaintextBytes)
{
  const bool validDigest = contentSha256.size() == 71 &&
    contentSha256.compare(0, 7, "sha256:") == 0 &&
    std::all_of(contentSha256.begin() + 7, contentSha256.end(), [] (unsigned char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
  if (service.empty() || !m_producer.isPrefixOf(semanticPrefix) ||
      semanticPrefix.size() <= m_producer.size() || semanticPrefix.at(-1).isVersion() ||
      !validDigest || plaintextBytes == 0 || plaintextBytes > m_limits.plaintextBytes) {
    return {};
  }
  const auto binding = m_semanticDigests.find(semanticPrefix);
  if (binding != m_semanticDigests.end() && binding->second != contentSha256) {
    return {Status::IdentityConflict, 0, {}, {}};
  }
  const Key key{semanticPrefix, service, generation.controllerGenerationTimestamp,
                generation.controllerEpoch};
  const auto hit = m_publications.find(key);
  if (hit != m_publications.end()) {
    return {Status::Reused, 0, hit->second->dataName, hit->second};
  }
  for (const auto& pending : m_pending) {
    if (pending.second.semanticPrefix == semanticPrefix) {
      return {pending.second.contentSha256 == contentSha256 ? Status::Busy :
              Status::IdentityConflict, 0, {}, {}};
    }
  }
  if (m_versionsExhausted || m_publications.size() + m_pending.size() >= m_limits.entries ||
      m_retainedBytes > m_limits.wireBytes - m_reservedBytes ||
      m_limits.reservationBytes > m_limits.wireBytes - m_reservedBytes - m_retainedBytes) {
    return {Status::Capacity, 0, {}, {}};
  }
  const auto version = m_nextVersion;
  ndn::Name name(semanticPrefix);
  name.appendVersion(version);
  Reservation pending{key, semanticPrefix, name, contentSha256};
  m_pending.emplace(version, std::move(pending));
  m_reservedBytes += m_limits.reservationBytes;
  if (version == std::numeric_limits<uint64_t>::max()) m_versionsExhausted = true;
  else ++m_nextVersion;
  return {Status::Reserved, version, name, {}};
}

bool
ImmutableProtectedPublicationStore::commit(uint64_t ticket, const Packets& packets)
{
  const auto pending = m_pending.find(ticket);
  if (pending == m_pending.end() || packets.empty()) return false;

  auto publication = std::make_shared<Publication>();
  publication->dataName = pending->second.dataName;
  publication->contentSha256 = pending->second.contentSha256;
  std::map<uint64_t, std::shared_ptr<const ndn::Data>> contentSegments;
  auto nextPackets = m_packets;
  for (const auto& packet : packets) {
    if (!packet || !m_producer.isPrefixOf(packet->getName())) return false;
    const auto& wire = packet->wireEncode();
    if (wire.size() > ndn::MAX_NDN_PACKET_SIZE ||
        wire.size() > m_limits.reservationBytes - publication->wireBytes) return false;
    if (publication->dataName.isPrefixOf(packet->getName())) {
      if (packet->getName().size() != publication->dataName.size() + 1 ||
          !packet->getName().at(-1).isSegment() ||
          !contentSegments.emplace(packet->getName().at(-1).toSegment(), packet).second) {
        return false;
      }
    }
    const auto existing = nextPackets.find(packet->getName());
    if (existing != nextPackets.end()) {
      const auto& oldWire = existing->second->wireEncode();
      if (oldWire.size() != wire.size() ||
          !std::equal(oldWire.begin(), oldWire.end(), wire.begin())) return false;
    }
    else nextPackets.emplace(packet->getName(), packet);
    publication->wireBytes += wire.size();
    publication->packets.push_back(packet);
  }
  const auto first = contentSegments.find(0);
  if (first == contentSegments.end() || !first->second->getFinalBlock() ||
      !first->second->getFinalBlock()->isSegment()) return false;
  const auto finalSegment = first->second->getFinalBlock()->toSegment();
  if (finalSegment >= packets.size() || contentSegments.size() != finalSegment + 1) return false;
  for (uint64_t index = 0; index <= finalSegment; ++index) {
    const auto segment = contentSegments.find(index);
    if (segment == contentSegments.end() || segment->second->getFinalBlock() !=
        first->second->getFinalBlock()) return false;
  }

  // Build all potentially allocating indices before exposing any packet. The
  // swaps below are the sole commit point; a failed preparation remains owned
  // by its reservation until the publisher explicitly rolls it back.
  auto nextPublications = m_publications;
  auto nextBindings = m_semanticDigests;
  if (!nextPublications.emplace(pending->second.key, publication).second) return false;
  const auto binding = nextBindings.emplace(pending->second.semanticPrefix,
                                           pending->second.contentSha256);
  if (!binding.second && binding.first->second != pending->second.contentSha256) return false;
  m_packets.swap(nextPackets);
  m_publications.swap(nextPublications);
  m_semanticDigests.swap(nextBindings);
  m_retainedBytes += publication->wireBytes;
  rollback(ticket);
  return true;
}

void
ImmutableProtectedPublicationStore::rollback(uint64_t ticket) noexcept
{
  const auto pending = m_pending.find(ticket);
  if (pending == m_pending.end()) return;
  m_reservedBytes -= m_limits.reservationBytes;
  m_pending.erase(pending);
}

std::shared_ptr<const ndn::Data>
ImmutableProtectedPublicationStore::find(const ndn::Interest& interest) const
{
  auto name = interest.getName();
  if (!name.empty() && name.at(-1).isImplicitSha256Digest()) name = name.getPrefix(-1);
  const auto packet = m_packets.lower_bound(name);
  if (packet != m_packets.end() && interest.matchesData(*packet->second)) return packet->second;
  return {};
}

} // namespace ndn_service_framework
