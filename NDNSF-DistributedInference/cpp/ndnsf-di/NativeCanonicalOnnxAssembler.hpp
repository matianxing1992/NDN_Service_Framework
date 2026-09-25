#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_CANONICAL_ONNX_ASSEMBLER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_CANONICAL_ONNX_ASSEMBLER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ndnsf::di {

/**
 * Options for the production post-Selection ONNX assembly bridge.
 *
 * The C++ Provider remains the owner of the authenticated assignment and
 * cache.  Graph assembly is performed by the pinned native ONNX worker
 * (OA02 subprocess transport, per-location preflight on every spawn); this
 * options object carries only Provider-owned policy plus the worker
 * location it trusts.  The worker's own PASS claim never bypasses the
 * parent digest/identity revalidation in prepareNativeCanonicalOnnxRole.
 */
struct NativeCanonicalOnnxAssemblerOptions
{
  std::string cacheDir = "/tmp/ndnsf-di-native-artifacts";
  // Explicit diagnostic mode for a verified, system-wide plaintext source
  // cache.  Empty means the normal Repo-backed path.
  std::filesystem::path cacheCompatibilitySourceDir;
  // The local assembled cache is system-owned and content-addressed by the
  // authenticated recipe directory plus manifest contract. Trust that
  // immutable local file by default; callers that cannot trust the cache
  // root can opt back into the full assembled-file digest check.
  bool verifyCachedArtifactDigest = false;
  std::string providerIdentity;
  std::uint64_t assemblyTimeoutMs = 30000;
  std::function<bool()> shouldCancel;
  // Called only after an authenticated assembly milestone has completed.  The
  // callback is request-owned; it must not outlive the synchronous assembly
  // call and must preserve the Provider operation sequence it reports.
  std::function<void(const std::string& phase, double progress)> reportProgress;
  std::function<std::string(const std::string& manifestBytes)> signManifest;
  std::shared_ptr<ProtectedRuntime> protectedRuntime;
  std::string roleAssemblySpecDigest;
  // OA02 worker to run every assembly through.  Empty path fails the request
  // up front (DI_PROVIDER_ASSEMBLY_WORKER_LOCATION_MISSING) before any fetch;
  // a non-empty sha256 is re-probed against the pinned binary on every spawn.
  NativeOnnxWorkerLocation workerLocation;
  // Process-local, lease-owned plaintext cache.  Protected grant verification
  // still happens for every request; this cache only avoids repeating the
  // already-authenticated decrypt/copy after an exact descriptor hit.
  std::shared_ptr<class NativeProtectedPlaintextCache> protectedPlaintextCache;
};

/** Exact identity for one decrypted protected assembled model. */
struct NativeProtectedPlaintextCacheKey
{
  std::string encryptedArtifactDigest;
  std::string modelManifestDigest;
  std::string graphDigest;
  std::string initializerDigest;
  std::string role;
  std::string recipeDigest;
  std::string backendAbi;
  std::string roleAssemblySpecDigest;
  std::string keyReferenceDigest;
  std::string entryKind = "MODEL_PROTO";

  std::string canonicalKey() const;
};

/**
 * Process-local cache for authenticated protected plaintext files.
 *
 * The cache stores no model bytes in the C++ heap.  A lease pins one private
 * staging directory while a runner uses it; idle entries remain available for
 * the next turn and are securely erased on eviction/stop.  The encrypted
 * descriptor and current ProtectedRuntime remain the authorization boundary.
 */
class NativeProtectedPlaintextCache
{
public:
  struct Release;

  class Lease
  {
  public:
    Lease() noexcept = default;
    ~Lease() noexcept;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;

    bool valid() const noexcept { return static_cast<bool>(m_release); }
    bool cacheHit() const noexcept { return m_cacheHit; }
    explicit operator bool() const noexcept { return valid(); }
    const std::filesystem::path& path() const noexcept { return m_path; }

  private:
    Lease(std::filesystem::path path, bool cacheHit,
          std::shared_ptr<Release> release) noexcept
      : m_path(std::move(path))
      , m_release(std::move(release))
      , m_cacheHit(cacheHit)
    {}

    std::filesystem::path m_path;
    std::shared_ptr<Release> m_release;
    bool m_cacheHit = false;
    friend class NativeProtectedPlaintextCache;
  };

  using Build = std::function<void(const std::filesystem::path& plaintextPath)>;

  explicit NativeProtectedPlaintextCache(std::filesystem::path cacheDir,
                                         std::size_t maxEntries = 8);
  ~NativeProtectedPlaintextCache() noexcept;
  NativeProtectedPlaintextCache(const NativeProtectedPlaintextCache&) = delete;
  NativeProtectedPlaintextCache& operator=(const NativeProtectedPlaintextCache&) = delete;

