#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/shared/uav-tracking-worker-process.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace ndnsf::test {
namespace {

using namespace examples::uav;
using namespace std::chrono_literals;

struct TemporaryDirectory
{
  std::filesystem::path path;

  TemporaryDirectory()
  {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "ndnsf-uav-worker-process-XXXXXX").string();
    const char* created = ::mkdtemp(pattern.data());
    if (!created) {
      throw std::runtime_error("cannot create worker process fixture directory");
    }
    path = created;
  }

  ~TemporaryDirectory()
  {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

bool
readPid(const std::filesystem::path& path, pid_t& pid)
{
  std::ifstream stream(path);
  return static_cast<bool>(stream >> pid) && pid > 0;
}

bool
isRunning(pid_t pid)
{
  std::ifstream stream("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(stream, line)) {
    return false;
  }
  const auto end = line.rfind(')');
  return end == std::string::npos || end + 2 >= line.size() || line[end + 2] != 'Z';
}

bool
waitStopped(pid_t pid)
{
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (isRunning(pid) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  return !isRunning(pid);
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavTrackingWorkerProcess)

BOOST_AUTO_TEST_CASE(LiteralArgvAndSeparateStreamsAndExitStatus)
{
  const std::string literal = "$(echo unexpected) 'quoted' ; & ";
  const auto echo = runUavTrackingWorkerProcess({"/bin/echo", literal}, 2s, 4096,
                                               [] { return true; });
  BOOST_CHECK(echo.outcome == UavTrackingWorkerOutcome::SUCCESS);
  BOOST_CHECK_EQUAL(echo.exitCode, 0);
  BOOST_CHECK_EQUAL(echo.stdoutText, literal + "\n");
  BOOST_CHECK(echo.stderrText.empty());
  const auto separated = runUavTrackingWorkerProcess(
    {"/bin/sh", "-c", "printf stdout; printf stderr >&2; exit 7"}, 2s, 4096,
    [] { return true; });
  BOOST_CHECK(separated.outcome == UavTrackingWorkerOutcome::CHILD_FAILURE);
  BOOST_CHECK_EQUAL(separated.exitCode, 7);
  BOOST_CHECK_EQUAL(separated.stdoutText, "stdout");
  BOOST_CHECK_EQUAL(separated.stderrText, "stderr");
  const auto missing = runUavTrackingWorkerProcess({"/ndnsf-no-such-worker"}, 2s, 4096,
                                                  [] { return true; });
  BOOST_CHECK(missing.outcome == UavTrackingWorkerOutcome::SPAWN_FAILED);
  BOOST_CHECK_NE(missing.systemError, 0);
  BOOST_CHECK_EQUAL(missing.exitCode, -1);
}

BOOST_AUTO_TEST_CASE(CancelBeforeSpawnAndRejectInvalidArguments)
{
  const auto cancelled = runUavTrackingWorkerProcess({"/ndnsf-no-such-worker"}, 2s, 4096,
                                                    [] { return false; });
  BOOST_CHECK(cancelled.outcome == UavTrackingWorkerOutcome::CANCELLED);
  BOOST_CHECK_EQUAL(cancelled.systemError, 0);
  BOOST_CHECK_EQUAL(cancelled.exitCode, -1);
  const auto authorized = [] { return true; };
  BOOST_CHECK_THROW(runUavTrackingWorkerProcess({}, 2s, 4096, authorized), std::invalid_argument);
  BOOST_CHECK_THROW(runUavTrackingWorkerProcess({"/bin/true"}, 0ms, 4096, authorized),
                    std::invalid_argument);
  const auto maximum = runUavTrackingWorkerProcess({"/bin/true"}, 60s, 4096, authorized);
  BOOST_CHECK(maximum.outcome == UavTrackingWorkerOutcome::SUCCESS);
  BOOST_CHECK_THROW(runUavTrackingWorkerProcess({"/bin/true"}, 61s, 4096, authorized),
                    std::invalid_argument);
  BOOST_CHECK_THROW(runUavTrackingWorkerProcess({"/bin/true"}, 2s, 0, authorized),
                    std::invalid_argument);
  BOOST_CHECK_THROW(runUavTrackingWorkerProcess({"/bin/true"}, 2s, 16 * 1024 * 1024 + 1,
                                               authorized), std::invalid_argument);
  BOOST_CHECK_THROW(runUavTrackingWorkerProcess({"/bin/echo", std::string(1, '\0')},
                                               2s, 4096, authorized), std::invalid_argument);
  BOOST_CHECK_THROW(runUavTrackingWorkerProcess(std::vector<std::string>(129, "/bin/true"),
                                               2s, 4096, authorized), std::invalid_argument);
  BOOST_CHECK_THROW(runUavTrackingWorkerProcess({"/bin/echo", std::string(65536, 'x')},
                                               2s, 4096, authorized), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SessionTimeoutLimitMatchesOneShot)
{
  UavTrackingWorkerSession maximum({"/bin/cat"}, 4096);
  const auto result = maximum.exchange("ready\n", "ready\n", 60s, [] { return true; });
  BOOST_CHECK(result.outcome == UavTrackingWorkerOutcome::SUCCESS);

  UavTrackingWorkerSession overLimit({"/bin/cat"}, 4096);
  BOOST_CHECK_THROW(overLimit.exchange("ready\n", "ready\n", 61s, [] { return true; }),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TimeoutEscalatesAndBothOutputBoundsStopTheGroup)
{
  const auto start = std::chrono::steady_clock::now();
  const auto timedOut = runUavTrackingWorkerProcess(
    {"/bin/sh", "-c", "trap '' TERM; while :; do sleep 10; done"}, 150ms, 4096,
    [] { return true; });
  BOOST_CHECK(timedOut.outcome == UavTrackingWorkerOutcome::TIMEOUT);
  BOOST_CHECK_NE(timedOut.exitCode, 0);
  BOOST_CHECK(std::chrono::steady_clock::now() - start < 10s);
  const auto out = runUavTrackingWorkerProcess({"/usr/bin/yes", "x"}, 2s, 128,
                                              [] { return true; });
  BOOST_CHECK(out.outcome == UavTrackingWorkerOutcome::OUTPUT_LIMIT);
  BOOST_CHECK_LE(out.stdoutText.size(), 128);
  const auto err = runUavTrackingWorkerProcess(
    {"/bin/sh", "-c", "exec /usr/bin/yes x >&2"}, 2s, 128, [] { return true; });
  BOOST_CHECK(err.outcome == UavTrackingWorkerOutcome::OUTPUT_LIMIT);
  BOOST_CHECK_LE(err.stderrText.size(), 128);
}

BOOST_AUTO_TEST_CASE(CancelDuringExecutionTerminatesDescendantAndReapsLeader)
{
  TemporaryDirectory temporary;
  const auto marker = temporary.path / "child.pid";
  pid_t descendant = -1;
  const auto start = std::chrono::steady_clock::now();
  const auto result = runUavTrackingWorkerProcess(
    {"/bin/sh", "-c", "sleep 30 & printf '%s\\n' $! > \"$1\"; wait", "fixture",
     marker.string()}, 5s, 4096, [&] { return !readPid(marker, descendant); });
  BOOST_CHECK(result.outcome == UavTrackingWorkerOutcome::CANCELLED);
  BOOST_REQUIRE_GT(descendant, 0);
  BOOST_CHECK(waitStopped(descendant));
  BOOST_CHECK(std::chrono::steady_clock::now() - start < 10s);
}

BOOST_AUTO_TEST_CASE(ThrowingAuthorityPredicateStillReapsTheOwnedChild)
{
  TemporaryDirectory temporary;
  const auto marker = temporary.path / "leader.pid";
  pid_t leader = -1;
  BOOST_CHECK_THROW(runUavTrackingWorkerProcess(
    {"/bin/sh", "-c", "printf '%s\\n' $$ > \"$1\"; sleep 30", "fixture", marker.string()},
    5s, 4096, [&] {
      if (readPid(marker, leader)) {
        throw std::runtime_error("authority owner failed");
      }
      return true;
    }), std::runtime_error);
  BOOST_REQUIRE_GT(leader, 0);
  BOOST_CHECK(!std::filesystem::exists("/proc/" + std::to_string(leader)));
}

BOOST_AUTO_TEST_CASE(LateDescendantOutputCannotPreserveProvisionalSuccess)
{
  TemporaryDirectory temporary;
  const auto marker = temporary.path / "exited.pid";
  const auto ready = temporary.path / "descendant.ready";
  int checks = 0;
  bool observedExit = false;
  const auto result = runUavTrackingWorkerProcess(
    {"/bin/sh", "-c",
     "(trap 'printf 0123456789abcdef; exit 0' TERM; "
     "printf ready > \"$1\"; while :; do sleep 10; done) 2>/dev/null & "
     "while [ ! -s \"$1\" ]; do sleep 0.001; done; "
     "printf '%s\\n' $$ > \"$2\"; exit 0",
     "fixture", ready.string(), marker.string()}, 2s, 8, [&] {
      if (++checks == 1) {
        return true;
      }
      // Observe exit without consuming the owner's wait status. The descendant keeps
      // stdout open and emits bytes only when the owner sends its post-exit TERM.
      const auto deadline = std::chrono::steady_clock::now() + 1s;
      while (std::chrono::steady_clock::now() < deadline) {
        pid_t pid = -1;
        if (readPid(marker, pid)) {
          siginfo_t information{};
          if (::waitid(P_PID, static_cast<id_t>(pid), &information,
                       WEXITED | WNOWAIT | WNOHANG) == 0 && information.si_pid == pid) {
            observedExit = true;
            return true;
          }
        }
        std::this_thread::sleep_for(1ms);
      }
      throw std::runtime_error("fixture child did not exit before output admission");
    });
  BOOST_REQUIRE(observedExit);
  BOOST_CHECK_EQUAL(result.exitCode, 0);
  BOOST_CHECK(result.outcome == UavTrackingWorkerOutcome::OUTPUT_LIMIT);
  BOOST_CHECK_EQUAL(result.stdoutText.size(), 8);
}

BOOST_AUTO_TEST_CASE(SessionReusesLeaderWithSeparateStreamsAndClosesPermanently)
{
  TemporaryDirectory temporary;
  const auto marker = temporary.path / "leader.pid";
  UavTrackingWorkerSession session({"/bin/sh", "-c",
    "printf '%s\\n' $$ > \"$1\"; "
    "while IFS= read -r line; do printf '%s\\n' \"$line\"; printf log >&2; done",
    "fixture", marker.string()}, 4096);
  pid_t leader = -1;
  for (const std::string request : {"one\n", "two\n"}) {
    const auto result = session.exchange(request, request, 2s, [] { return true; });
    BOOST_REQUIRE(result.outcome == UavTrackingWorkerOutcome::SUCCESS);
    BOOST_CHECK_EQUAL(result.exitCode, -1);
    BOOST_CHECK_EQUAL(result.stdoutText, request);
    BOOST_CHECK_EQUAL(result.stderrText, "log");
    pid_t current = -1;
    BOOST_REQUIRE(readPid(marker, current));
    if (leader > 0) BOOST_CHECK_EQUAL(current, leader);
    leader = current;
  }
  BOOST_REQUIRE(readPid(marker, leader));
  BOOST_CHECK(isRunning(leader));
  session.close();
  session.close();
  BOOST_CHECK(!std::filesystem::exists("/proc/" + std::to_string(leader)));
  BOOST_CHECK(session.exchange("three\n", "three\n", 2s, [] { return true; }).outcome ==
              UavTrackingWorkerOutcome::CHILD_FAILURE);
}

BOOST_AUTO_TEST_CASE(SessionRejectsBindingsLimitsAndNeverRespawnsAfterFailure)
{
  UavTrackingWorkerSession cancelled({"/ndnsf-no-such-worker"}, 4096);
  const auto status = cancelled.exchange("one\n", "one\n", 2s, [] { return false; });
  BOOST_CHECK(status.outcome == UavTrackingWorkerOutcome::CANCELLED);
  BOOST_CHECK_EQUAL(status.systemError, 0);
  BOOST_CHECK(cancelled.exchange("one\n", "one\n", 2s, [] { return true; }).outcome ==
              UavTrackingWorkerOutcome::CHILD_FAILURE);
  UavTrackingWorkerSession missing({"/ndnsf-no-such-worker"}, 4096);
  BOOST_CHECK(missing.exchange("one\n", "one\n", 2s, [] { return true; }).outcome ==
              UavTrackingWorkerOutcome::SPAWN_FAILED);
  for (const auto& invalid : std::vector<std::string>{
         "", "no-newline", "two\nlines\n", std::string(4096, 'x') + '\n'}) {
    UavTrackingWorkerSession session({"/bin/cat"}, 4096);
    BOOST_CHECK_THROW(session.exchange(invalid, "ok\n", 2s, [] { return true; }),
                      std::invalid_argument);
  }
  BOOST_CHECK_THROW(UavTrackingWorkerSession({}, 4096), std::invalid_argument);
  BOOST_CHECK_THROW(UavTrackingWorkerSession({"/bin/cat"}, 0), std::invalid_argument);
  UavTrackingWorkerSession timeout({"/bin/cat"}, 4096);
  BOOST_CHECK_THROW(timeout.exchange("one\n", "one\n", 61s, [] { return true; }),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SessionSplitCompletionAndCumulativeOutputBound)
{
  UavTrackingWorkerSession split({"/bin/sh", "-c",
    "while IFS= read -r line; do printf ac; sleep 0.03; printf 'k\\n'; done"}, 4096);
  BOOST_CHECK(split.exchange("job\n", "ack\n", 2s, [] { return true; }).outcome ==
              UavTrackingWorkerOutcome::SUCCESS);
  UavTrackingWorkerSession bounded({"/bin/cat"}, 6);
  BOOST_REQUIRE(bounded.exchange("one\n", "one\n", 2s, [] { return true; }).outcome ==
                UavTrackingWorkerOutcome::SUCCESS);
  const auto overflow = bounded.exchange("two\n", "two\n", 2s, [] { return true; });
  BOOST_CHECK(overflow.outcome == UavTrackingWorkerOutcome::OUTPUT_LIMIT);
  BOOST_CHECK_LE(overflow.stdoutText.size(), 2);
  UavTrackingWorkerSession late({"/bin/sh", "-c",
    "read line; printf 'ack\\n0123456789'; sleep 30"}, 8);
  BOOST_CHECK(late.exchange("job\n", "ack\n", 2s, [] { return true; }).outcome ==
              UavTrackingWorkerOutcome::OUTPUT_LIMIT);
}

BOOST_AUTO_TEST_CASE(SessionWrongCompletionClosedInputAndChildExitAreFailures)
{
  UavTrackingWorkerSession mismatch({"/bin/cat"}, 4096);
  BOOST_CHECK(mismatch.exchange("one\n", "other\n", 100ms, [] { return true; }).outcome ==
              UavTrackingWorkerOutcome::TIMEOUT);
  TemporaryDirectory temporary;
  const auto marker = temporary.path / "closed";
  UavTrackingWorkerSession closedInput({"/bin/sh", "-c",
    "exec 0<&-; printf ready > \"$1\"; exec sleep 30", "fixture", marker.string()}, 4096);
  size_t checks = 0;
  const auto broken = closedInput.exchange("job\n", "ack\n", 2s, [&] {
    if (++checks == 1) return true;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }
    if (!std::filesystem::exists(marker)) throw std::runtime_error("stdin closure not ready");
    return true;
  });
  BOOST_CHECK(broken.outcome == UavTrackingWorkerOutcome::CHILD_FAILURE);
  BOOST_CHECK_EQUAL(broken.systemError, EPIPE);
  const auto exitedPath = temporary.path / "exited.pid";
  UavTrackingWorkerSession exited({"/bin/sh", "-c",
    "read line; printf 'ack\\n'; printf '%s\\n' $$ > \"$1\"; exit 7",
    "fixture", exitedPath.string()}, 4096);
  size_t exitChecks = 0;
  const auto result = exited.exchange("job\n", "ack\n", 2s, [&] {
    if (++exitChecks <= 2) return true; // Admit spawn and the first bounded send.
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    pid_t pid = -1;
    do {
      if (readPid(exitedPath, pid)) {
        siginfo_t info{};
        if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT | WNOHANG) == 0 &&
            info.si_pid == pid) return true;
      }
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("fixture exit status not observable");
  });
  BOOST_CHECK(result.outcome == UavTrackingWorkerOutcome::CHILD_FAILURE);
  BOOST_CHECK_EQUAL(result.exitCode, 7);
  BOOST_CHECK_NE(broken.exitCode, -1);
}

BOOST_AUTO_TEST_CASE(SessionCancellationAndThrowingPredicateReapOwnedGroup)
{
  TemporaryDirectory temporary;
  const auto marker = temporary.path / "descendant.pid";
  pid_t descendant = -1;
  UavTrackingWorkerSession session({"/bin/sh", "-c",
    "sleep 30 & printf '%s\\n' $! > \"$1\"; wait", "fixture", marker.string()}, 4096);
  BOOST_CHECK(session.exchange("job\n", "ack\n", 2s,
    [&] { return !readPid(marker, descendant); }).outcome == UavTrackingWorkerOutcome::CANCELLED);
  BOOST_REQUIRE_GT(descendant, 0);
  BOOST_CHECK(waitStopped(descendant));
  const auto leaderPath = temporary.path / "leader.pid";
  pid_t leader = -1;
  UavTrackingWorkerSession throwing({"/bin/sh", "-c",
    "printf '%s\\n' $$ > \"$1\"; exec sleep 30", "fixture", leaderPath.string()}, 4096);
  BOOST_CHECK_THROW(throwing.exchange("job\n", "ack\n", 2s, [&] {
    if (readPid(leaderPath, leader)) throw std::runtime_error("authority lost");
    return true;
  }), std::runtime_error);
  BOOST_REQUIRE_GT(leader, 0);
  BOOST_CHECK(!std::filesystem::exists("/proc/" + std::to_string(leader)));
}

BOOST_AUTO_TEST_CASE(SessionDestructorReleasesDescriptorsAndTermIgnoringLeader)
{
  const auto countDescriptors = [] {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                          std::filesystem::directory_iterator());
  };
  const auto before = countDescriptors();
  TemporaryDirectory temporary;
  const auto marker = temporary.path / "leader.pid";
  pid_t leader = -1;
  {
    UavTrackingWorkerSession session({"/bin/sh", "-c",
      "trap '' TERM; printf '%s\\n' $$ > \"$1\"; "
      "while IFS= read -r line; do printf '%s\\n' \"$line\"; done; exec sleep 30",
      "fixture", marker.string()}, 4096);
    BOOST_REQUIRE(session.exchange("ready\n", "ready\n", 2s, [] { return true; }).outcome ==
                  UavTrackingWorkerOutcome::SUCCESS);
    BOOST_REQUIRE(readPid(marker, leader));
  }
  BOOST_CHECK(!std::filesystem::exists("/proc/" + std::to_string(leader)));
  BOOST_CHECK_EQUAL(countDescriptors(), before);
}

BOOST_AUTO_TEST_CASE(SessionCrossThreadCancellationJoinsBeforeOwnerDestruction)
{
  TemporaryDirectory temporary;
  const auto marker = temporary.path / "descendant.pid";
  const auto leaderPath = temporary.path / "leader.pid";
  std::atomic<bool> authorized{true};
  UavTrackingWorkerSession session({"/bin/sh", "-c",
    "IFS= read -r line; printf '%s\\n' $$ > \"$2\"; "
    "sleep 30 & printf '%s\\n' $! > \"$1\"; wait",
    "fixture", marker.string(), leaderPath.string()}, 4096);
  const auto start = std::chrono::steady_clock::now();
  // The future joins before the session, atomic and directory can be destroyed, even on exceptions.
  auto cancellation = std::async(std::launch::async, [&authorized, marker] {
    pid_t descendant = -1;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!readPid(marker, descendant) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }
    authorized.store(false, std::memory_order_release);
    return descendant;
  });
  const auto result = session.exchange("job\n", "ack\n", 5s, [&authorized] {
    return authorized.load(std::memory_order_acquire);
  });
  const auto descendant = cancellation.get();
  BOOST_REQUIRE_GT(descendant, 0);
  BOOST_CHECK(result.outcome == UavTrackingWorkerOutcome::CANCELLED);
  BOOST_CHECK(waitStopped(descendant));
  pid_t leader = -1;
  BOOST_REQUIRE(readPid(leaderPath, leader));
  BOOST_CHECK(!std::filesystem::exists("/proc/" + std::to_string(leader)));
  BOOST_CHECK(std::chrono::steady_clock::now() - start < 10s);
  BOOST_CHECK(session.exchange("again\n", "again\n", 2s, [] { return true; }).outcome ==
              UavTrackingWorkerOutcome::CHILD_FAILURE);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
