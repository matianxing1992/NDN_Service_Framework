#ifndef NDNSF_DI_NATIVE_GRANT_TYPES_HPP
#define NDNSF_DI_NATIVE_GRANT_TYPES_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::di {

/** The requester-side binding of one published, encrypted grant. */
struct NativeGrantBinding
{
  std::string provider;
  std::string role;
  std::string grantName;
  std::string grantDigest;
  std::string recipient;
  std::string wireJson;
  std::uint64_t expiresAtMs = 0;
};

/**
 * Stable authorization identity for a conversation-scoped grant lease.
 *
 * The lease identity is deliberately separate from a turn's requestId,
 * attempt, planCoreDigest and planDigest. Those values remain fresh for
 * every turn and continue to bind the current Selection and execution.
 * Lease state is process-local and is never restored from a conversation
 * journal; a process restart therefore requires a new authorization.
 */
struct NativeGrantLeaseScope
{
  std::string conversationId;
  std::string requesterIdentity;
  std::string serviceName;
  std::string requestId;
  std::uint64_t attempt = 1;
  std::string planCoreDigest;
  std::string scopeDigest;
  std::string securityPolicySnapshotDigest;
  std::string protectionEpoch;
  std::uint64_t expiresAtMs = 0;
  std::map<std::string, std::string> providerByRole;
};

struct NativeGrantLease
{
  NativeGrantLeaseScope scope;
  std::vector<NativeGrantBinding> grants;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_GRANT_TYPES_HPP
