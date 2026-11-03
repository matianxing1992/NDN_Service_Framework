#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-restore.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <algorithm>

namespace ndnsf::di {
namespace {

void
require(bool condition, const char* message)
{
  if (!condition) throw std::invalid_argument(message);
}

void
fields(const NativeJson& value, std::initializer_list<const char*> names)
{
  require(value.is_object() && value.size() == names.size(), "KV restore field set mismatch");
  for (const auto name : names)
    require(value.contains(name), "KV restore field missing");
}

} // namespace

NativeProtectedKvRecipientBindingV1
nativeKvRestoreRecipientBinding(const NativeSelectionProjectionV3& projection,
  const ndn::Name& requester, const ndn::Name& service,
  const ndn::Name& recipientCertificate, const std::string& publicKeyDigest,
  std::uint64_t expiresAtMs, std::uint64_t nowMs)
{
  require(projection.conversationStateReference && projection.conversationTurnBinding &&
    projection.generationContract.enabled && !projection.generationContract.generationId.empty() &&
    projection.hasGrantBinding && !projection.grantName.empty() && !projection.grantDigest.empty() &&
    !requester.empty() && !service.empty() && !ndn::Name(projection.provider).empty() &&
    !ndn::Name(projection.requestId).empty() && projection.attempt >= 1 && projection.attempt <= 2,
    "KV restore requires a current protected conversation Selection");
  const auto& parent = *projection.conversationStateReference;
  const auto& turn = *projection.conversationTurnBinding;
  parent.validate();
  turn.validate();
  require(parent.conversationId == turn.conversationId && parent.contextEpoch > 0 &&
    parent.contextEpoch == turn.parentContextEpoch &&
    parent.checkpointDigest == turn.parentCheckpointDigest &&
    ndn::Name(parent.serviceName) == service && ndn::Name(turn.serviceName) == service &&
    parent.roleName == projection.executionRole.roleId &&
    ndn::Name(projection.provider).append("KEY").isPrefixOf(recipientCertificate) &&
    recipientCertificate.size() > ndn::Name(projection.provider).size() + 1 &&
    expiresAtMs > nowMs && expiresAtMs <= projection.deadlineMs &&
    expiresAtMs <= parent.expiresAtMs && expiresAtMs <= turn.retentionDeadlineMs &&
    (!projection.grantLeaseScope || expiresAtMs <= projection.grantLeaseScope->expiresAtMs),
    "KV restore recipient or parent binding mismatch");
  decode_state_identity_detail::requireDigest(publicKeyDigest, "recipient public key");
  NativeProtectedKvRecipientBindingV1 result;
  result.requesterIdentity = requester;
  result.recipientIdentity = ndn::Name(projection.provider);
  result.serviceName = service;
  result.requestId = ndn::Name(projection.requestId);
  result.recipientCertificateName = recipientCertificate;
  result.recipientPublicKeyDigest = publicKeyDigest;
  result.authorizationBinding = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"provider", projection.provider}, {"role", parent.roleName},
    {"grantName", projection.grantName}, {"grantDigest", projection.grantDigest},
    {"grantLeaseScope", projection.grantLeaseScope ? projection.grantLeaseScope->scopeDigest : ""}}));
  result.planDigest = projection.planDigest;
  result.roleName = parent.roleName;
  result.attemptEpoch = projection.attempt;
  result.expiresAtMs = expiresAtMs;
  return result;
}

NativeJson
nativeKvRestoreToJson(const NativeSavedKvRoleV1& saved,
  const ndn_service_framework::RecipientEncryptedAssignment& capsule,
  const NativeProtectedKvRecipientBindingV1& recipient)
{
  nativeValidateProtectedKvRecipientCapsule(capsule, saved.protection, recipient);
  NativeJson result{{"schema", "ndnsf-di-kv-restore-v1"},
    {"savedRole", nativeSavedKvRoleToJson(saved)},
    {"recipientCertificateName", recipient.recipientCertificateName.toUri()},
    {"recipientPublicKeyDigest", recipient.recipientPublicKeyDigest},
    {"expiresAtMs", recipient.expiresAtMs}, {"capsule", capsule.getFields()}};
  require(nativeCanonicalJson(result).size() <= 196608, "KV restore exceeds encoded bound");
  return result;
}

