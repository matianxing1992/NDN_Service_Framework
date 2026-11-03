#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV4.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/portable-kv-checkpoint.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-publication.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-save.hpp"
#include "ndn-service-framework/NetworkProbeCachePolicy.hpp"
#include "ndn-service-framework/OperationRuntime.hpp"

#include <ndn-cxx/name.hpp>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <thread>
#include <utility>

#include <unistd.h>

namespace ndnsf::di {

using ServiceProvider = ndn_service_framework::ServiceProvider;

class NativeWithdrawalWireBudget
{
public:
  explicit NativeWithdrawalWireBudget(std::uint64_t maximum)
    : m_maximum(maximum)
  {
  }

  void reserve(const ndn::Name& owner,
               const NativeDurableStorageGrantRoleChallengeV1& challenge)
  {
    if (owner.empty() || challenge.roleName.empty() ||
        challenge.repoTarget.repoNode.empty() || challenge.manifestName.empty() ||
        challenge.packetWireBytes == 0)
      throw std::runtime_error("WITHDRAWAL_WIRE_CHALLENGE_INVALID");
    const auto key = std::make_tuple(owner.toUri(ndn::name::UriFormat::ALTERNATE),
      challenge.repoTarget.repoNode, challenge.manifestName);
    Reservation reservation;
    reservation.writer = challenge.writerIdentity;
    reservation.publicationIdentity = challenge.publicationIdentity;
    reservation.protectionEpoch = challenge.protectionEpoch;
    reservation.dataPrefix = challenge.dataPrefix;
    reservation.repoProvider = challenge.repoTarget.providerIdentity;
    reservation.repoService = challenge.repoTarget.servicePrefix;
    reservation.wireBytes = challenge.packetWireBytes;
    reservation.auxiliaryNames = challenge.auxiliaryPacketNames;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_reservations.find(key);
    if (found != m_reservations.end()) {
      if (found->second != reservation)
        throw std::runtime_error("WITHDRAWAL_WIRE_CHALLENGE_CONFLICT");
      return;
    }
    if (reservation.wireBytes > m_maximum - m_reservedBytes)
      throw std::runtime_error("BUDGET_EXCEEDED");
    m_reservedBytes += reservation.wireBytes;
    m_reservations.emplace(key, std::move(reservation));
  }

private:
  struct Reservation
  {
    std::string writer, publicationIdentity, protectionEpoch, dataPrefix;
    std::string repoProvider, repoService;
    std::uint64_t wireBytes = 0;
    std::vector<std::string> auxiliaryNames;
    bool operator!=(const Reservation& other) const
    {
      return writer != other.writer || publicationIdentity != other.publicationIdentity ||
        protectionEpoch != other.protectionEpoch || dataPrefix != other.dataPrefix ||
        repoProvider != other.repoProvider || repoService != other.repoService ||
        wireBytes != other.wireBytes || auxiliaryNames != other.auxiliaryNames;
    }
  };
  const std::uint64_t m_maximum;
  std::mutex m_mutex;
  std::uint64_t m_reservedBytes = 0;
  std::map<std::tuple<std::string, std::string, std::string>, Reservation> m_reservations;
};

namespace {

using HandoffClock = std::chrono::steady_clock;
using HandoffOperation = NativeConversationHandoffControlV1;
using HandoffReply = NativeConversationHandoffReplyV1;

std::string
classifiedHandoffFailure(std::string reason)
{
  if (reason == "protected KV publication unavailable: BUDGET_EXCEEDED")
    return "BUDGET_EXCEEDED";
  return reason;
}

struct NativeProviderHandoffAdmissionState
{
  std::mutex mutex;
  std::size_t activeEvents = 0;
  std::size_t activeRoleSlots = 0;
  std::uint64_t reservedScratchBytes = 0;
  std::vector<std::shared_ptr<ndn_service_framework::OperationRuntime>> coordinatorRuntimes;
  std::set<ndn_service_framework::OperationRuntime*> busyCoordinatorRuntimes;
  std::size_t effectiveMaxConcurrentEvents = std::numeric_limits<std::size_t>::max();
  std::size_t effectiveMaxConcurrentRoleSlots = std::numeric_limits<std::size_t>::max();
  std::uint64_t effectiveScratchQuotaBytes = std::numeric_limits<std::uint64_t>::max();
};

struct NativeProviderHandoffAdmissionLease
{
  std::shared_ptr<NativeProviderHandoffAdmissionState> state;
  std::shared_ptr<ndn_service_framework::OperationRuntime> coordinatorRuntime;
  std::size_t roleSlots = 0;
  std::uint64_t scratchBytes = 0;

  ~NativeProviderHandoffAdmissionLease()
  {
    if (!state)
      return;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->busyCoordinatorRuntimes.erase(coordinatorRuntime.get());
    --state->activeEvents;
    state->activeRoleSlots -= roleSlots;
    state->reservedScratchBytes -= scratchBytes;
    if (state->activeEvents == 0) {
      state->effectiveMaxConcurrentEvents = std::numeric_limits<std::size_t>::max();
      state->effectiveMaxConcurrentRoleSlots = std::numeric_limits<std::size_t>::max();
      state->effectiveScratchQuotaBytes = std::numeric_limits<std::uint64_t>::max();
    }
  }
};

std::uint64_t
handoffNowMs()
{
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string
handoffCanonicalName(const ndn::Name& name)
{
  return name.toUri(ndn::name::UriFormat::ALTERNATE);
}

HandoffClock::time_point
handoffDeadline(std::uint64_t deadlineMs)
{
  const auto now = handoffNowMs();
  constexpr auto maxDurationMs = std::uint64_t{10 * 60 * 1000};
  if (deadlineMs <= now || deadlineMs - now > maxDurationMs)
    throw std::invalid_argument("HANDOFF_DEADLINE_INVALID");
  return HandoffClock::now() + std::chrono::milliseconds(deadlineMs - now);
}

bool
sameHandoffBinding(const HandoffOperation& a, const HandoffOperation& b)
{
  return a.operationId == b.operationId && a.conversationId == b.conversationId &&
    a.checkpointDigest == b.checkpointDigest &&
    a.sourceRoleMapDigest == b.sourceRoleMapDigest &&
    a.destinationRoleMapDigest == b.destinationRoleMapDigest &&
    a.deadlineMs == b.deadlineMs;
}

struct NativeProviderHandoffOperation
{
  NativeProviderHandoffOperation(HandoffOperation control_, ndn::Name requester_,
    ndn::Name provider_, ndn::Name service_, std::string v2ContextDigest_,
    std::map<std::string, std::string> sourceProvidersByRole_,
    std::optional<NativeConversationHandoffControlV2> withdrawalControl_)
    : control(std::move(control_)), requester(std::move(requester_)),
      provider(std::move(provider_)), service(std::move(service_)),
      deadline(handoffDeadline(control.deadlineMs)),
      v2ContextDigest(std::move(v2ContextDigest_)),
      sourceProvidersByRole(std::move(sourceProvidersByRole_)),
      withdrawalControl(std::move(withdrawalControl_))
  {}

  void requireCurrent() const
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (publicationError)
      std::rethrow_exception(publicationError);
    if (!failure.empty())
      throw std::runtime_error(failure);
    if (cancelled)
      throw std::runtime_error("HANDOFF_CANCELLED");
    if (HandoffClock::now() >= deadline)
      throw std::runtime_error("HANDOFF_DEADLINE_EXPIRED");
  }

  void setOwnerContext(NativeConversationHandoffOwnerContextV1 value)
  {
    std::lock_guard<std::mutex> lock(mutex);
    ownerContext.emplace(std::move(value));
    initialized = true;
    condition.notify_all();
  }

  void setRoles(std::vector<std::string> value)
  {
    std::lock_guard<std::mutex> lock(mutex);
    roles = std::move(value);
    condition.notify_all();
  }

  void setGrantBarrier(std::shared_ptr<NativeDurableStorageGrantBarrierV1> value)
  {
    std::lock_guard<std::mutex> lock(mutex);
    grantBarrier = std::move(value);
    condition.notify_all();
  }

  void recordChallenge(const NativeDurableStorageGrantRoleChallengeV1& roleChallenge)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!failure.empty() || cancelled || HandoffClock::now() >= deadline)
      throw std::runtime_error("HANDOFF_NOT_ACTIVE");
    if (std::find(roles.begin(), roles.end(), roleChallenge.roleName) == roles.end())
      throw std::runtime_error("HANDOFF_GRANT_UNKNOWN_ROLE");
    const auto [it, inserted] = roleChallenges.emplace(roleChallenge.roleName, roleChallenge);
    if (!inserted) {
      const auto& prior = it->second;
      if (prior.repoTarget.repoNode != roleChallenge.repoTarget.repoNode ||
          prior.repoTarget.providerIdentity != roleChallenge.repoTarget.providerIdentity ||
          prior.repoTarget.servicePrefix != roleChallenge.repoTarget.servicePrefix ||
          prior.producerPrefix != roleChallenge.producerPrefix ||
          prior.writerIdentity != roleChallenge.writerIdentity ||
          prior.manifestName != roleChallenge.manifestName ||
          prior.publicationIdentity != roleChallenge.publicationIdentity ||
          prior.protectionEpoch != roleChallenge.protectionEpoch ||
          prior.dataPrefix != roleChallenge.dataPrefix ||
          prior.packetWireBytes != roleChallenge.packetWireBytes ||
          prior.auxiliaryPacketNames != roleChallenge.auxiliaryPacketNames)
        throw std::runtime_error("HANDOFF_GRANT_CHALLENGE_CONFLICT");
    }
    condition.notify_all();
  }

  void setChallenge(NativeDurableStorageGrantChallengeV1 value)
  {
    std::lock_guard<std::mutex> lock(mutex);
    challenge.emplace(std::move(value));
    condition.notify_all();
  }

  void applyGrant(const NativeDurableStorageGrantSubmissionV1& submission)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (publicationError)
      std::rethrow_exception(publicationError);
    if (!failure.empty())
      throw std::runtime_error(failure);
    if (!challenge)
      throw std::runtime_error("HANDOFF_GRANT_NOT_EXPECTED");
    if (submission.operationId != control.operationId ||
        submission.checkpointDigest != control.checkpointDigest ||
        submission.snapshotDigest != challenge->snapshotDigest ||
        submission.roleMapDigest != control.sourceRoleMapDigest)
      throw std::runtime_error("HANDOFF_GRANT_BINDING_MISMATCH");
    const auto digest = nativePlanningDigest(nativeCanonicalJson(
      nativeDurableStorageGrantSubmissionV1ToJson(submission)));
    if (grantsReleased) {
      if (digest != grantSubmissionDigest)
        throw std::runtime_error("HANDOFF_GRANT_CONFLICT");
      return;
    }
    if (cancelled || commitReleased || !grantBarrier || !ownerContext)
      throw std::runtime_error("HANDOFF_GRANT_NOT_EXPECTED");
    const auto& context = *ownerContext;
    NativeDurableStorageGrantBarrierV1::GrantSet grants;
    for (const auto& grant : submission.grants) {
      const auto roleChallenge = roleChallenges.find(grant.roleName);
      const auto route = context.repoTargetByRole.find(grant.roleName);
      if (roleChallenge == roleChallenges.end() || route == context.repoTargetByRole.end() ||
          grant.repoNode != route->second.repoNode ||
          grant.ownerIdentity != requester.toUri(ndn::name::UriFormat::ALTERNATE) ||
          grant.writerIdentity != roleChallenge->second.writerIdentity ||
          grant.objectName != roleChallenge->second.manifestName ||
          grant.grantId.empty() || grant.grantId.size() > 4096 || grant.ownerEpoch == 0 ||
          !grants.emplace(grant.roleName, NativeDurableStorageRoleGrantV1{
            grant.repoNode, grant.ownerIdentity, grant.grantId, grant.writerIdentity,
            grant.objectName, grant.ownerEpoch}).second)
        throw std::runtime_error("HANDOFF_GRANT_ROLE_BINDING_MISMATCH");
    }
    if (grants.size() != roles.size() || roleChallenges.size() != roles.size())
      throw std::runtime_error("HANDOFF_GRANT_ROLE_SET_MISMATCH");
    grantSubmissionDigest = digest;
    grantBarrier->releaseAll(std::move(grants));
    grantsReleased = true;
    condition.notify_all();
  }

  HandoffReply applyCommit()
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (publicationError)
      std::rethrow_exception(publicationError);
    if (!failure.empty())
      throw std::runtime_error(failure);
    if (cancelled)
      throw std::runtime_error("HANDOFF_CANCELLED");
    if (commitReleased)
      return makeReplyLocked(NativeConversationHandoffActionV1::Commit);
    if (!initialized || !grantsReleased || roles.empty() ||
        parkedRoles.size() != roles.size())
      throw std::runtime_error("HANDOFF_COMMIT_BEFORE_BOUNDARY_READY");
    if (HandoffClock::now() >= deadline)
      throw std::runtime_error("HANDOFF_DEADLINE_EXPIRED");
    if (onCommitReleased)
      onCommitReleased(control);
    commitReleased = true;
    condition.notify_all();
    return makeReplyLocked(NativeConversationHandoffActionV1::Commit);
  }

  HandoffReply cancel()
  {
    std::shared_ptr<NativeDurableStorageGrantBarrierV1> barrier;
    HandoffReply reply;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (commitReleased) {
        reply = makeReplyLocked(NativeConversationHandoffActionV1::Cancel);
        reply.state = NativeConversationHandoffStateV1::CancelClosed;
        return reply;
      }
      cancelled = true;
      barrier = grantBarrier;
      condition.notify_all();
      reply = makeReplyLocked(NativeConversationHandoffActionV1::Cancel);
      reply.state = NativeConversationHandoffStateV1::Cancelled;
    }
    if (barrier)
      barrier->abort(std::make_exception_ptr(std::runtime_error("HANDOFF_CANCELLED")));
    return reply;
  }

  void parkRole(const std::string& role)
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (std::find(roles.begin(), roles.end(), role) == roles.end())
      throw std::runtime_error("HANDOFF_BOUNDARY_UNKNOWN_ROLE");
    if (publicationError)
      std::rethrow_exception(publicationError);
    if (!failure.empty() || cancelled || HandoffClock::now() >= deadline)
      throw std::runtime_error("HANDOFF_BOUNDARY_NOT_ACTIVE");
    parkedRoles.insert(role);
    condition.notify_all();
    if (!condition.wait_until(lock, deadline, [this] {
          return commitReleased || cancelled || !failure.empty() || publicationError;
        }))
      throw std::runtime_error("HANDOFF_BOUNDARY_TIMEOUT");
    if (publicationError)
      std::rethrow_exception(publicationError);
    if (!failure.empty())
      throw std::runtime_error(failure);
    if (cancelled)
      throw std::runtime_error("HANDOFF_CANCELLED");
    if (!commitReleased)
      throw std::runtime_error("HANDOFF_COMMIT_REQUIRED");
  }

  void awaitInitialization()
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!condition.wait_until(lock, deadline, [this] {
          return initialized || cancelled || !failure.empty();
        }))
      throw std::runtime_error("HANDOFF_INITIALIZATION_TIMEOUT");
    if (!failure.empty())
      throw std::runtime_error(failure);
    if (cancelled)
      throw std::runtime_error("HANDOFF_CANCELLED");
  }

  void awaitAllChallenges()
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!condition.wait_until(lock, deadline, [this] {
          return roleChallenges.size() == roles.size() || cancelled || !failure.empty() ||
            publicationError;
        }))
      throw std::runtime_error("HANDOFF_GRANT_CHALLENGE_TIMEOUT");
    if (publicationError)
      std::rethrow_exception(publicationError);
    if (!failure.empty())
      throw std::runtime_error(failure);
    if (cancelled)
      throw std::runtime_error("HANDOFF_CANCELLED");
  }

  void awaitCommit()
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!condition.wait_until(lock, deadline, [this] {
          return commitReleased || cancelled || !failure.empty() || publicationError;
        }))
      throw std::runtime_error("HANDOFF_COMMIT_TIMEOUT");
    if (publicationError)
      std::rethrow_exception(publicationError);
    if (!failure.empty())
      throw std::runtime_error(failure);
    if (cancelled)
      throw std::runtime_error("HANDOFF_CANCELLED");
  }

  HandoffReply replyFor(NativeConversationHandoffActionV1 action) const
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!failure.empty())
      throw std::runtime_error(failure);
    if (HandoffClock::now() >= deadline && !saved)
      throw std::runtime_error("HANDOFF_DEADLINE_EXPIRED");
    return makeReplyLocked(action);
  }

  void finishSaved(std::map<std::string, NativeJson> references)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!commitReleased || HandoffClock::now() >= deadline ||
        references.size() != roles.size())
      throw std::runtime_error("HANDOFF_SAVED_ROLE_SET_MISMATCH");
    if (onSaved)
      onSaved(control);
    savedReferences = std::move(references);
    saved = true;
    done = true;
    ownerContext.reset();
    grantBarrier.reset();
    condition.notify_all();
  }

  void releaseOwnerContext()
  {
    std::lock_guard<std::mutex> lock(mutex);
    ownerContext.reset();
    grantBarrier.reset();
  }

  void fail(std::string reason)
  {
    std::shared_ptr<NativeDurableStorageGrantBarrierV1> barrier;
    std::function<void(const HandoffOperation&, const std::string&)> failedObserver;
    std::string failedReason;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (done || saved)
        return;
      failure = reason.empty() ? "HANDOFF_FAILED"
        : classifiedHandoffFailure(std::move(reason));
      failedReason = failure;
      failedObserver = onFailed;
      if (!commitReleased)
        cancelled = true;
      barrier = grantBarrier;
      done = true;
      condition.notify_all();
    }
    if (barrier)
      barrier->abort(std::make_exception_ptr(std::runtime_error("HANDOFF_FAILED")));
    if (failedObserver) {
      try { failedObserver(control, failedReason); }
      catch (...) {}
    }
  }

  void signalPublicationError(std::exception_ptr error)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!publicationError)
      publicationError = std::move(error);
    condition.notify_all();
  }

  HandoffReply makeReplyLocked(NativeConversationHandoffActionV1 action) const
  {
    HandoffReply reply;
    reply.operationId = control.operationId;
    reply.conversationId = control.conversationId;
    reply.checkpointDigest = control.checkpointDigest;
    reply.sourceRoleMapDigest = control.sourceRoleMapDigest;
    reply.destinationRoleMapDigest = control.destinationRoleMapDigest;
    reply.deadlineMs = control.deadlineMs;
    if (action == NativeConversationHandoffActionV1::Cancel) {
      reply.state = cancelled ? NativeConversationHandoffStateV1::Cancelled
                              : NativeConversationHandoffStateV1::CancelClosed;
      return reply;
    }
    if (action != NativeConversationHandoffActionV1::Poll) {
      reply.state = NativeConversationHandoffStateV1::Pending;
      return reply;
    }
    if (saved) {
      reply.state = NativeConversationHandoffStateV1::Saved;
      reply.ownerReferencesByRole = savedReferences;
    }
    else if (cancelled) {
      reply.state = NativeConversationHandoffStateV1::Cancelled;
    }
    else if (!commitReleased && !grantsReleased && challenge) {
      reply.state = NativeConversationHandoffStateV1::GrantRequired;
      reply.grantChallenge = challenge;
    }
    else if (!commitReleased && grantsReleased && !roles.empty() &&
             parkedRoles.size() == roles.size()) {
      reply.state = NativeConversationHandoffStateV1::BoundaryReady;
    }
    else {
      reply.state = NativeConversationHandoffStateV1::Pending;
    }
    return reply;
  }

  const HandoffOperation control;
  const ndn::Name requester;
  const ndn::Name provider;
  const ndn::Name service;
  const HandoffClock::time_point deadline;
  const std::string v2ContextDigest;
  const std::map<std::string, std::string> sourceProvidersByRole;
  const std::optional<NativeConversationHandoffControlV2> withdrawalControl;
  std::string localWithdrawalId;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::optional<NativeConversationHandoffOwnerContextV1> ownerContext;
  std::shared_ptr<NativeDurableStorageGrantBarrierV1> grantBarrier;
  std::exception_ptr publicationError;
  std::vector<std::string> roles;
  std::map<std::string, NativeDurableStorageGrantRoleChallengeV1> roleChallenges;
  std::optional<NativeDurableStorageGrantChallengeV1> challenge;
  std::map<std::string, NativeJson> savedReferences;
  std::set<std::string> parkedRoles;
  std::string grantSubmissionDigest;
  std::string failure;
  bool initialized = false;
  bool grantsReleased = false;
  bool commitReleased = false;
  bool cancelled = false;
  bool saved = false;
  bool done = false;
  bool workerFinished = false;
  std::shared_ptr<NativeProviderHandoffAdmissionLease> admission;
  std::function<void(const HandoffOperation&)> onCommitReleased;
  std::function<void(const HandoffOperation&)> onSaved;
  std::function<void(const HandoffOperation&, const std::string&)> onFailed;
  std::function<void(const NativeDurableStorageGrantRoleChallengeV1&)> onWireBudget;
};

class NativeProviderHandoffRegistry :
  public std::enable_shared_from_this<NativeProviderHandoffRegistry>
{
public:
  using LocalWithdrawalFenceResolver = std::function<
    std::optional<ConversationStateStore::SaveFence>(
      const ndn::Name&, const ndn::Name&,
      const NativeConversationHandoffControlV2&)>;
  using LocalWithdrawalQueryResolver = std::function<NativeWithdrawalQueryReplyV1(
    const ndn::Name&, const ndn::Name&, const NativeWithdrawalQueryV1&)>;
  using LocalWithdrawalStageObserver =
    std::function<void(const ndn::Name&, const ndn::Name&, const HandoffOperation&)>;
  using LocalWithdrawalFailureObserver = std::function<void(
    const ndn::Name&, const ndn::Name&, const HandoffOperation&, const std::string&)>;
  using LocalWithdrawalWireBudgetObserver = std::function<void(
    const ndn::Name&, const ndn::Name&, const HandoffOperation&,
    const NativeDurableStorageGrantRoleChallengeV1&)>;
  using LocalWithdrawalConfirmHandler = std::function<NativeWithdrawalConfirmReplyV1(
    const ndn::Name&, const ndn::Name&, const NativeWithdrawalConfirmV1&)>;

  NativeProviderHandoffRegistry(NativeProviderCollaborationRuntime runtime,
    NativeConversationHandoffOwnerContextResolverV1 resolver,
    std::shared_ptr<ServiceProvider> producer)
    : m_runtime(std::move(runtime)), m_resolver(std::move(resolver)),
      m_producer(std::move(producer)),
      m_admission(std::make_shared<NativeProviderHandoffAdmissionState>())
  {
    if (!m_resolver)
      throw std::invalid_argument("HANDOFF_OWNER_CONTEXT_RESOLVER_REQUIRED");
  }

  ~NativeProviderHandoffRegistry()
  {
    requestStop();
  }

  void requestStop() noexcept
  {
    std::vector<std::shared_ptr<NativeProviderHandoffOperation>> operations;
    std::vector<std::shared_ptr<ndn_service_framework::OperationRuntime>> runtimes;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stopping = true;
      for (const auto& [unused, operation] : m_operations) {
        (void)unused;
        operations.push_back(operation);
      }
    }
    {
      std::lock_guard<std::mutex> lock(m_admission->mutex);
      runtimes = m_admission->coordinatorRuntimes;
    }
    for (const auto& operation : operations)
      try { operation->cancel(); }
      catch (...) {}
    for (const auto& runtime : runtimes)
      runtime->close();
  }

  bool drain(std::chrono::milliseconds timeout)
  {
    if (timeout.count() < 0)
      throw std::invalid_argument("HANDOFF_DRAIN_TIMEOUT_INVALID");
    requestStop();
    const auto deadline = HandoffClock::now() + timeout;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      if (!m_handlersDrained.wait_until(lock, deadline,
            [this] { return m_activeHandlers == 0; }))
        return false;
    }
    std::vector<std::shared_ptr<ndn_service_framework::OperationRuntime>> runtimes;
    {
      std::lock_guard<std::mutex> lock(m_admission->mutex);
      runtimes = m_admission->coordinatorRuntimes;
    }
    for (const auto& runtime : runtimes) {
      const auto now = HandoffClock::now();
      const auto remaining = now >= deadline
        ? std::chrono::milliseconds::zero()
        : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      if (!runtime->drain(remaining))
        return false;
    }
    return true;
  }

  bool settleLocalWithdrawal(const std::string& withdrawalId,
                             HandoffClock::time_point deadline)
  {
    // A handler that already borrowed the parent fence may still be creating
    // its worker. The caller first disables new local borrowing, then waits
    // for those handlers before taking the worker snapshot.
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      if (!m_handlersDrained.wait_until(lock, deadline,
            [this] { return m_activeHandlers == 0; }))
        return false;
    }
    std::vector<std::shared_ptr<NativeProviderHandoffOperation>> operations;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      for (const auto& [unused, operation] : m_operations) {
        (void)unused;
        if (operation->localWithdrawalId == withdrawalId)
          operations.push_back(operation);
      }
    }
    for (const auto& operation : operations)
      operation->cancel(); // A released COMMIT cannot be cancelled.
    for (const auto& operation : operations) {
      std::unique_lock<std::mutex> lock(operation->mutex);
      if (!operation->condition.wait_until(lock, deadline,
            [&operation] { return operation->workerFinished; }))
        return false;
    }
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      for (const auto& operation : operations) {
        const auto active = m_conversationOperations.find(operation->control.conversationId);
        if (active != m_conversationOperations.end() &&
            active->second == operation->control.operationId)
          m_conversationOperations.erase(active);
      }
    }
    return true;
  }

  void beginHandler()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_activeHandlers;
  }

  void finishHandler() noexcept
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_activeHandlers > 0 && --m_activeHandlers == 0)
      m_handlersDrained.notify_all();
  }

  void setLocalWithdrawalFenceResolver(LocalWithdrawalFenceResolver resolver)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping)
      throw std::runtime_error("HANDOFF_SERVICE_CLOSING");
    m_localWithdrawalFenceResolver = std::move(resolver);
  }

  void setLocalWithdrawalQueryResolver(LocalWithdrawalQueryResolver resolver)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping)
      throw std::runtime_error("HANDOFF_SERVICE_CLOSING");
    m_localWithdrawalQueryResolver = std::move(resolver);
  }

  void setLocalWithdrawalStageObservers(LocalWithdrawalStageObserver onCommit,
                                        LocalWithdrawalStageObserver onSaved)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping)
      throw std::runtime_error("HANDOFF_SERVICE_CLOSING");
    m_localCommitObserver = std::move(onCommit);
    m_localSavedObserver = std::move(onSaved);
  }

  void setLocalWithdrawalBudgetObservers(LocalWithdrawalWireBudgetObserver onChallenge,
                                         LocalWithdrawalFailureObserver onFailure)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping)
      throw std::runtime_error("HANDOFF_SERVICE_CLOSING");
    m_localWireBudgetObserver = std::move(onChallenge);
    m_localFailureObserver = std::move(onFailure);
  }

  void setLocalWithdrawalConfirmHandler(LocalWithdrawalConfirmHandler handler)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping)
      throw std::runtime_error("HANDOFF_SERVICE_CLOSING");
    m_localConfirmHandler = std::move(handler);
  }

  HandoffReply handle(const ndn::Name& requester, const ndn::Name& provider,
    const ndn::Name& service, const HandoffOperation& control,
    const NativeConversationHandoffControlV2* v2 = nullptr,
    const std::optional<ConversationStateStore::SaveFence>& localWithdrawalFence = std::nullopt)
  {
    if (requester.empty() || provider.empty() || service.empty())
      throw std::invalid_argument("HANDOFF_AUTHENTICATED_SCOPE_REQUIRED");
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_stopping)
        throw std::runtime_error("HANDOFF_SERVICE_CLOSING");
    }
    reapFinished();
    const auto v2Digest = v2
      ? nativeConversationHandoffControlV2ToJson(*v2)
          .at("contextDigest").get<std::string>()
      : std::string{};
    if (control.action == NativeConversationHandoffActionV1::Begin)
      return begin(requester, provider, service, control, v2Digest,
                   v2 ? v2->sourceProvidersByRole :
                        std::map<std::string, std::string>{},
                   v2, localWithdrawalFence);

    auto operation = find(control.operationId);
    if (!operation)
      throw std::invalid_argument("HANDOFF_OPERATION_UNKNOWN");
    if (operation->requester != requester || operation->provider != provider ||
        operation->service != service || !sameHandoffBinding(operation->control, control) ||
        operation->v2ContextDigest != v2Digest)
      throw std::invalid_argument("HANDOFF_OPERATION_BINDING_MISMATCH");

    switch (control.action) {
      case NativeConversationHandoffActionV1::Begin:
        break;
      case NativeConversationHandoffActionV1::Poll:
        return operation->replyFor(control.action);
      case NativeConversationHandoffActionV1::Grant:
        if (!control.grantSubmission)
          throw std::invalid_argument("HANDOFF_GRANT_REQUIRED");
        operation->applyGrant(*control.grantSubmission);
        return operation->replyFor(control.action);
      case NativeConversationHandoffActionV1::Commit:
        return operation->applyCommit();
      case NativeConversationHandoffActionV1::Cancel:
        return operation->cancel();
    }
    throw std::invalid_argument("HANDOFF_ACTION_INVALID");
  }

  HandoffReply handleV2(const ndn::Name& requester, const ndn::Name& provider,
    const ndn::Name& service, const NativeConversationHandoffControlV2& event)
  {
    if (event.trigger != NativeConversationSaveTriggerV2::WithdrawalSave ||
        !event.withdrawalIntent)
      throw std::invalid_argument("HANDOFF_V2_TRIGGER_UNSUPPORTED");
    const auto& intent = *event.withdrawalIntent;
    const auto withdrawingProvider = ndn::Name(event.withdrawingProviderIdentity);
    if (requester.empty() || provider.empty() || service.empty() ||
        intent.ownerIdentity != handoffCanonicalName(requester) ||
        intent.serviceName != handoffCanonicalName(service))
      throw std::invalid_argument("HANDOFF_WITHDRAWAL_AUTHENTICATED_SCOPE_INVALID");
    std::optional<ConversationStateStore::SaveFence> localWithdrawalFence;
    if (withdrawingProvider == provider) {
      LocalWithdrawalFenceResolver resolver;
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        resolver = m_localWithdrawalFenceResolver;
      }
      if (!resolver)
        throw std::invalid_argument("HANDOFF_LOCAL_WITHDRAWAL_STATE_REQUIRED");
      localWithdrawalFence = resolver(requester, service, event);
      if (!localWithdrawalFence || !localWithdrawalFence->valid())
        throw std::invalid_argument("HANDOFF_LOCAL_WITHDRAWAL_STATE_REQUIRED");
    }
    const auto localProvider = handoffCanonicalName(provider);
    const bool isCurrentSource = std::any_of(
      event.sourceProvidersByRole.begin(), event.sourceProvidersByRole.end(),
      [&localProvider] (const auto& pair) { return pair.second == localProvider; });
    if (!isCurrentSource)
      throw std::invalid_argument("HANDOFF_SOURCE_PROVIDER_NOT_IN_ROLE_MAP");
    return handle(requester, provider, service, event.control, &event,
                  localWithdrawalFence);
  }

  NativeWithdrawalQueryReplyV1 handleWithdrawalQuery(
    const ndn::Name& requester, const ndn::Name& provider,
    const ndn::Name& service, const NativeWithdrawalQueryV1& query)
  {
    if (requester.empty() || provider.empty() || service.empty())
      throw std::invalid_argument("WITHDRAWAL_QUERY_AUTHENTICATED_SCOPE_REQUIRED");
    LocalWithdrawalQueryResolver resolver;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_stopping)
        throw std::runtime_error("HANDOFF_SERVICE_CLOSING");
      resolver = m_localWithdrawalQueryResolver;
    }
    (void)handoffDeadline(query.deadlineMs);
    NativeWithdrawalQueryReplyV1 reply;
    reply.query = query;
    return resolver ? resolver(requester, service, query) : reply;
  }

  NativeWithdrawalConfirmReplyV1 handleWithdrawalConfirm(
    const ndn::Name& requester, const ndn::Name& provider,
    const ndn::Name& service, const NativeWithdrawalConfirmV1& confirm)
  {
    if (requester.empty() || provider.empty() || service.empty())
      throw std::invalid_argument("WITHDRAWAL_CONFIRM_AUTHENTICATED_SCOPE_REQUIRED");
    LocalWithdrawalConfirmHandler handler;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_stopping)
        throw std::runtime_error("HANDOFF_SERVICE_CLOSING");
      handler = m_localConfirmHandler;
    }
    if (!handler)
      throw std::invalid_argument("WITHDRAWAL_CONFIRM_LOCAL_STATE_REQUIRED");
    return handler(requester, service, confirm);
  }

private:
  std::shared_ptr<NativeProviderHandoffAdmissionLease>
  reserveAdmission(const NativeConversationHandoffOwnerContextV1& context)
  {
    const auto& policy = context.policy;
    const auto checkpointLimits = nativeKvPublicationLimits(context.publication);
    const auto roles = context.repoTargetByRole.size();
    if (roles == 0 || roles > policy.maxRolesPerEvent ||
        checkpointLimits.first.portable.maxEncodedBytes >
          std::numeric_limits<std::uint64_t>::max() -
            checkpointLimits.second.maxWireBytes)
      throw std::runtime_error("BUDGET_EXCEEDED");
    const auto perRoleScratch = checkpointLimits.first.portable.maxEncodedBytes +
      checkpointLimits.second.maxWireBytes;
    if (perRoleScratch == 0 ||
        roles > policy.maxAggregateBytes / policy.maxPacketSetWireBytesPerRole ||
        roles > policy.maxScratchBytesPerEvent / perRoleScratch)
      throw std::runtime_error("BUDGET_EXCEEDED");
    std::lock_guard<std::mutex> registryLock(m_mutex);
    if (m_stopping)
      throw std::runtime_error("HANDOFF_SERVICE_CLOSING");
    std::lock_guard<std::mutex> lock(m_admission->mutex);
    const auto eventLimit = std::min(
      m_admission->effectiveMaxConcurrentEvents, policy.maxConcurrentEvents);
    const auto roleLimit = std::min(
      m_admission->effectiveMaxConcurrentRoleSlots, policy.maxConcurrentRoleSlots);
    const auto scratchLimit = std::min(
      m_admission->effectiveScratchQuotaBytes, policy.scratchQuotaBytes);
    if (m_admission->activeEvents >= eventLimit ||
        roles > roleLimit || m_admission->activeRoleSlots > roleLimit - roles ||
        policy.maxScratchBytesPerEvent > scratchLimit ||
        m_admission->reservedScratchBytes >
          scratchLimit - policy.maxScratchBytesPerEvent)
      throw std::runtime_error("BUDGET_EXCEEDED");

    std::shared_ptr<ndn_service_framework::OperationRuntime> coordinatorRuntime;
    for (const auto& runtime : m_admission->coordinatorRuntimes) {
      if (m_admission->busyCoordinatorRuntimes.count(runtime.get()) == 0) {
        coordinatorRuntime = runtime;
        break;
      }
    }
    if (!coordinatorRuntime) {
      if (m_admission->coordinatorRuntimes.size() >= eventLimit)
        throw std::runtime_error("BUDGET_EXCEEDED");
      coordinatorRuntime = ndn_service_framework::OperationRuntime::create();
      m_admission->coordinatorRuntimes.push_back(coordinatorRuntime);
    }

    auto lease = std::make_shared<NativeProviderHandoffAdmissionLease>();
    lease->coordinatorRuntime = coordinatorRuntime;
    lease->roleSlots = roles;
    lease->scratchBytes = policy.maxScratchBytesPerEvent;
    m_admission->busyCoordinatorRuntimes.insert(coordinatorRuntime.get());
    lease->state = m_admission;
    ++m_admission->activeEvents;
    m_admission->activeRoleSlots += roles;
    m_admission->reservedScratchBytes += policy.maxScratchBytesPerEvent;
    m_admission->effectiveMaxConcurrentEvents = eventLimit;
    m_admission->effectiveMaxConcurrentRoleSlots = roleLimit;
    m_admission->effectiveScratchQuotaBytes = scratchLimit;
    return lease;
  }

  std::shared_ptr<NativeProviderHandoffOperation> find(const std::string& id)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_operations.find(id);
    return it == m_operations.end() ? nullptr : it->second;
  }

  HandoffReply begin(const ndn::Name& requester, const ndn::Name& provider,
    const ndn::Name& service, const HandoffOperation& control,
    const std::string& v2ContextDigest,
    const std::map<std::string, std::string>& sourceProvidersByRole,
    const NativeConversationHandoffControlV2* v2,
    const std::optional<ConversationStateStore::SaveFence>& localWithdrawalFence)
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_stopping)
        throw std::runtime_error("HANDOFF_SERVICE_CLOSING");
      const auto existing = m_operations.find(control.operationId);
      if (existing != m_operations.end()) {
        const auto& operation = existing->second;
        if (operation->requester != requester || operation->provider != provider ||
            operation->service != service || !sameHandoffBinding(operation->control, control) ||
            operation->v2ContextDigest != v2ContextDigest)
          throw std::invalid_argument("HANDOFF_OPERATION_BINDING_MISMATCH");
        return operation->replyFor(NativeConversationHandoffActionV1::Begin);
      }
      const auto active = m_conversationOperations.find(control.conversationId);
      if (active != m_conversationOperations.end())
        throw std::invalid_argument("HANDOFF_CONVERSATION_ALREADY_ACTIVE");
      if (m_operations.size() >= 256)
        throw std::runtime_error("HANDOFF_OPERATION_LIMIT");
    }

    auto operation = std::make_shared<NativeProviderHandoffOperation>(
      control, requester, provider, service, v2ContextDigest, sourceProvidersByRole,
      v2 ? std::make_optional(*v2) : std::nullopt);
    LocalWithdrawalWireBudgetObserver onWireBudget;
    if (localWithdrawalFence) {
      operation->localWithdrawalId = v2->withdrawalIntent->withdrawalId;
      std::lock_guard<std::mutex> lock(m_mutex);
      if (!m_localCommitObserver || !m_localSavedObserver ||
          !m_localWireBudgetObserver || !m_localFailureObserver)
        throw std::runtime_error("HANDOFF_LOCAL_WITHDRAWAL_OBSERVERS_UNAVAILABLE");
      const auto onCommit = m_localCommitObserver;
      const auto onSaved = m_localSavedObserver;
      const auto onFailure = m_localFailureObserver;
      onWireBudget = m_localWireBudgetObserver;
      operation->onCommitReleased = [onCommit, requester, service](
        const HandoffOperation& binding) { onCommit(requester, service, binding); };
      operation->onSaved = [onSaved, requester, service](
        const HandoffOperation& binding) { onSaved(requester, service, binding); };
      operation->onFailed = [onFailure, requester, service](
        const HandoffOperation& binding, const std::string& reason) {
        onFailure(requester, service, binding, reason);
      };
      operation->onWireBudget = [onWireBudget, requester, service, control](
        const NativeDurableStorageGrantRoleChallengeV1& challenge) {
        onWireBudget(requester, service, control, challenge);
      };
    }
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto [it, inserted] = m_operations.emplace(control.operationId, operation);
      if (!inserted) {
        const auto& prior = it->second;
        if (prior->requester != requester || prior->provider != provider ||
            prior->service != service || !sameHandoffBinding(prior->control, control) ||
            prior->v2ContextDigest != v2ContextDigest)
          throw std::invalid_argument("HANDOFF_OPERATION_BINDING_MISMATCH");
        return prior->replyFor(NativeConversationHandoffActionV1::Begin);
      }
      const auto [conversationIt, conversationInserted] =
        m_conversationOperations.emplace(control.conversationId, control.operationId);
      if (!conversationInserted) {
        m_operations.erase(it);
        throw std::invalid_argument("HANDOFF_CONVERSATION_ALREADY_ACTIVE");
      }
    }

    const char* failureStage = "OWNER_CONTEXT_RESOLVE";
    try {
      auto context = resolveNativeConversationHandoffOwnerContextV1(
        m_resolver, requester, service, control.operationId);
      failureStage = "OWNER_CONTEXT_VALIDATE";
      nativeValidateKvSaveConfig(context.publication, provider);
      failureStage = "WITHDRAWAL_POLICY_CHECK";
      if (localWithdrawalFence &&
          (!v2 || !v2->withdrawalIntent ||
           context.policy.effectiveDigest != v2->withdrawalIntent->policyDigest))
        throw std::invalid_argument("HANDOFF_WITHDRAWAL_POLICY_CHANGED");
      failureStage = "OWNER_CONTEXT_BINDING";
      if (context.ownerEncryptionCertificate.getIdentity() != requester ||
          context.repoTargetByRole.empty() ||
          context.repoTargetByRole.size() > 64)
        throw std::invalid_argument("HANDOFF_OWNER_CONTEXT_BINDING_INVALID");
      failureStage = "DEADLINE_CHECK";
      const auto now = handoffNowMs();
      if (control.deadlineMs <= now ||
          control.deadlineMs - now > context.policy.maxSaveDurationMs)
        throw std::runtime_error("HANDOFF_DEADLINE_EXCEEDS_POLICY");
      failureStage = "ADMISSION_RESERVE";
      operation->admission = reserveAdmission(context);
      std::optional<ConversationStateStore::SaveFence> fence;
      if (localWithdrawalFence) {
        failureStage = "WITHDRAWAL_SAVE_FENCE";
        if (!v2 || !v2->withdrawalIntent ||
            !m_runtime.borrowConversationWithdrawalSaveFence)
          throw std::runtime_error("HANDOFF_LOCAL_WITHDRAWAL_FENCE_UNAVAILABLE");
        fence = m_runtime.borrowConversationWithdrawalSaveFence(
          *localWithdrawalFence, handoffCanonicalName(requester),
          handoffCanonicalName(service), control.conversationId,
          v2->withdrawalIntent->withdrawalId,
          v2->withdrawalIntent->registrationGeneration);
      }
      else {
        failureStage = "SAVE_FENCE";
        fence = m_runtime.beginConversationSaveFence(
          handoffCanonicalName(requester), handoffCanonicalName(service),
          control.conversationId);
      }
      failureStage = "SAVE_FENCE_VALIDATE";
      if (!fence || !fence->valid())
        throw std::runtime_error("HANDOFF_SAVE_FENCE_UNAVAILABLE");
      failureStage = "OWNER_CONTEXT_INSTALL";
      operation->setOwnerContext(std::move(context));
      failureStage = "WORKER_RUNTIME_ACQUIRE";
      auto coordinatorRuntime = operation->admission->coordinatorRuntime;
      auto workTicket = coordinatorRuntime->acquire();
      failureStage = "WORKER_POST";
      auto self = shared_from_this();
      coordinatorRuntime->post(workTicket,
        [self = std::move(self), coordinatorRuntime = std::move(coordinatorRuntime),
         operation, fence = *fence] {
          try {
            self->run(operation, fence);
          }
          catch (const std::exception& e) {
            operation->fail(e.what());
            operation->releaseOwnerContext();
          }
          catch (...) {
            operation->fail("HANDOFF_WORKER_FAILED_NONSTANDARD_EXCEPTION");
            operation->releaseOwnerContext();
          }
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            operation->workerFinished = true;
            operation->condition.notify_all();
          }
        });
    }
    catch (const std::exception& e) {
      try {
        ndnsf::di::logRuntimeEvidence(
          "NDNSF_DI_HANDOFF_BEGIN_FAILED stage=" +
          std::string(failureStage) + " exception=STANDARD");
      }
      catch (...) {}
      operation->fail(e.what());
      operation->releaseOwnerContext();
      operation->admission.reset();
      {
        std::lock_guard<std::mutex> lock(operation->mutex);
        operation->workerFinished = true;
        operation->condition.notify_all();
      }
      throw;
    }
    catch (...) {
      try {
        ndnsf::di::logRuntimeEvidence(
          "NDNSF_DI_HANDOFF_BEGIN_FAILED stage=" +
          std::string(failureStage) + " exception=NON_STANDARD");
      }
      catch (...) {}
      operation->fail("HANDOFF_BEGIN_FAILED_NONSTANDARD_EXCEPTION");
      operation->releaseOwnerContext();
      operation->admission.reset();
      {
        std::lock_guard<std::mutex> lock(operation->mutex);
        operation->workerFinished = true;
        operation->condition.notify_all();
      }
      throw;
    }
    return operation->replyFor(NativeConversationHandoffActionV1::Begin);
  }

  static std::filesystem::path makeWorkspace(
    const NativeProviderHandoffOperation& operation,
    const NativeConversationHandoffOwnerContextV1& context)
  {
    auto root = context.policy.scratchRoot /
      ("ndnsf-di-handoff-" + std::to_string(static_cast<unsigned long long>(::getuid())));
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec || std::filesystem::is_symlink(std::filesystem::symlink_status(root, ec)) || ec)
      throw std::runtime_error("HANDOFF_WORKSPACE_UNAVAILABLE");
    const auto available = std::filesystem::space(root, ec).available;
    if (ec || available < context.policy.maxScratchBytesPerEvent)
      throw std::runtime_error("BUDGET_EXCEEDED");
    std::filesystem::permissions(root, std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace, ec);
    if (ec)
      throw std::runtime_error("HANDOFF_WORKSPACE_UNAVAILABLE");
    const auto identity = nativePlanningDigest(nativeCanonicalJson(NativeJson{
      {"operationId", operation.control.operationId},
      {"requester", handoffCanonicalName(operation.requester)},
      {"provider", handoffCanonicalName(operation.provider)}}));
    std::string component;
    component.reserve(identity.size());
    for (const auto c : identity)
      component.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
        ? c : '_');
    std::random_device random;
    const auto directory = root / (component + "-" + std::to_string(random()));
    if (!std::filesystem::create_directory(directory, ec) || ec)
      throw std::runtime_error("HANDOFF_WORKSPACE_UNAVAILABLE");
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace, ec);
    if (ec) {
      std::filesystem::remove_all(directory);
      throw std::runtime_error("HANDOFF_WORKSPACE_UNAVAILABLE");
    }
    return directory;
  }

  void run(const std::shared_ptr<NativeProviderHandoffOperation>& operation,
    const ConversationStateStore::SaveFence& fence)
  {
    const char* failureStage = "WORKER_INITIALIZE";
    try {
      const auto current = [operation] { operation->requireCurrent(); };
      current();
      operation->awaitInitialization();
      NativeConversationHandoffOwnerContextV1 context;
      {
        std::lock_guard<std::mutex> lock(operation->mutex);
        context = *operation->ownerContext;
      }
      failureStage = "OWNER_CERTIFICATE_ADMISSION";
      const auto producerOwner = m_producer.lock();
      const auto& ownerCertificateWire = context.ownerEncryptionCertificate.wireEncode();
      if (!producerOwner || !producerOwner->validateProducerPacketWireUntil(
            std::vector<std::uint8_t>(ownerCertificateWire.begin(), ownerCertificateWire.end()),
            operation->requester, operation->deadline))
        throw std::runtime_error("HANDOFF_OWNER_CERTIFICATE_REJECTED");
      current();
      // The immutable V2 digest binds subsequent controls to this proof. Admit
      // it on the existing worker before any export, challenge or COMMIT boundary.
      if (operation->withdrawalControl) {
        const auto& event = *operation->withdrawalControl;
        const auto withdrawingProvider = ndn::Name(event.withdrawingProviderIdentity);
        if (withdrawingProvider != operation->provider) {
          failureStage = "WITHDRAWAL_PROOF_ADMISSION";
          nativeValidateWithdrawalIntentProofV1(
            event.withdrawalProofWire, withdrawingProvider, *event.withdrawalIntent,
            handoffNowMs(), [&] (const ndn::Data&, const ndn::Name& expectedProducer) {
              return producerOwner->validateProducerPacketWireUntil(
                event.withdrawalProofWire, expectedProducer, operation->deadline);
            });
          current();
        }
      }
      const auto checkpointLimits = nativeKvPublicationLimits(context.publication, current);
      if (!m_runtime.isSaveFenceQuiescent || !m_runtime.tryAcquireCommittedSaveBatch ||
          !m_runtime.exportCommittedSaveBatchRow || !m_runtime.publishProtectedKvCheckpoint)
        throw std::runtime_error("HANDOFF_RUNTIME_SEAMS_UNAVAILABLE");

      failureStage = "SAVE_FENCE_QUIESCENCE";
      while (!m_runtime.isSaveFenceQuiescent(fence)) {
        current();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      ConversationStateStore::SaveBatchResult savedBatch;
      failureStage = "SAVE_BATCH_ACQUIRE";
      for (;;) {
        current();
        savedBatch = m_runtime.tryAcquireCommittedSaveBatch(
          fence, context.saveBatchLimits, handoffNowMs());
        if (savedBatch.status != ConversationSaveBatchStatusV1::Busy)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (savedBatch.status != ConversationSaveBatchStatusV1::Ready || !savedBatch.batch)
        throw std::runtime_error("HANDOFF_SAVE_BATCH_UNAVAILABLE");
      auto batch = std::move(*savedBatch.batch);
      if (batch.size() == 0 || batch.size() > context.policy.maxRolesPerEvent ||
          batch.size() != context.repoTargetByRole.size())
        throw std::runtime_error("BUDGET_EXCEEDED");

      const auto requester = handoffCanonicalName(operation->requester);
      const auto service = handoffCanonicalName(operation->service);
      const auto provider = handoffCanonicalName(operation->provider);
      std::map<std::string, std::size_t> rowByRole;
      std::vector<std::string> roles;
      NativeDurableStorageGrantBarrierV1::ProducerPrefixesByRole producers;
      failureStage = "SAVE_ROW_BINDING";
      for (std::size_t i = 0; i < batch.rows().size(); ++i) {
        const auto& row = batch.rows()[i];
        row.binding.validate();
        const auto& role = row.binding.identity.roleName;
        if (role.empty() || row.binding.conversationId != operation->control.conversationId ||
            row.binding.serviceName != service ||
            row.binding.planRoleMapDigest != operation->control.sourceRoleMapDigest ||
            row.binding.checkpointDigest != operation->control.checkpointDigest ||
            row.metadata.authenticatedRequester != requester ||
            row.binding.identity.providerIdentity != provider ||
            row.binding.expiresAtMs <= handoffNowMs() ||
            !rowByRole.emplace(role, i).second)
          throw std::runtime_error("HANDOFF_SAVE_ROW_BINDING_INVALID");
        roles.push_back(role);
        producers.emplace(role, operation->provider);
      }
      std::sort(roles.begin(), roles.end());
      if (!operation->v2ContextDigest.empty()) {
        std::vector<std::string> expectedLocalRoles;
        for (const auto& [role, source] : operation->sourceProvidersByRole) {
          if (source == handoffCanonicalName(provider))
            expectedLocalRoles.push_back(role);
        }
        if (roles != expectedLocalRoles)
          throw std::runtime_error("HANDOFF_SOURCE_ROLE_MEMBERSHIP_CHANGED");
      }
      if (roles.size() > context.policy.maxRolesPerEvent)
        throw std::runtime_error("BUDGET_EXCEEDED");
      operation->setRoles(roles);
      if (context.repoTargetByRole.size() != roles.size())
        throw std::runtime_error("HANDOFF_REPO_TARGET_ROLE_SET_MISMATCH");
      for (const auto& role : roles) {
        const auto target = context.repoTargetByRole.find(role);
        if (target == context.repoTargetByRole.end())
          throw std::runtime_error("HANDOFF_REPO_TARGET_ROLE_SET_MISMATCH");
        for (const auto* uri : {&target->second.repoNode, &target->second.providerIdentity,
                                 &target->second.servicePrefix}) {
          const ndn::Name name(*uri);
          if (name.empty() || name.toUri(ndn::name::UriFormat::ALTERNATE) != *uri)
            throw std::runtime_error("HANDOFF_REPO_TARGET_INVALID");
        }
      }

      failureStage = "SAVE_WORKSPACE";
      const auto workspacePath = makeWorkspace(*operation, context);
      struct WorkspaceCleanup
      {
        std::filesystem::path path;
        ~WorkspaceCleanup()
        {
          std::error_code ec;
          std::filesystem::remove_all(path, ec);
        }
      } workspace{workspacePath};
      struct Material
      {
        std::string role;
        std::filesystem::path protectedPath;
        NativeProtectedKvCheckpointReferenceV1 reference;
      };
      std::vector<Material> materials;
      materials.reserve(roles.size());
      std::uint64_t actualScratchBytes = 0;
      NativeJson snapshotIdentity = NativeJson::object();
      const auto& fileLimits = checkpointLimits.first;

      for (const auto& role : roles) {
        current();
        const auto rowIndex = rowByRole.at(role);
        failureStage = "PORTABLE_EXPORT";
        const auto snapshot = m_runtime.exportCommittedSaveBatchRow(
          batch, rowIndex, checkpointLimits.first.portable, handoffNowMs());
        validateNativePortableKvSnapshot(snapshot, checkpointLimits.first.portable);
        if (snapshot.originIdentity.roleName != role ||
            snapshot.originIdentity.providerIdentity != provider)
          throw std::runtime_error("HANDOFF_PORTABLE_SNAPSHOT_BINDING_INVALID");
        const auto& row = batch.rows()[rowIndex];
        const auto checkpointIdentity = row.metadata.receipt.computedDigest();
        const auto snapshotRoleDigest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
          {"schema", "ndnsf-di-conversation-handoff-snapshot-v1"},
          {"operationId", operation->control.operationId},
          {"conversationId", operation->control.conversationId},
          {"checkpointIdentityDigest", checkpointIdentity},
          {"sourceRoleMapDigest", operation->control.sourceRoleMapDigest},
          {"roleName", role}, {"stateIoDigest", snapshot.stateIoDigest},
          {"encodedBytes", snapshot.encodedBytes}}));
        const auto base = workspacePath / ("role-" + std::to_string(materials.size()));
        const auto privatePath = std::filesystem::path(base.string() + ".private");
        const auto protectedPath = std::filesystem::path(base.string() + ".protected");
        failureStage = "CHECKPOINT_PROTECT";
        nativeExportPrivatePortableKvCheckpoint(privatePath, snapshot, fileLimits);
        std::error_code sizeError;
        const auto privateBytes = std::filesystem::file_size(privatePath, sizeError);
        if (sizeError || privateBytes > context.policy.maxScratchBytesPerEvent - actualScratchBytes)
          throw std::runtime_error("BUDGET_EXCEEDED");
        auto protectedLimits = checkpointLimits.second;
        const auto configuredCancelled = protectedLimits.cancelled;
        protectedLimits.cancelled = [operation, configuredCancelled] {
          if (configuredCancelled && configuredCancelled())
            return true;
          try {
            operation->requireCurrent();
            return false;
          }
          catch (...) {
            return true;
          }
        };
        auto reference = nativeProtectPortableKvCheckpoint(
          privatePath, protectedPath, context.ownerEncryptionCertificate,
          operation->provider, checkpointIdentity, protectedLimits);
        if (reference.wireBytes > context.policy.maxScratchBytesPerEvent -
              actualScratchBytes - privateBytes)
          throw std::runtime_error("BUDGET_EXCEEDED");
        actualScratchBytes += privateBytes + reference.wireBytes;
        std::filesystem::remove(privatePath);
        snapshotIdentity[role] = snapshotRoleDigest;
        materials.push_back(Material{role, protectedPath, std::move(reference)});
      }

      NativeDurableStorageGrantChallengeV1 fullChallenge;
      fullChallenge.operationId = operation->control.operationId;
      failureStage = "PUBLICATION_DISPATCH";
      fullChallenge.checkpointDigest = operation->control.checkpointDigest;
      fullChallenge.snapshotDigest = nativePlanningDigest(nativeCanonicalJson(snapshotIdentity));
      fullChallenge.roleMapDigest = operation->control.sourceRoleMapDigest;
      const auto barrier = std::make_shared<NativeDurableStorageGrantBarrierV1>(
        requester, producers, operation->deadline);
      operation->setGrantBarrier(barrier);

      std::vector<std::future<std::pair<std::string, NativeJson>>> publications;
      publications.reserve(materials.size());
      struct PublicationBarrierAbortGuard
      {
        std::shared_ptr<NativeProviderHandoffOperation> operation;
        std::shared_ptr<NativeDurableStorageGrantBarrierV1> barrier;
        std::exception_ptr error;
        bool completed = false;
        ~PublicationBarrierAbortGuard()
        {
          if (!completed) {
            operation->signalPublicationError(error);
            barrier->abort(error);
          }
        }
      } abortBarrier{operation, barrier,
        std::make_exception_ptr(std::runtime_error("HANDOFF_PUBLICATION_ABORTED"))};
      for (const auto& material : materials) {
        const auto role = material.role;
        const auto route = context.repoTargetByRole.at(role);
        const auto policy = context.policy;
        NativeDurableStorageOwner owner;
        owner.ownerIdentity = requester;
        owner.publication = context.publication;
        owner.policy = policy;
        owner.repoTarget = route;
        owner.receipt = batch.rows()[rowByRole.at(role)].metadata.receipt;
        owner.deadline = operation->deadline;
        owner.resolveGrant = barrier->resolverFor(role);
        owner.onGrantRequired = [operation, route, role, provider, policy](
          const NativeDurableStorageGrantRoleChallengeV1& challenge) {
          if (challenge.roleName != role || challenge.producerPrefix != provider ||
              challenge.writerIdentity != provider || challenge.manifestName.empty() ||
              challenge.publicationIdentity.empty() || challenge.protectionEpoch.empty() ||
              challenge.dataPrefix.empty() || challenge.packetWireBytes == 0 ||
              challenge.packetWireBytes > policy.maxPacketSetWireBytesPerRole ||
              challenge.auxiliaryPacketNames.size() + 1 > policy.maxPacketCountPerRole ||
              challenge.repoTarget.repoNode != route.repoNode ||
              challenge.repoTarget.providerIdentity != route.providerIdentity ||
              challenge.repoTarget.servicePrefix != route.servicePrefix)
            throw std::runtime_error("HANDOFF_GRANT_CHALLENGE_INVALID");
          operation->recordChallenge(challenge);
          if (operation->onWireBudget)
            operation->onWireBudget(challenge);
        };
        const auto existingBoundary = owner.onDurablePacketSetBoundary;
        owner.onDurablePacketSetBoundary = [operation, existingBoundary, role] {
          if (existingBoundary)
            existingBoundary();
          operation->parkRole(role);
        };
        auto protectedLimits = checkpointLimits.second;
        const auto configuredCancelled = protectedLimits.cancelled;
        protectedLimits.cancelled = [operation, configuredCancelled] {
          if (configuredCancelled && configuredCancelled())
            return true;
          try {
            operation->requireCurrent();
            return false;
          }
          catch (...) {
            return true;
          }
        };
        auto requestId = operation->provider;
        requestId.append("conversation-handoff").append(operation->control.operationId).append(role);
        const auto privateMaterial = material;
        publications.emplace_back(std::async(std::launch::async,
          [this, operation, barrier, owner = std::move(owner), protectedLimits,
           privateMaterial, role, requestId, current]() mutable {
            try {
              auto publication = m_runtime.publishProtectedKvCheckpoint(
                operation->requester, operation->service, requestId,
                privateMaterial.protectedPath, privateMaterial.reference, owner,
                protectedLimits, current);
              publication.requireCurrent();
              return std::make_pair(role, nativeProtectedKvPublicationReference(publication));
            }
            catch (...) {
              auto error = std::current_exception();
              operation->signalPublicationError(error);
              barrier->abort(error);
              throw;
            }
          }));
      }

      failureStage = "PUBLICATION_CHALLENGES";
      const auto minimalChallenges = barrier->waitForAllChallenges(current);
      operation->awaitAllChallenges();
      {
        std::lock_guard<std::mutex> lock(operation->mutex);
        if (operation->roleChallenges.size() != roles.size())
          throw std::runtime_error("HANDOFF_GRANT_CHALLENGE_ROLE_SET_MISMATCH");
        for (const auto& minimal : minimalChallenges) {
          const auto detailed = operation->roleChallenges.find(minimal.roleName);
          if (detailed == operation->roleChallenges.end() ||
              detailed->second.manifestName != minimal.manifestName.toUri(ndn::name::UriFormat::ALTERNATE) ||
              detailed->second.writerIdentity != minimal.writerIdentity)
            throw std::runtime_error("HANDOFF_GRANT_CHALLENGE_BINDING_MISMATCH");
          fullChallenge.roles.push_back(detailed->second);
        }
      }
      std::uint64_t publishedWireBytes = 0;
      for (const auto& roleChallenge : fullChallenge.roles) {
        if (roleChallenge.packetWireBytes >
            context.policy.maxAggregateBytes - publishedWireBytes)
          throw std::runtime_error("BUDGET_EXCEEDED");
        publishedWireBytes += roleChallenge.packetWireBytes;
      }
      operation->setChallenge(std::move(fullChallenge));
      failureStage = "OWNER_COMMIT";
      operation->awaitCommit();

      failureStage = "PUBLICATION_COMPLETE";
      std::map<std::string, NativeJson> references;
      for (auto& future : publications) {
        current();
        auto item = future.get();
        if (!references.emplace(std::move(item.first), std::move(item.second)).second)
          throw std::runtime_error("HANDOFF_SAVED_ROLE_DUPLICATE");
      }
      if (references.size() != roles.size())
        throw std::runtime_error("HANDOFF_SAVED_ROLE_SET_MISMATCH");
      operation->finishSaved(std::move(references));
      abortBarrier.completed = true;
    }
    catch (const std::exception& e) {
      try {
        ndnsf::di::logRuntimeEvidence(
          "NDNSF_DI_HANDOFF_SAVE_FAILED stage=" +
          std::string(failureStage) + " exception=STANDARD");
      }
      catch (...) {}
      operation->fail(e.what());
      operation->releaseOwnerContext();
    }
    catch (...) {
      try {
        ndnsf::di::logRuntimeEvidence(
          "NDNSF_DI_HANDOFF_SAVE_FAILED stage=" +
          std::string(failureStage) + " exception=NON_STANDARD");
      }
      catch (...) {}
      operation->fail("HANDOFF_FAILED_NONSTANDARD_EXCEPTION");
      operation->releaseOwnerContext();
    }
    {
      std::lock_guard<std::mutex> lock(operation->mutex);
      operation->admission.reset();
    }
  }

  void reapFinished()
  {
    std::vector<std::shared_ptr<NativeProviderHandoffOperation>> expired;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      for (const auto& [unused, operation] : m_operations) {
        (void)unused;
        std::lock_guard<std::mutex> operationLock(operation->mutex);
        if (operation->done && operation->workerFinished &&
            !operation->commitReleased &&
            (operation->cancelled || !operation->failure.empty())) {
          // A settled failure releases its conversation, while the retained
          // operation still answers duplicates with the original outcome.
          const auto active = m_conversationOperations.find(operation->control.conversationId);
          if (active != m_conversationOperations.end() &&
              active->second == operation->control.operationId)
            m_conversationOperations.erase(active);
        }
        if (HandoffClock::now() <= operation->deadline + std::chrono::seconds(60))
          continue;
        if (operation->done && operation->workerFinished)
          expired.push_back(operation);
      }
      for (const auto& operation : expired) {
        const auto active = m_conversationOperations.find(operation->control.conversationId);
        if (active != m_conversationOperations.end() &&
            active->second == operation->control.operationId)
          m_conversationOperations.erase(active);
        m_operations.erase(operation->control.operationId);
      }
    }
  }

private:
  NativeProviderCollaborationRuntime m_runtime;
  NativeConversationHandoffOwnerContextResolverV1 m_resolver;
  std::weak_ptr<ServiceProvider> m_producer;
  std::shared_ptr<NativeProviderHandoffAdmissionState> m_admission;
  std::mutex m_mutex;
  std::condition_variable m_handlersDrained;
  std::size_t m_activeHandlers = 0;
  bool m_stopping = false;
  std::map<std::string, std::shared_ptr<NativeProviderHandoffOperation>> m_operations;
  std::map<std::string, std::string> m_conversationOperations;
  LocalWithdrawalFenceResolver m_localWithdrawalFenceResolver;
  LocalWithdrawalQueryResolver m_localWithdrawalQueryResolver;
  LocalWithdrawalStageObserver m_localCommitObserver;
  LocalWithdrawalStageObserver m_localSavedObserver;
  LocalWithdrawalWireBudgetObserver m_localWireBudgetObserver;
  LocalWithdrawalFailureObserver m_localFailureObserver;
  LocalWithdrawalConfirmHandler m_localConfirmHandler;
};

