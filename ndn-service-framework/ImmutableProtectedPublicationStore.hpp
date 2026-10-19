#ifndef NDN_SERVICE_FRAMEWORK_IMMUTABLE_PROTECTED_PUBLICATION_STORE_HPP
#define NDN_SERVICE_FRAMEWORK_IMMUTABLE_PROTECTED_PUBLICATION_STORE_HPP

#include "ControllerVersion.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/interest.hpp>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace ndn_service_framework {

/** Face-executor-owned retention for immutable, already protected publications.
 * This owner never encrypts, signs, changes packets, or grants access. Versions
 * and original packets survive requests/source changes for this actor lifetime;
 * this is not a durable store or a globally monotonic version allocator.
 */
class ImmutableProtectedPublicationStore
{
public:
  struct Limits
  {
    size_t entries = 128;
    size_t wireBytes = 8 * 1024 * 1024;
    size_t plaintextBytes = 64 * 1024;
    size_t reservationBytes = 512 * 1024;
  };

  using Packets = std::vector<std::shared_ptr<const ndn::Data>>;
  struct Publication
  {
    ndn::Name dataName;
    std::string contentSha256;
    Packets packets;
    size_t wireBytes = 0;
  };

  enum class Status { Reserved, Reused, Invalid, IdentityConflict, Capacity, Busy };
  struct BeginResult
  {
    Status status = Status::Invalid;
    uint64_t ticket = 0;
    ndn::Name dataName;
    std::shared_ptr<const Publication> publication;
  };

  ImmutableProtectedPublicationStore(ndn::Name producer, uint64_t firstVersion);
  ImmutableProtectedPublicationStore(ndn::Name producer, uint64_t firstVersion, Limits limits);

  /** Reserve before crypto/serving. A hit returns the original packet owner.
   * Changed bytes for a committed semantic revision fail across all services.
   * A different service/controller generation gets a distinct global version.
   */
  BeginResult begin(const ndn::Name& service, const ControllerVersion& generation,
                    const ndn::Name& semanticPrefix, const std::string& contentSha256,
                    size_t plaintextBytes);

  /** Atomically expose a complete original packet set, including segment zero.
   * On failure nothing becomes visible; the caller must roll back the ticket.
   * A throw also leaves the reservation available for rollback.
   */
  bool commit(uint64_t ticket, const Packets& packets);
  void rollback(uint64_t ticket) noexcept;

  /** Original-wire serving after IMS eviction; never returns pending packets. */
  std::shared_ptr<const ndn::Data> find(const ndn::Interest& interest) const;

  size_t size() const noexcept { return m_publications.size(); }
  size_t retainedWireBytes() const noexcept { return m_retainedBytes; }
  size_t reservedWireBytes() const noexcept { return m_reservedBytes; }

private:
  using Key = std::tuple<ndn::Name, ndn::Name, uint64_t, uint64_t>;
  struct Reservation
  {
    Key key;
    ndn::Name semanticPrefix;
    ndn::Name dataName;
    std::string contentSha256;
  };

  ndn::Name m_producer;
  Limits m_limits;
  uint64_t m_nextVersion;
  bool m_versionsExhausted = false;
  size_t m_retainedBytes = 0;
  size_t m_reservedBytes = 0;
  std::map<uint64_t, Reservation> m_pending;
  std::map<Key, std::shared_ptr<const Publication>> m_publications;
  std::map<ndn::Name, std::string> m_semanticDigests;
  std::map<ndn::Name, std::shared_ptr<const ndn::Data>> m_packets;
};

} // namespace ndn_service_framework

#endif
