#include "ServiceController.hpp"
#include "CertificateBootstrap.hpp"
#include "utils.hpp"

#include <ndn-cxx/security/transform.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/logger.hpp>
#include <ndn-cxx/util/random.hpp>
#include <ndn-cxx/util/scope.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <ndn-cxx/util/sha256.hpp>

#include <chrono>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>

namespace ndn_service_framework {

NDN_LOG_INIT(ndn_service_framework.ServiceController);

namespace {

ndn::span<const uint8_t>
bufferToSpan(const ndn::Buffer& buffer)
{
  return ndn::span<const uint8_t>(buffer.data(), buffer.size());
}

ndn::span<uint8_t>
mutableBufferToSpan(ndn::Buffer& buffer)
{
  return ndn::span<uint8_t>(buffer.data(), buffer.size());
}

ndn::Buffer
runAesCbc(ndn::span<const uint8_t> input,
          ndn::span<const uint8_t> key,
          ndn::span<const uint8_t> iv,
          ndn::CipherOperator op)
{
  ndn::OBufferStream output;
  ndn::security::transform::bufferSource(input) >>
    ndn::security::transform::blockCipher(ndn::BlockCipherAlgorithm::AES_CBC,
                                          op,
                                          key,
                                          iv) >>
    ndn::security::transform::streamSink(output);

  const auto result = output.buf();
  return ndn::Buffer(result->begin(), result->end());
}

size_t
fnv1aPolicyHash(const std::string& path)
{
  constexpr uint64_t offsetBasis = 1469598103934665603ULL;
  constexpr uint64_t prime = 1099511628211ULL;
  uint64_t hash = offsetBasis;

  std::ifstream input(path, std::ios::binary);
  char ch = 0;
  while (input.get(ch)) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= prime;
  }

  if (hash == 0) {
    hash = 1;
  }
  if constexpr (sizeof(size_t) < sizeof(uint64_t)) {
    hash ^= hash >> (sizeof(size_t) * 8);
  }
  return static_cast<size_t>(
    hash & static_cast<uint64_t>(std::numeric_limits<size_t>::max()));
}

size_t
computePolicyEpoch(const std::string& path)
{
  if (const char* envEpoch = std::getenv("NDNSF_POLICY_EPOCH")) {
    try {
      const auto parsed = std::stoull(envEpoch);
      if (parsed > 0) {
        return static_cast<size_t>(parsed);
      }
    }
    catch (const std::exception&) {
      NDN_LOG_WARN("Ignoring invalid NDNSF_POLICY_EPOCH=" << envEpoch);
    }
  }

  return fnv1aPolicyHash(path);
}

uint64_t
nowMilliseconds()
{
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

ndn::Name
makeAuthorizationAttribute(const char* prefix, const std::string& service)
{
  ndn::Name attribute(prefix);
  attribute.append(ndn::Name(service));
  return attribute;
}

std::string
sha256FileDigest(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  std::vector<uint8_t> bytes;
  if (input) {
    bytes.assign(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
  }
  if (bytes.empty()) {
    bytes.assign(path.begin(), path.end());
  }
  ndn::util::Sha256 digest;
  digest << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return "sha256:" + digest.toString();
}

std::string
makeBootstrapToken(size_t length)
{
  static constexpr std::string_view alphabet =
    "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  ndn::Buffer randomBytes(length);
  ndn::random::generateSecureBytes(mutableBufferToSpan(randomBytes));

  std::string token;
  token.reserve(length);
  for (uint8_t byte : randomBytes) {
    token.push_back(alphabet[byte % alphabet.size()]);
  }
  return token;
}

ndn::Block
encryptWireBytesWithContentKeyForCertificate(const ndn::security::Certificate& cert,
                                             ndn::span<const uint8_t> plaintext)
{
  ndn::security::transform::PublicKey recipientPublicKey;
  recipientPublicKey.loadPkcs8(cert.getPublicKey());
  if (recipientPublicKey.getKeyType() != ndn::KeyType::RSA) {
    throw std::invalid_argument("Content encryption requires an RSA recipient certificate");
  }

  ndn::Buffer contentKey(32);
  ndn::Buffer iv(16);
  ndn::random::generateSecureBytes(mutableBufferToSpan(contentKey));
  ndn::random::generateSecureBytes(mutableBufferToSpan(iv));

  ndn::Buffer cipherText = runAesCbc(plaintext,
                                     bufferToSpan(contentKey),
                                     bufferToSpan(iv),
                                     ndn::CipherOperator::ENCRYPT);
  auto encryptedContentKey = recipientPublicKey.encrypt(bufferToSpan(contentKey));

  EncryptedPermissionResponse encryptedPayload;
  encryptedPayload.setRecipientCertName(cert.getName().toUri());
  encryptedPayload.setAlgorithm("RSA-WRAPPED-AES-CBC");
  encryptedPayload.setEncryptedAesKey(
    ndn::Buffer(encryptedContentKey->begin(), encryptedContentKey->end()));
  encryptedPayload.setIv(iv);
  encryptedPayload.setCipherText(cipherText);
  return encryptedPayload.WireEncode();
}

ndn::Name
stripTrailingParametersDigest(ndn::Name name)
{
  if (!name.empty() && name[-1].isParametersSha256Digest()) {
    name = name.getPrefix(-1);
  }
  return name;
}

ndn::Name
stripCertificateBootstrapInterestSuffix(const ndn::Name& name)
{
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i].isParametersSha256Digest()) {
      return name.getPrefix(i);
    }
  }
  return stripTrailingParametersDigest(name);
}

ndn::Block
makeCertificateBootstrapProofData(const CertificateBootstrapRequest& request)
{
  ndn::Block block(bootstrap_tlv::CertificateBootstrapProofData);
  block.push_back(request.identity.wireEncode());
  block.push_back(ndn::makeStringBlock(tlv::TokenType, request.token));
  ndn::Block certBlock(bootstrap_tlv::CertificateRequest);
  certBlock.push_back(request.certificateRequest.wireEncode());
  certBlock.encode();
  block.push_back(certBlock);
  block.push_back(ndn::makeBinaryBlock(bootstrap_tlv::ProofNonce,
                                       request.proofNonce.begin(),
                                       request.proofNonce.end()));
  block.encode();
  return block;
}

} // namespace

ServiceController::ServiceController(ndn::Face& face,
                                     ndn::security::Certificate aaCert,
                                     ndn::ValidatorConfig& validator,
                                     const std::string& configFilePath)
  : m_configFilePath(configFilePath)
  , m_face(face)
  , m_aaCert(std::move(aaCert))
  , m_validator(validator) // 关键：用传入的引用
  , m_keyChain()
  , m_aa(m_aaCert, m_face, m_validator, m_keyChain)
{
  // Load provider/user policies from config
  loadConfigFiles();
  m_policyValidFromMs = 0;
  m_policyEpoch = computePolicyEpoch(m_configFilePath);
  m_requiredKeyEpoch = m_policyEpoch;

  // Establish a durable Controller generation before any protected material
  // can be issued.  A failed lease or corrupt state leaves the Controller
  // unable to publish permission/status Data rather than reusing a version.
  m_generationReady = initializeControllerGeneration();

  if (m_generationReady) {
    initializeAbeGenerationIdentity();
  }

  // Convert policies into ABE attribute policies and register them in AA
  addAttributesForUsersAccordingToServicePolicy();

  // Build fast lookup tables for Interest answering
  buildLookupTables();
}

bool
ServiceController::initializeControllerGeneration()
{
  const auto identity = m_aaCert.getIdentity();
  const auto configured = std::getenv("NDNSF_CONTROLLER_GENERATION_STATE");
  fs::path statePath;
  if (configured != nullptr && *configured != '\0') {
    statePath = configured;
  }
  else {
    const auto key = fnv1aPolicyHash(m_configFilePath + identity.toUri());
    statePath = fs::path("/tmp") /
                ("ndnsf-controller-generation-" + std::to_string(key) + ".state");
  }

  try {
    m_generationStore = std::make_unique<ControllerGenerationStore>(statePath);
    if (!m_generationStore->acquireWriter(identity.toUri())) {
      NDN_LOG_ERROR("Controller generation writer unavailable identity="
                    << identity.toUri() << " state=" << statePath);
      return false;
    }
    const auto now = nowMilliseconds();
    m_revocations = m_generationStore->loadRevocations();
    m_controllerVersion = m_generationStore->startGeneration(now, m_revocations);
    m_policyEpoch = static_cast<size_t>(m_controllerVersion.controllerEpoch);
    m_requiredKeyEpoch = m_policyEpoch;
    m_policyValidFromMs = now;
    m_policyValidUntilMs = now + 24ULL * 60ULL * 60ULL * 1000ULL;
    NDN_LOG_INFO("NDNSF_CONTROLLER_VERSION generation="
                 << m_controllerVersion.controllerGenerationTimestamp
                 << " epoch=" << m_controllerVersion.controllerEpoch
                 << " state=" << statePath);
    return true;
  }
  catch (const std::exception& error) {
    NDN_LOG_ERROR("Controller generation initialization failed state="
                  << statePath << " error=" << error.what());
    return false;
  }
}

