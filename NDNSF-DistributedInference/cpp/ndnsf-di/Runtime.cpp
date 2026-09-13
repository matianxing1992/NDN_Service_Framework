#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ModelPreparationCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModel.hpp"
#include "ndn-service-framework/OperationRuntime.hpp"
#include "ndn-service-framework/common.hpp"

#include <ndn-cxx/face.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <atomic>
#include <thread>
#include <utility>

namespace ndnsf::di {
namespace {

constexpr std::size_t MAX_RUNTIME_CONFIG_BYTES = 4 * 1024 * 1024;

std::string readConfig(const std::filesystem::path& path)
{
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec)
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "configuration",
                  "native runtime configuration file is unavailable");
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size == 0 || size > MAX_RUNTIME_CONFIG_BYTES)
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "configuration",
                  "native runtime configuration size is invalid");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "configuration",
                  "native runtime configuration cannot be opened");
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "configuration",
                  "native runtime configuration cannot be read");
  return bytes;
}

const NativeJson& requiredObject(const NativeJson& root, const char* name)
{
  if (!root.is_object() || !root.contains(name) || !root.at(name).is_object())
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "configuration",
                  std::string("native runtime configuration requires object ") + name);
  return root.at(name);
}

std::string requiredString(const NativeJson& object, const char* name)
{
  if (!object.contains(name) || !object.at(name).is_string())
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "configuration",
                  std::string("native runtime configuration requires string ") + name);
  const auto value = object.at(name).get<std::string>();
  if (value.empty())
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "configuration",
                  std::string("native runtime configuration has empty ") + name);
  return value;
}

bool isDigest(const std::string& value)
{
  if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0)
    return false;
  return std::all_of(value.begin() + 7, value.end(), [] (unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

std::uint64_t positiveUnsigned(const NativeJson& object, const char* name)
{
  if (!object.contains(name) || !object.at(name).is_number_unsigned())
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "limits",
                  std::string("runtime limit requires unsigned ") + name);
  const auto value = object.at(name).get<std::uint64_t>();
  if (value == 0)
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "limits",
                  std::string("runtime limit must be positive: ") + name);
  return value;
}

bool absoluteName(const std::string& value)
{
  if (value.size() < 2 || value.front() != '/' ||
      std::any_of(value.begin(), value.end(), [] (unsigned char c) {
        return c < 0x20 || c == 0x7f || c == '\\';
      }))
    return false;
  try {
    return !ndn::Name(value).empty();
  }
  catch (...) {
    return false;
  }
}

int rejectPemPassword(char*, int, int, void*)
{
  return 0;
}

struct SensitiveBuffer
{
  explicit SensitiveBuffer(std::string value)
    : value(std::move(value))
  {
  }
  ~SensitiveBuffer()
  {
    if (!value.empty())
      OPENSSL_cleanse(value.data(), value.size());
  }
  std::string value;
};

std::string readOperatorFile(const std::filesystem::path& path, std::size_t limit)
{
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size == 0 || size > limit)
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "trust",
                  "configured operator file size is invalid");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "trust",
                  "configured operator file cannot be opened");
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "trust",
                  "configured operator file cannot be read");
  return bytes;
}

std::shared_ptr<EVP_PKEY> loadEd25519PrivateKey(const std::filesystem::path& path)
{
  SensitiveBuffer bytes(readOperatorFile(path, 65536));
  BIO* bio = BIO_new_mem_buf(bytes.value.data(), static_cast<int>(bytes.value.size()));
  EVP_PKEY* raw = bio == nullptr ? nullptr :
    PEM_read_bio_PrivateKey(bio, nullptr, rejectPemPassword, nullptr);
  if (bio != nullptr)
    BIO_free(bio);
  if (raw == nullptr || EVP_PKEY_base_id(raw) != EVP_PKEY_ED25519) {
    if (raw != nullptr)
      EVP_PKEY_free(raw);
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "trust",
                  "requester private key is not an Ed25519 PEM key");
  }
  return {raw, EVP_PKEY_free};
}

std::shared_ptr<EVP_PKEY> loadEd25519PublicKey(const std::filesystem::path& path)
{
  const auto bytes = readOperatorFile(path, 65536);
  BIO* bio = BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size()));
  EVP_PKEY* raw = bio == nullptr ? nullptr :
    PEM_read_bio_PUBKEY(bio, nullptr, rejectPemPassword, nullptr);
  if (bio != nullptr)
    BIO_free(bio);
  if (raw == nullptr || EVP_PKEY_base_id(raw) != EVP_PKEY_ED25519) {
    if (raw != nullptr)
      EVP_PKEY_free(raw);
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "trust",
                  "authority public key is not an Ed25519 PEM key");
  }
  return {raw, EVP_PKEY_free};
}

std::shared_ptr<ndn_service_framework::MessageValidator>
loadTrustSchema(const std::filesystem::path& path, const std::string& group,
                ndn::Face* callbackFace)
{
  try {
    // Constructing the existing native validator is the single trust-schema
    // parser. Local loading is synchronous while later certificate fetches
    // remain bound to Runtime's Face and IO context.
    return std::make_shared<ndn_service_framework::MessageValidator>(
      path.string(), ndn::Name(group), callbackFace);
  }
  catch (const std::exception& error) {
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "trust",
                  std::string("trust schema cannot be loaded: ") + error.what());
  }
}

struct FrozenConfig
{
  std::filesystem::path path;
  std::string canonicalJson;
  std::string configurationDigest;
  std::string requesterIdentity;
  std::string coreAuthorityIdentity;
  std::string group;
  std::filesystem::path trustSchema;
  std::string authorityIdentity;
  std::string authorityService;
  std::string protectionEpoch;
  std::shared_ptr<EVP_PKEY> requesterPrivateKey;
  std::shared_ptr<EVP_PKEY> authorityPublicKey;
  std::shared_ptr<ndn_service_framework::MessageValidator> trustValidator;
  std::shared_ptr<const NativeOfferAdmission> offerAdmission;
};

PreparationSpec preparationSpec(const FrozenConfig& frozen, const std::string& key)
{
  const auto root = nativeParseJson(frozen.canonicalJson);
  const auto& request = root.at("request");
  const auto& limits = root.at("limits");
  const auto& catalog = root.at("catalog");
  const auto taskName = requiredString(request, "task");
  const auto taskContractDigest = requiredString(request, "task_descriptor_digest");
  const auto inputLayoutDigest = requiredString(request, "input_layout_digest");
  PreparationSpec spec;
  spec.key = key;
  spec.baseDirectory = frozen.path.parent_path();
  spec.configurationJson = frozen.canonicalJson;
  spec.catalogConfigurationJson = nativeCanonicalJson(catalog);
  spec.taskName = taskName;
  spec.taskContractDigest = taskContractDigest;
  spec.inputLayoutDigest = inputLayoutDigest;
  spec.configurationDigest = frozen.configurationDigest;
  const auto positiveLimit = [&] (const char* field) {
    if (!limits.contains(field) || !limits.at(field).is_number_unsigned() ||
        limits.at(field).get<std::uint64_t>() == 0)
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "limits",
                    std::string("runtime limit must be positive: ") + field);
    return limits.at(field).get<std::uint64_t>();
  };
  spec.maxSourceBytes = positiveLimit("max_source_bytes");
  spec.maxAssembledBytes = positiveLimit("max_assembled_bytes");
  spec.loadSource = [] (const PreparationSpec& current,
                        std::chrono::steady_clock::time_point deadline) {
    try {
      if (std::chrono::steady_clock::now() >= deadline)
        throw DiError("PREPARATION_TIMEOUT", "local", "preparation", "preparation deadline expired");
      const auto catalog = nativeParseJson(current.catalogConfigurationJson);
      const auto& source = catalog.at("source");
      const auto sourceFile = requiredString(source, "file");
      NativeCanonicalSource result;
      const auto modelPath = (current.baseDirectory / sourceFile).lexically_normal();
      const auto model = readOperatorFile(modelPath, current.maxSourceBytes);
      result.modelBytes.assign(model.begin(), model.end());
      if (source.contains("initializer_file")) {
        const auto initializerFile = requiredString(source, "initializer_file");
        const auto initializerPath = (current.baseDirectory / initializerFile).lexically_normal();
        const auto initializer = readOperatorFile(initializerPath, current.maxSourceBytes);
        result.initializerBytes.emplace(initializer.begin(), initializer.end());
      }
      if (std::chrono::steady_clock::now() >= deadline)
        throw DiError("PREPARATION_TIMEOUT", "local", "preparation", "preparation deadline expired");
      return result;
    }
    catch (const DiError& error) {
      if (error.code() == "PREPARATION_TIMEOUT")
        throw;
      throw DiError("PREPARATION_SOURCE_UNAVAILABLE", "local", "preparation", error.what());
    }
  };
  return spec;
}

FrozenConfig freezeConfig(const std::filesystem::path& path, ndn::Face* callbackFace)
{
  const auto canonicalPath = std::filesystem::absolute(path).lexically_normal();
  NativeJson root;
  try {
    root = nativeParseJson(readConfig(canonicalPath));
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "configuration",
                  std::string("native requester JSON is invalid: ") + error.what());
  }

  if (!root.is_object() || !root.contains("schema") || !root.at("schema").is_string() ||
      root.at("schema").get<std::string>() != "ndnsf-di-native-requester-v1") {
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "schema",
                  "unsupported native requester configuration schema");
  }
  const auto& core = requiredObject(root, "core");
  const auto& grant = requiredObject(root, "grant");
  const auto& offerAdmission = requiredObject(root, "offer_admission");
  const auto& limits = requiredObject(root, "limits");
  const auto& request = requiredObject(root, "request");
  const auto bootstrapMs = positiveUnsigned(limits, "bootstrap_ms");
  if (bootstrapMs > 3600000)
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "limits",
                  "bootstrap_ms exceeds one hour");
  const auto bootstrapDeadline = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(bootstrapMs);
  const auto requireBootstrapBudget = [&] {
    if (std::chrono::steady_clock::now() >= bootstrapDeadline)
      throw DiError("PREPARATION_TIMEOUT", "local", "bootstrap",
                    "Runtime bootstrap exceeded bootstrap_ms");
  };
  const auto& catalog = requiredObject(root, "catalog");
  const auto& source = requiredObject(catalog, "source");
  (void)requiredString(source, "file");

  const auto requesterIdentity = requiredString(core, "requester_identity");
  const auto coreAuthorityIdentity = requiredString(core, "authority_identity");
  if (!absoluteName(requesterIdentity) || !absoluteName(coreAuthorityIdentity))
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "identity",
                  "core identities must be absolute NDN names");
  const auto group = requiredString(core, "group");
  if (!absoluteName(group))
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "identity",
                  "group must be an absolute name");
  const auto trustPath = requiredString(core, "trust_schema_file");
  const auto trustSchema = (canonicalPath.parent_path() / trustPath).lexically_normal();
  std::error_code ec;
  requireBootstrapBudget();
  if (!std::filesystem::is_regular_file(trustSchema, ec) || ec ||
      std::filesystem::file_size(trustSchema, ec) == 0 || ec) {
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "trust",
                  "trust schema file is unavailable");
  }

  const auto authorityIdentity = requiredString(grant, "authority_identity");
  const auto authorityService = requiredString(grant, "authority_service");
  const auto protectionEpoch = requiredString(grant, "protection_epoch");
  if (!absoluteName(authorityIdentity) || !absoluteName(authorityService))
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "identity",
                  "grant authority identity and service must be absolute NDN names");
  const auto requesterKey = requiredString(grant, "requester_private_key_file");
  const auto authorityPublicKey = requiredString(grant, "authority_public_key_file");
  for (const auto* field : {"authority_private_key_file", "content_key_file", "content_key_id"}) {
    if (grant.contains(field))
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "trust",
                    "authority signing or content keys are not accepted by Runtime");
  }
  const auto requesterKeyPath = (canonicalPath.parent_path() / requesterKey).lexically_normal();
  const auto authorityPublicKeyPath = (canonicalPath.parent_path() / authorityPublicKey).lexically_normal();
  for (const auto& candidate : {requesterKeyPath, authorityPublicKeyPath}) {
    if (!std::filesystem::is_regular_file(candidate, ec) || ec)
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "trust",
                    "configured requester or authority public key is unavailable");
  }
  requireBootstrapBudget();
  auto requesterPrivate = loadEd25519PrivateKey(requesterKeyPath);
  auto authorityPublic = loadEd25519PublicKey(authorityPublicKeyPath);
  requireBootstrapBudget();
  auto trustValidator = loadTrustSchema(trustSchema, group, callbackFace);
  requireBootstrapBudget();

  const auto& offerPolicy = requiredObject(offerAdmission, "policy");
  const auto& offerKeys = requiredObject(offerAdmission, "public_key_files");
  if (offerKeys.empty())
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "offer-admission",
                  "offer admission requires at least one public key");
  const auto candidateDigest = requiredString(offerAdmission, "candidate_digest");
  if (!isDigest(candidateDigest))
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "offer-admission",
                  "offer admission candidate_digest is not a sha256 digest");
  std::map<std::string, std::string> offerKeyPemById;
  for (const auto& entry : offerKeys.items()) {
    const auto keyId = entry.key();
    const auto keyFile = requiredString(offerKeys, keyId.c_str());
    const auto keyPath = (canonicalPath.parent_path() / keyFile).lexically_normal();
    if (!std::filesystem::is_regular_file(keyPath, ec) || ec)
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "offer-admission",
                    "offer admission public key file is unavailable");
    (void)loadEd25519PublicKey(keyPath);
    offerKeyPemById.emplace(keyId, readOperatorFile(keyPath, 65536));
  }
  std::shared_ptr<const NativeOfferAdmission> verifiedOfferAdmission;
  try {
    requireBootstrapBudget();
    verifiedOfferAdmission = std::make_shared<NativeOfferAdmission>(
      nativeCanonicalJson(offerPolicy), offerKeyPemById, candidateDigest);
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "offer-admission",
                  std::string("offer admission policy is invalid: ") + error.what());
  }

  positiveUnsigned(limits, "max_source_bytes");
  positiveUnsigned(limits, "max_assembled_bytes");
  const auto service = requiredString(request, "service");
  if (!absoluteName(service))
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "request",
                  "request service must be an absolute name");
  (void)requiredString(request, "task");
  for (const auto* digest : {"adapter_composition_digest", "task_descriptor_digest",
                             "input_layout_digest", "security_policy_digest"}) {
    if (!isDigest(requiredString(request, digest)))
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "request",
                    std::string("request field is not a sha256 digest: ") + digest);
  }
  const auto timeoutMs = positiveUnsigned(request, "timeout_ms");
  const auto ackTimeoutMs = positiveUnsigned(request, "ack_timeout_ms");
  if (timeoutMs > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
      ackTimeoutMs >= timeoutMs)
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "request",
                  "request timeout or ACK timeout is out of range");
  const auto boundedUnsigned = [&] (const char* name, std::uint64_t fallback,
                                    std::uint64_t maximum, bool requirePositive) {
    if (!request.contains(name))
      return fallback;
    if (!request.at(name).is_number_unsigned())
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "request",
                    std::string("request field is not unsigned: ") + name);
    const auto value = request.at(name).get<std::uint64_t>();
    if ((requirePositive && value == 0) || value > maximum)
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "request",
                    std::string("request field is out of range: ") + name);
    return value;
  };
  if (!request.contains("max_candidates") || !request.contains("max_policy_ms"))
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "request",
                  "request candidate budget requires max_candidates and max_policy_ms");
  const auto maxCandidates = boundedUnsigned("max_candidates", 1, 1024, true);
  const auto maxPolicyMs = boundedUnsigned("max_policy_ms", 100, 60'000, true);
  const auto maxReentries = boundedUnsigned("max_reentries", 1, 16, false);
  const auto noProgressMs = boundedUnsigned("no_progress_ms", 5000,
                                             24ULL * 60ULL * 60ULL * 1000ULL, true);
  const auto maxSegments = boundedUnsigned("max_segments", 4096, 1ULL << 20, true);
  std::string generationMode = "TOKEN_DIAGNOSTIC";
  if (request.contains("generation_mode"))
    generationMode = requiredString(request, "generation_mode");
  if (generationMode != "TOKEN_DIAGNOSTIC" && generationMode != "TOKEN_STREAMING")
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "request",
                  "request generation_mode is unsupported");
  if (request.contains("tokenizer_digest")) {
    const auto tokenizerDigest = requiredString(request, "tokenizer_digest");
    if (!isDigest(tokenizerDigest))
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "request",
                    "request tokenizer_digest is not a sha256 digest");
  }
  if (generationMode == "TOKEN_STREAMING" &&
      (!request.contains("tokenizer_digest") ||
       !isDigest(requiredString(request, "tokenizer_digest"))))
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "request",
                  "TOKEN_STREAMING requires a tokenizer digest");
  if (protectionEpoch == "plaintext-v1")
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "trust",
                  "protected Runtime cannot use plaintext-v1 protection epoch");
  if (request.contains("application_request_id")) {
    if (!request.at("application_request_id").is_string())
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "request",
                    "application_request_id must be a string");
    const auto applicationRequestId = request.at("application_request_id").get<std::string>();
    if (applicationRequestId.size() > 256 || applicationRequestId.find('\0') != std::string::npos)
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "request",
                    "application_request_id exceeds 256 bytes or contains NUL");
  }
  requireBootstrapBudget();

  // Materialize the same defaults that the native requester runtime builder
  // emits.  The frozen identity and its digest therefore describe the exact
  // contract consumed by T002+, rather than a source JSON with omitted keys.
  auto normalized = root;
  auto& normalizedRequest = normalized["request"];
  normalizedRequest["max_candidates"] = maxCandidates;
  normalizedRequest["max_policy_ms"] = maxPolicyMs;
  normalizedRequest["max_reentries"] = maxReentries;
  normalizedRequest["no_progress_ms"] = noProgressMs;
  normalizedRequest["max_segments"] = maxSegments;
  normalizedRequest["generation_mode"] = generationMode;
  if (!normalizedRequest.contains("tokenizer_digest"))
    normalizedRequest["tokenizer_digest"] = std::string{};
  const auto canonicalJson = nativeCanonicalJson(normalized);

  return FrozenConfig{canonicalPath, canonicalJson,
                      nativePlanningDigest(canonicalJson), requesterIdentity,
                      coreAuthorityIdentity, group, trustSchema, authorityIdentity,
                      authorityService, protectionEpoch, std::move(requesterPrivate),
                      std::move(authorityPublic), std::move(trustValidator),
                      std::move(verifiedOfferAdmission)};
}

bool sameTrustDomain(const FrozenConfig& a, const FrozenConfig& b)
{
  return a.requesterIdentity == b.requesterIdentity &&
         a.coreAuthorityIdentity == b.coreAuthorityIdentity && a.group == b.group &&
         a.trustSchema == b.trustSchema && a.authorityIdentity == b.authorityIdentity &&
         a.authorityService == b.authorityService && a.protectionEpoch == b.protectionEpoch;
}

} // namespace

namespace detail {

/**
 * Runtime-owned Core boundary.  T001 establishes the ownership graph so the
 * public shell never owns a borrowed Face or key.  T002 adds the lifecycle
 * barrier around this owner; domain ServiceUser/client registries are added
 * by the batches that introduce those operations.
 */
struct CoreRuntimeOwner
{
  boost::asio::io_context io;
  std::shared_ptr<ndn::Face> face;
  std::shared_ptr<EVP_PKEY> requesterPrivateKey;
  std::shared_ptr<EVP_PKEY> authorityPublicKey;
  std::shared_ptr<ndn_service_framework::OperationRuntime> operationRuntime;
  std::atomic<std::size_t> inFlight{0};
  std::atomic<bool> closed{false};

  void close() noexcept
  {
    if (closed.exchange(true))
      return;
    if (operationRuntime)
      operationRuntime->close();
  }

  void stopIo() noexcept
  {
    io.stop();
  }

  ~CoreRuntimeOwner() noexcept
  {
    close();
    stopIo();
    // Face is destroyed after the IO owner is stopped.  No callback may
    // observe a partially destroyed Core dependency graph.
    face.reset();
    authorityPublicKey.reset();
    requesterPrivateKey.reset();
    operationRuntime.reset();
  }
};

struct RuntimeState
{
  enum class Phase { Open, Closing, Drained };

  mutable std::mutex mutex;
  std::condition_variable cv;
  std::atomic<std::size_t> preparationInFlight{0};
  Phase phase = Phase::Open;
  // Domain work tickets are accounted by the Core runtime.  This counter is
  // reserved for DI-owned preparation/request tickets added by later batches;
  // it is never used as a substitute for the Core ticket barrier.
  std::size_t inFlight = 0;
  // Declared first so models/trust validators are destroyed before Face/IO.
  std::shared_ptr<CoreRuntimeOwner> coreOwner;
  std::shared_ptr<ModelPreparationCache> preparationCache;
  RuntimeConfig config;
  std::filesystem::path baseDirectory;
  std::map<std::string, FrozenConfig> models;
};

struct PreparationTicket
{
  std::shared_ptr<RuntimeState> state;
  bool armed = false;

  explicit PreparationTicket(std::shared_ptr<RuntimeState> state)
    : state(std::move(state))
  {
  }

  ~PreparationTicket() noexcept
  {
    if (!state)
      return;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (armed && state->inFlight != 0)
      --state->inFlight;
    if (armed)
      state->preparationInFlight.fetch_sub(1, std::memory_order_release);
    if (armed)
      state->cv.notify_all();
    if (armed && state->coreOwner && state->coreOwner->operationRuntime)
      state->coreOwner->operationRuntime->notifyWaiters();
  }
};

} // namespace detail

DiError::DiError(std::string code, std::string domain, std::string boundary,
                 std::string message, std::string requestId, std::uint64_t attempt)
  : std::runtime_error(std::move(message)), m_code(std::move(code)),
    m_domain(std::move(domain)), m_boundary(std::move(boundary)),
    m_requestId(std::move(requestId)), m_attempt(attempt)
{
}

namespace {

DiError mapPreparationError(const std::exception& error)
{
  if (const auto* operation = dynamic_cast<const ndn_service_framework::OperationError*>(&error)) {
    switch (operation->code()) {
    case ndn_service_framework::OperationErrorCode::Closed:
      return DiError("RUNTIME_CLOSED", "local", "preparation", error.what());
    case ndn_service_framework::OperationErrorCode::Timeout:
      return DiError("PREPARATION_TIMEOUT", "local", "preparation", error.what());
    case ndn_service_framework::OperationErrorCode::Cancelled:
      return DiError("PREPARATION_CANCELLED", "local", "preparation", error.what());
    default:
      break;
    }
  }
  const std::string message = error.what();
  if (message.find("RESULT_TIMEOUT") != std::string::npos)
    return DiError("WAIT_TIMEOUT", "local", "preparation", message);
  if (message.find("CANCELLED") != std::string::npos)
    return DiError("PREPARATION_CANCELLED", "local", "preparation", message);
  if (message.find("TIMEOUT") != std::string::npos)
    return DiError("PREPARATION_TIMEOUT", "local", "preparation", message);
  if (message.find("MODEL_NOT_READY") != std::string::npos)
    return DiError("MODEL_NOT_READY", "local", "preparation", message);
  if (message.find("PREPARATION_NOT_IN_FLIGHT") != std::string::npos)
    return DiError("PREPARATION_NOT_IN_FLIGHT", "local", "preparation", message);
  if (message.find("SUBSCRIPTION_LIMIT") != std::string::npos)
    return DiError("SUBSCRIPTION_LIMIT", "local", "preparation", message);
  if (message.find("must not be negative") != std::string::npos)
    return DiError("INVALID_ARGUMENT", "local", "preparation", message);
  if (message.find("timeout must be positive") != std::string::npos)
    return DiError("INVALID_ARGUMENT", "local", "preparation", message);
  if (message.find("BUDGET_EXCEEDED") != std::string::npos)
    return DiError("CACHE_BUDGET_EXCEEDED", "local", "preparation", message);
  if (message.find("UNSUPPORTED_CAPABILITY") != std::string::npos ||
      message.find("ADAPTER_UNAVAILABLE") != std::string::npos)
    return DiError("UNSUPPORTED_CAPABILITY", "local", "preparation", message);
  if (message.find("identity") != std::string::npos ||
      message.find("configuration") != std::string::npos ||
      message.find("JSON") != std::string::npos ||
      message.find("ONNX") != std::string::npos ||
      message.find("initializer") != std::string::npos ||
      message.find("digest") != std::string::npos)
    return DiError("SOURCE_IDENTITY_MISMATCH", "local", "preparation", message);
  return DiError("PREPARATION_FAILED", "local", "preparation", message);
}

} // namespace

PreparationStatus PreparationHandle::status() const
{
  return m_state && m_state->status ? m_state->status() : PreparationStatus::Cancelled;
}

PreparedModel PreparationHandle::result() const
{
  if (!m_state || !m_state->result)
    throw DiError("RUNTIME_CLOSED", "local", "preparation", "Preparation handle is empty");
  try {
    const bool onWorker = m_state->workerThread && m_state->workerThread();
    if (onWorker && m_state->wouldBlock && m_state->wouldBlock())
      throw DiError("WOULD_DEADLOCK", "local", "preparation",
                    "blocking preparation result from Core worker would deadlock");
    if (m_state->resultDefault)
      return m_state->resultDefault();
    return m_state->result(m_state->defaultTimeout);
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw mapPreparationError(error);
  }
}

PreparedModel PreparationHandle::result(Milliseconds timeout) const
{
  if (!m_state || !m_state->result)
    throw DiError("RUNTIME_CLOSED", "local", "preparation", "Preparation handle is empty");
  try {
    const bool onWorker = timeout.count() != 0 && m_state->workerThread &&
      m_state->workerThread();
    if (onWorker && m_state->wouldBlock && m_state->wouldBlock())
      throw DiError("WOULD_DEADLOCK", "local", "preparation",
                    "blocking preparation result from Core worker would deadlock");
    return m_state->result(timeout);
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw mapPreparationError(error);
  }
}

Subscription PreparationHandle::resultAsync(Milliseconds timeout,
                                            PreparationCompletion callback) const
{
  if (!m_state || !m_state->resultAsync)
    throw DiError("RUNTIME_CLOSED", "local", "preparation", "Preparation handle is empty");
  return m_state->resultAsync(timeout, std::move(callback));
}

Subscription PreparationHandle::onCompletion(PreparationCompletion callback) const
{
  if (!m_state || !m_state->onCompletion)
    throw DiError("RUNTIME_CLOSED", "local", "preparation", "Preparation handle is empty");
  return m_state->onCompletion(std::move(callback));
}

void PreparationHandle::cancel() const noexcept
{
  if (m_state && m_state->cancel)
    m_state->cancel();
}

User::User(std::shared_ptr<detail::RuntimeState> state, std::string profileName)
  : m_state(std::move(state)), m_profileName(std::move(profileName))
{
}

PreparedModel User::prepare(const std::string& modelKey, const PrepareOptions& options) const
{
  if (!m_state)
    throw DiError("RUNTIME_CLOSED", "local", "preparation", "User has no Runtime state");
  std::shared_ptr<ModelPreparationCache> cache;
  FrozenConfig frozen;
  detail::PreparationTicket ticket(m_state);
  // Check the Core executor before taking the Runtime mutex.  A worker-thread
  // caller must fail immediately even if another thread is closing the state.
  if (m_state->coreOwner && m_state->coreOwner->operationRuntime &&
      m_state->coreOwner->operationRuntime->isWorkerThread())
    throw DiError("WOULD_DEADLOCK", "local", "preparation",
                  "blocking preparation from Core worker would deadlock");
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->phase != detail::RuntimeState::Phase::Open)
      throw DiError("RUNTIME_CLOSED", "local", "preparation", "Runtime is closed");
    const auto found = m_state->models.find(modelKey);
    if (found == m_state->models.end())
      throw DiError("MODEL_NOT_FOUND", "local", "preparation", "registered model key is unknown");
    cache = m_state->preparationCache;
    frozen = found->second;
    ++m_state->inFlight;
    m_state->preparationInFlight.fetch_add(1, std::memory_order_acq_rel);
    ticket.armed = true;
  }
  if (!cache)
    throw DiError("RUNTIME_CLOSED", "local", "preparation", "preparation owner is unavailable");
  auto spec = preparationSpec(frozen, modelKey);
  auto owner = m_state->coreOwner;
  spec.dispatch = [owner](std::function<void()> task) {
    auto ticket = owner->operationRuntime->acquire();
    owner->operationRuntime->post(ticket, std::move(task));
  };
  spec.schedule = [owner](std::chrono::steady_clock::time_point deadline,
                          std::function<void()> task) {
    auto ticket = owner->operationRuntime->acquire();
    return owner->operationRuntime->scheduleAt(ticket, deadline, std::move(task));
  };
  spec.cancelled = [state = m_state] {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->phase != detail::RuntimeState::Phase::Open;
  };
  spec.acquireCommit = [state = m_state](std::chrono::steady_clock::time_point deadline) {
    using Guard = std::unique_lock<std::mutex>;
    auto guard = std::make_shared<Guard>(state->mutex, std::defer_lock);
    while (!guard->try_lock()) {
      if (std::chrono::steady_clock::now() >= deadline)
        return std::shared_ptr<void>{};
      std::this_thread::yield();
    }
    if (state->phase != detail::RuntimeState::Phase::Open)
      return std::shared_ptr<void>{};
    return std::shared_ptr<void>(guard, static_cast<void*>(guard.get()));
  };
  try {
    auto prepared = cache->prepare(spec, options.cache, options.timeout);
    {
      std::lock_guard<std::mutex> lock(m_state->mutex);
      if (m_state->phase != detail::RuntimeState::Phase::Open)
        throw DiError("RUNTIME_CLOSED", "local", "preparation",
                      "Runtime closed before preparation could be returned");
    }
    return prepared;
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw mapPreparationError(error);
  }
}

PreparationHandle User::prepareAsync(const std::string& modelKey,
                                     const PrepareOptions& options) const
{
  if (!m_state)
    throw DiError("RUNTIME_CLOSED", "local", "preparation", "User has no Runtime state");
  std::shared_ptr<ModelPreparationCache> cache;
  FrozenConfig frozen;
  auto ticket = std::make_shared<detail::PreparationTicket>(m_state);
  {
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (m_state->phase != detail::RuntimeState::Phase::Open)
      throw DiError("RUNTIME_CLOSED", "local", "preparation", "Runtime is closed");
    const auto found = m_state->models.find(modelKey);
    if (found == m_state->models.end())
      throw DiError("MODEL_NOT_FOUND", "local", "preparation", "registered model key is unknown");
    cache = m_state->preparationCache;
    frozen = found->second;
    ++m_state->inFlight;
    m_state->preparationInFlight.fetch_add(1, std::memory_order_acq_rel);
    ticket->armed = true;
  }
  if (!cache)
    throw DiError("RUNTIME_CLOSED", "local", "preparation", "preparation owner is unavailable");
  auto spec = preparationSpec(frozen, modelKey);
  auto owner = m_state->coreOwner;
  spec.dispatch = [owner](std::function<void()> task) {
    auto operationTicket = owner->operationRuntime->acquire();
    owner->operationRuntime->post(operationTicket, std::move(task));
  };
  spec.schedule = [owner](std::chrono::steady_clock::time_point deadline,
                          std::function<void()> task) {
    auto operationTicket = owner->operationRuntime->acquire();
    return owner->operationRuntime->scheduleAt(operationTicket, deadline, std::move(task));
  };
  spec.cancelled = [state = m_state] {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->phase != detail::RuntimeState::Phase::Open;
  };
  spec.acquireCommit = [state = m_state](std::chrono::steady_clock::time_point deadline) {
    using Guard = std::unique_lock<std::mutex>;
    auto guard = std::make_shared<Guard>(state->mutex, std::defer_lock);
    while (!guard->try_lock()) {
      if (std::chrono::steady_clock::now() >= deadline)
        return std::shared_ptr<void>{};
      std::this_thread::yield();
    }
    if (state->phase != detail::RuntimeState::Phase::Open)
      return std::shared_ptr<void>{};
    return std::shared_ptr<void>(guard, static_cast<void*>(guard.get()));
  };
  auto ticketBox = std::make_shared<std::shared_ptr<void>>(ticket);
  spec.onTerminal = [ticketBox] { ticketBox->reset(); };
  std::shared_ptr<PreparationHandle::State> operation;
  try {
    operation = cache->prepareAsync(spec, options.cache, options.timeout);
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw mapPreparationError(error);
  }
  operation->lifetime = std::shared_ptr<void>(ticketBox, static_cast<void*>(ticketBox.get()));
  operation->onDestroy = operation->cancel;
  operation->workerThread = [owner] {
    return owner && owner->operationRuntime && owner->operationRuntime->isWorkerThread();
  };
  auto rawResult = operation->result;
  operation->result = [rawResult](Milliseconds timeout) {
    try { return rawResult(timeout); }
    catch (const DiError&) { throw; }
    catch (const std::exception& error) { throw mapPreparationError(error); }
  };
  auto rawCompletion = operation->onCompletion;
  auto runtimeState = m_state;
  operation->onCompletion = [rawCompletion, runtimeState](PreparationCompletion callback) {
    std::lock_guard<std::mutex> lock(runtimeState->mutex);
    if (runtimeState->phase != detail::RuntimeState::Phase::Open)
      throw DiError("RUNTIME_CLOSED", "local", "preparation", "Runtime is closed");
    try {
      return rawCompletion([callback = std::move(callback)](
      std::exception_ptr error, std::optional<PreparedModel> value) mutable {
      if (error) {
        try { std::rethrow_exception(error); }
        catch (const DiError&) { }
        catch (const std::exception& source) {
          try { throw mapPreparationError(source); }
          catch (...) { error = std::current_exception(); }
        }
      }
      callback(error, std::move(value));
      });
    }
    catch (const DiError&) {
      throw;
    }
    catch (const std::exception& error) {
      throw mapPreparationError(error);
    }
  };
  auto rawResultAsync = operation->resultAsync;
  operation->resultAsync = [rawResultAsync, runtimeState](Milliseconds timeout,
                                             PreparationCompletion callback) {
    std::lock_guard<std::mutex> lock(runtimeState->mutex);
    if (runtimeState->phase != detail::RuntimeState::Phase::Open)
      throw DiError("RUNTIME_CLOSED", "local", "preparation", "Runtime is closed");
    try {
      return rawResultAsync(timeout, [callback = std::move(callback)](
      std::exception_ptr error, std::optional<PreparedModel> value) mutable {
      if (error) {
        try { std::rethrow_exception(error); }
        catch (const DiError&) { }
        catch (const std::exception& source) {
          try { throw mapPreparationError(source); }
          catch (...) { error = std::current_exception(); }
        }
      }
      callback(error, std::move(value));
      });
    }
    catch (const DiError&) {
      throw;
    }
    catch (const std::exception& error) {
      throw mapPreparationError(error);
    }
  };
  return PreparationHandle(std::move(operation));
}

Runtime::Runtime(std::shared_ptr<detail::RuntimeState> state)
  : m_state(std::move(state))
{
}

Runtime::~Runtime() noexcept
{
  if (!m_state) return;
  auto state = m_state;
  std::shared_ptr<detail::CoreRuntimeOwner> owner;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->phase == detail::RuntimeState::Phase::Open)
      state->phase = detail::RuntimeState::Phase::Closing;
    state->cv.notify_all();
    owner = state->coreOwner;
  }
  // The owner performs an idempotent Core close.  Its destructor later
  // releases Face/keys after the IO context has stopped; User handles retain
  // this closed state until their own references are gone.
  if (owner)
    owner->close();
}

void Runtime::close() noexcept
{
  if (!m_state)
    return;

  auto state = m_state;
  std::shared_ptr<detail::CoreRuntimeOwner> owner;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->phase == detail::RuntimeState::Phase::Open)
      state->phase = detail::RuntimeState::Phase::Closing;
    owner = state->coreOwner;
  }
  if (owner)
    owner->close();
}

bool Runtime::drain(Milliseconds timeout) const
{
  if (timeout.count() < 0)
    throw DiError("INVALID_ARGUMENT", "local", "lifecycle",
                  "Runtime drain timeout must not be negative");
  if (!m_state)
    throw DiError("RUNTIME_CLOSED", "local", "lifecycle", "Runtime has no state");

  auto state = m_state;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::shared_ptr<detail::CoreRuntimeOwner> owner;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->phase == detail::RuntimeState::Phase::Open)
      state->phase = detail::RuntimeState::Phase::Closing;
    owner = state->coreOwner;
  }
  if (owner)
    owner->close();
  bool drained = true;
  if (owner && owner->operationRuntime) {
    try {
      const auto now = std::chrono::steady_clock::now();
      const auto remaining = now >= deadline ? Milliseconds(0) :
        std::chrono::duration_cast<Milliseconds>(deadline - now);
      drained = owner->operationRuntime->drain(remaining);
    }
    catch (const ndn_service_framework::OperationError& error) {
      if (error.code() == ndn_service_framework::OperationErrorCode::WouldDeadlock)
        throw DiError("WOULD_DEADLOCK", "local", "lifecycle", error.what());
      throw DiError("RUNTIME_CLOSED", "local", "lifecycle", error.what());
    }
  }
  if (!drained)
    return false;
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    while (state->inFlight != 0) {
      if (state->cv.wait_until(lock, deadline) == std::cv_status::timeout)
        return false;
    }
    state->phase = detail::RuntimeState::Phase::Drained;
  }
  if (owner)
    owner->stopIo();
  return true;
}

Subscription Runtime::drainAsync(
  Milliseconds timeout, std::function<void(std::exception_ptr, bool)> callback) const
{
  if (timeout.count() < 0)
    throw DiError("INVALID_ARGUMENT", "local", "lifecycle",
                  "Runtime drain timeout must not be negative");
  if (!callback)
    throw DiError("INVALID_ARGUMENT", "local", "lifecycle",
                  "Runtime drain callback is empty");
  if (!m_state)
    throw DiError("RUNTIME_CLOSED", "local", "lifecycle", "Runtime has no state");

  auto state = m_state;
  std::shared_ptr<detail::CoreRuntimeOwner> owner;
  bool closeCore = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    owner = state->coreOwner;
    closeCore = state->phase != detail::RuntimeState::Phase::Open;
  }
  if (!owner || !owner->operationRuntime)
    throw DiError("RUNTIME_CLOSED", "local", "lifecycle", "Runtime has no Core owner");

  auto wrapped = [state, owner, callback = std::move(callback)](bool drained) mutable {
    auto notify = [callback = std::move(callback)](bool result) mutable {
      try {
        callback(nullptr, result);
      }
      catch (...) {
        // Public callbacks are failure-isolated from the Core worker.
      }
    };
    if (!drained) {
      notify(false);
      return;
    }
    bool finalizeShutdown = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->phase == detail::RuntimeState::Phase::Closing) {
        state->phase = detail::RuntimeState::Phase::Drained;
        finalizeShutdown = true;
      }
    }
    if (finalizeShutdown && owner)
      owner->stopIo();
    notify(true);
  };
  try {
    return owner->operationRuntime->drainAsync(
      timeout, std::move(wrapped), closeCore,
      [state] { return state->preparationInFlight.load(std::memory_order_acquire) == 0; });
  }
  catch (const ndn_service_framework::OperationError& error) {
    if (error.code() == ndn_service_framework::OperationErrorCode::Capacity)
      throw DiError("SUBSCRIPTION_LIMIT", "local", "lifecycle", error.what());
    if (error.code() == ndn_service_framework::OperationErrorCode::WouldDeadlock)
      throw DiError("WOULD_DEADLOCK", "local", "lifecycle", error.what());
    throw DiError("RUNTIME_CLOSED", "local", "lifecycle", error.what());
  }
}

std::shared_ptr<Runtime> Runtime::open(RuntimeConfig config)
{
  if (config.nativeConfigPath.empty())
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "configuration",
                  "RuntimeConfig.nativeConfigPath is required");
  if (config.maxPreparedBytes == 0 || config.maxPreparedEntries == 0 ||
      config.preparationJobTimeout.count() <= 0)
    throw DiError("CACHE_BUDGET_EXCEEDED", "local", "configuration",
                  "Runtime preparation limits must be positive");

  const auto primaryPath = std::filesystem::absolute(config.nativeConfigPath).lexically_normal();
  // Establish the actual Runtime Face before loading trust schemas so every
  // frozen validator is bound to this one IO context.
  auto coreOwner = std::make_shared<detail::CoreRuntimeOwner>();
  coreOwner->face = std::make_shared<ndn::Face>(coreOwner->io);
  const auto primary = freezeConfig(primaryPath, coreOwner->face.get());
  auto state = std::make_shared<detail::RuntimeState>();
  state->config = config;
  state->config.models.clear();
  state->config.nativeConfigPath = primary.path.string();
  state->baseDirectory = primary.path.parent_path();
  state->models.emplace("default", primary);
  // The key objects are transferred from freezeConfig.  Runtime never reads
  // operator key paths a second time after validation.
  coreOwner->requesterPrivateKey = primary.requesterPrivateKey;
  coreOwner->authorityPublicKey = primary.authorityPublicKey;
  coreOwner->operationRuntime = ndn_service_framework::OperationRuntime::create();
  state->coreOwner = coreOwner;
  state->preparationCache = std::make_shared<ModelPreparationCache>(
    config.maxPreparedBytes, config.maxPreparedEntries, config.preparationJobTimeout);

  std::set<std::string> keys;
  for (const auto& registration : config.models) {
    if (registration.key.empty() || registration.key == "default" ||
        !keys.insert(registration.key).second || registration.nativeConfigPath.empty()) {
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "model-registry",
                    "model registrations require unique non-default keys and paths");
    }
    const auto modelPath = (state->baseDirectory / registration.nativeConfigPath).lexically_normal();
    const auto frozen = freezeConfig(modelPath, coreOwner->face.get());
    if (!sameTrustDomain(primary, frozen))
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "identity",
                    "registered model configuration crosses the Runtime trust domain");
    state->config.models.push_back({registration.key, frozen.path.string()});
    state->models.emplace(registration.key, frozen);
  }
  return std::shared_ptr<Runtime>(new Runtime(std::move(state)));
}

User Runtime::user(UserConfig config)
{
  if (!m_state)
    throw DiError("RUNTIME_CLOSED", "local", "lifecycle", "Runtime has no state");
  if (!config.profileName.empty() && config.profileName != "default")
    throw DiError("UNSUPPORTED_CAPABILITY", "local", "profile",
                  "only the default Runtime profile is supported");
  std::lock_guard<std::mutex> lock(m_state->mutex);
  if (m_state->phase != detail::RuntimeState::Phase::Open)
    throw DiError("RUNTIME_CLOSED", "local", "lifecycle", "Runtime is closed");
  return User(m_state, std::move(config.profileName));
}

} // namespace ndnsf::di
