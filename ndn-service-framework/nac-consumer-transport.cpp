#include "detail/nac-consumer-transport.hpp"

#include <ndn-cxx/lp/packet.hpp>
#include <ndn-cxx/lp/fields.hpp>
#include <ndn-cxx/lp/tags.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace ndn_service_framework::detail {
namespace {
constexpr size_t MAX_PACKETS = 32;
constexpr size_t MAX_WIRE_BYTES = 64 * 1024;
constexpr size_t MAX_LEASES = 128;
constexpr size_t MAX_UPSTREAM_INTERESTS = 128;

ndn::Block dataFrame(const ndn::Data& data)
{
  if (data.getCongestionMark() == 0) return data.wireEncode();
  ndn::lp::Packet frame(data.wireEncode());
  frame.add<ndn::lp::CongestionMarkField>(data.getCongestionMark());
  return frame.wireEncode();
}

bool isCurrent(const std::function<bool()>& current)
{
  try { return current && current(); }
  catch (...) { return false; }
}
} // namespace

bool
isNacCkLocator(const ndn::Name& locator, const ndn::Name& producer)
{
  const auto offset = producer.size();
  if (producer.empty() || !producer.isPrefixOf(locator) || locator.size() != offset + 4 ||
      locator[offset] != ndn::name::Component("CK") ||
      locator[offset + 1].type() != ndn::tlv::GenericNameComponent ||
      locator[offset + 2] != ndn::name::Component("ENC-BY") ||
      locator[-1].type() != ndn::tlv::GenericNameComponent || locator[-1].value_size() == 0)
    return false;
  const auto& id = locator[offset + 1];
  if (id.value_size() == 0 || id.value_size() > 10) return false;
  const std::string text(reinterpret_cast<const char*>(id.value()), id.value_size());
  uint32_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
    std::to_string(value) == text;
}

struct NacConsumerTransport::PacketSet
{
  ndn::Name root;
  std::vector<std::shared_ptr<const ndn::Data>> packets;
  std::function<bool()> current;
};

NacConsumerTransport::NacConsumerTransport(ndn::Face& upstream)
  : m_upstream(upstream)
{
}

void
NacConsumerTransport::discardExpired()
{
  m_packetSets.erase(std::remove_if(m_packetSets.begin(), m_packetSets.end(),
    [](const auto& entry) { return entry.expired(); }), m_packetSets.end());
}

NacConsumerTransport::PacketLease
NacConsumerTransport::provide(
    const std::vector<std::shared_ptr<const ndn::Data>>& packets,
    std::function<bool()> current)
{
  discardExpired();
  if (packets.empty() || packets.size() > MAX_PACKETS || !current ||
      m_packetSets.size() >= MAX_LEASES || !packets.front() ||
      packets.front()->getName().empty())
    throw std::invalid_argument("original CK packet lease limit or arguments");
  const auto root = packets.front()->getName().getPrefix(-1);
  if (root.empty()) throw std::invalid_argument("original CK root missing");
  size_t wireBytes = 0;
  const auto final = ndn::name::Component::fromSegment(packets.size() - 1);
  for (size_t i = 0; i < packets.size(); ++i) {
    const auto& packet = packets[i];
    if (!packet || packet->getName() != ndn::Name(root).appendSegment(i) ||
        !packet->getFinalBlock() || *packet->getFinalBlock() != final)
      throw std::invalid_argument("original CK packet set is not complete and ordered");
    const auto size = packet->wireEncode().size();
    if (size > ndn::MAX_NDN_PACKET_SIZE || size > MAX_WIRE_BYTES - wireBytes)
      throw std::invalid_argument("original CK wire limit");
    wireBytes += size;
  }
  for (const auto& entry : m_packetSets) {
    const auto live = entry.lock();
    if (!live || live->root != root) continue;
    if (live->packets.size() != packets.size())
      throw std::invalid_argument("original CK immutable conflict");
    for (size_t i = 0; i < packets.size(); ++i) {
      if (live->packets[i]->wireEncode() != packets[i]->wireEncode())
        throw std::invalid_argument("original CK immutable conflict");
    }
  }
  auto lease = std::make_shared<PacketSet>();
  lease->root = root;
  lease->packets = packets;
  lease->current = std::move(current);
  m_packetSets.push_back(lease);
  return lease;
}

