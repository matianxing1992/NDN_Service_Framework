#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <filesystem>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace ndnsf_distributed_repo;

namespace {

void
require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

RepoObjectManifest
manifestFor(const std::string& name, const std::vector<uint8_t>& payload,
            uint64_t generation = 1)
{
  RepoObjectManifest manifest;
  manifest.objectName = name;
  manifest.objectType = "spec188-memory-budget";
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.segmentCount = 1;
  manifest.generation = generation;
  manifest.policyEpoch = "spec188";
  return manifest;
}

void
removeTree(const std::filesystem::path& path)
{
  std::error_code error;
  std::filesystem::remove_all(path, error);
  if (error) {
    throw std::runtime_error("failed to remove test tree: " + error.message());
  }
}

} // namespace

int
main()
{
  try {
    const auto root = std::filesystem::temp_directory_path() /
      ("ndnsf-spec188-memory-budget-" + std::to_string(::getpid()));
    removeTree(root);
    auto authoritative = std::make_shared<FilesystemRepoStoreBackend>(
      (root / "authority").string(), 256 * 1024, 128, "spec188-memory-budget");
    auto tiered = makeTieredRepoStore(authoritative, 1400, "filesystem", 256);

    const std::vector<uint8_t> payloadA(80, 0x11);
    const std::vector<uint8_t> payloadB(80, 0x22);
    const std::vector<uint8_t> payloadC(80, 0x33);
    const auto manifestA = manifestFor("/spec188/cache/A", payloadA);
    const auto manifestB = manifestFor("/spec188/cache/B", payloadB);
    const auto manifestC = manifestFor("/spec188/cache/C", payloadC);
    tiered->put(manifestA, payloadA);
    tiered->put(manifestB, payloadB);
    auto status = tiered->cacheStatus();
    require(status.entryCount >= 1 && status.usedBytes <= status.budgetBytes,
            "small objects exceeded hot-cache budget");

    tiered->pin(manifestA.objectName);
    tiered->put(manifestC, payloadC);
    status = tiered->cacheStatus();
    require(status.usedBytes <= status.budgetBytes,
            "pinned cache admission exceeded budget");
    require(tiered->get(manifestA.objectName).payload == payloadA,
            "active pin did not preserve readable object");
    tiered->unpin(manifestA.objectName);
    tiered->put(manifestC, payloadC);
    status = tiered->cacheStatus();
    require(status.evictions >= 1 && status.usedBytes <= status.budgetBytes,
            "unpinned object was not evicted under pressure");

    std::vector<uint8_t> largePayload(512 * 1024);
    for (size_t index = 0; index < largePayload.size(); ++index) {
      largePayload[index] = static_cast<uint8_t>((index * 13U + 7U) & 0xffU);
    }
    const auto largeManifest = manifestFor("/spec188/cache/large", largePayload, 3);
    const size_t half = largePayload.size() / 2;
    tiered->putRange(largeManifest, {0, half},
                     std::vector<uint8_t>(largePayload.begin(),
                                          largePayload.begin() + half));
    tiered->putRange(largeManifest, {half, largePayload.size() - half},
                     std::vector<uint8_t>(largePayload.begin() + half,
                                          largePayload.end()));
    tiered->commitRanges(largeManifest);
    status = tiered->cacheStatus();
    require(status.entryCount <= 2 && status.usedBytes <= status.budgetBytes,
            "large range object polluted payload cache");
    require(tiered->getRange(largeManifest.objectName, {half, 64}) ==
              std::vector<uint8_t>(largePayload.begin() + half,
                                   largePayload.begin() + half + 64),
            "tiered range read did not reach durable authority");
    require(authoritative->fullCopyFallbackCount() == 0,
            "range-only path used a full-vector fallback");

    // Exercise simultaneous pins on objects that are already resident in the
    // hot cache.  A pressure write while all workers hold their pins must not
    // evict those entries.  The barrier makes the overlap deterministic
    // without introducing a fixture-owned callback or detached worker.
    constexpr std::size_t workerCount = 8;
    auto pinnedAuthoritative = std::make_shared<FilesystemRepoStoreBackend>(
      (root / "pinned-authority").string(), 256 * 1024, 8192,
      "spec188-pinned-stress");
    auto pinnedTiered = makeTieredRepoStore(
      pinnedAuthoritative, 8192, "filesystem", 8192);
    std::vector<std::string> pinnedNames;
    std::vector<RepoObjectManifest> pinnedManifests;
    std::vector<std::vector<uint8_t>> pinnedPayloads;
    pinnedNames.reserve(workerCount);
    pinnedManifests.reserve(workerCount);
    pinnedPayloads.reserve(workerCount);
    for (std::size_t index = 0; index < workerCount; ++index) {
      pinnedPayloads.emplace_back(96, static_cast<uint8_t>(0x40 + index));
      pinnedNames.emplace_back("/spec188/cache/pinned/" + std::to_string(index));
      pinnedManifests.push_back(manifestFor(
        pinnedNames.back(), pinnedPayloads.back(), 20 + index));
      pinnedTiered->put(pinnedManifests.back(), pinnedPayloads.back());
    }
    auto pinnedStatus = pinnedTiered->cacheStatus();
    require(pinnedStatus.entryCount == workerCount,
            "pinned stress did not establish resident cache entries");
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> release{false};
    std::atomic<std::size_t> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    struct WorkerGuard
    {
      std::atomic<bool>& release;
      std::vector<std::thread>& workers;

      void join() noexcept
      {
        release.store(true, std::memory_order_release);
        for (auto& worker : workers) {
          if (worker.joinable())
            worker.join();
        }
      }

      ~WorkerGuard() { join(); }
    } workerGuard{release, workers};
    for (std::size_t index = 0; index < workerCount; ++index) {
      workers.emplace_back([&, index] {
        try {
          pinnedTiered->pin(pinnedNames[index]);
          ++ready;
          while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
          const auto object = pinnedTiered->get(pinnedNames[index]);
          if (object.payload != pinnedPayloads[index])
            ++failures;
          pinnedTiered->unpin(pinnedNames[index]);
        }
        catch (...) {
          ++failures;
          pinnedTiered->unpin(pinnedNames[index]);
        }
      });
    }
    const auto readyDeadline = std::chrono::steady_clock::now() +
      std::chrono::seconds(5);
    while (ready.load(std::memory_order_acquire) != workerCount &&
           std::chrono::steady_clock::now() < readyDeadline)
      std::this_thread::yield();
    if (ready.load(std::memory_order_acquire) != workerCount)
      throw std::runtime_error("simultaneous pinned workers did not reach the barrier");
    pinnedTiered->put(manifestFor("/spec188/cache/pinned/pressure",
                                  std::vector<uint8_t>(5000, 0x7f), 99),
                      std::vector<uint8_t>(5000, 0x7f));
    pinnedStatus = pinnedTiered->cacheStatus();
    require(pinnedStatus.entryCount == workerCount &&
              pinnedStatus.backingReads == 0,
            "pinned entries were evicted by a cacheable pressure write");
    workerGuard.join();
    require(failures.load(std::memory_order_acquire) == 0,
            "simultaneous pinned cache operation failed");
    pinnedStatus = pinnedTiered->cacheStatus();
    require(pinnedStatus.usedBytes <= pinnedStatus.budgetBytes &&
              pinnedStatus.entryCount == workerCount &&
              pinnedStatus.backingReads == 0 &&
              pinnedStatus.hits >= workerCount,
            "simultaneous pinned workers exceeded cache budget");
    for (std::size_t index = 0; index < pinnedNames.size(); ++index)
      require(pinnedTiered->get(pinnedNames[index]).payload == pinnedPayloads[index],
              "simultaneous pinned entry was evicted while pinned");

    require(tiered->erase(manifestB.objectName), "tiered erase failed");
    require(!authoritative->has(manifestB.objectName),
            "tiered erase left durable object");
    removeTree(root);
    std::cout << "SPEC188_REPO_MEMORY_BUDGET_OK " << pinnedStatus.toJson() << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "Spec188RepoMemoryBudget failed: " << error.what() << std::endl;
    return 1;
  }
}
