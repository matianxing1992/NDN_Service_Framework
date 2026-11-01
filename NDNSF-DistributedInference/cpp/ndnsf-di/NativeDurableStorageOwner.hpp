#pragma once

#include "ndn-service-framework/EncryptedLargeDataRangeStore.hpp"

#include <functional>
#include <string>

namespace ndnsf::di {

/** Process-local owner lane; the Repo still verifies the exact object grant. */
struct NativeDurableStorageOwner
{
  std::string ownerIdentity;
  ndn_service_framework::DurableStorageGrantResolver resolveGrant;
  ndn_service_framework::DurableStorageGrantChallengeObserverV1 onGrantRequired{};
  // Provider durable publication calls this at the irreversible packet-set
  // boundary: just before a new commit, or after validating a reused durable
  // set. A handoff callback parks its Provider operation here until the
  // authenticated control path receives the Owner's matching COMMIT or CANCEL;
  // polling reports BOUNDARY_READY while that callback is parked.
  std::function<void()> onDurablePacketSetBoundary{};

  void validate() const;
  ndn_service_framework::DurableStorageGrantResolver checkedResolver() const;
};

} // namespace ndnsf::di
