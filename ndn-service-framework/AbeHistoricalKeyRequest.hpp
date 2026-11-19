#ifndef NDNSF_ABE_HISTORICAL_KEY_REQUEST_HPP
#define NDNSF_ABE_HISTORICAL_KEY_REQUEST_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-info.hpp>

namespace ndn_service_framework {

/** A historical decryption key a consumer asks a Controller to mint.
 *
 * The request names one archived generation and the policy it needs, and carries the certificate
 * the reply must be encrypted to. It is sent as a Signed Interest, so it also carries a mint time and
 * an expiry: a Controller refuses a request whose signature is stale or whose nonce it has already
 * answered, and the reply itself expires.
 */
struct HistoricalKeyRequest
{
  ndn::Name controllerPrefix;
  uint64_t generation = 0;
  uint64_t epoch = 0;
  /** The protected object the requester wants to read, not just a policy it would like to have.
   *
   * Naming the object is what lets the Controller decide against the object's own authorization instead
   * of against a scope the requester chose for itself.
   */
  ndn::Name objectName;
  /** The producer that published the object, and the service scope it was published for. */
  ndn::Name producerName;
  ndn::Name serviceName;
  /** The content key the object's payload names, which is what the Controller checks the scope against. */
  ndn::Name ckName;
  std::string policy;
  ndn::Name recipientCertificate;
  uint64_t mintedAtMs = 0;
  uint64_t expiresAtMs = 0;

  bool operator==(const HistoricalKeyRequest& other) const;
};

/** Canonical body of a request; its digest names the request. */
std::string
encodeHistoricalKeyRequest(const HistoricalKeyRequest& request);

/** Parse a canonical body; returns false rather than guessing at a malformed one. */
bool
decodeHistoricalKeyRequest(const std::string& body, HistoricalKeyRequest& request);

/** `sha256:` digest of the canonical body, which is what the name and the reply bind. */
std::string
historicalKeyRequestDigest(const HistoricalKeyRequest& request);

/** Request name: `<controller>/NDNSF/HISTORY/DKEY/<generation>/<epoch>/<digest-hex>`.
 *
 * The digest is part of the name, so a reply under this name answers exactly this request and
 * nothing else.
 */
ndn::Name
makeHistoricalKeyRequestName(const ndn::Name& controllerPrefix,
                             const HistoricalKeyRequest& request);

/** Build and sign the Interest that carries a request.
 *
 * The body travels as the Interest parameters, so the signature covers it together with the name.
 * The caller supplies the signing parameters; a consumer signs with the certificate the request
 * names as the recipient, which is the only certificate the Controller will mint for.
 */
void
signHistoricalKeyRequest(ndn::Interest& interest, const HistoricalKeyRequest& request,
                         ndn::KeyChain& keyChain, const ndn::security::SigningInfo& signing);

/** Check the name a request arrived under against the body that travelled with it.
 *
 * A request is usable only when the name names this controller, carries the same generation and
 * epoch as the body, and ends in the digest of that body. Returns false for a name that does not, so
 * a reply can never be published under a name the requester did not choose.
 */
bool
matchHistoricalKeyRequestName(const ndn::Name& name, const ndn::Name& controllerPrefix,
                              const HistoricalKeyRequest& request);

/** Build the signed reply for a request.
 *
 * The content is a text envelope binding the request digest, the recipient certificate, the
 * generation, the policy and the reply's expiry to the key material. Returns false when the request
 * is unusable or the key material is empty, so a caller never answers with an unbound key.
 */
bool
buildHistoricalKeyReply(const HistoricalKeyRequest& request, const std::string& keyBytes,
                        const ndn::security::Certificate& recipient,
                        uint64_t replyExpiresAtMs,
                        const std::function<void(ndn::Data&)>& sign, ndn::Data& result,
                        const ndn::Name& replyName = {});

/** What a consumer learns from a reply after it has checked every binding. */
struct HistoricalKeyReply
{
  ndn::Name requestName;
  ndn::Name objectName;
  /** The key, encrypted to the recipient certificate the request named.
   *
   * Nothing here is readable without the recipient's private key, so a reply in transit or in a cache is
   * not a key hand-out.
   */
  std::string sealedKey;
  uint64_t generation = 0;
  uint64_t epoch = 0;
  uint64_t expiresAtMs = 0;
};

/** Verify a reply against the request the consumer actually sent.
 *
 * Rejects a reply whose name is not the request's, whose digest, recipient, generation, epoch or
 * policy differ, that has expired, or that carries no key material.
 */
bool
verifyHistoricalKeyReply(const ndn::Data& reply, const HistoricalKeyRequest& request,
                         uint64_t nowMs, HistoricalKeyReply& accepted);

/** Verify that the reply was signed by the Controller's key.
 *
 * The binding checks above say what the reply claims; this says who signed it. A consumer needs both
 * before it uses a key, because a binding it can read is not a binding the Controller vouched for.
 */
bool
verifyHistoricalKeyReplySignature(const ndn::Data& reply,
                                  const ndn::security::Certificate& controllerCertificate);

/** Open the key a verified reply carries, with the certificate the request named.
 *
 * Returns false when the reply was sealed to someone else or the local key cannot open it, which is
 * exactly the case an observer of the same reply hits.
 */
bool
unsealHistoricalKeyReply(const HistoricalKeyReply& reply, const ndn::security::Tpm& tpm,
                         const ndn::Name& certificateName, std::string& keyBytes);

/** Decides whether a requester may still read one object of a retired generation.
 *
 * It receives the verified signer identity, the generation the object was published under, the attribute
 * the requester asked for, and the digest of the certificate that actually signed the request, so a
 * Controller can refuse a revoked certificate rather than only a revoked identity. Returning false makes
 * the broker answer nothing, so a Controller that cannot decide fails closed by leaving the hook unset.
 */
using HistoricalKeyAuthorizationCheck =
  std::function<bool(const ndn::Name& identity, uint64_t generation, uint64_t epoch,
                     const std::string& policy, const std::string& signerCertificateDigest)>;

/** The decision part of a historical key responder, with no face of its own.
 *
 * Everything that can be wrong about a request is decided here - name binding, freshness, replay,
 * signer, archived generation - so the Controller and a test drive the same code instead of two
 * copies of the same conditions.
 */
class HistoricalKeyBroker
{
public:
  struct Material
  {
    std::string publicParameters;
    std::string masterSecret;
  };

