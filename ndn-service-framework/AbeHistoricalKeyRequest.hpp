#ifndef NDNSF_ABE_HISTORICAL_KEY_REQUEST_HPP
#define NDNSF_ABE_HISTORICAL_KEY_REQUEST_HPP

#include "ControllerVersion.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-info.hpp>

namespace ndn { class Face; class PendingInterestHandle; }

namespace ndn_service_framework {

class PolicyStatusData;

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
  std::vector<uint8_t> ckEvidence;
  std::vector<uint8_t> publicationEvidence;
  ndn::Name recipientCertificate;
  uint64_t mintedAtMs = 0;
  uint64_t expiresAtMs = 0;
  /** Authority state authorizing this read, distinct from the retired material. */
  ControllerVersion currentControllerVersion;

  bool operator==(const HistoricalKeyRequest& other) const;
};

/** Original Producer's immutable mapping of a historical publication.
 *
 * The object digest covers the ordered concatenation of the original Data
 * packet wires, including signatures, with exact segment names under objectName.
 * The CK FullName carries the implicit digest of the original CK packet.
 */
struct HistoricalPublication
{
  ndn::Name controller;
  ndn::Name producer;
  ndn::Name service;
  ndn::Name objectName;
  ndn::Name ckFullName;
  ndn::Name publicParametersName;
  uint64_t generation = 0;
  uint64_t epoch = 0;
  uint64_t policyGeneration = 0;
  uint64_t policyEpoch = 0;
  uint64_t segments = 0;
  std::string policy;
  std::string objectDigest;
  std::string publicParametersDigest;
};

/** Create the original Producer's signed, content-addressed mapping Data. */
ndn::Data
makeHistoricalPublicationData(const HistoricalPublication& publication, ndn::KeyChain& keyChain,
                              const ndn::security::SigningInfo& signing);

/** Bind a signed publication to the unmodified CK and ordered Producer packets. */
ndn::Data
makeHistoricalPublicationData(const PolicyStatusData& status, const std::string& attribute,
                              const ndn::Data& ck,
                              const std::vector<std::shared_ptr<ndn::Data>>& segments,
                              const ndn::security::Certificate& producer,
                              const ndn::security::Certificate& controller,
                              ndn::KeyChain& keyChain, uint64_t nowMs);

/** Parse and check the mapping's identity. Callers must validate its Producer signature. */
std::optional<HistoricalPublication>
decodeHistoricalPublication(const ndn::Data& data);

/** Create a Controller receipt after admitting the Producer for this service.
 *
 * The receipt names the exact signed publication and uses the currently
 * authorized publication version and its recorded material.
 */
ndn::Data
makeHistoricalPublicationReceipt(const ndn::Data& publication,
                                 const ndn::security::Certificate& producerCertificate,
                                 const PolicyStatusData& currentStatus,
                                 const ndn::security::Certificate& controllerCertificate,
                                 ndn::KeyChain& keyChain, uint64_t nowMs);

/** Verify an original receipt with a signer already trusted for this historical purpose. */
bool
verifyHistoricalPublicationReceipt(const ndn::Data& receipt, const ndn::Data& publication,
                                   const ndn::security::Certificate& receiptSigner);

struct HistoricalPublicationReceipt
{
  ndn::Name publicationFullName;
  ControllerVersion version;
};

/** A receipt for a publication protected by an Authority-signed grant instead of a content key.
 *
 * The DI family never mints a CK, so the CK receipt cannot describe it. This one binds the published
 * object to the grant digest the recipient was given, under the same controller version rules.
 */
struct HistoricalGrantReceipt
{
  ndn::Name publicationFullName;
  std::string grantDigest;          // "sha256:<hex>" of the Authority-signed grant body
  ndn::Name recipient;              // identity the grant was issued to
  std::string service;              // service whose permission the Controller verified
  ControllerVersion version;
};

/** A Producer's request for a grant-based historical receipt.
 *
 * Parallel to HistoricalPublication, which is CK-shaped: this one names the publication wire, the
 * Authority-signed grant digest and the recipient, plus the Controller version in force when the
 * publication was committed.
 */
struct HistoricalGrantReceiptRequest
{
  ndn::Name controllerPrefix;
  std::string publicationWire;      // the signed publication Data, as it was committed
  std::string grantDigest;          // "sha256:<hex>" of the Authority-signed grant body
  ndn::Name recipient;
  std::string service;              // service the publication belongs to; the Controller checks it
  ControllerVersion currentControllerVersion;
};

/** Canonical body of a grant receipt request; its digest names the request. */
std::string
encodeHistoricalGrantReceiptRequest(const HistoricalGrantReceiptRequest& request);

