#ifndef NDNSF_UAV_TRACKING_PROVIDER_HPP
#define NDNSF_UAV_TRACKING_PROVIDER_HPP

#include "uav-tracking-config.hpp"

#include <functional>
#include <memory>
#include <string>

namespace ndn_service_framework { class ServiceProvider; }

namespace ndnsf::examples::uav {

class IObservationSource;

struct UavTrackingSourceRequest;
using UavTrackingSourceFactory =
  std::function<std::shared_ptr<IObservationSource>(const UavTrackingSourceRequest&)>;

/** The registered Provider outlives admission closure and callback/worker drain. */
class UavTrackingProvider
{
public:
  explicit UavTrackingProvider(const UavTrackingProviderConfig& config);
  /** Attach a MiniMUAS-owned native observation source for a single source run. */
  UavTrackingProvider(const UavTrackingProviderConfig& config,
                      std::shared_ptr<IObservationSource> source);
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
  friend int runTrackingNodeCliImpl(int argc, char** argv,
                                    const UavTrackingSourceFactory* sourceFactory);
  struct Impl;
  std::shared_ptr<Impl> m_impl;
};

/** Describe the actual calling executable and maintained person backend without task startup. */
std::string
describeUavPersonNativeComputation(const std::filesystem::path& model,
                                   const std::filesystem::path& profile,
                                   const std::filesystem::path& reidModel);

/** Native source factory used by MiniMUAS to inject its software camera. */
struct UavTrackingSourceRequest
{
  std::string cameraId;
  std::string sourceId;
  uint64_t sourceEpoch = 0;
  uint64_t windowCount = 0;
  uint64_t mediaStartUs = 0;
  uint64_t captureEpoch = 0;
  std::string videoPath;
};

int
runTrackingNodeCli(int argc, char** argv);

int
runTrackingNodeCliWithSource(int argc, char** argv,
                             const UavTrackingSourceFactory& sourceFactory);

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_TRACKING_PROVIDER_HPP
