#ifndef NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <future>
#include <string>

namespace ndnsf::di {

class NdnsfCollaborationDependencyIo : public DependencyIo
{
public:
  explicit NdnsfCollaborationDependencyIo(
    ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
    int fetchTimeoutMs = 10000,
    std::size_t maxSegmentSize = 7000,
    int freshnessMs = 60000);

  std::future<TensorBundle>
  prefetchInput(const std::string& sessionId, const DependencyEdge& edge) override;

  void
  publishOutput(const std::string& sessionId,
                const DependencyEdge& edge,
                const TensorBundle& bundle) override;

private:
  ndn_service_framework::ServiceProvider::CollaborationContext& m_ctx;
  int m_fetchTimeoutMs = 10000;
  std::size_t m_maxSegmentSize = 7000;
  int m_freshnessMs = 60000;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP
