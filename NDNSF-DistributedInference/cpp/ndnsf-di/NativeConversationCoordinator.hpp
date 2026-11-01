#ifndef NDNSF_DI_NATIVE_CONVERSATION_COORDINATOR_HPP
#define NDNSF_DI_NATIVE_CONVERSATION_COORDINATOR_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationJournal.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationContinuation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantTypes.hpp"
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
class NativeProtectedKvPublicationV1;
class NativeVerifiedRemoteKvPublicationV1;

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
  // Optional owner-side observation after the durable publish linearization
  // point and before best-effort Provider FINALIZE.  It is invoked without
  // the coordinator lock and never participates in the commit decision.
  std::function<void()> afterDurableCommit;
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
  // Owner-restored placement; not accepted from an external continuation hint.
  std::map<std::string, std::string> providersByRole;
  // Process-local authorization only; never journaled or restored.
  std::optional<NativeGrantLease> grantLease;
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
  std::string modelContractDigest;
  std::string providerStateDigest;
  std::string checkpointDigest;
  std::string wire;
  NativeJson transcript;
  std::vector<std::string> expectedRoles;
  // Local recovery metadata, validated against the authenticated receipt's
  // runtime hash chain; not a new checkpoint or transcript wire field.
  std::optional<std::size_t> nativeInitialPromptTokenCount;
  // Local journal metadata bound to the checkpoint's planRoleMapDigest.
  std::map<std::string, std::string> providersByRole;
};

struct NativeConversationRecord
{
  NativeConversationCheckpoint checkpoint;
  std::string serviceName;
  std::string planRoleMapDigest;
  // Owner-selected next-turn placement. Kept separate from the historical
  // checkpoint/source map and its signed digest.
  std::map<std::string, std::string> nextTurnProvidersByRole;
  std::string requestContractDigest;
  std::uint64_t retentionDeadlineMs = 0;
  // Durable byte references only. Availability and execution authority are checked afresh.
  std::optional<NativeJson> protectedKvReferences;
};

// One owned historical parent snapshot, never a new storage or execution grant.
struct NativeConversationKvRecoveryV1
{
  NativeJson references;
  NativeJson checkpoint;
  // Original decoded transcript bytes; do not reconstruct or re-sign receipts.
  std::map<std::string, std::string> originReceipts;

  bool operator==(const NativeConversationKvRecoveryV1& other) const
  {
    return references == other.references && checkpoint == other.checkpoint &&
           originReceipts == other.originReceipts;
  }
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
  // Bind the first FULL_CONTEXT turn after native placement has selected its
  // providers. Dynamic placement is only known after ACK closure; the
  // coordinator owns the resulting role-map digest.
  NativeConversationTurn bindInitialPlanRoleMap(
    const NativeConversationTurn& turn,
    const std::map<std::string, std::string>& providersByRole) const;
  // Bind the replacement attempt to the role/provider map selected by the
  // planner. The parent checkpoint CAS remains bound to the map saved at
  // beginTurn, while the successor checkpoint carries this current map.
  NativeConversationTurn bindAttemptPlanRoleMap(
    const NativeConversationTurn& turn,
    const std::map<std::string, std::string>& providersByRole) const;
  // A fresh turn may select a new map only against this exact saved parent.
  // The historical checkpoint/receipt map remains the parent CAS identity.
  NativeConversationTurn bindRecoveryPlanRoleMap(
    const NativeConversationTurn& turn,
    const std::map<std::string, std::string>& providersByRole,
    const NativeConversationKvRecoveryV1& expectedRecovery) const;
  void bindGrantLease(const NativeConversationTurn& turn,
                      const NativeGrantLease& lease) const;
  NativeConversationCheckpoint prepareCheckpoint(
    const NativeConversationTurn& turn, const NativeCompletedAttempt& completed) const;
  NativeConversationRecord commitTurn(const NativeConversationTurn& turn,
                                      const NativeConversationCheckpoint& checkpoint);
  NativeConversationRecord promoteProtectedKvSet(const std::string& conversationId,
    const std::string& expectedCheckpointDigest,
    const std::map<std::string, NativeProtectedKvPublicationV1>& publications);
  // Current pending owner/CAS lookup, not a new grant or Repo availability proof.
  std::optional<NativeJson> protectedKvForTurn(const NativeConversationTurn& turn) const;
  std::optional<NativeConversationKvRecoveryV1> protectedKvRecoveryForTurn(
    const NativeConversationTurn& turn) const;
  void restore(const std::filesystem::path& journalRoot);
  void restore();

  std::optional<NativeConversationRecord> find(const std::string& conversationId) const;

private:
  friend class NativeRemoteProtectedKvSetPromoter;

  // Remote proofs are promoted only inside the Owner operation that fetched
  // them and rechecked every Repo claim; callers cannot retain a proof and
  // submit it after its storage claim has changed.
  NativeConversationRecord promoteProtectedKvSet(const std::string& conversationId,
    const std::string& expectedCheckpointDigest,
    const std::map<std::string, NativeVerifiedRemoteKvPublicationV1>& publications,
    const std::map<std::string, std::string>& nextTurnProvidersByRole,
    const std::function<void()>& requireActive);

  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// Parse the operator-owned native conversation configuration and construct
// the single coordinator used by C++ requesters and optional bindings.
// ``expectedRequesterIdentity`` is checked when non-empty so an entry point
// cannot accidentally attach a journal to another ServiceUser.
std::shared_ptr<NativeConversationCoordinator> nativeConversationCoordinatorFromConfig(
  const std::string& configurationJson,
  const std::filesystem::path& baseDirectory,
  const std::string& expectedRequesterIdentity = {});

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_CONVERSATION_COORDINATOR_HPP
