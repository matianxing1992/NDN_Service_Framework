#ifndef NDNSF_DI_LOCAL_PIPELINE_DEPENDENCY_IO_HPP
#define NDNSF_DI_LOCAL_PIPELINE_DEPENDENCY_IO_HPP

#include "NativeProviderRuntime.hpp"
#include "ProviderRoleWorker.hpp"

#include <condition_variable>
#include <mutex>
#include <set>

namespace ndnsf::di {

class ProtectedRuntime;
class ProviderGroupCoordinator;

class LocalDependencyAttempt
{
public:
  using Clock = std::chrono::steady_clock;

  LocalDependencyAttempt(std::string identity, std::size_t maxSlots, std::size_t maxBytes);

  void attach(const std::string& role, const std::string& identity, bool cpu,
              Clock::time_point deadline);
  bool awaitRoute(const std::function<bool()>& current);
  std::uint64_t leaseDeadline(std::uint64_t proposedDeadline);
  using ConversationUse = std::optional<ConversationStateStore::ExecutionAdmission>;
  ConversationUse acquireConversationUse(const std::function<ConversationUse()>& admit);
  void markComputed(const std::string& role);
  void awaitComputed(const std::function<bool()>& current);
  void finishRole(const std::string& role, bool succeeded);
  void cancelReads(const std::string& role) noexcept;

private:
  friend class LocalPipelineDependencyIo;

  struct PendingRead
  {
    std::promise<TensorBundle> promise;
    std::shared_ptr<const LocalDependencyDelivery> delivery;
    std::shared_ptr<NativeWorkStatistics> statistics;
    std::size_t maxBytes = 0;
  };

  struct Slot
  {
    DependencyEdge edge;
    std::optional<TensorBundle> value;
    std::optional<PendingRead> reader;
    bool consumed = false;
  };

  void requireActive() const;
  Slot& slotFor(const DependencyEdge& edge);
  std::future<TensorBundle> read(const DependencyEdge& edge, PendingRead reader);
  void publish(const DependencyEdge& edge, const TensorBundle& bundle, std::size_t bytes);
  void await(const std::function<bool()>& predicate, const std::function<bool()>& current);

  const std::string m_identity;
  const std::size_t m_maxSlots;
  const std::size_t m_maxBytes;
  std::mutex m_mutex;
  std::condition_variable m_changed;
  Clock::time_point m_deadline = Clock::time_point::max();
  std::set<std::string> m_roles;
  std::set<std::string> m_computed;
  std::set<std::string> m_finished;
  std::set<std::string> m_cancelledReaders;
  ConversationUse m_conversationUse;
  bool m_allCpu = true;
  bool m_failed = false;
  std::size_t m_bytes = 0;
  std::uint64_t m_leaseDeadline = 0;
  std::map<std::string, Slot> m_slots;
};

/** Role-owned view of the admitted local pair. Other edges retain their original IO. */
class LocalPipelineDependencyIo final : public DependencyIo,
                                       public PromptCancellableDependencyIo
{
public:
  LocalPipelineDependencyIo(std::shared_ptr<LocalDependencyAttempt> local,
                            std::shared_ptr<DependencyIo> network,
                            std::shared_ptr<ProtectedRuntime> protection,
                            std::shared_ptr<ProviderGroupCoordinator> group,
                            std::string sessionId, std::string role, std::string provider,
                            std::function<bool()> current,
                            std::shared_ptr<NativeWorkStatistics> statistics);

  std::future<TensorBundle> prefetchInput(const std::string& sessionId,
                                         const DependencyEdge& edge) final;
  void publishOutput(const std::string& sessionId, const DependencyEdge& edge,
                     const TensorBundle& bundle) final;
  bool supportsPromptPrefetchCancellation(const DependencyEdge& edge) const noexcept final;
  PrefetchOperation prefetchInputWithFirstReadBarrier(const std::string& sessionId,
                                                      const DependencyEdge& edge) final;
  void cancelPendingPrefetches() noexcept final;
  void drainPendingPrefetches() noexcept final;

private:
  bool isLocal(const DependencyEdge& edge) const noexcept;
  std::size_t authorize(const std::string& sessionId, const DependencyEdge& edge,
                        bool publish) const;

  const std::shared_ptr<LocalDependencyAttempt> m_local;
  const std::shared_ptr<DependencyIo> m_network;
  PromptCancellableDependencyIo* const m_cancellable;
  const std::shared_ptr<ProtectedRuntime> m_protection;
  const std::shared_ptr<ProviderGroupCoordinator> m_group;
  const std::string m_sessionId;
  const std::string m_role;
  const std::string m_provider;
  const std::function<bool()> m_current;
  const std::shared_ptr<NativeWorkStatistics> m_statistics;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_LOCAL_PIPELINE_DEPENDENCY_IO_HPP
