#ifndef NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderGroupCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ndnsf::di {

class NdnsfCollaborationDependencyIo : public DependencyIo
{
public:
  explicit NdnsfCollaborationDependencyIo(
    ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
    int fetchTimeoutMs = 30000,
    std::size_t maxSegmentSize = 7000,
    int freshnessMs = 60000,
    std::shared_ptr<ProviderGroupCoordinator> groupCoordinator = nullptr,
    std::shared_ptr<ProtectedRuntime> protectedRuntime = nullptr);

  std::future<TensorBundle>
  prefetchInput(const std::string& sessionId, const DependencyEdge& edge) override;

  /**
   * Publish a role output under the deterministic Data name assigned by the
   * native execution plan.  This path intentionally does not publish a
   * separate activation-ready notification: consumers discover dependencies by
   * prefetching the planned object/segment names.
   */
  void
  publishOutput(const std::string& sessionId,
                const DependencyEdge& edge,
                const TensorBundle& bundle) override;

private:
  ndn_service_framework::ServiceProvider::CollaborationContext& m_ctx;
  int m_fetchTimeoutMs = 30000;
  std::size_t m_maxSegmentSize = 7000;
  int m_freshnessMs = 60000;
  std::shared_ptr<ProviderGroupCoordinator> m_groupCoordinator;
  std::shared_ptr<ProtectedRuntime> m_protectedRuntime;
  std::mutex m_localDataV1Mutex;
  std::condition_variable m_localDataV1Cv;
  std::map<std::string, std::vector<ndn::Buffer>> m_localDataV1Segments;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP
