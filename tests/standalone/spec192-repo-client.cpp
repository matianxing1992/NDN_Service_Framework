#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/utils.hpp"
#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/logging.hpp>
#include <ndn-cxx/security/pib/impl/pib-sqlite3.hpp>
#include <boost/asio/post.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace repo = ndnsf_distributed_repo;
namespace nsf = ndn_service_framework;
namespace fs = std::filesystem;

namespace {

using Options = std::map<std::string, std::string>;

Options
parseOptions(int argc, char* argv[])
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--help") {
        std::cout << "spec192-repo-client --mode inapp-publish|inapp-reopen|inapp-reopen-expect-error|inapp-invalid-inputs|persistent-invalid-config|discover|archive|archive-begin|publish|fetch|"
                   "availability|failover-fetch|failover-cooldown|all-down-fetch|reject-provider-alias|"
                   "operation-begin|operation-status|operation-status-once|operation-replay|operation-conflict|receipt-query|"
                   "single-publish|invalid-selected-set|object-store-fetch|object-reopen-fetch|object-version-fetch|object-range-read|object-range-rejection-audit|object-remove-audit|object-corruption-audit|object-segmented-fetch|object-data-fetch|object-original-data-fetch|object-original-data-preimport-audit|object-original-data-location-audit|object-original-data-denied-audit|object-packet-set-fetch|object-payload-fetch|object-cache-status|object-catalog-query|object-catalog-delete|object-catalog-deleted|object-range-phase|object-inventory-audit|object-data-publisher|object-insert-request|object-insert-fetch|object-write-reject|import-public-certificate "
                   "[--source PATH] [--store-dir PATH] [--handle PATH] [--handle-out PATH] "
                   "[--destination PATH] [--receipt-out PATH] [--report-out PATH] "
                   "[--probe-kind metadata|verify_content] [--probe-first-repo NAME] [--group NAME] "
                   "[--certificate PATH] "
                   "[--controller NAME] [--authority NAME] [--user NAME] "
                   "[--trust-schema PATH] [--service-prefix NAME] [--repo-a NAME] "
                   "[--provider-a NAME] [--repo-b NAME] [--provider-b NAME] "
                   "[--repo-c NAME --provider-c NAME] [--key VALUE] "
                   "[--object-name NAME] [--expected-error TEXT] "
                   "[--consistency ONE|QUORUM|ALL]\n";
      std::exit(0);
    }
    if (key.rfind("--", 0) != 0 || i + 1 >= argc) {
      throw std::invalid_argument("invalid command line option: " + key);
    }
    if (!options.emplace(key.substr(2), argv[++i]).second) {
      throw std::invalid_argument("duplicate command line option: " + key);
    }
  }
  return options;
}

const std::string&
required(const Options& options, const std::string& key)
{
  auto it = options.find(key);
  if (it == options.end() || it->second.empty()) {
    throw std::invalid_argument("missing required option --" + key);
  }
  return it->second;
}

std::vector<uint8_t>
readBytes(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read file: " + path.string());
  }
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

ndn::security::Certificate
readCertificate(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read certificate: " + path.string());
  }

  // ndnsec writes a base64-encoded Data packet, optionally surrounded by armor lines.
  std::string encoded;
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("-----", 0) == 0) {
      continue;
    }
    for (unsigned char ch : line) {
      if (!std::isspace(ch)) {
        encoded.push_back(static_cast<char>(ch));
      }
    }
  }
  if (encoded.empty() || encoded.size() % 4 != 0 ||
      encoded.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("invalid base64 certificate file: " + path.string());
  }

  std::vector<uint8_t> wire((encoded.size() / 4) * 3);
  const int decodedSize = EVP_DecodeBlock(
    wire.data(), reinterpret_cast<const unsigned char*>(encoded.data()),
    static_cast<int>(encoded.size()));
  if (decodedSize < 0) {
    throw std::runtime_error("cannot decode base64 certificate: " + path.string());
  }
  size_t padding = 0;
  if (encoded.back() == '=') {
    padding = encoded.size() > 1 && encoded[encoded.size() - 2] == '=' ? 2 : 1;
  }
  if (padding > static_cast<size_t>(decodedSize)) {
    throw std::runtime_error("invalid base64 certificate padding: " + path.string());
  }
  wire.resize(static_cast<size_t>(decodedSize) - padding);
  std::istringstream packet(std::string(reinterpret_cast<const char*>(wire.data()), wire.size()));
  return ndn::security::Certificate(ndn::Block::fromStream(packet));
}

void
writeText(const fs::path& path, const std::string& text)
{
  fs::create_directories(path.parent_path());
  const fs::path temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output || !(output << text << '\n')) {
    throw std::runtime_error("cannot write file: " + temporary.string());
  }
  output.close();
  if (!output) throw std::runtime_error("cannot close file: " + temporary.string());
  std::error_code error;
  fs::rename(temporary, path, error);
  if (error) throw std::runtime_error("cannot publish file: " + error.message());
}

std::string
sha256(const std::vector<uint8_t>& bytes)
{
  unsigned char digest[SHA256_DIGEST_LENGTH]{};
  SHA256(bytes.data(), bytes.size(), digest);
  static constexpr char HEX[] = "0123456789abcdef";
  std::string result;
  result.reserve(SHA256_DIGEST_LENGTH * 2);
  for (const auto value : digest) {
    result.push_back(HEX[value >> 4]);
    result.push_back(HEX[value & 0x0f]);
  }
  return result;
}

repo::ArtifactReference
makeReference(const fs::path& source, const std::string& publisher)
{
  const auto bytes = readBytes(source);
  const auto digest = sha256(bytes);
  repo::ArtifactReference reference;
  reference.logicalName = "/spec192/e2e/" + source.filename().string();
  reference.contentDigest = digest;
  reference.sizeBytes = bytes.size();
  reference.rootManifestName = publisher + "/NDNSF/ARTIFACT/" + digest + "/root";
  reference.publisherIdentity = publisher;
  reference.policyEpoch = "repo-policy-v1";
  reference.validate();
  return reference;
}

repo::SelectedRepoSet
makeSelected(const Options& options, const ndn::Name& servicePrefix)
{
  repo::SelectedRepoSet selected;
  const auto repoA = required(options, "repo-a");
  selected.seedRepoId = repoA;
  selected.consistency = repo::RepoWriteConsistency::All;
  if (options.count("consistency")) {
    const auto& consistency = options.at("consistency");
    if (consistency == "ONE") {
      selected.consistency = repo::RepoWriteConsistency::One;
    }
    else if (consistency == "QUORUM") {
      selected.consistency = repo::RepoWriteConsistency::Quorum;
    }
    else if (consistency != "ALL") {
      throw std::invalid_argument("invalid --consistency " + consistency);
    }
  }
  selected.targets.push_back({repoA, ndn::Name(required(options, "provider-a")), servicePrefix});
  selected.targets.push_back({required(options, "repo-b"),
                              ndn::Name(required(options, "provider-b")), servicePrefix});
  if (options.count("repo-c") || options.count("provider-c")) {
    if (!options.count("repo-c") || !options.count("provider-c")) {
      throw std::invalid_argument("--repo-c and --provider-c must be supplied together");
    }
    selected.targets.push_back({required(options, "repo-c"),
                                ndn::Name(required(options, "provider-c")), servicePrefix});
  }
  selected.validate(servicePrefix);
  return selected;
}

bool
isTerminal(const repo::PublishOperationStatus& status)
{
  return status.state == "COMMITTED" || status.state == "PARTIAL" ||
         status.state == "FAILED";
}

repo::PublishOperationStatus
waitForTerminal(repo::ArtifactRepositoryApi& api,
                const repo::PublishOperationHandle& handle,
                int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  repo::PublishOperationStatus status;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) break;
    status = api.getPublishStatus(handle,
      static_cast<int>(std::min<int64_t>(remaining, 5000)));
    if (isTerminal(status)) return status;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  throw std::runtime_error("repo-publish-operation-timeout: operation did not become terminal");
}

std::string
readText(const fs::path& path)
{
  const auto bytes = readBytes(path);
  return std::string(bytes.begin(), bytes.end());
}

bool
hasFetchStagingFile(const fs::path& destinationPath)
{
  const auto destination = fs::absolute(destinationPath).lexically_normal();
  auto parent = destination.parent_path();
  if (parent.empty()) parent = fs::current_path();
  const auto prefix = "." + destination.filename().string() + ".ndnsf-";
  std::error_code error;
  if (!fs::is_directory(parent, error) || error) return false;
  for (const auto& entry : fs::directory_iterator(parent)) {
    const auto name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0 && entry.path().extension() == ".tmp") {
      return true;
    }
  }
  return false;
}

void
requireAvailability(const repo::AvailabilityReport& report,
                    const std::string& expectation,
                    const std::string& targetRepo)
{
  const auto target = std::find_if(report.locations.begin(), report.locations.end(),
    [&targetRepo] (const auto& item) { return item.repoNode == targetRepo; });
  if (target == report.locations.end()) {
    throw std::runtime_error("availability report omitted expected Repo " + targetRepo);
  }
  if (expectation == "healthy") {
    if (report.locations.empty() || std::any_of(report.locations.begin(), report.locations.end(),
        [] (const auto& item) {
          return item.reachability != "REACHABLE" ||
                 item.objectPresence != "PRESENT" || item.integrity != "VERIFIED";
        })) {
      throw std::runtime_error("availability oracle expected every selected replica to be healthy");
    }
  }
  else if (expectation == "metadata-healthy") {
    if (report.probeKind != "metadata" || report.locations.empty() ||
        std::any_of(report.locations.begin(), report.locations.end(),
        [] (const auto& item) {
          return item.probeKind != "metadata" ||
                 item.reachability != "REACHABLE" ||
                 item.objectPresence != "PRESENT" ||
                 item.integrity != "NOT_CHECKED" ||
                 item.locationVersion == 0 ||
                 item.validUntilMs <= item.observedAtMs;
        })) {
      throw std::runtime_error("availability oracle expected current complete metadata");
    }
  }
  else if (expectation == "reachable-target") {
    if (report.probeKind != "metadata" || target->reachability != "REACHABLE" ||
        target->objectPresence != "PRESENT" || target->integrity != "NOT_CHECKED") {
      throw std::runtime_error("availability oracle expected selected target to remain reachable");
    }
  }
  else if (expectation == "unknown-target") {
    if (report.probeKind != "metadata" || target->reachability != "REACHABLE" ||
        target->objectPresence != "UNKNOWN" || target->integrity != "NOT_CHECKED") {
      throw std::runtime_error("availability oracle expected a reachable Repo with unknown current location");
    }
  }
  else if (expectation == "unreachable") {
    if (target->reachability != "UNREACHABLE" || target->objectPresence != "UNKNOWN") {
      throw std::runtime_error("availability oracle expected the target Repo to be unreachable");
    }
  }
  else if (expectation == "missing") {
    if (target->reachability != "REACHABLE" ||
        (target->objectPresence != "ABSENT" && target->objectPresence != "PARTIAL")) {
      throw std::runtime_error("availability oracle expected the target object to be missing");
    }
  }
  else if (expectation == "corrupt") {
    if (target->reachability != "REACHABLE" || target->objectPresence != "PRESENT" ||
        target->integrity != "FAILED") {
      throw std::runtime_error("availability oracle expected corrupt content to fail verification");
    }
  }
  else {
    throw std::invalid_argument("unsupported availability expectation " + expectation);
  }
}

repo::ArtifactPublishResult
publish(repo::ArtifactRepositoryApi& api, const Options& options,
        const repo::ArtifactReference& reference,
        const ndn::Name& servicePrefix)
{
  const auto selected = makeSelected(options, servicePrefix);
  return api.publishFile(required(options, "source"), reference, selected, 180000);
}

class PersistentContext
{
public:
  explicit PersistentContext(const Options& options)
    : m_face()
    , m_keyChain()
    , m_userIdentity(required(options, "user"))
    , m_authorityIdentity(required(options, "authority"))
  {
    m_keyChain.setDefaultIdentity(m_keyChain.getPib().getIdentity(m_userIdentity));
    const auto userCertificate = getCertificate(m_userIdentity);
    const auto authorityCertificate = getCertificate(m_authorityIdentity);
    m_certificatePublisher = std::make_unique<nsf::CertificatePublisher>(
      m_face, m_keyChain, userCertificate.getName());
    m_serviceUser = std::make_unique<nsf::ServiceUser>(
      m_face, ndn::Name(required(options, "group")), userCertificate,
      authorityCertificate, required(options, "trust-schema"));
    try {
      m_serviceUser->init();
      m_serviceUser->fetchPermissionsFromController(m_authorityIdentity);
      m_faceThread = std::thread([this] {
        while (!m_stop.load()) {
          m_face.processEvents(ndn::time::milliseconds(10));
          m_face.getIoContext().restart();
        }
      });
      waitForRepoRequestBootstrap(options);
    }
    catch (...) {
      stopFaceThread();
      throw;
    }
  }

  ~PersistentContext()
  {
    stopFaceThread();
  }

  nsf::ServiceUser& user() { return *m_serviceUser; }
  void post(std::function<void()> work) { m_face.getIoContext().post(std::move(work)); }

private:
  void
  stopFaceThread()
  {
    m_stop.store(true);
    m_face.getIoContext().stop();
    if (m_faceThread.joinable()) {
      m_faceThread.join();
    }
    m_face.getIoContext().restart();
    // Run the shutdown handler after the event thread has joined. Process one
    // handler at a time and stop at the barrier: Face::shutdown closes the
    // Unix transport, and a queued receive completion must not run afterward
    // against the already-closed transport.
    bool shutdownProcessed = false;
    m_face.shutdown();
    boost::asio::post(m_face.getIoContext(), [&shutdownProcessed] {
      shutdownProcessed = true;
    });
    auto& io = m_face.getIoContext();
    while (!shutdownProcessed) {
      try {
        if (io.poll_one() == 0) {
          break;
        }
      }
      catch (const std::exception& error) {
        std::cerr << "Face shutdown callback failed: " << error.what() << '\n';
        break;
      }
    }
  }

  void
  waitForRepoRequestBootstrap(const Options& options)
  {
    static const std::vector<std::string> operations = {
      "STORE", "STORE_RANGE", "COMMIT_RANGES", "ABORT_RANGES", "INSERT",
      "STORE_MANIFEST", "FETCH", "FETCH_RANGE", "MANIFEST", "INVENTORY",
      "CAPABILITY", "CACHE_STATUS", "STATUS", "CATALOG_STATUS",
      "CATALOG_SNAPSHOT", "CATALOG_DELTA", "CATALOG_LOOKUP", "DELETE",
    };

    const ndn::Name servicePrefix(required(options, "service-prefix"));
    std::vector<ndn::Name> services;
    services.reserve(operations.size());
    for (const auto& operation : operations) {
      services.emplace_back(servicePrefix).append(operation);
    }

    std::vector<ndn::Name> providers;
    for (const char* option : {"provider-a", "provider-b", "provider-c"}) {
      const auto it = options.find(option);
      if (it != options.end() && !it->second.empty()) {
        providers.emplace_back(it->second);
      }
    }
    if (providers.empty()) {
      throw std::invalid_argument(
        "request bootstrap readiness requires at least one Repo provider");
    }

    struct ProbeState
    {
      std::mutex mutex;
      std::condition_variable changed;
      bool complete = false;
      bool ready = false;
      std::exception_ptr error;
    };

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
      auto probe = std::make_shared<ProbeState>();
      m_face.getIoContext().post([this, probe, services, providers] {
        try {
          const bool ready = std::all_of(
            services.begin(), services.end(), [this, &providers] (const auto& service) {
              return std::all_of(
                providers.begin(), providers.end(), [this, &service] (const auto& provider) {
                  return m_serviceUser->isRequestBootstrapReady(
                    service, std::vector<ndn::Name>{provider});
                });
            });
          std::lock_guard<std::mutex> lock(probe->mutex);
          probe->ready = ready;
        }
        catch (...) {
          std::lock_guard<std::mutex> lock(probe->mutex);
          probe->error = std::current_exception();
        }
        {
          std::lock_guard<std::mutex> lock(probe->mutex);
          probe->complete = true;
        }
        probe->changed.notify_one();
      });

      std::unique_lock<std::mutex> lock(probe->mutex);
      const auto pollDeadline = std::min(
        deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
      probe->changed.wait_until(lock, pollDeadline, [&probe] {
        return probe->complete;
      });
      if (probe->complete) {
        if (probe->error) {
          std::rethrow_exception(probe->error);
        }
        if (probe->ready) {
          return;
        }
      }
    }
    throw std::runtime_error(
      "repo-request-bootstrap-timeout: Repo services/providers did not become ready");
  }

  ndn::security::Certificate
  getCertificate(const ndn::Name& identity)
  {
    return m_keyChain.getPib().getIdentity(identity)
      .getDefaultKey().getDefaultCertificate();
  }

  ndn::Face m_face;
  ndn::security::KeyChain m_keyChain;
  ndn::Name m_userIdentity;
  ndn::Name m_authorityIdentity;
  std::unique_ptr<nsf::ServiceUser> m_serviceUser;
  std::unique_ptr<nsf::CertificatePublisher> m_certificatePublisher;
  std::atomic_bool m_stop{false};
  std::thread m_faceThread;
};

