#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedKvCheckpoint.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include <functional>
#include <map>

namespace ndnsf::di {
struct NativeDurableStorageOwner;
struct ConversationStateReferenceV1;
class NativeConversationCoordinator;
struct NativeConversationRecord;
}
namespace ndn_service_framework { class ServiceProvider; }

namespace ndnsf::di {

// Keeps actual original transport names/source separate from the inner protected-file identity.
class NativeProtectedKvPublicationV1
{
public:
  NativeProtectedKvPublicationV1() = default;
  NativeProtectedKvCheckpointReferenceV1 checkpoint;
  std::string ownerIdentity;
  ndn_service_framework::LargeDataPublishResult transport;

  void requireCurrent() const;
  const std::string& inputPublicationIdentity() const noexcept { return m_inputPublicationIdentity; }

private:
  NativeProtectedKvPublicationV1(NativeProtectedKvCheckpointReferenceV1 checkpoint,
    std::string ownerIdentity, ndn_service_framework::LargeDataPublishResult transport,
    std::string inputPublicationIdentity);
  std::string m_inputPublicationIdentity;
  std::string m_corePublicationIdentity;
  std::string m_keyReferenceVersion;
  std::string m_keyEnvelope;
  std::shared_ptr<const ndn_service_framework::EncryptedLargeDataRangeSource> m_actualSource;
  friend NativeProtectedKvPublicationV1 nativePublishProtectedPortableKvCheckpoint(
    std::shared_ptr<ndn_service_framework::ServiceUser>,
    const ndn_service_framework::PreparedServiceRequest&, const std::filesystem::path&,
    const NativeProtectedKvCheckpointReferenceV1&, const NativeDurableStorageOwner&,
    const NativeProtectedKvCheckpointLimitsV1&, const std::function<void()>&);
  friend NativeProtectedKvPublicationV1 nativePublishProtectedPortableKvCheckpoint(
    std::shared_ptr<ndn_service_framework::ServiceProvider>,
    const ndn_service_framework::PreparedServiceRequest&, const std::filesystem::path&,
    const NativeProtectedKvCheckpointReferenceV1&, const NativeDurableStorageOwner&,
    std::shared_ptr<ndn_service_framework::EncryptedLargeDataRangeStore>,
    const NativeProtectedKvCheckpointLimitsV1&, const std::function<void()>&);
};

// Private-minted key-graph evidence bound to the exact current Repo claim and
// packet bundle. Root/body semantics and Owner role promotion remain separate.
class NativeVerifiedRepoProtectedKvKeyGraphV1
{
public:
  NativeVerifiedRepoProtectedKvKeyGraphV1(NativeVerifiedRepoProtectedKvKeyGraphV1&&) noexcept = default;
  NativeVerifiedRepoProtectedKvKeyGraphV1& operator=(NativeVerifiedRepoProtectedKvKeyGraphV1&&) noexcept = default;
  NativeVerifiedRepoProtectedKvKeyGraphV1(const NativeVerifiedRepoProtectedKvKeyGraphV1&) = delete;
  NativeVerifiedRepoProtectedKvKeyGraphV1& operator=(const NativeVerifiedRepoProtectedKvKeyGraphV1&) = delete;

  const ndnsf_distributed_repo::VerifiedRepoUserStoragePacketSetV1& packetSet() const noexcept
  { return m_packetSet; }
  const ndn_service_framework::VerifiedHybridMessageKeyWrapperPacketSetV1& wrapper() const noexcept
  { return m_wrapper; }
  const ndn_service_framework::VerifiedHybridMessageKeyEnvelopeV1& envelope() const noexcept
  { return m_envelope; }
  const ndn_service_framework::VerifiedNacContentKeyPacketSetV1& contentKeys() const noexcept
  { return m_contentKeys; }

private:
  NativeVerifiedRepoProtectedKvKeyGraphV1(
    ndnsf_distributed_repo::VerifiedRepoUserStoragePacketSetV1 packetSet,
    ndn_service_framework::VerifiedHybridMessageKeyWrapperPacketSetV1 wrapper,
    ndn_service_framework::VerifiedHybridMessageKeyEnvelopeV1 envelope,
    ndn_service_framework::VerifiedNacContentKeyPacketSetV1 contentKeys)
    : m_packetSet(std::move(packetSet)), m_wrapper(std::move(wrapper)),
      m_envelope(std::move(envelope)), m_contentKeys(std::move(contentKeys))
  {}

  ndnsf_distributed_repo::VerifiedRepoUserStoragePacketSetV1 m_packetSet;
  ndn_service_framework::VerifiedHybridMessageKeyWrapperPacketSetV1 m_wrapper;
  ndn_service_framework::VerifiedHybridMessageKeyEnvelopeV1 m_envelope;
  ndn_service_framework::VerifiedNacContentKeyPacketSetV1 m_contentKeys;

  friend NativeVerifiedRepoProtectedKvKeyGraphV1 nativeBindRepoProtectedKvKeyGraphV1(
    ndnsf_distributed_repo::VerifiedRepoUserStoragePacketSetV1,
    ndn_service_framework::VerifiedHybridMessageKeyWrapperPacketSetV1,
    ndn_service_framework::VerifiedHybridMessageKeyEnvelopeV1,
    ndn_service_framework::VerifiedNacContentKeyPacketSetV1,
    const std::function<void()>&);
};

// Cross-bind independently validated wrapper/CK groups to one privately minted
// Repo packet set. The RepoClient caller must have supplied its authenticated
// Owner and an actual Producer signature/authorization policy.
NativeVerifiedRepoProtectedKvKeyGraphV1 nativeBindRepoProtectedKvKeyGraphV1(
  ndnsf_distributed_repo::VerifiedRepoUserStoragePacketSetV1 packetSet,
  ndn_service_framework::VerifiedHybridMessageKeyWrapperPacketSetV1 wrapper,
  ndn_service_framework::VerifiedHybridMessageKeyEnvelopeV1 envelope,
  ndn_service_framework::VerifiedNacContentKeyPacketSetV1 contentKeys,
  const std::function<void()>& requireActive = {});

// Full remote storage semantics over one current Repo packet set. This proof
// still requires Coordinator role/checkpoint validation before promotion.
class NativeVerifiedRemoteKvPublicationV1
{
public:
  NativeVerifiedRemoteKvPublicationV1(NativeVerifiedRemoteKvPublicationV1&&) noexcept = default;
  NativeVerifiedRemoteKvPublicationV1& operator=(NativeVerifiedRemoteKvPublicationV1&&) noexcept = default;
  NativeVerifiedRemoteKvPublicationV1(const NativeVerifiedRemoteKvPublicationV1&) = delete;
  NativeVerifiedRemoteKvPublicationV1& operator=(const NativeVerifiedRemoteKvPublicationV1&) = delete;

  const NativeJson& ownerReference() const noexcept { return m_ownerReference; }
  const NativeVerifiedRepoProtectedKvKeyGraphV1& keyGraph() const noexcept { return m_keyGraph; }
  const ndn_service_framework::ProtectedBlobManifest& rootManifest() const noexcept { return m_rootManifest; }

private:
  NativeVerifiedRemoteKvPublicationV1(
    NativeJson ownerReference, NativeVerifiedRepoProtectedKvKeyGraphV1 keyGraph,
    ndn_service_framework::ProtectedBlobManifest rootManifest)
    : m_ownerReference(std::move(ownerReference)), m_keyGraph(std::move(keyGraph)),
      m_rootManifest(std::move(rootManifest))
  {}

  NativeJson m_ownerReference;
  NativeVerifiedRepoProtectedKvKeyGraphV1 m_keyGraph;
  ndn_service_framework::ProtectedBlobManifest m_rootManifest;

  friend NativeVerifiedRemoteKvPublicationV1 nativeValidateRemoteProtectedKvPublicationV1(
    NativeVerifiedRepoProtectedKvKeyGraphV1, const NativeJson&,
    const std::function<void()>&);
};

NativeVerifiedRemoteKvPublicationV1 nativeValidateRemoteProtectedKvPublicationV1(
  NativeVerifiedRepoProtectedKvKeyGraphV1 keyGraph, const NativeJson& ownerReference,
  const std::function<void()>& requireActive = {});

// Owner-side production composition: fetch the exact current packet set from
// Repo, validate the protected-KV root/wrapper/CK graph with the configured
// Producer policy, and re-observe the same Owner claim after semantic checks.
// Blocking Repo work must run off the ServiceUser Face I/O thread. The returned
// proof cannot be submitted directly to the Coordinator; remote promotion goes
// through NativeRemoteProtectedKvSetPromoter's fresh full-role operation.
NativeVerifiedRemoteKvPublicationV1 nativeFetchAndValidateRemoteProtectedKvPublicationV1(
  ndn_service_framework::ServiceUser& owner,
  const ndnsf_distributed_repo::RepoArtifactTarget& target,
  const NativeJson& ownerReference,
  const std::function<bool(const ndn::Data&)>& verifyProducerSignature,
  int timeoutMs,
  ndnsf_distributed_repo::RepoUserStoragePacketSetFetchLimits limits = {},
  const std::function<void()>& requireActive = {});

// Trusted Owner configuration for one role. These selectors and Producer
// verification policy must come from the authenticated Owner's configuration,
// never from the stored publication reference itself.
struct NativeRemoteProtectedKvRoleSourceV1
{
  ndnsf_distributed_repo::RepoArtifactTarget target;
  NativeJson ownerReference;
  std::function<bool(const ndn::Data&)> verifyProducerSignature;
  ndnsf_distributed_repo::RepoUserStoragePacketSetFetchLimits limits;
};

// Owner-configured trust policy, deliberately separate from Provider reply
// references. producerVerifierPolicyDigest identifies the verifier's pinned
// trust profile for immutable handoff-operation deduplication.
struct NativeRemoteProtectedKvRoleTrustPolicyV1
{
  ndnsf_distributed_repo::RepoArtifactTarget target;
  std::string producerVerifierPolicyDigest;
  std::function<bool(const ndn::Data&)> verifyProducerSignature;
  ndnsf_distributed_repo::RepoUserStoragePacketSetFetchLimits limits;

