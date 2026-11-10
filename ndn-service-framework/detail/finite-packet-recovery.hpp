#ifndef NDN_SERVICE_FRAMEWORK_DETAIL_FINITE_PACKET_RECOVERY_HPP
#define NDN_SERVICE_FRAMEWORK_DETAIL_FINITE_PACKET_RECOVERY_HPP

#include "finite-packet-layout.hpp"

#include <map>

namespace ndn_service_framework::detail {

/** Immutable bridge from a validated finite layout to the existing exact-read
 * owner. It owns no networking, retries, authorization or mutable repair cache.
 * The caller reserves storage and retains bootstrap originals for its lifetime.
 */
class FinitePacketRecovery
{
public:
  FinitePacketRecovery(FinitePacketLayout layout, std::vector<ndn::Name> sources,
                      std::map<ndn::Name, std::shared_ptr<const ndn::Data>> bootstrap,
                      const ndn::Name& producer);

  const std::vector<ndn::Name>&
  requestedNames() const { return m_requested; }

  const std::vector<size_t>&
  optionalSlots() const { return m_optional; }

  std::optional<std::pair<size_t, size_t>>
  ready(size_t completedSlot, const std::vector<ndn::Buffer>& wires,
        std::vector<size_t>& snapshotSlots) const;

  std::optional<ndn::Buffer>
  recover(const ndn::Name& missing, const std::vector<ndn::Name>& names,
          const std::vector<ndn::Buffer>& wires) const;

private:
  FinitePacketLayout m_layout;
  ndn::Name m_producer;
  std::vector<ndn::Name> m_sources;
  std::map<ndn::Name, std::shared_ptr<const ndn::Data>> m_bootstrap;
  std::vector<ndn::Name> m_requested;
  std::vector<std::optional<size_t>> m_sourceSlots;
  std::vector<size_t> m_slotGroups;
  std::vector<size_t> m_firstRepair;
  std::vector<size_t> m_optional;
  std::string m_rootDigest;
};

} // namespace ndn_service_framework::detail
#endif
