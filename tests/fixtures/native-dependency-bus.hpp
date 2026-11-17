/** Shared in-memory transport for native worker tests; not a network oracle. */
#ifndef NDNSF_TESTS_NATIVE_DEPENDENCY_BUS_HPP
#define NDNSF_TESTS_NATIVE_DEPENDENCY_BUS_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"

#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    const auto published = m_published.find(scope);
    std::promise<TensorBundle> promise;
    auto future = promise.get_future();
    if (published != m_published.end()) {
      promise.set_value(published->second);
    }
    else {
      m_waiters[std::move(scope)].push_back(std::move(promise));
    }
    return future;
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
};

/** DependencyIo that moves contributions over a shared bus instead of a network. */
class BusDependencyIo final : public DependencyIo
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
    return m_bus->subscribe(edge.scope);
  }

  void
  publishOutput(const std::string&, const DependencyEdge& edge,
                const TensorBundle& bundle) override
  {
    ++publishCalls;
    m_bus->publish(edge.scope, bundle);
  }

  std::atomic<std::size_t> prefetchCalls{0};
  std::atomic<std::size_t> publishCalls{0};

private:
  std::shared_ptr<PartialBus> m_bus;
};


} // namespace ndnsf::di::tests

#endif // NDNSF_TESTS_NATIVE_DEPENDENCY_BUS_HPP
