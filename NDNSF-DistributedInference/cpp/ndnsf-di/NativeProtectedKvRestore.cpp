#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedKvRestore.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedKvPublication.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/DecodeStateIdentity.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"

namespace ndnsf::di {
namespace {
void require(bool valid)
{
  if (!valid) throw std::invalid_argument("invalid protected KV restore context");
}
void exact(const NativeJson& value, std::initializer_list<const char*> fields)
{
  require(value.is_object() && value.size() == fields.size());
  for (const auto field : fields) require(value.contains(field));
}
const std::string& text(const NativeJson& value, const char* field, std::size_t maximum = 4096)
{
  require(value.at(field).is_string());
  const auto& result = value.at(field).get_ref<const std::string&>();
  require(!result.empty() && result.size() <= maximum); return result;
}
void checkDigest(const std::string& value)
{
  require(decode_state_identity_detail::isSha256Digest(value));
}
ndn::Name name(const std::string& value)
{
  require(!value.empty() && value.size() <= 4096);
  const ndn::Name result(value);
  require(!result.empty() && result.wireEncode().size() <= 4096); return result;
}
void checkRecipient(const NativeProtectedKvRestoreRecipientV1& recipient)
{
  name(recipient.certificateName.toUri(ndn::name::UriFormat::CANONICAL));
  checkDigest(recipient.publicKeyDigest); require(recipient.expiresAtMs > 0);
}
std::optional<ProviderConversationStateReceiptV1>
checkRepresentation(const NativeProtectedKvRestoreV1& restore)
{
  const auto reference = nativeReadProtectedKvPublicationReference(restore.reference);
  require(!reference.keyEnvelope.empty()); checkRecipient(restore.recipient);
  NativeProtectedKvRecipientBindingV1 binding;
  // Certificate identity extraction is structural, not signer authorization.
  binding.recipientIdentity = ndn::security::extractIdentityFromCertName(restore.recipient.certificateName);
  binding.recipientCertificateName = restore.recipient.certificateName;
  nativeValidateProtectedKvRecipientCapsule(restore.capsule, binding);
  if (!restore.originReceipt) return std::nullopt;
  require(!restore.originReceipt->empty() && restore.originReceipt->size() <= 65536);
  return nativeReadProviderConversationStateReceipt(nativeParseJson(*restore.originReceipt));
}
void checkProjection(const NativeSelectionProjectionV3& projection,
                     const NativeProtectedKvRestoreRecipientV1& recipient, std::uint64_t nowMs)
{
  checkRecipient(recipient);
  require(projection.hasGrantBinding && projection.conversationStateReference && projection.conversationTurnBinding);
  const auto& state = *projection.conversationStateReference;
  const auto& turn = *projection.conversationTurnBinding;
  state.validate(); turn.validate();
  require(name(state.serviceName) == name(turn.serviceName) &&
    state.roleName == projection.executionRole.roleId && state.conversationId == turn.conversationId &&
    state.contextEpoch == turn.parentContextEpoch && state.checkpointDigest == turn.parentCheckpointDigest &&
    state.planRoleMapDigest == turn.planRoleMapDigest &&
    recipient.expiresAtMs > nowMs && recipient.expiresAtMs <= projection.deadlineMs &&
    recipient.expiresAtMs <= state.expiresAtMs && recipient.expiresAtMs <= turn.retentionDeadlineMs);
}

void checkOrigin(const std::optional<ProviderConversationStateReceiptV1>& receipt,
                 const NativeProtectedKvRestoreV1& restore,
                 const NativeSelectionProjectionV3& projection, std::uint64_t nowMs)
{
  if (!receipt) return; // Legacy representation remains decodable, not remote-import proof.
  const auto reference = nativeReadProtectedKvPublicationReference(restore.reference);
  const auto& state = *projection.conversationStateReference;
  require(receipt->computedDigest() == state.roleReceiptDigest &&
    receipt->conversationId == state.conversationId &&
    receipt->successorContextEpoch == state.contextEpoch && receipt->roleName == state.roleName &&
    name(receipt->serviceName) == name(state.serviceName) &&
    name(receipt->requesterIdentity) == reference.ownerIdentity &&
    name(receipt->providerIdentity) == reference.producerPrefix &&
    receipt->expiresAtMs > nowMs && receipt->expiresAtMs >= state.expiresAtMs &&
    receipt->expiresAtMs >= restore.recipient.expiresAtMs);
  // The receipt's digest commits the historical map. The current selected map
  // and recipient may legitimately differ; never rewrite that old commitment.
}
}

std::string nativeProtectedKvAuthorizationBinding(const std::string& provider,
  const std::string& role, const std::string& grantName, const std::string& grantDigest,
  const std::string& leaseScopeDigest)
{
  (void)name(provider); (void)name(grantName);
  require(!role.empty() && role.size() <= 4096); checkDigest(grantDigest);
  if (!leaseScopeDigest.empty()) checkDigest(leaseScopeDigest);
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"version", 1U}, {"purpose", "NDNSF-DI/KV-AUTHORIZATION-BINDING/V1"},
    {"provider", provider}, {"role", role}, {"grantName", grantName},
    {"grantDigest", grantDigest}, {"leaseScopeDigest", leaseScopeDigest}}));
}

