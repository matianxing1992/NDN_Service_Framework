#ifndef NDNSF_DI_MODEL_REPOSITORY_PUBLICATION_HPP
#define NDNSF_DI_MODEL_REPOSITORY_PUBLICATION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ndnsf_distributed_repo { struct RepoArtifactTarget; }

namespace ndnsf::di {

struct NativeStoredModelPacketIo;

/** Check the native Repo target before loading or preparing a model. */
void
nativeValidateModelRepositoryTarget(const ndnsf_distributed_repo::RepoArtifactTarget& repository,
                                    const ndn::Name& producer);

/** Bind bounded original-packet transport to an owned User and its running Face.
 * Read-only by default. Publication explicitly enables the existing storage-grant/INSERT path.
 * The caller must keep the User's I/O loop running until outstanding callbacks drain.
 * requireCurrent must support concurrent invocation when publication is enabled.
 */
NativeStoredModelPacketIo
nativeStoredModelRepositoryIo(std::shared_ptr<ndn_service_framework::ServiceUser> user,
  const ndnsf_distributed_repo::RepoArtifactTarget& repository,
  std::chrono::steady_clock::time_point deadline, std::function<void()> requireCurrent,
  bool allowPublication = false,
  std::function<std::shared_ptr<void>()> acquireOperationLifetime = {});

/** Original-packet storage receipt, not permission to consume a model.
 * A successful return confirms durable independent Repo commit and its exact
 * manifest. Key custody and consumer authorization are separate operations.
 */
struct NativeModelRepositoryReceipt
{
  ndn::Name producer, certificateName, rootName, metadataName;
  std::string rootDigest, metadataDigest;
  std::uint64_t wireBytes = 0;
  std::uint64_t packetGroups = 0;
  std::uint64_t protectedObjects = 0;
};

/** Synchronous bounded storage on the existing writer transport.
 * Never call from its Face thread. Partial/ambiguous failure retains committed
 * groups for Owner reconciliation; no previous publication is removed.
 */
NativeModelRepositoryReceipt
nativeStorePreparedModelPublication(ndn_service_framework::ServiceUser& writer,
  const ndn::security::Certificate& certificate,
  const ndn_service_framework::EncryptedLargeDataRangeStore& source,
  const NativePreparedCanonicalPublication& publication,
  const ndnsf_distributed_repo::RepoArtifactTarget& repository,
  std::chrono::steady_clock::time_point deadline, const std::function<void()>& requireCurrent);

} // namespace ndnsf::di

#endif // NDNSF_DI_MODEL_REPOSITORY_PUBLICATION_HPP
