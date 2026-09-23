#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

struct ChildOutput
{
  int status = 0;
  std::string output;
  bool timedOut = false;
  bool firstEventReadWhileRunning = false;
  bool generationPausedAfterFirstEvent = false;
  bool firstEventBeforeTerminal = false;
};

bool
existsNoThrow(const std::filesystem::path& path)
{
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

std::string
jsonString(const std::string& value)
{
  std::ostringstream result;
  result << '"';
  for (const auto character : value) {
    switch (character) {
      case '"': result << "\\\""; break;
      case '\\': result << "\\\\"; break;
      case '\n': result << "\\n"; break;
      case '\r': result << "\\r"; break;
      case '\t': result << "\\t"; break;
      default: result << character; break;
    }
  }
  result << '"';
  return result.str();
}

ChildOutput
runRequester(const std::string& binary, const std::string& config,
             std::chrono::milliseconds timeout,
             const std::filesystem::path& readyFile,
             const std::filesystem::path& releaseFile)
{
  int pipeEnds[2];
  if (::pipe(pipeEnds) != 0)
    throw std::runtime_error("spec190 live-turns pipe creation failed");
  const auto child = ::fork();
  if (child < 0) {
    ::close(pipeEnds[0]);
    ::close(pipeEnds[1]);
    throw std::runtime_error("spec190 live-turns fork failed");
  }
  if (child == 0) {
    ::dup2(pipeEnds[1], STDOUT_FILENO);
    ::dup2(pipeEnds[1], STDERR_FILENO);
    ::close(pipeEnds[0]);
    ::close(pipeEnds[1]);
    ::execl(binary.c_str(), binary.c_str(), "--config", config.c_str(),
            static_cast<char*>(nullptr));
    ::_exit(127);
  }

  ::close(pipeEnds[1]);
  const int flags = ::fcntl(pipeEnds[0], F_GETFL, 0);
  if (flags >= 0)
    ::fcntl(pipeEnds[0], F_SETFL, flags | O_NONBLOCK);
  ChildOutput result;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  bool reaped = false;
  while (!reaped || std::chrono::steady_clock::now() < deadline) {
    bool childRunningBeforeRead = !reaped;
    if (!reaped) {
      const auto waited = ::waitpid(child, &result.status, WNOHANG);
      if (waited == child) {
        reaped = true;
        childRunningBeforeRead = false;
      }
      else if (waited < 0) {
        result.timedOut = true;
        break;
      }
    }
    char buffer[4096];
    for (;;) {
      const auto count = ::read(pipeEnds[0], buffer, sizeof(buffer));
      if (count > 0) {
        result.output.append(buffer, static_cast<std::size_t>(count));
        continue;
      }
      break;
    }
    if (childRunningBeforeRead && !result.firstEventReadWhileRunning &&
        result.output.find("NATIVE_STREAM_FIRST_EVENT") != std::string::npos) {
      result.firstEventReadWhileRunning = true;
      result.firstEventBeforeTerminal =
        result.output.find("NATIVE_STREAM_TERMINAL") == std::string::npos;
      // The production requester waits on this explicit test barrier after
      // flushing the first token bytes.  Keep it blocked long enough to prove
      // the parent observed live output while terminal delivery was impossible.
      const auto readyDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
      while (!existsNoThrow(readyFile) &&
             std::chrono::steady_clock::now() < readyDeadline)
        ::usleep(10'000);
      if (!existsNoThrow(readyFile))
        result.timedOut = true;
      ::usleep(100'000);
      int status = 0;
      const auto waited = ::waitpid(child, &status, WNOHANG);
      result.generationPausedAfterFirstEvent = waited == 0;
      if (waited == child) {
        result.status = status;
        reaped = true;
      }
      std::ofstream release(releaseFile, std::ios::trunc);
      release << "release\n";
      release.flush();
    }
    if (reaped)
      break;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0)
      break;
    struct pollfd descriptor{pipeEnds[0], POLLIN, 0};
    (void)::poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(
      remaining.count(), 100)));
  }
  if (!reaped) {
    result.timedOut = true;
    ::kill(child, SIGKILL);
    (void)::waitpid(child, &result.status, 0);
  }
  for (;;) {
    char buffer[4096];
    const auto count = ::read(pipeEnds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    result.output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(pipeEnds[0]);
  return result;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec190LiveTurns)

BOOST_AUTO_TEST_CASE(ParentPipeReadsLiveEventsBeforeTerminal)
{
  const char* binary = std::getenv("SPEC190_NATIVE_REQUESTER_BINARY");
  const char* config = std::getenv("SPEC190_NATIVE_TURNS_CONFIG");
  if (!binary || !config || *binary == '\0' || *config == '\0') {
    BOOST_TEST_MESSAGE(
      "SPEC190_NATIVE_REQUESTER_BINARY/SPEC190_NATIVE_TURNS_CONFIG absent; "
      "real CLI live-turn qualification is unobserved");
    return;
  }

  struct Cleanup {
    std::vector<std::filesystem::path> paths;
    ~Cleanup()
    {
      std::error_code error;
      for (const auto& path : paths)
        std::filesystem::remove(path, error);
    }
  } cleanup;
  const auto configPath = std::filesystem::absolute(config);
  const auto suffix = std::to_string(static_cast<long long>(::getpid()));
  const auto readyFile = configPath.parent_path() / ("spec190-live-ready-" + suffix);
  const auto releaseFile = configPath.parent_path() / ("spec190-live-release-" + suffix);
  const auto testConfig = configPath.parent_path() / ("spec190-live-config-" + suffix + ".json");
  cleanup.paths = {testConfig, readyFile, releaseFile};
  std::error_code cleanupError;
  std::filesystem::remove(readyFile, cleanupError);
  std::filesystem::remove(releaseFile, cleanupError);
  std::ifstream input(configPath);
  BOOST_REQUIRE(input.good());
  const std::string original((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  auto testDocument = original;
  while (!testDocument.empty() &&
         std::isspace(static_cast<unsigned char>(testDocument.back())))
    testDocument.pop_back();
  BOOST_REQUIRE(!testDocument.empty() && testDocument.back() == '}');
  testDocument.pop_back();
  // Keep the launcher's numeric JSON types intact.  property_tree loses the
  // distinction between JSON numbers and strings when it writes a tree back,
  // while the native runtime intentionally rejects string-valued limits.
  testDocument += ",\"test_only\":true,\"test_stream_barrier\":{";
  testDocument += "\"ready_file\":" + jsonString(readyFile.string());
  testDocument += ",\"release_file\":" + jsonString(releaseFile.string());
  testDocument += ",\"timeout_ms\":10000}}\n";
  std::ofstream output(testConfig);
  BOOST_REQUIRE(output.good());
  output << testDocument;
  output.close();

  // A cold two-provider assembly can consume most of the native request
  // budget before the first token.  Keep the parent bound above that budget
  // so the live-event assertion observes the production deadline rather than
  // killing a still-valid runner preparation.
  const auto child = runRequester(binary, testConfig.string(), std::chrono::minutes(20),
                                  readyFile, releaseFile);
  // Preserve the real CLI's live markers in the parent process output.  The
  // parent already observed the first event through its pipe before release;
  // forwarding the captured bytes also lets the surrounding MiniNDN launcher
  // apply its ordinary per-turn terminal oracle to this C++ driver.
  std::cout << child.output << std::flush;
  BOOST_REQUIRE_MESSAGE(!child.timedOut, "real native requester did not exit within its bound");
  BOOST_REQUIRE_MESSAGE(WIFEXITED(child.status) && WEXITSTATUS(child.status) == 0,
                        child.output);
  const auto first = child.output.find("NATIVE_STREAM_FIRST_EVENT");
  const auto terminal = child.output.find("NATIVE_STREAM_TERMINAL");
  BOOST_REQUIRE_MESSAGE(first != std::string::npos, child.output);
  BOOST_REQUIRE_MESSAGE(terminal != std::string::npos, child.output);
  BOOST_REQUIRE(child.firstEventReadWhileRunning);
  BOOST_REQUIRE(child.generationPausedAfterFirstEvent);
  BOOST_REQUIRE(child.firstEventBeforeTerminal);
  const auto pending = child.output.find("NATIVE_STREAM_LIVE_PENDING");
  BOOST_REQUIRE(pending != std::string::npos);
  BOOST_CHECK_LT(pending, first);
  const auto firstStatus = child.output.find("NATIVE_STREAM_FIRST_EVENT_STATUS=PENDING");
  BOOST_REQUIRE(firstStatus != std::string::npos);
  BOOST_CHECK_LT(firstStatus, terminal);
  BOOST_CHECK_LT(first, terminal);
  BOOST_CHECK_NE(child.output.find("NATIVE_CONVERSATION_TURNS_SUCCEEDED"), std::string::npos);
  BOOST_CHECK_NE(child.output.find("NATIVE_REQUEST_SUCCEEDED"), std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
