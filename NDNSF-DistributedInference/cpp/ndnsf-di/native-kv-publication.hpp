#ifndef NDNSF_DI_NATIVE_KV_PUBLICATION_HPP
#define NDNSF_DI_NATIVE_KV_PUBLICATION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/portable-kv-checkpoint.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ndnsf_distributed_repo {
struct RepoDataReference;
}

namespace ndnsf::di {

struct NativeProviderCollaborationRuntime;
struct NativeProtectedKvPacketSet;

/** Describe the original KV packet set without claiming a Core transport envelope.
 * This internal Repo-enabled leaf preserves names, order, wire digest and Producer scope.
 */
ndnsf_distributed_repo::RepoDataReference
nativeKvPacketReference(const NativeProtectedKvPacketSet& packets, const ndn::Name& producer);

std::pair<NativePortableKvCheckpointLimitsV1, NativeProtectedKvCheckpointLimitsV1>
nativeKvPublicationLimits(const NativeProviderHandoffConfig& config,
  const std::function<void()>& current = {});

struct NativeDurableStorageRoleGrantChallengeV1
{
  std::string roleName;
  ndn::Name manifestName;
  std::string writerIdentity;
};

struct NativeDurableStorageRoleGrantV1
{
  std::string repoNode, ownerIdentity, grantId, writerIdentity, objectName;
  std::uint64_t ownerEpoch = 0;
};

using NativeDurableStorageGrantResolver =
  std::function<NativeDurableStorageRoleGrantV1(const ndn::Name&, const std::string&,
                                            const std::function<void()>&)>;

/** The source branch's all-role exact-object grant barrier.
 * Coordination does not confer storage authority; Repo validates each grant.
 */
class NativeDurableStorageGrantBarrierV1 final
  : public std::enable_shared_from_this<NativeDurableStorageGrantBarrierV1>
{
public:
  using ProducerPrefixesByRole = std::map<std::string, ndn::Name>;
  using GrantSet = std::map<std::string, NativeDurableStorageRoleGrantV1>;
  using SelectorSet = GrantSet;
  using Clock = std::chrono::steady_clock;

  NativeDurableStorageGrantBarrierV1(std::string ownerIdentity,
    ProducerPrefixesByRole producerPrefixesByRole, Clock::time_point deadline);

  NativeDurableStorageGrantResolver resolverFor(const std::string& roleName);
  std::vector<NativeDurableStorageRoleGrantChallengeV1> waitForAllChallenges(
    const std::function<void()>& current = {});
  void releaseAll(GrantSet grants);
  void abort(std::exception_ptr error);

private:
  using ReadyPredicate = std::function<bool()>;

  NativeDurableStorageRoleGrantV1 resolve(const std::string& roleName,
    const ndn::Name& manifestName, const std::string& writerIdentity,
    const std::function<void()>& current);
  void waitUntil(std::unique_lock<std::mutex>& lock, const ReadyPredicate& ready,
    const std::function<void()>& current, const char* timeoutError);
  void failLocked(std::exception_ptr error);
  void rethrowIfFailedLocked() const;
  [[noreturn]] void abortAndRethrow(std::exception_ptr error);

  const std::string m_ownerIdentity;
  const ProducerPrefixesByRole m_producerPrefixesByRole;
  const Clock::time_point m_deadline;
  std::mutex m_mutex;
  std::condition_variable m_condition;
  std::map<std::string, NativeDurableStorageRoleGrantChallengeV1> m_challenges;
  SelectorSet m_grants;
  std::exception_ptr m_error;
  bool m_released = false;
};

/** Existing publication callbacks bound to one original Source operation. */
struct NativeDurableStorageOwner
{
  std::string ownerIdentity;
  NativeProviderHandoffConfig publication;
  NativeConversationHandoffPolicyV1 policy;
  NativeDurableStorageGrantRepoTargetV1 repoTarget;
  ProviderConversationStateReceiptV1 receipt;
  NativeDurableStorageGrantResolver resolveGrant;
  std::function<void(const NativeDurableStorageGrantRoleChallengeV1&)> onGrantRequired;
  std::function<void()> onDurablePacketSetBoundary;
  std::chrono::steady_clock::time_point deadline;
};

/** Completed Source publication; the Owner still verifies the original packet set. */
class NativeProtectedKvPublicationV1
{
public:
  NativeProtectedKvPublicationV1(NativeSavedKvRoleV1 saved, std::function<void()> current)
    : m_saved(std::move(saved)), m_current(std::move(current))
  {
  }

  void requireCurrent() const;
  NativeJson reference() const;

private:
  NativeSavedKvRoleV1 m_saved;
  std::function<void()> m_current;
};

NativeJson
nativeProtectedKvPublicationReference(const NativeProtectedKvPublicationV1& publication);

void
bindNativeProviderDurableKvPublisher(NativeProviderCollaborationRuntime& runtime,
  std::shared_ptr<ndn_service_framework::ServiceProvider> producer);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_KV_PUBLICATION_HPP
