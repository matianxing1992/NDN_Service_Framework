#include "ndnsf-di/api.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativePhaseTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/runtime-logging.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/statistics-file.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/protected-preparation-index.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-filesystem-artifacts.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-owned-initializer-file.hpp"
#include "ndnsf-distributed-repo/RepoSourceProvider.hpp"
#include "ndnsf-distributed-repo/RepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndn-service-framework/PublishedContentName.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/scope_exit.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

NDN_LOG_INIT(ndnsf.di.NativeRequester);

bool
diagnosticOutputEnabled()
{
  return ndn_cxx_getLogger().isLevelEnabled(ndn::util::LogLevel::WARN);
}

void
logSpec220PreparationStage(const char* stage)
{
  NDN_LOG_DEBUG(std::string("SPEC220_PREP_STAGE requester.") + stage);
}

using namespace ndnsf::di;
using boost::property_tree::ptree;

constexpr std::uint64_t MAX_NATIVE_INPUT_BYTES = 16 * 1024 * 1024;

volatile std::sig_atomic_t interrupted = 0;

void
onSignal(int)
{
  interrupted = 1;
}

std::vector<std::uint8_t>
readBytes(const std::filesystem::path& path, std::uint64_t limit,
          std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max())
{
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    throw std::runtime_error("requester input file is unavailable: " + path.string());
  const auto size = input.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > limit)
    throw std::runtime_error("requester input file exceeds its configured limit");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.seekg(0);
  for (std::size_t offset = 0; offset < bytes.size();) {
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("requester input file deadline expired");
    const auto length = std::min<std::size_t>(1U << 20, bytes.size() - offset);
    if (!input.read(reinterpret_cast<char*>(bytes.data() + offset), length))
      throw std::runtime_error("requester input file read failed");
    offset += length;
  }
  return bytes;
}

ptree
readJson(const std::filesystem::path& path)
{
  ptree value;
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("requester configuration is unavailable: " + path.string());
  boost::property_tree::read_json(input, value);
  return value;
}

std::filesystem::path
relativeTo(const std::filesystem::path& base, const std::string& value)
{
  const auto path = std::filesystem::path(value);
  return path.is_absolute() ? path : base / path;
}

struct KvHandoffOptions
{
  std::size_t afterTurn = 0;
  std::chrono::milliseconds timeout{120000};
  bool waitForDeparture = false;
  std::map<std::string, std::string> destinations;
  std::map<std::string, NativeRemoteProtectedKvRoleTrustPolicyV1> trust;
};

KvHandoffOptions
readKvHandoffOptions(const ptree& value)
{
  KvHandoffOptions result;
  result.afterTurn = value.get<std::size_t>("after_turn");
  const auto timeout = value.get<int>("timeout_ms", 120000);
  if (timeout <= 0 || timeout > 600000)
    throw std::invalid_argument("kv_handoff timeout_ms must be in [1,600000]");
  result.timeout = std::chrono::milliseconds(timeout);
  result.waitForDeparture = value.get<bool>("wait_for_departure", false);
  for (const auto& entry : value.get_child("roles")) {
    if (entry.first.empty() || result.trust.size() >= 64)
      throw std::invalid_argument("kv_handoff requires bounded, named roles");
    const auto& role = entry.second;
    NativeRemoteProtectedKvRoleTrustPolicyV1 trust;
    trust.target.repoNode = role.get<std::string>("repo_node");
    trust.target.providerIdentity = role.get<std::string>("repo_provider_identity");
    trust.target.servicePrefix = role.get<std::string>("repo_service_prefix");
    if (trust.target.repoNode.empty() || trust.target.providerIdentity.empty() ||
        trust.target.servicePrefix.empty())
      throw std::invalid_argument("kv_handoff requires a complete Repo target");
    if (!result.waitForDeparture) {
      const auto destination = role.get<std::string>("destination_provider");
      if (destination.empty() || ndn::Name(destination).empty() ||
          !result.destinations.emplace(entry.first, destination).second)
        throw std::invalid_argument("kv_handoff has an invalid destination");
    }
    if (!result.trust.emplace(entry.first, std::move(trust)).second)
      throw std::invalid_argument("kv_handoff has an invalid or duplicate role");
  }
  if (result.trust.empty())
    throw std::invalid_argument("kv_handoff requires at least one role");
  return result;
}

std::vector<std::uint8_t>
readOptions(const ptree& request, const std::filesystem::path& base)
{
  const auto path = request.get_optional<std::string>("options_file");
  return path ? readBytes(relativeTo(base, *path), 4 * 1024 * 1024)
              : std::vector<std::uint8_t>{};
}

class CacheCompatibilityArtifactPublisher final : public RepositoryArtifactPublisher
{
public:
  explicit CacheCompatibilityArtifactPublisher(std::string sourceNamespace)
    : m_sourceNamespace(std::move(sourceNamespace))
  {
    if (m_sourceNamespace.rfind("sha256:", 0) != 0 || m_sourceNamespace.size() != 71 ||
        !std::all_of(m_sourceNamespace.begin() + 7, m_sourceNamespace.end(), [] (char value) {
          return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        }))
      throw std::invalid_argument("cache compatibility source namespace is invalid");
  }

  NativePreparedCanonicalPublication publish(
    const std::string& modelKey, const std::string& serviceName,
    const NativeInspectedModel& model, const NativeCanonicalSource& source,
    const NativeCanonicalPublicationOptions& options,
    const NativeRequestControl& control) const override
  {
    control.requireActive();
    model.validate();
    const bool hasPostSelectionMaterial = source.materializedRole ||
      !source.materialPayloads.empty() || !source.layerPayloads.empty();
    if (modelKey.empty() || serviceName.empty() || hasPostSelectionMaterial ||
        source.modelBytes.size() != model.canonicalSourceBytes ||
        nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size()) !=
          model.canonicalSourceDigest ||
        source.initializerBytes.has_value() != (model.canonicalInitializerBytes != 0) ||
        (source.initializerBytes &&
         (source.initializerBytes->size() != model.canonicalInitializerBytes ||
          nativePlanningDigest(source.initializerBytes->data(), source.initializerBytes->size()) !=
            model.canonicalInitializerObjectDigest)))
      throw std::runtime_error("DI_CACHE_COMPATIBILITY_SOURCE_NOT_PLAIN_CANONICAL");

    const auto prefix = std::string("/cache-compatible/") + m_sourceNamespace.substr(7);
    NativePreparedCanonicalPublication result;
    result.sourceDataName = prefix + "/source";
    result.rootDataName = prefix + "/root";
    result.artifactPrefetchRequired = false;
    if (model.canonicalInitializerBytes != 0)
      result.initializerDataName = prefix + "/initializer";
    result.artifactProfileDigest = options.artifactProfileDigest;
    result.layerManifestDigests = options.layerManifestDigests;
    for (const auto& digest : options.layerManifestDigests)
      result.layerDataNames.push_back(prefix + "/layer/" + digest.substr(7));

    NativeJson metadata{{"canonicalSourceBytes", model.canonicalSourceBytes},
      {"canonicalSourceDataName", result.sourceDataName},
      {"canonicalSourceDigest", model.canonicalSourceDigest},
      {"cacheCompatibilityNamespace", m_sourceNamespace}};
    if (model.canonicalInitializerBytes != 0) {
      metadata["canonicalInitializerBytes"] = model.canonicalInitializerBytes;
      metadata["canonicalInitializerDataName"] = result.initializerDataName;
      metadata["canonicalInitializerObjectDigest"] = model.canonicalInitializerObjectDigest;
    }
    if (!options.packageManifestDigest.empty())
      metadata["packageManifestDigest"] = options.packageManifestDigest;
    NativeJson root{{"schema", "ndnsf-di-canonical-model-manifest-v1"},
      {"state", "ACTIVE"}, {"artifactProfileDigest", options.artifactProfileDigest},
      {"modelIdentityDigest", model.descriptor.contentDigest},
      {"modelName", model.descriptor.modelName}, {"metadata", std::move(metadata)}};
    if (!options.layerManifestDigests.empty())
      root["layerManifestDigests"] = options.layerManifestDigests;
    result.canonicalManifestJson = nativeCanonicalJson(root);
    result.manifestDigest = nativePlanningDigest(result.canonicalManifestJson);
    result.publishedBytes = 0;
    result.rollbackOwned = false;
    result.validate();
    control.requireActive();
    return result;
  }

  void rollback(const NativePreparedCanonicalPublication&) const noexcept override
  {
    // The compatibility receipt owns no Core publication or serving lease.
  }

