#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ModelPreparationCache.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModel.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelPackage.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/RuntimeTestAccess.hpp"
#include "ndn-service-framework/OperationRuntime.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/common.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <string>
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

bool isRecoverableNacBootstrapFailure(const std::string& message) noexcept
{
  // NAC-ABE reports an unavailable Attribute Authority through a stable
  // library diagnostic after its bounded public-parameter retries.  The
  // requester remains fail-closed until a later bootstrap succeeds; this
  // expected unprovisioned state must not poison Runtime's Core lifecycle.
  constexpr const char marker[] =
    "Failed to fetch public parameters after multiple attempts";
  return message.find(marker) != std::string::npos;
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

PreparationSpec preparationSpec(const FrozenConfig& frozen, const std::string& key,
                                 RuntimeConfig::RepositorySourceLoader repositorySourceLoader = {},
                                 std::shared_ptr<const RepositorySourceProvider> repositorySourceProvider = {},
                                 std::shared_ptr<const RepositoryArtifactPublisher> repositoryArtifactPublisher = {})
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
  spec.publicationServiceName = request.value("service", std::string{});
  spec.repositoryArtifactPublisher = std::move(repositoryArtifactPublisher);
  if (spec.publicationServiceName.empty()) {
    const auto artifactRoot = catalog.at("publication").at("artifact_root").get<std::string>();
    const auto separator = artifactRoot.find('/', 1);
    spec.publicationServiceName = artifactRoot.substr(
      0, separator == std::string::npos ? artifactRoot.size() : separator);
  }
  const auto positiveLimit = [&] (const char* field) {
    if (!limits.contains(field) || !limits.at(field).is_number_unsigned() ||
        limits.at(field).get<std::uint64_t>() == 0)
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "limits",
                    std::string("runtime limit must be positive: ") + field);
    return limits.at(field).get<std::uint64_t>();
  };
  spec.maxSourceBytes = positiveLimit("max_source_bytes");
  spec.maxAssembledBytes = positiveLimit("max_assembled_bytes");
  spec.loadSource = [repositorySourceLoader = std::move(repositorySourceLoader),
                     repositorySourceProvider = std::move(repositorySourceProvider)] (
                      const PreparationSpec& current,
                      std::chrono::steady_clock::time_point deadline) {
    try {
      if (std::chrono::steady_clock::now() >= deadline)
        throw DiError("PREPARATION_TIMEOUT", "local", "preparation", "preparation deadline expired");
      const RepositorySourceRequest request{
        current.key, current.catalogConfigurationJson, current.maxSourceBytes, deadline};
      const auto localFallback = [&current] (const RepositorySourceRequest&) {
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
        return result;
      };
      if (repositorySourceProvider)
        return repositorySourceProvider->load(request, localFallback);
      if (repositorySourceLoader)
        return repositorySourceLoader(current.key, current.catalogConfigurationJson,
                                     current.maxSourceBytes, deadline);
      auto result = localFallback(request);
      if (std::chrono::steady_clock::now() >= deadline)
        throw DiError("PREPARATION_TIMEOUT", "local", "preparation", "preparation deadline expired");
      return result;
    }
    catch (const DiError& error) {
      // Preserve lifecycle and deadline semantics from a native source owner;
      // only an actual repository/local source failure is normalized here.
      if (error.code() == "PREPARATION_TIMEOUT" ||
          error.code() == "PREPARATION_CANCELLED" ||
          error.code() == "RUNTIME_CLOSED")
        throw;
      throw DiError("PREPARATION_SOURCE_UNAVAILABLE", "local", "preparation", error.what());
    }
    catch (const std::exception& error) {
      // Repository lifecycle meaning is carried by RepositorySourceError (or
      // another DiError), never guessed from arbitrary backend text.  A
      // non-typed adapter failure is a source-unavailable failure by design.
      throw DiError("PREPARATION_SOURCE_UNAVAILABLE", "local", "preparation", error.what());
    }
  };
  return spec;
}

