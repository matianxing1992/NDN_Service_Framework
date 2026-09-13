#ifndef NDNSF_DI_CONVERSATION_HPP
#define NDNSF_DI_CONVERSATION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModel.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationContinuation.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

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
