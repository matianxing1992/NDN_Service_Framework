#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactStaging.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedArtifactStore.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <openssl/crypto.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ndnsf::di {

namespace {

void
reportArtifactCleanupFailure(const char* phase) noexcept
{
  try {
    logRuntimeEvidence(std::string("NDNSF_DI_PROVIDER_ARTIFACT_CLEANUP_FAILED phase=") +
                       (phase == nullptr ? "unknown" : phase));
  }
  catch (...) { /* Preserve noexcept cleanup. */ }
}

void
logAssemblyProgressLine(const NativeSelectionProjectionV3& projection,
                        const char* phase,
                        double progress,
                        std::uint64_t bytes = 0,
                        const char* detail = nullptr) noexcept
{
  try {
    std::ostringstream record;
    record << "NDNSF_DI_ASSEMBLY_PROGRESS phase=" << (phase ? phase : "unknown")
           << " progress=" << std::fixed << std::setprecision(3) << progress
           << " requestId=" << projection.requestId << " role=" << projection.assembly.selectedRole
           << " bytes=" << bytes << " detail=" << (detail ? detail : "");
    logRuntimeEvidence(record.str());
  }
  catch (...) { /* Preserve noexcept progress reporting. */ }
}

void
logAssemblyFailureLine(const NativeSelectionProjectionV3& projection) noexcept
{
  try {
    throw;
  }
  catch (const std::exception& error) {
    try {
      logRuntimeEvidence("NDNSF_DI_ASSEMBLY_FAILURE requestId=" + projection.requestId +
        " role=" + projection.assembly.selectedRole + " error=" + error.what());
    }
    catch (...) { /* Never replace the original preparation exception. */ }
  }
  catch (...) {
    try {
      logRuntimeEvidence("NDNSF_DI_ASSEMBLY_FAILURE requestId=" + projection.requestId +
        " role=" + projection.assembly.selectedRole + " error=non-standard-exception");
    }
    catch (...) { /* Preserve noexcept diagnostics. */ }
  }
}

constexpr std::uint64_t MaxAssemblyMetadataBytes = 65536;
constexpr std::uint64_t MaxMaterialJsonDepth = 8;
constexpr std::uint64_t MaxMaterialJsonTokens = 1U << 20;
constexpr std::uint64_t MaxMaterialJsonStringBytes = 4096;
constexpr std::uint64_t MaxMaterialReceiptObjects = 65536;
constexpr std::uint64_t MaxInlineMaterialRootBytes = 4096;
constexpr std::uint64_t MaterialManifestParseMultiplier = 8;
constexpr std::uint64_t MaterialReceiptParseMultiplier = 16;

// Final artifact directories are content-addressed but may be reached by
// distinct request/cache keys.  Serialize the short finalization window so a
// failed creator cannot remove a directory while another creator is writing
// the same immutable model.
std::mutex nativeAssemblyFinalizationMutex;

void
validateBoundedMaterialJson(const std::vector<std::uint8_t>& bytes,
                            const char* error)
{
  if (bytes.empty())
    throw std::runtime_error(error);
  std::uint64_t tokens = 0;
  std::uint64_t strings = 0;
  std::uint64_t depth = 0;
  std::uint64_t stringLength = 0;
  bool inString = false;
  bool escaped = false;
  for (const auto byte : bytes) {
    const auto ch = static_cast<char>(byte);
    if (inString) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        continue;
      }
      if (ch == '"') {
        inString = false;
        continue;
      }
      if (++stringLength > MaxMaterialJsonStringBytes)
        throw std::runtime_error(error);
      continue;
    }
    if (ch == '"') {
      inString = true;
      stringLength = 0;
      if (++strings > MaxMaterialJsonTokens)
        throw std::runtime_error(error);
    }
    else if (ch == '{' || ch == '[') {
      if (++depth > MaxMaterialJsonDepth || ++tokens > MaxMaterialJsonTokens)
        throw std::runtime_error(error);
    }
    else if (ch == '}' || ch == ']') {
      if (depth == 0 || ++tokens > MaxMaterialJsonTokens)
        throw std::runtime_error(error);
      --depth;
    }
    else if (ch == ',' || ch == ':') {
      if (++tokens > MaxMaterialJsonTokens)
        throw std::runtime_error(error);
    }
  }
  if (inString || escaped || depth != 0)
    throw std::runtime_error(error);
}

bool
isUnsignedDecimal(const std::string& value)
{
  return !value.empty() && std::all_of(value.begin(), value.end(), [] (const char ch) {
    return ch >= '0' && ch <= '9';
  });
}

bool
isSha256Digest(const std::string& value)
{
  if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0)
    return false;
  return std::all_of(value.begin() + 7, value.end(), [] (const unsigned char ch) {
    return std::isxdigit(ch) != 0;
  });
}

void
validateMaterialObjectRecords(const boost::property_tree::ptree& objects,
                              const char* error)
{
  if (objects.size() > MaxMaterialReceiptObjects)
    throw std::runtime_error(error);
  for (const auto& item : objects) {
    if (!item.first.empty() || item.second.size() > 7)
      throw std::runtime_error(error);
    std::set<std::string> seenKeys;
    bool hasPayloadId = false;
    bool hasDataName = false;
    bool hasDigest = false;
    bool hasBytes = false;
    for (const auto& field : item.second) {
      if (!field.second.empty())
        throw std::runtime_error(error);
      const auto& key = field.first;
      const auto& value = field.second.data();
      if (!seenKeys.insert(key).second)
        throw std::runtime_error(error);
      if (key == "payloadId") {
        hasPayloadId = true;
        if (value.empty() || value.size() > MaxMaterialJsonStringBytes) throw std::runtime_error(error);
      }
      else if (key == "dataName") {
        hasDataName = true;
        if (value.empty() || value.size() > MaxMaterialJsonStringBytes) throw std::runtime_error(error);
      }
      else if (key == "digest" || key == "bundleDigest") {
        if (value.empty() || value.size() > 128) throw std::runtime_error(error);
        if (key == "digest") hasDigest = true;
      }
      else if (key == "bytes" || key == "bundleBytes" || key == "bundleOffset") {
        if (!isUnsignedDecimal(value) || value.size() > 20) throw std::runtime_error(error);
        if (key == "bytes") hasBytes = true;
      }
      else {
        throw std::runtime_error(error);
      }
    }
    if (!hasPayloadId || !hasDataName || !hasDigest || !hasBytes)
      throw std::runtime_error(error);
  }
}

void
validateMaterialReceiptTree(const boost::property_tree::ptree& receipt,
                             const char* error)
{
  std::set<std::string> seenKeys;
  for (const auto& field : receipt) {
    if (!seenKeys.insert(field.first).second)
      throw std::runtime_error(error);
    if (field.first != "schema" && field.first != "sourceDigest" &&
        field.first != "graphDigest" && field.first != "materialIdentityDigest" &&
        field.first != "materialManifestDigest" && field.first != "materialObjects")
      throw std::runtime_error(error);
    if (field.first != "materialObjects" && !field.second.empty())
      throw std::runtime_error(error);
  }
  for (const auto& required : {"schema", "sourceDigest", "graphDigest",
                               "materialIdentityDigest", "materialManifestDigest",
                               "materialObjects"}) {
    if (seenKeys.count(required) == 0)
      throw std::runtime_error(error);
  }
  const auto objects = receipt.get_child_optional("materialObjects");
  if (!objects)
    throw std::runtime_error(error);
  validateMaterialObjectRecords(*objects, error);
}

std::uint64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string
sha256Hex(ndn::span<const std::uint8_t> bytes)
{
  ndn::util::Sha256 digest;
  digest.update(bytes);
  auto hex = digest.toString();
  // ndn-cxx formats this digest helper's hexadecimal text in uppercase, while the
  // cross-language assembly contract requires canonical lowercase SHA-256.
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return "sha256:" + hex;
}

std::string
sha256Hex(const std::vector<std::uint8_t>& bytes)
{
  return sha256Hex(ndn::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

std::vector<std::uint8_t>
readFile(const std::filesystem::path& path, std::uint64_t maxBytes)
{
  std::ifstream input(path, std::ios::binary);
  if (!input.good()) {
    throw std::runtime_error("cannot read native assembly file: " + path.string());
  }
  std::vector<std::uint8_t> bytes;
  std::array<char, 8192> chunk;
  while (input) {
    input.read(chunk.data(), chunk.size());
    const auto count = static_cast<std::size_t>(input.gcount());
    // Guard both operands before subtracting the unsigned limit.  A file that
    // grows while it is being read must fail closed instead of wrapping the
    // remaining budget.
    if (bytes.size() > maxBytes || count > maxBytes - bytes.size()) {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_FILE_TOO_LARGE: " + path.filename().string());
    }
    bytes.insert(bytes.end(), chunk.data(), chunk.data() + count);
  }
  if (!input.eof()) throw std::runtime_error("cannot read native assembly file: " + path.string());
  return bytes;
}

ndn::Buffer
readBuffer(const std::filesystem::path& path, std::uint64_t maxBytes)
{
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0 || size > maxBytes)
    throw std::runtime_error("DI_CACHE_COMPATIBILITY_FILE_INVALID");
  std::ifstream input(path, std::ios::binary);
  if (!input.good())
    throw std::runtime_error("DI_CACHE_COMPATIBILITY_FILE_UNREADABLE");
  ndn::Buffer bytes(static_cast<std::size_t>(size));
  if (!input.read(reinterpret_cast<char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size())))
    throw std::runtime_error("DI_CACHE_COMPATIBILITY_FILE_UNREADABLE");
  return bytes;
}

std::string
sha256File(const std::filesystem::path& path, std::uint64_t maxBytes)
{
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0 || size > maxBytes)
    throw std::runtime_error("DI_NATIVE_ASSEMBLY_CACHE_FILE_INVALID");
  std::ifstream input(path, std::ios::binary);
  if (!input.good())
    throw std::runtime_error("DI_NATIVE_ASSEMBLY_CACHE_FILE_UNREADABLE");
  ndn::util::Sha256 digest;
  std::array<std::uint8_t, 1U << 20> buffer{};
  std::uint64_t total = 0;
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()),
               static_cast<std::streamsize>(buffer.size()));
    const auto count = static_cast<std::size_t>(input.gcount());
    if (count == 0)
      break;
    // Check the chunk itself before subtracting from the unsigned limit;
    // otherwise a small configured limit can wrap maxBytes - count and allow
    // an oversized cache file to pass the bound check.
    if (count > maxBytes || total > maxBytes - count)
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_CACHE_FILE_TOO_LARGE");
    digest.update(ndn::span<const std::uint8_t>(buffer.data(), count));
    total += count;
  }
  if (!input.eof() || total != size)
    throw std::runtime_error("DI_NATIVE_ASSEMBLY_CACHE_FILE_UNREADABLE");
  auto hex = digest.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return "sha256:" + hex;
}

void
writeFileAtomic(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes)
{
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.parent_path() /
    (path.filename().string() + ".tmp-" + std::to_string(::getpid()));
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
      throw std::runtime_error("cannot write native assembly file: " +
                               temporary.string());
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output.good()) {
      throw std::runtime_error("cannot flush native assembly file: " +
                               temporary.string());
    }
  }
  std::filesystem::rename(temporary, path);
}

std::string
jsonEscape(const std::string& value)
{
  std::ostringstream output;
  output << '"';
  for (const auto ch : value) {
    switch (ch) {
      case '"': output << "\\\""; break;
      case '\\': output << "\\\\"; break;
      case '\b': output << "\\b"; break;
      case '\f': output << "\\f"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          output << "\\u00" << std::hex << std::uppercase
                 << static_cast<int>(static_cast<unsigned char>(ch))
                 << std::dec << std::nouppercase;
        }
        else {
          output << ch;
        }
    }
  }
  output << '"';
  return output.str();
}

boost::property_tree::ptree
readJson(const std::filesystem::path& path, std::uint64_t maxBytes = MaxAssemblyMetadataBytes)
{
  boost::property_tree::ptree root;
  const auto bytes = readFile(path, maxBytes);
  std::istringstream input(std::string(bytes.begin(), bytes.end()));
  boost::property_tree::read_json(input, root);
  return root;
}

std::string
firstString(const boost::property_tree::ptree& node,
            std::initializer_list<const char*> keys)
{
  for (const auto* key : keys) {
    const auto value = node.get_optional<std::string>(key);
    if (value && !value->empty()) {
      return *value;
    }
  }
  return {};
}

std::uint64_t
firstUint64(const boost::property_tree::ptree& node,
            std::initializer_list<const char*> keys)
{
  for (const auto* key : keys) {
    const auto value = node.get_optional<std::uint64_t>(key);
    if (value && *value != 0) {
      return *value;
    }
  }
  return 0;
}

