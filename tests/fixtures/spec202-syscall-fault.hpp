#pragma once

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace spec202_test {

enum class Fault { SyncError, SyncDelay, RollbackRename, RollbackSync, CleanupUnlink,
                   PreparationCleanup };

struct FaultState
{
  std::filesystem::path destination;
  dev_t parentDevice = 0;
  ino_t parentInode = 0;
  ino_t originalInode = 0;
  Fault fault = Fault::SyncError;
  int delayMs = 0;
  unsigned hits = 0;
  bool publishedDirectory = false;
  bool active = false;
};

inline std::mutex& faultMutex() { static std::mutex value; return value; }
inline FaultState& faultState() { static FaultState value; return value; }

class ScopedFault
{
public:
  ScopedFault(const std::filesystem::path& destination, Fault fault,
              int delayMs = 0, bool publishedDirectory = false)
  {
    std::lock_guard<std::mutex> guard(faultMutex());
    if (faultState().active) throw std::logic_error("nested syscall fault");
    struct stat parent {}, old {};
    if (::stat(destination.parent_path().c_str(), &parent) != 0)
      throw std::runtime_error("fault parent is absent");
    const bool exists = ::lstat(destination.c_str(), &old) == 0;
    faultState() = {destination, parent.st_dev, parent.st_ino,
                    exists ? old.st_ino : 0, fault, delayMs, 0,
                    publishedDirectory, true};
  }

  ~ScopedFault()
  {
    std::lock_guard<std::mutex> guard(faultMutex());
    faultState().active = false;
  }

  unsigned hits() const
  {
    std::lock_guard<std::mutex> guard(faultMutex());
    return faultState().hits;
  }
  ScopedFault(const ScopedFault&) = delete;
  ScopedFault& operator=(const ScopedFault&) = delete;
};

inline bool isBackup(const char* path)
{
  const std::string value(path);
  const std::string suffix = ".previous";
  return value.size() >= suffix.size() &&
    value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0 &&
    std::filesystem::path(value).parent_path() == faultState().destination.parent_path();
}

} // namespace spec202_test

#ifdef SPEC202_DEFINE_SYSCALL_WRAPPERS
extern "C" int __wrap_fsync(int fd)
{
  using namespace spec202_test;
  int delay = 0;
  {
    std::lock_guard<std::mutex> guard(faultMutex());
    auto& state = faultState();
    struct stat directory {}, current {};
    const bool parent = state.active && ::fstat(fd, &directory) == 0 &&
      S_ISDIR(directory.st_mode) && directory.st_dev == state.parentDevice &&
      directory.st_ino == state.parentInode;
    const bool changed = parent && ::lstat(state.destination.c_str(), &current) == 0 &&
      (state.publishedDirectory || current.st_ino != state.originalInode);
    if (parent && (changed || state.fault == Fault::PreparationCleanup ||
                   (state.fault == Fault::RollbackSync && state.hits > 0)) &&
        state.fault != Fault::CleanupUnlink &&
        (state.hits == 0 || (state.fault == Fault::RollbackSync && state.hits == 1))) {
      ++state.hits;
      if (state.fault == Fault::SyncDelay) delay = state.delayMs;
      else { errno = EIO; return -1; }
    }
  }
  if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
  return static_cast<int>(::syscall(SYS_fsync, fd));
}

extern "C" int __wrap_rename(const char* source, const char* destination)
{
  using namespace spec202_test;
  {
    std::lock_guard<std::mutex> guard(faultMutex());
    auto& state = faultState();
    if (state.active && state.fault == Fault::RollbackRename && state.hits == 1 &&
        isBackup(source) && std::filesystem::path(destination) == state.destination) {
      ++state.hits;
      errno = EIO;
      return -1;
    }
  }
  return static_cast<int>(::syscall(SYS_renameat, AT_FDCWD, source, AT_FDCWD, destination));
}

extern "C" int __wrap_unlink(const char* path)
{
  using namespace spec202_test;
  {
    std::lock_guard<std::mutex> guard(faultMutex());
    auto& state = faultState();
    if (state.active && isBackup(path) &&
        ((state.fault == Fault::CleanupUnlink && state.hits == 0) ||
         (state.fault == Fault::PreparationCleanup && state.hits == 1))) {
      ++state.hits;
      errno = EIO;
      return -1;
    }
  }
  return static_cast<int>(::syscall(SYS_unlinkat, AT_FDCWD, path, 0));
}
#endif
