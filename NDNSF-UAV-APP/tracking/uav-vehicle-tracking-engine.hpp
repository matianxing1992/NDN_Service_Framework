#ifndef NDNSF_UAV_VEHICLE_TRACKING_ENGINE_HPP
#define NDNSF_UAV_VEHICLE_TRACKING_ENGINE_HPP

#include "../shared/uav-person-json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

/** Existing vehicle policies, frozen with the native computation identity. */
struct VehicleTrackingPolicy
{
  int64_t associationMaxGapUs = 5000000;
  double associationMaxCenterDistance = 1.0;
  size_t minimumPoints = 3;
  size_t maximumPoints = 32;
  int64_t maximumGapUs = 1000000;
  int64_t historyWindowUs = 1000000;
  int64_t minimumTimeSpanUs = 400000;
  double maximumSpeedMps = 60.0;
  double stationaryThresholdMps = 0.5;
  double maximumBearingSigmaDegrees = 30.0;

  std::string canonicalJson() const;
};

/** Original input and reference identity admitted by the native Provider. */
struct VehicleFrameContext
{
  std::string cameraId;
  std::string sourceId;
  uint64_t sourceEpoch = 0;
  uint64_t sequence = 0;
  int64_t commonPtsUs = 0;
  int width = 0;
  int height = 0;
  std::string inputDigest;
  std::string imageName;
  std::string observationName;
  std::string observationDigest;
  // Geometry is assembled from verified original observation/dependency Data.
  person_json::Json observation = nullptr;
};

struct VehicleWindowContext
{
  std::string runId;
  std::string missionId;
  std::string windowId;
  std::string nativeDecisionDigest;
  uint64_t sequence = 0;
  bool observationMode = true;
  std::array<VehicleFrameContext, 3> frames;
  // The explicit compatibility path alone supplies this original motion input.
  person_json::Json legacyMotion = nullptr;
};

/** Local model/tracker evidence; application IDs and motion never come from Python. */
struct VehicleDetection
{
  std::array<int, 4> box{};
  double confidence = 0;
  std::string label;
  std::optional<uint64_t> byteTrackId;
};

struct VehicleBackendFrame
{
  std::string cameraId;
  int width = 0;
  int height = 0;
  std::vector<VehicleDetection> detections;
};

/** An immutable admitted candidate; only the binding parser can construct one. */
class VehicleBackendWindow
{
public:
  const std::array<VehicleBackendFrame, 3>& frames() const { return m_frames; }

private:
  VehicleBackendWindow() = default;
  friend VehicleBackendWindow parseUavVehicleBackendWindow(
    const std::string&, const VehicleWindowContext&);
  friend class UavVehicleTrackingEngine;

  std::array<VehicleBackendFrame, 3> m_frames;
  person_json::Json m_backendState;
  std::string m_contextIdentity;
};

/** Validate the complete candidate against native input before exposing any detection. */
VehicleBackendWindow
parseUavVehicleBackendWindow(const std::string& json, const VehicleWindowContext& expected);

struct VehicleWindowOutput
{
  std::string resultJson;
  std::string candidateStateJson;
};

/**
 * Own vehicle association and source-time motion, never publication or commit.
 * One synchronous handler owns this engine. Restore before exporting backend
 * state, so a changed source cannot seed the next backend with an old tracker.
 * The caller commits the returned pair through UavTrackingWorkerTransaction.
 */
class UavVehicleTrackingEngine
{
public:
  explicit UavVehicleTrackingEngine(const VehicleTrackingPolicy& policy = {});
  ~UavVehicleTrackingEngine();

  UavVehicleTrackingEngine(const UavVehicleTrackingEngine&) = delete;
  UavVehicleTrackingEngine& operator=(const UavVehicleTrackingEngine&) = delete;

  void restoreState(const std::string& committedStateJson,
                    const VehicleWindowContext& nextContext);
  std::string backendStateJson() const;
  VehicleWindowOutput processWindow(const VehicleWindowContext& context,
                                    const VehicleBackendWindow& backend);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_VEHICLE_TRACKING_ENGINE_HPP
