#include "NDNSF-DistributedInference/cpp/ndnsf-di/Provider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderArtifactCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedArtifactStore.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloMergeRunner.hpp"

#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/io.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <boost/asio/io_context.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <condition_variable>
#include <fstream>
#include <cstdlib>
#include <future>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <utility>
#include <fcntl.h>

namespace ndnsf::di {

struct ProviderConfig::Impl
{
  std::filesystem::path sourcePath;
  std::string providerName = "/ndnsf/provider";
  std::string groupName = "/ndnsf/group";
  std::string controllerName = "/ndnsf/controller";
  std::filesystem::path trustSchema;
  std::filesystem::path controllerCertificatePath;
  std::filesystem::path planPath;
  std::filesystem::path manifestPath;
  std::filesystem::path artifactCacheDir = "/tmp/ndnsf-di-native-artifacts";
  std::string serviceName;
  std::vector<std::string> allowedRoles;
  std::size_t workerCount = 1;
  std::size_t maxArtifactBytes = 1ULL << 30;
  std::size_t maxArtifactEntries = 8;
  Milliseconds assemblyJobTimeout{300000};
  bool checkOnly = false;
  bool serve = false;
};

namespace {

[[noreturn]] void invalid(const std::string& message)
{
  throw std::invalid_argument("invalid provider configuration: " + message);
}

std::string readFile(const std::filesystem::path& path)
{
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size == 0 || size > 4 * 1024 * 1024)
    invalid("configuration file is unavailable or too large");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    invalid("configuration file cannot be opened");
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
    invalid("configuration file cannot be read");
  return bytes;
}

void requireAbsoluteName(const std::string& value, const char* field)
{
  if (value.empty())
    invalid(std::string(field) + " is empty");
  try {
    if (ndn::Name(value).empty() || value.front() != '/')
      invalid(std::string(field) + " must be an absolute NDN name");
  }
  catch (...) {
    invalid(std::string(field) + " is not a valid NDN name");
  }
}

std::size_t positive(const boost::property_tree::ptree& node,
                     const char* field)
{
  const auto value = node.get_optional<std::uint64_t>(field);
  if (!value || *value == 0)
    invalid(std::string(field) + " must be positive");
  return static_cast<std::size_t>(*value);
}

} // namespace

ProviderConfig detail::parseProviderLaunchFile(const std::filesystem::path& path)
{
  const auto canonical = std::filesystem::absolute(path).lexically_normal();
  boost::property_tree::ptree root;
  try {
    std::istringstream input(readFile(canonical));
    boost::property_tree::read_json(input, root);
  }
  catch (const std::exception& error) {
    invalid(std::string("malformed JSON: ") + error.what());
  }
  const auto schema = root.get<std::string>("schema", "");
  if (schema != "ndnsf-di-native-provider-launch-v1")
    invalid("schema must be ndnsf-di-native-provider-launch-v1");
  for (const auto& item : root) {
    if (item.first != "schema" && item.first != "arguments" && item.first != "cache")
      invalid("unknown top-level field " + item.first);
  }
  auto result = std::make_shared<ProviderConfig::Impl>();
  result->sourcePath = canonical;
  const auto arguments = root.get_child_optional("arguments");
  if (!arguments)
    invalid("arguments array is required");
  std::vector<std::string> argv;
  for (const auto& item : *arguments) {
    if (!item.first.empty() || item.second.data().empty())
      invalid("arguments must be a string array");
    argv.push_back(item.second.data());
  }
  // Parse one shared C++ grammar.  argv[0] is optional and ignored when it
  // is an executable name; all paths are resolved relative to this file.
  std::vector<const char*> raw;
  raw.reserve(argv.size() + 1);
  raw.push_back("provider");
  for (const auto& arg : argv) raw.push_back(arg.c_str());
  auto parsed = ProviderConfig::fromCommandLine(
    static_cast<int>(raw.size()), raw.data());
  result = std::make_shared<ProviderConfig::Impl>(*parsed.m_impl);
  result->sourcePath = canonical;
  if (result->trustSchema.is_relative())
    result->trustSchema = canonical.parent_path() / result->trustSchema;
  if (result->controllerCertificatePath.is_relative() && !result->controllerCertificatePath.empty())
    result->controllerCertificatePath = canonical.parent_path() / result->controllerCertificatePath;
  if (result->planPath.is_relative() && !result->planPath.empty())
    result->planPath = canonical.parent_path() / result->planPath;
  if (result->manifestPath.is_relative() && !result->manifestPath.empty())
    result->manifestPath = canonical.parent_path() / result->manifestPath;
  if (result->artifactCacheDir.is_relative())
    result->artifactCacheDir = canonical.parent_path() / result->artifactCacheDir;
  if (const auto cache = root.get_child_optional("cache")) {
    for (const auto& item : *cache) {
      if (item.first != "max_artifact_bytes" && item.first != "max_artifact_entries" &&
          item.first != "assembly_job_timeout_ms")
        invalid("unknown cache field " + item.first);
    }
    if (cache->get_optional<std::uint64_t>("max_artifact_bytes"))
      result->maxArtifactBytes = positive(*cache, "max_artifact_bytes");
    if (cache->get_optional<std::uint64_t>("max_artifact_entries"))
      result->maxArtifactEntries = positive(*cache, "max_artifact_entries");
    if (cache->get_optional<std::uint64_t>("assembly_job_timeout_ms"))
      result->assemblyJobTimeout = Milliseconds(positive(*cache, "assembly_job_timeout_ms"));
  }
  return ProviderConfig(std::move(result));
}

namespace {

enum class WorkerReapResult
{
  Joined,
  Detached,
  Unrecovered,
};

struct ProviderReaperOwner
{
  std::unique_ptr<std::thread> thread;
  std::unique_ptr<std::thread> target;

  static void finish(std::unique_ptr<std::thread>& candidate) noexcept
  {
    if (!candidate || !candidate->joinable())
      return;
    try {
      if (candidate->get_id() == std::this_thread::get_id())
        candidate->detach();
      else
        candidate->join();
    }
    catch (...) {
      try {
        if (candidate->joinable())
          candidate->detach();
      }
      catch (...) {
        // The last-resort owner intentionally leaks the std::thread object;
        // destroying a still-joinable object would terminate the process.
        candidate.release();
      }
    }
  }

  ~ProviderReaperOwner() noexcept
  {
    if (target)
      finish(target);
    finish(thread);
  }
};

WorkerReapResult reapWorkerNoexcept(std::thread& worker) noexcept
{
  if (!worker.joinable())
    return WorkerReapResult::Joined;
  try {
    worker.join();
    return WorkerReapResult::Joined;
  }
  catch (...) {
    try {
      if (worker.joinable())
        worker.join();
      return worker.joinable() ? WorkerReapResult::Unrecovered :
        WorkerReapResult::Joined;
    }
    catch (...) {
    }
    try {
      if (worker.joinable())
        worker.detach();
    }
    catch (...) {
    }
    return worker.joinable() ? WorkerReapResult::Unrecovered :
      WorkerReapResult::Detached;
  }
}

std::string valueAfter(const std::string& option, int& index, int argc,
                       const char* const* argv)
{
  if (index + 1 >= argc)
    invalid("missing value for " + option);
  return argv[++index];
}

std::size_t parsePositive(const std::string& value, const std::string& option)
{
  try {
    std::size_t used = 0;
    const auto parsed = std::stoull(value, &used);
    if (used != value.size() || parsed == 0 || parsed > std::numeric_limits<std::size_t>::max())
      invalid(option + " must be a positive integer");
    return static_cast<std::size_t>(parsed);
  }
  catch (...) {
    invalid(option + " must be a positive integer");
  }
}

std::shared_ptr<const NativeAdapterRegistry> emptyAdapters()
{
  return std::make_shared<const NativeAdapterRegistry>();
}

NativeOnnxWorkerLocation
resolveWorkerLocation()
{
  std::vector<std::string> candidates;
  if (const char* pinned = std::getenv("NDNSF_DI_WORKER_BINARY")) {
    if (*pinned != '\0') candidates.emplace_back(pinned);
  }
  std::array<char, 4096> selfPath{};
  const auto count = ::readlink("/proc/self/exe", selfPath.data(), selfPath.size() - 1);
  if (count > 0) {
    selfPath[static_cast<std::size_t>(count)] = '\0';
    const auto dir = std::filesystem::path(selfPath.data()).parent_path();
    candidates.push_back((dir / "DI_NativeOnnxAssemblyWorker").string());
    candidates.push_back((dir.parent_path() / "DI_NativeOnnxAssemblyWorker").string());
  }
  candidates.emplace_back("build-nac182/DI_NativeOnnxAssemblyWorker");
  candidates.emplace_back("build/DI_NativeOnnxAssemblyWorker");
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(candidate, ec) || ec)
      continue;
    std::ifstream input(candidate, std::ios::binary | std::ios::ate);
    if (!input)
      continue;
    const auto size = input.tellg();
    if (size <= 0 || static_cast<std::uint64_t>(size) > 256ULL * 1024ULL * 1024ULL)
      continue;
    input.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
      continue;
    return NativeOnnxWorkerLocation{candidate, sha256TensorBytes(bytes)};
  }
  return {};
}

std::string
signAssemblyManifest(ndn::KeyChain& keyChain,
                     const ndn::security::Certificate& certificate,
                     const std::string& manifest)
{
  ndn::Data data(ndn::Name("/NDNSF-DI/ASSEMBLY-MANIFEST"));
  data.setContent(ndn::span<const std::uint8_t>(
    reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size()));
  keyChain.sign(data, ndn::security::signingByCertificate(certificate));
  const auto signature = data.getSignatureValue();
  if (signature.value_size() == 0)
    return {};
  return ndn_service_framework::selectionGatedHex(
    ndn::span<const std::uint8_t>(signature.value_begin(), signature.value_size()));
}

std::string
fileDigest(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    invalid("file cannot be opened for digest: " + path.string());
  const auto size = input.tellg();
  if (size <= 0 || static_cast<std::uint64_t>(size) > 512ULL * 1024ULL * 1024ULL)
    invalid("file size is outside the supported digest bound: " + path.string());
  input.seekg(0);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    invalid("file cannot be read for digest: " + path.string());
  return sha256TensorBytes(bytes);
}

struct ProviderMetrics
{
  std::atomic<std::uint64_t> sourceFetches{0};
  std::atomic<std::uint64_t> assemblies{0};
  std::atomic<std::uint64_t> templateHits{0};
  std::atomic<std::uint64_t> runnersCreated{0};
};

ProviderArtifactKey
providerArtifactKey(const NativeSelectionProjectionV3& projection,
                    const std::string& providerIdentity,
                    const std::string& canonicalSourceName,
                    const std::string& canonicalSourceDigest)
{
  const auto& role = projection.assembly;
  return ProviderArtifactKey{
    canonicalSourceDigest,
    canonicalSourceName,
    projection.canonicalArtifactName,
    role.modelManifestDigest,
    role.canonicalInitializerDigest,
    role.graphDigest,
    role.selectedRole,
    projection.offerDigest,
    role.recipeDigest,
    role.backendAbi,
    role.deviceSet.empty() ? std::string{} : role.deviceSet.front(),
    role.precision,
    role.quantization,
    role.layout,
    role.artifactProfileDigest,
    projection.groupCapabilityV1,
    role.protectionEpoch,
    projection.hasGrantBinding
      ? projection.provider + "|" + projection.grantName + "|" + projection.grantDigest
      : providerIdentity};
}

std::filesystem::path
providerCachedModelPath(const std::string& cacheDir,
                        const NativeSelectionProjectionV3& projection,
                        const std::string& assembledDigest)
{
  if (assembledDigest.rfind("sha256:", 0) != 0 || assembledDigest.size() != 71)
    throw std::runtime_error("DI_PROVIDER_ARTIFACT_DIGEST_INVALID");
  auto role = projection.assembly.selectedRole;
  for (auto& ch : role) {
    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_'))
      ch = '_';
  }
  if (role.empty())
    role = "role";
  return std::filesystem::path(cacheDir) / "assembled" / role /
    assembledDigest.substr(7) / "model.onnx";
}

std::vector<std::uint8_t>
providerReadBounded(const std::filesystem::path& path, std::uint64_t maxBytes)
{
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size == 0 || size > maxBytes)
    throw std::runtime_error("DI_PROVIDER_ARTIFACT_CIPHERTEXT_UNAVAILABLE");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("DI_PROVIDER_ARTIFACT_CIPHERTEXT_UNAVAILABLE");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (!input.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size())))
    throw std::runtime_error("DI_PROVIDER_ARTIFACT_CIPHERTEXT_UNAVAILABLE");
  return bytes;
}

std::uint64_t
providerNowMs()
{
  return static_cast<std::uint64_t>(std::max<std::int64_t>(0,
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()));
}

std::filesystem::path
providerProtectedStaging(const std::filesystem::path& cacheDir)
{
  const auto base = cacheDir / ".staging";
  std::filesystem::create_directories(base);
  std::string pattern = (base / "provider-protected-XXXXXX").string();
  std::vector<char> mutablePattern(pattern.begin(), pattern.end());
  mutablePattern.push_back('\0');
  const auto created = ::mkdtemp(mutablePattern.data());
  if (created == nullptr)
    throw std::runtime_error("DI_PROVIDER_PROTECTED_STAGING_UNAVAILABLE");
  return std::filesystem::path(created);
}

void
providerWriteFile(const std::filesystem::path& path,
                  const std::vector<std::uint8_t>& bytes)
{
  const auto temporary = path.parent_path() /
    (path.filename().string() + ".tmp-" + std::to_string(::getpid()));
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
      throw std::runtime_error("DI_PROVIDER_PROTECTED_WRITE_UNAVAILABLE");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output)
      throw std::runtime_error("DI_PROVIDER_PROTECTED_WRITE_UNAVAILABLE");
  }
  std::filesystem::rename(temporary, path);
}

std::pair<std::string, std::string>
providerCanonicalSourceIdentity(
  ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
  const NativeSelectionProjectionV3& projection)
{
  if (projection.canonicalArtifactName.empty())
    throw std::runtime_error("DI_PROVIDER_ARTIFACT_ROOT_IDENTITY_MISSING");
  const auto rootPayload = ctx.getArtifact(ndn::Name(projection.canonicalArtifactName));
  if (!rootPayload || rootPayload->empty())
    throw std::runtime_error("DI_PROVIDER_ARTIFACT_ROOT_IDENTITY_UNAVAILABLE");
  if (sha256TensorBytes(std::vector<std::uint8_t>(rootPayload->begin(), rootPayload->end())) !=
      projection.assembly.modelManifestDigest)
    throw std::runtime_error("DI_PROVIDER_ARTIFACT_ROOT_IDENTITY_MISMATCH");
  boost::property_tree::ptree root;
  try {
    const std::string rootBytes(reinterpret_cast<const char*>(rootPayload->data()),
                                rootPayload->size());
    std::istringstream input(rootBytes);
    boost::property_tree::read_json(input, root);
  }
  catch (const std::exception& error) {
    throw std::runtime_error(std::string("DI_PROVIDER_ARTIFACT_ROOT_IDENTITY_INVALID: ") + error.what());
  }
  const auto metadata = root.get_child_optional("metadata");
  const auto sourceName = metadata
    ? metadata->get<std::string>("canonicalSourceDataName", "") : std::string{};
  const auto sourceDigest = metadata
    ? metadata->get<std::string>("canonicalSourceDigest", "") : std::string{};
  const auto materialManifestName = metadata
    ? metadata->get<std::string>("materialManifestDataName", "") : std::string{};
  const bool materialBacked = metadata &&
    (metadata->get<bool>("materialBacked", false) || !materialManifestName.empty());
  if ((!materialBacked && sourceName.empty()) || sourceDigest.empty())
    throw std::runtime_error("DI_PROVIDER_ARTIFACT_SOURCE_IDENTITY_MISSING");
  // Material-backed receipts intentionally omit a full source object. The
  // canonical root remains the stable source identity for the provider cache;
  // the digest still binds the assembled role to the inspected model.
  return {sourceName.empty() ? projection.canonicalArtifactName : sourceName, sourceDigest};
}

class CountingProviderRunnerFactory final : public NativeModelRunnerFactory
{
public:
  CountingProviderRunnerFactory(std::shared_ptr<NativeModelRunnerFactory> inner,
                                std::shared_ptr<ProviderMetrics> metrics)
    : m_inner(std::move(inner)), m_metrics(std::move(metrics))
  {
  }

  std::shared_ptr<NativeModelRunner>
  create(const NativeModelRunnerSpec& spec) const final
  {
    auto runner = m_inner->create(spec);
    m_metrics->runnersCreated.fetch_add(1, std::memory_order_relaxed);
    return runner;
  }

private:
  std::shared_ptr<NativeModelRunnerFactory> m_inner;
  std::shared_ptr<ProviderMetrics> m_metrics;
};

struct ProviderFaceServeCall
{
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic<bool> cancelled{false};
  bool done = false;
  std::exception_ptr error;
  std::shared_ptr<NativeServiceRegistration> result;
};

std::shared_ptr<NativeModelRunnerFactory>
makeProviderRunnerFactory(const std::shared_ptr<ProviderMetrics>& metrics)
{
  auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
  registerOnnxRuntimeBackend(*factory);
  factory->registerBackend("native-yolo-postprocess",
                           [] (const NativeModelRunnerSpec& spec) {
                             return makeNativeYoloMergeRunner(spec);
                           });
  factory->freeze();
  return std::make_shared<CountingProviderRunnerFactory>(std::move(factory), metrics);
}

NativeExecutionPlan
loadProviderPlan(const std::filesystem::path& planPath, const std::string& serviceName)
{
  NativeExecutionPlan plan;
  if (planPath.empty()) {
    plan.serviceName = serviceName;
    plan.executionPolicy = "DATA_DRIVEN_V2";
    return plan;
  }
  std::ifstream input(planPath);
  if (!input)
    invalid("plan file cannot be opened");
  try {
    plan = nativeExecutionPlanForServiceFromJson(input, serviceName);
  }
  catch (const std::exception& error) {
    invalid(std::string("plan is invalid: ") + error.what());
  }
  if (plan.serviceName != serviceName)
    invalid("plan service does not match Provider service");
  return plan;
}

} // namespace

ProviderConfig ProviderConfig::fromFile(const std::filesystem::path& path)
{
  return detail::parseProviderLaunchFile(path);
}

ProviderConfig ProviderConfig::fromCommandLine(int argc,
                                               const char* const* argv)
{
  if (argc < 1 || argv == nullptr)
    invalid("argument vector is empty");
  auto result = std::make_shared<Impl>();
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i] == nullptr ? std::string{} : argv[i];
    if (option == "--provider") result->providerName = valueAfter(option, i, argc, argv);
    else if (option == "--group") result->groupName = valueAfter(option, i, argc, argv);
    else if (option == "--controller") result->controllerName = valueAfter(option, i, argc, argv);
    else if (option == "--controller-cert") result->controllerCertificatePath = valueAfter(option, i, argc, argv);
    else if (option == "--trust-schema") result->trustSchema = valueAfter(option, i, argc, argv);
    else if (option == "--service") result->serviceName = valueAfter(option, i, argc, argv);
    else if (option == "--role") result->allowedRoles.push_back(valueAfter(option, i, argc, argv));
    else if (option == "--workers") result->workerCount = parsePositive(valueAfter(option, i, argc, argv), option);
    else if (option == "--artifact-cache-dir") result->artifactCacheDir = valueAfter(option, i, argc, argv);
    else if (option == "--plan") result->planPath = valueAfter(option, i, argc, argv);
    else if (option == "--manifest") result->manifestPath = valueAfter(option, i, argc, argv);
    else if (option == "--check-only") result->checkOnly = true;
    else if (option == "--serve") result->serve = true;
    else invalid("unknown option " + option);
  }
  requireAbsoluteName(result->providerName, "provider");
  requireAbsoluteName(result->groupName, "group");
  requireAbsoluteName(result->controllerName, "controller");
  if (result->trustSchema.empty())
    invalid("trust-schema is required");
  if (result->artifactCacheDir.empty())
    invalid("artifact-cache-dir is empty");
  if (result->serviceName.empty())
    invalid("service is required");
  requireAbsoluteName(result->serviceName, "service");
  if (result->allowedRoles.empty())
    result->allowedRoles.push_back("/Backbone");
  std::set<std::string> roles;
  for (const auto& role : result->allowedRoles) {
    requireAbsoluteName(role, "role");
    if (!roles.insert(role).second)
      invalid("duplicate role " + role);
  }
  return ProviderConfig(std::move(result));
}

bool ProviderConfig::valid() const noexcept
{
  return static_cast<bool>(m_impl);
}

bool ProviderConfig::equivalent(const ProviderConfig& other) const noexcept
{
  if (!m_impl || !other.m_impl)
    return !m_impl && !other.m_impl;
  return m_impl->providerName == other.m_impl->providerName &&
    m_impl->groupName == other.m_impl->groupName &&
    m_impl->controllerName == other.m_impl->controllerName &&
    m_impl->trustSchema == other.m_impl->trustSchema &&
    m_impl->controllerCertificatePath == other.m_impl->controllerCertificatePath &&
    m_impl->planPath == other.m_impl->planPath &&
    m_impl->manifestPath == other.m_impl->manifestPath &&
    m_impl->artifactCacheDir == other.m_impl->artifactCacheDir &&
    m_impl->serviceName == other.m_impl->serviceName &&
    m_impl->allowedRoles == other.m_impl->allowedRoles &&
    m_impl->workerCount == other.m_impl->workerCount &&
    m_impl->maxArtifactBytes == other.m_impl->maxArtifactBytes &&
    m_impl->maxArtifactEntries == other.m_impl->maxArtifactEntries &&
    m_impl->assemblyJobTimeout == other.m_impl->assemblyJobTimeout &&
    m_impl->checkOnly == other.m_impl->checkOnly &&
    m_impl->serve == other.m_impl->serve;
}

struct ProviderRegistration::State
{
  std::string serviceName;
  std::shared_ptr<NativeServiceRegistration> native;
};

ProviderRegistration::~ProviderRegistration() noexcept
{
  close();
}

void ProviderRegistration::close() noexcept
{
  if (m_state && m_state->native)
    m_state->native->close();
}

bool ProviderRegistration::closed() const noexcept
{
  return !m_state || !m_state->native || m_state->native->closed();
}

bool ProviderRegistration::valid() const noexcept
{
  return m_state != nullptr && m_state->native && m_state->native->valid();
}

const std::string& ProviderRegistration::serviceName() const noexcept
{
  static const std::string empty;
  return m_state ? m_state->serviceName : empty;
}

struct Provider::State
{
  std::shared_ptr<ProviderMetrics> metrics = std::make_shared<ProviderMetrics>();
  std::shared_ptr<ProviderArtifactCache> artifactCache;
  std::shared_ptr<const ProviderConfig::Impl> config;
  std::shared_ptr<ndn::Face> face;
  bool ownsFace = false;
  bool ownsIoContext = false;
  std::unique_ptr<ndn::KeyChain> keyChain;
  ndn::KeyChain* borrowedKeyChain = nullptr;
#if defined(NDNSF_DI_PROVIDER_TEST_SEAM)
  std::shared_ptr<NativeModelRunnerFactory> testRunnerFactory;
  NativeProviderHandlerConfig::RunnerPreparationFactory testPreparationFactory;
  NativeProviderHandlerConfig::ProtectedRuntimeFactory testProtectedRuntimeFactory;
  ndn_service_framework::ServiceProvider::AckStrategyHandler testAckHandler;
#endif
  ndn::security::Certificate providerCertificate;
  ndn::security::Certificate controllerCertificate;
  std::unique_ptr<ndn_service_framework::CertificatePublisher> certificatePublisher;
  std::shared_ptr<ndn_service_framework::ServiceProvider> serviceProvider;
  std::shared_ptr<NativeInferenceProvider> nativeHost;
  std::shared_ptr<const NativeAdapterRegistry> adapters;
  NativeOnnxWorkerLocation workerLocation;
  std::string manifestDigest;
  std::uint64_t providerStartedAtMs = 0;
  mutable std::mutex mutex;
  mutable std::mutex serveMutex;
  std::vector<std::shared_ptr<ProviderRegistration::State>> registrations;
  bool stopped = false;
  std::unique_ptr<boost::asio::io_context::work> ioWork;
  std::thread ioThread;
  std::thread::id ioThreadId;
  std::condition_variable ioCondition;
  std::mutex ioMutex;
  bool ioRunning = false;
  bool ioStopped = false;
  bool ioFailed = false;
  bool ioStopping = false;
  bool ioStopRequested = false;
  bool detachedWorkerCleanup = false;
  // The detached reaper closure owns itself.  Keeping only a weak reference
  // here prevents State destruction on the IO worker from joining a reaper
  // that is itself waiting for that worker.
  std::weak_ptr<ProviderReaperOwner> reaperOwner;
  std::shared_ptr<ProviderReaperOwner> orphanWorker;
  bool faceShutdownRequested = false;
  bool resourcesReleased = false;
  std::vector<std::shared_ptr<ProviderFaceServeCall>> pendingServeCalls;
  // Provider lifecycle callbacks use a Core-owned operation worker rather
  // than running drain synchronously on the caller.  The worker is retained
  // by State so it cannot outlive the Face, KeyChain, or native host.
  std::shared_ptr<ndn_service_framework::OperationRuntime> asyncRuntime;
};

void Provider::releaseStoppedResources() const noexcept
{
  if (!m_state)
    return;
  // A copied Provider may outlive the Runtime owner.  Serialize the final
  // owner move with every serve admission before inspecting the shared
  // pointers; serve takes this gate before taking State::mutex.
  std::unique_lock<std::mutex> serveLock(m_state->serveMutex);
  // Move owners out while holding the state mutex, then destroy them after
  // unlocking.  NativeInferenceProvider retains the ServiceProvider, so the
  // native host must be released first; both are Face-bound and may run
  // cancellation destructors that must not re-enter this mutex.
  std::unique_ptr<ndn_service_framework::CertificatePublisher> certificatePublisher;
  std::shared_ptr<ndn_service_framework::ServiceProvider> serviceProvider;
  std::shared_ptr<NativeInferenceProvider> nativeHost;
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (!m_state->ioStopped || m_state->resourcesReleased)
      return;
    m_state->resourcesReleased = true;
    certificatePublisher = std::move(m_state->certificatePublisher);
    serviceProvider = std::move(m_state->serviceProvider);
    nativeHost = std::move(m_state->nativeHost);
  }
  nativeHost.reset();
  serviceProvider.reset();
  certificatePublisher.reset();
}

bool Provider::requestOwnedFaceShutdown() const noexcept
{
  if (!m_state || !m_state->face || !m_state->ownsFace)
    return true;
  {
    std::lock_guard<std::mutex> lock(m_state->ioMutex);
    if (m_state->faceShutdownRequested)
      return true;
    m_state->faceShutdownRequested = true;
  }
  try {
    // ndn-cxx posts the cancellation work to the Face io_context. The caller
    // must keep that context alive until waitForOwnedFaceShutdown (or the
    // ordered stop marker in requestStopIo) has allowed the work to run.
    m_state->face->shutdown();
    return true;
  }
  catch (...) {
    std::lock_guard<std::mutex> lock(m_state->ioMutex);
    m_state->faceShutdownRequested = false;
    m_state->ioFailed = true;
    m_state->ioCondition.notify_all();
    return false;
  }
}

bool Provider::waitForOwnedFaceShutdown() const noexcept
{
  if (!m_state || !m_state->face || !m_state->ownsFace)
    return true;
  if (!requestOwnedFaceShutdown())
    return false;
  {
    std::lock_guard<std::mutex> lock(m_state->ioMutex);
    if (!m_state->ioThread.joinable())
      return true;
    if (std::this_thread::get_id() == m_state->ioThreadId)
      return true;
  }
  std::shared_ptr<std::atomic<bool>> completed;
  try {
    completed = std::make_shared<std::atomic<bool>>(false);
    boost::asio::post(m_state->face->getIoContext(), [state = m_state, completed] {
      completed->store(true, std::memory_order_release);
      state->ioCondition.notify_all();
    });
    std::unique_lock<std::mutex> lock(m_state->ioMutex);
    m_state->ioCondition.wait(lock, [state = m_state, completed] {
      return completed->load(std::memory_order_acquire) || state->ioFailed;
    });
    return completed->load(std::memory_order_acquire) && !m_state->ioFailed;
  }
  catch (...) {
    std::lock_guard<std::mutex> lock(m_state->ioMutex);
    m_state->ioFailed = true;
    m_state->ioCondition.notify_all();
    return false;
  }
}

bool Provider::finishOwnedFaceShutdownWithoutWorker() const noexcept
{
  if (!m_state || !m_state->face || !m_state->ownsFace)
    return true;
  if (!requestOwnedFaceShutdown())
    return false;
  try {
    auto& io = m_state->face->getIoContext();
    io.restart();
    // Face::shutdown() queues one cancellation operation. Poll a bounded
    // number of follow-up handlers so constructor-time NAC fetchers are
    // cancelled even when open() was closed before startIo().
    for (int round = 0; round != 64; ++round) {
      if (io.poll() == 0)
        break;
    }
    return true;
  }
  catch (...) {
    std::lock_guard<std::mutex> lock(m_state->ioMutex);
    m_state->ioFailed = true;
    m_state->ioCondition.notify_all();
    return false;
  }
}

bool Provider::postIoStopMarker() const noexcept
{
  if (!m_state || !m_state->face)
    return false;
  try {
    boost::asio::post(m_state->face->getIoContext(), [state = m_state] {
      if (state->ownsIoContext) {
        state->face->getIoContext().stop();
      }
      else {
        std::lock_guard<std::mutex> lock(state->ioMutex);
        state->ioStopRequested = true;
        state->ioCondition.notify_all();
      }
    });
    return true;
  }
  catch (...) {
    std::lock_guard<std::mutex> lock(m_state->ioMutex);
    m_state->ioFailed = true;
    if (!m_state->ownsIoContext)
      m_state->ioStopRequested = true;
    m_state->ioCondition.notify_all();
    return false;
  }
}

bool Provider::launchReaper(std::shared_ptr<std::thread> worker,
                            bool selfStop) const noexcept
{
  if (!m_state || !worker || !worker->joinable())
    return false;
  try {
    auto owner = std::make_shared<ProviderReaperOwner>();
    // Reserve the only allocation needed by the Unrecovered path before the
    // reaper thread starts.  Its worker-side lambda must not allocate while
    // transferring a still-joinable thread.
    owner->target = std::make_unique<std::thread>();
    const std::weak_ptr<State> weakState = m_state;
    owner->thread = std::make_unique<std::thread>([owner, weakState, worker] {
      {
        try {
          const auto joined = reapWorkerNoexcept(*worker);
          if (joined == WorkerReapResult::Unrecovered) {
            // target was allocated before this thread started, and
            // std::thread move assignment is noexcept. Keep ownership even
            // if the worker has already cleared ioRunning, so its joinable
            // object is never destroyed on the reaper stack.
            *owner->target = std::move(*worker);
          }
          const auto state = weakState.lock();
          if (!state)
            return;
          bool releaseResources = false;
          {
            std::lock_guard<std::mutex> lock(state->ioMutex);
            // A join/detach failure can race with the worker's final
            // callback. Once ioRunning is false, publish the terminal fence
            // here instead of leaving waiters blocked forever behind a
            // non-joinable worker.
            if (joined == WorkerReapResult::Joined || !state->ioRunning) {
              state->ioStopped = true;
              state->ioStopping = false;
              state->detachedWorkerCleanup = false;
              state->ioThreadId = {};
              state->ioWork.reset();
              if (joined != WorkerReapResult::Joined)
                state->ioFailed = true;
              releaseResources = true;
            }
            else {
              state->detachedWorkerCleanup = true;
              state->ioFailed = true;
            }
            if (joined == WorkerReapResult::Unrecovered)
              state->orphanWorker = owner;
          }
          state->ioCondition.notify_all();
          if (releaseResources)
            Provider(state).releaseStoppedResources();
        }
        catch (...) {
          // A condition-variable or mutex implementation may report an
          // exception even though all normal reaper operations are noexcept.
          // Never let it escape a std::thread entry point; retain the worker
          // in owner->target when needed and publish sticky failure best
          // effort so waiters cannot mistake this path for success.
          const auto state = weakState.lock();
          if (!state)
            return;
          try {
            std::lock_guard<std::mutex> lock(state->ioMutex);
            state->ioFailed = true;
            state->detachedWorkerCleanup = true;
            if (!state->ioRunning) {
              state->ioStopped = true;
              state->ioStopping = false;
              state->detachedWorkerCleanup = false;
              state->ioThreadId = {};
              state->ioWork.reset();
            }
          }
          catch (...) {
          }
          state->ioCondition.notify_all();
        }
      }
    });
    {
      std::lock_guard<std::mutex> lock(m_state->ioMutex);
      m_state->reaperOwner = owner;
    }
    try {
      owner->thread->detach();
    }
    catch (...) {
      std::lock_guard<std::mutex> lock(m_state->ioMutex);
      m_state->ioFailed = true;
      // The persistent owner retains a joinable reaper when detach is not
      // available; its destructor joins it outside the IO worker.
    }
    return true;
  }
  catch (...) {
    if (selfStop) {
      bool releaseResources = false;
      {
        std::lock_guard<std::mutex> lock(m_state->ioMutex);
        m_state->detachedWorkerCleanup = true;
        m_state->ioFailed = true;
        if (!m_state->ioRunning) {
          m_state->ioStopped = true;
          m_state->ioStopping = false;
          m_state->detachedWorkerCleanup = false;
          m_state->ioThreadId = {};
          m_state->ioWork.reset();
          releaseResources = true;
        }
      }
      const auto result = reapWorkerNoexcept(*worker);
      if (result == WorkerReapResult::Unrecovered)
        retainUnrecoveredWorker(*worker);
      m_state->ioCondition.notify_all();
      if (releaseResources)
        releaseStoppedResources();
      return false;
    }
    const auto joined = reapWorkerNoexcept(*worker);
    bool releaseResources = false;
    {
      std::lock_guard<std::mutex> lock(m_state->ioMutex);
      if (joined == WorkerReapResult::Joined || !m_state->ioRunning) {
        m_state->ioStopped = true;
        m_state->ioStopping = false;
        m_state->detachedWorkerCleanup = false;
        m_state->ioThreadId = {};
        m_state->ioWork.reset();
        if (joined != WorkerReapResult::Joined)
          m_state->ioFailed = true;
        releaseResources = true;
      }
      else {
        m_state->detachedWorkerCleanup = true;
        m_state->ioFailed = true;
      }
    }
    if (joined == WorkerReapResult::Unrecovered)
      retainUnrecoveredWorker(*worker);
    m_state->ioCondition.notify_all();
    if (releaseResources)
      releaseStoppedResources();
    return joined == WorkerReapResult::Joined;
  }
}

void Provider::retainUnrecoveredWorker(std::thread& worker) const noexcept
{
  if (!m_state || !worker.joinable())
    return;
  try {
    auto owner = std::make_shared<ProviderReaperOwner>();
    owner->target = std::make_unique<std::thread>(std::move(worker));
    std::lock_guard<std::mutex> lock(m_state->ioMutex);
    m_state->orphanWorker = std::move(owner);
  }
  catch (...) {
    // The caller has already recorded sticky failure. A final best-effort
    // detach keeps the local std::thread from terminating the process; the
    // worker itself will still observe detachedWorkerCleanup if it exits.
    try {
      if (worker.joinable())
        worker.detach();
    }
    catch (...) {
    }
  }
}

Provider Provider::fromConfig(const ProviderConfig& config)
{
  if (!config.m_impl)
    throw std::invalid_argument("Provider requires a validated ProviderConfig");
  std::error_code ec;
  if (!std::filesystem::is_regular_file(config.m_impl->trustSchema, ec) || ec ||
      std::filesystem::file_size(config.m_impl->trustSchema, ec) == 0 || ec)
    throw std::invalid_argument("provider trust schema is unavailable");
  auto state = std::make_shared<State>();
  state->config = config.m_impl;
  state->artifactCache = std::make_shared<ProviderArtifactCache>(
    ProviderArtifactCacheConfig{config.m_impl->maxArtifactBytes,
                                config.m_impl->maxArtifactEntries,
                                config.m_impl->assemblyJobTimeout});
  state->asyncRuntime = ndn_service_framework::OperationRuntime::create();
  state->face = std::make_shared<ndn::Face>();
  state->ownsFace = true;
  state->ownsIoContext = true;
  // Match the production executable boundary: ServiceProvider owns its
  // process KeyChain and resolves the same identities by name.  Keeping the
  // creating KeyChain alive here preserves the private keys for signer use.
  state->keyChain = std::make_unique<ndn::KeyChain>();
  const auto providerIdentity = state->keyChain->createIdentity(
    ndn::Name(state->config->providerName), ndn::RsaKeyParams(2048));
  ndn::security::Identity controllerIdentity;
  const auto providerCert = providerIdentity.getDefaultKey().getDefaultCertificate();
  ndn::security::Certificate controllerCert;
  if (!state->config->controllerCertificatePath.empty()) {
    auto loaded = ndn::io::load<ndn::security::Certificate>(
      state->config->controllerCertificatePath.string());
    if (!loaded || !loaded->isValid() ||
        loaded->getIdentity() != ndn::Name(state->config->controllerName))
      throw std::invalid_argument("controller certificate does not match controller identity");
    controllerCert = *loaded;
  }
  else if (const char* certPath = std::getenv("NDNSF_CONTROLLER_CERT_FILE");
           certPath != nullptr && *certPath != '\0') {
    auto loaded = ndn::io::load<ndn::security::Certificate>(certPath);
    if (!loaded || !loaded->isValid() ||
        loaded->getIdentity() != ndn::Name(state->config->controllerName))
      throw std::invalid_argument("NDNSF_CONTROLLER_CERT_FILE identity mismatch");
    controllerCert = *loaded;
  }
#if defined(NDNSF_DI_PROVIDER_TEST_SEAM)
  else if (std::getenv("NDNSF_SPEC185_ALLOW_LOCAL_CONTROLLER") != nullptr) {
    const auto controllerIdentity = state->keyChain->createIdentity(
      ndn::Name(state->config->controllerName), ndn::RsaKeyParams(2048));
    controllerCert = controllerIdentity.getDefaultKey().getDefaultCertificate();
  }
#endif
  else {
    throw std::invalid_argument(
      "controller certificate is required; set NDNSF_CONTROLLER_CERT_FILE or pass --controller-cert");
  }
  state->providerCertificate = providerCert;
  state->controllerCertificate = controllerCert;
  state->serviceProvider = std::make_shared<ndn_service_framework::ServiceProvider>(
    *state->face, ndn::Name(state->config->groupName), providerCert,
    controllerCert, state->config->trustSchema.string());
  state->serviceProvider->init();
  state->serviceProvider->fetchPermissionsFromController(
    ndn::Name(state->config->controllerName));
  state->certificatePublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
    *state->face, *state->keyChain, providerCert.getName());
  state->adapters = emptyAdapters();
  state->nativeHost = std::make_shared<NativeInferenceProvider>(
    state->serviceProvider, state->adapters);
  state->workerLocation = resolveWorkerLocation();
  if (!state->config->manifestPath.empty()) {
    state->manifestDigest = fileDigest(state->config->manifestPath);
  }
  const auto started = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  state->providerStartedAtMs = static_cast<std::uint64_t>(std::max<std::int64_t>(0, started));
  return Provider(std::move(state));
}

#if defined(NDNSF_DI_PROVIDER_TEST_SEAM)
Provider Provider::fromServiceProviderForTest(
  ndn::Face& face,
  ndn_service_framework::ServiceProvider& serviceProvider,
  ndn::KeyChain& keyChain,
  const ndn::security::Certificate& providerCertificate,
  const ndn::security::Certificate& controllerCertificate,
  const ProviderConfig& config,
  std::shared_ptr<NativeModelRunnerFactory> runnerFactory,
  NativeProviderHandlerConfig::RunnerPreparationFactory preparationFactory,
  NativeProviderHandlerConfig::ProtectedRuntimeFactory protectedRuntimeFactory,
  ndn_service_framework::ServiceProvider::AckStrategyHandler ackHandler)
{
  if (!config.m_impl)
    throw std::invalid_argument("Provider requires a validated ProviderConfig");
  auto state = std::make_shared<State>();
  state->config = config.m_impl;
  state->artifactCache = std::make_shared<ProviderArtifactCache>(
    ProviderArtifactCacheConfig{config.m_impl->maxArtifactBytes,
                                config.m_impl->maxArtifactEntries,
                                config.m_impl->assemblyJobTimeout});
  state->asyncRuntime = ndn_service_framework::OperationRuntime::create();
  state->face = std::shared_ptr<ndn::Face>(&face, [] (ndn::Face*) {});
  state->ownsFace = false;
  state->ownsIoContext = false;
  state->borrowedKeyChain = &keyChain;
  state->testRunnerFactory = std::move(runnerFactory);
  state->testPreparationFactory = std::move(preparationFactory);
  state->testProtectedRuntimeFactory = std::move(protectedRuntimeFactory);
  state->testAckHandler = std::move(ackHandler);
  state->providerCertificate = providerCertificate;
  state->controllerCertificate = controllerCertificate;
  state->serviceProvider = std::shared_ptr<ndn_service_framework::ServiceProvider>(
    &serviceProvider, [] (ndn_service_framework::ServiceProvider*) {});
  state->adapters = emptyAdapters();
  state->nativeHost = std::make_shared<NativeInferenceProvider>(
    state->serviceProvider, state->adapters);
  state->workerLocation = resolveWorkerLocation();
  if (!state->config->manifestPath.empty())
    state->manifestDigest = fileDigest(state->config->manifestPath);
  const auto started = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  state->providerStartedAtMs = static_cast<std::uint64_t>(
    std::max<std::int64_t>(0, started));
  return Provider(std::move(state));
}
#endif

Provider::~Provider() noexcept
{
}

void Provider::startIo()
{
  if (!m_state || !m_state->face)
    throw std::runtime_error("Provider has no Face");
  std::unique_lock<std::mutex> lock(m_state->ioMutex);
  std::lock_guard<std::mutex> stateLock(m_state->mutex);
  if (m_state->stopped)
    throw std::runtime_error("Provider is stopped");
  if (m_state->ioRunning || m_state->ioThread.joinable())
    return;
  m_state->ioWork = std::make_unique<boost::asio::io_context::work>(
    m_state->face->getIoContext());
  m_state->ioStopped = false;
  m_state->ioFailed = false;
  m_state->ioStopRequested = false;
  m_state->detachedWorkerCleanup = false;
  const auto state = m_state;
  state->ioThread = std::thread([state] {
    {
      std::lock_guard<std::mutex> lock(state->ioMutex);
      state->ioThreadId = std::this_thread::get_id();
      state->ioRunning = true;
    }
    state->ioCondition.notify_all();
    try {
      if (state->ownsIoContext) {
        state->face->getIoContext().run();
      }
      else {
        // A borrowed Face may share its io_context with the integration
        // environment. run_one() lets Provider leave its own worker on a
        // private stop marker without stopping that shared context.
        for (;;) {
          const auto handled = state->face->getIoContext().run_one_for(
            std::chrono::milliseconds(10));
          std::lock_guard<std::mutex> lock(state->ioMutex);
          if (state->ioStopRequested)
            break;
          if (handled == 0 && state->face->getIoContext().stopped()) {
            // A borrowed Face shares its io_context with the embedding
            // environment.  That owner may transiently leave the context in
            // the stopped state between processEvents/restart calls. Keep the
            // Provider worker alive until an explicit stop request rather
            // than publishing a false terminal state or busy-spinning.
            if (state->ioStopping) {
              // The embedding owner can stop its context after
              // requestStopIo() has posted the marker.  A stopped context
              // cannot execute that marker, so observe the already-published
              // stopping fence directly and let the reaper join this worker.
              state->ioStopRequested = true;
              break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }
      }
    }
    catch (...) {
      std::lock_guard<std::mutex> lock(state->ioMutex);
      state->ioFailed = true;
    }
    bool detachedCleanup = false;
    {
      std::lock_guard<std::mutex> lock(state->ioMutex);
      state->ioRunning = false;
      state->ioThreadId = {};
      detachedCleanup = state->detachedWorkerCleanup;
      if (detachedCleanup) {
        state->ioStopped = true;
        state->ioStopping = false;
        state->ioWork.reset();
        state->detachedWorkerCleanup = false;
      }
    }
    state->ioCondition.notify_all();
    if (detachedCleanup)
      Provider(state).releaseStoppedResources();
  });
}

bool Provider::waitForIoBarrier(Milliseconds timeout) const
{
  if (!m_state || !m_state->face)
    return false;
  std::unique_lock<std::mutex> initialLock(m_state->ioMutex);
  if (m_state->ioStopped)
    return !m_state->ioFailed;
  if (m_state->ioStopping) {
    if (std::this_thread::get_id() == m_state->ioThreadId)
      return true;
    const auto stopped = [state = m_state] { return state->ioStopped; };
    if (timeout.count() == 0 ? !stopped() :
        !m_state->ioCondition.wait_for(initialLock, timeout, stopped))
      return false;
    return !m_state->ioFailed;
  }
  if (!m_state->ioThread.joinable())
    return !m_state->ioFailed;
  if (std::this_thread::get_id() == m_state->ioThreadId)
    return true;
  initialLock.unlock();
  std::shared_ptr<std::atomic<bool>> completed =
    std::make_shared<std::atomic<bool>>(false);
  try {
    boost::asio::post(m_state->face->getIoContext(), [state = m_state, completed] {
      completed->store(true, std::memory_order_release);
      state->ioCondition.notify_all();
    });
  }
  catch (...) {
    return false;
  }
  std::unique_lock<std::mutex> lock(m_state->ioMutex);
  const auto ready = [state = m_state, completed] {
    return completed->load(std::memory_order_acquire) || !state->ioRunning;
  };
  if (timeout.count() == 0) {
    if (!ready())
      return false;
  }
  else if (!m_state->ioCondition.wait_for(lock, timeout, ready)) {
    return false;
  }
  return (completed->load(std::memory_order_acquire) || !m_state->ioRunning) &&
         !m_state->ioFailed;
}

bool Provider::stopIo() const noexcept
{
  if (!m_state || !m_state->face)
    return true;
  std::thread worker;
  bool selfStop = false;
  bool hasWorker = false;
  {
    std::unique_lock<std::mutex> lock(m_state->ioMutex);
    if (m_state->ioStopped)
      return !m_state->ioFailed;
    if (m_state->ioStopping) {
      if (std::this_thread::get_id() == m_state->ioThreadId)
        return false;
      m_state->ioCondition.wait(lock, [state = m_state] { return state->ioStopped; });
      return !m_state->ioFailed;
    }
    m_state->ioStopping = true;
    hasWorker = m_state->ioThread.joinable();
    selfStop = std::this_thread::get_id() == m_state->ioThreadId;
  }
  // Keep ioWork and the worker alive while Face::shutdown queues its
  // cancellation handler and while the completion fence waits for that
  // handler. Stopping the context first leaves NAC/Core SegmentFetchers live.
  const auto shutdownReady = hasWorker ? waitForOwnedFaceShutdown() :
    finishOwnedFaceShutdownWithoutWorker();
  {
    std::lock_guard<std::mutex> lock(m_state->ioMutex);
    if (m_state->ownsIoContext)
      m_state->ioWork.reset();
    if (m_state->ioThread.joinable())
      worker = std::move(m_state->ioThread);
    else {
      m_state->ioStopped = true;
      m_state->ioStopping = false;
      m_state->ioThreadId = {};
    }
  }
  if (!hasWorker) {
    m_state->ioCondition.notify_all();
    return shutdownReady && !m_state->ioFailed;
  }
  if (selfStop) {
    const auto markerPosted = postIoStopMarker();
    if (!markerPosted && m_state->ownsIoContext)
      m_state->face->getIoContext().stop();
    // The caller is the Face worker itself, so it cannot join its own thread.
    // Keep the explicit drain result false until the detached reaper has
    // completed the join; reporting success here would expose a live worker
    // behind an apparently drained Provider.
    try {
      launchReaper(std::make_shared<std::thread>(std::move(worker)), true);
    }
    catch (...) {
      bool releaseResources = false;
      {
        std::lock_guard<std::mutex> lock(m_state->ioMutex);
        m_state->detachedWorkerCleanup = true;
        m_state->ioFailed = true;
        if (!m_state->ioRunning) {
          m_state->ioStopped = true;
          m_state->ioStopping = false;
          m_state->detachedWorkerCleanup = false;
          m_state->ioThreadId = {};
          m_state->ioWork.reset();
          releaseResources = true;
        }
      }
      if (reapWorkerNoexcept(worker) == WorkerReapResult::Unrecovered)
        retainUnrecoveredWorker(worker);
      m_state->ioCondition.notify_all();
      if (releaseResources)
        releaseStoppedResources();
    }
    return false;
  }
  bool markerPosted = true;
  if (m_state->ownsIoContext)
    m_state->face->getIoContext().stop();
  else
    markerPosted = postIoStopMarker();
  if (!markerPosted && m_state->ownsIoContext)
    m_state->face->getIoContext().stop();
  const auto joined = reapWorkerNoexcept(worker);
  if (joined != WorkerReapResult::Joined) {
    bool releaseResources = false;
    {
      std::lock_guard<std::mutex> lock(m_state->ioMutex);
      m_state->ioFailed = true;
      if (!m_state->ioRunning) {
        m_state->ioStopped = true;
        m_state->ioStopping = false;
        m_state->detachedWorkerCleanup = false;
        m_state->ioThreadId = {};
        m_state->ioWork.reset();
        releaseResources = true;
      }
      else {
        m_state->detachedWorkerCleanup = true;
      }
    }
    if (joined == WorkerReapResult::Unrecovered)
      retainUnrecoveredWorker(worker);
    m_state->ioCondition.notify_all();
    if (releaseResources)
      releaseStoppedResources();
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(m_state->ioMutex);
    m_state->ioWork.reset();
    m_state->ioStopped = true;
    m_state->ioStopping = false;
    m_state->ioThreadId = {};
  }
  m_state->ioCondition.notify_all();
  return joined == WorkerReapResult::Joined && shutdownReady && !m_state->ioFailed;
}

void Provider::requestStopIo() const noexcept
{
  if (!m_state || !m_state->face)
    return;
  std::thread worker;
  bool releaseNow = false;
  bool selfStop = false;
  {
    std::lock_guard<std::mutex> lock(m_state->ioMutex);
    if (m_state->ioStopped || m_state->ioStopping)
      return;
    m_state->ioStopping = true;
    const auto pendingCalls = std::move(m_state->pendingServeCalls);
    for (const auto& pending : pendingCalls) {
      pending->cancelled.store(true, std::memory_order_release);
      {
        std::lock_guard<std::mutex> callLock(pending->mutex);
        pending->error = std::make_exception_ptr(
          std::runtime_error("Provider Face IO loop is stopping"));
        pending->done = true;
      }
      pending->condition.notify_all();
    }
    if (!m_state->ioThread.joinable()) {
      releaseNow = true;
    }
    else if (!m_state->ownsIoContext &&
             m_state->face->getIoContext().stopped()) {
      // A stopped borrowed context cannot execute the queued marker. Record
      // the explicit stop request directly so the Provider worker can leave
      // its run_one loop and the reaper can join it.
      m_state->ioStopRequested = true;
    }
  }
  if (releaseNow) {
    const auto shutdownReady = finishOwnedFaceShutdownWithoutWorker();
    {
      std::lock_guard<std::mutex> lock(m_state->ioMutex);
      m_state->ioStopped = true;
      m_state->ioStopping = false;
      m_state->ioThreadId = {};
      if (!shutdownReady)
        m_state->ioFailed = true;
    }
    m_state->ioCondition.notify_all();
    releaseStoppedResources();
    return;
  }
  // Keep the work guard and worker alive until ndn-cxx has queued its
  // asynchronous cancellation operation. The final stop marker below is
  // posted afterwards, establishing the required ordering on this Face.
  requestOwnedFaceShutdown();
  {
    std::lock_guard<std::mutex> lock(m_state->ioMutex);
    // Keep a borrowed context alive until the stop marker is posted and its
    // worker has a chance to observe the stop request.
    if (m_state->ownsIoContext)
      m_state->ioWork.reset();
    selfStop = std::this_thread::get_id() == m_state->ioThreadId;
    if (m_state->ioThread.joinable())
      worker = std::move(m_state->ioThread);
  }
  // NativeServiceRegistration::close() posts its detach to this same Face.
  // Queue a final stop marker after that detach and after Face::shutdown so
  // all cancellation work is processed before the reaper joins the loop.
  const auto markerPosted = postIoStopMarker();
  if (!markerPosted && m_state->ownsIoContext)
    m_state->face->getIoContext().stop();
  // Runtime::close() is a non-blocking admission fence.  The detached
  // reaper owns the moved thread and State until the Face loop has exited;
  // explicit drain() still uses stopIo() for a bounded join/result fence.
  try {
    launchReaper(std::make_shared<std::thread>(std::move(worker)), selfStop);
  }
  catch (...) {
    auto reapResult = WorkerReapResult::Joined;
    if (worker.joinable()) {
      reapResult = reapWorkerNoexcept(worker);
    }
    const bool joined = reapResult == WorkerReapResult::Joined;
    if (reapResult == WorkerReapResult::Unrecovered)
      retainUnrecoveredWorker(worker);
    bool releaseResources = false;
    {
      std::lock_guard<std::mutex> lock(m_state->ioMutex);
      m_state->ioFailed = true;
      if (joined || !m_state->ioRunning) {
        m_state->ioStopped = true;
        m_state->ioStopping = false;
        m_state->detachedWorkerCleanup = false;
        m_state->ioThreadId = {};
        m_state->ioWork.reset();
        releaseResources = true;
      }
      else {
        m_state->ioStopping = true;
        m_state->detachedWorkerCleanup = true;
      }
    }
    m_state->ioCondition.notify_all();
    if (releaseResources)
      releaseStoppedResources();
  }
}

ProviderRegistration Provider::serve(const ServiceDefinition& service)
{
  if (!m_state || !m_state->config)
    throw DiError("RUNTIME_CLOSED", "local", "provider", "Provider is empty");
  std::unique_lock<std::mutex> serveLock(m_state->serveMutex);
  if (service.serviceName.empty() || service.allowedRoles.empty())
    throw DiError("INVALID_ARGUMENT", "local", "provider",
                  "Provider service requires name and allowedRoles");
  const auto canonicalName = [] (const std::string& value, const char* field) {
    try {
      const ndn::Name parsed(value);
      if (parsed.empty() || value.front() != '/')
        throw std::invalid_argument("name is not absolute");
      return parsed.toUri();
    }
    catch (const std::exception& error) {
      throw DiError("INVALID_ARGUMENT", "local", "provider",
                    std::string("invalid ") + field + ": " + error.what());
    }
  };
  std::set<std::string> roles;
  std::set<std::string> configuredRoles;
  for (const auto& role : m_state->config->allowedRoles)
    configuredRoles.insert(canonicalName(role, "configured role"));
  for (const auto& role : service.allowedRoles) {
    if (role.empty())
      throw DiError("INVALID_ARGUMENT", "local", "provider",
                    "Provider service roles must be non-empty and unique");
    const auto canonical = canonicalName(role, "service role");
    if (!roles.insert(canonical).second || configuredRoles.count(canonical) == 0)
      throw DiError("INVALID_ARGUMENT", "local", "provider",
                    "Provider service role is outside its validated allow-list");
  }
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->stopped)
      throw DiError("PROVIDER_SERVE_FAILED", "local", "provider", "Provider is stopped");
  }
  const auto canonicalServiceName = canonicalName(service.serviceName, "service");
  if (canonicalServiceName != canonicalName(m_state->config->serviceName, "configured service"))
    throw DiError("INVALID_ARGUMENT", "local", "provider",
                  "Provider service does not match its validated configuration");
  NativeProviderHandlerConfig nativeConfig;
  try {
    nativeConfig.plan = loadProviderPlan(m_state->config->planPath, canonicalServiceName);
    if (nativeConfig.plan.modelName.empty())
      nativeConfig.plan.modelName = "provider-runtime";
    nativeConfig.executionPolicy = nativeConfig.plan.executionPolicy;
    if (!m_state->config->planPath.empty())
      nativeConfig.planDigest = fileDigest(m_state->config->planPath);
    nativeConfig.localProviderName = ndn::Name(m_state->config->providerName).toUri();
    std::shared_ptr<ndn_service_framework::ServiceProvider> serviceProvider;
    {
      std::lock_guard<std::mutex> lock(m_state->mutex);
      serviceProvider = m_state->serviceProvider;
    }
    if (!serviceProvider)
      throw std::runtime_error("Provider ServiceProvider is unavailable");
    nativeConfig.providerBootId = serviceProvider->getProviderBootEpoch();
    nativeConfig.workerCount = m_state->config->workerCount;
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw DiError("PROVIDER_SERVE_FAILED", "local", "provider", error.what());
  }
  try {
#if defined(NDNSF_DI_PROVIDER_TEST_SEAM)
    nativeConfig.runnerFactory = m_state->testRunnerFactory
      ? std::make_shared<CountingProviderRunnerFactory>(
          m_state->testRunnerFactory, m_state->metrics)
      : makeProviderRunnerFactory(m_state->metrics);
#else
    nativeConfig.runnerFactory = makeProviderRunnerFactory(m_state->metrics);
#endif
    installNativeProtectedGrantFactory(nativeConfig);
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw DiError("PROVIDER_SERVE_FAILED", "local", "provider", error.what());
  }
  const auto config = m_state->config;
  const auto workerLocation = m_state->workerLocation;
  const auto providerIdentity = nativeConfig.localProviderName;
  const auto providerBootId = nativeConfig.providerBootId;
  const auto providerCert = m_state->providerCertificate;
  const auto expectedManifestDigest = m_state->manifestDigest;
  const auto providerStartedAtMs = m_state->providerStartedAtMs;
  const auto artifactCache = m_state->artifactCache;
  auto* keyChain = m_state->keyChain != nullptr
    ? m_state->keyChain.get() : m_state->borrowedKeyChain;
  nativeConfig.runnerPreparationFactory =
    [cacheDir = config->artifactCacheDir.string(), providerIdentity,
     workerLocation, providerBootId, assemblyTimeout = config->assemblyJobTimeout,
     providerCert, keyChain, expectedManifestDigest, providerStartedAtMs,
     artifactCache,
     metrics = m_state->metrics] (
      ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
      const NativeSelectionProjectionV3& projection,
      const std::shared_ptr<ProtectedRuntime>& protectedRuntime) {
      if (!expectedManifestDigest.empty() &&
          projection.assembly.modelManifestDigest != expectedManifestDigest) {
        throw std::runtime_error("DI_PROVIDER_MANIFEST_BINDING_MISMATCH");
      }
      NativeModelRunnerSpec spec;
      if (projection.assembly.mergeKind == "NATIVE_POSTPROCESS") {
        spec = nativeYoloMergeRunnerSpecFromProjection(projection);
      }
      else {
        NativeCanonicalOnnxAssemblerOptions options;
        options.cacheDir = cacheDir;
        options.providerIdentity = providerIdentity;
        options.assemblyTimeoutMs = static_cast<std::uint64_t>(
          std::max<std::int64_t>(1, assemblyTimeout.count()));
        options.workerLocation = workerLocation;
        options.protectedRuntime = protectedRuntime;
        options.reportProgress = makeNativeAssemblyProgressReporter(
          ctx, projection, projection.assembly.backend.empty()
            ? std::string("native") : projection.assembly.backend,
          1, 0, projection.assemblyProgressSequence);
        if (protectedRuntime) {
          const auto& payload = ctx.assignment().assignmentPayload;
          options.roleAssemblySpecDigest = nativeAssemblyDigestFromCanonicalProjection(
            std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
        }
        options.signManifest = [keyChain, providerCert](const std::string& bytes) {
          return keyChain == nullptr ? std::string{} :
            signAssemblyManifest(*keyChain, providerCert, bytes);
        };
        if (!artifactCache) {
          spec = prepareNativeCanonicalOnnxRole(ctx, projection, options);
          metrics->sourceFetches.fetch_add(1, std::memory_order_relaxed);
        }
        else {
          NativeRequestControl control;
          control.requestId = projection.requestId;
          control.attempt = projection.attempt;
          const auto nowWall = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
          const auto remaining = projection.deadlineMs > static_cast<std::uint64_t>(
              std::max<std::int64_t>(0, nowWall))
            ? projection.deadlineMs - static_cast<std::uint64_t>(std::max<std::int64_t>(0, nowWall))
            : 0;
          control.deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(remaining);
          control.cancelled = [&ctx] {
            return ctx.isStreamed() && ctx.streamCancelled();
          };
          const auto sourceIdentity = providerCanonicalSourceIdentity(ctx, projection);
          const auto key = providerArtifactKey(
            projection, providerIdentity, sourceIdentity.first, sourceIdentity.second);
          auto lease = artifactCache->acquireWithRunner(
            key, projection, control,
            [&ctx, projection, options, metrics, protectedRuntime] (
                const NativeRequestControl& jobControl) mutable {
              options.shouldCancel = jobControl.cancelled;
              options.assemblyTimeoutMs = static_cast<std::uint64_t>(std::max<std::int64_t>(
                1, std::chrono::duration_cast<Milliseconds>(
                  jobControl.deadline - std::chrono::steady_clock::now()).count()));
              auto built = prepareNativeCanonicalOnnxRole(ctx, projection, options);
              metrics->sourceFetches.fetch_add(1, std::memory_order_relaxed);
              metrics->assemblies.fetch_add(1, std::memory_order_relaxed);
              auto artifact = std::make_shared<PreparedProviderArtifact>();
              artifact->encryptedObjectName = protectedRuntime
                ? "local-protected-assembled-ciphertext"
                : "local-immutable-assembled-artifact";
              artifact->ciphertextDigest = built.metadata.at("assembledModelDigest");
              artifact->formatVersion = "ndnsf-di-native-assembled-artifact-v1";
              artifact->canonicalMetadataJson =
                std::string("{\"schema\":\"ndnsf-di-provider-artifact-v1\","
                            "\"modelManifestDigest\":\"") +
                projection.assembly.modelManifestDigest +
                "\",\"artifactProfileDigest\":\"" +
                projection.assembly.artifactProfileDigest +
                "\",\"graphDigest\":\"" + projection.assembly.graphDigest +
                "\",\"role\":\"" + projection.assembly.selectedRole +
                "\",\"recipeDigest\":\"" + projection.assembly.recipeDigest +
                "\",\"backendAbi\":\"" + projection.assembly.backendAbi + "\"}";
              if (protectedRuntime) {
                const auto encryptedPath = built.metadata.find("encryptedArtifactPath");
                if (encryptedPath == built.metadata.end() || encryptedPath->second.empty())
                  throw std::runtime_error("DI_PROVIDER_ARTIFACT_CIPHERTEXT_UNAVAILABLE");
                auto ciphertext = std::make_shared<std::vector<std::uint8_t>>(
                  providerReadBounded(encryptedPath->second,
                    projection.assembly.maxAssembledBytes + 65536));
                artifact->ciphertextBytes = ciphertext->size();
                artifact->ciphertext = std::move(ciphertext);
              }
              else {
                artifact->ciphertextBytes = std::filesystem::file_size(built.path);
              }
              return ProviderArtifactCache::BuildResult{
                std::move(artifact),
                std::make_shared<const NativeModelRunnerSpec>(std::move(built))};
            });
          if (!lease.runnerSpec())
            throw std::runtime_error("DI_PROVIDER_ARTIFACT_RUNNER_TEMPLATE_MISSING");
          spec = *lease.runnerSpec();
          if (protectedRuntime) {
            if (!lease->ciphertext)
              throw std::runtime_error("DI_PROVIDER_ARTIFACT_CIPHERTEXT_UNAVAILABLE");
            const auto staging = providerProtectedStaging(cacheDir);
            try {
              registerNativePlaintextDirectory(
                *protectedRuntime, staging,
                "provider-artifact-" + std::to_string(::getpid()));
              const auto profile = std::string("\"ndnsf-di-provider-workdir-scratch-v1\"");
              const NativeAssembledEntryContext context{
                projection.assembly.modelManifestDigest,
                options.roleAssemblySpecDigest,
                sha256TensorBytes(std::vector<std::uint8_t>(profile.begin(), profile.end())),
                "MODEL_PROTO"};
              std::vector<std::uint8_t> plaintext;
              NativePlaintextBufferGuard plaintextGuard{plaintext};
              protectedRuntime->withContentKey(providerNowMs(), [&] (const auto& key) {
                plaintext = openNativeAssembledEntry(
                  key, *lease->ciphertext, context,
                  projection.assembly.maxAssembledBytes);
                providerWriteFile(staging / "model.onnx", plaintext);
              });
              spec.path = (staging / "model.onnx").string();
            }
            catch (...) {
              std::error_code ignored;
              std::filesystem::remove_all(staging, ignored);
              throw;
            }
          }
          else {
            spec.path = providerCachedModelPath(
              cacheDir, projection, lease->ciphertextDigest).string();
            if (!std::filesystem::is_regular_file(spec.path))
              throw std::runtime_error("DI_PROVIDER_ARTIFACT_MATERIALIZATION_MISSING");
          }
          if (lease.cacheHit())
            metrics->templateHits.fetch_add(1, std::memory_order_relaxed);
        }
      }
      if (projection.assembly.mergeKind == "NATIVE_POSTPROCESS")
        metrics->assemblies.fetch_add(1, std::memory_order_relaxed);
      bindNativeRunnerPreparationContext(spec, projection,
        {providerIdentity, providerBootId, providerStartedAtMs, cacheDir});
      return spec;
    };
#if defined(NDNSF_DI_PROVIDER_TEST_SEAM)
  if (m_state->testPreparationFactory) {
    auto preparationFactory = m_state->testPreparationFactory;
    auto metrics = m_state->metrics;
    nativeConfig.runnerPreparationFactory =
      [preparationFactory = std::move(preparationFactory), metrics = std::move(metrics)] (
        ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
        const NativeSelectionProjectionV3& projection,
        const std::shared_ptr<ProtectedRuntime>& protectedRuntime) {
        auto spec = preparationFactory(ctx, projection, protectedRuntime);
        metrics->assemblies.fetch_add(1, std::memory_order_relaxed);
        return spec;
      };
  }
  if (m_state->testProtectedRuntimeFactory)
    nativeConfig.protectedRuntimeFactory = m_state->testProtectedRuntimeFactory;
#endif
  NativeServiceDefinition definition;
  definition.serviceName = canonicalServiceName;
  definition.allowedRoles.assign(roles.begin(), roles.end());
#if defined(NDNSF_DI_PROVIDER_TEST_SEAM)
  definition.ackHandler = m_state->testAckHandler;
#endif
  std::shared_ptr<NativeServiceRegistration> native;
  try {
    std::shared_ptr<NativeInferenceProvider> nativeHost;
    {
      std::lock_guard<std::mutex> lock(m_state->mutex);
      if (m_state->stopped)
        throw DiError("PROVIDER_SERVE_FAILED", "local", "provider", "Provider is stopped");
      nativeHost = m_state->nativeHost;
    }
    if (!nativeHost)
      throw DiError("PROVIDER_SERVE_FAILED", "local", "provider",
                    "Provider native host is unavailable");
    struct ServeInvocation
    {
      std::shared_ptr<State> state;
      std::shared_ptr<NativeInferenceProvider> nativeHost;
      NativeServiceDefinition definition;
      NativeProviderHandlerConfig config;
    };
    auto invocation = std::make_shared<ServeInvocation>(ServeInvocation{
      m_state, nativeHost, definition, std::move(nativeConfig)});
    auto invokeNativeServe = [invocation] {
      {
        std::lock_guard<std::mutex> lock(invocation->state->mutex);
        if (invocation->state->stopped)
          throw DiError("PROVIDER_SERVE_FAILED", "local", "provider", "Provider is stopped");
      }
      return std::make_shared<NativeServiceRegistration>(
        invocation->nativeHost->serve(invocation->definition, invocation->config));
    };
    bool onFaceThread = false;
    bool dispatchToFace = false;
    {
      std::unique_lock<std::mutex> lock(m_state->ioMutex);
      if (m_state->ioThread.joinable() && !m_state->ioRunning && !m_state->ioStopped &&
          !m_state->ioFailed) {
        m_state->ioCondition.wait(lock, [state = m_state] {
          return state->ioRunning || state->ioStopped || state->ioFailed;
        });
      }
      onFaceThread = std::this_thread::get_id() == m_state->ioThreadId;
      dispatchToFace = m_state->ioRunning && !onFaceThread;
      if (m_state->ioFailed)
        throw DiError("PROVIDER_SERVE_FAILED", "local", "provider",
                      "Provider Face IO loop has failed");
      if (m_state->ioStopping || m_state->ioStopped)
        throw DiError("PROVIDER_SERVE_FAILED", "local", "provider",
                      "Provider Face IO loop is stopping");
      if (!m_state->ownsIoContext &&
          m_state->face->getIoContext().stopped())
        throw DiError("PROVIDER_SERVE_FAILED", "local", "provider",
                      "borrowed Provider Face IO context is stopped");
    }
    if (!dispatchToFace || onFaceThread) {
      native = invokeNativeServe();
    }
    else {
      const auto call = std::make_shared<ProviderFaceServeCall>();
      {
        std::lock_guard<std::mutex> ioLock(m_state->ioMutex);
        if (m_state->ioStopping || m_state->ioStopped || m_state->ioFailed)
          throw DiError("PROVIDER_SERVE_FAILED", "local", "provider",
                        "Provider Face IO loop is stopping");
        m_state->pendingServeCalls.push_back(call);
      }
      try {
        boost::asio::post(m_state->face->getIoContext(), [call, invokeNativeServe] () mutable {
          std::shared_ptr<NativeServiceRegistration> result;
          std::exception_ptr error;
          if (!call->cancelled.load(std::memory_order_acquire)) {
            try {
              result = invokeNativeServe();
              if (call->cancelled.load(std::memory_order_acquire)) {
                if (result)
                  result->close();
                error = std::make_exception_ptr(
                  DiError("PROVIDER_SERVE_FAILED", "local", "provider",
                          "Provider Face IO loop is stopping"));
              }
            }
            catch (...) { error = std::current_exception(); }
          }
          else {
            error = std::make_exception_ptr(
              DiError("PROVIDER_SERVE_FAILED", "local", "provider",
                      "Provider Face IO loop is stopping"));
          }
          {
            std::lock_guard<std::mutex> lock(call->mutex);
            if (!call->done) {
              call->result = std::move(result);
              call->error = std::move(error);
            }
            call->done = true;
          }
          call->condition.notify_all();
        });
      }
      catch (...) {
        std::lock_guard<std::mutex> ioLock(m_state->ioMutex);
        m_state->pendingServeCalls.erase(
          std::remove(m_state->pendingServeCalls.begin(),
                      m_state->pendingServeCalls.end(), call),
          m_state->pendingServeCalls.end());
        throw;
      }
      // The Face callback is serialized by the Face itself.  Do not hold the
      // admission mutex while waiting for it: a concurrent stop() or a
      // second serve() must be able to cancel/complete its own admission.
      serveLock.unlock();
      std::unique_lock<std::mutex> lock(call->mutex);
      call->condition.wait(lock, [call] { return call->done; });
      lock.unlock();
      serveLock.lock();
      {
        std::lock_guard<std::mutex> ioLock(m_state->ioMutex);
        m_state->pendingServeCalls.erase(
          std::remove(m_state->pendingServeCalls.begin(),
                      m_state->pendingServeCalls.end(), call),
          m_state->pendingServeCalls.end());
      }
      if (call->error)
        std::rethrow_exception(call->error);
      native = std::move(call->result);
    }
  }
  catch (const std::logic_error& error) {
    if (std::string(error.what()).find("already registered") != std::string::npos)
      throw DiError("SERVICE_ALREADY_REGISTERED", "local", "provider",
                    error.what());
    throw DiError("PROVIDER_SERVE_FAILED", "local", "provider", error.what());
  }
  catch (const std::exception& error) {
    throw DiError("PROVIDER_SERVE_FAILED", "local", "provider", error.what());
  }
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->stopped) {
      if (native)
        native->close();
      throw DiError("PROVIDER_SERVE_FAILED", "local", "provider",
                    "Provider stopped while service was being registered");
    }
  }
  auto registration = std::make_shared<ProviderRegistration::State>();
  registration->serviceName = service.serviceName;
  registration->native = std::move(native);
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->registrations.push_back(registration);
  }
  try {
    startIo();
  }
  catch (...) {
    registration->native->close();
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->registrations.erase(
      std::remove(m_state->registrations.begin(), m_state->registrations.end(), registration),
      m_state->registrations.end());
    try {
      throw;
    }
    catch (const DiError&) {
      throw;
    }
    catch (const std::exception& error) {
      throw DiError("PROVIDER_SERVE_FAILED", "local", "provider", error.what());
    }
    catch (...) {
      throw DiError("PROVIDER_SERVE_FAILED", "local", "provider",
                    "unknown provider start failure");
    }
  }
  return ProviderRegistration(std::move(registration));
}

void Provider::stop() const noexcept
{
  if (!m_state)
    return;
  bool needNativeStop = false;
  std::shared_ptr<NativeInferenceProvider> nativeHost;
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (!m_state->stopped) {
      m_state->stopped = true;
      needNativeStop = true;
    }
    nativeHost = m_state->nativeHost;
  }
  if (needNativeStop && nativeHost)
    nativeHost->stop();
  if (m_state->artifactCache)
    m_state->artifactCache->stop();
  requestStopIo();
}

bool Provider::drain(Milliseconds timeout) const
{
  if (timeout.count() < 0)
    throw std::invalid_argument("Provider drain timeout must not be negative");
  if (!m_state)
    return true;
  std::shared_ptr<NativeInferenceProvider> nativeHost;
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (!m_state->stopped) {
      m_state->stopped = true;
    }
    nativeHost = m_state->nativeHost;
  }
  if (nativeHost)
    nativeHost->stop();
  if (m_state->artifactCache)
    m_state->artifactCache->stop();
  const auto drained = waitForIoBarrier(timeout);
  if (!drained)
    return false;
  const auto stopped = stopIo();
  if (stopped)
    releaseStoppedResources();
  return stopped;
}

Subscription Provider::drainAsync(
  Milliseconds timeout, std::function<void(std::exception_ptr, bool)> callback) const
{
  if (timeout.count() < 0 || !callback)
    throw std::invalid_argument("Provider drainAsync arguments are invalid");
  if (!m_state || !m_state->asyncRuntime)
    throw std::runtime_error("Provider has no lifecycle operation runtime");

  struct Result
  {
    std::mutex mutex;
    std::exception_ptr error;
    bool value = false;
  };
  const auto result = std::make_shared<Result>();
  const auto ticket = std::make_shared<ndn_service_framework::OperationRuntime::WorkTicket>(
    m_state->asyncRuntime->acquire());
  const Provider self(m_state);
  m_state->asyncRuntime->post(*ticket, [self, ticket, timeout, result] {
    try {
      const auto value = self.drain(timeout);
      std::lock_guard<std::mutex> lock(result->mutex);
      result->value = value;
    }
    catch (...) {
      std::lock_guard<std::mutex> lock(result->mutex);
      result->error = std::current_exception();
    }
  });
  auto wrapped = [result, callback = std::move(callback)](bool runtimeDrained) mutable {
    std::exception_ptr error;
    bool value = false;
    {
      std::lock_guard<std::mutex> lock(result->mutex);
      error = result->error;
      value = result->value;
    }
    if (!runtimeDrained && !error) {
      try { callback(nullptr, false); } catch (...) {}
      return;
    }
    try { callback(error, error ? false : value); } catch (...) {}
  };
  return m_state->asyncRuntime->drainAsync(timeout, std::move(wrapped), false);
}

bool Provider::valid() const noexcept
{
  return m_state != nullptr;
}

ProviderCounters Provider::counters() const noexcept
{
  ProviderCounters result;
  if (!m_state || !m_state->metrics)
    return result;
  result.sourceFetches = m_state->metrics->sourceFetches.load(std::memory_order_relaxed);
  result.assemblies = m_state->metrics->assemblies.load(std::memory_order_relaxed);
  result.templateHits = m_state->metrics->templateHits.load(std::memory_order_relaxed);
  result.runnersCreated = m_state->metrics->runnersCreated.load(std::memory_order_relaxed);
  if (m_state->artifactCache)
    result.activeLeases = m_state->artifactCache->counters().activeLeases;
  return result;
}

} // namespace ndnsf::di