  using SignerLookup = std::function<bool(const ndn::Name&, ndn::security::Certificate&)>;
  using MaterialLookup = std::function<bool(uint64_t, uint64_t, Material&)>;
  using ReplySigner = std::function<void(ndn::Data&)>;

  HistoricalKeyBroker(SignerLookup signerLookup, MaterialLookup materialLookup,
                      ReplySigner replySigner)
    : m_signerLookup(std::move(signerLookup))
    , m_materialLookup(std::move(materialLookup))
    , m_replySigner(std::move(replySigner))
  {
  }

  /** Supply the permission decision. Without it the broker answers nothing. */
  void
  setAuthorizationCheck(HistoricalKeyAuthorizationCheck check)
  {
    m_authorizationCheck = std::move(check);
  }

  /** Decide one request and produce the reply, or nothing when it must not be answered.
   *
   * `nowMs` is passed in so a test can place itself on either side of an expiry.
   */
  std::optional<ndn::Data>
  answer(const ndn::Interest& interest, const ndn::Name& controllerPrefix, uint64_t nowMs);

  /** The same decisions, for a caller that has already validated the signature.
   *
   * The Controller validates through its trust anchors, which is asynchronous; it hands the result
   * here so there is still one place that decides freshness, replay and which generation to mint
   * from.
   */
  std::optional<ndn::Data>
  answerValidated(const ndn::Interest& interest, const ndn::Name& controllerPrefix, uint64_t nowMs,
                  const ndn::security::Certificate& signer);

private:
  std::optional<ndn::Data>
  finish(const HistoricalKeyRequest& request, uint64_t nowMs, const ndn::Name& replyName,
                  const ndn::security::Certificate& recipient);

  HistoricalKeyAuthorizationCheck m_authorizationCheck;
  SignerLookup m_signerLookup;
  MaterialLookup m_materialLookup;
  ReplySigner m_replySigner;
  std::map<std::string, uint64_t> m_seen;
};

} // namespace ndn_service_framework

#endif // NDNSF_ABE_HISTORICAL_KEY_REQUEST_HPP
