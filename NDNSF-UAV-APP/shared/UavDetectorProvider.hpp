#ifndef NDNSF_EXAMPLES_UAV_DETECTOR_PROVIDER_HPP
#define NDNSF_EXAMPLES_UAV_DETECTOR_PROVIDER_HPP

#include "UavProtocol.hpp"
#include "UavMultiViewRecognition.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/security/certificate.hpp>

#include <cstdint>
#include <functional>
#include <optional>

namespace ndnsf::examples::uav {

struct UavDetectorProviderConfig
{
  ndn::Name providerIdentity;
  std::string modelId;
  std::string modelDigest;
  std::string qualityProfile;
  std::string deviceClass;
  bool ready = false;
};

struct UavDetectorExecution
{
  bool success = false;
  std::string resultDigest;
  std::string detail;
};

/**
 * Evidence that crossed the NDN Data acceptance boundary.  A descriptor
 * (UavEvidenceReference) is deliberately not enough to execute a detector:
 * this value can only be produced by verifyFetchedData(), after the exact
 * requested name, producer certificate, signature, and content digest have
 * been checked.
 */
struct UavVerifiedEvidence
{
  UavEvidenceReference reference;
  ndn::Buffer content;
  ndn::Name signerIdentity;
  bool nameVerified = false;
  bool signatureVerified = false;
  bool digestVerified = false;

  bool isValid(std::string* reason = nullptr) const;
};

/**
 * Application adapter for one detector artifact. It owns readiness and model
 * provenance, but not NDNSF transport. Evidence bytes arrive only after the
 * caller has fetched and verified the named Data reference.
 */
class UavDetectorProvider
{
public:
  explicit UavDetectorProvider(UavDetectorProviderConfig config);

  const UavDetectorProviderConfig& config() const noexcept { return m_config; }
  bool ready() const noexcept { return m_config.ready; }

  UavProviderCapabilitySnapshot
  capability(uint64_t nowMs, uint64_t queueDepth = 0,
             bool evidenceAccess = true) const;

  UavDetectorExecution
  execute(const UavVerifiedEvidence& evidence) const;

  /**
   * Verify the exact Data name, producer identity, signature, and content
   * digest before execution. The certificate is supplied by the caller because
   * one detector may consume evidence from more than one producer. The
   * returned UavVerifiedEvidence is the only execution input accepted by the
   * detector adapter.
   */
  std::optional<UavVerifiedEvidence>
  verifyFetchedData(const UavEvidenceReference& evidence,
                    const ndn::Data& fetchedData,
                    const ndn::security::Certificate& signerCertificate,
                    std::string* reason = nullptr) const;

  /**
   * Admit content returned by CollaborationContext::fetchSignedExactData().
   * That Core API has already performed exact-name, configured-validator, and
   * expected-producer checks; this adapter rebinds its returned bytes to the
   * same typed evidence boundary and verifies the declared digest again.
   */
  std::optional<UavVerifiedEvidence>
  acceptValidatedContent(const UavEvidenceReference& evidence,
                         ndn::Buffer content,
                         const ndn::Name& verifiedSigner,
                         std::string* reason = nullptr) const;

  UavDetectorExecution
  execute(const UavEvidenceReference& evidence,
          const ndn::Data& fetchedData,
          const ndn::security::Certificate& signerCertificate,
          std::string* reason = nullptr) const;

  /** Execute the registered multi-view algorithm after every input has crossed
   * the same signed-Data acceptance boundary as the single-view path. */
  MultiViewExecutionResult
  executeMultiView(const MultiViewRecognitionJob& job,
                   const MultiViewModelProfile& profile,
                   const std::vector<UavVerifiedEvidence>& evidence,
                   const ndn::Name& terminalOwner,
                   const IMultiViewRecognitionAlgorithm& algorithm =
                     DeterministicMultiViewAlgorithm{}) const;

private:
  UavDetectorProviderConfig m_config;
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_DETECTOR_PROVIDER_HPP
