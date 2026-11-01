#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ConversationStateBinding.hpp"
#include <ndn-cxx/data.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <optional>
#include <map>
#include <mutex>
#include <set>
#include <condition_variable>
#include <chrono>

namespace ndnsf::di {

struct NativeSelectionProjectionV3;

/** Provider-authored executable parent commitment; transport authenticates it. */
struct NativeConversationStateReadyV2
{
  std::string requestId;
  std::uint64_t attemptEpoch = 0;
  std::string generationId, planDigest;
  ConversationStateReferenceV1 parent;
  std::string providerIdentity, providerBootId;
  std::uint64_t cacheEpoch = 0;
  std::string reason, originProviderIdentity, protectedKvWireDigest, readyDigest;
};

/** Requester-authored all-role barrier release, never a new execution grant. */
struct NativeConversationRecoveryActivationV1
{
  std::string requestId;
  std::uint64_t attemptEpoch = 0;
  std::string generationId, planDigest, conversationId;
  std::uint64_t contextEpoch = 0;
  std::string serviceName, planRoleMapDigest, checkpointDigest, roleName;
  std::string readyDigest, readySetDigest;
  std::uint64_t expiresAtMs = 0;
};

/** Requester cancellation of one admitted recovery; transport authenticates it. */
struct NativeConversationRecoveryCancelV1
{
  std::string requestId;
  std::uint64_t attemptEpoch = 0;
  std::string generationId, planDigest;
  ConversationStateReferenceV1 parent;
  std::string providerIdentity, providerBootId;
};

/** One role's exact protected-KV publication challenge for a user Repo grant. */
struct NativeDurableStorageGrantRepoTargetV1
{
  std::string repoNode, providerIdentity, servicePrefix;
};

struct NativeDurableStorageGrantRoleChallengeV1
{
  std::string roleName;
  NativeDurableStorageGrantRepoTargetV1 repoTarget;
  std::string producerPrefix, writerIdentity, manifestName, publicationIdentity;
  std::string protectionEpoch, dataPrefix;
  std::uint64_t packetWireBytes = 0;
  std::vector<std::string> auxiliaryPacketNames;
};

struct NativeDurableStorageGrantChallengeV1
{
  std::string operationId, checkpointDigest, snapshotDigest, roleMapDigest;
  std::vector<NativeDurableStorageGrantRoleChallengeV1> roles;
};

struct NativeDurableStorageGrantRoleSubmissionV1
{
  std::string roleName, repoNode, ownerIdentity, writerIdentity, objectName, grantId;
  std::uint64_t ownerEpoch = 0;
};

struct NativeDurableStorageGrantSubmissionV1
{
  std::string operationId, checkpointDigest, snapshotDigest, roleMapDigest;
  std::vector<NativeDurableStorageGrantRoleSubmissionV1> grants;
};

// Commit is the Owner's permit for a Provider parked at the durable packet-set
// boundary; it is distinct from the earlier storage-grant submission.
enum class NativeConversationHandoffActionV1 { Begin, Poll, Grant, Commit, Cancel };

/** Owner control for one immutable, authenticated conversation handoff event.
 * Outer requester/provider/service/request identities come from Core context. */
struct NativeConversationHandoffControlV1
{
  NativeConversationHandoffActionV1 action = NativeConversationHandoffActionV1::Begin;
  std::string operationId, conversationId, checkpointDigest;
  std::string sourceRoleMapDigest, destinationRoleMapDigest;
  std::uint64_t deadlineMs = 0;
  std::optional<NativeDurableStorageGrantSubmissionV1> grantSubmission;
};

enum class NativeConversationHandoffStateV1
{
  Pending,
  GrantRequired,
  // Provider is parked at onDurablePacketSetBoundary until COMMIT or CANCEL.
  BoundaryReady,
  Saved,
  Cancelled,
  CancelClosed
};

/** Provider reply for one authenticated handoff event. `Saved` carries only
 * per-role references; the Owner must still fetch and validate the original
 * Repo packet sets before promoting the complete role set. */
struct NativeConversationHandoffReplyV1
{
  NativeConversationHandoffStateV1 state = NativeConversationHandoffStateV1::Pending;
  std::string operationId, conversationId, checkpointDigest;
  std::string sourceRoleMapDigest, destinationRoleMapDigest;
  std::uint64_t deadlineMs = 0;
  std::optional<NativeDurableStorageGrantChallengeV1> grantChallenge;
  std::map<std::string, NativeJson> ownerReferencesByRole;
};

/** Owner-to-source inquiry. Core authenticates the requester and service;
 * an inquiry alone never authorizes or starts a KV export. */
struct NativeWithdrawalQueryV1
{
  std::string conversationId, checkpointDigest;
  std::uint64_t deadlineMs = 0;
};

/** Owner settlement after complete Repo proof and journal promotion.
 * Core authenticates the Owner; the Provider additionally matches its local
 * withdrawal ID, registration generation, checkpoint and saved operation. */
struct NativeWithdrawalConfirmV1
{
  std::string withdrawalId, conversationId, checkpointDigest, operationId;
  std::uint64_t registrationGeneration = 0;
  std::string completeRoleSetDigest, journalDigest;
  std::uint64_t deadlineMs = 0;
};

struct NativeWithdrawalConfirmReplyV1
{
  NativeWithdrawalConfirmV1 confirm;
};

enum class NativeWithdrawalScopeV1 { Service, Provider };

/** Provider-authored local withdrawal binding for one authenticated Owner's
 * committed conversation. The separately signed original Data wire carries
 * this exact canonical payload to other source Providers. */
struct NativeWithdrawalIntentV1
{
  std::string withdrawalId, ownerIdentity, serviceName, providerBootId;
  std::uint64_t registrationGeneration = 0;
  NativeWithdrawalScopeV1 scope = NativeWithdrawalScopeV1::Service;
  std::uint64_t deadlineMs = 0;
  std::string conversationId, checkpointDigest, sourceRoleMapDigest;
  std::vector<std::string> localRoles;
  std::string policyDigest;
};

enum class NativeWithdrawalQueryStatusV1 { NoIntent, Busy, Intent };

/** Response to one authenticated Owner query. A positive response carries
 * the Provider's original signed intent Data, unchanged. Parsing this DTO
 * does not verify that signature or authorize a save. */
struct NativeWithdrawalQueryReplyV1
{
  NativeWithdrawalQueryStatusV1 status = NativeWithdrawalQueryStatusV1::NoIntent;
  NativeWithdrawalQueryV1 query;
  std::optional<NativeWithdrawalIntentV1> intent;
  ndn::Buffer originalProofWire;
};

enum class NativeConversationSaveTriggerV2 { UserSwitch, WithdrawalSave };

/** Versioned Owner control binding for switch or voluntary withdrawal save.
 * Parsing preserves the original proof bytes but does not authorize them:
 * the Provider must validate the signed Producer Data before export. */
struct NativeConversationHandoffControlV2
{
  NativeConversationHandoffControlV1 control;
  std::map<std::string, std::string> sourceProvidersByRole;
  NativeConversationSaveTriggerV2 trigger = NativeConversationSaveTriggerV2::UserSwitch;
  std::string withdrawingProviderIdentity;
  std::optional<NativeWithdrawalIntentV1> withdrawalIntent;
  ndn::Buffer withdrawalProofWire;
};

/** Owner-side linearization gate for one handoff spanning source Providers.
 * Cancellation remains open until every Provider is parked at its packet-set
 * boundary, then `releaseCommit` closes it atomically before COMMIT fanout. */
class NativeConversationHandoffBoundaryGateV1
{
public:
  enum class State { Collecting, CommitReleased, Cancelled };

