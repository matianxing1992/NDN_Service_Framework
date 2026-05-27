#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

const ndn::Name GROUP_PREFIX("/example/hello/group");
const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
const ndn::Name PROVIDER_IDENTITY("/example/hello/provider");

class KeyChainInitLock
{
public:
  explicit KeyChainInitLock(const char* path)
  {
    m_fd = open(path, O_CREAT | O_RDWR, 0666);
    if (m_fd < 0 || flock(m_fd, LOCK_EX) != 0) {
      throw std::runtime_error("Failed to acquire KeyChain initialization lock");
    }
  }

  ~KeyChainInitLock()
  {
    unlock();
  }

  void
  unlock()
  {
    if (m_fd >= 0) {
      flock(m_fd, LOCK_UN);
      close(m_fd);
      m_fd = -1;
    }
  }

private:
  int m_fd = -1;
};

ndn::security::Certificate
getOrCreateIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
{
  try {
    return keyChain.getPib()
      .getIdentity(identity)
      .getDefaultKey()
      .getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey()
      .getDefaultCertificate();
  }
}

std::string
getOption(int argc, char** argv, const std::string& option, const std::string& fallback)
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == option) {
      return argv[i + 1];
    }
  }
  return fallback;
}

bool
hasFlag(int argc, char** argv, const std::string& option)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == option) {
      return true;
    }
  }
  return false;
}

int
parseIntOption(int argc, char** argv, const std::string& option, int fallback)
{
  const auto value = getOption(argc, argv, option, "");
  if (value.empty()) {
    return fallback;
  }
  try {
    return std::stoi(value);
  }
  catch (const std::exception&) {
    return fallback;
  }
}

double
parseDoubleOption(int argc, char** argv, const std::string& option, double fallback)
{
  const auto value = getOption(argc, argv, option, "");
  if (value.empty()) {
    return fallback;
  }
  try {
    return std::stod(value);
  }
  catch (const std::exception&) {
    return fallback;
  }
}

ndn::Buffer
makeBuffer(const std::string& text)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::string
payloadToString(const ndn_service_framework::RequestMessage& request)
{
  const auto payload = request.getPayload();
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

class IntermittentAvailability
{
public:
  IntermittentAvailability(double probability,
                           int epochMs,
                           int rejectMs,
                           uint32_t seed,
                           std::string availabilityFile = "")
    : m_probability(probability)
    , m_epochMs(std::max(1, epochMs))
    , m_rejectMs(std::max(0, rejectMs))
    , m_seed(seed)
    , m_availabilityFile(std::move(availabilityFile))
    , m_started(std::chrono::steady_clock::now())
  {
  }

  uint64_t
  epoch() const
  {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - m_started).count();
    return static_cast<uint64_t>(elapsed / m_epochMs);
  }

  bool
  isUnavailable() const
  {
    if (!m_availabilityFile.empty()) {
      std::ifstream input(m_availabilityFile);
      char value = '1';
      input >> value;
      return value == '0';
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - m_started).count();
    const auto epoch = static_cast<uint64_t>(elapsed / m_epochMs);
    const auto offset = elapsed % m_epochMs;
    return isRejectingEpoch(epoch) && offset < m_rejectMs;
  }

private:
  bool
  isRejectingEpoch(uint64_t epoch) const
  {
    std::seed_seq seq{
      m_seed,
      static_cast<uint32_t>(epoch),
      static_cast<uint32_t>(epoch >> 32),
      0x9e3779b9u
    };
    std::mt19937 rng(seq);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng) < m_probability;
  }

private:
  double m_probability;
  int m_epochMs;
  int m_rejectMs;
  uint32_t m_seed;
  std::string m_availabilityFile;
  std::chrono::steady_clock::time_point m_started;
};

} // namespace

