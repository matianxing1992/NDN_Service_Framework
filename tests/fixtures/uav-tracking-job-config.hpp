#ifndef NDNSF_TESTS_UAV_TRACKING_JOB_CONFIG_HPP
#define NDNSF_TESTS_UAV_TRACKING_JOB_CONFIG_HPP

#include "NDNSF-UAV-APP/shared/uav-person-json.hpp"
#include "NDNSF-UAV-APP/tracking/uav-tracking-config.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace ndnsf::test::tracking_fixture {

using namespace examples::uav;
using examples::uav::person_json::Json;
inline const std::string ABC_DIGEST =
  "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
inline const std::string SOURCE_MANIFEST =
  R"({"schema":"uav-source-generations-v1","windowCount":100,"generations":[{"firstWindowIndex":0,"sourceEpoch":1,"sourceIds":{"UAV1":"camera-source-1","UAV2":"camera-source-2","UAV3":"camera-source-3"}}]})";
inline const std::string SOURCE_DIGEST =
  "sha256:e8be446584698b1dee1a3df1165befbb19213e8a3608eb0f450e7b980b8f5269";

inline TrackingJobDefinition
validDefinition()
{
  TrackingJobDefinition result;
  result.jobId = "job-1";
  result.runId = "run-1";
  result.missionId = "replay-1";
  result.computeIdentity = ndn::Name("/lab/uav/compute");
  result.computeDroneId = "compute";
  result.outputDirectory = "/var/tmp/spec228-test-output";
  result.windowCount = 120;
  result.deadlineMs = 1200000;
  result.requestTimeoutMs = 60000;
  for (size_t i = 0; i < 3; ++i) {
    const auto id = std::to_string(i + 1);
    result.sources[i] = {"UAV" + id, "camera-source-" + id, 1,
      ndn::Name("/lab/uav/source-" + id), "source-" + id, {"/var/tmp/manifest", ABC_DIGEST}};
  }
  return result;
}

struct ConfigFixture
{
  ConfigFixture()
  {
    char pattern[] = "/tmp/spec228-config-XXXXXX";
    const auto* directory = ::mkdtemp(pattern);
    if (!directory) throw std::runtime_error("cannot create config fixture directory");
    root = directory;
    std::ofstream(root / "asset") << SOURCE_MANIFEST;
    const Json asset = {{"path", (root / "asset").string()}, {"digest", SOURCE_DIGEST}};
    config = {{"schema", "uav-tracking-job-v1"}, {"jobId", "job-1"},
      {"runId", "run-1"}, {"missionId", "replay-1"}, {"mode", "vehicle"},
      {"compute", {{"producerIdentity", "/lab/uav/compute"}, {"droneId", "compute"}}},
      {"sources", Json::array()}, {"calibrationIdentity", nullptr}, {"profile", nullptr},
      {"computationManifest", nullptr}, {"outputDirectory", (root / "output").string()},
      {"windowCount", 100}, {"deadlineMs", 60000}, {"requestTimeoutMs", 30000}};
    for (size_t i = 1; i <= 3; ++i) {
      const auto id = std::to_string(i);
      config["sources"].push_back({{"cameraId", "UAV" + id},
        {"sourceId", "camera-source-" + id}, {"sourceEpoch", 1},
        {"producerIdentity", "/lab/uav/source-" + id}, {"droneId", "source-" + id},
        {"sourceManifest", asset}});
    }
  }

  ~ConfigFixture()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  TrackingJobDefinition load()
  {
    std::ofstream(root / "job.json") << config.dump();
    return loadTrackingJobDefinition(root / "job.json");
  }

  std::filesystem::path root;
  Json config;
};


} // namespace ndnsf::test::tracking_fixture

#endif // NDNSF_TESTS_UAV_TRACKING_JOB_CONFIG_HPP
