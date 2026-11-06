#include "tests/boost-test.hpp"
#include "tests/fixtures/uav-tracking-job-config.hpp"
#include "tests/fixtures/uav-person-profile-golden.hpp"

#include "NDNSF-UAV-APP/shared/uav-person-json.hpp"
#include "NDNSF-UAV-APP/tracking/uav-tracking-config.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <ndn-cxx/util/sha256.hpp>

namespace ndnsf::test {
namespace {

using namespace examples::uav;
using namespace tracking_fixture;

Json
writeAsset(const std::filesystem::path& path, const std::string& bytes)
{
  std::ofstream(path) << bytes;
  ndn::util::Sha256 hash;
  hash << std::string_view(bytes);
  auto digest = hash.toString();
  for (auto& ch : digest) if (ch >= 'A' && ch <= 'F') ch += 'a' - 'A';
  return {{"path", path.string()}, {"digest", "sha256:" + digest}};
}

void
usePersonProfile(ConfigFixture& fixture, const std::string& profileBytes)
{
  const auto profile = parseUavPersonTrackingProfile(profileBytes);
  std::map<std::string, std::string> backend;
  for (const auto* key : {"tracking_worker.py", "person_reid.py", "person_profile.py",
                        "observation_schema.py", "motion_schema.py", "motion_estimator.py",
                        "byte_track_state.py", "person_runtime.py", "requirements-person.lock",
                        "bytetrack.yaml", "runtime/fingerprint", "runtime/torchreid-source"}) {
    backend.emplace(key, ABC_DIGEST);
  }
  const auto computation = makeUavPersonComputationManifest(profile, backend,
    {{"executable:/opt/ndnsf/bin/UavDroneApp", ABC_DIGEST}});
  fixture.config["mode"] = "person";
  fixture.config["profile"] = writeAsset(fixture.root / "profile", profileBytes);
  fixture.config["computationManifest"] = writeAsset(
    fixture.root / "computation", computation.canonicalJson);
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavTrackingConfig)

BOOST_AUTO_TEST_CASE(ExplicitProducerTopologyAndCpuBudget)
{
  auto definition = validDefinition();
  BOOST_CHECK_NO_THROW(validateTrackingJobDefinition(definition));
  definition.sources[1].producerIdentity = definition.sources[0].producerIdentity;
  BOOST_CHECK_THROW(validateTrackingJobDefinition(definition), std::invalid_argument);
  definition = validDefinition();
  definition.computeDroneId = definition.sources[0].droneId;
  BOOST_CHECK_THROW(validateTrackingJobDefinition(definition), std::invalid_argument);
  definition = validDefinition();
  definition.sources[2].sourceEpoch = 2;
  BOOST_CHECK_THROW(validateTrackingJobDefinition(definition), std::invalid_argument);
  definition = validDefinition();
  definition.calibrationIdentity = definition.computeIdentity;
  BOOST_CHECK_THROW(validateTrackingJobDefinition(definition), std::invalid_argument);
  definition.calibrationIdentity = definition.sources[0].producerIdentity;
  BOOST_CHECK_THROW(validateTrackingJobDefinition(definition), std::invalid_argument);
  definition = validDefinition();
  definition.sources[0].sourceId = "bad\nidentity";
  BOOST_CHECK_THROW(validateTrackingJobDefinition(definition), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PersonCannotBorrowVehicleGeometryOrMissingComputation)
{
  auto definition = validDefinition();
  definition.mode = TrackingMode::PERSON;
  BOOST_CHECK_THROW(validateTrackingJobDefinition(definition), std::invalid_argument);
  definition.profile = {"/var/tmp/profile", ABC_DIGEST};
  definition.computationManifest = {"/var/tmp/computation", ABC_DIGEST};
  BOOST_CHECK_NO_THROW(validateTrackingJobDefinition(definition));
  definition.calibrationIdentity = ndn::Name("/lab/calibration");
  BOOST_CHECK_THROW(validateTrackingJobDefinition(definition), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RejectTraversalAndOutOfRangeBudgets)
{
  for (const auto* id : {"..", "a/b", "", "a;b", "a b"}) {
    auto definition = validDefinition();
    definition.runId = id;
    BOOST_CHECK_THROW(validateTrackingJobDefinition(definition), std::invalid_argument);
  }
  for (const auto count : {0, 601}) {
    auto definition = validDefinition();
    definition.windowCount = count;
    BOOST_CHECK_THROW(validateTrackingJobDefinition(definition), std::invalid_argument);
  }
  auto definition = validDefinition();
  definition.deadlineMs = 1200001;
  BOOST_CHECK_THROW(validateTrackingJobDefinition(definition), std::invalid_argument);
  definition = validDefinition();
  definition.requestTimeoutMs = definition.deadlineMs + 1;
  BOOST_CHECK_THROW(validateTrackingJobDefinition(definition), std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(LoadBindsActualAssetBytesAndRejectsTamper, ConfigFixture)
{
  const auto definition = load();
  BOOST_CHECK_EQUAL(definition.sources[0].producerIdentity.toUri(), "/lab/uav/source-1");
  BOOST_CHECK_EQUAL(definition.sources[0].sourceManifest.digest, SOURCE_DIGEST);
  BOOST_CHECK_EQUAL(definition.windowCount, 100);
  std::ofstream(root / "asset") << "changed";
  BOOST_CHECK_THROW(load(), std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(RejectDigestCorrectButUnboundSourceManifest, ConfigFixture)
{
  config["sources"][0]["sourceId"] = "different-source";
  BOOST_CHECK_THROW(load(), std::invalid_argument);
  config["sources"][0]["sourceId"] = "camera-source-1";
  for (auto& source : config["sources"]) source["sourceEpoch"] = 2;
  BOOST_CHECK_THROW(load(), std::invalid_argument);
  for (auto& source : config["sources"]) source["sourceEpoch"] = 1;
  std::ofstream(root / "asset") << "abc";
  for (auto& source : config["sources"]) source["sourceManifest"]["digest"] = ABC_DIGEST;
  BOOST_CHECK_THROW(load(), std::exception);
}

BOOST_FIXTURE_TEST_CASE(PersonProfileCapacityAndComputationAreAdmittedTogether, ConfigFixture)
{
  usePersonProfile(*this, std::string(PERSON_PROFILE_GOLDEN));
  BOOST_CHECK_NO_THROW(load());
  config["deadlineMs"] = 900001;
  BOOST_CHECK_THROW(load(), std::invalid_argument);
  config["deadlineMs"] = 60000;
  auto shortProfile = Json::parse(PERSON_PROFILE_GOLDEN);
  shortProfile["limits"]["maxRetainedTransactionWindows"] = 99;
  usePersonProfile(*this, shortProfile.dump());
  BOOST_CHECK_THROW(load(), std::invalid_argument);
  usePersonProfile(*this, std::string(PERSON_PROFILE_GOLDEN));
  auto otherProfile = Json::parse(PERSON_PROFILE_GOLDEN);
  otherProfile["limits"]["maxCosineDistancePpm"] = 400000;
  config["profile"] = writeAsset(root / "profile", otherProfile.dump());
  BOOST_CHECK_THROW(load(), std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(RejectUnknownDuplicateAndWrongTypedFields, ConfigFixture)
{
  config["fault"] = "bad-digest";
  BOOST_CHECK_THROW(load(), std::invalid_argument);
  config.erase("fault");
  config["requestTimeoutMs"] = true;
  BOOST_CHECK_THROW(load(), std::invalid_argument);
  config["requestTimeoutMs"] = 30000;
  std::ofstream(root / "job.json") << "{\"schema\":1,\"schema\":2}";
  BOOST_CHECK_THROW(loadTrackingJobDefinition(root / "job.json"), std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(ProviderCatalogRejectsUnknownFieldsAndConflictingRoles, ConfigFixture)
{
  Json producers = Json::array();
  for (const auto& source : config["sources"]) producers.push_back(source["producerIdentity"]);
  const Json run = {{"runId", "run-1"}, {"mode", "vehicle"}, {"role", "source"},
    {"cameraId", "UAV1"}, {"sourceProducers", producers},
    {"computeProducer", config["compute"]["producerIdentity"]}, {"windowCount", 100},
    {"requestTimeoutMs", 30000},
    {"sourceGenerations", config["sources"][0]["sourceManifest"]},
    {"observationStreamDirectory", root.string()}, {"worldFrameId", "synthetic-local"},
    {"acquisitionKind", "simulated"}, {"deliveryMode", "replay"},
    {"model", nullptr}, {"profile", nullptr}, {"reidModel", nullptr},
    {"computationManifest", nullptr}, {"workerPython", nullptr}, {"workerScript", nullptr}};
  Json provider = {{"schema", "uav-tracking-provider-v1"},
    {"producerIdentity", config["sources"][0]["producerIdentity"]}, {"droneId", "source-1"},
    {"ownedWorkspace", (root / "provider-work").string()}, {"runs", Json::array({run})}};
  const auto read = [&] {
    std::ofstream(root / "provider.json") << provider.dump();
    return loadTrackingProviderConfig(root / "provider.json");
  };
  BOOST_CHECK_EQUAL(read().runs.size(), 1);
  const auto original = provider;
  provider["runs"][0].erase("requestTimeoutMs");
  BOOST_CHECK_THROW(read(), std::invalid_argument);
  provider = original;
  for (const auto& budget : {Json(true), Json(1000), Json(1200001)}) {
    provider["runs"][0]["requestTimeoutMs"] = budget;
    BOOST_CHECK_THROW(read(), std::invalid_argument);
  }
  provider = original;
  provider["runs"][0]["fault"] = "worker-failure";
  BOOST_CHECK_THROW(read(), std::invalid_argument);
  provider = original;
  provider["runs"][0]["sourceProducers"][0] = provider["runs"][0]["computeProducer"];
  BOOST_CHECK_THROW(read(), std::invalid_argument);
  provider = original;
  provider["runs"].push_back(run);
  BOOST_CHECK_THROW(read(), std::invalid_argument);
  provider = original;
  provider["runs"] = Json::array();
  BOOST_CHECK(read().runs.empty());
}

BOOST_AUTO_TEST_CASE(VehicleComputeRequiresTheSameObservationContextAsItsSources)
{
  const auto job = validDefinition();
  UavTrackingProviderConfig config;
  config.producerIdentity = job.computeIdentity;
  config.droneId = job.computeDroneId;
  config.ownedWorkspace = "/var/tmp/spec228-controlled-compute";
  TrackingProviderRunDefinition run;
  run.runId = job.runId;
  run.mode = TrackingMode::VEHICLE;
  run.role = TrackingProviderRole::COMPUTE;
  for (size_t i = 0; i < 3; ++i) run.sourceProducers[i] = job.sources[i].producerIdentity;
  run.computeProducer = job.computeIdentity;
  run.windowCount = job.windowCount;
  run.sourceGenerations = job.sources[0].sourceManifest;
  run.model = {"/var/tmp/model.pt", ABC_DIGEST};
  run.workerPython = {"/var/tmp/python3", ABC_DIGEST};
  run.workerScript = {"/var/tmp/tracking_worker.py", ABC_DIGEST};
  run.worldFrameId = "synthetic-local";
  run.acquisitionKind = "simulated";
  run.deliveryMode = "replay";
  config.runs.push_back(run);
  BOOST_CHECK_NO_THROW(validateTrackingProviderConfig(config));
  for (auto member : {&TrackingProviderRunDefinition::worldFrameId,
                      &TrackingProviderRunDefinition::acquisitionKind,
                      &TrackingProviderRunDefinition::deliveryMode}) {
    config.runs[0] = run;
    (config.runs[0].*member).clear();
    BOOST_CHECK_THROW(validateTrackingProviderConfig(config), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