private:
  std::string m_sourceNamespace;
};

std::vector<std::string>
providerNames(const ptree& request)
{
  std::vector<std::string> result;
  const auto providers = request.get_child_optional("provider_names");
  if (!providers)
    return result;
  for (const auto& item : *providers) {
    if (!item.first.empty())
      throw std::invalid_argument("request.provider_names must be an array");
    const auto value = item.second.get_value<std::string>();
    if (value.empty() || value.front() != '/')
      throw std::invalid_argument("request.provider_names entries must be absolute names");
    result.push_back(value);
  }
  return result;
}

std::optional<std::vector<std::uint8_t>>
parentCheckpoint(const ptree& root, const std::filesystem::path& base)
{
  const auto conversation = root.get_child_optional("conversation");
  if (!conversation)
    return std::nullopt;
  const auto turn = conversation->get_child_optional("turn");
  if (!turn)
    return std::nullopt;
  const auto parent = turn->get_optional<std::string>("parent_state_file");
  if (!parent)
    return std::nullopt;
  const auto bytes = readBytes(relativeTo(base, *parent), 4 * 1024 * 1024);
  // The retired requester wrote a JSON envelope. Accept it while the public
  // API itself remains opaque and owns only the authenticated wire.
  try {
    std::stringstream stream(std::string(bytes.begin(), bytes.end()));
    ptree state;
    boost::property_tree::read_json(stream, state);
    if (const auto wire = state.get_optional<std::string>("checkpoint_wire"))
      return std::vector<std::uint8_t>(wire->begin(), wire->end());
  }
  catch (const std::exception&) {
    // A raw public checkpoint is also accepted.
  }
  return bytes;
}

std::vector<std::int64_t>
integerArray(const ptree& value, const std::string& key)
{
  try {
    std::vector<std::int64_t> result;
    const auto array = value.get_child_optional(key);
    if (!array)
      throw std::invalid_argument(key + " must be an array");
    for (const auto& item : *array) {
      if (!item.first.empty())
        throw std::invalid_argument(key + " must be an array");
      result.push_back(item.second.get_value<std::int64_t>());
    }
    return result;
  }
  catch (const std::exception& error) {
    throw std::runtime_error(std::string("NATIVE_STREAM_ORACLE_FAILED: ") + error.what());
  }
}

std::vector<float>
floatArray(const ptree& value, const std::string& key)
{
  std::vector<float> result;
  const auto array = value.get_child_optional(key);
  if (!array)
    throw std::invalid_argument(key + " must be an array");
  for (const auto& item : *array) {
    if (!item.first.empty())
      throw std::invalid_argument(key + " must be an array");
    result.push_back(item.second.get_value<float>());
  }
  return result;
}

void
validateStreamOracle(const ptree& root, const Result& result, std::size_t observed)
{
  try {
    const auto oracle = root.get_child_optional("stream_oracle");
    if (!oracle)
      return;
    const auto expected = integerArray(*oracle, "token_ids");
    if (expected.empty())
      throw std::invalid_argument("stream_oracle.token_ids must not be empty");
    std::stringstream stream(std::string(result.payload.begin(), result.payload.end()));
    ptree final;
    boost::property_tree::read_json(stream, final);
    if (final.get<std::string>("schema", {}) != "NDNSF-DI-FINAL-V1" ||
        integerArray(final, "tokenIds") != expected || observed != expected.size())
      throw std::runtime_error("final sequence mismatch");
    if (diagnosticOutputEnabled())
      std::cout << "NATIVE_STREAM_ORACLE_PASS tokens=" << expected.size()
                << " events=" << observed << '\n';
  }
  catch (const std::exception& error) {
    const std::string message = error.what();
    if (message.rfind("NATIVE_STREAM_ORACLE_FAILED: ", 0) == 0)
      throw;
    throw std::runtime_error("NATIVE_STREAM_ORACLE_FAILED: malformed final payload: " + message);
  }
}

void
validateStreamEvent(const std::vector<std::int64_t>& expected,
                    const Event& event, std::size_t index)
{
  try {
    std::stringstream stream(std::string(event.payload.begin(), event.payload.end()));
    ptree value;
    boost::property_tree::read_json(stream, value);
    if (value.get<std::string>("schema", {}) != "GenerationTokenEventV1" ||
        value.get<std::int64_t>("tokenId", std::numeric_limits<std::int64_t>::min()) != expected[index] ||
        value.get<std::uint64_t>("tokenEpoch", 0) != index + 1)
      throw std::runtime_error("event sequence mismatch");
  }
  catch (const std::exception& error) {
    const std::string message = error.what();
    if (message.rfind("NATIVE_STREAM_ORACLE_FAILED: ", 0) == 0)
      throw;
    throw std::runtime_error("NATIVE_STREAM_ORACLE_FAILED: malformed event: " + message);
  }
}

std::string
streamEventText(const Event& event)
{
  std::stringstream stream(std::string(event.payload.begin(), event.payload.end()));
  ptree value;
  boost::property_tree::read_json(stream, value);
  if (value.get<std::string>("schema", {}) != "GenerationTokenEventV1")
    throw std::runtime_error("NATIVE_STREAM_ORACLE_FAILED: malformed event schema");
  return value.get<std::string>("textDelta", {});
}

