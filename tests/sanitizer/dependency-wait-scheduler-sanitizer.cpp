#include "NDNSF-DistributedInference/cpp/ndnsf-di/DependencyWaitScheduler.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace ndnsf::di;

int
main()
{
  DependencyWaitScheduler scheduler(4, 1024);
  std::atomic<std::size_t> completions{0};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (std::size_t i = 0; i < 1000; ++i) {
    const auto accepted = scheduler.submit(
      "sanitizer-" + std::to_string(i), deadline,
      [] (const DependencyWaitControl& control) {
        return control.isCancelled() ? DependencyWaitStatus::Cancelled
                                     : DependencyWaitStatus::Completed;
      },
      [&completions] (const DependencyWaitResult&) { ++completions; });
    if (accepted != DependencyWaitSubmitResult::Accepted) {
      std::cerr << "submission rejected at " << i << std::endl;
      return 2;
    }
  }
  if (!scheduler.waitForIdle(std::chrono::seconds(5))) {
    std::cerr << "scheduler did not become idle" << std::endl;
    return 3;
  }
  const auto snapshot = scheduler.snapshot();
  if (completions != 1000 || snapshot.completed != 1000 ||
      snapshot.queuedCount != 0 || snapshot.activeCount != 0) {
    std::cerr << "terminal accounting mismatch" << std::endl;
    return 4;
  }
  scheduler.shutdown();
  std::cout << "DEPENDENCY_WAIT_SCHEDULER_ASAN_UBSAN_PASS completions="
            << completions << " workers=" << snapshot.workerCount << std::endl;
  return 0;
}
