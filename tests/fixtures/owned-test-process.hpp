#ifndef NDNSF_TESTS_OWNED_TEST_PROCESS_HPP
#define NDNSF_TESTS_OWNED_TEST_PROCESS_HPP

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <map>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ndn_service_framework::test {

/** Execute and reap one test-owned process without inheriting application state. */
inline int
runOwnedTestProcess(const std::filesystem::path& executable, std::vector<std::string> arguments,
                    const std::map<std::string, std::string>& overrides,
                    std::chrono::milliseconds budget)
{
  if (arguments.empty() || budget <= std::chrono::milliseconds::zero())
    throw std::invalid_argument("invalid native child-process arguments or budget");
  std::vector<std::string> environment;
  for (auto entry = ::environ; *entry != nullptr; ++entry) {
    std::string value(*entry);
    if (overrides.count(value.substr(0, value.find('='))) == 0)
      environment.push_back(std::move(value));
  }
  for (const auto& [key, value] : overrides)
    environment.push_back(key + "=" + value);
  std::vector<char*> argv, envp;
  for (auto& value : arguments)
    argv.push_back(value.data());
  for (auto& value : environment)
    envp.push_back(value.data());
  argv.push_back(nullptr);
  envp.push_back(nullptr);
  pid_t child = -1;
  const auto error = ::posix_spawn(&child, executable.c_str(), nullptr, nullptr,
                                  argv.data(), envp.data());
  if (error != 0)
    throw std::runtime_error("cannot spawn native test process: " + std::to_string(error));
  const auto deadline = std::chrono::steady_clock::now() + budget;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      if (!WIFEXITED(status))
        throw std::runtime_error("native test process terminated by signal");
      return WEXITSTATUS(status);
    }
    if (waited < 0 && errno != EINTR)
      throw std::runtime_error("cannot reap owned native test process");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ::kill(child, SIGKILL);
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  throw std::runtime_error("native test process exceeded its budget");
}

} // namespace ndn_service_framework::test

#endif // NDNSF_TESTS_OWNED_TEST_PROCESS_HPP