FrozenConfig freezeConfig(const std::filesystem::path& path, ndn::Face* callbackFace,
                          bool repositorySourceLoaderConfigured)
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
  if (source.contains("file")) {
    (void)requiredString(source, "file");
  }
  else if (!repositorySourceLoaderConfigured) {
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "catalog",
                  "catalog source requires file when no repository source loader is configured");
  }
  else {
    // Repository-backed sources have no local locator.  Their immutable
    // identity remains pinned in the frozen catalog and is checked again by
    // NativeRequestCatalog when the returned bytes are assembled.
    (void)requiredString(source, "data_name");
    for (const auto* digest : {"digest", "model_manifest_digest", "canonical_graph_digest"}) {
      if (!isDigest(requiredString(source, digest)))
        throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "catalog",
                      std::string("repository-backed catalog source has an invalid digest: ") + digest);
    }
  }

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
  // Runtime owns the KeyChain that created the certificates supplied to
  // ServiceUser.  Keeping it here prevents a signing certificate from
  // outliving its private key material.
  std::unique_ptr<ndn::KeyChain> keyChain;
  ndn::security::Certificate requesterCertificate;
  ndn::security::Certificate authorityCertificate;
  // The ServiceUser is the sole Core transport owner for Runtime-bound
  // requests. It is declared after Face so destruction releases callbacks
  // before the Face and its IO context disappear.
  std::shared_ptr<ndn_service_framework::ServiceUser> serviceUser;
  std::shared_ptr<EVP_PKEY> requesterPrivateKey;
  std::shared_ptr<EVP_PKEY> authorityPublicKey;
  std::shared_ptr<ndn_service_framework::OperationRuntime> operationRuntime;
  std::unique_ptr<boost::asio::io_context::work> ioWork;
  std::thread ioThread;
  std::atomic<bool> ioRunning{false};
  std::atomic<bool> ioRecoveryStopped{false};
  std::atomic<bool> ioStopRequested{false};
  mutable std::mutex ioLifecycleMutex;
  std::condition_variable ioLifecycleCondition;
  std::mutex ioStopMutex;
  bool ioStopped = false;
  std::atomic<std::size_t> inFlight{0};
  std::atomic<bool> closed{false};
  std::atomic<bool> ioFailed{false};
  mutable std::mutex ioFailureMutex;
  std::string ioFailureMessage;
  std::function<void(const std::string&)> ioFailureNotifier;

  void recordIoFailure() noexcept
  {
    std::function<void(const std::string&)> notifier;
    std::string reason;
    try {
      throw;
    }
    catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(ioFailureMutex);
      ioFailureMessage = error.what();
      reason = ioFailureMessage;
      notifier = ioFailureNotifier;
    }
    catch (...) {
      std::lock_guard<std::mutex> lock(ioFailureMutex);
      ioFailureMessage = "unknown exception escaped the Core Face callback";
      reason = ioFailureMessage;
      notifier = ioFailureNotifier;
    }
    const bool recoverableBootstrap = isRecoverableNacBootstrapFailure(reason);
    if (!recoverableBootstrap)
      ioFailed.store(true, std::memory_order_release);
    if (!recoverableBootstrap && notifier) {
      try { notifier(reason); }
      catch (...) {
        // A lifecycle notifier must never replace the original Face failure.
      }
    }
    if (operationRuntime)
      operationRuntime->notifyWaiters();
    // Keep this Face loop alive in a failed-but-drainable state.  The
    // exception may race Runtime::close(), whose cancellation path can have
    // already queued Core cleanup; stopIo() is the owner-controlled fence
    // that stops this recovery loop after that queue has drained.  A known
    // NAC bootstrap-unavailable diagnostic is recoverable and deliberately
    // leaves ioFailed clear; request authorization still rejects until the
    // consumer is ready.
  }

  std::string ioFailure() const
  {
    std::lock_guard<std::mutex> lock(ioFailureMutex);
    return ioFailureMessage;
  }

  void close() noexcept
  {
    if (closed.exchange(true))
      return;
    if (operationRuntime)
      operationRuntime->close();
  }

  void startIo(const std::shared_ptr<CoreRuntimeOwner>& self)
  {
    ioWork = std::make_unique<boost::asio::io_context::work>(io);
    const std::weak_ptr<CoreRuntimeOwner> weakSelf = self;
    ioThread = std::thread([weakSelf] {
      if (const auto self = weakSelf.lock()) {
        self->ioRunning.store(true, std::memory_order_release);
        self->ioRecoveryStopped.store(false, std::memory_order_release);
        self->ioStopRequested.store(false, std::memory_order_release);
        try {
          self->io.run();
        }
        catch (...) {
          // ndn-cxx/NAC callbacks may throw from a Face event handler (for
          // example after an unprovisioned public-parameter retry budget is
          // exhausted).  Never let an exception escape the owner thread and
          // call std::terminate; retain the failure as a lifecycle signal so
          // drain cannot report a clean shutdown for a broken Face.
          self->recordIoFailure();
          // A Face callback can throw after close() has queued cancellation
          // work.  Restart the same owned context so those callbacks and
          // deferred conversation cleanup remain executable until the
          // enclosing Runtime performs its drain/stop fence.
          // Isolate every subsequent handler.  run_one() limits each
          // exception boundary to one callback, so a malformed cleanup task
          // cannot discard handlers queued behind it.
          self->io.restart();
          while (!self->io.stopped() &&
                 !self->ioStopRequested.load(std::memory_order_acquire)) {
            try {
              self->io.run_one();
            }
            catch (...) {
              // The original Face exception remains the lifecycle cause;
              // continue draining the next handler in the same owner loop.
              self->io.restart();
            }
          }
          self->ioRecoveryStopped.store(true, std::memory_order_release);
          self->ioLifecycleCondition.notify_all();
        }
        self->ioRunning.store(false, std::memory_order_release);
        self->ioLifecycleCondition.notify_all();
      }
    });
  }

  bool stopIo(std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) noexcept
  {
    std::unique_lock<std::mutex> stopLock(ioStopMutex);
    if (ioStopped)
      return true;
    if (ioFailed.load(std::memory_order_acquire) && ioRunning.load(std::memory_order_acquire) &&
        ioThread.joinable() && std::this_thread::get_id() != ioThread.get_id()) {
      // A failed Face remains in the recovery loop until this explicit
      // barrier runs.  It covers cancellation/scope callbacks queued by a
      // racing close and by deferred conversation finalization.
      auto completed = std::make_shared<std::atomic<bool>>(false);
      bool posted = false;
      try {
        boost::asio::post(io, [this, completed] {
          completed->store(true, std::memory_order_release);
          ioLifecycleCondition.notify_all();
        });
        posted = true;
        std::unique_lock<std::mutex> lock(ioLifecycleMutex);
        const auto ready = [this, completed, posted] {
          return (posted && completed->load(std::memory_order_acquire)) ||
                 ioRecoveryStopped.load(std::memory_order_acquire) ||
                 !ioRunning.load(std::memory_order_acquire);
        };
        if (deadline) {
          if (!ioLifecycleCondition.wait_until(lock, *deadline, ready))
            return false;
        }
        else {
          ioLifecycleCondition.wait(lock, ready);
        }
      }
      catch (...) {
        // If posting the barrier fails, a bounded drain keeps the owner
        // retryable unless the recovery loop has already terminated.  The
        // unbounded destructor path may proceed to join as its final fence.
        if (deadline) {
          std::unique_lock<std::mutex> lock(ioLifecycleMutex);
          const auto stopped = [this] {
            return ioRecoveryStopped.load(std::memory_order_acquire) ||
                   !ioRunning.load(std::memory_order_acquire);
          };
          if (!ioLifecycleCondition.wait_until(lock, *deadline, stopped))
            return false;
        }
      }
    }
    ioStopRequested.store(true, std::memory_order_release);
    ioWork.reset();
    io.stop();
    if (!ioThread.joinable()) {
      ioStopped = true;
      return true;
    }
    if (std::this_thread::get_id() != ioThread.get_id()) {
      std::unique_lock<std::mutex> lock(ioLifecycleMutex);
      const auto exited = [this] {
        return !ioRunning.load(std::memory_order_acquire);
      };
      if (deadline && !ioLifecycleCondition.wait_until(lock, *deadline, exited))
        return false;
      if (!deadline)
        ioLifecycleCondition.wait(lock, exited);
    }
    if (std::this_thread::get_id() == ioThread.get_id()) {
      // The owner is held by the IO lambda until run() returns.  Move the
      // self-join to a detached reaper so destruction never joins itself.
      // Keep ioStopped false: the caller is on the worker and therefore
      // cannot claim that the join/drain fence has completed yet.  The
      // detached reaper owns only the moved std::thread and never touches
      // this owner; the worker's self reference keeps the owner alive until
      // its callback has finished.
      auto thread = std::move(ioThread);
      std::thread([thread = std::move(thread)] () mutable {
        thread.join();
      }).detach();
      return false;
    }
    ioThread.join();
    ioStopped = true;
    return true;
  }

  ~CoreRuntimeOwner() noexcept
  {
    close();
    (void) stopIo();
    // Release callbacks before Face and its IO context, then release the
    // signing keys that back the ServiceUser certificates.
    serviceUser.reset();
    face.reset();
    keyChain.reset();
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
  std::shared_ptr<ndn_service_framework::ServiceUser> coreUser;
  std::shared_ptr<NativeAuthenticatedGrantClient> grants;
  std::shared_ptr<const NativeOfferAdmission> offerAdmission;
  std::shared_ptr<NativeConversationCoordinator> conversations;
  // Provider-only runtimes deliberately have no Core User or requester
  // model directory.  The façade is an opaque shared owner returned by
  // Runtime::provider(); User access remains rejected for that state.
  std::shared_ptr<Provider> provider;
  ProviderConfig providerConfig;
  bool providerOnly = false;
  std::shared_ptr<NativePlacementStrategyRegistry> placementRegistry;
  // Shared only as an opaque identity token; it never contains request or
  // authorization state and is used to bind placement handles to this State.
  std::shared_ptr<void> runtimeBinding;
  // Prepared views and pending handles own clients. Runtime keeps only a weak
  // lookup/index so idle cache entries can release their catalog/source bytes.
  std::map<std::string, std::weak_ptr<NativeInferenceClient>> clients;
  // Immutable client snapshots let the Core drain predicate inspect client
  // quiescence without acquiring the DI state mutex while the Core worker
  // holds its own runtime mutex.  Writers publish under mutex; readers use
  // the atomic shared_ptr operations below.
  using ClientList = std::vector<std::weak_ptr<NativeInferenceClient>>;
  std::shared_ptr<const ClientList> clientsSnapshot = std::make_shared<const ClientList>();
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

namespace {

std::string rawPublicKey(const std::shared_ptr<EVP_PKEY>& key)
{
  if (!key) throw std::runtime_error("Runtime authority public key is unavailable");
  std::string raw(32, '\0');
  std::size_t size = raw.size();
  if (EVP_PKEY_get_raw_public_key(key.get(),
        reinterpret_cast<unsigned char*>(raw.data()), &size) != 1 || size != raw.size())
    throw std::runtime_error("Runtime authority public key is not Ed25519");
  return raw;
}

NativeJson requestRuntimeConfiguration(const FrozenConfig& frozen,
                                       const PreparedModelPackage& package)
{
  const auto root = nativeParseJson(package.registration->configurationJson);
  const auto& request = root.at("request");
  return NativeJson{
    {"schema", "ndnsf-di-native-request-runtime-v1"},
    {"contract", {
      {"service_name", request.at("service")},
      {"task_name", request.at("task")},
      {"adapter_name", package.catalog.model.descriptor.adapterId},
      {"adapter_descriptor_digest", package.catalog.model.descriptor.adapter.descriptorDigest()},
      {"adapter_composition_digest", request.at("adapter_composition_digest")},
      {"task_descriptor_digest", request.at("task_descriptor_digest")},
      {"generation_mode", request.value("generation_mode", "TOKEN_DIAGNOSTIC")},
      {"tokenizer_digest", request.value("tokenizer_digest", std::string{})},
    }},
    {"requester_identity", frozen.requesterIdentity},
    {"protection_epoch", frozen.protectionEpoch},
    {"input_layout_digest", request.at("input_layout_digest")},
    {"security", {
      {"policy_digest", request.at("security_policy_digest")},
      {"require_protected_artifacts", true},
    }},
    {"budget", {
      {"max_candidates", request.at("max_candidates")},
      {"max_policy_ms", request.at("max_policy_ms")},
      {"max_reentries", request.value("max_reentries", 1)},
    }},
    {"state_mapping", {
      {"inputs", package.catalog.stateMapping.inputs},
      {"outputs", package.catalog.stateMapping.outputs},
    }},
    {"no_progress_ms", request.value("no_progress_ms", 5000)},
    {"max_segments", request.value("max_segments", 4096)},
  };
}

/** Materialize the Runtime-owned Core user before prepare-time publication.
 * Runtime::open remains metadata-only; this helper is called by prepare or
 * the first request and is idempotent for fixture-bound users. */
void ensureRuntimeCoreTransport(const std::shared_ptr<detail::RuntimeState>& state)
{
  if (!state)
    throw DiError("RUNTIME_CLOSED", "local", "preparation", "Runtime has no state");
  std::unique_lock<std::mutex> lock(state->mutex);
  if (state->phase != detail::RuntimeState::Phase::Open || !state->coreOwner ||
      !state->coreOwner->operationRuntime)
    throw DiError("RUNTIME_CLOSED", "local", "preparation", "Runtime is closed");
  if (state->coreOwner->ioFailed.load(std::memory_order_acquire))
    throw DiError("RUNTIME_CLOSED", "local", "preparation",
                  "Runtime Core I/O failed: " + state->coreOwner->ioFailure());
  if (state->coreUser && state->grants)
    return;
  if (state->coreUser || state->grants)
    throw DiError("RUNTIME_CLOSED", "local", "preparation",
                  "Runtime Core transport binding is incomplete");
  const auto primary = state->models.find("default");
  if (primary == state->models.end() || !primary->second.requesterPrivateKey ||
      !primary->second.authorityPublicKey || !state->coreOwner->keyChain ||
      state->coreOwner->requesterCertificate.getName().empty() ||
      state->coreOwner->authorityCertificate.getName().empty())
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "identity",
                  "Runtime Core identity material is unavailable");
  try {
    auto serviceUser = std::make_shared<ndn_service_framework::ServiceUser>(
      *state->coreOwner->face, ndn::Name(primary->second.group),
      state->coreOwner->requesterCertificate,
      state->coreOwner->requesterCertificate,
      state->coreOwner->authorityCertificate,
      primary->second.trustSchema.string(), *state->coreOwner->keyChain);
    serviceUser->init();
    serviceUser->fetchPermissionsFromController(
      ndn::Name(primary->second.coreAuthorityIdentity));
    auto grants = std::make_shared<NativeAuthenticatedGrantClient>(
      primary->second.requesterIdentity, primary->second.requesterPrivateKey,
      primary->second.authorityIdentity, rawPublicKey(primary->second.authorityPublicKey),
      primary->second.protectionEpoch,
      NativeAuthenticatedGrantClient::issueThroughCore(
        serviceUser, primary->second.authorityIdentity,
        primary->second.authorityService),
      NativeAuthenticatedGrantClient::publishThroughCore(serviceUser));
    if (!state->coreOwner->ioThread.joinable())
      state->coreOwner->startIo(state->coreOwner);
    if (state->coreOwner->ioFailed.load(std::memory_order_acquire))
      throw DiError("RUNTIME_CLOSED", "local", "preparation",
                    "Runtime Core I/O failed: " + state->coreOwner->ioFailure());
    state->coreOwner->serviceUser = serviceUser;
    state->coreUser = std::move(serviceUser);
    state->grants = std::move(grants);
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "identity",
                  std::string("Runtime Core user cannot be created: ") + error.what());
  }
}

