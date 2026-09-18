#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <atomic>
#include <future>
#include <map>
#include <mutex>

namespace ndnsf::di {

struct NativeCanonicalPublicationOptions
{
  std::string artifactRoot;
  // YOLO preserves its package provenance; Tiny Qwen supplies layer manifests.
  std::string packageManifestDigest;
  std::vector<std::string> layerManifestDigests;
  // All roles produced from one catalog share this immutable profile identity.
  std::string artifactProfileDigest;
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
  std::string canonicalManifestJson;
  std::string manifestDigest;
  std::string artifactProfileDigest;
  std::vector<std::string> layerDataNames;
  std::vector<std::string> layerManifestDigests;
  std::vector<std::string> rollbackDataNames;
  std::vector<NativePublicationKeyReference> rollbackKeyReferences;
  std::string rollbackKeyId;
  std::string rollbackServiceName;
  // Durable repository receipts are reusable by later preparations.  They do
  // not grant the failing caller ownership to remove already-committed
  // objects; transient Core publications keep the default true value.
  bool rollbackOwned = true;

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
      const std::string&)> publish;
    // Best-effort transaction rollback for names published by one prepare.
    // The Core implementation may retain an orphan until its bounded expiry,
    // but it must never expose a committed root after rollback.
    std::function<void(const std::vector<ndn_service_framework::LargeDataPublishResult>&)> abort;
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
    mutable std::mutex mutex;
    std::map<std::string, NativeArtifactBinding> completed;
    std::map<std::string, InFlight> inFlight;
    std::map<std::string, NativePreparedCanonicalPublication> prepared;
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