ControllerVersion
ServiceController::getControllerVersion() const
{
  return m_controllerVersion;
}

std::string
ServiceController::certificateDigest(const ndn::security::Certificate& certificate)
{
  const auto wire = certificate.wireEncode();
  ndn::util::Sha256 digest;
  // Certificate digests are exchanged in revocation targets and encryption
  // advertisements.  Hash the canonical encoded certificate block, not only
  // its Value, so Controller, User, and Provider compute the same identity
  // for certificate-only revocation.
  digest << std::string(reinterpret_cast<const char*>(wire.data()), wire.size());
  auto hex = digest.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [](unsigned char value) {
    return value >= 'A' && value <= 'F' ? value + ('a' - 'A') : value;
  });
  return "sha256:" + hex;
}

std::string
ServiceController::abePublicParametersDigest(const ndn::Buffer& wire)
{
  ndn::util::Sha256 digest;
  digest << std::string(reinterpret_cast<const char*>(wire.data()), wire.size());
  return "sha256:" + digest.toString();
}

ndn::Name
ServiceController::currentAbePublicParametersName() const
{
  ndn::Name name = m_aaCert.getIdentity();
  name.append(ndn::nacabe::PUBLIC_PARAMS);
  name.append(ndn::nacabe::ABE_TYPE_KP_ABE);
  name.appendVersion(m_aa.getPublicParametersVersion());
  return name;
}

void
ServiceController::initializeAbeGenerationIdentity()
{
  if (!m_controllerVersion.isValid())
    throw std::runtime_error("cannot initialize ABE generation without ControllerVersion");

  // A single Controller generation can have multiple authorization epochs.
  // Use a deterministic, version-addressable numeric component for each
  // global ABE generation while leaving grant-only updates on the same value.
  const auto generation = m_controllerVersion.controllerGenerationTimestamp;
  const auto epoch = m_controllerVersion.controllerEpoch;
  if (generation > (std::numeric_limits<uint64_t>::max() - epoch) / 1000ULL)
    throw std::overflow_error("ABE public-parameter generation version overflow");
  m_aa.setPublicParametersVersion(generation * 1000ULL + epoch);
  m_abePublicParametersName = currentAbePublicParametersName();
  m_abePublicParametersDigest = abePublicParametersDigest(
      m_aa.getPublicParametersWire());
}

bool
ServiceController::isAbeAttributeRevoked(const std::string& identity,
                                          const std::string& attribute) const
{
  const ndn::Name identityName(identity);
  const ndn::Name attributeName(attribute);
  std::string identityCertificateDigest;
  try {
    const auto cert = m_keyChain.getPib().getIdentity(identityName)
        .getDefaultKey().getDefaultCertificate();
    identityCertificateDigest = certificateDigest(cert);
  }
  catch (const std::exception&) {
    // A missing local certificate cannot prove certificate-scoped authority;
    // retain the configured attributes until the target certificate is
    // resolved by the normal NAC-ABE trust path.
  }

  for (const auto& target : m_revocations) {
    if (target.kind == RevocationKind::IDENTITY &&
        target.targetIdentity == identityName)
      return true;
    if (target.kind == RevocationKind::CERTIFICATE &&
        !identityCertificateDigest.empty() &&
        sameCertificateDigest(target.certificateDigest, identityCertificateDigest) &&
        (target.targetIdentity.empty() || target.targetIdentity == identityName))
      return true;
    if (target.kind == RevocationKind::SERVICE_AUTHORIZATION &&
        target.targetIdentity == identityName &&
        target.authorizationAttribute == attributeName)
      return true;
  }
  return false;
}

std::set<std::string>
ServiceController::effectiveAttributesFor(const std::string& identity) const
{
  std::set<std::string> effective;
  const auto it = m_attributesMap.find(identity);
  if (it == m_attributesMap.end())
    return effective;
  for (const auto& attribute : it->second) {
    if (!isAbeAttributeRevoked(identity, attribute))
      effective.insert(attribute);
  }
  return effective;
}

void
ServiceController::reissueAbePolicies()
{
  // DKEYs are monolithic per identity in the current KP-ABE implementation.
  // Reissue the complete remaining attribute set, never an individual
  // attribute fragment, and remove identities with no remaining authority.
  for (const auto& item : m_attributesMap) {
    const auto effective = effectiveAttributesFor(item.first);
    const ndn::Name identity(item.first);
    m_aa.removePolicy(identity);
    if (effective.empty())
      continue;
    std::list<std::string> attributes(effective.begin(), effective.end());
    m_aa.replacePolicy(identity, boost::algorithm::join(attributes, " OR "));
  }
}

void
ServiceController::rotateAbeGenerationAndReissuePolicies()
{
  // Test-only fault injection: NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE=1
  // throws before any crypto work so the revocation-rotation failure path
  // (recording + reconcile retry) is executable in RV-U24.  Production never
  // sets this variable.
  const char* injectRotateFailure = std::getenv("NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE");
  if (injectRotateFailure != nullptr && *injectRotateFailure != '\0') {
    // A one-shot network fault leaves subsequent explicit retries runnable.
    if (std::string(injectRotateFailure) == "once")
      ::unsetenv("NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE");
    throw std::runtime_error("injected ABE rotation failure");
  }
  if (!m_controllerVersion.isValid())
    throw std::runtime_error("cannot rotate ABE generation without ControllerVersion");
  const auto generation = m_controllerVersion.controllerGenerationTimestamp;
  const auto epoch = m_controllerVersion.controllerEpoch;
  if (generation > (std::numeric_limits<uint64_t>::max() - epoch) / 1000ULL)
    throw std::overflow_error("ABE public-parameter generation version overflow");
  const auto publicParamsVersion = generation * 1000ULL + epoch;
  // Avoid rotating twice within one reserved epoch. A pending-recovery
  // attempt reserves a fresh epoch before reaching this function because
  // the previous attempt's status may already have been published.
  if (m_aa.getPublicParametersVersion() != publicParamsVersion) {
    m_aa.rotateKeyGeneration(publicParamsVersion);
  }
  m_abePublicParametersName = currentAbePublicParametersName();
  m_abePublicParametersDigest = abePublicParametersDigest(
      m_aa.getPublicParametersWire());
  reissueAbePolicies();
}

bool
ServiceController::reconcilePendingAbeRotation()
{
  if (!m_abeRotationPending)
    return true;
  try {
    // The failure epoch may already have been served with the previous ABE
    // identity. Replacing that identity at the same version would conflict
    // with immutable cached status and be rejected by every accepting peer.
    // Persist a new version before touching crypto on every recovery attempt.
    advanceAuthorizationEpoch();
    rotateAbeGenerationAndReissuePolicies();
  }
  catch (const std::exception& error) {
    NDN_LOG_ERROR("NDNSF_CONTROLLER_ABE_REKEY_RETRY_FAILED error=" << error.what());
    return false;
  }
  m_abeRotationPending = false;
  NDN_LOG_WARN("NDNSF_CONTROLLER_ABE_REKEY_RECONCILED generation="
               << m_controllerVersion.controllerGenerationTimestamp
               << " epoch=" << m_controllerVersion.controllerEpoch);
  return true;
}

bool
ServiceController::isRevoked(const ndn::Name& identity,
                              const ndn::Name& serviceName,
                              const std::string& certificateDigestValue,
                              const ndn::Name& authorizationAttribute) const
{
  for (const auto& target : m_revocations) {
    switch (target.kind) {
    case RevocationKind::IDENTITY:
      if (target.targetIdentity == identity)
        return true;
      break;
    case RevocationKind::CERTIFICATE:
      if (sameCertificateDigest(target.certificateDigest, certificateDigestValue) &&
          (target.targetIdentity.empty() || target.targetIdentity == identity))
        return true;
      break;
    case RevocationKind::SERVICE_AUTHORIZATION:
      if (target.targetIdentity == identity && target.serviceName == serviceName &&
          (authorizationAttribute.empty() ||
           target.authorizationAttribute == authorizationAttribute))
        return true;
      break;
    }
  }
  return false;
}

bool
ServiceController::isIdentityOrCertificateRevoked(
    const ndn::Name& identity, const std::string& certificateDigestValue) const
{
  return isRevoked(identity, ndn::Name(), certificateDigestValue);
}