  bool operator==(const NativeRemoteProtectedKvRoleTrustPolicyV1& other) const noexcept
  {
    return target == other.target &&
      producerVerifierPolicyDigest == other.producerVerifierPolicyDigest &&
      limits.listingPageSize == other.limits.listingPageSize &&
      limits.maxListingPagesPerScan == other.limits.maxListingPagesPerScan &&
      limits.maxListedObjectsPerScan == other.limits.maxListedObjectsPerScan &&
      limits.maxListingWireBytes == other.limits.maxListingWireBytes &&
      limits.maxPacketCount == other.limits.maxPacketCount &&
      limits.maxPacketWireBytes == other.limits.maxPacketWireBytes &&
      limits.maxManifestWireBytes == other.limits.maxManifestWireBytes &&
      limits.ackCollectionTimeMs == other.limits.ackCollectionTimeMs;
  }

  bool operator!=(const NativeRemoteProtectedKvRoleTrustPolicyV1& other) const noexcept
  {
    return !(*this == other);
  }
};

// One bounded Owner-side operation: fetch and validate every role from current
// Repo claims, recheck all claims after the last role, then perform one full-role
// Coordinator CAS. The move-only proofs cannot be retained and promoted later.
class NativeRemoteProtectedKvSetPromoter
{
public:
  // The optional predicate is sampled on the worker before each role fetch,
  // around claim revalidation, and by the Coordinator before its CAS. Captures
  // must be safe to read from that worker thread.
  static NativeConversationRecord fetchAndPromote(
    ndn_service_framework::ServiceUser& owner,
    NativeConversationCoordinator& coordinator,
    const std::string& conversationId,
    const std::string& expectedCheckpointDigest,
    const std::map<std::string, NativeRemoteProtectedKvRoleSourceV1>& roleSources,
    int timeoutMs,
    const std::map<std::string, std::string>& nextTurnProvidersByRole = {},
    const std::function<bool()>& cancelled = {});
};

// Worker-only; caller prepares the request and keeps the Producer's Face running through return.
NativeProtectedKvPublicationV1 nativePublishProtectedPortableKvCheckpoint(
  std::shared_ptr<ndn_service_framework::ServiceUser> producer,
  const ndn_service_framework::PreparedServiceRequest& preparedRequest,
  const std::filesystem::path& protectedFile,
  const NativeProtectedKvCheckpointReferenceV1& checkpoint,
  const NativeDurableStorageOwner& owner,
  const NativeProtectedKvCheckpointLimitsV1& limits,
  const std::function<void()>& requireActive = {});

// Worker-only Provider-owned publication. The packet store and Producer are
// retained through the exact original-packet commit; the existing ServiceUser
// overload remains the compatibility path for its current callers.
NativeProtectedKvPublicationV1 nativePublishProtectedPortableKvCheckpoint(
  std::shared_ptr<ndn_service_framework::ServiceProvider> producer,
  const ndn_service_framework::PreparedServiceRequest& preparedRequest,
  const std::filesystem::path& protectedFile,
  const NativeProtectedKvCheckpointReferenceV1& checkpoint,
  const NativeDurableStorageOwner& owner,
  std::shared_ptr<ndn_service_framework::EncryptedLargeDataRangeStore> store,
  const NativeProtectedKvCheckpointLimitsV1& limits,
  const std::function<void()>& requireActive = {});

// Persisted commitments are candidates, never process-local provenance or fresh grants.
NativeJson nativeProtectedKvPublicationReference(const NativeProtectedKvPublicationV1& publication);
// Bounded stored representation only; does not prove current storage or authorization.
NativeProtectedKvCheckpointReferenceV1 nativeReadProtectedKvPublicationReference(const NativeJson& reference);
std::string nativeProtectedKvRoleBinding(const NativeJson& checkpoint, const std::string& role);
std::string nativeProtectedKvRoleBinding(const std::string& requesterIdentity,
                                        const ConversationStateReferenceV1& reference);
void nativeValidateProtectedKvReferences(const NativeJson& references, const NativeJson& checkpoint,
                                        const std::map<std::string, std::string>& providersByRole);
} // namespace ndnsf::di
