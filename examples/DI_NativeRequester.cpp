#include "ndnsf-di/api.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "ndnsf-distributed-repo/RepoSourceProvider.hpp"
#include "ndnsf-distributed-repo/RepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndn-service-framework/PublishedContentName.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/io.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <cmath>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace ndnsf::di;
using boost::property_tree::ptree;

constexpr std::uint64_t MAX_NATIVE_INPUT_BYTES = 16 * 1024 * 1024;

std::uint64_t
oneHourRetentionDeadlineMs()
{
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  return static_cast<std::uint64_t>(now) + 3'600'000;
}

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
readKvHandoffOptions(const ptree& value, const std::filesystem::path& base)
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
    const auto producer = ndn::Name(role.get<std::string>("producer_identity"));
    const auto certificatePath = relativeTo(base, role.get<std::string>("producer_certificate"));
    const auto bytes = readBytes(certificatePath, 64 * 1024);
    std::istringstream certificateInput(std::string(bytes.begin(), bytes.end()));
    const auto certificate = ndn::io::load<ndn::security::Certificate>(certificateInput);
    if (!certificate || producer.empty() || certificate->getIdentity() != producer ||
        !certificate->getValidityPeriod().isValid())
      throw std::invalid_argument("kv_handoff requires a valid pinned Producer certificate");
    NativeRemoteProtectedKvRoleTrustPolicyV1 trust;
    trust.target.repoNode = role.get<std::string>("repo_node");
    trust.target.providerIdentity = role.get<std::string>("repo_provider_identity");
    trust.target.servicePrefix = role.get<std::string>("repo_service_prefix");
    if (trust.target.repoNode.empty() || trust.target.providerIdentity.empty() ||
        trust.target.servicePrefix.empty())
      throw std::invalid_argument("kv_handoff requires a complete Repo target");
    // The operator pins the complete certificate, including its namespace and key.
    trust.producerVerifierPolicyDigest = nativePlanningDigest(
      std::string(bytes.begin(), bytes.end()));
    trust.verifyProducerSignature = [certificate, producer](const ndn::Data& data) {
      return certificate->getValidityPeriod().isValid() && producer.isPrefixOf(data.getName()) &&
        ndn::security::verifySignature(data, *certificate);
    };
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
  readyOutput << (std::string(configurationKey) == "test_stream_barrier" ?
                   "first-event-flushed" : configurationKey) << "\n";
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
waitForTestExecutionBarrier(const ptree& root, const ptree& barrier,
                            const std::filesystem::path& base,
                            const char* barrierName)
{
  if (!root.get<bool>("test_only", false))
    throw std::invalid_argument(std::string(barrierName) + " requires test_only=true");
  const auto readyValue = barrier.get_optional<std::string>("ready_file");
  const auto releaseValue = barrier.get_optional<std::string>("release_file");
  const auto timeoutMs = barrier.get<std::uint64_t>("timeout_ms", 10000);
  if (!readyValue || readyValue->empty() || !releaseValue || releaseValue->empty() ||
      timeoutMs == 0 || timeoutMs > 60000)
    throw std::invalid_argument(std::string(barrierName) + " is invalid");
  const auto ready = relativeTo(base, *readyValue);
  const auto release = relativeTo(base, *releaseValue);
  if (ready == release)
    throw std::invalid_argument(std::string(barrierName) + " ready and release paths must differ");
  if (std::filesystem::exists(ready) || std::filesystem::exists(release))
    throw std::runtime_error(std::string(barrierName) + " has a stale marker");
  std::ofstream readyOutput(ready, std::ios::trunc);
  if (!readyOutput)
    throw std::runtime_error(std::string(barrierName) + " ready file could not be written");
  readyOutput << barrierName << "\n";
  readyOutput.flush();
  if (!readyOutput)
    throw std::runtime_error(std::string(barrierName) + " ready file could not be flushed");
  readyOutput.close();
  if (!readyOutput)
    throw std::runtime_error(std::string(barrierName) + " ready file could not be closed");
  std::cout << "NATIVE_TEST_EXECUTION_BARRIER_READY name=" << barrierName << std::endl;
  const auto deadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(timeoutMs);
  while (!std::filesystem::exists(release)) {
    if (interrupted)
      throw std::runtime_error("NATIVE_REQUEST_INTERRUPTED");
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error(std::string(barrierName) + " release timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::cout << "NATIVE_TEST_EXECUTION_BARRIER_RELEASED name=" << barrierName << std::endl;
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
  std::optional<std::chrono::milliseconds> requestTimeout;
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
    std::optional<std::chrono::milliseconds> requestTimeout;
    if (item.second.count("request_timeout_ms") > 1)
      throw std::invalid_argument("turn request_timeout_ms must not be repeated");
    if (const auto value = item.second.get_child_optional("request_timeout_ms")) {
      const auto timeoutMs = value->get_value<std::uint64_t>();
      if (timeoutMs == 0 || timeoutMs > 3600000)
        throw std::invalid_argument("turn request_timeout_ms must be in [1,3600000]");
      requestTimeout = std::chrono::milliseconds(timeoutMs);
    }
    result.push_back({relativeTo(base, *input), relativeTo(base, *options),
                      relativeTo(base, *output), checkpoint
                        ? std::optional<std::filesystem::path>(relativeTo(base, *checkpoint))
                        : std::nullopt, requestTimeout});
  }
  if (result.empty())
    throw std::invalid_argument("turns must contain at least one entry");
  return result;
}

RequestOptions
requestOptionsForTurn(const RequestOptions& base, const TurnSpec& turn)
{
  if (turn.requestTimeout &&
      (*turn.requestTimeout <= base.ackTimeout || *turn.requestTimeout > base.timeout))
    throw std::invalid_argument(
      "turn request deadline must exceed its ACK window and not exceed the base deadline");
  auto options = base;
  if (turn.requestTimeout)
    options.timeout = *turn.requestTimeout;
  return options;
}

void
writeResult(const Result& result, const std::filesystem::path& path)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || (!result.payload.empty() && !output.write(
      reinterpret_cast<const char*>(result.payload.data()), result.payload.size())))
    throw std::runtime_error("requester output could not be written");
}

