#ifndef NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_ROLE_WORKER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_ROLE_WORKER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DependencyWaitScheduler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ndnsf::di {

struct InputFetchTiming
{
  std::string producerRole;
  std::string scope;
  std::string plannedDataName;
  std::vector<std::string> plannedSegmentNames;
  std::size_t expectedSegments = 0;
  std::size_t expectedBytes = 0;
  std::size_t bytes = 0;
  std::chrono::steady_clock::time_point prefetchStartedAt;
  std::chrono::steady_clock::time_point fetchCompletedAt;
};

struct OutputPublishTiming
{
  std::string producerRole;
  std::string scope;
  std::string plannedDataName;
  std::vector<std::string> plannedSegmentNames;
  std::size_t expectedSegments = 0;
  std::size_t expectedBytes = 0;
  std::size_t bytes = 0;
  std::chrono::steady_clock::time_point outputReadyAt;
  std::chrono::steady_clock::time_point publishDoneAt;
};

struct ProviderRoleResult
{
  std::map<std::string, TensorBundle> outputsByScope;
  RoleTiming timing;
  std::vector<InputFetchTiming> inputTimings;
  std::vector<OutputPublishTiming> outputTimings;
  bool exactForwardCacheHit = false;
  std::string exactForwardCacheKey;
};

struct ProviderRoleWorkerSnapshot
{
  std::size_t workerCount = 0;
  std::size_t readyQueueDepth = 0;
  std::size_t waitingForInputCount = 0;
  std::size_t activeWorkerCount = 0;
  std::size_t dependencyWaitWorkerCount = 0;
  std::size_t dependencyWaitQueueCapacity = 0;
  std::size_t dependencyWaitQueuedCount = 0;
  std::size_t dependencyWaitActiveCount = 0;
  std::size_t dependencyWaitCompleted = 0;
  std::size_t dependencyWaitCancelled = 0;
  std::size_t dependencyWaitDeadlineExpired = 0;
  std::size_t dependencyWaitFailed = 0;
  std::size_t dependencyWaitRejected = 0;
  bool stopping = false;

  std::size_t pendingWorkCount() const
  {
    return readyQueueDepth + waitingForInputCount + activeWorkerCount;
  }

  std::size_t idleWorkerCount() const
  {
    return workerCount > activeWorkerCount ? workerCount - activeWorkerCount : 0;
  }
};

class DependencyIo
{
public:
  virtual ~DependencyIo() = default;

  virtual std::future<TensorBundle>
  prefetchInput(const std::string& sessionId, const DependencyEdge& edge) = 0;

  virtual void
  publishOutput(const std::string& sessionId,
                const DependencyEdge& edge,
                const TensorBundle& bundle) = 0;
};

class ProviderRoleWorker
{
public:
  explicit ProviderRoleWorker(
    std::size_t workerCount = std::thread::hardware_concurrency(),
    std::size_t dependencyWaitWorkers = 4,
    std::size_t dependencyWaitQueueCapacity = 1024,
    std::chrono::milliseconds dependencyWaitTimeout = std::chrono::seconds(120));
  ~ProviderRoleWorker();

  std::future<ProviderRoleResult>
  executeAsync(std::string sessionId,
               RoleSpec role,
               std::shared_ptr<DependencyIo> io,
               RoleRunner runner,
               std::map<std::string, TensorBundle> initialInputsByScope = {});

  std::future<ProviderRoleResult>
  executeAsync(std::string sessionId,
               RoleSpec role,
               std::shared_ptr<DependencyIo> io,
               std::shared_ptr<NativeModelRunner> runner,
               std::map<std::string, TensorBundle> initialInputsByScope = {});

  ProviderRoleWorkerSnapshot
  snapshot() const;

private:
  struct WorkItem
  {
    std::string sessionId;
    RoleSpec role;
    std::shared_ptr<DependencyIo> io;
    std::shared_ptr<NativeModelRunner> runner;
    std::map<std::string, TensorBundle> initialInputsByScope;
    std::vector<InputFetchTiming> inputTimings;
    std::shared_ptr<std::promise<ProviderRoleResult>> promise;
    std::chrono::steady_clock::time_point queuedAt;
  };

  struct PendingInput
  {
    DependencyEdge edge;
    std::future<TensorBundle> future;
    InputFetchTiming timing;
  };

  void
  workerLoop();

  void
  execute(const WorkItem& item);

  void
  scheduleWhenInputsReady(WorkItem item, std::vector<PendingInput> pendingInputs);

  void
  enqueueReady(WorkItem item);

  static void
  failPromise(const std::shared_ptr<std::promise<ProviderRoleResult>>& promise,
              std::exception_ptr failure);

  ProviderRoleResult
  runReadyRole(const WorkItem& item);

  std::map<std::string, TensorBundle>
  getCachedOutputs(const std::string& key);

  void
  putCachedOutputs(std::string key, std::map<std::string, TensorBundle> outputs);

  static std::string
  exactForwardCacheKeyFor(const WorkItem& item,
                          const std::map<std::string, TensorBundle>& inputsByScope);

  static TensorBundle
  outputForEdge(const std::map<std::string, TensorBundle>& outputsByScope,
                const DependencyEdge& edge);

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<WorkItem> m_queue;
  std::vector<std::thread> m_workers;
  std::unique_ptr<DependencyWaitScheduler> m_dependencyWaitScheduler;
  std::mutex m_exactForwardCacheMutex;
  std::unordered_map<std::string, std::map<std::string, TensorBundle>> m_exactForwardCache;
  std::vector<std::string> m_exactForwardCacheOrder;
  std::size_t m_exactForwardCacheMaxEntries = 128;
  std::chrono::milliseconds m_dependencyWaitTimeout;
  std::atomic<std::uint64_t> m_nextDependencyWaitId{1};
  std::size_t m_activeWorkers = 0;
  bool m_stopping = false;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_ROLE_WORKER_HPP
