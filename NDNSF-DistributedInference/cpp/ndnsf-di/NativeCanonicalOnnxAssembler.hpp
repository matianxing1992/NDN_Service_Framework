#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_CANONICAL_ONNX_ASSEMBLER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_CANONICAL_ONNX_ASSEMBLER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <atomic>
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
