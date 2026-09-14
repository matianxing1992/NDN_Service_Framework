#ifndef NDNSF_DI_PROVIDER_ARTIFACT_CACHE_HPP
#define NDNSF_DI_PROVIDER_ARTIFACT_CACHE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Exact immutable identity used by the Provider artifact cache.
 *
 * Empty optional identities are encoded as empty fields, while every field is
 * length framed by canonicalKey().  A model name or cache directory is never
 * sufficient to address an entry.
 */
struct ProviderArtifactKey
{
  std::string sourceDigest;
  std::string canonicalSourceName;
  std::string canonicalRootName;
  std::string canonicalRootDigest;
  std::string initializerDigest;
  std::string canonicalGraphDigest;
  std::string role;
  std::string candidateDigest;
  std::string recipeDigest;
  std::string backendAbi;
  std::string device;
  std::string precision;
  std::string quantization;
  std::string layoutDigest;
  std::string artifactProfile;
  std::string securityDomain;
  std::string protectionEpoch;
  std::string protectionIdentity;

  std::string canonicalKey() const;
};

/** A cacheable ciphertext or immutable assembled artifact descriptor.
 * Request id, grant, receipt, plaintext path and mutable runner state are
 * deliberately absent; those values remain request scoped.
 */
struct PreparedProviderArtifact
{
  std::string encryptedObjectName;
  std::string ciphertextDigest;
  std::string formatVersion;
  std::string canonicalMetadataJson;
  std::uint64_t ciphertextBytes = 0;
  // Protected artifacts may be retained as ciphertext in memory.  The cache
  // never stores a plaintext path or mutable runner state.
  std::shared_ptr<const std::vector<std::uint8_t>> ciphertext;
};

struct ProviderArtifactCacheConfig
{
  std::uint64_t maxArtifactBytes = 1ULL << 30;
  std::size_t maxArtifactEntries = 8;
  std::chrono::milliseconds assemblyJobTimeout{300000};
};

struct ProviderArtifactCacheCounters
{
  std::uint64_t coldBuilds = 0;
  std::uint64_t templateHits = 0;
  std::uint64_t activeLeases = 0;
};

/** Move-only lease for one authenticated request's immutable artifact.
 * The destructor returns the entry's pin.  It does not own plaintext or a
 * mutable runner and remains safe if the cache owner is stopping.
 */
class ProviderArtifactLease
{
public:
  struct Release;
  ProviderArtifactLease() noexcept = default;
  ~ProviderArtifactLease() noexcept;
  ProviderArtifactLease(const ProviderArtifactLease&) = delete;
  ProviderArtifactLease& operator=(const ProviderArtifactLease&) = delete;
  ProviderArtifactLease(ProviderArtifactLease&& other) noexcept;
  ProviderArtifactLease& operator=(ProviderArtifactLease&& other) noexcept;

  bool valid() const noexcept { return static_cast<bool>(m_artifact); }
  bool cacheHit() const noexcept { return m_cacheHit; }
  explicit operator bool() const noexcept { return valid(); }
  const PreparedProviderArtifact* operator->() const noexcept { return m_artifact.get(); }
  const PreparedProviderArtifact& operator*() const { return *m_artifact; }
  std::shared_ptr<const PreparedProviderArtifact> get() const noexcept { return m_artifact; }

  // Internal Provider factory path: metadata only; the cached copy never
  // carries a plaintext path.  The Provider derives a per-request path from
  // the content-addressed artifact digest and verifies it before use.
  std::shared_ptr<const NativeModelRunnerSpec> runnerSpec() const noexcept
  {
    return m_runnerSpec;
  }

private:
  ProviderArtifactLease(std::shared_ptr<const PreparedProviderArtifact> artifact,
                        std::shared_ptr<const NativeModelRunnerSpec> runnerSpec,
                        std::shared_ptr<Release> release,
                        bool cacheHit) noexcept;
  std::shared_ptr<const PreparedProviderArtifact> m_artifact;
  std::shared_ptr<const NativeModelRunnerSpec> m_runnerSpec;
  std::shared_ptr<Release> m_release;
  bool m_cacheHit = false;
  friend class ProviderArtifactCache;
};

class ProviderArtifactCache
{
public:
  struct Shared;
  using Build = std::function<std::shared_ptr<const PreparedProviderArtifact>(
    const NativeRequestControl&)>;

  struct BuildResult
  {
    std::shared_ptr<const PreparedProviderArtifact> artifact;
    // Optional immutable runner metadata.  ProviderArtifactCache clears its
    // path before publication; each request reconstructs a fresh path/context.
    std::shared_ptr<const NativeModelRunnerSpec> runnerSpec;
  };
  using BuildWithRunner = std::function<BuildResult(const NativeRequestControl&)>;

  explicit ProviderArtifactCache(ProviderArtifactCacheConfig config = {});
  ~ProviderArtifactCache() noexcept;
  ProviderArtifactCache(const ProviderArtifactCache&) = delete;
  ProviderArtifactCache& operator=(const ProviderArtifactCache&) = delete;

  ProviderArtifactLease acquire(
    const ProviderArtifactKey& key,
    const NativeSelectionProjectionV3& projection,
    const NativeRequestControl& control,
    Build build);

  // Internal overload used by the Provider facade to recreate a mutable
  // runner from an immutable artifact hit without caching a live runner.
  ProviderArtifactLease acquireWithRunner(
    const ProviderArtifactKey& key,
    const NativeSelectionProjectionV3& projection,
    const NativeRequestControl& control,
    BuildWithRunner build);

  void stop() noexcept;
  ProviderArtifactCacheCounters counters() const noexcept;

private:
  static ProviderArtifactLease makeLease(const std::shared_ptr<Shared>& shared,
                                         const std::string& key,
                                         bool cacheHit);
  std::shared_ptr<Shared> m_shared;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_PROVIDER_ARTIFACT_CACHE_HPP