bool
decodeHistoricalGrantReceiptRequest(const std::string& body,
                                    HistoricalGrantReceiptRequest& request);

/** Ask the Controller for a grant-based receipt for a publication this Producer just committed.
 *
 * Parallel to requestHistoricalPublicationReceipt, which carries a CK: the grant request names the
 * publication wire, the Authority-signed grant digest, the recipient and the service. The request is
 * a signed control-RPC Interest under the same operation the Controller already accepts. */
/** Build the signed Interest that asks for a grant receipt; exposed so the request shape can be
 *  asserted without a Face. */
ndn::Interest
makeHistoricalGrantReceiptRequestInterest(const HistoricalGrantReceiptRequest& request,
                                          const ndn::security::Certificate& producer,
                                          ndn::KeyChain& keyChain);

ndn::PendingInterestHandle
requestHistoricalGrantReceipt(ndn::Face& face, ndn::KeyChain& keyChain,
                              const HistoricalGrantReceiptRequest& request,
                              const ndn::security::Certificate& producer,
                              const ndn::security::Certificate& controller,
                              uint32_t lifetimeMs,
                              std::function<void(std::optional<ndn::Name>)> onComplete);

/** Build the canonical grant receipt for a publication the caller just committed. */
ndn::Data
makeHistoricalGrantReceipt(const ndn::Name& controller, const ndn::Name& publicationFullName,
                           const std::string& grantDigest, const ndn::Name& recipient,
                           const std::string& service, const ControllerVersion& version,
                           const ndn::security::Certificate& controllerCertificate,
                           ndn::KeyChain& keyChain);

/** Decode the canonical grant receipt; the caller authenticates the signature or witnessed wire. */
std::optional<HistoricalGrantReceipt>
decodeHistoricalGrantReceipt(const ndn::Data& receipt, const ndn::Name& controller);

/** Decode the canonical receipt. The caller must authenticate its signature or witnessed wire. */
std::optional<HistoricalPublicationReceipt>
decodeHistoricalPublicationReceipt(const ndn::Data& receipt, const ndn::Name& controller);

/** Name a bounded Controller request carrying the original signed publication wire. */
ndn::Interest
makeHistoricalPublicationRequest(const ndn::Data& publication, ndn::KeyChain& keyChain,
                                 const ndn::security::Certificate& producerCertificate);

/** Authenticate the receipt pointer returned for this exact publication request. */
std::optional<ndn::Name>
verifyHistoricalPublicationAcknowledgement(const ndn::Interest& interest, const ndn::Data& reply,
                                          const ndn::Data& publication,
                                          const ndn::security::Certificate& controllerCertificate);

/** Request a receipt on the caller's Face. The returned handle owns cancellation. */
ndn::PendingInterestHandle
requestHistoricalPublicationReceipt(ndn::Face& face, ndn::KeyChain& keyChain,
                                    const ndn::Data& publication,
                                    const ndn::security::Certificate& producer,
                                    const ndn::security::Certificate& controller,
                                    uint32_t lifetimeMs,
                                    std::function<void(std::optional<ndn::Name>)> onComplete);

/** Require the exact publication scope and original CK wire claimed in the request. */
bool
historicalPublicationMatches(const HistoricalPublication& publication,
                             const HistoricalKeyRequest& request, const ndn::Data& ck);

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
 * The caller supplies the signing parameters. A distinct recipient must be named
 * by its complete certificate FullName and belong to the same authorized identity.
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
  ControllerVersion currentControllerVersion;
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
  enum class Failure
  {
    NONE,
    REQUEST_REJECTED,
    NOT_AUTHORIZED,
    KEY_UNAVAILABLE,
  };

  struct Material
  {
    std::string publicParameters;
    std::string masterSecret;
    ndn::Name publicParametersName;
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
   * here, along with a separately validated recipient when it differs from the signer.
   */
  std::optional<ndn::Data>
  answerValidated(const ndn::Interest& interest, const ndn::Name& controllerPrefix, uint64_t nowMs,
                  const ndn::security::Certificate& signer,
                  const ndn::security::Certificate* resolvedRecipient = nullptr,
                  Failure* failure = nullptr);

private:
  std::optional<ndn::Data>
  finish(const HistoricalKeyRequest& request, uint64_t nowMs, const ndn::Name& replyName,
         const ndn::security::Certificate& signer, const ndn::security::Certificate& recipient,
         Failure* failure = nullptr);

  HistoricalKeyAuthorizationCheck m_authorizationCheck;
  SignerLookup m_signerLookup;
  MaterialLookup m_materialLookup;
  ReplySigner m_replySigner;
  std::map<std::string, uint64_t> m_seen;
};

} // namespace ndn_service_framework

#endif // NDNSF_ABE_HISTORICAL_KEY_REQUEST_HPP