NativeKvRestoreV1
nativeReadKvRestore(const NativeJson& value, const NativeSelectionProjectionV3& projection,
  const ndn::Name& requester, const ndn::Name& service, std::uint64_t nowMs)
{
  require(nativeCanonicalJson(value).size() <= 196608, "KV restore exceeds encoded bound");
  fields(value, {"schema", "savedRole", "recipientCertificateName", "recipientPublicKeyDigest",
    "expiresAtMs", "capsule"});
  require(value.at("schema") == "ndnsf-di-kv-restore-v1" &&
    value.at("recipientCertificateName").is_string() &&
    value.at("recipientCertificateName").get_ref<const std::string&>().size() <= 4096 &&
    value.at("recipientPublicKeyDigest").is_string() && value.at("expiresAtMs").is_number_unsigned(),
    "KV restore field type mismatch");
  NativeKvRestoreV1 result;
  result.saved = nativeReadSavedKvRole(value.at("savedRole"));
  result.recipient = nativeKvRestoreRecipientBinding(projection, requester, service,
    ndn::Name(value.at("recipientCertificateName").get<std::string>()),
    value.at("recipientPublicKeyDigest").get<std::string>(),
    value.at("expiresAtMs").get<std::uint64_t>(), nowMs);
  const auto& receipt = result.saved.receipt;
  const auto& parent = *projection.conversationStateReference;
  require(result.saved.protection.ownerIdentity == requester &&
    receipt.conversationId == parent.conversationId &&
    receipt.successorContextEpoch == parent.contextEpoch &&
    receipt.serviceName == parent.serviceName && receipt.planRoleMapDigest == parent.planRoleMapDigest &&
    receipt.roleName == parent.roleName && receipt.computedDigest() == parent.roleReceiptDigest &&
    parent.expiresAtMs <= receipt.expiresAtMs &&
    receipt.securityDomainDigest == projection.securityPolicySnapshotDigest,
    "KV restore original receipt differs from selected parent");
  const auto& capsule = value.at("capsule");
  fields(capsule, {"schemaVersion", "algorithm", "recipient", "recipientCertName",
    "wrappedAssignmentKey", "nonce", "ciphertext", "authTag", "aadDigest", "assignmentDigest"});
  for (const auto& field : capsule.items()) {
    require(field.value().is_string(), "KV restore capsule field must be string");
    result.capsule.setField(field.key(), field.value().get<std::string>());
  }
  nativeValidateProtectedKvRecipientCapsule(result.capsule, result.saved.protection, result.recipient);
  return result;
}

void
nativeValidateSavedKvOrigin(const DecodeStateIdentityV1& identity,
  const ProviderConversationStateReceiptV1& receipt)
{
  identity.validate();
  receipt.validate();
  require(identity.modelDigest == receipt.modelDigest &&
    identity.graphSemanticDigest == receipt.graphSemanticDigest &&
    identity.adapterDigest == receipt.adapterDigest &&
    identity.roleName == receipt.roleName && identity.roleSplitDigest == receipt.roleSplitDigest &&
    identity.layoutDigest == receipt.layoutDigest &&
    identity.providerIdentity == receipt.providerIdentity &&
    identity.providerBootId == receipt.providerBootId && identity.cacheEpoch == receipt.cacheEpoch &&
    identity.prefixDigest == receipt.prefixDigest && identity.prefixTokenCount == receipt.prefixTokenCount &&
    identity.positionDigest == receipt.positionDigest && identity.stateSchemaDigest == receipt.stateSchemaDigest &&
    identity.stateComponentDigests == receipt.stateComponentDigests &&
    identity.securityDomainDigest == receipt.securityDomainDigest &&
    identity.requestId == receipt.originRequestId && identity.generationId == receipt.originGenerationId,
    "KV snapshot differs from authenticated receipt");
}

} // namespace ndnsf::di
