#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <optional>

namespace ndnsf::di {

struct NativeCanonicalPublicationOptions
{
  std::string artifactRoot;
  // YOLO preserves its package provenance; Tiny Qwen supplies layer manifests.
  std::string packageManifestDigest;
  std::vector<std::string> layerManifestDigests;
  // All roles produced from one catalog share this immutable profile identity.
  std::string artifactProfileDigest;
  // Bounded serialized publication budget for the complete prepare receipt.
  // This is independent of a selected role's maxAssembledBytes: a
  // topology-independent material set can be larger than any one role.
  // Zero lets the catalog derive a bounded default.
  std::uint64_t maxPublicationBytes = 0;
  // Stable prepare identity supplied by the operator-pinned catalog. Empty
  // preserves the legacy source-digest root for old direct publisher users.
  std::string publicationIdentityDigest;
};

struct NativePublicationKeyReference
{
  std::string keyId;
  std::string serviceName;
};

/** Immutable receipt for canonical publication completed by prepare. */
struct NativePreparedCanonicalPublication
{
  std::string sourceDataName;
  std::string initializerDataName;
  std::string rootDataName;
  std::string materialManifestDataName;
  std::string materialManifestDigest;
  std::uint64_t materialManifestBytes = 0;
  std::string materialReceiptDataName;
  std::string materialReceiptDigest;
  std::uint64_t materialReceiptBytes = 0;
  /** Sum of plaintext bytes handed to the protected publication transport. */
  std::uint64_t publishedBytes = 0;
  std::string canonicalManifestJson;
  std::string manifestDigest;
  std::string artifactProfileDigest;
  std::vector<std::string> layerDataNames;
  std::vector<std::string> layerManifestDigests;
  // Protected, topology-independent graph/node/tensor objects committed by
  // B189-1b.  Names and digests are kept separate from legacy layer fields so
  // old v1 receipts remain readable while new consumers require the material
  // manifest before fetching any role bytes.
  std::vector<std::string> materialPayloadIds;
  std::vector<std::string> materialDataNames;
  std::vector<std::string> materialDigests;
  // A prepared receipt may be returned by Repo lookup before all referenced
  // child objects are present.  This is an in-memory repair hint only; it is
  // never serialized into the authenticated root manifest or retained after
  // the publication owner returns a complete receipt.
  std::vector<std::string> missingDataNames;
  std::vector<std::string> rollbackDataNames;
  std::vector<NativePublicationKeyReference> rollbackKeyReferences;
  std::string rollbackKeyId;
  std::string rollbackServiceName;
  // Compatibility receipts are metadata-only: the authenticated assignment
  // still carries rootDataName as its artifact identity, but Core must not
  // prefetch that name because the Provider validates the shared local cache.
  bool artifactPrefetchRequired = true;
  // Durable repository receipts are reusable by later preparations.  They do
  // not grant the failing caller ownership to remove already-committed
  // objects; transient Core publications keep the default true value.
  bool rollbackOwned = true;
  // Optional reference-only material index restored by a Repo lookup. Selected
  // payloads are fetched after ACK/Selection, not during prepare lookup.
  std::shared_ptr<const NativeCanonicalSource::MaterialManifest> materialManifest;
  // Package/request copies retain Core's serving pins after prepare returns.
  std::vector<std::shared_ptr<void>> servingLeases;
  // Root-last, reference-only catalog metadata used to rebuild a fresh native
  // package without rereading canonical source bytes on a complete hit.
  // Kept at the end to preserve existing aggregate initialization order.
  std::string preparedMetadataJson;
  // Durable Repo publications keep potentially larger reference-only metadata
  // outside the 4 KiB business root; the root authenticates this object by
  // name, digest, and size.
  std::string preparedMetadataDataName;
  std::string preparedMetadataDigest;
  std::uint64_t preparedMetadataBytes = 0;

  void validate() const;
};

// Internal result used by the legacy request-time publication path so a
// cancelled shared job can roll back every Core object it published.
struct NativeUncachedPublication
{
  NativeArtifactBinding binding;
  std::vector<ndn_service_framework::LargeDataPublishResult> publications;
};

/** Requester-side canonical publication through the existing Core crypto owner.
 * Source resolution is native and returns immutable owned bytes. It must honor
 * the supplied control and authenticate remote sources before returning them.
 * This object is copyable; artifactPort() retains its Core owner and source port.
 */
class NativeCanonicalArtifactPublisher
{
public:
  using SourcePort = std::function<std::shared_ptr<const NativeCanonicalSource>(
    const NativeInspectedModel&, const NativeRequestControl&)>;

  NativeCanonicalArtifactPublisher(std::shared_ptr<ndn_service_framework::ServiceUser> user,
    std::string serviceName, NativeCanonicalPublicationOptions options, SourcePort source);

  NativeArtifactBinding operator()(const NativeInspectedModel& model,
    const NativeSplitCandidate& candidate, const std::vector<NativeSelectionRoleV3>& roles,
    const NativeRequestControl& control) const;

  /** Publish canonical source/initializer/root once during Runtime::prepare. */
  NativePreparedCanonicalPublication prepare(const NativeInspectedModel& model,
                                             const NativeRequestControl& control) const;

  /** Bind a selected request to a prepare-time receipt without Core I/O. */
  NativeArtifactBinding bindPrepared(const NativeInspectedModel& model,
    const NativeSplitCandidate& candidate,
    const std::vector<NativeSelectionRoleV3>& roles,
    const NativePreparedCanonicalPublication& publication,
    const NativeRequestControl& control) const;