void
waitForTestStreamBarrier(const ptree& config, const std::filesystem::path& base,
                         std::size_t turnIndex, std::size_t observedEvents,
                         const char* configurationKey = "test_stream_barrier")
{
  if (turnIndex != 0 || observedEvents != 1)
    return;
  const auto barrier = config.get_child_optional(configurationKey);
  if (!barrier)
    return;
  if (!config.get<bool>("test_only", false))
    throw std::invalid_argument(std::string(configurationKey) + " requires test_only=true");
  const auto readyValue = barrier->get_optional<std::string>("ready_file");
  const auto releaseValue = barrier->get_optional<std::string>("release_file");
  const auto timeoutMs = barrier->get<std::uint64_t>("timeout_ms", 10000);
  const std::uint64_t timeoutLimitMs =
    std::string(configurationKey) == "test_handoff_barrier" &&
    config.get<bool>("kv_handoff.wait_for_departure", false) ? 650000 : 60000;
  if (!readyValue || !releaseValue || readyValue->empty() || releaseValue->empty() ||
      timeoutMs == 0 || timeoutMs > timeoutLimitMs)
    throw std::invalid_argument("test_stream_barrier is invalid");
  const auto ready = relativeTo(base, *readyValue);
  const auto release = relativeTo(base, *releaseValue);
  std::ofstream readyOutput(ready, std::ios::trunc);
  if (!readyOutput)
    throw std::runtime_error("test stream barrier ready file could not be written");
  readyOutput << "first-event-flushed\n";
  readyOutput.flush();
  const auto deadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(timeoutMs);
  while (!std::filesystem::exists(release)) {
    if (interrupted)
      throw std::runtime_error("NATIVE_REQUEST_INTERRUPTED");
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("test stream barrier release timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void
writeCheckpoint(const Conversation& conversation, const std::filesystem::path& path)
{
  const auto bytes = conversation.checkpoint().bytes();
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || (!bytes.empty() && !output.write(
      reinterpret_cast<const char*>(bytes.data()), bytes.size())))
    throw std::runtime_error("conversation checkpoint could not be written");
}

struct TurnSpec
{
  std::filesystem::path input;
  std::filesystem::path options;
  std::filesystem::path output;
  std::optional<std::filesystem::path> checkpoint;
  std::optional<std::string> conversationId;
};

std::vector<TurnSpec>
readTurnSpecs(const ptree& config, const std::filesystem::path& base,
             const std::optional<std::filesystem::path>& legacyInput,
             const std::optional<std::filesystem::path>& legacyOutput)
{
  const auto turns = config.get_child_optional("turns");
  if (!turns) {
    if (!legacyInput || !legacyOutput)
      throw std::invalid_argument(
        "requester turns are absent; provide --input and --output for a single turn");
    return {{*legacyInput, {}, *legacyOutput, std::nullopt, std::nullopt}};
  }

  std::vector<TurnSpec> result;
  for (const auto& item : *turns) {
    if (!item.first.empty())
      throw std::invalid_argument("turns must be an array");
    const auto input = item.second.get_optional<std::string>("input_file");
    const auto options = item.second.get_optional<std::string>("options_file");
    const auto output = item.second.get_optional<std::string>("output_file");
    if (!input || input->empty() || !options || options->empty() ||
        !output || output->empty())
      throw std::invalid_argument(
        "each turns entry requires input_file, options_file, and output_file");
    const auto checkpoint = item.second.get_optional<std::string>(
      "checkpoint_output_file");
    const auto conversationId = item.second.get_optional<std::string>("conversation_id");
    if (checkpoint && checkpoint->empty())
      throw std::invalid_argument("turn checkpoint_output_file must not be empty");
    if (conversationId && conversationId->empty())
      throw std::invalid_argument("turn conversation_id must not be empty");
    // Reject malformed explicit IDs before preparing the model. Conversation
    // remains responsible for admitting the ID when the conversation opens.
    if (conversationId && (conversationId->size() < 16 ||
                           conversationId->find_first_of("/\\") != std::string::npos))
      throw std::invalid_argument(
        "turn conversation_id must contain at least 16 characters and no slash");
    result.push_back({relativeTo(base, *input), relativeTo(base, *options),
                      relativeTo(base, *output), checkpoint
                        ? std::optional<std::filesystem::path>(relativeTo(base, *checkpoint))
                        : std::nullopt,
                      conversationId ? std::optional<std::string>(*conversationId)
                                     : std::nullopt});
  }
  if (result.empty())
    throw std::invalid_argument("turns must contain at least one entry");

  const bool grouped = result.front().conversationId.has_value();
  for (const auto& turn : result) {
    if (turn.conversationId.has_value() != grouped)
      throw std::invalid_argument("turn conversation_id must be present on every turn or none");
  }
  if (grouped) {
    if (result.size() > 10 || result.size() % 2 != 0)
      throw std::invalid_argument(
        "independent conversation groups require two turns each and at most five groups");
    std::set<std::string> conversationIds;
    std::vector<std::filesystem::path> readOnlyPaths;
    std::vector<std::filesystem::path> outputPaths;
    const auto canonicalPath = [] (const std::filesystem::path& path) {
      std::error_code error;
      auto canonical = std::filesystem::weakly_canonical(
        std::filesystem::absolute(path), error);
      if (error)
        throw std::invalid_argument("independent conversation path cannot be resolved");
      return canonical;
    };
    const auto refersToSameFile = [&] (const std::filesystem::path& first,
                                       const std::filesystem::path& second) {
      if (canonicalPath(first) == canonicalPath(second))
        return true;
      std::error_code error;
      const bool firstExists = std::filesystem::exists(first, error);
      if (error)
        throw std::invalid_argument("independent conversation path cannot be inspected");
      const bool secondExists = std::filesystem::exists(second, error);
      if (error)
        throw std::invalid_argument("independent conversation path cannot be inspected");
      if (!firstExists || !secondExists)
        return false;
      const bool equivalent = std::filesystem::equivalent(first, second, error);
      if (error)
        throw std::invalid_argument("independent conversation path cannot be compared");
      return equivalent;
    };
    for (std::size_t index = 0; index < result.size(); index += 2) {
      const auto& first = result[index];
      const auto& second = result[index + 1];
      const auto& conversationId = *first.conversationId;
      if (*second.conversationId != conversationId ||
          !conversationIds.insert(conversationId).second)
        throw std::invalid_argument(
          "each independent conversation_id must name exactly two adjacent turns");
      for (const auto* turn : {&first, &second}) {
        if (!turn->checkpoint)
          throw std::invalid_argument(
            "independent conversation turns require checkpoint_output_file");
        readOnlyPaths.push_back(turn->input);
        readOnlyPaths.push_back(turn->options);
      }
    }
    if (const auto statisticsFile = config.get_optional<std::string>("statistics_file")) {
      if (statisticsFile->empty())
        throw std::invalid_argument("statistics_file must not be empty");
      const auto path = relativeTo(base, *statisticsFile);
      for (const auto& source : readOnlyPaths) {
        if (refersToSameFile(path, source))
          throw std::invalid_argument(
            "statistics_file cannot overwrite turn input or options");
      }
      outputPaths.push_back(path);
    }
    for (const auto& turn : result) {
      for (const auto& destination : {turn.output, *turn.checkpoint}) {
        for (const auto& source : readOnlyPaths) {
          if (refersToSameFile(destination, source))
            throw std::invalid_argument(
              "independent conversation output cannot overwrite input, options, or statistics");
        }
        for (const auto& previous : outputPaths) {
          if (refersToSameFile(destination, previous))
            throw std::invalid_argument(
              "independent conversation output and checkpoint paths must be unique");
        }
        outputPaths.push_back(destination);
      }
    }
  }
  return result;
}

void
writeResult(const Result& result, const std::filesystem::path& path)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || (!result.payload.empty() && !output.write(
      reinterpret_cast<const char*>(result.payload.data()), result.payload.size())))
    throw std::runtime_error("requester output could not be written");
}

class NativeRepoSourceOwner final : public RepositorySourceProvider
{
public:
  NativeRepoSourceOwner(std::filesystem::path root, std::filesystem::path base,
                        ndn::Name producer,
                        std::shared_ptr<ndnsf_distributed_repo::RepoSourceProvider> preparedOwner,
                        std::filesystem::path preparedIndexRoot)
    : m_root(std::move(root)), m_base(std::move(base)), m_producer(std::move(producer)),
      m_preparedOwner(std::move(preparedOwner)),
      m_protectedIndex(std::move(preparedIndexRoot), m_producer),
      m_api(ndnsf_distributed_repo::ArtifactRepositoryApi::makeInApp(
        {m_root, m_producer.toUri(ndn::name::UriFormat::CANONICAL)}))
  {
    if (m_producer.empty())
      throw std::invalid_argument("native Repo source Producer is missing");
  }

  NativeCanonicalSource load(const RepositorySourceRequest& request,
                             const Fallback&) const override
  try {
    const auto catalog = nativeParseJson(request.catalogConfigurationJson);
    const auto& source = catalog.at("source");
    const auto checkDeadline = [&] {
      if (std::chrono::steady_clock::now() >= request.deadline)
        throw RepositorySourceError(RepositorySourceError::Kind::Timeout,
                                    "native Repo source deadline expired");
    };
    const auto loadFile = [&] (const char* fileField, const char* digestField,
                              const char* label, const auto& consume) {
      checkDeadline();
      const auto path = (m_base / source.at(fileField).get<std::string>()).lexically_normal();
      const auto digest = source.at(digestField).get<std::string>();
      const auto sizeField = std::string(label) == "source" ? "bytes" : "initializer_bytes";
      const auto size = source.contains(sizeField)
        ? source.at(sizeField).get<std::uint64_t>() : std::filesystem::file_size(path);
      if (digest.size() != 71 || digest.rfind("sha256:", 0) != 0 ||
          size == 0 || size > request.maxSourceBytes)
        throw RepositorySourceError(RepositorySourceError::Kind::Unavailable,
                                    "native Repo source identity or size is invalid");
      ndnsf_distributed_repo::ArtifactReference reference;
      const auto modelName = catalog.at("model").at("model_name").get<std::string>();
      const auto modelDigest = source.at("model_manifest_digest").get<std::string>();
      if (modelDigest.size() != 71 || modelDigest.rfind("sha256:", 0) != 0)
        throw std::runtime_error("native Repo source lacks verified model identity");
      const auto modelHash = ndn_service_framework::makeNamingDigestComponent(modelDigest.substr(7));
      ndn_service_framework::PublishedContentName semantic;
      semantic.kind = std::string(label) == "source"
        ? ndn_service_framework::ContentDataKind::ModelFile
        : ndn_service_framework::ContentDataKind::Initializer;
      semantic.label = path.filename().string();
      semantic.modelName = modelName;
      semantic.modelIdentity = ndn_service_framework::computeNamingDigest("ndnsf-naming/sdk-model", {
        {1, 2, std::vector<std::uint8_t>(modelName.begin(), modelName.end())},
        {2, 4, std::vector<std::uint8_t>(modelHash.value(), modelHash.value() + modelHash.value_size())}});
      auto name = semantic.contentPrefix(m_producer, digest.substr(7));
      reference.logicalName = name.toUri(ndn::name::UriFormat::CANONICAL);
      reference.publisherIdentity = m_producer.toUri(ndn::name::UriFormat::CANONICAL);
      reference.contentDigest = digest.substr(7);
      reference.sizeBytes = size;
      ndn_service_framework::checkNamingNameSize(name);
      reference.formatVersion = "raw-file-v1";
      reference.policyEpoch = "plaintext-v1";
      const auto existing = m_api.lookupLocalCommitUntil(reference, request.deadline);
      if (!existing) {
        const auto published = m_api.publishFileUntil(path.string(), reference, request.deadline);
        if (!published.handle || !published.receipt)
          throw std::runtime_error("native Repo source publication has no receipt");
      }
      checkDeadline();
      const auto directory = m_root / "materialized";
      std::filesystem::create_directories(directory);
      const auto pattern = (directory / (digest.substr(7) + "-XXXXXX")).string();
      std::vector<char> outputPattern(pattern.begin(), pattern.end());
      outputPattern.push_back('\0');
      int temporaryFd = -1;
      bool created = false;
      BOOST_SCOPE_EXIT_ALL(&) {
        if (temporaryFd >= 0) ::close(temporaryFd);
        if (created) ::unlink(outputPattern.data());
      };
      temporaryFd = ::mkstemp(outputPattern.data());
      if (temporaryFd < 0)
        throw std::runtime_error("native Repo source materialization could not be reserved");
      created = true;
      const auto closeStatus = ::close(temporaryFd);
      temporaryFd = -1;
      if (closeStatus != 0)
        throw std::runtime_error("native Repo source materialization reservation could not close");
      const std::filesystem::path output(outputPattern.data());
      m_api.fetchFileUntil(reference, output.string(), request.deadline);
      checkDeadline();
      auto value = consume(output, size, digest);
      checkDeadline();
      if (diagnosticOutputEnabled())
        std::cout << "NATIVE_REPO_SOURCE_API label=" << label << " bytes=" << size
                  << " deduplicated=" << existing.has_value() << std::endl;
      return value;
    };
    NativeCanonicalSource result;
    result.modelBytes = loadFile("file", "digest", "source",
      [&] (const auto& path, std::uint64_t size, const std::string& expected) {
        auto bytes = readBytes(path, request.maxSourceBytes, request.deadline);
        if (bytes.size() != size || nativePlanningDigest(bytes.data(), bytes.size()) != expected)
          throw std::runtime_error("native Repo source fetch failed content verification");
        return bytes;
      });
    if (source.contains("initializer_file"))
      result.initializerRangeSource = loadFile("initializer_file", "initializer_digest", "initializer",
        [&] (const auto& path, std::uint64_t size, const std::string& expected) {
          return takeNativeCanonicalInitializerFile(path.string(), size, expected,
            {request.deadline, checkDeadline, request.maxSourceBytes, request.maxSourceBytes});
        });
    return result;
  }
  catch (const std::exception&) {
    if (std::chrono::steady_clock::now() >= request.deadline)
      throw RepositorySourceError(RepositorySourceError::Kind::Timeout,
                                  "native Repo source deadline expired");
    throw;
  }