std::shared_ptr<NativeInferenceClient> makeRuntimeClient(
  const std::shared_ptr<detail::RuntimeState>& state,
  const std::shared_ptr<const PreparedModelPackage>& package)
{
  if (!state || !package || !package->registration)
    throw DiError("RUNTIME_CLOSED", "local", "request", "Runtime client binding is unavailable");
  std::unique_lock<std::mutex> lock(state->mutex);
  if (state->phase != detail::RuntimeState::Phase::Open || !state->coreOwner ||
      !state->coreOwner->operationRuntime ||
      state->coreOwner->ioFailed.load(std::memory_order_acquire))
    throw DiError("RUNTIME_CLOSED", "local", "request",
                  state->coreOwner && state->coreOwner->ioFailed.load(std::memory_order_acquire)
                    ? "Runtime Core I/O failed: " + state->coreOwner->ioFailure()
                    : "Runtime is closed");
  // Runtime::open deliberately does not construct a production ServiceUser:
  // its NAC consumer starts an asynchronous public-parameter fetch as part of
  // construction.  Materialize the transport only when a real native client
  // is requested, after test-only fixture binding has had its chance to supply
  // a LocalMock user and grant client.
  if (!state->coreUser || !state->grants) {
    if (state->coreUser || state->grants)
      throw DiError("RUNTIME_CLOSED", "local", "request",
                    "Runtime Core transport binding is incomplete");
    const auto primary = state->models.find("default");
    if (primary == state->models.end() || !primary->second.requesterPrivateKey ||
        !primary->second.authorityPublicKey ||
        !state->coreOwner->keyChain ||
        state->coreOwner->requesterCertificate.getName().empty() ||
        state->coreOwner->authorityCertificate.getName().empty())
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "identity",
                    "Runtime Core identity material is unavailable");
    try {
      auto serviceUser = std::make_shared<ndn_service_framework::ServiceUser>(
        *state->coreOwner->face, ndn::Name(primary->second.group),
        state->coreOwner->requesterCertificate,
        state->coreOwner->requesterCertificate,
        state->coreOwner->authorityCertificate,
        primary->second.trustSchema.string(), *state->coreOwner->keyChain);
      serviceUser->init();
      // Runtime-owned requester identities need the same controller
      // permission/bootstrap wave as maintained native applications. Queue
      // it before starting the Face owner; the request path waits
      // asynchronously for DKEY, permission, and PolicyStatus readiness
      // without blocking Core I/O or assembling post-ACK state early.
      serviceUser->fetchPermissionsFromController(
        ndn::Name(primary->second.coreAuthorityIdentity));
      auto grants = std::make_shared<NativeAuthenticatedGrantClient>(
        primary->second.requesterIdentity, primary->second.requesterPrivateKey,
        primary->second.authorityIdentity, rawPublicKey(primary->second.authorityPublicKey),
        primary->second.protectionEpoch,
        NativeAuthenticatedGrantClient::issueThroughCore(
          serviceUser, primary->second.authorityIdentity,
          primary->second.authorityService),
        NativeAuthenticatedGrantClient::publishThroughCore(serviceUser));
      // Publish the pair only after both objects and their callback closures
      // are complete; a constructor failure leaves Runtime retryable.
      // Start the owned Face before publishing the ServiceUser/grant pair.
      // Thread or work-guard creation can fail; keeping the pair unpublished
      // leaves the Runtime retryable instead of making a later request skip
      // the only required start attempt.
      if (!state->coreOwner->ioThread.joinable())
        state->coreOwner->startIo(state->coreOwner);
      state->coreOwner->serviceUser = std::move(serviceUser);
      state->coreUser = state->coreOwner->serviceUser;
      state->grants = std::move(grants);
    }
    catch (const DiError&) {
      throw;
    }
    catch (const std::exception& error) {
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "identity",
                    std::string("Runtime Core user cannot be created: ") + error.what());
    }
  }
  // Runtime::open validates and freezes configuration without touching the
  // operator transport.  Start the owned Face only when this call materialized
  // the Runtime's ServiceUser.  A test or embedding boundary may have already
  // supplied an external ServiceUser/Grant client; starting the unused Face in
  // that case would create a second transport owner and can report a local NFD
  // connection failure even though all request traffic uses the supplied Face.
  if (state->coreOwner->ioFailed.load(std::memory_order_acquire))
    throw DiError("RUNTIME_CLOSED", "local", "request",
                  "Runtime Core I/O failed: " + state->coreOwner->ioFailure());
  const auto key = package->preparationKeyDigest;
  const auto found = state->clients.find(key);
  if (found != state->clients.end()) {
    if (const auto existing = found->second.lock())
      return existing;
    state->clients.erase(found);
  }
  const auto registration = state->models.find(package->registration->key);
  if (registration == state->models.end())
    throw DiError("MODEL_NOT_FOUND", "local", "request", "prepared model registration is not retained");
  const auto runtimeJson = requestRuntimeConfiguration(registration->second, *package);
  const auto runtime = nativeRequestRuntimeFromJson(
    nativeCanonicalJson(runtimeJson), package->catalog, state->grants);
  auto preparation = package->catalog.preparation->makePreparation(
    state->coreUser, runtime.contract.serviceName, package->preparedPublication);
  auto client = std::make_shared<NativeInferenceClient>(
    state->coreUser, package->catalog.preparation->adapters(), runtime,
    state->conversations,
    std::move(preparation), registration->second.offerAdmission);
  client->retainOwner(state->coreOwner);
  std::weak_ptr<detail::RuntimeState> weakState = state;
  const auto notifier = [weakState] {
    if (const auto state = weakState.lock()) {
      std::shared_ptr<detail::CoreRuntimeOwner> owner;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        owner = state->coreOwner;
      }
      if (owner && owner->operationRuntime)
        owner->operationRuntime->notifyWaiters();
    }
  };
  // Build the complete immutable snapshot before publishing the client.  All
  // allocating operations therefore happen while the map is unchanged; after
  // the insertion, the shared_ptr atomic store is non-throwing and cannot
  // leave the map and snapshot with different client sets.
  auto nextSnapshot = std::make_shared<detail::RuntimeState::ClientList>();
  nextSnapshot->reserve(state->clients.size() + 1);
  for (const auto& entry : state->clients)
    nextSnapshot->push_back(entry.second);
  nextSnapshot->push_back(client);
  std::shared_ptr<const detail::RuntimeState::ClientList> publishedSnapshot = nextSnapshot;

  // Install the notifier before publishing the client in either the map or
  // the immutable snapshot.  A concurrent caller can then never observe a
  // client whose terminal transition cannot wake the enclosing drain waiter.
  // The Core drain predicate reads the atomic snapshot and does not acquire
  // the DI state mutex, so this callback installation cannot form the former
  // Core-to-DI lock cycle.
  client->setDrainNotifier(notifier);
  state->clients.emplace(key, client);
  std::atomic_store_explicit(&state->clientsSnapshot, std::move(publishedSnapshot),
                             std::memory_order_release);
  return client;
}

std::vector<std::shared_ptr<NativeInferenceClient>> snapshotRuntimeClients(
  const std::shared_ptr<detail::RuntimeState>& state)
{
  std::vector<std::shared_ptr<NativeInferenceClient>> clients;
  if (!state)
    return clients;
  const auto snapshot = std::atomic_load_explicit(&state->clientsSnapshot,
                                                   std::memory_order_acquire);
  if (!snapshot)
    return clients;
  clients.reserve(snapshot->size());
  for (const auto& weak : *snapshot) {
    if (const auto client = weak.lock())
      clients.push_back(client);
  }
  return clients;
}

void closeRuntimeClients(
  const std::vector<std::shared_ptr<NativeInferenceClient>>& clients) noexcept
{
  for (const auto& client : clients) {
    if (client)
      client->close();
  }
}

} // namespace

DiError::DiError(std::string code, std::string domain, std::string boundary,
                 std::string message, std::string requestId, std::uint64_t attempt)
  : std::runtime_error(std::move(message)), m_code(std::move(code)),
    m_domain(std::move(domain)), m_boundary(std::move(boundary)),
    m_requestId(std::move(requestId)), m_attempt(attempt)
{
}

RepositorySourceError::RepositorySourceError(Kind kind, std::string message)
  : DiError(kind == Kind::Timeout ? "PREPARATION_TIMEOUT" :
            kind == Kind::Cancelled ? "PREPARATION_CANCELLED" :
            kind == Kind::Closed ? "RUNTIME_CLOSED" :
            "PREPARATION_SOURCE_UNAVAILABLE",
            "repository", "preparation", std::move(message))
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
  auto spec = preparationSpec(frozen, modelKey, m_state->config.repositorySourceLoader,
                              m_state->config.repositorySourceProvider,
                              m_state->config.repositoryArtifactPublisher);
  spec.runtimeBinding = m_state->runtimeBinding;
  const auto publicationServiceName = spec.publicationServiceName;
  spec.preparePublication = [state = m_state, modelKey, publicationServiceName](
    const NativeCanonicalPreparationCatalog& catalog, const NativeInspectedModel& model,
    const NativeRequestControl& control) {
    if (state->config.repositoryArtifactPublisher) {
      const auto source = catalog.sourceFor(model.descriptor);
      return state->config.repositoryArtifactPublisher->publish(
        modelKey, publicationServiceName, model, source,
        catalog.publicationFor(model.descriptor), control);
    }
    ensureRuntimeCoreTransport(state);
    std::shared_ptr<ndn_service_framework::ServiceUser> user;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      user = state->coreUser;
    }
    return catalog.preparePublication(user, publicationServiceName, model.descriptor, control);
  };
  spec.rollbackPublication = [state = m_state](const NativePreparedCanonicalPublication& publication) {
    if (state->config.repositoryArtifactPublisher) {
      state->config.repositoryArtifactPublisher->rollback(publication);
      return;
    }
    std::shared_ptr<ndn_service_framework::ServiceUser> user;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      user = state->coreUser;
    }
    if (!user)
      return;
    std::vector<ndn_service_framework::LargeDataPublishResult> rollbacks;
    const auto addData = [&publication](auto& rollback) {
      rollback.rollbackDataNames = publication.rollbackDataNames;
      rollback.rollbackDataNames.push_back(publication.sourceDataName);
      if (!publication.initializerDataName.empty())
        rollback.rollbackDataNames.push_back(publication.initializerDataName);
    };
    for (const auto& reference : publication.rollbackKeyReferences) {
      ndn_service_framework::LargeDataPublishResult rollback;
      rollback.success = true;
      rollback.encryptedDataName = ndn::Name(publication.rootDataName);
      addData(rollback);
      rollback.rollbackKeyId = reference.keyId;
      rollback.rollbackServiceName = reference.serviceName;
      rollbacks.push_back(std::move(rollback));
    }
    if (rollbacks.empty()) {
      ndn_service_framework::LargeDataPublishResult rollback;
      rollback.success = true;
      rollback.encryptedDataName = ndn::Name(publication.rootDataName);
      addData(rollback);
      rollbacks.push_back(std::move(rollback));
    }
    user->abortLargeDataPublications(rollbacks);
  };
  spec.clientFactory = [state = m_state](
    const std::shared_ptr<const PreparedModelPackage>& package) {
    return makeRuntimeClient(state, package);
  };
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
  auto spec = preparationSpec(frozen, modelKey, m_state->config.repositorySourceLoader,
                              m_state->config.repositorySourceProvider,
                              m_state->config.repositoryArtifactPublisher);
  spec.runtimeBinding = m_state->runtimeBinding;
  const auto publicationServiceName = spec.publicationServiceName;
  spec.preparePublication = [state = m_state, modelKey, publicationServiceName](
    const NativeCanonicalPreparationCatalog& catalog, const NativeInspectedModel& model,
    const NativeRequestControl& control) {
    if (state->config.repositoryArtifactPublisher) {
      const auto source = catalog.sourceFor(model.descriptor);
      return state->config.repositoryArtifactPublisher->publish(
        modelKey, publicationServiceName, model, source,
        catalog.publicationFor(model.descriptor), control);
    }
    ensureRuntimeCoreTransport(state);
    std::shared_ptr<ndn_service_framework::ServiceUser> user;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      user = state->coreUser;
    }
    return catalog.preparePublication(user, publicationServiceName, model.descriptor, control);
  };
  spec.rollbackPublication = [state = m_state](const NativePreparedCanonicalPublication& publication) {
    if (state->config.repositoryArtifactPublisher) {
      state->config.repositoryArtifactPublisher->rollback(publication);
      return;
    }
    std::shared_ptr<ndn_service_framework::ServiceUser> user;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      user = state->coreUser;
    }
    if (!user)
      return;
    std::vector<ndn_service_framework::LargeDataPublishResult> rollbacks;
    const auto addData = [&publication](auto& rollback) {
      rollback.rollbackDataNames = publication.rollbackDataNames;
      rollback.rollbackDataNames.push_back(publication.sourceDataName);
      if (!publication.initializerDataName.empty())
        rollback.rollbackDataNames.push_back(publication.initializerDataName);
    };
    for (const auto& reference : publication.rollbackKeyReferences) {
      ndn_service_framework::LargeDataPublishResult rollback;
      rollback.success = true;
      rollback.encryptedDataName = ndn::Name(publication.rootDataName);
      addData(rollback);
      rollback.rollbackKeyId = reference.keyId;
      rollback.rollbackServiceName = reference.serviceName;
      rollbacks.push_back(std::move(rollback));
    }
    if (rollbacks.empty()) {
      ndn_service_framework::LargeDataPublishResult rollback;
      rollback.success = true;
      rollback.encryptedDataName = ndn::Name(publication.rootDataName);
      addData(rollback);
      rollbacks.push_back(std::move(rollback));
    }
    user->abortLargeDataPublications(rollbacks);
  };
  spec.clientFactory = [state = m_state](
    const std::shared_ptr<const PreparedModelPackage>& package) {
    return makeRuntimeClient(state, package);
  };
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
  if (state->providerOnly && state->provider)
    state->provider->stop();
  {
    auto owner = state->coreOwner;
    std::unique_lock<std::mutex> ioGate;
    if (owner)
      ioGate = std::unique_lock<std::mutex>(owner->ioStopMutex);
    std::vector<std::shared_ptr<NativeInferenceClient>> clients;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->phase == detail::RuntimeState::Phase::Open)
        state->phase = detail::RuntimeState::Phase::Closing;
      state->cv.notify_all();
      clients.reserve(state->clients.size());
      for (const auto& entry : state->clients) {
        if (const auto client = entry.second.lock())
          clients.push_back(client);
      }
    }
    // The owner stop gate serializes this producer with a final stop while
    // the State mutex remains free for client completion notifiers.
    closeRuntimeClients(clients);
    if (owner && owner->operationRuntime)
      owner->operationRuntime->notifyWaiters();
    if (owner)
      owner->close();
  }
  // The owner performs an idempotent Core close.  Its destructor later
  // releases Face/keys after the IO context has stopped; User handles retain
  // this closed state until their own references are gone.
}

void detail::RuntimeTestAccess::bindProviderFixture(
  const std::shared_ptr<Runtime>& runtime,
  std::shared_ptr<ndn_service_framework::ServiceUser> user,
  std::shared_ptr<NativeAuthenticatedGrantClient> grants,
  std::shared_ptr<const NativeOfferAdmission> admission)
{
  if (!runtime || !runtime->m_state || !user || !grants || !admission)
    throw std::invalid_argument("Runtime test fixture binding is incomplete");
  std::lock_guard<std::mutex> lock(runtime->m_state->mutex);
  if (runtime->m_state->phase != detail::RuntimeState::Phase::Open)
    throw std::runtime_error("Runtime test fixture binding requires an open Runtime");
  auto owner = runtime->m_state->coreOwner;
  if (!owner || owner->ioThread.joinable() ||
      owner->ioRunning.load(std::memory_order_acquire))
    throw std::runtime_error(
      "Runtime test fixture binding must precede Core I/O start");
  const bool hasLiveClient = std::any_of(runtime->m_state->clients.begin(),
                                         runtime->m_state->clients.end(),
                                         [] (const auto& entry) {
                                           return !entry.second.expired();
                                         });
  if (runtime->m_state->preparationInFlight.load(std::memory_order_acquire) != 0 ||
      hasLiveClient)
    throw std::runtime_error(
      "Runtime test fixture binding requires no preparation or clients");
  // Runtime::open leaves production Core transport unmaterialized.  If a
  // caller reaches this hook after a future eager binding, drop all
  // state-held callbacks before releasing that ServiceUser; the owner Face
  // remains alive until the normal Core stop fence handles any queued work.
  runtime->m_state->grants.reset();
  runtime->m_state->coreUser.reset();
  for (auto& entry : runtime->m_state->models)
    entry.second.trustValidator.reset();
  owner->serviceUser.reset();
  runtime->m_state->coreUser = std::move(user);
  runtime->m_state->grants = std::move(grants);
  runtime->m_state->offerAdmission = admission;
  for (auto& entry : runtime->m_state->models)
    entry.second.offerAdmission = admission;
}

void detail::RuntimeTestAccess::bindProviderOnlyFixture(
  const std::shared_ptr<Runtime>& runtime, Provider provider)
{
  if (!runtime || !runtime->m_state || !provider.valid())
    throw std::invalid_argument("Provider-only Runtime test fixture binding is incomplete");
  std::lock_guard<std::mutex> lock(runtime->m_state->mutex);
  if (!runtime->m_state->providerOnly ||
      runtime->m_state->phase != detail::RuntimeState::Phase::Open)
    throw std::runtime_error(
      "Provider-only Runtime test fixture binding requires an open Provider Runtime");
  runtime->m_state->provider = std::make_shared<Provider>(std::move(provider));
}

void Runtime::close() noexcept
{
  if (!m_state)
    return;

  auto state = m_state;
  if (state->providerOnly && state->provider)
    state->provider->stop();
  auto owner = state->coreOwner;
  {
    std::unique_lock<std::mutex> ioGate;
    if (owner)
      ioGate = std::unique_lock<std::mutex>(owner->ioStopMutex);
    std::vector<std::shared_ptr<NativeInferenceClient>> clients;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->phase == detail::RuntimeState::Phase::Open)
        state->phase = detail::RuntimeState::Phase::Closing;
      clients.reserve(state->clients.size());
      for (const auto& entry : state->clients) {
        if (const auto client = entry.second.lock())
          clients.push_back(client);
      }
    }
    closeRuntimeClients(clients);
    if (owner && owner->operationRuntime)
      owner->operationRuntime->notifyWaiters();
    // Keep the Core operation worker alive until the caller registers its
    // drain barrier.  Runtime::close() is the non-blocking admission fence;
    // stopping the Core worker here races a subsequent drainAsync() that
    // still has to wait for NativeInferenceClient cleanup.  drain()/
    // drainAsync() close the owner after their waiter is installed, while
    // Runtime::~Runtime() remains the final close path for callers that do
    // not request an explicit drain.
  }
}

bool Runtime::drain(Milliseconds timeout) const
{
  if (timeout.count() < 0)
    throw DiError("INVALID_ARGUMENT", "local", "lifecycle",
                  "Runtime drain timeout must not be negative");
  if (!m_state)
    throw DiError("RUNTIME_CLOSED", "local", "lifecycle", "Runtime has no state");

  if (m_state->providerOnly) {
    {
      std::lock_guard<std::mutex> lock(m_state->mutex);
      if (m_state->phase == detail::RuntimeState::Phase::Open)
        m_state->phase = detail::RuntimeState::Phase::Closing;
    }
    const auto drained = m_state->provider ? m_state->provider->drain(timeout) : true;
    if (!drained)
      return false;
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->phase = detail::RuntimeState::Phase::Drained;
    return true;
  }

  auto state = m_state;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto owner = state->coreOwner;
  std::vector<std::shared_ptr<NativeInferenceClient>> clients;
  {
    std::unique_lock<std::mutex> ioGate;
    if (owner)
      ioGate = std::unique_lock<std::mutex>(owner->ioStopMutex);
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->phase == detail::RuntimeState::Phase::Open)
        state->phase = detail::RuntimeState::Phase::Closing;
      clients.reserve(state->clients.size());
      for (const auto& entry : state->clients) {
        if (const auto client = entry.second.lock())
          clients.push_back(client);
      }
    }
    closeRuntimeClients(clients);
    if (owner && owner->operationRuntime)
      owner->operationRuntime->notifyWaiters();
    if (owner)
      owner->close();
  }
  bool drained = true;
  for (const auto& client : clients) {
    if (!client)
      continue;
    try {
      const auto now = std::chrono::steady_clock::now();
      const auto remaining = now >= deadline ? Milliseconds(0) :
        std::chrono::duration_cast<Milliseconds>(deadline - now);
      drained = client->drain(remaining) && drained;
    }
    catch (const NativeDiError& error) {
      if (error.code() == "WOULD_DEADLOCK")
        throw DiError("WOULD_DEADLOCK", "local", "lifecycle", error.what());
      throw DiError("RUNTIME_CLOSED", "local", "lifecycle", error.what());
    }
  }
  if (owner && owner->operationRuntime) {
    try {
      const auto now = std::chrono::steady_clock::now();
      const auto remaining = now >= deadline ? Milliseconds(0) :
        std::chrono::duration_cast<Milliseconds>(deadline - now);
      drained = owner->operationRuntime->drain(remaining) && drained;
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
  }
  if (owner && !owner->stopIo(deadline))
    return false;
  if (owner && owner->ioFailed.load(std::memory_order_acquire))
    return false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->phase = detail::RuntimeState::Phase::Drained;
  }
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
  if (state->providerOnly) {
    const auto provider = state->provider;
    if (!provider)
      throw DiError("RUNTIME_CLOSED", "local", "lifecycle",
                    "Provider-only Runtime has no Provider");
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->phase == detail::RuntimeState::Phase::Open)
        state->phase = detail::RuntimeState::Phase::Closing;
    }
    auto wrapped = [state, callback = std::move(callback)](
      std::exception_ptr error, bool result) mutable {
      if (!error && result) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->phase == detail::RuntimeState::Phase::Open ||
            state->phase == detail::RuntimeState::Phase::Closing)
          state->phase = detail::RuntimeState::Phase::Drained;
      }
      try { callback(std::move(error), result); } catch (...) {}
    };
    return provider->drainAsync(timeout, std::move(wrapped));
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::shared_ptr<detail::CoreRuntimeOwner> owner;
  bool closeCore = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    owner = state->coreOwner;
    closeCore = state->phase != detail::RuntimeState::Phase::Open;
  }
  if (!owner || !owner->operationRuntime)
    throw DiError("RUNTIME_CLOSED", "local", "lifecycle", "Runtime has no Core owner");

  auto wrapped = [state, owner, deadline, callback = std::move(callback)](bool drained) mutable {
    auto notify = [callback = std::move(callback)](std::exception_ptr error,
                                                    bool result) mutable {
      try {
        callback(std::move(error), result);
      }
      catch (...) {
        // Public callbacks are failure-isolated from the Core worker.
      }
    };
    if (!drained) {
      notify(nullptr, false);
      return;
    }
    if (owner && owner->ioFailed.load(std::memory_order_acquire)) {
      const auto reason = owner->ioFailure();
      notify(std::make_exception_ptr(DiError(
               "RUNTIME_CLOSED", "local", "lifecycle",
               reason.empty() ? "Runtime Core I/O failed"
                              : "Runtime Core I/O failed: " + reason)),
             false);
      return;
    }
    bool finalizeShutdown = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->phase == detail::RuntimeState::Phase::Closing) {
        finalizeShutdown = true;
      }
    }
    if (finalizeShutdown && owner && !owner->stopIo(deadline)) {
      notify(nullptr, false);
      return;
    }
    if (owner && owner->ioFailed.load(std::memory_order_acquire)) {
      const auto reason = owner->ioFailure();
      notify(std::make_exception_ptr(DiError(
               "RUNTIME_CLOSED", "local", "lifecycle",
               reason.empty() ? "Runtime Core I/O failed"
                              : "Runtime Core I/O failed: " + reason)),
             false);
      return;
    }
    if (finalizeShutdown) {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->phase == detail::RuntimeState::Phase::Closing)
        state->phase = detail::RuntimeState::Phase::Drained;
    }
    notify(nullptr, true);
  };
  try {
    return owner->operationRuntime->drainAsync(
      timeout, std::move(wrapped), closeCore,
      [state, owner] {
        if (owner->ioFailed.load(std::memory_order_acquire))
          return true;
        if (state->preparationInFlight.load(std::memory_order_acquire) != 0)
          return false;
        const auto clients = snapshotRuntimeClients(state);
        return std::all_of(clients.begin(), clients.end(),
                           [] (const auto& client) {
                             return !client || client->isQuiescent();
                           });
      });
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
  const auto primary = freezeConfig(primaryPath, coreOwner->face.get(),
                                    static_cast<bool>(config.repositorySourceLoader) ||
                                      static_cast<bool>(config.repositorySourceProvider));
  try {
    // Runtime owns the KeyChain and creates the Core transport identities
    // once per Runtime. When the process supplies an NDN PIB/TPM pair (the
    // normal multi-process/MiniNDN boundary), use those stores so the
    // requester certificate and private key match the Controller-encrypted
    // permission/DKEY material. Keep the memory pair only for isolated
    // in-process callers that deliberately provide no NDN client stores.
    const char* pibLocator = std::getenv("NDN_CLIENT_PIB");
    const char* tpmLocator = std::getenv("NDN_CLIENT_TPM");
    if ((pibLocator != nullptr && *pibLocator != '\0') !=
        (tpmLocator != nullptr && *tpmLocator != '\0')) {
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "identity",
                    "NDN_CLIENT_PIB and NDN_CLIENT_TPM must be configured together");
    }
    if (pibLocator != nullptr && tpmLocator != nullptr &&
        *pibLocator != '\0' && *tpmLocator != '\0') {
      coreOwner->keyChain = std::make_unique<ndn::KeyChain>(
        std::string(pibLocator), std::string(tpmLocator));
    }
    else {
      coreOwner->keyChain = std::make_unique<ndn::KeyChain>("pib-memory:", "tpm-memory:");
    }
    const auto requesterIdentity = coreOwner->keyChain->createIdentity(
      ndn::Name(primary.requesterIdentity), ndn::RsaKeyParams(2048));
    const auto authorityIdentity = coreOwner->keyChain->createIdentity(
      ndn::Name(primary.coreAuthorityIdentity), ndn::RsaKeyParams(2048));
    coreOwner->requesterCertificate = requesterIdentity.getDefaultKey().getDefaultCertificate();
    coreOwner->authorityCertificate = authorityIdentity.getDefaultKey().getDefaultCertificate();
    coreOwner->requesterPrivateKey = primary.requesterPrivateKey;
    coreOwner->authorityPublicKey = primary.authorityPublicKey;
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "identity",
                  std::string("Runtime Core user cannot be created: ") + error.what());
  }
  auto state = std::make_shared<detail::RuntimeState>();
  std::weak_ptr<detail::RuntimeState> weakState = state;
  coreOwner->ioFailureNotifier = [weakState] (const std::string& reason) {
    const auto state = weakState.lock();
    if (!state)
      return;
    for (const auto& client : snapshotRuntimeClients(state)) {
      if (client)
        client->failIo(reason);
    }
  };
  state->runtimeBinding = std::make_shared<std::uint8_t>(0);
  state->placementRegistry = std::make_shared<NativePlacementStrategyRegistry>();
  state->placementRegistry->registerStrategy(
    "native-pre-split-first", std::make_shared<NativePreSplitFirstPlacement>());
  state->placementRegistry->freeze();
  state->config = config;
  state->config.models.clear();
  state->config.nativeConfigPath = primary.path.string();
  state->baseDirectory = primary.path.parent_path();
  state->models.emplace("default", primary);
  try {
    const auto primaryJson = nativeParseJson(primary.canonicalJson);
    if (primaryJson.contains("conversation")) {
      if (!primaryJson.at("conversation").is_object())
        throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "conversation",
                      "conversation configuration must be an object");
      state->conversations = nativeConversationCoordinatorFromConfig(
        nativeCanonicalJson(primaryJson.at("conversation")), state->baseDirectory,
        primary.requesterIdentity);
    }
  }
  catch (const DiError&) {
    throw;
  }
  catch (const std::exception& error) {
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "conversation",
                  std::string("conversation configuration is invalid: ") + error.what());
  }
  // The key objects are transferred from freezeConfig.  Runtime never reads
  // operator key paths a second time after validation.
  coreOwner->operationRuntime = ndn_service_framework::OperationRuntime::create();
  state->coreOwner = coreOwner;
  state->offerAdmission = primary.offerAdmission;
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
    const auto frozen = freezeConfig(modelPath, coreOwner->face.get(),
                                     static_cast<bool>(config.repositorySourceLoader) ||
                                       static_cast<bool>(config.repositorySourceProvider));
    if (!sameTrustDomain(primary, frozen))
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "identity",
                    "registered model configuration crosses the Runtime trust domain");
    try {
      const auto registeredJson = nativeParseJson(frozen.canonicalJson);
      if (registeredJson.contains("conversation"))
        throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "conversation",
                      "secondary model registrations cannot declare a conversation coordinator");
    }
    catch (const DiError&) {
      throw;
    }
    catch (const std::exception& error) {
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "conversation",
                    std::string("secondary model configuration is invalid: ") + error.what());
    }
    state->config.models.push_back({registration.key, frozen.path.string()});
    state->models.emplace(registration.key, frozen);
  }
  return std::shared_ptr<Runtime>(new Runtime(std::move(state)));
}

std::shared_ptr<Runtime> Runtime::open(const ProviderConfig& config)
{
  if (!config.valid())
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "configuration",
                  "ProviderConfig is empty");
  auto state = std::make_shared<detail::RuntimeState>();
  state->providerOnly = true;
  state->providerConfig = config;
  try {
    state->provider = std::make_shared<Provider>(Provider::fromConfig(config));
  }
  catch (const std::exception& error) {
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "provider",
                  std::string("Provider cannot be opened: ") + error.what());
  }
  state->runtimeBinding = std::make_shared<std::uint8_t>(0);
  return std::shared_ptr<Runtime>(new Runtime(std::move(state)));
}

User Runtime::user(UserConfig config)
{
  if (!m_state)
    throw DiError("RUNTIME_CLOSED", "local", "lifecycle", "Runtime has no state");
  if (m_state->providerOnly)
    throw DiError("ROLE_UNAVAILABLE", "local", "user",
                  "Provider-only Runtime has no User directory");
  if (!config.profileName.empty() && config.profileName != "default")
    throw DiError("UNSUPPORTED_CAPABILITY", "local", "profile",
                  "only the default Runtime profile is supported");
  std::lock_guard<std::mutex> lock(m_state->mutex);
  if (m_state->phase != detail::RuntimeState::Phase::Open)
    throw DiError("RUNTIME_CLOSED", "local", "lifecycle", "Runtime is closed");
  return User(m_state, std::move(config.profileName));
}

std::shared_ptr<const PlacementStrategy>
Runtime::placementStrategy(const std::string& id) const
{
  if (!m_state)
    throw DiError("RUNTIME_CLOSED", "local", "placement", "Runtime has no state");
  std::lock_guard<std::mutex> lock(m_state->mutex);
  if (m_state->phase != detail::RuntimeState::Phase::Open ||
      !m_state->placementRegistry || !m_state->runtimeBinding)
    throw DiError("RUNTIME_CLOSED", "local", "placement", "Runtime is closed");
  const auto strategy = m_state->placementRegistry->find(id);
  if (!strategy)
    throw DiError("STRATEGY_NOT_FOUND", "local", "placement",
                  "placement strategy is not registered: " + id);
  return std::shared_ptr<const PlacementStrategy>(
    new PlacementStrategy(strategy, m_state->runtimeBinding));
}

Provider Runtime::provider(const ProviderConfig& config)
{
  if (!m_state)
    throw DiError("RUNTIME_CLOSED", "local", "provider", "Runtime has no state");
  if (!config.valid())
    throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "provider",
                  "ProviderConfig is empty");
  std::lock_guard<std::mutex> lock(m_state->mutex);
  if (m_state->phase != detail::RuntimeState::Phase::Open)
    throw DiError("RUNTIME_CLOSED", "local", "provider", "Runtime is closed");
  if (!m_state->providerOnly)
    throw DiError("CONFIG_CONFLICT", "local", "provider",
                  "requester Runtime cannot be converted to Provider-only Runtime");
  if (m_state->providerConfig.valid() &&
      !m_state->providerConfig.equivalent(config)) {
    throw DiError("CONFIG_CONFLICT", "local", "provider",
                  "ProviderConfig conflicts with the configured Provider");
  }
  if (!m_state->provider) {
    try {
      m_state->provider = std::make_shared<Provider>(Provider::fromConfig(config));
    }
    catch (const std::exception& error) {
      throw DiError("INVALID_RUNTIME_CONFIGURATION", "local", "provider",
                    std::string("Provider cannot be opened: ") + error.what());
    }
  }
  return *m_state->provider;
}

Provider Runtime::provider()
{
  if (!m_state)
    throw DiError("RUNTIME_CLOSED", "local", "provider", "Runtime has no state");
  std::lock_guard<std::mutex> lock(m_state->mutex);
  if (m_state->phase != detail::RuntimeState::Phase::Open)
    throw DiError("RUNTIME_CLOSED", "local", "provider", "Runtime is closed");
  if (!m_state->providerOnly || !m_state->provider)
    throw DiError("ROLE_UNAVAILABLE", "local", "provider",
                  "Runtime has no configured Provider");
  return *m_state->provider;
}

} // namespace ndnsf::di
