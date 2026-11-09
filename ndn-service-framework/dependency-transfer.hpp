#ifndef NDN_SERVICE_FRAMEWORK_DEPENDENCY_TRANSFER_HPP
#define NDN_SERVICE_FRAMEWORK_DEPENDENCY_TRANSFER_HPP

#include "collaboration-input.hpp"

namespace ndn_service_framework {

struct DependencyPacket
{
  ndn::Name exactName;
  std::optional<uint64_t> wireLength;
  std::optional<std::string> wireSha256;
};

/** A pure native codec projects an authenticated index. All packet I/O,
 * deadlines and retransmissions remain inside the existing Core owner. */
struct DependencyPacketPlan
{
  ndn::Name rootName;
  ndn::Name producer;
  std::vector<DependencyPacket> packets;
  // Authenticated indexes needed to expand the source plan. Bootstrap reads
  // use bounded ARQ; frozen index wire may also belong to an XOR group.
  std::vector<ndn::Name> indexDataNames;
  std::string expectedAggregateWireDigest;
  uint64_t maxWireBytes = 0;
  uint64_t maxPlaintextBytes = 0;
  uint64_t maxPackets = 0;
};

} // namespace ndn_service_framework
#endif
