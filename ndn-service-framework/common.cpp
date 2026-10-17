#include "common.hpp"

#include <ndn-cxx/security/transform/public-key.hpp>
#include <ndn-cxx/security/validity-period.hpp>
#include <ndn-cxx/util/io.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/property_tree/info_parser.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace ndn_service_framework {

class EncapsulatedMessageValidator final : public ndn::svs::BaseValidator
{
public:
  explicit EncapsulatedMessageValidator(std::shared_ptr<MessageValidator> owner)
    : m_owner(std::move(owner)) {}

  void validate(const ndn::Data& data,
                const ndn::security::DataValidationSuccessCallback& success,
                const ndn::security::DataValidationFailureCallback& failure) override
  { m_owner->validateEncapsulatedData(data, success, failure); }

  void validate(const ndn::Interest& interest,
                const ndn::security::InterestValidationSuccessCallback& success,
                const ndn::security::InterestValidationFailureCallback& failure) override
  { m_owner->validate(interest, success, failure); }

private:
  std::shared_ptr<MessageValidator> m_owner;
};

std::shared_ptr<ndn::svs::BaseValidator>
MessageValidator::makeEncapsulatedValidator()
{
  return std::make_shared<EncapsulatedMessageValidator>(shared_from_this());
}

namespace {

using Certificate = ndn::security::Certificate;
using ValidationState = ndn::security::ValidationState;
using ValidationError = ndn::security::ValidationError;
using TimePoint = ndn::time::system_clock::time_point;

struct ChainLifetime
{
  TimePoint expires;
  uint64_t generation;
};
using ChainLifetimeTag = ndn::SimpleTag<ChainLifetime, 0x4e534654>;

// Keep ndn-cxx's schema and chain verification; add lifetime bounds that its
// trusted-certificate cache does not inherit from a certificate's ancestors.
class FixedRootPolicy final : public ndn::security::ValidationPolicyConfig
{
public:
  void
  loadFixed(std::istream& input, const std::string& filename)
  {
    m_rootValidityPeriods.clear();
    ++m_generation;
    m_verifiedUntil = TimePoint::max();
    m_validator->resetAnchors();
    m_validator->resetVerifiedCertificates();
    ndn::security::validator_config::ConfigSection config;
    if (!input) throw std::runtime_error("Cannot open NDNSF trust schema: " + filename);
    boost::property_tree::read_info(input, config);
    std::vector<Certificate> roots;
    std::vector<ndn::security::ValidityPeriod> rootValidityPeriods;
    const auto directory = std::filesystem::absolute(filename).parent_path();
    for (auto it = config.begin(); it != config.end();) {
      if (!boost::iequals(it->first, "trust-anchor")) {
        ++it;
        continue;
      }
      const auto& anchor = it->second;
      const auto type = anchor.get<std::string>("type", "");
      std::shared_ptr<Certificate> certificate;
      if (boost::iequals(type, "file")) {
        // Pin the bytes loaded here. A file refresh must not silently replace
        // the application's configured root while the validator is running.
        if (anchor.get_optional<std::string>("refresh")) {
          throw std::runtime_error("NDNSF requires a fixed trust-anchor file without refresh");
        }
        certificate = ndn::io::load<Certificate>(
          (directory / anchor.get<std::string>("file-name")).string());
      }
      else if (boost::iequals(type, "base64")) {
        std::istringstream input(anchor.get<std::string>("base64-string"));
        certificate = ndn::io::load<Certificate>(input);
      }
      else {
        throw std::runtime_error("NDNSF requires fixed file/base64 trust anchors; type " +
                                 type + " is not allowed");
      }
      if (!certificate) {
        throw std::runtime_error("NDNSF trust anchor is missing, malformed, or outside validity");
      }
      const auto validity = certificate->getValidityPeriod();
      if (!validity.isValid()) {
        throw std::runtime_error("NDNSF trust anchor is missing, malformed, or outside validity");
      }
      rootValidityPeriods.push_back(validity);
      roots.push_back(std::move(*certificate));
      it = config.erase(it);
    }
    if (roots.empty()) {
      throw std::runtime_error("NDNSF trust schema must contain a fixed trust anchor");
    }
    ndn::security::ValidationPolicyConfig::load(config, filename);
    for (const auto& root : roots) {
      m_validator->loadAnchor(root.getName().toUri(), Certificate(root));
    }
    // Anchors are pinned until explicit reload. Decode their immutable dates
    // once, but compare both bounds with the current clock on every packet.
    m_rootValidityPeriods = std::move(rootValidityPeriods);
  }

