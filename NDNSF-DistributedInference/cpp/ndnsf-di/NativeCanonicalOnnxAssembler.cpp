#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedArtifactStore.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ndnsf::di {

namespace {

constexpr std::uint64_t MaxHelperMetadataBytes = 65536;

std::uint64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string
sha256Hex(const std::vector<std::uint8_t>& bytes)
{
  ndn::util::Sha256 digest;
  digest.update(ndn::span<const std::uint8_t>(bytes.data(), bytes.size()));
  auto hex = digest.toString();
  // ndn-cxx formats this helper's hexadecimal text in uppercase, while the
  // cross-language assembly contract requires canonical lowercase SHA-256.
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return "sha256:" + hex;
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
    if (count > maxBytes - bytes.size()) {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_FILE_TOO_LARGE: " + path.filename().string());
    }
    bytes.insert(bytes.end(), chunk.data(), chunk.data() + count);
  }
  if (!input.eof()) throw std::runtime_error("cannot read native assembly file: " + path.string());
  return bytes;
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

template<typename T>
std::string
jsonArray(const std::vector<T>& values,
          std::function<std::string(const T&)> encode)
{
  std::ostringstream output;
  output << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      output << ',';
    }
    output << encode(values[i]);
  }
  output << ']';
  return output.str();
}

bool
isCanonicalJsonInteger(const std::string& value)
{
  if (value.empty()) {
    return false;
  }
  std::size_t first = 0;
  if (value.front() == '-') {
    first = 1;
  }
  if (first == value.size() ||
      (value.size() - first > 1 && value[first] == '0') ||
      !std::all_of(value.begin() + first,
                   value.end(), [] (unsigned char ch) {
                     return std::isdigit(ch) != 0;
                   })) {
    return false;
  }
  return true;
}

std::string
jsonContracts(const std::vector<NativeAssemblyTensorContractV3>& contracts)
{
  std::ostringstream output;
  output << '[';
  for (std::size_t i = 0; i < contracts.size(); ++i) {
    if (i != 0) {
      output << ',';
    }
    const auto& contract = contracts[i];
    output << "{\"dtype\":" << jsonEscape(contract.dtype)
           << ",\"name\":" << jsonEscape(contract.name)
           << ",\"shape\":[";
    for (std::size_t shapeIndex = 0;
         shapeIndex < contract.shape.size(); ++shapeIndex) {
      if (shapeIndex != 0) {
        output << ',';
      }
      const auto& dimension = contract.shape[shapeIndex];
      // NativeExecutionPlanJson stores dimensions as text because a tensor
      // shape may contain symbolic names.  Re-emit canonical integer
      // dimensions as JSON numbers so the helper reconstructs the exact
      // Python recipe digest; symbolic dimensions remain JSON strings.
      if (isCanonicalJsonInteger(dimension)) {
        output << dimension;
      }
      else {
        output << jsonEscape(dimension);
      }
    }
    output << "]}";
  }
  output << ']';
  return output.str();
}

std::string
assemblyRequestJson(const NativeSelectionProjectionV3& projection,
                    const std::filesystem::path& sourcePath,
                    const std::filesystem::path& initializerPath,
                    const std::string& modelName,
                    const std::string& modelDigest,
                    const std::string& profileDigest,
                    const std::string& provider)
{
  const auto& role = projection.assembly;
  std::vector<std::string> inputNames;
  inputNames.reserve(role.expectedInputs.size());
  for (const auto& item : role.expectedInputs) {
    inputNames.push_back(item.name);
  }
  std::sort(inputNames.begin(), inputNames.end());
  std::vector<std::string> outputNames;
  outputNames.reserve(role.expectedOutputs.size());
  for (const auto& item : role.expectedOutputs) {
    outputNames.push_back(item.name);
  }
  std::sort(outputNames.begin(), outputNames.end());

  std::ostringstream output;
  output << "{\"canonical_model_path\":" << jsonEscape(sourcePath.string());
  if (!initializerPath.empty()) {
    output << ",\"canonical_initializer_path\":"
           << jsonEscape(initializerPath.string());
  }
  output << ",\"model_name\":" << jsonEscape(modelName)
         << ",\"model_digest\":" << jsonEscape(modelDigest)
         << ",\"profile_digest\":" << jsonEscape(profileDigest)
         << ",\"provider\":" << jsonEscape(provider)
         << ",\"role_spec\":{";
  output << "\"adapter_id\":" << jsonEscape(role.adapterId)
         << ",\"adapter_version\":" << jsonEscape(role.adapterVersion)
         << ",\"artifact_digest\":" << jsonEscape(role.artifactDigest)
         << ",\"backend\":" << jsonEscape(role.backend)
         << ",\"backend_abi\":" << jsonEscape(role.backendAbi)
         << ",\"canonical_initializer_digest\":"
         << jsonEscape(role.canonicalInitializerDigest)
         << ",\"device_set\":"
         << jsonArray<std::string>(role.deviceSet,
                                    [] (const auto& value) {
                                      return jsonEscape(value);
                                    })
         << ",\"expected_inputs\":" << jsonContracts(role.expectedInputs)
         << ",\"expected_outputs\":" << jsonContracts(role.expectedOutputs)
         << ",\"graph_digest\":" << jsonEscape(role.graphDigest)
         << ",\"layer_begin\":" << role.layerBegin
         << ",\"layer_end\":" << role.layerEnd
         << ",\"model_manifest_digest\":"
         << jsonEscape(role.modelManifestDigest)
         << ",\"artifact_profile_digest\":"
         << jsonEscape(role.artifactProfileDigest)
         << ",\"adapter_descriptor_digest\":"
         << jsonEscape(role.adapterDescriptorDigest)
         << ",\"assembler_descriptor_digest\":"
         << jsonEscape(role.assemblerDescriptorDigest)
         << ",\"node_indices\":"
         << jsonArray<std::uint64_t>(role.nodeIndices,
                                     [] (const auto value) {
                                       return std::to_string(value);
                                     })
         << ",\"padding\":" << jsonEscape(role.padding)
         << ",\"precision\":" << jsonEscape(role.precision)
         << ",\"protection_epoch\":" << jsonEscape(role.protectionEpoch)
         << ",\"quantization\":" << jsonEscape(role.quantization)
         << ",\"rank\":" << role.rank
         << ",\"recipe_digest\":" << jsonEscape(role.recipeDigest)
         << ",\"required_device_memory_mb\":"
         << role.requiredDeviceMemoryMb
         << ",\"resource_envelope\":{\"maxAssembledBytes\":"
         << role.maxAssembledBytes << ",\"maxNodes\":" << role.maxNodes
         << ",\"maxSourceBytes\":" << role.maxSourceBytes << '}'
         << ",\"role\":" << jsonEscape(role.role)
         << ",\"role_kind\":" << jsonEscape(role.roleKind)
         << ",\"layout\":" << jsonEscape(role.layout)
         << "},\"recipe\":{";
  output << "\"adapter_descriptor_digest\":"
         << jsonEscape(role.adapterDescriptorDigest)
         << ",\"artifact_profile_digest\":"
         << jsonEscape(role.artifactProfileDigest)
         << ",\"assembler_descriptor_digest\":"
         << jsonEscape(role.assemblerDescriptorDigest)
         << ",\"backend_abi\":" << jsonEscape(role.backendAbi)
         << ",\"canonical_initializer_digest\":"
         << jsonEscape(role.canonicalInitializerDigest)
         << ",\"expected_inputs\":" << jsonContracts(role.expectedInputs)
         << ",\"expected_outputs\":" << jsonContracts(role.expectedOutputs)
         << ",\"graph_digest\":" << jsonEscape(role.graphDigest)
         << ",\"input_names\":"
         << jsonArray<std::string>(inputNames,
                                    [] (const auto& value) {
                                      return jsonEscape(value);
                                    })
         << ",\"layer_begin\":" << role.layerBegin
         << ",\"layer_end\":" << role.layerEnd
         << ",\"max_assembled_bytes\":" << role.maxAssembledBytes
         << ",\"max_nodes\":" << role.maxNodes
         << ",\"max_source_bytes\":" << role.maxSourceBytes
         << ",\"model_manifest_digest\":"
         << jsonEscape(role.modelManifestDigest)
         << ",\"node_indices\":"
         << jsonArray<std::uint64_t>(role.nodeIndices,
                                     [] (const auto value) {
                                       return std::to_string(value);
                                     })
         << ",\"output_names\":"
         << jsonArray<std::string>(outputNames,
                                    [] (const auto& value) {
                                      return jsonEscape(value);
                                    })
         << ",\"padding\":" << jsonEscape(role.padding)
         << ",\"precision\":" << jsonEscape(role.precision)
         << ",\"quantization\":" << jsonEscape(role.quantization)
         << ",\"role_kind\":" << jsonEscape(role.roleKind)
         << ",\"schema\":\"ndnsf-di-certified-onnx-assembly-v1\"}}"
         ;
  return output.str();
}