void
NacConsumerTransport::connect(boost::asio::io_context& io, ReceiveCallback receive)
{
  if (&io != &m_upstream.getIoContext())
    throw std::invalid_argument("NAC transport must share its upstream executor");
  ndn::Transport::connect(io, std::move(receive));
  setState(State::RUNNING);
}

void
NacConsumerTransport::deliver(ndn::Block packet, std::function<bool()> current)
{
  const auto weak = weak_from_this();
  boost::asio::post(*m_ioCtx, [weak, epoch = m_epoch, packet = std::move(packet),
                             current = std::move(current)] {
    const auto owner = weak.lock();
    if (owner && owner->m_epoch == epoch && owner->getState() == State::RUNNING &&
        owner->m_receiveCallback &&
        (!current || isCurrent(current)))
      owner->m_receiveCallback(packet);
  });
}

void
NacConsumerTransport::reject(const ndn::Interest& interest, ndn::lp::NackReason reason)
{
  ndn::lp::NackHeader header;
  header.setReason(reason);
  ndn::lp::Packet reply(interest.wireEncode());
  reply.add<ndn::lp::NackField>(header);
  deliver(reply.wireEncode());
}

void
NacConsumerTransport::send(const ndn::Block& block)
{
  if (getState() != State::RUNNING) return;
  const ndn::lp::Packet lp(block);
  const auto fragment = lp.get<ndn::lp::FragmentField>();
  const ndn::Block inner({fragment.first, fragment.second});
  if (inner.type() != ndn::tlv::Interest || lp.has<ndn::lp::NackField>())
    throw std::invalid_argument("NAC Consumer transport accepts Interests only");
  ndn::Interest interest(inner);
  if (lp.has<ndn::lp::NextHopFaceIdField>())
    interest.setTag(std::make_shared<ndn::lp::NextHopFaceIdTag>(
      lp.get<ndn::lp::NextHopFaceIdField>()));
  if (lp.has<ndn::lp::CongestionMarkField>())
    interest.setCongestionMark(lp.get<ndn::lp::CongestionMarkField>());
  discardExpired();
  bool bound = false;
  for (const auto& entry : m_packetSets) {
    const auto lease = entry.lock();
    if (!lease || !lease->root.isPrefixOf(interest.getName())) continue;
    bound = true;
    if (!isCurrent(lease->current)) continue;
    for (const auto& packet : lease->packets) {
      if (interest.matchesData(*packet)) {
        deliver(dataFrame(*packet), lease->current);
        return;
      }
    }
  }
  // A bound missing/revoked object cannot fall through to another location.
  if (bound || m_pending.size() >= MAX_UPSTREAM_INTERESTS) {
    reject(interest);
    return;
  }
  const auto id = ++m_nextPending;
  const auto weak = weak_from_this();
  m_pending.emplace(id, m_upstream.expressInterest(interest,
    [weak, id](const auto&, const ndn::Data& data) {
      if (const auto owner = weak.lock()) {
        if (owner->m_pending.erase(id)) owner->deliver(dataFrame(data));
      }
    },
    [weak, id](const auto& request, const ndn::lp::Nack& nack) {
      if (const auto owner = weak.lock()) {
        if (owner->m_pending.erase(id)) owner->reject(request, nack.getReason());
      }
    },
    [weak, id](const auto&) {
      if (const auto owner = weak.lock()) owner->m_pending.erase(id);
    }));
}

void
NacConsumerTransport::close()
{
  m_pending.clear();
  m_packetSets.clear();
  ++m_epoch;
  m_receiveCallback = {};
  setState(State::CLOSED);
}

void
NacConsumerTransport::pause()
{
  m_pending.clear();
  ++m_epoch;
  setState(State::PAUSED);
}

void
NacConsumerTransport::resume()
{
  if (getState() == State::PAUSED) setState(State::RUNNING);
}

} // namespace ndn_service_framework::detail
