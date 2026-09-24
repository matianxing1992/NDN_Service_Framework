#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_CANONICAL_ONNX_ASSEMBLER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_CANONICAL_ONNX_ASSEMBLER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
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
  // Cross-process admission lock for the model-sized cold assembly working
  // set. Empty derives a lock beside the cache root; Provider supplies the
  // shared production path explicitly. Cache hits never acquire this lock.
  std::filesystem::path coldAssemblyLockPath;
  // Explicit diagnostic mode for a verified, system-wide plaintext source
  // cache.  Empty means the normal Repo-backed path.
  std::filesystem::path cacheCompatibilitySourceDir;
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
 * Provider creates request-scoped plaintext staging.
 */
std::optional<NativeModelRunnerSpec>
tryLoadNativeCanonicalOnnxRoleFromCache(
  const NativeSelectionProjectionV3& projection,
  const NativeCanonicalOnnxAssemblerOptions& options,
  const std::string& canonicalSourceName = {},
  const std::string& canonicalSourceDigest = {});

/**
 * Materialize the request-scoped plaintext for a protected assembled-cache
 * hit.  Cache lookup deliberately returns only the authenticated ciphertext
 * descriptor; this helper binds the current ProtectedRuntime, decrypts into
 * private staging, and fills the runner path before runner validation.
 */
void
materializeNativeCanonicalOnnxCacheHit(
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
