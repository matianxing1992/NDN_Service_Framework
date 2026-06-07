#ifndef NDNSF_DISTRIBUTED_INFERENCE_ASYNC_DATAFLOW_RUNTIME_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_ASYNC_DATAFLOW_RUNTIME_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ndnsf::di {

struct TensorBundle
{
  std::string name;
  std::vector<uint8_t> payload;
  std::size_t expectedSegments = 0;
  std::size_t expectedBytes = 0;
};

struct DependencyEdge
{
  DependencyEdge() = default;

  DependencyEdge(std::string scope,
                 std::string producerRole,
                 std::string consumerRole,
                 std::string plannedDataName,
                 std::size_t expectedSegments = 0,
                 std::size_t expectedBytes = 0,
                 std::vector<std::string> tensors = {});

  std::string scope;
  std::string producerRole;
  std::string consumerRole;
  std::string plannedDataName;
  std::size_t expectedSegments = 0;
  std::size_t expectedBytes = 0;
  std::vector<std::string> tensors;
};

struct RoleSpec
{
  std::string role;
  std::vector<DependencyEdge> inputs;
  std::vector<DependencyEdge> outputs;
};

struct RoleExecutionContext
{
  std::string sessionId;
  std::string role;
  std::map<std::string, TensorBundle> inputsByScope;
};

using RoleRunner = std::function<std::map<std::string, TensorBundle>(
  const RoleExecutionContext&)>;

struct RoleTiming
{
  std::string role;
  std::chrono::steady_clock::time_point queuedAt;
  std::chrono::steady_clock::time_point startedAt;
  std::chrono::steady_clock::time_point finishedAt;
};

struct DataflowResult
{
  std::map<std::string, TensorBundle> outputsByScope;
  std::vector<RoleTiming> roleTimings;
};

class AsyncDataflowRuntime
{
public:
  explicit AsyncDataflowRuntime(std::size_t workerCount = std::thread::hardware_concurrency());

  ~AsyncDataflowRuntime();

  DataflowResult
  run(const std::string& sessionId,
      const std::vector<RoleSpec>& roles,
      const std::map<std::string, TensorBundle>& initialInputsByScope,
      const RoleRunner& runner);

private:
  struct RunState
  {
    std::string sessionId;
    RoleRunner runner;
    std::map<std::string, RoleSpec> roles;
    std::map<std::string, std::set<std::string>> consumersByScope;
    std::map<std::string, TensorBundle> initialInputsByScope;
    std::map<std::string, TensorBundle> availableByScope;
    std::map<std::string, TensorBundle> outputsByScope;
    std::set<std::string> scheduledRoles;
    std::size_t remainingRoles = 0;
    std::vector<RoleTiming> roleTimings;
    std::optional<std::exception_ptr> failure;
    std::mutex mutex;
    std::condition_variable doneCv;
  };

  struct WorkItem
  {
    std::shared_ptr<RunState> state;
    std::string role;
    std::chrono::steady_clock::time_point queuedAt;
  };

  static bool
  readyToRun(const RunState& state, const RoleSpec& role);

  static RoleExecutionContext
  makeContext(const RunState& state, const RoleSpec& role);

  static void
  publishToRun(RunState& state, const std::string& scope, const TensorBundle& bundle);

  void
  scheduleRole(const std::shared_ptr<RunState>& state, const std::string& role);

  void
  workerLoop();

  void
  execute(const WorkItem& item);

  static void
  failRun(RunState& state, std::exception_ptr failure);

private:
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<WorkItem> m_queue;
  std::vector<std::thread> m_workers;
  bool m_stopping = false;
};

double
durationMs(std::chrono::steady_clock::time_point start,
           std::chrono::steady_clock::time_point end);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_ASYNC_DATAFLOW_RUNTIME_HPP
