#ifndef NDNSF_UAV_TRACKING_PROVIDER_HPP
#define NDNSF_UAV_TRACKING_PROVIDER_HPP

#include "uav-tracking-config.hpp"

#include <memory>

namespace ndn_service_framework { class ServiceProvider; }

namespace ndnsf::examples::uav {

/** The registered Provider outlives admission closure and callback/worker drain. */
class UavTrackingProvider
{
public:
  explicit UavTrackingProvider(const UavTrackingProviderConfig& config);
  ~UavTrackingProvider();

  UavTrackingProvider(const UavTrackingProvider&) = delete;
  UavTrackingProvider& operator=(const UavTrackingProvider&) = delete;

  void registerServices(ndn_service_framework::ServiceProvider& provider);
  void stopAdmission();
  void cancelActiveWork();
  bool isDrained() const;

private:
  friend struct UavTrackingProviderTestAccess;
  friend int runTrackingNodeCli(int argc, char** argv);
  struct Impl;
  std::shared_ptr<Impl> m_impl;
};

/** Describe the actual calling executable and maintained person backend without task startup. */
std::string
describeUavPersonComputation(const std::filesystem::path& model,
                             const std::filesystem::path& profile,
                             const std::filesystem::path& reidModel,
                             const std::filesystem::path& workerPython,
                             const std::filesystem::path& workerScript);

int
runTrackingNodeCli(int argc, char** argv);

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_TRACKING_PROVIDER_HPP
