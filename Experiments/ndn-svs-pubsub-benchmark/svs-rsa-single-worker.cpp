#include <ndn-svs/svspubsub.hpp>

#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/io.hpp>

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <dirent.h>
#include <sys/resource.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace ndn;
using namespace ndn::svs;
using namespace std::chrono_literals;

namespace {

constexpr uint64_t NS = 1000000000ULL;
constexpr size_t PAYLOAD_SIZE = 256;
constexpr size_t WORKER_QUEUE_CAPACITY = 4096;
constexpr uint16_t PUBLICATION_FETCH_WINDOW = 64;
constexpr unsigned SYNC_BATCH_WINDOW_MS = 5;
constexpr size_t NDNSF_MAX_PIGGY_DATA_SIZE = 800;
constexpr size_t NDNSF_MAX_APPLICATION_PARAMETERS_SIZE = 4096;
constexpr uint16_t NDNSF_MAPPING_FETCH_WINDOW = 10;
constexpr size_t NDNSF_SYNC_WORKERS = 4;
constexpr size_t NDNSF_SYNC_QUEUE = 256;

uint64_t
nowRaw()
{
  ::timespec value{};
  if (::clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
    throw std::runtime_error("CLOCK_MONOTONIC_RAW failed");
  }
  return static_cast<uint64_t>(value.tv_sec) * NS +
         static_cast<uint64_t>(value.tv_nsec);
}

uint64_t
hostToBe64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return __builtin_bswap64(value);
#else
  return value;
#endif
}

uint64_t
beToHost64(uint64_t value)
{
  return hostToBe64(value);
}

uint64_t
fnv1a(const std::string& value)
{
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

uint64_t
percentile(std::vector<uint64_t> values, unsigned pct)
{
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  const size_t rank = (values.size() * pct + 99) / 100;
  return values[std::max<size_t>(1, rank) - 1];
}

uint64_t
mean(const std::vector<uint64_t>& values)
{
  if (values.empty()) {
    return 0;
  }
  const auto total = std::accumulate(
    values.begin(), values.end(), uint64_t{0});
  return total / values.size();
}

void
updateMaximum(std::atomic<uint64_t>& destination, uint64_t value)
{
  auto current = destination.load(std::memory_order_relaxed);
  while (current < value &&
         !destination.compare_exchange_weak(current, value,
                                            std::memory_order_relaxed)) {
  }
}

uint64_t
threadHash()
{
  return static_cast<uint64_t>(
    std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

struct ResourceSnapshot
{
  uint64_t monotonicRawNs = 0;
  uint64_t steadyNs = 0;
  uint64_t userCpuUs = 0;
  uint64_t systemCpuUs = 0;
  uint64_t threadCount = 0;
  int64_t maxRssKiB = 0;
};

uint64_t
timevalUs(const ::timeval& value)
{
  return static_cast<uint64_t>(value.tv_sec) * 1000000ULL +
         static_cast<uint64_t>(value.tv_usec);
}

uint64_t
countProcessThreads()
{
  std::unique_ptr<DIR, decltype(&::closedir)> directory(
    ::opendir("/proc/self/task"), &::closedir);
  if (!directory) {
    throw std::runtime_error("cannot open /proc/self/task");
  }
  uint64_t count = 0;
  while (const auto* entry = ::readdir(directory.get())) {
    if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') {
      ++count;
    }
  }
  return count;
}

ResourceSnapshot
captureResourceSnapshot()
{
  ::rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0) {
    throw std::runtime_error("getrusage failed");
  }
  return {
    nowRaw(),
    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count()),
    timevalUs(usage.ru_utime),
    timevalUs(usage.ru_stime),
    countProcessThreads(),
    usage.ru_maxrss,
  };
}

struct Options
{
  std::string mode;
  std::string syncPrefix;
  std::string nodePrefix;
  std::string peerId;
  std::string remotePeerId;
  std::string identity;
  std::string peerCertificate;
  std::string summary;
  std::string summarySchema = "spec136.peer-summary.v6";
  std::string runtimeProfile = "forced-fetch-v2";
  std::string deliverySamples;
  unsigned rate = 0;
  unsigned warmup = 0;
  unsigned measure = 0;
  unsigned drain = 0;
  bool pacerOnly = false;
  bool securityPreflight = false;
  bool profileOnly = false;
};

Options
parseOptions(int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    auto value = [&] {
      if (++i >= argc) {
        throw std::runtime_error("missing value for " + key);
      }
      return std::string(argv[i]);
    };
    if (key == "--mode") options.mode = value();
    else if (key == "--sync-prefix") options.syncPrefix = value();
    else if (key == "--node-prefix") options.nodePrefix = value();
    else if (key == "--peer-id") options.peerId = value();
    else if (key == "--remote-peer-id") options.remotePeerId = value();
    else if (key == "--identity") options.identity = value();
    else if (key == "--peer-certificate") options.peerCertificate = value();
    else if (key == "--summary") options.summary = value();
    else if (key == "--summary-schema") options.summarySchema = value();
    else if (key == "--runtime-profile") options.runtimeProfile = value();
    else if (key == "--delivery-samples") options.deliverySamples = value();
    else if (key == "--rate") options.rate = std::stoul(value());
    else if (key == "--warmup") options.warmup = std::stoul(value());
    else if (key == "--measure") options.measure = std::stoul(value());
    else if (key == "--drain") options.drain = std::stoul(value());
    else if (key == "--pacer-only") options.pacerOnly = true;
    else if (key == "--security-preflight") options.securityPreflight = true;
    else if (key == "--profile-only") options.profileOnly = true;
    else throw std::runtime_error("unknown argument: " + key);
  }
  if (static_cast<unsigned>(options.pacerOnly) +
        static_cast<unsigned>(options.securityPreflight) +
        static_cast<unsigned>(options.profileOnly) > 1) {
    throw std::runtime_error("preflight modes are mutually exclusive");
  }
  if (options.summarySchema != "spec136.peer-summary.v6" &&
      options.summarySchema != "spec140.peer-summary.v1" &&
      options.summarySchema != "spec142.peer-summary.v1" &&
      options.summarySchema != "spec143.peer-summary.v1") {
    throw std::runtime_error("unsupported summary schema");
  }
  if (options.summarySchema == "spec140.peer-summary.v1" &&
      options.deliverySamples.empty()) {
    throw std::runtime_error(
      "Spec 140 summary requires --delivery-samples");
  }
  const bool validMode =
    options.mode == "face-inline-rsa" || options.mode == "worker-rsa";
  const bool validProfile =
    options.runtimeProfile == "forced-fetch-v2" ||
    options.runtimeProfile == "ndnsf-v3";
  const bool valid = options.securityPreflight
    ? !options.identity.empty() && !options.peerCertificate.empty() &&
      !options.summary.empty()
    : options.profileOnly
    ? validMode && validProfile && !options.peerId.empty() &&
      !options.summary.empty() && options.rate > 0
    : options.pacerOnly
    ? validMode && !options.peerId.empty() && !options.summary.empty() &&
      options.rate > 0 && options.measure > 0
    : validMode && validProfile &&
      !options.syncPrefix.empty() && !options.nodePrefix.empty() &&
      !options.peerId.empty() && !options.remotePeerId.empty() &&
      !options.identity.empty() && !options.peerCertificate.empty() &&
      !options.summary.empty() && options.rate > 0 && options.measure > 0 &&
      options.peerId != options.remotePeerId;
  if (!valid) {
    throw std::runtime_error("missing or invalid required argument");
  }
  return options;
}

uint16_t
adaptiveFetchWindow(unsigned rate)
{
  const auto scaled = static_cast<unsigned>(
    std::ceil(static_cast<double>(rate) * 0.64));
  return static_cast<uint16_t>(std::max(32U, std::min(128U, scaled)));
}

SVSPubSubOptions
makePubSubOptions(const Options& options)
{
  SVSPubSubOptions pubsubOptions;
  pubsubOptions.useTimestamp = false;
  pubsubOptions.publicationPreparationWorkers =
    options.mode == "worker-rsa" ? 1 : 0;
  pubsubOptions.publicationPreparationQueueCapacity = WORKER_QUEUE_CAPACITY;
  if (options.runtimeProfile == "ndnsf-v3") {
    pubsubOptions.maxApplicationParametersSize =
      NDNSF_MAX_APPLICATION_PARAMETERS_SIZE;
    pubsubOptions.maxPiggyDataSize = NDNSF_MAX_PIGGY_DATA_SIZE;
    pubsubOptions.mappingFetchWindow = NDNSF_MAPPING_FETCH_WINDOW;
    pubsubOptions.mappingFetchRetries = 0;
    pubsubOptions.mappingFetchFailureBackoff = 200_ms;
    pubsubOptions.publicationFetchWindow = adaptiveFetchWindow(options.rate);
    pubsubOptions.publicationFetchRetries = 2;
    pubsubOptions.publicationFetchInnerRetries = 2;
    pubsubOptions.publicationFetchInterestLifetime = 500_ms;
    pubsubOptions.publicationFetchMinInterestLifetime = 250_ms;
    pubsubOptions.publicationFetchMaxInterestLifetime = 2_s;
    pubsubOptions.publicationFetchFailureBackoff = 50_ms;
    pubsubOptions.publicationFetchMaxBackoff = 2_s;
    pubsubOptions.syncProtocol.version = SvsProtocolVersion::V3;
    pubsubOptions.syncProtocol.syncInterestLifetime = 1_s;
    pubsubOptions.syncProtocol.suppressionPeriod = 1_ms;
    pubsubOptions.syncProtocol.periodicTimeout = 30_s;
    pubsubOptions.syncProtocol.periodicJitter = 0.1;
  }
  else {
    pubsubOptions.maxPiggyDataSize = 1;
    pubsubOptions.publicationFetchWindow = PUBLICATION_FETCH_WINDOW;
    pubsubOptions.syncProtocol.version = SvsProtocolVersion::V2;
    pubsubOptions.syncProtocol.suppressionPeriod = 5_ms;
    pubsubOptions.syncProtocol.periodicTimeout = 1_s;
    pubsubOptions.syncProtocol.periodicJitter = 0.1;
  }
  return pubsubOptions;
}

void
writeProfileFields(std::ostream& output, const Options& options,
                   const SVSPubSubOptions& configured,
                   const ResolvedSyncProtocolOptions& resolved)
{
  const bool ndnsf = options.runtimeProfile == "ndnsf-v3";
  output
    << "  \"runtimeProfile\": \"" << options.runtimeProfile << "\",\n"
    << "  \"protocolVersion\": " << static_cast<unsigned>(resolved.version) << ",\n"
    << "  \"syncInterestLifetimeMs\": "
    << resolved.syncInterestLifetime.count() << ",\n"
    << "  \"syncSuppressionMs\": " << resolved.suppressionPeriod.count() << ",\n"
    << "  \"periodicSyncMs\": " << resolved.periodicTimeout.count() << ",\n"
    << "  \"useTimestamp\": " << (configured.useTimestamp ? "true" : "false") << ",\n"
    << "  \"applicationPayloadBytes\": " << PAYLOAD_SIZE << ",\n"
    << "  \"maxPiggyDataSize\": " << configured.maxPiggyDataSize << ",\n"
    << "  \"maxApplicationParametersSize\": "
    << configured.maxApplicationParametersSize << ",\n"
    << "  \"mappingFetchWindow\": " << configured.mappingFetchWindow << ",\n"
    << "  \"mappingFetchRetries\": " << configured.mappingFetchRetries << ",\n"
    << "  \"mappingFetchFailureBackoffMs\": "
    << configured.mappingFetchFailureBackoff.count() << ",\n"
    << "  \"publicationFetchWindow\": "
    << configured.publicationFetchWindow << ",\n"
    << "  \"publicationFetchRetries\": "
    << configured.publicationFetchRetries << ",\n"
    << "  \"publicationFetchInnerRetries\": "
    << configured.publicationFetchInnerRetries << ",\n"
    << "  \"publicationFetchInterestLifetimeMs\": "
    << configured.publicationFetchInterestLifetime.count() << ",\n"
    << "  \"publicationFetchMinInterestLifetimeMs\": "
    << configured.publicationFetchMinInterestLifetime.count() << ",\n"
    << "  \"publicationFetchMaxInterestLifetimeMs\": "
    << configured.publicationFetchMaxInterestLifetime.count() << ",\n"
    << "  \"publicationFetchFailureBackoffMs\": "
    << configured.publicationFetchFailureBackoff.count() << ",\n"
    << "  \"publicationFetchMaxBackoffMs\": "
    << configured.publicationFetchMaxBackoff.count() << ",\n"
    << "  \"parallelSyncProcessing\": " << (ndnsf ? "true" : "false") << ",\n"
    << "  \"parallelSyncProcessingWorkers\": " << (ndnsf ? NDNSF_SYNC_WORKERS : 0) << ",\n"
    << "  \"parallelSyncProcessingQueue\": " << (ndnsf ? NDNSF_SYNC_QUEUE : 0) << ",\n"
    << "  \"parallelSyncProduction\": " << (ndnsf ? "true" : "false") << ",\n"
    << "  \"parallelSyncProductionWorkers\": " << (ndnsf ? NDNSF_SYNC_WORKERS : 0) << ",\n"
    << "  \"parallelSyncProductionQueue\": " << (ndnsf ? NDNSF_SYNC_QUEUE : 0) << ",\n"
    << "  \"parallelSyncProductionSigning\": false,\n"
    << "  \"parallelSyncProductionExtraBlock\": "
    << (ndnsf ? "true" : "false") << ",\n"
    << "  \"syncInterestBatching\": " << (ndnsf ? "false" : "true") << ",\n"
    << "  \"syncInterestBatchWindowMs\": "
    << (ndnsf ? 0 : SYNC_BATCH_WINDOW_MS) << ",\n"
    << "  \"publicationWorkers\": "
    << configured.publicationPreparationWorkers << ",\n"
    << "  \"publicationWorkerQueueCapacity\": "
    << configured.publicationPreparationQueueCapacity << ",\n";
}

int
runProfileOnly(const Options& options)
{
  const auto configured = makePubSubOptions(options);
  const auto resolved = configured.syncProtocol.resolve();
  std::ofstream output(options.summary, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot write profile-only summary");
  }
  output << "{\n"
         << "  \"schema\": \"" << options.summarySchema << "\",\n"
         << "  \"peer\": \"" << options.peerId << "\",\n"
         << "  \"mode\": \"" << options.mode << "\",\n"
         << "  \"ratePerPeer\": " << options.rate << ",\n";
  writeProfileFields(output, options, configured, resolved);
  output << "  \"profileOnly\": true\n}\n";
  return 0;
}

struct SignerSnapshot
{
  uint64_t dataCalls = 0;
  uint64_t dataNs = 0;
  uint64_t dataWaitNs = 0;
  uint64_t dataServiceNs = 0;
  uint64_t interestCalls = 0;
  uint64_t interestNs = 0;
  uint64_t interestWaitNs = 0;
  uint64_t interestServiceNs = 0;
  uint64_t maxActive = 0;
};

class TimedRsaSigner final : public BaseSigner
{
public:
  TimedRsaSigner(KeyChain& keyChain, const security::pib::Identity& identity)
    : m_delegate(keyChain)
  {
    signingInfo = security::signingByIdentity(identity);
    signingInfo.setSignedInterestFormat(security::SignedInterestFormat::V03);
    m_delegate.signingInfo = signingInfo;
  }

  void
  sign(Data& data) const final
  {
    const auto started = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto acquired = std::chrono::steady_clock::now();
    const auto active = m_active.fetch_add(1, std::memory_order_relaxed) + 1;
    updateMaximum(m_maxActive, active);
    try {
      m_delegate.sign(data);
    }
    catch (...) {
      m_active.fetch_sub(1, std::memory_order_relaxed);
      throw;
    }
    m_active.fetch_sub(1, std::memory_order_relaxed);
    const auto completed = std::chrono::steady_clock::now();
    lock.unlock();
    const auto elapsed = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(completed - started).count());
    const auto wait = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(acquired - started).count());
    const auto service = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(completed - acquired).count());
    if (data.getSignatureInfo().getSignatureType() != ndn::tlv::SignatureSha256WithRsa) {
      throw std::runtime_error("publication Data is not RSA signed");
    }
    m_dataCalls.fetch_add(1, std::memory_order_relaxed);
    m_dataNs.fetch_add(elapsed, std::memory_order_relaxed);
    m_dataWaitNs.fetch_add(wait, std::memory_order_relaxed);
    m_dataServiceNs.fetch_add(service, std::memory_order_relaxed);
  }

  void
  sign(Interest& interest) const final
  {
    const auto started = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto acquired = std::chrono::steady_clock::now();
    const auto active = m_active.fetch_add(1, std::memory_order_relaxed) + 1;
    updateMaximum(m_maxActive, active);
    try {
      m_delegate.sign(interest);
    }
    catch (...) {
      m_active.fetch_sub(1, std::memory_order_relaxed);
      throw;
    }
    m_active.fetch_sub(1, std::memory_order_relaxed);
    const auto completed = std::chrono::steady_clock::now();
    lock.unlock();
    const auto elapsed = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(completed - started).count());
    const auto wait = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(acquired - started).count());
    const auto service = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(completed - acquired).count());
    const auto info = interest.getSignatureInfo();
    if (!info) {
      throw std::runtime_error("Sync Interest has no v0.3 SignatureInfo");
    }
    if (info->getSignatureType() != ndn::tlv::SignatureSha256WithRsa) {
      throw std::runtime_error(
        "Sync Interest signature type is not SignatureSha256WithRsa");
    }
    m_interestCalls.fetch_add(1, std::memory_order_relaxed);
    m_interestNs.fetch_add(elapsed, std::memory_order_relaxed);
    m_interestWaitNs.fetch_add(wait, std::memory_order_relaxed);
    m_interestServiceNs.fetch_add(service, std::memory_order_relaxed);
  }

  SignerSnapshot
  snapshot() const
  {
    return {
      m_dataCalls.load(std::memory_order_relaxed),
      m_dataNs.load(std::memory_order_relaxed),
      m_dataWaitNs.load(std::memory_order_relaxed),
      m_dataServiceNs.load(std::memory_order_relaxed),
      m_interestCalls.load(std::memory_order_relaxed),
      m_interestNs.load(std::memory_order_relaxed),
      m_interestWaitNs.load(std::memory_order_relaxed),
      m_interestServiceNs.load(std::memory_order_relaxed),
      m_maxActive.load(std::memory_order_relaxed),
    };
  }