ServiceProvider::AsyncTargetedRequestHandler
makeNativeConversationHandoffHandler(
  const NativeProviderCollaborationRuntime& runtime,
  NativeConversationHandoffOwnerContextResolverV1 resolver,
  std::shared_ptr<NativeProviderHandoffRegistry>& registryOwner,
  std::shared_ptr<ServiceProvider> producer)
{
  auto registry = std::make_shared<NativeProviderHandoffRegistry>(
    runtime, std::move(resolver), std::move(producer));
  registryOwner = registry;
  return [registry = std::move(registry)](
    const ndn::Name& requester, const ndn::Name& provider, const ndn::Name& service,
    const ndn::Name&, const ndn_service_framework::RequestMessage& request,
    ServiceProvider::AsyncTargetedCompletion completion) {
    registry->beginHandler();
    struct HandlerCompletion
    {
      std::shared_ptr<NativeProviderHandoffRegistry> registry;
      ~HandlerCompletion() { registry->finishHandler(); }
    } handlerCompletion{registry};
    ndn_service_framework::ResponseMessage response;
    const char* failureStage = "CONTROL_SIZE_CHECK";
    try {
      if (request.getPayloadSize() == 0 || request.getPayloadSize() > 128 * 1024)
        throw std::invalid_argument("HANDOFF_CONTROL_SIZE_INVALID");
      const auto payload = request.getPayload();
      const std::string wire(reinterpret_cast<const char*>(payload.data()), payload.size());
      failureStage = "CONTROL_ENVELOPE_PARSE";
      const auto envelope = nativeParseJson(wire);
      failureStage = "CONTROL_SCHEMA_CHECK";
      if (!envelope.is_object() || !envelope.contains("schema") ||
          !envelope.at("schema").is_string())
        throw std::invalid_argument("HANDOFF_CONTROL_SCHEMA_INVALID");
      const auto schema = envelope.at("schema").get<std::string>();
      NativeJson replyValue;
      if (schema == "ndnsf-di-withdrawal-query-v1") {
        failureStage = "WITHDRAWAL_QUERY_DISPATCH";
        replyValue = nativeWithdrawalQueryReplyV1ToJson(
          registry->handleWithdrawalQuery(requester, provider, service,
            nativeWithdrawalQueryV1FromJson(wire)));
      }
      else if (schema == "ndnsf-di-withdrawal-confirm-v1") {
        failureStage = "WITHDRAWAL_CONFIRM_DISPATCH";
        replyValue = nativeWithdrawalConfirmReplyV1ToJson(
          registry->handleWithdrawalConfirm(requester, provider, service,
            nativeWithdrawalConfirmV1FromJson(wire)));
      }
      else if (schema == "ndnsf-di-handoff-control-v2") {
        failureStage = "HANDOFF_CONTROL_V2_DISPATCH";
        replyValue = nativeConversationHandoffReplyV1ToJson(
          registry->handleV2(requester, provider, service,
            nativeConversationHandoffControlV2FromJson(wire)));
      }
      else {
        failureStage = "HANDOFF_CONTROL_V1_PARSE";
        const auto control = nativeConversationHandoffControlV1FromJson(wire);
        failureStage = "HANDOFF_CONTROL_V1_REGISTRY";
        const auto reply = registry->handle(requester, provider, service, control);
        failureStage = "HANDOFF_CONTROL_V1_REPLY";
        replyValue = nativeConversationHandoffReplyV1ToJson(reply);
      }
      failureStage = "REPLY_SERIALIZE";
      const auto replyWire = nativeCanonicalJson(replyValue);
      failureStage = "REPLY_ENCODE";
      ndn::Buffer replyPayload(replyWire.begin(), replyWire.end());
      response.setStatus(true);
      response.setPayload(replyPayload, replyPayload.size());
    }
    catch (const std::exception& error) {
      try {
        ndnsf::di::logRuntimeEvidence(
          "NDNSF_DI_HANDOFF_HANDLER_STAGE_FAILED stage=" +
          std::string(failureStage) + " exception=STANDARD");
      }
      catch (...) {}
      try {
        static constexpr std::array<std::string_view, 8> safeReasonCodes{{
          "BUDGET_EXCEEDED", "HANDOFF_AUTHENTICATED_OWNER_SCOPE_INVALID",
          "HANDOFF_OWNER_CONTEXT_BINDING_INVALID", "HANDOFF_EFFECTIVE_POLICY_INVALID",
          "HANDOFF_OWNER_TRUST_PROFILE_MISMATCH", "HANDOFF_CONTROL_SIZE_INVALID",
          "HANDOFF_CONTROL_SCHEMA_INVALID", "HANDOFF_SAVE_FENCE_UNAVAILABLE"}};
        const std::string_view detail(error.what());
        const auto known = std::find(safeReasonCodes.begin(), safeReasonCodes.end(), detail);
        ndnsf::di::logRuntimeEvidence(
          "NDNSF_DI_HANDOFF_HANDLER_REJECTED reasonCode=" +
          (known != safeReasonCodes.end() ? std::string(*known) :
                                         std::string("UNCLASSIFIED_EXCEPTION")));
      }
      catch (...) {}
      response.setStatus(false);
      const auto publicError = std::string_view(error.what()) == "BUDGET_EXCEEDED"
        ? "BUDGET_EXCEEDED" : "conversation handoff event failed";
      response.setErrorInfo(publicError);
    }
    catch (...) {
      try {
        ndnsf::di::logRuntimeEvidence(
          "NDNSF_DI_HANDOFF_HANDLER_STAGE_FAILED stage=" +
          std::string(failureStage) + " exception=NON_STANDARD");
      }
      catch (...) {}
      response.setStatus(false);
      response.setErrorInfo("conversation handoff event failed");
    }
    completion(std::move(response));
  };
}

} // namespace

class NativeWithdrawalCommitGate
{
public:
  std::mutex mutex;
  std::weak_ptr<NativeLocalWithdrawalState> parent;
  bool cancelled = false;
  bool commitReleased = false;
};

class NativeLocalWithdrawalState
{
public:
  struct Entry
  {
    NativeWithdrawalIntentV1 intent;
    ndn::Buffer proofWire;
    std::string savedOperationId;
    std::string confirmationDigest;
    bool localSaved = false;
    bool confirmed = false;
  };
  using Key = std::tuple<std::string, std::string, std::string>;

  explicit NativeLocalWithdrawalState(std::string id_, NativeWithdrawalOptionsV1 options_,
    ConversationStateStore::SaveFence fence_)
    : id(std::move(id_)), options(std::move(options_)), fence(std::move(fence_))
  {
  }

  bool cancel() noexcept
  {
    std::unique_lock<std::mutex> gateLock;
    if (hostCommitGate) {
      gateLock = std::unique_lock<std::mutex>(hostCommitGate->mutex);
      if (hostCommitGate->commitReleased)
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (outcome || settling || commitReleased || closing)
      return false;
    if (hostCommitGate)
      hostCommitGate->cancelled = true;
    cancellationRequested = true;
    changed.notify_all();
    bool cancelled = true;
    for (auto& child : children)
      cancelled = child.cancel() && cancelled;
    return cancelled;
  }

  void finish(NativeWithdrawalStatusV1 terminal, NativeWithdrawalResultV1 result,
              bool retainFence = false)
  {
    ConversationStateStore::SaveFence released;
    std::vector<ConversationStateStore::SaveFence> releasedParents;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (settling || outcome)
        return;
      settling = true;
      outcome = std::move(result);
      isolated = retainFence;
      if (!retainFence) {
        released = std::move(fence);
        releasedParents = std::move(providerFences);
      }
    }
    released = {};
    releasedParents.clear();
    {
      std::lock_guard<std::mutex> lock(mutex);
      phase = terminal;
      published = true;
    }
    changed.notify_all();
    if (worker)
      worker->close();
  }

  bool isTerminal() const
  {
    std::lock_guard<std::mutex> lock(mutex);
    return published && !isolated;
  }

  std::string id;
  NativeWithdrawalOptionsV1 options;
  ConversationStateStore::SaveFence fence;
  std::vector<ConversationStateStore::SaveFence> providerFences;
  std::shared_ptr<ndn_service_framework::OperationRuntime> worker;
  mutable std::mutex mutex;
  std::condition_variable changed;
  NativeWithdrawalStatusV1 phase = NativeWithdrawalStatusV1::Quiescing;
  std::optional<NativeWithdrawalResultV1> outcome;
  std::map<Key, Entry> entries;
  std::vector<NativeWithdrawalHandle> children;
  std::shared_ptr<NativeWithdrawalCommitGate> hostCommitGate;
  std::shared_ptr<NativeWithdrawalWireBudget> wireBudget;
  std::string failureReason;
  bool cancellationRequested = false;
  bool commitReleased = false;
  bool closing = false;
  bool settling = false;
  bool published = false;
  bool queryEnabled = false;
  bool isolated = false;
};

// One served native service of the host. host->targets keeps at most one
// entry per serviceName: a closed target survives as the draining record
// until a same-name re-serve replaces it (closing-service semantics of the
// native-provider lifecycle design).  The record lives as long as the map
// entry, in-flight router copies, or a registration handle hold it.
struct Target
{
  std::shared_ptr<ServiceProvider::ServiceRegistration> core;
  NativeProviderCollaborationRuntime runtime;
  std::string withdrawalPersistencePolicyId;
  std::mutex withdrawalMutex;
  std::shared_ptr<NativeLocalWithdrawalState> withdrawal;
  ServiceProvider::ServiceRegistration::TargetedHandlerDrainWaiter
    targetedHandlerDrain;
  std::shared_ptr<ExecutionLeaseService> lease;
  std::shared_ptr<std::atomic<bool>> draining;
  std::shared_ptr<std::atomic<bool>> admissionStopped;
  std::shared_ptr<NativeProviderHandoffRegistry> handoffRegistry;
  std::function<NativeWithdrawalHandle(
    const NativeWithdrawalOptionsV1&,
    ConversationStateStore::SaveFence, std::string,
    std::shared_ptr<NativeWithdrawalCommitGate>,
    std::shared_ptr<NativeWithdrawalWireBudget>)> withdrawPrepared;
};

// Host singleton state (see NativeInferenceProvider.hpp). Lease resolvers use
// weak references so the host's target graph cannot form a self-cycle through
// ExecutionLeaseService. Core handlers retain the host while their runtime
// state may still refer to the host-owned lease table.
struct HostState
{
  std::string providerName;
  std::string providerBootId;
  std::size_t workerSlots = 1;
  std::shared_ptr<SharedExecutionLeaseState> sharedLease;
  std::shared_ptr<ServiceProvider::ServiceRegistration> fixedLease;
  std::mutex routerMutex;
  std::map<std::string, std::shared_ptr<Target>> targets;
  std::shared_ptr<NativeLocalWithdrawalState> withdrawal;
  bool withdrawalClosed = false;
};

struct NativeServiceRegistration::State
{
  std::string serviceName;
  std::string withdrawalPersistencePolicyId;
  std::string providerName;
  std::string providerBootId;
  std::shared_ptr<ServiceProvider::ServiceRegistration> core;
  NativeProviderCollaborationRuntime runtime;
  NativeConversationHandoffOwnerContextResolverV1 ownerContextResolver;
  std::weak_ptr<ServiceProvider> producer;
  ServiceProvider::ServiceRegistration::TargetedHandlerDrainWaiter
    targetedHandlerDrain;
  std::shared_ptr<std::atomic<bool>> draining;
  std::shared_ptr<NativeProviderHandoffRegistry> handoffRegistry;
  std::weak_ptr<Target> target;
  std::mutex withdrawalMutex;
  std::shared_ptr<NativeLocalWithdrawalState> withdrawal;
};

