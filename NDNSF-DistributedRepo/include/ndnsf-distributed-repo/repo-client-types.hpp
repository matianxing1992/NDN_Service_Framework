#ifndef NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_TYPES_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_TYPES_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ndnsf_distributed_repo {

/** Completion strength requested by a publication. DURABLE only reports
 *  success once the requested consistency is confirmed; ACCEPTED only reports
 *  that a persistent operation was accepted. */
enum class RepoCompletion
{
  DURABLE,
  ACCEPTED
};

/** Lifecycle state of a publication result. */
enum class RepoPublishState
{
  ACCEPTED,
  COMMITTED
};

/** Outcome of one replica target. */
enum class RepoTargetState
{
  COMMITTED,
  ACCEPTED,
  FAILED,
  UNKNOWN
};

/** Optional per-target result; an unconfirmed target is never reported as
 *  committed. */
struct RepoReplicaResult
{
  std::string targetId;
  RepoTargetState state = RepoTargetState::UNKNOWN;
  std::string receiptId;
  std::string message;
};

/** Bounded per-operation controls. The deadline is absolute and mandatory; a
 *  callback is only invoked before the call returns. */
struct RepoOperationOptions
{
  std::chrono::steady_clock::time_point deadline{};
  std::function<void()> requireCurrent;
  /** Called with (completed, total) units; total may be zero when unknown. */
  std::function<void(uint64_t, uint64_t)> onProgress;
};

/** File identity. Field semantics follow the existing artifact reference; the
 *  wire codecs and format strings are unchanged. */
struct FileReference
{
  std::string logicalName;
  std::string digestAlgorithm = "sha256";
  std::string contentDigest;
  uint64_t sizeBytes = 0;
  std::string formatVersion = "artifact-manifest-v2";
  std::string rootManifestName;
  std::string publisherIdentity;
  std::string policyEpoch;
};

/** Ordered set of original named packets with their wire identity. */
struct ExactPacketSetReference
{
  std::string objectName;
  std::vector<std::string> packetNames;
  uint64_t wireBytes = 0;
  std::string wireDigest;
};

/** One application object described as bounded segments. */
struct SegmentedObjectReference
{
  std::string producer;
  std::string prefix;
  uint64_t payloadBytes = 0;
  std::string payloadDigest;
  uint64_t segmentBytes = 0;
  uint64_t finalSegment = 0;
};

/** Closed reference variant: an operation must know which representation it
 *  addresses before it produces any side effect. */
using RepoReference =
  std::variant<FileReference, ExactPacketSetReference, SegmentedObjectReference>;

/** Public storage reference. It is not an authorization or decryption
 *  credential, and a static receipt does not replace a current claim. */
struct RepoHandle
{
  uint32_t schemaVersion = 1;
  RepoReference reference;
  std::vector<std::string> commitReceiptIds;
  std::vector<std::string> selectedTargets;
  std::vector<RepoReplicaResult> replicaCommits;
  std::string seedRepoId;
  std::string writeConsistency;
  std::string lifecycleMode;
};

/** Local read-only source plus its verified pre-existing Repo state. */
struct StoredFileSource
{
  RepoHandle handle;
  std::string logicalName;
};

/** Requested file publication. */
struct RepoFilePublication
{
  std::string sourcePath;
  std::optional<StoredFileSource> storedSource;
  FileReference reference;
  std::vector<std::string> selectedTargets;
  std::string writeConsistency;
  uint32_t replicaCount = 1;
  std::string idempotencyKey;
  RepoCompletion completion = RepoCompletion::DURABLE;
};

/** Result of a publication or write authorization. */
struct RepoPublishResult
{
  RepoReference reference;
  RepoPublishState state = RepoPublishState::COMMITTED;
  std::optional<RepoHandle> committedHandle;
  std::vector<std::string> receiptIds;
  std::optional<std::string> operationReference;
  std::vector<RepoReplicaResult> replicas;
  bool deduplicated = false;
  bool resumed = false;
};

/** Requested file retrieval. Exactly one of reference or handle identifies the
 *  object; an optional range never claims a whole-file verification. */
struct RepoFileFetch
{
  std::optional<FileReference> reference;
  std::optional<RepoHandle> handle;
  std::string targetPath;
  bool hasRange = false;
  uint64_t rangeOffsetBytes = 0;
  uint64_t rangeLengthBytes = 0;
};

/** Result of a file retrieval. */
struct RepoFetchResult
{
  uint64_t bytesWritten = 0;
  bool digestVerified = false;
  std::string digestAlgorithm = "sha256";
  std::string contentDigest;
  std::vector<std::string> attemptedSources;
};

/** Bounded pull source of original Data packets. The source is owned by the
 *  running operation; a background task must not outlive the call. */
struct PacketSource
{
  std::function<bool(std::vector<uint8_t>&)> next;  // false when exhausted
  /** Ordered names and their wire offsets (size = names + 1). The caller owns
   *  the geometry; the facade never infers a packet set from pulled bytes. */
  std::vector<std::string> packetNames;
  std::vector<uint64_t> packetOffsets;
  uint64_t expectedWireBytes = 0;
  uint64_t expectedPacketCount = 0;
  std::string expectedDigest;
};

/** Opaque producer-side object description: names, segmentation and the signer
 *  stay with the application; no master key is accepted here. */
struct ProducerObjectSource
{
  std::string prefix;
  uint64_t payloadBytes = 0;
  uint64_t segmentBytes = 0;
  std::string payloadDigest;
};

/** Requested original-Data publication. The wire verifier is caller-owned; the
 *  Repo never invents producer trust, so a publication that needs verification
 *  must supply it (the vector is the exact original wire). */
struct RepoDataPublication
{
  std::optional<PacketSource> packets;
  std::optional<ProducerObjectSource> object;
  std::string producerIdentity;
  std::string targetId;
  /** Node name of the target; derived only when the deployment does not give
   *  one, because a Repo node name is not always the provider's last component. */
  std::string repoNode;
  /** Owner authorization the writer was given; empty means self-owned. */
  std::string ownerIdentity;
  std::string grantId;
  uint64_t ownerEpoch = 0;
  uint64_t generation = 0;
  /** Immutable object name; required when the packet source is empty. */
  std::string objectName;
  std::string idempotencyKey;
  /** Shared so copying a publication never deep-copies the verifier. */
  std::shared_ptr<const std::function<bool(const std::vector<uint8_t>&)>> verifyWire;
};

class VerifiedRepoUserStoragePacketSetV1;
struct RepoObjectManifest;

/** Manifest-bound retrieval: the caller already holds a committed manifest for
 *  the object (for example from an Owner commitment) and asks Repo for exactly
 *  those packets. This is not a current Owner claim and confers no authority. */
struct RepoManifestBoundFetch
{
  /** Committed manifest the caller holds; Repo transfers exactly its packets. */
  std::shared_ptr<const RepoObjectManifest> manifest;
  std::string objectName;
  std::string dataPrefix;
  std::string expectedSha256;
  uint64_t expectedWireBytes = 0;
  std::string producerIdentity;
  uint64_t maxPacketCount = 65536;
  uint64_t maxPacketWireBytes = 256ULL * 1024 * 1024;
  uint64_t maxManifestWireBytes = 4ULL * 1024 * 1024;
  uint32_t ackCollectionTimeMs = 1000;
};

/** Owner-claim retrieval: the same fetch operation in its verified Owner mode.
 *  The result carries the transport proof RepoClient mints, never an execution
 *  authorization. */
struct RepoOwnerClaimFetch
{
  std::string objectName;
  std::string targetId;
  std::string repoNode;
  uint32_t ackCollectionTimeMs = 1000;
  uint64_t maxPacketCount = 65536;
  uint64_t maxPacketWireBytes = 256ULL * 1024 * 1024;
  uint64_t maxManifestWireBytes = 4ULL * 1024 * 1024;
};

/** Requested original-Data retrieval. */
struct RepoDataFetch
{
  std::optional<ExactPacketSetReference> packets;
  std::optional<SegmentedObjectReference> object;
  /** Ordered names with a byte budget but no whole-set digest commitment: the
   *  shape a caller uses when it verifies each packet itself (an established
   *  fetcher contract). Never treated as a verified packet set. */
  std::vector<std::string> packetNames;
  uint64_t maxWireBytes = 0;
  std::string targetId;
  std::string repoNode;
  /** When set, the retrieval verifies and returns an Owner-claimed packet set
   *  instead of an unproven wire list. */
  std::optional<RepoOwnerClaimFetch> ownerClaim;
  /** When set, the retrieval is bound to the caller's committed manifest. */
  std::optional<RepoManifestBoundFetch> manifestBound;
  uint64_t rangeOffsetBytes = 0;
  uint64_t rangeLengthBytes = 0;
  bool hasRange = false;
  std::shared_ptr<const std::function<bool(const std::vector<uint8_t>&)>> verifyWire;
};

/** Result of an original-Data retrieval. */
struct RepoDataFetchResult
{
  std::vector<std::vector<uint8_t>> packets;
  uint64_t wireBytes = 0;
  std::string wireDigest;
  bool digestVerified = false;
  std::vector<std::string> attemptedSources;
  /** Present only for an Owner-claim fetch; minted by RepoClient after matching
   *  the manifest, every indexed packet and the ordered wire digest. */
  std::shared_ptr<const VerifiedRepoUserStoragePacketSetV1> ownerPacketSet;
};

