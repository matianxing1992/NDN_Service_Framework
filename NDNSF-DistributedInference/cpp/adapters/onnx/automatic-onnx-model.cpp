#include "automatic-onnx-model.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace ndnsf::di {
namespace {

constexpr std::size_t MAX_PATH_BYTES = 4096;

[[noreturn]] void
failSource(const char* code)
{
  throw std::runtime_error(std::string("DI_AUTOMATIC_ONNX_") + code);
}

void
requireActive(const NativeAssemblyControl& control)
{
  if (!control.requireActive) failSource("OWNER");
  control.requireActive();
  if (std::chrono::steady_clock::now() >= control.deadline) failSource("DEADLINE");
}

class OwnedFd
{
public:
  explicit OwnedFd(int fd)
    : m_fd(fd)
  {
  }
  ~OwnedFd()
  {
    if (m_fd >= 0) ::close(m_fd);
  }
  OwnedFd(OwnedFd&& other) noexcept
    : m_fd(std::exchange(other.m_fd, -1))
  {
  }
  OwnedFd(const OwnedFd&) = delete;
  OwnedFd& operator=(const OwnedFd&) = delete;
  OwnedFd& operator=(OwnedFd&& other) noexcept
  {
    if (this != &other) {
      if (m_fd >= 0) ::close(m_fd);
      m_fd = std::exchange(other.m_fd, -1);
    }
    return *this;
  }
  int
  get() const noexcept
  {
    return m_fd;
  }

private:
  int m_fd;
};

std::vector<std::string>
components(const std::filesystem::path& path, bool absolute)
{
  const auto text = path.native();
  if (text.empty() || text.size() > MAX_PATH_BYTES || text.find('\0') != std::string::npos ||
      text.find('\\') != std::string::npos || path.is_absolute() != absolute ||
      text.back() == '/' || text.find("//") != std::string::npos)
    failSource("PATH");
  std::vector<std::string> result;
  for (const auto& part : path) {
    if (part == "/" && absolute) continue;
    if (part.empty() || (!absolute && (part == "." || part == ".."))) failSource("PATH");
    result.push_back(part.native());
  }
  if (result.empty()) failSource("PATH");
  return result;
}

OwnedFd
openDirectoryAt(int parent, const std::string& name)
{
  OwnedFd fd(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (fd.get() < 0) failSource("DIRECTORY");
  return fd;
}

/** One fd and its original stat snapshot; the path is never consulted again. */
class SourceFile final : public NativeCanonicalByteRangeSource
{
public:
  explicit SourceFile(OwnedFd fd)
    : m_fd(std::move(fd))
  {
    if (::fstat(m_fd.get(), &m_snapshot) != 0 || !S_ISREG(m_snapshot.st_mode) ||
        m_snapshot.st_size <= 0) failSource("FILE");
  }

  std::uint64_t
  size() const noexcept override
  {
    return m_snapshot.st_size;
  }

  std::vector<std::uint8_t>
  read(std::uint64_t offset, std::uint64_t length) const override
  {
    if (offset > size() || length > size() - offset ||
        length > NativeCanonicalMaterialBundleMaxBytes) failSource("RANGE");
    if (m_recording) {
      if (offset != m_blockDigests.size() * NativeCanonicalMaterialBundleMaxBytes ||
          length != std::min(NativeCanonicalMaterialBundleMaxBytes, size() - offset))
        failSource("OWNER");
      const auto bytes = readRaw(offset, length);
      m_blockDigests.push_back(nativePlanningDigest(bytes.data(), bytes.size()));
      return bytes;
    }
    std::vector<std::uint8_t> result;
    result.reserve(static_cast<std::size_t>(length));
    while (length > 0) {
      const auto index = offset / NativeCanonicalMaterialBundleMaxBytes;
      const auto blockOffset = index * NativeCanonicalMaterialBundleMaxBytes;
      const auto blockBytes = std::min(NativeCanonicalMaterialBundleMaxBytes, size() - blockOffset);
      const auto bytes = readRaw(blockOffset, blockBytes);
      if (nativePlanningDigest(bytes.data(), bytes.size()) != m_blockDigests.at(index))
        failSource("SOURCE_CHANGED");
      const auto begin = static_cast<std::size_t>(offset - blockOffset);
      const auto count = static_cast<std::size_t>(std::min(length, blockBytes - begin));
      result.insert(result.end(), bytes.begin() + begin, bytes.begin() + begin + count);
      offset += count;
      length -= count;
    }
    return result;
  }

  void
  finishSeal()
  {
    if (!m_recording || m_blockDigests.size() !=
        1 + (size() - 1) / NativeCanonicalMaterialBundleMaxBytes) failSource("OWNER");
    m_recording = false;
  }

private:
  std::vector<std::uint8_t>
  readRaw(std::uint64_t offset, std::uint64_t length) const
  {
    checkSnapshot();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    std::size_t consumed = 0;
    unsigned interrupted = 0;
    while (consumed < bytes.size()) {
      const auto count = ::pread(m_fd.get(), bytes.data() + consumed, bytes.size() - consumed,
                                 static_cast<off_t>(offset + consumed));
      if (count < 0 && errno == EINTR && ++interrupted <= 3) continue;
      if (count <= 0) failSource("READ");
      interrupted = 0;
      consumed += static_cast<std::size_t>(count);
    }
    checkSnapshot();
    return bytes;
  }

  void
  checkSnapshot() const
  {
    struct stat current{};
    if (::fstat(m_fd.get(), &current) != 0 || current.st_dev != m_snapshot.st_dev ||
        current.st_ino != m_snapshot.st_ino || current.st_mode != m_snapshot.st_mode ||
        current.st_size != m_snapshot.st_size ||
        current.st_mtim.tv_sec != m_snapshot.st_mtim.tv_sec ||
        current.st_mtim.tv_nsec != m_snapshot.st_mtim.tv_nsec ||
        current.st_ctim.tv_sec != m_snapshot.st_ctim.tv_sec ||
        current.st_ctim.tv_nsec != m_snapshot.st_ctim.tv_nsec) failSource("SOURCE_CHANGED");
  }

  OwnedFd m_fd;
  struct stat m_snapshot{};
  // Written only during the private initial hash pass; immutable before publication.
  mutable std::vector<std::string> m_blockDigests;
  bool m_recording = true;
};

std::shared_ptr<SourceFile>
openFileAt(int root, const std::vector<std::string>& parts,
           const NativeAssemblyControl& control)
{
  requireActive(control);
  OwnedFd directory(::fcntl(root, F_DUPFD_CLOEXEC, 0));
  if (directory.get() < 0) failSource("DIRECTORY");
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    requireActive(control);
    directory = openDirectoryAt(directory.get(), parts[i]);
  }
  OwnedFd fd(::openat(directory.get(), parts.back().c_str(),
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (fd.get() < 0) failSource("FILE");
  return std::make_shared<SourceFile>(std::move(fd));
}

class FileRanges final : public NativeCanonicalByteRangeSource
{
public:
  explicit FileRanges(std::vector<std::shared_ptr<const SourceFile>> files)
    : m_files(std::move(files))
  {
    for (const auto& file : m_files) {
      if (file->size() > std::numeric_limits<std::uint64_t>::max() - m_size)
        failSource("SOURCE_LIMIT");
      m_size += file->size();
    }
  }
  std::uint64_t
  size() const noexcept override
  {
    return m_size;
  }

  std::vector<std::uint8_t>
  read(std::uint64_t offset, std::uint64_t length) const override
  {
    if (offset > m_size || length > m_size - offset ||
        length > NativeCanonicalMaterialBundleMaxBytes) failSource("RANGE");
    std::vector<std::uint8_t> result;
    result.reserve(static_cast<std::size_t>(length));
    for (const auto& file : m_files) {
      if (offset >= file->size()) {
        offset -= file->size();
        continue;
      }
      const auto count = std::min(length, file->size() - offset);
      const auto bytes = file->read(offset, count);
      result.insert(result.end(), bytes.begin(), bytes.end());
      length -= count;
      offset = 0;
      if (length == 0) break;
    }
    return result;
  }

private:
  const std::vector<std::shared_ptr<const SourceFile>> m_files;
  std::uint64_t m_size = 0;
};

std::string
fileDigest(const std::shared_ptr<SourceFile>& file, const NativeAssemblyControl& control)
{
  NativeCanonicalSource source;
  source.initializerRangeSource = file;
  const auto digest = nativeCanonicalInitializerDigest(source, control);
  file->finishSeal();
  return digest;
}

} // namespace

std::string
automaticOnnxSourceDigest(const std::string& modelDigest,
                          const std::vector<AutomaticOnnxSourceFile>& files,
                          const NativeAssemblyControl& control)
{
  requireActive(control);
  const auto isDigest = [](const std::string& value) {
    return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
      value.find_first_not_of("0123456789abcdef", 7) == std::string::npos;
  };
  if (!isDigest(modelDigest) || files.size() > NATIVE_ONNX_MAX_EXTERNAL_FILES)
    failSource("INVENTORY");
  auto identities = NativeJson::array();
  std::string previous;
  std::uint64_t bytes = 0;
  for (const auto& file : files) {
    requireActive(control);
    components(file.location, false);
    if ((!previous.empty() && previous >= file.location) || file.bytes == 0 ||
        file.bytes > std::numeric_limits<std::uint64_t>::max() - bytes || !isDigest(file.digest))
      failSource("INVENTORY");
    previous = file.location;
    bytes += file.bytes;
    identities.push_back(NativeJson{{"location", file.location}, {"bytes", file.bytes},
                                     {"digest", file.digest}});
  }
  requireActive(control);
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-automatic-source-v1"}, {"modelDigest", modelDigest},
    {"files", std::move(identities)}}));
}