namespace {

std::string
newWithdrawalId()
{
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    throw std::runtime_error("WITHDRAWAL_ID_GENERATION_FAILED");
  static constexpr char digits[] = "0123456789abcdef";
  std::string id = "withdrawal-";
  id.reserve(id.size() + bytes.size() * 2);
  for (const auto byte : bytes) {
    id.push_back(digits[byte >> 4]);
    id.push_back(digits[byte & 15]);
  }
  return id;
}

bool
sameWithdrawalOptions(const NativeWithdrawalOptionsV1& a,
                      const NativeWithdrawalOptionsV1& b)
{
  return a.deadline == b.deadline && a.drainDeadline == b.drainDeadline &&
    a.failurePolicy == b.failurePolicy && a.maxConversations == b.maxConversations &&
    a.maxTotalBytes == b.maxTotalBytes &&
    a.persistencePolicyId == b.persistencePolicyId;
}

constexpr std::size_t MAX_PLACEMENT_PROBE_REQUEST_BYTES = 4096;
constexpr std::size_t MAX_PLACEMENT_PROBE_RESPONSE_BYTES = NATIVE_PLACEMENT_PROBE_RESPONSE_MAX_BYTES;
constexpr std::size_t MAX_PLACEMENT_PROBE_ROSTER = 17;
constexpr std::size_t MAX_PLACEMENT_PROBE_EDGES = 16;
constexpr std::size_t MAX_PLACEMENT_PROBE_RESPONSE_ROWS = 16;
constexpr std::uint64_t MAX_PLACEMENT_PROBE_BUDGET_MS = 5000;
constexpr std::uint64_t MAX_PLACEMENT_PROBE_SAMPLE_AGE_MS = 5000;

bool
isCanonicalProviderName(const std::string& value)
{
  if (value.empty() || value.front() != '/') {
    return false;
  }
  try {
    const ndn::Name name(value);
    return !name.empty() &&
      (name.toUri(ndn::name::UriFormat::CANONICAL) == value ||
       name.toUri(ndn::name::UriFormat::ALTERNATE) == value);
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
isSha256Digest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char ch) {
      return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

std::uint64_t
readPlacementProbeUint(const NativeJson& value, const char* field)
{
  if (!value.is_object() || !value.contains(field)) {
    throw std::invalid_argument("missing placement probe integer");
  }
  const auto& item = value.at(field);
  if (item.is_number_unsigned()) {
    return item.get<std::uint64_t>();
  }
  if (!item.is_number_integer() || item.get<std::int64_t>() < 0) {
    throw std::invalid_argument("invalid placement probe integer");
  }
  return static_cast<std::uint64_t>(item.get<std::int64_t>());
}

std::string
readPlacementProbeString(const NativeJson& value, const char* field)
{
  if (!value.is_object() || !value.contains(field) ||
      !value.at(field).is_string()) {
    throw std::invalid_argument("missing placement probe string");
  }
  return value.at(field).get<std::string>();
}

void
requirePlacementProbeKeys(const NativeJson& value,
                          std::initializer_list<const char*> expected)
{
  const auto hintFields = value.is_object() && value.contains("peer_boot_epochs") ? 1 : 0;
  if (!value.is_object() || value.size() != expected.size() + hintFields) {
    throw std::invalid_argument("placement probe request has wrong fields");
  }
  for (const auto* field : expected) {
    if (!value.contains(field)) {
      throw std::invalid_argument("placement probe request is incomplete");
    }
  }
}

using PlacementProbePair = std::pair<std::string, std::string>;

bool
containsPlacementProbeName(const std::set<std::string>& names, const ndn::Name& expected)
{
  return std::any_of(names.begin(), names.end(), [&expected](const auto& value) {
    return ndn::Name(value) == expected;
  });
}

std::vector<PlacementProbePair>
readPlacementProbePairs(const NativeJson& value, const char* field,
                        const std::string& localProvider,
                        const std::set<std::string>& roster,
                        bool rttPairs)
{
  if (!value.is_object() || !value.contains(field) ||
      !value.at(field).is_array() || value.at(field).size() >
        MAX_PLACEMENT_PROBE_EDGES) {
    throw std::invalid_argument("placement probe edge list exceeds bounds");
  }
  std::vector<PlacementProbePair> pairs;
  pairs.reserve(value.at(field).size());
  std::set<std::pair<ndn::Name, ndn::Name>> identities;
  for (const auto& item : value.at(field)) {
    if (!item.is_object() || item.size() != 2 ||
        !item.contains("src_provider") || !item.contains("dst_provider") ||
        !item.at("src_provider").is_string() ||
        !item.at("dst_provider").is_string()) {
      throw std::invalid_argument("placement probe edge is malformed");
    }
    const auto source = item.at("src_provider").get<std::string>();
    const auto destination = item.at("dst_provider").get<std::string>();
    if (!isCanonicalProviderName(source) ||
        !isCanonicalProviderName(destination) || ndn::Name(source) == ndn::Name(destination) ||
        !containsPlacementProbeName(roster, ndn::Name(source)) ||
        !containsPlacementProbeName(roster, ndn::Name(destination)) ||
        (rttPairs ? ndn::Name(source) != ndn::Name(localProvider) :
                    ndn::Name(destination) != ndn::Name(localProvider)) ||
        !identities.emplace(ndn::Name(source), ndn::Name(destination)).second) {
      throw std::invalid_argument("placement probe edge is outside its scope");
    }
    const PlacementProbePair pair{source, destination};
    if (!pairs.empty() && !(pairs.back() < pair)) {
      throw std::invalid_argument("placement probe edges are not canonical");
    }
    pairs.push_back(pair);
  }
  return pairs;
}

NativeJson
placementProbePairsJson(const std::vector<PlacementProbePair>& pairs)
{
  NativeJson result = NativeJson::array();
  for (const auto& pair : pairs) {
    result.push_back(NativeJson{{"src_provider", pair.first},
                                {"dst_provider", pair.second}});
  }
  return result;
}

NativeJson
placementProbeScopeIdentity(const std::string& requestId,
                            std::uint64_t attempt,
                            const std::string& requester,
                            const std::string& observer,
                            const std::string& service,
                            const std::vector<std::string>& roster,
                            const std::string& rosterDigest,
                            const std::vector<PlacementProbePair>& rttPairs,
                            const std::vector<PlacementProbePair>& goodputEdges,
                            std::uint64_t deadlineMs,
                            const std::string& intentNonce)
{
  return NativeJson{
    {"schema", "ndnsf-di-placement-probe-scope-v1"},
    {"request_id", requestId}, {"attempt", attempt},
    {"requester_identity", requester}, {"observer_provider", observer},
    {"service", service}, {"provider_roster", roster},
    {"roster_digest", rosterDigest},
    {"rtt_pairs", placementProbePairsJson(rttPairs)},
    {"goodput_edges", placementProbePairsJson(goodputEdges)},
    {"deadline_ms", deadlineMs}, {"intent_nonce", intentNonce}};
}

NativeJson
placementProbeObservationJson(
  const ndn_service_framework::NetworkTelemetrySnapshot& sample,
  std::uint64_t deadlineMs)
{
  const auto observedAt = sample.lastUpdatedMs;
  const auto sampleExpiry = observedAt >
      std::numeric_limits<std::uint64_t>::max() - MAX_PLACEMENT_PROBE_SAMPLE_AGE_MS
    ? std::numeric_limits<std::uint64_t>::max()
    : observedAt + MAX_PLACEMENT_PROBE_SAMPLE_AGE_MS;
  const auto expiresAt = std::min(deadlineMs, sampleExpiry);
  const bool isRtt = sample.kind ==
    ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeRtt;
  const double value = isRtt ? sample.rttMs : sample.goodputMbps;
  if ((!isRtt && sample.kind !=
         ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeGoodput) ||
      !std::isfinite(value) || value < 0 || observedAt == 0 || expiresAt < observedAt) {
    throw std::invalid_argument("placement probe sample is not encodable");
  }
  // RTT has no packet-train measurements. Keep its authenticated identity and
  // freshness, without spending the bounded response on empty goodput fields.
  if (isRtt) {
    return NativeJson{
      {"observer", sample.providerName.toUri()},
      {"target", sample.peerName.toUri()}, {"kind", "active_probe_rtt"},
      {"value", value}, {"observed_at_ms", observedAt},
      {"expires_at_ms", expiresAt}, {"peer_boot_epoch", sample.peerBootEpoch},
      {"data_name", sample.dataName.toUri()},
      {"target_signature_validated", sample.targetSignatureValidated}};
  }
  NativeJson probeRtt = NativeJson::array();
  for (const auto value : sample.probeRttMs)
    probeRtt.push_back(value);
  NativeJson interestGaps = NativeJson::array();
  for (const auto value : sample.probeInterestGapsMs)
    interestGaps.push_back(value);
  NativeJson dataArrivalGaps = NativeJson::array();
  for (const auto value : sample.probeDataArrivalGapsMs)
    dataArrivalGaps.push_back(value);
  NativeJson probeWireBytes = NativeJson::array();
  for (const auto value : sample.probeDataWireBytes)
    probeWireBytes.push_back(value);
  NativeJson result{
    {"observer", sample.providerName.toUri()},
    {"target", sample.peerName.toUri()},
    {"kind", isRtt ? "active_probe_rtt" : "active_probe_goodput"},
    {"value", value}, {"observed_at_ms", observedAt},
    {"expires_at_ms", expiresAt},
    {"payload_bytes", sample.payloadBytes}, {"wire_bytes", sample.wireBytes},
    {"elapsed_ms", sample.elapsedMs},
    {"peer_boot_epoch", sample.peerBootEpoch},
    {"data_name", sample.dataName.toUri()},
    {"train_complete", sample.probeComplete},
    {"target_signature_validated", sample.targetSignatureValidated},
    {"probe_profile_validated", sample.probeSignatureValid},
    {"received_segments", sample.receivedSegments},
    {"sent_segments", sample.probeSegmentsSent},
    {"probe_rtt_ms", std::move(probeRtt)},
    {"probe_interest_gaps_ms", std::move(interestGaps)},
    {"probe_data_arrival_gaps_ms", std::move(dataArrivalGaps)},
    {"probe_data_wire_bytes", std::move(probeWireBytes)},
    {"profile_digest", sample.probeProfileDigest}};
  result.update(nativeProbeTrainRateMetadataV4(sample));
  return result;
}

void
dropPlacementProbeRow(ndnsf::di::ServiceProvider::PeerProbeCollection& collection,
                      const ndn_service_framework::NetworkTelemetrySnapshot& row)
{
  if (row.kind == ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeRtt) {
    ++collection.omittedTargetCount;
  }
  else if (row.kind ==
           ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeGoodput) {
    ++collection.omittedGoodputEdgeCount;
  }
}

bool
isUsablePlacementProbeRow(
  const ndn_service_framework::NetworkTelemetrySnapshot& sample)
{
  if (sample.kind == ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeRtt) {
    return sample.targetSignatureValidated && std::isfinite(sample.rttMs) &&
           sample.rttMs > 0;
  }
  if (sample.kind ==
      ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeGoodput) {
    return sample.probeComplete && sample.targetSignatureValidated &&
           sample.probeSegmentsSent == ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS &&
           sample.receivedSegments == ndn_service_framework::NETWORK_PROBE_WARMED_SEGMENTS && sample.payloadBytes > 0 &&
           !sample.probeProfileDigest.empty() && std::isfinite(sample.goodputMbps) &&
           sample.goodputMbps > 0 && std::isfinite(sample.elapsedMs) &&
           sample.elapsedMs > 0;
  }
  return false;
}

NativeJson
placementProbeResponseJson(
  const std::string& requestId, std::uint64_t attempt,
  const std::string& requester, const std::string& observer,
  const std::string& service, const std::string& observerBootEpoch,
  const std::string& scopeDigest, const std::string& intentNonce,
  std::uint64_t deadlineMs, std::uint64_t completedAtMs,
  const ndnsf::di::ServiceProvider::PeerProbeCollection& collection,
  const std::map<ndn::Name, std::uint64_t>& originalExpiries = {},
  bool cacheOnly = false)
{
  NativeJson observations = NativeJson::array();
  for (const auto& sample : collection.observations) {
    const auto expiry = originalExpiries.find(sample.dataName);
    if (cacheOnly && expiry == originalExpiries.end())
      throw std::invalid_argument("cached probe observation has no original expiry");
    observations.push_back(placementProbeObservationJson(sample,
      cacheOnly ? std::min(deadlineMs, expiry->second) : deadlineMs));
  }
  NativeJson response{
    {"schema", "ndnsf-di-placement-probe-response-v1"},
    {"request_id", requestId}, {"attempt", attempt},
    {"requester_identity", requester}, {"observer_provider", observer},
    {"service", service}, {"observer_boot_epoch", observerBootEpoch},
    {"scope_digest", scopeDigest}, {"intent_nonce", intentNonce},
    {"deadline_ms", deadlineMs}, {"completed_at_ms", completedAtMs},
    {"requested_rtt_count", collection.requestedTargetCount},
    {"omitted_rtt_count", collection.omittedTargetCount},
    {"requested_goodput_count", collection.requestedGoodputEdgeCount},
    {"omitted_goodput_count", collection.omittedGoodputEdgeCount},
    {"observations", std::move(observations)}};
  if (cacheOnly) {
    response["cache_complete"] = collection.omittedTargetCount == 0 &&
      collection.omittedGoodputEdgeCount == 0 &&
      collection.observations.size() ==
        collection.requestedTargetCount + collection.requestedGoodputEdgeCount;
  }
  return response;
}

uint64_t
hostEpochMs()
{
  const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
  const auto ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  return static_cast<uint64_t>(std::max<long long>(0, ms));
}

ndn_service_framework::ResponseMessage
encodePlacementProbeCollection(
  const NativeJson& request, const std::string& providerBootEpoch,
  ndnsf::di::ServiceProvider::PeerProbeCollection collection,
  const std::map<ndn::Name, std::uint64_t>& originalExpiries = {},
  bool cacheOnly = false)
{
  ndn_service_framework::ResponseMessage response;
  try {
    const auto deadlineMs = request.at("deadline_ms").get<std::uint64_t>();
    const auto completedAtMs = hostEpochMs();
    if (completedAtMs >= deadlineMs) {
      response.setStatus(false);
      response.setErrorInfo("placement probe deadline expired");
      return response;
    }
    for (auto it = collection.observations.begin(); it != collection.observations.end();) {
      const auto expiry = originalExpiries.find(it->dataName);
      const bool staleCache = cacheOnly &&
        (expiry == originalExpiries.end() || expiry->second <= completedAtMs ||
         it->lastUpdatedMs == 0 || it->lastUpdatedMs > completedAtMs ||
         completedAtMs - it->lastUpdatedMs >=
           ndn_service_framework::NETWORK_PROBE_CACHE_MAX_AGE_MS);
      if (staleCache || !isUsablePlacementProbeRow(*it)) {
        dropPlacementProbeRow(collection, *it);
        it = collection.observations.erase(it);
      }
      else ++it;
    }
    const auto dropLast = [&] {
      dropPlacementProbeRow(collection, collection.observations.back());
      collection.observations.pop_back();
    };
    while (collection.observations.size() > MAX_PLACEMENT_PROBE_RESPONSE_ROWS)
      dropLast();
    const auto encode = [&] {
      return nativeCanonicalJson(placementProbeResponseJson(
        request.at("request_id").get<std::string>(),
        request.at("attempt").get<std::uint64_t>(),
        request.at("requester_identity").get<std::string>(),
        request.at("observer_provider").get<std::string>(),
        request.at("service").get<std::string>(), providerBootEpoch,
        request.at("scope_digest").get<std::string>(),
        request.at("intent_nonce").get<std::string>(), deadlineMs, completedAtMs,
        collection, originalExpiries, cacheOnly));
    };
    auto encoded = encode();
    while (encoded.size() > MAX_PLACEMENT_PROBE_RESPONSE_BYTES &&
           !collection.observations.empty()) {
      dropLast();
      encoded = encode();
    }
    if (encoded.size() > MAX_PLACEMENT_PROBE_RESPONSE_BYTES) {
      response.setStatus(false);
      response.setErrorInfo("placement probe response exceeds the byte limit");
    }
    else {
      ndn::Buffer bytes(reinterpret_cast<const std::uint8_t*>(encoded.data()),
                        encoded.size());
      response.setStatus(true);
      response.setPayload(bytes, bytes.size());
    }
  }
  catch (const std::exception&) {
    response.setStatus(false);
    response.setErrorInfo("placement probe result could not be encoded");
  }
  return response;
}

// Host-wide compute-slot resolver: scan the host's boot-time slot range
// (host resources, never grown by later serves) and return the first slot
// without an active conflict key. An empty result means the host is
// saturated.  All targets of the same host serialize through the shared
// prepare mutex inside ExecutionLeaseService.
ExecutionLeaseService::ConflictKeyResolver
makeHostSlotResolver(const std::shared_ptr<HostState>& host)
{
  const std::weak_ptr<HostState> weakHost = host;
  return [weakHost](const LeaseOperationRequest&,
                    const ExecutionLeaseRequestContext&) {
    const auto host = weakHost.lock();
    if (!host) {
      return std::vector<std::string>{};
    }
    const auto now = hostEpochMs();
    for (std::size_t slot = 0; slot < host->workerSlots; ++slot) {
      const auto key =
        host->providerName + ":compute-slot:" + std::to_string(slot);
      if (!host->sharedLease->table.hasActiveConflictKey(key, now)) {
        return std::vector<std::string>{key};
      }
    }
    return std::vector<std::string>{};
  };
}

ServiceProvider::AckStrategyHandler
acceptingAckHandler(std::string message)
{
  return [message = std::move(message)](
           const ndn_service_framework::RequestMessage&) {
    ServiceProvider::AckDecision decision;
    decision.status = true;
    decision.message = message;
    return decision;
  };
}

bool
isNativeConversationHandoffBootstrap(
  const ndn_service_framework::RequestMessage& request)
{
  if (request.getRequestMode() != ndn_service_framework::tlv::TargetedBootstrapRequest)
    return false;
  if (request.hasRequestCapabilities()) {
    const auto& capabilities = request.getRequestCapabilities();
    if (request.getPayload().empty() &&
        capabilities.hasField("RequestScopedConfidentialityV1") &&
        capabilities.getField("RequestScopedConfidentialityV1") == "required" &&
        ((capabilities.hasField("NativeConversationHandoffV1") &&
          capabilities.getField("NativeConversationHandoffV1") == "required") ||
         (capabilities.hasField("NativeConversationHandoffV2") &&
          capabilities.getField("NativeConversationHandoffV2") == "required"))) {
      return true;
    }
  }
  const auto payload = request.getPayload();
  if (payload.empty() || payload.size() > (128U << 10))
    return false;
  try {
    const std::string wire(payload.begin(), payload.end());
    (void)nativeConversationHandoffControlV1FromJson(wire);
    return true;
  }
  catch (const std::exception&) {
    try {
      const std::string wire(payload.begin(), payload.end());
      (void)nativeConversationHandoffControlV2FromJson(wire);
      return true;
    }
    catch (const std::exception&) {
      try {
        const std::string wire(payload.begin(), payload.end());
        (void)nativeWithdrawalQueryV1FromJson(wire);
        return true;
      }
      catch (const std::exception&) {
        try {
          const std::string wire(payload.begin(), payload.end());
          (void)nativeWithdrawalConfirmV1FromJson(wire);
          return true;
        }
        catch (const std::exception&) {
          return false;
        }
      }
    }
  }
}

// Fixed lease entry handler: route one decoded lease operation to the target
// named by request.targetServiceName. An unknown target and a draining
// target are answered here without reaching any ExecutionLeaseService; the
// target's own instance still re-checks the binding for every operation it
// accepts. No exception ever crosses the Core callback.
ServiceProvider::RequestHandler
makeLeaseRouter(const std::shared_ptr<HostState>& host)
{
  return [host](const ndn::Name& requesterIdentity,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const ndn_service_framework::RequestMessage& request) {
    const auto wirePayload = request.getPayload();
    const std::string payload(
      reinterpret_cast<const char*>(wirePayload.data()), wirePayload.size());
    const ExecutionLeaseRequestContext context{
      requesterIdentity.toUri(), providerName.toUri(), serviceName.toUri(),
      requestId.toUri()};
    const auto now = hostEpochMs();
    // Errors travel inside the encoded LeaseOperationResponse; the wire
    // Response stays status=true so a lease client always decodes one.
    ndn_service_framework::ResponseMessage response;
    response.setStatus(true);
    auto complete = [&response](const std::string& responsePayload) {
      ndn::Buffer bytes(
        reinterpret_cast<const uint8_t*>(responsePayload.data()),
        responsePayload.size());
      response.setPayload(bytes, bytes.size());
      return response;
    };
    try {
      const auto operation = decodeLeaseOperationRequest(payload);
      std::shared_ptr<Target> target;
      {
        std::lock_guard<std::mutex> lock(host->routerMutex);
        const auto it = host->targets.find(operation.targetServiceName);
        if (it != host->targets.end()) {
          target = it->second;
        }
      }
      if (!target) {
        LeaseOperationResponse failure;
        failure.status = false;
        failure.operation = operation.operation;
        failure.reasonCode = "LEASE_SERVICE_MISMATCH";
        return complete(encodeLeaseOperationResponse(failure));
      }
      // Draining fences stop new Prepare/Commit/Renew; Abort/Release keep
      // routing so in-flight records finish their cleanup.
      if (target->draining->load(std::memory_order_relaxed) &&
          operation.operation != LeaseOperation::Abort &&
          operation.operation != LeaseOperation::Release) {
        LeaseOperationResponse failure;
        failure.status = false;
        failure.operation = operation.operation;
        failure.reasonCode = "LEASE_TARGET_DRAINING";
        return complete(encodeLeaseOperationResponse(failure));
      }
      // All Core identity/state/replay validation happens inside the
      // target's ExecutionLeaseService over the shared host table.
      return complete(target->lease->handle(context, payload, now));
    }
    catch (const std::exception&) {
      // A malformed payload or an unexpected failure is answered in-band.
      LeaseOperationResponse failure;
      failure.status = false;
      failure.operation = LeaseOperation::Prepare;
      failure.reasonCode = "LEASE_INTERNAL_ERROR";
      return complete(encodeLeaseOperationResponse(failure));
    }
  };
}

} // namespace

ServiceProvider::AsyncTargetedRequestHandler
makeNativePlacementMeasurementTargetedHandler(ServiceProvider& provider)
{
  return [&provider](const ndn::Name& requesterIdentity,
                     const ndn::Name& providerName,
                     const ndn::Name& serviceName,
                     const ndn::Name&,
                     const ndn_service_framework::RequestMessage& request,
                     ServiceProvider::AsyncTargetedCompletion complete) {
    if (!complete) {
      return;
    }
    const auto fail = [&complete](const std::string& reason) {
      ndn_service_framework::ResponseMessage response;
      response.setStatus(false);
      response.setErrorInfo(reason);
      complete(std::move(response));
    };
    try {
      if (providerName.empty() || provider.getName() != providerName ||
          request.getTargetProvider() != providerName) {
        fail("placement probe target identity mismatch");
        return;
      }
      const auto payload = request.getPayload();
      if (payload.empty() || payload.size() > MAX_PLACEMENT_PROBE_REQUEST_BYTES) {
        fail("placement probe request exceeds the byte limit");
        return;
      }
      const std::string wire(reinterpret_cast<const char*>(payload.data()),
                             payload.size());
      const auto decoded = nativeParseJson(wire);
      requirePlacementProbeKeys(decoded, {
        "action", "schema", "request_id", "attempt", "requester_identity",
        "observer_provider", "service", "provider_roster", "roster_digest",
        "rtt_pairs", "goodput_edges", "measurement_kinds", "deadline_ms",
        "intent_nonce", "scope_digest"});
      const auto action = readPlacementProbeString(decoded, "action");
      if (action != "authorize" && action != "measure" && action != "cache") {
        throw std::invalid_argument("unsupported placement probe action");
      }
      if (action == "authorize" && decoded.contains("peer_boot_epochs")) {
        throw std::invalid_argument("probe epoch hints are measure-only metadata");
      }
      if (readPlacementProbeString(decoded, "schema") !=
          "ndnsf-di-placement-probe-request-v1") {
        throw std::invalid_argument("unsupported placement probe request schema");
      }

      const auto requestId = readPlacementProbeString(decoded, "request_id");
      const auto requester = readPlacementProbeString(decoded, "requester_identity");
      const auto observer = readPlacementProbeString(decoded, "observer_provider");
      const auto service = readPlacementProbeString(decoded, "service");
      const auto rosterDigest = readPlacementProbeString(decoded, "roster_digest");
      const auto intentNonce = readPlacementProbeString(decoded, "intent_nonce");
      const auto scopeDigest = readPlacementProbeString(decoded, "scope_digest");
      const auto attempt = readPlacementProbeUint(decoded, "attempt");
      const auto deadlineMs = readPlacementProbeUint(decoded, "deadline_ms");
      if (!isCanonicalProviderName(requestId) ||
          !isCanonicalProviderName(requester) || ndn::Name(requester) != requesterIdentity ||
          !isCanonicalProviderName(observer) ||
          (action != "authorize" && ndn::Name(observer) != providerName) ||
          (action == "authorize" && ndn::Name(observer) == providerName) ||
          serviceName != ndn::Name(ndn_service_framework::NETWORK_PROBE_SERVICE) ||
          !isCanonicalProviderName(service) ||
          attempt == 0 || intentNonce.size() < 16 || intentNonce.size() > 128 ||
          !isSha256Digest(rosterDigest) || !isSha256Digest(scopeDigest) ||
          !decoded.at("provider_roster").is_array() ||
          decoded.at("provider_roster").size() < 2 ||
          decoded.at("provider_roster").size() > MAX_PLACEMENT_PROBE_ROSTER) {
        throw std::invalid_argument("placement probe request binding is invalid");
      }

      std::vector<std::string> roster;
      roster.reserve(decoded.at("provider_roster").size());
      std::set<std::string> rosterSet;
      std::set<ndn::Name> rosterIdentities;
      std::string previousProvider;
      for (const auto& entry : decoded.at("provider_roster")) {
        if (!entry.is_string()) {
          throw std::invalid_argument("placement probe roster is malformed");
        }
        const auto name = entry.get<std::string>();
        if (!isCanonicalProviderName(name) ||
            (!previousProvider.empty() && previousProvider >= name) ||
            !rosterIdentities.emplace(ndn::Name(name)).second) {
          throw std::invalid_argument("placement probe roster is not canonical");
        }
        previousProvider = name;
        roster.push_back(name);
        rosterSet.insert(name);
      }
      if (!containsPlacementProbeName(rosterSet, ndn::Name(observer)) ||
          !containsPlacementProbeName(rosterSet, providerName) ||
          nativePlanningDigest(nativeCanonicalJson(decoded.at("provider_roster"))) !=
            rosterDigest) {
        throw std::invalid_argument("placement probe roster digest mismatch");
      }

      const auto rttPairs = readPlacementProbePairs(
        decoded, "rtt_pairs", observer, rosterSet, true);
      const auto goodputEdges = readPlacementProbePairs(
        decoded, "goodput_edges", observer, rosterSet, false);
      if (rttPairs.empty() && goodputEdges.empty()) {
        throw std::invalid_argument("placement probe request has no authorized edges");
      }
      std::map<ndn::Name, std::string> peerBootEpochHints;
      if (decoded.contains("peer_boot_epochs")) {
        const auto& hints = decoded.at("peer_boot_epochs");
        std::set<std::string> targets;
        for (const auto& pair : rttPairs) targets.insert(pair.second);
        for (const auto& edge : goodputEdges) targets.insert(edge.first);
        if (!hints.is_object() || hints.size() > MAX_PLACEMENT_PROBE_ROSTER) {
          throw std::invalid_argument("placement probe epoch hints are malformed");
        }
        for (auto it = hints.begin(); it != hints.end(); ++it) {
          if (!isCanonicalProviderName(it.key()) ||
              !containsPlacementProbeName(targets, ndn::Name(it.key())) || !it.value().is_string() ||
              it.value().get_ref<const std::string&>().empty() ||
              it.value().get_ref<const std::string&>().size() > 128) {
            throw std::invalid_argument("placement probe epoch hint is out of scope");
          }
          if (!peerBootEpochHints.emplace(ndn::Name(it.key()), it.value().get<std::string>()).second)
            throw std::invalid_argument("placement probe epoch hint repeats a full Name");
        }
      }
      const auto& kinds = decoded.at("measurement_kinds");
      std::vector<std::string> expectedKinds;
      if (!goodputEdges.empty()) expectedKinds.emplace_back("active_probe_goodput");
      if (!rttPairs.empty()) expectedKinds.emplace_back("active_probe_rtt");
      if (!kinds.is_array() || kinds.size() != expectedKinds.size()) {
        throw std::invalid_argument("placement probe measurement kinds mismatch");
      }
      for (std::size_t i = 0; i < expectedKinds.size(); ++i) {
        if (!kinds[i].is_string() || kinds[i].get<std::string>() != expectedKinds[i]) {
          throw std::invalid_argument("placement probe measurement kinds mismatch");
        }
      }

      const auto expectedScope = placementProbeScopeIdentity(
        requestId, attempt, requester, observer, service, roster, rosterDigest,
        rttPairs, goodputEdges, deadlineMs, intentNonce);
      if (nativePlanningDigest(nativeCanonicalJson(expectedScope)) != scopeDigest) {
        throw std::invalid_argument("placement probe scope digest mismatch");
      }

      if (action == "authorize") {
        const auto& localProvider = providerName;
        const bool isRttTarget = std::any_of(
          rttPairs.begin(), rttPairs.end(), [&localProvider](const auto& pair) {
            return ndn::Name(pair.second) == localProvider;
          });
        const bool isGoodputTarget = std::any_of(
          goodputEdges.begin(), goodputEdges.end(),
          [&localProvider](const auto& edge) {
            return ndn::Name(edge.first) == localProvider;
          });
        if (!isRttTarget && !isGoodputTarget) {
          throw std::invalid_argument(
            "placement probe authorization target is outside requested edges");
        }
      }

      const auto wallNow = std::chrono::system_clock::now();
      const auto wallEpoch = wallNow.time_since_epoch();
      const auto wholeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        wallEpoch);
      const auto nowMs = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, wholeMs.count()));
      if (deadlineMs <= nowMs) {
        fail("placement probe deadline expired");
        return;
      }
      auto remaining = std::chrono::milliseconds(std::min<std::uint64_t>(
        deadlineMs - nowMs, MAX_PLACEMENT_PROBE_BUDGET_MS));
      if (deadlineMs - nowMs <= MAX_PLACEMENT_PROBE_BUDGET_MS) {
        remaining -= std::chrono::duration_cast<std::chrono::milliseconds>(
          wallEpoch - wholeMs);
      }
      if (remaining <= std::chrono::milliseconds::zero()) {
        fail("placement probe deadline expired");
        return;
      }
      const auto collectionStart = std::chrono::steady_clock::now();
      const auto probeDeadline = collectionStart + remaining;
      if (action == "cache") {
        // This authenticated service read cannot authorize a peer, create a
        // collection scope or send probe Interests. Retain original validity.
        ServiceProvider::PeerProbeCollection collection;
        collection.requestedTargetCount = rttPairs.size();
        collection.requestedGoodputEdgeCount = goodputEdges.size();
        std::vector<ndn::Name> peers;
        for (const auto& hint : peerBootEpochHints) peers.push_back(hint.first);
        std::map<ndn::Name, std::uint64_t> originalExpiries;
        std::set<std::pair<ndn_service_framework::NetworkTelemetrySampleKind, ndn::Name>> seen;
        for (const auto& cached : provider.getCachedPeerProbeSamples(peers)) {
          const auto& sample = cached.first;
          const auto hint = peerBootEpochHints.find(sample.peerName);
          const bool isRtt = sample.kind ==
            ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeRtt;
          const auto pair = isRtt
            ? std::make_pair(ndn::Name(observer), sample.peerName)
            : std::make_pair(sample.peerName, ndn::Name(observer));
          const auto& allowed = isRtt ? rttPairs : goodputEdges;
          if (sample.providerName != providerName || hint == peerBootEpochHints.end() ||
              sample.peerBootEpoch != hint->second || cached.second <= nowMs ||
              !std::any_of(allowed.begin(), allowed.end(), [&pair](const auto& edge) {
                return ndn::Name(edge.first) == pair.first && ndn::Name(edge.second) == pair.second;
              }) ||
              originalExpiries.count(sample.dataName) ||
              !seen.emplace(sample.kind, sample.peerName).second)
            continue;
          originalExpiries.emplace(sample.dataName, cached.second);
          collection.observations.push_back(sample);
        }
        const auto rttCount = static_cast<std::size_t>(std::count_if(
          collection.observations.begin(), collection.observations.end(),
          [] (const auto& sample) { return sample.kind ==
            ndn_service_framework::NetworkTelemetrySampleKind::ActiveProbeRtt; }));
        collection.omittedTargetCount = rttPairs.size() - rttCount;
        collection.omittedGoodputEdgeCount = goodputEdges.size() -
          (collection.observations.size() - rttCount);
        complete(encodePlacementProbeCollection(decoded, provider.getProviderBootEpoch(),
          std::move(collection), originalExpiries, true));
        return;
      }
      const auto authorizeScope = [&] {
        return provider.authorizeCandidatePeerObservationScope(
            scopeDigest, [&] {
              std::vector<ndn::Name> names;
              names.reserve(roster.size());
              for (const auto& name : roster) names.emplace_back(name);
              return names;
            }(), [&] {
              std::vector<ndn_service_framework::NetworkTelemetryDataEdge> edges;
              edges.reserve(rttPairs.size());
              for (const auto& pair : rttPairs) {
                edges.push_back({ndn::Name(pair.first), ndn::Name(pair.second)});
              }
              return edges;
            }(), [&] {
              std::vector<ndn_service_framework::NetworkTelemetryDataEdge> edges;
              edges.reserve(goodputEdges.size());
              for (const auto& edge : goodputEdges) {
                edges.push_back({ndn::Name(edge.first), ndn::Name(edge.second)});
              }
              return edges;
            }(), probeDeadline);
      };
      if (!authorizeScope()) {
        fail("placement probe scope rejected");
        return;
      }

      if (action == "authorize") {
        auto authorizationAck = NativeJson{
          {"schema", "ndnsf-di-placement-probe-scope-ack-v1"},
          {"provider", providerName.toUri()},
          {"observer_provider", observer},
          {"scope_digest", scopeDigest},
          {"boot_epoch", provider.getProviderBootEpoch()},
          {"status", "authorized"}};
        auto authorizationWire = nativeCanonicalJson(authorizationAck);
        if (authorizationWire.size() > 512) {
          // Optional reuse metadata must not make a previously fitting scope
          // ACK fail; omit it and retain the original fresh-measurement path.
          authorizationAck.erase("boot_epoch");
          authorizationWire = nativeCanonicalJson(authorizationAck);
        }
        if (authorizationWire.empty() || authorizationWire.size() > 512) {
          fail("placement probe scope acknowledgement exceeds the byte limit");
          return;
        }
        ndnsf::di::logRuntimeEvidence(
          "NDNSF_DI_PROVIDER_PEER_PROBE_SCOPE_AUTHORIZED provider=" +
          providerName.toUri() + " observer=" + observer +
          " scope=" + scopeDigest);
        ndn_service_framework::ResponseMessage response;
        response.setStatus(true);
        ndn::Buffer responsePayload(
          reinterpret_cast<const std::uint8_t*>(authorizationWire.data()),
          authorizationWire.size());
        response.setPayload(responsePayload, responsePayload.size());
        complete(std::move(response));
        return;
      }

      const auto providerBootEpoch = provider.getProviderBootEpoch();
      provider.collectCandidatePeerObservations(
        [&] {
          std::vector<ndn::Name> names;
          names.reserve(roster.size());
          for (const auto& name : roster) names.emplace_back(name);
          return names;
        }(), [&] {
          std::vector<ndn_service_framework::NetworkTelemetryDataEdge> edges;
          edges.reserve(rttPairs.size());
          for (const auto& pair : rttPairs) {
            edges.push_back({ndn::Name(pair.first), ndn::Name(pair.second)});
          }
          return edges;
        }(), [&] {
          std::vector<ndn_service_framework::NetworkTelemetryDataEdge> edges;
          edges.reserve(goodputEdges.size());
          for (const auto& edge : goodputEdges) {
            edges.push_back({ndn::Name(edge.first), ndn::Name(edge.second)});
          }
          return edges;
        }(), scopeDigest, probeDeadline, {},
        [probeRequest = decoded, providerBootEpoch, complete = std::move(complete)]
        (ServiceProvider::PeerProbeCollection collection) mutable {
          complete(encodePlacementProbeCollection(
            probeRequest, providerBootEpoch, std::move(collection)));
        }, true, peerBootEpochHints);
    }
    catch (const std::exception&) {
      fail("invalid placement probe request");
    }
  };
}

