#ifndef NDNSF_TESTS_UAV_TRACKING_PROVIDER_ACCESS_HPP
#define NDNSF_TESTS_UAV_TRACKING_PROVIDER_ACCESS_HPP

#include "NDNSF-UAV-APP/tracking/uav-tracking-provider.hpp"
#include "NDNSF-UAV-APP/tracking/uav-vehicle-tracking-engine.hpp"
#include "NDNSF-UAV-APP/shared/uav-tracking-worker-process.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

namespace ndnsf::examples::uav {

/** Test-only ownership probe; supplies an already READY real child, never a fake close. */
struct UavTrackingProviderTestAccess
{
  static void verifyAssets(UavTrackingProvider& owner, const std::string& runId,
                           const std::function<bool()>& stillAuthorized);
  static void initializeHandlers(UavTrackingProvider& owner, const std::string& runId,
                                 const std::function<bool()>& stillAuthorized);

  static ndn_service_framework::ServiceProvider::AckDecision
  acknowledge(UavTrackingProvider& owner, const ndn_service_framework::RequestMessage& request);

  static void adoptIdleSession(UavTrackingProvider& owner,
                               std::shared_ptr<UavTrackingWorkerSession> session);

  // Substitute only local model execution; admission and commit remain on the native path.
  using VehicleBackend = std::function<VehicleBackendWindow(
    const VehicleWindowContext&, const std::array<ndn::Buffer, 3>&,
    const std::string&, const std::function<bool()>&)>;

  static void setVehicleBackend(UavTrackingProvider& owner, const std::string& runId,
                                VehicleBackend backend);
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_TESTS_UAV_TRACKING_PROVIDER_ACCESS_HPP
