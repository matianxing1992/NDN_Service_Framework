#ifndef NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_ROLE_WORKER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_ROLE_WORKER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
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
  explicit ProviderRoleWorker(std::size_t workerCount = std::thread::hardware_concurrency());
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

private:
  struct WorkItem
  {
    std::string sessionId;
    RoleSpec role;
    std::shared_ptr<DependencyIo> io;
    std::shared_ptr<NativeModelRunner> runner;
    std::map<std::string, TensorBundle> initialInputsByScope;
    std::shared_ptr<std::promise<ProviderRoleResult>> promise;
    std::chrono::steady_clock::time_point queuedAt;
  };

  void
  workerLoop();

  void
  execute(const WorkItem& item);

  static ProviderRoleResult
  runRole(const WorkItem& item);

  static TensorBundle
  outputForEdge(const std::map<std::string, TensorBundle>& outputsByScope,
                const DependencyEdge& edge);

private:
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<WorkItem> m_queue;
  std::vector<std::thread> m_workers;
  bool m_stopping = false;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_ROLE_WORKER_HPP
