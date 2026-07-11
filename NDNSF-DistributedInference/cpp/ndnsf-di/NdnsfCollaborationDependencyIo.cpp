#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

bool
dependencyObjectTraceEnabled()
{
  return std::getenv("NDNSF_DI_RUNTIME_TIMING") != nullptr ||
         std::getenv("NDNSF_DI_DEPENDENCY_OBJECT_TRACE") != nullptr;
}

void
logDependencyObject(const std::string& sessionId,
                    const DependencyEdge& edge,
                    const char* direction,
                    std::size_t payloadBytes,
                    const char* status)
{
  if (!dependencyObjectTraceEnabled()) {
    return;
  }
  std::cout << "\nNDNSF_DI_DEPENDENCY_OBJECT"
            << " session=" << sessionId
            << " scope=" << edge.scope
            << " producer=" << edge.producerRole
            << " consumer=" << edge.consumerRole
            << " direction=" << direction
            << " payload_bytes=" << payloadBytes
            << " planned_name=" << (edge.plannedDataName.empty() ? "none" : edge.plannedDataName)
            << " status=" << status
            << std::endl;
}

} // namespace

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
NdnsfCollaborationDependencyIo::prefetchInput(const std::string& sessionId,
                                              const DependencyEdge& edge)
{
  if (edge.plannedDataName.empty()) {
    throw std::invalid_argument(
      "NdnsfCollaborationDependencyIo requires plannedDataName for input " +
      edge.scope);
  }
  return std::async(std::launch::async, [this, sessionId, edge] {
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
    logDependencyObject(sessionId, edge, "fetch", bundle.payload.size(), "ok");
    return bundle;
  });
}

void
NdnsfCollaborationDependencyIo::publishOutput(const std::string& sessionId,
                                              const DependencyEdge& edge,
                                              const TensorBundle& bundle)
{
  const ndn::Buffer payload(bundle.payload.data(), bundle.payload.size());
  logDependencyObject(sessionId, edge, "publish", bundle.payload.size(), "ok");
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