struct ObjectCapabilityEvidence
{
  std::string requestId;
  std::string ackWireDigest;
  std::string signerKeyLocator;
  std::vector<uint8_t> response;
};

struct WrapperReply
{
  ndn::Name requestId;
  std::vector<uint8_t> payload;
  bool status = true;
  std::string error;
};

WrapperReply
awaitWrapper(PersistentContext& context,
             std::function<ndn::Name(nsf::ServiceUser::TimeoutHandler,
                                     nsf::ServiceUser::ResponseHandler)> start,
             bool allowRejected = false, bool allowTimeout = false)
{
  struct State
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    bool abandoned = false;
    bool started = false;
    std::string error;
    WrapperReply reply;
  };
  auto state = std::make_shared<State>();
  context.post([state, start = std::move(start), allowRejected, allowTimeout] {
    {
      std::lock_guard<std::mutex> guard(state->mutex);
      if (state->abandoned) return;
    }
    auto timeout = [state, allowTimeout] (const ndn::Name&) {
      std::lock_guard<std::mutex> guard(state->mutex);
      if (state->done || state->abandoned) return;
      state->reply.status = false;
      state->reply.error = "network-wrapper-timeout";
      if (!allowTimeout) state->error = "network-wrapper-timeout";
      state->done = true;
      state->changed.notify_one();
    };
    auto response = [state, allowRejected] (const nsf::ResponseMessage& message) {
      std::lock_guard<std::mutex> guard(state->mutex);
      if (state->done || state->abandoned) return;
      const auto bytes = message.getPayload();
      state->reply.payload.assign(bytes.begin(), bytes.end());
      state->reply.status = message.getStatus();
      state->reply.error = message.getErrorInfo();
      if (!message.getStatus() && !allowRejected) state->error = "network-wrapper-rejected: " + message.getErrorInfo();
      state->done = true;
      state->changed.notify_one();
    };
    try {
      const auto id = start(std::move(timeout), std::move(response));
      std::lock_guard<std::mutex> guard(state->mutex);
      state->reply.requestId = id;
      state->started = true;
      if (id.empty()) {
        state->error = "network-wrapper-empty-request-id";
        state->done = true;
        state->changed.notify_one();
      }
      if (state->done) state->changed.notify_one();
    }
    catch (const std::exception& error) {
      std::lock_guard<std::mutex> guard(state->mutex);
      state->error = error.what();
      state->started = true;
      state->done = true;
      state->changed.notify_one();
    }
  });
  std::unique_lock<std::mutex> guard(state->mutex);
  if (!state->changed.wait_for(guard, std::chrono::seconds(40), [&] { return state->done && state->started; })) {
    state->abandoned = true;
    throw std::runtime_error("network-wrapper-driver-deadline");
  }
  if (!state->error.empty()) throw std::runtime_error(state->error);
  return state->reply;
}

ObjectCapabilityEvidence
probeObjectCapability(PersistentContext& context, const ndn::Name& provider,
                      const ndn::Name& serviceName, const std::string& repoNode)
{
  std::mutex mutex;
  std::condition_variable condition;
  bool done = false;
  std::string error;
  ObjectCapabilityEvidence evidence;
  size_t selectedCount = 0;
  const auto requestId = context.user().RequestService(
    std::vector<ndn::Name>{provider}, serviceName,
    repo::RepoClient::makeRequest({}), 15000,
    [&] (const std::vector<nsf::AckSelectionCandidate>& candidates) {
      std::vector<nsf::AckSelectionCandidate> selected;
      for (const auto& candidate : candidates) {
        if (candidate.providerName != provider || !candidate.ack.getStatus()) {
          continue;
        }
        if (!candidate.authenticationEvidence.trustSchemaValidated ||
            candidate.authenticationEvidence.signerIdentity != provider.toUri()) {
          continue;
        }
        try {
          const auto payload = candidate.ack.getPayload();
          const auto ack = repo::ArtifactCapabilityAck::fromJson(
            std::string(payload.begin(), payload.end()));
          if (ack.capability.repoNode != repoNode ||
              ack.capability.supportsReplicaReceipts) {
            std::lock_guard<std::mutex> guard(mutex);
            error = "object CAPABILITY advertised wrong Repo or replica receipts";
          }
        }
        catch (const std::exception& exception) {
          std::lock_guard<std::mutex> guard(mutex);
          error = exception.what();
        }
        {
          std::lock_guard<std::mutex> guard(mutex);
          evidence.ackWireDigest = candidate.authenticationEvidence.wireDigest;
          evidence.signerKeyLocator =
            candidate.authenticationEvidence.signerKeyLocator;
        }
        selected.push_back(candidate);
      }
      {
        std::lock_guard<std::mutex> guard(mutex);
        selectedCount = selected.size();
      }
      return selected;
    },
    120000,
    [&] (const ndn::Name&) {
      std::lock_guard<std::mutex> guard(mutex);
      error = "object CAPABILITY request timed out";
      done = true;
      condition.notify_all();
    },
    [&] (const nsf::ResponseMessage& response) {
      std::lock_guard<std::mutex> guard(mutex);
      if (!response.getStatus()) {
        error = response.getErrorInfo();
      }
      else {
        const auto payload = response.getPayload();
        evidence.response.assign(payload.begin(), payload.end());
      }
      done = true;
      condition.notify_all();
    });
  if (requestId.empty()) {
    throw std::runtime_error("object CAPABILITY request was not admitted");
  }
  evidence.requestId = requestId.toUri();
  {
    std::unique_lock<std::mutex> guard(mutex);
    if (!condition.wait_for(guard, std::chrono::seconds(130),
                            [&] { return done; })) {
      throw std::runtime_error("object CAPABILITY callback did not complete");
    }
  }
  if (!error.empty()) {
    throw std::runtime_error(error);
  }
  if (selectedCount != 1 || evidence.ackWireDigest.empty() ||
      evidence.signerKeyLocator.empty()) {
    throw std::runtime_error("object CAPABILITY lacked one authenticated ACK");
  }
  return evidence;
}

int
run(const Options& options)
{
  const auto mode = required(options, "mode");
  if (mode == "import-public-certificate") {
    const auto certificate = readCertificate(required(options, "certificate"));
    ndn::security::pib::PibSqlite3 pib;
    pib.addCertificate(certificate);
    std::cout << "PUBLIC_CERTIFICATE_IMPORTED name=" << certificate.getName() << '\n';
    return 0;
  }
  if (mode == "legacy-inapp-read") {
    repo::InAppRepoConfig config;
    config.durableRoot = required(options, "store-dir");
    config.ownerId = "legacy-import-" + required(options, "import-id");
    auto api = repo::ArtifactRepositoryApi::makeInApp(config);
    const auto bytes = readBytes(required(options, "reference"));
    const auto reference = repo::ArtifactReference::fromJson(std::string(bytes.begin(), bytes.end()));
    if (!api.lookupLocalCommit(reference))
      throw std::runtime_error("imported CAS artifact has no native local commit");
    api.fetchFile(reference, required(options, "destination"));
    const auto fetched = readBytes(required(options, "destination"));
    if (fetched.size() != reference.sizeBytes || sha256(fetched) != reference.contentDigest)
      throw std::runtime_error("imported CAS artifact read mismatch");
    std::cout << "SPEC193_LEGACY_INAPP_READ_PASS sha256=" << sha256(fetched) << std::endl;
    return 0;
  }
  if (mode == "inapp-publish") {
    repo::InAppRepoConfig config;
    config.durableRoot = required(options, "store-dir");
    config.ownerId = required(options, "user");
    auto api = repo::ArtifactRepositoryApi::makeInApp(config);
    const auto reference = makeReference(required(options, "source"), config.ownerId);
    const auto result = api.publishFile(required(options, "source"), reference);
    if (!result.handle || !result.receipt) {
      throw std::runtime_error("In-App publish did not produce a handle and receipt");
    }
    writeText(required(options, "handle-out"), result.handle->toJson());
    writeText(required(options, "receipt-out"), result.receipt->toJson());
    std::cout << "INAPP_COMMITTED sha256=" << reference.contentDigest
              << " receipt=" << result.receipt->receiptId << '\n';
    return 0;
  }
  if (mode == "inapp-invalid-inputs") {
    const auto source = fs::path(required(options, "source"));
    const auto root = fs::path(required(options, "store-dir"));
    repo::InAppRepoConfig config;
    config.durableRoot = root;
    config.ownerId = required(options, "user");
    auto api = repo::ArtifactRepositoryApi::makeInApp(config);
    const auto reference = makeReference(source, config.ownerId);
    auto expectFailure = [] (const std::string& label, const std::string& expected,
                             const auto& call) {
      try {
        call();
      }
      catch (const std::exception& error) {
        if (std::string(error.what()).find(expected) != std::string::npos) {
          std::cout << "SPEC193_INVALID_INPUT_REJECTED case=" << label
                    << " reason=" << expected << '\n';
          return;
        }
        throw;
      }
      throw std::runtime_error("invalid input was accepted: " + label);
    };
    auto invalidReference = reference;
    invalidReference.logicalName.clear();
    expectFailure("reference", "artifact", [&] {
      (void)api.publishFile(source.string(), invalidReference);
    });
    expectFailure("missing-source", "repo-artifact-source-open-failed", [&] {
      (void)api.publishFile((root / "absent-source.bin").string(), reference);
    });
    const auto symlink = root.parent_path() / "spec193-source-link.bin";
    fs::create_symlink(source, symlink);
    try {
      expectFailure("symlink-source", "repo-artifact-source-open-failed", [&] {
        (void)api.publishFile(symlink.string(), reference);
      });
    }
    catch (...) {
      fs::remove(symlink);
      throw;
    }
    fs::remove(symlink);
    const auto rootLink = root.parent_path() / "spec193-root-link";
    fs::create_directory_symlink(root, rootLink);
    try {
      config.durableRoot = rootLink;
      expectFailure("symlink-root", "repo-inapp-root-invalid", [&] {
        (void)repo::ArtifactRepositoryApi::makeInApp(config);
      });
    }
    catch (...) {
      fs::remove(rootLink);
      throw;
    }
    fs::remove(rootLink);
    return 0;
  }
  if (mode == "inapp-reopen") {
    const auto handle = repo::ArtifactHandle::fromJson(
      [&] { const auto bytes = readBytes(required(options, "handle"));
            return std::string(bytes.begin(), bytes.end()); }());
    repo::InAppRepoConfig config;
    config.durableRoot = required(options, "store-dir");
    config.ownerId = handle.reference.publisherIdentity;
    auto api = repo::ArtifactRepositoryApi::makeInApp(config);
    api.fetchFile(handle.reference, required(options, "destination"));
    const auto bytes = readBytes(required(options, "destination"));
    if (bytes.size() != handle.reference.sizeBytes ||
        sha256(bytes) != handle.reference.contentDigest) {
      throw std::runtime_error("In-App reopen oracle mismatch");
    }
    std::cout << "INAPP_REOPEN_FETCH_PASS sha256=" << sha256(bytes) << '\n';
    return 0;
  }
  if (mode == "inapp-reopen-expect-error") {
    const auto handle = repo::ArtifactHandle::fromJson(
      [&] { const auto bytes = readBytes(required(options, "handle"));
            return std::string(bytes.begin(), bytes.end()); }());
    repo::InAppRepoConfig config;
    config.durableRoot = required(options, "store-dir");
    config.ownerId = handle.reference.publisherIdentity;
    auto api = repo::ArtifactRepositoryApi::makeInApp(config);
    const auto destination = fs::path(required(options, "destination"));
    const auto original = readBytes(destination);
    try {
      api.fetchFile(handle.reference, destination.string());
    }
    catch (const std::exception& error) {
      if (std::string(error.what()).find(required(options, "expected-error")) ==
          std::string::npos) {
        throw;
      }
      if (readBytes(destination) != original) {
        throw std::runtime_error("failed In-App fetch changed destination bytes");
      }
      std::cout << "INAPP_FETCH_REJECT_PASS reason="
                << required(options, "expected-error") << '\n';
      return 0;
    }
    throw std::runtime_error("corrupt or missing In-App payload was fetched");
  }
  if (mode == "object-data-publisher") {
    const auto content = readBytes(required(options, "source"));
    const ndn::Name identity(required(options, "user"));
    const ndn::Name prefix(required(options, "data-prefix"));
    ndn::Face face;
    ndn::security::KeyChain keyChain;
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(identity));
    const auto certificate = keyChain.getPib().getIdentity(identity)
                               .getDefaultKey().getDefaultCertificate();
    ndn::Data data{ndn::Name(prefix).appendSegment(0)};
    data.setContent(ndn::span<const uint8_t>(content.data(), content.size()));
    data.setFreshnessPeriod(ndn::time::seconds(30));
    keyChain.sign(data, ndn::security::signingByIdentity(identity));
    const auto& encoded = data.wireEncode();
    const std::vector<uint8_t> wire(encoded.begin(), encoded.end());
    const auto& certBlock = certificate.wireEncode();
    const std::vector<uint8_t> certWire(certBlock.begin(), certBlock.end());
    for (const auto& item : std::vector<std::pair<fs::path, std::vector<uint8_t>>>{
           {required(options, "wire-source"), wire},
           {required(options, "certificate"), certWire}}) {
      fs::create_directories(item.first.parent_path());
      std::ofstream output(item.first, std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char*>(item.second.data()), item.second.size());
      if (!output) throw std::runtime_error("cannot write published Data fixture");
    }
    bool registered = false;
    std::string registrationError;
    auto handle = face.setInterestFilter(
      ndn::InterestFilter(prefix),
      [&] (const ndn::InterestFilter&, const ndn::Interest& interest) {
        if (interest.getName() == data.getName()) {
          face.put(data);
          std::cout << "SPEC193_DATA_PUBLISHER_SENT name=" << data.getName() << std::endl;
        }
      },
      [&] (const ndn::Name&) {
        registered = true;
        std::cout << "SPEC193_DATA_PUBLISHER_READY name=" << data.getName()
                  << " sha256=" << sha256(wire) << std::endl;
      },
      [&] (const ndn::Name&, const std::string& error) {
        registrationError = error;
      });
    for (;;) {
      face.processEvents(ndn::time::milliseconds(25));
      face.getIoContext().restart();
      if (!registrationError.empty()) {
        throw std::runtime_error("Data publisher registration failed: " + registrationError);
      }
    }
  }

  if (mode != "discover" && mode != "archive" && mode != "publish" &&
      mode != "single-publish" && mode != "invalid-selected-set" && mode != "fetch" &&
      mode != "availability" && mode != "failover-fetch" &&
      mode != "failover-cooldown" &&
      mode != "all-down-fetch" && mode != "reject-provider-alias" &&
      mode != "operation-begin" && mode != "operation-status" &&
      mode != "operation-status-once" &&
      mode != "archive-begin" &&
      mode != "operation-replay" && mode != "operation-conflict" &&
      mode != "receipt-query" &&
      mode != "object-store-fetch" && mode != "object-reopen-fetch" &&
      mode != "object-version-fetch" &&
      mode != "object-range-read" && mode != "object-range-rejection-audit" &&
      mode != "object-remove-audit" && mode != "object-corruption-audit" &&
      mode != "object-segmented-fetch" &&
      mode != "object-data-fetch" && mode != "object-original-data-fetch" &&
      mode != "object-original-data-preimport-audit" &&
      mode != "object-original-data-location-audit" &&
      mode != "object-original-data-denied-audit" &&
      mode != "object-original-data-server-auth-audit" &&
      mode != "object-packet-set-fetch" &&
      mode != "object-payload-fetch" &&
      mode != "object-cache-status" &&
      mode != "object-catalog-query" &&
      mode != "object-catalog-delete" && mode != "object-catalog-deleted" &&
      mode != "object-range-phase" && mode != "object-inventory-audit" &&
      mode != "object-insert-request" && mode != "object-insert-fetch" &&
      mode != "object-write-reject" &&
      mode != "network-object-wrappers" && mode != "persistent-invalid-config") {
    throw std::invalid_argument("unsupported --mode " + mode);
  }

  PersistentContext context(options);
  if (mode == "network-object-wrappers") {
    const auto source = readBytes(required(options, "source"));
    const auto object = required(options, "object-name");
    const ndn::Name prefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    const auto phase = required(options, "phase");
    const fs::path reportPath(required(options, "report-out"));
    std::ofstream report(reportPath, std::ios::binary | std::ios::trunc);
    if (!report || source.size() < 300) throw std::runtime_error("wrapper fixture/report invalid");
    report << "{\"phase\":\"" << phase << "\",\"calls\":[";
    size_t calls = 0;
    auto record = [&] (const std::string& operation, const WrapperReply& reply) {
      const auto filename = "nw-" + phase + "-" + std::to_string(calls) + "-" + operation + ".bin";
      std::ofstream payloadFile(reportPath.parent_path() / filename, std::ios::binary | std::ios::trunc);
      payloadFile.write(reinterpret_cast<const char*>(reply.payload.data()), reply.payload.size());
      payloadFile.close();
      if (!payloadFile) throw std::runtime_error("cannot save wrapper response payload");
      if (calls++) report << ",";
      report << "{\"operation\":\"" << operation << "\",\"requestId\":\""
             << reply.requestId.toUri() << "\",\"payloadFile\":\"" << filename
             << "\",\"payloadSha256\":\"" << sha256(reply.payload)
             << "\",\"status\":" << (reply.status ? "true" : "false")
             << ",\"errorCode\":\"" << reply.error.substr(0, reply.error.find(':')) << "\"}";
      report.flush();
      std::cout << "SPEC193_NETWORK_WRAPPER_RETURN operation=" << operation
                << " requestId=" << reply.requestId << std::endl;
      return reply.payload;
    };
    auto call = [&] (const std::string& operation, auto start) {
      std::cout << "SPEC193_NETWORK_WRAPPER_START operation=" << operation << std::endl;
      return record(operation, awaitWrapper(context, std::move(start)));
    };
    if (phase == "unauthorized") {
      const auto original = repo::ArtifactCommitReceipt::fromJson(repo::toString(readBytes(required(options, "receipt"))));
      if (original.reference.publisherIdentity == required(options, "user"))
        throw std::runtime_error("unauthorized fixture accidentally uses original publisher");
      repo::RepoReceiptLookupRequest query;
      query.reference = original.reference;
      query.receiptId = original.receiptId;
      query.repoNode = required(options, "repo-a");
      query.providerIdentity = provider.toUri();
      const auto reply = awaitWrapper(context, [&context, provider, prefix, query] (auto t, auto r) {
        return repo::RepoClient::requestReceipt(context.user(), provider, prefix, query, 30000, t, r);
      }, true);
      const std::string expected = "repo-publish-operation-unauthorized: requester is not the submitting principal";
      if (reply.status || reply.error != expected || repo::toString(reply.payload) != expected)
        throw std::runtime_error("unauthorized wrapper did not fail closed: " + reply.error);
      (void)record("RECEIPT", reply);
    }
    else if (phase == "malformed" || phase == "wrong-signer") {
      ndn::util::Logging::setLevel(phase == "malformed" ?
        "ndn_service_framework.ServiceUser=ERROR" : "ndn_service_framework.ServiceUser=WARN");
      repo::RepoReceiptLookupRequest query;
      query.reference = makeReference(required(options, "source"), required(options, "user"));
      query.receiptId = "spec193-fault-receipt";
      query.repoNode = required(options, "repo-a");
      query.providerIdentity = provider.toUri();
      const auto reply = awaitWrapper(context, [&context, provider, prefix, query] (auto t, auto r) {
        return repo::RepoClient::requestReceipt(context.user(), provider, prefix, query, 6000, t, r);
      }, false, true);
      if (reply.status || reply.error != "network-wrapper-timeout" || !reply.payload.empty())
        throw std::runtime_error("fault Response reached an application completion callback");
      (void)record(phase == "malformed" ? "RECEIPT_MALFORMED" : "RECEIPT_WRONG_SIGNER", reply);
    }
    else if (phase == "timeout") {
      const auto original = repo::ArtifactCommitReceipt::fromJson(repo::toString(readBytes(required(options, "receipt"))));
      repo::RepoReceiptLookupRequest query;
      query.reference = original.reference;
      query.receiptId = original.receiptId;
      query.repoNode = required(options, "repo-a");
      query.providerIdentity = provider.toUri();
      const fs::path gate(required(options, "request-gate"));
      const auto gateDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      std::cout << "SPEC193_NEGATIVE_BOOTSTRAP_READY" << std::endl;
      while (!fs::exists(gate)) {
        if (std::chrono::steady_clock::now() >= gateDeadline)
          throw std::runtime_error("timeout fixture request gate expired");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      const auto started = std::chrono::steady_clock::now();
      const auto reply = awaitWrapper(context, [&context, provider, prefix, query] (auto t, auto r) {
        return repo::RepoClient::requestReceipt(context.user(), provider, prefix, query, 6000, t, r);
      }, false, true);
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
      if (reply.status || reply.error != "network-wrapper-timeout" || !reply.payload.empty() || elapsed < 5000 || elapsed > 12000)
        throw std::runtime_error("wrapper timeout outcome or deadline is invalid");
      (void)record("RECEIPT_TIMEOUT", reply);
      std::cout << "SPEC193_NEGATIVE_TIMEOUT elapsedMs=" << elapsed << std::endl;
    }
    else if (phase == "rejected") {
      const auto operation = required(options, "operation");
      const auto manifest = repo::RepoClient::makeManifest(object, "object", source, 1, {}, "spec193-reject");
      std::vector<uint8_t> input;
      std::string expected = "repo-persistent-operation-required: ";
      if (operation == "STORE") {
        input = repo::encodeStoreRequest(manifest, source);
        expected += "STORE requires a versioned artifact request";
      }
      else if (operation == "STORE_RANGE") {
        input = repo::encodeRangeWriteRequest(manifest, {0, source.size()}, source);
        expected += "STORE_RANGE requires a versioned artifact request";
      }
      else if (operation == "COMMIT_RANGES") {
        input = repo::encodeManifestRequest(manifest);
        expected += "COMMIT_RANGES requires a versioned artifact request";
      }
      else if (operation == "ABORT_RANGES") {
        input = repo::encodeRangeAbortRequest(object);
        expected += "ABORT_RANGES requires a versioned artifact request";
      }
      else if (operation == "INSERT") {
        repo::RepoDataReference reference;
        reference.objectName = object;
        reference.dataPrefix = required(options, "user") + "/spec193/rejected-producer";
        reference.hasFinalSegment = true;
        reference.finalSegment = 0;
        reference.expectedSize = source.size();
        reference.expectedSha256 = sha256(source);
        input = repo::encodeDataReferenceRequest(reference);
        expected += "INSERT is disabled for persistent artifact repos";
      }
      else if (operation == "STORE_MANIFEST") {
        input = repo::encodeManifestRequest(manifest);
        expected += "STORE_MANIFEST is disabled for persistent artifact repos";
      }
      else if (operation == "DELETE") {
        input = repo::toBytes(object);
        expected += "DELETE is disabled for persistent artifact repos";
      }
      else throw std::invalid_argument("unknown Persistent rejection operation");
      const auto reply = awaitWrapper(context, [&context, provider, prefix, operation, input] (auto t, auto r) {
        return context.user().RequestService(std::vector<ndn::Name>{provider},
          repo::makeRepoServiceName(prefix, operation), repo::RepoClient::makeRequest(input), 30000, t, r);
      }, true);
      if (reply.status || reply.error != expected || repo::toString(reply.payload) != expected)
        throw std::runtime_error("Persistent legacy boundary response is not the expected rejection: " + reply.error);
      (void)record(operation, reply);
    }
    else if (phase == "object") {
      const auto capability = repo::toString(call("CAPABILITY",
        [&context, prefix] (auto t, auto r) { return repo::RepoClient::requestCapability(context.user(), prefix, 30000, t, r); }));
      if (capability.find("\"repoNode\":\"" + required(options, "repo-a") + "\"") == std::string::npos) throw std::runtime_error("wrapper capability owner mismatch");
      (void)repo::parseCacheStatusJson(repo::toString(call("CACHE_STATUS",
        [&context, prefix] (auto t, auto r) { return repo::RepoClient::requestCacheStatus(context.user(), prefix, 30000, t, r); })));
      const auto manifest = repo::RepoClient::makeManifest(object + "/primary", "object", source, 1, {}, "spec193-nw");
      const auto stored = repo::parseManifestJson(repo::toString(call("STORE",
        [&context, prefix, manifest, source] (auto t, auto r) { return repo::RepoClient::requestStore(context.user(), prefix, manifest, source, 30000, t, r); })));
      if (stored.sha256 != sha256(source)) throw std::runtime_error("wrapper STORE digest mismatch");
      auto ranged = manifest;
      ranged.objectName = object + "/range";
      ranged.generation = 1;
      const std::vector<uint8_t> first(source.begin(), source.begin() + 256);
      const std::vector<uint8_t> last(source.begin() + 256, source.end());
      for (const bool tail : {true, false}) {
        const auto bytes = tail ? last : first;
        const repo::RepoByteRange range{tail ? 256u : 0u, bytes.size()};
        if (repo::toString(call("STORE_RANGE", [&context, prefix, ranged, range, bytes] (auto t, auto r) {
          return repo::RepoClient::requestStoreRange(context.user(), prefix, ranged, range, bytes, 30000, t, r);
        })) != "accepted") throw std::runtime_error("wrapper range was not accepted");
      }
      const auto committed = repo::parseManifestJson(repo::toString(call("COMMIT_RANGES",
        [&context, prefix, ranged] (auto t, auto r) { return repo::RepoClient::requestCommitRanges(context.user(), prefix, ranged, 30000, t, r); })));
      if (committed.sha256 != sha256(source)) throw std::runtime_error("wrapper range commit digest mismatch");
      auto aborted = ranged;
      aborted.objectName = object + "/aborted";
      (void)call("STORE_RANGE", [&context, prefix, aborted, first] (auto t, auto r) {
        return repo::RepoClient::requestStoreRange(context.user(), prefix, aborted, {0, first.size()}, first, 30000, t, r);
      });
      if (repo::toString(call("ABORT_RANGES", [&context, prefix, aborted] (auto t, auto r) {
        return repo::RepoClient::requestAbortRanges(context.user(), prefix, aborted.objectName, 30000, t, r);
      })) != "aborted") throw std::runtime_error("wrapper abort did not finish");
      if (call("FETCH", [&context, prefix, manifest] (auto t, auto r) {
        return repo::RepoClient::requestFetch(context.user(), prefix, manifest.objectName, 30000, t, r);
      }) != source) throw std::runtime_error("wrapper FETCH differs from source");
      const std::vector<uint8_t> window(source.begin() + 37, source.begin() + 150);
      if (call("FETCH_RANGE", [&context, prefix, ranged] (auto t, auto r) {
        return repo::RepoClient::requestFetchRange(context.user(), prefix, ranged.objectName, {37, 113}, 30000, t, r);
      }) != window) throw std::runtime_error("wrapper range differs from source window");
      if (call("FETCH_RANGE_DIRECT", [&context, provider, prefix, ranged] (auto t, auto r) {
        return repo::RepoClient::requestFetchRange(context.user(), provider, prefix, ranged.objectName, {37, 113}, 30000, t, r);
      }) != window) throw std::runtime_error("direct wrapper range differs from source window");
      const auto found = repo::parseManifestJson(repo::toString(call("MANIFEST",
        [&context, prefix, ranged] (auto t, auto r) { return repo::RepoClient::requestManifest(context.user(), prefix, ranged.objectName, 30000, t, r); })));
      if (found.sha256 != committed.sha256) throw std::runtime_error("wrapper MANIFEST mismatch");
      const auto wire = readBytes(required(options, "wire-source"));
      repo::RepoDataReference reference;
      reference.objectName = object + "/insert";
      reference.dataPrefix = required(options, "data-prefix");
      reference.hasFinalSegment = true;
      reference.finalSegment = 0;
      reference.expectedSize = wire.size();
      reference.expectedSha256 = sha256(wire);
      const auto inserted = repo::parseOperationStatusJson(repo::toString(call("INSERT",
        [&context, prefix, reference] (auto t, auto r) { return repo::RepoClient::requestInsert(context.user(), prefix, reference, 30000, t, r); })));
      const auto status = repo::parseOperationStatusJson(repo::toString(call("STATUS",
        [&context, prefix, inserted] (auto t, auto r) { return repo::RepoClient::requestStatus(context.user(), prefix, inserted.operationId, 30000, t, r); })));
      if (inserted.state != "DONE" || status.state != "DONE" || status.completedSegments != 1) throw std::runtime_error("wrapper INSERT/STATUS incomplete");
      const auto packetName = ndn::Name(reference.dataPrefix).appendSegment(0).toUri();
      if (call("FETCH_DATA", [&context, prefix, packetName] (auto t, auto r) {
        return repo::RepoClient::requestFetch(context.user(), prefix, packetName, 30000, t, r);
      }) != wire) throw std::runtime_error("wrapper FETCH changed original signed Data");
      if (repo::parseInventoryJson(repo::toString(call("INVENTORY",
        [&context, prefix] (auto t, auto r) { return repo::RepoClient::requestInventory(context.user(), prefix, 30000, t, r); }))).size() != 4) throw std::runtime_error("wrapper inventory count mismatch");
      if (repo::toString(call("DELETE", [&context, prefix, manifest] (auto t, auto r) {
        return repo::RepoClient::requestDelete(context.user(), prefix, manifest.objectName, 30000, t, r);
      })) != "deleted") throw std::runtime_error("wrapper DELETE failed");
    }
    else if (phase == "artifact") {
      const repo::RepoArtifactTarget target{required(options, "repo-a"), provider.toUri(), prefix.toUri()};
      for (const bool ranged : {false, true}) {
        auto reference = makeReference(required(options, "source"), required(options, "user"));
        reference.logicalName = object + (ranged ? "/range" : "/whole");
        reference.validate();
        repo::PublishOperationIntent intent;
        intent.reference = reference;
        intent.generation = 1;
        intent.seed = target;
        intent.targets = {target};
        intent.consistency = "ALL";
        intent.idempotencyKeyDigest = repo::digestPublishIdempotencyKey(reference.logicalName);
        intent.requestDigest = repo::makePublishRequestDigest(reference, 1, target, "ALL", {target});
        intent.validate();
        const auto operationId = repo::makePublishOperationId(required(options, "user"), target, intent.idempotencyKeyDigest);
        auto admitted = awaitWrapper(context, [&context, provider, prefix, intent] (auto t, auto r) {
          return context.user().RequestService(std::vector<ndn::Name>{provider},
            repo::makeRepoServiceName(prefix, "STORE"), repo::RepoClient::makeRequest(repo::toBytes(intent.toJson())),
            30000, t, r);
        });
        if (repo::toString(record("PUBLISH_INTENT", admitted)) != operationId)
          throw std::runtime_error("wrapper publish intent operation binding failed");
        std::vector<uint8_t> receiptWire;
        if (!ranged) {
          repo::ArtifactStoreRequest request;
          request.operationId = operationId;
          request.generation = 1;
          request.reference = reference;
          request.bytes = source;
          receiptWire = call("ARTIFACT_STORE", [&context, prefix, request] (auto t, auto r) {
            return repo::RepoClient::requestArtifactStore(context.user(), prefix, request, 30000, t, r);
          });
        }
        else {
          for (const bool tail : {true, false}) {
            repo::ArtifactRangeWriteRequest request;
            request.operationId = operationId;
            request.generation = 1;
            request.reference = reference;
            request.offsetBytes = tail ? 256 : 0;
            request.lengthBytes = tail ? source.size() - 256 : 256;
            request.bytes.assign(source.begin() + request.offsetBytes,
                                 source.begin() + request.offsetBytes + request.lengthBytes);
            if (repo::toString(call("ARTIFACT_STORE_RANGE", [&context, prefix, request] (auto t, auto r) {
              return repo::RepoClient::requestArtifactStoreRange(context.user(), prefix, request, 30000, t, r);
            })) != "staged") throw std::runtime_error("artifact wrapper range not staged");
          }
          repo::ArtifactRangeCommitRequest request;
          request.operationId = operationId;
          request.generation = 1;
          request.reference = reference;
          receiptWire = call("ARTIFACT_COMMIT_RANGES", [&context, prefix, request] (auto t, auto r) {
            return repo::RepoClient::requestArtifactCommitRanges(context.user(), prefix, request, 30000, t, r);
          });
        }
        const auto receipt = repo::RepoArtifactCommitReceipt::fromJson(repo::toString(receiptWire));
        if (receipt.reference != reference || receipt.operationId != operationId ||
            receipt.repoNode != target.repoNode || receipt.providerIdentity != target.providerIdentity ||
            receipt.generation != 1 || receipt.persistedBytes != source.size())
          throw std::runtime_error("artifact wrapper receipt binding failed");
        repo::PublishOperationStatusRequest statusRequest;
        statusRequest.operationId = operationId;
        repo::PublishOperationStatus publishStatus;
        const auto statusDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
          publishStatus = repo::PublishOperationStatus::fromJson(repo::toString(call("PUBLISH_STATUS",
            [&context, provider, prefix, statusRequest] (auto t, auto r) {
              return context.user().RequestService(std::vector<ndn::Name>{provider},
                repo::makeRepoServiceName(prefix, "STATUS"),
                repo::RepoClient::makeRequest(repo::toBytes(statusRequest.toJson())), 30000, t, r);
            })));
          if (publishStatus.state == "COMMITTED") break;
          if (publishStatus.state == "FAILED" || publishStatus.state == "PARTIAL")
            throw std::runtime_error("artifact wrapper publish terminal failure");
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        } while (std::chrono::steady_clock::now() < statusDeadline);
        if (publishStatus.state != "COMMITTED" || publishStatus.reference != reference ||
            publishStatus.operationId != operationId || publishStatus.receiptId.empty())
          throw std::runtime_error("artifact wrapper aggregate commit missing");
        repo::ArtifactLocationRequest locationRequest;
        locationRequest.reference = reference;
        const auto location = repo::ArtifactLocationRecord::fromJson(repo::toString(call("LOCATION",
          [&context, provider, prefix, locationRequest] (auto t, auto r) {
            return repo::RepoClient::requestLocation(context.user(), provider, prefix, locationRequest, 30000, t, r);
          })));
        if (location.reference != reference || location.receiptId != receipt.receiptId ||
            location.operationId != operationId || location.providerIdentity != target.providerIdentity)
          throw std::runtime_error("artifact wrapper current location binding failed");
        repo::RepoReceiptLookupRequest receiptRequest;
        receiptRequest.reference = reference;
        receiptRequest.receiptId = publishStatus.receiptId;
        receiptRequest.repoNode = target.repoNode;
        receiptRequest.providerIdentity = target.providerIdentity;
        const auto historical = repo::ArtifactCommitReceipt::fromJson(repo::toString(call("RECEIPT",
          [&context, provider, prefix, receiptRequest] (auto t, auto r) {
            return repo::RepoClient::requestReceipt(context.user(), provider, prefix, receiptRequest, 30000, t, r);
          })));
        if (historical.receiptId != publishStatus.receiptId || historical.operationId != operationId ||
            historical.reference != reference || historical.state != "COMMITTED")
          throw std::runtime_error("artifact wrapper historical receipt binding failed");
        repo::RepoAvailabilityProbeRequest probe;
        probe.reference = reference;
        probe.expectedLocation = {location.repoNode, location.providerIdentity, location.operationId,
                                  location.receiptId, location.generation, location.locationVersion};
        probe.probeKind = "verify_content";
        const auto availability = repo::RepoAvailabilityProbeResponse::fromJson(repo::toString(call("AVAILABILITY",
          [&context, provider, prefix, probe] (auto t, auto r) {
            return repo::RepoClient::requestAvailability(context.user(), provider, prefix, probe, 30000, t, r);
          })));
        if (availability.reference != reference || availability.expectedLocation.toJson() != probe.expectedLocation.toJson() ||
            availability.report.repoNode != target.repoNode || availability.report.providerIdentity != target.providerIdentity ||
            availability.report.receiptId != location.receiptId || availability.report.operationId != operationId ||
            availability.report.reachability != "REACHABLE" ||
            availability.report.objectPresence != "PRESENT" || availability.report.integrity != "VERIFIED")
          throw std::runtime_error("artifact wrapper current availability failed");
        if (call("ARTIFACT_FETCH", [&context, provider, prefix, reference, source] (auto t, auto r) {
          return repo::RepoClient::requestFetchRange(context.user(), provider, prefix,
            repo::artifactStorageObjectName(reference), {0, source.size()}, 30000, t, r);
        }) != source) throw std::runtime_error("artifact wrapper bytes differ from source");
      }
    }
    else if (phase == "missing" || phase == "restored") {
      const auto historicalWire = readBytes(required(options, "receipt"));
      const auto historical = repo::ArtifactCommitReceipt::fromJson(repo::toString(historicalWire));
      const auto locationWire = readBytes(required(options, "location"));
      const auto originalLocation = repo::ArtifactLocationRecord::fromJson(repo::toString(locationWire));
      const auto reference = historical.reference;
      if (originalLocation.reference != reference || originalLocation.operationId != historical.operationId ||
          originalLocation.repoNode != required(options, "repo-a") || originalLocation.providerIdentity != provider.toUri())
        throw std::runtime_error("metadata wrapper fixture owner/reference mismatch");
      repo::RepoReceiptLookupRequest lookup;
      lookup.reference = reference;
      lookup.receiptId = historical.receiptId;
      lookup.repoNode = originalLocation.repoNode;
      lookup.providerIdentity = originalLocation.providerIdentity;
      if (call("RECEIPT", [&context, provider, prefix, lookup] (auto t, auto r) {
        return repo::RepoClient::requestReceipt(context.user(), provider, prefix, lookup, 30000, t, r);
      }) != historicalWire) throw std::runtime_error("physical loss changed historical aggregate receipt");
      repo::ArtifactLocationRequest query;
      query.reference = reference;
      if (call("LOCATION", [&context, provider, prefix, query] (auto t, auto r) {
        return repo::RepoClient::requestLocation(context.user(), provider, prefix, query, 30000, t, r);
      }) != locationWire) throw std::runtime_error("physical loss changed stored location tuple");
      repo::RepoAvailabilityProbeRequest probe;
      probe.reference = reference;
      probe.expectedLocation = {originalLocation.repoNode, originalLocation.providerIdentity,
        originalLocation.operationId, originalLocation.receiptId, originalLocation.generation, originalLocation.locationVersion};
      probe.probeKind = "verify_content";
      const auto observation = repo::RepoAvailabilityProbeResponse::fromJson(repo::toString(call("AVAILABILITY",
        [&context, provider, prefix, probe] (auto t, auto r) {
          return repo::RepoClient::requestAvailability(context.user(), provider, prefix, probe, 30000, t, r);
        })));
      if (observation.reference != reference || observation.expectedLocation.toJson() != probe.expectedLocation.toJson() ||
          observation.report.repoNode != originalLocation.repoNode || observation.report.providerIdentity != provider.toUri() ||
          observation.report.receiptId != originalLocation.receiptId || observation.report.operationId != historical.operationId ||
          observation.report.reachability != "REACHABLE" ||
          observation.report.objectPresence != (phase == "missing" ? "PARTIAL" : "PRESENT") ||
          observation.report.integrity != (phase == "missing" ? "NOT_CHECKED" : "VERIFIED"))
        throw std::runtime_error("metadata wrapper did not distinguish current physical availability");
      if (phase == "restored" && call("ARTIFACT_FETCH", [&context, provider, prefix, reference, source] (auto t, auto r) {
        return repo::RepoClient::requestFetchRange(context.user(), provider, prefix,
          repo::artifactStorageObjectName(reference), {0, source.size()}, 30000, t, r);
      }) != source) throw std::runtime_error("restored artifact wrapper fetch differs from source");
    }
    else if (phase == "catalog") {
      for (const std::string operation : {"CATALOG_STATUS", "CATALOG_SNAPSHOT", "CATALOG_DELTA", "CATALOG_LOOKUP"}) {
        std::vector<uint8_t> input;
        if (operation == "CATALOG_DELTA") input = repo::encodeCatalogDeltaRequest(0);
        if (operation == "CATALOG_LOOKUP") input = repo::encodeCatalogLookupRequest(object + "/range");
        const auto bytes = repo::RepoClient::requestSelectedService(context.user(), provider,
          repo::makeRepoServiceName(prefix, operation), input, 1000, 30000);
        (void)record(operation, {{}, bytes, true, {}});
      }
    }
    else throw std::invalid_argument("unknown wrapper phase");
    report << "]}";
    report.close();
    if (!report) throw std::runtime_error("cannot finish wrapper report");
    std::cout << "SPEC193_NETWORK_WRAPPERS_PASS phase=" << phase << " count=" << calls << std::endl;
    return 0;
  }
  if (mode == "persistent-invalid-config") {
    repo::PersistentRepoClientConfig config;
    config.requestTimeoutMs = config.ackCollectionTimeMs;
    try {
      (void)repo::ArtifactRepositoryApi::makePersistent(context.user(), config);
    }
    catch (const std::invalid_argument& error) {
      if (std::string(error.what()).find("repo-persistent-client-config-invalid") !=
          std::string::npos) {
        std::cout << "SPEC193_INVALID_INPUT_REJECTED case=persistent-deadline reason="
                  << error.what() << '\n';
        return 0;
      }
      throw;
    }
    throw std::runtime_error("invalid Persistent configuration was accepted");
  }
  if (mode == "object-write-reject") {
    const auto payload = readBytes(required(options, "source"));
    const auto manifest = repo::RepoClient::makeManifest(
      required(options, "object-name"), "application/octet-stream", payload,
      1, {required(options, "repo-a")}, "spec193-persistent-boundary");
    try {
      (void)repo::RepoClient::requestSelectedService(
        context.user(), ndn::Name(required(options, "provider-a")),
        repo::makeRepoServiceName(
          ndn::Name(required(options, "service-prefix")), "STORE"),
        repo::encodeStoreRequest(manifest, payload), 15000, 120000);
    }
    catch (const std::exception& exception) {
      if (std::string(exception.what()).find("repo-persistent-operation-required") !=
          std::string::npos) {
        std::cout << "SPEC193_PERSISTENT_OBJECT_REJECT_PASS object="
                  << manifest.objectName << " provider="
                  << required(options, "provider-a") << '\n';
        return 0;
      }
      throw;
    }
    throw std::runtime_error("Persistent Repo accepted an unversioned object STORE");
  }
  if (mode == "object-store-fetch") {
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    const auto payload = readBytes(required(options, "source"));
    const auto objectName = required(options, "object-name");
    const auto manifest = repo::RepoClient::makeManifest(
      objectName, "application/octet-stream", payload, 1,
      {required(options, "repo-a")}, "spec193-object-baseline");
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    const auto capability = probeObjectCapability(
      context, provider, repo::makeRepoServiceName(servicePrefix, "CAPABILITY"),
      required(options, "repo-a"));
    if (repo::toString(capability.response).find(required(options, "repo-a")) ==
        std::string::npos) {
      throw std::runtime_error("selected object Repo capability identity mismatch");
    }
    std::cout << "SPEC193_OBJECT_CAPABILITY request_id=" << capability.requestId
              << " ack_digest=" << capability.ackWireDigest
              << " locator=" << capability.signerKeyLocator
              << " receipts=0" << '\n';
    const auto stored = repo::parseManifestJson(repo::toString(selected(
      "STORE", repo::encodeStoreRequest(manifest, payload))));
    if (stored.objectName != objectName || stored.sha256 != sha256(payload) ||
        stored.size != payload.size()) {
      throw std::runtime_error("object Repo durable manifest differs from source");
    }
    const auto fetched = selected("FETCH", repo::toBytes(objectName));
    if (fetched != payload) {
      throw std::runtime_error("object Repo fetch bytes differ from source");
    }
    const auto inventory = repo::parseInventoryJson(repo::toString(
      selected("INVENTORY", {})));
    if (std::count_if(inventory.begin(), inventory.end(),
                      [&] (const auto& item) {
                        return item.objectName == objectName &&
                               item.sha256 == stored.sha256;
                      }) != 1) {
      throw std::runtime_error("object Repo inventory lacks the committed object");
    }
    const auto destination = fs::path(required(options, "destination"));
    fs::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(fetched.data()), fetched.size());
    if (!output) {
      throw std::runtime_error("cannot write object Repo destination");
    }
    std::cout << "SPEC193_OBJECT_NETWORK_PASS object=" << objectName
              << " sha256=" << sha256(payload) << " provider=" << provider << '\n';
    return 0;
  }
  if (mode == "object-reopen-fetch") {
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    const auto objectName = required(options, "object-name");
    const auto expectedSha = required(options, "expected-sha256");
    const auto expectedSize = std::stoull(required(options, "expected-size"));
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    const auto manifest = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(objectName))));
    const auto inventory = repo::parseInventoryJson(repo::toString(
      selected("INVENTORY", {})));
    const auto fetched = selected("FETCH", repo::toBytes(objectName));
    if (manifest.objectName != objectName || manifest.sha256 != expectedSha ||
        manifest.size != expectedSize || fetched.size() != expectedSize ||
        sha256(fetched) != expectedSha ||
        std::count_if(inventory.begin(), inventory.end(),
          [&] (const auto& item) { return item.toJson() == manifest.toJson(); }) != 1) {
      throw std::runtime_error("reopened Repo object differs from prior MiniNDN run");
    }
    const auto destination = fs::path(required(options, "destination"));
    fs::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(fetched.data()), fetched.size());
    output.close();
    if (!output) throw std::runtime_error("cannot save reopened Repo object");
    if (options.count("manifest-out"))
      writeText(options.at("manifest-out"), manifest.toJson());
    std::cout << "SPEC193_OBJECT_REOPEN_PASS object=" << objectName
              << " sha256=" << expectedSha << " size=" << expectedSize << '\n';
    return 0;
  }
  if (mode == "object-version-fetch") {
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    const auto objectName = required(options, "object-name");
    const auto expected = readBytes(required(options, "source"));
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    const auto manifest = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(objectName))));
    const auto inventory = repo::parseInventoryJson(repo::toString(
      selected("INVENTORY", {})));
    const auto fetched = selected("FETCH", repo::toBytes(objectName));
    if (manifest.objectName != objectName || manifest.generation != 3 ||
        manifest.objectType != "application/octet-stream" ||
        manifest.sha256 != sha256(expected) ||
        manifest.size != expected.size() || fetched != expected ||
        std::count_if(inventory.begin(), inventory.end(),
          [&] (const auto& item) {
            return item.objectName == objectName &&
                   item.toJson() == manifest.toJson();
          }) != 1) {
      throw std::runtime_error("O01 network manifest/inventory/fetch disagree");
    }
    const auto destination = fs::path(required(options, "destination"));
    fs::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(fetched.data()), fetched.size());
    output.close();
    if (!output) throw std::runtime_error("cannot save O01 fetched bytes");
    std::ofstream report(required(options, "report-out"),
                         std::ios::binary | std::ios::trunc);
    report << "{\"manifest\":" << manifest.toJson()
           << ",\"inventoryCount\":" << inventory.size() << "}";
    report.close();
    if (!report) throw std::runtime_error("cannot save O01 network report");
    std::cout << "SPEC193_O01_NETWORK_PASS generation=" << manifest.generation
              << " sha256=" << manifest.sha256 << std::endl;
    return 0;
  }
  if (mode == "object-range-read") {
    const auto objectName = required(options, "object-name");
    const auto expected = readBytes(required(options, "source"));
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    const auto manifest = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(objectName))));
    const auto inventory = repo::parseInventoryJson(repo::toString(
      selected("INVENTORY", {})));
    const auto complete = selected("FETCH", repo::toBytes(objectName));
    const auto range = [&] (size_t start, size_t length) {
      const auto fetched = selected(
        "FETCH_RANGE", repo::encodeRangeReadRequest(
          objectName, repo::RepoByteRange{start, length}));
      return fetched == std::vector<uint8_t>(expected.begin() + start,
                                             expected.begin() + start + length);
    };
    if (manifest.objectName != objectName || manifest.generation != 1 ||
        manifest.size != expected.size() || manifest.sha256 != sha256(expected) ||
        complete != expected || !range(0, 101) || !range(73, 281) ||
        !range(318, expected.size() - 318) ||
        std::count_if(inventory.begin(), inventory.end(),
          [&] (const auto& item) {
            return item.objectName == objectName &&
                   item.toJson() == manifest.toJson();
          }) != 1) {
      throw std::runtime_error("O02 network range or manifest differs from source");
    }
    const auto destination = fs::path(required(options, "destination"));
    fs::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(complete.data()), complete.size());
    output.close();
    if (!output) throw std::runtime_error("cannot save O02 fetched bytes");
    std::ofstream report(required(options, "report-out"),
                         std::ios::binary | std::ios::trunc);
    report << "{\"manifest\":" << manifest.toJson()
           << ",\"rangeChecks\":[[0,101],[73,281],[318,"
           << expected.size() - 318 << "]]}";
    report.close();
    if (!report) throw std::runtime_error("cannot save O02 report");
    std::cout << "SPEC193_O02_NETWORK_PASS size=" << expected.size() << std::endl;
    return 0;
  }
  if (mode == "object-range-rejection-audit") {
    const auto baseName = required(options, "object-name");
    const auto expected = readBytes(required(options, "source"));
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    const auto ownedName = baseName + "/owned";
    const auto manifest = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(ownedName))));
    const auto fetched = selected("FETCH", repo::toBytes(ownedName));
    const auto inventory = repo::parseInventoryJson(repo::toString(
      selected("INVENTORY", {})));
    if (manifest.objectName != ownedName || manifest.size != expected.size() ||
        manifest.sha256 != sha256(expected) || fetched != expected ||
        std::count_if(inventory.begin(), inventory.end(),
          [&] (const auto& item) { return item.objectName == ownedName; }) != 1) {
      throw std::runtime_error("O03 rightful owner result unavailable after restart");
    }
    for (const std::string suffix : {"incomplete", "aborted", "bounds"}) {
      const auto missingName = baseName + "/" + suffix;
      if (std::any_of(inventory.begin(), inventory.end(),
                      [&] (const auto& item) {
                        return item.objectName == missingName;
                      })) {
        throw std::runtime_error("O03 rejected object appeared in network inventory");
      }
      for (const std::string operation : {"MANIFEST", "FETCH"}) {
        bool rejected = false;
        try {
          (void)selected(operation, repo::toBytes(missingName));
        }
        catch (const std::exception& error) {
          rejected = std::string(error.what()).find("repo-object-not-found") !=
                     std::string::npos;
        }
        if (!rejected) {
          throw std::runtime_error("O03 missing range object was readable: " +
                                   suffix + " " + operation);
        }
      }
    }
    const auto destination = fs::path(required(options, "destination"));
    fs::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(fetched.data()), fetched.size());
    output.close();
    if (!output) throw std::runtime_error("cannot save O03 committed bytes");
    std::ofstream report(required(options, "report-out"),
                         std::ios::binary | std::ios::trunc);
    report << "{\"committed\":" << manifest.toJson()
           << ",\"rejectedNames\":3,\"rejectedOperations\":6}";
    report.close();
    if (!report) throw std::runtime_error("cannot save O03 network report");
    std::cout << "SPEC193_O03_NETWORK_PASS rejected=3" << std::endl;
    return 0;
  }
  if (mode == "object-remove-audit") {
    const auto objectName = required(options, "object-name");
    const auto expected = readBytes(required(options, "source"));
    const auto sinceEpoch = std::stoull(required(options, "since-epoch"));
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    const auto status = repo::parseCatalogStatusJson(repo::toString(
      selected("CATALOG_STATUS", {})));
    const auto snapshot = repo::parseCatalogDeltaJson(repo::toString(
      selected("CATALOG_SNAPSHOT", {})));
    const auto delta = repo::parseCatalogDeltaJson(repo::toString(
      selected("CATALOG_DELTA", repo::encodeCatalogDeltaRequest(sinceEpoch))));
    const auto inventory = repo::parseInventoryJson(repo::toString(
      selected("INVENTORY", {})));
    if (!status.reconciliationRequired || status.objectCount != 0 ||
        snapshot.entries.size() != 0 || delta.entries.size() != 0 ||
        std::any_of(inventory.begin(), inventory.end(),
          [&] (const auto& item) { return item.objectName == objectName; })) {
      throw std::runtime_error("O04 restarted Repo exposed deleted object or hid reconciliation");
    }
    for (const std::string operation : {"MANIFEST", "FETCH", "CATALOG_LOOKUP"}) {
      bool rejected = false;
      try {
        (void)selected(operation, repo::toBytes(objectName));
      }
      catch (const std::exception& error) {
        rejected = std::string(error.what()).find("repo-object-not-found") !=
                   std::string::npos;
      }
      if (!rejected) {
        throw std::runtime_error("O04 deleted object was readable via " + operation);
      }
    }
    std::ofstream report(required(options, "report-out"),
                         std::ios::binary | std::ios::trunc);
    report << "{\"status\":" << status.toJson()
           << ",\"snapshot\":" << snapshot.toJson()
           << ",\"delta\":" << delta.toJson()
           << ",\"sourceSha256\":\"" << sha256(expected) << "\""
           << ",\"rejectedOperations\":3}";
    report.close();
    if (!report) throw std::runtime_error("cannot save O04 network report");
    std::cout << "SPEC193_O04_NETWORK_PASS rejected=3" << std::endl;
    return 0;
  }
  if (mode == "object-corruption-audit") {
    const auto objectName = required(options, "object-name");
    const auto phase = required(options, "expect");
    if (phase != "corrupt" && phase != "restored") {
      throw std::invalid_argument("O05 phase must be corrupt or restored");
    }
    const auto expected = readBytes(required(options, "source"));
    const auto destination = fs::path(required(options, "destination"));
    const auto oldDestination = readBytes(destination);
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    const auto missing = objectName + "/absent";
    for (const std::string operation : {"MANIFEST", "FETCH"}) {
      bool rejected = false;
      try {
        (void)selected(operation, repo::toBytes(missing));
      }
      catch (const std::exception& error) {
        rejected = std::string(error.what()).find("repo-object-not-found") !=
                   std::string::npos;
      }
      if (!rejected || readBytes(destination) != oldDestination) {
        throw std::runtime_error("O05 missing object changed destination or returned data");
      }
    }
    const auto manifest = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(objectName))));
    const auto inventory = repo::parseInventoryJson(repo::toString(
      selected("INVENTORY", {})));
    if (manifest.objectName != objectName || manifest.size != expected.size() ||
        manifest.sha256 != sha256(expected) ||
        std::count_if(inventory.begin(), inventory.end(),
          [&] (const auto& item) {
            return item.objectName == objectName &&
                   item.toJson() == manifest.toJson();
          }) != 1 ||
        std::any_of(inventory.begin(), inventory.end(),
          [&] (const auto& item) { return item.objectName == missing; })) {
      throw std::runtime_error("O05 manifest or inventory misstates object identity");
    }
    if (phase == "corrupt") {
      bool rejected = false;
      try {
        (void)selected("FETCH", repo::toBytes(objectName));
      }
      catch (const std::exception& error) {
        rejected = std::string(error.what()).find("corrupt") != std::string::npos;
      }
      if (!rejected || readBytes(destination) != oldDestination) {
        throw std::runtime_error("O05 corrupt payload was returned or destination changed");
      }
    }
    else {
      const auto fetched = selected("FETCH", repo::toBytes(objectName));
      if (fetched != expected) {
        throw std::runtime_error("O05 restored payload differs from source");
      }
      std::ofstream output(destination, std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char*>(fetched.data()), fetched.size());
      output.close();
      if (!output) throw std::runtime_error("cannot save O05 restored bytes");
    }
    std::ofstream report(required(options, "report-out"),
                         std::ios::binary | std::ios::trunc);
    report << "{\"manifest\":" << manifest.toJson()
           << ",\"phase\":\"" << phase << "\""
           << ",\"missingRejected\":true"
           << ",\"corruptRejected\":"
           << (phase == "corrupt" ? "true" : "null") << "}";
    report.close();
    if (!report) throw std::runtime_error("cannot save O05 report");
    std::cout << "SPEC193_O05_NETWORK_PASS phase=" << phase << std::endl;
    return 0;
  }
  if (mode == "object-segmented-fetch") {
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    const auto expected = readBytes(required(options, "source"));
    const auto objectName = required(options, "object-name");
    const auto expectation = options.count("expect") ? options.at("expect") : "present";
    const fs::path destination = required(options, "destination");
    const bool existed = fs::exists(destination);
    const auto original = existed ? readBytes(destination) : std::vector<uint8_t>{};
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    const auto manifest = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(objectName))));
    if (manifest.objectName != objectName || manifest.segmentCount < 3 ||
        manifest.size != expected.size() || manifest.sha256 != sha256(expected)) {
      throw std::runtime_error("segmented Repo parent manifest mismatches source");
    }
    try {
      std::vector<uint8_t> rebuilt;
      rebuilt.reserve(expected.size());
      for (uint32_t index = 0; index < manifest.segmentCount; ++index) {
        const auto segment = selected("FETCH", repo::toBytes(
          objectName + "/seg/" + std::to_string(index)));
        rebuilt.insert(rebuilt.end(), segment.begin(), segment.end());
      }
      if (rebuilt.size() != manifest.size || sha256(rebuilt) != manifest.sha256) {
        throw std::runtime_error("segmented Repo assembled digest mismatch");
      }
      if (expectation != "present") {
        throw std::runtime_error("segmented Repo unexpectedly returned complete content");
      }
      fs::create_directories(destination.parent_path());
      std::ofstream output(destination, std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char*>(rebuilt.data()), rebuilt.size());
      if (!output) throw std::runtime_error("cannot write segmented destination");
      std::cout << "SPEC193_SEGMENTED_NETWORK_PASS count=" << manifest.segmentCount
                << " sha256=" << sha256(rebuilt) << std::endl;
      return 0;
    }
    catch (const std::exception& error) {
      const std::string reason = error.what();
      const bool expectedFault = (expectation == "missing" &&
        (reason.find("not-found") != std::string::npos ||
         reason.find("not found") != std::string::npos)) ||
        (expectation == "corrupt" &&
         reason.find("assembled digest mismatch") != std::string::npos);
      if (!expectedFault || fs::exists(destination) != existed ||
          (existed && readBytes(destination) != original) ||
          hasFetchStagingFile(destination)) {
        throw;
      }
      std::cout << "SPEC193_SEGMENTED_NETWORK_REJECTED fault=" << expectation
                << " reason=" << reason << std::endl;
      return 0;
    }
  }
  if (mode == "object-original-data-server-auth-audit") {
    const auto dataName = required(options, "object-name");
    const auto expectedWire = readBytes(required(options, "wire-source"));
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const auto allowedService = repo::makeRepoServiceName(servicePrefix, "FETCH");
    const auto controllerVersion = context.user().getControllerVersion(allowedService);
    const auto policyEpoch = context.user().getCurrentPolicyEpoch(allowedService);
    if (!controllerVersion || policyEpoch == 0 ||
        context.user().getControllerVersion(
          repo::makeRepoServiceName(servicePrefix, "DATA_RECEIPT")) ||
        context.user().getControllerVersion(
          repo::makeRepoServiceName(servicePrefix, "DATA_LOCATION"))) {
      throw std::runtime_error("server auth audit lacks an isolated partial ACL");
    }
    const repo::RepoDataPacketQuery query{1, dataName, sha256(expectedWire)};
    const ndn::Name requester(required(options, "user"));
    for (const std::string operation : {"DATA_RECEIPT", "DATA_LOCATION"}) {
      const auto service = repo::makeRepoServiceName(servicePrefix, operation);
      auto request = repo::RepoClient::makeRequest(repo::toBytes(query.toJson()));
      request.setControllerVersion(*controllerVersion);
      request.setPolicyEpoch(policyEpoch);
      // Bypass the public client's admission check. The server must reject
      // this signed, encrypted request without an authorization token.
      const ndn::Name requestId("/spec193-server-auth-" +
        sha256(repo::toBytes(dataName + operation)).substr(0, 32));
      const auto requestName = nsf::makeRequestNameV2(requester, service, requestId);
      const auto suffix = nsf::makeRequestNameWithoutPrefixV2(service, requestId);
      const auto parsed = nsf::parseRequestNameV2(requestName);
      if (!parsed || parsed->requesterName != requester ||
          parsed->serviceName != service || parsed->requestId != requestId) {
        throw std::runtime_error("server auth request name does not round-trip");
      }
      context.user().PublishMessage(requestName, suffix, request);
      std::cout << "SPEC193_SERVER_AUTH_REQUEST_PUBLISHED operation=" << operation
                << " requestId=" << requestId << " name=" << requestName << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    return 0;
  }
  if (mode == "object-original-data-denied-audit") {
    std::cout << "SPEC193_ORIGINAL_DATA_DENIAL_BOOTSTRAP_READY\n";
    const auto dataName = required(options, "object-name");
    const auto expectedWire = readBytes(required(options, "wire-source"));
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    const repo::RepoDataPacketQuery query{1, dataName, sha256(expectedWire)};
    const auto request = repo::toBytes(query.toJson());
    for (const std::string operation : {"DATA_RECEIPT", "DATA_LOCATION"}) {
      try {
        (void)repo::RepoClient::requestSelectedService(
          context.user(), provider,
          repo::makeRepoServiceName(servicePrefix, operation),
          request, 1000, 15000);
      }
      catch (const std::exception& error) {
        std::cout << "SPEC193_ORIGINAL_DATA_QUERY_DENIED operation=" << operation
                  << " reason=" << error.what() << "\n";
        continue;
      }
      throw std::runtime_error("unauthorized original Data query succeeded: " + operation);
    }
    return 0;
  }
  if (mode == "object-original-data-preimport-audit") {
    const auto dataName = required(options, "object-name");
    const auto expectedWire = readBytes(required(options, "wire-source"));
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    const repo::RepoDataPacketQuery query{1, dataName, sha256(expectedWire)};
    const auto request = repo::toBytes(query.toJson());
    for (const std::string operation : {"DATA_RECEIPT", "DATA_LOCATION"}) {
      try {
        (void)repo::RepoClient::requestSelectedService(
          context.user(), provider,
          repo::makeRepoServiceName(servicePrefix, operation),
          request, 1000, 15000);
      }
      catch (const std::exception& error) {
        if (std::string(error.what()).find("repo-data-receipt-not-found") ==
            std::string::npos) {
          throw;
        }
        std::cout << "SPEC193_LEGACY_DATA_HIDDEN operation=" << operation << "\n";
        continue;
      }
      throw std::runtime_error("unimported Data exposed metadata: " + operation);
    }
    return 0;
  }
  if (mode == "object-original-data-location-audit") {
    const auto dataName = required(options, "object-name");
    const auto expectedWire = readBytes(required(options, "wire-source"));
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    const auto repoNode = required(options, "repo-a");
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 1000, 30000);
    };
    const repo::RepoDataPacketQuery query{1, dataName, sha256(expectedWire)};
    const auto request = repo::toBytes(query.toJson());
    const auto receipt = repo::RepoWriteReceipt::dataPacketFromJson(
      repo::toString(selected("DATA_RECEIPT", request)));
    const auto before = repo::RepoDataPacketLocation::fromJson(
      repo::toString(selected("DATA_LOCATION", request)));
    const auto expectedState = options.count("expected-state") ?
      required(options, "expected-state") : "PRESENT";
    const auto expectedHint = expectedState == "PRESENT" ? provider.toUri() : "";
    if (receipt.repoNode != repoNode || receipt.providerIdentity != provider.toUri() ||
        receipt.objectName != dataName || receipt.digest != query.expectedWireSha256 ||
        before.receiptId != receipt.operationId || before.state != expectedState ||
        before.forwardingHint != expectedHint) {
      throw std::runtime_error("original Data receipt/location pre-delete mismatch");
    }
    if (expectedState == "UNKNOWN") {
      std::cout << "SPEC193_ORIGINAL_DATA_UNKNOWN_LOCATION_PASS name=" << dataName
                << " receipt=" << receipt.operationId << "\n";
      return 0;
    }
    if (expectedState != "PRESENT") {
      throw std::invalid_argument("unsupported original Data expected state");
    }
    bool rejectedVersion = false;
    try {
      auto invalid = query;
      invalid.schemaVersion = 2;
      (void)selected("DATA_LOCATION", repo::toBytes(invalid.toJson()));
    }
    catch (const std::exception& error) {
      rejectedVersion = std::string(error.what()).find("repo-data-query-invalid") !=
                        std::string::npos;
    }
    bool rejectedDigest = false;
    try {
      auto invalid = query;
      invalid.expectedWireSha256 = std::string(64, '0');
      (void)selected("DATA_RECEIPT", repo::toBytes(invalid.toJson()));
    }
    catch (const std::exception& error) {
      rejectedDigest = std::string(error.what()).find("repo-data-receipt-not-found") !=
                       std::string::npos;
    }
    if (!rejectedVersion || !rejectedDigest ||
        repo::toString(selected("DELETE", repo::toBytes(dataName))) != "deleted") {
      throw std::runtime_error("original Data negative query or deletion failed");
    }
    const auto retained = repo::RepoWriteReceipt::dataPacketFromJson(
      repo::toString(selected("DATA_RECEIPT", request)));
    const auto after = repo::RepoDataPacketLocation::fromJson(
      repo::toString(selected("DATA_LOCATION", request)));
    if (retained.operationId != receipt.operationId ||
        after.receiptId != receipt.operationId || after.state != "ABSENT" ||
        !after.forwardingHint.empty()) {
      throw std::runtime_error("deleted original Data lost its receipt or remained PRESENT");
    }
    std::cout << "SPEC193_ORIGINAL_DATA_LOCATION_AUDIT_PASS name=" << dataName
              << " receipt=" << receipt.operationId << "\n";
    return 0;
  }
  if (mode == "object-original-data-fetch") {
    const auto dataName = required(options, "object-name");
    const auto expectedWire = readBytes(required(options, "wire-source"));
    const auto expectedPayload = readBytes(required(options, "source"));
    const auto certificateWire = readBytes(required(options, "certificate"));
    ndn::Block certBlock(ndn::span<const uint8_t>(certificateWire.data(), certificateWire.size()));
    certBlock.parse();
    const ndn::security::Certificate certificate(certBlock);
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    std::vector<repo::OriginalDataReplica> replicas{
      {required(options, "repo-a"), ndn::Name(required(options, "provider-a")),
       servicePrefix}};
    if (options.count("duplicate-provider")) {
      replicas.push_back(replicas.front());
    }
    if (options.count("provider-b")) {
      replicas.push_back({required(options, "repo-b"),
                          ndn::Name(required(options, "provider-b")), servicePrefix});
    }
    if (options.count("no-candidates")) {
      replicas.clear();
      try {
        (void)repo::RepoClient::fetchOriginalDataPacket(
          context.user(), ndn::Name(dataName), sha256(expectedWire), replicas,
          [&certificate] (const ndn::Data& data) {
            return ndn::security::verifySignature(data, certificate);
          }, 120000);
      }
      catch (const std::invalid_argument& error) {
        if (std::string(error.what()) == "repo-original-data-request-invalid") {
          std::cout << "SPEC193_ORIGINAL_DATA_NO_CANDIDATE_REJECTED\n";
          return 0;
        }
        throw;
      }
      throw std::runtime_error("original Data fetch accepted an empty candidate set");
    }
    const auto fetched = repo::RepoClient::fetchOriginalDataPacket(
      context.user(), ndn::Name(dataName), sha256(expectedWire), replicas,
      [&certificate] (const ndn::Data& data) {
        return ndn::security::verifySignature(data, certificate);
      }, 120000);
    if (fetched != expectedWire) {
      throw std::runtime_error("original Data wire differs from signed source");
    }
    ndn::Block dataBlock(ndn::span<const uint8_t>(fetched.data(), fetched.size()));
    dataBlock.parse();
    const ndn::Data data(dataBlock);
    const auto& content = data.getContent();
    const auto& locator = data.getSignatureInfo().getKeyLocator().getName();
    if (data.getName().toUri() != dataName ||
        content.value_size() != expectedPayload.size() ||
        !std::equal(content.value_begin(), content.value_end(), expectedPayload.begin()) ||
        locator != certificate.getName() ||
        !ndn::security::verifySignature(data, certificate)) {
      throw std::runtime_error("original Data name, payload, KeyLocator, or signature invalid");
    }
    const fs::path destination(required(options, "destination"));
    fs::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(fetched.data()), fetched.size());
    if (!output) throw std::runtime_error("cannot write original Data wire");
    std::cout << "SPEC193_ORIGINAL_DATA_PASS name=" << dataName
              << " sha256=" << sha256(fetched) << std::endl;
    return 0;
  }
  if (mode == "object-data-fetch") {
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    const auto dataName = required(options, "object-name");
    const auto expectedWire = readBytes(required(options, "wire-source"));
    const auto expectedPayload = readBytes(required(options, "source"));
    const auto certificateWire = readBytes(required(options, "certificate"));
    ndn::Block certBlock(ndn::span<const uint8_t>(certificateWire.data(), certificateWire.size()));
    certBlock.parse();
    const ndn::security::Certificate certificate(certBlock);
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    const auto manifest = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(dataName))));
    const auto fetched = selected("FETCH", repo::toBytes(dataName));
    if (fetched != expectedWire || manifest.objectName != dataName ||
        manifest.objectType != "ndn-data-wire" ||
        manifest.packetNames != std::vector<std::string>{dataName} ||
        manifest.size != fetched.size() || manifest.sha256 != sha256(fetched)) {
      throw std::runtime_error("network Data wire or durable manifest differs from source");
    }
    ndn::Block dataBlock(ndn::span<const uint8_t>(fetched.data(), fetched.size()));
    dataBlock.parse();
    const ndn::Data data(dataBlock);
    const auto& content = data.getContent();
    const auto& locator = data.getSignatureInfo().getKeyLocator().getName();
    if (data.getName().toUri() != dataName ||
        content.value_size() != expectedPayload.size() ||
        !std::equal(content.value_begin(), content.value_end(), expectedPayload.begin()) ||
        locator != certificate.getName() ||
        !ndn::security::verifySignature(data, certificate)) {
      throw std::runtime_error("network Data name, payload, KeyLocator, or signature invalid");
    }
    const fs::path destination(required(options, "destination"));
    fs::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(fetched.data()), fetched.size());
    if (!output) throw std::runtime_error("cannot write fetched Data wire");
    std::cout << "SPEC193_DATA_PACKET_NETWORK_PASS name=" << dataName
              << " sha256=" << sha256(fetched) << " locator=" << locator << std::endl;
    return 0;
  }
  if (mode == "object-packet-set-fetch") {
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    const auto objectName = required(options, "object-name");
    const auto source = readBytes(required(options, "source"));
    if (source.size() != 513) {
      throw std::invalid_argument("packet set source must be 513 bytes");
    }
    const auto certificateWire = readBytes(required(options, "certificate"));
    ndn::Block certBlock(ndn::span<const uint8_t>(certificateWire.data(), certificateWire.size()));
    certBlock.parse();
    const ndn::security::Certificate certificate(certBlock);
    const auto expectation = options.count("expect") ? options.at("expect") : "present";
    const fs::path destination(required(options, "destination"));
    const bool existed = fs::exists(destination);
    const auto original = existed ? readBytes(destination) : std::vector<uint8_t>{};
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    const auto manifest = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(objectName))));
    const std::array<int, 3> order{{2, 0, 1}};
    std::vector<std::string> expectedNames;
    for (int index : order) {
      expectedNames.push_back(objectName + "/pkt/" + std::to_string(index));
    }
    if (manifest.objectName != objectName ||
        manifest.objectType != "ndn-data-packet-set" ||
        manifest.segmentCount != order.size() ||
        manifest.packetNames != expectedNames) {
      throw std::runtime_error("packet set manifest did not preserve application order");
    }
    try {
      std::vector<uint8_t> concatenated;
      for (int index : order) {
        const auto name = objectName + "/pkt/" + std::to_string(index);
        const auto fetched = selected("FETCH", repo::toBytes(name));
        const auto expectedWire = readBytes(
          fs::path(required(options, "wire-dir")) /
          ("o09-packet-" + std::to_string(index) + ".wire"));
        if (fetched != expectedWire) {
          throw std::runtime_error("packet set wire changed during network fetch");
        }
        ndn::Block block(ndn::span<const uint8_t>(fetched.data(), fetched.size()));
        block.parse();
        const ndn::Data data(block);
        const auto& content = data.getContent();
        const auto start = static_cast<size_t>(index) * 171;
        if (data.getName().toUri() != name ||
            data.getSignatureInfo().getKeyLocator().getName() != certificate.getName() ||
            content.value_size() != 171 ||
            !std::equal(content.value_begin(), content.value_end(), source.begin() + start) ||
            !ndn::security::verifySignature(data, certificate)) {
          throw std::runtime_error("packet set name, content, locator, or signature invalid");
        }
        concatenated.insert(concatenated.end(), fetched.begin(), fetched.end());
      }
      if (concatenated.size() != manifest.size ||
          sha256(concatenated) != manifest.sha256 || expectation != "present") {
        throw std::runtime_error("packet set complete digest or expectation mismatch");
      }
      fs::create_directories(destination.parent_path());
      std::ofstream output(destination, std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char*>(concatenated.data()), concatenated.size());
      if (!output) throw std::runtime_error("cannot write packet set destination");
      std::cout << "SPEC193_PACKET_SET_NETWORK_PASS order=2,0,1 sha256="
                << sha256(concatenated) << std::endl;
      return 0;
    }
    catch (const std::exception& error) {
      const std::string reason = error.what();
      if (expectation != "missing" ||
          (reason.find("not-found") == std::string::npos &&
           reason.find("not found") == std::string::npos) ||
          fs::exists(destination) != existed ||
          (existed && readBytes(destination) != original) ||
          hasFetchStagingFile(destination)) {
        throw;
      }
      std::cout << "SPEC193_PACKET_SET_NETWORK_REJECTED missing=1 reason="
                << reason << std::endl;
      return 0;
    }
  }
  if (mode == "object-insert-request" || mode == "object-insert-fetch") {
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    const auto objectName = required(options, "object-name");
    const auto dataPrefix = required(options, "data-prefix");
    const auto dataName = ndn::Name(dataPrefix).appendSegment(0).toUri();
    const auto expectedWire = readBytes(required(options, "wire-source"));
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    if (mode == "object-insert-request") {
      repo::RepoDataReference reference;
      reference.objectName = objectName;
      reference.dataPrefix = dataPrefix;
      reference.firstSegment = 0;
      reference.finalSegment = 0;
      reference.hasFinalSegment = true;
      reference.expectedSize = expectedWire.size();
      reference.expectedSha256 = sha256(expectedWire);
      reference.objectType = "ndn-inserted-data";
      const auto inserted = repo::parseOperationStatusJson(repo::toString(
        selected("INSERT", repo::encodeDataReferenceRequest(reference))));
      if (inserted.state != "DONE" || inserted.operationId.empty() ||
          inserted.completedSegments != 1 || inserted.totalSegments != 1) {
        throw std::runtime_error("Repo INSERT did not complete: " + inserted.message);
      }
      const auto queried = repo::parseOperationStatusJson(repo::toString(
        selected("STATUS", repo::encodeStatusRequest(inserted.operationId))));
      if (queried.operationId != inserted.operationId || queried.state != "DONE" ||
          queried.objectName != objectName || queried.completedSegments != 1) {
        throw std::runtime_error("Repo STATUS lost completed INSERT operation");
      }
      writeText(required(options, "report-out"), inserted.toJson());
      std::cout << "SPEC193_INSERT_REQUEST_PASS operation=" << inserted.operationId
                << " data=" << dataName << " sha256=" << sha256(expectedWire) << std::endl;
      return 0;
    }
    const auto reportBytes = readBytes(required(options, "report"));
    const auto inserted = repo::parseOperationStatusJson(repo::toString(reportBytes));
    const auto queried = repo::parseOperationStatusJson(repo::toString(
      selected("STATUS", repo::encodeStatusRequest(inserted.operationId))));
    if (queried.operationId != inserted.operationId || queried.state != "DONE" ||
        queried.objectName != objectName || queried.completedSegments != 1) {
      throw std::runtime_error("independent consumer cannot query INSERT status");
    }
    const auto manifest = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(objectName))));
    const auto fetched = selected("FETCH", repo::toBytes(dataName));
    if (manifest.objectName != objectName ||
        manifest.objectType != "ndn-inserted-data" ||
        manifest.packetNames != std::vector<std::string>{dataName} ||
        manifest.segmentCount != 1 || manifest.size != fetched.size() ||
        manifest.sha256 != sha256(fetched) || fetched != expectedWire) {
      throw std::runtime_error("inserted Data manifest or fetched wire differs from source");
    }
    const auto certWire = readBytes(required(options, "certificate"));
    ndn::Block certBlock(ndn::span<const uint8_t>(certWire.data(), certWire.size()));
    certBlock.parse();
    const ndn::security::Certificate certificate(certBlock);
    ndn::Block dataBlock(ndn::span<const uint8_t>(fetched.data(), fetched.size()));
    dataBlock.parse();
    const ndn::Data data(dataBlock);
    const auto payload = readBytes(required(options, "source"));
    const auto& content = data.getContent();
    if (data.getName().toUri() != dataName ||
        data.getSignatureInfo().getKeyLocator().getName() != certificate.getName() ||
        content.value_size() != payload.size() ||
        !std::equal(content.value_begin(), content.value_end(), payload.begin()) ||
        !ndn::security::verifySignature(data, certificate)) {
      throw std::runtime_error("inserted Data name, content, locator, or signature invalid");
    }
    const fs::path destination(required(options, "destination"));
    fs::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(fetched.data()), fetched.size());
    if (!output) throw std::runtime_error("cannot write inserted Data destination");
    std::cout << "SPEC193_INSERT_CONSUMER_PASS operation=" << inserted.operationId
              << " sha256=" << sha256(fetched) << std::endl;
    return 0;
  }
  if (mode == "object-range-phase" || mode == "object-inventory-audit") {
    const auto phase = required(options, "expect");
    const auto rangeName = required(options, "object-name");
    const auto rangePayload = readBytes(required(options, "source"));
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    if (mode == "object-range-phase") {
      const auto manifest = repo::RepoClient::makeManifest(
        rangeName, "o16-range-object", rangePayload, 1,
        {required(options, "repo-a")}, "spec193-o16");
      if (phase == "stage") {
        const size_t split = rangePayload.size() / 2;
        const std::vector<uint8_t> second(rangePayload.begin() + split,
                                           rangePayload.end());
        const std::vector<uint8_t> first(rangePayload.begin(),
                                          rangePayload.begin() + split);
        if (repo::toString(selected("STORE_RANGE", repo::encodeRangeWriteRequest(
              manifest, repo::RepoByteRange{split, second.size()}, second))) != "accepted" ||
            repo::toString(selected("STORE_RANGE", repo::encodeRangeWriteRequest(
              manifest, repo::RepoByteRange{0, first.size()}, first))) != "accepted") {
          throw std::runtime_error("O16 network range staging was not accepted");
        }
      }
      else if (phase == "commit") {
        const auto committed = repo::parseManifestJson(repo::toString(
          selected("COMMIT_RANGES", repo::toBytes(manifest.toJson()))));
        if (committed.objectName != rangeName ||
            committed.sha256 != sha256(rangePayload) ||
            committed.size != rangePayload.size()) {
          throw std::runtime_error("O16 range commit manifest differs from source");
        }
      }
      else {
        throw std::invalid_argument("O16 range phase must be stage or commit");
      }
      std::cout << "SPEC193_O16_RANGE_PASS phase=" << phase << std::endl;
      return 0;
    }
    if (phase != "staged" && phase != "committed") {
      throw std::invalid_argument("O16 inventory phase must be staged or committed");
    }
    const auto parentName = required(options, "parent-name");
    const auto dataName = required(options, "data-name");
    const auto originalWire = readBytes(required(options, "wire"));
    const auto operation = repo::parseOperationStatusJson(repo::toString(
      readBytes(required(options, "report"))));
    const auto status = repo::parseOperationStatusJson(repo::toString(
      selected("STATUS", repo::encodeStatusRequest(operation.operationId))));
    const auto inventory = repo::parseInventoryJson(repo::toString(
      selected("INVENTORY", {})));
    const auto parent = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(parentName))));
    const auto child = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(dataName))));
    const auto fetchedWire = selected("FETCH", repo::toBytes(dataName));
    ndn::Block block(ndn::span<const uint8_t>(fetchedWire.data(), fetchedWire.size()));
    block.parse();
    const ndn::Data data(block);
    if (status.operationId != operation.operationId || status.state != "DONE" ||
        status.objectName != parentName || status.completedSegments != 1 ||
        parent.objectName != parentName || parent.packetNames.size() != 1 ||
        parent.packetNames.front() != dataName ||
        parent.sha256 != sha256(originalWire) || parent.size != originalWire.size() ||
        child.objectName != dataName || child.objectType != "ndn-data-wire" ||
        child.sha256 != sha256(originalWire) || child.size != originalWire.size() ||
        fetchedWire != originalWire || data.getName().toUri() != dataName) {
      throw std::runtime_error("O16 INSERT status, manifest, or exact Data wire differs");
    }
    const auto countName = [&] (const std::string& name) {
      return std::count_if(inventory.begin(), inventory.end(),
                           [&] (const auto& entry) { return entry.objectName == name; });
    };
    if (countName(parentName) != 1 || countName(dataName) != 1) {
      throw std::runtime_error("O16 inventory omitted INSERT parent or Data child");
    }
    std::string rangeJson = "null";
    if (phase == "staged") {
      if (countName(rangeName) != 0) {
        throw std::runtime_error("O16 staged range object leaked into inventory");
      }
      for (const auto& operationName : {"MANIFEST", "FETCH"}) {
        bool rejected = false;
        try {
          (void)selected(operationName, repo::toBytes(rangeName));
        }
        catch (const std::exception& e) {
          rejected = std::string(e.what()).find("repo-object-not-found") !=
                     std::string::npos;
          if (!rejected) throw;
        }
        if (!rejected) {
          throw std::runtime_error("O16 staged range object became readable");
        }
      }
    }
    else {
      if (countName(rangeName) != 1 ||
          selected("FETCH", repo::toBytes(rangeName)) != rangePayload) {
        throw std::runtime_error("O16 committed range object absent or changed");
      }
      const auto range = repo::parseManifestJson(repo::toString(
        selected("MANIFEST", repo::toBytes(rangeName))));
      if (range.objectName != rangeName || range.sha256 != sha256(rangePayload) ||
          range.size != rangePayload.size()) {
        throw std::runtime_error("O16 committed range manifest differs");
      }
      rangeJson = range.toJson();
    }
    std::ofstream report(required(options, "report-out"),
                         std::ios::binary | std::ios::trunc);
    report << "{\"status\":" << status.toJson()
           << ",\"inventory\":" << repo::encodeInventory(inventory)
           << ",\"parent\":" << parent.toJson()
           << ",\"child\":" << child.toJson()
           << ",\"range\":" << rangeJson
           << ",\"phase\":\"" << phase << "\"}";
    report.close();
    if (!report) throw std::runtime_error("cannot save O16 inventory report");
    std::cout << "SPEC193_O16_INVENTORY_PASS phase=" << phase << std::endl;
    return 0;
  }
  if (mode == "object-catalog-delete" || mode == "object-catalog-deleted") {
    const auto objectName = required(options, "object-name");
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    if (mode == "object-catalog-delete") {
      if (repo::toString(selected("DELETE", repo::toBytes(objectName))) != "deleted") {
        throw std::runtime_error("O15 network DELETE did not remove target");
      }
      std::cout << "SPEC193_O15_DELETE_PASS" << std::endl;
      return 0;
    }
    const auto phase = required(options, "expect");
    if (phase != "live" && phase != "restarted") {
      throw std::invalid_argument("O15 deleted catalog phase must be live or restarted");
    }
    const auto sinceEpoch = std::stoull(required(options, "since-epoch"));
    const auto payload = readBytes(required(options, "source"));
    const auto status = repo::parseCatalogStatusJson(repo::toString(
      selected("CATALOG_STATUS", {})));
    const auto snapshot = repo::parseCatalogDeltaJson(repo::toString(
      selected("CATALOG_SNAPSHOT", {})));
    const auto delta = repo::parseCatalogDeltaJson(repo::toString(
      selected("CATALOG_DELTA", repo::encodeCatalogDeltaRequest(sinceEpoch))));
    bool lookupRejected = false;
    try {
      (void)selected("CATALOG_LOOKUP", repo::encodeCatalogLookupRequest(objectName));
    }
    catch (const std::exception& e) {
      lookupRejected = std::string(e.what()).find("repo-object-not-found") !=
                       std::string::npos;
      if (!lookupRejected) throw;
    }
    if (!lookupRejected || status.objectCount != snapshot.entries.size() ||
        snapshot.catalogEpoch != status.catalogEpoch ||
        delta.catalogEpoch != status.catalogEpoch ||
        delta.sinceEpoch != sinceEpoch ||
        std::any_of(snapshot.entries.begin(), snapshot.entries.end(),
                    [&] (const auto& entry) {
                      return entry.manifest.objectName == objectName;
                    })) {
      throw std::runtime_error("O15 deleted object remains visible in catalog");
    }
    const auto tombstone = [&] (const repo::RepoCatalogEntry& entry) {
      return entry.manifest.objectName == objectName &&
             entry.manifest.sha256 == sha256(payload) &&
             entry.state == "DELETED";
    };
    const auto tombstoneCount = std::count_if(
      delta.entries.begin(), delta.entries.end(), tombstone);
    if (phase == "live" && (status.catalogEpoch <= sinceEpoch ||
                            tombstoneCount != 1 ||
                            delta.entries.size() != 1 ||
                            delta.entries.front().catalogEpoch != status.catalogEpoch)) {
      throw std::runtime_error("O15 stale epoch omitted live deletion tombstone");
    }
    if (phase == "restarted" && tombstoneCount == 0 &&
        !status.reconciliationRequired) {
      throw std::runtime_error("O15 restart lost deletion without reconciliation flag");
    }
    std::ofstream report(required(options, "report-out"),
                         std::ios::binary | std::ios::trunc);
    report << "{\"status\":" << status.toJson()
           << ",\"snapshot\":" << snapshot.toJson()
           << ",\"delta\":" << delta.toJson()
           << ",\"sinceEpoch\":" << sinceEpoch
           << ",\"lookupRejected\":true"
           << ",\"phase\":\"" << phase << "\"}";
    report.close();
    if (!report) throw std::runtime_error("cannot save O15 deleted catalog report");
    std::cout << "SPEC193_O15_DELETED_PASS phase=" << phase << std::endl;
    return 0;
  }
  if (mode == "object-catalog-query") {
    const auto objectName = required(options, "object-name");
    const auto payload = readBytes(required(options, "source"));
    const auto phase = required(options, "expect");
    if (phase != "live" && phase != "restarted") {
      throw std::invalid_argument("O14 catalog phase must be live or restarted");
    }
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    const auto status = repo::parseCatalogStatusJson(repo::toString(
      selected("CATALOG_STATUS", {})));
    const auto snapshot = repo::parseCatalogDeltaJson(repo::toString(
      selected("CATALOG_SNAPSHOT", {})));
    const auto delta = repo::parseCatalogDeltaJson(repo::toString(
      selected("CATALOG_DELTA", repo::encodeCatalogDeltaRequest(0))));
    const auto lookup = repo::parseCatalogEntryJson(repo::toString(
      selected("CATALOG_LOOKUP", repo::encodeCatalogLookupRequest(objectName))));
    const auto matches = [&] (const repo::RepoCatalogEntry& entry) {
      return entry.manifest.objectName == objectName &&
             entry.manifest.sha256 == sha256(payload) &&
             entry.manifest.size == payload.size() &&
             entry.sourceRepo == required(options, "repo-a") &&
             entry.state == "AVAILABLE";
    };
    const auto countMatches = [&] (const repo::RepoCatalogDelta& view) {
      return std::count_if(view.entries.begin(), view.entries.end(), matches);
    };
    const auto manifest = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(objectName))));
    if (status.objectCount != snapshot.entries.size() ||
        snapshot.catalogEpoch != status.catalogEpoch ||
        delta.catalogEpoch != status.catalogEpoch || delta.sinceEpoch != 0 ||
        countMatches(snapshot) != 1 || !matches(lookup) ||
        manifest.sha256 != lookup.manifest.sha256 ||
        manifest.size != lookup.manifest.size) {
      throw std::runtime_error("O14 catalog status/snapshot/delta/lookup disagree");
    }
    if (phase == "live" && (status.catalogEpoch == 0 || countMatches(delta) != 1)) {
      throw std::runtime_error("O14 live catalog omitted committed object delta");
    }
    if (phase == "restarted" && countMatches(delta) == 0 &&
        !status.reconciliationRequired) {
      throw std::runtime_error("O14 restart lost delta without reconciliation flag");
    }
    std::ofstream report(required(options, "report-out"),
                         std::ios::binary | std::ios::trunc);
    report << "{\"status\":" << status.toJson()
           << ",\"snapshot\":" << snapshot.toJson()
           << ",\"delta\":" << delta.toJson()
           << ",\"lookup\":" << lookup.toJson()
           << ",\"manifest\":" << manifest.toJson()
           << ",\"phase\":\"" << phase << "\"}";
    report.close();
    if (!report) throw std::runtime_error("cannot save O14 catalog report");
    std::cout << "SPEC193_O14_CATALOG_PASS phase=" << phase << std::endl;
    return 0;
  }
  if (mode == "object-cache-status") {
    const auto objectName = required(options, "object-name");
    const auto payload = readBytes(required(options, "source"));
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    const auto cacheStatus = [&] {
      return repo::parseCacheStatusJson(repo::toString(selected("CACHE_STATUS", {})));
    };
    const auto before = cacheStatus();
    const auto manifest = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(objectName))));
    const auto first = selected("FETCH", repo::toBytes(objectName));
    const auto afterFirst = cacheStatus();
    const auto second = selected("FETCH", repo::toBytes(objectName));
    const auto afterSecond = cacheStatus();
    const auto charge = payload.size() + objectName.size() + manifest.toJson().size();
    if (first != payload || second != payload || manifest.size != payload.size() ||
        before.entryCount != 0 || before.usedBytes != 0 ||
        afterFirst.entryCount != 1 || afterFirst.usedBytes != charge ||
        afterFirst.misses != before.misses + 1 ||
        afterFirst.backingReads != before.backingReads + 1 ||
        afterFirst.admissions != before.admissions + 1 ||
        afterSecond.entryCount != 1 || afterSecond.usedBytes != charge ||
        afterSecond.hits != afterFirst.hits + 1 ||
        afterSecond.misses != afterFirst.misses ||
        afterSecond.backingReads != afterFirst.backingReads) {
      throw std::runtime_error("O13 network cache status disagrees with FETCH effects");
    }
    std::ofstream report(required(options, "report-out"),
                         std::ios::binary | std::ios::trunc);
    report << "{\"manifest\":" << manifest.toJson()
           << ",\"before\":" << before.toJson()
           << ",\"afterFirst\":" << afterFirst.toJson()
           << ",\"afterSecond\":" << afterSecond.toJson()
           << ",\"expectedChargeBytes\":" << charge << "}";
    report.close();
    if (!report) throw std::runtime_error("cannot save O13 network cache report");
    std::cout << "SPEC193_O13_NETWORK_CACHE_PASS" << std::endl;
    return 0;
  }
  if (mode == "object-payload-fetch") {
    const ndn::Name servicePrefix(required(options, "service-prefix"));
    const ndn::Name provider(required(options, "provider-a"));
    const auto objectName = required(options, "object-name");
    const auto expectedPayload = readBytes(required(options, "source"));
    const auto certificateWire = readBytes(required(options, "certificate"));
    ndn::Block certBlock(ndn::span<const uint8_t>(certificateWire.data(), certificateWire.size()));
    certBlock.parse();
    const ndn::security::Certificate certificate(certBlock);
    if (certificate.getIdentity() != provider) {
      throw std::runtime_error("insertPayload signing identity differs from Repo A");
    }
    auto selected = [&] (const std::string& operation,
                         const std::vector<uint8_t>& request) {
      return repo::RepoClient::requestSelectedService(
        context.user(), provider,
        repo::makeRepoServiceName(servicePrefix, operation),
        request, 15000, 120000);
    };
    const auto manifest = repo::parseManifestJson(repo::toString(
      selected("MANIFEST", repo::toBytes(objectName))));
    const size_t expectedCount = (expectedPayload.size() + 127) / 128;
    if (manifest.objectName != objectName ||
        manifest.objectType != "ndn-auto-segmented-payload" ||
        manifest.segmentCount != expectedCount ||
        manifest.packetNames.size() != expectedCount || expectedCount < 3) {
      throw std::runtime_error("insertPayload manifest lacks generated segments");
    }
    std::vector<uint8_t> rebuilt;
    std::vector<uint8_t> orderedWire;
    for (size_t index = 0; index < expectedCount; ++index) {
      const auto name = ndn::Name(objectName).appendSegment(index).toUri();
      if (manifest.packetNames[index] != name) {
        throw std::runtime_error("insertPayload manifest segment name or order changed");
      }
      const auto wire = selected("FETCH", repo::toBytes(name));
      const auto expectedWire = readBytes(
        fs::path(required(options, "wire-dir")) /
        ("o11-segment-" + std::to_string(index) + ".wire"));
      if (wire != expectedWire) {
        throw std::runtime_error("insertPayload network segment wire changed");
      }
      ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
      block.parse();
      const ndn::Data data(block);
      const auto& content = data.getContent();
      const size_t start = index * 128;
      const size_t expectedSize = std::min<size_t>(128, expectedPayload.size() - start);
      if (data.getName().toUri() != name ||
          data.getSignatureInfo().getKeyLocator().getName() != certificate.getName() ||
          content.value_size() != expectedSize ||
          !std::equal(content.value_begin(), content.value_end(),
                      expectedPayload.begin() + start) ||
          !ndn::security::verifySignature(data, certificate)) {
        throw std::runtime_error("insertPayload segment name, bytes, or signature invalid");
      }
      rebuilt.insert(rebuilt.end(), content.value_begin(), content.value_end());
      orderedWire.insert(orderedWire.end(), wire.begin(), wire.end());
    }
    if (rebuilt != expectedPayload || orderedWire.size() != manifest.size ||
        sha256(orderedWire) != manifest.sha256) {
      throw std::runtime_error("insertPayload application reassembly digest mismatch");
    }
    const fs::path destination(required(options, "destination"));
    fs::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(rebuilt.data()), rebuilt.size());
    if (!output) throw std::runtime_error("cannot write insertPayload result");
    std::cout << "SPEC193_INSERT_PAYLOAD_NETWORK_PASS count=" << expectedCount
              << " sha256=" << sha256(rebuilt)
              << " signer=" << certificate.getIdentity() << std::endl;
    return 0;
  }
  repo::PersistentRepoClientConfig config;
  config.servicePrefix = ndn::Name(required(options, "service-prefix"));
  config.ackCollectionTimeMs = 1500;
  config.requestTimeoutMs = 30000;
  config.operationWaitTimeoutMs = 60000;
  if (mode == "operation-begin") {
    // Leave time to fetch the selected STORE ACK after five-host SVS sync.
    config.ackCollectionTimeMs = 10000;
  }
  if (mode == "archive" || mode == "archive-begin") {
    // Leave a seed-write window after the API's STATUS recovery reserve.
    config.operationWaitTimeoutMs = 90000;
  }
  if (mode == "publish") {
    // The explicit A/B set must be present in the complete authenticated
    // CAPABILITY closure before publishFile accepts it as current. The cold
    // five-host MiniNDN may deliver the second Repo's SVS ACK near 30 s.
    // Keep a 60 s window so CAPABILITY closure does not race that delivery.
    // beginPublishFileImpl shares its request budget across STATUS,
    // CAPABILITY discovery, and the initial seed write. Leave room for cold
    // five-host discovery and several serial ACK windows before that write
    // starts. The synchronous operation wait below remains a separate bound.
    config.ackCollectionTimeMs = 60000;
    config.requestTimeoutMs = 600000;
    config.operationWaitTimeoutMs = 180000;
  }
  if (mode == "single-publish") {
    config.ackCollectionTimeMs = 60000;
    config.requestTimeoutMs = 600000;
    config.operationWaitTimeoutMs = 180000;
  }
  if (mode == "discover") {
    config.ackCollectionTimeMs = 60000;
    config.requestTimeoutMs = 120000;
  }
  if (mode == "fetch") {
    // Cold five-host MiniNDN responses need the same wide ACK window as the
    // Python peer; allow a second committed replica attempt within one fetch.
    config.ackCollectionTimeMs = 30000;
    config.requestTimeoutMs = 60000;
    config.operationWaitTimeoutMs = 120000;
  }
  if (mode == "receipt-query") {
    config.ackCollectionTimeMs = 30000;
    config.requestTimeoutMs = 90000;
  }
  if (mode == "availability" || mode == "failover-fetch" ||
      mode == "failover-cooldown" ||
      mode == "all-down-fetch") {
    config.requestTimeoutMs = 2000;
  }
  if (mode == "failover-cooldown") {
    config.fetchFailureCooldownMs = 60000;
  }
  auto api = repo::ArtifactRepositoryApi::makePersistent(context.user(), config);

  if (mode == "invalid-selected-set") {
    const auto source = required(options, "source");
    const auto reference = makeReference(source, required(options, "user"));
    const auto valid = makeSelected(options, config.servicePrefix);
    auto expectRejected = [&] (const std::string& label, const std::string& reason,
                               const repo::SelectedRepoSet& selected) {
      try {
        (void)api.publishFile(source, reference, selected, 15000);
      }
      catch (const std::invalid_argument& error) {
        if (std::string(error.what()).find(reason) != std::string::npos) {
          std::cout << "SPEC193_SELECTED_SET_REJECTED case=" << label
                    << " reason=" << reason << '\n';
          return;
        }
        throw;
      }
      throw std::runtime_error("invalid selected set was accepted: " + label);
    };
    auto outsideSeed = valid;
    outsideSeed.seedRepoId = "/example/repo/node/outside";
    expectRejected("outside-seed", "seed is outside selected set", outsideSeed);
    auto duplicateRepo = valid;
    duplicateRepo.targets[1].repoNode = duplicateRepo.targets[0].repoNode;
    expectRejected("duplicate-repo", "duplicate Repo or Provider identity", duplicateRepo);
    auto duplicateProvider = valid;
    duplicateProvider.targets[1].providerIdentity =
      duplicateProvider.targets[0].providerIdentity;
    expectRejected("duplicate-provider", "duplicate Repo or Provider identity",
                   duplicateProvider);
    auto wrongPrefix = valid;
    wrongPrefix.targets[1].servicePrefix = ndn::Name("/spec193/wrong-service");
    expectRejected("wrong-prefix", "Provider or service prefix mismatch", wrongPrefix);
    return 0;
  }

  if (mode == "discover") {
    const auto reference = makeReference(required(options, "source"),
                                         required(options, "user"));
    repo::ArtifactCapabilityRequirements requirements;
    requirements.artifact = reference;
    requirements.rootSignatureAlgorithm = "rsa-sha256";
    requirements.chunkBytes = 4;
    requirements.rootEncodedBytes = 1;
    requirements.pageEncodedBytes = 1;
    requirements.pageEntries = 1;
    requirements.manifestDepth = 1;
    requirements.requireResume = true;
    requirements.requireReplicaReceipts = true;
    const auto discovered = api.discoverRepoCandidates(requirements, 120000);
    if (discovered.requestId.empty()) {
      throw std::runtime_error("discovery returned no request ID");
    }
    std::cout << "DISCOVERY_REQUEST id=" << discovered.requestId << '\n';
    std::set<std::string> seenRepos;
    std::set<std::string> seenProviders;
    for (const auto& candidate : discovered.candidates) {
      if (!seenRepos.insert(candidate.repoNode).second ||
          !seenProviders.insert(candidate.providerIdentity.toUri()).second ||
          candidate.servicePrefix != config.servicePrefix ||
          candidate.repoNode == candidate.providerIdentity.toUri() ||
          !candidate.capability.supportsReplicaReceipts ||
          candidate.ackWireDigest.empty() || candidate.signerKeyLocator.empty()) {
        throw std::runtime_error("discovery returned an invalid or duplicate Repo candidate");
      }
      std::cout << "DISCOVERY_CANDIDATE repo=" << candidate.repoNode
                << " provider=" << candidate.providerIdentity
                << " prefix=" << candidate.servicePrefix
                << " wire_digest=" << candidate.ackWireDigest
                << " locator=" << candidate.signerKeyLocator << '\n';
    }
    std::cout << "DISCOVERY_COMPLETE count=" << discovered.candidates.size() << '\n';
    return 0;
  }

  if (mode == "operation-begin" || mode == "operation-replay" ||
      mode == "operation-conflict") {
    const auto reference = makeReference(required(options, "source"),
                                         required(options, "user"));
    const auto selected = makeSelected(options, config.servicePrefix);
    const auto key = required(options, "key");
    if (mode == "operation-conflict") {
      try {
        (void)api.beginPublishFile(required(options, "source"), reference,
                                   selected, key, 120000);
      }
      catch (const std::exception& error) {
        if (std::string(error.what()).find(
              "repo-publish-idempotency-conflict:") != std::string::npos) {
          std::cout << "OPERATION_KEY_CONFLICT_REJECTED " << error.what() << '\n';
          return 0;
        }
        throw;
      }
      throw std::runtime_error("same-key changed request was unexpectedly accepted");
    }

    // VM-20 transfers a multi-chunk seed after cold authenticated discovery.
    // Preserve the API's STATUS recovery reserve inside this bounded begin call.
    const int beginTimeoutMs = mode == "operation-begin" ? 360000 : 120000;
    const auto handle = api.beginPublishFile(
      required(options, "source"), reference, selected, key, beginTimeoutMs);
    if (mode == "operation-replay") {
      const auto previous = repo::PublishOperationHandle::fromJson(
        readText(required(options, "handle")));
      if (handle.operationId != previous.operationId ||
          handle.seed.repoNode != previous.seed.repoNode ||
          handle.seed.providerIdentity != previous.seed.providerIdentity ||
          handle.seed.servicePrefix != previous.seed.servicePrefix) {
        throw std::runtime_error("same-key operation replay returned a different handle");
      }
      std::cout << "OPERATION_REPLAY_PASS operation=" << handle.operationId << '\n';
      return 0;
    }
    writeText(required(options, "handle-out"), handle.toJson());
    std::cout << "OPERATION_SEED_ACCEPTED operation=" << handle.operationId
              << " seed=" << handle.seed.repoNode << '\n';
    return 0;
  }

  if (mode == "operation-status" || mode == "operation-status-once") {
    const auto handle = repo::PublishOperationHandle::fromJson(
      readText(required(options, "handle")));
    const auto status = mode == "operation-status-once"
      ? api.getPublishStatus(handle, 30000)
      : waitForTerminal(api, handle,
          std::stoi(options.count("wait-ms") ? options.at("wait-ms") : "120000"));
    if (options.count("status-out")) {
      writeText(options.at("status-out"), status.toJson());
    }
    if (mode == "operation-status" && status.state != "COMMITTED") {
      throw std::runtime_error("resumed publish operation did not commit all selected targets: " +
                               status.state);
    }
    std::cout << (mode == "operation-status-once"
                    ? "OPERATION_STATUS_OBSERVED operation="
                    : "OPERATION_RESUME_PASS operation=") << status.operationId
              << " state=" << status.state << " targets=" << status.targets.size() << '\n';
    return 0;
  }

  if (mode == "availability") {
    auto handle = repo::ArtifactHandle::fromJson(readText(required(options, "handle")));
    if (options.count("probe-first-repo")) {
      const auto first = std::find_if(handle.replicaCommits.begin(),
        handle.replicaCommits.end(), [&options] (const auto& replica) {
          return replica.repoNode == options.at("probe-first-repo");
        });
      if (first == handle.replicaCommits.end()) {
        throw std::invalid_argument("probe-first-repo is not in the selected handle");
      }
      std::rotate(handle.replicaCommits.begin(), first, first + 1);
    }
    const auto expectation = required(options, "expect");
    const auto targetRepo = required(options, "target-repo");
    const auto timeoutMs = std::stoi(options.count("timeout-ms")
      ? options.at("timeout-ms") : "10000");
    const auto probeKind = options.count("probe-kind")
      ? options.at("probe-kind") : "verify_content";
    const auto report = api.checkAvailability(
      handle, probeKind, timeoutMs, 0);
    requireAvailability(report, expectation, targetRepo);
    if (options.count("report-out")) {
      writeText(options.at("report-out"), report.toJson());
    }
    std::cout << "AVAILABILITY_PASS " << report.toJson() << '\n';
    return 0;
  }

  if (mode == "reject-provider-alias") {
    auto handle = repo::ArtifactHandle::fromJson(readText(required(options, "handle")));
    const auto repoA = required(options, "repo-a");
    const auto target = std::find_if(handle.selectedTargets.begin(), handle.selectedTargets.end(),
      [&repoA] (const auto& item) { return item.repoNode == repoA; });
    const auto replica = std::find_if(handle.replicaCommits.begin(), handle.replicaCommits.end(),
      [&repoA] (const auto& item) { return item.repoNode == repoA; });
    if (target == handle.selectedTargets.end() || replica == handle.replicaCommits.end()) {
      throw std::runtime_error("alias test handle lacks its Repo A binding");
    }
    auto aliasTarget = *target;
    aliasTarget.repoNode += "/alias";
    auto aliasReplica = *replica;
    aliasReplica.repoNode = aliasTarget.repoNode;
    handle.selectedTargets.push_back(std::move(aliasTarget));
    handle.replicaCommits.push_back(std::move(aliasReplica));
    const fs::path destination = required(options, "destination");
    try {
      (void)api.fetchFile(handle, destination.string(), 5000);
    }
    catch (const std::exception& error) {
      if (std::string(error.what()).find("duplicate selected identities") !=
          std::string::npos) {
        if (fs::exists(destination) || hasFetchStagingFile(destination)) {
          throw std::runtime_error("duplicate Provider alias created fetch output before rejection");
        }
        std::cout << "DUPLICATE_PROVIDER_ALIAS_REJECTED_BEFORE_FETCH\n";
        return 0;
      }
      throw;
    }
    throw std::runtime_error("duplicate Provider alias was not rejected");
  }

  if (mode == "failover-fetch") {
    const auto handle = repo::ArtifactHandle::fromJson(readText(required(options, "handle")));
    const auto failedProvider = required(options, "failed-provider");
    const auto fetched = api.fetchFile(handle, required(options, "destination"),
      std::stoi(options.count("timeout-ms") ? options.at("timeout-ms") : "15000"));
    if (fetched.providerIdentity == failedProvider || fetched.attempts.size() < 2 ||
        fetched.attempts.front().providerIdentity != failedProvider) {
      throw std::runtime_error("fetch did not fail over from the expected first Provider");
    }
    const auto bytes = readBytes(required(options, "destination"));
    if (bytes.size() != handle.reference.sizeBytes ||
        sha256(bytes) != handle.reference.contentDigest) {
      throw std::runtime_error("failover fetch content digest/size mismatch");
    }
    std::cout << "FAILOVER_FETCH_PASS attempts=" << fetched.attempts.size()
              << " elapsedMs=" << fetched.totalElapsedMs
              << " failedProvider=" << fetched.attempts.front().providerIdentity
              << " selectedProvider=" << fetched.providerIdentity << '\n';
    return 0;
  }

  if (mode == "failover-cooldown") {
    const auto handle = repo::ArtifactHandle::fromJson(readText(required(options, "handle")));
    const fs::path destination = required(options, "destination");
    const auto failedProvider = required(options, "failed-provider");
    const auto first = api.fetchFile(handle, destination.string(), 15000);
    if (first.attempts.size() < 2 || first.attempts.front().providerIdentity != failedProvider ||
        first.attempts.front().outcome != "FAILED" ||
        first.providerIdentity == failedProvider ||
        sha256(readBytes(destination)) != handle.reference.contentDigest) {
      throw std::runtime_error("first fetch did not fail over from the offline Provider");
    }
    for (const auto& attempt : first.attempts) {
      std::cout << "SPEC193_A21_FIRST_ATTEMPT provider=" << attempt.providerIdentity
                << " outcome=" << attempt.outcome
                << " failureClass=" << attempt.failureClass << '\n';
    }
    const auto original = readBytes(destination);
    writeText(required(options, "phase-ready"), "{\"phase\":\"first-fetch-complete\"}");
    const fs::path continuePath = required(options, "phase-continue");
    const auto barrierDeadline = std::chrono::steady_clock::now() +
      std::chrono::seconds(30);
    while (!fs::exists(continuePath) && std::chrono::steady_clock::now() < barrierDeadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!fs::exists(continuePath)) {
      throw std::runtime_error("A21 runner did not release the all-offline fetch barrier");
    }
    try {
      (void)api.fetchFile(handle, destination.string(), 10000);
    }
    catch (const repo::ArtifactFetchError& error) {
      const auto& second = error.result();
      std::set<std::string> attemptedProviders;
      bool failedProviderCooled = false;
      for (const auto& attempt : second.attempts) {
        std::cout << "SPEC193_A21_SECOND_ATTEMPT provider=" << attempt.providerIdentity
                  << " outcome=" << attempt.outcome
                  << " failureClass=" << attempt.failureClass
                  << " cooled=" << attempt.wasCoolingDownAtSelection
                  << " budgetMs=" << attempt.requestTimeoutBudgetMs << '\n';
        if (!attemptedProviders.insert(attempt.providerIdentity).second ||
            attempt.outcome != "FAILED" || attempt.failureClass != "timeout") {
          throw std::runtime_error("second fetch repeated a Provider or misclassified a failure");
        }
        if (attempt.providerIdentity == failedProvider) {
          failedProviderCooled = attempt.wasCoolingDownAtSelection;
        }
      }
      if (attemptedProviders.size() != 3 || !failedProviderCooled ||
          second.totalElapsedMs > 11000 || readBytes(destination) != original ||
          hasFetchStagingFile(destination)) {
        throw std::runtime_error("A21 cooldown/deadline/destination oracle failed");
      }
      std::cout << "SPEC193_A21_COOLDOWN_PASS attempts=" << second.attempts.size()
                << " elapsedMs=" << second.totalElapsedMs << '\n';
      return 0;
    }
    throw std::runtime_error("A21 all-offline second fetch unexpectedly succeeded");
  }

  if (mode == "all-down-fetch") {
    const auto handle = repo::ArtifactHandle::fromJson(readText(required(options, "handle")));
    const fs::path destination = required(options, "destination");
    const bool destinationExisted = fs::exists(destination);
    const auto originalDestination = destinationExisted
      ? readBytes(destination) : std::vector<uint8_t>{};
    const auto timeoutMs = std::stoi(options.count("timeout-ms")
      ? options.at("timeout-ms") : "7000");
    try {
      (void)api.fetchFile(handle, destination.string(), timeoutMs);
    }
    catch (const repo::ArtifactFetchError& error) {
      const auto& result = error.result();
      std::set<std::string> attemptedProviders;
      for (const auto& attempt : result.attempts) {
        std::cout << "SPEC193_FETCH_ATTEMPT repo=" << attempt.repoNode
                  << " provider=" << attempt.providerIdentity
                  << " outcome=" << attempt.outcome
                  << " failureClass=" << attempt.failureClass
                  << " budgetMs=" << attempt.requestTimeoutBudgetMs
                  << " elapsedMs=" << attempt.elapsedMs << '\n';
        if (!attemptedProviders.insert(attempt.providerIdentity).second) {
          throw std::runtime_error("fetch retried one authenticated Provider principal");
        }
        if (attempt.requestTimeoutBudgetMs > static_cast<uint64_t>(timeoutMs)) {
          throw std::runtime_error("fetch attempt exceeded the total operation timeout");
        }
      }
      if (attemptedProviders.size() < 2 ||
          attemptedProviders.size() > handle.selectedTargets.size() || result.totalElapsedMs >
          static_cast<uint64_t>(timeoutMs + 1000) ||
          fs::exists(destination) != destinationExisted ||
          (destinationExisted && readBytes(destination) != originalDestination) ||
          hasFetchStagingFile(destination)) {
        throw std::runtime_error("all-down fetch violated unique-attempt/deadline/destination oracle");
      }
      std::cout << "ALL_DOWN_FETCH_BOUNDED attempts=" << result.attempts.size()
                << " elapsedMs=" << result.totalElapsedMs << '\n';
      return 0;
    }
    throw std::runtime_error("fetch unexpectedly succeeded while every Repo was offline");
  }

  if (mode == "single-publish") {
    const auto reference = makeReference(required(options, "source"),
                                         required(options, "user"));
    repo::SelectedRepoSet selected;
    selected.seedRepoId = required(options, "repo-a");
    selected.consistency = repo::RepoWriteConsistency::One;
    selected.targets.push_back({selected.seedRepoId,
      ndn::Name(required(options, "provider-a")), config.servicePrefix});
    selected.validate(config.servicePrefix);
    const auto result = api.publishFile(required(options, "source"), reference,
                                        selected, 180000);
    if (!result.handle || !result.receipt) {
      throw std::runtime_error("single-target publish lacks handle or receipt");
    }
    writeText(required(options, "handle-out"), result.handle->toJson());
    writeText(required(options, "receipt-out"), result.receipt->toJson());
    std::cout << "SPEC193_SINGLE_COMMITTED sha256=" << reference.contentDigest
              << " receipt=" << result.receipt->receiptId << '\n';
    return 0;
  }
  if (mode == "publish") {
    const auto reference = makeReference(required(options, "source"),
                                         required(options, "user"));
    const auto result = publish(api, options, reference, config.servicePrefix);
    if (!result.handle || !result.receipt) {
      throw std::runtime_error("Persistent publish did not produce a handle and receipt");
    }
    writeText(required(options, "handle-out"), result.handle->toJson());
    writeText(required(options, "receipt-out"), result.receipt->toJson());
    std::cout << "PERSISTENT_COMMITTED sha256=" << reference.contentDigest
              << " receipt=" << result.receipt->receiptId << '\n';
    return 0;
  }
  if (mode == "receipt-query") {
    const auto handleBytes = readBytes(required(options, "handle"));
    auto handle = repo::ArtifactHandle::fromJson(
      std::string(handleBytes.begin(), handleBytes.end()));
    const auto receiptBytes = readBytes(required(options, "receipt"));
    const auto expected = repo::ArtifactCommitReceipt::fromJson(
      std::string(receiptBytes.begin(), receiptBytes.end()));
    if (options.count("forge-handle-receipt")) {
      if (handle.commitReceiptIds.empty()) {
        throw std::runtime_error("receipt query handle has no receipt ID");
      }
      handle.commitReceiptIds.front() = expected.receiptId;
    }
    if (options.count("expected-error")) {
      try {
        (void)api.getReceipt(expected.receiptId, handle, 90000);
      }
      catch (const std::exception& error) {
        if (std::string(error.what()).find(required(options, "expected-error")) !=
            std::string::npos) {
          std::cout << "SPEC193_RECEIPT_REJECTED reason=" << error.what() << '\n';
          return 0;
        }
        throw;
      }
      throw std::runtime_error("nonmatching receipt query unexpectedly succeeded");
    }
    const auto actual = api.getReceipt(expected.receiptId, handle, 90000);
    if (actual.toJson() != expected.toJson()) {
      throw std::runtime_error("Persistent receipt query changed immutable bytes");
    }
    if (options.count("receipt-out")) {
      writeText(options.at("receipt-out"), actual.toJson());
    }
    std::cout << "SPEC193_RECEIPT_QUERY_PASS receipt=" << actual.receiptId << '\n';
    return 0;
  }
  if (mode == "archive" || mode == "archive-begin") {
    const auto bytes = readBytes(required(options, "handle"));
    const auto handle = repo::ArtifactHandle::fromJson(
      std::string(bytes.begin(), bytes.end()));
    repo::InAppRepoConfig localConfig;
    localConfig.durableRoot = required(options, "store-dir");
    localConfig.ownerId = handle.reference.publisherIdentity;
    auto inApp = repo::ArtifactRepositoryApi::makeInApp(localConfig);
    if (mode == "archive-begin") {
      const auto operation = api.beginArchive(
        inApp, handle, makeSelected(options, config.servicePrefix),
        required(options, "key"), 360000);
      writeText(required(options, "handle-out"), operation.toJson());
      std::cout << "ARCHIVE_SEED_ACCEPTED operation=" << operation.operationId
                << " seed=" << operation.seed.repoNode << '\n';
      return 0;
    }
    const auto result = api.archive(inApp, handle,
                                    makeSelected(options, config.servicePrefix), 90000);
    if (!result.handle || !result.receipt) {
      throw std::runtime_error("archive did not produce a Persistent handle and receipt");
    }
    writeText(required(options, "handle-out"), result.handle->toJson());
    writeText(required(options, "receipt-out"), result.receipt->toJson());
    std::cout << "ARCHIVE_COMMITTED sha256=" << result.reference.contentDigest
              << " receipt=" << result.receipt->receiptId << '\n';
    return 0;
  }

  const auto bytes = readBytes(required(options, "handle"));
  const auto handle = repo::ArtifactHandle::fromJson(
    std::string(bytes.begin(), bytes.end()));
  const auto fetched = api.fetchFile(handle, required(options, "destination"), 120000);
  const auto payload = readBytes(required(options, "destination"));
  if (payload.size() != handle.reference.sizeBytes ||
      sha256(payload) != handle.reference.contentDigest ||
      fetched.reference.toJson() != handle.reference.toJson()) {
    throw std::runtime_error("Persistent fetch oracle mismatch");
  }
  if (options.count("receipt")) {
    const auto receiptBytes = readBytes(options.at("receipt"));
    const auto expected = repo::ArtifactCommitReceipt::fromJson(
      std::string(receiptBytes.begin(), receiptBytes.end()));
    const auto actual = api.getReceipt(expected.receiptId, handle, 60000);
    if (actual.toJson() != expected.toJson()) {
      throw std::runtime_error("Persistent receipt interoperability mismatch");
    }
  }
  std::cout << "PERSISTENT_FETCH_PASS sha256=" << sha256(payload)
            << " provider=" << fetched.providerIdentity
            << " repo=" << fetched.repoNode << '\n';
  return 0;
}

} // namespace

int
main(int argc, char* argv[])
{
  try {
    return run(parseOptions(argc, argv));
  }
  catch (const std::exception& e) {
    std::cerr << "spec192-repo-client error: " << e.what() << '\n';
    return 2;
  }
}
