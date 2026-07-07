#ifndef NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/Stream.hpp"

#include <future>
#include <string>

namespace ndnsf::di {

class NdnsfCollaborationDependencyIo : public DependencyIo
{
public:
  explicit NdnsfCollaborationDependencyIo(
    ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
    int fetchTimeoutMs = 30000,
    std::size_t maxSegmentSize = 7000,
    int freshnessMs = 60000,
    bool streamChunkDependencies = false);

  static constexpr const char* StreamTensorBundleContentType =
    "application/x-ndnsf-di-tensor-bundle";

  /**
   * Encode one complete tensor bundle as one app-neutral StreamChunk.
   *
   * This helper is intentionally independent of CollaborationContext so
   * the DI large-data payload format can be tested without networking.
   */
  static ndn::Buffer
  encodeTensorBundleAsStreamChunk(const std::string& sessionId,
                                  const DependencyEdge& edge,
                                  const TensorBundle& bundle);

  /**
   * Decode one complete tensor bundle from a StreamChunk payload fetched
   * through NDNSF large-data.
   */
  static TensorBundle
  decodeTensorBundleFromStreamChunk(const std::string& sessionId,
                                    const DependencyEdge& edge,
                                    const ndn::Buffer& payload);

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
  bool m_streamChunkDependencies = false;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP
