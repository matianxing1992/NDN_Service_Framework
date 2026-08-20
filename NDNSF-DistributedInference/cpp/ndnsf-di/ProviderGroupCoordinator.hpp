#ifndef NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_GROUP_COORDINATOR_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_GROUP_COORDINATOR_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollectiveControl.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::di {

using ProviderGroupBytes = std::vector<std::uint8_t>;

struct GroupMemberV1
{
  std::string provider;
  std::uint64_t rank = 0;
  std::string offerDigest;
  std::string endpointPrefix;
};

struct GroupOperationV1
{
  std::uint64_t operationIndex = 0;
  std::string kind;
  std::vector<std::string> producerRanks;
  std::vector<std::string> consumerRanks;
  std::string tensorLayoutDigest;
  std::uint64_t maxBytes = 0;
  std::uint64_t maxSegments = 0;
};

/**
 * The authenticated, request-scoped capability projected to each Provider.
 * The plaintext epoch key is deliberately not a field of this structure.
 */
struct GroupCapabilityV1
{
  std::string requestId;
  std::string attemptId;
  std::string planDigest;
  std::string groupId;
  std::uint64_t epoch = 0;
  std::vector<GroupMemberV1> orderedMembers;
  std::vector<GroupOperationV1> permittedOperations;
  std::uint64_t maxInflightBytes = 0;
  std::uint64_t noProgressMs = 0;
  std::uint64_t hardDeadlineMs = 0;
  std::string epochKeyId;
  /** Signed commitments for every member's Provider-specific envelope. */
  std::map<std::string, std::string> wrappedEpochKeyDigestByProvider;
  /** Delivery projection: all envelopes at the sealer, one at a Provider. */
  std::map<std::string, ProviderGroupBytes> wrappedEpochKeyByProvider;
  std::string capabilityDigest;
  ProviderGroupBytes sealerSignature;

  void validate() const;
  ProviderGroupBytes canonicalBytes(bool includeDigest = false) const;
  GroupCapabilityV1 projectForProvider(const std::string& provider) const;
};

struct CollectiveOperationManifestV1
{
  std::string capabilityDigest;
  std::string epochKeyId;
  std::string requestId;
  std::string attemptId;
  std::string planDigest;
  std::string groupId;
  std::uint64_t epoch = 0;
  std::uint64_t operationIndex = 0;
  std::string operationKind;
  std::string producerRank;
  std::string sourceLayoutDigest;
  std::string targetLayoutDigest;
  std::string tensorDigest;
  std::uint64_t totalBytes = 0;
  std::uint64_t segmentSize = 0;
  std::uint64_t segmentCount = 0;
  std::vector<std::string> orderedSegmentDigests;
  std::uint64_t createdAtMs = 0;
  std::uint64_t noProgressMs = 0;
  std::uint64_t hardDeadlineMs = 0;
  ProviderGroupBytes producerSignature;

  void validate() const;
  ProviderGroupBytes canonicalBytes(bool includeSignature = false) const;
  std::string digest() const;
};

struct ProviderGroupCoordinatorOptions
{
  using RandomBytesFn = std::function<ProviderGroupBytes(std::size_t)>;
  using KeyWrapFn = std::function<ProviderGroupBytes(
    const std::string& provider, const ProviderGroupBytes& epochKey)>;
  using KeyUnwrapFn = std::function<ProviderGroupBytes(
    const std::string& provider, const ProviderGroupBytes& wrappedEpochKey)>;
  using SignFn = std::function<ProviderGroupBytes(const ProviderGroupBytes&)>;
  using VerifyFn = std::function<bool(const ProviderGroupBytes&,
                                     const ProviderGroupBytes&)>;

  RandomBytesFn randomBytes;
  /** Provider identity used to unwrap a Selection-delivered epoch key. */
  std::string localProvider;
  KeyWrapFn wrapEpochKey;
  KeyUnwrapFn unwrapEpochKey;
  SignFn signCapability;
  VerifyFn verifyCapability;
  SignFn signManifest;
  VerifyFn verifyManifest;
  // When explicit callbacks are absent, the coordinator uses HMAC-SHA256
  // with the request-scoped epoch key.  The outer authenticated Selection
  // binds the capability issuer; signed SVSPubSub Data binds the producer
  // transport identity.  The inner MAC prevents tampering/replay across
  // request, attempt, group, epoch, and operation bindings.
  std::size_t maxSegments = 1U << 20;
  std::uint64_t maxInflightBytes = 64ULL << 20;
};

struct SealedCollectiveOperationV1
{
  CollectiveOperationManifestV1 manifest;
  std::vector<NdnsfDataV1Segment> segments;
};

/**
 * Request-scoped coordinator for the mandatory cross-Provider data profile.
 * It owns the epoch key only until terminal completion/cancellation and
 * refuses to expose plaintext until the complete authenticated operation is
 * assembled.
 */
class ProviderGroupCoordinator
{
public:
  explicit ProviderGroupCoordinator(ProviderGroupCoordinatorOptions options = {});
  ~ProviderGroupCoordinator();

  GroupCapabilityV1
  createCapability(std::string requestId,
                   std::string attemptId,
                   std::string planDigest,
                   std::string groupId,
                   std::uint64_t epoch,
                   std::vector<GroupMemberV1> orderedMembers,
                   std::vector<GroupOperationV1> permittedOperations,
                   std::uint64_t maxInflightBytes,
                   std::uint64_t noProgressMs,
                   std::uint64_t hardDeadlineMs);

  void
  installCapability(GroupCapabilityV1 capability,
                    ProviderGroupBytes epochKey,
                    bool verifySignature = true);

  const GroupCapabilityV1& capability() const;
  bool hasCapability() const noexcept;

  ProviderGroupBytes
  epochKeyForProvider(const std::string& provider) const;

  /** Canonical bounded wire form carried inside a Provider assignment. */
  static ProviderGroupBytes
  encodeCapability(const GroupCapabilityV1& capability);

  static GroupCapabilityV1
  decodeCapability(const ProviderGroupBytes& wire);

  CollectiveOperationManifestV1
  makeManifest(const GroupOperationV1& operation,
               const std::string& producerRank,
               const std::string& sourceLayoutDigest,
               const std::string& targetLayoutDigest,
               const std::string& tensorDigest,
               const std::vector<ProviderGroupBytes>& plaintextSegments,
               std::uint64_t createdAtMs) const;

  SealedCollectiveOperationV1
  sealOperation(const GroupOperationV1& operation,
                const std::string& producerRank,
                const std::string& sourceLayoutDigest,
                const std::string& targetLayoutDigest,
                const std::string& tensorDigest,
                const std::vector<ProviderGroupBytes>& plaintextSegments,
                std::uint64_t createdAtMs,
                const std::vector<std::string>& exactDataNames = {});

  ProviderGroupBytes
  signTensorObjectManifest(const ProviderGroupBytes& signingBytes) const;

  bool
  verifyTensorObjectManifest(const ProviderGroupBytes& signingBytes,
                             const ProviderGroupBytes& signature) const;

  /** Bounded transport framing for the signed manifest followed by segments. */
  static ProviderGroupBytes
  encodeOperation(const SealedCollectiveOperationV1& operation);

  static SealedCollectiveOperationV1
  decodeOperation(const ProviderGroupBytes& wire);

  /**
   * Encode one manifest-bound segment for publication through SVSPubSub.
   * Each segment is independently fetchable and repairable; the manifest's
   * segmentCount remains the operation-wide count.
   */
  static ProviderGroupBytes
  encodeSegment(const CollectiveOperationManifestV1& manifest,
                const NdnsfDataV1Segment& segment);

  static SealedCollectiveOperationV1
  decodeSegment(const ProviderGroupBytes& wire);

  ProviderGroupBytes
  openSegment(const CollectiveOperationManifestV1& manifest,
              const NdnsfDataV1Segment& segment,
              const std::string& expectedDataName = {});

  DataSegmentReplayWindow::Result
  acceptSegment(const CollectiveOperationManifestV1& manifest,
                const NdnsfDataV1Segment& segment,
                const std::string& expectedDataName = {});

  bool recordProgress(std::uint64_t nowMs);
  bool deadlineExpired(std::uint64_t nowMs) const;
  void cancel(std::string reason);
  void fail(std::string reason);
  bool terminal() const noexcept;
  bool cancelled() const noexcept;
  bool failed() const noexcept;
  const std::string& terminalReason() const noexcept;
  void clearEpochKey() noexcept;

  static std::string
  makeDataName(const GroupCapabilityV1& capability,
               const CollectiveOperationManifestV1& manifest,
               std::uint64_t segmentNo);

  static ProviderGroupBytes
  deriveOperationKey(const ProviderGroupBytes& epochKey,
                     const GroupCapabilityV1& capability,
                     const CollectiveOperationManifestV1& manifest);

  static ProviderGroupBytes
  deriveNonce(const GroupCapabilityV1& capability,
              const CollectiveOperationManifestV1& manifest,
              const std::string& exactDataName,
              std::uint64_t segmentNo);

private:
  const GroupOperationV1& findOperation(std::uint64_t operationIndex) const;
  void validateManifestAgainstCapability(
    const CollectiveOperationManifestV1& manifest) const;
  DataSegmentReplayWindow& replayWindow(
    const CollectiveOperationManifestV1& manifest);

private:
  ProviderGroupCoordinatorOptions m_options;
  GroupCapabilityV1 m_capability;
  ProviderGroupBytes m_epochKey;
  bool m_hasCapability = false;
  bool m_cancelled = false;
  bool m_failed = false;
  std::string m_terminalReason;
  std::uint64_t m_groupStartedAtMs = 0;
  std::uint64_t m_lastProgressMs = 0;
  std::map<std::string, std::unique_ptr<DataSegmentReplayWindow>> m_replayWindows;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_PROVIDER_GROUP_COORDINATOR_HPP