  Lease acquire(const NativeProtectedPlaintextCacheKey& key,
                const NativeRequestControl& control,
                std::uint64_t maxPlaintextBytes,
                Build build);
  void invalidate(const NativeProtectedPlaintextCacheKey& key) noexcept;
  void stop() noexcept;

private:
  struct Shared;
  static void releaseLease(const std::shared_ptr<Shared>& shared,
                           const std::string& key) noexcept;
  std::shared_ptr<Shared> m_shared;
};

/**
 * Build the Provider-signed progress reporter used by production assembly
 * callers.  The reporter uses the existing per-operation epoch/sequence
 * fields in SelectionExecutionStatus; it is not a wall-clock heartbeat.
 */
std::function<void(const std::string& phase, double progress)>
makeNativeAssemblyProgressReporter(
  ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
  const NativeSelectionProjectionV3& projection,
  const std::string& adapterIdentity = "native",
  std::uint64_t epoch = 1,
  std::uint64_t initialSequence = 0,
  std::shared_ptr<std::atomic<std::uint64_t>> sequenceState = {});

/**
 * Provider-owned read ports used by the post-Selection assembler.  The
 * production overload below binds these ports to CollaborationContext.  The
 * explicit port type also lets a process-level fixture exercise the exact
 * root/source/digest/cache path without replacing the assembler with a fake
 * runner factory.
 */
struct NativeCanonicalOnnxFetchers
{
  std::function<std::optional<ndn::Buffer>(const ndn::Name&)> getArtifact;
  std::function<std::optional<ndn::Buffer>(const ndn::Name&, const ndn::Name&)>
    fetchEncryptedLargeData;
};

/**
 * Serialize final artifact-directory cleanup with assembler finalization.
 * The guard is process-local and covers content-addressed directories shared
 * by multiple Provider cache keys.
 */
void
withNativeArtifactDirectoryFinalization(const std::string& directory,
                                        const std::function<void()>& action);

/**
 * Reopen a finalized plaintext assembled artifact from the stable cache root.
 * The caller must already have an authenticated post-Selection projection.
 * Admission is recipe-addressed: the directory name is the pre-assembly
 * recipeDigest (the authenticated model/layer/node/backend contract), while
 * manifest.json records the post-assembly model SHA-256. Both identities are
 * checked before a hit is returned. Missing, stale, or corrupt entries return
 * nullopt and leave the normal fetch/assembly path available. Protected
 * entries retain only authenticated ciphertext under a key-reference-bound
 * stable directory; the current grant/Selection still authorizes each hit and
 * Provider binds the current grant and obtains a lease from the process-local
 * plaintext cache; the lease, rather than the immutable template, owns the
 * plaintext lifetime.
 */
std::optional<NativeModelRunnerSpec>
tryLoadNativeCanonicalOnnxRoleFromCache(
  const NativeSelectionProjectionV3& projection,
  const NativeCanonicalOnnxAssemblerOptions& options,
  const std::string& canonicalSourceName = {},
  const std::string& canonicalSourceDigest = {});

/**
 * Legacy request-scoped materialization for a protected assembled-cache hit.
 * Production callers use the overload below so the decrypted path can be
 * reused by an exact process-local plaintext-cache lease.
 */
void
materializeNativeCanonicalOnnxCacheHit(
  NativeModelRunnerSpec& spec,
  const NativeSelectionProjectionV3& projection,
  const NativeCanonicalOnnxAssemblerOptions& options);

/** Materialize through the process-local protected plaintext cache. */
NativeProtectedPlaintextCache::Lease
materializeNativeCanonicalOnnxCacheHit(
  NativeModelRunnerSpec& spec,
  const NativeSelectionProjectionV3& projection,
  const NativeCanonicalOnnxAssemblerOptions& options,
  const NativeRequestControl& control);

/** Reuse a cached protected ciphertext descriptor without rehashing it. */
bool
reuseNativeProtectedCanonicalOnnxCacheDescriptor(
  NativeModelRunnerSpec& spec,
  const NativeSelectionProjectionV3& projection,
  const NativeCanonicalOnnxAssemblerOptions& options);

NativeModelRunnerSpec
prepareNativeCanonicalOnnxRole(
  const NativeCanonicalOnnxFetchers& fetchers,
  const NativeSelectionProjectionV3& projection,
  const NativeCanonicalOnnxAssemblerOptions& options);

/**
 * Fetch and assemble one exact role after Selection.
 *
 * The assignment-bound root is obtained from CollaborationContext, while the
 * canonical ONNX source name is read from the signed root metadata and fetched
 * through the Provider's encrypted large-Data path.  The returned runner spec
 * points only to the newly activated content-addressed local model file.
 */
NativeModelRunnerSpec
prepareNativeCanonicalOnnxRole(
  ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
  const NativeSelectionProjectionV3& projection,
  const NativeCanonicalOnnxAssemblerOptions& options);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_CANONICAL_ONNX_ASSEMBLER_HPP