  NativeRequestPreparation::ArtifactPort artifactPort() const;

  /** Counters for the immutable publication cache.  They describe native
   * source verification and Core publication calls; they are deliberately
   * separate from request/Selection counters. */
  struct Stats
  {
    std::size_t sourceVerifications = 0;
    std::size_t publicationCalls = 0;
    std::size_t cacheHits = 0;
    std::size_t sharedWaiters = 0;
  };

  Stats stats() const noexcept;

private:
  // Test-only scheduling/transport seam. Production construction always binds
  // ServiceUser::postToIo/prepareServiceRequest/publishEncryptedLargeData.
  friend class NativeCanonicalPublisherTestAccess;
  struct Transport
  {
    std::function<void(std::function<void()>)> post;
    std::function<bool()> isOnIoThread;
    std::function<ndn_service_framework::PreparedServiceRequest()> begin;
    std::function<ndn_service_framework::LargeDataPublishResult(
      const ndn_service_framework::PreparedServiceRequest&, const std::vector<std::uint8_t>&,
      const std::string&, const NativeRequestControl&)> publish;
    // Best-effort transaction rollback for names published by one prepare.
    // The Core implementation may retain an orphan until its bounded expiry,
    // but it must never expose a committed root after rollback.
    std::function<void(const std::vector<ndn_service_framework::LargeDataPublishResult>&)> abort;
    bool prepareOnWorker = false;
    // Core's protected publication owner supplies a bounded URI budget.  Keep
    // this after the historical aggregate fields so existing test transports
    // that end with prepareOnWorker remain source-compatible.
    std::uint64_t maxPublishedDataNameBytes =
      NativeCanonicalMaterialReceiptDataNameMaxBytes;
    // Optional durable Core path.  Legacy test transports and transient
    // publishers leave this empty and retain the historical publish call.
    std::function<ndn_service_framework::LargeDataPublishResult(
      const ndn_service_framework::PreparedServiceRequest&, const std::vector<std::uint8_t>&,
      const std::string&, const std::string&, const NativeRequestControl&)> durablePublish;
  };
  NativeCanonicalArtifactPublisher(Transport transport, std::string serviceName,
    NativeCanonicalPublicationOptions options, SourcePort source);

  struct CacheState
  {
    struct SharedControl
    {
      std::atomic<bool> cancelled{false};
      // Set while the cache entry and shared promise are handed off under the
      // cache mutex. A late owner cancellation then observes a committed,
      // reusable publication instead of orphaning a valid receipt.
      std::atomic<bool> committed{false};
      std::atomic<std::size_t> participants{1};
    };
    struct InFlight
    {
      std::shared_future<NativeArtifactBinding> future;
      std::shared_ptr<SharedControl> control;
    };
    struct PreparedInFlight
    {
      std::shared_future<NativePreparedCanonicalPublication> future;
      std::shared_ptr<SharedControl> control;
    };
    struct PreparedEntry
    {
      NativePreparedCanonicalPublication receipt;
      std::vector<std::weak_ptr<void>> leases;

      explicit PreparedEntry(const NativePreparedCanonicalPublication& value)
        : receipt(value)
      {
        for (const auto& lease : receipt.servingLeases)
          leases.emplace_back(lease);
        receipt.servingLeases.clear();
      }

      // A lookup acquires all pins before exposing the receipt. The publisher
      // is an index, not another cache budget or publication lifetime owner.
      std::optional<NativePreparedCanonicalPublication> acquire() const
      {
        auto result = receipt;
        for (const auto& weak : leases) {
          auto lease = weak.lock();
          if (!lease)
            return std::nullopt;
          result.servingLeases.push_back(std::move(lease));
        }
        return result;
      }
      bool expired() const
      {
        for (const auto& lease : leases)
          if (lease.expired()) return true;
        return false;
      }
    };
    mutable std::mutex mutex;
    std::map<std::string, NativeArtifactBinding> completed;
    std::map<std::string, InFlight> inFlight;
    std::map<std::string, PreparedEntry> prepared;
    std::map<std::string, PreparedInFlight> preparedInFlight;
    std::atomic<std::size_t> sourceVerifications{0};
    std::atomic<std::size_t> publicationCalls{0};
    std::atomic<std::size_t> cacheHits{0};
    std::atomic<std::size_t> sharedWaiters{0};
  };

  std::string cacheKey(const NativeInspectedModel& model,
                       const NativeSplitCandidate& candidate,
                       const std::vector<NativeSelectionRoleV3>& roles) const;
  NativeUncachedPublication publishUncached(const NativeInspectedModel& model,
                                        const NativeSplitCandidate& candidate,
                                        const std::vector<NativeSelectionRoleV3>& roles,
                                        const NativeRequestControl& control) const;
  NativePreparedCanonicalPublication prepareUncached(
    const NativeInspectedModel& model, const NativeRequestControl& control) const;
  void abortPreparedPublication(
    const NativePreparedCanonicalPublication& publication) const noexcept;
  void abortPublishedResults(
    const std::vector<ndn_service_framework::LargeDataPublishResult>& publications) const noexcept;
  std::string prepareKey(const NativeInspectedModel& model) const;

  Transport m_transport;
  std::string m_serviceName;
  NativeCanonicalPublicationOptions m_options;
  SourcePort m_source;
  std::shared_ptr<CacheState> m_cache;
};
}
