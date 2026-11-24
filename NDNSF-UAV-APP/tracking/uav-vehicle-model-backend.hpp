#ifndef NDNSF_UAV_VEHICLE_MODEL_BACKEND_HPP
#define NDNSF_UAV_VEHICLE_MODEL_BACKEND_HPP

#include "../shared/uav-person-onnx.hpp"
#include "uav-vehicle-tracking-engine.hpp"

namespace ndnsf::examples::uav {

/** Runs the detector and one restorable local tracker per admitted camera window. */
class UavVehicleModelBackend
{
public:
  explicit UavVehicleModelBackend(const std::filesystem::path& detectorModel);

  VehicleBackendWindow
  processWindow(const VehicleWindowContext& context,
                const std::array<ndn::Buffer, 3>& admittedJpegs,
                const std::string& committedBackendState,
                const std::function<bool()>& stillAuthorized);

private:
  UavVehicleDetectorSession m_detector;
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_VEHICLE_MODEL_BACKEND_HPP
