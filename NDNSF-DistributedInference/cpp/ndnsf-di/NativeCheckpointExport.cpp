#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCheckpointExport.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-private-checkpoint-file.hpp"

#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <stdexcept>
#include <string>
#include <utility>

namespace ndnsf::di {
namespace {

class Fd
{
public:
  explicit Fd(int fd = -1) noexcept : m_fd(fd) {}
  ~Fd() { reset(); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const noexcept { return m_fd; }
  void reset(int fd = -1) noexcept
  {
    if (m_fd >= 0) ::close(m_fd);
    m_fd = fd;
  }

private:
  int m_fd;
};

void require(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}

void invoke(const NativeCheckpointExportOptions& options, NativeCheckpointExportStage stage,
            const std::function<bool()>& cancelled)
{
  if (options.beforeStage) options.beforeStage(stage);
  if (cancelled && cancelled()) {
    throw std::runtime_error("native checkpoint cancelled");
  }
}

void writeAll(int fd, std::string_view bytes)
{
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    require(count > 0, "native checkpoint write failed");
    offset += static_cast<std::size_t>(count);
  }
}

void requireRegularOwner(int fd)
{
  struct stat value{};
  require(::fstat(fd, &value) == 0 && S_ISREG(value.st_mode) &&
          value.st_uid == ::geteuid() && value.st_nlink == 1,
          "native checkpoint temporary file ownership invalid");
  require(::fchmod(fd, S_IRUSR | S_IWUSR) == 0, "native checkpoint mode failed");
  require(::fstat(fd, &value) == 0 && (value.st_mode & 0777) == 0600,
          "native checkpoint mode is not 0600");
}

std::filesystem::path makeTemporaryPath(const std::filesystem::path& parent,
                                        const std::string& filename,
                                        Fd& fd)
{
  static std::atomic<unsigned long> sequence{0};
  for (unsigned int attempt = 0; attempt != 128; ++attempt) {
    const auto suffix = ".tmp-" + std::to_string(static_cast<unsigned long>(::getpid())) +
      "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    const auto candidate = parent / (filename + suffix);
    const auto opened = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL |
      O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (opened >= 0) {
      fd.reset(opened);
      requireRegularOwner(fd.get());
      return candidate;
    }
    if (errno != EEXIST) throw std::runtime_error("native checkpoint temporary file open failed");
  }
  throw std::runtime_error("native checkpoint temporary file name exhausted");
}

} // namespace

NativeCheckpointRollbackUnconfirmed::NativeCheckpointRollbackUnconfirmed(
  std::filesystem::path retainedBackup)
  : std::runtime_error("native checkpoint rollback unconfirmed")
  , m_retainedBackup(std::move(retainedBackup))
{
}

void
detail::writePrivateCheckpointFile(const std::filesystem::path& destination,
                                        const NativeCheckpointExportOptions& options,
                                        const std::function<void(int)>& writer,
                                        const std::function<bool()>& cancelled)
{
  require(!destination.empty() && destination.filename() != "." &&
          destination.filename() != "..", "native checkpoint destination invalid");
  const auto parent = destination.parent_path().empty() ? std::filesystem::path(".") : destination.parent_path();
  struct stat existing{};
  const bool hadDestination = ::lstat(destination.c_str(), &existing) == 0;
  if (hadDestination)
    require(!S_ISLNK(existing.st_mode), "native checkpoint destination symlink rejected");
  else require(errno == ENOENT, "native checkpoint destination stat failed");
  if (hadDestination)
    require(S_ISREG(existing.st_mode), "native checkpoint destination is not a regular file");

  Fd parentFd(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  require(parentFd.get() >= 0, "native checkpoint parent directory unavailable");
  Fd temporary;
  const auto temporaryPath = makeTemporaryPath(parent, destination.filename().string(), temporary);
  struct stat newFile{};
  bool renamedNew = false;
  const auto rename = [&] (unsigned flags) {
    return ::syscall(SYS_renameat2, AT_FDCWD, temporaryPath.c_str(),
                     AT_FDCWD, destination.c_str(), flags);
  };
  const auto discard = [] (const std::filesystem::path& path) {
    return ::unlink(path.c_str()) == 0 || errno == ENOENT;
  };
  try {
    invoke(options, NativeCheckpointExportStage::BeforeWrite, cancelled);
    writer(temporary.get());
    invoke(options, NativeCheckpointExportStage::BeforeFileSync, cancelled);
    require(::fsync(temporary.get()) == 0, "native checkpoint file sync failed");
    require(::fstat(temporary.get(), &newFile) == 0, "native checkpoint file stat failed");
    invoke(options, NativeCheckpointExportStage::BeforeRename, cancelled);
    if (hadDestination) {
      struct stat active{};
      require(::lstat(destination.c_str(), &active) == 0 &&
                active.st_dev == existing.st_dev && active.st_ino == existing.st_ino,
              "native checkpoint destination changed");
    }
    // Exchange leaves the old inode under the private temporary name while
    // keeping the active path readable throughout the replacement.
    require(rename(hadDestination ? RENAME_EXCHANGE : RENAME_NOREPLACE) == 0,
            "native checkpoint atomic rename failed");
    renamedNew = true;
    invoke(options, NativeCheckpointExportStage::BeforeDirectorySync, cancelled);
    require(::fsync(parentFd.get()) == 0, "native checkpoint directory sync failed");
    if (hadDestination)
      require(discard(temporaryPath), "native checkpoint backup cleanup failed");
  }
  catch (...) {
    if (renamedNew) {
      // Keep the new inode pinned so a concurrent replacement cannot recycle its identity.
      struct stat active{};
      const bool stillOwnsOutput = ::lstat(destination.c_str(), &active) == 0 &&
        active.st_dev == newFile.st_dev && active.st_ino == newFile.st_ino;
      if (!stillOwnsOutput || (hadDestination ? rename(RENAME_EXCHANGE) != 0
                                               : !discard(destination))) {
        (void)::fsync(parentFd.get());
        throw NativeCheckpointRollbackUnconfirmed(hadDestination ? temporaryPath :
                                                                        std::filesystem::path());
      }
    }
    const bool cleanupConfirmed = discard(temporaryPath);
    if (::fsync(parentFd.get()) != 0 || !cleanupConfirmed)
      throw NativeCheckpointRollbackUnconfirmed();
    throw;
  }
}

void
detail::writePrivateCheckpointBytes(const std::filesystem::path& destination,
                                    std::string_view bytes,
                                    const NativeCheckpointExportOptions& options)
{
  writePrivateCheckpointFile(destination, options, [&] (int fd) { writeAll(fd, bytes); });
}

void
nativeExportPrivateCheckpoint(const std::filesystem::path& destination,
                              const NativeJson& state,
                              const NativeCheckpointExportOptions& options)
{
  detail::writePrivateCheckpointBytes(destination, nativeCanonicalJson(state), options);
}

} // namespace ndnsf::di
