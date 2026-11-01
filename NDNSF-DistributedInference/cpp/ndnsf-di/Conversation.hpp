#ifndef NDNSF_DI_CONVERSATION_HPP
#define NDNSF_DI_CONVERSATION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModel.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationContinuation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedKvPublication.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di {

enum class ConversationHandoffStatus
{
  Pending,
  PromotedAwaitingAppend,
  Succeeded,
  Cancelled,
  Failed
};

/** Shared handle for one Owner-authenticated conversation Provider switch. */
class ConversationHandoffHandle
{
public:
  ConversationHandoffHandle() noexcept = default;

  std::string operationId() const;
  ConversationHandoffStatus status() const;
  void result(std::chrono::milliseconds waitTimeout) const;
  bool cancel() const;

private:
  struct State;
  explicit ConversationHandoffHandle(std::shared_ptr<State> state)
    : m_state(std::move(state))
  {
  }
  static ConversationHandoffHandle create(std::string operationId);
  void setCancel(std::function<bool()> cancel);
  void setCoreIoThreadProbe(std::function<bool()> probe);
  void setPromoted();
  void setSavedWithoutPlacement();
  void setSucceeded();
  void setCancelled();
  void setFailed(std::exception_ptr failure);
  void retainAppendCompletion(Subscription completion);

  std::shared_ptr<State> m_state;
  friend class Conversation;
  friend class NativeInferenceClient;
};

struct NativeDepartureWatchOptionsV1
{
  std::chrono::steady_clock::time_point deadline;
  std::chrono::milliseconds pollInterval{1000};
  std::chrono::milliseconds queryTimeout{1000};
  std::chrono::milliseconds saveTimeout{120000};
  std::size_t maxOutstandingQueries = 1;
  std::size_t maxConcurrentSaves = 1;
  std::map<std::string, NativeRemoteProtectedKvRoleTrustPolicyV1>
    trustedSourcePolicyByRole;
};

enum class NativeDepartureWatchStatusV1
{
  Watching, Saving, Saved, Cancelled, Expired, Failed
};

struct NativeDepartureWatchResultV1
{
  NativeDepartureWatchStatusV1 status = NativeDepartureWatchStatusV1::Watching;
  std::string conversationId;
  std::string checkpointDigest;
  std::string providerIdentity;
  std::string withdrawalId;
  std::string saveOperationId;
  std::string reason;
};

struct NativeDepartureWatchState;

/** Owner-enabled polling for an authenticated, voluntary Provider departure. */
class NativeDepartureWatchHandle
{
public:
  NativeDepartureWatchHandle() noexcept = default;
  bool valid() const noexcept;
  NativeDepartureWatchStatusV1 status() const;
  NativeDepartureWatchResultV1 result(std::chrono::milliseconds waitTimeout) const;
  /** Stop future queries. A save already past durable COMMIT cannot be cancelled. */
  bool cancel() noexcept;
  void close() noexcept;

private:
  explicit NativeDepartureWatchHandle(std::shared_ptr<NativeDepartureWatchState> state)
    : m_state(std::move(state)) {}
  std::shared_ptr<NativeDepartureWatchState> m_state;
  friend class Conversation;
};

/**
 * Move-only serial entry point for one model-bound native conversation.
 * The coordinator and journal remain native owners; this wrapper stores only
 * an active-turn guard and the verified model/runtime lease.
 */
class Conversation
{
public:
  Conversation() = delete;
  Conversation(Conversation&&) noexcept = default;
  Conversation& operator=(Conversation&&) noexcept;
  Conversation(const Conversation&) = delete;
  Conversation& operator=(const Conversation&) = delete;
  ~Conversation() noexcept;

  RequestHandle request(const Input& input, const RequestOptions& options = {}) const;
  ConversationHandoffHandle switchProviders(
    const std::map<std::string, std::string>& destinationProvidersByRole,
    const std::map<std::string, NativeRemoteProtectedKvRoleTrustPolicyV1>&
      trustedSourcePolicyByRole,
    std::chrono::steady_clock::time_point deadline) const;
  NativeDepartureWatchHandle watchProviderDeparture(
    const NativeDepartureWatchOptionsV1& options) const;
  ConversationCheckpoint checkpoint() const;
  void exportCheckpoint(const std::filesystem::path& destination) const;
  void close() noexcept;

private:
  struct State;
  explicit Conversation(std::shared_ptr<State> state)
    : m_state(std::move(state))
  {
  }

  NativeConversationContinuation makeContinuation(const Input& input) const;
  void requireIdle() const;

  std::shared_ptr<State> m_state;
  friend class PreparedModel;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_CONVERSATION_HPP