NativeServiceRegistration::NativeServiceRegistration(std::shared_ptr<State> state,
                                                     bool closeOnDestroy)
  : m_state(std::move(state)), m_closeOnDestroy(closeOnDestroy)
{
}

NativeWithdrawalHandle::NativeWithdrawalHandle(
  std::shared_ptr<NativeLocalWithdrawalState> state)
  : m_state(std::move(state))
{
}

bool
NativeWithdrawalHandle::valid() const noexcept
{
  return static_cast<bool>(m_state);
}

const std::string&
NativeWithdrawalHandle::operationId() const noexcept
{
  static const std::string empty;
  return m_state ? m_state->id : empty;
}

NativeWithdrawalStatusV1
NativeWithdrawalHandle::status() const
{
  if (!m_state)
    throw std::logic_error("WITHDRAWAL_HANDLE_INVALID");
  std::lock_guard<std::mutex> lock(m_state->mutex);
  return m_state->phase;
}

NativeWithdrawalResultV1
NativeWithdrawalHandle::result(std::chrono::milliseconds waitTimeout) const
{
  if (!m_state)
    throw std::logic_error("WITHDRAWAL_HANDLE_INVALID");
  if (waitTimeout.count() < 0)
    throw std::invalid_argument("WITHDRAWAL_WAIT_TIMEOUT_INVALID");
  if (m_state->worker && m_state->worker->isWorkerThread())
    throw std::logic_error("WITHDRAWAL_WORKER_CANNOT_WAIT_ON_ITSELF");
  std::unique_lock<std::mutex> lock(m_state->mutex);
  if (!m_state->changed.wait_for(lock, waitTimeout,
        [this] { return m_state->published; }))
    throw ndn_service_framework::OperationError(
      ndn_service_framework::OperationErrorCode::Timeout,
      "WITHDRAWAL_WAIT_TIMEOUT");
  return *m_state->outcome;
}

bool
NativeWithdrawalHandle::cancel() noexcept
{
  return m_state && m_state->cancel();
}

NativeWithdrawalHandle
NativeServiceRegistration::withdraw(const NativeWithdrawalOptionsV1& options)
{
  return withdrawPrepared(options, std::nullopt, {});
}

NativeWithdrawalHandle
NativeServiceRegistration::withdrawPrepared(
  const NativeWithdrawalOptionsV1& options,
  std::optional<ConversationStateStore::SaveFence> preparedFence,
  std::string preparedId,
  std::shared_ptr<NativeWithdrawalCommitGate> parentCommitGate,
  std::shared_ptr<NativeWithdrawalWireBudget> parentWireBudget)
{
  if (!m_state || !m_state->core)
    throw std::logic_error("WITHDRAWAL_SERVICE_NOT_ACTIVE");
  if (m_state->withdrawalPersistencePolicyId.empty())
    throw std::logic_error("WITHDRAWAL_DISABLED");
  if (options.persistencePolicyId != m_state->withdrawalPersistencePolicyId)
    throw std::invalid_argument("WITHDRAWAL_POLICY_ID_MISMATCH");
  const auto now = HandoffClock::now();
  if (options.deadline <= now ||
      options.deadline - now > std::chrono::minutes(10) ||
      options.deadline == HandoffClock::time_point::max() ||
      options.drainDeadline < options.deadline ||
      options.drainDeadline == HandoffClock::time_point::max() ||
      options.maxConversations == 0 ||
      options.maxConversations > std::numeric_limits<std::size_t>::max() / 64 ||
      options.maxTotalBytes == 0 || options.persistencePolicyId.empty())
    throw std::invalid_argument("WITHDRAWAL_OPTIONS_INVALID");
  if (options.failurePolicy != NativeWithdrawalFailurePolicyV1::Abort &&
      options.failurePolicy != NativeWithdrawalFailurePolicyV1::ForceUnsaved)
    throw std::invalid_argument("WITHDRAWAL_FAILURE_POLICY_INVALID");
  if (!m_state->runtime.beginServiceWithdrawalSaveFence ||
      !m_state->runtime.isSaveFenceQuiescent ||
      !m_state->runtime.tryAcquireCommittedSaveBatch)
    throw std::logic_error("WITHDRAWAL_RUNTIME_UNAVAILABLE");

  std::lock_guard<std::mutex> lock(m_state->withdrawalMutex);
  if (m_state->withdrawal && !m_state->withdrawal->isTerminal()) {
    if (!sameWithdrawalOptions(m_state->withdrawal->options, options))
      throw std::logic_error("WITHDRAWAL_OPERATION_CONFLICT");
    return NativeWithdrawalHandle(m_state->withdrawal);
  }
  if (m_state->core->closed() ||
      m_state->draining->load(std::memory_order_relaxed))
    throw std::logic_error("WITHDRAWAL_SERVICE_NOT_ACTIVE");
  const auto id = preparedId.empty() ? newWithdrawalId() : std::move(preparedId);
  const auto generation = m_state->core->generation();
  if (generation == 0)
    throw std::logic_error("WITHDRAWAL_GENERATION_UNAVAILABLE");
  auto fence = preparedFence
    ? std::move(preparedFence)
    : m_state->runtime.beginServiceWithdrawalSaveFence(
        m_state->serviceName, id, generation);
  if (!fence || !fence->valid())
    throw std::logic_error("WITHDRAWAL_SCOPE_BUSY");
  if (!m_state->runtime.matchesServiceWithdrawalSaveFence ||
      !m_state->runtime.matchesServiceWithdrawalSaveFence(
        *fence, m_state->serviceName, id, generation))
    throw std::logic_error("WITHDRAWAL_SCOPE_BINDING_MISMATCH");
  auto operation = std::make_shared<NativeLocalWithdrawalState>(
    id, options, std::move(*fence));
  operation->wireBudget = parentWireBudget ? std::move(parentWireBudget)
    : std::make_shared<NativeWithdrawalWireBudget>(options.maxTotalBytes);
  operation->worker = ndn_service_framework::OperationRuntime::create();
  operation->worker->deferJoinOnDestroy();
  auto ticket = operation->worker->acquire();
  m_state->withdrawal = operation;
  auto target = m_state->target.lock();
  if (target) {
    std::lock_guard<std::mutex> targetLock(target->withdrawalMutex);
    target->withdrawal = operation;
  }
  try {
    if (!m_state->handoffRegistry)
      throw std::logic_error("WITHDRAWAL_TARGETED_HANDLER_UNAVAILABLE");
    const std::weak_ptr<NativeLocalWithdrawalState> weakOperation = operation;
    m_state->handoffRegistry->setLocalWithdrawalFenceResolver(
      [weakOperation](const ndn::Name& requester, const ndn::Name& service,
                      const NativeConversationHandoffControlV2& event)
        -> std::optional<ConversationStateStore::SaveFence> {
        const auto active = weakOperation.lock();
        if (!active || !event.withdrawalIntent)
          return std::nullopt;
        const auto key = NativeLocalWithdrawalState::Key{
          handoffCanonicalName(requester), handoffCanonicalName(service),
          event.control.conversationId};
        std::lock_guard<std::mutex> guard(active->mutex);
        if (active->published || active->cancellationRequested ||
            !active->queryEnabled ||
            (active->phase != NativeWithdrawalStatusV1::AwaitingOwners &&
             active->phase != NativeWithdrawalStatusV1::Saving) ||
            HandoffClock::now() >= active->options.deadline)
          return std::nullopt;
        const auto found = active->entries.find(key);
        if (found == active->entries.end() ||
            nativeWithdrawalIntentV1ToJson(found->second.intent) !=
              nativeWithdrawalIntentV1ToJson(*event.withdrawalIntent) ||
            found->second.proofWire != event.withdrawalProofWire)
          return std::nullopt;
        return active->fence;
      });
    m_state->handoffRegistry->setLocalWithdrawalQueryResolver(
      [weakOperation](const ndn::Name& requester, const ndn::Name& service,
                      const NativeWithdrawalQueryV1& query) {
        NativeWithdrawalQueryReplyV1 reply;
        reply.query = query;
        const auto active = weakOperation.lock();
        if (!active)
          return reply;
        const auto key = NativeLocalWithdrawalState::Key{
          handoffCanonicalName(requester), handoffCanonicalName(service),
          query.conversationId};
        std::lock_guard<std::mutex> guard(active->mutex);
        if (active->published || active->cancellationRequested ||
            !active->queryEnabled ||
            (active->phase != NativeWithdrawalStatusV1::AwaitingOwners &&
             active->phase != NativeWithdrawalStatusV1::Saving) ||
            HandoffClock::now() >= active->options.deadline)
          return reply;
        const auto found = active->entries.find(key);
        if (found != active->entries.end() &&
            found->second.intent.checkpointDigest == query.checkpointDigest &&
            query.deadlineMs <= found->second.intent.deadlineMs) {
          reply.status = NativeWithdrawalQueryStatusV1::Intent;
          reply.intent = found->second.intent;
          reply.originalProofWire = found->second.proofWire;
        }
        return reply;
      });
    m_state->handoffRegistry->setLocalWithdrawalStageObservers(
      [weakOperation, parentCommitGate](const ndn::Name& requester, const ndn::Name& service,
                      const HandoffOperation& binding) {
        const auto active = weakOperation.lock();
        if (!active)
          throw std::runtime_error("WITHDRAWAL_OPERATION_GONE");
        std::unique_lock<std::mutex> parentLock;
        if (parentCommitGate) {
          parentLock = std::unique_lock<std::mutex>(parentCommitGate->mutex);
          if (parentCommitGate->cancelled)
            throw std::runtime_error("WITHDRAWAL_PARENT_CANCELLED");
        }
        const auto key = NativeLocalWithdrawalState::Key{
          handoffCanonicalName(requester), handoffCanonicalName(service),
          binding.conversationId};
        {
          std::lock_guard<std::mutex> guard(active->mutex);
          const auto found = active->entries.find(key);
          if (found == active->entries.end() || active->published ||
              active->cancellationRequested ||
              found->second.intent.checkpointDigest != binding.checkpointDigest ||
              found->second.intent.sourceRoleMapDigest != binding.sourceRoleMapDigest ||
              binding.deadlineMs > found->second.intent.deadlineMs ||
              (!found->second.savedOperationId.empty() &&
               found->second.savedOperationId != binding.operationId))
            throw std::runtime_error("WITHDRAWAL_COMMIT_BINDING_MISMATCH");
          found->second.savedOperationId = binding.operationId;
          if (parentCommitGate)
            parentCommitGate->commitReleased = true;
          active->commitReleased = true;
          active->phase = NativeWithdrawalStatusV1::Saving;
          active->changed.notify_all();
        }
        if (parentCommitGate) {
          if (const auto parent = parentCommitGate->parent.lock()) {
            std::lock_guard<std::mutex> guard(parent->mutex);
            if (!parent->outcome && !parent->settling) {
              parent->phase = NativeWithdrawalStatusV1::Saving;
              parent->changed.notify_all();
            }
          }
        }
      },
      [weakOperation](const ndn::Name& requester, const ndn::Name& service,
                      const HandoffOperation& binding) {
        const auto active = weakOperation.lock();
        if (!active)
          throw std::runtime_error("WITHDRAWAL_OPERATION_GONE");
        const auto key = NativeLocalWithdrawalState::Key{
          handoffCanonicalName(requester), handoffCanonicalName(service),
          binding.conversationId};
        std::lock_guard<std::mutex> guard(active->mutex);
        const auto found = active->entries.find(key);
        if (found == active->entries.end() || active->published ||
            found->second.savedOperationId != binding.operationId ||
            found->second.intent.checkpointDigest != binding.checkpointDigest)
          throw std::runtime_error("WITHDRAWAL_SAVED_BINDING_MISMATCH");
        found->second.localSaved = true;
        active->changed.notify_all();
      });
    m_state->handoffRegistry->setLocalWithdrawalBudgetObservers(
      [weakOperation](const ndn::Name& requester, const ndn::Name& service,
                      const HandoffOperation& binding,
                      const NativeDurableStorageGrantRoleChallengeV1& challenge) {
        const auto active = weakOperation.lock();
        if (!active)
          throw std::runtime_error("WITHDRAWAL_OPERATION_GONE");
        const auto key = NativeLocalWithdrawalState::Key{
          handoffCanonicalName(requester), handoffCanonicalName(service),
          binding.conversationId};
        {
          std::lock_guard<std::mutex> guard(active->mutex);
          const auto found = active->entries.find(key);
          if (found == active->entries.end() || active->published ||
              active->cancellationRequested || !active->queryEnabled ||
              found->second.intent.checkpointDigest != binding.checkpointDigest ||
              found->second.intent.sourceRoleMapDigest != binding.sourceRoleMapDigest ||
              (!found->second.savedOperationId.empty() &&
               found->second.savedOperationId != binding.operationId) ||
              std::find(found->second.intent.localRoles.begin(),
                        found->second.intent.localRoles.end(), challenge.roleName) ==
                found->second.intent.localRoles.end())
            throw std::runtime_error("WITHDRAWAL_WIRE_CHALLENGE_BINDING_MISMATCH");
        }
        active->wireBudget->reserve(requester, challenge);
      },
      [weakOperation](const ndn::Name& requester, const ndn::Name& service,
                      const HandoffOperation& binding, const std::string& reason) {
        const auto active = weakOperation.lock();
        if (!active)
          return;
        const auto key = NativeLocalWithdrawalState::Key{
          handoffCanonicalName(requester), handoffCanonicalName(service),
          binding.conversationId};
        std::lock_guard<std::mutex> guard(active->mutex);
        const auto found = active->entries.find(key);
        if (found == active->entries.end() || active->published ||
            found->second.intent.checkpointDigest != binding.checkpointDigest ||
            found->second.intent.sourceRoleMapDigest != binding.sourceRoleMapDigest ||
            found->second.confirmed || !active->failureReason.empty())
          return;
        active->failureReason = reason.empty() ? "HANDOFF_FAILED" : reason;
        active->changed.notify_all();
      });
    m_state->handoffRegistry->setLocalWithdrawalConfirmHandler(
      [weakOperation](const ndn::Name& requester, const ndn::Name& service,
                      const NativeWithdrawalConfirmV1& confirm) {
        const auto active = weakOperation.lock();
        if (!active)
          throw std::runtime_error("WITHDRAWAL_OPERATION_GONE");
        const auto key = NativeLocalWithdrawalState::Key{
          handoffCanonicalName(requester), handoffCanonicalName(service),
          confirm.conversationId};
        const auto digest = nativePlanningDigest(nativeConversationCanonicalJson(
          nativeWithdrawalConfirmV1ToJson(confirm)));
        std::lock_guard<std::mutex> guard(active->mutex);
        const auto found = active->entries.find(key);
        if (found == active->entries.end() || active->published ||
            found->second.intent.withdrawalId != confirm.withdrawalId ||
            found->second.intent.registrationGeneration !=
              confirm.registrationGeneration ||
            found->second.intent.checkpointDigest != confirm.checkpointDigest ||
            found->second.intent.deadlineMs != confirm.deadlineMs ||
            found->second.savedOperationId != confirm.operationId ||
            !found->second.localSaved)
          throw std::runtime_error("WITHDRAWAL_CONFIRM_BINDING_MISMATCH");
        if (found->second.confirmed) {
          if (found->second.confirmationDigest != digest)
            throw std::runtime_error("WITHDRAWAL_CONFIRM_CONFLICT");
        }
        else {
          if (HandoffClock::now() >= active->options.deadline)
            throw std::runtime_error("WITHDRAWAL_CONFIRM_EXPIRED");
          found->second.confirmationDigest = digest;
          found->second.confirmed = true;
          active->changed.notify_all();
        }
        return NativeWithdrawalConfirmReplyV1{confirm};
      });
    operation->worker->post(ticket, [state = m_state, operation] {
      NativeWithdrawalResultV1 result;
      result.withdrawalId = operation->id;
      result.scope = NativeWithdrawalScopeV1::Service;
      const auto abort = [&] (std::string reason) {
        bool hasEntries = false;
        const bool force =
          operation->options.failurePolicy == NativeWithdrawalFailurePolicyV1::ForceUnsaved &&
          reason != "CANCELLED" && reason != "WITHDRAWAL_SERVICE_CLOSED";
        {
          std::lock_guard<std::mutex> guard(operation->mutex);
          operation->queryEnabled = false;
          hasEntries = !operation->entries.empty() ||
                       !result.conversationResults.empty();
          for (auto& item : result.conversationResults) {
            const auto key = NativeLocalWithdrawalState::Key{
              item.ownerIdentity, item.serviceName, item.conversationId};
            const auto found = operation->entries.find(key);
            if (found == operation->entries.end())
              continue;
            item.operationId = found->second.savedOperationId;
            if (found->second.confirmed) {
              item.classification = "CONFIRMED";
              item.reason.clear();
            }
            else if (found->second.localSaved) {
              item.classification = "LOCAL_SAVED";
              item.reason = "OWNER_CONFIRMATION_MISSING";
            }
          }
        }
        operation->changed.notify_all();
        if (force)
          state->draining->store(true, std::memory_order_relaxed);
        if (force && !hasEntries) {
          result.reason = "FORCE_UNSAVED_SCOPE_UNCLASSIFIED";
          operation->finish(NativeWithdrawalStatusV1::Failed,
                            std::move(result), true);
          return;
        }
        if (hasEntries && state->handoffRegistry &&
            !state->handoffRegistry->settleLocalWithdrawal(
              operation->id, operation->options.drainDeadline)) {
          result.reason = "DRAIN_TIMEOUT";
          operation->finish(NativeWithdrawalStatusV1::Failed,
                            std::move(result), true);
          return;
        }
        if (force) {
          const auto drainFailed = [&] {
            result.reason = "DRAIN_TIMEOUT";
            operation->finish(NativeWithdrawalStatusV1::Failed,
                              std::move(result), true);
          };
          while (!state->runtime.isSaveFenceQuiescent(operation->fence)) {
            if (HandoffClock::now() >= operation->options.drainDeadline) {
              drainFailed();
              return;
            }
            std::unique_lock<std::mutex> guard(operation->mutex);
            operation->changed.wait_for(guard, std::chrono::milliseconds(10));
          }
          const auto remaining = [&] {
            const auto now = HandoffClock::now();
            return now >= operation->options.drainDeadline
              ? std::chrono::milliseconds::zero()
              : std::chrono::duration_cast<std::chrono::milliseconds>(
                  operation->options.drainDeadline - now);
          };
          if ((state->handoffRegistry &&
               !state->handoffRegistry->drain(remaining())) ||
              (state->targetedHandlerDrain &&
               !state->targetedHandlerDrain(remaining()))) {
            drainFailed();
            return;
          }
          state->core->close();
          for (auto& item : result.conversationResults) {
            if (item.classification != "CONFIRMED") {
              item.classification = "FORCED_UNSAVED";
              item.reason = reason;
            }
          }
          result.closed = true;
          result.forced = true;
          result.reason = "FORCED_UNSAVED:" + reason;
          operation->finish(NativeWithdrawalStatusV1::ForcedUnsaved,
                            std::move(result));
          return;
        }
        result.reason = std::move(reason);
        operation->finish(NativeWithdrawalStatusV1::Aborted, std::move(result));
      };
      try {
        for (;;) {
          bool cancelled = false;
          {
            std::lock_guard<std::mutex> guard(operation->mutex);
            cancelled = operation->cancellationRequested;
          }
          if (cancelled) {
            abort("CANCELLED");
            return;
          }
          if (state->draining->load(std::memory_order_relaxed) ||
              !state->core || state->core->closed()) {
            abort("WITHDRAWAL_SERVICE_CLOSED");
            return;
          }
          if (HandoffClock::now() >= operation->options.deadline) {
            abort("DEADLINE_EXCEEDED");
            return;
          }
          if (!state->runtime.isSaveFenceQuiescent(operation->fence)) {
            std::unique_lock<std::mutex> guard(operation->mutex);
            operation->changed.wait_for(guard, std::chrono::milliseconds(10),
              [&] { return operation->cancellationRequested; });
            continue;
          }
          const auto maxRows = operation->options.maxConversations * 64;
          const auto maxMetadataBytes = static_cast<std::size_t>(std::min<std::uint64_t>(
            operation->options.maxTotalBytes,
            std::numeric_limits<std::size_t>::max()));
          auto batch = state->runtime.tryAcquireCommittedSaveBatch(
            operation->fence,
            {maxRows, maxMetadataBytes, operation->options.maxTotalBytes},
            handoffNowMs());
          if (batch.status == ConversationSaveBatchStatusV1::Busy) {
            std::unique_lock<std::mutex> guard(operation->mutex);
            operation->changed.wait_for(guard, std::chrono::milliseconds(10),
              [&] { return operation->cancellationRequested; });
            continue;
          }
          if (batch.status == ConversationSaveBatchStatusV1::Empty) {
            {
              std::lock_guard<std::mutex> guard(operation->mutex);
              cancelled = operation->cancellationRequested;
              if (!cancelled)
                operation->closing = true;
            }
            if (cancelled) {
              abort("CANCELLED");
              return;
            }
            state->draining->store(true, std::memory_order_relaxed);
            if (state->handoffRegistry)
              state->handoffRegistry->requestStop();
            state->core->close();
            result.closed = true;
            result.reason = "EMPTY";
            operation->finish(NativeWithdrawalStatusV1::Closed, std::move(result));
            return;
          }
          const bool forceBoundedRows =
            operation->options.failurePolicy == NativeWithdrawalFailurePolicyV1::ForceUnsaved &&
            batch.status == ConversationSaveBatchStatusV1::BoundsExceeded &&
            !batch.affectedRows.empty();
          if (!forceBoundedRows &&
              (batch.status != ConversationSaveBatchStatusV1::Ready || !batch.batch)) {
            abort(batch.status == ConversationSaveBatchStatusV1::BoundsExceeded
                    ? "BUDGET_EXCEEDED" : "UNSUPPORTED_STATE");
            return;
          }
          struct Group
          {
            NativeWithdrawalConversationResultV1 result;
            std::string sourceRoleMapDigest;
            std::vector<std::string> roles;
            std::size_t metadataBytes = 0;
          };
          std::map<NativeLocalWithdrawalState::Key, Group> conversations;
          std::string incompatibleReason;
          const auto& rows = forceBoundedRows ? batch.affectedRows : batch.batch->rows();
          for (const auto& row : rows) {
            row.binding.validate();
            const auto key = std::make_tuple(row.metadata.authenticatedRequester,
              row.binding.serviceName, row.binding.conversationId);
            auto& item = conversations[key];
            if ((!item.result.conversationId.empty() &&
                 (item.result.checkpointDigest != row.binding.checkpointDigest ||
                  item.sourceRoleMapDigest != row.binding.planRoleMapDigest)) ||
                row.binding.serviceName != state->serviceName ||
                row.binding.identity.providerIdentity != state->providerName ||
                row.binding.identity.providerBootId != state->providerBootId ||
                row.metadata.authenticatedRequester.empty() ||
                row.serializedMetadataBytes >
                  std::numeric_limits<std::size_t>::max() - item.metadataBytes) {
              incompatibleReason = "INCOMPATIBLE_STATE";
              break;
            }
            item.result.ownerIdentity = row.metadata.authenticatedRequester;
            item.result.serviceName = row.binding.serviceName;
            item.result.conversationId = row.binding.conversationId;
            item.result.checkpointDigest = row.binding.checkpointDigest;
            item.sourceRoleMapDigest = row.binding.planRoleMapDigest;
            item.roles.push_back(row.binding.identity.roleName);
            item.metadataBytes += row.serializedMetadataBytes;
          }
          batch.batch.reset();
          if (!incompatibleReason.empty()) {
            abort(std::move(incompatibleReason));
            return;
          }
          if (conversations.size() > operation->options.maxConversations) {
            abort("BUDGET_EXCEEDED");
            return;
          }
          if (forceBoundedRows) {
            result.conversationResults.reserve(conversations.size());
            for (auto& [key, group] : conversations) {
              (void)key;
              group.result.classification = "FAILED";
              group.result.reason = "BUDGET_EXCEEDED";
              result.conversationResults.push_back(std::move(group.result));
            }
            abort("BUDGET_EXCEEDED");
            return;
          }
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            operation->options.deadline - HandoffClock::now()).count();
          if (remaining <= 0 || remaining > 10 * 60 * 1000) {
            abort("DEADLINE_EXCEEDED");
            return;
          }
          const auto deadlineMs = handoffNowMs() +
            static_cast<std::uint64_t>(remaining);
          auto producer = state->producer.lock();
          if (!producer || !state->ownerContextResolver)
            throw std::runtime_error("OWNER_UNAVAILABLE");
          std::map<NativeLocalWithdrawalState::Key,
                   NativeLocalWithdrawalState::Entry> prepared;
          std::vector<NativeWithdrawalConversationResultV1> preparedResults;
          preparedResults.reserve(conversations.size());
          for (auto& [key, group] : conversations) {
            std::sort(group.roles.begin(), group.roles.end());
            if (group.roles.empty() || group.roles.size() > 64 ||
                std::adjacent_find(group.roles.begin(), group.roles.end()) !=
                  group.roles.end())
              throw std::runtime_error("INCOMPATIBLE_STATE");
            auto context = resolveNativeConversationHandoffOwnerContextV1(
              state->ownerContextResolver, ndn::Name(group.result.ownerIdentity),
              ndn::Name(group.result.serviceName), operation->id);
            if (static_cast<std::uint64_t>(remaining) >
                  context.policy.maxSaveDurationMs ||
                group.roles.size() > context.saveBatchLimits.maxEntries ||
                group.roles.size() > context.policy.maxRolesPerEvent ||
                group.metadataBytes > context.saveBatchLimits.maxMetadataBytes)
              throw std::runtime_error("BUDGET_EXCEEDED");
            if (context.repoTargetByRole.size() != group.roles.size())
              throw std::runtime_error("WITHDRAWAL_REPO_ROLE_SET_MISMATCH");
            for (const auto& role : group.roles) {
              if (context.repoTargetByRole.count(role) != 1)
                throw std::runtime_error("WITHDRAWAL_REPO_ROLE_SET_MISMATCH");
            }
            NativeWithdrawalIntentV1 intent;
            intent.withdrawalId = operation->id;
            intent.ownerIdentity = group.result.ownerIdentity;
            intent.serviceName = group.result.serviceName;
            intent.providerBootId = state->providerBootId;
            intent.registrationGeneration = state->core->generation();
            intent.scope = NativeWithdrawalScopeV1::Service;
            intent.deadlineMs = deadlineMs;
            intent.conversationId = group.result.conversationId;
            intent.checkpointDigest = group.result.checkpointDigest;
            intent.sourceRoleMapDigest = group.sourceRoleMapDigest;
            intent.localRoles = group.roles;
            intent.policyDigest = context.policy.effectiveDigest;
            const auto proof = nativeSignWithdrawalIntentProofV1(
              ndn::Name(state->providerName), intent,
              [producer, operation](ndn::Data& data) {
                const auto current = [operation] {
                  std::lock_guard<std::mutex> guard(operation->mutex);
                  if (operation->cancellationRequested)
                    throw std::runtime_error("CANCELLED");
                  if (operation->closing || operation->settling || operation->outcome)
                    throw std::runtime_error("WITHDRAWAL_SERVICE_CLOSED");
                  if (HandoffClock::now() >= operation->options.deadline)
                    throw std::runtime_error("DEADLINE_EXCEEDED");
                };
                current();
                if (!producer->signProducerDataUntil(data, operation->options.deadline, current)) {
                  current();
                  throw std::runtime_error("WITHDRAWAL_PRODUCER_SIGNING_FAILED");
                }
              });
            const auto& wire = proof.wireEncode();
            NativeLocalWithdrawalState::Entry entry;
            entry.intent = std::move(intent);
            entry.proofWire.assign(wire.data(), wire.data() + wire.size());
            prepared.emplace(key, std::move(entry));
            group.result.classification = "FAILED";
            group.result.reason = "OWNER_CONFIRMATION_MISSING";
            preparedResults.push_back(std::move(group.result));
          }
          bool cancelledBeforePublish = false;
          {
            std::lock_guard<std::mutex> guard(operation->mutex);
            cancelledBeforePublish = operation->cancellationRequested;
            if (!cancelledBeforePublish) {
              operation->entries = std::move(prepared);
              operation->phase = NativeWithdrawalStatusV1::AwaitingOwners;
              result.conversationResults = std::move(preparedResults);
              operation->queryEnabled = true;
            }
          }
          if (cancelledBeforePublish) {
            abort("CANCELLED");
            return;
          }
          operation->changed.notify_all();
          std::unique_lock<std::mutex> guard(operation->mutex);
          const auto allConfirmed = [&] {
            return std::all_of(operation->entries.begin(), operation->entries.end(),
              [](const auto& entry) { return entry.second.confirmed; });
          };
          operation->changed.wait_until(guard, operation->options.deadline,
            [&] { return operation->cancellationRequested ||
              !operation->failureReason.empty() || allConfirmed(); });
          if (allConfirmed()) {
            operation->phase = NativeWithdrawalStatusV1::Confirmed;
            operation->changed.notify_all();
            // Keep the authenticated confirmation endpoint available for
            // idempotent retries through the original intent deadline.
            while (HandoffClock::now() < operation->options.deadline)
              operation->changed.wait_until(guard, operation->options.deadline);
          }
          const bool wasCancelled = operation->cancellationRequested;
          const bool confirmed = allConfirmed();
          const auto failureReason = operation->failureReason;
          operation->queryEnabled = false;
          if (confirmed) {
            for (auto& item : result.conversationResults) {
              const auto key = NativeLocalWithdrawalState::Key{
                item.ownerIdentity, item.serviceName, item.conversationId};
              const auto found = operation->entries.find(key);
              item.operationId = found->second.savedOperationId;
              item.classification = "CONFIRMED";
              item.reason.clear();
            }
          }
          guard.unlock();
          if (!confirmed) {
            abort(wasCancelled ? "CANCELLED" :
                  (failureReason.empty() ? "OWNER_CONFIRMATION_MISSING" : failureReason));
            return;
          }
          if (!state->handoffRegistry->settleLocalWithdrawal(
                operation->id, operation->options.drainDeadline)) {
            result.reason = "DRAIN_TIMEOUT";
            operation->finish(NativeWithdrawalStatusV1::Failed,
                              std::move(result), true);
            return;
          }
          state->draining->store(true, std::memory_order_relaxed);
          const auto now = HandoffClock::now();
          const auto drainRemaining = now >= operation->options.drainDeadline
            ? std::chrono::milliseconds::zero()
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                operation->options.drainDeadline - now);
          if (!state->handoffRegistry->drain(drainRemaining)) {
            result.reason = "DRAIN_TIMEOUT";
            operation->finish(NativeWithdrawalStatusV1::Failed,
                              std::move(result), true);
            return;
          }
          const auto beforeReplyDrain = HandoffClock::now();
          const auto replyDrainRemaining = beforeReplyDrain >= operation->options.drainDeadline
            ? std::chrono::milliseconds::zero()
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                operation->options.drainDeadline - beforeReplyDrain);
          if (state->targetedHandlerDrain &&
              !state->targetedHandlerDrain(replyDrainRemaining)) {
            result.reason = "DRAIN_TIMEOUT";
            operation->finish(NativeWithdrawalStatusV1::Failed,
                              std::move(result), true);
            return;
          }
          state->core->close();
          result.closed = true;
          result.reason = "CONFIRMED";
          operation->finish(NativeWithdrawalStatusV1::Closed, std::move(result));
          return;
        }
      }
      catch (const std::exception& error) {
        abort(error.what());
      }
      catch (...) {
        abort("WITHDRAWAL_UNKNOWN_FAILURE");
      }
    });
  }
  catch (...) {
    m_state->withdrawal.reset();
    if (target) {
      std::lock_guard<std::mutex> targetLock(target->withdrawalMutex);
      if (target->withdrawal == operation)
        target->withdrawal.reset();
    }
    operation->worker->close();
    throw;
  }
  return NativeWithdrawalHandle(std::move(operation));
}

NativeServiceRegistration::~NativeServiceRegistration() noexcept
{
  if (m_closeOnDestroy)
    close();
}

void
NativeServiceRegistration::close() noexcept
{
  if (!m_state) {
    return;
  }
  std::shared_ptr<NativeLocalWithdrawalState> withdrawal;
  {
    std::lock_guard<std::mutex> lock(m_state->withdrawalMutex);
    withdrawal = m_state->withdrawal;
    if (m_state->draining)
      m_state->draining->store(true, std::memory_order_relaxed);
  }
  if (withdrawal)
    withdrawal->cancel();
  if (m_state->handoffRegistry) {
    m_state->handoffRegistry->requestStop();
  }
  if (m_state->core) {
    m_state->core->close();
  }
}

bool
NativeServiceRegistration::drain(std::chrono::milliseconds timeout)
{
  if (timeout.count() < 0)
    throw std::invalid_argument("native service drain timeout is negative");
  close();
  if (!m_state)
    return true;
  const auto deadline = HandoffClock::now() + timeout;
  std::shared_ptr<NativeLocalWithdrawalState> withdrawal;
  {
    std::lock_guard<std::mutex> lock(m_state->withdrawalMutex);
    withdrawal = m_state->withdrawal;
  }
  if (withdrawal && withdrawal->worker &&
      !withdrawal->worker->drain(timeout))
    return false;
  const auto nowAfterWithdrawal = HandoffClock::now();
  const auto targetedRemaining = nowAfterWithdrawal >= deadline
    ? std::chrono::milliseconds::zero()
    : std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - nowAfterWithdrawal);
  if (m_state->targetedHandlerDrain &&
      !m_state->targetedHandlerDrain(targetedRemaining))
    return false;
  if (!m_state->handoffRegistry)
    return true;
  const auto now = HandoffClock::now();
  const auto remaining = now >= deadline
    ? std::chrono::milliseconds::zero()
    : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return m_state->handoffRegistry->drain(remaining);
}

bool
NativeServiceRegistration::closed() const noexcept
{
  return !m_state || !m_state->core || m_state->core->closed();
}

bool
NativeServiceRegistration::valid() const noexcept
{
  return m_state != nullptr;
}

uint64_t
NativeServiceRegistration::generation() const noexcept
{
  return (m_state && m_state->core) ? m_state->core->generation() : 0;
}

const std::string&
NativeServiceRegistration::serviceName() const noexcept
{
  static const std::string empty;
  return m_state ? m_state->serviceName : empty;
}

NativeInferenceProvider::NativeInferenceProvider(
  std::shared_ptr<ndn_service_framework::ServiceProvider> provider,
  std::shared_ptr<const NativeAdapterRegistry> adapters)
  : m_provider(std::move(provider)), m_adapters(std::move(adapters))
{
  if (!m_provider) {
    throw std::invalid_argument(
      "NativeInferenceProvider requires a Core ServiceProvider");
  }
  if (!m_adapters) {
    throw std::invalid_argument(
      "NativeInferenceProvider requires the NativeAdapterRegistry");
  }
}


NativeInferenceProvider::~NativeInferenceProvider() noexcept
{
  stop();
}

NativeServiceRegistration
NativeInferenceProvider::serve(const NativeServiceDefinition& service,
                               const NativeProviderHandlerConfig& config)
{
  if (service.serviceName.empty()) {
    throw std::invalid_argument(
      "native service definition requires a serviceName");
  }
  if (service.allowedRoles.empty()) {
    throw std::invalid_argument(
      "native service definition requires allowedRoles");
  }

  // The identity advertised in offers, execution evidence, leases, and
  // cross-Provider data names must be the identity actually owned by this
  // ServiceProvider.  Leaving this optional let a MiniNDN fixture work by
  // accident while a multi-host deployment published a valid-looking
  // capability for a different node and then failed at Selection/data fetch.
  ndn::Name configuredProvider;
  try {
    configuredProvider = ndn::Name(config.localProviderName);
  }
  catch (const std::exception&) {
    throw std::invalid_argument(
      "native service config providerName is not a valid NDN name");
  }
  const auto actualProvider = m_provider->getName();
  if (configuredProvider.empty() || actualProvider.empty() ||
      configuredProvider != actualProvider) {
    throw std::invalid_argument(
      "native service config providerName must match ServiceProvider identity");
  }
  const auto canonicalProviderName = actualProvider.toUri();
  if (config.providerBootId.empty()) {
    throw std::invalid_argument(
      "native service config providerBootId is required");
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_stopped) {
    throw std::runtime_error("NativeInferenceProvider host is stopped");
  }

  // The first serve builds the host singleton and its fixed lease entry in a
  // local transaction.  Do not publish m_host until the target runtime and
  // scoped Core registration have both succeeded; otherwise a failed first
  // serve would pin an identity/lease table that has no usable target.
  auto host = m_host;
  bool hostCreated = false;
  if (!host) {
    host = std::make_shared<HostState>();
    // Persist the canonical NDN URI, not caller spelling.  All lease keys,
    // evidence and cross-Provider data names then use one stable identity even
    // when equivalent escaped component spellings reach separate callers.
    host->providerName = canonicalProviderName;
    host->providerBootId = config.providerBootId;
    host->workerSlots = std::max<std::size_t>(1, config.workerCount);
    host->sharedLease =
      std::make_shared<SharedExecutionLeaseState>(host->providerBootId);
    try {
      host->fixedLease = std::make_shared<ServiceProvider::ServiceRegistration>(
        m_provider->addScopedService(
          ndn::Name(EXECUTION_LEASE_SERVICE_NAME),
          acceptingAckHandler("execution lease service ready"),
          makeLeaseRouter(host),
          ServiceProvider::ServiceInvocationMode::NormalAndTargeted));
    }
    catch (...) {
      throw;
    }
    hostCreated = true;
  }
  else {
    std::lock_guard<std::mutex> routerLock(host->routerMutex);
    if (host->withdrawal || host->withdrawalClosed)
      throw std::logic_error("WITHDRAWAL_PROVIDER_SCOPE_ACTIVE");
  }

  std::shared_ptr<std::atomic<bool>> draining;
  std::shared_ptr<std::atomic<bool>> admissionStopped;
  std::shared_ptr<ServiceProvider::ServiceRegistration> core;
  std::shared_ptr<NativeProviderHandoffRegistry> handoffRegistry;
  std::shared_ptr<Target> servedTarget;
  NativeProviderCollaborationRuntime servedRuntime;
  try {
    // A serve must not silently reshape the host's shared boot identity or its
    // compute-slot range: the slot range is host resources configured at boot,
    // not a sum over serves.
    if (canonicalProviderName != host->providerName) {
      throw std::invalid_argument("native service config providerName '"
        + canonicalProviderName + "' conflicts with the host's '"
        + host->providerName + "'");
    }
    if (!config.providerBootId.empty() &&
        config.providerBootId != host->providerBootId) {
      throw std::invalid_argument(
        "native service config providerBootId conflicts with the host's");
    }
    if (std::max<std::size_t>(1, config.workerCount) != host->workerSlots) {
      throw std::invalid_argument(
        "native service config workerCount conflicts with the host's "
        "compute-slot range");
    }

    // Same-name re-serve: an active target is refused; a draining (closed)
    // record may be replaced below by a fresh registration that never reuses
    // the old target's lease instance, draining fence, or bindings.
    {
      std::lock_guard<std::mutex> routerLock(host->routerMutex);
      const auto existing = host->targets.find(service.serviceName);
      if (existing != host->targets.end() &&
          !existing->second->draining->load(std::memory_order_relaxed)) {
        throw std::logic_error("native service is already registered: "
                               + service.serviceName);
      }
    }

    // The host owns the shared lease table: when the configuration opts into
    // execution leases, inject the host table and bind the lease target to
    // this service. The resolver observes the host weakly to avoid a target
    // self-cycle, while the Core handler retains the host until its runtime
    // state has been detached and no longer needs the table.
    NativeProviderHandlerConfig effectiveConfig = config;
    // Runtime evidence must use the same canonical identity as the shared
    // host state; do not let alternate URI spellings split a Provider's
    // cross-machine bindings.
    effectiveConfig.localProviderName = canonicalProviderName;
    if (!config.executionLeaseTargetService.empty()) {
      if (config.executionLeaseTargetService != service.serviceName) {
        throw std::invalid_argument("executionLeaseTargetService '"
          + config.executionLeaseTargetService
          + "' must equal the served serviceName '" + service.serviceName
          + "'");
      }
      if (config.executionLeaseTable != nullptr) {
        throw std::invalid_argument(
          "executionLeaseTable is owned by the host; pass a null table");
      }
      effectiveConfig.executionLeaseTable = &host->sharedLease->table;
    }
    else if (config.executionLeaseTable != nullptr) {
      throw std::invalid_argument(
        "executionLeaseTable requires a non-empty executionLeaseTargetService");
    }

    // Assemble the native runtime first; nothing is registered until the
    // runtime and the observation seam both succeed.
    if (!service.withdrawalPersistencePolicyId.empty() &&
        !service.handoffOwnerContextResolver)
      throw std::invalid_argument(
        "withdrawal policy requires an authenticated Owner context resolver");
    if (effectiveConfig.handoff)
      throw std::invalid_argument("handoff policy belongs to the served service definition");
    if (service.handoff) {
      auto handoff = std::make_shared<const NativeProviderHandoffConfig>(*service.handoff);
      nativeValidateKvSaveConfig(*handoff, m_provider->getName());
      effectiveConfig.handoff = std::move(handoff);
    }
    auto runtime = makeNativeProviderCollaborationRuntime(
      std::move(effectiveConfig));
    if (service.runtimeObserver) {
      service.runtimeObserver(runtime);
    }
    // Publication is a production service capability, not an observer hook.
    // Bind it after observation and before the authenticated targeted-handler
    // factory receives this runtime.
    bindNativeProviderDurableKvPublisher(runtime, m_provider);
    servedRuntime = runtime;
    ServiceProvider::AsyncTargetedRequestHandler targetedRequestHandler;
    if (service.asyncTargetedRequestHandlerFactory) {
      targetedRequestHandler =
        service.asyncTargetedRequestHandlerFactory(
          runtime, service.handoffOwnerContextResolver);
      if (!targetedRequestHandler) {
        throw std::invalid_argument(
          "native service targeted-request factory returned an empty handler");
      }
    }
    else if (service.handoffOwnerContextResolver) {
      targetedRequestHandler = makeNativeConversationHandoffHandler(
        runtime, service.handoffOwnerContextResolver, handoffRegistry, m_provider);
    }

    draining = std::make_shared<std::atomic<bool>>(false);
    admissionStopped = std::make_shared<std::atomic<bool>>(false);
    auto guardedHandler =
      [host, draining, admissionStopped, handler = runtime.handler](
        ServiceProvider::CollaborationContext& ctx,
        const ndn_service_framework::RequestMessage& request) {
        // The Core registration gate is authoritative; this fence only covers
        // the tiny window between draining and the Core entry detaching.
        if (draining->load(std::memory_order_relaxed) ||
            admissionStopped->load(std::memory_order_relaxed)) {
          ctx.fail("native service is closing");
          return;
        }
        handler(ctx, request);
      };
    ServiceProvider::AsyncTargetedRequestHandler guardedTargetedHandler;
    if (targetedRequestHandler) {
      guardedTargetedHandler =
        [draining, handler = std::move(targetedRequestHandler)](
          const ndn::Name& requesterIdentity,
          const ndn::Name& providerName,
          const ndn::Name& serviceName,
          const ndn::Name& requestId,
          const ndn_service_framework::RequestMessage& request,
          ServiceProvider::AsyncTargetedCompletion completion) mutable {
          if (draining->load(std::memory_order_relaxed)) {
            ndn_service_framework::ResponseMessage response;
            response.setStatus(false);
            response.setErrorInfo("native service is closing");
            completion(std::move(response));
            return;
          }
          handler(requesterIdentity, providerName, serviceName, requestId,
                  request, std::move(completion));
        };
    }
    if (service.asyncAckHandler && service.ackHandler) {
      throw std::invalid_argument(
        "native service must select either synchronous or asynchronous ACK handling");
    }
    auto ackHandler = service.ackHandler
      ? service.ackHandler
      : acceptingAckHandler("native provider ready");
    auto asyncAckHandler = service.asyncAckHandler;
    if (service.handoffOwnerContextResolver) {
      const auto modelAckHandler = ackHandler;
      ackHandler = [modelAckHandler](const ndn_service_framework::RequestMessage& request) {
        if (isNativeConversationHandoffBootstrap(request)) {
          ServiceProvider::AckDecision decision;
          decision.status = true;
          decision.message = "native conversation handoff control";
          return decision;
        }
        return modelAckHandler(request);
      };
      if (asyncAckHandler) {
        const auto modelAsyncAckHandler = asyncAckHandler;
        asyncAckHandler = [modelAsyncAckHandler](
            const ServiceProvider::AckRequestContext& context,
            const ndn_service_framework::RequestMessage& request,
            ServiceProvider::AsyncAckCompletion completion) {
          if (isNativeConversationHandoffBootstrap(request)) {
            ServiceProvider::AckDecision decision;
            decision.status = true;
            decision.message = "native conversation handoff control";
            completion(std::move(decision));
            return;
          }
          modelAsyncAckHandler(context, request, std::move(completion));
        };
      }
    }
    core = std::make_shared<ServiceProvider::ServiceRegistration>(
      service.asyncAckHandler
        ? m_provider->addScopedAsyncCollaborationHandler(
            ndn::Name(service.serviceName), service.allowedRoles,
            std::move(asyncAckHandler), guardedHandler,
            std::move(guardedTargetedHandler))
        : m_provider->addScopedCollaborationHandler(
            ndn::Name(service.serviceName), service.allowedRoles,
            ackHandler, ServiceProvider::AsyncAckStrategyHandler{},
            guardedHandler, std::move(guardedTargetedHandler)));
    auto target = std::make_shared<Target>();
    servedTarget = target;
    target->core = core;
    target->runtime = runtime;
    target->withdrawalPersistencePolicyId = service.withdrawalPersistencePolicyId;
    target->targetedHandlerDrain = core->makeTargetedHandlerDrainWaiter();
    target->draining = draining;
    target->admissionStopped = admissionStopped;
    target->handoffRegistry = handoffRegistry;
    target->lease = std::make_shared<ExecutionLeaseService>(
      host->providerName, service.serviceName,
      makeHostSlotResolver(host), host->sharedLease);
    {
      std::lock_guard<std::mutex> routerLock(host->routerMutex);
      host->targets[service.serviceName] = std::move(target);
    }
    // Publish only after the full first target is installed.  m_mutex is held
    // for the entire serve call, so a concurrent serve cannot observe a half
    // initialized host.
    if (hostCreated) {
      m_host = host;
      hostCreated = false;
    }
  }
  catch (...) {
    if (draining) {
      draining->store(true, std::memory_order_relaxed);
    }
    if (handoffRegistry) {
      handoffRegistry->requestStop();
    }
    if (core) {
      core->close();
    }
    if (hostCreated) {
      if (host->fixedLease) {
        host->fixedLease->close();
      }
      m_host.reset();
    }
    throw;
  }

  auto state = std::make_shared<NativeServiceRegistration::State>();
  state->serviceName = service.serviceName;
  state->withdrawalPersistencePolicyId = service.withdrawalPersistencePolicyId;
  state->providerName = host->providerName;
  state->providerBootId = host->providerBootId;
  state->ownerContextResolver = service.handoffOwnerContextResolver;
  state->producer = m_provider;
  if (core)
    state->targetedHandlerDrain = core->makeTargetedHandlerDrainWaiter();
  state->core = std::move(core);
  state->runtime = std::move(servedRuntime);
  state->draining = std::move(draining);
  state->handoffRegistry = std::move(handoffRegistry);
  state->target = servedTarget;
  servedTarget->withdrawPrepared = [weakState = std::weak_ptr<NativeServiceRegistration::State>(state)](
      const NativeWithdrawalOptionsV1& options,
      ConversationStateStore::SaveFence fence, std::string id,
      std::shared_ptr<NativeWithdrawalCommitGate> parentCommitGate,
      std::shared_ptr<NativeWithdrawalWireBudget> parentWireBudget) {
    const auto active = weakState.lock();
    if (!active)
      throw std::logic_error("WITHDRAWAL_SERVICE_NOT_ACTIVE");
    NativeServiceRegistration registration(active, false);
    return registration.withdrawPrepared(options, std::move(fence), std::move(id),
                                         std::move(parentCommitGate),
                                         std::move(parentWireBudget));
  };
  return NativeServiceRegistration(std::move(state));
}