OpenedAutomaticOnnxSource
openAutomaticOnnxSource(const std::filesystem::path& modelFile,
                        const NativeAssemblyControl& control)
{
  requireActive(control);
  const auto path = std::filesystem::absolute(modelFile);
  const auto parts = components(path, true);
  OwnedFd root(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (root.get() < 0) failSource("DIRECTORY");
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    requireActive(control);
    root = openDirectoryAt(root.get(), parts[i]);
  }
  const auto model = openFileAt(root.get(), {parts.back()}, control);
  if (control.maxSourceBytes == 0 || model->size() > control.maxSourceBytes ||
      model->size() > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    failSource("SOURCE_LIMIT");
  OpenedAutomaticOnnxSource result;
  result.modelDigest = fileDigest(model, control);
  result.source.modelBytes.reserve(static_cast<std::size_t>(model->size()));
  for (std::uint64_t offset = 0; offset < model->size();) {
    requireActive(control);
    const auto length = std::min(NativeCanonicalMaterialBundleMaxBytes, model->size() - offset);
    const auto bytes = model->read(offset, length);
    result.source.modelBytes.insert(result.source.modelBytes.end(), bytes.begin(), bytes.end());
    offset += length;
  }
  const auto locations = inspectNativeOnnxExternalLocations(result.source.modelBytes, control);
  std::uint64_t total = model->size();
  std::uint64_t offset = 0;
  std::vector<std::shared_ptr<const SourceFile>> files;
  for (const auto& location : locations) {
    requireActive(control);
    const auto file = openFileAt(root.get(), components(location, false), control);
    if (file->size() > control.maxSourceBytes - total) failSource("SOURCE_LIMIT");
    total += file->size();
    const auto digest = fileDigest(file, control);
    result.source.externalFileRanges.emplace(location,
      NativeCanonicalSource::ExternalFileRange{offset, file->size()});
    result.externalFiles.push_back({location, file->size(), digest});
    files.push_back(file);
    offset += file->size();
  }
  if (!files.empty()) {
    result.source.initializerRangeSource = std::make_shared<FileRanges>(std::move(files));
  }
  result.sourceDigest = automaticOnnxSourceDigest(result.modelDigest, result.externalFiles, control);
  requireActive(control);
  return result;
}

} // namespace ndnsf::di