void requireActiveAssembly(const NativeCanonicalOnnxAssemblerOptions& options,
                           std::uint64_t requestDeadlineMs)
{
  if (requestDeadlineMs != 0 && nowMs() >= requestDeadlineMs)
    throw std::runtime_error("DI_NATIVE_ASSEMBLY_TIMEOUT: request expired");
  if (options.shouldCancel && options.shouldCancel())
    throw std::runtime_error("DI_NATIVE_ASSEMBLY_CANCELLED");
  if (options.protectedRuntime)
    options.protectedRuntime->withContentKey(nowMs(), [] (const auto&) {});
}

class NativeColdAssemblyGate
{
public:
  NativeColdAssemblyGate(const NativeCanonicalOnnxAssemblerOptions& options,
                         const NativeSelectionProjectionV3& projection)
  {
    auto lockPath = options.coldAssemblyLockPath;
    if (lockPath.empty()) {
      const auto cachePath = options.cacheDir.empty()
        ? std::filesystem::temp_directory_path() / "ndnsf-di-native-artifacts"
        : std::filesystem::path(options.cacheDir);
      const auto absoluteCachePath = cachePath.is_absolute()
        ? cachePath : std::filesystem::absolute(cachePath);
      lockPath = absoluteCachePath.parent_path() / "cold-assembly.lock";
    }
    std::error_code error;
    if (!lockPath.parent_path().empty())
      std::filesystem::create_directories(lockPath.parent_path(), error);
    if (error) {
      throw std::runtime_error(
        "DI_NATIVE_COLD_ASSEMBLY_LOCK_DIRECTORY_FAILED: " + error.message());
    }
    m_fd = ::open(lockPath.c_str(),
                  O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (m_fd < 0) {
      throw std::runtime_error(
        "DI_NATIVE_COLD_ASSEMBLY_LOCK_OPEN_FAILED: " +
        std::string(std::strerror(errno)));
    }
    const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(options.assemblyTimeoutMs);
    auto nextWaitReport = std::chrono::steady_clock::now();
    bool reportedWait = false;
    try {
      while (::flock(m_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
          throw std::runtime_error(
            "DI_NATIVE_COLD_ASSEMBLY_LOCK_FAILED: " +
            std::string(std::strerror(errno)));
        }
        requireActiveAssembly(options, projection.deadlineMs);
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          throw std::runtime_error("DI_NATIVE_COLD_ASSEMBLY_ADMISSION_TIMEOUT");
        }
        if (!reportedWait || now >= nextWaitReport) {
          logRuntimeEvidence("NDNSF_DI_COLD_ASSEMBLY_GATE state=WAIT path=" + lockPath.string());
          if (options.reportProgress)
            options.reportProgress("COLD_ASSEMBLY_WAIT", 0.0);
          reportedWait = true;
          nextWaitReport = now + std::chrono::seconds(2);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      m_locked = true;
      logRuntimeEvidence("NDNSF_DI_COLD_ASSEMBLY_GATE state=ENTERED path=" + lockPath.string());
      if (options.reportProgress)
        options.reportProgress("COLD_ASSEMBLY_ENTERED", 0.0);
    }
    catch (...) {
      close();
      throw;
    }
  }

  NativeColdAssemblyGate(const NativeColdAssemblyGate&) = delete;
  NativeColdAssemblyGate& operator=(const NativeColdAssemblyGate&) = delete;

  ~NativeColdAssemblyGate() noexcept
  {
    close();
  }

private:
  void close() noexcept
  {
    if (m_fd < 0)
      return;
    if (m_locked)
      (void)::flock(m_fd, LOCK_UN);
    (void)::close(m_fd);
    m_fd = -1;
    m_locked = false;
  }

private:
  int m_fd = -1;
  bool m_locked = false;
};

/**
 * Keep the authenticated Selection-status view alive while the native OA02
 * worker performs a long synchronous assembly.  This is deliberately scoped
 * to the worker call: the reporter is request-owned, the thread has an
 * explicit join barrier, and a reporter failure is rethrown only after the
 * worker returns so the worker's first exception remains authoritative.
 */
class NativeAssemblyProgressHeartbeat
{
public:
  NativeAssemblyProgressHeartbeat(
    std::function<void(const std::string&, double)> reporter,
    double progress)
    : reporter_(std::move(reporter))
    , progress_(progress)
  {
    if (!reporter_)
      return;
    thread_ = std::thread([this] {
      std::unique_lock<std::mutex> lock(mutex_);
      while (!stopped_) {
        if (condition_.wait_for(lock, std::chrono::seconds(1),
                                [this] { return stopped_; })) {
          break;
        }
        lock.unlock();
        try {
          reporter_("ASSEMBLY_HEARTBEAT", progress_);
        }
        catch (...) {
          lock.lock();
          if (!error_)
            error_ = std::current_exception();
          stopped_ = true;
          lock.unlock();
          condition_.notify_all();
          break;
        }
        lock.lock();
      }
    });
  }

  ~NativeAssemblyProgressHeartbeat()
  {
    stop();
  }

  void stop()
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    condition_.notify_all();
    if (thread_.joinable())
      thread_.join();
  }

  void rethrowIfFailed()
  {
    std::exception_ptr error;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      error = error_;
    }
    if (error)
      std::rethrow_exception(error);
  }

private:
  std::function<void(const std::string&, double)> reporter_;
  double progress_ = 0.0;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stopped_ = false;
  std::exception_ptr error_;
  std::thread thread_;
};

std::filesystem::path
makeStagingDirectory(const std::filesystem::path& cacheDir)
{
  const auto base = cacheDir / ".staging";
  std::filesystem::create_directories(base);
  std::string pattern = (base / "assembly-XXXXXX").string();
  std::vector<char> mutablePattern(pattern.begin(), pattern.end());
  mutablePattern.push_back('\0');
  const auto created = ::mkdtemp(mutablePattern.data());
  if (created == nullptr) {
    throw std::runtime_error("cannot create native assembly staging directory");
  }
  const auto directory = std::filesystem::path(created);
  markNativeArtifactStagingLease(directory);
  return directory;
}

void
requireAssemblyDirectoryUnderCacheRoot(const std::filesystem::path& cacheDir,
                                       const std::filesystem::path& directory)
{
  std::error_code error;
  const auto cacheRoot = std::filesystem::canonical(cacheDir, error);
  if (error)
    throw std::runtime_error("DI_NATIVE_ASSEMBLY_CACHE_ROOT_INVALID");
  const auto physicalDirectory = std::filesystem::weakly_canonical(directory, error);
  if (error)
    throw std::runtime_error("DI_NATIVE_ASSEMBLY_CACHE_PATH_INVALID");
  const auto relative = physicalDirectory.lexically_relative(cacheRoot);
  if (relative.empty() || relative.is_absolute() || relative.begin() == relative.end() ||
      relative.begin()->string() == "..")
    throw std::runtime_error("DI_NATIVE_ASSEMBLY_CACHE_PATH_INVALID");
  if (std::filesystem::exists(directory)) {
    const auto existing = std::filesystem::canonical(directory, error);
    if (error || existing != physicalDirectory)
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_CACHE_PATH_INVALID");
  }
}

std::string
safeRole(std::string role)
{
  for (auto& ch : role) {
    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')) {
      ch = '_';
    }
  }
  return role.empty() ? "role" : role;
}

std::string
protectedAssembledCacheKey(const NativeSelectionProjectionV3& projection,
                           const std::string& roleAssemblySpecDigest,
                           const std::string& keyReferenceDigest)
{
  if (!isSha256Digest(projection.assembly.recipeDigest) ||
      !isSha256Digest(roleAssemblySpecDigest) ||
      !isSha256Digest(keyReferenceDigest)) {
    throw std::runtime_error("DI_PROTECTED_ASSEMBLED_CACHE_IDENTITY_INVALID");
  }
  const auto canonical = std::string("ndnsf-di-protected-assembled-cache-v1|") +
    projection.assembly.recipeDigest + "|" + roleAssemblySpecDigest + "|" +
    keyReferenceDigest;
  const auto bytes = std::vector<std::uint8_t>(canonical.begin(), canonical.end());
  return sha256Hex(bytes).substr(7);
}

/**
 * Bind the runner's output metadata to the same runtime scope that the
 * production V3 role selector installs in ProviderRoleWorker.  The
 * assembler must not recover this scope from an ONNX output name: a named
 * tensor is the adapter identity, while the runtime scope also reflects the
 * authenticated multi-tensor/redistribution contract.  Keep this helper
 * shared by the cold assembly and assembled-cache paths so a cache hit cannot
 * regress the contract fixed for a cold build.
 */
void
bindNativeRunnerOutputScopes(const NativeSelectionProjectionV3& projection,
                             NativeModelRunnerSpec& spec)
{
  const auto roleSpec = roleSpecFromSelectionProjectionV3(
    projection, projection.provider);
  if (projection.dataflow.terminalResponseOwner) {
    spec.metadata["outputScope"] = "final-response";
    spec.metadata["final"] = "true";
    return;
  }
  // The role dataflow carries the authenticated bundle scope.  Keep every
  // concrete ONNX boundary tensor name in the encoded bundle: the consumer
  // role resolves its own authenticated concrete input names from that bundle.
  // A semantic edge ID is a transport identity, not a tensor rename; mapping
  // several concrete outputs to one alias would lose tensor identity.
  if (roleSpec.outputs.empty())
    return;
  std::set<std::string> passthrough;
  for (const auto& edge : roleSpec.outputs) {
    for (const auto& name : edge.bundleTensorNames) {
      const bool isInput = std::any_of(
        projection.assembly.expectedInputs.begin(),
        projection.assembly.expectedInputs.end(),
        [&name] (const auto& tensor) { return tensor.name == name; });
      const bool isState = std::find(
        projection.generationContract.stateInputNames.begin(),
        projection.generationContract.stateInputNames.end(), name) !=
        projection.generationContract.stateInputNames.end();
      if (isInput && !isState)
        passthrough.insert(name);
    }
  }
  if (!passthrough.empty()) {
    std::ostringstream names;
    for (const auto& name : passthrough) {
      if (names.tellp() != std::streampos(0)) names << ',';
      names << name;
    }
    spec.metadata["passthroughTensors"] = names.str();
  }
  if (roleSpec.outputs.size() == 1) {
    spec.metadata["outputScope"] = roleSpec.outputs.front().scope;
    return;
  }
  const auto& firstScope = roleSpec.outputs.front().scope;
  const bool oneBundleScope = std::all_of(
    roleSpec.outputs.begin(), roleSpec.outputs.end(), [&firstScope] (const auto& edge) {
      return edge.scope == firstScope;
    });
  if (oneBundleScope)
    spec.metadata["outputBundleScope"] = firstScope;
  for (std::size_t index = 0; index < roleSpec.outputs.size(); ++index)
    spec.metadata["outputScope." + std::to_string(index)] =
      roleSpec.outputs[index].scope;
}

} // namespace

void
withNativeArtifactDirectoryFinalization(const std::string& directory,
                                        const std::function<void()>& action)
{
  std::lock_guard<std::mutex> lock(nativeAssemblyFinalizationMutex);
  action();
}

