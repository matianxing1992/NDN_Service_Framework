#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

using namespace ndnsf_distributed_repo;

RepoObjectManifest
makeManifest(const std::string& name,
             const std::vector<uint8_t>& payload,
             const std::string& type = "tiered-cache-test")
{
  RepoObjectManifest manifest;
  manifest.objectName = name;
  manifest.objectType = type;
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.segmentCount = 1;
  return manifest;
}

uint64_t
logicalCharge(const RepoObjectManifest& manifest, const std::vector<uint8_t>& payload)
{
  return manifest.objectName.size() + manifest.toJson().size() + payload.size();
}

void
removeDatabase(const std::filesystem::path& path)
{
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-wal");
  std::filesystem::remove(path.string() + "-shm");
}

void
require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class RejectableStore : public RepoStoreBackend
{
public:
  void put(const RepoObjectManifest& manifest, std::vector<uint8_t> payload) override
  {
    rejectIfRequested();
    m_delegate.put(manifest, std::move(payload));
  }

  void putManifest(const RepoObjectManifest& manifest) override
  {
    rejectIfRequested();
    m_delegate.putManifest(manifest);
  }

  StoredObject get(const std::string& objectName) const override
  {
    return m_delegate.get(objectName);
  }

  bool has(const std::string& objectName) const override
  {
    return m_delegate.has(objectName);
  }

  bool erase(const std::string& objectName) override
  {
    rejectIfRequested();
    return m_delegate.erase(objectName);
  }

  size_t size() const override
  {
    return m_delegate.size();
  }

  std::vector<RepoObjectManifest> listManifests() const override
  {
    return m_delegate.listManifests();
  }

  uint64_t usedBytes() const override
  {
    return m_delegate.usedBytes();
  }

  void setRejectWrites(bool reject)
  {
    m_rejectWrites = reject;
  }

private:
  void rejectIfRequested() const
  {
    if (m_rejectWrites) {
      throw std::runtime_error("injected authoritative write failure");
    }
  }

private:
  InMemoryRepoStore m_delegate;
  bool m_rejectWrites = false;
};

} // namespace

int
main()
{
  using namespace ndnsf_distributed_repo;

  try {
    const auto sqlitePath = std::filesystem::temp_directory_path() /
      ("ndnsf-distributed-repo-tiered-" + std::to_string(getpid()) + ".sqlite3");
    removeDatabase(sqlitePath);

    const std::string persistentName = "/repo/tiered/persistent";
    const std::vector<uint8_t> persistentPayload(256, 0x41);
    {
      RepoCore core({"/repo/tiered", 1024 * 1024, 0, 0.0, 1.0,
                     "test", {"object"}},
                    makeTieredRepoStore(sqlitePath.string(), 4096));
      core.put(persistentName, persistentPayload, "persistent-test");
      const auto status = core.cacheStatus();
      require(status.storageBackend == "tiered" &&
              status.authoritativeBackend == "sqlite" &&
              status.cachePolicy == "lru" &&
              status.admissions == 1 && status.backingWrites == 1 &&
              status.entryCount == 1 && status.usedBytes <= status.budgetBytes,
              "initial tiered write status mismatch");
    }
    {
      RepoCore restarted({"/repo/tiered", 1024 * 1024, 0, 0.0, 1.0,
                          "test", {"object"}},
                         makeTieredRepoStore(sqlitePath.string(), 4096));
      require(restarted.cacheStatus().entryCount == 0,
              "restarted cache must begin empty");
      require(restarted.get(persistentName) == persistentPayload,
              "restart fetch did not read SQLite authority");
      auto status = restarted.cacheStatus();
      require(status.misses == 1 && status.backingReads == 1 &&
              status.admissions == 1 && status.hits == 0,
              "cold restart status mismatch");
      require(restarted.get(persistentName) == persistentPayload,
              "hot restart fetch payload mismatch");
      status = restarted.cacheStatus();
      require(status.hits == 1 && status.backingReads == 1,
              "repeat fetch did not hit memory");
      const auto parsed = parseCacheStatusJson(status.toJson());
      require(parsed.storageBackend == status.storageBackend &&
              parsed.usedBytes == status.usedBytes &&
              parsed.hits == status.hits &&
              parsed.backingReads == status.backingReads,
              "cache status JSON round-trip mismatch");
    }

    auto rejectable = std::make_shared<RejectableStore>();
    auto failureStore = makeTieredRepoStore(rejectable, 4096, "test-store");
    const std::string failureName = "/repo/tiered/write-failure";
    const std::vector<uint8_t> oldPayload(64, 0x11);
    const std::vector<uint8_t> newPayload(64, 0x22);
    failureStore->put(makeManifest(failureName, oldPayload), oldPayload);
    rejectable->setRejectWrites(true);
    bool writeFailed = false;
    try {
      failureStore->put(makeManifest(failureName, newPayload), newPayload);
    }
    catch (const std::runtime_error&) {
      writeFailed = true;
    }
    require(writeFailed, "injected authoritative failure was not returned");
    require(failureStore->get(failureName).payload == oldPayload,
            "failed overwrite changed readable cache content");
    const auto failureStatus = failureStore->cacheStatus();
    require(failureStatus.backingWrites == 1 &&
            failureStatus.admissions == 1 &&
            failureStatus.invalidations == 0,
            "failed overwrite changed cache counters");

    const std::vector<uint8_t> lruPayload(96, 0x33);
    const auto manifestA = makeManifest("/repo/tiered/lru/A", lruPayload);
    const auto manifestB = makeManifest("/repo/tiered/lru/B", lruPayload);
    const auto manifestC = makeManifest("/repo/tiered/lru/C", lruPayload);
    const auto twoEntryBudget = logicalCharge(manifestA, lruPayload) +
                                logicalCharge(manifestB, lruPayload);
    auto lruStore = makeTieredRepoStore(makeMemoryRepoStore(),
                                        twoEntryBudget,
                                        "memory-test-authority");
    lruStore->put(manifestA, lruPayload);
    lruStore->put(manifestB, lruPayload);
    require(lruStore->cacheStatus().entryCount == 2,
            "two exact-budget entries were not admitted");
    lruStore->get(manifestA.objectName);
    lruStore->put(manifestC, lruPayload);
    auto lruStatus = lruStore->cacheStatus();
    require(lruStatus.entryCount == 2 && lruStatus.evictions == 1 &&
            lruStatus.usedBytes <= lruStatus.budgetBytes,
            "LRU admission did not evict one entry");
    const auto missesBeforeB = lruStatus.misses;
    require(lruStore->get(manifestB.objectName).payload == lruPayload,
            "evicted object was not available from authority");
    lruStatus = lruStore->cacheStatus();
    require(lruStatus.misses == missesBeforeB + 1 &&
            lruStatus.backingReads >= 1 &&
            lruStatus.usedBytes <= lruStatus.budgetBytes,
            "evicted object did not follow backing read-through");

    const std::vector<uint8_t> oversizedPayload(512, 0x44);
    auto oversizedStore = makeTieredRepoStore(makeMemoryRepoStore(), 128,
                                              "memory-test-authority");
    const auto oversizedManifest = makeManifest("/repo/tiered/oversized",
                                                oversizedPayload);
    oversizedStore->put(oversizedManifest, oversizedPayload);
    auto oversizedStatus = oversizedStore->cacheStatus();
    require(oversizedStatus.entryCount == 0 &&
            oversizedStatus.oversizedBypasses == 1,
            "oversized write was admitted");
    require(oversizedStore->get(oversizedManifest.objectName).payload == oversizedPayload,
            "oversized object was not available from authority");
    oversizedStatus = oversizedStore->cacheStatus();
    require(oversizedStatus.entryCount == 0 &&
            oversizedStatus.misses == 1 &&
            oversizedStatus.oversizedBypasses == 2,
            "oversized read-through was admitted");

    auto disabledStore = makeTieredRepoStore(makeMemoryRepoStore(), 0,
                                             "memory-test-authority");
    disabledStore->put(manifestA, lruPayload);
    require(disabledStore->get(manifestA.objectName).payload == lruPayload,
            "zero-budget store lost authoritative object");
    const auto disabledStatus = disabledStore->cacheStatus();
    require(disabledStatus.cachePolicy == "disabled" &&
            disabledStatus.entryCount == 0 &&
            disabledStatus.admissions == 0 &&
            disabledStatus.usedBytes == 0,
            "zero-budget cache was not disabled");

    auto disabledSqliteStore = makeTieredRepoStore(sqlitePath.string(), 0);
    const auto disabledSqliteStatus = disabledSqliteStore->cacheStatus();
    require(disabledSqliteStatus.storageBackend == "sqlite" &&
            disabledSqliteStatus.authoritativeBackend == "sqlite" &&
            disabledSqliteStatus.cachePolicy == "disabled",
            "zero-budget SQLite status contract mismatch");

    auto deleteStore = makeTieredRepoStore(makeMemoryRepoStore(), 4096,
                                           "memory-test-authority");
    deleteStore->put(manifestA, lruPayload);
    require(deleteStore->erase(manifestA.objectName), "delete did not reach authority");
    const auto deleteStatus = deleteStore->cacheStatus();
    require(deleteStatus.entryCount == 0 && deleteStatus.invalidations == 1,
            "delete did not invalidate cache");
    bool missingAfterDelete = false;
    try {
      deleteStore->get(manifestA.objectName);
    }
    catch (const std::out_of_range&) {
      missingAfterDelete = true;
    }
    require(missingAfterDelete, "deleted object remained readable");

    auto concurrentStore = makeTieredRepoStore(makeMemoryRepoStore(), 4096,
                                               "memory-test-authority");
    concurrentStore->put(manifestA, lruPayload);
    std::atomic<bool> concurrentOk{true};
    std::vector<std::thread> threads;
    for (size_t threadIndex = 0; threadIndex < 8; ++threadIndex) {
      threads.emplace_back([&] {
        try {
          for (size_t readIndex = 0; readIndex < 100; ++readIndex) {
            if (concurrentStore->get(manifestA.objectName).payload != lruPayload) {
              concurrentOk = false;
            }
          }
        }
        catch (...) {
          concurrentOk = false;
        }
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
    const auto concurrentStatus = concurrentStore->cacheStatus();
    require(concurrentOk && concurrentStatus.hits == 800 &&
            concurrentStatus.usedBytes <= concurrentStatus.budgetBytes,
            "concurrent cache reads were inconsistent");

    removeDatabase(sqlitePath);
    std::cout << "DISTRIBUTED_REPO_TIERED_CACHE_TEST_OK "
              << concurrentStatus.toJson() << std::endl;
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "DistributedRepoTieredCacheTest failed: " << e.what() << std::endl;
    return 1;
  }
}
