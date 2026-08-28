#ifndef NDNSF_DISTRIBUTED_INFERENCE_CONVERSATION_STATE_BINDING_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_CONVERSATION_STATE_BINDING_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/DecodeStateIdentity.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Exact Provider-local identity for state retained across Requests. */
struct ConversationStateBinding
{
  std::string conversationId;
  std::uint64_t contextEpoch = 0;
  std::string serviceName;
  std::string planRoleMapDigest;
  // Digest of the Provider-signed receipt which committed this exact local
  // role state. A later Selection resolves by this commitment rather than by
  // trusting a caller-supplied DecodeStateIdentityV1.
  std::string receiptDigest;
  // Digest of the aggregate User checkpoint that authorized this committed
  // successor. It is empty only while a candidate is staged; once committed,
  // every later Selection must match it exactly.
  std::string checkpointDigest;
  std::uint64_t expiresAtMs = 0;
  // Origin request/generation are retained for audit, but are intentionally
  // ignored by conversation-store equality for a fresh resumed Request.
  DecodeStateIdentityV1 identity;

  void
  validate() const;

  bool
  operator==(const ConversationStateBinding& other) const;
};

/** Compact role-local parent-state reference carried by one Selection. */
struct ConversationStateReferenceV1
{
  std::string conversationId;
  std::uint64_t contextEpoch = 0;
  std::string serviceName;
  std::string planRoleMapDigest;
  std::string checkpointDigest;
  std::string roleName;
  std::string roleReceiptDigest;
  std::uint64_t expiresAtMs = 0;

  void
  validate() const;
};

/** Request-scoped commitment authorizing one successor conversation epoch. */
struct ConversationTurnBindingV1
{
  std::string conversationId;
  std::uint64_t parentContextEpoch = 0;
  std::uint64_t successorContextEpoch = 0;
  std::string serviceName;
  std::string planRoleMapDigest;
  std::string requestContractDigest;
  std::uint64_t retentionDeadlineMs = 0;
  std::string parentCheckpointDigest;

  void
  validate() const;
};

/** Provider-authored commitment to one staged successor state.
 *
 * The JSON payload is carried inside Provider-signed, request-scope-encrypted
 * Collaboration Data. `receiptDigest` covers the canonical unsigned fields;
 * the NDN Data signature is the network authentication boundary and is not
 * duplicated as an application HMAC here.
 */
struct ProviderConversationStateReceiptV1
{
  std::string conversationId;
  std::uint64_t parentContextEpoch = 0;
  std::uint64_t successorContextEpoch = 0;
  std::string originRequestId;
  std::string originGenerationId;
  std::string serviceName;
  std::string requesterIdentity;
  std::string securityDomainDigest;
  std::string modelDigest;
  std::string graphSemanticDigest;
  std::string adapterDigest;
  std::string roleName;
  std::string roleSplitDigest;
  std::string layoutDigest;
  std::string planRoleMapDigest;
  std::string providerIdentity;
  std::string providerBootId;
  std::uint64_t cacheEpoch = 0;
  std::string prefixDigest;
  std::uint32_t prefixTokenCount = 0;
  std::string positionDigest;
  std::string stateSchemaDigest;
  std::vector<std::string> stateComponentDigests;
  std::uint64_t expiresAtMs = 0;

  void
  validate() const;

  std::string
  canonicalUnsignedJson() const;

  std::string
  computedDigest() const;

  std::string
  toJson() const;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_CONVERSATION_STATE_BINDING_HPP
