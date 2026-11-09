#include "model-repository-publication.hpp"
#include "stored-model-repository.hpp"

#include <stdexcept>

namespace ndnsf::di {

NativeStoredModelPacketIo
nativeStoredModelRepositoryIo(std::shared_ptr<ndn_service_framework::ServiceUser>,
  const ndnsf_distributed_repo::RepoArtifactTarget&, std::chrono::steady_clock::time_point,
  std::function<void()>, bool, std::function<std::shared_ptr<void>()>)
{
  throw std::invalid_argument("Stored model transport requires the Repo component");
}

void
nativeValidateModelRepositoryTarget(const ndnsf_distributed_repo::RepoArtifactTarget&,
                                    const ndn::Name&)
{
  throw std::invalid_argument("Model repository publication requires the Repo component");
}

NativeModelRepositoryReceipt
nativeStorePreparedModelPublication(ndn_service_framework::ServiceUser&,
  const ndn::security::Certificate&, const ndn_service_framework::EncryptedLargeDataRangeStore&,
  const NativePreparedCanonicalPublication&, const ndnsf_distributed_repo::RepoArtifactTarget&,
  std::chrono::steady_clock::time_point, const std::function<void()>&)
{
  throw std::invalid_argument("Model repository publication requires the Repo component");
}

} // namespace ndnsf::di
