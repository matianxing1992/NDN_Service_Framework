#ifndef NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderGroupCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

struct PrefetchWorkerOracle;

class NdnsfCollaborationDependencyIo : public DependencyIo,
                                     public PromptCancellableDependencyIo
{
public:
  explicit NdnsfCollaborationDependencyIo(
    ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
    int fetchTimeoutMs = 30000,
    std::size_t maxSegmentSize = 7000,
    int freshnessMs = 60000,
    std::shared_ptr<ProviderGroupCoordinator> groupCoordinator = nullptr,
    std::shared_ptr<ProtectedRuntime> protectedRuntime = nullptr,
    std::size_t prefetchWorkerCount = 0,
    std::shared_ptr<NativeWorkStatistics> workStatistics = {});

  ~NdnsfCollaborationDependencyIo() override;

  std::future<TensorBundle>
  prefetchInput(const std::string& sessionId, const DependencyEdge& edge) override;

  PromptCancellableDependencyIo::PrefetchOperation
  prefetchInputWithFirstReadBarrier(
    const std::string& sessionId, const DependencyEdge& edge) override;

  bool
  supportsPromptPrefetchCancellation(
    const DependencyEdge& edge) const noexcept override;

  void
  cancelPendingPrefetches() noexcept override;

  void
  drainPendingPrefetches() noexcept override;

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
  friend struct PrefetchWorkerOracle;

  ndn_service_framework::ServiceProvider::CollaborationContext& m_ctx;
  int m_fetchTimeoutMs = 30000;
  std::size_t m_maxSegmentSize = 7000;
  int m_freshnessMs = 60000;
  std::shared_ptr<ProviderGroupCoordinator> m_groupCoordinator;
  std::shared_ptr<ProtectedRuntime> m_protectedRuntime;
  std::shared_ptr<NativeWorkStatistics> m_workStatistics;
  // Invocation-owned immutable originals. A live entry is never evicted or
  // re-encrypted; changed plaintext or protection bindings fail explicitly.
  struct CachedTensorPublication {
    std::string fingerprint;
    std::vector<std::shared_ptr<const ndn::Data>> originals;
    std::size_t wireBytes = 0;
  };
  std::mutex m_tensorPublicationMutex;
  std::map<ndn::Name, CachedTensorPublication> m_tensorPublications;
  std::size_t m_tensorPublicationBytes = 0;
  // One invocation-owned advisory, learned only from a fully admitted decode
  // object. It never authorizes input or replaces the next signed manifest.
  struct DecodedTensorSizing {
    std::string key;
    std::uint64_t round = 0;
    std::uint64_t bytes = 0;
    std::uint64_t segmentBytes = 0;
  };
  std::mutex m_decodedSizingMutex;
  std::optional<DecodedTensorSizing> m_decodedSizing;
  std::shared_ptr<std::atomic<bool>> m_pendingPrefetchCancelled =
    std::make_shared<std::atomic<bool>>(false);
  // Dedicated invocation workers must drain before the state above is released.
  std::unique_ptr<ndn_service_framework::BoundedWorkerPool> m_prefetchWorkers;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NDNSF_COLLABORATION_DEPENDENCY_IO_HPP