  void
  checkPolicy(const ndn::Data& data, const std::shared_ptr<ValidationState>& state,
              const ValidationContinuation& continuation) override
  {
    if (!checkLifetime(state)) return;
    if (data.getContentType() == ndn::tlv::ContentType_Key &&
        Certificate::isValidName(data.getName())) {
      bool valid = false;
      TimePoint expires;
      try {
        const Certificate certificate(data);
        valid = certificate.isValid();
        expires = certificate.getValidityPeriod().getPeriod().second;
      }
      catch (const std::exception& error) {
        state->fail({ValidationError::MALFORMED_CERT, error.what()});
        return;
      }
      // Consumer callback exceptions must not be caught as certificate errors
      // or cause a second terminal callback for the same validation state.
      if (!valid) {
        state->fail({ValidationError::EXPIRED_CERT, "certificate is outside its validity period"});
        return;
      }
      auto lifetime = state->getTag<ChainLifetimeTag>()->get();
      lifetime.expires = std::min(lifetime.expires, expires);
      state->setTag(std::make_shared<ChainLifetimeTag>(lifetime));
    }
    ndn::security::ValidationPolicyConfig::checkPolicy(
      data, state, guardContinuation(continuation));
  }

  void
  checkPolicy(const ndn::Interest& interest, const std::shared_ptr<ValidationState>& state,
              const ValidationContinuation& continuation) override
  {
    if (!checkLifetime(state)) return;
    ndn::security::ValidationPolicyConfig::checkPolicy(
      interest, state, guardContinuation(continuation));
  }

private:
  bool
  checkLifetime(const std::shared_ptr<ValidationState>& state)
  {
    const auto now = ndn::time::system_clock::now();
    if (m_rootValidityPeriods.empty()) {
      state->fail({ValidationError::POLICY_ERROR, "no fixed NDNSF trust anchor configured"});
      return false;
    }
    auto rootExpiry = TimePoint::max();
    for (const auto& validity : m_rootValidityPeriods) {
      if (!validity.isValid(now)) {
        m_validator->resetVerifiedCertificates();
        state->fail({ValidationError::EXPIRED_CERT, "configured trust anchor is outside validity"});
        return false;
      }
      rootExpiry = std::min(rootExpiry, validity.getPeriod().second);
    }
    if (now >= m_verifiedUntil) {
      m_validator->resetVerifiedCertificates();
      m_verifiedUntil = TimePoint::max();
    }
    auto tag = state->getTag<ChainLifetimeTag>();
    if (!tag) {
      tag = std::make_shared<ChainLifetimeTag>(
        ChainLifetime{std::min(rootExpiry, m_verifiedUntil), m_generation});
      state->setTag(tag);
    }
    if (tag->get().generation != m_generation || now >= tag->get().expires) {
      state->fail({ValidationError::EXPIRED_CERT, "validation chain expired or trust was reloaded"});
      return false;
    }
    return true;
  }

  ValidationContinuation
  guardContinuation(ValidationContinuation continuation)
  {
    return [this, continuation = std::move(continuation)](
      const std::shared_ptr<ndn::security::CertificateRequest>& request,
      const std::shared_ptr<ValidationState>& state) {
      if (!checkLifetime(state)) return;
      if (!request || request->interest.getName() ==
                        ndn::security::SigningInfo::getDigestSha256Identity()) {
        state->fail({ValidationError::POLICY_ERROR,
                     "NDNSF requires certificate-backed signature validation"});
        return;
      }
      continuation(request, state);
      if (state->getOutcome()) {
        const auto lifetime = state->getTag<ChainLifetimeTag>()->get();
        // ndn-cxx caches the chain after calling the consumer. A consumer may
        // reload trust synchronously; do not retain the old chain afterward.
        if (lifetime.generation != m_generation) {
          m_validator->resetVerifiedCertificates();
        }
        else {
          m_verifiedUntil = std::min(m_verifiedUntil, lifetime.expires);
        }
      }
    };
  }

