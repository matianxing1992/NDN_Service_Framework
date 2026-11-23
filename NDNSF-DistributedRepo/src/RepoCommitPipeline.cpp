#include "ndnsf-distributed-repo/repo-commit-pipeline.hpp"

#include <algorithm>
#include <chrono>

namespace ndnsf_distributed_repo {

RepoCommitPipeline::RepoCommitPipeline(CurrentCheck requireCurrent,
                                       std::uint64_t maxGroupWireBytes)
  : RepoCommitPipeline(std::move(requireCurrent), maxGroupWireBytes, MAX_ACTIVE_COMMITS)
{
}

RepoCommitPipeline::RepoCommitPipeline(CurrentCheck requireCurrent,
                                       std::uint64_t maxGroupWireBytes, std::size_t capacity,
                                       CompletionOrder order)
  : m_requireCurrent(std::move(requireCurrent))
  , m_owner(std::this_thread::get_id())
  , m_cancelled(std::make_shared<std::atomic<bool>>(false))
  , m_maxGroupWireBytes(maxGroupWireBytes)
  , m_capacity(capacity)
  , m_order(order)
{
  if (!m_requireCurrent) throw std::invalid_argument("REPO_COMMIT_PIPELINE_CURRENT_MISSING");
  if (m_maxGroupWireBytes == 0)
    throw std::invalid_argument("REPO_COMMIT_PIPELINE_WIRE_LIMIT_MISSING");
  if (m_capacity == 0 || m_capacity > MAX_CONFIGURED_COMMITS)
    throw std::invalid_argument("REPO_COMMIT_PIPELINE_CAPACITY_INVALID");
  m_requireCurrent();
}

RepoCommitPipeline::~RepoCommitPipeline() noexcept
{
  cancelAndDrain();
}

void
RepoCommitPipeline::requireOwner() const
{
  if (std::this_thread::get_id() != m_owner)
    throw std::logic_error("REPO_COMMIT_PIPELINE_WRONG_OWNER_THREAD");
}

void
RepoCommitPipeline::cancelAndDrain() noexcept
{
  m_cancelled->store(true, std::memory_order_release);
  m_closed = true;
  while (!m_pending.empty()) {
    auto pending = std::move(m_pending.front());
    m_pending.pop_front();
    try {
      pending.completion.get();
      if (pending.observe) pending.observe();
    }
    catch (...) {
      // A failed scope cannot roll back a later independently durable commit.
      // Observation is best-effort here; the original failure stays primary.
    }
  }
}

void
RepoCommitPipeline::fail(std::exception_ptr error)
{
  cancelAndDrain();
  std::rethrow_exception(error);
}

void
RepoCommitPipeline::markReady(const std::shared_ptr<bool>& ready)
{
  if (!ready) return;
  {
    std::lock_guard<std::mutex> lock(m_readyMutex);
    *ready = true;
  }
  m_ready.notify_one();
}

void
RepoCommitPipeline::completeOne()
{
  auto position = m_pending.begin();
  if (m_order == CompletionOrder::READY) {
    std::unique_lock<std::mutex> lock(m_readyMutex);
    m_ready.wait(lock, [&] {
      position = std::find_if(m_pending.begin(), m_pending.end(), [](const Pending& p) {
        return *p.ready;
      });
      return position != m_pending.end();
    });
  }
  auto pending = std::move(*position);
  m_pending.erase(position);
  try {
    pending.completion.get();
    if (pending.observe) pending.observe();
  }
  catch (...) {
    fail(std::current_exception());
  }
}

void
RepoCommitPipeline::submit(std::uint64_t wireBytes, Commit commit,
                                  std::function<void()> observe)
{
  requireOwner();
  try {
    if (m_closed || !commit || wireBytes == 0 || wireBytes > m_maxGroupWireBytes)
      throw std::invalid_argument("REPO_COMMIT_PIPELINE_SUBMISSION_INVALID");
    m_requireCurrent();
    while (m_pending.size() >= m_capacity) completeOne();
    m_requireCurrent();
    const auto cancelled = m_cancelled;
    // Reserve the owner record before starting a task: allocation failure must
    // not leave an unowned asynchronous commit whose cancellation cannot drain.
    m_pending.emplace_back();
    auto& pending = m_pending.back();
    try {
      pending.observe = std::move(observe);
      if (m_order == CompletionOrder::READY) pending.ready = std::make_shared<bool>(false);
      pending.completion = std::async(std::launch::async,
        [this, commit = std::move(commit), cancelled, ready = pending.ready] {
          try {
            const CurrentCheck current = [cancelled] {
              if (cancelled->load(std::memory_order_acquire))
                throw std::runtime_error("REPO_COMMIT_PIPELINE_CANCELLED");
            };
            current();
            commit(current);
          }
          catch (...) {
            markReady(ready);
            throw;
          }
          markReady(ready);
        });
    }
    catch (...) {
      m_pending.pop_back();
      throw;
    }
  }
  catch (...) {
    fail(std::current_exception());
  }
}

void
RepoCommitPipeline::drain()
{
  requireOwner();
  if (m_closed) throw std::logic_error("REPO_COMMIT_PIPELINE_CLOSED");
  try {
    m_requireCurrent();
    while (!m_pending.empty()) completeOne();
    m_requireCurrent();
  }
  catch (...) {
    fail(std::current_exception());
  }
}

void
RepoCommitPipeline::finish()
{
  drain();
  m_closed = true;
}

} // namespace ndnsf_distributed_repo