std::string
executeConversationTurn(const ptree& config, const ptree& request,
                        const std::filesystem::path& base, const TurnSpec& turn,
                        std::size_t turnIndex, const RequestOptions& baseOptions,
                        bool streaming, const std::vector<std::int64_t>& streamExpected,
                        const User& user, const PreparedModel& prepared,
                        Conversation* conversation)
{
  const auto options = requestOptionsForTurn(baseOptions, turn);
  const auto payload = readBytes(turn.input, MAX_NATIVE_INPUT_BYTES);
  const auto optionsBytes = turn.options.empty()
    ? readOptions(request, base) : readBytes(turn.options, 4 * 1024 * 1024);
  RequestHandle handle = conversation
    ? conversation->request(Input::inlineBytes(payload, optionsBytes), options)
    : user.request(prepared, Input::inlineBytes(payload, optionsBytes), options);
  const auto requestId = handle.id();
  std::cout << "NATIVE_CONVERSATION_TURN_BUDGET index=" << turnIndex
            << " timeoutMs=" << options.timeout.count()
            << " ackTimeoutMs=" << options.ackTimeout.count() << std::endl;
  std::cout << "NATIVE_CONVERSATION_TURN_START index=" << turnIndex
            << " request=" << requestId << std::endl;

  std::size_t observedEvents = 0;
  bool terminalSeen = false;
  Result result;
  if (streaming) {
    auto reader = handle.events();
    bool livePendingReported = false;
    std::cout << "NATIVE_STREAM_READER_OPENED index=" << turnIndex
              << " request=" << requestId << std::endl;
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
        std::cout << "NATIVE_STREAM_LIVE_PENDING index=" << turnIndex
                  << " request=" << requestId << std::endl;
        livePendingReported = true;
      }
      std::optional<Event> event;
      try {
        event = reader.next(wait);
      }
      catch (const DiError& error) {
        // EventReader::next reports a bounded local read timeout as
        // WAIT_TIMEOUT. A streamed request may remain quiet while ACK/Selection
        // completes authenticated material fetch and runner assembly; only
        // the absolute request deadline is terminal.
        if (error.code() != "WAIT_TIMEOUT")
          throw;
        if (handle.status() == RequestStatus::Pending)
          continue;
        result = handle.result(std::chrono::milliseconds(0));
        throw std::runtime_error(
          "NATIVE_STREAM_ORACLE_FAILED: terminal event missing");
      }
      if (!event) {
        // A local read timeout is not EOF. Continue until the business
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
        std::cout << "NATIVE_STREAM_TERMINAL index=" << turnIndex
                  << " request=" << requestId << std::endl;
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
      // NativeInferenceClient records API queue delivery separately. This
      // marker follows actual text output and its flush boundary.
      std::cout.flush();
      if (firstEvent) {
        std::cout << "NATIVE_STREAM_FIRST_EVENT index=" << turnIndex
                  << " request=" << requestId << std::endl;
        std::cout << "NATIVE_STREAM_FIRST_EVENT_STATUS="
                  << (firstEventPending ? "PENDING" : "TERMINAL")
                  << " index=" << turnIndex << " request=" << requestId << std::endl;
      }
      logRuntimePhase(
        "di-cli", "tokenEmitted", requestId, "request",
        {{"executionRole", "cli-output"},
         {"conversationId", "none"},
         {"tokenIndex", std::to_string(observedEvents)}});
      waitForTestStreamBarrier(config, base, turnIndex, observedEvents);
    }
    auto completionWait = reader.remainingTimeout();
    if (completionWait.count() <= 0)
      completionWait = std::chrono::milliseconds(100);
    result = handle.result(completionWait);
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
    std::cout << "NATIVE_NUMERICAL_ORACLE_PASS tensor=" << tensor
              << " values=" << expected.size() << '\n';
  }
  if (conversation) {
    const auto conversationConfig = config.get_child_optional("conversation");
    if (!conversationConfig)
      throw std::invalid_argument("conversation checkpoint config is missing");
    std::optional<std::filesystem::path> path;
    if (turn.checkpoint)
      path = *turn.checkpoint;
    else if (const auto configured = conversationConfig->get_optional<std::string>(
               "checkpoint_output_file"))
      path = relativeTo(base, *configured);
    if (path) {
      writeCheckpoint(*conversation, *path);
      std::cout << "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN index="
                << turnIndex << '\n';
    }
  }
  writeResult(result, turn.output);
  std::cout << "NATIVE_CONVERSATION_TURN_COMPLETE index=" << turnIndex
            << " request=" << requestId << " plan=" << result.planDigest << '\n';
  std::cout << "NATIVE_REQUEST_SUCCEEDED request=" << requestId
            << " plan=" << result.planDigest << '\n';
  return requestId;
}

