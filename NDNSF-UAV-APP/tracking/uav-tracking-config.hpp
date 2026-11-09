#ifndef NDNSF_UAV_TRACKING_CONFIG_HPP
#define NDNSF_UAV_TRACKING_CONFIG_HPP

#include "../shared/UavSourceGeneration.hpp"
#include "../shared/uav-person-computation.hpp"

#include <ndn-cxx/name.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

enum class TrackingMode { VEHICLE, PERSON };

struct TrackingAssetReference
{
  std::filesystem::path path;
  std::string digest;
};

struct TrackingSourceBinding
{
  std::string cameraId;
  std::string sourceId;
  uint64_t sourceEpoch = 0;
  ndn::Name producerIdentity;
  std::string droneId;
  TrackingAssetReference sourceManifest;
};

/** Immutable local input to the native job; it confers no remote permission. */
struct TrackingJobDefinition
{
  std::string jobId;
  std::string runId;
  std::string missionId;
  TrackingMode mode = TrackingMode::VEHICLE;
  std::array<TrackingSourceBinding, 3> sources;
  ndn::Name computeIdentity;
  std::string computeDroneId;
  ndn::Name calibrationIdentity;
  TrackingAssetReference profile;
  TrackingAssetReference computationManifest;
  std::filesystem::path outputDirectory;
  uint64_t windowCount = 0;
  uint64_t deadlineMs = 0;
  uint64_t requestTimeoutMs = 0;
};

/** Validate identities, topology and resource bounds without opening assets. */
void
validateTrackingJobDefinition(const TrackingJobDefinition& definition);

struct TrackingJobInputs
{
  UavSourceGenerationSchedule sourceGenerations;
  std::optional<UavPersonTrackingProfile> personProfile;
  std::optional<UavPersonComputationManifest> personComputation;
};

/** Verify references and parse the exact verified bytes for admission without a later reopen. */
TrackingJobInputs
validateTrackingJobInputs(const TrackingJobDefinition& definition);

/** Read a bounded, duplicate-key-free config and verify its immutable asset references. */
TrackingJobDefinition
loadTrackingJobDefinition(const std::filesystem::path& path);

enum class TrackingProviderRole { SOURCE, COMPUTE };

/** One prepared replay; it cannot be rebound while the capability is running. */
struct TrackingProviderRunDefinition
{
  std::string runId;
  TrackingMode mode = TrackingMode::VEHICLE;
  TrackingProviderRole role = TrackingProviderRole::SOURCE;
  std::string cameraId;
  std::array<ndn::Name, 3> sourceProducers;
  ndn::Name computeProducer;
  uint64_t windowCount = 0;
  uint64_t requestTimeoutMs = 30000;
  TrackingAssetReference sourceGenerations;
  std::filesystem::path observationStreamDirectory;
  std::string worldFrameId;
  std::string acquisitionKind;
  std::string deliveryMode;
  TrackingAssetReference model;
  TrackingAssetReference profile;
  TrackingAssetReference reidModel;
  TrackingAssetReference computationManifest;
  TrackingAssetReference workerPython;
  TrackingAssetReference workerScript;
};

struct UavTrackingProviderConfig
{
  ndn::Name producerIdentity;
  std::string droneId;
  std::filesystem::path ownedWorkspace;
  std::vector<TrackingProviderRunDefinition> runs;
};

/** Validate structure only; individual asset readiness belongs to the capability. */
void
validateTrackingProviderConfig(const UavTrackingProviderConfig& config);

UavTrackingProviderConfig
loadTrackingProviderConfig(const std::filesystem::path& path);

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_TRACKING_CONFIG_HPP