void
ServiceController::advanceAuthorizationEpoch()
{
  if (!m_generationReady || !m_generationStore)
    throw std::runtime_error("Controller generation is not ready");
  m_controllerVersion = m_generationStore->advanceEpoch(m_revocations);
  m_policyEpoch = static_cast<size_t>(m_controllerVersion.controllerEpoch);
  m_requiredKeyEpoch = m_policyEpoch;
  m_policyValidFromMs = nowMilliseconds();
  m_policyValidUntilMs = m_policyValidFromMs + 24ULL * 60ULL * 60ULL * 1000ULL;
}

bool
ServiceController::revoke(const RevocationTarget& target)
{
  if (!m_generationReady || !target.isValid())
    return false;
  const bool recovering = m_abeRotationPending;
  // Reconcile before the duplicate-target check: callers retry the operation
  // that returned false, not an unrelated revocation invented to trigger it.
  if (!reconcilePendingAbeRotation())
    return false;
  for (const auto& existing : m_revocations) {
    if (existing.kind == target.kind &&
        existing.targetIdentity == target.targetIdentity &&
        existing.serviceName == target.serviceName &&
        sameCertificateDigest(existing.certificateDigest, target.certificateDigest) &&
        existing.authorizationAttribute == target.authorizationAttribute) {
      if (recovering) {
        NDN_LOG_WARN("NDNSF_CONTROLLER_REVOKED kind="
                     << static_cast<int>(target.kind)
                     << " identity=" << target.targetIdentity
                     << " service=" << target.serviceName
                     << " generation=" << m_controllerVersion.controllerGenerationTimestamp
                     << " epoch=" << m_controllerVersion.controllerEpoch
                     << " recovered=true");
      }
      return recovering;
    }
  }

  m_revocations.push_back(target);
  try {
    advanceAuthorizationEpoch();
  }
  catch (const std::exception& error) {
    // Durable commit did not complete, so restore the in-memory target and
    // leave the prior ControllerVersion authoritative.
    m_revocations.pop_back();
    NDN_LOG_ERROR("NDNSF_CONTROLLER_REVOKE_FAILED error=" << error.what());
    return false;
  }
  try {
    rotateAbeGenerationAndReissuePolicies();
  }
  catch (const std::exception& error) {
    NDN_LOG_ERROR("NDNSF_CONTROLLER_ABE_REKEY_FAILED error=" << error.what());
    // The generation store is monotonic; do not pretend an already-persisted
    // ControllerVersion can be rolled back.  Keep the revocation in memory so
    // a subsequent status publication cannot issue authority that the newly
    // advanced version has withdrawn, and record the pending rotation so the
    // next revoke entry retries it (reconcilePendingAbeRotation) instead of
    // serving the mixed state indefinitely.
    m_abeRotationPending = true;
    return false;
  }
  NDN_LOG_WARN("NDNSF_CONTROLLER_REVOKED kind="
               << static_cast<int>(target.kind)
               << " identity=" << target.targetIdentity
               << " service=" << target.serviceName
               << " generation=" << m_controllerVersion.controllerGenerationTimestamp
               << " epoch=" << m_controllerVersion.controllerEpoch);
  return true;
}

bool
ServiceController::grant(const ndn::Name& identity,
                          const ndn::Name& serviceName,
                          const ndn::Name& authorizationAttribute)
{
  RevocationTarget target;
  target.kind = RevocationKind::SERVICE_AUTHORIZATION;
  target.targetIdentity = identity;
  target.serviceName = serviceName;
  target.authorizationAttribute = authorizationAttribute;
  if (!target.isValid() || !m_generationReady)
    return false;
  // A grant following a failed withdrawal is not grant-only until the
  // withdrawal's rotation has completed. Never erase its target or issue a
  // replacement policy under the pre-withdrawal pair, even for another user.
  if (!reconcilePendingAbeRotation())
    return false;

  // An explicit grant can reauthorize an identity after an identity-wide
  // withdrawal, but never bypasses a certificate-wide withdrawal.  The
  // resulting DKEY is generated under the current post-withdrawal pair.
  bool reauthorizingIdentity = false;
  for (const auto& existing : m_revocations) {
    if (existing.targetIdentity != identity)
      continue;
    if (existing.kind == RevocationKind::CERTIFICATE)
      return false;
    if (existing.kind == RevocationKind::IDENTITY)
      reauthorizingIdentity = true;
  }

  const auto serviceUri = serviceName.toUri();
  const bool isUser = authorizationAttribute.getPrefix(1) == ndn::Name("/PERMISSION");
  const auto hasServiceGrant = [&] {
    if (isUser) {
      for (const auto& policy : m_userPolicies) {
        if (policy.userName == identity.toUri() &&
            std::find(policy.allowedServices.begin(), policy.allowedServices.end(), serviceUri) !=
                policy.allowedServices.end())
          return true;
      }
    }
    else {
      for (const auto& policy : m_providerPolicies) {
        if (policy.providerName == identity.toUri() &&
            std::find(policy.allowedServices.begin(), policy.allowedServices.end(), serviceUri) !=
                policy.allowedServices.end())
          return true;
      }
    }
    return false;
  };
  const auto existingAttributes = m_attributesMap.find(identity.toUri());
  const bool hasAttribute = existingAttributes != m_attributesMap.end() &&
                            existingAttributes->second.count(authorizationAttribute.toUri()) != 0;
  const bool hasMatchingWithdrawal = std::any_of(
      m_revocations.begin(), m_revocations.end(), [&target] (const RevocationTarget& existing) {
        return existing.kind == target.kind &&
               existing.targetIdentity == target.targetIdentity &&
               existing.serviceName == target.serviceName &&
               existing.authorizationAttribute == target.authorizationAttribute;
      });
  // An identity-wide withdrawal leaves the configured service/policy entries
  // in place but blocks their use. An explicit grant is the reauthorization
  // operation for that identity, so allow it to remove the identity target
  // even when the role/service attribute already exists. Certificate-wide
  // withdrawal is rejected above and cannot be bypassed by grant().
  if (hasServiceGrant() && hasAttribute && !hasMatchingWithdrawal &&
      !reauthorizingIdentity)
    return false;

  const auto oldUserPolicies = m_userPolicies;
  const auto oldProviderPolicies = m_providerPolicies;
  const auto oldAttributesMap = m_attributesMap;
  const auto oldRevocations = m_revocations;
  const auto oldVersion = m_controllerVersion;
  const auto oldPolicyEpoch = m_policyEpoch;
  const auto oldRequiredKeyEpoch = m_requiredKeyEpoch;
  const auto oldValidFrom = m_policyValidFromMs;
  const auto oldValidUntil = m_policyValidUntilMs;
  const auto restoreTargetPolicy = [&] {
    m_aa.removePolicy(identity);
    const auto effective = effectiveAttributesFor(identity.toUri());
    if (!effective.empty()) {
      std::list<std::string> attributes(effective.begin(), effective.end());
      m_aa.replacePolicy(identity, boost::algorithm::join(attributes, " OR "));
    }
  };
  bool foundIdentity = false;
  if (isUser) {
    for (auto& policy : m_userPolicies) {
      if (policy.userName != identity.toUri())
        continue;
      foundIdentity = true;
      if (std::find(policy.allowedServices.begin(), policy.allowedServices.end(), serviceUri) ==
          policy.allowedServices.end()) {
        policy.allowedServices.push_back(serviceUri);
      }
    }
    if (!foundIdentity)
      m_userPolicies.push_back(UserPolicy{identity.toUri(), {serviceUri}});
  }
  else {
    for (auto& policy : m_providerPolicies) {
      if (policy.providerName != identity.toUri())
        continue;
      foundIdentity = true;
      if (std::find(policy.allowedServices.begin(), policy.allowedServices.end(), serviceUri) ==
          policy.allowedServices.end()) {
        policy.allowedServices.push_back(serviceUri);
      }
    }
    if (!foundIdentity)
      m_providerPolicies.push_back(ProviderPolicy{identity.toUri(), {serviceUri}});
  }

  m_attributesMap[identity.toUri()].insert(authorizationAttribute.toUri());
  m_revocations.erase(std::remove_if(m_revocations.begin(), m_revocations.end(),
                                     [&target](const RevocationTarget& existing) {
    return (existing.kind == target.kind &&
            existing.targetIdentity == target.targetIdentity &&
            existing.serviceName == target.serviceName &&
            existing.authorizationAttribute == target.authorizationAttribute) ||
           (existing.kind == RevocationKind::IDENTITY &&
            target.targetIdentity == existing.targetIdentity);
  }), m_revocations.end());
  try {
    // Grant-only: retain the exact ABE pair and replace only this identity's
    // complete policy.  NAC-ABE generates its replacement DKEY lazily on the
    // target identity's next fetch; no global rotation or unaffected-identity
    // fan-out is performed.  Register the granted identity's certificate with
    // the Attribute Authority when the Controller keychain already knows it,
    // so the AA can serve the identity's lazy DKEY fetch without a network
    // certificate lookup; fall back to the name-only policy when no local
    // certificate exists yet.
    const auto effective = effectiveAttributesFor(identity.toUri());
    m_aa.removePolicy(identity);
    if (!effective.empty()) {
      std::list<std::string> attributes(effective.begin(), effective.end());
      const auto policy = boost::algorithm::join(attributes, " OR ");
      try {
        const auto cert = m_keyChain.getPib()
                            .getIdentity(identity)
                            .getDefaultKey()
                            .getDefaultCertificate();
        m_aa.addNewPolicy(cert, policy);
      }
      catch (const std::exception&) {
        m_aa.replacePolicy(identity, policy);
      }
    }
    buildLookupTables();
    advanceAuthorizationEpoch();
  }
  catch (const std::exception& error) {
    m_userPolicies = oldUserPolicies;
    m_providerPolicies = oldProviderPolicies;
    m_attributesMap = oldAttributesMap;
    m_revocations = oldRevocations;
    m_controllerVersion = oldVersion;
    m_policyEpoch = oldPolicyEpoch;
    m_requiredKeyEpoch = oldRequiredKeyEpoch;
    m_policyValidFromMs = oldValidFrom;
    m_policyValidUntilMs = oldValidUntil;
    buildLookupTables();
    try {
      restoreTargetPolicy();
    }
    catch (const std::exception& restoreError) {
      NDN_LOG_ERROR("NDNSF_CONTROLLER_GRANT_ROLLBACK_FAILED error="
                    << restoreError.what());
    }
    NDN_LOG_ERROR("NDNSF_CONTROLLER_GRANT_FAILED error=" << error.what());
    return false;
  }
  NDN_LOG_INFO("NDNSF_CONTROLLER_GRANT identity=" << identity
               << " service=" << serviceName
               << " attribute=" << authorizationAttribute
               << " generation=" << m_controllerVersion.controllerGenerationTimestamp
               << " epoch=" << m_controllerVersion.controllerEpoch
               << " reauthorizingIdentity=" << (reauthorizingIdentity ? "true" : "false")
               << " abeParametersUnchanged=true");
  return true;
}

PolicyStatusData
ServiceController::getPolicyStatus(const ndn::Name& serviceName) const
{
  if (!m_generationReady || !m_controllerVersion.isValid() || serviceName.empty())
    throw std::runtime_error("Controller policy status is not ready");
  PolicyStatusData status;
  status.setServiceName(serviceName);
  status.setControllerVersion(m_controllerVersion);
  status.setValidity(m_policyValidFromMs, m_policyValidUntilMs);
  status.setPolicyDigest(sha256FileDigest(m_configFilePath));
  status.setAbePublicParametersName(m_abePublicParametersName);
  status.setAbePublicParametersDigest(m_abePublicParametersDigest);
  status.setControllerCertificate(m_aaCert.getName());
  for (const auto& target : m_revocations) {
    if (target.kind != RevocationKind::SERVICE_AUTHORIZATION ||
        target.serviceName == serviceName)
      status.addRevocation(target);
  }
  return status;
}

void ServiceController::setControllerPrefix(const ndn::Name& prefix)
{
  if (m_isRegistered) {
    throw std::logic_error("Cannot change ServiceController prefix after start()");
  }

  m_controllerPrefix = prefix;
  m_hasCustomControllerPrefix = true;
}

void
ServiceController::setBootstrapTokenFile(const std::string& path)
{
  if (m_isRegistered) {
    throw std::logic_error("Cannot change bootstrap token file after start()");
  }
  loadBootstrapTokenFile(path);
}

void
ServiceController::cancelStart() noexcept
{
  m_startCancelled.store(true, std::memory_order_release);
}

void
ServiceController::resetStartCancellation() noexcept
{
  m_startCancelled.store(false, std::memory_order_release);
}

void ServiceController::start()
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  if (m_startCancelled.load(std::memory_order_acquire)) {
    throw std::runtime_error(
      "ServiceController AttributeAuthority PUBPARAMS readiness cancelled");
  }

  registerInterestHandlers();

  // A second Face has its own transport, PIT and event loop.
  // Never express the readiness Interest on the AA Face: ndn-cxx could satisfy
  // it in-process even when no Interest/response crosses NFD. Reuse ndn-cxx's
  // configured endpoint, not a hard-coded socket or the AA transport object.
  auto& io = m_face.getIoContext();
  // Keep probe callbacks on a private context: destroying a temporary Face on
  // the long-lived AA context could leave ndn-cxx transport callbacks queued
  // there after the temporary Face is gone.
  boost::asio::io_context probeIo;
  ndn::Face probeFace(nullptr, probeIo, m_keyChain);
  struct ReadinessProbeState {
    bool receivedByAuthority = false;
    bool challengeReady = false;
    bool ready = false;
    bool finished = false;
    bool inFlight = false;
    std::string error;
  };
  const auto state = std::make_shared<ReadinessProbeState>();
  // NAC-ABE names parameters by their actual generation. Keep the fresh
  // liveness challenge separate; never relabel those bytes with a nonce.
  const auto paramsName = currentAbePublicParametersName();
  const auto paramsWire = m_aa.getPublicParametersWire();
  const auto challengeContent = paramsName.toUri() + "|" +
    abePublicParametersDigest(paramsWire);
  const auto probeName = ndn::Name(m_aaCert.getIdentity()).append("NDNSF")
    .append("READINESS").appendNumber(ndn::random::generateSecureWord64())
    .appendNumber(ndn::random::generateSecureWord64());
  ndn::ScopedInterestFilterHandle observer = m_face.setInterestFilter(probeName,
    [this, state, probeName, challengeContent](const ndn::InterestFilter&,
                                             const ndn::Interest& interest) {
      // NFD decrements 1 to 0 on ingress and only forwards the result to local
      // faces. Thus both Faces must reach the same local forwarder. If a caller
      // supplied a different endpoint, fail closed instead of probing another NFD.
      if (!state->finished && interest.getName() == probeName) {
        state->receivedByAuthority = interest.getHopLimit() == 0;
        ndn::Data reply(probeName);
        reply.setContent(challengeContent);
        reply.setFreshnessPeriod(ndn::time::milliseconds(0));
        m_keyChain.sign(reply, ndn::security::signingByCertificate(m_aaCert));
        m_face.put(reply);
      }
    });
  ndn::ScopedPendingInterestHandle pending;
  auto finish = ndn::make_scope_exit([state] { state->finished = true; });

  // Only restart on entry. A concurrent stop() must not be undone in the loop.
  // A work guard makes each idle run_for block; Nacks have a retry floor too.
  io.restart();
  auto work = boost::asio::make_work_guard(io);
  auto probeWork = boost::asio::make_work_guard(probeIo);
  auto nextAttempt = std::chrono::steady_clock::now();
  while (true) {
    if (m_startCancelled.load(std::memory_order_acquire)) {
      throw std::runtime_error(
        "ServiceController AttributeAuthority PUBPARAMS readiness cancelled");
    }
    if (io.stopped() || probeIo.stopped()) {
      throw std::runtime_error(
        "ServiceController AttributeAuthority PUBPARAMS readiness event loop stopped");
    }
    if (!m_registrationState->error.empty()) {
      throw std::runtime_error("ServiceController prefix registration failed: " +
                               m_registrationState->error);
    }
    if (!state->error.empty()) {
      throw std::runtime_error(state->error);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw std::runtime_error(
        "ServiceController AttributeAuthority PUBPARAMS readiness timeout");
    }
    if (state->ready && m_registrationState->pending == 0) {
      return;
    }
    if (!state->ready && !state->inFlight && now >= nextAttempt) {
      state->inFlight = true;
      nextAttempt = now + std::chrono::milliseconds(100);
      const bool parameters = state->challengeReady;
      ndn::Interest interest(parameters ? paramsName : probeName);
      interest.setMustBeFresh(true);
      interest.setCanBePrefix(!parameters);
      interest.setHopLimit(1);
      interest.setInterestLifetime(ndn::time::milliseconds(250));
      pending = probeFace.expressInterest(
        interest,
        [state, cert = m_aaCert, parameters, paramsName, paramsWire,
         probeName, challengeContent](const ndn::Interest&, const ndn::Data& data) {
          if (!state->finished) {
            state->inFlight = false;
            if (!state->receivedByAuthority) {
              state->error = "ServiceController PUBPARAMS probe did not reach the authority Face";
            }
            else if (data.getContent().value_size() == 0 ||
                      !ndn::security::verifySignature(data, cert)) {
              state->error = "ServiceController PUBPARAMS probe returned invalid authority Data";
            }
            else if (parameters) {
              const auto& content = data.getContent();
              if (data.getName() != paramsName ||
                  content.value_size() != paramsWire.size() ||
                  !std::equal(content.value_begin(), content.value_end(), paramsWire.begin())) {
                state->error = "ServiceController PUBPARAMS probe returned invalid current parameters";
              }
              else {
                state->ready = true;
              }
            }
            else {
              const auto& content = data.getContent();
              const std::string text(reinterpret_cast<const char*>(content.value()),
                                     content.value_size());
              if (data.getName() != probeName || text != challengeContent) {
                state->error = "ServiceController PUBPARAMS probe returned invalid authority Data";
              }
              else {
                state->challengeReady = true;
              }
            }
          }
        },
        [state](const ndn::Interest&, const ndn::lp::Nack&) {
          state->inFlight = false;
        },
        [state](const ndn::Interest&) {
          state->inFlight = false;
        });
    }
    // Both contexts run on this thread; cancellation is checked at least once
    // per two 25 ms slices, without sleeps or busy polling an empty context.
    const auto slice = std::chrono::steady_clock::duration(std::chrono::milliseconds(25));
    io.run_for(std::min(deadline - now, slice));
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining > std::chrono::steady_clock::duration::zero() &&
        !m_startCancelled.load(std::memory_order_acquire) && !io.stopped()) {
      probeIo.run_for(std::min(remaining, slice));
    }
  }
}

void ServiceController::run()
{
  start();
  while (true) {
    m_face.getIoContext().restart();
    m_face.processEvents(ndn::time::milliseconds(1000));
  }
}

void ServiceController::loadConfigFiles()
{
  if (fs::is_regular_file(m_configFilePath)) {
    NDN_LOG_INFO("Loading config file: " << m_configFilePath);

    PolicyParser parser;
    auto policies = parser.parsePolicyFile(m_configFilePath);

    m_providerPolicies.insert(m_providerPolicies.end(),
                              policies.first.begin(), policies.first.end());
    m_userPolicies.insert(m_userPolicies.end(),
                          policies.second.begin(), policies.second.end());
  }
  else {
    NDN_LOG_ERROR("Error: " << m_configFilePath << " is not a valid file.");
  }
}

void ServiceController::addAttribute(const std::string& identity, const std::string& attributeName)
{
  m_attributesMap[identity].emplace(attributeName);
}

void ServiceController::addAttributesForUsersAccordingToServicePolicy()
{
  // Provider policies -> SERVICE/<service-name> attributes
  for (const auto& policy : m_providerPolicies) {
    for (const auto& service : policy.allowedServices) {
      addAttribute(policy.providerName, "/SERVICE" + service);
    }
  }

  // User policies -> PERMISSION/<service-name> attributes
  for (const auto& policy : m_userPolicies) {
    ndn::Name userName(policy.userName);
    for (const auto& service : policy.allowedServices) {
      addAttribute(userName.toUri(), "/PERMISSION" + service);
    }
  }

  // Register each identity's OR-policy into the Attribute Authority.
  for (const auto& item : m_attributesMap) {
    const std::string& identity = item.first;
    const std::set<std::string>& attrs = item.second;

    std::list<std::string> attributeList(attrs.begin(), attrs.end());
    std::string abePolicy = boost::algorithm::join(attributeList, " OR ");

    try {
      auto cert = m_keyChain.getPib()
                    .getIdentity(identity)
                    .getDefaultKey()
                    .getDefaultCertificate();
      m_aa.addNewPolicy(cert, abePolicy);
      NDN_LOG_INFO("Add ABE policy: " << abePolicy
                << " for identity (cert): " << identity);
    }
    catch (const std::exception&) {
      m_aa.addNewPolicy(ndn::Name(identity), abePolicy);
      NDN_LOG_INFO("Add ABE policy (fallback): " << abePolicy
                << " for identity: " << identity);
    }
  }
}

std::vector<std::string> ServiceController::uniqSorted(std::vector<std::string> v)
{
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

void ServiceController::buildLookupTables()
{
  m_userAllowedServices.clear();
  m_providerAllowedServices.clear();

  for (const auto& p : m_userPolicies) {
    auto& vec = m_userAllowedServices[p.userName];
    vec.insert(vec.end(), p.allowedServices.begin(), p.allowedServices.end());
  }

  for (const auto& p : m_providerPolicies) {
    auto& vec = m_providerAllowedServices[p.providerName];
    vec.insert(vec.end(), p.allowedServices.begin(), p.allowedServices.end());
  }

  for (auto& it : m_userAllowedServices) {
    it.second = uniqSorted(std::move(it.second));
  }
  for (auto& it : m_providerAllowedServices) {
    it.second = uniqSorted(std::move(it.second));
  }
}

bool ServiceController::extractEntityAfterPrefix(const ndn::Name& interestName,
                                                const ndn::Name& prefix,
                                                ndn::Name& entityOut)
{
  if (!prefix.isPrefixOf(interestName)) {
    return false;
  }
  if (interestName.size() <= prefix.size()) {
    return false;
  }
  entityOut = stripTrailingParametersDigest(interestName.getSubName(prefix.size()));
  return true;
}

bool ServiceController::parseUserPermissionsInterestName(const ndn::Name& interestName,
                                                         ndn::Name& targetIdentity) const
{
  if (!m_prefixUserPermissions.isPrefixOf(interestName) ||
      interestName.size() <= m_prefixUserPermissions.size()) {
    return false;
  }

  targetIdentity = stripTrailingParametersDigest(
    interestName.getSubName(m_prefixUserPermissions.size()));
  return true;
}

bool ServiceController::parseProviderPermissionsInterestName(const ndn::Name& interestName,
                                                             ndn::Name& targetIdentity) const
{
  if (!m_prefixProviderPermissions.isPrefixOf(interestName) ||
      interestName.size() <= m_prefixProviderPermissions.size()) {
    return false;
  }

  targetIdentity = stripTrailingParametersDigest(
    interestName.getSubName(m_prefixProviderPermissions.size()));
  return true;
}

void ServiceController::registerInterestHandlers()
{
  if (m_isRegistered) {
    return;
  }

  if (!m_hasCustomControllerPrefix) {
    m_controllerPrefix = m_aaCert.getIdentity();
  }

  m_prefixServiceAccess = m_controllerPrefix;
  m_prefixServiceAccess.append("NDNSF").append("SERVICEACCESS");

  m_prefixServiceProvision = m_controllerPrefix;
  m_prefixServiceProvision.append("NDNSF").append("SERVICEPROVISION");

  m_prefixUserPermissions = m_controllerPrefix;
  m_prefixUserPermissions.append("NDNSF").append("PERMISSIONS").append("USER");

  m_prefixProviderPermissions = m_controllerPrefix;
  m_prefixProviderPermissions.append("NDNSF").append("PERMISSIONS").append("PROVIDER");

  m_prefixPolicyManifest = m_controllerPrefix;
  m_prefixPolicyManifest.append("NDNSF").append("POLICY-MANIFEST");

  m_prefixPolicyStatus = m_controllerPrefix;
  m_prefixPolicyStatus.append("NDNSF").append("POLICY-STATUS");

  m_prefixCertificateBootstrap = m_controllerPrefix;
  m_prefixCertificateBootstrap.append("NDNSF").append("CERTBOOTSTRAP");

  m_registrationState = std::make_shared<RegistrationState>();
  const auto onRegistered = [state = m_registrationState](const ndn::Name&) {
    --state->pending;
  };
  const auto onFailure = [state = m_registrationState](const ndn::Name& prefix,
                                                      const std::string& reason) {
    state->error = prefix.toUri() + ": " + reason;
    NDN_LOG_ERROR("Failed to register prefix " << prefix << " reason=" << reason);
  };

  auto registerFilter = [this, onRegistered, onFailure](
      const ndn::Name& prefix,
      std::function<void(const ndn::InterestFilter&, const ndn::Interest&)> onInterest) {
    auto holder = std::make_shared<ndn::ScopedRegisteredPrefixHandle>();
    m_registrationHandles.push_back(holder);
    *holder = m_face.setInterestFilter(
      prefix, std::move(onInterest), onRegistered, onFailure);
  };

  registerFilter(
    m_prefixServiceAccess,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onServiceAccessInterest(f, i);
    });

  registerFilter(
    m_prefixServiceProvision,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onServiceProvisionInterest(f, i);
    });

  registerFilter(
    m_prefixUserPermissions,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onUserPermissionsInterest(f, i);
    });

  registerFilter(
    m_prefixProviderPermissions,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onProviderPermissionsInterest(f, i);
    });

  registerFilter(
    m_prefixPolicyManifest,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onPolicyManifestInterest(f, i);
    });

  registerFilter(
    m_prefixPolicyStatus,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onPolicyStatusInterest(f, i);
    });

  registerFilter(
    m_prefixCertificateBootstrap,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onCertificateBootstrapInterest(f, i);
    });

  NDN_LOG_INFO("ServiceController listening on:\n"
            << "  " << m_prefixServiceAccess 
            << "  " << m_prefixServiceProvision 
            << "  " << m_prefixUserPermissions 
            << "  " << m_prefixProviderPermissions
            << "  " << m_prefixPolicyManifest
            << "  " << m_prefixPolicyStatus
            << "  " << m_prefixCertificateBootstrap);

  m_isRegistered = true;
}

