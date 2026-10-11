#include "common.hpp"

#include <ndn-cxx/security/transform/public-key.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <string>

namespace ndn_service_framework {

namespace {

bool
signatureTimingEnabled() noexcept
{
  const char* raw = std::getenv("NDNSF_PHASE_TIMING");
  return raw != nullptr && *raw != '\0' &&
         std::strcmp(raw, "0") != 0 && std::strcmp(raw, "false") != 0 &&
         std::strcmp(raw, "FALSE") != 0 && std::strcmp(raw, "no") != 0 &&
         std::strcmp(raw, "NO") != 0;
}

std::optional<uint64_t>
signatureThreadCpuUs() noexcept
{
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0 ||
      value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000) {
    return std::nullopt;
  }
  const auto seconds = static_cast<uint64_t>(value.tv_sec);
  const auto micros = static_cast<uint64_t>(value.tv_nsec) / 1000;
  if (seconds > (std::numeric_limits<uint64_t>::max() - micros) / 1000000) {
    return std::nullopt;
  }
  return seconds * 1000000 + micros;
}

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

NDN_LOG_MEMBER_INIT(SerializedWorkerQueue, ndn_service_framework.SerializedWorkerQueue);
NDN_LOG_MEMBER_INIT(BoundedWorkerPool, ndn_service_framework.BoundedWorkerPool);
NDN_LOG_MEMBER_INIT(MessageValidator, ndn_service_framework.MessageValidator);

bool
MessageValidator::verifyLocalDataSignature(
  const ndn::Data& data,
  const std::optional<ndn::security::Certificate>& cert)
{
  if (!signatureTimingEnabled()) {
    return ndn::security::verifySignature(data, cert);
  }
  const auto wallBegin = std::chrono::steady_clock::now();
  const auto cpuBegin = signatureThreadCpuUs();
  const auto report = [&] (const char* result) noexcept {
    try {
      const auto cpuEnd = signatureThreadCpuUs();
      const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - wallBegin).count();
      std::string cpu = "unknown";
      if (cpuBegin && cpuEnd && *cpuEnd >= *cpuBegin) {
        cpu = std::to_string(*cpuEnd - *cpuBegin);
      }
      NDN_LOG_WARN("NDNSF_SIGNATURE_VERIFY data_name=" << data.getName()
                   << " certificate_name=" << (cert ? cert->getName() : ndn::Name())
                   << " result=" << result << " duration_us=" << duration
                   << " thread_cpu_us=" << cpu);
    }
    catch (...) {
      // Optional diagnostics must not change signature validation behavior.
    }
  };
  bool verified;
  try {
    verified = ndn::security::verifySignature(data, cert);
  }
  catch (...) {
    report("exception");
    throw;
  }
  report(verified ? "valid" : "invalid");
  return verified;
}

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