boost::property_tree::ptree
readJson(const std::filesystem::path& path, std::uint64_t maxBytes = MaxHelperMetadataBytes)
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

std::filesystem::path
checkedChildPath(const std::filesystem::path& parent,
                 const std::string& value,
                 const char* label)
{
  const auto child = std::filesystem::absolute(std::filesystem::path(value));
  const auto parentAbsolute = std::filesystem::absolute(parent);
  const auto relative = child.lexically_relative(parentAbsolute);
  if (relative.empty() || relative == "." ||
      relative.begin()->string() == ".." || relative.is_absolute()) {
    throw std::runtime_error(std::string("native assembly ") + label +
                             " escapes its staging directory");
  }
  return child;
}

// A zombie leader pins the process-group identity until descendants have been
// killed. Never signal a process/group after reaping and releasing that PID.
class OwnedAssemblyHelper
{
public:
  void start(const NativeCanonicalOnnxAssemblerOptions& options,
             const std::filesystem::path& request, const std::filesystem::path& outputDir,
             int stdoutFd, int stderrFd, std::uint64_t fileLimit)
  {
    m_pid = ::fork();
    if (m_pid < 0) throw std::runtime_error("cannot fork native assembly helper");
    if (m_pid == 0) {
      const rlimit limit{static_cast<rlim_t>(fileLimit), static_cast<rlim_t>(fileLimit)};
      if (::setpgid(0, 0) != 0 || ::setrlimit(RLIMIT_FSIZE, &limit) != 0 ||
          ::dup2(stdoutFd, STDOUT_FILENO) < 0 || ::dup2(stderrFd, STDERR_FILENO) < 0) _exit(126);
      ::close(stdoutFd);
      ::close(stderrFd);
      ::execlp(options.pythonExecutable.c_str(), options.pythonExecutable.c_str(),
               "-m", options.pythonModule.c_str(), "--input", request.c_str(),
               "--output-dir", outputDir.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    // The child also establishes its group before exec; EACCES/ESRCH here
    // simply means that it already advanced or exited.
    if (::setpgid(m_pid, m_pid) < 0 && errno != EACCES && errno != ESRCH)
      throw std::runtime_error("cannot isolate native assembly helper");
  }

  bool poll()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pid <= 0) return true;
    siginfo_t info{};
    if (::waitid(P_PID, m_pid, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
      if (errno == EINTR) return false;
      // If another reaper consumed the child, this PID is no longer ours.
      if (errno == ECHILD) m_pid = -1;
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_WAIT_FAILED");
    }
    if (info.si_pid == 0) return false;
    ::kill(-m_pid, SIGKILL);
    reapLocked();
    return true;
  }

  void stop()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pid <= 0) return;
    ::kill(-m_pid, SIGKILL);
    ::kill(m_pid, SIGKILL);
    reapLocked();
  }

  int status() const { return m_status; }

private:
  void reapLocked()
  {
    pid_t reaped;
    do { reaped = ::waitpid(m_pid, &m_status, 0); } while (reaped < 0 && errno == EINTR);
    m_pid = -1;
    if (reaped < 0) throw std::runtime_error("DI_NATIVE_ASSEMBLY_REAP_FAILED");
  }
  std::mutex m_mutex;
  pid_t m_pid = -1;
  int m_status = 0;
};

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

void
runPythonHelper(const NativeCanonicalOnnxAssemblerOptions& options,
                std::uint64_t requestDeadlineMs, std::uint64_t maxAssembledBytes,
                const std::filesystem::path& request,
                const std::filesystem::path& outputDir,
                const std::filesystem::path& stdoutPath,
                const std::filesystem::path& stderrPath)
{
  requireActiveAssembly(options, requestDeadlineMs);
  const auto deadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(options.helperTimeoutMs);
  auto child = std::make_shared<OwnedAssemblyHelper>();
  const auto stdoutFd = ::open(stdoutPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  const auto stderrFd = ::open(stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (stdoutFd < 0 || stderrFd < 0) {
    if (stdoutFd >= 0) ::close(stdoutFd);
    if (stderrFd >= 0) ::close(stderrFd);
    throw std::runtime_error("cannot open native assembly helper logs");
  }

  try {
    child->start(options, request, outputDir, stdoutFd, stderrFd,
                 std::max(maxAssembledBytes, MaxHelperMetadataBytes));
  }
  catch (...) {
    ::close(stdoutFd);
    ::close(stderrFd);
    child->stop();
    throw;
  }
  ::close(stdoutFd);
  ::close(stderrFd);
  try {
    if (options.protectedRuntime) {
      // Registered after the staging lease: reverse-order draining reaps the
      // process before removing any plaintext that it can still access.
      options.protectedRuntime->registerHostPlaintextLease(
        "helper-" + request.parent_path().filename().string(), [child] { child->stop(); });
    }
    while (!child->poll()) {
      requireActiveAssembly(options, requestDeadlineMs);
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("DI_NATIVE_ASSEMBLY_TIMEOUT: helper budget exhausted");
      if (std::filesystem::file_size(stdoutPath) > MaxHelperMetadataBytes ||
          std::filesystem::file_size(stderrPath) > MaxHelperMetadataBytes)
        throw std::runtime_error("DI_NATIVE_ASSEMBLY_FILE_TOO_LARGE: helper logs");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    requireActiveAssembly(options, requestDeadlineMs);
    // Completion can race the last poll or a scheduler pause. Success must
    // still satisfy the budget and metadata limits at the return boundary.
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_TIMEOUT: helper budget exhausted");
    if (std::filesystem::file_size(stdoutPath) > MaxHelperMetadataBytes ||
        std::filesystem::file_size(stderrPath) > MaxHelperMetadataBytes)
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_FILE_TOO_LARGE: helper logs");
  }
  catch (...) {
    child->stop();
    throw;
  }
  const auto status = child->status();
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::string detail;
    try {
      const auto bytes = readFile(stderrPath, MaxHelperMetadataBytes);
      detail.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    catch (...) {
      detail = "helper stderr unavailable or exceeds metadata limit";
    }
    throw std::runtime_error("native certified ONNX assembly helper failed: " + detail);
  }
}

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
  return std::filesystem::path(created);
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

} // namespace

NativeModelRunnerSpec
prepareNativeCanonicalOnnxRole(
  const NativeCanonicalOnnxFetchers& fetchers,
  const NativeSelectionProjectionV3& projection,
  const NativeCanonicalOnnxAssemblerOptions& options)
{
  if (options.helperTimeoutMs == 0 || options.helperTimeoutMs > 3600000 ||
      projection.assembly.maxSourceBytes == 0 || projection.assembly.maxAssembledBytes == 0 ||
      projection.assembly.maxAssembledBytes > std::numeric_limits<std::uint64_t>::max() - MaxHelperMetadataBytes)
    throw std::runtime_error("DI_NATIVE_ASSEMBLY_LIMITS_INVALID");
  requireActiveAssembly(options, projection.deadlineMs);
  if (options.providerIdentity.empty() || !options.signManifest) {
    throw std::runtime_error("DI_PROVIDER_ASSEMBLY_SIGNER_MISSING");
  }
  const bool protectedRole = projection.assembly.protectionEpoch != "plaintext-v1";
  if (protectedRole) {
    if (!options.protectedRuntime || options.roleAssemblySpecDigest.empty()) {
      throw std::runtime_error("DI_PROTECTED_GRANT_UNAVAILABLE: assembly runtime is missing");
    }
    options.protectedRuntime->withContentKey(nowMs(), [] (const auto&) {});
  }
  if (!fetchers.getArtifact || !fetchers.fetchEncryptedLargeData) {
    throw std::runtime_error("DI_PROVIDER_ASSEMBLY_FETCHERS_MISSING");
  }
  if (projection.canonicalArtifactName.empty()) {
    throw std::runtime_error("DI_PROVIDER_ASSEMBLY_ROOT_MISSING");
  }
  const ndn::Name rootName(projection.canonicalArtifactName);
  const auto rootPayload = fetchers.getArtifact(rootName);
  if (!rootPayload || rootPayload->empty() || rootPayload->size() >
      projection.assembly.maxSourceBytes) {
    throw std::runtime_error("DI_CANONICAL_ROOT_UNAVAILABLE");
  }
  if (sha256Hex(std::vector<std::uint8_t>(rootPayload->begin(), rootPayload->end())) !=
      projection.assembly.modelManifestDigest) {
    throw std::runtime_error("DI_CANONICAL_ROOT_DIGEST_MISMATCH");
  }

  const auto rootPath = makeStagingDirectory(
    std::filesystem::path(options.cacheDir));
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
      registerNativePlaintextDirectory(*options.protectedRuntime, rootPath,
                                       "assembly-" + rootPath.filename().string());
    }
    const auto rootFile = rootPath / "root.json";
    const auto requestFile = rootPath / "request.json";
    const auto sourceFile = rootPath / "canonical.onnx";
    const auto resultDir = rootPath / "result";
    const auto helperStdout = rootPath / "helper.stdout";
    const auto helperStderr = rootPath / "helper.stderr";
    storeWhileAuthorized([&] {
      writeFileAtomic(rootFile,
                      std::vector<std::uint8_t>(rootPayload->begin(), rootPayload->end()));
    });
    const auto root = readJson(rootFile, projection.assembly.maxSourceBytes);
    if (root.get<std::string>("schema", "") !=
          "ndnsf-di-canonical-model-manifest-v1" ||
        root.get<std::string>("state", "") != "ACTIVE") {
      throw std::runtime_error("DI_CANONICAL_ROOT_SCHEMA_MISMATCH");
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
    if (sourceName.empty()) {
      throw std::runtime_error("DI_CANONICAL_SOURCE_NAME_MISSING");
    }
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
    auto source = fetchers.fetchEncryptedLargeData(
      ndn::Name(sourceName), ndn::Name(projection.plan.serviceName));
    std::vector<std::uint8_t> emptyPayload;
    NativePlaintextBufferGuard sourcePayloadGuard{source ? *source : emptyPayload};
    if (!source || source->empty() || source->size() != expectedSourceBytes ||
        source->size() > projection.assembly.maxSourceBytes) {
      if (source && source->size() != expectedSourceBytes) {
        throw std::runtime_error("DI_CANONICAL_SOURCE_SIZE_MISMATCH");
      }
      throw std::runtime_error("DI_CANONICAL_SOURCE_UNAVAILABLE");
    }
    auto sourceBytes = std::vector<std::uint8_t>(source->begin(), source->end());
    NativePlaintextBufferGuard sourceGuard{sourceBytes};
    if (sha256Hex(sourceBytes) != sourceDigest) {
      throw std::runtime_error("DI_CANONICAL_SOURCE_DIGEST_MISMATCH");
    }
    storeWhileAuthorized([&] { writeFileAtomic(sourceFile, sourceBytes); });

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
    std::filesystem::path initializerFile;
    if (anyInitializerMetadata) {
      if (initializerName.empty() || initializerDigest.empty() ||
          expectedInitializerBytes == 0) {
        throw std::runtime_error("DI_CANONICAL_INITIALIZER_METADATA_MISSING");
      }
      auto initializer = fetchers.fetchEncryptedLargeData(
        ndn::Name(initializerName), ndn::Name(projection.plan.serviceName));
      NativePlaintextBufferGuard initializerPayloadGuard{initializer ? *initializer : emptyPayload};
      if (!initializer || initializer->empty() ||
          initializer->size() != expectedInitializerBytes ||
          initializer->size() > projection.assembly.maxSourceBytes) {
        if (initializer && initializer->size() != expectedInitializerBytes) {
          throw std::runtime_error("DI_CANONICAL_INITIALIZER_SIZE_MISMATCH");
        }
        throw std::runtime_error("DI_CANONICAL_INITIALIZER_UNAVAILABLE");
      }
      auto initializerBytes = std::vector<std::uint8_t>(
        initializer->begin(), initializer->end());
      NativePlaintextBufferGuard initializerGuard{initializerBytes};
      if (sha256Hex(initializerBytes) != initializerDigest) {
        throw std::runtime_error("DI_CANONICAL_INITIALIZER_DIGEST_MISMATCH");
      }
      initializerFile = rootPath / "model.onnx.data";
      storeWhileAuthorized([&] { writeFileAtomic(initializerFile, initializerBytes); });
    }

    const auto modelName = root.get<std::string>("modelName", projection.plan.modelName);
    const auto modelDigest = root.get<std::string>("modelIdentityDigest", "");
    if (modelName.empty() || modelDigest.empty()) {
      throw std::runtime_error("DI_CANONICAL_ROOT_MODEL_IDENTITY_MISSING");
    }
    const auto requestJson = assemblyRequestJson(
      projection, sourceFile, initializerFile, modelName, modelDigest, rootProfile,
      options.providerIdentity);
    storeWhileAuthorized([&] {
      writeFileAtomic(requestFile,
                      std::vector<std::uint8_t>(requestJson.begin(), requestJson.end()));
      std::filesystem::create_directories(resultDir);
    });
    runPythonHelper(options, projection.deadlineMs, projection.assembly.maxAssembledBytes,
                    requestFile, resultDir, helperStdout, helperStderr);
    const auto result = readJson(helperStdout);
    if (result.get<std::string>("schema", "") !=
          "ndnsf-di-native-assembly-result-v1") {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_RESULT_SCHEMA_MISMATCH");
    }
    const auto modelPath = checkedChildPath(resultDir,
      result.get<std::string>("model_path", ""), "model path");
    const auto manifestPath = checkedChildPath(resultDir,
      result.get<std::string>("manifest_path", ""), "manifest path");
    auto modelBytes = readFile(modelPath, projection.assembly.maxAssembledBytes);
    NativePlaintextBufferGuard modelGuard{modelBytes};
    const auto manifestBytes = readFile(manifestPath, MaxHelperMetadataBytes);
    if (modelBytes.empty() || sha256Hex(modelBytes) !=
        result.get<std::string>("model_digest", "")) {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_MODEL_DIGEST_MISMATCH");
    }
    if (manifestBytes.empty() || sha256Hex(manifestBytes) !=
        result.get<std::string>("manifest_digest", "")) {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_MANIFEST_DIGEST_MISMATCH");
    }
    requireActiveAssembly(options, projection.deadlineMs);
    const auto signature = options.signManifest(
      std::string(reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size()));
    if (signature.empty()) {
      throw std::runtime_error("DI_PROVIDER_ASSEMBLY_SIGNATURE_EMPTY");
    }

    const auto digest = result.get<std::string>("model_digest", "");
    if (digest.rfind("sha256:", 0) != 0 || digest.size() != 71) {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_MODEL_IDENTITY_INVALID");
    }
    const auto finalDir = std::filesystem::path(options.cacheDir) /
      (protectedRole ? "protected" : "assembled") /
      safeRole(projection.assembly.selectedRole) /
      (protectedRole ? rootPath.filename().string() : digest.substr(7));
    std::filesystem::create_directories(finalDir);
    auto finalModel = finalDir / "model.onnx";
    const auto finalManifest = finalDir / "manifest.json";
    const auto finalSignature = finalDir / "manifest.signature";
    if (protectedRole) {
      // Source/helper files are already owned by the staging-directory lease.
      // Ciphertext alone is retained in the final cache; ORT reads a fresh
      // authenticated plaintext allocation under that same private lease.
      const std::string profile = "\"ndnsf-di-provider-workdir-scratch-v1\"";
      const NativeAssembledEntryContext context{
        projection.assembly.modelManifestDigest, options.roleAssemblySpecDigest,
        sha256Hex(std::vector<std::uint8_t>(profile.begin(), profile.end())), "MODEL_PROTO"};
      std::vector<std::uint8_t> sealed;
      options.protectedRuntime->withContentKey(nowMs(), [&] (const auto& key) {
        sealed = sealNativeAssembledEntry(key, modelBytes, context);
      });
      const auto cipherPath = finalDir / "model.onnx.cipher";
      writeFileAtomic(cipherPath, sealed);
      const auto stored = readFile(cipherPath, projection.assembly.maxAssembledBytes + MaxHelperMetadataBytes);
      std::vector<std::uint8_t> plaintext;
      NativePlaintextBufferGuard plaintextGuard{plaintext};
      options.protectedRuntime->withContentKey(nowMs(), [&] (const auto& key) {
        plaintext = openNativeAssembledEntry(key, stored, context,
                                             projection.assembly.maxAssembledBytes);
        // Keep both authority and directory lifetime through the plaintext
        // write; a cancellation after decryption must not recreate its lease.
        finalModel = rootPath / "model.onnx";
        writeFileAtomic(finalModel, plaintext);
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
        readFile(finalManifest, MaxHelperMetadataBytes) != manifestBytes) {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_MANIFEST_CACHE_CONFLICT");
    }
    if (!std::filesystem::exists(finalManifest)) {
      writeFileAtomic(finalManifest, manifestBytes);
    }
    const std::vector<std::uint8_t> signatureBytes(signature.begin(), signature.end());
    if (std::filesystem::exists(finalSignature) &&
        readFile(finalSignature, MaxHelperMetadataBytes) != signatureBytes) {
      throw std::runtime_error("DI_NATIVE_ASSEMBLY_SIGNATURE_CACHE_CONFLICT");
    }
    if (!std::filesystem::exists(finalSignature)) {
      writeFileAtomic(finalSignature, signatureBytes);
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
      {"assembledFrom", "canonical-root-post-selection"},
    };
    if (projection.dataflow.terminalResponseOwner) {
      // The V3 dataflow contract is the authority for terminal ownership.
      // Bind the assembled ONNX output to the same sealed scope consumed by
      // NativeProviderHandler; do not recover it by guessing an ONNX output
      // name at response time.
      spec.metadata["outputScope"] = "final-response";
      spec.metadata["final"] = "true";
    }
    requireActiveAssembly(options, projection.deadlineMs);
    if (!protectedRole) std::filesystem::remove_all(rootPath);
    return spec;
  }
  catch (...) {
    if (protectedRole) {
      options.protectedRuntime->cancel("native assembly failed");
      // Registration itself can fail before the empty directory gains a
      // lease. Remove only an empty directory here; leases own all wiping.
      std::error_code ignored;
      std::filesystem::remove(rootPath, ignored);
    }
    else std::filesystem::remove_all(rootPath);
    throw;
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