  std::optional<NativePreparedCanonicalPublication> lookupPrepared(
    const RepositoryPreparedLookupRequest& request) const override
  {
    if (auto prepared = m_protectedIndex.lookupWithLegacy(request, m_root))
      return prepared;
    return m_preparedOwner->lookupPrepared(request);
  }

  void rememberPrepared(const RepositoryPreparedLookupRequest& request,
                        const NativePreparedCanonicalPublication& publication) const override
  {
    m_protectedIndex.remember(request, publication);
  }

private:
  std::filesystem::path m_root;
  std::filesystem::path m_base;
  ndn::Name m_producer;
  std::shared_ptr<ndnsf_distributed_repo::RepoSourceProvider> m_preparedOwner;
  ProtectedPreparationIndex m_protectedIndex;
  mutable ndnsf_distributed_repo::ArtifactRepositoryApi m_api;
};

int
run(int argc, char** argv)
{
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "Usage: DI_NativeRequester --config FILE "
                 "[--prepare-only | --input FILE --output FILE]\n"
                 "Config schema: ndnsf-di-native-requester-v1\n"
                 "Optional statistics_file writes one JSONL timing row per turn.\n"
                 "A config with a turns array runs every turn in one native Conversation.\n"
                 "The request uses Runtime -> native model preparation -> User::request.\n";
    return 0;
  }
  const bool legacyArgs = argc == 7 && std::string(argv[1]) == "--config" &&
    std::string(argv[3]) == "--input" && std::string(argv[5]) == "--output";
  const bool turnsArgs = argc == 3 && std::string(argv[1]) == "--config";
  const bool prepareOnly = argc == 4 && std::string(argv[1]) == "--config" &&
    std::string(argv[3]) == "--prepare-only";
  if (!legacyArgs && !turnsArgs && !prepareOnly) {
    std::cerr << "Usage: DI_NativeRequester --config FILE "
                 "[--prepare-only | --input FILE --output FILE]\n";
    return 2;
  }

  const auto configPath = std::filesystem::absolute(argv[2]).lexically_normal();
  detail::configureSynchronousLogging(std::clog);
  const auto base = configPath.parent_path();
  const auto config = readJson(configPath);
  if (config.get<std::string>("schema", {}) != "ndnsf-di-native-requester-v1")
    throw std::invalid_argument("unsupported requester configuration schema");
  const auto modelInput = config.get_child_optional("model_input");
  const auto modelInputMode = modelInput
    ? modelInput->get<std::string>("mode", "") : std::string("source");
  if (modelInputMode != "source" && modelInputMode != "publish-stored" &&
      modelInputMode != "stored")
    throw std::invalid_argument("model_input.mode is unsupported");

  std::optional<ndnsf_distributed_repo::RepoArtifactTarget> modelRepository;
  if (const auto target = config.get_child_optional("model_repository")) {
    modelRepository.emplace(ndnsf_distributed_repo::RepoArtifactTarget{
      target->get<std::string>("repo_node"), target->get<std::string>("provider_identity"),
      target->get<std::string>("service_prefix")});
    nativeValidateModelRepositoryTarget(*modelRepository,
      ndn::Name(config.get<std::string>("core.requester_identity")));
  }
  std::optional<ndnsf_distributed_repo::RepoArtifactTarget> storedModelRepository;
  std::filesystem::path storedPreparationState;
  std::filesystem::path storedPrivateHandle;
  if (modelInputMode == "publish-stored" || (modelInputMode == "source" && !legacyArgs)) {
    if ((modelInputMode == "publish-stored" && !prepareOnly) || modelRepository ||
        config.get_child_optional("encrypted_repository") ||
        config.get_child_optional("cache_compatibility"))
      throw std::invalid_argument(
        "source/stored publication cannot select another publication mode; "
        "publish-stored requires --prepare-only");
    if (!modelInput)
      throw std::invalid_argument("source mode requires explicit model_input publication fields");
    const auto target = modelInput->get_child_optional("repo_target");
    if (!target)
      throw std::invalid_argument("model_input.repo_target is required for model preparation");
    storedModelRepository.emplace(ndnsf_distributed_repo::RepoArtifactTarget{
      target->get<std::string>("repo_node"), target->get<std::string>("provider_identity"),
      target->get<std::string>("service_prefix")});
    nativeValidateModelRepositoryTarget(*storedModelRepository,
      ndn::Name(config.get<std::string>("core.requester_identity")));
    const auto stateFile = modelInput->get_optional<std::string>("preparation_state_file");
    const auto handleFile = modelInput->get_optional<std::string>("private_handle_file");
    if (!stateFile || !handleFile || stateFile->empty() || handleFile->empty())
      throw std::invalid_argument(
        "model preparation requires preparation_state_file and private_handle_file");
    storedPreparationState = relativeTo(base, *stateFile);
    storedPrivateHandle = relativeTo(base, *handleFile);
    if (std::filesystem::absolute(storedPreparationState).lexically_normal() ==
        std::filesystem::absolute(storedPrivateHandle).lexically_normal())
      throw std::invalid_argument(
        "preparation_state_file and private_handle_file must be different files");
  }
  else if (modelInputMode == "stored") {
    if (prepareOnly || modelRepository || config.get_child_optional("encrypted_repository") ||
        config.get_child_optional("cache_compatibility") ||
        modelInput->get_optional<std::string>("preparation_state_file"))
      throw std::invalid_argument(
        "stored mode requires inference and cannot select source or publication inputs");
    const auto target = modelInput->get_child_optional("repo_target");
    const auto handleFile = modelInput->get_optional<std::string>("private_handle_file");
    if (!target || !handleFile || handleFile->empty())
      throw std::invalid_argument(
        "stored mode requires model_input.repo_target and private_handle_file");
    storedModelRepository.emplace(ndnsf_distributed_repo::RepoArtifactTarget{
      target->get<std::string>("repo_node"), target->get<std::string>("provider_identity"),
      target->get<std::string>("service_prefix")});
    storedPrivateHandle = relativeTo(base, *handleFile);
  }
  const auto& request = config.get_child("request");
  const auto turns = prepareOnly ? std::vector<TurnSpec>{} : readTurnSpecs(
    config, base,
    legacyArgs ? std::optional<std::filesystem::path>(
      std::filesystem::absolute(argv[4]).lexically_normal()) : std::nullopt,
    legacyArgs ? std::optional<std::filesystem::path>(
      std::filesystem::absolute(argv[6]).lexically_normal()) : std::nullopt);
  const bool groupedTurns = !turns.empty() && turns.front().conversationId.has_value();
  if (turns.size() > 1 && !config.get_child_optional("conversation"))
    throw std::invalid_argument("NATIVE_TURNS_REQUIRES_CONVERSATION");
  if (groupedTurns) {
    const auto conversationConfig = config.get_child_optional("conversation");
    if (!conversationConfig)
      throw std::invalid_argument("independent conversation turns require conversation options");
    const auto turn = conversationConfig->get_child_optional("turn");
    if (!turn || turn->get<std::string>("mode", {}) != "FULL_CONTEXT" ||
        turn->get_optional<std::string>("parent_state_file") ||
        turn->get_optional<std::string>("parent_checkpoint_digest") ||
        turn->get_optional<std::string>("conversation_id"))
      throw std::invalid_argument(
        "independent conversation turns require per-turn IDs, explicit FULL_CONTEXT, and no parent checkpoint");
    if (conversationConfig->get_optional<std::string>("checkpoint_output_file"))
      throw std::invalid_argument(
        "independent conversation turns require per-turn checkpoint_output_file values");
  }
  std::shared_ptr<ndnsf::di::detail::StatisticsFile> statisticsOutput;
  if (const auto path = config.get_optional<std::string>("statistics_file"))
    statisticsOutput = std::make_shared<ndnsf::di::detail::StatisticsFile>(
      relativeTo(base, *path).string());
  bool statisticsWriteFailed = false;

  RuntimeConfig runtimeConfig;
  runtimeConfig.nativeConfigPath = configPath.string();
  std::string fixedPlacementStrategyId;
  std::string requestPlacementStrategyId;
  const auto placementScoreMode = request.get<std::string>(
    "placement_score_mode", "preparation-and-network");
  if (const auto placement = request.get_optional<std::string>("placement_strategy")) {
    if (placement->empty())
      throw std::invalid_argument("request.placement_strategy must not be empty");
    if (config.get_child_optional("fixed_placement"))
      throw std::invalid_argument(
        "request.placement_strategy cannot be combined with fixed_placement");
    requestPlacementStrategyId = *placement;
  }
  if (placementScoreMode != "preparation-and-network" &&
      placementScoreMode != "network-only")
    throw std::invalid_argument("request.placement_score_mode is unsupported");
  if (placementScoreMode != "preparation-and-network" &&
      requestPlacementStrategyId != "native-observation-aware-v4")
    throw std::invalid_argument(
      "request.placement_score_mode requires native-observation-aware-v4");
  if (const auto fixedPlacement = config.get_child_optional("fixed_placement")) {
    const auto roleProviders = fixedPlacement->get_child_optional("role_providers");
    if (!roleProviders || roleProviders->empty())
      throw std::invalid_argument("fixed_placement.role_providers is required");
    std::map<std::string, std::string> providerByRole;
    for (const auto& item : *roleProviders) {
      if (item.first.empty() || item.second.data().empty() ||
          !providerByRole.emplace(item.first, item.second.data()).second)
        throw std::invalid_argument("fixed_placement.role_providers contains an invalid or duplicate role");
    }
    auto strategy = std::make_shared<NativeFixedProviderPlacement>(std::move(providerByRole));
    fixedPlacementStrategyId = strategy->identity().name;
    runtimeConfig.fixedPlacementStrategy = std::move(strategy);
  }
  if (const auto repository = config.get_child_optional("repository")) {
    auto path = std::filesystem::path(repository->get<std::string>("path"));
    if (path.is_relative())
      path = base / path;
    if (!std::filesystem::exists(path)) {
      std::filesystem::create_directories(path);
      std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    }
    ndnsf_distributed_repo::StorageCapability capability;
    capability.repoNode = "/local/canonical-model-materials";
    capability.repoMode = "persistent";
    capability.freeBytes = repository->get<std::uint64_t>("max_bytes", 4ULL << 30);
    auto repo = std::make_shared<ndnsf_distributed_repo::RepoCore>(
      std::move(capability), ndnsf_distributed_repo::makeFilesystemRepoStore(
        path.string(), 1U << 20, 1U << 20, "native-requester-canonical"));
    auto sourceOwner = std::make_shared<ndnsf_distributed_repo::RepoSourceProvider>(
      std::move(repo));
    auto sourceCachePath = std::filesystem::path(
      repository->get<std::string>("source_cache_path", (path / "native-artifact-api").string()));
    if (sourceCachePath.empty())
      throw std::invalid_argument("repository.source_cache_path must not be empty");
    if (sourceCachePath.is_relative())
      sourceCachePath = base / sourceCachePath;
    runtimeConfig.repositorySourceProvider = std::make_shared<NativeRepoSourceOwner>(
      std::move(sourceCachePath), base,
      ndn::Name(config.get<std::string>("core.requester_identity")), std::move(sourceOwner), path);
  }
  if (const auto compatibility = config.get_child_optional("cache_compatibility")) {
    if (compatibility->get<bool>("enabled", false)) {
      if (config.get_child_optional("encrypted_repository"))
        throw std::invalid_argument(
          "cache compatibility must not configure an encrypted repository");
      const auto sourceNamespace = compatibility->get<std::string>("source_namespace", {});
      const auto sourceDirectory = compatibility->get<std::string>("source_directory", {});
      if (!sourceDirectory.empty()) {
        runtimeConfig.repositorySourceProvider =
          std::make_shared<NativeFilesystemSourceProvider>(sourceDirectory);
        runtimeConfig.repositoryArtifactPublisher =
          std::make_shared<NativeFilesystemArtifactPublisher>(sourceDirectory,
            ndn::Name(config.get<std::string>("core.requester_identity")), sourceNamespace);
      }
      else {
        runtimeConfig.repositoryArtifactPublisher =
          std::make_shared<CacheCompatibilityArtifactPublisher>(sourceNamespace);
      }
      if (diagnosticOutputEnabled())
        std::cout << "NDNSF_DI_CACHE_COMPATIBILITY_REQUESTER enabled=true "
                   "protectedPublication=skipped" << std::endl;
    }
  }
  if (const auto repository = config.get_child_optional("encrypted_repository")) {
    auto path = std::filesystem::path(repository->get<std::string>("path"));
    if (path.is_relative())
      path = base / path;
    if (!std::filesystem::exists(path)) {
      std::filesystem::create_directories(path);
      std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    }
    ndnsf_distributed_repo::StorageCapability capability;
    capability.repoNode = "/local/encrypted-model-materials";
    capability.repoMode = "persistent";
    capability.freeBytes = repository->get<std::uint64_t>("max_bytes", 4ULL << 30);
    auto repo = std::make_shared<ndnsf_distributed_repo::RepoCore>(
      std::move(capability), ndnsf_distributed_repo::makeFilesystemRepoStore(
        path.string(), 1U << 20, 1U << 20, "native-requester"));
    runtimeConfig.encryptedRangeStore =
      std::make_shared<ndnsf_distributed_repo::RepoEncryptedLargeDataStore>(std::move(repo));
  }
  if (const auto limits = config.get_child_optional("limits")) {
    if (const auto value = limits->get_optional<std::uint64_t>("bootstrap_ms"))
      runtimeConfig.preparationJobTimeout = std::chrono::milliseconds(*value);
    if (const auto value = limits->get_optional<std::uint64_t>("max_prepared_bytes")) {
      if (*value > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("limits.max_prepared_bytes exceeds size_t");
      runtimeConfig.maxPreparedBytes = static_cast<std::size_t>(*value);
    }
    if (const auto value = limits->get_optional<std::uint64_t>("max_prepared_entries")) {
      if (*value > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("limits.max_prepared_entries exceeds size_t");
      runtimeConfig.maxPreparedEntries = static_cast<std::size_t>(*value);
    }
  }
  PrepareOptions prepareOptions;
  prepareOptions.timeout = runtimeConfig.preparationJobTimeout;
  logSpec220PreparationStage("runtime-open.begin");
  std::optional<KvHandoffOptions> kvHandoff;
  if (const auto handoffConfig = config.get_child_optional("kv_handoff");
      !prepareOnly && handoffConfig) {
    kvHandoff = readKvHandoffOptions(*handoffConfig);
    if (groupedTurns || !config.get_child_optional("conversation") || turns.size() < 2 ||
        kvHandoff->afterTurn >= turns.size() - 1)
      throw std::invalid_argument("kv_handoff requires a conversation and a following turn");
    if (config.get_child_optional("fixed_placement"))
      throw std::invalid_argument("kv_handoff requires live compatible-Provider placement");
  }
  auto runtime = Runtime::open(std::move(runtimeConfig));
  logSpec220PreparationStage("runtime-open.complete");
  std::shared_ptr<const PlacementStrategy> requestPlacementStrategy;
  if (!prepareOnly && !requestPlacementStrategyId.empty()) {
    requestPlacementStrategy = runtime->placementStrategy(requestPlacementStrategyId);
    if (diagnosticOutputEnabled())
      std::cout << "NATIVE_REQUEST_PLACEMENT strategy="
              << requestPlacementStrategyId << std::endl;
  }
  auto user = runtime->user();
  if (modelInputMode == "publish-stored") {
    const auto receipt = user.publishStoredModel("default", *storedModelRepository,
      storedPreparationState, storedPrivateHandle, prepareOptions.timeout);
    runtime->close();
    if (!runtime->drain(std::chrono::seconds(5)))
      throw std::runtime_error("NATIVE_PREPARATION_DRAIN_TIMEOUT");
    std::cout << nativeCanonicalJson(NativeJson{
      {"type", "ndnsf-di-stored-model-preparation"}, {"status", "PUBLISHED"},
      {"model", {
        {"producer", receipt.producer.toUri(ndn::name::UriFormat::CANONICAL)},
        {"repository", receipt.repository.toUri(ndn::name::UriFormat::CANONICAL)},
        {"rootName", receipt.rootName.toUri(ndn::name::UriFormat::CANONICAL)},
        {"rootWireDigest", receipt.rootWireDigest},
      }},
      {"sourceBytes", receipt.sourceBytes}, {"wireBytes", receipt.wireBytes},
      {"packetGroups", receipt.packetGroups}, {"objects", receipt.objectCount},
    }) << std::endl;
    return 0;
  }
  logSpec220PreparationStage("user-prepare.begin");
  std::optional<PreparedModel> preparedView;
  if (modelInputMode == "stored") {
    if (!storedModelRepository)
      throw std::logic_error("stored model Repo target was not configured");
    preparedView.emplace(user.openStoredModel("default", storedPrivateHandle,
      *storedModelRepository, prepareOptions.timeout));
  }
  else if (!legacyArgs) {
    if (!storedModelRepository)
      throw std::logic_error("source model Repo target was not configured");
    preparedView.emplace(user.prepareStoredModel("default", *storedModelRepository,
      storedPreparationState, storedPrivateHandle, prepareOptions.timeout));
  }
  else {
    preparedView.emplace(user.prepare("default", prepareOptions));
  }
  logSpec220PreparationStage("user-prepare.complete");
  std::optional<NativeModelRepositoryReceipt> stored;
  if (modelRepository)
    stored = user.storePreparedModel(*preparedView, *modelRepository, prepareOptions.timeout);
  if (prepareOnly) {
    const auto receipt = preparedView->receipt();
    preparedView.reset();
    runtime->close();
    if (!runtime->drain(std::chrono::seconds(5)))
      throw std::runtime_error("NATIVE_PREPARATION_DRAIN_TIMEOUT");
    NativeJson result{
      {"type", "ndnsf-di-native-preparation"}, {"status", "PREPARED"},
      {"preparationKeyDigest", receipt.preparationKeyDigest},
      {"manifestDigest", receipt.manifestDigest}, {"elapsedMs", receipt.elapsed.count()}
    };
    if (stored) {
      result["originalPacketRepository"] = NativeJson{
        {"repoNode", modelRepository->repoNode},
        {"providerIdentity", modelRepository->providerIdentity},
        {"servicePrefix", modelRepository->servicePrefix},
        {"producer", stored->producer.toUri(ndn::name::UriFormat::CANONICAL)},
        {"certificateName", stored->certificateName.toUri(ndn::name::UriFormat::CANONICAL)},
        {"rootName", stored->rootName.toUri(ndn::name::UriFormat::CANONICAL)},
        {"rootDigest", stored->rootDigest},
        {"metadataName", stored->metadataName.toUri(ndn::name::UriFormat::CANONICAL)},
        {"metadataDigest", stored->metadataDigest}, {"wireBytes", stored->wireBytes},
        {"packetGroups", stored->packetGroups}, {"protectedObjects", stored->protectedObjects}
      };
    }
    std::cout << nativeCanonicalJson(result) << std::endl;
    return 0;
  }
  const auto& prepared = *preparedView;

  RequestOptions options;
  options.timeout = std::chrono::milliseconds(
    request.get<std::uint64_t>("timeout_ms", options.timeout.count()));
  options.ackTimeout = std::chrono::milliseconds(
    request.get<std::uint64_t>("ack_timeout_ms", options.ackTimeout.count()));
  options.applicationRequestId = request.get<std::string>("application_request_id", {});
  options.providerNames = providerNames(request);
  if (requestPlacementStrategy) {
    options.placement = std::move(requestPlacementStrategy);
  }
  else if (!fixedPlacementStrategyId.empty()) {
    options.placement = runtime->placementStrategy(fixedPlacementStrategyId);
    if (diagnosticOutputEnabled())
      std::cout << "NATIVE_FIXED_PLACEMENT strategy=" << fixedPlacementStrategyId << std::endl;
  }
  if (options.timeout.count() <= 0 || options.ackTimeout.count() <= 0 ||
      options.ackTimeout >= options.timeout) {
    throw std::invalid_argument(
      "request ACK window must be positive and smaller than the request deadline");
  }
  if (diagnosticOutputEnabled())
    std::cout << "SPEC190_ACK_WINDOW_NATIVE {\"ackTimeoutMs\":"
              << options.ackTimeout.count()
              << ",\"requestTimeoutMs\":" << options.timeout.count()
              << "}" << std::endl;

  const auto generationMode = request.get<std::string>("generation_mode", "TOKEN_DIAGNOSTIC");
  const bool streaming = generationMode == "TOKEN_STREAMING" ||
    request.get<std::string>("output_mode", "FULL") == "TOKEN_STREAMING";
  if (streaming) {
    options.outputMode = "TOKEN_STREAMING";
    StreamOptions streamOptions;
    streamOptions.enabled = true;
    // A streamed request may be silent while a Provider performs the
    // authenticated post-Selection material fetch and runner assembly.  Keep
    // these values explicit in the requester contract instead of relying on
    // the generic 3x500ms defaults.  StreamRequestOptions::validate() still
    // enforces the protocol bounds before the request is published.
    if (const auto value = request.get_optional<std::uint64_t>(
          "interest_lifetime_ms")) {
      if (*value > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("request.interest_lifetime_ms exceeds uint32");
      streamOptions.interestLifetimeMs = static_cast<std::uint32_t>(*value);
    }
    if (const auto value = request.get_optional<std::uint64_t>(
          "max_event_retries")) {
      if (*value > std::numeric_limits<std::uint8_t>::max())
        throw std::invalid_argument("request.max_event_retries exceeds uint8");
      streamOptions.maxEventRetries = static_cast<std::uint8_t>(*value);
    }
    streamOptions.allowReplacement = request.get<bool>("allow_replacement", false);
    const auto maxReplacements = request.get<unsigned>(
      "max_replacements", streamOptions.allowReplacement ? 1u : 0u);
    if (maxReplacements > 1 ||
        (!streamOptions.allowReplacement && maxReplacements != 0))
      throw std::invalid_argument(
        "request replacement options require allow_replacement=true and max_replacements=1");
    streamOptions.maxReplacements = static_cast<std::uint8_t>(maxReplacements);
    options.stream = streamOptions;
    if (const auto maxTokens = request.get_optional<std::size_t>("max_new_tokens"))
      options.generation = GenerationOptions{*maxTokens};
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  if (diagnosticOutputEnabled())
    std::cout << "NATIVE_REQUEST_ROUTE=Runtime.open->User.prepare->User.request\n";

  std::optional<Conversation> conversation;
  std::optional<std::string> activeConversationId;
  if (const auto conversationConfig = config.get_child_optional("conversation");
      !groupedTurns && conversationConfig) {
    ConversationOptions conversationOptions;
    const auto turn = conversationConfig->get_child_optional("turn");
    const bool hasParent = turn && turn->get_optional<std::string>("parent_state_file").has_value();
    if (turn) {
      const auto mode = turn->get<std::string>("mode", hasParent ? "APPEND_DELTA" : "FULL_CONTEXT");
      const auto expectedMode = hasParent ? "APPEND_DELTA" : "FULL_CONTEXT";
      if (mode != expectedMode)
        throw std::runtime_error("DI_NATIVE_CONVERSATION_PARENT_MISMATCH: invalid continuation mode");
    }
    if (const auto turn = conversationConfig->get_child_optional("turn")) {
      if (const auto id = turn->get_optional<std::string>("conversation_id"))
        conversationOptions.conversationId = *id;
    }
    const auto checkpoint = parentCheckpoint(config, base);
    if (turn) {
      if (const auto expectedDigest = turn->get_optional<std::string>("parent_checkpoint_digest")) {
        if (!checkpoint)
          throw std::runtime_error("DI_NATIVE_CONVERSATION_PARENT_MISMATCH: parent checkpoint is missing");
        try {
          std::stringstream stream(std::string(checkpoint->begin(), checkpoint->end()));
          ptree wire;
          boost::property_tree::read_json(stream, wire);
          if (wire.get<std::string>("checkpointDigest", {}) != *expectedDigest)
            throw std::runtime_error("DI_NATIVE_CONVERSATION_PARENT_MISMATCH: checkpoint digest mismatch");
        }
        catch (const std::runtime_error&) {
          throw;
        }
        catch (const std::exception& error) {
          throw std::runtime_error(std::string("DI_NATIVE_CONVERSATION_PARENT_MISMATCH: malformed checkpoint: ") + error.what());
        }
      }
    }
    if (checkpoint)
      conversationOptions.checkpoint = ConversationCheckpoint::fromBytes(*checkpoint);
    logSpec220PreparationStage("conversation-open.begin");
    conversation.emplace(user.openConversation(prepared, conversationOptions));
    logSpec220PreparationStage("conversation-open.complete");
  }

  std::vector<std::int64_t> streamExpected;
  if (const auto oracle = config.get_child_optional("stream_oracle"))
    streamExpected = integerArray(*oracle, "token_ids");
  std::optional<ConversationHandoffHandle> providerSwitch;
  const auto finishProviderSwitch = [&] {
    if (providerSwitch) {
      providerSwitch->result(std::chrono::seconds(5));
      std::cout << "NATIVE_KV_HANDOFF_APPEND_SUCCEEDED operation="
                << providerSwitch->operationId() << std::endl;
      providerSwitch.reset();
    }
  };
  for (std::size_t turnIndex = 0; turnIndex < turns.size(); ++turnIndex) {
    finishProviderSwitch();
    if (kvHandoff && turnIndex == kvHandoff->afterTurn + 1) {
      const auto deadline = std::chrono::steady_clock::now() + kvHandoff->timeout;
      if (kvHandoff->waitForDeparture) {
        NativeDepartureWatchOptionsV1 watchOptions;
        watchOptions.deadline = deadline;
        watchOptions.saveTimeout = kvHandoff->timeout;
        watchOptions.trustedSourcePolicyByRole = kvHandoff->trust;
        auto watch = conversation->watchProviderDeparture(watchOptions);
        std::cout << "NATIVE_KV_DEPARTURE_WATCH_READY index=" << kvHandoff->afterTurn << std::endl;
        while (watch.status() == NativeDepartureWatchStatusV1::Watching ||
               watch.status() == NativeDepartureWatchStatusV1::Saving) {
          if (interrupted || std::chrono::steady_clock::now() >= deadline) {
            watch.close();
            throw std::runtime_error("KV departure watch interrupted or timed out");
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto saved = watch.result(std::chrono::milliseconds(0));
        watch.close();
        if (saved.status != NativeDepartureWatchStatusV1::Saved)
          throw std::runtime_error("KV departure save failed: " + saved.reason);
        std::cout << "NATIVE_KV_DEPARTURE_SAVED operation=" << saved.saveOperationId << std::endl;
      }
      else {
        providerSwitch = conversation->switchProviders(
          kvHandoff->destinations, kvHandoff->trust, deadline);
        while (providerSwitch->status() == ConversationHandoffStatus::Pending) {
          if (interrupted || std::chrono::steady_clock::now() >= deadline) {
            const auto accepted = providerSwitch->cancel();
            std::cout << "NATIVE_KV_HANDOFF_CANCEL_REQUESTED accepted="
                      << (accepted ? 1 : 0) << std::endl;
            throw std::runtime_error("KV Provider switch interrupted or timed out");
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (providerSwitch->status() != ConversationHandoffStatus::PromotedAwaitingAppend)
          providerSwitch->result(std::chrono::milliseconds(0));
        std::cout << "NATIVE_KV_HANDOFF_PROMOTED operation="
                  << providerSwitch->operationId() << std::endl;
      }
      // An initial source-only filter must not constrain the continuation.
      // Conversation supplies the authenticated destination map after a switch.
      options.providerNames.clear();
      waitForTestStreamBarrier(config, base, 0, 1, "test_handoff_barrier");
    }
    const auto& turn = turns[turnIndex];
    if (groupedTurns && (!activeConversationId ||
                         *activeConversationId != *turn.conversationId)) {
      if (conversation) {
        conversation->close();
        conversation.reset();
      }
      ConversationOptions conversationOptions;
      conversationOptions.conversationId = *turn.conversationId;
      logSpec220PreparationStage("conversation-open.begin");
      conversation.emplace(user.openConversation(prepared, conversationOptions));
      activeConversationId = *turn.conversationId;
      logSpec220PreparationStage("conversation-open.complete");
    }
    Result result;
    std::string statisticsRequestId;
    bool statisticsCompleted = false;
    std::size_t observedEvents = 0;
    bool terminalSeen = false;
    bool checkpointWritten = false;
    options.workStatistics = statisticsOutput
      ? makeNativeWorkStatistics() : nullptr;
    std::int64_t requestSubmittedSteadyNs = 0;
    std::int64_t requestSubmittedCpuNs = -1;
    const auto statisticsStartedCpu = statisticsOutput
      ? ndnsf::di::detail::nativeProcessCpuTimeNs() : -1;
    const auto statisticsStarted = statisticsOutput
      ? std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count() : 0;
    BOOST_SCOPE_EXIT_ALL(&) {
      if (statisticsOutput) {
        try {
          const auto finished = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
          const auto finishedCpu = ndnsf::di::detail::nativeProcessCpuTimeNs();
          const auto clockDomain = nativeWorkClockDomain();
          statisticsOutput->write(NativeJson{
            {"schema", "ndnsf-di-native-requester-statistics-v1"}, {"type", "request"},
            {"turn", turnIndex}, {"requestId", statisticsRequestId},
            {"planDigest", result.planDigest}, {"completed", statisticsCompleted},
            {"streaming", streaming}, {"observedEvents", observedEvents},
            {"terminalSeen", terminalSeen}, {"checkpointWritten", checkpointWritten},
            {"startedSteadyNs", statisticsStarted}, {"finishedSteadyNs", finished},
            {"requestSubmittedSteadyNs", requestSubmittedSteadyNs == 0
              ? NativeJson(nullptr) : NativeJson(requestSubmittedSteadyNs)},
            {"requestSubmittedProcessCpuNs", requestSubmittedCpuNs < 0
              ? NativeJson(nullptr) : NativeJson(requestSubmittedCpuNs)},
            {"startedBoundary", "before-input-and-options-read"},
            {"submittedBoundary", "before-native-request-call"},
            {"clockDomain", clockDomain ? NativeJson(*clockDomain) : NativeJson(nullptr)},
            {"workStatistics", options.workStatistics
              ? nativeParseJson(nativeWorkStatisticsToJson(options.workStatistics->snapshot()))
              : NativeJson(nullptr)},
            {"startedProcessCpuNs", statisticsStartedCpu < 0
              ? NativeJson(nullptr) : NativeJson(statisticsStartedCpu)},
            {"finishedProcessCpuNs", finishedCpu < 0
              ? NativeJson(nullptr) : NativeJson(finishedCpu)}}.dump());
        }
        catch (...) {
          statisticsWriteFailed = true;
        }
      }
    };
    const auto payload = readBytes(turn.input, MAX_NATIVE_INPUT_BYTES);
    const auto optionsBytes = turn.options.empty()
      ? readOptions(request, base) : readBytes(turn.options, 4 * 1024 * 1024);
    if (options.workStatistics) {
      options.workStatistics->addCount("request.inputPlaintextBytes", payload.size());
      options.workStatistics->addCount("request.optionsPlaintextBytes", optionsBytes.size());
    }
    if (statisticsOutput) {
      requestSubmittedCpuNs = ndnsf::di::detail::nativeProcessCpuTimeNs();
      requestSubmittedSteadyNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    RequestHandle handle = conversation
      ? conversation->request(Input::inlineBytes(payload, optionsBytes), options)
      : user.request(prepared, Input::inlineBytes(payload, optionsBytes), options);
    if (statisticsOutput) {
      try {
        statisticsRequestId = handle.id();
      }
      catch (const std::bad_alloc&) {
        statisticsWriteFailed = true;
      }
    }
    if (diagnosticOutputEnabled())
      std::cout << "NATIVE_CONVERSATION_TURN_START index=" << turnIndex
                << " request=" << handle.id() << std::endl;

    if (streaming) {
      auto reader = handle.events();
      bool livePendingReported = false;
      if (diagnosticOutputEnabled())
        std::cout << "NATIVE_STREAM_READER_OPENED index=" << turnIndex
                  << " request=" << handle.id() << std::endl;
      while (!terminalSeen) {
        if (interrupted) {
          handle.cancel();
          throw std::runtime_error("NATIVE_REQUEST_INTERRUPTED");
        }
        const auto remaining = reader.remainingTimeout();
        if (remaining.count() <= 0) {
          handle.cancel();
          throw std::runtime_error(
            "NATIVE_STREAM_ORACLE_FAILED: terminal event timeout");
        }
        const auto wait = std::min(remaining, std::chrono::milliseconds(100));
        if (!livePendingReported && observedEvents == 0 &&
            handle.status() == RequestStatus::Pending) {
          if (diagnosticOutputEnabled())
            std::cout << "NATIVE_STREAM_LIVE_PENDING index=" << turnIndex
                    << " request=" << handle.id() << std::endl;
          livePendingReported = true;
        }
        std::optional<Event> event;
        try {
          event = reader.next(wait);
        }
        catch (const DiError& error) {
          // EventReader::next reports a bounded local read timeout as
          // WAIT_TIMEOUT.  A streamed request may legitimately remain quiet
          // while ACK/Selection completes authenticated material fetch and
          // runner assembly; only the absolute request deadline is terminal.
          if (error.code() != "WAIT_TIMEOUT")
            throw;
          if (handle.status() == RequestStatus::Pending)
            continue;
          result = handle.result(std::chrono::milliseconds(0));
          throw std::runtime_error(
            "NATIVE_STREAM_ORACLE_FAILED: terminal event missing");
        }
        if (!event) {
          // A local read timeout is not EOF.  Continue until the business
          // deadline; a failed handle is surfaced by result() below.
          if (handle.status() != RequestStatus::Pending) {
            result = handle.result(std::chrono::milliseconds(0));
            throw std::runtime_error(
              "NATIVE_STREAM_ORACLE_FAILED: terminal event missing");
          }
          continue;
        }
        if (event->terminal) {
          terminalSeen = true;
          if (diagnosticOutputEnabled())
            std::cout << "NATIVE_STREAM_TERMINAL index=" << turnIndex
                    << " request=" << handle.id() << std::endl;
          break;
        }
        const auto firstEventStatus = observedEvents == 0
          ? handle.status() : RequestStatus::Pending;
        const bool firstEventPending = observedEvents == 0 &&
          firstEventStatus == RequestStatus::Pending;
        if (observedEvents == 0 && config.get_child_optional("test_stream_barrier") &&
            !firstEventPending)
          throw std::runtime_error(
            "NATIVE_STREAM_ORACLE_FAILED: first event arrived after native completion");
        if (!streamExpected.empty()) {
          if (observedEvents >= streamExpected.size())
            throw std::runtime_error("NATIVE_STREAM_ORACLE_FAILED: too many events");
          validateStreamEvent(streamExpected, *event, observedEvents);
        }
        const bool firstEvent = observedEvents == 0;
        std::cout << streamEventText(*event);
        ++observedEvents;
        // NativeInferenceClient records API queue delivery separately.  This
        // marker follows actual text output from the requester executable and
        // its flush boundary, with a distinct execution role.
        std::cout.flush();
        if (firstEvent && diagnosticOutputEnabled()) {
          std::cout << "NATIVE_STREAM_FIRST_EVENT index=" << turnIndex
                    << " request=" << handle.id() << std::endl;
          std::cout << "NATIVE_STREAM_FIRST_EVENT_STATUS="
                    << (firstEventPending ? "PENDING" : "TERMINAL")
                    << " index=" << turnIndex << " request=" << handle.id() << std::endl;
        }
        if (diagnosticOutputEnabled()) {
          logRuntimePhase(
            "di-cli", "tokenEmitted", handle.id(), "request",
            {{"executionRole", "cli-output"},
             {"conversationId", "none"},
             {"tokenIndex", std::to_string(observedEvents)}});
        }
        waitForTestStreamBarrier(config, base, turnIndex, observedEvents);
      }
      auto completionWait = reader.remainingTimeout();
      if (completionWait.count() <= 0)
        completionWait = std::chrono::milliseconds(100);
      result = handle.result(completionWait);
      if (diagnosticOutputEnabled())
        std::cout << "NATIVE_STREAM_EVENTS index=" << turnIndex
                  << " NATIVE_STREAM_EVENTS=" << observedEvents << '\n';
    }
    else {
      result = handle.result();
    }
    if (streaming && !terminalSeen)
      throw std::runtime_error("NATIVE_STREAM_ORACLE_FAILED: terminal event missing");
    validateStreamOracle(config, result, observedEvents);
    if (const auto oracle = config.get_child_optional("oracle")) {
      const auto tensor = oracle->get<std::string>("tensor", {});
      const auto expected = floatArray(*oracle, "float32");
      const auto tolerance = oracle->get<double>("tolerance", 1e-5);
      if (tensor.empty() || expected.empty() || !std::isfinite(tolerance) || tolerance < 0.0 ||
          !result.matchesFloat32Tensor(tensor, expected, tolerance))
        throw std::runtime_error("NATIVE_NUMERICAL_ORACLE_FAILED: tensor value mismatch");
      if (diagnosticOutputEnabled())
        std::cout << "NATIVE_NUMERICAL_ORACLE_PASS tensor=" << tensor
                  << " values=" << expected.size() << '\n';
    }
    if (conversation) {
      if (const auto conversationConfig = config.get_child_optional("conversation")) {
        std::optional<std::filesystem::path> path;
        if (turn.checkpoint)
          path = *turn.checkpoint;
        else if (const auto configured = conversationConfig->get_optional<std::string>(
                   "checkpoint_output_file"))
          path = relativeTo(base, *configured);
        if (path) {
          writeCheckpoint(*conversation, *path);
          checkpointWritten = true;
          if (diagnosticOutputEnabled())
            std::cout << "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN index="
                      << turnIndex << '\n';
        }
      }
    }
    writeResult(result, turn.output);
    if (diagnosticOutputEnabled()) {
      std::cout << "NATIVE_CONVERSATION_TURN_COMPLETE index=" << turnIndex
                << " request=" << handle.id() << " plan=" << result.planDigest << '\n';
      std::cout << "NATIVE_REQUEST_SUCCEEDED request=" << handle.id()
                << " plan=" << result.planDigest << '\n';
    }
    statisticsCompleted = true;
  }
  finishProviderSwitch();
  if (turns.size() > 1 && diagnosticOutputEnabled())
    std::cout << "NATIVE_CONVERSATION_TURNS_SUCCEEDED count=" << turns.size() << '\n';
  if (groupedTurns && conversation) {
    conversation->close();
    conversation.reset();
  }
  runtime->close();
  (void)runtime->drain(std::chrono::seconds(5));
  if (statisticsWriteFailed)
    throw std::runtime_error("NATIVE_STATISTICS_WRITE_FAILED");
  return 0;
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    return run(argc, argv);
  }
  catch (const DiError& error) {
    std::cerr << "NATIVE_REQUEST_STAGE_FAILED code=" << error.code()
              << " boundary=" << error.boundary() << '\n';
    std::cerr << error.code() << " domain=" << error.domain()
              << " boundary=" << error.boundary() << " message=" << error.what() << '\n';
    return interrupted ? 130 : 1;
  }
  catch (const std::exception& error) {
    std::cerr << "NATIVE_REQUESTER_FAILED: " << error.what() << '\n';
    return 1;
  }
}
