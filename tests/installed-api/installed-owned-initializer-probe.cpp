#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-owned-initializer-file.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <ndnsf-distributed-repo/ArtifactRepositoryApi.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace ndnsf::di;

namespace {

struct ProbeResources
{
  int fd = -1;
  pid_t child = -1;
  bool created = false;
  std::vector<char> path;
  std::filesystem::path privateDirectory;

  ~ProbeResources()
  {
    if (child > 0) {
      int status = 0;
      pid_t result;
      do {
        result = ::waitpid(child, &status, WNOHANG);
      } while (result < 0 && errno == EINTR);
      if (result == 0) {
        ::kill(child, SIGKILL);
        do {
          result = ::waitpid(child, &status, 0);
        } while (result < 0 && errno == EINTR);
      }
    }
    if (fd >= 0) ::close(fd);
    if (created) ::unlink(path.data());
    if (!privateDirectory.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(privateDirectory, ignored);
    }
  }
};

} // namespace

int
main(int argc, char** argv)
{
  try {
    const bool throughRepo = argc == 4 && std::string(argv[3]) == "--repo";
    if (argc != 3 && !throughRepo)
      throw std::invalid_argument("pass NFS/local directories and optional --repo");
    NativeJson results = NativeJson::array();
    for (int index = 1; index <= 2; ++index) {
      const std::filesystem::path directory(argv[index]);
      std::filesystem::create_directories(directory);
      const auto name = (directory / "owned-snapshot-XXXXXX").string();
      ProbeResources resources;
      resources.path.assign(name.begin(), name.end());
      resources.path.push_back('\0');
      resources.fd = ::mkstemp(resources.path.data());
      if (resources.fd < 0) throw std::runtime_error("probe mkstemp failed");
      resources.created = true;
      if (throughRepo) {
        const auto root = (directory / "api-XXXXXX").string();
        std::vector<char> pattern(root.begin(), root.end());
        pattern.push_back('\0');
        if (!::mkdtemp(pattern.data())) throw std::runtime_error("probe mkdtemp failed");
        resources.privateDirectory = pattern.data();
      }
      const std::vector<std::uint8_t> bytes(2U << 20, 17);
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const auto count = ::write(resources.fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("probe write failed");
        offset += static_cast<std::size_t>(count);
      }
      struct statfs filesystem{};
      if (::fstatfs(resources.fd, &filesystem))
        throw std::runtime_error("probe statfs failed");
      const int closed = ::close(resources.fd);
      resources.fd = -1;
      if (closed)
        throw std::runtime_error("probe file close failed");
      const auto child = ::fork();
      if (child < 0) throw std::runtime_error("probe fork failed");
      resources.child = child;
      if (child == 0) {
        const struct rlimit noCore{0, 0};
        ::setrlimit(RLIMIT_CORE, &noCore);
        std::signal(SIGIO, SIG_DFL);
        sigset_t unblocked;
        ::sigemptyset(&unblocked);
        ::sigaddset(&unblocked, SIGIO);
        ::sigprocmask(SIG_UNBLOCK, &unblocked, nullptr);
        ::alarm(20);
        try {
          std::cout << "SNAPSHOT_BEFORE_TAKE " << directory << std::endl;
          const auto expected = nativePlanningDigest(bytes.data(), bytes.size());
          const NativeAssemblyControl control{
            std::chrono::steady_clock::now() + std::chrono::seconds(15),
            [] {}, bytes.size(), bytes.size()};
          std::optional<ndnsf_distributed_repo::ArtifactRepositoryApi> api;
          ProbeResources materialized;
          const char* input = resources.path.data();
          if (throughRepo) {
            const std::string producer = "/ndnsf/di/spec222/snapshot-probe";
            api.emplace(ndnsf_distributed_repo::ArtifactRepositoryApi::makeInApp(
              {resources.privateDirectory, producer}));
            ndnsf_distributed_repo::ArtifactReference reference;
            reference.logicalName = producer + "/initializer/" + expected.substr(7);
            reference.publisherIdentity = producer;
            reference.contentDigest = expected.substr(7);
            reference.sizeBytes = bytes.size();
            reference.formatVersion = "raw-file-v1";
            reference.policyEpoch = "plaintext-v1";
            const auto published = api->publishFileUntil(input, reference, control.deadline);
            if (!published.handle || !published.receipt)
              throw std::runtime_error("probe Repo publication incomplete");
            const auto destination = resources.privateDirectory / "materialized";
            std::filesystem::create_directory(destination);
            const auto name = (destination / "fetched-XXXXXX").string();
            materialized.path.assign(name.begin(), name.end());
            materialized.path.push_back('\0');
            materialized.fd = ::mkstemp(materialized.path.data());
            if (materialized.fd < 0) throw std::runtime_error("probe materialization reserve failed");
            materialized.created = true;
            const int closed = ::close(materialized.fd);
            materialized.fd = -1;
            if (closed) throw std::runtime_error("probe materialization close failed");
            api->fetchFileUntil(reference, materialized.path.data(), control.deadline);
            input = materialized.path.data();
            std::cout << "SNAPSHOT_AFTER_REPO_FETCH " << directory << std::endl;
          }
          NativeCanonicalSource source;
          source.initializerRangeSource = takeNativeCanonicalInitializerFile(
            input, bytes.size(), expected, control);
          for (int repeat = 0; repeat < 3; ++repeat) {
            if (nativeCanonicalInitializerDigest(source, control) != expected)
              throw std::runtime_error("probe digest mismatch");
          }
          // Keep the owner alive after the fast digest calls so a delayed
          // lease notification is observable rather than hidden by _exit.
          std::cout << "SNAPSHOT_HOLD_BEGIN " << directory << std::endl;
          ::sleep(5);
          std::cout << "SNAPSHOT_COMPLETED " << directory << std::endl;
          ::_exit(0);
        }
        catch (const std::exception& error) {
          std::cerr << "SNAPSHOT_EXCEPTION " << error.what() << std::endl;
          ::_exit(3);
        }
      }
      int status = 0;
      pid_t waited;
      do {
        waited = ::waitpid(child, &status, 0);
      } while (waited < 0 && errno == EINTR);
      if (waited != child)
        throw std::runtime_error("probe wait failed");
      resources.child = -1;
      results.push_back({{"directory", directory.string()},
        {"filesystemType", static_cast<std::uint64_t>(filesystem.f_type)},
        {"throughRepo", throughRepo},
        {"bytes", bytes.size()},
        {"exitCode", WIFEXITED(status) ? NativeJson(WEXITSTATUS(status)) : NativeJson(nullptr)},
        {"signal", WIFSIGNALED(status) ? NativeJson(WTERMSIG(status)) : NativeJson(nullptr)}});
    }
    std::cout << "OWNED_INITIALIZER_PROBE " << nativeCanonicalJson(results) << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "OWNED_INITIALIZER_PROBE_FAILED " << error.what() << std::endl;
    return 1;
  }
}
