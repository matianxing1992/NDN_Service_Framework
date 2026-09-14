#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderArtifactCache.hpp"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ndnsf::di {
namespace {

using Clock = std::chrono::steady_clock;

std::string frame(const std::string& value)
{
  return std::to_string(value.size()) + ":" + value;
}

std::runtime_error cacheError(const char* code, const std::string& message)
{
  return std::runtime_error(std::string(code) + ": " + message);
}

void requireControlActive(const NativeRequestControl& control)
{
  control.requireActive();
  if (control.cancelled && control.cancelled())
    throw cacheError("DI_PROVIDER_ARTIFACT_CANCELLED", "request was cancelled");
}

} // namespace

std::string ProviderArtifactKey::canonicalKey() const
{
  std::ostringstream result;
  for (const auto* value : {&sourceDigest, &canonicalSourceName, &canonicalRootName,
                            &canonicalRootDigest,
                            &initializerDigest,
                            &canonicalGraphDigest,
                            &role, &candidateDigest, &recipeDigest, &backendAbi,
                            &device, &precision, &quantization, &layoutDigest,
                            &artifactProfile, &securityDomain, &protectionEpoch,
                            &protectionIdentity}) {
    result << frame(*value);
  }
  return result.str();
}

struct ProviderArtifactLease::Release
{
  std::function<void()> callback;
  ~Release() noexcept
  {
    if (!callback)
      return;
    try { callback(); }
    catch (...) {}
  }
};

ProviderArtifactLease::ProviderArtifactLease(
  std::shared_ptr<const PreparedProviderArtifact> artifact,
  std::shared_ptr<const NativeModelRunnerSpec> runnerSpec,
  std::shared_ptr<Release> release,
  bool cacheHit) noexcept
  : m_artifact(std::move(artifact))
  , m_runnerSpec(std::move(runnerSpec))
  , m_release(std::move(release))
  , m_cacheHit(cacheHit)
{
}

ProviderArtifactLease::~ProviderArtifactLease() noexcept = default;

ProviderArtifactLease::ProviderArtifactLease(ProviderArtifactLease&& other) noexcept
  : m_artifact(std::move(other.m_artifact))
  , m_runnerSpec(std::move(other.m_runnerSpec))
  , m_release(std::move(other.m_release))
  , m_cacheHit(other.m_cacheHit)
{
}

ProviderArtifactLease& ProviderArtifactLease::operator=(ProviderArtifactLease&& other) noexcept
{
  if (this == &other)
    return *this;
  m_release.reset();
  m_artifact = std::move(other.m_artifact);
  m_runnerSpec = std::move(other.m_runnerSpec);
  m_release = std::move(other.m_release);
  m_cacheHit = other.m_cacheHit;
  return *this;
}

struct ProviderArtifactCache::Shared
{
  struct Entry
  {
    std::string key;
    std::shared_ptr<const PreparedProviderArtifact> artifact;
    std::shared_ptr<const NativeModelRunnerSpec> runnerSpec;
    std::uint64_t chargedBytes = 0;
    std::uint64_t activeLeases = 0;
    std::uint64_t lastUse = 0;
  };

  struct Job
  {
    std::condition_variable condition;
    bool done = false;
    bool cancelled = false;
    bool stopRequested = false;
    bool creatorActive = false;
    std::size_t waiters = 0;
    std::uint64_t reservedBytes = 0;
    std::chrono::steady_clock::time_point deadline;
    std::exception_ptr error;
  };

  explicit Shared(ProviderArtifactCacheConfig value)
    : config(std::move(value))
  {
    if (config.maxArtifactBytes == 0 || config.maxArtifactEntries == 0 ||
        config.assemblyJobTimeout.count() <= 0)
      throw std::invalid_argument("provider artifact cache limits must be positive");
  }

  ProviderArtifactCacheConfig config;
  mutable std::mutex mutex;
  bool stopped = false;
  std::uint64_t chargedBytes = 0;
  std::uint64_t reservedBytes = 0;
  std::uint64_t sequence = 0;
  std::uint64_t coldBuilds = 0;
  std::uint64_t templateHits = 0;
  std::uint64_t activeLeases = 0;
  std::map<std::string, Entry> entries;
  std::map<std::string, std::shared_ptr<Job>> jobs;
};

