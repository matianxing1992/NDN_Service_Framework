#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactStaging.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace ndnsf::di {
namespace {

constexpr const char* LeaseSuffix = ".ndnsf-di-provider-lease";

std::filesystem::path
leasePath(const std::filesystem::path& directory)
{
  return directory.parent_path() / (directory.filename().string() + LeaseSuffix);
}

bool
hasPrefix(const std::string& value, const char* prefix)
{
  const auto length = std::char_traits<char>::length(prefix);
  return value.size() > length && value.compare(0, length, prefix) == 0;
}

bool
pidIsLive(const std::filesystem::path& marker)
{
  std::ifstream input(marker);
  std::string field;
  std::string value;
  if (!(input >> field >> value) || field != "pid")
    return false;
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtol(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0' || parsed <= 1)
    return false;
  errno = 0;
  if (::kill(static_cast<pid_t>(parsed), 0) == 0)
    return true;
  return errno == EPERM;
}

bool
oldEnough(const std::filesystem::path& directory,
          const std::chrono::milliseconds staleAfter)
{
  std::error_code error;
  const auto modified = std::filesystem::last_write_time(directory, error);
  if (error)
    return false;
  const auto age = std::filesystem::file_time_type::clock::now() - modified;
  return age > staleAfter;
}

bool
isDirectoryWithoutSymlink(const std::filesystem::path& path)
{
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  return !error && std::filesystem::is_directory(status);
}

bool
eligible(const std::filesystem::path& directory,
         const std::chrono::milliseconds staleAfter)
{
  const auto marker = leasePath(directory);
  std::error_code error;
  if (std::filesystem::is_regular_file(marker, error) && !error) {
    if (pidIsLive(marker))
      return false;
    // A dead lease is an unambiguous crash/forced-stop boundary. It does not
    // need to wait for age before the next Provider can reclaim it.
    return true;
  }
  return oldEnough(directory, staleAfter);
}

std::size_t
removeCandidate(const std::filesystem::path& directory,
                const std::chrono::milliseconds staleAfter) noexcept
{
  if (!isDirectoryWithoutSymlink(directory) || !eligible(directory, staleAfter))
    return 0;
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  if (error)
    return 0;
  std::error_code markerError;
  std::filesystem::remove(leasePath(directory), markerError);
  return 1;
}

} // namespace

void
markNativeArtifactStagingLease(const std::filesystem::path& directory) noexcept
{
  try {
    const auto marker = leasePath(directory);
    const auto fd = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
      return;
    const std::string content = "pid " + std::to_string(static_cast<long long>(::getpid())) + "\n";
    const auto written = ::write(fd, content.data(), content.size());
    (void)::fchmod(fd, 0600);
    (void)::close(fd);
    if (written != static_cast<ssize_t>(content.size())) {
      std::error_code error;
      std::filesystem::remove(marker, error);
    }
  }
  catch (...) {
  }
}

std::size_t
cleanupNativeArtifactStaging(const std::filesystem::path& cacheDir,
                             const std::chrono::milliseconds staleAfter) noexcept
{
  if (cacheDir.empty() || staleAfter.count() <= 0)
    return 0;
  try {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(cacheDir, error);
    if (error || !isDirectoryWithoutSymlink(root))
      return 0;
    std::size_t removed = 0;
    const auto collect = [&] (const std::filesystem::path& parent,
                              const bool roleDirectories) {
      std::error_code iterationError;
      for (std::filesystem::directory_iterator it(parent, iterationError), end;
           !iterationError && it != end; it.increment(iterationError)) {
        const auto path = it->path();
        if (!isDirectoryWithoutSymlink(path))
          continue;
        const auto name = path.filename().string();
        if (roleDirectories) {
          std::error_code childError;
          for (std::filesystem::directory_iterator child(path, childError), childEnd;
               !childError && child != childEnd; child.increment(childError)) {
            const auto candidate = child->path();
            if (isDirectoryWithoutSymlink(candidate) &&
                hasPrefix(candidate.filename().string(), "assembly-"))
              removed += removeCandidate(candidate, staleAfter);
          }
        }
        else if (hasPrefix(name, "assembly-") ||
                 hasPrefix(name, "provider-protected-") ||
                 hasPrefix(name, "protected-plaintext-")) {
          removed += removeCandidate(path, staleAfter);
        }
      }
    };
    collect(root / ".staging", false);
    collect(root / "protected", true);
    return removed;
  }
  catch (...) {
    return 0;
  }
}

} // namespace ndnsf::di
