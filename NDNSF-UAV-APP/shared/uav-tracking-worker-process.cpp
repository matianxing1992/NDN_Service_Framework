#include "uav-tracking-worker-process.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <initializer_list>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace ndnsf::examples::uav {
namespace {

constexpr size_t MAX_ARGUMENTS = 128;
constexpr size_t MAX_ARGUMENT_BYTES = 65536;
constexpr size_t MAX_OUTPUT_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_CONTROL_BYTES = 4096;
constexpr auto MAX_TIMEOUT = std::chrono::milliseconds(60000);
constexpr auto TERMINATION_GRACE = std::chrono::milliseconds(500);
constexpr int POLL_INTERVAL_MS = 20;

void
checkSystemCall(int error)
{
  if (error != 0) {
    throw std::system_error(error, std::generic_category(), "tracking worker process");
  }
}

void
promoteDescriptor(int& descriptor)
{
  if (descriptor < 3) {
    const int promoted = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
    if (promoted < 0) checkSystemCall(errno);
    ::close(descriptor);
    descriptor = promoted;
  }
}

std::vector<char*>
makeArguments(const std::vector<std::string>& argv)
{
  if (argv.empty() || argv.size() > MAX_ARGUMENTS || argv.front().empty()) {
    throw std::invalid_argument("invalid tracking worker argv");
  }
  size_t bytes = 0;
  std::vector<char*> arguments;
  for (const auto& argument : argv) {
    if (argument.find('\0') != std::string::npos ||
        argument.size() >= MAX_ARGUMENT_BYTES - bytes) {
      throw std::invalid_argument("tracking worker argv exceeds byte limit or contains NUL");
    }
    bytes += argument.size() + 1;
    arguments.push_back(const_cast<char*>(argument.c_str()));
  }
  arguments.push_back(nullptr);
  return arguments;
}

struct OwnedPipe
{
  int readFd = -1;
  int writeFd = -1;

  OwnedPipe()
  {
    int descriptors[2];
    if (::pipe2(descriptors, O_CLOEXEC) != 0) {
      checkSystemCall(errno);
    }
    readFd = descriptors[0];
    writeFd = descriptors[1];
    try {
      // Keep spawn dup/close actions independent of initially closed standard descriptors.
      for (int* descriptor : {&readFd, &writeFd}) {
        promoteDescriptor(*descriptor);
      }
      if (::fcntl(readFd, F_SETFL, O_NONBLOCK) < 0) {
        checkSystemCall(errno);
      }
    }
    catch (...) {
      closeRead();
      closeWrite();
      throw;
    }
  }

  ~OwnedPipe()
  {
    closeRead();
    closeWrite();
  }

  OwnedPipe(const OwnedPipe&) = delete;
  OwnedPipe& operator=(const OwnedPipe&) = delete;

  void
  closeRead() noexcept
  {
    if (readFd >= 0) {
      ::close(readFd);
      readFd = -1;
    }
  }

  void
  closeWrite() noexcept
  {
    if (writeFd >= 0) {
      ::close(writeFd);
      writeFd = -1;
    }
  }
};

struct OwnedInput
{
  int parentFd = -1;
  int childFd = -1;

  OwnedInput()
  {
    int descriptors[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors) < 0) {
      checkSystemCall(errno);
    }
    parentFd = descriptors[0];
    childFd = descriptors[1];
    try {
      promoteDescriptor(parentFd);
      promoteDescriptor(childFd);
      if (::fcntl(parentFd, F_SETFL, O_NONBLOCK) < 0) checkSystemCall(errno);
    }
    catch (...) {
      close();
      throw;
    }
  }

  ~OwnedInput()
  {
    close();
  }
  OwnedInput(const OwnedInput&) = delete;
  OwnedInput& operator=(const OwnedInput&) = delete;

  void
  close() noexcept
  {
    if (parentFd >= 0) ::close(parentFd);
    if (childFd >= 0) ::close(childFd);
    parentFd = childFd = -1;
  }
};

struct SpawnSetup
{
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;

  SpawnSetup()
  {
    checkSystemCall(::posix_spawn_file_actions_init(&actions));
    const int error = ::posix_spawnattr_init(&attributes);
    if (error != 0) {
      ::posix_spawn_file_actions_destroy(&actions);
      checkSystemCall(error);
    }
  }

  ~SpawnSetup()
  {
    ::posix_spawnattr_destroy(&attributes);
    ::posix_spawn_file_actions_destroy(&actions);
  }

  SpawnSetup(const SpawnSetup&) = delete;
  SpawnSetup& operator=(const SpawnSetup&) = delete;
};

