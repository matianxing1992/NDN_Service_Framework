#ifndef NDNSF_DI_NATIVE_CONVERSATION_COORDINATOR_HPP
#define NDNSF_DI_NATIVE_CONVERSATION_COORDINATOR_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationJournal.hpp"
#include <functional>

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {
class NativeDiError;

struct NativeConversationConfig
{
  std::shared_ptr<NativeConversationJournal> journal;
  // Empty uses the journal's domain-separated key ring. Without a journal,
  // the owner must explicitly supply a 32-byte authentication key ring.
  std::vector<std::vector<std::uint8_t>> authenticationKeys;
  std::string requesterIdentity;
  std::string serviceName;
  std::string securityDomainDigest;
  std::function<std::uint64_t()> nowMs;
};

struct NativeConversationContinuation
{
  std::string conversationId;
  std::uint64_t parentContextEpoch = 0;
  std::string serviceName;
  std::string planRoleMapDigest;
  std::string parentCheckpointDigest;
  // Digest of the current encoded request envelope. The requester fills an
  // empty value after its native owner allocates requestId; a supplied value
  // is checked exactly against that envelope.
  std::string requestContractDigest;
  std::uint64_t retentionDeadlineMs = 0;
  std::string mode = "FULL_CONTEXT";
  std::string parentCheckpointWire;
  std::string generationId;
  std::vector<std::int64_t> canonicalTokenIds;
  std::vector<std::string> expectedRoles;
};

struct NativeConversationTurn
{
  NativeConversationContinuation parent;
  std::string requestId;
  std::uint64_t attempt = 1;
  std::uint64_t successorContextEpoch = 0;
  std::vector<std::int64_t> acceptedTokenIds;
  std::string prefixDigest;
  bool aborted = false;
  std::string ticket;
  std::string executionRequestId;
};

struct NativeCompletedAttempt
{
  std::string requestId;
  std::uint64_t attempt = 0;
  std::vector<std::int64_t> tokenIds;
  std::string providerStateDigest;
  bool complete = false;
  std::string generationId;
  std::string modelContractDigest;
  std::string tokenizerDigest;
  std::string chatTemplateDigest;
  std::string applicationMessages;
  std::vector<NativeJson> authenticatedReceipts;
  std::function<void(const std::string&)> commitProviderState;
  std::function<void()> rollbackProviderState;
  // The operation owner serializes this publication with cancel/deadline and
  // its successful terminal transition. It must execute publish exactly once.
  std::function<void(const std::function<void()>& publish)> durableCommitGate;
  // Best-effort release of Provider commit-wait slots after durable success.
  std::function<void()> finalizeProviderState;
};

struct NativeConversationCheckpoint
{
  std::string conversationId;
  std::uint64_t parentContextEpoch = 0;
  std::uint64_t successorContextEpoch = 0;
  std::string requestId;
  std::string parentCheckpointDigest;
  std::string prefixDigest;
  std::string providerStateDigest;
  std::string checkpointDigest;
  std::string wire;
  NativeJson transcript;
  // Local recovery metadata, validated against the authenticated receipt's
  // runtime hash chain; not a new checkpoint or transcript wire field.
  std::optional<std::size_t> nativeInitialPromptTokenCount;
};

struct NativeConversationRecord
{
  NativeConversationCheckpoint checkpoint;
  std::string serviceName;
  std::string planRoleMapDigest;
  std::string requestContractDigest;
  std::uint64_t retentionDeadlineMs = 0;
};

class NativeConversationCoordinator
{
public:
  explicit NativeConversationCoordinator(NativeConversationConfig config);
  explicit NativeConversationCoordinator(std::filesystem::path journalRoot);
  ~NativeConversationCoordinator();

  NativeConversationTurn beginTurn(const NativeConversationContinuation& continuation,
                                   std::string requestId,
                                   std::uint64_t attempt = 1) const;
  void abortTurn(const NativeConversationTurn& turn, const NativeDiError& error);
  void acceptTokenPrefix(const NativeConversationTurn& turn,
                         const std::vector<std::int64_t>& tokenIds);
  NativeConversationTurn replaceAttempt(const NativeConversationTurn& turn,
                                         std::string executionRequestId,
                                         std::string requestContractDigest = {});
  // Bind the replacement attempt to the role/provider map selected by the
  // planner. The parent checkpoint CAS remains bound to the map saved at
  // beginTurn, while the successor checkpoint carries this current map.
  NativeConversationTurn bindAttemptPlanRoleMap(
    const NativeConversationTurn& turn,
    const std::map<std::string, std::string>& providersByRole) const;
  NativeConversationCheckpoint prepareCheckpoint(
    const NativeConversationTurn& turn, const NativeCompletedAttempt& completed) const;
  NativeConversationRecord commitTurn(const NativeConversationTurn& turn,
                                      const NativeConversationCheckpoint& checkpoint);
  void restore(const std::filesystem::path& journalRoot);
  void restore();

  std::optional<NativeConversationRecord> find(const std::string& conversationId) const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_CONVERSATION_COORDINATOR_HPP
