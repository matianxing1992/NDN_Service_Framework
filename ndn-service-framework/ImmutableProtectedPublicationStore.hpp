#ifndef NDN_SERVICE_FRAMEWORK_IMMUTABLE_PROTECTED_PUBLICATION_STORE_HPP
#define NDN_SERVICE_FRAMEWORK_IMMUTABLE_PROTECTED_PUBLICATION_STORE_HPP

#include "ControllerVersion.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/interest.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace ndn_service_framework {

/** Retention for immutable, already protected publications.
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
    std::string protectionSha256;
    Packets packets;
    /** Private per-object key; never included in a publicly served packet. */
    ndn::Buffer objectKey;
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
   * A different service/controller generation or in-band layout gets a distinct
   * version; transport changes never rewrite an existing original packet set.
   */
  BeginResult begin(const ndn::Name& service, const ControllerVersion& generation,
                    const ndn::Name& semanticPrefix, const std::string& contentSha256,
                    size_t plaintextBytes, bool inBandXor = false);

  /** Reserve an exact supplied name without allocating a published version.
   * The protection digest binds the caller's complete encryption/packet context.
   * Changed content or protection is a conflict, including after packet eviction
   * from another cache. The caller owns serialization and authorization.
   */
  BeginResult
  beginExact(const ndn::Name& dataName, const std::string& contentSha256,
             const std::string& protectionSha256, size_t plaintextBytes,
             size_t reservationBytes);

  /** Admit into a separate exact-name domain without changing versioned limits.
   * This owner serializes admission/commit/find, never external authorization
   * or crypto callbacks. Limits are fixed on first use. A pending duplicate
   * returns Busy; callers retry only after the original operation completes.
   */
  BeginResult
  beginExact(Limits limits, const ndn::Name& dataName, const std::string& contentSha256,
             const std::string& protectionSha256, size_t plaintextBytes,
             size_t reservationBytes);
  bool commitExact(uint64_t ticket, const Packets& packets);
  bool commitExact(uint64_t ticket, const Packets& packets, const ndn::Buffer& objectKey);
  void rollbackExact(uint64_t ticket) noexcept;

  /** Atomically expose a complete original packet set, including segment zero.
   * On failure nothing becomes visible; the caller must roll back the ticket.
   * A throw also leaves the reservation available for rollback.
   */
  bool commit(uint64_t ticket, const Packets& packets);
  bool commit(uint64_t ticket, const Packets& packets, const ndn::Buffer& objectKey);
  void rollback(uint64_t ticket) noexcept;

  /** Original-wire serving after IMS eviction; never returns pending packets. */
  std::shared_ptr<const ndn::Data> find(const ndn::Interest& interest) const;

  size_t size() const;
  size_t retainedWireBytes() const;
  size_t reservedWireBytes() const;

private:
  using Key = std::tuple<ndn::Name, ndn::Name, uint64_t, uint64_t, bool>;
  struct Reservation
  {
    Key key;
    ndn::Name semanticPrefix;
    ndn::Name dataName;
    std::string contentSha256;
    std::string protectionSha256;
    size_t reservedWireBytes;
  };

  BeginResult reserve(Reservation reservation);
  void releaseReservation(uint64_t ticket) noexcept;

  ndn::Name m_producer;
  Limits m_limits;
  uint64_t m_nextVersion;
  bool m_versionsExhausted = false;
  size_t m_retainedBytes = 0;
  size_t m_reservedBytes = 0;
  std::map<uint64_t, Reservation> m_pending;
  std::map<Key, std::shared_ptr<const Publication>> m_publications;
  std::map<ndn::Name, std::string> m_semanticBindings;
  std::map<ndn::Name, std::shared_ptr<const ndn::Data>> m_packets;
  mutable std::mutex m_mutex;
  std::unique_ptr<ImmutableProtectedPublicationStore> m_exactPublications;
};

} // namespace ndn_service_framework

#endif