class NativeRepoSourceOwner final : public RepositorySourceProvider
{
public:
  NativeRepoSourceOwner(std::filesystem::path root, std::filesystem::path base,
                        ndn::Name producer,
                        std::shared_ptr<ndnsf_distributed_repo::RepoSourceProvider> preparedOwner)
    : m_root(std::move(root)), m_base(std::move(base)), m_producer(std::move(producer)),
      m_preparedOwner(std::move(preparedOwner)),
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
    static std::atomic<std::uint64_t> materializationSequence{0};
    const auto loadFile = [&] (const char* fileField, const char* digestField,
                              const char* label) {
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
      const auto outputName = digest.substr(7) + "-" + std::to_string(::getpid()) + "-" +
        std::to_string(materializationSequence.fetch_add(1, std::memory_order_relaxed));
      const auto output = m_root / "materialized" / outputName;
      std::filesystem::create_directories(output.parent_path());
      struct RemoveMaterializedFile {
        std::filesystem::path path;
        ~RemoveMaterializedFile()
        {
          std::error_code error;
          std::filesystem::remove(path, error);
        }
      } cleanup{output};
      m_api.fetchFileUntil(reference, output.string(), request.deadline);
      auto bytes = readBytes(output, request.maxSourceBytes, request.deadline);
      const auto fetchedDigest = nativePlanningDigest(bytes.data(), bytes.size());
      if (bytes.size() != size || fetchedDigest != digest) {
        throw std::runtime_error("native Repo source fetch failed content verification: expectedBytes=" +
          std::to_string(size) + " actualBytes=" + std::to_string(bytes.size()) +
          " expectedDigest=" + digest + " actualDigest=" + fetchedDigest);
      }
      checkDeadline();
      std::cout << "NATIVE_REPO_SOURCE_API label=" << label << " bytes=" << size
                << " deduplicated=" << existing.has_value() << std::endl;
      return bytes;
    };
    NativeCanonicalSource result;
    result.modelBytes = loadFile("file", "digest", "source");
    if (source.contains("initializer_file"))
      result.initializerBytes = loadFile("initializer_file", "initializer_digest", "initializer");
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
    return m_preparedOwner->lookupPrepared(request);
  }

