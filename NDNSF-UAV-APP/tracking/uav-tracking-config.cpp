#include "uav-tracking-config.hpp"

#include "../shared/uav-person-json.hpp"
#include "../shared/uav-person-computation.hpp"
#include "../shared/UavSourceGeneration.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string_view>

namespace ndnsf::examples::uav {
namespace {

using person_json::Json;
constexpr uint64_t MAX_CONFIG_BYTES = 256 * 1024;
constexpr uint64_t MAX_ASSET_BYTES = 16 * 1024 * 1024;

void
require(bool condition, const std::string& reason)
{
  if (!condition) throw std::invalid_argument("tracking-config: " + reason);
}

void
checkComponent(const std::string& value)
{
  require(!value.empty() && value.size() <= 128 &&
    std::all_of(value.begin(), value.end(), [] (unsigned char ch) {
      return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
             (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
    }) && value != "." && value != "..", "invalid identifier");
}

std::string
readBounded(const std::filesystem::path& path, uint64_t limit)
{
  require(path.is_absolute() && std::filesystem::is_regular_file(path) &&
          !std::filesystem::is_symlink(path), "expected an absolute regular file");
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  const auto end = input.tellg();
  require(input.good() && end > 0 && static_cast<uint64_t>(end) <= limit, "file byte limit");
  std::string bytes(static_cast<size_t>(end), '\0');
  input.seekg(0);
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  require(input.good() && static_cast<size_t>(input.gcount()) == bytes.size(), "short read");
  return bytes;
}

ndn::Name
readIdentity(const Json& value)
{
  const auto text = person_json::text(value);
  require(!text.empty() && text.front() == '/', "identity must be absolute");
  ndn::Name name(text);
  require(!name.empty() && name.toUri() == text, "noncanonical identity");
  return name;
}

TrackingAssetReference
readAssetReference(const Json& value)
{
  person_json::exactKeys(value, {"path", "digest"});
  return {person_json::text(value.at("path")), person_json::text(value.at("digest"))};
}

std::string
readVerifiedAsset(const TrackingAssetReference& reference)
{
  require(person_json::isDigest(reference.digest), "invalid asset digest");
  const auto bytes = readBounded(reference.path, MAX_ASSET_BYTES);
  ndn::util::Sha256 hash;
  hash << std::string_view(bytes);
  auto digest = hash.toString();
  std::transform(digest.begin(), digest.end(), digest.begin(), [] (unsigned char ch) {
    return ch >= 'A' && ch <= 'F' ? static_cast<char>(ch + ('a' - 'A')) : ch;
  });
  require("sha256:" + digest == reference.digest, "asset digest mismatch");
  return bytes;
}

} // namespace

void
validateTrackingJobDefinition(const TrackingJobDefinition& definition)
{
  checkComponent(definition.jobId);
  checkComponent(definition.runId);
  checkComponent(definition.missionId);
  checkComponent(definition.computeDroneId);
  require(definition.mode == TrackingMode::VEHICLE || definition.mode == TrackingMode::PERSON,
          "unknown mode");
  require(!definition.computeIdentity.empty(), "missing compute identity");
  require(definition.windowCount > 0 && definition.windowCount <= 600, "window count");
  require(definition.requestTimeoutMs > 1000 &&
          definition.requestTimeoutMs <= definition.deadlineMs &&
          definition.deadlineMs <= 1200000, "deadline bounds");
  require(definition.outputDirectory.is_absolute(), "output directory must be absolute");
  std::set<ndn::Name> producers{definition.computeIdentity};
  std::set<std::string> drones{definition.computeDroneId};
  std::set<std::string> sourceIds;
  for (size_t i = 0; i < definition.sources.size(); ++i) {
    const auto& source = definition.sources[i];
    require(source.cameraId == "UAV" + std::to_string(i + 1), "camera order");
    checkComponent(source.droneId);
    person_json::checkIdentifier(source.sourceId);
    require(source.sourceId.size() <= 128 &&
            sourceIds.insert(source.sourceId).second, "source identity");
    require(source.sourceEpoch > 0 &&
            source.sourceEpoch == definition.sources.front().sourceEpoch, "source epoch");
    require(!source.producerIdentity.empty() && producers.insert(source.producerIdentity).second &&
            drones.insert(source.droneId).second, "source/compute co-location or duplicate source");
    require(source.sourceManifest.path.is_absolute() &&
            person_json::isDigest(source.sourceManifest.digest), "source manifest reference");
  }
  require(definition.calibrationIdentity.empty() ||
          producers.count(definition.calibrationIdentity) == 0, "calibration role collision");
  if (definition.mode == TrackingMode::PERSON) {
    require(definition.calibrationIdentity.empty(), "person geometry must remain UNKNOWN");
    require(definition.profile.path.is_absolute() && person_json::isDigest(definition.profile.digest) &&
            definition.computationManifest.path.is_absolute() &&
            person_json::isDigest(definition.computationManifest.digest), "person computation binding");
  }
}

TrackingJobInputs
validateTrackingJobInputs(const TrackingJobDefinition& result)
{
  validateTrackingJobDefinition(result);
  TrackingJobInputs inputs;
  for (size_t i = 0; i < result.sources.size(); ++i) {
    const auto schedule = parseUavSourceGenerationSchedule(readVerifiedAsset(result.sources[i].sourceManifest), result.windowCount);
    require(schedule.generations.size() == 1, "source generation must remain fixed within a job");
    if (i == 0) inputs.sourceGenerations = schedule;
    const auto& generation = schedule.generations.front();
    for (size_t camera = 0; camera < result.sources.size(); ++camera) {
      require(generation.sourceIds[camera] == result.sources[camera].sourceId &&
              generation.sourceEpoch == result.sources[camera].sourceEpoch,
              "source manifest identity does not match job");
    }
  }
  if (result.mode == TrackingMode::PERSON) {
    const auto profile = parseUavPersonTrackingProfile(readVerifiedAsset(result.profile));
    const auto computation = parseUavPersonComputationManifest(
      readVerifiedAsset(result.computationManifest), profile);
    inputs.personProfile = profile;
    inputs.personComputation = computation;
    requireUavPersonWindowCapacity(profile, result.windowCount);
    require(result.deadlineMs <= profile.limits.at("runDeadlineMs") &&
            result.requestTimeoutMs <= profile.limits.at("runDeadlineMs"),
            "job exceeds frozen person budget");
  }
  else {
    if (!result.profile.path.empty()) readVerifiedAsset(result.profile);
    if (!result.computationManifest.path.empty()) readVerifiedAsset(result.computationManifest);
  }
  return inputs;
}

TrackingJobDefinition
loadTrackingJobDefinition(const std::filesystem::path& path)
{
  const auto config = person_json::parseBounded(readBounded(path, MAX_CONFIG_BYTES), MAX_CONFIG_BYTES);
  person_json::exactKeys(config, {"schema", "jobId", "runId", "missionId", "mode", "sources",
    "compute", "calibrationIdentity", "profile", "computationManifest", "outputDirectory",
    "windowCount", "deadlineMs", "requestTimeoutMs"});
  require(config.at("schema") == "uav-tracking-job-v1", "schema");
  TrackingJobDefinition result;
  result.jobId = person_json::text(config.at("jobId"));
  result.runId = person_json::text(config.at("runId"));
  result.missionId = person_json::text(config.at("missionId"));
  const auto mode = person_json::text(config.at("mode"));
  require(mode == "person" || mode == "vehicle", "mode");
  result.mode = mode == "person" ? TrackingMode::PERSON : TrackingMode::VEHICLE;
  const auto& sources = config.at("sources");
  require(sources.is_array() && sources.size() == result.sources.size(), "three sources required");
  for (size_t i = 0; i < result.sources.size(); ++i) {
    const auto& item = sources.at(i);
    person_json::exactKeys(item, {"cameraId", "sourceId", "sourceEpoch", "producerIdentity",
                                 "droneId", "sourceManifest"});
    result.sources[i] = {person_json::text(item.at("cameraId")),
      person_json::text(item.at("sourceId")), person_json::unsignedNumber(item.at("sourceEpoch")),
      readIdentity(item.at("producerIdentity")), person_json::text(item.at("droneId")),
      readAssetReference(item.at("sourceManifest"))};
  }
  person_json::exactKeys(config.at("compute"), {"producerIdentity", "droneId"});
  result.computeIdentity = readIdentity(config.at("compute").at("producerIdentity"));
  result.computeDroneId = person_json::text(config.at("compute").at("droneId"));
  if (!config.at("calibrationIdentity").is_null()) {
    result.calibrationIdentity = readIdentity(config.at("calibrationIdentity"));
  }
  if (!config.at("profile").is_null()) {
    result.profile = readAssetReference(config.at("profile"));
  }
  if (!config.at("computationManifest").is_null()) {
    result.computationManifest = readAssetReference(config.at("computationManifest"));
  }
  result.outputDirectory = person_json::text(config.at("outputDirectory"));
  result.windowCount = person_json::unsignedNumber(config.at("windowCount"), 600);
  result.deadlineMs = person_json::unsignedNumber(config.at("deadlineMs"), 1200000);
  result.requestTimeoutMs = person_json::unsignedNumber(config.at("requestTimeoutMs"), 1200000);
  validateTrackingJobInputs(result);
  return result;
}

void
validateTrackingProviderConfig(const UavTrackingProviderConfig& config)
{
  require(!config.producerIdentity.empty(), "provider identity is required");
  checkComponent(config.droneId);
  require(config.ownedWorkspace.is_absolute() &&
          config.ownedWorkspace.lexically_normal() == config.ownedWorkspace &&
          config.ownedWorkspace != config.ownedWorkspace.root_path(), "invalid provider workspace");
  require(config.runs.size() <= 16, "provider run catalog limit");
  std::set<std::string> runs;
  const auto reference = [] (const TrackingAssetReference& asset, bool required) {
    if (!required && asset.path.empty() && asset.digest.empty()) return;
    require(asset.path.is_absolute() && asset.path.lexically_normal() == asset.path &&
            person_json::isDigest(asset.digest), "invalid provider asset reference");
  };
  for (const auto& run : config.runs) {
    checkComponent(run.runId);
    require(runs.insert(run.runId).second, "duplicate provider run");
    require(run.mode == TrackingMode::PERSON || run.mode == TrackingMode::VEHICLE,
            "invalid provider mode");
    require(run.role == TrackingProviderRole::SOURCE || run.role == TrackingProviderRole::COMPUTE,
            "invalid provider role");
    require(run.windowCount > 0 && run.windowCount <= 600, "provider window bound");
    require(!run.computeProducer.empty(), "compute Producer is required");
    std::set<ndn::Name> producers{run.computeProducer};
    for (const auto& producer : run.sourceProducers) {
      require(!producer.empty() && producers.insert(producer).second,
              "provider run needs four distinct Producers");
    }
    reference(run.sourceGenerations, true);
    const bool compute = run.role == TrackingProviderRole::COMPUTE;
    if (compute) {
      require(run.cameraId.empty() && config.producerIdentity == run.computeProducer,
              "compute role identity mismatch");
      require(run.observationStreamDirectory.empty(), "compute cannot own source streams");
    }
    else {
      const size_t camera = run.cameraId == "UAV1" ? 0 : run.cameraId == "UAV2" ? 1 :
                            run.cameraId == "UAV3" ? 2 : 3;
      require(camera < 3, "invalid provider camera");
      require(config.producerIdentity == run.sourceProducers[camera],
              "source role identity mismatch");
      require(run.observationStreamDirectory.is_absolute() &&
              run.observationStreamDirectory.lexically_normal() == run.observationStreamDirectory,
              "invalid observation stream directory");
    }
    if (!compute || run.mode == TrackingMode::VEHICLE) {
      checkComponent(run.worldFrameId);
      require(run.acquisitionKind == "simulated" || run.acquisitionKind == "measured",
              "invalid tracking observation acquisition kind");
      require(run.deliveryMode == "replay", "main tracking catalog requires prepared replay");
    }
    require(run.requestTimeoutMs > 1000 && run.requestTimeoutMs <= 1200000,
            "invalid prepared tracking request budget");
    reference(run.model, compute);
    reference(run.workerPython, compute);
    reference(run.workerScript, compute);
    const bool person = compute && run.mode == TrackingMode::PERSON;
    reference(run.profile, person);
    reference(run.reidModel, person);
    reference(run.computationManifest, person);
    if (!compute) {
      require(run.model.path.empty() && run.workerPython.path.empty() && run.workerScript.path.empty() &&
              run.profile.path.empty() && run.reidModel.path.empty() &&
              run.computationManifest.path.empty(), "source cannot load compute assets");
    }
    if (!person) {
      require(run.profile.path.empty() && run.reidModel.path.empty() &&
              run.computationManifest.path.empty(), "person assets require person compute mode");
    }
  }
}

UavTrackingProviderConfig
loadTrackingProviderConfig(const std::filesystem::path& path)
{
  const auto json = person_json::parseBounded(readBounded(path, MAX_CONFIG_BYTES), MAX_CONFIG_BYTES);
  person_json::exactKeys(json,
    {"schema", "producerIdentity", "droneId", "ownedWorkspace", "runs"});
  require(json.at("schema") == "uav-tracking-provider-v1", "provider config schema");
  UavTrackingProviderConfig result;
  result.producerIdentity = readIdentity(json.at("producerIdentity"));
  result.droneId = person_json::text(json.at("droneId"));
  result.ownedWorkspace = person_json::text(json.at("ownedWorkspace"));
  require(json.at("runs").is_array() && json.at("runs").size() <= 16, "provider run array");
  for (const auto& entry : json.at("runs")) {
    person_json::exactKeys(entry, {"runId", "mode", "role", "cameraId", "sourceProducers",
      "computeProducer", "windowCount", "requestTimeoutMs", "sourceGenerations", "observationStreamDirectory",
      "worldFrameId", "acquisitionKind", "deliveryMode", "model", "profile", "reidModel",
      "computationManifest", "workerPython", "workerScript"});
    TrackingProviderRunDefinition run;
    run.runId = person_json::text(entry.at("runId"));
    const auto mode = person_json::text(entry.at("mode"));
    require(mode == "person" || mode == "vehicle", "provider mode");
    run.mode = mode == "person" ? TrackingMode::PERSON : TrackingMode::VEHICLE;
    const auto role = person_json::text(entry.at("role"));
    require(role == "source" || role == "compute", "provider role");
    run.role = role == "source" ? TrackingProviderRole::SOURCE : TrackingProviderRole::COMPUTE;
    run.cameraId = person_json::text(entry.at("cameraId"));
    const auto& sources = entry.at("sourceProducers");
    require(sources.is_array() && sources.size() == 3, "provider source Producer array");
    for (size_t i = 0; i < 3; ++i) run.sourceProducers[i] = readIdentity(sources.at(i));
    run.computeProducer = readIdentity(entry.at("computeProducer"));
    run.windowCount = person_json::unsignedNumber(entry.at("windowCount"), 600);
    run.requestTimeoutMs = person_json::unsignedNumber(entry.at("requestTimeoutMs"), 1200000);
    run.sourceGenerations = readAssetReference(entry.at("sourceGenerations"));
    run.observationStreamDirectory = person_json::text(entry.at("observationStreamDirectory"));
    run.worldFrameId = person_json::text(entry.at("worldFrameId"));
    run.acquisitionKind = person_json::text(entry.at("acquisitionKind"));
    run.deliveryMode = person_json::text(entry.at("deliveryMode"));
    const auto optionalAsset = [&] (const char* field) {
      return entry.at(field).is_null() ? TrackingAssetReference{} : readAssetReference(entry.at(field));
    };
    run.model = optionalAsset("model");
    run.profile = optionalAsset("profile");
    run.reidModel = optionalAsset("reidModel");
    run.computationManifest = optionalAsset("computationManifest");
    run.workerPython = optionalAsset("workerPython");
    run.workerScript = optionalAsset("workerScript");
    result.runs.push_back(std::move(run));
  }
  validateTrackingProviderConfig(result);
  return result;
}

} // namespace ndnsf::examples::uav
