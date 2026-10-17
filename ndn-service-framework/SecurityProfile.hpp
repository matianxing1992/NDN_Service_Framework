#ifndef NDN_SERVICE_FRAMEWORK_SECURITY_PROFILE_HPP
#define NDN_SERVICE_FRAMEWORK_SECURITY_PROFILE_HPP

#include "NDNSFMessages.hpp"
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-info.hpp>
#include <functional>
#include <memory>

namespace ndn_service_framework {
class MessageValidator;

/** Immutable public encryption metadata. This is never execution authority. */
struct ProducerSecurityProfile
{
  static constexpr uint32_t TYPE = 0xF850;
  static constexpr uint32_t BINARY_TARGETED_ENVELOPE = 1;
  static constexpr uint32_t INLINE_SIGNED_INPUT = 2;
  static constexpr uint32_t COMPACT_REQUEST = 4;
  static constexpr uint32_t SUPPORTED_CAPABILITIES = 7;

  ndn::Name producer;
  ndn::security::Certificate certificate;
  uint32_t capabilities = 0;

  /** Return canonical bytes; reject wrong certificate owner/type or flags. */
  ndn::Block wireEncode() const;
  /** Decode one canonical profile, preserving the original certificate wire. */
  static ProducerSecurityProfile wireDecode(const ndn::Block& wire);
  /** Exact producer-owned content identity, independent of invocation IDs. */
  ndn::Name getName() const;
  /** Derive the legacy full-certificate advertisement digest and validity. */
  EncryptionCertificateAdvertisement advertisement() const;
  /** Derive the distinct legacy Provider public-key digest. */
  std::string publicKeyDigest() const;
  /** Sign one original Data packet; caller publishes/stores it transparently.
   * @throws std::invalid_argument on invalid freshness or an oversized packet.
   * KeyChain/signingInfo must name a signer authorized for producer.
   */
  std::shared_ptr<const ndn::Data> makeData(ndn::KeyChain& keyChain,
    const ndn::security::SigningInfo& signingInfo, ndn::time::milliseconds freshness) const;
};

/** Bounded exact-name cache of validated public profiles, not permissions.
 * Resolve/invalidate and callbacks run on the supplied Face executor. The Face
 * outlives this object. close() may run off-executor and drains deliveries;
 * do not destroy the store/owner inside its own delivery callback.
 */
class SecurityProfileStore
{
public:
  struct Record
  {
    std::shared_ptr<const ndn::Data> packet;
    ProducerSecurityProfile profile;
    EncryptionCertificateAdvertisement advertisement;
    std::string publicKeyDigest;
    std::string publicKeyHex;
  };
  using Handler = std::function<void(std::shared_ptr<const Record>, const std::string&)>;

  SecurityProfileStore(ndn::Face& face, std::shared_ptr<MessageValidator> validator,
                       size_t capacity = 64);
  ~SecurityProfileStore();
  SecurityProfileStore(const SecurityProfileStore&) = delete;
  SecurityProfileStore& operator=(const SecurityProfileStore&) = delete;
  /** Hit delivers without network I/O. Miss coalesces same-name requests.
   * Failure delivers nullptr and a reason; metadata is not authorization.
   */
  void resolve(const ndn::Name& exactName, const ndn::Name& expectedProducer,
               ndn::time::milliseconds timeout, Handler handler);
  /** Invalidate validated metadata and fail pending fetches; preserve replay state. */
  void invalidate();
  /** Cancel pending fetches and suppress subsequent callbacks, then drain deliveries.
   * Calling from a handler cancels immediately and defers draining that handler
   * until it returns. An off-handler close still waits for all active deliveries.
   */
  void close();
  size_t size() const;
  uint64_t fetchCount() const;

private:
  struct State;
  std::shared_ptr<State> m_state;
};
} // namespace ndn_service_framework
#endif
