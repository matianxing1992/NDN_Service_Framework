#ifndef NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_HPP

#include "ndnsf-distributed-repo/artifact-client-types.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/repo-client-types.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-info.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace ndn_service_framework {
class LocalServiceRegistry;
}

namespace ndnsf_distributed_repo {

class RepoNode;

struct OriginalDataReplica
{
  std::string repoNode;
  ndn::Name providerIdentity;
  ndn::Name servicePrefix;
};

struct RepoUserStoragePacketSetFetchLimits
{
  uint32_t listingPageSize = 100;
  // Applied independently to the initial claim scan and the final recheck.
  uint32_t maxListingPagesPerScan = 128;
  uint64_t maxListedObjectsPerScan = 12800;
  // Applies cumulatively across both scans.
  uint64_t maxListingWireBytes = 16ULL << 20;
  uint32_t maxPacketCount = 4096;
  uint64_t maxPacketWireBytes = 64ULL << 20;
  uint64_t maxManifestWireBytes = 4ULL << 20;
  int ackCollectionTimeMs = 200;
  // Per-operation exact Interest cap for segment-manifest retrieval. Zero is adaptive.
  uint32_t manifestFetchWindow = 0;
  // Opt in only with a Repo deployment that supports targeted Owner queries.
  bool targetedExactOwnerClaims = false;
};

/**
 * Immutable transport result for one exact, currently committed Owner claim.
 * RepoClient mints this only after matching the manifest to that claim, fetching
 * every indexed original Data packet, checking the ordered wire digest, and
 * observing the same claim again. It does not validate protected-KV semantics
 * or grant execution authority.
 */
class VerifiedRepoUserStoragePacketSetV1
{
public:
  VerifiedRepoUserStoragePacketSetV1(VerifiedRepoUserStoragePacketSetV1&&) noexcept = default;
  VerifiedRepoUserStoragePacketSetV1& operator=(VerifiedRepoUserStoragePacketSetV1&&) noexcept = default;
  VerifiedRepoUserStoragePacketSetV1(const VerifiedRepoUserStoragePacketSetV1&) = delete;
  VerifiedRepoUserStoragePacketSetV1& operator=(const VerifiedRepoUserStoragePacketSetV1&) = delete;

  const RepoArtifactTarget& target() const noexcept { return m_target; }
  const RepoUserStorageClaim& claim() const noexcept { return m_claim; }
  const RepoObjectManifest& manifest() const noexcept { return m_manifest; }
  const std::vector<std::vector<uint8_t>>& packetWires() const noexcept { return m_packetWires; }

private:
  VerifiedRepoUserStoragePacketSetV1(RepoArtifactTarget target,
    RepoUserStorageClaim claim, RepoObjectManifest manifest,
    std::vector<std::vector<uint8_t>> packetWires)
    : m_target(std::move(target)), m_claim(std::move(claim)),
      m_manifest(std::move(manifest)), m_packetWires(std::move(packetWires))
  {}

  RepoArtifactTarget m_target;
  RepoUserStorageClaim m_claim;
  RepoObjectManifest m_manifest;
  std::vector<std::vector<uint8_t>> m_packetWires;
  friend class RepoClient;
};

struct StoreOptions
{
  std::string objectType = "object";
  uint32_t replicationFactor = 1;
  std::vector<std::string> replicaNodes;
  std::string policyEpoch;
};

class RepoClient
{
public:
  /* Unified Repo client facade. The nine operations below are the whole business surface;
   * the statics in this class are factories plus the Owner proof minter, and the migration
   * helpers live in headers that are deliberately not installed. */

  /** Open a client that owns an in-process Repo (no network). */
  static RepoClient makeInApp(InAppRepoConfig config);

  /** Open a client over an existing service user; most operations need this form. */
  static RepoClient makePersistent(std::shared_ptr<ndn_service_framework::ServiceUser> user,
                                   PersistentRepoClientConfig config = {});

  RepoClient(RepoClient&&) noexcept;
  RepoClient& operator=(RepoClient&&) noexcept;
  ~RepoClient();

  RepoClient(const RepoClient&) = delete;
  RepoClient& operator=(const RepoClient&) = delete;