NativeJson nativeProtectedKvRestoreToJson(const NativeProtectedKvRestoreV1& restore)
{
  checkRepresentation(restore);
  NativeJson result{{"version", 1U}, {"reference", restore.reference},
    {"recipient", {{"certificateName", restore.recipient.certificateName.toUri(ndn::name::UriFormat::CANONICAL)},
      {"publicKeyDigest", restore.recipient.publicKeyDigest}, {"expiresAtMs", restore.recipient.expiresAtMs}}},
    {"capsule", {{"version", 1U}, {"fields", restore.capsule.getFields()}}}};
  if (restore.originReceipt) {
    result["version"] = 2U;
    result["originReceipt"] = nativeConversationBase64Encode(*restore.originReceipt);
  }
  require(nativeCanonicalJson(result).size() <= (restore.originReceipt ? 196608U : 65536U));
  return result;
}

NativeProtectedKvRestoreV1 nativeProtectedKvRestoreFromJson(const NativeJson& value)
{
  require(value.is_object() && value.contains("version") && value.at("version").is_number_unsigned());
  const auto version = value.at("version").get<std::uint64_t>();
  require(version == 1 || version == 2);
  if (version == 1) exact(value, {"version", "reference", "recipient", "capsule"});
  else exact(value, {"version", "reference", "recipient", "capsule", "originReceipt"});
  // Bound every nested field before copying or canonicalizing caller data.
  (void)nativeReadProtectedKvPublicationReference(value.at("reference"));
  NativeProtectedKvRestoreV1 result; result.reference = value.at("reference");
  const auto& recipient = value.at("recipient");
  exact(recipient, {"certificateName", "publicKeyDigest", "expiresAtMs"});
  const auto& certificateName = text(recipient, "certificateName");
  result.recipient.certificateName = name(certificateName);
  require(result.recipient.certificateName.toUri(ndn::name::UriFormat::CANONICAL) == certificateName);
  result.recipient.publicKeyDigest = text(recipient, "publicKeyDigest", 71);
  require(recipient.at("expiresAtMs").is_number_unsigned());
  result.recipient.expiresAtMs = recipient.at("expiresAtMs").get<std::uint64_t>();
  const auto& capsule = value.at("capsule"); exact(capsule, {"version", "fields"});
  require(capsule.at("version").is_number_unsigned() && capsule.at("version").get<std::uint64_t>() == 1);
  const auto& fields = capsule.at("fields"); require(fields.is_object() && fields.size() == 10);
  for (const auto& field : fields.items()) result.capsule.setField(field.key(), text(fields, field.key().c_str()));
  if (version == 2) {
    const auto& encoded = text(value, "originReceipt", 87384);
    auto wire = nativeConversationBase64Decode(encoded);
    require(!wire.empty() && wire.size() <= 65536 && nativeConversationBase64Encode(wire) == encoded);
    result.originReceipt = std::move(wire);
  }
  checkRepresentation(result);
  require(nativeCanonicalJson(value).size() <= (version == 2 ? 196608U : 65536U)); return result;
}

