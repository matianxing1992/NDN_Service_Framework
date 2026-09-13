#ifndef NDNSF_DI_NATIVE_CONVERSATION_CONTINUATION_HPP
#define NDNSF_DI_NATIVE_CONVERSATION_CONTINUATION_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Complete native continuation value shared by the client and coordinator. */
struct NativeConversationContinuation
{
  std::string conversationId;
  std::uint64_t parentContextEpoch = 0;
  std::string serviceName;
  std::string planRoleMapDigest;
  std::string parentCheckpointDigest;
  // Digest of the current encoded request envelope. The requester fills an
  // empty value after native requestId allocation and validates a supplied
  // value against that envelope.
  std::string requestContractDigest;
  std::uint64_t retentionDeadlineMs = 0;
  std::string mode = "FULL_CONTEXT";
  std::string parentCheckpointWire;
  std::string generationId;
  std::vector<std::int64_t> canonicalTokenIds;
  std::vector<std::string> expectedRoles;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_CONVERSATION_CONTINUATION_HPP
