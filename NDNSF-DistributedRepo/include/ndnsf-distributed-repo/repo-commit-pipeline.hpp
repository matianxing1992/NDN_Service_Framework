#ifndef NDNSF_DISTRIBUTED_REPO_REPO_COMMIT_PIPELINE_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_COMMIT_PIPELINE_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ndnsf_distributed_repo {

/** Bounded concurrent publication for one caller scope.
 *
 *  Submissions run on worker threads, at most the configured capacity at a time; drain()
 *  confirms everything outstanding before a dependent object is submitted, and a
 *  failure cancels and drains the rest. The owner thread is fixed at construction so a
 *  stray thread cannot drive the scope.
 *
 *  The bound is **per pipeline, not per client**: a caller that runs several publication
 *  scopes at once runs that many pipelines, and their commits overlap. Nothing in this
 *  library enforces a process-wide limit, and callers must not read MAX_ACTIVE_COMMITS as
 *  one.
 *
 *  This is an internal helper of the Repo library: it is not part of the installed
 *  surface, and callers outside Repo use the client facade instead.
 */
class RepoCommitPipeline
{
public:
  using CurrentCheck = std::function<void()>;
  using Commit = std::function<void(const CurrentCheck&)>;
  static constexpr std::size_t MAX_ACTIVE_COMMITS = 3;
  static constexpr std::size_t MAX_CONFIGURED_COMMITS = 5;
  enum class CompletionOrder { SUBMISSION, READY };

  RepoCommitPipeline(CurrentCheck requireCurrent, std::uint64_t maxGroupWireBytes);
  RepoCommitPipeline(CurrentCheck requireCurrent, std::uint64_t maxGroupWireBytes,
                     std::size_t capacity, CompletionOrder order = CompletionOrder::SUBMISSION);
  ~RepoCommitPipeline() noexcept;
  RepoCommitPipeline(const RepoCommitPipeline&) = delete;
  RepoCommitPipeline&
  operator=(const RepoCommitPipeline&) = delete;

  void
  submit(std::uint64_t wireBytes, Commit commit, std::function<void()> observe = {});

  /** Confirm outstanding groups before submitting a dependent object. */
  void
  drain();

  void
  finish();

private:
  struct Pending
  {
    std::future<void> completion;
    std::function<void()> observe;
    std::shared_ptr<bool> ready;
  };

  void
  requireOwner() const;
  void
  completeOne();
  void
  markReady(const std::shared_ptr<bool>& ready);
  void
  cancelAndDrain() noexcept;
  [[noreturn]] void
  fail(std::exception_ptr error);

  CurrentCheck m_requireCurrent;
  std::thread::id m_owner;
  std::shared_ptr<std::atomic<bool>> m_cancelled;
  std::deque<Pending> m_pending;
  std::mutex m_readyMutex;
  std::condition_variable m_ready;
  std::uint64_t m_maxGroupWireBytes = 0;
  std::size_t m_capacity;
  CompletionOrder m_order;
  bool m_closed = false;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_COMMIT_PIPELINE_HPP
