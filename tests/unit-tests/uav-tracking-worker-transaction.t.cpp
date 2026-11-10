#include "../../NDNSF-UAV-APP/shared/UavTrackingWorkerTransaction.hpp"

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

ndn::Buffer
bufferOf(const std::string& value)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

std::string
textOf(const ndn::Buffer& value)
{
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

struct TemporaryDirectory
{
  std::filesystem::path path = std::filesystem::temp_directory_path() /
    ("ndnsf-uav-worker-txn-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory()
  {
    std::filesystem::create_directories(path);
  }

  ~TemporaryDirectory()
  {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

constexpr const char* INPUT_DIGEST =
  "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

constexpr const char* NEXT_INPUT_DIGEST =
  "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

std::string
readFile(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read transaction fixture file: " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(input), {});
}

class CommitWriter
{
public:
  explicit
  CommitWriter(const ndnsf::examples::uav::UavTrackingWorkerTransaction& transaction)
  {
    int descriptors[2];
    if (::pipe(descriptors) != 0) {
      throw std::runtime_error("cannot create commit writer pipe");
    }
    const auto parent = ::getpid();
    m_pid = ::fork();
    if (m_pid < 0) {
      ::close(descriptors[0]);
      ::close(descriptors[1]);
      throw std::runtime_error("cannot fork commit writer");
    }
    if (m_pid == 0) {
      ::close(descriptors[0]);
      if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent) {
        ::_exit(93);
      }
      try {
        transaction.commit("window-1", NEXT_INPUT_DIGEST,
          bufferOf("candidate result bytes"), bufferOf("candidate state bytes"), [&] () -> bool {
            const char ready = 'R';
            ssize_t written;
            do {
              written = ::write(descriptors[1], &ready, 1);
            } while (written < 0 && errno == EINTR);
            if (written != 1) {
              ::_exit(90);
            }
            ::close(descriptors[1]);
            // Remain inside the real authorization callback, before the commit rename.
            for (;;) {
              ::pause();
            }
          });
      }
      catch (...) {
        ::_exit(91);
      }
      ::_exit(92);
    }
    ::close(descriptors[1]);
    m_readFd = descriptors[0];
  }

  CommitWriter(const CommitWriter&) = delete;
  CommitWriter& operator=(const CommitWriter&) = delete;

  ~CommitWriter()
  {
    if (m_readFd >= 0) {
      ::close(m_readFd);
    }
    stop();
  }

  void
  waitUntilReady() const
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
      pollfd descriptor{m_readFd, POLLIN, 0};
      const auto polled = ::poll(&descriptor, 1, static_cast<int>(remaining > 0 ? remaining : 1));
      if (polled < 0 && errno == EINTR) {
        continue;
      }
      if (polled <= 0 || (descriptor.revents & POLLIN) == 0) {
        break;
      }
      char ready = 0;
      const auto count = ::read(m_readFd, &ready, 1);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count == 1 && ready == 'R') {
        return;
      }
      break;
    }
    throw std::runtime_error("writer did not reach the native pre-commit boundary");
  }

  int
  killAndWait()
  {
    if (!stop()) {
      throw std::runtime_error("commit writer was not reaped within ten seconds");
    }
    return m_status;
  }

private:
  bool
  stop() noexcept
  {
    if (m_pid < 0) {
      return m_reaped;
    }
    if (!m_stopping) {
      m_stopping = true;
      m_stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      ::kill(m_pid, SIGKILL);
    }
    do {
      const auto reaped = ::waitpid(m_pid, &m_status, WNOHANG);
      if (reaped == m_pid) {
        m_pid = -1;
        m_reaped = true;
        return true;
      }
      if (reaped < 0 && errno != EINTR) {
        if (errno == ECHILD) {
          m_pid = -1;
        }
        return false;
      }
      // Polling never blocks reaping past the shared absolute deadline, including teardown.
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        m_stopDeadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0) {
        return false;
      }
      ::poll(nullptr, 0, static_cast<int>(remaining < 10 ? remaining : 10));
    } while (std::chrono::steady_clock::now() < m_stopDeadline);
    return false;
  }

private:
  pid_t m_pid = -1;
  int m_readFd = -1;
  int m_status = 0;
  bool m_stopping = false;
  bool m_reaped = false;
  std::chrono::steady_clock::time_point m_stopDeadline;
};

} // namespace