ProviderArtifactLease ProviderArtifactCache::makeLease(
  const std::shared_ptr<Shared>& shared, const std::string& cacheKey, bool cacheHit)
{
  const auto found = shared->entries.find(cacheKey);
  if (found == shared->entries.end())
    throw cacheError("DI_PROVIDER_ARTIFACT_INVALID", "cache lease entry is missing");
  auto& entry = found->second;
  auto release = std::make_shared<ProviderArtifactLease::Release>();
  const auto entryKey = entry.key;
  release->callback = [shared, entryKey] {
    std::lock_guard<std::mutex> lock(shared->mutex);
    const auto found = shared->entries.find(entryKey);
    if (found == shared->entries.end())
      return;
    if (found->second.activeLeases != 0)
      --found->second.activeLeases;
    if (shared->activeLeases != 0)
      --shared->activeLeases;
  };
  ++entry.activeLeases;
  ++shared->activeLeases;
  entry.lastUse = ++shared->sequence;
  return ProviderArtifactLease(entry.artifact, entry.runnerSpec, std::move(release), cacheHit);
}

namespace {

void validateArtifact(const PreparedProviderArtifact& artifact)
{
  if (artifact.encryptedObjectName.empty() || artifact.ciphertextDigest.empty() ||
      artifact.formatVersion.empty() || artifact.canonicalMetadataJson.empty() ||
      artifact.ciphertextBytes == 0)
    throw cacheError("DI_PROVIDER_ARTIFACT_INVALID", "immutable artifact descriptor is incomplete");
}

std::uint64_t runnerTemplateBytes(const NativeModelRunnerSpec& spec)
{
  std::uint64_t total = 0;
  for (const auto value : {spec.role.size(), spec.kind.size(), spec.backend.size()}) {
    if (total > std::numeric_limits<std::uint64_t>::max() - value)
      throw cacheError("CACHE_BUDGET_EXCEEDED", "runner template size overflows cache budget");
    total += value;
  }
  for (const auto& [key, value] : spec.metadata) {
    if (key.size() > std::numeric_limits<std::uint64_t>::max() - total)
      throw cacheError("CACHE_BUDGET_EXCEEDED", "runner template size overflows cache budget");
    total += static_cast<std::uint64_t>(key.size());
    if (value.size() > std::numeric_limits<std::uint64_t>::max() - total)
      throw cacheError("CACHE_BUDGET_EXCEEDED", "runner template size overflows cache budget");
    total += static_cast<std::uint64_t>(value.size());
  }
  return total;
}

void publish(const std::shared_ptr<ProviderArtifactCache::Shared>& shared,
             const std::string& key,
             const ProviderArtifactCache::BuildResult& result,
             std::uint64_t ownReservation)
{
  if (!result.artifact)
    throw cacheError("DI_PROVIDER_ARTIFACT_INVALID", "builder returned no artifact");
  validateArtifact(*result.artifact);
  if (result.artifact->canonicalMetadataJson.size() >
      std::numeric_limits<std::uint64_t>::max() - result.artifact->ciphertextBytes)
    throw cacheError("CACHE_BUDGET_EXCEEDED", "artifact size overflows cache budget");
  const auto bytes = result.artifact->ciphertextBytes +
    static_cast<std::uint64_t>(result.artifact->canonicalMetadataJson.size());
  const auto templateBytes = result.runnerSpec ? runnerTemplateBytes(*result.runnerSpec) : 0;
  if (templateBytes > std::numeric_limits<std::uint64_t>::max() - bytes)
    throw cacheError("CACHE_BUDGET_EXCEEDED", "artifact/template size overflows cache budget");
  const auto chargedBytes = bytes + templateBytes;
  if (chargedBytes > shared->config.maxArtifactBytes || ownReservation > shared->reservedBytes)
    throw cacheError("CACHE_BUDGET_EXCEEDED", "artifact exceeds maxArtifactBytes");
  const auto otherReservations = shared->reservedBytes - ownReservation;
  const auto availableAfterArtifact = shared->config.maxArtifactBytes - chargedBytes;
  if (otherReservations > availableAfterArtifact ||
      shared->chargedBytes > availableAfterArtifact - otherReservations)
    throw cacheError("CACHE_BUDGET_EXCEEDED", "artifact exceeds maxArtifactBytes");

  // Reserve vector capacity before changing the map.  Node extraction and
  // reinsertion use the same allocator and are noexcept, so the mutation
  // below cannot leave a partially evicted cache if an allocation fails.
  std::vector<decltype(shared->entries)::node_type> evicted;
  evicted.reserve(shared->entries.size());
  std::uint64_t evictedBytes = 0;
  ProviderArtifactCache::Shared::Entry entry;
  entry.key = key;
  entry.artifact = result.artifact;
  if (result.runnerSpec) {
    auto metadataOnly = std::make_shared<NativeModelRunnerSpec>(*result.runnerSpec);
    metadataOnly->path.clear();
    metadataOnly->metadata.erase("encryptedArtifactPath");
    entry.runnerSpec = std::move(metadataOnly);
  }
  entry.chargedBytes = chargedBytes;
  entry.lastUse = shared->sequence + 1;
  const auto [inserted, didInsert] = shared->entries.emplace(key, std::move(entry));
  if (!didInsert)
    throw cacheError("DI_PROVIDER_ARTIFACT_KEY_MISMATCH", "cache key was published concurrently");
  inserted->second.chargedBytes = chargedBytes;
  try {
    while (shared->entries.size() > shared->config.maxArtifactEntries ||
           shared->chargedBytes > availableAfterArtifact - otherReservations) {
    auto victim = shared->entries.end();
    for (auto it = shared->entries.begin(); it != shared->entries.end(); ++it) {
      if (it->second.activeLeases == 0 &&
          (victim == shared->entries.end() ||
           it->second.lastUse < victim->second.lastUse))
        victim = it;
    }
      if (victim == shared->entries.end())
        throw cacheError("CACHE_BUDGET_EXCEEDED", "all artifact entries are pinned");
      if (victim == inserted)
        throw cacheError("CACHE_BUDGET_EXCEEDED", "new artifact cannot be admitted");
      shared->chargedBytes -= victim->second.chargedBytes;
      evictedBytes += victim->second.chargedBytes;
      evicted.emplace_back(shared->entries.extract(victim));
    }
  }
  catch (...) {
    shared->entries.erase(inserted);
    for (auto& old : evicted)
      shared->entries.insert(std::move(old));
    shared->chargedBytes += evictedBytes;
    throw;
  }
  inserted->second.lastUse = ++shared->sequence;
  shared->chargedBytes += chargedBytes;
  ++shared->coldBuilds;
}

} // namespace

ProviderArtifactCache::ProviderArtifactCache(ProviderArtifactCacheConfig config)
  : m_shared(std::make_shared<Shared>(std::move(config)))
{
}

ProviderArtifactCache::~ProviderArtifactCache() noexcept
{
  stop();
}

ProviderArtifactLease ProviderArtifactCache::acquire(
  const ProviderArtifactKey& key,
  const NativeSelectionProjectionV3& projection,
  const NativeRequestControl& control,
  Build build)
{
  return acquireWithRunner(key, projection, control,
    [build = std::move(build)] (const NativeRequestControl& request) {
      return BuildResult{build(request), {}};
    });
}

ProviderArtifactLease ProviderArtifactCache::acquireWithRunner(
  const ProviderArtifactKey& key,
  const NativeSelectionProjectionV3& projection,
  const NativeRequestControl& control,
  BuildWithRunner build)
{
  if (!build || projection.provider.empty() || projection.requestId.empty())
    throw std::invalid_argument("provider artifact cache requires authenticated projection and builder");
  if (key.role != projection.assembly.selectedRole ||
      key.canonicalRootName != projection.canonicalArtifactName ||
      key.canonicalRootDigest != projection.assembly.modelManifestDigest ||
      key.recipeDigest != projection.assembly.recipeDigest ||
      key.backendAbi != projection.assembly.backendAbi ||
      key.precision != projection.assembly.precision ||
      key.quantization != projection.assembly.quantization ||
      key.protectionEpoch != projection.assembly.protectionEpoch)
    throw cacheError("DI_PROVIDER_ARTIFACT_KEY_MISMATCH",
                     "cache identity is not bound to the authenticated Selection projection");
  if (projection.hasGrantBinding) {
    if (projection.grantName.empty() || projection.grantDigest.empty())
      throw cacheError("DI_PROVIDER_ARTIFACT_KEY_MISMATCH",
                       "authenticated projection lacks grant identity");
    const auto suffix = std::string("|") + projection.grantName + "|" + projection.grantDigest;
    const auto expectedPrefix = projection.provider + suffix;
    if (key.protectionIdentity != expectedPrefix ||
        key.protectionIdentity.size() < suffix.size() ||
        key.protectionIdentity.compare(key.protectionIdentity.size() - suffix.size(),
                                       suffix.size(), suffix) != 0)
      throw cacheError("DI_PROVIDER_ARTIFACT_KEY_MISMATCH",
                       "cache identity is not bound to the authenticated grant");
  }
  const auto cacheKey = key.canonicalKey();
  if (cacheKey.empty())
    throw std::invalid_argument("provider artifact cache key is empty");
  requireControlActive(control);
  constexpr std::uint64_t AssemblyMetadataBudget = 65536;
  if (projection.assembly.maxSourceBytes == 0 ||
      projection.assembly.maxAssembledBytes == 0 ||
      projection.assembly.maxAssembledBytes >
        std::numeric_limits<std::uint64_t>::max() - AssemblyMetadataBudget ||
      projection.assembly.maxSourceBytes >
        std::numeric_limits<std::uint64_t>::max() - AssemblyMetadataBudget -
          projection.assembly.maxAssembledBytes)
    throw cacheError("CACHE_BUDGET_EXCEEDED", "assembly budget is empty");
  const auto reservation = projection.assembly.maxSourceBytes +
    projection.assembly.maxAssembledBytes + AssemblyMetadataBudget;
  const auto shared = m_shared;
  std::shared_ptr<Shared::Job> job;
  bool creator = false;
  {
    std::unique_lock<std::mutex> lock(shared->mutex);
    if (shared->stopped)
      throw cacheError("RUNTIME_CLOSED", "provider artifact cache is stopped");
    if (const auto found = shared->entries.find(cacheKey); found != shared->entries.end()) {
      constexpr std::uint64_t CiphertextFramingBudget = 65536;
      const auto overhead = projection.assembly.protectionEpoch == "plaintext-v1"
        ? 0 : CiphertextFramingBudget;
      if (projection.assembly.maxAssembledBytes >
            std::numeric_limits<std::uint64_t>::max() - overhead ||
          found->second.artifact->ciphertextBytes >
            projection.assembly.maxAssembledBytes + overhead)
        throw cacheError("CACHE_BUDGET_EXCEEDED",
                         "cached artifact exceeds this request's assembled limit");
      ++shared->templateHits;
      return makeLease(shared, cacheKey, true);
    }
    auto existing = shared->jobs.find(cacheKey);
    if (existing != shared->jobs.end() && existing->second->cancelled &&
        !existing->second->done) {
      const auto oldJob = existing->second;
      while (!oldJob->done) {
        if (control.cancelled && control.cancelled())
          throw cacheError("DI_PROVIDER_ARTIFACT_CANCELLED",
                           "cache flight generation wait was cancelled");
        const auto deadline = std::min(control.deadline, oldJob->deadline);
        if (oldJob->condition.wait_until(lock, deadline) == std::cv_status::timeout &&
            !oldJob->done)
          throw cacheError("DI_PROVIDER_ARTIFACT_CANCELLED",
                           "cache flight generation wait expired");
      }
      existing = shared->jobs.find(cacheKey);
      if (shared->stopped)
        throw cacheError("RUNTIME_CLOSED", "provider artifact cache is stopped");
      if (existing != shared->jobs.end() && existing->second == oldJob)
        shared->jobs.erase(existing);
      existing = shared->jobs.find(cacheKey);
    }
    if (existing != shared->jobs.end() && !existing->second->cancelled) {
      job = existing->second;
      ++job->waiters;
    }
    else {
      // A completed cancelled flight can be removed.  An in-flight cancelled
      // flight was rejected above, so no same-key generation can overlap.
      if (existing != shared->jobs.end())
        shared->jobs.erase(existing);
      if (reservation > shared->config.maxArtifactBytes ||
          shared->reservedBytes > shared->config.maxArtifactBytes - reservation)
        throw cacheError("CACHE_BUDGET_EXCEEDED",
                         "assembly temporary/template budget is exhausted");
      auto candidate = std::make_shared<Shared::Job>();
      candidate->waiters = 1;
      candidate->creatorActive = true;
      candidate->reservedBytes = reservation;
      candidate->deadline = std::min(
        control.deadline, Clock::now() + shared->config.assemblyJobTimeout);
      const auto inserted = shared->jobs.emplace(cacheKey, candidate);
      if (!inserted.second)
        throw cacheError("DI_PROVIDER_ARTIFACT_KEY_MISMATCH",
                         "cache flight was published concurrently");
      job = std::move(candidate);
      try {
        const auto available = shared->config.maxArtifactBytes -
          shared->reservedBytes - reservation;
        using EntryIterator = decltype(shared->entries.begin());
        std::vector<EntryIterator> victims;
        victims.reserve(shared->entries.size());
        const auto required = shared->chargedBytes > available
          ? shared->chargedBytes - available : 0;
        const auto requiredVictimCount = shared->entries.size() >=
              shared->config.maxArtifactEntries
            ? shared->entries.size() - shared->config.maxArtifactEntries + 1
            : 0;
        std::uint64_t reclaimable = 0;
        while (reclaimable < required || victims.size() < requiredVictimCount) {
          auto victim = shared->entries.end();
          for (auto it = shared->entries.begin(); it != shared->entries.end(); ++it) {
            if (it->second.activeLeases == 0 &&
                std::find(victims.begin(), victims.end(), it) == victims.end() &&
                (victim == shared->entries.end() ||
                 it->second.lastUse < victim->second.lastUse))
              victim = it;
          }
          if (victim == shared->entries.end())
            throw cacheError("CACHE_BUDGET_EXCEEDED",
                             "assembly temporary/template budget is exhausted");
          if (reclaimable > std::numeric_limits<std::uint64_t>::max() -
                              victim->second.chargedBytes)
            throw cacheError("CACHE_BUDGET_EXCEEDED", "cache budget overflows");
          victims.push_back(victim);
          reclaimable += victim->second.chargedBytes;
        }
        if (reclaimable < required) {
          throw cacheError("CACHE_BUDGET_EXCEEDED",
                           "assembly temporary/template budget is exhausted");
        }
        // All potentially throwing work is complete.  Erase only the
        // preselected, unleased nodes; these operations cannot throw.
        for (const auto victim : victims) {
          shared->chargedBytes -= victim->second.chargedBytes;
          shared->entries.erase(victim);
        }
        shared->reservedBytes += reservation;
      }
      catch (...) {
        shared->jobs.erase(inserted.first);
        throw;
      }
      creator = true;
    }
  }

  if (!creator) {
    std::unique_lock<std::mutex> lock(shared->mutex);
    while (!job->done) {
      if (job->cancelled) {
        if (job->waiters != 0) --job->waiters;
        throw cacheError("DI_PROVIDER_ARTIFACT_CANCELLED", "cache job was cancelled");
      }
      if ((control.cancelled && control.cancelled()) || Clock::now() >= job->deadline) {
        if (job->waiters != 0) --job->waiters;
        if (job->waiters == 0) job->cancelled = true;
        throw cacheError("DI_PROVIDER_ARTIFACT_CANCELLED", "cache waiter expired or cancelled");
      }
      const auto deadline = std::min(control.deadline, job->deadline);
      if (job->condition.wait_until(lock, deadline) == std::cv_status::timeout && !job->done) {
        if (job->waiters != 0) --job->waiters;
        if (job->waiters == 0) job->cancelled = true;
        throw cacheError("DI_PROVIDER_ARTIFACT_CANCELLED", "cache waiter deadline expired");
      }
    }
    if (job->waiters != 0) --job->waiters;
    if (job->error)
      std::rethrow_exception(job->error);
    if (shared->stopped)
      throw cacheError("RUNTIME_CLOSED", "provider artifact cache stopped");
    const auto found = shared->entries.find(cacheKey);
    if (found == shared->entries.end())
      throw cacheError("DI_PROVIDER_ARTIFACT_INVALID", "completed cache job published no entry");
    requireControlActive(control);
    return makeLease(shared, cacheKey, true);
  }

  NativeRequestControl jobControl = control;
  jobControl.deadline = job->deadline;
  // The creator's request cancellation is independent from the shared job.
  // Its authorization may leave while another waiter still owns the exact
  // same build; only the shared deadline, stop fence, or last-waiter policy
  // may cancel the job itself.
  const auto creatorCancellation = control.cancelled;
  jobControl.cancelled = [shared, job, creatorCancellation] {
    const bool requested = creatorCancellation && creatorCancellation();
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (shared->stopped || job->cancelled)
      return true;
    // The creator is itself a waiter.  Its cancellation may stop a build only
    // while it is the last waiter; a joined waiter keeps the shared job alive.
    if (requested && job->creatorActive) {
      job->creatorActive = false;
      if (job->waiters != 0)
        --job->waiters;
      if (job->waiters == 0)
        job->cancelled = true;
    }
    return job->cancelled;
  };
  bool published = false;
  try {
    requireControlActive(jobControl);
    const auto result = build(jobControl);
    std::unique_lock<std::mutex> lock(shared->mutex);
    if (shared->stopped || job->cancelled)
      throw cacheError("DI_PROVIDER_ARTIFACT_CANCELLED", "cache job was cancelled");
    publish(shared, cacheKey, result, job->reservedBytes);
    if (shared->reservedBytes >= job->reservedBytes)
      shared->reservedBytes -= job->reservedBytes;
    job->reservedBytes = 0;
    job->done = true;
    const auto currentJob = shared->jobs.find(cacheKey);
    if (currentJob != shared->jobs.end() && currentJob->second == job)
      shared->jobs.erase(currentJob);
    job->condition.notify_all();
    published = true;
    if (job->creatorActive) {
      job->creatorActive = false;
      if (job->waiters != 0)
        --job->waiters;
    }
    auto lease = makeLease(shared, cacheKey, false);
    lock.unlock();
    if (control.cancelled && control.cancelled()) {
      lease = {};
      throw cacheError("DI_PROVIDER_ARTIFACT_CANCELLED", "creator waiter was cancelled");
    }
    return lease;
  }
  catch (...) {
    if (published)
      throw;
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (shared->reservedBytes >= job->reservedBytes)
      shared->reservedBytes -= job->reservedBytes;
    job->reservedBytes = 0;
    if (job->creatorActive) {
      job->creatorActive = false;
      if (job->waiters != 0)
        --job->waiters;
    }
    if (!job->stopRequested && !job->error)
      job->error = std::current_exception();
    job->done = true;
    const auto currentJob = shared->jobs.find(cacheKey);
    if (currentJob != shared->jobs.end() && currentJob->second == job)
      shared->jobs.erase(currentJob);
    job->condition.notify_all();
    throw;
  }
}