  std::vector<ndn::security::ValidityPeriod> m_rootValidityPeriods;
  TimePoint m_verifiedUntil = TimePoint::max();
  uint64_t m_generation = 0;
};

std::optional<Certificate>
findPibCertificate(ndn::KeyChain& keyChain, const ndn::Interest& interest)
{
  try {
    const auto identityName = ndn::security::extractIdentityNameFromKeyLocator(interest.getName());
    const auto identity = keyChain.getPib().getIdentity(identityName);
    for (const auto& key : identity.getKeys()) {
      try {
        const auto certificate = key.getDefaultCertificate();
        if (interest.matchesData(certificate)) return certificate;
      }
      catch (const ndn::security::pib::Pib::Error&) {
        // A key without a default certificate may still have other candidates.
      }
      for (const auto& candidate : key.getCertificates()) {
        if (interest.matchesData(candidate)) return candidate;
      }
    }
  }
  catch (const std::exception&) {
    // A local miss remains eligible for ordinary network certificate retrieval.
  }
  return std::nullopt;
}

class PibCertificateFetcher final : public ndn::security::CertificateFetcherFromNetwork
{
public:
  explicit
  PibCertificateFetcher(ndn::Face& face)
    : CertificateFetcherFromNetwork(face)
  {
  }

private:
  void
  doFetch(const std::shared_ptr<ndn::security::CertificateRequest>& request,
          const std::shared_ptr<ValidationState>& state,
          const ValidationContinuation& continuation) override
  {
    auto certificate = findPibCertificate(m_keyChain, request->interest);
    if (certificate) {
      // CertificateFetcher::fetch caches this only as an unverified candidate
      // and then continues ndn-cxx's recursive policy and signature validation.
      continuation(*certificate, state);
      return;
    }
    CertificateFetcherFromNetwork::doFetch(request, state, continuation);
  }

  ndn::KeyChain m_keyChain;
};

size_t
readSizeEnvironment(const char* name, size_t current)
{
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return current;
  }

  std::string text(raw);
  if (text.empty() || !std::all_of(text.begin(), text.end(),
                                  [] (unsigned char c) { return std::isdigit(c) != 0; })) {
    throw std::invalid_argument(std::string(name) + " must be a positive integer");
  }
  size_t parsed = 0;
  try {
    parsed = static_cast<size_t>(std::stoull(text));
  }
  catch (const std::exception&) {
    throw std::invalid_argument(std::string(name) + " must be a positive integer");
  }
  if (parsed == 0 || parsed > ndn::MAX_NDN_PACKET_SIZE) {
    throw std::invalid_argument(std::string(name) + " exceeds the NDN packet-size bound");
  }
  return parsed;
}

ndn::time::milliseconds
readPositiveMillisecondsEnvironment(const char* name,
                                    ndn::time::milliseconds current)
{
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return current;
  }

  std::string text(raw);
  if (!std::all_of(text.begin(), text.end(),
                   [] (unsigned char c) { return std::isdigit(c) != 0; })) {
    throw std::invalid_argument(std::string(name) + " must be a positive integer");
  }
  uint64_t parsed = 0;
  try {
    parsed = std::stoull(text);
  }
  catch (const std::exception&) {
    throw std::invalid_argument(std::string(name) + " must be a positive integer");
  }
  constexpr uint64_t MAX_PERIODIC_SYNC_MS = 60ULL * 60ULL * 1000ULL;
  if (parsed == 0 || parsed > MAX_PERIODIC_SYNC_MS) {
    throw std::invalid_argument(
      std::string(name) + " must be between 1 and 3600000 milliseconds");
  }
  return ndn::time::milliseconds(parsed);
}

} // namespace

TrustSchemaValidator::TrustSchemaValidator(
  std::unique_ptr<ndn::security::CertificateFetcher> fetcher,
  const CommandInterestOptions& commandOptions,
  const SignedInterestOptions& signedOptions)
  : ndn::security::Validator(
      std::make_unique<ndn::security::ValidationPolicySignedInterest>(
        std::make_unique<ndn::security::ValidationPolicyCommandInterest>(
          std::make_unique<FixedRootPolicy>(), commandOptions), signedOptions),
      std::move(fetcher))
{
}

TrustSchemaValidator::TrustSchemaValidator(ndn::Face& face,
                                          const CommandInterestOptions& commandOptions,
                                          const SignedInterestOptions& signedOptions)
  : TrustSchemaValidator(std::make_unique<PibCertificateFetcher>(face),
                         commandOptions, signedOptions)
{
}

void
TrustSchemaValidator::load(const std::string& filename)
{
  auto& policy = static_cast<FixedRootPolicy&>(getPolicy().getInnerPolicy().getInnerPolicy());
  std::ifstream input(filename);
  // Pass a failed stream through the same fail-closed reload path.
  policy.loadFixed(input, filename);
}

void
TrustSchemaValidator::load(const std::string& input, const std::string& filename)
{
  auto& policy = static_cast<FixedRootPolicy&>(getPolicy().getInnerPolicy().getInnerPolicy());
  std::istringstream stream(input);
  policy.loadFixed(stream, filename);
}

void
loadTrustSchema(TrustSchemaValidator& validator, const std::string& filename)
{
  validator.load(filename);
}

std::unique_ptr<ndn::security::CertificateFetcher>
MessageValidator::makeCertificateFetcher(ndn::Face& fallbackFace, ndn::Face* callbackFace)
{
  auto& face = callbackFace == nullptr ? fallbackFace : *callbackFace;
  return std::make_unique<PibCertificateFetcher>(face);
}

NDN_LOG_MEMBER_INIT(SerializedWorkerQueue, ndn_service_framework.SerializedWorkerQueue);
NDN_LOG_MEMBER_INIT(BoundedWorkerPool, ndn_service_framework.BoundedWorkerPool);
NDN_LOG_MEMBER_INIT(MessageValidator, ndn_service_framework.MessageValidator);

ndn::KeyType
getCertificateKeyType(const ndn::security::Certificate& cert)
{
  ndn::security::transform::PublicKey publicKey;
  publicKey.loadPkcs8(cert.getPublicKey());
  return publicKey.getKeyType();
}

ndn::security::Certificate
getEcdsaSigningCertificateOrFallback(ndn::KeyChain& keyChain,
                                     const ndn::security::Certificate& fallbackCert)
{
  try {
    auto identity = keyChain.getPib().getIdentity(fallbackCert.getIdentity());
    for (const auto& key : identity.getKeys()) {
      if (key.getKeyType() == ndn::KeyType::EC) {
        try {
          return key.getDefaultCertificate();
        }
        catch (const std::exception&) {
        }
      }
    }
  }
  catch (const std::exception&) {
  }
  return fallbackCert;
}

ndn::security::Certificate
getRsaEncryptionCertificateOrThrow(ndn::KeyChain& keyChain,
                                   const ndn::security::Certificate& identityHintCert)
{
  if (getCertificateKeyType(identityHintCert) == ndn::KeyType::RSA) {
    return identityHintCert;
  }

  try {
    auto identity = keyChain.getPib().getIdentity(identityHintCert.getIdentity());
    for (const auto& key : identity.getKeys()) {
      if (key.getKeyType() == ndn::KeyType::RSA) {
        try {
          return key.getDefaultCertificate();
        }
        catch (const std::exception&) {
        }
      }
    }
  }
  catch (const std::exception&) {
  }

  throw std::invalid_argument("NDNSF requires an RSA encryption certificate for NAC-ABE");
}

ndn::security::Certificate
getRsaEncryptionCertificateOrThrow(const ndn::security::Certificate& identityHintCert)
{
  ndn::KeyChain keyChain;
  return getRsaEncryptionCertificateOrThrow(keyChain, identityHintCert);
}

