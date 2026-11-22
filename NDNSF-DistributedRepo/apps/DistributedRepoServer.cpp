#include "ndnsf-distributed-repo/PersistentRepoServerConfig.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"
#include "../src/face-data-reference-fetcher.hpp"

#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/controller-certificate.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/scope.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

void
printUsage(std::ostream& output)
{
  output << "Usage: DistributedRepoServer --repo-node NAME --database ABS_PATH "
            "--group-prefix NAME --provider-identity NAME "
            "--authority-identity NAME --trust-schema ABS_PATH "
            "[--repo-mode persistent] [--service-prefix NAME] "
            "[--cache-bytes N] [--large-object-threshold-bytes N] "
            "[--failure-domain NAME] [--ack-collection-ms N] "
            "[--request-timeout-ms N] [--availability-freshness-ms N] "
            "[--manifest-fetch-window N] [--manifest-source-window N] "
            "[--manifest-handler shared|reserved] [--manifest-fetch-face shared|dedicated]\n";
}

class ManifestFetchFace
{
public:
  explicit
  ManifestFetchFace(boost::asio::io_context& serviceIo)
    : m_work(boost::asio::make_work_guard(m_face.getIoContext()))
    , m_worker([this, &serviceIo] {
        try {
          m_face.getIoContext().run();
        }
        catch (...) {
          m_error = std::current_exception();
          m_failed.store(true, std::memory_order_release);
          // Stop on the service loop itself, including when it is about to
          // restart for readiness or shutdown.
          try {
            boost::asio::post(serviceIo, [&serviceIo] { serviceIo.stop(); });
          }
          catch (...) {
            serviceIo.stop();
          }
        }
      })
  {
  }

  ~ManifestFetchFace()
  {
    m_face.getIoContext().stop();
    m_worker.join();
  }

  ndn::Face&
  get() noexcept
  {
    return m_face;
  }

  void
  rethrowIfFailed() const
  {
    if (m_failed.load(std::memory_order_acquire)) {
      std::rethrow_exception(m_error);
    }
  }

private:
  ndn::Face m_face;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> m_work;
  std::exception_ptr m_error;
  std::atomic<bool> m_failed{false};
  std::thread m_worker;
};

ndn::security::Certificate
getDefaultCertificate(ndn::security::KeyChain& keyChain,
                      const ndn::Name& identityName)
{
  return keyChain.getPib()
    .getIdentity(identityName)
    .getDefaultKey()
    .getDefaultCertificate();
}

} // namespace

int
main(int argc, char* argv[])
{
  if (argc == 2 && std::string(argv[1]) == "--help") {
    printUsage(std::cout);
    return 0;
  }

  try {
    // Parsing and mode/path validation precede Face, KeyChain, Repo stores,
    // and Provider construction so unsupported modes have no startup effects.
    const auto config =
      ndnsf_distributed_repo::PersistentRepoServerConfig::fromArguments(argc, argv);

    ndn::Face face;
    // Outlive the Provider's handler pool and any source callbacks it cancels.
    std::unique_ptr<ManifestFetchFace> manifestFace;
    if (config.dedicatedManifestFetchFace) {
      manifestFace = std::make_unique<ManifestFetchFace>(face.getIoContext());
    }
    auto& fetchFace = manifestFace ? manifestFace->get() : face;
    ndn::security::KeyChain keyChain;
    auto providerIdentity = keyChain.getPib().getIdentity(config.providerIdentity);
    keyChain.setDefaultIdentity(providerIdentity);
    const auto providerCert = getDefaultCertificate(keyChain, config.providerIdentity);
    const auto authorityCert = ndn_service_framework::loadControllerCertificate(
      keyChain, config.authorityIdentity, std::getenv("NDNSF_CONTROLLER_CERT_FILE"));

    std::filesystem::create_directories(config.databasePath.parent_path());
    ndnsf_distributed_repo::StorageCapability capability;
    capability.repoNode = config.repoNode;
    capability.repoMode = "persistent";
    capability.failureDomain = config.failureDomain;

    const auto repoStoreRoot = config.databasePath.string() + ".store";
    auto repoStore = ndnsf_distributed_repo::makeTieredRepoStore(
      ndnsf_distributed_repo::makeFilesystemRepoStore(
        repoStoreRoot, 16 * 1024 * 1024,
        config.largeObjectThresholdBytes,
        config.repoNode + "|" + config.providerIdentity.toUri(ndn::name::UriFormat::ALTERNATE),
        ndnsf_distributed_repo::FilesystemRecoveryPolicy::KEEP_UNCONFIRMED),
      config.memoryCacheBytes, "filesystem", config.largeObjectThresholdBytes);
    // Filesystem availability already excludes the persistent store's bytes.
    // RepoCore accepts total capacity as freeBytes + usedBytes on startup.
    capability.usedBytes = repoStore->usedBytes();
    capability.freeBytes =
      std::filesystem::space(config.databasePath.parent_path()).available;

    ndn_service_framework::CertificatePublisher certificatePublisher(
      face, keyChain, providerCert.getName());
    (void)certificatePublisher;
    auto publisherUser = std::make_shared<ndn_service_framework::ServiceUser>(
      face, config.groupPrefix, providerCert, authorityCert,
      config.trustSchemaPath.string());
    publisherUser->init();
    publisherUser->fetchPermissionsFromController(config.authorityIdentity);
    auto repo = std::make_shared<ndnsf_distributed_repo::RepoNode>(
      config.servicePrefix,
      std::move(capability),
      std::move(repoStore),
      config.repoNode,
      config.providerIdentity.toUri(ndn::name::UriFormat::ALTERNATE),
      config.databasePath.string() + ".artifact-v2.sqlite",
      ndnsf_distributed_repo::ArtifactLimits{},
      config.availabilityFreshnessMs);
    repo->setDataReferenceSpoolFetcher(
      [&face, weakRepo = std::weak_ptr<ndnsf_distributed_repo::RepoNode>(repo),
       timeout = std::chrono::milliseconds(config.requestTimeoutMs)](
        const ndnsf_distributed_repo::RepoDataReference& reference) {
        // The bounded Provider handler pool owns each independent fetch operation.
        const auto requireCurrent = [weakRepo] {
          const auto owner = weakRepo.lock();
          if (!owner || owner->isStoppingOriginalPacketOperations()) {
            throw std::runtime_error("repo-original-packet-insert-shutting-down");
          }
        };
        ndnsf_distributed_repo::detail::FaceDataReferenceFetcher fetcher(face, timeout, timeout);
        return fetcher(reference, requireCurrent);
      });
    ndn_service_framework::ServiceProvider provider(
      face,
      config.groupPrefix,
      providerCert,
      authorityCert,
      config.trustSchemaPath.string());

    using ndnsf_distributed_repo::detail::ManifestSourceWindow;
    std::shared_ptr<ManifestSourceWindow> sourceWindow;
    if (config.manifestSourceWindow != 0) {
      sourceWindow = std::make_shared<ManifestSourceWindow>(config.manifestSourceWindow);
    }
    repo->setManifestPacketFetcher(
      [&fetchFace, &provider, timeout = std::chrono::milliseconds(config.requestTimeoutMs),
       window = config.manifestFetchWindow, sourceWindow = std::move(sourceWindow)](
        const ndnsf_distributed_repo::RepoDataReference& reference,
        const ndn_service_framework::SegmentManifestScope& scope,
        std::chrono::steady_clock::time_point deadline, std::function<void()> requireCurrent) {
        ndnsf_distributed_repo::detail::FaceDataReferenceFetcher fetcher(fetchFace, timeout, timeout,
                                                                       window);
        return fetcher.fetchManifest(reference, scope, deadline,
          [&provider](const auto& wire, const auto& expected, auto expires) {
            return provider.validateManifestRootUntil(wire, expected, expires);
          }, std::move(requireCurrent),
          ndnsf_distributed_repo::detail::MAX_NAMED_PACKET_COUNT, sourceWindow);
      });

    // Fence owned INSERT and validation before Provider teardown on every exit path.
    struct OwnedInsertShutdownFence
    {
      ndnsf_distributed_repo::RepoNode& repo;
      ndn_service_framework::ServiceProvider& provider;

      void
      close() noexcept
      {
        repo.stopOriginalPacketOperations();
        provider.stopProducerPacketValidation();
      }

      ~OwnedInsertShutdownFence() noexcept
      {
        close();
      }
    } shutdownFence{*repo, provider};
    // Drain queued owner-token completions on every exit path (early stopping,
    // readiness failure, exception, normal return). An exhausted bound is a
    // hard exit instead of an unbounded Provider destructor wait.
    struct OwnerDrainGuard
    {
      ndn::Face& face;
      ndn_service_framework::ServiceProvider& provider;
      OwnedInsertShutdownFence& shutdownFence;

      ~OwnerDrainGuard() noexcept
      {
        shutdownFence.close();
        // Refuse new work first so the drain can actually finish; otherwise a
        // still-running executor keeps admitting owner tokens past the bound.
        provider.stopOwnerAdmission();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(10);
        while (provider.hasPendingOwnerTokens() &&
               std::chrono::steady_clock::now() < deadline) {
          face.getIoContext().restart();
          face.getIoContext().run_for(std::chrono::milliseconds(20));
        }
        if (provider.hasPendingOwnerTokens()) {
          std::cerr << "NDNSF_REPO_OWNER_TOKENS_UNDRAINED" << std::endl;
          std::_Exit(3);
        }
      }
    } ownerDrain{face, provider, shutdownFence};
    bool stopping = false;
    const auto signalActive = std::make_shared<bool>(true);
    boost::asio::signal_set signals(face.getIoContext(), SIGINT, SIGTERM);
    signals.async_wait([&, signalActive] (const boost::system::error_code& error, int) {
      if (!error && *signalActive) {
        stopping = true;
        shutdownFence.close();
        face.shutdown();
        // Repo owns this context; SVS/Scheduler work otherwise keeps processEvents alive.
        face.getIoContext().stop();
      }
    });
    // A queued signal must not access owners while their destructors drain callbacks.
    const auto disableSignal = ndn::make_scope_exit([signalActive] { *signalActive = false; });

    repo->registerServices(provider, publisherUser,
                           config.ackCollectionTimeMs,
                           config.requestTimeoutMs);
    if (config.reserveManifestHandler) {
      provider.reserveUnaryHandler(
        ndnsf_distributed_repo::makeRepoServiceName(config.servicePrefix, "MANIFEST"));
    }
    repo->registerOriginalDataServing(face);
    provider.init();
    provider.fetchPermissionsFromController(config.authorityIdentity);

    std::cout << "Persistent Repo serving " << repo->servicePrefix()
              << " as " << config.repoNode << " using "
              << repoStoreRoot << " handlers=" << provider.getHandlerThreads()
              << " manifestHandler=" << (config.reserveManifestHandler ? "reserved" : "shared")
              << " manifestFetchFace=" << (manifestFace ? "dedicated" : "shared")
              << std::endl;
    if (std::getenv("NDNSF_DEPLOYMENT_BOOT_ID") != nullptr) {
      // The manager's fresh owned log binds this marker to its PID/launch.
      // Construction alone does not prove Controller permission or NAC readiness.
      const auto ready = [&] {
        return repo->isOriginalDataServingReady() && provider.isRequestDecryptionReady() &&
          provider.hasProviderPermissionForService(
            ndnsf_distributed_repo::makeRepoServiceName(config.servicePrefix, "STORE")) &&
          provider.hasProviderPermissionForService(
            ndnsf_distributed_repo::makeRepoServiceName(config.servicePrefix, "FETCH")) &&
          provider.hasProviderPermissionForService(
            ndnsf_distributed_repo::makeRepoServiceName(config.servicePrefix, "STATUS"));
      };
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (!stopping && !ready() && std::chrono::steady_clock::now() < deadline) {
        if (manifestFace) manifestFace->rethrowIfFailed();
        face.processEvents(ndn::time::milliseconds(20));
      }
      if (stopping)
        return 0;
      if (!ready())
        throw std::runtime_error("Repo permission/decryption readiness deadline exceeded");
      std::cout << "NDNSF_REPO_READY identity="
                << config.providerIdentity.toUri(ndn::name::UriFormat::CANONICAL) << std::endl;
    }
    if (!stopping)
      face.processEvents();
    if (manifestFace) manifestFace->rethrowIfFailed();
    // Queued completions still hold owner tokens after the context stops; the
    // OwnerDrainGuard above drains them on every exit path before the Provider
    // and Face are destroyed.
    std::cout << "NDNSF_REPO_CACHE_STATUS " << repo->cacheStatus().toJson() << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "DistributedRepoServer error: " << error.what() << std::endl;
    printUsage(std::cerr);
    return 2;
  }
}