std::optional<NativeModelRunnerSpec>
tryLoadNativeCanonicalOnnxRoleFromCache(
  const NativeSelectionProjectionV3& projection,
  const NativeCanonicalOnnxAssemblerOptions& options,
  const std::string& canonicalSourceName,
  const std::string& canonicalSourceDigest)
{
  if (options.cacheDir.empty() || options.providerIdentity.empty())
    return std::nullopt;
  const bool protectedRole = projection.assembly.protectionEpoch != "plaintext-v1";
  std::string keyReferenceDigest;
  if (protectedRole) {
    if (!options.protectedRuntime ||
        (options.protectedRuntime->state() != ProtectedRuntimeState::GrantVerified &&
         options.protectedRuntime->state() != ProtectedRuntimeState::HostPlaintextLeased &&
         options.protectedRuntime->state() != ProtectedRuntimeState::DevicePlaintextLeased)) {
      throw std::runtime_error(
        "DI_PROTECTED_GRANT_UNAVAILABLE: assembled cache requires an authorized runtime");
    }
    if (options.roleAssemblySpecDigest.empty())
      throw std::runtime_error("DI_PROTECTED_ROLE_ASSEMBLY_SPEC_MISSING");
    const auto keyReference = options.protectedRuntime->keyReference();
    if (!keyReference)
      throw std::runtime_error("DI_PROTECTED_KEY_REFERENCE_UNAVAILABLE");
    keyReferenceDigest = keyReference->digest();
  }
  requireActiveAssembly(options, projection.deadlineMs);
  // Selection remains the authorization boundary. The recipe digest is known
  // before assembly and binds the model root, graph, role/layer node set,
  // tensor boundary, initializer, backend, and assembler profile. The output
  // model digest is only learned after assembly and is checked from the cache
  // manifest below; it cannot be the lookup key.
  static_cast<void>(canonicalSourceName);
  static_cast<void>(canonicalSourceDigest);
  if (!isSha256Digest(projection.assembly.recipeDigest))
    return std::nullopt;

  const auto cacheRoot = std::filesystem::path(options.cacheDir);
  const auto roleRoot = cacheRoot / (protectedRole ? "protected" : "assembled") / safeRole(
    projection.assembly.selectedRole);
  const auto directory = roleRoot /
    (protectedRole
       ? protectedAssembledCacheKey(projection, options.roleAssemblySpecDigest,
                                    keyReferenceDigest)
       : projection.assembly.recipeDigest.substr(7));
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error) || error)
    return std::nullopt;
  const auto discardCorruptEntry = [&] (const std::filesystem::path& entry) {
    std::error_code cleanupError;
    withNativeArtifactDirectoryFinalization(entry.string(), [&] {
      std::filesystem::remove_all(entry, cleanupError);
    });
  };

  try {
    const auto modelPath = directory /
      (protectedRole ? "model.onnx.cipher" : "model.onnx");
    const auto manifestPath = directory / "manifest.json";
    if (!std::filesystem::is_regular_file(modelPath) ||
        !std::filesystem::is_regular_file(manifestPath))
      return std::nullopt;
    if (protectedRole && !std::filesystem::is_regular_file(directory / "manifest.signature"))
      return std::nullopt;
    requireAssemblyDirectoryUnderCacheRoot(options.cacheDir, modelPath);
    requireAssemblyDirectoryUnderCacheRoot(options.cacheDir, manifestPath);
    const auto manifest = readJson(manifestPath);
    const auto assembledDigest = firstString(manifest, {"assembledModelDigest"});
    if (firstString(manifest, {"recipeDigest"}) != projection.assembly.recipeDigest ||
        firstString(manifest, {"modelManifestDigest"}) != projection.assembly.modelManifestDigest ||
        firstString(manifest, {"artifactProfileDigest"}) != projection.assembly.artifactProfileDigest ||
        firstString(manifest, {"graphDigest"}) != projection.assembly.graphDigest ||
        firstString(manifest, {"canonicalInitializerDigest"}) !=
          projection.assembly.canonicalInitializerDigest ||
        firstString(manifest, {"role"}) != projection.assembly.selectedRole ||
        firstString(manifest, {"roleKind"}) != projection.assembly.roleKind ||
        firstString(manifest, {"adapterDescriptorDigest"}) !=
          projection.assembly.adapterDescriptorDigest ||
        firstString(manifest, {"assemblerDescriptorDigest"}) !=
          projection.assembly.assemblerDescriptorDigest ||
        firstString(manifest, {"backendAbi"}) != projection.assembly.backendAbi ||
        firstString(manifest, {"precision"}) != projection.assembly.precision ||
        firstString(manifest, {"quantization"}) != projection.assembly.quantization ||
        firstString(manifest, {"layout"}) != projection.assembly.layout ||
        firstString(manifest, {"padding"}) != projection.assembly.padding ||
        (protectedRole && firstString(manifest, {"keyReferenceDigest"}) !=
           keyReferenceDigest) ||
        !isSha256Digest(assembledDigest) ||
        (!protectedRole && options.verifyCachedArtifactDigest &&
          sha256File(modelPath, projection.assembly.maxAssembledBytes) != assembledDigest)) {
      discardCorruptEntry(directory);
      return std::nullopt;
    }
    requireActiveAssembly(options, projection.deadlineMs);

    NativeModelRunnerSpec spec;
    spec.role = projection.assembly.selectedRole;
    spec.kind = "onnx";
    spec.backend = projection.assembly.backend;
    // Protected hits expose only the durable ciphertext descriptor.  Provider
    // performs the current-runtime/AAD authentication and writes a fresh
    // request-scoped plaintext staging file after this lookup succeeds.
    spec.path = protectedRole ? std::string{} : modelPath.string();
    spec.metadata = {
        {"artifactDigest", projection.assembly.artifactDigest},
        {"fragmentDigest", projection.assembly.artifactDigest},
        {"recipeDigest", projection.assembly.recipeDigest},
        {"modelManifestDigest", projection.assembly.modelManifestDigest},
        {"artifactProfileDigest", projection.assembly.artifactProfileDigest},
        {"graphDigest", projection.assembly.graphDigest},
        {"canonicalInitializerDigest", projection.assembly.canonicalInitializerDigest},
        {"adapterDescriptorDigest", projection.assembly.adapterDescriptorDigest},
        {"assemblerDescriptorDigest", projection.assembly.assemblerDescriptorDigest},
        {"backendAbi", projection.assembly.backendAbi},
        {"precision", projection.assembly.precision},
        {"quantization", projection.assembly.quantization},
        {"layout", projection.assembly.layout},
        {"padding", projection.assembly.padding},
        {"maxSourceBytes", std::to_string(projection.assembly.maxSourceBytes)},
      {"maxAssembledBytes", std::to_string(projection.assembly.maxAssembledBytes)},
      {"maxNodes", std::to_string(projection.assembly.maxNodes)},
      {"assembledModelDigest", assembledDigest},
      {"assembledFrom", protectedRole
        ? "protected-assembled-cache" : "canonical-root-post-selection-cache"},
    };
    if (!protectedRole)
      spec.metadata["assembledCachePath"] = modelPath.string();
    if (protectedRole) {
      spec.metadata["protectedCacheHit"] = "true";
      spec.metadata["protectedArtifactPersistent"] = "true";
      spec.metadata["protectedKeyReferenceDigest"] = keyReferenceDigest;
      spec.metadata["encryptedArtifactPath"] = modelPath.string();
      spec.metadata["encryptedArtifactDigest"] = sha256File(
        modelPath, projection.assembly.maxAssembledBytes + 65536);
    }
    if (projection.generationContract.enabled) {
        const auto& generation = projection.generationContract;
        if (!generation.stateSuccessorMap.empty())
          spec.metadata["stateSuccessorMap"] = generation.stateSuccessorMap;
        if (!generation.positionInputPolicy.empty())
          spec.metadata["positionInputPolicy"] = generation.positionInputPolicy;
        if (!generation.attentionMaskInputName.empty())
          spec.metadata["attentionMaskInputName"] = generation.attentionMaskInputName;
        if (!generation.positionIdsInputName.empty())
          spec.metadata["positionIdsInputName"] = generation.positionIdsInputName;
        if (!generation.cachePositionInputName.empty())
          spec.metadata["cachePositionInputName"] = generation.cachePositionInputName;
    }
    bindNativeRunnerOutputScopes(projection, spec);
    if (options.reportProgress)
      options.reportProgress("CACHE_HIT", 0.90);
    return spec;
  }
  catch (const std::exception&) {
    // Cache is an optimization, not an authority. Ignore stale or partial
    // entries and let the authenticated cold path decide.
    if (std::filesystem::exists(directory))
      discardCorruptEntry(directory);
  }
  return std::nullopt;
}

void
materializeNativeCanonicalOnnxCacheHit(
  NativeModelRunnerSpec& spec,
  const NativeSelectionProjectionV3& projection,
  const NativeCanonicalOnnxAssemblerOptions& options)
{
  if (!spec.path.empty())
    return;
  const auto encryptedPath = spec.metadata.find("encryptedArtifactPath");
  if (encryptedPath == spec.metadata.end() || encryptedPath->second.empty())
    throw std::runtime_error("DI_NATIVE_ASSEMBLY_CACHE_HIT_PATH_MISSING");
  if (!options.protectedRuntime)
    throw std::runtime_error("DI_PROTECTED_RUNTIME_UNAVAILABLE_FOR_CACHE_HIT");
  const auto keyReference = options.protectedRuntime->keyReference();
  if (!keyReference)
    throw std::runtime_error("DI_PROTECTED_KEY_REFERENCE_UNAVAILABLE");

  const auto ciphertextPath = std::filesystem::path(encryptedPath->second);
  requireAssemblyDirectoryUnderCacheRoot(options.cacheDir, ciphertextPath);
  if (!std::filesystem::is_regular_file(ciphertextPath))
    throw std::runtime_error("DI_NATIVE_ASSEMBLY_CIPHERTEXT_UNAVAILABLE");

  const auto staging = makeStagingDirectory(std::filesystem::path(options.cacheDir));
  try {
    registerNativePlaintextDirectory(
      *options.protectedRuntime, staging,
      "cache-hit-" + staging.filename().string());
    const auto profile = std::string("\"ndnsf-di-provider-workdir-scratch-v1\"");
    const NativeAssembledEntryContext context{
      projection.assembly.modelManifestDigest,
      options.roleAssemblySpecDigest,
      sha256Hex(std::vector<std::uint8_t>(profile.begin(), profile.end())),
      "MODEL_PROTO",
      keyReference->digest()};
    const auto expectedDigest = spec.metadata.find("encryptedArtifactDigest");
    std::string actualDigest;
    options.protectedRuntime->withContentKey(nowMs(), [&] (const auto& key) {
      actualDigest = openNativeAssembledEntryToFile(
        key, ciphertextPath, staging / "model.onnx", context,
        projection.assembly.maxAssembledBytes,
        expectedDigest == spec.metadata.end() ? std::string{} : expectedDigest->second);
    });
    if (expectedDigest != spec.metadata.end() && actualDigest != expectedDigest->second)
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_CIPHERTEXT_DIGEST_MISMATCH");
    spec.metadata["encryptedArtifactPath"] = ciphertextPath.string();
    spec.path = (staging / "model.onnx").string();
  }
  catch (...) {
    try {
      options.protectedRuntime->cancel("native assembled cache-hit materialization failed");
    }
    catch (...) {
      // Preserve the materialization failure; ProtectedRuntime still owns the
      // best-effort zeroization boundary for the registered staging lease.
    }
    throw;
  }
}

namespace {

struct NativeAssemblyArtifactDirectoryOwner
{
  std::filesystem::path directory;

  ~NativeAssemblyArtifactDirectoryOwner() noexcept
  {
    try {
      std::error_code cleanupError;
      withNativeArtifactDirectoryFinalization(directory.string(), [&] {
        std::filesystem::remove_all(directory, cleanupError);
      });
      if (cleanupError)
        reportArtifactCleanupFailure("assembler-directory");
    }
    catch (...) {
      reportArtifactCleanupFailure("assembler-directory-exception");
    }
  }
};

} // namespace

std::function<void(const std::string& phase, double progress)>
makeNativeAssemblyProgressReporter(
  ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
  const NativeSelectionProjectionV3& projection,
  const std::string& adapterIdentity,
  std::uint64_t epoch,
  std::uint64_t initialSequence,
  std::shared_ptr<std::atomic<std::uint64_t>> sequenceState)
{
  const auto operationId = ctx.assignment().selectionDigest + ":" +
    projection.assembly.selectedRole + ":assembly-progress";
  const auto role = projection.assembly.selectedRole;
  const auto requestId = projection.requestId;
  const auto attempt = projection.attempt;
  const auto planDigest = projection.planDigest;
  const auto sequence = sequenceState
    ? std::move(sequenceState)
    : std::make_shared<std::atomic<std::uint64_t>>(initialSequence);
  return [&ctx, projection, operationId, role, requestId, attempt, planDigest,
          adapterIdentity, epoch, sequence](const std::string& phase, double progress) {
    if (phase.empty() || phase.size() > 128 || progress < 0.0 || progress > 1.0) {
      throw std::invalid_argument("invalid native assembly progress");
    }
    ndn_service_framework::ServiceProvider::ServiceOperationStatus status;
    status.operationId = operationId;
    status.operation = "ensure-deployment";
    status.role = role;
    status.attempt = attempt == 0 ? 1 : attempt;
    status.epoch = epoch;
    status.sequence = sequence->fetch_add(1, std::memory_order_relaxed) + 1;
    status.state = "RUNNING";
    status.progressKnown = true;
    status.progress = progress;
    const auto now = static_cast<std::uint64_t>(std::max<std::int64_t>(0,
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()));
    status.createdAtMs = now;
    status.updatedAtMs = now;
    status.expiresAtMs = now + 120000;
    status.detailsSchema = "ndnsf-di-preparation-progress-v1";
    const auto details = std::string("{\"phase\":\"") + phase +
      "\",\"planDigest\":\"" + planDigest +
      "\",\"adapter\":\"" + adapterIdentity + "\"}";
    status.detailsPayload = ndn::Buffer(
      reinterpret_cast<const std::uint8_t*>(details.data()), details.size());
    logAssemblyProgressLine(projection, phase.c_str(), progress);
    ctx.reportOperationStatus(std::move(status));
  };
}

NativeModelRunnerSpec
prepareNativeCanonicalOnnxRole(
  const NativeCanonicalOnnxFetchers& fetchers,
  const NativeSelectionProjectionV3& projection,
  const NativeCanonicalOnnxAssemblerOptions& options)
{
  if (options.assemblyTimeoutMs == 0 || options.assemblyTimeoutMs > 3600000 ||
      projection.assembly.maxSourceBytes == 0 || projection.assembly.maxAssembledBytes == 0 ||
      projection.assembly.maxAssembledBytes > std::numeric_limits<std::uint64_t>::max() - MaxAssemblyMetadataBytes)
    throw std::runtime_error("DI_NATIVE_ASSEMBLY_LIMITS_INVALID");
  requireActiveAssembly(options, projection.deadlineMs);
  if (options.providerIdentity.empty() || !options.signManifest) {
    throw std::runtime_error("DI_PROVIDER_ASSEMBLY_SIGNER_MISSING");
  }
  if (options.workerLocation.path.empty()) {
    throw std::runtime_error("DI_PROVIDER_ASSEMBLY_WORKER_LOCATION_MISSING");
  }
  // The caller reaches this function only after the recipe-addressed cache
  // lookup missed (or when the cache is intentionally disabled). Serialize
  // the model-sized cold working set across Provider processes, while leaving
  // cache hits entirely outside the gate.
  NativeColdAssemblyGate coldAssemblyGate(options, projection);
  const bool protectedRole = projection.assembly.protectionEpoch != "plaintext-v1";
  if (protectedRole) {
    if (!options.protectedRuntime || options.roleAssemblySpecDigest.empty()) {
      throw std::runtime_error("DI_PROTECTED_GRANT_UNAVAILABLE: assembly runtime is missing");
    }
    options.protectedRuntime->withContentKey(nowMs(), [] (const auto&) {});
  }
  const bool cacheCompatibilityMode = !options.cacheCompatibilitySourceDir.empty();
  // Cache compatibility replaces only the root/source/initializer transport
  // reads with hash-verified local objects.  It does not bypass authenticated
  // Selection or the protected-role lease above: protected assembly still
  // runs under protectedRuntime and its plaintext eraser.
  if ((!cacheCompatibilityMode &&
       (!fetchers.getArtifact || !fetchers.fetchEncryptedLargeData)) ||
      (cacheCompatibilityMode && !fetchers.fetchEncryptedLargeData)) {
    throw std::runtime_error("DI_PROVIDER_ASSEMBLY_FETCHERS_MISSING");
  }
  if (projection.canonicalArtifactName.empty()) {
    throw std::runtime_error("DI_PROVIDER_ASSEMBLY_ROOT_MISSING");
  }
  const auto logMaterialFetch = [&projection] (const char* kind,
                                                const ndn::Name& name,
                                                const char* status,
                                                const std::optional<ndn::Buffer>* payload = nullptr,
                                                const std::string& expectedDigest = {}) {
    std::ostringstream record;
    record << "NDNSF_DI_PROVIDER_MATERIAL_FETCH"
           << " requestId=" << projection.requestId
           << " attemptEpoch=" << projection.attempt
           << " provider=" << projection.provider
           << " role=" << projection.selectedRole.selectedRole
           << " planDigest=" << projection.planDigest
           << " kind=" << kind
           << " name=" << name.toUri()
           << " status=" << status;
    if (!expectedDigest.empty()) {
      record << " expectedDigest=" << expectedDigest;
    }
    if (payload != nullptr && *payload) {
      record << " bytes=" << (*payload)->size();
      if (!(*payload)->empty()) {
        record << " digest=" << sha256Hex(
          ndn::span<const std::uint8_t>((*payload)->data(), (*payload)->size()));
      }
    }
    logRuntimeEvidence(record.str());
  };
  const auto logMaterialFetchRange = [&projection] (const char* kind,
                                                    const ndn::Name& name,
                                                    const char* status,
                                                    std::uint64_t bytes,
                                                    const std::string& digest,
                                                    const std::string& expectedDigest) {
    std::ostringstream record;
    record << "NDNSF_DI_PROVIDER_MATERIAL_FETCH"
           << " requestId=" << projection.requestId
           << " attemptEpoch=" << projection.attempt
           << " provider=" << projection.provider
           << " role=" << projection.selectedRole.selectedRole
           << " planDigest=" << projection.planDigest
           << " kind=" << kind
           << " name=" << name.toUri()
           << " status=" << status;
    if (!expectedDigest.empty()) {
      record << " expectedDigest=" << expectedDigest;
    }
    if (bytes != 0) {
      record << " bytes=" << bytes;
      if (!digest.empty()) {
        record << " digest=" << digest;
      }
    }
    logRuntimeEvidence(record.str());
  };
  const auto reportProgress = [&options, &projection] (const char* phase,
                                                        double progress) {
    requireActiveAssembly(options, projection.deadlineMs);
    if (options.reportProgress) {
      options.reportProgress(phase, progress);
    }
  };
  const ndn::Name rootName(projection.canonicalArtifactName);
  std::filesystem::path cacheSourcePath;
  std::filesystem::path cacheInitializerPath;
  std::string cacheSourceDigest;
  std::string cacheInitializerDigest;
  std::string cacheSourceName;
  boost::property_tree::ptree cacheRoot;
  reportProgress("ASSEMBLY_STARTED", 0.0);
  std::optional<ndn::Buffer> rootPayload;
  std::uint64_t rootPayloadBytes = 0;
  if (cacheCompatibilityMode) {
    const auto cacheRootDir = options.cacheCompatibilitySourceDir;
    std::error_code cacheError;
    if (!cacheRootDir.is_absolute() ||
        !std::filesystem::is_directory(cacheRootDir, cacheError) || cacheError) {
      throw std::runtime_error("DI_CACHE_COMPATIBILITY_SOURCE_DIR_INVALID");
    }
    const auto identity = readJson(cacheRootDir / "cache-identity.json", 64 * 1024);
    const auto validDigest = [] (const std::string& value) {
      return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
        std::all_of(value.begin() + 7, value.end(), [] (const char ch) {
          return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
        });
    };
    // The cache marker keeps the external source model name (for example,
    // Qwen/Qwen3-0.6B), while the sealed execution plan uses its control-plane
    // URI (for example, /Model/Qwen3/0.6B).  They are different naming
    // domains.  The graph/source/initializer digests below are the material
    // identity and remain bound to the authenticated Selection contract.
    if (identity.get<std::string>("schema", "") !=
          "ndnsf-di-model-source-cache-v1" ||
        identity.get<std::string>("modelName", "").empty() ||
        identity.get<std::string>("graphDigest", "") != projection.assembly.graphDigest) {
      throw std::runtime_error("DI_CACHE_COMPATIBILITY_IDENTITY_MISMATCH");
    }
    // The cache marker's modelManifestDigest is the preparation-stage
    // manifest identity. bindPublishedRoles deliberately rewrites the
    // selected role's modelManifestDigest to the authenticated publication
    // receipt digest, so those two digest domains must not be compared.
    const auto cacheModelManifestDigest = identity.get<std::string>(
      "modelManifestDigest", "");
    if (!validDigest(cacheModelManifestDigest))
      throw std::runtime_error("DI_CACHE_COMPATIBILITY_DIGEST_INVALID");
    cacheSourceDigest = identity.get<std::string>("sourceDigest", "");
    cacheInitializerDigest = identity.get<std::string>("initializerDigest", "");
    if (!validDigest(cacheSourceDigest) || !validDigest(cacheInitializerDigest))
      throw std::runtime_error("DI_CACHE_COMPATIBILITY_DIGEST_INVALID");
    const auto objectPath = [&] (const std::string& digest) {
      const auto hex = digest.substr(7);
      const auto path = cacheRootDir / "payloads" / "sha256" /
        hex.substr(0, 2) / hex;
      if (!std::filesystem::is_regular_file(path, cacheError) || cacheError ||
          sha256File(path, projection.assembly.maxSourceBytes) != digest)
        throw std::runtime_error("DI_CACHE_COMPATIBILITY_OBJECT_INVALID");
      return path;
    };
    cacheSourcePath = objectPath(cacheSourceDigest);
    cacheInitializerPath = objectPath(cacheInitializerDigest);
    cacheSourceName = "/cache-compatible/" + cacheRootDir.filename().string() + "/source";
    cacheRoot.put("schema", "ndnsf-di-canonical-model-manifest-v1");
    cacheRoot.put("state", "ACTIVE");
    cacheRoot.put("artifactProfileDigest", projection.assembly.artifactProfileDigest);
    cacheRoot.put("modelIdentityDigest", projection.assembly.modelManifestDigest);
    cacheRoot.put("modelName", projection.plan.modelName);
    boost::property_tree::ptree metadata;
    metadata.put("canonicalSourceDataName", cacheSourceName);
    metadata.put("canonicalSourceDigest", cacheSourceDigest);
    metadata.put("canonicalSourceBytes",
                 std::filesystem::file_size(cacheSourcePath));
    metadata.put("canonicalInitializerDataName", cacheSourceName + "/initializer");
    metadata.put("canonicalInitializerObjectDigest", cacheInitializerDigest);
    metadata.put("canonicalInitializerBytes",
                 std::filesystem::file_size(cacheInitializerPath));
    cacheRoot.add_child("metadata", metadata);
    logRuntimeEvidence(
      "NDNSF_DI_CACHE_COMPATIBILITY_MODE enabled=true rootFetch=skipped repoFetch=skipped "
      "sourceDir=" + cacheRootDir.string() + " sourceDigest=" + cacheSourceDigest +
      " initializerDigest=" + cacheInitializerDigest);
    reportProgress("ROOT_VERIFIED", 0.05);
  }
  else {
    logMaterialFetch("root", rootName, "begin", nullptr,
                     projection.assembly.modelManifestDigest);
    try {
      rootPayload = fetchers.getArtifact(rootName);
    }
    catch (...) {
      logMaterialFetch("root", rootName, "error", nullptr,
                       projection.assembly.modelManifestDigest);
      throw;
    }
    logMaterialFetch("root", rootName,
                     rootPayload && !rootPayload->empty() ? "returned" : "empty",
                     &rootPayload, projection.assembly.modelManifestDigest);
    if (!rootPayload || rootPayload->empty() || rootPayload->size() >
        projection.assembly.maxSourceBytes ||
        rootPayload->size() > MaxAssemblyMetadataBytes ||
        rootPayload->size() > projection.assembly.maxAssembledBytes) {
      throw std::runtime_error("DI_CANONICAL_ROOT_UNAVAILABLE");
    }
    if (sha256Hex(std::vector<std::uint8_t>(rootPayload->begin(), rootPayload->end())) !=
        projection.assembly.modelManifestDigest) {
      throw std::runtime_error("DI_CANONICAL_ROOT_DIGEST_MISMATCH");
    }
    validateBoundedMaterialJson(*rootPayload, "DI_CANONICAL_ROOT_SCHEMA_MISMATCH");
    rootPayloadBytes = rootPayload->size();
    logMaterialFetch("root", rootName, "verified", &rootPayload,
                     projection.assembly.modelManifestDigest);
    reportProgress("ROOT_VERIFIED", 0.05);
  }
  reportProgress("ROOT_VERIFIED", 0.05);

  const auto rootPath = makeStagingDirectory(
    std::filesystem::path(options.cacheDir));
  std::filesystem::path protectedArtifactDirectory;
  bool protectedArtifactOwned = false;
  std::filesystem::path finalArtifactDirectory;
  bool finalArtifactDirectoryOwned = false;
  NativePlaintextFileEraser eraseProtectedSourceFile;
  const auto storeWhileAuthorized = [&] (auto&& operation) {
    if (protectedRole) {
      // Cancellation uses the same mutex. Never recreate a staging path
      // after its lease has already been drained by another thread.
      options.protectedRuntime->withContentKey(nowMs(), [&] (const auto&) { operation(); });
    }
    else {
      requireActiveAssembly(options, projection.deadlineMs);
      operation();
    }
  };
  try {
    if (protectedRole) {
      eraseProtectedSourceFile = registerNativePlaintextDirectoryWithFileEraser(
        *options.protectedRuntime, rootPath,
        "assembly-" + rootPath.filename().string());
    }
    const auto rootFile = rootPath / "root.json";
    const auto sourceFile = rootPath / "canonical.onnx";
    NativeCanonicalSource canonicalSource;
    struct CanonicalSourceScrubber
    {
      NativeCanonicalSource& source;
      bool scrubbed = false;

      void scrub() noexcept
      {
        if (scrubbed) return;
        {
          NativePlaintextBufferGuard modelGuard{source.modelBytes};
          if (source.initializerBytes) {
            NativePlaintextBufferGuard initializerGuard{*source.initializerBytes};
          }
          for (auto& payload : source.materialPayloads)
            payload.scrub();
        }
        source = {};
        scrubbed = true;
      }

      ~CanonicalSourceScrubber()
      {
        scrub();
      }
    } sourceScrubber{canonicalSource};
    boost::property_tree::ptree root;
    if (cacheCompatibilityMode) {
      root = cacheRoot;
    }
    else {
      storeWhileAuthorized([&] {
        writeFileAtomic(rootFile,
                        std::vector<std::uint8_t>(rootPayload->begin(), rootPayload->end()));
      });
      root = readJson(rootFile, projection.assembly.maxSourceBytes);
      rootPayload.reset();
      if (root.get<std::string>("schema", "") !=
            "ndnsf-di-canonical-model-manifest-v1" ||
          root.get<std::string>("state", "") != "ACTIVE") {
        throw std::runtime_error("DI_CANONICAL_ROOT_SCHEMA_MISMATCH");
      }
    }
    const auto rootProfile = root.get<std::string>("artifactProfileDigest", "");
    if (rootProfile != projection.assembly.artifactProfileDigest) {
      throw std::runtime_error("DI_CANONICAL_ROOT_PROFILE_MISMATCH");
    }
    const auto metadata = root.get_child_optional("metadata");
    const auto sourceName = metadata
      ? firstString(*metadata, {"canonicalSourceDataName", "canonical_source_data_name",
                               "sourceDataName", "source_data_name"})
      : std::string();
    const auto sourceDigest = metadata
      ? firstString(*metadata, {"canonicalSourceDigest", "canonical_source_digest",
                               "sourceDigest", "source_digest"})
      : std::string();
    const auto expectedSourceBytes = metadata
      ? firstUint64(*metadata, {"canonicalSourceBytes", "canonical_source_bytes",
                               "sourceBytes", "source_bytes"})
      : 0;
    if (sourceDigest.empty() || expectedSourceBytes == 0) {
      throw std::runtime_error("DI_CANONICAL_SOURCE_METADATA_MISSING");
    }
    NativeAssemblyControl assemblyControl;
    const auto wallNow = nowMs();
    const auto remainingRequestMs = projection.deadlineMs == 0 ?
      options.assemblyTimeoutMs :
      (projection.deadlineMs > wallNow ? projection.deadlineMs - wallNow : 0);
    assemblyControl.deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(std::min<std::uint64_t>(
        options.assemblyTimeoutMs, remainingRequestMs));
    assemblyControl.maxSourceBytes = projection.assembly.maxSourceBytes;
    assemblyControl.maxAssembledBytes = projection.assembly.maxAssembledBytes;
    assemblyControl.requireActive = [&] {
      requireActiveAssembly(options, projection.deadlineMs);
    };
    const auto materialManifestName = metadata
      ? firstString(*metadata, {"materialManifestDataName", "material_manifest_data_name"})
      : std::string();
    const auto materialManifestDigest = metadata
      ? firstString(*metadata, {"materialManifestDigest", "material_manifest_digest"})
      : std::string();
    const auto materialManifestBytes = metadata
      ? firstUint64(*metadata, {"materialManifestBytes", "material_manifest_bytes"})
      : 0;
    const auto materialIdentityDigest = metadata
      ? firstString(*metadata, {"materialIdentityDigest", "material_identity_digest"})
      : std::string();
    const auto materialReceiptName = metadata
      ? firstString(*metadata, {"materialReceiptDataName", "material_receipt_data_name"})
      : std::string();
    const auto materialReceiptDigest = metadata
      ? firstString(*metadata, {"materialReceiptDigest", "material_receipt_digest"})
      : std::string();
    const auto materialReceiptBytes = metadata
      ? firstUint64(*metadata, {"materialReceiptBytes", "material_receipt_bytes"})
      : 0;
    const bool inlineMaterialObjects = metadata &&
      metadata->get_child_optional("materialObjects").has_value();
    const bool materialMetadataPresent = !materialManifestName.empty() ||
      !materialManifestDigest.empty() || !materialIdentityDigest.empty() ||
      materialManifestBytes != 0 ||
      inlineMaterialObjects || !materialReceiptName.empty() ||
      !materialReceiptDigest.empty() || materialReceiptBytes != 0;
    if (!materialMetadataPresent && sourceName.empty())
      throw std::runtime_error("DI_CANONICAL_SOURCE_NAME_MISSING");

    NativeCanonicalOnnxFetchers effectiveFetchers = fetchers;
    if (cacheCompatibilityMode) {
      const auto localSourceName = sourceName;
      const auto localInitializerName = firstString(
        metadata ? *metadata : boost::property_tree::ptree{},
        {"canonicalInitializerDataName", "canonical_initializer_data_name",
         "initializerDataName", "initializer_data_name"});
      effectiveFetchers.fetchEncryptedLargeData =
        [cacheSourcePath, cacheInitializerPath, localSourceName,
         localInitializerName, maxBytes = projection.assembly.maxSourceBytes]
        (const ndn::Name& name, const ndn::Name&) -> std::optional<ndn::Buffer> {
          const auto uri = name.toUri();
          if (uri == localSourceName)
            return readBuffer(cacheSourcePath, maxBytes);
          if (!localInitializerName.empty() && uri == localInitializerName)
            return readBuffer(cacheInitializerPath, maxBytes);
          return std::nullopt;
        };
    }

    const auto fetchPlainObject = [&] (const std::string& name,
                                       const std::string& digest,
                                       std::uint64_t expectedBytes,
                                       const char* kind) {
      assemblyControl.requireActive();
      const ndn::Name dataName(name);
      logAssemblyProgressLine(projection, "OBJECT_READ_BEGIN", 0.25,
                              expectedBytes, kind);
      logMaterialFetch(kind, dataName, "begin", nullptr, digest);
      std::optional<ndn::Buffer> fetched;
      try {
        fetched = effectiveFetchers.fetchEncryptedLargeData(
          dataName, ndn::Name(projection.plan.serviceName));
      }
      catch (...) {
        logMaterialFetch(kind, dataName, "error", nullptr, digest);
        throw;
      }
      logMaterialFetch(kind, dataName,
                       fetched && !fetched->empty() ? "returned" : "empty",
                       &fetched, digest);
      if (!fetched || fetched->empty() || fetched->size() != expectedBytes ||
          fetched->size() > projection.assembly.maxSourceBytes)
        throw std::runtime_error(std::string("DI_CANONICAL_") + kind + "_UNAVAILABLE");
      if (sha256Hex(*fetched) != digest)
        throw std::runtime_error(std::string("DI_CANONICAL_") + kind + "_DIGEST_MISMATCH");
      assemblyControl.requireActive();
      logMaterialFetch(kind, dataName, "verified", &fetched, digest);
      logAssemblyProgressLine(projection, "OBJECT_READ_DONE", 0.25,
                              static_cast<std::uint64_t>(fetched->size()), kind);
      return std::vector<std::uint8_t>(fetched->begin(), fetched->end());
    };

    if (materialMetadataPresent) {
      // Inline object indices live in the authenticated root itself. Reject
      // an oversized inline root before fetching the manifest or receipt;
      // producers must publish a bounded receipt instead.
      if (inlineMaterialObjects && rootPayloadBytes > MaxInlineMaterialRootBytes)
        throw std::runtime_error("DI_CANONICAL_INLINE_MATERIAL_METADATA_TOO_LARGE");
      if (materialManifestName.empty() || materialManifestDigest.empty() ||
          materialManifestBytes == 0 || materialIdentityDigest.empty() || !metadata ||
          (!inlineMaterialObjects && (materialReceiptName.empty() ||
                                      materialReceiptDigest.empty() ||
                                      materialReceiptBytes == 0)) ||
          (inlineMaterialObjects && (!materialReceiptName.empty() ||
                                     !materialReceiptDigest.empty() ||
                                     materialReceiptBytes != 0)))
        throw std::runtime_error("DI_CANONICAL_MATERIAL_METADATA_MISSING");
      if (materialManifestBytes > projection.assembly.maxSourceBytes ||
          materialManifestBytes > projection.assembly.maxAssembledBytes)
        throw std::runtime_error("DI_CANONICAL_MATERIAL_MANIFEST_UNAVAILABLE");
      const auto checkedMultiply = [] (const std::uint64_t value,
                                       const std::uint64_t multiplier) {
        if (multiplier != 0 && value > std::numeric_limits<std::uint64_t>::max() / multiplier)
          throw std::runtime_error("DI_CANONICAL_MATERIAL_METADATA_UNAVAILABLE");
        return value * multiplier;
      };
      const auto manifestParseReservation = checkedMultiply(
        materialManifestBytes, MaterialManifestParseMultiplier);
      const auto receiptParseReservation = checkedMultiply(
        materialReceiptBytes, MaterialReceiptParseMultiplier);
      if (receiptParseReservation > std::numeric_limits<std::uint64_t>::max() -
          manifestParseReservation)
        throw std::runtime_error("DI_CANONICAL_MATERIAL_METADATA_UNAVAILABLE");
      const auto parseReservation = manifestParseReservation + receiptParseReservation;
      if (parseReservation < materialManifestBytes ||
          parseReservation > projection.assembly.maxAssembledBytes)
        throw std::runtime_error("DI_CANONICAL_MATERIAL_METADATA_UNAVAILABLE");
      auto manifestBytes = fetchPlainObject(
        materialManifestName, materialManifestDigest,
        materialManifestBytes,
        "material-manifest");
      validateBoundedMaterialJson(manifestBytes, "DI_CANONICAL_MATERIAL_MANIFEST_INVALID");
      auto materialManifest = parseNativeCanonicalMaterialManifest(manifestBytes);
      if (materialManifest->manifestDigest != materialIdentityDigest ||
          materialManifest->sourceDigest != sourceDigest ||
          materialManifest->graphDigest != projection.assembly.graphDigest ||
          materialManifest->initializerDigest != projection.assembly.canonicalInitializerDigest)
        throw std::runtime_error("DI_CANONICAL_MATERIAL_IDENTITY_MISMATCH");
      // The parsed manifest is the authenticated owner from this point on;
      // release the transport copy before retaining selected payloads.
      manifestBytes.clear();
      manifestBytes.shrink_to_fit();
      reportProgress("MATERIAL_MANIFEST_VERIFIED", 0.15);

      boost::property_tree::ptree materialReceipt;
      const boost::property_tree::ptree* materialObjects = nullptr;
      std::uint64_t materialMetadataBytes = materialManifestBytes;
      if (inlineMaterialObjects) {
        materialObjects = &*metadata->get_child_optional("materialObjects");
        validateMaterialObjectRecords(*materialObjects,
                                      "DI_CANONICAL_MATERIAL_RECEIPT_INVALID");
      }
      else {
        if (materialReceiptBytes > projection.assembly.maxSourceBytes ||
            materialMetadataBytes > projection.assembly.maxAssembledBytes ||
            materialReceiptBytes > projection.assembly.maxAssembledBytes - materialMetadataBytes)
          throw std::runtime_error("DI_CANONICAL_MATERIAL_RECEIPT_UNAVAILABLE");
        auto receiptBytes = fetchPlainObject(
          materialReceiptName, materialReceiptDigest, materialReceiptBytes,
          "material-receipt");
        validateBoundedMaterialJson(receiptBytes, "DI_CANONICAL_MATERIAL_RECEIPT_INVALID");
        try {
          std::istringstream receiptInput(std::string(receiptBytes.begin(), receiptBytes.end()));
          boost::property_tree::read_json(receiptInput, materialReceipt);
        }
        catch (const std::exception&) {
          throw std::runtime_error("DI_CANONICAL_MATERIAL_RECEIPT_INVALID");
        }
        validateMaterialReceiptTree(materialReceipt,
                                    "DI_CANONICAL_MATERIAL_RECEIPT_INVALID");
        if (materialReceipt.get<std::string>("schema", "") !=
              "ndnsf-di-canonical-material-receipt-v1" ||
            materialReceipt.get<std::string>("sourceDigest", "") != sourceDigest ||
            materialReceipt.get<std::string>("graphDigest", "") != projection.assembly.graphDigest ||
            materialReceipt.get<std::string>("materialIdentityDigest", "") != materialIdentityDigest ||
            materialReceipt.get<std::string>("materialManifestDigest", "") != materialManifestDigest ||
            !materialReceipt.get_child_optional("materialObjects"))
          throw std::runtime_error("DI_CANONICAL_MATERIAL_RECEIPT_IDENTITY_MISMATCH");
        materialObjects = &*materialReceipt.get_child_optional("materialObjects");
        materialMetadataBytes += materialReceiptBytes;
        receiptBytes.clear();
        receiptBytes.shrink_to_fit();
      }
      reportProgress("MATERIAL_RECEIPT_VERIFIED", 0.25);

      struct MaterialObjectReceipt {
        std::string dataName;
        std::string digest;
        std::uint64_t bytes = 0;
        std::string bundleDigest;
        std::uint64_t bundleBytes = 0;
        std::uint64_t bundleOffset = 0;
      };
      std::map<std::string, MaterialObjectReceipt> objects;
      std::map<std::string, std::pair<std::string, std::uint64_t>> bundleIdentities;
      std::map<std::string, bool> bundleKinds;
      for (const auto& item : *materialObjects) {
        const auto id = item.second.get<std::string>("payloadId", "");
        const auto dataName = item.second.get<std::string>("dataName", "");
        const auto digest = item.second.get<std::string>("digest", "");
        const auto bytes = item.second.get<std::uint64_t>("bytes", 0);
        const auto bundleDigest = item.second.get<std::string>("bundleDigest", "");
        const auto bundleBytes = item.second.get<std::uint64_t>("bundleBytes", 0);
        const auto bundleOffset = item.second.get<std::uint64_t>("bundleOffset", 0);
        const bool bundleFieldsPresent = item.second.count("bundleDigest") != 0 ||
          item.second.count("bundleBytes") != 0 || item.second.count("bundleOffset") != 0;
        const bool hasBundle = bundleFieldsPresent;
        if (bundleFieldsPresent && (!item.second.count("bundleDigest") ||
                                    !item.second.count("bundleBytes") ||
                                    !item.second.count("bundleOffset") ||
                          bundleDigest.empty() || bundleBytes == 0 ||
                          bundleBytes > NativeCanonicalMaterialBundleMaxBytes))
          throw std::runtime_error("DI_CANONICAL_MATERIAL_BUNDLE_RECEIPT_INVALID");
        const auto kind = bundleKinds.find(dataName);
        if (kind != bundleKinds.end() && kind->second != hasBundle)
          throw std::runtime_error("DI_CANONICAL_MATERIAL_BUNDLE_KIND_MISMATCH");
        bundleKinds[dataName] = hasBundle;
        if (hasBundle) {
          const auto identity = bundleIdentities.emplace(
            dataName, std::make_pair(bundleDigest, bundleBytes));
          if (!identity.second && identity.first->second !=
              std::make_pair(bundleDigest, bundleBytes))
            throw std::runtime_error("DI_CANONICAL_MATERIAL_BUNDLE_IDENTITY_MISMATCH");
        }
        if (id.empty() || dataName.empty() || digest.empty() || bytes == 0 ||
            !objects.emplace(id, MaterialObjectReceipt{dataName, digest, bytes,
                                                       bundleDigest, bundleBytes,
                                                       bundleOffset}).second)
          throw std::runtime_error("DI_CANONICAL_MATERIAL_RECEIPT_INVALID");
      }

      std::set<std::string> selectedIds{materialManifest->templatePayloadId};
      std::set<std::string> selectedDependencies;
      for (const auto nodeIndex : projection.assembly.nodeIndices) {
        const auto logicalName = "node/" + std::to_string(nodeIndex);
        const auto found = std::find_if(materialManifest->references.begin(),
          materialManifest->references.end(), [&] (const auto& reference) {
            return reference.kind == "graph-node" && reference.logicalName == logicalName;
          });
        if (found == materialManifest->references.end())
          throw std::runtime_error("DI_CANONICAL_MATERIAL_NODE_MISSING");
        selectedIds.insert(found->payloadId);
        selectedIds.insert(found->chunkPayloadIds.begin(), found->chunkPayloadIds.end());
        selectedDependencies.insert(found->dependencies.begin(), found->dependencies.end());
      }
      for (const auto& dependency : selectedDependencies) {
        const auto found = std::find_if(materialManifest->references.begin(),
          materialManifest->references.end(), [&] (const auto& reference) {
            return reference.kind == "shared-initializer" &&
                   reference.logicalName == dependency;
          });
        if (found == materialManifest->references.end())
          throw std::runtime_error("DI_CANONICAL_MATERIAL_INITIALIZER_MISSING");
        selectedIds.insert(found->payloadId);
        selectedIds.insert(found->chunkPayloadIds.begin(), found->chunkPayloadIds.end());
      }
      std::map<std::string, MaterialObjectReceipt> selectedObjects;
      for (const auto& payloadId : selectedIds) {
        const auto object = objects.find(payloadId);
        if (object == objects.end())
          throw std::runtime_error("DI_CANONICAL_MATERIAL_RECEIPT_MISSING");
        selectedObjects.emplace(payloadId, object->second);
      }
      // The full receipt/index is no longer needed once selected identities
      // have been copied.  This prevents unselected object metadata from
      // overlapping the chunk buffers and model assembly working set.
      objects.clear();
      bundleIdentities.clear();
      bundleKinds.clear();
      materialReceipt.clear();
      // Charge the transient parse/index representation before fetching any
      // selected payload.  The full receipt is released below, but it must fit
      // the same working-set ceiling while it is being authenticated.
      std::uint64_t selectedMaterialBytes = std::max(materialMetadataBytes,
                                                     parseReservation);
      // Keep one authenticated bundle allocation and expose selected chunks as
      // shared ranges.  Copying every 1 MiB chunk into materialPayloads would
      // retain the bundle cache and a second full initializer at the same time.
      std::map<std::string, std::shared_ptr<std::vector<std::uint8_t>>> fetchedBundles;
      std::set<std::string> countedBundles;
      std::size_t selectedMaterialIndex = 0;
      for (const auto& payloadId : selectedIds) {
        assemblyControl.requireActive();
        const auto object = selectedObjects.find(payloadId);
        const auto reference = std::find_if(materialManifest->references.begin(),
          materialManifest->references.end(), [&] (const auto& candidate) {
            return candidate.payloadId == payloadId ||
              std::find(candidate.chunkPayloadIds.begin(), candidate.chunkPayloadIds.end(),
                        payloadId) != candidate.chunkPayloadIds.end();
          });
        if (reference == materialManifest->references.end())
          throw std::runtime_error("DI_CANONICAL_MATERIAL_REFERENCE_MISSING");
        const bool chunkPayload = reference->payloadId != payloadId;
        if ((!chunkPayload && (reference->digest != object->second.digest ||
                               reference->bytes != object->second.bytes)) ||
            (chunkPayload && object->second.bytes > NativeCanonicalMaterialBundleMaxBytes))
          throw std::runtime_error("DI_CANONICAL_MATERIAL_RECEIPT_MISMATCH");
        std::vector<std::uint8_t> bytes;
        std::shared_ptr<const std::vector<std::uint8_t>> backing;
        std::size_t backingOffset = 0;
        std::size_t backingSize = 0;
        if (!object->second.bundleDigest.empty()) {
          if (countedBundles.find(object->second.dataName) == countedBundles.end()) {
            if (object->second.bundleBytes > NativeCanonicalMaterialBundleMaxBytes ||
                selectedMaterialBytes > projection.assembly.maxAssembledBytes ||
                object->second.bundleBytes >
                  projection.assembly.maxAssembledBytes - selectedMaterialBytes)
              throw std::runtime_error("DI_CANONICAL_MATERIAL_BUDGET_EXCEEDED");
            selectedMaterialBytes += object->second.bundleBytes;
            countedBundles.insert(object->second.dataName);
          }
          auto bundle = fetchedBundles.find(object->second.dataName);
          if (bundle == fetchedBundles.end()) {
            auto fetched = fetchPlainObject(object->second.dataName,
                                            object->second.bundleDigest,
                                            object->second.bundleBytes,
                                            "material-bundle");
            auto owned = std::make_shared<std::vector<std::uint8_t>>(std::move(fetched));
            bundle = fetchedBundles.emplace(object->second.dataName, std::move(owned)).first;
          }
          const auto& bundleBytes = bundle->second;
          if (object->second.bundleOffset > bundleBytes->size() ||
              object->second.bytes > bundleBytes->size() - object->second.bundleOffset)
            throw std::runtime_error("DI_CANONICAL_MATERIAL_BUNDLE_RANGE_INVALID");
          backing = bundle->second;
          backingOffset = static_cast<std::size_t>(object->second.bundleOffset);
          backingSize = static_cast<std::size_t>(object->second.bytes);
          if (sha256Hex(ndn::span<const std::uint8_t>(
                bundleBytes->data() + backingOffset, backingSize)) != object->second.digest)
            throw std::runtime_error("DI_CANONICAL_MATERIAL_PAYLOAD_DIGEST_MISMATCH");
          logMaterialFetchRange("material-payload", ndn::Name(object->second.dataName),
                                "verified", backingSize, object->second.digest,
                                object->second.digest);
        }
        else {
          if (selectedMaterialBytes > projection.assembly.maxAssembledBytes ||
              object->second.bytes >
                projection.assembly.maxAssembledBytes - selectedMaterialBytes)
            throw std::runtime_error("DI_CANONICAL_MATERIAL_BUDGET_EXCEEDED");
          selectedMaterialBytes += object->second.bytes;
          bytes = fetchPlainObject(object->second.dataName, object->second.digest,
                                   object->second.bytes, "material-payload");
        }
        canonicalSource.materialPayloads.push_back({payloadId, object->second.digest,
                                                     std::move(bytes), std::move(backing),
                                                     backingOffset, backingSize});
        ++selectedMaterialIndex;
        const double materialProgress = selectedIds.empty() ? 0.55 :
          0.25 + 0.40 * static_cast<double>(selectedMaterialIndex) /
            static_cast<double>(selectedIds.size());
        reportProgress("MATERIAL_PAYLOAD_VERIFIED", materialProgress);
      }
      // The assembled source retains only selected slices; do not keep the
      // shared bundle buffers alive through worker execution.
      fetchedBundles.clear();
      selectedObjects.clear();
      canonicalSource.materialManifest = std::move(materialManifest);
      canonicalSource.materializedRole = true;
      canonicalSource.materializedNodeIndices = projection.assembly.nodeIndices;
      canonicalSource.materializedSourceDigest = sourceDigest;
      canonicalSource.materializedGraphDigest = projection.assembly.graphDigest;
      canonicalSource.materializedInitializerDigest =
        projection.assembly.canonicalInitializerDigest;
      canonicalSource.modelBytes = materializeNativeCanonicalModel(
        canonicalSource, projection.assembly.nodeIndices,
        projection.assembly.expectedInputs, projection.assembly.expectedOutputs,
        assemblyControl);
      // materializeNativeCanonicalModel has consumed the selected payloads
      // and copied the required initializer ranges into the role model.  Do
      // not retain those source slices while deriving the role identity or
      // writing/staging the model: that would overlap the model-sized
      // protobuf with a second model-sized material buffer.
      std::vector<NativeCanonicalSource::MaterialPayload>{}.swap(
        canonicalSource.materialPayloads);
      canonicalSource.materialManifest.reset();
      // materializeNativeCanonicalModel has authenticated and copied the
      // selected initializer payloads into the role model as ordinary
      // TensorProto data.  The full canonical initializer is therefore no
      // longer an input to the worker; retaining it here would keep the
      // complete 1.5 GiB source alive beside the materialized role and the
      // worker's protobuf/ORT copies.
      if (canonicalSource.initializerBytes) {
        auto& initializerBytes = canonicalSource.initializerBytes->asVector();
        if (!initializerBytes.empty())
          OPENSSL_cleanse(initializerBytes.data(), initializerBytes.size());
        std::vector<std::uint8_t>{}.swap(initializerBytes);
        canonicalSource.initializerBytes.reset();
      }
      storeWhileAuthorized([&] { writeFileAtomic(sourceFile, canonicalSource.modelBytes); });
      reportProgress("MODEL_MATERIALIZED", 0.75);
    }
    else {
    const ndn::Name sourceNameValue(sourceName);
    logAssemblyProgressLine(projection, "SOURCE_READ_BEGIN", 0.30,
                            expectedSourceBytes, "source");
    logMaterialFetch("source", sourceNameValue, "begin", nullptr, sourceDigest);
    std::optional<ndn::Buffer> source;
    try {
      source = effectiveFetchers.fetchEncryptedLargeData(
        sourceNameValue, ndn::Name(projection.plan.serviceName));
    }
    catch (...) {
      logMaterialFetch("source", sourceNameValue, "error", nullptr, sourceDigest);
      throw;
    }
    std::vector<std::uint8_t> emptyPayload;
    NativePlaintextBufferGuard sourcePayloadGuard{source ? *source : emptyPayload};
    logMaterialFetch("source", sourceNameValue,
                     source && !source->empty() ? "returned" : "empty", &source,
                     sourceDigest);
    if (!source || source->empty() || source->size() != expectedSourceBytes ||
        source->size() > projection.assembly.maxSourceBytes) {
      if (source && source->size() != expectedSourceBytes) {
        throw std::runtime_error("DI_CANONICAL_SOURCE_SIZE_MISMATCH");
      }
      throw std::runtime_error("DI_CANONICAL_SOURCE_UNAVAILABLE");
    }
    if (sha256Hex(*source) != sourceDigest) {
      throw std::runtime_error("DI_CANONICAL_SOURCE_DIGEST_MISMATCH");
    }
    logMaterialFetch("source", sourceNameValue, "verified", &source, sourceDigest);
    logAssemblyProgressLine(projection, "SOURCE_READ_DONE", 0.40,
                            static_cast<std::uint64_t>(source->size()), "source");
    reportProgress("SOURCE_VERIFIED", 0.40);
    // Transfer the fetched buffer into the canonical source instead of
    // retaining a second 1.5 GiB copy while the worker is running.
    canonicalSource.modelBytes = std::move(*source);
    storeWhileAuthorized([&] { writeFileAtomic(sourceFile, canonicalSource.modelBytes); });

    // External initializers are a second authenticated canonical object.  The
    // graph object alone is not sufficient for ONNX Runtime assembly; keep
    // this metadata optional for existing inline-ONNX roots, but require the
    // complete name/digest/size tuple whenever a root advertises it.
    const auto initializerName = metadata
      ? firstString(*metadata, {"canonicalInitializerDataName",
                                "canonical_initializer_data_name",
                                "initializerDataName", "initializer_data_name"})
      : std::string();
    const auto initializerDigest = metadata
      ? firstString(*metadata, {"canonicalInitializerObjectDigest",
                                "canonical_initializer_object_digest",
                                "initializerObjectDigest",
                                "initializer_object_digest"})
      : std::string();
    const auto expectedInitializerBytes = metadata
      ? firstUint64(*metadata, {"canonicalInitializerBytes",
                                "canonical_initializer_bytes",
                                "initializerBytes", "initializer_bytes"})
      : 0;
    const bool anyInitializerMetadata = !initializerName.empty() ||
      !initializerDigest.empty() || expectedInitializerBytes != 0;
    if (anyInitializerMetadata) {
      if (initializerName.empty() || initializerDigest.empty() ||
          expectedInitializerBytes == 0) {
        throw std::runtime_error("DI_CANONICAL_INITIALIZER_METADATA_MISSING");
      }
      const ndn::Name initializerNameValue(initializerName);
      logAssemblyProgressLine(projection, "INITIALIZER_READ_BEGIN", 0.45,
                              expectedInitializerBytes, "initializer");
      logMaterialFetch("initializer", initializerNameValue, "begin", nullptr,
                       initializerDigest);
      std::optional<ndn::Buffer> initializer;
      try {
        initializer = effectiveFetchers.fetchEncryptedLargeData(
          initializerNameValue, ndn::Name(projection.plan.serviceName));
      }
      catch (...) {
        logMaterialFetch("initializer", initializerNameValue, "error", nullptr,
                         initializerDigest);
        throw;
      }
      NativePlaintextBufferGuard initializerPayloadGuard{initializer ? *initializer : emptyPayload};
      logMaterialFetch("initializer", initializerNameValue,
                       initializer && !initializer->empty() ? "returned" : "empty",
                       &initializer, initializerDigest);
      if (!initializer || initializer->empty() ||
          initializer->size() != expectedInitializerBytes ||
          initializer->size() > projection.assembly.maxSourceBytes) {
        if (initializer && initializer->size() != expectedInitializerBytes) {
          throw std::runtime_error("DI_CANONICAL_INITIALIZER_SIZE_MISMATCH");
        }
        throw std::runtime_error("DI_CANONICAL_INITIALIZER_UNAVAILABLE");
      }
      if (sha256Hex(*initializer) != initializerDigest) {
        throw std::runtime_error("DI_CANONICAL_INITIALIZER_DIGEST_MISMATCH");
      }
      logMaterialFetch("initializer", initializerNameValue, "verified", &initializer,
                       initializerDigest);
      logAssemblyProgressLine(projection, "INITIALIZER_READ_DONE", 0.55,
                              static_cast<std::uint64_t>(initializer->size()),
                              "initializer");
      // Move into the final canonical source owner.  sourceScrubber already
      // covers this vector, including exceptions before worker startup.
      canonicalSource.initializerBytes = std::move(*initializer);
      reportProgress("INITIALIZER_VERIFIED", 0.55);
    }
    }

    const auto modelName = root.get<std::string>("modelName", projection.plan.modelName);
    const auto modelDigest = root.get<std::string>("modelIdentityDigest", "");
    if (modelName.empty() || modelDigest.empty()) {
      throw std::runtime_error("DI_CANONICAL_ROOT_MODEL_IDENTITY_MISSING");
    }
    // OA02 worker transport: the certified recipe and the source bytes cross
    // the pipe, and the child's PASS claim is accepted only after the parent
    // revalidated the model bytes against the certified digest below.
    NativeCertifiedAssembly assembled;
    NativeAssemblyProgressHeartbeat assemblyHeartbeat(options.reportProgress, 0.55);
    try {
      std::filesystem::path workerModelFile;
      const auto workerModelBytes =
        static_cast<std::uint64_t>(canonicalSource.modelBytes.size());
      logAssemblyProgressLine(projection, "WORKER_START", 0.55,
                              workerModelBytes,
                              "assembly-worker");
      auto workerRecipe = projection.assembly;
      if (canonicalSource.materializedRole) {
        if (canonicalSource.materializedNodeIndices != projection.assembly.nodeIndices ||
            canonicalSource.materializedSourceDigest != sourceDigest ||
            canonicalSource.materializedGraphDigest != projection.assembly.graphDigest ||
            canonicalSource.materializedInitializerDigest !=
              projection.assembly.canonicalInitializerDigest)
          throw std::runtime_error("DI_CANONICAL_MATERIAL_PROVENANCE_MISMATCH");
        const auto roleIdentity = canonicalOnnxSourceIdentity(canonicalSource, assemblyControl);
        workerRecipe.graphDigest = roleIdentity.graphDigest;
        workerRecipe.canonicalInitializerDigest = roleIdentity.initializerDigest;
        workerRecipe.nodeIndices.clear();
        for (std::size_t index = 0; index < canonicalSource.materializedNodeIndices.size(); ++index)
          workerRecipe.nodeIndices.push_back(index);
        if (workerRecipe.roleKind != "COMPONENT_SET")
          workerRecipe.layerEnd = workerRecipe.nodeIndices.size();
        workerRecipe.materializedRole = true;
        workerRecipe.recipeDigest = nativePlanningDigest(
          canonicalNativeOnnxRecipeJson(workerRecipe));
        // The materialized role has already been durably staged as
        // canonical.onnx.  Pass that immutable file by descriptor so the
        // worker does not receive another anonymous model-sized vector; the
        // child rechecks size and SHA-256 before streaming protobuf parsing.
        workerModelFile = sourceFile;
        if (!canonicalSource.modelBytes.empty()) {
          OPENSSL_cleanse(canonicalSource.modelBytes.data(),
                          canonicalSource.modelBytes.size());
          std::vector<std::uint8_t>{}.swap(canonicalSource.modelBytes);
        }
        // materializedNodeIndices and the digest fields remain as the compact
        // provenance needed for the recipe and audit; selected payloads and
        // the full manifest were released immediately after materialization.
      }
      assembled = runNativeOnnxAssemblyWorkerAt(
        options.workerLocation, canonicalSource, workerRecipe,
        assemblyControl, &canonicalSource, workerModelFile);
      assemblyHeartbeat.stop();
      assemblyHeartbeat.rethrowIfFailed();
      logAssemblyProgressLine(projection, "WORKER_DONE", 0.90,
                              static_cast<std::uint64_t>(assembled.modelBytes.size()),
                              "assembly-worker");
      reportProgress("WORKER_ASSEMBLY_VERIFIED", 0.90);
    }
    catch (...) {
      assemblyHeartbeat.stop();
      sourceScrubber.scrub();
      logAssemblyProgressLine(projection, "WORKER_FAILED", 0.90, 0,
                              "assembly-worker");
      throw;
    }
    // The worker has returned the certified assembled bytes.  Release and
    // cleanse the parent-side canonical graph/initializer before writing
    // manifests and activating the runner; this bounds both resident set and
    // plaintext lifetime.
    sourceScrubber.scrub();
    if (protectedRole) {
      // The worker consumed the authenticated source from memory. The
      // canonical.onnx staging file is no longer an input to finalization;
      // remove it before sealing/decrypting the assembled artifact so the
      // protected path does not retain a full extra model-sized plaintext
      // file beside its ciphertext and request-scoped runner file.
      try {
        eraseProtectedSourceFile(sourceFile);
      }
      catch (const std::exception& error) {
        throw std::runtime_error(
          std::string("DI_NATIVE_ASSEMBLY_SOURCE_STAGING_CLEANUP_FAILED: ") +
          error.what());
      }
    }
    auto modelBytes = std::move(assembled.modelBytes);
    NativePlaintextBufferGuard modelGuard{modelBytes};
    if (modelBytes.empty() || sha256Hex(modelBytes) != assembled.modelDigest) {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_MODEL_DIGEST_MISMATCH");
    }
    std::string protectedKeyReferenceDigest;
    if (protectedRole) {
      const auto keyReference = options.protectedRuntime->keyReference();
      if (!keyReference)
        throw std::runtime_error("DI_PROTECTED_KEY_REFERENCE_UNAVAILABLE");
      protectedKeyReferenceDigest = keyReference->digest();
    }
    std::ostringstream manifest;
    manifest << "{\"schema\":\"ndnsf-di-assembled-onnx-v1\",\"modelName\":"
             << jsonEscape(modelName) << ",\"modelDigest\":"
             << jsonEscape(modelDigest) << ",\"assembledModelDigest\":"
             << jsonEscape(assembled.modelDigest) << ",\"modelManifestDigest\":"
             << jsonEscape(projection.assembly.modelManifestDigest)
             << ",\"artifactProfileDigest\":" << jsonEscape(rootProfile)
             << ",\"graphDigest\":" << jsonEscape(projection.assembly.graphDigest)
             << ",\"role\":" << jsonEscape(projection.assembly.selectedRole)
             << ",\"roleKind\":" << jsonEscape(projection.assembly.roleKind)
             << ",\"artifactDigest\":"
             << jsonEscape(projection.assembly.artifactDigest)
             << ",\"canonicalSourceDataName\":" << jsonEscape(sourceName)
             << ",\"canonicalSourceDigest\":" << jsonEscape(sourceDigest)
             << ",\"canonicalInitializerDigest\":"
             << jsonEscape(projection.assembly.canonicalInitializerDigest)
             << ",\"keyReferenceDigest\":"
             << (protectedRole ? jsonEscape(protectedKeyReferenceDigest) : "\"\"")
             << ",\"rank\":" << projection.assembly.rank
             << ",\"layerBegin\":" << projection.assembly.layerBegin
             << ",\"layerEnd\":" << projection.assembly.layerEnd
             << ",\"recipeDigest\":" << jsonEscape(projection.assembly.recipeDigest)
             << ",\"adapterDescriptorDigest\":"
             << jsonEscape(projection.assembly.adapterDescriptorDigest)
             << ",\"assemblerDescriptorDigest\":"
             << jsonEscape(projection.assembly.assemblerDescriptorDigest)
             << ",\"backendAbi\":" << jsonEscape(projection.assembly.backendAbi)
             << ",\"precision\":" << jsonEscape(projection.assembly.precision)
             << ",\"quantization\":" << jsonEscape(projection.assembly.quantization)
             << ",\"layout\":" << jsonEscape(projection.assembly.layout)
             << ",\"padding\":" << jsonEscape(projection.assembly.padding)
             << ",\"nodeCount\":" << assembled.nodeCount
             << ",\"onnxChecker\":\"NATIVE_STRUCTURAL_CHECK\",\"onnxRuntimeLoad\":\"PENDING_NATIVE_PROVIDER\",\"signer\":"
             << jsonEscape(options.providerIdentity) << "}";
    const auto manifestText = manifest.str();
    std::vector<std::uint8_t> manifestBytes(manifestText.begin(), manifestText.end());
    if (manifestBytes.empty() || manifestBytes.size() > MaxAssemblyMetadataBytes) {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_MANIFEST_TOO_LARGE");
    }
    requireActiveAssembly(options, projection.deadlineMs);
    const auto signature = options.signManifest(
      std::string(reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size()));
    if (signature.empty()) {
      throw std::runtime_error("DI_PROVIDER_ASSEMBLY_SIGNATURE_EMPTY");
    }

    const auto digest = assembled.modelDigest;
    if (digest.rfind("sha256:", 0) != 0 || digest.size() != 71) {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_MODEL_IDENTITY_INVALID");
    }
    // Hold the finalization mutex only while the immutable directory files
    // are created/verified.  Do not keep it across runner-spec construction
    // or exception unwinding: NativeAssemblyArtifactDirectoryOwner cleanup
    // acquires the same mutex and would otherwise self-deadlock.
    std::filesystem::path finalDir;
    std::filesystem::path finalModel;
    std::filesystem::path encryptedArtifactPath;
    std::string encryptedArtifactDigest;
    const bool persistentPlaintextArtifact = protectedRole && cacheCompatibilityMode;
    {
    logAssemblyProgressLine(projection, "CACHE_FINALIZATION_BEGIN", 0.92,
                            static_cast<std::uint64_t>(modelBytes.size()),
                            persistentPlaintextArtifact ? "plaintext-cache" :
                                                         "protected-cache");
    std::unique_lock<std::mutex> finalizationLock(nativeAssemblyFinalizationMutex);
    const auto persistentCacheKey = projection.assembly.recipeDigest;
    if (!isSha256Digest(persistentCacheKey))
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_RECIPE_DIGEST_INVALID");
    finalDir = std::filesystem::path(options.cacheDir) /
      (persistentPlaintextArtifact || !protectedRole ? "assembled" : "protected") /
      safeRole(projection.assembly.selectedRole) /
      (persistentPlaintextArtifact || !protectedRole
         ? persistentCacheKey.substr(7)
         : protectedAssembledCacheKey(projection, options.roleAssemblySpecDigest,
                                      protectedKeyReferenceDigest));
    requireAssemblyDirectoryUnderCacheRoot(options.cacheDir, finalDir);
    const bool finalDirWasAbsent = !std::filesystem::exists(finalDir);
    std::filesystem::create_directories(finalDir);
    if (finalDirWasAbsent) {
      finalArtifactDirectory = finalDir;
      finalArtifactDirectoryOwned = true;
    }
    requireAssemblyDirectoryUnderCacheRoot(options.cacheDir, finalDir);
    if (protectedRole && !persistentPlaintextArtifact && finalDirWasAbsent) {
      protectedArtifactDirectory = finalDir;
      protectedArtifactOwned = true;
    }
    finalModel = finalDir / "model.onnx";
    const auto finalManifest = finalDir / "manifest.json";
    const auto finalSignature = finalDir / "manifest.signature";
    if (persistentPlaintextArtifact) {
      // The explicit cache-compatibility diagnostic has already passed
      // authenticated Selection and an authorized ProtectedRuntime.  It is
      // deliberately a plaintext, recipe-addressed cache, so do not create
      // a model-sized ciphertext and decrypt it again just to populate this
      // diagnostic cache.  The ordinary protected Repo path below retains
      // its encrypted-at-rest contract.
      if (std::filesystem::exists(finalModel)) {
        if (sha256File(finalModel, projection.assembly.maxAssembledBytes) != digest) {
          std::error_code staleError;
          std::filesystem::remove_all(finalDir, staleError);
          if (staleError)
            throw std::runtime_error("DI_NATIVE_ASSEMBLY_CACHE_CONFLICT");
          finalArtifactDirectoryOwned = true;
          std::filesystem::create_directories(finalDir);
        }
      }
      if (!std::filesystem::exists(finalModel)) {
        logAssemblyProgressLine(projection, "CACHE_MODEL_WRITE_BEGIN", 0.94,
                                static_cast<std::uint64_t>(modelBytes.size()),
                                "model.onnx");
        writeFileAtomic(finalModel, modelBytes);
        logAssemblyProgressLine(projection, "CACHE_MODEL_WRITE_DONE", 0.96,
                                static_cast<std::uint64_t>(modelBytes.size()),
                                "model.onnx");
      }
      OPENSSL_cleanse(modelBytes.data(), modelBytes.size());
      std::vector<std::uint8_t>().swap(modelBytes);
    }
    else if (protectedRole) {
      // Source and native assembly buffers are already owned by the staging-directory lease.
      // Ciphertext alone is retained in the final cache; the authenticated
      // plaintext handoff is streamed into the private staging directory.
      const std::string profile = "\"ndnsf-di-provider-workdir-scratch-v1\"";
      const NativeAssembledEntryContext context{
        projection.assembly.modelManifestDigest, options.roleAssemblySpecDigest,
        sha256Hex(std::vector<std::uint8_t>(profile.begin(), profile.end())),
        "MODEL_PROTO", protectedKeyReferenceDigest};
      const auto cipherPath = finalDir / "model.onnx.cipher";
      options.protectedRuntime->withContentKey(nowMs(), [&] (const auto& key) {
        encryptedArtifactDigest = sealNativeAssembledEntryToFile(
          key, modelBytes, cipherPath, context);
      });
      encryptedArtifactPath = cipherPath;
      OPENSSL_cleanse(modelBytes.data(), modelBytes.size());
      std::vector<std::uint8_t>().swap(modelBytes);
      options.protectedRuntime->withContentKey(nowMs(), [&] (const auto& key) {
        // Keep both authority and directory lifetime through the plaintext
        // write; a cancellation after decryption must not recreate its lease.
        finalModel = rootPath / "model.onnx";
        openNativeAssembledEntryToFile(
          key, cipherPath, finalModel, context,
          projection.assembly.maxAssembledBytes, encryptedArtifactDigest);
      });
    }
    else if (std::filesystem::exists(finalModel) &&
             readFile(finalModel, projection.assembly.maxAssembledBytes) != modelBytes) {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_CACHE_CONFLICT");
    }
    if (!protectedRole && !std::filesystem::exists(finalModel)) {
      writeFileAtomic(finalModel, modelBytes);
    }
    if (std::filesystem::exists(finalManifest) &&
        readFile(finalManifest, MaxAssemblyMetadataBytes) != manifestBytes) {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_MANIFEST_CACHE_CONFLICT");
    }
    if (!std::filesystem::exists(finalManifest)) {
      writeFileAtomic(finalManifest, manifestBytes);
    }
    const std::vector<std::uint8_t> signatureBytes(signature.begin(), signature.end());
    if (std::filesystem::exists(finalSignature) &&
        readFile(finalSignature, MaxAssemblyMetadataBytes) != signatureBytes) {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_SIGNATURE_CACHE_CONFLICT");
    }
    if (!std::filesystem::exists(finalSignature)) {
      writeFileAtomic(finalSignature, signatureBytes);
    }
    logAssemblyProgressLine(projection, "CACHE_FINALIZATION_DONE", 0.98,
                            static_cast<std::uint64_t>(modelBytes.size()),
                            "artifact-files");
    }

    NativeModelRunnerSpec spec;
    spec.role = projection.assembly.selectedRole;
    spec.kind = "onnx";
    spec.backend = projection.assembly.backend;
    spec.path = finalModel.string();
    spec.metadata = {
      {"artifactDigest", projection.assembly.artifactDigest},
      {"fragmentDigest", projection.assembly.artifactDigest},
      {"recipeDigest", projection.assembly.recipeDigest},
      {"modelManifestDigest", projection.assembly.modelManifestDigest},
      {"artifactProfileDigest", projection.assembly.artifactProfileDigest},
      {"graphDigest", projection.assembly.graphDigest},
      {"canonicalInitializerDigest", projection.assembly.canonicalInitializerDigest},
      {"adapterDescriptorDigest", projection.assembly.adapterDescriptorDigest},
      {"assemblerDescriptorDigest", projection.assembly.assemblerDescriptorDigest},
      {"backendAbi", projection.assembly.backendAbi},
      {"precision", projection.assembly.precision},
      {"quantization", projection.assembly.quantization},
      {"layout", projection.assembly.layout},
      {"padding", projection.assembly.padding},
      {"maxSourceBytes", std::to_string(projection.assembly.maxSourceBytes)},
      {"maxAssembledBytes", std::to_string(
          projection.assembly.maxAssembledBytes)},
      {"maxNodes", std::to_string(projection.assembly.maxNodes)},
      {"assembledModelDigest", digest},
      {"assemblyManifestDigest", sha256Hex(manifestBytes)},
      {"assemblySignature", signature},
      {"assembledFrom", cacheCompatibilityMode
        ? "cache-compatibility-local-source"
        : "canonical-root-post-selection"},
    };
    // Carry the authenticated generation contract into the runner spec. The
    // provider-side preparation hook narrows the successor map to this local
    // role before the runner is constructed; keeping the projection fields on
    // the assembled spec prevents an adapter-local default from changing the
    // signed dynamic-past or causal-position contract.
    if (projection.generationContract.enabled) {
      const auto& generation = projection.generationContract;
      if (!generation.stateSuccessorMap.empty())
        spec.metadata["stateSuccessorMap"] = generation.stateSuccessorMap;
      if (!generation.positionInputPolicy.empty())
        spec.metadata["positionInputPolicy"] = generation.positionInputPolicy;
      if (!generation.attentionMaskInputName.empty())
        spec.metadata["attentionMaskInputName"] = generation.attentionMaskInputName;
      if (!generation.positionIdsInputName.empty())
        spec.metadata["positionIdsInputName"] = generation.positionIdsInputName;
      if (!generation.cachePositionInputName.empty())
        spec.metadata["cachePositionInputName"] = generation.cachePositionInputName;
    }
    if (protectedRole && !persistentPlaintextArtifact)
      spec.metadata["encryptedArtifactPath"] = encryptedArtifactPath.string();
    if (protectedRole && !persistentPlaintextArtifact)
      spec.metadata["encryptedArtifactDigest"] = encryptedArtifactDigest;
    if (protectedRole && !persistentPlaintextArtifact) {
      // The ciphertext directory is a durable, content-addressed cache entry.
      // Only rootPath/model.onnx remains request-scoped and is erased by the
      // ProtectedRuntime plaintext lease.
      spec.metadata["protectedArtifactPersistent"] = "true";
      spec.metadata["protectedCacheHit"] = "false";
      spec.metadata["protectedKeyReferenceDigest"] = protectedKeyReferenceDigest;
    }
    bindNativeRunnerOutputScopes(projection, spec);
    requireActiveAssembly(options, projection.deadlineMs);
    logAssemblyProgressLine(projection, "RUNNER_SPEC_READY", 1.0, 0, "runner");
    if (!protectedRole) std::filesystem::remove_all(rootPath);
    // The Provider cache receives the runner metadata and installs its own
    // eviction cleanup only after this function returns successfully.
    protectedArtifactOwned = false;
    finalArtifactDirectoryOwned = false;
    return spec;
  }
  catch (...) {
    logAssemblyFailureLine(projection);
    logAssemblyProgressLine(projection, "ASSEMBLY_FAILED", 0.0, 0, "exception");
    const auto failure = std::current_exception();
    if (protectedRole) {
      try {
        options.protectedRuntime->cancel("native assembly failed");
      }
      catch (...) {
        // Preserve the first assembly/cleanup boundary. ProtectedRuntime
        // zeroization remains best effort here; replacing the source-staging
        // or artifact error with a later zeroization exception hides the
        // production failure that selected the cleanup path.
      }
      // Registration itself can fail before the empty directory gains a
      // lease. Remove only an empty directory here; leases own all wiping.
      std::error_code ignored;
      if (protectedArtifactOwned && !protectedArtifactDirectory.empty())
        std::filesystem::remove_all(protectedArtifactDirectory, ignored);
      std::filesystem::remove(rootPath, ignored);
    }
    else std::filesystem::remove_all(rootPath);
    if (finalArtifactDirectoryOwned && !finalArtifactDirectory.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(finalArtifactDirectory, ignored);
    }
    std::rethrow_exception(failure);
  }
}

NativeModelRunnerSpec
prepareNativeCanonicalOnnxRole(
  ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
  const NativeSelectionProjectionV3& projection,
  const NativeCanonicalOnnxAssemblerOptions& options)
{
  NativeCanonicalOnnxFetchers fetchers;
  fetchers.getArtifact = [&ctx] (const ndn::Name& name) {
    return ctx.getArtifact(name);
  };
  fetchers.fetchEncryptedLargeData = [&ctx] (
      const ndn::Name& name, const ndn::Name& service) {
    return ctx.fetchEncryptedLargeData(name, service);
  };
  auto effective = options;
  effective.shouldCancel = [&ctx, configured = options.shouldCancel] {
    return (configured && configured()) || (ctx.isStreamed() && ctx.streamCancelled());
  };
  return prepareNativeCanonicalOnnxRole(fetchers, projection, effective);
}

} // namespace ndnsf::di
