#include "NDNSF-DistributedInference/cpp/ndnsf-di/DependencyWaitScheduler.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {

const char*
toString(DependencyWaitStatus status) noexcept
{
  switch (status) {
  case DependencyWaitStatus::Completed: return "COMPLETED";
  case DependencyWaitStatus::Cancelled: return "CANCELLED";
  case DependencyWaitStatus::DeadlineExpired: return "DEADLINE_EXPIRED";
  case DependencyWaitStatus::Failed: return "FAILED";
  }
  return "FAILED";
}

const char*
toString(DependencyWaitSubmitResult result) noexcept
{
  switch (result) {
  case DependencyWaitSubmitResult::Accepted: return "ACCEPTED";
  case DependencyWaitSubmitResult::QueueFull: return "QUEUE_FULL";
  case DependencyWaitSubmitResult::Duplicate: return "DUPLICATE";
  case DependencyWaitSubmitResult::ShuttingDown: return "SHUTTING_DOWN";
  }
  return "SHUTTING_DOWN";
}

std::ostream&
operator<<(std::ostream& os, DependencyWaitStatus status)
{
  return os << toString(status);
}

std::ostream&
operator<<(std::ostream& os, DependencyWaitSubmitResult result)
{
  return os << toString(result);
}

DependencyWaitControl::DependencyWaitControl(
  std::shared_ptr<std::atomic<bool>> cancelled,
  std::chrono::steady_clock::time_point deadline)
  : m_cancelled(std::move(cancelled))
  , m_deadline(deadline)
{
}

bool
DependencyWaitControl::isCancelled() const noexcept
{
  return m_cancelled && m_cancelled->load(std::memory_order_acquire);
}

bool
DependencyWaitControl::deadlineExpired() const noexcept
{
  return m_deadline != std::chrono::steady_clock::time_point{} &&
         std::chrono::steady_clock::now() >= m_deadline;
}

DependencyWaitScheduler::DependencyWaitScheduler(std::size_t workerCount,
                                                 std::size_t queueCapacity)
  : m_queueCapacity(queueCapacity)
{
  if (workerCount == 0) {
    throw std::invalid_argument("dependency wait scheduler requires workers");
  }
  if (queueCapacity == 0) {
    throw std::invalid_argument("dependency wait scheduler requires queue capacity");
  }
  m_workers.reserve(workerCount);
  for (std::size_t i = 0; i < workerCount; ++i) {
    m_workers.emplace_back([this] { workerLoop(); });
  }
}

DependencyWaitScheduler::~DependencyWaitScheduler()
{
  shutdown();
}

DependencyWaitSubmitResult
DependencyWaitScheduler::submit(std::string waitId,
                                std::chrono::steady_clock::time_point deadline,
                                WaitTask task,
                                Completion completion)
{
  if (waitId.empty() || !task || !completion) {
    throw std::invalid_argument("dependency wait submission is incomplete");
  }
  auto job = std::make_shared<Job>(Job{
    std::move(waitId), deadline, std::move(task), std::move(completion),
    std::make_shared<std::atomic<bool>>(false),
  });
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
      ++m_rejected;
      return DependencyWaitSubmitResult::ShuttingDown;
    }
    if (m_jobs.count(job->waitId) != 0) {
      ++m_rejected;
      return DependencyWaitSubmitResult::Duplicate;
    }
    if (m_queue.size() >= m_queueCapacity) {
      ++m_rejected;
      return DependencyWaitSubmitResult::QueueFull;
    }
    m_jobs.emplace(job->waitId, job);
    m_queue.push_back(job);
  }
  m_cv.notify_one();
  return DependencyWaitSubmitResult::Accepted;
}

bool
DependencyWaitScheduler::cancel(const std::string& waitId)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_jobs.find(waitId);
  if (found == m_jobs.end()) {
    return false;
  }
  found->second->cancelled->store(true, std::memory_order_release);
  m_cv.notify_all();
  return true;
}

void
DependencyWaitScheduler::shutdown() noexcept
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_joined) {
      return;
    }
    m_stopping = true;
    for (const auto& item : m_jobs) {
      item.second->cancelled->store(true, std::memory_order_release);
    }
  }
  m_cv.notify_all();
  for (auto& worker : m_workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_joined = true;
  }
}

bool
DependencyWaitScheduler::waitForIdle(std::chrono::milliseconds timeout)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  return m_idleCv.wait_for(lock, timeout, [this] {
    return m_queue.empty() && m_activeCount == 0 && m_jobs.empty();
  });
}

DependencyWaitSchedulerSnapshot
DependencyWaitScheduler::snapshot() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return DependencyWaitSchedulerSnapshot{
    m_workers.size(), m_queueCapacity, m_queue.size(), m_activeCount,
    m_completed, m_cancelled, m_deadlineExpired, m_failed, m_rejected,
    m_stopping,
  };
}

void
DependencyWaitScheduler::workerLoop()
{
  while (true) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
      if (m_stopping && m_queue.empty()) {
        return;
      }
      job = std::move(m_queue.front());
      m_queue.pop_front();
      ++m_activeCount;
    }

    DependencyWaitStatus status = DependencyWaitStatus::Failed;
    std::string reason;
    try {
      DependencyWaitControl control(job->cancelled, job->deadline);
      if (control.isCancelled()) {
        status = DependencyWaitStatus::Cancelled;
      }
      else if (control.deadlineExpired()) {
        status = DependencyWaitStatus::DeadlineExpired;
      }
      else {
        status = job->task(control);
      }
    }
    catch (const std::exception& error) {
      status = DependencyWaitStatus::Failed;
      reason = error.what();
    }
    catch (...) {
      status = DependencyWaitStatus::Failed;
      reason = "unknown dependency wait failure";
    }
    finish(job, status, std::move(reason));
  }
}

void
DependencyWaitScheduler::finish(const std::shared_ptr<Job>& job,
                                DependencyWaitStatus status,
                                std::string reason) noexcept
{
  try {
    job->completion(DependencyWaitResult{job->waitId, status, std::move(reason)});
  }
  catch (...) {
    // A completion observer must not terminate a fixed scheduler worker.
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_activeCount > 0) {
      --m_activeCount;
    }
    m_jobs.erase(job->waitId);
    switch (status) {
    case DependencyWaitStatus::Completed: ++m_completed; break;
    case DependencyWaitStatus::Cancelled: ++m_cancelled; break;
    case DependencyWaitStatus::DeadlineExpired: ++m_deadlineExpired; break;
    case DependencyWaitStatus::Failed: ++m_failed; break;
    }
    if (m_queue.empty() && m_activeCount == 0 && m_jobs.empty()) {
      m_idleCv.notify_all();
    }
  }
}

} // namespace ndnsf::di
