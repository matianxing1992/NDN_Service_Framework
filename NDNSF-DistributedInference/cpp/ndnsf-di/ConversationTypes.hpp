#ifndef NDNSF_DI_CONVERSATION_TYPES_HPP
#define NDNSF_DI_CONVERSATION_TYPES_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf::di {

class Conversation;

/** Opaque owning bytes for one authenticated native conversation checkpoint. */
class ConversationCheckpoint
{
public:
  ConversationCheckpoint() = default;
  static ConversationCheckpoint fromBytes(const std::vector<std::uint8_t>& bytes);
  std::vector<std::uint8_t> bytes() const;

private:
  explicit ConversationCheckpoint(std::string wire)
    : m_wire(std::move(wire))
  {
  }
  std::string m_wire;
  friend class Conversation;
  friend class PreparedModel;
};

struct ConversationOptions
{
  std::string conversationId;
  std::optional<ConversationCheckpoint> checkpoint;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_CONVERSATION_TYPES_HPP