void ProviderArtifactCache::stop() noexcept
{
  const auto shared = m_shared;
  if (!shared)
    return;
  std::lock_guard<std::mutex> lock(shared->mutex);
  if (shared->stopped)
    return;
  shared->stopped = true;
  for (const auto& item : shared->jobs) {
    item.second->cancelled = true;
    item.second->stopRequested = true;
    try {
      item.second->error = std::make_exception_ptr(
        cacheError("RUNTIME_CLOSED", "provider artifact cache stopped"));
    }
    catch (...) {
      // The stop fence must remain noexcept even if exception_ptr allocation
      // fails; a done job without an entry is still rejected by its waiter.
      item.second->error = {};
    }
    item.second->done = true;
    item.second->reservedBytes = 0;
    item.second->condition.notify_all();
  }
  shared->reservedBytes = 0;
  shared->jobs.clear();
  for (auto it = shared->entries.begin(); it != shared->entries.end();) {
    if (it->second.activeLeases == 0) {
      shared->chargedBytes -= it->second.chargedBytes;
      it = shared->entries.erase(it);
    }
    else {
      ++it;
    }
  }
}

ProviderArtifactCacheCounters ProviderArtifactCache::counters() const noexcept
{
  ProviderArtifactCacheCounters result;
  const auto shared = m_shared;
  if (!shared)
    return result;
  std::lock_guard<std::mutex> lock(shared->mutex);
  result.coldBuilds = shared->coldBuilds;
  result.templateHits = shared->templateHits;
  result.activeLeases = shared->activeLeases;
  return result;
}

} // namespace ndnsf::di