ndn::security::SigningInfo
makeEcdsaPreferredSigningInfo(ndn::KeyChain& keyChain,
                              const ndn::Name& identityName)
{
  try {
    auto identity = keyChain.getPib().getIdentity(identityName);
    for (const auto& key : identity.getKeys()) {
      if (key.getKeyType() == ndn::KeyType::EC) {
        try {
          return ndn::security::signingByCertificate(key.getDefaultCertificate());
        }
        catch (const std::exception&) {
        }
      }
    }
    for (const auto& key : identity.getKeys()) {
      if (key.getKeyType() == ndn::KeyType::RSA) {
        try {
          return ndn::security::signingByCertificate(key.getDefaultCertificate());
        }
        catch (const std::exception&) {
        }
      }
    }
  }
  catch (const std::exception&) {
  }
  return ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_ID,
                                    identityName);
}

void
signDataEcdsaPreferred(ndn::KeyChain& keyChain,
                       ndn::Data& data,
                       const ndn::Name& identityName)
{
  keyChain.sign(data, makeEcdsaPreferredSigningInfo(keyChain, identityName));
}

void
configureSvsPubSubOptionsFromEnvironment(ndn::svs::SVSPubSubOptions& options)
{
  // These are transport/resource bounds. They do not alter SVS suppression,
  // retry, or selection semantics; the piggyback value is only a safety
  // margin and oversized publications remain recoverable by name.
  const ndn::svs::SVSPubSubOptions libraryDefaults;
  const auto applicationBudget = options.maxApplicationParametersSize == libraryDefaults.maxApplicationParametersSize ?
      6144 : options.maxApplicationParametersSize;
  const auto piggyBudget = options.maxPiggyDataSize == libraryDefaults.maxPiggyDataSize ?
      4096 : options.maxPiggyDataSize;
  options.maxApplicationParametersSize = readSizeEnvironment(
    "NDNSF_SVS_MAX_APP_PARAMS_BYTES", applicationBudget);
  options.maxPiggyDataSize = readSizeEnvironment(
    "NDNSF_SVS_MAX_PIGGYDATA_BYTES", piggyBudget);
  if (std::getenv("NDNSF_SVS_PERIODIC_SYNC_MS") != nullptr) {
    // Configure this before SVSPubSub construction. Calling
    // setPeriodicSyncTime() afterward changes the stored interval but does not
    // necessarily reschedule the already-armed first periodic timer.
    options.syncProtocol.periodicTimeout =
      readPositiveMillisecondsEnvironment(
        "NDNSF_SVS_PERIODIC_SYNC_MS", ndn::time::seconds(30));
  }
}

const char*
selectionExecutionStateToString(SelectionExecutionState state)
{
  switch (state) {
  case SelectionExecutionState::Unknown: return "Unknown";
  case SelectionExecutionState::Received: return "Received";
  case SelectionExecutionState::Queued: return "Queued";
  case SelectionExecutionState::Running: return "Running";
  case SelectionExecutionState::Completed: return "Completed";
  case SelectionExecutionState::Failed: return "Failed";
  case SelectionExecutionState::Rejected: return "Rejected";
  case SelectionExecutionState::Expired: return "Expired";
  case SelectionExecutionState::Cancelled: return "Cancelled";
  }
  return "Unknown";
}

SelectionExecutionState
selectionExecutionStateFromString(const std::string& state)
{
  if (state == "Received") return SelectionExecutionState::Received;
  if (state == "Queued") return SelectionExecutionState::Queued;
  if (state == "Running") return SelectionExecutionState::Running;
  if (state == "Completed") return SelectionExecutionState::Completed;
  if (state == "Failed") return SelectionExecutionState::Failed;
  if (state == "Rejected") return SelectionExecutionState::Rejected;
  if (state == "Expired") return SelectionExecutionState::Expired;
  if (state == "Cancelled") return SelectionExecutionState::Cancelled;
  return SelectionExecutionState::Unknown;
}

} // namespace ndn_service_framework
