#include "ImmutableProtectedPublicationStore.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ndn_service_framework {
namespace {

bool
isSha256Digest(const std::string& digest)
{
  return digest.size() == 71 && digest.compare(0, 7, "sha256:") == 0 &&
    std::all_of(digest.begin() + 7, digest.end(), [] (unsigned char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

} // namespace

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
  std::lock_guard<std::mutex> lock(m_mutex);
  if (service.empty() || !m_producer.isPrefixOf(semanticPrefix) ||
      semanticPrefix.size() <= m_producer.size() || semanticPrefix.at(-1).isVersion() ||
      !isSha256Digest(contentSha256) || plaintextBytes == 0 ||
      plaintextBytes > m_limits.plaintextBytes) {
    return {};
  }
  const Key key{semanticPrefix, service, generation.controllerGenerationTimestamp,
                generation.controllerEpoch};
  return reserve({key, semanticPrefix, {}, contentSha256, {}, m_limits.reservationBytes});
}

ImmutableProtectedPublicationStore::BeginResult
ImmutableProtectedPublicationStore::beginExact(
  const ndn::Name& dataName, const std::string& contentSha256,
  const std::string& protectionSha256, size_t plaintextBytes, size_t reservationBytes)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_producer.isPrefixOf(dataName) || dataName.size() <= m_producer.size() ||
      !isSha256Digest(contentSha256) || !isSha256Digest(protectionSha256) ||
      plaintextBytes > m_limits.plaintextBytes || reservationBytes == 0 ||
      reservationBytes > m_limits.reservationBytes) {
    return {};
  }
  return reserve({Key{dataName, {}, 0, 0}, dataName, dataName, contentSha256,
                  protectionSha256, reservationBytes});
}

ImmutableProtectedPublicationStore::BeginResult
ImmutableProtectedPublicationStore::beginExact(
  Limits limits, const ndn::Name& dataName, const std::string& contentSha256,
  const std::string& protectionSha256, size_t plaintextBytes, size_t reservationBytes)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_producer.isPrefixOf(dataName) || dataName.size() <= m_producer.size()) {
    return {};
  }
  const auto existing = m_packets.lower_bound(dataName);
  if (existing != m_packets.end() && dataName.isPrefixOf(existing->first)) {
    throw std::invalid_argument("immutable named Data publication conflict");
  }
  for (const auto& pending : m_pending) {
    if (pending.second.dataName == dataName) {
      throw std::invalid_argument("immutable named Data publication is already reserved");
    }
  }
  if (!m_exactPublications) {
    m_exactPublications = std::make_unique<ImmutableProtectedPublicationStore>(m_producer, 1, limits);
  }
  else {
    const auto& original = m_exactPublications->m_limits;
    if (original.entries != limits.entries || original.wireBytes != limits.wireBytes ||
        original.plaintextBytes != limits.plaintextBytes ||
        original.reservationBytes != limits.reservationBytes) {
      throw std::invalid_argument("exact publication retention limits changed");
    }
  }
  return m_exactPublications->beginExact(dataName, contentSha256, protectionSha256,
                                        plaintextBytes, reservationBytes);
}

bool
ImmutableProtectedPublicationStore::commitExact(uint64_t ticket, const Packets& packets)
{
  return commitExact(ticket, packets, {});
}

bool
ImmutableProtectedPublicationStore::commitExact(
  uint64_t ticket, const Packets& packets, const ndn::Buffer& objectKey)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_exactPublications) return false;
  for (const auto& packet : packets) {
    if (!packet) return false;
    const auto existing = m_packets.find(packet->getName());
    if (existing != m_packets.end() &&
        existing->second->wireEncode() != packet->wireEncode()) return false;
  }
  return m_exactPublications->commit(ticket, packets, objectKey);
}

void
ImmutableProtectedPublicationStore::rollbackExact(uint64_t ticket) noexcept
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_exactPublications) m_exactPublications->rollback(ticket);
}

ImmutableProtectedPublicationStore::BeginResult
ImmutableProtectedPublicationStore::reserve(Reservation reservation)
{
  const auto bindingValue = reservation.contentSha256 + "/" + reservation.protectionSha256;
  const auto binding = m_semanticBindings.find(reservation.semanticPrefix);
  if (binding != m_semanticBindings.end() && binding->second != bindingValue) {
    return {Status::IdentityConflict, 0, {}, {}};
  }
  const auto hit = m_publications.find(reservation.key);
  if (hit != m_publications.end()) {
    return {Status::Reused, 0, hit->second->dataName, hit->second};
  }
  for (const auto& pending : m_pending) {
    if (pending.second.semanticPrefix == reservation.semanticPrefix) {
      const bool same = pending.second.contentSha256 == reservation.contentSha256 &&
        pending.second.protectionSha256 == reservation.protectionSha256;
      return {same ? Status::Busy : Status::IdentityConflict, 0, {}, {}};
    }
  }
  if (m_versionsExhausted || m_publications.size() + m_pending.size() >= m_limits.entries ||
      m_retainedBytes > m_limits.wireBytes - m_reservedBytes ||
      reservation.reservedWireBytes > m_limits.wireBytes - m_reservedBytes - m_retainedBytes) {
    return {Status::Capacity, 0, {}, {}};
  }
  const auto version = m_nextVersion;
  if (reservation.dataName.empty()) {
    reservation.dataName = reservation.semanticPrefix;
    reservation.dataName.appendVersion(version);
  }
  const auto name = reservation.dataName;
  const auto bytes = reservation.reservedWireBytes;
  m_pending.emplace(version, std::move(reservation));
  m_reservedBytes += bytes;
  if (version == std::numeric_limits<uint64_t>::max()) m_versionsExhausted = true;
  else ++m_nextVersion;
  return {Status::Reserved, version, name, {}};
}

bool
ImmutableProtectedPublicationStore::commit(uint64_t ticket, const Packets& packets)
{
  return commit(ticket, packets, {});
}

bool
ImmutableProtectedPublicationStore::commit(
  uint64_t ticket, const Packets& packets, const ndn::Buffer& objectKey)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto pending = m_pending.find(ticket);
  if (pending == m_pending.end() || packets.empty() ||
      (!objectKey.empty() && objectKey.size() != 32)) return false;

  auto publication = std::make_shared<Publication>();
  publication->dataName = pending->second.dataName;
  publication->contentSha256 = pending->second.contentSha256;
  publication->protectionSha256 = pending->second.protectionSha256;
  publication->objectKey = objectKey;
  std::map<uint64_t, std::shared_ptr<const ndn::Data>> contentSegments;
  auto nextPackets = m_packets;
  for (const auto& packet : packets) {
    if (!packet || !m_producer.isPrefixOf(packet->getName())) return false;
    const auto& wire = packet->wireEncode();
    if (wire.size() > ndn::MAX_NDN_PACKET_SIZE ||
        wire.size() > pending->second.reservedWireBytes - publication->wireBytes) return false;
    if (!publication->protectionSha256.empty() &&
        !publication->dataName.isPrefixOf(packet->getName())) return false;
    if (m_exactPublications) {
      const auto original = m_exactPublications->find(ndn::Interest(packet->getName()));
      if (original && original->wireEncode() != wire) return false;
    }
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
  auto nextBindings = m_semanticBindings;
  if (!nextPublications.emplace(pending->second.key, publication).second) return false;
  const auto bindingValue = pending->second.contentSha256 + "/" +
    pending->second.protectionSha256;
  const auto binding = nextBindings.emplace(pending->second.semanticPrefix, bindingValue);
  if (!binding.second && binding.first->second != bindingValue) return false;
  m_packets.swap(nextPackets);
  m_publications.swap(nextPublications);
  m_semanticBindings.swap(nextBindings);
  m_retainedBytes += publication->wireBytes;
  releaseReservation(ticket);
  return true;
}

void
ImmutableProtectedPublicationStore::rollback(uint64_t ticket) noexcept
{
  std::lock_guard<std::mutex> lock(m_mutex);
  releaseReservation(ticket);
}

void
ImmutableProtectedPublicationStore::releaseReservation(uint64_t ticket) noexcept
{
  const auto pending = m_pending.find(ticket);
  if (pending == m_pending.end()) return;
  m_reservedBytes -= pending->second.reservedWireBytes;
  m_pending.erase(pending);
}

std::shared_ptr<const ndn::Data>
ImmutableProtectedPublicationStore::find(const ndn::Interest& interest) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto name = interest.getName();
  if (!name.empty() && name.at(-1).isImplicitSha256Digest()) name = name.getPrefix(-1);
  const auto packet = m_packets.lower_bound(name);
  if (packet != m_packets.end() && interest.matchesData(*packet->second)) return packet->second;
  return m_exactPublications ? m_exactPublications->find(interest) : nullptr;
}

size_t
ImmutableProtectedPublicationStore::size() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_publications.size();
}

size_t
ImmutableProtectedPublicationStore::retainedWireBytes() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_retainedBytes;
}

size_t
ImmutableProtectedPublicationStore::reservedWireBytes() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_reservedBytes;
}

} // namespace ndn_service_framework
