#ifndef NDNSF_DI_NATIVE_ARTIFACT_STAGING_HPP
#define NDNSF_DI_NATIVE_ARTIFACT_STAGING_HPP

#include <chrono>
#include <cstddef>
#include <filesystem>

namespace ndnsf::di {

/** Create a best-effort PID lease beside a provider artifact staging tree. */
void markNativeArtifactStagingLease(const std::filesystem::path& directory) noexcept;

/** Remove only orphaned provider staging/protected assembly directories.
 *
 * Content-addressed source and assembled caches are deliberately outside this
 * sweep.  A live PID lease always wins over age; legacy trees without a lease
 * need to be older than staleAfter before they are eligible.
 */
std::size_t cleanupNativeArtifactStaging(
  const std::filesystem::path& cacheDir,
  std::chrono::milliseconds staleAfter) noexcept;

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_ARTIFACT_STAGING_HPP
