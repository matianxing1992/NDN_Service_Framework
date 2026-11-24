#ifndef NDN_SERVICE_FRAMEWORK_HISTORICAL_OBJECT_PUBLISHER_HPP
#define NDN_SERVICE_FRAMEWORK_HISTORICAL_OBJECT_PUBLISHER_HPP

#include "PolicyStatus.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace ndn_service_framework {

/** Original, signed packets admitted for publication under a service's user read permission. */
struct HistoricalObjectPublication
{
  ndn::Data mapping;
  ndn::Data ck;
  std::vector<std::shared_ptr<const ndn::Data>> segments;
  ndn::Name receiptFullName;
  PolicyStatusData status;
  std::chrono::steady_clock::time_point deadline;

  /** A sink that waits for I/O must call this again immediately before making the packets visible. */
  void requireCurrent() const;
};

/** Publish a durable application object encrypted for users currently entitled to the service.
 *
 * The Producer creates a new immutable name below objectPrefix, binds the exact named CK and
 * ordered original Data to the Controller's signed status, and obtains an authenticated receipt.
 * The synchronous commit receives the packets only after confirmation. It must store/serve the
 * original wires atomically or report failure. Retain the confirmed packet set for a storage
 * retry if its outcome is uncertain. Use a dedicated Face on its owning thread, outside callbacks.
 */
void
publishHistoricalObject(ndn::Face& face, ndn::KeyChain& keys,
                        const ndn::security::Certificate& producer,
                        const ndn::security::Certificate& controller, const ndn::Name& service,
                        const ndn::Name& objectPrefix, ndn::span<const uint8_t> plaintext,
                        const std::function<void(const HistoricalObjectPublication&)>& commit,
                        size_t segmentSize = 7000);

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_HISTORICAL_OBJECT_PUBLISHER_HPP
