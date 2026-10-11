#include "ndnsf-distributed-repo/PersistentRepoServerConfig.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>

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
    const auto authorityCert = getDefaultCertificate(keyChain, config.authorityIdentity);

    std::filesystem::create_directories(config.databasePath.parent_path());
    ndnsf_distributed_repo::StorageCapability capability;
    capability.repoNode = config.repoNode;
    capability.repoMode = "persistent";
    capability.freeBytes =
      std::filesystem::space(config.databasePath.parent_path()).available;
    capability.failureDomain = config.failureDomain;

    const auto repoStoreRoot = config.databasePath.string() + ".store";
    auto repoStore = ndnsf_distributed_repo::makeTieredRepoStore(
      ndnsf_distributed_repo::makeFilesystemRepoStore(
        repoStoreRoot, config.largeObjectThresholdBytes,
        config.largeObjectThresholdBytes,
        config.repoNode + "|" + config.providerIdentity.toUri()),
      config.memoryCacheBytes, "filesystem", config.largeObjectThresholdBytes);

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
      config.providerIdentity.toUri(),
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
    face.processEvents();
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "DistributedRepoServer error: " << error.what() << std::endl;
    printUsage(std::cerr);
    return 2;
  }
}
