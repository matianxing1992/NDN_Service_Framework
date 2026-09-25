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
 * sufficient to address an entry.  For protected projections,
 * protectionIdentity includes the authenticated provider/grant identity;
 * independently issued grants therefore address separate entries even when
 * all immutable model and runner fields match.
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
  // Optional compatibility/test payload.  The production Provider keeps
  // protected ciphertext on disk and stores only its immutable path in the
  // runner metadata, so idle cache entries do not retain model-sized buffers.
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
  // carries a live lease or plaintext path field.  The Provider revalidates
  // the immutable assembledCachePath under its configured cache root before
  // opening the local model.
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
    // Optional immutable runner metadata.  The plaintext runner path is
    // cleared before publication; protected ciphertext paths remain as
    // content-addressed descriptors for lazy request-scoped reads.
    std::shared_ptr<const NativeModelRunnerSpec> runnerSpec;
    // Optional cleanup for immutable on-disk material owned by this entry.
    // It runs only after the entry has no active leases and never handles
    // request-scoped plaintext.
    std::function<void()> cleanup;
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

  // Mark an immutable entry unusable after a failed path/digest/authentication
  // check.  Active leases defer removal until their final release.
  void invalidate(const ProviderArtifactKey& key) noexcept;

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
