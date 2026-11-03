#ifndef NDNSF_DI_NATIVE_KV_RESTORE_HPP
#define NDNSF_DI_NATIVE_KV_RESTORE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePortableKv.hpp"

namespace ndnsf::di {

/** Parsed encrypted recovery material; current Core admission remains mandatory. */
struct NativeKvRestoreV1
{
  NativeSavedKvRoleV1 saved;
  ndn_service_framework::RecipientEncryptedAssignment capsule;
  NativeProtectedKvRecipientBindingV1 recipient;
};

NativeProtectedKvRecipientBindingV1
nativeKvRestoreRecipientBinding(const NativeSelectionProjectionV3& projection,
  const ndn::Name& requester, const ndn::Name& service,
  const ndn::Name& recipientCertificate, const std::string& publicKeyDigest,
  std::uint64_t expiresAtMs, std::uint64_t nowMs);

NativeJson
nativeKvRestoreToJson(const NativeSavedKvRoleV1& saved,
  const ndn_service_framework::RecipientEncryptedAssignment& capsule,
  const NativeProtectedKvRecipientBindingV1& recipient);

NativeKvRestoreV1
nativeReadKvRestore(const NativeJson& value, const NativeSelectionProjectionV3& projection,
  const ndn::Name& requester, const ndn::Name& service, std::uint64_t nowMs);

/** Exact shared receipt semantics after portable framing/hash/GCM validation. */
void
nativeValidateSavedKvOrigin(const DecodeStateIdentityV1& identity,
  const ProviderConversationStateReceiptV1& receipt);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_KV_RESTORE_HPP
