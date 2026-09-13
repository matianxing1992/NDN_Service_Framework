#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCheckpointExport.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <stdexcept>
#include <string>

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
  int release() noexcept { const auto fd = m_fd; m_fd = -1; return fd; }
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

void invoke(const NativeCheckpointExportOptions& options, NativeCheckpointExportStage stage)
{
  if (options.beforeStage) options.beforeStage(stage);
}

void writeAll(int fd, const std::string& bytes)
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

void nativeExportPrivateCheckpoint(const std::filesystem::path& destination,
                                   const NativeJson& state,
                                   const NativeCheckpointExportOptions& options)
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
  const auto bytes = nativeCanonicalJson(state);
  Fd temporary;
  const auto temporaryPath = makeTemporaryPath(parent, destination.filename().string(), temporary);
  Fd backupPlaceholder;
  std::filesystem::path backupPath;
  bool movedOld = false;
  bool renamedNew = false;
  try {
    invoke(options, NativeCheckpointExportStage::BeforeWrite);
    writeAll(temporary.get(), bytes);
    invoke(options, NativeCheckpointExportStage::BeforeFileSync);
    require(::fsync(temporary.get()) == 0, "native checkpoint file sync failed");
    require(::close(temporary.release()) == 0, "native checkpoint file close failed");
    if (hadDestination) {
      backupPath = makeTemporaryPath(parent, destination.filename().string() + ".backup",
                                     backupPlaceholder);
      backupPlaceholder.reset();
      require(::unlink(backupPath.c_str()) == 0, "native checkpoint backup placeholder cleanup failed");
      require(::rename(destination.c_str(), backupPath.c_str()) == 0,
              "native checkpoint old file backup failed");
      movedOld = true;
    }
    invoke(options, NativeCheckpointExportStage::BeforeRename);
    require(::rename(temporaryPath.c_str(), destination.c_str()) == 0,
            "native checkpoint atomic rename failed");
    renamedNew = true;
    invoke(options, NativeCheckpointExportStage::BeforeDirectorySync);
    require(::fsync(parentFd.get()) == 0, "native checkpoint directory sync failed");
    if (movedOld) {
      require(::unlink(backupPath.c_str()) == 0, "native checkpoint backup cleanup failed");
      movedOld = false;
    }
  }
  catch (...) {
    if (temporary.get() >= 0) temporary.reset();
    (void)::unlink(temporaryPath.c_str());
    if (renamedNew)
      (void)::unlink(destination.c_str());
    if (movedOld)
      (void)::rename(backupPath.c_str(), destination.c_str());
    if (!backupPath.empty())
      (void)::unlink(backupPath.c_str());
    throw;
  }
}

} // namespace ndnsf::di