private:
  mutable KeyChainSigner m_delegate;
  mutable std::mutex m_mutex;
  mutable std::atomic<uint64_t> m_dataCalls{0};
  mutable std::atomic<uint64_t> m_dataNs{0};
  mutable std::atomic<uint64_t> m_dataWaitNs{0};
  mutable std::atomic<uint64_t> m_dataServiceNs{0};
  mutable std::atomic<uint64_t> m_interestCalls{0};
  mutable std::atomic<uint64_t> m_interestNs{0};
  mutable std::atomic<uint64_t> m_interestWaitNs{0};
  mutable std::atomic<uint64_t> m_interestServiceNs{0};
  mutable std::atomic<uint64_t> m_active{0};
  mutable std::atomic<uint64_t> m_maxActive{0};
};

struct ValidatorSnapshot
{
  uint64_t dataValid = 0;
  uint64_t dataInvalid = 0;
  uint64_t dataNs = 0;
  uint64_t interestValid = 0;
  uint64_t interestInvalid = 0;
  uint64_t interestNs = 0;
};

class FixedCertificateValidator final : public BaseValidator
{
public:
  explicit
  FixedCertificateValidator(const std::string& peerCertificatePath,
                            const security::Certificate& ownCertificate)
  {
    auto loaded = io::load<security::Certificate>(peerCertificatePath);
    if (!loaded) {
      throw std::runtime_error("cannot load peer certificate");
    }
    m_certificates.push_back(*loaded);
    m_certificates.push_back(ownCertificate);
  }

  void
  validate(const Data& data,
           const security::DataValidationSuccessCallback& success,
           const security::DataValidationFailureCallback& failure) final
  {
    const auto started = std::chrono::steady_clock::now();
    const bool valid =
      data.getSignatureInfo().getSignatureType() == ndn::tlv::SignatureSha256WithRsa &&
      std::any_of(m_certificates.begin(), m_certificates.end(),
                  [&data] (const auto& certificate) {
                    return security::verifySignature(
                      data, std::optional<security::Certificate>(certificate));
                  });
    m_dataNs.fetch_add(static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count()),
      std::memory_order_relaxed);
    if (valid) {
      m_dataValid.fetch_add(1, std::memory_order_relaxed);
      success(data);
    }
    else {
      m_dataInvalid.fetch_add(1, std::memory_order_relaxed);
      failure(data, security::ValidationError(1, "fixed peer RSA validation failed"));
    }
  }

  void
  validate(const Interest& interest,
           const security::InterestValidationSuccessCallback& success,
           const security::InterestValidationFailureCallback& failure) final
  {
    const auto started = std::chrono::steady_clock::now();
    const auto info = interest.getSignatureInfo();
    const bool valid =
      info && info->getSignatureType() == ndn::tlv::SignatureSha256WithRsa &&
      std::any_of(m_certificates.begin(), m_certificates.end(),
                  [&interest] (const auto& certificate) {
                    return security::verifySignature(
                      interest, std::optional<security::Certificate>(certificate));
                  });
    m_interestNs.fetch_add(static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count()),
      std::memory_order_relaxed);
    if (valid) {
      m_interestValid.fetch_add(1, std::memory_order_relaxed);
      success(interest);
    }
    else {
      m_interestInvalid.fetch_add(1, std::memory_order_relaxed);
      failure(interest, security::ValidationError(1, "fixed peer RSA validation failed"));
    }
  }

  ValidatorSnapshot
  snapshot() const
  {
    return {
      m_dataValid.load(std::memory_order_relaxed),
      m_dataInvalid.load(std::memory_order_relaxed),
      m_dataNs.load(std::memory_order_relaxed),
      m_interestValid.load(std::memory_order_relaxed),
      m_interestInvalid.load(std::memory_order_relaxed),
      m_interestNs.load(std::memory_order_relaxed),
    };
  }

private:
  std::vector<security::Certificate> m_certificates;
  std::atomic<uint64_t> m_dataValid{0};
  std::atomic<uint64_t> m_dataInvalid{0};
  std::atomic<uint64_t> m_dataNs{0};
  std::atomic<uint64_t> m_interestValid{0};
  std::atomic<uint64_t> m_interestInvalid{0};
  std::atomic<uint64_t> m_interestNs{0};
};

int
runSecurityPreflight(const Options& options)
{
  KeyChain keyChain;
  auto identity =
    keyChain.createIdentity(Name(options.identity), RsaKeyParams(2048));
  TimedRsaSigner signer(keyChain, identity);
  FixedCertificateValidator validator(
    options.peerCertificate,
    identity.getDefaultKey().getDefaultCertificate());

  const std::vector<uint8_t> validPayload{'S', 'V', 'S', '1', '3', '6'};
  auto tamperedPayload = validPayload;
  tamperedPayload.back() ^= 0x5a;

  Data validData(Name("/spec136/security/data"));
  validData.setContent(make_span(validPayload.data(), validPayload.size()));
  signer.sign(validData);
  Data tamperedData(validData);
  tamperedData.setContent(
    make_span(tamperedPayload.data(), tamperedPayload.size()));

  Interest validInterest(Name("/spec136/security/interest"));
  validInterest.setCanBePrefix(false);
  signer.sign(validInterest);
  Interest tamperedInterest(validInterest);
  tamperedInterest.setName(
    Name(validInterest.getName()).append("tampered-after-signing"));

  bool validDataAccepted = false;
  bool tamperedDataAccepted = false;
  bool tamperedDataRejected = false;
  bool validInterestAccepted = false;
  bool tamperedInterestAccepted = false;
  bool tamperedInterestRejected = false;
  validator.validate(
    validData,
    [&] (const Data&) { validDataAccepted = true; },
    [] (const Data&, const security::ValidationError&) {});
  validator.validate(
    tamperedData,
    [&] (const Data&) { tamperedDataAccepted = true; },
    [&] (const Data&, const security::ValidationError&) {
      tamperedDataRejected = true;
    });
  validator.validate(
    validInterest,
    [&] (const Interest&) { validInterestAccepted = true; },
    [] (const Interest&, const security::ValidationError&) {});
  validator.validate(
    tamperedInterest,
    [&] (const Interest&) { tamperedInterestAccepted = true; },
    [&] (const Interest&, const security::ValidationError&) {
      tamperedInterestRejected = true;
    });

  const auto validInterestInfo = validInterest.getSignatureInfo();
  const bool dataRsa =
    validData.getSignatureInfo().getSignatureType() ==
    ndn::tlv::SignatureSha256WithRsa;
  const bool interestRsa =
    validInterestInfo &&
    validInterestInfo->getSignatureType() == ndn::tlv::SignatureSha256WithRsa;
  const auto stats = validator.snapshot();
  const bool passed =
    dataRsa && interestRsa && validDataAccepted && validInterestAccepted &&
    tamperedDataRejected && tamperedInterestRejected &&
    !tamperedDataAccepted && !tamperedInterestAccepted &&
    stats.dataValid == 1 && stats.dataInvalid == 1 &&
    stats.interestValid == 1 && stats.interestInvalid == 1;

  std::ofstream output(options.summary);
  if (!output) {
    throw std::runtime_error("cannot write security preflight summary");
  }
  output
    << "{\n"
    << "  \"schema\": \"spec136.security-preflight.v1\",\n"
    << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
    << "  \"dataSignatureType\": "
    << validData.getSignatureInfo().getSignatureType() << ",\n"
    << "  \"interestSignatureType\": "
    << (validInterestInfo ? validInterestInfo->getSignatureType() : 0) << ",\n"
    << "  \"validDataAccepted\": "
    << (validDataAccepted ? "true" : "false") << ",\n"
    << "  \"validInterestAccepted\": "
    << (validInterestAccepted ? "true" : "false") << ",\n"
    << "  \"tamperedDataRejected\": "
    << (tamperedDataRejected ? "true" : "false") << ",\n"
    << "  \"tamperedInterestRejected\": "
    << (tamperedInterestRejected ? "true" : "false") << ",\n"
    << "  \"tamperedDataReachedProcessing\": "
    << (tamperedDataAccepted ? "true" : "false") << ",\n"
    << "  \"tamperedInterestReachedProcessing\": "
    << (tamperedInterestAccepted ? "true" : "false") << ",\n"
    << "  \"dataValid\": " << stats.dataValid << ",\n"
    << "  \"dataInvalid\": " << stats.dataInvalid << ",\n"
    << "  \"interestValid\": " << stats.interestValid << ",\n"
    << "  \"interestInvalid\": " << stats.interestInvalid << "\n"
    << "}\n";
  return passed ? 0 : 1;
}

class NoOpPacer
{
public:
  explicit
  NoOpPacer(Options options)
    : m_options(std::move(options))
  {
  }

  int
  run()
  {
    m_faceThreadHash = threadHash();
    auto guard = boost::asio::make_work_guard(m_io);
    std::thread pacer([this, &guard] {
      m_pacerThreadHash.store(threadHash(), std::memory_order_relaxed);
      const auto period = std::chrono::nanoseconds(NS / m_options.rate);
      const auto start = std::chrono::steady_clock::now() + 100ms;
      const auto measuredStart =
        start + std::chrono::seconds(m_options.warmup);
      const auto measuredEnd =
        measuredStart + std::chrono::seconds(m_options.measure);
      auto deadline = start;
      while (deadline < measuredEnd) {
        std::this_thread::sleep_until(deadline);
        if (std::chrono::steady_clock::now() >= measuredEnd) {
          break;
        }
        const bool measured = deadline >= measuredStart;
        if (measured) {
          m_scheduledMeasured.fetch_add(1, std::memory_order_relaxed);
        }
        if (m_options.mode == "face-inline-rsa") {
          boost::asio::post(m_io, [this, measured] {
            recordCall(measured);
          });
        }
        else {
          recordCall(measured);
        }
        deadline += period;
      }
      guard.reset();
    });
    m_io.run();
    pacer.join();

    const auto scheduled =
      m_scheduledMeasured.load(std::memory_order_relaxed);
    const auto attempted =
      m_attemptedMeasured.load(std::memory_order_relaxed);
    const auto expected =
      static_cast<uint64_t>(m_options.rate) * m_options.measure;
    const double attemptedRate =
      static_cast<double>(attempted) / m_options.measure;
    const bool passed =
      scheduled == expected && attempted >= expected * 0.98 &&
      attempted <= expected * 1.02 &&
      m_faceThreadHash != 0 &&
      m_pacerThreadHash.load(std::memory_order_relaxed) != 0 &&
      m_faceThreadHash != m_pacerThreadHash.load(std::memory_order_relaxed) &&
      ((m_options.mode == "face-inline-rsa" &&
        m_publishCallThreadHash.load(std::memory_order_relaxed) ==
          m_faceThreadHash &&
        m_publishCallsOnFace.load(std::memory_order_relaxed) == expected &&
        m_publishCallsOnPacer.load(std::memory_order_relaxed) == 0) ||
       (m_options.mode == "worker-rsa" &&
        m_publishCallThreadHash.load(std::memory_order_relaxed) ==
          m_pacerThreadHash.load(std::memory_order_relaxed) &&
        m_publishCallsOnPacer.load(std::memory_order_relaxed) == expected &&
        m_publishCallsOnFace.load(std::memory_order_relaxed) == 0));

    std::ofstream output(m_options.summary);
    if (!output) {
      throw std::runtime_error("cannot write no-op pacer summary");
    }
    output
      << "{\n"
      << "  \"schema\": \"spec136.noop-pacer.v1\",\n"
      << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
      << "  \"peerId\": \"" << m_options.peerId << "\",\n"
      << "  \"mode\": \"" << m_options.mode << "\",\n"
      << "  \"ratePerPeer\": " << m_options.rate << ",\n"
      << "  \"measureSeconds\": " << m_options.measure << ",\n"
      << "  \"scheduledMeasured\": " << scheduled << ",\n"
      << "  \"attemptedMeasured\": " << attempted << ",\n"
      << "  \"attemptedPps\": " << attemptedRate << ",\n"
      << "  \"faceThreadHash\": " << m_faceThreadHash << ",\n"
      << "  \"pacerThreadHash\": "
      << m_pacerThreadHash.load(std::memory_order_relaxed) << ",\n"
      << "  \"publishCallThreadHash\": "
      << m_publishCallThreadHash.load(std::memory_order_relaxed) << ",\n"
      << "  \"publishCallsOnFace\": "
      << m_publishCallsOnFace.load(std::memory_order_relaxed) << ",\n"
      << "  \"publishCallsOnPacer\": "
      << m_publishCallsOnPacer.load(std::memory_order_relaxed) << ",\n"
      << "  \"rsaSignCalls\": 0,\n"
      << "  \"ndnPublications\": 0,\n"
      << "  \"syncInterests\": 0,\n"
      << "  \"fetches\": 0\n"
      << "}\n";
    return passed ? 0 : 1;
  }

private:
  void
  recordCall(bool measured)
  {
    const auto current = threadHash();
    m_publishCallThreadHash.store(current, std::memory_order_relaxed);
    if (current == m_faceThreadHash) {
      m_publishCallsOnFace.fetch_add(measured ? 1 : 0, std::memory_order_relaxed);
    }
    if (current == m_pacerThreadHash.load(std::memory_order_relaxed)) {
      m_publishCallsOnPacer.fetch_add(measured ? 1 : 0, std::memory_order_relaxed);
    }
    if (measured) {
      m_attemptedMeasured.fetch_add(1, std::memory_order_relaxed);
    }
  }

private:
  Options m_options;
  boost::asio::io_context m_io;
  uint64_t m_faceThreadHash = 0;
  std::atomic<uint64_t> m_pacerThreadHash{0};
  std::atomic<uint64_t> m_publishCallThreadHash{0};
  std::atomic<uint64_t> m_publishCallsOnFace{0};
  std::atomic<uint64_t> m_publishCallsOnPacer{0};
  std::atomic<uint64_t> m_scheduledMeasured{0};
  std::atomic<uint64_t> m_attemptedMeasured{0};
};

class Peer
{
public:
  explicit
  Peer(Options options)
    : m_options(std::move(options))
    , m_configuredOptions(makePubSubOptions(m_options))
    , m_heartbeatTimer(m_face.getIoContext())
    , m_ownHash(fnv1a(m_options.peerId))
    , m_remoteHash(fnv1a(m_options.remotePeerId))
  {
    auto identity = m_keyChain.createIdentity(Name(m_options.identity), RsaKeyParams(2048));
    m_signer = std::make_shared<TimedRsaSigner>(m_keyChain, identity);
    m_validator = std::make_shared<FixedCertificateValidator>(
      m_options.peerCertificate,
      identity.getDefaultKey().getDefaultCertificate());

    SecurityOptions security(m_keyChain);
    security.interestSigner = m_signer;
    security.dataSigner = m_signer;
    security.pubSigner = m_signer;
    security.validator = m_validator;
    security.encapsulatedDataValidator = m_validator;

    m_pubsub = std::make_unique<SVSPubSub>(
      Name(m_options.syncPrefix), Name(m_options.nodePrefix), m_face,
      [] (const std::vector<MissingDataInfo>&) {}, m_configuredOptions, security);
    if (m_options.runtimeProfile == "ndnsf-v3") {
      m_pubsub->getSVSync().getCore().setParallelSyncProcessing(
        true, NDNSF_SYNC_WORKERS, NDNSF_SYNC_QUEUE);
      m_pubsub->getSVSync().getCore().setParallelSyncProduction(
        true, NDNSF_SYNC_WORKERS, NDNSF_SYNC_QUEUE, false, true);
      m_pubsub->getSVSync().getCore().setSyncInterestBatching(false);
    }
    else {
      m_pubsub->getSVSync().getCore().setParallelSyncProcessing(false);
      m_pubsub->getSVSync().getCore().setParallelSyncProduction(false);
      m_pubsub->getSVSync().getCore().setSyncInterestBatching(
        true, ndn::time::milliseconds(SYNC_BATCH_WINDOW_MS));
    }
    m_pubsub->subscribe(Name("/spec136/publication").append(m_options.remotePeerId),
                        [this] (const auto& data) { onDelivery(data); });
  }

  int
  run()
  {
    m_faceThreadHash = threadHash();
    m_start = std::chrono::steady_clock::now() + 2s;
    m_measuredStart = m_start + std::chrono::seconds(m_options.warmup);
    m_measuredEnd = m_measuredStart + std::chrono::seconds(m_options.measure);
    m_stopAt = m_measuredEnd + std::chrono::seconds(m_options.drain);
    m_nextHeartbeat = m_start;

    std::cout << "SPEC136_READY peer=" << m_options.peerId
              << " mode=" << m_options.mode
              << " publication_workers="
              << m_pubsub->getPublicationPreparationWorkerCount()
              << " data_signature_type=" << ndn::tlv::SignatureSha256WithRsa
              << " interest_signature_type="
              << (m_options.runtimeProfile == "ndnsf-v3"
                    ? 0 : ndn::tlv::SignatureSha256WithRsa)
              << " sync_envelope_signature_type="
              << ndn::tlv::SignatureSha256WithRsa
              << std::endl;

    m_pacer = std::thread([this] { runPacer(); });
    scheduleHeartbeat();
    m_face.processEvents();
    joinPacer();
    m_dispatchOpen.store(false, std::memory_order_relaxed);
    m_faceDispatchAbandoned.store(
      m_faceDispatchPending.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
    try {
      writeSummary();
    }
    catch (const std::exception& e) {
      std::cerr << "SPEC136_SUMMARY_ERROR " << e.what() << std::endl;
      m_exitCode = 2;
    }
    m_face.shutdown();
    return m_exitCode;
  }

private:
  std::vector<uint8_t>
  makePayload(uint64_t logicalId, uint8_t phase, uint64_t sentRawNs) const
  {
    std::vector<uint8_t> payload(PAYLOAD_SIZE);
    const uint8_t magic[8] = {'S', 'V', 'S', '1', '3', '6', 0, 0};
    std::memcpy(payload.data(), magic, sizeof(magic));
    payload[8] = phase;
    const auto sender = hostToBe64(fnv1a(m_options.peerId));
    const auto id = hostToBe64(logicalId);
    const auto sent = hostToBe64(sentRawNs);
    std::memcpy(payload.data() + 16, &sender, sizeof(sender));
    std::memcpy(payload.data() + 24, &id, sizeof(id));
    std::memcpy(payload.data() + 32, &sent, sizeof(sent));
    uint64_t state = sender ^ id ^ sent;
    for (size_t i = 40; i < payload.size(); ++i) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      payload[i] = static_cast<uint8_t>(state >> 56);
    }
    return payload;
  }

  void
  recordPublishCallThread()
  {
    const auto current = threadHash();
    m_publishCallThreadHash.store(current, std::memory_order_relaxed);
    if (current == m_faceThreadHash) {
      m_publishCallsOnFace.fetch_add(1, std::memory_order_relaxed);
    }
    if (current == m_pacerThreadHash.load(std::memory_order_relaxed)) {
      m_publishCallsOnPacer.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void
  invokePublication(Name name, std::vector<uint8_t> payload, bool measured)
  {
    recordPublishCallThread();
    try {
      m_pubsub->publishAsync(name, payload);
      if (measured) {
        m_acceptedMeasured.fetch_add(1, std::memory_order_relaxed);
      }
    }
    catch (const std::exception&) {
      if (measured) {
        m_publishErrors.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  void
  releasePublication(uint64_t logicalId, bool measured)
  {
    auto name =
      Name("/spec136/publication").append(m_options.peerId).appendNumber(logicalId);
    auto payload = makePayload(logicalId, measured ? 1 : 0, nowRaw());
    if (m_options.mode == "face-inline-rsa") {
      m_faceDispatchPending.fetch_add(1, std::memory_order_relaxed);
      boost::asio::post(
        m_face.getIoContext(),
        [this, name = std::move(name), payload = std::move(payload), measured] () mutable {
          m_faceDispatchPending.fetch_sub(1, std::memory_order_relaxed);
          if (!m_dispatchOpen.load(std::memory_order_relaxed)) {
            return;
          }
          invokePublication(std::move(name), std::move(payload), measured);
        });
    }
    else {
      invokePublication(std::move(name), std::move(payload), measured);
    }
  }

  void
  runPacer()
  {
    try {
      m_pacerThreadHash.store(threadHash(), std::memory_order_relaxed);
      const auto period = std::chrono::nanoseconds(NS / m_options.rate);
      auto deadline = m_start;
      while (deadline < m_measuredEnd) {
        std::this_thread::sleep_until(deadline);
        const auto releasedAt = std::chrono::steady_clock::now();
        if (releasedAt >= m_measuredEnd) {
          break;
        }
        const bool measured = deadline >= m_measuredStart;
        if (measured) {
          if (!m_measureStartCaptured) {
            m_resourceAtMeasureStart = captureResourceSnapshot();
            m_measureStartSigner = m_signer->snapshot();
            m_publicationFetchAtMeasureStart = m_pubsub->getPublicationFetchStats();
            m_mappingFetchAtMeasureStart = m_pubsub->getMappingFetchStats();
            m_piggybackAtMeasureStart = m_pubsub->getPiggybackStats();
            m_measureStartCaptured = true;
          }
          m_attemptedMeasured.fetch_add(1, std::memory_order_relaxed);
          m_releaseLateness.push_back(releasedAt > deadline
            ? static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                  releasedAt - deadline).count())
            : 0);
        }
        releasePublication(++m_logicalId, measured);
        deadline += period;
      }
      std::this_thread::sleep_until(m_measuredEnd);
      m_measureEndPreparation = m_pubsub->getPublicationPreparationStats();
      m_publicationFetchAtMeasureEnd = m_pubsub->getPublicationFetchStats();
      m_mappingFetchAtMeasureEnd = m_pubsub->getMappingFetchStats();
      m_piggybackAtMeasureEnd = m_pubsub->getPiggybackStats();
      m_measureEndSigner = m_signer->snapshot();
      m_measureEndValidator = m_validator->snapshot();
      m_resourceAtMeasureEnd = captureResourceSnapshot();
      m_faceDispatchPendingAtMeasureEnd =
        m_faceDispatchPending.load(std::memory_order_relaxed);
      m_deliveredWarmupAtMeasureEnd =
        m_deliveredWarmup.load(std::memory_order_relaxed);
      m_deliveredMeasuredAtMeasureEnd =
        m_deliveredMeasured.load(std::memory_order_relaxed);
    }
    catch (const std::exception& e) {
      {
        std::lock_guard<std::mutex> lock(m_pacerErrorMutex);
        m_pacerError = e.what();
      }
      m_pacerFailed.store(true, std::memory_order_relaxed);
    }
    catch (...) {
      {
        std::lock_guard<std::mutex> lock(m_pacerErrorMutex);
        m_pacerError = "unknown pacer failure";
      }
      m_pacerFailed.store(true, std::memory_order_relaxed);
    }
    std::this_thread::sleep_until(m_stopAt + 100ms);
    m_dispatchOpen.store(false, std::memory_order_relaxed);
    m_face.getIoContext().stop();
  }

  void
  joinPacer()
  {
    if (m_pacer.joinable()) {
      m_pacer.join();
    }
  }

  void
  scheduleHeartbeat()
  {
    if (m_nextHeartbeat >= m_stopAt) {
      return;
    }
    m_heartbeatTimer.expires_at(m_nextHeartbeat);
    m_heartbeatTimer.async_wait([this] (const auto& error) {
      if (error) {
        return;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= m_measuredStart && now < m_measuredEnd) {
        m_heartbeatLateness.push_back(now > m_nextHeartbeat
          ? static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - m_nextHeartbeat).count())
          : 0);
      }
      m_nextHeartbeat += 1ms;
      if (m_nextHeartbeat <= now) {
        const auto missed =
          static_cast<uint64_t>((now - m_nextHeartbeat) / 1ms) + 1;
        m_nextHeartbeat += 1ms * missed;
        if (now >= m_measuredStart && now < m_measuredEnd) {
          m_heartbeatSkipped += missed;
        }
      }
      scheduleHeartbeat();
    });
  }

  void
  onDelivery(const SVSPubSub::SubscriptionData& data)
  {
    if (data.data.size() != PAYLOAD_SIZE ||
        std::memcmp(data.data.data(), "SVS136", 6) != 0) {
      ++m_invalid;
      return;
    }
    uint64_t senderBe = 0;
    uint64_t idBe = 0;
    uint64_t sentBe = 0;
    std::memcpy(&senderBe, data.data.data() + 16, sizeof(senderBe));
    std::memcpy(&idBe, data.data.data() + 24, sizeof(idBe));
    std::memcpy(&sentBe, data.data.data() + 32, sizeof(sentBe));
    const auto sender = beToHost64(senderBe);
    const auto logicalId = beToHost64(idBe);
    const auto sentRaw = beToHost64(sentBe);
    if (sender == m_ownHash) {
      ++m_selfDeliveries;
      return;
    }
    if (sender != m_remoteHash) {
      ++m_invalid;
      return;
    }
    const uint64_t key = sender ^ (logicalId * 0x9e3779b97f4a7c15ULL);
    if (!m_seen.insert(key).second) {
      ++m_duplicates;
      return;
    }
    if (data.data[8] == 0) {
      m_deliveredWarmup.fetch_add(1, std::memory_order_relaxed);
    }
    else if (data.data[8] == 1) {
      m_deliveredMeasured.fetch_add(1, std::memory_order_relaxed);
      const auto receivedRaw = nowRaw();
      if (receivedRaw >= sentRaw) {
        m_deliveryDelay.push_back(receivedRaw - sentRaw);
      }
    }
    else {
      ++m_invalid;
    }
  }

  void
  writeSummary()
  {
    joinPacer();
    const auto preparation = m_pubsub->getPublicationPreparationStats();
    const auto piggyback = m_pubsub->getPiggybackStats();
    const auto publicationFetch = m_pubsub->getPublicationFetchStats();
    const auto mappingFetch = m_pubsub->getMappingFetchStats();
    const auto signer = m_signer->snapshot();
    const auto validator = m_validator->snapshot();
    ::rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);

    if (!m_options.deliverySamples.empty()) {
      std::ofstream samples(m_options.deliverySamples, std::ios::trunc);
      if (!samples) {
        throw std::runtime_error("cannot open delivery sample output");
      }
      samples << "latencyNs\n";
      for (const auto latency : m_deliveryDelay) {
        samples << latency << '\n';
      }
      if (!samples) {
        throw std::runtime_error("cannot write delivery sample output");
      }
    }

    std::ofstream output(m_options.summary, std::ios::trunc);
    if (!output) {
      throw std::runtime_error("cannot open summary output");
    }
    const auto scheduledMeasured =
      static_cast<uint64_t>(m_options.rate) * m_options.measure;
    const auto attemptedMeasured =
      m_attemptedMeasured.load(std::memory_order_relaxed);
    const auto skippedMeasured =
      scheduledMeasured > attemptedMeasured ? scheduledMeasured - attemptedMeasured : 0;
    const auto resourceWallNs =
      m_resourceAtMeasureEnd.monotonicRawNs >= m_resourceAtMeasureStart.monotonicRawNs
        ? m_resourceAtMeasureEnd.monotonicRawNs - m_resourceAtMeasureStart.monotonicRawNs
        : 0;
    const auto resourceUserCpuUs =
      m_resourceAtMeasureEnd.userCpuUs >= m_resourceAtMeasureStart.userCpuUs
        ? m_resourceAtMeasureEnd.userCpuUs - m_resourceAtMeasureStart.userCpuUs
        : 0;
    const auto resourceSystemCpuUs =
      m_resourceAtMeasureEnd.systemCpuUs >= m_resourceAtMeasureStart.systemCpuUs
        ? m_resourceAtMeasureEnd.systemCpuUs - m_resourceAtMeasureStart.systemCpuUs
        : 0;
    const auto resourceTotalCpuUs = resourceUserCpuUs + resourceSystemCpuUs;
    const double resourceCpuPctOneCore = resourceWallNs == 0
      ? 0.0
      : 100.0 * static_cast<double>(resourceTotalCpuUs) * 1000.0 /
          static_cast<double>(resourceWallNs);
    const double resourceCpuPctFourCore = resourceCpuPctOneCore / 4.0;
    std::string pacerError;
    {
      std::lock_guard<std::mutex> lock(m_pacerErrorMutex);
      pacerError = m_pacerError;
    }
    output
      << "{\n"
      << "  \"schema\": \"" << m_options.summarySchema << "\",\n"
      << "  \"peer\": \"" << m_options.peerId << "\",\n"
      << "  \"mode\": \"" << m_options.mode << "\",\n"
      << "  \"ratePerPeer\": " << m_options.rate << ",\n"
      << "  \"warmupSeconds\": " << m_options.warmup << ",\n"
      << "  \"measureSeconds\": " << m_options.measure << ",\n"
      << "  \"drainSeconds\": " << m_options.drain << ",\n"
      ;
    writeProfileFields(output, m_options, m_configuredOptions,
                       m_pubsub->getSyncProtocolOptions());
    output
      << "  \"dataSignatureType\": "
      << static_cast<uint32_t>(ndn::tlv::SignatureSha256WithRsa) << ",\n"
      << "  \"interestSignatureType\": "
      << (m_options.runtimeProfile == "ndnsf-v3"
            ? 0 : static_cast<uint32_t>(ndn::tlv::SignatureSha256WithRsa))
      << ",\n"
      << "  \"syncEnvelopeSignatureType\": "
      << static_cast<uint32_t>(ndn::tlv::SignatureSha256WithRsa) << ",\n"
      << "  \"syncInterestSigned\": "
      << (m_options.runtimeProfile == "ndnsf-v3" ? "false" : "true") << ",\n"
      << "  \"pacerKind\": \"independent-app-thread\",\n"
      << "  \"faceThreadHash\": " << m_faceThreadHash << ",\n"
      << "  \"pacerThreadHash\": "
      << m_pacerThreadHash.load(std::memory_order_relaxed) << ",\n"
      << "  \"publishCallThreadHash\": "
      << m_publishCallThreadHash.load(std::memory_order_relaxed) << ",\n"
      << "  \"publishCallsOnFace\": "
      << m_publishCallsOnFace.load(std::memory_order_relaxed) << ",\n"
      << "  \"publishCallsOnPacer\": "
      << m_publishCallsOnPacer.load(std::memory_order_relaxed) << ",\n"
      << "  \"pacerFailed\": "
      << (m_pacerFailed.load(std::memory_order_relaxed) ? "true" : "false") << ",\n"
      << "  \"pacerError\": \"" << pacerError << "\",\n"
      << "  \"scheduledMeasured\": " << scheduledMeasured << ",\n"
      << "  \"attemptedMeasured\": " << attemptedMeasured << ",\n"
      << "  \"acceptedMeasured\": "
      << m_acceptedMeasured.load(std::memory_order_relaxed) << ",\n"
      << "  \"deliveredWarmup\": "
      << m_deliveredWarmup.load(std::memory_order_relaxed) << ",\n"
      << "  \"deliveredMeasured\": "
      << m_deliveredMeasured.load(std::memory_order_relaxed) << ",\n"
      << "  \"deliveredWarmupAtMeasureEnd\": "
      << m_deliveredWarmupAtMeasureEnd << ",\n"
      << "  \"deliveredMeasuredAtMeasureEnd\": "
      << m_deliveredMeasuredAtMeasureEnd << ",\n"
      << "  \"skippedMeasured\": " << skippedMeasured << ",\n"
      << "  \"publishErrors\": "
      << m_publishErrors.load(std::memory_order_relaxed) << ",\n"
      << "  \"faceDispatchPending\": "
      << m_faceDispatchPending.load(std::memory_order_relaxed) << ",\n"
      << "  \"faceDispatchAbandoned\": "
      << m_faceDispatchAbandoned.load(std::memory_order_relaxed) << ",\n"
      << "  \"faceDispatchPendingAtMeasureEnd\": "
      << m_faceDispatchPendingAtMeasureEnd << ",\n"
      << "  \"duplicates\": " << m_duplicates << ",\n"
      << "  \"invalid\": " << m_invalid << ",\n"
      << "  \"selfDeliveries\": " << m_selfDeliveries << ",\n"
      << "  \"releaseP99Ns\": " << percentile(m_releaseLateness, 99) << ",\n"
      << "  \"heartbeatP99Ns\": " << percentile(m_heartbeatLateness, 99) << ",\n"
      << "  \"heartbeatSkipped\": " << m_heartbeatSkipped << ",\n"
      << "  \"deliverySamples\": " << m_deliveryDelay.size() << ",\n"
      << "  \"deliveryMeanNs\": " << mean(m_deliveryDelay) << ",\n"
      << "  \"deliveryP50Ns\": " << percentile(m_deliveryDelay, 50) << ",\n"
      << "  \"deliveryP95Ns\": " << percentile(m_deliveryDelay, 95) << ",\n"
      << "  \"deliveryP99Ns\": " << percentile(m_deliveryDelay, 99) << ",\n"
      << "  \"dataSignCalls\": " << signer.dataCalls << ",\n"
      << "  \"dataSignNs\": " << signer.dataNs << ",\n"
      << "  \"dataSignWaitNs\": " << signer.dataWaitNs << ",\n"
      << "  \"dataSignServiceNs\": " << signer.dataServiceNs << ",\n"
      << "  \"interestSignCalls\": " << signer.interestCalls << ",\n"
      << "  \"interestSignNs\": " << signer.interestNs << ",\n"
      << "  \"interestSignWaitNs\": " << signer.interestWaitNs << ",\n"
      << "  \"interestSignServiceNs\": " << signer.interestServiceNs << ",\n"
      << "  \"maxActiveSigners\": " << signer.maxActive << ",\n"
      << "  \"dataValid\": " << validator.dataValid << ",\n"
      << "  \"dataInvalid\": " << validator.dataInvalid << ",\n"
      << "  \"dataVerifyNs\": " << validator.dataNs << ",\n"
      << "  \"interestValid\": " << validator.interestValid << ",\n"
      << "  \"interestInvalid\": " << validator.interestInvalid << ",\n"
      << "  \"interestVerifyNs\": " << validator.interestNs << ",\n"
      << "  \"dataSignCallsAtMeasureEnd\": "
      << m_measureEndSigner.dataCalls << ",\n"
      << "  \"dataSignCallsAtMeasureStart\": "
      << m_measureStartSigner.dataCalls << ",\n"
      << "  \"dataSignServiceNsAtMeasureStart\": "
      << m_measureStartSigner.dataServiceNs << ",\n"
      << "  \"dataSignServiceNsAtMeasureEnd\": "
      << m_measureEndSigner.dataServiceNs << ",\n"
      << "  \"dataSignWaitNsAtMeasureStart\": "
      << m_measureStartSigner.dataWaitNs << ",\n"
      << "  \"dataSignWaitNsAtMeasureEnd\": "
      << m_measureEndSigner.dataWaitNs << ",\n"
      << "  \"interestSignCallsAtMeasureEnd\": "
      << m_measureEndSigner.interestCalls << ",\n"
      << "  \"interestSignCallsAtMeasureStart\": "
      << m_measureStartSigner.interestCalls << ",\n"
      << "  \"interestSignServiceNsAtMeasureStart\": "
      << m_measureStartSigner.interestServiceNs << ",\n"
      << "  \"interestSignServiceNsAtMeasureEnd\": "
      << m_measureEndSigner.interestServiceNs << ",\n"
      << "  \"interestSignWaitNsAtMeasureStart\": "
      << m_measureStartSigner.interestWaitNs << ",\n"
      << "  \"interestSignWaitNsAtMeasureEnd\": "
      << m_measureEndSigner.interestWaitNs << ",\n"
      << "  \"dataValidAtMeasureEnd\": "
      << m_measureEndValidator.dataValid << ",\n"
      << "  \"interestValidAtMeasureEnd\": "
      << m_measureEndValidator.interestValid << ",\n"
      << "  \"workerSubmitted\": " << preparation.submitted << ",\n"
      << "  \"workerAccepted\": " << preparation.accepted << ",\n"
      << "  \"workerRejected\": " << preparation.rejected << ",\n"
      << "  \"workerStarted\": " << preparation.started << ",\n"
      << "  \"workerPrepared\": " << preparation.prepared << ",\n"
      << "  \"workerCommitted\": " << preparation.committed << ",\n"
      << "  \"workerFailed\": " << preparation.failed << ",\n"
      << "  \"workerCancelled\": " << preparation.cancelled << ",\n"
      << "  \"workerPending\": " << preparation.pending << ",\n"
      << "  \"workerOutstanding\": " << preparation.outstanding << ",\n"
      << "  \"workerCommittedAtMeasureEnd\": "
      << m_measureEndPreparation.committed << ",\n"
      << "  \"workerOutstandingAtMeasureEnd\": "
      << m_measureEndPreparation.outstanding << ",\n"
      << "  \"workerPendingAtMeasureEnd\": "
      << m_measureEndPreparation.pending << ",\n"
      << "  \"publicationFetchQueuedAtMeasureEnd\": "
      << m_publicationFetchAtMeasureEnd.queued << ",\n"
      << "  \"publicationFetchPendingAtMeasureEnd\": "
      << m_publicationFetchAtMeasureEnd.pending << ",\n"
      << "  \"publicationFetchQueuedAtDrainEnd\": "
      << publicationFetch.queued << ",\n"
      << "  \"publicationFetchPendingAtDrainEnd\": "
      << publicationFetch.pending << ",\n"
      << "  \"publicationFetchDispatchedAtMeasureEnd\": "
      << m_publicationFetchAtMeasureEnd.dispatched << ",\n"
      << "  \"publicationFetchDispatchedAtDrainEnd\": "
      << publicationFetch.dispatched << ",\n"
      << "  \"publicationFetchDataAtMeasureEnd\": "
      << m_publicationFetchAtMeasureEnd.data << ",\n"
      << "  \"publicationFetchDataAtDrainEnd\": "
      << publicationFetch.data << ",\n"
      << "  \"publicationFetchNacksAtMeasureEnd\": "
      << m_publicationFetchAtMeasureEnd.nacks << ",\n"
      << "  \"publicationFetchNacksAtMeasureStart\": "
      << m_publicationFetchAtMeasureStart.nacks << ",\n"
      << "  \"publicationFetchNacksAtDrainEnd\": "
      << publicationFetch.nacks << ",\n"
      << "  \"publicationFetchTimeoutsAtMeasureEnd\": "
      << m_publicationFetchAtMeasureEnd.timeouts << ",\n"
      << "  \"publicationFetchTimeoutsAtMeasureStart\": "
      << m_publicationFetchAtMeasureStart.timeouts << ",\n"
      << "  \"publicationFetchTimeoutsAtDrainEnd\": "
      << publicationFetch.timeouts << ",\n"
      << "  \"publicationFetchRetriesAtMeasureStart\": "
      << (m_publicationFetchAtMeasureStart.retries +
          m_piggybackAtMeasureStart.publicationRetryActivations)
      << ",\n"
      << "  \"publicationFetchRetriesAtMeasureEnd\": "
      << (m_publicationFetchAtMeasureEnd.retries +
          m_piggybackAtMeasureEnd.publicationRetryActivations)
      << ",\n"
      << "  \"publicationFetchRetriesAtDrainEnd\": "
      << (publicationFetch.retries + piggyback.publicationRetryActivations)
      << ",\n"
      << "  \"publicationFetchInnerRetriesAtMeasureStart\": "
      << m_publicationFetchAtMeasureStart.retries << ",\n"
      << "  \"publicationFetchInnerRetriesAtMeasureEnd\": "
      << m_publicationFetchAtMeasureEnd.retries << ",\n"
      << "  \"publicationFetchInnerRetriesAtDrainEnd\": "
      << publicationFetch.retries << ",\n"
      << "  \"publicationFetchOuterRetryActivationsAtMeasureStart\": "
      << m_piggybackAtMeasureStart.publicationRetryActivations << ",\n"
      << "  \"publicationFetchOuterRetryActivationsAtMeasureEnd\": "
      << m_piggybackAtMeasureEnd.publicationRetryActivations << ",\n"
      << "  \"publicationFetchOuterRetryActivationsAtDrainEnd\": "
      << piggyback.publicationRetryActivations << ",\n"
      << "  \"mappingFetchQueuedAtMeasureEnd\": "
      << m_mappingFetchAtMeasureEnd.queued << ",\n"
      << "  \"mappingFetchPendingAtMeasureEnd\": "
      << m_mappingFetchAtMeasureEnd.pending << ",\n"
      << "  \"mappingFetchQueuedAtDrainEnd\": "
      << mappingFetch.queued << ",\n"
      << "  \"mappingFetchPendingAtDrainEnd\": "
      << mappingFetch.pending << ",\n"
      << "  \"mappingFetchDispatchedAtMeasureEnd\": "
      << m_mappingFetchAtMeasureEnd.dispatched << ",\n"
      << "  \"mappingFetchDispatchedAtDrainEnd\": "
      << mappingFetch.dispatched << ",\n"
      << "  \"mappingFetchDataAtMeasureEnd\": "
      << m_mappingFetchAtMeasureEnd.data << ",\n"
      << "  \"mappingFetchDataAtDrainEnd\": "
      << mappingFetch.data << ",\n"
      << "  \"mappingFetchNacksAtMeasureEnd\": "
      << m_mappingFetchAtMeasureEnd.nacks << ",\n"
      << "  \"mappingFetchNacksAtMeasureStart\": "
      << m_mappingFetchAtMeasureStart.nacks << ",\n"
      << "  \"mappingFetchNacksAtDrainEnd\": "
      << mappingFetch.nacks << ",\n"
      << "  \"mappingFetchTimeoutsAtMeasureEnd\": "
      << m_mappingFetchAtMeasureEnd.timeouts << ",\n"
      << "  \"mappingFetchTimeoutsAtMeasureStart\": "
      << m_mappingFetchAtMeasureStart.timeouts << ",\n"
      << "  \"mappingFetchTimeoutsAtDrainEnd\": "
      << mappingFetch.timeouts << ",\n"
      << "  \"mappingFetchRetriesAtMeasureStart\": "
      << m_mappingFetchAtMeasureStart.retries << ",\n"
      << "  \"mappingFetchRetriesAtMeasureEnd\": "
      << m_mappingFetchAtMeasureEnd.retries << ",\n"
      << "  \"mappingFetchRetriesAtDrainEnd\": "
      << mappingFetch.retries << ",\n"
      << "  \"signedPublicationWireBytesCount\": "
      << piggyback.preparedCount << ",\n"
      << "  \"signedPublicationWireBytesTotal\": "
      << piggyback.preparedWireBytesTotal << ",\n"
      << "  \"signedPublicationWireBytesMax\": "
      << piggyback.preparedWireBytesMax << ",\n"
      << "  \"piggybackEligibleCount\": "
      << piggyback.eligiblePrepared << ",\n"
      << "  \"piggybackIneligibleCount\": "
      << piggyback.ineligiblePrepared << ",\n"
      << "  \"piggybackSentCount\": " << piggyback.sent << ",\n"
      << "  \"piggybackReceivedCount\": " << piggyback.received << ",\n"
      << "  \"piggybackDeliveredCount\": " << piggyback.delivered << ",\n"
      << "  \"publicationFetchFallbackCount\": "
      << piggyback.publicationFetchFallbacks << ",\n"
      << "  \"workerMaxPending\": " << preparation.maxPending << ",\n"
      << "  \"workerQueueWaitNsTotal\": " << preparation.queueWaitNsTotal << ",\n"
      << "  \"workerQueueWaitNsMax\": " << preparation.queueWaitNsMax << ",\n"
      << "  \"workerServiceNsTotal\": " << preparation.serviceNsTotal << ",\n"
      << "  \"workerServiceNsMax\": " << preparation.serviceNsMax << ",\n"
      << "  \"resourceMeasureWallNs\": " << resourceWallNs << ",\n"
      << "  \"resourceMeasureStartSteadyNs\": "
      << m_resourceAtMeasureStart.steadyNs << ",\n"
      << "  \"resourceMeasureEndSteadyNs\": "
      << m_resourceAtMeasureEnd.steadyNs << ",\n"
      << "  \"resourceUserCpuUs\": " << resourceUserCpuUs << ",\n"
      << "  \"resourceSystemCpuUs\": " << resourceSystemCpuUs << ",\n"
      << "  \"resourceTotalCpuUs\": " << resourceTotalCpuUs << ",\n"
      << "  \"resourceCpuPctOneCore\": " << resourceCpuPctOneCore << ",\n"
      << "  \"resourceCpuPctFourCore\": " << resourceCpuPctFourCore << ",\n"
      << "  \"resourceThreadsAtMeasureStart\": "
      << m_resourceAtMeasureStart.threadCount << ",\n"
      << "  \"resourceThreadsAtMeasureEnd\": "
      << m_resourceAtMeasureEnd.threadCount << ",\n"
      << "  \"resourceMaxRssKiBAtMeasureStart\": "
      << m_resourceAtMeasureStart.maxRssKiB << ",\n"
      << "  \"resourceMaxRssKiBAtMeasureEnd\": "
      << m_resourceAtMeasureEnd.maxRssKiB << ",\n"
      << "  \"maxRssKiB\": " << usage.ru_maxrss << "\n"
      << "}\n";
  }

private:
  Options m_options;
  SVSPubSubOptions m_configuredOptions;
  Face m_face;
  KeyChain m_keyChain;
  std::shared_ptr<TimedRsaSigner> m_signer;
  std::shared_ptr<FixedCertificateValidator> m_validator;
  std::unique_ptr<SVSPubSub> m_pubsub;
  boost::asio::steady_timer m_heartbeatTimer;
  std::thread m_pacer;
  std::mutex m_pacerErrorMutex;
  std::string m_pacerError;
  int m_exitCode = 0;
  uint64_t m_ownHash;
  uint64_t m_remoteHash;
  std::chrono::steady_clock::time_point m_start;
  std::chrono::steady_clock::time_point m_measuredStart;
  std::chrono::steady_clock::time_point m_measuredEnd;
  std::chrono::steady_clock::time_point m_stopAt;
  std::chrono::steady_clock::time_point m_nextHeartbeat;
  uint64_t m_faceThreadHash = 0;
  std::atomic<uint64_t> m_pacerThreadHash{0};
  std::atomic<uint64_t> m_publishCallThreadHash{0};
  std::atomic<uint64_t> m_publishCallsOnFace{0};
  std::atomic<uint64_t> m_publishCallsOnPacer{0};
  std::atomic<uint64_t> m_faceDispatchPending{0};
  std::atomic<uint64_t> m_faceDispatchAbandoned{0};
  std::atomic_bool m_dispatchOpen{true};
  std::atomic_bool m_pacerFailed{false};
  SVSPubSub::PublicationPreparationStats m_measureEndPreparation;
  Fetcher::Stats m_publicationFetchAtMeasureStart;
  Fetcher::Stats m_publicationFetchAtMeasureEnd;
  Fetcher::Stats m_mappingFetchAtMeasureStart;
  Fetcher::Stats m_mappingFetchAtMeasureEnd;
  SVSPubSub::PiggybackStats m_piggybackAtMeasureStart;
  SVSPubSub::PiggybackStats m_piggybackAtMeasureEnd;
  SignerSnapshot m_measureEndSigner;
  SignerSnapshot m_measureStartSigner;
  ValidatorSnapshot m_measureEndValidator;
  ResourceSnapshot m_resourceAtMeasureStart;
  ResourceSnapshot m_resourceAtMeasureEnd;
  uint64_t m_faceDispatchPendingAtMeasureEnd = 0;
  uint64_t m_deliveredWarmupAtMeasureEnd = 0;
  uint64_t m_deliveredMeasuredAtMeasureEnd = 0;
  bool m_measureStartCaptured = false;
  uint64_t m_logicalId = 0;
  std::atomic<uint64_t> m_attemptedMeasured{0};
  std::atomic<uint64_t> m_acceptedMeasured{0};
  std::atomic<uint64_t> m_deliveredWarmup{0};
  std::atomic<uint64_t> m_deliveredMeasured{0};
  std::atomic<uint64_t> m_publishErrors{0};
  uint64_t m_duplicates = 0;
  uint64_t m_invalid = 0;
  uint64_t m_selfDeliveries = 0;
  uint64_t m_heartbeatSkipped = 0;
  std::unordered_set<uint64_t> m_seen;
  std::vector<uint64_t> m_releaseLateness;
  std::vector<uint64_t> m_heartbeatLateness;
  std::vector<uint64_t> m_deliveryDelay;
};

} // namespace

int
main(int argc, char** argv)
{
  try {
    auto options = parseOptions(argc, argv);
    if (options.securityPreflight) {
      return runSecurityPreflight(options);
    }
    if (options.pacerOnly) {
      NoOpPacer pacer(std::move(options));
      return pacer.run();
    }
    if (options.profileOnly) {
      return runProfileOnly(options);
    }
    Peer peer(std::move(options));
    return peer.run();
  }
  catch (const std::exception& error) {
    std::cerr << "SPEC136_ERROR " << error.what() << std::endl;
    return 2;
  }
}