  explicit NativeConversationHandoffBoundaryGateV1(
    const std::vector<std::string>& sourceProviders,
    std::chrono::steady_clock::time_point deadline);

  // Returns false after cancellation. Repeated reports from a known Provider
  // are idempotent, including after the Owner releases COMMIT.
  bool reportBoundaryReady(const std::string& providerIdentity);
  // Returns false if the all-Provider barrier already released COMMIT.
  bool cancel();
  // Succeeds only after all expected source Providers are boundary-ready.
  bool releaseCommit();
  State state() const;

private:
  mutable std::mutex m_mutex;
  std::set<std::string> m_expectedProviders;
  std::set<std::string> m_boundaryReadyProviders;
  std::chrono::steady_clock::time_point m_deadline;
  State m_state = State::Collecting;
};

/** Provider-local role barrier. Each publisher callback parks at its own
 * packet-set boundary; BOUNDARY_READY is exposed only after every role parks. */
class NativeConversationHandoffRoleBoundaryGateV1
{
public:
  enum class State { Collecting, CommitReleased, Cancelled };

  explicit NativeConversationHandoffRoleBoundaryGateV1(
    const std::vector<std::string>& roles,
    std::chrono::steady_clock::time_point deadline);

  // Called by each role's onDurablePacketSetBoundary callback. It waits for
  // the Owner's permit and returns false on CANCEL or the immutable deadline.
  bool parkRoleAndWait(const std::string& role);
  bool boundaryReady() const;
  std::size_t readyRoleCount() const;
  bool cancel();
  bool releaseCommit();
  State state() const;

private:
  mutable std::mutex m_mutex;
  std::condition_variable m_changed;
  std::set<std::string> m_expectedRoles;
  std::set<std::string> m_readyRoles;
  std::chrono::steady_clock::time_point m_deadline;
  State m_state = State::Collecting;
};

NativeJson nativeDurableStorageGrantChallengeV1ToJson(
  const NativeDurableStorageGrantChallengeV1& challenge);
NativeDurableStorageGrantChallengeV1 nativeDurableStorageGrantChallengeV1FromJson(
  const std::string& wire);
NativeJson nativeDurableStorageGrantSubmissionV1ToJson(
  const NativeDurableStorageGrantSubmissionV1& submission);
NativeDurableStorageGrantSubmissionV1 nativeDurableStorageGrantSubmissionV1FromJson(
  const std::string& wire);
void nativeValidateDurableStorageGrantSubmissionV1(
  const NativeDurableStorageGrantChallengeV1& challenge,
  const NativeDurableStorageGrantSubmissionV1& submission,
  const std::string& authenticatedOwnerIdentity);
NativeJson nativeConversationHandoffControlV1ToJson(
  const NativeConversationHandoffControlV1& control);
NativeConversationHandoffControlV1 nativeConversationHandoffControlV1FromJson(
  const std::string& wire);
NativeJson nativeConversationHandoffReplyV1ToJson(
  const NativeConversationHandoffReplyV1& reply);
NativeConversationHandoffReplyV1 nativeConversationHandoffReplyV1FromJson(
  const std::string& wire);
void nativeValidateConversationHandoffReplyBindingV1(
  const NativeConversationHandoffControlV1& control,
  const NativeConversationHandoffReplyV1& reply);
NativeJson nativeWithdrawalQueryV1ToJson(const NativeWithdrawalQueryV1& query);
NativeWithdrawalQueryV1 nativeWithdrawalQueryV1FromJson(const std::string& wire);
NativeJson nativeWithdrawalConfirmV1ToJson(const NativeWithdrawalConfirmV1& confirm);
NativeWithdrawalConfirmV1 nativeWithdrawalConfirmV1FromJson(const std::string& wire);
NativeJson nativeWithdrawalConfirmReplyV1ToJson(
  const NativeWithdrawalConfirmReplyV1& reply);
NativeWithdrawalConfirmReplyV1 nativeWithdrawalConfirmReplyV1FromJson(
  const std::string& wire);
NativeJson nativeWithdrawalIntentV1ToJson(const NativeWithdrawalIntentV1& intent);
NativeWithdrawalIntentV1 nativeWithdrawalIntentV1FromJson(const std::string& wire);
NativeJson nativeWithdrawalQueryReplyV1ToJson(
  const NativeWithdrawalQueryReplyV1& reply);
NativeWithdrawalQueryReplyV1 nativeWithdrawalQueryReplyV1FromJson(
  const std::string& wire);
ndn::Name nativeWithdrawalIntentProofNameV1(
  const ndn::Name& providerIdentity, const NativeWithdrawalIntentV1& intent);
// The signer must use the actual Provider certificate. No signer or verifier
// fallback exists; callers supply the configured producer trust decision.
ndn::Data nativeSignWithdrawalIntentProofV1(
  const ndn::Name& providerIdentity, const NativeWithdrawalIntentV1& intent,
  const std::function<void(ndn::Data&)>& signProviderData);
void nativeValidateWithdrawalIntentProofV1(
  const ndn::Buffer& originalWire, const ndn::Name& expectedProvider,
  const NativeWithdrawalIntentV1& expectedIntent, std::uint64_t nowMs,
  const std::function<bool(const ndn::Data&, const ndn::Name&)>& verifyProducer);
NativeJson nativeConversationHandoffControlV2ToJson(
  const NativeConversationHandoffControlV2& control);
NativeConversationHandoffControlV2 nativeConversationHandoffControlV2FromJson(
  const std::string& wire);

NativeJson nativeConversationRecoveryCancelV1ToJson(const NativeConversationRecoveryCancelV1& cancel);
NativeConversationRecoveryCancelV1 nativeConversationRecoveryCancelV1FromJson(const std::string& wire);
void nativeValidateConversationRecoveryCancelV1(
  const NativeConversationRecoveryCancelV1& cancel, const NativeSelectionProjectionV3& projection,
  const std::string& localBootId, std::uint64_t nowMs);

NativeJson nativeConversationStateReadyV2ToJson(const NativeConversationStateReadyV2& ready);
NativeConversationStateReadyV2 nativeConversationStateReadyV2FromJson(const std::string& wire);
NativeJson nativeConversationRecoveryActivationV1ToJson(
  const NativeConversationRecoveryActivationV1& activation);
NativeConversationRecoveryActivationV1 nativeConversationRecoveryActivationV1FromJson(
  const std::string& wire);
std::string nativeConversationRecoveryReadySetDigest(
  const std::map<std::string, NativeConversationStateReadyV2>& readyByRole);
void nativeValidateConversationStateReadyV2(
  const NativeConversationStateReadyV2& ready,
  const NativeSelectionProjectionV3& projection, const std::string& admittedBootEpoch,
  const std::string& actualProducer, const std::string& actualProducerRole,
  std::uint64_t nowMs);
void nativeValidateConversationRecoveryActivationV1(
  const NativeConversationRecoveryActivationV1& activation,
  const NativeConversationStateReadyV2& ownReady, std::uint64_t nowMs);

struct NativeConversationJournalKey
{
  std::string id;
  std::vector<std::uint8_t> bytes;
};

std::vector<std::vector<std::uint8_t>> nativeConversationAuthenticationKeys(
  const std::string& identity, const std::vector<NativeConversationJournalKey>& keys);
std::string nativeReadConversationEnvelope(
  const std::string& encoded, const std::string& envelopeId,
  const std::vector<NativeConversationJournalKey>& keys, std::uint64_t nowMs);
std::string nativeSealConversationEnvelope(
  const std::string& plaintext, const std::string& envelopeId,
  std::uint64_t expiresAtMs, const NativeConversationJournalKey& key);
std::string nativeConversationBase64Encode(const std::string& bytes);
std::string nativeConversationBase64Decode(const std::string& encoded);
// Conversation V1 follows the Python reference's ensure_ascii=False wire;
// this is intentionally separate from the framework canonical JSON contract.
std::string nativeConversationCanonicalJson(const NativeJson& value);

// Parse metadata from an already authenticated transport or owner checkpoint.
// This does not authenticate an original Provider Data packet.
ProviderConversationStateReceiptV1 nativeReadProviderConversationStateReceipt(const NativeJson& value);

// Existing conversation.py V1 wires. These helpers own validation and
// authentication only; the coordinator owns turns and durable promotion.
std::string nativeConversationPrefixDigest(const std::vector<std::int64_t>& tokens);
std::string nativeSignConversationCheckpoint(
  NativeJson unsignedCheckpoint, const std::vector<std::uint8_t>& authenticationKey);
NativeJson nativeReadConversationCheckpoint(
  const std::string& wire,
  const std::vector<std::vector<std::uint8_t>>& verificationKeys,
  std::uint64_t nowMs);

// Call only for decrypted journal payloads and an authenticated checkpoint.
// Never expose transcript contents to observers or network edges.
void nativeValidateConversationTranscript(
  const NativeJson& transcript, const NativeJson& checkpoint,
  std::optional<std::size_t> nativeInitialPromptTokenCount = std::nullopt);

} // namespace ndnsf::di
