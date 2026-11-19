/** Shared in-memory transport for native worker tests; not a network oracle. */
#ifndef NDNSF_TESTS_NATIVE_DEPENDENCY_BUS_HPP
#define NDNSF_TESTS_NATIVE_DEPENDENCY_BUS_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"

#include <atomic>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::di::tests {

class PartialBus
{
public:
  std::future<TensorBundle>
  subscribe(std::string scope)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::promise<TensorBundle> promise;
    auto future = promise.get_future();
    if (m_failure) {
      promise.set_exception(m_failure);
      return future;
    }
    const auto published = m_published.find(scope);
    if (published != m_published.end()) {
      promise.set_value(published->second);
    }
    else {
      m_waiters[std::move(scope)].push_back(std::move(promise));
    }
    return future;
  }

  /** Wake current and future readers when the fixture's bounded generation is stopped. */
  void
  cancel(std::exception_ptr failure) noexcept
  {
    std::map<std::string, std::vector<std::promise<TensorBundle>>> waiters;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_failure) return;
      m_failure = std::move(failure);
      waiters.swap(m_waiters);
    }
    for (auto& entry : waiters)
      for (auto& waiter : entry.second) waiter.set_exception(m_failure);
  }

  void
  publish(const std::string& scope, const TensorBundle& bundle)
  {
    std::vector<std::promise<TensorBundle>> waiters;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_published[scope] = bundle;
      const auto found = m_waiters.find(scope);
      if (found != m_waiters.end()) {
        waiters = std::move(found->second);
        m_waiters.erase(found);
      }
    }
    for (auto& waiter : waiters) waiter.set_value(bundle);
  }

  std::size_t publishedCount() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_published.size();
  }

  /** A copy of what was published under one scope.
   *
   * The bus already keeps the latest bundle per scope for late subscribers; returning a copy lets a
   * test compare an intermediate a step published against the unsplit export's same intermediate,
   * which is the only way to locate where a chain of steps first diverges from the reference.
   */
  std::optional<TensorBundle>
  published(const std::string& scope) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_published.find(scope);
    if (found == m_published.end()) return std::nullopt;
    return found->second;
  }

  std::vector<std::string>
  publishedScopes() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> scopes;
    scopes.reserve(m_published.size());
    for (const auto& item : m_published) scopes.push_back(item.first);
    return scopes;
  }

private:
  mutable std::mutex m_mutex;
  std::map<std::string, TensorBundle> m_published;
  std::map<std::string, std::vector<std::promise<TensorBundle>>> m_waiters;
  std::exception_ptr m_failure;
};

/** DependencyIo that moves contributions over a shared bus instead of a network. */
class BusDependencyIo : public DependencyIo
{
public:
  explicit BusDependencyIo(std::shared_ptr<PartialBus> bus)
    : m_bus(std::move(bus))
  {
  }

  std::future<TensorBundle>
  prefetchInput(const std::string&, const DependencyEdge& edge) override
  {
    ++prefetchCalls;
    return m_bus->subscribe(transportKey(edge));
  }

  void
  publishOutput(const std::string&, const DependencyEdge& edge,
                const TensorBundle& bundle) override
  {
    ++publishCalls;
    m_bus->publish(transportKey(edge), bundle);
  }

  std::atomic<std::size_t> prefetchCalls{0};
  std::atomic<std::size_t> publishCalls{0};

protected:
  virtual const std::string&
  transportKey(const DependencyEdge& edge) const
  {
    return edge.scope;
  }

  std::shared_ptr<PartialBus> m_bus;
};

/** The same transport with explicit cancellation for multi-epoch Coordinator fixtures. */
class CancellableBusDependencyIo final : public BusDependencyIo,
                                        public PromptCancellableDependencyIo
{
public:
  // Return false to model a Producer whose exact object never becomes available.
  using PublicationMutation = std::function<bool(const DependencyEdge&, TensorBundle&)>;

  explicit CancellableBusDependencyIo(std::shared_ptr<PartialBus> bus,
                                      PublicationMutation mutation = {})
    : BusDependencyIo(std::move(bus))
    , m_mutation(std::move(mutation))
  {
  }

  void
  publishOutput(const std::string& sessionId, const DependencyEdge& edge,
                const TensorBundle& bundle) override
  {
    if (!m_mutation) {
      BusDependencyIo::publishOutput(sessionId, edge, bundle);
      return;
    }
    auto changed = bundle;
    if (m_mutation(edge, changed))
      BusDependencyIo::publishOutput(sessionId, edge, changed);
  }

  bool
  supportsPromptPrefetchCancellation(const DependencyEdge&) const noexcept override
  {
    return true;
  }

  PrefetchOperation
  prefetchInputWithFirstReadBarrier(const std::string& sessionId,
                                    const DependencyEdge& edge) override
  {
    auto result = prefetchInput(sessionId, edge);
    std::promise<void> registered;
    registered.set_value();
    return {std::move(result), registered.get_future().share()};
  }

  void
  cancelPendingPrefetches() noexcept override
  {
    m_bus->cancel(std::make_exception_ptr(std::runtime_error("fixture dependency cancelled")));
  }

  void
  drainPendingPrefetches() noexcept override
  {
    // Futures are backed by already-resolved promises; no transport worker owns a thread.
  }

protected:
  const std::string&
  transportKey(const DependencyEdge& edge) const override
  {
    if (edge.plannedDataName.empty())
      throw std::invalid_argument("generation fixture requires an exact Data name");
    return edge.plannedDataName;
  }

private:
  PublicationMutation m_mutation;
};


} // namespace ndnsf::di::tests

#endif // NDNSF_TESTS_NATIVE_DEPENDENCY_BUS_HPP