class OwnedChild
{
public:
  explicit OwnedChild(pid_t pid)
    : m_pid(pid)
  {
  }

  ~OwnedChild()
  {
    finish();
  }

  int
  finish() noexcept
  {
    if (m_pid <= 0) return exitCode();
    // Also stop descendants that closed their output pipes before their leader exited.
    ::kill(-m_pid, SIGKILL);
    if (!m_reaped) {
      pid_t observed;
      do {
        observed = ::waitpid(m_pid, &m_status, 0);
      } while (observed < 0 && errno == EINTR);
      m_reaped = observed == m_pid;
    }
    m_pid = -1;
    return exitCode();
  }

  OwnedChild(const OwnedChild&) = delete;
  OwnedChild& operator=(const OwnedChild&) = delete;

  void
  signal(int value)
  {
    if (::kill(-m_pid, value) < 0 && errno != ESRCH) {
      checkSystemCall(errno);
    }
  }

  bool
  pollExit()
  {
    if (!m_reaped) {
      pid_t status;
      do {
        status = ::waitpid(m_pid, &m_status, WNOHANG);
      } while (status < 0 && errno == EINTR);
      if (status < 0) {
        checkSystemCall(errno);
      }
      m_reaped = status == m_pid;
    }
    return m_reaped;
  }

  int
  exitCode() const
  {
    if (!m_reaped) {
      return -1;
    }
    return WIFEXITED(m_status) ? WEXITSTATUS(m_status) : 128 + WTERMSIG(m_status);
  }

private:
  pid_t m_pid;
  int m_status = 0;
  bool m_reaped = false;
};

pid_t
spawnChild(const std::vector<std::string>& argv, OwnedPipe& stdoutPipe,
           OwnedPipe& stderrPipe, OwnedInput* input, int& error)
{
  auto arguments = makeArguments(argv);
  SpawnSetup setup;
  if (input) {
    checkSystemCall(::posix_spawn_file_actions_adddup2(&setup.actions, input->childFd, 0));
    checkSystemCall(::posix_spawn_file_actions_addclose(&setup.actions, input->parentFd));
    checkSystemCall(::posix_spawn_file_actions_addclose(&setup.actions, input->childFd));
  }
  checkSystemCall(::posix_spawn_file_actions_adddup2(&setup.actions, stdoutPipe.writeFd, 1));
  checkSystemCall(::posix_spawn_file_actions_adddup2(&setup.actions, stderrPipe.writeFd, 2));
  for (int descriptor : {stdoutPipe.readFd, stdoutPipe.writeFd,
                         stderrPipe.readFd, stderrPipe.writeFd}) {
    checkSystemCall(::posix_spawn_file_actions_addclose(&setup.actions, descriptor));
  }
  sigset_t mask;
  ::sigemptyset(&mask);
  checkSystemCall(::posix_spawnattr_setsigmask(&setup.attributes, &mask));
  checkSystemCall(::posix_spawnattr_setpgroup(&setup.attributes, 0));
  checkSystemCall(::posix_spawnattr_setflags(&setup.attributes,
                                          POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK));
  pid_t pid = -1;
  error = ::posix_spawnp(&pid, arguments.front(), &setup.actions, &setup.attributes,
                        arguments.data(), environ);
  return pid;
}

bool
drain(OwnedPipe& pipe, std::string& output, size_t limit)
{
  std::array<char, 8192> buffer;
  while (pipe.readFd >= 0) {
    const auto count = ::read(pipe.readFd, buffer.data(), buffer.size());
    if (count == 0) {
      pipe.closeRead();
      break;
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      checkSystemCall(errno);
    }
    const auto size = static_cast<size_t>(count);
    if (size > limit - output.size()) {
      output.append(buffer.data(), limit - output.size());
      pipe.closeRead();
      return false;
    }
    output.append(buffer.data(), size);
  }
  return true;
}

} // namespace