ndn::Block ServiceController::makeAllowedServiceListTlv(const std::vector<std::string>& services) const
{
  ndn::Block list(tlv::AllowedServiceListType);

  for (const auto& s : services) {
    ndn::Name svcName(s);
    ndn::Block nameBlock = svcName.wireEncode();

    ndn::Block item(tlv::AllowedServiceType);
    item.push_back(nameBlock);
    item.encode();

    list.push_back(item);
  }

  list.encode();
  return list;
}

PermissionResponse
ServiceController::buildUserPermissionResponse(const ndn::Name& targetIdentity) const
{
  PermissionResponse response;
  response.setTargetIdentity(targetIdentity.toUri());
  response.setPermissionKind(tlv::UserPermission);
  response.setPolicyEpoch(m_policyEpoch);
  if (m_controllerVersion.isValid())
    response.setControllerVersion(m_controllerVersion);

  std::vector<std::string> allowedServices;
  if (auto it = m_userAllowedServices.find(targetIdentity.toUri());
      it != m_userAllowedServices.end()) {
    allowedServices = it->second;
  }

  for (const auto& service : allowedServices) {
    for (const auto& provider : m_providerAllowedServices) {
      const auto& providerName = provider.first;
      const auto& providerServices = provider.second;
      if (std::find(providerServices.begin(), providerServices.end(), service) ==
          providerServices.end()) {
        continue;
      }

      if (isRevoked(targetIdentity, ndn::Name(service), {},
                    makeAuthorizationAttribute("/PERMISSION", service)))
        continue;

      PermissionEntry entry;
      entry.setProviderName(providerName);
      entry.setServiceName(service);
      // Deprecated wire-compatibility field. Invocation authentication uses
      // per-request UserToken and per-ACK ProviderToken, not controller tokens.
      entry.setToken("");
      entry.setTtl(0);
      entry.setVersion(m_policyEpoch);
      response.addEntry(entry);
    }
  }

  return response;
}

PermissionResponse
ServiceController::buildProviderPermissionResponse(const ndn::Name& targetIdentity) const
{
  PermissionResponse response;
  response.setTargetIdentity(targetIdentity.toUri());
  response.setPermissionKind(tlv::ProviderPermission);
  response.setPolicyEpoch(m_policyEpoch);
  if (m_controllerVersion.isValid())
    response.setControllerVersion(m_controllerVersion);

  std::vector<std::string> allowedServices;
  if (auto it = m_providerAllowedServices.find(targetIdentity.toUri());
      it != m_providerAllowedServices.end()) {
    allowedServices = it->second;
  }

  for (const auto& service : allowedServices) {
    if (isRevoked(targetIdentity, ndn::Name(service), {},
                  makeAuthorizationAttribute("/SERVICE", service)))
      continue;
    PermissionEntry entry;
    entry.setProviderName(targetIdentity.toUri());
    entry.setServiceName(service);
    // Deprecated wire-compatibility field. Provider permission presence is
    // still installed, but no invocation token is issued by the controller.
    entry.setToken("");
    entry.setTtl(0);
    entry.setVersion(m_policyEpoch);
    response.addEntry(entry);
  }

  return response;
}

PolicyManifest
ServiceController::buildPolicyManifest() const
{
  PolicyManifest manifest;
  manifest.setPolicyEpoch(m_policyEpoch);
  manifest.setValidFromMs(m_policyValidFromMs);
  manifest.setGracePeriodMs(m_policyGracePeriodMs);
  manifest.setRequiredKeyEpoch(m_requiredKeyEpoch);
  if (m_controllerVersion.isValid())
    manifest.setControllerVersion(m_controllerVersion);
  return manifest;
}

ndn::security::Certificate
ServiceController::getTargetIdentityCertificate(const ndn::Name& targetIdentity)
{
  if (auto issued = m_bootstrapIssuedCertificates.find(targetIdentity.toUri());
      issued != m_bootstrapIssuedCertificates.end()) {
    // Permission responses are encrypted with the identity's RSA key even
    // when the runtime uses a separate ECDSA signing certificate.  Bootstrap
    // may have issued either certificate type, so normalize it to RSA here.
    return getRsaEncryptionCertificateOrThrow(m_keyChain, issued->second);
  }

  auto cert = m_keyChain.getPib()
                .getIdentity(targetIdentity)
                .getDefaultKey()
                .getDefaultCertificate();
  if (!cert.isValid()) {
    throw std::runtime_error("Target identity certificate is not currently valid: " +
                             cert.getName().toUri());
  }
  // The default key may be ECDSA because it is preferred for Data signing;
  // NAC-ABE permission encryption still requires the RSA certificate.
  return getRsaEncryptionCertificateOrThrow(m_keyChain, cert);
}

void
ServiceController::generateBootstrapTokenFile(const std::string& path) const
{
  std::map<std::string, std::string> entries;
  for (const auto& policy : m_providerPolicies) {
    entries.emplace(ndn::Name(policy.providerName).toUri(), "provider");
  }
  for (const auto& policy : m_userPolicies) {
    entries.emplace(ndn::Name(policy.userName).toUri(), "user");
  }

  if (entries.empty()) {
    throw std::runtime_error("Cannot generate bootstrap token file without policy identities: " +
                             path);
  }

  const fs::path outputPath(path);
  if (outputPath.has_parent_path()) {
    fs::create_directories(outputPath.parent_path());
  }

  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Cannot create bootstrap token file: " + path);
  }

  output << "# identity token role\n";
  for (const auto& entry : entries) {
    output << entry.first << ' ' << makeBootstrapToken(8) << ' ' << entry.second << '\n';
  }
  output.close();
  if (!output) {
    throw std::runtime_error("Cannot finish writing bootstrap token file: " + path);
  }

  fs::permissions(outputPath,
                  fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace);

  NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_TOKEN_FILE_GENERATED path=" << path
               << " entries=" << entries.size());
}

void
ServiceController::loadBootstrapTokenFile(const std::string& path)
{
  if (path.empty()) {
    return;
  }

  std::ifstream input(path);
  if (!input) {
    if (!fs::exists(path)) {
      generateBootstrapTokenFile(path);
      input.open(path);
    }
    if (!input) {
      throw std::runtime_error("Cannot open bootstrap token file: " + path);
    }
  }

  std::string line;
  size_t lineNo = 0;
  while (std::getline(input, line)) {
    ++lineNo;
    const auto commentPos = line.find('#');
    if (commentPos != std::string::npos) {
      line = line.substr(0, commentPos);
    }

    std::istringstream parser(line);
    std::string identity;
    std::string token;
    std::string role;
    if (!(parser >> identity >> token)) {
      continue;
    }
    parser >> role;

    try {
      ndn::Name identityName(identity);
      if (identityName.empty() || token.empty()) {
        throw std::runtime_error("empty identity or token");
      }
      m_bootstrapTokens[identityName.toUri()] = BootstrapTokenEntry{token, role};
    }
    catch (const std::exception& e) {
      throw std::runtime_error("Invalid bootstrap token file entry " + path +
                               ":" + std::to_string(lineNo) + ": " + e.what());
    }
  }

  NDN_LOG_INFO("NDNSF_CERT_BOOTSTRAP_TOKEN_FILE path=" << path
               << " entries=" << m_bootstrapTokens.size());
}

void
ServiceController::sendCertificateBootstrapResponse(const ndn::Interest& interest,
                                                    bool status,
                                                    const std::string& message,
                                                    const ndn::security::Certificate* issuedCertificate)
{
  CertificateBootstrapResponse response;
  response.status = status;
  response.message = message;
  if (issuedCertificate != nullptr) {
    response.issuedCertificate = *issuedCertificate;
    response.hasIssuedCertificate = true;
  }

  ndn::Data data(interest.getName());
  data.setFreshnessPeriod(ndn::time::milliseconds(0));
  data.setContent(response.wireEncode());
  m_keyChain.sign(data, ndn::security::SigningInfo(
    ndn::security::SigningInfo::SIGNER_TYPE_ID, m_controllerPrefix));
  m_face.put(data);
}

// ===================== Signer cert extraction =====================

bool
ServiceController::identitiesMatch(const ndn::Name& lhs, const ndn::Name& rhs) const
{
  return lhs == rhs;
}

