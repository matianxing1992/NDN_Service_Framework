#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedKvCheckpoint.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <optional>

namespace ndnsf::di {
struct NativeSelectionProjectionV3;

struct NativeProtectedKvRestoreRecipientV1
{
  ndn::Name certificateName;
  std::string publicKeyDigest;
  std::uint64_t expiresAtMs = 0;
};

// An authenticated assignment carries this value; decoding it grants nothing.
struct NativeProtectedKvRestoreV1
{
  NativeJson reference;
  NativeProtectedKvRestoreRecipientV1 recipient;
  ndn_service_framework::RecipientEncryptedAssignment capsule;
  // Original owner-retained receipt JSON bytes. Present only in restore-v2;
  // metadata commitment, not a substitute for its original signed Data packet.
  std::optional<std::string> originReceipt;
};

NativeJson nativeProtectedKvRestoreToJson(const NativeProtectedKvRestoreV1& restore);
NativeProtectedKvRestoreV1 nativeProtectedKvRestoreFromJson(const NativeJson& value);
// Projection-visible consistency only; requester authority requires the context validator.
void nativeValidateProtectedKvRestoreProjection(const NativeProtectedKvRestoreV1& restore,
                                               const NativeSelectionProjectionV3& projection);

std::string nativeProtectedKvAuthorizationBinding(const std::string& provider,
  const std::string& role, const std::string& grantName, const std::string& grantDigest,
  const std::string& leaseScopeDigest = {});

// Build expected AAD from current authenticated invocation context, not nested fields.
NativeProtectedKvRecipientBindingV1 nativeProtectedKvRecipientBindingForProjection(
  const NativeSelectionProjectionV3& projection, const ndn::Name& expectedRequester,
  const ndn::Name& expectedService, const NativeProtectedKvRestoreRecipientV1& recipient,
  std::uint64_t nowMs);

// Caller supplies sealed-owner scope or the actual authenticated Handler context.
void nativeValidateProtectedKvRestoreContext(const NativeProtectedKvRestoreV1& restore,
  const NativeSelectionProjectionV3& projection, const ndn::Name& expectedRequester,
  const ndn::Name& expectedService, std::uint64_t nowMs);
} // namespace ndnsf::di
