#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/syscall.h>
#include <unistd.h>

namespace spec202_test {

struct ImportLockPauseState
{
  std::filesystem::path path;
  std::mutex mutex;
  std::condition_variable wake;
  bool entered = false;
  bool released = false;
};

inline std::mutex& importPauseMutex() { static std::mutex value; return value; }
inline std::shared_ptr<ImportLockPauseState>& importPauseState()
{
  static std::shared_ptr<ImportLockPauseState> value;
  return value;
}

class ScopedImportLockPause
{
public:
  explicit ScopedImportLockPause(const std::filesystem::path& path)
    : m_state(std::make_shared<ImportLockPauseState>())
  {
    std::lock_guard<std::mutex> lock(importPauseMutex());
    if (importPauseState()) throw std::logic_error("nested import lock pause");
    m_state->path = std::filesystem::absolute(path);
    importPauseState() = m_state;
  }
  ~ScopedImportLockPause()
  {
    release();
    std::lock_guard<std::mutex> lock(importPauseMutex());
    importPauseState().reset();
  }
  bool waitForEntry()
  {
    std::unique_lock<std::mutex> lock(m_state->mutex);
    return m_state->wake.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return m_state->entered; });
  }
  void release()
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->released = true;
    m_state->wake.notify_all();
  }
  ScopedImportLockPause(const ScopedImportLockPause&) = delete;
  ScopedImportLockPause& operator=(const ScopedImportLockPause&) = delete;
private:
  std::shared_ptr<ImportLockPauseState> m_state;
};

} // namespace spec202_test

#ifdef SPEC202_DEFINE_IMPORT_LOCK_WRAPPER
extern "C" int __wrap_flock(int fd, int operation)
{
  std::shared_ptr<spec202_test::ImportLockPauseState> state;
  {
    std::lock_guard<std::mutex> lock(spec202_test::importPauseMutex());
    state = spec202_test::importPauseState();
  }
  if (state) {
    char path[4096];
    const auto link = "/proc/self/fd/" + std::to_string(fd);
    const auto length = ::readlink(link.c_str(), path, sizeof(path));
    if (length > 0 && std::filesystem::path(std::string(path, length)) == state->path) {
      std::unique_lock<std::mutex> lock(state->mutex);
      if (!state->entered) {
        state->entered = true;
        state->wake.notify_all();
        state->wake.wait_for(lock, std::chrono::seconds(30), [&] { return state->released; });
      }
    }
  }
  return static_cast<int>(::syscall(SYS_flock, fd, operation));
}
#endif