BOOST_AUTO_TEST_SUITE(UavTrackingWorkerTransactionTests)

BOOST_AUTO_TEST_CASE(CommitBindsImmutableResultAndStateWithManifest)
{
  TemporaryDirectory temporary;
  ndnsf::examples::uav::UavTrackingWorkerTransaction transaction(temporary.path);
  const auto result = bufferOf("verified result bytes");
  const auto state = bufferOf("candidate state bytes");

  BOOST_REQUIRE(transaction.commit("window-0", INPUT_DIGEST, result, state, [] { return true; }));
  const auto restored = transaction.loadCommittedResult("window-0", INPUT_DIGEST);
  BOOST_REQUIRE(restored);
  BOOST_CHECK_EQUAL(textOf(*restored), textOf(result));

  const auto statePath = transaction.committedStatePath("window-0");
  BOOST_REQUIRE(statePath);
  std::ifstream stateInput(*statePath, std::ios::binary);
  const std::string restoredState((std::istreambuf_iterator<char>(stateInput)), {});
  BOOST_CHECK_EQUAL(restoredState, textOf(state));
  BOOST_CHECK_THROW(transaction.loadCommittedResult(
    "window-0", "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(RevokedWorkDoesNotCommitCandidatePair)
{
  TemporaryDirectory temporary;
  ndnsf::examples::uav::UavTrackingWorkerTransaction transaction(temporary.path);
  const bool committed = transaction.commit("window-0", INPUT_DIGEST,
    bufferOf("result"), bufferOf("state"), [] { return false; });

  BOOST_CHECK(!committed);
  BOOST_CHECK(!transaction.loadCommittedResult("window-0", INPUT_DIGEST));
  BOOST_CHECK(!transaction.committedStatePath("window-0"));
}

BOOST_AUTO_TEST_CASE(InterruptedCandidateCanBeReplacedAndTamperingIsDetected)
{
  TemporaryDirectory temporary;
  ndnsf::examples::uav::UavTrackingWorkerTransaction transaction(temporary.path);
  const auto partial = temporary.path / "window-0";
  std::filesystem::create_directories(partial);
  std::ofstream(partial / "result.json") << "uncommitted";

  BOOST_REQUIRE(transaction.commit("window-0", INPUT_DIGEST,
    bufferOf("verified result"), bufferOf("verified state"), [] { return true; }));
  const auto state = transaction.committedStatePath("window-0");
  BOOST_REQUIRE(state);
  std::ofstream(*state, std::ios::binary | std::ios::trunc) << "tampered";
  BOOST_CHECK_THROW(transaction.loadCommittedResult("window-0", INPUT_DIGEST),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(KilledPreCommitWriterPreservesPreviousPairAndRecoversExactlyOnce)
{
  TemporaryDirectory temporary;
  const auto root = temporary.path / "run" / "mission";
  ndnsf::examples::uav::UavTrackingWorkerTransaction transaction(root, 2);
  BOOST_REQUIRE(transaction.commit("window-0", INPUT_DIGEST,
    bufferOf("previous result"), bufferOf("previous state"), [] { return true; }));
  std::vector<std::pair<std::filesystem::path, std::string>> previous;
  for (const auto* name : {"manifest.json", "result.json", "state.json"}) {
    const auto path = root / "window-0" / name;
    previous.emplace_back(path, readFile(path));
  }
  const auto candidate = root / "window-1";
  {
    CommitWriter writer(transaction);
    writer.waitUntilReady();
    BOOST_CHECK_EQUAL(readFile(candidate / "result.json"), "candidate result bytes");
    BOOST_CHECK_EQUAL(readFile(candidate / "state.json"), "candidate state bytes");
    BOOST_CHECK_NE(readFile(candidate / "manifest.json.tmp").find(
      "\"windowId\":\"window-1\""), std::string::npos);
    BOOST_CHECK(!std::filesystem::exists(candidate / "manifest.json"));
    const auto began = std::chrono::steady_clock::now();
    const auto status = writer.killAndWait();
    BOOST_REQUIRE(WIFSIGNALED(status));
    BOOST_CHECK_EQUAL(WTERMSIG(status), SIGKILL);
    BOOST_CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(10));
  }
  // A new owner observes the actual killed writer's files, not a fabricated partial fixture.
  ndnsf::examples::uav::UavTrackingWorkerTransaction restarted(root, 2);
  BOOST_CHECK(!restarted.loadCommittedResult("window-1", NEXT_INPUT_DIGEST));
  BOOST_CHECK(!restarted.committedStatePath("window-1"));
  const auto oldResult = restarted.loadCommittedResult("window-0", INPUT_DIGEST);
  BOOST_REQUIRE(oldResult);
  BOOST_CHECK_EQUAL(textOf(*oldResult), "previous result");
  for (const auto& [path, bytes] : previous) {
    BOOST_CHECK_EQUAL(readFile(path), bytes);
  }
  BOOST_CHECK_NO_THROW(restarted.requireCapacity("window-1"));
  BOOST_CHECK_THROW(restarted.requireCapacity("window-2"), std::runtime_error);
  const auto result = bufferOf("verified recovery result");
  const auto state = bufferOf("verified recovery state");
  BOOST_REQUIRE(restarted.commit("window-1", NEXT_INPUT_DIGEST, result, state,
                                [] { return true; }));
  const auto restored = restarted.loadCommittedResult("window-1", NEXT_INPUT_DIGEST);
  BOOST_REQUIRE(restored);
  BOOST_CHECK_EQUAL(textOf(*restored), textOf(result));
  const auto statePath = restarted.committedStatePath("window-1");
  BOOST_REQUIRE(statePath);
  BOOST_CHECK_EQUAL(readFile(*statePath), textOf(state));
  BOOST_CHECK(!std::filesystem::exists(candidate / "manifest.json.tmp"));
  for (const auto* name : {"manifest.json", "result.json", "state.json"}) {
    const auto path = candidate / name;
    previous.emplace_back(path, readFile(path));
  }
  BOOST_CHECK_THROW(restarted.commit("window-1", NEXT_INPUT_DIGEST, result, state,
                                    [] { return true; }), std::runtime_error);
  BOOST_CHECK_THROW(restarted.loadCommittedResult("window-1", INPUT_DIGEST), std::runtime_error);
  for (const auto& [path, bytes] : previous) {
    BOOST_CHECK_EQUAL(readFile(path), bytes);
  }
}

BOOST_AUTO_TEST_CASE(CrossThreadRevocationOrdersFinalAuthorizationAndKeepsCommittedPairImmutable)
{
  using namespace std::chrono_literals;
  TemporaryDirectory temporary;
  const auto root = temporary.path / "run" / "mission";
  // Leave room for window-2 so its refusal must reach the authorization boundary.
  ndnsf::examples::uav::UavTrackingWorkerTransaction transaction(root, 3);
  BOOST_REQUIRE(transaction.commit("window-0", INPUT_DIGEST,
    bufferOf("previous result"), bufferOf("previous state"), [] { return true; }));
  std::vector<std::pair<std::filesystem::path, std::string>> previous;
  for (const auto* name : {"manifest.json", "result.json", "state.json"}) {
    const auto path = root / "window-0" / name;
    previous.emplace_back(path, readFile(path));
  }
  const auto candidate = root / "window-1";
  {
    std::atomic<bool> authorized{true};
    std::promise<void> boundary;
    auto atBoundary = boundary.get_future();
    std::promise<void> revoked;
    auto afterRevocation = revoked.get_future();
    const auto callerThread = std::this_thread::get_id();
    auto revoker = std::async(std::launch::async, [&] {
      if (atBoundary.wait_for(2s) != std::future_status::ready) {
        return false;
      }
      const bool candidateReady = std::this_thread::get_id() != callerThread &&
        readFile(candidate / "result.json") == "cancelled result" &&
        readFile(candidate / "state.json") == "cancelled state" &&
        std::filesystem::is_regular_file(candidate / "manifest.json.tmp") &&
        !std::filesystem::exists(candidate / "manifest.json");
      authorized.store(false, std::memory_order_release);
      revoked.set_value();
      return candidateReady;
    });
    const auto began = std::chrono::steady_clock::now();
    const bool committed = transaction.commit("window-1", NEXT_INPUT_DIGEST,
      bufferOf("cancelled result"), bufferOf("cancelled state"), [&] {
        boundary.set_value();
        if (afterRevocation.wait_for(2s) != std::future_status::ready) {
          throw std::runtime_error("transaction revoker did not reach the boundary");
        }
        return authorized.load(std::memory_order_acquire);
      });
    const bool observedCandidate = revoker.get();
    BOOST_CHECK(std::chrono::steady_clock::now() - began < 10s);
    BOOST_CHECK(observedCandidate);
    BOOST_CHECK(!authorized.load());
    BOOST_CHECK(!committed);
    BOOST_CHECK(!std::filesystem::exists(candidate));
    BOOST_CHECK(!transaction.loadCommittedResult("window-1", NEXT_INPUT_DIGEST));
    BOOST_CHECK(!transaction.committedStatePath("window-1"));
    const auto oldResult = transaction.loadCommittedResult("window-0", INPUT_DIGEST);
    BOOST_REQUIRE(oldResult);
    BOOST_CHECK_EQUAL(textOf(*oldResult), "previous result");
    for (const auto& [path, bytes] : previous) {
      BOOST_CHECK_EQUAL(readFile(path), bytes);
    }
  }
  {
    std::atomic<bool> authorized{true};
    std::promise<void> boundary;
    auto atBoundary = boundary.get_future();
    std::promise<void> waiting;
    auto revokerWaiting = waiting.get_future();
    std::promise<void> committedSignal;
    auto afterCommit = committedSignal.get_future();
    const auto callerThread = std::this_thread::get_id();
    auto revoker = std::async(std::launch::async, [&] {
      if (atBoundary.wait_for(2s) != std::future_status::ready) {
        return false;
      }
      waiting.set_value();
      if (afterCommit.wait_for(2s) != std::future_status::ready) {
        return false;
      }
      const bool markerPresent = std::this_thread::get_id() != callerThread &&
        std::filesystem::is_regular_file(candidate / "manifest.json") &&
        !std::filesystem::exists(candidate / "manifest.json.tmp");
      authorized.store(false, std::memory_order_release);
      return markerPresent;
    });
    const auto began = std::chrono::steady_clock::now();
    const bool committed = transaction.commit("window-1", NEXT_INPUT_DIGEST,
      bufferOf("committed result"), bufferOf("committed state"), [&] {
        boundary.set_value();
        if (revokerWaiting.wait_for(2s) != std::future_status::ready) {
          throw std::runtime_error("transaction revoker did not start waiting");
        }
        return authorized.load(std::memory_order_acquire);
      });
    if (committed) {
      committedSignal.set_value();
    }
    const bool observedCommit = revoker.get();
    BOOST_CHECK(std::chrono::steady_clock::now() - began < 10s);
    BOOST_REQUIRE(committed);
    BOOST_CHECK(observedCommit);
    BOOST_CHECK(!authorized.load());
    const auto restored = transaction.loadCommittedResult("window-1", NEXT_INPUT_DIGEST);
    BOOST_REQUIRE(restored);
    BOOST_CHECK_EQUAL(textOf(*restored), "committed result");
    const auto statePath = transaction.committedStatePath("window-1");
    BOOST_REQUIRE(statePath);
    BOOST_CHECK_EQUAL(readFile(*statePath), "committed state");
    for (const auto* name : {"manifest.json", "result.json", "state.json"}) {
      const auto path = candidate / name;
      previous.emplace_back(path, readFile(path));
    }
    bool finalCheckReached = false;
    BOOST_CHECK(!transaction.commit("window-2", INPUT_DIGEST,
      bufferOf("unauthorized result"), bufferOf("unauthorized state"), [&] {
        finalCheckReached = true;
        return authorized.load(std::memory_order_acquire);
      }));
    BOOST_CHECK(finalCheckReached);
    BOOST_CHECK(!std::filesystem::exists(root / "window-2"));
    BOOST_CHECK(!transaction.loadCommittedResult("window-2", INPUT_DIGEST));
    BOOST_CHECK(!transaction.committedStatePath("window-2"));
    BOOST_CHECK_THROW(transaction.commit("window-1", NEXT_INPUT_DIGEST,
      bufferOf("replacement"), bufferOf("replacement state"), [] { return true; }),
      std::runtime_error);
    for (const auto& [path, bytes] : previous) {
      BOOST_CHECK_EQUAL(readFile(path), bytes);
    }
  }
}

BOOST_AUTO_TEST_SUITE_END()
