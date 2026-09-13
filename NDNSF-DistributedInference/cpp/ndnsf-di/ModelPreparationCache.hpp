#ifndef NDNSF_DI_MODEL_PREPARATION_CACHE_HPP
#define NDNSF_DI_MODEL_PREPARATION_CACHE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModel.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelPackage.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

#include <chrono>
#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ndnsf::di {

/** Input frozen by Runtime::open and consumed by the preparation owner. */
struct PreparationSpec
{
  std::string key;
  std::filesystem::path baseDirectory;
  std::string configurationJson;
  std::string catalogConfigurationJson;
  std::string taskName;
  std::string taskContractDigest;
  std::string inputLayoutDigest;
  std::string configurationDigest;
  std::uint64_t maxSourceBytes = 0;
  std::uint64_t maxAssembledBytes = 0;

  /** Runtime-owned factory used to bind a verified package to its existing
   * request owner. It never stores request, grant, or plan state in cache. */
  using ClientFactory = std::function<std::shared_ptr<NativeInferenceClient>(
    const std::shared_ptr<const PreparedModelPackage>&)>;
  ClientFactory clientFactory;

  /** Opaque identity shared by Runtime-created placement handles and packages. */
  std::shared_ptr<void> runtimeBinding;

  /** Native source owner. The returned bytes are copied into the package. */
  using SourceLoader = std::function<NativeCanonicalSource(
    const PreparationSpec&, std::chrono::steady_clock::time_point)>;
  SourceLoader loadSource;
  /** Owner cancellation fence checked between every native preparation phase. */
  std::function<bool()> cancelled;
  /** Acquire an owner commit guard held through READY publication/return. */
  std::function<std::shared_ptr<void>(std::chrono::steady_clock::time_point)> acquireCommit;
  /** Submit one native preparation job to the owning Core executor. */
  std::function<void(std::function<void()>)> dispatch;
  /** Schedule a native timeout callback; used by resultAsync. */
  std::function<std::function<void()>(std::chrono::steady_clock::time_point,
                                      std::function<void()>)> schedule;
  /** Monotonic job generation assigned by the cache owner. */
  std::uint64_t jobGeneration = 0;
  /** Release the Runtime-owned preparation ticket at job terminal state. */
  std::function<void()> onTerminal;
};

/**
 * Verified preparation owner. T003 intentionally performs one bounded cold
 * build or an exact immutable hit; T004 adds shared jobs, waiter cancellation,
 * refresh generations and leases without changing Package validation.
 */
class ModelPreparationCache
{
public:
  ModelPreparationCache(std::size_t maxBytes, std::size_t maxEntries,
                        std::chrono::milliseconds jobTimeout);
  ~ModelPreparationCache() noexcept;

  PreparedModel prepare(const PreparationSpec& spec,
                        CachePolicy policy = static_cast<CachePolicy>(2),
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

  std::size_t parseCount() const noexcept;
  std::size_t entryCount() const noexcept;
  std::size_t chargedBytes() const noexcept;

private:
  friend class User;
  friend struct Spec185PreparationTestAccess;

  std::shared_ptr<PreparationHandle::State> prepareAsync(
    const PreparationSpec& spec, CachePolicy policy, std::chrono::milliseconds timeout);

  struct LeaseRecord;
  struct PreparationJob;

  std::shared_ptr<const PreparedModelPackage> buildPackage(
    const PreparationSpec& spec, std::chrono::steady_clock::time_point deadline) const;
  PreparedModel prepareSingle(const PreparationSpec& spec, CachePolicy policy,
                              std::chrono::steady_clock::time_point deadline);
  std::shared_ptr<void> acquireLease(const std::shared_ptr<LeaseRecord>& lease);
  void retireLease(const std::shared_ptr<LeaseRecord>& lease);
  std::size_t retiredBytes() const noexcept;
  void runJob(const std::shared_ptr<PreparationJob>& job);
  void finishJob(const std::shared_ptr<PreparationJob>& job,
                std::optional<PreparedModel> result, std::exception_ptr error);
  static std::string makePreparationKey(const PreparationSpec& spec);

  struct Entry
  {
    std::shared_ptr<const PreparedModelPackage> package;
    std::uint64_t generation = 0;
    std::shared_ptr<LeaseRecord> lease;
    std::uint64_t lastUse = 0;
  };

  const std::size_t m_maxBytes;
  const std::size_t m_maxEntries;
  const std::chrono::milliseconds m_jobTimeout;
  mutable std::mutex m_mutex;
  mutable std::mutex m_workerMutex;
  struct WorkerRecord
  {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<WorkerRecord> m_workers;
  std::map<std::string, Entry> m_entries;
  struct JobSlot
  {
    std::shared_ptr<PreparationJob> normal;
    std::shared_ptr<PreparationJob> refresh;
  };
  std::map<std::string, JobSlot> m_jobs;
  struct LeaseBook;
  std::shared_ptr<LeaseBook> m_leaseBook;
  std::uint64_t m_nextJobGeneration = 1;
  std::size_t m_chargedBytes = 0;
  std::size_t m_reservedBytes = 0;
  std::size_t m_parseCount = 0;
  std::uint64_t m_nextGeneration = 1;
  std::uint64_t m_nextUse = 1;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_MODEL_PREPARATION_CACHE_HPP