  /** Publish a file as a chunked artifact with its manifest.
   *
   * Selected targets are validated against fresh authenticated discovery, so the Repo must
   * advertise an artifact capability that satisfies the reference. **Not qualified against the
   * maintained Repo**: its current advertisement is rejected by that check, which is why this
   * operation has only fixture-level evidence. */
  RepoPublishResult publishFile(const RepoFilePublication& input,
                                const RepoOperationOptions& options);

  /** Publish an original packet set under an Owner grant.
   *
   * `grantId`, `ownerIdentity` and `ownerEpoch` must come from authorizeWrite. Multi-packet
   * sets are supported; the Repo names the set by its first packet name. */
  RepoPublishResult publishData(const RepoDataPublication& input,
                                const RepoOperationOptions& options);

  /** Publish Data owned by a locally prepared manifest. The packet source supplies
   * geometry and the expected digest through input.packets, with no wire callback.
   * The prepared packet set is used only during this call. */
  RepoPublishResult publishPreparedManifest(
    const RepoDataPublication& input, const ndn_service_framework::ManifestPublication& prepared,
    const RepoOperationOptions& options);

  /** Retrieve an artifact by exactly one of a committed handle or a file reference. */
  RepoFetchResult fetchFile(const RepoFileFetch& input,
                            const RepoOperationOptions& options) const;

  /** Retrieve original Data in one of three modes: explicit names, a caller-held manifest bound,
   *  or an Owner claim that returns the Repo's verified proof. */
  RepoDataFetchResult fetchData(const RepoDataFetch& input,
                                const RepoOperationOptions& options) const;

  /** Ask the Repo for an Owner write grant, which publishData then presents. */
  RepoWriteGrant authorizeWrite(const RepoWriteAuthorization& input,
                                const RepoOperationOptions& options);

  /** Observe one object, receipt, or publication operation. Owner, node and claim inspections
   *  are refused; an OBJECT probe reports availability and size but no lifecycle state. */
  RepoInspection inspect(const RepoInspectionRequest& input,
                         const RepoOperationOptions& options) const;

  /** Page the authenticated Owner listing. Each entry carries the claim that `remove` needs. */
  RepoPage list(const RepoListRequest& input,
                const RepoOperationOptions& options) const;

  /** Delete one object using the claim from a listing entry, or a file reference, plus a unique
   *  cleanup id so a retry is idempotent rather than a second deletion. */
  RepoDeleteResult remove(const RepoDeleteRequest& input,
                          const RepoOperationOptions& options);

  /** Discover Repo candidates for an operation. */
  RepoCandidateSet discover(const RepoDiscoveryRequest& input,
                            const RepoOperationOptions& options) const;

  /** Bounds this client enforces on one publication group. */
  RepoPublicationLimits
  publicationLimits() const;

