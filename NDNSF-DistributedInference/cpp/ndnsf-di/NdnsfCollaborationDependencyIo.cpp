#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"

#include <stdexcept>
#include <utility>

namespace ndnsf::di {

NdnsfCollaborationDependencyIo::NdnsfCollaborationDependencyIo(
  ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
  int fetchTimeoutMs,
  std::size_t maxSegmentSize,
  int freshnessMs)
  : m_ctx(ctx)
  , m_fetchTimeoutMs(fetchTimeoutMs)
  , m_maxSegmentSize(maxSegmentSize)
  , m_freshnessMs(freshnessMs)
{
}

std::future<TensorBundle>
NdnsfCollaborationDependencyIo::prefetchInput(const std::string&, const DependencyEdge& edge)
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
      m_fetchTimeoutMs,
      edge.expectedSegments);
    if (!payload) {
      throw std::runtime_error(
        "failed to fetch planned dependency object: " +
        edge.plannedDataName);
    }
    TensorBundle bundle;
    bundle.name = edge.tensors.size() == 1 ? edge.tensors.front() : edge.plannedDataName;
    bundle.payload.assign(payload->data(), payload->data() + payload->size());
    bundle.expectedSegments = edge.expectedSegments;
    bundle.expectedBytes = edge.expectedBytes;
    return bundle;
  });
}

void
NdnsfCollaborationDependencyIo::publishOutput(const std::string&,
                                              const DependencyEdge& edge,
                                              const TensorBundle& bundle)
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

} // namespace ndnsf::di
