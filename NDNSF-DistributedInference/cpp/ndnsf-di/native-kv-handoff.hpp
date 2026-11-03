#ifndef NDNSF_DI_NATIVE_KV_HANDOFF_HPP
#define NDNSF_DI_NATIVE_KV_HANDOFF_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"

#include <ndn-cxx/security/certificate.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace ndn_service_framework {
class ServiceUser;
}

namespace ndnsf::di {

enum class NativeWithdrawalScopeV1 { Service, Provider };

/** Native packet transport using the Provider identity and its existing Face.
 * The caller keeps that Face alive until the service and its workers drain.
 * workspace is an existing private directory owned by the process.
 */
struct NativeProviderHandoffConfig
{
  std::shared_ptr<ndn_service_framework::ServiceUser> storageWriter;
  ndn::security::Certificate signingCertificate;
  std::filesystem::path workspace;
  std::string repoNode;
  std::string repoProvider;
  std::string repoServicePrefix;
  std::uint64_t maxStateBytes = 128ULL * 1024ULL * 1024ULL;
};

struct NativeDurableStorageGrantRepoTargetV1
{
  std::string repoNode, providerIdentity, servicePrefix;
};

/** Per-role pinned replica route; object trust always uses the actual User profile. */
struct NativeRemoteProtectedKvRoleTrustPolicyV1
{
  NativeDurableStorageGrantRepoTargetV1 target;

  bool operator==(const NativeRemoteProtectedKvRoleTrustPolicyV1& other) const noexcept
  {
    return target.repoNode == other.target.repoNode &&
      target.providerIdentity == other.target.providerIdentity &&
      target.servicePrefix == other.target.servicePrefix;
  }

  bool operator!=(const NativeRemoteProtectedKvRoleTrustPolicyV1& other) const noexcept
  {
    return !(*this == other);
  }
};

/** Operator-pinned limits for the original bounded handoff transaction. */
struct NativeConversationHandoffPolicyV1
{
  bool enabled = false;
  std::size_t maxConcurrentEvents = 0;
  std::size_t maxRolesPerEvent = 0;
  std::size_t maxConcurrentRoleSlots = 0;
  std::size_t maxPacketCountPerRole = 0;
  std::uint64_t maxPacketSetWireBytesPerRole = 0;
  std::uint64_t maxAggregateBytes = 0;
  std::uint64_t maxScratchBytesPerEvent = 0;
  std::uint64_t scratchQuotaBytes = 0;
  std::uint64_t maxSaveDurationMs = 0;
  std::filesystem::path scratchRoot;
  std::string effectiveDigest;
};

/** Inputs resolved for one Core-authenticated Owner, service and operation.
 * The resolver verifies the Owner certificate against configured trust.
 * policy.scratchRoot equals publication.workspace, one private disk workspace.
 * Private checkpoint codec limits remain implementation details.
 */
struct NativeConversationHandoffOwnerContextV1
{
  ndn::security::Certificate ownerEncryptionCertificate;
  NativeProviderHandoffConfig publication;
  ConversationSaveBatchLimitsV1 saveBatchLimits;
  NativeConversationHandoffPolicyV1 policy;
  std::map<std::string, NativeDurableStorageGrantRepoTargetV1> repoTargetByRole;
};

using NativeConversationHandoffOwnerContextResolverV1 =
  std::function<NativeConversationHandoffOwnerContextV1(
    const ndn::Name& authenticatedOwner,
    const ndn::Name& serviceName,
    const std::string& operationId)>;

NativeConversationHandoffOwnerContextV1
resolveNativeConversationHandoffOwnerContextV1(
  const NativeConversationHandoffOwnerContextResolverV1& resolver,
  const ndn::Name& authenticatedOwner,
  const ndn::Name& serviceName,
  const std::string& operationId);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_KV_HANDOFF_HPP
