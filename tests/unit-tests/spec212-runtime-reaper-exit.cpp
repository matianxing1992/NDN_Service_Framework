#include "ndn-service-framework/OperationRuntime.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

constexpr char CHILD_MODE[] = "--late-enqueue-child";
constexpr char ENQUEUED_MARKER[] = "LATE_RUNTIME_JOIN_ENQUEUED";
std::shared_ptr<ndn_service_framework::OperationRuntime>* lateRuntime = nullptr;

void
enqueueLateRuntimeAtExit() noexcept
{
  if (lateRuntime == nullptr) {
    std::fputs("LATE_RUNTIME_MISSING\n", stderr);
    std::fflush(stderr);
    return;
  }

  (*lateRuntime)->close();
  if (!(*lateRuntime)->drain(std::chrono::seconds(1))) {
    std::fputs("LATE_RUNTIME_DRAIN_FAILED\n", stderr);
    std::fflush(stderr);
    std::abort();
  }
  (*lateRuntime)->deferJoinOnDestroy();
  delete lateRuntime;
  lateRuntime = nullptr;
  std::fputs("LATE_RUNTIME_JOIN_ENQUEUED\n", stderr);
  std::fflush(stderr);
}

int
runLateEnqueueChild()
{
  // Main can register late runtime cleanup before the first deferred join.
  // The join service must already have registered its own later teardown.
  if (std::atexit(enqueueLateRuntimeAtExit) != 0) {
    std::cerr << "failed to register child exit callback\n";
    return 2;
  }

  auto first = ndn_service_framework::OperationRuntime::create();
  first->close();
  if (!first->drain(std::chrono::seconds(1))) {
    std::cerr << "first runtime did not drain\n";
    return 3;
  }
  first->deferJoinOnDestroy();
  first.reset(); // Starts the lazy join worker after registering late cleanup.

  lateRuntime = new std::shared_ptr<ndn_service_framework::OperationRuntime>(
    ndn_service_framework::OperationRuntime::create());
  return 0;
}

class ScopedFd
{
public:
  explicit ScopedFd(int fd) noexcept
    : m_fd(fd)
  {
  }

  ~ScopedFd()
  {
    if (m_fd >= 0)
      ::close(m_fd);
  }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  int get() const noexcept { return m_fd; }

private:
  int m_fd;
};

std::string
readTail(int fd)
{
  if (::lseek(fd, 0, SEEK_SET) < 0)
    return "<unable to seek child output>";

  constexpr size_t MAX_TAIL = 4096;
  char buffer[1024];
  std::string output;
  for (;;) {
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count == 0)
      break;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      output += "\n<error reading child output>";
      break;
    }
    output.append(buffer, static_cast<size_t>(count));
    if (output.size() > MAX_TAIL)
      output.erase(0, output.size() - MAX_TAIL);
  }
  return output;
}

int
runParent(const char* executable)
{
  std::error_code pathError;
  const auto executablePath = std::filesystem::canonical(executable, pathError);
  if (pathError) {
    std::cerr << "cannot resolve test executable: " << pathError.message() << '\n';
    return 1;
  }

  char outputPath[] = "/tmp/ndnsf-reaper-exit-XXXXXX";
  ScopedFd outputFd(::mkstemp(outputPath));
  if (outputFd.get() < 0) {
    std::cerr << "cannot create child output file: " << std::strerror(errno) << '\n';
    return 1;
  }
  ::unlink(outputPath); // Keep only the open descriptor; leave no scratch file.

  posix_spawn_file_actions_t actions;
  int error = ::posix_spawn_file_actions_init(&actions);
  if (error != 0) {
    std::cerr << "cannot initialize child file actions: " << std::strerror(error) << '\n';
    return 1;
  }
  error = ::posix_spawn_file_actions_adddup2(&actions, outputFd.get(), STDERR_FILENO);
  if (error == 0)
    error = ::posix_spawn_file_actions_addclose(&actions, outputFd.get());
  if (error != 0) {
    ::posix_spawn_file_actions_destroy(&actions);
    std::cerr << "cannot configure child output: " << std::strerror(error) << '\n';
    return 1;
  }

  char* childArguments[] = {
    const_cast<char*>(executablePath.c_str()),
    const_cast<char*>(CHILD_MODE),
    nullptr
  };
  pid_t child = -1;
  error = ::posix_spawn(&child, executablePath.c_str(), &actions, nullptr,
                        childArguments, environ);
  ::posix_spawn_file_actions_destroy(&actions);
  if (error != 0) {
    std::cerr << "cannot start isolated child: " << std::strerror(error) << '\n';
    return 1;
  }

  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  for (;;) {
    const pid_t waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child)
      break;
    if (waited < 0 && errno != EINTR) {
      std::cerr << "waitpid failed: " << std::strerror(errno) << '\n';
      return 1;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      ::kill(child, SIGKILL);
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      std::cerr << "child exceeded 8 second exit bound\n" << readTail(outputFd.get());
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  const auto childOutput = readTail(outputFd.get());
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::cerr << "child failed during static teardown (status=" << status << ")\n"
              << childOutput;
    return 1;
  }
  if (childOutput.find(ENQUEUED_MARKER) == std::string::npos) {
    std::cerr << "child exited without the late-enqueue marker\n" << childOutput;
    return 1;
  }

  std::cout << "PASS: isolated late runtime destructor enqueued its join during static teardown\n";
  return 0;
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc == 2 && std::string(argv[1]) == CHILD_MODE) {
    try {
      return runLateEnqueueChild();
    }
    catch (const std::exception& error) {
      std::cerr << "child setup failed: " << error.what() << '\n';
      return 4;
    }
  }
  if (argc != 1) {
    std::cerr << "unexpected arguments\n";
    return 2;
  }
  return runParent(argv[0]);
}
