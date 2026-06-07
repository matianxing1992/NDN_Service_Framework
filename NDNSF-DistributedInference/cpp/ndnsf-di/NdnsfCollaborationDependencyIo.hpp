#ifndef NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <future>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::di {

class NdnsfCollaborationDependencyIo : public DependencyIo
{
public:
  explicit NdnsfCollaborationDependencyIo(
    ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
    int fetchTimeoutMs = 10000,
    std::size_t maxSegmentSize = 7000,
    int freshnessMs = 60000)
    : m_ctx(ctx)
    , m_fetchTimeoutMs(fetchTimeoutMs)
    , m_maxSegmentSize(maxSegmentSize)
    , m_freshnessMs(freshnessMs)
  {
  }

  std::future<TensorBundle>
  prefetchInput(const std::string&, const DependencyEdge& edge) override
  {
    if (edge.plannedDataName.empty()) {
      throw std::invalid_argument(
        "NdnsfCollaborationDependencyIo requires plannedDataName for input " +
        edge.scope);
    }
    return std::async(std::launch::async, [this, edge] {
      auto payload = m_ctx.fetchLarge(
        ndn::Name(edge.plannedDataName),
        edge.scope,
        m_fetchTimeoutMs);
      if (!payload) {
        throw std::runtime_error(
          "failed to fetch planned dependency object: " +
          edge.plannedDataName);
      }
      TensorBundle bundle;
      bundle.name = edge.plannedDataName;
      bundle.payload.assign(payload->data(), payload->data() + payload->size());
      bundle.expectedSegments = edge.expectedSegments;
      return bundle;
    });
  }

  void
  publishOutput(const std::string&, const DependencyEdge& edge, const TensorBundle& bundle) override
  {
    const auto payload = ndn::Buffer(bundle.payload.data(), bundle.payload.size());
    if (edge.plannedDataName.empty()) {
      m_ctx.publishLarge(
        edge.scope,
        edge.producerRole.empty() ? ndn::Name("/output") : ndn::Name(edge.producerRole),
        payload,
        m_maxSegmentSize,
        m_freshnessMs);
      return;
    }
    m_ctx.publishLargeNamed(
      edge.scope,
      ndn::Name(edge.plannedDataName),
      payload,
      m_maxSegmentSize,
      m_freshnessMs);
  }

private:
  ndn_service_framework::ServiceProvider::CollaborationContext& m_ctx;
  int m_fetchTimeoutMs = 10000;
  std::size_t m_maxSegmentSize = 7000;
  int m_freshnessMs = 60000;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP
