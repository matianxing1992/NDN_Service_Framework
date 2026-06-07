#ifndef NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_ROLE_WORKER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_ROLE_WORKER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ndnsf::di {

struct InputFetchTiming
{
  std::string scope;
  std::string plannedDataName;
  std::size_t expectedSegments = 0;
  std::size_t expectedBytes = 0;
  std::chrono::steady_clock::time_point prefetchStartedAt;
  std::chrono::steady_clock::time_point fetchCompletedAt;
};

struct ProviderRoleResult
{
  std::map<std::string, TensorBundle> outputsByScope;
  RoleTiming timing;
  std::vector<InputFetchTiming> inputTimings;
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
  explicit ProviderRoleWorker(std::size_t workerCount = std::thread::hardware_concurrency())
  {
    if (workerCount == 0) {
      workerCount = 1;
    }
    m_workers.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) {
      m_workers.emplace_back([this] { workerLoop(); });
    }
  }

  ~ProviderRoleWorker()
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stopping = true;
    }
    m_cv.notify_all();
    for (auto& worker : m_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  std::future<ProviderRoleResult>
  executeAsync(std::string sessionId,
               RoleSpec role,
               std::shared_ptr<DependencyIo> io,
               RoleRunner runner)
  {
    return executeAsync(std::move(sessionId),
                        std::move(role),
                        std::move(io),
                        makeNativeModelRunner(std::move(runner)));
  }

  std::future<ProviderRoleResult>
  executeAsync(std::string sessionId,
               RoleSpec role,
               std::shared_ptr<DependencyIo> io,
               std::shared_ptr<NativeModelRunner> runner)
  {
    if (role.role.empty()) {
      throw std::invalid_argument("ProviderRoleWorker requires a non-empty role");
    }
    if (!io) {
      throw std::invalid_argument("ProviderRoleWorker requires DependencyIo");
    }
    if (!runner) {
      throw std::invalid_argument("ProviderRoleWorker requires NativeModelRunner");
    }

    auto promise = std::make_shared<std::promise<ProviderRoleResult>>();
    auto future = promise->get_future();
    WorkItem item{
      std::move(sessionId),
      std::move(role),
      std::move(io),
      std::move(runner),
      promise,
      std::chrono::steady_clock::now(),
    };
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_stopping) {
        throw std::logic_error("ProviderRoleWorker is stopping");
      }
      m_queue.push_back(std::move(item));
    }
    m_cv.notify_one();
    return future;
  }

private:
  struct WorkItem
  {
    std::string sessionId;
    RoleSpec role;
    std::shared_ptr<DependencyIo> io;
    std::shared_ptr<NativeModelRunner> runner;
    std::shared_ptr<std::promise<ProviderRoleResult>> promise;
    std::chrono::steady_clock::time_point queuedAt;
  };

  void
  workerLoop()
  {
    while (true) {
      WorkItem item;
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return m_stopping || !m_queue.empty(); });
        if (m_stopping && m_queue.empty()) {
          return;
        }
        item = std::move(m_queue.front());
        m_queue.pop_front();
      }
      execute(item);
    }
  }

  void
  execute(const WorkItem& item)
  {
    try {
      item.promise->set_value(runRole(item));
    }
    catch (...) {
      item.promise->set_exception(std::current_exception());
    }
  }

  static ProviderRoleResult
  runRole(const WorkItem& item)
  {
    std::vector<std::future<TensorBundle>> futures;
    std::vector<InputFetchTiming> inputTimings;
    futures.reserve(item.role.inputs.size());
    inputTimings.reserve(item.role.inputs.size());

    for (const auto& edge : item.role.inputs) {
      InputFetchTiming timing;
      timing.scope = edge.scope;
      timing.plannedDataName = edge.plannedDataName;
      timing.expectedSegments = edge.expectedSegments;
      timing.expectedBytes = edge.expectedBytes;
      timing.prefetchStartedAt = std::chrono::steady_clock::now();
      futures.push_back(item.io->prefetchInput(item.sessionId, edge));
      inputTimings.push_back(timing);
    }

    std::map<std::string, TensorBundle> inputsByScope;
    for (std::size_t i = 0; i < futures.size(); ++i) {
      auto bundle = futures[i].get();
      inputTimings[i].fetchCompletedAt = std::chrono::steady_clock::now();
      inputsByScope.emplace(item.role.inputs[i].scope, std::move(bundle));
    }

    ProviderRoleResult result;
    result.timing.role = item.role.role;
    result.timing.queuedAt = item.queuedAt;
    result.timing.startedAt = std::chrono::steady_clock::now();
    result.inputTimings = std::move(inputTimings);

    RoleExecutionContext ctx;
    ctx.sessionId = item.sessionId;
    ctx.role = item.role.role;
    ctx.inputsByScope = std::move(inputsByScope);
    result.outputsByScope = item.runner->run(ctx);

    for (const auto& edge : item.role.outputs) {
      const auto found = result.outputsByScope.find(edge.scope);
      if (found == result.outputsByScope.end()) {
        throw std::logic_error("runner did not publish output scope: " + edge.scope);
      }
      item.io->publishOutput(item.sessionId, edge, found->second);
    }

    result.timing.finishedAt = std::chrono::steady_clock::now();
    return result;
  }

private:
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<WorkItem> m_queue;
  std::vector<std::thread> m_workers;
  bool m_stopping = false;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_ROLE_WORKER_HPP
