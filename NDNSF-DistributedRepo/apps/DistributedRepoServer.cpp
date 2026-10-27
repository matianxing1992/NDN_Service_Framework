#include "ndnsf-distributed-repo/PersistentRepoServerConfig.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/controller-certificate.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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
            "[--request-timeout-ms N] [--availability-freshness-ms N]\n";
}

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
        config.repoNode + "|" + config.providerIdentity.toUri(ndn::name::UriFormat::ALTERNATE)),
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
    ndn_service_framework::ServiceProvider provider(
      face,
      config.groupPrefix,
      providerCert,
      authorityCert,
      config.trustSchemaPath.string());

    repo->registerServices(provider, publisherUser,
                           config.ackCollectionTimeMs,
                           config.requestTimeoutMs);
    provider.init();
    provider.fetchPermissionsFromController(config.authorityIdentity);

    std::cout << "Persistent Repo serving " << repo->servicePrefix()
              << " as " << config.repoNode << " using "
              << repoStoreRoot << std::endl;
    if (std::getenv("NDNSF_DEPLOYMENT_BOOT_ID") != nullptr) {
      // The manager's fresh owned log binds this marker to its PID/launch.
      // Construction alone does not prove Controller permission or NAC readiness.
      const auto ready = [&] {
        return provider.isRequestDecryptionReady() &&
          provider.hasProviderPermissionForService(
            ndnsf_distributed_repo::makeRepoServiceName(config.servicePrefix, "STORE")) &&
          provider.hasProviderPermissionForService(
            ndnsf_distributed_repo::makeRepoServiceName(config.servicePrefix, "FETCH")) &&
          provider.hasProviderPermissionForService(
            ndnsf_distributed_repo::makeRepoServiceName(config.servicePrefix, "STATUS"));
      };
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (!ready() && std::chrono::steady_clock::now() < deadline)
        face.processEvents(ndn::time::milliseconds(20));
      if (!ready())
        throw std::runtime_error("Repo permission/decryption readiness deadline exceeded");
      std::cout << "NDNSF_REPO_READY identity="
                << config.providerIdentity.toUri(ndn::name::UriFormat::CANONICAL) << std::endl;
    }
    face.processEvents();
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "DistributedRepoServer error: " << error.what() << std::endl;
    printUsage(std::cerr);
    return 2;
  }
}
