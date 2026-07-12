#ifndef NDNSF_DISTRIBUTED_INFERENCE_DEPENDENCY_WAIT_SCHEDULER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_DEPENDENCY_WAIT_SCHEDULER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

namespace ndnsf::di {

enum class DependencyWaitStatus
{
  Completed,
  Cancelled,
  DeadlineExpired,
  Failed,
};

enum class DependencyWaitSubmitResult
{
  Accepted,
  QueueFull,
  Duplicate,
  ShuttingDown,
};

const char* toString(DependencyWaitStatus status) noexcept;
const char* toString(DependencyWaitSubmitResult result) noexcept;
std::ostream& operator<<(std::ostream& os, DependencyWaitStatus status);
std::ostream& operator<<(std::ostream& os, DependencyWaitSubmitResult result);

class DependencyWaitControl
{
public:
  bool isCancelled() const noexcept;
  bool deadlineExpired() const noexcept;

private:
  friend class DependencyWaitScheduler;
  DependencyWaitControl(std::shared_ptr<std::atomic<bool>> cancelled,
                        std::chrono::steady_clock::time_point deadline);

private:
  std::shared_ptr<std::atomic<bool>> m_cancelled;
  std::chrono::steady_clock::time_point m_deadline;
};

struct DependencyWaitResult
{
  std::string waitId;
  DependencyWaitStatus status = DependencyWaitStatus::Failed;
  std::string reason;
};

struct DependencyWaitSchedulerSnapshot
{
  std::size_t workerCount = 0;
  std::size_t queueCapacity = 0;
  std::size_t queuedCount = 0;
  std::size_t activeCount = 0;
  std::size_t completed = 0;
  std::size_t cancelled = 0;
  std::size_t deadlineExpired = 0;
  std::size_t failed = 0;
  std::size_t rejected = 0;
  bool stopping = false;
};

class DependencyWaitScheduler
{
public:
  using WaitTask = std::function<DependencyWaitStatus(const DependencyWaitControl&)>;
  using Completion = std::function<void(const DependencyWaitResult&)>;

  explicit DependencyWaitScheduler(std::size_t workerCount = 4,
                                   std::size_t queueCapacity = 1024);
  ~DependencyWaitScheduler();

  DependencyWaitScheduler(const DependencyWaitScheduler&) = delete;
  DependencyWaitScheduler& operator=(const DependencyWaitScheduler&) = delete;

  DependencyWaitSubmitResult
  submit(std::string waitId,
         std::chrono::steady_clock::time_point deadline,
         WaitTask task,
         Completion completion);

  bool cancel(const std::string& waitId);
  void shutdown() noexcept;
  bool waitForIdle(std::chrono::milliseconds timeout);
  DependencyWaitSchedulerSnapshot snapshot() const;

private:
  struct Job
  {
    std::string waitId;
    std::chrono::steady_clock::time_point deadline;
    WaitTask task;
    Completion completion;
    std::shared_ptr<std::atomic<bool>> cancelled;
  };

  void workerLoop();
  void finish(const std::shared_ptr<Job>& job, DependencyWaitStatus status,
              std::string reason) noexcept;

private:
  const std::size_t m_queueCapacity;
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::condition_variable m_idleCv;
  std::deque<std::shared_ptr<Job>> m_queue;
  std::map<std::string, std::shared_ptr<Job>> m_jobs;
  std::vector<std::thread> m_workers;
  std::size_t m_activeCount = 0;
  std::size_t m_completed = 0;
  std::size_t m_cancelled = 0;
  std::size_t m_deadlineExpired = 0;
  std::size_t m_failed = 0;
  std::size_t m_rejected = 0;
  bool m_stopping = false;
  bool m_joined = false;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_DEPENDENCY_WAIT_SCHEDULER_HPP
