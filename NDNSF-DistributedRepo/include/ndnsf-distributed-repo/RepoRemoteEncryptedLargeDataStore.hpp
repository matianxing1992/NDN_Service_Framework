#pragma once

#include "ndnsf-distributed-repo/ArtifactTypes.hpp"
#include "ndn-service-framework/EncryptedLargeDataRangeStore.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/security/certificate.hpp>
#include <atomic>
#include <chrono>
#include <mutex>

namespace ndnsf_distributed_repo {

/** Original-packet publication to one authenticated remote Repo. The caller
 * keeps the ServiceUser's Face and signing KeyChain alive through all workers.
 * Blocking methods must run outside the Face thread. */
class RepoRemoteEncryptedLargeDataStore final
  : public ndn_service_framework::EncryptedLargeDataRangeStore
{
public:
  struct Options
  {
    RepoArtifactTarget target;
    std::filesystem::path scratchRoot;
    std::uint64_t maxWireBytes = 64ULL << 20;
    std::size_t maxPacketCount = 4096;
    int timeoutMs = 120000;
    int ackCollectionTimeMs = 1000;
  };

  RepoRemoteEncryptedLargeDataStore(
    std::shared_ptr<ndn_service_framework::ServiceUser> writer,
    ndn::security::Certificate producerCertificate, Options options);

  void setPublicationDeadline(std::chrono::steady_clock::time_point deadline);

  bool requiresDurableStorageActor() const noexcept override { return true; }
  bool supportsDurableRetention() const noexcept override { return true; }
  bool supportsOriginalDataPackets() const noexcept override { return true; }
  bool supportsAuxiliaryDataPackets() const noexcept override { return true; }

  std::shared_ptr<const ndn_service_framework::EncryptedLargeDataRangeSource>
  commitFile(const std::string&, const std::filesystem::path&, std::uint64_t,
             const std::function<void()>& = {}) override;

  std::shared_ptr<const ndn_service_framework::EncryptedLargeDataRangeSource>
  commitOriginalDataPackets(const ndn::Name& manifestName,
    const ndn_service_framework::EncryptedLargeDataCommitOptions& options,
    const std::function<std::optional<std::pair<ndn::Name, std::vector<uint8_t>>>()>& next,
    const std::function<void()>& requireActive = {}) override;

private:
  std::shared_ptr<ndn_service_framework::ServiceUser> m_writer;
  ndn::security::Certificate m_certificate;
  Options m_options;
  std::mutex m_deadlineMutex;
  std::chrono::steady_clock::time_point m_deadline =
    std::chrono::steady_clock::time_point::max();
  bool m_started = false;
  std::shared_ptr<std::atomic<std::uint64_t>> m_spoolBytes =
    std::make_shared<std::atomic<std::uint64_t>>(0);
};

} // namespace ndnsf_distributed_repo