ndn::Name ServiceController::getSignerCertNameFromInterest(const ndn::Interest& interest) const
{
  const auto sigInfo = interest.getSignatureInfo();
  if (!sigInfo) {
    throw std::runtime_error("Interest is not signed");
  }

  if (!sigInfo->hasKeyLocator()) {
    throw std::runtime_error("Interest SignatureInfo has no KeyLocator");
  }

  const auto& kl = sigInfo->getKeyLocator();

  if (kl.getType() != ndn::tlv::Name) {
    throw std::runtime_error("Interest KeyLocator is not a Name");
  }

  const ndn::Name certName = kl.getName();
  if (!ndn::security::Certificate::isValidName(certName)) {
    throw std::runtime_error("Interest KeyLocator Name is not a certificate name: " +
                             certName.toUri());
  }

  return certName;
}

ndn::security::Certificate
ServiceController::getSignerCertificateFromInterest(const ndn::Interest& interest) const
{
  const ndn::Name certName = getSignerCertNameFromInterest(interest);

  try {
    const ndn::Name identityName = ndn::security::extractIdentityFromCertName(certName);
    const ndn::Name keyName = ndn::security::extractKeyNameFromCertName(certName);
    return m_keyChain.getPib()
      .getIdentity(identityName)
      .getKey(keyName)
      .getCertificate(certName);
  }
  catch (const std::exception&) {
    throw std::runtime_error("Cannot find signer certificate in PIB: " + certName.toUri());
  }
}

ndn::Name
ServiceController::getSignerIdentityFromInterest(const ndn::Interest& interest) const
{
  const auto signerCert = getSignerCertificateFromInterest(interest);
  return ndn::security::extractIdentityFromCertName(signerCert.getName());
}

// ===================== Encryption =====================

ndn::Block
ServiceController::encryptForCertificate(const ndn::security::Certificate& cert,
                                        const ndn::Block& plaintext) const
{
  ndn::Block pt = plaintext;
  pt.encode();
  return encryptWireBytesWithContentKeyForCertificate(
    cert, ndn::span<const uint8_t>(pt.data(), pt.size()));
}

// ===================== Handlers =====================

void ServiceController::onServiceAccessInterest(const ndn::InterestFilter&,
                                               const ndn::Interest& interest)
{
  // <controller>/NDNSF/SERVICEACCESS/<user>
  ndn::Name userName;
  if (!extractEntityAfterPrefix(interest.getName(), m_prefixServiceAccess, userName)) {
    return;
  }

  // 先验证 Interest（必须！否则 KeyLocator 可被伪造导致加密给错误的人）
  m_validator.validate(
    interest,
    // validated
    [this, userName](const ndn::Interest& validatedInterest) {
      ndn::Name signerIdentity;
      ndn::security::Certificate signerCert;
      try {
        signerIdentity = getSignerIdentityFromInterest(validatedInterest);
        signerCert = getSignerCertificateFromInterest(validatedInterest);
      }
      catch (const std::exception& e) {
        NDN_LOG_ERROR("[SERVICEACCESS] authorization failed: requested="
                  << userName
                  << " signer=<unknown>"
                  << " error=" << e.what());
        return;
      }

      if (!identitiesMatch(signerIdentity, userName)) {
        NDN_LOG_ERROR("[SERVICEACCESS] authorization failed: requested="
                  << userName
                  << " signer=" << signerIdentity);
        return;
      }

      const std::string userUri = userName.toUri();

      // 查权限（你现在按 name 里的 userUri 查）
      std::vector<std::string> services;
      if (auto it = m_userAllowedServices.find(userUri); it != m_userAllowedServices.end()) {
        services = it->second;
      }

      // Data name = prefix + user + timestamp
      ndn::Name dataName = validatedInterest.getName();
      dataName.appendTimestamp(ndn::time::system_clock::now());

      ndn::Data data(dataName);

      ndn::Block plaintext = makeAllowedServiceListTlv(services);
      ndn::Block content   = encryptForCertificate(signerCert, plaintext);

      data.setContent(content);
      data.setFreshnessPeriod(ndn::time::seconds(2));
      m_keyChain.sign(data);
      m_face.put(data);

      NDN_LOG_INFO("[SERVICEACCESS] Reply to " << userUri
                << " services=" << services.size()
                << " data=" << data.getName()
                << " encryptedFor=" << signerCert.getName());
    },
    // failed
    [](const ndn::Interest& badInterest, const ndn::security::ValidationError& err) {
      NDN_LOG_ERROR("[SERVICEACCESS] Interest validation failed: "
                << err << " name=" << badInterest.getName());
    });
}

void ServiceController::onServiceProvisionInterest(const ndn::InterestFilter&,
                                                  const ndn::Interest& interest)
{
  // <controller>/NDNSF/SERVICEPROVISION/<provider>
  ndn::Name providerName;
  if (!extractEntityAfterPrefix(interest.getName(), m_prefixServiceProvision, providerName)) {
    return;
  }

  m_validator.validate(
    interest,
    // validated
    [this, providerName](const ndn::Interest& validatedInterest) {
      ndn::Name signerIdentity;
      ndn::security::Certificate signerCert;
      try {
        signerIdentity = getSignerIdentityFromInterest(validatedInterest);
        signerCert = getSignerCertificateFromInterest(validatedInterest);
      }
      catch (const std::exception& e) {
        NDN_LOG_ERROR("[SERVICEPROVISION] authorization failed: requested="
                  << providerName
                  << " signer=<unknown>"
                  << " error=" << e.what());
        return;
      }

      if (!identitiesMatch(signerIdentity, providerName)) {
        NDN_LOG_ERROR("[SERVICEPROVISION] authorization failed: requested="
                  << providerName
                  << " signer=" << signerIdentity);
        return;
      }

      const std::string providerUri = providerName.toUri();

      std::vector<std::string> services;
      if (auto it = m_providerAllowedServices.find(providerUri); it != m_providerAllowedServices.end()) {
        services = it->second;
      }

      ndn::Name dataName = validatedInterest.getName();
      dataName.appendTimestamp(ndn::time::system_clock::now());

      ndn::Data data(dataName);

      ndn::Block plaintext = makeAllowedServiceListTlv(services);
      ndn::Block content   = encryptForCertificate(signerCert, plaintext);

      data.setContent(content);
      data.setFreshnessPeriod(ndn::time::seconds(2));
      m_keyChain.sign(data);
      m_face.put(data);

      NDN_LOG_INFO("[SERVICEPROVISION] Reply to " << providerUri
                << " services=" << services.size()
                << " data=" << data.getName()
                << " encryptedFor=" << signerCert.getName());
    },
    // failed
    [](const ndn::Interest& badInterest, const ndn::security::ValidationError& err) {
      NDN_LOG_ERROR("[SERVICEPROVISION] Interest validation failed: "
                << err << " name=" << badInterest.getName());
    });
}

void ServiceController::onUserPermissionsInterest(const ndn::InterestFilter&,
                                                  const ndn::Interest& interest)
{
  ndn::Name targetIdentity;
  if (!parseUserPermissionsInterestName(interest.getName(), targetIdentity)) {
    return;
  }

  if (!m_generationReady) {
    NDN_LOG_ERROR("[PERMISSIONS/USER] Refusing protected issuance because "
                  "Controller generation is unavailable target="
                  << targetIdentity);
    return;
  }

  EncryptedPermissionResponse encryptedResponse;
  try {
    const auto targetCert = getTargetIdentityCertificate(targetIdentity);
    PermissionResponse response = buildUserPermissionResponse(targetIdentity);
    if (isIdentityOrCertificateRevoked(targetIdentity, certificateDigest(targetCert))) {
      response.setEntries({});
      NDN_LOG_WARN("[PERMISSIONS/USER] Revoked identity receives empty current-version "
                   "response target=" << targetIdentity);
    }
    encryptedResponse = encryptPermissionResponseForCertificate(response, targetCert);

    ndn::Name dataName = interest.getName();
    dataName.appendTimestamp(ndn::time::system_clock::now());

    ndn::Data data(dataName);
    data.setContent(encryptedResponse.WireEncode());
    data.setFreshnessPeriod(ndn::time::milliseconds(0));
    m_keyChain.sign(data, ndn::security::SigningInfo(
      ndn::security::SigningInfo::SIGNER_TYPE_ID, m_controllerPrefix));
    m_face.put(data);

    NDN_LOG_INFO("[PERMISSIONS/USER] Encrypted reply target="
              << targetIdentity
              << " data=" << data.getName()
              << " payload=" << encryptedResponse.toString());
  }
  catch (const std::exception& e) {
    NDN_LOG_ERROR("[PERMISSIONS/USER] Refusing to reply without encrypting for target="
              << targetIdentity.toUri()
              << " error=" << e.what());
    return;
  }

}

void ServiceController::onProviderPermissionsInterest(const ndn::InterestFilter&,
                                                      const ndn::Interest& interest)
{
  ndn::Name targetIdentity;
  if (!parseProviderPermissionsInterestName(interest.getName(), targetIdentity)) {
    return;
  }

  if (!m_generationReady) {
    NDN_LOG_ERROR("[PERMISSIONS/PROVIDER] Refusing protected issuance because "
                  "Controller generation is unavailable target="
                  << targetIdentity);
    return;
  }

  EncryptedPermissionResponse encryptedResponse;
  try {
    const auto targetCert = getTargetIdentityCertificate(targetIdentity);
    PermissionResponse response = buildProviderPermissionResponse(targetIdentity);
    if (isIdentityOrCertificateRevoked(targetIdentity, certificateDigest(targetCert))) {
      response.setEntries({});
      NDN_LOG_WARN("[PERMISSIONS/PROVIDER] Revoked identity receives empty "
                   "current-version response target=" << targetIdentity);
    }
    encryptedResponse = encryptPermissionResponseForCertificate(response, targetCert);

    ndn::Name dataName = interest.getName();
    dataName.appendTimestamp(ndn::time::system_clock::now());

    ndn::Data data(dataName);
    data.setContent(encryptedResponse.WireEncode());
    data.setFreshnessPeriod(ndn::time::milliseconds(0));
    m_keyChain.sign(data, ndn::security::SigningInfo(
      ndn::security::SigningInfo::SIGNER_TYPE_ID, m_controllerPrefix));
    m_face.put(data);

    NDN_LOG_INFO("[PERMISSIONS/PROVIDER] Encrypted reply target="
              << targetIdentity
              << " data=" << data.getName()
              << " payload=" << encryptedResponse.toString());
  }
  catch (const std::exception& e) {
    NDN_LOG_ERROR("[PERMISSIONS/PROVIDER] Refusing to reply without encrypting for target="
              << targetIdentity.toUri()
              << " error=" << e.what());
    return;
  }

}

void ServiceController::onPolicyManifestInterest(const ndn::InterestFilter&,
                                                 const ndn::Interest& interest)
{
  PolicyManifest manifest = buildPolicyManifest();
  ndn::Data data(interest.getName());
  data.setContent(manifest.WireEncode());
  data.setFreshnessPeriod(ndn::time::milliseconds(0));
  m_keyChain.sign(data, ndn::security::SigningInfo(
    ndn::security::SigningInfo::SIGNER_TYPE_ID, m_controllerPrefix));
  m_face.put(data);

  NDN_LOG_INFO("[POLICY-MANIFEST] Reply data=" << data.getName()
               << " payload=" << manifest.toString());
}

void
ServiceController::onPolicyStatusInterest(const ndn::InterestFilter&,
                                          const ndn::Interest& interest)
{
  if (!m_generationReady || !m_prefixPolicyStatus.isPrefixOf(interest.getName()) ||
      interest.getName().size() <= m_prefixPolicyStatus.size()) {
    NDN_LOG_WARN("NDNSF_POLICY_STATUS_REFUSED reason=controller-generation-unavailable");
    return;
  }

  const auto parsedName = parsePolicyStatusName(m_controllerPrefix, interest.getName());
  if (!parsedName || parsedName->serviceName.empty()) {
    NDN_LOG_WARN("NDNSF_POLICY_STATUS_REFUSED reason=missing-service");
    return;
  }

  const auto serviceName = parsedName->serviceName;
  try {
    const auto status = getPolicyStatus(serviceName);
    if (parsedName->version &&
        status.getControllerVersion() != *parsedName->version) {
      NDN_LOG_WARN("NDNSF_POLICY_STATUS_REFUSED service=" << serviceName
                   << " reason=version-not-available requested="
                   << parsedName->version->toString()
                   << " current=" << status.getControllerVersion().toString());
      return;
    }
    const auto dataName = makePolicyStatusName(
        m_controllerPrefix, serviceName, status.getControllerVersion());
    ndn::Data data(dataName);
    data.setContent(status.wireEncode());
    data.setFreshnessPeriod(ndn::time::milliseconds(0));
    m_keyChain.sign(data, ndn::security::SigningInfo(
      ndn::security::SigningInfo::SIGNER_TYPE_ID, m_controllerPrefix));
    m_face.put(data);
    NDN_LOG_INFO("NDNSF_POLICY_STATUS_PUBLISHED service=" << serviceName
                 << " generation=" << m_controllerVersion.controllerGenerationTimestamp
                 << " epoch=" << m_controllerVersion.controllerEpoch
                 << " revocations=" << status.getRevocations().size());
  }
  catch (const std::exception& error) {
    NDN_LOG_ERROR("NDNSF_POLICY_STATUS_REFUSED service=" << serviceName
                  << " reason=" << error.what());
  }
}

void
ServiceController::onCertificateBootstrapInterest(const ndn::InterestFilter&,
                                                  const ndn::Interest& interest)
{
  if (!m_prefixCertificateBootstrap.isPrefixOf(interest.getName()) ||
      interest.getName().size() <= m_prefixCertificateBootstrap.size()) {
    return;
  }

  const ndn::Name targetIdentity = stripCertificateBootstrapInterestSuffix(
    interest.getName().getSubName(m_prefixCertificateBootstrap.size()));
  const std::string targetUri = targetIdentity.toUri();

  try {
    const auto tokenIt = m_bootstrapTokens.find(targetUri);
    if (tokenIt == m_bootstrapTokens.end()) {
      const std::string message = "no bootstrap token configured for " + targetUri;
      NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                   << " reason=no-token");
      sendCertificateBootstrapResponse(interest, false, message, nullptr);
      return;
    }

    const auto& tokenEntry = tokenIt->second;

    const auto& params = interest.getApplicationParameters();
    if (!params.isValid()) {
      NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                   << " reason=missing-parameters");
      sendCertificateBootstrapResponse(interest, false, "missing bootstrap request parameters", nullptr);
      return;
    }

    CertificateBootstrapRequest request;
    bool encryptedRequest = false;
    EncryptedCertificateBootstrapRequest encrypted;
    if (encrypted.wireDecode(params)) {
      try {
        request = decryptCertificateBootstrapRequestWithKeyChain(encrypted, m_keyChain);
        encryptedRequest = true;
      }
      catch (const std::exception& e) {
        NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                     << " reason=decrypt-failed error=" << e.what());
        sendCertificateBootstrapResponse(interest, false, "encrypted bootstrap request decrypt failed", nullptr);
        return;
      }
    }
    else {
      NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                   << " reason=unencrypted-or-malformed-request");
      sendCertificateBootstrapResponse(interest, false, "encrypted bootstrap request required", nullptr);
      return;
    }

    if (request.identity != targetIdentity) {
      NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                   << " reason=request-name-mismatch"
                   << " requestIdentity=" << request.identity.toUri());
      sendCertificateBootstrapResponse(interest, false, "bootstrap name mismatch", nullptr);
      return;
    }

    if (request.token != tokenEntry.token) {
      NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                   << " reason=token-mismatch");
      sendCertificateBootstrapResponse(interest, false, "bootstrap token mismatch", nullptr);
      return;
    }

    if (request.certificateRequest.getIdentity() != targetIdentity) {
      NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                   << " reason=request-identity-mismatch"
                   << " certIdentity=" << request.certificateRequest.getIdentity());
      sendCertificateBootstrapResponse(interest, false, "certificate request identity mismatch", nullptr);
      return;
    }

    if (request.proofNonce.empty() || request.proofSignature.empty()) {
      NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                   << " reason=missing-proof");
      sendCertificateBootstrapResponse(interest, false, "certificate bootstrap proof missing", nullptr);
      return;
    }

    auto proofData = makeCertificateBootstrapProofData(request);
    proofData.encode();
    if (!ndn::security::verifySignature(
          ndn::InputBuffers{ndn::span<const uint8_t>(proofData.data(), proofData.size())},
          bufferToSpan(request.proofSignature),
          request.certificateRequest.getPublicKey())) {
      NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                   << " reason=request-proof-invalid");
      sendCertificateBootstrapResponse(interest, false, "certificate bootstrap proof invalid", nullptr);
      return;
    }

    auto issued = m_keyChain.makeCertificate(
      request.certificateRequest,
      ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_ID,
                                 m_controllerPrefix));
    m_bootstrapIssuedCertificates[targetUri] = issued;

    NDN_LOG_INFO("NDNSF_CERT_BOOTSTRAP_ISSUED identity=" << targetUri
                 << " cert=" << issued.getName().toUri()
                 << " role=" << tokenEntry.role
                 << " encryptedRequest=" << (encryptedRequest ? "true" : "false")
                 << " requesterProof=true");
    sendCertificateBootstrapResponse(interest, true, "issued", &issued);
  }
  catch (const std::exception& e) {
    NDN_LOG_ERROR("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                  << " reason=exception error=" << e.what());
    sendCertificateBootstrapResponse(interest, false, e.what(), nullptr);
  }
}

} // namespace ndn_service_framework