NativeWithdrawalHandle
NativeInferenceProvider::withdraw(const NativeWithdrawalOptionsV1& options)
{
  const auto now = HandoffClock::now();
  if (options.deadline <= now ||
      options.deadline - now > std::chrono::minutes(10) ||
      options.deadline == HandoffClock::time_point::max() ||
      options.drainDeadline < options.deadline ||
      options.drainDeadline == HandoffClock::time_point::max() ||
      options.maxConversations == 0 ||
      options.maxConversations > std::numeric_limits<std::size_t>::max() / 64 ||
      options.maxTotalBytes == 0 || options.persistencePolicyId.empty())
    throw std::invalid_argument("WITHDRAWAL_OPTIONS_INVALID");
  if (options.failurePolicy != NativeWithdrawalFailurePolicyV1::Abort &&
      options.failurePolicy != NativeWithdrawalFailurePolicyV1::ForceUnsaved)
    throw std::invalid_argument("WITHDRAWAL_FAILURE_POLICY_INVALID");

  struct PreparedService
  {
    std::shared_ptr<Target> target;
    ConversationStateStore::SaveFence fence;
    std::string id;
  };
  std::lock_guard<std::mutex> hostLock(m_mutex);
  if (m_stopped || !m_host)
    throw std::logic_error("WITHDRAWAL_PROVIDER_NOT_ACTIVE");
  const auto host = m_host;
  std::unique_lock<std::mutex> routerLock(host->routerMutex);
  if (host->withdrawal && host->withdrawal->isTerminal() &&
      NativeWithdrawalHandle(host->withdrawal).status() ==
        NativeWithdrawalStatusV1::Aborted)
    host->withdrawal.reset();
  if (host->withdrawal) {
    if (!sameWithdrawalOptions(host->withdrawal->options, options))
      throw std::logic_error("WITHDRAWAL_OPERATION_CONFLICT");
    return NativeWithdrawalHandle(host->withdrawal);
  }
  if (host->withdrawalClosed || !host->fixedLease || host->fixedLease->closed())
    throw std::logic_error("WITHDRAWAL_PROVIDER_NOT_ACTIVE");

  std::vector<std::shared_ptr<Target>> targets;
  for (const auto& item : host->targets) {
    const auto& target = item.second;
    if (!target->core || target->core->closed())
      continue;
    if (target->withdrawalPersistencePolicyId.empty() ||
        !target->withdrawPrepared || !target->admissionStopped ||
        !target->runtime.beginProviderWithdrawalSaveFence ||
        !target->runtime.borrowServiceWithdrawalSaveFence ||
        !target->runtime.matchesServiceWithdrawalSaveFence)
      throw std::logic_error("WITHDRAWAL_DISABLED");
    if (target->withdrawalPersistencePolicyId != options.persistencePolicyId)
      throw std::invalid_argument("WITHDRAWAL_POLICY_ID_MISMATCH");
    targets.push_back(target);
  }
  const auto id = newWithdrawalId();
  const auto providerGeneration = host->fixedLease->generation();
  if (providerGeneration == 0)
    throw std::logic_error("WITHDRAWAL_GENERATION_UNAVAILABLE");
  auto operation = std::make_shared<NativeLocalWithdrawalState>(
    id, options, ConversationStateStore::SaveFence{});
  operation->hostCommitGate = std::make_shared<NativeWithdrawalCommitGate>();
  operation->hostCommitGate->parent = operation;
  operation->wireBudget =
    std::make_shared<NativeWithdrawalWireBudget>(options.maxTotalBytes);
  operation->worker = ndn_service_framework::OperationRuntime::create();
  operation->worker->deferJoinOnDestroy();
  auto ticket = operation->worker->acquire();
  host->withdrawal = operation;
  for (const auto& target : targets)
    target->admissionStopped->store(true, std::memory_order_release);

  std::vector<ConversationStateStore::SaveFence> parents;
  std::vector<PreparedService> prepared;
  prepared.reserve(targets.size());
  try {
    for (const auto& target : targets) {
      const auto childId = newWithdrawalId();
      const auto generation = target->core->generation();
      if (generation == 0)
        throw std::logic_error("WITHDRAWAL_GENERATION_UNAVAILABLE");
      std::optional<ConversationStateStore::SaveFence> child;
      for (const auto& parent : parents) {
        child = target->runtime.borrowServiceWithdrawalSaveFence(
          parent, target->core->serviceName().toUri(), childId, generation);
        if (child)
          break;
      }
      if (!child) {
        auto parent = target->runtime.beginProviderWithdrawalSaveFence(
          id, providerGeneration);
        if (!parent)
          throw std::logic_error("WITHDRAWAL_SCOPE_BUSY");
        child = target->runtime.borrowServiceWithdrawalSaveFence(
          *parent, target->core->serviceName().toUri(), childId, generation);
        if (!child)
          throw std::logic_error("WITHDRAWAL_SCOPE_BUSY");
        parents.push_back(std::move(*parent));
      }
      if (!target->runtime.matchesServiceWithdrawalSaveFence(
            *child, target->core->serviceName().toUri(), childId, generation))
        throw std::logic_error("WITHDRAWAL_SCOPE_BINDING_MISMATCH");
      prepared.push_back({target, std::move(*child), childId});
    }
    operation->providerFences = std::move(parents);
    operation->worker->post(ticket,
      [host, operation, targets,
       prepared = std::move(prepared)]() mutable {
        NativeWithdrawalResultV1 result;
        result.withdrawalId = operation->id;
        result.scope = NativeWithdrawalScopeV1::Provider;
        const auto preflightFailure = [&] (std::string reason, bool isolate) {
          result.reason = std::move(reason);
          if (!isolate)
            prepared.clear();
          if (!isolate) {
            std::lock_guard<std::mutex> lock(host->routerMutex);
            for (const auto& target : targets)
              target->admissionStopped->store(false, std::memory_order_release);
          }
          operation->finish(isolate ? NativeWithdrawalStatusV1::Failed
            : NativeWithdrawalStatusV1::Aborted, std::move(result), isolate);
          if (!isolate) {
            std::lock_guard<std::mutex> lock(host->routerMutex);
            if (host->withdrawal == operation)
              host->withdrawal.reset();
          }
        };
        try {
        std::uint64_t totalBytes = 0;
        bool totalOverflow = false;
        std::set<std::tuple<std::string, std::string, std::string>> conversations;
        for (const auto& item : prepared) {
          ConversationStateStore::SaveBatchResult batch;
          for (;;) {
            bool cancelled = false;
            {
              std::lock_guard<std::mutex> lock(operation->mutex);
              cancelled = operation->cancellationRequested;
            }
            if (cancelled) {
              preflightFailure("CANCELLED", false);
              return;
            }
            if (HandoffClock::now() >= operation->options.deadline) {
              preflightFailure("DEADLINE_EXCEEDED", false);
              return;
            }
            if (item.target->runtime.isSaveFenceQuiescent(item.fence)) {
              batch = item.target->runtime.tryAcquireCommittedSaveBatch(
                item.fence,
                {operation->options.maxConversations * 64,
                 std::numeric_limits<std::size_t>::max(),
                 std::numeric_limits<std::uint64_t>::max()},
                handoffNowMs());
              if (batch.status != ConversationSaveBatchStatusV1::Busy)
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
          if (batch.status != ConversationSaveBatchStatusV1::Ready &&
              batch.status != ConversationSaveBatchStatusV1::Empty) {
            preflightFailure(batch.status == ConversationSaveBatchStatusV1::BoundsExceeded
              ? "BUDGET_EXCEEDED" : "UNSUPPORTED_STATE",
              operation->options.failurePolicy ==
                NativeWithdrawalFailurePolicyV1::ForceUnsaved);
            return;
          }
          if (batch.batch) {
            for (const auto& row : batch.batch->rows())
              conversations.emplace(row.metadata.authenticatedRequester,
                row.binding.serviceName, row.binding.conversationId);
            batch.batch.reset();
          }
          if (batch.totalBytes >
              std::numeric_limits<std::uint64_t>::max() - totalBytes) {
            totalBytes = std::numeric_limits<std::uint64_t>::max();
            totalOverflow = true;
          }
          else
            totalBytes += batch.totalBytes;
        }
        const bool overBudget = totalOverflow ||
          totalBytes > operation->options.maxTotalBytes ||
          conversations.size() > operation->options.maxConversations;
        if (overBudget && operation->options.failurePolicy ==
              NativeWithdrawalFailurePolicyV1::Abort) {
          preflightFailure("BUDGET_EXCEEDED", false);
          return;
        }
        auto childOptions = operation->options;
        if (overBudget)
          childOptions.maxTotalBytes = 1;
        bool isolated = false;
        bool allClosed = true;
        bool anyClosed = false;
        bool anySaved = false;
        std::set<std::tuple<std::string, std::string, std::string,
                            std::string, std::uint64_t>> seen;
        for (auto& item : prepared) {
          bool cancelled = false;
          {
            std::lock_guard<std::mutex> lock(operation->mutex);
            cancelled = operation->cancellationRequested;
          }
          if (cancelled) {
            allClosed = false;
            result.reason = "CANCELLED";
            break;
          }
          try {
            auto child = item.target->withdrawPrepared(
              childOptions, std::move(item.fence), std::move(item.id),
              operation->hostCommitGate, operation->wireBudget);
            std::lock_guard<std::mutex> lock(operation->mutex);
            operation->children.push_back(std::move(child));
            if (operation->cancellationRequested)
              operation->children.back().cancel();
            operation->phase = NativeWithdrawalStatusV1::AwaitingOwners;
          }
          catch (const std::exception& error) {
            allClosed = false;
            result.reason = error.what();
            operation->cancel();
            break;
          }
        }
        std::vector<NativeWithdrawalHandle> children;
        {
          std::lock_guard<std::mutex> lock(operation->mutex);
          children = operation->children;
        }
        for (std::size_t i = 0; i < children.size(); ++i) {
          try {
            const auto now = HandoffClock::now();
            const auto remaining = now >= operation->options.drainDeadline
              ? std::chrono::milliseconds::zero()
              : std::chrono::duration_cast<std::chrono::milliseconds>(
                  operation->options.drainDeadline - now);
            const auto childResult = children[i].result(remaining);
            allClosed = allClosed && childResult.closed;
            anyClosed = anyClosed || childResult.closed;
            anySaved = anySaved || childResult.reason == "CONFIRMED";
            result.forced = result.forced || childResult.forced;
            if (!childResult.closed && result.reason.empty())
              result.reason = childResult.reason;
            if (!childResult.closed &&
                children[i].status() == NativeWithdrawalStatusV1::Failed)
              isolated = true;
            const auto generation = targets[i]->core->generation();
            for (const auto& row : childResult.conversationResults) {
              const auto key = std::make_tuple(row.ownerIdentity, row.serviceName,
                row.conversationId, row.checkpointDigest, generation);
              if (seen.insert(key).second)
                result.conversationResults.push_back(row);
            }
          }
          catch (const ndn_service_framework::OperationError& error) {
            allClosed = false;
            isolated = true;
            if (result.reason.empty())
              result.reason = error.code() ==
                ndn_service_framework::OperationErrorCode::Timeout
                  ? "DRAIN_TIMEOUT" : error.what();
          }
          catch (const std::exception& error) {
            allClosed = false;
            isolated = true;
            if (result.reason.empty())
              result.reason = error.what();
          }
        }
        if (allClosed) {
          host->fixedLease->close();
          result.closed = true;
          result.reason = result.forced ? "FORCED_UNSAVED"
            : (anySaved ? "CONFIRMED" : "EMPTY");
          {
            std::lock_guard<std::mutex> lock(host->routerMutex);
            host->withdrawalClosed = true;
          }
          operation->finish(result.forced
            ? NativeWithdrawalStatusV1::ForcedUnsaved
            : NativeWithdrawalStatusV1::Closed, std::move(result));
          return;
        }
        if (result.reason.empty())
          result.reason = "WITHDRAWAL_INCOMPLETE";
        {
          std::lock_guard<std::mutex> gateLock(operation->hostCommitGate->mutex);
          isolated = isolated || anyClosed || operation->hostCommitGate->commitReleased;
        }
        if (!isolated)
          prepared.clear();
        if (!isolated) {
          std::lock_guard<std::mutex> lock(host->routerMutex);
          for (const auto& target : targets) {
            if (!target->core->closed())
              target->admissionStopped->store(false, std::memory_order_release);
          }
        }
        operation->finish(isolated ? NativeWithdrawalStatusV1::Failed
          : NativeWithdrawalStatusV1::Aborted, std::move(result), isolated);
        if (!isolated) {
          std::lock_guard<std::mutex> lock(host->routerMutex);
          if (host->withdrawal == operation)
            host->withdrawal.reset();
        }
        }
        catch (const std::exception& error) {
          operation->cancel();
          result.reason = error.what();
          operation->finish(NativeWithdrawalStatusV1::Failed,
                            std::move(result), true);
        }
        catch (...) {
          operation->cancel();
          result.reason = "WITHDRAWAL_UNKNOWN_FAILURE";
          operation->finish(NativeWithdrawalStatusV1::Failed,
                            std::move(result), true);
        }
      });
  }
  catch (...) {
    for (const auto& target : targets)
      target->admissionStopped->store(false, std::memory_order_release);
    operation->providerFences.clear();
    host->withdrawal.reset();
    operation->worker->close();
    throw;
  }
  return NativeWithdrawalHandle(std::move(operation));
}

void
NativeInferenceProvider::stop() noexcept
{
  std::shared_ptr<HostState> host;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopped) {
      return;
    }
    m_stopped = true;
    host = m_host;
  }
  if (!host) {
    return;
  }
  std::shared_ptr<NativeLocalWithdrawalState> hostWithdrawal;
  {
    std::lock_guard<std::mutex> routerLock(host->routerMutex);
    hostWithdrawal = host->withdrawal;
  }
  if (hostWithdrawal)
    hostWithdrawal->cancel();
  if (host->fixedLease) {
    host->fixedLease->close();
  }
  std::vector<std::shared_ptr<Target>> targets;
  {
    std::lock_guard<std::mutex> routerLock(host->routerMutex);
    targets.reserve(host->targets.size());
    for (const auto& entry : host->targets) {
      targets.push_back(entry.second);
    }
  }
  for (const auto& target : targets) {
    target->draining->store(true, std::memory_order_relaxed);
    std::shared_ptr<NativeLocalWithdrawalState> withdrawal;
    {
      std::lock_guard<std::mutex> lock(target->withdrawalMutex);
      withdrawal = target->withdrawal;
    }
    if (withdrawal)
      withdrawal->cancel();
    if (target->handoffRegistry) {
      target->handoffRegistry->requestStop();
    }
    if (target->core) {
      target->core->close();
    }
  }
}

bool
NativeInferenceProvider::drain(std::chrono::milliseconds timeout)
{
  if (timeout.count() < 0)
    throw std::invalid_argument("native provider drain timeout is negative");
  stop();
  std::shared_ptr<HostState> host;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    host = m_host;
  }
  if (!host)
    return true;

  const auto deadline = HandoffClock::now() + timeout;
  std::shared_ptr<NativeLocalWithdrawalState> hostWithdrawal;
  std::vector<std::shared_ptr<Target>> targets;
  {
    std::lock_guard<std::mutex> routerLock(host->routerMutex);
    hostWithdrawal = host->withdrawal;
    targets.reserve(host->targets.size());
    for (const auto& entry : host->targets)
      targets.push_back(entry.second);
  }
  if (hostWithdrawal && hostWithdrawal->worker &&
      !hostWithdrawal->worker->drain(timeout))
    return false;
  for (const auto& target : targets) {
    auto now = HandoffClock::now();
    auto remaining = now >= deadline
      ? std::chrono::milliseconds::zero()
      : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    std::shared_ptr<NativeLocalWithdrawalState> withdrawal;
    {
      std::lock_guard<std::mutex> lock(target->withdrawalMutex);
      withdrawal = target->withdrawal;
    }
    if (withdrawal && withdrawal->worker &&
        !withdrawal->worker->drain(remaining))
      return false;
    now = HandoffClock::now();
    remaining = now >= deadline
      ? std::chrono::milliseconds::zero()
      : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (target->targetedHandlerDrain &&
        !target->targetedHandlerDrain(remaining))
      return false;
    if (!target->handoffRegistry)
      continue;
    now = HandoffClock::now();
    remaining = now >= deadline
      ? std::chrono::milliseconds::zero()
      : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (!target->handoffRegistry->drain(remaining))
      return false;
  }
  return true;
}

} // namespace ndnsf::di
