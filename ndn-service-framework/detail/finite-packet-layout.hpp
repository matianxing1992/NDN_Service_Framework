#ifndef NDN_SERVICE_FRAMEWORK_DETAIL_FINITE_PACKET_LAYOUT_HPP
#define NDN_SERVICE_FRAMEWORK_DETAIL_FINITE_PACKET_LAYOUT_HPP

#include "dependency-repair-packet.hpp"

#include <functional>

namespace ndn_service_framework::detail {

/** Producer-signed transport metadata on the existing object root. It changes
 * no application Content and grants no input admission. Fragments per repair
 * group are uniform so complete repair names follow from bounded scalars.
 */
struct FinitePacketLayout
{
  enum class Framing { SEGMENTED = 0, NAMED_PACKETS = 1 };
  static constexpr uint32_t APP_META_TYPE = 240;

  ndn::Name objectName;
  uint64_t sourceCount = 0;
  uint64_t repairFragmentsPerGroup = 0;
  Framing framing = Framing::SEGMENTED;

  uint64_t
  groupCount() const;

  uint64_t
  totalPacketCount() const;

  ndn::Name
  repairName(uint64_t group, uint64_t fragment) const;

  ndn::Block
  wireEncode(const ndn::Name& producer) const;

  static FinitePacketLayout
  wireDecode(const ndn::Block& wire, const ndn::Name& producer);

  /** Modify an unpublished root only; its Producer must sign it afterwards. */
  void
  applyTo(ndn::Data& root, const ndn::Name& producer) const;

  /** Read only after the ordinary root validation and current-authority gate.
   * Other application MetaInfo is preserved and is not interpreted as a layout.
   */
  static std::optional<FinitePacketLayout>
  readFrom(const ndn::Data& root, const ndn::Name& objectName, const ndn::Name& producer);
};

/** Prepare an entire unpublished object before its owner inserts any packet.
 * Source Content and names are preserved. The root receives the signed layout;
 * segmented sources receive the packet-set FinalBlock before signing and XOR.
 * The returned source prefix is followed by the in-band repair fragments.
 */
std::vector<std::shared_ptr<const ndn::Data>>
prepareFinitePacketSet(std::vector<ndn::Data> sources, FinitePacketLayout layout,
                       const ndn::Name& producer,
                       const std::function<void(ndn::Data&)>& sign,
                       size_t maxWireBytes, size_t repairPayloadBytes);

} // namespace ndn_service_framework::detail
#endif
