#ifndef NDNSF_HISTORICAL_READ_CLIENT_HPP
#define NDNSF_HISTORICAL_READ_CLIENT_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>

namespace ndn_service_framework {

/** Reads an object published under a generation that has since been retired.
 *
 * The flow is fixed and each step is checked: fetch the payload segments and the content key, ask the
 * Controller for the generation's key through a signed Interest, verify the reply and open it, then
 * reassemble and decrypt. Nothing here trusts the object's name alone - the caller supplies the
 * certificate the CK and payload must verify against, and the generation details are bound into the
 * request the Controller answers.
 */
struct HistoricalReadRequest
{
  ndn::Name controllerPrefix;
  uint64_t generation = 0;
  uint64_t epoch = 0;
  ndn::Name objectName;
  ndn::Name producerName;
  ndn::Name serviceName;
  ndn::Name ckName;
  ndn::Name payloadName;
  ndn::Name publicationName;
  /** Optional Repo location for original packets. It grants no Producer trust. */
  ndn::Name originalDataForwardingHint;
  std::string policy;
  std::vector<uint8_t> ckEvidence;
  std::vector<uint8_t> publicationEvidence;
  ndn::security::Certificate recipientCertificate;
  /** Optional same-identity signing certificate when the RSA recipient key is separate. */
  std::optional<ndn::security::Certificate> signingCertificate;
  /** The Controller whose signature the key reply must carry. Verification is not optional: an envelope
   *  that names the right generation proves nothing about who wrote it. */
  ndn::security::Certificate controllerCertificate;
  /** The producer certificate the CK and payload Data must be signed by. */
  ndn::security::Certificate producerCertificate;
  /** Optionally prevalidated public parameters. Otherwise fetch the retired proof. */
  std::string publicParameters;
  /** Maximum allowed payload segment count. */
  uint32_t maxSegments = 4096;
  uint32_t maxPayloadBytes = 32 * 1024 * 1024;
  /** Per-packet timeout and overall read budget, including proof lookup and DKEY. */
  uint32_t fetchTimeoutMs = 4000;
  uint32_t totalTimeoutMs = 20000;
};

/** Fetch one Data packet by its exact name, requiring the given signer's signature. */
std::optional<std::vector<uint8_t>>
fetchSingleData(ndn::Face& face, const ndn::Name& name, const ndn::security::Certificate& signer,
                uint32_t timeoutMs);

/** Fetch every exact segment of `root`, requiring a consistent signed FinalBlockId. */
std::optional<std::vector<uint8_t>>
fetchSegmentedData(ndn::Face& face, const ndn::Name& root, const ndn::security::Certificate& signer,
                   uint32_t maxSegments, uint32_t timeoutMs);

/** Ask the Controller for the retired generation's key and open it with this identity's key. */
std::optional<std::vector<uint8_t>>
fetchHistoricalKey(ndn::Face& face, ndn::KeyChain& keyChain, const HistoricalReadRequest& request,
                   uint32_t timeoutMs);

/** Discover a retired generation's public parameters through the Controller's signed index and proof.
 *
 * The proof name carries a digest a reader cannot guess, so the index is what makes a cold read possible:
 * index -> bound proof -> parameters, each step signed by the Controller and each digest checked.
 */
std::optional<std::string>
fetchHistoricalPublicParameters(ndn::Face& face, const ndn::Name& controllerPrefix, uint64_t generation,
                                uint64_t epoch, const ndn::security::Certificate& controllerCertificate,
                                uint32_t timeoutMs, const ndn::Name& serviceName = {});

/** Verify a current permission for the object before the acceptance run revokes it. */
bool
primePermissions(ndn::Face& face, ndn::KeyChain& keyChain, const HistoricalReadRequest& request,
                 uint32_t timeoutMs);

/** The whole read: fetch, verify, request the key, unseal, reassemble and decrypt. */
std::optional<std::string>
readHistoricalObject(ndn::Face& face, ndn::KeyChain& keyChain, const HistoricalReadRequest& request);

enum class HistoricalReadFailure
{
  NONE,
  INVALID_DATA,
  ORIGINAL_UNAVAILABLE,
  ARCHIVE_UNAVAILABLE,
  CONTROLLER_UNAVAILABLE,
  DEADLINE_EXPIRED,
  KEY_TIMEOUT,
  KEY_REPLY_INVALID,
  REQUEST_REJECTED,
  NOT_AUTHORIZED,
  EVIDENCE_REQUIRED,
  KEY_UNAVAILABLE,
  AUTHORITY_CHANGED,
  DECRYPTION_FAILED,
};

/** Failed reads report a local boundary or an authenticated Controller refusal. */
std::optional<std::string>
readHistoricalObject(ndn::Face& face, ndn::KeyChain& keyChain,
                     const HistoricalReadRequest& request, HistoricalReadFailure& failure);

} // namespace ndn_service_framework

#endif // NDNSF_HISTORICAL_READ_CLIENT_HPP