private:
  std::filesystem::path m_root;
  std::filesystem::path m_base;
  ndn::Name m_producer;
  std::shared_ptr<ndnsf_distributed_repo::RepoSourceProvider> m_preparedOwner;
  mutable ndnsf_distributed_repo::ArtifactRepositoryApi m_api;
};

int
run(int argc, char** argv)
{
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "Usage: DI_NativeRequester --config FILE [--input FILE --output FILE]\n"
                 "Config schema: ndnsf-di-native-requester-v1\n"
                 "A config with a turns array runs every turn in one native Conversation.\n"
                 "The request is routed through Runtime -> User::prepare -> User::request.\n";
    return 0;
  }
  const bool legacyArgs = argc == 7 && std::string(argv[1]) == "--config" &&
    std::string(argv[3]) == "--input" && std::string(argv[5]) == "--output";
  const bool turnsArgs = argc == 3 && std::string(argv[1]) == "--config";
  if (!legacyArgs && !turnsArgs) {
    std::cerr << "Usage: DI_NativeRequester --config FILE [--input FILE --output FILE]\n";
    return 2;
  }

  const auto configPath = std::filesystem::absolute(argv[2]).lexically_normal();
  const auto base = configPath.parent_path();
  const auto config = readJson(configPath);
  if (config.get<std::string>("schema", {}) != "ndnsf-di-native-requester-v1")
    throw std::invalid_argument("unsupported requester configuration schema");
  const auto& request = config.get_child("request");
  const auto turns = readTurnSpecs(
    config, base,
    legacyArgs ? std::optional<std::filesystem::path>(
      std::filesystem::absolute(argv[4]).lexically_normal()) : std::nullopt,
    legacyArgs ? std::optional<std::filesystem::path>(
      std::filesystem::absolute(argv[6]).lexically_normal()) : std::nullopt);
  if (turns.size() > 1 && !config.get_child_optional("conversation"))
    throw std::invalid_argument("NATIVE_TURNS_REQUIRES_CONVERSATION");

  RequestOptions parsedOptions;
  parsedOptions.timeout = std::chrono::milliseconds(
    request.get<std::uint64_t>("timeout_ms", parsedOptions.timeout.count()));
  parsedOptions.ackTimeout = std::chrono::milliseconds(
    request.get<std::uint64_t>("ack_timeout_ms", parsedOptions.ackTimeout.count()));
  parsedOptions.applicationRequestId = request.get<std::string>("application_request_id", {});
  parsedOptions.providerNames = providerNames(request);
  if (parsedOptions.timeout.count() <= 0 || parsedOptions.ackTimeout.count() <= 0 ||
      parsedOptions.ackTimeout >= parsedOptions.timeout)
    throw std::invalid_argument(
      "request ACK window must be positive and smaller than the request deadline");
  for (const auto& turn : turns)
    (void)requestOptionsForTurn(parsedOptions, turn);

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
    runtimeConfig.repositorySourceProvider = std::make_shared<NativeRepoSourceOwner>(
      path / "native-artifact-api", base,
      ndn::Name(config.get<std::string>("core.requester_identity")), std::move(sourceOwner));
  }
  if (const auto compatibility = config.get_child_optional("cache_compatibility")) {
    if (compatibility->get<bool>("enabled", false)) {
      if (config.get_child_optional("encrypted_repository"))
        throw std::invalid_argument(
          "cache compatibility must not configure an encrypted repository");
      const auto sourceNamespace = compatibility->get<std::string>("source_namespace", {});
      runtimeConfig.repositoryArtifactPublisher =
        std::make_shared<CacheCompatibilityArtifactPublisher>(sourceNamespace);
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
    const auto repoName = capability.repoNode;
    const auto journalPath = (path / "native-user-storage.sqlite").string();
    auto node = std::make_shared<ndnsf_distributed_repo::RepoNode>(
      ndn::Name(repoName), capability, ndnsf_distributed_repo::makeFilesystemRepoStore(
        path.string(), 1U << 20, 1U << 20, "native-requester"),
      repoName, "/local/native-requester/repo", journalPath);
    runtimeConfig.encryptedRangeStore = node->createEncryptedLargeDataStore();
    auto journal = std::make_shared<ndnsf_distributed_repo::SqliteArtifactMetadataStore>(
      journalPath, true, 16, false);
    const auto owner = ndn::Name(config.get<std::string>("core.requester_identity"))
      .toUri(ndn::name::UriFormat::ALTERNATE);
    auto sequence = std::make_shared<std::atomic<std::uint64_t>>(0);
    runtimeConfig.durableStorageOwner = NativeDurableStorageOwner{
      owner, [journal, owner, repoName, sequence](const ndn::Name& manifest,
        const std::string& writer, const std::function<void()>& active) {
        if (active) active();
        if (writer != owner) throw std::runtime_error("DURABLE_OWNER_BINDING_MISMATCH");
        const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<
          std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        const auto id = nativePlanningDigest(manifest.toUri() + ":" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ":" +
          std::to_string(sequence->fetch_add(1)));
        const auto grant = journal->grantUserStorage(owner, repoName, id, writer,
          manifest.toUri(ndn::name::UriFormat::ALTERNATE),
          now + 60000, now);
        if (active) active();
        return ndn_service_framework::DurableStorageGrantSelector{
          repoName, owner, id, grant.ownerEpoch};
      }};
  }
  if (const auto limits = config.get_child_optional("limits")) {
    if (const auto value = limits->get_optional<std::uint64_t>("preparation_timeout_ms")) {
      if (*value == 0 || *value > 3600000)
        throw std::invalid_argument(
          "limits.preparation_timeout_ms must be between 1 ms and one hour");
      runtimeConfig.preparationJobTimeout = std::chrono::milliseconds(*value);
    }
    else if (const auto value = limits->get_optional<std::uint64_t>("bootstrap_ms")) {
      // Preserve the legacy requester configuration while allowing Runtime
      // bootstrap and the source-backed User::prepare job to have independent
      // bounds for large local model catalogs.
      runtimeConfig.preparationJobTimeout = std::chrono::milliseconds(*value);
    }
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
  std::optional<KvHandoffOptions> kvHandoff;
  if (const auto handoffConfig = config.get_child_optional("kv_handoff")) {
    kvHandoff = readKvHandoffOptions(*handoffConfig, base);
    if (!config.get_child_optional("conversation") || turns.size() < 2 ||
        kvHandoff->afterTurn >= turns.size() - 1)
      throw std::invalid_argument("kv_handoff requires a conversation and a following turn");
    if (config.get_child_optional("fixed_placement"))
      throw std::invalid_argument("kv_handoff requires live compatible-Provider placement");
  }
  auto runtime = Runtime::open(std::move(runtimeConfig));
  std::shared_ptr<const PlacementStrategy> requestPlacementStrategy;
  if (!requestPlacementStrategyId.empty()) {
    requestPlacementStrategy = runtime->placementStrategy(requestPlacementStrategyId);
    std::cout << "NATIVE_REQUEST_PLACEMENT strategy="
              << requestPlacementStrategyId << std::endl;
  }
  auto user = runtime->user();
  const auto prepared = user.prepare("default", prepareOptions);

  // Runtime-bound placement must be released before the Runtime.
  RequestOptions options = parsedOptions;
  if (requestPlacementStrategy) {
    options.placement = std::move(requestPlacementStrategy);
  }
  else if (!fixedPlacementStrategyId.empty()) {
    options.placement = runtime->placementStrategy(fixedPlacementStrategyId);
    std::cout << "NATIVE_FIXED_PLACEMENT strategy=" << fixedPlacementStrategyId << std::endl;
  }
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
  std::cout << "NATIVE_REQUEST_ROUTE=Runtime.open->User.prepare->User.request\n";

  std::vector<std::int64_t> streamExpected;
  if (const auto oracle = config.get_child_optional("stream_oracle"))
    streamExpected = integerArray(*oracle, "token_ids");
  bool refreshConversationRetentionDeadline = false;
  if (const auto prewarm = config.get_child_optional("test_destination_prewarm")) {
    if (!config.get<bool>("test_only", false))
      throw std::invalid_argument("test_destination_prewarm requires test_only=true");
    const auto conversationConfig = config.get_child_optional("conversation");
    if (!conversationConfig)
      throw std::invalid_argument("test_destination_prewarm requires conversation config");
    const auto mainTurnConfig = conversationConfig->get_child_optional("turn");
    const auto mainConversationId = mainTurnConfig
      ? mainTurnConfig->get<std::string>("conversation_id", {}) : std::string{};
    const auto prewarmConversationId = prewarm->get<std::string>("conversation_id", {});
    if (mainConversationId.empty() || prewarmConversationId.empty() ||
        mainConversationId == prewarmConversationId)
      throw std::invalid_argument(
        "test_destination_prewarm requires a distinct prewarm and main conversation_id");
    const auto prewarmRequest = prewarm->get_child_optional("request");
    if (!prewarmRequest)
      throw std::invalid_argument("test_destination_prewarm.request is required");
    RequestOptions prewarmOptions = options;
    prewarmOptions.providerNames = providerNames(*prewarmRequest);
    if (options.providerNames.size() != 1 || prewarmOptions.providerNames.size() != 1 ||
        options.providerNames.front() == prewarmOptions.providerNames.front())
      throw std::invalid_argument(
        "test_destination_prewarm requires distinct single source and destination Providers");
    if (const auto timeout = prewarmRequest->get_optional<std::uint64_t>("timeout_ms")) {
      if (*timeout == 0 || *timeout > 3600000)
        throw std::invalid_argument(
          "test_destination_prewarm.request.timeout_ms must be between 1 ms and one hour");
      prewarmOptions.timeout = std::chrono::milliseconds(*timeout);
    }
    if (const auto ackTimeout = prewarmRequest->get_optional<std::uint64_t>("ack_timeout_ms")) {
      if (*ackTimeout == 0 || *ackTimeout > 3600000)
        throw std::invalid_argument(
          "test_destination_prewarm.request.ack_timeout_ms must be between 1 ms and one hour");
      prewarmOptions.ackTimeout = std::chrono::milliseconds(*ackTimeout);
    }
    if (prewarmOptions.timeout.count() <= 0 || prewarmOptions.ackTimeout.count() <= 0 ||
        prewarmOptions.ackTimeout >= prewarmOptions.timeout)
      throw std::invalid_argument(
        "test_destination_prewarm ACK window must be positive and smaller than its deadline");
    const auto prewarmTurns = readTurnSpecs(*prewarm, base, std::nullopt, std::nullopt);
    if (prewarmTurns.size() != 1)
      throw std::invalid_argument("test_destination_prewarm requires exactly one turn");
    ConversationOptions prewarmConversationOptions;
    prewarmConversationOptions.conversationId = prewarmConversationId;
    prewarmConversationOptions.retentionDeadlineMs = oneHourRetentionDeadlineMs();
    auto prewarmConversation = user.openConversation(prepared, prewarmConversationOptions);
    refreshConversationRetentionDeadline = true;

    // Keep the Runtime/User publication face alive, but isolate warm-up
    // conversation state and exclude the main event gates from this request.
    ptree prewarmRoot = config;
    prewarmRoot.erase("test_destination_prewarm");
    prewarmRoot.erase("test_stream_barrier");
    prewarmRoot.erase("test_handoff_barrier");
    prewarmRoot.erase("kv_handoff");
    auto prewarmConversationConfig = *conversationConfig;
    prewarmConversationConfig.put("turn.conversation_id", prewarmConversationId);
    prewarmRoot.put_child("conversation", prewarmConversationConfig);
    prewarmRoot.put_child("turns", prewarm->get_child("turns"));
    const auto prewarmRequestId = executeConversationTurn(
      prewarmRoot, request, base, prewarmTurns.front(), 0, prewarmOptions,
      streaming, streamExpected, user, prepared, &prewarmConversation);
    prewarmConversation.close();
    std::cout << "NATIVE_DESTINATION_PREWARM_SUCCEEDED warmupConversation="
              << prewarmConversationId << " mainConversation=" << mainConversationId
              << " request=" << prewarmRequestId << std::endl;
    const auto barrier = prewarm->get_child_optional("barrier");
    if (!barrier)
      throw std::invalid_argument("test_destination_prewarm.barrier is required");
    waitForTestExecutionBarrier(
      config, *barrier, base, "test_destination_prewarm");
  }

  std::optional<Conversation> conversation;
  if (const auto conversationConfig = config.get_child_optional("conversation")) {
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
    if (refreshConversationRetentionDeadline)
      conversationOptions.retentionDeadlineMs = oneHourRetentionDeadlineMs();
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
    conversation.emplace(user.openConversation(prepared, conversationOptions));
  }

  std::optional<ConversationHandoffHandle> providerSwitch;
  for (std::size_t turnIndex = 0; turnIndex < turns.size(); ++turnIndex) {
    const auto& turn = turns[turnIndex];
    (void)executeConversationTurn(
      config, request, base, turn, turnIndex, options, streaming,
      streamExpected, user, prepared, conversation ? &*conversation : nullptr);
    if (providerSwitch) {
      providerSwitch->result(std::chrono::seconds(5));
      std::cout << "NATIVE_KV_HANDOFF_APPEND_SUCCEEDED operation="
                << providerSwitch->operationId() << std::endl;
      providerSwitch.reset();
    }
    if (kvHandoff && turnIndex == kvHandoff->afterTurn) {
      const auto deadline = std::chrono::steady_clock::now() + kvHandoff->timeout;
      if (kvHandoff->waitForDeparture) {
        NativeDepartureWatchOptionsV1 watchOptions;
        watchOptions.deadline = deadline;
        watchOptions.saveTimeout = kvHandoff->timeout;
        watchOptions.trustedSourcePolicyByRole = kvHandoff->trust;
        auto watch = conversation->watchProviderDeparture(watchOptions);
        std::cout << "NATIVE_KV_DEPARTURE_WATCH_READY index=" << turnIndex << std::endl;
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
            providerSwitch->cancel();
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
  }
  if (turns.size() > 1)
    std::cout << "NATIVE_CONVERSATION_TURNS_SUCCEEDED count=" << turns.size() << '\n';
  runtime->close();
  (void)runtime->drain(std::chrono::seconds(5));
  return 0;
}

} // namespace

#ifndef NDNSF_DI_REQUESTER_NO_MAIN
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
#endif
