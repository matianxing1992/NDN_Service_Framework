#ifndef NDN_SERVICE_FRAMEWORK_DETAIL_NAC_CONSUMER_TRANSPORT_HPP
#define NDN_SERVICE_FRAMEWORK_DETAIL_NAC_CONSUMER_TRANSPORT_HPP

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/lp/nack.hpp>
#include <ndn-cxx/transport/transport.hpp>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace ndn_service_framework::detail {

/** Bound the CK namespace before Core performs I/O. SDK validation still runs. */
bool isNacCkLocator(const ndn::Name& locator, const ndn::Name& producer);

/** Face-thread adapter for an unchanged NAC Consumer. Core supplies original
 * CK packets through an operation-owned lease; SDK Interest/validation and
 * decryption stay on the SDK Face. No packet enters NFD or an SDK key cache.
 * Unbound legacy/bootstrap traffic uses the ordinary upstream Face.
 */
class NacConsumerTransport final : public ndn::Transport,
                                   public std::enable_shared_from_this<NacConsumerTransport>
{
  struct PacketSet;

public:
  using PacketLease = std::shared_ptr<const PacketSet>;

  explicit NacConsumerTransport(ndn::Face& upstream);

  /** Supply one complete CK object. Call on the shared Face executor, and
   * retain the lease in both SDK terminal callbacks until they are retired.
   * This supplies bytes only; it conveys no trust or service permission.
   */
  PacketLease provide(const std::vector<std::shared_ptr<const ndn::Data>>& packets,
                      std::function<bool()> current);

  void connect(boost::asio::io_context& io, ReceiveCallback receive) override;
  void send(const ndn::Block& block) override;
  void close() override;
  void pause() override;
  void resume() override;

private:
  void deliver(ndn::Block packet, std::function<bool()> current = {});
  void reject(const ndn::Interest& interest,
              ndn::lp::NackReason reason = ndn::lp::NackReason::NO_ROUTE);
  void discardExpired();

  ndn::Face& m_upstream;
  std::vector<std::weak_ptr<const PacketSet>> m_packetSets;
  std::map<uint64_t, ndn::ScopedPendingInterestHandle> m_pending;
  uint64_t m_nextPending = 0;
  uint64_t m_epoch = 0;
};

} // namespace ndn_service_framework::detail
#endif