  bool close(std::chrono::steady_clock::time_point deadline);

private:
  RepoPublishResult publishDataImpl(const RepoDataPublication& input,
                                    const ndn_service_framework::ManifestPublication* prepared,
                                    const RepoOperationOptions& options);
  struct Impl;
  explicit RepoClient(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;

public:
  static constexpr const char* DEFAULT_SERVICE_NAME = "/NDNSF/DistributedRepo/Object/v1";

  /** Target tuple is frozen by selection; user supplies the authenticated owner. */
  /** Query each Repo's stored Data receipt and current location before an
   * original-name Interest. Producer trust is mandatory and caller-owned.
   */

  /** Fetch one exact packet from a current user-storage packet set.
   * The caller must bind the complete set to the Owner listing and verify its
   * aggregate wire digest. Producer trust is mandatory and caller-owned.
   * This uses the Owner's existing Face, blocks outside its I/O thread, and
   * requires the Owner Face event loop to remain active during the fetch.
   */

  /** Retrieve an explicitly indexed immutable set through the existing User Face.
   * Reference routing/byte commitments do not grant storage or execution authority.
   * Checks the complete ordered digest, then verifies standard certificates before
   * ordinary Data with the caller's configured Producer policy. Returned order and
   * wire bytes are unchanged. Call off Face and retain its owner until callbacks drain.
   * The original deadline bounds an in-progress fetch; current-work checks gate
   * admission, packet validation and return. No Owner claim is minted by this API. */

  /** Fetch explicitly named original packets with a byte upper bound, without an Owner listing.
   * The caller pins application commitments separately and supplies packet verification.
   * No exact size/digest or authorization proof is fabricated. Uses the existing Face window,
   * retry and spool owner. Caller keeps its event loop running until cancellation/drain completes;
   * the operation retains the User and its Face lease while callbacks remain pending, and can
   * retain an additional operation owner through that same callback drain.
   */

  /**
   * Page the authenticated Owner listing for one exact claim, fetch its Repo
   * manifest and every original packet from the selected Provider, then
   * revalidate the Owner claim. Producer trust is caller-owned. Packet Interests
   * use the Owner's existing Face; this blocks outside its I/O thread and
   * requires the Owner Face event loop to remain active during the fetch.
   */
  static VerifiedRepoUserStoragePacketSetV1 fetchOriginalDataPacketSetFromUserStorage(
    ndn_service_framework::ServiceUser& owner,
    const RepoArtifactTarget& target,
    const std::string& objectName,
    const std::function<bool(const ndn::Data&)>& verifyProducerSignature,
    int timeoutMs,
    RepoUserStoragePacketSetFetchLimits limits = {},
    const std::function<void()>& requireCurrent = {},
    const std::optional<ndn_service_framework::SegmentManifestScope>& segmentManifest = {});

  /** Re-read one exact Owner claim after caller-side semantic validation.
   * Returns only when the complete committed claim tuple is unchanged. This is
   * a bounded observation, not a durability lease, and must not run on the
   * ServiceUser Face I/O thread.
   */

  /** Commit an exact signed packet set using a current Owner grant.
   * Serves a bounded private spool only through the writer's existing route.
   * Confirms durable INSERT by a current writer-bound manifest after withdrawing
   * the temporary Producer. Retrieval is a separate operation.
   * The caller keeps writer and its running Face alive; never call on that Face.
   * On ambiguous failure the Owner retains cleanup/reconciliation responsibility.
   */

  

  

  

  

  

  

  

  

  

  /** Store one signed Data packet under the exact name encoded in its wire. */
  

  /** Retrieve the original packet wire by its complete NDN Data name. */
  

  /**
   * Retrieve an application-produced packet set by the manifest's ordered exact
   * Data names. The operation validates the complete name encoded in every wire
   * and throws before returning if the packet index is incomplete or invalid.
   */
  

  

  /**
   * Convenience INSERT adapter for callers that have payload bytes instead of
   * pre-published Data. The adapter segments and signs the payload under
   * objectName using ndn-cxx Segmenter, then stores the resulting Data wire
   * packets under their exact encoded Data names through insert().
   */
  

  

  

  

  

  

  

  /**
   * Legacy opaque-byte helper: store a large object as object-level chunks named
   * <objectName>/seg/<index>, then store a manifest-only parent object.
   *
   * This is not the canonical NDN Data packet API. Repo storage is opaque here:
   * the repo does not perform APP trust, signature, or
   * hash verification while storing. Use getSegmented() on the APP side to
   * reassemble and verify size/hash against the returned manifest.
   */
  

  /**
   * Fetch chunks described by manifest, reassemble them, and verify APP-side
   * size/hash metadata. Throws std::runtime_error on mismatch.
   */
  

  /**
   * Fetch one logical repo object described by manifest. This is the preferred
   * object-level API for callers that do not care whether the object was stored
   * as one payload or as object-level chunks.
   */
  

  

  

  

  

  

  

  

  

  

  

  

  

  

  

  

  

  /**
   * Same as putSegmented(), but invokes a repo registered in the same trusted
   * LocalServiceRegistry instead of using NDNSF network messages.
   */
  

  /**
   * Same as getSegmented(), but fetches chunks from the local registry.
   */
  

  /**
   * Local trusted equivalent of getObject().
   */
  

  

  

  

  

  

  

  

  

  /**
   * Run one request through the normal ACK/selection/Response path while
   * restricting both ACK collection and final selection to one application-
   * selected Provider identity.
   * The same timeout covers authorization bootstrap and the invocation.
   * Readiness checks run on the existing Face context without blocking it;
   * deadline/cancellation before readiness abandons bootstrap without emission.
   * The caller keeps User and Face alive and running while queued work drains.
   * The optional emission latch is caller-owned: clear it once before a
   * multi-request target attempt; this helper only sets it.
   */


  /** Check caller-owned cancellation before dispatch and while waiting.
   * A thrown probe abandons the local request; an emitted operation may still
   * have committed remotely and must not be treated as rolled back.
   */


  

  

  

  

  



  

  

  

  

  

  

  


};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_CLIENT_HPP
