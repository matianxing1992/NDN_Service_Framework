#include "UavDetectorProvider.hpp"

#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <sstream>
#include <stdexcept>

namespace ndnsf::examples::uav {

namespace {

std::string
sha256Digest(const ndn::Buffer& content)
{
  ndn::util::Sha256 digest;
  if (!content.empty()) {
    digest << std::string(reinterpret_cast<const char*>(content.data()), content.size());
  }
  return "sha256:" + digest.toString();
}

bool
validationError(std::string* reason, const std::string& message)
{
  if (reason) {
    *reason = message;
  }
  return false;
}

} // namespace

bool
UavVerifiedEvidence::isValid(std::string* reason) const
{
  if (!reference.isValid(reason)) {
    return false;
  }
  if (!nameVerified || !signatureVerified || !digestVerified) {
    return validationError(reason, "evidence has not crossed the signed-Data acceptance boundary");
  }
  if (signerIdentity != reference.producerIdentity) {
    return validationError(reason, "verified signer does not match evidence producer");
  }
  if (sha256Digest(content) != reference.contentDigest) {
    return validationError(reason, "verified evidence content digest mismatch");
  }
  return true;
}

UavDetectorProvider::UavDetectorProvider(UavDetectorProviderConfig config)
  : m_config(std::move(config))
{
  if (m_config.providerIdentity.empty() || m_config.modelId.empty() ||
      m_config.modelDigest.empty() || m_config.qualityProfile.empty() ||
      m_config.deviceClass.empty()) {
    throw std::invalid_argument("detector provider configuration is incomplete");
  }
}

UavProviderCapabilitySnapshot
UavDetectorProvider::capability(uint64_t nowMs, uint64_t queueDepth,
                                bool evidenceAccess) const
{
  UavProviderCapabilitySnapshot value;
  value.providerIdentity = m_config.providerIdentity;
  value.modelId = m_config.modelId;
  value.modelDigest = m_config.modelDigest;
  value.qualityProfile = m_config.qualityProfile;
  value.deviceClass = m_config.deviceClass;
  value.ready = m_config.ready;
  value.queueDepth = queueDepth;
  value.estimatedStartMs = nowMs + queueDepth;
  value.evidenceAccess = evidenceAccess;
  value.snapshotTimeMs = nowMs;
  return value;
}

UavDetectorExecution
UavDetectorProvider::execute(const UavVerifiedEvidence& evidence) const
{
  UavDetectorExecution result;
  std::string reason;
  if (!m_config.ready) {
    result.detail = "detector provider is not ready";
    return result;
  }
  if (!evidence.isValid(&reason)) {
    result.detail = reason;
    return result;
  }

  ndn::util::Sha256 outputDigest;
  outputDigest << evidence.reference.exactDataName.toUri() << m_config.modelId
               << m_config.modelDigest << evidence.reference.contentDigest;
  result.resultDigest = "sha256:" + outputDigest.toString();
  result.success = true;
  result.detail = "detector executed with verified named evidence";
  return result;
}

std::optional<UavVerifiedEvidence>
UavDetectorProvider::verifyFetchedData(
  const UavEvidenceReference& evidence, const ndn::Data& fetchedData,
  const ndn::security::Certificate& signerCertificate, std::string* reason) const
{
  std::string evidenceReason;
  if (!evidence.isValid(&evidenceReason)) {
    validationError(reason, evidenceReason);
    return std::nullopt;
  }
  if (fetchedData.getName() != evidence.exactDataName) {
    validationError(reason, "fetched Data name does not match evidence reference");
    return std::nullopt;
  }
  const auto keyLocator = fetchedData.getKeyLocator();
  if (!keyLocator || keyLocator->getType() != ndn::tlv::Name ||
      !evidence.producerIdentity.isPrefixOf(keyLocator->getName()) ||
      !fetchedData.getSignatureValue().hasWire()) {
    validationError(reason, "fetched Data signer is not producer-authorized");
    return std::nullopt;
  }
  if (signerCertificate.getIdentity() != evidence.producerIdentity ||
      !evidence.producerIdentity.isPrefixOf(signerCertificate.getName())) {
    validationError(reason, "signer certificate identity does not match evidence producer");
    return std::nullopt;
  }
  try {
    if (!ndn::security::verifySignature(fetchedData, signerCertificate)) {
      validationError(reason, "fetched Data signature verification failed");
      return std::nullopt;
    }
  }
  catch (const std::exception& error) {
    validationError(reason, std::string("fetched Data signature verification error: ") + error.what());
    return std::nullopt;
  }
  ndn::Buffer content(fetchedData.getContent().value_begin(),
                      fetchedData.getContent().value_end());
  UavVerifiedEvidence verified;
  verified.reference = evidence;
  verified.content = std::move(content);
  verified.signerIdentity = signerCertificate.getIdentity();
  verified.nameVerified = true;
  verified.signatureVerified = true;
  verified.digestVerified = sha256Digest(verified.content) == evidence.contentDigest;
  if (!verified.digestVerified) {
    validationError(reason, "fetched Data content digest does not match evidence reference");
    return std::nullopt;
  }
  return verified;
}

std::optional<UavVerifiedEvidence>
UavDetectorProvider::acceptValidatedContent(
  const UavEvidenceReference& evidence, ndn::Buffer content,
  const ndn::Name& verifiedSigner, std::string* reason) const
{
  if (evidence.exactDataName.empty() || verifiedSigner.empty() ||
      verifiedSigner != evidence.producerIdentity) {
    validationError(reason, "validated evidence signer does not match producer");
    return std::nullopt;
  }
  UavVerifiedEvidence verified;
  verified.reference = evidence;
  verified.content = std::move(content);
  verified.signerIdentity = verifiedSigner;
  verified.nameVerified = true;
  verified.signatureVerified = true;
  verified.digestVerified = sha256Digest(verified.content) == evidence.contentDigest;
  if (!verified.digestVerified || !verified.isValid(reason)) {
    if (reason && reason->empty()) {
      *reason = "validated evidence content digest mismatch";
    }
    return std::nullopt;
  }
  return verified;
}

UavDetectorExecution
UavDetectorProvider::execute(const UavEvidenceReference& evidence,
                             const ndn::Data& fetchedData,
                             const ndn::security::Certificate& signerCertificate,
                             std::string* reason) const
{
  const auto verified = verifyFetchedData(evidence, fetchedData, signerCertificate, reason);
  if (!verified) {
    UavDetectorExecution result;
    result.detail = reason && !reason->empty() ? *reason : "fetched Data verification failed";
    return result;
  }
  return execute(*verified);
}

MultiViewExecutionResult
UavDetectorProvider::executeMultiView(
  const MultiViewRecognitionJob& job,
  const MultiViewModelProfile& profile,
  const std::vector<UavVerifiedEvidence>& evidence,
  const ndn::Name& terminalOwner,
  const IMultiViewRecognitionAlgorithm& algorithm) const
{
  std::vector<VerifiedMultiViewInput> inputs;
  inputs.reserve(evidence.size());
  for (const auto& item : evidence) {
    VerifiedMultiViewInput input;
    input.reference.viewId = item.reference.exactDataName.get(-2).toUri();
    input.reference.producerIdentity = item.reference.producerIdentity;
    input.reference.exactDataName = item.reference.exactDataName;
    input.reference.contentDigest = item.reference.contentDigest;
    input.reference.captureTimeMs = item.reference.windowStartMs;
    input.reference.targetId = job.targetId;
    input.reference.mediaType = item.reference.contentType == "image/jpeg" ?
      "image/jpeg" : "image/png";
    input.content = item.content;
    input.signerIdentity = item.signerIdentity;
    input.nameVerified = item.nameVerified;
    input.signatureVerified = item.signatureVerified;
    input.digestVerified = item.digestVerified;
    inputs.push_back(std::move(input));
  }
  return executeMultiViewJob(job, profile, inputs, terminalOwner, algorithm);
}

} // namespace ndnsf::examples::uav