NativeProtectedKvRecipientBindingV1 nativeProtectedKvRecipientBindingForProjection(
  const NativeSelectionProjectionV3& projection, const ndn::Name& expectedRequester,
  const ndn::Name& expectedService, const NativeProtectedKvRestoreRecipientV1& recipient,
  std::uint64_t nowMs)
{
  checkProjection(projection, recipient, nowMs);
  require(!expectedRequester.empty() && !expectedService.empty());
  const auto& state = *projection.conversationStateReference;
  const auto& turn = *projection.conversationTurnBinding;
  require(name(state.serviceName) == expectedService && name(turn.serviceName) == expectedService);
  NativeProtectedKvRecipientBindingV1 binding;
  binding.requesterIdentity = expectedRequester; binding.recipientIdentity = name(projection.provider);
  binding.serviceName = expectedService; binding.requestId = name(projection.requestId);
  binding.recipientCertificateName = recipient.certificateName;
  require(ndn::Name(binding.recipientIdentity).append("KEY").isPrefixOf(recipient.certificateName));
  binding.recipientPublicKeyDigest = recipient.publicKeyDigest;
  binding.authorizationBinding = nativeProtectedKvAuthorizationBinding(projection.provider, state.roleName,
    projection.grantName, projection.grantDigest,
    projection.grantLeaseScope ? projection.grantLeaseScope->scopeDigest : std::string{});
  binding.roleName = state.roleName; binding.planDigest = projection.planDigest;
  checkDigest(binding.planDigest); require(projection.attempt >= 1 && projection.attempt <= 2);
  binding.attemptEpoch = projection.attempt; binding.expiresAtMs = recipient.expiresAtMs;
  return binding;
}

void nativeValidateProtectedKvRestoreProjection(const NativeProtectedKvRestoreV1& restore,
                                               const NativeSelectionProjectionV3& projection)
{
  const auto origin = checkRepresentation(restore);
  checkProjection(projection, restore.recipient, 0);
  checkOrigin(origin, restore, projection, 0);
  NativeProtectedKvRecipientBindingV1 binding;
  binding.recipientIdentity = name(projection.provider);
  binding.recipientCertificateName = restore.recipient.certificateName;
  nativeValidateProtectedKvRecipientCapsule(restore.capsule, binding);
}

void nativeValidateProtectedKvRestoreContext(const NativeProtectedKvRestoreV1& restore,
  const NativeSelectionProjectionV3& projection, const ndn::Name& expectedRequester,
  const ndn::Name& expectedService, std::uint64_t nowMs)
{
  const auto origin = checkRepresentation(restore);
  const auto binding = nativeProtectedKvRecipientBindingForProjection(projection, expectedRequester,
    expectedService, restore.recipient, nowMs);
  const auto reference = nativeReadProtectedKvPublicationReference(restore.reference);
  require(reference.ownerIdentity == expectedRequester);
  checkOrigin(origin, restore, projection, nowMs);
  // Historical checkpoints commit the requester's URI spelling verbatim.
  // Accept both supported spellings without consulting the default URI mode.
  const auto& state = *projection.conversationStateReference;
  require(reference.checkpointIdentityDigest == nativeProtectedKvRoleBinding(
      expectedRequester.toUri(ndn::name::UriFormat::CANONICAL), state) ||
    reference.checkpointIdentityDigest == nativeProtectedKvRoleBinding(
      expectedRequester.toUri(ndn::name::UriFormat::ALTERNATE), state));
  nativeValidateProtectedKvRecipientCapsule(restore.capsule, reference, binding);
}
} // namespace ndnsf::di
