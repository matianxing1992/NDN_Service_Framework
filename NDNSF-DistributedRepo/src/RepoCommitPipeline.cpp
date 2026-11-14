#include "ndnsf-distributed-repo/repo-commit-pipeline.hpp"

#include <chrono>

namespace ndnsf_distributed_repo {

RepoCommitPipeline::RepoCommitPipeline(CurrentCheck requireCurrent,
                                       std::uint64_t maxGroupWireBytes)
  : m_requireCurrent(std::move(requireCurrent))
  , m_owner(std::this_thread::get_id())
  , m_cancelled(std::make_shared<std::atomic<bool>>(false))
  , m_maxGroupWireBytes(maxGroupWireBytes)
{
  if (!m_requireCurrent) throw std::invalid_argument("REPO_COMMIT_PIPELINE_CURRENT_MISSING");
  if (m_maxGroupWireBytes == 0)
    throw std::invalid_argument("REPO_COMMIT_PIPELINE_WIRE_LIMIT_MISSING");
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
RepoCommitPipeline::completeFirst()
{
  auto pending = std::move(m_pending.front());
  m_pending.pop_front();
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
    while (m_pending.size() >= MAX_ACTIVE_COMMITS) completeFirst();
    m_requireCurrent();
    const auto cancelled = m_cancelled;
    // Reserve the owner record before starting a task: allocation failure must
    // not leave an unowned asynchronous commit whose cancellation cannot drain.
    m_pending.emplace_back();
    auto& pending = m_pending.back();
    try {
      pending.observe = std::move(observe);
      pending.completion = std::async(std::launch::async,
        [commit = std::move(commit), cancelled] {
          const CurrentCheck current = [cancelled] {
            if (cancelled->load(std::memory_order_acquire))
              throw std::runtime_error("REPO_COMMIT_PIPELINE_CANCELLED");
          };
          current();
          commit(current);
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
    while (!m_pending.empty()) completeFirst();
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