int
main(int argc, char** argv)
{
  try {
    const std::string providerId = getOption(argc, argv, "--provider-id", "A");
    const ndn::Name providerIdentity = ndn::Name(PROVIDER_IDENTITY).append(providerId);
    const ndn::Name serviceName(getOption(argc, argv, "--service", "/HELLO"));
    const double failureProbability =
      parseDoubleOption(argc, argv, "--failure-probability", 0.2);
    const int epochMs = parseIntOption(argc, argv, "--epoch-ms", 10000);
    const int rejectMs = parseIntOption(argc, argv, "--reject-ms", 10000);
    const int normalDelayMs = parseIntOption(argc, argv, "--processing-delay-ms", 5);
    const int unavailableDelayMs =
      parseIntOption(argc, argv, "--unavailable-processing-delay-ms", 10000);
    const int handlerThreads = parseIntOption(argc, argv, "--handler-threads", 4);
    const auto seed = static_cast<uint32_t>(
      parseIntOption(argc, argv, "--seed", 1000 + static_cast<int>(providerId[0])));
    const std::string availabilityFile = getOption(argc, argv, "--availability-file", "");
    const bool serveCertificates = !hasFlag(argc, argv, "--no-serve-certificates");

    ndn::Face face;
    ndn::KeyChain keyChain;

    KeyChainInitLock keyLock("/tmp/ndnsf-intermittent-provider-keychain.lock");
    auto providerCert = getOrCreateIdentity(keyChain, providerIdentity);
    auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(providerIdentity));
    keyLock.unlock();

    std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
    if (serveCertificates) {
      certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
        face, keyChain, providerCert.getName());
    }

    IntermittentAvailability availability(
      failureProbability, epochMs, rejectMs, seed, availabilityFile);

    ndn_service_framework::ServiceProvider provider(
      face, GROUP_PREFIX, providerCert, controllerCert, "examples/trust-schema.conf");
    provider.setPerformanceMode(true);
    provider.setUseTokens(true);
    provider.setHandlerThreads(static_cast<size_t>(std::max(0, handlerThreads)));

    provider.addService(
      serviceName,
      ndn_service_framework::ServiceProvider::AckStrategyHandler(
        [providerId, &availability, serviceName](
          const ndn_service_framework::RequestMessage&) {
          ndn_service_framework::ServiceProvider::AckDecision decision;
          if (availability.isUnavailable()) {
            decision.suppressAck = true;
            decision.status = false;
            decision.message = "provider unavailable";
            std::cout << "INTERMITTENT_PROVIDER_ACK_SUPPRESSED provider="
                      << providerId << " service=" << serviceName.toUri()
                      << " epoch=" << availability.epoch() << std::endl;
            return decision;
          }

          std::ostringstream metadata;
          metadata << "provider=" << providerId
                   << ";epoch=" << availability.epoch()
                   << ";available=1";
          const auto metadataText = metadata.str();
          decision.status = true;
          decision.message = "available";
          decision.payload = makeBuffer(metadataText);
          return decision;
        }),
      ndn_service_framework::ServiceProvider::RequestHandler(
        [providerId, &availability, normalDelayMs, unavailableDelayMs](
          const ndn::Name&,
          const ndn::Name&,
          const ndn::Name& serviceName,
          const ndn::Name& requestId,
          const ndn_service_framework::RequestMessage& request) {
          const bool unavailable = availability.isUnavailable();
          const int delayMs = unavailable ? unavailableDelayMs : normalDelayMs;
          if (delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
          }

          ndn_service_framework::ResponseMessage response;
          if (unavailable || payloadToString(request) != "HELLO") {
            response.setStatus(false);
            response.setErrorInfo(unavailable ? "provider unavailable" :
                                                "unexpected request payload");
            std::cout << "INTERMITTENT_PROVIDER_RESPONSE_FAILURE provider="
                      << providerId << " request=" << requestId.toUri()
                      << " service=" << serviceName.toUri()
                      << " unavailable=" << unavailable << std::endl;
            return response;
          }

          auto payload = makeBuffer("HELLO");
          response.setStatus(true);
          response.setErrorInfo("No error");
          response.setPayload(payload, payload.size());
          return response;
        }));

    provider.init();
    provider.fetchPermissionsFromController(CONTROLLER_PREFIX);

    std::cout << "INTERMITTENT_PROVIDER_READY provider=" << providerId
              << " identity=" << providerIdentity.toUri()
              << " service=" << serviceName.toUri()
              << " failureProbability=" << failureProbability
              << " epochMs=" << epochMs
              << " rejectMs=" << rejectMs
              << " selectiveAck=suppress-when-unavailable"
              << std::endl;

    face.processEvents();
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "App_IntermittentProvider error: " << e.what() << std::endl;
    return 1;
  }
}
