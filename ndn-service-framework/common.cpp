#include "common.hpp"

#include <ndn-cxx/security/transform/public-key.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace ndn_service_framework {

namespace {

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

} // namespace

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
  options.maxApplicationParametersSize = readSizeEnvironment(
    "NDNSF_SVS_MAX_APP_PARAMS_BYTES", options.maxApplicationParametersSize);
  options.maxPiggyDataSize = readSizeEnvironment(
    "NDNSF_SVS_MAX_PIGGYDATA_BYTES", options.maxPiggyDataSize);
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