UavTrackingWorkerProcessResult
runUavTrackingWorkerProcess(const std::vector<std::string>& argv,
                            std::chrono::milliseconds timeout,
                            size_t maxOutputBytes,
                            const std::function<bool()>& stillAuthorized)
{
  if (argv.empty() || argv.size() > MAX_ARGUMENTS || argv.front().empty() ||
      timeout.count() <= 0 || timeout > MAX_TIMEOUT || maxOutputBytes == 0 ||
      maxOutputBytes > MAX_OUTPUT_BYTES || !stillAuthorized) {
    throw std::invalid_argument("invalid tracking worker process limits/arguments");
  }
  makeArguments(argv);
  UavTrackingWorkerProcessResult result;
  if (!stillAuthorized()) {
    result.outcome = UavTrackingWorkerOutcome::CANCELLED;
    return result;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  OwnedPipe stdoutPipe;
  OwnedPipe stderrPipe;
  const auto pid = spawnChild(argv, stdoutPipe, stderrPipe, nullptr, result.systemError);
  if (result.systemError != 0) {
    return result;
  }
  OwnedChild child(pid);
  stdoutPipe.closeWrite();
  stderrPipe.closeWrite();
  result.outcome = UavTrackingWorkerOutcome::SUCCESS;
  bool stopping = false;
  bool killed = false;
  std::chrono::steady_clock::time_point terminationDeadline;
  const auto stop = [&] (UavTrackingWorkerOutcome outcome) {
    // Successful exit is provisional until all buffered output has been admitted.
    if (result.outcome == UavTrackingWorkerOutcome::SUCCESS) {
      result.outcome = outcome;
    }
    if (!stopping) {
      stopping = true;
      terminationDeadline = std::chrono::steady_clock::now() + TERMINATION_GRACE;
      child.signal(SIGTERM);
    }
  };
  while (true) {
    if (!stopping && !stillAuthorized()) {
      stop(UavTrackingWorkerOutcome::CANCELLED);
    }
    if (!stopping && std::chrono::steady_clock::now() >= deadline) {
      stop(UavTrackingWorkerOutcome::TIMEOUT);
    }
    const bool exited = child.pollExit();
    const bool stdoutOk = drain(stdoutPipe, result.stdoutText, maxOutputBytes);
    const bool stderrOk = drain(stderrPipe, result.stderrText, maxOutputBytes);
    if (!stdoutOk || !stderrOk) {
      stop(UavTrackingWorkerOutcome::OUTPUT_LIMIT);
    }
    if (exited && !stopping) {
      stop(child.exitCode() == 0 ? UavTrackingWorkerOutcome::SUCCESS :
                                  UavTrackingWorkerOutcome::CHILD_FAILURE);
    }
    if (stopping && !killed && std::chrono::steady_clock::now() >= terminationDeadline) {
      child.signal(SIGKILL);
      killed = true;
    }
    if (exited && (killed || (stdoutPipe.readFd < 0 && stderrPipe.readFd < 0))) {
      result.exitCode = child.exitCode();
      return result;
    }
    pollfd descriptors[] = {{stdoutPipe.readFd, POLLIN, 0}, {stderrPipe.readFd, POLLIN, 0}};
    if (::poll(descriptors, 2, POLL_INTERVAL_MS) < 0 && errno != EINTR) {
      checkSystemCall(errno);
    }
  }
}

struct UavTrackingWorkerSession::Impl
{
  std::vector<std::string> argv;
  size_t limit;
  size_t stdoutBytes = 0;
  size_t stderrBytes = 0;
  bool stopped = false;
  int exitCode = -1;
  std::unique_ptr<OwnedPipe> output;
  std::unique_ptr<OwnedPipe> error;
  std::unique_ptr<OwnedInput> input;
  std::optional<OwnedChild> child;

  Impl(std::vector<std::string> arguments, size_t maxOutputBytes)
    : argv(std::move(arguments))
    , limit(maxOutputBytes)
  {
    makeArguments(argv);
    if (limit == 0 || limit > MAX_OUTPUT_BYTES) {
      throw std::invalid_argument("invalid tracking worker session output bound");
    }
  }

  void
  close() noexcept
  {
    stopped = true;
    input.reset();
    if (child) {
      try {
        child->signal(SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + TERMINATION_GRACE;
        while (!child->pollExit() && std::chrono::steady_clock::now() < deadline) {
          ::poll(nullptr, 0, POLL_INTERVAL_MS);
        }
      }
      catch (...) {
        // OwnedChild still kills the entire group and reaps the leader below.
      }
      exitCode = child->finish();
      child.reset();
    }
    output.reset();
    error.reset();
  }
};

UavTrackingWorkerSession::UavTrackingWorkerSession(std::vector<std::string> argv,
                                                 size_t maxOutputBytes)
  : m_impl(std::make_unique<Impl>(std::move(argv), maxOutputBytes))
{
}

UavTrackingWorkerSession::~UavTrackingWorkerSession()
{
  close();
}

void
UavTrackingWorkerSession::close() noexcept
{
  m_impl->close();
}

UavTrackingWorkerProcessResult
UavTrackingWorkerSession::exchange(const std::string& requestLine,
                                  const std::string& completionLine,
                                  std::chrono::milliseconds timeout,
                                  const std::function<bool()>& stillAuthorized)
{
  auto& owner = *m_impl;
  UavTrackingWorkerProcessResult result;
  const auto fail = [&] (UavTrackingWorkerOutcome outcome) {
    result.outcome = outcome;
    owner.close();
    if (result.exitCode < 0) result.exitCode = owner.exitCode;
    return result;
  };
  try {
    const auto validLine = [] (const std::string& line) {
      return !line.empty() && line.size() <= MAX_CONTROL_BYTES && line.back() == '\n' &&
             line.find('\n') == line.size() - 1 && line.find('\0') == std::string::npos;
    };
    if (!validLine(requestLine) || !validLine(completionLine) || timeout.count() <= 0 ||
        timeout > MAX_TIMEOUT || !stillAuthorized) {
      throw std::invalid_argument("invalid tracking worker session exchange");
    }
    if (owner.stopped) return fail(UavTrackingWorkerOutcome::CHILD_FAILURE);
    if (!stillAuthorized()) return fail(UavTrackingWorkerOutcome::CANCELLED);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!owner.child) {
      owner.output = std::make_unique<OwnedPipe>();
      owner.error = std::make_unique<OwnedPipe>();
      owner.input = std::make_unique<OwnedInput>();
      const auto pid = spawnChild(owner.argv, *owner.output, *owner.error,
                                  owner.input.get(), result.systemError);
      if (result.systemError != 0) return fail(UavTrackingWorkerOutcome::SPAWN_FAILED);
      owner.child.emplace(pid); // No allocation between successful spawn and ownership.
      owner.output->closeWrite();
      owner.error->closeWrite();
      ::close(owner.input->childFd);
      owner.input->childFd = -1;
    }
    const auto stdoutLimit = owner.limit - owner.stdoutBytes;
    const auto stderrLimit = owner.limit - owner.stderrBytes;
    size_t sent = 0;
    bool completionSeen = false;
    while (true) {
      if (!stillAuthorized()) return fail(UavTrackingWorkerOutcome::CANCELLED);
      if (std::chrono::steady_clock::now() >= deadline) {
        return fail(UavTrackingWorkerOutcome::TIMEOUT);
      }
      const bool exited = owner.child->pollExit();
      const bool stdoutOk = drain(*owner.output, result.stdoutText, stdoutLimit);
      const bool stderrOk = drain(*owner.error, result.stderrText, stderrLimit);
      if (!stdoutOk || !stderrOk) return fail(UavTrackingWorkerOutcome::OUTPUT_LIMIT);
      if (exited) {
        result.exitCode = owner.child->exitCode();
        return fail(UavTrackingWorkerOutcome::CHILD_FAILURE);
      }
      if (owner.output->readFd < 0) return fail(UavTrackingWorkerOutcome::CHILD_FAILURE);
      if (sent < requestLine.size()) {
        const auto count = ::send(owner.input->parentFd, requestLine.data() + sent,
                                  requestLine.size() - sent, MSG_NOSIGNAL);
        if (count > 0) sent += static_cast<size_t>(count);
        else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
          result.systemError = count == 0 ? EPIPE : errno;
          return fail(UavTrackingWorkerOutcome::CHILD_FAILURE);
        }
      }
      bool complete = false;
      if (sent == requestLine.size() && result.stdoutText.size() >= completionLine.size()) {
        const auto offset = result.stdoutText.size() - completionLine.size();
        complete = (offset == 0 || result.stdoutText[offset - 1] == '\n') &&
                   result.stdoutText.compare(offset, completionLine.size(), completionLine) == 0;
      }
      // Drain again after a poll before accepting a completion: buffered overflow/exit wins.
      if (complete && completionSeen) {
        if (!stillAuthorized()) return fail(UavTrackingWorkerOutcome::CANCELLED);
        owner.stdoutBytes += result.stdoutText.size();
        owner.stderrBytes += result.stderrText.size();
        result.outcome = UavTrackingWorkerOutcome::SUCCESS;
        return result;
      }
      completionSeen = complete;
      pollfd descriptors[] = {{owner.output->readFd, POLLIN, 0},
                              {owner.error->readFd, POLLIN, 0},
                              {sent < requestLine.size() ? owner.input->parentFd : -1, POLLOUT, 0}};
      if (::poll(descriptors, 3, POLL_INTERVAL_MS) < 0 && errno != EINTR) checkSystemCall(errno);
    }
  }
  catch (...) {
    owner.close();
    throw;
  }
}

} // namespace ndnsf::examples::uav