/** Owner-scoped listing or a Repo inventory page. */
enum class RepoListScope
{
  OWNER_OBJECTS,
  REPO_INVENTORY
};

struct RepoListRequest
{
  RepoListScope scope = RepoListScope::OWNER_OBJECTS;
  std::string targetId;
  std::string repoNode;
  std::string cursor;
  uint32_t limit = 100;
  std::string catalogRevision;
};

struct RepoListItem
{
  RepoReference reference;
  uint64_t storedBytes = 0;
  std::string generation;
  std::string lifecycleState;
};

struct RepoPage
{
  std::vector<RepoListItem> items;
  std::string nextCursor;
  std::string catalogRevision;
};

/** Closed inspection targets. */
struct RepoOwnerScope
{
  std::string ownerIdentity;
  std::string objectName;
};

/** Read-only, copyable claim reference; it carries no payload bytes. */
struct RepoClaimReference
{
  std::string targetId;
  std::string ownerIdentity;
  std::string objectName;
  uint64_t ownerEpoch = 0;
  uint64_t generation = 0;
};

struct RepoInspectionRequest
{
  enum class Kind
  {
    OBJECT,
    RECEIPT,
    OPERATION,
    OWNER,
    NODE,
    CLAIM
  };
  Kind kind = Kind::OBJECT;
  std::optional<RepoHandle> object;
  std::string receiptId;
  std::string operationReference;
  RepoOwnerScope owner;
  std::string nodeId;
  RepoClaimReference claim;
  bool probeMetadata = true;
  bool probeLiveness = false;
};

/** Typed observation with its time and observer, never an authorization. */
struct RepoInspection
{
  enum class Availability
  {
    AVAILABLE,
    UNAVAILABLE,
    UNKNOWN
  };
  Availability availability = Availability::UNKNOWN;
  std::chrono::system_clock::time_point observedAt{};
  std::string observerId;
  std::optional<RepoHandle> handle;
  std::optional<uint64_t> storedBytes;
  std::string lifecycleState;
  std::optional<uint64_t> claimEpoch;
  /** True only when the same claim still validates against authority. */
  bool claimStillCurrent = false;
};

struct RepoDeleteRequest
{
  RepoReference reference;
  std::string generation;
  std::string expectedEpoch;
  std::vector<std::string> targets;
};

struct RepoDeleteResult
{
  std::vector<RepoReplicaResult> targets;
  uint32_t removedCount = 0;
  bool confirmed = false;
};

/** Results reuse the maintained candidate type so discovery keeps one shape;
 *  the facade only projects the requested requirements. */
struct RepoDiscoveryRequest
{
  std::vector<std::string> requiredCapabilities;
  std::string providerScope;
  uint32_t ackCollectionMs = 1000;
};

struct RepoWriteAuthorization
{
  std::string targetId;
  std::string repoNode;
  std::string writerIdentity;
  std::string objectName;
  std::string grantId;
  uint64_t expiresAtMs = 0;
};

struct RepoWriteGrant
{
  std::string ownerIdentity;
  std::string writerIdentity;
  std::string objectName;
  std::string targetId;
  std::string grantId;
  uint64_t ownerEpoch = 0;
  uint64_t expiresAtMs = 0;
};

/** Confirmed progress retained by a structured failure. It never contains
 *  secrets or a claim proof that could be reused as authorization. */
struct RepoPartialProgress
{
  std::vector<std::string> confirmedGroups;
  std::vector<std::string> replicaReceipts;
  std::vector<std::string> unconfirmedTargets;
  std::vector<std::string> failedAttempts;
};

/** Structured operation failure: a stable code, the phase it failed in, the
 *  operation reference when one was allocated, and confirmed progress. */
class RepoClientError : public std::runtime_error
{
public:
  RepoClientError(std::string code, std::string message,
                  std::string phase, std::string operationReference = {},
                  RepoPartialProgress partial = {})
    : std::runtime_error(std::move(message))
    , m_code(std::move(code))
    , m_phase(std::move(phase))
    , m_operationReference(std::move(operationReference))
    , m_partial(std::move(partial))
  {
  }

  const std::string&
  code() const noexcept
  {
    return m_code;
  }

  const std::string&
  phase() const noexcept
  {
    return m_phase;
  }

  const std::optional<std::string>&
  operationReference() const noexcept
  {
    return m_operationReference;
  }

  const RepoPartialProgress&
  partial() const noexcept
  {
    return m_partial;
  }

private:
  std::string m_code;
  std::string m_phase;
  std::optional<std::string> m_operationReference;
  RepoPartialProgress m_partial;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_TYPES_HPP
